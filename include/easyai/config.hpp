// easyai/config.hpp — INI configuration for the server.
//
// One file at a stable path (default `/etc/easyai/easyai.ini`),
// section-organised, where every easyai feature that needs
// operator-tunable values lives. Today the only consumer is the
// MCP auth gate (section `[MCP_USER]`); future sections will
// govern logging, per-tool ACLs, rate limits, etc — the parser
// is general so adding a new section is a single-file change.
//
// Format
// ------
//   ; or # — line comment (whole line; in-line comments would
//            collide with values that legitimately contain `#`)
//   [section]              — section header
//   key = value            — entry inside the current section
//   key = "quoted value"   — surrounding double-quotes are stripped
//
// Whitespace around section names, keys, and values is trimmed.
// Keys are unique within a section; a duplicate key overwrites
// the earlier value.
//
// Errors
// ------
// `load_ini_file` is permissive: a missing file returns an empty
// `Ini` and an empty `err_out`. Parse problems on individual lines
// (key without section, missing `=`) are reported via `err_out`
// but do NOT stop parsing — well-formed lines BEFORE and AFTER
// the bad line are still loaded. The operator sees the warning at
// startup and fixes the line; meanwhile the server runs with a
// best-effort config.
#pragma once

#include <map>
#include <string>

namespace easyai::config {

struct Ini {
    // section name -> (key -> value), all post-trim and post-unquote.
    std::map<std::string, std::map<std::string, std::string>> sections;

    // Returns the value at [section] key=, or "" if either is absent.
    std::string get(const std::string & section,
                    const std::string & key) const;

    // Returns the inner map for `section`, or a static empty map if
    // the section is absent. Useful when you want to enumerate every
    // entry of a section (e.g. every user under [MCP_USER]).
    const std::map<std::string, std::string> &
        section_or_empty(const std::string & section) const;

    // True if at least one entry exists in `section`.
    bool has_nonempty_section(const std::string & section) const;
};

// Parse an INI file. Missing-file is NOT an error (returns empty
// Ini, empty err_out). Per-line parse problems are reported in
// `err_out` (one or more lines of "line N: …") but parsing
// continues — well-formed entries from the rest of the file still
// load.
Ini load_ini_file(const std::string & path, std::string & err_out);

// Find the best-matching [MODEL_<pattern>] section for a resolved model
// name.  Scans every section whose name starts with "MODEL_" and picks
// the best fit using this precedence:
//
//   1. Exclusive alias — a section whose `alias = <name>[,<name>...]` key
//      contains an exact (case-insensitive) match for `model_name`.  Use
//      this when the pattern would otherwise be ambiguous and you want
//      the section to apply ONLY to this model.  Multiple aliases may be
//      comma-separated.  Among aliased matches the longest alias wins.
//   2. Prefix pattern — `MODEL_<pattern>` matches if `model_name` STARTS
//      WITH `pattern` (case-insensitive).  The longest matching prefix
//      wins.  Lets one section cover a whole quant family
//      (`MODEL_Qwen3.6` → Qwen3.6-25B-A38M-Q4_K_M, Qwen3.6-25B-A38M-Q6_K, …).
//      Sections with an `alias` key are skipped here so they only
//      activate via their explicit aliases.
//
// Returns the full section name (e.g. "MODEL_Qwen3-Coder") or "" if
// nothing matches.
std::string find_model_section(const Ini & ini, const std::string & model_name);

}  // namespace easyai::config
