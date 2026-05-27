// src/session.cpp — easyai::Session implementation.
//
// Session owns one Backend (LocalBackend or RemoteBackend) and exposes
// the OpenAI-Python-SDK-shaped fluent surface declared in
// easyai/session.hpp.  All the "build the agent" orchestration that
// used to live duplicated in examples/cli.cpp, examples/server.cpp,
// and examples/local.cpp is centralised here so a third-party agent
// gets the same defaults for free.
//
// Lives in the unified libeasyai library alongside Engine, Client,
// the built-in tools, and the preamble composer — one link target,
// one include path.
#include "easyai/session.hpp"

#include "easyai/client.hpp"
#include "easyai/engine.hpp"
#include "easyai/preamble.hpp"
#include "easyai/presets.hpp"

#include <utility>

namespace easyai {

// --------------------------------------------------------------------- Impl
struct Session::Impl {
    Mode                                       mode = Mode::Local;
    std::unique_ptr<Backend>                   backend;

    // Mirrors of the underlying Backend Configs so fluent setters can
    // update values BEFORE init().  After init() we still keep them
    // around for refresh_system() and for any post-init mutation
    // (set_sampling etc. — those go through the Backend directly).
    LocalBackend::Config                       local_cfg;
    RemoteBackend::Config                      remote_cfg;

    // System composition state.
    std::string                                system_base;
    bool                                       use_builtin_system  = true;
    bool                                       has_explicit_base   = false;
    std::vector<std::string>                   system_static;
    std::vector<std::function<std::string()>>  system_dynamic;
    preamble::Options                          preamble_opts;
    bool                                       preamble_opts_user_set = false;

    // Toolbelt configuration — captured here instead of on the
    // LocalBackend::Config so REMOTE sessions get the same fluent
    // surface (RemoteBackend::Config has fewer fields; we route them
    // through the lib's cli::Toolbelt at init time).
    std::string                                sandbox_dir;
    bool                                       with_defaults    = true;
    bool                                       allow_bash       = false;
    bool                                       allow_python     = true;
    bool                                       show_bash        = false;
    bool                                       show_python      = false;
    bool                                       no_web           = false;
    bool                                       no_datetime      = false;
    bool                                       use_google       = false;
    cli::ToolMode                              tool_mode        = cli::ToolMode::Split;

    std::string                                memory_dir;
    std::string                                external_dir;

    std::vector<Tool>                          extra_tools;

    TokenCallback                              token_cb;

    std::string                                last_err;

    // Compose the system prompt from the current state.  The same
    // algorithm runs at init() (where it seeds the Backend) and at
    // refresh_system() (where it re-pushes the result to the backend).
    std::string compose_system(const std::vector<Tool> & registered_tools) {
        std::string out;
        if (has_explicit_base) {
            out = system_base;
        } else if (use_builtin_system) {
            preamble::ToolsetView view;
            view.datetime_on    = !no_datetime;
            view.web_on         = !no_web;
            // Mirrors cli::Toolbelt::tools(): fs is on whenever the
            // operator gave us a sandbox OR a subprocess executor.
            view.fs_on          = !sandbox_dir.empty() || allow_bash;
            view.bash_on        = allow_bash;
            view.python_on      = allow_python && view.fs_on;
            view.memory_on      = !memory_dir.empty();
            view.tool_lookup_on = with_defaults;
            // Source of truth: the actual registry once we have it.
            view.active_tools   = registered_tools;
            out = preamble::build_builtin_system_prompt(view);
        }
        return out;
    }

    // Dynamic addenda — datetime, knowledge cutoff, memory vocab.
    // Rendered fresh every refresh_system() so the value tracks the
    // live state of the memory store.
    std::string compose_dynamic() {
        preamble::Options opt = preamble_opts;
        if (!preamble_opts_user_set) {
            opt.inject_datetime  = !no_datetime;
            opt.cite_sources     = use_builtin_system && !has_explicit_base;
            opt.has_memory       = !memory_dir.empty();
            opt.memory_root      = memory_dir;
        }
        return preamble::build(opt);
    }

    void rebuild_local_cfg_tools() {
        // Project Session-level knobs into LocalBackend::Config so the
        // backend's existing toolbelt path stays in charge.
        local_cfg.sandbox            = sandbox_dir;
        local_cfg.allow_bash         = allow_bash;
        local_cfg.allow_python       = allow_python;
        local_cfg.external_tools_dir = external_dir;
        local_cfg.rag_dir            = memory_dir;
        local_cfg.load_tools         = with_defaults;
        local_cfg.extra_tools        = extra_tools;
    }

    void rebuild_remote_cfg_tools() {
        remote_cfg.sandbox     = sandbox_dir;
        remote_cfg.allow_bash  = allow_bash;
        remote_cfg.with_tools  = with_defaults;
        remote_cfg.extra_tools = extra_tools;
    }

    // Compose the whole system prompt — base + tool addenda + static /
    // dynamic appends + dynamic preamble + (local-only) catalogue tail.
    // Used by refresh_system / set_system / render_system.  Tool addenda
    // and operator appends are sanitized at concat (SECURITY_AUDIT §25.1).
    std::string full_compose(const std::vector<Tool> & registered_tools,
                             bool include_session_info_tail);
};

// ----------------------------------------------------------- ctors / factories
Session::Session() : p_(std::make_unique<Impl>()) {
    // Default preset matches LocalBackend / RemoteBackend: "precise".
    if (const auto * pr = find_preset("precise")) {
        p_->local_cfg.preset  = *pr;
        p_->remote_cfg.preset = *pr;
    }
}

Session::Session(Session &&) noexcept            = default;
Session & Session::operator=(Session &&) noexcept = default;
Session::~Session()                              = default;

Session Session::local(LocalBackend::Config cfg) {
    Session s;
    s.p_->mode      = Mode::Local;
    s.p_->local_cfg = std::move(cfg);
    // Mirror caller-supplied sandbox / executor flags so fluent
    // setters / system composition see the intent.
    s.p_->sandbox_dir   = s.p_->local_cfg.sandbox;
    s.p_->allow_bash    = s.p_->local_cfg.allow_bash;
    s.p_->allow_python  = s.p_->local_cfg.allow_python;
    s.p_->memory_dir    = s.p_->local_cfg.rag_dir;
    s.p_->external_dir  = s.p_->local_cfg.external_tools_dir;
    s.p_->with_defaults = s.p_->local_cfg.load_tools;
    if (!s.p_->local_cfg.system_prompt.empty()) {
        s.p_->system_base       = s.p_->local_cfg.system_prompt;
        s.p_->has_explicit_base = true;
    }
    return s;
}

Session Session::local(std::string model_path) {
    LocalBackend::Config cfg;
    cfg.model_path = std::move(model_path);
    return Session::local(std::move(cfg));
}

Session Session::remote(std::string base_url, std::string model_id) {
    Session s;
    s.p_->mode             = Mode::Remote;
    s.p_->remote_cfg.base_url = std::move(base_url);
    s.p_->remote_cfg.model    = std::move(model_id);
    return s;
}

// ----------------------------------------------------------------- system
Session & Session::system(std::string base) {
    p_->system_base       = std::move(base);
    p_->has_explicit_base = true;
    return *this;
}

Session & Session::no_builtin_system(bool on) {
    p_->use_builtin_system = !on;
    return *this;
}

Session & Session::system_append(std::string addendum) {
    if (!addendum.empty()) p_->system_static.push_back(std::move(addendum));
    return *this;
}

Session & Session::system_append(std::function<std::string()> dynamic) {
    if (dynamic) p_->system_dynamic.push_back(std::move(dynamic));
    return *this;
}

Session & Session::preamble_options(preamble::Options opt) {
    p_->preamble_opts          = std::move(opt);
    p_->preamble_opts_user_set = true;
    return *this;
}

// ----------------------------------------------------------------- tools
Session & Session::with_default_tools(bool on)    { p_->with_defaults = on; return *this; }
Session & Session::sandbox        (std::string d) { p_->sandbox_dir   = std::move(d); return *this; }
Session & Session::allow_bash     (bool on)       { p_->allow_bash    = on; return *this; }
Session & Session::allow_python   (bool on)       { p_->allow_python  = on; return *this; }
Session & Session::show_bash      (bool on)       { p_->show_bash     = on; return *this; }
Session & Session::show_python    (bool on)       { p_->show_python   = on; return *this; }
Session & Session::no_web         (bool on)       { p_->no_web        = on; return *this; }
Session & Session::no_datetime    (bool on)       { p_->no_datetime   = on; return *this; }
Session & Session::use_google     (bool on)       { p_->use_google    = on; return *this; }
Session & Session::tool_mode      (cli::ToolMode m){p_->tool_mode     = m;  return *this; }
Session & Session::memory         (std::string d) { p_->memory_dir    = std::move(d); return *this; }
Session & Session::external_tools (std::string d) { p_->external_dir  = std::move(d); return *this; }
Session & Session::add_tool(Tool t) {
    if (p_->backend) {
        // Post-init: register on the live Engine / Client AND refresh
        // the system prompt so the tool's `system_addendum` and the
        // updated AVAILABLE-TOOLS catalogue actually reach the model.
        // Without this, calling add_tool after init() was a silent
        // no-op — see SECURITY_AUDIT §25.3.  History is preserved.
        if (auto * e = engine_ptr())      e->add_tool(t);
        else if (auto * c = client_ptr()) c->add_tool(t);
        p_->extra_tools.push_back(std::move(t));
        refresh_system();
    } else {
        // Pre-init: queue for the backend to pick up via
        // Config::extra_tools when init() runs.
        p_->extra_tools.push_back(std::move(t));
    }
    return *this;
}

// --------------------------------------------------------------- sampling
Session & Session::preset(Preset p) {
    p_->local_cfg.preset  = p;
    p_->remote_cfg.preset = std::move(p);
    return *this;
}
Session & Session::preset(const std::string & name) {
    if (const auto * pr = find_preset(name)) {
        p_->local_cfg.preset  = *pr;
        p_->remote_cfg.preset = *pr;
    }
    return *this;
}
Session & Session::temperature(float t) {
    p_->local_cfg.preset.temperature  = t;
    p_->remote_cfg.preset.temperature = t;
    return *this;
}
Session & Session::top_p(float v) {
    p_->local_cfg.preset.top_p  = v;
    p_->remote_cfg.preset.top_p = v;
    return *this;
}
Session & Session::top_k(int v) {
    p_->local_cfg.preset.top_k  = v;
    p_->remote_cfg.preset.top_k = v;
    return *this;
}
Session & Session::min_p(float v) {
    p_->local_cfg.preset.min_p  = v;
    p_->remote_cfg.preset.min_p = v;
    return *this;
}
Session & Session::repeat_penalty(float v) {
    p_->local_cfg.repeat_penalty = v;
    return *this;  // Remote: no equivalent on the existing Config; OpenAI shape.
}
Session & Session::max_tokens(int n) {
    p_->local_cfg.max_tokens  = n;
    p_->remote_cfg.max_tokens = n;
    return *this;
}
Session & Session::seed(long long s) {
    p_->local_cfg.seed  = static_cast<std::uint32_t>(s < 0 ? 0 : s);
    p_->remote_cfg.seed = s;
    return *this;
}

// --------------------------------------------------------------- transport
Session & Session::api_key        (std::string k) { p_->remote_cfg.api_key      = std::move(k); return *this; }
Session & Session::model          (std::string m) { p_->remote_cfg.model        = std::move(m); return *this; }
Session & Session::timeout_seconds(int s)         { p_->remote_cfg.timeout_seconds = s;         return *this; }
Session & Session::tls_insecure   (bool on)       { p_->remote_cfg.tls_insecure = on;           return *this; }
Session & Session::ca_cert_path   (std::string p) { p_->remote_cfg.ca_cert_path = std::move(p); return *this; }

// ----------------------------------------------------------------- engine
Session & Session::context        (int n) { p_->local_cfg.n_ctx     = n; return *this; }
Session & Session::gpu_layers     (int n) { p_->local_cfg.ngl       = n; return *this; }
Session & Session::threads        (int n) { p_->local_cfg.n_threads = n; return *this; }
Session & Session::batch          (int n) { p_->local_cfg.n_batch   = n; return *this; }

Session & Session::on_token(TokenCallback cb) { p_->token_cb = std::move(cb); return *this; }

// --------------------------------------------------------------- lifecycle
bool Session::init(std::string & err) {
    // BASE system prompt — built from the operator's intent.  At this
    // point the backend hasn't been built yet, so we can't pass a
    // populated `active_tools` view; backend->init() appends the real
    // catalogue tail (local mode) via preamble::build_session_info.
    const std::string base = p_->compose_system({});

    // Operator's .system_append() blocks + the first dynamic preamble
    // snapshot are concatenated into system_appendix so the Backend's
    // own composer (which also runs the §25.1 sanitizer) splices them
    // verbatim after the tool addenda.  Sanitization happens at the
    // Backend boundary — see SECURITY_AUDIT §25.1.
    std::string static_appends;
    for (const auto & s : p_->system_static) {
        if (s.empty()) continue;
        if (!static_appends.empty()) static_appends += "\n\n";
        static_appends += s;
    }
    for (auto & fn : p_->system_dynamic) {
        std::string s = fn ? fn() : std::string();
        if (s.empty()) continue;
        if (!static_appends.empty()) static_appends += "\n\n";
        static_appends += s;
    }
    const std::string dyn = p_->compose_dynamic();
    if (!dyn.empty()) {
        if (!static_appends.empty()) static_appends += "\n\n";
        static_appends += dyn;
    }

    if (p_->mode == Mode::Local) {
        p_->rebuild_local_cfg_tools();
        p_->local_cfg.system_prompt   = base;
        p_->local_cfg.system_appendix = static_appends;
        p_->backend = std::make_unique<LocalBackend>(std::move(p_->local_cfg));
    } else {
        p_->rebuild_remote_cfg_tools();
        p_->remote_cfg.system_prompt   = base;
        p_->remote_cfg.system_appendix = static_appends;
        p_->backend = std::make_unique<RemoteBackend>(std::move(p_->remote_cfg));
    }

    if (!p_->backend->init(err)) {
        p_->last_err = err;
        return false;
    }
    return true;
}

namespace {
// Per-tool addendum cap (8 KB) lives inside
// `preamble::compose_system_prompt`. The appendix cap here covers
// operator-supplied `system_static` / `system_dynamic` blocks that
// land AFTER the tool addenda — see SECURITY_AUDIT §25.1.
constexpr std::size_t kAppendixCap = 16 * 1024;

void append_block_with_blank_line(std::string & buf, const std::string & block) {
    if (block.empty()) return;
    if (!buf.empty() && buf.back() != '\n') buf += '\n';
    buf += '\n';
    buf += block;
}
}  // namespace

std::string Session::Impl::full_compose(const std::vector<Tool> & registered_tools,
                                        bool include_session_info_tail) {
    // Base + per-tool addenda. Delegates to the lib helper so the
    // addendum-concat policy (effective_system_addendum fallback,
    // 8 KB per-tool cap, sanitization, blank-line separator) lives
    // in one place — see easyai/preamble.hpp.
    std::string out = preamble::compose_system_prompt(
        compose_system(registered_tools), registered_tools);

    // Operator-supplied static and dynamic appends.
    for (const auto & s : system_static) {
        if (s.empty()) continue;
        const std::string clean = preamble::sanitize_addendum(s, kAppendixCap);
        append_block_with_blank_line(out, clean);
    }
    for (auto & fn : system_dynamic) {
        std::string s = fn ? fn() : std::string();
        if (s.empty()) continue;
        const std::string clean = preamble::sanitize_addendum(s, kAppendixCap);
        append_block_with_blank_line(out, clean);
    }

    const std::string dyn = compose_dynamic();
    if (!dyn.empty()) {
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += dyn;
    }

    if (include_session_info_tail && !registered_tools.empty()) {
        const std::string si = preamble::build_session_info(registered_tools);
        if (!si.empty()) out += si;
    }
    return out;
}

void Session::refresh_system() {
    if (!p_->backend) return;
    const std::vector<Tool> live = tools();
    const std::string composed = p_->full_compose(
        live, /*include_session_info_tail=*/p_->mode == Mode::Local);

    // Push the new system WITHOUT clearing history — Engine::system /
    // Client::system are pure setters.  Going through
    // backend->set_system would clear history (matches the explicit
    // /system <text> REPL semantics, but is wrong for a mid-session
    // append).  See SECURITY_AUDIT §25.2.
    if (auto * e = engine_ptr()) e->system(composed);
    else if (auto * c = client_ptr()) c->system(composed);
}

void Session::reset() {
    if (p_->backend) p_->backend->reset();
}

void Session::set_system(std::string text) {
    p_->system_base       = std::move(text);
    p_->has_explicit_base = true;
    // Operator-facing "replace base + fresh start" — go through
    // backend->set_system which clears history (matches REPL
    // /system <text> semantics).  Compose with the full chain so
    // tool addenda / appends / dynamic / catalogue all stick.
    if (!p_->backend) return;
    const std::vector<Tool> live = tools();
    const std::string composed = p_->full_compose(
        live, /*include_session_info_tail=*/p_->mode == Mode::Local);
    p_->backend->set_system(composed);
}

// --------------------------------------------------------------- chat
std::string Session::chat(const std::string & user) {
    if (!p_->backend) {
        p_->last_err = "session not initialised (call init() first)";
        return {};
    }
    auto cb = p_->token_cb ? p_->token_cb : Backend::Tokenizer{};
    std::string answer = p_->backend->chat(user, cb);
    if (answer.empty()) {
        const std::string be = p_->backend->last_error();
        if (!be.empty()) p_->last_err = be;
    }
    return answer;
}

// --------------------------------------------------------------- introspection
Session::Mode Session::mode() const { return p_->mode; }

std::string Session::render_system() const {
    // Same composition as refresh_system, but returns the string
    // instead of pushing it.  Safe to call pre-init (live tools()
    // returns empty, so the catalogue tail is omitted; everything
    // else still renders).
    const std::vector<Tool> snap = tools();
    return p_->full_compose(snap, /*include_session_info_tail=*/p_->mode == Mode::Local);
}

std::vector<Tool> Session::tools() const {
    // The Backend interface only exposes name/desc pairs, but Session's
    // contract is to return real Tool objects.  Reach into the
    // underlying Engine / Client for the live registry.
    std::vector<Tool> out;
    if (auto * e = engine_ptr()) {
        const auto & ts = e->tools();
        out.insert(out.end(), ts.begin(), ts.end());
        return out;
    }
    if (auto * c = client_ptr()) {
        const auto & ts = c->tools();
        out.insert(out.end(), ts.begin(), ts.end());
        return out;
    }
    return out;
}

std::string Session::last_error() const {
    if (!p_->last_err.empty()) return p_->last_err;
    if (p_->backend) return p_->backend->last_error();
    return {};
}

Backend * Session::backend() const { return p_->backend.get(); }

int  Session::ctx_pct          () const { return p_->backend ? p_->backend->ctx_pct() : -1; }
bool Session::last_was_ctx_full() const { return p_->backend && p_->backend->last_was_ctx_full(); }
std::string Session::info       () const { return p_->backend ? p_->backend->info() : std::string(); }
std::size_t Session::tool_count () const { return p_->backend ? p_->backend->tool_count() : 0; }

std::vector<std::pair<std::string, std::string>> Session::tool_list() const {
    return p_->backend ? p_->backend->tool_list()
                       : std::vector<std::pair<std::string, std::string>>{};
}

void Session::set_sampling(float t, float p, int k, float m) {
    if (p_->backend) p_->backend->set_sampling(t, p, k, m);
    // Mirror into the cached preset so refresh_system() / further
    // setters keep coherent state.
    if (t >= 0) { p_->local_cfg.preset.temperature = t; p_->remote_cfg.preset.temperature = t; }
    if (p >= 0) { p_->local_cfg.preset.top_p       = p; p_->remote_cfg.preset.top_p       = p; }
    if (k >= 0) { p_->local_cfg.preset.top_k       = k; p_->remote_cfg.preset.top_k       = k; }
    if (m >= 0) { p_->local_cfg.preset.min_p       = m; p_->remote_cfg.preset.min_p       = m; }
}

Engine * Session::engine_ptr() const {
    if (p_->mode != Mode::Local) return nullptr;
    auto * lb = dynamic_cast<LocalBackend *>(p_->backend.get());
    return lb ? lb->engine() : nullptr;
}

Client * Session::client_ptr() const {
    if (p_->mode != Mode::Remote) return nullptr;
    auto * rb = dynamic_cast<RemoteBackend *>(p_->backend.get());
    return rb ? rb->client() : nullptr;
}

}  // namespace easyai
