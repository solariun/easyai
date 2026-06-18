# easyai-server on Linux — operator's guide

This document is for the person who runs `install_easyai_server.sh`
on a Linux box and wants to know what landed where, how to configure
it, what to watch out for, and how to keep it healthy.

If you want the binary's full INI / CLI / API reference (every flag
and endpoint, with the INI section at the top), see
[`easyai-server.md`](easyai-server.md). If you want a model-free,
high-concurrency MCP daemon for thousands of parallel clients instead
of (or alongside) the chat server, see
[`easyai-mcp-server.md`](easyai-mcp-server.md). If you're a developer,
see `design.md` and `manual.md`. If you're writing tool manifests,
see `EXTERNAL_TOOLS.md`. If you want to understand the agent's
long-term memory (the `memory` tool), see `RAG.md`. If you want to
expose easyai's tools to other AI applications (Claude Desktop,
Cursor, Continue), see `MCP.md`.

---

## Table of contents

0. [Quick start — connect your editor](#0-quick-start--connect-your-editor)
1. [What gets installed where](#1-what-gets-installed-where)
2. [The systemd unit](#2-the-systemd-unit)
3. [Configuration files](#3-configuration-files)
4. [The four mutable directories](#4-the-four-mutable-directories)
5. [The `--external-tools` directory](#5-the---external-tools-directory)
6. [The `memory` directory](#6-the-reg-directory)
7. [Performance tuning](#7-performance-tuning)
8. [Common gotchas](#8-common-gotchas)
9. [Hitting the API](#9-hitting-the-api)
10. [Health checks and verification](#10-health-checks-and-verification)
11. [Backup, restore, migration](#11-backup-restore-migration)
12. [Upgrading](#12-upgrading)
13. [Uninstalling](#13-uninstalling)
14. [Troubleshooting](#14-troubleshooting)

---

## 0. Quick start — connect your editor

The installer leaves you with an OpenAI-compatible HTTP API and an
mDNS advertisement (`--no-avahi` to skip). With the default install
the box advertises itself as `<hostname>.local`; the examples below
assume the server's hostname is **`ai`** so the URL is
**`http://ai.local:80/v1`**. Substitute your actual hostname (run
`hostname` on the server) if it isn't `ai`.

**Sanity check first** — fail fast on networking before fighting
extension config:

```bash
curl http://ai.local:80/v1/models
```

You should see a JSON object listing one model whose `id` matches
`[SERVER] alias` in `easyai.ini` (default `EasyAi`). If `curl` hangs
or returns nothing, fix DNS / firewall / port before going further.

If your INI has `api_key` set, also pass `Authorization: Bearer …`
on every request.

### VSCode + Continue.dev

Install + configure in one shot — paste in any shell on the machine
where VSCode runs:

```bash
# 1. install the extension via VSCode CLI
code --install-extension Continue.continue

# 2. write the Continue config pointing at ai.local
mkdir -p ~/.continue
cat > ~/.continue/config.yaml <<'YAML'
name: ai.local
version: 1.0.0
schema: v1
models:
  - name: EasyAi
    provider: openai
    model: EasyAi              # matches [SERVER] alias = EasyAi
    apiBase: http://ai.local:80/v1
    apiKey: dummy              # any non-empty string when api_key is unset
    roles: [chat, edit, apply]
context:
  - provider: code
  - provider: docs
  - provider: diff
  - provider: terminal
  - provider: problems
  - provider: folder
  - provider: codebase
YAML
```

Open VSCode → Continue panel (sidebar icon, or `Cmd+L` / `Ctrl+L`)
→ pick `EasyAi` from the model dropdown → start chatting.
`Cmd+L` opens chat with the current selection;
`Cmd+I` does inline edits.

If `api_key` is set on the server, replace `dummy` with that token.

### OpenCode (TUI agentic coder)

OpenCode is roughly Claude-Code-shaped: agentic, in-terminal, edits
files and runs shell commands in the project you launch it from.

```bash
# 1. install the binary (one-line installer from opencode.ai)
curl -fsSL https://opencode.ai/install | bash

# 2. configure it to use easyai-server as an OpenAI-compatible provider
mkdir -p ~/.config/opencode
cat > ~/.config/opencode/opencode.json <<'JSON'
{
  "$schema": "https://opencode.ai/config.json",
  "provider": {
    "easyai": {
      "npm": "@ai-sdk/openai-compatible",
      "name": "easyai",
      "options": {
        "baseURL": "http://ai.local:80/v1",
        "apiKey": "dummy"
      },
      "models": {
        "EasyAi": { "name": "EasyAi" }
      }
    }
  },
  "model": "easyai/EasyAi"
}
JSON

# 3. launch it inside any project
cd ~/some/project && opencode
```

OpenCode runs its own fs/bash sandboxing on YOUR local machine —
unrelated to the server's `[SERVER] sandbox` and `allow_fs` /
`allow_bash` gates. The server-side gates only affect tools that
the model itself calls server-side (web / `memory` / etc.).

### VSCode + Cline (heavier agent)

```bash
code --install-extension saoudrizwan.claude-dev
```

Click the Cline icon in the sidebar → API Provider →
**OpenAI Compatible** → fill in:

| Field | Value |
| --- | --- |
| Base URL | `http://ai.local:80/v1` |
| API Key | any non-empty string (or your INI `api_key` if set) |
| Model ID | `EasyAi` |

Cline calls `/v1/models` on save to verify; you'll see `EasyAi`
appear in the model picker.

### Other OpenAI-compatible clients

The pattern is identical for anything that takes a base URL and a
model id:

| Client | Base URL | Model |
| --- | --- | --- |
| `openai` Python SDK | `http://ai.local:80/v1` | `EasyAi` |
| `openai` Node SDK | `http://ai.local:80/v1` | `EasyAi` |
| Open WebUI (OpenAI mode) | `http://ai.local:80/v1` | `EasyAi` |
| LobeChat | `http://ai.local:80/v1` | `EasyAi` |
| LM Studio (remote) | `http://ai.local:80/v1` | `EasyAi` |
| LiteLLM proxy upstream | `http://ai.local:80/v1` | `openai/EasyAi` |

Ollama-mode clients work too — point them at
`http://ai.local:80/api/tags` and pick the same model id. See §9 for
the full endpoint table.

---

## 1. What gets installed where

The installer (`scripts/install_easyai_server.sh`) follows the FHS
roughly:

| Path | Owned by | Mode | Purpose |
| --- | --- | --- | --- |
| `/usr/bin/easyai-server` | root:root | 755 | the binary |
| `/usr/bin/easyai-cli` | root:root | 755 | OpenAI-shape client (talks to a remote server) |
| `/usr/bin/easyai-local` | root:root | 755 | single-process REPL with a local model |
| `/usr/lib/easyai/` | root:root | 755 | bundled `.so` files (libllama, libggml, libeasyai, …) |
| `/etc/easyai/` | root:easyai | 750 | operator configuration |
| `/etc/easyai/system.txt_template` | root:easyai | 644 | system prompt template (refreshed on every `--upgrade`); copy to `system.txt` to activate a custom persona |
| `/etc/easyai/system.txt` | root:easyai | 640 | **NOT installed by default** — created only by the operator (e.g. `sudo cp system.txt_template system.txt`); when present and `SERVER.system_file` is uncommented in `easyai.ini`, replaces the binary's built-in "Deep" prompt |
| `/etc/easyai/api_key` | easyai:easyai | 600 | optional bearer-token gate |
| `/etc/easyai/external-tools/` | root:easyai | 750 | operator-defined tools (`EASYAI-*.tools`) |
| `/etc/easyai/favicon[.ext]` | root:easyai | 644 | optional webui favicon |
| `/var/lib/easyai/` | easyai:easyai | 750 | mutable agent state |
| `/var/lib/easyai/rag/` | easyai:easyai | 750 | `memory` tool long-term store |
| `/var/lib/easyai/workspace/` | easyai:easyai | 750 | sandbox for *_file and bash tools |
| `/var/lib/easyai/models/` | easyai:easyai | 750 | the GGUF symlink target |
| `/etc/systemd/system/easyai-server.service` | root:root | 644 | the unit file |
| `/etc/systemd/system/easyai-server.service.d/override.conf` | root:root | 644 | LimitMEMLOCK / LimitCORE / Environment overrides |

The installer creates the `easyai` system user / group if missing.

---

## 2. The systemd unit

```bash
systemctl cat easyai-server
```

Roughly:

```ini
[Unit]
Description=easyai-server (llama.cpp + OpenAI shim)
After=network.target

[Service]
User=easyai
Group=easyai
ExecStart=/bin/sh -c '...EASYAI_API_KEY=$(cat /etc/easyai/api_key) ...
                      exec /usr/bin/easyai-server \
                          -m /var/lib/easyai/models/current.gguf \
                          --host 0.0.0.0 --port 80 \
                          --alias EasyAi \
                          -c 262144 \
                          --ngl 99 \
                          -t <jobs> -tb <jobs> \
                          --preset balanced \
                          --sandbox /var/lib/easyai/workspace \
                          --system-file /etc/easyai/system.txt \
                          --external-tools /etc/easyai/external-tools \
                          --memory /var/lib/easyai/rag \
                          ... '
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
```

Note: the actual unit also carries `StartLimitBurst=2` +
`StartLimitIntervalSec=60` in `[Unit]`, so the server attempts to
start at most twice before giving up.  Rapid back-to-back failures
(missing model file, bad CLI flag, GPU not exposed) leave the unit
in the `failed` state instead of looping forever — check the journal,
fix the root cause, then `sudo systemctl reset-failed easyai-server`
+ `sudo systemctl start easyai-server`.  A long-running service that
fails after running successfully for more than the 60 s window is
NOT penalised — only boot-time failures hit the cap.

Important pieces:

- `User=easyai`. The agent runs unprivileged. `bash`, the unified
  `fs` tool, every external tool inherits this uid. THE single biggest
  "isolation" you have. Don't run as root.
- `--sandbox /var/lib/easyai/workspace`. Where the agent's `bash` /
  `fs` tools land. The agent `chdir`s here at startup so
  `fs(action="cwd")` reports this path.
- `--external-tools /etc/easyai/external-tools`. Operator-defined
  tools live here. Empty dir is a normal state.
- `--memory /var/lib/easyai/rag` (legacy alias: `--RAG`). The agent's
  persistent **memory** (search / store / recall / update / forget).
  Registers ONE `memory(action=...)` tool with sub-actions save /
  append / search / load / list / delete / keywords — a passive RAG
  technique over keyword-indexed Markdown files. Memories whose title
  starts with `fix-easyai-` are immutable — the model can't overwrite
  or forget them, useful for seeding system designs and hard rules.
  **Also triggers per-request injection** of a compact `# MEMORY
  VOCABULARY` block into the AUTHORITATIVE preamble (every distinct
  keyword + count, top 40) so the model sees its keyword index every
  turn and can dispatch `memory(action="search")` without first
  calling `memory(action="keywords")`. Empty store → block omitted.

Optional add-ons the systemd unit does NOT pass by default but the
installer leaves room for in `/etc/easyai/easyai.ini`:

- `[SERVER] mcp = http://upstream-host:port`. easyai-server connects
  to that MCP server as a client and merges its tool catalogue into
  this one. Pair with `mcp_token = …` if the upstream uses bearer
  auth. Local tool names win on collision.
- `[SERVER] use_google = true`. Enables `engine="google"` inside the
  unified `web` tool (Google Custom Search JSON API), and lets the
  default `engine="auto"` cascade try google as its first hop. Needs
  `GOOGLE_API_KEY` and `GOOGLE_CSE_ID` in `Environment=` lines of a
  drop-in. Counts against your Google quota (free tier: 100/day).
  Without it, the auto cascade falls through to brave (keyless HTML,
  best understanding of niche named entities) → ddg-lite (keyless
  no-JS DDG with Netscape UA, page 1 only) → bing (keyless RSS)
  → ddg (keyless HTML scrape).
- `[SERVER] local_tools = false` (or pass `--no-local-tools`).
  Skips the LOCAL built-in toolbelt — the model only sees the
  `memory` tool, external-tools, and any `--mcp` upstream. **Renamed
  from `load_tools` / `--no-tools`** so the scope is unambiguous now
  that the MCP client is its own concern.
- `LimitMEMLOCK=infinity` (in the drop-in) so `mlock` works.
- `LimitCORE=infinity` (in the drop-in) so coredumps land for
  forensics.

### Drop-in for environment overrides

```bash
sudo systemctl edit easyai-server
```

Add `Environment=` lines. Common ones:

```
[Service]
Environment=RADV_PERFTEST=gpl
Environment=DEPLOY_TOKEN=xxx     # if your --external-tools needs it
```

Reload + restart:

```bash
sudo systemctl daemon-reload
sudo systemctl restart easyai-server
```

---

## 3. Configuration files

### `/etc/easyai/easyai.ini` — the central config

> **Full reference:** [`easyai-server.md`](easyai-server.md) §1 lists every key the binary
> understands, what section it belongs to, what the CLI equivalent
> is, and gives worked examples.

All operator-tunable knobs live in one INI file. The systemd unit's
`ExecStart` is intentionally short — `--config /etc/easyai/easyai.ini`
plus the model path and the api-key plumbing — and **everything else**
(host, port, alias, sandbox, memory dir, KV cache types, mlock, flash-attn,
threads, MCP auth, …) lives in this file.

Precedence: **CLI flag in the systemd unit > `[MODEL_<pattern>]` >
`[ENGINE]` > hardcoded default in the binary.** So tweak this file for
the normal case; flip a CLI flag only for one-off overrides.

Sections:

| Section | Purpose | Status |
| --- | --- | --- |
| `[SERVER]` | HTTP layer + paths + tool gating + MCP auth posture | active |
| `[ENGINE]` | Model loading + inference tunables (context, ngl, KV, mlock, flash-attn, sampling, RoPE, split mode) | active |
| `[MODEL_<pattern>]` | Per-model ENGINE overrides — same keys as `[ENGINE]`, matched by model filename substring | active |
| `[MCP_USER]` | Bearer-token auth for `/mcp` (one user per line, `name = token`) | active |
| `[TOOLS]` | Per-tool ACL (`mcp_allowed = …, mcp_denied = …`) | reserved for future |

**New `[ENGINE]` keys** (added alongside existing sampling keys):

| Key | Type | Range | Default | Description |
| --- | --- | --- | --- | --- |
| `frequency_penalty` | float | 0.0 -- 2.0 | 0.05 | Per-token penalty proportional to appearance count. 0.0 = disabled. |
| `split_mode` | string | `none` / `layer` / `row` / `tensor` | `none` | GPU split strategy for multi-GPU setups. |
| `rope_scaling` | string | `none` / `linear` / `yarn` | `yarn` | RoPE positional encoding scaling for context extension. |
| `rope_freq_scale` | float | -- | 2 | Scaling factor for RoPE frequencies. |
| `yarn_orig_ctx` | int | -- | 131072 | Model's original training context length before YaRN extension. |

**`[MODEL_<pattern>]` — per-model ENGINE overrides**

Allows per-model tuning based on the model filename. The server
resolves the model path (follows symlinks), strips to basename
without extension, then does case-insensitive substring matching
against all `[MODEL_*]` section names (with the `MODEL_` prefix
stripped). Longest match wins. Keys inside are the same as
`[ENGINE]`.

Precedence: **CLI flag > `[MODEL_<pattern>]` > `[ENGINE]` >
hardcoded default.**

Example: serving `Qwen3-Coder-Next-Q6_K_M.gguf` with two INI
sections `[MODEL_Qwen3]` and `[MODEL_Qwen3-Coder-Next]` — the
longer match `[MODEL_Qwen3-Coder-Next]` wins.

```ini
[ENGINE]
temperature = 0.2
top_p       = 0.92

[MODEL_Qwen3-Coder-Next]
temperature = 0.1
top_k       = 30

[MODEL_Qwen3]
temperature = 0.3
```

The installer drops a fully-populated `easyai.ini` (every key
documented inline). On `--upgrade` we **leave it alone** — your edits
win — so any keys we add in newer versions need to be merged
manually. We may at some point grow a polite `upsert`; for now the
release notes call out each new key.

To enable MCP Bearer auth: edit `[MCP_USER]`, uncomment one of the
example lines, replace the placeholder token with output from
`openssl rand -hex 32`. Restart the server. See `MCP.md` §9 for the
full guide and per-client config.

To temporarily reopen `/mcp` without editing the INI: pass
`--no-mcp-auth` on the systemd unit's `ExecStart` (or run the
binary by hand). The flag always wins.

### `/etc/easyai/system.txt` (operator-supplied) + `system.txt_template`

By default the installer ships **only** the template
(`/etc/easyai/system.txt_template`) — the active `system.txt` is
NOT created.  Out of the box the binary's built-in "Deep" prompt
(gated on actually-registered tools) is what the model sees, and
`SERVER.system_file` is left commented out in `easyai.ini`.

To activate a custom persona:

```bash
sudo cp /etc/easyai/system.txt_template /etc/easyai/system.txt
sudo nano /etc/easyai/system.txt              # tweak as needed
sudoedit /etc/easyai/easyai.ini               # uncomment SERVER.system_file
sudo systemctl restart easyai-server
```

The template is refreshed on every `--upgrade` (it's the canonical
"factory reset" copy); the active `system.txt` is **never** touched
by the installer once created — operator edits survive every
`--upgrade` / `--force` run.

Note (2026-06-12): `system.txt` (like the built-in "Deep" prompt) only
applies to requests that do NOT carry their own `system` message. A
client-supplied `system` message replaces it and is used verbatim —
nothing is appended to it, not even the AUTHORITATIVE preamble —
unless that request also sends `X-Easyai-Inject: on`. Agentic clients
(easyai-cli, opencode, Claude Code) compose their own prompts, so
this is what makes them behave identically against easyai-server and
any other OpenAI-compatible endpoint.

Customise to add domain context, persona, language preferences. If
you want the model to use the `memory` tool aggressively, mention it
here:

```
You have a persistent registry: the `memory` tool. Save important
things the user tells you (preferences, project facts, recipes that
worked) with memory(action="save"). Search it with
memory(action="search") before assuming you don't know something the
user might have told you in a past session.
```

The installer also injects an authoritative date/time prefix at
runtime — see `design.md` §5c.

### `/etc/easyai/api_key`

If present, the server requires `Authorization: Bearer <key>` on
every request. Mode 600, owned by `easyai`.

```bash
echo -n "my-secret-token" | sudo tee /etc/easyai/api_key
sudo chown easyai:easyai /etc/easyai/api_key
sudo chmod 600 /etc/easyai/api_key
sudo systemctl restart easyai-server
```

To disable auth:

```bash
sudo rm /etc/easyai/api_key
sudo systemctl restart easyai-server
```

### `/etc/easyai/favicon[.ext]`

Optional webui favicon. The installer accepts `.ico`, `.png`, `.svg`,
`.gif`, `.jpg`, `.webp`. Re-run the installer with `--webui-icon` to
swap.

---

## 4. The four mutable directories

| Dir | What lives here | Watch out for |
| --- | --- | --- |
| `/var/lib/easyai/models/` | GGUF symlink target. The unit's `-m` arg points here. | Big files. Easy to fill the disk. |
| `/var/lib/easyai/workspace/` | The sandbox for `bash` / `*_file` tools. | The agent reads / writes here. Keep it on a partition with room. |
| `/var/lib/easyai/rag/` | The `memory` tool's long-term store (one `.md` per entry). | Tiny. Backup-friendly. See `RAG.md`. |
| `/etc/easyai/external-tools/` | Operator-defined tools (`EASYAI-*.tools`). | Operator-curated. See `EXTERNAL_TOOLS.md`. |

---

## 5. The `--external-tools` directory

The installer creates `/etc/easyai/external-tools/` (mode 750,
root:easyai) and drops a README plus `EASYAI-example.tools.disabled`
in it. The systemd unit always passes `--external-tools` so a restart
picks up new files.

**To add a tool pack:**

```bash
sudo cp my-tools.tools /etc/easyai/external-tools/EASYAI-my-tools.tools
sudo chmod 640 /etc/easyai/external-tools/EASYAI-my-tools.tools
sudo chown root:easyai /etc/easyai/external-tools/EASYAI-my-tools.tools
sudo systemctl restart easyai-server
sudo journalctl -u easyai-server -n 30 --no-pager | grep external-tools
```

**To disable without deleting:**

```bash
sudo mv /etc/easyai/external-tools/EASYAI-my-tools.tools{,.disabled}
sudo systemctl restart easyai-server
```

**Full reference:** `EXTERNAL_TOOLS.md`. Schema, recipes,
anti-patterns, troubleshooting, collaboration workflow.

---

## 6. The `memory` directory

Active by default. The systemd unit always passes
`--memory /var/lib/easyai/rag` (the legacy `--RAG` flag is still
accepted as an alias). The agent writes here at runtime — that is why
it's under `/var/lib` (mutable state) rather than `/etc` (operator
config).

**Visibility:** the `memory` tool is the model's PRIVATE long-term
memory — there is no end-user UI, command, or API to browse or read
entries. The operator can `cat` files on disk; the user talking to the
model cannot. Current builds spell this out in the `memory` tool
description itself so the model stops saying things like "check the
memory for the code" — but if you ship a custom system prompt, repeat
the rule there too.

**Quick checks:**

```bash
# How many entries does the agent have?
ls /var/lib/easyai/rag/*.md 2>/dev/null | wc -l

# What did it save most recently?
ls -lt /var/lib/easyai/rag/*.md | head -10

# What's in a specific entry?
sudo -u easyai cat /var/lib/easyai/rag/<title>.md

# Search across all entries
sudo grep -l "user-prefs" /var/lib/easyai/rag/*.md
```

**Hand-author an entry:**

```bash
sudo -u easyai bash -c 'cat > /var/lib/easyai/rag/welcome.md' <<'EOF'
keywords: user-prefs, locale

The user prefers PT-BR responses with technical jargon in English.
Keep responses terse — code over explanation.
EOF
sudo systemctl restart easyai-server
```

**Full reference:** `RAG.md`. File format, the unified
`memory(action=...)` tool, workflows, roadmap, troubleshooting.

---

## 7. Performance tuning

### Context size

```
-c 262144
```

262k tokens (256k). Paired with YaRN RoPE scaling (`rope_scaling =
yarn`, `rope_freq_scale = 2`, `yarn_orig_ctx = 131072`) to extend
models trained on shorter contexts. Keep in mind the KV cache scales
linearly with context; see below.

### KV cache quantisation

The installer defaults to:

```
-ctk q8_0 -ctv q8_0
```

This halves the KV cache footprint at no measurable quality loss
on modern models. `-ctv q4_0` halves it again at a small perplexity
cost — try if you're VRAM-bound.

### Flash attention

```
-fa
```

On by default. Free perf on every backend that supports it (CUDA,
Metal, Vulkan ≥ recent). Required for some KV-quant combinations.

### GPU layers (`--ngl`)

```
--ngl 99
```

Offload up to 99 layers to the GPU (effectively "all of them" for any
current model). The installer now pins this instead of auto-fit
(`-1`). If the model doesn't fit in VRAM, llama.cpp falls back to
partial offload at load time.

### `--mlock` and `--no-mmap`

The installer enables both. Pinning model weights in RAM prevents
the kernel paging them under memory pressure (catastrophic for token
latency). `--no-mmap` is needed for `--mlock` to be portable across
all GPU backends.

The drop-in's `LimitMEMLOCK=infinity` is what makes `mlock` actually
work (the systemd default is too small).

### `RADV_PERFTEST=gpl` (AMD Radeon)

The installer adds this `Environment=` line on AMD GPUs. Substantial
shader-compile speedup on RADV.

### CPU threads

```
-t <jobs> -tb <jobs>
```

The installer sets these to `nproc`. For a single-user agent box
that's right. For shared hardware, reduce so the agent doesn't
starve the rest of the system.

### MTP speculative decoding (`--mtp`)

When the served model was trained with Multi-Token Prediction heads
(DeepSeek V3 / V3.2, MimoVL, Qwen3-MTP, and similar), the installer's
`--mtp` flag bakes speculative decoding into the systemd unit:

```bash
./install_easyai_server.sh --mtp                 # n_max=6 (default)
./install_easyai_server.sh --mtp --mtp-n-max 8   # wider draft window
```

What ends up in `ExecStart`:

```
ExecStart=/bin/sh -c '... exec /usr/local/bin/easyai-server \
  --config /etc/easyai/easyai.ini \
  -m /var/lib/easyai/models/active.gguf \
  --spec-type draft-mtp --spec-draft-n-max 6 ...'
```

**Why this is fast.** MTP heads ship inside the main GGUF — no second
model loaded, no extra VRAM, no separate KV cache. The draft heads
predict N tokens ahead each step; the main model verifies them in
one forward pass. Typical decode-phase speedup with `n_max=6`: 1.5-2×
tok/s on single-user latency.

**When NOT to pass `--mtp`.** If the model isn't MTP-trained,
llama.cpp will refuse to load with `--spec-type draft-mtp`. You'll
see something like `error: model does not contain MTP weights` in the
startup logs and the service will fail. Skip `--mtp` and run plain
autoregressive; speculative decoding via n-grams or a classic draft
model isn't wired through the installer yet.

To remove MTP later: re-run the installer **without** `--mtp` (the
unit gets rewritten with the new flag set), or `systemctl edit
easyai-server` and strip the two flags from the `ExecStart` line.

### Installer CLI flags for sampling and engine tunables

The installer accepts flags that land in the systemd unit's
`ExecStart` and/or the generated `easyai.ini`. Override any of them
on the installer command line:

```bash
./install_easyai_server.sh \
    --ctx-size 262144 \
    --ngl 99 \
    --temperature 0.2 \
    --top-p 0.92 \
    --top-k 50 \
    --min-p 0.03 \
    --repeat-penalty 1.04 \
    --presence-penalty 0.1 \
    --frequency-penalty 0.05 \
    --max-tokens 12288 \
    --split-mode none \
    --rope-scaling yarn \
    --rope-scale 2 \
    --yarn-orig-ctx 131072
```

Full flag table (showing current defaults):

| Flag | Default | Description |
| --- | --- | --- |
| `--ctx-size` | 262144 | Context window in tokens. |
| `--ngl` | 99 | GPU layers to offload. |
| `--temperature` | 0.2 | Sampling temperature. |
| `--top-p` | 0.92 | Nucleus sampling cutoff. |
| `--top-k` | 50 | Top-k sampling cutoff. |
| `--min-p` | 0.03 | Min-p sampling threshold. |
| `--repeat-penalty` | 1.04 | Repetition penalty. |
| `--presence-penalty` | 0.1 | Presence penalty. |
| `--frequency-penalty` | 0.05 | Frequency penalty (proportional to count). |
| `--max-tokens` | 12288 | Max tokens per response. |
| `--split-mode` | `none` | GPU split strategy (`none` / `layer` / `row` / `tensor`). |
| `--rope-scaling` | `yarn` | RoPE scaling method (`none` / `linear` / `yarn`). |
| `--rope-scale` | 2 | RoPE frequency scaling factor. |
| `--yarn-orig-ctx` | 131072 | Original training context length for YaRN. |

---

## 8. Common gotchas

### `mlock failed: Cannot allocate memory`

`LimitMEMLOCK` isn't `infinity`. Re-run the installer with `--upgrade`
to refresh the drop-in, or edit `systemctl edit easyai-server`
manually.

### `n_gpu_layers already set by user, abort`

The pinned `--ngl` value doesn't fit. Lower it to match your VRAM, or
set `--ngl -1` to let llama.cpp auto-fit.

### "model not found" on startup

The unit's `-m` arg points at a symlink that doesn't resolve. Check:

```bash
ls -la /var/lib/easyai/models/current.gguf
```

The installer accepts a `--model` flag to swap; or `ln -sfn` the
target manually.

### GTT exhaustion on AMD Radeon (Vulkan/RADV)

Symptom: large model load → GPU hangs → `journalctl -k` shows
`amdgpu: ttm pool full`.

Fix: increase the GTT page limit in your boot config (kernel param
`amdgpu.gttsize` or `ttm.pages_limit`). The installer manages this
for you — re-run with `--gtt N` (default **29 GiB**, was 28 in
earlier installs) and reboot. Gustavo's MINISFORUM UM690L (Radeon
680M, 32 GB system RAM) currently runs at `ttm.pages_limit=7602176`
(29 GiB GTT) — leaves enough headroom for a Q5_K_M / MXFP4_MOE 30B
MoE plus a 32k KV cache fully on the iGPU.

Since 2026-05-12: re-running the installer with a **different**
`--gtt` value now rewrites the existing `ttm.pages_limit=N` token in
`/etc/default/grub` and re-runs `update-grub`, instead of bailing
with "already present; skipping" and leaving the stale value behind.
After reboot, verify with `cat /proc/cmdline` (or `grep ttm.pages
/proc/cmdline`).  Same `--gtt` value across re-runs still no-ops.

### Model uses a tool that's disabled in the INI

Symptom: `allow_bash = off` in `easyai.ini`, but the model still
emits a `bash` tool call (which the server rejects, model retries,
loop).

Cause: pre-2026-05 builds had two bugs that surfaced here. Both
fixed in current builds:

1. The server read `allow_fs` from the INI but never propagated it
   to the toolbelt — a non-empty `sandbox` re-enabled the `fs` tool
   even with `allow_fs = off`. Now `allow_fs` / `allow_bash` are
   honoured independently of `sandbox`.
2. The built-in system prompt named every tool by hand
   (`fs`, `bash`, `plan`, …) regardless of whether they were
   registered. Models then hallucinated calls to disabled tools.
   Now the built-in prompt only mentions tools that are actually
   registered for the current invocation.

If you supply your OWN system prompt via `[SERVER] system_file`
(`/etc/easyai/system.txt`), the server cannot rewrite it for you —
remove any references to tools you've gated off. Verify with:

```bash
sudo -u easyai easyai-server --config /etc/easyai/easyai.ini \
                             --show-system-prompt | grep -E 'bash|fs_'
```

If you see `bash` / `*_file` listed and the corresponding INI flag is
`off`, edit `system.txt` to drop those lines.

### Webui blank / shows yesterday's UI

Browser cache. **Cmd+Shift+R** (Linux: Ctrl+Shift+R) to hard-reload.
The bundle is hashed so a stale CSS file is the usual culprit.

### Coredump under load

Already wired by the installer:

- `systemd-coredump` package installed
- `LimitCORE=infinity` in the drop-in
- Coredumps land in `/var/lib/systemd/coredump/`

To examine:

```bash
coredumpctl list easyai-server.service
coredumpctl gdb <PID>
```

### "external-tools dir does not exist"

The path doesn't exist. Re-run installer with `--upgrade`, or:

```bash
sudo install -d -o root -g easyai -m 750 /etc/easyai/external-tools
sudo systemctl restart easyai-server
```

### Agent doesn't seem to remember anything

Either the `memory` tool isn't enabled or the dir is wrong:

```bash
journalctl -u easyai-server | grep "memory enabled"
```

If absent, re-run installer or check `systemctl cat` for
`--memory` (or the legacy `--RAG`).

---

## 9. Hitting the API

The server speaks **three** API dialects so most AI clients work
unchanged. Endpoints:

| Verb | Path | API | Notes |
| --- | --- | --- | --- |
| GET | `/` | webui | embedded webui |
| GET | `/health` | easyai | `{model, backend, tools, preset, compat:{...}}` |
| GET | `/v1/models` | OpenAI | OpenAI-shape list-models |
| POST | `/v1/chat/completions` | OpenAI | the workhorse — streaming SSE, tools, sampling controls |
| POST | `/v1/preset` | easyai | swap the ambient preset |
| GET | `/v1/tools` | easyai | tool catalogue for the webui popover |
| GET | `/api/tags` | Ollama | Ollama-shape list-models (LobeChat, OpenWebUI in Ollama mode, etc.) |
| GET/POST | `/api/show` | Ollama | Ollama-shape model detail |
| POST | `/mcp` | MCP | JSON-RPC 2.0 — the full tool catalogue exposed to other AI apps. See `MCP.md`. |
| GET | `/mcp` | MCP | reserved for future SSE notifications; currently `405 Method Not Allowed`. |

### Bearer auth (if `/etc/easyai/api_key` present)

```bash
curl -H "Authorization: Bearer $(sudo cat /etc/easyai/api_key)" \
     http://localhost/health
```

### One-shot completion via curl

```bash
curl -sN \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $TOKEN" \
  -X POST http://localhost/v1/chat/completions \
  -d '{
    "model": "easyai",
    "messages": [
      {"role": "user", "content": "Show me last week’s deploys."}
    ],
    "stream": true
  }'
```

The server streams SSE deltas. Tools fire automatically — no special
client setup required.

### Calling from the OpenAI Python SDK

```python
from openai import OpenAI

client = OpenAI(
    base_url="http://localhost/v1",
    api_key="my-token",
)

resp = client.chat.completions.create(
    model="easyai",
    messages=[{"role": "user", "content": "What's our deploy status?"}],
    stream=True,
)
for chunk in resp:
    print(chunk.choices[0].delta.content or "", end="")
```

The model dispatches whatever tools the operator declared on the
server side (built-ins + `--external-tools` + the `memory` tool).

### `X-Easyai-Inject: off` to skip date/time injection

Useful for A/B regression suites:

```bash
curl -H "X-Easyai-Inject: off" ...
```

---

## 10. Health checks and verification

### Server alive?

```bash
curl -fsS http://localhost/health | jq .
```

```json
{
  "status": "ok",
  "model": "easyai",
  "backend": "cuda|metal|vulkan|cpu",
  "tool_count": 12,
  "preset": "balanced"
}
```

### Tools registered?

```bash
curl -fsS http://localhost/health | jq .tool_count
```

Expected (rough):

- 3 (datetime, the unified `web` tool, plan)
- + 1 (`--memory`: the unified `memory(action=...)` tool)
- + 1 (`--allow-fs`: the unified `fs` tool)
- + 1 (`--allow-bash`: bash)
- + N (your `--external-tools` packs)
- + M (tools fetched via `--mcp` from an upstream MCP server)
- `--use-google` enables `engine="google"` *inside* the unified `web`
  tool — does NOT add a new entry to the catalogue.

### `memory` tool working?

```bash
ls /var/lib/easyai/rag/
journalctl -u easyai-server | grep "memory enabled"
```

### External tools loaded?

```bash
journalctl -u easyai-server | grep external-tools
```

Expected:

```
easyai-server: loaded N external tool(s) from M file(s) in /etc/easyai/external-tools
```

If a file failed to parse:

```
easyai-server: [external-tools] error: /etc/easyai/external-tools/EASYAI-foo.tools: ...
```

---

## 11. Backup, restore, migration

### What to back up

| Path | Frequency | Why |
| --- | --- | --- |
| `/etc/easyai/` | on change | system prompt, api key, external tools |
| `/var/lib/easyai/rag/` | regular | the agent's accumulated knowledge |
| `/var/lib/easyai/workspace/` | maybe | depends what you let the agent write here |
| `/var/lib/easyai/models/` | usually no | GGUFs are big and re-downloadable |

### One-shot snapshot

```bash
sudo tar -czf easyai-backup-$(date +%F).tar.gz \
    /etc/easyai \
    /var/lib/easyai/rag \
    /var/lib/easyai/workspace \
    /etc/systemd/system/easyai-server.service.d
```

### Restoring on a fresh install

1. Run `install_easyai_server.sh` on the new host (creates user, dirs,
   unit).
2. `sudo systemctl stop easyai-server`.
3. Untar over `/`:
   ```bash
   sudo tar -xzpf easyai-backup-*.tar.gz -C /
   ```
4. Fix ownership:
   ```bash
   sudo chown -R easyai:easyai /var/lib/easyai/rag /var/lib/easyai/workspace
   ```
5. `sudo systemctl start easyai-server`.

---

## 12. Upgrading

```bash
cd ~/easy
git pull
./scripts/install_easyai_server.sh --upgrade --enable-now
```

`--upgrade`:

- Refreshes `/usr/bin/easyai-*` and `/usr/lib/easyai/`
- Re-renders the systemd unit (so flag changes propagate)
- WILL refresh `/etc/easyai/system.txt_template` (the canonical
  factory copy)
- Does NOT touch `/etc/easyai/system.txt` (operator-supplied; not
  installed by default), `/etc/easyai/api_key`,
  `/var/lib/easyai/rag/*` — your data is safe
- Does NOT touch `/etc/easyai/external-tools/*` — your manifests
  are safe
- WILL refresh the README in `external-tools/` and the
  `EASYAI-example.tools.disabled` sample

`--enable-now`:

- `systemctl enable easyai-server` (auto-start on boot)
- `systemctl start easyai-server`

After upgrading, sanity-check:

```bash
journalctl -u easyai-server -n 50 --no-pager | grep -E "memory enabled|external-tools|loaded"
```

---

## 13. Uninstalling

The installer doesn't ship an uninstaller (yet). Manual:

```bash
sudo systemctl disable --now easyai-server
sudo rm /etc/systemd/system/easyai-server.service
sudo rm -rf /etc/systemd/system/easyai-server.service.d
sudo systemctl daemon-reload

sudo rm -f /usr/bin/easyai-server /usr/bin/easyai-cli /usr/bin/easyai-local
sudo rm -rf /usr/lib/easyai/

# data — back up first if you might want it back
sudo rm -rf /var/lib/easyai/

# config — same
sudo rm -rf /etc/easyai/

# user
sudo userdel easyai
```

---

## 14. Troubleshooting

### Server won't start, journal shows "model not found"

```bash
ls -la /var/lib/easyai/models/
```

The unit's `-m` arg points at a symlink. If broken, re-symlink:

```bash
sudo -u easyai ln -sfn /path/to/your.gguf /var/lib/easyai/models/current.gguf
sudo systemctl restart easyai-server
```

### "Permission denied" writing to `--sandbox`

The agent runs as `easyai`. The dir must be `easyai`-writable:

```bash
sudo chown -R easyai:easyai /var/lib/easyai/workspace
sudo chmod 750 /var/lib/easyai/workspace
```

### High RSS / OOM-killer killing easyai-server

Likely the model is mmap'd but pinned, or the KV cache is huge. Check:

```bash
ps -o rss,cmd -p $(systemctl show -p MainPID --value easyai-server)
```

If RSS approaches your physical RAM, drop `-ctv q8_0` to `-ctv q4_0`,
or shrink `-c 262144` to something smaller.

### Webui seems to lose connection mid-stream

`X-Accel-Buffering: no` is required if you have nginx in front of
easyai-server. The server sets it on streams, but a misconfigured
proxy can override.

If the proxy itself disconnects on long thinking turns, raise its
read/write timeout to match the server's `--http-timeout` (default
600 s; bump higher if you run thinking-heavy models). The nginx
recipe in `manual.md` §6.1 uses `proxy_read_timeout 1800`; pick
whichever is highest among nginx, `--http-timeout`, and the
client's `--timeout`.

### Client logs `[easyai-cli] HTTP attempt N/M failed`

Expected behaviour, not a bug. The libeasyai-cli HTTP layer retries
transient transport failures (default 5 extra attempts, exponential
backoff) and logs every retry to stderr unconditionally so an
operator reading journalctl sees the pattern without `--verbose`.
If a long sequence of retries appears, the upstream is genuinely
flapping; check the server-side journal for matching exceptions or
`HTTP 408 timeout` warnings (also logged unconditionally) to see
which side is dropping the connection.

The same retry-and-log pattern applies to the MCP client
(`[easyai-mcp]` prefix) when `--mcp <url>` points at a flaky
upstream, and to the unified `web` tool's libcurl calls (`[easyai-web]`
prefix). Configurable via `--http-retries N` (default 5, set 0 to disable).

### External tool calls hang forever

The tool's `timeout_ms` is too high (cap is 5 min). Edit the
`.tools` file, lower `timeout_ms`, restart the server.

### `memory` entries appear duplicated

Two things to check:

1. Is the same dir mounted twice? `mount | grep var/lib/easyai`.
2. Are two easyai-server processes running? `pgrep -af easyai-server`
   should return exactly one.

### Coredump

```bash
coredumpctl list easyai-server.service
coredumpctl gdb <PID>
```

In gdb: `bt`, `bt full`, `info threads`, `thread apply all bt`. File
the bug at the easyai issue tracker with the trace.

### Verbose logs

Re-run installer with `--enable-verbose`, OR transient:

```bash
sudo systemctl edit --runtime easyai-server
# Add:
[Service]
Environment=EASYAI_VERBOSE=1
```

Then `daemon-reload && restart`. Drop the override when done.

---

*See also:* `RAG.md`, `EXTERNAL_TOOLS.md`, `manual.md`, `design.md`,
`SECURITY_AUDIT.md`.
