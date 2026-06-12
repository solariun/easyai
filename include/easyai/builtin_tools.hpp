// easyai/builtin_tools.hpp — batteries-included tools for agents.
#pragma once

#include "tool.hpp"

#include <functional>
#include <string>
#include <vector>

namespace easyai::tools {

// ---------- time --------------------------------------------------------
Tool datetime();   // returns current UTC + local time in ISO-8601

// ---------- web (requires libcurl at build time) ------------------------
// web: unified search + fetch tool. Single dispatcher with two actions:
//
//   action="search"  query → numbered title/url/snippet results
//                    engine: "auto" (default) cascades through
//                    google → brave → ddg-lite → bing → ddg and
//                    returns the first that succeeds. Explicit picks:
//                    "google" (Google Custom Search JSON API; needs
//                    GOOGLE_API_KEY + GOOGLE_CSE_ID env vars at call
//                    time AND `google_enabled=true` at registration —
//                    the operator-opt-in gate that keeps a billed
//                    third-party API off by default), "brave" (Brave
//                    HTML scrape; keyless, ~20 results per query, the
//                    keyless engine that best understands the full
//                    query — Bing RSS, by contrast, ignores quoted
//                    phrases and rare named entities; downside is
//                    Brave throttles single IPs aggressively),
//                    "ddg-lite" (DuckDuckGo's no-JS Lite endpoint
//                    `lite.duckduckgo.com/lite/` accessed with a
//                    Netscape Communicator 4.79 User-Agent; keyless,
//                    ~10 results, page 1 only — the Netscape UA
//                    bypasses the anti-bot wall the main DDG endpoint
//                    applies to scripted clients, and the result
//                    ranking is good for niche entity queries),
//                    "bing" (Bing RSS feed; keyless, captcha-free,
//                    ~10 results, no real pagination, weak query
//                    understanding for niche terms but stable),
//                    "ddg" (DuckDuckGo HTML scrape; keyless but
//                    increasingly blocked from server IPs by anti-bot
//                    heuristics). Page-based pagination over the
//                    engine's own ordering. The output's `engine:
//                    <name>` header tells the caller which backend
//                    actually answered.
//
//   action="fetch"   url → text content (HTML stripped, or raw with
//                    as_html=true). Pagination via start (byte offset)
//                    + limit (window size, default 8 KB).
//
// `google_enabled` only governs whether engine="google" is accepted at
// CALL time (or auto-cascade attempts it); the env vars are still
// re-read on every call so a key rotation mid-session works without a
// restart, and a missing key in the auto cascade is silently skipped
// (not a failure) so deployment without Google credentials still works.
Tool web(bool google_enabled = false);

// Focused per-action variants of `web` (web_search, web_fetch). Same
// handlers as the unified surface; smaller / quantised tool-callers
// work better with one verb per tool than with a `web(action="...")`
// dispatcher. The Toolbelt exposes this via tool_mode(Split | Both).
std::vector<Tool> web_split(bool google_enabled = false);

// ---------- filesystem --------------------------------------------------
// fs: unified filesystem tool. Single dispatcher with eight actions:
//
//   action="read"        read a UTF-8 file (path, offset, limit)
//   action="write"       write/append a UTF-8 file (path, content, append)
//   action="list"        non-recursive directory listing (path)
//   action="glob"        recursive wildcard file search (pattern, path)
//   action="grep"        recursive regex content search (pattern, path,
//                        file_glob, max_matches, case_insensitive)
//   action="check_path"  pre-flight stat + r/w/x probe (path, touch);
//                        the canonical "look before you leap" call —
//                        every other action's description tells the
//                        model to run this first when in doubt
//   action="cwd"         the process's current working directory at
//                        call time (getcwd)
//   action="sandbox"     the sandbox root, pinned at registration —
//                        the authoritative on-disk anchor every fs/bash
//                        action resolves RELATIVE paths against
//
// Sandboxed to `root`. Pass "" or "." to allow the process's current
// working directory tree. Path convention surfaced to the model:
// RELATIVE paths under the sandbox root (e.g. `report.md`,
// `src/main.cpp`, `.` for the root itself). Absolute / `..`-laden
// inputs are silently re-anchored under the root for safety, but the
// tool description tells the model to use relatives only — it makes
// the boundary obvious and matches what bash with the pinned cwd sees.
Tool fs(std::string root = ".");

// Focused per-action variants of `fs` (fs_read, fs_write, fs_append,
// fs_edit, fs_list, fs_glob, fs_grep, fs_check_path, fs_cwd,
// fs_sandbox). Same handlers as the unified surface — see fs_split's
// banner in builtin_tools.cpp for the rationale. Use the Toolbelt's
// tool_mode(Split | Both) to register these alongside (or instead of)
// the unified `fs`.
std::vector<Tool> fs_split(std::string root = ".");

// ---------- shell-class executors ---------------------------------------
// Run a shell command via /bin/sh -c. Working directory is set to `root`.
//
// IMPORTANT: this is NOT a hardened sandbox. The child runs with the
// caller's full uid/gid, can read/write any file the caller can, can
// hit the network, can spawn long-lived processes, etc. The only
// safety nets are:
//   - cwd is fixed to `root` (so the model's relative paths land there);
//   - merged stdout/stderr is captured and capped at 50 KB / 2000 lines;
//   - a hard timeout (default 120s, capped at 600s) sends SIGTERM then
//     SIGKILL.
// Caller is responsible for deciding whether bash is appropriate for
// their threat model — we surface it only when the user opts in
// (e.g. --allow-bash in cli-remote).
//
// `show_output`: when true, the merged child stdout/stderr is also
// mirrored to the parent's stderr in real time as it drains. The model
// still receives the full captured buffer as the tool result; the
// stderr mirror is a diagnostic aid for the operator watching the
// terminal so a long-running command (build, test suite) doesn't look
// like a stalled session. Off by default to keep tool output strictly
// in-band; the CLI flips it on unless --no-show-bash is passed.
Tool bash         (std::string root = ".", bool show_output = false);

// python3: run a Python 3 snippet via `python3 -I -S -E -c <code>`.
// Working directory is `root`. Same hardening as `bash` (cwd pinned,
// fds 3+ closed before exec, SIGTERM/SIGKILL deadline, 32 KB output
// cap, optional operator-facing stderr mirror via `show_output`).
//
// `-I -S -E` puts the interpreter in *isolated mode*: no PYTHON*
// environment variables (`-E`), no `site.py` / `.pth` files / site-
// packages auto-load (`-S`), no cwd on `sys.path` (`-I`, which also
// implies `-E -s`). The standard library is available; third-party
// packages are NOT. Imports beyond the stdlib will fail with
// ModuleNotFoundError.
//
// IMPORTANT: this is NOT a hardened sandbox. The interpreter runs
// with the caller's full uid/gid and can `import os`, `import socket`,
// `import urllib`, `import subprocess` to do anything bash can do.
// `-I -S -E` constrains *startup*, not capabilities. Caller is
// responsible for deciding whether `python3` is appropriate for
// their threat model — surfaced only when the operator opts in
// (e.g. `--allow-python`).
Tool python3      (std::string root = ".", bool show_output = false);

// ---------- interactivity -------------------------------------------------
// question: ask the human at the terminal 1-4 multiple-choice questions
// and block the agent turn until they answer. Bridges into the
// interactive TUI (easyai::tui::ask_questions); outside a TUI session
// the handler returns a clean "no interactive user available" error so
// unattended runs degrade gracefully. Register it only for interactive
// surfaces — the CLI does this automatically in TUI mode.
Tool question();

// ---------- introspection -----------------------------------------------
// tool_lookup: return the currently-registered tool catalogue, optionally
// filtered by a substring match on the tool name (case-insensitive,
// partial). Format is a numbered list 1..N with each tool's name and its
// declared description.
//
// The model uses this to verify what's actually wired up before it tries
// to call something. Without this tool, smaller models hallucinate
// "obvious" names (`write`, `read`, `ls`, `curl`, `python`) and produce
// "unknown tool: …" errors mid-task. With it, the model can sanity-check
// "is `write_file` actually available here?" in one hop, and only ever
// dispatches names that exist.
//
// `get_tools` is a callable that returns a snapshot of the live tool
// catalogue as (name, description) pairs.  Wire it at registration time
// to your Engine's or Client's `tools()` accessor:
//
//     engine.add_tool(easyai::tools::tool_lookup([&engine]() {
//         std::vector<std::pair<std::string,std::string>> v;
//         for (const auto & t : engine.tools()) {
//             v.emplace_back(t.name, t.description);
//         }
//         return v;
//     }));
//
// (Handlers — `std::function<...>` — aren't part of the snapshot, so
// no expensive closure copies. Add tool_lookup AFTER all other tools so
// it sees the complete list. tool_lookup's own entry will appear in
// the result, which is intentional: the model knows it has this
// affordance.)
//
// Parameters (the model fills these in):
//   `name` — string, optional. Substring; case-insensitive; matches the
//            tool name. Empty / missing = "list everything".
//
// Output shape (what the model sees):
//   - "1. <name>: <description>"
//   - "2. ..."
//   - "(no tools match: …)" when a filter found nothing
//   - "(no tools registered)" when the registry is empty
//
// Read-only over the registry; never spawns a process or touches the
// filesystem.
// One entry in the registry snapshot returned to `tool_lookup`'s
// handler. Both descriptions are present so the tool can show the
// short trigger string in the index view (no-arg call) and the full
// manual when a specific tool is asked for by name.
struct ToolCatalogEntry {
    std::string name;
    std::string short_description;
    std::string full_description;
};
using ToolCatalog    = std::vector<ToolCatalogEntry>;
using ToolListGetter = std::function<ToolCatalog()>;
Tool tool_lookup(ToolListGetter get_tools);

}  // namespace easyai::tools
