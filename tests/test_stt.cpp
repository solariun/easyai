// Voice transcription (spec.md "Voice transcription (STT) service").
// Model-free: WAV decoding + resampling. With EASYAI_STT_TEST_MODEL set to
// a whisper ggml model and ../whisper.cpp/samples/jfk.wav present, also
// runs one real transcription and expects the word "country".
#include "easyai/stt.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } else { std::printf("ok   %s\n", #cond); } } while (0)

static const double kPi          = 3.14159265358979323846;
static const int    kToneHz      = 1000;
static const float  kToneAmp     = 0.5f;
static const double kToneSeconds = 0.5;

static void put16(std::string & s, unsigned v) { s.push_back((char) (v & 0xFF)); s.push_back((char) ((v >> 8) & 0xFF)); }
static void put32(std::string & s, unsigned v) { put16(s, v & 0xFFFF); put16(s, (v >> 16) & 0xFFFF); }

// format: 1 = PCM16, 3 = float32, 0xFFFE = extensible wrapping `sub`.
static std::string make_wav(int rate, int channels, unsigned format, unsigned sub, const std::vector<float> & mono) {
    const bool is_float = (format == 3) || (format == 0xFFFE && sub == 3);
    const unsigned bits = is_float ? 32 : 16;
    std::string data;
    for (float v : mono) {
        for (int c = 0; c < channels; ++c) {
            if (is_float) { unsigned u; std::memcpy(&u, &v, 4); put32(data, u); }
            else          { put16(data, (unsigned) (short) std::lrint(v * 32767.0f)); }
        }
    }
    std::string fmt;
    put16(fmt, format); put16(fmt, channels); put32(fmt, rate);
    put32(fmt, rate * channels * bits / 8); put16(fmt, channels * bits / 8); put16(fmt, bits);
    if (format == 0xFFFE) {
        put16(fmt, 22); put16(fmt, bits); put32(fmt, 0);
        put16(fmt, sub); fmt.append(14, '\0');   // SubFormat GUID: first two bytes carry the format
    }
    std::string s = "RIFF"; put32(s, 4 + 8 + fmt.size() + 8 + data.size()); s += "WAVE";
    s += "fmt "; put32(s, fmt.size()); s += fmt;
    s += "data"; put32(s, data.size()); s += data;
    return s;
}

static std::vector<float> tone(int rate, double seconds) {
    std::vector<float> v((size_t) (rate * seconds));
    for (size_t i = 0; i < v.size(); ++i) v[i] = kToneAmp * (float) std::sin(2 * kPi * kToneHz * i / rate);
    return v;
}

static float peak(const std::vector<float> & v) { float m = 0; for (float x : v) m = std::max(m, std::fabs(x)); return m; }

int main() {
    using easyai::Stt;
    std::vector<float> pcm; std::string err;

    // --- 44.1 kHz stereo PCM16 -> 16 kHz mono --------------------------------
    {
        const auto src = tone(44100, kToneSeconds);
        CHECK(easyai::decode_wav(make_wav(44100, 2, 1, 0, src), pcm, err));
        CHECK(pcm.size() == (size_t) (kToneSeconds * Stt::kSampleRate));
        CHECK(std::fabs(peak(pcm) - kToneAmp) < 0.05f);
    }
    // --- float32 mono at 16 kHz: no resampling, exact length -----------------
    {
        const auto src = tone(16000, kToneSeconds);
        CHECK(easyai::decode_wav(make_wav(16000, 1, 3, 0, src), pcm, err));
        CHECK(pcm.size() == src.size());
        CHECK(std::fabs(pcm[100] - src[100]) < 1e-6f);
    }
    // --- WAVE_FORMAT_EXTENSIBLE wrapping PCM16, 48 kHz ------------------------
    {
        const auto src = tone(48000, kToneSeconds);
        CHECK(easyai::decode_wav(make_wav(48000, 1, 0xFFFE, 1, src), pcm, err));
        CHECK(pcm.size() == (size_t) (kToneSeconds * Stt::kSampleRate));
    }
    // --- rejections -----------------------------------------------------------
    CHECK(!easyai::decode_wav("not audio at all", pcm, err) && err.find("RIFF") != std::string::npos);
    {
        std::string w = make_wav(16000, 1, 1, 0, tone(16000, 0.1));
        CHECK(!easyai::decode_wav(w.substr(0, 20), pcm, err) && err.find("fmt") != std::string::npos);
        // 8-bit PCM: patch bits-per-sample in the fmt chunk (offset 12 + 8 + 14).
        std::string eight = w; eight[12 + 8 + 14] = 8; eight[12 + 8 + 15] = 0;
        CHECK(!easyai::decode_wav(eight, pcm, err) && err.find("8-bit") != std::string::npos);
        std::string nodata = w.substr(0, 12 + 8 + 16);
        CHECK(!easyai::decode_wav(nodata, pcm, err) && err.find("data") != std::string::npos);
    }
    // --- resampler keeps endpoints and length --------------------------------
    {
        std::vector<float> in = {0, 1, 2, 3, 4, 5, 6, 7}, out;
        easyai::resample_linear(in, 8, 4, out);
        CHECK(out.size() == 4 && out[0] == 0 && std::fabs(out[3] - 6) < 1e-6f);
        easyai::resample_linear(in, 8, 8, out);
        CHECK(out == in);
    }

    // --- optional real transcription -----------------------------------------
    const char * model = std::getenv("EASYAI_STT_TEST_MODEL");
    const char * wav   = std::getenv("EASYAI_STT_TEST_WAV");
    if (model && Stt::available()) {
        std::ifstream f(wav ? wav : "../whisper.cpp/samples/jfk.wav", std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        if (ss.str().empty()) {
            std::printf("skip real transcription: sample WAV not found\n");
        } else {
            Stt stt;
            CHECK(stt.load(model));
            CHECK(easyai::decode_wav(ss.str(), pcm, err));
            Stt::Result r;
            CHECK(stt.transcribe(pcm.data(), pcm.size(), "en", r));
            std::printf("transcript (%.0f ms audio, %.0f ms run, lang=%s): %s\n",
                        r.audio_ms, r.run_ms, r.language.c_str(), r.text.c_str());
            CHECK(r.text.find("country") != std::string::npos);
            CHECK(!r.segments.empty() && r.segments.front().t1_ms > r.segments.front().t0_ms);
            Stt::Result rp;   // the prompt overload: a glossary sentence must not change a clear transcript
            CHECK(stt.transcribe(pcm.data(), pcm.size(), "en", "The speaker is called John F. Kennedy.", rp));
            CHECK(rp.text.find("country") != std::string::npos);
        }
    } else {
        std::printf("skip real transcription: set EASYAI_STT_TEST_MODEL=<ggml-*.bin> (whisper %s)\n",
                    Stt::available() ? "available" : "NOT built");
    }

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
