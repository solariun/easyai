// easyai-assistant model-free core (spec.md "easyai-assistant"): wake
// matcher, sentence chunker, VAD + segmenter on a synthetic signal,
// language guess, beep, WAV round trip.
#include "easyai/assistant_core.hpp"
#include "easyai/stt.hpp"
#include "easyai/tts.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
