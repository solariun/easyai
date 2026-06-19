#!/usr/bin/env bash
# =============================================================================
# build_macos.sh — configure + build easyai with the Metal/AMX backend.
#
# macOS-only. CMake auto-enables Metal on Apple Silicon and AMX on
# recent Intel; no explicit `-DGGML_METAL=ON` needed. This script just
# stays out of llama.cpp's way and lets it pick the right kernels.
#
# llama.cpp source — IMPORTANT
# ----------------------------
# This script links against the sibling directory `../llama.cpp` and
# auto-clones upstream (ggml-org/llama.cpp) there if it's missing.
# Override the URL via $LLAMA_CPP_REPO if you need a different fork —
# e.g. the PrismML fork shipped by ./scripts/install_easyai_macos.sh,
# the only one with the Bonsai-8B-Q1_0 1.125-bit kernel.
#
# Fork compatibility:
#   * PrismML fork  → Bonsai-8B-Q1_0 (Q1_0 kernel only lives there)
#   * Upstream      → every other quant family
#
# Pass --upgrade to `git pull --ff-only` both this easyai checkout and
# ../llama.cpp before configuring, then rebuild against the fresher
# trees. Local commits/changes that don't fast-forward abort the pull
# rather than auto-merging — resolve them by hand and rerun.
#
# By default this script ONLY configures + builds. Output lands under
# ./build-macos/. Pass --install to ALSO run `sudo cmake --install`
# into /usr/local (override with --prefix). For the full first-time
# setup including deps + a default model, use scripts/install_easyai_macos.sh.
#
# Build deps (one-time):
#   xcode-select --install
#   brew install cmake git curl
#
# Usage:
#   ./build_macos.sh                          # Release, all logical cores
#   ./build_macos.sh --debug                  # Debug build
#   ./build_macos.sh --jobs 6                 # explicit -j 6
#   ./build_macos.sh --rebuild                # wipe build-macos/ first
#   ./build_macos.sh --upgrade                # git pull easyai + ../llama.cpp, then rebuild
#   ./build_macos.sh --build-dir build-foo
#   ./build_macos.sh --install                # build then `sudo cmake --install` to /usr/local
#   ./build_macos.sh --install --prefix /opt/easyai
#                                             # custom install prefix (still via sudo)
# =============================================================================

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BUILD_DIR="${BUILD_DIR:-$script_dir/build-macos}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)}"
REBUILD=0
UPGRADE=0
DO_INSTALL=0
INSTALL_PREFIX="${INSTALL_PREFIX:-/usr/local}"

# llama.cpp source. Defaults to upstream so a from-scratch checkout
# produces a working binary for every non-Bonsai GGUF. The installer
# (scripts/install_easyai_macos.sh) overrides this to the PrismML fork
# when it sets up Bonsai-8B-Q1_0 — we don't touch an existing sibling
# checkout, so that setup is preserved.
LLAMA_CPP_REPO="${LLAMA_CPP_REPO:-https://github.com/ggml-org/llama.cpp.git}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rebuild)   REBUILD=1; shift ;;
        --upgrade)   UPGRADE=1; shift ;;
        --debug)     BUILD_TYPE=Debug; shift ;;
        --jobs)      JOBS="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --install)   DO_INSTALL=1; shift ;;
        --prefix)    INSTALL_PREFIX="$2"; DO_INSTALL=1; shift 2 ;;
        -h|--help)
            sed -n '/^# Usage:/,/^# ===/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown arg: $1 (try --help)" >&2; exit 1 ;;
    esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "build_macos.sh: not macOS — use ./build_vulkan.sh or ./build_cuda.sh" >&2
    exit 1
fi

# Sibling llama.cpp — auto-cloned from $LLAMA_CPP_REPO (default upstream)
# when missing. An existing checkout (e.g. the PrismML fork dropped by
# install_easyai_macos.sh) is kept untouched.
llama_dir="$(dirname "$script_dir")/llama.cpp"
if [[ ! -d "$llama_dir/.git" ]]; then
    echo "==> Cloning $LLAMA_CPP_REPO -> $llama_dir"
    git clone "$LLAMA_CPP_REPO" "$llama_dir"
fi

# --upgrade: fast-forward both checkouts before configuring. We pull
# easyai first; if `git pull --ff-only` would clobber local commits or
# uncommitted edits that conflict, it aborts and the build never starts.
# llama.cpp pulls from whatever remote/branch is already configured —
# upstream for fresh clones, PrismML for installer-seeded trees.
if [[ "$UPGRADE" == "1" ]]; then
    echo "==> --upgrade: pulling easyai ($script_dir)"
    git -C "$script_dir" pull --ff-only
    echo "==> --upgrade: pulling llama.cpp ($llama_dir)"
    git -C "$llama_dir" pull --ff-only
fi

# Show what fork is actually wired in, so the operator can confirm
# they're not accidentally building against an upstream checkout when
# they meant the PrismML fork (Bonsai will load but fail at decode).
llama_origin="$(git -C "$llama_dir" config --get remote.origin.url 2>/dev/null || echo unknown)"
llama_head="$(git -C "$llama_dir" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "==> llama.cpp sibling: $llama_origin @ $llama_head"
case "$llama_origin" in
    *PrismML-Eng/llama.cpp*) echo "    (PrismML fork — Bonsai-8B-Q1_0 supported)" ;;
    *ggml-org/llama.cpp*)    echo "    (upstream — Bonsai-8B-Q1_0 will fail at decode; switch fork or pick another GGUF)" ;;
    *)                       echo "    (custom fork — proceeding, you know what you're doing)" ;;
esac

if [[ "$REBUILD" == "1" && -d "$BUILD_DIR" ]]; then
    echo "==> --rebuild: removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# Foreign-ownership guard — same protection as install_easyai_macos.sh:
# a stray sudo run leaves root-owned files in the build dir, which
# breaks the next unprivileged cmake --build with a confusing
# permission error. Detect early.
if [[ -d "$BUILD_DIR" ]]; then
    foreign_owner="$(find "$BUILD_DIR" -maxdepth 2 ! -user "$USER" -print -quit 2>/dev/null || true)"
    if [[ -n "$foreign_owner" ]]; then
        cat >&2 <<EOF
==> $BUILD_DIR contains files not owned by $USER:
       $foreign_owner

This typically happens after a sudo invocation. Pick one and rerun:

   # keep the build cache, fix ownership:
   sudo chown -R $USER $BUILD_DIR

   # nuke and start fresh:
   sudo rm -rf $BUILD_DIR
EOF
        exit 1
    fi
fi

# brew openssl@3 is keg-only — find_package(OpenSSL) needs an explicit
# hint or it half-detects the macOS system libs and fails to create the
# imported OpenSSL::SSL target. Resolve once here and pass to CMake.
# Empty string is harmless to CMake (it'll fall back to its own search).
openssl_root=""
if command -v brew >/dev/null 2>&1; then
    openssl_root="$(brew --prefix openssl@3 2>/dev/null || true)"
fi
if [[ -z "$openssl_root" ]]; then
    echo "==> brew openssl@3 not found — HTTPS in easyai-cli will be disabled."
    echo "    install with: brew install openssl@3"
fi

echo "==> Configuring (Metal/AMX, $BUILD_TYPE, $BUILD_DIR)"
cmake -S "$script_dir" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DOPENSSL_ROOT_DIR="$openssl_root" \
    -DEASYAI_BUILD_SERVICES=ON \
    -DEASYAI_WITH_CURL=ON \
    -DEASYAI_BUILD_WEBUI=ON \
    -DEASYAI_INSTALL=ON

echo "==> Building (jobs=$JOBS)"
cmake --build "$BUILD_DIR" -j "$JOBS"

echo
echo "==> Done. Binaries in $BUILD_DIR:"
ls "$BUILD_DIR" 2>/dev/null | grep -E '^easyai' | sed 's/^/    /'

if [[ "$DO_INSTALL" == "1" ]]; then
    # Decide on sudo: anything under /usr/local on Apple Silicon is
    # root-owned, and even on Intel /usr/local/bin is in the admin group
    # so unprivileged installs partially fail with confusing chmod
    # errors. Detect by trying to write a probe file; sudo if not
    # writable. Honours an explicit --prefix in $HOME so writable
    # custom prefixes don't gratuitously prompt.
    sudo_cmd=""
    probe="$INSTALL_PREFIX/.easyai-write-probe.$$"
    if mkdir -p "$INSTALL_PREFIX" 2>/dev/null && : > "$probe" 2>/dev/null; then
        rm -f "$probe"
    else
        sudo_cmd="sudo"
    fi

    echo
    echo "==> Installing into $INSTALL_PREFIX${sudo_cmd:+ (via sudo)}"
    # `cmake --install ... --prefix X` overrides CMAKE_INSTALL_PREFIX
    # at install time so the build directory doesn't need
    # reconfiguration when the operator points at a non-default prefix.
    $sudo_cmd cmake --install "$BUILD_DIR" --prefix "$INSTALL_PREFIX"

    echo
    echo "==> Installed to $INSTALL_PREFIX:"
    ls "$INSTALL_PREFIX/bin" 2>/dev/null | grep -E '^easyai' | sed "s|^|    $INSTALL_PREFIX/bin/|"
    echo
    case ":$PATH:" in
        *":$INSTALL_PREFIX/bin:"*) : ;;
        *) echo "    NOTE: $INSTALL_PREFIX/bin is not in \$PATH — add it to use the binaries by name." ;;
    esac
    echo "Run e.g.:"
    echo "    easyai-server -m \$HOME/easyai/models/Bonsai-8B-Q1_0.gguf \\"
    echo "        --ngl 99 -c 8192 --temperature 0.5 --top-p 0.85 --top-k 20 \\"
    echo "        --host 0.0.0.0 --port 8080"
else
    echo
    echo "Run one without installing, e.g.:"
    echo "    $BUILD_DIR/easyai-server -m \$HOME/easyai/models/Bonsai-8B-Q1_0.gguf \\"
    echo "        --ngl 99 -c 8192 --temperature 0.5 --top-p 0.85 --top-k 20 \\"
    echo "        --host 0.0.0.0 --port 8080"
    echo
    echo "Or install system-wide with:"
    echo "    ./build_macos.sh --install               # → /usr/local (sudo)"
    echo "    ./build_macos.sh --install --prefix /opt/easyai"
fi
