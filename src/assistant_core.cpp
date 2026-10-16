#include "easyai/assistant_core.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace easyai {
namespace assistant {

namespace {
constexpr double kPi            = 3.14159265358979323846;
constexpr double kMinRmsDb      = -100.0;
constexpr double kPcmFullScale  = 32768.0;
constexpr float  kBeepAmp       = 0.3f;
constexpr int    kMsPerSecond   = 1000;

// Accented letters folded to ASCII (UTF-8 two-byte sequences).
const char * kFoldFrom[] = {"á","à","â","ã","ä","é","è","ê","ë","í","ì","î","ï","ó","ò","ô","õ","ö","ú","ù","û","ü","ç","ñ",
                            "Á","À","Â","Ã","Ä","É","È","Ê","Ë","Í","Ì","Î","Ï","Ó","Ò","Ô","Õ","Ö","Ú","Ù","Û","Ü","Ç","Ñ"};
const char   kFoldTo[]   = {'a','a','a','a','a','e','e','e','e','i','i','i','i','o','o','o','o','o','u','u','u','u','c','n',
                            'a','a','a','a','a','e','e','e','e','i','i','i','i','o','o','o','o','o','u','u','u','u','c','n'};

// Call words, with the spellings whisper gives them: "O easyai" for "oi easyai", "Em" / "É e" for "ei", "hoy", "High" for "hi".
const char * kWakePrefixes[] = {"hey", "hi", "high", "oi", "o", "oh", "hoy", "ei", "em", "ok", "ola", "alo", "e", "a"};
// Spellings the small whisper models produce for the built-in names in pt-BR / en.
const char * kEasyAliases[] = {"izi", "isi", "eaz", "iase", "ize", "easi", "izy", "esy", "ísi", "hizi", "iz", "asse", "ase", "aise", "azi", "izze"};
const char * kMaxAliases[]  = {"maks", "macs", "mex", "mac", "mach", "maxx", "marx", "máx", "mas"};
// "easyai" as whisper tiny/base wrote it for Kokoro pt-BR ("oi ísi ai") and en ("hey easy AI") voices, 2026-10-16:
// "O Iziai", "Oisiai" (oi glued), "Oi Ziai", "Eisiai", "Easy I", "O Ijazia", "E ZI", "Gisiai", "Isia", "EasyA", "easy A, I".
const char * kEasyaiAliases[] = {"iziai", "isiai", "isia", "ziai", "siai", "ezi", "easya", "gisiai", "ijazia", "izai", "isai", "easyeye", "easyay"};
// Names whose spelling misleads the phonemizer: how to write them for the voice.
struct SpokenName { const char * name, * en, * pt; };
const SpokenName kSpokenNames[] = {{"easyai", "easy AI", "ísi ai"}};
const char * kPtWords[] = {"que", "nao", "voce", "e", "de", "para", "com", "uma", "um", "isso", "esta", "como", "qual", "quem",
                           "onde", "quando", "por", "me", "eu", "sim", "tudo", "bem", "obrigado", "hora", "hoje"};
const char * kEnWords[] = {"the", "is", "what", "are", "you", "how", "and", "it", "to", "of", "a", "in", "that", "this",
                           "time", "please", "can", "do", "me", "yes", "no", "today", "thanks", "who", "where"};

std::vector<std::string> words(const std::string & s) {
    std::vector<std::string> out; std::stringstream ss(s); std::string w;
    while (ss >> w) out.push_back(w);
    return out;
}
}  // namespace

double rms_db(const std::int16_t * pcm, std::size_t n) {
    if (!pcm || n == 0) return kMinRmsDb;
    double acc = 0;
    for (std::size_t i = 0; i < n; ++i) { const double v = pcm[i] / kPcmFullScale; acc += v * v; }
    const double rms = std::sqrt(acc / (double) n);
    return rms > 0 ? std::max(kMinRmsDb, 20.0 * std::log10(rms)) : kMinRmsDb;
}

bool Vad::feed(const std::int16_t * pcm, std::size_t n) {
    last_db_ = rms_db(pcm, n);
    if (!calibrated_) { floor_db_ = std::max(kFloorMinDb, last_db_); calibrated_ = true; return false; }
    const double period_s = (double) n / kSampleRate;
    if (last_db_ < floor_db_) floor_db_ = std::max(kFloorMinDb, last_db_);              // down at once
    else                      floor_db_ = std::min(last_db_, floor_db_ + kFloorRiseDbPerS * period_s);   // up slowly
    return last_db_ > floor_db_ + kSpeechMarginDb;
}

Segmenter::Segmenter(int end_silence_ms, int max_ms) : end_silence_ms_(end_silence_ms), max_ms_(max_ms) {}

void Segmenter::reset() { in_burst_ = false; silence_ms_ = burst_ms_ = speech_ms_ = 0; burst_.clear(); ready_.clear(); }

bool Segmenter::push(const std::int16_t * pcm, std::size_t n, bool speech) {
    const int ms = (int) (n * kMsPerSecond / kSampleRate);
    if (!in_burst_) {
        preroll_.insert(preroll_.end(), pcm, pcm + n);
        const std::size_t keep = (std::size_t) kSampleRate * kPrerollMs / kMsPerSecond;
        if (preroll_.size() > keep) preroll_.erase(preroll_.begin(), preroll_.end() - (std::ptrdiff_t) keep);
        if (!speech) return false;
        in_burst_ = true; silence_ms_ = 0; burst_ms_ = 0; speech_ms_ = ms;
        burst_ = preroll_;
        preroll_.clear();
        burst_.insert(burst_.end(), pcm, pcm + n);
        return false;
    }
    burst_.insert(burst_.end(), pcm, pcm + n);
    burst_ms_ += ms;
    if (speech) speech_ms_ += ms;
    silence_ms_ = speech ? 0 : silence_ms_ + ms;
    if (silence_ms_ >= end_silence_ms_ || burst_ms_ >= max_ms_) {
        const bool real = speech_ms_ >= kMinSpeechMs;
        if (real) ready_.swap(burst_);
        burst_.clear();
        in_burst_ = false;
        return real;
    }
    return false;
}

std::vector<std::int16_t> Segmenter::take() { std::vector<std::int16_t> out; out.swap(ready_); return out; }

std::string normalize_text(const std::string & s) {
    std::string t;
    t.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        bool folded = false;
        for (std::size_t k = 0; k < sizeof(kFoldFrom) / sizeof(kFoldFrom[0]); ++k) {
            const std::size_t L = std::char_traits<char>::length(kFoldFrom[k]);
            if (s.compare(i, L, kFoldFrom[k]) == 0) { t.push_back(kFoldTo[k]); i += L; folded = true; break; }
        }
        if (folded) continue;
        const unsigned char c = (unsigned char) s[i++];
        if (c >= 0x80) continue;                         // other non-ASCII: dropped
        if (std::isalnum(c)) t.push_back((char) std::tolower(c));
        else t.push_back(' ');
    }
    std::string out; bool space = true;
    for (char c : t) {
        if (c == ' ') { if (!space) out.push_back(' '); space = true; }
        else { out.push_back(c); space = false; }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

int levenshtein(const std::string & a, const std::string & b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = (int) j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = (int) i;
        for (std::size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        prev.swap(cur);
    }
    return prev[b.size()];
}

namespace {
std::string no_spaces(const std::string & s) { std::string o; for (char c : s) if (c != ' ') o.push_back(c); return o; }
}

std::vector<std::string> default_aliases(const std::string & name) {
    std::vector<std::string> out;
    const std::string nm = no_spaces(normalize_text(name));
    if (nm == "easy")   for (const char * a : kEasyAliases)   out.push_back(normalize_text(a));
    if (nm == "max")    for (const char * a : kMaxAliases)    out.push_back(normalize_text(a));
    if (nm == "easyai") for (const char * a : kEasyaiAliases) out.push_back(normalize_text(a));
    return out;
}

std::string spoken_name(const std::string & name, const std::string & lang) {
    const std::string nm = no_spaces(normalize_text(name));
    for (const auto & sn : kSpokenNames) if (nm == sn.name) return lang == "pt" ? sn.pt : sn.en;
    return name;
}

WakeResult wake_match(const std::string & transcript, const std::string & name, const std::vector<std::string> & aliases) {
    WakeResult r;
    const auto w = words(normalize_text(transcript));
    const std::string nm = no_spaces(normalize_text(name));
    if (w.empty() || nm.empty()) return r;
    const int max_edits = (int) nm.size() >= kLongNameChars ? kNameMaxEditsLong : kNameMaxEdits;
    std::vector<std::string> al;
    for (const auto & a : aliases) al.push_back(no_spaces(normalize_text(a)));
    auto is_name = [&](const std::string & cand) {
        if (levenshtein(cand, nm) <= max_edits) return true;
        for (const auto & a : al) if (cand == a) return true;
        return false;
    };
    // the name may come back as several words ("easy ai", "easy a i"): try the longest join first
    auto name_words_at = [&](std::size_t i) -> std::size_t {
        for (std::size_t n = std::min((std::size_t) kMaxNameWords, w.size() - i); n >= 1; --n) {
            std::string cand;
            for (std::size_t k = 0; k < n; ++k) cand += w[i + k];
            if (is_name(cand)) return n;
        }
        return 0;
    };
    for (std::size_t i = 0; i < w.size(); ++i) {
        bool prefixed = (i == 0);
        if (i > 0) for (const char * p : kWakePrefixes) if (w[i - 1] == p) { prefixed = true; break; }
        std::size_t used = prefixed ? name_words_at(i) : 0;
        if (used == 0) {   // the call word glued to the name: "oiasse", "heyeasy", "oisiai" (two-letter call words and longer only)
            for (const char * p : kWakePrefixes) {
                const std::string ps = p;
                if (ps.size() < 2) continue;
                if (w[i].size() > ps.size() + 1 && w[i].compare(0, ps.size(), ps) == 0 && is_name(w[i].substr(ps.size()))) { used = 1; break; }
            }
        }
        if (used == 0) continue;
        r.matched = true;
        std::string q;
        for (std::size_t k = i + used; k < w.size(); ++k) { if (!q.empty()) q += ' '; q += w[k]; }
        if ((int) std::count(q.begin(), q.end(), ' ') + 1 >= kMinQuestionWords) r.question = q;
        return r;
    }
    return r;
}

void SentenceChunker::push(const std::string & piece) { buf_ += piece; }

bool SentenceChunker::pop(std::string & sentence) {
    for (std::size_t i = 0; i < buf_.size(); ++i) {
        const char c = buf_[i];
        const bool nl = (c == '\n');
        const bool punct = (c == '.' || c == '!' || c == '?');
        if (!nl && !punct) continue;
        // a sentence mark needs a following space/newline (so "3.14" and "e.g." stay), or end-of-buffer is NOT enough mid-stream
        if (punct && (i + 1 >= buf_.size() || (buf_[i + 1] != ' ' && buf_[i + 1] != '\n'))) continue;
        if ((int) i + 1 < kMinSentenceChars && !nl) continue;
        if (c == '.') {   // abbreviations: "e.g." / "i.e." / a single initial never end a sentence
            const std::size_t ws = buf_.find_last_of(" \n", i);
            const std::string word = buf_.substr(ws == std::string::npos ? 0 : ws + 1, i - (ws == std::string::npos ? 0 : ws + 1));
            if (word.size() <= 1 || word.find('.') != std::string::npos) continue;
        }
        sentence = buf_.substr(0, i + 1);
        buf_.erase(0, i + 1);
        // trim
        const auto a = sentence.find_first_not_of(" \n\t"), b = sentence.find_last_not_of(" \n\t");
        sentence = (a == std::string::npos) ? "" : sentence.substr(a, b - a + 1);
        if (sentence.empty()) { --i; continue; }
        return true;
    }
    return false;
}

std::string SentenceChunker::flush() {
    const auto a = buf_.find_first_not_of(" \n\t"), b = buf_.find_last_not_of(" \n\t");
    std::string s = (a == std::string::npos) ? "" : buf_.substr(a, b - a + 1);
    buf_.clear();
    return s;
}

bool looks_like_question(const std::string & transcript) {
    // strip bracketed / starred markers before counting words
    std::string t; bool skip = false; char close = 0;
    for (char c : transcript) {
        if (!skip && (c == '*' || c == '[' || c == '(')) { skip = true; close = c == '*' ? '*' : c == '[' ? ']' : ')'; continue; }
        if (skip) { if (c == close) skip = false; continue; }
        t.push_back(c);
    }
    int n = 0;
    for (const auto & w : words(normalize_text(t))) if (w.size() >= 2 || std::isalpha((unsigned char) w[0])) ++n;
    return n >= kMinQuestionWords;
}

std::string guess_language(const std::string & text) {
    int pt = 0, en = 0;
    for (const auto & w : words(normalize_text(text))) {
        for (const char * p : kPtWords) if (w == p) { ++pt; break; }
        for (const char * p : kEnWords) if (w == p) { ++en; break; }
    }
    return pt > en ? "pt" : "en";
}

std::vector<PcmDevice> parse_asound_pcm(const std::string & text) {
    std::vector<PcmDevice> out;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        // "cc-dd: name : subname : playback N : capture N"
        if (line.size() < 6 || line[2] != '-' || line[5] != ':') continue;
        PcmDevice d;
        d.card = std::atoi(line.substr(0, 2).c_str());
        d.dev  = std::atoi(line.substr(3, 2).c_str());
        std::vector<std::string> fields; std::string f;
        std::stringstream fs(line.substr(6));
        while (std::getline(fs, f, ':')) {
            const auto a = f.find_first_not_of(' '), b = f.find_last_not_of(' ');
            fields.push_back(a == std::string::npos ? "" : f.substr(a, b - a + 1));
        }
        if (!fields.empty()) d.name = fields[0];
        for (const auto & x : fields) {
            if (x.rfind("playback", 0) == 0) d.playback = true;
            if (x.rfind("capture", 0) == 0)  d.capture = true;
        }
        out.push_back(d);
    }
    return out;
}

std::vector<std::int16_t> make_off_sound(int rate_hz) {
    std::vector<std::int16_t> out;
    const std::size_t tone = (std::size_t) rate_hz * kOffToneMs / kMsPerSecond;
    for (int hz : {kOffHighHz, kOffLowHz})
        for (std::size_t i = 0; i < tone; ++i) {
            const double env = std::sin(kPi * (double) i / (double) tone);
            out.push_back((std::int16_t) (kBeepAmp * env * std::sin(2 * kPi * hz * (double) i / rate_hz) * 32767.0));
        }
    return out;
}

std::vector<std::int16_t> make_beep(int rate_hz, int count) {
    std::vector<std::int16_t> out;
    const std::size_t tone = (std::size_t) rate_hz * kBeepMs / kMsPerSecond;
    const std::size_t gap  = (std::size_t) rate_hz * kBeepGapMs / kMsPerSecond;
    for (int b = 0; b < count; ++b) {
        for (std::size_t i = 0; i < tone; ++i) {
            const double env = std::sin(kPi * (double) i / (double) tone);   // half-sine envelope: no click
            out.push_back((std::int16_t) (kBeepAmp * env * std::sin(2 * kPi * kBeepHz * (double) i / rate_hz) * 32767.0));
        }
        if (b + 1 < count) out.insert(out.end(), gap, 0);
    }
    return out;
}

}  // namespace assistant
}  // namespace easyai
