// src/remote_model_tool.cpp — "ai-<name>" tools: consult a remote
// OpenAI-protocol model as a peer. See easyai/remote_model_tool.hpp.

#include "easyai/remote_model_tool.hpp"

#include "easyai/client.hpp"
#include "easyai/log.hpp"

#include <cctype>
#include <chrono>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace easyai::tools {

namespace {

// ---- defaults --------------------------------------------------------------
const char * const kSectionPrefix = "REMOTE_MODEL_";
const char * const kDefaultModel  = "easyai";

// Two baked-in connections so the family works with no config at all.
struct DefaultConn { const char * name; const char * url; const char * desc; };
const DefaultConn kDefaults[] = {
    { "local", "http://ai.local",
      "Local AI box model — fast, always-on, general purpose." },
    { "pro",   "http://ai-pro.local",
      "Larger model for harder problems and deeper reasoning." },
};

// A peer call should not hang the agent forever; the Client's own
// default (24h) is wrong here. Operators override per connection with
// `timeout` in the INI.
constexpr int kDefaultTimeoutSeconds = 300;
// The peer is a single-shot advisor: it gets no tools, so the agentic
// loop never needs more than the one completion hop.
constexpr int kPeerMaxToolHops = 1;

// System prompt handed to the remote peer so it knows its role. The
// caller (another AI agent) integrates the answer, so we ask for
// directness over hand-holding.
const char * const kPeerSystemPrompt =
    "You are a capable AI assistant acting as a peer reviewer and "
    "problem-solving partner for ANOTHER AI agent — not for an end user. "
    "The agent will send you a question, a piece of its own reasoning to "
    "check, or a hard problem to co-solve. Answer directly and precisely. "
    "If you spot an error or a better approach, say so plainly. State your "
    "assumptions and how confident you are. Be concise: the agent will read "
    "and integrate your answer, it will not relay it verbatim to a human.";

std::string trim(std::string s) {
    auto sp = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && sp((unsigned char) s.front())) s.erase(s.begin());
    while (!s.empty() && sp((unsigned char) s.back()))  s.pop_back();
    return s;
}

std::string to_lower(std::string s) {
    for (char & c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}

// Truthy / falsy INI words, mirroring the cli's own boolean parsing.
bool is_false_word(const std::string & v) {
    std::string lc = to_lower(v);
    return lc == "false" || lc == "no" || lc == "off" || lc == "0";
}
bool is_true_word(const std::string & v) {
    std::string lc = to_lower(v);
    return lc == "true" || lc == "yes" || lc == "on" || lc == "1";
}

// Ensure an endpoint carries a scheme — match the cli's convenience of
// auto-prefixing http:// when the operator wrote a bare host[:port].
void ensure_scheme(std::string & url) {
    if (url.empty()) return;
    if (url.compare(0, 7, "http://") == 0)  return;
    if (url.compare(0, 8, "https://") == 0) return;
    url = "http://" + url;
}

// One-line, truncated preview of text for the raw transaction log
// (control chars collapsed to spaces, capped). Kept out of the
// stderr-tee'd lines so prompt / answer bodies don't leak into journald.
std::string log_preview(const std::string & s, std::size_t cap = 200) {
    std::string out;
    out.reserve((s.size() < cap ? s.size() : cap) + 4);
    for (char c : s) {
        if (out.size() >= cap) { out += "..."; break; }
        out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    return out;
}

// ---- INI value lookup helpers ---------------------------------------------
using Section = std::map<std::string, std::string>;

std::string get_one(const Section & m, std::initializer_list<const char *> keys) {
    for (const char * k : keys) {
        auto it = m.find(k);
        if (it != m.end() && !it->second.empty()) return it->second;
    }
    return std::string();
}

void set_float(const Section & m, const char * key, float & out) {
    auto it = m.find(key);
    if (it == m.end() || it->second.empty()) return;
    try { out = std::stof(it->second); } catch (...) {}
}
void set_int(const Section & m, const char * key, int & out) {
    auto it = m.find(key);
    if (it == m.end() || it->second.empty()) return;
    try { out = std::stoi(it->second); } catch (...) {}
}

// ---- description composition ----------------------------------------------
// The per-turn trigger that ships in every request's tools[].
std::string build_short(const RemoteModelSpec & s) {
    return "Consult AI peer \"" + s.name +
           "\": double-check your work, co-solve a hard problem, or get a "
           "second opinion. Send one `prompt`.";
}

// The full manual returned by tool_lookup / /v1/tools. Operator text
// (if any) sits ABOVE the static base that normalises usage.
std::string build_full(const RemoteModelSpec & s) {
    std::string out;
    std::string op = trim(s.description);
    if (!op.empty()) out += op + "\n\n";

    out +=
        "You are calling \"" + s.name + "\", a separate AI model reachable "
        "over the network. Treat it as a knowledgeable peer sitting next to "
        "you — a thinking partner, not a search engine or a fact database.\n"
        "\n"
        "WHEN TO CALL IT\n"
        "- You are unsure and want a second pair of eyes — ask it to check "
        "your work.\n"
        "- The problem is hard (tricky math, subtle logic, deep domain "
        "knowledge) — ask it to solve it with you and compare answers.\n"
        "- You are stuck or going in circles — hand it the problem fresh.\n"
        "- You want to confirm a risky or irreversible step before taking "
        "it.\n"
        "\n"
        "HOW TO CALL IT\n"
        "- Put everything it needs in `prompt`: the question, the relevant "
        "context, what you already tried, and the kind of answer you want. "
        "It CANNOT see your conversation — it only sees the text you send.\n"
        "- Ask one clear thing at a time. Be specific.\n"
        "\n"
        "WHAT YOU GET BACK\n"
        "- Its full reply as text. Read it critically: it is a peer, not an "
        "oracle. If it disagrees with you, weigh both views and decide. Tell "
        "the user when you relied on it.\n"
        "\n"
        "Each call is independent — it does not remember previous calls.";
    return out;
}

// Concise reinforcement composed into the system prompt. Kept short so
// many connections don't bloat the prompt; the operator's one-liner is
// folded in so the model can tell peers apart when several are present.
std::string build_addendum(const RemoteModelSpec & s) {
    std::string out = "`ai-" + s.name + "` — ";
    std::string op = trim(s.description);
    if (!op.empty()) {
        if (op.back() != '.' && op.back() != '!' && op.back() != '?')
            op += '.';
        out += op + " ";
    }
    out +=
        "A separate AI model you can consult as a peer: ask it to "
        "double-check your work, co-solve hard problems, or give a second "
        "opinion before a risky step. Put full context in `prompt` (it "
        "can't see this chat); weigh its answer rather than obeying it "
        "blindly.";
    return out;
}

}  // namespace

std::vector<RemoteModelSpec> resolve_remote_models(const config::Ini & ini) {
    std::vector<RemoteModelSpec> out;

    // Insertion-ordered lookup-or-create by connection name.
    auto slot = [&out](const std::string & name) -> RemoteModelSpec & {
        for (auto & s : out)
            if (s.name == name) return s;
        out.push_back(RemoteModelSpec{});
        out.back().name = name;
        return out.back();
    };

    // 1) seed the two defaults.
    for (const auto & d : kDefaults) {
        RemoteModelSpec & s = slot(d.name);
        s.url         = d.url;
        s.model       = kDefaultModel;
        s.description = d.desc;
    }

    // 2) overlay [REMOTE_MODEL_<name>] sections.
    const std::string prefix = kSectionPrefix;
    for (const auto & kv : ini.sections) {
        const std::string & sec = kv.first;
        if (sec.size() <= prefix.size())                 continue;
        if (sec.compare(0, prefix.size(), prefix) != 0)  continue;
        std::string name = trim(sec.substr(prefix.size()));
        if (name.empty()) continue;

        const Section & m = kv.second;
        RemoteModelSpec & s = slot(name);

        std::string url = get_one(m, { "url", "endpoint" });
        if (!url.empty()) s.url = url;

        std::string key = get_one(m, { "key", "api_key" });
        if (!key.empty()) s.api_key = key;

        std::string model = get_one(m, { "model" });
        if (!model.empty()) s.model = model;

        std::string desc = get_one(m, { "description", "desc" });
        if (!desc.empty()) s.description = desc;

        set_float(m, "temperature", s.temperature);
        set_float(m, "top_p",       s.top_p);
        set_int  (m, "top_k",       s.top_k);
        set_float(m, "min_p",       s.min_p);
        set_int  (m, "max_tokens",  s.max_tokens);
        set_int  (m, "timeout",     s.timeout_seconds);

        std::string ca = get_one(m, { "ca_cert_path", "ca_cert" });
        if (!ca.empty()) s.ca_cert_path = ca;

        std::string tls = get_one(m, { "tls_insecure" });
        if (!tls.empty() && is_true_word(tls)) s.tls_insecure = true;

        // Opt-in switch: default off; `enabled = true` (or `enable =
        // true`) turns the connection on. `false` keeps it off.
        std::string en = get_one(m, { "enabled", "enable" });
        if (!en.empty()) {
            if (is_true_word(en))       s.enabled = true;
            else if (is_false_word(en)) s.enabled = false;
        }
    }

    // 3) normalise schemes (bare host → http://).
    for (auto & s : out) ensure_scheme(s.url);

    return out;
}

Tool remote_model(const RemoteModelSpec & spec) {
    const RemoteModelSpec s = spec;  // captured by value into the handler

    auto handler = [s](const ToolCall & call) -> ToolResult {
        std::string prompt = trim(args::get_string_or(call.arguments_json, "prompt", ""));
        if (prompt.empty())
            return ToolResult::error(
                "ai-" + s.name + ": `prompt` is required and was empty.");
        if (s.url.empty())
            return ToolResult::error(
                "ai-" + s.name + ": no endpoint configured (set `url` in "
                "[REMOTE_MODEL_" + s.name + "]).");

        const char * model = s.model.empty() ? kDefaultModel : s.model.c_str();

        Client cli;
        cli.endpoint(s.url)
           .model(model)
           .timeout_seconds(s.timeout_seconds > 0 ? s.timeout_seconds
                                                   : kDefaultTimeoutSeconds)
           .http_retries(2)
           .max_tool_hops(kPeerMaxToolHops)
           .system(kPeerSystemPrompt);
        if (!s.api_key.empty())      cli.api_key(s.api_key);
        if (s.temperature >= 0.0f)   cli.temperature(s.temperature);
        if (s.top_p       >= 0.0f)   cli.top_p(s.top_p);
        if (s.top_k       >= 0)      cli.top_k(s.top_k);
        if (s.min_p       >= 0.0f)   cli.min_p(s.min_p);
        if (s.max_tokens  >  0)      cli.max_tokens(s.max_tokens);
        if (s.tls_insecure)          cli.tls_insecure(true);
        if (!s.ca_cert_path.empty()) cli.ca_cert_path(s.ca_cert_path);

        // Logged after Client construction so the auto-opened raw log is
        // attached and the prompt preview lands in it. The summary line
        // also tees to stderr (no body — keeps prompt text out of journald).
        easyai::log::write(
            "ai-%s: calling %s (model=%s, prompt=%zu chars)\n",
            s.name.c_str(), s.url.c_str(), model, prompt.size());
        if (auto * fp = easyai::log::file()) {
            std::fprintf(fp, "ai-%s: prompt> %s\n",
                         s.name.c_str(), log_preview(prompt).c_str());
            std::fflush(fp);
        }

        const auto t0 = std::chrono::steady_clock::now();
        std::string reply = cli.chat(prompt);
        const long long ms = (long long) std::chrono::duration_cast<
            std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
        std::string err = cli.last_error();
        if (reply.empty()) {
            const char * why = err.empty() ? "empty reply" : err.c_str();
            easyai::log::error("ai-%s: FAILED after %lld ms — %s\n",
                               s.name.c_str(), ms, why);
            if (!err.empty())
                return ToolResult::error(
                    "ai-" + s.name + " call failed: " + err);
            return ToolResult::error(
                "ai-" + s.name + " returned an empty reply.");
        }
        easyai::log::write("ai-%s: ok after %lld ms (reply=%zu chars)\n",
                           s.name.c_str(), ms, reply.size());
        if (auto * fp = easyai::log::file()) {
            std::fprintf(fp, "ai-%s: reply> %s\n",
                         s.name.c_str(), log_preview(reply).c_str());
            std::fflush(fp);
        }
        return ToolResult::ok(reply);
    };

    return Tool::builder("ai-" + s.name)
        .short_describe(build_short(s))
        .describe(build_full(s))
        .system_addendum(build_addendum(s))
        .param("prompt", "string",
               "The full question or task for the peer model. Include all "
               "context it needs — it cannot see your conversation.", true)
        .handle(handler)
        .build();
}

}  // namespace easyai::tools
