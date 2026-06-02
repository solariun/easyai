// easyai/remote_model_tool.hpp — "ai-<name>" tools: consult a remote
// OpenAI-protocol model as a peer.
//
// What this is
// ------------
// A built-in tool family that lets the running agent hand a hard
// question, a piece of its own reasoning, or a stuck task to ANOTHER
// AI model reachable over the network (any /v1/chat/completions
// endpoint — our easyai-server, an upstream llama-server, OpenAI, …).
// One connection becomes one tool: `[REMOTE_MODEL_<name>]` in the INI
// resolves to a tool named `ai-<name>` that takes a single `prompt`
// parameter. From one to many connections, all resolved at start-up.
//
// The model running the agent treats each `ai-<name>` as a peer it can
// lean on — "check my work", "co-solve this", "second opinion before a
// risky step". The tool ships its own usage guidance (a static base
// the operator's per-connection description sits above) so even small
// models understand WHEN and HOW to reach for it without the
// application having to mirror-paste a paragraph into its own system
// prompt.
//
// Opt-in
// ------
// Every connection is DISABLED by default — nothing calls out until the
// operator turns it on with `enabled = true` in its section. This keeps
// outbound peer calls strictly under operator control (e.g. so a box
// that IS `ai.local` never points a peer back at itself). There is no
// automatic self-reference guard: enablement is explicit.
//
// Two connections are baked in as named presets (url + description
// pre-filled) so enabling one is a one-liner:
//   * ai-local → http://ai.local      (the local AI box, general purpose)
//   * ai-pro   → http://ai-pro.local  (larger model for harder problems)
// An operator section with the same <name> overrides the matching
// preset's fields (and enables it); new names are simply added.
//
//   [REMOTE_MODEL_pro]
//   enabled = true            ; that's all you need to switch on ai-pro
#pragma once

#include "easyai/config.hpp"
#include "easyai/tool.hpp"

#include <string>
#include <vector>

namespace easyai::tools {

// One resolved remote-model connection. Built from a
// [REMOTE_MODEL_<name>] INI section (or one of the two baked-in
// defaults). `name` becomes the tool name `ai-<name>`.
//
// Sampling / transport knobs are "unset" at their sentinel values
// (-1.0f / -1) so the remote server picks its own defaults; only the
// knobs the operator pinned in the INI are sent on the wire.
struct RemoteModelSpec {
    std::string name;                  // <name>; tool is "ai-<name>"
    std::string url;                   // http:// or https:// endpoint
    std::string api_key;               // Bearer token (optional)
    std::string model = "easyai";      // request-body model id
    std::string description;           // operator text, shown ABOVE the
                                       // tool's static usage base

    float       temperature     = -1.0f;
    float       top_p           = -1.0f;
    int         top_k           = -1;
    float       min_p           = -1.0f;
    int         max_tokens      = -1;
    int         timeout_seconds = -1;  // <0 → tool's own default
    bool        tls_insecure    = false;
    std::string ca_cert_path;

    // Off by default — the operator opts in per connection with
    // `enabled = true` (or `enable = true`) in the section. Callers
    // register only enabled specs.
    bool        enabled = false;
};

// Resolve every [REMOTE_MODEL_<name>] section in `ini` into a spec
// list, seeded with the two presets (ai-local, ai-pro). An operator
// section with the same <name> overrides the preset's fields; a new
// <name> is appended. Insertion order is stable (presets first, then
// operator-only sections in INI order).
//
// Every spec starts DISABLED; a section enables it with
// `enabled = true`. Callers register only `enabled` specs. No
// self-reference guard — enablement is explicit.
//
// Pure: no I/O, no network call.
std::vector<RemoteModelSpec> resolve_remote_models(const config::Ini & ini);

// Build the `ai-<name>` tool for one spec.
//
// The handler opens a FRESH, stateless Client per call, sends `prompt`
// as a single user turn (no tools exposed to the remote model — it is a
// consultant, not a sub-agent), and returns the reply. Each call is
// independent: the peer does not remember previous calls.
//
// The tool carries:
//   * short_description — the per-turn trigger sent in every request's
//     tools[] (the channel that always reaches the model);
//   * description — the full manual: the operator's per-connection text
//     ABOVE a static base that normalises how to use a peer model;
//   * system_addendum — a concise reinforcement composed into the
//     system prompt by Session / RemoteBackend.
//
// `spec.enabled` is advisory here — this always yields a working tool.
// Callers register only the enabled specs (see resolve_remote_models).
Tool remote_model(const RemoteModelSpec & spec);

}  // namespace easyai::tools
