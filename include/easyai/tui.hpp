// easyai/tui.hpp — full-screen interactive chat TUI for easyai-cli.
//
// Look & feel modelled on opencode's terminal UI (github.com/anomalyco/
// opencode, MIT) — re-implemented from scratch in C++17 on plain ANSI
// escape sequences. No ncurses, no third-party TUI dep, same hand-rolled
// philosophy as easyai/ui.hpp, just a full-screen retained renderer
// instead of an end-of-line spinner.
//
// What you get:
//   * alternate-screen chat view: user messages in a ┃-bordered panel,
//     assistant markdown, reasoning collapsed to a "Thought" line,
//     per-tool rows (read/write/edit/glob/grep/bash/web/...) with the
//     same icons and title shapes opencode uses, diff rendering for
//     edits, todo checklist blocks for the plan tool;
//   * a bordered multiline prompt with placeholder, paste support,
//     shift+enter newline (kitty/CSI-u aware, ctrl+j fallback),
//     @-file completion and /-command completion popups;
//   * status row (spinner + "esc interrupt") and footer (cwd, tool
//     count, /status hint) mirroring opencode's layout;
//   * modal select dialogs (/models, /theme, command palette) with
//     fuzzy filtering;
//   * flicker-free painting: full-frame diff + synchronized-update
//     brackets (CSI ?2026), wide-char aware wrapping.
//
// The TUI owns the Client's streaming callbacks for the duration of
// run() (on_token / on_reason / on_prompt_progress) and wraps every
// registered tool handler so it can show live "running…" state. The
// transport, history, session file and system prompt stay exactly as
// the caller configured them — this is a presentation layer only.
// History the caller pre-loaded (--continue / --session-file) is
// replayed into the scrollback on startup, closed by a "Resumed …"
// divider, so a resumed conversation is visible, not just in effect.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace easyai {
class Client;
class Plan;
}

namespace easyai::tui {

// True when stdin AND stdout are TTYs (the TUI needs both).
bool terminal_capable();

struct Options {
    std::string model;                 // model id shown under the prompt
    std::string url;                   // server endpoint (footer /status)
    std::string theme = "opencode";    // "opencode" | "opencode-light"
    std::string agent = "Build";       // agent label (prompt meta + ▣ row)
    std::string version;               // shown on the empty-session screen
    std::string cwd;                   // footer-left; empty → getcwd()
    std::string session_path;          // shown by /status
    bool        show_reasoning = true; // render reasoning parts at all
};

// Integration points the binary provides. All optional — a missing
// hook simply disables the matching command.
struct Hooks {
    // Persist the session after a mutation. Return false + set err to
    // surface a warning toast.
    std::function<bool(std::string * err)> save_session;
    // Compact the conversation (the /compress flow). Runs on the
    // worker thread while the UI shows a "Compacting…" status.
    std::function<bool(std::string * err)> compress;
    // /models dialog source + apply.
    std::function<std::vector<std::string>()> list_models;
    std::function<void(const std::string &)>  set_model;
};

// Run the interactive loop until /exit, double ctrl+c, or ctrl+d on an
// empty prompt. Returns the process exit code. The Client must already
// be fully configured (endpoint, system prompt, tools, history).
int run(Client & cli, Plan & plan, const Options & opt, const Hooks & hooks);

// ---------- question bridge -------------------------------------------------
//
// The builtin `question` tool (easyai::tools::question()) calls
// ask_questions() to pop the TUI's modal Q&A dialog and block until the
// user answers. Outside run() (no TUI active) asker_active() is false
// and the tool returns a clean "no interactive terminal" error instead.
struct QuestionOption {
    std::string label;
    std::string description;
};
struct QuestionItem {
    std::string question;                 // full question text
    std::string header;                   // short chip, e.g. "Theme"
    bool        multiple = false;         // allow multi-select
    bool        custom   = true;          // offer a free-text answer
    std::vector<QuestionOption> options;
};

bool asker_active();
// Returns false when the user dismissed the dialog (esc). On success
// `answers_out[i]` holds the selected label(s) for items[i].
bool ask_questions(const std::vector<QuestionItem> & items,
                   std::vector<std::vector<std::string>> & answers_out);

}  // namespace easyai::tui
