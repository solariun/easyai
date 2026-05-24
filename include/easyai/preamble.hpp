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

}  // namespace easyai::preamble
