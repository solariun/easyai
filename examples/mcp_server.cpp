// =============================================================================
//  easyai-mcp-server — standalone Model Context Protocol provider.
//
//  Goals
//  -----
//   * Expose the SAME tool catalogue easyai-server exposes — built-ins
//     (datetime, web, fs, bash), RAG (single `rag(action=...)` tool),
//     and operator-defined `EASYAI-*.tools` external packs — over a
//     dedicated MCP endpoint at POST /mcp.
//   * No GGUF model loaded. No /v1/chat/completions. No webui. The
//     binary's only purpose is dispatching tool calls for other AI
//     applications (Claude Desktop, Cursor, Continue, custom JSON-RPC
//     clients) over the Model Context Protocol.
//   * Designed for high concurrency — thousands of in-flight JSON-RPC
//     requests across hundreds of worker threads. Tool implementations
//     come unmodified from libeasyai (whose RagStore now uses
//     std::shared_mutex for parallel reads, whose external-tools and
//     bash spawn paths are process-isolated, whose fetch_web cache is
//     guarded by its own mutex). The binary itself adds:
//       - configurable cpp-httplib ThreadPool size (--threads N)
//       - in-flight tools/call cap with 503 on saturation
//         (--max-concurrent-calls N)
//       - bounded Authorization header, request body, and JSON depth.
//
//  Configuration
//  -------------
//  Identical INI/CLI overlay machinery as easyai-server — same FlagDef
//  table pattern, same `[SERVER] / [ENGINE] / [MCP_USER]` sections. The
//  ENGINE keys are intentionally absent here (no model). The default
//  config path is /etc/easyai/easyai-mcp.ini (separate from
//  easyai.ini so a chat server and an MCP server can coexist on one
//  host with their own knobs).
//
//  Memory hygiene
//  --------------
//   * No raw new/delete. All resources owned by std::unique_ptr or by
//     value types with a custom destructor.
//   * Bounded request body size (default 1 MiB).
//   * Bounded Authorization header (4 KiB) — matches easyai-server.
//   * Bounded JSON depth (64 levels) inside the MCP dispatcher (already
//     enforced by easyai::mcp::handle_request).
//   * Bounded in-flight tools/call dispatches (atomic counter; reject
//     with 503 when cap reached).
//   * Catches std::exception at every HTTP boundary so a malformed
//     request can never tear down the server.
// =============================================================================

#include "easyai/builtin_tools.hpp"     // tool_lookup
#include "easyai/cli.hpp"               // Toolbelt
#include "easyai/config.hpp"            // INI parser
#include "easyai/external_tools.hpp"    // EASYAI-*.tools loader
#include "easyai/log.hpp"               // optional log tee
#include "easyai/mcp.hpp"               // JSON-RPC dispatcher
#include "easyai/rag_tools.hpp"         // knowledge_split_tools
#include "easyai/tool.hpp"

#include "httplib.h"                    // vendored by llama.cpp
#include "nlohmann/json.hpp"            // vendored by llama.cpp

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>                     // chdir
#include <vector>

namespace {

using nlohmann::ordered_json;
using json = nlohmann::ordered_json;

// =============================================================================
// Tunable defaults — bounded values for a public-network deployment.
// =============================================================================
//
// These are conservative on purpose. Each one closes a class of resource
// exhaustion. Operators can override via CLI flag or INI key; rationale
// for raising any of them belongs in the change description.

// HTTP transport.
constexpr int    kDefaultPort                 = 8089;
constexpr char   kDefaultHost[]               = "127.0.0.1";

// Concurrency.
//
// kDefaultThreads — cpp-httplib worker pool size. 256 is plenty for a
// per-host MCP daemon; the bottleneck on real workloads is the tools
// themselves (libcurl outbound for web_*, fork/execve for external-tools
// + bash, disk for RAG / fs_*), not the dispatcher. Each worker thread
// costs ~8 MiB of pthread stack on Linux/glibc → 256 ≈ 2 GiB virtual
// (commit-on-touch, real RSS far smaller). Larger pools are routinely
// fine on modern hosts; 256 is the safe default.
//
// kDefaultMaxConcurrentCalls — backstop on tools/call dispatches.
// Independent from threads count: a 1024-thread pool with this set to
// 256 will accept tools/list, ping, initialize on every worker but
// reject `tools/call` with 503 once 256 are in flight, leaving thread
// headroom for cheap requests during a fork-heavy spike. Default
// equals threads so the cap doesn't fire under default config; reduce
// to leave headroom, or raise if you've upsized threads.
constexpr int    kDefaultThreads              = 256;
constexpr int    kDefaultMaxConcurrentCalls   = 256;

// Per-request limits.
//
// kDefaultMaxBody — 1 MiB. MCP request bodies are tiny: a tools/call
// is a method name + a small arguments object. 1 MiB is already
// generous; easyai-server uses 8 MiB because chat completions ship
// long histories, but MCP doesn't.
//
// kReadTimeoutSeconds / kWriteTimeoutSeconds — slow-loris defence. A
// JSON-RPC request takes <1 ms over loopback and <200 ms over a LAN;
// 30/60 s ceilings catch hung sockets without false-positiving real
// clients.
//
// (The Authorization header cap — 4 KiB — lives in libeasyai as
// easyai::mcp::kMaxAuthHeaderBytes; both auth gates below use that
// constant directly so the value is single-sourced.)
constexpr std::size_t kDefaultMaxBody         = 1u * 1024u * 1024u;
constexpr int    kReadTimeoutSeconds          = 30;
constexpr int    kWriteTimeoutSeconds         = 60;

// =============================================================================
// In-flight tools/call limiter — bounds peak concurrency for the one
// JSON-RPC method that may spawn external processes / hit the network.
// Cheap methods (initialize, tools/list, ping) are not counted because
// they don't consume host resources beyond a JSON parse.
// =============================================================================
//
// Implementation: lock-free atomic counter. acquire() does fetch_add and
// rolls back if the post-increment value exceeds the cap. Slightly
// over-pessimistic at the boundary (briefly observes count==max+1
// before the rollback) but never overshoots — the worst that can
// happen is a thread sees count==max, rejects, and decrements; another
// thread that successfully acquired will eventually release. No lost
// wakeups, no deadlock, no priority inversion.
class InFlightLimiter {
public:
    explicit InFlightLimiter(int max_in_flight)
        : max_(max_in_flight) {}

    // Returns true on success (counter incremented; caller must call
    // release() exactly once). Returns false on saturation.
    bool acquire() {
        // fetch_add returns the OLD value; if it was already at the cap
        // we've now taken the count to max+1 — roll back and reject.
        const int prev = count_.fetch_add(1, std::memory_order_acq_rel);
        if (prev >= max_) {
            count_.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
        return true;
    }

    void release() {
        count_.fetch_sub(1, std::memory_order_acq_rel);
    }

    int in_flight() const {
        return count_.load(std::memory_order_acquire);
    }

    int capacity() const { return max_; }

private:
    std::atomic<int> count_{0};
    int              max_;
};

// RAII guard so the counter is always released, even on exceptions.
class InFlightGuard {
public:
    explicit InFlightGuard(InFlightLimiter & l) : limiter_(&l), held_(false) {
        held_ = limiter_->acquire();
    }
    ~InFlightGuard() { if (held_) limiter_->release(); }

    bool ok() const { return held_; }

    InFlightGuard(const InFlightGuard &)             = delete;
    InFlightGuard & operator=(const InFlightGuard &) = delete;

private:
    InFlightLimiter * limiter_;
    bool              held_;
};

// =============================================================================
// ServerArgs / FlagDef — same pattern as examples/server.cpp.
// One row per setting, both CLI parser and INI overlay walk the same
// table.
// =============================================================================

struct ServerArgs {
    // ----- core paths / identity (SERVER) -----
    std::string config_path = "/etc/easyai/easyai-mcp.ini";
    std::string host        = kDefaultHost;
    int         port        = kDefaultPort;

    // Sandbox + tool gating (mirrors easyai-server).
    std::string sandbox;
    bool        allow_fs     = false;
    bool        allow_bash   = false;
    // python3 defaults ON: stdlib-only interpreter with disk access
    // auto-restricted to the sandbox root via a Python preamble.
    // Auto-registers when allow_bash is on OR sandbox is set; the
    // server's existing explicit-opt-in policy for fs is preserved
    // (fs only registers when allow_fs is also set), so the matrix
    // becomes: --sandbox alone → python3; --allow-fs --sandbox →
    // python3 + fs; --allow-bash --sandbox → python3 + bash. Use
    // --no-python (or [SERVER] allow_python = off) to opt out.
    bool        allow_python = true;
    bool        load_tools  = true;

    // Tool packs.
    std::string external_tools_dir;
    std::string rag_dir;

    // HTTP knobs.
    std::size_t max_body    = kDefaultMaxBody;

    // Authentication.
    std::string api_key;          // Bearer for /health, /metrics, /v1/tools.
    bool        no_mcp_auth = false;

    // Concurrency.
    int         threads             = kDefaultThreads;
    int         max_concurrent_calls = kDefaultMaxConcurrentCalls;

    // Observability.
    bool        metrics = false;
    bool        verbose = false;

    // Server identity surfaced on /health and on the MCP `initialize`
    // response. Operators on a multi-server fleet override to identify
    // each instance in their MCP client picker.
    std::string name = "easyai-mcp-server";

    // Tracks which CLI flags were explicitly passed; INI applies only
    // as defaults to fields the operator did NOT pass on the command
    // line. Same precedence as easyai-server: CLI > INI > hardcoded.
    std::set<std::string> cli_set;
};

struct FlagDef {
    std::vector<std::string> cli;
    std::string              ini_section;
    std::string              ini_key;
    std::string              canonical;
    bool                     takes_value = true;
    std::function<void(ServerArgs &, const std::string &)> set;
};

bool str_to_bool(const std::string & s_raw, bool def) {
    std::string s = s_raw;
    for (auto & c : s) c = (char) std::tolower((unsigned char) c);
    if (s.empty()) return true;     // CLI no-value → true
    if (s == "on" || s == "true" || s == "yes" || s == "1" ||
        s == "enable" || s == "enabled") return true;
    if (s == "off" || s == "false" || s == "no" || s == "0" ||
        s == "disable" || s == "disabled") return false;
    return def;
}

auto SET_STR(std::string ServerArgs::* f) {
    return [f](ServerArgs & a, const std::string & v) {
        if (!v.empty()) a.*f = v;
    };
}
auto SET_INT(int ServerArgs::* f) {
    return [f](ServerArgs & a, const std::string & v) {
        if (v.empty()) return;
        try { a.*f = std::stoi(v); } catch (...) {}
    };
}
auto SET_SIZE(std::size_t ServerArgs::* f) {
    return [f](ServerArgs & a, const std::string & v) {
        if (v.empty()) return;
        try {
            long long n = std::stoll(v);
            if (n < 0) n = 0;
            a.*f = (std::size_t) n;
        } catch (...) {}
    };
}
auto SET_BOOL_TRUE(bool ServerArgs::* f) {
    return [f](ServerArgs & a, const std::string & v) {
        a.*f = str_to_bool(v, a.*f);
    };
}
auto SET_BOOL_FALSE(bool ServerArgs::* f) {
    return [f](ServerArgs & a, const std::string & v) {
        if (v.empty()) { a.*f = false; return; }
        a.*f = str_to_bool(v, a.*f);
    };
}

// One table, two consumers (parse_args + apply_ini_to_args). Adding a
// new flag is one row.
const std::vector<FlagDef> & kFlags() {
    static const std::vector<FlagDef> table = {
        // ----- SERVER: identity / transport -----
        { {"--config"},                "",       "",                       "config",              true,  SET_STR(&ServerArgs::config_path) },
        { {"--host"},                  "SERVER", "host",                   "host",                true,  SET_STR(&ServerArgs::host) },
        { {"--port"},                  "SERVER", "port",                   "port",                true,  SET_INT(&ServerArgs::port) },
        { {"-n","--name"},             "SERVER", "name",                   "name",                true,  SET_STR(&ServerArgs::name) },
        { {"--max-body"},              "SERVER", "max_body",               "max_body",            true,  SET_SIZE(&ServerArgs::max_body) },
        // ----- SERVER: tool gating -----
        { {"--sandbox"},               "SERVER", "sandbox",                "sandbox",             true,  SET_STR(&ServerArgs::sandbox) },
        { {"--allow-fs"},              "SERVER", "allow_fs",               "allow_fs",            false, SET_BOOL_TRUE(&ServerArgs::allow_fs) },
        { {"--allow-bash"},            "SERVER", "allow_bash",             "allow_bash",          false, SET_BOOL_TRUE(&ServerArgs::allow_bash) },
        { {"--no-python"},             "",       "",                       "no_python",           false, SET_BOOL_FALSE(&ServerArgs::allow_python) },
        // [SERVER] allow_python = on/off — INI-only path. Operators
        // can also pass --no-python on the CLI to flip it off, but
        // there is no --allow-python (the flag defaults on).
        { {},                          "SERVER", "allow_python",           "allow_python",        true,  SET_BOOL_TRUE(&ServerArgs::allow_python) },
        { {"--no-tools"},              "SERVER", "load_tools",             "load_tools",          false, SET_BOOL_FALSE(&ServerArgs::load_tools) },
        { {"--external-tools"},        "SERVER", "external_tools",         "external_tools",      true,  SET_STR(&ServerArgs::external_tools_dir) },
        // `memory` (formerly `rag`): the legacy INI key `rag` is still
        // read (first row, INI-only) so existing easyai.ini files keep
        // working; the `memory` row is second so the new key wins when
        // both appear. Both share canonical `memory` so either CLI flag
        // suppresses both INI rows.
        { {},                          "SERVER", "rag",                    "memory",              true,  SET_STR(&ServerArgs::rag_dir) },
        { {"--memory","--RAG"},        "SERVER", "memory",                 "memory",              true,  SET_STR(&ServerArgs::rag_dir) },
        // ----- SERVER: auth -----
        { {"--api-key"},               "SERVER", "api_key",                "api_key",             true,  SET_STR(&ServerArgs::api_key) },
        { {"--no-mcp-auth"},           "",       "",                       "no_mcp_auth",         false, SET_BOOL_TRUE(&ServerArgs::no_mcp_auth) },
        // mcp_auth has NO CLI alias — INI-only; --no-mcp-auth is the override path.
        { {},                          "SERVER", "mcp_auth",               "mcp_auth",            true,
          [](ServerArgs & a, const std::string & v) {
              std::string s = v;
              for (auto & c : s) c = (char) std::tolower((unsigned char) c);
              if (s == "off" || s == "open" || s == "disabled" || s == "disable" ||
                  s == "false" || s == "no" || s == "0") {
                  a.no_mcp_auth = true;
              } else if (s == "on" || s == "required" || s == "enabled" ||
                         s == "enable" || s == "true" || s == "yes" || s == "1") {
                  a.no_mcp_auth = false;
              }
              // "auto" leaves the field untouched (default behaviour).
          } },
        // ----- SERVER: concurrency -----
        { {"-t","--threads"},          "SERVER", "threads",                "threads",             true,  SET_INT(&ServerArgs::threads) },
        { {"--max-concurrent-calls"},  "SERVER", "max_concurrent_calls",   "max_concurrent_calls",true,  SET_INT(&ServerArgs::max_concurrent_calls) },
        // ----- SERVER: observability -----
        { {"--metrics"},               "SERVER", "metrics",                "metrics",             false, SET_BOOL_TRUE(&ServerArgs::metrics) },
        { {"-v","--verbose"},          "SERVER", "verbose",                "verbose",             false, SET_BOOL_TRUE(&ServerArgs::verbose) },
    };
    return table;
}

[[noreturn]] void die_usage(const char * argv0) {
    std::fprintf(stderr,
        "Usage: %s [options]\n\n"
        "easyai-mcp-server — standalone Model Context Protocol provider.\n"
        "Exposes the same tool catalogue easyai-server exposes (built-ins\n"
        "+ memory + operator-defined external-tools manifests) over POST /mcp\n"
        "without loading a model. Designed for high-concurrency multi-client\n"
        "deployments.\n"
        "\nConfig:\n"
        "      --config <path>          Central INI config (default\n"
        "                                /etc/easyai/easyai-mcp.ini).\n"
        "                                Missing file = all-defaults +\n"
        "                                MCP open. CLI flags override INI;\n"
        "                                INI overrides hardcoded defaults.\n"
        "\nNetwork:\n"
        "      --host <addr>            Bind address (default 127.0.0.1).\n"
        "                                Use 0.0.0.0 for any-iface.\n"
        "      --port <n>               TCP port (default 8089).\n"
        "  -n, --name <id>              Server identity surfaced on\n"
        "                                /health and MCP initialize.\n"
        "      --max-body <bytes>       Max request body (default 1 MiB).\n"
        "\nConcurrency:\n"
        "  -t, --threads <n>            cpp-httplib worker pool size\n"
        "                                (default 256).\n"
        "      --max-concurrent-calls <n>\n"
        "                                Cap on in-flight tools/call\n"
        "                                dispatches; 503 on saturation\n"
        "                                (default 256).\n"
        "\nTools:\n"
        "      --sandbox <dir>          Root for fs / bash / external\n"
        "                                tools $SANDBOX placeholder.\n"
        "      --allow-fs               Register the unified `fs` tool\n"
        "                                (action=read / write / list / glob /\n"
        "                                grep / check_path / cwd / sandbox).\n"
        "      --allow-bash             Register `bash`. NOT a hardened\n"
        "                                sandbox — runs with this process's\n"
        "                                user privileges.\n"
        "      --no-python              Drop the `python3` tool. By default\n"
        "                                it auto-registers when --sandbox\n"
        "                                or --allow-bash is set. Stdlib-only\n"
        "                                interpreter (no PYTHON* env, no\n"
        "                                site-packages, no cwd on sys.path);\n"
        "                                disk access auto-restricted to the\n"
        "                                sandbox root via a Python preamble.\n"
        "                                NOT a hardened sandbox — `import\n"
        "                                os`, `import socket`, `import\n"
        "                                subprocess` all work. Use\n"
        "                                --no-python (or [SERVER]\n"
        "                                allow_python = off) to opt out.\n"
        "      --no-tools               Skip the built-in toolbelt entirely\n"
        "                                (datetime / web).\n"
        "      --external-tools <dir>   Load every EASYAI-*.tools manifest\n"
        "                                in <dir>. Per-file fault isolation.\n"
        "                                See EXTERNAL_TOOLS.md.\n"
        "      --memory <dir>           Enable the agent's persistent memory\n"
        "                                store, rooted at <dir> (alias: --RAG).\n"
        "                                Registers ONE `memory(action=...)`\n"
        "                                tool with sub-actions save / append /\n"
        "                                search / load / list / delete /\n"
        "                                keywords. See RAG.md.\n"
        "\nAuth:\n"
        "      --api-key <token>        Bearer required for /health,\n"
        "                                /metrics, /v1/tools when set.\n"
        "                                /mcp uses [MCP_USER] from the\n"
        "                                INI (see below).\n"
        "      --no-mcp-auth            Force /mcp open even if [MCP_USER]\n"
        "                                is populated. Emergency override.\n"
        "\nObservability:\n"
        "      --metrics                Enable Prometheus /metrics.\n"
        "  -v, --verbose                Log every dispatch to stderr.\n"
        "\n"
        "INI sections recognised (see easyai-mcp-server.md for the full\n"
        "key reference):\n"
        "  [SERVER]      every flag above (host, port, sandbox, threads,\n"
        "                 max_concurrent_calls, allow_fs, allow_bash,\n"
        "                 allow_python,\n"
        "                 external_tools, memory (legacy: rag), api_key, mcp_auth,\n"
        "                 metrics, verbose, max_body, name).\n"
        "  [MCP_USER]    one user per line: name = bearer-token.\n"
        "                 Populating any line enables Bearer auth on /mcp.\n",
        argv0);
    std::exit(2);
}

ServerArgs parse_args(int argc, char ** argv) {
    ServerArgs a;
    const auto & flags = kFlags();
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "-h" || s == "--help") die_usage(argv[0]);

        const FlagDef * matched = nullptr;
        for (const auto & f : flags) {
            for (const auto & alias : f.cli) {
                if (alias == s) { matched = &f; break; }
            }
            if (matched) break;
        }
        if (!matched) {
            std::fprintf(stderr, "unknown arg: %s\n", s.c_str());
            die_usage(argv[0]);
        }
        std::string value;
        if (matched->takes_value) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", s.c_str());
                die_usage(argv[0]);
            }
            value = argv[++i];
        }
        matched->set(a, value);
        a.cli_set.insert(matched->canonical);
    }
    return a;
}

void apply_ini_to_args(const easyai::config::Ini & ini, ServerArgs & a) {
    for (const auto & f : kFlags()) {
        if (f.ini_section.empty() || f.ini_key.empty()) continue;
        if (a.cli_set.count(f.canonical))               continue;
        std::string v = ini.get(f.ini_section, f.ini_key);
        if (v.empty()) continue;
        f.set(a, v);
    }
}

// =============================================================================
// ServerCtx — owns the registered tools, the auth table, and the
// counters. No engine, no model, no preset state.
// =============================================================================

struct ServerCtx {
    // Tool catalogue — populated once at startup, read-only thereafter.
    // Multiple worker threads read this concurrently without locking
    // because std::vector<Tool> is never mutated after main()'s setup
    // phase.
    std::vector<easyai::Tool> default_tools;

    // [MCP_USER] table: token → username for the auth gate. Populated
    // once at startup, read-only at runtime.
    std::map<std::string, std::string> mcp_keys;

    // Optional Bearer for /health, /metrics, /v1/tools (different from
    // /mcp's per-user table — single shared token, like easyai-server's
    // --api-key).
    std::string api_key;

    // Server identity surfaced on /health + MCP `initialize`.
    std::string name = "easyai-mcp-server";

    // Verbose logging mirror of args.verbose.
    bool verbose = false;

    // Counters — atomic so /metrics can scrape without holding any
    // mutex.
    std::atomic<std::uint64_t> n_requests{0};      // every JSON-RPC envelope
    std::atomic<std::uint64_t> n_errors{0};        // JSON-RPC error replies
    std::atomic<std::uint64_t> n_tool_calls{0};    // successful tools/call
    std::atomic<std::uint64_t> n_rejected{0};      // 503s from limiter saturation

    // The in-flight cap. unique_ptr because its size is set from CLI/INI
    // and we want it on the heap alongside ctx.
    std::unique_ptr<InFlightLimiter> limiter;
};

// =============================================================================
// MCP Bearer-token auth — thin wrapper around easyai::mcp::check_bearer.
//
// The matching logic (header-size cap, Bearer prefix, table lookup,
// audit-friendly username) lives in libeasyai so easyai-server and
// this binary stay in sync. Here we just bridge cpp-httplib's
// request/response objects to the lib's transport-agnostic verdict.
// =============================================================================

bool check_mcp_auth(const ServerCtx &        ctx,
                    const httplib::Request & req,
                    std::string &            user_out,
                    httplib::Response &      res) {
    auto verdict = easyai::mcp::check_bearer(
        ctx.mcp_keys, req.get_header_value("Authorization"));
    if (verdict.ok) {
        user_out = std::move(verdict.user);
        return true;
    }
    res.status = verdict.status;
    if (!verdict.www_authenticate.empty()) {
        res.set_header("WWW-Authenticate", verdict.www_authenticate);
    }
    res.set_content(verdict.body, "application/json");
    user_out.clear();
    return false;
}

// Generic Bearer auth for /health-style endpoints (single shared key —
// same as easyai-server's require_auth).
bool require_auth(const ServerCtx &        ctx,
                  const httplib::Request & req,
                  httplib::Response &      res) {
    if (ctx.api_key.empty()) return true;
    const std::string auth = req.get_header_value("Authorization");
    if (auth.size() > easyai::mcp::kMaxAuthHeaderBytes) {
        res.status = 401;
        res.set_content(
            "{\"error\":{\"message\":\"Authorization header too large\","
            "\"type\":\"authentication_error\"}}",
            "application/json");
        return false;
    }
    const std::string expected = "Bearer " + ctx.api_key;
    if (auth != expected) {
        res.status = 401;
        res.set_content(
            "{\"error\":{\"message\":\"missing or invalid Bearer token\","
            "\"type\":\"authentication_error\"}}",
            "application/json");
        return false;
    }
    return true;
}

// =============================================================================
// Routes
// =============================================================================

// POST /mcp — JSON-RPC 2.0 dispatcher.
//
// Concurrency:
//  1. Bearer auth (cheap, no resource cost) gates every request.
//  2. We peek at the JSON body to find the method without committing
//     to a full parse-then-dispatch — methods like `initialize`,
//     `tools/list`, `ping`, and `notifications/*` skip the limiter
//     because they don't spawn anything. Only `tools/call` enters the
//     limiter (and may be rejected with 503 on saturation).
//  3. easyai::mcp::handle_request runs the actual dispatch. It is a
//     pure function over `ctx.default_tools` — no global state inside
//     mcp.cpp itself. Tool handlers serialise themselves where they
//     need to (RagStore's shared_mutex, fetch_web cache mutex, the
//     bash/external-tools fork+execve runners are process-isolated).
void route_mcp(ServerCtx &              ctx,
               const httplib::Request & req,
               httplib::Response &      res) {
    std::string mcp_user;
    if (!check_mcp_auth(ctx, req, mcp_user, res)) return;

    if (!mcp_user.empty()) {
        // Audit log per request — token is never logged.
        std::fprintf(stderr,
            "[mcp] request from user '%s'\n", mcp_user.c_str());
    }

    ctx.n_requests.fetch_add(1, std::memory_order_relaxed);

    // Cheap pre-parse to identify the method. If it's tools/call, take
    // an in-flight slot before handing off to the dispatcher; otherwise
    // skip the limiter entirely so the cheap methods always succeed.
    //
    // The dispatcher itself enforces the 64-level JSON depth cap; we
    // don't duplicate that here. Failure to parse just means we don't
    // know the method — let the dispatcher produce the proper JSON-RPC
    // parse-error envelope.
    bool is_tools_call = false;
    try {
        // Peek without committing to the full parse-then-walk the
        // dispatcher does — cheap one-shot.
        json req_body = json::parse(req.body, /*cb*/ nullptr, /*allow_exceptions*/ false);
        if (req_body.is_object() && req_body.contains("method") &&
            req_body["method"].is_string()) {
            is_tools_call = (req_body["method"].get<std::string>() == "tools/call");
        }
    } catch (...) {
        // Parse failure → let handle_request produce the standard error.
        is_tools_call = false;
    }

    std::unique_ptr<InFlightGuard> guard;
    if (is_tools_call && ctx.limiter) {
        guard = std::make_unique<InFlightGuard>(*ctx.limiter);
        if (!guard->ok()) {
            ctx.n_rejected.fetch_add(1, std::memory_order_relaxed);
            res.status = 503;
            res.set_header("Retry-After", "1");
            res.set_content(
                "{\"jsonrpc\":\"2.0\",\"id\":null,"
                "\"error\":{\"code\":-32000,"
                "\"message\":\"server at concurrent-call cap; "
                "retry shortly\"}}",
                "application/json");
            return;
        }
    }

    easyai::mcp::ServerInfo info;
    info.name             = ctx.name;
    info.version          = "0.1.0";
    info.protocol_version = "2024-11-05";

    std::string body;
    try {
        body = easyai::mcp::handle_request(req.body, ctx.default_tools, info);
    } catch (const std::exception & e) {
        // handle_request is documented as never-throw, but defence in
        // depth: convert any escape into a generic JSON-RPC internal
        // error. We don't want a worker thread to die on a tool
        // handler bug.
        ctx.n_errors.fetch_add(1, std::memory_order_relaxed);
        res.status = 500;
        std::ostringstream o;
        o << "{\"jsonrpc\":\"2.0\",\"id\":null,"
             "\"error\":{\"code\":-32603,"
             "\"message\":\"internal error: ";
        // Escape minimally — JSON inside a string literal.
        for (char c : std::string(e.what())) {
            if (c == '"' || c == '\\') { o << '\\' << c; }
            else if (c == '\n') o << "\\n";
            else if (c == '\r') o << "\\r";
            else if ((unsigned char) c < 0x20) o << ' ';
            else o << c;
        }
        o << "\"}}";
        res.set_content(o.str(), "application/json");
        return;
    }

    if (body.empty()) {
        // JSON-RPC notification — no response per the spec.
        res.status = 204;
        return;
    }

    if (is_tools_call) {
        ctx.n_tool_calls.fetch_add(1, std::memory_order_relaxed);
    }

    if (ctx.verbose) {
        std::fprintf(stderr,
            "[mcp] dispatched %s (in_flight=%d)\n",
            is_tools_call ? "tools/call" : "non-call method",
            ctx.limiter ? ctx.limiter->in_flight() : 0);
    }

    res.set_content(body, "application/json");
}

// GET /health — JSON status. Always open; cheap; ops uses for liveness.
void route_health(const ServerCtx & ctx,
                  const httplib::Request &,
                  httplib::Response & res) {
    ordered_json j;
    j["status"]  = "ok";
    j["server"]  = ctx.name;
    j["tools"]   = (std::uint64_t) ctx.default_tools.size();
    j["mcp_auth"] = ctx.mcp_keys.empty() ? "open" : "required";

    ordered_json compat;
    compat["mcp"]          = "/mcp";
    compat["mcp_protocol"] = "2024-11-05";
    j["compat"] = std::move(compat);

    if (ctx.limiter) {
        ordered_json conc;
        conc["in_flight"]            = ctx.limiter->in_flight();
        conc["max_concurrent_calls"] = ctx.limiter->capacity();
        j["concurrency"] = std::move(conc);
    }

    j["counters"] = {
        {"requests",   (std::uint64_t) ctx.n_requests.load()},
        {"tool_calls", (std::uint64_t) ctx.n_tool_calls.load()},
        {"errors",     (std::uint64_t) ctx.n_errors.load()},
        {"rejected",   (std::uint64_t) ctx.n_rejected.load()},
    };

    res.set_content(j.dump(), "application/json");
}

// GET /metrics — Prometheus text exposition.
void route_metrics(const ServerCtx & ctx,
                   const httplib::Request &,
                   httplib::Response & res) {
    std::ostringstream o;
    o << "# HELP easyai_mcp_requests_total Total JSON-RPC requests received.\n"
      << "# TYPE easyai_mcp_requests_total counter\n"
      << "easyai_mcp_requests_total " << ctx.n_requests.load() << "\n"
      << "# HELP easyai_mcp_tool_calls_total Total successful tools/call dispatches.\n"
      << "# TYPE easyai_mcp_tool_calls_total counter\n"
      << "easyai_mcp_tool_calls_total " << ctx.n_tool_calls.load() << "\n"
      << "# HELP easyai_mcp_errors_total Total JSON-RPC error envelopes returned.\n"
      << "# TYPE easyai_mcp_errors_total counter\n"
      << "easyai_mcp_errors_total " << ctx.n_errors.load() << "\n"
      << "# HELP easyai_mcp_rejected_total tools/call requests rejected by concurrency cap.\n"
      << "# TYPE easyai_mcp_rejected_total counter\n"
      << "easyai_mcp_rejected_total " << ctx.n_rejected.load() << "\n";
    if (ctx.limiter) {
        o << "# HELP easyai_mcp_in_flight Tool dispatches currently in flight.\n"
          << "# TYPE easyai_mcp_in_flight gauge\n"
          << "easyai_mcp_in_flight " << ctx.limiter->in_flight() << "\n"
          << "# HELP easyai_mcp_max_concurrent_calls Configured concurrent-call cap.\n"
          << "# TYPE easyai_mcp_max_concurrent_calls gauge\n"
          << "easyai_mcp_max_concurrent_calls " << ctx.limiter->capacity() << "\n";
    }
    o << "# HELP easyai_mcp_tools_registered Total tools advertised over /mcp tools/list.\n"
      << "# TYPE easyai_mcp_tools_registered gauge\n"
      << "easyai_mcp_tools_registered " << ctx.default_tools.size() << "\n";
    res.set_content(o.str(), "text/plain; version=0.0.4");
}

// GET /v1/tools — tool catalogue (name + description) for diagnostics.
// Same shape as easyai-server's /v1/tools — useful when wiring up a
// new client and verifying which tools the operator exposed.
void route_tools(const ServerCtx & ctx,
                 const httplib::Request &,
                 httplib::Response & res) {
    ordered_json arr = ordered_json::array();
    for (const auto & t : ctx.default_tools) {
        ordered_json e;
        e["name"]        = t.name;
        e["description"] = t.description;
        arr.push_back(std::move(e));
    }
    ordered_json env;
    env["object"] = "list";
    env["data"]   = std::move(arr);
    res.set_content(env.dump(), "application/json");
}

// =============================================================================
// Signal handling — same atomic g_server pointer pattern as
// examples/server.cpp.
// =============================================================================

std::atomic<httplib::Server *> g_server{nullptr};
void on_signal(int) {
    httplib::Server * s = g_server.load();
    if (s) s->stop();
}

}  // namespace

// =============================================================================
// main
// =============================================================================

int main(int argc, char ** argv) {
    ServerArgs args = parse_args(argc, argv);

    // -------- INI overlay (CLI > INI > hardcoded) -------------------------
    easyai::config::Ini ini_config;
    {
        std::string ini_err;
        ini_config = easyai::config::load_ini_file(args.config_path, ini_err);
        if (!ini_err.empty()) {
            std::fprintf(stderr,
                "easyai-mcp-server: %s warnings:\n%s\n",
                args.config_path.c_str(), ini_err.c_str());
        }
        apply_ini_to_args(ini_config, args);
    }

    // -------- sanity-check tunables --------------------------------------
    if (args.threads < 1)              args.threads = 1;
    if (args.max_concurrent_calls < 1) args.max_concurrent_calls = 1;
    if (args.max_body == 0)            args.max_body = kDefaultMaxBody;

    // -------- chdir into sandbox so $SANDBOX in external-tools manifests
    //          resolves to the operator-chosen dir, and so get_current_dir
    //          (when allow_fs is on) reports the boundary the model is
    //          working inside.
    if (!args.sandbox.empty()) {
        if (::chdir(args.sandbox.c_str()) != 0) {
            std::fprintf(stderr,
                "easyai-mcp-server: chdir(%s): %s\n",
                args.sandbox.c_str(), std::strerror(errno));
            return 2;
        }
    }

    // -------- build the context -------------------------------------------
    auto ctx = std::make_unique<ServerCtx>();
    ctx->name    = args.name;
    ctx->api_key = args.api_key;
    ctx->verbose = args.verbose;
    ctx->limiter = std::make_unique<InFlightLimiter>(args.max_concurrent_calls);

    // -------- register the standard toolbelt ------------------------------
    //
    // Same factory as easyai-server: built-ins (datetime, unified web)
    // always; unified `fs` when --allow-fs; bash when --allow-bash.
    // Sandbox dir resolves against the cwd we chdir'd into above.
    if (args.load_tools) {
        // python3 defaults ON, but registration is gated (inside
        // Toolbelt::tools()) on sandbox-or-bash. So setting sb="."
        // when allow_python alone is on would expose ambient cwd to
        // python3 — explicitly only fall back to "." when fs / bash
        // is on (matching the historical server policy).
        std::string sb = args.sandbox;
        if (sb.empty() && (args.allow_fs || args.allow_bash)) sb = ".";
        auto tb = easyai::cli::Toolbelt()
                      .sandbox     (sb)
                      .allow_fs    (args.allow_fs)
                      .allow_bash  (args.allow_bash)
                      .allow_python(args.allow_python);
        for (auto & t : tb.tools()) ctx->default_tools.push_back(std::move(t));
    }

    // -------- knowledge ----------------------------------------------------
    // Seven single-responsibility memory tools (learning_knowledge,
    // learning_more_knowledge, search_knowledge, recall_knowledge,
    // browse_knowledge, forget_knowledge, keywords_knowledge) — each
    // surfaced as its own MCP tool.
    if (!args.rag_dir.empty()) {
        for (auto & t : easyai::tools::knowledge_split_tools(args.rag_dir)) {
            ctx->default_tools.push_back(std::move(t));
        }
        std::fprintf(stderr,
            "easyai-mcp-server: knowledge enabled (learning_knowledge, "
            "learning_more_knowledge, search_knowledge, recall_knowledge, "
            "browse_knowledge, forget_knowledge, keywords_knowledge), "
            "root = %s\n",
            args.rag_dir.c_str());
    }

    // -------- external-tools dir -----------------------------------------
    //
    // Loaded AFTER built-ins + RAG so the loader can reject any manifest
    // entry that collides with a name we already registered. Per-file
    // fault isolation: a bad file is logged and skipped, the server
    // still starts.
    if (!args.external_tools_dir.empty()) {
        std::vector<std::string> reserved;
        reserved.reserve(ctx->default_tools.size());
        for (const auto & t : ctx->default_tools) reserved.push_back(t.name);
        auto loaded = easyai::load_external_tools_from_dir(
            args.external_tools_dir, reserved);

        for (const auto & e_msg : loaded.errors) {
            std::fprintf(stderr,
                "easyai-mcp-server: [external-tools] error: %s\n",
                e_msg.c_str());
        }
        for (const auto & w : loaded.warnings) {
            std::fprintf(stderr,
                "easyai-mcp-server: [external-tools] warn: %s\n",
                w.c_str());
        }
        for (auto & t : loaded.tools) ctx->default_tools.push_back(std::move(t));
        std::fprintf(stderr,
            "easyai-mcp-server: loaded %zu external tool(s) from %zu file(s) in %s\n",
            loaded.tools.size(), loaded.loaded_files.size(),
            args.external_tools_dir.c_str());
    }

    // tool_lookup MUST be added last so its snapshot covers every
    // other tool registered above (built-ins, RAG, external).  Same
    // ctx-pointer-capture trick as easyai-server; ctx outlives every
    // request handler.
    {
        auto * tools_ptr = &ctx->default_tools;
        ctx->default_tools.push_back(easyai::tools::tool_lookup(
            [tools_ptr]() {
                easyai::tools::ToolCatalog v;
                v.reserve(tools_ptr->size());
                for (const auto & t : *tools_ptr) {
                    v.push_back({ t.name, t.wire_description(), t.description });
                }
                return v;
            }));
    }

    // -------- MCP auth user table from already-loaded INI -----------------
    //
    // Three-way precedence on the gate (matches easyai-server):
    //   * --no-mcp-auth (or [SERVER] mcp_auth = off) → force open.
    //   * [MCP_USER] populated → Bearer required.
    //   * [MCP_USER] empty → open.
    {
        ctx->mcp_keys = easyai::mcp::load_mcp_users(
            ini_config.section_or_empty("MCP_USER"));
        if (args.no_mcp_auth && !ctx->mcp_keys.empty()) {
            std::fprintf(stderr,
                "easyai-mcp-server: MCP auth OVERRIDDEN OPEN — `--no-mcp-auth` "
                "(or [SERVER] mcp_auth=off) discards %zu [MCP_USER] entry(ies)\n",
                ctx->mcp_keys.size());
            ctx->mcp_keys.clear();
        }
        if (!ctx->mcp_keys.empty()) {
            std::fprintf(stderr,
                "easyai-mcp-server: MCP auth ENABLED — %zu user(s) loaded from %s\n",
                ctx->mcp_keys.size(), args.config_path.c_str());
        } else if (!ini_config.sections.empty()) {
            std::fprintf(stderr,
                "easyai-mcp-server: MCP auth OPEN — [MCP_USER] section in %s "
                "is empty (or absent)\n", args.config_path.c_str());
        } else {
            std::fprintf(stderr,
                "easyai-mcp-server: MCP auth OPEN — no INI config at %s\n",
                args.config_path.c_str());
        }
    }

    std::fprintf(stderr,
        "[easyai-mcp-server] %s\n"
        "                tools=%zu\n"
        "                listening on http://%s:%d  (POST /mcp)\n"
        "                threads=%d  max_concurrent_calls=%d\n"
        "                api_key=%s  metrics=%s  verbose=%s\n",
        ctx->name.c_str(), ctx->default_tools.size(),
        args.host.c_str(), args.port,
        args.threads, args.max_concurrent_calls,
        ctx->api_key.empty() ? "OFF" : "ON",
        args.metrics ? "ON" : "OFF",
        args.verbose ? "ON" : "OFF");

    // -------- HTTP server -------------------------------------------------
    httplib::Server svr;
    svr.set_payload_max_length(args.max_body);
    svr.set_read_timeout (kReadTimeoutSeconds);
    svr.set_write_timeout(kWriteTimeoutSeconds);
    // Idle-keep-alive: 1 hour, decoupled from the per-request
    // slow-loris read/write timeout (30 / 60 s).  MCP clients
    // typically dispatch a few tools/call requests, then sit idle
    // waiting on their own LLM turn before issuing the next batch.
    // Holding the TCP socket open across that idle window avoids
    // server-side TIME_WAIT pile-up under load.  cpp-httplib's
    // CPPHTTPLIB_KEEPALIVE_MAX_COUNT (default 100) still bounds
    // total requests per socket, so a 1-hour idle ceiling does not
    // mean unbounded server state per client.
    constexpr int kKeepAliveTimeoutSeconds = 3600;
    svr.set_keep_alive_timeout(kKeepAliveTimeoutSeconds);

    // Custom thread pool — cpp-httplib's default is ThreadPool(8); we
    // resize via the new_task_queue factory hook so high-concurrency
    // deployments don't bottleneck on dispatch.
    const int worker_threads = args.threads;
    svr.new_task_queue = [worker_threads]() {
        return new httplib::ThreadPool(worker_threads);
    };

    // CORS — permissive (matches easyai-server). Tighten via reverse
    // proxy if exposed on a public network.
    svr.set_default_headers({
        {"Access-Control-Allow-Origin",  "*"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
    });
    svr.Options(R"(.*)", [](const httplib::Request &, httplib::Response & r) {
        r.status = 204;
    });

    auto & ctx_ref = *ctx;

    svr.Get("/health", [&](const httplib::Request & q, httplib::Response & r) {
        // /health is intentionally OPEN even when --api-key is set —
        // ops liveness probes need to reach it without a credential.
        route_health(ctx_ref, q, r);
    });

    if (args.metrics) {
        svr.Get("/metrics", [&](const httplib::Request & q, httplib::Response & r) {
            // /metrics IS gated by --api-key when set — a public-network
            // deployment shouldn't expose internal counters to anyone.
            if (!require_auth(ctx_ref, q, r)) return;
            route_metrics(ctx_ref, q, r);
        });
    }

    svr.Get("/v1/tools", [&](const httplib::Request & q, httplib::Response & r) {
        if (!require_auth(ctx_ref, q, r)) return;
        route_tools(ctx_ref, q, r);
    });

    // POST /mcp — the main event.
    svr.Post("/mcp", [&](const httplib::Request & q, httplib::Response & r) {
        route_mcp(ctx_ref, q, r);
    });
    svr.Get("/mcp", [](const httplib::Request &, httplib::Response & r) {
        r.status = 405;
        r.set_header("Allow", "POST");
        r.set_content(
            "{\"error\":\"GET /mcp is not yet implemented; "
            "use POST with a JSON-RPC 2.0 request body. "
            "Server-pushed notifications via SSE will land in a "
            "future version.\"}",
            "application/json");
    });

    // Last-chance error handler — never let a thrown exception
    // propagate out of the HTTP layer.
    svr.set_exception_handler([&](const httplib::Request &,
                                  httplib::Response & res,
                                  std::exception_ptr ep) {
        ctx_ref.n_errors.fetch_add(1, std::memory_order_relaxed);
        try { if (ep) std::rethrow_exception(ep); }
        catch (const std::exception & e) {
            res.status = 500;
            std::ostringstream o;
            o << "{\"error\":{\"message\":\"uncaught: ";
            for (char c : std::string(e.what())) {
                if (c == '"' || c == '\\') { o << '\\' << c; }
                else if (c == '\n') o << "\\n";
                else if (c == '\r') o << "\\r";
                else if ((unsigned char) c < 0x20) o << ' ';
                else o << c;
            }
            o << "\",\"type\":\"internal_error\"}}";
            res.set_content(o.str(), "application/json");
        } catch (...) {
            res.status = 500;
            res.set_content(
                "{\"error\":{\"message\":\"uncaught unknown exception\","
                "\"type\":\"internal_error\"}}",
                "application/json");
        }
    });

    g_server.store(&svr);
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    // SIGPIPE — possible on a client disconnect mid-write. Default
    // disposition kills the process; ignore so we just get an EPIPE
    // returned from write() and continue serving other workers.
    std::signal(SIGPIPE, SIG_IGN);

    const bool ok = svr.listen(args.host.c_str(), args.port);
    g_server.store(nullptr);
    std::fprintf(stderr,
        "[easyai-mcp-server] %s\n",
        ok ? "stopped cleanly" : "listen failed");
    return ok ? 0 : 1;
}
