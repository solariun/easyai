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
| python3 | `--sandbox` and not `--no-python` |

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

## memory_load / memory_search Limits

| Operation | Default | Max |
|-----------|---------|-----|
| load (titles per call) | — | 20 |
| search (results per page) | 10 | 20 |
| list (entries) | 50 | 200 |
| keywords (vocabulary) | 200 | 500 |

## memory_append Tool Behavior

| Title exists? | Behavior | Return message |
|--------------|----------|----------------|
| Yes | Append content after `---` separator | `updated "title.md" (+N B → M B total, K keywords)` |
| No (keywords given) | Create new memory (save semantics) | `new memory saved as "title.md" (N bytes, K keywords)` |
| No (no keywords) | Error | Explains keywords are required for new memories |
| Fixed (`fix-easyai-*`) | Error | Immutable, cannot append |

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
| `python3` | yes (read-only) | **NO** — sandbox preamble rejects any write-mode `open()`, even inside the sandbox root |

Code enforcement: `kPythonSandboxPreamble` (src/builtin_tools.cpp) wraps `builtins.open`, `io.open`, and `os.open` to raise `PermissionError` on any `w/a/x/+` mode or `O_WRONLY|O_RDWR|O_CREAT|O_TRUNC|O_APPEND` flag. Read-only `r`, `rb`, default-mode open continue to work inside the sandbox.

Prompt enforcement: `easyai::preamble::tools_block(view)` emits the `## Write/edit policy (AUTHORITATIVE)` section whenever `python3` is registered. MCP servers inject the same policy via `initialize.result.instructions` so MCP clients' models see it too.

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

### python3 disk enforcement

The `kPythonSandboxPreamble` injected before every `python3` snippet enforces TWO invariants:

1. **Sandbox containment** — open() / io.open() / os.open() reject paths resolving outside the cwd (sandbox root).
2. **Read-only** — write-mode `open(...)` rejected regardless of path. Mode chars `w/a/x/+` (any case) on `builtins.open` / `io.open`; flags `O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND` on `os.open`.

PermissionError messages name the right alternative (`fs(action="write"|"edit"|"append")` or `bash`). Read-only opens inside the sandbox continue to work (legitimate "load CSV, compute, print result" flows are unaffected).

**Documented residual:** Python's `__closure__` introspection on `builtins.open` recovers the unwrapped open from the closure cell, bypassing both checks. Same class as the existing `ctypes` / `_io.FileIO` / `subprocess` bypasses — adversarial intent is out of scope; defense is against accident. See SECURITY_AUDIT §23.2.
