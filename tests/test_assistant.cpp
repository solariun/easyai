// easyai-assistant model-free core (spec.md "easyai-assistant"): wake
// matcher, sentence chunker, VAD + segmenter on a synthetic signal,
// language guess, beep, WAV round trip.
#include "easyai/assistant.hpp"
#include "easyai/assistant_core.hpp"
#include "easyai/config.hpp"
#include "easyai/stt.hpp"
#include "easyai/tts.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace easyai::assistant;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } else { std::printf("ok   %s\n", #cond); } } while (0)

static std::vector<std::int16_t> tone(int ms, float amp) {
    std::vector<std::int16_t> v((std::size_t) kSampleRate * ms / 1000);
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = (std::int16_t) (amp * 32767 * std::sin(2 * 3.14159265 * 440.0 * i / kSampleRate));
    return v;
}
static std::vector<std::int16_t> noise(int ms, float amp) {
    std::vector<std::int16_t> v((std::size_t) kSampleRate * ms / 1000);
    unsigned s = 12345;
    for (auto & x : v) { s = s * 1103515245u + 12345u; x = (std::int16_t) (amp * 32767 * (((s >> 8) & 0xFFFF) / 32768.0 - 1.0)); }
    return v;
}

int main() {
    // --- normalize / levenshtein -------------------------------------------
    CHECK(normalize_text("  Olá, EASY!  Tudo bem?") == "ola easy tudo bem");
    CHECK(normalize_text("Hey, Easy. What's up?") == "hey easy what s up");
    CHECK(levenshtein("easy", "easi") == 1 && levenshtein("easy", "easy") == 0 && levenshtein("easy", "izzy") == 3);

    // --- wake word -------------------------------------------------------------
    WakeResult w = wake_match("Hey Easy, what time is it?", "easy");
    CHECK(w.matched && w.question == "what time is it");
    w = wake_match("oi easy", "easy");
    CHECK(w.matched && w.question.empty());
    w = wake_match("Ei, Easi!", "easy");                    // whisper misspelling within one edit
    CHECK(w.matched);
    w = wake_match("Easy, qual é a hora?", "easy");        // name first, no prefix word
    CHECK(w.matched && w.question == "qual e a hora");
    w = wake_match("the exam was easy today", "easy");     // name not preceded by a call word
    CHECK(!w.matched);
    w = wake_match("hey easy ok", "easy");                  // one trailing word is not a question
    CHECK(w.matched && w.question.empty());
    w = wake_match("hey jarvis what time is it", "easy");
    CHECK(!w.matched);
    // what whisper tiny / base heard for a Kokoro "Oi easy. Que horas são agora?" on the AI box
    w = wake_match("O eaz de hora são agora.", "easy", default_aliases("easy"));
    CHECK(w.matched && w.question == "de hora sao agora");
    w = wake_match("O iase que horas são agora?", "easy", default_aliases("easy"));
    CHECK(w.matched && w.question == "que horas sao agora");
    w = wake_match("Hey, easy. What time is it?", "easy");
    CHECK(w.matched && w.question == "what time is it");
    w = wake_match("Oiasse, te olha só agora.", "easy", default_aliases("easy"));   // call word glued to the name (tiny, through a speaker)
    CHECK(w.matched && w.question == "te olha so agora");
    w = wake_match("Heyeasy what time", "easy");
    CHECK(w.matched);
    // the default name "max", as whisper base / tiny heard Kokoro saying it (measured 2026-10-16)
    for (const char * t : {"Oi Max, que autoção agora.", "Oi Max como você está?", "Em Max me conta uma piada.", "Hey Max, what time is it?",
                           "Max que dia é hoje.", "hoy max como vos he estado", "É e Max me conta uma piada."}) {
        w = wake_match(t, "max", default_aliases("max"));
        if (!w.matched) std::printf("   not matched: %s\n", t);
        CHECK(w.matched && !w.question.empty());
    }
    CHECK(!wake_match("the maximum is ten", "max").matched);
    // the default name "easyai": whisper writes it as two or three words, or one odd word (measured 2026-10-16, Kokoro pt/en)
    for (const char * t : {"Hey easy AI, what time is it?", "Hey easy A, I, what day is today?", "Hey, easy-a-a-i, what day is today?",
                           "High Easy AI, tell me a joke.", "Easy AI, what is the date today?", "O Iziai que horas são agora.",
                           "Oisiai como você está!", "Oi Ziai, como você está!", "Eisiai me conta uma piada.", "Easy I me conto uma piada.",
                           "O Ijazia e qual é da te de hoje.", "E ZI que dia é hoje.", "Oi, Gisiai, como você está.", "Oi, Iziai, como você está?",
                           // with the whisper prompt "O assistente chama-se easy AI. The assistant is called easy AI."
                           "O easy AI que horas são agora.", "E easy AI me conta uma piada.", "Oi Reazi AI, qual é a data de hoje?",
                           "Easy AI, que dia é hoje.", "Oi, Zia, como você está.", "O Easy AI qual é a data de hoje?"}) {
        w = wake_match(t, "easyai", default_aliases("easyai"));
        if (!w.matched) std::printf("   not matched: %s\n", t);
        CHECK(w.matched && !w.question.empty());
    }
    w = wake_match("Hey easy AI, what time is it?", "easyai");
    CHECK(w.matched && w.question == "what time is it");               // the joined words are the name, not the question
    w = wake_match("Hey easy a i what day is today", "easyai");
    CHECK(w.matched && w.question == "what day is today");             // three-word join
    CHECK(!wake_match("it was an easy day", "easyai").matched);        // "easy" alone without a call word
    CHECK(!wake_match("this ai is easy", "easyai").matched);
    CHECK(spoken_name("easyai", "en") == "easy AI" && spoken_name("easyai", "pt") == "ísi ai" && spoken_name("max", "en") == "max");
    // strict call words (the default since 2026-10-17): "hello max" / "olá max", as whisper tiny/base wrote Kokoro saying them
    const auto calls = default_call_words();
    for (const char * t : {"Hello Max, what time is it?", "Hello Max, tell me a joke.", "Olá Max como você está?", "Olá Max, que dia é hoje?",
                           "Elo Max qual é a data de hoje?", "Elomax qual é a data de hoje?", "Allo Max mi conto ma piade.", "A lo Max me conta uma piada."}) {
        w = wake_match(t, "max", default_aliases("max"), calls);
        if (!w.matched) std::printf("   not matched: %s\n", t);
        CHECK(w.matched && !w.question.empty());
    }
    w = wake_match("Hello Max, what time is it?", "max", {}, calls);
    CHECK(w.matched && w.question == "what time is it");
    CHECK(!wake_match("Mas eu acho que o máximo é 10 mais ou menos.", "max", default_aliases("max"), calls).matched);
    CHECK(!wake_match("The maximum is 10 and max out the volume.", "max", default_aliases("max"), calls).matched);
    CHECK(!wake_match("Max, what time is it?", "max", {}, calls).matched);            // bare name: no wake in strict mode
    CHECK(!wake_match("hey max what time is it", "max", {}, calls).matched);          // "hey" is not a call word now
    CHECK(wake_match("oi max que horas", "max", {}, {"oi"}).matched);                 // operator's own list
    CHECK(wake_match("hey max what time is it", "max").matched);                      // empty list = loose mode
    // processing sound and microphone gain
    {
        const auto th = make_think_sound(kSampleRate);
        CHECK(th.size() == (std::size_t) 3 * kSampleRate * kThinkToneMs / 1000);
        CHECK(th.front() == 0 && std::abs(th[th.size() / 6]) > 2000);
        std::vector<std::int16_t> g = {1000, -1000, 30000};
        apply_gain_db(g, 6);
        CHECK(std::abs(g[0] - 1995) <= 2 && g[1] == -g[0] && g[2] == 32767);         // +6 dB doubles, clipped
        apply_gain_db(g, -6); CHECK(std::abs(g[0] - 1000) <= 2);
        std::vector<std::int16_t> z = {123}; apply_gain_db(z, 0); CHECK(z[0] == 123);
        std::vector<std::int16_t> big = {100}; apply_gain_db(big, 90); CHECK(big[0] == 3162);  // bounded to +30 dB
    }
    w = wake_match("oi izi", "easy", {"izi"});                    // operator alias
    CHECK(w.matched);
    w = wake_match("the exam was easy today", "easy", default_aliases("easy"));
    CHECK(!w.matched);
    CHECK(wake_match("hey mix", "max").matched && !wake_match("hey mixt", "max").matched);   // short names keep 1 edit

    // --- sentence chunker -------------------------------------------------------
    {
        SentenceChunker c; std::string s; std::vector<std::string> out;
        for (const char * piece : {"The time ", "is 3.14 past noon, e.g. ", "soon. Second sentence here", "! Third:", " part\nfourth"}) {
            c.push(piece);
            while (c.pop(s)) out.push_back(s);
        }
        CHECK(out.size() == 2);
        CHECK(out.size() > 0 && out[0] == "The time is 3.14 past noon, e.g. soon.");
        // "Second sentence here!" is under kMinSentenceChars, so it rides with the next clause up to the newline
        CHECK(out.size() > 1 && out[1] == "Second sentence here! Third: part");
        CHECK(c.flush() == "fourth");
        CHECK(c.flush().empty());
        SentenceChunker d; d.push("Ok."); CHECK(!d.pop(s) && d.flush() == "Ok.");   // too short to cut mid-stream, flushed at the end
    }

    // --- VAD + segmenter on silence → speech → silence ---------------------------
    {
        Vad vad; Segmenter seg(kBurstEndMs, kWakeMaxMs);
        auto sil = noise(2000, 0.002f), sp = tone(800, 0.3f), sil2 = noise(1000, 0.002f);
        std::vector<std::int16_t> all; all.insert(all.end(), sil.begin(), sil.end()); all.insert(all.end(), sp.begin(), sp.end()); all.insert(all.end(), sil2.begin(), sil2.end());
        const std::size_t per = (std::size_t) kSampleRate * kPeriodMs / 1000;
        int bursts = 0; std::size_t burst_len = 0; int speech_periods = 0;
        for (std::size_t off = 0; off + per <= all.size(); off += per) {
            const bool speech = vad.feed(all.data() + off, per);
            speech_periods += speech;
            if (seg.push(all.data() + off, per, speech)) { ++bursts; burst_len = seg.take().size(); }
        }
        CHECK(bursts == 1);
        // burst = preroll (300 ms) + 800 ms tone + 500 ms end silence, ±one period
        const std::size_t expect = (std::size_t) kSampleRate * (kPrerollMs + 800 + kBurstEndMs) / 1000;
        CHECK(burst_len >= expect - per && burst_len <= expect + per);
        CHECK(speech_periods >= 38 && speech_periods <= 41);   // 800 ms / 20 ms
        CHECK(vad.floor_db() < -40);
        // constant room noise from the first period: the floor starts there, nothing is speech
        Vad noisy; Segmenter seg2(kBurstEndMs, kWakeMaxMs); auto hum = noise(3000, 0.05f); int sp2 = 0, b2 = 0;
        for (std::size_t off = 0; off + per <= hum.size(); off += per) { const bool s = noisy.feed(hum.data() + off, per); sp2 += s; if (seg2.push(hum.data() + off, per, s)) ++b2; }
        CHECK(sp2 == 0 && b2 == 0 && noisy.floor_db() > -35);
        // a click (one 20 ms period of "speech") is not a burst
        Vad v3; Segmenter seg3(kBurstEndMs, kWakeMaxMs); int b3 = 0;
        auto quiet = noise(1500, 0.002f); auto click = tone(20, 0.5f);
        std::vector<std::int16_t> seq; seq.insert(seq.end(), quiet.begin(), quiet.end()); seq.insert(seq.end(), click.begin(), click.end()); seq.insert(seq.end(), quiet.begin(), quiet.end());
        for (std::size_t off = 0; off + per <= seq.size(); off += per) { const bool s = v3.feed(seq.data() + off, per); if (seg3.push(seq.data() + off, per, s)) ++b3; }
        CHECK(b3 == 0);
    }

    // --- hallucination guard ---------------------------------------------------
    CHECK(looks_like_question("Como você está?") && looks_like_question("what time is it"));
    CHECK(!looks_like_question("*crash*") && !looks_like_question("phase") && !looks_like_question("[BLANK_AUDIO]") && !looks_like_question("(music playing)"));
    CHECK(!looks_like_question("") && looks_like_question("sim senhor"));

    // --- language guess ------------------------------------------------------------
    CHECK(guess_language("Qual é a previsão do tempo para hoje?") == "pt");
    CHECK(guess_language("What is the weather like today?") == "en");
    CHECK(guess_language("xyz") == "en");
    // detect_language: the voice follows the text (rule 6) and a wrong-language answer is caught (rule 5)
    CHECK(detect_language("Conte até três.") == "pt");                                   // accents + numbers (the dashboard ask that came out en before)
    CHECK(detect_language("Um, dois, três.") == "pt");
    CHECK(detect_language("São onze e cinquenta e dois da manhã.") == "pt");
    CHECK(detect_language("Vou consultar a ferramenta de data e hora.") == "pt");
    CHECK(detect_language("Hoje é sábado, quinze de novembro.") == "pt");
    CHECK(detect_language("Today is Saturday, November fifteenth, twenty twenty-six.") == "en");
    CHECK(detect_language("Let me check the web.") == "en");
    CHECK(detect_language("What day is it today?") == "en");
    CHECK(detect_language("It is eleven fifty in the morning.") == "en");
    CHECK(detect_language("xyz") == "" && detect_language("") == "" && detect_language("42") == "");   // no evidence
    CHECK(detect_language("ok") == "" && detect_language("no") == "");                               // words both languages use count for neither

    // --- /proc/asound/pcm: the AI box (HDMI-only card 0, analog card 1 with capture) ---
    {
        auto d = parse_asound_pcm("00-03: HDMI 0 : HDMI 0 : playback 1\n00-07: HDMI 1 : HDMI 1 : playback 1\n"
                                  "01-00: ALC245 Analog : ALC245 Analog : playback 1 : capture 1\n");
        CHECK(d.size() == 3 && !d[0].capture && d[0].playback && d[0].card == 0 && d[0].dev == 3);
        CHECK(d[2].capture && d[2].playback && d[2].name == "ALC245 Analog" && d[2].alsa_name() == "plughw:1,0");
        CHECK(parse_asound_pcm("garbage\n").empty());
    }

    // --- beep + WAV round trip ---------------------------------------------------
    {
        auto off = make_off_sound(kSampleRate);
        CHECK(off.size() == (std::size_t) kSampleRate * 2 * kOffToneMs / 1000 && off.front() == 0);
        auto b = make_beep(kSampleRate, 2);
        CHECK(b.size() == (std::size_t) kSampleRate * (2 * kBeepMs + kBeepGapMs) / 1000);
        CHECK(b.front() == 0 && std::abs((int) b[b.size() / 8]) > 1000);   // half-sine envelope: starts at zero, loud inside
        std::string wav = easyai::encode_wav_pcm16(b.data(), b.size(), kSampleRate);
        std::vector<float> pcm; std::string err;
        CHECK(easyai::decode_wav(wav, pcm, err) && pcm.size() == b.size());
        CHECK(std::fabs(pcm[b.size() / 8] - b[b.size() / 8] / 32768.0f) < 1e-4f);
        std::vector<std::int16_t> back; int rate = 0;
        CHECK(easyai::decode_wav_pcm16(wav, back, rate) && rate == kSampleRate && back == b);
        CHECK(!easyai::decode_wav_pcm16("not a wav", back, rate));
    }

    {   // whisper echoing its own prompt on audio it cannot make out (ai.local, 2026-10-21)
        const std::string prompt = "O assistente chama-se max. The assistant is called max.";
        CHECK(drop_prompt_echo("The assistant is called max. The assistant is called max. The assistant is called max.", prompt).empty());
        CHECK(drop_prompt_echo(" The assistant is called Max! O assistente chama-se Max.", prompt).empty());
        CHECK(drop_prompt_echo("Hello Max, what time is it?", prompt) == "Hello Max, what time is it.");
        CHECK(drop_prompt_echo("The assistant is called max. Hello Max, what time is it?", prompt) == "Hello Max, what time is it.");
        CHECK(drop_prompt_echo("[BLANK_AUDIO]", prompt) == "[BLANK_AUDIO].");
        CHECK(drop_prompt_echo("Olá Max, como você está?", "").rfind("Olá Max", 0) == 0);
        CHECK(wake_match(drop_prompt_echo("The assistant is called max. Hello Max, what time is it?", prompt), "max", default_aliases("max"), default_call_words()).matched);
        CHECK(!wake_match(drop_prompt_echo("The assistant is called max. The assistant is called max.", prompt), "max", default_aliases("max"), default_call_words()).matched);
    }


    // --- easyai::Assistant option table -----------------------------------------------
    {
        using A = easyai::Assistant;
        const auto & opts = A::options();
        std::set<std::string> keys, flags;
        bool all_help = true, all_group = true, roundtrip = true, defaults_match = true, prefix = true;
        A::Config def;
        for (const auto & o : opts) {
            keys.insert(o.key); flags.insert(o.flag);
            if (o.help.empty()) all_help = false;
            if (o.group.empty()) all_group = false;
            if (A::key_of_flag(o.flag) != o.key) roundtrip = false;
            if (A::get(def, o.key) != o.def) { defaults_match = false; std::printf("   default mismatch %s: '%s' vs '%s'\n", o.key.c_str(), A::get(def, o.key).c_str(), o.def.c_str()); }
            if (o.flag.rfind("--assistant-", 0) != 0 || o.flag.find('_') != std::string::npos) prefix = false;
        }
        CHECK(opts.size() == 31);
        CHECK(keys.size() == opts.size());
        CHECK(flags.size() == opts.size());
        CHECK(all_help && all_group && roundtrip && defaults_match && prefix);
        CHECK(A::key_of_flag("--assistant-mic-gain-db") == "mic_gain_db" && A::key_of_flag("--mic") == "");
        const std::set<std::string> groups = {"General", "Wake word", "Audio", "Voice", "Timing", "Brain", "Advanced"};
        bool groups_ok = true;
        for (const auto & o : opts) if (!groups.count(o.group)) groups_ok = false;
        CHECK(groups_ok);
        // the closed sets and the ranges
        auto opt = [&](const char * k) { for (const auto & o : opts) if (o.key == k) return o; return A::Option(); };
        CHECK(opt("greeting").choices == "en|pt|off" && opt("language").choices == "auto|pt|en" && opt("tts").choices == "auto|server|kokoro|piper|espeak|say");
        CHECK(opt("pause_ms").min == 300 && opt("pause_ms").max == 10000 && opt("max_listen_ms").min == 2000 && opt("max_listen_ms").max == 120000);
        CHECK(opt("followup_ms").min == 0 && opt("followup_ms").max == 30000 && opt("mic_gain_db").min == -30 && opt("mic_gain_db").max == 30);
        CHECK(opt("history_turns").min == 0 && opt("history_turns").max == 50 && opt("history_idle_s").min == 10 && opt("history_idle_s").max == 86400);
        CHECK(opt("enabled").type == A::Option::Bool && opt("pause_ms").type == A::Option::Int && opt("name").type == A::Option::Str);
        CHECK(opt("wake_model").def == "server" && opt("endpoint").def.empty() && opt("tts").def == "auto");

        // set(): validation
        A::Config c; std::string err;
        CHECK(A::set(c, "pause_ms", "2000", err) && c.pause_ms == 2000 && err.empty());
        CHECK(!A::set(c, "pause_ms", "abc", err) && !err.empty());
        CHECK(!A::set(c, "pause_ms", "12x", err));
        CHECK(!A::set(c, "pause_ms", "100", err) && err.find("range") != std::string::npos && c.pause_ms == 2000);
        CHECK(!A::set(c, "pause_ms", "10001", err));
        CHECK(A::set(c, "followup_ms", "0", err) && c.followup_ms == 0);
        CHECK(A::set(c, "mic_gain_db", "-30", err) && c.mic_gain_db == -30 && !A::set(c, "mic_gain_db", "31", err));
        CHECK(!A::set(c, "greeting", "fr", err) && err.find("en pt off") != std::string::npos);
        CHECK(A::set(c, "greeting", "PT", err) && c.greeting == "pt");
        CHECK(!A::set(c, "tts", "festival", err) && A::set(c, "tts", "kokoro", err) && c.tts == "kokoro");
        CHECK(!A::set(c, "no_such_key", "1", err) && err.find("unknown") != std::string::npos);
        for (const char * t : {"on", "1", "true", "yes", "ON", "Yes"}) { CHECK(A::set(c, "enabled", t, err) && c.enabled); }
        for (const char * f : {"off", "0", "false", "no", "OFF", "No"}) { CHECK(A::set(c, "enabled", f, err) && !c.enabled); }
        CHECK(!A::set(c, "verbose", "maybe", err) && !err.empty());
        CHECK(A::set(c, "name", "easy", err) && c.name == "easy" && A::get(c, "name") == "easy");
        CHECK(A::get(c, "enabled") == "off" && A::get(c, "followup_ms") == "0" && A::get(c, "nope").empty());

        // to_map covers every option
        const auto m = A::to_map(c);
        CHECK(m.size() == opts.size());
        bool covered = true;
        for (const auto & o : opts) if (!m.count(o.key) || m.at(o.key) != A::get(c, o.key)) covered = false;
        CHECK(covered && m.at("name") == "easy" && m.at("tts") == "kokoro");

        // from_ini: the INI over the defaults; a bad value keeps the default
        easyai::config::Ini ini;
        ini.sections["ASSISTANT"]["name"] = "jarvis";
        ini.sections["ASSISTANT"]["pause_ms"] = "900";
        ini.sections["ASSISTANT"]["enabled"] = "yes";
        ini.sections["ASSISTANT"]["mic_gain_db"] = "99";       // out of range → default 0
        ini.sections["ASSISTANT"]["greeting"] = "pt";
        ini.sections["SERVER"]["name"] = "ignored";
        const A::Config fi = A::from_ini(ini);
        CHECK(fi.name == "jarvis" && fi.pause_ms == 900 && fi.enabled && fi.mic_gain_db == 0 && fi.greeting == "pt");
        CHECK(fi.max_listen_ms == def.max_listen_ms && fi.tts == "auto" && fi.wake_model == "server");
        CHECK(A::from_ini(easyai::config::Ini()).name == "max");
    }

    // --- config::set_ini_values ------------------------------------------------------------
    {
        using easyai::config::set_ini_values;
        std::string err;
        const std::string base =
            "# easyai\n"
            "[SERVER]\n"
            "port = 8080\n"
            "\n"
            "[ASSISTANT]\n"
            "# the voice assistant\n"
            "name      = max\n"
            "# endpoint = (this server)\n"
            "; mic = default\n"
            "pause_ms=1500\n"
            "\n"
            "\n"
            "[TTS]\n"
            "engine = auto\n";
        std::string t = base;
        CHECK(set_ini_values(t, "ASSISTANT", {{"name", "easy"}, {"pause_ms", "900"}}, err) && err.empty());
        CHECK(t.find("name      = easy\n") != std::string::npos);                     // replaced in place, alignment kept
        CHECK(t.find("pause_ms=900\n") != std::string::npos);                          // no spaces around '=' stays that way
        CHECK(t.find("# the voice assistant\n") != std::string::npos);                 // comments untouched
        CHECK(t.find("port = 8080\n") != std::string::npos && t.find("engine = auto\n") != std::string::npos);
        t = base;
        CHECK(set_ini_values(t, "ASSISTANT", {{"endpoint", "http://x:1"}, {"mic", "plughw:1,0"}}, err));
        CHECK(t.find("endpoint = http://x:1\n") != std::string::npos && t.find("# endpoint") == std::string::npos);   // un-commented '#'
        CHECK(t.find("mic = plughw:1,0\n") != std::string::npos && t.find("; mic") == std::string::npos);            // un-commented ';'
        t = base;
        CHECK(set_ini_values(t, "ASSISTANT", {{"speaker", "plughw:1,0"}}, err));
        CHECK(t.find("pause_ms=1500\nspeaker = plughw:1,0\n\n\n[TTS]\n") != std::string::npos);   // appended before the blank lines
        {   // every other section byte-identical
            const auto head = t.substr(0, t.find("[ASSISTANT]"));
            const auto tail = t.substr(t.find("[TTS]"));
            CHECK(head == base.substr(0, base.find("[ASSISTANT]")) && tail == base.substr(base.find("[TTS]")));
        }
        t = base;
        CHECK(set_ini_values(t, "STT", {{"model", "/m/ggml-base.bin"}}, err));
        CHECK(t.rfind("engine = auto\n\n[STT]\nmodel = /m/ggml-base.bin\n") != std::string::npos && t.rfind("[STT]") > t.rfind("[TTS]"));   // created at the end
        CHECK(t.substr(0, base.size()) == base);
        t = base;
        CHECK(!set_ini_values(t, "ASSISTANT", {{"name", "two\nlines"}}, err) && !err.empty() && t == base);   // newline rejected, text untouched
        t = "";
        CHECK(set_ini_values(t, "ASSISTANT", {{"enabled", "on"}}, err) && t == "[ASSISTANT]\nenabled = on\n");
        t = "[ASSISTANT]\nname = max";   // no trailing newline, section at EOF
        CHECK(set_ini_values(t, "ASSISTANT", {{"name", "easy"}, {"tts", "auto"}}, err) && t == "[ASSISTANT]\nname = easy\ntts = auto");
        t = "[ASSISTANT]\r\nname = max\r\n";
        CHECK(set_ini_values(t, "ASSISTANT", {{"name", "easy"}}, err) && t == "[ASSISTANT]\r\nname = easy\r\n");   // CRLF kept
        t = "[A]\nk = 1\n[ASSISTANT]\nk = 2\n";
        CHECK(set_ini_values(t, "ASSISTANT", {{"k", "3"}}, err) && t == "[A]\nk = 1\n[ASSISTANT]\nk = 3\n");       // same key in another section untouched
    }

    // --- audio::list_devices: never crashes, "default" first --------------------------------
    {
        std::string err;
        const auto devs = easyai::audio::list_devices(err);
        if (easyai::audio::backend_name() == "none") {
            CHECK(devs.empty() && !err.empty());
        } else {
            CHECK(!devs.empty() && devs[0].id == "default" && devs[0].is_default && err.empty());
            std::set<std::string> ids; bool unique = true, one_default = true;
            for (std::size_t i = 0; i < devs.size(); ++i) { if (!ids.insert(devs[i].id).second) unique = false; if (i > 0 && devs[i].is_default) one_default = false; }
            CHECK(unique && one_default);
            for (const auto & d : devs) std::printf("   device %-28s cap=%d play=%d card=%s name=%s\n", d.id.c_str(), d.capture, d.playback, d.card.c_str(), d.name.c_str());
        }
    }

    // --- Kokoro voice catalogue -------------------------------------------------------------
    {
        const auto & cat = easyai::kokoro_voice_catalog();
        std::set<std::string> ids;
        bool shape = true;
        for (const auto & v : cat) {
            ids.insert(v.id);
            if (v.id.size() < 4 || v.id[2] != '_' || (v.gender != "m" && v.gender != "f") || v.lang.empty()) shape = false;
            if ((v.id[1] == 'm') != (v.gender == "m")) shape = false;
        }
        CHECK(cat.size() == 54 && ids.size() == 54 && shape);
        CHECK(ids.count("am_michael") && ids.count("pm_alex") && ids.count("pf_dora") && ids.count("af_heart"));
        const std::string m = easyai::kokoro_best_voice("en-us", "m");
        CHECK(m.rfind("am_", 0) == 0);
        CHECK(easyai::kokoro_best_voice("pt-br", "m").rfind("pm_", 0) == 0 && easyai::kokoro_best_voice("pt", "f") == "pf_dora");
        CHECK(easyai::kokoro_best_voice("xx", "m").empty());
    }

    // --- Assistant object: status while stopped, say() without a voice is an error, not a crash -----
    {
        easyai::Assistant as;
        CHECK(!as.running() && as.status().state == "off" && as.status_json().find("\"state\":\"off\"") != std::string::npos);
        CHECK(as.recent_log().empty());
        std::string err;
        CHECK(!as.say("", "en", err) && !err.empty());            // nothing to say
        easyai::Assistant::Config c; c.tts = "piper"; c.piper = "/nonexistent/piper"; c.speaker = "null";
        as.configure(c, easyai::Assistant::Hooks{});
        err.clear();
        CHECK(!as.say("hi", "en", err) && !err.empty());          // no usable voice: an error, not a crash
        CHECK(!as.running() && as.status().state == "off");
    }

    // --- set_ini_values: header on the last line, duplicate keys, insertion before a banner ---
    {
        std::string err, t = "[SERVER]\nport=1\n[ASSISTANT]\n";
        CHECK(easyai::config::set_ini_values(t, "ASSISTANT", {{"enabled", "on"}}, err) && t == "[SERVER]\nport=1\n[ASSISTANT]\nenabled = on\n");
        t = "[ASSISTANT]\nname = a\nname = b\n";
        CHECK(easyai::config::set_ini_values(t, "ASSISTANT", {{"name", "easy"}}, err) && t == "[ASSISTANT]\nname = easy\nname = easy\n");
        t = "[ASSISTANT]\nverbose = 0\n\n# ====\n# [ENGINE] — tunables\n# ====\n[ENGINE]\nngl = 99\n";
        CHECK(easyai::config::set_ini_values(t, "ASSISTANT", {{"tls_insecure", "on"}}, err) &&
              t == "[ASSISTANT]\nverbose = 0\ntls_insecure = on\n\n# ====\n# [ENGINE] — tunables\n# ====\n[ENGINE]\nngl = 99\n");
    }

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
