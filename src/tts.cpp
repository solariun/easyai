#include "easyai/tts.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace easyai {

namespace {

constexpr int         kSayRateHz        = 22050;
constexpr int         kEspeakRateHz     = 22050;
constexpr int         kDefaultPiperRate = 22050;
constexpr int         kTtsTimeoutMs     = 60000;
constexpr std::size_t kErrTailBytes     = 2000;
constexpr std::size_t kWavHeaderBytes   = 44;
constexpr std::size_t kRiffHeaderBytes  = 12;

bool file_exists(const std::string & p) {
    struct stat st;
    return !p.empty() && ::stat(p.c_str(), &st) == 0;
}

bool on_path(const std::string & bin) {
    if (bin.find('/') != std::string::npos) return file_exists(bin);
    const char * path = std::getenv("PATH");
    if (!path) return false;
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':'))
        if (!dir.empty() && ::access((dir + "/" + bin).c_str(), X_OK) == 0) return true;
    return false;
}

// "sample_rate": 22050 from the piper sidecar <voice>.onnx.json.
int piper_rate(const std::string & onnx) {
    std::ifstream f(onnx + ".json");
    if (!f) return kDefaultPiperRate;
    std::stringstream ss; ss << f.rdbuf();
    const std::string s = ss.str();
    const auto k = s.find("\"sample_rate\"");
    if (k == std::string::npos) return kDefaultPiperRate;
    const auto c = s.find(':', k);
    if (c == std::string::npos) return kDefaultPiperRate;
    const int r = std::atoi(s.c_str() + c + 1);
    return r > 0 ? r : kDefaultPiperRate;
}

void put16(std::string & s, unsigned v) { s.push_back((char) (v & 0xFF)); s.push_back((char) ((v >> 8) & 0xFF)); }
void put32(std::string & s, unsigned v) { put16(s, v & 0xFFFF); put16(s, (v >> 16) & 0xFFFF); }

}  // namespace

std::string encode_wav_pcm16(const std::int16_t * pcm, std::size_t n, int rate_hz) {
    std::string s;
    s.reserve(kWavHeaderBytes + n * 2);
    const unsigned data_bytes = (unsigned) (n * 2);
    s += "RIFF"; put32(s, 36 + data_bytes); s += "WAVE";
    s += "fmt "; put32(s, 16); put16(s, 1); put16(s, 1); put32(s, (unsigned) rate_hz);
    put32(s, (unsigned) rate_hz * 2); put16(s, 2); put16(s, 16);
    s += "data"; put32(s, data_bytes);
    for (std::size_t i = 0; i < n; ++i) put16(s, (unsigned) (std::uint16_t) pcm[i]);
    return s;
}

int run_capture(const std::vector<std::string> & argv, const std::string & input,
                std::string & out, std::string & err, int timeout_ms) {
    out.clear(); err.clear();
    if (argv.empty()) { err = "empty argv"; return -1; }
    int in_pipe[2], out_pipe[2], err_pipe[2];
    if (::pipe(in_pipe) || ::pipe(out_pipe) || ::pipe(err_pipe)) { err = std::strerror(errno); return -1; }
    const pid_t pid = ::fork();
    if (pid < 0) { err = std::strerror(errno); return -1; }
    if (pid == 0) {
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) ::close(fd);
        std::vector<char *> av;
        for (const auto & a : argv) av.push_back(const_cast<char *>(a.c_str()));
        av.push_back(nullptr);
        ::execvp(av[0], av.data());
        std::_Exit(127);
    }
    ::close(in_pipe[0]); ::close(out_pipe[1]); ::close(err_pipe[1]);

    // Feed stdin and drain both outputs without deadlocking on full pipes.
    std::size_t in_off = 0;
    bool in_open = true;
    if (input.empty()) { ::close(in_pipe[1]); in_open = false; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool out_open = true, err_open = true;
    char buf[1 << 14];
    while (out_open || err_open || in_open) {
        struct pollfd fds[3];
        int n = 0;
        if (out_open) { fds[n].fd = out_pipe[0]; fds[n].events = POLLIN; ++n; }
        if (err_open) { fds[n].fd = err_pipe[0]; fds[n].events = POLLIN; ++n; }
        if (in_open)  { fds[n].fd = in_pipe[1];  fds[n].events = POLLOUT; ++n; }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { ::kill(pid, SIGKILL); err += "\n[timeout]"; break; }
        if (::poll(fds, n, (int) left) < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < n; ++i) {
            if (!(fds[i].revents & (POLLIN | POLLOUT | POLLHUP | POLLERR))) continue;
            if (fds[i].fd == in_pipe[1]) {
                const ssize_t w = ::write(in_pipe[1], input.data() + in_off, input.size() - in_off);
                if (w <= 0 || (in_off += (std::size_t) w) >= input.size()) { ::close(in_pipe[1]); in_open = false; }
            } else {
                const ssize_t r = ::read(fds[i].fd, buf, sizeof buf);
                if (r <= 0) {
                    ::close(fds[i].fd);
                    if (fds[i].fd == out_pipe[0]) out_open = false; else err_open = false;
                } else if (fds[i].fd == out_pipe[0]) {
                    out.append(buf, (std::size_t) r);
                } else {
                    err.append(buf, (std::size_t) r);
                    if (err.size() > kErrTailBytes) err.erase(0, err.size() - kErrTailBytes);
                }
            }
        }
    }
    if (in_open) ::close(in_pipe[1]);
    if (out_open) ::close(out_pipe[0]);
    if (err_open) ::close(err_pipe[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string Tts::resolve_local(const std::string & want, std::string & err) const {
    std::string e = want;
    const bool piper_ok = on_path(cfg_.piper) && (file_exists(cfg_.voice_pt) || file_exists(cfg_.voice_en));
    if (e == "auto") {
        if (piper_ok)                  e = "piper";
#if defined(__APPLE__)
        else if (on_path("say"))       e = "say";
#endif
        else if (on_path("espeak-ng")) e = "espeak";
        else e = "none";
    }
    if (e == "piper" && !piper_ok) {
        err = "piper: binary '" + cfg_.piper + "' or voices (" + cfg_.voice_pt + ", " + cfg_.voice_en + ") not found"; return "none";
    }
    if (e == "say" && !on_path("say")) { err = "say: not on PATH (macOS only)"; return "none"; }
    if (e == "espeak" && !on_path("espeak-ng")) { err = "espeak-ng: not on PATH"; return "none"; }
    if (e != "piper" && e != "say" && e != "espeak" && e != "none") {
        err = "unknown TTS engine '" + e + "' (auto|http|piper|say|espeak|none)"; return "none";
    }
    return e;
}

std::string Tts::configure(const Config & cfg, std::string & err) {
    cfg_ = cfg; err.clear();
    std::string e = cfg.engine.empty() ? "auto" : cfg.engine;
    fallback_ = "none";
    auto pick_fallback = [&]() {
        std::string ferr;
        fallback_ = resolve_local("auto", ferr);
        if (cfg_.piper.empty()) cfg_.piper = "piper";
        if (fallback_ == "say")    { if (cfg_.voice_pt.empty()) cfg_.voice_pt = "Eddy (Portuguese (Brazil))"; if (cfg_.voice_en.empty()) cfg_.voice_en = "Daniel"; }
        if (fallback_ == "espeak") { if (cfg_.voice_pt.empty()) cfg_.voice_pt = "pt-br"; if (cfg_.voice_en.empty()) cfg_.voice_en = "en"; }
        if (fallback_ == "piper")  { piper_rate_pt_ = file_exists(cfg_.voice_pt) ? piper_rate(cfg_.voice_pt) : 0; piper_rate_en_ = file_exists(cfg_.voice_en) ? piper_rate(cfg_.voice_en) : 0; }
    };
    if (e == "kokoro" || (e == "auto" && !cfg.kokoro_model.empty())) {
        Kokoro::Config kc;
        kc.model = cfg.kokoro_model; kc.voices = cfg.kokoro_voices; kc.onnxruntime = cfg.onnxruntime;
        kc.espeak_lib = cfg.espeak_lib; kc.espeak_data = cfg.espeak_data;
        std::string kerr;
        if (kokoro_.load(kc, kerr)) {
            engine_ = "kokoro";
            kokoro_voice_pt_ = (!cfg.kokoro_voice_pt.empty() && kokoro_.has_voice(cfg.kokoro_voice_pt)) ? cfg.kokoro_voice_pt : "pm_alex";
            kokoro_voice_en_ = (!cfg.kokoro_voice_en.empty() && kokoro_.has_voice(cfg.kokoro_voice_en)) ? cfg.kokoro_voice_en : "am_michael";
            pick_fallback();   // voice_pt / voice_en stay the child-process voices (piper .onnx, say, espeak)
            return engine_;
        }
        err = "kokoro: " + kerr;
        if (e == "kokoro") { engine_ = "none"; return engine_; }
        err += "; falling back";
    }
    if (e == "http" || (e == "auto" && !cfg.url.empty())) {
        httplib::Client c(cfg.url);
        c.set_connection_timeout(2); c.set_read_timeout(5);
        auto r = c.Get("/health");
        if (r && r->status == 200) {
            engine_ = "http";
            std::string ferr;
            fallback_ = resolve_local("auto", ferr);
            if (cfg_.piper.empty()) cfg_.piper = "piper";
            if (fallback_ == "say")    { if (cfg_.voice_pt.empty()) cfg_.voice_pt = "Eddy (Portuguese (Brazil))"; if (cfg_.voice_en.empty()) cfg_.voice_en = "Daniel"; }
            if (fallback_ == "espeak") { if (cfg_.voice_pt.empty()) cfg_.voice_pt = "pt-br"; if (cfg_.voice_en.empty()) cfg_.voice_en = "en"; }
            if (fallback_ == "piper")  { piper_rate_pt_ = file_exists(cfg_.voice_pt) ? piper_rate(cfg_.voice_pt) : 0; piper_rate_en_ = file_exists(cfg_.voice_en) ? piper_rate(cfg_.voice_en) : 0; }
            return engine_;
        }
        err = "http: " + cfg.url + "/health not answering (" + (r ? std::to_string(r->status) : httplib::to_string(r.error())) + ")";
        if (e == "http") { engine_ = "none"; return engine_; }
        err += "; falling back to a local engine";
    }
    std::string lerr;
    e = resolve_local(e == "http" ? "auto" : e, lerr);
    if (e == "none" && !lerr.empty()) err = err.empty() ? lerr : err + "; " + lerr;
    if (e == "piper") {
        piper_rate_pt_ = file_exists(cfg_.voice_pt) ? piper_rate(cfg_.voice_pt) : 0;
        piper_rate_en_ = file_exists(cfg_.voice_en) ? piper_rate(cfg_.voice_en) : 0;
    }
    if (e == "say") {
        if (cfg_.voice_pt.empty()) cfg_.voice_pt = "Eddy (Portuguese (Brazil))";
        if (cfg_.voice_en.empty()) cfg_.voice_en = "Daniel";
    }
    if (e == "espeak") {
        if (cfg_.voice_pt.empty()) cfg_.voice_pt = "pt-br";
        if (cfg_.voice_en.empty()) cfg_.voice_en = "en";
    }
    engine_ = e;
    return e;
}

std::string Tts::resolve_voice(const std::string & voice, int & rate_hz) const {
    const std::string local = (engine_ == "http" || engine_ == "kokoro") ? fallback_ : engine_;
    rate_hz = local == "say" ? kSayRateHz : local == "espeak" ? kEspeakRateHz : kDefaultPiperRate;
    if (voice == "pt" || voice == "pt-br" || voice == "pt_BR") {
        if (local == "piper") rate_hz = piper_rate_pt_ ? piper_rate_pt_ : piper_rate_en_;
        return !cfg_.voice_pt.empty() ? cfg_.voice_pt : cfg_.voice_en;
    }
    if (voice.empty() || voice == "en" || voice == "en-us" || voice == "en_US") {
        if (local == "piper") rate_hz = piper_rate_en_ ? piper_rate_en_ : piper_rate_pt_;
        return !cfg_.voice_en.empty() ? cfg_.voice_en : cfg_.voice_pt;
    }
    if (local == "piper") rate_hz = piper_rate(voice);
    return voice;   // engine-native voice name or path
}

bool Tts::synthesize_http(const std::string & text, const std::string & voice,
                          std::vector<std::int16_t> & pcm, int & rate_hz, std::string & err) {
    httplib::Client c(cfg_.url);
    c.set_connection_timeout(2); c.set_read_timeout(kTtsTimeoutMs / 1000);
    nlohmann::json body{{"model", "tts"}, {"input", text}, {"voice", voice}, {"response_format", "wav"}};
    auto r = c.Post("/v1/audio/speech", body.dump(), "application/json");
    if (!r) { err = "http: " + httplib::to_string(r.error()); return false; }
    if (r->status != 200) { err = "http: " + std::to_string(r->status) + " " + r->body.substr(0, kErrTailBytes); return false; }
    if (!decode_wav_pcm16(r->body, pcm, rate_hz)) { err = "http: helper returned something that is not PCM16 WAV"; return false; }
    return true;
}

bool Tts::synthesize_local(const std::string & engine, const std::string & text, const std::string & voice,
                           std::vector<std::int16_t> & pcm, int & rate_hz, std::string & err) {
    const std::string v = resolve_voice(voice, rate_hz);
    std::string out, e;
    int rc = -1;
    if (engine == "piper") {
        rc = run_capture({cfg_.piper, "--model", v, "--output-raw"}, text + "\n", out, e, kTtsTimeoutMs);
    } else if (engine == "espeak") {
        rc = run_capture({"espeak-ng", "-v", v, "--stdout", "--stdin"}, text, out, e, kTtsTimeoutMs);
        if (rc == 0 && out.size() > kWavHeaderBytes) out.erase(0, kWavHeaderBytes);   // espeak writes a WAV header
    } else if (engine == "say") {
        // say cannot stream to stdout; it writes a CAF file. It silently leaves a
        // pre-existing output file empty and appends ".caf" to an extension-less
        // name, so reserve a unique name, remove it, and hand say "<name>.caf".
        char tmpl[] = "/tmp/easyai-say-XXXXXX";
        const int fd = ::mkstemp(tmpl);
        if (fd < 0) { err = "mkstemp failed"; return false; }
        ::close(fd);
        ::unlink(tmpl);
        const std::string path = std::string(tmpl) + ".caf";
        rc = run_capture({"say", "-v", v, "-o", path, "--data-format=LEI16@" + std::to_string(kSayRateHz), "--file-format=caff", "-f", "-"},
                         text, out, e, kTtsTimeoutMs);
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        out = ss.str();
        ::unlink(path.c_str());
        // CAF container: the PCM sits in the 'data' chunk; find it and skip the chunk header (type 4 + size 8 + edit count 4).
        const auto k = out.find("data");
        if (k != std::string::npos) out.erase(0, k + 4 + 8 + 4); else out.clear();
    } else {
        err = "no local TTS engine"; return false;
    }
    if (rc != 0) { err = engine + " exited " + std::to_string(rc) + (e.empty() ? "" : ": " + e); return false; }
    if (out.size() < 2) { err = engine + " produced no audio" + (e.empty() ? "" : ": " + e); return false; }
    pcm.resize(out.size() / 2);
    std::memcpy(pcm.data(), out.data(), pcm.size() * 2);
    return true;
}

bool Tts::synthesize(const std::string & text, const std::string & voice,
                     std::vector<std::int16_t> & pcm, int & rate_hz, std::string & err) {
    pcm.clear();
    if (!available()) { err = "no TTS engine"; return false; }
    if (text.empty()) { err = "empty text"; return false; }
    if (engine_ == "kokoro" || engine_ == "http") {
        bool ok;
        if (engine_ == "kokoro") {
            const std::string v = (voice == "pt" || voice == "pt-br" || voice == "pt_BR") ? kokoro_voice_pt_
                                : (voice.empty() || voice == "en" || voice == "en-us" || voice == "en_US") ? kokoro_voice_en_ : voice;
            rate_hz = Kokoro::kSampleRate;
            ok = kokoro_.synthesize(text, v, 1.0f, pcm, err);
        } else {
            ok = synthesize_http(text, voice, pcm, rate_hz, err);
        }
        if (ok) return true;
        if (fallback_ == "none") return false;
        std::string ferr;
        if (synthesize_local(fallback_, text, voice, pcm, rate_hz, ferr)) {
            std::fprintf(stderr, "[easyai] tts: %s failed (%s) — spoke with %s\n", engine_.c_str(), err.c_str(), fallback_.c_str());
            err.clear();
            return true;
        }
        err += "; fallback " + fallback_ + ": " + ferr;
        return false;
    }
    return synthesize_local(engine_, text, voice, pcm, rate_hz, err);
}

bool decode_wav_pcm16(const std::string & bytes, std::vector<std::int16_t> & pcm, int & rate_hz) {
    pcm.clear(); rate_hz = 0;
    if (bytes.size() < kRiffHeaderBytes || bytes.compare(0, 4, "RIFF") != 0 || bytes.compare(8, 4, "WAVE") != 0) return false;
    const auto * d = (const unsigned char *) bytes.data();
    std::size_t off = kRiffHeaderBytes; int channels = 1, bits = 16;
    while (off + 8 <= bytes.size()) {
        const std::uint32_t len = d[off + 4] | (d[off + 5] << 8) | (d[off + 6] << 16) | ((std::uint32_t) d[off + 7] << 24);
        const std::size_t body = off + 8;
        if (bytes.compare(off, 4, "fmt ") == 0 && body + 16 <= bytes.size()) {
            channels = d[body + 2] | (d[body + 3] << 8);
            rate_hz  = (int) (d[body + 4] | (d[body + 5] << 8) | (d[body + 6] << 16) | ((std::uint32_t) d[body + 7] << 24));
            bits     = d[body + 14] | (d[body + 15] << 8);
        } else if (bytes.compare(off, 4, "data") == 0) {
            if (bits != 16 || rate_hz <= 0 || channels <= 0) return false;
            const std::size_t frames = std::min<std::size_t>(len, bytes.size() - body) / (2 * (std::size_t) channels);
            pcm.resize(frames);
            for (std::size_t i = 0; i < frames; ++i) {
                const std::size_t k = body + i * (std::size_t) channels * 2;   // first channel
                pcm[i] = (std::int16_t) (d[k] | (d[k + 1] << 8));
            }
            return true;
        }
        off = body + len + (len & 1);
    }
    return false;
}

}  // namespace easyai
