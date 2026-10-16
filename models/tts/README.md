# Natural voice model — Kokoro-82M (`kokoro-v1.0.onnx` + `voices-v1.0.bin`)

The text-to-speech model easyai-server runs in-process for `POST
/v1/audio/speech` and the assistant's voice (`[TTS] kokoro_model` /
`kokoro_voices`, engine `kokoro`, `src/kokoro.cpp`). Both files ship in
this directory through **Git LFS**; a clone without git-lfs holds pointer
files, resolve them with:

```bash
git lfs install && git lfs pull
```

The Linux installer copies the LFS-resolved files into
`/var/lib/easyai/tts/` and downloads them from the source below only when
it finds pointers.

## Provenance

| | `kokoro-v1.0.onnx` | `voices-v1.0.bin` |
|---|---|---|
| Source | https://github.com/thewh1teagle/kokoro-onnx/releases/tag/model-files-v1.0 | same release |
| Upstream model | [hexgrad/Kokoro-82M](https://huggingface.co/hexgrad/Kokoro-82M) v1.0, exported to ONNX by the kokoro-onnx project | the 54 voice style vectors of the same release, numpy `.npz` of float32 `[510, 1, 256]` |
| Size | 325,532,387 bytes (310 MiB) | 28,214,398 bytes (27 MiB) |
| SHA-256 | `7d5df8ecf7d4b1878015a32686053fd0eebe2bc377234608764cc0ef3636a6c5` | `bca610b8308e8d99f32e6fe4197e7ec01679264efed0cac9140fe9c29f1fbf7d` |
| License | Apache-2.0 (Kokoro-82M and the ONNX export) | Apache-2.0 |
| Architecture | StyleTTS2-derived, 82 M parameters, 24 kHz output | — |
| Inputs | `input_ids` int64 `[1, n ≤ 512]` (0-padded both ends), `style` float32 `[1, 256]`, `speed` float32 `[1]` | — |
| Languages | en-US, en-GB, pt-BR, es, fr, it, hi, ja, zh (phonemes from espeak-ng) | voice id prefix: `pm_`/`pf_` pt-BR, `am_`/`af_` en-US, `bm_`/`bf_` en-GB, `em`/`ef` es, `fm`/`ff` fr, `im`/`if` it, `hm`/`hf` hi, `jm`/`jf` ja, `zm`/`zf` zh |
| easyai defaults | `kokoro_voice_pt = pm_alex`, `kokoro_voice_en = am_michael` (both male); `pm_santa`, `bm_george` are the other male picks | |
| Runtime | onnxruntime ≥ 1.16 shared library + libespeak-ng, both opened at run time (`[TTS] onnxruntime`, `espeak_lib`, `espeak_data`) | |
| Measured | pt-BR 137 chars → 8.4 s of audio in 1.6 s, en 92 chars → 6.7 s in 1.4 s, Apple M-series CPU | |

## Enable

```ini
[TTS]
engine          = auto            ; kokoro when the files and libraries are found
kokoro_model    = /var/lib/easyai/tts/kokoro-v1.0.onnx
kokoro_voices   = /var/lib/easyai/tts/voices-v1.0.bin
kokoro_voice_pt = pm_alex
kokoro_voice_en = am_michael
onnxruntime     = /opt/easyai/onnxruntime/lib/libonnxruntime.so
```

```bash
easyai-server -m ai.gguf --tts-kokoro-model models/tts/kokoro-v1.0.onnx --tts-kokoro-voices models/tts/voices-v1.0.bin
curl -s localhost:8080/v1/audio/speech -H 'content-type: application/json' \
     -d '{"input":"Olá, tudo bem?","voice":"pt"}' -o ola.wav
```
