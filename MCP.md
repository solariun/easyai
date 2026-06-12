# MCP — easyai-server as a Model Context Protocol provider

> *"You build the tools once. Every AI app that speaks MCP gets to
> use them — your knowledge tools, your deploy CLI, your monitoring
> queries — without you writing a plugin per app."*

This document is the authoritative guide to the MCP surface
exposed by `easyai-server`. Other AI applications (Claude Desktop,
Cursor, Continue, OpenWebUI in MCP mode, custom JSON-RPC clients)
connect to easyai-server and use its tools as if they were native.

> **Looking for a model-free, high-concurrency MCP daemon?** See
> [`easyai-mcp-server.md`](easyai-mcp-server.md) — same tool
> catalogue, no GGUF loaded, sized for thousands of parallel clients.
> The protocol surface, JSON-RPC methods, auth model, and per-client
> connection guides in this document apply unchanged to that binary
> (just point clients at port 8089 / whatever you configured).

> **Going the other direction — easyai-server as an MCP CLIENT?**
> Pass `--mcp <url>` (and `--mcp-token <token>` if the upstream
> requires bearer auth) to merge another MCP server's tool catalogue
> into the agent's local toolbox. Local tools win on name collision;
> remote dups are skipped with a warning. See
> [easyai-server.md](easyai-server.md) §"Toolbelt opt-ins" and the
> "MCP client" subsection in `[SERVER]` of the INI reference. The
> implementation lives in `easyai::mcp::fetch_remote_tools()` —
> public to libeasyai consumers, so anything built on top of the
> engine library can stack remote MCP catalogues the same way.

---

## Table of contents

1. [What we expose, and why](#1-what-we-expose-and-why)
2. [Wire format](#2-wire-format)
3. [Quickstart with curl](#3-quickstart-with-curl)
4. [Connecting from Claude Desktop](#4-connecting-from-claude-desktop)
5. [Connecting from Cursor](#5-connecting-from-cursor)
6. [Connecting from Continue](#6-connecting-from-continue)
7. [Connecting from a custom client](#7-connecting-from-a-custom-client)
8. [Compatibility shims (`/v1/models`, `/api/tags`)](#8-compatibility-shims)
9. [Security model](#9-security-model)
9.5. [easyai-server as an MCP CLIENT](#95-easyai-server-as-an-mcp-client)
10. [Roadmap](#10-roadmap)
11. [Troubleshooting](#11-troubleshooting)

---

## 1. What we expose, and why

`easyai-server` registers a tool catalogue at startup — built-in
tools, the seven keyword-only knowledge tools (`knowledge_save`,
`knowledge_append`, `knowledge_search`, `knowledge_load`,
`knowledge_list`, `knowledge_delete`, `knowledge_keywords` — a
passive RAG technique over keyword-indexed Markdown files), and any
operator-defined tools loaded from `--external-tools`. The MCP layer
exposes that **same** catalogue via the Model Context Protocol so
other AI applications can list and dispatch them as if they had
registered the tools themselves.

```
                    ┌─────────────────────────────────────┐
                    │         OTHER AI APPLICATIONS        │
                    │  Claude Desktop / Cursor / Continue  │
                    │   OpenWebUI / custom JSON-RPC SDKs   │
                    └─────────────────────────────────────┘
                                      │
                       MCP / JSON-RPC 2.0
                                      │
                                      ▼
   ┌───────────────────────────────────────────────────────────────┐
   │                     easyai-server                              │
   │                                                                │
   │   POST /mcp           ◄── stateless JSON-RPC dispatcher        │
   │                                                                │
   │   exposes the SAME tool catalogue the local model uses:        │
   │                                                                │
   │     • datetime, web (search/fetch), plan                       │
   │     • fs (read/write/list/glob/grep/check_path/cwd/sandbox)   (+--allow-fs)│
   │     • bash                                          (+--allow-bash)│
   │     • knowledge_save/append/search/load/list/delete/keywords   │
   │     • every tool in /etc/easyai/external-tools/EASYAI-*.tools  │
   └───────────────────────────────────────────────────────────────┘
```

**Why this is useful.** The same knowledge tools you populated by
chatting with the local model are now reachable from Claude Desktop.
The internal deploy-cli you wrote a `EASYAI-deploy.tools` manifest for
is now callable from Cursor's chat. Operators write tools once; every
AI client benefits.

---

## 2. Wire format

JSON-RPC 2.0 over a single endpoint:

```
POST /mcp
Content-Type: application/json

{ "jsonrpc": "2.0", "id": 1, "method": "<name>", "params": {...} }
```

Methods we currently implement:

| Method | What it does |
| --- | --- |
| `initialize` | Handshake. Server returns `capabilities`, `serverInfo`, the protocol version it supports (`2024-11-05`), and (since 2026-05-26) a free-text `instructions` field carrying the closed-set rule + write/edit policy (see §2a below). |
| `tools/list` | Enumerate every registered tool as `{ name, description, inputSchema }`. |
| `tools/call` | Dispatch a tool by name with `arguments` (object). Returns `{ content: [{type:"text", text}], isError }`. |
| `ping` | Cheap round-trip to confirm reachability. |
| `notifications/initialized` etc. | Accepted as no-op (per JSON-RPC, notifications don't get a response — server returns 204). |

Methods we do **not** yet implement:

- `resources/list`, `resources/read` — knowledge entries as MCP resources is on the roadmap.
- `prompts/list`, `prompts/get` — easyai doesn't ship prompt templates.
- Streaming `notifications/tools/list_changed` — would require SSE on `/mcp` and hot-reload of the tool catalogue, both deferred.

Unsupported methods return JSON-RPC error code `-32601` (method not found) — clients can introspect via the `initialize` response's `capabilities` block, which only advertises `tools`.

---

## 3. Quickstart with curl

The endpoint is HTTP-only, request/response, no streaming required.
With `easyai-server` running on `http://localhost:80`:

### Initialize

```sh
curl -fsS http://localhost/mcp \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "initialize",
    "params": {
      "protocolVersion": "2024-11-05",
      "capabilities": {},
      "clientInfo": {"name": "curl", "version": "0"}
    }
  }' | jq .
```

Expected:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "protocolVersion": "2024-11-05",
    "capabilities": { "tools": { "listChanged": false } },
    "serverInfo": { "name": "easyai-server", "version": "0.1.0" },
    "instructions": "easyai MCP server. Call ONLY tools listed in tools/list — no paraphrases (`read_file` is not the filesystem tool; `shell` is not `bash`). If a name isn't in tools/list, it does NOT exist on this server; do not invent calls.\n\nWrite/edit policy:\n  - `evaluate` is for COMPUTE / algorithm prototyping ONLY. It runs Python 3 in a sandbox; FORBIDDEN to use it for filesystem writes, subprocess launches, network I/O, or ctypes. Every write-mode open() is rejected even inside the sandbox.\n  - The filesystem tool(s) listed in tools/list are the authoritative path for file creation, modification, and deletion.\n  - `bash` is allowed to write files (redirects, `sed -i`, `mkdir`); use it for shell features the filesystem tool can't do.\n  - On the first PermissionError from `evaluate`, switch to the filesystem tool or `bash` — do not retry the evaluate call.\n"
  }
}
```

### 2a. `initialize.instructions` — the closed-set + write/edit policy

The MCP spec defines `result.instructions` as a free-text hint a
server may surface to the client's model. easyai-server populates
it with:

1. The **closed-set rule** — "call ONLY tools listed in
   `tools/list`; no paraphrases (`read_file` is not `fs`; `shell`
   is not `bash`)".
2. The **write/edit policy**, keyed off which write/exec tools the
   server actually registered:
   - `evaluate` (Python 3 sandbox under the hood) is COMPUTE-only,
     READ-ONLY on disk, FORBIDDEN to use for subprocess / network /
     ctypes. The runtime sandbox rejects write-mode `open()`
     regardless of path.
   - The filesystem tool(s) named in `tools/list` are the
     authoritative writer — names differ by mode (`fs(action=...)`
     in Unified mode, the `fs_write` / `fs_edit` / ... family in
     Split mode).
   - **Read-before-write** (2026-06-12): an EXISTING file must first
     be read with the filesystem read tool in this session before a
     write/edit on it is accepted; blind overwrites come back as an
     error telling the model to read first. New files are exempt.
   - `bash` is allowed to write files (redirects, `sed -i`, `mkdir`).
3. The **recovery rule** — "on the first `PermissionError` from
   `evaluate`, switch to the filesystem tool / `bash`; do not retry
   the evaluate call".

Well-behaved MCP clients (Claude Desktop, Cursor) inject this text
into their model's system prompt automatically. Non-conforming
clients ignore it harmlessly — the policy still holds at the tool
level (the `evaluate` sandbox raises PermissionError regardless of
what the model was told).

The text reshapes itself based on the live toolset: with
`--no-python` the python paragraph drops out, with `--allow-bash`
off the bash paragraph drops out, and so on.

### List tools

```sh
curl -fsS http://localhost/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  | jq '.result.tools[] | .name'
```

You should see the full catalogue — datetime, the unified `web` tool,
the seven `knowledge_*` tools, any external tools you have configured.

### Call a tool

```sh
curl -fsS http://localhost/mcp \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc":"2.0","id":3,
    "method":"tools/call",
    "params": {
      "name": "knowledge_keywords",
      "arguments": {}
    }
  }' | jq -r '.result.content[0].text'
```

Returns the live knowledge vocabulary the local model has built up.

### Ping

```sh
curl -fsS http://localhost/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":4,"method":"ping"}' | jq .
```

Returns `{"jsonrpc":"2.0","id":4,"result":{}}`.

---

## 4. Connecting from Claude Desktop

Claude Desktop only speaks the **stdio** MCP transport — it spawns
the MCP server as a subprocess and exchanges JSON-RPC over
stdin/stdout. easyai-server is HTTP-only.

Use the included **stdio bridge** at `scripts/mcp-stdio-bridge.py`.
Claude Desktop spawns the bridge; the bridge POSTs to `/mcp`.

### Step 1 — copy the bridge to a stable path

```sh
sudo cp scripts/mcp-stdio-bridge.py /usr/local/bin/easyai-mcp-bridge
sudo chmod +x /usr/local/bin/easyai-mcp-bridge
```

### Step 2 — wire it into Claude Desktop's config

Edit the platform-appropriate config file:

| Platform | Path |
| --- | --- |
| macOS | `~/Library/Application Support/Claude/claude_desktop_config.json` |
| Linux | `~/.config/Claude/claude_desktop_config.json` |

```json
{
  "mcpServers": {
    "easyai": {
      "command": "/usr/local/bin/easyai-mcp-bridge",
      "args": [
        "--url", "http://192.168.1.10:80"
      ],
      "env": {
        "EASYAI_API_KEY": ""
      }
    }
  }
}
```

Replace the URL with your easyai-server's address. If you have a
Bearer token configured (`/etc/easyai/api_key` exists), put it in
`EASYAI_API_KEY`; the bridge reads it from the environment.

### Step 3 — restart Claude Desktop

Claude Desktop reloads the config on startup. After restart, the
"Search and tools" menu shows easyai's tools alongside whatever
Anthropic ships natively.

### Verifying

In Claude Desktop, ask: *"Use the knowledge tools to show me your
registry vocabulary."* Claude will dispatch `tools/call` with
name `knowledge_keywords` and `arguments: {}` — easyai handles it
locally, returns the keyword counts, and Claude reads them.

---

## 5. Connecting from Cursor

Cursor speaks HTTP MCP natively — no bridge required.

In Cursor's settings → Features → MCP → Add server:

```json
{
  "mcpServers": {
    "easyai": {
      "url": "http://192.168.1.10:80/mcp"
    }
  }
}
```

If you have a Bearer token, add an `Authorization` header:

```json
{
  "mcpServers": {
    "easyai": {
      "url": "http://192.168.1.10:80/mcp",
      "headers": {
        "Authorization": "Bearer YOUR-TOKEN"
      }
    }
  }
}
```

(easyai-server's `/mcp` endpoint currently runs WITHOUT auth — see
[§9 Security](#9-security-model) — but if your build has the auth
patch applied this is where you put the header.)

---

## 6. Connecting from Continue

Continue (continue.dev) supports MCP via HTTP from version 0.8.x.
In your `~/.continue/config.json`:

```json
{
  "mcpServers": [
    {
      "name": "easyai",
      "url": "http://192.168.1.10:80/mcp"
    }
  ]
}
```

After saving, Continue's chat shows easyai's tools in its tool
picker.

---

## 7. Connecting from a custom client

Any JSON-RPC 2.0 library works. The flow:

1. POST `initialize` once at startup (declare client identity).
2. POST `tools/list` to enumerate.
3. POST `tools/call` per invocation.
4. (Optional) periodic `ping` for liveness.

Python sketch using `urllib`:

```python
import json, urllib.request

URL = "http://localhost/mcp"

def call(method, params=None, req_id=1):
    body = json.dumps({
        "jsonrpc": "2.0", "id": req_id,
        "method": method, "params": params or {}
    }).encode()
    r = urllib.request.urlopen(
        urllib.request.Request(URL, data=body,
                               headers={"Content-Type": "application/json"}))
    return json.loads(r.read())

print(call("initialize", {"protocolVersion":"2024-11-05",
                          "capabilities":{},
                          "clientInfo":{"name":"smoke","version":"0"}}))
print([t["name"] for t in call("tools/list")["result"]["tools"]])
print(call("tools/call",
           {"name":"knowledge_keywords","arguments":{}})["result"]["content"][0]["text"])
```

Node / TypeScript clients can use any JSON-RPC library
(`json-rpc-2.0`, `@modelcontextprotocol/sdk`'s HTTP transport,
etc.). The Anthropic-published `@modelcontextprotocol/sdk` works
out of the box once you point its `StreamableHTTPClientTransport`
at `http://your-server/mcp`.

---

## 8. Compatibility shims

`easyai-server` also speaks two adjacent APIs so OpenAI- or
Ollama-aware clients can discover the model without knowing about
MCP:

### OpenAI list-models — `GET /v1/models`

```sh
curl -fsS http://localhost/v1/models | jq .
```

```json
{
  "object": "list",
  "data": [
    {
      "id": "EasyAi",
      "object": "model",
      "created": 0,
      "owned_by": "easyai"
    }
  ]
}
```

This is what every OpenAI SDK probes on startup. Continue,
LangChain, LiteLLM, the `openai` Python client, and so on all
work against `/v1/chat/completions` once they've seen a model in
this list.

### Ollama list-models — `GET /api/tags`

```sh
curl -fsS http://localhost/api/tags | jq .
```

```json
{
  "models": [
    {
      "name": "EasyAi",
      "model": "EasyAi",
      "modified_at": "1970-01-01T00:00:00Z",
      "size": 0,
      "digest": "",
      "details": {
        "format": "gguf",
        "family": "easyai",
        "families": ["easyai"],
        "parameter_size": "",
        "quantization_level": ""
      }
    }
  ]
}
```

LobeChat, OpenWebUI in Ollama mode, Continue's Ollama provider,
and various GUI tools (Ollama-WebUI, big-AGI) probe `/api/tags`
to populate their model picker. With this shim they auto-discover
easyai's single loaded model and chat against it via OpenAI's
endpoint (most modern Ollama clients also speak OpenAI-compat for
chat).

`/api/show` (POST or GET) is also supported — returns details
about the single model, mirroring Ollama's response shape with
placeholder values where we don't have real metadata
(no per-model digest, no precise parameter size).

`/health` includes a `compat` block listing every protocol the
server speaks:

```json
{
  "status": "ok",
  "model": "EasyAi",
  "tools": 14,
  "preset": "balanced",
  "compat": {
    "openai":   "/v1/chat/completions",
    "ollama":   "/api/tags",
    "mcp":      "/mcp",
    "mcp_protocol": "2024-11-05"
  }
}
```

---

## 9. Security model

The `/mcp` endpoint authenticates via Bearer tokens declared in
the central INI config (`/etc/easyai/easyai.ini` by default).
Full INI reference: [`easyai-server.md`](easyai-server.md) §1.

Auth is **opt-in by configuration**: if the INI's `[MCP_USER]`
section is empty or missing, the endpoint accepts any request
(handy for local dev). Populate at least one user to require
auth in production.

### `[MCP_USER]` — adding a user

Edit `/etc/easyai/easyai.ini`:

```ini
[MCP_USER]
gustavo = abcdef0123456789...   # generate: openssl rand -hex 32
ci      = different-strong-token
```

Each line registers `username = bearer_token`. Restart the server
to pick up changes:

```bash
sudo systemctl restart easyai-server
```

The username appears in the audit log per request — `journalctl
-u easyai-server | grep "[mcp]"` shows e.g. `[mcp] request from
user 'gustavo'`. The token never logs.

Generate strong tokens with `openssl rand -hex 32` (or
`python3 -c 'import secrets; print(secrets.token_hex(32))'`).
Treat them like sudoers passwords — they grant tool dispatch
privilege.

### Client side — sending the token

```sh
curl -fsS http://localhost/mcp \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer abcdef0123456789..." \
  -d '{"jsonrpc":"2.0","id":1,"method":"ping"}'
```

In Cursor/Continue config:

```json
{
  "mcpServers": {
    "easyai": {
      "url": "http://192.168.1.10/mcp",
      "headers": { "Authorization": "Bearer abcdef..." }
    }
  }
}
```

In the stdio bridge (Claude Desktop):

```json
{ "mcpServers": { "easyai": {
    "command": "/usr/local/bin/easyai-mcp-bridge",
    "args": ["--url", "http://192.168.1.10"],
    "env": { "EASYAI_API_KEY": "abcdef..." } }}}
```

### Disabling auth temporarily

Three ways:

1. **Empty `[MCP_USER]`** — comment out every user line in the INI.
2. **`[SERVER] mcp_auth = off`** — overrides the auto-detect.
3. **`--no-mcp-auth` CLI flag** — overrides everything (the
   binary opens /mcp regardless of INI, useful for one-off
   debugging without editing the file).

### Mitigations beyond Bearer auth

For high-trust deployments stack:

1. **Bind to LAN only** — `[SERVER] host = 127.0.0.1` and
   SSH-tunnel from clients.
2. **Reverse proxy with mTLS / IP allowlist** — nginx / Caddy in
   front of easyai-server, require client cert or restrict by
   source.
3. **Token rotation** — change the values in `[MCP_USER]` and
   restart; old tokens immediately invalid.
4. **Don't enable `--allow-bash`** with auth-open mode — the
   worst MCP can dispatch is the `knowledge_*` tools + read-only `web_*`
   and your `--external-tools` allowlist.

---

## 9.5 easyai-server as an MCP CLIENT

The same process that exposes `/mcp` can also **consume** another
MCP server's catalogue. Pass `--mcp <url>` (and `--mcp-token` if the
upstream needs bearer auth) and at startup easyai-server runs:

```
initialize          → claim protocolVersion 2024-11-05
notifications/initialized
tools/list          → enumerate the upstream's tools
```

Each remote tool is registered locally as a `Tool` whose handler
proxies `tools/call` over HTTP. From the model's perspective there's
no distinction — local and remote tools sit in the same catalogue.

```
                   easyai-server (this process)
                         │
            ┌────────────┼─────────────┬──────────────┐
            │            │             │              │
            ▼            ▼             ▼              ▼
       local toolbelt  memory    external-tools   ┌────────┐
                                                  │  --mcp │
                                                  │   ▼    │
                                                  │  HTTP  │
                                                  │   ▼    │
                                                  │ remote │
                                                  │  /mcp  │
                                                  └────────┘
```

**Collision policy.** Local tool names take precedence. A remote
tool whose name already exists locally is skipped with a startup
warning so the operator can see what was dropped. Pass
`--no-local-tools` if you want the remote catalogue unopposed.

**Retry & timeout.** The MCP client honours the same `--http-retries`
(default `5`) and `--http-timeout` (default `600 s`) flags as the
listen socket. Transient failures (`CURLE_COULDNT_CONNECT`,
`CURLE_OPERATION_TIMEDOUT`, `CURLE_RECV_ERROR`/`SEND_ERROR`,
`CURLE_GOT_NOTHING`/`PARTIAL_FILE`, plus HTTP 5xx) trigger an
exponential-backoff retry (250 ms → 500 ms → 1 s → 2 s → 4 s,
capped). 4xx responses (auth rejected, malformed request) skip
the retry loop. Each retry logs to stderr unconditionally:

```
[easyai-mcp] http://up:8089/mcp attempt 2/6 failed (Couldn't connect to server); retrying in 500ms
```

**Failure modes.** A connect failure that exhausts the retry budget
at startup logs a warning and lets the server start anyway — a
transient outage at the upstream MCP server should not take down
chat. Auth rejection (401/403) skips the retry loop and produces the
same warning. Mid-session call failures, post-retry, surface as
`ToolResult::error` with the curl/HTTP error attached — same shape
every other tool failure has.

**API for downstream consumers.** `easyai::mcp::fetch_remote_tools(opts)`
is public in libeasyai; any program built on top of the engine
library can stack a remote MCP catalogue without writing a new
client. `ClientOptions::retries` and `ClientOptions::timeout_seconds`
are the programmatic equivalents of `--http-retries` / `--http-timeout`.
The implementation is libcurl-based, gated on `EASYAI_HAVE_CURL`
(the same flag that gates the unified `web` tool).

---

## 10. Roadmap

Phase 1 (this version): **tools-only, request/response, no auth.**

What we'll add next, roughly in priority order:

1. **Bearer auth gate** on `/mcp`. See §9.
2. **Resources surface.** Expose knowledge entries as MCP resources at
   URIs like `rag://entry-name`, so a client can `resources/read`
   without going through `tools/call knowledge_load`.
3. **Streaming HTTP transport.** `GET /mcp` returns an SSE stream
   for server-pushed `notifications/tools/list_changed` (when
   external-tools dir is hot-reloaded). Required for a future
   `tools/list_changed` notification.
4. **Stdio transport built into the binary.** `easyai-server
   --stdio` runs in stdio mode without the Python bridge — useful
   for shipping easyai as a one-binary MCP provider in a Docker
   image.
5. **Prompts surface.** A library of pre-built prompts the user
   can invoke.
6. **Resource subscriptions.** Live updates as the knowledge store changes.

---

## 11. Troubleshooting

### `404 not found` on `/mcp`

Server isn't running the binary that has MCP. Confirm:

```sh
journalctl -u easyai-server | grep -i mcp
sudo systemctl restart easyai-server
sudo journalctl -u easyai-server -n 30 --no-pager
```

The unit's startup log mentions registered tools and (if you're
running a fresh build) the MCP wire surface.

### `405 Method Not Allowed` on `GET /mcp`

That's by design. Use `POST` with a JSON-RPC body. `GET /mcp`
is reserved for a future SSE notification stream and currently
returns `405` with an `Allow: POST` header.

### Bridge script never returns from Claude Desktop

Most likely the bridge script can't reach easyai-server. Test
manually:

```sh
echo '{"jsonrpc":"2.0","id":1,"method":"ping"}' \
  | /usr/local/bin/easyai-mcp-bridge --url http://192.168.1.10:80
```

If you see `cannot reach easyai-server at .../mcp`, it's a network
issue (firewall, wrong IP, server down). If you see
`{"jsonrpc":"2.0","id":1,"result":{}}` the bridge is fine and
Claude Desktop's config or restart cycle is the issue.

### Tool call returns `isError: true`

Per the MCP spec, `isError: true` means the tool ran but reported
a logical failure (missing argument, invalid input, etc.). The
`content[0].text` field has the human-readable error. This is
distinct from a JSON-RPC `error` envelope — the request itself
succeeded, only the wrapped tool reported a problem.

### Tools/list returns more entries than I expected

Every registered tool is exposed: built-ins + the seven `knowledge_*`
tools + external-tools (operator's `EASYAI-*.tools` manifests). Use
`/health` to see the count and `/v1/tools` for a brief description
list.

### How do I add or remove tools?

Restart easyai-server. The MCP catalogue is built from
`ctx->default_tools` at startup and isn't hot-reloaded. After
adding a `EASYAI-*.tools` file (or removing one), restart:

```sh
sudo systemctl restart easyai-server
```

Future versions will support `notifications/tools/list_changed`
without a restart, but it's not in V1.

---

*See also:* `LINUX_SERVER.md` (operator's guide), `RAG.md` (the
seven keyword-only knowledge tools that the model writes to and
clients read from), `EXTERNAL_TOOLS.md` (operator-defined tool packs
that show up in the MCP catalogue alongside built-ins), `design.md`
(architecture).
