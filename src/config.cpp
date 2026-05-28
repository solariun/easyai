// src/config.cpp — INI parser for easyai's central config file.
//
// Trivial recursive-descent: line at a time, a section header
// switches the active section, anything else with an `=` is a
// `key = value` entry. No nested sections, no list values, no
// includes — everything fancy lives in code, not in the config.
//
// Robust against typical hand-edits: trailing whitespace, BOM,
// CRLF line endings, optional surrounding quotes on values.

#include "easyai/config.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace easyai::config {

namespace {

std::string trim(std::string s) {
    auto issp = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && issp(s.front())) s.erase(s.begin());
    while (!s.empty() && issp(s.back()))  s.pop_back();
    return s;
}

// Strip a UTF-8 BOM if present at the very start of the file.
void strip_bom(std::string & s) {
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
}

}  // namespace

std::string Ini::get(const std::string & section,
                     const std::string & key) const {
    auto it = sections.find(section);
    if (it == sections.end()) return std::string();
    auto kit = it->second.find(key);
    if (kit == it->second.end()) return std::string();
    return kit->second;
}

const std::map<std::string, std::string> &
Ini::section_or_empty(const std::string & section) const {
    static const std::map<std::string, std::string> kEmpty;
    auto it = sections.find(section);
    return (it == sections.end()) ? kEmpty : it->second;
}

bool Ini::has_nonempty_section(const std::string & section) const {
    auto it = sections.find(section);
    return it != sections.end() && !it->second.empty();
}

Ini load_ini_file(const std::string & path, std::string & err_out) {
    err_out.clear();
    Ini out;

    std::ifstream f(path);
    if (!f) {
        // Missing file is NOT an error — operator just hasn't
        // configured yet. Caller treats this as "all defaults".
        return out;
    }

    // Hard caps on what we'll accept from a config file. Any sane
    // operator config is far below these — they only fire when the
    // file is corrupt, swapped for a giant blob, or pointed at the
    // wrong path (`/etc/passwd`, a runaway log file). The parser is
    // O(file_size) so without a bound, a misconfigured `--config
    // /dev/zero` would spin forever filling RAM.
    constexpr std::size_t kMaxFileBytes = 1u * 1024u * 1024u;   // 1 MiB
    constexpr std::size_t kMaxLineBytes = 64u * 1024u;          // 64 KiB
    constexpr int         kMaxLines     = 100000;

    std::ostringstream errs;
    std::string current_section;
    std::string line;
    int line_no = 0;
    std::size_t bytes_read = 0;

    while (std::getline(f, line)) {
        bytes_read += line.size() + 1;
        if (bytes_read > kMaxFileBytes) {
            errs << "line " << (line_no + 1)
                 << ": file exceeds " << kMaxFileBytes
                 << " bytes; stopping parse\n";
            break;
        }
        if (line.size() > kMaxLineBytes) {
            errs << "line " << (line_no + 1)
                 << ": line exceeds " << kMaxLineBytes
                 << " bytes; skipping\n";
            ++line_no;
            continue;
        }
        if (line_no >= kMaxLines) {
            errs << "stopped at " << kMaxLines
                 << " lines; rest of file ignored\n";
            break;
        }
        ++line_no;
        // CRLF safety — getline strips \n, leaves \r.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // BOM safety on the very first line.
        if (line_no == 1) strip_bom(line);

        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;
        if (trimmed[0] == '#' || trimmed[0] == ';') continue;

        // Section header: [name]
        if (trimmed.front() == '[') {
            if (trimmed.back() != ']' || trimmed.size() < 3) {
                errs << "line " << line_no
                     << ": malformed section header\n";
                continue;
            }
            current_section = trim(trimmed.substr(1, trimmed.size() - 2));
            // Touch the section so an EMPTY section is still
            // representable (operator wrote `[MCP_USER]` with
            // every entry commented out — we want to know the
            // section EXISTS but is empty, distinct from missing).
            out.sections[current_section];
            continue;
        }

        // key = value
        auto eq = trimmed.find('=');
        if (eq == std::string::npos) {
            errs << "line " << line_no
                 << ": missing '=' in entry\n";
            continue;
        }
        if (current_section.empty()) {
            errs << "line " << line_no
                 << ": entry outside any section (add a [Section] header)\n";
            continue;
        }
        std::string key = trim(trimmed.substr(0, eq));
        std::string val = trim(trimmed.substr(eq + 1));

        // Strip surrounding double-quotes ("…") so values with
        // leading/trailing whitespace can be expressed.
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        if (key.empty()) {
            errs << "line " << line_no
                 << ": empty key\n";
            continue;
        }
        out.sections[current_section][key] = val;
    }

    err_out = errs.str();
    if (!err_out.empty() && err_out.back() == '\n') {
        err_out.pop_back();
    }
    return out;
}

std::string find_model_section(const Ini & ini, const std::string & model_name) {
    if (model_name.empty()) return {};

    std::string name_lc;
    name_lc.reserve(model_name.size());
    for (char c : model_name)
        name_lc.push_back((char) std::tolower((unsigned char) c));

    std::string best_section;
    std::size_t best_len = 0;

    for (const auto & kv : ini.sections) {
        const auto & sec = kv.first;
        if (sec.size() <= 6) continue;
        if (sec.substr(0, 6) != "MODEL_") continue;
        std::string pattern = sec.substr(6);

        std::string pattern_lc;
        pattern_lc.reserve(pattern.size());
        for (char c : pattern)
            pattern_lc.push_back((char) std::tolower((unsigned char) c));

        if (pattern_lc.empty()) continue;
        if (name_lc.find(pattern_lc) != std::string::npos &&
            pattern_lc.size() > best_len) {
            best_len     = pattern_lc.size();
            best_section = sec;
        }
    }
    return best_section;
}

}  // namespace easyai::config
