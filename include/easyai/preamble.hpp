// easyai/preamble.hpp — the AUTHORITATIVE preamble appended to every
// system message before generation.
//
// This used to live as `build_authoritative_preamble` inside
// `examples/server.cpp`, with parallel copies in `examples/local.cpp`
// (memory-vocab portion only) and nothing in `examples/cli.cpp`. The
// drift was a smell: change the format here and you'd silently miss
// the other binaries. Now there's one builder; every consumer
// (server, local, cli, anything else linking libeasyai) calls it.
//
// What the preamble contains, in order:
//
//   1. # AUTHORITATIVE DATE/TIME — current wall-clock + timezone, so
//      the model anchors "today" / "now" to ground truth instead of
//      training-data intuition.
//
//   2. # KNOWLEDGE CUTOFF — a one-line reminder of where the model's
//      training data ends, plus a rule: post-cutoff facts must be
//      verified via a tool or stated as uncertain.
//
//   3. # MEMORY VOCABULARY — the keywords currently tagged in the
//      agent's persistent memory store, sorted by count desc / name
//      asc, capped at the top 40. Lets the model see what it can
//      memory(action="search") for without having to first call
//      memory(action="keywords"). Skipped when no memory store is
//      configured OR the store is empty / fully untagged.
//
// The function is STATELESS — every call recomputes (fresh date,
// fresh disk scan for the memory vocabulary). Cost is dominated by
// the memory directory walk (~10-50ms for typical stores; rounding
// error against inference latency). Safe to call on the hot path,
// safe to call from any thread, safe to call concurrently with
// memory tool writes (the underlying RagStore uses shared_mutex).
//
// WHERE TO ATTACH the returned string:
//   * Local in-process model — append once at startup to the
//     system prompt before constructing the Engine, OR call per
//     turn before each generate() if your memory is mutating.
//   * Network-facing server — call per request and append to
//     whichever system message goes into the model's prompt (see
//     examples/server.cpp's prepare_engine_for_request).
//   * Agentic HTTP client — call when building the system prompt
//     prefix and send the combined text as the system message.
#pragma once

#include "easyai/tool.hpp"

#include <string>
#include <vector>

namespace easyai::preamble {

// Snapshot of which tools are live for this session.  Passed to
// `build_builtin_system_prompt` so it can render an accurate "Active
// tools" block and gate the per-tool advisory paragraphs.  Server,
// local, and cli all populate one of these from their respective
// argument structs; the renderer is identical across binaries.
//
// Naming convention: the booleans are about INTENT (was the operator
// flag on?); `active_tools` is GROUND TRUTH (what's actually wired up).
// We prefer the latter for "list the names", but the booleans drive
// which advisory bullets to emit (e.g. python-read-only advice is
// only useful when python3 is registered).
struct ToolsetView {
    bool datetime_on    = false;
    bool web_on         = false;
    bool fs_on          = false;
    bool bash_on        = false;
    bool python_on      = false;
    bool memory_on      = false;
    bool tool_lookup_on = false;

    // Optional: the actual registered tool catalogue.  When present,
    // the rendered "tools available" list uses these names + their
    // wire_description() — strict ground truth, immune to the boolean
    // flags drifting out of sync with what was actually registered.
    // When empty, the renderer falls back to the booleans for naming.
    std::vector<easyai::Tool> active_tools;

    // True iff any flag is on (or active_tools is non-empty).  Used to
    // suppress the "no tools" advisory when nothing's wired.
    bool any() const {
        return datetime_on || web_on || fs_on || bash_on || python_on
            || memory_on || tool_lookup_on || !active_tools.empty();
    }
};

struct Options {
    // Date/time + knowledge-cutoff blocks. When false, both blocks
    // are skipped — useful for the HTTP-client case where the
    // remote server typically handles the date/time injection
    // itself.
    bool inject_datetime = true;

    // Model training-data cutoff hint. Mentioned in the date/time
    // block so post-cutoff hallucinations stand out. Empty string
    // → cutoff block omitted (the date/time block still renders if
    // inject_datetime is true).
    std::string knowledge_cutoff = "2024-10";

    // Memory store root (the --memory / --RAG directory). When
    // non-empty AND the store has at least one tagged entry, a
    // MEMORY VOCABULARY block is appended. Empty string OR empty
    // store → block omitted, no tokens wasted on "(nothing here)".
    std::string memory_root;

    // Append the CITE-SOURCES block (see cite_sources_block()).
    // Default off so the historical "empty Options → empty preamble"
    // contract still holds. Server, local, and cli all opt in.
    bool cite_sources = false;

    // When true, the cite_sources block emits its memory-tool bullets
    // (memory(action="search"), memory(action="load"), memory_search,
    // memory_load). When false, those bullets are omitted so the model
    // is not told to cite tools that aren't registered. Drive this from
    // whether the memory/RAG tool is actually wired up this session.
    // Ignored when cite_sources=false.
    bool has_memory = false;
};

// Build the AUTHORITATIVE preamble. Returns a string that should
// be appended verbatim to the system message — already prefixed
// with a blank line so it joins cleanly onto whatever came before.
//
// Empty Options (all defaults except memory_root="") still returns
// a non-empty string (the date/time + cutoff blocks). To get an
// empty string, set inject_datetime=false AND memory_root="".
std::string build(const Options & opt);

// Return the canonical CITE-SOURCES instruction block — strengthened
// and centralised so server/local/cli render the exact same text. The
// returned string starts with the section header (no leading "\n\n"),
// so callers control how it joins onto the surrounding prompt:
//   * build() prepends "\n\n" when emitting it from the preamble path.
//   * inline emitters (build_builtin_system_prompt, cli's prefix) just
//     append it where their other sections live.
//
// Third revision — Qwen3-coder-next and Gemma4 comply with the second
// revision, but Qwen3.6-class reasoning fine-tunes still drop the
// Sources block after a long <think> trace. This revision enumerates
// every triggering tool by exact call name, adds a POST-REASONING
// CHECKPOINT, and shows the memory citation format in the example.
//
// `has_memory` gates the memory(action=...) bullets: when false they
// are omitted so the model is not told to cite a tool that isn't
// registered this session.
std::string cite_sources_block(bool has_memory = true);

// Build the AVAILABLE TOOLS + VERIFY-BEFORE-YOU-CALL block. The
// returned string starts with a blank-line separator so it joins
// cleanly onto whatever came before. Returns "" if `tools` is empty.
//
// What this is for: weak tool-callers (notably Qwen3-Coder-Next) drift
// into hallucinated tool names — most often by calling a composite
// tool's sub-action as if it were a standalone tool
// (e.g. `update(...)` instead of `plan(action="update", ...)`). The
// block:
//   1. Lists every registered tool by canonical name, with the action
//      enum spelled out for composite tools, so the model sees the
//      exact dispatch shape.
//   2. Carries an UNBREAKABLE rule pointing at the most common
//      mistakes and instructing the model to call `tool_lookup` first
//      whenever a name was not previously confirmed.
//
// CALL SITE — emit this ONCE per session, on the FIRST user turn (no
// prior assistant message in the history). The existing preamble's
// datetime block is what refreshes per turn; re-emitting the tool
// catalogue every turn just burns tokens for no behavioural gain.
std::string build_session_info(const std::vector<easyai::Tool> & tools);

// Render the "Tools available this session" section.  Always emits the
// closed-set rule (the model must call only tools advertised this turn)
// and the python3 read-only / fs-and-bash-only-write policy.  The body
// enumerates the active tools by name + short_description (one line
// each) so the model has a stable, readable index alongside the
// machine-readable `<tools>` schema the chat template emits.
//
// Side-effect-free; safe to call per-request.
std::string tools_block(const ToolsetView & view);

// One full default system prompt, shared by easyai-local and
// easyai-server.  Previously each binary maintained its own ~180-line
// copy that drifted whenever someone touched one and not the other —
// see git blame on examples/server.cpp build_builtin_system_prompt and
// examples/local.cpp build_builtin_system_prompt circa 2026-Q1.  The
// renderer is now here; the binaries are 5-line wrappers.
//
// The output starts with the persona + reasoning rules, includes the
// tools_block(view), the information pipeline (memory→web→answer or
// web→answer), stop signals, scope discipline, and ends with the
// cite_sources_block().  Date/time and memory-vocabulary blocks are
// appended later by build() — this function builds only the STATIC
// portion that doesn't change per-request.
std::string build_builtin_system_prompt(const ToolsetView & view);

// Sanitize a multi-paragraph addendum before splicing it into the
// system prompt.  Strips C0 control bytes (0x00–0x1f) and DEL (0x7f)
// EXCEPT `\n` (0x0a) and `\t` (0x09), which are legitimate in
// multi-paragraph text; collapses any run of stripped bytes into a
// single space; UTF-8 multi-byte (0x80+) passes through unchanged;
// caps the output at `cap` bytes.
//
// Use this on every Tool::system_addendum / Config::system_appendix
// before concatenating it into the prompt.  Defends against:
//   * Terminal-escape injection via `--show-system-prompt` and the
//     CLI banners that print the resolved system prompt to a TTY
//     (same class as SECURITY_AUDIT §20.1 / §22.1).
//   * Structural corruption of the prompt when a future caller wires
//     the field from a less-trusted source (an external-tools
//     manifest, an MCP server's tool descriptor) — same class as
//     §23.1 for `tools_block`.
//
// `\n` is intentionally preserved so authored guardrail paragraphs
// retain their line structure when concatenated.  `\t` is preserved
// for code-block formatting.  Bell (0x07), ESC (0x1b), and the rest
// of C0 / DEL are stripped to a space.
std::string sanitize_addendum(const std::string & s, std::size_t cap);

// Compose the final system prompt for the given base + tool list. For
// every tool, appends `Tool::effective_system_addendum()` (which is
// `system_addendum` when the tool set one, else `description` as a
// fallback — see easyai/tool.hpp), each piped through
// `sanitize_addendum(_, 8192)` and separated by blank lines. Single
// source of truth for the addendum-concat policy that Session,
// LocalBackend, and RemoteBackend (cli-client) all execute — and the
// canonical "request the final system" entry point for binaries that
// want to dump or inspect what the model will receive.
//
// Pure function: no I/O, no global state, no mutation of inputs. Cheap
// to call (microseconds for typical tool counts) — safe to call on
// hot paths and from any thread.
//
// Does NOT include operator-supplied `system_appendix` or dynamic
// per-request preamble (datetime / memory vocabulary). Callers that
// want those should append `preamble::build(...)` separately and run
// their appendix through `sanitize_addendum` themselves — see
// LocalBackend::init() for the canonical post-compose tail.
std::string compose_system_prompt(const std::string             & base,
                                  const std::vector<easyai::Tool> & tools);

}  // namespace easyai::preamble
