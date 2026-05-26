// easyai/tool.hpp — dead-simple tool definition for agents.
#pragma once

#include <functional>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace easyai {

// ToolResult is what a handler returns. content goes back to the model;
// is_error=true tags the message so the model knows the call failed.
// duration_ms is filled in by the Engine after the handler returns —
// it's purely a UI/telemetry field, handlers don't set it.
struct ToolResult {
    std::string content;
    bool        is_error    = false;
    int         duration_ms = 0;   // wall-clock dispatch time, set by Engine
    std::string display;           // ANSI-colored terminal output (optional)

    static ToolResult ok(std::string s)    { return { std::move(s), false, 0, {} }; }
    static ToolResult error(std::string s) { return { std::move(s), true,  0, {} }; }
    static ToolResult ok_display(std::string s, std::string d) {
        return { std::move(s), false, 0, std::move(d) };
    }
};

// ToolCall is what easyai hands the handler. `arguments_json` is the raw JSON
// string emitted by the model; for almost every tool, the helpers below let
// you skip touching it and just declare typed parameters.
struct ToolCall {
    std::string name;
    std::string arguments_json;
    std::string id;
};

using ToolHandler = std::function<ToolResult(const ToolCall &)>;

// A Tool is name + description + JSON-schema parameters + handler.
//
// Two ways to build one:
//   1. Tool::make("name", "desc", R"({"type":"object",...})", handler);
//   2. Tool::builder("name").describe("desc")
//                           .param("query", "string", "what to search", true)
//                           .handle([](const ToolCall & c){ ... }).build();
// Tools carry TWO descriptions:
//   * short_description — one-line trigger (≤ ~80 chars). This is what
//     ships in the per-turn `<tools>` block sent to the model. Cheap on
//     tokens; gives the model just enough to know WHEN to reach for the
//     tool. Always omits schema detail — the JSON schema already
//     advertises the parameters.
//   * description — the full multi-line manual: rules, examples, edge
//     cases. NEVER sent in the per-turn tools block. Returned by the
//     `tool_lookup` tool on demand, and exposed unchanged on the MCP
//     `tools/list` endpoint (MCP clients drive their own context budget,
//     so we hand them the full text).
//
// If a tool sets only `description` (legacy), the wire serialiser falls
// back to the first sentence/line of the long form so existing callers
// keep working until they migrate.
struct Tool {
    std::string name;
    std::string description;
    std::string short_description;
    std::string parameters_json;   // JSON schema (object)
    ToolHandler handler;

    // Resolve the trigger string that should be sent in the per-turn
    // tools block. Falls back to the first non-empty line of `description`
    // if `short_description` is empty, capped at ~120 chars so we don't
    // accidentally ship the whole manual.
    std::string wire_description() const;

    static Tool make(std::string n, std::string d, std::string p, ToolHandler h) {
        return Tool{ std::move(n), std::move(d), std::string(), std::move(p), std::move(h) };
    }

    class Builder {
       public:
        explicit Builder(std::string name) : name_(std::move(name)) {}

        Builder & describe      (std::string d) { desc_ = std::move(d); return *this; }
        Builder & short_describe(std::string d) { short_ = std::move(d); return *this; }

        // Add a typed parameter to the JSON schema.
        // type: "string" | "integer" | "number" | "boolean" | "array" | "object"
        Builder & param(std::string name,
                        std::string type,
                        std::string description = "",
                        bool        required    = false) {
            params_.push_back({ std::move(name), std::move(type),
                                std::move(description), required });
            return *this;
        }

        Builder & handle(ToolHandler h) { handler_ = std::move(h); return *this; }

        Tool build() const;  // see tool.cpp

       private:
        struct P { std::string name, type, description; bool required; };
        std::string    name_;
        std::string    desc_;
        std::string    short_;
        std::vector<P> params_;
        ToolHandler    handler_;
    };

    static Builder builder(std::string name) { return Builder(std::move(name)); }
};

// Back-compat tool-name aliases.
//
//   * `rag` → `memory` (renamed 2026-04-?)
//   * `python3` → `evaluate` (renamed 2026-05-26 — the underlying
//     runtime is still python3 but the model-facing dispatch name
//     changed to defeat the "python = write files" training prior;
//     legacy chat sessions, manifest-reservation lists, and any
//     external code that types `python3` route through here).
//
// Single source of truth — every tool-name lookup runs through this.
inline std::string canonical_tool_name(const std::string & name) {
    if (name == "rag")     return "memory";
    if (name == "python3") return "evaluate";
    return name;
}

// Tiny JSON-arg helpers so handlers don't need a JSON dep.
// They scan the raw arguments_json for top-level "key":value pairs.
//
// Two flavours per type:
//   get_<T>(json, key, out)               -> bool ; legacy, fills `out` only on success
//   get_<T>_or(json, key, default)        -> T    ; returns default if missing/wrong type
//
// Use `_or` whenever you have a sensible default.  It usually cuts a
// 4-line "declare, get, check, fallback" pattern down to one line.
namespace args {
    bool get_string(const std::string & json, const std::string & key, std::string & out);
    bool get_int   (const std::string & json, const std::string & key, long long  & out);
    bool get_double(const std::string & json, const std::string & key, double     & out);
    bool get_bool  (const std::string & json, const std::string & key, bool       & out);

    std::string get_string_or(const std::string & json, const std::string & key,
                              std::string default_value);
    long long   get_int_or   (const std::string & json, const std::string & key,
                              long long default_value);
    double      get_double_or(const std::string & json, const std::string & key,
                              double default_value);
    bool        get_bool_or  (const std::string & json, const std::string & key,
                              bool default_value);

    // True when `key` appears at top level with any value (string, number,
    // bool, null, array, object).  Useful for "did the model fill this in?".
    bool has(const std::string & json, const std::string & key);

    // Extract elements of a JSON array at `key` into `out`.
    // Each element is returned as a raw JSON substring (e.g. the full
    // object text for [{...}, {...}]).  Handles nested brackets/strings.
    bool get_array(const std::string & json, const std::string & key,
                   std::vector<std::string> & out);
}

}  // namespace easyai
