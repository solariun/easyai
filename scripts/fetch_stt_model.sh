#!/usr/bin/env bash
# Fetch a whisper.cpp model into models/stt/ (default: the recommended
# large-v3-turbo-q5_0) and verify it. Provenance + table: models/stt/README.md
set -euo pipefail
name="${1:-large-v3-turbo-q5_0}"
[[ "$name" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "bad model name: $name" >&2; exit 1; }
repo="ggerganov/whisper.cpp"
file="ggml-$name.bin"
dir="$(cd "$(dirname "$0")/.." && pwd)/models/stt"
dest="$dir/$file"
url="https://huggingface.co/$repo/resolve/main/$file"
case "$name" in   # known checksums (bash 3 compatible, no associative arrays)
    large-v3-turbo-q5_0) want="394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2" ;;
    *)                   want="" ;;
esac

mkdir -p "$dir"
if [[ -f "$dest" ]]; then echo "already present: $dest"; else
    echo "downloading $url"
    curl -fL --retry 3 --progress-bar -o "$dest.part" "$url"
    mv "$dest.part" "$dest"
fi
if [[ -n "$want" ]]; then
    have="$( (sha256sum "$dest" 2>/dev/null || shasum -a 256 "$dest") | awk '{print $1}')"
    [[ "$have" == "$want" ]] || { echo "SHA-256 MISMATCH for $dest: $have" >&2; exit 1; }
    echo "sha256 ok"
fi
echo "ready: $dest"
echo "enable: easyai-server --stt-model $dest   or   [STT] model = $dest"
