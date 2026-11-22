# easyai Specification

## Library boundary (AUTHORITATIVE — 2026-05-27)

easyai ships as ONE library — `libeasyai`. There is no split between
"engine" and "cli" libraries; the same shared object carries Engine,
Client, Session, every tool, and the system-prompt composer. CLI /
server / MCP binaries are demos that prove the lib's surface; they
link a single target (`easyai` / `easyai::easyai`). Legacy aliases
`easyai::engine` and `easyai::cli` resolve to the unified target so
existing split-layout link lines still work.

| Concern | Lives where |
|---------|-------------|
| AI connection — local llama.cpp | `easyai::Engine` (lib) |
| AI connection — remote OpenAI-protocol | `easyai::Client` (lib) |
| Backend abstraction (uniform `chat`/`reset`) | `easyai::Backend`, `LocalBackend`, `RemoteBackend` (lib) |
| Built-in tools (datetime/web/fs/bash/python/memory/tool_lookup) | `easyai::tools::*` (lib) |
| System-prompt composition | `easyai::preamble::*` + `easyai::Session` (lib) |
| One-call agent setup | `easyai::Session` (lib) |
| External tool loader (`EASYAI-*.tools` manifests) | `easyai::load_external_tools_from_dir` (lib) |
| MCP server / client | `easyai::mcp::*`, `easyai::McpClient` (lib) |
| HTTP server, SSE, web UI | `services/server.cpp` ONLY |
| REPL, hybrid shell, signal handling | `services/cli.cpp` ONLY |
| `--show-system-prompt`, presets, banners | shared via lib helpers; binary owns the flag |

Rule: anything that touches the model, registers a tool, or composes a
system prompt MUST live in the lib so a third-party agent reads as
short as ours. The binaries own the surface their job requires
(HTTP routes, terminal UX, signal handling), nothing more.

## Session API (AUTHORITATIVE — 2026-05-27)

`easyai::Session` is the OpenAI-Python-SDK-shaped one-call entry
point. See `LIB_GUIDE.md` for the prose; the contract:

| Surface | Contract |
|---------|----------|
| `Session::local(path)` · `Session::local(Config)` · `Session::remote(url, model)` | Pick the backend once; same fluent API after. |
| `.system(text)` | Replace the BASE prompt verbatim — lib default suppressed. |
| `.no_builtin_system()` | Drop the lib's default BASE; combine with `.system_append(...)` to author from scratch. |
| `.system_append(text)` / `.system_append(callable)` | Concatenated after the BASE in call order. Dynamic form recomputed on every `refresh_system()`. |
| `.preamble_options(opt)` | Override per-turn `preamble::Options` (date/time, cutoff, memory_root, cite_sources, visual_ui). |
| `.with_default_tools(bool)` | Toggle the canonical toolset (datetime + web + tool_lookup baseline, plus gated fs/bash/python/memory/external). On by default. |
| `.add_tool(Tool)` | Append a custom tool; its `Tool::system_addendum` is collected. |
| `.init(err)` | Builds backend + tools + system. Once per session. |
| `.refresh_system()` | Re-render and push the system prompt after a mid-session `.system_append`. |
| `.chat(user)` | Runs the agentic loop, returns visible reply. Tokens stream via `.on_token` callback. |
| `.render_system()` | Returns the resolved system prompt for inspection. |

System-prompt composition order (5 layers, see `LIB_GUIDE.md` §4):
**base → tool addenda → operator appends → dynamic preamble → tools catalogue**.
The tools-catalogue tail is local-only; remote sessions delegate to the
server's own `build_session_info` (server-side, per-request).

## Tool::system_addendum (AUTHORITATIVE — 2026-05-27)

New optional `std::string` field on `easyai::Tool`. When a tool is
registered onto a `Session` (or any Backend that honours
`Config::extra_tools`), the addendum is concatenated into the system
prompt with blank-line separators. Authoring rule: a tool that needs
prompt-level guardrails (policy, citation rules, "always confirm X
first") ships them itself instead of asking the application to
mirror-paste a paragraph into its own system prompt. Removes the
"did we update both places?" drift.

Builder access: `Tool::builder("name").system_addendum("…")`.

## Remote-model peer tools — `ai-<name>` (AUTHORITATIVE — 2026-06-01)

A built-in tool family that lets the running agent consult ANOTHER
OpenAI-protocol model as a peer ("check my work", "co-solve this",
"second opinion before a risky step"). Lives in the lib
(`include/easyai/remote_model_tool.hpp`, `src/remote_model_tool.cpp`,
namespace `easyai::tools`) so any consumer reads as short as ours.

| Surface | Contract |
|---------|----------|
| `[REMOTE_MODEL_<name>]` INI section | One connection → one tool named `ai-<name>` with a single required `prompt` param. From 1 to many; resolved at start-up. OFF until `enabled = true`. |
| `tools::resolve_remote_models(ini)` | Pure. Seeds two presets, overlays INI sections. Returns `std::vector<RemoteModelSpec>` (every spec carries an `enabled` flag). No self-reference logic. |
| `tools::remote_model(spec)` | Builds one `ai-<name>` Tool. Handler opens a FRESH, stateless `Client` per call, sends `prompt` as a single user turn, returns the reply. NO tools exposed to the peer (consultant, not sub-agent). Each call is independent. |

INI keys (section `[REMOTE_MODEL_<name>]`): `enabled`/`enable` (**default
false** — must be `true` to switch the connection on), `url`/`endpoint`
(http or https; bare host gets `http://`), `key`/`api_key`, `model`
(default `easyai`), `description`, sampling (`temperature` `top_p`
`top_k` `min_p` `max_tokens`), `timeout` (default 300s), `tls_insecure`,
`ca_cert_path`.

## Per-model `[MODEL_*]` overrides + `alias` served-name override (2026-06-03)

`find_model_section(ini, model_name)` picks the `[MODEL_*]` profile whose
pattern is the **longest prefix** of the resolved gguf basename
(case-insensitive). `[MODEL_<pattern>]` matches when the basename STARTS
WITH `<pattern>`, so one section covers every quant in a family
(`[MODEL_Qwen3.6]` → `Qwen3.6-25B-A38M-Q4_K_M`, …) and a longer, more
specific pattern wins over a shorter one (`[MODEL_Qwen3-Coder-Next]` beats
`[MODEL_Qwen3]`). Matching is purely on the section name; `alias` is not
consulted here.

`alias` inside a `[MODEL_*]` section is the **public model-id the matched
profile advertises** — it OVERRIDES the `[SERVER] alias` for `/v1/models`,
the webui badge, and chat responses, so the served name tracks whichever
model the box loads. Resolution lives in `server.cpp` right after
`apply_model_overrides`: when a profile matched and the operator did NOT
pass `--alias` on the CLI, the matched section's `alias` (single name;
first token of a comma list, trimmed) is written into `args.alias`, which
feeds `ctx->model_id`. Served-id precedence: **CLI `--alias` > matched
`[MODEL_*] alias` > `[SERVER] alias` > gguf basename**.

The installer (`install_easyai_server.sh`) emits `alias = $service_alias`
in every generated `[MODEL_*]` profile so the brand name survives a model
swap; operators change a profile's `alias` to give that specific model its
own session name.

Profile-key precedence stays `CLI > MODEL_<match> > [ENGINE] > hardcoded`.
The overlay (`apply_model_overrides`) iterates the same `kFlags()` ENGINE
keys, so any [ENGINE] key — including `chat_template_file`,
`reasoning_format`, every sampling/penalty/speculative key — is valid
inside a `[MODEL_*]` section.

**Opt-in, no auto-disable** (2026-06-02): every connection — including
the two presets — starts `enabled = false`. Nothing dials out until the
operator sets `enabled = true`. There is NO self-reference guard; the
old `host[:port]`-match auto-disable and the `self_url`/`self_model`
params were removed. Explicit enablement is the whole control surface —
a box that IS `ai.local` simply leaves the `ai-local` preset off.

**Two presets** (url + description pre-filled, both OFF by default):
`ai-local` → `http://ai.local`, `ai-pro` → `http://ai-pro.local`. A
section with the same `<name>` overrides the preset's fields and (with
`enabled = true`) switches it on; new names are added the same way.

**Description composition** (per the [[Tool::system_addendum]] rule —
the tool ships its own guidance):
* `short_description` — the per-turn trigger sent in every request's
  `tools[]` (the channel that always reaches the model, via
  `Tool::wire_description`). Static reinforcement + the peer name.
* `description` — full manual: the operator's per-connection text
  ABOVE a static base that normalises WHEN / HOW / WHAT-YOU-GET-BACK.
  Returned by `tool_lookup` / `/v1/tools`.
* `system_addendum` — concise reinforcement (operator one-liner folded
  in) composed into the system prompt by Session / RemoteBackend. Kept
  short so many connections don't bloat the prompt.

**Binary wiring** (resolution is lib-side; each binary registers only
`enabled` specs, BEFORE `tool_lookup` so the snapshot covers them, and
skips an enabled-but-url-less spec with a warning):
* `services/cli.cpp` `register_tools()` — re-reads the INI, honours an
  explicit `--tools` allowlist by `ai-<name>`.
* `services/server.cpp` — resolves from the server's own loaded
  `ini_config`. Tools execute SERVER-SIDE (the server is the agent),
  so the webui and any `/v1/chat/completions` consumer get them and
  they appear on `/v1/tools`. Gated by `--no-local-tools` /
  `[SERVER] local_tools`. Peer URLs must be reachable from the server.

**Logging (2026-06-02)**: the `remote_model` handler (lib, so both
binaries) logs each call via `easyai::log` — a summary line on stderr
(`ai-<name>: calling <url> (model=…, prompt=N chars)` →
`ai-<name>: ok after N ms (reply=M chars)` or `… FAILED after N ms — …`
via `log::error`) plus prompt/answer previews written only to the raw
transaction log (`log::file()`), so bodies don't leak into journald.
Registration logs list each enabled peer with its endpoint + model
(server: stderr; cli: `--verbose`).

## Multi-part request logging (server, 2026-06-02)

`parse_chat_request` coalesces array-form message `content` into text.
Non-text parts (`image_url`, `input_audio`, …) are silently dropped by
this text-only path — now COUNTED and surfaced: a text-only multi-part
message gets a quiet raw-log line; a message with dropped parts logs to
stderr (`multi-part content in N message(s): … M non-text part(s)
DROPPED (<deduped types>) — the model will not see them`) so the data
loss is visible to the operator.

## Prompt-progress logging (server, 2026-06-02)

Under `--verbose`, the server logs the per-batch prompt-eval progress
line the CLI already prints — same format, now server-side:
`easyai-server: [prompt_progress] <processed>/<total> (<cached> cached)
<ms> ms → thinking <N>% · ctx <N>% (<used>/<n_ctx> tok)`
(`thinking = processed/total`; `ctx = (cached+processed)/n_ctx`, the
LIVE projection of where the KV cache lands). Wired in
`handle_chat`'s `engine.on_prompt_progress` and decoupled from the
client's SSE preference: the callback is installed when the client
wants SSE progress OR the operator runs `--verbose`, so the log fires
regardless of `stream_options.easyai_prompt_progress`. Same 80 ms / 5 %
throttle as the SSE path; only emitted on streaming generations (the
prompt-eval batches are a streaming-path concept).

**Immediate 0 % tick on reception (2026-06-05).** When the per-batch
prompt processing is wired with SSE emit active (client wants
`easyai.prompt_progress`), the server emits one synthetic
`{pct:0, processed:0, total:0}` event right after wiring the callback —
before prompt-eval starts. The gauge therefore starts at 0 % the instant
the request lands instead of jumping to the first n_batch boundary
(~512 tokens in); short prompts no longer leap straight to the final
100 % tick with no "just started" frame. Only `last_emit_pct` is seeded
(to 0); `last_emit_ms` stays −1 so the first real batch tick — which
carries real timing — is never throttled away. Gated on `emit_progress`
(not `log_progress`), so a `--verbose`-only request logs no synthetic
SSE event.

## Webui status split: chip vs processing-info bar (server, 2026-06-02)

Two surfaces, both driven by server.cpp's injected webui JS (no
bundle.js edit):

**Per-message chip** (`buildChip` / `__easyaiSetStatus` — the
inline-flex span beside the copy/edit/fork/delete actions; dot `.d` +
label `.l`) now shows ONLY the waving dot + a status word. From the
instant the request is sent it reads `processing`, then
`processing <N>%` (from `easyai.prompt_progress` `pct` during
ingestion — visible on longer prompts; near-instant for short ones),
then `answering` / `thinking` once tokens flow, then `fetching·<tool>`
/ `complete` / `error`. All numeric metrics were removed from it. The
bundle's own `.processing-container` "Processing…/Initializing…"
shimmer is hidden via CSS — its role (signal the model is working) is
now the chip's job. For the WHOLE time a response is being produced —
every active state (`processing` / `thinking` / `answering` /
`fetching`), gated on the same `active` flag that drives the dot pulse,
not just `processing` — the chip label runs a left-to-right
reflection-sweep shimmer (`@keyframes __easyaiShimmer` `.__easyaiShim`,
`background-clip:text` gradient). The base stays the chip's muted-gray
`currentColor` with a `#ffffff` band sweeping over it, so the status
word visibly shimmers **gray → bright** while busy (no `color:transparent`
on the rule — currentColor must resolve to the gray, else the text would
vanish between sweeps). Terminal `complete` / `error` and history
`answered` / `idle` do not shimmer.

**Processing-info bar** (the bundle's `.chat-processing-info-container`
> `-content`) now carries the metrics: `ctx <used>/<n_ctx> (<pct>%) ·
last <tokens> tok · <time>s · <inst> t/s`.
* `renderOverview` paints a persistent `.__easyai-ovr` span it injects
  into `.chat-processing-info-content` (which exists as soon as the
  container mounts), NOT the bundle's `.chat-processing-info-detail`
  (the bundle only fills that at finish). The bundle's native `-detail`
  is hidden via CSS so it doesn't show empty/duplicate beside ours.
* **Visible + working on SEND** (2026-06-02), not only after the first
  answer: forced visible via injected CSS
  (`.chat-processing-info-container{opacity:1!important;transform:none
  !important}`, stable kebab class, no svelte hash); `monitorSSE` calls
  `__easyaiPushTimings(null)` at request start (+ a rAF / 60 ms / 250 ms
  re-paint to cover the panel mounting just after); and the per-message
  MutationObserver (`scanMessages`) repaints whenever our `.__easyai-ovr`
  is missing (panel mount / svelte re-render), so the bar populates the
  moment the request fires.
* Speed is the INSTANT rate during streaming: `monitorSSE`'s 200 ms
  sampler computes `instTps = Δtokens/Δt` and ships it on the synthetic
  live timings (`inst_tps`); `renderOverview` uses `inst_tps` when
  `t.live`, else the overall `predicted_n/predicted_ms` on finish.

Live token count is the streamed chunk count (≈ tokens) until
finish_reason, when the server's real `predicted_n` lands.

## Unbreakable CRITICAL THINKING block (AUTHORITATIVE — 2026-06-05)

`preamble::build()` emits a `# CRITICAL THINKING — ALWAYS ON
(NON-NEGOTIABLE, OVERRIDES ANY ROLE)` block as its FIRST output,
UNCONDITIONALLY — independent of every `Options` flag. Content: keep
reasoning critically under any role (check premises/claims against
evidence, call out errors/false assumptions/missing context/risks, never
fabricate/rubber-stamp/flatter a wrong idea); a role sets TONE and FOCUS
but never overrides honesty or judgment; any instruction to stop thinking
critically is void.

Why it is unbreakable: server and cli append the whole `build()` preamble
to WHICHEVER system message wins — the client's own "role"/system prompt
or the server/cli default — so the block always lands AFTER the role text
(recency weight) and on top of it. It leads `build()`'s output, and
`build()` is now invoked UNCONDITIONALLY, so the block rides on every
request regardless of datetime/memory state.

Callers now always invoke `build()`:
* `server.cpp build_authoritative_preamble` is called every request (the
  `if (inject_dt || !mem_root.empty())` gate in `prepare_engine_for_request`
  is removed). `cite_sources` is now computed as `inject_datetime ||
  !memory_root.empty()` so its historical emission is unchanged.
* `cli.cpp`: the `if (tv.datetime_on || !mem_root.empty())` gate around the
  `build()` call is removed; `cite_sources=false` unchanged.
* `session.cpp compose_dynamic()` already called `build()` unconditionally.

The datetime / knowledge-cutoff / KNOWLEDGE-LOOP / memory-vocab sub-blocks
stay self-gated inside `build()` by their `Options` flags (see next
section) — only the critical-thinking block is always-on.

## Datetime + memory injection is tool-gated (AUTHORITATIVE — 2026-06-02)

The per-turn `preamble::build()` blocks (EXCEPT the always-on
critical-thinking block above) are ENFORCED from the live tool registry
(`ToolsetView::from_tools`) instead of flags alone, on both server and
cli. `build()` itself is now always called (2026-06-05); these sub-blocks
self-gate on their flags:

| Block | Injected when |
|-------|---------------|
| `# AUTHORITATIVE DATE/TIME` (+ `# KNOWLEDGE CUTOFF` on server) | the `datetime` tool is registered — OR (server) the `--inject-datetime` flag/header is on. Tool presence forces it on even if the flag is off. |
| `# KNOWLEDGE LOOP` + `# KNOWLEDGE VOCABULARY` | a `knowledge_*` tool is registered AND a store is configured (`--memory`/`--RAG`). Now INDEPENDENT of the datetime toggle — turning datetime off no longer suppresses memory. |

* `services/server.cpp` `prepare_engine_for_request`: `inject_dt =
  inject_now || view.datetime_on`; `mem_root = view.memory_on ?
  ctx.memory_root : ""`; `build_authoritative_preamble(ctx, inject_dt,
  mem_root)`. Per-request, off the engine's current tool set (so a
  client that replaces tools is judged on what it actually sent).
* `services/cli.cpp`: injects from `ToolsetView::from_tools(cli.tools())`
  into the baked system prompt — `datetime` tool ⇒ date/time block (no
  CUTOFF; the remote model's cutoff is unknown), `knowledge_*` + `--memory`
  ⇒ memory blocks. `cite_sources=false` here (the cli prefix emits it).
  Note: against an easyai-server this double-injects the date/time block
  (cli + server); both read their own wall clock.

## Reasoning-effort fallback ladder (AUTHORITATIVE — 2026-10-04)

**Plain words.** The operator or the client asks for a thinking depth
(`reasoning_effort`, e.g. `max`). Some chat templates only know a few
names and `raise_exception` on the rest (seen in production: *"Unexpected
reasoning effort max. Supported types are xhigh (default), medium, and
low."*). Before this rule the exception escaped `render()`, the server fell
back to the generic parser, the engine produced nothing and the turn ended
as `finish_reason="error"`. Now the engine quietly picks the closest level
the template accepts and keeps going.

**Rules.**

1. `Engine::Impl::render()` passes `reasoning_effort` to
   `common_chat_templates_apply` only when non-empty (unchanged).
2. If the template throws and the message contains the word `effort`
   (case-insensitive), the level was rejected. Any other exception
   propagates untouched — a malformed history must still surface.
3. Candidates are tried in order on the ladder
   `minimal < low < medium < high < xhigh < max`: nearest neighbours of
   the requested level first, lower before higher on ties, then `""`
   (inject nothing, template default). A level outside the ladder tries
   the ladder from `max` down. Example for `max`: `xhigh`, `high`,
   `medium`, `low`, `minimal`, none.
4. The first accepted candidate is cached per requested level in
   `effort_remap` for the lifetime of the loaded model (cleared on
   `load()` / `unload()`), so later requests pay no retry.
5. One `easyai::log::error` line per remapped level, naming the request,
   the template's message and the chosen level.
6. If every candidate is rejected the original exception is rethrown.

```mermaid
flowchart TD
    A[render with requested level] -->|ok| Z[prompt]
    A -->|throws, no 'effort' in message| X[rethrow]
    A -->|throws 'effort'| B{next candidate on ladder}
    B -->|accepted| C[cache level, log once] --> Z
    B -->|rejected| B
    B -->|none left| X
```

**Code.** `easyai::apply_reasoning_effort(templates, inputs, requested,
remap)` in `src/engine.cpp`, declared in `engine.hpp` so it can be driven
without a model. `Engine::Impl::render()` calls it with the per-model
`effort_remap` cache.

**Proof.** `tests/test_reasoning_effort.cpp` (target
`easyai-test-reasoning-effort`, `-DEASYAI_BUILD_TESTS=ON`, `ctest`) renders
real Jinja templates through llama.cpp with no model loaded: `max`→`xhigh`,
`high`→`medium`, `minimal`→`low`, `turbo`→`xhigh`, accepted level leaves the
cache empty, all-rejected rethrows the template's message, an unrelated
`raise_exception` propagates untouched. 15 checks, all green on
2026-10-04. Before the change the same template ended every turn with
`finish_reason="error"` (production log, ai-pro, 2026-10-04 12:10).

**Server default = `max` (Gustavo, 2026-10-04).** `--reasoning-effort`
defaults to `max` instead of `auto`: with the ladder in place, `max` means
"the deepest level this template accepts" (`xhigh` on the Nemotron/GLM
family, `max` on DeepSeek V4, `high` on gpt-oss-style three-level
templates that validate). `auto` is still available to inject nothing.
**Known limit:** a template that does *not* validate the level and prints
it verbatim (gpt-oss, llm-jp: `Reasoning: max`) gets a word its model was
not trained on; the ladder cannot see that because nothing throws. Operators
of such models set `--reasoning-effort high`. Accepted values gain `xhigh`.

**Decision.** Fallback lives in the Engine, not the server, so the CLI,
library users and the server all get it. Parsing the template's
"Supported types are …" text was rejected: the wording is template-specific;
the ladder retry is template-agnostic and costs a few extra Jinja renders
only on the first rejected request per model.

## `/models` catalog: popular ∪ recent snapshot + live search (AUTHORITATIVE — 2026-10-04)

**Plain words.** The model browser looked "utterly incomplete": unsloth,
google, Qwen and friends never showed up. The catalog was a snapshot of the
1000 *most-recently-modified* GGUF repos on HuggingFace, and "search" was a
substring filter over that snapshot. Measured on 2026-10-04: that window
covers ~2 days of uploads, 161 of the 1000 belong to one re-uploader, and
unsloth, google and Qwen have 0 entries each. Searching could never find
what the window did not contain. Now the snapshot has a stable popular half,
and a search goes to HuggingFace's own index.

**Rules.**

1. **Snapshot** (`hf_fetch_catalog`): `catalog_size` repos by `downloads`
   plus `catalog_size` by `lastModified`, both `filter=gguf`, cursor-paged,
   deduplicated with the popular list first. Either half may fail: the
   other is kept and the error is reported in the envelope. Persisted to
   `easyai_hf_catalog.json` as before (same format, more rows).
2. **Live search** (`hf_live_search`, `ModelsEngine::live_search`): a
   non-empty `search` sends `GET /api/models?filter=gguf&search=<q>&sort=downloads&direction=-1&limit=L`
   with `L = clamp(limit, 50, 200)`. HF matches `search` against the full
   repo id, owner included, and accepts several words.
3. **Merge**: live hits first (HF download order), then snapshot rows whose
   `name provider params` contains the query, deduplicated by repo. The
   existing fit scoring, `min_fit`, `use_case` and `sort` apply to the
   merged list unchanged.
4. **Cache**: per lowercased query, TTL 600 s, at most 64 queries (oldest
   evicted). The cache mutex is never held across the network call. Repeat
   query measured at 4 ms.
5. **Envelope**: `live_search:true` when the live half answered; on HF
   failure the answer is snapshot-only and `error` carries the reason.
6. `catalog_size` keeps its name and range; it now sizes each half.

```mermaid
flowchart LR
    R[refresh] --> P[HF sort=downloads ×N] --> U[dedupe, popular first] --> S[(snapshot)]
    R --> Q[HF sort=lastModified ×N] --> U
    G[GET /models/api/models?search=q] --> C{cache hit <10 min?}
    C -->|no| H[HF search=q sort=downloads] --> K[cache] --> M
    C -->|yes| M[live hits + snapshot substring matches] --> F[score, filter, sort] --> J[JSON]
    G -->|empty search| S --> F
```

**Proof** (2026-10-04, real HF, `ModelsEngine` driven directly):

| query | before | after |
|---|---|---|
| snapshot size | 1000 (recent only) | 1975 (1000 popular ∪ 1000 recent) |
| `search=unsloth` | 0 | 115 |
| `search=google` | 0 | 60 |
| `search=gemma` | ~0 | 148 |
| repeat query | n/a | 4 ms (cache) |

**Decision.** Live search over a bigger snapshot was chosen instead of a
much bigger snapshot: HF holds far more GGUF repos than one process should
mirror, and HF's index already ranks by downloads. Parsing `author=` was
rejected: `search=` already matches the owner. An async live search was
rejected: one 300–800 ms round-trip per new query is acceptable and the
webui debounces input; the cache removes the cost on repeats.

## Webui effort chip (bundle webui, 2026-10-04)

**Plain words.** Gustavo asked for a popup to change the reasoning effort
right after the model-name pill in the prompt box. The bundle (llama-server
derived SvelteKit app) is not ours to edit, so the chip is injected the same
way the tone and tools pills are: a shallow clone of the model pill so it
inherits the bundle's styling, placed by the existing `reposition()` walk.

**Layout.** `[ tone ][ tools ][ model pill ][ effort ][ send ]`. The chip is
inserted after the pill's branch in the enclosing horizontal flex row; the
fast-path check in `reposition()` includes it so the MutationObserver does
not thrash.

**Rules.**

1. Label `effort <level>`; when the choice is `auto` the label shows the
   server default (`EFFORT_DEFAULT`, baked at render time from
   `ctx->default_reasoning_effort`, `auto` when empty).
2. Popover (`#__easyaiEffortPop`, fixed, attached to `document.body` like the
   tools popover) lists `auto minimal low medium high xhigh max`; the
   current one is highlighted; `auto` shows `server: <default>`, `max`
   shows `deepest the model accepts`. Click selects and closes; outside
   click, resize and scroll handling mirror the tools popover.
3. Persistence: `localStorage['easyai-effort']`, default `auto`.
4. Request: block4's fetch interceptor sets `body.reasoning_effort` when the
   choice is not `auto` and the bundle did not set it. Independent of the
   tone sampling overrides and of the server's authoritative preset.
5. Startup order: the server now resolves `--reasoning-effort` BEFORE it
   builds the webui HTML (it used to run after, so the chip would have baked
   `auto` while the server default was `max`).

```mermaid
sequenceDiagram
    participant U as user
    participant C as effort chip
    participant F as fetch interceptor
    participant S as easyai-server
    U->>C: click, pick xhigh
    C->>C: localStorage easyai-effort = xhigh
    U->>F: send message (bundle POST /v1/chat/completions)
    F->>F: body.reasoning_effort = xhigh
    F->>S: POST
    S->>S: Engine::reasoning_effort(xhigh) → ladder if rejected
```

**Proof (2026-10-04).** Local server on a 1 MB test GGUF: served page
contains `__easyaiEffortHost` and `EFFORT_DEFAULT='max'`; all 12 injected
scripts parse under JavaScriptCore. Browser click-through was not possible
from the session (no browser tool); see the manual check in the chat log.

## Server starts without a model (AUTHORITATIVE — 2026-10-04)

**Plain words.** Gustavo: "if loading the gguf file fails the server should
always start; the user can download, run or slink another model, and the
prompt is unavailable". Before, a bad `--model` path killed the process, so
the model manager that could have fixed it was unreachable too.

**Rules.**

1. `Engine::load()` failure at start-up is logged (`NO MODEL LOADED: <why>`,
   the path, and the `/models` URL) and the server keeps going.
   `ctx->model_id = "(no model)"`, `ctx->load_error = <why>`. No `--model`
   at all is the same case ("model path not set").
2. `require_model()` guards every generation route (`/v1/chat/completions`):
   `503 {"error":{"type":"model_not_loaded","message":"no model loaded — open
   /models to download, run or slink one (<why>)"}}`. Nothing touches the
   engine. Listing, tools, MCP, `/models/*` and the webui stay up.
3. `/health` gains `model_loaded` (bool) and `load_error` (when set);
   `/props` gains `model_loaded`.
4. `POST /models/api/run` (`Engine::reload`) is the way back: on success
   `load_error` is cleared and `model_id` recomputed; `link:true` also
   re-points the `ai.gguf` symlink so the next restart loads it.
5. Webui gate: both UIs poll `/health` every 3 s. Bundle: the prompt is
   locked through the existing `setInputLocked` with the reason text and a
   red banner with a link to `/models` is inserted above the form; the
   ctx-full logic's unlock ticks are ignored while `window.__easyaiNoModel`
   is set, only the poller's unlock (`reason === 'model'`) releases it.
   Minimal: textarea + send disabled, placeholder carries the message.
6. The engine getters the server calls while empty (`n_ctx`, `model_path`,
   `backend_summary`, `perf_data`, `set_sampling`) are already null-safe;
   verified, no change needed.

```mermaid
stateDiagram-v2
    [*] --> Empty: load() fails / no --model
    [*] --> Loaded: load() ok
    Empty --> Empty: chat → 503 model_not_loaded
    Empty --> Loaded: POST /models/api/run ok
    Loaded --> Loaded: chat → 200
    Loaded --> Loaded: POST /models/api/run (swap)
    Empty --> Empty: webui prompt locked, banner → /models
```

**Proof (2026-10-04, local).** Start with `--model …/ai.gguf` that does not
exist: banner printed, `/health` → `model_loaded:false` with `load_error`,
chat → 503, `/models` → 200, page carries the gate. `POST /models/api/run
{"name":"stories260K.gguf","link":true}` → `ok`, `ai.gguf` symlink created,
`/health` → `model_loaded:true`, chat → 200. Start with no `--model` at all:
same empty state, `/models` → 200. All injected scripts parse (JavaScriptCore);
minimal webui carries `pollHealth`. Full build green, ctest 1/1.

**Decision.** The gate lives in the server (`require_model`), not in the
engine, because the engine already reports `is_loaded()` and library users
decide their own policy. Polling `/health` (3 s) was chosen over a push
channel: the state changes once per operator action and the probe is
lock-free.

## Visual replies, never-assume, parallel tool calls (AUTHORITATIVE — 2026-10-04)

**Plain words.** Gustavo wants the web chat to be visual (draw diagrams,
show pictures from the web), the model to stop answering from memory when
it can look things up, and to let it fire several lookups at once.

**Rules.**

0. **Identity first and last.** `kWebUIPrelude` is the FIRST text of the
   server's static prompt: the UI renders ```svg as a drawing, ```mermaid
   as a diagram, markdown images as pictures; "you can draw; never
   describe yourself as text-only". `preamble::Options::visual_ui`
   (server sets true) emits a `# VISUAL REMINDER` block at the tail of
   every request's preamble, just before the volatile memory vocabulary
   so the cached prefix is untouched. Reason: the field test showed the
   model's self-image ("since I am a text-based AI") overrides any rule
   placed mid-prompt; the first and last positions are the ones obeyed.
1. **Web appendix** (`kWebUIAppendix`, server static prompt, web only):
   `## Be visual` — draw an SVG whenever a picture conveys more; technical
   aspects get a diagram by default; fenced ```` ```svg ```` block, one
   complete `<svg>` with viewBox, no script / external refs / foreignObject,
   readable on dark and light, under ~120 lines; image tag
   `![caption](url)` for image URLs that appeared in a tool result this
   turn, never invented; text always accompanies a figure; no pictures for
   greetings / one-liners. `## Never assume` — knowledge → web → answer;
   up to 10 tool calls per turn.
2. **Library guidance** (`src/preamble.cpp`, shared with the CLI):
   base prompt now says "greetings, chitchat and arithmetic" skip tools and
   "for anything factual, NEVER ASSUME"; the knowledge-cutoff block no
   longer exempts "stable facts" from verification; the information
   pipeline opens with "even one you think you know"; WEB-only sessions
   read "WEB first — never answer a factual question from memory while web
   tools are available"; new `BATCH INDEPENDENT CALLS` paragraph (up to 10
   at once, dependent steps one hop at a time); STOP SIGNAL counts
   *rounds*, not calls.
3. **Engine**: `parallel_tool_calls(true)` runs every call of a turn;
   `kMaxParallelToolCalls = 10` — extra calls are dropped with an
   `easyai::log::error` line naming the counts. Default stays `false`
   for library users; the server sets `true` and `/props` reports
   `supports_parallel_tool_calls:true`.
4. **Rendering** (`kEasyaiVisualJs`, injected into the bundle and baked
   into the minimal UI). **Mermaid**: a `<pre>`/`<p>` whose text starts
   with a Mermaid diagram keyword (or carries `language-mermaid`) is
   rendered through mermaid@11, lazy-loaded from jsdelivr on first use,
   `securityLevel:'strict'`, `htmlLabels:false`, theme `default` (light); because a Mermaid block has no closing marker, it is
   rendered only after its text has been unchanged for 1200 ms, the SVG
   is cached by text (UIs that rebuild on every token mount it
   synchronously on the next pass), parse failures are cached as bad.
   `window.__easyaiRenderFigures(root)` runs both renderers: the bundle
   on its 250 ms tick, the minimal UI right after it sets the message
   HTML (on the attached element, since Mermaid resolves asynchronously).
   **SVG**: the text is first passed through `decodeEntities` (every
   named entity except `amp lt gt quot apos` is decoded — models write
   `&rarr;`, `&nbsp;`, `&mdash;`, which XML does not define), parsed as
   `image/svg+xml`, and on a parser error re-parsed with the lenient
   `text/html` parser (the `<svg>` lands in the SVG namespace either
   way). Then: any `<pre>` or `<p>` whose whole text is one
   complete `<svg>…</svg>` — whatever fence language the model used
   (svg, xml, html, none), and raw SVG the markdown renderer escaped into
   a paragraph — becomes `<figure class="easyai-svg-fig">` with the
   sanitised SVG and the source in a collapsed `<details>`. Sanitiser:
   parse as `image/svg+xml`, reject parser errors, drop `script`,
   `foreignObject`, `iframe`, `object`, `embed`, `link`, `meta`, `style`,
   `image`, `animate`, `set`; keep `use` only for `#fragment` hrefs; strip
   `on*`, non-fragment `href`/`xlink:href`, and `style` attributes with
   `url(`/`expression`/`@import`; force `width=100%` and a viewBox. Bundle:
   run on the existing 250 ms tick, processed blocks skipped; minimal UI:
   run inside `renderMD` after highlight.js. Markdown images already
   render as `<img>` in both UIs; CSS caps them at the column width.

```mermaid
flowchart LR
    M[model reply] -->|```svg block| P[pre > code]
    P -->|complete svg| S[sanitise: parse, drop bad tags, strip handlers]
    S -->|ok| F[figure + collapsed source]
    S -->|parser error| P
    M -->|![caption](url)| I[img, max-width 100%]
```

**Field test and revision (Gustavo, 2026-10-04 evening).** First deploy:
"info about X.25" came back text-only, "diagrams" too, and "an SVG"
produced a block labelled `xml` (the model wrote "since I am an AI text
model…"); the block was complete — the paste was taken mid-stream. Revision: the `## Be visual`
block now states that the UI renders SVG as a drawing and the model is not
text-only here; WHEN is mandatory with trigger words and "text-only or
xml/html-labelled code is a FAILURE"; WHAT lists a shape per case; HOW
pins the exact fence word, the `<svg>` opening, a palette readable on
both themes, a ~40-element / ~80-line budget so the figure completes, and
"always close `</svg>` and the fence". The renderer was widened to any
`<pre>`/`<p>` holding a complete SVG regardless of fence language, and the
bundle's code-block wrapper (language label + copy button) is replaced
with the figure.

**Second field test (Gustavo, 2026-10-04 night).** "create a diagram,
graphical for me" → "since I am a text-based AI, the best way is
Mermaid.js syntax… most markdown viewers will render this" plus a
```mermaid block. Two conclusions: the identity belief is the blocker,
and Mermaid is the model's reflex. Revision: prelude + tail reminder
(rule 0) and Mermaid rendering (rule 4) so the reflex also yields a
picture; SVG stays the stated preference.

**Third field test (Gustavo, 2026-10-04 night).** Mermaid rendered, a
900×550 SVG did not. Cause: `&rarr;` inside a `<text>` — an HTML entity
that XML does not define — so the strict parser failed and the block was
marked bad. Fix: entity decoding + HTML-parser fallback (above), the
canvas-rect drop, and a HOW line telling the model to write arrows and
symbols as UTF-8 characters and use only the three XML entities.
Reproduced outside the browser with the same two steps: strict parse of
the original fails on `&rarr;`, parse after decoding succeeds, the
900×550 background rect is dropped and the content rect kept.

**White figure box (Gustavo, 2026-10-04).** Every SVG and Mermaid figure
is shown inside a WHITE box (`.easyai-svg-fig {background:#fff;
color:#1f2937}`) whatever the page theme; Mermaid uses its light
`default` theme; the model's own canvas rect is kept as drawn (the
earlier canvas-rect drop was removed — it only existed for dark pages);
the HOW palette now targets white: dark text, light fills or mid-tone
fills with white text. Reason: diagrams are designed for paper-white and
one consistent background beats theme-dependent readability.

**Fit and zoom (Gustavo, 2026-10-07).** Figures were clipped: the model's
`width`/`height`/`viewBox` rarely cover what it drew, and Mermaid pins a
`max-width` on its SVG. `mountFig` (shared by the SVG and Mermaid paths)
now, once the figure is in the DOM, calls `getBBox()` on the root `<svg>`
and rewrites the `viewBox` to the measured drawing plus 8 units of
padding (`preserveAspectRatio="xMidYMid meet"`, `overflow:visible`),
strips the model's `width`/`height`, and shows the SVG at 100% of the
column with `height:auto`. A toolbar above every figure offers
**−** / **+** (×1.25 per step, 0.5×–6×), **1:1** (fit to width) and
**⤢** (open the SVG full size in a new tab via a Blob URL); zooming
scales the SVG's rendered width inside a scrollable box capped at 80vh.
`getBBox()` needs an attached element, so the SVG path also mounts
through `mountFig` and the minimal UI keeps calling the renderer on the
attached message element. Degenerate boxes (0 width/height) leave the
viewBox untouched.

**Proof of the revision.** `--show-system-prompt` now opens with the
prelude line; `preamble::build()` driven directly with `visual_ui=true`
emits `# VISUAL REMINDER` 609 bytes from the end of a 4629-byte preamble
and omits it with `visual_ui=false`; modern and minimal pages both carry
the Mermaid-capable helper with 0 script syntax errors; full build green,
ctest 1/1.

**Proof (2026-10-04).** `--show-system-prompt` carries `## Be visual`,
`## Never assume`, `BATCH INDEPENDENT CALLS`, "WEB first", "NEVER ASSUME".
`/props.chat_template_caps.supports_parallel_tool_calls == true`. Modern
page: 13 injected scripts, 0 syntax errors, helper present; minimal page:
2 scripts, 0 errors, helper present, placeholder substituted. Full build
green, ctest 1/1. Not verified here: the figure rendering in a real
browser (no browser tool in this session).

**Decisions.** The visual rules live in the server's web appendix only,
because the CLI renders in a terminal. The never-assume rule changes the
shared library text because a contradicting sentence ("anything you
already know — no tool needed") would have left the model with two
authoritative instructions. Tool calls beyond 10 are dropped rather than
queued: a model that emits dozens is looping, and the log line makes it
visible. SVG is sanitised client-side instead of trusting the model: the
model's output is untrusted content in the page.

## Voice transcription (STT) service + endpoint directory (AUTHORITATIVE — 2026-10-15)

**Plain words.** The server can turn speech into text. A whisper model
sits next to the chat model; a mic chip in the web UI records, the
browser converts the recording to a plain WAV, the server transcribes it
and the text lands in the prompt box for the user to check before
sending. Any other program on the LAN can use the same route with the
OpenAI audio API shape. The `/models` Status tab lists every endpoint
the server speaks, with full URLs, so a client is configured by copying.

```mermaid
flowchart LR
  subgraph Browser
    MIC[mic chip\nafter the effort chip] --> REC[MediaRecorder\nwebm / mp4]
    REC --> WA[Web Audio decode\n16 kHz mono PCM16 WAV]
    WA --> POST
    POST --> BOX[text appended to\nthe prompt box]
  end
  POST -->|multipart file= or raw audio/wav| EP["POST /v1/audio/transcriptions"]
  subgraph easyai-server
    EP --> WAV[decode_wav\nPCM16 / float32 / extensible\nany rate, any channels]
    WAV --> RS[resample_linear\n-> 16 kHz mono]
    RS --> STT["easyai::Stt::transcribe\n(stt_mu, not engine_mu)"]
    STT --> J["{text, language, duration, segments}"]
  end
  CFG["[STT] model / language / threads\n--stt-model --stt-language --stt-threads"] -.-> STT
  HL["/health: stt_loaded, stt_model\ncompat.stt"] -.-> STT
```

**Rules.**

1. **Library.** `easyai::Stt` (`include/easyai/stt.hpp`, `src/stt.cpp`)
   wraps `whisper.cpp`: `load(path)`, `unload()`, `is_loaded()`,
   `model_path()`, `last_error()`, `threads(n)`,
   `transcribe(pcm, n, language, Result&)`. `Result` = `text`,
   `language` (detected or forced), `audio_ms`, `run_ms`, `segments`
   (`t0_ms`, `t1_ms`, `text`). Input is always 16 kHz mono float PCM
   (`Stt::kSampleRate`). `Stt::available()` says whether the build has
   whisper; without it `load()` fails with "built without whisper".
   Model-free helpers: `decode_wav(bytes, pcm, err)` (RIFF/WAVE, PCM16,
   float32, WAVE_FORMAT_EXTENSIBLE, any rate, channels averaged) and
   `resample_linear(in, in_rate, out_rate, out)`.
2. **Build.** `EASYAI_WITH_WHISPER` (default ON) adds the sibling
   `../whisper.cpp` with `EXCLUDE_FROM_ALL` after llama.cpp, so whisper
   links the ggml already built (Metal / CUDA / Vulkan included) instead
   of a second copy. Missing sibling → warning, STT compiled out. The
   install rule ships `libwhisper*` next to `libllama*`.
3. **Server config.** New INI section `[STT]`: `model` (`--stt-model`,
   path to `ggml-*.bin`, may be a symlink), `language` (`--stt-language`,
   default `auto`), `threads` (`--stt-threads`, 0 = whisper default).
   Load is non-fatal, like the chat model: the server starts, logs
   `NO STT MODEL`, and the route answers 503 `stt_not_loaded` until a
   model is loaded from `/models`.
4. **Route.** `POST /v1/audio/transcriptions`, OpenAI shape, Bearer-gated
   like every `/v1/*`. Body: multipart with `file` (required), `model`
   (echoed, otherwise ignored), `language` (`auto` or ISO-639-1),
   `response_format` = `json` (default, `{"text"}`), `text` (plain
   body), `verbose_json` (`text`, `language`, `duration`, `segments[]`
   with `id`, `start`, `end`, `text`), `prompt` (OpenAI's field: text
   whisper sees before the audio, passed as `initial_prompt`; names and
   spellings to prefer — easyai-assistant sends "The assistant is called
   easy AI."). A raw body with
   `Content-Type: audio/wav` (or `audio/x-wav`, `audio/wave`) is accepted
   too, for `curl --data-binary`, with the same names as query parameters. Accepted audio: WAV only (see
   decision). Errors: 400 `invalid_request_error` (no file / bad WAV),
   415 `unsupported_media_type` (not WAV), 503 `stt_not_loaded`.
   One transcription at a time per context (`stt_mu`); chat generation
   is never blocked by it (separate mutex from `engine_mu`).
   Body cap is `max_body` (default 8 MiB ≈ 4 min of 16 kHz mono PCM16).
5. **Discovery.** `/health` adds `stt_loaded`, `stt_model`, and
   `compat.stt = "/v1/audio/transcriptions"`. `/props.modalities.audio`
   stays `false` (true would make the bundle attach audio to chat turns,
   which the chat model cannot read).
6. **Web UI, bundle.** Mic chip cloned from the model pill, placed after
   the effort chip: `[tone][tools][pill][effort][mic][send]`. Click =
   record (red dot + seconds), click = stop → decode → WAV → POST →
   text appended to the textarea with an `input` event so the bundle's
   binding sees it. The chip is greyed with the title "no STT model —
   open /models" while `/health.stt_loaded` is false (the existing 3 s
   poll carries the flag). Bearer header comes from the bundle's own
   `apiKey` setting. The bundle's own mic button stays hidden (it would
   attach audio to the chat). Minimal UI: a 🎤 button in the input bar
   with the same helper. Both UIs share `kEasyaiVoiceJs`
   (`window.__easyaiVoice`): `available()`, `recording()`, `toggle(cb)`
   with `cb({state:'recording'|'transcribing'|'done'|'error', text,
   seconds, error})`.
7. **Secure context.** Browsers expose `getUserMedia` only on `https://`
   or `localhost`. Over plain `http://ai-pro.local:8080` the mic chip
   says "needs HTTPS or localhost" and stays disabled. The server gains
   optional TLS: `[SERVER] tls_cert` / `tls_key` (`--tls-cert`,
   `--tls-key`, PEM) switch the listener to `httplib::SSLServer`; the
   startup banner prints `https://`. A self-signed certificate is
   accepted once in the browser. Alternative without TLS: Chrome flag
   `chrome://flags/#unsafely-treat-insecure-origin-as-secure` with the
   server origin. Both are documented.
8. **STT model management (`/models`).** Session-cookie gated like the
   other `/models/api/*` routes. `GET /models/api/stt` → `{loaded,
   model, model_path, configured_path, language, dir, local:[{name,
   size_bytes, mtime, is_current}], catalog:[{name, file, size_bytes,
   multilingual, note}], download:{…}}`. `POST /models/api/stt/download
   {name}` fetches `ggml-<name>.bin` from `ggerganov/whisper.cpp` on
   HuggingFace through the existing download worker (one download at a
   time, same progress object). `POST /models/api/stt/run {name,
   link}` hot-swaps under `stt_mu`; `link:true` repoints the
   `--stt-model` symlink (same rules as `set_model_symlink`).
   `POST /models/api/stt/unload`, `POST /models/api/stt/delete {name}`
   (never the running one). The Local tab shows a "Voice transcription
   (STT)" panel: current model, local `ggml-*.bin` files with Run /
   Slink / Delete, and the catalog with Download. The catalog is the
   whisper.cpp model table (tiny…large-v3-turbo, `.en` and `q5`
   variants) with sizes; `large-v3-turbo-q5_0` is marked recommended
   for pt-BR + en on a small box. The shared download manager's filename guard
   (`safe_in_dir`) accepts `.gguf` **and** `.bin`: whisper weights live in
   the same directory. Until 2026-10-28 it accepted `.gguf` only, so the
   Voice panel's Download button had answered 409 "not a .gguf file"
   since the panel was introduced (found on ai-pro; no download had ever
   gone through the panel, the local `ggml-*.bin` files were copied by
   hand).
9. **Endpoint directory.** `/models/api/status` gains `endpoints`: a
   list of `{family, method, path, auth, description, enabled}` built
   from the routes actually registered (metrics only when `--metrics`,
   TLS scheme reflected). The Status tab renders an "Endpoints" card
   grouped by family — OpenAI-compatible, Ollama-compatible, Voice
   (STT), MCP, Health, Web UI, Models API — each row showing the full
   URL (`location.origin + path`), the method, and the auth posture
   (`open`, `Bearer api_key`, `MCP_USER bearer`, `session cookie`), plus
   a copy button per URL and one "Base URL for OpenAI clients" line
   (`<origin>/v1`). Every path keeps the name the upstream API uses, so
   third-party tools connect without a translation layer.

**Decisions.**

- **WAV only on the wire; the browser converts.** `decodeAudioData`
  handles webm/opus and mp4/aac in every browser, so the server never
  needs ffmpeg or libopus. Rejected: server-side decoding (a codec
  dependency for one feature), whisper.cpp's miniaudio (wav/mp3/flac,
  still no opus).
- **whisper, not an audio-capable chat model.** mtmd audio models tie
  voice to one chat model and force a swap; whisper works with any LLM
  and runs beside it.
- **Transcript into the box, never auto-sent.** The user sees and fixes
  the text first.
- **Separate mutex.** Transcription is short and must not wait behind
  a long generation; whisper contexts are single-run, so `stt_mu`
  serialises transcriptions only.
- **Own management routes instead of extending the GGUF manager.**
  whisper weights are `ggml-*.bin`, not GGUF; the local list, catalog
  and symlink logic differ. The download worker is shared through a
  `start_download_file()` entry that skips the GGUF rules.
- **TLS optional, in the same binary.** cpp-httplib already carries
  OpenSSL for outbound HTTPS; `SSLServer` costs 30 lines and unlocks the
  microphone on a LAN host. Rejected: a reverse proxy requirement.

**Proof.** `easyai-test-stt` (ctest `stt`): a synthetic 1 kHz tone WAV
at 44.1 kHz stereo decodes to 16 kHz mono of the right length and
amplitude; float32 and WAVE_FORMAT_EXTENSIBLE variants decode; a
truncated header, a non-RIFF body and an 8-bit file are rejected with
the right error; `resample_linear` keeps endpoints and length. When
`EASYAI_STT_TEST_MODEL` names a whisper model and
`../whisper.cpp/samples/jfk.wav` exists, the test transcribes it and
expects the word "country".
`easyai-test-local-models` (ctest `local_models`) also resolves and
deletes a `ggml-tiny.bin` through the engine (the Voice panel path) and
rejects a `.txt`.

Measured 2026-10-15 (macOS, Metal, `ggml-tiny`): the 11.0 s JFK sample
transcribes in 437 ms from the library and in 457 ms / 68 ms (first /
warm) through the live route; `decode_wav` + 44.1 kHz stereo → 16 kHz
mono keeps the amplitude within 0.05. Live server proof: `/health`
carries `stt_loaded`, `stt_model`, `tls`, `compat.stt`; multipart
`json`, raw `audio/wav` `verbose_json` (1 segment, `en`, 11.0 s) and
`text` all return the sentence; mp3 → 415, a non-WAV file → 400,
unload → 503, `run` + `link` reloads and re-points `stt.bin`, deleting
the running file → 409. A second instance with `--tls-cert/--tls-key`
+ `--api-key` answers `https://` (`tls:true`), 401 without the token,
503 with it (no STT model there). `/models/api/status.endpoints` has 21
rows in 7 families. Bundle page: 14 inline scripts, 0 syntax errors,
voice helper + mic chip present; minimal page: 3 scripts, 0 errors,
placeholder substituted; models page: 1 script, 0 errors. Full build
green, ctest 2/2 (`reasoning_effort`, `stt` with the real model). Not
verified here: microphone capture in a real browser (no browser tool in
this session).

**Model file.** The recommended weight `ggml-large-v3-turbo-q5_0.bin`
(574 MB, SHA-256 `394221709c…a7e2`, MIT, from `ggerganov/whisper.cpp`
at HF commit `5359861`) is documented in `models/stt/README.md` with its
provenance table and alternatives, and fetched by
`scripts/fetch_stt_model.sh` (checksum-verified). The binary is committed
through **Git LFS** (`.gitattributes`: `models/stt/*.bin filter=lfs`), by
Gustavo's decision on 2026-10-15 so a fresh checkout is ready to work;
GitHub rejects plain blobs over 100 MB. Cost accepted: each LFS checkout
of the file counts against the account's LFS bandwidth quota. The Linux
installer (`git lfs pull` after clone, then `install` from
`models/stt/` into `/var/lib/easyai/models/`) needs no HuggingFace
access for the default model; a pointer-only checkout (no git-lfs,
detected by size ≤ 4096 bytes) falls back to the download. Measured with
it: the 11 s JFK sample in 1224 ms on Metal, punctuation included.

**Limits.** No streaming / partial transcripts (push-to-talk only).
Non-WAV uploads from external clients are refused. Browser microphone
needs HTTPS or localhost (rule 7). `/api/chat` (Ollama generation) is
still not served: the Ollama family lists only `/api/tags` and
`/api/show`.

## Model finder mode — TUI and web (AUTHORITATIVE — 2026-10-15)

**Plain words.** Typing a search in the model list is a different job
from asking for recommendations. A search must find the model, whether
or not it fits this box; the fit stays visible as a column. Both the
web `/models` Recommend tab and the `easyai-cli --llm-manager` Recommend
tab now switch to that behaviour the moment a query is typed, and
multi-word queries work.

```mermaid
flowchart LR
  Q["search typed
'qwen3 unsloth'"] --> D[url_decode
%20 / + → space]
  D --> T[split terms
max 3]
  T -->|one HF call per term,
cached 10 min| HF[(HuggingFace
filter=gguf)]
  HF --> U[union, by downloads]
  U --> F["all-terms match on
repo + name + owner + params"]
  F --> G{filters}
  G -->|finder defaults| A["min_fit=any, sort=downloads
(fit shown, not gated)"]
  G -->|explicit min_fit / sort| N[narrowed]
```

**Rules.**

1. **Two modes in `GET /models/api/models`.** No `search` → *recommend*:
   defaults `min_fit=marginal`, `sort=score` (unchanged). `search`
   present → *finder*: defaults `min_fit=any`, `sort=downloads`; a
   client that passes `min_fit` / `sort` explicitly narrows as before.
   `min_fit=any` (new value, rank 0) and `too_tight` both mean "show
   all". The envelope reports `search_mode`, `min_fit`, `sort`.
2. **Multi-term matching.** `search_terms_match(hay, query)`: every
   whitespace-separated term must be a substring, any order; the
   haystack is `repo + name + owner + params`, lowercased. HuggingFace's
   `search=` is one substring over the repo id, so the live half asks
   once per term (at most 3 terms, each cached by the existing 10-minute
   search cache), takes the union sorted by downloads, then applies the
   all-terms match.
3. **Query strings are percent-decoded.** `parse_query` now decodes
   `%XX` and `+`. Before this every search containing a space reached
   the matcher as `qwen3%20unsloth` and returned nothing — the root
   cause of "model finding" looking broken in both UIs.
4. **Web UI.** Typing a query flips Min fit → `Any (show all)` and Sort
   → `Downloads` once (previous values restored when the query is
   cleared); the user can still narrow. A hint line under the toolbar
   says "Finder: N matches for “q” on HuggingFace (live), most
   downloaded first — fit is informational; set Min fit / Sort to
   narrow", or "snapshot only" with the HF error.
5. **TUI (`--llm-manager`, Recommend tab).** Same auto-switch on `/`
   search (fit → `any`, sort → `downloads`, restored on clear); new `s`
   key cycles sort (`score, downloads, likes, params, tps, mem`); the
   header shows `finder` / `recommend`, the active sort, and the
   live/snapshot note; list limit raised to 100. Fit option `all`
   renamed `any` to match the API.

**Decisions.** Finder defaults live on the server (absent parameter)
and are mirrored in both UIs, so curl users get the finder behaviour
too while the UIs stay explicit about what they send. Per-term HF
queries instead of one query: HF has no multi-term search; three cached
calls are cheaper than paging the whole index. The fit filter is not
removed in finder mode, only defaulted off: narrowing a search by fit
is still a legitimate ask.

**Proof (2026-10-15).** `tests/test_model_search.cpp` (ctest
`model_search`, 9 CHECKs: any order, extra spaces, owner/name prefix,
missing term, empty query). Live server: `search=qwen3 unsloth` →
`search_mode=finder, min_fit=any, sort=downloads, live=true, total=34`
(top rows `unsloth/Qwen3.8-27B-GGUF too_tight`, `…Qwen3.5-4B-GGUF
perfect`); same query with `min_fit=good` → 18; `google gemma` → 45;
no search → `recommend, marginal, score`. Before the decode fix both
multi-word queries returned 0 with `live=true`. models.html: 1 script,
0 syntax errors, finder code present. Full build green, ctest 3/3.

## Backend::Config extensions (2026-05-27)

`LocalBackend::Config` and `RemoteBackend::Config` gained two fields,
populated by `Session` and also usable directly by callers who skip
the Session layer:

| Field | What |
|-------|------|
| `std::vector<Tool> extra_tools` | Caller-supplied tools, registered AFTER built-ins / memory / external, BEFORE `tool_lookup`. |
| `std::string system_appendix` | Static text appended to the system prompt AFTER tool addenda, BEFORE the AVAILABLE-TOOLS catalogue (local). |

## Sanitization at prompt-splice boundaries (AUTHORITATIVE — 2026-05-27, SECURITY_AUDIT §25.1)

Every text source that gets spliced into the system prompt by the lib
runs through one of two sanitizers in `easyai::preamble`:

| Source | Sanitizer | Cap | Why |
|--------|-----------|-----|-----|
| Tool name + section body in `tools_block` / `build_session_info` | `sanitize_for_prompt` (anonymous, src/preamble.cpp) — strips ALL C0 incl. `\n` | 64 / 400 | Structural — each tool renders as one `### name` section; an embedded `\n` would forge a new section heading. (§23.1) |
| Multi-paragraph `Tool::system_addendum` | `preamble::sanitize_addendum` | 8 KiB / tool | Paragraph block — keeps `\n` / `\t`, strips ESC / DEL / bell / other C0. (§25.1) |
| Operator's `Config::system_appendix` / `Session::system_append(...)` | `preamble::sanitize_addendum` | 16 KiB | Same. (§25.1) |

Applied at the THREE splice sites: `LocalBackend::init`,
`RemoteBackend::Impl::rebuild`, `Session::Impl::full_compose` (which
also serves `Session::refresh_system` / `set_system` /
`render_system`). Today every source is operator-controlled, but the
sanitizer is the seatbelt for future plumbing (external manifests,
MCP server tool descriptors) and defends the operator's TTY against
ESC injection via `--show-system-prompt`.

## Session mid-session contracts (AUTHORITATIVE — 2026-05-27, SECURITY_AUDIT §25.2 / §25.3)

| Call | History preserved? | Underlying backend call |
|------|-------------------|------------------------|
| `Session::refresh_system()` | **YES** | `engine_ptr()->system(...)` / `client_ptr()->system(...)` (pure setter) |
| `Session::set_system(text)` | NO (cleared, matches REPL `/system <text>`) | `backend->set_system(...)` |
| `Session::add_tool(t)` post-init | YES | `engine_ptr()->add_tool(t)` / `client_ptr()->add_tool(t)` + `refresh_system()` |
| `Session::add_tool(t)` pre-init | n/a (no history yet) | queued into `Config::extra_tools` |
| `Session::reset()` | NO | `backend->reset()` |

Calling `engine_ptr()->add_tool(t)` (or `client_ptr()->add_tool(t)`)
directly is allowed but bypasses Session — the tool will be visible
in the model's `<tools>` block on the next turn, but its
`system_addendum` will NOT be added to the system prompt until the
next `Session::set_system` / `refresh_system` runs. Use
`Session::add_tool` for the safe path.

# easyai-cli Specification

## Modes

| Mode | Invocation | Description |
|------|-----------|-------------|
| One-shot | `easyai-cli --url URL -p PROMPT` | Execute prompt and exit |
| Interactive REPL | `easyai-cli --url URL` | AI prompt loop with `/` commands |
| Hybrid AI Shell | `easyai-cli --url URL --shell` | User's shell with `>` AI prefix |
| Management | `easyai-cli --url URL --list-models` | Server diagnostics |

## Signal Handling (Ctrl+C)

### Interactive Mode (REPL and Shell)

| State | Ctrl+C effect |
|-------|---------------|
| AI generating | Stop generation, return to prompt |
| At prompt | Clear line, show new prompt (like bash) |
| Shell command running (--shell) | Kill command, return to prompt |
| Triple rapid Ctrl+C | Force-exit (escape hatch) |

Exit via `/exit`, `/quit`, or Ctrl+D (EOF).

### Quiet Mode (--quiet / -q)

First Ctrl+C hard-cancels and exits. Second force-exits.

## --shell Mode

Hybrid shell: the user's `$SHELL` executes normal commands, lines prefixed
with `>` are forwarded to the AI model.

### Command dispatch

| Input pattern | Action |
|--------------|--------|
| `> prompt text` | Send to AI model |
| `/exit`, `/quit` | Exit shell |
| `/clear`, `/reset`, `/compress` | Session management |
| `/plan`, `/tools`, `/help` | Info commands |
| `cd [dir]` | Change CWD (persists) |
| `export KEY=VALUE` | Set env var (persists) |
| `unset VAR` | Remove env var (persists) |
| anything else | Execute via `$SHELL -c command` |

### Implied flags

- `--shell` implies `--allow-bash` (AI can run commands)
- INI: `[cli] shell = true` (CLI flag overrides)

### Prompt format

```
~/project $ command
~/project $ > ask the AI something
```

CWD shown with `~` abbreviation for `$HOME`.

### Shell builtins

These run in-process to persist state across commands:

- **cd**: Supports `~`, `-` (OLDPWD), relative and absolute paths
- **export KEY=VALUE**: Sets env var, strips surrounding quotes
- **unset VAR**: Removes env var

### Subprocess execution

Normal commands fork the user's shell (`$SHELL -c command`). The child
shares the parent's process group so Ctrl+C (SIGINT) reaches it directly
from the kernel. The parent's signal handler ignores SIGINT while a shell
command is running and resumes `waitpid` on EINTR.

## Session Persistence

- `.easyai_session` written after every AI turn (atomic temp+rename)
- `--continue` to resume, `--compress` to recap
- `--no-local-session` for read-only mode

## Built-in Tools

| Tool | Condition |
|------|-----------|
| datetime | always |
| plan | unless `--no-plan` |
| web | always (runtime check for libcurl) |
| fs (split/unified) | `--sandbox DIR` |
| bash | `--allow-bash` or `--shell` |
| evaluate (formerly python3; runtime is Python 3) | `--sandbox` and not `--no-python` |

## REPL Prompt

Green `●` icon (same style as tool-call success markers in streaming output).

## Markdown table rendering (2026-06-15)

The CLI renders GitHub-flavoured markdown tables in the model's streamed
reply as aligned Unicode box-drawing tables. Implemented in
`easyai::ui::MdTableStream` (`include/easyai/ui.hpp` + `src/ui.cpp`) and
wired onto the content stream inside `easyai::ui::Streaming` — so it
benefits every `Streaming` consumer (currently `easyai-cli`).

### Streaming contract

Token output must keep streaming, but a table cannot be laid out until
every row is known (column widths span all rows). Resolution: the filter
is **line-oriented and selective** — only a line whose first non-blank
character is `|` is held back. Everything else streams token-by-token,
unchanged.

State machine over completed lines (`State::NORMAL → HEADER_SEEN → IN_TABLE`):

| State | Next line | Action |
|---|---|---|
| NORMAL | row candidate (`\|…`) | stash as header, → HEADER_SEEN |
| NORMAL | anything else | emit immediately (prose streams) |
| HEADER_SEEN | separator row (`\|:?-+:?\|…`) | → IN_TABLE (header + separator buffered) |
| HEADER_SEEN | not a separator | false alarm: emit stashed header raw, reprocess line |
| IN_TABLE | row candidate | append to table |
| IN_TABLE | anything else | render the buffered table, reprocess line |

A trailing partial line (no `\n` yet) is held only when it could belong
to a table (mid-table, starts like a row, or is leading whitespace);
otherwise it streams at once, preserving the token-by-token feel for
prose.

### Flush points

The buffered table is drained (`Streaming::flush_pending()` →
`MdTableStream::flush()`) at every content-stream boundary:

- **end of turn** — `run_one()` calls `flush_pending()` after `chat()`
  returns, before `spinner.finish()` (covers a table that is the last
  thing in the reply).
- **content → reasoning** — `emit_reason_()` flushes first.
- **content → tool marker** — `on_tool_()` flushes first.

### Rendering

- Columns sized to the widest cell; width counts UTF-8 code points
  (continuation bytes `0b10xxxxxx` skipped), so accented Latin text
  aligns. CJK double-width glyphs are undercounted (accepted edge).
- Alignment from the separator row: `:--` left, `--:` right, `:-:`
  centre, `---` left (GFM default).
- Header cells bold; box-drawing borders (`┌┬┐├┼┤└┴┘─│`) dim.
- `\|` inside a cell is an escaped literal pipe.

### Colour-off passthrough

When `Style::color` is false (non-TTY stdout or `NO_COLOR`),
`MdTableStream` is a **pure passthrough**: raw markdown is emitted
unchanged so piped output / captured logs stay valid markdown. No
buffering occurs in this mode.

## Spinner Transition Report

When the spinner transitions FROM token-streaming mode (showing tk/s) TO
thinking/shimmer mode or end-of-turn, a summary line is emitted in dark blue:

```
● XX% / NNNN tokens  last: 00.0tk/s
```

- `XX%` — context window fill percentage
- `NNNN tokens` — absolute context token count (omitted if unavailable)
- `00.0tk/s` — last observed generation speed

Emitted on:
- Thinking transition (between agentic hops, when prompt eval restarts)
- End of turn (`finish()`)

Not emitted when `token_speed_ < 0.1` (no meaningful generation occurred).

## Prompt-eval progress + final summary

### Spinner suffix during prompt eval

The shimmer's suffix shows BOTH the live prompt-eval percentage AND the running context-fill percentage:

```
thinking <N>% · ctx <M>%
```

| Element | Source |
|---|---|
| `<N>%` | `processed / total` from the server's `easyai.prompt_progress` SSE event (one tick per `n_batch` tokens decoded). |
| `<M>%` | LIVE ctx-%: `(cached + processed) / n_ctx`. Reflects where the KV cache will be when this prompt-eval pass finishes — not stale data from the prior turn. |

When `n_ctx` is unknown (first turn before the server reports it), only the `<N>%` part renders.

### `--no-prompt-progress`

CLI flag + INI key `[cli] prompt_progress = on|off`. When off, the cli sends `stream_options.easyai_prompt_progress = false` in the request body. The server inspects this and skips wiring the per-batch `on_prompt_progress` callback for that request — no `easyai.prompt_progress` SSE events fire. The final `easyai.prompt_eval` summary still fires either way.

| State | `--verbose` | Per-batch SSE | Spinner % during eval | Per-batch log lines | Final summary on screen | Final summary in log |
|---|---|---|---|---|---|---|
| default | OFF | yes | `thinking N% · ctx M%` | none | `● prompt eval: N tok · ms · t/s · ctx M%` | yes |
| default | ON | yes | same | per batch via `easyai::log::write` | same | yes |
| `--no-prompt-progress` | OFF | NO | static "thinking" | none | same | yes |
| `--no-prompt-progress` | ON | NO | static "thinking" | none (no events to log) | same | yes |

### "Final metrics always logged"

`on_prompt_eval` (the final summary, fires once per agentic hop) always calls `easyai::log::write` with the structured line `[prompt_eval] N tok (M cached) · X ms · Y t/s · ctx Z% (used/total)`. `log::write` tees stderr + the `--log-file` file (if set), so the final metrics land in the log regardless of `--verbose`.

## read_file Tool Behavior (opencode-parity since 2026-06-12)

Output prefixes every line with `<n>: ` (line numbers on by default in both modes); individual lines longer than 2000 chars are cut with a `... (line truncated to 2000 chars)` marker. Reports total line count for files ≤ 8 MiB. Reading a DIRECTORY path returns its entries one per line (subdirectories get a trailing `/`, sorted, capped at 1000). A missing path suggests up to 3 similarly-named entries from the parent directory (`Did you mean: …?`). Every successful read registers the path in the read-before-write registry (next section).

| Mode | Trigger | Default limit | Line numbers | Total count |
|------|---------|---------------|-------------|-------------|
| Line mode | `start_line` set | 2000 lines (max 2000) | Always on | Yes |
| Byte mode | default / `offset` set | 65536 bytes (max 1 MiB) | On (default true) | Yes (≤ 8 MiB files) |

When line mode stops short of EOF the output ends with `(File has more lines. Pass start_line=N to read beyond this point — M more lines.)`.

## Read-before-write registry (2026-06-12)

`write_file` (overwrite of an EXISTING non-empty file) and `edit_file` (both modes) require the target to have been `read_file` in this session first; otherwise they return a clear error telling the model to read first. The registry lives on the shared `Sandbox` (per tool-registration, process lifetime — on a long-running easyai-server it is process-wide), keyed by normalized path, capped at 16384 entries; on overflow it clears, which only forces a cheap re-read before the next overwrite. A successful write/edit also registers the path (the writer knows the content it just wrote). `append_file` and brand-new files are exempt.

## edit_file string mode (2026-06-12)

`edit_file` accepts two parameter shapes, both atomic (temp file + rename):

| Mode | Params | Contract |
|---|---|---|
| String (preferred) | `oldString`, `newString`, optional `replaceAll` | `oldString` must match the file content EXACTLY and be unique (otherwise: `found N matches … pass replaceAll=true`); `newString=""` deletes; identical old/new is an error; CRLF files accept LF-normalized `oldString` and stay CRLF on disk; empty `oldString` + missing file creates it. |
| Line (legacy) | `start_line`, `end_line`, `content` | Replaces lines [start..end] (1-based, inclusive); `content=""` deletes; `end_line=start_line-1` inserts; `start_line=line_count+1` appends. |

## grep_file / glob_file output (opencode-parity, 2026-06-12)

`grep_file`: header `Found N matches` (or `No files found`), then per file `path:` followed by `  Line <n>: <text>` rows, blank line between files; truncation note when `max_matches` (default 100) hits. Accepts `include` as the preferred alias of `file_glob`. `glob_file`: plain newline-separated paths sorted newest-first by mtime, capped at `limit` (default 100, max 1000) with a `(Results are truncated: …)` note; empty → `No files found`.

## bash caps (2026-06-12)

Model-facing capture: 50 KB and 2000 lines (head-biased line cap with a `... N lines truncated ...` marker; byte overflow appends a re-run hint). Timeout: default 120 s, max 600 s (SIGTERM, then SIGKILL +2 s). Optional `description` param (5-10 words, active voice) — surfaced by the easyai-cli TUI as the running-command title.

## fs Batch Mode (`action="ops"`)

The unified `fs` tool accepts a batch via `ops` — an array of single-op shapes. Lets a model land many file edits in one round trip and amortises the per-call overhead.

| Cap | Value | Why |
|---|---|---|
| Ops per call | **50** | Bound the report length and the worst-case file-system churn per turn. |
| Distinct files per call | **20** | Counted only over ops that name a `path` (`cwd`/`sandbox`/`glob`/`grep`/`list` are free). Stops a runaway batch from blasting many files at once. |
| Same-path edits reorder | descending `start_line`, stable | Each edit's `start_line` refers to the file's ORIGINAL line numbers — model doesn't have to track line drift. String-mode edits (`oldString`/`newString`, no `start_line`) keep their authored order and run AFTER the line-mode edits on the same file (content matching is line-number-independent, so last is the safe slot). |
| Read clip in batch | 2 KiB per op | Successful `read` ops are clipped to 2 KiB in the batch report; re-issue a standalone `read` for the full body. |
| Error visibility | full body | Failed ops emit their complete diagnostic so the model can self-correct without re-running. |
| `continue_on_error` | default `false` | Stop-on-first matches small-model debugging flow. |
| Report header | `batch: N ops across F files (path1, path2, …)` | Single line at the top so the model can verify it touched the right set before reading per-op statuses. |

Exposed only on the **unified** `fs` surface. Default `ToolMode` is `Split` (one focused tool per action — better small-model dispatch); opt into the ops batch with `--tools-mode unified` or `--tools-mode both`.

## ToolMode default

`easyai::cli::Toolbelt::tool_mode_` defaults to `ToolMode::Split` — one focused tool per action (`read_file`, `write_file`, `edit_file`, …, `search_web`, `fetch_web`, `search_knowledge`, `recall_knowledge`, …). Small / weaker tool-callers dispatch more reliably against flat one-verb-per-tool schemas than against an `action`-discriminated union.

To pick up the unified `fs(action="ops")` batch (or the `web(action=…)` dispatcher), opt in with `.tool_mode(ToolMode::Unified)` or `--tools-mode unified`. `Both` registers both surfaces side-by-side.

## Knowledge tool names — memory, not files (2026-06-05)

The seven knowledge tools are named as MEMORY operations so the model
never treats the store as a filesystem. They keep the `knowledge_`
prefix (groups them in the flat tool list) with memory-themed verbs:

| Action | Tool name | Renamed from |
|---|---|---|
| remember a piece of knowledge | `learning_knowledge` | `knowledge_save` |
| add to existing knowledge | `learning_more_knowledge` | `knowledge_append` |
| find by keywords (ranked) | `search_knowledge` | (unchanged) |
| return a topic's full content | `recall_knowledge` | `knowledge_load` |
| list all remembered topics | `browse_knowledge` | `knowledge_list` |
| drop a piece of knowledge | `forget_knowledge` | `knowledge_delete` |
| show the keyword vocabulary | `keywords_knowledge` | (unchanged) |

Result messages are reframed to match: no `.md` filenames, no byte
counts, no file verbs — e.g. `learned "python async" (2 keywords)`,
`expanded "python async" — added to what you already knew`,
`forgot "python async"`; pinned (not "FIXED"/"immutable"). Descriptions
state the store is memory, NOT a file store, and redirect file work to
the `fs` tool. The C++ factory keeps its internal name
`knowledge_split_tools()` (`rag_tools.cpp`).

## Knowledge Loop (memory + web)

Mandatory workflow when both memory and web tools are available:

1. **Memory first** — `search_knowledge` relevant keywords, then `recall_knowledge` the hits
2. **Web second** — also search the web, even if memory had results
3. **Merge & answer** — combine both, prefer more recent/authoritative on conflict
4. **Update memory** — remember durable new facts (`learning_knowledge` / `learning_more_knowledge`) the web provided

Enforced in three places: system preamble (preamble.cpp), memory tool description (rag_tools.cpp), web tool descriptions (builtin_tools.cpp).

## Knowledge Tool Limits

| Operation | Default | Max |
|-----------|---------|-----|
| `search_knowledge` (results per page) | 10 | 20 |
| `browse_knowledge` (topics) | 50 | 200 |
| `keywords_knowledge` (vocabulary) | 200 | 500 |

## Entry Identity

Keywords are the sole identifier. Sorted + joined by `_` = filename stem.
Example: `"python async sockets"` → file `async_python_sockets.md`.
Files starting with `fix-` are pinned/immutable (cannot overwrite or
forget). The `.md` stem and byte sizes are an on-disk detail and are NOT
surfaced to the model — result messages show the keyword phrase only.

## learning_more_knowledge Behavior

| Knowledge exists? | Behavior | Return message |
|--------------|----------|----------------|
| Yes | Append content after `---` separator | `expanded "python async" — added to what you already knew` |
| No | Remember it fresh | `learned "python async" (new)` |
| Pinned (`fix-*`) | Error | Pinned, cannot add to it |

## Knowledge content policy (2026-06-05)

The knowledge store holds **only knowledge and information** — facts,
concepts, decisions, how-tos. It is **never** a file store: files and
file content must NOT be written through `learning_knowledge` /
`learning_more_knowledge`, EVER. File reads/writes go through the `fs`
tool. Enforced in the `learning_knowledge` and `learning_more_knowledge`
tool descriptions (`rag_tools.cpp`), which state the store is memory and
redirect file work to `fs`.

## Tools-in-prompt Contract

Three independent channels the model sees per turn — these MUST stay in sync.

| Channel | Contains | Producer | Source field |
|---|---|---|---|
| `<tools>` block in system message (Jinja template) | `name + short trigger + JSON schema` | `Engine::Pimpl::chat_tools()` / `Client::tool_to_json()` | `Tool::wire_description()` |
| Inline "Active tools" sections in system prompt | `### name` + trigger body per tool | `easyai::preamble::tools_block(view)` | `Tool::short_description` (else first line of `description`), capped 400 |
| MCP `tools/list` response | `name + full description + inputSchema` | `easyai::mcp::tool_descriptor(t)` | `Tool::description` (full) |

Authoring rule: every tool sets both `.short_describe(...)` and `.describe(...)`. `wire_description()` falls back to the first line of `.describe(...)` if `.short_describe(...)` was omitted (pre-Shape-C tools keep working).

`tool_lookup`:
- **No arg** → INDEX view: numbered `name: short` list. Cheap, scannable.
- **`name=<substring>`** → MANUAL view: full `.describe()` body for every match. The expanded help text the model drills into when the short trigger isn't enough.

### Write/edit policy (cross-tool, enforced both in prompt AND in code)

| Tool | Disk reads | Disk writes/edits |
|---|---|---|
| `fs` (or split `*_file`) | yes | **YES — primary** |
| `bash` | yes | **YES — for shell features fs can't do** |
| `evaluate` (legacy alias `python3`; runtime is Python 3) | yes (read-only) | **NO** — sandbox preamble rejects any write-mode `open()`, even inside the sandbox root |

Code enforcement: `kPythonSandboxPreamble` (src/builtin_tools.cpp) wraps `builtins.open`, `io.open`, and `os.open` to raise `PermissionError` on any `w/a/x/+` mode or `O_WRONLY|O_RDWR|O_CREAT|O_TRUNC|O_APPEND` flag. Read-only `r`, `rb`, default-mode open continue to work inside the sandbox.

Prompt enforcement: `easyai::preamble::tools_block(view)` emits the `## Write/edit policy (AUTHORITATIVE)` section whenever `evaluate` is registered. MCP servers inject the same policy via `initialize.result.instructions` so MCP clients' models see it too.

### Model-facing rename — `python3` → `evaluate` (2026-05-26)

To defeat the model's strong "python = write files / call subprocess / fetch URL" training prior, the **model-facing** tool name was renamed from `python3` to `evaluate`. The **operator-facing** surface (CLI flag `--no-python`, INI key `allow_python`, the runtime binary `python3 -I -S -E -c`) is unchanged.

`canonical_tool_name("python3")` returns `"evaluate"` so resumed chat sessions, hardcoded manifest reservations, and any legacy caller that dispatches by the old name still work — the dispatcher routes the legacy name to the new tool, no second schema shipped.

The model's short trigger is now: `"Evaluate Python 3 code for compute / algorithm prototyping. FORBIDDEN: filesystem, subprocess, network, ctypes. Stdlib compute only."` — the first non-generic word is `evaluate`, framing the affordance as expression evaluation, not file authoring.

### Per-turn KV-cache friendliness

`easyai::preamble::build()` emits blocks in this order so prompt-eval cache survives a memory write:

1. AUTHORITATIVE DATE/TIME
2. KNOWLEDGE CUTOFF
3. KNOWLEDGE LOOP rules
4. CITE SOURCES
5. **MEMORY VOCABULARY** (volatile — appended at the tail)

`render_memory_vocabulary` caches the rendered string keyed on `(root_dir, directory mtime, file count)`. Memory saves bump the directory mtime via `rename(2)`, invalidating the cache without explicit signalling. Warm path: one `stat()` per request. Edge case: filesystems with second-resolution mtime (HFS+, some NFS) can serve one second of stale vocab if two writes within the same second leave the file-count unchanged — accepted because vocab is advisory (cf. SECURITY_AUDIT §23.3).

### Prompt-render sanitization

`easyai::preamble::tools_block` runs `t.name` and each tool's section body (`short_description`, else the first line of `description`) through `sanitize_for_prompt(s, cap)` before emitting the per-tool `### name` sections. C0 control bytes (`0x00`–`0x1f`) and `DEL` (`0x7f`) collapse to a single space; UTF-8 multi-byte (`0x80+`) passes through. Caps: 64 chars (name), 400 chars (body, then UTF-8-safe `truncate_utf8`). Closes the structural-corruption prompt-injection vector when `active_tools` is populated from a less-trusted source (e.g. cli's `/v1/tools` runtime fetch). See SECURITY_AUDIT §23.1.

### `evaluate` disk enforcement (Python 3 sandbox preamble)

The `kPythonSandboxPreamble` injected before every `evaluate` snippet enforces TWO invariants:

1. **Sandbox containment** — open() / io.open() / os.open() reject paths resolving outside the cwd (sandbox root).
2. **Read-only** — write-mode `open(...)` rejected regardless of path. Mode chars `w/a/x/+` (any case) on `builtins.open` / `io.open`; flags `O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND` on `os.open`.

PermissionError messages point the model at the filesystem write tool registered this session (the exact callable name is read from the model's AVAILABLE TOOLS list — that way the error message stays correct whether the operator chose Split mode `write_file` or Unified mode `fs(action="write")`). Read-only opens inside the sandbox continue to work (legitimate "load CSV, compute, print result" flows are unaffected).

**Documented residual:** Python's `__closure__` introspection on `builtins.open` recovers the unwrapped open from the closure cell, bypassing both checks. Same class as the existing `ctypes` / `_io.FileIO` / `subprocess` bypasses — adversarial intent is out of scope; defense is against accident. See SECURITY_AUDIT §23.2.

## llama.cpp API port + pinned sibling checkouts (AUTHORITATIVE — 2026-10-15)

**Plain words.** easyai compiles against the sibling checkout `../llama.cpp`
(and `../whisper.cpp`). Upstream changed its chat API on 2026-10-14
(`8b5436102 chat: refactor API`, `ff026f8c0 chat: replace output string
with parts`) and the Linux installer, which cloned floating `main`, stopped
building. easyai now follows that API, and the installer checks out the
exact upstream commits easyai was built and tested against.

```mermaid
flowchart LR
  T[chat template] -->|common_chat_templates_apply| P["common_chat_params\nprompt: parts, parser: PEG arena"]
  P -->|prompt.str| TK[common_tokenize]
  P -->|parser copied| PP[common_chat_parser_params]
  G[generated text] -->|common_chat_input| PARSE[common_chat_parse]
  PP --> PARSE
  PARSE --> M["common_chat_msg\ncontent / reasoning_content / tool_calls"]
```

**Rules.**

1. `common_chat_params::prompt` is a `common_chat_prompt` (template parts and
   user-input parts). Any code that tokenises or prints the rendered prompt
   uses `prompt.str()`.
2. `common_chat_params::parser` is a `common_peg_arena` value, not a
   serialised string. It is copied into `common_chat_parser_params::parser`;
   there is no `load()`. An empty arena makes `common_chat_parse` a
   content-only parser.
3. `common_chat_parse` takes a `common_chat_input`. easyai wraps plain text
   with `common_chat_input(text)` (token ids unknown, all `LLAMA_TOKEN_NULL`);
   the lenient PEG parse behaves as before for both partial (streaming) and
   final parses.
4. `parse_tool_calls`, `reasoning_format`, `reasoning_in_content` no longer
   exist on the parser params. Reasoning and tool-call extraction are encoded
   in the arena the template produced, so `Engine::reasoning_format` keeps
   acting through `common_chat_templates_inputs::reasoning_format` at render
   time only.
5. The installer defaults `llama_ref` to `fd55cb98fe29d3df0054d3f3e6e6f15958ea72ab`
   (2026-10-15) and `whisper_ref` to `0c7be696371069e0baa2515ed7aa08d5a5473aa6`
   (2026-10-12). `--llama-ref <sha|tag|latest>` / `--whisper-ref …` override;
   `latest` means floating upstream main. A port to a newer llama.cpp
   updates both the code and these two defaults in the same commit.
6. CI builds what the installer builds (2026-11-21). Every llama.cpp clone
   in `.github/workflows/` (build.yml and release.yml, three jobs each)
   runs `scripts/ci_clone_llama.sh`, which reads the 40-hex `llama_ref`
   from `install_easyai_server.sh` and fetches exactly that commit
   (`git fetch --depth 1 <url> <sha>`). The pin has one home; no workflow
   clones a floating branch. `tests/test_installer.sh` fails on an
   unpinned `git clone … llama.cpp` in a workflow or a missing script call.
   Why: CI cloned floating main and went red on all three Linux jobs from
   `1d43d60` on — upstream changed `common_get_device_memory_data()` to a
   path list while easyai is pinned at `fd55cb98` (b11658+1), so CI was
   testing a combination no install builds. Verified: the script fetches
   `fd55cb98…` in 6 s, and a clean `git archive HEAD` copy configures and
   builds with CI's exact commands against it.

**Decisions.** Pinning in the installer rather than vendoring: a submodule
or vendored copy would double the repository size and still need the same
bump discipline. Floating main (previous default) was rejected because an
upstream API change breaks every fresh Linux install the same day, which
is what happened here. `latest` is kept as an explicit opt-in for testing
the next port.

**Proof.** macOS: full build green against `fd55cb98f`, ctest 3/3
(`reasoning_effort`, `stt`, `model_search`). Live server on the toy model
with `qwen3-think.jinja`: non-stream `/v1/chat/completions` returns content,
stream returns `reasoning_content` deltas separately from content, and
`/v1/audio/transcriptions` transcribes `jfk.wav` unchanged. Linux: the
pasted errors (`parse_tool_calls`, `common_peg_arena::load`,
`common_chat_input`, `common_tokenize(common_chat_prompt)`) are the four
sites rules 1–3 cover; Linux rebuild against the pinned sha pending on Gustavo's box.

**Limits.** `common_chat_input(text)` carries no token ids, so the parser
cannot tell a special token typed by the user from one emitted by the
model; the old string API had the same limit. Using the new
`common_chat_session` (tokenised input, prefill echo) is a larger change
left for a later port.

## Installer self-upgrade (AUTHORITATIVE — 2026-10-21)

**Plain words.** `install_easyai_server.sh --upgrade` pulls the easyai
checkout, and the script itself lives in that checkout. bash reads a
script while it runs it, so when the pull replaces the file the rest of
the run executes a mix of the old and the new installer. That is what
happened on `ai.local` on 2026-10-21: the June installer pulled the
October tree, built it, then wrote the June INI (no `[STT]`, `[TTS]`,
`[ASSISTANT]`), cloned no whisper.cpp, pinned no llama.cpp and never
wrote the assistant unit. The operator saw a successful run and no
assistant. Since this change the installer runs from a private copy and,
when the pull changed it, starts the new installer once with the same
arguments.

```mermaid
flowchart LR
    A[install_easyai_server.sh<br/>in the checkout] -->|copy to mktemp, exec| B[running copy<br/>old installer]
    B --> C[git pull --ff-only<br/>checkout now holds the new script]
    C --> D{cmp checkout script<br/>vs running copy}
    D -->|equal| E[carry on]
    D -->|differ, first time| F[exec new script with<br/>the original arguments<br/>EASYAI_INSTALL_REEXEC=1]
    F --> G[new copy: pull again, no-op,<br/>equal, carry on]
```

**Rules.**

1. The script copies itself to `mktemp -t install_easyai_server.XXXXXX`
   and re-executes the copy (`EASYAI_INSTALL_COPY`, `EASYAI_INSTALL_ORIGIN`
   carry the paths); the copy is removed on exit (`trap … EXIT`). No pull
   can change the file bash is reading.
2. Right after `fetch_repo "$easyai_dir"` under `--upgrade`, the checkout's
   script is compared with the running copy (`cmp -s`). When they differ
   and `EASYAI_INSTALL_REEXEC` is unset, the new script is started with
   the arguments saved at entry (`easyai_install_args`) and
   `EASYAI_INSTALL_REEXEC=1`; the old copy is deleted first. The second run
   pulls again (already up to date), finds the files equal and continues.
   Two files that keep differing cannot loop: the guard allows one restart.
3. `--help` and `--list-tags` go through the copy too (`$0` is the copy;
   the header comment is the same text).

**Decision.** Copy-and-compare instead of "pull first, then tell the
operator to run again": the operator's own history on both boxes shows
the installer run three times in a row for that reason, and the
mid-run mix is silent. Rejected: `exec` straight after the pull without
the copy (the lines between the pull and the `exec` would already come
from the new file); reading the whole script into a function first
(bash still reads a sourced file incrementally).

**Proof (2026-10-21, macOS).** The two blocks, extracted verbatim from
the script with `sed`, driven by a fake `fetch_repo` that swaps the
origin file: version 1 starts, the pull replaces it, the log line
"restarting with the new one" appears, version 2 runs with
`--upgrade --force --gtt 29`, `EASYAI_INSTALL_REEXEC=1`, exit 0, no
temporary copy left; a second run with an unchanged script does not
restart. `bash -n` and `--help` on the real script pass. On the box the
proof is the log of the next `--upgrade` that brings a new installer.

**Two more installer rules found on the same box (2026-10-21).**

4. **The INI template runs no commands.** The template is an unquoted
   heredoc (it expands `$service_port` and friends), so a backtick in a
   comment is a command substitution: the `[ASSISTANT]` comment
   `\`easyai-assistant --list-options\`` ran the assistant at install
   time, pasted its option table into the INI (36 lines "missing '='
   in entry") and the companion `\`easyai-assistant --text "question"\``
   asked the still-loading server a question. Backticks in the template
   are escaped; `tests/test_installer.sh` (ctest `installer`) fails on an
   unescaped one.
5. **Only the live library chains are installed.** The build tree keeps
   versioned files of earlier builds (ninja never deletes
   `libllama.so.0.7.0` when the pin goes back to a 0.6.0 tree) and
   `ldconfig` re-points a SONAME link at the highest version it sees:
   the server built against `libllama-common.so.0.6.0` loaded the stale
   `0.7.0` and aborted in `common_params` (`std::logic_error`, construction
   from null). The installer purges `$prefix/lib/easyai` of the easyai,
   llama, ggml, llava, whisper and httplib libraries, copies the symlinks
   and the files they resolve to (plus unversioned plain `.so` files), and
   runs `ldconfig -X` (cache only, links untouched). libeasyai's own chain
   joined the list on 2026-11-14: until then only the bare `libeasyai.so`
   was installed, as a plain file, while the binary loads the SONAME
   `libeasyai.so.0` — so the versioned file of the previous install stayed
   and the first upgrade that changed libeasyai's exported symbols
   (`Assistant::key_of_flag`, ai-pro) killed the service at start with
   `symbol lookup error`, exit 127, which `ldd` cannot see (every SONAME
   resolved). After copying, the installer runs the installed binary once
   against `$prefix/lib/easyai` (`--config /dev/null --assistant-options`,
   no model, exit 0) and `die`s before touching the unit when it does not
   run; `tests/test_installer.sh` checks the chain, the purge and the
   self-test. Every file in the model and voice directories is set to the
   service user (`chown -c`, logged when something changed): files placed
   by an earlier installer or by hand were root:root 0640 and the service
   could not open them (ai-pro: the STT model behind `stt.bin` and
   `ggml-tiny.bin`, "failed to open", from October to 2026-11-15 — the
   assistant heard nothing). An existing INI is migrated only for the two
   values the retired 2026-10-16 template wrote and nothing else: `name =
   easy` → `max` (the name since 2026-10-17; "hello Max" is the standard)
   and `wake_model = …/ggml-tiny.bin` → `ggml-base.bin`; `--assistant-name
   <name>` given on an upgrade sets the name in the INI as well. The name
   stays configurable in three places: `[ASSISTANT] name`,
   `--assistant-name` (server and installer) and the Assistant tab's Wake
   word group.

**Evidence of the bug.** `ai.local` reflogs: llama.cpp
`pull --ff-only` to `0a1ef309c` at 14:24 (unpinned, the June script),
easyai `pull --ff-only` 6e721ea → 5834558 at 14:26, binaries installed
14:29 from the October tree, INI rewritten 14:29 with the June sections
only.

## Running vs default in the local model list (AUTHORITATIVE — 2026-10-15)

**Plain words.** Two different things look like "the model" on the AI box:
the file the engine holds in memory, and the file the `ai.gguf` symlink
(`--model`) points at, which is what the next start loads. The engine
resolves the symlink once at load time (`Engine::load` follows it to the
real file), so after a **Slink** or a **Run (this session)** the two drift
apart on purpose. The page used to show only "running", so a box that
booted on Qwen3 and was then re-linked to Gemma2 showed Qwen3 as running
and nothing on `ai.gguf` / Gemma2, which read as a bug. Both truths are
now shown, on every file they apply to.

```mermaid
flowchart LR
  E["engine.model_path()\n(real file loaded at boot or hot-swap)"] -->|canonical ==| R["pill: running ●"]
  L["--model path (ai.gguf)\nresolved now"] -->|canonical ==| D["pill: default ○"]
  F[(each .gguf in download_dir)] --> R
  F --> D
  R -.differ.-> N["note: running X ≠ default Y\nrestart or Run the default"]
  D -.differ.-> N
```

**Rules.**

1. `GET /models/api/local` → `{dir, models:[{name, size_bytes, mtime,
   is_current, is_default, link_target}], running, default_link,
   default_target}`. `is_current`: the file's canonical path equals the
   engine's model path. `is_default`: it equals the canonical path of the
   configured `--model`. `link_target`: the whole symlink chain of the
   entry, hops joined by ` → ` (max 8 hops), so a stray
   `Qwen.gguf → ai.gguf → gemma.gguf` reads as a second name for gemma
   and not as Qwen weights (the AI box had exactly that: a Qwen-named
   link at the Gemma size, flagged running). The chain is a yellow chip
   (`pill marginal`, "symlink → …") on the row and in the detail panel
   (`link_target` is in the detail JSON too), Gustavo's call on
   2026-10-15: a link is important information, not a muted note. Link rows offer **Unlink**
   instead of Delete, always enabled: it removes the link only
   (`fs::remove` does not follow symlinks), the target file stays.
   Found while writing the test: `safe_in_dir` canonicalised the leaf
   name, so a delete of `ai.gguf` resolved to the weights behind it and
   removed those. The guard now validates the name (no separators, no
   `..`) and returns `download_dir/name` as is; `delete_local` uses
   `symlink_status`, so dangling links are listed (size 0, pill
   **broken link**) and can be unlinked.
   `running` and `default_target` are real paths; `default_link` is the
   `--model` path as configured.
2. `GET /models/api/local/detail` carries the same `is_current` and
   `is_default`.
3. `ModelsEngine` takes a second getter `default_model` (the server passes
   `args.model_path`); absent getter → no file is default.
4. Web `/models` Local and Downloads tabs: pill **running** (green) and
   pill **default** (blue), link rows show `→ target`, and a note above the
   table when `running ≠ default_target` saying which is which and that a
   restart or a Run of the default makes them agree. The detail panel
   shows both pills. Run / Delete stay disabled on the running file only.
5. TUI `--llm-manager` Local tab: `●` running, `○` default, `◉` both, with
   a legend line and the same mismatch note.

**Decisions.** Showing the link target rather than hiding `ai.gguf` from
the list: the link is the operator's handle and seeing where it points is
the whole question. Not auto-reloading the default: a Run is a deliberate
session choice; the note tells the operator how to resolve it.

**Proof.** New ctest `local_models` (`tests/test_local_models.cpp`, 14
checks) builds a temp dir with `gemma.gguf`, `qwen.gguf`, `ai.gguf →
gemma`, then moves the link and the engine path independently and checks
every flag on every file. Live server on the toy model: boot with
`ai.gguf → gemma` marks `ai.gguf` and `gemma.gguf` running+default,
`qwen.gguf` neither; after `ln -sfn qwen.gguf ai.gguf` the listing reports
`running gemma.gguf`, `default qwen.gguf`, `ai.gguf` default only. jsc
syntax check of the page script clean.

## easyai-assistant — voice assistant service + `/v1/audio/speech` (AUTHORITATIVE — 2026-10-16; built into easyai-server 2026-11-14)

**Plain words.** A voice assistant **built into `easyai-server`**
(`easyai::Assistant`, `include/easyai/assistant.hpp`, since 2026-11-14;
from 2026-10-16 to then it was a separate program, `easyai-assistant`,
with its own systemd unit — rules 13–16 say what moved and why). It
listens on the machine's own microphone all the time, wakes up when it hears a call
word and its name ("hello Max", "olá Max" — the default name is `max`,
the call words are configurable), plays a short rising sound when it
takes the question, records the question until
the speaker pauses for 1.5 s, sends the audio to a speech-to-text
endpoint, sends the text to a chat endpoint with its own short-answer
system prompt, and speaks the answer back with a male voice, sentence by
sentence while the rest is still being generated. It runs on Linux (ALSA)
and macOS (CoreAudio) with no third-party libraries. Transcription and
speech use the server's own `Stt` and `Tts` in-process by default; the
chat call is OpenAI-shaped (`POST /v1/chat/completions` on this server
over loopback), so the INI can still point it at easyai-server on another
box, llama-server, or OpenAI itself with an API key. Thinking is switched off per request;
tools (MCP / external) run inside the assistant like in easyai-cli, and the
system prompt makes the model say in one sentence that it is about to use
a tool. easyai-server gained the matching OpenAI endpoint
`POST /v1/audio/speech` on 2026-10-16 (piper on Linux, `say` on macOS;
Kokoro-82M in C++ since the same day, rule 10a) so the browser and any
client can get the same voice. The dashboard (`/models` → **Assistant**)
shows its state, the audio devices and the voices, edits every
`[ASSISTANT]` key and starts / stops it live (rule 14).

```mermaid
flowchart LR
  subgraph SRV["easyai-server (one process)"]
    direction LR
    M["mic 16 kHz mono\nALSA / CoreAudio"] --> V["VAD\nRMS vs adaptive floor"]
    V -->|burst ended| W["wake word\nwake_model = server (every burst through STT)\nor a local whisper base\nhello|olá <name>"]
    W -->|match, beep| L["listen until\npause_ms = 1500 of silence"]
    L -->|PCM16| S["Hooks.stt = the server's Stt\n(or stt_endpoint over HTTP)"]
    S -->|text, language| C["POST /v1/chat/completions on loopback\nstream, assistant system prompt,\nchat_template_kwargs.enable_thinking=false\n(endpoint = llama-server / OpenAI instead)"]
    C -->|tool_calls| X["tools run in the assistant\n(datetime, web, EASYAI-*.tools)"]
    X --> C
    C -->|sentence| T["Hooks.tts = the server's Tts (Kokoro)\nor tts_endpoint / local piper, espeak-ng, say"]
    T --> P["speaker\nmic muted meanwhile"]
  end
  D["/models → Assistant\n/models/api/assistant/*"] -.->|status, config, start/stop, say, ask| SRV
```

### Rules — assistant

1. **Audio in/out.** `easyai::audio::Capture` and `easyai::audio::Playback`
   (`include/easyai/audio_io.hpp`): PCM16 mono, capture at
   `Stt::kSampleRate` (16 000 Hz), playback at the rate the voice
   delivers. Linux: ALSA (`libasound`, system library), device names as
   ALSA knows them (`default`, `plughw:1,0`). macOS: CoreAudio
   AudioQueue, device `default`. `[ASSISTANT] mic`, `[ASSISTANT] speaker`.
   Capture is a blocking `read()` of one period (`kPeriodMs = 20`);
   the device clock paces the loop, no polling timer. ALSA playback sets
   explicit hw params like capture (access, S16, 1 channel, rate near,
   period `kPlayChunkMs = 250`, buffer `kPlayBuffers = 2` periods), not
   `snd_pcm_set_params()`: that helper derives the period from a latency
   and fails with "Unable to get period size for PLAYBACK" behind the
   plug layer when the card's rate is not an integer multiple of the
   voice's (ai.local, ALC269VC: 44100 Hz minimum for 24000 Hz Kokoro
   audio; the ALC245 at 48000 Hz never showed it, 2026-10-21).
1a. **Voice source.** `[ASSISTANT] tts = auto` (default) or `server`
   with an empty `tts_endpoint` speaks through `Hooks.tts`, the server's
   own `Tts` (`[TTS]`, Kokoro by default), in-process, no HTTP, no probe
   (since 2026-11-14, rule 13). When `tts_endpoint` is set (another
   box's voice) the pre-2026-11-14 behaviour applies: the endpoint's
   `/v1/audio/speech` is probed at start; such a server usually answers
   only once its model is loaded, so a failed probe does not fix the
   choice: every `kTtsProbeS = 30` s the next sentence is tried on the
   server first and the first answer switches the voice to it, logged as
   `voice: server …`; the local engine (Kokoro, piper) covers the gap;
   with none, the greeting is skipped with one log line and answers are
   logged only — never spoken by espeak-ng (rule 10, 2026-11-14). `tts =
   server` without a local engine prints the answers until then. `tts =
   kokoro|piper|espeak|say` forces the local `easyai::Tts`
   built from the `kokoro_*` / `piper` / `voice_*` keys (a Mac next to
   the box, or a server whose `[TTS]` has no engine).
2. **VAD.** Per period RMS in dB. Noise floor = minimum tracker that
   starts at the level of the first period (the room as it is; a fixed
   start made 20 s of constant fan noise count as speech on the box),
   falls at once and rises `kFloorRiseDbPerS = 1`. Speech when RMS >
   floor + `kSpeechMarginDb = 10`; a burst ends after `kBurstEndMs = 500`
   of non-speech and is dropped when it holds less than `kMinSpeechMs =
   200` of speech (clicks). Pre-roll `kPrerollMs = 300` is kept so the
   first syllable is not cut. Listening after wake ends after `pause_ms`
   (default 1500) of non-speech or `max_listen_ms` (default 20 000).
3. **Wake word.** Each burst (bounded to `kWakeMaxMs = 4000`) goes to a
   local whisper model `[ASSISTANT] wake_model` (a ggml path; the
   installer sets `/var/lib/easyai/models/ggml-base.bin`, 142 MB; tiny was
   measured too weak for a pt-BR call through a room) or, with the
   default `server` (since 2026-11-14; before that the default was
   `models/stt/ggml-base.bin`), to the server's own STT (`Hooks.stt`,
   the `[STT]` model, in-process). The transcript is lower-cased, punctuation stripped, and
   matched by `wake_match(text, name, aliases, call_words)`: one of the
   **call words** followed by the name. `[ASSISTANT] call_words` (csv,
   default `hello, olá, alô, hallo, hola`) is strict: the name only
   wakes right after one of them, never alone; each built-in call word
   carries the spellings whisper gives it (`default_call_words()`:
   hello, olá, alô, hallo, hola, halo, allo, elo, ola, alo; "A lo Max"
   with the call word split in two is joined). An empty list restores
   the loose pre-2026-10-17 set `{hey, hi, high, oi, o, oh, hoy, ei,
   em, ok, olá, ola, alô, alo, e, a}` (whisper writes "oi" as "O", "ei"
   as "Em" / "É e", "hi" as "High") and lets the bare name at the start
   wake too. Strict is the default because the loose set plus a short
   name fired on ordinary talk ("e mais", "o máximo"): measured
   2026-10-17 with Kokoro through whisper tiny and base, "Hello Max"
   came back as "Hello Max" 9 of 9 times in en, "Olá Max" as "Olá Max",
   "Elo Max", "Elomax", "Allo Max", "A lo Max" (all matched, one "O
   Lamá X" lost), and "Mas eu acho que o máximo é dez, mais ou menos" /
   "The maximum is ten and max out the volume" do not wake. The name may come back as one word or as up to
   `kMaxNameWords = 3` consecutive words ("easy ai", "easy a i"): the
   words are joined, longest join first, and the join is the name when
   its Levenshtein distance is ≤ `kNameMaxEdits = 1`, or ≤
   `kNameMaxEditsLong = 2` for names of 4+ letters, or when it equals
   one of the aliases (spaces ignored): built-in for `easyai` (`iziai,
   isiai, isia, ziai, siai, ezi, easya, gisiai, ijazia, izai, isai,
   easyeye, easyay`), `easy` and `max`, plus `[ASSISTANT] name_aliases`.
   The words after the join are the question, so "hello easy AI what time
   is it" asks "what time is it". A call word glued to the name
   ("oiasse", "heyeasy", "oisiai") matches too, for call words of two
   letters or more. Measured 2026-10-16 with Kokoro saying the default
   name (pt `pm_alex`/`pm_santa` "oi ísi ai", en `am_michael`/`am_adam`
   "hey easy AI") through whisper tiny and base: en came back as "Hey
   easy AI", "Hey easy A, I", "Hey, easy-a-a-i", "High Easy AI" (all
   matched); pt as "O Iziai", "Oisiai", "Oi Ziai", "Eisiai", "Easy I",
   "O Ijazia", "E ZI", "Oi, Gisiai" (all matched; one of fourteen,
   "O irreaziai", is not and is left out on purpose). Writing the name
   `easyai` as one word mis-leads Kokoro's phonemizer ("isae"), so the
   greeting spells it through `spoken_name(name, lang)`: "easy AI" for
   en, "ísi ai" for pt; other names (`max`) are spoken as written. Every
   transcription (local wake model and `/v1/audio/transcriptions`)
   carries the whisper prompt "O assistente chama-se <spoken en>. The
   assistant is called <spoken en>." (`Stt::transcribe(..., prompt,
   ...)` → `initial_prompt`), which makes whisper write the name the way
   `wake_match` expects. Measured on the same ten call phrases with the
   prompt: base writes "O easy AI que horas são agora", "Easy AI como
   você está", "Hey easy AI, what time is it?" (9 of 10 with the name,
   the tenth "Oi Reazi AI" is an alias now); tiny 9 of 10 as well. A
   call phrase as prompt ("Hey easy AI. Oi easy AI.") is worse: whisper
   treats it as text already said and drops the real call in 6 of 12
   phrases, so the prompt is a sentence *about* the name, never the
   call itself. Words after the name mean the question came in the
   same breath: the same burst audio is then sent to the STT endpoint
   (the large model) and the words after the name in that transcript
   are the question, so the small model only ever decides the name;
   otherwise the assistant beeps (`kBeepHz = 880`, `kBeepMs = 120`,
   generated, no file) and listens. Without a wake model the burst goes
   to the STT path of rule 4 instead (`wake_model = server`: the
   server's own `Stt`, or `stt_endpoint`).
3a. **Processing sound.** The moment the question is taken (the pause
   ended, or the name and the question came in one breath) the
   assistant plays `make_think_sound`: a rising C6–E6–G6 arpeggio
   (`kThinkHz = {1047, 1319, 1568}`, `kThinkToneMs = 45` each, half-sine
   envelope, `kThinkAmp = 0.22`, 135 ms in all), then transcribes. It is
   the third sound and means "got it, working": the single beep means
   "speak now", the falling two-tone means "done". Capture is flushed
   after it like after every sound.
3b. **Microphone gain.** `[ASSISTANT] mic_gain_db` (default 0, bounded
   to `kMicGainMinDb = -30` … `kMicGainMaxDb = 30`) is a software gain
   applied to every period before the VAD and the wake model
   (`apply_gain_db`: ×10^(dB/20), clipped to int16). It does not undo a
   saturated hardware input — for that lower the ALSA capture volume —
   but it lets a quiet or a hot room be levelled from the INI.
4. **Transcription.** The recorded utterance goes to `Hooks.stt` when
   the owner set it and `stt_endpoint` is empty: the server's own
   `Stt::transcribe(pcm, language, prompt)` under `stt_mu`, in-process,
   no WAV, no HTTP (since 2026-11-14; `false` + err when no `[STT]`
   model is loaded, which then falls through to HTTP). Otherwise it is
   sent as PCM16 WAV to `stt_endpoint` (default: `endpoint`) `POST
   /v1/audio/transcriptions` multipart, `response_format=verbose_json`.
   Both paths carry `prompt` (the sentence naming the assistant, rule 3)
   and `language` from `[ASSISTANT] language` (`auto` default). The
   returned `language` drives the voice. An empty transcript, or one that is not a question
   (`looks_like_question`: fewer than `kMinQuestionWords = 2` real words
   after dropping whisper's noise markers `*crash*`, `[BLANK_AUDIO]`,
   `(music)`; on the box a beep echo became "phase" and was answered) →
   short double beep, back to idle.
4a. **Prompt echo.** Whisper given an initial prompt and audio it cannot
   make out (silence, a far speaker, a noisy room) answers with the prompt
   itself, repeated: on ai.local (speaker-to-mic SNR about 13 dB) every
   burst came back as "The assistant is called max. The assistant is
   called max. …" (2026-10-21). `drop_prompt_echo(transcript, prompt)`
   (`assistant_core`) removes every sentence of the prompt from the
   transcript before the wake check and before the question is taken,
   normalized compare, so such a transcript is empty and wakes nothing;
   a real sentence after an echoed one survives ("… called max. Hello
   Max, what time is it?" → "Hello Max, what time is it."). Proof: eight
   checks in `tests/test_assistant.cpp`.

5. **Chat.** `easyai::Client` to `endpoint` with `api_key`, `model`,
   streaming. System prompt = `[ASSISTANT] system` (file or inline)
   else the built-in `kAssistantSystem`: answer in ≤ 3 sentences unless
   asked for detail, same language as the question, no lists, no
   markdown, no reasoning or preamble, before calling any tool say
   one short sentence naming it ("Vou consultar a ferramenta X." /
   "Let me check X."), never cite where an answer came from (no
   sources, sites, links or tool names in the answer itself), and use
   the internal clock for date and time questions. The **internal
   clock**: every turn the system prompt is re-set to the base prompt
   plus `clock_line()` — "Internal clock: Friday, 2026-10-17 14:32
   (-03)." from `localtime` — so "que horas são" is answered without a
   tool and a long-running process never drifts. **The
   language is enforced, not hoped for (2026-11-15, Gustavo: "match the
   language it is speaking … imperative … enforce it").** The turn's
   language is `pt` or `en`: whisper's detected language when the
   question was spoken, `detect_language(question)` for a written one,
   `[ASSISTANT] language` when forced; anything else counts as `en`
   (Limits). Every turn the system prompt also carries
   `language_line(lang)` — "Idioma desta resposta: português do Brasil.
   Responda somente em português…" / "Language of this answer: English.
   Answer only in English…". The first complete sentence of the stream
   (or the whole answer when it is shorter than one sentence) goes through
   `detect_language` before anything is spoken: a confident other language
   cancels the attempt (`Client::request_cancel`), the conversation is
   rolled back to `dump_history()` taken before the turn, and the question
   is asked once more with `language_nudge(lang)` appended ("(Responda
   somente em português do Brasil.)" / "(Answer only in English.)"), logged
   as "answer came in English to a question in Portuguese — asking again".
   The retry is spoken whatever it brings (its sentences still get the
   matching voice, rule 6). A cancel that came from `stop()` or a
   reconfiguration is never retried. The request carries
   `chat_template_kwargs: {"enable_thinking": false}` and
   `reasoning_effort: "low"`; a server that ignores them still works.
   Tools: `[ASSISTANT] tools` (csv, default `datetime,web`) plus
   `[ASSISTANT] external_tools` directory of `EASYAI-*.tools`; the
   assistant runs them (the server only forwards `tool_calls`).
   Conversation memory: the last `history_turns` (default 6) turns are
   kept, cleared after `history_idle_s` (default 300) of silence.
6. **Speech out.** `SentenceChunker` cuts the token stream at `.`, `!`,
   `?` followed by space/newline, or at newline, with a minimum of
   `kMinSentenceChars = 24`; a period after a one-letter word or a word
   that already contains a period ("e.g.", "i.e.") never cuts, and
   "3.14" stays whole because no space follows its period. Each
   sentence goes to `Hooks.tts` (the server's `Tts::synthesize(text,
   voice, pcm, rate)` under `tts_mu`, voice `pt` / `en`) when the owner
   set it, `tts` is `auto` or `server` and `tts_endpoint` is empty; else
   it is sent to `tts_endpoint` `POST /v1/audio/speech` `{model, input,
   voice, response_format: "wav"}`; else to the local `easyai::Tts`. The
   clip is played and the next sentence is fetched while the current one
   plays (one-deep queue). `[ASSISTANT] tts =
   auto|server|kokoro|piper|espeak|say`: `auto` uses the server's voice
   when it answers, else local Kokoro when `kokoro_model` / `kokoro_voices`
   are set, else `piper` if `[ASSISTANT] piper` is set, else `say` on
   macOS, else nothing (espeak only when written by hand). **The voice
   follows the sentence, not the question** (2026-11-15): every sentence
   is spoken with `voice_for(sentence)` = `detect_language(sentence)`,
   the turn's language only when the text gives no evidence — so a
   Portuguese answer to a question whisper took for English is still
   read by `pm_alex`, and a mixed answer switches voice per sentence.
   `detect_language` (assistant_core) weighs the letters only Portuguese
   writes (ã õ ç á é í ó ú â ê ô à) twice and counts the function words of
   each language; words both use (a, me, no, do, as, ok) count for
   neither; a tie is "" (undecided). Voice
   per language: `voice_pt`, `voice_en` (server: `pt`, `en` voice ids;
   `say`: `Eddy (Portuguese (Brazil))`, `Daniel`; piper: paths of the
   `.onnx` voices). Capture is muted from the first sentence until
   playback drains plus `kUnmuteMs = 300`.
5a. **Conversation mode.** After every answer the assistant plays the
   same single beep that acknowledges the name ("you can speak now")
   and keeps listening for `[ASSISTANT] followup_ms` (default 2000,
   0 = off) without needing the name: speech in that window starts the
   next question at once (same pause rule), its answer ends with the
   beep again and re-opens the window; a noise that is not a question
   extends it; silence for the whole window ends the conversation with
   the falling two-tone "off" sound (`make_off_sound`: 660 Hz then
   440 Hz, 110 ms each). So the ear hears: name → beep → question →
   answer → beep → (question → answer → beep …) → off sound. The
   microphone is flushed after every sound and answer so none of them
   is heard.
6a. **Own sound never heard.** `Capture::flush()` drops what the device
   buffered during a synchronous beep or a spoken answer; it runs after
   every beep, after the greeting and after each answer (on the box the
   beep itself came back as a "question" before this).
6b. **Greeting.** `[ASSISTANT] greeting = en|pt|off` (default `en`):
   once the microphone is open the assistant says "The max assistant
   is online. Say 'hello max' and ask your question." (pt: "O
   assistente max está no ar. Diga 'olá max' e faça a sua pergunta."),
   the name through `spoken_name`, the call word "hello" / "olá" (or
   "hey" / "oi" when `call_words` is empty), so a restart proves the
   voice path end to end.
7. **Availability.** Endpoint down or non-2xx: spoken "servidor
   indisponível" / "server unavailable" once per outage (language =
   `[ASSISTANT] language` when not `auto`, else the last heard one, else
   en), spoken once per outage; every later question is tried again at
   once, there is no backoff timer. A missing
   microphone is not an error either: state `no_mic`, the device is
   retried every `kMicRetryS = 15` s (an unplugged USB mic). Every log
   line goes to stderr as `[assistant] …` and into a ring of
   `Assistant::kLogKeep = 400` lines that the dashboard shows. Test
   modes: `easyai-server --assistant-say "text"` speaks a text on the
   configured speaker and exits (rule 15), `POST
   /models/api/assistant/ask {text}` answers a written question through
   the chat and voice path (rule 14). Until 2026-11-14 these were
   `easyai-assistant --once` and `--text "<question>"`.
8. **Config.** `[ASSISTANT]` section of the server's INI file
   (`/etc/easyai/easyai.ini`, `--config`), read by
   `Assistant::from_ini`. Every key is one field of `Assistant::Config`,
   one row of `Assistant::options()` (key, flag, type, default, choices,
   min/max, dashboard group, help — the single source of truth for the
   CLI, the API and the panel) and one easyai-server flag
   `--assistant-<key-with-dashes>` (`Assistant::key_of_flag`), CLI over
   INI like the other sections: `enabled` (start with the server, default
   off; the installer writes `on`), `name`, `call_words`, `name_aliases`,
   `greeting`, `endpoint` (empty = this server over loopback), `api_key`
   (empty = the server's own key), `model`, `stt_endpoint`,
   `tts_endpoint` (empty = in-process), `language`, `pause_ms`,
   `followup_ms`, `max_listen_ms`, `wake_model` (default `server`), `mic`,
   `mic_gain_db`, `speaker`, `tts`, `voice_pt`, `voice_en`, `piper`,
   `kokoro_model`, `kokoro_voices`, `tools`, `external_tools`, `system`,
   `history_turns`, `history_idle_s`, `verbose`, `tls_insecure`.
   `Assistant::set(cfg, key, value, err)` validates type, range and
   choices and is the one path every writer (flag, INI, dashboard)
   goes through. Until 2026-11-14 the flags were `easyai-assistant
   --<key-with-dashes>` and there was no `enabled` (the unit was).

### Rules — server

9. **`POST /v1/audio/speech`** (OpenAI shape, Bearer auth like `/v1`).
   Body `{model, input, voice, response_format, speed}`; `input` ≤
   `kTtsMaxChars = 4000`; `response_format` `wav` only (415 otherwise);
   `voice` is `pt`, `en`, or an engine voice name; `model` ignored.
   Response `audio/wav`, PCM16 mono at the engine's rate. 503
   `tts_not_available` when no engine is configured, 500 on engine
   failure with the engine's stderr tail. One synthesis at a time
   (`tts_mu`).
10. **Engines.** `[TTS] engine = auto|http|piper|say|espeak|none`, `url`
    (engine http), `piper` (binary path), `voice_pt`, `voice_en` (piper:
    `.onnx` paths, sample rate read from the sidecar `.onnx.json`; `say`:
    voice names; espeak: `pt-br`, `en`). Local engines are child
    processes: piper `--model <onnx> --output-raw` (raw PCM16), `say -v
    <voice> -o <tmp>.caf --data-format=LEI16@22050`, `espeak-ng -v <v>
    --stdout`. **`http`** forwards the request to a helper that speaks the
    same `POST /v1/audio/speech` shape on `url` and must answer `GET
    /health`; the best local engine is kept as **fallback** and used,
    with one log line, when the helper fails. `auto` = http when `url`
    is set and answers, else piper if the binary and a voice exist, else
    `say` on macOS, else none: **espeak-ng is never chosen automatically**,
    neither as the engine nor as the fallback behind kokoro / http — a box
    without a natural voice answers 503 and the assistant stays silent and
    logs why (Gustavo, 2026-11-14: "never use the robotized one"). `engine
    = espeak` written by hand still works. `/health` adds
    `tts_fallback` when the engine is http. A voice name that is neither `pt`/`en` nor an
    existing file is spoken by piper with the English voice instead of
    being passed as a model path (piper aborts with "Model file doesn't
    exist" and leaves a core dump per request; seen on ai-pro,
    2026-10-28).
10a. **Natural voice: Kokoro-82M in C++ (`easyai::Kokoro`,
    `src/kokoro.cpp`).** Engine `kokoro`: `[TTS] kokoro_model`
    (`kokoro-v1.0.onnx`), `kokoro_voices` (`voices-v1.0.bin`),
    `kokoro_voice_pt` / `kokoro_voice_en` (default `pm_alex` /
    `am_michael`, male), `onnxruntime`, `espeak_lib`, `espeak_data`. The
    ONNX graph runs through the onnxruntime **C API opened with
    `dlopen`** (header vendored in `third_party/onnxruntime/`, API table
    requested at version 16 so any ORT ≥ 1.16 works; search order:
    config, `EASYAI_ONNXRUNTIME`, `/opt/easyai/onnxruntime/lib`, system,
    brew). Grapheme-to-phoneme is **libespeak-ng opened with `dlopen`**
    (`espeak_Initialize` with `espeakINITIALIZE_DONT_EXIT`,
    `espeak_SetVoiceByName`, `espeak_TextToPhonemes` in IPA mode),
    system or brew locations searched. The voices file is a numpy `.npz`
    read through its central directory (numpy writes streaming local
    headers), 54 voices of float32 [510, 1, 256]. Text handling mirrors
    kokoro-onnx 0.6.1 exactly: punctuation marks `;:,.!?¡¿—…"«»“”(){}[]`
    are split off, each chunk phonemized, marks re-attached, espeak
    language-switch flags `(en)` and `_` separators removed, symbols
    outside the vocabulary dropped (vocabulary from the model's
    `kokoro_config` metadata when present, else the built-in table
    `src/kokoro_vocab.inc`); batches of ≤ 510 phonemes cut at sentence
    marks, then clause marks, then spaces, balanced so no short tail;
    style row = `voice[min(n_tokens, 510) − 1]`; input `[0, tokens…, 0]`;
    librosa-style trim (60 dB, 2048/512) per batch; 0.25 s after `.!?…`
    and 0.10 s after `,;:` between batches; output 24 kHz PCM16. The
    phonemes for a pt-BR and an en sentence are byte-identical to python
    phonemizer 3.4 + espeak-ng 1.52 (asserted in `tests/test_kokoro.cpp`).
    No Python anywhere: the helper script of the first Kokoro commit is
    removed and the installer deletes its unit and venv on upgrade. The
    two model files are committed through Git LFS in `models/tts/` with
    `models/tts/README.md` (source release, upstream model, sizes,
    SHA-256, Apache-2.0, inputs, languages, voice prefixes); the
    installer copies them from the checkout and downloads only when it
    finds LFS pointers, like the STT weight.
11. **Thinking per request.** `chat_template_kwargs.enable_thinking`
    (boolean) in the request body sets the engine flag for that request
    under the engine lock and restores the server default after it, same
    pattern as `reasoning_effort`. Absent → server default.
12a. **Services tab** (dashboard `/models` → Services; routes gated like
   the rest of `/models/api`). `GET /models/api/services` → `manager`
   (`systemd` | `none`), one entry per unit of `admin::kUnits` — since
   2026-11-14 only `easyai-server` (the assistant lives in it, rule 13;
   from 2026-10-17 to then `easyai-assistant` was the second entry) —
   from `systemctl show -p LoadState,ActiveState,
   SubState,UnitFileState,ActiveEnterTimestamp,MainPID`, and the config
   file's `path` / `exists` / `writable`. `POST /models/api/services/
   action {name, action}` runs `systemctl start|stop|restart <unit>` as
   the service user (400 with the list of known units for any other
   name); the installer's polkit rule
   `/etc/polkit-1/rules.d/50-easyai-services.rules` answers yes for
   `org.freedesktop.systemd1.manage-units` on exactly that unit
   for that user, so there is no sudo and no root in the server. A
   stop/restart of `easyai-server` itself answers first and runs the
   command `kSelfRestartDelayMs = 400` later from a detached thread.
   `GET /models/api/services/log?name=&lines=` is `journalctl -u <unit>
   -n <lines> -o short-iso` (the unit runs with
   `SupplementaryGroups=systemd-journal`). `GET /models/api/config`
   returns the INI text; `POST /models/api/config {text}` (≤ 1 MiB)
   checks every line is a comment, `[section]` or `key = value` (400
   with the line number otherwise), keeps the previous content in
   `<ini>.prev`, writes `<ini>.tmp` and renames it over the file
   (atomic; in-place write when the directory refuses new files). The
   installer makes the file `660` and the directory `2770`
   (`root:easyai`) for that. Without systemd (macOS) the status reads
   `unknown`, the buttons are disabled with 501, and the editor still
   works. The page warns in the editor when the dashboard has no
   password, because the file holds the API keys.
12. **Surface.** `/health` adds `tts_available`, `tts_engine`, and since
    2026-11-14 `assistant_enabled`, `assistant_running` (the live state —
    who is talking in the room — stays behind the dashboard password);
    `/models/api/status.services` adds the same TTS fields and
    `assistant: {enabled, state, mic, voice}`; the Status tab's service
    chips gain `assistant <state>`; the endpoint directory
    adds the `Audio` row for `/v1/audio/speech`; the chat UI gets a
    speaker toggle on the answer later (not in this change).

### Rules — the assistant inside easyai-server (2026-11-14)

13. **In-process assistant.** `easyai::Assistant`
    (`include/easyai/assistant.hpp`, `src/assistant.cpp`, in libeasyai) is
    the whole old program minus `main()`: `Config` (rule 8), `Hooks`,
    `start(cfg, hooks, err)` / `stop()` / `running()` / `status()` /
    `status_json()` / `recent_log()` / `say()` / `ask()`. easyai-server
    owns one instance (`ServerCtx::assistant`) and lends it, through
    `Hooks`, what it already has: `stt` = `ctx.stt.transcribe` under
    `stt_mu`, `tts` = `ctx.tts.synthesize` under `tts_mu`, `log` = stderr
    `[assistant] …`, and the loopback `self_endpoint`
    (`http(s)://127.0.0.1:<port>`, or `args.host` when it is a concrete
    address), `self_api_key` (the server's own key) and
    `self_tls_insecure`, used whenever `Config::endpoint` is empty. Both
    the server's key and its certificate allowance go to the server itself
    and to no other host: an `endpoint`, `stt_endpoint` or `tts_endpoint`
    that is not the loopback URL is called with `[ASSISTANT] api_key` when
    set and anonymously otherwise (the old program's `EASYAI_API_KEY`
    environment fallback is gone: the unit carries the server's key there),
    and redirects are not followed, so a Bearer token never travels to a
    second host (2026-11-14 security review). The
    chat itself stays an HTTP call through `easyai::Client` to `endpoint`
    (rule 5), so tools keep running in the assistant and the endpoint can
    be any OpenAI-shaped server. The loop is the one of rules 1–7
    (VAD → wake → beep → listen → think sound → transcribe → chat → speak
    → follow-up window → off sound; greeting; prompt echo drop; mic
    auto-pick from `/proc/asound/pcm` when `default` has no capture, the
    speaker following the mic card; mic retry every 15 s; server-down
    phrase once). Life cycle: with `[ASSISTANT] enabled = on` a thread
    waits until `svr.is_running()` and calls `start()` (logged); after
    `svr.listen()` returns the server calls `stop()`, which cancels a
    running answer (`Client::request_cancel`) and joins the thread; a
    `start()` while running restarts with the new config. `say()` and
    `ask()` work while running (the microphone is muted meanwhile) and
    while stopped (speaker opened for the clip, client built on demand),
    one playback at a time with the loop's own speaker. `Status` (`state`
    one of `off, starting, no_mic, idle, listening, thinking, speaking,
    conversation, error`, `since`, `mic`, `speaker`, `backend`,
    `wake_model`, `voice`, `tools`, `level_db`, `floor_db`, `speech`,
    `last_heard`, `last_question`, `last_answer` with times, `n_wakes`,
    `n_questions`, `n_answers`, `n_errors`, `last_error`) is updated at
    every state change. `start()` fails only for an unusable
    configuration (built without an audio backend); no microphone is
    `no_mic`, not an error. The old `services/assistant.cpp`, the
    `easyai-assistant` target and `easyai-assistant.service` are removed.
14. **Assistant panel and API.** Dashboard `/models` → **Assistant** tab
    (between Downloads and Status; same login as the rest of `/models`):
    a state pill with a live dot and `since`, **Start / Stop / Restart**,
    an **Enabled** switch (writes `enabled` and applies at once); cards
    *Now* (mic, speaker, voice, wake model, tools, level meter from
    `level_db` vs `floor_db`, last heard / question / answer with times,
    counters), *Test* (text + language → **Say**; question → **Ask**, the
    answer shown) and *Log* (60 lines, **more** loads 400, refreshed with
    the status every 2 s while the tab is open); and the form, one
    control per row of `Assistant::options()` grouped by `group`
    (**Audio**: mic and speaker selects from the device list, `default`
    first, capture-only devices in mic and playback-only in speaker,
    label `id — name (card)`, **Refresh devices**, `mic_gain_db`;
    **Voice**: `tts`, the Kokoro voice for pt and for en from the
    catalogue grouped by language with `id · gender · grade`, only the
    voices the loaded model has enabled, `greeting`; **Wake word**:
    `name`, `call_words`, `name_aliases`, `wake_model` select (every
    `ggml-*.bin` in the `[STT]` model / download dir plus `server`),
    `language`; **Timing**: `pause_ms`, `followup_ms`, `max_listen_ms`
    within min/max; **Brain**: `endpoint` (placeholder "this server"),
    `model`, `api_key` (password field), `tools`, `external_tools`,
    `system`, `history_turns`, `history_idle_s`; **Advanced**: `verbose`,
    `tls_insecure`), each with its `help` line, unknown future keys
    rendered by type; footer **Save & apply**, **Save only**, **Revert**,
    a dirty mark. Routes, gated like `/models/api/services`:
    `GET /models/api/assistant/status` → `{status, enabled, log: [last 60
    lines]}` (light; polled every 2 s); `GET /models/api/assistant/options`
    → `{config: {key: value…}, options: [{key, flag, type, default,
    choices: [], min, max, group, help}], devices: [{id, name, card,
    capture, playback, is_default}], voices: {engine, fallback, kokoro,
    list: [{id, lang, gender, grade, available}], current_pt, current_en},
    wake_models: [{name, path, size_bytes}], ini: {path, writable},
    self_endpoint}`; `GET /models/api/assistant/devices` → `{devices}`;
    `GET /models/api/assistant/log?lines=N` → `{lines}` (N ≤ 400); `POST
    /models/api/assistant/action {action: start|stop|restart}` → runtime
    only, the INI untouched, `{ok, status}`, 400 on a bad action, 500
    with the error from `start()`; `POST /models/api/assistant/config
    {values: {key: value…}, apply}` (body ≤ 64 KiB) → every key validated
    with `Assistant::set` on a copy (400 with the first error and its
    key), then `config::set_ini_values(text, "ASSISTANT", values)` on the
    INI text read from disk — an existing `key = …` line replaced in
    place, a commented-out `# key = …` un-commented, a missing key appended
    to the section, a missing section appended at the end, every comment
    and other line kept — written with `admin::write_config` (`.prev`
    kept), `ctx.assistant_cfg` updated, and when `apply`: enabled →
    `start(new cfg)` (a restart), disabled → `stop()`; reply `{ok, saved,
    backup, applied, status, warnings: []}`. The two keys
    `tts.kokoro_voice_pt` / `tts.kokoro_voice_en` are accepted in the same
    body and written to `[TTS]` (`ctx.tts` re-configured under `tts_mu`),
    so the voice pickers of the panel change the server's voice too.
    `api_key` is returned as `""` when unset, else as the mask `••••`, and
    written only when the value sent is non-empty and not the mask. `POST
    /models/api/assistant/say {text, lang}` → `{ok, ms}` (text ≤ 500
    chars; at most `kMaxInflight = 2` say / ask calls run at a time, a
    third answers 429 so the HTTP workers are never all parked on the
    voice); `POST /models/api/assistant/ask {text}` → `{ok, answer, lang,
    ms}` (text ≤ 2000 chars). Public, Bearer like `/v1`: `GET
    /v1/audio/voices` → `{engine, fallback, voices: [{id, lang, gender,
    grade, available}]}` — the OpenAI audio shape has no voice listing, so
    this one is kept simple; when the engine in effect is not kokoro the
    list holds only that engine's pt and en voice, gender and grade empty. The catalogue is `kokoro_voice_catalog()`
    (static table of the 54 Kokoro v1.0 voices from hexgrad's VOICES.md:
    id, language tag, gender, upstream grade) and `kokoro_best_voice(lang,
    gender)`; `available` says whether the loaded voices file has it.
15. **CLI.** Four easyai-server flags print and exit before any model
    loads: `--list-audio-devices` (table: id, capture, playback, card,
    name, `default` marked — `audio::list_devices()` from
    `include/easyai/audio_io.hpp`: ALSA `snd_device_name_hint()` names plus
    one `plughw:<card>,<dev>` entry per `/proc/asound/pcm` line with its
    capture/playback flags, `default` first with `capture` = any card has
    capture, nothing opened so a listing never disturbs a running
    capture; macOS `default` plus the CoreAudio device names);
    `--list-voices` (configures `[TTS]` only, the start-up code path, and
    prints `engine`, `fallback` and the voices: id, language, gender,
    grade, `*` on the current pt / en voice; a non-kokoro engine prints its
    `voice_pt` / `voice_en`); `--assistant-options` (every `[ASSISTANT]`
    option: key, flag, value in effect after INI and flags, default, help);
    `--assistant-say "text"` (configures `[TTS]` and the assistant from the
    INI, speaks the text on the configured speaker through
    `Assistant::say`, exit 0/1; `--assistant-say-lang pt|en`, default
    auto) — the installer's voice check and the operator's quick test.
    Plus `--assistant-<key-with-dashes>` for every option (rule 8), e.g.
    `--assistant-enabled on|off`, `--assistant-mic plughw:1,0`.
16. **Installer.** `scripts/install_easyai_server.sh` installs every
    dependency of the voice path by default (`libasound2-dev alsa-utils
    espeak-ng libespeak-ng1`, onnxruntime), builds with
    `-DEASYAI_BUILD_TESTS=ON` and runs `ctest --test-dir build
    --output-on-failure` after the build (a failure is a `warn` naming
    the tests, not a stop). The INI template's `[ASSISTANT]` starts with
    `enabled = on` (`off` with `--no-assistant` or `--no-whisper`; `name`
    from `--assistant-name`), `endpoint` and `api_key` commented out (this
    server), `wake_model = /var/lib/easyai/models/ggml-base.bin`. The
    server unit gets `SupplementaryGroups=render video systemd-journal
    audio` (`usermod -aG audio easyai`) and `After=network-online.target
    sound.target`. **Stale artefacts**, whenever the service is written
    (also `--upgrade`): `easyai-assistant.service` and the older
    `easyai-tts-kokoro.service` are `systemctl disable --now`,
    `reset-failed`, their unit files and `.d/` removed, `daemon-reload`;
    `$prefix/bin/easyai-assistant` and `/opt/easyai/kokoro` removed; each
    removal logged, silent when nothing is there. The polkit rule names
    only `easyai-server.service`. The default downloads are SHA-256 pinned (the
    Kokoro files to the values in `models/tts/README.md`, whisper
    `ggml-base.bin`, the onnxruntime tgz for x64 and aarch64, piper and
    its voices); a Kokoro, whisper or onnxruntime mismatch removes the
    file and stops the install, a piper archive or voice mismatch removes
    the file and continues with a warning (piper is the fallback only).
    Other `--stt-model` / `--ort-version` choices have no pin and install
    with an "unverified" warning. At the end, with the service and Kokoro installed,
    `easyai-server --config /etc/easyai/easyai.ini --list-voices` must
    print `engine = kokoro`, else a `warn` with the reason and the fix.
    `tests/test_installer.sh` checks the script text for all of this (no
    `AUNIT` heredoc, no `easyai-assistant.service` written, `enabled` in
    the template, `audio` in the server unit's `SupplementaryGroups=`, the
    stale cleanup naming `easyai-assistant.service` and
    `bin/easyai-assistant`, `-DEASYAI_BUILD_TESTS=ON`, the `--list-voices`
    check).

### Decisions

- **Subprocess engines, not linked TTS.** piper's runtime is onnxruntime
  plus espeak-ng-data; linking it would add two heavy dependencies to
  libeasyai. A child process keeps the library clean and lets the
  installer swap voices. Rejected: implementing a TTS model in ggml
  (weeks), browser-side speechSynthesis for the assistant (no browser).
- **Kokoro-82M as the natural voice, in C++** (Gustavo, 2026-10-16:
  "quero modelos mais naturais", then "quero só em C++"). Among the
  models that run on the box's CPU in real time with a male pt-BR voice,
  Kokoro (Apache-2.0, 82M parameters, ~330 MB) is the clear quality
  winner; XTTS-v2 is non-commercial and heavy, Orpheus has no pt and
  wants a GPU, OuteTTS through llama.cpp's `llama-tts` is in-tree but
  weaker and its pt support is not guaranteed. The ggml runtime cannot
  run an ONNX graph and nobody has ported Kokoro to ggml, so the
  inference uses onnxruntime; it and espeak-ng are opened with `dlopen`
  at run time, which keeps libeasyai free of link-time dependencies and
  lets a box without them still build and run with piper. A first
  version used a Python helper over HTTP; it was replaced the same day
  by the C++ engine (the phonemizer, the part feared hard, is a few
  hundred lines once espeak-ng does the work). Rejected: vendoring
  onnxruntime sources (a multi-hour build), writing a G2P from scratch.
- **1.5 s pause** as asked; exposed as `pause_ms` because rooms differ.
- **Wake name `easyai`, said "easy AI"** (Gustavo, 2026-10-16: "renomeie
  para hey easyai"; earlier names were `easy`, then `max`). Whisper never
  writes a compound name one way: en gives "easy AI" / "easy A, I" /
  "easy-a-a-i", pt gives "Iziai" / "Isia" / "Ziai". Joining up to three
  consecutive words before the edit-distance test (and stripping spaces
  from aliases) handles the split spellings with one rule instead of a
  list of them; the odd one-word spellings stay in the alias table like
  before. Rejected: a per-language name (one INI key, one name, is what
  was asked), phonetic matching (Soundex and friends are English-only).
- **Strict call words, default "hello / olá"** (Gustavo, 2026-10-17:
  "altere o nome para Hello Max, porque ainda está ativando muito").
  The loose set (hey, oi, o, e, a, …) plus a three-letter name within
  one edit woke on everyday Portuguese ("e mais", "o máximo"); one
  required call word before the name is the cheapest filter and whisper
  hears "hello" / "olá" reliably (measured above). `call_words` stays
  configurable and an empty value keeps the old behaviour. Rejected: a
  longer name (harder to say), a confidence threshold from whisper (not
  exposed per word), a second confirmation beep before listening (slow).
- **A third sound for "processing"** (Gustavo, 2026-10-17: "um tom bem
  leve e prazeroso e moderno e rápido"). A 135 ms rising major arpeggio
  at low amplitude: three notes read as "understood, working" and
  cannot be confused with the single beep ("speak") or the falling pair
  ("done"). Rejected: reusing the beep (one cue, two meanings, the
  problem of the day before), a spoken "ok" (slower than the STT it
  precedes).
- **Internal clock in the prompt, not a tool call** (Gustavo,
  2026-10-17: "sempre injetar a data atual e hora como o relógio
  interno"). A tool round trip costs a second generation; the clock
  line costs nothing and is always right. `datetime` stays in the tool
  list for arithmetic (days until…), the prompt says not to call it for
  the time. Sources are no longer cited in answers (same request): a
  spoken answer with "according to…" is noise to the ear; the one
  sentence announcing a tool stays as asked on 2026-10-16.
- **Dashboard controls the two services through polkit** (Gustavo,
  2026-10-17: edit both INIs and start/stop/restart from the server's
  administration). The server runs as `easyai` under `NoNewPrivileges`
  and a capability bounding set, which rules out sudo; a polkit rule
  scoped to the two unit names is the designed way for an unprivileged
  user to drive systemd, and keeps the server free of root. The INI is
  one file for both services, so the editor is a text editor with a
  syntax check, not a form (a form would have to know every key of two
  programs and would hide what the operator typed). Rejected: a
  setuid helper (more code to audit than a rule), a root-run sidecar,
  sudoers (needs the unit's hardening relaxed). Since 2026-11-14 there
  is one unit: the rule, the Services tab and the editor's note cover
  `easyai-server.service` alone; the `[ASSISTANT]` section has its own
  form (rule 14) because its keys are known to the same process.
- **Whisper prompt instead of more aliases.** OpenAI's `prompt` /
  whisper.cpp `initial_prompt` is the documented way to teach whisper a
  proper noun; one sentence naming the assistant fixed the spelling of
  "easy AI" in pt and en on tiny and base without touching the
  matcher. The alias table stays for the spellings that survive it.
  Rejected: a fine-tuned wake model (training data, days), a keyword
  spotter (another model and runtime for one word).
- **Listening beep before the follow-up window** (Gustavo, 2026-10-16:
  "quando terminar a vocalização e começar a ouvir novamente adicione o
  tone de escuta"). The same 880 Hz beep that follows the name, not a
  third sound: one tone means "speak now" wherever it occurs, the
  falling two-tone means "done". Rejected: a softer or shorter variant
  (two meanings for one cue is what the user had trouble with).
- **The assistant moves into easyai-server** (Gustavo, 2026-11-14: one
  program, the assistant configured and driven from the dashboard, the
  old binary and its stale units cleaned up). The separate process bought
  nothing the server did not already have and cost a second unit, a
  second polkit entry, a second copy of the API key, a WAV round trip per
  utterance and per sentence over loopback, and a voice probe that
  waited for the server's model. In one process there is one thing to
  secure and to upgrade, the `Stt` and `Tts` already loaded for
  `/v1/audio/*` serve the microphone too (no second whisper, no second
  Kokoro in memory), the audio devices and the voices are visible to the
  dashboard without a helper, and `enabled` is an INI key the panel
  flips instead of a unit to enable. The class keeps the old loop line
  for line (rules 1–7 unchanged) so the measured behaviour of October
  stays measured. Rejected: keeping the binary as a thin client of the
  same class (two code paths to test), a D-Bus / socket control channel
  from the dashboard to a separate process (more surface than the
  problem).
- **Chat still over HTTP, speech and transcription in-process** (2026-11-14).
  The chat stays an OpenAI-shaped `POST /v1/chat/completions` through
  `easyai::Client` so `endpoint` can be llama-server or OpenAI as before
  and tools (`datetime`, `web`, `EASYAI-*.tools`) keep running in the
  assistant like in `easyai-cli`; on loopback the cost is one local
  socket per question, the same path the browser takes, exercised the
  same way. Speech and transcription have no provider to swap and pay
  for a WAV encode and decode per sentence, so they call the server's
  own `Stt` / `Tts` under the same mutexes the routes use; `stt_endpoint`
  / `tts_endpoint` keep the HTTP path for another box. Rejected: an
  in-process chat through `Engine` directly (bypasses the server's tool
  gating, preamble and auth, and loses the provider choice).
- **The old binary and units are purged by the installer** (Gustavo,
  2026-11-14: "clean the old binary and systemd stale services"). A
  stale `easyai-assistant.service` would start a program that no longer
  exists and sit `failed` in `systemctl`, and an old
  `/usr/bin/easyai-assistant` would run the October code against a
  server that no longer passes it a key. One block removes both (and
  the earlier Python helper's `easyai-tts-kokoro.service`), logged, on
  every install and `--upgrade`; one less thing to install, one less to
  upgrade. Rejected: leaving them for `--uninstall` (there is none yet)
  or for the operator (the symptom, a failed unit, points nowhere).
- **Kokoro v1.0 fp32 stays the voice** (2026-11-14, research in the
  design folder of the day). Among open models that run on the box's CPU
  in real time with a male pt-BR and an en voice, Kokoro-82M v1.0 is still
  the best (Apache-2.0, 54 voices, the pt-BR `pm_alex` / `pm_santa` /
  `pf_dora` and the graded en set); the fp16 / int8 exports save disk but
  the fp32 graph is the one measured here (rule 10a Proof) and the
  phonemes are byte-identical to the reference, so it is the one pinned.
  The upstream grades (hexgrad VOICES.md: `af_heart` A, `af_bella` A-,
  `am_michael` C+, `bm_george` C, pt-BR and es ungraded) are shown in
  the panel and in `models/tts/README.md` so an operator picking a voice
  sees what upstream measured, not a guess. piper (`pt_BR-faber`,
  `en_US-ryan`) stays as the fallback when onnxruntime or espeak-ng are
  missing; espeak-ng speaks only when an operator writes `engine = espeak`.
  Rejected: switching the default to a graded en
  voice (the box speaks pt-BR first; `pm_alex` is the natural male pt-BR
  voice and the en pick follows the same male timbre).
- **The robotic voice is never automatic** (Gustavo, 2026-11-14: "the
  start up voice was the robotized one … it must be the standard, never
  use the robotized one"). What he heard was the October standalone
  `easyai-assistant`, re-installed by a stale copy of the installer, with
  no server to ask and espeak-ng as its last fallback. espeak-ng stays
  installed because Kokoro's phonemizer is its library, but `Tts` auto
  resolution ends at piper / `say`: no engine and no fallback is ever
  espeak unless `[TTS] engine = espeak` is written by hand, the assistant
  skips its greeting and logs answers when no natural voice exists, and
  the installer re-execs the checkout's own installer when the one
  running is a stale copy (rule 16). Rejected: keeping espeak as a
  last-resort fallback (one robotic sentence undoes the impression the
  natural voice makes; silence plus a log line is the honest failure).
- **Answer language enforced in code, voice chosen per sentence**
  (Gustavo, 2026-11-15: "change the assistant part to match the language
  it is speaking, it is imperative … enforce it and make it happen"). The
  system prompt alone ("same language as the question") is a request the
  model may ignore, and the voice used to follow the question: a
  Portuguese answer to "Conte até três" (which the old word-list guess
  took for English) was read by the English voice. Now the language is
  decided before the turn and written into the prompt, the first sentence
  is checked before it is spoken and a wrong one costs one silent retry
  (~3 s on the box) instead of a wrong-language answer, and each sentence
  picks its own voice. Rejected: translating a wrong-language answer
  (another generation, drifts from what the model said), a language
  model for detection (two languages, function words and accents are
  enough and cost nothing), refusing to speak after a failed retry (an
  answer in the wrong language with the right voice beats silence).

### Proof (2026-10-16, macOS; Linux box pending Gustavo's install)

- ctest `assistant` (`tests/test_assistant.cpp`, 100 checks): `wake_match`
  (accents, one-edit misspelling, call words, name first, name inside a
  sentence rejected, question after the name, the fourteen measured
  `easyai` transcripts, joined words excluded from the question, "it was
  an easy day" / "this ai is easy" rejected, `spoken_name`, strict call
  words on the eight measured "Hello / Olá / Elo / Elomax / Allo / A lo
  Max" transcripts, the two false-positive sentences rejected, bare name
  and "hey" rejected in strict mode, an operator list and the loose
  mode, `make_think_sound` length and envelope, `apply_gain_db` +6 dB /
  −6 dB / clipping / bounds), server admin endpoints exercised by hand on
  macOS (manager `none`, 501 on an action, 400 on a foreign unit, config
  round trip with `.prev` copy and mode 660 kept, a line without `=`
  refused with its number), `SentenceChunker`
  ("3.14", "e.g.", short sentence rides to the newline, flush), VAD +
  `Segmenter` on 2 s noise → 800 ms tone → 1 s noise (one burst of
  pre-roll + tone + end silence, 38–41 speech periods, floor < −40 dB),
  `guess_language`, `make_beep` envelope, WAV encode → `decode_wav` round
  trip. All ctest targets green (5/5).
- Server on the toy model, `say` engine: `/v1/audio/speech` pt → 4.4 s
  audio in 377 ms, en → 3.4 s in 445 ms, peaks 20 450 / 26 139, played;
  415 for `mp3`, 400 for a missing input; `chat_template_kwargs.
  enable_thinking=false` yields a reply with no `reasoning_content`.
- `easyai-assistant --text` against that server: streamed answer, cut into
  sentences, each fetched from the server voice and played through
  CoreAudio. Microphone mode: CoreAudio capture at 16 kHz / 20 ms, VAD
  levels −24…−5 dB over a −49 dB floor, local tiny model transcribed the
  room's Portuguese speech.
- Found while testing: `say` leaves a pre-existing output file empty and
  appends `.caf` to an extension-less name — handled in `Tts`.
- Kokoro in C++ on this Mac (onnxruntime 1.31 dylib, brew espeak-ng
  1.52): through easyai-server `--tts-kokoro-model …`: pt-BR `pm_alex`
  137 chars → 8.4 s of audio in 1608 ms, en `am_michael` 92 chars →
  6.7 s in 1371 ms, 24 kHz, played; `/health` `tts_engine: kokoro`,
  `tts_fallback: say`. ctest `kokoro`: 16 model-free checks (npz with
  numpy's streaming local headers, batching and balancing, pauses,
  cleaning) plus, with `EASYAI_KOKORO_MODEL` / `EASYAI_KOKORO_VOICES`,
  54 voices loaded, phonemes byte-identical to python phonemizer for
  "Olá mundo, tudo bem? Vou consultar a ferramenta." and "Hello world,
  what time is it?", 1.2 s of audio for "Olá, tudo bem?", unknown voice
  refused. The earlier Python helper measured 7.0 s of pt audio in
  2038 ms for 120 chars; the C++ path is the same model at a similar
  rate (first-call warm-up included in both).

### Proof (2026-11-14, ai-pro, Ubuntu 25.10 x86_64 — the merge into easyai-server)

- Build green with `-DEASYAI_BUILD_TESTS=ON`; ctest 7 of 7, `assistant`
  now also covers the option table (unique keys and flags, defaults,
  `key_of_flag`, `set()` ranges / choices / bool spellings, `from_ini`
  precedence and empty values), `set_ini_values` (replace in place, keep
  the `=` alignment, un-comment, append before the next banner, create
  the section, header on the last line, duplicate keys, newline refused),
  `list_devices` ("default" first), the 54-voice catalogue and
  `kokoro_best_voice`, and `say()` on a stopped assistant without a voice
  (an error, not a crash); `installer` runs the 24 text checks of
  `tests/test_installer.sh`.
- `easyai-server --list-audio-devices` on the box: `default`, four
  `plughw:0,*` HDMI outputs (playback only), `plughw:1,0` ALC245 Analog
  (capture + playback) — the card the INI names. `--list-voices` with the
  installed files: `engine = kokoro`, `fallback = piper`, 54 voices all
  available, `*` on `pm_alex` / `am_michael`; the same with onnxruntime
  1.32.0 (the library the installer now pins; `LD_DEBUG=libs` showed it
  loaded) and `--assistant-say` synthesised "Olá, tudo bem? The quick
  brown fox." in 501 ms (2.3 s of audio). Playback itself could not be
  heard from the test shell (no `audio` group, the old unit held the
  card); the mic loop on the box is pending Gustavo's `--upgrade`.
- Dashboard API by hand on a second server (port 18080, no model, chat to
  the box's own server on port 80): 400 with the key for a bad integer,
  an unknown key and a foreign voice id; a save rewrites only the keys
  given (`[ASSISTANT] pause_ms`, `name_aliases`, `[TTS] kokoro_voice_en`),
  keeps `<ini>.prev`, and `apply` restarts the assistant; `tools =`
  (empty) survives a restart as "no tools"; stop / start / bad action
  (400); `ask` "Que horas são agora?" → "São onze e cinquenta e dois da
  manhã." in 2 966 ms; three `ask`s interrupted by `say`, `restart` and
  `Save & apply` ended as "cancelled" and the process stayed up (the
  in-flight guard of rule 13); `say` on a stopped assistant no longer
  opens the microphone (`configure()`). A 501-character text is refused
  as "longer than 500 characters".
- Installer: `bash -n`, `--help` prints the whole header, the INI
  migration block run with a stubbed `sudo` inserts `enabled = on` once
  after `[ASSISTANT]` (idempotent) and creates the section when absent.
- Language enforcement (2026-11-15, dashboard `ask` through the box's own
  model, Gemma 4 26B): "Conte até três." → `Q (pt)`, "Um, dois, três.",
  Portuguese voice (before this change the word-list guess said `en` and
  the English voice read it); "Que dia é hoje e que horas são?" → two
  Portuguese sentences, each with the Portuguese voice; "What day is it
  today and what time is it?" → English, English voice; "Me explique em
  uma frase o que é machine learning." → Portuguese despite the English
  term; `language = pt` + "What is the capital of France?" → "A capital
  da França é Paris."; `language = en` + "Qual é a capital da Alemanha?"
  → the model answered in Portuguese, the gate cancelled it unspoken,
  "answer came in Portuguese to a question in English — asking again",
  retry "The capital of Germany is Berlin." in the English voice (5.3 s
  for the two attempts, 2.6–5.4 s for the single ones). `detect_language`
  checks in `tests/test_assistant.cpp` (eleven sentences, three
  undecided).

### Limits

- No barge-in: saying the name while it speaks does nothing until it
  finishes. No speaker identification. VAD is energy-based: a loud TV
  produces bursts that cost a tiny-whisper run each (~0.3 s CPU).
- Languages: pt and en voices ship; whisper detects others and the
  answer is spoken with the `en` voice.

## Vulkan shader compiler — every llama.cpp shader feature (AUTHORITATIVE — 2026-11-21)

**Plain words.** Gustavo, 2026-11-21: "is there a way to increase the speed
of my GPU processing?" → "Yes, build and test" → "also make it part of the
installation process, also add what we can add on all GPU of that class".
llama.cpp's Vulkan backend compiles its shaders at build time with `glslc`.
Before that, `ggml/src/ggml-vulkan/CMakeLists.txt` compiles one tiny test
shader per optional GLSL extension (`vulkan-shaders/feature-tests/*.comp`)
and, for each one `glslc` rejects, leaves every shader of that feature out
— no error, no warning beyond a CMake status line. Ubuntu 25.10's `glslc`
(shaderc 2025.2) rejects five of the seven: `GL_EXT_integer_dot_product`,
`GL_EXT_bfloat16`, `GL_EXT_float_e4m3`, `GL_EXT_float_e2m1` and
`GL_NV_cooperative_matrix_decode_vector`. So ai-pro's server logged
`AMD Radeon 890M … int dot: 0` although RDNA 3.5 has int8 dot-product
hardware, and llama.cpp never used its int8 kernels there. Code:
`scripts/install_easyai_server.sh` (`glslc_missing_features`,
`vulkan_sdk_sha256`, the `vulkan)` case of the build step). Test:
`tests/test_installer.sh`.

### Rules

1. On a Vulkan build the installer runs llama.cpp's own feature tests of the
   pinned checkout (`$llama_dir/ggml/src/ggml-vulkan/vulkan-shaders/
   feature-tests/*.comp`) with the distro `glslc`, the same way CMake does
   (`-fshader-stage=compute --target-env=vulkan1.3`). All compile → the
   distro `glslc` is used.
2. Any test fails, `do_vulkan_sdk = 1` (default) and the machine is x86_64 →
   the installer downloads the pinned LunarG Vulkan SDK
   (`vulkan_sdk_version`, default 1.4.363.1, `--vulkan-sdk <ver>`),
   verifies its sha256 (`vulkan_sdk_sha256`; a version without a pin
   downloads with an "unverified" warning), extracts only
   `<ver>/x86_64/bin/glslc` into `$src_root/vulkan-sdk/` (10 MB; the
   366 MB archive is deleted) and re-runs the tests with it. A download or
   checksum failure falls back to the distro `glslc` with a warning — the
   build never stops for this. An already-extracted `glslc` is reused.
3. The chosen `glslc` is always passed as `-DVulkan_GLSLC_EXECUTABLE=`, so
   a path cached by an earlier configure never outlives the decision.
4. `--no-vulkan-sdk`, a non-x86_64 machine (LunarG ships no Linux SDK for
   it) or no feature-tests directory → distro `glslc`, with a warning that
   names the features left out.
5. Nothing changes at run time: llama.cpp decides per device which compiled
   paths to use (int8 dot product only when the device reports it
   accelerated; bf16 / fp8 / fp4 only when the device has them). The
   switches `GGML_VK_DISABLE_INTEGER_DOT_PRODUCT=1` and
   `GGML_VK_DISABLE_BFLOAT16=1` remain for A/B tests.
6. (2026-11-22) A build dir follows its `glslc`. The installer records the
   chosen `glslc` (path + `--version` line) in `build/.easyai-vulkan-glslc`
   (`vulkan_shader_reset`). When that differs from the recorded one, or none
   is recorded, it removes `build/_deps/llama.cpp/ggml/src/ggml-vulkan`
   before configuring, so every shader and the shader header are generated
   again by the same `glslc`. Same `glslc` → nothing removed. Reason:
   Gustavo, 2026-11-22, "install sh --upgrade is failing badly" … "the
   installer needs to handle this in the future" —
   llama.cpp regenerates `ggml-vulkan-shaders.hpp` only when the
   generator's own sources change, so ai-pro's build dir kept the header
   made on 2026-10-15 by the distro `glslc` (no int8-dot shaders) next to
   `.comp.cpp` files regenerated by the SDK `glslc`, and `ggml-vulkan`
   failed with `mul_mat_vec_q4_0_q8_1_f32_data was not declared`. The
   opposite switch (back to a `glslc` with fewer features) breaks too: the
   `vulkan-shaders-gen` sub-build keeps every `*_GLSLC_SUPPORT=ON` it was
   once given.

### Decisions

- **Reset the whole generated Vulkan directory, not single files.** The
  stale state is spread over the header, the `.comp.cpp` files, the
  `.spv` directory and the `vulkan-shaders-gen` sub-build's own cache;
  removing the directory is the one change that covers all of them, and it
  costs nothing extra: `ggml-vulkan.cpp` includes the header, so it
  recompiles anyway. A build dir with no record (every build dir before
  2026-11-22) is reset once.
- **Applies to every Vulkan GPU, not only AMD APUs.** The gap is in the
  build tool, so AMD (RDNA int8 dot), Intel (Arc/Xe int8 dot, bf16) and
  NVIDIA (coopmat2 decode, fp8) gain the same way; the device still decides.
- **LunarG SDK, glslc only.** It is the toolchain llama.cpp's own CI uses,
  it publishes a sha256 per archive, and its `glslc` is self-contained
  (needs only libstdc++). Nothing else from the SDK is kept or installed;
  the service never loads it (the SPIR-V is compiled into
  `libggml-vulkan.so`).
- **Fallback, not failure.** A box without internet still builds, as before.
- **No GPU clock pinning.** `power_dpm_force_performance_level = high` was
  not measured (needs root on ai-pro); the TDP is already set by
  `easyai-tdp` (54 W). Not added until measured.

## Installer: no Lemonade Server (AUTHORITATIVE — 2026-11-21)

**Plain words.** Gustavo, 2026-11-21, after a look at adding Lemonade's API
to easyai: "lets undo the lemonade compatibility and clean it all up if it
is not being used". The installer had installed AMD's Lemonade Server
(PPA `lemonade-team/stable`, package `lemonade-server`) by default as an
NPU sidecar for manual experiments. On ai-pro it was never used: no
engine, no model, no request since it started, and Lemonade 11 renamed its
unit to `lemond.service`, so the installer's "disable the unit" step no
longer worked and it ran all the time.

### Rules

1. The installer installs nothing from Lemonade: no PPA, no package, no
   unit handling, no summary line (`do_lemonade` is gone).
2. `--no-lemonade` and `--with-lemonade` are accepted and ignored with a
   one-line note on stderr, so existing command lines keep running.
3. An existing Lemonade install is left alone by `--upgrade`; removal is
   manual (LINUX_SERVER.md §13 "Lemonade Server").
4. easyai serves no Lemonade API. The NPU stays unused by easyai: llama.cpp
   has no AMD XDNA backend (ggml-org/llama.cpp#21725, open).

### Decisions

- **Remove, not fix.** Fixing the unit name would keep a second LLM server,
  a PPA and a broadcasting service on every box for an experiment nobody
  runs. The Lemonade API compatibility layer was scoped (≈ 40 endpoints
  under four prefixes) and dropped before any code was written: too much
  surface for no current user.
- **No automatic purge.** The installer never removes a package an operator
  may have started using; the manual steps are documented instead.

Test: `tests/test_installer.sh` (no PPA, no package install, no
`do_lemonade`, both flags accepted under `set -euo pipefail`).

## Disk-paged MoE experts — models larger than memory (AUTHORITATIVE — 2026-11-20)

Gustavo, 2026-11-20: "change the easyai code to be able to load models too
big to fit on the memory and page it from the disk … today we rely entirely
on full on GPU, I want it also work by having a page cache for hot expert
processing." Until this date every model was loaded whole into GPU memory
(`ngl = 99`, `mlock = on`, `no_mmap = on` in the installed INI), so a GGUF
larger than memory did not load. Code: `include/easyai/paging.hpp`,
`src/paging.cpp`, `Engine::Impl::plan_memory()` in `src/engine.cpp`,
`services/server.cpp`. Test: `tests/test_paging.cpp` (ctest `paging`).

Three tiers, one per kind of memory:

| Tier | Holds | Managed by |
|------|-------|------------|
| disk — the GGUF, mmap'd | every routed expert | the kernel (page faults) + `ExpertPager` read-ahead |
| RAM page cache | recently read experts, reclaimable | the kernel; `ExpertPager` decides what to read ahead |
| GPU hot-expert cache | the experts used in generation, LRU | llama.cpp `moe_cache_size` (upstream, experimental) — discrete GPUs; off by default on an iGPU (rule 4) |

### Rules

1. `memory_mode = auto | gpu | paged` — `[ENGINE] memory_mode`,
   `--memory-mode`, `Engine::memory_mode()`. Default `auto`. Aliases:
   `full`/`vram` → gpu, `disk`/`mmap` → paged; anything else is a startup
   error (exit 1).
2. `auto` reads the tensor table of the GGUF and of every split
   (`-0000k-of-0000N`) without loading it. Budget = RAM − max(4 GiB, RAM/16)
   on an iGPU / unified-memory or CPU-only machine, VRAM − max(512 MiB,
   VRAM/32) on a discrete GPU. Tensors ≤ budget → `gpu`, which is the
   historical load, byte for byte (`mlock`, `no_mmap`, `ngl` exactly as
   configured). Otherwise → `paged`. An unreadable footprint → `gpu`.
   When `auto` chose `gpu` and that load fails (the weights fit but the KV
   cache of the configured context or the compute buffers do not), the
   load is retried once as `paged` with the reason "auto: the full-GPU
   load failed … — paged" — the server never comes up model-less for a
   model that pages.
   On ai-pro (91.2 GiB RAM) the budget is 85.5 GiB, so Ornith-1.5-122B
   (≈ 75 GiB of tensors) keeps loading exactly as before.
3. `paged`, MoE model: the GGUF is mmap'd (`load_mode = mmap`, nothing
   pinned; an INI `mlock`/`no_mmap` is ignored and the reason says so);
   the routed experts (`blk.N.ffn_{gate,up,down,gate_up}_exps`, llama.cpp's
   `LLM_FFN_EXPS_REGEX`) get the CPU buffer type, i.e. zero-copy views of
   the mapping; CPU weight repacking is off (`no_extra_bufts`), because
   `CPU_REPACK` would copy every expert into anonymous RAM; llama.cpp's
   `fit_params` is off; `ngl` stays as configured, so with 99 every
   attention / shared-expert / embedding tensor and the KV cache are on the
   GPU; `n_ubatch` is raised to min(`batch`, 2048) because each prompt
   ubatch touches almost every expert and so streams the missing ones from
   disk once — fewer, larger ubatches mean fewer passes.
4. GPU hot-expert cache = llama.cpp's `moe_cache_size` (an LRU of single
   experts in device memory, used for ubatches of ≤ 32 tokens, i.e.
   generation and speculative verification). `[ENGINE] expert_cache =
   auto | off | <MiB>`, `--expert-cache`, `Engine::expert_cache_mb()`
   (-1 auto, 0 off). `auto` on unified memory (iGPU / APU, ai-pro) is
   **off**: the CPU computes the routed experts straight from the page
   cache (measured below). `auto` on a discrete GPU measures what the
   dense weights + KV + compute buffers need with a no-alloc dry run
   (`common_get_device_memory_data`) and gives the rest of the free VRAM
   minus 1 GiB to the cache, capped at the expert bytes; under 1 GiB →
   off. An explicit size is used as given on either kind of GPU. If the
   context cannot be created with the cache, the load is retried once
   without it and the reason says "GPU expert cache did not fit — off".
5. `ExpertPager` — the RAM tier. After the load it reserves one graph of
   64 tokens with every token an output (`llama_graph_reserve`, so no
   layer is trimmed to one row and none uses the cache banks), and reads
   each MoE layer's host expert tensors from its `MUL_MAT_ID` nodes
   (a node already rewritten to a device copy `"<backend>#<name>#<n>"` is
   resolved back to the leaf `<name>`). Its graph eval callback asks only
   for the `ffn_moe_topk-<layer>` nodes of those layers, reads the
   selected experts, and:
   - replays llama.cpp's LRU (same slot count per group of layers with the
     same expert layout: largest n with Σ pad(nb2·(n+1), alignment) ≤
     budget·group_share; same touch/evict order) to know which experts
     the GPU cache will upload from host memory; for a large ubatch every
     routed expert the cache does not hold;
   - probes those experts' bytes in the page cache with
     `preadv2(RWF_NOWAIT)` of one byte per sampled page of the GGUF file
     (resolved through `/proc/self/maps`), and for the ones not cached
     issues `MADV_WILLNEED` on the expert's slice — one large asynchronous
     read instead of 128 KiB page-fault read-arounds;
   - counts routed / GPU hits / RAM hits / disk reads / bytes read ahead.
   `[ENGINE] no_expert_prefetch = on` (`--no-expert-prefetch`,
   `Engine::expert_prefetch(false)`) removes the pager and its callback
   entirely (no per-layer sync).
6. `paged`, dense model: whole layers on the GPU, n = budget / (tensors /
   (n_layer + 1)) − 1, with the budget cut to ¾ on unified memory so the
   page cache can feed the rest; a smaller `ngl` from the user wins; the
   rest of the layers run on the CPU from the mapping.
7. The MTP and draft-model contexts get no expert cache and no callback;
   the draft model never inherits the target's tensor overrides.
8. A hot-swap (`Engine::reload`) re-plans from the user's settings every
   time: nothing of a previous paged load (overrides, cache, ubatch,
   callback) carries over to the next model.
9. Visible everywhere: one `[easyai] memory: mode=… (reason) weights=…
   experts=… gpu_expert_cache=… ngl=…` line at load, an `expert pager on
   N MoE layers, GPU cache mirror M expert slots` line, a `memory:` line in
   the server banner, `/health` → `memory {mode, requested, reason,
   weights_bytes, experts_bytes, ram_bytes, gpu_layers,
   gpu_expert_cache_bytes, expert_prefetch, experts {layers,
   gpu_cache_slots, ubatches, routed, gpu_hits, ram_hits, disk_reads,
   prefetch_bytes, gpu_hit_rate, ram_hit_rate, disk_rate}}`,
   `/models/api/status` → `model.memory` and `parameters.memory_mode /
   expert_cache / expert_prefetch`, `/metrics` →
   `easyai_experts_routed_total`, `easyai_experts_served_total{tier=gpu|
   ram|disk}`, `easyai_experts_prefetch_bytes_total`, and the periodic
   METRICS line ends with `experts: gpu=…% ram=…% disk=…% read_ahead=…`.
   `EASYAI_PAGING_DEBUG=1` prints every MoE matmul the pager looked at.
10. Every flag is in the INI (2026-11-21, Gustavo: "be sure all the new
    command line also appear in the ini"). `memory_mode`, `expert_cache`
    and `no_expert_prefetch` are `kFlags()` rows (`[ENGINE]`, so also valid
    in a `[MODEL_<pattern>]` profile; CLI > `[MODEL_*]` > `[ENGINE]`). They
    are written in the installer's INI template (`memory_mode = auto`,
    `expert_cache = auto`, `#no_expert_prefetch = on`), in
    `resources/easyai.ini.example` (`[ENGINE]` with comments and the
    `[MODEL_TEMPLATE]` reference) and in `easyai-server.md`. An INI written
    before this change gets them from `install_easyai_server.sh --upgrade`:
    `ini_add_engine_keys` adds each key that `[ENGINE]` lacks (commented or
    not) once, right under the `[ENGINE]` header (a new `[ENGINE]` at the end
    when there is none), at its default — behaviour unchanged — and never
    changes a value that is there; a key present only in a `[MODEL_*]`
    profile still gets its `[ENGINE]` line. ctest `ini_keys`
    (`tests/test_ini_keys.sh`) fails when any `kFlags()` row with a CLI flag
    and an INI key is missing from its section of
    `resources/easyai.ini.example` or from `easyai-server.md`.

### Decisions

- **Reuse llama.cpp's GPU expert cache; do not write a second one.** Only
  the graph builder can point a matmul at a device copy of an expert, and
  the pinned checkout (b11658) already ships `llama_moe_cache` with exactly
  that. easyai owns the decision (on/off/size) and the tier below it.
- **No GPU expert cache by default on unified memory.** Measured on
  ai-pro (Proof below): with the experts in the page cache the CPU
  generates at 14 tok/s; a 2 GiB GPU cache drops that to 8.9 and an
  8 GiB one to 9.5 with slower prompts, because every miss is a
  single-threaded copy into GTT plus a per-layer sync, and the cache's
  RAM comes out of the page cache (disk reads rose from 10 % to 19–25 %).
  On a discrete GPU the trade is the opposite (VRAM is separate and much
  faster than PCIe), so `auto` keeps the leftover VRAM there — not
  measured, there is no discrete card on ai-pro.
- **No mlock-pinned "hot set" in RAM.** On ai-pro the GPU is an iGPU: its
  cache already lives in RAM, so pinning a second copy of the same hot
  experts spends the same bytes twice. The kernel's active list keeps
  experts that are read repeatedly, and a GPU-cached expert is never read
  from host memory again, so its page-cache copy ages out by itself.
- **Mirror the LRU instead of reading ahead every routed expert.** An
  expert the GPU already holds must not be read from disk; replaying the
  same deterministic LRU tells which uploads will happen. Measured: the
  mirror's slot count equals llama.cpp's own log line (below).
- **`preadv2(RWF_NOWAIT)`, not `mincore()`.** Since Linux 5.0
  (CVE-2019-5489) `mincore()` reports every page of a file mapping as
  resident when the process may not write the file — the root-owned
  Ornith GGUFs on ai-pro, and any test user. The first build used
  `mincore()` and counted 100 % RAM hits while the disk was busy.
- **`auto` never pages a model that fits.** Paging buys nothing for a
  model that fits, and the live box must not change behaviour on upgrade.
- **The load still reads the whole file once.** llama.cpp's
  `init_mappings(true)` maps with `MAP_POPULATE`; the pinned checkout is
  not patched, so a paged load costs one sequential read of the GGUF.
- **The disk sets the speed.** ai-pro's Kingston OM8TAP4512K1 (PCIe 4 ×4)
  reads data written two days earlier at 1.3–1.4 GB/s (16 parallel
  streams: 1.39 GiB/s) but data written in October at 340–450 MB/s and
  July data at ~250 MB/s (`dd iflag=direct`, 2026-11-20) — reads of aged
  data degrade on this drive. Rewriting a GGUF (copy + rename) restores
  the fresh speed; do that before paging a large model.

### Proof (2026-11-20, ai-pro, Vulkan RADV 890M, live server running beside it)

- `ctest -R paging`: names, mode parsing, the plan on ai-pro's numbers
  (80 GiB → gpu, 150 GiB → paged, request wins, discrete 24 GiB card,
  dense split), cache sizing, slot arithmetic, the LRU against
  llama.cpp's touch/evict order, `/proc/self/maps` resolution, the
  `RWF_NOWAIT` probe seeing a `POSIX_FADV_DONTNEED` eviction, and the
  pager fed by hand on an mmap'd temp file.
- Qwen3.6-35B-A3B Q4_K_M (21.2 GB, 18.2 GiB of experts), `--memory-mode
  paged --expert-cache 2048`, `--verbose`: llama.cpp printed `Vulkan0 MoE
  cache size = 2045.27 MiB for 18600.00 MiB of host experts`, `20 layers,
  q6_K: 562 slots`, `20 layers, q4_K: 562 slots`; easyai printed `expert
  pager on 40 MoE layers, GPU cache mirror 1124 expert slots` (562 + 562).
  The answers were coherent ("A CPU cache is a small, extremely fast
  memory storage located close to the processor …").
- Run under a 10–12 GiB memory cgroup and, separately, beside the live
  server (≈ 15 GiB of page cache left): the experts paged —
  `disk_reads` 79 % on a cold first prompt, 27 % in steady generation
  with GPU hits at 51 %, 36.8 GB queued for read-ahead.
- Benchmark, same model rewritten to a fresh file (`~/models`, 1.4 GB/s
  reads), page cache evicted before each start, live server beside it
  (≈ 15 GiB of page cache left for the 18.2 GiB of experts), 8K context,
  three chat requests of ~200 tokens each on top of a 2.5K-token system
  prompt (prompt t/s is the server's cumulative average):

  | Config | Load | Prompt t/s | Gen t/s (1st → 3rd) | Experts served gpu / ram / disk (3rd) |
  |--------|------|-----------|---------------------|----------------------------------------|
  | A — GPU cache 2 GiB + read-ahead | 19 s | 73 → 79 | 8.2 → 8.9 | 55 / 29 / 16 % |
  | B — GPU cache 2 GiB, no read-ahead | 19 s | 66 → 73 | 7.5 → 8.6 | (no pager) |
  | C — no GPU cache + read-ahead | 18 s | 76 → 108 | 13.7 → 14.4 | 0 / 89 / 11 % |
  | D — GPU cache 8 GiB + read-ahead | 21 s | 48 → 58 | 7.4 → 9.5 | 80 / 2 / 19 % |
  | E — as C with 12 threads | 18 s | 83 → 101 | 13.9 → 14.2 | 0 / 91 / 9 % |

  Read-ahead: +3–10 % (A vs B). No GPU cache on the iGPU: +60 % generation
  (C vs A). The same 21 GB file in its July copy took 200–286 s to load
  (aged-data reads, above).
- Final build, defaults (`--memory-mode paged`, `expert_cache` auto) on a
  second architecture, Gemma4-26B-A4B Q4_K_M (fused `ffn_gate_up_exps`):
  `GPU expert cache off (unified memory: …)`, `expert pager on 30 MoE
  layers`, a correct answer at 10.6 tok/s from the July file with 7.9 %
  of experts from disk; `/metrics` → `easyai_experts_served_total{tier=
  "ram"} 5828`, `{tier="disk"} 1223`; `/models/api/status` →
  `parameters.memory_mode = paged`, `expert_cache = auto`. ctest 8/8.
- A model four times larger than the memory left: Nex-N2.6 IQ1_M
  (bartowski/Nex-N2.6-GGUF, `glm5-next`, 320.8 B total / ~18 B active,
  three splits, 77.3 GiB of tensors, 70.2 GiB of experts, sha256-checked
  fresh download), `--memory-mode paged` beside the live server with
  ≈ 20 GiB of RAM free: `expert pager on 42 MoE layers`; "In two
  sentences: why is the sky blue?" → a correct Rayleigh-scattering answer;
  "Name three prime numbers greater than 100." → 101, 103, 107. Prompt
  7.9–9.4 tok/s, generation 0.64–0.67 tok/s, 67–75 % of the routed
  experts from disk. With ai-pro to itself the same file is under the
  85.5 GiB budget and `auto` loads it whole.
- Rule 10 (2026-11-21): `ini_keys` found eight flags missing from
  `resources/easyai.ini.example` — the three of this section plus
  `tls_cert`, `tls_key`, `data_dir`, `catalog_size`, `reasoning_effort`
  (`frequency_penalty`, `split_mode` and the RoPE / YaRN keys were only in
  the `[MODEL_TEMPLATE]` block; `[ENGINE]` now has them too); after the
  fix 91 keys pass, and renaming `;tls_key` in the example fails the test.
  `tests/test_installer.sh` runs the real `ini_add_engine_keys` on temp
  INIs: an old `[ENGINE]` gets the three keys under its header with `ngl`
  / `mlock` and a `[MODEL_Big] memory_mode = paged` left as they were; a
  second run adds nothing and leaves the same bytes; `memory_mode = gpu` +
  `;expert_cache = 4096` gets only `no_expert_prefetch`; a file without
  `[ENGINE]` gets one. Run the way the installer runs it (`bash -c
  "$(declare -f …)"`) the file keeps its 0640 mode. `easyai-server
  --config` on the example with every new key uncommented loads without a
  warning. ctest 9/9.

### Limits

- One GPU: with several devices the mirror is not exact (llama.cpp splits
  the cache across them); read-ahead still works, a few experts may be
  read that the GPU already holds.
- Prompt processing of a model larger than memory streams the experts
  that are not cached once per ubatch; long prompts are disk-bound.
- The `/models` dashboard still scores fits for a fully resident model:
  a MoE marked too big may run paged.
- `MAP_POPULATE` at load (above). A model larger than RAM + the slow
  aged-data reads of ai-pro's NVMe make the first load several minutes.
