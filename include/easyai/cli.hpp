// easyai/cli.hpp — high-level helpers for building CLIs on top of libeasyai.
//
// What's here is everything the example binaries (easyai-cli,
// easyai-cli-remote, easyai-server) ended up writing the same way three
// times: standard tool registration, opening a tee log file with a
// pid+timestamp path and a header, sandbox validation.  Lifted into the
// lib so a third-party agent can do the same in a handful of lines.
//
// None of this is mandatory.  You can still register tools by hand with
// engine.add_tool() / client.add_tool() — Toolbelt just spares you
// writing the same `if (sandbox.empty()) ... else ...` for the fifth
// time.
#pragma once

#include "tool.hpp"
#include "plan.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace easyai {
class Engine;
class Client;
}

namespace easyai::cli {

// ---------- Toolbelt: standard agent toolset, fluently configured ----------
//
// Composes the canonical "agent flavour" of the built-in tools:
//   - datetime           (always, unless no_datetime())
//   - web                (unless no_web()) — unified search + fetch tool;
//                         engine="google" only enabled when use_google()
//                         is called AND env vars are present
//   - plan tool          (when with_plan(Plan&) is called)
//   - fs                 (when sandbox(<dir>) is called or allow_bash /
//                         allow_python is on — scoped to <dir>;
//                         unified read/write/list/glob/grep/check_path/
//                         cwd/sandbox dispatcher)
//   - python3            (default ON — runs snippets via
//                         `python3 -I -S -E -c <code>`; isolated stdlib-
//                         only interpreter; disk access auto-restricted
//                         to the sandbox root via a Python preamble.
//                         Opt out with allow_python(false).)
//   - bash               (when allow_bash() is called)
//
// `apply(Engine&)` and `apply(Client&)` register the tools AND, when
// either bash or python3 is enabled, bump the agentic-loop
// max_tool_hops to 99999 (interactive subprocess flows naturally span
// far more turns than the default 8 cap allows).
//
// Example:
//     easyai::cli::Toolbelt()
//         .sandbox("/srv/data")
//         .allow_bash()
//         .with_plan(plan)
//         .apply(client);
// Tool surface mode — controls how the multi-action tools (fs, web,
// and the externally-registered memory) are exposed to the model.
//
//   Unified  — single dispatcher tool with an `action` argument (e.g.
//              `fs(action="read")`).  Smallest system-prompt footprint.
//              Best for large models that can hold the discriminated
//              union schema in their head.
//
//   Split    — one focused tool per action (`read_file`, `edit_file`, …).
//              Flat schemas, name == semantic anchor, no "unknown
//              action" failure mode.  Best for smaller / quantised
//              tool-callers (7-8B and below) — they consistently work
//              more reliably with one verb per tool.
//
//   Both     — both surfaces registered side-by-side.  Costs more
//              system-prompt tokens but lets the model pick whichever
//              shape it's more comfortable with on a per-call basis.
enum class ToolMode { Unified, Split, Both };

class Toolbelt {
public:
    Toolbelt & sandbox      (std::string dir);   // "" stays the default (no fs)
    Toolbelt & allow_fs     (bool on = true);    // gate fs registration
    Toolbelt & allow_bash   (bool on = true);
    Toolbelt & allow_python (bool on = true);    // gate python3 registration
    // Mirror the bash subprocess's merged stdout+stderr to the parent's
    // stderr in real time. The model still receives the full captured
    // buffer as the tool result; this is a parallel diagnostic channel
    // for the operator. Off by default to keep stderr clean for callers
    // that haven't opted in.
    Toolbelt & show_bash    (bool on = true);
    // Same diagnostic mirror for the `python3` tool. Independent of
    // show_bash so operators can quiet one without losing the other.
    Toolbelt & show_python  (bool on = true);
    Toolbelt & with_plan    (Plan & plan);
    Toolbelt & no_web       (bool on = true);    // drop the web tool
    Toolbelt & no_datetime  (bool on = true);    // drop datetime

    // Opt-in: enable engine="google" inside the unified `web` tool.
    // Off by default — Google Custom Search needs GOOGLE_API_KEY +
    // GOOGLE_CSE_ID env vars and counts against a quota, so we don't
    // expose it unless the caller asks. Even when on, engine="google"
    // is only accepted at registration if both env vars are present at
    // apply()-time. The tool itself re-reads the env at call time so a
    // key rotation mid-session surfaces a clear error rather than
    // silent disappearance.
    Toolbelt & use_google   (bool on = true);

    // How fs / web are exposed to the model (Unified | Split | Both).
    // Default Unified preserves the legacy registration shape; switch
    // to Split or Both via this knob, or via the CLI's --tools-mode
    // flag / `[cli] tools_mode` INI key.
    Toolbelt & tool_mode    (ToolMode m);

    // Materialise the configured tool list.  Order is the canonical
    // one shown above; callers can append their own tools after.
    std::vector<Tool> tools() const;

    // Convenience: register the tools onto an Engine / Client and
    // (if bash is enabled) bump max_tool_hops to 99999.
    void apply(Engine & engine) const;
    void apply(Client & client) const;

    // Inspectors — handy when callers want to drive their own help
    // text or banner ("registered N tools, sandbox=<dir>, bash=on").
    const std::string & sandbox_dir() const { return sandbox_; }
    bool                bash_on   () const { return allow_bash_; }
    bool                python_on () const { return allow_python_; }

private:
    std::string sandbox_;
    // allow_fs_ defaults TRUE so callers that pre-date this flag (Agent,
    // backend.cpp, examples/cli.cpp) keep the legacy "sandbox dir auto-
    // enables *_file" behaviour.  Server flips it OFF unless --allow-fs.
    bool        allow_fs_     = true;
    bool        allow_bash_   = false;
    // python3 defaults ON: a stdlib-only interpreter with the disk
    // surface auto-restricted to the sandbox root (preamble in
    // builtin_tools.cpp). Operators who don't want any subprocess
    // executor at all can flip this off via .allow_python(false).
    bool        allow_python_ = true;
    bool        show_bash_    = false;
    bool        show_python_  = false;
    bool        no_web_       = false;
    bool        no_datetime_  = false;
    bool        use_google_   = false;
    // Default Split: one focused tool per action. Most real-world
    // tool-callers (small models in particular, but also large ones)
    // dispatch more reliably against flat one-verb-per-tool schemas
    // than against a discriminated `action`-string union. Operators
    // who want the historical single-dispatcher surface can opt back
    // in with .tool_mode(ToolMode::Unified) or --tools-mode unified.
    //
    // Note (2026-05-26): the `fs(action="ops")` batch (50 ops / 20
    // files per call) lives on the UNIFIED `fs` surface — opt in
    // with `.tool_mode(ToolMode::Unified)` or `--tools-mode unified`
    // when you want to drive multi-op edits from one tool call.
    ToolMode    tool_mode_    = ToolMode::Split;
    Plan *      plan_         = nullptr;
};

// ---------- Log file helper ------------------------------------------------
//
// Opens a tee log file at `path` (or auto-picks /tmp/<prefix>-<pid>-<epoch>.log
// if path is empty), writes a header line listing argv, and registers the
// FILE* with easyai::log so subsequent `easyai::log::write()` calls tee
// into it.  Returns the FILE* (still owned by the caller — fclose it,
// then call easyai::log::set_file(nullptr)).  Returns nullptr on failure.
//
// `prefix` controls the auto-path basename.  argc/argv are recorded in
// the header so a stray log file is self-describing.
std::FILE * open_log_tee(const std::string & path,
                         const std::string & prefix,
                         int argc, char ** argv,
                         std::string * resolved_path = nullptr);

// Pair to open_log_tee: writes a "END OF LOG" footer, fcloses, and
// clears the easyai::log sink.  Safe to call on nullptr.
void close_log_tee(std::FILE * fp);

// ---------- Client introspection helpers -----------------------------------

// Returns true if the Client has a tool with the given name registered
// (matches Tool::name).  Inline so consumers don't take the
// libeasyai-cli link dep just for this lookup.
bool client_has_tool(const Client & client, const std::string & name);


// ---------- Management subcommands -----------------------------------------
//
// Standard "introspect / drive a remote easyai-server" commands lifted
// from cli_remote's run_management() so any consumer of libeasyai-cli
// can ship the same toolbelt.  Each returns a process exit code
// (0 on success, non-zero on transport / parse failure) and writes
// the human-readable result to `out`.
}  // namespace easyai::cli
namespace easyai::ui { struct Style; }
namespace easyai::cli {

// /v1/models — list everything the server is willing to serve.
int print_models       (Client & client, const ui::Style & st, std::FILE * out = stdout);
// Locally-registered tools (what we send to the model in the
// request body's tools[]).
int print_local_tools  (Client & client, const ui::Style & st, std::FILE * out = stdout);
// /v1/tools — easyai-server extension: catalogue of tools the
// server registered.
int print_remote_tools (Client & client, const ui::Style & st, std::FILE * out = stdout);
// /health — boolean check (prints "ok" / "unhealthy: <reason>").
int print_health       (Client & client, const ui::Style & st, std::FILE * out = stdout);
// /props — dump the server's props JSON.
int print_props        (Client & client, std::FILE * out = stdout);
// /metrics — dump Prometheus exposition.
int print_metrics      (Client & client, std::FILE * out = stdout);
// /v1/preset — set the server's ambient sampling preset.
int set_preset         (Client & client, const std::string & name,
                        const ui::Style & st, std::FILE * out = stdout);


// ---------- Sandbox dir validation -----------------------------------------
//
// Returns true if `path` is empty (caller didn't pass --sandbox) or
// names an existing directory.  Returns false (with `err` set to a
// human-readable reason) otherwise.  Lifts the duplicate "does it
// exist? is it a dir?" block out of every CLI's main().
bool validate_sandbox(const std::string & path, std::string & err);

}  // namespace easyai::cli
