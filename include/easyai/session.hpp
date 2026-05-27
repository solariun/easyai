// easyai/session.hpp — single-call agent setup, OpenAI-Python-SDK style.
//
// Why this exists
// ---------------
// The lib already has every building block: Engine (local llama.cpp),
// Client (remote OpenAI-protocol), LocalBackend / RemoteBackend (uniform
// chat interface), cli::Toolbelt (canonical tool registration),
// preamble::build_builtin_system_prompt (the unified system prompt).
//
// What was missing — and what Session adds — is the one-call
// orchestration that ties them together.  Before Session, every binary
// (cli, server, third-party agent) wrote the same 50 lines of "build
// the toolset view from my args, register the canonical tools, compose
// the system prompt, attach tool addenda, hand off to the backend".
// Now they don't.
//
// The shape mirrors the OpenAI Python SDK so anyone with that mental
// model can pick it up cold:
//
//   easyai::Session s = easyai::Session::remote("http://localhost:8080");
//   s.with_default_tools()
//    .sandbox("/srv/work").allow_bash()
//    .system_append("You are the Acme support bot.")
//    .add_tool(my_custom_tool);
//   std::string err;
//   s.init(err);
//   std::string reply = s.chat("hello");
//
// What lives where
// ----------------
// * AI connection (Engine, Client) and the built-in tools live in the
//   lib — Session is a thin composer on top.
// * The cli / server binaries keep ONLY their UI surface (REPL, HTTP
//   routes, SSE, signal handling, web UI).  All "wire the agent" work
//   delegates to Session.
//
// Custom system prompts
// ---------------------
// The lib's default system prompt (preamble::build_builtin_system_prompt)
// is opinionated and good.  Replace it entirely with .system("...") OR
// turn the default off with .no_builtin_system() and provide your own
// via .system_append("...").  The two static rules:
//   1. Base = .system(text) if set, else built-in default unless
//      .no_builtin_system() is set, else empty.
//   2. Appended after the base, in this order: every registered tool's
//      `Tool::system_addendum`, every .system_append(text) entry, the
//      datetime / knowledge-cutoff / memory-vocabulary block
//      (preamble::build), and the AVAILABLE-TOOLS catalogue (local
//      mode only — remote servers render their own catalogue).
//
// One library
// -----------
// easyai ships as a single unified library (libeasyai).  Consumers
// `target_link_libraries(... easyai)` and `#include "easyai/session.hpp"`.
// Engine, Client, every built-in tool, the preamble composer, the
// Backend abstraction, and Session all live in the same library —
// there is no "engine vs. cli" split to manage at link time.
#pragma once

#include "backend.hpp"
#include "cli.hpp"        // ToolMode
#include "preamble.hpp"   // preamble::Options
#include "presets.hpp"
#include "tool.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace easyai {

class Engine;   // libeasyai
class Client;   // libeasyai-cli

class Session {
public:
    enum class Mode { Local, Remote };

    using TokenCallback = std::function<void(const std::string &)>;

    // ---------------- factories ----------------------------------------
    // Local-only: take a fully-populated LocalBackend::Config (the
    // caller has already chosen model_path, n_ctx, …).  Sampling /
    // preset / sandbox knobs can be set fluently AFTER construction —
    // they override whatever the Config carried.
    static Session local(LocalBackend::Config cfg);

    // Convenience: just a model path + sensible defaults.  Equivalent
    // to local({.model_path = path}) and then setting sandbox /
    // memory / preset via fluent setters.
    static Session local(std::string model_path);

    // Remote: base URL only required.  Defaults model="easyai".  Use
    // .api_key, .model, .tls_insecure etc. for the rest.
    static Session remote(std::string base_url, std::string model = "easyai");

    // ---------------- system prompt ------------------------------------
    // Replace the base system prompt.  When set (non-empty), the lib's
    // built-in default is NOT used; the caller is fully in charge of
    // the base.  Tool addenda + .system_append still apply on top.
    Session & system            (std::string base);
    // Don't use the lib's default base prompt.  Combine with
    // .system_append(...) to author your own from scratch.
    Session & no_builtin_system (bool on = true);
    // Append a static block to the system prompt.  Multiple calls
    // concatenate in call order.  Each block is rendered with a blank
    // line of separation.
    Session & system_append     (std::string addendum);
    // Append a dynamic block.  The function is called each time
    // refresh_system() runs (which happens at init() and on demand).
    // Returning "" emits nothing.  Useful when the addendum depends
    // on per-session state that may change.
    Session & system_append     (std::function<std::string()> dynamic);

    // Override the per-turn preamble::Options (date/time toggle,
    // knowledge cutoff string, memory root for the vocabulary
    // snapshot, cite_sources flag).  When .memory(<dir>) is called,
    // memory_root is auto-populated unless the caller has already
    // pinned it via this method.
    Session & preamble_options  (preamble::Options opt);

    // ---------------- tools --------------------------------------------
    // Turn on the canonical agent toolset: datetime + web + tool_lookup
    // baseline, plus the per-flag adds below (sandbox → fs, allow_bash
    // → bash, allow_python → python3).  On by default — call with
    // .with_default_tools(false) for a bare session.
    Session & with_default_tools(bool on = true);
    Session & sandbox           (std::string dir);
    Session & allow_bash        (bool on = true);
    Session & allow_python      (bool on = true);
    Session & show_bash         (bool on = true);
    Session & show_python       (bool on = true);
    Session & no_web            (bool on = true);
    Session & no_datetime       (bool on = true);
    Session & use_google        (bool on = true);
    Session & tool_mode         (cli::ToolMode m);

    // Persistent memory store directory.  Empty = off.  When set, the
    // memory_split_tools() bundle is registered AND the memory-
    // vocabulary block is appended to the system per turn.
    Session & memory            (std::string dir);
    // Directory of EASYAI-*.tools manifests.  Errors are logged to
    // stderr; per-file fault isolation (one bad file doesn't sink the
    // session).
    Session & external_tools    (std::string dir);
    // Add a custom tool.  Its Tool::system_addendum is collected and
    // emitted in the system prompt.
    Session & add_tool          (Tool t);

    // ---------------- sampling / runtime -------------------------------
    Session & preset            (Preset p);
    Session & preset            (const std::string & name);   // looked up
    Session & temperature       (float t);
    Session & top_p             (float v);
    Session & top_k             (int   v);
    Session & min_p             (float v);
    Session & repeat_penalty    (float v);
    Session & max_tokens        (int   n);
    Session & seed              (long long s);

    // ---------------- transport (remote only — no-op for Local) --------
    Session & api_key           (std::string key);
    Session & model             (std::string id);
    Session & timeout_seconds   (int s);
    Session & tls_insecure      (bool on = true);
    Session & ca_cert_path      (std::string path);

    // ---------------- engine knobs (local only — no-op for Remote) -----
    Session & context           (int n_ctx);
    Session & gpu_layers        (int n);
    Session & threads           (int n);
    Session & batch             (int n);

    // ---------------- callbacks ----------------------------------------
    Session & on_token          (TokenCallback cb);

    // ---------------- lifecycle ----------------------------------------
    bool   init                 (std::string & err);
    void   reset                ();          // wipe history, keep system
    // Rebuild and push the current system prompt — call this after
    // .system_append(...) mid-session so the next chat() sees the
    // change.  Implicitly called by init().
    void   refresh_system       ();
    // Mid-session: replace the BASE system prompt with `text` and push
    // it to the backend immediately (history is cleared by the
    // backend, matching Backend::set_system).  Equivalent to
    // .system(text) then .refresh_system() — provided as a single
    // call because the REPL "/system <text>" pattern is common.
    void   set_system           (std::string text);

    // ---------------- chat ---------------------------------------------
    std::string chat            (const std::string & user);

    // ---------------- introspection ------------------------------------
    Mode               mode         () const;
    std::string        render_system() const;   // resolved prompt text
    std::vector<Tool>  tools        () const;
    std::string        last_error   () const;
    Backend *          backend      () const;
    // Direct access to the underlying engine / client for callers that
    // need raw streaming knobs or the perf counters.  Returns nullptr
    // for the other mode.
    Engine *           engine_ptr   () const;
    Client *           client_ptr   () const;

    // ---------------- runtime knobs (Backend parity, post-init) --------
    // Live context-window load: 0..100 percentage of n_ctx currently
    // occupied, or -1 if the backend doesn't have a number yet
    // (LocalBackend before first chat; RemoteBackend before the first
    // SSE timings).  Same contract as Backend::ctx_pct().
    int                ctx_pct           () const;
    // True when the LAST chat() ran out of context window and the
    // agentic loop bailed early.  Same contract as
    // Backend::last_was_ctx_full().
    bool               last_was_ctx_full () const;
    // Human-readable one-liner: "loaded <model> backend=Metal ctx=4096
    // tools=N" for local; "remote <url> (model=<id>) [auth]" for
    // remote.  Empty before init().
    std::string        info              () const;
    // (name, description) pairs — cheaper than .tools() when the
    // caller only needs to render a /tools listing.
    std::vector<std::pair<std::string, std::string>> tool_list() const;
    std::size_t        tool_count        () const;
    // Live sampling reconfig — equivalent to Backend::set_sampling.
    // Pass -1 to keep a field unchanged.
    void               set_sampling      (float temperature,
                                          float top_p,
                                          int   top_k,
                                          float min_p);

    Session(Session &&) noexcept;
    Session & operator=(Session &&) noexcept;
    Session(const Session &)             = delete;
    Session & operator=(const Session &) = delete;
    ~Session();

private:
    Session();
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace easyai
