// easyai-assistant model-free core (spec.md "easyai-assistant"): wake
// matcher, sentence chunker, VAD + segmenter on a synthetic signal,
// language guess, beep, WAV round trip.
#include "easyai/assistant_core.hpp"
#include "easyai/stt.hpp"
#include "easyai/tts.hpp"
#include <cmath>
#include <cstdio>
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
    }

    // --- language guess ------------------------------------------------------------
    CHECK(guess_language("Qual é a previsão do tempo para hoje?") == "pt");
    CHECK(guess_language("What is the weather like today?") == "en");
    CHECK(guess_language("xyz") == "en");

    // --- beep + WAV round trip ---------------------------------------------------
    {
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
