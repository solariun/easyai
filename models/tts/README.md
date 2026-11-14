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
it finds pointers. Either way it verifies both files against the SHA-256
values in the table below and stops (removing the file) on a mismatch.

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

## Voices

The 54 voices of `voices-v1.0.bin`, as `easyai-server --list-voices`,
`GET /v1/audio/voices` and the dashboard's Assistant panel list them
(`easyai::kokoro_voice_catalog()`, a static table). The language tag is
what `lang` carries in the API; `m` / `f` is the gender; the grade is the
**upstream overall grade** from hexgrad's `VOICES.md` for Kokoro v1.0 (an
estimate of the quality and quantity of each voice's training data, A best,
F worst; `—` = upstream gives no grade for that language). easyai's
defaults are `pm_alex` (pt) and `am_michael` (en); `kokoro_best_voice(lang,
gender)` picks the best-graded voice of a language and gender.

| Language | Voices | id · gender · grade |
|---|---|---|
| **en-us** · American English | 20 | `af_heart` f A, `af_alloy` f C, `af_aoede` f C+, `af_bella` f A-, `af_jessica` f D, `af_kore` f C+, `af_nicole` f B-, `af_nova` f C, `af_river` f D, `af_sarah` f C+, `af_sky` f C-, `am_adam` m F+, `am_echo` m D, `am_eric` m D, `am_fenrir` m C+, `am_liam` m D, `am_michael` m C+, `am_onyx` m D, `am_puck` m C+, `am_santa` m D- |
| **en-gb** · British English | 8 | `bf_alice` f D, `bf_emma` f B-, `bf_isabella` f C, `bf_lily` f D, `bm_daniel` m D, `bm_fable` m C, `bm_george` m C, `bm_lewis` m D+ |
| **pt-br** · Brazilian Portuguese | 3 | `pf_dora` f —, `pm_alex` m —, `pm_santa` m — |
| **es** · Spanish | 3 | `ef_dora` f —, `em_alex` m —, `em_santa` m — |
| **fr** · French | 1 | `ff_siwis` f B- |
| **hi** · Hindi | 4 | `hf_alpha` f C, `hf_beta` f C, `hm_omega` m C, `hm_psi` m C |
| **it** · Italian | 2 | `if_sara` f C, `im_nicola` m C |
| **ja** · Japanese | 5 | `jf_alpha` f C+, `jf_gongitsune` f C, `jf_nezumi` f C-, `jf_tebukuro` f C, `jm_kumo` m C- |
| **zh** · Mandarin Chinese | 8 | `zf_xiaobei` f D, `zf_xiaoni` f D, `zf_xiaoxiao` f D, `zf_xiaoyi` f D, `zm_yunjian` m D, `zm_yunxi` m D, `zm_yunxia` m D, `zm_yunyang` m D |

Upstream notes that apply here: voices sound best on 100–200 tokens of
text and worse on very short utterances (the assistant speaks sentence by
sentence, so a one-word answer is the weak case); support for languages
other than English rests on espeak-ng phonemes and little training data,
which is why the pt-BR and es voices carry no grade. Per-voice SHA-256
prefixes and the data sources are in the upstream file.

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
