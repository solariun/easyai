// easyai — the voice assistant, built into easyai-server (spec.md
// "easyai-assistant — voice assistant service", rules 13-16).
//
//   mic → VAD → wake word (local whisper) → listen until a pause →
//   transcription (the server's own Stt, or an STT endpoint) →
//   POST /v1/chat/completions (stream, tools run here) →
//   speech (the server's own Tts, or a TTS endpoint, or a local engine) →
//   speaker
//
// Until 2026-11 this was a separate program (easyai-assistant) talking to
// the server over HTTP. It is now a class owned by easyai-server: one
// process, one INI section ([ASSISTANT]), audio and voice shared with
// POST /v1/audio/*, and a dashboard panel (/models → Assistant) that
// lists the audio devices and the voices, edits every key and starts /
// stops it live. Chat still goes through easyai::Client to `endpoint`
// (this server by default: OpenAI-shaped, so the INI can point it at
// llama-server or OpenAI instead), so tools run inside the assistant.
//
// The model-free pieces (VAD, segmenter, wake matcher, chunker, sounds)
// stay in assistant_core.hpp.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "easyai/audio_io.hpp"

namespace easyai {
namespace config { struct Ini; }


class Assistant {
public:
    // ---- configuration: the [ASSISTANT] section ---------------------------
    // Every field is one INI key (same name) and one dashboard control.
    // Empty endpoint = "this server" (the owner fills the loopback URL, its
    // API key and tls_insecure); stt_endpoint / tts_endpoint empty = the
    // owner's own Stt / Tts through Hooks, else that URL.
    struct Config {
        bool        enabled = false;                 // [ASSISTANT] enabled: start with the server
        std::string name = "max";
        std::string call_words = "hello, olá, alô, hallo, hola";
        std::string name_aliases;
        std::string greeting = "en";                 // en | pt | off
        std::string language = "auto";               // auto | pt | en
        std::string endpoint;                        // "" = this server
        std::string api_key;                         // "" = the server's own key
        std::string model = "default";
        std::string stt_endpoint, tts_endpoint;      // "" = in-process (Hooks)
        int         pause_ms = 1500, max_listen_ms = 20000, followup_ms = 2000;
        std::string wake_model = "server";           // whisper ggml path, or "server" = every burst through STT
        std::string mic = "default", speaker = "default";
        int         mic_gain_db = 0;
        std::string tts = "auto";                    // auto | server | kokoro | piper | espeak | say
        std::string voice_pt, voice_en, piper = "piper";
        std::string kokoro_model, kokoro_voices;     // local Kokoro when tts is not the server
        std::string tools = "datetime,web", external_tools;
        std::string system;                          // file path or inline text; "" = built-in
        int         history_turns = 6, history_idle_s = 300;
        bool        verbose = false, tls_insecure = false;
    };

    // The option table: one row per Config field. key = INI key; flag =
    // the easyai-server flag ("--assistant-<key-with-dashes>"); type;
    // def = default as text; choices = "|"-separated allowed values for
    // Str options with a closed set ("" = free text); min/max for Int;
    // group = dashboard section (General, Wake word, Audio, Voice, Timing,
    // Brain, Advanced); help = one line.
    struct Option {
        enum Type { Str, Int, Bool };
        std::string key, flag;
        Type        type = Str;
        std::string def;
        std::string choices;
        int         min = 0, max = 0;
        std::string group, help;
    };
    static const std::vector<Option> & options();

    // Read every [ASSISTANT] key found in `ini` over the defaults.
    static Config from_ini(const config::Ini & ini);
    // Set one key from text with validation (type, range, choices).
    // Unknown key, bad number, out of range or unknown choice → false + err.
    static bool set(Config & c, const std::string & key, const std::string & value, std::string & err);
    // One key as text ("on"/"off" for Bool).
    static std::string get(const Config & c, const std::string & key);
    // Every key → text, in option order.
    static std::map<std::string, std::string> to_map(const Config & c);
    // key ↔ flag: "--assistant-mic-gain-db" → "mic_gain_db".
    static std::string key_of_flag(const std::string & flag);

    // ---- what the owner (easyai-server) lends ------------------------------
    struct Hooks {
        // In-process transcription (16 kHz mono PCM16). prompt = whisper
        // initial prompt. Set `lang` to the detected language. Return false
        // with err when no model is loaded (the assistant then uses
        // stt_endpoint, or `endpoint`). null → HTTP only.
        std::function<bool(const std::vector<std::int16_t> & pcm, const std::string & language,
                           const std::string & prompt, std::string & text, std::string & lang, std::string & err)> stt;
        // In-process speech: voice is "pt" or "en"; PCM16 mono + rate.
        std::function<bool(const std::string & text, const std::string & voice,
                           std::vector<std::int16_t> & pcm, int & rate_hz, std::string & err)> tts;
        // One line of log (no newline). null → stderr "[assistant] …".
        // Every line also lands in recent_log().
        std::function<void(const std::string & line)> log;
        // Called with the loopback base URL / key to use when
        // Config::endpoint is empty. Set by easyai-server.
        std::string self_endpoint, self_api_key;
        bool        self_tls_insecure = false;
    };

    // ---- runtime ---------------------------------------------------------------
    struct Status {
        // off | starting | no_mic | idle | listening | thinking | speaking | conversation | error
        std::string state = "off";
        bool        running = false;
        std::int64_t since = 0;            // unix seconds the current run started; 0 when off
        std::string mic, speaker, backend; // devices in use ("plughw:1,0 (ALC245 Analog)"), "alsa"
        std::string wake_model;            // path, "server", or "none"
        std::string voice;                 // "server (in-process), local piper as fallback", "local piper", "NONE (…)"
        std::string tools;                 // "datetime, web"
        double      level_db = -100, floor_db = -100;
        bool        speech = false;
        std::string last_heard;    std::int64_t last_heard_at = 0;      // wake model transcript
        std::string last_question; std::string last_question_lang; std::int64_t last_question_at = 0;
        std::string last_answer;   std::int64_t last_answer_at = 0;
        std::uint64_t n_wakes = 0, n_questions = 0, n_answers = 0, n_errors = 0;
        std::string last_error;
    };

    Assistant();
    ~Assistant();
    Assistant(const Assistant &)             = delete;
    Assistant & operator=(const Assistant &) = delete;

    // Starts the listening thread. Returns false with err only when the
    // configuration is unusable (e.g. built without an audio backend);
    // a missing microphone is NOT an error: the thread retries every
    // kMicRetryS and the state reads "no_mic". Calling start() while
    // running restarts with the new config.
    bool start(const Config & cfg, const Hooks & hooks, std::string & err);
    // Sets the configuration and hooks that say() and ask() use while the
    // assistant is stopped, without opening the microphone or starting the
    // thread (the dashboard's "Test voice" on a disabled assistant, the
    // installer's --assistant-say check). No effect while running: start()
    // owns the config then.
    void configure(const Config & cfg, const Hooks & hooks);
    // Stops the thread and waits for it: a running answer is cancelled
    // (Client::request_cancel), a playing sentence finishes.
    void stop();
    bool running() const;

    const Config & config() const;     // the config of the current / last run
    Status      status() const;
    std::string status_json() const;   // Status as a JSON object (same field names)
    // The last `max` log lines (newest last), kept in a ring of kLogKeep.
    std::vector<std::string> recent_log(std::size_t max = 200) const;

    // Speak one text now, on the configured speaker, with the configured
    // voice (lang "pt"/"en"/"auto"). Works while running (the microphone
    // is muted meanwhile) and while stopped (opens the speaker for the
    // clip). Blocking. Used by the dashboard's "Test voice" and the
    // installer's check.
    bool say(const std::string & text, const std::string & lang, std::string & err);
    // Answer a written question through the same chat + voice path as a
    // spoken one (the dashboard's "Ask"); the answer is spoken and
    // returned. Blocking. Works while running or stopped.
    bool ask(const std::string & question, std::string & answer, std::string & lang, std::string & err);

    static constexpr int kLogKeep = 400;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Kokoro-82M voice catalogue (hexgrad VOICES.md, v1.0): id → language tag
// (pt-br, en-us, en-gb, es, fr, hi, it, ja, zh), gender (m/f) and the
// upstream quality grade ("A", "C+", … "" when ungraded). Static table,
// no model needed; the dashboard shows it next to the voices the loaded
// model really has.
struct KokoroVoiceInfo {
    std::string id, lang, gender, grade;
};
const std::vector<KokoroVoiceInfo> & kokoro_voice_catalog();
// Best-graded voice of a language and gender ("en-us","m" → am_michael).
std::string kokoro_best_voice(const std::string & lang, const std::string & gender);

namespace config {
// Rewrite the values of `section` in an INI text: an existing `key = …`
// line is replaced in place (a commented-out `# key = …` / `; key = …`
// line is un-commented and replaced), a missing key is appended at the
// end of the section (before the blank lines that precede the next
// header), a missing section is appended at the end. Comments and every
// other line are untouched. Values must not contain a newline (false +
// err). Keys are matched case-sensitively, whitespace around `=` kept
// aligned with the line it replaces.
bool set_ini_values(std::string & text, const std::string & section,
                    const std::map<std::string, std::string> & values, std::string & err);
}  // namespace config

}  // namespace easyai
