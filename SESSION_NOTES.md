# SESSION NOTES — easyai

> Context dump for resuming work in a fresh chat session.  Paste this into
> the new conversation (or point at the file URL on github.com/solariun/easy).

## 1. Project at a glance

`easyai` is a C++17 framework around `llama.cpp` that ships **two
libraries** (`find_package(easyai)` exports `easyai::engine` and
`easyai::cli`) plus six binaries:

| Artifact              | Type    | Role                                                                                                                                    |
|-----------------------|---------|-----------------------------------------------------------------------------------------------------------------------------------------|
| `libeasyai`           | library | local llama.cpp engine — `Engine`, `Tool`, `Plan`, built-in tools (datetime, unified `web` and `fs` tools, `bash`, `python3`, unified `rag`), presets. Linked via `easyai::engine`. |
| `libeasyai-cli`       | library | OpenAI-protocol client — `Client` mirrors `Engine`'s fluent API but the model runs remote and tools execute locally.  Linked via `easyai::cli`. |
| `easyai-local`        | binary  | Local-only REPL: loads a GGUF in-process via `easyai::Engine`. Drop-in `llama-cli` replacement.                                       |
| `easyai-cli`          | binary  | Agentic OpenAI-protocol client built on `libeasyai-cli` — no local model.  REPL or `-p`, full sampling control, plan tool, server-management subcommands. |
| `easyai-server`       | binary  | Drop-in `llama-server` replacement: embeds llama-server's SvelteKit webui, no MCP                                                      |
| `easyai-agent`        | binary  | Demo agent showing every built-in tool                                                                                                  |
| `easyai-chat`         | binary  | Bare REPL                                                                                                                               |
| `easyai-recipes`      | binary  | Tutorial agent paired with manual.md ch. 3.8                                                                                            |

GitHub: **https://github.com/solariun/easy** (branch `main`, tag `v0.1.0`
is the first formal release).
Sibling repo path: `develop/easyai/easyai/` next to `develop/easyai/llama.cpp/`.

## 2. User context (important — read this)

- **Owner**: Gustavo Campos (`solariun` on GitHub, `lgustavocampos@gmail.com`).
- **Communicates in Portuguese** (Brazilian).  Mix Portuguese in replies; Brazilian tech jargon is fine.
- **Hardware (the "AI box")**: MINISFORUM UM690L Slim — Ryzen 9 6900HX (8C/16T) +
  Radeon 680M iGPU (RDNA2, gfx1035) + 32 GB DDR5.  Linux + Vulkan/RADV.  GTT
  set to **28 GiB** via `ttm.pages_limit=7340032` kernel cmdline.
- **Models in use**:
  - Qwen2.5-3B-Instruct-Q4_K_M (dev / smoke testing on Mac M3)
  - Qwen3.6-35B-A3_eng_q4_k_m.gguf (production on AI box; MoE, ~22 GB on GPU)
  - Gemma4-31B Q4_K_M (alternate)
- **Preferences observed**:
  - Wants visible status / observability (status pills, tokens/s, context usage).
  - Hates flaky UI behaviour (flickering stats, panels out of place).
  - Hates relying on bundle-internal renderers — prefers our own DOM injection.
  - Wants thinking blocks rendered as **small monospace gray collapsible
    panels** above the message body (NOT inline as plain text).
  - Wants tone preset selector (`deterministic / precise / balanced / creative`)
    near the input.
  - Wants metrics (`ctx X/Y · last N tok · s · t/s`) BELOW the input, lifted
    so it doesn't overlap the textarea.
  - Wants tool calls visible as a **single-line indicator** beside the
    copy/edit/fork/delete actions (NOT a big body of text).
  - Frustrated when something is "easy and yet you didn't deliver".
- **Replaces `install_llama_server.sh`** (a 3000-line bash installer in the
  user's gist).  Keeps the same flag set (drop-in compat) but cuts MCP /
  SearXNG / proxy.  Ours: `scripts/install_easyai_server.sh`.

## 3. Repo layout

```
easyai/
├── include/easyai/{engine,tool,builtin_tools,presets,plan,client,
│                   ui,text,log,cli,backend,agent,easyai}.hpp                  # public API
├── src/{engine,tool,builtin_tools,presets,plan,client,
│        ui,log,cli,cli_client,backend,agent}.cpp                              # impl
├── examples/{local,cli,server,agent,chat,recipes}.cpp                         # binaries
├── webui/{index.html,bundle.js,bundle.css,loading.html,AI-brain.svg}          # llama-server fork
├── cmake/{xxd.cmake,easyaiConfig.cmake.in}                                    # build helpers + find_package
├── scripts/install_easyai_server.sh                                           # Linux installer
├── README.md  design.md  manual.md  SESSION_NOTES.md (this file)
└── CMakeLists.txt
```

Build: `cmake -S . -B build && cmake --build build -j` (Metal auto on macOS;
`-DGGML_VULKAN=ON` / `-DGGML_CUDA=ON` / `-DGGML_HIP=ON` for other GPUs).
Selective targets: `cmake --build build --target easyai|easyai_cli|easyai-server|easyai-cli|easyai-local|...`.

Install: `cmake --install build --prefix /usr/local` lays out
`<prefix>/lib/libeasyai{,-cli}.so.0.1.0`,
`<prefix>/include/easyai/*.hpp`, and
`<prefix>/lib/cmake/easyai/easyaiConfig.cmake` so downstream projects
do `find_package(easyai 0.1 REQUIRED)` + `target_link_libraries(myapp
PRIVATE easyai::engine easyai::cli)`.  The hand-rolled config dodges
install(EXPORT)'s issue with the `EXCLUDE_FROM_ALL` llama.cpp subdir
by creating IMPORTED targets at find_package time.

## 4. Architecture

**Engine** (`src/engine.cpp`)
- pImpl with `common_init_from_params` from llama.cpp's common library.
- `chat()` runs the agentic tool-call loop (default cap 8 hops, configurable
  via `Engine::max_tool_hops(int)` — bumped to 99999 when bash registers);
  `generate_one()` is single-pass for forwarding tool_calls to clients.
- Has setters for every tunable: `temperature/top_p/top_k/min_p/repeat_penalty/
  max_tokens/seed/batch/threads/threads_batch/cache_type_k/cache_type_v/
  no_kv_offload/kv_unified/add_kv_override/flash_attn/use_mlock/use_mmap/
  numa/enable_thinking`.
- Public `chat_params_for_current_state(bool)` returns the rendered
  `common_chat_params` so the HTTP layer can do incremental parsing.
- `perf_data()` wraps `llama_perf_context()` for tokens/s timings.
- Three-layer recovery in `parse_assistant` for malformed tool_calls:
  (1) Qwen-style doubled-brace JSON, (2) Hermes-XML `<function=NAME>/
  <parameter=K>V`, (3) markdown indicator `*🔧 NAME(args)*` (when the
  model abandons the structured format mid-conversation).  Wrench-emoji
  byte sequence (F0 9F 94 A7) is the gate so prose doesn't trip recovery.
- `extract_think_block` splits `<think>...</think>` out of raw content
  when the official parser threw, so the streaming layer's last-resort
  fallback never re-emits reasoning that was already streamed as
  `delta.reasoning_content`.

**Client** (`src/client.cpp`, libeasyai-cli)
- pImpl with raw OpenAI-shape JSON history strings — no nlohmann::json
  in the public ABI.
- HTTP/SSE via cpp-httplib; URL parser supports `http(s)://host[:port][/base]`.
  HTTPS reports a clear "not in this build" error (no OpenSSL link yet).
- Streaming SSE parser handles `\n\n` / `\r\n\r\n` terminators, ignores
  comments and our server's UI-only `easyai.tool_call`/`tool_result`
  custom events.
- Tool-call deltas accumulated by `index` into `PendingToolCall`
  (string-built `arguments` across deltas — exactly how OpenAI streams
  them).  Multi-hop loop default-bounded at 8 (`Client::max_tool_hops(int)`
  to lift), mirrors `Engine::chat_continue`.
- SSE pending buffer capped at 16 MiB; on overflow the client aborts
  with `last_error` set.  Stops a malformed/runaway stream from OOMing
  the process.
- Auto-retry-with-nudge on `timings.incomplete=true`: discards the
  bad assistant turn, appends a corrective user message
  ("don't announce, execute"), re-issues once.  Default ON in
  `easyai-cli`; opt-out with `--no-retry-on-incomplete`.
- Full sampling/penalty surface: `temperature`, `top_p`, `top_k`, `min_p`,
  `repeat_penalty`, `frequency_penalty`, `presence_penalty`, `seed`,
  `max_tokens`, `stop(vector)`, `extra_body_json` (free-form JSON merged
  last so it overrides anything the typed setters wrote).
- Direct-endpoint helpers (`list_models`, `list_remote_tools`, `health`,
  `metrics`, `props`, `set_preset`) round out the SDK so downstream apps
  can manage an easyai-server without touching curl.

**Plan** (`src/plan.cpp`, `include/easyai/plan.hpp`)
- In-memory checklist of `{id, text, status}` items + `ChangeCallback`.
- `Plan::tool()` returns a single `Tool` with `action=add|start|done|list`
  schema — wires into `Engine::add_tool` or `Client::add_tool` the same way.
- `render_string()` produces a GitHub-style markdown checklist
  (`- [ ] / [~] / [x]`).

**Authoritative datetime + cutoff hint** (`examples/server.cpp`,
`build_authoritative_preamble` + `prepare_engine_for_request`)
- Per-request fresh timestamp injected into the system prompt: current
  date+time+TZ + knowledge-cutoff rule (verify post-cutoff facts via
  tools or say "not certain").  Default ON via `--inject-datetime on`.
- Preamble is APPENDED to whichever system message reaches the model:
  if the client sent its own `system` (opencode / Claude-Code style),
  we splice the preamble into the LAST one of those; otherwise we
  append to ctx.default_system.  Either way, exactly one system block
  reaches the chat template.
- Per-request override via `X-Easyai-Inject: on|off` HTTP header.
  Defaults to the server-side flag if header absent or unrecognised.

**Server** (`examples/server.cpp`)
- cpp-httplib + nlohmann::json (vendored by llama.cpp).
- Endpoints: `GET /` (webui), `/bundle.{js,css}`, `/loading.html`, `/favicon{.ico}`,
  `/health`, `/metrics`, `/v1/models`, `POST /v1/chat/completions` (OpenAI-compat
  with `stream:true`), `POST /v1/preset`.
- Streaming pipeline now mirrors **llama-server's exact approach**:
  accumulate raw text → `common_chat_parse(text, is_partial=true)` per token
  → `common_chat_msg_diff::compute_diffs(prev, new)` → emit standard OpenAI
  deltas with `delta.reasoning_content` / `delta.content`.  Tool calls
  surfaced via custom SSE events (`event: easyai.tool_call`,
  `event: easyai.tool_result`) AND an inline single-line indicator (`*🔧 name*`).
- Per-request override: when client supplies `system` and/or `tools`, those
  override server defaults for that one call (key feature for opencode /
  claude-code / OpenAI clients).
- `--api-key` Bearer auth on `/v1/*`.

**Webui** = llama-server's compiled SvelteKit bundle at `webui/`, embedded
into the binary at build time via `cmake/xxd.cmake`.  Customisations live in
two layers:

1. **At-startup string substitution on `bundle.js`**:
   - `>llama.cpp</h1>` → `>{title}</h1>` (sidebar + welcome brand)
   - `llama.cpp - AI Chat Interface` → `{title}` (page title)
   - `Initializing connection to llama.cpp server...` → `... {title} server …`
   - `} - llama.cpp` → `} - {title}` (per-conversation page title)
   - `Type a message...` / `Type a message` → `--webui-placeholder`

2. **At-runtime injection into `<head>` of served `index.html`**:
   - Title pin via `Object.defineProperty(document, 'title', {set: ...})`.
   - LocalStorage seed: `LlamaCppWebui.mcpDefaultEnabled=false` +
     `keepStatsVisible=true`, `showMessageStats=true`.
   - DOM scrubber: MutationObserver finds elements whose visible text matches
     `MCP Servers` / `MCP Prompt` / `Sign in` / `Login` / `Authorize` /
     `Load model` / `Use Pyodide` etc and hides their container ancestors.
   - `fetch` interceptor: 501s `/authorize`, `/token`, `/register`,
     `/.well-known/*`, `/models/load`, `/cors-proxy`, `/dev/poll`,
     `/home/web_user/*`; stubs `/properties` with `{}`.
   - **Tone chip + metrics bar** (single Shadow DOM host
     `__easyaiBarHost`) at `bottom: 0.55rem`, centred — has tone selector
     (`deterministic / precise / balanced / creative` → set via fetch
     interceptor injecting temperature into request body) + ctx + last
     response stats.
   - **Per-message status chip**: appended to the action toolbar (next to
     copy/edit/fork/delete) of each `[aria-label="Assistant message with
     actions"]`; shows `thinking` / `answering` / `fetching · <tool>` during
     streaming, then `complete · 98 tok · 4.4s · 22.3 t/s`.
   - **Per-message thinking panel** (`<details class="__easyai-thinking">`):
     created on first `delta.reasoning_content`, plain text body, max-height
     18em, monospace gray, click to expand/collapse, auto-collapses on first
     `delta.content` or `finish_reason`.
   - SSE monitor inside fetch interceptor: drives the status pill, thinking
     panel, and metrics bar from the streamed events.
   - Input form gets `margin-bottom: 44px !important` injected so the
     metrics bar fits below it without overlap.

**CLI utilities** (`src/{ui,text,log,cli,cli_client}.cpp` + matching headers)
Lifted out of the example binaries during the 2026-04-27 refactor so a
third-party agent can build the same CLI experience in a handful of lines.
- `easyai::ui::Style` + `detect_style()` — ANSI colour helpers; auto-disabled
  on non-TTY stdout and when `NO_COLOR` is set.
- `easyai::ui::Spinner` — `'|/-\\'` glyph that follows the cursor; throttled
  frame advance (~10 Hz) plus a heartbeat thread that keeps the animation
  alive during dead air (slow tool calls, hidden reasoning).  Exposes a
  RAII `WriteScope` AND a one-call `write(text)` for the common path.
- `easyai::ui::StreamStats` — counters + timing that on_token/on_reason/
  on_tool callbacks update; lets the verbose summary show
  "[hop N: content=… reason=… tools=… +Tms]".
- `easyai::text::*` — `slurp_file`, `punctuate_think_tags` (newlines around
  `<think>`/`</think>` tags so they don't visually glue to the stream),
  `prompt_wants_file_write` heuristic for the missing-fs_write_file tip.
- `easyai::log::set_file(FILE*) / write(fmt, …)` — single-sink tee to
  stderr + an optional `--log-file` FILE.  `easyai-cli`'s vlog is now a
  4-line wrapper around this; libeasyai-cli ALSO writes raw SSE bytes
  to the SAME FILE via `Client::log_file(fp)` so timestamps interleave.
- `easyai::cli::Toolbelt` — fluent builder.  `.sandbox(dir).allow_bash()
  .with_plan(plan).apply(engine_or_client)`.  apply() bumps
  `max_tool_hops` to 99999 when bash is enabled.  Engine variant lives
  in libeasyai (`src/cli.cpp`); Client variant in libeasyai-cli
  (`src/cli_client.cpp`) so the engine-only library doesn't drag in
  the HTTP client.
- `easyai::cli::open_log_tee / close_log_tee` — opens
  `/tmp/<prefix>-<pid>-<epoch>.log` with a header listing argv,
  registers it as the global log sink.
- `easyai::cli::validate_sandbox(path, &err)` — uniform "exists? is a
  dir?" check.

**Backend abstraction** (`include/easyai/backend.hpp`,
`src/backend.cpp` + `src/cli_client.cpp`)
The local↔remote unification.  `easyai::Backend` is the abstract
interface (`init/chat/reset/set_system/set_sampling/info/tool_list/
tool_count/last_error`).  `easyai::LocalBackend` wraps `Engine` and
ships in libeasyai; `easyai::RemoteBackend` wraps `Client` and ships
in libeasyai-cli — so the engine-only library doesn't drag in the
HTTP client unless the consumer actually links against it.  Each has
a public `Config` struct (sandbox, allow_bash, preset, sampling
overrides, KV cache controls for local, TLS knobs for remote).

**Agent — Tier-1 façade** (`include/easyai/agent.hpp`,
`src/agent.cpp` in libeasyai-cli)
The "extremely easy for all skill levels" entry point.  Three-line
hello world: `easyai::Agent a("model.gguf"); a.ask("…");`.  Remote
variant via the static factory `easyai::Agent::remote(url, key?)`.
Fluent setters (`system / sandbox / allow_bash / preset /
remote_model / temperature / top_p / top_k / min_p / on_token`) all
queue into the underlying Backend's Config; the Backend is built
lazily on first `ask()`.  Escape hatch: `agent.backend()` returns the
materialised `Backend &` for everything Agent doesn't surface
directly.  Default toolset: datetime + web_search + web_fetch on;
fs_* and bash off until the user opts in.

**The four-tier API rule** (Gustavo's standard pattern, codified
2026-04-27):

  1. **Tier 1 — 3-line "hello world"** via `Agent` (or any future
     façade) with sensible defaults.
  2. **Tier 2 — fluent customisation** (`Toolbelt`, `Streaming`,
     `Agent` setters) chainable on the same façade.
  3. **Tier 3 — explicit composables** (`Engine`, `Client`,
     `Backend`, `Tool::builder`).  Users who outgrow Tier 1/2 step
     here without rewriting.
  4. **Tier 4 — escape hatch** (`Agent::backend()`, raw llama.cpp
     handles, raw HTTP).  Power users never hit a wall.

  Higher tiers are ALWAYS implemented on top of lower tiers — never
  parallel codepaths.  That's how Tier 1 stays trustworthy.

**Deep — default assistant persona** (server.cpp's `kBuiltinSystem`)
A fresh `easyai-server` boots up as **Deep**, an expert system
engineer who answers from CHECKED FACTS.  Operating loop:
`TIME → THINK → PLAN → EXECUTE → VERIFY`.  "Time first" is its own
rule — `datetime` is the first tool call any time the answer touches
"now", "today", a deadline, a release version, or a fact that could
have changed since cutoff.  Operators who want a different persona
pass `--system` or `-s` — Deep is the default, not hardcoded.
Webui title default also flips to `"Deep"`.

**Installer** (`scripts/install_easyai_server.sh`)
- Linux/Debian only.  Backend auto-detect: `nvidia-smi` → CUDA;
  `rocminfo` → ROCm/HIP; `vulkaninfo` / AMD `lspci` → Vulkan; else CPU.
- Installs libs to `/usr/lib/easyai/` (isolated from system) +
  `LD_LIBRARY_PATH=/usr/lib/easyai` in systemd unit (no ldconfig dependency).
- Drop-in compat: accepts every flag from `install_llama_server.sh` —
  unsupported features (MCP, draft model, webui-title-rebuild, thinking-
  budget, list-tags) become friendly no-op warnings.
- Systemd unit hardening: `OOMScoreAdjust=-700`, `CPUSchedulingPolicy=fifo`
  priority 50, `LimitMEMLOCK=infinity`, `RADV_PERFTEST=gpl`, `mlock`,
  `--no-mmap`, `flash-attn`, `q8_0` KV cache, render+video groups.
- `--upgrade` does `git fetch + git pull --ff-only` (was just fetch — bug
  fixed in commit `e03705e`).

## 5. Recent commits (most recent first)

```
2026-05-13 — install_easyai_server.sh: cap easyai-server restart at 2.
             Operator ask: "change the easyai-server service to try
             to initialize twice and stop".  Previous unit had
             Restart=on-failure + RestartSec=10 with no burst cap,
             so a broken config (missing model, bad flag, GPU not
             exposed) produced an infinite restart loop and filled
             journald.

  Patch (scripts/install_easyai_server.sh ~line 1296):
    Added to the [Unit] section:
      StartLimitBurst=2
      StartLimitIntervalSec=60

  Semantics: at most 2 starts allowed within any 60s look-back
  window.  Initial start + 1 retry on failure → if the retry also
  fails, the unit enters "failed" state instead of looping.  A
  long-running service that fails 2+ minutes after a successful
  start is NOT penalised because the burst counter resets when no
  starts happen within the interval window.

  [Service] block: kept Restart=on-failure + RestartSec=10
  unchanged; added a comment cross-referencing the new burst cap
  so future readers don't wonder why the cap isn't in the [Service]
  section (it's a systemd quirk — StartLimit* lives under [Unit]).

  Recovery flow: journalctl -u easyai-server to see what broke;
  fix; sudo systemctl reset-failed easyai-server; sudo systemctl
  start easyai-server.  Without reset-failed the start command
  would still be blocked by the rate limit.

  Existing installs: --force or --upgrade to refresh the unit.

  Verification:
    * bash -n install_easyai_server.sh — syntax OK
    * systemd-analyze verify path (manual): not run; live test
      requires a Linux box.

  Docs:
    * README.md "What's new" entry with before/after table.
    * LINUX_SERVER.md systemd-unit excerpt updated:
      RestartSec value corrected from stale "2" to actual "10",
      added a note paragraph describing the burst cap, recovery
      command, and the "long-running failures aren't penalised"
      semantics.
```

```
2026-05-13 — install_easyai_server.sh: ship only system.txt_template;
             default install uses the binary's built-in prompt.
             Two motivating asks from the operator:
             (1) "do not install the system.txt it must be created
             as the system.txt_template" — the active system.txt
             should not exist out of the box, only the template;
             (2) implicit rename to English (`_modelo` was Portuguese).

  Behaviour change:
    Before: first install dropped both /etc/easyai/system.txt_modelo
            (template) AND /etc/easyai/system.txt (active, mode 640).
            --force or first-install path rewrote system.txt.
    Now:    installer drops ONLY /etc/easyai/system.txt_template
            (mode 644).  system.txt is NOT created at install time;
            the binary's built-in "Deep" prompt (already gated on
            actually-registered tools) is what the server uses.
            Operator activates a custom persona by:
              sudo cp system.txt_template system.txt
              sudoedit system.txt
              # uncomment SERVER.system_file in easyai.ini
              sudo systemctl restart easyai-server

  Code (scripts/install_easyai_server.sh):
    * system_template_file: $config_dir/system.txt_modelo
                          → $config_dir/system.txt_template
    * Heredoc tag MODELO → TEMPLATE; header comment block above
      the template body rewritten (drop "DORMANT" framing; explain
      the cp-to-activate workflow).
    * Drop the entire `if [[ ! -f $system_file || $do_force ]] …
      cat > $system_file <<SYS … SYS` block.  Replaced with a
      one-line "preserving existing $system_file" log when one
      happens to already exist (legacy install).
    * easyai.ini hint block above `# system_file = $system_file`
      rewritten to describe the cp+uncomment activation flow.
    * Final printout: "system :" row split into two lines —
      template first ("TEMPLATE; refreshed every --upgrade"),
      then system.txt with a conditional message ("active custom
      prompt — uncomment SERVER.system_file" when it exists, or
      "NOT created; built-in Deep prompt is in use" when it doesn't).
    * Header banner comment (line 14) + --force / do_force comment
      blocks updated to drop the "AND system.txt" reference.

  Upgrade safety: existing installs that already have
  /etc/easyai/system.txt KEEP it across --upgrade and --force runs.
  The installer only stopped *creating* system.txt; it doesn't
  delete one if it's there.  Operators who customised the prompt
  before this change see no behavioural diff.

  Verification:
    * bash -n install_easyai_server.sh — syntax OK
    * grep system.txt_modelo / MODELO — zero matches (clean rename)

  Docs:
    * README.md "What's new" entry with before/after table.
    * LINUX_SERVER.md §6 file-layout table updated (template now
      listed alongside the operator-supplied system.txt row, both
      with explicit mode + "NOT installed by default" note).
    * LINUX_SERVER.md §6.X "/etc/easyai/system.txt (operator-
      supplied) + system.txt_template" rewritten to lead with the
      activation workflow.
    * LINUX_SERVER.md §12 (Upgrading) updated: --upgrade WILL
      refresh system.txt_template; will NOT touch system.txt.
```

```
2026-05-12 — install_easyai_server.sh: ttm.pages_limit updated in place
             on re-run (was: "already present; skipping" left stale
             value behind).
             Operator-reported: re-running with a different --gtt
             previously printed "ttm.pages_limit already present;
             skipping" and silently left the prior page count in
             /etc/default/grub, so the next reboot kept the stale
             GTT.

  Patch: the existing token comparison was a bare grep -q presence
  check.  Now it scrapes the current page count out of GRUB_CMDLINE_
  LINUX_DEFAULT, compares against the target, rewrites in place via
  sed -i when they differ, and runs update-grub so /boot/grub/grub.cfg
  picks the new value up.  Reboot reminder also points at
  /proc/cmdline so operators can verify after restart.

  No flag change.  Same --gtt value on every run still hits the
  "already present; skipping" path (true no-op).

  Commit: ab8a99d
```

```
2026-05-12 — Brand: AI Box logo aura softened (loud → quiet).
             Earlier tuning (07c2347) introduced a two-layer cyan
             aura around the mark; operator feedback was "vamos
             diminuir a aura do logo por favor para ser subita"
             (subtler).  Tuned both stacked Gaussian blurs down:

    Outer pass: stdDeviation 14→10, flood-opacity 0.5→0.3
    Inner pass: stdDeviation 4→3,   flood-opacity 1.0→0.6

  Gradient (#1de9b6 → #2196f3), mark geometry (50/100/200 ring +
  rounded 62×62 center), viewBox (-50 -50 300 300 with 60% filter
  headroom) and flood color (#00bcd4) all unchanged.

  Wiring: webui/AI-brain.svg (canonical) + inline kBrandSvg in
  examples/server.cpp updated in lockstep so the favicon route
  serves the same softened version downstream embedders see.

  Docs: README.md "What's new" entry with before/after table.

  Commit: cc92d51
```

```
2026-05-12 — Cli: checkpoint .easyai_session after every tool dispatch
             (survives force-exit).
             Operator-reported: a long agentic turn that got
             force-exited (Ctrl-C 3x → stage 3 → _exit(130)) left no
             .easyai_session behind.  Previous save points covered
             every interruption mode EXCEPT force-exit:

    Stage 1 (graceful, Ctrl-C once during turn) — chat() finishes
      naturally, save fires from the post-chat() path in run_one
    Stage 2 (cancel, Ctrl-C twice) — request_cancel() flag set,
      chat() returns with last_error="cancelled", save fires
    Stage 3 (force-exit, Ctrl-C three times) — _exit(130) from the
      signal handler skips atexit AND skips run_one's post-chat()
      path → save NEVER fires

  Adding a save after every tool round-trip means a partial turn that
  gets force-exited still leaves the state up to the last completed
  tool on disk.  Only the in-flight partial reply is lost.

  Wiring: run_one() already calls streaming.attach(cli) which sets a
  single on_tool callback driving the canonical UI (tool indicators,
  dim styling, etc.).  cli.on_tool() replaces (not composes), so we
  expose a public forwarder on Streaming and then register a wrapped
  callback that calls both:

    include/easyai/ui.hpp
      + Streaming::notify_tool(const ToolCall &, const ToolResult &)
        Public alias for the private on_tool_ UI handler, so external
        callers can compose extra behaviour onto the on_tool slot
        without losing the streaming output.

    src/ui.cpp
      + void Streaming::notify_tool(...) { on_tool_(...); }
        One-line forwarder.

    examples/cli.cpp run_one()
      The post-streaming.attach(cli) wrapper:
        cli.on_tool([&](const ToolCall & c, const ToolResult & r) {
            streaming.notify_tool(c, r);   // canonical UI
            std::string save_err;
            if (!save_session(cli, &save_err)) {
                std::printf("%s[easyai-cli-remote] warning: failed "
                            "to save .easyai_session: %s%s\n",
                            st.yellow(), save_err.c_str(), st.reset());
            }
        });

  Docs: README.md "What's new" entry.  easyai-cli.md §10 new
  "Save cadence (force-exit survival)" subsection enumerating the
  three layers (per-tool / per-chat / per-slash-command) and which
  signal-handler stage each one covers.

  No public-API change other than the new notify_tool forwarder.
  Library embedders not using Streaming::attach() are unaffected.

  Commit: 3b95ef4
```

```
2026-05-12 — Brand: AI Box logo green → blue gradient + two-layer cyan aura.
             Replaced the prior orange→teal gradient with a cleaner
             mint→Material-blue arc and added a quiet two-pass
             cyan aura around the mark to give it presence.

    Gradient: #1de9b6 (top-left) → #2196f3 (bottom-right)
    Aura: feGaussianBlur stacked (outer + inner) with #00bcd4
          flood; merged behind SourceGraphic.

  Geometry: outer rounded-square ring (50 → 150, r=50, evenodd) +
  center mark (rect 69×69→62 wide rx=13).  ViewBox bumped to
  -50 -50 300 300 with filter x=-60% width=220% so the halo has
  headroom and doesn't get clipped at the SVG edge.

  Same SVG content in webui/AI-brain.svg (canonical) and inline
  kBrandSvg in examples/server.cpp (compiled into the binary,
  served at /favicon route).

  Subsequent tuning (cc92d51) softened the aura — see the aura
  softening entry above for the final intensity numbers.

  Commit: 07c2347
```

```
2026-05-12 — Cli: session resume default-ON + every session knob in [cli] INI.
             Iteration on the persistence feature that landed earlier
             today (99a9efc).  Two motivating asks from the operator:
             (1) "auto-load .easyai_session if present" — flip the
             default so you don't need --continue to pick up where you
             left off; (2) "expose every command we discussed in the
             INI + documentation" — surface session knobs as [cli]
             keys.

  Semantics flip:
    Before: default = start fresh; --continue = resume.
    Now:    default = resume if .easyai_session exists, fresh if not;
            --no-continue = start fresh anyway.
    --continue is kept as a no-op alias (useful in scripts that
    assert resume semantics against an operator's INI that may have
    flipped auto_continue off).
    --compress + --no-continue now warns instead of erroring (the
    auto_continue=off + auto_compress=on case from INI).

  Options struct (examples/cli.cpp):
    * Renamed: continue_session -> auto_continue (default true)
    * Renamed: compress_session -> auto_compress (default false)
    * Added: auto_continue_cli_set, auto_compress_cli_set,
             log_file_path_cli_set — to track CLI override vs INI
             (CLI > INI > hardcoded precedence)
    * Added: auto_log (default false) — controls whether the
             library's auto-/tmp log is allowed (default suppressed
             via EASYAI_NO_AUTO_LOG=1)

  Flag parsing:
    * --continue: still works (sets auto_continue=true + cli_set)
    * --no-continue: new flag (sets auto_continue=false + cli_set)
    * --compress: sets auto_compress=true + cli_set
    * --log-file PATH: also marks cli_set so INI log_file doesn't
                      override

  INI overlay (parse_args end):
    * Generalised load_show_flag -> load_bool_flag
    * Added load_str_flag for log_file
    * Loads: auto_continue, auto_compress, auto_log, log_file
             (plus existing show_bash, show_python)

  Dispatch (main):
    * Dropped --compress-requires-continue hard-error
    * Replaced explicit "no session to resume" warn with silent
      fresh-start (a missing file is the natural first-run case)
    * auto_log=on path leaves EASYAI_NO_AUTO_LOG unset so the
      library's auto-/tmp log returns

  Help text:
    * --continue: rewritten to explain it's a no-op default
    * --no-continue: new entry
    * --compress: drop "requires --continue", mention auto_compress INI
    * --log-file: mention log_file INI
    * --config: full table of [cli] keys with defaults

  Docs:
    * easyai-cli.md §10 rewritten: default-ON semantics, new
      4-row control-points table, full INI-mapping table with
      defaults + CLI flag cross-refs, example easyai-cli.ini.
    * Flag table (§4) rewritten for --continue / --no-continue /
      --compress with INI references inline.
    * README.md "What's new" entry above the previous session
      feature entry, with before/after table and the INI block.

  Verification (build-macos):
    * Build green
    * --help shows --continue / --no-continue / --compress with
      INI cross-refs + a [cli] table on --config
    * --no-continue parses cleanly
    * Backward compat: --continue still works (no-op)

  No public-API change to libeasyai-cli (Client::dump_history /
  load_history unchanged).  Operators who pinned [cli] sections
  for show_bash / show_python keep them; new keys default to the
  previous behaviour.
```

```
2026-05-12 — easyai-cli session persistence + raw log default OFF.
             New feature on easyai-cli (the OpenAI-protocol client
             binary, not the local-engine `easyai-local`).  Adds
             per-process conversation persistence and silences the
             previously-default /tmp raw-log files.

  Behaviour:
    easyai-cli                       fresh history, .easyai_session
                                     auto-saved in cwd after each turn
    easyai-cli --continue            resume the .easyai_session from
                                     cwd (warn + start fresh if none)
    easyai-cli --continue --compress resume AND ask the model for a
                                     lossless recap before the first
                                     prompt; recap replaces history
                                     and gets saved as the new ctx
    /compress (mid-REPL)             same recap flow on demand

  Code (libeasyai-cli):
    * include/easyai/client.hpp: new Client::dump_history() /
      Client::load_history(json_array, err?).  Serialise the in-memory
      history (OpenAI message-shape array, no system prompt) to a
      JSON string for save-to-disk, and replace it from a JSON string
      on load.  Validates each message has a string "role" field.
    * src/client.cpp: implementations.  Works on the existing
      history_json vector<string> the streaming loop already
      populates — no separate serialisation path to maintain.

  Code (examples/cli.cpp):
    * session_file_path() resolves to <cwd>/.easyai_session.
    * save_session() atomic write (tempfile + rename, O_NOFOLLOW,
      mode 0600) — called after every cli.chat() return in
      run_one() and after every history-mutating slash command.
    * load_session() reads and feeds Client::load_history().
    * do_compress() runs the recap turn (single chat() call with a
      "summarise this losslessly + don't call any tool" prompt),
      then replaces history with a synthetic user/assistant pair
      carrying the recap.  Restores original history on failure.
    * --continue / --compress flags wired with validation
      (--compress without --continue rejected at parse time).
    * /compress slash command added in the REPL loop, next to
      /clear and /reset.  History-mutating slash commands now save
      .easyai_session so the post-command state survives a later
      --continue.
    * REPL banner + /help listing updated to mention the new flags
      and /compress.
    * Top-of-file comment block updated with the new specials.

  Code (raw log default flipped to OFF):
    * examples/cli.cpp: the binary's open_log_tee() only fires when
      --log-file PATH is given.  Previous --verbose-implies-auto-/tmp
      behaviour removed — operators got a stale .log per session
      whether they wanted one or not.
    * examples/cli.cpp main(): EASYAI_NO_AUTO_LOG=1 is set by default
      (only if the env var is not already set, so operator override
      still wins) — suppresses the library-side auto-open in
      src/log.cpp::auto_open that was firing on every Client
      construction with its own /tmp/easyai-client-<pid>-<epoch>.log.

  Verification (build-macos):
    --compress without --continue  -> "error: --compress requires
                                       --continue", exits 2
    --continue with no session     -> warns + starts fresh
    --help                         -> all three new entries shown
    default invocation             -> 0 /tmp log files created
    --log-file /tmp/x.log          -> still works (mode 0600)

  Docs: README.md "What's new" entry. easyai-cli.md new §10 "Session
  persistence", §9 "Raw transaction log" rewritten as opt-in, flag
  table rows for --continue / --compress / updated --verbose /
  --log-file, TOC renumbered.  Cross-refs in the file fixed up to
  the new section numbers.

  Tag: v0.5.5 cut at HEAD~1 (commit d6bb546) as a pre-feature
  checkpoint, in case the new persistence path regresses anything.

  Commit: 99a9efc
```

```
2026-05-12 — fs: friendlier dispatch in read / list / grep
             (file-vs-dir hints + optional path on list).
             Three small UX fixes to the unified fs() tool, motivated
             by a model that called fs(action="grep",
             path="adelide_bitnet.c", ...) and got "not a directory"
             — a legit input treated as an error.  Same class of
             "do what the model wants" issue showed up in two
             adjacent actions when I scanned the rest of the surface.

    fs.grep: dispatches on is_regular_file vs is_directory at the
             top (mirrors `grep -r`).  Per-file scan factored into a
             shared lambda so matching, size cap, line cap, and
             output formatting are identical on both paths.
             file_glob acts as a name-guard when path is a single
             file (so path="foo.c" file_glob="*.py" returns
             "No matches" instead of grepping foo.c with a Python
             filter).
    fs.read: pre-empts the cryptic "read failed: Is a directory"
             errno with an fstat-after-open check.  Returns
             "path is a directory: X — use action=\"list\" to
             enumerate entries, or action=\"glob\"/\"grep\" for
             recursive search."
    fs.list: path is now OPTIONAL — empty/missing defaults to "."
             matching glob/grep convention.  When path points at a
             regular file, friendly redirect to action="read".

  Smoke matrix: 9-case suite (file/dir/empty path × match/no-match
  × glob-miss/missing) — ALL PASS.

  Commit: d6bb546
```

```
2026-05-11 — fs(action="edit") seam-line corruption fix (HIGH; post-publish correction to §22.4).
             User-reported bug: a model invoking fs.edit with content
             that lacked a trailing \n had the last byte of content
             glued onto the first preserved line, producing silent
             file corruption.  Most common failure shape: replacing
             one line in a C source caused the `}` of an enclosing
             function to be consumed → "function definition not
             allowed here" + "expected '}'" on next compile.

  Code (builtin_tools.cpp make_fs_edit_handler):
    * Two-sided auto-separator: insert a '\n' before content if the
      prefix is non-empty + doesn't end with '\n' + content is
      non-empty (covers append-at-EOF after a no-newline file).
      Insert a '\n' after content if content is non-empty + doesn't
      end with '\n' + there's a preserved tail (covers the user-
      reported bug shape).
    * Both guards no-op when the contract is already satisfied
      (content with trailing \n, pure delete content="", append-at-
      EOF after a \n-terminated file).
    * Tool description updated: dropped the "include trailing \n
      yourself" advice — line semantics now preserved automatically.

  Verification: 9-case smoke matrix in /tmp/fsedit_full_test.cpp
  (since cleaned up) exercises every boundary shape — middle-replace
  with/without \n, multi-line content lacking \n, pure delete, pure
  insert, append-at-EOF on files with and without \n, replace-last-
  line on a file without \n, whole-file replacement.  ALL PASS post-
  fix; the original bug case now produces what the model intended.

  Docs: SECURITY_AUDIT.md §22.8 (POST-PUBLISH CORRECTION) +
  amended §22.4 title with forward-pointer + section index updated.
  README.md "What's new" entry above the 7th-pass entry.

  Auditor's note: §22.4's "audited at intro, no findings" claim
  reviewed the sandbox containment + O_NOFOLLOW + atomic-write
  posture, which IS correct.  The line-level *semantic* contract
  wasn't separately exercised against a seam case.  A behavioural
  smoke test (handful of fs.edit calls against known-shape inputs,
  diff the output) would have caught this at §22.4 time.  Adding
  behavioural smoke for new tool surfaces is the follow-up TODO.
```

```
2026-05-11 — Brand asset: AI Box logo inlined as constexpr in server.cpp.
             Replaced webui/AI-brain.svg's xxd build-step with an
             inline constexpr std::string_view kBrandSvg in
             examples/server.cpp.  Removed the brand-specific
             add_custom_command from CMakeLists.txt.  Canonical
             copy stays at webui/AI-brain.svg for external rebrand /
             docs use.  Favicon route's #if EASYAI_BUILD_WEBUI guard
             dropped (the SVG is now in source, so no-webui builds
             can serve a favicon too instead of 204).  New AI Box
             gradient (#ffb547 → #00d4a8) replaces the prior
             AI-brain mark.
```

```
2026-05-11 — Security audit 7th pass: 1 HIGH, 1 MEDIUM, 1 LOW.
             Re-applied the standing audit on the ~5,000 LoC added
             since the 6th pass (2026-05-08). Three findings, all
             closed in this commit. No public-interface change.

  HIGH — run_capped_subprocess banner sanitization (builtin_tools.cpp).
    The `[bash] $ ...` / `[python3] $ ...` opening banner used to
    print the model-supplied command/code through fprintf verbatim,
    so a snippet that embedded an ANSI/OSC sequence could repaint
    the operator's terminal (window title, screen wipe, OSC 52
    clipboard write) one line before any child output arrived. The
    live mirror was already hardened in §20.1; the banner is now
    sanitized the same way. For python3 the banner now shows the
    user's `code` only — the 25-line sandbox preamble was previously
    included in the body_arg displayed on the banner.

  MEDIUM — python3 sandbox preamble closure tightening (builtin_tools.cpp).
    The preamble wrapping open() left _e_open_orig / _e_chk / _e_root
    at module scope, so user code could trivially call _e_open_orig
    by name and bypass the check — the comment claimed "closure cell"
    protection the implementation didn't provide. Restructured into
    an `_e_make_wrappers` factory whose locals become real lexical
    closure cells; module scope post-preamble carries only the
    `_e_os` / `_e_b` / `_e_io` module imports. The "ctypes /
    subprocess / _io.FileIO" bypass class remains documented as
    out-of-scope.

  LOW — installer INI-shape validation widened (install_easyai_server.sh).
    Extended require_numeric to --service-port, --threads,
    --threads-batch, --ngl. Added new `require_no_injection` helper
    (rejects \n, \r, =, [, ]) for the non-numeric knobs
    (--service-host, --alias, --webui-title, --cache-type-k,
    --cache-type-v). Same operator-typo / hostile-CI threat model
    as §20.4.

  Audit-cleared this pass: easyai.prompt_progress SSE (numeric-only
  payload, clamped int suffix), CLI thinking label (hardcoded color
  + word), fs.ops batch reordering (numeric comparison + map key
  equality), plan 80-char cap, metrics-always-on (§21.6 carry).

  Build: green on macOS Metal (build-macos/) — easyai, easyai_cli,
  easyai-cli, easyai-server, easyai-mcp-server, easyai-chat,
  easyai-local, easyai-agent, easyai-recipes all link clean.
  Python preamble smoke-tested (5 probes pass: in-sandbox write OK,
  /etc/passwd rejected, _e_open_orig / _e_chk / _e_root all
  NameError as expected).

  Doc updates: SECURITY_AUDIT.md §22 narrative + TL;DR + section
  index. README.md "What's new" entry. This SESSION_NOTES line.
```

```
2026-05-10 — CLI thinking label: static dark gray (no shimmer sweep).
             Replaced the 10 Hz spotlight-sweep animation with a
             single 256-colour grayscale 244 (mid-gray RGB 128/128/128)
             that paints the "thinking[ N%]" label once and only
             repaints when set_thinking_pct() fires from the server's
             easyai.prompt_progress SSE event. Heartbeat drops to one
             cadence (250 ms idle) and skips its repaint while
             thinking_ is on.

  Code: ui.cpp::draw_thinking_locked_ + heartbeat_loop_ +
        set_thinking(). ui.hpp drops shimmer_phase_ field and
        kThinkingIntervalMs constant.
```

```
2026-05-09 — python3 tool result rendered with the executed snippet.
             Tool result now opens with a fenced ```python ...```
             block carrying the snippet that just ran, then a
             `[python3 executed]` notification line, then the exit
             code and captured output. Chat UIs that render markdown
             (webui, typical clients) display the code with syntax
             highlighting; operators skimming the transcript see
             what ran without expanding the raw tool-call JSON.

  Code:
    * builtin_tools.cpp python3 handler: post-process the
      ToolResult from run_capped_subprocess() — prepend
      ```python\n<code>\n```\n[python3 executed]\n to the
      content. Spawn-side errors (pipe / fork) still surface
      unwrapped (no misleading "executed" notice).
    * The kPythonSandboxPreamble is intentionally stripped from
      the rendered block — only the model's actual `code`
      argument shows up. Avoids 25 lines of preamble clutter
      on every call.
    * Description's OUTPUT section updated to document the
      result format so the model knows it doesn't need to add
      its own markdown wrapping.

  Smoke-tested 4/4: oneliner, multiline (preserves indentation),
  denied-disk (preamble still triggers, error appears below the
  code block), missing-arg (returns unwrapped error, no exec).
```

```
2026-05-09 — easyai-server METRICS line: always on, default 5 min.
             The periodic METRICS log line was previously gated on
             --verbose. Operators told us they need this telemetry
             (CPU / mem / GPU / TCP-state / TIME_WAIT pressure) in
             journalctl regardless of debug noise level. Lifted the
             gate; bumped the default interval from 1s to 300s so
             the line is low-overhead enough to leave on
             permanently.

  Code:
    * server.cpp: metrics_thread now starts whenever
      args.metrics_interval > 0 (was: && args.verbose).
    * ServerArgs::metrics_interval default 1 → 300.
    * Help text + banner: METRICS line described as "always on";
      verbose-only logging now mentions only the per-request
      → / ← arrival/completion lines.
    * Periodic-metrics-sampler header comment retitled "always on"
      (was "verbose-mode-only").

  Installer:
    * scripts/install_easyai_server.sh easyai.ini template:
      metrics_interval bumped 60 → 300, comment updated to
      "ALWAYS ON regardless of `verbose`".

  Docs:
    * README.md: new changelog entry; legacy entry retitled "TCP
      state breakdown" (verbose-mode caveat dropped).
    * easyai-server.md: INI table + §9.2 (Periodic METRICS line)
      updated to "always on" and default 300.

  Existing operators who pinned [SERVER] metrics_interval in
  their INI keep their value — only the unspecified default
  shifts.
```

```
2026-05-09 — `python3` default-on + sandbox-rooted disk surface.
             Promotion from explicit-opt-in to "auto-on whenever the
             operator signals files-are-OK" (i.e. --sandbox or
             --allow-bash). The webui inherits it for free since the
             systemd unit always sets --sandbox.

  Behaviour shift:
    * Toolbelt::allow_python_ defaults TRUE (was FALSE).
    * cfg.allow_python defaults TRUE.
    * Toolbelt::tools() registers python3 when allow_python_ AND
      (sandbox-or-bash). Same gate as `fs`.
    * --allow-python flag REMOVED. --no-python opts out
      (mirroring --no-web / --no-datetime). [SERVER] allow_python
      defaults on; set to off in the INI for the same effect.

  Disk surface restriction (defense-in-depth):
    * Every snippet auto-prefixed with kPythonSandboxPreamble — a
      ~25-line Python preamble that monkey-patches builtins.open,
      io.open, os.open to reject paths whose realpath is outside
      the cwd Python was chdir'd into.
    * open("/etc/passwd"), open("../escape"), os.open("/etc/hosts"),
      pathlib.Path("/etc/hostname").read_text() all raise
      PermissionError with a descriptive message that points the
      model to fs(action=...).
    * NOT a hardened sandbox — import ctypes; CDLL("libc.so.6").open
      escapes; subprocess.run / os.system escape. Same threat model
      as bash, hence kept gated on operator opt-in.

  Description rewrite:
    * USE FOR: testing, calculation, data processing, networking,
      information gathering. With concrete examples (Decimal math,
      urllib HTTP fetch, date arithmetic, regex over text).
    * NEVER USE FOR DISK — every disk operation has a
      fs(action=...) equivalent listed inline.

  Smoke tests passed (10/10):
    sandbox_read_ok, etc_passwd_blocked, dotdot_blocked,
    os_open_blocked, pathlib_blocked (caught through pathlib.py's
    internal open()), compute_ok, network_ok (gethostbyname),
    sandbox_write_ok, stdout_ok (fd-int passthrough), sandbox_subdir_ok.

  Docs updated: README changelog + flag tables, easyai-server.md
  + easyai-cli.md + easyai-mcp-server.md INI/flag tables.
```

```
2026-05-09 — `python3` tool added.
             Second shell-class executor alongside `bash`. Runs snippets
             via `python3 -I -S -E -c <code>` (isolated mode: no
             PYTHON* env, no site-packages, no cwd on sys.path —
             stdlib only). Same hardening as bash (cwd pinned,
             fds 3+ closed, SIGTERM/SIGKILL deadline, 32 KB output
             cap, optional operator-facing live mirror via
             --no-show-python).

  Plumbing:
    * easyai::tools::python3(root, show_output) factory.
    * Toolbelt::allow_python(bool) + show_python(bool) methods.
    * cli::Toolbelt auto-registers `fs` whenever any subprocess
      executor (allow_bash OR allow_python) is on; bumps
      max_tool_hops to 99999.
    * cfg.allow_python on easyai::LocalBackend::Config.
    * --allow-python flag wired into all four binaries
      (cli, local, mcp_server, server) plus [SERVER] allow_python
      INI key.
    * `python3` added to kBuiltInNames reserved list — manifests
      cannot shadow it.

  Internal cleanup:
    * Extracted run_capped_subprocess() helper inside
      builtin_tools.cpp's anonymous namespace; bash() and python3()
      both delegate to it. The fork/fd-close/chdir/drain/wait
      machinery now lives in one place. CappedExecKind enum
      selects exec(/bin/sh -c) vs execvp(python3 -I -S -E -c) in
      the child; the rest is shared.

  Smoke tests passed:
    print(2+2), import json, isolated check (sys.path[0] is the
    stdlib zip not cwd), stderr capture, raise SystemExit(2),
    missing arg, timeout, third-party import correctly fails with
    ModuleNotFoundError.

  Docs: README.md changelog + flag tables, easyai-server.md INI
  + flag tables, easyai-cli.md flag tables, easyai-mcp-server.md
  flag table + tool catalogue, EXTERNAL_TOOLS.md reserved-name
  list.
```

```
2026-05-09 — Tool surface unification: one tool per concept.
             Three loose collections (web, filesystem, rag) collapsed
             to one Tool each, all shaped the same way: single Tool
             with an `action` parameter and a flat schema (every
             parameter optional except `action`). Pattern mirrors the
             rag dispatcher introduced 2026-05-04.

  Web:
    * `web(action="search"|"fetch")` replaces web_search /
      web_fetch / web_google.
    * action=search: engine="ddg" (default, no key) or "google"
      (Custom Search; opt-in via --use-google + GOOGLE_API_KEY +
      GOOGLE_CSE_ID env vars). Page-based pagination over the
      engine's own ordering — page= header in the response with
      total_entries / has_more so the model can walk forward.
    * action=fetch: start (byte offset) + limit (window size,
      default 8 KB, max 64 KB) + as_html. Same in-process LRU
      cache as the old web_fetch (16 entries, 5-minute TTL).

  Filesystem:
    * `fs(action="read"|"write"|"list"|"glob"|"grep"|"check_path"
                |"cwd"|"sandbox")` replaces fs_read_file /
      fs_write_file / fs_list_dir / fs_glob / fs_grep /
      fs_check_path / get_current_dir / get_sandbox_path. Eight
      sub-actions, one factory: easyai::tools::fs(root).
    * Sandbox containment, O_NOFOLLOW + post-mkdir TOCTOU defenses,
      lstat + access() probing in check_path — all unchanged from
      the legacy split implementations.

  RAG:
    * `--split-rag` flag and the legacy seven rag_* tools removed
      everywhere — CLI, INI, examples, all four binaries. The single
      `rag(action=...)` dispatcher (default since 2026-05-04) is
      the only layout. On-disk format unchanged.
    * make_unified_rag_tool() renamed to make_rag_tool().
    * make_rag_tools() factory and the RagTools struct deleted from
      the public API.

  Library API (BREAKING — direct libeasyai consumers must migrate):
    * Removed: easyai::tools::web_search(), web_fetch(), web_google(),
      fs_read_file(), fs_write_file(), fs_list_dir(), fs_glob(),
      fs_grep(), fs_check_path(), get_current_dir(),
      get_sandbox_path(), make_rag_tools(), RagTools struct.
    * Added: easyai::tools::web(google_enabled), fs(root),
      make_rag_tool(root).
    * cfg.split_rag dropped from easyai::Config (LocalBackend).

  Toolbelt:
    * .allow_fs(), .no_web(), .use_google() flags retained but their
      meaning shifted — register the unified tool instead of multiple,
      and (for use_google) toggle whether engine="google" is accepted
      at call time. Env vars still re-read every call so a key
      rotation surfaces an actionable error rather than silent
      disappearance.

  Built-in description prose:
    * Both new dispatchers' descriptions follow the same per-action
      block / "USE THIS AGGRESSIVELY"-flavored guidance / anti-pattern
      callouts / MANDATORY-first-call notes style as the existing
      unified rag tool, including AUTHORITATIVE SANDBOX RULE on `fs`
      directing the model to call action="sandbox" + action="check_path"
      before the first read/write of any task.

  Inside builtin_tools.cpp:
    * `namespace fs = std::filesystem` renamed to `stdfs` because
      the new public `Tool fs(...)` factory in the same namespace
      collided with the alias.
    * Each handler factory lifted into its own static
      make_*_handler() function (mirroring the rag handler-factory
      pattern); the unified tool's lambda routes by `action` and
      forwards the original ToolCall.

  Docs updated: README.md, RAG.md, easyai-server.md, easyai-cli.md,
  easyai-mcp-server.md, LINUX_SERVER.md, MCP.md, manual.md, design.md,
  AI_TOOLS.md, EXTERNAL_TOOLS.md, SECURITY_AUDIT.md (just the live
  paragraph; historical findings keep their original tool names).
```

```
2026-05-08 — Server observability + connection-pool fix + prompt
             cleanup (multi-commit pre-noon batch on the AI box).
             Driven by a production failure: an agentic session
             hung mid-stream, the cli retried six times, and we had
             no visibility into what the TCP stack was doing on
             the server.

  Cli keep-alive root-cause fix:
    * stream_chat / simple_get / simple_post each constructed a
      fresh httplib::Client per call, dropping the TCP socket at
      function end. set_keep_alive(true) had nothing to keep
      alive — N tool calls = N sockets piling into TIME_WAIT.
    * Hoisted ONE persistent httplib::Client onto Client::Impl;
      all three call sites reuse it. ONE TCP connection per
      session.

  Server observability (verbose mode):
    * HTTP-level →/← log per request via set_pre_routing_handler
      + set_logger (method/path/peer, status, duration, body
      bytes, running totals).
    * Periodic METRICS line every metrics_interval seconds
      (default 1, --metrics-interval N to tune, 0 disables) with
      CPU%/load/RSS/sys-mem/AMD-GTT/in-flight/cumulative/fd-usage
      AND a TCP state breakdown
      (ESTABLISHED/TIME_WAIT/CLOSE_WAIT/FIN_WAIT/LISTEN) parsed
      from /proc/net/tcp{,6} with TIME_WAIT pressure tag
      (X% [elevated|HIGH|CRITICAL]). Linux-only deep metrics;
      macOS prints n/a.
    * Tool dispatch timing (steady_clock around tool->handler())
      surfaced as ToolResult::duration_ms, shown in CLI logs and
      the webui reasoning panel; new duration_ms field on the
      easyai.tool_result SSE event.

  Tool registration / prompt discipline:
    * REVERSED 2026-05-05 default: --sandbox no longer implies
      --allow-fs. The server read [SERVER] allow_fs but never
      propagated it; default install ships allow_fs=off +
      sandbox=/var/lib/easyai/workspace and hit exactly this.
      allow_fs and allow_bash are now honoured INDEPENDENTLY of
      sandbox. Behaviour change: pass --allow-fs explicitly to
      register fs_*.
    * "Built-in system prompt is tool-aware" — the hardcoded
      prompt used to list fs_*/bash/plan/host-metric tools whether
      or not they were registered, encouraging hallucinated
      calls. Tool notes section is now built dynamically per
      registration; entries for tools the server NEVER registers
      (plan in server, host metrics) are removed entirely. Same
      fix in easyai-local. Both binaries gained a "## Tools —
      closed set" block: "tools are EXACTLY those listed in your
      tools schema; do NOT invent tools; paraphrases (read_file
      vs fs_read_file, shell vs bash) are NOT available."
    * New fs_check_path tool — pre-flight stat + access probe
      under the sandbox root, with optional touch=true to create
      the file when missing. Tool descriptions tell the model to
      call this before any read/write to confirm the boundary +
      effective rights.
    * RAG tool descriptions spell out PRIVATE — MODEL-ONLY STORE
      to forbid "I saved it to memory" / "check the rag for the
      code" answers. Model is instructed to rag_load and put the
      body inline when the user asks for stored content.
    * Stay-in-scope replaces PROTOTYPE FIRST. The 1./2./3. ritual
      ("build → verify → ASK which next step") was making agents
      stop after step 1 and ask. Collapsed to a single
      "## Stay strictly in scope" paragraph. Updated everywhere
      the wording lived (server.cpp, local.cpp, cli.cpp,
      installer system.txt template).

  Other:
    * presence_penalty knob added (engine API + INI [ENGINE]
      key + CLI --presence-penalty).
    * Cli: 3-stage Ctrl-C — graceful (turn finishes) → cancel
      (drop the in-flight stream) → force-exit (process exit
      130). Single ^C is the graceful path; doubling within ~1s
      escalates.
    * Installer GTT default 28 → 29 GiB (matches
      ttm.pages_limit=7602176).
    * Quick-start editor section in LINUX_SERVER.md: VSCode +
      Continue.dev / OpenCode / VSCode + Cline copy-paste
      snippets pointing at http://ai.local:80/v1.
    * No patches/derivatives of llama.cpp — backed out a
      VerboseServer subclass experiment that needed widening
      access on a private virtual in vendored cpp-httplib.

2026-05-08 (later, one commit) — Fifth-pass security hardening
             + tool_lookup builtin. Static review of the ~5,000
             lines that landed in the last 30 commits, plus a
             complementary affordance to the morning's "closed-
             set tool rule" prompt cleanup.

  tool_lookup builtin (lib + every binary):
    * New easyai::tools::tool_lookup(getter) factory in
      include/easyai/builtin_tools.hpp + src/builtin_tools.cpp.
      No-arg call returns numbered 1..N catalogue with one-line
      summaries. name="<substring>" filters case-insensitive
      partial on the tool NAME (not description).
    * Wired into easyai-cli, easyai-server, easyai-mcp-server,
      LocalBackend (covers easyai-local), easyai-agent,
      easyai-recipes — registered LAST so the snapshot covers
      every other tool, including itself.
    * Authoritative description: AUTHORITATIVE / SINGLE SOURCE
      OF TRUTH; training data is NOT; if a name isn't in this
      list IT DOES NOT EXIST; do not retry an unknown-tool call.
      Layered on top of the morning's "Tools — closed set"
      block — the prompt block tells the model the rule, the
      tool gives it a recovery path. One-line tool_lookup
      pointer added inside both kBuiltinSystem closed-set
      blocks (server.cpp, local.cpp's
      build_builtin_system_prompt) and in the easyai-cli
      [tools] dynamic-prefix block.
    * Fail-closed: null getter returns a sentinel tool whose
      handler errors with a deployment-bug message. Getter
      exceptions surface as ToolResult::error; never UB.

  Security 5th-pass findings (2 HIGH, 3 MEDIUM, 2 LOW). Every
  fix preserves the public interface (no flag/tool/header/INI
  key changes).

  HIGH — bash live-mirror to operator stderr (commit 0de93f2)
         was raw + uncapped. A model emitting OSC/CSI escapes
         could retitle the operator's window or wipe the screen
         (none of those bytes appeared in the model-facing tool
         result). Fixed two layers: a sanitize_for_operator_tty()
         strip (CR/LF/TAB through, ESC → visible "^[", other C0
         dropped) and a 128 KiB mirror cap (independent of the
         model-facing 32 KiB cap). The model copy is unaffected;
         only the operator-tty channel is governed.

  HIGH — args::get_array stringified-array unwrap recursion was
         uncapped. A hostile model emitting deeply-nested escape
         sequences forced get_array to recurse N times before
         reaching the actual array, blowing the stack. Refactored
         into get_array_impl(json, key, out, depth) with
         kMaxUnwrapDepth = 4. Public get_array() preserved as a
         depth=0 shim; no callers changed.

  MEDIUM — plan render passes model-supplied item text verbatim
           between our ANSI status codes. Same hijack class as
           bash live-mirror, narrower budget. Added
           sanitize_plan_text() that strips C0+DEL from text
           before render (in-memory plan still carries the raw
           text, only the rendered form is governed).

  MEDIUM — get_sandbox_path used realpath() with a "fall back
           to the unresolved input" branch. Migrated to
           fs::weakly_canonical() with fs::absolute() fallback,
           matching the canonicalisation Sandbox::inside_sandbox()
           uses. Cosmetic-but-correct; the model never sees a
           relative-path leak now.

  MEDIUM — installer accepts --temperature, --top-p, etc. and
           writes them into easyai.ini via heredoc. A value like
           $'0.3\\nallow_bash = on' would inject extra INI keys.
           New require_numeric() helper validates every
           sampling/timeout flag against ^-?[0-9]+(\\.[0-9]+)?$
           before any INI write happens.

  LOW — --mcp <url>: libcurl's protocol filter rejects
        non-http(s), but the operator got a generic curl error.
        Now pre-validated in two places (server.cpp and
        fetch_remote_tools), with a clear message
        "must start with http:// or https://".

  LOW — easyai.ini.bak (created by --force) inherited whatever
        permissions the live INI had. Now explicitly chmod 640
        + chown root:easyai before the new file is written.

  Audit-cleared this pass (no action): need() lambda dash-prefix
  bypass via "--key=value" (false alarm — the lambda already
  catches it), signal-handler safety in cli.cpp (uses only
  async-signal-safe primitives), MCP tool name shadowing (server
  collision-checks at registration; library doc'd as caller's
  responsibility for embedders), httplib retry TLS inheritance
  (settings on shared Client object, no downgrade).

  Documented in:
    SECURITY_AUDIT.md §0 (operator-facing TL;DR — new) and §20
    (this pass's findings); README.md "What's new" 2026-05-08
    entry; manual.md §5.5 cross-references the audit.

2026-05-05 — Tool surface + system prompt overhaul (single-day session,
             ~25 commits). Driven by a production "models drift, use
             bash for file work, ignore tools" report. Three big
             changes plus a new doc.

  Plan tool — tolerance shims + observation coalescing:
    * args::get_array now unwraps a stringified JSON array
      ("items": "[{...}]") — small/quantised models repeatedly
      emit this shape. Same lenient pattern as get_string/get_int.
    * Action inference when 'action' is missing: look at items'
      fields and (for ambiguous cases) plan state to pick add /
      update / delete. Plus synonym mapping (create → add,
      remove → delete, etc.).
    * `add` honours an optional per-item `status` so models can
      create + mark "working" in one call.
    * Plan::Batch RAII guard: coalesces on_change callbacks
      across a tool call — was N renders for N items, now 1.
    * Description rewritten in rag-style: action="X" sections,
      Required/Optional per action, copy-pasteable example
      payloads, role-prefixed param descriptions ("Used by add
      / update / delete. ...").
    * Fixed pre-existing stray '"' that made the schema invalid
      JSON under strict parsers.
    * Errors that teach: on rejection, the message includes the
      correct shape inline ({action:"add",text:"..."}).

  Built-in tools — described to the same standard:
    * Polished 6 thin tools (datetime, read_file, write_file,
      list_dir, glob, grep) with output-shape notes, examples,
      param descriptions that lead with Required./Optional.
      web_fetch / web_search / web_google / get_current_dir /
      bash were already detailed and left alone.
    * NEW tool: `get_sandbox_path` (alongside get_current_dir).
      Pinned at registration to the configured root, so the
      answer is always the truth — distinct from get_current_dir
      which is the live cwd.
    * `bash` description rewritten to LEAD with "PREFER fs tools",
      list bash anti-patterns (cat > / cat <<EOF / echo > /
      mkdir / sed -i) and which fs_* tool replaces each.
      Reserves bash for shell features the dedicated tools don't
      have (pipelines, find | xargs, build runners, git, etc).

  Tool registration defaults:
    * --sandbox <dir> NOW auto-registers fs_* + get_sandbox_path
      (was: required separate --allow-fs).
    * --allow-bash NOW also registers fs_* (bash is strictly
      more permissive — allowing bash without fs_* is incoherent
      and traps the model into using bash for file work).
    * --allow-fs becomes implied by either flag; still works
      explicitly for the no-sandbox / no-bash case.
    * Toolbelt::tools() predicate + examples/cli.cpp wants()
      lambda + examples/server.cpp arg wiring all updated
      together.
    * In-the-wild bug: a session with --allow-bash but no
      --allow-fs produced a model with bash but no fs tools, so
      it used cat > / sed -i for everything. Confirmed via raw
      transaction log; new defaults eliminate the trap.

  System prompt injection (easyai-cli):
    * Two small blocks now prepend the user's --system /
      --system-file content when the agent has any
      create/mutate affordance:
        [environment]  — absolute path of the sandbox root
                         (saves the wasted "where am I" tool
                         hop on turn 1)
        [guidance]     — "Stay strictly in scope. Build the
                         simplest thing that does EXACTLY what
                         the user asked. No extras, no defensive
                         scaffolding, no 'while I'm at it'
                         cleanups. The user's request is the
                         ceiling, not a starting point."
    * Same guidance lives in server.cpp's Deep persona and
      local.cpp's built-in prompt so all three default-prompt
      sites match.
    * Iteration history: started as "pick one and ship", grew
      into a 3-step "PROTOTYPE FIRST → verify → ask for next
      steps" ritual to bound scope. The ritual itself caused
      problems — models would build, stop, and ask even when
      the user just wanted the simplest thing done. Collapsed
      back to a single stay-in-scope paragraph; the no-extras
      / no-scaffolding / no-while-I'm-at-it specifics carry
      the scope-bounding work without forcing the build-then-
      ask dance.

  Default preset: balanced → precise everywhere:
    * Library fallbacks (backend.cpp, cli_client.cpp,
      presets.cpp), CLI defaults (server.cpp ServerArgs,
      local.cpp CliArgs), webui PRESETS map + active button,
      systemd installer INI templates (install_easyai_server.sh,
      install_easyai_pi.sh).
    * Tuned for code/math/factual Q&A — dominant use case for a
      tool-calling agent.
    * README §Sampling presets table rewritten with a Behaviour
      column and a "Pick when…" column.

  --show-system-prompt — added to all 4 binaries:
    * easyai-cli, easyai-server, easyai-local, easyai-chat all
      gain the flag. Each resolves the system prompt as if it
      were about to run (built-in default → --system-file →
      --system, plus the cli's [environment] / [guidance]
      injection), prints, exits.
    * Doesn't load the model, doesn't bind the port, doesn't
      need --url on cli or -m on local. Pure diagnostic.
    * easyai-cli sets EASYAI_NO_AUTO_LOG=1 in this mode so a
      pure diagnostic doesn't leave an empty /tmp/*.log behind.

  Graceful Ctrl-C in easyai-cli (interactive mode):
    * Mid-turn first Ctrl-C / SIGTERM: prints
      `<exiting: waiting for the ai session to be finished.
      Ctrl-C again to force.>` and lets the in-flight chat
      finish naturally. Exits rc=0 once the turn ends.
      Conversation isn't truncated mid-stream.
    * Second Ctrl-C: hard cancel (rc=130) — escape hatch.
    * --quiet keeps the existing immediate-cancel for batch
      scripts (kill <pid> still terminates immediately).
    * Three new globals join g_signal_caught: g_quiet_mode,
      g_in_chat, g_graceful_exit. Handler uses ::write to
      STDERR_FILENO (printf isn't async-signal-safe).

  Parser hardening (easyai-cli):
    * need() lambda now refuses to consume the next argv as a
      flag value if it starts with `--` — catches the typo
      `--system --url X` (was: silently absorbed --url as the
      value of --system, then "url required" later).
    * --show-system-prompt joined --list-tools in the
      only-local-diagnostic exemption so it works without
      --url.

  New doc: easyai-cli.md (552 lines, mirrors easyai-server.md).
    Connection / modes / flags / tool registration / system
    prompt + injection / sampling / reasoning / log /
    management subcommands / 8 worked examples / cross-refs.
    Cross-linked from README.md / easyai-server.md / manual.md.

  Authoring guide: design.md §5 "Writing tool descriptions
    reliably" (architectural) + manual.md §3.2.1 (cookbook).
    Documents the rag-style pattern for multi-action tools
    AND the lenient-handler tolerance shims (synonym mapping,
    action inference, errors-that-teach, batch coalescing).
    Pointer added from AI_TOOLS.md Chapter 9.

  Linux portability fix: <limits.h> for PATH_MAX (not pulled
    transitively on glibc); GCC warn_unused_result on write()
    worked around with the assign-then-discard idiom.

2026-05-05 — Plan tool redesign + HTTP retries everywhere + bumped
             timeout defaults + non-verbose timeout logging.

(pending commit) Two-part change driven by an in-the-wild
                 "Failed to read connection" SSE drop on a
                 thinking model and a plan-tool duplicate-id
                 bug Gustavo hit while watching a session.

  Plan tool (src/plan.cpp + include/easyai/plan.hpp):
    * New statuses: pending / working / done / error / deleted.
      `working` replaces the old `doing`.  `deleted` is a soft
      delete — entry stays in the list rendered struck-through,
      so the user can see what the model abandoned.
    * New actions: `update` (id + text? + status?) replaces the
      old start/done; `delete` (id, items, or `id="all"`).
      Tool description tells the model "never re-add to mutate
      a step — use update", which closes the duplicate-id
      failure mode (the original bug report).
    * Batch mode: every action accepts an `items` array of
      up to 20.  Single-item top-level fields (id/text/status)
      remain for one-off ops.
    * ANSI-colored render: `Plan::render(out, color=true)` emits
      bold/cyan/dim/red/strikethrough.  ui::render_plan and
      Streaming::attach(Plan&) pass through Style.color so the
      checklist colours respect the operator's TTY / NO_COLOR.
    * New args::get_array helper (no JSON dep) walks a JSON
      array at top level and returns each element as a raw
      JSON substring, handling nested objects/strings.

  HTTP retry layer (3 sites, symmetric design):
    * src/client.cpp — Client::http_retries(int) (default 5)
      wraps stream_chat's POST + simple_get + simple_post.
      NEVER retries mid-stream (received_anything sentinel).
      4xx never retries.  Backoff 250ms→500ms→1s→2s→4s capped.
    * src/mcp_client.cpp — ClientOptions::retries (default 5)
      wraps http_post_json's curl_easy_perform.  Resets the
      response buffer between attempts so partial bodies don't
      leak.  No mid-stream concern (whole-response read).
    * src/builtin_tools.cpp — http_get / http_post_form gain
      a `retries` parameter (default kWebHttpRetries=5).
      Same backoff + retryable-error set.
    * tool.hpp/.cpp — args::get_array helper (also used by
      plan.cpp).
    * Retry events log via easyai::log::error so they land on
      stderr without --verbose; format strings include trailing
      \n (the existing "stream_chat failed" line ALREADY had
      this bug — pre-dates this work).

  Timeouts bumped:
    * Client::timeout_seconds default: 600s → 1800s (30 min).
      Long-thinking models hold streams for many minutes between
      visible tokens.
    * easyai-server cpp-httplib set_read/write_timeout: hardcoded
      60s → configurable args.http_timeout (default 600s).
      `--http-timeout SECONDS` / `[SERVER] http_timeout` in INI.

  CLI / INI wiring:
    * examples/cli.cpp: --http-retries N, EASYAI_HTTP_RETRIES env,
      EASYAI_TIMEOUT env.
    * examples/server.cpp: --http-retries N + --http-timeout SECONDS,
      both via the FlagDef table (so they get CLI + INI for free).
      MCP-client opts inherit both values.
    * Startup banner now echoes "http_timeout=Xs http_retries=Y"
      unconditionally so operators see them in journalctl.
    * set_exception_handler now logs to stderr unconditionally
      with method/path/peer; new set_error_handler logs HTTP
      408/504 timeouts and 5xx with the same context.

  Smoke tested: --http-retries 0 → 1 attempt; --http-retries 2
  → 3 attempts with visible exponential backoff in the logs.
  All examples build clean.  Documentation updated across
  README / manual / design / easyai-server / MCP / LINUX_SERVER /
  SECURITY_AUDIT (see commit "Docs: ...").
```

```
2026-05-04 — RAG default flipped to single-tool dispatcher; --split-rag
             opts back into legacy seven; concise default prompt.

(pending commit) Two user-driven changes:

  (1) RAG default flip. The unified `rag(action=...)` dispatcher
      used to be opt-in behind --experimental-rag; it is now the
      DEFAULT for every binary (easyai-server, easyai-cli,
      easyai-local, easyai-mcp-server). The legacy seven separate
      `rag_*` tools are still available behind a renamed flag,
      --split-rag (and INI key SERVER.split_rag), for operators
      driving weak / 1-bit-quant tool callers (Bonsai-class) that
      handle many flat schemas more reliably than one
      discriminated schema. On-disk format, locking, and
      fix-memory rules unchanged — only the catalog shape
      differs. Files: include/easyai/rag_tools.hpp (comments
      flipped), src/rag_tools.cpp (description prose, drop
      "EXPERIMENTAL —" prefix, dispatcher section header),
      include/easyai/backend.hpp + src/backend.cpp (Config gains
      split_rag bool; LocalBackend picks the factory by it),
      examples/cli.cpp (--split-rag flag, register_tools branch
      flipped, --tools/--RAG help refreshed), examples/server.cpp
      (FlagDef table: split_rag/--split-rag/SERVER.split_rag,
      registration block flipped, --RAG/--split-rag help
      refreshed), examples/mcp_server.cpp (same FlagDef + INI
      key + registration flip), examples/local.cpp (--split-rag
      arg, plumbed into LocalBackend::Config). All four affected
      binaries build clean. easyai-cli --list-tools --RAG <dir>
      reports "rag" by default; with --split-rag added, reports
      the seven legacy names. Server INI sanity-check confirms
      [SERVER] split_rag = on logs "RAG enabled (split: seven
      rag_* tools)" while the absence logs "RAG enabled (single
      rag tool)".

  (2) Default system prompt rewritten to emphasise plan → act →
      iterate in tight steps, kept brief on purpose so the user
      has room to refine. examples/server.cpp's kBuiltinSystem
      and examples/local.cpp's kBuiltinSystem went from ~30-90
      lines of operating-loop / rule prose down to ~20-30 lines
      that say: answer briefly; for real work, plan ONE small
      next step, act on it in the same turn, read the result,
      then finish or take ONE more step; stop as soon as you
      have something useful. Retains the no-announce-without-
      call rule and the search → fetch discipline. The server
      prompt is what plays through /v1/chat/completions when
      no client supplies its own system message; easyai-cli
      remains a thin client and uses whatever prompt the
      backing server defaults to.

  Docs updated: README.md (new What's new entry + every
  --experimental-rag mention rewritten to --split-rag with
  flipped polarity in the options/flags tables), RAG.md (intro
  example switched to rag(action=...), §4 dispatcher rewritten
  as default + opt-in seven), easyai-server.md (INI table row
  + "All options" row flipped), LINUX_SERVER.md (default-paths
  block + tool-count math reflect new default), MCP.md (catalog
  references mention both layouts), manual.md (RAG quickstart
  shows both factories), design.md ("Why seven tools" rewritten
  as "One tool by default, seven under --split-rag" + four-tier
  table updated), easyai-mcp-server.md (tool-source table now
  has BOTH rows), SECURITY_AUDIT.md (mutex paragraph clarifies
  both layouts share the same RagStore + locks).
2026-05-02 (later) — RAG: rag_append + user-focus prompts.

(pending commit) Adds a seventh tool to RagTools: rag_append.
                 Read-modify-write on an existing memory: reads
                 body off disk, appends new content after a
                 Markdown horizontal rule (`---`), atomically
                 rewrites via tempfile + rename (same
                 save_locked path rag_save uses). The whole RMW
                 runs under one std::unique_lock<shared_mutex>,
                 so concurrent appenders to the SAME title queue
                 cleanly (both appendices land), and concurrent
                 saves/deletes/reads serialise / parallelise via
                 the existing reader/writer discipline. Refuses
                 on titles that don't exist, on fix-easyai-* (the
                 immutability invariant covers append the same
                 way it covers save/delete), and when merged
                 size > kMaxContentBytes (256 KiB).
                 Optional keywords[] arg merges into the existing
                 keyword list (deduped, total still capped at 8;
                 oldest wins on overflow). New handler:
                 make_append_handler in src/rag_tools.cpp;
                 wired into make_rag_tools as RagTools::append
                 and into make_unified_rag_tool as
                 action="append" (kSubs entry added so legacy
                 prose references like rag_append in the inner
                 description get rewritten to
                 rag(action="append") inside unified mode).
                 User-focus prompt update: rag_save and
                 rag_append descriptions now explicitly tell the
                 model to prioritise notes about the user
                 themselves (name, role, hardware, projects,
                 working style, corrections, likes, dislikes)
                 and prefer rag_append on the existing profile
                 memory over rag_save (which would overwrite).
                 Suggested user-* titles: user-profile,
                 user-prefs, user-projects, user-hardware,
                 user-corrections.
                 All four consumers updated to register the new
                 tool: examples/server.cpp, examples/mcp_server.cpp,
                 examples/cli.cpp (with tools_enabled gating
                 mirroring the rest), src/backend.cpp (used by
                 easyai-local + easyai-chat). Help-text strings
                 also updated — the lib went from 5/6 to seven
                 tools and every CLI reference (server.cpp,
                 cli.cpp, local.cpp, mcp_server.cpp) now lists
                 the canonical seven names.
                 Docs: README.md (What's new + options table),
                 RAG.md (TOC, intro, quickstart, full
                 rag_append section in §4, dispatcher §,
                 tool-flow ASCII diagram restructured into
                 write-row + read-row), manual.md, design.md
                 (renamed "Why five tools" → "Why seven tools"),
                 LINUX_SERVER.md, MCP.md (3 places),
                 easyai-server.md (INI table + flag reference +
                 cross-refs), easyai-mcp-server.md, SECURITY_AUDIT.md
                 (§16.6b reflects shared_mutex + seven tools).
                 All 7 binaries build clean. easyai-cli
                 --list-tools confirms the new tool registers
                 correctly (16 tools when --RAG is on: 9 default
                 + 7 RAG).

2026-05-02 — Fourth-pass security audit + readability batch.

(commits 5143799 + b44b615) Two small commits, no public API
                 change. Fourth security pass found one MEDIUM:
                 the auto-generated transaction log at
                 /tmp/easyai-<pid>-<epoch>.log was opened with
                 std::fopen("w") (follows symlinks) and process
                 umask (typically 0644 → world-readable). Path is
                 predictable (16-bit PID + 1-second epoch), so a
                 local attacker on a multi-tenant host could
                 plant a symlink at the predicted path pointing
                 at any user-writable file (~/.bashrc, ~/.ssh/…)
                 and have the next easyai-* process truncate-
                 and-overwrite it. Mode 0644 also leaked prompts
                 (which can contain API keys or PII) to other
                 accounts on the same box. Fixed in src/log.cpp
                 (auto_open) and src/cli.cpp (open_log_tee) by
                 swapping fopen for ::open with
                 O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,
                 mode 0600, then ::fdopen. Caller-supplied paths
                 keep O_TRUNC for log rotation but still gain
                 O_NOFOLLOW + 0600. Validated with a standalone
                 symlink-attack smoke test (errno=EEXIST,
                 victim file untouched). Documented in
                 SECURITY_AUDIT.md §19. The same audit pass
                 cleared nine other findings as already-mitigated
                 / by-design / accepted-risk (§19.2).
                 Readability batch: three inline patterns lifted
                 into named helpers — file_mtime_unix() in
                 rag_tools.cpp (3 dup), glob_to_regex() +
                 kGlobRegexMetachars in builtin_tools.cpp, and
                 looks_like_announce_phrase() in engine.cpp
                 (used twice inside chat_continue). Helpers carry
                 the WHY comments; call sites are now one line.
                 Net diff +103/-85 across three files; all 7
                 binaries build clean.

2026-04-30 (late evening) — Central INI config (/etc/easyai/easyai.ini).

(pending commit) Single config file replaces ~17-flag systemd
                 ExecStart. Sections: [SERVER], [ENGINE], [MCP_USER],
                 [TOOLS] (reserved). Precedence: CLI > INI > hardcoded.
                 New `--config <path>` flag (default
                 /etc/easyai/easyai.ini); missing file = use defaults
                 + open MCP. INI parser in src/config.cpp (no deps,
                 ~80 lines, tolerates missing/malformed lines as
                 warnings). cli_set tracks explicit CLI flags so
                 INI applies only as defaults. New `--no-mcp-auth`
                 force-opens /mcp even with [MCP_USER] populated.
                 [SERVER] mcp_auth=on/off/auto INI-side equivalent.
                 [MCP_USER] populates Bearer-token auth: each line
                 `name = token`, audit log shows
                 `[mcp] request from user 'name'`. Empty section
                 = open. Install script writes a fully-populated
                 easyai.ini on fresh install; leaves it alone on
                 --upgrade (operator edits win). Systemd ExecStart
                 shrunk from ~17 flags to:
                   easyai-server --config /etc/easyai/easyai.ini -m <model>
                 [+ --api-key '${EASYAI_API_KEY}' if /etc/easyai/api_key
                  exists; + --webui-icon if installer was given one].
                 NEW doc INI_KFlags.md — full key reference, every key with
                 type / CLI equivalent / default / notes; worked
                 examples (minimal local-dev / production-with-auth /
                 CLI overrides); cross-refs from README, MCP.md,
                 LINUX_SERVER.md, SECURITY_AUDIT.md.

2026-04-30 (evening) — easyai-server speaks MCP + Ollama list-models.

(pending commit) MCP server: POST /mcp endpoint exposes the full
                 tool catalogue (built-ins + RAG + external-tools) as
                 a JSON-RPC 2.0 MCP provider. Stateless request/
                 response, methods: initialize / tools/list /
                 tools/call / ping plus notification no-ops. Pure
                 function dispatcher in src/mcp.cpp — no global
                 state, never throws. Tool exceptions become MCP
                 isError=true (not JSON-RPC errors).
                 Auth: OPEN by V1 design (Bearer-gate planned for
                 V2; SECURITY_AUDIT.md §17.1 documents accepted
                 risk and compensating controls).
                 Compatibility shims: GET /api/tags + /api/show
                 (Ollama-shape list-models for LobeChat / OpenWebUI
                 / Continue's Ollama provider). /v1/models was
                 already there (OpenAI). /health gains a `compat`
                 block listing all three protocol surfaces.
                 Stdio bridge: scripts/mcp-stdio-bridge.py — Python,
                 stdlib only — for Claude Desktop which only speaks
                 stdio MCP. Cursor / Continue talk HTTP direct.
                 NEW doc MCP.md with quickstart, per-client
                 connection cookbook (Claude Desktop / Cursor /
                 Continue / custom), auth roadmap, troubleshooting.
                 README, design.md §6c, LINUX_SERVER.md, and
                 SECURITY_AUDIT.md §17 all updated.

2026-04-30 (afternoon) — RAG: agent's persistent registry / long-term memory.

(pending commit) RAG: a tag-keyed file-backed long-term memory the
                 model writes to via 5 tools (rag_save, rag_search,
                 rag_load, rag_list, rag_delete). Directory of
                 .md files <title>.md, format `keywords: a, b, c\n
                 \n<body>` — hand-editable, grep-able, scp-able. Title
                 regex closes path traversal. Bounded sizes (title 64,
                 keyword 32, max 8/entry, body 256 KiB, max 4 loads/call).
                 Atomic writes (tempfile + rename). In-memory index
                 lazy-built. New `--RAG <dir>` flag on easyai-server,
                 easyai-cli, easyai-local; install script creates
                 /var/lib/easyai/rag (owned by service user, 750) and
                 systemd unit always passes the flag. Tool descriptions
                 actively encourage save-aggressively /
                 search-before-assuming / delete-stale. New top-level
                 docs: RAG.md (full guide w/ workflows, document
                 ingestion cycle, roadmap) and LINUX_SERVER.md (operator
                 guide for the systemd-installed server, file layout,
                 perf tips, gotchas, API examples, backup/upgrade).

2026-04-30 (morning) — External tools v2: directory loader + sanity-check warnings.

(pending commit) External tools: rename --tools-json PATH to
                 --external-tools DIR, scanning EASYAI-*.tools files in
                 a directory. Per-file fault isolation: a syntax/schema
                 error in one file is logged and skipped, others still
                 load. Empty dir is a normal state (silent, no
                 error/warning). Sanity-check pass at load: warns on
                 shell wrappers, dynamic-linker env passthrough
                 (LD_PRELOAD etc), world-writable command binaries,
                 world-writable manifest files. easyai-cli -q
                 suppresses warnings; load errors always emit. Install
                 script creates /etc/easyai/external-tools/ empty +
                 README + EASYAI-example.tools.disabled. New top-level
                 doc EXTERNAL_TOOLS.md (collaboration guide with 10
                 recipes, anti-patterns, corner cases, troubleshooting).
                 README sales pitch + cross-link.

2026-04-29 — External tools v1 (operator-defined commands via JSON manifest).

e966cf1  External tools: harden child setup, reject NaN/Inf, retire magic numbers
                    (review follow-up: kMaxFdScan caps the close() loop so
                     RLIMIT_NOFILE=infinity no longer leaks parent fds; PR_SET_PDEATHSIG
                     ties subprocess lifetime to the agent; std::isfinite() rejects
                     NaN/Inf model args; slurp() stat-first; env_passthrough value
                     length cap; all magic numbers promoted to named constexprs.)
d0f7965  Tools: get_current_dir builtin + JSON-manifest external tools
                    (Two-part feature. (1) get_current_dir: zero-param builtin
                     returning getcwd() at call time; CLIs chdir() into --sandbox
                     at startup so what the tool reports == bash/fs_*'s effective
                     dir. Toolbelt registers it whenever any fs-tool is enabled.
                     (2) load_external_tools_from_json: operator declares
                     name/description/abs-command/argv-template/parameters/timeout
                     /max_output/cwd/env_passthrough/stderr in a JSON manifest;
                     each entry compiles to a regular Tool. Fork+execve, never a
                     shell; absolute path validated at load (no PATH-hijack);
                     whole-element placeholders only ("{x}", never "--flag={x}");
                     hard caps on manifest size / tools / params / argv / env;
                     env passthrough opt-in allowlist; closed stdin; setpgid +
                     SIGTERM-then-SIGKILL kills grandchildren. New --tools-json
                     PATH flag in easyai-local / easyai-cli / easyai-server.
                     manual.md §3.3.3 + §3.3.4. examples/tools.example.json.)

2026-04-29 (earlier) — robustness pass on server + CLI streaming.

84009a0  Cancel: stop server job when the client connection drops
a3038fa  CLI: tolerate non-UTF-8 bytes in tool output and model content
2af3fe8  Docs: add AI_TOOLS.md — a vendor-neutral book on AI tool calling
6939e4c  CLI: stop <think> tags from bleeding into streamed content

2026-04-28 — webui polish + fs/bash gating split.

c10c812  Webui: hide the floating top-right Settings gear button
4b3f9fa  Server: --allow-fs gate for fs_*; webui thinking lock + tool URL log
36c5f10  Webui: remove HTML-like markup from AI-brain.svg <style> comment
cedad05  Webui: restore original AI-brain favicon, add theme-aware brand fill
f9ea948  Webui: narrow SVG theme rule to hardcoded near-black colors only

2026-04-27 (afternoon) — Deep + lib-first refactor (phase 5/6).

2dd5e79  Deep — name + persona for the default easyai-server assistant
866abde  Phase 6 — easyai::Agent, the friendly Tier-1 front door (3-line hello)
1280aac  Phase 5c — Backend abstraction (LocalBackend + RemoteBackend) in lib
22ece56  Phase 5d — easyai::cli management subcommands (print_models, …)
3b66832  Phase 5b — easyai::ui::Streaming wires the canonical agent UX
344ff4b  Phase 5a — extract small reusable utilities into the lib
                    (ThinkStripper, trim_for_log, render_plan, print_presets,
                     print_tool_row, slurp_file w/ max_bytes, client_has_tool)
8314eb6  Client::retry_on_incomplete now defaults ON in libeasyai-cli
dec031f  client: fix json::type_error 302 in retry-on-incomplete nudge

2026-04-27 (morning)  — sandbox virtualisation + bash tool + library refactor.

8b60868  SESSION_NOTES update for the morning's work
25bf165  Docs refresh: Toolbelt, bash tool, --sandbox/--allow-bash gating
2572bc0  Static analysis pass: SSE buffer cap + grep regex DoS guard
bc28474  easyai::cli — Toolbelt builder + log file helper + sandbox validator
2fa381a  Extract CLI utilities into the public lib (easyai::ui / text / log)
21590b9  max_tool_hops: configurable; --allow-bash bumps to 99999
b8eda29  easyai-cli: same fs_*/bash gating as cli-remote and server
8e957af  Auto-retry-with-nudge for incomplete turns; default ON in cli-remote
0a93741  bash tool + tighten fs_*/bash gating in server and cli-remote
430f9b0  fs tools: virtual /-rooted view; sandbox base hidden from the model
56d44c3  Sandbox::resolve: total containment via path-component filtering
443468a  cli-remote: drop per-piece [content N, +Tms] / [reason N, +Tms] traces
fb8becb  cli-remote: newline on reasoning↔content transition
afe7a57  Server: incomplete-detection two-tier threshold (80 B / 350 B post-tool)
e3cf422  libeasyai-cli + cli-remote: RAW transaction log file for offline analysis

v0.1.0 (tag) — first formal release, 2026-04-26.

dc74102  easyai-cli phase 3 + full sampling control on Client
8e6c4e4  libeasyai-cli phase 2: full Client implementation (HTTP/SSE + agentic)
5bf32c0  Lib-ise easyai + scaffold libeasyai-cli (phase 1 of 3)
8a1ca33  Webui: live ctx during stream, override favicon, tools badge, lock UX
46903e3  Engine: recover tool_calls from markdown markers when model abandons XML
d7f638e  Webui ctx counter: cumulative session usage + near-limit input lock
b2f77ff  Webui: move AI-brain.svg src/->webui/, reopen think panel, diag console
870be9b  Webui: fix SyntaxError that killed tone-badge IIFE + bundle SSE noise
c2eb22c  Webui: chip always visible, tone badge unstuck, smaller response body
9b31a38  Stop reasoning + Hermes tool_call XML bleeding into chat body
e9297fe  Brand to EasyAi + embed brain SVG favicon + harden bar/tone visibility
f8d83d1  Stack-overflow audit chapter in design.md
3dec718  Fix SIGSEGV stack-overflow from std::regex in strip_html + silent SSE
7976dbe  Webui follow-ups + Bug B fallback now reaches SSE stream
731d441  Webui: blue brain panel, hide bundle Reasoning, live metrics + pulsing pill
2ff181e  Framework polish: args defaults, recipes example, docs, installer
5f7fa7d  Bug B fix + streaming UI normalisation
186c20a  Update SESSION_NOTES.md with reasoning-format fix and Bug A/B status.
709684b  THE missing piece: set reasoning_format on common_chat_templates_inputs
3a94bd3  Fix Jinja crash: push user msg before chat_params render in streaming
882abed  Add SESSION_NOTES.md
e03705e  installer: --upgrade now actually does git pull, not just git fetch
15e6056  Streaming pipeline: incremental common_chat_parse + diff (llama-server parity)
204e376  Real per-message thinking panel via DOM injection
25d43de  Route thinking to delta.reasoning_content
25e8095  --verbose flag + auto-prepend <think> for Qwen3-thinking
f1282e4  UTF-8 tolerant SSE dumps + count any non-CPU device as backend
89bab48  installer: default --ngl to -1 (auto-fit) instead of 99
8649ddd  Linux installer: ship libs into /usr/lib/easyai + LD_LIBRARY_PATH
0fcaad4  Linux installer: ship every llama.cpp .so + force --ngl based on what built
b660bbf  installer: parity pass with install_llama_server.sh on systemd unit + sysctl
1e9542b  installer: set RADV_PERFTEST=gpl + HOME/XDG_CACHE_HOME
0fa4c0a  Lift the input form to make room for the metrics bar below it
4b727d1  Move bar below textarea + chip alignment fix
c6a09d6  Single combined bar above the textarea (tone + ctx + last)
```

`git log --oneline | head -30` for the rest.

### Recent rabbit-hole notes

- **Sandbox containment refactored to total-anchor (2026-04-27)**: the
  original `Sandbox::resolve` rejected any path whose canonical form
  escaped the root.  Models would research, then emit `write_file
  path: "/2026-04-27_news.md"` and the rejection killed the whole
  deliverable.  Two iterations:
  1. `56d44c3` — iterate the input's path components, drop empty
     fragments, separators (`/`, `\`), and pure-dot components
     (`.`, `..`, `...`, …), join survivors onto sandbox root.
     Trade-off: a malicious symlink already inside the sandbox
     could now follow itself out, since we no longer call
     `weakly_canonical`.  Acceptable — the sandbox is user-owned
     and we don't expose symlink creation to the model.
  2. `430f9b0` — in addition, render the sandbox path as `/`-rooted
     in tool descriptions and result/error messages.  The model
     thinks the world starts at `/`, never sees the real prefix.
     Stops the model from inventing `/home/user/`, `root/`, etc as
     phantom subdirs (those choices were prompted by ambiguous
     descriptions saying "default = root").

- **Auto-retry with corrective nudge for incomplete turns
  (2026-04-27, `8e957af`)**: the previous `--retry-on-incomplete`
  did a blind retry on the same prompt, which usually reproduced
  the same "Let me fetch a few more sources." stop-without-tool_call
  bailout.  Now the retry appends a synthetic user message:
  *"Your previous reply only announced an action without emitting
  any tool_call. Do NOT say 'let me…' / 'I'll…' unless the tool_call
  follows in the SAME turn. Either call the next tool you actually
  need, or give the user the final answer."*  Default ON in
  cli-remote; one retry max (no spirals).  Reproduces across
  multiple models including Gemini, so it's an infra-level fix.

- **bash tool design choices (2026-04-27, `0a93741`)**: `/bin/sh -c`
  for full shell features (pipes, redirects, &&); cwd pinned to
  --sandbox; output capped at 32 KB; SIGTERM at deadline, SIGKILL
  +2 s grace; per-command timeout default 30 s, max 300 s.  Honest
  in its own description: NOT a hardened sandbox — it's a normal
  shell process with the caller's user privileges.  Opt-in only via
  `--allow-bash`.  Bumps `max_tool_hops` to 99999 because bash flows
  span many turns (compile → run → fix → re-run).

- **Static analysis findings (2026-04-27, `2572bc0`)**: SSE pending
  buffer was unbounded — a malformed stream that never emits a
  terminator would OOM the client.  Capped at 16 MiB with a clean
  abort.  Separately, `fs_grep` ran user regexes against
  multi-megabyte single lines (binary blobs, minified JS) — libstdc++'s
  recursive regex blows up on `(a+)+` style patterns.  Skip lines
  >64 KiB.  Audit also cleared format-string vulns (no
  `printf(user_string)`), URL allowlist (http/https only), HTTP body
  caps (2/4 MB), JSON parse exception handling.

- **Why thinking was rendering inline as message body** (root cause found
  in `709684b`): `Engine::Impl::render()` left `common_chat_templates_inputs::
  reasoning_format` defaulted to `NONE`.  Every PEG parser builder under
  `common/chat.cpp` does `auto extract_reasoning = inputs.reasoning_format
  != COMMON_REASONING_FORMAT_NONE;` — with NONE, the parser leaves
  `<think>...</think>` inside `msg.content`, our diff dumps it as
  `delta.content`, and the webui paints it as the regular reply.  Fixed
  by setting `in.reasoning_format = COMMON_REASONING_FORMAT_AUTO`.
  AS OF 2026-04-26 this fix is unverified on the user's AI box.

- **Why streaming was crashing with "No user query found in messages"**
  (fixed in `3a94bd3`): Qwen3 templates raise a Jinja exception if the
  history doesn't contain a user message at template-apply time.  We were
  calling `Engine::chat_params_for_current_state(true)` BEFORE pushing the
  user message into history.  Fix: push first, then render.  Required
  splitting `Engine::chat()` into `chat()` (does push + chat_continue) and
  the new `chat_continue()` (assumes user is already last in history) so
  the streaming path can render in between.

- **Engine::chat_continue()** (added in `3a94bd3`) is what the streaming
  HTTP path now calls in agentic mode.  Same multi-hop loop as `chat()`
  minus the initial `push_message("user", …)`.

- **Bug B root cause and fix** (this session, 2026-04-26):
  Qwen3.6-35B-A3 fine-tune `eng_v5` sometimes terminated the turn after
  `</think>` without emitting either content or a tool_call — model's
  last reasoning chunk was literally *"(Let's execute the tools now)"*.
  Curl trace showed 0 content + 0 tool_calls + finish_reason=stop.
  Adding `--verbose` confirmed parser was NOT engulfing — model truly
  emitted EOS.  Fix: in `chat_continue()`, detect
  `tool_calls.empty() && content.empty() && reasoning_content non-empty`,
  discard the empty turn, clear KV, fire `on_hop_reset` (new callback
  that streaming layer hooks to drop accumulated/prev_msg), and loop.
  Budget = 2 retries.  Final fallback promotes reasoning → content so
  the user never sees an empty bubble.  Validated via curl: now lands
  reliably on a real tool_call (web_search) + content reply (~970 chars).

- **`compute_diffs` strict-monotonic exception** discovered while
  testing the retry path: sometimes the partial parser temporarily
  assembles a tool_call and then "unassembles" it as more tokens
  arrive; `common_chat_msg_diff::compute_diffs` throws
  *"Invalid diff: now finding less tool calls!"*.  Fix: try/catch
  around the diff in the streaming `on_token` lambda — hold prev_msg
  on last good state and wait for next token to settle.

## 6. Known issues / pending validation

- **HTTPS for `easyai::Client`** — not wired yet.  Needs cpp-httplib's
  `CPPHTTPLIB_OPENSSL_SUPPORT` define + linking OpenSSL into the
  `easyai_cli` target + a `make_http()` branch returning an SSLClient
  base pointer (httplib's SSLClient inherits from `httplib::Client`).
  Until this lands the `Client` returns a clear error on `https://`
  endpoints.  Workaround: terminate TLS at a reverse proxy.

- **(A) Webui thinking-panel rendering** — ✅ **FIX VALIDATED**.
  Commit `709684b` made the per-message reasoning panel light up
  correctly.  Curl confirmed (812 / 1165 / 2027 reasoning_content
  deltas across runs vs 0 content deltas pre-fix; bundle now paints
  its own native panel).  This session also disabled our custom
  `__easyai-thinking` panel (dormant behind `window.__easyaiCustomThink`)
  so we don't get two glued panels — only the bundle's "Reasoning"
  panel renders, font shrunk to 0.72rem mono, default open during
  streaming, auto-collapse on finish_reason.

- **(B) Qwen3-thinking model stops after `</think>`** —
  ✅ **FIXED via thought-only retry path** (see "Bug B root cause" above).
  Validated via curl in this session.  Pending user webui validation.

- **RAG (`--RAG DIR`)** — agent's persistent registry / long-term memory.
  Five tools (rag_save / rag_search / rag_load / rag_list / rag_delete)
  share a directory of `.md` files with `keywords: a,b,c` headers and
  free-form bodies. Server enables by default
  (`/var/lib/easyai/rag`, systemd unit always passes `--RAG`); CLI
  variants opt in. End-to-end smoke test passed (round-trip save →
  list → search → load → delete). Authoritative doc: `RAG.md`.
  Operator guide: `LINUX_SERVER.md`.

- **External tools (`--external-tools DIR`)** — landed in `d0f7965`,
  hardened in `e966cf1`, evolved to dir loader + sanity warnings in
  the 2026-04-30 commit. Operators drop `EASYAI-<name>.tools` files
  in a directory; library scans the directory at startup (per-file
  fault isolation: a bad file is logged + skipped, others still
  load; empty dir is silent). Spawn path is `fork`+`execve` (never
  a shell). Security guarantees enforced at load time (absolute
  command path, no embedded placeholders, hard caps on
  manifest/argv/params/env/output/timeout, regular-file check), at
  call time (JSON-Schema arg validation, NaN/Inf rejection,
  `kMaxFdScan` close-loop, `PR_SET_PDEATHSIG`, process-group SIGTERM
  → SIGKILL, env-value cap), and at deploy time (manifest is the
  operator's trusted artefact, treat like sudoers). Sanity-check
  warnings at load: shell wrappers, `LD_*` passthrough,
  world-writable binaries / manifests. Authoritative doc:
  `EXTERNAL_TOOLS.md`. Also: paired `get_current_dir` builtin so
  the model can find out the absolute path it's running in
  (Toolbelt auto-registers it when any fs-tool is enabled).

- **Webui core dump under heavy load** — user reported coredump while
  using webui.  Did NOT reproduce in this session.  Tooling now in
  place to capture next occurrence:
    - `systemd-coredump` package installed (also baked into installer's
      apt deps so future installs get it)
    - `LimitCORE=infinity` in service drop-in (also baked into installer's
      unit template so future installs get it)
    - `--verbose` available via `--enable-verbose` installer flag (default OFF)
  Next time it crashes, run `coredumpctl list easyai-server.service`
  then `coredumpctl gdb <PID>` to get a stack.

- **llama-server parity reference**: when the user reports that
  `llama-server` does X correctly and we don't, look at `llama.cpp/tools/
  server/server-{task,chat,context}.cpp` for their stream pipeline.
  We've already mirrored:
      - common_chat_parse(text, is_partial=true, params) per token
      - common_chat_msg_diff::compute_diffs(prev, new)
      - server_chat_msg_diff_to_json_oaicompat-style delta envelope
        (.reasoning_content + .content + tool_calls)
  Differences that may still matter: their reasoning_format passed at
  task params construction time; their handling of `multi_step_tool`
  flag in chat templates; their default sampling params.

## 7. Common pitfalls / debugging tips

- After any `examples/server.cpp` change that affects the served HTML/JS
  injection, the user MUST do **Cmd+Shift+R** in the browser to bypass cache.
- The bundle.js is 6.2 MB; binary size jumps from ~1.5 MB to ~8.3 MB.
- `--ngl 99` poisons `common_fit_params` (refuses to lower a user-pinned
  value).  Always default `--ngl -1` (auto-fit) so the engine picks how
  many layers fit.
- Vulkan/RADV often reports devices as `ACCEL` not `GPU` — the engine's
  `backend_summary` accepts any non-CPU type now.
- `<think>` content can split across two model tokens — the streaming pipe
  uses `common_chat_parse(is_partial=true)` which handles this; don't
  reintroduce regex-based tag matching.
- The bundle hashes class names — never rely on `[class*="something"]`
  selectors.  Use `aria-label`, `data-testid`, or text-content matching.
- Shadow DOM mounts (`__easyaiBarHost`) survive Svelte body re-renders
  because they're attached to `<html>`, not `<body>`.

## 8. How the user typically iterates

```
# On Mac M3 (development):
cd ~/develop/easyai/easyai
cmake --build build -j
./build/easyai-server -m models/qwen2.5-3b-instruct-q4_k_m.gguf --webui-title "AI Box"
# user opens http://127.0.0.1:8080, Cmd+Shift+R, tests

# On Linux AI box (production):
cd ~/easy
git pull
sudo systemctl stop easyai-server
./scripts/install_easyai_server.sh --upgrade --enable-now
sudo journalctl -u easyai-server -f
```

## 9. Communication style

- Reply in Brazilian Portuguese.
- Keep replies tight; the user is technically fluent and prefers concrete
  diffs / commands over long explanations.
- Show what changed, why, and the exact command to reproduce on the AI box.
- When you commit, push immediately — the user often pulls right after.
- Use task tracking via TaskCreate/TaskUpdate when work spans multiple
  steps.

## 10. Recent changes (2026-05-20)

### easyai-cli

| Change | Detail |
|--------|--------|
| `--shell` mode | Hybrid AI shell: normal commands execute in user's `$SHELL`, lines prefixed with `>` go to the AI model. Builtins (`cd`, `export`, `unset`) run in-process. Subprocess execution via `fork`/`exec`/`waitpid`. Requires TTY. |
| Single Ctrl+C | One press stops generation and returns to prompt (no multi-press escalation). Triple rapid press force-exits. Shell subprocesses receive SIGINT from kernel directly (shared pgrp). |
| Green `●` prompt | REPL prompt changed from cyan `>` to green `●` matching tool-call success markers. |
| Spinner transition report | When spinner leaves token-streaming mode (tk/s visible), emits a dark blue summary line: `● XX% / NNNN tokens  last: 00.0tk/s` before starting the next shimmer/wheel. Also emitted at `finish()`. |

### memory_append tool

| Condition | Behavior |
|-----------|----------|
| Title exists | Appends content, returns "updated …" |
| Title does not exist + keywords provided | Creates new memory (like `memory_save`), returns "new memory saved as …" |
| Title does not exist + no keywords | Returns error asking for `keywords[]` |

### Files touched

`examples/cli.cpp`, `src/rag_tools.cpp`, `include/easyai/ui.hpp`, `src/ui.cpp`, `src/cli_client.cpp`, `spec.md`, `easyai-cli.md`, `RAG.md`, `manual.md`, `design.md`, `README.md`.

---

*End of session notes.  Update this file as work progresses so future
sessions resume cleanly.*
