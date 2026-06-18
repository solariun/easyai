// easyai/engine.hpp — high-level llama.cpp wrapper for building agent engines.
//
// Usage:
//
//   easyai::Engine engine;
//   engine.model("models/qwen2.5-0.5b.gguf")
//         .gpu_layers(99)               // -1 == all (default), 0 == CPU only
//         .context(4096)
//         .system("You are a concise assistant.")
//         .temperature(0.8f)
//         .top_p(0.95f)
//         .add_tool(easyai::tools::datetime())
//         .add_tool(easyai::tools::web())
//         .load();
//
//   engine.on_token([](const std::string & piece){ std::cout << piece; });
//   std::string reply = engine.chat("What time is it in Tokyo?");
//
#pragma once

#include "tool.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward-declared at global namespace so chat_params_for_current_state can
// return one without forcing every easyai::Engine consumer to include the
// hefty common/chat.h.  Callers that actually use the result include it
// themselves.
struct common_chat_params;

namespace easyai {

using TokenCallback    = std::function<void(const std::string & piece)>;
using ToolCallback     = std::function<void(const ToolCall &, const ToolResult &)>;
// Fires at the start of every multi-hop iteration AFTER the first.
// Streaming layers register this to drop their per-turn accumulators
// (e.g. the running raw-text buffer + previous parsed message used to
// compute SSE diffs) so that retries (thought-only loops, tool-result
// follow-ups) don't bleed state across turns.
using HopResetCallback = std::function<void()>;

// Fires every time chat_continue discards a turn that announced an
// action without emitting a tool_call ("Let me search…", "I'll do
// that now") and is about to nudge + retry. Streaming layers
// register this to surface the retry to the user — e.g. push a
// reasoning_content delta into the SSE stream so the webui's
// Thinking panel shows "↻ Retry 3/10: model announced without
// calling a tool — nudging" while it happens, instead of leaving
// the user staring at a frozen UI for 10 silent retries.
//
// Args: (attempt, max, reason) — attempt is 1-based.
using IncompleteRetryCallback =
    std::function<void(int attempt, int max, const std::string & reason)>;

// Fires once per generate() AFTER the prompt-eval llama_decode loop
// completes and BEFORE the first token is sampled. Mirrors what
// llama-server emits via stderr / `prompt eval time` — the moment
// where the user/operator can see "the model finished ingesting the
// prompt; generation starts now". The callback receives the number
// of tokens that were actually decoded in this pass (n_tokens), how
// many were already in the KV cache from a prior turn (n_cached),
// and the wall time the decode loop took (ms).
//
// Streaming layers (server SSE → webui chip + libeasyai-cli on_token
// pipeline) register this to print a one-line status above the
// model's first content delta.
//
// Skipped when n_tokens == 0 (every prompt token already cached —
// nothing to report).
struct PromptEvalReport {
    int    n_tokens   = 0;     // tokens decoded in this prompt-eval pass
    int    n_cached   = 0;     // tokens already in KV cache (no decode)
    double prompt_ms  = 0.0;   // wall time spent in the decode loop
};
using PromptEvalCallback = std::function<void(const PromptEvalReport &)>;

// Fires BETWEEN batches of the prompt-eval llama_decode loop —
// one tick per `n_batch` tokens decoded plus a final tick at 100 %.
// Mirrors llama-server's `prompt_progress` SSE field
// ({total, cache, processed, time_ms}) so streaming consumers can
// surface a real "thinking N %" gauge during the prompt-ingestion
// window, instead of the polite-fiction spinner that just animates
// in place while the user waits.
//
// Args mirror the llama-server payload:
//   processed — tokens decoded so far in THIS prompt-eval pass
//   total     — total tokens that need decoding in this pass
//                (== processed at completion)
//   cached    — tokens already in the KV cache from a prior turn
//                (the prefix that did NOT need decoding)
//   ms        — wall time elapsed since the decode loop started
//
// processed / total → overall progress 0..1.
// (processed - 0) / (total - 0) → same here because cached are NOT
// part of total in our accounting; the cached count is reported as
// context only, exactly like llama-server when stream resumes from a
// shared prefix.
//
// Skipped entirely when total == 0 (everything cached). Fires at
// least twice for any non-trivial prompt: once with processed >= n_batch
// and once at processed == total. For a tiny prompt that fits in one
// batch the only tick is the final one.
struct PromptProgressReport {
    int    processed = 0;
    int    total     = 0;
    int    cached    = 0;
    double ms        = 0.0;
};
using PromptProgressCallback = std::function<void(const PromptProgressReport &)>;

class Engine {
   public:
    Engine();
    ~Engine();

    Engine(const Engine &)             = delete;
    Engine & operator=(const Engine &) = delete;
    Engine(Engine &&) noexcept;
    Engine & operator=(Engine &&) noexcept;

    // ---------------- configuration (chainable, take effect on load()) -----
    Engine & model         (std::string gguf_path);
    Engine & context       (int n_ctx);            // default 4096
    Engine & batch         (int n_batch);          // default = n_ctx
    Engine & gpu_layers    (int n);                // -1 = all, 0 = CPU only
    Engine & threads       (int n);                // default = hw threads
    Engine & seed          (uint32_t s);           // default = random
    Engine & system        (std::string prompt);
    Engine & temperature   (float t);              // default 0.7
    Engine & top_p         (float p);              // default 0.95
    Engine & top_k         (int   k);              // default 40
    Engine & min_p         (float p);              // default 0.05
    Engine & repeat_penalty(float r);              // default 1.15 (anti-loop)
    Engine & presence_penalty(float p);            // default 0.0 (disabled, OpenAI [-2.0, 2.0])
    Engine & frequency_penalty(float p);           // default 0.0 (disabled, range [0.0, 2.0])
    Engine & max_tokens    (int   n);              // per chat() call, -1 = until ctx
    Engine & tool_choice_auto    ();
    Engine & tool_choice_required();
    Engine & tool_choice_none    ();
    Engine & parallel_tool_calls (bool enable);    // default false
    Engine & verbose       (bool on);              // default false

    // Agentic-loop safety cap: how many tool round-trips chat() will run
    // before bailing out.  Default 8 — fine for small research flows.
    // Bash flows often need many more (compile → run → fix → re-run);
    // when --allow-bash is in play, callers bump this to ~99999.
    Engine & max_tool_hops (int n);

    // Auto-retry-with-nudge for "incomplete" turns — model finished without
    // a tool_call AND emitted only a tiny visible reply (typical
    // "Let me search…" / "I'll do that now" announce-without-action
    // pattern).  When enabled (default ON) chat_continue discards the bad
    // assistant turn, appends a corrective synthetic user message
    // ("don't announce, execute"), fires on_hop_reset so streaming
    // consumers can drop their accumulated parse state, and retries up to
    // `max_incomplete_retries()` times before giving up. Mirrors
    // libeasyai-cli's Client::retry_on_incomplete behaviour at the engine
    // layer so every consumer (server, agent, recipes) gets the same
    // recovery without each app rolling its own.
    Engine & retry_on_incomplete (bool on = true);

    // How many times chat_continue will discard + nudge + retry an
    // "announce-only" turn before bailing out and returning whatever
    // the last attempt produced. Default 10. Set to 0 to disable
    // retries (equivalent to retry_on_incomplete(false)). Higher
    // values cost tokens but give weak / over-cautious models more
    // chances to actually call a tool — useful for 1-bit quants
    // (Bonsai, BitNet) where the first few replies are noisy.
    Engine & max_incomplete_retries (int n);

    // Hard ceiling on context fill — once the rendered prompt + KV
    // cache reach this percentage of n_ctx, chat_continue stops
    // dispatching further tool calls, sets last_error to a context-
    // full message, and returns whatever the model produced this hop
    // so the caller can show the user the result + a clear note.
    // Default 100 (only stops at the wall); pass 0 to disable.
    Engine & stop_at_ctx_pct(int pct);

    // Mirror of Client::last_was_ctx_full — true when the most recent
    // chat() / chat_continue() ran out of context window and the
    // agentic loop bailed early.  Lets the app layer pick a friendlier
    // banner ("contexto cheio, recomece a conversa") rather than the
    // raw last_error string.
    bool last_was_ctx_full() const;

    // ---------------- KV cache & model overrides ----------------------------
    // KV cache data type — accepts ggml_type names: "f32", "f16", "bf16",
    // "q8_0", "q4_0", "q4_1", "q5_0", "q5_1", "iq4_nl". Lower precision
    // dramatically cuts VRAM / RAM at a small quality cost.  Defaults to f16.
    // Invalid names are recorded in last_error() and otherwise ignored.
    Engine & cache_type_k (const std::string & ggml_type_name);
    Engine & cache_type_v (const std::string & ggml_type_name);
    // Keep KV cache on CPU even when layers are on GPU — useful when VRAM is
    // tiny.  Trades GPU bandwidth for capacity.
    Engine & no_kv_offload(bool on = true);
    // Use a single unified KV buffer across the input sequences when computing
    // attention (recent llama.cpp feature; mostly for speculative + parallel).
    Engine & kv_unified   (bool on = true);
    // Override a key-value entry in the loaded GGUF.  Format:
    //   "key=int:42"        "key=float:0.75"
    //   "key=bool:true"     "key=str:hello"
    // Repeatable.  Useful for fixing tokenizer or rope parameters at load time.
    Engine & add_kv_override(const std::string & spec);

    // ---------------- speculative decoding ----------------------------------
    // Set the speculative-decoding backend. Pass one of:
    //   "none"           — autoregressive (default; speculative off)
    //   "draft-mtp"      — Multi-Token Prediction heads embedded in the main
    //                      model. Requires a model TRAINED with MTP (e.g.
    //                      DeepSeek V3, MimoVL). No separate draft model.
    //   "draft-eagle3"   — Eagle3 draft model (needs a draft model file).
    //   "draft-simple"   — Standalone draft model speculative decoding
    //                      (classic; needs a draft model file).
    //   "ngram-simple"   — Self-speculative via n-grams.
    //   "ngram-map-k"    — n-gram keys only.
    //   "ngram-map-k4v"  — n-gram keys + 4 m-gram values.
    //   "ngram-mod"      — n-gram mod.
    //   "ngram-cache"    — 3-level n-gram cache.
    // Unrecognised strings record an error in last_error() and leave
    // speculation off. Defaults to "none".
    Engine & spec_type(const std::string & name);
    // Max draft tokens per speculation step. 0 = disabled, ≥1 enables (with a
    // sensible per-type ceiling enforced by llama.cpp). Typical for MTP: 6.
    Engine & spec_draft_n_max(int n);
    // Path to a standalone GGUF draft model for draft-simple / draft-eagle3.
    // The draft model must share the same vocabulary as the target.
    // Ignored when spec_type is none, draft-mtp, or ngram-*.
    Engine & spec_draft_model(const std::string & path);

    // ---------------- compute / memory knobs --------------------------------
    // Flash attention — auto, on, off.  Default 'auto' lets llama.cpp decide
    // based on backend capability.  Pass true to force on.
    Engine & flash_attn   (bool on = true);
    // Pin model weights in physical memory so they aren't paged out.  Costs
    // RAM but improves latency consistency on heavily-loaded hosts.
    Engine & use_mlock    (bool on = true);
    // Set to false to disable mmap and read the GGUF straight into RAM.
    // Slightly slower start-up; sometimes needed on network filesystems.
    Engine & use_mmap     (bool on = true);
    // Separate thread pool size for batch (prompt-eval) compute.
    Engine & threads_batch(int n);
    // NUMA strategy: "distribute", "isolate", "numactl", "" (default off).
    Engine & numa         (const std::string & strategy);
    // GPU split mode: "none" (single GPU), "layer" (default), "row", "tensor".
    Engine & split_mode   (const std::string & mode);
    // RoPE scaling type: "none", "linear", "yarn". Default unspecified.
    Engine & rope_scaling (const std::string & type);
    // RoPE frequency scale factor. 0.0 = default (no scaling).
    Engine & rope_freq_scale(float scale);
    // YaRN original context length. 0 = use model default.
    Engine & yarn_orig_ctx(int ctx);

    // ---------------- reasoning / thinking ----------------------------------
    // Toggle the chat-template `enable_thinking` flag (used by Qwen3, R1,
    // etc.). Default ON: the model sees thinking as enabled and may emit
    // <think> blocks. Pass false to ask the model not to think aloud.
    Engine & enable_thinking(bool on = true);

    // Override the chat template embedded in the GGUF with a Jinja file on
    // disk. The file is read at load() time and its contents are passed as
    // the `chat_template_override` argument to common_chat_templates_init.
    // Mirrors llama-server's --chat-template-file. Empty path = use the
    // model's embedded template (the default). Read errors are recorded in
    // last_error() and load() fails.
    Engine & chat_template_file(const std::string & path);

    // Pick the reasoning-content extraction format used when rendering the
    // chat template. Accepts the same names as llama-server's
    // --reasoning-format: "none", "auto", "deepseek", "deepseek-legacy".
    // Default "auto" (which currently behaves like "deepseek"). Unknown
    // names fall back to "none" — see common_reasoning_format_from_name in
    // common/chat.h.
    Engine & reasoning_format(const std::string & name);

    // How hard the model should think before answering — the "reasoning
    // effort" knob exposed by GPT-OSS and friends. Rendered into the chat
    // template via the `reasoning_effort` template kwarg (the same channel
    // llama-server uses), so a template that reads it (e.g. emits
    // "Reasoning: high" into its system block) adjusts its thinking depth.
    //
    // Accepts (case-insensitive): "low", "medium", "high", "minimal", or
    // any model-specific level. The sentinels "auto", "none", "default",
    // "model", and "" all mean USE THE MODEL DEFAULT — nothing is injected
    // and the template decides on its own. Default is "auto".
    //
    // No-op on templates that don't consult `reasoning_effort` (most
    // non-reasoning models simply ignore the extra kwarg).
    Engine & reasoning_effort(const std::string & level);

    // ---------------- tools -------------------------------------------------
    Engine & add_tool   (Tool t);
    Engine & clear_tools();

    // ---------------- callbacks --------------------------------------------
    Engine & on_token            (TokenCallback             cb);
    Engine & on_tool             (ToolCallback              cb);
    Engine & on_hop_reset        (HopResetCallback          cb);
    Engine & on_incomplete_retry (IncompleteRetryCallback   cb);
    Engine & on_prompt_eval      (PromptEvalCallback        cb);
    Engine & on_prompt_progress  (PromptProgressCallback    cb);

    // ---------------- lifecycle --------------------------------------------
    bool load();              // loads gguf + builds context. returns true on success.
    bool is_loaded() const;
    void reset();             // wipes conversation history + KV cache.
    void clear_kv();          // wipes ONLY the KV cache (history kept intact).
                              // useful between retry attempts so the next
                              // generate_one() re-renders the same history
                              // with a fresh starting state.

    // ---------------- runtime sampler reconfig ------------------------------
    // The sampler is built at load() time. Use this to re-create it with new
    // values mid-conversation (server preset switching, /temp commands).
    // Pass any negative value to leave that field untouched.
    Engine & set_sampling(float temperature  = -1.0f,
                          float top_p        = -1.0f,
                          int   top_k        = -1,
                          float min_p        = -1.0f);

    // ---------------- conversation primitives -------------------------------
    // Push a message of any role onto the history WITHOUT generating.
    // Useful for replaying full conversations (HTTP server) or seeding the
    // chat with assistant priming. tool_name / tool_call_id are only
    // meaningful when role == "tool".
    Engine & push_message(std::string role,
                          std::string content,
                          std::string tool_name    = "",
                          std::string tool_call_id = "");

    // Replace the entire history at once. The first system message coming
    // from .system() is kept as a baseline only when `messages` does not
    // already contain one.
    void replace_history(const std::vector<std::pair<std::string, std::string>> & messages);

    // Full-fidelity history replay.  Mirrors what llama-server does with
    // common_chat_msgs_parse_oaicompat → common_chat_templates_inputs::messages:
    // assistant turns keep their `tool_calls`, tool turns keep their
    // `tool_call_id` + `name`, so the chat template (Qwen3, DeepSeek,
    // Hermes, …) can render the proper <tool_call>…</tool_call> markup
    // and the model sees a structurally-correct conversation.  The
    // (role, content)-only overload above LOSES that structure and was
    // the root cause of malformed multi-hop turns from external clients
    // that send full agentic histories (cli-remote, OpenAI SDK,
    // Claude-Code).
    struct ToolCallSpec {
        std::string name;             // function name
        std::string arguments_json;   // raw JSON object literal (per OpenAI spec)
        std::string id;               // call_X — empty allowed, gets generated
    };
    struct HistoryMessage {
        std::string role;                 // "system" | "user" | "assistant" | "tool"
        std::string content;              // visible body (may be empty if tool_calls non-empty)
        std::string reasoning_content;    // assistant's <think> block, if persisted
        std::string tool_name;            // role == "tool": which tool produced `content`
        std::string tool_call_id;         // role == "tool": matches an assistant tool_calls[i].id
        std::vector<ToolCallSpec> tool_calls;   // role == "assistant"
    };
    void replace_history(const std::vector<HistoryMessage> & messages);

    // Direct access for advanced HTTP scenarios.
    void clear_history();

    // Pop the last N entries off history (no-op if N >= history.size).
    // Used by callers that need to rollback a synthetic nudge they
    // pushed before a retry, so the conversation handed back to the
    // client doesn't leak internal-only messages.
    void pop_last(size_t n = 1);

    // ---------------- inference --------------------------------------------
    // chat() runs a full turn including any tool-call/tool-result loops.
    // The returned string is the final assistant message content.
    std::string chat(const std::string & user_message);

    // Same as chat() but assumes the user message is ALREADY the last
    // entry in history (e.g. you called push_message("user", ...) before
    // and want to render+inspect the chat template's params without
    // duplicating the user message).  Used by the HTTP streaming path
    // that needs the rendered common_chat_params at on_token wiring time.
    std::string chat_continue();

    // Generate exactly ONE assistant turn from the current history. Returns
    // the parsed message (so the caller can inspect tool_calls and decide
    // whether to dispatch them or forward them to a remote client).
    //
    // The message is appended to the history so subsequent generate_one()
    // calls see it. If the model emitted tool calls and you want to feed in
    // tool results, use push_message("tool", result, name, id) and call
    // generate_one() again.
    struct GeneratedTurn {
        std::string                       content;
        std::string                       reasoning;
        std::vector<std::pair<std::string /*name*/, std::string /*args_json*/>> tool_calls;
        std::vector<std::string>          tool_call_ids;
        std::string                       finish_reason;  // "stop" | "tool_calls" | "length" | "error"
    };
    GeneratedTurn generate_one();

    // Lower-level: just generate raw text from current state.
    std::string generate();

    // ---------------- cooperative cancel ------------------------------------
    // Thread-safe shutdown signal for an in-flight chat()/chat_continue()/
    // generate(). request_cancel() flips an atomic flag that the decode
    // loop checks between every sampled token, and that chat_continue()
    // checks between agentic hops — so the next token boundary is the
    // worst-case latency before the engine returns. clear_cancel() resets
    // the flag and SHOULD be called before each new turn (the server's
    // SSE handler does this). cancel_requested() is the read accessor.
    //
    // The server uses this to react to a dropped client connection: when
    // the SSE sink's write fails it calls request_cancel() and the engine
    // unwinds cleanly instead of running to completion against a dead
    // socket. The library is OpenAI-protocol-compatible — there is no new
    // /cancel endpoint; the signal is intra-process only.
    Engine & request_cancel();
    Engine & clear_cancel();
    bool     cancel_requested() const;

    // ---------------- introspection -----------------------------------------
    std::string                 last_error()        const;
    int                         turns()             const;
    const std::vector<Tool>   & tools()             const;
    // Resolve the system prompt the model will receive: whatever was
    // set via `system(...)` PLUS every registered tool's
    // `effective_system_addendum()` (its `system_addendum` if the
    // tool set one, else its `description` as a fallback — see
    // `Tool::effective_system_addendum` in easyai/tool.hpp), composed
    // via the canonical `preamble::compose_system_prompt` helper so
    // the policy (8 KB per-tool cap, sanitization, blank-line
    // separator) stays in lockstep with Session / LocalBackend /
    // RemoteBackend. Pure: no I/O, no mutation, cheap to call. Use
    // this from binaries that want to "request the final system" and
    // dump it (e.g., `--show-system-prompt`).
    //
    // Caveat: if a backend (LocalBackend) has already composed and
    // written back via `system(composed)`, calling this getter again
    // will RE-append addenda. Backends own the lifecycle; callers
    // outside that pipeline should read this BEFORE backend init.
    std::string                 composed_system()   const;
    std::string                 backend_summary()   const;  // e.g. "Metal (GPU)"
    int                         n_ctx()             const;  // configured context window
    std::string                 model_path()        const;

    // Performance counters (cumulative since the last perf_reset).  Used by
    // the HTTP server to emit per-request timings in the SSE stream that the
    // webui renders (tokens/s, prompt/gen times, KV cache pressure).
    struct PerfData {
        int    n_prompt_tokens    = 0;   // tokens in the prompt that needed eval
        int    n_predicted_tokens = 0;   // tokens generated
        double prompt_ms          = 0.0; // total prompt-eval wall time
        double predicted_ms       = 0.0; // total generation wall time
        int    n_ctx_used         = 0;   // tokens currently in the KV cache
    };
    PerfData perf_data()  const;
    void     perf_reset();

    // Render the chat-template state for the current history+tools and
    // return the resulting common_chat_params.  Exposed so the HTTP layer
    // can build a parser (with the right PEG arena + reasoning_format)
    // and call common_chat_parse incrementally during streaming —
    // matching how llama-server splits reasoning_content from content.
    // Pass true to include the assistant generation prompt suffix.
    //
    // Returns a global-namespace common_chat_params (fully qualified to
    // dodge ADL into our own easyai:: namespace).
    ::common_chat_params chat_params_for_current_state(bool add_generation_prompt = true) const;

   private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace easyai
