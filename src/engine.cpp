#include "easyai/engine.hpp"
#include "easyai/tool.hpp"        // easyai::args::get_string for tool_call recovery
#include "easyai/log.hpp"          // raw transaction log + problem markers

#include "common.h"
#include "sampling.h"
#include "speculative.h"
#include "chat.h"
#include "llama.h"
#include "ggml-backend.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace easyai {

// ===========================================================================
// Tool-call recovery helpers
// ---------------------------------------------------------------------------
// Some models (notably Qwen2.5-Instruct in its current GGUF chat-template
// builds) emit tool calls with DOUBLED braces:
//
//     <tool_call>
//     {{"name":"web_search","arguments":{"query":"hello"}}}
//     </tool_call>
//
// The Jinja chat-template's example uses `{{ ... }}` for variable
// interpolation, and the template itself isn't escaping them when rendering
// the tool-call demo, so the model imitates the literal form.  The PEG
// parser in llama.cpp (correctly) refuses that JSON, falls through, and we
// end up with no tool_calls at all even though the user clearly asked for
// one.
//
// These helpers recover those calls by hand: we scan the raw output for
// <tool_call>...</tool_call> blocks and pull `name` + `arguments` out of
// each, tolerant of extra wrapping braces.
// ===========================================================================
namespace {

// Walk a JSON object starting at `s[i]` (which must be '{'), respecting
// strings.  Returns the index ONE PAST the matching '}', or npos on failure.
size_t walk_balanced_braces(const std::string & s, size_t i) {
    if (i >= s.size() || s[i] != '{') return std::string::npos;
    int  depth = 0;
    bool in_str = false, esc = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            if (esc)             esc = false;
            else if (c == '\\')  esc = true;
            else if (c == '"')   in_str = false;
            continue;
        }
        if      (c == '"') in_str = true;
        else if (c == '{') ++depth;
        else if (c == '}') {
            --depth;
            if (depth == 0) return i + 1;
        }
    }
    return std::string::npos;
}

// Scan `raw` for <tool_call>...</tool_call> blocks (terminated or
// unterminated) and return whatever {name, arguments} pairs we can recover.
std::vector<common_chat_tool_call> recover_qwen_tool_calls(const std::string & raw) {
    std::vector<common_chat_tool_call> out;
    static const std::string open_tag  = "<tool_call>";
    static const std::string close_tag = "</tool_call>";

    size_t pos = 0;
    while (true) {
        size_t a = raw.find(open_tag, pos);
        if (a == std::string::npos) break;
        size_t body_begin = a + open_tag.size();
        size_t b = raw.find(close_tag, body_begin);
        std::string body = (b == std::string::npos)
                               ? raw.substr(body_begin)
                               : raw.substr(body_begin, b - body_begin);

        // Pull out "name" — `args::get_string` does a forgiving top-level
        // key scan that already tolerates the doubled-brace wrapper.
        std::string name;
        if (!args::get_string(body, "name", name) || name.empty()) {
            pos = (b == std::string::npos) ? raw.size() : b + close_tag.size();
            continue;
        }

        // Pull out "arguments" — find the colon, then walk the JSON object.
        std::string arguments_json = "{}";
        size_t k = body.find("\"arguments\"");
        if (k != std::string::npos) {
            k = body.find(':', k);
            if (k != std::string::npos) {
                ++k;
                while (k < body.size() &&
                       std::isspace((unsigned char) body[k])) ++k;
                if (k < body.size() && body[k] == '{') {
                    size_t end = walk_balanced_braces(body, k);
                    if (end != std::string::npos) {
                        arguments_json = body.substr(k, end - k);
                    }
                }
            }
        }

        common_chat_tool_call tc;
        tc.name      = std::move(name);
        tc.arguments = std::move(arguments_json);
        out.push_back(std::move(tc));

        pos = (b == std::string::npos) ? raw.size() : b + close_tag.size();
    }
    return out;
}

// Drop any text inside <tool_call>...</tool_call> blocks (so the user
// doesn't see the raw JSON in the visible content after recovery).
std::string strip_tool_call_blocks(std::string s) {
    static const std::string open_tag  = "<tool_call>";
    static const std::string close_tag = "</tool_call>";
    size_t pos = 0;
    while ((pos = s.find(open_tag, pos)) != std::string::npos) {
        size_t end = s.find(close_tag, pos + open_tag.size());
        if (end == std::string::npos) { s.erase(pos); break; }
        s.erase(pos, end + close_tag.size() - pos);
    }
    return s;
}

// Named-tag tool-call recovery.
// ---------------------------------------------------------------------------
// Some weak / aggressively-quantised models drop the <tool_call> envelope
// entirely and use the TOOL NAME itself as the XML tag:
//
//     <tool_lookup>
//     </tool_lookup>
//
// or, when they do pass arguments, with a JSON body:
//
//     <rag>{"action":"search","keywords":["bitnet"]}</rag>
//
// The PEG parser doesn't recognise this shape, tool_calls comes back
// empty, and the tag leaks into msg.content as the "final answer" —
// killing the agentic loop. We recover by scanning for <NAME>...</NAME>
// where NAME is a *registered* tool. Body handling: empty -> "{}"; a
// JSON object -> used verbatim; anything else -> "{}" (the tool's own
// argument validation then drives the model to a correct retry).
//
// `tool_names` MUST be the live registry — matching arbitrary <tags>
// would misfire on prose. Even so this is the most false-positive-prone
// recovery path, so the caller gates it hard (runs last, content-only,
// and only when stripping the spans leaves a whitespace-only remainder
// — i.e. the WHOLE visible turn was just tool-name tags). See the
// dispatch site.
std::vector<common_chat_tool_call>
recover_named_tag_tool_calls(const std::string & content,
                             const std::vector<std::string> & tool_names) {
    std::vector<common_chat_tool_call> out;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t lt = content.find('<', pos);
        if (lt == std::string::npos) break;
        // Which registered tool (if any) does this tag open?
        const std::string * matched = nullptr;
        for (const auto & name : tool_names) {
            if (name.empty()) continue;
            if (content.compare(lt + 1, name.size(), name) == 0 &&
                lt + 1 + name.size() < content.size() &&
                content[lt + 1 + name.size()] == '>') {
                matched = &name;
                break;
            }
        }
        if (!matched) { pos = lt + 1; continue; }
        const std::string & name = *matched;
        size_t body_begin = lt + 1 + name.size() + 1;       // past "<name>"
        const std::string close_tag = "</" + name + ">";
        size_t b = content.find(close_tag, body_begin);
        if (b == std::string::npos) { pos = body_begin; continue; }

        // Trim the body.
        std::string body = content.substr(body_begin, b - body_begin);
        size_t s = 0, e = body.size();
        while (s < e && std::isspace((unsigned char) body[s]))     ++s;
        while (e > s && std::isspace((unsigned char) body[e - 1])) --e;
        body = body.substr(s, e - s);

        std::string arguments_json = "{}";
        if (!body.empty() && body.front() == '{') {
            size_t end = walk_balanced_braces(body, 0);
            if (end != std::string::npos) arguments_json = body.substr(0, end);
        }

        common_chat_tool_call tc;
        tc.name      = name;
        tc.arguments = std::move(arguments_json);
        out.push_back(std::move(tc));
        pos = b + close_tag.size();
    }
    return out;
}

// Strip <NAME>...</NAME> spans (NAME a registered tool) from `s` — the
// content-side counterpart to recover_named_tag_tool_calls.
std::string strip_named_tag_blocks(std::string s,
                                   const std::vector<std::string> & tool_names) {
    size_t pos = 0;
    while (pos < s.size()) {
        size_t lt = s.find('<', pos);
        if (lt == std::string::npos) break;
        const std::string * matched = nullptr;
        for (const auto & name : tool_names) {
            if (name.empty()) continue;
            if (s.compare(lt + 1, name.size(), name) == 0 &&
                lt + 1 + name.size() < s.size() &&
                s[lt + 1 + name.size()] == '>') {
                matched = &name;
                break;
            }
        }
        if (!matched) { pos = lt + 1; continue; }
        const std::string close_tag = "</" + *matched + ">";
        size_t end = s.find(close_tag, lt);
        if (end == std::string::npos) { s.erase(lt); break; }
        s.erase(lt, end + close_tag.size() - lt);
        pos = lt;
    }
    return s;
}

// Hermes-style tool call recovery.  Some Qwen3 fine-tunes (notably the
// user's eng_v5 35B-A3) emit tool calls in this XML-ish format instead of
// the Qwen3 JSON one:
//
//     <tool_call>
//     <function=web_search>
//     <parameter=query>
//     top news today
//     </parameter>
//     <parameter=max_results>
//     10
//     </parameter>
//     </function>
//     </tool_call>
//
// The PEG parser refuses it and the whole block leaks into msg.content.
// We rebuild a {name, JSON-arguments} pair by scanning the inner XML.
// Numeric-looking parameter values are emitted as JSON numbers; everything
// else is emitted as JSON strings.
std::vector<common_chat_tool_call> recover_hermes_tool_calls(const std::string & raw) {
    std::vector<common_chat_tool_call> out;
    static const std::string open_tag  = "<tool_call>";
    static const std::string close_tag = "</tool_call>";
    static const std::string fn_open   = "<function=";
    static const std::string fn_close  = "</function>";
    static const std::string par_open  = "<parameter=";
    static const std::string par_close = "</parameter>";

    auto trim = [](std::string s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char) s[a])) ++a;
        while (b > a && std::isspace((unsigned char) s[b - 1])) --b;
        return s.substr(a, b - a);
    };
    auto looks_numeric = [](const std::string & s) {
        if (s.empty()) return false;
        size_t i = 0;
        if (s[0] == '-' || s[0] == '+') ++i;
        bool seen_digit = false, seen_dot = false;
        for (; i < s.size(); ++i) {
            char c = s[i];
            if (c >= '0' && c <= '9') { seen_digit = true; continue; }
            if (c == '.' && !seen_dot) { seen_dot = true; continue; }
            return false;
        }
        return seen_digit;
    };
    auto json_escape = [](const std::string & s) {
        std::string o; o.reserve(s.size() + 4);
        for (char c : s) {
            switch (c) {
                case '\\': o += "\\\\"; break;
                case '"':  o += "\\\""; break;
                case '\n': o += "\\n";  break;
                case '\r': o += "\\r";  break;
                case '\t': o += "\\t";  break;
                case '\b': o += "\\b";  break;
                case '\f': o += "\\f";  break;
                default:
                    if ((unsigned char) c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned) c);
                        o += buf;
                    } else {
                        o += c;
                    }
            }
        }
        return o;
    };

    size_t pos = 0;
    while (true) {
        size_t a = raw.find(open_tag, pos);
        if (a == std::string::npos) break;
        size_t body_begin = a + open_tag.size();
        size_t b = raw.find(close_tag, body_begin);
        std::string body = (b == std::string::npos)
                               ? raw.substr(body_begin)
                               : raw.substr(body_begin, b - body_begin);

        // Find <function=NAME>...</function> inside the body.
        size_t fn_a = body.find(fn_open);
        if (fn_a == std::string::npos) {
            pos = (b == std::string::npos) ? raw.size() : b + close_tag.size();
            continue;
        }
        size_t name_begin = fn_a + fn_open.size();
        size_t name_end = body.find('>', name_begin);
        if (name_end == std::string::npos) {
            pos = (b == std::string::npos) ? raw.size() : b + close_tag.size();
            continue;
        }
        std::string name = trim(body.substr(name_begin, name_end - name_begin));
        if (name.empty()) {
            pos = (b == std::string::npos) ? raw.size() : b + close_tag.size();
            continue;
        }
        size_t fn_b = body.find(fn_close, name_end);
        std::string fn_body = (fn_b == std::string::npos)
                                  ? body.substr(name_end + 1)
                                  : body.substr(name_end + 1, fn_b - (name_end + 1));

        // Walk every <parameter=KEY>VAL</parameter> inside the function body.
        std::ostringstream args;
        args << "{";
        size_t p = 0;
        bool first = true;
        while (true) {
            size_t pa = fn_body.find(par_open, p);
            if (pa == std::string::npos) break;
            size_t key_begin = pa + par_open.size();
            size_t key_end = fn_body.find('>', key_begin);
            if (key_end == std::string::npos) break;
            std::string key = trim(fn_body.substr(key_begin, key_end - key_begin));
            size_t val_begin = key_end + 1;
            size_t pb = fn_body.find(par_close, val_begin);
            std::string val = (pb == std::string::npos)
                                  ? fn_body.substr(val_begin)
                                  : fn_body.substr(val_begin, pb - val_begin);
            val = trim(val);
            if (!first) args << ",";
            args << "\"" << json_escape(key) << "\":";
            if (looks_numeric(val))           args << val;
            else if (val == "true" || val == "false" || val == "null") args << val;
            else                              args << "\"" << json_escape(val) << "\"";
            first = false;
            p = (pb == std::string::npos) ? fn_body.size() : pb + par_close.size();
        }
        args << "}";

        common_chat_tool_call tc;
        tc.name      = std::move(name);
        tc.arguments = args.str();
        out.push_back(std::move(tc));

        pos = (b == std::string::npos) ? raw.size() : b + close_tag.size();
    }
    return out;
}

// Extract a <think>...</think> span out of `s`.  Returns reasoning text
// (without the wrapping tags); `s` is left with the reasoning span removed
// in-place so the remainder is safe to use as visible content.  Tolerates
// missing opener (Qwen3 prefills <think>) and unterminated </think>.
std::string extract_think_block(std::string & s) {
    static const std::string open_tag  = "<think>";
    static const std::string close_tag = "</think>";

    size_t close_pos = s.find(close_tag);
    if (close_pos == std::string::npos) {
        // No closer at all — nothing safe to extract; leave content alone.
        return {};
    }
    size_t open_pos  = s.find(open_tag);
    size_t reasoning_begin, span_begin;
    if (open_pos != std::string::npos && open_pos < close_pos) {
        // Both tags present — span = [<think> ... </think>].
        reasoning_begin = open_pos + open_tag.size();
        span_begin      = open_pos;
    } else {
        // Only </think> — Qwen3-style prefill where <think> is implicit.
        // Treat everything before </think> as reasoning.
        reasoning_begin = 0;
        span_begin      = 0;
    }
    std::string reasoning = s.substr(reasoning_begin, close_pos - reasoning_begin);
    s.erase(span_begin, close_pos + close_tag.size() - span_begin);
    // Trim leading whitespace/newlines that the closing tag left behind.
    size_t lead = 0;
    while (lead < s.size() && (s[lead] == '\n' || s[lead] == '\r' ||
                               s[lead] == ' ' || s[lead] == '\t')) ++lead;
    if (lead) s.erase(0, lead);
    // Trim around the reasoning too.
    size_t a = 0, b = reasoning.size();
    while (a < b && std::isspace((unsigned char) reasoning[a])) ++a;
    while (b > a && std::isspace((unsigned char) reasoning[b - 1])) --b;
    return reasoning.substr(a, b - a);
}

// Markdown-style fake tool-call recovery.
// ---------------------------------------------------------------------------
// Some Qwen3 fine-tunes, when they "lose confidence" in the <tool_call>
// XML format mid-conversation (typically after a few real tool calls,
// some of which errored), give up on the syntax and instead emit a
// *visual* indicator in markdown that mimics how chat UIs render tool
// invocations.  Observed shape:
//
//     *🔧 datetime*
//     *🔧 web_search(query="Hugging Face Daily Papers latest")*
//
// or with bold instead of italics: `**🔧 web_fetch(url="...")**`.
//
// The engine sees these as plain content with tool_calls=0, treats it as
// the final answer, and the user sees an empty-looking bubble.  We
// recover by scanning for the pattern, parsing the args, and re-emitting
// real common_chat_tool_call entries so the agentic loop continues.
//
// Heuristic — the marker must contain the wrench emoji (🔧, UTF-8
// F0 9F 94 A7) so we don't misfire on legitimate prose.
namespace {

bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string trim_str(std::string s) {
    size_t a = 0, b = s.size();
    while (a < b && is_ws(s[a])) ++a;
    while (b > a && is_ws(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Convert a free-form `key=value, key="quoted", key=12` argument list
// into a JSON object string.  Tolerant of either single or double
// quotes, and of bare numeric / boolean values.
std::string kv_args_to_json(const std::string & args) {
    std::ostringstream out;
    out << "{";
    bool first = true;
    size_t i = 0;
    while (i < args.size()) {
        while (i < args.size() && (is_ws(args[i]) || args[i] == ',')) ++i;
        if (i >= args.size()) break;
        // Key: identifier chars + dashes/dots.
        size_t k0 = i;
        while (i < args.size() &&
               (std::isalnum((unsigned char) args[i]) || args[i] == '_' ||
                args[i] == '-' || args[i] == '.')) ++i;
        if (i == k0) break;
        std::string key = args.substr(k0, i - k0);
        while (i < args.size() && is_ws(args[i])) ++i;
        if (i >= args.size() || args[i] != '=') break;
        ++i;  // past '='
        while (i < args.size() && is_ws(args[i])) ++i;
        if (i >= args.size()) break;
        // Value: quoted or bare.
        std::string raw_val;
        bool quoted = false;
        if (args[i] == '"' || args[i] == '\'') {
            char q = args[i++];
            std::string buf;
            while (i < args.size() && args[i] != q) {
                if (args[i] == '\\' && i + 1 < args.size()) {
                    buf += args[i + 1];
                    i += 2;
                    continue;
                }
                buf += args[i++];
            }
            if (i < args.size()) ++i;  // past closing quote
            raw_val = std::move(buf);
            quoted = true;
        } else {
            size_t v0 = i;
            while (i < args.size() && args[i] != ',' && !is_ws(args[i])) ++i;
            raw_val = args.substr(v0, i - v0);
        }
        if (!first) out << ",";
        first = false;
        // JSON escape the key.
        out << "\"";
        for (char c : key) {
            if (c == '"' || c == '\\') out << '\\';
            out << c;
        }
        out << "\":";
        // Decide value type: quoted -> string; bare numeric / bool / null -> as-is.
        if (!quoted) {
            // Numeric?
            bool numeric = !raw_val.empty();
            for (size_t j = 0; j < raw_val.size() && numeric; ++j) {
                char c = raw_val[j];
                if (j == 0 && (c == '-' || c == '+')) continue;
                if (c >= '0' && c <= '9') continue;
                if (c == '.' || c == 'e' || c == 'E') continue;
                numeric = false;
            }
            if (numeric)                                            { out << raw_val; continue; }
            if (raw_val == "true" || raw_val == "false" || raw_val == "null") {
                out << raw_val;
                continue;
            }
        }
        out << "\"";
        for (char c : raw_val) {
            if (c == '"' || c == '\\') out << '\\';
            else if (c == '\n')        { out << "\\n"; continue; }
            else if (c == '\r')        { out << "\\r"; continue; }
            else if (c == '\t')        { out << "\\t"; continue; }
            out << c;
        }
        out << "\"";
    }
    out << "}";
    return out.str();
}

// Find every `*🔧 NAME(...)*` (or `**🔧 NAME(...)**`) pattern in `text`.
// Returns the list of recovered tool_calls AND fills `out_marker_spans`
// with the [begin, end) byte ranges of the markers so the caller can
// strip them from the visible content.
struct MdMarkerSpan { size_t begin; size_t end; };
std::vector<common_chat_tool_call>
recover_markdown_tool_calls(const std::string & text,
                            std::vector<MdMarkerSpan> * out_marker_spans) {
    std::vector<common_chat_tool_call> out;
    // 🔧 (wrench, U+1F527) in UTF-8.
    static const char wrench[]   = "\xF0\x9F\x94\xA7";
    static const char hammer_w[] = "\xF0\x9F\x9B\xA0";  // 🛠 base; we accept variants
    size_t pos = 0;
    while (pos < text.size()) {
        // Find next '*' that may start a marker.
        size_t a = text.find('*', pos);
        if (a == std::string::npos) break;
        size_t star_begin = a;
        size_t i = a + 1;
        if (i < text.size() && text[i] == '*') ++i;  // **
        // Skip whitespace after the leading stars.
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        // Check for the wrench emoji.
        bool has_emoji = false;
        if (i + 4 <= text.size() && std::memcmp(&text[i], wrench, 4) == 0) {
            i += 4;
            has_emoji = true;
        } else if (i + 4 <= text.size() && std::memcmp(&text[i], hammer_w, 4) == 0) {
            i += 4;
            has_emoji = true;
            // Skip optional VS16 (U+FE0F, EF B8 8F) emoji presentation selector.
            if (i + 3 <= text.size() &&
                (unsigned char) text[i]     == 0xEF &&
                (unsigned char) text[i + 1] == 0xB8 &&
                (unsigned char) text[i + 2] == 0x8F) i += 3;
        }
        if (!has_emoji) { pos = star_begin + 1; continue; }
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        // Tool name: identifier chars.
        size_t n0 = i;
        while (i < text.size() &&
               (std::isalnum((unsigned char) text[i]) || text[i] == '_')) ++i;
        if (i == n0) { pos = star_begin + 1; continue; }
        std::string name = text.substr(n0, i - n0);
        // Optional `(args)` block — match parens with depth + quote awareness.
        std::string args_inner;
        bool got_args = false;
        if (i < text.size() && text[i] == '(') {
            size_t arg_begin = i + 1;
            int depth = 1;
            bool in_str = false;
            char q = 0;
            bool esc = false;
            size_t j = arg_begin;
            for (; j < text.size() && depth > 0; ++j) {
                char c = text[j];
                if (esc) { esc = false; continue; }
                if (in_str) {
                    if (c == '\\') esc = true;
                    else if (c == q) in_str = false;
                    continue;
                }
                if (c == '"' || c == '\'') { in_str = true; q = c; continue; }
                if (c == '(') ++depth;
                else if (c == ')') --depth;
            }
            if (depth == 0) {
                args_inner = text.substr(arg_begin, (j - 1) - arg_begin);
                i = j;
                got_args = true;
            } else {
                // Unbalanced parens — bail on this candidate.
                pos = star_begin + 1;
                continue;
            }
        }
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        // Closing star(s).
        if (i < text.size() && text[i] == '*') {
            ++i;
            if (i < text.size() && text[i] == '*') ++i;
        } else {
            // No closing star — this may still be a one-line pattern at
            // end-of-stream; accept as long as we already captured a name.
        }
        size_t end = i;
        // Eat trailing newline so the content strips cleanly.
        if (end < text.size() && text[end] == '\n') ++end;

        common_chat_tool_call tc;
        tc.name      = std::move(name);
        tc.arguments = got_args ? kv_args_to_json(trim_str(args_inner)) : std::string("{}");
        out.push_back(std::move(tc));
        if (out_marker_spans) out_marker_spans->push_back({ star_begin, end });
        pos = end;
    }
    return out;
}

// Strip the spans returned by recover_markdown_tool_calls from `s`.
std::string strip_marker_spans(const std::string & s,
                               const std::vector<MdMarkerSpan> & spans) {
    if (spans.empty()) return s;
    std::string out;
    out.reserve(s.size());
    size_t cursor = 0;
    for (const auto & sp : spans) {
        if (sp.begin > cursor) out.append(s, cursor, sp.begin - cursor);
        cursor = sp.end;
    }
    if (cursor < s.size()) out.append(s, cursor, s.size() - cursor);
    return trim_str(out);
}

}  // namespace (markdown-recovery helpers)

// Heuristic: does this short reply look like an "announce-without-action"
// turn — the model promising to do something ("Let me search…", "I'll
// look that up…") but emitting no tool_call? Used by chat_continue's
// retry-with-nudge path. We match a small library of leading phrases
// against a lower-cased copy. False positives lose the user one nudge
// round-trip; false negatives let an announce-only turn through. Empty
// content also counts as announce — model finished early without saying
// anything actionable, so a nudge is the safe move.
bool looks_like_announce_phrase(const std::string & s) {
    if (s.empty()) return true;
    std::string lc;
    lc.reserve(s.size());
    for (char c : s) lc.push_back((char) std::tolower((unsigned char) c));
    static const char * patterns[] = {
        "let me ",
        "i'll ",
        "i will ",
        "i'm going to ",
        "i am going to ",
        "let's ",
        "one moment",
        "hold on",
        "give me a moment",
        "give me a sec",
        "searching ",
        "looking up",
        "looking that up",
        "checking the",
        "fetching ",
        "i'll check",
        "i'll look",
        "i'll search",
    };
    for (const char * p : patterns) {
        if (lc.find(p) != std::string::npos) return true;
    }
    return false;
}

}  // namespace (top-level helpers)

// ===========================================================================
// Engine::Impl
// ===========================================================================
struct Engine::Impl {
    common_params               params;
    common_init_result_ptr      init;
    common_chat_templates_ptr   templates;
    common_sampler            * sampler = nullptr;

    // ------ speculative decoding (see Engine::spec_type) ------------------
    // MTP: the draft model is the SAME as the target — we open a second
    //   llama_context against model_tgt with LLAMA_CONTEXT_TYPE_MTP.
    // Draft-simple / eagle3: a SEPARATE model is loaded from
    //   params.speculative.draft.mparams.path; model_dft owns it.
    common_speculative_ptr      spec;                  // null = no speculation
    llama_context             * ctx_dft       = nullptr; // owned (MTP + draft-simple)
    llama_model               * model_dft     = nullptr; // owned (draft-simple / eagle3 only)
    bool                        spec_mtp      = false;   // true iff spec is DRAFT_MTP
    bool                        spec_active   = false;   // true iff spec successfully initialised
    // Per-generation stats (reset in chat_continue's outer loop).
    uint64_t                    spec_drafted  = 0;
    uint64_t                    spec_accepted = 0;

    std::vector<common_chat_msg> history;
    std::vector<Tool>            tools;

    std::string  system_prompt;
    int          max_new_tokens = -1;
    bool         loaded         = false;
    bool         verbose        = false;
    bool         enable_thinking = true;   // sent to chat templates that use it
    // Optional path to a Jinja chat-template file. Empty = use the
    // template embedded in the GGUF. Resolved (read into a string) by
    // load() and passed as the override to common_chat_templates_init.
    std::string  chat_template_file;
    // Reasoning extraction format for render(). Default AUTO (≡ DEEPSEEK
    // for templates that support it). Overridden by Engine::reasoning_format.
    common_reasoning_format reasoning_format = COMMON_REASONING_FORMAT_AUTO;
    common_chat_tool_choice tool_choice = COMMON_CHAT_TOOL_CHOICE_AUTO;
    bool         parallel_tool_calls    = false;
    int          max_tool_hops          = 8;     // default agentic safety cap
    bool         retry_on_incomplete    = true;  // see Engine::retry_on_incomplete
    int          max_incomplete_retries = 10;    // see Engine::max_incomplete_retries
    int          stop_at_ctx_pct        = 100;   // 0 disables; otherwise abort
                                                  // chat_continue when KV >= this %
    bool         last_was_ctx_full      = false;

    TokenCallback    on_token;
    ToolCallback     on_tool;
    HopResetCallback         on_hop_reset;
    IncompleteRetryCallback  on_incomplete_retry;
    PromptEvalCallback       on_prompt_eval;
    PromptProgressCallback   on_prompt_progress;

    // Cooperative cancel — flipped from another thread (typically the
    // server's SSE provider when the client drops) and polled by the
    // decode loop between every sampled token, plus the chat_continue
    // loop between agentic hops. See Engine::request_cancel for the
    // design rationale.
    std::atomic<bool> cancel_requested{false};

    std::string last_error;
    std::string backend_summary;

    Impl() {
        // Sensible chat defaults overlaid on the llama.cpp baseline.
        params.n_ctx                 = 4096;
        params.n_batch               = 4096;
        params.n_predict             = -1;
        params.cpuparams.n_threads   = std::max(1, (int) std::thread::hardware_concurrency() / 2);
        params.warmup                = false;
        params.n_gpu_layers          = -1;  // auto
        params.sampling.temp         = 0.7f;
        params.sampling.top_p        = 0.95f;
        params.sampling.top_k        = 40;
        params.sampling.min_p        = 0.05f;
        params.sampling.penalty_repeat = 1.1f;
    }

    ~Impl() {
        if (sampler) {
            common_sampler_free(sampler);
            sampler = nullptr;
        }
        // spec is unique_ptr; frees itself (releases internal ctx_dft
        // for draft-simple, but NOT for MTP where we own ctx_dft).
        spec.reset();
        // ctx_dft is a raw llama_context owned by us (MTP creates it
        // directly; draft-simple also stores it here after loading).
        // Free BEFORE the model it was created from goes out of scope.
        if (ctx_dft) {
            llama_free(ctx_dft);
            ctx_dft = nullptr;
        }
        // model_dft is the standalone draft model (draft-simple / eagle3).
        // Free AFTER ctx_dft since the context references the model.
        if (model_dft) {
            llama_model_free(model_dft);
            model_dft = nullptr;
        }
        // common_init_result_ptr + common_chat_templates_ptr free themselves.
    }

    // -------- helpers ---------------------------------------------------
    llama_model   * model() const { return init ? init->model()   : nullptr; }
    llama_context * ctx()   const { return init ? init->context() : nullptr; }

    std::vector<common_chat_tool> chat_tools() const {
        std::vector<common_chat_tool> out;
        out.reserve(tools.size());
        for (const auto & t : tools) {
            out.push_back({ t.name, t.description, t.parameters_json });
        }
        return out;
    }

    // Build the rendered prompt for the *full* current history (incl. any
    // assistant/tool messages). add_generation_prompt asks for the assistant
    // turn header.
    common_chat_params render(bool add_generation_prompt) const {
        common_chat_templates_inputs in;
        in.messages              = history;
        in.add_generation_prompt = add_generation_prompt;
        in.use_jinja             = true;
        in.tools                 = chat_tools();
        in.tool_choice           = tool_choice;
        in.parallel_tool_calls   = parallel_tool_calls;
        in.enable_thinking       = enable_thinking;
        // Tell the template builder to wire reasoning extraction into the
        // PEG parser it produces.  Without this the parser leaves <think>
        // content inside msg.content, so the streaming code can't split
        // reasoning_content from content.  Default AUTO maps to DEEPSEEK
        // (the canonical "extract <think>...</think> blocks") for every
        // template that supports thinking; user can override via
        // Engine::reasoning_format (--reasoning-format on the CLI).
        in.reasoning_format      = reasoning_format;
        return common_chat_templates_apply(templates.get(), in);
    }

    bool feed_prompt(const std::string & prompt, int & n_past_inout) {
        // Tokenize and decode the new prompt span past whatever is already
        // in the KV cache (n_past tokens for sequence 0).
        const llama_vocab * vocab = llama_model_get_vocab(model());
        std::vector<llama_token> toks =
            common_tokenize(vocab, prompt, /*add_special=*/n_past_inout == 0,
                            /*parse_special=*/true);

        if (toks.empty()) return true;

        const int n_ctx = llama_n_ctx(ctx());
        if (n_past_inout + (int) toks.size() > n_ctx) {
            last_error = "prompt does not fit context (need "
                         + std::to_string(n_past_inout + toks.size())
                         + ", have " + std::to_string(n_ctx) + ")";
            return false;
        }

        const int n_batch = params.n_batch > 0 ? params.n_batch : 512;
        for (size_t i = 0; i < toks.size(); i += n_batch) {
            int n = std::min<int>(n_batch, toks.size() - i);
            llama_batch b = llama_batch_get_one(toks.data() + i, n);
            if (llama_decode(ctx(), b) != 0) {
                last_error = "llama_decode failed while feeding prompt";
                return false;
            }
            // For MTP: feed the same batch to the speculative pipeline so
            // its internal state machine tracks the prompt evolution. The
            // MTP impl uses this to keep its draft context aligned with
            // the target's KV cache.
            if (spec_active) {
                common_speculative_process(spec.get(), b);
            }
            n_past_inout += n;
        }
        // Tell the speculative pipeline this is the start of a new
        // generation against this seq. MTP doesn't actually consume the
        // prompt tokens (it reads them from the shared model state via
        // the draft context), but the call wires up the per-seq state
        // the next draft() invocation needs. Idempotent if called
        // multiple times for the same generation.
        if (spec_active) {
            // Reset per-generation acceptance counters.
            spec_drafted  = 0;
            spec_accepted = 0;
            common_speculative_begin(spec.get(), /*seq_id=*/0, toks);
        }
        return true;
    }

    // Speculative-decoding generation loop. Works for ALL spec types
    // (MTP, draft-simple, eagle3, ngram-*) via the generic
    // common_speculative_* API — each type's impl dispatches internally.
    //
    // The prompt has already been decoded into ctx() (target) and fed
    // to the spec pipeline via feed_prompt's common_speculative_process
    // / _begin calls. Each loop iteration:
    //   1. Set draft params (n_past, id_last, ...) and ask for drafts.
    //   2. Build a batch with [last_id, draft0, ..., draftN-1], all
    //      with logits=true so we can verify every position.
    //   3. Decode on target; feed the same batch to the spec pipeline.
    //   4. common_sampler_sample_and_accept_n verifies each draft
    //      against the target's logits, returning accepted-prefix.
    //   5. Tell spec how many drafts were accepted; the last accepted
    //      token becomes the next last_id.
    std::string generate_until_done_spec(int & n_past_inout) {
        const llama_vocab * vocab = llama_model_get_vocab(model());

        std::string raw;
        int generated = 0;
        const int budget = max_new_tokens > 0
                               ? max_new_tokens
                               : params.n_predict > 0 ? params.n_predict : -1;
        const int n_ctx        = llama_n_ctx(ctx());
        const int n_draft_max  = params.speculative.draft.n_max;

        // Sample the FIRST token from the prompt's last-position logits.
        // From here on, last_id becomes the token sitting at the end of
        // the decoded sequence — we hand it to spec_draft as id_last.
        llama_token last_id = common_sampler_sample(sampler, ctx(), -1);
        common_sampler_accept(sampler, last_id, /*accept_grammar=*/true);

        // Reusable scratch — recreated every iter because draft size
        // varies (1..n_max+1 tokens).
        llama_tokens draft;
        std::vector<int> i_logits;        // batch positions to sample at

        while (true) {
            if (cancel_requested.load(std::memory_order_relaxed)) {
                if (verbose) std::fprintf(stderr,
                    "[easyai] generate cancelled (after %d tokens)\n", generated);
                last_error = "cancelled";
                break;
            }

            // EOG check on the most recent accepted token, mirroring the
            // non-spec path's break point.
            if (llama_vocab_is_eog(vocab, last_id)) break;

            // Emit `last_id` to the caller. We do this BEFORE adding it
            // to the next batch so streaming sees tokens as they're
            // accepted, not buffered.
            std::string piece = common_token_to_piece(ctx(), last_id, /*special=*/false);
            raw += piece;
            if (on_token) on_token(piece);
            ++generated;
            if (budget > 0 && generated >= budget) break;
            if (n_past_inout + 1 >= n_ctx) {
                if (verbose) std::fprintf(stderr, "[easyai] context full, stopping\n");
                break;
            }

            // --- 0. Align ctx_dft to target's CURRENT position ----------
            // Previous iter's process() decoded the full verify batch
            // (last_id + K drafts) into ctx_dft. After sample_and_accept_n
            // accepted A tokens, target trimmed [n_past+A..n_past+K+1)
            // but ctx_dft still has those rejected positions. Trim here
            // so ctx_dft is back in lockstep with target before draft()
            // runs. Applies to all spec types that use a draft context
            // (MTP, draft-simple, eagle3). For the very first iter (no
            // leftover state from feed_prompt) this is a no-op.
            llama_memory_seq_rm(
                llama_get_memory(ctx_dft), /*seq=*/0,
                n_past_inout, /*p1=*/-1);

            // --- 1. Ask MTP for draft tokens following last_id ----------
            draft.clear();
            auto & dp = common_speculative_get_draft_params(spec.get(), /*seq=*/0);
            dp.drafting = true;
            dp.n_max    = std::min(n_draft_max,
                                   std::max(0, n_ctx - n_past_inout - 1));
            dp.n_past   = n_past_inout;
            dp.id_last  = last_id;
            dp.result   = &draft;
            // MTP and draft-simple don't read .prompt (they reuse KV
            // state via ctx_dft). ngram-* impls DO read it — set to
            // null for now since ngram isn't wired through this loop.
            dp.prompt   = nullptr;

            common_speculative_draft(spec.get());
            spec_drafted += draft.size();

            // MTP's draft() decodes each drafted token into ctx_dft to
            // chain predictions (see common_speculative_state_draft_mtp::
            // draft in speculative.cpp). Those positions now sit in
            // ctx_dft's KV at [n_past_inout .. n_past_inout + draft.size()).
            // common_speculative_process() about to run NEXT also decodes
            // the verify batch on ctx_dft at the SAME positions — a
            // conflict that triggers M-RoPE's "X < Y" position check and
            // fails. Trim ctx_dft back to (n_past_inout - 1) so process()
            // starts from a clean slate. Mirrors llama-server's
            // common_context_seq_rm(ctx_dft, ...) at
            // tools/server/server-context.cpp:2352.
            llama_memory_seq_rm(
                llama_get_memory(ctx_dft), /*seq=*/0,
                n_past_inout, /*p1=*/-1);

            // --- 2. Build target batch: [last_id, draft[0..N-1]] --------
            // All positions need logits=true so sample_and_accept_n can
            // verify each draft. We use llama_batch_init / common_batch_add
            // (not llama_batch_get_one which doesn't expose per-token
            // logits flags).
            const int n_tokens = 1 + (int) draft.size();
            llama_batch b = llama_batch_init(n_tokens, /*embd=*/0, /*n_seq_max=*/1);
            common_batch_add(b, last_id, n_past_inout, {0}, /*logits=*/true);
            i_logits.clear();
            i_logits.push_back(0);  // position of last_id in batch
            for (size_t k = 0; k < draft.size(); ++k) {
                common_batch_add(b, draft[k],
                                 n_past_inout + 1 + (int) k,
                                 {0}, /*logits=*/true);
                i_logits.push_back(1 + (int) k);
            }

            // --- 3. Decode on target + feed spec pipeline ----------------
            if (llama_decode(ctx(), b) != 0) {
                llama_batch_free(b);
                last_error = "llama_decode failed during speculative generation";
                break;
            }
            common_speculative_process(spec.get(), b);

            // --- 4. Verify drafts + sample next continuation -----------
            // Returns: vector of accepted tokens (size 1..draft.size()+1).
            // The first element is the verified continuation of last_id
            // (which always exists — equals draft[0] if accepted, the
            // resampled token if not). Subsequent elements are accepted
            // drafts. The last element is the new last_id.
            auto accepted = common_sampler_sample_and_accept_n(
                sampler, ctx(), i_logits, draft);
            llama_batch_free(b);

            if (accepted.empty()) {
                // Defensive — should never happen per llama-server's
                // GGML_ASSERT(accepted.size() >= 1).
                last_error = "sample_and_accept_n returned no tokens";
                break;
            }

            // --- 5. Tell spec how many DRAFTS were accepted ------------
            // accepted.size() - 1 = drafts accepted (the +1 is the
            // mandatory new sample at position 0).
            const uint16_t n_draft_accepted = (uint16_t) (accepted.size() - 1);
            common_speculative_accept(spec.get(), /*seq=*/0, n_draft_accepted);
            spec_accepted += n_draft_accepted;

            // The KV cache now holds [last_id, draft0, ..., draftN-1]
            // (all n_tokens positions). But we only accepted
            // accepted.size() = 1 + n_draft_accepted of them. Trim KV
            // back to keep only the accepted positions.
            const int n_evicted = (int) draft.size() - (int) n_draft_accepted;
            if (n_evicted > 0) {
                // Wipe positions [n_past_inout + accepted.size() .. n_past_inout + n_tokens)
                llama_memory_seq_rm(
                    llama_get_memory(ctx()), /*seq=*/0,
                    n_past_inout + (int) accepted.size(),
                    n_past_inout + n_tokens);
            }
            n_past_inout += (int) accepted.size();

            // --- 6. Emit accepted draft tokens (skip index 0 which is
            //         the new sample — we emit it next iter as last_id). --
            // Actually, ALL accepted entries except the last need to be
            // emitted now; the last becomes the next last_id (emitted at
            // the top of the next iter).
            for (size_t k = 0; k + 1 < accepted.size(); ++k) {
                llama_token id = accepted[k];
                if (llama_vocab_is_eog(vocab, id)) {
                    // EOG mid-draft → emit nothing past it and stop.
                    return raw;
                }
                std::string p = common_token_to_piece(ctx(), id, /*special=*/false);
                raw += p;
                if (on_token) on_token(p);
                ++generated;
                if (budget > 0 && generated >= budget) {
                    last_id = id;
                    return raw;
                }
            }
            last_id = accepted.back();
        }
        return raw;
    }

    // Autoregressive generation — the path used when speculation isn't active.
    // Generate tokens until EOG, tool-call grammar trigger, or max_new_tokens.
    // Returns the raw assistant text (may contain tool-call syntax).
    std::string generate_until_done(int & n_past_inout) {
        if (spec_active) {
            return generate_until_done_spec(n_past_inout);
        }
        const llama_vocab * vocab = llama_model_get_vocab(model());

        std::string raw;
        int generated = 0;
        const int budget = max_new_tokens > 0
                               ? max_new_tokens
                               : params.n_predict > 0 ? params.n_predict : -1;

        const int n_ctx = llama_n_ctx(ctx());

        llama_token id = 0;
        while (true) {
            // Cooperative cancel — checked every sampled token. Flipped
            // by the SSE provider when the client connection drops, so
            // the engine doesn't keep decoding into a dead socket.
            if (cancel_requested.load(std::memory_order_relaxed)) {
                if (verbose) std::fprintf(stderr,
                    "[easyai] generate cancelled (after %d tokens)\n", generated);
                last_error = "cancelled";
                break;
            }

            id = common_sampler_sample(sampler, ctx(), -1);
            common_sampler_accept(sampler, id, /*accept_grammar=*/true);

            if (llama_vocab_is_eog(vocab, id)) break;

            std::string piece = common_token_to_piece(ctx(), id, /*special=*/false);
            raw += piece;
            if (on_token) on_token(piece);

            ++generated;
            if (budget > 0 && generated >= budget) break;
            if (n_past_inout + 1 >= n_ctx) {
                if (verbose) std::fprintf(stderr, "[easyai] context full, stopping\n");
                break;
            }

            llama_batch b = llama_batch_get_one(&id, 1);
            if (llama_decode(ctx(), b) != 0) {
                last_error = "llama_decode failed during generation";
                break;
            }
            ++n_past_inout;
        }
        return raw;
    }

    // Parse model output into a structured chat message according to the
    // current chat template (handles native + PEG tool-call formats).
    // The PEG arena is shipped as a serialized string in chat_params.parser;
    // we load it into the parser_params before dispatching.
    common_chat_msg parse_assistant(const std::string & raw, const common_chat_params & p) {
        common_chat_parser_params pp(p);
        pp.parse_tool_calls = true;
        if (!p.parser.empty()) {
            try { pp.parser.load(p.parser); }
            catch (const std::exception & e) {
                std::fprintf(stderr,
                    "[easyai] failed to load chat parser arena: %s\n", e.what());
            }
        }
        // 1) Try the official parser first.
        common_chat_msg msg;
        bool parser_threw = false;
        try {
            msg = common_chat_parse(raw, /*is_partial=*/false, pp);
        } catch (const std::exception & e) {
            const size_t tail = std::min<size_t>(600, raw.size());
            std::fprintf(stderr,
                "[easyai] chat parser failed (%s) — attempting recovery\n"
                "[easyai] broken raw (%zu bytes, last %zu):\n%.*s\n",
                e.what(), raw.size(), tail,
                (int) tail, raw.c_str() + raw.size() - tail);
            parser_threw = true;
            msg.role    = "assistant";
            msg.content = raw;
            easyai::log::mark_problem(
                "Engine::parse_assistant common_chat_parse threw (%s)\n"
                "raw_bytes=%zu — falling back to recovery passes\n"
                "raw tail (last %zu bytes):\n%.*s",
                e.what(), raw.size(),
                std::min<size_t>(400, raw.size()),
                (int) std::min<size_t>(400, raw.size()),
                raw.c_str() + raw.size() - std::min<size_t>(400, raw.size()));
        }

        // 2a) When the parser threw and dumped raw into content, we still
        //     want to keep reasoning separated from visible content so the
        //     streaming layer doesn't double-render the <think> block (once
        //     as reasoning_content_delta from the partial parses that DID
        //     succeed, and again as content_delta from the last-resort
        //     fallback that re-emits the engine's final text).
        if (parser_threw && msg.reasoning_content.empty()) {
            std::string r = extract_think_block(msg.content);
            if (!r.empty()) {
                msg.reasoning_content = std::move(r);
                std::fprintf(stderr,
                    "[easyai] split <think> block from raw fallback (reasoning=%zu, "
                    "content=%zu)\n", msg.reasoning_content.size(), msg.content.size());
            }
        }

        // 2b) Recovery pass — if the official parser produced no tool_calls
        //     but the raw output contains <tool_call> markers, the model
        //     most likely emitted Qwen-style doubled-brace JSON OR the
        //     Hermes-XML <function=name>/<parameter=key> shape that the PEG
        //     grammar refused.  Pull what we can out of it by hand.
        if (msg.tool_calls.empty() && raw.find("<tool_call>") != std::string::npos) {
            auto recovered = recover_qwen_tool_calls(raw);
            const char * recovery_kind = "qwen";
            if (recovered.empty() || recovered.front().arguments == "{}") {
                auto hermes = recover_hermes_tool_calls(raw);
                if (!hermes.empty()) { recovered = std::move(hermes); recovery_kind = "hermes"; }
            }
            if (!recovered.empty()) {
                msg.tool_calls = std::move(recovered);
                msg.content    = strip_tool_call_blocks(msg.content);
                const size_t tail = std::min<size_t>(600, raw.size());
                std::fprintf(stderr,
                    "[easyai] recovered %zu tool call(s) from malformed output (%s)\n"
                    "[easyai] broken raw (%zu bytes, last %zu):\n%.*s\n",
                    msg.tool_calls.size(), recovery_kind,
                    raw.size(), tail,
                    (int) tail, raw.c_str() + raw.size() - tail);
                easyai::log::mark_problem(
                    "Engine::parse_assistant recovered %zu tool call(s) "
                    "from malformed %s output (PEG parser refused)\n"
                    "raw_bytes=%zu",
                    msg.tool_calls.size(), recovery_kind, raw.size());
            }
        }

        // 2c) Markdown-marker recovery — when the model abandons the
        //     <tool_call> XML and instead writes `*🔧 toolname(args)*`
        //     as plain content (typically after a few real calls some
        //     of which errored), the engine would otherwise return the
        //     markers as the FINAL answer (tool_calls=0) and the chat
        //     loop terminates with the user seeing two italicised
        //     wrench lines as the "reply".  Recover the intent and
        //     surface real tool_calls so the agentic loop continues.
        if (msg.tool_calls.empty() && !msg.content.empty() &&
                msg.content.find("\xF0\x9F\x94\xA7") != std::string::npos) {
            std::vector<MdMarkerSpan> spans;
            auto recovered = recover_markdown_tool_calls(msg.content, &spans);
            if (!recovered.empty()) {
                msg.tool_calls = std::move(recovered);
                msg.content    = strip_marker_spans(msg.content, spans);
                const size_t tail = std::min<size_t>(600, raw.size());
                std::fprintf(stderr,
                    "[easyai] recovered %zu tool call(s) from markdown markers "
                    "(model abandoned <tool_call> syntax — agentic loop continues)\n"
                    "[easyai] broken raw (%zu bytes, last %zu):\n%.*s\n",
                    msg.tool_calls.size(),
                    raw.size(), tail,
                    (int) tail, raw.c_str() + raw.size() - tail);
                easyai::log::mark_problem(
                    "Engine::parse_assistant recovered %zu tool call(s) "
                    "from markdown wrench markers (model abandoned <tool_call> XML)",
                    msg.tool_calls.size());
            }
        }

        // 2d) Named-tag recovery — LAST resort. Some weak / aggressively
        //     quantised models drop the <tool_call> envelope entirely and
        //     use the tool NAME as the XML tag: `<tool_lookup></tool_lookup>`.
        //     There's no distinctive marker to gate on (the tag IS a tool
        //     name), so this is the most false-positive-prone heuristic —
        //     it runs only when (a) every other recovery came up empty and
        //     (b) stripping the named-tag spans leaves a whitespace-only
        //     remainder, i.e. the WHOLE visible turn was just tool-name
        //     tags. A real prose answer that happens to mention `<plan>`
        //     keeps a non-empty remainder and is left untouched.
        if (msg.tool_calls.empty() && !msg.content.empty()) {
            std::vector<std::string> names;
            names.reserve(tools.size());
            for (const auto & t : tools) names.push_back(t.name);
            auto recovered = recover_named_tag_tool_calls(msg.content, names);
            if (!recovered.empty()) {
                std::string stripped = strip_named_tag_blocks(msg.content, names);
                const bool only_ws = std::all_of(
                    stripped.begin(), stripped.end(),
                    [](unsigned char c) { return std::isspace(c); });
                if (only_ws) {
                    msg.tool_calls = std::move(recovered);
                    msg.content.clear();
                    const size_t tail = std::min<size_t>(600, raw.size());
                    std::fprintf(stderr,
                        "[easyai] recovered %zu tool call(s) from bare named "
                        "tags (model used <toolname> instead of <tool_call>)\n"
                        "[easyai] broken raw (%zu bytes, last %zu):\n%.*s\n",
                        msg.tool_calls.size(), raw.size(), tail,
                        (int) tail, raw.c_str() + raw.size() - tail);
                    easyai::log::mark_problem(
                        "Engine::parse_assistant recovered %zu tool call(s) "
                        "from bare <toolname> tags (model dropped the "
                        "<tool_call> envelope)",
                        msg.tool_calls.size());
                }
            }
        }
        return msg;
    }

    // Find the registered tool by name (back-compat aliases resolved).
    const Tool * find_tool(const std::string & name) const {
        const std::string canon = canonical_tool_name(name);
        for (const auto & t : tools) if (t.name == canon) return &t;
        return nullptr;
    }
};

// ===========================================================================
// Engine — public API
// ===========================================================================
Engine::Engine() : p_(std::make_unique<Impl>()) {}
Engine::~Engine() = default;
Engine::Engine(Engine &&) noexcept = default;
Engine & Engine::operator=(Engine &&) noexcept = default;

Engine & Engine::model(std::string path)        { p_->params.model.path = std::move(path); return *this; }
Engine & Engine::context(int n)                 { p_->params.n_ctx = n; if (p_->params.n_batch > n) p_->params.n_batch = n; return *this; }
Engine & Engine::batch(int n)                   { p_->params.n_batch = n; return *this; }
Engine & Engine::gpu_layers(int n)              { p_->params.n_gpu_layers = n; return *this; }
Engine & Engine::threads(int n)                 { p_->params.cpuparams.n_threads = n; p_->params.cpuparams_batch.n_threads = n; return *this; }
Engine & Engine::seed(uint32_t s)               { p_->params.sampling.seed = s; return *this; }
Engine & Engine::system(std::string s)          { p_->system_prompt = std::move(s); return *this; }
Engine & Engine::temperature(float t)           { p_->params.sampling.temp = t; return *this; }
Engine & Engine::top_p(float v)                 { p_->params.sampling.top_p = v; return *this; }
Engine & Engine::top_k(int v)                   { p_->params.sampling.top_k = v; return *this; }
Engine & Engine::min_p(float v)                 { p_->params.sampling.min_p = v; return *this; }
Engine & Engine::repeat_penalty(float v)        { p_->params.sampling.penalty_repeat = v; return *this; }
Engine & Engine::presence_penalty(float v)      { p_->params.sampling.penalty_present = v; return *this; }
Engine & Engine::max_tokens(int n)              { p_->max_new_tokens = n; return *this; }
Engine & Engine::tool_choice_auto()             { p_->tool_choice = COMMON_CHAT_TOOL_CHOICE_AUTO;     return *this; }
Engine & Engine::tool_choice_required()         { p_->tool_choice = COMMON_CHAT_TOOL_CHOICE_REQUIRED; return *this; }
Engine & Engine::tool_choice_none()             { p_->tool_choice = COMMON_CHAT_TOOL_CHOICE_NONE;     return *this; }
Engine & Engine::parallel_tool_calls(bool e)    { p_->parallel_tool_calls = e; return *this; }
Engine & Engine::max_tool_hops      (int  n)    { if (n > 0) p_->max_tool_hops = n; return *this; }
Engine & Engine::retry_on_incomplete(bool on)   { p_->retry_on_incomplete = on; return *this; }
Engine & Engine::max_incomplete_retries(int n)  {
    p_->max_incomplete_retries = std::max(0, n);
    return *this;
}
Engine & Engine::stop_at_ctx_pct    (int  pct)  {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    p_->stop_at_ctx_pct = pct;
    return *this;
}
bool     Engine::last_was_ctx_full() const      { return p_->last_was_ctx_full; }
Engine & Engine::verbose(bool v)                { p_->verbose = v; return *this; }

// Cooperative cancel — see engine.hpp for design notes.
Engine & Engine::request_cancel() {
    p_->cancel_requested.store(true, std::memory_order_relaxed);
    return *this;
}
Engine & Engine::clear_cancel()   {
    p_->cancel_requested.store(false, std::memory_order_relaxed);
    return *this;
}
bool     Engine::cancel_requested() const {
    return p_->cancel_requested.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// KV cache & model overrides
// ---------------------------------------------------------------------------
namespace {

// Map a ggml_type_name() string ("f16", "q8_0", …) back to the enum.
// Returns GGML_TYPE_COUNT on miss (used as the "invalid" sentinel).
ggml_type ggml_type_from_name(const std::string & s) {
    // Restrict to types that llama.cpp's KV cache actually supports — F32,
    // F16, BF16, Q8_0, Q4_0, Q4_1, Q5_0, Q5_1, IQ4_NL.  Anything else would
    // be silently ignored by ggml so we'd rather flag it explicitly.
    static const ggml_type allowed[] = {
        GGML_TYPE_F32,  GGML_TYPE_F16,  GGML_TYPE_BF16,
        GGML_TYPE_Q8_0, GGML_TYPE_Q4_0, GGML_TYPE_Q4_1,
        GGML_TYPE_Q5_0, GGML_TYPE_Q5_1, GGML_TYPE_IQ4_NL,
    };
    for (auto t : allowed) {
        if (s == ggml_type_name(t)) return t;
    }
    return GGML_TYPE_COUNT;
}

}  // namespace

Engine & Engine::cache_type_k(const std::string & name) {
    ggml_type t = ggml_type_from_name(name);
    if (t == GGML_TYPE_COUNT) {
        p_->last_error = "cache_type_k: unsupported ggml type '" + name + "'";
    } else {
        p_->params.cache_type_k = t;
    }
    return *this;
}

Engine & Engine::cache_type_v(const std::string & name) {
    ggml_type t = ggml_type_from_name(name);
    if (t == GGML_TYPE_COUNT) {
        p_->last_error = "cache_type_v: unsupported ggml type '" + name + "'";
    } else {
        p_->params.cache_type_v = t;
    }
    return *this;
}

// Map a CLI-friendly spec-type string to llama.cpp's enum. Names match
// the `--spec-type` flag llama-server accepts (e.g. "draft-mtp"); the
// underscore-cased enum variants are intentionally not exposed. Returns
// COMMON_SPECULATIVE_TYPE_COUNT on miss.
static common_speculative_type spec_type_from_name(const std::string & s) {
    if (s == "none")          return COMMON_SPECULATIVE_TYPE_NONE;
    if (s == "draft-simple")  return COMMON_SPECULATIVE_TYPE_DRAFT_SIMPLE;
    if (s == "draft-eagle3")  return COMMON_SPECULATIVE_TYPE_DRAFT_EAGLE3;
    if (s == "draft-mtp")     return COMMON_SPECULATIVE_TYPE_DRAFT_MTP;
    if (s == "ngram-simple")  return COMMON_SPECULATIVE_TYPE_NGRAM_SIMPLE;
    if (s == "ngram-map-k")   return COMMON_SPECULATIVE_TYPE_NGRAM_MAP_K;
    if (s == "ngram-map-k4v") return COMMON_SPECULATIVE_TYPE_NGRAM_MAP_K4V;
    if (s == "ngram-mod")     return COMMON_SPECULATIVE_TYPE_NGRAM_MOD;
    if (s == "ngram-cache")   return COMMON_SPECULATIVE_TYPE_NGRAM_CACHE;
    return COMMON_SPECULATIVE_TYPE_COUNT;
}

Engine & Engine::spec_type(const std::string & name) {
    common_speculative_type t = spec_type_from_name(name);
    if (t == COMMON_SPECULATIVE_TYPE_COUNT) {
        p_->last_error = "spec_type: unknown speculative type '" + name
            + "' (valid: none, draft-simple, draft-eagle3, draft-mtp, "
              "ngram-simple, ngram-map-k, ngram-map-k4v, ngram-mod, "
              "ngram-cache)";
        return *this;
    }
    // llama.cpp keeps a vector to allow chained types; we expose only
    // the single-type case from the easyai API since chained speculation
    // is an exotic configuration nobody asked for yet.
    p_->params.speculative.types = { t };
    return *this;
}

Engine & Engine::spec_draft_n_max(int n) {
    if (n < 0) n = 0;
    p_->params.speculative.draft.n_max = (int32_t) n;
    return *this;
}

Engine & Engine::spec_draft_model(const std::string & path) {
    p_->params.speculative.draft.mparams.path = path;
    return *this;
}

Engine & Engine::no_kv_offload(bool on) { p_->params.no_kv_offload = on; return *this; }
Engine & Engine::kv_unified  (bool on) { p_->params.kv_unified    = on; return *this; }

Engine & Engine::add_kv_override(const std::string & spec) {
    // Parse "key=type:value" — minimal but strict.
    auto eq = spec.find('=');
    if (eq == std::string::npos || eq == 0) {
        p_->last_error = "add_kv_override: missing '=' in '" + spec + "'";
        return *this;
    }
    std::string key  = spec.substr(0, eq);
    std::string rest = spec.substr(eq + 1);
    auto colon = rest.find(':');
    if (colon == std::string::npos) {
        p_->last_error = "add_kv_override: missing ':' in '" + spec + "' (expected key=type:value)";
        return *this;
    }
    std::string type  = rest.substr(0, colon);
    std::string value = rest.substr(colon + 1);
    if (key.size() >= sizeof(((llama_model_kv_override*)0)->key)) {
        p_->last_error = "add_kv_override: key too long (max 127 chars)";
        return *this;
    }

    llama_model_kv_override ov{};
    std::strncpy(ov.key, key.c_str(), sizeof(ov.key) - 1);
    ov.key[sizeof(ov.key) - 1] = '\0';

    if (type == "int" || type == "i") {
        ov.tag     = LLAMA_KV_OVERRIDE_TYPE_INT;
        ov.val_i64 = std::strtoll(value.c_str(), nullptr, 10);
    } else if (type == "float" || type == "f") {
        ov.tag     = LLAMA_KV_OVERRIDE_TYPE_FLOAT;
        ov.val_f64 = std::strtod(value.c_str(), nullptr);
    } else if (type == "bool" || type == "b") {
        ov.tag      = LLAMA_KV_OVERRIDE_TYPE_BOOL;
        ov.val_bool = (value == "true" || value == "1" || value == "yes");
    } else if (type == "str" || type == "s") {
        ov.tag = LLAMA_KV_OVERRIDE_TYPE_STR;
        if (value.size() >= sizeof(ov.val_str)) value.resize(sizeof(ov.val_str) - 1);
        std::strncpy(ov.val_str, value.c_str(), sizeof(ov.val_str) - 1);
        ov.val_str[sizeof(ov.val_str) - 1] = '\0';
    } else {
        p_->last_error = "add_kv_override: unknown type '" + type
                          + "' (expected int|float|bool|str)";
        return *this;
    }

    // The vector must be passed to llama with a final empty-key sentinel; we
    // append the real entry now and add the sentinel at load() time.
    p_->params.kv_overrides.push_back(ov);
    return *this;
}

// ---------------------------------------------------------------------------
// Compute / memory knobs
// ---------------------------------------------------------------------------
Engine & Engine::flash_attn(bool on) {
    p_->params.flash_attn_type = on ? LLAMA_FLASH_ATTN_TYPE_ENABLED
                                    : LLAMA_FLASH_ATTN_TYPE_DISABLED;
    return *this;
}
Engine & Engine::use_mlock(bool on)        { p_->params.use_mlock = on; return *this; }
Engine & Engine::use_mmap (bool on)        { p_->params.use_mmap  = on; return *this; }
Engine & Engine::threads_batch(int n)      { p_->params.cpuparams_batch.n_threads = n; return *this; }

Engine & Engine::numa(const std::string & strategy) {
    if      (strategy == "" || strategy == "off" || strategy == "disabled")
        p_->params.numa = GGML_NUMA_STRATEGY_DISABLED;
    else if (strategy == "distribute") p_->params.numa = GGML_NUMA_STRATEGY_DISTRIBUTE;
    else if (strategy == "isolate")    p_->params.numa = GGML_NUMA_STRATEGY_ISOLATE;
    else if (strategy == "numactl")    p_->params.numa = GGML_NUMA_STRATEGY_NUMACTL;
    else if (strategy == "mirror")     p_->params.numa = GGML_NUMA_STRATEGY_MIRROR;
    else p_->last_error = "numa: unknown strategy '" + strategy + "'";
    return *this;
}

// ---------------------------------------------------------------------------
// Reasoning toggle — propagated through chat-template rendering.
// ---------------------------------------------------------------------------
Engine & Engine::enable_thinking(bool on) {
    p_->enable_thinking = on;
    return *this;
}

Engine & Engine::chat_template_file(const std::string & path) {
    p_->chat_template_file = path;
    return *this;
}

Engine & Engine::reasoning_format(const std::string & name) {
    // common_reasoning_format_from_name throws on unknown values
    // (see llama.cpp/common/chat.cpp). We catch so a bad --reasoning-
    // format value records a clean error instead of unwinding through
    // the server's arg-apply path.
    try {
        p_->reasoning_format = common_reasoning_format_from_name(name);
    } catch (const std::exception & e) {
        p_->last_error = std::string("reasoning_format: ") + e.what();
        easyai::log::error("[easyai] Engine::reasoning_format: %s",
                           p_->last_error.c_str());
    }
    return *this;
}

Engine & Engine::add_tool(Tool t)               { p_->tools.push_back(std::move(t)); return *this; }
Engine & Engine::clear_tools()                  { p_->tools.clear(); return *this; }
Engine & Engine::on_token(TokenCallback cb)         { p_->on_token     = std::move(cb); return *this; }
Engine & Engine::on_tool(ToolCallback cb)           { p_->on_tool      = std::move(cb); return *this; }
Engine & Engine::on_hop_reset(HopResetCallback cb)  { p_->on_hop_reset = std::move(cb); return *this; }
Engine & Engine::on_incomplete_retry(IncompleteRetryCallback cb) {
    p_->on_incomplete_retry = std::move(cb); return *this;
}
Engine & Engine::on_prompt_eval(PromptEvalCallback cb) {
    p_->on_prompt_eval = std::move(cb); return *this;
}
Engine & Engine::on_prompt_progress(PromptProgressCallback cb) {
    p_->on_prompt_progress = std::move(cb); return *this;
}

bool Engine::load() {
    if (p_->loaded) return true;
    // Auto-open the raw transaction log on the first Engine::load() so
    // every server / agent / CLI built on libeasyai inherits a /tmp
    // log file natively — no per-binary wiring required.  No-op when
    // a sink was already attached (e.g. the CLI binary called
    // easyai::cli::open_log_tee earlier) or when EASYAI_NO_AUTO_LOG=1.
    easyai::log::auto_open("easyai");
    if (p_->params.model.path.empty()) {
        p_->last_error = "model path not set; call .model(\"path/to/file.gguf\") first";
        easyai::log::error("[easyai] Engine::load: %s", p_->last_error.c_str());
        return false;
    }

    // Pre-load the optional Jinja chat-template override (read once,
    // cheap) so a typo'd path fails fast — before the multi-second
    // GGUF load. Empty path keeps the model's embedded template.
    std::string tmpl_override;
    if (!p_->chat_template_file.empty()) {
        std::ifstream f(p_->chat_template_file, std::ios::binary);
        if (!f) {
            p_->last_error = "failed to open chat template file: " +
                             p_->chat_template_file;
            easyai::log::error("[easyai] Engine::load: %s", p_->last_error.c_str());
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        tmpl_override = ss.str();
        if (tmpl_override.empty()) {
            p_->last_error = "chat template file is empty: " +
                             p_->chat_template_file;
            easyai::log::error("[easyai] Engine::load: %s", p_->last_error.c_str());
            return false;
        }
    }

    // quiet logs unless verbose
    if (!p_->verbose) {
        llama_log_set([](enum ggml_log_level lvl, const char * txt, void *) {
            if (lvl >= GGML_LOG_LEVEL_ERROR) std::fprintf(stderr, "%s", txt);
        }, nullptr);
    }
    ggml_backend_load_all();

    // common_init_from_params asserts that kv_overrides ends with an
    // empty-key sentinel; honour that contract.
    if (!p_->params.kv_overrides.empty() &&
        p_->params.kv_overrides.back().key[0] != '\0') {
        llama_model_kv_override term{};
        p_->params.kv_overrides.push_back(term);
    }

    p_->init = common_init_from_params(p_->params);
    if (!p_->init || !p_->init->model() || !p_->init->context()) {
        p_->last_error = "failed to load model: " + p_->params.model.path;
        easyai::log::error("[easyai] Engine::load: %s", p_->last_error.c_str());
        return false;
    }

    p_->sampler = common_sampler_init(p_->init->model(), p_->params.sampling);
    if (!p_->sampler) {
        p_->last_error = "failed to initialize sampler";
        easyai::log::error("[easyai] Engine::load: %s", p_->last_error.c_str());
        return false;
    }

    // tmpl_override was pre-loaded above (before common_init_from_params)
    // so a typo'd --chat-template-file fails fast without paying the GGUF
    // load cost. Empty when no override path was set; in that case we
    // pass "" and common_chat_templates_init uses the embedded template.
    p_->templates = common_chat_templates_init(p_->init->model(), tmpl_override);
    if (!p_->templates) {
        p_->last_error = "model has no usable chat template";
        easyai::log::error("[easyai] Engine::load: %s", p_->last_error.c_str());
        return false;
    }

    if (!p_->system_prompt.empty()) {
        p_->history.push_back({ "system", p_->system_prompt, {}, {}, "", "", "" });
    }

    // ---- speculative decoding init ---------------------------------------
    // Only MTP is wired through the decode loop today. ngram-* and
    // draft-simple set their type in params but the engine still runs
    // autoregressive — common_init_from_params allocates the per-type
    // memory pre-emptively, so the cost is just a few extra MB.
    if (!p_->params.speculative.types.empty()) {
        const auto t0 = p_->params.speculative.types.front();
        if (t0 == COMMON_SPECULATIVE_TYPE_DRAFT_MTP) {
            // Build a SECOND llama_context against the SAME target model
            // with ctx_type=MTP. The MTP heads in the model produce draft
            // tokens here; the original context verifies them. This is
            // structurally how llama-server does it (see
            // tools/server/server-context.cpp ~line 800).
            auto cparams_mtp = common_context_params_to_llama(p_->params);
            cparams_mtp.ctx_type = LLAMA_CONTEXT_TYPE_MTP;
            cparams_mtp.n_rs_seq = 0;
            p_->ctx_dft = llama_init_from_model(p_->init->model(), cparams_mtp);
            if (!p_->ctx_dft) {
                p_->last_error =
                    "spec_type=draft-mtp: failed to create MTP draft "
                    "context — the model probably wasn't trained with "
                    "MTP heads (try a Qwen3.5/3.6, DeepSeek V3, or "
                    "MimoVL build, OR remove --spec-type to run "
                    "autoregressive)";
                easyai::log::error("[easyai] Engine::load: %s",
                                   p_->last_error.c_str());
                return false;
            }
            // common_speculative_init's MTP enable check is
            //   has_mtp = (types & MTP) && params.draft.ctx_dft != nullptr
            // — it REQUIRES both context pointers to be wired into
            // params BEFORE the init call. Without this, init logs
            // "no implementations specified" and returns null. See
            // tools/server/server-context.cpp:799-800 for the same
            // assignment in llama-server.
            p_->params.speculative.draft.ctx_tgt = p_->init->context();
            p_->params.speculative.draft.ctx_dft = p_->ctx_dft;
            p_->spec.reset(common_speculative_init(p_->params.speculative,
                                                   /*n_seq=*/1));
            if (!p_->spec) {
                p_->last_error =
                    "spec_type=draft-mtp: common_speculative_init failed";
                easyai::log::error("[easyai] Engine::load: %s",
                                   p_->last_error.c_str());
                return false;
            }
            p_->spec_mtp    = true;
            p_->spec_active = true;
            easyai::log::error(
                "[easyai] speculative MTP enabled (n_max=%d)\n",
                (int) p_->params.speculative.draft.n_max);
        }
        else if (t0 == COMMON_SPECULATIVE_TYPE_DRAFT_SIMPLE) {
            if (!p_->params.speculative.has_dft()) {
                p_->last_error =
                    "spec_type=draft-simple requires --draft-model PATH "
                    "(a GGUF file with the same vocabulary as the target "
                    "model)";
                easyai::log::error("[easyai] Engine::load: %s",
                                   p_->last_error.c_str());
                return false;
            }
            auto params_dft          = p_->params;
            params_dft.model         = p_->params.speculative.draft.mparams;
            params_dft.n_gpu_layers  =
                p_->params.speculative.draft.n_gpu_layers >= 0
                    ? p_->params.speculative.draft.n_gpu_layers
                    : p_->params.n_gpu_layers;

            auto mparams = common_model_params_to_llama(params_dft);
            p_->model_dft = llama_model_load_from_file(
                params_dft.model.path.c_str(), mparams);
            if (!p_->model_dft) {
                p_->last_error = "failed to load draft model: "
                    + params_dft.model.path;
                easyai::log::error("[easyai] Engine::load: %s",
                                   p_->last_error.c_str());
                return false;
            }
            auto cparams_dft = common_context_params_to_llama(params_dft);
            p_->ctx_dft = llama_init_from_model(p_->model_dft, cparams_dft);
            if (!p_->ctx_dft) {
                p_->last_error = "failed to create draft model context";
                easyai::log::error("[easyai] Engine::load: %s",
                                   p_->last_error.c_str());
                return false;
            }
            p_->params.speculative.draft.ctx_tgt = p_->init->context();
            p_->params.speculative.draft.ctx_dft = p_->ctx_dft;

            p_->spec.reset(common_speculative_init(
                p_->params.speculative, /*n_seq=*/1));
            if (!p_->spec) {
                p_->last_error =
                    "spec_type=draft-simple: common_speculative_init "
                    "failed — the draft model's vocabulary may be "
                    "incompatible with the target";
                easyai::log::error("[easyai] Engine::load: %s",
                                   p_->last_error.c_str());
                return false;
            }
            p_->spec_mtp    = false;
            p_->spec_active = true;
            easyai::log::error(
                "[easyai] speculative draft-simple enabled "
                "(draft=%s, n_max=%d)\n",
                params_dft.model.path.c_str(),
                (int) p_->params.speculative.draft.n_max);
        }
        else if (t0 != COMMON_SPECULATIVE_TYPE_NONE) {
            easyai::log::error(
                "[easyai] spec_type=%s requested but not yet wired "
                "through the easyai decode loop — running "
                "autoregressive\n",
                common_speculative_type_to_str(t0).c_str());
        }
    }

    // ---- backend summary --------------------------------------------------
    {
        std::ostringstream s;
        const int n_dev = ggml_backend_dev_count();
        bool any = false;
        for (int i = 0; i < n_dev; ++i) {
            ggml_backend_dev_t dev = ggml_backend_dev_get(i);
            // Anything that's not the bare CPU device counts as an offload
            // backend (GPU, ACCEL, IPU, etc.). RADV+Vulkan in particular
            // sometimes reports GPU as ACCEL, which the strict GPU filter
            // missed and made the banner say "CPU" even when offloading.
            if (ggml_backend_dev_type(dev) != GGML_BACKEND_DEVICE_TYPE_CPU) {
                if (any) s << ", ";
                s << ggml_backend_dev_name(dev);
                any = true;
            }
        }
        p_->backend_summary = any ? s.str() : "CPU";
    }

    p_->loaded = true;
    return true;
}

bool Engine::is_loaded() const { return p_->loaded; }

void Engine::reset() {
    p_->history.clear();
    if (!p_->system_prompt.empty()) {
        p_->history.push_back({ "system", p_->system_prompt, {}, {}, "", "", "" });
    }
    if (p_->ctx())     llama_memory_clear(llama_get_memory(p_->ctx()), true);
    if (p_->sampler)   common_sampler_reset(p_->sampler);
}

void Engine::clear_kv() {
    if (p_->ctx())   llama_memory_clear(llama_get_memory(p_->ctx()), true);
    if (p_->sampler) common_sampler_reset(p_->sampler);
}

void Engine::pop_last(size_t n) {
    while (n-- > 0 && !p_->history.empty()) {
        p_->history.pop_back();
    }
}

std::string Engine::generate() {
    if (!p_->loaded) { p_->last_error = "engine not loaded"; return {}; }

    // Clear sticky error from a previous call. The gate after generate()
    // ("if (!last_error.empty() && raw.empty()) return {}") would otherwise
    // misfire on this run if a prior request left last_error set
    // (e.g. "cancelled" from a dropped client) AND this run happens to
    // produce empty output (model emits EOG on the first sample after a
    // tool turn it can't follow up on). Symptom: every subsequent request
    // returns prompt_n=0, predicted_n=0 with no recovery until process
    // restart.
    p_->last_error.clear();

    auto chat_p = p_->render(/*add_generation_prompt=*/true);

    // Compute how many KV tokens are already cached so we only feed the new
    // tail. Sequence 0 is what we use; pos_max+1 is the next empty position.
    int n_past = llama_memory_seq_pos_max(llama_get_memory(p_->ctx()), 0) + 1;
    if (n_past < 0) n_past = 0;

    // For simplicity we re-render from scratch and feed whatever isn't yet
    // in cache. Tokenize the *full* rendered prompt and skip the prefix that
    // matches the existing KV. (Simple safe approach: tokenize all, decode
    // only the suffix beyond n_past.)
    const llama_vocab * vocab = llama_model_get_vocab(p_->init->model());
    std::vector<llama_token> all =
        common_tokenize(vocab, chat_p.prompt, /*add_special=*/n_past == 0,
                        /*parse_special=*/true);

    if ((int) all.size() < n_past) {
        // History was rolled back (reset/edit); restart from scratch.
        llama_memory_clear(llama_get_memory(p_->ctx()), true);
        n_past = 0;
    }
    std::vector<llama_token> tail(all.begin() + n_past, all.end());
    const int n_cached_at_start = n_past;
    const int n_to_eval         = (int) tail.size();
    if (!tail.empty()) {
        const int n_ctx = llama_n_ctx(p_->ctx());
        if (n_past + (int) tail.size() > n_ctx) {
            p_->last_error = "prompt overflows context window";
            return {};
        }
        const int n_batch = p_->params.n_batch > 0 ? p_->params.n_batch : 512;
        const auto t_eval0 = std::chrono::steady_clock::now();
        int processed = 0;
        for (size_t i = 0; i < tail.size(); i += n_batch) {
            int n = std::min<int>(n_batch, tail.size() - i);
            llama_batch b = llama_batch_get_one(tail.data() + i, n);
            if (llama_decode(p_->ctx(), b) != 0) {
                p_->last_error = "llama_decode failed feeding prompt";
                return {};
            }
            n_past    += n;
            processed += n;
            // Per-batch progress tick — mirrors llama-server's
            // `prompt_progress` SSE field. Streaming consumers (server
            // SSE → cli shimmer "thinking N%") read this to render a
            // real progress gauge during the prompt-ingestion window
            // instead of a polite-fiction spinner. Always fires at
            // least once at processed == n_to_eval (loop exit).
            if (p_->on_prompt_progress) {
                const double ms_now =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t_eval0).count();
                p_->on_prompt_progress(PromptProgressReport{
                    /*processed=*/ processed,
                    /*total=*/     n_to_eval,
                    /*cached=*/    n_cached_at_start,
                    /*ms=*/        ms_now,
                });
            }
        }
        const auto t_eval1 = std::chrono::steady_clock::now();
        // Mirror llama-server's "prompt eval time" line. Fired once
        // per generate() call right after the prompt-eval loop and
        // BEFORE the first token is sampled — streaming consumers
        // (server SSE → webui chip + libeasyai-cli) use this to
        // surface "prompt processed: N tok / X ms / T t/s" so the
        // user sees the model finished ingesting before the first
        // visible delta appears.
        if (p_->on_prompt_eval) {
            const double prompt_ms =
                std::chrono::duration<double, std::milli>(t_eval1 - t_eval0).count();
            p_->on_prompt_eval(PromptEvalReport{
                /*n_tokens=*/ n_to_eval,
                /*n_cached=*/ n_cached_at_start,
                /*prompt_ms=*/ prompt_ms,
            });
        }
    }

    return p_->generate_until_done(n_past);
}

std::string Engine::chat(const std::string & user_message) {
    if (!p_->loaded) { p_->last_error = "engine not loaded"; return {}; }

    p_->history.push_back({ "user", user_message, {}, {}, "", "", "" });
    return chat_continue();
}

std::string Engine::chat_continue() {
    if (!p_->loaded) { p_->last_error = "engine not loaded"; return {}; }

    // Drop any sticky error from a prior call (see Engine::generate for
    // the full rationale — same fix point, belt-and-braces here since
    // chat_continue itself can set last_error in its between-hop cancel
    // check before generate() runs).
    p_->last_error.clear();

    const int kMaxToolHops          = p_->max_tool_hops;
    // Bumped from 2/1 → 10/10 in 2026-04-28: malformed / "announce only"
    // turns hit production agents far more often than the original budget
    // (Qwen3 fine-tunes especially) and a 10x retry ceiling absorbs the
    // long tail without losing the "no infinite spiral" guarantee.  Set in
    // the lib so every server / CLI / agent inherits the same recovery.
    // Thought-only retries share the operator-tunable cap with the
    // announce-only path below: a single `--max-incomplete-retries N`
    // (or [ENGINE] max_incomplete_retries) knob covers both failure
    // modes from the operator's POV — "the model isn't producing a
    // useful answer, please retry harder". Counters stay separate so
    // each pathology gets its own budget; the cap is shared.
    const int kMaxThoughtRetries = std::max(0, p_->max_incomplete_retries);
    // Operator-tunable via Engine::max_incomplete_retries (server flag
    // --max-incomplete-retries / INI [ENGINE] max_incomplete_retries).
    // Default 10; clamp to ≥0 since negative values would never enter
    // the retry loop. Snapshotted into a const here so per-hop reads
    // don't pay the indirection cost.
    const int kMaxIncompleteRetries = std::max(0, p_->max_incomplete_retries);
    constexpr size_t kAnnounceFloor      = 80;  // bytes — sub-tweet replies
    int thought_retries    = 0;
    int incomplete_retries = 0;
    std::string final_text;
    p_->last_was_ctx_full = false;

    for (int hop = 0; hop < kMaxToolHops; ++hop) {
        // Bail out between agentic hops if the caller cancelled. The
        // per-token check inside generate() catches mid-decode aborts;
        // this catches aborts that arrive AFTER a hop returns (e.g.
        // during tool dispatch) and BEFORE the next hop starts.
        if (p_->cancel_requested.load(std::memory_order_relaxed)) {
            // Always log — operator wants cancellations visible in
            // journalctl regardless of --verbose. Verbose is for
            // per-token / per-hop diagnostic noise, not actionable
            // events.
            std::fprintf(stderr,
                "[easyai] chat_continue cancelled at hop %d\n", hop);
            p_->last_error = "cancelled";
            break;
        }
        auto chat_p = p_->render(/*add_generation_prompt=*/true);
        std::string raw = generate();
        if (!p_->last_error.empty() && raw.empty()) return {};

        common_chat_msg msg = p_->parse_assistant(raw, chat_p);
        msg.role = "assistant";

        if (p_->verbose) {
            std::fprintf(stderr,
                "[easyai] hop %d: raw=%zu content=%zu reasoning=%zu tool_calls=%zu\n",
                hop, raw.size(), msg.content.size(),
                msg.reasoning_content.size(), msg.tool_calls.size());
            if (!raw.empty()) {
                const size_t tail = std::min<size_t>(140, raw.size());
                std::fprintf(stderr, "[easyai] hop %d raw tail: %.*s\n",
                    hop, (int) tail, raw.c_str() + raw.size() - tail);
            }
        }

        // Detect "thought-only" turn — model emitted reasoning but produced
        // neither content nor tool_calls.  Some Qwen3 fine-tunes (the
        // user's eng_v5 in particular) terminate after </think> when they
        // intended to call a tool but failed to emit the tool_call header.
        // Discard the empty turn, clear KV, and retry — sampling is
        // stochastic so the second pass usually produces a real answer.
        //
        // Whitespace-only content counts as empty: 1-bit quants (Bonsai-
        // 8B-Q1_0 hits this often) frequently emit a stray `\n` or two
        // after </think> before EOS. Without this trim the .empty() check
        // misses, no retry fires, and the user sees what looks like an
        // empty assistant bubble even though the engine had budget left.
        auto is_blank = [](const std::string & s) {
            for (char c : s) {
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
            }
            return true;
        };
        const bool thought_only =
            msg.tool_calls.empty()
            && (msg.content.empty() || is_blank(msg.content))
            && !msg.reasoning_content.empty();

        if (thought_only && thought_retries < kMaxThoughtRetries
                         && hop + 1 < kMaxToolHops) {
            ++thought_retries;
            // Always log — actionable warning. Operators tracking
            // "why is the model giving empty replies?" need this in
            // journalctl regardless of --verbose.
            std::fprintf(stderr,
                "[easyai] hop %d: thought-only turn — clearing KV and retrying (%d/%d)\n",
                hop, thought_retries, kMaxThoughtRetries);
            // Mark the bad turn in the raw transaction log so an
            // operator can grep for `PROBLEMATIC` and find every
            // thought-only retry plus the raw tail that triggered it.
            const size_t tail = std::min<size_t>(400, raw.size());
            easyai::log::mark_problem(
                "Engine::chat_continue thought-only turn (retry %d/%d)\n"
                "hop=%d raw_bytes=%zu reasoning_bytes=%zu content_bytes=%zu tool_calls=0\n"
                "raw tail (last %zu bytes):\n%.*s",
                thought_retries, kMaxThoughtRetries,
                hop, raw.size(), msg.reasoning_content.size(), msg.content.size(),
                tail, (int) tail, raw.c_str() + raw.size() - tail);
            // Surface to streaming consumers (server SSE → webui
            // Thinking panel) so the user sees the retries happen
            // live instead of staring at silence. Reason string is
            // distinct from the announce-only path so the operator
            // can tell which failure mode we're recovering from.
            if (p_->on_incomplete_retry) {
                p_->on_incomplete_retry(
                    thought_retries, kMaxThoughtRetries,
                    "model produced reasoning but no visible reply (thought-only)");
            }
            llama_memory_clear(llama_get_memory(p_->ctx()), true);
            if (p_->on_hop_reset) p_->on_hop_reset();
            continue;
        }

        // If we exhausted retries on a thought-only turn, fall back to
        // promoting reasoning_content into content so the user at least
        // sees the model's thoughts instead of an empty bubble. Same
        // whitespace-tolerant check as thought_only above so a model
        // that emits "\n" post-</think> still hits the promotion path.
        if (msg.tool_calls.empty()
                && (msg.content.empty() || is_blank(msg.content))
                && !msg.reasoning_content.empty()) {
            msg.content = msg.reasoning_content;
            // Always log — actionable: the user is about to see
            // reasoning-text in their answer bubble instead of a
            // proper reply. Operator may want to bump
            // --max-incomplete-retries or rephrase the system prompt.
            std::fprintf(stderr,
                "[easyai] hop %d: thought-retry budget exhausted — promoting reasoning to content\n", hop);
            easyai::log::mark_problem(
                "Engine::chat_continue thought-only retry budget exhausted\n"
                "hop=%d budget=%d — promoting reasoning_content (%zu bytes) into content\n",
                hop, kMaxThoughtRetries, msg.reasoning_content.size());
            // Negative-attempt give-up signal — same convention as the
            // announce-only path's exhaust branch. The webui renders
            // this with a distinct cue ("⚠ ...") so the user knows
            // we're falling back to thinking-as-answer rather than
            // doing yet another retry.
            if (p_->on_incomplete_retry) {
                p_->on_incomplete_retry(
                    -1, kMaxThoughtRetries,
                    "thought-retry budget exhausted — showing reasoning as answer");
            }
            // Also push the synthesized text through on_token so the
            // streaming HTTP layer (which builds its SSE diffs from the
            // token stream, not from history) emits a content delta.
            // Without this, the engine ends up with content in history
            // but the client only ever saw reasoning_content deltas and
            // shows an empty bubble.  We feed it as a single chunk —
            // the partial parser appends it after any prior <think>…
            // </think> block so it parses cleanly as content.
            if (p_->on_token && !msg.content.empty()) {
                p_->on_token(msg.content);
            }
        }

        // thought_retries resets every iteration that survived past the
        // thought-only branch (either model produced content/tool_calls,
        // or the budget was exhausted and we promoted reasoning). The
        // announce-only counter (incomplete_retries) is NOT reset here —
        // see the comment at the tool-dispatch site below. The previous
        // unconditional reset of incomplete_retries at this spot is the
        // bug that pinned the counter at 1 across all retries: each
        // announce-retry would `continue`, fall back here, and zero the
        // counter before the next iteration's announce-check could
        // observe it — so kMaxIncompleteRetries was never reached and
        // the loop kept retrying past any reasonable budget.
        thought_retries = 0;
        p_->history.push_back(msg);

        // Hard ceiling on context fill — once the KV cache hits the
        // configured pct of n_ctx, the next hop will either truncate
        // from the head or just OOM the decode.  Stop here, surface
        // the latest assistant content, set a clear last_error.
        // Threshold 0 disables; default 100 = pinned at the wall.
        if (p_->stop_at_ctx_pct > 0 && p_->ctx()) {
            const int n_ctx_total = llama_n_ctx(p_->ctx());
            int n_ctx_used = llama_memory_seq_pos_max(
                                 llama_get_memory(p_->ctx()), 0) + 1;
            if (n_ctx_used < 0) n_ctx_used = 0;
            if (n_ctx_total > 0
                    && (long long) n_ctx_used * 100
                           >= (long long) p_->stop_at_ctx_pct * n_ctx_total) {
                p_->last_was_ctx_full = true;
                p_->last_error = "context full ("
                              + std::to_string(n_ctx_used) + "/"
                              + std::to_string(n_ctx_total) + " tokens — "
                              + std::to_string(p_->stop_at_ctx_pct)
                              + "%) — stopping agentic loop. "
                                "Start a new chat to free the context window.";
                easyai::log::mark_problem(
                    "Engine::chat_continue ctx full hop=%d ctx_used=%d n_ctx=%d "
                    "threshold=%d%%",
                    hop, n_ctx_used, n_ctx_total, p_->stop_at_ctx_pct);
                final_text = msg.content;
                return final_text;
            }
        }

        if (msg.tool_calls.empty()) {
            final_text = msg.content;

            // Auto-retry-with-nudge on "incomplete" turn — model finished
            // without a tool_call AND emitted ONLY a tool-announce
            // narration ("Let me search…", "I'll look that up…") that
            // promises action but never delivers.  We pop the bad
            // assistant turn, push a corrective synthetic user message,
            // fire on_hop_reset so streaming consumers can drop their
            // incremental-parse state, and continue the loop.  Mirrors
            // libeasyai-cli's Client::retry_on_incomplete so every
            // consumer of the lib gets the same recovery for free.
            //
            // The signal is *announce intent*, not raw length — a short
            // greeting reply ("Hi! How can I help?") is a perfectly valid
            // turn and should NOT trip the retry.  We require:
            //   - tools are configured (otherwise nothing to call)
            //   - reply is short (otherwise it's not an announce)
            //   - reply EITHER is empty OR matches an announce phrase
            //     (see looks_like_announce_phrase at file top)
            if (p_->retry_on_incomplete
                    && incomplete_retries < kMaxIncompleteRetries
                    && hop + 1 < kMaxToolHops
                    && !p_->tools.empty()
                    && final_text.size() < kAnnounceFloor
                    && looks_like_announce_phrase(final_text)) {
                ++incomplete_retries;
                // Always log — actionable. Each retry is one nudge
                // round-trip; operator wants to know if a turn went
                // through 10 retries before bailing.
                std::fprintf(stderr,
                    "[easyai] hop %d: incomplete turn (%zu B content, no tool_call) "
                    "— discarding, nudging, retrying (%d/%d)\n",
                    hop, final_text.size(),
                    incomplete_retries, kMaxIncompleteRetries);
                easyai::log::mark_problem(
                    "Engine::chat_continue announce-without-action (retry %d/%d)\n"
                    "hop=%d content_bytes=%zu reasoning_bytes=%zu tool_calls=0\n"
                    "content: %.*s",
                    incomplete_retries, kMaxIncompleteRetries,
                    hop, final_text.size(), msg.reasoning_content.size(),
                    (int) std::min<size_t>(400, final_text.size()),
                    final_text.c_str());
                // Surface the retry to streaming consumers (server SSE
                // → webui Thinking panel) so the user sees what the
                // engine is doing instead of staring at silence.
                // Snippet of what the model just emitted, escaped down
                // to a single line so the panel stays scannable.
                if (p_->on_incomplete_retry) {
                    std::string snippet = final_text;
                    if (snippet.size() > 100) {
                        snippet.resize(100);
                        snippet += "…";
                    }
                    for (auto & c : snippet) if (c == '\n' || c == '\r') c = ' ';
                    p_->on_incomplete_retry(
                        incomplete_retries,
                        kMaxIncompleteRetries,
                        snippet.empty()
                            ? std::string("model emitted no visible reply")
                            : ("model said: \"" + snippet + "\" (no tool_call)"));
                }
                if (!p_->history.empty()) p_->history.pop_back();
                p_->history.push_back({
                    "user",
                    "Your previous reply only announced an action without "
                    "emitting any tool_call. Do NOT say 'let me…' / 'I'll…' "
                    "unless the tool_call follows in the SAME turn. Either "
                    "call the next tool you actually need, or give the user "
                    "the final answer.",
                    {}, {}, "", "", ""
                });
                llama_memory_clear(llama_get_memory(p_->ctx()), true);
                if (p_->on_hop_reset) p_->on_hop_reset();
                final_text.clear();
                continue;
            }
            // Retry budget exhausted — model still announce-only after
            // kMaxIncompleteRetries nudges. Tell streaming consumers
            // explicitly so the webui can render a "gave up" note in
            // the Thinking panel before the empty assistant bubble
            // confuses the user. Distinct from the per-retry signal
            // (negative attempt) so the consumer can style it as a
            // give-up, not yet-another-retry.
            if (p_->retry_on_incomplete
                    && incomplete_retries >= kMaxIncompleteRetries
                    && !p_->tools.empty()
                    && final_text.size() < kAnnounceFloor
                    && looks_like_announce_phrase(final_text)
                    && p_->on_incomplete_retry) {
                p_->on_incomplete_retry(
                    -1, kMaxIncompleteRetries,
                    "gave up after " + std::to_string(kMaxIncompleteRetries)
                    + " retries — try rephrasing more specifically");
            }

            // Highlight empty-content turns at hop end — usually means
            // the model thought, decided not to call a tool, and then
            // emitted EOS without any visible reply.  The streaming
            // layer's last-resort fallback will paint the bubble with
            // the engine's promoted reasoning if it ran (logged
            // upstream), but if even that came up empty, the user's
            // bubble will be blank — surface it loudly so the operator
            // can correlate against journalctl.
            if (final_text.empty()) {
                // Always log — empty final content is the user-
                // visible failure (blank assistant bubble). Operator
                // looking at "why no reply?" needs this in journalctl
                // regardless of --verbose.
                std::fprintf(stderr,
                    "[easyai] hop %d: WARN final content is EMPTY after %d hop(s); "
                    "reasoning=%zu tool_calls=%zu — model gave up without an "
                    "answer.  Common causes: tool error chain (rate limits, "
                    "network); over-prescriptive system prompt; model "
                    "exhausted on a niche question.\n",
                    hop, hop + 1,
                    msg.reasoning_content.size(),
                    msg.tool_calls.size());
                easyai::log::mark_problem(
                    "Engine::chat_continue empty final content\n"
                    "hop=%d reasoning_bytes=%zu tool_calls=%zu",
                    hop,
                    msg.reasoning_content.size(),
                    msg.tool_calls.size());
            }
            break;
        }

        // Tool-call success — reset the announce-retry counter so a
        // later announce-only turn within the SAME chat_continue call
        // gets a fresh budget. The semantics the user wants for this
        // counter: per-turn (per chat_continue), errors accumulate,
        // success (tool dispatch) resets.
        incomplete_retries = 0;

        // Run each tool call; append a tool message for each result.
        for (const auto & tc : msg.tool_calls) {
            const Tool * tool = p_->find_tool(tc.name);
            ToolResult result;
            ToolCall   call{ tc.name, tc.arguments, tc.id };

            std::fprintf(stderr, "[easyai] hop %d: tool_call '%s' (id=%s) args=%.*s\n",
                hop, tc.name.c_str(),
                tc.id.empty() ? "(none)" : tc.id.c_str(),
                (int) std::min<size_t>(400, tc.arguments.size()),
                tc.arguments.c_str());

            const auto t_tool_begin = std::chrono::steady_clock::now();
            if (!tool) {
                result = ToolResult::error("unknown tool: " + tc.name);
                easyai::log::error(
                    "[easyai] Engine: model called unknown tool '%s' (id=%s) "
                    "args=%.*s",
                    tc.name.c_str(),
                    tc.id.empty() ? "(none)" : tc.id.c_str(),
                    (int) std::min<size_t>(400, tc.arguments.size()),
                    tc.arguments.c_str());
            } else {
                try {
                    result = tool->handler(call);
                } catch (const std::exception & e) {
                    result = ToolResult::error(std::string("tool threw: ") + e.what());
                    easyai::log::error(
                        "[easyai] Engine: tool '%s' (id=%s) threw: %s",
                        tc.name.c_str(),
                        tc.id.empty() ? "(none)" : tc.id.c_str(), e.what());
                } catch (...) {
                    result = ToolResult::error("tool threw unknown exception");
                    easyai::log::error(
                        "[easyai] Engine: tool '%s' (id=%s) threw unknown exception",
                        tc.name.c_str(),
                        tc.id.empty() ? "(none)" : tc.id.c_str());
                }
            }
            result.duration_ms = (int) std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_tool_begin).count();

            if (p_->on_tool) p_->on_tool(call, result);

            common_chat_msg tool_msg{};
            tool_msg.role         = "tool";
            tool_msg.content      = result.content;
            tool_msg.tool_name    = tc.name;
            tool_msg.tool_call_id = tc.id;
            p_->history.push_back(std::move(tool_msg));

            if (!p_->parallel_tool_calls) break;  // serial dispatch
        }
        // Loop again to let the model digest the tool output.
    }

    return final_text;
}

std::string Engine::last_error()    const { return p_->last_error; }
int         Engine::turns()         const { return (int) p_->history.size(); }
const std::vector<Tool> & Engine::tools() const { return p_->tools; }
std::string Engine::backend_summary() const { return p_->backend_summary; }
int         Engine::n_ctx()         const { return p_->ctx() ? llama_n_ctx(p_->ctx()) : p_->params.n_ctx; }
std::string Engine::model_path()    const { return p_->params.model.path; }

::common_chat_params Engine::chat_params_for_current_state(bool add_generation_prompt) const {
    if (!p_->loaded || !p_->templates) return {};
    return p_->render(add_generation_prompt);
}

Engine::PerfData Engine::perf_data() const {
    PerfData out;
    if (!p_->loaded || !p_->ctx()) return out;
    auto d = llama_perf_context(p_->ctx());
    out.n_prompt_tokens    = d.n_p_eval;
    out.n_predicted_tokens = d.n_eval;
    out.prompt_ms          = d.t_p_eval_ms;
    out.predicted_ms       = d.t_eval_ms;
    out.n_ctx_used = llama_memory_seq_pos_max(llama_get_memory(p_->ctx()), 0) + 1;
    if (out.n_ctx_used < 0) out.n_ctx_used = 0;
    return out;
}

void Engine::perf_reset() {
    if (p_->loaded && p_->ctx()) llama_perf_context_reset(p_->ctx());
}

// ---------------------------------------------------------------------------
// set_sampling — rebuild the underlying common_sampler with new values.
// We free the old one (so its KV-state and grammar arrays are reclaimed) and
// rebuild from scratch. -1.0f / -1 means "leave unchanged".
// ---------------------------------------------------------------------------
Engine & Engine::set_sampling(float temperature, float top_p, int top_k, float min_p) {
    if (temperature >= 0.0f) p_->params.sampling.temp           = temperature;
    if (top_p       >= 0.0f) p_->params.sampling.top_p          = top_p;
    if (top_k       >= 0)    p_->params.sampling.top_k          = top_k;
    if (min_p       >= 0.0f) p_->params.sampling.min_p          = min_p;

    if (p_->loaded) {
        if (p_->sampler) {
            common_sampler_free(p_->sampler);
            p_->sampler = nullptr;
        }
        p_->sampler = common_sampler_init(p_->init->model(), p_->params.sampling);
        if (!p_->sampler) {
            p_->last_error = "set_sampling: failed to rebuild sampler";
        }
    }
    return *this;
}

// ---------------------------------------------------------------------------
// push_message — append a message of any role to history without generating.
// Used by HTTP server to replay an OpenAI request and by tool-result feeding.
// ---------------------------------------------------------------------------
Engine & Engine::push_message(std::string role,
                              std::string content,
                              std::string tool_name,
                              std::string tool_call_id) {
    common_chat_msg m{};
    m.role         = std::move(role);
    m.content      = std::move(content);
    m.tool_name    = std::move(tool_name);
    m.tool_call_id = std::move(tool_call_id);
    p_->history.push_back(std::move(m));
    return *this;
}

void Engine::clear_history() {
    p_->history.clear();
    if (p_->ctx())   llama_memory_clear(llama_get_memory(p_->ctx()), true);
    if (p_->sampler) common_sampler_reset(p_->sampler);
}

void Engine::replace_history(const std::vector<std::pair<std::string, std::string>> & messages) {
    clear_history();
    bool has_system = false;
    for (const auto & m : messages) if (m.first == "system") { has_system = true; break; }
    if (!has_system && !p_->system_prompt.empty()) {
        p_->history.push_back({ "system", p_->system_prompt, {}, {}, "", "", "" });
    }
    for (const auto & m : messages) {
        common_chat_msg msg{};
        msg.role    = m.first;
        msg.content = m.second;
        p_->history.push_back(std::move(msg));
    }
}

// Full-fidelity overload — preserves tool_calls, tool_call_id, name,
// reasoning_content so the chat template can render the proper
// <tool_call> markup.  This is what llama-server feeds into the
// template via common_chat_templates_inputs::messages, and matches
// the parse done by common_chat_msgs_parse_oaicompat.
void Engine::replace_history(const std::vector<HistoryMessage> & messages) {
    clear_history();
    bool has_system = false;
    for (const auto & m : messages) if (m.role == "system") { has_system = true; break; }
    if (!has_system && !p_->system_prompt.empty()) {
        p_->history.push_back({ "system", p_->system_prompt, {}, {}, "", "", "" });
    }
    for (const auto & m : messages) {
        common_chat_msg msg{};
        msg.role              = m.role;
        msg.content           = m.content;
        msg.reasoning_content = m.reasoning_content;
        msg.tool_name         = m.tool_name;
        msg.tool_call_id      = m.tool_call_id;
        msg.tool_calls.reserve(m.tool_calls.size());
        for (const auto & tc : m.tool_calls) {
            common_chat_tool_call out{};
            out.name      = tc.name;
            // Per OpenAI spec, `arguments` is a JSON string literal.
            // Empty arguments → "{}" so the chat template's
            // function|tojson invocation doesn't print bare empty.
            out.arguments = tc.arguments_json.empty()
                                ? std::string("{}")
                                : tc.arguments_json;
            out.id        = tc.id;
            msg.tool_calls.push_back(std::move(out));
        }
        p_->history.push_back(std::move(msg));
    }
}

// ---------------------------------------------------------------------------
// generate_one — single-pass generation. Renders prompt, decodes, parses, and
// returns the structured message. Used by HTTP layer when client provides its
// own tools (we do NOT dispatch them; we forward them back).
// ---------------------------------------------------------------------------
Engine::GeneratedTurn Engine::generate_one() {
    GeneratedTurn out{};
    if (!p_->loaded) { out.finish_reason = "error"; p_->last_error = "engine not loaded"; return out; }

    // Same recovery-from-sticky-error fix as Engine::generate /
    // Engine::chat_continue. Without this, a leftover last_error makes
    // the gate at line ~2421 short-circuit any future request whose
    // generate() returns empty.
    p_->last_error.clear();

    auto chat_p = p_->render(/*add_generation_prompt=*/true);
    std::string raw = generate();

    // Zero-work retry loop. The model emitted EOG on the first sample
    // OR the chat template produced a prompt the model treats as
    // already terminated (usual cause: a malformed assistant turn in
    // history — tool_call args with stray <|...|> tokens scraped by
    // the recovery parser — made the rendered tail look complete to
    // the model). Push a synthetic "please respond" nudge ONCE so
    // the prompt's tail changes, then keep re-issuing up to the
    // configured budget so the sampler's advancing RNG state can
    // produce different output even on the same rendered prompt.
    // Mirrors the announce-only retry budget in chat_continue —
    // bound by max_incomplete_retries (default 10). Only after the
    // whole budget is exhausted do we surface finish_reason="error";
    // a one-attempt give-up was the wrong shape (the server's
    // zero-work signal would fire after the first attempt, defeating
    // the point of having a retry budget at all).
    const int kMaxZeroWorkRetries = std::max(0, p_->max_incomplete_retries);
    int zero_work_retries = 0;
    bool nudge_pushed = false;
    while (raw.empty() && p_->last_error.empty()
                       && zero_work_retries < kMaxZeroWorkRetries) {
        ++zero_work_retries;
        if (!nudge_pushed) {
            p_->history.push_back({
                "user",
                "Your previous turn produced no output. Please respond "
                "to the previous message — either call the next tool you "
                "actually need or give the user the final answer. Do not "
                "leave the turn empty.",
                {}, {}, "", "", ""
            });
            nudge_pushed = true;
        }
        // Surface each retry to streaming consumers so the webui /
        // libeasyai-cli can render the attempts live in the Thinking
        // panel instead of staring at silence.
        if (p_->on_incomplete_retry) {
            p_->on_incomplete_retry(
                zero_work_retries, kMaxZeroWorkRetries,
                "engine produced no output — nudging and retrying");
        }
        std::fprintf(stderr,
            "[easyai] generate_one zero-work retry %d/%d\n",
            zero_work_retries, kMaxZeroWorkRetries);
        chat_p = p_->render(/*add_generation_prompt=*/true);
        raw = generate();
    }

    // Budget exhausted on zero-work — give-up signal so consumers can
    // render a distinct cue.
    if (raw.empty() && p_->last_error.empty()
                    && zero_work_retries >= kMaxZeroWorkRetries
                    && p_->on_incomplete_retry) {
        p_->on_incomplete_retry(
            -1, kMaxZeroWorkRetries,
            "engine produced no output after "
            + std::to_string(kMaxZeroWorkRetries) + " retries");
    }

    if (!p_->last_error.empty() && raw.empty()) {
        out.finish_reason = "error";
        return out;
    }
    if (raw.empty()) {
        out.finish_reason = "error";
        return out;
    }

    common_chat_msg msg = p_->parse_assistant(raw, chat_p);
    msg.role = "assistant";

    if (p_->verbose) {
        // Single-turn equivalent of chat_continue's per-hop dump.  Lets
        // an operator running --verbose see EXACTLY what the model
        // emitted in client_tools mode (where a thought-only turn used
        // to silently produce an empty bubble).  Tail is bounded so a
        // long generation doesn't flood the journal.
        std::fprintf(stderr,
            "[easyai] generate_one: raw=%zu content=%zu reasoning=%zu tool_calls=%zu\n",
            raw.size(), msg.content.size(),
            msg.reasoning_content.size(), msg.tool_calls.size());
        if (!raw.empty()) {
            const size_t tail = std::min<size_t>(220, raw.size());
            std::fprintf(stderr, "[easyai] generate_one raw tail: %.*s\n",
                         (int) tail, raw.c_str() + raw.size() - tail);
        }
    }

    p_->history.push_back(msg);

    out.content   = msg.content;
    out.reasoning = msg.reasoning_content;
    out.tool_calls.reserve(msg.tool_calls.size());
    out.tool_call_ids.reserve(msg.tool_calls.size());
    for (const auto & tc : msg.tool_calls) {
        out.tool_calls.emplace_back(tc.name, tc.arguments);
        out.tool_call_ids.push_back(tc.id);
    }
    out.finish_reason = msg.tool_calls.empty() ? "stop" : "tool_calls";
    return out;
}

}  // namespace easyai
