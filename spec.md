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
| HTTP server, SSE, web UI | `examples/server.cpp` ONLY |
| REPL, hybrid shell, signal handling | `examples/cli.cpp` ONLY |
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
| `.preamble_options(opt)` | Override per-turn `preamble::Options` (date/time, cutoff, memory_root, cite_sources). |
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
* `examples/cli.cpp` `register_tools()` — re-reads the INI, honours an
  explicit `--tools` allowlist by `ai-<name>`.
* `examples/server.cpp` — resolves from the server's own loaded
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
now the chip's job, and during the `processing` phase the chip label
itself runs a left-to-right reflection-sweep shimmer
(`@keyframes __easyaiShimmer`, `background-clip:text` gradient) so the
user gets the same "model is busy" visual on the new surface.

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

## Datetime + memory injection is tool-gated (AUTHORITATIVE — 2026-06-02)

The per-turn `preamble::build()` blocks are now ENFORCED from the live
tool registry (`ToolsetView::from_tools`) instead of flags alone, on
both server and cli:

| Block | Injected when |
|-------|---------------|
| `# AUTHORITATIVE DATE/TIME` (+ `# KNOWLEDGE CUTOFF` on server) | the `datetime` tool is registered — OR (server) the `--inject-datetime` flag/header is on. Tool presence forces it on even if the flag is off. |
| `# KNOWLEDGE LOOP` + `# KNOWLEDGE VOCABULARY` | a `knowledge_*` tool is registered AND a store is configured (`--memory`/`--RAG`). Now INDEPENDENT of the datetime toggle — turning datetime off no longer suppresses memory. |

* `examples/server.cpp` `prepare_engine_for_request`: `inject_dt =
  inject_now || view.datetime_on`; `mem_root = view.memory_on ?
  ctx.memory_root : ""`; `build_authoritative_preamble(ctx, inject_dt,
  mem_root)`. Per-request, off the engine's current tool set (so a
  client that replaces tools is judged on what it actually sent).
* `examples/cli.cpp`: injects from `ToolsetView::from_tools(cli.tools())`
  into the baked system prompt — `datetime` tool ⇒ date/time block (no
  CUTOFF; the remote model's cutoff is unknown), `knowledge_*` + `--memory`
  ⇒ memory blocks. `cite_sources=false` here (the cli prefix emits it).
  Note: against an easyai-server this double-injects the date/time block
  (cli + server); both read their own wall clock.

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
| Single-line tool name / wire_description in `tools_block` | `sanitize_for_prompt` (anonymous, src/preamble.cpp) — strips ALL C0 incl. `\n` | 64 / 200 | Structural — must stay one bullet item per tool. (§23.1) |
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

## fs_read Tool Behavior

Output always prefixes every line with `<n>| ` (line numbers on by default in both modes). Reports total line count for files ≤ 8 MiB. Description tells the model to read before `fs_edit` for accurate line references.

| Mode | Trigger | Default limit | Line numbers | Total count |
|------|---------|---------------|-------------|-------------|
| Line mode | `start_line` set | 200 lines (max 2000) | Always on | Yes |
| Byte mode | default / `offset` set | 65536 bytes (max 1 MiB) | On (default true) | Yes (≤ 8 MiB files) |

## fs Batch Mode (`action="ops"`)

The unified `fs` tool accepts a batch via `ops` — an array of single-op shapes. Lets a model land many file edits in one round trip and amortises the per-call overhead.

| Cap | Value | Why |
|---|---|---|
| Ops per call | **50** | Bound the report length and the worst-case file-system churn per turn. |
| Distinct files per call | **20** | Counted only over ops that name a `path` (`cwd`/`sandbox`/`glob`/`grep`/`list` are free). Stops a runaway batch from blasting many files at once. |
| Same-path edits reorder | descending `start_line` | Each edit's `start_line` refers to the file's ORIGINAL line numbers — model doesn't have to track line drift. |
| Read clip in batch | 2 KiB per op | Successful `read` ops are clipped to 2 KiB in the batch report; re-issue a standalone `read` for the full body. |
| Error visibility | full body | Failed ops emit their complete diagnostic so the model can self-correct without re-running. |
| `continue_on_error` | default `false` | Stop-on-first matches small-model debugging flow. |
| Report header | `batch: N ops across F files (path1, path2, …)` | Single line at the top so the model can verify it touched the right set before reading per-op statuses. |

Exposed only on the **unified** `fs` surface. Default `ToolMode` is `Split` (one focused tool per action — better small-model dispatch); opt into the ops batch with `--tools-mode unified` or `--tools-mode both`.

## ToolMode default

`easyai::cli::Toolbelt::tool_mode_` defaults to `ToolMode::Split` — one focused tool per action (`fs_read`, `fs_write`, `fs_edit`, …, `web_search`, `web_fetch`, `memory_search`, …). Small / weaker tool-callers dispatch more reliably against flat one-verb-per-tool schemas than against an `action`-discriminated union.

To pick up the unified `fs(action="ops")` batch (or the `web(action=…)` dispatcher), opt in with `.tool_mode(ToolMode::Unified)` or `--tools-mode unified`. `Both` registers both surfaces side-by-side.

## Knowledge Loop (memory + web)

Mandatory workflow when both memory and web tools are available:

1. **Memory first** — search/load relevant keywords
2. **Web second** — also search the web, even if memory had results
3. **Merge & answer** — combine both, prefer more recent/authoritative on conflict
4. **Update memory** — save durable new facts the web provided

Enforced in three places: system preamble (preamble.cpp), memory tool description (rag_tools.cpp), web tool descriptions (builtin_tools.cpp).

## Knowledge Tool Limits

| Operation | Default | Max |
|-----------|---------|-----|
| search (results per page) | 10 | 20 |
| list (entries) | 50 | 200 |
| keywords (vocabulary) | 200 | 500 |

## Entry Identity

Keywords are the sole identifier. Sorted + joined by `_` = filename stem.
Example: `"python async sockets"` → file `async_python_sockets.md`.
Files starting with `fix-` are immutable (cannot overwrite or delete).

## knowledge_append Behavior

| Entry exists? | Behavior | Return message |
|--------------|----------|----------------|
| Yes | Append content after `---` separator | `updated "key.md" (+N B → M B total)` |
| No | Create new entry | `created "key.md" (N bytes)` |
| Fixed (`fix-*`) | Error | Immutable, cannot append |

## Tools-in-prompt Contract

Three independent channels the model sees per turn — these MUST stay in sync.

| Channel | Contains | Producer | Source field |
|---|---|---|---|
| `<tools>` block in system message (Jinja template) | `name + short trigger + JSON schema` | `Engine::Pimpl::chat_tools()` / `Client::tool_to_json()` | `Tool::wire_description()` |
| Inline "Active tools" enumeration in system prompt | `name — short trigger` per tool | `easyai::preamble::tools_block(view)` | `Tool::wire_description()` |
| MCP `tools/list` response | `name + full description + inputSchema` | `easyai::mcp::tool_descriptor(t)` | `Tool::description` (full) |

Authoring rule: every tool sets both `.short_describe(...)` and `.describe(...)`. `wire_description()` falls back to the first line of `.describe(...)` if `.short_describe(...)` was omitted (pre-Shape-C tools keep working).

`tool_lookup`:
- **No arg** → INDEX view: numbered `name: short` list. Cheap, scannable.
- **`name=<substring>`** → MANUAL view: full `.describe()` body for every match. The expanded help text the model drills into when the short trigger isn't enough.

### Write/edit policy (cross-tool, enforced both in prompt AND in code)

| Tool | Disk reads | Disk writes/edits |
|---|---|---|
| `fs` (or split `fs_*`) | yes | **YES — primary** |
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

`easyai::preamble::tools_block` runs `t.name` and `t.wire_description()` through `sanitize_for_prompt(s, cap)` before emitting the active-tools bullet list. C0 control bytes (`0x00`–`0x1f`) and `DEL` (`0x7f`) collapse to a single space; UTF-8 multi-byte (`0x80+`) passes through. Caps: 64 chars (name), 200 chars (description). Closes the structural-corruption prompt-injection vector when `active_tools` is populated from a less-trusted source (e.g. cli's `/v1/tools` runtime fetch). See SECURITY_AUDIT §23.1.

### `evaluate` disk enforcement (Python 3 sandbox preamble)

The `kPythonSandboxPreamble` injected before every `evaluate` snippet enforces TWO invariants:

1. **Sandbox containment** — open() / io.open() / os.open() reject paths resolving outside the cwd (sandbox root).
2. **Read-only** — write-mode `open(...)` rejected regardless of path. Mode chars `w/a/x/+` (any case) on `builtins.open` / `io.open`; flags `O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND` on `os.open`.

PermissionError messages point the model at the filesystem write tool registered this session (the exact callable name is read from the model's AVAILABLE TOOLS list — that way the error message stays correct whether the operator chose Split mode `fs_write` or Unified mode `fs(action="write")`). Read-only opens inside the sandbox continue to work (legitimate "load CSV, compute, print result" flows are unaffected).

**Documented residual:** Python's `__closure__` introspection on `builtins.open` recovers the unwrapped open from the closure cell, bypassing both checks. Same class as the existing `ctypes` / `_io.FileIO` / `subprocess` bypasses — adversarial intent is out of scope; defense is against accident. See SECURITY_AUDIT §23.2.
