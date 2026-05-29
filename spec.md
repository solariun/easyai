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
