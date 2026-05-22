// client.cpp — easyai::Client (libeasyai-cli).
//
// HTTP/SSE transport over cpp-httplib + nlohmann::json + a small
// SSE event splitter; agentic multi-hop loop that mirrors what
// Engine::chat_continue() does locally.  Tools registered via
// add_tool() are dispatched in-process whenever the remote model
// emits a tool_calls finish_reason; the result is appended to the
// conversation as a `role: tool` message and the next turn is
// requested automatically.
//
// Wire-format support:
//   delta.content              — visible reply text
//   delta.reasoning_content    — easyai-server / llama-server / DeepSeek
//   delta.tool_calls           — OpenAI tool-call deltas (incremental
//                                 arguments concatenated by index)
//
// Custom easyai-server SSE events (`event: easyai.tool_call`,
// `event: easyai.tool_result`) are ignored — when we sent `tools=[...]`
// in the request body the server forwards real delta.tool_calls, so
// the custom events are pure UI noise from our side.
#include "easyai/client.hpp"
#include "easyai/log.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace easyai {

namespace {

using nlohmann::json;
using ordered_json = nlohmann::ordered_json;

// ---------------------------------------------------------------------------
// URL parsing
// ---------------------------------------------------------------------------
struct ParsedUrl {
    std::string scheme;   // "http" or "https"
    std::string host;
    int         port = 80;
    std::string base_path;
    bool        ok = false;
};

ParsedUrl parse_url(const std::string & url) {
    ParsedUrl out;
    auto sp = url.find("://");
    if (sp == std::string::npos) return out;
    out.scheme = url.substr(0, sp);
    if (out.scheme != "http" && out.scheme != "https") return out;
    size_t i = sp + 3;
    out.port = (out.scheme == "https") ? 443 : 80;
    auto path_start = url.find('/', i);
    std::string authority =
        (path_start == std::string::npos) ? url.substr(i)
                                          : url.substr(i, path_start - i);
    if (path_start != std::string::npos) out.base_path = url.substr(path_start);
    auto colon = authority.find(':');
    if (colon == std::string::npos) {
        out.host = authority;
    } else {
        out.host = authority.substr(0, colon);
        try { out.port = std::stoi(authority.substr(colon + 1)); }
        catch (...) { return out; }
    }
    if (out.host.empty()) return out;
    // Strip any trailing slash from base_path so path joining is clean.
    while (!out.base_path.empty() && out.base_path.back() == '/')
        out.base_path.pop_back();
    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------
// Tool → JSON (OpenAI tool spec shape)
// ---------------------------------------------------------------------------
ordered_json tool_to_json(const Tool & t) {
    ordered_json fn;
    fn["name"]        = t.name;
    fn["description"] = t.description;
    auto empty_schema = []() {
        ordered_json e;
        e["type"]       = "object";
        e["properties"] = ordered_json::object();
        return e;
    };
    if (!t.parameters_json.empty()) {
        try { fn["parameters"] = ordered_json::parse(t.parameters_json); }
        catch (...) { fn["parameters"] = empty_schema(); }
    } else {
        fn["parameters"] = empty_schema();
    }
    ordered_json tool;
    tool["type"]     = "function";
    tool["function"] = std::move(fn);
    return tool;
}

// ---------------------------------------------------------------------------
// SSE event splitter — feed bytes, get back fully-buffered events as
// {evt_type, data} pairs.
// ---------------------------------------------------------------------------
struct SseEvent {
    std::string event;   // "message" if no `event:` line
    std::string data;
};

class SseBuffer {
public:
    // Hard ceiling on the SSE pending buffer.  In a healthy stream the
    // buffer drains completely between events; if a malformed/truncated
    // stream skips its terminators, buf_ would otherwise grow without
    // bound and OOM the client.  16 MiB is roughly 4–8x the largest
    // single tool result we ship today (32 KB bash output → 4 MB
    // web_fetch → 16 MB ceiling has plenty of headroom).
    static constexpr size_t kMaxPending = 16 * 1024 * 1024;

    bool feed(const char * bytes, size_t n) {
        if (buf_.size() + n > kMaxPending) return false;
        buf_.append(bytes, n);
        return true;
    }
    bool next(SseEvent & out) {
        // SSE event terminator is a blank line — accept either \n\n
        // or \r\n\r\n.
        size_t end = std::string::npos;
        size_t terminator_len = 0;
        size_t a = buf_.find("\n\n");
        size_t b = buf_.find("\r\n\r\n");
        if (a != std::string::npos && (b == std::string::npos || a < b)) {
            end = a; terminator_len = 2;
        } else if (b != std::string::npos) {
            end = b; terminator_len = 4;
        }
        if (end == std::string::npos) return false;

        out = SseEvent{};
        out.event = "message";
        std::string raw = buf_.substr(0, end);
        buf_.erase(0, end + terminator_len);

        std::string data;
        size_t i = 0;
        while (i < raw.size()) {
            size_t nl = raw.find('\n', i);
            std::string line = raw.substr(i, nl == std::string::npos
                                             ? raw.size() - i : nl - i);
            i = (nl == std::string::npos) ? raw.size() : nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == ':') continue;  // comment
            auto colon = line.find(':');
            std::string field = (colon == std::string::npos)
                                    ? line : line.substr(0, colon);
            std::string val = (colon == std::string::npos)
                                  ? std::string() : line.substr(colon + 1);
            if (!val.empty() && val.front() == ' ') val.erase(0, 1);
            if      (field == "event") out.event = val;
            else if (field == "data")  { if (!data.empty()) data += "\n"; data += val; }
            else                       { /* ignore id/retry/etc */ }
        }
        out.data = std::move(data);
        return true;
    }
private:
    std::string buf_;
};

// Exponential backoff for retry attempt N (0-indexed):
// 250ms, 500ms, 1s, 2s, 4s, 4s, … capped at 4s.  Bounded so a long
// retry budget can't stall a stuck call for minutes.
int retry_backoff_ms(int attempt) {
    int ms = 250 << (attempt > 4 ? 4 : attempt);
    return ms > 4000 ? 4000 : ms;
}

// Cancellable sleep used between retry attempts.  Polls the caller's
// cancel flag every 50ms instead of one long blocking sleep, so a
// Ctrl-C during a 4s backoff escalates within ~50ms instead of
// waiting out the full delay.  Returns true when the full duration
// elapsed naturally; false when cancel fired.
bool cancellable_sleep_ms(int total_ms,
                          const std::atomic<bool> & cancel_flag) {
    constexpr int kPollMs = 50;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(total_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancel_flag.load(std::memory_order_relaxed)) return false;
        auto now = std::chrono::steady_clock::now();
        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count();
        if (remaining_ms <= 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(
            remaining_ms < kPollMs ? remaining_ms : kPollMs));
    }
    return !cancel_flag.load(std::memory_order_relaxed);
}

// Decide whether a cpp-httplib failure is worth retrying.  Pre-stream
// connect/read/write failures and 5xx responses qualify; 4xx (auth /
// bad request) does not.  `received_anything` short-circuits to false:
// retrying a half-streamed turn would duplicate output.
bool is_retryable_httplib(const httplib::Result & res,
                          bool received_anything) {
    if (received_anything) return false;
    if (!res) {
        // Transport error before we got headers — connect refused,
        // dns failure, read/write timeout, peer reset.
        switch (res.error()) {
            case httplib::Error::Connection:
            case httplib::Error::BindIPAddress:
            case httplib::Error::Read:
            case httplib::Error::Write:
            case httplib::Error::ExceedRedirectCount:
            case httplib::Error::Canceled:
            case httplib::Error::SSLConnection:
            case httplib::Error::ConnectionTimeout:
                return true;
            default:
                return true;  // be generous — operator can lower retries
        }
    }
    // Got a response; only retry server-side errors.  4xx is a contract
    // failure (bad token, malformed body) and won't fix itself.
    return res->status >= 500 && res->status < 600;
}

}  // namespace

// ===========================================================================
// Pending tool call accumulator (tool_calls deltas come in pieces).
// ===========================================================================
namespace {
struct PendingToolCall {
    int         index = -1;
    std::string id;
    std::string name;
    std::string arguments;     // built up across deltas
};
}  // namespace

// ===========================================================================
// Impl
// ===========================================================================
struct Client::Impl {
    // Transport / auth.
    std::string endpoint;
    std::string api_key;
    // 86400s (24 hours) — sized for multi-hour agentic sessions where
    // the model spends long stretches in tool dispatch / reasoning
    // without producing visible bytes.  Long-running agent flows
    // (Tetris-from-scratch, codebase rewrites, multi-source research)
    // routinely run for hours; 30-minute defaults trip mid-session.
    // Set lower (e.g. 1800) for short interactive turns; the read
    // timeout only fires on TRUE silence — every SSE delta resets it.
    int         timeout_seconds = 86400;
    bool        verbose         = false;
    bool        tls_insecure    = false;   // skip peer cert verification
    std::string tls_ca_path;                // PEM bundle for custom CAs
    int         max_reasoning_chars = 0;    // 0 = unlimited; >0 aborts SSE on overflow
    // Retry budget for pre-stream HTTP failures (connect refused, read
    // timeout, 5xx before any byte streamed). Mid-stream failures are
    // never retried.  Default 5 — see Client::http_retries().
    int         http_retries        = 5;
    // Default ON: when the server flags a turn as incomplete (model
    // announced a tool but never emitted it, or stopped with a tiny
    // post-tool-call reply), we discard the bad turn, append a
    // corrective user nudge ("don't announce, execute"), and re-issue
    // ONCE.  Bounded — no spirals, no nudge stacking.  Library
    // consumers who want the raw incomplete signal call
    // retry_on_incomplete(false).
    bool        retry_on_incomplete = true;
    bool        last_was_incomplete = false; // mirror of last turn's timings.incomplete
    int         last_ctx_used       = -1;    // mirror of last turn's timings.ctx_used
    int         last_n_ctx          = -1;    // mirror of last turn's timings.n_ctx
    int         max_tool_hops       = 8;     // agentic loop safety cap; bumped by bash
    int         stop_at_ctx_pct     = 100;   // 0 disables; otherwise abort agentic
                                              // loop when ctx_used/n_ctx >= this %
    bool        last_was_ctx_full   = false; // tripped this turn's stop_at_ctx_pct
    // Per-chat budget for the announce-without-action / malformed-turn
    // recovery loop.  Bumped from 1 → 10 in 2026-04-28 so a flaky model
    // gets the same chances as the local Engine path before we surface
    // an empty bubble.  No infinite spirals — once the budget is spent
    // the bad turn is handed back to the caller.
    int         max_incomplete_retries = 10;

    // Diagnostic log file (tee).  Borrowed; caller owns fclose.
    std::FILE * log_fp = nullptr;

    // Request shape.  -1 / -1.0f / empty == "leave server default in place".
    std::string              model_id;
    std::string              system_prompt;
    float                    temperature       = -1.0f;
    float                    top_p             = -1.0f;
    int                      top_k             = -1;
    float                    min_p             = -1.0f;
    // Anti-loop safety net: thinking models sometimes lock into rephrasing
    // their own intent ("I'll write types.h / Let me write…" forever).  A
    // mild repetition penalty breaks the cycle without hurting normal
    // factual answers.  Pin 1.15 by default for every Client (the value
    // is sent in every request body; pass `repeat_penalty(1.0f)` to
    // disable, or any other value to override).
    float                    repeat_penalty    = 1.15f;
    float                    frequency_penalty = -2.0f;  // -2 = unset (real OpenAI range starts at -2)
    float                    presence_penalty  = -2.0f;
    long long                seed              = -1;
    int                      max_tokens        = -1;
    std::vector<std::string> stop_sequences;
    std::string              extra_body_raw;            // JSON object literal

    // Tools (registered locally; their handlers run in this process).
    std::vector<Tool> tools;

    // Callbacks.
    Client::TokenCallback          on_token;
    Client::TokenCallback          on_reason;
    Client::ToolCallback           on_tool;
    Client::PromptProgressCallback on_prompt_progress;
    Client::PromptEvalCallback     on_prompt_eval;

    // Conversation state.  Each entry is one OpenAI message (raw JSON
    // object) so we don't leak nlohmann::json into the public ABI.
    std::vector<std::string> history_json;

    std::string last_error;

    // Cooperative cancel — flipped from any thread (typically the cli's
    // SIGINT handler). Polled by the SSE content_receiver every chunk;
    // returning false from the receiver makes cpp-httplib abort the read
    // and close the socket. The remote server detects the dropped TCP
    // connection on its next write and cancels its own decode loop.
    std::atomic<bool> cancel_requested{false};

    // -----------------------------------------------------------------
    // Transport — single persistent httplib::Client, lazy-initialised on
    // first use. Reusing one Client across the whole agentic loop is the
    // whole point: cpp-httplib keeps the underlying TCP socket between
    // requests when set_keep_alive(true) is set, so a 50-tool-call agentic
    // session uses ONE connection instead of 50 (each prior connection
    // would otherwise pile up in TIME_WAIT for ~60 s, eventually exhausting
    // the client's ephemeral-port range on long sessions).
    //
    // Thread-safety: cpp-httplib's Client serialises operations on its
    // internal socket, so concurrent calls from cancellation handlers etc.
    // are safe.  The mutex below only guards the lazy-init path.
    std::unique_ptr<httplib::Client> http_;
    std::mutex                        http_mu_;

    httplib::Client * get_http() {
        std::lock_guard<std::mutex> lk(http_mu_);
        if (http_) return http_.get();
        ParsedUrl u = parse_url(endpoint);
        if (!u.ok) {
            last_error = "invalid endpoint URL: " + endpoint;
            return nullptr;
        }
        // Authority-only URL httplib's Client constructor wants.  It
        // dispatches internally to ClientImpl for http:// and to SSLClient
        // for https:// when CPPHTTPLIB_OPENSSL_SUPPORT is on.
        std::string auth = u.scheme + "://" + u.host + ":"
                         + std::to_string(u.port);
        auto cli = std::make_unique<httplib::Client>(auth);
        if (!cli->is_valid()) {
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
            if (u.scheme == "https") {
                last_error = "HTTPS endpoint requires OpenSSL — install "
                             "libssl-dev and rebuild libeasyai-cli";
                return nullptr;
            }
#endif
            last_error = "transport setup failed for endpoint " + endpoint;
            return nullptr;
        }
        if (u.scheme == "https") {
            if (tls_insecure) {
                cli->enable_server_certificate_verification(false);
            }
            if (!tls_ca_path.empty()) {
                cli->set_ca_cert_path(tls_ca_path.c_str());
            }
        }
        cli->set_read_timeout (timeout_seconds, 0);
        cli->set_write_timeout(timeout_seconds, 0);
        cli->set_keep_alive(true);
        http_ = std::move(cli);
        return http_.get();
    }

    httplib::Headers headers_with_auth(const std::string & accept) const {
        httplib::Headers h;
        if (!accept.empty()) h.emplace("Accept", accept);
        if (!api_key.empty()) h.emplace("Authorization", "Bearer " + api_key);
        return h;
    }

    std::string base_path() const {
        ParsedUrl u = parse_url(endpoint);
        return u.ok ? u.base_path : std::string();
    }

    // -----------------------------------------------------------------
    // Build messages array from history + system prompt.
    // -----------------------------------------------------------------
    ordered_json messages_array() const {
        ordered_json arr = ordered_json::array();
        if (!system_prompt.empty()) {
            ordered_json sys;
            sys["role"]    = "system";
            sys["content"] = system_prompt;
            arr.push_back(std::move(sys));
        }
        for (const auto & raw : history_json) {
            try {
                arr.push_back(ordered_json::parse(raw));
            } catch (...) {
                // Drop malformed entries silently — we shouldn't have
                // pushed bad JSON in the first place.
            }
        }
        return arr;
    }

    // Build the POST body for /v1/chat/completions.
    std::string build_chat_body() const {
        ordered_json body;
        if (!model_id.empty()) body["model"] = model_id;
        body["messages"] = messages_array();
        body["stream"]   = true;
        if (temperature       >= 0.0f) body["temperature"]       = temperature;
        if (top_p             >= 0.0f) body["top_p"]             = top_p;
        if (top_k             >= 0)    body["top_k"]             = top_k;
        if (min_p             >= 0.0f) body["min_p"]             = min_p;
        if (repeat_penalty    >  0.0f) body["repeat_penalty"]    = repeat_penalty;
        if (frequency_penalty > -2.0f) body["frequency_penalty"] = frequency_penalty;
        if (presence_penalty  > -2.0f) body["presence_penalty"]  = presence_penalty;
        if (seed              >= 0)    body["seed"]              = seed;
        if (max_tokens        >= 0)    body["max_tokens"]        = max_tokens;
        if (!stop_sequences.empty()) {
            ordered_json arr = ordered_json::array();
            for (const auto & s : stop_sequences) arr.push_back(s);
            body["stop"] = std::move(arr);
        }
        if (!tools.empty()) {
            ordered_json tarr = ordered_json::array();
            for (const auto & t : tools) tarr.push_back(tool_to_json(t));
            body["tools"]       = std::move(tarr);
            body["tool_choice"] = "auto";
        }
        // Merge user-supplied extras last so they can override anything above.
        if (!extra_body_raw.empty()) {
            try {
                ordered_json extras = ordered_json::parse(extra_body_raw);
                if (extras.is_object()) {
                    for (auto it = extras.begin(); it != extras.end(); ++it) {
                        body[it.key()] = it.value();
                    }
                }
            } catch (...) {
                // Silently skip — the public setter validates at set time
                // (or the body simply lacks the extras the caller wanted).
            }
        }
        return body.dump();
    }

    // -----------------------------------------------------------------
    // SSE → callbacks; populates `out_msg` with what the assistant
    // produced this turn (content + tool_calls + finish_reason).
    // -----------------------------------------------------------------
    struct AssistantTurn {
        std::string                  content;
        std::string                  reasoning;
        std::vector<PendingToolCall> tool_calls;
        std::string                  finish_reason = "stop";
        bool                         incomplete = false;  // mirrors timings.incomplete
        int                          ctx_used   = -1;     // tokens currently in KV
        int                          n_ctx      = -1;     // configured context window
    };

    bool stream_chat(AssistantTurn & out) {
        out = AssistantTurn{};
        auto * cli = get_http();
        if (!cli) return false;

        SseBuffer sse;
        std::map<int, PendingToolCall> tc_by_index;
        bool received_anything = false;
        bool reasoning_aborted = false;
        bool skip_next_reason  = false;

        auto on_chunk = [&](const char * data, size_t len) -> bool {
            // Cooperative cancel — checked first thing on every SSE
            // chunk. Returning false from a content_receiver makes
            // cpp-httplib abort the read and close the socket, which
            // the server picks up on its next sink.write() and uses to
            // cancel its decode loop. No /cancel endpoint, no protocol
            // change.
            if (cancel_requested.load(std::memory_order_relaxed)) {
                last_error = "cancelled";
                return false;
            }
            // Tee raw SSE bytes verbatim into the log file when one is
            // attached.  Lets the operator see exactly what crossed the
            // wire (including custom easyai.* events, server timings,
            // and any tool_call deltas the parser might have skipped).
            if (log_fp) {
                std::fwrite(data, 1, len, log_fp);
                std::fflush(log_fp);
            }
            if (!sse.feed(data, len)) {
                last_error = "SSE pending buffer exceeded 16 MiB — "
                             "abandoning stream (malformed server response?)";
                return false;
            }
            SseEvent ev;
            while (sse.next(ev)) {
                if (ev.data.empty() || ev.data == "[DONE]") continue;
                if (ev.event == "easyai.tool_call" ||
                    ev.event == "easyai.tool_result") continue;
                if (ev.event == "easyai.prompt_eval") {
                    if (on_prompt_eval) {
                        try {
                            auto j = ordered_json::parse(ev.data);
                            int    n  = j.value("n_tokens",          0);
                            int    nc = j.value("n_cached",          0);
                            double ms = j.value("prompt_ms",         0.0);
                            double tp = j.value("tokens_per_second", 0.0);
                            on_prompt_eval(n, nc, ms, tp);
                        } catch (...) {}
                    }
                    skip_next_reason = true;
                    continue;
                }
                // Per-batch prompt-eval progress — fire the typed
                // callback so cli's shimmer can paint a real "thinking
                // N%" gauge. The event payload mirrors llama-server's
                // `prompt_progress` shape so consumers that already
                // speak that contract work without changes.
                if (ev.event == "easyai.prompt_progress") {
                    if (on_prompt_progress) {
                        try {
                            auto j = ordered_json::parse(ev.data);
                            int processed = j.value("processed", 0);
                            int total     = j.value("total",     0);
                            int cached    = j.value("cache",     0);
                            double ms     = j.value("time_ms",   0.0);
                            on_prompt_progress(processed, total, cached, ms);
                        } catch (...) { /* best-effort */ }
                    }
                    continue;
                }
                ordered_json j;
                try { j = ordered_json::parse(ev.data); }
                catch (...) { continue; }

                if (!j.contains("choices") || !j["choices"].is_array()
                        || j["choices"].empty()) continue;
                received_anything = true;
                static const ordered_json kEmptyObj = ordered_json::object();
                auto & ch    = j["choices"][0];
                const ordered_json & delta = ch.contains("delta")
                                                 ? ch["delta"]
                                                 : kEmptyObj;

                if (delta.contains("reasoning_content")
                        && delta["reasoning_content"].is_string()) {
                    if (skip_next_reason) {
                        skip_next_reason = false;
                    } else {
                        const auto & s = delta["reasoning_content"].get_ref<const std::string &>();
                        out.reasoning += s;
                        if (on_reason) on_reason(s);
                        if (max_reasoning_chars > 0
                                && (int) out.reasoning.size() > max_reasoning_chars) {
                            reasoning_aborted = true;
                            return false;
                        }
                    }
                }
                if (delta.contains("content") && delta["content"].is_string()) {
                    const auto & s = delta["content"].get_ref<const std::string &>();
                    out.content += s;
                    if (on_token) on_token(s);
                }
                if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                    for (const auto & tcj : delta["tool_calls"]) {
                        int idx = tcj.value("index", 0);
                        auto & p = tc_by_index[idx];
                        if (p.index < 0) p.index = idx;
                        if (tcj.contains("id") && tcj["id"].is_string())
                            p.id = tcj["id"].get<std::string>();
                        if (tcj.contains("function") && tcj["function"].is_object()) {
                            const auto & fn = tcj["function"];
                            if (fn.contains("name") && fn["name"].is_string()) {
                                if (p.name.empty())
                                    p.name = fn["name"].get<std::string>();
                            }
                            if (fn.contains("arguments") && fn["arguments"].is_string()) {
                                p.arguments += fn["arguments"].get<std::string>();
                            }
                        }
                    }
                }
                if (ch.contains("finish_reason") && ch["finish_reason"].is_string()) {
                    out.finish_reason = ch["finish_reason"].get<std::string>();
                }
                // The server's final SSE chunk carries a `timings` block
                // with the `incomplete` flag.  Keep the LAST seen value
                // (later chunks override earlier ones).
                if (j.contains("timings") && j["timings"].is_object()) {
                    const auto & tm = j["timings"];
                    if (tm.contains("incomplete") && tm["incomplete"].is_boolean()) {
                        out.incomplete = tm["incomplete"].get<bool>();
                    }
                    if (tm.contains("ctx_used") && tm["ctx_used"].is_number_integer()) {
                        out.ctx_used = tm["ctx_used"].get<int>();
                    }
                    if (tm.contains("n_ctx") && tm["n_ctx"].is_number_integer()) {
                        out.n_ctx = tm["n_ctx"].get<int>();
                    }
                }
            }
            return true;
        };

        std::string body = build_chat_body();
        std::string path = base_path() + "/v1/chat/completions";

        if (verbose) {
            std::fprintf(stderr,
                "[easyai-cli] POST %s%s  (body=%zu bytes, tools=%zu, hist=%zu)\n",
                endpoint.c_str(), path.c_str(), body.size(),
                tools.size(), history_json.size());
            if (!tools.empty()) {
                std::fprintf(stderr, "[easyai-cli]   tools[]:");
                for (const auto & t : tools)
                    std::fprintf(stderr, " %s", t.name.c_str());
                std::fputc('\n', stderr);
            }
        }

        if (log_fp) {
            // Header + full request body (NOT pretty-printed — we want
            // the exact bytes that hit the wire) so the log can be
            // replayed against any /v1/chat/completions endpoint.
            const auto now = std::chrono::system_clock::now();
            const auto t   = std::chrono::system_clock::to_time_t(now);
            char ts[32] = {0};
            std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S",
                          std::localtime(&t));
            std::fprintf(log_fp,
                "\n========== REQUEST %s ==========\n"
                "POST %s%s\n"
                "tools=%zu hist=%zu body_bytes=%zu\n"
                "----- BODY -----\n%s\n"
                "----- SSE STREAM (raw) -----\n",
                ts, endpoint.c_str(), path.c_str(),
                tools.size(), history_json.size(), body.size(),
                body.c_str());
            std::fflush(log_fp);
        }

        auto headers = headers_with_auth("text/event-stream");
        // Retry loop — only wraps the pre-stream connect.  Once
        // `received_anything` is true (the server started emitting SSE)
        // we can't safely re-issue: the model already produced bytes
        // the caller has seen, and a fresh attempt would duplicate
        // them.  See is_retryable_httplib() above.
        httplib::Result res;
        const int max_attempts = http_retries + 1;
        for (int attempt = 1; attempt <= max_attempts; ++attempt) {
            if (cancel_requested.load(std::memory_order_relaxed)) {
                last_error = "cancelled";
                return false;
            }
            res = cli->Post(path, headers, body, "application/json",
                            [&](const char * d, size_t l) {
                                return on_chunk(d, l);
                            });
            if (reasoning_aborted) break;  // soft success; handled below
            if (received_anything)  break;  // partial response — keep what we have
            if (res && res->status >= 200 && res->status < 300) break;
            if (!is_retryable_httplib(res, received_anything)) break;
            if (attempt >= max_attempts) {
                easyai::log::error(
                    "[easyai-cli] HTTP attempt %d/%d failed (%s) — "
                    "retry budget exhausted\n",
                    attempt, max_attempts,
                    res ? ("HTTP " + std::to_string(res->status)).c_str()
                        : httplib::to_string(res.error()).c_str());
                break;
            }
            const int backoff = retry_backoff_ms(attempt - 1);
            easyai::log::error(
                "[easyai-cli] HTTP attempt %d/%d failed (%s); retrying in %dms\n",
                attempt, max_attempts,
                res ? ("HTTP " + std::to_string(res->status)).c_str()
                    : httplib::to_string(res.error()).c_str(),
                backoff);
            // Cancellable so a Ctrl-C during the backoff escalates
            // within ~50ms instead of waiting the full delay.
            if (!cancellable_sleep_ms(backoff, cancel_requested)) {
                last_error = "cancelled";
                return false;
            }
        }

        if (reasoning_aborted) {
            // We deliberately closed the stream when reasoning_content
            // exceeded the configured cap.  Treat as a soft success:
            // the caller gets whatever was streamed (typically empty
            // visible content + a long reasoning), the loop terminates,
            // and last_error is set so the operator sees what happened.
            last_error = "reasoning runaway: aborted after "
                       + std::to_string(out.reasoning.size())
                       + " chars (cap=" + std::to_string(max_reasoning_chars)
                       + ").  The model didn't converge to an answer in "
                         "this thinking budget.";
            out.finish_reason = "stop";
            return true;
        }
        if (!res) {
            last_error = "HTTP request failed: "
                       + httplib::to_string(res.error());
            return false;
        }
        if (res->status < 200 || res->status >= 300) {
            last_error = "HTTP " + std::to_string(res->status) + ": " + res->body;
            return false;
        }
        if (!received_anything) {
            last_error = "empty SSE stream from server";
            return false;
        }
        // Promote tool-call map → vector in stable index order.
        for (auto & kv : tc_by_index) out.tool_calls.push_back(std::move(kv.second));

        if (log_fp) {
            std::fprintf(log_fp,
                "\n----- PARSED TURN -----\n"
                "http_status=%d content_bytes=%zu reasoning_bytes=%zu "
                "tool_calls=%zu finish_reason=%s incomplete=%s\n",
                res ? res->status : -1,
                out.content.size(), out.reasoning.size(),
                out.tool_calls.size(),
                out.finish_reason.c_str(),
                out.incomplete ? "yes" : "no");
            for (const auto & p : out.tool_calls) {
                std::fprintf(log_fp,
                    "tool_call: name=%s id=%s args=%s\n",
                    p.name.c_str(),
                    p.id.empty() ? "(none)" : p.id.c_str(),
                    p.arguments.c_str());
            }
            std::fputs("==========\n", log_fp);
            std::fflush(log_fp);
        }
        return true;
    }

    // -----------------------------------------------------------------
    // Build the assistant message JSON we want to push into history.
    // OpenAI shape: {"role":"assistant","content":...,"tool_calls":[...]}
    // -----------------------------------------------------------------
    std::string assistant_msg_json(const AssistantTurn & turn) const {
        ordered_json msg;
        msg["role"]    = "assistant";
        msg["content"] = turn.content;
        if (!turn.tool_calls.empty()) {
            ordered_json tcs = ordered_json::array();
            for (const auto & p : turn.tool_calls) {
                ordered_json tc;
                tc["id"]       = p.id.empty() ? ("call_" + std::to_string(p.index)) : p.id;
                tc["type"]     = "function";
                tc["function"] = { {"name", p.name},
                                   {"arguments", p.arguments.empty()
                                                     ? std::string("{}")
                                                     : p.arguments } };
                tcs.push_back(std::move(tc));
            }
            msg["tool_calls"] = std::move(tcs);
        }
        // Tool output and model content can carry stray non-UTF-8 bytes
        // (e.g. bash piping a sed result with Windows-1252 curly quotes,
        // 0x92). Strict dump throws type_error.316; replace invalid bytes
        // with U+FFFD instead so the agent loop survives.
        return msg.dump(-1, ' ', false, ordered_json::error_handler_t::replace);
    }

    std::string tool_msg_json(const std::string & tool_call_id,
                              const std::string & tool_name,
                              const std::string & content,
                              bool                is_error) const {
        ordered_json msg;
        msg["role"]         = "tool";
        msg["tool_call_id"] = tool_call_id;
        msg["name"]         = tool_name;
        msg["content"]      = is_error ? ("ERROR: " + content) : content;
        return msg.dump(-1, ' ', false, ordered_json::error_handler_t::replace);
    }

    // -----------------------------------------------------------------
    // Local tool dispatch.  Returns the rendered tool message JSON.
    // -----------------------------------------------------------------
    std::string dispatch_one_tool(const PendingToolCall & p) {
        const Tool * found = nullptr;
        const std::string canon = canonical_tool_name(p.name);
        for (const auto & t : tools) if (t.name == canon) { found = &t; break; }

        ToolCall   call{ p.name, p.arguments.empty() ? "{}" : p.arguments,
                         p.id.empty() ? ("call_" + std::to_string(p.index)) : p.id };
        ToolResult result;

        if (!found) {
            result = ToolResult::error("unknown tool: " + p.name);
        } else {
            try {
                result = found->handler(call);
            } catch (const std::exception & e) {
                result = ToolResult::error(std::string("tool threw: ") + e.what());
            } catch (...) {
                result = ToolResult::error("tool threw unknown exception");
            }
        }
        if (on_tool) on_tool(call, result);
        if (log_fp) {
            std::fprintf(log_fp,
                "\n----- TOOL DISPATCH -----\n"
                "name=%s id=%s is_error=%s\n"
                "args=%s\n"
                "result_bytes=%zu\n"
                "result=%s\n"
                "==========\n",
                call.name.c_str(), call.id.c_str(),
                result.is_error ? "yes" : "no",
                call.arguments_json.c_str(),
                result.content.size(),
                result.content.c_str());
            std::fflush(log_fp);
        }
        return tool_msg_json(call.id, call.name, result.content, result.is_error);
    }

    // -----------------------------------------------------------------
    // Agentic multi-hop loop.  Mirrors Engine::chat_continue limits.
    // -----------------------------------------------------------------
    // Agentic-loop safety cap: how many tool round-trips we allow before
    // bailing out.  Configurable via Client::max_tool_hops (default 8).
    // For bash-enabled flows the caller bumps this much higher because
    // a single shell command can naturally span many turns.

    std::string run_chat_loop() {
        last_was_incomplete         = false;
        last_was_ctx_full           = false;
        int incomplete_retries      = 0;

        // Snapshot history size before the loop runs. We use this on the
        // exhausted-retry exit path to roll back the nudge(s) and the
        // empty assistant turn we'd otherwise leave behind — see the
        // post-retry-branch rollback below for the full rationale.
        const size_t history_size_at_start = history_json.size();

        // We only push the corrective nudge ONCE per call. Stacking 10
        // identical nudges (the old behaviour) just inflates the history
        // without changing the prompt's meaning, and on later turns the
        // chat template can throw on 10 consecutive user messages.
        bool nudge_pushed = false;

        for (int hop = 0; hop < max_tool_hops; ++hop) {
            // Honour cancel between agentic hops too — the per-chunk
            // check inside stream_chat catches mid-stream aborts; this
            // catches aborts that arrive AFTER a hop returns and
            // BEFORE the next stream_chat starts (e.g. signal arriving
            // during local tool dispatch).
            if (cancel_requested.load(std::memory_order_relaxed)) {
                last_error = "cancelled";
                return {};
            }
            AssistantTurn turn;
            if (!stream_chat(turn)) {
                if (!last_error.empty()) {
                    easyai::log::error(
                        "[easyai-cli] Client::run_chat_loop hop %d: stream_chat failed: %s",
                        hop, last_error.c_str());
                }
                return {};
            }

            if (verbose) {
                std::fprintf(stderr,
                    "[easyai-cli] hop %d done: content=%zu reasoning=%zu "
                    "tool_calls=%zu finish=%s incomplete=%s\n",
                    hop,
                    turn.content.size(),
                    turn.reasoning.size(),
                    turn.tool_calls.size(),
                    turn.finish_reason.c_str(),
                    turn.incomplete ? "yes" : "no");
                for (const auto & p : turn.tool_calls) {
                    std::string args = p.arguments;
                    if (args.size() > 160) args = args.substr(0, 160) + "…";
                    for (char & c : args) if (c == '\n' || c == '\r') c = ' ';
                    std::fprintf(stderr,
                        "[easyai-cli]   tool_call name=%s id=%s args=%s\n",
                        p.name.c_str(),
                        p.id.empty() ? "(none)" : p.id.c_str(),
                        args.c_str());
                }
                if (turn.tool_calls.empty() && turn.incomplete) {
                    std::string tail = turn.content;
                    if (tail.size() > 160) tail = "…" + tail.substr(tail.size() - 160);
                    for (char & c : tail) if (c == '\n' || c == '\r') c = ' ';
                    std::fprintf(stderr,
                        "[easyai-cli]   content tail: %s\n", tail.c_str());
                }
            }

            // Opt-in retry on incomplete turns.  Discards the bad
            // assistant entry from history (we never push it) and
            // re-emits the conversation ONCE, this time with a
            // corrective user nudge so the model has something
            // pointed to react to — a blind retry on the SAME prompt
            // tends to reproduce the SAME announce-and-stop bailout.
            //
            // The nudge stays in history after the retry. That's the
            // honest record of what we sent, and downstream turns are
            // fine with it.
            if (turn.incomplete && retry_on_incomplete
                                 && incomplete_retries < max_incomplete_retries) {
                ++incomplete_retries;
                // Mark the bad transaction in the raw log so an
                // operator can grep `PROBLEMATIC` and find every retry.
                {
                    std::string tail = turn.content;
                    if (tail.size() > 400)
                        tail = "…" + tail.substr(tail.size() - 400);
                    easyai::log::mark_problem(
                        "Client::run_chat_loop announce-without-action "
                        "(retry %d/%d) hop=%d content_bytes=%zu reasoning_bytes=%zu "
                        "tool_calls=%zu finish=%s\ncontent: %s",
                        incomplete_retries, max_incomplete_retries,
                        hop, turn.content.size(), turn.reasoning.size(),
                        turn.tool_calls.size(),
                        turn.finish_reason.c_str(),
                        tail.c_str());
                }
                if (!nudge_pushed) {
                    ordered_json nudge;
                    nudge["role"]    = "user";
                    nudge["content"] =
                        "Your previous reply only announced an action without "
                        "emitting any tool_call. Do NOT say \"let me…\", "
                        "\"I'll…\", or similar setup phrases unless the "
                        "tool_call follows in the SAME turn. Right now: "
                        "either call the next tool you actually need to make "
                        "progress, or give the user the final answer. Pick "
                        "one and execute it now.";
                    // history_json is std::vector<std::string> (raw JSON); we
                    // serialise here.  Earlier this pushed `nudge` directly,
                    // which threw json::type_error 302 at runtime when the
                    // implicit nlohmann json→string conversion ran on an
                    // object-typed value.
                    history_json.push_back(nudge.dump());
                    nudge_pushed = true;
                }
                // On retries 2..N the nudge is already at the tail; we
                // re-issue the same prompt and rely on sampling
                // stochasticity (the same input is the only thing the
                // retry can reasonably try again with).
                if (verbose) {
                    std::fprintf(stderr,
                        "[easyai-cli] retry_on_incomplete: discarding bad turn "
                        "(content=%zu, tool_calls=%zu), nudging, and re-issuing "
                        "(%d/%d)\n",
                        turn.content.size(), turn.tool_calls.size(),
                        incomplete_retries, max_incomplete_retries);
                }
                continue;
            }

            // Exhausted-retry exit (or retry_on_incomplete was off) on
            // an incomplete turn. Roll history back to the pre-loop
            // snapshot so the nudge and the empty assistant turn don't
            // poison the NEXT user turn — without this rollback, every
            // subsequent request the CLI sends carries the same junk
            // and the server bounces immediately into another
            // exhausted-retry loop (template rendering throws on the
            // sequence of consecutive user messages we built up). The
            // user's most recent message remains in place; the CLI
            // surfaces turn.incomplete to the caller which renders the
            // "(incomplete response — ...)" placeholder against it.
            if (turn.incomplete) {
                history_json.resize(history_size_at_start);
                last_was_incomplete = true;
                easyai::log::mark_problem(
                    "Client::run_chat_loop exhausted incomplete retries — "
                    "rolling history back to %zu entries "
                    "(retries=%d/%d, retry_on_incomplete=%s) hop=%d "
                    "content_bytes=%zu reasoning_bytes=%zu",
                    history_size_at_start,
                    incomplete_retries, max_incomplete_retries,
                    retry_on_incomplete ? "on" : "off",
                    hop, turn.content.size(), turn.reasoning.size());
                return turn.content;
            }

            history_json.push_back(assistant_msg_json(turn));
            last_was_incomplete = turn.incomplete;
            if (turn.ctx_used >= 0) last_ctx_used = turn.ctx_used;
            if (turn.n_ctx    >  0) last_n_ctx    = turn.n_ctx;

            // Hard ceiling on context fill — once the chat has filled
            // (or about to fill) n_ctx, the next hop will either
            // truncate from the head or just OOM the request.  Stop
            // here, surface the latest assistant content, and tag the
            // turn so the app layer can show a clear note.  Threshold
            // 0 disables; default 100 = pinned at the wall.
            if (stop_at_ctx_pct > 0
                    && last_ctx_used >= 0
                    && last_n_ctx    >  0
                    && (long long) last_ctx_used * 100
                           >= (long long) stop_at_ctx_pct * last_n_ctx) {
                last_was_ctx_full = true;
                last_error = "context full ("
                           + std::to_string(last_ctx_used) + "/"
                           + std::to_string(last_n_ctx) + " tokens — "
                           + std::to_string(stop_at_ctx_pct)
                           + "%) — stopping agentic loop. "
                             "Start a new chat to free the context window.";
                easyai::log::mark_problem(
                    "Client::run_chat_loop ctx full hop=%d ctx_used=%d n_ctx=%d "
                    "threshold=%d%%",
                    hop, last_ctx_used, last_n_ctx, stop_at_ctx_pct);
                return turn.content;
            }

            // If we land here with `turn.incomplete` set, the retry budget
            // was exhausted (or retry_on_incomplete was off).  Mark it
            // loudly in the raw log so the operator can correlate the
            // empty-bubble with the wire-level transcript.
            if (turn.incomplete) {
                easyai::log::mark_problem(
                    "Client::run_chat_loop incomplete turn surfaced to caller "
                    "(retries=%d/%d, retry_on_incomplete=%s) hop=%d "
                    "content_bytes=%zu reasoning_bytes=%zu",
                    incomplete_retries, max_incomplete_retries,
                    retry_on_incomplete ? "on" : "off",
                    hop, turn.content.size(), turn.reasoning.size());
            }

            if (turn.finish_reason != "tool_calls" || turn.tool_calls.empty()) {
                return turn.content;
            }
            for (const auto & p : turn.tool_calls) {
                history_json.push_back(dispatch_one_tool(p));
            }
            // Loop continues — request next turn with tool results in history.
        }
        last_error = "max tool hops (" + std::to_string(max_tool_hops) + ") exceeded";
        easyai::log::error("[easyai-cli] %s", last_error.c_str());
        return {};
    }

    // -----------------------------------------------------------------
    // Direct-endpoint helpers — share a synchronous GET/POST path.
    // -----------------------------------------------------------------
    bool simple_get(const std::string & path,
                    const std::string & accept,
                    std::string & out_body,
                    int *         out_status = nullptr) {
        auto * cli = get_http();
        if (!cli) return false;
        auto headers = headers_with_auth(accept);
        const std::string full_path = base_path() + path;
        const int max_attempts = http_retries + 1;
        httplib::Result res;
        for (int attempt = 1; attempt <= max_attempts; ++attempt) {
            res = cli->Get(full_path, headers);
            if (res && res->status >= 200 && res->status < 300) break;
            if (!is_retryable_httplib(res, /*received=*/false)) break;
            if (attempt >= max_attempts) break;
            const int backoff = retry_backoff_ms(attempt - 1);
            easyai::log::error(
                "[easyai-cli] GET %s attempt %d/%d failed (%s); retrying in %dms\n",
                full_path.c_str(), attempt, max_attempts,
                res ? ("HTTP " + std::to_string(res->status)).c_str()
                    : httplib::to_string(res.error()).c_str(),
                backoff);
            if (!cancellable_sleep_ms(backoff, cancel_requested)) {
                last_error = "cancelled";
                return false;
            }
        }
        if (!res) {
            last_error = "HTTP request failed: "
                       + httplib::to_string(res.error());
            return false;
        }
        if (out_status) *out_status = res->status;
        out_body = res->body;
        if (res->status < 200 || res->status >= 300) {
            last_error = "HTTP " + std::to_string(res->status) + ": " + res->body;
            return false;
        }
        return true;
    }

    bool simple_post(const std::string & path,
                     const std::string & body,
                     std::string & out_body) {
        auto * cli = get_http();
        if (!cli) return false;
        auto headers = headers_with_auth("application/json");
        const std::string full_path = base_path() + path;
        const int max_attempts = http_retries + 1;
        httplib::Result res;
        for (int attempt = 1; attempt <= max_attempts; ++attempt) {
            res = cli->Post(full_path, headers, body, "application/json");
            if (res && res->status >= 200 && res->status < 300) break;
            if (!is_retryable_httplib(res, /*received=*/false)) break;
            if (attempt >= max_attempts) break;
            const int backoff = retry_backoff_ms(attempt - 1);
            easyai::log::error(
                "[easyai-cli] POST %s attempt %d/%d failed (%s); retrying in %dms\n",
                full_path.c_str(), attempt, max_attempts,
                res ? ("HTTP " + std::to_string(res->status)).c_str()
                    : httplib::to_string(res.error()).c_str(),
                backoff);
            if (!cancellable_sleep_ms(backoff, cancel_requested)) {
                last_error = "cancelled";
                return false;
            }
        }
        if (!res) {
            last_error = "HTTP request failed: "
                       + httplib::to_string(res.error());
            return false;
        }
        out_body = res->body;
        if (res->status < 200 || res->status >= 300) {
            last_error = "HTTP " + std::to_string(res->status) + ": " + res->body;
            return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------
Client::Client() : p_(std::make_unique<Impl>()) {
    // Auto-open a /tmp raw transaction log on first Client construction
    // so every consumer of libeasyai-cli (cli-remote, RemoteBackend,
    // Agent, third-party apps) inherits the log natively.  No-op when
    // a sink was already attached (CLI binary opened one earlier) or
    // when EASYAI_NO_AUTO_LOG=1.  We adopt the global sink as our
    // per-client log_fp so the existing wire-tee path (request body +
    // raw SSE chunks + tool dispatch summaries) lands in the same file.
    easyai::log::auto_open("easyai-client");
    if (!p_->log_fp) p_->log_fp = easyai::log::file();
}
Client::~Client() = default;
Client::Client(Client &&) noexcept = default;
Client & Client::operator=(Client &&) noexcept = default;

// Setters that affect the persistent httplib::Client's configuration
// invalidate the cached connection so the next request rebuilds it
// with the new settings. Without this the persistent-Client
// optimization (one TCP connection across an N-hop session) silently
// pinned whatever endpoint / TLS posture / timeout was in force at the
// FIRST request and ignored any later mutation — a dev who flipped
// `tls_insecure(false)` mid-session would still reach the staging
// server with verification off. Drop http_ here and let get_http()
// re-create cleanly.
Client & Client::endpoint(std::string url) {
    std::lock_guard<std::mutex> lk(p_->http_mu_);
    if (p_->endpoint != url) p_->http_.reset();
    p_->endpoint = std::move(url);
    return *this;
}
Client & Client::api_key         (std::string key) { p_->api_key         = std::move(key); return *this; }
Client & Client::timeout_seconds (int s) {
    std::lock_guard<std::mutex> lk(p_->http_mu_);
    if (p_->timeout_seconds != s) p_->http_.reset();
    p_->timeout_seconds = s;
    return *this;
}
Client & Client::verbose         (bool v)          { p_->verbose         = v;   return *this; }
Client & Client::http_retries    (int  n)          { p_->http_retries    = n < 0 ? 0 : n; return *this; }
Client & Client::log_file        (std::FILE * fp)  { p_->log_fp          = fp;  return *this; }
Client & Client::tls_insecure(bool v) {
    std::lock_guard<std::mutex> lk(p_->http_mu_);
    if (p_->tls_insecure != v) p_->http_.reset();
    p_->tls_insecure = v;
    return *this;
}
Client & Client::ca_cert_path(std::string path) {
    std::lock_guard<std::mutex> lk(p_->http_mu_);
    if (p_->tls_ca_path != path) p_->http_.reset();
    p_->tls_ca_path = std::move(path);
    return *this;
}
Client & Client::max_reasoning_chars(int n)        { p_->max_reasoning_chars = n; return *this; }
Client & Client::retry_on_incomplete(bool v)       { p_->retry_on_incomplete = v; return *this; }
Client & Client::max_tool_hops      (int n)         { if (n > 0) p_->max_tool_hops = n; return *this; }
bool     Client::last_turn_was_incomplete() const   { return p_->last_was_incomplete; }
int      Client::last_ctx_used()             const   { return p_->last_ctx_used; }
int      Client::last_n_ctx()                 const  { return p_->last_n_ctx; }
int      Client::last_ctx_pct()               const  {
    if (p_->last_ctx_used < 0 || p_->last_n_ctx <= 0) return -1;
    long long n = (long long) p_->last_ctx_used * 100;
    int pct = (int) (n / p_->last_n_ctx);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}
Client & Client::stop_at_ctx_pct(int pct) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    p_->stop_at_ctx_pct = pct;
    return *this;
}
bool     Client::last_was_ctx_full()          const  { return p_->last_was_ctx_full; }

Client & Client::model              (std::string id)     { p_->model_id          = std::move(id);     return *this; }
Client & Client::system             (std::string prompt) { p_->system_prompt     = std::move(prompt); return *this; }
Client & Client::temperature        (float t)            { p_->temperature       = t;                 return *this; }
Client & Client::top_p              (float v)            { p_->top_p             = v;                 return *this; }
Client & Client::top_k              (int   v)            { p_->top_k             = v;                 return *this; }
Client & Client::min_p              (float v)            { p_->min_p             = v;                 return *this; }
Client & Client::repeat_penalty     (float v)            { p_->repeat_penalty    = v;                 return *this; }
Client & Client::frequency_penalty  (float v)            { p_->frequency_penalty = v;                 return *this; }
Client & Client::presence_penalty   (float v)            { p_->presence_penalty  = v;                 return *this; }
Client & Client::seed               (long long s)        { p_->seed              = s;                 return *this; }
Client & Client::max_tokens         (int   n)            { p_->max_tokens        = n;                 return *this; }
Client & Client::stop               (std::vector<std::string> s) { p_->stop_sequences = std::move(s); return *this; }
Client & Client::extra_body_json    (std::string raw)    { p_->extra_body_raw    = std::move(raw);    return *this; }

Client & Client::add_tool   (Tool t) { p_->tools.push_back(std::move(t)); return *this; }
Client & Client::clear_tools()       { p_->tools.clear();                  return *this; }
const std::vector<Tool> & Client::tools() const { return p_->tools; }

Client & Client::on_token  (TokenCallback cb) { p_->on_token  = std::move(cb); return *this; }
Client & Client::on_reason (TokenCallback cb) { p_->on_reason = std::move(cb); return *this; }
Client & Client::on_tool   (ToolCallback  cb) { p_->on_tool   = std::move(cb); return *this; }
Client & Client::on_prompt_progress(PromptProgressCallback cb) {
    p_->on_prompt_progress = std::move(cb); return *this;
}
Client & Client::on_prompt_eval(PromptEvalCallback cb) {
    p_->on_prompt_eval = std::move(cb); return *this;
}

// Cooperative cancel — see client.hpp for design notes.
Client & Client::request_cancel() {
    p_->cancel_requested.store(true, std::memory_order_relaxed);
    return *this;
}
Client & Client::clear_cancel() {
    p_->cancel_requested.store(false, std::memory_order_relaxed);
    return *this;
}
bool     Client::cancel_requested() const {
    return p_->cancel_requested.load(std::memory_order_relaxed);
}

std::string Client::chat(const std::string & user_message) {
    p_->last_error.clear();
    // Snapshot BEFORE pushing the user turn so we can roll all the way
    // back if the loop ends incomplete. Without this pop, the failed
    // user message would land in the saved .easyai_session file; on
    // --continue the next user input would create two consecutive
    // "user" turns and the chat template throws (Qwen3-class templates
    // require user/assistant alternation), reproducing the exhausted-
    // retry loop the user already escaped from.
    const size_t pre_user_size = p_->history_json.size();
    ordered_json u;
    u["role"]    = "user";
    u["content"] = user_message;
    p_->history_json.push_back(u.dump());
    std::string answer = p_->run_chat_loop();
    if (p_->last_was_incomplete) {
        // run_chat_loop already trimmed any nudges and the empty
        // assistant turn; this pops the user message too so the
        // session-file write below sees a fully-clean history that
        // matches the state at the end of the last successful turn.
        p_->history_json.resize(pre_user_size);
    }
    return answer;
}

std::string Client::chat_continue() {
    p_->last_error.clear();
    return p_->run_chat_loop();
}

void Client::clear_history() {
    p_->history_json.clear();
    p_->last_error.clear();
}

std::string Client::dump_history() const {
    // history_json is already a vector of per-message JSON strings; we
    // just reassemble them into a single array and dump.  Use ordered_json
    // so the resulting file preserves message field order (role / content
    // / tool_calls / tool_call_id), which makes diffs against a saved
    // session readable.
    ordered_json arr = ordered_json::array();
    for (const auto & raw : p_->history_json) {
        try {
            arr.push_back(ordered_json::parse(raw));
        } catch (const std::exception &) {
            // Skip an unparseable cached entry rather than corrupt the
            // whole dump.  In practice this is unreachable — every entry
            // in history_json was produced by a `.dump()` call on the
            // same library — but defensive against future code paths
            // that might push a malformed string.
        }
    }
    return arr.dump(2);
}

bool Client::load_history(const std::string & json_array, std::string * err) {
    ordered_json parsed;
    try {
        parsed = ordered_json::parse(json_array);
    } catch (const std::exception & e) {
        if (err) *err = std::string("invalid JSON: ") + e.what();
        return false;
    }
    if (!parsed.is_array()) {
        if (err) *err = "expected a JSON array of message objects";
        return false;
    }
    std::vector<std::string> new_history;
    new_history.reserve(parsed.size());
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        const auto & msg = parsed[i];
        if (!msg.is_object()) {
            if (err) {
                *err = "messages[" + std::to_string(i)
                     + "] is not an object";
            }
            return false;
        }
        if (!msg.contains("role") || !msg["role"].is_string()) {
            if (err) {
                *err = "messages[" + std::to_string(i)
                     + "] missing string field \"role\"";
            }
            return false;
        }
        new_history.push_back(msg.dump());
    }
    p_->history_json = std::move(new_history);
    p_->last_error.clear();
    return true;
}

// ---- direct endpoints -----------------------------------------------------
bool Client::list_models(std::vector<RemoteModel> & out) {
    out.clear();
    std::string body;
    if (!p_->simple_get("/v1/models", "application/json", body)) return false;
    try {
        auto j = json::parse(body);
        if (!j.contains("data") || !j["data"].is_array()) return true;
        for (const auto & e : j["data"]) {
            RemoteModel m;
            m.id       = e.value("id", "");
            m.owned_by = e.value("owned_by", "");
            m.created  = e.value("created", 0L);
            out.push_back(std::move(m));
        }
        return true;
    } catch (const std::exception & e) {
        p_->last_error = std::string("list_models: bad JSON: ") + e.what();
        return false;
    }
}

bool Client::list_remote_tools(std::vector<RemoteTool> & out) {
    out.clear();
    std::string body;
    if (!p_->simple_get("/v1/tools", "application/json", body)) return false;
    try {
        auto j = json::parse(body);
        if (!j.contains("data") || !j["data"].is_array()) return true;
        for (const auto & e : j["data"]) {
            RemoteTool t;
            t.name        = e.value("name", "");
            t.description = e.value("description", "");
            out.push_back(std::move(t));
        }
        return true;
    } catch (const std::exception & e) {
        p_->last_error = std::string("list_remote_tools: bad JSON: ") + e.what();
        return false;
    }
}

bool Client::health() {
    std::string body;
    return p_->simple_get("/health", "application/json", body);
}

bool Client::metrics(std::string & out_text) {
    return p_->simple_get("/metrics", "text/plain", out_text);
}

bool Client::props(std::string & out_json) {
    return p_->simple_get("/props", "application/json", out_json);
}

bool Client::set_preset(const std::string & preset_name) {
    ordered_json req;
    req["preset"] = preset_name;
    std::string body;
    return p_->simple_post("/v1/preset", req.dump(), body);
}

std::string Client::last_error() const { return p_->last_error; }

}  // namespace easyai
