// easyai-assistant — the model-free pieces (spec.md "easyai-assistant"):
// energy VAD + burst segmenter, wake-word matcher, sentence chunker for
// streamed speech, language guess, beep. All pure, all unit-tested.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace easyai {
namespace assistant {

constexpr int    kSampleRate      = 16000;
constexpr int    kPeriodMs        = 20;
constexpr int    kPrerollMs       = 300;    // audio kept before the first speech period
constexpr int    kBurstEndMs      = 500;    // idle mode: silence that ends a burst
constexpr int    kWakeMaxMs       = 4000;   // idle mode: longest burst sent to the wake model
constexpr double kSpeechMarginDb  = 10.0;   // speech = RMS above floor + margin
constexpr double kFloorRiseDbPerS = 1.0;    // floor tracks upwards slowly, downwards at once
constexpr double kFloorInitDb     = -50.0;
constexpr double kFloorMinDb      = -70.0;  // digital silence must not pull the floor to -inf
constexpr int    kNameMaxEdits    = 1;      // Levenshtein tolerance on the wake name
constexpr int    kMinSentenceChars = 24;
constexpr int    kBeepHz          = 880;
constexpr int    kBeepMs          = 120;
constexpr int    kBeepGapMs       = 80;

double rms_db(const std::int16_t * pcm, std::size_t n);

// Speech / silence decision per period with an adaptive noise floor.
class Vad {
public:
    bool   feed(const std::int16_t * pcm, std::size_t n);   // true = speech
    double floor_db() const { return floor_db_; }
    double last_db() const { return last_db_; }
private:
    double floor_db_ = kFloorInitDb;
    double last_db_  = kFloorInitDb;
};

// Collects one burst: preroll + speech periods, ends after end_silence_ms
// of non-speech or at max_ms. push() returns true when a burst is ready
// in take().
class Segmenter {
public:
    Segmenter(int end_silence_ms, int max_ms);
    bool push(const std::int16_t * pcm, std::size_t n, bool speech);
    std::vector<std::int16_t> take();
    bool in_burst() const { return in_burst_; }
    void reset();
private:
    int  end_silence_ms_, max_ms_;
    bool in_burst_ = false;
    int  silence_ms_ = 0, burst_ms_ = 0;
    std::vector<std::int16_t> preroll_, burst_, ready_;
};

// lower-case, accents folded (á→a, ç→c …), punctuation → space, single spaces.
std::string normalize_text(const std::string & s);
int levenshtein(const std::string & a, const std::string & b);

struct WakeResult {
    bool        matched = false;
    std::string question;   // words after the name (empty = just the call)
};
// "hey easy what time is it" → matched, question "what time is it".
// Accepts hey|hi|oi|ei|ok|ola|alo before the name, name within kNameMaxEdits.
WakeResult wake_match(const std::string & transcript, const std::string & name);

// Cuts streamed text into sentences for TTS without waiting for the end.
class SentenceChunker {
public:
    void push(const std::string & piece);
    bool pop(std::string & sentence);     // next complete sentence, if any
    std::string flush();                  // whatever is left (end of answer)
    void clear() { buf_.clear(); }
private:
    std::string buf_;
};

// "pt" or "en" from common function words; "en" when undecided.
std::string guess_language(const std::string & text);

// count beeps of kBeepHz/kBeepMs with kBeepGapMs gaps, PCM16 at rate_hz.
std::vector<std::int16_t> make_beep(int rate_hz, int count);

}  // namespace assistant
}  // namespace easyai
