// examples/cli_remote.cpp — full agentic OpenAI-protocol CLI built on
// libeasyai-cli.  Talks to any /v1/chat/completions endpoint (our
// easyai-server, llama-server, OpenAI itself).  Tools execute LOCALLY
// in this process — the model picks which tool to call, the Client
// dispatches it, and the result is fed back into the conversation.
//
// Modes:
//   easyai-cli-remote --url URL [-p PROMPT]        one-shot (exits after)
//   easyai-cli-remote --url URL                    interactive REPL
//   easyai-cli-remote --url URL --shell             hybrid AI shell
//   easyai-cli-remote --url URL --list-models      management subcommand
//   easyai-cli-remote --url URL --list-tools       management subcommand
//   easyai-cli-remote --url URL --health           management subcommand
//   easyai-cli-remote --url URL --props            management subcommand
//   easyai-cli-remote --url URL --metrics          management subcommand
//   easyai-cli-remote --url URL --set-preset NAME  management subcommand
//
// Built-in tools (off by default in the model's choice list — supplied so
// it CAN call them):
//   datetime, plan          (always)
//   web                     (unified search + fetch; needs libcurl at build
//                            time — runtime check returns an error if not)
//   fs                      (unified read / write / list / glob / grep /
//                            check_path / cwd / sandbox dispatcher; only
//                            when --sandbox DIR is given; root scoped)
//
// REPL specials:
//   /exit, /quit       leave
//   /clear             clear conversation history (keep tools + system)
//   /reset             clear history AND plan
//   /compress          ask the model for a lossless recap of the session,
//                      replace history with the recap, save .easyai_session
//   /plan              re-render the plan checklist
//   /tools             list registered tools and their descriptions
//
// Session persistence: every invocation writes .easyai_session in cwd
// after each turn.  --continue resumes the last session in this dir;
// --continue --compress resumes AND recaps before the first prompt.
//
// Configuration is layered: CLI flags > env vars > defaults.  Env vars:
//   EASYAI_URL, EASYAI_API_KEY, EASYAI_MODEL.
//
// Output styling: ANSI dim for reasoning_content, cyan for tool-call
// indicators, yellow for plan checklist updates, bold for final answer.
// Auto-disabled when stdout is not a TTY.

#include "easyai/builtin_tools.hpp"
#include "easyai/cli.hpp"
#include "easyai/client.hpp"
#include "easyai/config.hpp"
#include "easyai/external_tools.hpp"
#include "easyai/log.hpp"
#include "easyai/plan.hpp"
#include "easyai/preamble.hpp"
#include "easyai/rag_tools.hpp"
#include "easyai/text.hpp"
#include "easyai/tool.hpp"
#include "easyai/ui.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits.h>      // PATH_MAX (Linux: not pulled in transitively)
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>    // waitpid
#include <fcntl.h>       // open / O_* flags for atomic session write
#include <unistd.h>      // getpid, write, close
#include <vector>

namespace {

using easyai::ui::Style;
using easyai::ui::Spinner;
using easyai::ui::StreamStats;

// Shorthand: easyai::log::write tees stderr + the optional --log-file FILE.
// vlog(...) is just the historical name we kept for in-file readability.
inline void vlog(const char * fmt, ...) __attribute__((format(printf, 1, 2)));
inline void vlog(const char * fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    // libstdc++ has no public va_list overload of write(); just expand
    // through a small buffer.  Logging volume is low (per-hop summaries),
    // so this is fine.
    char buf[4096];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    easyai::log::write("%s", buf);
}

// ===========================================================================
// Session persistence — `.easyai_session` in cwd
// ---------------------------------------------------------------------------
// Every easyai-cli invocation drops a `.easyai_session` file in the
// current working directory, atomically updated after each chat() turn
// and after every history-mutating slash command (/clear, /reset,
// /compress).  The file is the raw JSON array produced by
// Client::dump_history() — OpenAI-shape messages, no envelope.
//
//   easyai-cli                  -> fresh history, save on every turn
//   easyai-cli --continue       -> load existing .easyai_session first
//                                  (warn if none; start fresh)
//   easyai-cli --continue \
//              --compress       -> load, ask the model for a lossless
//                                  recap of the conversation, replace
//                                  history with the recap, save
//   /compress  (inside the REPL) -> same compress flow, run mid-session
// ===========================================================================
constexpr const char * kSessionFileName = "easyai-session.json";

// Canonical default is `.easyai_session` (hidden, dot-prefixed) in
// cwd. `--session-file <name>` overrides this:
//   * absolute path → used as-is (e.g. `/var/lib/easyai/preset.json`)
//   * relative path → resolved against cwd (e.g. `mysession.json`)
//   * empty string  → fall back to `.easyai_session`
// Lets operators keep backups of pre-baked sessions outside the
// working directory and feed them as scheduled-job seed contexts.
inline std::filesystem::path session_file_path(const std::string & override_name = "") {
    if (!override_name.empty()) {
        std::filesystem::path p(override_name);
        if (p.is_absolute()) return p;
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (ec) cwd = std::filesystem::path(".");
        return cwd / p;
    }
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) cwd = std::filesystem::path(".");
    return cwd / ".easyai_session";
}

// Atomic write: tempfile + rename(2).  O_NOFOLLOW so a planted symlink
// at the session path doesn't redirect us out of cwd; mode 0600 because
// the file echoes prompts, tool results, and reasoning content (which
// can contain secrets, API keys mentioned in passing, or private notes).
bool save_session(const easyai::Client & cli,
                  const std::string & override_name = "",
                  std::string * err = nullptr) {
    const std::string body = cli.dump_history();
    const auto target = session_file_path(override_name);
    const std::string tmp = target.string() + ".tmp";

    int fd = ::open(tmp.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                    0600);
    if (fd < 0) {
        if (err) *err = std::string("open .easyai_session.tmp: ")
                       + std::strerror(errno);
        return false;
    }
    const char * data = body.data();
    size_t       left = body.size();
    while (left > 0) {
        ssize_t n = ::write(fd, data, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(tmp.c_str());
            if (err) *err = std::string("write .easyai_session.tmp: ")
                           + std::strerror(errno);
            return false;
        }
        data += n;
        left -= (size_t) n;
    }
    ::close(fd);
    if (::rename(tmp.c_str(), target.c_str()) != 0) {
        const int e = errno;
        ::unlink(tmp.c_str());
        if (err) *err = std::string("rename .easyai_session.tmp: ")
                       + std::strerror(e);
        return false;
    }
    return true;
}

// Returns true on success.  Sets `*err` on failure (and on the
// "session file missing" case, which the caller treats as informational
// when --continue was passed against an unprimed cwd).
bool load_session(easyai::Client & cli,
                  const std::string & override_name = "",
                  std::string * err = nullptr) {
    const auto target = session_file_path(override_name);
    std::ifstream f(target);
    if (!f) {
        if (err) *err = "no session file at " + target.string();
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return cli.load_history(ss.str(), err);
}

// The compress prompt is the entire instruction we hand to the model
// for the summarise-this-conversation turn.  Spelled out deliberately —
// "lossless" is aspirational (no LLM is truly lossless), but the
// constraint list forces the model toward density over polish and toward
// preserving every facet that future turns might lean on.
constexpr const char * kCompressPrompt =
    "Summarize this entire conversation as densely as possible without "
    "losing information needed for continuation.  Preserve verbatim: "
    "every file path mentioned, every decision made, every code change "
    "applied, every error encountered with its cause, every tool result "
    "that may still be relevant, every user constraint or stated "
    "preference.  Strip: pleasantries, abandoned exploratory branches "
    "that were superseded, retries of the same query.  Output ONE "
    "markdown block, no preamble, no closing remarks — just the dense "
    "summary, ordered by topic.  Do NOT call any tool — reply with the "
    "summary text only.";

// Runs the compress flow against the Client's current history.  Returns
// true on success; on failure prints a diagnostic and leaves history
// untouched.  Caller is responsible for save_session() after.
//
// No spinner — the compress turn can take 30–90 s on a long
// conversation, but there's no per-turn streaming wiring at the
// callsites that invoke this (startup --compress runs before the REPL
// is up; /compress runs between turns where the per-run_one Spinner is
// torn down).  A simple "compressing... done" pair on stderr is the
// best we can do without plumbing a fresh Streaming/Spinner instance.
bool do_compress(easyai::Client & cli, const Style & st) {
    // Empty history → nothing to compress.  Return false so callers can
    // refrain from saving a no-op state.
    const std::string before = cli.dump_history();
    if (before == "[]") {
        std::fprintf(stderr,
            "%scompress:%s history is empty — nothing to compress.\n",
            st.yellow(), st.reset());
        return false;
    }

    std::fprintf(stderr, "%scompressing session...%s\n",
                 st.dim(), st.reset());
    std::fflush(stderr);

    const std::string summary = cli.chat(kCompressPrompt);

    if (summary.empty() || !cli.last_error().empty()) {
        std::fprintf(stderr, "%scompress failed:%s %s\n",
                     st.red(), st.reset(),
                     cli.last_error().empty() ? "empty reply"
                                               : cli.last_error().c_str());
        // Restore prior history so the failed compress turn doesn't
        // leave a half-mutated state behind.
        std::string ignored;
        cli.load_history(before, &ignored);
        return false;
    }

    // Replace the entire history with a synthetic user / assistant pair.
    // user → "Previous conversation summarised below; continue from here."
    // assistant → the model's own summary.
    // This shape works for every chat template (single user/assistant
    // turn looks like a normal exchange to the model) and ensures the
    // first thing the next turn sees is the recap, not the original
    // long history.
    std::string compressed_array;
    {
        compressed_array.reserve(summary.size() + 256);
        // Hand-built JSON — small, two messages, no need to pull a JSON
        // dependency into the CLI for this one spot.  We escape backslash
        // and double-quote on the values.
        auto json_escape = [](const std::string & s) {
            std::string out;
            out.reserve(s.size() + 16);
            for (char c : s) {
                switch (c) {
                    case '\\': out += "\\\\"; break;
                    case '"':  out += "\\\""; break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:
                        if ((unsigned char) c < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf),
                                          "\\u%04x", (unsigned) c);
                            out += buf;
                        } else {
                            out += c;
                        }
                }
            }
            return out;
        };
        compressed_array =
            "[\n"
            "  {\"role\":\"user\",\"content\":\"Previous conversation "
            "summarised below; continue from here.\"},\n"
            "  {\"role\":\"assistant\",\"content\":\""
              + json_escape(summary) + "\"}\n"
            "]";
    }

    std::string lh_err;
    if (!cli.load_history(compressed_array, &lh_err)) {
        std::fprintf(stderr,
            "%scompress load_history failed:%s %s — keeping original "
            "history.\n",
            st.red(), st.reset(), lh_err.c_str());
        std::string ignored;
        cli.load_history(before, &ignored);
        return false;
    }

    std::fprintf(stderr,
        "%scompressed.%s recap is %zu chars; original history "
        "(%zu chars) replaced.\n",
        st.dim(), st.reset(), summary.size(), before.size());
    return true;
}


// ===========================================================================
// Inline system-info tools — demonstrate how to wire your own custom
// Tool right inside the CLI.  All four are Linux-specific (read /proc),
// they return a clear "Linux only" error on macOS / *BSD.  Hooking
// these into the model gives it observability over the host running
// the agent: "is this box paging?", "is one core saturated?", etc.
//
// Cookbook for adding your own:
//   1. Build an easyai::Tool with Tool::builder("name").describe(...)
//      .param(...).handle([](const ToolCall &){ ... }).build()
//   2. Pass it to cli.add_tool().  That's it.
//
// The model sees `name` + `description` + `parameters` (auto-generated
// from .param() calls) and decides when to call it.  Your handler runs
// in this process when the model invokes it; whatever you return as
// ToolResult::ok(text) becomes the tool message the model sees next.
// ===========================================================================

namespace systools {

// ---- /proc parsing helpers ------------------------------------------------
using easyai::text::slurp_file;

// Parse "key: NUM unit\n" lines into a kB-valued map (kB is the unit
// /proc/meminfo always uses, despite the "kB" suffix).
std::map<std::string, long long> parse_proc_meminfo(const std::string & text) {
    std::map<std::string, long long> kv;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key  = line.substr(0, colon);
        std::string rest = line.substr(colon + 1);
        try { kv[key] = std::stoll(rest); }
        catch (...) { /* skip malformed line */ }
    }
    return kv;
}

// Per-cpu cumulative ticks from a single /proc/stat line.
struct CpuTicks {
    std::string  label;     // "cpu", "cpu0", "cpu1", …
    long long    user      = 0;
    long long    nice      = 0;
    long long    system    = 0;
    long long    idle      = 0;
    long long    iowait    = 0;
    long long    irq       = 0;
    long long    softirq   = 0;
    long long    steal     = 0;
    long long    total() const { return user + nice + system + idle + iowait + irq + softirq + steal; }
    long long    busy()  const { return total() - idle - iowait; }
};

std::vector<CpuTicks> parse_proc_stat(const std::string & text) {
    std::vector<CpuTicks> out;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.size() < 3 || line.compare(0, 3, "cpu") != 0) continue;
        CpuTicks t;
        std::stringstream ls(line);
        ls >> t.label
           >> t.user >> t.nice >> t.system >> t.idle
           >> t.iowait >> t.irq >> t.softirq >> t.steal;
        out.push_back(t);
    }
    return out;
}

// ---- Tool factories -------------------------------------------------------
easyai::Tool make_system_meminfo() {
    return easyai::Tool::builder("system_meminfo")
        .describe("Return total / available / free / buffers / cached memory and "
                  "swap totals from /proc/meminfo, in MiB.  Linux only.  No args.")
        .handle([](const easyai::ToolCall &) -> easyai::ToolResult {
            std::string raw;
            if (!slurp_file("/proc/meminfo", raw))
                return easyai::ToolResult::error("/proc/meminfo unreadable (Linux only)");
            auto kv = parse_proc_meminfo(raw);
            auto get = [&](const char * k) -> long long {
                auto it = kv.find(k); return it == kv.end() ? 0 : it->second;
            };
            std::ostringstream out;
            out << "Memory (MiB):\n"
                << "  Total:     " << get("MemTotal")     / 1024 << "\n"
                << "  Available: " << get("MemAvailable") / 1024 << "\n"
                << "  Free:      " << get("MemFree")      / 1024 << "\n"
                << "  Buffers:   " << get("Buffers")      / 1024 << "\n"
                << "  Cached:    " << get("Cached")       / 1024 << "\n"
                << "  Used (total - available): "
                <<     (get("MemTotal") - get("MemAvailable")) / 1024 << "\n"
                << "Swap (MiB):\n"
                << "  Total: " << get("SwapTotal") / 1024 << "\n"
                << "  Free:  " << get("SwapFree")  / 1024 << "\n"
                << "  Used:  " << (get("SwapTotal") - get("SwapFree")) / 1024 << "\n";
            return easyai::ToolResult::ok(out.str());
        }).build();
}

easyai::Tool make_system_loadavg() {
    return easyai::Tool::builder("system_loadavg")
        .describe("Return the 1, 5 and 15 minute load averages plus the "
                  "running/total process counter from /proc/loadavg.  Linux only.")
        .handle([](const easyai::ToolCall &) -> easyai::ToolResult {
            std::string raw;
            if (!slurp_file("/proc/loadavg", raw))
                return easyai::ToolResult::error("/proc/loadavg unreadable (Linux only)");
            float l1 = 0, l5 = 0, l15 = 0;
            char  procs[64] = {0};
            int   last_pid  = 0;
            std::sscanf(raw.c_str(), "%f %f %f %63s %d",
                        &l1, &l5, &l15, procs, &last_pid);
            std::ostringstream out;
            out << "Load average:\n"
                << "  1m:  " << l1  << "\n"
                << "  5m:  " << l5  << "\n"
                << "  15m: " << l15 << "\n"
                << "  running/total: " << procs << "\n"
                << "  last pid:      " << last_pid << "\n";
            return easyai::ToolResult::ok(out.str());
        }).build();
}

easyai::Tool make_system_cpu_usage() {
    return easyai::Tool::builder("system_cpu_usage")
        .describe("Sample /proc/stat twice with a configurable gap and report "
                  "per-CPU busy% (1.0 = 100% saturated).  Useful when the "
                  "user asks 'how loaded is the box right now'.  Linux only.")
        .param("sample_ms", "integer",
               "Window between samples in milliseconds.  Default 200, max 2000.",
               false)
        .handle([](const easyai::ToolCall & call) -> easyai::ToolResult {
            long long sample = easyai::args::get_int_or(
                call.arguments_json, "sample_ms", 200);
            if (sample < 50)   sample = 50;
            if (sample > 2000) sample = 2000;

            std::string a;
            if (!slurp_file("/proc/stat", a))
                return easyai::ToolResult::error("/proc/stat unreadable (Linux only)");
            std::this_thread::sleep_for(std::chrono::milliseconds(sample));
            std::string b;
            if (!slurp_file("/proc/stat", b))
                return easyai::ToolResult::error("/proc/stat unreadable on second sample");

            auto va = parse_proc_stat(a);
            auto vb = parse_proc_stat(b);
            // Index by label so we don't rely on order.
            std::map<std::string, CpuTicks> by_label;
            for (const auto & c : va) by_label[c.label] = c;

            std::ostringstream out;
            out << "CPU usage over " << sample << " ms:\n";
            for (const auto & y : vb) {
                auto it = by_label.find(y.label);
                if (it == by_label.end()) continue;
                long long dt   = y.total() - it->second.total();
                long long dbsy = y.busy()  - it->second.busy();
                if (dt <= 0) continue;
                double pct = double(dbsy) / double(dt);
                out << "  " << y.label << ": "
                    << int(pct * 100.0 + 0.5) << "%\n";
            }
            return easyai::ToolResult::ok(out.str());
        }).build();
}

easyai::Tool make_system_swaps() {
    return easyai::Tool::builder("system_swaps")
        .describe("List configured swap devices/files with size and used "
                  "amount, from /proc/swaps.  Linux only.")
        .handle([](const easyai::ToolCall &) -> easyai::ToolResult {
            std::string raw;
            if (!slurp_file("/proc/swaps", raw))
                return easyai::ToolResult::error("/proc/swaps unreadable (Linux only)");
            return easyai::ToolResult::ok(raw);
        }).build();
}

}  // namespace systools

// ---- options + parsing ----------------------------------------------------
struct Options {
    std::string url;
    std::string api_key;
    std::string model = "EasyAi";
    std::string system_prompt;
    std::string system_file;
    std::string sandbox;
    bool        allow_bash      = false;       // opt-in: register `bash` tool
    // python3 defaults ON when --sandbox is set (or --allow-bash is on).
    // It ships a stdlib-only interpreter with disk access mechanically
    // restricted to the sandbox root — the cwd Python is chdir'd into.
    // --no-python opts out entirely.
    bool        allow_python    = true;
    // show_bash / show_python: mirror the subprocess's merged
    // stdout+stderr to the parent's stderr in real time so the operator
    // can watch a long-running build / test / computation scroll by.
    // Default ON when the matching --allow-* is given; --no-show-bash /
    // --no-show-python opts out (or set show_bash=false /
    // show_python=false in the INI's [cli] section).
    bool        show_bash       = true;
    bool        show_python     = true;
    bool        use_google      = false;       // opt-in: enable engine="google"
                                                // inside the `web` tool (needs
                                                // GOOGLE_API_KEY + GOOGLE_CSE_ID
                                                // env vars too)
    std::set<std::string> tools_enabled;       // empty = all defaults
    std::string external_tools_dir;            // dir of EASYAI-*.tools files
    std::string rag_dir;                        // optional RAG persistent-registry dir
    // Default "split": focused one-verb-per-tool surfaces (fs_read,
    // fs_edit, memory_save, …) instead of the legacy single dispatcher
    // (fs(action="read"), …). Smaller / quantised tool-callers
    // dispatch much more reliably against the split shape; large
    // models handle either. Pass --tools-mode unified to opt back into
    // the legacy single-dispatcher registration.
    std::string tools_mode = "split";          // "unified" | "split" | "both"
    bool        tools_mode_cli_set = false;
    std::string prompt;                        // -p one-shot
    // Sampling / penalty knobs — -1 / -2 / empty == server default.
    float                    temperature       = -1.0f;
    float                    top_p             = -1.0f;
    int                      top_k             = -1;
    float                    min_p             = -1.0f;
    // 1.15 by default to break thinking-model rephrasing loops
    // ("I'll write types.h / Let me write types.h / OK, creating
    // types.h" repeated). Pass --repeat-penalty 1.0 to disable.
    float                    repeat_penalty    = 1.15f;
    float                    frequency_penalty = -2.0f;
    float                    presence_penalty  = -2.0f;
    long long                seed              = -1;
    int                      max_tokens        = -1;
    std::vector<std::string> stop_sequences;
    std::string              extra_body;       // JSON object literal
    int                      timeout           = 86400;  // 24 hours — multi-hour agentic sessions
    int                      http_retries      = 5;     // extra attempts on transient HTTP fails
    // Per-turn tool-hop ceiling.  Library default is 8 (safe for thin
    // embedders); the cli binary unconditionally lifts it to
    // effectively-unlimited because the cli is the agentic surface —
    // a paper-review / refactor / build-and-test session routinely
    // does dozens of tool calls per turn before it converges, and a
    // hard cap of 8 silently truncates real work mid-task.  Per-tool
    // timeouts, HTTP retry budgets, and retry_on_incomplete still
    // bound runaway behaviour.  --max-tool-hops N to put a finite cap
    // back; 0 / negative resolve to the same effectively-unlimited
    // ceiling.
    int                      max_tool_hops     = 99999;
    bool        show_reasoning   = true;   // default ON; --no-reasoning to opt out
    bool        verbose          = false;
    bool        quiet            = false;  // --quiet/-q: disable spinner + ctx-% gauge
                                            // (batch / scripted / service usage)
    std::string log_file_path;             // explicit --log-file override

    // Session persistence — drop a `.easyai_session` in cwd updated after
    // every turn.  Loading is DEFAULT-OFF: even if a session file exists
    // in cwd, we start fresh (and overwrite it on the first turn) unless
    // `--continue` (or [cli] auto_continue = on) is set.  `--compress`
    // (or [cli] auto_compress = on) runs a lossless recap on load and
    // therefore implies --continue at use time.
    bool        auto_continue       = false;    // load .easyai_session only when explicitly enabled
    bool        auto_continue_cli_set = false;  // CLI > INI > hardcoded default
    bool        auto_compress       = false;    // recap on every load when on
    bool        auto_compress_cli_set = false;
    // --session-file <name>: override the default `.easyai_session`
    // file name (or full path). Empty → use canonical default. When
    // non-empty, implies --continue (load this file on startup) so
    // the operator's "pre-baked session as scheduled-job input" use
    // case works in one flag.
    std::string session_file;
    // --no-local-session: skip every save_session() call. The session
    // file is read on load (if --continue / --session-file is set)
    // but never written back. Designed for read-only pre-session
    // seeding — the caller wants the prepared context as input but
    // doesn't want this run mutating the file.
    bool        no_local_session    = false;
    bool        log_file_path_cli_set = false;  // tracks --log-file vs INI log_file
    bool        auto_log            = false;    // legacy /tmp auto-log: opt-in via INI
    int         max_reasoning    = 0;      // 0 = unlimited (disable runaway abort)
    bool        retry_on_incomplete = true;    // matches libeasyai-cli default; --no-retry-on-incomplete to opt out
    bool        no_plan          = false;     // skip auto-registering Plan
    bool        tls_insecure     = false;     // skip peer cert verification
    std::string tls_ca_path;                  // PEM bundle for custom CAs

    // Management subcommands (mutually exclusive with prompt mode).
    bool        list_models       = false;
    bool        list_tools        = false;    // local tools (registered in this process)
    bool        list_remote_tools = false;    // server tools (GET /v1/tools)
    bool        health            = false;
    bool        props             = false;
    bool        metrics           = false;
    std::string set_preset;
    bool        show_system_prompt = false;   // print resolved prompt and exit
    // unattended: tells the model there is no human at the terminal —
    // it cannot ask clarifying questions, request approval, or present
    // a numbered choice and wait. Auto-set when a prompt is given on
    // the command line (one-shot mode, including `-p`, positional arg,
    // and stdin pipe); --unattended forces it on regardless.
    bool        unattended       = false;

    // --shell: hybrid AI shell. Normal commands execute via the user's
    // $SHELL. Lines prefixed with > are sent to the AI model. CWD and
    // env vars persist across commands via builtin cd/export handling.
    bool        shell_mode       = false;
    bool        shell_mode_cli_set = false;

    // INI overlay (CLI > INI > hardcoded). Default is resolved at load
    // time via a layered lookup: $HOME/.easyai/easyai-cli.ini first
    // (per-user, the common case — easyai-cli runs as your user, not a
    // service), /etc/easyai/easyai-cli.ini as fallback (system-wide).
    // --config <path> bypasses the layers and pins one file. An empty
    // resolved path means "no INI found" and the CLI runs on defaults.
    std::string config_path;            // populated by resolve_config_path()
    bool        config_path_cli_set = false;  // true when --config was given
    // Whether show_bash / show_python were explicitly set by the user
    // on the command line — distinguishes "user wants the default"
    // from "user passed --no-show-*" so the INI can fill the gap when
    // CLI is silent.
    bool        show_bash_cli_set   = false;
    bool        show_python_cli_set = false;

    // ---- CLI > INI > hardcoded tracking bits ------------------------
    // Set true the moment the matching flag is seen on argv so the INI
    // overlay knows not to clobber an explicit operator choice. Only
    // flags whose default value isn't a sentinel (e.g. timeout=86400,
    // model="EasyAi") need a tracking bit; sampling knobs use their
    // sentinel (-1.0f / -2.0f / -1) instead.
    bool        url_cli_set            = false;
    bool        api_key_cli_set        = false;
    bool        model_cli_set          = false;
    bool        timeout_cli_set        = false;
    bool        http_retries_cli_set   = false;
    bool        max_tool_hops_cli_set  = false;
    bool        system_prompt_cli_set  = false;
    bool        system_file_cli_set    = false;
    bool        sandbox_cli_set        = false;
    bool        allow_bash_cli_set     = false;
    bool        allow_python_cli_set   = false;
    bool        use_google_cli_set     = false;
    bool        external_tools_cli_set = false;
    bool        rag_dir_cli_set        = false;
    bool        max_reasoning_cli_set  = false;
    bool        show_reasoning_cli_set = false;
    bool        retry_on_incomplete_cli_set = false;
    bool        no_plan_cli_set        = false;
    bool        verbose_cli_set        = false;
    bool        quiet_cli_set          = false;
    bool        tls_insecure_cli_set   = false;
    bool        tls_ca_path_cli_set    = false;
    bool        session_file_cli_set   = false;
    bool        no_local_session_cli_set = false;
    bool        unattended_cli_set     = false;
    bool        tools_enabled_cli_set  = false;  // --tools was passed
    bool        stop_cli_set           = false;  // any --stop seen
    bool        extra_body_cli_set     = false;
};

void usage(const char * argv0) {
    std::fprintf(stderr,
"Usage: %s [options]\n"
"\n"
"  Connection (env fallback in parens):\n"
"    --url URL                  OpenAI-compat endpoint (EASYAI_URL)\n"
"    --api-key KEY              Bearer auth (EASYAI_API_KEY)\n"
"    --model NAME               request body 'model' field (EASYAI_MODEL)\n"
"    --timeout SECONDS          read+write timeout (default 86400 = 24h,\n"
"                                sized for multi-hour agentic sessions —\n"
"                                Tetris-from-scratch, codebase rewrites,\n"
"                                multi-source research). The timer only\n"
"                                fires on TRUE silence; every SSE delta\n"
"                                resets it. Set lower for short turns.\n"
"                                EASYAI_TIMEOUT env also accepted.\n"
"    --http-retries N           extra attempts on transient HTTP failures\n"
"                                (connect refused, read timeout, 5xx) per\n"
"                                request. 0 disables. Default 5. Each retry\n"
"                                logs to stderr. EASYAI_HTTP_RETRIES env\n"
"                                also accepted.\n"
"    --max-tool-hops N          per-turn ceiling on tool calls before the\n"
"                                agentic loop bails with `max tool hops\n"
"                                exceeded`. Default effectively unlimited\n"
"                                (99999) — agentic tasks routinely need 10+\n"
"                                hops. Set a finite N to put a hard cap\n"
"                                back; 0 / negative resolve to unlimited.\n"
"    --insecure-tls             skip peer cert check (https only — DEV ONLY)\n"
"    --ca-cert PATH             trust this CA bundle (PEM) for https://\n"
"\n"
"  Conversation shape:\n"
"    --system TEXT              system prompt as inline string\n"
"    --system-file PATH         system prompt from a file\n"
"\n"
"  Sampling / penalty (omit any to keep the server default):\n"
"    --temperature F            sampling temperature\n"
"    --top-p F                  nucleus top-p\n"
"    --top-k N                  top-k cutoff\n"
"    --min-p F                  llama-server / easyai min-p\n"
"    --repeat-penalty F         repetition penalty (default 1.15 — anti-loop\n"
"                                safety net for thinking models). Pass 1.0\n"
"                                to disable.\n"
"    --frequency-penalty F      OpenAI standard ([-2.0, 2.0])\n"
"    --presence-penalty F       OpenAI standard ([-2.0, 2.0])\n"
"    --seed N                   deterministic sampling seed\n"
"    --max-tokens N             cap reply length\n"
"    --stop SEQ                 add a stop string (repeatable)\n"
"    --extra-json '{...}'       free-form JSON merged into the request body\n"
"\n"
"  Tools:\n"
"    --tools LIST               comma list, valid names:\n"
"                                 datetime, plan, web (unified search+fetch),\n"
"                                 fs (unified file work; auto-on with\n"
"                                     --sandbox / --allow-bash),\n"
"                                 python3 (auto-on with --sandbox /\n"
"                                     --allow-bash; --no-python opts out),\n"
"                                 bash (only with --allow-bash),\n"
"                                 system_meminfo, system_loadavg,\n"
"                                 system_cpu_usage, system_swaps,\n"
"                                 memory (only with --memory DIR)\n"
"                               default: datetime,plan,web,\n"
"                                 system_meminfo,system_loadavg,\n"
"                                 system_cpu_usage,system_swaps,\n"
"                                 (memory is auto-registered when --memory is set;\n"
"                                  fs and python3 are auto-registered when\n"
"                                  --sandbox or --allow-bash is set)\n"
"    --sandbox DIR              enable file work scoped to DIR.\n"
"                                 Auto-registers the unified `fs` tool\n"
"                                 (action=read / write / list / glob / grep\n"
"                                 / check_path / cwd / sandbox) AND the\n"
"                                 `python3` tool (a stdlib-only Python 3\n"
"                                 interpreter for compute / network / data;\n"
"                                 NEVER for disk — its open() / os.open()\n"
"                                 calls are auto-restricted to DIR). Without\n"
"                                 --sandbox (and without --allow-bash) the\n"
"                                 model has no file access.\n"
"    --allow-bash               register the `bash` tool (run shell\n"
"                                 commands). Implies `fs` and `python3`\n"
"                                 registration (bash subsumes both; without\n"
"                                 them the model would use bash for file\n"
"                                 work and computation alike).\n"
"                                 WARNING: NOT a hardened sandbox — the\n"
"                                 command runs with your user privileges\n"
"                                 (network, full FS, etc). cwd is set to\n"
"                                 --sandbox DIR if given, otherwise the\n"
"                                 current working dir.\n"
"    --no-python                drop the `python3` tool. By default it's\n"
"                                 auto-registered alongside `fs` (whenever\n"
"                                 --sandbox or --allow-bash is set). The\n"
"                                 interpreter is isolated (no PYTHON* env,\n"
"                                 no site-packages, no cwd on sys.path) and\n"
"                                 disk access is auto-restricted to the\n"
"                                 sandbox root via a Python preamble. Pass\n"
"                                 --no-python to disable it entirely (e.g.\n"
"                                 in environments where any subprocess\n"
"                                 executor is too much capability).\n"
"    --no-show-bash             suppress the live mirror of bash output to\n"
"                                 stderr. By default, when the model calls\n"
"                                 `bash`, the merged child stdout+stderr is\n"
"                                 also written to the parent's stderr in real\n"
"                                 time so the operator can watch a long build\n"
"                                 / test scroll by. The model still receives\n"
"                                 the full captured buffer either way; this\n"
"                                 flag only silences the diagnostic mirror.\n"
"                                 Override the default in the INI's [cli]\n"
"                                 section: show_bash = false.\n"
"    --no-show-python           same as --no-show-bash, but for `python3`.\n"
"                                 INI: [cli] show_python = false.\n"
"    --tools-mode MODE          how the multi-action tool families (fs, web,\n"
"                                 memory) are exposed to the model:\n"
"                                   \"split\"  — one focused tool per action\n"
"                                     (fs_read, fs_edit, fs_glob, …, web_search,\n"
"                                     web_fetch, memory_save, …); flat schemas,\n"
"                                     no \"unknown action\" failure mode. DEFAULT\n"
"                                     since 2026-05-15 — works reliably across\n"
"                                     small / quantised callers and large ones.\n"
"                                   \"unified\" — single dispatcher with an\n"
"                                     `action` argument (e.g. fs(action=\"read\"));\n"
"                                     smallest system-prompt footprint. Pick this\n"
"                                     when you specifically want the legacy\n"
"                                     surface (3 dispatchers instead of 19\n"
"                                     focused tools).\n"
"                                   \"both\"   — register both surfaces side by\n"
"                                     side. Costs more system-prompt tokens but\n"
"                                     lets the model pick whichever shape it's\n"
"                                     more comfortable with on a per-call basis.\n"
"                                 INI: [cli] tools_mode = unified|split|both.\n"
"    --use-google               enable engine=\"google\" inside the `web`\n"
"                                 tool (Google Custom Search JSON API).\n"
"                                 Requires both GOOGLE_API_KEY and\n"
"                                 GOOGLE_CSE_ID env vars; counts against\n"
"                                 your Google quota (free tier: 100/day).\n"
"                                 The default engine \"ddg\" (DuckDuckGo)\n"
"                                 needs no env vars and no opt-in.\n"
"    --external-tools DIR       load every EASYAI-*.tools file in DIR as an\n"
"                                 external-tools manifest. Empty dir is a\n"
"                                 normal state (no extra tools). Per-file\n"
"                                 errors are logged and skipped; other files\n"
"                                 still load. -q hides security sanity-check\n"
"                                 warnings (errors are always shown).\n"
"                                 See EXTERNAL_TOOLS.md.\n"
"    --memory DIR               enable the agent's persistent memory store\n"
"                                 rooted at DIR (alias: --RAG). Registers ONE\n"
"                                 `memory(action=...)` tool with sub-actions\n"
"                                 save / append / search / load / list /\n"
"                                 delete / keywords. Each memory is a small\n"
"                                 Markdown file in DIR. See RAG.md.\n"
"    --no-plan                  don't auto-register the planning tool\n"
"\n"
"  Behaviour:\n"
"    -p, --prompt TEXT          one-shot prompt; without this you get a REPL\n"
"                               (you can also pass the prompt as a positional\n"
"                                arg, or pipe it via stdin)\n"
"    --no-reasoning             hide delta.reasoning_content (default: shown\n"
"                                inline in dim grey).  --hide-reasoning is\n"
"                                an alias.  --show-reasoning is now a no-op\n"
"                                (kept for backwards compat).\n"
"    --max-reasoning N          abort the SSE stream when this turn's\n"
"                                accumulated reasoning_content exceeds N\n"
"                                chars.  Useful for chatty thinking models\n"
"                                that fall into long deliberation loops on\n"
"                                niche questions.  0 = unlimited (default).\n"
"    --no-retry-on-incomplete   disable the auto-retry-with-nudge for\n"
"                                incomplete turns (default: ON).  When the\n"
"                                server flags a turn as incomplete\n"
"                                (timings.incomplete=true — model produced\n"
"                                no tool_call AND only a tiny reply, e.g.\n"
"                                'I'll search…'), the client by default\n"
"                                drops that turn, appends a corrective user\n"
"                                nudge, and re-issues ONCE.  Use this flag\n"
"                                if you want the raw incomplete signal\n"
"                                without recovery.\n"
"    --retry-on-incomplete      legacy alias for the now-default behaviour;\n"
"                                kept for backwards compatibility, no-op.\n"
"    --verbose                  log HTTP+SSE traffic to stderr (timestamps +\n"
"                                per-piece diagnostics).  Stderr-only;\n"
"                                does NOT create a /tmp log file (use\n"
"                                --log-file PATH for that).\n"
"    -q, --quiet                disable the spinner glyph + context-fill\n"
"                                gauge (e.g. |45%%).  Use for batch / scripted\n"
"                                runs where stdout is captured.  Streamed\n"
"                                content + tool markers still print; only\n"
"                                the in-place spinner is suppressed.\n"
"                                Also: in quiet mode Ctrl-C / SIGTERM\n"
"                                hard-cancels the in-flight request (the\n"
"                                expected behaviour for `kill <pid>` in\n"
"                                a script).  Without --quiet, Ctrl-C\n"
"                                stops the current generation; press\n"
"                                Ctrl-C a second time to quit.\n"
"    --log-file PATH            opt in to a raw transaction log at PATH\n"
"                                (request body + every SSE chunk + every\n"
"                                tool dispatch input/output).  Default\n"
"                                OFF — no log file is created without\n"
"                                this flag.  Implies --verbose.\n"
"                                INI: [cli] log_file = PATH.\n"
"    --continue                 load `.easyai_session` from the current\n"
"                                directory before the first prompt.\n"
"                                Default OFF: without this flag, any\n"
"                                existing session file is ignored and\n"
"                                overwritten on the first turn.  The\n"
"                                file is written atomically after EVERY\n"
"                                turn regardless of how loading was\n"
"                                decided.\n"
"                                INI: [cli] auto_continue = true|false.\n"
"    --no-continue              explicit form of the default — ignore\n"
"                                any existing `.easyai_session` and\n"
"                                overwrite it on the first turn.  Useful\n"
"                                in scripts to override an operator's\n"
"                                INI that sets auto_continue = on.\n"
"    --compress                 after loading the session, ask the model\n"
"                                for a single lossless recap of the\n"
"                                entire conversation and replace the\n"
"                                history with that recap before the\n"
"                                next prompt fires.  Useful when context\n"
"                                gets long; the recap drops tool result\n"
"                                noise + abandoned branches and keeps\n"
"                                facts / decisions / file paths.  Also\n"
"                                reachable mid-REPL via /compress.\n"
"                                No-op without --continue (nothing in\n"
"                                memory to recap on a fresh session).\n"
"                                INI: [cli] auto_compress = true|false.\n"
"    --session-file NAME        override the default `.easyai_session`\n"
"                                file. Accepts a name (relative to cwd)\n"
"                                or a full path. Implies --continue —\n"
"                                the file is loaded before the first\n"
"                                prompt. Use to feed a pre-baked\n"
"                                session into a scheduled job or to\n"
"                                keep multiple parallel sessions in\n"
"                                one project directory.\n"
"                                Examples:\n"
"                                  --session-file mysession\n"
"                                  --session-file /var/lib/easyai/seed.json\n"
"    --no-local-session         read-only mode for the session file.\n"
"                                --continue / --session-file still LOAD\n"
"                                the file at startup, but no save is\n"
"                                ever written back. Pair with\n"
"                                --session-file to seed a scheduled job\n"
"                                with a pre-structured pre-session\n"
"                                without mutating the source file.\n"
"    --config PATH              INI overlay (CLI > INI > hardcoded). Without\n"
"                                this flag the CLI looks in layers:\n"
"                                  1. $HOME/.easyai/easyai-cli.ini  (per-user)\n"
"                                  2. /etc/easyai/easyai-cli.ini    (fallback)\n"
"                                  3. <none>                        (defaults)\n"
"                                --config <path> pins one file; a missing\n"
"                                explicit path prints a warning and falls\n"
"                                through to defaults. The [cli] section\n"
"                                accepts every flag below as a snake_case key\n"
"                                (url, api_key, model, temperature, top_p,\n"
"                                tools, sandbox, allow_bash, …). See\n"
"                                easyai-cli.md §5 for the full table and\n"
"                                resources/easyai-cli.ini.example for a\n"
"                                pristine reference file to copy.\n"
"    --shell                    hybrid AI shell: starts the user's $SHELL.\n"
"                                Normal commands execute via the shell.\n"
"                                Lines prefixed with > are sent to the AI.\n"
"                                cd and export persist across commands.\n"
"                                Ctrl+C stops AI generation or the running\n"
"                                command and returns to the prompt.\n"
"                                /exit to quit. Implies --allow-bash.\n"
"    --unattended               inject an [unattended] block into the system\n"
"                                prompt: tells the model there is no human at\n"
"                                the terminal, so it cannot ask clarifying\n"
"                                questions, request approval, or present a\n"
"                                numbered menu and wait. The model picks the\n"
"                                most reasonable interpretation and drives\n"
"                                the task to completion in this turn.\n"
"                                Auto-set whenever a prompt is given on the\n"
"                                command line (-p / positional / piped\n"
"                                stdin). Has no effect in interactive REPL\n"
"                                mode unless passed explicitly.\n"
"\n"
"  Management subcommands (use one, no chat):\n"
"    --list-tools               list LOCAL tools (registered in this CLI)\n"
"                               with their full descriptions — useful to\n"
"                               see exactly what the model will be told\n"
"    --list-remote-tools        GET /v1/tools — list server-side tools\n"
"                               (easyai-server extension, may not exist on\n"
"                               other OpenAI-compat servers)\n"
"    --list-models              GET /v1/models\n"
"    --health                   GET /health\n"
"    --props                    GET /props\n"
"    --metrics                  GET /metrics (Prometheus text)\n"
"    --set-preset NAME          POST /v1/preset {preset:NAME}\n"
"    --show-system-prompt       print the resolved system prompt (built-in\n"
"                                injection PLUS --system / --system-file\n"
"                                content) and exit. Doesn't need --url —\n"
"                                useful for confirming the [environment] /\n"
"                                [guidance] blocks landed.\n"
"\n"
"  Misc:\n"
"    -h, --help                 this help\n",
                 argv0);
}

bool parse_args(int argc, char ** argv, Options & o) {
    auto need = [&](int & i, const char * flag) -> std::string {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "missing value for %s\n", flag);
            return std::string();
        }
        // Refuse to silently consume the next arg if it looks like
        // another flag (`--something`). That's almost always a missing
        // value typo, e.g. `--system --url X` where the user forgot the
        // text for --system and then accidentally fed `--url` to it.
        // Single-dash forms (`-1`, `-q`) stay valid because they're
        // legitimate flag values for sampling / shorthand.
        const char * next = argv[i + 1];
        if (next[0] == '-' && next[1] == '-') {
            std::fprintf(stderr,
                "missing value for %s (next arg `%s` looks like a flag — "
                "quote it if intentional, e.g. %s \"%s\")\n",
                flag, next, flag, next);
            return std::string();
        }
        return argv[++i];
    };
    if (const char * v = std::getenv("EASYAI_URL"))     o.url     = v;
    if (const char * v = std::getenv("EASYAI_API_KEY")) o.api_key = v;
    if (const char * v = std::getenv("EASYAI_MODEL"))   o.model   = v;
    if (const char * v = std::getenv("EASYAI_TIMEOUT")) {
        try { o.timeout = std::stoi(v); } catch (...) {}
    }
    if (const char * v = std::getenv("EASYAI_HTTP_RETRIES")) {
        try { o.http_retries = std::stoi(v); } catch (...) {}
    }

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--url")            { o.url = need(i, "--url"); o.url_cli_set = true; }
        else if (a == "--api-key")        { o.api_key = need(i, "--api-key"); o.api_key_cli_set = true; }
        else if (a == "--model")          { o.model = need(i, "--model"); o.model_cli_set = true; }
        else if (a == "--timeout")        { o.timeout = std::stoi(need(i, "--timeout")); o.timeout_cli_set = true; }
        else if (a == "--http-retries")   { o.http_retries = std::stoi(need(i, "--http-retries")); o.http_retries_cli_set = true; }
        else if (a == "--max-tool-hops") {
            try { o.max_tool_hops = std::stoi(need(i, "--max-tool-hops")); } catch (...) {}
            // 0 / negative → effectively unlimited (same as default).
            if (o.max_tool_hops <= 0) o.max_tool_hops = 99999;
            o.max_tool_hops_cli_set = true;
        }
        else if (a == "--system")         { o.system_prompt = need(i, "--system"); o.system_prompt_cli_set = true; }
        else if (a == "--system-file")    { o.system_file = need(i, "--system-file"); o.system_file_cli_set = true; }
        else if (a == "--sandbox")        { o.sandbox = need(i, "--sandbox"); o.sandbox_cli_set = true; }
        else if (a == "--allow-bash")     { o.allow_bash = true; o.allow_bash_cli_set = true; }
        else if (a == "--no-python")      { o.allow_python = false; o.allow_python_cli_set = true; }
        else if (a == "--no-show-bash") {
            o.show_bash         = false;
            o.show_bash_cli_set = true;
        }
        else if (a == "--show-bash") {
            o.show_bash         = true;
            o.show_bash_cli_set = true;
        }
        else if (a == "--no-show-python") {
            o.show_python         = false;
            o.show_python_cli_set = true;
        }
        else if (a == "--show-python") {
            o.show_python         = true;
            o.show_python_cli_set = true;
        }
        else if (a == "--config") {
            o.config_path         = need(i, "--config");
            o.config_path_cli_set = true;
        }
        else if (a == "--shell")           { o.shell_mode = true; o.shell_mode_cli_set = true; }
        else if (a == "--unattended")     { o.unattended = true; o.unattended_cli_set = true; }
        else if (a == "--use-google")     { o.use_google = true; o.use_google_cli_set = true; }
        else if (a == "--external-tools") { o.external_tools_dir = need(i, "--external-tools"); o.external_tools_cli_set = true; }
        else if (a == "--memory" ||
                 a == "--RAG")            { o.rag_dir = need(i, a.c_str()); o.rag_dir_cli_set = true; }
        else if (a == "--temperature")    o.temperature       = std::stof(need(i, "--temperature"));
        else if (a == "--top-p")          o.top_p             = std::stof(need(i, "--top-p"));
        else if (a == "--top-k")          o.top_k             = std::stoi(need(i, "--top-k"));
        else if (a == "--min-p")          o.min_p             = std::stof(need(i, "--min-p"));
        else if (a == "--repeat-penalty") o.repeat_penalty    = std::stof(need(i, "--repeat-penalty"));
        else if (a == "--frequency-penalty") o.frequency_penalty = std::stof(need(i, "--frequency-penalty"));
        else if (a == "--presence-penalty")  o.presence_penalty  = std::stof(need(i, "--presence-penalty"));
        else if (a == "--seed")           o.seed              = std::stoll(need(i, "--seed"));
        else if (a == "--max-tokens")     o.max_tokens        = std::stoi(need(i, "--max-tokens"));
        else if (a == "--stop")           { o.stop_sequences.push_back(need(i, "--stop")); o.stop_cli_set = true; }
        else if (a == "--extra-json")     { o.extra_body = need(i, "--extra-json"); o.extra_body_cli_set = true; }
        else if (a == "--tools") {
            std::string list = need(i, "--tools");
            std::stringstream ss(list);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                if (!tok.empty()) o.tools_enabled.insert(tok);
            }
            o.tools_enabled_cli_set = true;
        }
        else if (a == "--tools-mode") {
            std::string m = need(i, "--tools-mode");
            if (m != "unified" && m != "split" && m != "both") {
                std::fprintf(stderr,
                    "invalid --tools-mode \"%s\" — valid: \"unified\" "
                    "(single dispatcher per family), \"split\" (one tool "
                    "per action), or \"both\" (register both surfaces)\n",
                    m.c_str());
                return false;
            }
            o.tools_mode         = m;
            o.tools_mode_cli_set = true;
        }
        else if (a == "--no-plan")        { o.no_plan = true; o.no_plan_cli_set = true; }
        else if (a == "--show-reasoning") { o.show_reasoning = true; o.show_reasoning_cli_set = true; }
        else if (a == "--no-reasoning"
              || a == "--hide-reasoning") { o.show_reasoning = false; o.show_reasoning_cli_set = true; }
        else if (a == "--max-reasoning")  { o.max_reasoning = std::stoi(need(i, "--max-reasoning")); o.max_reasoning_cli_set = true; }
        else if (a == "--retry-on-incomplete")    { o.retry_on_incomplete = true; o.retry_on_incomplete_cli_set = true; }
        else if (a == "--no-retry-on-incomplete") { o.retry_on_incomplete = false; o.retry_on_incomplete_cli_set = true; }
        else if (a == "--verbose" || a == "-v") { o.verbose = true; o.verbose_cli_set = true; }
        else if (a == "--quiet"   || a == "-q") { o.quiet   = true; o.quiet_cli_set   = true; }
        else if (a == "--log-file") {
            o.log_file_path = need(i, "--log-file");
            o.log_file_path_cli_set = true;
        }
        else if (a == "--continue") {
            // Opt in to loading `.easyai_session` before the first
            // prompt.  Default is auto_continue=false; setting cli_set
            // also guards against `[cli] auto_continue = off` in the
            // INI flipping this back off for this invocation.
            o.auto_continue         = true;
            o.auto_continue_cli_set = true;
        }
        else if (a == "--no-continue") {
            // Explicit form of the default — useful when an operator's
            // INI sets `auto_continue = on` and a script wants to force
            // a fresh start for this invocation.
            o.auto_continue         = false;
            o.auto_continue_cli_set = true;
        }
        else if (a == "--compress") {
            o.auto_compress         = true;
            o.auto_compress_cli_set = true;
        }
        else if (a == "--session-file") {
            o.session_file = need(i, "--session-file");
            o.session_file_cli_set = true;
            // Auto-imply --continue: the operator passed an explicit
            // path so they obviously want it loaded. Don't make them
            // type both flags. They can still pass --no-continue
            // AFTER --session-file on the same command line to
            // override this (rare; --session-file without loading
            // is essentially a useless write-only mode).
            o.auto_continue         = true;
            o.auto_continue_cli_set = true;
        }
        else if (a == "--no-local-session") {
            // Read-only mode for session files. --continue / --session-
            // file still LOAD the file at startup, but no save_session()
            // call writes anything back. Designed for "feed a pre-baked
            // session into a scheduled job without mutating the file".
            o.no_local_session         = true;
            o.no_local_session_cli_set = true;
        }
        else if (a == "--insecure-tls")   { o.tls_insecure = true; o.tls_insecure_cli_set = true; }
        else if (a == "--ca-cert")        { o.tls_ca_path  = need(i, "--ca-cert"); o.tls_ca_path_cli_set = true; }
        else if (a == "-p" || a == "--prompt") o.prompt = need(i, "--prompt");
        else if (a == "--list-models")    o.list_models       = true;
        else if (a == "--list-tools")     o.list_tools        = true;
        else if (a == "--list-remote-tools") o.list_remote_tools = true;
        else if (a == "--health")         o.health      = true;
        else if (a == "--props")          o.props       = true;
        else if (a == "--metrics")        o.metrics     = true;
        else if (a == "--set-preset")     o.set_preset  = need(i, "--set-preset");
        else if (a == "--show-system-prompt") o.show_system_prompt = true;
        else if (a == "-h" || a == "--help") { usage(argv[0]); std::exit(0); }
        else if (!a.empty() && a[0] != '-' && o.prompt.empty()) {
            // Positional argument is treated as the one-shot prompt, so
            // `easyai-cli-remote --url ai.local "what date is today?"`
            // works without the explicit -p / --prompt flag.  Multiple
            // positionals get joined with a space.
            o.prompt = a;
            for (++i; i < argc; ++i) {
                std::string extra = argv[i];
                if (!extra.empty() && extra[0] != '-') {
                    o.prompt += " ";
                    o.prompt += extra;
                } else {
                    --i;
                    break;
                }
            }
        }
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return false;
        }
    }

    if (!o.system_file.empty()) {
        std::ifstream f(o.system_file);
        if (!f) {
            std::fprintf(stderr, "cannot read --system-file %s\n", o.system_file.c_str());
            return false;
        }
        std::stringstream ss; ss << f.rdbuf();
        o.system_prompt = ss.str();
    }

    // INI overlay: precedence is CLI > INI > hardcoded.
    //
    // Path resolution (when --config wasn't given):
    //   1. $HOME/.easyai/easyai-cli.ini   — per-user, primary location
    //   2. /etc/easyai/easyai-cli.ini     — system-wide fallback
    //   3. <none>                         — run on defaults
    //
    // --config <path> bypasses the layers and pins one file; if it's
    // missing we still proceed on defaults but emit a warning (the
    // operator explicitly asked for that path, so silence would be
    // a foot-gun). A missing layered-default path is silent — it
    // just means the operator hasn't created a config yet.
    if (!o.config_path_cli_set) {
        const char * home = std::getenv("HOME");
        std::string user_path;
        if (home && *home) {
            user_path = std::string(home) + "/.easyai/easyai-cli.ini";
        }
        const std::string system_path = "/etc/easyai/easyai-cli.ini";
        std::error_code ec;
        if (!user_path.empty() && std::filesystem::exists(user_path, ec)) {
            o.config_path = user_path;
        } else if (std::filesystem::exists(system_path, ec)) {
            o.config_path = system_path;
        } else {
            o.config_path.clear();
        }
    } else {
        std::error_code ec;
        if (!std::filesystem::exists(o.config_path, ec)) {
            std::fprintf(stderr,
                "easyai-cli: --config %s not found; continuing on defaults\n",
                o.config_path.c_str());
            o.config_path.clear();
        }
    }
    if (o.verbose && !o.config_path.empty()) {
        std::fprintf(stderr,
            "easyai-cli: loading config from %s\n", o.config_path.c_str());
    }

    // A missing INI file is NOT an error — load_ini_file returns an
    // empty Ini and we simply keep the hardcoded defaults.
    {
        std::string ini_err;
        easyai::config::Ini ini =
            easyai::config::load_ini_file(o.config_path, ini_err);
        if (!ini_err.empty()) {
            std::fprintf(stderr,
                "easyai-cli: %s warnings:\n%s\n",
                o.config_path.c_str(), ini_err.c_str());
        }
        auto warn_bad = [&](const char * key, const std::string & v,
                            const char * expected, const char * keeping) {
            std::fprintf(stderr,
                "easyai-cli: %s [cli] %s=%s — expected %s; keeping default %s\n",
                o.config_path.c_str(), key, v.c_str(), expected, keeping);
        };
        auto load_bool_flag = [&](const char * key, bool & target,
                                  bool cli_set) {
            if (cli_set) return;
            const std::string v = ini.get("cli", key);
            if (v.empty()) return;
            std::string lc; lc.reserve(v.size());
            for (char ch : v) lc.push_back((char) std::tolower((unsigned char) ch));
            if (lc == "false" || lc == "no" || lc == "off" || lc == "0") {
                target = false;
            } else if (lc == "true" || lc == "yes" || lc == "on" || lc == "1") {
                target = true;
            } else {
                warn_bad(key, v, "true/false",
                         target ? "true" : "false");
            }
        };
        auto load_str_flag = [&](const char * key, std::string & target,
                                 bool cli_set) {
            if (cli_set) return;
            const std::string v = ini.get("cli", key);
            if (v.empty()) return;
            target = v;
        };
        auto load_int_flag = [&](const char * key, int & target,
                                 bool cli_set) {
            if (cli_set) return;
            const std::string v = ini.get("cli", key);
            if (v.empty()) return;
            try { target = std::stoi(v); }
            catch (...) {
                warn_bad(key, v, "integer", std::to_string(target).c_str());
            }
        };
        auto load_long_flag = [&](const char * key, long long & target,
                                  bool cli_set) {
            if (cli_set) return;
            const std::string v = ini.get("cli", key);
            if (v.empty()) return;
            try { target = std::stoll(v); }
            catch (...) {
                warn_bad(key, v, "integer", std::to_string(target).c_str());
            }
        };
        auto load_float_flag = [&](const char * key, float & target,
                                   bool cli_set) {
            if (cli_set) return;
            const std::string v = ini.get("cli", key);
            if (v.empty()) return;
            try { target = std::stof(v); }
            catch (...) {
                warn_bad(key, v, "float", std::to_string(target).c_str());
            }
        };
        // Sentinel-driven loaders for sampling knobs where the "unset"
        // value is a negative number (CLI default), not a tracking bit.
        // Apply the INI only when no --flag was given AND the value is
        // still the sentinel — the operator may have explicitly passed
        // e.g. --temperature 0.0 which is a legit value but not the
        // default.
        auto load_float_sentinel = [&](const char * key, float & target,
                                       float sentinel) {
            if (target != sentinel) return;
            const std::string v = ini.get("cli", key);
            if (v.empty()) return;
            try { target = std::stof(v); }
            catch (...) {
                warn_bad(key, v, "float", std::to_string(target).c_str());
            }
        };
        auto load_int_sentinel = [&](const char * key, int & target,
                                     int sentinel) {
            if (target != sentinel) return;
            const std::string v = ini.get("cli", key);
            if (v.empty()) return;
            try { target = std::stoi(v); }
            catch (...) {
                warn_bad(key, v, "integer", std::to_string(target).c_str());
            }
        };
        // Comma-separated list (used for --tools and --stop).
        auto load_csv_list = [&](const char * key,
                                 std::vector<std::string> & target,
                                 bool cli_set) {
            if (cli_set) return;
            const std::string v = ini.get("cli", key);
            if (v.empty()) return;
            target.clear();
            std::stringstream ss(v);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                // trim surrounding whitespace
                auto issp = [](unsigned char c){ return std::isspace(c)!=0; };
                while (!tok.empty() && issp(tok.front())) tok.erase(tok.begin());
                while (!tok.empty() && issp(tok.back()))  tok.pop_back();
                if (!tok.empty()) target.push_back(std::move(tok));
            }
        };
        auto load_csv_set = [&](const char * key,
                                std::set<std::string> & target,
                                bool cli_set) {
            std::vector<std::string> tmp;
            load_csv_list(key, tmp, cli_set);
            if (tmp.empty()) return;
            target.clear();
            for (auto & t : tmp) target.insert(std::move(t));
        };

        // ----- Connection ---------------------------------------------
        load_str_flag  ("url",            o.url,            o.url_cli_set);
        load_str_flag  ("api_key",        o.api_key,        o.api_key_cli_set);
        load_str_flag  ("model",          o.model,          o.model_cli_set);
        load_int_flag  ("timeout",        o.timeout,        o.timeout_cli_set);
        load_int_flag  ("http_retries",   o.http_retries,   o.http_retries_cli_set);
        load_int_flag  ("max_tool_hops",  o.max_tool_hops,  o.max_tool_hops_cli_set);
        if (!o.max_tool_hops_cli_set && o.max_tool_hops <= 0) o.max_tool_hops = 99999;
        load_bool_flag ("insecure_tls",   o.tls_insecure,   o.tls_insecure_cli_set);
        load_str_flag  ("ca_cert",        o.tls_ca_path,    o.tls_ca_path_cli_set);

        // ----- Conversation -------------------------------------------
        load_str_flag  ("system",         o.system_prompt,  o.system_prompt_cli_set);
        load_str_flag  ("system_file",    o.system_file,    o.system_file_cli_set);

        // ----- Sampling / penalty (sentinel-driven) -------------------
        load_float_sentinel("temperature",       o.temperature,       -1.0f);
        load_float_sentinel("top_p",             o.top_p,             -1.0f);
        load_int_sentinel  ("top_k",             o.top_k,             -1);
        load_float_sentinel("min_p",             o.min_p,             -1.0f);
        // repeat_penalty has a non-sentinel default (1.15); use a
        // bool-tracked overlay so an INI value can override 1.15.
        {
            const std::string v = ini.get("cli", "repeat_penalty");
            if (!v.empty()) {
                try { o.repeat_penalty = std::stof(v); }
                catch (...) {
                    warn_bad("repeat_penalty", v, "float",
                             std::to_string(o.repeat_penalty).c_str());
                }
            }
        }
        load_float_sentinel("frequency_penalty", o.frequency_penalty, -2.0f);
        load_float_sentinel("presence_penalty",  o.presence_penalty,  -2.0f);
        load_long_flag ("seed",           o.seed,           /*cli_set=*/o.seed != -1);
        load_int_sentinel ("max_tokens",  o.max_tokens,     -1);
        load_csv_list  ("stop",           o.stop_sequences, o.stop_cli_set);
        load_str_flag  ("extra_json",     o.extra_body,     o.extra_body_cli_set);

        // ----- Tools --------------------------------------------------
        load_csv_set   ("tools",          o.tools_enabled,  o.tools_enabled_cli_set);
        load_str_flag  ("sandbox",        o.sandbox,        o.sandbox_cli_set);
        load_bool_flag ("allow_bash",     o.allow_bash,     o.allow_bash_cli_set);
        load_bool_flag ("allow_python",   o.allow_python,   o.allow_python_cli_set);
        load_bool_flag ("use_google",     o.use_google,     o.use_google_cli_set);
        load_str_flag  ("external_tools", o.external_tools_dir, o.external_tools_cli_set);
        load_str_flag  ("memory",         o.rag_dir,        o.rag_dir_cli_set);
        load_bool_flag ("no_plan",        o.no_plan,        o.no_plan_cli_set);
        load_bool_flag ("show_bash",      o.show_bash,      o.show_bash_cli_set);
        load_bool_flag ("show_python",    o.show_python,    o.show_python_cli_set);

        // ----- Reasoning / retry --------------------------------------
        load_bool_flag ("show_reasoning",      o.show_reasoning,      o.show_reasoning_cli_set);
        load_int_flag  ("max_reasoning",       o.max_reasoning,       o.max_reasoning_cli_set);
        load_bool_flag ("retry_on_incomplete", o.retry_on_incomplete, o.retry_on_incomplete_cli_set);

        // ----- Display / logging --------------------------------------
        load_bool_flag ("verbose",        o.verbose,        o.verbose_cli_set);
        load_bool_flag ("quiet",          o.quiet,          o.quiet_cli_set);
        load_bool_flag ("auto_log",       o.auto_log,       /*cli_set=*/false);
        load_str_flag  ("log_file",       o.log_file_path,  o.log_file_path_cli_set);
        load_bool_flag ("unattended",     o.unattended,     o.unattended_cli_set);

        // ----- Session ------------------------------------------------
        load_bool_flag ("auto_continue",     o.auto_continue,     o.auto_continue_cli_set);
        load_bool_flag ("auto_compress",     o.auto_compress,     o.auto_compress_cli_set);
        load_str_flag  ("session_file",      o.session_file,      o.session_file_cli_set);
        load_bool_flag ("no_local_session",  o.no_local_session,  o.no_local_session_cli_set);
        load_bool_flag ("shell",             o.shell_mode,        o.shell_mode_cli_set);

        // ----- Tools-mode (closed enum, validated separately) ---------
        if (!o.tools_mode_cli_set) {
            const std::string v = ini.get("cli", "tools_mode");
            if (!v.empty()) {
                if (v == "unified" || v == "split" || v == "both") {
                    o.tools_mode = v;
                } else {
                    std::fprintf(stderr,
                        "%s: [cli] tools_mode = \"%s\" is invalid — "
                        "valid: \"unified\" / \"split\" / \"both\". "
                        "Keeping default \"%s\".\n",
                        o.config_path.c_str(), v.c_str(),
                        o.tools_mode.c_str());
                }
            }
        }
    }
    return true;
}

bool any_management(const Options & o) {
    return o.list_models || o.list_tools || o.list_remote_tools
        || o.health      || o.props      || o.metrics
        || !o.set_preset.empty();
}

// ---- tool registration ----------------------------------------------------
// Default catalog when --tools isn't given.
const std::vector<std::string> kDefaultTools = {
    "datetime", "plan", "web",
    "tool_lookup",
    "system_meminfo", "system_loadavg", "system_cpu_usage", "system_swaps",
};

void register_tools(easyai::Client & cli,
                    easyai::Plan & plan,
                    const Options & o,
                    const Style & st) {
    auto wants = [&](const std::string & name) {
        if (o.tools_enabled.empty()) {
            for (const auto & d : kDefaultTools) if (d == name) return true;
            // `fs` auto-enables when --sandbox is set OR --allow-bash
            // is on. bash is strictly more permissive than `fs`, and
            // a sandbox without `fs` is the same trap inverted.
            if (name == "fs" && (!o.sandbox.empty() || o.allow_bash))
                return true;
            // python3 defaults ON: same gate as `fs` (sandbox or bash),
            // but additionally honours --no-python (allow_python=false).
            // Disk access from python3 is auto-restricted to the
            // sandbox root via a Python preamble; the description tells
            // the model "never use python3 for disk — use fs(action=...)".
            if (name == "python3" && o.allow_python
                    && (!o.sandbox.empty() || o.allow_bash))
                return true;
            // bash is opt-in by --allow-bash.
            if (o.allow_bash && name == "bash") return true;
            return false;
        }
        // Explicit --tools list: --no-python still wins over an
        // explicit `python3` in the list (the operator clearly
        // doesn't want it registered).
        if (name == "python3" && !o.allow_python) return false;
        return o.tools_enabled.count(name) != 0;
    };

    if (wants("datetime"))         cli.add_tool(easyai::tools::datetime());
    if (!o.no_plan && wants("plan")) cli.add_tool(plan.tool());
    // Unified `web` tool. engine="google" is gated on --use-google AND
    // both env vars present at registration time; the tool itself
    // re-reads the env at call time so a key rotation surfaces a clear
    // error rather than silent disappearance.
    // Tool surface — unified (single dispatcher) / split (one tool per
    // action) / both. Smaller / quantised tool-callers consistently work
    // better with split surfaces; large models handle either. See
    // --tools-mode below and the README "tools mode" section.
    const bool tm_unified = (o.tools_mode == "unified" || o.tools_mode == "both");
    const bool tm_split   = (o.tools_mode == "split"   || o.tools_mode == "both");

    if (wants("web")) {
        bool google = false;
        if (o.use_google) {
            const char * gk = std::getenv("GOOGLE_API_KEY");
            const char * gx = std::getenv("GOOGLE_CSE_ID");
            google = (gk && *gk && gx && *gx);
        }
        if (tm_unified) cli.add_tool(easyai::tools::web(google));
        if (tm_split) {
            for (auto & t : easyai::tools::web_split(google)) {
                cli.add_tool(std::move(t));
            }
        }
    }

    // fs — scoped to --sandbox if given, otherwise CWD. Unified
    // dispatcher (`fs(action="...")`) and/or focused per-action tools
    // (`fs_read`, `fs_edit`, …) per --tools-mode.
    const std::string root = o.sandbox.empty() ? "." : o.sandbox;
    if (wants("fs")) {
        if (tm_unified) cli.add_tool(easyai::tools::fs(root));
        if (tm_split) {
            for (auto & t : easyai::tools::fs_split(root)) {
                cli.add_tool(std::move(t));
            }
        }
    }

    // bash — same root as fs; opt-in via --allow-bash or --tools bash.
    if (wants("bash")) {
        // o.show_bash mirrors merged child stdout+stderr to our stderr
        // in real time so a long build / test scroll is visible to the
        // operator. Default ON; --no-show-bash (or [cli] show_bash=false
        // in the INI) silences the mirror without affecting what the
        // model sees.
        cli.add_tool(easyai::tools::bash(root, o.show_bash));
    }
    // python3 — same root as fs / bash; opt-in via --allow-python or
    // --tools python3. Same diagnostic mirror as bash, controlled by
    // its own --no-show-python / [cli] show_python.
    if (wants("python3")) {
        cli.add_tool(easyai::tools::python3(root, o.show_python));
    }
    // Tool-hop ceiling.  Apply unconditionally — the cli binary is the
    // agentic surface, and even a tools-only session (no bash) commonly
    // does 10+ hops on a non-trivial task: tool_lookup, fs(action="list"),
    // a handful of fs(action="read") calls, web(action="search") +
    // web(action="fetch"), fs(action="write"), etc.  The library default
    // of 8 truncates real work in production.
    // Default here is 99999 (effectively unlimited); --max-tool-hops N
    // puts a finite cap back if the operator wants one.  Per-tool
    // timeouts and HTTP retry budgets still bound runaway behaviour.
    cli.max_tool_hops(o.max_tool_hops);

    // Inline system-info tools — defined above in `namespace systools`.
    // They demonstrate how to add your own custom Tool with a couple of
    // lines using Tool::builder().
    if (wants("system_meminfo"))   cli.add_tool(systools::make_system_meminfo());
    if (wants("system_loadavg"))   cli.add_tool(systools::make_system_loadavg());
    if (wants("system_cpu_usage")) cli.add_tool(systools::make_system_cpu_usage());
    if (wants("system_swaps"))     cli.add_tool(systools::make_system_swaps());

    // Persistent memory — the agent's long-term store.
    // One `memory(action=...)` tool registered when --memory <dir> is
    // given. The dir does not need to exist yet; the tool creates it on
    // first save. See RAG.md.
    if (!o.rag_dir.empty()) {
        if (o.tools_enabled.empty() || o.tools_enabled.count("memory")) {
            if (tm_unified) cli.add_tool(easyai::tools::make_rag_tool(o.rag_dir));
            if (tm_split) {
                for (auto & t : easyai::tools::memory_split_tools(o.rag_dir)) {
                    cli.add_tool(std::move(t));
                }
            }
        }
    }

    // External tools directory (--external-tools DIR). Loads every
    // EASYAI-*.tools file in DIR. Per-file fault isolation: a bad
    // file is logged and skipped, the agent still starts. We pass
    // the already-registered tool names as `reserved_names` so a
    // manifest entry trying to shadow `bash` becomes a load error,
    // not a silent override.
    //
    // Quiet-mode policy (-q): suppress sanity-check WARNINGS but
    // ALWAYS surface load ERRORS. Errors mean the operator's
    // manifest is broken and the model isn't getting that tool —
    // they need to know even in scripted invocations. Warnings are
    // informational ("you wrapped a shell" / "this is world-
    // writable") and would just clutter a -q session.
    if (!o.external_tools_dir.empty()) {
        std::vector<std::string> reserved;
        reserved.reserve(cli.tools().size());
        for (const auto & t : cli.tools()) reserved.push_back(t.name);
        auto loaded = easyai::load_external_tools_from_dir(
            o.external_tools_dir, reserved);

        for (const auto & e_msg : loaded.errors) {
            std::fprintf(stderr, "%serror:%s [external-tools] %s\n",
                         st.red(), st.reset(), e_msg.c_str());
        }
        if (!o.quiet) {
            for (const auto & w : loaded.warnings) {
                std::fprintf(stderr, "%swarn:%s  [external-tools] %s\n",
                             st.yellow(), st.reset(), w.c_str());
            }
        }
        for (auto & t : loaded.tools) {
            // Honour --tools allowlist if the operator passed one.
            if (!o.tools_enabled.empty()
                    && o.tools_enabled.count(t.name) == 0) continue;
            cli.add_tool(std::move(t));
        }
    }

    // tool_lookup MUST be registered last so the snapshot it returns
    // covers every other tool (built-ins, plan, fs_*, bash, RAG,
    // external-tools manifests).  The factory captures a getter that
    // re-reads cli.tools() at every call, so even tools registered
    // dynamically (e.g. webui-side runtime additions) show up.
    if (wants("tool_lookup")) {
        cli.add_tool(easyai::tools::tool_lookup([&cli]() {
            easyai::tools::ToolCatalog v;
            v.reserve(cli.tools().size());
            for (const auto & t : cli.tools()) {
                v.push_back({ t.name, t.wire_description(), t.description });
            }
            return v;
        }));
    }

    if (o.verbose) {
        vlog("%s[easyai-cli-remote]%s registered %zu tool(s):\n",
             st.dim(), st.reset(), cli.tools().size());
        for (const auto & t : cli.tools()) {
            // Squash the JSON schema down to a single line for the log.
            std::string schema = t.parameters_json;
            for (char & c : schema) if (c == '\n' || c == '\r') c = ' ';
            // Collapse runs of spaces to one, just for readability.
            std::string compact;
            compact.reserve(schema.size());
            bool prev_space = false;
            for (char c : schema) {
                if (c == ' ') {
                    if (!prev_space) compact.push_back(' ');
                    prev_space = true;
                } else {
                    compact.push_back(c);
                    prev_space = false;
                }
            }
            // Trim long descriptions in the log so the line stays
            // scannable; the model still sees the full text.
            std::string desc = t.description;
            for (char & c : desc) if (c == '\n' || c == '\r') c = ' ';
            if (desc.size() > 120) desc = desc.substr(0, 120) + "…";
            vlog("%s  - %s%s%s  desc=\"%s\"\n",
                 st.dim(),
                 st.bold(), t.name.c_str(), st.reset(),
                 desc.c_str());
            vlog("%s    schema=%s%s\n",
                 st.dim(), compact.c_str(), st.reset());
        }
    }
}

// Trim a string to N chars with ellipsis suffix for log lines.
using easyai::text::trim_for_log;

using easyai::ui::print_tool_row;

int run_management(easyai::Client & cli, const Options & o, const Style & st) {
    // Thin dispatcher — the actual work lives in easyai::cli helpers
    // (see include/easyai/cli.hpp).  Each helper returns the process
    // exit code (0 on success, 1 on transport / parse failure).
    if (o.list_models)        return easyai::cli::print_models       (cli, st);
    if (o.list_tools)         return easyai::cli::print_local_tools  (cli, st);
    if (o.list_remote_tools)  return easyai::cli::print_remote_tools (cli, st);
    if (o.health)             return easyai::cli::print_health       (cli, st);
    if (o.props)              return easyai::cli::print_props        (cli);
    if (o.metrics)            return easyai::cli::print_metrics      (cli);
    if (!o.set_preset.empty())return easyai::cli::set_preset         (cli, o.set_preset, st);
    return 0;
}

// ---- callback wiring ------------------------------------------------------
using easyai::ui::render_plan;

// Per-turn streaming wiring is set up directly inside run_one() now,
// using easyai::ui::Streaming.  No globals — the Spinner, StreamStats
// and Streaming objects all live on the run_one stack frame and are
// torn down cleanly at end of turn.

// ---- repl helpers ---------------------------------------------------------
bool is_special(const std::string & line, const std::string & cmd) {
    return line == cmd
        || (line.size() > cmd.size() && line.rfind(cmd + " ", 0) == 0);
}

using easyai::text::prompt_wants_file_write;

using easyai::cli::client_has_tool;

// ---- Ctrl+C / SIGTERM handling --------------------------------------------
//
// Two modes — selected at startup based on --quiet / -q:
//
// QUIET MODE (batch / scripted runs)
//   First signal hard-cancels the in-flight request and exits.
//   Second signal force-exits via _exit(130).
//
// INTERACTIVE (default) MODE — shell-like single-Ctrl+C:
//   During AI generation → STOP: cooperative cancel aborts the SSE
//   stream, the model stops, REPL returns to the prompt.
//   During a shell subprocess (--shell mode) → ignored in our
//   handler; the child in our process group receives SIGINT directly
//   from the kernel and dies.
//   At the REPL / shell prompt → getline returns EINTR, the loop
//   clears cin and re-prompts (like bash). Does NOT exit.
//   Triple rapid signal → force _exit(130) — escape hatch.
//   Exit via /exit, /quit, or Ctrl+D (EOF).
//
// std::atomic<T>::store on a lock-free atomic_bool is async-signal-safe in
// practice on every platform we care about (x86, ARM64, RISC-V) — that
// plus a single ::write() to STDERR_FILENO is all we do from inside the
// handler. printf / fprintf would NOT be safe.
static std::atomic<int>    g_signal_count{0};     // 0=none, 1=stop, 2+=force
static std::atomic<bool>   g_quiet_mode{false};   // mirror of o.quiet
static std::atomic<bool>   g_in_chat{false};      // true between cli.chat() entry/exit
static std::atomic<bool>   g_in_shell_cmd{false};  // true during --shell subprocess wait
static std::atomic<bool>   g_graceful_exit{false}; // quiet-mode only: exit after cancel
static easyai::Client *    g_active_client = nullptr;

static void on_terminating_signal(int /*sig*/) {
    const int count = g_signal_count.fetch_add(1, std::memory_order_relaxed) + 1;

    // Triple signal: force-exit (escape hatch for stuck streams/subprocesses).
    if (count >= 3) {
        static const char kMsg[] = "\n<force-exiting now.>\n";
        ssize_t _ = ::write(STDERR_FILENO, kMsg, sizeof(kMsg) - 1);
        (void) _;
        ::_exit(130);
    }

    // Quiet mode: first signal hard-cancels and marks exit.
    if (g_quiet_mode.load(std::memory_order_relaxed)) {
        g_graceful_exit.store(true, std::memory_order_relaxed);
        if (g_active_client != nullptr) g_active_client->request_cancel();
        return;
    }

    // Shell subprocess running: the child shares our process group and
    // receives SIGINT directly from the kernel — nothing for us to do.
    // Reset the counter so the next Ctrl+C after the child exits starts
    // fresh.
    if (g_in_shell_cmd.load(std::memory_order_relaxed)) {
        g_signal_count.store(0, std::memory_order_relaxed);
        return;
    }

    // Mid-chat (AI generation): cancel the stream, return to prompt.
    if (g_in_chat.load(std::memory_order_relaxed)) {
        if (g_active_client != nullptr) g_active_client->request_cancel();
        if (count == 1) {
            static const char kMsg[] = "\n<stopped.>\n";
            ssize_t _ = ::write(STDERR_FILENO, kMsg, sizeof(kMsg) - 1);
            (void) _;
        }
        return;
    }

    // At REPL / shell prompt: getline returns EINTR. The loop clears
    // cin and re-prompts — does NOT exit (like bash).
}

static void install_cancel_handlers() {
    struct sigaction sa{};
    sa.sa_handler = on_terminating_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   // no SA_RESTART → blocked syscalls return EINTR
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

int run_one(easyai::Client & cli, easyai::Plan & plan,
            const std::string & prompt,
            const Options & o, const Style & st) {
    // Tip targets newcomers who've forgotten --sandbox.  If the user
    // already passed --sandbox we stay quiet — they know about it; the
    // missing tool would be from an explicit --tools filter, where the
    // tip's "pass --sandbox" advice is wrong anyway. We probe by the
    // registered tool name `fs` — when --sandbox is unset the unified
    // fs tool isn't registered and `fs(action="write")` is unreachable.
    if (o.sandbox.empty()
        && prompt_wants_file_write(prompt)
        && !client_has_tool(cli, "fs")) {
        std::fprintf(stderr,
            "%s[easyai-cli-remote] tip:%s your prompt looks like it wants "
            "the model to write a file, but the `fs` tool is NOT registered. "
            "Pass %s--sandbox DIR%s to give the model file read+write access "
            "scoped to DIR (or %s--tools fs%s explicitly). "
            "Without it the model will research, then stall when it tries "
            "to save and finds no write tool.\n",
            st.yellow(), st.reset(),
            st.bold(),  st.reset(),
            st.bold(),  st.reset());
    }

    // --quiet/-q disables the in-place spinner glyph + ctx-% gauge.
    // Streamed content + tool markers still print; only the cursor-
    // tracking decoration is suppressed, which is what batch / scripted
    // / service consumers want (no \b dance polluting captured stdout).
    Spinner     spinner(/*enabled=*/!o.quiet);
    StreamStats stats;  stats.reset();

    // Attach the canonical streaming UX (spinner-locked content + dim
    // reasoning + ●/✗ tool markers + live plan render) to the client
    // and plan.  MUST be a named local — the attach() lambdas capture
    // `this`, so a temporary that died at the end of its expression
    // would leave them dangling for the duration of cli.chat().
    easyai::ui::Streaming streaming(spinner, stats, st);
    streaming.show_reasoning(o.show_reasoning)
             .verbose       (o.verbose)
             .attach        (cli)
             .attach        (plan);

    // Checkpoint .easyai_session after every tool round-trip so a
    // long agentic turn that gets force-exited (3-stage Ctrl-C → stage
    // 3 → _exit(130)) still leaves the last completed tool's state on
    // disk.  Stages 1-2 (graceful / cancel) already get a save via
    // the post-chat() path; only the force-exit case needs this.
    //
    // Composes with Streaming's on_tool_ via notify_tool() so the
    // canonical UI prints first, then we persist.  Replaces the
    // single on_tool callback that streaming.attach(cli) just
    // installed — we keep its content/reasoning callbacks intact
    // because cli.on_tool() only touches the tool slot.
    cli.on_tool([&streaming, &cli, &st, &o]
                (const easyai::ToolCall & call,
                 const easyai::ToolResult & result) {
        streaming.notify_tool(call, result);
        if (o.no_local_session) return;
        std::string save_err;
        if (!save_session(cli, o.session_file, &save_err)) {
            std::fprintf(stderr,
                "%s[easyai-cli] warning:%s checkpoint "
                "session file: %s\n",
                st.yellow(), st.reset(), save_err.c_str());
        }
    });

    spinner.initial_draw();
    spinner.start_heartbeat();
    // Enter the "thinking" state right after the request goes out so
    // the operator sees a left-to-right shimmer sweep across the word
    // "thinking <pct>%" while the server is processing the prompt
    // (i.e. before the first reasoning_content / content delta lands).
    // Streaming::on_token_ / on_reason_ / on_tool_ each call
    // spinner.set_thinking(false) on the first piece they receive, so
    // the shimmer is replaced by the regular `/<pct>%` glyph the
    // moment generation begins.  We also flip it off explicitly after
    // chat() returns to cover the no-output / error / cancel paths.
    spinner.set_thinking(true);
    // Real prompt-eval progress wired from the server's per-batch
    // easyai.prompt_progress events (mirrors llama-server's
    // `prompt_progress` shape). Without this the shimmer's "xx%"
    // would either be absent or fall back to stale ctx_pct from the
    // previous turn — neither of which tells the operator how far
    // through the prompt the model actually is.
    //
    // set_thinking(true) here is the multi-hop hook: in agentic flows
    // the server runs a fresh prompt-eval pass after every tool
    // dispatch (system + user + ... + tool_result + assistant primer
    // → llama_decode), and emits easyai.prompt_progress for each.
    // Without this re-entry the shimmer would only show on the very
    // first hop; subsequent "model digesting tool result" windows
    // would silently fall back to the rotating |/- glyph and the
    // operator would have no visual cue that real work is happening.
    // Idempotent on hop 0 (already in thinking mode from the
    // explicit set_thinking(true) above); the next on_token /
    // on_reason / on_tool from Streaming flips it back off as soon
    // as generation begins for that hop.
    cli.on_prompt_progress(
        [&spinner](int processed, int total, int /*cached*/, double /*ms*/) {
            if (total <= 0) return;
            spinner.set_thinking(true);
            spinner.set_thinking_pct((int)(100.0 * processed / total));
        });

    // Mark in-flight: the signal handler reads this to decide between
    // graceful (mid-chat) and prompt-level (between turns) handling.
    g_in_chat.store(true, std::memory_order_relaxed);
    std::string answer = cli.chat(prompt);
    g_in_chat.store(false, std::memory_order_relaxed);

    spinner.set_thinking(false);
    spinner.stop_heartbeat();
    spinner.finish();
    std::fputc('\n', stdout);

    // Persist the post-turn state to the session file.  Best-effort: a
    // disk failure here doesn't fail the turn (the model already
    // replied; the user already saw the answer), but the warning lets
    // the operator notice an out-of-space / permission issue.  Atomic
    // tempfile + rename — a partial write never replaces the prior
    // session file. Skipped entirely when --no-local-session is set.
    if (!o.no_local_session) {
        std::string save_err;
        if (!save_session(cli, o.session_file, &save_err)) {
            std::fprintf(stderr,
                "%swarning:%s could not save session file: %s\n",
                st.yellow(), st.reset(), save_err.c_str());
        }
    }

    // Signal guard. Three outcomes:
    //   - graceful_exit (second Ctrl+C): print exit banner, return 0
    //     so the REPL breaks.
    //   - stopped (first Ctrl+C, generation cancelled): print a soft
    //     "stopped" footer and return 0 — the REPL continues.
    //   - quiet-mode cancel: return 130 for the script.
    if (g_graceful_exit.load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "%s── exited ──%s\n",
                     st.dim(), st.reset());
        return 0;
    }
    if (g_signal_count.load(std::memory_order_relaxed) > 0) {
        if (g_quiet_mode.load(std::memory_order_relaxed)) {
            std::fprintf(stderr, "%s── cancelled ──%s\n",
                         st.yellow(), st.reset());
            return 130;
        }
        std::fprintf(stderr, "%s── stopped ──%s\n",
                     st.dim(), st.reset());
        return 0;
    }

    // Context-full guard fires BEFORE the generic error/incomplete
    // banners — it's the cleanest of the bad outcomes (the model
    // produced a partial reply, we just have nowhere to put more
    // tokens).  Show the latest content (already streamed) plus a
    // distinct "context full" note so the operator knows to start a
    // new chat instead of debugging a "tool didn't fire" loop.
    if (cli.last_was_ctx_full()) {
        std::fprintf(stdout,
            "\n%s── context full ──%s\n"
            "%s%s%s\n"
            "Start a new conversation (or shorten the prompt) to keep going.\n",
            st.yellow(), st.reset(),
            st.dim(), cli.last_error().c_str(), st.reset());
        return 0;
    }
    if (answer.empty() && !cli.last_error().empty()) {
        std::fprintf(stderr, "%serror:%s %s\n", st.red(), st.reset(),
                     cli.last_error().c_str());
        return 1;
    }
    // Single placeholder path, fed by the same `timings.incomplete`
    // signal the webui consumes — the two surfaces report the same
    // diagnosis for the same turn.  Triggers when the server flagged
    // the turn (no tool_call, content < 80 bytes) OR the answer
    // string came back empty for any other reason.  When
    // --retry-on-incomplete was on this only fires AFTER the retry
    // also failed.
    if (answer.empty() || cli.last_turn_was_incomplete()) {
        std::fprintf(stdout,
            "%s(incomplete response — the model produced no tool_call and "
            "only a tiny visible reply, AND the auto-retry with corrective "
            "nudge ALSO failed%s. The model is repeatedly announcing a "
            "tool without emitting it. Try rephrasing more specifically "
            "(e.g. \"use write_file to save X to Y\"), shorten the prompt, "
            "or check the tool list — a missing tool the model thinks it "
            "needs can produce this loop.)%s\n",
            st.yellow(),
            o.retry_on_incomplete ? "" : " (auto-retry is OFF)",
            st.reset());
    }
    return 0;
}

// ---- --shell mode --------------------------------------------------------
//
// Hybrid AI shell: the user's $SHELL executes normal commands; lines
// prefixed with > are forwarded to the AI model.  CWD and env vars
// persist across commands via builtin cd/export handling.  Ctrl+C
// cancels the running command or AI generation and returns to the
// prompt.  /exit or Ctrl+D to quit.

static std::string abbreviate_home(const std::string & path) {
    const char * home = std::getenv("HOME");
    if (!home) return path;
    std::string h = home;
    if (path.compare(0, h.size(), h) == 0
        && (path.size() == h.size() || path[h.size()] == '/'))
        return "~" + path.substr(h.size());
    return path;
}

static bool handle_shell_builtin(const std::string & cmd) {
    // cd [dir]
    if (cmd == "cd" || cmd.compare(0, 3, "cd ") == 0
                    || cmd.compare(0, 3, "cd\t") == 0) {
        std::string target;
        if (cmd.size() <= 3) {
            const char * home = std::getenv("HOME");
            target = home ? home : "/";
        } else {
            target = cmd.substr(3);
            // trim leading whitespace
            auto pos = target.find_first_not_of(" \t");
            if (pos != std::string::npos) target = target.substr(pos);
            // trim trailing whitespace
            pos = target.find_last_not_of(" \t");
            if (pos != std::string::npos) target.erase(pos + 1);
            // expand leading ~
            if (!target.empty() && target[0] == '~') {
                const char * home = std::getenv("HOME");
                if (home) target = std::string(home) + target.substr(1);
            }
            // cd - → OLDPWD
            if (target == "-") {
                const char * old = std::getenv("OLDPWD");
                if (!old) {
                    std::fprintf(stderr, "cd: OLDPWD not set\n");
                    return true;
                }
                target = old;
            }
        }
        char prev[PATH_MAX];
        if (::getcwd(prev, sizeof(prev)) != nullptr)
            ::setenv("OLDPWD", prev, 1);
        if (::chdir(target.c_str()) != 0) {
            std::fprintf(stderr, "cd: %s: %s\n",
                         target.c_str(), std::strerror(errno));
        }
        return true;
    }

    // export KEY=VALUE
    if (cmd.compare(0, 7, "export ") == 0) {
        std::string kv = cmd.substr(7);
        auto pos = kv.find_first_not_of(" \t");
        if (pos != std::string::npos) kv = kv.substr(pos);
        auto eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) {
            std::fprintf(stderr, "export: invalid format (expected KEY=VALUE)\n");
            return true;
        }
        std::string key = kv.substr(0, eq);
        std::string val = kv.substr(eq + 1);
        // strip surrounding quotes from value
        if (val.size() >= 2
            && ((val.front() == '"'  && val.back() == '"')
             || (val.front() == '\'' && val.back() == '\'')))
            val = val.substr(1, val.size() - 2);
        ::setenv(key.c_str(), val.c_str(), 1);
        return true;
    }

    // unset VAR
    if (cmd.compare(0, 6, "unset ") == 0) {
        std::string var = cmd.substr(6);
        auto pos = var.find_first_not_of(" \t");
        if (pos != std::string::npos) var = var.substr(pos);
        pos = var.find_last_not_of(" \t");
        if (pos != std::string::npos) var.erase(pos + 1);
        ::unsetenv(var.c_str());
        return true;
    }

    return false;
}

static void exec_shell_command(const char * shell, const std::string & cmd) {
    pid_t pid = ::fork();
    if (pid < 0) {
        std::fprintf(stderr, "fork: %s\n", std::strerror(errno));
        return;
    }
    if (pid == 0) {
        // Child — default signal disposition is restored by exec.
        ::signal(SIGINT,  SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::execl(shell, shell, "-c", cmd.c_str(), nullptr);
        ::_exit(127);
    }
    // Parent: mark shell-cmd state so the signal handler ignores SIGINT
    // (the child shares our process group and receives it directly).
    g_in_shell_cmd.store(true, std::memory_order_relaxed);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;  // retry on EINTR — our signal handler returns, waitpid resumes
    g_in_shell_cmd.store(false, std::memory_order_relaxed);
    g_signal_count.store(0, std::memory_order_relaxed);
}

int run_shell(easyai::Client & cli, easyai::Plan & plan,
              const Options & o, const Style & st) {
    const char * shell = std::getenv("SHELL");
    if (!shell || shell[0] == '\0') shell = "/bin/sh";

    std::fprintf(stderr,
        "%seasyai-shell%s — commands execute via %s%s%s.\n"
        "Prefix with %s>%s for AI prompts.  /exit to quit, /help for commands.\n"
        "Ctrl+C stops the current AI generation or command.\n",
        st.bold(),  st.reset(),
        st.bold(),  shell,  st.reset(),
        st.cyan(),  st.reset());

    std::string line;
    while (true) {
        cli.clear_cancel();
        g_signal_count.store(0, std::memory_order_relaxed);

        // CWD-aware prompt: ~/project $
        char cwd_buf[PATH_MAX];
        const char * cwd_raw = ::getcwd(cwd_buf, sizeof(cwd_buf));
        std::string cwd_display = cwd_raw ? abbreviate_home(cwd_raw) : "?";
        std::fprintf(stdout, "%s%s%s %s$%s ",
                     st.bold(), cwd_display.c_str(), st.reset(),
                     st.cyan(), st.reset());
        std::fflush(stdout);

        if (!std::getline(std::cin, line)) {
            std::fputc('\n', stdout);
            if (g_signal_count.load(std::memory_order_relaxed) > 0) {
                std::cin.clear();
                std::clearerr(stdin);
                continue;
            }
            break;  // EOF
        }
        if (line.empty()) continue;

        // Slash commands — same as REPL
        auto save_after_mutation = [&]() {
            if (o.no_local_session) return;
            std::string save_err;
            if (!save_session(cli, o.session_file, &save_err)) {
                std::fprintf(stderr,
                    "%swarning:%s could not save session file: %s\n",
                    st.yellow(), st.reset(), save_err.c_str());
            }
        };

        if (is_special(line, "/exit") || is_special(line, "/quit")) break;
        if (is_special(line, "/clear")) {
            cli.clear_history();
            save_after_mutation();
            std::fprintf(stderr, "%shistory cleared%s\n", st.dim(), st.reset());
            continue;
        }
        if (is_special(line, "/reset")) {
            cli.clear_history(); plan.clear();
            save_after_mutation();
            std::fprintf(stderr, "%shistory + plan cleared%s\n",
                         st.dim(), st.reset());
            continue;
        }
        if (is_special(line, "/compress")) {
            if (do_compress(cli, st)) save_after_mutation();
            continue;
        }
        if (is_special(line, "/plan"))  { render_plan(plan, st); continue; }
        if (is_special(line, "/tools")) {
            for (const auto & t : cli.tools()) {
                std::fprintf(stdout, "%s%s%s\n  %s%s%s\n",
                             st.bold(), t.name.c_str(), st.reset(),
                             st.dim(),  t.description.c_str(), st.reset());
            }
            continue;
        }
        if (is_special(line, "/help")) {
            std::fputs(
                "  > prompt      send prompt to AI\n"
                "  command       execute via shell\n"
                "  /exit /quit   leave\n"
                "  /clear        clear AI conversation\n"
                "  /reset        clear conversation + plan\n"
                "  /compress     recap session\n"
                "  /plan         show plan checklist\n"
                "  /tools        list AI tools\n",
                stdout);
            continue;
        }
        if (line[0] == '/' && line.size() > 1) {
            std::fprintf(stderr, "unknown command: %s — try /help\n",
                         line.c_str());
            continue;
        }

        // AI prompt: > prefix
        if (line[0] == '>') {
            std::string prompt = line.substr(1);
            auto pos = prompt.find_first_not_of(" \t");
            if (pos == std::string::npos) continue;
            prompt = prompt.substr(pos);
            run_one(cli, plan, prompt, o, st);
            if (g_graceful_exit.load(std::memory_order_relaxed)) break;
            continue;
        }

        // Shell builtins (cd, export, unset)
        if (handle_shell_builtin(line)) continue;

        // Normal shell command
        exec_shell_command(shell, line);
    }
    return 0;
}

int run_repl(easyai::Client & cli, easyai::Plan & plan,
             const Options & o, const Style & st) {
    std::fprintf(stderr,
        "%seasyai-cli-remote%s — interactive.  /exit to quit, /help for commands.\n"
        "Session auto-saves to %s.easyai_session%s in the current "
        "directory after every turn; pass %s--continue%s next time to "
        "resume.  /compress to recap mid-session.\n"
        "Ctrl+C stops AI generation and returns to the prompt.\n"
        "Ctrl+D or /exit to quit.\n",
        st.bold(), st.reset(),
        st.bold(), st.reset(),
        st.bold(), st.reset());
    std::string line;
    while (true) {
        cli.clear_cancel();
        g_signal_count.store(0, std::memory_order_relaxed);

        std::fprintf(stdout, "%s● %s", st.green(), st.reset());
        std::fflush(stdout);
        if (!std::getline(std::cin, line)) {
            std::fputc('\n', stdout);
            if (g_signal_count.load(std::memory_order_relaxed) > 0) {
                std::cin.clear();
                std::clearerr(stdin);
                continue;
            }
            break;
        }
        if (line.empty()) continue;

        // Best-effort save after a history-mutating slash command so
        // the session file reflects the post-command state. A disk
        // failure here is a warning, not a fatal — the REPL keeps
        // going. Skipped when --no-local-session is set.
        auto save_after_mutation = [&]() {
            if (o.no_local_session) return;
            std::string save_err;
            if (!save_session(cli, o.session_file, &save_err)) {
                std::fprintf(stderr,
                    "%swarning:%s could not save session file: %s\n",
                    st.yellow(), st.reset(), save_err.c_str());
            }
        };

        if (is_special(line, "/exit") || is_special(line, "/quit")) break;
        if (is_special(line, "/clear")) {
            cli.clear_history();
            save_after_mutation();
            std::fprintf(stderr, "%shistory cleared%s\n", st.dim(), st.reset());
            continue;
        }
        if (is_special(line, "/reset")) {
            cli.clear_history(); plan.clear();
            save_after_mutation();
            std::fprintf(stderr, "%shistory + plan cleared%s\n",
                         st.dim(), st.reset());
            continue;
        }
        if (is_special(line, "/compress")) {
            // do_compress() mutates history in place when it succeeds.
            // We save AFTER so .easyai_session carries the compressed
            // recap and a subsequent --continue picks it up.
            if (do_compress(cli, st)) {
                save_after_mutation();
            }
            continue;
        }
        if (is_special(line, "/plan")) { render_plan(plan, st); continue; }
        if (is_special(line, "/tools")) {
            for (const auto & t : cli.tools()) {
                std::fprintf(stdout, "%s%s%s\n  %s%s%s\n",
                             st.bold(), t.name.c_str(), st.reset(),
                             st.dim(),  t.description.c_str(), st.reset());
            }
            continue;
        }
        if (is_special(line, "/help")) {
            std::fputs(
                "/exit /quit /clear /reset /compress /plan /tools /help\n",
                stdout);
            continue;
        }
        if (line[0] == '/') {
            std::fprintf(stderr, "unknown command: %s — try /help\n", line.c_str());
            continue;
        }

        run_one(cli, plan, line, o, st);

        // Second Ctrl+C sets graceful_exit → quit the REPL.
        if (g_graceful_exit.load(std::memory_order_relaxed)) break;
    }
    return 0;
}

}  // namespace

int main(int argc, char ** argv) {
    Options o;
    if (!parse_args(argc, argv, o)) { usage(argv[0]); return 2; }

    // The library-side auto-log in src/log.cpp::auto_open opens a fresh
    // /tmp/easyai-client-<pid>-<epoch>.log on every Client construction
    // unless EASYAI_NO_AUTO_LOG is set.  We want logging to be OPT-IN
    // through --log-file PATH (or [cli] log_file) from the binary side;
    // otherwise no /tmp log files should appear.
    //
    // Precedence:
    //   - Operator-set EASYAI_NO_AUTO_LOG in env always wins (so
    //     `EASYAI_NO_AUTO_LOG=0 easyai-cli ...` still restores the
    //     legacy library log if the operator explicitly asks).
    //   - Otherwise, if `[cli] auto_log = on` is in the INI, leave the
    //     env var unset so the library opens its log.
    //   - Otherwise (default), set EASYAI_NO_AUTO_LOG=1 so the library
    //     skips its auto-open.  --log-file PATH from the binary side
    //     handles the logging via open_log_tee + cli.log_file(log_fp).
    if (std::getenv("EASYAI_NO_AUTO_LOG") == nullptr && !o.auto_log) {
        ::setenv("EASYAI_NO_AUTO_LOG", "1", /*overwrite=*/0);
    }
    Style st = easyai::ui::detect_style();

    // Validate --sandbox up front so the user gets a clear error
    // BEFORE the agent loop starts hitting "No such file or directory".
    if (std::string err; !easyai::cli::validate_sandbox(o.sandbox, err)) {
        std::fprintf(stderr, "%serror:%s %s\n",
                     st.red(), st.reset(), err.c_str());
        return 2;
    }

    // Anchor the process CWD to the sandbox if one was given. This
    // means: fs(action="cwd") reports the sandbox path, every external
    // tool with `cwd: "$SANDBOX"` resolves there at load time, and the
    // bash tool's relative paths land where the operator authorised
    // file access. Without --sandbox we leave CWD alone and the
    // model's "current directory" is wherever the user invoked us
    // from — which is fine for read-only tools but means file-writing
    // tools won't be registered (handled by --sandbox-gating above).
    if (!o.sandbox.empty()) {
        if (::chdir(o.sandbox.c_str()) != 0) {
            std::fprintf(stderr, "%serror:%s chdir(%s): %s\n",
                         st.red(), st.reset(),
                         o.sandbox.c_str(), std::strerror(errno));
            return 2;
        }
    }

    // Auto-prepend http:// when --url omits a scheme — convenience for
    // local dev so `--url ai.local:8080` Just Works.  https endpoints
    // still need the explicit `https://` prefix.
    if (!o.url.empty()
        && o.url.compare(0, 7, "http://")  != 0
        && o.url.compare(0, 8, "https://") != 0) {
        o.url = "http://" + o.url;
    }

    // Pipe / heredoc input — when stdin isn't a TTY and no prompt was
    // given on the command line, read all of stdin as a one-shot
    // prompt.  Lets you do:
    //   echo "que dia eh hoje" | easyai-cli-remote --url ai.local
    //   easyai-cli-remote --url ai.local <<EOF
    //   ... long question ...
    //   EOF
    if (o.prompt.empty() && !o.shell_mode && ::isatty(fileno(stdin)) == 0) {
        std::string buf, line;
        while (std::getline(std::cin, line)) {
            if (!buf.empty()) buf += "\n";
            buf += line;
        }
        if (!buf.empty()) o.prompt = std::move(buf);
    }

    // --shell implies --allow-bash and requires a TTY.
    if (o.shell_mode) {
        o.allow_bash = true;
        if (::isatty(fileno(stdin)) == 0) {
            std::fprintf(stderr, "%serror:%s --shell requires an interactive terminal\n",
                         st.red(), st.reset());
            return 2;
        }
    }

    // One-shot runs (--prompt / positional / piped stdin) imply
    // unattended: no human is at the REPL to answer a clarifying
    // question or pick from a menu. Explicit --unattended already won;
    // this just covers the common case where the operator forgot.
    if (!o.prompt.empty()) o.unattended = true;

    // Some diagnostics are purely LOCAL — no network call, so they
    // shouldn't require --url:
    //   --list-tools         (prints the tools registered in this CLI)
    //   --show-system-prompt (prints the resolved system prompt)
    // When ONLY one of these is requested, skip the --url requirement.
    const bool only_local_diag =
        (o.list_tools || o.show_system_prompt)
        && !o.list_models && !o.list_remote_tools && !o.health
        && !o.props && !o.metrics && o.set_preset.empty();
    if (!only_local_diag && o.url.empty()) {
        std::fprintf(stderr, "%serror:%s --url (or EASYAI_URL) is required\n",
                     st.red(), st.reset());
        usage(argv[0]);
        return 2;
    }

    // --log-file PATH is the ONLY way to materialise a raw transaction
    // log on disk now.  --verbose stays a pure stderr-verbosity knob;
    // the previous "verbose-implies-auto-/tmp-log" behaviour is gone
    // because operators kept ending up with a stale `/tmp/easyai-cli-
    // remote-<pid>-<epoch>.log` per session whether they wanted one or
    // not.  --log-file implies --verbose so the file carries CLI-side
    // diagnostics alongside the raw HTTP/SSE bytes (otherwise the log
    // would just be wire dumps with no context).
    if (!o.log_file_path.empty()) o.verbose = true;

    std::string resolved_log_path;
    std::FILE * log_fp = nullptr;
    if (!o.log_file_path.empty()) {
        log_fp = easyai::cli::open_log_tee(
            o.log_file_path, "easyai-cli-remote",
            argc, argv, &resolved_log_path);
        if (!log_fp) {
            std::fprintf(stderr,
                "%swarning:%s could not open log file %s — continuing without raw log.\n",
                st.yellow(), st.reset(), resolved_log_path.c_str());
        } else {
            std::fprintf(stderr,
                "%s[easyai-cli-remote]%s raw transaction log: %s%s%s\n",
                st.dim(), st.reset(),
                st.bold(), resolved_log_path.c_str(), st.reset());
        }
    }

    easyai::Client cli;
    if (log_fp) cli.log_file(log_fp);
    if (!o.url.empty()) cli.endpoint(o.url);
    cli.model(o.model).timeout_seconds(o.timeout).http_retries(o.http_retries);
    if (!o.api_key.empty())            cli.api_key(o.api_key);

    // Register tools UP FRONT — before the system-prompt prefix builder
    // appends the AVAILABLE TOOLS / VERIFY-BEFORE-YOU-CALL session-info
    // block (which needs cli.tools() to be populated) and before
    // --show-system-prompt dumps the resolved prompt (so the diagnostic
    // reflects what the model will actually see). The chat path / REPL
    // also rely on the catalogue being ready; --list-tools always did.
    easyai::Plan plan;
    register_tools(cli, plan, o, st);

    // Prefix the user's system prompt with two small in-binary blocks:
    //
    //   [environment] — the absolute path of the agent's sandbox root.
    //   Models without this typically waste turn 1 on
    //   `fs(action="sandbox")` / `pwd` before they can do anything
    //   useful. Injecting it up front saves the hop on every coding
    //   task.
    //
    //   [guidance] — "pick one implementation and ship it" assertiveness
    //   note. Smaller models otherwise enumerate options, ask
    //   permission for every choice, or stop at a draft. The user can
    //   refine after they see something running.
    //
    // Both blocks are conditional on the agent actually having a
    // create/mutate affordance (fs / bash / plan). With no such tool
    // the guidance is irrelevant and we leave the prompt alone.
    {
        const bool any_fs_like = o.allow_bash || !o.sandbox.empty();
        const bool any_prefix  = any_fs_like || !o.no_plan || o.unattended;
        std::string prefix;

        // [tool-discipline] — closed-set rule. The server's own system
        // prompt carries the authoritative AVAILABLE TOOLS catalogue
        // (rendered by `easyai::preamble::build_session_info(tools)`
        // server-side); we just state the rule here so the cli's
        // injected prefix doesn't lose it when --system overrides the
        // server default. Deliberately doesn't re-enumerate tools —
        // duplicating the server's session-info block would waste
        // tokens and risk drift.
        if (any_prefix) {
            prefix +=
                "[tool-discipline]\n"
                "Your tools are EXACTLY those listed in your tools "
                "schema for this session — the AVAILABLE TOOLS block "
                "is the authoritative catalogue. Do NOT invent tools. "
                "Do NOT call paraphrases of names you remember from "
                "other systems (`read_file` is not the same as the "
                "filesystem tool you actually have; `shell` is not "
                "the same as `bash`). When unsure of a name, call "
                "`tool_lookup` first — a no-match result is "
                "authoritative.\n"
                "\n"
                "If a request needs a capability with no matching tool, "
                "do the work in your visible reply. Asked to write a "
                "file / save a document / produce a manual and you have "
                "no write tool? Put the content DIRECTLY in the chat "
                "response — never paste it into a tool call that "
                "doesn't exist. Every hallucinated call returns "
                "`unknown tool` and wastes the turn.\n";
        }
        if (any_fs_like) {
            if (!prefix.empty()) prefix += "\n";
            std::string abs_root = o.sandbox.empty() ? "." : o.sandbox;
            char rb[PATH_MAX];
            if (::realpath(abs_root.c_str(), rb) != nullptr) abs_root = rb;
            prefix += "[environment]\n";
            prefix += "sandbox root: " + abs_root + "\n";
            prefix += "fs_* tools' virtual `/` maps here; bash runs with "
                      "this as its cwd.\n";
        }
        if (any_fs_like || !o.no_plan) {
            if (!prefix.empty()) prefix += "\n";
            prefix +=
                "[guidance]\n"
                "Stay strictly in scope. Build the simplest thing that "
                "does EXACTLY what the user asked. No extra features. "
                "No defensive scaffolding for cases they didn't mention. "
                "No \"while I'm at it\" cleanups. The user's request is "
                "the ceiling, not a starting point — they steer, you "
                "implement what they pick.\n";
        }
        // The old [tools] section lived here.  Its content — closed-set
        // rule + `tool_lookup` guidance — is now in
        // preamble::tools_block() emitted at the top of this prefix,
        // so the hand-rolled version is gone.
        // [unattended] — emitted on --unattended OR any one-shot mode
        // (--prompt / positional / piped stdin). Overrides the "ask
        // the user" parts of [guidance]: there's no REPL on the other
        // side to answer, so the model has to commit to a choice and
        // drive the task to completion in this turn.
        if (o.unattended) {
            if (!prefix.empty()) prefix += "\n";
            prefix +=
                "[unattended]\n"
                "This run is UNATTENDED — no human is at the terminal. "
                "You CANNOT ask clarifying questions, request approval "
                "before tool calls, present a numbered menu and wait, "
                "or pause for confirmation. The user has delegated full "
                "authority for this turn; nobody will read a follow-up "
                "question or pick an option.\n"
                "\n"
                "When the request is ambiguous, PICK the most reasonable "
                "interpretation, briefly note the choice in your final "
                "answer, and execute. Do not stop after a draft to ask "
                "\"should I continue?\" — carry the task to completion in "
                "this turn. This OVERRIDES step 3 of [guidance] above: "
                "instead of asking which next-step the user wants, list "
                "any ideas in your final answer and stop.\n";
        }
        // [cite-sources] — unconditional. The server's built-in prompt
        // carries the same rule, but a CLI that passes --system to
        // override the server default would otherwise lose it. Render
        // the centralised text here so server + local + cli all agree
        // verbatim. The memory-vocab preamble below ALSO re-emits the
        // block at the prompt tail when --memory is on, for models
        // that drop sources after a long reasoning trace. has_memory
        // gates the memory-tool bullets so we don't tell the model to
        // cite a tool that isn't registered this session.
        if (!prefix.empty()) prefix += "\n";
        prefix += easyai::preamble::cite_sources_block(
            /*has_memory=*/ !o.rag_dir.empty());
        if (!prefix.empty()) {
            o.system_prompt = o.system_prompt.empty()
                                  ? prefix
                                  : prefix + "\n" + o.system_prompt;
        }
    }

    // Memory vocabulary snapshot — when --memory is in use, append
    // the current keyword index so the model can see what it has
    // tagged without burning a memory(action="keywords") hop. The
    // remote server typically owns the date/time block, so we
    // pass inject_datetime=false: only the MEMORY VOCABULARY block
    // is rendered, and only when the store is non-empty.
    //
    // This is the same easyai::preamble::build() helper server.cpp
    // and local.cpp use, so the format stays in sync across all
    // three binaries — change the renderer once, every binary
    // updates.
    if (!o.rag_dir.empty()) {
        std::string vocab = easyai::preamble::build({
            /* inject_datetime  = */ false,
            /* knowledge_cutoff = */ std::string(),
            /* memory_root      = */ o.rag_dir,
            /* cite_sources     = */ true,
        });
        if (!vocab.empty()) {
            o.system_prompt += vocab;
        }
    }

    // AVAILABLE TOOLS + VERIFY-BEFORE-YOU-CALL block. easyai-cli's
    // system prompt is baked once at startup (no per-request rebuild
    // like the server does), and the tool registry doesn't change
    // mid-session, so we append the catalogue once here. The model
    // sees it every turn as part of the rolling system message —
    // slightly redundant after turn 1 but avoids needing first-turn
    // detection across the HTTP boundary. The block carries the
    // unbreakable "call tool_lookup first if unsure" rule which is
    // the actual fix for the qwen3-coder-next sub-action-as-tool
    // mistake (see 2026-05-24 session screenshot).
    {
        std::string si = easyai::preamble::build_session_info(cli.tools());
        if (!si.empty()) o.system_prompt += si;
    }

    // --show-system-prompt: dump the resolved prompt (built-in injection
    // + user --system / --system-file content) and exit before any HTTP
    // call. Doesn't need a working --url. The output is exactly what
    // would be sent to the server in the first request body's system
    // message — useful for confirming that the [environment] /
    // [guidance] blocks landed and that the user's persona is appended
    // correctly.
    if (o.show_system_prompt) {
        if (o.system_prompt.empty()) {
            std::fprintf(stderr,
                "(no system prompt — neither --system, --system-file, "
                "nor any tool that triggers the [environment] / [guidance] "
                "injection. The server's default persona handles this turn.)\n");
        } else {
            std::fputs(o.system_prompt.c_str(), stdout);
            std::fputc('\n', stdout);
        }
        // Tear down the log file if the libeasyai-cli auto-log path
        // opened one — we made no HTTP call, so the log is empty noise.
        if (log_fp) easyai::cli::close_log_tee(log_fp);
        return 0;
    }

    if (!o.system_prompt.empty())      cli.system(o.system_prompt);
    if (o.temperature       >= 0.0f)   cli.temperature(o.temperature);
    if (o.top_p             >= 0.0f)   cli.top_p(o.top_p);
    if (o.top_k             >= 0)      cli.top_k(o.top_k);
    if (o.min_p             >= 0.0f)   cli.min_p(o.min_p);
    if (o.repeat_penalty    >  0.0f)   cli.repeat_penalty(o.repeat_penalty);
    if (o.frequency_penalty > -2.0f)   cli.frequency_penalty(o.frequency_penalty);
    if (o.presence_penalty  > -2.0f)   cli.presence_penalty(o.presence_penalty);
    if (o.seed              >= 0)      cli.seed(o.seed);
    if (o.max_tokens        >= 0)      cli.max_tokens(o.max_tokens);
    if (!o.stop_sequences.empty())     cli.stop(o.stop_sequences);
    if (!o.extra_body.empty())         cli.extra_body_json(o.extra_body);
    if (o.verbose)                     cli.verbose(true);
    if (o.tls_insecure)                cli.tls_insecure(true);
    if (!o.tls_ca_path.empty())        cli.ca_cert_path(o.tls_ca_path);
    if (o.max_reasoning   > 0)         cli.max_reasoning_chars(o.max_reasoning);
    if (o.retry_on_incomplete)         cli.retry_on_incomplete(true);

    // (Plan + tool registration moved up front, right after the Client
    // is configured — needed there so the system-prompt builder can
    // call build_session_info(cli.tools()) and so --show-system-prompt
    // reflects the registered catalogue.)

    // --------------- session persistence --------------------------------
    // `.easyai_session` in cwd is the per-process state.  Default
    // behaviour: start fresh and overwrite any existing file on the
    // first turn.  `--continue` (or [cli] auto_continue = on) loads
    // the existing file before the first prompt.  `--compress` (or
    // [cli] auto_compress = on) asks the model for a lossless recap
    // on load and replaces history with that recap.  Compressing
    // implies loading first; running compress without auto_continue
    // is a no-op because there's no history in memory to recap.
    if (o.auto_continue && !any_management(o)) {
        std::string load_err;
        if (load_session(cli, o.session_file, &load_err)) {
            const auto path = session_file_path(o.session_file);
            std::fprintf(stderr,
                "%s[easyai-cli-remote]%s continued from %s%s%s%s\n",
                st.dim(),  st.reset(),
                st.bold(), path.filename().string().c_str(), st.reset(),
                o.no_local_session
                    ? " (read-only: --no-local-session is on)"
                    : "");
        }
        // No session file is a normal first-run state — start fresh
        // silently.  (Earlier versions warned here because --continue
        // was the explicit opt-in; with the auto-on default that
        // warning fires every time the operator opens a new project
        // dir, which is just noise.)

        if (o.auto_compress) {
            if (do_compress(cli, st)) {
                if (!o.no_local_session) {
                    std::string save_err;
                    if (!save_session(cli, o.session_file, &save_err)) {
                        std::fprintf(stderr,
                            "%swarning:%s could not persist compressed "
                            "session: %s\n",
                            st.yellow(), st.reset(), save_err.c_str());
                    }
                }
            }
        }
    } else if (o.auto_compress) {
        // auto_continue=off + auto_compress=on is contradictory:
        // nothing in memory to recap.  Don't error — just warn so
        // the operator notices the wiring is off.
        std::fprintf(stderr,
            "%s[easyai-cli-remote] warning:%s auto_compress is on "
            "but auto_continue is off — nothing to compress on a "
            "fresh session.\n", st.yellow(), st.reset());
    }

    auto close_log_fp = [&]() {
        easyai::cli::close_log_tee(log_fp);
        log_fp = nullptr;
    };

    // Wire SIGINT/SIGTERM so Ctrl+C aborts the in-flight stream, closes
    // the TCP connection, and lets the server cancel its decode loop —
    // instead of leaving the model running for minutes against a dead
    // socket and forcing an operator to restart the server.
    g_active_client = &cli;
    g_quiet_mode.store(o.quiet, std::memory_order_relaxed);
    install_cancel_handlers();

    int rc;
    if (any_management(o)) {
        rc = run_management(cli, o, st);
    } else if (o.shell_mode) {
        rc = run_shell(cli, plan, o, st);
    } else {
        if (!o.prompt.empty()) rc = run_one(cli, plan, o.prompt, o, st);
        else                   rc = run_repl(cli, plan, o, st);
    }
    g_active_client = nullptr;
    close_log_fp();
    return rc;
}
