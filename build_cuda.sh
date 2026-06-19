#!/usr/bin/env bash
# =============================================================================
# build_cuda.sh — configure + build easyai with the CUDA backend.
#
# Linux only (Windows works via MSVC + CUDA but isn't covered by this
# script). Pick CUDA over Vulkan when:
#   * you have an NVIDIA card with CC >= 6.0 (Pascal+)
#   * you want maximum throughput (CUDA paths are usually 1.3-1.8x
#     faster than Vulkan on the same hardware)
#   * you don't care about portability across GPU vendors
#
# Build deps (one-time, Debian / Ubuntu):
#
#   sudo apt install build-essential cmake ninja-build git pkg-config \
#                    libcurl4-openssl-dev
#   # CUDA toolkit (nvcc + libraries). Either:
#   sudo apt install nvidia-cuda-toolkit                 # Ubuntu's repo
#   # OR install upstream CUDA from
#   #   https://developer.nvidia.com/cuda-downloads
#   nvcc --version       # sanity check; should print Cuda 12.x
#
# This script ONLY configures + builds. No install step. Output lands
# under ./build-cuda/. For a system install, use
# scripts/install_easyai_server.sh --backend cuda.
#
# Usage:
#   ./build_cuda.sh                  # Release, all cores
#   ./build_cuda.sh --debug          # Debug build (slow)
#   ./build_cuda.sh --jobs 8         # explicit -j 8
#   ./build_cuda.sh --rebuild        # wipe build-cuda/ first
#   ./build_cuda.sh --build-dir build-foo
#   ./build_cuda.sh --arch 89        # restrict to one CC (faster build)
# =============================================================================

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BUILD_DIR="${BUILD_DIR:-$script_dir/build-cuda}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
REBUILD=0
# CUDA architectures to build kernels for. Empty = let llama.cpp pick
# its default range (currently 60;70;75;80;86;89;90 — covers most cards
# but bloats build time + binary). Set to your GPU's CC for fastest
# build (e.g. --arch 89 for RTX 4090).
CUDA_ARCH="${CUDA_ARCH:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rebuild)   REBUILD=1; shift ;;
        --debug)     BUILD_TYPE=Debug; shift ;;
        --jobs)      JOBS="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --arch)      CUDA_ARCH="$2"; shift 2 ;;
        -h|--help)
            sed -n '/^# Usage:/,/^# ===/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown arg: $1 (try --help)" >&2; exit 1 ;;
    esac
done

if [[ "$(uname -s)" == "Darwin" ]]; then
    echo "build_cuda.sh: macOS detected — no CUDA on Apple Silicon, use ./build_macos.sh" >&2
    exit 1
fi

# nvcc sanity. We don't fail hard — CMake's enable_language(CUDA) will
# give a perfectly cromulent error if nvcc isn't on PATH — but a one-
# line warning here saves the operator a longer round trip.
if ! command -v nvcc >/dev/null 2>&1; then
    echo "==> WARN nvcc not on PATH; CMake will fail at configure unless your CUDA install is in a non-default location" >&2
fi

# llama.cpp sibling — same convention as the other build scripts.
llama_dir="$(dirname "$script_dir")/llama.cpp"
if [[ ! -d "$llama_dir/.git" ]]; then
    cat >&2 <<EOF
build_cuda.sh: missing sibling checkout at
   $llama_dir

easyai builds against ../llama.cpp. Clone upstream once:

   git clone https://github.com/ggml-org/llama.cpp.git "$llama_dir"

Or run the system installer (handles deps + clone + build):

   sudo ./scripts/install_easyai_server.sh --backend cuda
EOF
    exit 1
fi

if [[ "$REBUILD" == "1" && -d "$BUILD_DIR" ]]; then
    echo "==> --rebuild: removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

cmake_args=(
    -S "$script_dir"
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DGGML_CUDA=ON
    -DEASYAI_BUILD_SERVICES=ON
    -DEASYAI_WITH_CURL=ON
    -DEASYAI_BUILD_WEBUI=ON
    -DEASYAI_INSTALL=ON
)
if [[ -n "$CUDA_ARCH" ]]; then
    # Override llama.cpp's default arch list. Format is the standard
    # CMAKE_CUDA_ARCHITECTURES — semicolon-separated integers, no dot
    # ('89' for sm_89, not '8.9'). Single value or multi: '89' or '80;86;89'.
    cmake_args+=( -DCMAKE_CUDA_ARCHITECTURES="$CUDA_ARCH" )
    echo "==> CUDA arch: $CUDA_ARCH"
fi

echo "==> Configuring (CUDA, $BUILD_TYPE, $BUILD_DIR)"
cmake "${cmake_args[@]}"

echo "==> Building (jobs=$JOBS)"
cmake --build "$BUILD_DIR" -j "$JOBS"

echo
echo "==> Done. Binaries in $BUILD_DIR:"
ls "$BUILD_DIR" 2>/dev/null | grep -E '^easyai' | sed 's/^/    /'
echo
echo "Run one without installing, e.g.:"
echo "    $BUILD_DIR/easyai-server -m /path/to/model.gguf --ngl 99 \\"
echo "        --host 0.0.0.0 --port 8080"
