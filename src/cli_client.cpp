// libeasyai-cli-side overloads — symbols that touch easyai::Client live
// here so the engine-only library doesn't drag in the HTTP client.
#include "easyai/backend.hpp"
#include "easyai/cli.hpp"
#include "easyai/client.hpp"
#include "easyai/presets.hpp"
#include "easyai/tool.hpp"
#include "easyai/ui.hpp"

#include <cstdio>
#include <memory>
#include <sstream>

namespace easyai::cli {

void Toolbelt::apply(Client & client) const {
    for (auto & t : tools()) client.add_tool(t);
    // Mirror src/cli.cpp's apply(Engine &): bump the hop ceiling
    // whenever an interactive subprocess executor is registered.
    if (allow_bash_ || allow_python_) client.max_tool_hops(99999);
}

bool client_has_tool(const Client & client, const std::string & name) {
    for (const auto & t : client.tools()) if (t.name == name) return true;
    return false;
}

namespace {
// One-liner for "operation failed" — prints the Client's last_error
// in red on stderr (so stdout stays clean for piping) and returns 1.
int report_error(Client & client, const ui::Style & st) {
    std::fprintf(stderr, "%serror:%s %s\n",
                 st.red(), st.reset(), client.last_error().c_str());
    return 1;
}
}  // namespace

int print_models(Client & client, const ui::Style & st, std::FILE * out) {
    std::vector<RemoteModel> ms;
    if (!client.list_models(ms)) return report_error(client, st);
    for (const auto & m : ms) {
        std::fprintf(out, "%s%s%s  (owned_by=%s)\n",
                     st.bold(), m.id.c_str(), st.reset(), m.owned_by.c_str());
    }
    return 0;
}

int print_local_tools(Client & client, const ui::Style & st, std::FILE * out) {
    if (client.tools().empty()) {
        std::fprintf(stderr,
            "%sno tools registered.%s  Use --tools / --sandbox / --allow-bash "
            "to enable some.\n", st.dim(), st.reset());
        return 0;
    }
    std::fprintf(out, "%slocal tools (%zu):%s\n",
                 st.bold(), client.tools().size(), st.reset());
    for (const auto & t : client.tools()) {
        ui::print_tool_row(t.name, t.description, st, out);
    }
    return 0;
}

int print_remote_tools(Client & client, const ui::Style & st, std::FILE * out) {
    std::vector<RemoteTool> ts;
    if (!client.list_remote_tools(ts)) return report_error(client, st);
    std::fprintf(out, "%sremote tools (%zu):%s\n",
                 st.bold(), ts.size(), st.reset());
    for (const auto & t : ts) {
        ui::print_tool_row(t.name, t.description, st, out);
    }
    return 0;
}

int print_health(Client & client, const ui::Style & st, std::FILE * out) {
    if (!client.health()) {
        std::fprintf(stderr, "%sunhealthy:%s %s\n",
                     st.red(), st.reset(), client.last_error().c_str());
        return 1;
    }
    std::fprintf(out, "%sok%s\n", st.green(), st.reset());
    return 0;
}

int print_props(Client & client, std::FILE * out) {
    std::string body;
    if (!client.props(body)) {
        std::fprintf(stderr, "error: %s\n", client.last_error().c_str());
        return 1;
    }
    std::fputs(body.c_str(), out);
    std::fputc('\n', out);
    return 0;
}

int print_metrics(Client & client, std::FILE * out) {
    std::string body;
    if (!client.metrics(body)) {
        std::fprintf(stderr, "error: %s\n", client.last_error().c_str());
        return 1;
    }
    std::fputs(body.c_str(), out);
    return 0;
}

int set_preset(Client & client, const std::string & name,
               const ui::Style & st, std::FILE * out) {
    if (!client.set_preset(name)) return report_error(client, st);
    std::fprintf(out, "%spreset → %s%s\n", st.green(), name.c_str(), st.reset());
    return 0;
}

}  // namespace easyai::cli

namespace easyai::ui {

// Streaming::attach(Client &) — Client-specific because Engine has no
// on_reason channel (reasoning is only streamed in the SSE/remote
// path).  Same dispatch as the Engine variant otherwise.
//
// Also wires the Client's last_ctx_pct into the Spinner: each tool
// dispatch and each token batch is a natural moment to refresh the
// gauge — by the time the next turn lands, the spinner already shows
// the latest %, and the heartbeat keeps it visually animated.
Streaming & Streaming::attach(Client & client) {
    Client * pcli = &client;
    auto refresh_pct = [this, pcli]() {
        int pct = pcli->last_ctx_pct();
        if (pct >= 0) spinner_.set_context_pct(pct);
        int used = pcli->last_ctx_used();
        int total = pcli->last_n_ctx();
        if (used >= 0 && total > 0) spinner_.set_context_tokens(used, total);
    };
    client.on_token ([this, refresh_pct](const std::string & p){
        this->on_token_(p);
        refresh_pct();
    });
    client.on_reason([this, refresh_pct](const std::string & p){
        this->on_reason_(p);
        refresh_pct();
    });
    client.on_tool  ([this, refresh_pct](const ToolCall & c, const ToolResult & r){
        this->on_tool_(c, r);
        refresh_pct();
    });
    client.on_prompt_eval(
        [this, pcli](int n_tokens, int /*n_cached*/, double prompt_ms, double tps) {
            if (n_tokens <= 0) return;
            int used  = pcli->last_ctx_used();
            int total = pcli->last_n_ctx();
            if (used < 0) used = n_tokens;
            if (used >= 0 && total > 0) {
                spinner_.set_context_pct((int)(100LL * used / total));
                spinner_.set_context_tokens(used, total);
            } else if (used >= 0) {
                spinner_.set_context_tokens(used, -1);
            }
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                "\n%s● prompt eval: %d tok · %.0f ms · %.1f t/s%s\n",
                style_.green(), n_tokens, prompt_ms, tps, style_.reset());
            spinner_.write(std::string(buf));
        });
    return *this;
}

}  // namespace easyai::ui


// ============================================================================
// RemoteBackend — wraps easyai::Client.  Lives in libeasyai-cli because
// the engine-only library doesn't have access to Client.
// ============================================================================
namespace easyai {

struct RemoteBackend::Impl {
    Config                  cfg;
    std::unique_ptr<Client> client;
    std::string             last_err;

    void rebuild() {
        client = std::make_unique<Client>();
        client->endpoint(cfg.base_url)
               .model   (cfg.model)
               .timeout_seconds((int) cfg.timeout_seconds);
        if (!cfg.api_key.empty())       client->api_key(cfg.api_key);
        if (!cfg.system_prompt.empty()) client->system (cfg.system_prompt);
        push_sampling();
        if (cfg.max_tokens > 0)  client->max_tokens(cfg.max_tokens);
        if (cfg.seed       >= 0) client->seed      (cfg.seed);
        if (cfg.tls_insecure)            client->tls_insecure(true);
        if (!cfg.ca_cert_path.empty())   client->ca_cert_path(cfg.ca_cert_path);

        if (cfg.with_tools) {
            cli::Toolbelt()
                .sandbox   (cfg.sandbox)
                .allow_bash(cfg.allow_bash)
                .apply     (*client);
        }
    }
    void push_sampling() {
        if (!client) return;
        if (cfg.preset.temperature >= 0) client->temperature(cfg.preset.temperature);
        if (cfg.preset.top_p       >= 0) client->top_p      (cfg.preset.top_p);
        if (cfg.preset.top_k       >  0) client->top_k      (cfg.preset.top_k);
        if (cfg.preset.min_p       >  0) client->min_p      (cfg.preset.min_p);
    }
};

RemoteBackend::RemoteBackend(Config c) : p_(std::make_unique<Impl>()) {
    p_->cfg = std::move(c);
    if (p_->cfg.preset.name.empty()) {
        // Default to "precise" (matches LocalBackend + easyai-server +
        // easyai-local + easyai-cli — see src/presets.cpp for values).
        if (const auto * pr = find_preset("precise")) p_->cfg.preset = *pr;
    }
    p_->rebuild();
}
RemoteBackend::~RemoteBackend() = default;

bool RemoteBackend::init(std::string & err) {
    if (p_->cfg.base_url.empty()) {
        err = "remote backend: --url is required";
        return false;
    }
    return true;
}

std::string RemoteBackend::chat(const std::string & user_text, const Tokenizer & cb) {
    p_->client->on_token(cb);
    std::string answer = p_->client->chat(user_text);
    if (answer.empty() && !p_->client->last_error().empty()) {
        p_->last_err = p_->client->last_error();
        return {};
    }
    return answer;
}

void RemoteBackend::reset() {
    p_->client->clear_history();
    p_->last_err.clear();
}

void RemoteBackend::set_system(const std::string & t) {
    p_->cfg.system_prompt = t;
    p_->rebuild();
}

void RemoteBackend::set_sampling(float t, float p, int k, float m) {
    if (t >= 0) p_->cfg.preset.temperature = t;
    if (p >= 0) p_->cfg.preset.top_p       = p;
    if (k >= 0) p_->cfg.preset.top_k       = k;
    if (m >= 0) p_->cfg.preset.min_p       = m;
    p_->push_sampling();
}

std::string RemoteBackend::info() const {
    std::ostringstream o;
    o << "remote " << p_->cfg.base_url
      << "  (model=" << p_->cfg.model << ")"
      << (p_->cfg.api_key.empty() ? "" : "  [auth]");
    return o.str();
}

std::string RemoteBackend::last_error() const { return p_->last_err; }
std::size_t RemoteBackend::tool_count() const { return p_->client ? p_->client->tools().size() : 0; }

std::vector<std::pair<std::string,std::string>> RemoteBackend::tool_list() const {
    std::vector<std::pair<std::string,std::string>> out;
    if (!p_->client) return out;
    for (const auto & t : p_->client->tools()) out.emplace_back(t.name, t.description);
    return out;
}

int  RemoteBackend::ctx_pct()           const { return p_->client ? p_->client->last_ctx_pct() : -1; }
bool RemoteBackend::last_was_ctx_full() const { return p_->client && p_->client->last_was_ctx_full(); }

}  // namespace easyai
