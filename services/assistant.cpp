// easyai-assistant — always-listening voice assistant (spec.md
// "easyai-assistant — voice assistant service").
//
//   mic → VAD → wake word (local whisper) → listen until a pause →
//   POST /v1/audio/transcriptions → POST /v1/chat/completions (stream,
//   tools run here) → POST /v1/audio/speech per sentence → speaker
//
// Every network call is OpenAI-shaped, so [ASSISTANT] endpoint can be
// easyai-server, llama-server or OpenAI with an API key.
#include "easyai/assistant_core.hpp"
#include "easyai/audio_io.hpp"
#include "easyai/builtin_tools.hpp"
#include "easyai/client.hpp"
#include "easyai/config.hpp"
#include "easyai/external_tools.hpp"
#include "easyai/stt.hpp"
#include "easyai/tts.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace easyai::assistant;

namespace {

constexpr int kUnmuteMs        = 300;     // after the voice stops, before the mic counts again
constexpr int kRetryS          = 10;      // backoff while the endpoint is down
constexpr int kHttpTimeoutS    = 120;
constexpr int kDefaultPauseMs  = 1500;
constexpr int kDefaultMaxListenMs = 20000;
constexpr int kDefaultHistoryTurns = 6;
constexpr int kDefaultHistoryIdleS = 300;
constexpr int kDefaultToolHops = 6;
constexpr int kMaxAnswerTokens = 400;     // spoken answers are short; a runaway model must not talk for minutes
constexpr const char * kDefaultConfig   = "/etc/easyai/easyai.ini";
constexpr const char * kDefaultEndpoint = "http://127.0.0.1:8080";
constexpr const char * kIniSection      = "ASSISTANT";

const char * kAssistantSystem =
    "You are %s, a voice assistant. Your answers are spoken aloud by a text-to-speech engine, so:\n"
    "- Answer in at most three short sentences unless the user explicitly asks for detail.\n"
    "- Be direct: no preamble, no restating the question, no closing offers.\n"
    "- ALWAYS answer in the same language the user spoke (Portuguese or English).\n"
    "- Plain spoken prose only: no lists, no markdown, no code, no URLs, no emojis. Spell numbers and units the way people say them.\n"
    "- Do not think out loud and do not explain your reasoning.\n"
    "- Before calling any tool, say one short sentence naming what you are about to use, in the user's language "
    "(for example \"Vou consultar a ferramenta de data e hora.\" or \"Let me check the web.\"), then call it.\n"
    "- If the question is unclear, ask one short question back.\n";

struct Args {
    std::string config = kDefaultConfig;
    std::string name = "easy";
    std::string endpoint = kDefaultEndpoint, api_key, model = "default";
    std::string stt_endpoint, tts_endpoint;
    std::string language = "auto";
    int         pause_ms = kDefaultPauseMs, max_listen_ms = kDefaultMaxListenMs;
    std::string wake_model = "models/stt/ggml-tiny.bin";
    std::string mic = "default", speaker = "default";
    std::string tts = "auto", voice_pt, voice_en, piper = "piper";
    std::string kokoro_model, kokoro_voices;   // local Kokoro voice when the endpoint has none
    std::string tools = "datetime,web", external_tools;
    std::string system;                     // file path or inline text
    int         history_turns = kDefaultHistoryTurns, history_idle_s = kDefaultHistoryIdleS;
    bool        verbose = false, once = false, tls_insecure = false;
    std::string text;                       // --text: skip the microphone
};

struct Key { const char * flag; const char * ini; enum { S, I, B } type; void * dst; const char * help; };

std::vector<Key> keys(Args & a) {
    return {
        {"--config",         nullptr,          Key::S, &a.config,         "INI file (default /etc/easyai/easyai.ini), section [ASSISTANT]"},
        {"--name",           "name",           Key::S, &a.name,           "wake name: hey <name> / oi <name> (default easy)"},
        {"--endpoint",       "endpoint",       Key::S, &a.endpoint,       "OpenAI-compatible base URL (default http://127.0.0.1:8080)"},
        {"--api-key",        "api_key",        Key::S, &a.api_key,        "Bearer token for the endpoint(s); env EASYAI_API_KEY as fallback"},
        {"--model",          "model",          Key::S, &a.model,          "model id sent in the request (default: default)"},
        {"--stt-endpoint",   "stt_endpoint",   Key::S, &a.stt_endpoint,   "base URL for /v1/audio/transcriptions (default: endpoint)"},
        {"--tts-endpoint",   "tts_endpoint",   Key::S, &a.tts_endpoint,   "base URL for /v1/audio/speech (default: endpoint)"},
        {"--language",       "language",       Key::S, &a.language,       "auto (default) | pt | en — forced transcription language"},
        {"--pause-ms",       "pause_ms",       Key::I, &a.pause_ms,       "silence that ends a question (default 1500)"},
        {"--max-listen-ms",  "max_listen_ms",  Key::I, &a.max_listen_ms,  "longest question (default 20000)"},
        {"--wake-model",     "wake_model",     Key::S, &a.wake_model,     "local whisper ggml for the wake word, or 'server' (default models/stt/ggml-tiny.bin)"},
        {"--mic",            "mic",            Key::S, &a.mic,            "capture device (ALSA name; default)"},
        {"--speaker",        "speaker",        Key::S, &a.speaker,        "playback device (ALSA name; default)"},
        {"--tts",            "tts",            Key::S, &a.tts,            "auto | server | kokoro | say | piper | espeak (default auto)"},
        {"--voice-pt",       "voice_pt",       Key::S, &a.voice_pt,       "local engine voice for pt (piper .onnx / say / espeak)"},
        {"--voice-en",       "voice_en",       Key::S, &a.voice_en,       "local engine voice for en"},
        {"--piper",          "piper",          Key::S, &a.piper,          "piper binary for the local fallback"},
        {"--kokoro-model",   "kokoro_model",   Key::S, &a.kokoro_model,   "local Kokoro-82M .onnx (natural voice without a server voice)"},
        {"--kokoro-voices",  "kokoro_voices",  Key::S, &a.kokoro_voices,  "local Kokoro voices-v1.0.bin"},
        {"--tools",          "tools",          Key::S, &a.tools,          "csv of built-in tools: datetime,web (default) — empty = none"},
        {"--external-tools", "external_tools", Key::S, &a.external_tools, "directory of EASYAI-*.tools manifests (MCP-style external tools)"},
        {"--system",         "system",         Key::S, &a.system,         "system prompt: a file path or inline text (default: built-in)"},
        {"--history-turns",  "history_turns",  Key::I, &a.history_turns,  "turns kept before the conversation restarts (default 6)"},
        {"--history-idle-s", "history_idle_s", Key::I, &a.history_idle_s, "idle seconds after which the conversation restarts (default 300)"},
        {"--verbose",        "verbose",        Key::B, &a.verbose,        "log VAD levels, transcripts and timings"},
        {"--insecure-tls",   "tls_insecure",   Key::B, &a.tls_insecure,   "skip certificate verification (self-signed server)"},
        {"--once",           nullptr,          Key::B, &a.once,           "handle one question and exit"},
        {"--text",           nullptr,          Key::S, &a.text,           "answer this text instead of listening (test mode)"},
    };
}

void set_key(const Key & k, const std::string & v) {
    if (k.type == Key::S)      *static_cast<std::string *>(k.dst) = v;
    else if (k.type == Key::I) *static_cast<int *>(k.dst) = std::atoi(v.c_str());
    else                       *static_cast<bool *>(k.dst) = !(v == "0" || v == "false" || v == "off" || v == "no");
}

void usage(Args & a) {
    std::fprintf(stderr, "Usage: easyai-assistant [options]\n\nVoice assistant: listens for \"hey %s\" / \"oi %s\", answers aloud.\n"
                         "Precedence: CLI flag > [ASSISTANT] key in --config > default.\n\n", a.name.c_str(), a.name.c_str());
    for (const auto & k : keys(a))
        std::fprintf(stderr, "  %-18s %-16s %s\n", k.flag, k.ini ? k.ini : "", k.help);
}

bool parse_args(int argc, char ** argv, Args & a) {
    // --config first so the INI can be read before the other flags override it.
    for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], "--config") == 0) a.config = argv[i + 1];
    std::string err;
    auto ini = easyai::config::load_ini_file(a.config, err);
    if (!err.empty()) std::fprintf(stderr, "[easyai-assistant] %s: %s\n", a.config.c_str(), err.c_str());
    for (const auto & k : keys(a)) {
        if (!k.ini) continue;
        const std::string v = ini.get(kIniSection, k.ini);
        if (!v.empty()) set_key(k, v);
    }
    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        if (f == "-h" || f == "--help") { usage(a); return false; }
        bool found = false;
        for (const auto & k : keys(a)) {
            if (f != k.flag) continue;
            found = true;
            if (k.type == Key::B) { set_key(k, "1"); break; }
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", k.flag); return false; }
            set_key(k, argv[++i]);
            break;
        }
        if (!found) { std::fprintf(stderr, "unknown flag %s\n", f.c_str()); usage(a); return false; }
    }
    if (a.api_key.empty()) { const char * k = std::getenv("EASYAI_API_KEY"); if (k && *k) a.api_key = k; }   // same env the server unit uses
    if (a.stt_endpoint.empty()) a.stt_endpoint = a.endpoint;
    if (a.tts_endpoint.empty()) a.tts_endpoint = a.endpoint;
    return true;
}

// ---------------------------------------------------------------------------
// HTTP helpers (OpenAI shapes)
// ---------------------------------------------------------------------------

std::unique_ptr<httplib::Client> make_http(const std::string & base, const Args & a) {
    auto c = std::make_unique<httplib::Client>(base);
    c->set_connection_timeout(10);
    c->set_read_timeout(kHttpTimeoutS);
    c->set_follow_location(true);
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    if (a.tls_insecure) c->enable_server_certificate_verification(false);
#endif
    if (!a.api_key.empty()) c->set_default_headers({{"Authorization", "Bearer " + a.api_key}});
    return c;
}

// ---------------------------------------------------------------------------
// Speaker: a queue of sentences spoken in order on its own thread, so the
// chat stream never waits for the voice. speaking() mutes the microphone.
// ---------------------------------------------------------------------------

class Speaker {
public:
    Speaker(const Args & a) : a_(a) {
        if (a.tts != "server") {
            easyai::Tts::Config c; c.engine = (a.tts == "auto") ? "auto" : a.tts; c.piper = a.piper; c.voice_pt = a.voice_pt; c.voice_en = a.voice_en;
            c.kokoro_model = a.kokoro_model; c.kokoro_voices = a.kokoro_voices;
            std::string err;
            local_.configure(c, err);
            local_err_ = err;
        }
        if (a.tts == "auto" || a.tts == "server") {
            auto c = make_http(a.tts_endpoint, a);
            auto r = c->Post("/v1/audio/speech", R"({"input":"ok","voice":"en","response_format":"wav"})", "application/json");
            use_server_ = r && r->status == 200;
            if (!use_server_ && a.tts == "server")
                std::fprintf(stderr, "[easyai-assistant] tts: %s/v1/audio/speech not available (%s)\n", a.tts_endpoint.c_str(),
                             r ? std::to_string(r->status).c_str() : httplib::to_string(r.error()).c_str());
        }
        std::fprintf(stderr, "[easyai-assistant] voice: %s\n", describe().c_str());
        worker_ = std::thread([this] { run(); });
    }
    ~Speaker() {
        { std::lock_guard<std::mutex> lk(mu_); stop_ = true; cv_.notify_all(); }
        worker_.join();
    }
    std::string describe() const {
        if (use_server_) return "server " + a_.tts_endpoint + "/v1/audio/speech";
        if (local_.available()) return "local " + local_.engine();
        return "NONE (" + local_err_ + ") — answers are printed only";
    }
    bool available() const { return use_server_ || local_.available(); }
    void speak(const std::string & text, const std::string & lang) {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back({text, lang});
        speaking_ = true;
        cv_.notify_all();
    }
    void play_pcm(const std::vector<std::int16_t> & pcm, int rate) {   // beeps
        std::lock_guard<std::mutex> lk(play_mu_);
        std::string err; easyai::audio::Playback pb;
        if (!pb.play(a_.speaker, pcm.data(), pcm.size(), rate, err)) std::fprintf(stderr, "[easyai-assistant] playback: %s\n", err.c_str());
    }
    void wait_idle() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return queue_.empty() && !busy_; });
        speaking_ = false;
    }
    bool speaking() const { return speaking_; }

private:
    struct Item { std::string text, lang; };
    void run() {
        for (;;) {
            Item it;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
                if (stop_) return;
                it = queue_.front(); queue_.pop_front();
                busy_ = true;
            }
            say(it);
            { std::lock_guard<std::mutex> lk(mu_); busy_ = false; cv_.notify_all(); }
        }
    }
    void say(const Item & it) {
        const std::string voice = it.lang.rfind("pt", 0) == 0 ? "pt" : "en";
        std::vector<std::int16_t> pcm; int rate = 0; std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = false;
        if (use_server_) {
            auto c = make_http(a_.tts_endpoint, a_);
            nlohmann::json body{{"model", "tts"}, {"input", it.text}, {"voice", voice}, {"response_format", "wav"}};
            auto r = c->Post("/v1/audio/speech", body.dump(), "application/json");
            if (r && r->status == 200) ok = easyai::decode_wav_pcm16(r->body, pcm, rate);
            else err = r ? "HTTP " + std::to_string(r->status) : httplib::to_string(r.error());
        }
        if (!ok && local_.available()) ok = local_.synthesize(it.text, voice, pcm, rate, err);
        if (!ok) { std::fprintf(stderr, "[easyai-assistant] tts failed: %s\n", err.c_str()); return; }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (a_.verbose) std::fprintf(stderr, "[easyai-assistant] tts %.0f ms, %.1f s audio: %s\n", ms, (double) pcm.size() / rate, it.text.c_str());
        play_pcm(pcm, rate);
    }

    const Args &            a_;
    easyai::Tts             local_;
    std::string             local_err_;
    bool                    use_server_ = false;
    std::thread             worker_;
    std::mutex              mu_, play_mu_;
    std::condition_variable cv_;
    std::deque<Item>        queue_;
    bool                    stop_ = false, busy_ = false;
    std::atomic<bool>       speaking_{false};
};

// ---------------------------------------------------------------------------
// The assistant
// ---------------------------------------------------------------------------

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

std::string read_file(const std::string & p) {
    std::ifstream f(p); if (!f) return "";
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

std::string unavailable_phrase(const std::string & lang) {
    return lang.rfind("pt", 0) == 0 ? "O servidor está indisponível no momento." : "The server is unavailable right now.";
}

class Assistant {
public:
    Assistant(Args & a) : a_(a), speaker_(a) {
        std::string sys = a.system;
        if (!sys.empty() && sys.find('\n') == std::string::npos && std::ifstream(sys).good()) sys = read_file(sys);
        if (sys.empty()) {
            char buf[4096];
            std::snprintf(buf, sizeof buf, kAssistantSystem, a.name.c_str());
            sys = buf;
        }
        cli_.endpoint(a.endpoint).api_key(a.api_key).model(a.model).system(sys)
            .max_tool_hops(kDefaultToolHops).max_tokens(kMaxAnswerTokens).verbose(false)
            .reasoning_effort("low")
            .extra_body_json(R"({"chat_template_kwargs":{"enable_thinking":false}})");
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
        cli_.tls_insecure(a.tls_insecure);
#endif
        cli_.on_token([this](const std::string & piece) {
            std::fputs(piece.c_str(), stdout); std::fflush(stdout);
            chunker_.push(piece);
            std::string s;
            while (chunker_.pop(s)) speaker_.speak(s, lang_);
        });
        cli_.on_tool([this](const easyai::ToolCall & c, const easyai::ToolResult &) {
            std::fprintf(stderr, "\n[easyai-assistant] tool: %s\n", c.name.c_str());
        });
        register_tools();
    }

    void register_tools() {
        std::stringstream ss(a_.tools); std::string t;
        while (std::getline(ss, t, ',')) {
            t.erase(0, t.find_first_not_of(' ')); t.erase(t.find_last_not_of(' ') + 1);
            if (t == "datetime") cli_.add_tool(easyai::tools::datetime());
            else if (t == "web") cli_.add_tool(easyai::tools::web(false));
            else if (!t.empty()) std::fprintf(stderr, "[easyai-assistant] unknown built-in tool '%s' (datetime, web)\n", t.c_str());
        }
        if (!a_.external_tools.empty()) {
            std::vector<std::string> reserved;
            for (const auto & x : cli_.tools()) reserved.push_back(x.name);
            auto loaded = easyai::load_external_tools_from_dir(a_.external_tools, reserved);
            for (const auto & e : loaded.errors) std::fprintf(stderr, "[easyai-assistant] external-tools: %s\n", e.c_str());
            for (auto & x : loaded.tools) cli_.add_tool(std::move(x));
        }
        std::string names;
        for (const auto & x : cli_.tools()) names += (names.empty() ? "" : ", ") + x.name;
        std::fprintf(stderr, "[easyai-assistant] tools: %s\n", names.empty() ? "(none)" : names.c_str());
    }

    bool open_audio() {
        std::string err;
        if (!cap_.open(a_.mic, kSampleRate, kPeriodMs, err)) {
            std::fprintf(stderr, "[easyai-assistant] microphone: %s\n", err.c_str());
            return false;
        }
        std::fprintf(stderr, "[easyai-assistant] microphone: %s (%s, %d Hz, %d ms periods)\n",
                     a_.mic.c_str(), easyai::audio::backend_name().c_str(), kSampleRate, kPeriodMs);
        if (a_.wake_model != "server") {
            if (!easyai::Stt::available()) std::fprintf(stderr, "[easyai-assistant] wake model: built without whisper — using the server for every burst\n");
            else if (!wake_.load(a_.wake_model)) std::fprintf(stderr, "[easyai-assistant] wake model: %s — using the server for every burst\n", wake_.last_error().c_str());
            else std::fprintf(stderr, "[easyai-assistant] wake model: %s\n", a_.wake_model.c_str());
        }
        return true;
    }

    // One burst of audio → text (local wake model when loaded, else the STT endpoint).
    bool transcribe(const std::vector<std::int16_t> & pcm, bool local_ok, std::string & text, std::string & lang) {
        text.clear();
        if (local_ok && wake_.is_loaded()) {
            std::vector<float> f(pcm.size());
            for (std::size_t i = 0; i < pcm.size(); ++i) f[i] = pcm[i] / 32768.0f;
            easyai::Stt::Result r;
            if (!wake_.transcribe(f.data(), f.size(), a_.language, r)) return false;
            text = r.text; lang = r.language;
            return true;
        }
        auto c = make_http(a_.stt_endpoint, a_);
        httplib::UploadFormDataItems items = {
            {"file", easyai::encode_wav_pcm16(pcm.data(), pcm.size(), kSampleRate), "speech.wav", "audio/wav"},
            {"model", "whisper-1", "", ""},
            {"response_format", "verbose_json", "", ""},
        };
        if (a_.language != "auto") items.push_back({"language", a_.language, "", ""});
        auto r = c->Post("/v1/audio/transcriptions", items);
        if (!r || r->status != 200) {
            std::fprintf(stderr, "[easyai-assistant] stt: %s\n", r ? ("HTTP " + std::to_string(r->status) + " " + r->body).c_str() : httplib::to_string(r.error()).c_str());
            return false;
        }
        try {
            auto j = nlohmann::json::parse(r->body);
            text = j.value("text", ""); lang = j.value("language", "");
        } catch (...) { return false; }
        return true;
    }

    void answer(const std::string & question, const std::string & lang) {
        lang_ = lang.empty() ? guess_language(question) : lang;
        const auto now = std::chrono::steady_clock::now();
        if (turns_ > 0 && (turns_ >= a_.history_turns ||
                           std::chrono::duration_cast<std::chrono::seconds>(now - last_turn_).count() >= a_.history_idle_s)) {
            cli_.clear_history(); turns_ = 0;
        }
        std::fprintf(stderr, "[easyai-assistant] Q (%s): %s\nA: ", lang_.c_str(), question.c_str());
        chunker_.clear();
        const auto t0 = std::chrono::steady_clock::now();
        std::string reply = cli_.chat(question);
        const std::string rest = chunker_.flush();
        if (!rest.empty()) speaker_.speak(rest, lang_);
        std::fputs("\n", stdout);
        if (reply.empty()) {
            const std::string e = cli_.last_error();
            std::fprintf(stderr, "[easyai-assistant] chat failed: %s\n", e.c_str());
            if (!server_down_) { speaker_.speak(unavailable_phrase(lang_), lang_); server_down_ = true; }
        } else {
            server_down_ = false;
            ++turns_; last_turn_ = std::chrono::steady_clock::now();
        }
        speaker_.wait_idle();
        if (a_.verbose) std::fprintf(stderr, "[easyai-assistant] turn %.0f ms\n",
                                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }

    void beep(int n) { speaker_.play_pcm(make_beep(kSampleRate, n), kSampleRate); }

    int run() {
        if (!a_.text.empty()) { answer(a_.text, a_.language == "auto" ? "" : a_.language); return 0; }
        if (!open_audio()) return 2;
        std::fprintf(stderr, "[easyai-assistant] listening for \"hey %s\" / \"oi %s\" (pause %d ms)\n", a_.name.c_str(), a_.name.c_str(), a_.pause_ms);

        Vad vad;
        Segmenter idle(kBurstEndMs, kWakeMaxMs), listen(a_.pause_ms, a_.max_listen_ms);
        bool listening = false;
        std::vector<std::int16_t> period;
        auto mute_until = std::chrono::steady_clock::now();
        int level_log = 0;

        while (!g_stop) {
            if (!cap_.read(period)) { std::fprintf(stderr, "[easyai-assistant] microphone: %s\n", cap_.last_error().c_str()); return 2; }
            const bool speech = vad.feed(period.data(), period.size());
            if (a_.verbose && ++level_log % 50 == 0)
                std::fprintf(stderr, "[easyai-assistant] level %.0f dB floor %.0f dB%s\n", vad.last_db(), vad.floor_db(), speech ? " SPEECH" : "");
            if (speaker_.speaking() || std::chrono::steady_clock::now() < mute_until) { idle.reset(); listen.reset(); continue; }

            if (!listening) {
                if (!idle.push(period.data(), period.size(), speech)) continue;
                auto burst = idle.take();
                std::string text, lang;
                if (!transcribe(burst, true, text, lang)) continue;
                if (a_.verbose && !text.empty()) std::fprintf(stderr, "[easyai-assistant] heard: %s\n", text.c_str());
                const WakeResult w = wake_match(text, a_.name);
                if (!w.matched) continue;
                if (!w.question.empty() && !wake_.is_loaded()) {   // server transcript is good enough to answer directly
                    answer(w.question, lang);
                    mute_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUnmuteMs);
                    if (a_.once) break;
                    continue;
                }
                beep(1);
                listening = true; listen.reset();
                mute_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUnmuteMs);
                continue;
            }

            if (!listen.push(period.data(), period.size(), speech)) continue;
            listening = false;
            auto utt = listen.take();
            std::string text, lang;
            const bool ok = transcribe(utt, false, text, lang);
            if (!ok || normalize_text(text).empty()) { beep(2); mute_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUnmuteMs); continue; }
            answer(text, lang);
            mute_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUnmuteMs);
            if (a_.once) break;
        }
        return 0;
    }

private:
    Args &                 a_;
    Speaker                speaker_;
    easyai::Client         cli_;
    easyai::Stt            wake_;
    easyai::audio::Capture cap_;
    SentenceChunker        chunker_;
    std::string            lang_ = "en";
    int                    turns_ = 0;
    bool                   server_down_ = false;
    std::chrono::steady_clock::time_point last_turn_;
};

}  // namespace

int main(int argc, char ** argv) {
    Args a;
    if (!parse_args(argc, argv, a)) return 1;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::fprintf(stderr, "[easyai-assistant] name=%s endpoint=%s model=%s stt=%s tts=%s audio=%s\n",
                 a.name.c_str(), a.endpoint.c_str(), a.model.c_str(), a.stt_endpoint.c_str(), a.tts_endpoint.c_str(),
                 easyai::audio::backend_name().c_str());
    Assistant as(a);
    return as.run();
}
