// easyai — speech-to-text over whisper.cpp.
//
// One Stt object holds one whisper model. Input is always 16 kHz mono
// float PCM (kSampleRate); decode_wav() + resample_linear() turn any
// PCM16 / float32 WAV into that shape without a codec dependency.
// transcribe() runs one audio clip at a time per object — the caller
// serialises (easyai-server uses its own stt_mu, separate from the
// chat engine lock, so a transcription never waits behind a long
// generation).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace easyai {

class Stt {
public:
    static constexpr int kSampleRate = 16000;

    struct Segment {
        std::int64_t t0_ms = 0;
        std::int64_t t1_ms = 0;
        std::string  text;
    };
    struct Result {
        std::string          text;        // all segments joined, trimmed
        std::string          language;    // ISO-639-1 detected or forced
        double               audio_ms = 0;
        double               run_ms   = 0;
        std::vector<Segment> segments;
    };

    Stt();
    ~Stt();
    Stt(const Stt &)             = delete;
    Stt & operator=(const Stt &) = delete;

    // True when the library was built against whisper.cpp.
    static bool available();

    bool load(const std::string & path);
    void unload();
    bool is_loaded() const;
    const std::string & model_path() const;
    const std::string & last_error() const;

    // 0 = whisper default (min(4, hw threads)).
    void threads(int n);

    // language: "" or "auto" = detect; otherwise an ISO-639-1 code.
    // prompt: text whisper sees before the audio (OpenAI's "prompt"): names and
    // spellings it should prefer, e.g. "Hey easy AI." for the wake word.
    bool transcribe(const float * pcm, std::size_t n, const std::string & language, Result & out);
    bool transcribe(const float * pcm, std::size_t n, const std::string & language, const std::string & prompt, Result & out);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// RIFF/WAVE → 16 kHz mono float PCM in [-1, 1]. Accepts PCM16 (format 1),
// IEEE float32 (format 3) and WAVE_FORMAT_EXTENSIBLE wrapping either;
// channels are averaged, any sample rate is resampled. Returns false with
// err set for anything else (8-bit, 24-bit, µ-law, truncated chunks).
bool decode_wav(const std::string & bytes, std::vector<float> & pcm, std::string & err);

// Linear-interpolation resampler. in_rate == out_rate copies.
void resample_linear(const std::vector<float> & in, int in_rate, int out_rate, std::vector<float> & out);

}  // namespace easyai
