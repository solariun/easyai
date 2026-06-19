#!/usr/bin/env bash
# =============================================================================
# build_cpu.sh — configure + build easyai with the CPU-only backend.
#
# The universal fallback. Works on every platform we support: Linux
# (x86_64 / aarch64 — including Raspberry Pi 4 / 5), macOS (when you
# specifically want to bypass Metal for benchmarking or testing), and
# WSL. llama.cpp's CMake auto-enables the host's best SIMD path —
# NEON on ARM, AVX2/AVX512 on x86 — via -DGGML_NATIVE=ON (default ON).
#
# Pick this over build_vulkan / build_cuda / build_macos when:
#   * the host has no GPU at all (Pi 4 / 5, headless servers, VMs)
#   * you want a portable build that doesn't drag in a GPU SDK
#   * you're benchmarking the CPU floor against a GPU build to know
#     how much the offload actually buys you
#
# We deliberately set -DGGML_METAL=OFF so on macOS this is "pure CPU"
# (no half-CPU half-GPU build sneaking in). For the normal macOS
# experience use ./build_macos.sh, which lets Metal stay on.
#
# Build deps (Debian / Ubuntu / Pi OS, install once):
#
#   sudo apt install build-essential cmake ninja-build git pkg-config \
#                    libcurl4-openssl-dev
#
# This script ONLY configures + builds — no install step. Output lands
# under ./build-cpu/. For a system install, use
# scripts/install_easyai_server.sh --backend cpu (or
# scripts/install_easyai_pi.sh on a Raspberry Pi).
#
# Usage:
#   ./build_cpu.sh                   # Release, all cores
#   ./build_cpu.sh --debug           # Debug build
#   ./build_cpu.sh --jobs 4          # explicit -j 4
#   ./build_cpu.sh --rebuild         # wipe build-cpu/ first
#   ./build_cpu.sh --build-dir build-foo
# =============================================================================

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BUILD_DIR="${BUILD_DIR:-$script_dir/build-cpu}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)}"
REBUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rebuild)   REBUILD=1; shift ;;
        --debug)     BUILD_TYPE=Debug; shift ;;
        --jobs)      JOBS="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        -h|--help)
            sed -n '/^# Usage:/,/^# ===/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown arg: $1 (try --help)" >&2; exit 1 ;;
    esac
done

# Friendly nudge on macOS — build_macos.sh keeps Metal on, this one
# turns it off. We don't refuse here (CPU-only on Mac is a legitimate
# benchmark / debug case) but tell the operator they probably want the
# other script for daily use.
if [[ "$(uname -s)" == "Darwin" ]]; then
    echo "==> NOTE on macOS: this is pure CPU (Metal disabled). For the"
    echo "    normal Mac path with Metal, use ./build_macos.sh instead."
fi

# llama.cpp sibling — same convention as the other build scripts.
llama_dir="$(dirname "$script_dir")/llama.cpp"
if [[ ! -d "$llama_dir/.git" ]]; then
    cat >&2 <<EOF
build_cpu.sh: missing sibling checkout at
   $llama_dir

easyai builds against ../llama.cpp. Two ways to populate it:

   # Recommended — handles deps + clone + build + service unit:
   sudo ./scripts/install_easyai_server.sh                 # generic Linux
   sudo ./scripts/install_easyai_pi.sh                     # Raspberry Pi 4 / 5

   # Manual — pick the fork that matches your GGUF:
   git clone https://github.com/PrismML-Eng/llama.cpp.git "$llama_dir"   # for Bonsai-8B-Q1_0
   git clone https://github.com/ggml-org/llama.cpp.git "$llama_dir"      # upstream, every other quant
EOF
    exit 1
fi

# Show what fork is actually wired in — same hint as build_macos.sh,
# because Bonsai-8B-Q1_0 fails at decode on upstream regardless of
# whether you're running CPU or GPU.
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

echo "==> Configuring (CPU-only, $BUILD_TYPE, $BUILD_DIR)"
cmake -S "$script_dir" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DGGML_VULKAN=OFF \
    -DGGML_CUDA=OFF \
    -DGGML_HIP=OFF \
    -DGGML_METAL=OFF \
    -DEASYAI_BUILD_SERVICES=ON \
    -DEASYAI_WITH_CURL=ON \
    -DEASYAI_BUILD_WEBUI=ON \
    -DEASYAI_INSTALL=ON

echo "==> Building (jobs=$JOBS)"
cmake --build "$BUILD_DIR" -j "$JOBS"

echo
echo "==> Done. Binaries in $BUILD_DIR:"
ls "$BUILD_DIR" 2>/dev/null | grep -E '^easyai' | sed 's/^/    /'
echo
echo "Run one without installing, e.g.:"
echo "    $BUILD_DIR/easyai-server -m /path/to/model.gguf \\"
echo "        --ngl 0 -c 4096 --threads $(nproc 2>/dev/null || echo 4) \\"
echo "        --host 0.0.0.0 --port 8080 \\"
echo "        --download-dir /var/lib/easyai/models --data-dir /var/lib/easyai/data"
