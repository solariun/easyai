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

namespace easyai {

namespace {

constexpr int         kSayRateHz        = 22050;
constexpr int         kEspeakRateHz     = 22050;
constexpr int         kDefaultPiperRate = 22050;
constexpr int         kTtsTimeoutMs     = 60000;
constexpr std::size_t kErrTailBytes     = 2000;
constexpr std::size_t kWavHeaderBytes   = 44;

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

std::string Tts::configure(const Config & cfg, std::string & err) {
    cfg_ = cfg; err.clear();
    std::string e = cfg.engine.empty() ? "auto" : cfg.engine;
    const bool piper_ok = on_path(cfg.piper) && (file_exists(cfg.voice_pt) || file_exists(cfg.voice_en));
    if (e == "auto") {
        if (piper_ok)                 e = "piper";
#if defined(__APPLE__)
        else if (on_path("say"))      e = "say";
#endif
        else if (on_path("espeak-ng")) e = "espeak";
        else e = "none";
    }
    if (e == "piper" && !piper_ok) {
        err = "piper: binary '" + cfg.piper + "' or voices (" + cfg.voice_pt + ", " + cfg.voice_en + ") not found";
        e = "none";
    } else if (e == "say" && !on_path("say")) {
        err = "say: not on PATH (macOS only)"; e = "none";
    } else if (e == "espeak" && !on_path("espeak-ng")) {
        err = "espeak-ng: not on PATH"; e = "none";
    } else if (e != "piper" && e != "say" && e != "espeak" && e != "none") {
        err = "unknown TTS engine '" + e + "' (auto|piper|say|espeak|none)"; e = "none";
    }
    if (e == "piper") {
        piper_rate_pt_ = file_exists(cfg.voice_pt) ? piper_rate(cfg.voice_pt) : 0;
        piper_rate_en_ = file_exists(cfg.voice_en) ? piper_rate(cfg.voice_en) : 0;
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
    rate_hz = engine_ == "say" ? kSayRateHz : engine_ == "espeak" ? kEspeakRateHz : kDefaultPiperRate;
    if (voice == "pt" || voice == "pt-br" || voice == "pt_BR") {
        if (engine_ == "piper") rate_hz = piper_rate_pt_ ? piper_rate_pt_ : piper_rate_en_;
        return !cfg_.voice_pt.empty() ? cfg_.voice_pt : cfg_.voice_en;
    }
    if (voice.empty() || voice == "en" || voice == "en-us" || voice == "en_US") {
        if (engine_ == "piper") rate_hz = piper_rate_en_ ? piper_rate_en_ : piper_rate_pt_;
        return !cfg_.voice_en.empty() ? cfg_.voice_en : cfg_.voice_pt;
    }
    if (engine_ == "piper") rate_hz = piper_rate(voice);
    return voice;   // engine-native voice name or path
}

bool Tts::synthesize(const std::string & text, const std::string & voice,
                     std::vector<std::int16_t> & pcm, int & rate_hz, std::string & err) {
    pcm.clear();
    if (!available()) { err = "no TTS engine"; return false; }
    if (text.empty()) { err = "empty text"; return false; }
    const std::string v = resolve_voice(voice, rate_hz);
    std::string out, e;
    int rc = -1;
    if (engine_ == "piper") {
        rc = run_capture({cfg_.piper, "--model", v, "--output-raw"}, text + "\n", out, e, kTtsTimeoutMs);
    } else if (engine_ == "espeak") {
        rc = run_capture({"espeak-ng", "-v", v, "--stdout", "--stdin"}, text, out, e, kTtsTimeoutMs);
        if (rc == 0 && out.size() > kWavHeaderBytes) out.erase(0, kWavHeaderBytes);   // espeak writes a WAV header
    } else if (engine_ == "say") {
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
    }
    if (rc != 0) { err = engine_ + " exited " + std::to_string(rc) + (e.empty() ? "" : ": " + e); return false; }
    if (out.size() < 2) { err = engine_ + " produced no audio" + (e.empty() ? "" : ": " + e); return false; }
    pcm.resize(out.size() / 2);
    std::memcpy(pcm.data(), out.data(), pcm.size() * 2);
    return true;
}

}  // namespace easyai
