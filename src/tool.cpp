#include "easyai/tool.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace easyai {

// --------------------------------------------------------------------------
// Tool::Builder::build  —  emit a JSON-schema object from the typed params.
// --------------------------------------------------------------------------
static std::string json_escape(const std::string & s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

Tool Tool::Builder::build() const {
    std::ostringstream js;
    js << "{\"type\":\"object\",\"properties\":{";
    bool first = true;
    for (const auto & p : params_) {
        if (!first) js << ',';
        first = false;
        js << '"' << json_escape(p.name) << "\":{\"type\":\""
           << json_escape(p.type) << "\"";
        if (!p.description.empty()) {
            js << ",\"description\":\"" << json_escape(p.description) << "\"";
        }
        js << '}';
    }
    js << "},\"required\":[";
    first = true;
    for (const auto & p : params_) {
        if (!p.required) continue;
        if (!first) js << ',';
        first = false;
        js << '"' << json_escape(p.name) << '"';
    }
    js << "]}";

    return Tool{ name_, desc_, short_, js.str(), handler_, addendum_ };
}

// Resolve the description that should ship in the per-turn `<tools>`
// block (or in the OpenAI tools array on /v1/chat/completions). Order:
//
//   1. `short_description` if the tool set one — the canonical path.
//   2. otherwise, the first non-empty line of `description`, capped at
//      120 chars. Lets pre-existing tools work without their authors
//      having migrated yet; they just lose the "single sentence"
//      curation until they call `.short_describe(...)` themselves.
//   3. otherwise, empty.
std::string Tool::wire_description() const {
    if (!short_description.empty()) return short_description;
    if (description.empty())        return std::string();

    constexpr std::size_t kCap = 120;
    std::size_t i = 0;
    while (i < description.size() && (description[i] == ' '  ||
                                       description[i] == '\t' ||
                                       description[i] == '\n' ||
                                       description[i] == '\r')) ++i;
    std::size_t start = i;
    while (i < description.size() && description[i] != '\n' && (i - start) < kCap) ++i;
    std::size_t end = i;
    while (end > start && (description[end - 1] == ' '  ||
                            description[end - 1] == '\t' ||
                            description[end - 1] == '\r')) --end;
    if (end == start) return std::string();
    std::string out = description.substr(start, end - start);
    if (end == start + kCap && end < description.size()
        && description[end] != '\n' && description[end] != '\0') {
        // Mid-word truncation — back off to the last word boundary.
        std::size_t last_sp = out.find_last_of(" \t");
        if (last_sp != std::string::npos && last_sp > 0) out.resize(last_sp);
        out += "…";
    }
    return out;
}

// --------------------------------------------------------------------------
// Tiny JSON arg getters — good enough for flat object payloads from LLMs.
// They do NOT implement a full JSON parser; they look for `"key"` then walk
// past whitespace + ':' and read a string / number / bool literal.
// --------------------------------------------------------------------------
namespace {

size_t find_key(const std::string & j, const std::string & key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = j.find(needle, pos)) != std::string::npos) {
        // ensure we matched a real key (preceded by '{' or ',' modulo ws)
        size_t b = pos;
        while (b > 0 && std::isspace(static_cast<unsigned char>(j[b - 1]))) --b;
        if (b == 0 || j[b - 1] == '{' || j[b - 1] == ',') {
            size_t after = pos + needle.size();
            while (after < j.size() &&
                   std::isspace(static_cast<unsigned char>(j[after]))) ++after;
            if (after < j.size() && j[after] == ':') return after + 1;
        }
        pos += needle.size();
    }
    return std::string::npos;
}

size_t skip_ws(const std::string & j, size_t i) {
    while (i < j.size() && std::isspace(static_cast<unsigned char>(j[i]))) ++i;
    return i;
}

bool read_json_string(const std::string & j, size_t i, std::string & out) {
    i = skip_ws(j, i);
    if (i >= j.size() || j[i] != '"') return false;
    ++i;
    out.clear();
    while (i < j.size()) {
        char c = j[i++];
        if (c == '"') return true;
        if (c == '\\' && i < j.size()) {
            char esc = j[i++];
            switch (esc) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u': {
                    if (i + 4 > j.size()) return false;
                    std::string hex = j.substr(i, 4);
                    i += 4;
                    unsigned code = std::strtoul(hex.c_str(), nullptr, 16);
                    if (code < 0x80) {
                        out += static_cast<char>(code);
                    } else if (code < 0x800) {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default: out += esc;
            }
        } else {
            out += c;
        }
    }
    return false;
}

}  // namespace

namespace args {

// Tolerant string getter — accepts:
//   "value"           → "value"           (the spec form)
//   123 / -1.5 / 1e3  → "123" / "-1.5" / "1e3"   (model emitted a number
//                                                  for a string field)
//   true / false      → "true" / "false"  (likewise for booleans)
//   null              → returns false     (treat as missing)
// This mirrors how lenient tool-arg parsers (nlohmann's value<>, OpenAI
// SDK's tool-arg coercion) handle real-world model output where the
// model imitates the format it saw in the prompt rather than the schema.
// Concrete repro: plan tool's schema declares id="string", but its
// render shows "1.", "2.", "3." — many models emit `"id": 1` (integer)
// and expect us to accept it.  Without this coercion the call fails
// with "needs id" even though the value is right there.
bool get_string(const std::string & json, const std::string & key, std::string & out) {
    size_t i = find_key(json, key);
    if (i == std::string::npos) return false;
    i = skip_ws(json, i);
    if (i >= json.size()) return false;
    char c = json[i];
    if (c == '"') return read_json_string(json, i, out);
    if (c == 't' && json.compare(i, 4, "true")  == 0) { out = "true";  return true; }
    if (c == 'f' && json.compare(i, 5, "false") == 0) { out = "false"; return true; }
    if (c == 'n' && json.compare(i, 4, "null")  == 0) return false;
    // Number (integer or floating-point, with optional sign / exponent).
    // Scan the literal and copy its bytes verbatim so we don't lose
    // precision converting through double.
    if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
        size_t start = i;
        if (c == '-' || c == '+') ++i;
        while (i < json.size() &&
               ((json[i] >= '0' && json[i] <= '9') ||
                json[i] == '.' || json[i] == 'e' || json[i] == 'E' ||
                json[i] == '+' || json[i] == '-')) ++i;
        if (i > start) {
            out.assign(json, start, i - start);
            return true;
        }
    }
    return false;
}

// Tolerant int getter — also accepts a quoted integer literal `"42"`,
// which models occasionally emit when they see a string-typed example
// in the prompt next to a numeric field.
bool get_int(const std::string & json, const std::string & key, long long & out) {
    size_t i = find_key(json, key);
    if (i == std::string::npos) return false;
    i = skip_ws(json, i);
    if (i >= json.size()) return false;
    if (json[i] == '"') {
        std::string s;
        if (!read_json_string(json, i, s)) return false;
        char * end = nullptr;
        out = std::strtoll(s.c_str(), &end, 10);
        return end != s.c_str() && *end == '\0';
    }
    char * end = nullptr;
    out = std::strtoll(json.c_str() + i, &end, 10);
    return end != json.c_str() + i;
}

bool get_double(const std::string & json, const std::string & key, double & out) {
    size_t i = find_key(json, key);
    if (i == std::string::npos) return false;
    i = skip_ws(json, i);
    if (i >= json.size()) return false;
    if (json[i] == '"') {
        std::string s;
        if (!read_json_string(json, i, s)) return false;
        char * end = nullptr;
        out = std::strtod(s.c_str(), &end);
        return end != s.c_str() && *end == '\0';
    }
    char * end = nullptr;
    out = std::strtod(json.c_str() + i, &end);
    return end != json.c_str() + i;
}

bool get_bool(const std::string & json, const std::string & key, bool & out) {
    size_t i = find_key(json, key);
    if (i == std::string::npos) return false;
    i = skip_ws(json, i);
    if (i >= json.size()) return false;
    // Native booleans.
    if (json.compare(i, 4, "true")  == 0) { out = true;  return true; }
    if (json.compare(i, 5, "false") == 0) { out = false; return true; }
    // Quoted booleans (`"true"` / `"false"`) — same model-tolerance idea.
    if (json[i] == '"') {
        std::string s;
        if (!read_json_string(json, i, s)) return false;
        if (s == "true"  || s == "1") { out = true;  return true; }
        if (s == "false" || s == "0") { out = false; return true; }
    }
    return false;
}

// _or variants — return the supplied default when the key is missing or
// the value couldn't be parsed as the requested type.

std::string get_string_or(const std::string & json, const std::string & key,
                          std::string default_value) {
    std::string out;
    return get_string(json, key, out) ? out : std::move(default_value);
}

long long get_int_or(const std::string & json, const std::string & key,
                     long long default_value) {
    long long out = 0;
    return get_int(json, key, out) ? out : default_value;
}

double get_double_or(const std::string & json, const std::string & key,
                     double default_value) {
    double out = 0.0;
    return get_double(json, key, out) ? out : default_value;
}

bool get_bool_or(const std::string & json, const std::string & key,
                 bool default_value) {
    bool out = false;
    return get_bool(json, key, out) ? out : default_value;
}

bool has(const std::string & json, const std::string & key) {
    return find_key(json, key) != std::string::npos;
}

// Internal: stringified-array unwrap is recursive (it re-enters get_array
// with a synthetic wrapper). A hostile model could feed
//     "items": "\"\\\"\\\\\\\"...\""
// (a string-of-string-of-string-…) and blow the stack. Cap the depth.
static bool get_array_impl(const std::string & json, const std::string & key,
                           std::vector<std::string> & out, int depth) {
    constexpr int kMaxUnwrapDepth = 4;
    if (depth > kMaxUnwrapDepth) return false;
    size_t i = find_key(json, key);
    if (i == std::string::npos) return false;
    i = skip_ws(json, i);
    if (i >= json.size()) return false;

    // Tolerate stringified arrays: smaller MoE/dense models often emit
    //     "items": "[{\"text\":\"...\"}]"
    // — the array escaped into a JSON string — instead of the spec form
    //     "items": [{"text":"..."}].
    // Same lenient pattern as get_string/get_int/get_bool above.
    if (json[i] == '"') {
        std::string unwrapped;
        if (!read_json_string(json, i, unwrapped)) return false;
        std::string synthetic = "{\"_a\":" + unwrapped + "}";
        return get_array_impl(synthetic, "_a", out, depth + 1);
    }

    if (json[i] != '[') return false;
    ++i;
    out.clear();
    i = skip_ws(json, i);
    if (i < json.size() && json[i] == ']') return true;

    while (i < json.size()) {
        i = skip_ws(json, i);
        if (i >= json.size()) return false;
        if (json[i] == ']') return true;

        size_t start = i;
        int brace_depth = 0;
        bool in_str = false;
        while (i < json.size()) {
            char c = json[i];
            if (in_str) {
                if (c == '\\') { i += 2; continue; }
                if (c == '"') in_str = false;
                ++i;
                continue;
            }
            if (c == '"')            { in_str = true; ++i; continue; }
            if (c == '{' || c == '[') { ++brace_depth; ++i; continue; }
            if (c == '}' || c == ']') {
                if (brace_depth == 0) break;
                --brace_depth; ++i; continue;
            }
            if (c == ',' && brace_depth == 0) break;
            ++i;
        }

        size_t end = i;
        while (end > start &&
               std::isspace(static_cast<unsigned char>(json[end - 1]))) --end;
        if (end > start) out.push_back(json.substr(start, end - start));

        if (i >= json.size()) return false;
        if (json[i] == ',') { ++i; continue; }
    }
    return true;
}

bool get_array(const std::string & json, const std::string & key,
               std::vector<std::string> & out) {
    return get_array_impl(json, key, out, 0);
}

}  // namespace args
}  // namespace easyai
