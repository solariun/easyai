# External tools — the operator-curated toolbox

> *"Some tools ship with the agent. Some tools belong to whoever runs
> the agent. The interesting question is who gets to declare which
> is which — and how to do it without giving the model a shell."*

This document is the authoritative guide to easyai's external-tools
subsystem: a directory-driven, JSON-declared way for operators (and
their teammates) to give a running easyai-server access to specific
commands without recompiling, without writing C++, and without
opening a generic shell.

If you're a developer looking for the C++ API, jump to [§7. The
library API](#7-the-library-api). If you're an operator wiring up
your first manifest, start with [§2. Quickstart (3 minutes)](#2-quickstart-3-minutes).

---

## Table of contents

1. [What this is, and why](#1-what-this-is-and-why)
2. [Quickstart (3 minutes)](#2-quickstart-3-minutes)
3. [Anatomy of a manifest](#3-anatomy-of-a-manifest)
4. [Recipes](#4-recipes)
5. [Anti-patterns — how NOT to declare a tool](#5-anti-patterns--how-not-to-declare-a-tool)
6. [Corner cases](#6-corner-cases)
7. [The library API](#7-the-library-api)
8. [Security model](#8-security-model)
9. [Sanity-check warnings](#9-sanity-check-warnings)
10. [Best practices](#10-best-practices)
11. [Collaboration workflow](#11-collaboration-workflow)
12. [Troubleshooting](#12-troubleshooting)

---

## 1. What this is, and why

easyai ships with a small set of built-in tools (`web`, `fs`,
`datetime`, `bash`, `evaluate` (legacy alias: `python3`), `memory`, `tool_lookup`). Those are the tools we
— the agent's authors — wrote, reviewed, and take responsibility
for. They are the right answer for tools that should ship in every
easyai install. (`memory` is also reachable under its legacy name
`rag` as a back-compat alias.)

But that is not the full story. **You** — the agent's operator —
have your own programs:

- internal CLIs (`/opt/internal/bin/deploy-cli`, `/usr/local/bin/our-jq-wrapper`),
- workflow helpers (a python script that summarises last week's
  Jira tickets, a bash one-liner that checks build status),
- system inspectors (`hostnamectl`, `df -h`, an internal monitoring
  query),
- domain-specific tools that only make sense in your environment.

You want the model to be able to call those. The wrong answer is
"give the model `bash` and hope" — that's a generic shell with no
schema, no description, no resource caps the model knows about. The
right answer is **the operator manifest**: a JSON file that
declares exactly which commands the model is allowed to dispatch,
what arguments each takes, what the model should call them for, and
what the resource caps are.

The manifest is read by humans (you, in code review), enforced by
the library (at load time and at every call), and consumed by the
model (the schema is what it sees in the prompt). It is the missing
**Tier 3 surface** between "ship a built-in tool" (C++ change) and
"give the model a shell" (no safety).

> **Trust direction:** the manifest is a *deploy artefact*, not a
> chat artefact. It's written by humans, code-reviewed, version-
> controlled, and shipped alongside the binary. The model never
> writes it; the model only consumes the surface it exposes. Treat
> it like `sudoers` — anyone who can write the file can make the
> model run arbitrary commands as the agent's user.

### Why a *directory* of files, not one big file

Real deployments quickly outgrow a single manifest:

- The operator has system-level tools (uptime, df, monitoring queries).
- The on-call has incident-response tools (force-restart, log-tail).
- Individual users have personal helpers (their own grep wrappers,
  a project-specific build script).

Cramming all of that into one `tools.json` is a coordination
nightmare: every change is a merge conflict, no one feels safe
deleting their teammate's tool, the file grows unbounded.

The directory model fixes this. Each `EASYAI-<name>.tools` file is
a self-contained pack. Drop a file in, the tools appear. Remove the
file, the tools go away. Per-file fault isolation: a syntax error
in `EASYAI-experimental.tools` does NOT break the rest of the load.
The agent still starts.

This makes external tools a **collaboration mechanism**: the
operator-supplied dir can hold packs from multiple authors, each
reviewed independently, each disable-able independently (rename to
`.tools.disabled` and it's silently skipped).

---

## 2. Quickstart (3 minutes)

You already have easyai-server running via the installer. The
installer created `/etc/easyai/external-tools/` for you (or your
site equivalent — check the systemd unit's `--external-tools`
argument).

**Step 1 — drop a file in the dir.**

```bash
sudo bash -c 'cat > /etc/easyai/external-tools/EASYAI-host-status.tools' <<'EOF'
{
  "version": 1,
  "tools": [
    {
      "name": "host_status",
      "description": "Return the system uptime and load averages of the host. Use when the user asks how the box is doing.",
      "command": "/usr/bin/uptime",
      "argv": [],
      "parameters": { "type": "object", "properties": {} },
      "timeout_ms": 2000,
      "max_output_bytes": 4096,
      "cwd": "$SANDBOX",
      "env_passthrough": [],
      "stderr": "discard"
    }
  ]
}
EOF
sudo chmod 640 /etc/easyai/external-tools/EASYAI-host-status.tools
sudo chown root:easyai /etc/easyai/external-tools/EASYAI-host-status.tools
```

**Step 2 — restart the server.**

```bash
sudo systemctl restart easyai-server
sudo journalctl -u easyai-server -n 30 --no-pager | grep external-tools
```

You should see:

```
easyai-server: loaded 1 external tool(s) from 1 file(s) in /etc/easyai/external-tools
```

**Step 3 — try it.**

Open the webui (or curl the API), ask: *"how long has this box been up?"*. The model dispatches `host_status`, gets a one-liner back, and answers.

That's the whole loop. From here, [the recipes section](#4-recipes) covers anything else you might want.

---

## 3. Anatomy of a manifest

A manifest file is a JSON document with two top-level fields:

```json
{
  "version": 1,
  "tools": [ /* one or more tool objects */ ]
}
```

`version` is currently always `1`. Future schema breakages will
bump it; old files will be rejected with a clear error.

Each tool object:

| Field | Required | Notes |
| --- | --- | --- |
| `name` | yes | Identifier the model uses. `^[a-zA-Z][a-zA-Z0-9_]{0,63}$`. Cannot collide with built-ins (`bash`, `evaluate`, `web`, `fs`, `memory`, `datetime`, `tool_lookup`; legacy aliases `rag` → `memory` and `python3` → `evaluate` are reserved too) or with tools declared in earlier-sorted files. |
| `description` | yes | Plain English. 1..4096 chars. **The single most important field.** Available via `tool_lookup(name="<your-tool>")` when the model needs the full manual. Mention edge cases ("returns empty when nothing matches"), expected use ("call this AFTER `web(action=\"search\")`"), and units ("returns kilobytes"). **Wire shape (Shape-C, 2026-05-26):** the per-turn `<tools>` block ships only the FIRST 120 CHARS of `description` as the wire trigger; the rest stays server-side. So put the trigger sentence FIRST — e.g. *"Lookup local DNS for a hostname (returns A/AAAA/CNAME)."* — then expand. There's no manifest field for an explicit short trigger yet; the auto-derived first-line works in practice. |
| `command` | yes | **Absolute** path to a regular, executable file. Validated via stat() + access(X_OK) at load. No PATH lookup. |
| `argv` | yes | Array of strings. Each element is either a literal (no `{` / `}`) or exactly `"{paramname}"`. Embedded placeholders (`"--flag={x}"`) are rejected — split into `["--flag", "{x}"]`. |
| `parameters` | optional | JSON-Schema-shaped: `{type:"object", properties:{...}, required:[...]}`. Types: `string` / `integer` / `number` / `boolean`. |
| `timeout_ms` | optional | Default 10000. Clamped to [100, 300000]. SIGTERM at deadline, SIGKILL after a 1s grace. |
| `max_output_bytes` | optional | Default 65536. Clamped to [1024, 4 MiB]. Excess output is silently discarded; the response notes truncation. |
| `cwd` | optional | Either an absolute path or the magic token `"$SANDBOX"` (resolves to the process's CWD at load time). Default: `"$SANDBOX"`. |
| `env_passthrough` | optional | Allowlist of parent-process env vars to inherit. **Default empty** — clean env. Add `"PATH"` / `"HOME"` only when the wrapped command needs them. |
| `stderr` | optional | `"merge"` (default — stderr captured into the model's output) or `"discard"` (stderr to /dev/null). |
| `treat_nonzero_exit_as_error` | optional | Default `true`. Set `false` for tools whose non-zero exit is informational (`pgrep`, `grep`, `diff`). |

A manifest can declare up to 128 tools. A directory can hold any
number of manifest files; only files matching `EASYAI-<at least 1
char>.tools` (top-level, exact pattern, case-sensitive) are loaded.

> **Tip — disable a file without deleting it.** Rename
> `EASYAI-foo.tools` → `EASYAI-foo.tools.disabled`. The pattern
> match fails and the file is silently ignored. Restart the server
> to pick up the change.

---

## 4. Recipes

Each recipe is a complete, deployable manifest file. Save under
`/etc/easyai/external-tools/EASYAI-<name>.tools`, restart, you're
live.

### Recipe 1 — Read-only system inspector (no parameters)

```json
{
  "version": 1,
  "tools": [
    {
      "name": "host_status",
      "description": "Return the system uptime and load averages.",
      "command": "/usr/bin/uptime", "argv": [],
      "parameters": { "type": "object", "properties": {} },
      "timeout_ms": 2000, "max_output_bytes": 4096,
      "cwd": "$SANDBOX", "env_passthrough": [], "stderr": "discard"
    },
    {
      "name": "host_disk",
      "description": "Show disk usage in human-readable form.",
      "command": "/usr/bin/df", "argv": ["-h"],
      "parameters": { "type": "object", "properties": {} },
      "timeout_ms": 3000, "max_output_bytes": 16384,
      "cwd": "$SANDBOX", "env_passthrough": []
    },
    {
      "name": "host_memory",
      "description": "Show memory usage in MiB.",
      "command": "/usr/bin/free", "argv": ["-m"],
      "parameters": { "type": "object", "properties": {} },
      "timeout_ms": 2000, "max_output_bytes": 8192,
      "cwd": "$SANDBOX", "env_passthrough": []
    }
  ]
}
```

**Teaches:** zero-parameter tools, conservative timeouts, empty env, multiple tools per file.

### Recipe 2 — Code search via ripgrep (with `--` sentinel)

```json
{
  "version": 1,
  "tools": [
    {
      "name": "code_search",
      "description": "Search the project tree for a literal string or regex. Returns file:line:match. Limit yourself to specific patterns — broad searches are slow and noisy.",
      "command": "/usr/bin/rg",
      "argv": [
        "--no-heading", "--line-number", "--max-count", "100",
        "--", "{pattern}", "."
      ],
      "parameters": {
        "type": "object",
        "properties": {
          "pattern": { "type": "string", "description": "Literal string or regex." }
        },
        "required": ["pattern"]
      },
      "timeout_ms": 15000, "max_output_bytes": 262144,
      "cwd": "$SANDBOX",
      "env_passthrough": ["HOME"],
      "stderr": "merge",
      "treat_nonzero_exit_as_error": false
    }
  ]
}
```

**Teaches:** `"--"` sentinel between flags and string placeholder
(so `pattern = "-r"` is searched-for, not interpreted as a flag);
`treat_nonzero_exit_as_error: false` because rg exits 1 when no
matches (informational, not failure); `HOME` passthrough so `rg`
can read `~/.config/ripgrep/config`.

### Recipe 3 — JSON filter via jq

```json
{
  "version": 1,
  "tools": [
    {
      "name": "json_filter",
      "description": "Apply a jq expression to a JSON file in the sandbox. The 'filter' argument is the jq expression (e.g. '.users[] | .email').",
      "command": "/usr/bin/jq",
      "argv": ["--", "{filter}", "{file}"],
      "parameters": {
        "type": "object",
        "properties": {
          "filter": { "type": "string", "description": "jq expression." },
          "file":   { "type": "string", "description": "Path to a JSON file inside the sandbox." }
        },
        "required": ["filter", "file"]
      },
      "timeout_ms": 5000, "max_output_bytes": 65536,
      "cwd": "$SANDBOX"
    }
  ]
}
```

**Teaches:** complex strings (jq filters with quotes / pipes /
parens) pass through as a single argv element — no shell, no
escaping. The library guarantees `{filter}` fills exactly one argv
slot regardless of contents.

### Recipe 4 — Internal CLI with a credential

```json
{
  "version": 1,
  "tools": [
    {
      "name": "deploy_status",
      "description": "Return the deployment status of a service from our internal control plane. Service must be one we own.",
      "command": "/opt/internal/bin/deploy-cli",
      "argv": ["status", "--", "{service}"],
      "parameters": {
        "type": "object",
        "properties": {
          "service": { "type": "string", "description": "Service name (e.g. 'billing-api')." }
        },
        "required": ["service"]
      },
      "timeout_ms": 10000, "max_output_bytes": 32768,
      "cwd": "$SANDBOX",
      "env_passthrough": ["DEPLOY_TOKEN", "HOME", "PATH"]
    }
  ]
}
```

**Teaches:** opt-in env passthrough for credentials. `DEPLOY_TOKEN`
is read from the parent's environment at every call, so rotating
it (in the systemd unit's `Environment=` line, or its
`EnvironmentFile=`) takes effect on the next call without a
restart. **Never put credentials in argv** — argv shows up in
`/proc/<pid>/cmdline`, world-readable.

### Recipe 5 — Python one-liner (advanced)

```json
{
  "version": 1,
  "tools": [
    {
      "name": "python_eval",
      "description": "Evaluate a SHORT Python expression and return its repr. Single line, no imports beyond math/datetime/json. Use for arithmetic / date math the model would otherwise get wrong.",
      "command": "/usr/bin/python3",
      "argv": ["-c", "{expr}"],
      "parameters": {
        "type": "object",
        "properties": {
          "expr": { "type": "string", "description": "Python expression. Output goes to stdout via print(repr(...))." }
        },
        "required": ["expr"]
      },
      "timeout_ms": 3000, "max_output_bytes": 16384,
      "cwd": "$SANDBOX", "env_passthrough": []
    }
  ]
}
```

**Teaches:** `-c "{expr}"` works because `{expr}` is one argv
element. **But** `python -c '<arbitrary>'` is essentially a Python
shell — anything Python can do, this tool can do (read files,
network, subprocess). The library's "no shell" guarantee doesn't
help when the wrapped *command* is itself a code interpreter.
Decide consciously whether you want this; you'll get a sanity-check
warning if you ship it (see [§9](#9-sanity-check-warnings)).

### Recipe 6 — git porcelain (integer parameter)

```json
{
  "version": 1,
  "tools": [
    {
      "name": "git_log",
      "description": "Show the last N commits of the repository in the sandbox. Format: short hash, author, ISO date, subject.",
      "command": "/usr/bin/git",
      "argv": ["log", "--max-count", "{count}", "--pretty=format:%h %an %ad %s", "--date=iso-strict"],
      "parameters": {
        "type": "object",
        "properties": {
          "count": { "type": "integer", "description": "1..100 commits to show." }
        },
        "required": ["count"]
      },
      "timeout_ms": 8000, "max_output_bytes": 131072,
      "cwd": "$SANDBOX",
      "env_passthrough": ["HOME", "PATH"]
    }
  ]
}
```

**Teaches:** integer parameters are immune to the leading-dash
gotcha (an integer can't start with `-` after JSON validation).
`HOME` for `~/.gitconfig`; `PATH` for git's sub-command lookup
(`git-log` etc).

### Recipe 7 — npm scripts

```json
{
  "version": 1,
  "tools": [
    {
      "name": "npm_test",
      "description": "Run the project's test suite via `npm test`. Returns the test runner's full output. May take up to 2 minutes.",
      "command": "/usr/bin/npm",
      "argv": ["test", "--", "--silent"],
      "parameters": { "type": "object", "properties": {} },
      "timeout_ms": 120000, "max_output_bytes": 1048576,
      "cwd": "$SANDBOX",
      "env_passthrough": ["HOME", "PATH", "NODE_ENV"],
      "stderr": "merge"
    }
  ]
}
```

**Teaches:** long-running tools at higher timeout/output caps;
project-runner conventions (`npm test --` passes args after to the
test runner).

### Recipe 8 — Cloud CLI status check

```json
{
  "version": 1,
  "tools": [
    {
      "name": "k8s_pod_status",
      "description": "Get the status of pods in a namespace. Read-only.",
      "command": "/usr/local/bin/kubectl",
      "argv": ["-n", "{namespace}", "get", "pods", "-o", "wide"],
      "parameters": {
        "type": "object",
        "properties": {
          "namespace": { "type": "string", "description": "Kubernetes namespace name." }
        },
        "required": ["namespace"]
      },
      "timeout_ms": 10000, "max_output_bytes": 131072,
      "cwd": "$SANDBOX",
      "env_passthrough": ["HOME", "KUBECONFIG"]
    }
  ]
}
```

**Teaches:** `KUBECONFIG` passthrough; tool names that include the
domain (`k8s_pod_status`) help the model pick the right one when
multiple are available.

### Recipe 9 — Internal monitoring query (HTTP via curl)

```json
{
  "version": 1,
  "tools": [
    {
      "name": "metric_lookup",
      "description": "Query our internal Prometheus for the latest value of a named metric. Returns the JSON the API returned.",
      "command": "/usr/bin/curl",
      "argv": [
        "-fsS", "--max-time", "10",
        "-G", "https://prom.internal/api/v1/query",
        "--data-urlencode", "query={query}"
      ],
      "parameters": {
        "type": "object",
        "properties": {
          "query": { "type": "string", "description": "Promql expression, e.g. 'up{job=\"api\"}'." }
        },
        "required": ["query"]
      },
      "timeout_ms": 15000, "max_output_bytes": 131072,
      "cwd": "$SANDBOX",
      "env_passthrough": []
    }
  ]
}
```

**Teaches:** wrapping HTTP-based tools using `curl` rather than
implementing them in C++. `--data-urlencode` ensures the query
string is properly encoded; the model's value goes into one argv
slot, curl handles the encoding.

### Recipe 10 — File classifier

```json
{
  "version": 1,
  "tools": [
    {
      "name": "file_type",
      "description": "Identify the type of a file using libmagic. Useful before reading something you're not sure is text.",
      "command": "/usr/bin/file",
      "argv": ["--", "{path}"],
      "parameters": {
        "type": "object",
        "properties": {
          "path": { "type": "string", "description": "Absolute or relative path." }
        },
        "required": ["path"]
      },
      "timeout_ms": 3000, "max_output_bytes": 4096,
      "cwd": "$SANDBOX"
    }
  ]
}
```

**Teaches:** the `--` sentinel works for any binary that accepts
options. `file -- foo.txt` works the same as `file foo.txt` for
"normal" filenames AND is safe when filenames start with `-`.

---

## 5. Anti-patterns — how NOT to declare a tool

### ❌ The generic shell wrapper

```json
{
  "name": "shell",
  "command": "/bin/sh",
  "argv": ["-c", "{cmd}"]
}
```

This is functionally equivalent to `--allow-bash`. The structural
"no shell" safety the manifest gives you is gone the moment you
hand the model a shell command line. You'll get a sanity-check
warning at load: *"wraps a shell with -c {placeholder}; this is
functionally equivalent to --allow-bash."*

If you genuinely need a shell, use `--allow-bash` (which is honest
about being unsafe) or wrap the *specific* shell behaviour you
need in a script (see Recipe 4).

### ❌ Relative command paths

```json
{ "command": "uname" }
```

Rejected at load. Anyone who can drop a binary into the agent's
PATH (`~/.local/bin/`, a writable container `/tmp`, …) hijacks
your tool. Use `/usr/bin/uname`.

### ❌ Embedded placeholders

```json
{ "argv": ["--filter={query}"] }
```

Rejected at load. Looks innocent, invites a per-element
interpolator that's the same trap as writing a "safe shell." Split:
`["--filter", "{query}"]`.

### ❌ Credentials in argv

```json
{ "argv": ["--token", "{token}"] }
```

argv lives in `/proc/<pid>/cmdline`, which is world-readable on
every standard distro. Use `env_passthrough` instead — credentials
in env vars are at least kernel-private to the process tree.

### ❌ Lazy descriptions

```json
{ "name": "deploy", "description": "deploy a thing" }
```

The description is how the model picks WHICH tool to call. A vague
description means the model misuses the tool. Spend real time here:
mention edge cases, expected sequencing, units, side effects.

### ❌ Default-everything timeouts and output caps

```json
{ "name": "ping", "command": "/bin/ping" }
```

The default timeout is 10 s; the default output cap is 64 KiB. For
a sub-second status check this wastes 9.99 s of the agent's
patience on a hung command. Match `timeout_ms` and
`max_output_bytes` to the actual expected behaviour.

### ❌ `LD_PRELOAD` / `LD_LIBRARY_PATH` in env_passthrough

```json
{ "env_passthrough": ["LD_PRELOAD", "PATH"] }
```

You'll get a sanity-check warning at load. There's almost never a
legitimate reason to let the model influence dynamic linking.

### ❌ World-writable manifest

If you `chmod 666 EASYAI-foo.tools`, you'll get a sanity-check
warning. Anyone with write access to that file can register
additional tools that survive the next restart. Keep it `chmod 640
root:easyai`.

### ❌ Daemonising commands

```json
{ "name": "start_my_daemon", "command": "/usr/local/bin/launch-daemon" }
```

Daemons fork and the parent exits. The library reaps the parent
immediately; the daemon survives but inherits stdout connected to a
pipe with no reader — first write will SIGPIPE and the daemon
dies. Don't expose tools that daemonise. If the goal is *"start a
service and forget it"*, do that out of band (`systemctl start
…`) and expose a query-only tool to the agent.

### ❌ Interactive tools (`vim`, `nano`, `more`)

stdin is closed; interactive tools detect EOF and either exit
strangely or hang until timeout. Don't expose them.

### ❌ One mega-manifest

```
/etc/easyai/external-tools/
└── EASYAI-everything.tools     # 80 tools, 1500 lines
```

Hard to review, hard to disable a piece, hard to attribute. Split:
`EASYAI-system.tools`, `EASYAI-deploy.tools`,
`EASYAI-user-gustavo.tools`. Each pack has a clear owner, a clear
review path, and can be disabled (rename to `.disabled`) without
touching others.

### ❌ Stateful tools

```json
{ "name": "db_query", "command": "/usr/bin/psql", "argv": ["-c", "{sql}"] }
```

Each call is a fresh `fork+exec`, a fresh psql connection, a fresh
TCP handshake. For workloads that need a connection pool / session
state, an external-tool-as-subprocess is the wrong shape. Write a
C++ tool instead with a captured `std::shared_ptr` to a state
object — that's a Tier-4 escape hatch (`Tool::builder()`).

---

## 6. Corner cases

| Situation | What happens |
| --- | --- |
| `--external-tools` points to an empty dir | Silent. Zero tools loaded. Normal state. |
| Dir contains no `EASYAI-*.tools` files (other files only) | Silent. Other files ignored. Normal state. |
| Dir doesn't exist | Error logged to stderr/journalctl. Agent starts with built-ins only. |
| Dir is a regular file, not a dir | Error logged. Agent starts with built-ins only. |
| Manifest fails JSON parse | Per-file error logged with line number. THAT file is skipped; other files still load. |
| Manifest declares a name that collides with a built-in | Per-file error; file skipped. |
| Two manifest files declare the same tool name | First (alphabetically-earlier) file wins. Second file gets a "duplicate name" error and is skipped, but its OTHER (non-conflicting) tools are NOT loaded either — the file is all-or-nothing. |
| Manifest binary doesn't exist when loading | Per-file error logged. File skipped. |
| Binary removed AFTER successful load | Tool call returns `exit=127` with `external_tool: execve failed`. |
| Binary on a stalled NFS mount | Call blocks until `timeout_ms`, then SIGTERM/SIGKILL. |
| Required parameter missing in model's call | `ToolResult::error` before `fork()`. Model sees the error in its tool result and can recover. |
| Optional parameter missing | Empty string substituted into the argv slot. |
| Extra keys in model's JSON arguments | Silently ignored. |
| Model sends `"true"` (string) for a `boolean` param | Validation rejects: `expected boolean`. |
| Model sends `1.5` for an `integer` param | Validation rejects: `expected integer`. |
| Model sends `NaN` / `Infinity` for a `number` param | Validation rejects: `must be a finite number`. |
| Manifest is edited while server is running | No effect. Restart `easyai-server` to pick up. |
| Server runs as `easyai`, manifest in `/home/me/foo/EASYAI-x.tools` | Server can't read it (probably). Move to `/etc/easyai/external-tools/` with proper perms. |
| Manifest ≥ 1 MiB | Rejected: `manifest exceeds 1048576 bytes`. Split into multiple files. |
| Manifest declares > 128 tools | Rejected. Split into multiple files. |
| `env_passthrough` includes a var that doesn't exist in the parent | Silently skipped. Subprocess just doesn't see that var. |
| `env_passthrough` value is multi-megabyte | Skipped (capped at 4 KiB). Subprocess gets clean env for that var. |
| Subprocess outputs more than `max_output_bytes` | Excess silently discarded (drained, not buffered — child stays unblocked). Response notes `[truncated at N bytes]`. |
| Subprocess writes 1 KB then sleeps 30 minutes | Output captured immediately; SIGTERM on `timeout_ms`, SIGKILL after 1s grace. |
| Two concurrent calls to the same tool | Each `fork`s its own subprocess. No shared state. The wrapped command is responsible for its own concurrency. |
| Agent (parent) crashes mid-call | On Linux, `PR_SET_PDEATHSIG(SIGKILL)` ensures the subprocess dies with the agent. No orphans reparented to PID 1. |
| Subdirectory inside `--external-tools` dir | Skipped (only top-level scanned). Use a subdir named `archive/` or `disabled/` to keep stuff out. |

---

## 7. The library API

```cpp
#include <easyai/external_tools.hpp>

// Single-file: load one manifest, all-or-nothing.
struct ExternalToolsLoad {
    std::vector<easyai::Tool> tools;
    std::vector<std::string>  warnings;   // sanity-check observations
    std::string               error;      // populated iff load failed
};
ExternalToolsLoad load_external_tools_from_json(
    const std::string &              json_path,
    const std::vector<std::string> & reserved_names);

// Directory: load every EASYAI-*.tools, per-file fault isolation.
struct ExternalToolsDirLoad {
    std::vector<easyai::Tool> tools;
    std::vector<std::string>  warnings;
    std::vector<std::string>  errors;        // per-file load errors (file skipped)
    std::vector<std::string>  loaded_files;  // files contributing to `tools`
    std::vector<std::string>  skipped_files; // files skipped by name pattern
};
ExternalToolsDirLoad load_external_tools_from_dir(
    const std::string &              dir,
    const std::vector<std::string> & reserved_names);
```

Typical wiring (see `src/backend.cpp`):

```cpp
std::vector<std::string> reserved;
for (const auto & t : engine.tools()) reserved.push_back(t.name);

auto loaded = easyai::load_external_tools_from_dir(dir, reserved);

// Errors: always emit (manifests are operator-supplied; broken
// ones must be fixed).
for (const auto & e : loaded.errors)   std::fprintf(stderr, "error: %s\n", e.c_str());
// Warnings: emit unless the caller asked for quiet mode.
for (const auto & w : loaded.warnings) std::fprintf(stderr, "warn: %s\n", w.c_str());

for (auto & t : loaded.tools) engine.add_tool(t);
```

Both functions are pure with respect to global state — they parse,
validate, and return. Calling them does NOT spawn any process.
Spawn happens at tool-call time, inside the handler captured in
each returned `Tool`.

For programmatic testing of a single manifest:

```cpp
auto loaded = easyai::load_external_tools_from_json("mytools.json", {});
assert(loaded.error.empty());
assert(loaded.tools.size() == kExpectedToolCount);
```

---

## 8. Security model

The threat model is **operator → model**. The manifest is an
operator artefact; the model only fills in parameter values. The
library enforces:

### At load time

1. **Absolute command path.** stat() + S_ISREG + access(X_OK).
2. **Manifest is a regular file.** stat-first, rejects FIFOs, dirs, devices.
3. **Whole-element argv placeholders.** `"{x}"` accepted; `"--flag={x}"` rejected.
4. **No name collisions.** With built-ins; with previously-loaded files.
5. **Hard caps.** Manifest size 1 MiB, 128 tools, 32 params/tool, 256 argv elements, 4 KiB per arg, 16 env vars, [100 ms, 5 min] timeout, [1 KiB, 4 MiB] output cap.

### At call time

1. **Schema-validated arguments.** Type errors rejected before fork().
2. **Non-finite numbers rejected.** `std::isfinite()` on the parsed double.
3. **No shell.** Spawn is `fork()` + `execve()`. Never `/bin/sh -c`.
4. **Closed stdin.** Child gets `/dev/null` on fd 0.
5. **Bounded fd inheritance.** Loop closes fds 3..65536 in the child between fork and execve. `RLIMIT_NOFILE = unlimited` does NOT defeat this.
6. **Linux PDEATHSIG.** Agent crash → kernel kills the subprocess.
7. **Process-group lifetime.** SIGTERM the group on timeout, SIGKILL after 1 s grace.
8. **Output capped.** Drained continuously; excess discarded.
9. **Clean env.** Only listed `env_passthrough` vars inherit; values capped at 4 KiB each.

### What this is NOT

- **Not a sandbox.** External tools run with the agent's full uid/gid.
- **Not a process supervisor.** No restart, no PID files, no log rotation.
- **Not async.** A tool call blocks the agent loop until it returns or times out.
- **Not stateful.** Each call gets a fresh subprocess.

For deployments needing OS-level isolation, run easyai-server inside
a container / firejail / unprivileged user.

Full audit and residual-risk discussion in `SECURITY_AUDIT.md` §16.

---

## 9. Sanity-check warnings

The loader runs a security audit on each parsed manifest entry and
emits human-readable warnings. The tool still loads — the warning
is informational, intended for the operator's startup log.

| Warning class | What triggers it | What it usually means |
| --- | --- | --- |
| Shell wrapper detected | `command` is a shell binary (sh, bash, zsh, dash, ksh, ash, fish) AND argv contains `-c` followed by a placeholder | You've reintroduced shell-injection surface. Consider declaring focused tools instead. |
| Shell binary as command (no -c) | `command` is a shell binary but argv shape isn't the standard `-c {placeholder}` | argv may still reach shell parsing; review. |
| Dynamic-linker env passthrough | `env_passthrough` includes `LD_PRELOAD`, `LD_LIBRARY_PATH`, `LD_AUDIT`, `DYLD_INSERT_LIBRARIES`, `DYLD_LIBRARY_PATH` | Almost never the right thing. Remove unless documented reason. |
| World-writable command | wrapped binary has `S_IWOTH` mode bit | Anyone with shell on the host can replace the binary. `chmod o-w`. |
| World-writable manifest | the `.tools` file has `S_IWOTH` mode bit | Anyone with write access can register new tools. `chmod o-w`. |

**Quiet mode** (`easyai-cli -q`, `easyai-local -q`): warnings are
suppressed; errors are still emitted. easyai-server always logs
both (it's a daemon — you read the journal anyway).

If you need to silence a specific warning permanently, fix the
underlying issue. There is no "ack" mechanism — that would just
mean inheriting your own old technical debt later.

---

## 10. Best practices

### Naming

- **Tool names match domain vocabulary.** `git_log`, not `vcs_history`. `k8s_pod_status`, not `cluster_check`. The model's training data uses the same vocabulary the operator does.
- **File names match the pack's purpose.** `EASYAI-system.tools`, `EASYAI-deploy.tools`, `EASYAI-user-gustavo.tools`. One purpose per file.
- **Disable, don't delete.** `EASYAI-experimental.tools.disabled` keeps the file as documentation that the experiment was tried.

### Security

- **Default `env_passthrough` to `[]`.** Add only what fails without it.
- **Never put secrets in argv.** Use env passthrough.
- **`chmod 640 root:easyai`** on every `.tools` file. The dir itself `chmod 750`.
- **Run easyai-server as a dedicated unprivileged user** (the installer does this). External tools inherit that user's uid; less surface to abuse.
- **Insert `"--"` before string placeholders** for any binary that accepts options.
- **Lock cwd with `"$SANDBOX"`** unless the tool genuinely needs a different working directory.

### Ergonomics

- **Spend real time on `description`.** It's the highest-leverage field. The model reads the first 120 chars on every turn (Shape-C wire trigger) and the full body via `tool_lookup(name="<your-tool>")` when it needs detail. Put the trigger sentence first — second sentence onwards is for the manual lookup.
- **Match `timeout_ms` and `max_output_bytes` to the worst plausible case** — not a global default.
- **Set `treat_nonzero_exit_as_error: false`** for tools whose non-zero is informational (`pgrep`, `grep`, `diff`).
- **Use multiple parameters instead of one composite parameter.** `service` and `region` separately is easier for the model than a `service_qualifier` blob.

### Deploy

- **Version-control your `EASYAI-*.tools` files.** Treat them like any other config artifact. Code review them.
- **Validate before deploy.** `easyai-local --no-tools --external-tools /path/to/dir 2>&1 | grep -E "(error|warn)"` exercises the loader without starting the model. Wire into CI.
- **Restart picks up changes.** `sudo systemctl restart easyai-server` after dropping a new file.
- **Keep packs small.** ~5–15 tools per file. Easier review, easier rollback.

---

## 11. Collaboration workflow

External tools are designed to scale across teams. Pattern that works:

### 1. The shared dir

`/etc/easyai/external-tools/` is owned by `root:easyai` (the server
group). Mode 750. Every operator with sudo can drop files; only
the server can read them.

### 2. One file per author / domain

```
/etc/easyai/external-tools/
├── EASYAI-system.tools          # owned by sysadmin, system inspectors
├── EASYAI-deploy.tools          # owned by SRE, deploy CLIs
├── EASYAI-monitoring.tools      # owned by SRE, prometheus queries
├── EASYAI-user-gustavo.tools    # personal helpers
├── EASYAI-on-call.tools         # incident-response tooling
└── README.md                    # pointer to this doc
```

Each file has a clear owner. PRs against the file route to that
owner. Disable-able independently.

### 3. Code review checklist

When reviewing a new `.tools` file:

- [ ] Does every `description` mention WHEN to call (and ideally
      WHEN NOT to)?
- [ ] Is `command` an absolute path?
- [ ] Are all string-typed placeholders preceded by `"--"`?
- [ ] Is `env_passthrough` the minimum set?
- [ ] Is `timeout_ms` plausible for this tool? (Not just the default.)
- [ ] Is `max_output_bytes` plausible? (Not just the default.)
- [ ] Does `treat_nonzero_exit_as_error` match the tool's exit-code conventions?
- [ ] Any sanity-check warnings expected? (Shell wrapper, LD_*, etc.)
- [ ] If the tool is dangerous, is it locked to a specific cwd or env?
- [ ] Does the tool name avoid conflicting with built-ins or other packs?

### 4. CI validation

```sh
# In CI, against a checked-in copy of the manifests:
for f in deploy/external-tools/EASYAI-*.tools; do
    easyai-local --no-tools --external-tools "$(dirname "$f")" 2>&1 \
        | grep -E "error" && exit 1
done
echo "all manifests valid"
```

The `--no-tools` flag is a separate easyai concept (don't register
built-ins); combined with `--external-tools` it gives you a
load-only smoke test.

### 5. Promotion path

1. Author writes `EASYAI-experiment.tools` locally.
2. Tests with `easyai-local --external-tools .` against a small model.
3. PR to the deploy repo.
4. Code review against the checklist above.
5. Merge → CI runs the validation step → systemd push to the AI box.
6. `sudo systemctl restart easyai-server` on the box.

---

## 12. Troubleshooting

### "external-tools dir does not exist"

The path passed to `--external-tools` doesn't exist. Check the
systemd unit's `ExecStart`:

```sh
systemctl cat easyai-server | grep external-tools
```

Compare with what's actually on disk.

### "manifest is not a regular file"

You pointed the loader at a directory or a symlink chain ending at
something weird. Make sure each `.tools` entry is a plain file.

### "manifest exceeds 1048576 bytes"

The 1 MiB cap is hit. Real manifests are tiny — if you're hitting
this, you've probably added a giant inline string or have base64
data where it shouldn't be. Refactor.

### "<name>.tools: tools[N] is not a valid tool name"

Tool names must match `^[a-zA-Z][a-zA-Z0-9_]{0,63}$`. No hyphens,
no leading digits, no spaces. `git_log` ✓, `git-log` ✗.

### "<name>.tools: collides with a built-in or already-registered tool"

You're trying to use a name that's either a built-in (`bash`,
`read_file`, …) or already declared in an earlier-sorted file in
the same dir. Rename your tool.

### "<name>.tools: argv[N]: braces are only allowed as full placeholders"

Embedded placeholder. `"--flag={x}"` → split into `["--flag", "{x}"]`.

### "<name>.tools: command: must be an absolute path"

Add the full path: `/usr/bin/uname`, not `uname`.

### "<name>.tools: command: not executable for the current user"

Either chmod the binary executable, or fix the agent user's
permissions on the file.

### "wraps a shell with -c {placeholder}; this is functionally equivalent to --allow-bash"

Sanity warning, not an error. Either accept it (you really do want
a shell — fine) or refactor into a focused tool.

### "env_passthrough includes \"LD_PRELOAD\""

Sanity warning, not an error. Almost certainly remove it.

### "command \"…\" is world-writable"

Sanity warning. `chmod o-w /path/to/binary`.

### "manifest file \"…\" is world-writable"

Sanity warning. `chmod o-w /etc/easyai/external-tools/EASYAI-*.tools`.

### Tool loaded but model never calls it

Description problem. The model decides based on the description
text. Make it concrete: include trigger phrases ("when the user
asks about X"), edge cases, sequencing hints. Test by asking the
model exactly what scenarios that tool would be appropriate for.

### Tool calls fail with `exit=127`

`execve failed` — usually because the binary path no longer
resolves at call time (uninstalled, NFS mount stalled, chmod -x).
Check `which <command>` from the agent's user.

### Tool calls fail with `exit=signal:13`

`SIGPIPE`. The wrapped command tried to write more output than the
pipe could buffer AND wasn't draining. Most often happens to
daemonising commands (parent exits, the orphaned daemon writes to
a now-closed pipe). Don't expose daemonising commands.

### Tool calls fail with `exit=-1 [killed: timeout after Nms]`

Tool exceeded `timeout_ms`. Either raise the cap (up to 5 min) or
investigate why the wrapped command is slow.

---

*See also:* [`easyai-server.md`](easyai-server.md) (full chat-server
config; the `external_tools` INI key is wired identically there),
[`easyai-mcp-server.md`](easyai-mcp-server.md) (standalone MCP
daemon — `EASYAI-*.tools` packs are loaded the same way and
expose the same fork+execve hardening to thousands of concurrent
clients), [`manual.md`](manual.md) §3.3.4-3.3.5 (C++ API reference),
[`design.md`](design.md) §5f (why the subsystem is shaped this way),
[`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) §16 (audit detail),
[`AI_TOOLS.md`](AI_TOOLS.md) ch. 21 (vendor-neutral background).
