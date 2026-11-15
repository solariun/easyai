// easyai::Assistant — the voice assistant built into easyai-server (spec.md
// "easyai-assistant — voice assistant service", rules 13-16). Behaviour of
// the loop is the old services/assistant.cpp; see include/easyai/assistant.hpp.
#include "easyai/assistant.hpp"

#include "easyai/assistant_core.hpp"
#include "easyai/builtin_tools.hpp"
#include "easyai/client.hpp"
#include "easyai/config.hpp"
#include "easyai/external_tools.hpp"
#include "easyai/stt.hpp"
#include "easyai/tts.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace easyai {

using namespace easyai::assistant;

namespace {

constexpr int kUnmuteMs          = 300;    // after the voice stops, before the mic counts again
constexpr int kMicRetryS         = 15;     // no capture device yet (unplugged USB mic): retry, do not give up
constexpr int kHttpTimeoutS      = 120;
constexpr int kTtsProbeS         = 30;     // tts=auto/server and the server's voice was down: try it again this often
constexpr int kDefaultToolHops   = 6;
constexpr int kMaxAnswerTokens   = 400;    // spoken answers are short; a runaway model must not talk for minutes
constexpr const char * kFallbackEndpoint = "http://127.0.0.1:8080";
constexpr const char * kIniSection       = "ASSISTANT";

const char * kAssistantSystem =
    "You are %s, a voice assistant. Your answers are spoken aloud by a text-to-speech engine, so:\n"
    "- Answer in at most three short sentences unless the user explicitly asks for detail.\n"
    "- Be direct: no preamble, no restating the question, no closing offers.\n"
    "- ALWAYS answer in the same language the user spoke (Portuguese or English).\n"
    "- Plain spoken prose only: no lists, no markdown, no code, no URLs, no emojis. Spell numbers and units the way people say them.\n"
    "- Do not think out loud and do not explain your reasoning.\n"
    "- Before calling any tool, say one short sentence naming what you are about to use, in the user's language "
    "(for example \"Vou consultar a ferramenta de data e hora.\" or \"Let me check the web.\"), then call it.\n"
    "- Never cite where an answer came from: no source names, sites, links or tool names in the answer itself.\n"
    "- The internal clock below is the current local date and time; answer date and time questions from it, "
    "without any tool.\n"
    "- If the question is unclear, ask one short question back.\n";

// The internal clock, refreshed for every turn and appended to the system prompt.
std::string clock_line() {
    const std::time_t t = std::time(nullptr);
    char buf[128];
    std::strftime(buf, sizeof buf, "%A, %Y-%m-%d %H:%M (%Z)", std::localtime(&t));
    return std::string("Internal clock: ") + buf + ".";
}

std::int64_t now_s() {
    return (std::int64_t) std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string read_file(const std::string & p) {
    std::ifstream f(p); if (!f) return "";
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char) s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char) s.back()))  s.pop_back();
    return s;
}

std::string lower(std::string s) {
    for (auto & c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}

std::vector<std::string> split_csv(const std::string & csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv); std::string t;
    while (std::getline(ss, t, ',')) { t = trim(t); if (!t.empty()) out.push_back(t); }
    return out;
}

std::string unavailable_phrase(const std::string & lang) {
    return lang.rfind("pt", 0) == 0 ? "O servidor está indisponível no momento." : "The server is unavailable right now.";
}

bool parse_bool(const std::string & v, bool & out) {
    const std::string s = lower(trim(v));
    if (s == "on" || s == "1" || s == "true" || s == "yes")  { out = true;  return true; }
    if (s == "off" || s == "0" || s == "false" || s == "no") { out = false; return true; }
    return false;
}

bool parse_int(const std::string & v, int & out) {
    const std::string s = trim(v);
    if (s.empty()) return false;
    char * end = nullptr;
    errno = 0;
    const long n = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    if (n < -2147483647L - 1 || n > 2147483647L) return false;
    out = (int) n;
    return true;
}

bool in_choices(const std::string & choices, const std::string & v) {
    std::stringstream ss(choices); std::string c;
    while (std::getline(ss, c, '|')) if (c == v) return true;
    return false;
}

// One row per Config field. The pointer-to-member accessors keep set/get/to_map
// in one place; the table itself is the single source of truth.
struct Field {
    Assistant::Option opt;
    std::string Assistant::Config::* s = nullptr;
    int         Assistant::Config::* i = nullptr;
    bool        Assistant::Config::* b = nullptr;
};

std::string flag_of(const std::string & key) {
    std::string f = "--assistant-" + key;
    std::replace(f.begin(), f.end(), '_', '-');
    return f;
}

Field S(const char * key, std::string Assistant::Config::* m, const char * def, const char * group, const char * help, const char * choices = "") {
    Field f; f.opt.key = key; f.opt.flag = flag_of(key); f.opt.type = Assistant::Option::Str; f.opt.def = def;
    f.opt.choices = choices; f.opt.group = group; f.opt.help = help; f.s = m; return f;
}
Field I(const char * key, int Assistant::Config::* m, int def, int min, int max, const char * group, const char * help) {
    Field f; f.opt.key = key; f.opt.flag = flag_of(key); f.opt.type = Assistant::Option::Int; f.opt.def = std::to_string(def);
    f.opt.min = min; f.opt.max = max; f.opt.group = group; f.opt.help = help; f.i = m; return f;
}
Field B(const char * key, bool Assistant::Config::* m, bool def, const char * group, const char * help) {
    Field f; f.opt.key = key; f.opt.flag = flag_of(key); f.opt.type = Assistant::Option::Bool; f.opt.def = def ? "on" : "off";
    f.opt.group = group; f.opt.help = help; f.b = m; return f;
}

const std::vector<Field> & fields() {
    using C = Assistant::Config;
    static const std::vector<Field> k = {
        B("enabled",        &C::enabled,        false, "General",   "start the voice assistant with the server (the dashboard's Assistant tab starts and stops it live too)"),
        S("name",           &C::name,           "max", "Wake word", "wake name, said after a call word: hello <name> / olá <name>"),
        S("call_words",     &C::call_words,     "hello, olá, alô, hallo, hola", "Wake word",
                                                "csv of words that must come right before the name; empty = loose mode (hey/oi/… or the bare name), wakes more often"),
        S("name_aliases",   &C::name_aliases,   "",    "Wake word", "csv of what whisper hears for the name (max, easy and easyai have built-in ones); verbose shows 'heard:' lines to collect them"),
        S("greeting",       &C::greeting,       "en",  "Voice",     "spoken once at start-up: says it is up and how to call it", "en|pt|off"),
        S("language",       &C::language,       "auto", "Wake word", "forced transcription language, auto = detect", "auto|pt|en"),
        S("endpoint",       &C::endpoint,       "",    "Brain",     "OpenAI-compatible base URL for the chat; empty = this server"),
        S("api_key",        &C::api_key,        "",    "Brain",     "Bearer token for the endpoint(s); empty = this server's own key when the endpoint is this server, no key for any other endpoint"),
        S("model",          &C::model,          "default", "Brain", "model id sent in the request"),
        S("stt_endpoint",   &C::stt_endpoint,   "",    "Brain",     "base URL for /v1/audio/transcriptions; empty = this server's own speech-to-text"),
        S("tts_endpoint",   &C::tts_endpoint,   "",    "Brain",     "base URL for /v1/audio/speech; empty = this server's own voice"),
        I("pause_ms",       &C::pause_ms,       1500, 300, 10000, "Timing", "silence that ends a question"),
        I("max_listen_ms",  &C::max_listen_ms,  20000, 2000, 120000, "Timing", "longest question"),
        I("followup_ms",    &C::followup_ms,    2000, 0, 30000, "Timing", "after an answer keep listening without the name; silence this long ends the conversation with a falling sound (0 = off)"),
        S("wake_model",     &C::wake_model,     "server", "Wake word", "local whisper ggml for the wake word, or 'server' = every burst through the speech-to-text of the server"),
        S("mic",            &C::mic,            "default", "Audio",  "capture device (ALSA name, e.g. plughw:1,0)"),
        S("speaker",        &C::speaker,        "default", "Audio",  "playback device (ALSA name)"),
        I("mic_gain_db",    &C::mic_gain_db,    0, kMicGainMinDb, kMicGainMaxDb, "Audio", "software gain on the microphone in dB; negative when the room saturates the input"),
        S("tts",            &C::tts,            "auto", "Voice",    "voice engine: auto/server = the server's voice (Kokoro), else a local engine; espeak (robotic) only when set here by hand, never automatically", "auto|server|kokoro|piper|espeak|say"),
        S("voice_pt",       &C::voice_pt,       "",    "Voice",     "local engine voice for pt (piper .onnx / say / espeak)"),
        S("voice_en",       &C::voice_en,       "",    "Voice",     "local engine voice for en"),
        S("piper",          &C::piper,          "piper", "Voice",   "piper binary for the local fallback"),
        S("kokoro_model",   &C::kokoro_model,   "",    "Voice",     "local Kokoro-82M .onnx (natural voice when tts is not the server)"),
        S("kokoro_voices",  &C::kokoro_voices,  "",    "Voice",     "local Kokoro voices-v1.0.bin"),
        S("tools",          &C::tools,          "datetime,web", "Brain", "csv of built-in tools: datetime, web — empty = none"),
        S("external_tools", &C::external_tools, "",    "Brain",     "directory of EASYAI-*.tools manifests (MCP-style external tools)"),
        S("system",         &C::system,         "",    "Brain",     "system prompt: a file path or inline text; empty = built-in"),
        I("history_turns",  &C::history_turns,  6, 0, 50, "Brain",  "turns kept before the conversation restarts"),
        I("history_idle_s", &C::history_idle_s, 300, 10, 86400, "Brain", "idle seconds after which the conversation restarts"),
        B("verbose",        &C::verbose,        false, "Advanced",  "log VAD levels, transcripts and timings"),
        B("tls_insecure",   &C::tls_insecure,   false, "Advanced",  "skip certificate verification (self-signed endpoint)"),
    };
    return k;
}

const Field * field_of(const std::string & key) {
    for (const auto & f : fields()) if (f.opt.key == key) return &f;
    return nullptr;
}

// Kokoro grades: "A+" > "A" > "A-" > "B+" … ; "" (ungraded) last.
int grade_rank(const std::string & g) {
    if (g.empty()) return -1;
    int r = ('G' - (char) std::toupper((unsigned char) g[0])) * 3 + 1;   // A → 18, B → 15 …
    if (g.size() > 1 && g[1] == '+') r += 1;
    if (g.size() > 1 && g[1] == '-') r -= 1;
    return r;
}

}  // namespace

// ===========================================================================
// Option table, Config I/O
// ===========================================================================

const std::vector<Assistant::Option> & Assistant::options() {
    static const std::vector<Option> k = [] {
        std::vector<Option> v;
        for (const auto & f : fields()) v.push_back(f.opt);
        return v;
    }();
    return k;
}

std::string Assistant::key_of_flag(const std::string & flag) {
    for (const auto & f : fields()) if (f.opt.flag == flag) return f.opt.key;
    return "";
}

bool Assistant::set(Config & c, const std::string & key, const std::string & value, std::string & err) {
    err.clear();
    const Field * f = field_of(key);
    if (!f) { err = "unknown [ASSISTANT] key '" + key + "'"; return false; }
    switch (f->opt.type) {
    case Option::Bool: {
        bool b = false;
        if (!parse_bool(value, b)) { err = key + ": expected on|off (also 1|0, true|false, yes|no), got '" + value + "'"; return false; }
        c.*(f->b) = b;
        return true;
    }
    case Option::Int: {
        int n = 0;
        if (!parse_int(value, n)) { err = key + ": expected an integer, got '" + value + "'"; return false; }
        if ((f->opt.min != 0 || f->opt.max != 0) && (n < f->opt.min || n > f->opt.max)) {
            err = key + ": " + std::to_string(n) + " is out of range " + std::to_string(f->opt.min) + ".." + std::to_string(f->opt.max);
            return false;
        }
        c.*(f->i) = n;
        return true;
    }
    case Option::Str:
    default: {
        std::string v = value;
        if (!f->opt.choices.empty()) {
            v = lower(trim(v));
            if (!in_choices(f->opt.choices, v)) {
                std::string ch = f->opt.choices; std::replace(ch.begin(), ch.end(), '|', ' ');
                err = key + ": '" + value + "' is not one of: " + ch; return false;
            }
        }
        c.*(f->s) = v;
        return true;
    }
    }
}

std::string Assistant::get(const Config & c, const std::string & key) {
    const Field * f = field_of(key);
    if (!f) return "";
    if (f->opt.type == Option::Bool) return c.*(f->b) ? "on" : "off";
    if (f->opt.type == Option::Int)  return std::to_string(c.*(f->i));
    return c.*(f->s);
}

std::map<std::string, std::string> Assistant::to_map(const Config & c) {
    std::map<std::string, std::string> m;
    for (const auto & f : fields()) m[f.opt.key] = get(c, f.opt.key);
    return m;
}

Assistant::Config Assistant::from_ini(const config::Ini & ini) {
    Config c;
    // Every key PRESENT in the section counts, an empty value included:
    // "call_words =" means loose wake mode and "tools =" means no tools, as
    // the option table says (a missing key keeps the default).
    for (const auto & kv : ini.section_or_empty(kIniSection)) {
        std::string err;
        if (!set(c, kv.first, kv.second, err)) std::fprintf(stderr, "[assistant] [ASSISTANT] %s — keeping the default\n", err.c_str());
    }
    return c;
}

// ===========================================================================
// Kokoro voice catalogue
// ===========================================================================

const std::vector<KokoroVoiceInfo> & kokoro_voice_catalog() {
    static const std::vector<KokoroVoiceInfo> k = {
#include "kokoro_voices.inc"
    };
    return k;
}

std::string kokoro_best_voice(const std::string & lang, const std::string & gender) {
    std::string l = lower(trim(lang));
    if (l == "en") l = "en-us";
    if (l == "pt") l = "pt-br";
    const std::string g = lower(trim(gender));
    const KokoroVoiceInfo * best = nullptr;
    for (const auto & v : kokoro_voice_catalog()) {
        if (v.lang != l || (!g.empty() && v.gender != g)) continue;
        if (!best) { best = &v; continue; }
        const int a = grade_rank(v.grade), b = grade_rank(best->grade);
        if (a > b) best = &v;
        // Same grade (the whole table ungraded, say): the engine's default voices win, else the first row.
        else if (a == b && (v.id == "am_michael" || v.id == "pm_alex")) best = &v;
    }
    return best ? best->id : "";
}

// ===========================================================================
// Impl
// ===========================================================================

struct Assistant::Impl {
    // A queue of sentences spoken in order on its own thread, so the chat stream
    // never waits for the voice. speaking() mutes the microphone. say() items from
    // the dashboard carry a Done and are serialised with the loop's sentences.
    class Speaker {
    public:
        struct Done { std::mutex mu; std::condition_variable cv; bool done = false, ok = false; std::string err; };
        struct Item { std::string text, lang; std::shared_ptr<Done> done; };

        ~Speaker() {
            { std::lock_guard<std::mutex> lk(mu_); stop_ = true; for (auto & it : queue_) finish(it, false, "voice closed"); queue_.clear(); cv_.notify_all(); }
            worker_.join();
        }
        bool available() const { return use_hook_ || use_server_ || local_.available(); }
        void speak(const std::string & text, const std::string & lang, std::shared_ptr<Done> done = nullptr) {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.push_back({text, lang, std::move(done)});
            speaking_ = true;
            cv_.notify_all();
        }
        void wait_idle() {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return queue_.empty() && !busy_; });
            speaking_ = false;
        }
        // stop(): drop what has not been spoken yet; the sentence playing finishes.
        void cancel_pending() {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto & it : queue_) finish(it, false, "cancelled");
            queue_.clear();
            cv_.notify_all();
        }
        bool speaking() const { return speaking_; }
        // say() from another thread: the clip is done, nothing else queued → the mic may listen again.
        void idle_if_empty() { std::lock_guard<std::mutex> lk(mu_); if (queue_.empty() && !busy_) speaking_ = false; }

        Speaker(Impl & o) : o_(o) {
            const auto & a = o_.cfg;
            use_hook_ = o_.use_tts_hook();
            if (a.tts != "server") {
                Tts::Config c; c.engine = (a.tts == "auto") ? "auto" : a.tts; c.piper = a.piper; c.voice_pt = a.voice_pt; c.voice_en = a.voice_en;
                c.kokoro_model = a.kokoro_model; c.kokoro_voices = a.kokoro_voices;
                std::string err;
                local_.configure(c, err);
                local_err_ = err;
            }
            want_server_ = !use_hook_ && (a.tts == "auto" || a.tts == "server");
            if (want_server_) {
                server_url_ = o_.tts_endpoint();
                auto c = o_.make_http(server_url_);
                auto r = c->Post("/v1/audio/speech", R"({"input":"ok","voice":"en","response_format":"wav"})", "application/json");
                use_server_ = r && r->status == 200;
                last_probe_ = std::chrono::steady_clock::now();
                if (!use_server_)
                    o_.logf("tts: %s/v1/audio/speech not available yet (%s)%s", server_url_.c_str(),
                            r ? std::to_string(r->status).c_str() : httplib::to_string(r.error()).c_str(),
                            local_.available() ? " — local voice until it answers" : "");
            }
            o_.logf("voice: %s", describe().c_str());
            worker_ = std::thread([this] { run(); });
        }

        std::string describe() const {
            if (use_hook_)   return "server (in-process)" + std::string(local_.available() ? ", local " + local_.engine() + " as fallback" : "");
            if (use_server_) return "server " + server_url_ + "/v1/audio/speech";
            if (local_.available()) return "local " + local_.engine();
            return "NONE (" + local_err_ + ") — answers are logged only";
        }

        void play_pcm(const std::vector<std::int16_t> & pcm, int rate) {   // beeps
            std::lock_guard<std::mutex> lk(play_mu_);
            std::string err; audio::Playback pb;
            if (!pb.play(o_.speaker_dev(), pcm.data(), pcm.size(), rate, err)) o_.logf("playback: %s", err.c_str());
        }

        bool say(const Item & it, std::string & err) {
            const std::string voice = it.lang.rfind("pt", 0) == 0 ? "pt" : "en";
            std::vector<std::int16_t> pcm; int rate = 0;
            const auto t0 = std::chrono::steady_clock::now();
            bool ok = false;
            if (use_hook_) {
                ok = o_.hooks.tts(it.text, voice, pcm, rate, err);
                if (!ok) o_.vlogf("tts (server): %s", err.c_str());
            }
            // The server usually starts with the assistant and answers only once
            // its model is loaded: when tts=auto/server found it down at start,
            // try it again every kTtsProbeS with the real sentence and switch to
            // it the first time it answers (the local engine covers the gap).
            const auto now = std::chrono::steady_clock::now();
            const bool retry = want_server_ && !use_server_ && (now - last_probe_) >= std::chrono::seconds(kTtsProbeS);
            if (!ok && (use_server_ || retry)) {
                auto c = o_.make_http(server_url_);
                nlohmann::json body{{"model", "tts"}, {"input", it.text}, {"voice", voice}, {"response_format", "wav"}};
                auto r = c->Post("/v1/audio/speech", body.dump(), "application/json");
                if (r && r->status == 200) ok = decode_wav_pcm16(r->body, pcm, rate);
                else err = r ? "HTTP " + std::to_string(r->status) : httplib::to_string(r.error());
                if (retry) {
                    last_probe_ = now;
                    if (ok) { use_server_ = true; o_.logf("voice: %s", describe().c_str()); o_.with_status([&](Status & x) { x.voice = describe(); }); }
                }
            }
            if (!ok && local_.available()) ok = local_.synthesize(it.text, voice, pcm, rate, err);
            if (!ok) {
                if (err.empty()) err = local_err_.empty() ? "no voice available" : local_err_;
                o_.logf("tts failed: %s", err.c_str());
                o_.with_status([&](Status & x) { ++x.n_errors; x.last_error = "tts: " + err; });
                return false;
            }
            if (rate <= 0 || pcm.empty()) { err = "tts returned no audio"; o_.logf("tts failed: %s", err.c_str()); return false; }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            o_.vlogf("tts %.0f ms, %.1f s audio: %s", ms, (double) pcm.size() / rate, it.text.c_str());
            {
                std::lock_guard<std::mutex> lk(play_mu_);
                audio::Playback pb;
                std::string perr;
                if (!pb.play(o_.speaker_dev(), pcm.data(), pcm.size(), rate, perr)) { err = "playback: " + perr; o_.logf("%s", err.c_str()); return false; }
            }
            return true;
        }

    private:
        static void finish(const Item & it, bool ok, const std::string & err) {
            if (!it.done) return;
            std::lock_guard<std::mutex> lk(it.done->mu);
            it.done->done = true; it.done->ok = ok; it.done->err = err;
            it.done->cv.notify_all();
        }
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
                std::string err;
                const bool ok = say(it, err);
                finish(it, ok, err);
                { std::lock_guard<std::mutex> lk(mu_); busy_ = false; cv_.notify_all(); }   // speaking_ stays until wait_idle(): a gap between streamed sentences is not the end of the answer
            }
        }

        Impl &                  o_;
        easyai::Tts             local_;
        std::string             local_err_;
        std::string             server_url_;
        bool                    use_hook_ = false;
        bool                    use_server_ = false;
        bool                    want_server_ = false;
        std::chrono::steady_clock::time_point last_probe_{};
        std::thread             worker_;
        std::mutex              mu_, play_mu_;
        std::condition_variable cv_;
        std::deque<Item>        queue_;
        bool                    stop_ = false, busy_ = false;
        std::atomic<bool>       speaking_{false};
    };

    Config cfg;
    Hooks  hooks;

    mutable std::mutex       st_mu;
    Status                   st;
    mutable std::mutex       log_mu;
    std::deque<std::string>  log_ring;

    std::thread              th;
    std::atomic<bool>        stop_{false};
    std::atomic<bool>        running_{false};
    std::atomic<bool>        flush_pending_{false};   // say()/ask() from another thread spoke: the loop drops what the mic took in

    std::mutex               rt_mu;      // creation / destruction of speaker and client, inflight
    std::condition_variable  rt_cv;
    int                      inflight = 0;   // say() / ask() calls using speaker / cli right now
    std::unique_ptr<Speaker> speaker;
    std::unique_ptr<Client>  cli;
    std::mutex               chat_mu;    // one chat at a time (loop vs ask())
    std::mutex               ctl_mu;     // start()/stop() from several threads

    Stt                      wake;
    audio::Capture           cap;
    SentenceChunker          chunker;
    std::vector<std::string> aliases, call_words;
    std::string              wake_prompt, base_sys;
    std::string              lang = "en";      // the language of the current turn: "pt" or "en" (rule 5)
    // Language enforcement for the turn: was the first sentence checked, which
    // language it came in when it was the wrong one (→ one retry), is this the
    // retry. cancel_external_: stop() / reset_runtime() cancelled the chat, so
    // a cancelled attempt is theirs, never ours to retry.
    bool                     turn_gate_checked = false;
    std::string              turn_mismatch;
    bool                     turn_retry = false;
    std::atomic<bool>        cancel_external_{false};
    int                      turns = 0;
    bool                     server_down = false;
    bool                     stt_hook_down = false;   // the server's own STT answered "no model": logged once
    std::chrono::steady_clock::time_point last_turn{};
    std::string              mic_in_use, speaker_in_use;   // after the auto-pick
    mutable std::mutex       dev_mu;                       // cfg.mic / cfg.speaker change in open_mic while say() may read them
    std::string speaker_dev() const { std::lock_guard<std::mutex> lk(dev_mu); return cfg.speaker; }

    // ---- logging -------------------------------------------------------------
    // A log line reaches the dashboard as JSON, and nlohmann::json refuses
    // invalid UTF-8 (a transcript cut by the 4 KiB printf buffer in the
    // middle of a multi-byte character, a raw HTTP error body): replace
    // every malformed byte with U+FFFD so the status route always answers.
    static std::string utf8_clean(const std::string & in) {
        std::string out; out.reserve(in.size());
        for (std::size_t i = 0; i < in.size();) {
            const unsigned char c = (unsigned char) in[i];
            std::size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
            bool ok = n > 0 && i + n <= in.size();
            for (std::size_t k = 1; ok && k < n; ++k) ok = ((unsigned char) in[i + k] >> 6) == 0x2;
            if (ok) { out.append(in, i, n); i += n; }
            else { out += "\xEF\xBF\xBD"; ++i; }
        }
        return out;
    }
    void log(const std::string & raw) {
        const std::string line = utf8_clean(raw);
        {
            std::lock_guard<std::mutex> lk(log_mu);
            log_ring.push_back(line);
            while (log_ring.size() > (std::size_t) kLogKeep) log_ring.pop_front();
        }
        if (hooks.log) hooks.log(line);
        else std::fprintf(stderr, "[assistant] %s\n", line.c_str());
    }
    void logf(const char * fmt, ...) {
        char buf[4096];
        va_list ap; va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        log(buf);
    }
    void vlogf(const char * fmt, ...) {
        if (!cfg.verbose) return;
        char buf[4096];
        va_list ap; va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        log(buf);
    }

    // ---- status ----------------------------------------------------------------
    template <class F> void with_status(F f) { std::lock_guard<std::mutex> lk(st_mu); f(st); }
    void set_state(const std::string & s) { with_status([&](Status & x) { x.state = s; }); }
    void error(const std::string & e) {
        logf("error: %s", e.c_str());
        with_status([&](Status & x) { x.last_error = e; ++x.n_errors; });
    }

    // ---- endpoint resolution ------------------------------------------------------
    std::string chat_endpoint() const {
        if (!cfg.endpoint.empty()) return cfg.endpoint;
        if (!hooks.self_endpoint.empty()) return hooks.self_endpoint;
        return kFallbackEndpoint;
    }
    // The server's own key and its self-signed-certificate allowance go to
    // the server itself and nowhere else: an operator who points `endpoint`
    // (or stt_endpoint / tts_endpoint) at another box or at OpenAI without
    // setting api_key gets an anonymous call, never the box's secret. The
    // old standalone program read EASYAI_API_KEY from its environment; the
    // server's unit carries the server's key there, so that fallback is gone.
    bool is_self(const std::string & base) const { return !hooks.self_endpoint.empty() && base == hooks.self_endpoint; }
    std::string key_for(const std::string & base) const {
        if (!cfg.api_key.empty()) return cfg.api_key;
        if (is_self(base)) return hooks.self_api_key;
        return "";
    }
    bool insecure_for(const std::string & base) const { return cfg.tls_insecure || (is_self(base) && hooks.self_tls_insecure); }
    std::string api_key() const { return key_for(chat_endpoint()); }
    bool tls_insecure() const { return insecure_for(chat_endpoint()); }
    std::string stt_endpoint() const { return cfg.stt_endpoint.empty() ? chat_endpoint() : cfg.stt_endpoint; }
    std::string tts_endpoint() const { return cfg.tts_endpoint.empty() ? chat_endpoint() : cfg.tts_endpoint; }
    bool use_stt_hook() const { return (bool) hooks.stt && cfg.stt_endpoint.empty(); }
    bool use_tts_hook() const { return (bool) hooks.tts && cfg.tts_endpoint.empty() && (cfg.tts == "auto" || cfg.tts == "server"); }

    std::unique_ptr<httplib::Client> make_http(const std::string & base) const {
        auto c = std::make_unique<httplib::Client>(base);
        c->set_connection_timeout(10);
        c->set_read_timeout(kHttpTimeoutS);
        c->set_follow_location(false);   // a redirect must not carry the Bearer token to another host
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
        if (insecure_for(base)) c->enable_server_certificate_verification(false);
#endif
        const std::string k = key_for(base);
        if (!k.empty()) c->set_default_headers({{"Authorization", "Bearer " + k}});
        return c;
    }

    // ---- runtime pieces (speaker + client), built from cfg on demand ----------------
    // ---- the language of a turn (rule 5): the answer comes in the language of
    // the question, the voice follows the text it speaks (rule 6) --------------
    static std::string language_line(const std::string & l) {
        return l == "pt"
            ? "Idioma desta resposta: português do Brasil. Responda somente em português, mesmo que a pergunta misture idiomas."
            : "Language of this answer: English. Answer only in English, even if the question mixes languages.";
    }
    static std::string language_nudge(const std::string & l) {
        return l == "pt" ? "\n\n(Responda somente em português do Brasil.)" : "\n\n(Answer only in English.)";
    }
    static std::string lang_name(const std::string & l) { return l == "pt" ? "Portuguese" : l == "en" ? "English" : l; }
    // The voice a sentence is spoken with follows the sentence itself; the
    // question's language decides only when the text gives no evidence.
    std::string voice_for(const std::string & text) const { const std::string d = detect_language(text); return d.empty() ? lang : d; }
    // The first sentence of an answer: true = speak it. False = the model
    // answered in the other language; the attempt is cancelled before a word
    // is spoken and answer() asks once more, with the language spelled out.
    bool gate_ok(const std::string & s) {
        if (turn_gate_checked) return true;
        turn_gate_checked = true;
        if (turn_retry) return true;
        const std::string d = detect_language(s);
        if (d.empty() || d == lang) return true;
        turn_mismatch = d;
        return false;
    }

    void build_runtime() {
        std::lock_guard<std::mutex> lk(rt_mu);
        if (!speaker && !cli) cancel_external_ = false;   // a fresh runtime: no cancel of ours is pending
        if (!speaker) {
            speaker = std::make_unique<Speaker>(*this);
            with_status([&](Status & x) { x.voice = speaker->describe(); });
        }
        if (!cli) build_client();
    }
    // say() / ask() run on HTTP threads and use speaker / cli for seconds; a
    // configure() or start() meanwhile must not free them under their feet.
    // Every such call holds an InflightGuard; reset_runtime() cancels what
    // they are doing (so they return fast), waits until none is left, and
    // only then drops the objects — and installs the new cfg / hooks in the
    // same locked step, so no thread ever reads a half-written Config.
    struct InflightGuard {
        Impl & o;
        explicit InflightGuard(Impl & i) : o(i) { std::lock_guard<std::mutex> lk(o.rt_mu); ++o.inflight; }
        ~InflightGuard() { std::lock_guard<std::mutex> lk(o.rt_mu); --o.inflight; o.rt_cv.notify_all(); }
    };
    void reset_runtime(const Config * new_cfg = nullptr, const Hooks * new_hooks = nullptr) {
        std::unique_lock<std::mutex> lk(rt_mu);
        cancel_external_ = true;
        if (cli) cli->request_cancel();
        if (speaker) speaker->cancel_pending();
        rt_cv.wait(lk, [&] { return inflight == 0; });
        speaker.reset();
        cli.reset();
        if (new_cfg)   cfg   = *new_cfg;
        if (new_hooks) hooks = *new_hooks;
    }
    // The same configuration again? Then the runtime (client history, a
    // loaded local voice) is worth keeping.
    bool same_config(const Config & c, const Hooks & h) const {
        return to_map(c) == to_map(cfg) && h.self_endpoint == hooks.self_endpoint && h.self_api_key == hooks.self_api_key &&
               h.self_tls_insecure == hooks.self_tls_insecure && (bool) h.stt == (bool) hooks.stt && (bool) h.tts == (bool) hooks.tts;
    }
    void build_client() {
        std::string sys = cfg.system;
        if (!sys.empty() && sys.find('\n') == std::string::npos && std::ifstream(sys).good()) sys = read_file(sys);
        if (sys.empty()) {
            char buf[4096];
            std::snprintf(buf, sizeof buf, kAssistantSystem, cfg.name.c_str());
            sys = buf;
        }
        base_sys = sys;
        cli = std::make_unique<Client>();
        cli->endpoint(chat_endpoint()).api_key(api_key()).model(cfg.model).system(sys)
            .max_tool_hops(kDefaultToolHops).max_tokens(kMaxAnswerTokens).verbose(false)
            .timeout_seconds(kHttpTimeoutS)   // per read of the stream; a stop() waits at most this long on a dead endpoint
            .reasoning_effort("low")
            .extra_body_json(R"({"chat_template_kwargs":{"enable_thinking":false}})");
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
        cli->tls_insecure(tls_insecure());
#endif
        cli->on_token([this](const std::string & piece) {
            if (!turn_mismatch.empty()) return;   // this attempt is being cancelled: nothing of it is spoken
            chunker.push(piece);
            std::string s;
            while (chunker.pop(s)) {
                if (!gate_ok(s)) { chunker.clear(); cli->request_cancel(); return; }
                set_state("speaking");
                speaker->speak(s, voice_for(s));
            }
        });
        cli->on_tool([this](const ToolCall & c, const ToolResult &) { logf("tool: %s", c.name.c_str()); });
        register_tools();
        turns = 0;
    }
    void register_tools() {
        for (const auto & t : split_csv(cfg.tools)) {
            if (t == "datetime") cli->add_tool(tools::datetime());
            else if (t == "web") cli->add_tool(tools::web(false));
            else logf("unknown built-in tool '%s' (datetime, web)", t.c_str());
        }
        if (!cfg.external_tools.empty()) {
            std::vector<std::string> reserved;
            for (const auto & x : cli->tools()) reserved.push_back(x.name);
            auto loaded = load_external_tools_from_dir(cfg.external_tools, reserved);
            for (const auto & e : loaded.errors) logf("external-tools: %s", e.c_str());
            for (auto & x : loaded.tools) cli->add_tool(std::move(x));
        }
        std::string names;
        for (const auto & x : cli->tools()) names += (names.empty() ? "" : ", ") + x.name;
        logf("tools: %s", names.empty() ? "(none)" : names.c_str());
        with_status([&](Status & x) { x.tools = names; });
    }
    void build_wake_words() {
        aliases = default_aliases(cfg.name);
        call_words.clear();
        const auto builtin = default_call_words();
        for (const auto & c : split_csv(cfg.call_words)) {   // each built-in one expanded with the spellings whisper gives it
            const std::string n = normalize_text(c);
            const bool known = std::find(builtin.begin(), builtin.end(), n) != builtin.end() || n == "ola" || n == "alo" || n == "hello";
            if (known) { for (const auto & b : builtin) if (std::find(call_words.begin(), call_words.end(), b) == call_words.end()) call_words.push_back(b); }
            else call_words.push_back(n);
        }
        // whisper's prompt: a sentence that names the assistant, so the decoder prefers that
        // spelling. Never a call phrase ("Hey easy AI."): whisper then takes the real call as
        // already said and drops it (measured 2026-10-16: 6 of 12 names lost).
        wake_prompt = "O assistente chama-se " + spoken_name(cfg.name, "en") + ". The assistant is called " + spoken_name(cfg.name, "en") + ".";
        for (const auto & al : split_csv(cfg.name_aliases)) aliases.push_back(al);
    }

    // ---- microphone ------------------------------------------------------------------
    // ALSA "default" is card 0, which on many boxes is HDMI-only: no capture.
    // When the configured mic fails and it was "default", try every PCM that
    // reports capture in /proc/asound/pcm; a matching speaker follows the mic.
    bool open_mic() {
        std::string err;
        mic_in_use = cfg.mic; speaker_in_use = cfg.speaker;
        if (cap.open(cfg.mic, kSampleRate, kPeriodMs, err)) return true;
        logf("microphone %s: %s", cfg.mic.c_str(), err.c_str());
        if (cfg.mic != "default") return false;
        const auto devs = parse_asound_pcm(read_file("/proc/asound/pcm"));
        bool any = false;
        for (const auto & d : devs) {
            if (!d.capture) continue;
            any = true;
            if (cap.open(d.alsa_name(), kSampleRate, kPeriodMs, err)) {
                logf("microphone: using %s (%s) — set [ASSISTANT] mic to keep it", d.alsa_name().c_str(), d.name.c_str());
                mic_in_use = d.alsa_name() + " (" + d.name + ")";
                std::lock_guard<std::mutex> lk(dev_mu);
                cfg.mic = d.alsa_name();
                if (cfg.speaker == "default" && d.playback) {
                    cfg.speaker = d.alsa_name();
                    speaker_in_use = cfg.speaker + " (" + d.name + ")";
                    logf("speaker: following the microphone card, %s", cfg.speaker.c_str());
                }
                return true;
            }
            logf("microphone %s (%s): %s", d.alsa_name().c_str(), d.name.c_str(), err.c_str());
        }
        if (!any) log("no ALSA capture device on this box (plug a microphone; /proc/asound/pcm lists none)");
        return false;
    }
    bool open_audio() {
        while (!open_mic()) {
            if (stop_) return false;
            set_state("no_mic");
            logf("retrying the microphone in %d s", kMicRetryS);
            for (int i = 0; i < kMicRetryS * 10 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (stop_) return false;
        }
        logf("microphone: %s (%s, %d Hz, %d ms periods)", cfg.mic.c_str(), audio::backend_name().c_str(), kSampleRate, kPeriodMs);
        std::string wm = cfg.wake_model;
        if (cfg.wake_model != "server" && !wake.is_loaded()) {
            if (!Stt::available()) { log("wake model: built without whisper — using the server for every burst"); wm = "none"; }
            else if (!wake.load(cfg.wake_model)) { logf("wake model: %s — using the server for every burst", wake.last_error().c_str()); wm = "none"; }
            else logf("wake model: %s", cfg.wake_model.c_str());
        }
        with_status([&](Status & x) {
            x.mic = mic_in_use; x.speaker = speaker_in_use; x.backend = audio::backend_name(); x.wake_model = wm;
        });
        return true;
    }

    // ---- one burst of audio → text ------------------------------------------------------
    bool transcribe(const std::vector<std::int16_t> & pcm, bool local_ok, std::string & text, std::string & lng) {
        text.clear();
        if (local_ok && wake.is_loaded()) {
            std::vector<float> f(pcm.size());
            for (std::size_t i = 0; i < pcm.size(); ++i) f[i] = pcm[i] / 32768.0f;
            Stt::Result r;
            if (!wake.transcribe(f.data(), f.size(), cfg.language, wake_prompt, r)) return false;
            text = drop_prompt_echo(r.text, wake_prompt); lng = r.language;
            return true;
        }
        if (use_stt_hook()) {
            std::string t, l, err;
            if (hooks.stt(pcm, cfg.language, wake_prompt, t, l, err)) { stt_hook_down = false; text = drop_prompt_echo(t, wake_prompt); lng = l; return true; }
            if (cfg.stt_endpoint.empty()) {
                // The hook IS this server's speech-to-text: an HTTP request to
                // the same box would hit the same unloaded model. Say it once.
                if (!stt_hook_down) { logf("stt: %s (load a voice model from /models → Local models → Voice transcription)", err.c_str()); stt_hook_down = true; }
                with_status([&](Status & x) { x.last_error = "stt: " + err; });
                return false;
            }
            vlogf("stt (server): %s — trying %s", err.c_str(), stt_endpoint().c_str());
        }
        auto c = make_http(stt_endpoint());
        httplib::UploadFormDataItems items = {
            {"file", encode_wav_pcm16(pcm.data(), pcm.size(), kSampleRate), "speech.wav", "audio/wav"},
            {"model", "whisper-1", "", ""},
            {"response_format", "verbose_json", "", ""},
            {"prompt", wake_prompt, "", ""},
        };
        if (cfg.language != "auto") items.push_back({"language", cfg.language, "", ""});
        auto r = c->Post("/v1/audio/transcriptions", items);
        if (!r || r->status != 200) {
            logf("stt: %s", r ? ("HTTP " + std::to_string(r->status) + " " + r->body).c_str() : httplib::to_string(r.error()).c_str());
            with_status([&](Status & x) { ++x.n_errors; x.last_error = r ? "stt HTTP " + std::to_string(r->status) : "stt: " + httplib::to_string(r.error()); });
            return false;
        }
        try {
            auto j = nlohmann::json::parse(r->body);
            text = drop_prompt_echo(j.value("text", ""), wake_prompt); lng = j.value("language", "");
        } catch (...) { return false; }
        return true;
    }

    // ---- one question → spoken answer (caller holds chat_mu) -----------------------------------
    std::string answer(const std::string & question, const std::string & lang_hint) {
        {   // the assistant speaks Portuguese and English (Limits): anything else is answered in English
            const std::string l = lang_hint.empty() ? guess_language(question) : lang_hint;
            lang = l.rfind("pt", 0) == 0 ? "pt" : "en";
        }
        const auto now = std::chrono::steady_clock::now();
        if (turns > 0 && (turns >= cfg.history_turns ||
                          std::chrono::duration_cast<std::chrono::seconds>(now - last_turn).count() >= cfg.history_idle_s)) {
            cli->clear_history(); turns = 0;
        }
        with_status([&](Status & x) { x.state = "thinking"; x.last_question = question; x.last_question_lang = lang; x.last_question_at = now_s(); ++x.n_questions; });
        logf("Q (%s): %s", lang.c_str(), question.c_str());
        cli->system(base_sys + "\n" + clock_line() + "\n" + language_line(lang));   // the internal clock and the language, fresh every turn
        chunker.clear();
        turn_gate_checked = false; turn_mismatch.clear(); turn_retry = false;
        const std::string history_before = cli->dump_history();
        const auto t0 = std::chrono::steady_clock::now();
        std::string reply = cli->chat(question);
        std::string rest = chunker.flush();
        if (!turn_gate_checked && !rest.empty() && !gate_ok(rest)) rest.clear();   // a short answer: all of it is the first sentence
        if (!turn_mismatch.empty() && !stop_ && !cancel_external_) {
            // The model ignored the language; nothing was spoken. Roll the
            // conversation back and ask once more with the language spelled
            // out in the question itself. The retry is spoken whatever comes.
            logf("answer came in %s to a question in %s — asking again", lang_name(turn_mismatch).c_str(), lang_name(lang).c_str());
            cli->clear_cancel();
            std::string herr;
            if (!cli->load_history(history_before, &herr)) vlogf("history rollback: %s", herr.c_str());
            turn_retry = true; turn_gate_checked = false; turn_mismatch.clear(); chunker.clear();
            reply = cli->chat(question + language_nudge(lang));
            rest = chunker.flush();
        }
        if (!rest.empty()) { set_state("speaking"); speaker->speak(rest, voice_for(rest)); }
        if (reply.empty()) {
            const std::string e = cli->last_error();
            logf("chat failed: %s", e.c_str());
            with_status([&](Status & x) { ++x.n_errors; x.last_error = "chat: " + e; });
            if (!server_down && !cli->cancel_requested()) { speaker->speak(unavailable_phrase(lang), lang); server_down = true; }
        } else {
            logf("A (%s): %s", lang_name(voice_for(reply)).c_str(), reply.c_str());
            with_status([&](Status & x) { x.last_answer = reply; x.last_answer_at = now_s(); ++x.n_answers; });
            server_down = false;
            ++turns; last_turn = std::chrono::steady_clock::now();
        }
        speaker->wait_idle();
        flush_pending_ = true;   // what the mic took in while we spoke is not a question
        vlogf("turn %.0f ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        return reply;
    }

    void beep(int n)     { speaker->play_pcm(make_beep(kSampleRate, n), kSampleRate); cap.flush(); }
    void off_sound()     { speaker->play_pcm(make_off_sound(kSampleRate), kSampleRate); cap.flush(); }
    void think_sound()   { speaker->play_pcm(make_think_sound(kSampleRate), kSampleRate); cap.flush(); }

    // Spoken once after the microphone is open: proof the box hears and talks, and how to call it.
    void greet() {
        if (cfg.greeting == "off" || cfg.greeting.empty()) return;
        const bool pt = cfg.greeting == "pt";
        const std::string nm = spoken_name(cfg.name, pt ? "pt" : "en");
        const std::string call = call_words.empty() ? (pt ? "oi" : "hey") : (pt ? "olá" : "hello");
        const std::string text = pt
            ? "O assistente " + nm + " está no ar. Diga \"" + call + " " + nm + "\" e faça a sua pergunta."
            : "The " + nm + " assistant is online. Say \"" + call + " " + nm + "\" and ask your question.";
        if (!speaker->available()) {   // no natural voice: say nothing rather than anything robotic
            logf("greeting skipped — %s", speaker->describe().c_str());
            return;
        }
        logf("greeting (%s): %s", pt ? "pt" : "en", text.c_str());
        set_state("speaking");
        speaker->speak(text, pt ? "pt" : "en");
        speaker->wait_idle();
        cap.flush();
    }

    // ---- the loop ---------------------------------------------------------------------------
    void run() {
        bool greeted = false;
        while (!stop_) {
            if (!open_audio()) break;
            set_state("idle");
            logf("listening for \"%s %s\" (pause %d ms, mic gain %d dB)",
                 call_words.empty() ? "hey" : call_words.front().c_str(), cfg.name.c_str(), cfg.pause_ms, cfg.mic_gain_db);
            if (!greeted) { greet(); greeted = true; set_state("idle"); }
            listen();            // returns on a microphone failure or stop()
            cap.close();
            if (!stop_) { set_state("no_mic"); logf("microphone lost — retrying in %d s", kMicRetryS); for (int i = 0; i < kMicRetryS * 10 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        }
        cap.close();
    }

    void listen() {
        Vad vad;
        Segmenter idle(kBurstEndMs, kWakeMaxMs), listening_seg(cfg.pause_ms, cfg.max_listen_ms);
        bool listening = false;      // the question after the name is being recorded
        bool conversation = false;   // after an answer: a follow-up needs no name
        std::vector<std::int16_t> period;
        auto mute_until = std::chrono::steady_clock::now();
        auto followup_until = mute_until;
        int level_log = 0;
        // After an answer: the listening beep, then listen on for followup_ms; nothing said → falling sound, back to idle.
        auto after_answer = [&]() {
            if (cfg.followup_ms > 0) {
                beep(1);   // same tone as after the name: "you can speak now"
                conversation = true;
                set_state("conversation");
                mute_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUnmuteMs);
                followup_until = mute_until + std::chrono::milliseconds(cfg.followup_ms);
                listening_seg.reset();
                vlogf("conversation: beep, listening %d ms for a follow-up", cfg.followup_ms);
            } else {
                set_state("idle");
                mute_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUnmuteMs);
            }
        };
        auto do_answer = [&](const std::string & q, const std::string & l) {
            std::lock_guard<std::mutex> lk(chat_mu);
            answer(q, l);
        };

        while (!stop_) {
            if (!cap.read(period)) { logf("microphone: %s", cap.last_error().c_str()); return; }
            if (flush_pending_.exchange(false)) { cap.flush(); mute_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUnmuteMs); continue; }
            apply_gain_db(period, cfg.mic_gain_db);
            const bool speech = vad.feed(period.data(), period.size());
            with_status([&](Status & x) { x.level_db = vad.last_db(); x.floor_db = vad.floor_db(); x.speech = speech; });
            if (cfg.verbose && ++level_log % 50 == 0)
                vlogf("level %.0f dB floor %.0f dB%s", vad.last_db(), vad.floor_db(), speech ? " SPEECH" : "");
            if (speaker->speaking() || std::chrono::steady_clock::now() < mute_until) { idle.reset(); listening_seg.reset(); continue; }

            if (conversation && !listening) {
                // follow-up window: speech starts the question directly; silence ends the conversation
                if (speech) {
                    listening = true;
                    set_state("listening");
                    listening_seg.push(period.data(), period.size(), speech);
                    continue;
                }
                if (std::chrono::steady_clock::now() >= followup_until) {
                    conversation = false;
                    vlogf("conversation over");
                    off_sound();
                    set_state("idle");
                    mute_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUnmuteMs);
                }
                continue;
            }

            if (!listening) {
                if (!idle.push(period.data(), period.size(), speech)) continue;
                auto burst = idle.take();
                std::string text, lng;
                if (!transcribe(burst, true, text, lng)) continue;
                if (!text.empty()) {
                    vlogf("heard: %s", text.c_str());
                    with_status([&](Status & x) { x.last_heard = text; x.last_heard_at = now_s(); });
                }
                const WakeResult w = wake_match(text, cfg.name, aliases, call_words);
                if (!w.matched) continue;
                with_status([&](Status & x) { ++x.n_wakes; });
                if (!w.question.empty()) {
                    // The question came in the same breath. The small wake model is good
                    // enough to spot the name, not to transcribe the question: send the same
                    // audio to the STT of the server (the large model) and take the words after
                    // the name from that transcript.
                    std::string full = text, full_lang = lng;
                    think_sound();
                    set_state("thinking");
                    if (wake.is_loaded() && !transcribe(burst, false, full, full_lang)) full = text;
                    if (wake.is_loaded()) vlogf("server heard: %s", full.c_str());
                    const WakeResult w2 = wake_match(full, cfg.name, aliases, call_words);
                    const std::string q = w2.matched && !w2.question.empty() ? w2.question : (wake.is_loaded() ? "" : w.question);
                    if (!q.empty()) {
                        do_answer(q, w2.matched ? full_lang : lng);
                        after_answer();
                        continue;
                    }
                }
                beep(1);
                listening = true; listening_seg.reset();
                set_state("listening");
                mute_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUnmuteMs);
                continue;
            }

            if (!listening_seg.push(period.data(), period.size(), speech)) continue;
            listening = false;
            auto utt = listening_seg.take();
            think_sound();   // "got it, working": the pause was understood
            set_state("thinking");
            std::string text, lng;
            const bool ok = transcribe(utt, false, text, lng);
            if (!ok || !looks_like_question(text)) {   // nothing, or whisper's noise markers / a lone hallucinated word
                if (ok) vlogf("ignored: %s", text.c_str());
                if (conversation) {   // a noise inside the follow-up window: keep the window open a little longer
                    set_state("conversation");
                    mute_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUnmuteMs);
                    followup_until = mute_until + std::chrono::milliseconds(cfg.followup_ms);
                    continue;
                }
                beep(2); set_state("idle"); mute_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUnmuteMs); continue;
            }
            do_answer(text, lng);
            after_answer();
        }
    }
};

// ===========================================================================
// Assistant
// ===========================================================================

Assistant::Assistant() : p_(new Impl) {}
Assistant::~Assistant() { stop(); p_->reset_runtime(); }

bool Assistant::start(const Config & cfg, const Hooks & hooks, std::string & err) {
    std::lock_guard<std::mutex> ctl(p_->ctl_mu);
    err.clear();
    stop();
    p_->reset_runtime(&cfg, &hooks);
    p_->stop_ = false;
    p_->server_down = false;
    p_->turns = 0;
    p_->wake.unload();
    p_->with_status([&](Status & x) {
        x = Status();
        x.state = "starting"; x.running = true; x.since = now_s();
        x.backend = audio::backend_name(); x.wake_model = cfg.wake_model; x.mic = cfg.mic; x.speaker = cfg.speaker;
    });
    if (audio::backend_name() == "none") {
        err = "built without an audio backend (ALSA headers missing at build time)";
        p_->error(err);
        p_->with_status([&](Status & x) { x.state = "error"; x.running = false; x.since = 0; });
        return false;
    }
    for (const char * k : {"pause_ms", "max_listen_ms", "followup_ms", "mic_gain_db", "history_turns", "history_idle_s"}) {
        std::string e;
        Config probe = cfg;
        if (!set(probe, k, get(cfg, k), e)) {
            err = e; p_->error(err);
            p_->with_status([&](Status & x) { x.state = "error"; x.running = false; x.since = 0; });
            return false;
        }
    }
    p_->logf("name=%s endpoint=%s model=%s stt=%s tts=%s audio=%s", cfg.name.c_str(), p_->chat_endpoint().c_str(), cfg.model.c_str(),
             p_->use_stt_hook() ? "server (in-process)" : p_->stt_endpoint().c_str(),
             p_->use_tts_hook() ? "server (in-process)" : (cfg.tts == "auto" || cfg.tts == "server") ? p_->tts_endpoint().c_str() : cfg.tts.c_str(),
             audio::backend_name().c_str());
    p_->build_wake_words();
    p_->build_runtime();
    p_->running_ = true;
    p_->th = std::thread([this] {
        p_->run();
        p_->running_ = false;
        p_->with_status([&](Status & x) { if (x.state != "error") x.state = "off"; x.running = false; x.since = 0; x.speech = false; });
    });
    return true;
}

void Assistant::configure(const Config & cfg, const Hooks & hooks) {
    std::lock_guard<std::mutex> ctl(p_->ctl_mu);
    if (p_->th.joinable()) return;   // running: start() owns the config
    if (p_->same_config(cfg, hooks) && (p_->speaker || p_->cli)) return;   // nothing changed: keep the client's history and the loaded voice
    p_->reset_runtime(&cfg, &hooks);
    p_->wake.unload();
    p_->build_wake_words();
    p_->with_status([&](Status & x) {
        x = Status();
        x.backend = audio::backend_name(); x.wake_model = cfg.wake_model; x.mic = cfg.mic; x.speaker = cfg.speaker;
    });
}

void Assistant::stop() {
    const bool was_running = p_->th.joinable();
    p_->stop_ = true;
    {
        std::lock_guard<std::mutex> lk(p_->rt_mu);
        p_->cancel_external_ = true;
        if (p_->cli) p_->cli->request_cancel();
        if (p_->speaker) p_->speaker->cancel_pending();
    }
    if (p_->th.joinable()) p_->th.join();
    p_->running_ = false;
    { std::lock_guard<std::mutex> lk(p_->rt_mu); if (p_->cli) p_->cli->clear_cancel(); }
    p_->with_status([&](Status & x) { x.state = "off"; x.running = false; x.since = 0; x.speech = false; });
    if (was_running) p_->log("stopped");
}

bool Assistant::running() const { return p_->running_; }

const Assistant::Config & Assistant::config() const { return p_->cfg; }

Assistant::Status Assistant::status() const {
    std::lock_guard<std::mutex> lk(p_->st_mu);
    return p_->st;
}

std::string Assistant::status_json() const {
    const Status s = status();
    nlohmann::json j{
        {"state", s.state}, {"running", s.running}, {"since", s.since},
        {"mic", s.mic}, {"speaker", s.speaker}, {"backend", s.backend}, {"wake_model", s.wake_model},
        {"voice", s.voice}, {"tools", s.tools},
        {"level_db", s.level_db}, {"floor_db", s.floor_db}, {"speech", s.speech},
        {"last_heard", s.last_heard}, {"last_heard_at", s.last_heard_at},
        {"last_question", s.last_question}, {"last_question_lang", s.last_question_lang}, {"last_question_at", s.last_question_at},
        {"last_answer", s.last_answer}, {"last_answer_at", s.last_answer_at},
        {"n_wakes", s.n_wakes}, {"n_questions", s.n_questions}, {"n_answers", s.n_answers}, {"n_errors", s.n_errors},
        {"last_error", s.last_error},
    };
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::vector<std::string> Assistant::recent_log(std::size_t max) const {
    std::lock_guard<std::mutex> lk(p_->log_mu);
    std::vector<std::string> out;
    const std::size_t n = std::min(max, p_->log_ring.size());
    out.reserve(n);
    for (std::size_t i = p_->log_ring.size() - n; i < p_->log_ring.size(); ++i) out.push_back(p_->log_ring[i]);
    return out;
}

bool Assistant::say(const std::string & text, const std::string & lang, std::string & err) {
    err.clear();
    if (trim(text).empty()) { err = "nothing to say"; return false; }
    if (audio::backend_name() == "none") { err = "built without an audio backend"; return false; }
    Impl::InflightGuard guard(*p_);
    if (!p_->speaker) {   // stopped: build the voice from the current config
        std::lock_guard<std::mutex> lk(p_->rt_mu);
        if (!p_->speaker) { p_->speaker = std::make_unique<Impl::Speaker>(*p_); p_->with_status([&](Status & x) { x.voice = p_->speaker->describe(); }); }
    }
    std::string l = lower(trim(lang));
    if (l.empty() || l == "auto") l = guess_language(text);
    const std::string prev = status().state;
    if (p_->running_) p_->set_state("speaking");
    auto done = std::make_shared<Impl::Speaker::Done>();
    p_->speaker->speak(text, l, done);
    {
        std::unique_lock<std::mutex> lk(done->mu);
        done->cv.wait(lk, [&] { return done->done; });
    }
    p_->speaker->idle_if_empty();
    if (p_->running_) { p_->flush_pending_ = true; if (status().state == "speaking") p_->set_state(prev == "speaking" ? "idle" : prev); }
    if (!done->ok) { err = done->err; return false; }
    return true;
}

bool Assistant::ask(const std::string & question, std::string & answer, std::string & lang, std::string & err) {
    err.clear(); answer.clear();
    if (trim(question).empty()) { err = "empty question"; return false; }
    Impl::InflightGuard guard(*p_);
    p_->build_runtime();
    const std::string prev = status().state;
    std::string reply;
    {
        std::lock_guard<std::mutex> lk(p_->chat_mu);
        if (p_->cli->cancel_requested()) p_->cli->clear_cancel();
        reply = p_->answer(question, p_->cfg.language == "auto" ? "" : p_->cfg.language);
        lang = p_->lang;
    }
    {
        const std::string cur = status().state;   // only undo what answer() set; the loop may have moved on
        if (cur == "thinking" || cur == "speaking") p_->set_state(!p_->running_ ? "off" : (prev == "thinking" || prev == "speaking") ? "idle" : prev);
    }
    if (reply.empty()) { err = p_->cli->last_error(); if (err.empty()) err = "no answer"; return false; }
    answer = reply;
    return true;
}

}  // namespace easyai
