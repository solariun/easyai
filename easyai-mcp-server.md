# easyai-mcp-server — standalone Model Context Protocol provider

> **A model-free MCP server for thousands of parallel clients.** Same
> tool catalogue `easyai-server` exposes (built-ins + the `knowledge_*`
> tools + operator-defined `EASYAI-*.tools`), wired through the same
> lib-level factories — but no GGUF loaded, no `/v1/chat/completions`,
> no webui. Just `POST /mcp`, sized for high concurrency and fail-fast
> backpressure when the host runs out of headroom.

> **Stack it under an `easyai-server`.** As of the 2026-05-01 update,
> `easyai-server` is also an MCP **client**: pass
> `--mcp http://this-host:8089` (and `--mcp-token` if you've set
> `[MCP_USER]`) on the chat server and the `easyai-mcp-server`'s
> entire tool catalogue gets merged into the chat agent's toolbox.
> Pattern: many lightweight chat servers, one shared
> `easyai-mcp-server` exposing the `knowledge_*` tools + your
> operator-defined tool packs centrally. See [`MCP.md`](MCP.md) §9.5.

---

## Table of contents

1. [Configuration — `/etc/easyai/easyai-mcp.ini`](#1-configuration--etceasyaieasyai-mcpini)
2. [Command-line flags](#2-command-line-flags)
3. [API endpoints](#3-api-endpoints)
4. [Concurrency model](#4-concurrency-model)
5. [Tool catalogue](#5-tool-catalogue)
6. [Authentication](#6-authentication)
7. [Operating the server](#7-operating-the-server)
8. [Hardening / security](#8-hardening--security)
9. [Choosing between easyai-server and easyai-mcp-server](#9-choosing-between-easyai-server-and-easyai-mcp-server)
10. [Roadmap](#10-roadmap)
11. [Cross-references](#11-cross-references)

---

## 1. Configuration — `/etc/easyai/easyai-mcp.ini`

Same INI overlay machinery as `easyai-server`. Every CLI flag has a
matching INI entry; precedence is **CLI > INI > hardcoded default**.
Missing file = all-defaults (which means MCP open, since `[MCP_USER]`
is empty).

### File location

| Path | Notes |
| --- | --- |
| `/etc/easyai/easyai-mcp.ini` | Default. Separate from `easyai.ini` so a chat server and an MCP server can coexist on one host with their own knobs. |
| `<other path>` | Pass `--config /path/to.ini` on the binary's command line. |

### Sections

| Section | Purpose | Status |
| --- | --- | --- |
| `[SERVER]` | HTTP layer, paths, tool gating, auth posture, concurrency | active |
| `[MCP_USER]` | Bearer-token auth for `/mcp` (one user per line) | active |

The `[ENGINE]` section recognised by `easyai-server` is **deliberately
absent** here — there is no engine. Including it does no harm; the
binary doesn't read it.

### Format quick reference

```ini
# comments start with # or ;
; both work

[Section]
key = value
key2 = "value with spaces"        ; quotes optional
empty_key =
```

- Whitespace around `=`, section names, keys, and values is trimmed.
- Boolean values accept: `on` / `off`, `true` / `false`, `yes` / `no`,
  `1` / `0`, `enable` / `disable`, `enabled` / `disabled`.
- Surrounding double-quotes on values are stripped.
- Duplicate keys within a section: last value wins.
- Missing file: server starts with hardcoded defaults.

### `[SERVER]`

The HTTP layer, paths, tool gating, concurrency, MCP auth.

| Key | Type | CLI equivalent | Default | Notes |
| --- | --- | --- | --- | --- |
| `host` | string | `--host` | `127.0.0.1` | Bind address. `0.0.0.0` to listen on every interface. |
| `port` | int | `--port` | `8089` | TCP port. (Different default from `easyai-server`'s 8080 so both can coexist on one host.) |
| `name` | string | `-n`, `--name` | `easyai-mcp-server` | Server identity surfaced on `/health` and the MCP `initialize` response. Override on multi-server fleets so MCP clients can identify each instance. |
| `max_body` | int | `--max-body` | `1048576` (1 MiB) | Max HTTP request body. MCP requests are tiny; 1 MiB is generous. |
| `sandbox` | path | `--sandbox` | (none) | Root directory for `bash` / the unified `fs` tool / external-tools `$SANDBOX` placeholder. The binary `chdir`s into `<dir>` at startup so the model's relative paths land there. |
| `allow_fs` | bool | `--allow-fs` | `off` | Register the unified `fs` tool (action=`read` / `write` / `list` / `glob` / `grep` / `check_path` / `cwd` / `sandbox`), scoped to the sandbox. |
| `allow_bash` | bool | `--allow-bash` | `off` | Register the `bash` tool. **Not** a hardened sandbox — runs with this process's user privileges. Per-call timeouts + output cap remain. |
| `allow_python` | bool | (no `--allow-python`; `--no-python` flips off) | `on` | Register the compute tool (model-facing name **`evaluate`**, runtime `python3`) — runs snippets via `python3 -I -S -E -c <code>`. Renamed from `python3` → `evaluate` on 2026-05-26; legacy name still aliased. **Defaults ON**, auto-registers when `--sandbox` is set or `--allow-bash` is on. Isolated stdlib-only interpreter (no PYTHON* env, no site-packages, no cwd on `sys.path`). **READ-ONLY disk surface**: the Python preamble rejects any path outside the sandbox AND any write-mode `open()` regardless of path. The same policy is echoed in the `initialize.instructions` response field so MCP clients can surface it to their model. Defense-in-depth, **not** a hardened sandbox — `import os` / `import socket` / `import subprocess` / `import ctypes` all still work. Same per-call timeout + output cap as bash. Pass `--no-python` (or `[SERVER] allow_python = off`) to skip registration. |
| `load_tools` | bool | `--no-tools` (negative) | `on` | Master switch for the built-in toolbelt. Set `off` to register zero default tools and rely on `external_tools` + knowledge tools only. |
| `external_tools` | path | `--external-tools` | (none) | Directory of `EASYAI-*.tools` manifests. Per-file fault isolation. See [`EXTERNAL_TOOLS.md`](EXTERNAL_TOOLS.md). |
| `memory` | path | `--memory` | (none) | Directory of knowledge entries — enables the seven split `knowledge_*` tools (a passive RAG technique). The legacy key `rag` (CLI `--RAG`) is still read for back-compat. See [`RAG.md`](RAG.md). |
| `api_key` | string | `--api-key` | (none — open) | Bearer token for `/health`, `/metrics`, `/v1/tools`. `/health` is intentionally NOT gated even when set, so liveness probes don't need a credential. The `/mcp` endpoint uses `[MCP_USER]` instead. |
| `mcp_auth` | enum | (no CLI; `--no-mcp-auth` overrides) | `auto` | `auto` (Bearer required iff `[MCP_USER]` non-empty), `on` (force require — invalid against an empty table), `off` (force open). |
| `threads` | int | `-t`, `--threads` | `256` | cpp-httplib worker pool size. Each worker handles one request at a time; excess queues. |
| `max_concurrent_calls` | int | `--max-concurrent-calls` | `256` | In-flight `tools/call` cap. Returns 503 with `Retry-After: 1` when saturated. Cheap methods (`initialize`, `tools/list`, `ping`, `notifications/*`) bypass the cap. |
| `metrics` | bool | `--metrics` | `off` | Expose Prometheus `/metrics` (gated by `api_key` when set). |
| `verbose` | bool | `-v`, `--verbose` | `off` | Per-dispatch stderr log line. |

### `[MCP_USER]`

Bearer-token authentication for `POST /mcp`. Identical shape to
`easyai-server`'s table. Each line registers one user:

```ini
[MCP_USER]
gustavo  = abcdef0123456789...   ; openssl rand -hex 32
ci       = different-strong-token
claude   = token-for-claude-desktop
```

- The username on the matching line is logged per request:
  `[mcp] request from user 'gustavo'`. The token is never logged.
- If the section is missing or empty, `/mcp` is open (subject to
  `[SERVER] mcp_auth` and `--no-mcp-auth`).
- Generate strong tokens with `openssl rand -hex 32`.
- Restart to pick up changes — there is no in-memory survival of old
  tokens after restart.

### Worked examples

#### Minimal local-dev INI

```ini
[SERVER]
host   = 127.0.0.1
port   = 8089
memory = /home/me/.easyai/rag
```

Localhost only, knowledge tools enabled, no fs/bash, no auth. Fits on a laptop.

#### Production high-concurrency deployment

```ini
[SERVER]
host                  = 0.0.0.0
port                  = 80
name                  = easyai-mcp.prod-1
sandbox               = /var/lib/easyai-mcp/workspace
external_tools        = /etc/easyai-mcp/external-tools
memory                = /var/lib/easyai-mcp/rag
allow_fs              = on
allow_bash            = off
threads               = 512
max_concurrent_calls  = 384
max_body              = 1048576
api_key               = REPLACE-WITH-OPENSSL-RAND-HEX-32
mcp_auth              = on
metrics               = on
verbose               = off

[MCP_USER]
gustavo  = REPLACE-WITH-OPENSSL-RAND-HEX-32
claude   = ANOTHER-STRONG-TOKEN
ci       = THIRD-TOKEN
```

512 worker threads, 384 in-flight `tools/call` cap (leaves 128
threads for cheap methods + one-off probes), Bearer required on every
endpoint. fs_* enabled, bash deliberately off (this is multi-tenant —
prefer focused external-tools manifests).

#### Disable MCP auth temporarily for one-off debug

```
easyai-mcp-server --config /etc/easyai/easyai-mcp.ini --no-mcp-auth
```

CLI flag wins over `[SERVER] mcp_auth = on` and over `[MCP_USER]`.

---

## 2. Command-line flags

```
easyai-mcp-server [options]
```

No required arguments. Pass `--help` for the live list.

| Flag | INI key | Default | Notes |
| --- | --- | --- | --- |
| `--config <path>` | (n/a) | `/etc/easyai/easyai-mcp.ini` | Override the default INI path. |
| `--host <addr>` | `host` | `127.0.0.1` | Bind address. |
| `--port <n>` | `port` | `8089` | TCP port. |
| `-n`, `--name <id>` | `name` | `easyai-mcp-server` | Server identity. |
| `--max-body <bytes>` | `max_body` | `1048576` | Max request body. |
| `--sandbox <dir>` | `sandbox` | (none) | Root for `fs` / `bash` / `$SANDBOX`. |
| `--allow-fs` | `allow_fs` | `off` | Register the unified `fs` tool. |
| `--allow-bash` | `allow_bash` | `off` | Register `bash`. |
| `--no-python` | `allow_python = off` | python3 on | Drop the default-on compute tool (model-facing `evaluate`, runtime `python3`). |
| `--no-tools` | `load_tools = off` | n/a | Skip built-in toolbelt entirely. |
| `--external-tools <dir>` | `external_tools` | (none) | Load `EASYAI-*.tools`. |
| `--memory <dir>` | `memory` | (none) | Enable the seven split `knowledge_*` tools. `--RAG` is still accepted as a back-compat alias. |
| `--api-key <token>` | `api_key` | (none — open) | Bearer for `/metrics`, `/v1/tools`. |
| `--no-mcp-auth` | (n/a) | `false` | Force `/mcp` open. Emergency override. |
| `-t`, `--threads <n>` | `threads` | `256` | cpp-httplib worker pool size. |
| `--max-concurrent-calls <n>` | `max_concurrent_calls` | `256` | In-flight `tools/call` cap. |
| `--metrics` | `metrics` | `off` | Expose Prometheus `/metrics`. |
| `-v`, `--verbose` | `verbose` | `off` | Per-dispatch stderr log line. |
| `-h`, `--help` | (n/a) | n/a | Print usage and exit. |

---

## 3. API endpoints

| Verb | Path | Auth | Notes |
| --- | --- | --- | --- |
| GET | `/health` | (open) | `{status, server, tools, mcp_auth, compat:{...}, concurrency:{in_flight, max_concurrent_calls}, counters:{requests, tool_calls, errors, rejected}}`. Always open so liveness probes work without credentials. |
| GET | `/metrics` | api_key | Prometheus exposition (only when `--metrics` is on). Counters: `easyai_mcp_requests_total`, `easyai_mcp_tool_calls_total`, `easyai_mcp_errors_total`, `easyai_mcp_rejected_total`; gauges: `easyai_mcp_in_flight`, `easyai_mcp_max_concurrent_calls`, `easyai_mcp_tools_registered`. |
| GET | `/v1/tools` | api_key | Tool catalogue. Each entry: `{name, description, short_description}` — `description` is the full manual (also surfaced via MCP `tools/list`), `short_description` is the one-line trigger used by `easyai-cli` for its prompt prefix. Pre-Shape-C clients see only `description` — backward compatible. |
| POST | `/mcp` | `[MCP_USER]` | JSON-RPC 2.0 dispatcher. Methods: `initialize`, `tools/list`, `tools/call`, `ping`, `notifications/*`. |
| GET | `/mcp` | (open) | `405 Method Not Allowed`. Reserved for a future SSE notification stream. |

### `POST /mcp` example

```sh
# initialize  (the response includes an `instructions` free-text
# field carrying the closed-set rule + write/edit policy — well-
# behaved MCP clients inject this into their model's system prompt)
curl -fsS http://localhost:8089/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"my-app","version":"0.1"}}}'

# list tools
curl -fsS http://localhost:8089/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  | jq '.result.tools[] | .name'

# call a tool
curl -fsS http://localhost:8089/mcp \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer YOUR-TOKEN' \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call",
       "params":{"name":"knowledge_search","arguments":{"keywords":["user-prefs"]}}}'
```

### `initialize.instructions` — closed-set + write/edit policy

The MCP `initialize` response includes a free-text `instructions`
field (spec-defined hint to the client's model). easyai-mcp-server
populates it with:

1. The **closed-set rule**: "Call ONLY tools listed in `tools/list`.
   Do NOT invent paraphrases (`read_file` is not `fs`; `shell` is
   not `bash`)."
2. The **write/edit policy**, keyed off which tools are registered:
   - `evaluate` (Python 3 sandbox under the hood) is COMPUTE-only,
     READ-ONLY on disk, FORBIDDEN to do subprocess/network/ctypes.
   - The filesystem tool(s) listed in `tools/list` are the
     authoritative writer (`fs(action=...)` in Unified mode, the
     `fs_write`/`fs_edit`/... family in Split mode).
   - `bash` is allowed to write files (redirects, `sed -i`, `mkdir`).
3. The recovery rule: "On the first `PermissionError` from
   `evaluate`, switch to the filesystem tool / `bash` — do not retry
   the evaluate call."

Well-behaved MCP clients (Claude Desktop, Cursor) inject this text
into the client model's system prompt; non-conforming clients ignore
it harmlessly. The text changes shape automatically when the tool set
changes (e.g. with `--no-python` the python paragraph is omitted).

Per-client connection guides for Claude Desktop / Cursor / Continue
are identical to the chat server's — see [`MCP.md`](MCP.md) §4–7;
just point at port 8089 (or whatever you configured) instead of 80 /
8080.

### `GET /health` example response

```json
{
  "status": "ok",
  "server": "easyai-mcp-server",
  "tools": 9,
  "mcp_auth": "required",
  "compat": {
    "mcp": "/mcp",
    "mcp_protocol": "2024-11-05"
  },
  "concurrency": {
    "in_flight": 0,
    "max_concurrent_calls": 256
  },
  "counters": {
    "requests": 1042,
    "tool_calls": 998,
    "errors": 3,
    "rejected": 0
  }
}
```

---

## 4. Concurrency model

The binary is built for "thousands of parallel clients" — three
independent layers of bounded concurrency, each with its own knob.

### Layer 1 — cpp-httplib worker pool (`--threads`)

Each incoming TCP connection is handed to a worker thread from a fixed
pool. Default pool size is **256** workers; configurable via
`--threads N` or `[SERVER] threads = N`. The bottleneck on real
workloads is the tools themselves (libcurl outbound for `web_*`,
`fork`+`execve` for `bash` / external-tools, disk for the `memory`
tool / fs_*), not the dispatcher — so a few hundred workers is plenty
for most deployments.

Resource note: each pthread on Linux/glibc costs ~8 MiB of *virtual*
stack (commit-on-touch — real RSS is far smaller). 256 workers ≈ 2
GiB virtual address space, ~50-100 MiB RSS in practice. Larger pools
are routinely fine on modern hosts.

### Layer 2 — in-flight `tools/call` cap (`--max-concurrent-calls`)

A separate atomic counter tracks how many `tools/call` JSON-RPC
methods are currently dispatching. When the count reaches the cap,
new `tools/call` requests are **rejected with 503 + `Retry-After:
1`** instead of being queued. Default is 256 (equal to threads — so
the cap doesn't fire under default config; lower it to leave headroom
for cheap methods).

Why fail fast instead of queueing: under thousands-of-clients load,
queueing past a meaningful depth turns into a memory hazard (a
million queued requests all holding their handler-local state). The
correct response is back-pressure — let the client retry. Real-world
clients (Cursor, Continue, Claude Desktop, custom JSON-RPC SDKs) all
handle 503 + `Retry-After` gracefully.

Cheap methods (`initialize`, `tools/list`, `ping`, `notifications/*`)
**bypass** the cap because they don't spawn anything — pure JSON
parse + serialise. Only `tools/call` enters the limiter.

### Layer 3 — per-tool internal locking

Tools that share state synchronise themselves at the lib level:

- **Knowledge tools** (`src/rag_tools.cpp`): `RagStore::mu` is a
  `std::shared_mutex`. `knowledge_search` / `knowledge_load` / `knowledge_list` / `knowledge_keywords`
  take `std::shared_lock` — many parallel readers; `knowledge_save` /
  `knowledge_delete` take `std::unique_lock`. The index is eager-loaded
  under a unique lock at startup so readers never need to upgrade.
  Atomic-rename writes (tempfile + `rename(2)`) make on-disk reads
  tear-free regardless of the lock.
- **web fetch cache** (`src/builtin_tools.cpp`): single `std::mutex`
  guarding a 16-entry LRU shared by every `web(action="fetch")` call.
  Critical sections are O(1) hash-table + list-splice operations — a
  hot spot under extreme load but not a bottleneck for normal operation.
- **bash + external-tools**: each call is a fresh `fork`+`execve`.
  Process-isolated by definition — no shared in-process state. The
  `--max-concurrent-calls` cap is the right backstop for fork-rate
  pressure on the host.
- **datetime, web (search/fetch), fs (read-only actions)**: no
  shared mutable state. Naturally parallel.

### What the layers compose to

```
  client                      cpp-httplib                in-flight
  ──────                      ───────────                limiter
  N concurrent  ───TCP───►   accept                      ┌──────┐
  connections                  │                         │      │
                               ▼                         │      │
                            ThreadPool(256)              │      │
                              │ │ │ ... 256 workers      │      │
                              ▼ ▼ ▼                      │      │
                            route_mcp                    │      │
                              │                          │      │
                  cheap method? ────► dispatch           │      │
                              │                          │      │
                  tools/call?  ────►  acquire ───►───────┤  256 │
                                       │                 │ slot │
                                       ▼                 │      │
                                     tool->handler       │      │
                                       │                 │      │
                                     release ◄───────────┤      │
                                                         └──────┘
```

A 1000-client burst with default config:
1. cpp-httplib's listen accept queue holds the excess (kernel-level).
2. 256 workers pick up requests as they're freed.
3. Cheap methods sail through. `tools/call` flows through the
   in-flight limiter.
4. With `max_concurrent_calls = 256` (default), every worker that
   gets a `tools/call` is allowed to dispatch — no rejection.
5. With `max_concurrent_calls < 256`, the limiter rejects past the
   cap; clients retry per `Retry-After`.

### Picking values for your deployment

| Workload | `--threads` | `--max-concurrent-calls` |
| --- | --- | --- |
| Single-client laptop | 16 | 16 |
| Small team (5-20 clients) | 64 | 64 |
| Large team / public-facing | 256 (default) | 256 (default) |
| Heavy fork pressure (lots of bash / external-tools) | 256 | 64-128 (cap fork rate) |
| Light tools (knowledge tools only, no fork) | 512 | 512 |

Watch `easyai_mcp_in_flight` (gauge) and `easyai_mcp_rejected_total`
(counter) on `/metrics`. If `rejected_total` is climbing, either
raise `--max-concurrent-calls` or right-size the host (more cores,
faster disk for the knowledge tools, larger libcurl connection pool for
`web_fetch`).

---

## 5. Tool catalogue

`easyai-mcp-server` registers the **same** tool factories that
`easyai-server` does. Every tool is implemented in `libeasyai`, so
applications linking the lib see identical behaviour, identical
parameter shapes, and the same hardening — `easyai-mcp-server` is
just one consumer of those factories.

| Tool | Source | Gated by |
| --- | --- | --- |
| `datetime` | `easyai::tools::datetime()` | `load_tools` (default on) |
| `web` (action=`search` / `fetch`) | `easyai::tools::web(google_enabled)` | `load_tools` + libcurl at build |
| `fs` (action=`read` / `write` / `list` / `glob` / `grep` / `check_path` / `cwd` / `sandbox`) | `easyai::tools::fs(sandbox)` | `--allow-fs` |
| `bash` | `easyai::tools::bash(sandbox)` | `--allow-bash` |
| `evaluate` (legacy alias: `python3`) | `easyai::tools::python3(sandbox)` | default ON when sandbox set or `--allow-bash`; `--no-python` to skip |
| `knowledge_save`, `knowledge_append`, `knowledge_search`, `knowledge_load`, `knowledge_list`, `knowledge_delete`, `knowledge_keywords` | `easyai::tools::knowledge_split_tools(dir)` | `--memory <dir>` (alias `--RAG`) |
| (any `EASYAI-*.tools` manifest) | `easyai::load_external_tools_from_dir(dir, reserved)` | `--external-tools <dir>` |

The `plan` tool is **deliberately omitted** in `easyai-mcp-server` —
it carries conversational state that doesn't make sense in a
stateless multi-tenant MCP context. If you need a multi-step plan,
the calling AI app (Claude Desktop, Cursor, Continue, etc.) tracks
it on the client side.

### Adding your own tools without forking

`easyai-mcp-server` is an example binary — anyone can build their
own MCP server in ~20 lines using the same library:

```cpp
#include "easyai/cli.hpp"          // Toolbelt
#include "easyai/external_tools.hpp"
#include "easyai/mcp.hpp"
#include "easyai/rag_tools.hpp"

std::vector<easyai::Tool> tools;
auto tb = easyai::cli::Toolbelt().sandbox("/srv/work").allow_fs();
for (auto & t : tb.tools()) tools.push_back(std::move(t));

for (auto & t : easyai::tools::knowledge_split_tools("/var/lib/myapp/rag"))
    tools.push_back(std::move(t));

// Your custom tools, side by side with the built-ins:
tools.push_back(easyai::Tool::builder("my_internal_query")
    .describe("Look up something in our internal control plane.")
    .param("service", "string", "Service name", true)
    .handle([](const easyai::ToolCall & c) { /* ... */ })
    .build());

// Dispatch over MCP:
easyai::mcp::ServerInfo info{"my-mcp-server", "1.0", "2024-11-05"};
std::string body_in  = /* JSON-RPC request body from HTTP */;
std::string body_out = easyai::mcp::handle_request(body_in, tools, info);
```

`easyai::mcp::handle_request` is documented in
[`include/easyai/mcp.hpp`](include/easyai/mcp.hpp); it's a pure
function with no global state.

---

## 6. Authentication

Two auth surfaces, two mechanisms — same as `easyai-server`.

### `/health`, `/metrics`, `/v1/tools` — single shared API key (`--api-key`)

When `--api-key` (or `[SERVER] api_key`) is set, every request to
`/metrics` or `/v1/tools` must carry `Authorization: Bearer <key>`.
The Authorization header is capped at 4 KiB before string comparison.

`/health` is intentionally **open even when `--api-key` is set** so
liveness probes (kubernetes readinessProbe, systemd watchdog,
external monitoring) don't need a credential.

### `/mcp` — per-user Bearer tokens (`[MCP_USER]`)

Three-way precedence on the gate:

1. `--no-mcp-auth` on the CLI (or `[SERVER] mcp_auth = off`) →
   force open; `[MCP_USER]` is cleared even if populated.
2. `[MCP_USER]` populated → Bearer required, looked up in the
   token→username map at request time. Audit log per request:
   `[mcp] request from user 'gustavo'`. Token never logged.
3. `[MCP_USER]` empty/missing → open.

Same 4 KiB header cap. Tokens are matched in the map; missing or
malformed Bearer returns a JSON-RPC 401 with a `WWW-Authenticate:
Bearer realm="easyai-mcp"` header.

### Compensating controls for high-trust deployments

1. **Bind to LAN only** — `host = 127.0.0.1`; SSH-tunnel from
   external clients.
2. **Reverse proxy with mTLS / IP allowlist** — nginx / Caddy in
   front of `easyai-mcp-server`, require client cert or restrict by
   source net.
3. **Token rotation** — `[MCP_USER]` edits land on next restart;
   old tokens are immediately invalid. There is no in-memory token
   cache that would survive a config change.
4. **Don't enable `--allow-bash` with auth-open mode** — the worst
   `/mcp` can dispatch is the knowledge tools + read-only `web_*` + your
   `--external-tools` allowlist.

---

## 7. Operating the server

### Starting from the command line

```sh
# minimal
easyai-mcp-server --port 8089 --memory /var/lib/easyai-mcp/rag

# production
easyai-mcp-server --config /etc/easyai/easyai-mcp.ini --metrics
```

No systemd unit ships with the installer (intentional — see §10).
For production you'd typically run it under whatever supervisor your
environment provides (systemd, runit, supervisord, container init).
A minimal systemd unit:

```ini
[Unit]
Description=easyai-mcp-server (MCP tool dispatcher)
After=network.target

[Service]
User=easyai-mcp
Group=easyai-mcp
ExecStart=/usr/bin/easyai-mcp-server --config /etc/easyai/easyai-mcp.ini
Restart=on-failure
RestartSec=2
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

`LimitNOFILE=65536` — important under high concurrency: each open
socket consumes an fd, plus each in-flight subprocess (`bash` /
external-tools) holds pipe fds while running.

### Health checks

```sh
# liveness
curl -fsS http://localhost:8089/health > /dev/null

# tools registered?
curl -fsS http://localhost:8089/health | jq .tools

# concurrency saturation?
curl -fsS http://localhost:8089/health | jq .concurrency
```

### Metrics

When `--metrics` is on, `/metrics` exposes Prometheus text format:

```
# HELP easyai_mcp_requests_total Total JSON-RPC requests received.
# TYPE easyai_mcp_requests_total counter
easyai_mcp_requests_total 1042
# HELP easyai_mcp_tool_calls_total Total successful tools/call dispatches.
# TYPE easyai_mcp_tool_calls_total counter
easyai_mcp_tool_calls_total 998
# HELP easyai_mcp_errors_total Total JSON-RPC error envelopes returned.
# TYPE easyai_mcp_errors_total counter
easyai_mcp_errors_total 3
# HELP easyai_mcp_rejected_total tools/call requests rejected by concurrency cap.
# TYPE easyai_mcp_rejected_total counter
easyai_mcp_rejected_total 0
# HELP easyai_mcp_in_flight Tool dispatches currently in flight.
# TYPE easyai_mcp_in_flight gauge
easyai_mcp_in_flight 0
# HELP easyai_mcp_max_concurrent_calls Configured concurrent-call cap.
# TYPE easyai_mcp_max_concurrent_calls gauge
easyai_mcp_max_concurrent_calls 256
# HELP easyai_mcp_tools_registered Total tools advertised over /mcp tools/list.
# TYPE easyai_mcp_tools_registered gauge
easyai_mcp_tools_registered 9
```

Wire into Grafana / Prometheus / AlertManager. Useful alerts:
- `rate(easyai_mcp_rejected_total[5m]) > 0` — backpressure firing.
- `easyai_mcp_in_flight / easyai_mcp_max_concurrent_calls > 0.8` —
  capacity headroom shrinking.
- `rate(easyai_mcp_errors_total[5m]) > 0` — client / tool errors.

### Graceful shutdown

The binary handles `SIGINT` and `SIGTERM` by calling
`httplib::Server::stop()`, which causes `listen()` to return cleanly.
In-flight requests complete; new connections are refused. Exit code
0 on clean shutdown, 1 on listen failure.

### Log lines worth grepping

```sh
journalctl -u easyai-mcp-server | grep -E '\[mcp\]|external-tools|RAG'
```

- `[mcp] request from user 'gustavo'` — every authenticated request.
- `easyai-mcp-server: MCP auth ENABLED — N user(s) loaded from <path>`
  — startup posture confirmation.
- `easyai-mcp-server: knowledge enabled (7 split tools), root = <path>` — the knowledge tools wired.
- `easyai-mcp-server: loaded N external tool(s) from M file(s)` —
  external-tools dir scan.
- `easyai-mcp-server: [external-tools] error: <path>: ...` — manifest
  parse error; that file skipped, others still loaded.
- `easyai-mcp-server: [external-tools] warn: <path>: ...` —
  sanity-check warning (shell wrapper, LD_* passthrough,
  world-writable binary, world-writable manifest).

---

## 8. Hardening / security

Inherits every protection documented in
[`SECURITY_AUDIT.md`](SECURITY_AUDIT.md), since the dispatcher and
the tool implementations are the same as `easyai-server`'s. Specific
to `easyai-mcp-server`:

- **Smaller body cap by default** (1 MiB vs `easyai-server`'s 8 MiB).
  MCP requests are tiny — no chat history, no system prompt, no
  multi-turn carryover. Keeping the cap tight closes a memory-DoS
  vector.
- **Authorization header cap (4 KiB)** on every endpoint — same as
  `easyai-server`.
- **JSON depth cap (64 levels)** inside `easyai::mcp::handle_request`
  so a hostile client can't `{"a":{"a":...}}` the worker thread's
  stack.
- **In-flight `tools/call` cap** rejects with 503 + `Retry-After`
  past the limiter capacity — bounded fork pressure on the host
  even under thousands-of-clients burst.
- **`SIGPIPE` ignored** so a client disconnecting mid-write returns
  EPIPE instead of killing the server.
- **No model loaded** — eliminates the entire chat-completion attack
  surface (Jinja templates, sampler, partial-parse exception paths,
  HTML stripping, model-output recovery). The dispatch path is
  small: HTTP body → JSON parse → method dispatch → tool handler.
- **Same `fork`+`execve` hardening** as `easyai-server` for `bash` /
  external-tools (process group, `PR_SET_PDEATHSIG`, fd close-loop
  bounded by `kMaxFdScan`, stdin → `/dev/null`, opt-in
  `env_passthrough`).

For threat models requiring OS-level isolation, run inside a
container / firejail / unprivileged user. The binary's own
hardening bounds the model's reachable surface; the OS bounds what
the agent process can do.

---

## 9. Choosing between easyai-server and easyai-mcp-server

| Question | `easyai-server` | `easyai-mcp-server` |
| --- | --- | --- |
| Loads a GGUF model? | Yes | No |
| Speaks `/v1/chat/completions`? | Yes | No |
| Speaks Ollama (`/api/tags`, `/api/show`)? | Yes | No |
| Serves a webui? | Yes (embedded SvelteKit) | No |
| Speaks `/mcp` (JSON-RPC 2.0)? | Yes | Yes |
| Designed for thousands of parallel MCP clients? | No (single engine, mutex-serialised) | Yes (256+ workers, in-flight limiter) |
| `knowledge_*` tools, external-tools, fs_*, bash | Yes | Yes (same factories) |
| systemd unit ships with the installer | Yes (`scripts/install_easyai_server.sh`) | No (run under your own supervisor) |
| Right binary when… | …you want one process to BOTH chat AND expose tools to other AI apps | …you want a dedicated tool API for thousands of parallel clients without the model in the loop |

**Both can run on the same host** — they read separate INI files
(`easyai.ini` vs `easyai-mcp.ini`) and listen on different ports
(8080 vs 8089 by default). Operators commonly run both when:

- The chat server is a single-user / small-team interface.
- The MCP server is exposed to the rest of the infrastructure
  (CI systems, internal AI apps, monitoring tooling) where the
  consumer drives its own model and just needs the tool catalogue.

Tools registered in both servers come from the **same lib factories**
and operate on the **same on-disk data** (knowledge dir, sandbox dir,
external-tools manifests). A `knowledge_save` from the chat
server is visible to a `knowledge_search` from the MCP server
immediately — filesystem ACLs are the boundary, not process identity.

---

## 10. Roadmap

Phase 1 (this version): **standalone MCP-only HTTP server, no model,
shared lib code with `easyai-server`.**

What's planned next, roughly in priority order:

1. **Factor the MCP auth helper into the lib.** `check_mcp_auth` is
   currently duplicated between `examples/server.cpp` and
   `examples/mcp_server.cpp`. The next refactor lifts it into
   `easyai::mcp::check_bearer()` so both binaries call the same
   code. Same for the `[MCP_USER]` table loader.
2. **Per-user / per-token rate limit.** Today the only backpressure
   is global (`--max-concurrent-calls`). A `[MCP_RATE_LIMIT]`
   section with `gustavo = 1000/min` etc. would let the operator
   stop one runaway client without affecting others. The user
   already flagged this as out-of-scope for the first cut.
3. **Streaming HTTP transport.** `GET /mcp` returns an SSE stream
   for server-pushed `notifications/tools/list_changed` (when the
   external-tools dir is hot-reloaded). Required for a future
   `tools/list_changed` notification.
4. **Stdio transport built into the binary.** `easyai-mcp-server
   --stdio` runs in stdio mode without the Python bridge — useful
   for shipping easyai as a one-binary MCP provider in a Docker
   image (Claude Desktop spawns the binary directly).
5. **Per-tool ACL via `[TOOLS]`.** The section is reserved in the
   parser; populating it currently does nothing. A future release
   wires `mcp_allowed = knowledge_*, datetime` etc. so `[MCP_USER]
   gustavo` can dispatch the knowledge tools but not `bash`.
6. **MCP resources surface.** Expose knowledge entries as MCP resources
   at URIs like `rag://entry-name`, so a client can `resources/read`
   without going through `tools/call knowledge_load`.
7. **Installer + systemd unit.** Today the binary is positioned as
   a tool for "other environments" (containers, custom supervisors,
   non-Linux). If demand for a Debian/Ubuntu installer materialises,
   it'll mirror `scripts/install_easyai_server.sh`.

---

## 11. Cross-references

- [`README.md`](README.md) — sales overview + quickstart.
- [`easyai-server.md`](easyai-server.md) — the chat server (full INI
  + CLI + API surface + persona + webui customisation + perf tuning).
- [`LINUX_SERVER.md`](LINUX_SERVER.md) — operator's guide for the
  systemd-installed `easyai-server` (file layout, the unit file,
  upgrade / backup).
- [`MCP.md`](MCP.md) — the MCP protocol surface, per-client
  connection cookbook, security model.
- [`RAG.md`](RAG.md) — persistent registry, the split
  `knowledge_*` tools, workflows.
- [`EXTERNAL_TOOLS.md`](EXTERNAL_TOOLS.md) — operator-defined
  external tools (`EASYAI-*.tools` JSON manifests).
- [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) — seven audit passes,
  HIGH / MEDIUM / LOW findings, accepted residual risk.
- [`design.md`](design.md) — architecture + why decisions.
- [`AI_TOOLS.md`](AI_TOOLS.md) — vendor-neutral background on AI
  tool calling, especially Chapter 21 (operator-defined tools at
  deploy time).
