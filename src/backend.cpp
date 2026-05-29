// libeasyai-side: LocalBackend impl (wraps easyai::Engine).
// RemoteBackend (wraps Client) lives in src/cli_client.cpp / libeasyai-cli.
#include "easyai/backend.hpp"

#include "easyai/builtin_tools.hpp"
#include "easyai/cli.hpp"
#include "easyai/engine.hpp"
#include "easyai/external_tools.hpp"
#include "easyai/preamble.hpp"
#include "easyai/presets.hpp"
#include "easyai/rag_tools.hpp"
#include "easyai/tool.hpp"

#include <cstdio>
#include <sstream>

namespace easyai {

// --------------------------------------------------------------- LocalBackend
struct LocalBackend::Impl {
    Config         cfg;
    Engine         engine;
    Tokenizer      cb;
    std::string    last_err;
};

LocalBackend::LocalBackend(Config c) : p_(std::make_unique<Impl>()) { p_->cfg = std::move(c); }
LocalBackend::~LocalBackend() = default;

bool LocalBackend::init(std::string & err) {
    auto & cfg    = p_->cfg;
    auto & engine = p_->engine;

    engine.model      (cfg.model_path)
          .context    (cfg.n_ctx)
          .gpu_layers (cfg.ngl)
          .system     (cfg.system_prompt)
          .verbose    (false)
          .on_token   ([this](const std::string & s){ if (p_->cb) p_->cb(s); });
    if (cfg.n_threads  > 0) engine.threads   (cfg.n_threads);
    if (cfg.n_batch    > 0) engine.batch     (cfg.n_batch);
    if (cfg.seed       > 0) engine.seed      (cfg.seed);
    if (cfg.max_tokens >= 0) engine.max_tokens(cfg.max_tokens);
    if (!cfg.cache_type_k.empty()) engine.cache_type_k(cfg.cache_type_k);
    if (!cfg.cache_type_v.empty()) engine.cache_type_v(cfg.cache_type_v);
    if (cfg.no_kv_offload) engine.no_kv_offload(true);
    if (cfg.kv_unified)    engine.kv_unified(true);
    for (const auto & ov : cfg.kv_overrides) engine.add_kv_override(ov);
    if (!cfg.spec_type.empty())          engine.spec_type(cfg.spec_type);
    if (cfg.spec_draft_n_max > 0)        engine.spec_draft_n_max(cfg.spec_draft_n_max);
    if (!cfg.spec_draft_model.empty())   engine.spec_draft_model(cfg.spec_draft_model);

    if (cfg.preset.name.empty()) {
        // Default to "precise" — tuned for code, math, factual Q&A, the
        // dominant use case for an embedded agent. Override via cfg.preset
        // before constructing the backend if you want looser sampling.
        if (const auto * p = find_preset("precise")) cfg.preset = *p;
    }
    engine.temperature(cfg.preset.temperature)
          .top_p      (cfg.preset.top_p)
          .top_k      (cfg.preset.top_k)
          .min_p      (cfg.preset.min_p);
    if (cfg.repeat_penalty > 0) engine.repeat_penalty(cfg.repeat_penalty);

    if (cfg.load_tools) {
        cli::Toolbelt()
            .sandbox     (cfg.sandbox)
            .allow_bash  (cfg.allow_bash)
            .allow_python(cfg.allow_python)
            .apply       (engine);
    }

    // RAG — the agent's persistent knowledge store (long-term memory).
    // Seven single-responsibility tools (knowledge_save, knowledge_append,
    // knowledge_search, knowledge_load, knowledge_list, knowledge_delete,
    // knowledge_keywords). The directory does NOT have to exist yet; the
    // tools create it on first save.
    if (!cfg.rag_dir.empty()) {
        for (auto & t : tools::knowledge_split_tools(cfg.rag_dir)) {
            engine.add_tool(std::move(t));
        }
    }

    // External tools directory. Loaded after the built-in toolbelt so
    // collisions with built-in names surface as a load-time error
    // instead of silently shadowing. Per-file fault isolation: a bad
    // file in the directory is logged and skipped — the agent still
    // starts. The operator sees the error in stderr/journal.
    //
    // Quiet mode (`cfg.quiet`) suppresses the security sanity-check
    // warnings (shell wrappers, dynamic-linker env passthrough,
    // world-writable binaries / manifests) so an interactive `-q`
    // CLI session isn't noisy. Errors are always emitted, regardless.
    if (!cfg.external_tools_dir.empty()) {
        std::vector<std::string> reserved;
        reserved.reserve(engine.tools().size());
        for (const auto & t : engine.tools()) reserved.push_back(t.name);
        auto loaded = load_external_tools_from_dir(
            cfg.external_tools_dir, reserved);

        for (const auto & e_msg : loaded.errors) {
            std::fprintf(stderr, "[external-tools] error: %s\n", e_msg.c_str());
        }
        if (!cfg.quiet) {
            for (const auto & w : loaded.warnings) {
                std::fprintf(stderr, "[external-tools] warning: %s\n", w.c_str());
            }
        }
        for (auto & t : loaded.tools) engine.add_tool(t);
    }

    // Caller-supplied tools — registered AFTER built-ins, memory, and
    // external tools so name collisions surface as obvious wins for
    // the operator's intent (their last-registered tool shadows the
    // earlier one).  Their `system_addendum` (if any) is collected
    // below and concatenated into the prompt.
    for (auto & t : cfg.extra_tools) engine.add_tool(t);

    // tool_lookup — registered last so its snapshot covers everything
    // above. Always on (no opt-out) when load_tools is enabled: the
    // model uses it as a guard before assuming a tool exists. The
    // getter re-reads engine.tools() at every call, so dynamic
    // additions during a session (uncommon for LocalBackend, but
    // possible) are still visible.
    if (cfg.load_tools) {
        engine.add_tool(tools::tool_lookup([&engine]() {
            tools::ToolCatalog v;
            v.reserve(engine.tools().size());
            for (const auto & t : engine.tools()) {
                v.push_back({ t.name, t.wire_description(), t.description });
            }
            return v;
        }));
    }

    engine.on_tool([](const ToolCall & c, const ToolResult & r){
        std::fprintf(stderr,
            "\n\033[32m● %s -> %s%.200s%s\033[0m\n",
            c.name.c_str(),
            r.is_error ? "ERR " : "",
            r.content.c_str(),
            r.content.size() > 200 ? "…" : "");
    });

    // Compose the final system prompt:
    //
    //   base (cfg.system_prompt)
    //     + concatenated Tool::system_addendum from every registered tool
    //     + cfg.system_appendix (caller-supplied static text)
    //     + AVAILABLE TOOLS + VERIFY-BEFORE-YOU-CALL block
    //
    // Done once at init (the registry doesn't change after this point),
    // so every LocalBackend caller — local one-shot, embedded REPL,
    // anything wiring up a LocalBackend directly — gets the same
    // composition without having to do it themselves. Mirrors the
    // per-request injection server.cpp does on the first user turn.
    //
    // Both addenda and the appendix are run through
    // `preamble::sanitize_addendum` before splicing — see
    // SECURITY_AUDIT §25.1.  Today the fields are operator-controlled,
    // but the sanitizer keeps the door closed against a future
    // external-tools / MCP plumbing where the source is less trusted,
    // and it also defends the operator's TTY from rogue ANSI when
    // `--show-system-prompt` is invoked.
    {
        // Per-tool addendum cap (8 KB) is now centralised in
        // `preamble::compose_system_prompt`; only the operator-static
        // appendix cap stays here.
        constexpr std::size_t kAppendixCap = 16 * 1024;
        std::string sys = preamble::compose_system_prompt(
            cfg.system_prompt, engine.tools());
        if (!cfg.system_appendix.empty()) {
            const std::string clean = preamble::sanitize_addendum(
                cfg.system_appendix, kAppendixCap);
            if (!clean.empty()) {
                if (!sys.empty() && sys.back() != '\n') sys += '\n';
                sys += '\n';
                sys += clean;
            }
        }
        if (cfg.load_tools && !engine.tools().empty()) {
            const std::string si = preamble::build_session_info(engine.tools());
            if (!si.empty()) sys += si;
        }
        if (sys != cfg.system_prompt) engine.system(sys);
    }

    if (!engine.load()) {
        err = engine.last_error();
        return false;
    }
    return true;
}

std::string LocalBackend::chat(const std::string & user_text, const Tokenizer & cb) {
    p_->cb = cb;
    try {
        return p_->engine.chat(user_text);
    } catch (const std::exception & e) {
        p_->last_err = std::string("local engine error: ") + e.what();
        return {};
    }
}

void LocalBackend::reset()                                                      { p_->engine.clear_history(); }
void LocalBackend::set_system(const std::string & t)                            { p_->engine.system(t); p_->engine.clear_history(); }
void LocalBackend::set_sampling(float t, float p, int k, float m)               { p_->engine.set_sampling(t, p, k, m); }

std::string LocalBackend::info() const {
    std::ostringstream o;
    o << "loaded "      << p_->cfg.model_path
      << "  backend="   << p_->engine.backend_summary()
      << "  ctx="       << p_->engine.n_ctx()
      << "  tools="     << p_->engine.tools().size();
    return o.str();
}

std::string LocalBackend::last_error() const {
    return p_->last_err.empty() ? p_->engine.last_error() : p_->last_err;
}

std::size_t LocalBackend::tool_count() const { return p_->engine.tools().size(); }

std::vector<std::pair<std::string,std::string>> LocalBackend::tool_list() const {
    std::vector<std::pair<std::string,std::string>> out;
    for (const auto & t : p_->engine.tools()) out.emplace_back(t.name, t.description);
    return out;
}

int LocalBackend::ctx_pct() const {
    auto pd = p_->engine.perf_data();
    int n_ctx = p_->engine.n_ctx();
    if (n_ctx <= 0 || pd.n_ctx_used < 0) return -1;
    long long n = (long long) pd.n_ctx_used * 100;
    int pct = (int) (n / n_ctx);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

bool LocalBackend::last_was_ctx_full() const {
    return p_->engine.last_was_ctx_full();
}

Engine *       LocalBackend::engine()       { return &p_->engine; }
const Engine * LocalBackend::engine() const { return &p_->engine; }

}  // namespace easyai
