// easyai/presets.hpp — named sampling presets and a tiny chat-line parser.
//
// Goal: let users (or AI agents talking to easyai-server) tweak generation
// behavior with words instead of numbers.
//
//   "deterministic"      → temp 0.0,  greedy
//   "precise 0.1"        → preset 'precise', temp overridden to 0.1
//   "/temp 0.5"          → just set temperature
//
// parse_preset() is a pure parser — it never touches an Engine. Apply the
// returned values via Engine::set_sampling().
//
#pragma once

#include "engine.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace easyai {

// A named sampling profile. Numbers are used as a *baseline*; commands like
// "creative 0.8" override `temperature` only.
struct Preset {
    std::string name;          // "creative"
    std::string description;   // human-readable hint
    float       temperature;
    float       top_p;
    int         top_k;
    float       min_p;
    // When true this preset imposes NO opinion of its own — its numbers
    // mirror the engine's built-in defaults and selecting it means "use
    // the model default sampling". The `auto` preset sets this; callers
    // may use it to label the UI ("auto") or to skip writing the sliders.
    bool        model_default = false;
};

// All built-in presets: "auto" (model default) first, then the rest
// ordered from most deterministic to most creative.
const std::vector<Preset> & all_presets();

// Returns a pointer to the matching preset, or nullptr.  Case-insensitive,
// also matches a few synonyms (e.g. "exact" → "precise").
const Preset * find_preset(std::string_view name);

// Parsed result of parse_preset(). If `applied` is non-empty, the parser
// recognised the command and the four sampling fields are set.
//
// `consumed` is the byte length of the prefix that was eaten by the parser.
// This lets callers tell whether the line is JUST a command (consumed ==
// line size) or a command followed by a normal user message (consumed <
// size). The intended pattern in CLI/server is:
//
//   auto p = parse_preset(line);
//   if (!p.applied.empty()) engine.set_sampling(p.temperature, p.top_p, p.top_k, p.min_p);
//   if (p.consumed < line.size()) engine.chat(line.substr(p.consumed));
//
struct PresetResult {
    std::string applied;      // empty means: no preset/temperature command found
    float       temperature = 0.7f;
    float       top_p       = 0.95f;
    int         top_k       = 40;
    float       min_p       = 0.05f;
    size_t      consumed    = 0;
};

// Recognises (case-insensitive, with optional leading '/'):
//
//   <preset_name>                   apply preset
//   <preset_name> <number>          apply preset, override temperature
//   temp <number>                   set temperature only
//   temperature <number>            alias of "temp"
//   t <number>                      short alias
//
// Numbers are clamped to [0.0, 2.0] for safety.
PresetResult parse_preset(std::string_view line);

}  // namespace easyai
