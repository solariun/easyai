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

## memory_append Tool Behavior

| Title exists? | Behavior | Return message |
|--------------|----------|----------------|
| Yes | Append content after `---` separator | `updated "title.md" (+N B → M B total, K keywords)` |
| No (keywords given) | Create new memory (save semantics) | `new memory saved as "title.md" (N bytes, K keywords)` |
| No (no keywords) | Error | Explains keywords are required for new memories |
| Fixed (`fix-easyai-*`) | Error | Immutable, cannot append |
