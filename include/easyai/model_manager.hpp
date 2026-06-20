// easyai/model_manager.hpp — terminal model manager (the /models dashboard,
// in the terminal).
//
// A full-screen, interactive TUI counterpart of the web /models dashboard:
// browse the HuggingFace recommendation catalogue with hardware-fit scoring,
// inspect and act on the locally-installed GGUFs (run / hot-swap / delete /
// symlink), download new GGUFs with a live progress bar, and watch a
// comprehensive live Status view (server, running model, active sampling
// parameters, services & tools, hardware, catalogue, downloads and request
// metrics — every field the dashboard's Status tab shows).
//
// Everything here drives the server's /models/api/* endpoints through an
// easyai::Client (get_json / post_json), so it works against any easyai-server
// reachable over the same OpenAI-compatible transport the rest of the CLI uses
// (including auth, retries and TLS).
//
//   easyai::manager::Options opt;
//   opt.url = "http://ai.local:8080";
//   opt.theme = "opencode";
//   easyai::manager::run(client, opt);          // --llm-manager
//   easyai::manager::print_status(client, st);  // --status / REPL /status
#pragma once

#include <cstdio>
#include <string>

namespace easyai {
class Client;
namespace ui { struct Style; }
}

namespace easyai::manager {

// Cosmetic / connection context for the manager screens. The Client owns
// the actual transport; these fields are surfaced in the header/footer so
// the operator always sees which server and model they're driving.
struct Options {
    std::string url;       // server endpoint (header/footer)
    std::string model;     // current model id (cosmetic; status reports live)
    std::string theme;     // "opencode" (default) | "opencode-light"
    std::string version;   // e.g. "easyai-cli 0.1.0" (header)
    // Password for a server whose /models gate is closed (--webui-password).
    // When the gate is closed and this is empty, the screens prompt for it
    // interactively (echo off) before the first request. Empty + open gate
    // → no login needed.
    std::string webui_password;
};

// Full-screen interactive model manager — tabs: Status · Local · Recommend ·
// Downloads. Sets up its own raw-mode alt-screen terminal and restores it on
// exit. Returns a process exit code (0 on a clean quit; non-zero only on a
// fatal terminal-setup failure, in which case nothing was drawn).
int run(Client & cli, const Options & opt);

// Full-screen, live-refreshing read-only Status view. Blocks until the user
// presses q / Esc / Ctrl-C. Reused by the chat TUI's /status command (which
// suspends its own terminal first). Returns 0.
int show_status_screen(Client & cli, const Options & opt);

// One-shot: GET /models/api/status and print the comprehensive status (every
// field the webui Status tab shows) to `out`, colorized per `st`. Backs
// `easyai-cli --status` and the REPL `/status` command. If the server's
// /models gate is closed it logs in first — using `webui_password`, or, on a
// TTY, prompting for it (echo off). Returns a process exit code (0 on success,
// 1 on transport / parse / auth failure — the reason is written to stderr).
int print_status(Client & cli, const ui::Style & st, std::FILE * out = stdout,
                 const std::string & webui_password = "");

}  // namespace easyai::manager
