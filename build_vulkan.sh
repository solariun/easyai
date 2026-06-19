#!/usr/bin/env bash
# =============================================================================
# build_vulkan.sh — configure + build easyai with the Vulkan backend.
#
# Linux + Windows. Vulkan is the most portable GPU backend; works on AMD
# (RDNA1/2/3), NVIDIA (open + proprietary), Intel Arc, and most Mesa-
# capable iGPUs. Pick CUDA instead if you have a CUDA-only NVIDIA box
# and want the absolute fastest path; pick Vulkan if you want one binary
# that runs on everything.
#
# Build deps (Debian / Ubuntu, install once):
#
#   sudo apt install build-essential cmake ninja-build git pkg-config \
#                    libcurl4-openssl-dev libvulkan-dev vulkan-tools \
#                    glslc glslang-tools libshaderc-dev mesa-vulkan-drivers
#
# This script ONLY configures + builds — no install step. Output lands
# under ./build-vulkan/. For a system install, use
# scripts/install_easyai_server.sh.
#
# Usage:
#   ./build_vulkan.sh                # Release, all cores
#   ./build_vulkan.sh --debug        # Debug build
#   ./build_vulkan.sh --jobs 4       # explicit -j 4
#   ./build_vulkan.sh --rebuild      # wipe build-vulkan/ first
#   ./build_vulkan.sh --build-dir build-foo
# =============================================================================

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BUILD_DIR="${BUILD_DIR:-$script_dir/build-vulkan}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
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

# Platform sanity. Vulkan-on-macOS exists via MoltenVK but llama.cpp's
# Metal backend is the right call there — fail loud rather than silently
# building something the operator didn't intend.
if [[ "$(uname -s)" == "Darwin" ]]; then
    echo "build_vulkan.sh: macOS detected — use ./build_macos.sh (Metal)" >&2
    exit 1
fi

# llama.cpp sibling check. easyai's CMakeLists.txt does
# `add_subdirectory(../llama.cpp)`, so the build expects the checkout
# right next to this one. We don't auto-clone — the platform installers
# (scripts/install_easyai_server.sh on Linux) do that, and on a hand
# build the operator picks the right fork themselves.
llama_dir="$(dirname "$script_dir")/llama.cpp"
if [[ ! -d "$llama_dir/.git" ]]; then
    cat >&2 <<EOF
build_vulkan.sh: missing sibling checkout at
   $llama_dir

easyai builds against ../llama.cpp. Clone upstream once:

   git clone https://github.com/ggml-org/llama.cpp.git "$llama_dir"

Or run the system installer (handles deps + clone + build):

   sudo ./scripts/install_easyai_server.sh
EOF
    exit 1
fi

if [[ "$REBUILD" == "1" && -d "$BUILD_DIR" ]]; then
    echo "==> --rebuild: removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

echo "==> Configuring (Vulkan, $BUILD_TYPE, $BUILD_DIR)"
cmake -S "$script_dir" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DGGML_VULKAN=ON \
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
echo "    $BUILD_DIR/easyai-server -m /path/to/model.gguf --host 0.0.0.0 --port 8080"
