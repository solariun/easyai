#!/usr/bin/env bash
# ci_clone_llama.sh [dest]  (default ../llama.cpp)
#
# Clones llama.cpp at the commit the installer pins (`llama_ref` in
# scripts/install_easyai_server.sh), so CI and release builds compile
# exactly what an install builds. The pin lives in ONE place: a port to a
# newer llama.cpp bumps `llama_ref` and the code in the same commit
# (spec.md "llama.cpp API port + pinned sibling checkouts", rule 6).
set -euo pipefail
dest="${1:-../llama.cpp}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ref="$(sed -n 's/^llama_ref="\([0-9a-f]\{40\}\)".*/\1/p' "$here/install_easyai_server.sh" | head -1)"
if [[ -z "$ref" ]]; then
    echo "ci_clone_llama.sh: no 40-hex llama_ref=\"…\" line in install_easyai_server.sh" >&2
    exit 1
fi
git init -q "$dest"
git -C "$dest" fetch -q --depth 1 https://github.com/ggml-org/llama.cpp.git "$ref"
git -C "$dest" checkout -q FETCH_HEAD
echo "llama.cpp at $(git -C "$dest" rev-parse HEAD) (installer llama_ref)"
