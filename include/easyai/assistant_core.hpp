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
constexpr int    kNameMaxEdits    = 1;      // Levenshtein tolerance on the wake name (short names)
constexpr int    kNameMaxEditsLong = 2;     //   names of kLongNameChars letters or more
constexpr int    kLongNameChars   = 4;
constexpr int    kMinSpeechMs     = 200;    // a burst with less real speech than this is noise, never transcribed
constexpr int    kMinSentenceChars = 24;
constexpr int    kBeepHz          = 880;
constexpr int    kBeepMs          = 120;
constexpr int    kBeepGapMs       = 80;

double rms_db(const std::int16_t * pcm, std::size_t n);

// Speech / silence decision per period with an adaptive noise floor. The
// floor starts at the level of the first period (the room as it is), then
// falls at once and rises slowly, so constant noise never counts as speech.
class Vad {
public:
    bool   feed(const std::int16_t * pcm, std::size_t n);   // true = speech
    double floor_db() const { return floor_db_; }
    double last_db() const { return last_db_; }
private:
    bool   calibrated_ = false;
    double floor_db_ = kFloorInitDb;
    double last_db_  = kFloorInitDb;
};

// Collects one burst: preroll + speech periods, ends after end_silence_ms
// of non-speech or at max_ms. push() returns true when a burst is ready
// in take(); a burst with under kMinSpeechMs of speech is dropped.
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
    int  silence_ms_ = 0, burst_ms_ = 0, speech_ms_ = 0;
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
// Accepts hey|hi|oi|o|ei|ok|ola|alo before the name; the name within
// kNameMaxEdits (kNameMaxEditsLong for long names) or any of `aliases`,
// the spellings whisper produces for it ("eaz", "iase" for "easy" said in
// Portuguese). default_aliases(name) gives the built-in ones.
WakeResult wake_match(const std::string & transcript, const std::string & name,
                      const std::vector<std::string> & aliases = {});
std::vector<std::string> default_aliases(const std::string & name);

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

// One line of /proc/asound/pcm: "01-00: ALC245 Analog : ALC245 Analog : playback 1 : capture 1".
struct PcmDevice {
    int         card = 0, dev = 0;
    std::string name;
    bool        playback = false, capture = false;
    std::string alsa_name() const { return "plughw:" + std::to_string(card) + "," + std::to_string(dev); }
};
// Parses the whole /proc/asound/pcm text; used to find a capture device when
// ALSA "default" has none (an HDMI-only card 0 is the usual reason).
std::vector<PcmDevice> parse_asound_pcm(const std::string & text);

}  // namespace assistant
}  // namespace easyai
