#!/usr/bin/env bash
# =============================================================================
# easyai install — macOS one-shot installer
#
# What this script does, in order:
#
#   1. Verifies / installs the build deps (xcode-select, brew, cmake, git,
#      curl) and prints what it found.
#   2. Clones llama.cpp as a sibling of this easyai checkout (CMakeLists
#      looks at `../llama.cpp` by default), pinned to a tag known to
#      build cleanly with the Bonsai 8B Q1_0 GGUF (1.58-bit family). You
#      can override the pin via $LLAMA_CPP_REF.
#   3. Builds easyai with the Metal backend on Apple Silicon (auto), or
#      CPU/AMX on Intel — the CMake configure step picks the right one
#      from the host arch. Verbose build log goes to ./build/build.log.
#   4. Installs the binaries to $PREFIX/bin and the libraries to
#      $PREFIX/lib. The default $PREFIX is the easyai checkout root,
#      so a fresh `git clone … && ./scripts/install_easyai_macos.sh`
#      ends up with bin/, lib/, models/, build/ all inside one tree —
#      uninstall is `rm -rf` of that directory. RPATH is set so no
#      DYLD_LIBRARY_PATH gymnastics are needed at runtime.
#   5. Downloads the Bonsai 8B Q1_0 GGUF into $MODEL_DIR (default
#      $PREFIX/models, i.e. $easyai/models). Skipped if the file
#      already exists.
#   6. Prints the exact `easyai-server` command to run, with the right
#      --model path filled in.
#
# Why a separate script (not the Linux installer): macOS doesn't have
# systemd; brew handles the deps; Metal is the right default backend
# instead of Vulkan/CUDA. The Linux installer (`install_easyai_server.sh`)
# stays focused on Debian-style hosts with a system-service deployment.
#
# Usage:
#
#   ./scripts/install_easyai_macos.sh                       # full setup
#   ./scripts/install_easyai_macos.sh --prefix /usr/local   # custom prefix
#   ./scripts/install_easyai_macos.sh --model-url https://… # alternate model
#   ./scripts/install_easyai_macos.sh --skip-model          # build only
#   ./scripts/install_easyai_macos.sh --rebuild             # wipe build/
#   ./scripts/install_easyai_macos.sh --jobs 8              # explicit parallelism
#   ./scripts/install_easyai_macos.sh --gemma4              # use Gemma 4 E4B + upstream llama.cpp
#   ./scripts/install_easyai_macos.sh --no-install          # don't run cmake --install
#
# Verify the model URL before running on a slow link — Bonsai 8B Q1_0 is
# ~1.7 GB. The script writes to <name>.partial first and renames on
# success, and `curl -C -` resumes if you re-run after a network drop.
# To swap models entirely, pass --model-url <full URL> and
# --model-file <basename>.
#
# IMPORTANT — the default URL is a placeholder: HuggingFace GGUF URLs
# rotate when uploaders revise their repos. Verify the link below
# before downloading, and pass --model-url if it 404s.
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults (override via env or flags). Every default is a single-source
# value here so a future Bonsai release pin is a one-line change.
# ---------------------------------------------------------------------------

# Where the script itself lives. Used to anchor src/build paths on the
# easyai checkout regardless of where the user invoked us from.
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
easyai_dir="$(cd "$script_dir/.." && pwd)"
src_root="$(cd "$easyai_dir/.." && pwd)"
llama_dir="$src_root/llama.cpp"

# Install prefix — defaults to the easyai checkout itself so
# everything (bin, lib, models, build/) lives under one tree. No
# sudo needed; uninstall is `rm -rf` the directory. Override with
# --prefix /usr/local for a traditional system install (sudo).
PREFIX="${PREFIX:-$easyai_dir}"
MODEL_DIR="${MODEL_DIR:-$PREFIX/models}"
BUILD_DIR="${BUILD_DIR:-$easyai_dir/build}"

# llama.cpp source — Bonsai 8B Q1_0 ships a custom 1.125-bit kernel
# (1 sign bit + FP16 scale per group of 128 weights, no FP16
# materialisation) that lives ONLY in PrismML's fork. The upstream
# ggml-org/llama.cpp will load the GGUF metadata then fail to
# dequantize at the first decode step. Track the fork's master so
# we pick up Q1_0 kernel fixes; override $LLAMA_CPP_REPO if PrismML
# upstreams the kernel later.
LLAMA_CPP_REPO="${LLAMA_CPP_REPO:-https://github.com/PrismML-Eng/llama.cpp.git}"
LLAMA_CPP_REF="${LLAMA_CPP_REF:-master}"

# Bonsai 8B Q1_0 — prism-ml's release on HuggingFace. ~2 GB on disk.
# Q1_0 is the new ternary-style 1-bit family; you need a recent
# llama.cpp (the LLAMA_CPP_REF default tracks master so it picks up
# Q1_0 support automatically).
#
# If this URL 404s (HF reorganises repos occasionally), browse to:
#   https://huggingface.co/prism-ml/Bonsai-8B-gguf
# pick a current variant, and pass --model-url <url> --model-file <basename>.
MODEL_URL="${MODEL_URL:-https://huggingface.co/prism-ml/Bonsai-8B-gguf/resolve/main/Bonsai-8B-Q1_0.gguf}"
MODEL_FILE="${MODEL_FILE:-Bonsai-8B-Q1_0.gguf}"

# Build parallelism. Defaults to all logical cores; override with --jobs.
JOBS="${JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)}"

# Toggles flipped by flags below.
SKIP_MODEL=0
SKIP_INSTALL=0
REBUILD=0
GEMMA4=0

# ---------------------------------------------------------------------------
# Pretty-printing helpers — `step` for major phases, `log` for substeps,
# `die` for unrecoverable errors. ANSI colour only when stdout is a tty;
# the heredoc-style messages stay readable in a piped log either way.
# ---------------------------------------------------------------------------
if [[ -t 1 ]]; then
    C_RESET=$'\033[0m'
    C_BOLD=$'\033[1m'
    C_GREEN=$'\033[32m'
    C_YELLOW=$'\033[33m'
    C_RED=$'\033[31m'
    C_CYAN=$'\033[36m'
else
    C_RESET=''; C_BOLD=''; C_GREEN=''; C_YELLOW=''; C_RED=''; C_CYAN=''
fi

step() { printf '\n%s==>%s %s%s%s\n' "$C_GREEN" "$C_RESET" "$C_BOLD" "$*" "$C_RESET"; }
log()  { printf '    %s\n' "$*"; }
warn() { printf '%s[warn]%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
die()  { printf '%s[fatal]%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Flag parser. Kept tiny on purpose — every flag has a default above so
# the script is usable with zero arguments.
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)        PREFIX="$2"; MODEL_DIR="${MODEL_DIR_OVERRIDE:-$PREFIX/models}"; shift 2 ;;
        --model-dir)     MODEL_DIR="$2"; MODEL_DIR_OVERRIDE=1; shift 2 ;;
        --model-url)     MODEL_URL="$2"; shift 2 ;;
        --model-file)    MODEL_FILE="$2"; shift 2 ;;
        --llama-ref)     LLAMA_CPP_REF="$2"; shift 2 ;;
        --llama-repo)    LLAMA_CPP_REPO="$2"; shift 2 ;;
        --build-dir)     BUILD_DIR="$2"; shift 2 ;;
        --jobs)          JOBS="$2"; shift 2 ;;
        --skip-model)    SKIP_MODEL=1; shift ;;
        --no-install)    SKIP_INSTALL=1; shift ;;
        --gemma4)        GEMMA4=1; shift ;;
        --rebuild)       REBUILD=1; shift ;;
        -h|--help)
            sed -n '/^# Usage:/,/^# ===/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            die "unknown arg: $1 (try --help)" ;;
    esac
done

# ---------------------------------------------------------------------------
# --gemma4: switch to upstream llama.cpp + Gemma 4 E4B Q4_K_M model.
# ---------------------------------------------------------------------------
if [[ "$GEMMA4" == "1" ]]; then
    LLAMA_CPP_REPO="https://github.com/ggml-org/llama.cpp.git"
    LLAMA_CPP_REF="master"
    MODEL_URL="https://huggingface.co/unsloth/gemma-4-E4B-it-GGUF/resolve/main/gemma-4-E4B-it-Q4_K_M.gguf"
    MODEL_FILE="gemma-4-E4B-it-Q4_K_M.gguf"
fi

# ---------------------------------------------------------------------------
# Sanity — must be macOS.
# ---------------------------------------------------------------------------
if [[ "$(uname -s)" != "Darwin" ]]; then
    die "this script is macOS-only. For Linux see scripts/install_easyai_server.sh"
fi
arch="$(uname -m)"
case "$arch" in
    arm64)  log "host: macOS arm64 (Apple Silicon) — Metal backend will be used" ;;
    x86_64) log "host: macOS x86_64 (Intel) — Metal backend works, AMX kernels active" ;;
    *)      warn "host: unrecognised arch '$arch' — proceeding, but YMMV" ;;
esac

# ---------------------------------------------------------------------------
# Step 1 — deps (Xcode CLT, Homebrew, cmake, git, curl, libomp opt).
# ---------------------------------------------------------------------------
step "Checking build dependencies"

if ! xcode-select -p >/dev/null 2>&1; then
    log "Xcode Command Line Tools missing — running 'xcode-select --install'"
    log "(if a dialog opens, accept it and rerun this script)"
    xcode-select --install || true
    die "rerun after Xcode CLT install completes"
fi
log "Xcode Command Line Tools: $(xcode-select -p)"

if ! command -v brew >/dev/null 2>&1; then
    die "Homebrew not found. Install via https://brew.sh/ then rerun."
fi
log "brew: $(brew --version | head -1)"

# git + curl ship with Xcode CLT, so we only brew-install them when the
# command is missing entirely. cmake and openssl@3 are different — they
# don't come with CLT and openssl@3 is keg-only (no PATH symlinks), so
# we probe brew directly and install if absent regardless of any system
# stubs. macOS' /usr/lib has no usable libssl anymore, so without brew
# openssl, CMake's find_package(OpenSSL) half-detects something and
# fails to create the imported OpenSSL::SSL target, breaking both
# easyai_cli and the vendored cpp-httplib (LLAMA_OPENSSL=ON by default).
need_install=()

for p in git curl; do
    if command -v "$p" >/dev/null 2>&1; then
        log "$p: $(command -v "$p")"
    elif ! brew list --formula "$p" >/dev/null 2>&1; then
        need_install+=("$p")
    else
        log "$p: installed via brew (not linked)"
    fi
done

for p in cmake openssl@3; do
    if brew list --formula "$p" >/dev/null 2>&1; then
        log "$p: installed via brew"
    else
        need_install+=("$p")
    fi
done

if [[ ${#need_install[@]} -gt 0 ]]; then
    log "installing brew packages: ${need_install[*]}"
    brew install "${need_install[@]}"
fi

# Capture the brew openssl prefix for the cmake configure below. Brew's
# openssl is keg-only, so we have to spoonfeed the path to find_package.
OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
log "openssl root: $OPENSSL_ROOT_DIR"

# ---------------------------------------------------------------------------
# Step 2 — clone / refresh llama.cpp sibling.
# ---------------------------------------------------------------------------
step "Preparing llama.cpp sibling at $llama_dir"
if [[ ! -d "$llama_dir/.git" ]]; then
    log "cloning $LLAMA_CPP_REPO -> $llama_dir"
    git clone --depth 50 "$LLAMA_CPP_REPO" "$llama_dir"
else
    log "found existing checkout; fetching"
    git -C "$llama_dir" fetch --tags --depth 50 origin "$LLAMA_CPP_REF" 2>/dev/null || \
        git -C "$llama_dir" fetch --tags origin
fi
log "checking out $LLAMA_CPP_REF"
git -C "$llama_dir" checkout --quiet "$LLAMA_CPP_REF"
log "llama.cpp HEAD: $(git -C "$llama_dir" rev-parse --short HEAD) ($(git -C "$llama_dir" log -1 --pretty=%s | cut -c-60))"

# ---------------------------------------------------------------------------
# Step 3 — configure + build easyai.
# ---------------------------------------------------------------------------
step "Building easyai (prefix=$PREFIX, jobs=$JOBS)"
if [[ "$REBUILD" == "1" && -d "$BUILD_DIR" ]]; then
    log "--rebuild: removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# Guard against the classic "I ran this with sudo once and now nothing
# works" pitfall: if anything inside $BUILD_DIR is owned by root (or
# anyone other than $USER), `cmake --install` will hit Permission
# denied on install_manifest.txt mid-run, leaving the install half-
# applied. Detect early, tell the operator how to fix, exit before
# cmake even starts.
if [[ -d "$BUILD_DIR" ]]; then
    foreign_owner="$(find "$BUILD_DIR" -maxdepth 2 ! -user "$USER" -print -quit 2>/dev/null || true)"
    if [[ -n "$foreign_owner" ]]; then
        warn "$BUILD_DIR contains files not owned by $USER:"
        warn "  example: $foreign_owner"
        warn "this happens when a previous run was invoked with sudo, leaving"
        warn "root-owned artefacts behind. Pick one and rerun:"
        warn ""
        warn "  # keep the build cache, fix ownership:"
        warn "  sudo chown -R $USER $BUILD_DIR"
        warn ""
        warn "  # nuke and start fresh:"
        warn "  sudo rm -rf $BUILD_DIR"
        die "ownership mismatch — refusing to continue"
    fi
fi

# CMake flags. We let easyai's CMakeLists pick Metal vs CPU off the
# host arch (it does: APPLE && CMAKE_SYSTEM_PROCESSOR=arm64 → Metal).
# Explicit flags here would only fight that detection.
cmake_args=(
    -S "$easyai_dir"
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
    -DOPENSSL_ROOT_DIR="$OPENSSL_ROOT_DIR"
    -DEASYAI_BUILD_SERVICES=ON
    -DEASYAI_WITH_CURL=ON
    -DEASYAI_BUILD_WEBUI=ON
    -DEASYAI_INSTALL=ON
)

log "cmake configure"
cmake "${cmake_args[@]}" | tail -20

log "cmake --build (parallel $JOBS, log: $BUILD_DIR/build.log)"
mkdir -p "$BUILD_DIR"
if ! cmake --build "$BUILD_DIR" -j "$JOBS" 2>&1 | tee "$BUILD_DIR/build.log" | tail -10; then
    die "build failed — see $BUILD_DIR/build.log for full output"
fi

# ---------------------------------------------------------------------------
# Step 4 — install (cmake --install). Skipped under --no-install for a
# fast local-dev edit/build cycle.
# ---------------------------------------------------------------------------
if [[ "$SKIP_INSTALL" == "1" ]]; then
    step "Skipping install (--no-install) — binaries live under $BUILD_DIR"
else
    step "Installing to $PREFIX"
    if [[ -w "$PREFIX" || ! -e "$PREFIX" ]]; then
        cmake --install "$BUILD_DIR"
    else
        log "$PREFIX is not writeable as $USER — using sudo"
        sudo cmake --install "$BUILD_DIR"
    fi
    log "installed: $(ls "$PREFIX/bin" 2>/dev/null | tr '\n' ' ')"
fi

# Resolve the binary we'll print in the final command. Falls back to
# the in-build copy when --no-install was passed.
if [[ "$SKIP_INSTALL" == "1" ]]; then
    server_bin="$BUILD_DIR/easyai-server"
else
    server_bin="$PREFIX/bin/easyai-server"
fi
[[ -x "$server_bin" ]] || die "easyai-server missing at $server_bin (build problem?)"

# ---------------------------------------------------------------------------
# Step 5 — model download.
# ---------------------------------------------------------------------------
model_path="$MODEL_DIR/$MODEL_FILE"
if [[ "$SKIP_MODEL" == "1" ]]; then
    step "Skipping model download (--skip-model)"
elif [[ -f "$model_path" ]]; then
    step "Model already present: $model_path"
    log "size: $(du -h "$model_path" | awk '{print $1}')"
    log "(delete and rerun to force a fresh download)"
else
    step "Downloading model to $model_path"
    mkdir -p "$MODEL_DIR"
    log "URL: $MODEL_URL"
    log "this may take a while on a slow link. Hit Ctrl+C to abort."
    # curl flags:
    #   -f  fail on HTTP 4xx/5xx (otherwise we'd save the error page as a .gguf)
    #   -L  follow redirects (HF returns 302 to a CDN host)
    #   -C - resume partial download if any
    #   --retry/--retry-delay handles flaky CDN edges
    #   --output-dir + --remote-name would lose the configured filename
    if ! curl -fL --retry 3 --retry-delay 4 -C - -o "$model_path.partial" "$MODEL_URL"; then
        rm -f "$model_path.partial"
        die "download failed. Verify the URL is still valid, then
       re-run with: --model-url <url> --model-file <basename>"
    fi
    mv "$model_path.partial" "$model_path"
    log "saved: $model_path  ($(du -h "$model_path" | awk '{print $1}'))"
fi

# ---------------------------------------------------------------------------
# Step 6 — print the run command.
# ---------------------------------------------------------------------------
step "Done. Run easyai-server with:"

if [[ "$GEMMA4" == "1" ]]; then
cat <<EOF

  ${C_CYAN}# add easyai to PATH for this shell:${C_RESET}
  export PATH="$PREFIX/bin:\$PATH"

  ${C_CYAN}# start the server (Gemma 4 E4B defaults, LAN-reachable):${C_RESET}
  $server_bin \\
      -m "$model_path" \\
      --ngl 99 \\
      -c 8192 \\
      --host 0.0.0.0 \\
      --port 8080

  ${C_CYAN}# in another terminal, smoke-test:${C_RESET}
  curl -s http://127.0.0.1:8080/health | python3 -m json.tool

  ${C_CYAN}# OR open the webui:${C_RESET}
  open http://127.0.0.1:8080/

EOF
else
cat <<EOF

  ${C_CYAN}# add easyai to PATH for this shell:${C_RESET}
  export PATH="$PREFIX/bin:\$PATH"

  ${C_CYAN}# start the server (Bonsai-tuned defaults, LAN-reachable):${C_RESET}
  $server_bin \\
      -m "$model_path" \\
      --ngl 99 \\
      -c 8192 \\
      --temperature 0.5 \\
      --top-p 0.85 \\
      --top-k 20 \\
      --host 0.0.0.0 \\
      --port 8080

  ${C_CYAN}# in another terminal, smoke-test:${C_RESET}
  curl -s http://127.0.0.1:8080/health | python3 -m json.tool

  ${C_CYAN}# OR open the webui:${C_RESET}
  open http://127.0.0.1:8080/

EOF
fi

if [[ "$SKIP_MODEL" == "1" ]]; then
    warn "you skipped the model download. Provide -m <path-to.gguf> to start."
fi
