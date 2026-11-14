// easyai — text-to-speech through a child process (piper, macOS say,
// espeak-ng). No linked TTS runtime: the engine is spawned per sentence
// and its PCM is read back. Used by easyai-server (POST /v1/audio/speech)
// and by easyai::Assistant (the voice assistant inside easyai-server) as its
// local fallback voice.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "easyai/kokoro.hpp"

namespace easyai {

class Tts {
public:
    struct Config {
        std::string engine = "auto";   // auto | kokoro | http | piper | say | espeak | none
        std::string kokoro_model;      // engine kokoro (the natural voice, in-process C++): kokoro-v1.0.onnx
        std::string kokoro_voices;     //   voices-v1.0.bin
        std::string kokoro_voice_pt;   //   Kokoro voice ids (default pm_alex / am_michael, both male)
        std::string kokoro_voice_en;
        std::string onnxruntime;       //   libonnxruntime path; empty = search
        std::string espeak_lib;        //   libespeak-ng path; empty = search
        std::string espeak_data;       //   espeak-ng-data dir; empty = search
        std::string url;               // engine http: base URL of another box speaking POST /v1/audio/speech
        std::string piper;             // piper binary path (engine piper)
        std::string voice_pt;          // piper: .onnx path; say: voice name; espeak: voice id
        std::string voice_en;
    };

    // Resolves "auto" and checks the engine's binary and voices exist.
    // Returns the engine chosen ("none" when nothing usable) and fills err.
    // auto = kokoro when model, voices, onnxruntime and espeak-ng are found,
    // else http when url answers /health, else piper, else say (macOS),
    // else none — espeak-ng (robotic) is never chosen automatically, only
    // by an explicit `engine = espeak`. With kokoro or http, the best
    // natural child-process engine stays as fallback for a synthesis that
    // fails.
    std::string configure(const Config & cfg, std::string & err);
    const std::string & engine() const { return engine_; }
    const std::string & fallback_engine() const { return fallback_; }
    bool available() const { return engine_ != "none" && !engine_.empty(); }
    // The voice ids the loaded Kokoro voices file holds (empty when the
    // engine is not kokoro). The dashboard marks the catalogue with it.
    std::vector<std::string> kokoro_voice_names() const { return kokoro_.loaded() ? kokoro_.voice_names() : std::vector<std::string>{}; }
    // Change the pt / en Kokoro voices of a loaded engine without reloading
    // the model (the dashboard's voice pickers). Empty keeps the current one;
    // an id the voices file lacks, or a non-kokoro engine, is an error.
    bool set_kokoro_voices(const std::string & voice_pt, const std::string & voice_en, std::string & err);
    const std::string & kokoro_voice_pt() const { return kokoro_voice_pt_; }
    const std::string & kokoro_voice_en() const { return kokoro_voice_en_; }

    // voice: "pt", "en", or an engine voice name/path. Output is PCM16 mono
    // at `rate_hz` (engine dependent: piper from the model, say 22050,
    // espeak 22050).
    bool synthesize(const std::string & text, const std::string & voice,
                    std::vector<std::int16_t> & pcm, int & rate_hz, std::string & err);

private:
    std::string engine_ = "none";
    std::string fallback_ = "none";    // child-process engine used when kokoro / http fail
    Config      cfg_;
    Kokoro      kokoro_;
    std::string kokoro_voice_pt_, kokoro_voice_en_;
    int         piper_rate_pt_ = 0, piper_rate_en_ = 0;
    std::string resolve_voice(const std::string & voice, int & rate_hz) const;
    std::string resolve_local(const std::string & want, std::string & err) const;
    bool synthesize_local(const std::string & engine, const std::string & text, const std::string & voice,
                          std::vector<std::int16_t> & pcm, int & rate_hz, std::string & err);
    bool synthesize_http(const std::string & text, const std::string & voice,
                         std::vector<std::int16_t> & pcm, int & rate_hz, std::string & err);
};

// RIFF/WAVE (PCM16, any channel count → first channel) → samples + rate.
bool decode_wav_pcm16(const std::string & bytes, std::vector<std::int16_t> & pcm, int & rate_hz);

// Runs argv[0] with the given arguments, feeds `input` on stdin, collects
// stdout into `out` and the stderr tail into `err`. Returns the exit code,
// -1 when the process could not be started.
int run_capture(const std::vector<std::string> & argv, const std::string & input,
                std::string & out, std::string & err, int timeout_ms);

// PCM16 mono → RIFF/WAVE bytes.
std::string encode_wav_pcm16(const std::int16_t * pcm, std::size_t n, int rate_hz);

}  // namespace easyai
