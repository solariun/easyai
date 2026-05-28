// easyai/backend.hpp — common interface for local-engine vs. remote-HTTP
// agents, plus LocalBackend (libeasyai) and RemoteBackend (libeasyai-cli)
// implementations.
//
// Why this exists: every CLI / agent we ship has the same shape — accept
// `--model PATH` OR `--url BASE`, build the right kind of engine,
// register the same toolset, drive a streaming chat loop.  Backend
// hides which side of that fork you ended up on so the rest of your
// program can stay agnostic.
//
// Layering: the interface + LocalBackend live in libeasyai; RemoteBackend
// lives in libeasyai-cli.  Linking only libeasyai gives you the local
// flavour; linking libeasyai-cli on top adds the remote flavour without
// duplicating the abstract base.
#pragma once

#include "presets.hpp"
#include "tool.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace easyai {

class Engine;   // libeasyai
class Client;   // libeasyai-cli

// ----------------------------------------------------------------------
// Backend — abstract interface
// ----------------------------------------------------------------------
class Backend {
public:
    using Tokenizer = std::function<void(const std::string &)>;

    virtual ~Backend() = default;

    virtual bool        init       (std::string & err)                                  = 0;
    virtual std::string chat       (const std::string & user_text,
                                    const Tokenizer & cb)                               = 0;
    virtual void        reset      ()                                                   = 0;
    virtual void        set_system (const std::string & text)                           = 0;
    virtual void        set_sampling(float temp, float top_p,
                                     int top_k, float min_p)                            = 0;
    virtual std::string info       () const                                             = 0;
    virtual std::string last_error () const                                             = 0;
    virtual std::size_t tool_count () const                                             = 0;
    virtual std::vector<std::pair<std::string,std::string>> tool_list() const           = 0;

    // Live context-window load: 0..100 percentage of n_ctx currently
    // occupied, or -1 if the backend doesn't have a number yet (Local
    // before first chat; Remote before the first SSE timings).
    virtual int  ctx_pct        () const { return -1; }
    // True when the LAST chat() ran out of context window and the
    // agentic loop bailed early.  Default false — overrides in
    // LocalBackend / RemoteBackend mirror Engine::last_was_ctx_full /
    // Client::last_was_ctx_full.
    virtual bool last_was_ctx_full() const { return false; }
};


// ----------------------------------------------------------------------
// LocalBackend — wraps easyai::Engine.  Lives in libeasyai.
// ----------------------------------------------------------------------
class LocalBackend final : public Backend {
public:
    struct Config {
        std::string model_path;
        std::string system_prompt;
        std::string sandbox;            // empty = fs_* tools NOT registered
        bool        allow_bash = false; // explicit opt-in for the bash tool
        // python3 defaults ON: the unified `python3` tool ships a Python
        // preamble that auto-restricts disk access (open / os.open /
        // io.open) to the sandbox root. Auto-registers whenever sandbox
        // is set OR allow_bash is on — same gating as `fs`. Operators
        // who want to disable it explicitly can flip this to false.
        bool        allow_python = true;
        std::string external_tools_dir; // optional external-tools dir (EASYAI-*.tools)
        bool        quiet           = false; // suppress sanity warnings (errors still emitted)
        std::string rag_dir;            // optional RAG (long-term registry) dir; empty = RAG off
        int         n_ctx      = 4096;
        int         n_batch    = 0;     // 0 = follow ctx
        int         ngl        = -1;
        int         n_threads  = 0;
        bool        load_tools = true;
        Preset      preset{};
        // Sampling overrides (applied after preset).  -1 / 0 means
        // "unset" for fields where that's a sentinel.
        float       repeat_penalty = 1.0f;
        int         max_tokens     = -1;
        std::uint32_t seed         = 0u;
        // KV cache & GGUF-metadata overrides
        std::string cache_type_k;
        std::string cache_type_v;
        bool        no_kv_offload  = false;
        bool        kv_unified     = false;
        std::vector<std::string> kv_overrides;
        // Speculative decoding
        std::string spec_type;
        std::string spec_draft_model;
        int         spec_draft_n_max = 0;

        // Caller-supplied tools, registered AFTER the built-in toolbelt
        // (and AFTER memory / external tools) but BEFORE tool_lookup so
        // they show up in tool_lookup's catalogue.  Each tool's optional
        // `system_addendum` is appended to the system prompt at init().
        std::vector<Tool> extra_tools;
        // Static text appended to the system prompt AFTER the built-in
        // preamble + tool addenda, BEFORE the AVAILABLE-TOOLS session
        // info block.  Use for per-app additions like "You are the
        // Acme support bot." that should NOT replace the lib's default.
        std::string       system_appendix;
    };

    explicit LocalBackend(Config c);
    ~LocalBackend() override;

    LocalBackend(const LocalBackend &)             = delete;
    LocalBackend & operator=(const LocalBackend &) = delete;

    bool        init       (std::string & err) override;
    std::string chat       (const std::string & user_text,
                            const Tokenizer & cb) override;
    void        reset      () override;
    void        set_system (const std::string & text) override;
    void        set_sampling(float temp, float top_p,
                             int top_k, float min_p) override;
    std::string info       () const override;
    std::string last_error () const override;
    std::size_t tool_count () const override;
    std::vector<std::pair<std::string,std::string>> tool_list() const override;
    int         ctx_pct        () const override;
    bool        last_was_ctx_full() const override;

    // Direct access to the wrapped Engine for callers that need the
    // streaming knobs / perf counters / chat_params introspection that
    // Backend doesn't expose.  Returns nullptr if init() hasn't run.
    Engine *       engine();
    const Engine * engine() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};


// ----------------------------------------------------------------------
// RemoteBackend — wraps easyai::Client.  Lives in libeasyai-cli.
// ----------------------------------------------------------------------
class RemoteBackend final : public Backend {
public:
    struct Config {
        std::string base_url;       // "http://127.0.0.1:8080" or "/v1"
        std::string api_key;        // optional Bearer token
        std::string model = "easyai";
        std::string system_prompt;
        std::string sandbox;        // empty = fs_* NOT registered (even with_tools)
        bool        allow_bash = false;
        Preset      preset{};
        long        timeout_seconds = 300;
        int         max_tokens      = -1;
        long long   seed            = -1;
        bool        with_tools      = false;  // register builtin tools on Client
        bool        tls_insecure    = false;
        std::string ca_cert_path;

        // Caller-supplied tools, registered AFTER the built-in toolbelt.
        // Each tool's optional `system_addendum` is appended to the
        // system prompt at init() / rebuild() time.
        std::vector<Tool> extra_tools;
        // Static text appended to the system prompt AFTER the lib's
        // default content.  Same semantics as
        // LocalBackend::Config::system_appendix.
        std::string       system_appendix;
    };

    explicit RemoteBackend(Config c);
    ~RemoteBackend() override;

    RemoteBackend(const RemoteBackend &)             = delete;
    RemoteBackend & operator=(const RemoteBackend &) = delete;

    bool        init       (std::string & err) override;
    std::string chat       (const std::string & user_text,
                            const Tokenizer & cb) override;
    void        reset      () override;
    void        set_system (const std::string & text) override;
    void        set_sampling(float temp, float top_p,
                             int top_k, float min_p) override;
    std::string info       () const override;
    std::string last_error () const override;
    std::size_t tool_count () const override;
    std::vector<std::pair<std::string,std::string>> tool_list() const override;
    int         ctx_pct        () const override;
    bool        last_was_ctx_full() const override;

    // Direct access to the wrapped Client.  Returns nullptr only if
    // the impl hasn't been built (init() always builds it).
    Client *       client();
    const Client * client() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace easyai
