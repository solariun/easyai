# Voice transcription model — `ggml-large-v3-turbo-q5_0.bin`

The whisper model easyai-server loads for `POST /v1/audio/transcriptions`
and the chat's mic chip (`[STT] model` / `--stt-model`). The weight file
ships in this directory through **Git LFS** (574 MB; plain git blobs are
capped at 100 MB on GitHub). A clone without git-lfs holds a 130-byte
pointer file instead of the weights; resolve it with:

```bash
git lfs install && git lfs pull        # → models/stt/ggml-large-v3-turbo-q5_0.bin (real file)
scripts/fetch_stt_model.sh             # alternative: download from HuggingFace, sha256-verified
scripts/fetch_stt_model.sh small       # any other name from the table below
```

The Linux installer copies the LFS-resolved file into
`/var/lib/easyai/models/` and falls back to the HuggingFace download when
it only finds the pointer.

The Linux installer downloads the same file into `/var/lib/easyai/models`
and links it as `stt.bin`; the `/models` dashboard (Local models → Voice
transcription) can fetch any of the others.

## Provenance

| Field | Value |
| --- | --- |
| Source | https://huggingface.co/ggerganov/whisper.cpp/blob/main/ggml-large-v3-turbo-q5_0.bin |
| Direct URL | https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo-q5_0.bin |
| HF repo commit | `5359861c739e955e79d9a303bcbc70fb988958b1` (file last modified 2024-10-29) |
| Size | 574 041 195 bytes (547 MiB) |
| SHA-256 | `394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2` |
| Format | whisper.cpp ggml binary (not GGUF), 5-bit (`q5_0`) quantisation of `large-v3-turbo` |
| Upstream model | OpenAI Whisper `large-v3-turbo` (https://github.com/openai/whisper), converted by ggml-org (https://github.com/ggml-org/whisper.cpp/blob/master/models/README.md) |
| License | MIT (both the OpenAI Whisper weights and the whisper.cpp conversion) |
| Languages | multilingual (99 languages incl. pt, en, es, de, fr, it, ja, zh); auto-detect or force with `[STT] language` |
| Parameters | 809 M (4 decoder layers, 32 encoder layers, 1280-dim) |
| Memory | ≈ 0.6 GB on disk, ≈ 1 GB resident with the KV state; runs on Metal / CUDA / Vulkan through the ggml built for llama.cpp, or CPU |
| Measured | easyai `tests/test_stt.cpp` on macOS Metal: 11 s JFK sample ≈ 0.45 s (tiny) — large-v3-turbo-q5_0 is roughly 6× slower, still well under real time on a GPU |

Why this one: best accuracy per byte for pt-BR + en among the whisper.cpp
conversions; `large-v3` full (2.9 GB) gains little, `small` (466 MB) is
noticeably worse on Portuguese, `*.en` variants are English only.

## Alternatives (same repo, same license)

| Name | Size | Languages | Note |
| --- | --- | --- | --- |
| `large-v3-turbo-q5_0` | 547 MiB | multi | **this file** — recommended |
| `large-v3-turbo` | 1.5 GiB | multi | same model, f16 |
| `large-v3-q5_0` | 1.1 GiB | multi | full large-v3, 5-bit |
| `large-v3` | 2.9 GiB | multi | full large-v3, f16 |
| `medium` / `medium.en` | 1.5 GiB | multi / en | slow on CPU |
| `small` / `small.en` | 466 MiB | multi / en | good CPU balance |
| `base` / `base.en` | 142 MiB | multi / en | fast, rough on non-English |
| `tiny` / `tiny.en` | 75 MiB | multi / en | fastest; used by the unit test |

File name pattern: `ggml-<name>.bin` under
`https://huggingface.co/ggerganov/whisper.cpp/resolve/main/`.

## Enable it

```ini
[STT]
model    = /path/to/models/stt/ggml-large-v3-turbo-q5_0.bin   # or a symlink such as stt.bin
language = auto                                                # or pt, en, …
# threads = 0
```

```bash
easyai-server -m ai.gguf --stt-model models/stt/ggml-large-v3-turbo-q5_0.bin --stt-language auto
curl -F file=@clip.wav http://localhost:8080/v1/audio/transcriptions
```
