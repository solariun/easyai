// =============================================================================
//  easyai-local — drop-in REPL like llama-cli, run a GGUF model in-process.
//
//   * LOCAL ONLY.  Loads a GGUF and runs the model in this process via
//     easyai::Engine.  For talking to a remote OpenAI-compatible endpoint,
//     see `easyai-cli` (agentic HTTP/SSE client built on libeasyai-cli).
//   * One-shot mode (`-p`/`--prompt`) so it slots into shell scripts and
//     pipelines.  Banners go to stderr; only the model's text goes to
//     stdout, so `result=$(easyai-local -p '...')` works.
//   * Streams tokens token-by-token, with a live spinner + context-fill
//     gauge (`|45%`) — `--quiet`/`-q` for batch / scripted callers.
//   * Inline preset commands (`creative 0.9 …`) and slash commands
//     (`/temp 0.5`, `/system …`, `/reset`, `/tools`, …).
//   * Optional `<think>…</think>` stripper for noisy reasoning models —
//     thinking is shown by default, `--no-think` turns suppression on.
//
//  Examples:
//
//    easyai-local -m models/qwen2.5-1.5b.gguf
//    easyai-local -m model.gguf -p "What is 2+2?"
//    easyai-local -m model.gguf --sandbox /tmp/work --allow-bash
//
//  Memory hygiene: only RAII / unique_ptr; no raw new/delete; signals only
//  flip flags / call cooperative shutdown.
// =============================================================================

#include "easyai/easyai.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <unistd.h>     // isatty, chdir
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
//  helpers
// ============================================================================
namespace {

// Pretty-print all built-in presets.
void print_presets() {
    std::fprintf(stderr, "\nAvailable presets:\n");
    for (const auto & p : easyai::all_presets()) {
        std::fprintf(stderr, "  %-14s  temp=%.2f top_p=%.2f top_k=%d  — %s\n",
                     p.name.c_str(), p.temperature, p.top_p, p.top_k,
                     p.description.c_str());
    }
    std::fprintf(stderr, "\nAlso: 'temp <number>', '/reset', '/tools', '/system <text>', '/quit'\n\n");
}

// Ctrl-C trap: a single SIGINT during generation interrupts; a second one
// inside ~1s exits the process. We only flip an std::atomic flag from inside
// the handler — never touch the engine from a signal context.
std::atomic<bool>    g_interrupt{false};
std::atomic<int64_t> g_last_sigint_ms{0};

void handle_sigint(int) {
    using namespace std::chrono;
    auto now = duration_cast<milliseconds>(
                   steady_clock::now().time_since_epoch()).count();
    if (now - g_last_sigint_ms.load() < 1000) {
        std::fprintf(stderr, "\n[easyai-local] interrupted twice — exiting\n");
        std::_Exit(130);
    }
    g_last_sigint_ms.store(now);
    g_interrupt.store(true);
}
void install_sigint() { std::signal(SIGINT, handle_sigint); }

}  // namespace


// ============================================================================
//  Argument parsing
// ============================================================================
struct CliArgs {
    // -m / --model is required.
    std::string model_path;

    // common config
    std::string system_path;
    std::string system_inline;
    // Default preset: "precise" (temp=0.2, top_p=0.95, top_k=40, min_p=0.10).
    // Tuned for code, math, and factual Q&A — the dominant use case for
    // a local agent. Override with --preset (e.g. --preset balanced for
    // looser sampling, --preset creative for brainstorming).
    std::string preset = "precise";
    std::string prompt;          // -p one-shot mode; empty => REPL
    bool        no_think = false;
    bool        quiet    = false;   // --quiet/-q: disable spinner + ctx-% gauge
                                     // (batch / scripted / service usage)

    // sampling overrides — when set, win over the preset baseline.
    // Sentinel values: <0 / 0u means "unset" (use preset value).
    float temperature    = -1.0f;
    float top_p          = -1.0f;
    int   top_k          = -1;
    float min_p          = -1.0f;
    // 1.15 by default — anti-loop safety net (rephrasing loops on
    // thinking models). Pass --repeat-penalty 1.0 to disable.
    float repeat_penalty = 1.15f;
    int   max_tokens     = -1;     // -1 = until EOG / context full
    uint32_t seed        = 0u;     // 0 = leave as preset/library default

    // local engine tuning
    int  n_ctx = 4096, ngl = -1, n_threads = 0;
    int  n_batch = 0;              // 0 = follow ctx
    bool load_tools = true;
    std::string sandbox;        // empty = `fs` tool NOT registered
    bool allow_bash = false;    // explicit opt-in for `bash`
    // python3 defaults ON (auto-registers when --sandbox or
    // --allow-bash is set). Stdlib-only interpreter with disk access
    // restricted to the sandbox root via a Python preamble. Use
    // --no-python to opt out.
    bool allow_python = true;
    bool show_system_prompt = false;  // --show-system-prompt: dump and exit
    std::string external_tools_dir;     // optional external-tools dir (EASYAI-*.tools)
    std::string rag_dir;                 // optional RAG persistent-registry dir

    // KV cache controls
    std::string cache_type_k;      // empty = library default (f16)
    std::string cache_type_v;
    bool no_kv_offload = false;
    bool kv_unified    = false;
    std::vector<std::string> kv_overrides;  // each: "key=type:value"

    // speculative decoding
    std::string spec_type;
    std::string spec_draft_model;
    int         spec_draft_n_max = 0;
};

[[noreturn]] static void die_usage(const char * argv0) {
    std::fprintf(stderr,
        "Usage: %s -m model.gguf [options]\n\n"
        "Local-only REPL: loads a GGUF model in-process and chats.\n"
        "For a remote OpenAI-compatible endpoint, use `easyai-cli`.\n"
        "\nRequired:\n"
        "  -m, --model <path>            Local GGUF model file\n"
        "\nCommon options:\n"
        "  -p, --prompt <text>           One-shot: run prompt, print, exit\n"
        "  -s, --system-file <path>      Read system prompt from file\n"
        "      --system <text>           Inline system prompt\n"
        "      --show-system-prompt      Print the resolved system prompt\n"
        "                                 (built-in default OR --system OR\n"
        "                                  --system-file content) and exit.\n"
        "                                 Doesn't load the model — useful for\n"
        "                                 confirming what the model would see.\n"
        "      --preset <name>           Initial preset (default 'precise').\n"
        "                                 Choices: deterministic, precise,\n"
        "                                 balanced, creative, wild. See\n"
        "                                 README.md for what each implies.\n"
        "      --no-think                Strip <think>...</think> from output\n"
        "                                 (thinking is shown by default)\n"
        "  -q, --quiet                   Disable the spinner glyph + ctx-fill\n"
        "                                 gauge (e.g. |45%%).  For batch /\n"
        "                                 scripted runs where stdout is captured.\n"
        "\nSampling overrides (apply on top of --preset):\n"
        "      --temperature <f>         Override temperature (0.0-2.0)\n"
        "      --top-p <f>               Override nucleus sampling p\n"
        "      --top-k <n>               Override top-k\n"
        "      --min-p <f>               Override min-p\n"
        "      --repeat-penalty <f>      Repetition penalty (default 1.15 —\n"
        "                                anti-loop safety net; pass 1.0 to\n"
        "                                disable)\n"
        "      --max-tokens <n>          Cap tokens generated per turn\n"
        "      --seed <u32>              RNG seed (0 = random)\n"
        "\nEngine tuning:\n"
        "  -c, --ctx <n>                 Context size (default 4096)\n"
        "      --batch <n>               Logical batch size (default = ctx)\n"
        "      --ngl <n>                 GPU layers (-1=auto, 0=CPU)\n"
        "  -t, --threads <n>             CPU threads\n"
        "      --no-tools                Don't register the built-in toolbelt\n"
        "      --sandbox <dir>           Enable fs_* tools (read_file,\n"
        "                                 list_dir, glob, grep, write_file),\n"
        "                                 ALL scoped to <dir>. Without\n"
        "                                 --sandbox these tools are NOT\n"
        "                                 registered.\n"
        "      --allow-bash              Register the `bash` tool (run shell\n"
        "                                 commands). cwd = --sandbox dir if\n"
        "                                 given, otherwise CWD. NOT a\n"
        "                                 hardened sandbox — the command\n"
        "                                 runs with your user privileges.\n"
        "      --no-python               Drop the `python3` tool. By default\n"
        "                                 it auto-registers alongside `fs`\n"
        "                                 (whenever --sandbox or --allow-bash\n"
        "                                 is set). The interpreter is\n"
        "                                 stdlib-only (no PYTHON* env, no\n"
        "                                 site-packages, no cwd on sys.path)\n"
        "                                 and a Python preamble auto-\n"
        "                                 restricts disk access to the\n"
        "                                 sandbox root. NOT a hardened\n"
        "                                 sandbox — `import os`, `import\n"
        "                                 socket`, `import subprocess` all\n"
        "                                 work. Use --no-python to skip\n"
        "                                 registration entirely.\n"
        "      --external-tools <dir>    Load every EASYAI-*.tools file in <dir>\n"
        "                                 as an external-tools manifest. Empty\n"
        "                                 dir is a normal state (no extra tools).\n"
        "                                 Per-file errors are logged and skipped;\n"
        "                                 other files still load. -q hides the\n"
        "                                 security sanity-check warnings (errors\n"
        "                                 are always shown). See EXTERNAL_TOOLS.md.\n"
        "      --memory <dir>            Enable the agent's persistent memory\n"
        "                                 store (alias: --RAG). Each entry is\n"
        "                                 one Markdown file in <dir>.\n"
        "                                 Registers ONE `memory(action=...)`\n"
        "                                 tool with sub-actions save / append /\n"
        "                                 search / load / list / delete /\n"
        "                                 keywords. See RAG.md.\n"
        "\nKV cache (all optional):\n"
        " -ctk, --cache-type-k <type>    K-cache dtype (f32|f16|bf16|q8_0|q4_0|q4_1|q5_0|q5_1|iq4_nl)\n"
        " -ctv, --cache-type-v <type>    V-cache dtype (same options) — quantising V saves a lot of VRAM\n"
        "-nkvo, --no-kv-offload          Keep KV cache on CPU even with GPU layers\n"
        "      --kv-unified              Use a single unified KV buffer across sequences\n"
        "      --override-kv <k=t:v>     Override a GGUF metadata entry (repeatable).\n"
        "                                 Types: int|float|bool|str.\n"
        "                                 Example: --override-kv tokenizer.ggml.add_bos_token=bool:false\n"
        "\nSpeculative decoding (all optional):\n"
        "      --spec-type <type>        none (default), draft-mtp, draft-simple,\n"
        "                                 draft-eagle3, ngram-simple|map-k|map-k4v|\n"
        "                                 mod|cache.\n"
        "      --draft-model <path>      GGUF path for the draft model (draft-simple /\n"
        "                                 draft-eagle3). Must share vocabulary with the\n"
        "                                 target.\n"
        "      --spec-draft-n-max <n>    Max draft tokens per speculation step.\n"
        "                                 Typical MTP: 6. Default 16.\n"
        "\n  -h, --help                    Show this help and exit\n",
        argv0);
    std::exit(1);
}

static CliArgs parse(int argc, char ** argv) {
    CliArgs a;
    auto need = [&](int & i, const char * flag) -> const char * {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "missing value for %s\n", flag);
            die_usage(argv[0]);
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if      (s == "-m" || s == "--model")         a.model_path    = need(i, "-m");
        else if (s == "-s" || s == "--system-file")   a.system_path   = need(i, "-s");
        else if (s == "--system")                     a.system_inline = need(i, "--system");
        else if (s == "--preset")                     a.preset        = need(i, "--preset");
        else if (s == "-p" || s == "--prompt")        a.prompt        = need(i, "-p");
        else if (s == "--no-think")                   a.no_think      = true;
        else if (s == "-q" || s == "--quiet")         a.quiet         = true;
        else if (s == "--temperature" || s == "--temp") a.temperature  = std::atof(need(i, "--temperature"));
        else if (s == "--top-p")                      a.top_p         = std::atof(need(i, "--top-p"));
        else if (s == "--top-k")                      a.top_k         = std::atoi(need(i, "--top-k"));
        else if (s == "--min-p")                      a.min_p         = std::atof(need(i, "--min-p"));
        else if (s == "--repeat-penalty")             a.repeat_penalty= std::atof(need(i, "--repeat-penalty"));
        else if (s == "--max-tokens")                 a.max_tokens    = std::atoi(need(i, "--max-tokens"));
        else if (s == "--seed")                       a.seed          = (uint32_t) std::strtoul(need(i, "--seed"), nullptr, 10);
        else if (s == "-c" || s == "--ctx")           a.n_ctx         = std::atoi(need(i, "-c"));
        else if (s == "--batch")                      a.n_batch       = std::atoi(need(i, "--batch"));
        else if (s == "--ngl")                        a.ngl           = std::atoi(need(i, "--ngl"));
        else if (s == "-t" || s == "--threads")       a.n_threads     = std::atoi(need(i, "-t"));
        else if (s == "--no-tools")                   a.load_tools    = false;
        else if (s == "--sandbox")                    a.sandbox       = need(i, "--sandbox");
        else if (s == "--allow-bash")                 a.allow_bash    = true;
        else if (s == "--no-python")                  a.allow_python  = false;
        else if (s == "--external-tools")             a.external_tools_dir = need(i, "--external-tools");
        else if (s == "--memory" || s == "--RAG")     a.rag_dir            = need(i, s.c_str());
        else if (s == "--show-system-prompt")         a.show_system_prompt = true;
        // KV controls
        else if (s == "-ctk" || s == "--cache-type-k") a.cache_type_k = need(i, "-ctk");
        else if (s == "-ctv" || s == "--cache-type-v") a.cache_type_v = need(i, "-ctv");
        else if (s == "-nkvo" || s == "--no-kv-offload") a.no_kv_offload = true;
        else if (s == "--kv-unified")                 a.kv_unified    = true;
        else if (s == "--override-kv")                a.kv_overrides.push_back(need(i, "--override-kv"));
        // speculative decoding
        else if (s == "--spec-type")                 a.spec_type         = need(i, "--spec-type");
        else if (s == "--draft-model")               a.spec_draft_model  = need(i, "--draft-model");
        else if (s == "--spec-draft-n-max")          a.spec_draft_n_max  = std::atoi(need(i, "--spec-draft-n-max"));
        else if (s == "-h" || s == "--help")          die_usage(argv[0]);
        else { std::fprintf(stderr, "unknown arg: %s\n", s.c_str()); die_usage(argv[0]); }
    }
    // --show-system-prompt is a pure diagnostic: it doesn't load the
    // model, so don't insist on -m for that path.
    if (a.model_path.empty() && !a.show_system_prompt) {
        std::fprintf(stderr, "error: -m <model> is required\n\n");
        die_usage(argv[0]);
    }
    return a;
}

// Build the built-in system prompt with tool-notes bullets gated on which
// tools will actually be registered. Naming an unregistered tool here
// (e.g. bash with allow_bash=off) makes models try to call it.  Mirrors
// the gating LocalBackend / cli::Toolbelt apply at registration time.
static std::string build_builtin_system_prompt(const CliArgs & args) {
    // LocalBackend doesn't expose allow_fs separately; cli::Toolbelt's
    // predicate registers `fs` whenever sandbox is set OR a subprocess
    // executor (allow_bash / allow_python) is on.
    const bool fs_on       = !args.sandbox.empty() || args.allow_bash;
    const bool bash_on     = args.allow_bash;
    // python3 defaults ON; auto-on under same gate as `fs` (sandbox or
    // allow_bash); --no-python flips allow_python false to opt out.
    const bool python_on   = args.allow_python && fs_on;
    const bool web_on      = true;            // datetime + web are default-on with --no-tools=false
    const bool datetime_on = true;
    const bool rag_on      = !args.rag_dir.empty();

    std::string s;
    s.reserve(2048);

    s +=
        "You are a concise, honest assistant. Answer briefly; let the user "
        "steer.\n"
        "\n"
        "Answer directly for greetings, chitchat, math, and anything you "
        "already know — no tool needed.\n"
        "\n"
        "When a request truly needs work, run a tight loop:\n"
        "  1. Plan ONE small concrete next step (not a roadmap).\n"
        "  2. Act — call the tool in the same turn. Never announce a "
        "tool call without making it (\"I'll search…\" without the call "
        "is forbidden).\n"
        "  3. Read the result, then finish or take ONE more step.\n"
        "Stop as soon as you have something useful. Prefer a short answer "
        "the user can refine over a long pre-committed plan.\n"
        "\n"
        "## Tools — closed set\n"
        "Your tools are EXACTLY those listed in your tools schema for "
        "this session. Do NOT invent tools. Anything you remember from "
        "other AI systems or training that isn't in the schema is NOT "
        "available — including paraphrases (`read_file` is not `fs`; "
        "you must use `fs(action=\"read\")`; `shell` is not `bash`).\n"
        "\n"
        "Uncertain whether a name is registered? Call `tool_lookup` "
        "first. With no argument it returns the full numbered "
        "catalogue; with `name=\"<substring>\"` it confirms or denies a "
        "specific name (case-insensitive partial match). A no-match "
        "result is authoritative — do not retry with variations.\n"
        "\n"
        "If a request needs a capability with no matching tool, do the "
        "work in your visible reply. Asked to write a file / save a "
        "document / produce a manual and you have no write tool? Put "
        "the content DIRECTLY in the chat response — never paste it "
        "into a tool call that doesn't exist. Every hallucinated call "
        "returns `unknown tool` and wastes the turn.\n"
        "\n";

    const bool any_tool_note = datetime_on || web_on || fs_on || bash_on
                            || python_on || rag_on;
    if (any_tool_note) {
        // One-line trigger index. Each tool's full rules live in its
        // own description — these lines are just "when to reach for it".
        s += "Active tools (one-line triggers — see each tool's "
             "description for details):\n";
        if (datetime_on) {
            s += "  - datetime: call for 'now' / 'today' / 'latest'.\n";
        }
        if (web_on) {
            s += "  - web: search → fetch top 1-3 URLs → answer from "
                 "fetched text. REPLY MUST END WITH `Sources:` block "
                 "(see Cite sources rule below).\n";
        }
        if (rag_on) {
            s += "  - memory: search first for STABLE facts (vocab "
                 "appended below); save only DURABLE info, one "
                 "comprehensive entry per topic. REPLY MUST END WITH "
                 "`Sources:` block citing memory titles when you use "
                 "retrieved content (see Cite sources rule below).\n";
        }
        if (fs_on) {
            s += "  - fs: sandbox + check_path before any file work. "
                 "RELATIVE paths only.\n";
        }
        if (bash_on) {
            s += "  - bash: only for shell features fs/python can't "
                 "do (pipes, build runners, git, sed/awk).\n";
        }
        if (python_on) {
            s += "  - python3: stdlib-only; print() what you want "
                 "returned.\n";
        }
        s += "\n";
    }

    s +=
        "## Information pipeline (AUTHORITATIVE)\n"
        "When the request needs facts you don't already know, "
        "follow this order — strictly:\n"
        "\n";
    if (rag_on) {
        s +=
            "  1. MEMORY FIRST. memory(action=\"search\") with "
            "keywords from the vocabulary appended below. If memory "
            "returns enough to answer, SKIP the web and go straight "
            "to step 3.\n"
            "  2. WEB only if memory had nothing or was "
            "insufficient. ONE web search, then web_fetch the top "
            "1-3 URLs.\n"
            "  3. ANSWER. As soon as steps 1-2 give you enough, "
            "answer the user. Don't re-search memory, don't "
            "re-search the web, don't save more memories first.\n";
    } else {
        s +=
            "  1. WEB if you don't already know. ONE web search, "
            "then web_fetch the top 1-3 URLs.\n"
            "  2. ANSWER. As soon as the fetched text gives you "
            "enough, answer. Don't re-search the same query.\n";
    }
    s +=
        "\n"
        "STOP SIGNAL. After each tool result, ask: do I have enough "
        "now? Yes → answer immediately. No → ONE more focused tool "
        "call, then re-check. Three or more tool calls in a row "
        "without re-checking is a bug — you're exploring instead of "
        "answering.\n"
        "\n"
        "BUGS TO AVOID:\n";
    if (rag_on) {
        s +=
            "  - Skipping memory and going straight to web when "
            "memory is enabled.\n"
            "  - After a memory load returns a stable fact "
            "(definition, syntax, architecture), re-verifying with "
            "the web — only do this when the user asked for "
            "\"latest\" / \"current\" / dated info.\n"
            "  - After saving a memory, re-searching the web on the "
            "same topic in the same turn — the save means you "
            "already learned what you needed.\n";
    }
    s +=
        "  - Looping verify → save → re-verify.\n"
        "  - Running the same web query twice in a row.\n"
        "\n"
        "Saving new memories (when the info is durable — see the "
        "memory tool's GUIDELINES) happens AFTER your reply is "
        "written, as a final tool call. It's not another "
        "verification step.\n"
        "\n"
        "## Stop when you have enough — the user can refine (AUTHORITATIVE)\n"
        "Go only as far as the searches you need to give a useful "
        "answer to THIS question. Not the perfect answer, not the "
        "exhaustive one — a useful one.\n"
        "\n"
        "The user is on the other side of a chat box. They CAN send "
        "you another message — refining the query, narrowing the "
        "scope, asking for more depth on one point. They CANNOT "
        "interrupt your tool loop or skim while you fetch URL #8. "
        "Their cost of asking a follow-up is one sentence; your "
        "cost of an extra round of searches is their wait time.\n"
        "\n"
        "Practical shape: 1-3 targeted searches, then answer with "
        "what you have. If something's missing, name it in the "
        "reply (\"I couldn't find X — want me to check Y instead?\"). "
        "Let the user steer the next step.\n"
        "\n"
        "## Stay strictly in scope (AUTHORITATIVE)\n"
        "Do EXACTLY what the user asked — no more, no less. No extra "
        "features, no defensive scaffolding for cases they didn't "
        "mention, no \"while I'm at it\" cleanups, no proactive "
        "refactors. The request is the ceiling, not a starting point. "
        "If genuinely unsure what's in scope, ASK before acting — "
        "don't expand the task to be safe.\n"
        "\n"
        "";
    // Citation rule lives in libeasyai now (preamble.hpp) so server,
    // local, and cli render the exact same text. easyai-local appends
    // it once here; the preamble path adds a second copy at the end
    // of the prompt when --memory is enabled, for models (notably
    // Qwen3.x reasoning fine-tunes) that drop the Sources block
    // after a long <think> trace. has_memory gates the memory-tool
    // bullets so we don't tell the model to cite a tool that isn't
    // wired up this run.
    s += easyai::preamble::cite_sources_block(/*has_memory=*/ rag_on);
    return s;
}

// ============================================================================
//  main
// ============================================================================
int main(int argc, char ** argv) {
    CliArgs args = parse(argc, argv);
    install_sigint();

    // Anchor process cwd to --sandbox so fs(action="cwd") surfaces the
    // path the operator authorised, and so $SANDBOX placeholders in
    // any --external-tools manifests resolve to the right directory at
    // load time. Same idiom used by easyai-server / easyai-cli.
    if (!args.sandbox.empty()) {
        if (::chdir(args.sandbox.c_str()) != 0) {
            std::fprintf(stderr,
                "[easyai-local] chdir(%s): %s\n",
                args.sandbox.c_str(), std::strerror(errno));
            return 2;
        }
    }

    // Resolve system prompt: --system inline > -s file > built-in default.
    // Kept deliberately short. Goal: get a useful answer fast and let
    // the user refine — no walls of text, no pre-committed roadmaps.
    // The "Tool notes:" section only mentions tools that will actually
    // be registered for THIS invocation — naming an unregistered tool
    // (e.g. bash with allow_bash=off) makes models hallucinate calls
    // to it. Mirrors the gating in LocalBackend / cli::Toolbelt.
    std::string system_prompt = args.system_inline;
    if (system_prompt.empty() && !args.system_path.empty()) {
        easyai::text::slurp_file(args.system_path, system_prompt);
        if (system_prompt.empty()) {
            std::fprintf(stderr, "[easyai-local] WARNING: failed to read system file '%s'\n",
                         args.system_path.c_str());
        }
    }
    if (system_prompt.empty() && args.load_tools) {
        system_prompt = build_builtin_system_prompt(args);
    }

    // Memory vocabulary snapshot — appended once at startup when
    // --memory is enabled. Local mode rebuilds the prompt only at
    // process start, so the model sees the keyword index as it stood
    // when the binary launched; new memories saved mid-session are
    // visible to memory(action="search") but won't update the
    // injected vocabulary until the next run. Acceptable for the
    // one-shot / single-chat local pattern; the server gets a fresh
    // snapshot per request.
    //
    // We pass inject_datetime=false because LocalBackend doesn't
    // append a date/time block per turn; injecting it once at
    // startup would freeze "today" at whatever date the binary
    // launched, which is worse than no injection at all. Date is
    // expected to come from the datetime tool when needed.
    if (!system_prompt.empty() && !args.rag_dir.empty()) {
        std::string vocab = easyai::preamble::build({
            /* inject_datetime  = */ false,
            /* knowledge_cutoff = */ std::string(),
            /* memory_root      = */ args.rag_dir,
            /* cite_sources     = */ true,
        });
        if (!vocab.empty()) system_prompt += vocab;
    }

    // --show-system-prompt: dump the resolved prompt to stdout and exit
    // before any model is loaded. Doesn't need -m / a working sandbox /
    // anything else — purely a "what would the model see?" diagnostic.
    if (args.show_system_prompt) {
        std::fputs(system_prompt.c_str(), stdout);
        std::fputc('\n', stdout);
        return 0;
    }

    const easyai::Preset * p0 = easyai::find_preset(args.preset);
    easyai::Preset preset = p0 ? *p0 : *easyai::find_preset("precise");
    // Overlay any explicit --temperature/--top-p/--top-k/--min-p on top of the
    // chosen preset so the user's flags always win.
    if (args.temperature >= 0) preset.temperature = args.temperature;
    if (args.top_p       >= 0) preset.top_p       = args.top_p;
    if (args.top_k       >= 0) preset.top_k       = args.top_k;
    if (args.min_p       >= 0) preset.min_p       = args.min_p;

    // ----- build backend ---------------------------------------------------
    easyai::LocalBackend::Config lc;
    lc.model_path     = args.model_path;
    lc.system_prompt  = system_prompt;
    lc.sandbox        = args.sandbox;
    lc.allow_bash     = args.allow_bash;
    lc.allow_python   = args.allow_python;
    lc.external_tools_dir = args.external_tools_dir;
    lc.quiet              = args.quiet;
    lc.rag_dir            = args.rag_dir;
    lc.n_ctx          = args.n_ctx;
    lc.n_batch        = args.n_batch;
    lc.ngl            = args.ngl;
    lc.n_threads      = args.n_threads;
    lc.load_tools     = args.load_tools;
    lc.preset         = preset;
    lc.repeat_penalty = args.repeat_penalty;
    lc.max_tokens     = args.max_tokens;
    lc.seed           = args.seed;
    lc.cache_type_k   = args.cache_type_k;
    lc.cache_type_v   = args.cache_type_v;
    lc.no_kv_offload     = args.no_kv_offload;
    lc.kv_unified        = args.kv_unified;
    lc.kv_overrides      = args.kv_overrides;
    lc.spec_type         = args.spec_type;
    lc.spec_draft_model  = args.spec_draft_model;
    lc.spec_draft_n_max  = args.spec_draft_n_max;
    auto backend = std::make_unique<easyai::LocalBackend>(std::move(lc));

    std::string err;
    if (!backend->init(err)) {
        std::fprintf(stderr, "[easyai-local] init failed: %s\n", err.c_str());
        return 1;
    }

    // ----- one-shot mode --------------------------------------------------
    if (!args.prompt.empty()) {
        // Banners → stderr so stdout is clean for piping.
        std::fprintf(stderr, "[easyai-local] %s\n", backend->info().c_str());

        easyai::text::ThinkStripper strip;
        strip.enabled = args.no_think;
        easyai::ui::Spinner spinner(/*enabled=*/!args.quiet);
        spinner.start_heartbeat();

        // Honour an inline preset prefix in the prompt too.
        std::string text = args.prompt;
        easyai::PresetResult pr = easyai::parse_preset(text);
        if (!pr.applied.empty()) {
            backend->set_sampling(pr.temperature, pr.top_p, pr.top_k, pr.min_p);
            std::fprintf(stderr, "[preset → %s]\n", pr.applied.c_str());
            text = text.substr(pr.consumed);
        }

        try {
            backend->chat(text, [&](const std::string & p){
                std::string visible = strip.filter(p);
                if (!visible.empty()) spinner.write(visible);
                // Refresh the ctx-fill gauge each token so `|45%`
                // tracks the cursor live.  No-op when --quiet (the
                // spinner is disabled and ignores set_context_pct).
                int pct = backend->ctx_pct();
                if (pct >= 0) spinner.set_context_pct(pct);
            });
        } catch (const std::exception & e) {
            spinner.stop_heartbeat();
            spinner.finish();
            std::fprintf(stderr, "\n[easyai-local] error: %s\n", e.what());
            return 1;
        }
        std::string tail = strip.flush();
        if (!tail.empty()) spinner.write(tail);
        spinner.stop_heartbeat();
        spinner.finish();
        std::cout << std::endl;
        // Distinct ctx-full note before any other diagnostics — the
        // model produced a partial answer and the loop bailed because
        // n_ctx is full.  Operator needs to know to /reset (REPL) or
        // start a new process (one-shot).
        if (backend->last_was_ctx_full()) {
            std::fprintf(stderr,
                "\n── context full ──\n%s\n"
                "Start a new conversation (or shorten the prompt) to keep going.\n",
                backend->last_error().c_str());
        }
        return 0;
    }

    // ----- REPL mode ------------------------------------------------------
    std::fprintf(stderr,
        "[easyai-local] %s  preset=%s%s\n"
        "             type '/help' for commands, '/quit' to exit\n",
        backend->info().c_str(), preset.name.c_str(),
        args.no_think ? "  [no-think]" : "");

    easyai::text::ThinkStripper strip;
    strip.enabled = args.no_think;
    easyai::ui::Spinner spinner(/*enabled=*/!args.quiet);

    std::string line;
    while (true) {
        std::cout << "\n\033[32m> \033[0m" << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        // -------- meta-commands -------------------------------------------
        if (line == "/quit" || line == "/exit") break;
        if (line == "/help" || line == "/?")    { print_presets(); continue; }
        if (line == "/reset") {
            backend->reset();
            strip.reset();
            std::cout << "[history cleared]\n";
            continue;
        }
        if (line == "/think")    { strip.enabled = false; std::cout << "[thinking shown]\n"; continue; }
        if (line == "/no-think") { strip.enabled = true;  std::cout << "[thinking hidden]\n"; continue; }
        if (line == "/tools") {
            for (const auto & [n, d] : backend->tool_list()) {
                std::cout << "  " << n << " — " << d << "\n";
            }
            if (backend->tool_count() == 0) std::cout << "[no tools registered]\n";
            continue;
        }
        if (line.rfind("/system ", 0) == 0) {
            backend->set_system(line.substr(8));
            std::cout << "[system prompt updated; history cleared]\n";
            continue;
        }

        // -------- preset / temperature command ----------------------------
        easyai::PresetResult pr = easyai::parse_preset(line);
        if (!pr.applied.empty()) {
            backend->set_sampling(pr.temperature, pr.top_p, pr.top_k, pr.min_p);
            std::fprintf(stderr, "[preset → %s]\n", pr.applied.c_str());
            if (pr.consumed >= line.size()) continue;
            line = line.substr(pr.consumed);
        }

        // -------- normal generation ---------------------------------------
        g_interrupt.store(false);
        std::cout << "\033[33m";
        spinner.start_heartbeat();
        try {
            backend->chat(line, [&](const std::string & p){
                std::string visible = strip.filter(p);
                if (!visible.empty()) spinner.write(visible);
                int pct = backend->ctx_pct();
                if (pct >= 0) spinner.set_context_pct(pct);
            });
            std::string tail = strip.flush();
            if (!tail.empty()) spinner.write(tail);
            spinner.stop_heartbeat();
            spinner.finish();
        } catch (const std::exception & e) {
            spinner.stop_heartbeat();
            spinner.finish();
            std::fprintf(stderr, "\n[easyai-local] error: %s\n", e.what());
        }
        std::cout << "\033[0m" << std::endl;

        // Distinct context-full banner — bail-out at the wall, not an
        // error.  REPL stays open; the operator can /reset and keep
        // going.  When this fires we suppress the generic last_error
        // line below since they'd be redundant.
        if (backend->last_was_ctx_full()) {
            std::fprintf(stderr,
                "── context full ──\n%s\n"
                "Use /reset to clear history and free the context window.\n",
                backend->last_error().c_str());
        } else if (!backend->last_error().empty()) {
            std::fprintf(stderr, "[easyai-local] %s\n", backend->last_error().c_str());
        }
    }

    std::fprintf(stderr, "[easyai-local] bye\n");
    return 0;
}
