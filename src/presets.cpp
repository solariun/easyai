// presets.cpp — see header for design notes.
#include "easyai/presets.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>

namespace easyai {

// ---------------------------------------------------------------------------
// Built-in preset table.  Tuned conservatively so they "just work" across the
// wide variety of GGUFs people will throw at this engine.
// ---------------------------------------------------------------------------
static const std::vector<Preset> kPresets = {
    // `auto` leads the table — it's the default. Its numbers mirror the
    // Engine's built-in sampler defaults, and model_default=true marks it
    // as "impose nothing, use the model default". Listed first so UIs and
    // /v1/models surface it as the natural starting point.
    { "auto",
      "Use the model's own default sampling — easyai imposes no preset.",
      0.7f, 0.95f, 40, 0.05f, true },

    { "deterministic",
      "No randomness — same prompt always produces the same answer (greedy decoding).",
      0.0f, 1.0f,    1, 0.0f },

    { "precise",
      "Stick to the most likely answers. Good for code, math, factual Q&A.",
      0.2f, 0.95f, 40, 0.10f },

    { "balanced",
      "General-purpose default. Slight variety, still focused.",
      0.7f, 0.95f, 40, 0.05f },

    { "creative",
      "More variety and surprising phrasing. Good for brainstorming or fiction.",
      1.0f, 0.95f, 40, 0.05f },

    { "wild",
      "Maximum entropy. Use for exploration; can go off-topic.",
      1.4f, 0.98f, 60, 0.00f },
};

const std::vector<Preset> & all_presets() { return kPresets; }

// Case-insensitive ASCII compare.
static bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower((unsigned char) a[i]) !=
            std::tolower((unsigned char) b[i])) return false;
    }
    return true;
}

const Preset * find_preset(std::string_view name) {
    // canonical names + a few synonyms
    struct Alias { const char * from; const char * to; };
    static const Alias aliases[] = {
        { "exact",    "precise"       },
        { "default",  "balanced"      },
        { "model",    "auto"          },
        { "fun",      "creative"      },
        { "chaos",    "wild"          },
        { "greedy",   "deterministic" },
    };
    for (const auto & a : aliases) {
        if (ieq(name, a.from)) { name = a.to; break; }
    }
    for (const auto & p : kPresets) if (ieq(name, p.name)) return &p;
    return nullptr;
}

// ---------------------------------------------------------------------------
// parse_preset
//
// Scans the prefix of `line` for either a preset name or a "temp <n>" form.
// Designed to be safe with garbage input (no allocations beyond a small string
// view; no exceptions thrown).
// ---------------------------------------------------------------------------
namespace {

// Skip ASCII whitespace, return new index.
size_t skip_ws(std::string_view s, size_t i) {
    while (i < s.size() && std::isspace((unsigned char) s[i])) ++i;
    return i;
}

// Read an alphabetic word starting at `i`. Updates `i` past the word.
std::string_view take_word(std::string_view s, size_t & i) {
    size_t start = i;
    while (i < s.size() && (std::isalpha((unsigned char) s[i]) || s[i] == '_')) ++i;
    return s.substr(start, i - start);
}

// Try to read a non-negative number at `i`. Returns true if a number was read.
// Updates `i` past the number on success; leaves `i` unchanged on failure.
bool take_number(std::string_view s, size_t & i, float & out) {
    size_t start = i;
    // optional sign — we accept and clamp negatives
    if (start < s.size() && (s[start] == '-' || s[start] == '+')) ++start;
    bool seen_digit = false, seen_dot = false;
    size_t j = start;
    while (j < s.size()) {
        char c = s[j];
        if (std::isdigit((unsigned char) c)) { seen_digit = true; ++j; }
        else if (c == '.' && !seen_dot)      { seen_dot   = true; ++j; }
        else break;
    }
    if (!seen_digit) return false;
    // Use std::strtof on a NUL-terminated copy to avoid locale surprises.
    std::string buf(s.data() + i, j - i);
    char * end = nullptr;
    float v = std::strtof(buf.c_str(), &end);
    if (end == buf.c_str()) return false;
    out = v;
    i = j;
    return true;
}

}  // namespace

PresetResult parse_preset(std::string_view line) {
    PresetResult r{};
    size_t i = skip_ws(line, 0);
    if (i >= line.size()) return r;

    // optional leading '/'
    if (line[i] == '/') ++i;

    size_t word_start = i;
    auto word = take_word(line, i);
    if (word.empty()) return r;

    // -------- temp / temperature / t <number> --------
    if (ieq(word, "temp") || ieq(word, "temperature") || ieq(word, "t")) {
        size_t j = skip_ws(line, i);
        float v = 0.7f;
        if (!take_number(line, j, v)) return r;
        v = std::clamp(v, 0.0f, 2.0f);
        // baseline = precise (matches the project-wide default), then
        // override only temperature from the user's "temp <n>" line.
        const Preset * b = find_preset("precise");
        r.temperature = v;
        r.top_p       = b->top_p;
        r.top_k       = b->top_k;
        r.min_p       = b->min_p;
        r.applied     = "temperature=" + std::to_string(v);
        // Anything past the number is *user content* — return its offset.
        r.consumed    = skip_ws(line, j);
        return r;
    }

    // -------- preset name (optionally followed by override number) -----
    const Preset * p = find_preset(word);
    if (!p) return r;
    size_t after_word = i;
    size_t j = skip_ws(line, after_word);
    float v = p->temperature;
    bool has_override = take_number(line, j, v);

    r.temperature = std::clamp(v, 0.0f, 2.0f);
    r.top_p       = p->top_p;
    r.top_k       = p->top_k;
    r.min_p       = p->min_p;
    r.applied     = p->name;
    if (has_override) r.applied += "(t=" + std::to_string(r.temperature) + ")";
    r.consumed    = skip_ws(line, has_override ? j : after_word);
    (void) word_start;  // unused; left in case of future debug logging
    return r;
}

}  // namespace easyai
