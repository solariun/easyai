# Your private AI, running in your house.

**One command turns a Debian/Ubuntu box into a self-hosted ChatGPT
replacement — local model, local data, no cloud round-trip.**

```sh
curl -fsSL https://raw.githubusercontent.com/solariun/easy/main/scripts/install_easyai_server.sh \
    | bash -s -- --model /path/to/model.gguf --enable-now
```

That's it. Backend (Vulkan / CUDA / ROCm / CPU) is auto-detected.
A hardened systemd service starts on the next reboot. A browser-based
chat UI is live on port 80. Your house has its own AI.

---

## Why a box, not a cloud subscription?

| Concern | Cloud LLM | Your AI Box |
| --- | --- | --- |
| Where your prompts go | Provider's servers, retained per ToS | Stays on the box. Period. |
| Where your files / memory live | Provider's storage | Local disk. Encrypted FS if you want. |
| Monthly cost | $20–$200/seat indefinitely | Hardware once. Power $1–$5/mo. |
| Latency | Network round-trip (50–400 ms) | LAN-local (1–5 ms) |
| Available when WAN is down | No | Yes |
| Custom models / fine-tunes | Provider menu only | Any GGUF you can find or train |
| Audit trail | Vendor dashboard | Your `journalctl` |

The AI Box is the box that runs **your** model on **your** silicon for
**your** household. Nothing about a conversation, a document, or an
inferred action ever needs to leave the LAN.

---

## What you get on first boot

The installer wires up a complete agent stack, not just a model
loader:

| Layer | What's running | Surface |
| --- | --- | --- |
| **HTTP server** | `easyai-server` — drop-in `llama-server` replacement | OpenAI-compatible `/v1/chat/completions` (full SSE streaming) |
| **Web UI** | llama.cpp's SvelteKit chat, embedded in the binary | `http://ai-box/` — phone, tablet, laptop, any browser |
| **MCP server** | Model Context Protocol provider | `POST /mcp` — Claude Desktop, Cursor, Continue, Zed |
| **Tool catalogue** | Auto-registered built-ins | `search_web` · `fetch_web` · `fs` · `bash` · `python3` · `memory` · `plan` · datetime |
| **Persistent memory** | `memory` tool over `/var/lib/easyai/rag/` | A passive RAG technique — per-topic markdown the model can save, append, and search across conversations |
| **Sandbox** | `/var/lib/easyai/workspace/` | Everything `fs` / `bash` / `python3` touches is pinned inside |
| **Telemetry** | `/metrics` (Prometheus) + journald METRICS line | CPU / GPU / mem / TCP / TIME_WAIT pressure |
| **Security** | Bearer auth, MCP user tokens, audited tool surface | See `SECURITY_AUDIT.md` for the full standing review |

Hardening that ships **by default** on the systemd unit:
`OOMScoreAdjust=-700`, `CPUSchedulingPolicy=fifo`, `LimitMEMLOCK=infinity`,
`mlock` on, q8_0 KV cache, flash-attn enabled, coredump capture on, `easyai`
system user (not root), workspace mode 750, `[MCP_USER]` Bearer tokens, INI
shape validation on every install parameter, sandbox enforcement
double-checked after every parent-dir creation. Seven audit passes
(2026-04 → 2026-05) have closed every HIGH/MEDIUM found.

---

## What can you actually do with it?

### 1. A private ChatGPT for the family

Open `http://ai-box/` in any browser. Conversations are streamed
in real time. The model can search the web (`search_web`), pull
articles (`fetch_web`), run calculations (`python3`), keep
running notes per topic (the `memory` tool), and execute shell
commands in the sandbox (`bash`, opt-in). No history is ever uploaded.

### 2. A coding co-pilot in your terminal

`easyai-cli "explain why this test fails"` from any directory.
With `--sandbox $(pwd)` and `--allow-fs`, the model can read,
edit (`fs(action="edit")`), and rewrite files in the current
project. Same flow as Claude Code / Cursor — but the bytes never
leave your machine.

### 3. Wire it into your IDE via MCP

Point Claude Desktop, Cursor, Continue, or Zed at
`http://ai-box/mcp` with a Bearer token from `[MCP_USER]`. Your
IDE's coding agent now uses your model and your tools (the `memory`
tool, local `bash`, your sandbox, any custom tools you registered via
`--external-tools`) instead of the vendor's defaults.

### 4. Home automation with natural language

Connect Home Assistant (or any automation hub that speaks HTTP) to
`/v1/chat/completions`. Register your home-control commands as
external tools (JSON manifest — no code needed). Then you can write
automations that say things like *"if the model thinks someone's
phone hasn't moved for 6 hours and they're not asleep, send a
welfare check"*. The agent does the reasoning; your manifest does
the action.

### 5. A radio / hobbyist knowledge base

`memory(action="save", title="callsign-XX0XYZ", ...)` — the model now
remembers every QSO log, contest entry, propagation note, or contact
card you hand it. Search across years of memory without burying it in
a folder hierarchy. Build a custom external tool to query your
linbpq / KISS BBS / SimpleBLE service and the agent can answer
"who logged into the BBS in the last hour" without you writing a
query layer.

### 6. Document Q&A over personal files

`fs(action="glob")` + `fs(action="read")` lets the model
summarise, compare, or extract from anything in your workspace —
tax PDFs, lab notebooks, codebase, journal entries. The
classifier and the retrieved bytes never leave the box.

### 7. Long-running agent tasks

`python3` runs stdlib snippets — JSON wrangling, HTTP probes,
arithmetic, date math, hashing, regex. `bash` runs anything
shell can do, capped at 32 KB / 300 s. The agent loops up to
99 999 hops with `--allow-bash`, so multi-step builds, test
runs, and orchestration scripts are in reach.

### 8. Offline / WAN-down resilience

Your AI Box keeps working when the internet is down. The local
`memory` tool, the model, the chat UI, the MCP tool surface — none of
them have a cloud dependency. (Web tools obviously don't work
without WAN; everything else does.)

---

## Recommended hardware

Anything that runs Debian/Ubuntu with a GPU that has ≥8 GB of
VRAM is fine. Reference setups that we test against:

| Class | Box | GPU / iGPU | Models that fit comfortably |
| --- | --- | --- | --- |
| Mini-PC (~$500) | MINISFORUM UM690L Slim | Radeon 680M iGPU (28 GiB GTT) | Qwen 3.6 35B-A3 Q4 MoE (~22 GB) |
| Workstation (~$1500) | Any Ryzen + RX 6800 / 6900 | 16 GB dGPU | Qwen 2.5 32B Q4 |
| Apple Silicon | Mac mini M2 Pro 32 GB | Metal | Qwen 2.5 14B Q5 |
| CPU-only | Old Xeon / Ryzen, ≥32 GB RAM | none | Qwen 3B / Gemma 4B |

The whole stack is built around the assumption that your home box
is doing serious work, not just running a demo. KV-cache
quantization, flash-attn, mlock, NUMA-aware threading — all on by
default on the installed unit.

---

## How to get started

1. **Pick a box.** Anything with apt + systemd + a recent GPU
   driver (Vulkan / CUDA / ROCm) or just a fast CPU.
2. **Get a GGUF model.** Hugging Face, llama.cpp's converter, your
   own fine-tune. Qwen 2.5 3B Q4 is a fine starter (~2 GB).
3. **Run the installer.**

   ```sh
   ./install_easyai_server.sh --model /path/to/model.gguf --enable-now
   ```

   The installer prints exactly what it's about to do and exits
   non-zero on any failure. `--upgrade` for in-place updates;
   `--force` to push new defaults to a box you already installed.

4. **Open the browser.** `http://<your-box>/` — chat UI is live.
   `journalctl -u easyai-server -f` for the box's running log.

5. **Plug in your tools.** Copy `examples/EASYAI-example.tools` to
   `/etc/easyai/external_tools/`, edit, restart — the model now
   knows about your home commands.

---

## Where to go next

- `README.md` — full project overview, library API, library examples
- `easyai-server.md` — every CLI flag, every INI key, every endpoint
- `easyai-cli.md` — using the agentic terminal client
- `easyai-mcp-server.md` — running the MCP-only daemon for IDE wiring
- `MCP.md` — protocol details, client configuration recipes
- `RAG.md` — persistent memory layout, tool surface, encryption note
- `AI_TOOLS.md` — full built-in tool catalogue with examples
- `EXTERNAL_TOOLS.md` — registering your own commands as model tools
- `SECURITY_AUDIT.md` — the full standing security review
- `LINUX_SERVER.md` — production deployment patterns

---

*easyai is open source (MIT). Built on top of `llama.cpp` and
`cpp-httplib`. Issues / pull requests welcome at
github.com/solariun/easy.*
