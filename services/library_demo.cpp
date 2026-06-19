// services/library_demo.cpp
//
// Minimum-viable demo for the OpenAI-Python-SDK-shaped easyai::Session
// surface.  Two binaries-in-one:
//
//   $ easyai-library-demo --url http://127.0.0.1:8080
//   $ easyai-library-demo --model /path/to/model.gguf
//
// The point is to be a SHORT, COPYABLE template — not a feature-rich
// agent.  If you need a feature-rich agent, look at services/cli.cpp
// (HTTP transport) and services/local.cpp (in-process llama.cpp).
// This file exists to show third-party developers what the smallest
// "build an agent, register a tool, chat" program looks like.
//
// Demonstrates:
//   * Session::remote(url) — remote OpenAI-compatible server
//   * Session::local(path) — local llama.cpp
//   * Custom Tool with Tool::system_addendum (auto-appended to system)
//   * Session::system_append(...) for per-app prompt extensions
//   * Streaming via .on_token(...) callback
//
// Build (when examples are enabled): `make easyai-library-demo`.

#include "easyai/session.hpp"
#include "easyai/tool.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// ---- a tiny custom tool -----------------------------------------------------
// The model sees: name + short_description + JSON schema in the per-turn
// tools[] block.  Tool::system_addendum is appended to the system prompt
// once, by Session, when this tool is registered — no separate "remember
// to also update your system prompt" step.
static easyai::Tool make_acme_status_tool() {
    return easyai::Tool::builder("acme_status")
        .describe(
            "Return the current Acme service status. Use ONLY when the user "
            "asks about Acme uptime, outages, or service health.\n"
            "\n"
            "Returns a short status line plus the timestamp of the last check.")
        .short_describe(
            "Acme service status — call only on uptime / outage / health asks.")
        .system_addendum(
            "## Acme support guardrails\n"
            "* `acme_status` is the ONLY authoritative source of Acme status. "
            "Never speculate about outages from training data.\n"
            "* Mention Acme by full name on first reference each turn.")
        .handle([](const easyai::ToolCall &) {
            return easyai::ToolResult::ok(
                "status: green\nlast_check: 2026-05-27T12:00:00Z\n");
        })
        .build();
}

// ---- arg parsing ------------------------------------------------------------
struct Args {
    std::string url;
    std::string model_path;
    std::string prompt = "Hello — what's the status of Acme?";
    std::string sandbox;
    bool        allow_bash = false;
};

static void usage(const char * argv0) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s --url URL [--prompt TXT] [--sandbox DIR] [--allow-bash]\n"
        "  %s --model PATH [--prompt TXT] [--sandbox DIR] [--allow-bash]\n",
        argv0, argv0);
    std::exit(1);
}

static Args parse(int argc, char ** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&](const char * flag) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", flag);
                usage(argv[0]);
            }
            return std::string(argv[++i]);
        };
        if      (s == "--url")          a.url        = next("--url");
        else if (s == "--model")        a.model_path = next("--model");
        else if (s == "--prompt")       a.prompt     = next("--prompt");
        else if (s == "--sandbox")      a.sandbox    = next("--sandbox");
        else if (s == "--allow-bash")   a.allow_bash = true;
        else if (s == "-h" || s == "--help") usage(argv[0]);
        else { std::fprintf(stderr, "unknown arg: %s\n", s.c_str()); usage(argv[0]); }
    }
    if (a.url.empty() && a.model_path.empty()) usage(argv[0]);
    return a;
}

// ---- main -------------------------------------------------------------------
int main(int argc, char ** argv) {
    Args args = parse(argc, argv);

    // 1. Build the Session — five fluent lines.  The lib handles the
    //    canonical toolset, the system prompt, and the AI connection.
    auto session = args.url.empty()
        ? easyai::Session::local (args.model_path)
        : easyai::Session::remote(args.url);

    session
        .with_default_tools()        // datetime + web + tool_lookup baseline
        .sandbox(args.sandbox)
        .allow_bash(args.allow_bash)
        .system_append("Speak in plain English, max one short paragraph.")
        .add_tool(make_acme_status_tool())
        .on_token([](const std::string & piece) {
            // Stream tokens straight to stdout — same shape as
            // openai.ChatCompletion.create(stream=True) in Python.
            std::fputs(piece.c_str(), stdout);
            std::fflush(stdout);
        });

    // 2. Initialise — wires up the backend (Engine or Client) and
    //    composes the final system prompt from base + tool addenda +
    //    operator appends + datetime/cite-sources + tool catalogue.
    std::string err;
    if (!session.init(err)) {
        std::fprintf(stderr, "session init failed: %s\n", err.c_str());
        return 1;
    }

    // 3. Chat.  Returns the visible reply; tokens already streamed via
    //    the on_token callback above.
    std::string answer = session.chat(args.prompt);
    std::fputc('\n', stdout);

    if (answer.empty() && !session.last_error().empty()) {
        std::fprintf(stderr, "chat failed: %s\n",
                     session.last_error().c_str());
        return 1;
    }
    return 0;
}
