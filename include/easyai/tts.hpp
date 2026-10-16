// easyai — text-to-speech through a child process (piper, macOS say,
// espeak-ng). No linked TTS runtime: the engine is spawned per sentence
// and its PCM is read back. Used by easyai-server (POST /v1/audio/speech)
// and by easyai-assistant as its local fallback voice.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace easyai {

class Tts {
public:
    struct Config {
        std::string engine = "auto";   // auto | piper | say | espeak | none
        std::string piper;             // piper binary path (engine piper)
        std::string voice_pt;          // piper: .onnx path; say: voice name; espeak: voice id
        std::string voice_en;
    };

    // Resolves "auto" and checks the engine's binary and voices exist.
    // Returns the engine chosen ("none" when nothing usable) and fills err.
    std::string configure(const Config & cfg, std::string & err);
    const std::string & engine() const { return engine_; }
    bool available() const { return engine_ != "none" && !engine_.empty(); }

    // voice: "pt", "en", or an engine voice name/path. Output is PCM16 mono
    // at `rate_hz` (engine dependent: piper from the model, say 22050,
    // espeak 22050).
    bool synthesize(const std::string & text, const std::string & voice,
                    std::vector<std::int16_t> & pcm, int & rate_hz, std::string & err);

private:
    std::string engine_ = "none";
    Config      cfg_;
    int         piper_rate_pt_ = 0, piper_rate_en_ = 0;
    std::string resolve_voice(const std::string & voice, int & rate_hz) const;
};

// Runs argv[0] with the given arguments, feeds `input` on stdin, collects
// stdout into `out` and the stderr tail into `err`. Returns the exit code,
// -1 when the process could not be started.
int run_capture(const std::vector<std::string> & argv, const std::string & input,
                std::string & out, std::string & err, int timeout_ms);

// PCM16 mono → RIFF/WAVE bytes.
std::string encode_wav_pcm16(const std::int16_t * pcm, std::size_t n, int rate_hz);

}  // namespace easyai
