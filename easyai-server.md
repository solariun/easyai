# easyai-server — the OpenAI-compatible chat server

> **A drop-in `llama-server` replacement that loads a GGUF, exposes
> `/v1/chat/completions` (streaming SSE + tool calls), serves an
> embedded SvelteKit webui, and answers MCP / Ollama clients out of the
> same process.** Single binary, hardened systemd unit, central INI
> config, keyword-indexed knowledge tools (a passive RAG technique),
> operator-defined external tools.

---

## Table of contents

1. [Configuration — `/etc/easyai/easyai.ini`](#1-configuration--etceasyaieasyaiini)
2. [Command-line flags](#2-command-line-flags)
3. [API endpoints](#3-api-endpoints)
4. [Tool gating + sandbox](#4-tool-gating--sandbox)
5. [Authentication](#5-authentication)
6. [The default persona — Deep](#6-the-default-persona--deep)
7. [Webui customisation](#7-webui-customisation)
8. [Performance tuning](#8-performance-tuning)
9. [Verbose observability](#9-verbose-observability)
10. [Hardening / security](#10-hardening--security)
11. [Cross-references](#11-cross-references)

---

## 1. Configuration — `/etc/easyai/easyai.ini`

Every CLI flag `easyai-server` accepts has a matching INI entry. The
systemd unit's `ExecStart` is intentionally short — operators tweak
this file and restart, not the unit.

### File location

| Path | Notes |
| --- | --- |
| `/etc/easyai/easyai.ini` | Default — what the installer drops and what the systemd unit reads via `--config`. |
| `<other path>` | Pass `--config /path/to.ini` on the binary's command line. |

The file is owned `root:easyai`, mode `640` — readable by the
service user, world-unreadable.

### Precedence

```
   CLI flag  >  [MODEL_<match>]  >  [ENGINE]  >  hardcoded default
   (highest)     (per-model)        (this file)     (in the binary)
```

`[MODEL_<pattern>]` sections (see below) sit between CLI and
`[ENGINE]` — they let per-model overrides apply without touching the
global section. If the operator passed `--port 8080` in the systemd
unit AND the INI says `port = 9090`, the server listens on **8080**.
Drop the CLI flag and the INI value takes over.

### Sections

| Section | Purpose | Status |
| --- | --- | --- |
| `[SERVER]` | HTTP layer, paths, tool gating, MCP auth posture | active |
| `[ENGINE]` | Model loading + inference tunables | active |
| `[MODEL_<pattern>]` | Per-model `[ENGINE]` overrides matched by model filename | active |
| `[MCP_USER]` | Bearer-token auth for `/mcp` (one user per line) | active |
| `[TOOLS]` | Per-tool ACL (`mcp_allowed = …`) | reserved for a future release |

The `[TOOLS]` section is recognised by the parser but **not yet
consumed** by code. Populating it does no harm; the keys are ignored
until a future release wires them.

### Format quick reference

```ini
# comments start with # or ;
; both work

[Section]
key = value
key2 = "value with spaces or trailing whitespace"   ; quotes optional
empty_key =
```

- Whitespace around `=`, section names, keys, and values is trimmed.
- Boolean values accept: `on` / `off`, `true` / `false`, `yes` / `no`,
  `1` / `0`, `enable` / `disable`, `enabled` / `disabled`.
- Surrounding double-quotes on values are stripped, so values can
  preserve internal whitespace by being quoted.
- Duplicate keys within a section: last value wins.
- Missing file: server starts with hardcoded defaults.

### `[SERVER]`

The HTTP layer, paths, tool gating, MCP auth.

| Key | Type | CLI equivalent | Default | Notes |
| --- | --- | --- | --- | --- |
| `model` | path | `-m`, `--model` | (none — REQUIRED) | GGUF file the engine loads. |
| `host` | string | `--host` | `127.0.0.1` | Bind address. `0.0.0.0` to listen on every interface. |
| `port` | int | `--port` | `8080` | TCP port. |
| `alias` | string | `-a`, `--alias` | basename of `model` | Public model id reported by `/v1/models` and `/api/tags`. |
| `sandbox` | path | `--sandbox` | (none) | Root directory for `bash` and `*_file` tools. **Auto-registers the `*_file` tools** when set (no need to also pass `--allow-fs`). `bash` still requires `--allow-bash`. |
| `system_file` | path | `-s`, `--system-file` | (none — uses built-in default) | File containing the server-default system prompt. |
| `system_inline` | string | `--system` | (none) | Inline system prompt. Beats `system_file` if both are set. |
| `external_tools` | path | `--external-tools` | (none — feature off) | Directory of `EASYAI-*.tools` manifests. See `EXTERNAL_TOOLS.md`. |
| `memory` | path | `--memory` | (none — feature off) | Directory of knowledge entries. Also triggers per-request injection of a compact `# MEMORY VOCABULARY` block into the AUTHORITATIVE preamble so the model sees its current keyword index without having to call `keywords_knowledge`. The legacy key `rag` (CLI `--RAG`) is still read for back-compat. See `RAG.md` §5 "Automatic vocabulary injection" and `design.md` §5c. |
| `webui_title` | string | `--webui-title` | `Deep` | Document title pinned in the embedded webui. |
| `webui_icon` | path | `--webui-icon` | (none) | `.ico` / `.png` / `.svg` / `.gif` / `.jpg` / `.webp`. |
| `webui_mode` | enum | `--webui` | `modern` | `modern` (embedded llama-server bundle) or `minimal` (inline). |
| `webui_placeholder` | string | `--webui-placeholder` | `Type a message…` | Input box hint. |
| `metrics` | bool | `--metrics` | `off` | Expose Prometheus `/metrics`. |
| `verbose` | bool | `-v`, `--verbose` | `off` | Noisy logs. Enables HTTP-level `→` / `←` lines per request (with status, duration, bytes, running totals). The periodic `METRICS` line is **independent of verbose** — see `metrics_interval` below. |
| `metrics_interval` | int | `--metrics-interval` | `300` | Periodic METRICS log line every N seconds, **ALWAYS ON regardless of `verbose`** since 2026-05-09. Reports CPU%, iowait%, load avg, process RSS + peak, system mem, GPU GTT (Linux/AMD), HTTP in-flight + cumulative reqs / err / bytes, fd usage, AND TCP state breakdown with **explicit `TIME_WAIT N/M ephemeral ports (X.X% [elevated\|HIGH\|CRITICAL])`** so socket exhaustion shows up before connections fail. `0` disables. Default `300` (5 min) — low-overhead enough to leave on permanently; bump down (60, 30, 5) when actively troubleshooting. Lives outside Prometheus `/metrics` so you can tail it from journalctl. |
| `allow_fs` | bool | `--allow-fs` | `off` | Register the unified `fs` tool (action=`read` / `write` / `list` / `glob` / `grep` / `check_path` / `cwd` / `sandbox`). **`--sandbox` ALONE no longer implies `--allow-fs`** (the sandbox is also the cwd / external-tools root / `fs(action="sandbox")` target — operators legitimately set it while keeping the `fs` tool off). Pass `--allow-fs` explicitly. `--allow-bash` still implies `fs` (bash strictly subsumes it). |
| `allow_bash` | bool | `--allow-bash` | `off` | Register the `bash` tool. **Not** a hardened sandbox. Note: on the server, `--allow-bash` alone does NOT auto-register `fs` — pass `--allow-fs` alongside if you want both. (The cli / local helpers DO auto-register `fs` whenever `--allow-bash` or `--allow-python` is on, since they treat the operator's intent as "let the model touch files".) |
| `allow_python` | bool | (no `--allow-python`; `--no-python` flips off) | `on` | Register the compute tool — runs Python 3 snippets via `python3 -I -S -E -c <code>`. **Model-facing name: `evaluate`** (renamed 2026-05-26 from `python3` to defeat the model's "python = write files" training prior; `canonical_tool_name("python3")` still aliases to `evaluate` for back-compat). **Defaults ON**, auto-registered whenever `--sandbox` is set or `--allow-bash` is on. Isolated stdlib-only interpreter: no PYTHON* env, no site-packages, no cwd on `sys.path`; third-party imports fail with ModuleNotFoundError. **READ-ONLY disk surface**: the Python preamble wraps `builtins.open` / `io.open` / `os.open` to reject (a) any path resolving outside the sandbox root AND (b) any write-mode call (`'w'/'a'/'x'/'+'` mode chars, or `O_WRONLY|O_RDWR|O_CREAT|O_TRUNC|O_APPEND` flags) regardless of path. PermissionError points the model at the filesystem write tool registered this session. Defense-in-depth, not a hardened sandbox: `import os` / `import socket` / `import subprocess` / `import ctypes` all still work (closure-cell introspection also bypasses — see SECURITY_AUDIT §23.2). Pass `--no-python` (or `[SERVER] allow_python = off`) to skip registration. |
| `use_google` | bool | `--use-google` | `off` | Enable `engine="google"` inside the unified `web` tool (Google Custom Search JSON API), and let the default `engine="auto"` cascade try google as its first hop. Requires `GOOGLE_API_KEY` and `GOOGLE_CSE_ID` env vars. Counts against your Google quota (free tier: 100 queries/day per key). When either env var is missing the auto cascade silently skips google and falls through to brave → ddg-lite → bing → ddg, all four keyless. |
| `mcp` | string | `--mcp` | (none — MCP client off) | URL of an upstream MCP server to connect to as a CLIENT. Format: `http(s)://host:port` (the `/mcp` endpoint is appended). Tools fetched from the upstream are merged into the local catalogue; local-tool names take precedence on collision. Failure at startup logs a warning and continues with whatever local / knowledge tools were registered. |
| `mcp_token` | string | `--mcp-token` | (empty) | Bearer token sent on every request to the upstream `mcp` URL. Empty = no `Authorization` header — appropriate when the upstream is in open mode. Don't put a real token in the INI directly if you can help it; load it from a separate file (analogous to how `api_key` is wired through `${EASYAI_API_KEY}` in the systemd installer). |
| `http_retries` | int | `--http-retries` | `5` | Extra attempts on transient HTTP failures. Applies to the MCP client (`--mcp` upstream calls) and to the unified `web` tool's libcurl calls. 4xx never retries; 5xx + connect/read/write errors do. Each retry logs to stderr unconditionally (e.g. `[easyai-mcp] http://up:8089/mcp attempt 2/6 failed (Couldn't connect to server); retrying in 500ms`). 0 disables. |
| `http_timeout` | int | `--http-timeout` | `600` | Read/write timeout (seconds) for **both** the listen socket (cpp-httplib) AND the MCP-client connection. Bumped from llama-server's traditional 60 s default to give long-thinking models room to breathe before the network drops them. Echoed in the startup banner. HTTP 408/504 timeouts hit by the listen socket are logged unconditionally as `[easyai-server] WARN HTTP 408 timeout on POST /v1/chat/completions from CLIENT (check --http-timeout, …)`. |
| `local_tools` | bool | `--no-local-tools` (negative) | `on` | Master switch for the LOCAL built-in toolbelt (datetime, web, fs, bash, ...). Set `off` (or pass `--no-local-tools`) to register zero local default tools. Has no effect on the knowledge tools, external tools, or remote tools fetched via `mcp` — those have their own switches. `allow_fs` / `allow_bash` / `use_google` further opt in. **Renamed from `load_tools` / `--no-tools`** to make clear the MCP client (`mcp`) is unaffected. |
| `max_body` | int | `--max-body` | `8388608` (8 MiB) | Max HTTP request body size. |
| `api_key` | string | `--api-key` | (none — `/v1/*` open) | Bearer token for `/v1/*`. Don't put real keys in INI directly — use `/etc/easyai/api_key` (file-based, the installer wires `${EASYAI_API_KEY}`). |
| `mcp_auth` | enum | (no CLI; `--no-mcp-auth` overrides) | `auto` | `auto` (auth iff `[MCP_USER]` non-empty), `on` (force require), `off` (force open). |
| `no_think` | bool | `--no-think` | `off` | Strip `<think>` tags from responses. |
| `inject_datetime` | bool | `--inject-datetime` | `on` | Authoritative date/time + knowledge-cutoff injection in the system prompt. |
| `knowledge_cutoff` | string | `--knowledge-cutoff` | `2024-10` | YYYY-MM hint for the model. |
| `reasoning` | bool | `--reasoning on/off` | `on` | Honour the model's reasoning channel. |

### `[ENGINE]`

Model loading and inference tunables.

| Key | Type | CLI equivalent | Default | Notes |
| --- | --- | --- | --- | --- |
| `context` | int | `-c`, `--ctx` | `262144` | Context window size in tokens. |
| `ngl` | int | `--ngl` | `99` | GPU layers. `-1` = auto, `0` = CPU only, `99` = force all on GPU (will OOM if it doesn't fit). |
| `split_mode` | string | `-sm`, `--split-mode` | `none` | Multi-GPU split strategy. `none` (single GPU), `layer` (split layers across GPUs, llama.cpp default), `row` (tensor parallelism), `tensor` (full tensor parallelism). |
| `threads` | int | `-t`, `--threads` | `0` (lib default) | CPU threads for prompt processing. |
| `threads_batch` | int | `-tb`, `--threads-batch` | `0` (lib default) | CPU threads for batched inference. |
| `batch` | int | `--batch` | `0` (follows ctx) | Logical batch size. |
| `parallel` | int | `-np`, `--parallel` | `1` | Llama-server compat. |
| `preset` | string | `--preset` | `precise` | `deterministic` / `precise` / `balanced` / `creative` / `wild`. See the [Sampling presets table](#sampling-presets--the-five-built-ins) below for what each implies. The installer drops `#preset = <name>` (commented) in the generated INI so the engine picks the name from `--preset` instead. |
| `flash_attn` | bool | `-fa`, `--flash-attn` | `off` | Free perf on every backend that supports it. |
| `mlock` | bool | `--mlock` | `off` | Pin model weights in RAM. Needs `LimitMEMLOCK=infinity` on the unit. |
| `no_mmap` | bool | `--no-mmap` | `off` | Required with `mlock` for portability. |
| `no_kv_offload` | bool | `-nkvo`, `--no-kv-offload` | `off` | Keep KV cache on CPU even with GPU layers. |
| `kv_unified` | bool | `--kv-unified` | `off` | Single unified KV buffer across sequences. |
| `rope_scaling` | string | `--rope-scaling` | `yarn` | RoPE positional-encoding scaling for context extension beyond the model's training length. `none` (no scaling), `linear` (linear RoPE scaling), `yarn` (YaRN scaling). |
| `rope_freq_scale` | float | `--rope-scale` | `2` | Scaling factor for RoPE frequencies. A value of 2 doubles the effective context window. |
| `yarn_orig_ctx` | int | `--yarn-orig-ctx` | `131072` | The model's original training context length before YaRN extension. Needed when `rope_scaling=yarn` so the algorithm knows the boundary between trained and extended positions. |
| `cache_type_k` | enum | `-ctk`, `--cache-type-k` | `f16` | `f32 / f16 / bf16 / q8_0 / q4_0 / q4_1 / q5_0 / q5_1 / iq4_nl`. |
| `cache_type_v` | enum | `-ctv`, `--cache-type-v` | `f16` | Same options as K. Quantising V saves a lot of VRAM. |
| `numa` | string | `--numa` | (none) | Llama-server compat. |
| `override_kv` | list | `--override-kv` (repeat) | (empty) | GGUF metadata overrides. Comma-separated list of `key=type:value` triples (e.g. `tokenizer.ggml.eos_token_id=int:151645`). On the CLI, repeat `--override-kv` per entry; in INI, comma-separate. |
| `spec_type` | enum | `--spec-type` | `none` | Speculative decoding backend. `none` (off), `draft-mtp` (MTP heads embedded in the main model — requires MTP-trained model), `draft-simple` (classic standalone draft model — requires `--draft-model PATH`), `draft-eagle3` (Eagle3 draft model), `ngram-simple` / `ngram-map-k` / `ngram-map-k4v` / `ngram-mod` / `ngram-cache` (self-speculative via n-grams). Unknown strings are recorded in `Engine::last_error()` and leave speculation off. |
| `spec_draft_n_max` | int | `--spec-draft-n-max` | (llama.cpp default: 16) | Max draft tokens per speculation step. Typical for MTP: `6`. Set to `0` to defer to llama.cpp's default; ignored when `spec_type=none`. |
| `spec_draft_model` | path | `--draft-model` | (empty) | GGUF path for the standalone draft model (`draft-simple` / `draft-eagle3`). Must share vocabulary with the target model. Ignored when `spec_type` is `none`, `draft-mtp`, or `ngram-*`. |
| `chat_template_file` | path | `--chat-template-file` | (empty → embedded) | Override the chat template embedded in the GGUF with a Jinja file on disk. Mirrors `llama-server --chat-template-file`. The file is read once at load. Useful for shipping a tuned Qwen3 thinking template (e.g. `qwen3-think.jinja`) without rebuilding the GGUF. Read errors abort startup. |
| `reasoning_format` | enum | `--reasoning-format` | `auto` | How to extract reasoning content: `none` (leave `<think>` inline), `auto` (default; currently behaves like `deepseek`), `deepseek` (extract `<think>…</think>` into `message.reasoning_content`, including during streaming — the Qwen3 / R1 default), `deepseek-legacy` (extract into `reasoning_content` for sync, leave inline for streaming — old behaviour). Unknown names fall back to `none`. |
| `temperature` | float | `--temperature`, `--temp` | `0.2` | Sampling temperature. |
| `top_p` | float | `--top-p` | `0.92` | Nucleus sampling threshold. |
| `top_k` | int | `--top-k` | `50` | Top-K sampling. |
| `min_p` | float | `--min-p` | `0.03` | Minimum probability threshold. |
| `repeat_penalty` | float | `--repeat-penalty` | `1.04` | Repetition penalty — *multiplicative* on logits of recently-seen tokens. Anti-loop safety net for thinking models that lock into rephrasing their own intent ("I'll write X / Let me write X / OK, creating X" forever). Set `1.0` to disable. Pairs naturally with `presence_penalty=0`; the production AI box flips that pairing (see next row). |
| `presence_penalty` | float | `--presence-penalty` | `0.1` | Presence penalty (OpenAI semantics, range `[-2.0, 2.0]`) — *additive*, fixed cost per token that has appeared *at all* in the recent window, regardless of count. Discourages topic stickiness without penalising literal tool-name repetition. The installer ships `1.5` paired with `repeat_penalty=1.0` because long agentic flows (10+ tool hops) tested better with that pairing than with `repeat_penalty=1.15` alone — `repeat_penalty` was making the model paraphrase tool names like `fs` after the third call, breaking dispatch. See [`design.md` §4b](design.md#4b-sampling-and-the-penalty-stack) for the full rationale. Persists across requests (no per-request override path). |
| `frequency_penalty` | float | `--frequency-penalty` | `0.05` | Per-token penalty proportional to how many times a token has already appeared (range `[0.0, 2.0]`). Unlike `presence_penalty` (flat cost per seen token), this scales with repetition count. `0.0` = disabled. |
| `max_tokens` | int | `--max-tokens` | `12288` | Per-turn cap. |
| `seed` | uint32 | `--seed` | `0` (random) | RNG seed. |
| `max_incomplete_retries` | int | `--max-incomplete-retries` | `10` | How many times the engine discards + nudges + retries when the model finishes a turn with no tool_call and only an "announce" snippet ("Let me…", "I'll…"). `0` disables retries (equivalent to `retry_on_incomplete = off`). Bump to 15-20 for weak / 1-bit-quant models that keep announcing-without-acting. Each retry surfaces in the webui Thinking panel as `↻ Retry N/max`. |

### Sampling presets — the five built-ins

A **preset** is a named bundle of sampling parameters (`temperature`,
`top_p`, `top_k`, `min_p`). Five ship with easyai; the one the server
runs on comes from `--preset` / `[ENGINE] preset` and is exposed in
the webui as a `default` badge so operators don't have to remember
the specific numbers.

| Preset | temp | top_p | top_k | min_p | Behaviour |
|---|---|---|---|---|---|
| **`deterministic`** | 0.0 | 1.00 | 1 | 0.00 | Greedy — same prompt → identical answer every time. Reproducibility, regression tests, anything piped into a parser. |
| **`precise`** (default) | 0.2 | 0.95 | 40 | 0.10 | High-confidence tokens only. Best for code, math, factual Q&A, tool-calling agents, structured output. The installer's baseline. |
| **`balanced`** | 0.7 | 0.95 | 40 | 0.05 | Some phrasing variety, still focused. General-purpose chat, summarisation. |
| **`creative`** | 1.0 | 0.95 | 40 | 0.05 | Wider phrasing, surprising word choices. Brainstorming, fiction, marketing copy. Code/math get worse. |
| **`wild`** | 1.4 | 0.98 | 60 | 0.00 | Maximum entropy. Frequent off-topic, contradictions, hallucinations. Pure exploration only. |

Aliases (case-insensitive, recognised by `easyai::find_preset()`):
`exact` → `precise`, `default` → `balanced` (library alias only —
NOT the same as the webui's "default" badge, which resolves
dynamically), `fun` → `creative`, `chaos` → `wild`, `greedy` →
`deterministic`.

Where the preset name shows up across the server:

| Surface | What it does |
|---|---|
| `install_easyai_server.sh --preset NAME` | Bakes the chosen name into the INI template. The line itself stays commented (operator un-comments to pin); explicit `temperature`/`top_k`/... overrides below WIN when both are set. |
| `/etc/easyai/easyai.ini` `[ENGINE] preset` | Server reads at startup. Survives restarts. |
| `easyai-server --preset NAME` | Wins over INI for THIS launch. |
| `GET /health` `.preset` field | Liveness probe reports the active preset name. |
| `POST /v1/preset` body `{"preset":"NAME"}` | Live swap. Sets the server-wide ambient default for every subsequent request — no restart. |
| Webui **`default` badge** | First button in the tone bar. Resolves to the server's currently-active preset (read at page-load from the values baked into the bundle's injected JS). New sessions start here when `localStorage` has no prior choice; existing sessions keep whatever the user last cycled to. |
| Webui named badges | `deterministic` / `precise` / `balanced` / `creative` / `wild` — per-session client-side override. Affects only requests THIS browser tab sends; doesn't change the server's ambient default. Persists in `localStorage`. |
| Inline preset command | First word in the user's message (`creative 0.9 …`, `precise …`). `parse_preset()` peels the prefix; the rest becomes the actual prompt. Per-turn override. |

The explicit per-knob overrides (`temperature`, `top_p`, `top_k`,
`min_p`, …) WIN over the preset's baseline values when both are set,
so `preset = precise` + `temperature = 0.5` yields precise's
top_p/top_k/min_p with temperature bumped to 0.5. That's how the
installer's default config works — `preset` left commented, with
explicit `temperature = 0.5` / `top_k = 64` / `presence_penalty =
1.5` tuned for long agentic flows on the AI box (see
[`design.md` §4b](design.md#4b-sampling-and-the-penalty-stack)).

### `[MODEL_<pattern>]` — per-model ENGINE overrides

A `[MODEL_<pattern>]` section lets you override any `[ENGINE]` key for
a specific model without touching the global section. The server
resolves the loaded model's filename (following symlinks), strips the
path and extension to get the **model name**, then scans all
`[MODEL_*]` sections. The `MODEL_` prefix is stripped to get a
pattern, and a **case-insensitive substring match** is performed
against the model name. When multiple sections match, the **longest
matching pattern wins**.

Keys inside a `MODEL_` section are the same as `[ENGINE]` keys — only
include keys you want to override; omitted keys keep their `[ENGINE]`
value.

**Precedence:** `CLI flags > [MODEL_<match>] > [ENGINE] > hardcoded defaults`

Example: loading `Qwen3-Coder-Next-Q6_K_M.gguf` with sections
`[MODEL_Qwen3]` and `[MODEL_Qwen3-Coder-Next]` — the latter wins
(longer match). The server logs:

```
[easyai-server] model profile: [MODEL_Qwen3-Coder-Next] matched for 'Qwen3-Coder-Next-Q6_K_M'
```

Worked example — aggressive context and conservative sampling for
large Qwen3 models, plus a general fallback for all Qwen3 variants:

```ini
[ENGINE]
context         = 262144
temperature     = 0.2
ngl             = 99

[MODEL_Qwen3]
temperature     = 0.3
top_k           = 40

[MODEL_Qwen3-Coder-Next]
context         = 131072
temperature     = 0.15
rope_scaling    = yarn
yarn_orig_ctx   = 131072
rope_freq_scale = 2
```

Loading `Qwen3-Coder-Next-Q6_K_M.gguf` applies the
`[MODEL_Qwen3-Coder-Next]` profile (longer match). Loading
`Qwen3-8B-Q8_0.gguf` applies `[MODEL_Qwen3]`. Loading a non-Qwen
model applies nothing — pure `[ENGINE]` values.

### `[MCP_USER]`

Bearer-token authentication for `POST /mcp`. Each line registers one
user:

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
- The first matching token wins. Duplicate tokens (operator config
  bug) silently take the alphabetically-first user.
- Generate strong tokens with `openssl rand -hex 32` or
  `python3 -c 'import secrets; print(secrets.token_hex(32))'`.
- Restart the server to pick up changes:
  `sudo systemctl restart easyai-server`.

Full per-client connection guides (Cursor / Continue / Claude Desktop
/ curl) live in `MCP.md` §4–7.

### `[TOOLS]` (RESERVED for a future release)

Per-tool ACL controlled by glob patterns. **Not yet consumed by
code** — the parser accepts the section, the binary ignores it.
Documented up-front so operators can plan / pre-populate.

```ini
[TOOLS]
mcp_allowed = rag, datetime, web
mcp_denied  = bash, fs
```

Future semantics (subject to refinement):

- `mcp_allowed` — comma-separated globs of tool names that may be
  exposed via `/mcp` and dispatched. Empty / missing = all tools
  allowed.
- `mcp_denied` — same shape; takes precedence over `mcp_allowed`.

### Worked examples

#### Minimal local-dev INI

```ini
[SERVER]
model = /home/me/models/qwen2.5-3b.gguf
host  = 127.0.0.1
port  = 8080

[ENGINE]
context = 32768
ngl     = -1
preset = precise
```

Runs on localhost only. MCP open (no `[MCP_USER]`). Auto-fit GPU.

#### Production server with auth

```ini
[SERVER]
model           = /var/lib/easyai/models/ai.gguf
host            = 0.0.0.0
port            = 80
alias           = EasyAi
sandbox         = /var/lib/easyai/workspace
system_file     = /etc/easyai/system.txt
external_tools  = /etc/easyai/external-tools
memory          = /var/lib/easyai/rag
webui_title     = EasyAi
metrics         = on
verbose         = off
mcp_auth        = on

[ENGINE]
context         = 262144
ngl             = 99
threads         = 16
threads_batch   = 16
preset = precise
flash_attn      = on
cache_type_k    = q8_0
cache_type_v    = q8_0
mlock           = on
no_mmap         = on
rope_scaling    = yarn
yarn_orig_ctx   = 131072
rope_freq_scale = 2

[MODEL_Qwen3-Coder]
temperature     = 0.15
context         = 131072

[MCP_USER]
gustavo  = REPLACE-WITH-OPENSSL-RAND-HEX-32
ci       = ANOTHER-STRONG-TOKEN
```

Systemd unit's `ExecStart` is just:

```
ExecStart=/usr/bin/easyai-server --config /etc/easyai/easyai.ini
```

(The installer additionally appends `-m <model>` and `--api-key
'${EASYAI_API_KEY}'` for runtime substitution from
`/etc/easyai/api_key`.)

#### Override one value via CLI

Operator wants to test a different model without editing the INI:

```
ExecStart=/usr/bin/easyai-server --config /etc/easyai/easyai.ini -m /tmp/test-model.gguf
```

CLI `-m` overrides INI `model`. Everything else still comes from
the INI.

#### Disable MCP auth temporarily

```
ExecStart=/usr/bin/easyai-server --config /etc/easyai/easyai.ini --no-mcp-auth
```

CLI flag wins over `[SERVER] mcp_auth = on` and over `[MCP_USER]`.

---

## 2. Command-line flags

The CLI accepts every key from the INI plus a handful of
binary-only flags (`--config`, `--no-mcp-auth`). Pass `--help` for
the full list at the version you're running.

```
easyai-server -m model.gguf [options]
```

Required:

```
  -m, --model <path>           GGUF model file
```

The full list mirrors the `[SERVER]` and `[ENGINE]` tables in §1 —
each row's "CLI equivalent" column is the CLI alias, and the "Default"
column is the value when neither CLI nor INI sets it.

Binary-only flags (no INI equivalent):

| Flag | Purpose |
| --- | --- |
| `--config <path>` | Override the default INI path (`/etc/easyai/easyai.ini`). |
| `--no-mcp-auth` | Force `/mcp` open even if `[MCP_USER]` populated. Emergency override. |
| `--show-system-prompt` | Resolve and print the persona (built-in `Deep` default OR `--system` OR `--system-file`, in the same precedence the running server uses) and exit before any port is bound or any model loads. Useful for operators inspecting the persona without bouncing the service. |
| `-h`, `--help` | Print usage and exit. |

---

## 3. API endpoints

The server speaks **three** API dialects so most AI clients work
unchanged.

| Verb | Path | Dialect | Auth | Notes |
| --- | --- | --- | --- | --- |
| GET | `/` | webui | (open) | Embedded SvelteKit chat UI. |
| GET | `/bundle.{js,css}` | webui | (open) | Bundle assets. |
| GET | `/loading.html` | webui | (open) | Loading splash. |
| GET | `/favicon` (+ `.ico`/`.svg`) | webui | (open) | Operator-supplied (`--webui-icon`) or the embedded AI Box logo SVG (compiled into the binary as `kBrandSvg`; canonical copy at `webui/AI-brain.svg`). |
| GET | `/health` | easyai | (open) | `{model, backend, tools, preset, compat:{...}}` — liveness probe. |
| GET | `/metrics` | easyai | api_key | Prometheus exposition (only when `--metrics` is on). |
| GET | `/v1/models` | OpenAI | api_key | OpenAI-shape list-models. |
| GET | `/v1/tools` | easyai | api_key | Tool catalogue. Each entry: `{name, description, short_description}`. `description` is the full multi-line manual; `short_description` is the one-line trigger shipped in the per-turn `<tools>` block. `easyai-cli` reads `short_description` to render its prompt prefix; the webui popover reads `description`. Pre-Shape-C consumers see only the `description` field — backward compatible. |
| POST | `/v1/chat/completions` | OpenAI | api_key | The workhorse — streaming SSE, tools, sampling controls. |
| POST | `/v1/preset` | easyai | api_key | Swap the ambient preset. |
| GET | `/api/tags` | Ollama | api_key | Ollama-shape list-models (LobeChat, OpenWebUI in Ollama mode, etc.). |
| GET/POST | `/api/show` | Ollama | api_key | Ollama-shape model detail. |
| POST | `/mcp` | MCP | `[MCP_USER]` | JSON-RPC 2.0 — full tool catalogue exposed to other AI apps. See `MCP.md`. |
| GET | `/mcp` | MCP | (open) | Reserved for future SSE notification stream; currently `405 Method Not Allowed`. |

### `POST /v1/chat/completions` — the killer feature

When a *client* (Claude Code, an OpenAI SDK, LiteLLM, LangChain…)
posts its **own** `system` message and/or **own** `tools`, those win
for that single request:

* Client provides `tools` → easyai forwards generated tool calls back
  to the client and *does not* dispatch them locally. The client
  controls the loop.
* Client provides no `tools` → easyai uses its own toolbelt and runs
  the multi-hop loop server-side, returning the final assistant
  message.

Either way the server-supplied `system.txt` is used **only** when the
request doesn't already include a `system` message.

**System-prompt ownership (since 2026-06-12):** a client-supplied
`system` message REPLACES the server default and is used **verbatim**
— the server splices nothing into it: no AUTHORITATIVE preamble, no
first-turn tool catalogue, no addenda. Agentic clients (easyai-cli,
opencode, Claude Code) compose their own complete prompt client-side,
and double-injection corrupted it. When the request has **no** system
message, the server default (`system.txt` / `--system`) plus the
AUTHORITATIVE preamble (§"Hardening" below) is used, exactly as
before. Thin clients that send their own persona but still want the
server's datetime / cutoff / memory-vocabulary blocks appended can opt
back in per-request with `X-Easyai-Inject: on`.

### `X-Easyai-Inject` — preamble control per request

Useful for A/B regression suites and for thin clients (see above):

```bash
curl -H "X-Easyai-Inject: off" ...
```

Header values: `off` (skip the preamble for this request only), `on`
(force injection on this request — also the opt-in that appends the
preamble to a client-supplied `system` message), or absent (defer to
the server flag; with a client-supplied `system` message absent means
verbatim, no preamble).

### `stream_options.easyai_prompt_progress: false` to skip per-batch progress events

easyai extension on top of OpenAI's `stream_options` envelope. When
the request body contains:

```json
{
  "model": "EasyAi",
  "messages": [...],
  "stream": true,
  "stream_options": { "easyai_prompt_progress": false }
}
```

…the server skips wiring the per-batch `easyai.prompt_progress` SSE
events for that request. The final `easyai.prompt_eval` summary
still fires. Trades the client's live "thinking N%" spinner for
less SSE wire chatter and lower per-batch llama_decode callback
overhead — useful for batched / scripted callers that don't render
a live gauge anyway.

Unknown keys inside `stream_options` are ignored, so older clients
that don't set this flag continue to receive per-batch events.

The `easyai-cli` client exposes this via `--no-prompt-progress` /
`[cli] prompt_progress = off`.

### `POST /mcp` — Model Context Protocol

JSON-RPC 2.0 over a single endpoint. Methods: `initialize`,
`tools/list`, `tools/call`, `ping`, plus the standard MCP
notifications. Full surface in `MCP.md`.

```sh
curl -fsS http://localhost/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | jq '.result.tools[] | .name'
```

Auth model: see §5 below. For a higher-throughput tool API without
the model in the loop, use [`easyai-mcp-server`](easyai-mcp-server.md).

---

## 4. Tool gating + sandbox

Default is **safe**: no filesystem access, no shell. Every privileged
opt-in is logged at startup with sanity warnings.

| Flag / INI key | What it enables |
| --- | --- |
| (no flag) | `datetime` and the unified `web` tool (action=`search` / `fetch`). |
| `--sandbox <dir>` (`[SERVER] sandbox`) | Sets the working root: cwd for the server process, root for the unified `fs` tool / `bash` when those are enabled, base for `$SANDBOX` placeholders in external-tool manifests, and target of `fs(action="sandbox")`. **`--sandbox` alone NO LONGER auto-registers `fs`** (operators legitimately set it for cwd / external-tools / sandbox-path while keeping the file tool off). Pass `--allow-fs` explicitly to register `fs`. `--allow-bash` still implies `fs`. |
| `--allow-fs` (`[SERVER] allow_fs`) | The unified `fs` tool (action=`read` / `write` / `list` / `glob` / `grep` / `check_path` / `cwd` / `sandbox`). The working root is `--sandbox <dir>` if given, else `.` (the server's cwd). Implied by `--allow-bash`. **Honored independently of `sandbox`** since the 2026-05-08 fix — `allow_fs = off` in the INI now genuinely disables `fs`, even with a sandbox set. |
| `--allow-bash` (`[SERVER] allow_bash`) | `bash` (run `/bin/sh -c`). cwd = sandbox if given, else the binary's CWD. NOT a hardened sandbox — runs with this process's user privileges. Also bumps the agentic-loop `max_tool_hops` to 99999 (bash flows naturally span many turns). On the server, does NOT auto-register `fs` — pair with `--allow-fs` if you want both. |
| `--no-python` / `[SERVER] allow_python = off` | Drop the compute tool (model-facing name `evaluate`, runtime `python3`). Defaults ON and auto-registers whenever `--sandbox` is set or `--allow-bash` is on. Isolated stdlib-only interpreter (no PYTHON* env, no site-packages, no cwd on `sys.path`); third-party imports fail with ModuleNotFoundError. **READ-ONLY disk + tool-name rename** (2026-05-26): the model now sees `evaluate` instead of `python3`, framed as "compute / algorithm prototyping only; FORBIDDEN: filesystem, subprocess, network, ctypes". The runtime sandbox rejects every write-mode open(); `PermissionError` points the model at the filesystem write tool. Defense-in-depth, NOT a hardened sandbox: `import os` / `import socket` / `import subprocess` / `import ctypes` all still work at the Python layer. Bumps `max_tool_hops` to 99999, same as `--allow-bash`. |
| `--use-google` (`[SERVER] use_google`) | Enables `engine="google"` inside the unified `web` tool (Google Custom Search JSON API), and lets the default `engine="auto"` cascade try google as its first hop. Requires `GOOGLE_API_KEY` and `GOOGLE_CSE_ID` env vars; the auto cascade silently skips google if either is missing (a key rotation that briefly drops the env doesn't take down the server). Counts against your Google quota (free tier: 100 queries/day per key). Without `--use-google`, the auto cascade starts at brave (keyless HTML, best query understanding for niche entities) and falls through to ddg-lite (keyless no-JS DDG with Netscape UA, page 1 only) → bing (keyless RSS) → ddg (keyless HTML scrape, often blocked from server IPs). |
| `--external-tools <dir>` (`[SERVER] external_tools`) | Load every `EASYAI-<name>.tools` file in `<dir>` as an operator-defined tool pack. Per-file fault isolation. Spawns via `fork`+`execve` — never a shell. **The supported way to give the model focused powers without flipping `--allow-bash`.** See [`EXTERNAL_TOOLS.md`](EXTERNAL_TOOLS.md). |
| `--memory <dir>` (`[SERVER] memory`) | Enable the agent's persistent **knowledge** (search / store / append / recall / update / forget) — a passive RAG technique over keyword-indexed Markdown files. Registers seven split tools — `knowledge_save`, `knowledge_append`, `search_knowledge`, `knowledge_load`, `knowledge_list`, `knowledge_delete`, `keywords_knowledge` — each entry one Markdown file in `<dir>`, operator-readable and hand-editable. Keywords are the identifier: sorted and joined by `_` they become the filename. Entries whose keywords resolve to a `fix-` prefix are immutable: save/append/delete refuse them. Pass `fix=true` (`knowledge_save`) to mint one. **Also triggers per-request injection** of a compact `# MEMORY VOCABULARY` block into the AUTHORITATIVE preamble (every distinct keyword + count, sorted by count desc / name asc, top 40) so the model can dispatch the right `search_knowledge` without first calling `keywords_knowledge`. Empty store → block omitted, no wasted tokens. The legacy flag `--RAG` (INI key `rag`) is still accepted as an alias. The systemd-installed server passes this by default (`/var/lib/easyai/rag`). See [`RAG.md`](RAG.md). |
| `--mcp <url>` (`[SERVER] mcp`) | Connect to a remote MCP server as a CLIENT. The upstream's tool catalogue is merged into ours via `tools/list` at startup; each remote tool's handler proxies `tools/call` over HTTP. Local-tool names take precedence on collision (warning logged, remote dup skipped). Pair with `--mcp-token` for bearer-auth servers. Transient failures (connect refused, read timeout, 5xx) retry per `--http-retries`; each retry is logged. Connect failure after the retry budget logs a warning and continues with whatever local / knowledge tools were registered. |
| `--mcp-token <token>` (`[SERVER] mcp_token`) | Bearer token attached to every `--mcp` request. Empty = no auth header. |
| `--http-retries N` (`[SERVER] http_retries`) | Default `5`. Extra attempts on transient HTTP failures, applied to the `--mcp` upstream calls AND to the unified `web` tool's libcurl calls. 4xx never retries; 5xx + connect/read/write errors retry with exponential backoff (250 ms → 500 ms → 1 s → 2 s → 4 s, capped). Set 0 to disable. Every retry logs to stderr (visible in journalctl without `--verbose`). |
| `--http-timeout SECONDS` (`[SERVER] http_timeout`) | Default `600`. Read/write timeout for **both** the listen socket AND the MCP-client connection. Bumped from llama-server's traditional 60 s to give long-thinking models room before the network drops them. The chosen value is echoed in the startup banner; HTTP 408 / 504 listen-side timeouts log unconditionally on stderr with the request method/path/peer. |
| `--no-local-tools` (`[SERVER] local_tools = off`) | Skip the LOCAL built-in toolbelt entirely (renamed from `--no-tools` / `load_tools`). Useful when you want ONLY external-tools, ONLY the knowledge tools, or ONLY tools fetched via `--mcp`. The MCP client remains active even with this flag set. |

Sandbox semantics: paths sent by the model are anchored to the root
by iterating path components and dropping any `..`, `.`, or absolute
markers before joining. Total containment by construction — there is
no path the model can construct that escapes. The model sees a
virtual `/`-rooted filesystem (`/report.md`, `/docs/spec.md`); the
real sandbox path is hidden from descriptions and result messages.

Concurrency: built-in tools that share state (the knowledge tools' index,
the web tool's fetch LRU cache) use lock-free or fine-grained synchronisation
internally. The knowledge tools specifically use `std::shared_mutex` so parallel
reads from multiple workers don't serialise — the same tools used by
[`easyai-mcp-server`](easyai-mcp-server.md) under thousands-of-clients
load. See [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) §16, §16.6b.

---

## 5. Authentication

Two auth surfaces, two mechanisms.

### `/v1/*` — single shared API key (`--api-key`)

`require_auth` checks every `/v1/*` request for `Authorization: Bearer
<key>`. When `--api-key` (or `[SERVER] api_key`) is set, every request
without a valid match returns 401. `/health` stays open so liveness
probes don't need a credential.

The Authorization header is capped at 4 KiB before string comparison
to defend against multi-megabyte headers being sent on every probe.

```bash
curl -H "Authorization: Bearer $TOKEN" http://localhost/v1/models
```

The systemd installer wires `${EASYAI_API_KEY}` from
`/etc/easyai/api_key` (mode 600, owned by `easyai`) so the literal
key never appears in unit files.

### `/mcp` — per-user Bearer tokens (`[MCP_USER]`)

Three-way precedence on the gate:

1. `--no-mcp-auth` on the CLI (or `[SERVER] mcp_auth = off`) →
   force open; the `[MCP_USER]` table is cleared even if populated.
2. `[MCP_USER]` populated → Bearer required, looked up in the
   token→username map at request time. Audit log per request:
   `[mcp] request from user 'gustavo'`. The token is never logged.
3. `[MCP_USER]` empty/missing → open (zero-friction local-dev
   default).

The same 4 KiB header cap applies here. Generate strong tokens with
`openssl rand -hex 32` and rotate by editing `[MCP_USER]` and
restarting the server (no token cache, no in-memory survival of old
tokens after restart).

### Compensating controls for high-trust deployments

1. **Bind to LAN only** — `[SERVER] host = 127.0.0.1`; SSH-tunnel
   from clients.
2. **Reverse proxy with mTLS / IP allowlist** — nginx / Caddy in
   front, require client cert or restrict by source net.
3. **Token rotation** — `[MCP_USER]` edits land on next restart;
   old tokens are immediately invalid.
4. **Don't enable `--allow-bash` with auth-open mode** — the worst
   `/mcp` can dispatch is the knowledge tools + read-only `web_*` + your
   `--external-tools` allowlist.

Full security model: [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) §17.

---

## 6. The default persona — Deep

A fresh `easyai-server` boots up as **Deep** — an expert system
engineer who answers from CHECKED FACTS, not impressions. Built into
the default system prompt so a small open-weights model behaves like
an engineer instead of a chatbot from minute one.

Deep's operating loop is: **TIME → THINK → PLAN → EXECUTE → VERIFY**.

- **Time first.** Any question that touches "now", "today", a
  deadline, a release version, or a fact that could have changed
  since training cutoff → `datetime` is the first tool call. Anchors
  the rest of the turn to the real wall clock.
- **Think.** State the goal, identify what's known vs. needs lookup,
  what could go wrong.
- **Plan.** Multi-step tasks call `plan(action='add', text=…)` first
  so the user can see and intervene live. The model uses
  `plan(action='update', id=…, status='working'|'done'|'error')`
  to advance steps and `action='delete'` to retire abandoned ones
  (rendered struck through, not removed). Statuses:
  `pending | working | done | error | deleted`. Batch up to 20
  items per call via the `items` array (e.g.
  `plan(action='add', items=[{text:'…'}, {text:'…'}, …])`).
- **Execute.** Every registered tool is fair game.
- **Verify.** Before claiming success — does the file exist? does
  the test pass? does the URL really say that? When in doubt, run
  another tool instead of guessing.

Old behaviour rules carry over: `RULE 1` (execute or answer, never
just announce), `web(action="search") → web(action="fetch")`
mandatory, citations stick to the URL actually fetched.

Operators who want a different persona pass `--system "<text>"`
(`[SERVER] system_inline`) or `-s persona.txt` (`[SERVER]
system_file`) — Deep is the default, not a hardcoded identity.

---

## 7. Webui customisation

The webui shipped is the compiled SvelteKit bundle from `llama-server`,
embedded into the binary at build time via `cmake/xxd.cmake` (one
`.hpp` per asset). Customisations are runtime DOM injection +
at-startup string substitution on `bundle.js`:

- **Title pin** via `Object.defineProperty(document, 'title', {set:})` so
  the bundle's hard-coded `"llama.cpp - AI Chat Interface"` doesn't
  win.
- **DOM scrubber** — a `MutationObserver` matches MCP / Sign-in /
  Authorize / Load-model / Use-Pyodide chrome by visible text and
  hides the containing card / list-item / dialog.
- **Fetch interceptor** — 501s `/authorize`, `/token`, `/register`,
  `/.well-known/*`, `/models/load`, `/cors-proxy`, `/dev/poll`,
  `/home/web_user/*`; stubs `/properties` with `{}`; and tees the SSE
  response of `/v1/chat/completions` into a status-pill state machine.
- **Tone chip + metrics bar** — `deterministic / precise / balanced /
  creative` selector + `ctx X/Y · last N tok · s · t/s` overview.
- **Per-message status pill** — appended to each assistant action
  toolbar; shows `thinking` / `answering` / `fetching · <tool>` /
  `complete · 98 tok · 4.4s · 22.3 t/s`.
- **Reasoning panel shrink** — the bundle's native Reasoning panel is
  capped at 18em tall and auto-collapses on `finish_reason`.

Operator-tunable knobs (all match an INI key — `webui_title`,
`webui_icon`, `webui_mode`, `webui_placeholder`):

| CLI | INI | Default | Notes |
| --- | --- | --- | --- |
| `--webui-title <str>` | `webui_title` | `Deep` | Sidebar / window title text. |
| `--webui-icon <path>` | `webui_icon` | (embedded AI Box logo SVG) | `.ico` / `.png` / `.svg` / `.gif` / `.jpg` / `.webp`. |
| `--webui <mode>` | `webui_mode` | `modern` | `modern` (embedded bundle) or `minimal` (single-file inline UI). |
| `--webui-placeholder <str>` | `webui_placeholder` | `Type a message…` | Input box hint. |

Browser cache caveat: after any change to served HTML/JS, hit
**Cmd+Shift+R** (Linux: Ctrl+Shift+R) to force-reload. The bundle is
hashed so a stale CSS file is the usual culprit.

---

## 8. Performance tuning

### Context size

```
-c 128000
```

128k tokens covers most chat sessions without reaching the cap. Keep
in mind the KV cache scales linearly with context.

### KV cache quantisation

```
-ctk q8_0 -ctv q8_0
```

Halves the KV cache footprint at no measurable quality loss on modern
models. `-ctv q4_0` halves it again at a small perplexity cost — try
if you're VRAM-bound. The systemd installer ships `q8_0` by default.

### Flash attention

```
-fa
```

Free perf on every backend that supports it (CUDA, Metal, Vulkan ≥
recent). Required for some KV-quant combinations.

### GPU layers (`--ngl`)

```
--ngl -1
```

Auto-fit. Manually pinning `--ngl 99` can poison `common_fit_params`
if it doesn't fit (it refuses to lower a user-pinned value), so let
auto-fit decide.

### `--mlock` and `--no-mmap`

Pinning model weights in RAM prevents the kernel paging them under
memory pressure (catastrophic for token latency). `--no-mmap` is
needed for `--mlock` to be portable across all GPU backends. The
drop-in's `LimitMEMLOCK=infinity` is what makes `mlock` actually work.

### CPU threads

```
-t <jobs> -tb <jobs>
```

For a single-user agent box, set both to `nproc`. For shared hardware,
reduce so the agent doesn't starve the rest of the system.

### Tool budget

`Engine::chat()` caps at 8 tool hops by default; bumps to 99999 when
`--allow-bash` is on (bash flows span many turns). A model that runs
away calling tools without converging will hit the cap and bail with
the last partial answer.

### Incomplete-turn retry budget

`--max-incomplete-retries N` (default 10) — how many times the engine
discards + nudges + retries when the model finishes a turn announcing
an action ("Let me…", "I'll…") without actually emitting the
tool_call. Bump to 15-20 for weak / 1-bit-quant models; set to 0 to
disable retries entirely. Each retry surfaces in the webui Thinking
panel as `↻ Retry N/max`.

### Speculative decoding (`--spec-type` / `--spec-draft-n-max`)

Off by default. Set `--spec-type` to enable; the most useful path is
**MTP** (`--spec-type draft-mtp`), which uses Multi-Token Prediction
heads baked into the main model — no separate draft model file,
zero extra VRAM for a draft. Typical speedup with `--spec-draft-n-max
6` on an MTP-trained model: 1.5-2× tok/s in the decode phase, more on
small-batch single-user latency.

```bash
# MTP (recommended when the model supports it)
easyai-server -m /models/deepseek-v3.gguf \
  --spec-type draft-mtp --spec-draft-n-max 6

# Self-speculative via n-grams (no extra model, no MTP heads needed)
easyai-server -m /models/qwen3-7b.gguf --spec-type ngram-cache
```

**Caveat — MTP requires an MTP-trained model.** DeepSeek V3 / V3.2,
MimoVL, and a handful of others ship with the MTP heads in the GGUF.
Pass `--spec-type draft-mtp` against a model without them and llama.cpp
will refuse to load (or fall back to autoregressive silently — check
the startup banner).

**Classic standalone-draft mode** is now supported: pass
`--spec-type draft-simple --draft-model /path/to/small.gguf`. The
draft model must share the same vocabulary as the target. Typical
speedups are 1.3-1.8x depending on the draft/target model pair.

INI keys: `[ENGINE] spec_type`, `[ENGINE] spec_draft_n_max`, and
`[ENGINE] spec_draft_model`. The systemd installer's `--mtp` flag
bakes `--spec-type draft-mtp --spec-draft-n-max 6` into the unit's
`ExecStart` (see `LINUX_SERVER.md`).

---

## 9. Verbose observability

In `--verbose` mode (`[SERVER] verbose = on`) the server emits two
families of diagnostic lines on stderr — they land in `journalctl -u
easyai-server` for free. Both are gated on `verbose`; outside
verbose mode there's zero logging overhead.

### 9.1 HTTP request → / ← lines

Per-request arrival + completion, wired via `set_pre_routing_handler`
+ `set_logger`:

```
[easyai-server] → POST /v1/chat/completions  from=192.168.1.42:51324  body=2417B  in_flight=1
[easyai-server] ← POST /v1/chat/completions  status=200  dur=4823ms  out=streamed  totals: req=87 err=2 tools=143 in_flight=0  bytes: in=109218B out=148329B
```

Streaming responses (chat completions) write through chunked
transfer and bypass `res.body` — they show `out=streamed` instead
of a byte count. Running totals on the `←` line are cumulative
since process start. Same atomics power Prometheus `/metrics`.

There is intentionally **no per-TCP-connection accept/close
log**. cpp-httplib's `process_and_close_socket` is `private virtual`
upstream and we don't patch llama.cpp's vendored header. The
periodic METRICS line below covers the same diagnostic territory
(TIME_WAIT pressure, fd exhaustion, request throughput) using only
public APIs and `/proc`.

### 9.2 Periodic METRICS line

A background ticker every `[SERVER] metrics_interval` seconds (CLI
`--metrics-interval N`, **default `300`** (5 min), `0` disables)
emits one line covering CPU / memory / GPU / load / HTTP / fd / TCP
states. **Always on**, regardless of `--verbose` — operators need
the telemetry whether or not they're chasing a debug session.

```
[easyai-server] METRICS uptime=600s  cpu: usage=18.3% iowait=0.4% load=1.42 1.85 2.01  mem: rss=12.45GiB peak=12.51GiB sys=78.2% (28.3GiB/36.2GiB)  gpu: gtt=18.4GiB/29.0GiB (63.4%)  http: in_flight=1 reqs=87 err=2 in=109218B out=148329B  fd: 14/4096 (0.3%)  tcp: estab=24 time_wait=8123 close_wait=2 fin_wait=0 listen=4  TIME_WAIT 8123/28232 ephemeral ports (28.8% elevated)
```

Field by field:

| Group | Source | Notes |
|---|---|---|
| `cpu: usage% iowait% load` | `/proc/stat` deltas + `getloadavg(3)` | system-wide; load is 1 / 5 / 15 min |
| `mem: rss peak sys% (used/total)` | `/proc/self/status` (VmRSS, VmHWM) + `/proc/meminfo` | rss + peak are this process; sys is the host |
| `gpu: gtt=USED/TOTAL (%)` | `/sys/class/drm/cardN/device/mem_info_gtt_*` | AMD only; shows `n/a` on NVIDIA / Intel |
| `http: in_flight reqs err in=B out=B` | atomics in `ServerCtx` | `in_flight` = currently-being-served, `reqs` / `err` = cumulative HTTP, `in`/`out` = cumulative request/response body bytes |
| `fd: N/M (%)` | `/proc/self/fd` + `getrlimit(RLIMIT_NOFILE)` | proxy for "how many more sockets can I accept" |
| `tcp: estab time_wait close_wait fin_wait listen  TIME_WAIT N/M ephemeral ports (X.X%)` | `/proc/net/tcp` + `/proc/net/tcp6` + `/proc/sys/net/ipv4/ip_local_port_range` | **system-wide** TCP state breakdown. The `TIME_WAIT N/M` segment is the choke-point indicator: numerator is the count, denominator is the kernel's actual ephemeral-port range. Tagged `elevated` (≥20%), `HIGH` (≥50%), or `CRITICAL` (≥80%) so socket exhaustion shows up before connections start failing. |

The deep metrics (cpu / mem / gpu / fd / tcp states) are Linux-only.
On macOS the line still prints; the Linux-only fields show `n/a` or
zeros and the server runs fine — `easyai-server`'s deploy target is
Linux per the install scripts. macOS is the dev path.

### What this catches in practice

The 2026-05-08 incident: a long agentic run hung mid-stream and the
cli failed with `Connection timed out` retries. The combined view
surfaces:

- `tcp: ... TIME_WAIT 27500/28232 ephemeral ports (97.4% CRITICAL)` —
  the host has run out of ephemeral ports.
- `http: in_flight=1 reqs=...` flat for minutes while the model
  thinks (no completion event) — a hung stream, not a fast loop.

The root cause was the cli's per-call `httplib::Client` construction
(fixed in commit `841dd47`); the metrics now make that visible
upstream of the symptoms. See README §What's new (2026-05-08) for
the full incident write-up.

The `metrics_interval` default of `300` seconds (5 minutes) is
low-overhead enough to leave on permanently in production. Bump
**down** (60, 30, 5) when actively troubleshooting a slow leak or
TIME_WAIT pressure; set `0` to disable the ticker entirely.

---

## 10. Hardening / security

Highlights of the work documented in [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md):

- **`std::regex` is banned on hostile input.** A 2026-04-26 production
  SIGSEGV (94 766 stack frames in libstdc++'s recursive regex engine
  on an HTML page from a `web(action="fetch")` call) drove the rule.
  All scanners over model output / HTTP bodies / file contents are
  forward-only.
- **JSON depth cap (64 levels)** on every accepted-from-network body
  (`/v1/chat/completions`, `/mcp`). Iterative walk so the validator
  itself doesn't recurse.
- **Authorization header cap (4 KiB)** on `/v1/*` and `/mcp` — defends
  against multi-megabyte Bearer headers used to amortise CPU on
  hostile probes.
- **Body cap (default 8 MiB, `--max-body`)** at the cpp-httplib layer.
- **Fork+execve hardening** for `bash` and external-tools subprocesses:
  `setpgid` for process-group lifetime, `PR_SET_PDEATHSIG(SIGKILL)`
  on Linux, fd close-loop bounded by `kMaxFdScan = 65536` (defeats
  `RLIMIT_NOFILE = unlimited`), stdin → `/dev/null`, opt-in
  `env_passthrough`.
- **Sandbox symlink-escape closed.** `Sandbox::resolve` runs
  `fs::weakly_canonical()` + path-component containment, plus
  `O_NOFOLLOW | O_CLOEXEC` on `fs(action="read")` / `fs(action="write")`
  so a TOCTOU race can't follow a last-second symlink.
- **Knowledge entries written mode 0600** so the OS-level ACL is
  owner-only even if the operator's umask leaves a wider default.
- **AUTHORITATIVE preamble** appended to whichever system message
  reaches the model (server's default OR client-supplied). Blocks in
  order: `# AUTHORITATIVE DATE/TIME`, `# KNOWLEDGE CUTOFF`, `# KNOWLEDGE
  LOOP` (when `--memory` is set), `# CITE SOURCES`, then **at the
  tail** `# MEMORY VOCABULARY` (top-40 keyword index when `--memory`
  is set). The vocab block is positioned LAST on purpose so a knowledge
  save (which mutates the keyword index) only invalidates the suffix
  of the prompt-eval KV cache — the stable rules above stay warm.
  Builder lives in libeasyai (`easyai::preamble::build`) and is shared
  with `easyai-local` and `easyai-cli`. The vocab itself is cached
  by `(directory mtime, file count)` so repeat requests pay one
  `stat(2)`, not a full directory walk.
- **Default system prompt is also library-owned**:
  `easyai::preamble::build_builtin_system_prompt(ToolsetView)` —
  shared between server and local. Renders the "Active tools this
  session" enumeration from the registry (ground truth, can't drift
  from what was wired) plus the closed-set rule, the write/edit
  policy, the information pipeline, and the cite-sources block. The
  ~180-line copies that used to live in `examples/server.cpp` and
  `examples/local.cpp` collapsed onto 15-line wrappers.
- **Auto-generated transaction logs at `/tmp/easyai-<pid>-<epoch>.log`
  are created with `O_EXCL | O_NOFOLLOW | O_CLOEXEC` and mode `0600`.**
  `O_EXCL` makes the create atomic-or-fail so a local attacker can't
  win the race by pre-planting a symlink at the predictable path; mode
  `0600` keeps prompt content (which can include API keys / PII) private
  to the running user even on multi-tenant hosts.
- **Seven audit passes** — the standing review in
  [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) runs end-to-end on every
  pass and adds a delta section for new surfaces.  Latest pass
  2026-05-11 (7th), with §22.8 follow-up the same day closing a HIGH
  data-integrity bug on `fs(action="edit")` (silent seam-line glue
  when `content` lacked a trailing `\n`).  Earlier passes closed the
  `/tmp` log symlink race (4th), `apply_ini_to_args` dead code +
  sandbox symlink escape + `bash` fork-hardening (3rd), `bash`
  live-mirror terminal-escape injection (5th), `presence_penalty`
  NaN/Inf accept + persistent-httplib setter races (6th), and the
  python3-tool banner + sandbox-preamble closure-cell tightening
  (7th).

For threat models requiring OS-level isolation: run easyai-server
inside a container / firejail / unprivileged user with disabled
network egress. The sandbox + tool gates bound what the *model* can
ask for; the OS bounds what the *agent process* can do.

---

## 11. Cross-references

- [`README.md`](README.md) — sales overview + quickstart.
- [`LINUX_SERVER.md`](LINUX_SERVER.md) — operator's guide for the
  systemd-installed server (file layout, the unit file, perf tuning,
  gotchas, backup / upgrade / uninstall).
- [`easyai-cli.md`](easyai-cli.md) — the matching client: REPL,
  one-shot, piped, management subcommands, tool registration, the
  `[environment]` + `[guidance]` prompt injection, sampling, reasoning,
  the raw transaction log (opt-in), session persistence
  (`.easyai_session` auto-save, `--continue`, `--compress`,
  `/compress`).
- [`easyai-mcp-server.md`](easyai-mcp-server.md) — standalone
  MCP-only server: same tools, no model, dedicated for high-concurrency
  multi-client deployments.
- [`MCP.md`](MCP.md) — Model Context Protocol surface;
  per-client connection cookbook (Claude Desktop / Cursor / Continue /
  curl).
- [`RAG.md`](RAG.md) — persistent registry, the split
  `knowledge_*` tools, workflows.
- [`EXTERNAL_TOOLS.md`](EXTERNAL_TOOLS.md) — operator-defined
  external tools (`EASYAI-*.tools` JSON manifests).
- [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) — seven audit passes,
  HIGH / MEDIUM / LOW findings, accepted residual risk.
- [`design.md`](design.md) — architecture + why decisions.
- [`manual.md`](manual.md) — hands-on developer manual.
