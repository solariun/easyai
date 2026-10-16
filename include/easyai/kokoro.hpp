// easyai — Kokoro-82M text-to-speech in C++ (spec.md "easyai-assistant",
// rule 10a). The ONNX model runs through the onnxruntime C API and the
// grapheme-to-phoneme step through libespeak-ng, both opened with dlopen at
// load time, so libeasyai has no link-time dependency on either: a box
// without them simply has no Kokoro engine. Voices come from the
// kokoro-onnx voices-v1.0.bin (a numpy .npz of float32 [510, 1, 256]).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace easyai {

class Kokoro {
public:
    static constexpr int kSampleRate = 24000;

    struct Config {
        std::string model;         // kokoro-v1.0.onnx
        std::string voices;        // voices-v1.0.bin (npz)
        std::string onnxruntime;   // libonnxruntime shared library; empty = search the usual places
        std::string espeak_lib;    // libespeak-ng shared library; empty = search
        std::string espeak_data;   // espeak-ng-data directory; empty = search
        int         threads = 0;   // onnxruntime intra-op threads; 0 = its default
    };

    Kokoro();
    ~Kokoro();
    Kokoro(const Kokoro &)             = delete;
    Kokoro & operator=(const Kokoro &) = delete;

    bool load(const Config & cfg, std::string & err);
    bool loaded() const;
    const std::string & onnxruntime_path() const;
    const std::string & espeak_path() const;
    std::vector<std::string> voice_names() const;
    bool has_voice(const std::string & name) const;

    // voice: a Kokoro voice id (pm_alex, am_michael, …). The language for
    // espeak-ng follows the id's first letter (p pt-br, a en-us, b en-gb,
    // e es, f fr-fr, i it, h hi, j ja, z cmn). speed 0.5 … 2.0.
    bool synthesize(const std::string & text, const std::string & voice, float speed,
                    std::vector<std::int16_t> & pcm, std::string & err);

    // Exposed for tests: text → IPA phoneme string the model consumes.
    bool phonemize(const std::string & text, const std::string & lang, std::string & phonemes, std::string & err);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Model-free pieces, exposed for unit tests.
namespace kokoro {
// Kokoro voice id → espeak-ng voice name.
std::string lang_of_voice(const std::string & voice);
// Removes espeak language-switch flags like "(en)", collapses whitespace.
std::string clean_phonemes(const std::string & s);
// Splits a phoneme string into batches of at most max_len characters,
// preferring sentence marks, then clause marks, then spaces.
std::vector<std::string> split_batches(const std::string & phonemes, std::size_t max_len);
// Seconds of silence after a batch ending with a sentence / clause mark.
float pause_after(const std::string & batch, float sentence_s, float clause_s);
// numpy .npz (stored entries, float32 arrays) → name → flat floats.
bool load_npz_float32(const std::string & bytes, std::vector<std::pair<std::string, std::vector<float>>> & out, std::string & err);
}  // namespace kokoro

}  // namespace easyai
