#!/usr/bin/env bash
# =============================================================================
# easyai install — Raspberry Pi 4 / Pi 5 (Debian / Raspberry Pi OS)
#
# What this script does, in order:
#
#   1. Verifies the host (Pi 4 / Pi 5, ARM64) and prints its capabilities
#      (RAM, CPU model, Pi revision). Refuses to run on non-ARM64 boxes
#      so the user runs the right script on the right hardware.
#   2. Installs the apt deps (build-essential + cmake + ninja + git +
#      libcurl + systemd-coredump). No GPU SDK — Pi 4/5 GPUs aren't
#      useful for llama.cpp inference; we run CPU-only with NEON.
#   3. Clones PrismML's llama.cpp fork (Bonsai 8B Q1_0 needs a custom
#      kernel that lives ONLY there; upstream ggml-org/llama.cpp will
#      load the GGUF then fail at decode) and the easyai checkout.
#   4. Builds easyai (Release, NEON kernels auto-enabled by llama.cpp's
#      CMake on aarch64).
#   5. Installs binaries to $PREFIX/bin and libs to $PREFIX/lib (default
#      $PREFIX = /usr/local). RPATH points at the install-time lib dir.
#   6. Creates the easyai service user, /var/lib/easyai/{models,workspace,rag},
#      and /etc/easyai with a Bonsai-tuned easyai.ini.
#   7. Downloads Bonsai 8B Q1_0 GGUF to /var/lib/easyai/models/ (skip
#      with --no-model). Symlinks ai.gguf → the downloaded file so the
#      service unit doesn't carry a versioned filename.
#   8. Drops a systemd unit (easyai-server.service) tuned for Pi:
#      4 threads, 4 K context, OOM-protected, swap-aware. Enables + starts
#      the service unless --no-enable was passed.
#   9. Prints the service status, the LAN URL, and a smoke-test curl.
#
# Why a Pi-specific script (not the general Linux installer): Pi 4/5
# don't benefit from Vulkan/CUDA/HIP, so the GPU detection + driver
# install in install_easyai_server.sh is dead weight here. Pi tuning
# (4 cores, smaller ctx default, ARM-only deps, no SCHED_FIFO) is also
# different enough to deserve its own surface.
#
# Usage:
#   sudo ./scripts/install_easyai_pi.sh                       # full setup
#   sudo ./scripts/install_easyai_pi.sh --no-model            # skip GGUF download
#   sudo ./scripts/install_easyai_pi.sh --port 80             # bind low port
#   sudo ./scripts/install_easyai_pi.sh --host 127.0.0.1      # LAN-only off
#   sudo ./scripts/install_easyai_pi.sh --threads 3           # leave 1 core for OS
#   sudo ./scripts/install_easyai_pi.sh --ctx 8192            # bigger KV cache
#   sudo ./scripts/install_easyai_pi.sh --rebuild             # wipe build/ first
#   sudo ./scripts/install_easyai_pi.sh --no-enable           # install but don't start
#   sudo ./scripts/install_easyai_pi.sh --mdns-hostname my-ai # different .local name
#   sudo ./scripts/install_easyai_pi.sh --no-mdns             # keep current hostname, no mDNS
#   sudo ./scripts/install_easyai_pi.sh --model-url <url> --model-file <name>
#
# Pi 4 with 4 GB RAM is tight (Bonsai is ~1.2 GB resident + KV cache +
# OS); 8 GB is the sweet spot. Pi 5 fares much better thanks to the
# Cortex-A76 IPC bump.
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults — every value here is the single source of truth. Override via
# env or flag below.
# ---------------------------------------------------------------------------

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
easyai_dir="$(cd "$script_dir/.." && pwd)"
src_root="$(cd "$easyai_dir/.." && pwd)"
llama_dir="$src_root/llama.cpp"

# Sources. PrismML fork is REQUIRED for Bonsai 8B Q1_0 (custom kernel).
# Override $LLAMA_CPP_REPO if PrismML upstreams the kernel later.
LLAMA_CPP_REPO="${LLAMA_CPP_REPO:-https://github.com/PrismML-Eng/llama.cpp.git}"
LLAMA_CPP_REF="${LLAMA_CPP_REF:-master}"

# Install layout — system-wide because we run easyai-server as a daemon.
PREFIX="${PREFIX:-/usr/local}"
BUILD_DIR="${BUILD_DIR:-$easyai_dir/build}"

# Service identity / paths.
service_user="easyai"
service_group="easyai"
service_home="/var/lib/easyai"
service_model_dir="$service_home/models"
service_model_link="ai.gguf"
service_workspace="$service_home/workspace"
service_rag="$service_home/rag"
service_name="easyai-server.service"
config_dir="/etc/easyai"
ini_file="$config_dir/easyai.ini"
system_file="$config_dir/system.txt"
external_tools_dir="$config_dir/external-tools"

# Service network defaults — Pi is typically a LAN appliance, so bind to
# every interface by default. Override with --host 127.0.0.1 for
# loopback-only.
service_host="0.0.0.0"
service_port=80

# mDNS — make the Pi reachable on the LAN as `<name>.local` without
# the operator having to look up an IP that DHCP may move around.
# Default name is `pi-ai`; pass --mdns-hostname <name> for a different
# one or --no-mdns to skip the hostname change + avahi service entirely.
mdns_hostname="pi-ai"
do_mdns=1

# Bonsai 8B Q1_0 — prism-ml/Bonsai-8B-gguf.
MODEL_URL="${MODEL_URL:-https://huggingface.co/prism-ml/Bonsai-8B-gguf/resolve/main/Bonsai-8B-Q1_0.gguf}"
MODEL_FILE="${MODEL_FILE:-Bonsai-8B-Q1_0.gguf}"

# Pi-tuned engine defaults. The Pi 4/5 has 4 cores; using all 4 leaves
# nothing for the OS / network stack. We default to 4 because the model
# is so small that the inference loop is the bottleneck; the operator
# can drop to 3 with --threads 3 if they're co-locating other services.
n_threads_default=4
# 4 K context strikes the right balance on a 4 GB Pi: KV cache scales
# linearly with ctx, and Bonsai's modest size means a 4 K window holds
# a long enough chat without paging.
ctx_size=4096
# CPU-only: no layers go to a non-existent GPU.
ngl=0
# Ambient sampling — Bonsai's HF README:
#   temp 0.5, top_p 0.85, top_k 20, repeat_penalty 1.0
sampling_temp=0.5
sampling_top_p=0.85
sampling_top_k=20

# Toggles flipped by flags below.
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
SKIP_MODEL=0
DO_ENABLE=1
REBUILD=0
DO_BUILD=1

# ---------------------------------------------------------------------------
# Pretty-printing.
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
# Flags.
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)        PREFIX="$2"; shift 2 ;;
        --build-dir)     BUILD_DIR="$2"; shift 2 ;;
        --jobs)          JOBS="$2"; shift 2 ;;
        --threads)       n_threads_default="$2"; shift 2 ;;
        --ctx)           ctx_size="$2"; shift 2 ;;
        --host)          service_host="$2"; shift 2 ;;
        --port)          service_port="$2"; shift 2 ;;
        --model-url)     MODEL_URL="$2"; shift 2 ;;
        --model-file)    MODEL_FILE="$2"; shift 2 ;;
        --llama-ref)     LLAMA_CPP_REF="$2"; shift 2 ;;
        --llama-repo)    LLAMA_CPP_REPO="$2"; shift 2 ;;
        --no-model)      SKIP_MODEL=1; shift ;;
        --no-enable)     DO_ENABLE=0; shift ;;
        --no-build)      DO_BUILD=0; shift ;;
        --rebuild)       REBUILD=1; shift ;;
        --mdns-hostname) mdns_hostname="$2"; shift 2 ;;
        --no-mdns)       do_mdns=0; shift ;;
        -h|--help)
            sed -n '/^# Usage:/,/^# ===/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            die "unknown arg: $1 (try --help)" ;;
    esac
done

# ---------------------------------------------------------------------------
# Sanity — must be Linux ARM64 on a Pi-class board. Refuses to proceed
# on the wrong hardware so the operator runs the right script.
# ---------------------------------------------------------------------------
[[ "$(uname -s)" == "Linux" ]] || die "this script is Linux-only"
arch="$(uname -m)"
case "$arch" in
    aarch64|arm64) : ;;
    *) die "expected aarch64 (Pi 4/5 64-bit OS); got '$arch'. Reflash with the 64-bit Raspberry Pi OS or use install_easyai_server.sh on x86_64 hosts." ;;
esac

# Pi-class detection (informational; we don't refuse non-Pi boards but
# we want to log what we see for diagnostics).
pi_model="unknown ARM64 board"
if [[ -r /proc/device-tree/model ]]; then
    # /proc/device-tree/model has a trailing NUL — strip with `tr -d`.
    pi_model="$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo unknown)"
fi

# Privilege check — most steps need root. We re-exec under sudo so the
# operator doesn't have to remember.
if [[ $EUID -ne 0 ]]; then
    if command -v sudo >/dev/null 2>&1; then
        log "re-running under sudo"
        exec sudo -E "$0" "$@"
    else
        die "this script needs root; install sudo or rerun as root"
    fi
fi

# ---------------------------------------------------------------------------
# Step 1 — host info banner.
# ---------------------------------------------------------------------------
step "Host snapshot"
log "model:    $pi_model"
log "arch:     $arch"
log "kernel:   $(uname -r)"
log "RAM:      $(free -h | awk '/^Mem:/ {print $2}')"
log "CPU:      $(awk -F: '/^model name|^Hardware|^Model/ {gsub(/^ +/,"",$2); print $2; exit}' /proc/cpuinfo)"
log "cores:    $(nproc)"

# Warn the operator if they're on 4 GB of RAM — Bonsai 8B Q1 fits but
# leaves very little for the system, KV cache, and any other services.
ram_kb="$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)"
if [[ "$ram_kb" -lt 5500000 ]]; then     # ~5.5 GB threshold
    warn "this host has < 8 GB RAM. Bonsai 8B Q1 + 4 K context + OS is tight"
    warn "on 4 GB Pis. Consider --ctx 2048 or run swap (Pi OS default)."
fi

# ---------------------------------------------------------------------------
# Step 2 — apt deps. CPU-only — we deliberately avoid mesa-vulkan-drivers
# and friends: VideoCore VI/VII Vulkan support is mature for graphics
# but irrelevant to llama.cpp inference, and pulling those packages
# bloats a fresh Pi OS by ~150 MB.
# ---------------------------------------------------------------------------
step "Installing build + runtime deps"
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git ccache pkg-config curl ca-certificates \
    libcurl4-openssl-dev jq \
    systemd-coredump
if [[ "$do_mdns" == "1" ]]; then
    apt-get install -y --no-install-recommends avahi-daemon avahi-utils
fi

# ---------------------------------------------------------------------------
# Step 3 — clone llama.cpp (PrismML fork) sibling.
# ---------------------------------------------------------------------------
step "Preparing llama.cpp at $llama_dir"
if [[ ! -d "$llama_dir/.git" ]]; then
    log "cloning $LLAMA_CPP_REPO -> $llama_dir"
    git clone --filter=blob:none "$LLAMA_CPP_REPO" "$llama_dir"
else
    log "found existing checkout; fetching"
    git -C "$llama_dir" fetch --tags --prune --force
fi
log "checking out $LLAMA_CPP_REF"
git -C "$llama_dir" checkout --quiet "$LLAMA_CPP_REF"
log "llama.cpp HEAD: $(git -C "$llama_dir" rev-parse --short HEAD) ($(git -C "$llama_dir" log -1 --pretty=%s | cut -c-60))"

# ---------------------------------------------------------------------------
# Step 4 — build easyai.
# ---------------------------------------------------------------------------
if [[ "$DO_BUILD" == "1" ]]; then
    step "Building easyai (Release, CPU+NEON, jobs=$JOBS)"
    if [[ "$REBUILD" == "1" && -d "$BUILD_DIR" ]]; then
        log "--rebuild: removing $BUILD_DIR"
        rm -rf "$BUILD_DIR"
    fi
    cmake -S "$easyai_dir" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DEASYAI_BUILD_SERVICES=ON \
        -DEASYAI_WITH_CURL=ON \
        -DEASYAI_BUILD_WEBUI=ON \
        -DEASYAI_INSTALL=ON | tail -20
    log "cmake --build (log: $BUILD_DIR/build.log)"
    if ! cmake --build "$BUILD_DIR" -j "$JOBS" 2>&1 | tee "$BUILD_DIR/build.log" | tail -10; then
        die "build failed — see $BUILD_DIR/build.log"
    fi
else
    log "--no-build: skipping cmake build (assumes $BUILD_DIR is current)"
fi

# ---------------------------------------------------------------------------
# Step 5 — install binaries + libs.
# ---------------------------------------------------------------------------
step "Installing to $PREFIX"
cmake --install "$BUILD_DIR"
log "binaries: $(ls "$PREFIX/bin" | grep -E '^easyai' | tr '\n' ' ')"

server_bin="$PREFIX/bin/easyai-server"
[[ -x "$server_bin" ]] || die "easyai-server missing at $server_bin (build problem?)"

# ---------------------------------------------------------------------------
# Step 6 — service user + dirs + INI config.
# ---------------------------------------------------------------------------
step "Provisioning service user + filesystem layout"

if ! id -u "$service_user" >/dev/null 2>&1; then
    log "creating system user '$service_user'"
    useradd --system --home-dir "$service_home" --shell /usr/sbin/nologin \
            --create-home --user-group "$service_user"
else
    log "user '$service_user' already exists; ensuring home is $service_home"
    install -d -m 750 -o "$service_user" -g "$service_group" "$service_home"
fi

for d in "$service_home" "$service_model_dir" "$service_workspace" "$service_rag"; do
    install -d -m 750 -o "$service_user" -g "$service_group" "$d"
done

install -d -m 755 -o root -g root "$config_dir"
install -d -m 755 -o root -g root "$external_tools_dir"

if [[ ! -f "$system_file" ]]; then
    log "writing $system_file (the operator-editable system prompt)"
    # Bonsai 8B Q1's HF README only suggests "You are a helpful assistant",
    # but easyai's full agentic loop benefits from a slightly richer
    # prompt. Keep it short — Bonsai is small and a long preamble eats
    # context. Operators replace this file freely; the service picks
    # up the new content on restart.
    cat > "$system_file" <<'SYSTXT'
You are a helpful assistant.
Answer concisely. When you don't know something, say so plainly.
When the user asks about "today", "now", or anything time-sensitive,
trust the AUTHORITATIVE DATE/TIME block injected into your prompt
over your training-data intuition.
SYSTXT
    chmod 640 "$system_file"
    chown root:"$service_group" "$system_file"
else
    log "$system_file exists — leaving it alone"
fi

if [[ ! -f "$ini_file" ]]; then
    log "writing $ini_file (Bonsai-tuned defaults)"
    cat > "$ini_file" <<INI
# easyai-server config — generated by install_easyai_pi.sh.
# Edit + restart the service:
#     sudo systemctl restart easyai-server
#
# Precedence: CLI flag > value here > hardcoded default in binary.

[SERVER]
model           = $service_model_dir/$service_model_link
host            = $service_host
port            = $service_port
alias           = Bonsai
sandbox         = $service_workspace
system_file     = $system_file
external_tools  = $external_tools_dir
rag             = $service_rag
webui_title     = Bonsai
metrics         = off
verbose         = off
allow_fs        = off
allow_bash      = off
max_body        = 8388608

# /mcp authentication
# off     : open (anyone reaching /mcp can dispatch any tool) — DEFAULT
# auto    : enabled iff [MCP_USER] below has at least one entry
# on      : require Bearer match — also overridable via --no-mcp-auth
#
# Default 'off' assumes a home-LAN Pi behind a router. If you expose
# port 80 on a public IP, switch to 'on' and populate [MCP_USER]
# below with a strong token (openssl rand -hex 32).
mcp_auth        = off

[ENGINE]
context         = $ctx_size
ngl             = $ngl
threads         = $n_threads_default
preset          = precise
flash_attn      = off
mlock           = off
no_mmap         = off
# Bonsai 8B Q1_0 sampling — straight from the HF README. The 1.125-bit
# quant is more sensitive to high temperature than full-precision; pushing
# temp past 0.7 visibly degrades accuracy. Adjust here, not via CLI.
temperature     = $sampling_temp
top_p           = $sampling_top_p
top_k           = $sampling_top_k
# repeat_penalty = 1.0
# max_tokens    = -1

[MCP_USER]
# Bearer-token auth for POST /mcp. Each line is \`username = bearer_token\`.
# If this section is empty (and SERVER.mcp_auth=auto), /mcp is OPEN —
# fine for a Pi behind a home router, NOT fine on a public IP.
# Generate a strong token:    openssl rand -hex 32
# gustavo  = REPLACE-ME-WITH-OPENSSL-RAND-HEX-32
INI
    chmod 640 "$ini_file"
    chown root:"$service_group" "$ini_file"
else
    log "$ini_file exists — leaving operator edits in place"
fi

# ---------------------------------------------------------------------------
# Step 7 — model download.
# ---------------------------------------------------------------------------
model_path="$service_model_dir/$MODEL_FILE"
if [[ "$SKIP_MODEL" == "1" ]]; then
    step "Skipping model download (--no-model)"
elif [[ -f "$model_path" ]]; then
    step "Model already at $model_path — $(du -h "$model_path" | awk '{print $1}')"
else
    step "Downloading $MODEL_FILE (~1.2 GB)"
    log "URL: $MODEL_URL"
    log "(this is a big-ish download over the Pi's NIC; expect a few minutes)"
    if ! curl -fL --retry 3 --retry-delay 4 -C - -o "$model_path.partial" "$MODEL_URL"; then
        rm -f "$model_path.partial"
        die "download failed. If the URL is stale, find a current Bonsai 8B GGUF on
       https://huggingface.co/prism-ml/Bonsai-8B-gguf
       then re-run with: --model-url <url> --model-file <basename>"
    fi
    mv "$model_path.partial" "$model_path"
    chown "$service_user":"$service_group" "$model_path"
    chmod 640 "$model_path"
    log "saved: $model_path  ($(du -h "$model_path" | awk '{print $1}'))"
fi

# Symlink ai.gguf → MODEL_FILE so the systemd unit references a stable
# name. Operators can swap the underlying GGUF and just relink.
if [[ -f "$model_path" ]]; then
    ln -sfn "$MODEL_FILE" "$service_model_dir/$service_model_link"
    chown -h "$service_user":"$service_group" "$service_model_dir/$service_model_link"
fi

# ---------------------------------------------------------------------------
# Step 8 — systemd unit.
# ---------------------------------------------------------------------------
step "Writing systemd unit /etc/systemd/system/$service_name"

# Build the ExecStart args. INI-first design: only the model path and
# the config path live in the unit; every other knob is in the INI so
# the operator iterates without `systemctl edit` cadence.
args=( --config "$ini_file" -m "$service_model_dir/$service_model_link" )
arg_string=""
for a in "${args[@]}"; do
    arg_string+=" $(printf '%q' "$a")"
done

cat > "/etc/systemd/system/$service_name" <<UNIT
[Unit]
Description=easyai OpenAI-compatible LLM server (Bonsai 8B Q1_0 on Raspberry Pi)
Documentation=https://github.com/solariun/easy
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$service_user
Group=$service_group
WorkingDirectory=$service_home

# Make the service much less likely to be picked by the OOM killer.
# Important on 4 GB Pis where the model + KV + page cache + system
# leave very little headroom.
OOMScoreAdjust=-700

Environment=HOME=$service_home
Environment=XDG_CACHE_HOME=$service_home/cache
Environment=LD_LIBRARY_PATH=$PREFIX/lib

ExecStart=$server_bin$arg_string
Restart=on-failure
RestartSec=10
KillSignal=SIGINT
TimeoutStartSec=0
TimeoutStopSec=20

# Hardening
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ReadWritePaths=$service_home $config_dir
ProtectHome=true
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectControlGroups=yes
LockPersonality=yes
RestrictSUIDSGID=yes
LimitNOFILE=65536
LimitCORE=infinity

# Allow binding to low ports (only relevant if --port < 1024).
AmbientCapabilities=CAP_NET_BIND_SERVICE
CapabilityBoundingSet=CAP_NET_BIND_SERVICE

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload

# Wipe any legacy drop-ins from previous installs that could mask the
# main unit's ExecStart. Same logic as the open-box installer.
dropin_dir="/etc/systemd/system/$service_name.d"
if [[ -d "$dropin_dir" ]]; then
    legacy=( "verbose.conf" )
    for f in "${legacy[@]}"; do
        if [[ -f "$dropin_dir/$f" ]]; then
            log "removing legacy drop-in $dropin_dir/$f"
            rm -f "$dropin_dir/$f"
        fi
    done
    if find "$dropin_dir" -name '*.conf' -exec grep -lE '^ExecStart=' {} + 2>/dev/null | grep -q .; then
        warn "operator drop-in(s) override ExecStart= — they will mask the main unit"
        warn "review with: systemctl cat $service_name"
    fi
fi

# ---------------------------------------------------------------------------
# Step 8.5 — mDNS. Two things together so the Pi shows up on the LAN as
# `<mdns_hostname>.local`:
#
#   1. Set the system hostname so the kernel's mDNS announcement
#      advertises the right A record. avahi-daemon auto-publishes the
#      hostname; no avahi config edit is needed for the basic
#      `<name>.local` resolution to work.
#   2. Drop /etc/avahi/services/easyai.service so DNS-SD aware clients
#      (Safari's Bonjour panel, dns-sd, Avahi browser) discover the
#      easyai HTTP endpoint as a service entry under
#      `<name>.local._http._tcp` — the operator can find the box
#      without knowing the port either.
#
# Skipped under --no-mdns, in which case the operator keeps their
# existing hostname and falls back to LAN-IP access.
# ---------------------------------------------------------------------------
if [[ "$do_mdns" == "1" ]]; then
    step "Configuring mDNS as $mdns_hostname.local"

    current_host="$(hostname)"
    if [[ "$current_host" != "$mdns_hostname" ]]; then
        log "renaming host: $current_host → $mdns_hostname"
        hostnamectl set-hostname "$mdns_hostname"
        # Keep /etc/hosts in sync so `sudo`, etc. don't complain about
        # "unable to resolve host". Replace the loopback line that
        # points at the old hostname; if there's no such line, append.
        if grep -qE "^127\.0\.1\.1[[:space:]]" /etc/hosts; then
            sed -i -E "s|^127\\.0\\.1\\.1[[:space:]].*|127.0.1.1\t$mdns_hostname|" /etc/hosts
        else
            printf '127.0.1.1\t%s\n' "$mdns_hostname" >> /etc/hosts
        fi
    else
        log "hostname already $mdns_hostname; not changing"
    fi

    # Avahi service file. The XML shape is the standard DNS-SD service
    # description; %h is replaced at announce time with the host's
    # current name (so the file stays valid across hostname changes).
    avahi_dir="/etc/avahi/services"
    install -d -m 755 -o root -g root "$avahi_dir"
    cat > "$avahi_dir/easyai.service" <<AVAHI
<?xml version="1.0" standalone='no'?>
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<service-group>
  <name replace-wildcards="yes">easyai on %h</name>
  <service>
    <type>_http._tcp</type>
    <port>$service_port</port>
    <txt-record>path=/</txt-record>
    <txt-record>api=openai</txt-record>
    <txt-record>mcp=/mcp</txt-record>
  </service>
</service-group>
AVAHI
    chmod 644 "$avahi_dir/easyai.service"

    # Make sure avahi-daemon is up so the service file is picked up.
    # `systemctl enable --now` is idempotent; safe to re-run.
    systemctl enable --now avahi-daemon >/dev/null 2>&1 || true
    systemctl reload avahi-daemon 2>/dev/null || systemctl restart avahi-daemon || true

    log "advertised as $mdns_hostname.local on _http._tcp port $service_port"
fi

if [[ "$DO_ENABLE" == "1" ]]; then
    log "enabling + starting $service_name"
    systemctl enable "$service_name"
    systemctl restart "$service_name"

    # Give the unit a couple of seconds to stand up before we report on it.
    sleep 2
    if ! systemctl is-active --quiet "$service_name"; then
        warn "$service_name did NOT start cleanly. Recent log:"
        journalctl -u "$service_name" --no-pager -n 30 | sed 's/^/    /'
        die "service failed to start; fix the issue above and re-run."
    fi
fi

# ---------------------------------------------------------------------------
# Step 9 — final report.
# ---------------------------------------------------------------------------
step "Done"

# LAN-friendly URL: prefer the host's actual IP so the operator can
# point a browser at it. `hostname -I` returns space-separated v4 IPs.
primary_ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
[[ -z "$primary_ip" ]] && primary_ip="$service_host"

# When mDNS is on, the .local URL is the friendlier address — DHCP-IP
# changes don't break the bookmark. Print both so the operator has a
# fallback if their network blocks multicast (e.g. some corp Wi-Fi).
mdns_url=""
if [[ "$do_mdns" == "1" ]]; then
    mdns_url="http://$mdns_hostname.local:$service_port"
fi

cat <<EOF

  ${C_CYAN}# service status:${C_RESET}
  systemctl status $service_name --no-pager

  ${C_CYAN}# tail the live log:${C_RESET}
  journalctl -u $service_name -f

  ${C_CYAN}# smoke-test (from the Pi itself):${C_RESET}
  curl -s http://127.0.0.1:$service_port/health | jq .
EOF

if [[ -n "$mdns_url" ]]; then
    cat <<EOF

  ${C_CYAN}# from any device on your LAN (mDNS, no IP needed):${C_RESET}
  curl -s $mdns_url/health | jq .
  open $mdns_url/          # macOS / iOS Safari
  xdg-open $mdns_url/      # Linux desktop
  ${C_CYAN}# (Windows: install Bonjour Print Services or use the LAN-IP form below)${C_RESET}
EOF
fi

cat <<EOF

  ${C_CYAN}# fallback — direct LAN IP (always works):${C_RESET}
  curl -s http://$primary_ip:$service_port/health | jq .
  open http://$primary_ip:$service_port/

  ${C_CYAN}# config files (edit + 'systemctl restart $service_name'):${C_RESET}
  $ini_file
  $system_file
  $external_tools_dir/   # drop EASYAI-*.tools manifests here

EOF

if [[ "$DO_ENABLE" == "1" ]]; then
    log "$service_name is enabled (will auto-start on boot) and currently active"
fi
if [[ "$SKIP_MODEL" == "1" ]]; then
    warn "model download skipped — drop a GGUF at $model_path then 'systemctl restart $service_name'"
fi
