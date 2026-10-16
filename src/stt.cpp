#include "easyai/stt.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>

#if defined(EASYAI_HAVE_WHISPER)
#include "whisper.h"
#endif

namespace easyai {

// ---------------------------------------------------------------------------
// PCM helpers (model-free)
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint16_t kWaveFormatPcm        = 1;
constexpr std::uint16_t kWaveFormatIeeeFloat  = 3;
constexpr std::uint16_t kWaveFormatExtensible = 0xFFFE;
constexpr std::size_t   kRiffHeaderBytes      = 12;
constexpr std::size_t   kChunkHeaderBytes     = 8;
constexpr std::size_t   kFmtMinBytes          = 16;
constexpr std::size_t   kFmtExtensibleBytes   = 40;   // 16 + cbSize(2) + 22
constexpr float         kPcm16Scale           = 1.0f / 32768.0f;
constexpr int           kCentisecondsPerMs    = 10;   // whisper timestamps are in 10 ms units

std::uint16_t rd16(const unsigned char * p) { return (std::uint16_t) (p[0] | (p[1] << 8)); }
std::uint32_t rd32(const unsigned char * p) {
    return (std::uint32_t) p[0] | ((std::uint32_t) p[1] << 8) |
           ((std::uint32_t) p[2] << 16) | ((std::uint32_t) p[3] << 24);
}

std::string trim(const std::string & s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char) s[a])) ++a;
    while (b > a && std::isspace((unsigned char) s[b - 1])) --b;
    return s.substr(a, b - a);
}

}  // namespace

void resample_linear(const std::vector<float> & in, int in_rate, int out_rate, std::vector<float> & out) {
    out.clear();
    if (in.empty() || in_rate <= 0 || out_rate <= 0) return;
    if (in_rate == out_rate) { out = in; return; }
    const double step = (double) in_rate / (double) out_rate;
    const std::size_t n_out = (std::size_t) ((double) in.size() * out_rate / in_rate);
    out.resize(n_out);
    for (std::size_t i = 0; i < n_out; ++i) {
        const double pos = i * step;
        const std::size_t i0 = (std::size_t) pos;
        const std::size_t i1 = std::min(i0 + 1, in.size() - 1);
        const float frac = (float) (pos - (double) i0);
        out[i] = in[i0] + (in[i1] - in[i0]) * frac;
    }
}

bool decode_wav(const std::string & bytes, std::vector<float> & pcm, std::string & err) {
    pcm.clear();
    const auto * d = (const unsigned char *) bytes.data();
    const std::size_t n = bytes.size();
    if (n < kRiffHeaderBytes || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) {
        err = "not a RIFF/WAVE file";
        return false;
    }

    std::uint16_t format = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
    bool have_fmt = false;
    const unsigned char * data = nullptr;
    std::size_t data_len = 0;

    std::size_t off = kRiffHeaderBytes;
    while (off + kChunkHeaderBytes <= n) {
        const unsigned char * id = d + off;
        std::uint32_t len = rd32(d + off + 4);
        const std::size_t body = off + kChunkHeaderBytes;
        if (std::memcmp(id, "fmt ", 4) == 0) {
            if (len < kFmtMinBytes || body + kFmtMinBytes > n) { err = "truncated fmt chunk"; return false; }
            format   = rd16(d + body);
            channels = rd16(d + body + 2);
            rate     = rd32(d + body + 4);
            bits     = rd16(d + body + 14);
            if (format == kWaveFormatExtensible) {
                if (len < kFmtExtensibleBytes || body + kFmtExtensibleBytes > n) {
                    err = "truncated extensible fmt chunk";
                    return false;
                }
                format = rd16(d + body + 24);   // first two bytes of the SubFormat GUID
            }
            have_fmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            if (body > n) { err = "truncated data chunk"; return false; }
            data     = d + body;
            data_len = std::min<std::size_t>(len, n - body);   // tolerate streaming writers (len = 0xFFFFFFFF)
            break;
        }
        off = body + len + (len & 1);   // chunks are word-aligned
    }

    if (!have_fmt)    { err = "missing fmt chunk";  return false; }
    if (!data)        { err = "missing data chunk"; return false; }
    if (channels == 0 || rate == 0) { err = "bad fmt: zero channels or rate"; return false; }

    std::vector<float> mono;
    if (format == kWaveFormatPcm && bits == 16) {
        const std::size_t frames = data_len / (2 * channels);
        mono.resize(frames);
        for (std::size_t f = 0; f < frames; ++f) {
            float acc = 0;
            for (std::uint16_t c = 0; c < channels; ++c) {
                const std::int16_t s = (std::int16_t) rd16(data + (f * channels + c) * 2);
                acc += s * kPcm16Scale;
            }
            mono[f] = acc / channels;
        }
    } else if (format == kWaveFormatIeeeFloat && bits == 32) {
        const std::size_t frames = data_len / (4 * channels);
        mono.resize(frames);
        for (std::size_t f = 0; f < frames; ++f) {
            float acc = 0;
            for (std::uint16_t c = 0; c < channels; ++c) {
                const std::uint32_t u = rd32(data + (f * channels + c) * 4);
                float v; std::memcpy(&v, &u, 4);
                acc += v;
            }
            mono[f] = acc / channels;
        }
    } else {
        err = "unsupported WAV encoding: format " + std::to_string(format) + ", " +
              std::to_string(bits) + "-bit (need PCM16 or float32)";
        return false;
    }
    if (mono.empty()) { err = "empty data chunk"; return false; }

    resample_linear(mono, (int) rate, Stt::kSampleRate, pcm);
    return true;
}

// ---------------------------------------------------------------------------
// Stt
// ---------------------------------------------------------------------------

struct Stt::Impl {
#if defined(EASYAI_HAVE_WHISPER)
    whisper_context * ctx = nullptr;
#endif
    std::string path;
    std::string error;
    int         n_threads = 0;
};

Stt::Stt() : p_(new Impl) {}

Stt::~Stt() { unload(); }

bool Stt::available() {
#if defined(EASYAI_HAVE_WHISPER)
    return true;
#else
    return false;
#endif
}

bool Stt::is_loaded() const {
#if defined(EASYAI_HAVE_WHISPER)
    return p_->ctx != nullptr;
#else
    return false;
#endif
}

const std::string & Stt::model_path() const { return p_->path; }
const std::string & Stt::last_error() const { return p_->error; }
void Stt::threads(int n) { p_->n_threads = n; }

void Stt::unload() {
#if defined(EASYAI_HAVE_WHISPER)
    if (p_->ctx) { whisper_free(p_->ctx); p_->ctx = nullptr; }
#endif
    p_->path.clear();
}

#if defined(EASYAI_HAVE_WHISPER)
namespace {
void quiet_log(enum ggml_log_level level, const char * text, void *) {
    if (level == GGML_LOG_LEVEL_ERROR) std::fputs(text, stderr);
}
}  // namespace
#endif

bool Stt::load(const std::string & path) {
#if defined(EASYAI_HAVE_WHISPER)
    unload();
    p_->error.clear();
    if (path.empty()) { p_->error = "no STT model path"; return false; }
    whisper_log_set(quiet_log, nullptr);
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = true;
    p_->ctx = whisper_init_from_file_with_params(path.c_str(), cp);
    if (!p_->ctx) { p_->error = "whisper could not load " + path; return false; }
    p_->path = path;
    return true;
#else
    (void) path;
    p_->error = "built without whisper (EASYAI_WITH_WHISPER=OFF or ../whisper.cpp missing)";
    return false;
#endif
}

bool Stt::transcribe(const float * pcm, std::size_t n, const std::string & language, Result & out) {
    return transcribe(pcm, n, language, "", out);
}

bool Stt::transcribe(const float * pcm, std::size_t n, const std::string & language, const std::string & prompt, Result & out) {
    out = Result();
#if defined(EASYAI_HAVE_WHISPER)
    if (!p_->ctx) { p_->error = "no STT model loaded"; return false; }
    if (!pcm || n == 0) { p_->error = "empty audio"; return false; }

    whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    const bool autodetect = language.empty() || language == "auto";
    wp.language        = autodetect ? "auto" : language.c_str();
    wp.detect_language = false;
    wp.translate       = false;
    wp.no_context      = true;
    wp.initial_prompt  = prompt.empty() ? nullptr : prompt.c_str();
    wp.print_progress  = false;
    wp.print_realtime  = false;
    wp.print_timestamps = false;
    wp.print_special   = false;
    if (p_->n_threads > 0) wp.n_threads = p_->n_threads;

    const auto t0 = std::chrono::steady_clock::now();
    if (whisper_full(p_->ctx, wp, pcm, (int) n) != 0) {
        p_->error = "whisper_full failed";
        return false;
    }
    out.run_ms   = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    out.audio_ms = 1000.0 * (double) n / kSampleRate;

    const int n_seg = whisper_full_n_segments(p_->ctx);
    std::string text;
    for (int i = 0; i < n_seg; ++i) {
        Segment s;
        s.t0_ms = whisper_full_get_segment_t0(p_->ctx, i) * kCentisecondsPerMs;
        s.t1_ms = whisper_full_get_segment_t1(p_->ctx, i) * kCentisecondsPerMs;
        s.text  = trim(whisper_full_get_segment_text(p_->ctx, i));
        if (!s.text.empty()) {
            if (!text.empty()) text += ' ';
            text += s.text;
        }
        out.segments.push_back(std::move(s));
    }
    out.text = text;
    const int lang_id = whisper_full_lang_id(p_->ctx);
    out.language = lang_id >= 0 ? whisper_lang_str(lang_id) : (autodetect ? "" : language);
    return true;
#else
    (void) pcm; (void) n; (void) language; (void) prompt;
    p_->error = "built without whisper";
    return false;
#endif
}

}  // namespace easyai
