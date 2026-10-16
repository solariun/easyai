#!/usr/bin/env python3
"""easyai TTS helper: Kokoro-82M behind the OpenAI speech shape.

Loads the model once and serves

    POST /v1/audio/speech   {"input": "...", "voice": "pt"|"en"|<kokoro voice>, "speed": 1.0}
                            -> audio/wav, PCM16 mono 24 kHz
    GET  /health            -> {"ok": true, "engine": "kokoro", "voices": {...}}

easyai-server's [TTS] url points here (engine = http); piper stays as its
fallback. Standard library only, plus kokoro-onnx (onnxruntime) from the
venv the installer creates. Voices: pm_alex / pm_santa (pt-BR male),
am_michael / bm_george (en male); any Kokoro voice id is accepted as-is.
"""
import argparse
import json
import struct
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_INPUT_CHARS = 4000
DEFAULT_VOICES = {"pt": "pm_alex", "en": "am_michael"}
LANG_OF_PREFIX = {"p": "pt-br", "a": "en-us", "b": "en-gb", "e": "es", "f": "fr-fr", "i": "it", "h": "hi", "j": "ja", "z": "cmn"}


def wav_bytes(samples, rate):
    pcm = struct.pack("<%dh" % len(samples), *[max(-32768, min(32767, int(s * 32767))) for s in samples])
    header = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE" + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", len(pcm))
    return header + pcm


# espeak-ng does the grapheme→phoneme step for pt-BR (and en fallback). The
# espeak-ng bundled in the espeakng-loader wheel has a build-machine data
# path compiled in and ignores the one passed at init (seen on macOS arm64,
# 0.2.4), so the system espeak-ng is preferred when present.
ESPEAK_CANDIDATES = [
    ("/usr/lib/x86_64-linux-gnu/libespeak-ng.so.1", "/usr/lib/x86_64-linux-gnu/espeak-ng-data"),
    ("/usr/lib/aarch64-linux-gnu/libespeak-ng.so.1", "/usr/lib/aarch64-linux-gnu/espeak-ng-data"),
    ("/usr/lib/x86_64-linux-gnu/libespeak-ng.so.1", "/usr/share/espeak-ng-data"),
    ("/usr/lib/aarch64-linux-gnu/libespeak-ng.so.1", "/usr/share/espeak-ng-data"),
    ("/opt/homebrew/lib/libespeak-ng.dylib", "/opt/homebrew/share/espeak-ng-data"),
    ("/opt/Homebrew/lib/libespeak-ng.dylib", "/opt/Homebrew/share/espeak-ng-data"),
    ("/usr/local/lib/libespeak-ng.dylib", "/usr/local/share/espeak-ng-data"),
]


def find_espeak(lib_arg, data_arg):
    import glob
    import os
    if lib_arg and data_arg:
        return lib_arg, data_arg
    env = os.environ.get("PHONEMIZER_ESPEAK_LIBRARY"), os.environ.get("ESPEAK_DATA_PATH")
    if env[0] and env[1]:
        return env
    cands = list(ESPEAK_CANDIDATES)
    for cellar in ("/opt/homebrew/Cellar/espeak-ng/*", "/opt/Homebrew/Cellar/espeak-ng/*"):
        for d in sorted(glob.glob(cellar), reverse=True):
            cands.append((d + "/lib/libespeak-ng.dylib", d + "/share/espeak-ng-data"))
    for lib, data in cands:
        if os.path.isfile(lib) and os.path.isfile(data + "/phontab"):
            return lib, data
    return None, None


class Engine:
    def __init__(self, model, voices_bin, voice_map, espeak_lib=None, espeak_data=None):
        from kokoro_onnx import Kokoro  # noqa: import here so --help works without the venv
        from kokoro_onnx.config import EspeakConfig
        lib, data = find_espeak(espeak_lib, espeak_data)
        cfg = EspeakConfig(lib_path=lib, data_path=data) if lib else None
        print(f"[easyai-tts-kokoro] espeak-ng: {lib or 'bundled wheel (may fail for pt-BR on macOS)'}", file=sys.stderr, flush=True)
        t0 = time.time()
        self.k = Kokoro(model, voices_bin, espeak_config=cfg)
        self.voice_map = voice_map
        print(f"[easyai-tts-kokoro] model loaded in {time.time() - t0:.1f}s: {model}", file=sys.stderr, flush=True)

    def speak(self, text, voice, speed):
        voice_id = self.voice_map.get(voice, voice)
        lang = LANG_OF_PREFIX.get(voice_id[:1], "en-us")
        samples, rate = self.k.create(text, voice=voice_id, speed=speed, lang=lang)
        return wav_bytes(samples, rate), rate, voice_id


class Handler(BaseHTTPRequestHandler):
    engine = None

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            return self._json(200, {"ok": True, "engine": "kokoro", "voices": self.engine.voice_map})
        self._json(404, {"error": {"message": "not found"}})

    def do_POST(self):
        if self.path not in ("/v1/audio/speech", "/audio/speech"):
            return self._json(404, {"error": {"message": "not found"}})
        try:
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))) or b"{}")
        except Exception:
            return self._json(400, {"error": {"message": "invalid JSON", "type": "invalid_request_error"}})
        text = (body.get("input") or "").strip()
        if not text:
            return self._json(400, {"error": {"message": "'input' is required", "type": "invalid_request_error"}})
        if len(text) > MAX_INPUT_CHARS:
            return self._json(400, {"error": {"message": "'input' too long", "type": "invalid_request_error"}})
        if body.get("response_format", "wav") != "wav":
            return self._json(415, {"error": {"message": "wav only", "type": "unsupported_media_type"}})
        try:
            t0 = time.time()
            wav, rate, voice_id = self.engine.speak(text, body.get("voice", "en"), float(body.get("speed", 1.0)))
        except Exception as e:  # model or phonemizer failure
            return self._json(500, {"error": {"message": f"kokoro: {e}", "type": "internal_error"}})
        print(f"[easyai-tts-kokoro] {len(text)} chars -> {(len(wav) - 44) / 2 / rate:.1f}s audio in {1000 * (time.time() - t0):.0f} ms (voice {voice_id})", file=sys.stderr, flush=True)
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(wav)))
        self.end_headers()
        self.wfile.write(wav)

    def log_message(self, *_):  # quiet: one line per synthesis above is enough
        pass


def main():
    ap = argparse.ArgumentParser(description="Kokoro TTS behind POST /v1/audio/speech")
    ap.add_argument("--model", required=True, help="kokoro-v1.0.onnx")
    ap.add_argument("--voices", required=True, help="voices-v1.0.bin")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8111)
    ap.add_argument("--voice-pt", default=DEFAULT_VOICES["pt"])
    ap.add_argument("--voice-en", default=DEFAULT_VOICES["en"])
    ap.add_argument("--espeak-lib", default=None, help="libespeak-ng shared library (auto: system, then brew Cellar)")
    ap.add_argument("--espeak-data", default=None, help="espeak-ng-data directory")
    a = ap.parse_args()
    Handler.engine = Engine(a.model, a.voices, {"pt": a.voice_pt, "en": a.voice_en}, a.espeak_lib, a.espeak_data)
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    print(f"[easyai-tts-kokoro] listening on http://{a.host}:{a.port}/v1/audio/speech (pt={a.voice_pt}, en={a.voice_en})", file=sys.stderr, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
