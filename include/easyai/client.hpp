// easyai/client.hpp — OpenAI-protocol counterpart of Engine.
//
// Same fluent API and tool model as the local Engine, but the model
// itself runs on a remote server (any /v1/chat/completions endpoint:
// our easyai-server, an upstream llama-server, OpenAI itself, etc.).
// Tools execute LOCALLY in the consumer process — the model picks
// which tool to call, the Client dispatches it, and the result is
// fed back into the conversation.
//
// Intended use:
//
//   easyai::Client cli;
//   cli.endpoint("http://ai.local")
//      .api_key(getenv("EASYAI_KEY"))
//      .model("EasyAi")
//      .system("You are a planning agent.");
//   cli.add_tool(easyai::tools::search_web());
//   cli.add_tool(easyai::tools::fetch_web());
//   cli.on_token([](const std::string & p){ std::fputs(p.c_str(), stdout); });
//   std::string answer = cli.chat("summarise today's arxiv ml posts");
//
// The class is move-only (one outstanding HTTP transport per instance).
#pragma once

#include "easyai/tool.hpp"

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace easyai {

// Lightweight description of a remote model returned by /v1/models.
struct RemoteModel {
    std::string id;        // e.g. "EasyAi", "gpt-4o-mini"
    std::string owned_by;  // e.g. "easyai", "openai"
    long        created = 0;
};

// Lightweight description of a remote tool returned by /v1/tools
// (an easyai-server extension; not present on stock OpenAI).
//
// `short_description` is the one-line trigger string the server emits
// alongside the full body — present on easyai-servers built ≥ 2026-05.
// Older servers send only `description`; the parser falls back to
// `description` in that case so the caller never sees an empty field.
struct RemoteTool {
    std::string name;
    std::string description;
    std::string short_description;
};

class Client {
public:
    Client();
    ~Client();
    Client(const Client &)             = delete;
    Client & operator=(const Client &) = delete;
    Client(Client &&) noexcept;
    Client & operator=(Client &&) noexcept;

    // ----- transport / auth (fluent) ---------------------------------------
    Client & endpoint        (std::string url);             // http(s)://host[:port]
    Client & api_key         (std::string key);             // Bearer
    Client & timeout_seconds (int  s);                      // connect+read; default 86400 (24h)
    Client & verbose         (bool v);                      // log SSE lines to stderr

    // Per-batch easyai.prompt_progress SSE events (mirrors the
    // llama-server `prompt_progress` field). Default ON. Set false
    // to ask easyai-server to skip the per-batch events entirely —
    // request body carries `stream_options.easyai_prompt_progress
    // = false`, the server inspects it and doesn't wire the
    // callback. Trades the live spinner percentage for less SSE
    // wire chatter; the final `easyai.prompt_eval` summary still
    // fires either way. Older servers ignore the flag and keep
    // emitting; the client ignores the events on its side too.
    Client & send_prompt_progress (bool v);

    // Number of EXTRA attempts on transport failures (connect refused,
    // read timeout, 5xx with no streamed bytes yet). 0 disables retries
    // entirely; default is 5.  Each retry logs via easyai::log::error so
    // operators see it on stderr even without --verbose.  Mid-stream
    // failures are NEVER retried (would duplicate output / state).
    Client & http_retries    (int  n);

    // Tee EVERY HTTP transaction (request body + raw SSE chunks + tool
    // dispatch summaries) into the given FILE*.  The Client does NOT
    // take ownership — caller is responsible for fclose().  Pass nullptr
    // to disable.  Useful for offline analysis of "model produced no
    // tool_call" cases: the file contains exactly what crossed the wire
    // so you can grep for delta.tool_calls fragments, finish_reason,
    // timings.incomplete, etc.
    Client & log_file        (std::FILE * fp);

    // Hard cap on accumulated reasoning_content bytes for ONE turn.
    // When the running model's reasoning exceeds this threshold, the SSE
    // stream is aborted (cpp-httplib content_receiver returns false); the
    // current chat() call returns whatever text was streamed so far and
    // sets last_error to a descriptive "reasoning runaway" message.
    // Pass 0 to disable (default).  Useful with chatty thinking models
    // that occasionally fall into a long deliberation loop on niche
    // questions and you want a hard timeout in tokens, not seconds.
    Client & max_reasoning_chars (int n);

    // Auto-retry-with-nudge when the server flags a turn as incomplete
    // (no tool_call + tiny content — typically the "I'll search…"
    // then EOS pattern).  run_chat_loop discards the bad assistant
    // entry, appends a corrective user message ("don't announce,
    // execute"), and re-issues ONCE.  Bounded — no spirals, no nudge
    // stacking.  Default ON: every consumer of libeasyai-cli gets the
    // recovery for free.  Pass false to receive raw incomplete turns
    // transparently (still observable via last_turn_was_incomplete()).
    Client & retry_on_incomplete (bool v);

    // Whether the LAST turn returned by chat() / chat_continue() was
    // flagged incomplete by the server (timings.incomplete=true).
    // Use this to render a placeholder / surface a warning at the
    // app layer when retry_on_incomplete is off, or after the retry
    // budget was exhausted.
    bool last_turn_was_incomplete() const;

    // Live context-window usage (mirror of the server's
    // timings.{ctx_used,n_ctx} on the most recent turn).  Returns -1
    // until the first turn lands.  Used by the CLI spinner to render
    // a "%XX" load gauge next to its glyph so the operator can tell at
    // a glance how close the chat is to filling n_ctx.
    int  last_ctx_used () const;
    int  last_n_ctx    () const;
    // Convenience: last_ctx_used / last_n_ctx as a 0..100 percentage,
    // -1 when either side is unknown.
    int  last_ctx_pct  () const;

    // Generation stats for the most recent chat()/chat_continue() call,
    // summed across agentic hops (mirror of the server's
    // timings.{predicted_n,predicted_ms}; llama-server emits the same
    // fields).  -1 until the first turn lands.  predicted_n is the
    // completion-token count, predicted_ms the decode wall time —
    // divide for the average decode t/s the TUI footer badge shows.
    // last_turn_ms is the WHOLE call wall clock (prompt processing,
    // decode, tool dispatch, retries included).
    int    last_predicted_n () const;
    double last_predicted_ms() const;
    double last_turn_ms     () const;

    // Re-seed every last_* stat mirror above after a session resume, so
    // a freshly-constructed Client reports the stats of the conversation
    // it just load_history()'d as if the turn had run in this process.
    // Pass -1 for anything unknown.  clear_history() resets them all
    // EXCEPT last_n_ctx (the window size is a server property, not
    // conversation state) — the stats describe the conversation, not
    // the process.
    Client & restore_turn_stats(int ctx_used, int n_ctx, int predicted_n,
                                double predicted_ms, double turn_ms);

    // Hard stop when the chat context fills up.  After each agentic
    // hop, if the server-reported `timings.ctx_used / n_ctx` ratio is
    // >= this percentage, run_chat_loop aborts: it stops dispatching
    // further tool calls, sets last_error to a context-full message,
    // and returns the latest assistant content so the caller can
    // show the user what they got plus a note that the conversation
    // ran out of room.  Default 100 — pinned at the wall, no false
    // positives on chats that are merely large.  Pass 0 to disable
    // (legacy "burn until you OOM" behaviour).
    Client & stop_at_ctx_pct(int pct);

    // Whether the LAST turn was aborted because last_ctx_pct hit the
    // threshold above.  Lets the app layer pick a different banner
    // ("contexto cheio, recomece a conversa") rather than the
    // generic last_error string.
    bool last_was_ctx_full() const;

    // Agentic-loop safety cap: how many tool round-trips before chat()
    // bails out with "max tool hops exceeded".  Default 8 — fine for
    // small research flows.  Bash-driven flows often need many more
    // because a single shell session naturally spans many turns;
    // when --allow-bash is in play, callers bump this to ~99999.
    Client & max_tool_hops (int n);

    // ----- TLS (only meaningful for https:// endpoints) --------------------
    // tls_insecure(true) skips peer certificate verification — useful for
    // local dev with self-signed certs, NEVER for production.  ca_cert_path
    // points at a custom CA bundle (PEM) when the system store doesn't have
    // the issuer (corp CAs, internal microservices, etc.).  Both are no-ops
    // on http:// endpoints and on builds without OpenSSL support.
    Client & tls_insecure    (bool v);
    Client & ca_cert_path    (std::string path);

    // ----- request shape (fluent) ------------------------------------------
    // Every sampling/penalty knob below maps directly to the matching
    // OpenAI / llama-server / easyai-server field; -1.0f / -1 / "" are
    // "unset, server picks the default".  Multiple knobs can be pinned
    // at once and the request body only includes the ones you set.
    Client & model              (std::string id);            // request body field
    Client & system             (std::string prompt);        // 0..N system msgs
    Client & temperature        (float t);
    Client & top_p              (float v);
    Client & top_k              (int   v);
    Client & min_p              (float v);                   // llama-server / easyai
    Client & repeat_penalty     (float v);                   // llama-server / easyai
    Client & frequency_penalty  (float v);                   // OpenAI standard
    Client & presence_penalty   (float v);                   // OpenAI standard
    Client & seed               (long long s);               // -1 = randomise
    Client & max_tokens         (int   n);
    Client & stop               (std::vector<std::string> sequences);
    // Free-form passthrough for fields the public setters above don't cover.
    // The string MUST be a valid JSON object literal; its keys are merged
    // into the request body verbatim.  Useful for non-standard server
    // extensions (e.g. {"reasoning_effort":"high"}).
    Client & extra_body_json    (std::string raw_json);

    // Reasoning-effort level sent as the OpenAI-style `reasoning_effort`
    // request-body field ("low" / "medium" / "high" / model-specific).
    // easyai-server (and llama-server) feed it to the chat template so a
    // reasoning model adjusts how hard it thinks. The sentinels "auto",
    // "none", "default", "model", and "" mean USE THE MODEL DEFAULT — the
    // field is omitted from the body so the server/model decides. Default
    // is unset (model default).
    Client & reasoning_effort   (std::string level);

    // ----- tool registration (mirrors Engine) ------------------------------
    Client & add_tool        (Tool t);
    Client & clear_tools     ();
    const std::vector<Tool> & tools() const;

    // Resolve the system prompt that would be sent to the remote
    // server: whatever was set via `system(...)` PLUS every registered
    // tool's `effective_system_addendum()` (its `system_addendum` if
    // the tool set one, else its `description` as a fallback — see
    // `Tool::effective_system_addendum` in easyai/tool.hpp), composed
    // via the canonical `preamble::compose_system_prompt` helper so
    // the policy (8 KB per-tool cap, sanitization, blank-line
    // separator) stays in lockstep with Session / LocalBackend /
    // RemoteBackend. Pure: no I/O, no mutation, no network call.
    // The intended "request the final system" entry point for
    // binaries that want to dump or inspect what the model will
    // receive (e.g., `--show-system-prompt`).
    //
    // Caveat: if a backend (RemoteBackend) has already composed and
    // written back via `system(composed)`, calling this getter again
    // will RE-append addenda. Backends own the lifecycle; callers
    // outside that pipeline should read this BEFORE backend rebuild.
    std::string composed_system() const;

    // ----- streaming callbacks ---------------------------------------------
    using TokenCallback = std::function<void(const std::string &)>;
    using ToolCallback  = std::function<void(const ToolCall &, const ToolResult &)>;

    // Per-batch prompt-eval progress fired during the server's prompt
    // ingestion (before the first generated token).  Mirrors the
    // easyai.prompt_progress SSE event the server emits, which itself
    // mirrors llama-server's `prompt_progress` shape so any backend
    // speaking that contract works.  Args:
    //   processed — tokens decoded so far in this prompt-eval pass
    //   total     — total tokens that need decoding
    //   cached    — tokens already in KV cache (prefix that did NOT
    //                need decoding; reported for context only)
    //   ms        — wall time elapsed since prompt processing started
    // The cli's shimmer reads this to render a real "thinking N%"
    // gauge instead of an animated placeholder.
    using PromptProgressCallback = std::function<void(
        int processed, int total, int cached, double ms)>;

    using PromptEvalCallback = std::function<void(
        int n_tokens, int n_cached, double prompt_ms, double tps)>;

    Client & on_token           (TokenCallback);   // delta.content (visible reply)
    Client & on_reason          (TokenCallback);   // delta.reasoning_content (thinking)
    Client & on_tool            (ToolCallback);    // every dispatched tool round-trip
    Client & on_prompt_progress (PromptProgressCallback);  // per-batch prompt-eval %
    Client & on_prompt_eval     (PromptEvalCallback);      // prompt-eval summary

    // ----- chat ------------------------------------------------------------
    // chat() pushes the user message, runs the agentic multi-hop loop
    // until the model emits a non-tool finish_reason, and returns the
    // final visible content.  chat_continue() is the same minus the
    // user push (for callers that want to inject tool results manually).
    std::string chat          (const std::string & user_message);
    std::string chat_continue ();
    void        clear_history ();

    // ----- session persistence ---------------------------------------------
    // dump_history() returns the user / assistant / tool messages in the
    // OpenAI on-wire shape, as a JSON array string.  The system prompt is
    // NOT included — it lives on the Client as config and is re-attached
    // on every request.  Empty history returns "[]".  Intended for
    // serialising to disk so a later process can resume the conversation
    // via load_history().
    //
    // load_history(json_array) replaces the existing message history
    // with the messages parsed from `json_array`.  Returns false on parse
    // failure or schema mismatch (must be an array of objects, each with
    // a "role" string); on success the new history is in effect for the
    // next chat() / chat_continue() call.  Pass `err` to capture a
    // diagnostic.
    std::string dump_history() const;
    bool        load_history(const std::string & json_array,
                             std::string * err = nullptr);

    // ----- direct endpoints (optional helpers) -----------------------------
    // Each method returns false on transport / HTTP failure; on success
    // the parsed value is written to the out parameter.  See last_error()
    // for diagnostic detail.
    bool list_models      (std::vector<RemoteModel> & out);
    bool list_remote_tools(std::vector<RemoteTool>  & out);   // /v1/tools
    bool health           ();                                  // GET /health
    bool metrics          (std::string & out_text);            // Prometheus
    bool props            (std::string & out_json);            // raw JSON
    bool set_preset       (const std::string & preset_name);   // /v1/preset

    // ----- introspection ---------------------------------------------------
    std::string last_error() const;

    // ----- cooperative cancel ----------------------------------------------
    // Thread-safe: call request_cancel() from a signal handler or any
    // other thread to abort an in-flight chat()/chat_continue(). The SSE
    // content receiver polls the flag every chunk and aborts the read by
    // returning false to cpp-httplib, which closes the TCP connection.
    // The remote server detects the dropped socket on its next write and
    // cancels its own decode loop — no /cancel endpoint, no protocol
    // change, OpenAI-compatible.
    //
    // The flag is sticky: once set, the current and any subsequent chat()
    // call short-circuits until clear_cancel() is invoked. Call
    // clear_cancel() between turns of an interactive REPL.
    Client & request_cancel();
    Client & clear_cancel();
    bool     cancel_requested() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace easyai
