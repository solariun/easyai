// src/mcp_client.cpp — MCP client over HTTP JSON-RPC.
//
// Implementation notes:
//
//   * libcurl as the transport. easyai already depends on it
//     conditionally for fetch_web / search_web; reusing means no
//     new link dep and no new build flag.
//
//   * A single libcurl easy handle per Conn, guarded by a mutex.
//     curl_easy_perform is not reentrant on the same handle; an
//     agent firing several tools/call requests concurrently would
//     race the handle without the lock. The lock is fine-grained
//     (held only across one HTTP exchange) so contention stays
//     bounded.
//
//   * One Conn shared by every Tool the client emits. Held via
//     shared_ptr captured in each handler closure, so the
//     connection lives as long as any returned Tool is reachable.
//     fetch_remote_tools() does the handshake and walks away —
//     the runtime keeps the Conn through the Tools' lifetimes.
//
//   * No retry, no reconnect. JSON-RPC over HTTP is stateless;
//     transient failures surface as ToolResult::error with the
//     curl message attached. The operator decides whether to
//     retry by re-issuing through the model.
//
//   * Authorization: Bearer is the only supported auth — matches
//     what easyai-server's /mcp endpoint enforces and what
//     [MCP_USER] in the INI populates.

#include "easyai/mcp_client.hpp"
#include "easyai/log.hpp"
#include "easyai/tool.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(EASYAI_HAVE_CURL)
#include <curl/curl.h>
#endif

namespace easyai::mcp {

#if defined(EASYAI_HAVE_CURL)

namespace {

using nlohmann::json;

// Cap on how much we'll buffer from a single MCP response. Real
// tools/list payloads are <100 KiB; an adversarial server replying
// with a multi-gigabyte JSON blob would otherwise make the agent
// run out of memory. 4 MiB is far past any legitimate response.
constexpr std::size_t kMaxResponseBytes = 4u * 1024u * 1024u;

// Per-connection state. One Conn lives behind a shared_ptr captured
// by every emitted Tool's handler — the libcurl handle (`h`) lasts
// as long as any of those Tools.
struct Conn {
    std::string         url;             // base, no trailing /mcp
    std::string         bearer;
    int                 timeout_seconds = 20;
    int                 retries         = 5;   // extra attempts on transient failure
    std::mutex          mu;              // guards the easy handle below
    CURL *              h = nullptr;
    std::atomic<long>   next_id{1};

    Conn() = default;
    ~Conn() { if (h) curl_easy_cleanup(h); }

    Conn(const Conn &) = delete;
    Conn & operator=(const Conn &) = delete;
};

// True for transient libcurl errors worth retrying. Permanent errors
// (bad URL, SSL cert) and 4xx HTTP fall through and surface as-is.
bool curl_error_is_retryable(CURLcode rc) {
    switch (rc) {
        case CURLE_COULDNT_CONNECT:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
            return true;
        default:
            return false;
    }
}

// Exponential backoff: 250ms, 500ms, 1s, 2s, 4s, capped.
int mcp_retry_backoff_ms(int attempt) {
    int ms = 250 << (attempt > 4 ? 4 : attempt);
    return ms > 4000 ? 4000 : ms;
}

// libcurl write callback that appends to a std::string up to
// kMaxResponseBytes. Returning short of `incoming` makes curl abort
// with CURLE_WRITE_ERROR — that's the right signal here: a response
// that exceeds our cap is a protocol violation we don't want to
// silently accept.
size_t curl_write_cb(void * buf, size_t sz, size_t n, void * ud) {
    auto * out = static_cast<std::string *>(ud);
    const std::size_t incoming = sz * n;
    if (out->size() + incoming > kMaxResponseBytes) {
        return 0;  // signal abort
    }
    out->append(static_cast<char *>(buf), incoming);
    return incoming;
}

// One JSON-RPC POST against `<url>/mcp`. On success returns true
// with the response body in `out`; on any kind of failure returns
// false with `err` populated. Holds the connection mutex for the
// duration so concurrent calls don't race the easy handle.
bool http_post_json(Conn & c, const std::string & body,
                    std::string & out, std::string & err) {
    std::lock_guard<std::mutex> lk(c.mu);

    if (!c.h) {
        c.h = curl_easy_init();
        if (!c.h) { err = "curl_easy_init failed"; return false; }
    }
    // Reset everything that might leak from a previous call.
    curl_easy_reset(c.h);
    out.clear();

    const std::string full_url = c.url + "/mcp";

    curl_easy_setopt(c.h, CURLOPT_URL,            full_url.c_str());
    curl_easy_setopt(c.h, CURLOPT_POST,           1L);
    curl_easy_setopt(c.h, CURLOPT_POSTFIELDS,     body.data());
    curl_easy_setopt(c.h, CURLOPT_POSTFIELDSIZE,  (long) body.size());
    curl_easy_setopt(c.h, CURLOPT_WRITEFUNCTION,  curl_write_cb);
    curl_easy_setopt(c.h, CURLOPT_WRITEDATA,      &out);
    curl_easy_setopt(c.h, CURLOPT_TIMEOUT,        (long) c.timeout_seconds);
    curl_easy_setopt(c.h, CURLOPT_NOSIGNAL,       1L);
    curl_easy_setopt(c.h, CURLOPT_FOLLOWLOCATION, 1L);
    // Keep transport-protocol surface tight: only http(s). MCP servers
    // never legitimately redirect to file://, gopher://, etc.
#if defined(LIBCURL_VERSION_NUM) && LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c.h, CURLOPT_PROTOCOLS_STR,         "http,https");
    curl_easy_setopt(c.h, CURLOPT_REDIR_PROTOCOLS_STR,   "http,https");
#else
    curl_easy_setopt(c.h, CURLOPT_PROTOCOLS,
                     (long) (CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c.h, CURLOPT_REDIR_PROTOCOLS,
                     (long) (CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

    curl_slist * headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    std::string auth_hdr;  // outlives the curl_perform — slist holds a ptr
    if (!c.bearer.empty()) {
        auth_hdr = "Authorization: Bearer " + c.bearer;
        headers = curl_slist_append(headers, auth_hdr.c_str());
    }
    curl_easy_setopt(c.h, CURLOPT_HTTPHEADER, headers);

    // Retry loop — only spans pre-stream errors.  http_post_json reads
    // the whole response into `out` before returning, so there's no
    // partial-stream concern.  Reset `out` between attempts so a
    // partial body from a failed try doesn't leak into the next.
    const int max_attempts = (c.retries < 0 ? 0 : c.retries) + 1;
    CURLcode rc = CURLE_OK;
    long http_code = 0;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        out.clear();
        rc = curl_easy_perform(c.h);
        http_code = 0;
        curl_easy_getinfo(c.h, CURLINFO_RESPONSE_CODE, &http_code);

        const bool curl_ok = (rc == CURLE_OK);
        const bool http_ok = (http_code >= 200 && http_code < 300)
                          || http_code == 0;  // 0 when curl failed pre-headers
        if (curl_ok && http_ok) break;

        // Decide whether to retry.  Transient curl error → yes.
        // 5xx → yes.  Permanent curl error or 4xx → no.
        const bool retryable = (!curl_ok && curl_error_is_retryable(rc))
                            || (curl_ok && http_code >= 500 && http_code < 600);
        if (!retryable) break;
        if (attempt >= max_attempts) {
            easyai::log::error(
                "[easyai-mcp] %s attempt %d/%d failed (%s) — "
                "retry budget exhausted\n",
                full_url.c_str(), attempt, max_attempts,
                curl_ok ? ("HTTP " + std::to_string(http_code)).c_str()
                        : curl_easy_strerror(rc));
            break;
        }
        const int backoff = mcp_retry_backoff_ms(attempt - 1);
        easyai::log::error(
            "[easyai-mcp] %s attempt %d/%d failed (%s); retrying in %dms\n",
            full_url.c_str(), attempt, max_attempts,
            curl_ok ? ("HTTP " + std::to_string(http_code)).c_str()
                    : curl_easy_strerror(rc),
            backoff);
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
    }
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        err = std::string("curl: ") + curl_easy_strerror(rc);
        return false;
    }
    if (http_code >= 400) {
        // Surface the body too — MCP servers often put a JSON-RPC
        // error envelope into a 4xx body. Capped to 1 KiB so we
        // don't blow up the operator's log / model context.
        std::string snippet = out.substr(0, std::min<std::size_t>(out.size(), 1024));
        err = "HTTP " + std::to_string(http_code) + ": " + snippet;
        return false;
    }
    return true;
}

// Build the per-tool handler. Closes over the shared Conn so every
// tool emitted by one fetch_remote_tools() call keeps using the
// same upstream connection.
ToolHandler make_remote_handler(std::shared_ptr<Conn> conn,
                                std::string remote_name) {
    return [conn, remote_name](const ToolCall & tc) -> ToolResult {
        // The model's arguments come in as a JSON string. MCP's
        // tools/call expects them as a JSON object. Parse + re-embed.
        // An empty arguments string (some models emit "" instead of "{}"
        // for zero-arg calls) becomes an empty object.
        json args;
        if (tc.arguments_json.empty()) {
            args = json::object();
        } else {
            try {
                args = json::parse(tc.arguments_json);
            } catch (const std::exception & e) {
                return ToolResult::error(
                    std::string("mcp: invalid arguments JSON: ") + e.what());
            }
        }
        if (!args.is_object()) {
            // Upstream tools always take an object per MCP spec; if the
            // model emitted a bare array / scalar, wrap it so we don't
            // send a non-object on the wire (which would 4xx).
            args = json::object();
        }

        const long id = conn->next_id.fetch_add(1, std::memory_order_relaxed);
        const json req = {
            { "jsonrpc", "2.0" },
            { "id",      id    },
            { "method",  "tools/call" },
            { "params",  {
                { "name",      remote_name },
                { "arguments", args }
            }}
        };

        std::string body, err;
        if (!http_post_json(*conn, req.dump(), body, err)) {
            return ToolResult::error("mcp: " + err);
        }

        json resp;
        try { resp = json::parse(body); }
        catch (const std::exception & e) {
            return ToolResult::error(
                std::string("mcp: malformed response JSON: ") + e.what());
        }

        // JSON-RPC error envelope (auth failure, unknown method,
        // server-side handler exception, etc).
        if (resp.contains("error") && resp["error"].is_object()) {
            const std::string msg = resp["error"].value("message", "unknown error");
            return ToolResult::error("mcp error: " + msg);
        }
        if (!resp.contains("result") || !resp["result"].is_object()) {
            return ToolResult::error("mcp: response missing result object");
        }

        // MCP success shape:
        //   { "content": [{"type":"text", "text":"..."}], "isError": bool }
        // We concatenate every text part. Non-text parts (image,
        // resource references) aren't useful to our chat-template
        // pipeline; flag them so the operator can decide.
        const auto & result = resp["result"];
        const bool   is_error = result.value("isError", false);

        std::string text;
        bool        saw_unknown_part = false;
        if (result.contains("content") && result["content"].is_array()) {
            for (const auto & part : result["content"]) {
                if (!part.is_object()) continue;
                const std::string ty = part.value("type", "");
                if (ty == "text") {
                    text += part.value("text", "");
                } else if (!ty.empty()) {
                    saw_unknown_part = true;
                }
            }
        }
        if (text.empty() && saw_unknown_part) {
            // The remote returned only non-text content (image, etc).
            // Without something stringifiable we can't feed the model;
            // mark the call as errored so the agent can react.
            return ToolResult::error(
                "mcp: remote tool returned only non-text content "
                "(images, resources) — unsupported in this client");
        }
        if (is_error) return ToolResult::error(text);
        return ToolResult::ok(std::move(text));
    };
}

// Strip trailing slashes off a URL so we can append /mcp without
// doubling. Tolerates the empty case.
std::string normalize_url(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

}  // namespace

std::vector<Tool> fetch_remote_tools(const ClientOptions & opts,
                                     std::string & err) {
    std::vector<Tool> out;
    err.clear();

    if (opts.url.empty()) {
        err = "mcp client: url is empty";
        return out;
    }
    // Defence-in-depth: libcurl's CURLOPT_PROTOCOLS_STR also blocks
    // non-http(s) schemes at transport time, but rejecting here gives
    // every caller (server.cpp, future callers, embedders) a uniform
    // "scheme must be http(s)://" error instead of a curl diagnostic
    // that varies by version.
    if (opts.url.compare(0, 7,  "http://")  != 0
     && opts.url.compare(0, 8,  "https://") != 0) {
        err = "mcp client: url must start with http:// or https:// (got: "
            + opts.url + ")";
        return out;
    }

    auto conn = std::make_shared<Conn>();
    conn->url             = normalize_url(opts.url);
    conn->bearer          = opts.bearer_token;
    conn->timeout_seconds = opts.timeout_seconds > 0 ? opts.timeout_seconds : 20;
    conn->retries         = opts.retries < 0 ? 0 : opts.retries;

    // ---------- initialize ----------
    // We claim the same protocol version easyai-server advertises;
    // a strict server might reject a different one, but any sane
    // peer accepts our handshake regardless. We don't validate the
    // server's response beyond shape — the goal here is "did we
    // make it through auth".
    const json init_req = {
        { "jsonrpc", "2.0" },
        { "id",      conn->next_id.fetch_add(1, std::memory_order_relaxed) },
        { "method",  "initialize" },
        { "params",  {
            { "protocolVersion", "2024-11-05" },
            { "capabilities",    json::object() },
            { "clientInfo", {
                { "name",    "easyai-mcp-client" },
                { "version", "0.1.0" }
            }}
        }}
    };
    std::string body;
    if (!http_post_json(*conn, init_req.dump(), body, err)) {
        return out;
    }
    try {
        const auto resp = json::parse(body);
        if (resp.contains("error") && resp["error"].is_object()) {
            err = "mcp initialize: "
                + resp["error"].value("message", std::string("unknown error"));
            return out;
        }
    } catch (const std::exception & e) {
        err = std::string("mcp initialize: malformed response: ") + e.what();
        return out;
    }

    // ---------- notifications/initialized ----------
    // A notification (no `id` field), so per JSON-RPC 2.0 the server
    // sends no response body. easyai's own server treats it as a
    // recognised notification and 204s; other servers may behave
    // differently. We send it for protocol correctness and ignore
    // the result entirely — even an HTTP error here doesn't fail
    // the handshake.
    const json init_notify = {
        { "jsonrpc", "2.0" },
        { "method",  "notifications/initialized" }
    };
    std::string notify_body, notify_err;
    (void) http_post_json(*conn, init_notify.dump(), notify_body, notify_err);

    // ---------- tools/list ----------
    const json list_req = {
        { "jsonrpc", "2.0" },
        { "id",      conn->next_id.fetch_add(1, std::memory_order_relaxed) },
        { "method",  "tools/list" }
    };
    body.clear();
    if (!http_post_json(*conn, list_req.dump(), body, err)) {
        return out;
    }

    json resp;
    try { resp = json::parse(body); }
    catch (const std::exception & e) {
        err = std::string("mcp tools/list: malformed response: ") + e.what();
        return out;
    }
    if (resp.contains("error") && resp["error"].is_object()) {
        err = "mcp tools/list: "
            + resp["error"].value("message", std::string("unknown error"));
        return out;
    }
    if (!resp.contains("result") || !resp["result"].is_object()) {
        err = "mcp tools/list: missing result object";
        return out;
    }
    const auto & result = resp["result"];
    if (!result.contains("tools") || !result["tools"].is_array()) {
        err = "mcp tools/list: result.tools is not an array";
        return out;
    }

    out.reserve(result["tools"].size());
    for (const auto & t : result["tools"]) {
        if (!t.is_object()) continue;
        const std::string name = t.value("name",        std::string{});
        const std::string desc = t.value("description", std::string{});
        if (name.empty()) continue;

        // MCP `inputSchema` IS the JSON Schema we want — copy it
        // verbatim into the Tool. Falls back to "object with no
        // properties" so the engine's tool-call template still
        // renders something sensible.
        std::string params = "{\"type\":\"object\"}";
        if (t.contains("inputSchema") && t["inputSchema"].is_object()) {
            params = t["inputSchema"].dump();
        }

        Tool tool;
        tool.name            = name;
        tool.description     = desc;
        tool.parameters_json = std::move(params);
        tool.handler         = make_remote_handler(conn, name);
        out.push_back(std::move(tool));
    }
    return out;
}

#else  // !EASYAI_HAVE_CURL

std::vector<Tool> fetch_remote_tools(const ClientOptions & /*opts*/,
                                     std::string & err) {
    err = "mcp client unavailable: easyai built without libcurl";
    return {};
}

#endif

}  // namespace easyai::mcp
