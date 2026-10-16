# easyai-assistant — voice assistant

An always-listening voice assistant that runs on a machine with a
microphone and a speaker, on Linux (ALSA) and macOS (CoreAudio), with no
third-party libraries. Say **"hey easy"** or **"oi easy"**, ask, pause,
and it answers aloud with a male voice in the language you used
(Portuguese or English). Every network call is OpenAI-shaped, so the same
binary works against easyai-server, llama-server or OpenAI with an API
key. Normative text: `spec.md` → "easyai-assistant".

```mermaid
flowchart LR
  M["mic 16 kHz"] --> V["VAD\n(energy, adaptive floor)"]
  V -->|burst| W["wake word\nlocal whisper tiny"]
  W -->|hey easy + beep| L["listen until\n1.5 s of silence"]
  L -->|WAV| S["POST /v1/audio/transcriptions"]
  S -->|text, language| C["POST /v1/chat/completions\n(stream, thinking off,\ntools run here)"]
  C -->|sentence by sentence| T["POST /v1/audio/speech\nor local say / piper"]
  T --> P["speaker\n(mic muted meanwhile)"]
```

## 1. Run it

```bash
# on the server box (installer writes /etc/easyai/easyai.ini [ASSISTANT] and a unit)
sudo systemctl status easyai-assistant
journalctl -u easyai-assistant -f

# on a Mac against a box on the LAN
easyai-assistant --endpoint https://ai-pro.local:8080 --insecure-tls --api-key "$TOKEN"

# test the chat + voice path without a microphone
easyai-assistant --endpoint http://127.0.0.1:8080 --text "que horas são?"

# one question then exit (CI / smoke test)
easyai-assistant --once
```

Startup prints the audio backend, the voice source (server endpoint or
local engine), the wake model and the tools. `--verbose` adds the VAD
levels, every transcript the wake model heard, and timings.

## 2. What happens, step by step

1. **Listening.** The mic is read in 20 ms periods. An energy VAD with an
   adaptive noise floor marks speech. A burst (speech plus 300 ms of
   pre-roll, ended by 500 ms of silence, at most 4 s) goes to the local
   wake model (`ggml-tiny.bin`, ~0.3 s on CPU).
2. **Wake word.** The transcript is normalised (accents folded,
   punctuation dropped) and matched: one of `hey hi oi ei ok ola alo`
   before the name, the name within one spelling edit. "Hey easy, what
   time is it?" is answered right away; a bare "oi easy" beeps once and
   listens.
3. **Question.** Listening ends after `pause_ms` (1500) of silence or
   `max_listen_ms` (20 000). The audio goes as WAV to
   `/v1/audio/transcriptions` (`verbose_json`, so the language comes
   back). An empty transcript gives a double beep.
4. **Answer.** `/v1/chat/completions`, streaming, with the built-in system
   prompt (short, same language, no lists, no reasoning, announce tools)
   and `chat_template_kwargs.enable_thinking=false`. Tools run inside the
   assistant exactly like in `easyai-cli`: `datetime`, `web`, and any
   `EASYAI-*.tools` manifests under `external_tools`.
5. **Voice.** Each complete sentence is sent to `/v1/audio/speech` and
   played while the next one is still being generated. The microphone is
   muted until playback ends plus 300 ms.
6. **Follow-ups.** After the answer it keeps listening for `followup_ms`
   (2 s): speak again without the name and it answers; stay quiet and a
   falling two-tone sound says the conversation is over.
7. **Memory.** The conversation keeps the last `history_turns` turns and
   restarts after `history_idle_s` seconds of silence.

## 3. Configuration — `[ASSISTANT]` in `easyai.ini`

Precedence: CLI flag > INI key > default. `--config` picks the file
(default `/etc/easyai/easyai.ini`). Every INI key has a flag with the same
name and dashes instead of underscores. Three commands tell you everything:

```bash
easyai-assistant --help              # every option: flag, INI key, default, meaning
easyai-assistant --list-options      # same, with the VALUE IN EFFECT after the INI and the flags
easyai-assistant --text "que horas são?"   # try the chat + voice path, no microphone needed
```

| INI key | flag | default | meaning |
|---|---|---|---|
| `name` | `--name` | `easy` | wake name |
| `name_aliases` | `--name-aliases` | built-in for easy | csv of what whisper hears for the name; run `--verbose` and copy the `heard:` lines |
| `greeting` | `--greeting` | `en` | `en` / `pt` / `off`: spoken once at start-up ("The easy assistant is online. Say 'hey easy'…") |
| `endpoint` | `--endpoint` | `http://127.0.0.1:8080` | OpenAI-compatible base URL |
| `api_key` | `--api-key` | | Bearer token (env `EASYAI_API_KEY` as fallback) |
| `model` | `--model` | `default` | model id in the request |
| `stt_endpoint`, `tts_endpoint` | `--stt-endpoint`, `--tts-endpoint` | = endpoint | other boxes for speech in / out |
| `language` | `--language` | `auto` | force `pt` / `en` transcription |
| `pause_ms`, `max_listen_ms` | same | 1500, 20000 | end of question |
| `followup_ms` | `--followup-ms` | 2000 | after an answer keep listening without the name; this much silence ends the conversation with a falling two-tone sound (0 = off) |
| `wake_model` | `--wake-model` | `models/stt/ggml-base.bin` | local whisper for the name only (a question said in the same breath is re-transcribed by the server's large model); `server` = send every burst to the STT endpoint |
| `mic`, `speaker` | same | `default` | ALSA device names (`plughw:1,0`). When `default` has no capture (card 0 is HDMI-only on many boxes) the assistant picks the first card that has one, logs it, and the speaker follows that card; `cat /proc/asound/pcm` shows the cards. macOS uses the system default |
| `tts` | `--tts` | `auto` | `server` / `say` / `piper` / `espeak`; auto = endpoint, else local |
| `voice_pt`, `voice_en`, `piper` | same | engine defaults | local voices |
| `tools` | `--tools` | `datetime,web` | built-in tools; empty = none |
| `external_tools` | `--external-tools` | | directory of `EASYAI-*.tools` |
| `system` | `--system` | built-in | file path or inline text |
| `history_turns`, `history_idle_s` | same | 6, 300 | conversation memory |
| `verbose`, `tls_insecure` | `--verbose`, `--insecure-tls` | off | |

Pointing it at OpenAI:

```ini
[ASSISTANT]
endpoint = https://api.openai.com
api_key  = sk-...
model    = gpt-4o-mini
tts      = auto        ; OpenAI's /v1/audio/speech answers, voices pt/en map to its defaults
```

## 4. Voices

The box's `/v1/audio/speech` (easyai-server, `[TTS]`) runs **Kokoro-82M**
in-process, in C++ (`easyai::Kokoro`): the ONNX model through
onnxruntime and the phonemes through espeak-ng, both shared libraries
opened at run time, no Python. Voices `pm_alex` for Portuguese and
`am_michael` for English, both male; `--kokoro-voice-pt pm_santa` /
`--kokoro-voice-en bm_george` at install, or `[TTS] kokoro_voice_pt`,
pick others. **piper** (`pt_BR-faber`, `en_US-ryan`) stays installed as
the server's automatic fallback. On a Mac the assistant can carry the
same voice itself with `[ASSISTANT] kokoro_model` / `kokoro_voices`
(brew `espeak-ng` and an onnxruntime dylib in `/opt/homebrew/lib` or
`EASYAI_ONNXRUNTIME`), else `say` (Eddy pt-BR, Daniel en-GB), else
`espeak-ng`.

```bash
curl -LO https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/kokoro-v1.0.onnx
curl -LO https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/voices-v1.0.bin
easyai-server ... --tts-kokoro-model kokoro-v1.0.onnx --tts-kokoro-voices voices-v1.0.bin
```

## 5. Limits

No barge-in while it speaks, no speaker identification, energy VAD (a
loud TV costs a tiny-whisper run per burst), languages other than pt/en
are answered in the `en` voice.
