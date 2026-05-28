# easyai

> **A C++17 framework anyone can use to build AI agents that talk to
> their own services — no llama.cpp, JSON-Schema, or template-engine
> knowledge required.**

easyai turns [llama.cpp](https://github.com/ggml-org/llama.cpp) into an
*agent engine* you can drop into any program in a dozen lines.  You give
it C++ functions; it gives the model the ability to call them.  That's
the whole pitch.

It ships **one unified library** (`libeasyai`) you can
`find_package(easyai)` and link against, plus seven ready-to-run
binaries. The library is the product; the binaries are demos that
prove what it can do. See [`LIB_GUIDE.md`](LIB_GUIDE.md) for the
OpenAI-Python-SDK-shaped `easyai::Session` quickstart and the tour of
the lib surface.

| Library             | Purpose                                                                                                                                       |
|---------------------|-----------------------------------------------------------------------------------------------------------------------------------------------|
| `libeasyai`         | Everything in one shared object — `easyai::Engine` (local llama.cpp), `easyai::Client` (OpenAI-protocol HTTP), `easyai::Session` (one-call agent), `easyai::Tool` + built-ins (datetime/web/fs/bash/python/memory/tool_lookup), external-tool loader, RAG store, MCP server/client, the `preamble` composer. Linked via `easyai` (alias `easyai::easyai`). Legacy aliases `easyai::engine` and `easyai::cli` still resolve to the same target. |

| Binary               | What it gives you                                                                                                                                  |
|----------------------|----------------------------------------------------------------------------------------------------------------------------------------------------|
| `easyai-local`       | Local-only REPL: loads a GGUF in-process via `easyai::Engine` (driven through `easyai::Session`). Drop-in `llama-cli` replacement — one-shot scripting (`-p`), tools, presets, optional `<think>` strip, sandboxed `fs_*` tools, opt-in `bash` tool. |
| `easyai-cli`         | Agentic OpenAI-protocol client — no local model.  REPL, `--shell` (hybrid AI shell), or `-p` one-shot.  Full sampling control (`--temperature`, `--top-p`, `--top-k`, `--min-p`, `--repeat-penalty`, `--frequency-penalty`, `--presence-penalty`, `--seed`, `--max-tokens`, `--stop`), plan tool, server-management subcommands (`--list-models`, `--list-tools`, `--health`, `--props`, `--metrics`, `--set-preset`).  HTTPS via OpenSSL; `--insecure-tls` / `--ca-cert` for dev/internal CAs.  Full doc: [`easyai-cli.md`](easyai-cli.md). |
| `easyai-server`      | Drop-in `llama-server` replacement: OpenAI-compat HTTP **with full SSE streaming**, embedded SvelteKit webui, Bearer auth, Prometheus `/metrics`, KV-cache controls, flash-attn, mlock.  Speaks MCP, OpenAI, Ollama from one process.  Full doc: [`easyai-server.md`](easyai-server.md). |
| `easyai-mcp-server`  | **Standalone Model Context Protocol provider — no model loaded.** Same tool catalogue as `easyai-server` (built-ins + the `memory` tool + external-tools), exposed over `POST /mcp` with a configurable cpp-httplib worker pool (`--threads`) and an in-flight `tools/call` cap (`--max-concurrent-calls`) for thousands-of-clients deployments.  Full doc: [`easyai-mcp-server.md`](easyai-mcp-server.md). |
| `easyai-library-demo`| Five-line `easyai::Session` template — pair with [`LIB_GUIDE.md`](LIB_GUIDE.md).  The smallest "build an agent, register a tool, chat" program in the repo. |
| `easyai-agent`       | A demo agent showing every built-in tool plus an inline custom tool.                                                                                |
| `easyai-recipes`     | Tutorial agent paired with `manual.md` — implements `today_is` and `weather` (HTTP-calling) from scratch.                                          |
| `easyai-chat`        | A bare-bones REPL with no tools — useful as a sanity check.                                                                                          |

> **Status** — used in production on a Linux Vulkan box (Radeon 680M)
> as a self-hosted ChatGPT-style assistant.  Apple Silicon (Metal),
> Linux/Windows Vulkan, NVIDIA CUDA, and AMD ROCm are all wired up out
> of the box.  `scripts/install_easyai_server.sh` handles the whole
> Debian/Ubuntu deployment in one command (systemd-coredump,
> hardened unit, optional `--enable-verbose`, drop-in compat with
> `install_llama_server.sh`).

---

## What's new

A running log of user-facing changes. Latest first — keep this list
current as features land so anyone returning to the repo (or
landing on it for the first time) sees what shipped recently.

### 2026-05-27 — Unified library + `easyai::Session` (OpenAI-Python-SDK shape)

Single library now — `libeasyai` carries everything (Engine, Client,
every tool, the system-prompt composer, Session). The previous split
into `libeasyai` + `libeasyai-cli` is gone. Demos all link a single
target (`easyai`). Legacy aliases (`easyai::engine`, `easyai::cli`)
still resolve to it so existing CMakeLists keep working.

The new `easyai::Session` (in `easyai/session.hpp`) is the
recommended entry point — five fluent lines for "build an agent,
register a tool, chat", mirroring the OpenAI Python SDK call site:

```cpp
auto session = easyai::Session::remote("http://localhost:8080");
session.with_default_tools()
       .system_append("Speak in plain English.")
       .on_token([](const std::string & p){ std::fputs(p.c_str(), stdout); });
std::string err;
session.init(err);
session.chat("hello");
```

Pair with `easyai::Tool::builder(...).system_addendum("...")` to let a
custom tool ship its own system-prompt guardrails — Session
auto-concatenates them. Full reference in
[`LIB_GUIDE.md`](LIB_GUIDE.md); minimum demo in `examples/library_demo.cpp`
(binary `easyai-library-demo`). `examples/local.cpp` has been
migrated to Session as a reference for in-process agents; `cli.cpp`
and `server.cpp` follow in a later pass and continue to work via the
existing Engine/Client paths.

### 2026-05-26 — `python3` model-facing rename to `evaluate` (back-compat alias)

Final fix to a stubborn failure mode: models with a strong "Python
writes files / runs subprocesses / fetches URLs" training prior were
reaching for the `python3` tool to do exactly those system-side
things even when the system prompt, the tool description, and the
sandbox PermissionError all said *don't*. The lighter fixes (Shape-C
short triggers, write/edit policy block, runtime sandbox enforcement)
took the failure rate down but didn't kill it.

**The rename:** the **model-facing** tool name changed from
`python3` → `evaluate`. The runtime is still Python 3 (operators
still see `--no-python`, `[SERVER] allow_python`, the Python sandbox
preamble, etc.). The split is deliberate:

| Surface | Name |
|---|---|
| Tool name in `<tools>` / `tools/list` / what the model dispatches | `evaluate` |
| Tool short description | `"Evaluate Python 3 code for compute / algorithm prototyping. FORBIDDEN: filesystem, subprocess, network, ctypes. Stdlib compute only."` |
| Operator CLI flag | `--no-python` (unchanged) |
| Operator INI key | `[SERVER] allow_python` (unchanged) |

`canonical_tool_name("python3")` returns `"evaluate"` so resumed chat
sessions, external-tools manifest reservation lists, and any caller
that dispatches `python3` by name still work — the dispatcher routes
the legacy name to the new tool, no second schema shipped.

Reframing: model now sees an "evaluate" affordance with an explicit
**FORBIDDEN: filesystem, subprocess, network, ctypes** list, not a
"python3" affordance with a "don't write files" caveat. The first
non-generic word the model parses on the bullet line is "evaluate" —
no "python = open(f, 'w')" training prior to override.

### 2026-05-26 — Shape-C tools wire shape, `evaluate` read-only, `fs.ops` 50/20

Five linked changes refactor how tools reach the model:

* **Shape-C wire shape.** Per-turn `<tools>` blocks now ship
  `name + short_description + schema` (~2 000 tokens saved per
  session on a typical catalogue). Full multi-line manual stays
  in libeasyai and is returned by `tool_lookup(name="<x>")` on
  demand. `Tool::short_description` + `wire_description()` are
  new; tools without an explicit short trigger fall back to the
  first 120 chars of `description`.

* **`tool_lookup` gains a MANUAL view.** No-arg call returns the
  INDEX (numbered `name: short trigger` list); `name="<substr>"`
  returns the FULL description for every match. The model uses
  the index to scan and drills in only when it needs the manual.

* **`evaluate` (formerly `python3`) is now read-only on disk.**
  `kPythonSandboxPreamble` rejects any write-mode `open()` (mode
  `'w'/'a'/'x'/'+'` or `os.open` with
  `O_WRONLY|O_RDWR|O_CREAT|O_TRUNC|O_APPEND`) regardless of path.
  Read-only opens inside the sandbox still work. `PermissionError`
  points the model at the filesystem write tool registered this
  session. Defense-in-depth — adversarial bypasses (`ctypes`,
  `subprocess`, `_io.FileIO`, closure-cell introspection) are
  documented residuals.

* **`fs(action="ops")` batch caps raised to 50 ops / 20 files.**
  One call can land up to 50 file operations across up to 20
  distinct files. Same-path edits auto-reorder bottom-up so every
  `start_line` refers to the file's ORIGINAL line numbers — no
  manual offset math. Report header names the touched files;
  successful `read` ops in a batch clip at 2 KiB; failed ops show
  the full diagnostic so the model can self-correct without
  re-running.

* **`fs(action="ops")` batch lives on the unified `fs` surface.**
  Default `ToolMode` stays `Split` (one focused tool per action —
  small models drive it more reliably). To pick up the batch, run
  with `--tools-mode unified` (or `--tools-mode both`).

Plus: MCP server adds an `initialize.instructions` field carrying
the closed-set rule
plus the same write/edit policy; memory vocabulary block moved to
the preamble tail and cached by `(mtime, file count)` so prompt-
eval KV stays warm across memory writes.

Security audit: see [SECURITY_AUDIT.md §23 (eighth pass)](SECURITY_AUDIT.md#23-eighth-pass--2026-05-26-shape-c-tools-refactor).
One MEDIUM finding (`tools_block` rendered untrusted fields verbatim
— fixed by `sanitize_for_prompt`), two LOW residuals documented.

### 2026-05-17 — MTP speculative decoding (`--spec-type draft-mtp`) + installer `--mtp`

llama.cpp's Multi-Token Prediction merged upstream on 2026-05-16; we
bumped our vendored llama.cpp checkout to `39cf5d619` (same-day HEAD,
all 262 commits since the previous pin) and wired the MTP path
through the three layers in one go.

**Library API** ([include/easyai/engine.hpp](include/easyai/engine.hpp)):
```cpp
engine.spec_type("draft-mtp")      // or: none (default), draft-simple,
                                    //     draft-eagle3, ngram-simple,
                                    //     ngram-map-k, ngram-map-k4v,
                                    //     ngram-mod, ngram-cache
       .spec_draft_n_max(6);        // max draft tokens per step
```
Unknown strings land in `Engine::last_error()` and leave speculation
off (no silent default switch).

**Server CLI**:
```bash
easyai-server -m /path/to/mtp-model.gguf \
  --spec-type draft-mtp --spec-draft-n-max 6
```
INI keys: `[ENGINE] spec_type` and `[ENGINE] spec_draft_n_max`.

**Installer shortcut**:
```bash
./install_easyai_server.sh --mtp                # n_max=6 (default)
./install_easyai_server.sh --mtp --mtp-n-max 8  # override
```
The installer bakes the two flags into the systemd `ExecStart` so the
service inherits MTP without `systemctl edit`.

**Caveat**: MTP needs a model TRAINED with MTP heads (DeepSeek V3,
MimoVL, and similar). Plain models will refuse to load with
`--spec-type draft-mtp`. The installer's `--mtp` flag is the operator
saying "I know what I'm doing"; there's no validation.

Classic standalone-draft-model speculative decoding (the
`--draft-model PATH` path) is not yet wired — only MTP, which doesn't
need a separate model file. The old installer compat lines for
`--draft-model` / `--draft-max` / `--draft-min` still warn and skip.

### 2026-05-16 — Memory vocabulary auto-injection + shared `easyai::preamble::build()`

Every binary that loads `--memory <dir>` now auto-injects a compact
keyword-vocabulary block into the system prompt so the model knows
what it has tagged without having to call `memory(action="keywords")`
first. The block looks like:

```
# MEMORY VOCABULARY (the keywords your private memory currently
has tagged — the FIRST place to look for anything you might
already know)
12 entries (most-common first; call memory(action="search",
keywords=["<name>", ...]) to recall):
easyai(8) claude(5) bitnet(3) build(3) iteration(2) …
```

Sorted count desc / name asc, capped at top 40. Empty store →
block omitted, no wasted tokens.

| Binary | When the vocab is computed |
| --- | --- |
| `easyai-server` | Every request (fresh disk scan, ~10-50ms — rounding error vs. inference). New saves visible on the next request. |
| `easyai-local`  | Once at startup, appended to the system prompt. New saves visible after restart. |
| `easyai-cli`    | Once when building the system prefix sent to the remote server. |

The AUTHORITATIVE preamble used to live as a `build_authoritative_
preamble` inside `examples/server.cpp` with parallel partial
copies in `local.cpp` and nothing in `cli.cpp`. That drift is gone:
the builder is now public in libeasyai —

```cpp
// include/easyai/preamble.hpp
namespace easyai::preamble {
    struct Options {
        bool        inject_datetime  = true;
        std::string knowledge_cutoff = "2024-10";
        std::string memory_root;        // empty → vocab block omitted
    };
    std::string build(const Options & opt);
}
```

— and all three binaries call it. Change the renderer once, every
binary updates. Third-party hosts of libeasyai get the same
behaviour out of the box.

See `RAG.md` §5 "Automatic vocabulary injection" and `design.md`
§5c for the full design.

### 2026-05-15 — `split` is the new tools-mode default

Same-day follow-up to the morning's `--tools-mode` landing: **`split`
is now the out-of-the-box default**, not `unified`.

Reason: smaller / quantised tool-callers (Llama 3 8B, Qwen 2.5 7B,
Phi-3.5, GPT-OSS-20B) dispatch much more reliably against flat
one-verb-per-tool schemas than against a `fs(action="...")`
discriminated-union dispatcher.  Large models handle either shape
fine.  The split surface costs ~15-20% extra system-prompt tokens for
a 30-50% reduction in retry / "unknown action" hops in practice —
worth it for everyone, surprising for nobody.

| Surface | Registered out of the box | Old behaviour | New default |
| --- | --- | --- | --- |
| Multi-action families | `fs`, `web`, `memory` | 3 dispatchers | `fs_read`, `fs_write`, `fs_append`, `fs_edit`, `fs_list`, `fs_glob`, `fs_grep`, `fs_check_path`, `fs_cwd`, `fs_sandbox`, `web_search`, `web_fetch`, `memory_save`, `memory_append`, `memory_search`, `memory_load`, `memory_list`, `memory_delete`, `memory_keywords` — 19 focused tools |

```bash
# new default (no flag)
easyai-cli --url http://ai.local:8080 --sandbox ~/proj

# opt back in to the legacy dispatcher (3 tools instead of 19)
easyai-cli --tools-mode unified --url ai.local:8080 --sandbox ~/proj

# best of both worlds — costs more tokens, lets the model pick
easyai-cli --tools-mode both --url ai.local:8080 --sandbox ~/proj
```

Library callers: `Toolbelt::tool_mode_` now defaults to
`ToolMode::Split`; pass `ToolMode::Unified` explicitly if your prompt
relies on the legacy tool names.

INI: `[cli] tools_mode = unified|split|both` (default `split`).

### 2026-05-15 — `--tools-mode` lets small models work with one-verb-per-tool

`fs`, `web`, and `memory` ship as **unified dispatchers** with an
`action` parameter (e.g. `fs(action="read", ...)`).  That shape keeps
the system prompt small and lets a large model batch many actions, but
**smaller / quantised tool-callers** (Llama 3 8B, Qwen 2.5 7B, Phi-3.5,
GPT-OSS-20B) gravitate toward one-purpose tools — `fs_read`, `fs_edit`,
etc. — because the verb IS the tool name and the parameter schema is
flat.

Three modes, selected by the new flag (defaults flipped to `split` in
the same-day follow-up entry above):

```
easyai-cli --tools-mode unified     # legacy: one dispatcher per family
easyai-cli --tools-mode split       # one focused tool per action
easyai-cli --tools-mode both        # register both surfaces side-by-side
```

| Mode | Tools registered (with `--sandbox` + `--memory`) |
| --- | --- |
| `unified` | `fs`, `web`, `memory` — 3 dispatchers |
| `split` (new default) | `fs_read`, `fs_write`, `fs_append`, `fs_edit`, `fs_list`, `fs_glob`, `fs_grep`, `fs_check_path`, `fs_cwd`, `fs_sandbox`, `web_search`, `web_fetch`, `memory_save`, `memory_append`, `memory_search`, `memory_load`, `memory_list`, `memory_delete`, `memory_keywords` — 19 focused tools |
| `both` | unified + split, same handlers under both names |

Same handlers under the hood — behaviour is identical to the unified
surface; only the registration shape changes.  Library API:

```cpp
easyai::cli::Toolbelt()
    .sandbox("/srv/data")
    .tool_mode(easyai::cli::ToolMode::Split)   // or Both, or Unified
    .apply(client);
```

INI: `[cli] tools_mode = unified|split|both`.

### 2026-05-13 — `easyai-cli` session resume flips back to opt-in

Reverts the 2026-05-12 default flip: loading the existing
`.easyai_session` is **opt-in again** via `--continue`.  Without the
flag, any file in cwd is ignored and overwritten on the first turn
— matching the behaviour shipped originally on 2026-05-12 morning
before the auto-on flip.

Why: the auto-on default surprised operators who opened a project
directory expecting a fresh agent and instead picked up history
from a previous experiment.  An explicit opt-in matches the rest
of the cli's surface (nothing else implicitly carries state across
invocations) and removes the silent action-at-a-distance.

| | Previous (2026-05-12 → 2026-05-13) | Now |
| --- | --- | --- |
| Resume on launch | default ON | opt-in via `--continue` |
| Start fresh | opt-in via `--no-continue` | **default** |
| `--compress` without `--continue` | no-op (warning) | no-op (warning) |

Saving is unchanged: every turn (and every tool round-trip) still
rewrites `.easyai_session` atomically.  `--no-continue` stays as the
explicit form of the default — useful for scripts overriding an
operator's `[cli] auto_continue = on` INI line.

Default for `[cli] auto_continue` flips to `false`.  Operators who
prefer the auto-on behaviour can opt in once via INI:

```ini
[cli]
auto_continue = true
```

Full doc: [`easyai-cli.md`](easyai-cli.md) §10.

### 2026-05-13 — Installer: cap `easyai-server` restart attempts at 2

The systemd unit now carries `StartLimitBurst=2` +
`StartLimitIntervalSec=60` in `[Unit]`, so the service attempts to
start at most **twice** in any 60-second window before giving up and
leaving the unit in the `failed` state.

Before, `Restart=on-failure` + `RestartSec=10` with no burst cap
would retry indefinitely — a missing model file, a bad CLI flag, or
a GPU that wasn't exposed to the container produced an infinite
restart loop that filled journald and never surfaced the real
problem.

Now:

| State | Behaviour |
| --- | --- |
| Initial start fails | Wait `RestartSec=10`, retry once |
| Retry also fails | Unit enters `failed` state; no further attempts |
| Long-running service fails after running > 60 s | Burst counter has reset → still gets one retry (not penalised for late failures) |

Recovery: `journalctl -u easyai-server` to inspect the two failed
attempts, fix the root cause, then
`sudo systemctl reset-failed easyai-server`
+ `sudo systemctl start easyai-server`.

Existing installs: re-run `install_easyai_server.sh --force` (or
`--upgrade`) to refresh the unit file.  `Restart=on-failure` and
`RestartSec=10` are unchanged.

### 2026-05-13 — Installer: ship only `system.txt_template`; default install uses the binary's built-in prompt

`scripts/install_easyai_server.sh` no longer drops an active
`/etc/easyai/system.txt` on first install.  Out-of-the-box, only the
template `/etc/easyai/system.txt_template` ships (the canonical
"factory" copy of the Deep persona, refreshed on every `--upgrade`),
and `SERVER.system_file` is left commented out in `easyai.ini` — so
the server uses the binary's built-in prompt, which is **already
gated on actually-registered tools**: it never advertises `fs` /
`bash` if those are off in the INI.

The template file was also renamed `system.txt_modelo` →
`system.txt_template` (English-only convention).

| State | Before (≤ 2026-05-12) | Now (2026-05-13+) |
| --- | --- | --- |
| Template file at `/etc/easyai/` | `system.txt_modelo` (Portuguese) | `system.txt_template` |
| Active `/etc/easyai/system.txt` on first install | dropped (Deep persona) | **NOT installed** |
| `--force` rewrites `system.txt` | yes | no (file isn't there) |
| `SERVER.system_file` in `easyai.ini` | commented out | commented out (unchanged) |
| Out-of-the-box prompt | active `system.txt` (same Deep body) | binary's built-in, tool-gated |

To activate a custom persona — same one-liner as before:

```bash
sudo cp /etc/easyai/system.txt_template /etc/easyai/system.txt
sudoedit /etc/easyai/system.txt              # tweak as needed
sudoedit /etc/easyai/easyai.ini              # uncomment SERVER.system_file
sudo systemctl restart easyai-server
```

Existing installs are unaffected: the installer still **preserves**
any existing `/etc/easyai/system.txt` across `--upgrade` and `--force`
runs (it just no longer creates one when it doesn't exist).

Full doc: [`LINUX_SERVER.md`](LINUX_SERVER.md) §6
("`/etc/easyai/system.txt` (operator-supplied) + `system.txt_template`")
and §12 ("Upgrading").

### 2026-05-12 — Installer: `ttm.pages_limit` updated in place on re-run

`scripts/install_easyai_server.sh` used to print
`ttm.pages_limit already present; skipping` when `/etc/default/grub`
already had a `ttm.pages_limit=N` token — even if N differed from
the value the operator just passed via `--gtt`.  Result: re-running
the installer with a new GTT size was silently a no-op on the
GRUB side, and the next reboot kept the stale page count.

The patch now compares the existing token's page count against the
target, rewrites it in place when they differ (via `sed -i`), and
runs `update-grub` so the change lands in `/boot/grub/grub.cfg`.
The reboot reminder also points at `/proc/cmdline` so operators
can verify the new value boots cleanly.

No flag change.  Operators who pass the same `--gtt` value on every
run see the same idempotent "already present; skipping" message.

### 2026-05-12 — AI Box logo: softer two-layer aura

Tuned the aura halo on the AI Box mark so it reads as a quiet
emission instead of a neon outline.  The earlier tuning was
described internally as "loud"; this pass cuts both stacked
Gaussian blurs to subtler values:

| Layer | Before (07c2347) | Now (cc92d51) |
| --- | --- | --- |
| Outer halo `stdDeviation` | 14 | **10** |
| Outer halo `flood-opacity` | 0.5 | **0.3** |
| Inner halo `stdDeviation` | 4  | **3**  |
| Inner halo `flood-opacity` | 1.0 | **0.6** |

Gradient, mark geometry, viewBox headroom and filter cyan flood
(`#00bcd4`) all unchanged.  Both `webui/AI-brain.svg` (the
canonical SVG source) and the inline `constexpr kBrandSvg` in
[`examples/server.cpp`](examples/server.cpp) updated in lockstep,
so the favicon route serves the same softened version every
embedder sees.

### 2026-05-12 — `easyai-cli` session: per-tool checkpoint survives force-exit

The previous save points covered every interruption mode **except
force-exit** — triple rapid Ctrl-C triggers the force-exit handler
(`_exit(130)`), which bypasses `atexit` and the post-`chat()`
save in `run_one()`.  Operators reported that a long agentic turn
that got force-exited left no `.easyai_session` on disk.

Fix: layer an additional save into the `on_tool` callback so
`.easyai_session` is rewritten **after every tool round-trip** in a
turn, not just at the end of the turn.  Only the in-flight partial
reply since the last completed tool is lost; everything earlier
(file edits, bash output, plan steps, RAG queries) is on disk and
re-loadable.

Wiring: `easyai::ui::Streaming::notify_tool(call, result)` is now a
public forwarder for the private on_tool UI handler, so external
embedders can compose extra behaviour onto the `on_tool` slot
(checkpoint to disk, telemetry, audit log) without losing the
streaming output (tool indicators, dim styling, plan rendering).
The cli's binary uses it as:

```cpp
cli.on_tool([&](const ToolCall & c, const ToolResult & r) {
    streaming.notify_tool(c, r);   // canonical UI
    save_session(cli, &err);       // disk checkpoint
});
```

Pattern is documented inline in
[`include/easyai/ui.hpp`](include/easyai/ui.hpp) above the
`notify_tool` declaration.  No flag / INI change.

### 2026-05-12 — Session resume default-ON + every session knob now in `[cli]` INI

Iteration on yesterday's session-persistence feature: loading the
existing `.easyai_session` is now the **default** (you don't need
`--continue` to pick up where you left off).  The semantics flip:

| | Previous (2026-05-12 morning) | Now |
| --- | --- | --- |
| Resume on launch | opt-in via `--continue` | **default ON** |
| Start fresh | default | opt-in via `--no-continue` |
| `--compress` without `--continue` | hard error | warning (no-op when combined with `--no-continue`) |

The cli also now exposes every session-related knob plus the raw-log
knobs through `[cli]` in `/etc/easyai/easyai-cli.ini`:

```ini
[cli]
auto_continue = true       # default; load .easyai_session if present
auto_compress = false      # default; recap on every load when on
log_file      =            # default empty; path enables --log-file equivalent
auto_log      = false      # default; when true, restores the library's legacy /tmp auto-log
show_bash     = true       # default; mirror bash subprocess output to the operator terminal
show_python   = true       # default; same for python3
```

CLI flag precedence is unchanged: explicit flag > INI > hardcoded
default.  All `--continue` / `--no-continue` / `--compress` /
`--log-file` flags continue to work and override the INI for that
invocation.

`--continue` is kept as a no-op alias for backward compat (useful in
scripts that want to force resume even when an operator's INI flipped
`auto_continue` off).

Full doc: [`easyai-cli.md`](easyai-cli.md) §10.

### 2026-05-12 — easyai-cli session persistence + raw log default OFF

Every `easyai-cli` invocation now writes a `.easyai_session` file in
the current working directory after each chat turn (atomic tempfile
+ rename, mode 0600).  Three control points:

| Surface | What it does |
| --- | --- |
| (no flag) | Start fresh, overwrite on first turn, save every turn |
| `--continue` | Resume the `.easyai_session` in cwd; warn + start fresh if none |
| `--continue --compress` | Resume + ask the model for one lossless recap; replace history with the recap before the first prompt |
| `/compress` (REPL) | Same recap flow, fired mid-session |

The file is the raw OpenAI-shape message array (greppable, diffable,
re-loadable).  Two new methods on the public `Client` API
(`dump_history()` / `load_history()`) make the same persistence
available to library embedders.

**Raw log default flipped to OFF.**  Prior versions created
`/tmp/easyai-cli-remote-<pid>-<epoch>.log` whenever `--verbose` was
set, AND the library opened a separate `/tmp/easyai-client-<pid>-<epoch>.log`
on every Client construction.  Both are now opt-in:

* The binary's transaction log opens **only** when `--log-file PATH`
  is given (mode 0600 at PATH).  `--verbose` is now stderr-only.
* The library's auto-log is suppressed by setting
  `EASYAI_NO_AUTO_LOG=1` in the cli binary's `main()` before the
  Client is constructed.  Operator override
  (`EASYAI_NO_AUTO_LOG=0` in the env) still wins.

Net: a default invocation leaves nothing in `/tmp`.  See
[`easyai-cli.md`](easyai-cli.md) §9 and §10 for full docs.

### 2026-05-11 — fs(action="edit") seam-line corruption fix (HIGH, post-publish correction)

A user-reported bug: `fs(action="edit")` was silently corrupting
files when the model passed `content` without a trailing `\n`.
The last byte of `content` got glued onto the first preserved line
after the edit range — turning `int b = 22;\n    return a + b;`
into `int b = 22;    return a + b;`.  When the deleted range
happened to contain the only `}` between two function bodies,
this silently swallowed the brace and the file failed to compile
with "function definition is not allowed here" + "expected '}'"
on the next build.

Root cause: the tool description said "include a trailing `\n`
yourself" but the model consistently forgot.  Fix:
`make_fs_edit_handler` now auto-inserts a `\n` separator on each
side of `content` if and only if one is needed to keep the seam
lines apart.  Both guards no-op when `content` is already
correctly terminated (or empty for a pure delete), so the change
is invisible to model calls that were already doing the right
thing.

Tool description updated to drop the "include trailing `\n`"
advice — line semantics are now preserved automatically.

Verified against a 9-case smoke matrix (middle-replace with/without
trailing newline, multi-line content lacking newline, pure delete,
pure insert, append-at-EOF on files with and without trailing
newline, replace-last-line on a file without trailing newline,
whole-file replacement) — all nine pass.

Documented as §22.8 (post-publish correction) in
[`SECURITY_AUDIT.md`](SECURITY_AUDIT.md); §22.4's "no findings"
claim for the fs.edit/append/ops batch surface has been amended
with a forward-pointer to §22.8.  No CLI / INI / library API
changes; rebuild to pick up the fix.

### 2026-05-11 — Security audit 7th pass (1 HIGH, 1 MEDIUM, 1 LOW; no public-interface change)

Re-applied the standing audit on the ~5,000 LoC added since the 6th
pass (2026-05-08). Three findings, all closed in this commit:

* **HIGH — `run_capped_subprocess` banner sanitization.** The
  `[bash] $ …` / `[python3] $ …` opening banner used to print the
  model-supplied command/code through `fprintf` verbatim, so a
  snippet that embedded an ANSI/OSC sequence could repaint the
  operator's terminal (window title, screen wipe, OSC 52
  clipboard write) one line before any child output arrived. The
  live mirror channel was already hardened in §20.1; the banner
  is now sanitized the same way (CR/LF/TAB pass; ESC rendered as
  visible `^[` marker; other C0/DEL dropped). For `python3` the
  banner now shows the *user's code* only — the 25-line sandbox
  preamble was previously included, cluttering every transcript.
* **MEDIUM — python3 sandbox preamble closure tightening.** The
  preamble that wraps `open()` to pin disk access to the sandbox
  used to leave `_e_open_orig`, `_e_chk`, and `_e_root` at module
  scope, so user code could trivially call the raw `_e_open_orig`
  by name and bypass the check — the comment claimed "closure cell"
  protection that the implementation didn't actually provide.
  Restructured into an `_e_make_wrappers` factory whose function-
  local names become real lexical closure cells; the wrappers
  still work, but the originals are no longer reachable from
  module scope. (Adversarial bypass via `ctypes` / `subprocess` /
  `_io.FileIO` is unchanged and still documented as out-of-scope.)
* **LOW — installer INI-shape validation widened.** §20.4 / §21.4
  already validated `--temperature`, `--top-p`, `--frequency-penalty`, `--ctx-size` etc.
  via `require_numeric` to defeat heredoc injection. Today
  extended the integer roster (`--service-port`, `--threads`,
  `--threads-batch`, `--ngl`) and added a new `require_no_injection`
  helper that rejects `\n` / `\r` / `=` / `[` / `]` in the
  non-numeric knobs (`--service-host`, `--alias`, `--webui-title`,
  `--cache-type-k`, `--cache-type-v`). Same operator-typo /
  hostile-CI threat model as §20.4.

Full narrative in [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) §22.
Rebuild to pick up the fixes — no INI, CLI, or library API changes.

### 2026-05-10 — CLI "thinking" label: static dark gray, no shimmer sweep

The CLI's prompt-eval indicator no longer animates. While the server
is ingesting the prompt the spinner shows a steady `thinking[ N%]`
in 256-colour grayscale 244 (mid-gray, RGB 128/128/128) — bright
enough to read on a dark terminal, dim enough to clearly signal "in
progress, not the model's output." Replaces the 10 Hz spotlight
sweep that landed in `d7e7202`. Drops the dual-cadence heartbeat —
the heartbeat now runs at one cadence (250 ms) and skips its
repaint entirely while the thinking label is up; only
`set_thinking_pct()` (driven by the server's `easyai.prompt_progress`
SSE event) triggers a redraw when the % suffix changes.

### 2026-05-09 — `python3` tool result rendered with the executed snippet

The tool result returned by `python3` now opens with a fenced
```python ...``` block carrying the snippet that just ran, followed
by a `[python3 executed]` notification line, then the exit code and
captured output. Chat UIs that render markdown (the embedded webui,
typical clients) display the code with syntax highlighting, so an
operator skimming the conversation transcript can see what executed
without having to expand the raw tool-call JSON.

The model's `code` argument is what gets rendered — the
`kPythonSandboxPreamble` (the disk-restriction monkey-patch) is
deliberately stripped from the displayed source so the transcript
isn't cluttered with the same 25 lines on every call.

Result shape:

```
```python
<the snippet>
```
[python3 executed]
exit=0
<captured stdout+stderr>
```

Spawn-side errors (pipe / fork failure — the interpreter never
ran) still surface unwrapped, so the error message stays the
actual cause and isn't dressed up with a misleading "executed"
notice.

### 2026-05-09 — METRICS line: always on, default every 5 minutes

The periodic METRICS log line in `easyai-server` is now emitted
**unconditionally** — no longer gated on `--verbose`. Operators
need the CPU / mem / GPU / TCP-state / TIME_WAIT-pressure telemetry
in journalctl whether or not they're chasing a debug session.

* `metrics_interval` default raised from `1` second to `300`
  seconds (5 minutes). Low-overhead enough to leave on permanently
  in production; bump **down** (60, 30, 5) when actively
  troubleshooting.
* The systemd installer's `easyai.ini` template was bumped from
  `metrics_interval = 60` to `metrics_interval = 300` to match.
* `--verbose` no longer claims the METRICS line in its description
  or banner — only the request-level `→` / `←` lines remain
  verbose-only.

Existing operators who pinned `[SERVER] metrics_interval` in their
INI keep their value; only the unspecified default shifts.

### 2026-05-09 — `python3` is default-on with a sandboxed disk surface

Promoting `python3` from explicit-opt-in (--allow-python) to
auto-on whenever the operator has signalled "the model can touch
files" — same gate as `fs`: --sandbox set OR --allow-bash on. The
embedded webui inherits this for free since the systemd unit ships
with --sandbox /var/lib/easyai/workspace.

* **`--allow-python` removed; `--no-python` is the new opt-out.**
  Mirrors `--no-web` / `--no-datetime`: the tool defaults on and
  operators who don't want it pass the `--no-*` flag (or set
  `[SERVER] allow_python = off` in the INI).
* **Disk access auto-restricted to the sandbox root.** Every
  snippet is auto-prefixed with a short Python preamble that
  monkey-patches `builtins.open`, `io.open`, and `os.open` to
  reject any path resolving outside the cwd Python was chdir'd
  into. `open("/etc/passwd")` raises `PermissionError`;
  `pathlib.Path("/etc/hostname").read_text()` raises through
  `pathlib`'s internal `open()` call.
* **Description rewritten to forbid disk use.** "USE FOR: testing,
  calculation, data processing, networking, information gathering.
  NEVER USE FOR DISK — every disk operation has a fs(action=...)
  equivalent." The preamble is defense-in-depth; the description
  is the primary contract.
* **Defense-in-depth, not a real sandbox.** The model can still
  escape via `import ctypes; ctypes.CDLL("libc.so.6").open(...)`,
  `subprocess.run(["cat", "/etc/passwd"])`, or `os.system(...)` —
  the protection is against accident, not adversarial intent. Same
  threat model as `bash`: explicit operator opt-in, not a real
  sandbox.

### 2026-05-09 — `python3` tool: isolated Python 3 snippet runner

A second shell-class executor alongside `bash`, gated by its own
`--allow-python` flag (off by default — same threat model as bash).
The model gets one extra tool when enabled:

* `python3(code, timeout_sec?)` — runs the snippet via
  `python3 -I -S -E -c <code>`. Isolated mode: no `PYTHON*` env vars,
  no `site.py` / no .pth files / no site-packages, no cwd on
  `sys.path`. The standard library is available; `import requests`
  fails with `ModuleNotFoundError`, by design — predictable behaviour
  regardless of host Python configuration.
* Same hardening as `bash`: cwd pinned to `--sandbox`, fds 3+ closed
  before exec, SIGTERM/SIGKILL deadline, 32 KB stdout+stderr cap,
  optional operator-facing live mirror via `--no-show-python` to opt
  out (default ON when `--allow-python` is on).
* Internally, `bash` and `python3` now share one `run_capped_subprocess`
  helper — the fork/fd-close/chdir/drain/wait machinery only lives in
  one place.

When to reach for `python3` vs `bash`: data manipulation (JSON, regex,
Decimal math, statistics, date arithmetic) is one Python snippet; shell
pipelines / build runners / git / package managers stay in `bash`.

`--allow-python` flag is wired through every binary (`easyai-cli`,
`easyai-local`, `easyai-server`, `easyai-mcp-server`) plus the INI
`[SERVER] allow_python` key. `EASYAI-*.tools` manifests cannot shadow
the new `python3` reserved name.

### 2026-05-09 — One tool per concept: unified `web`, unified `fs`, RAG `--split-rag` removed

A consolidation pass on the built-in tool surface. Three loose
collections (web, filesystem, rag) collapsed to one tool each, all
shaped the same way — single `Tool` with an `action` parameter and a
flat schema (every parameter optional except `action`). Pattern
mirrors the rag dispatcher introduced 2026-05-04.

* **`web` tool** — `web(action="search"|"fetch")`. Replaces the
  separate `web_search`, `web_fetch`, and `web_google` tools. Search
  takes an `engine` parameter (`"auto"` default — cascades through
  google → brave → ddg-lite → bing → ddg, returning the first that
  succeeds; explicit picks: `"google"` opt-in via `--use-google` plus
  the GOOGLE_API_KEY / GOOGLE_CSE_ID env vars, `"brave"` keyless HTML
  scrape with the best understanding of niche named entities,
  `"ddg-lite"` keyless no-JS DDG endpoint with a Netscape UA (page 1
  only — bypasses the anti-bot wall the modern DDG endpoint applies),
  `"bing"` keyless RSS feed, `"ddg"` keyless HTML scrape but
  increasingly blocked from server IPs). Both actions take `page` for
  pagination; `fetch` takes `start` + `limit` for byte-window control.
* **`fs` tool** — `fs(action="read"|"write"|"list"|"glob"|"grep"|"check_path"|"cwd"|"sandbox")`.
  Replaces seven separate factories plus `get_current_dir` and
  `get_sandbox_path`. `--allow-fs` now registers one tool, not seven.
* **`--split-rag` removed.** The legacy seven `rag_*` tools and the
  `--split-rag` flag are gone everywhere — CLI, INI, examples, all
  four binaries. The single `rag(action=...)` dispatcher (default
  since 2026-05-04) is the only RAG layout. On-disk format unchanged.
* **Public-API breakage.** Anyone consuming `libeasyai` directly: the
  individual `easyai::tools::web_search()` / `web_fetch()` /
  `web_google()` / `fs_read_file()` / `fs_write_file()` / `fs_list_dir()`
  / `fs_glob()` / `fs_grep()` / `fs_check_path()` / `get_current_dir()`
  / `get_sandbox_path()` / `make_rag_tools()` / `RagTools` factories
  are removed. Switch to `easyai::tools::web(google_enabled)`,
  `easyai::tools::fs(root)`, and `easyai::tools::make_rag_tool(root)`.
* **Why.** Three matching surfaces with the same shape make the
  catalogue smaller (one entry per capability instead of nine), tool
  prose can use one consolidated description style across all three,
  and the model reasons about each capability as ONE thing with sub-
  actions. The flat-schema-with-runtime-validation choice is the
  same one the unified rag tool already validated against weak /
  1-bit-quant tool callers.

### 2026-05-08 — Server observability + connection-pool fix + prompt cleanup

Driven by a real production failure: an agentic session hung mid-stream,
the cli retried six times, and we had no visibility into what the
TCP stack was doing on the server. Fixes landed across the cli's
HTTP transport, the server's verbose logging, the system prompts,
and the build.

* **Cli keep-alive bug fixed (the actual root cause).**
  `stream_chat()` / `simple_get()` / `simple_post()` were each
  constructing a fresh `httplib::Client` per call. The Client's
  TCP socket dropped at function end, so `set_keep_alive(true)` had
  nothing to keep alive — every agentic hop opened a new connection.
  An N-tool-call session piled up N sockets in `TIME_WAIT`,
  eventually exhausting the client's ephemeral port range or
  per-process fd ceiling. **Hoisted a single persistent `httplib::Client`
  onto the `Impl` struct; all three call sites now reuse it.** ONE
  TCP connection per session instead of N. Cancellation and
  server-restart paths are preserved (cpp-httplib reconnects
  internally on dead-socket errors).
* **Server: HTTP-level `→` / `←` log per request (verbose mode).**
  `set_pre_routing_handler` + `set_logger` emit arrival and
  completion lines with method/path/peer/body size, status,
  duration, response bytes (or `streamed` for SSE), and running
  totals (req / err / tools / in_flight / bytes_in / bytes_out).
* **Server: periodic `METRICS` line with TCP state breakdown.**
  Background ticker every `metrics_interval` seconds
  (`--metrics-interval N` or `[SERVER] metrics_interval` to tune,
  `0` disables — **default raised to 300 / always-on as of
  2026-05-09**, see entry above) emits one
  line with: CPU% + iowait%, load 1/5/15, process RSS + peak,
  system memory total/used/%, AMD GTT used/total/% (Linux + AMD
  only), in-flight requests, cumulative requests / errors / bytes,
  fd usage vs RLIMIT_NOFILE, AND an explicit TCP state breakdown
  (ESTABLISHED / TIME_WAIT / CLOSE_WAIT / FIN_WAIT / LISTEN)
  parsed from `/proc/net/tcp{,6}` with
  `TIME_WAIT N/M ephemeral ports (X.X% [elevated|HIGH|CRITICAL])`
  so socket exhaustion shows up in `journalctl` long before
  connections start failing. Linux-only for the deep metrics;
  macOS prints `n/a` and the server runs fine — easyai-server's
  deploy target is Linux.
* **Tool dispatch timing in every visible log.** Engine wraps
  `tool->handler()` with `steady_clock` and writes `duration_ms`
  into `ToolResult`. CLI shows `🔧 web_search (412ms)({"query":...})`
  and the webui's reasoning panel shows the same. The
  `easyai.tool_result` SSE event also gains a `duration_ms` field
  so future external SSE consumers can render their own timing UI.
* **`allow_fs = off` in the INI is now honoured.** The server read
  the flag but never propagated it to the toolbelt — a non-empty
  `[SERVER] sandbox` re-enabled `fs_*` regardless. Default install
  ships `allow_fs = off` + `sandbox = /var/lib/easyai/workspace`,
  which hit exactly this. Now `allow_fs` and `allow_bash` are
  honoured independently of `sandbox`. **Behaviour change:**
  `--sandbox /foo` alone NO LONGER implies `--allow-fs`; pass
  `--allow-fs` explicitly to register fs_*.
* **Built-in system prompt is tool-aware.** The hardcoded prompt
  used to list `fs_*` / `bash` / `plan` / host-metric tools by name
  whether or not they were registered. Models hallucinated calls to
  unregistered tools (especially `bash` after the `allow_fs` fix
  above). The `Tool notes:` section is now built dynamically:
  each bullet is gated on the same flag that controls registration,
  and the entries for tools the server NEVER registers (`plan`,
  host metrics) are removed entirely. Same fix in
  easyai-local's built-in prompt.
* **RAG tool descriptions spell out "model-only store".** Added a
  `PRIVATE — MODEL-ONLY STORE` paragraph to `rag_save` / `rag_append`
  / the unified `rag` dispatcher, telling the model that the user
  has no UI / command / API to read what's saved there. Forbids
  `"check the rag for the code"` / `"I saved it to memory"` answers
  and tells the model to `rag_load` and put the body inline when
  the user asks for stored content.
* **Stay-in-scope replaces "PROTOTYPE FIRST".** The old 1./2./3.
  ritual ("build → verify → ASK which next step") was making the
  agent stop after step 1 and ask, even when the user wanted the
  simplest end-to-end thing. Collapsed to a single
  `## Stay strictly in scope` paragraph that keeps the no-extras /
  no-defensive-scaffolding / no-while-I'm-at-it-cleanups specifics
  and drops the build-then-ask dance. Updated everywhere the
  wording lived: server.cpp built-in prompt, local.cpp built-in
  prompt, cli.cpp [guidance] block, installer's
  `/etc/easyai/system.txt` template.
* **Installer GTT default 28 → 29 GiB.** `gtt_gb=29` in
  `scripts/install_easyai_server.sh`. Matches `ttm.pages_limit=7602176`.
  Leaves headroom for a Q5_K_M / MXFP4_MOE 30B MoE plus a 32k KV
  cache fully on the iGPU.
* **Quick-start editor section added to `LINUX_SERVER.md`.** New
  section 0 with copy-paste shell snippets for VSCode + Continue.dev,
  OpenCode, and VSCode + Cline, all pointing at `http://ai.local:80/v1`.
  Plus a quick-reference table for other OpenAI-compatible clients.
* **No patches or derivatives of llama.cpp.** A short-lived
  experiment subclassed `httplib::Server` to log per-TCP-connection
  accept/close events — that needed widening the access on a
  private virtual in the vendored cpp-httplib header. Backed out
  entirely: no CMake patch script, no `#define private protected`
  trick, no derivative copies. The HTTP `→`/`←` lines and the
  periodic METRICS line (with system-wide TCP state breakdown
  including TIME_WAIT pressure) cover the same diagnostic ground
  using only public APIs and `/proc`.

### 2026-05-08 — `tool_lookup` builtin + tool-discipline rule

Builds on the same-day "Built-in system prompt is tool-aware" work
above with a complementary affordance: the model gets a runtime
introspection tool so it can verify what's wired up before
dispatching, and an authoritative discipline rule that points at
that tool. Driven by the same failure mode the prompt-cleanup
addressed (`write` / `read` / `ls` etc. invented by the model);
this layer makes the closure explicit and gives the model a
recovery path when it's uncertain.

* **New `tool_lookup` builtin.** Read-only introspection over the
  agent's live tool registry. Call it with no args to get a numbered
  catalogue of every registered tool (1..N), or pass
  `name="<substring>"` to filter — case-insensitive, partial match.
  Output is plain numbered text the model parses naturally; only
  active tools are returned. Wired into every binary
  (`easyai-cli`, `easyai-server`, `easyai-mcp-server`, `easyai-local`,
  `easyai-agent`, `easyai-recipes`) and the `LocalBackend` library
  wrapper. Always registered last so its snapshot covers every
  other tool, including itself. Public C++ API:
  `easyai::tools::tool_lookup(getter)` where `getter` is a callable
  returning `std::vector<std::pair<std::string,std::string>>` of
  (name, description) pairs.
* **Authoritative `[tools]` / "Tool discipline" prompt block.**
  Layered on top of the closed-set rule from the prompt-cleanup
  commit: *"This catalogue is the SINGLE SOURCE OF TRUTH; training
  data is NOT; if a name isn't in this list IT DOES NOT EXIST;
  call `tool_lookup` first when uncertain; do not retry an
  unknown-tool call."* Common hallucinated names called out by
  example: `write`, `read`, `ls`, `cat`, `curl`, `python`, `sed`,
  `grep`, `find`, `mkdir`. Same wording in `easyai-cli` (the
  `[tools]` block injected into the dynamic prefix), `easyai-server`
  and `easyai-local` (the `## Tool discipline` section in their
  `kBuiltinSystem` strings).

### 2026-05-08 — Fifth-pass security hardening (no behaviour change)

A fresh static review of the ~5,000 lines that landed in the last 30
commits. Two HIGH, three MEDIUM, two LOW findings — all closed in
this commit; every public interface (CLI flags, tool names, library
headers, INI keys) is unchanged.

* **bash live-mirror is now control-byte stripped and byte-capped.**
  When the model calls `bash`, the merged stdout/stderr was being
  mirrored verbatim to the operator's terminal. A model could emit
  `\e]0;HACKED\a` to retitle the operator's window or `\e[2J` to wipe
  the screen — neither showed up in the model-facing tool result.
  Now: ESC is rendered as a visible `^[`, all other C0 controls are
  dropped, and the mirror channel is capped at 128 KiB (model still
  gets the full 32 KiB it always did). Set `[cli] show_bash = false`
  or `--no-show-bash` to silence the mirror entirely.
* **`plan` tool render strips control bytes from item text.** Same
  hijack class, narrower budget — a `plan add` with embedded `\e[…`
  no longer reaches the operator's terminal raw.
* **`get_array` parser now caps stringified-array recursion depth.**
  Tool-args parsing tolerates `"items": "[…]"` (the array escaped
  into a JSON string — small models double-escape sometimes). The
  unwrap path was recursive without a depth cap; a hostile model
  emitting deeply-nested escapes blew the stack. Capped at depth 4
  (legitimate cases stay under depth 2).
* **`get_sandbox_path` now uses `fs::weakly_canonical`.** Was using
  `realpath()` with a "fall back to the unresolved input" branch
  that could leak relative-path shape into the model on transient
  errors. Cosmetic but correct; matches the canonicalisation the
  sandbox containment check uses.
* **`--mcp <url>` rejects non-`http(s)://` schemes up front.** The
  libcurl protocol filter still blocks `file://`, `gopher://` etc.
  at transport time, but the operator now gets a clear error
  instead of a curl diagnostic, and embedders using
  `easyai::mcp::fetch_remote_tools` get the same defence-in-depth.
* **Installer validates numeric sampling/timeout flags.**
  `--temperature`, `--top-p`, `--top-k`, `--min-p`,
  `--repeat-penalty`, `--frequency-penalty`, `--max-tokens`, `--http-timeout`, `--ctx-size`
  must match `^-?[0-9]+(\.[0-9]+)?$` before they flow into the INI
  via heredoc. Closes a defence-in-depth gap where a crafted value
  containing `\n` could inject extra INI keys.
* **`/etc/easyai/easyai.ini.bak` (created by `--force`) gets
  explicit `chmod 640` and `chown root:easyai`.** Previously
  inherited whatever the live INI had; matches the new file's
  posture so a token leak via a backup with looser perms is
  impossible.

Full write-up: [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) §0 (operator
TL;DR) and §20 (this pass's findings). Read §0 if you operate easyai
in production — it's the 60-second summary of what easyai does and
doesn't protect for you.

### 2026-05-05 — Tool surface + system prompt overhaul

Driven by a production "models drift, use bash for file work, ignore
tools" report. The fix landed across the tool descriptions, the
default prompts, and the CLI flag wiring at once.

* **`--sandbox` and `--allow-bash` now imply `fs_*`.** The previous
  matrix had operators passing `--allow-bash --sandbox DIR` and ending
  up with bash but no file tools — so the model fell back to
  `cat > file` / `cat <<EOF` / `sed -i` for everything. Bash is
  strictly more permissive than `fs_*`, so requiring an extra flag
  was inverted. Both flags now register the full file set (and the
  new `get_sandbox_path` companion) at once. `--allow-fs` still works
  for the no-sandbox / no-bash case; otherwise it's redundant.
* **New `get_sandbox_path` tool.** Returns the absolute path of the
  sandbox root, pinned at registration time — distinct from
  `get_current_dir` which is the live process cwd and can drift.
  Lets the model resolve where its work actually lands without a
  wasted `pwd` tool hop.
* **`bash` description rewritten.** Now leads with **PREFER fs tools**
  and lists the exact bash anti-patterns (`cat > file`, `cat <<EOF`,
  `echo > file`, `mkdir`, `sed -i`) with the dedicated tool that
  replaces each. Reserves bash for shell features the dedicated
  tools don't have — pipelines, `find | xargs`, build runners
  (make / cmake / cargo / npm), git, package managers, sed/awk for
  in-place edits.
* **System prompts inject `[environment]` + `[guidance]`.** When
  any create/mutate affordance is registered (fs_* / bash / plan),
  the cli prepends two short blocks to the user's `--system` content:
  the absolute sandbox path (saves a "where am I" tool hop on turn 1)
  and a stay-in-scope behavioral rule (build EXACTLY what the user
  asked — no extras, no defensive scaffolding, no "while I'm at it"
  cleanups). The same guidance lives in the server's Deep persona
  and easyai-local's built-in prompt.
* **Default sampling preset → `precise`** (was `balanced`).
  Temp 0.2, top_p 0.92, top_k 50, min_p 0.03. Tuned for code,
  math, and factual Q&A — the dominant use case for a tool-calling
  agent. Flipped across server, local, cli, webui, library
  fallbacks, and the systemd installer's INI templates. README's
  preset table now includes a Behaviour column and a "Pick when…"
  column to make the choice explicit.
* **`--show-system-prompt`** added to all four binaries
  (`easyai-cli`, `easyai-server`, `easyai-local`, `easyai-chat`).
  Resolves the system prompt the binary would actually use (built-in
  default → `--system-file` → `--system`, plus the cli's injected
  blocks), prints, exits. No model load, no port bind, no network.
  Useful for confirming the persona before bouncing a service.
* **Graceful `Ctrl-C` in `easyai-cli`.** In interactive mode (no
  `--quiet`), the first `Ctrl-C` mid-turn prints
  `<exiting: waiting for the ai session to be finished. Ctrl-C
  again to force.>` and lets the in-flight chat finish naturally
  (rc=0). Conversation isn't truncated mid-stream. Second `Ctrl-C`
  is the hard-cancel escape hatch (rc=130). `--quiet` keeps the
  existing immediate-cancel for batch scripts.
* **Plan tool tolerance shims.** `args::get_array` now accepts a
  stringified JSON array (`"items": "[...]"`) — small/quantised
  models repeatedly emit this shape. The handler infers a missing
  `action` from the items' fields plus current plan state, and
  maps common synonyms (`create` → `add`, `remove` → `delete`,
  etc.). `add` honours an optional per-item `status` so create +
  mark "working" lands in one call. Errors include the correct
  shape inline so the model can copy-fix.
* **Plan re-renders coalesce.** A new `Plan::Batch` RAII guard
  collapses N per-item `on_change` callbacks across one tool call
  into a single fire — the UI's "── plan ──" block now prints once
  per batch, not once per item.
* **New doc: [`easyai-cli.md`](easyai-cli.md)** mirrors
  `easyai-server.md`. 14 sections covering connection, modes, full
  flag reference, tool registration, system prompt + injection,
  sampling, reasoning streams, the raw transaction log, RAG,
  external tools, management subcommands, worked examples,
  cross-references.
* **Tool authoring guide.** New `design.md §5 Writing tool
  descriptions reliably` (architectural) and `manual.md §3.2.1`
  (cookbook) document the rag-style multi-action pattern, the
  per-`.param()` "Used by add / update / …" idiom, and the
  lenient-handler tolerance shims. `AI_TOOLS.md` Chapter 9 has a
  pointer.

### 2026-05-04 — Single-tool RAG is now the default; concise system prompt

* **Default RAG layout flipped: one `rag(action=...)` tool.** The
  unified single-tool dispatcher used to be opt-in behind
  `--experimental-rag`; it is now the default for every binary
  (`easyai-server`, `easyai-cli`, `easyai-local`, `easyai-mcp-server`).
  One catalog entry instead of seven keeps the model's tool list
  short and saves a few hundred tokens per turn. On-disk format,
  locking, and fix-memory rules are unchanged.
* **`--split-rag` opts back into the legacy seven `rag_*` tools.**
  Replaces `--experimental-rag`. Same semantics, opposite default.
  Wired as a CLI flag on every binary AND as `[SERVER] split_rag`
  in the INI overlay (`easyai.ini` / `easyai-mcp.ini`; per-model
  overrides via `[MODEL_<pattern>]` sections). Useful for
  weak / 1-bit-quant tool callers (Bonsai-class) that handle many
  flat schemas more reliably than one discriminated schema.
* **Default system prompts trimmed.** `easyai-server` and
  `easyai-local` now ship a much shorter built-in prompt focused on
  a tight **plan → act → iterate** loop with one small concrete
  next step at a time, finishing as soon as the answer is useful so
  the user has room to refine. Cuts about three quarters of the old
  prompt's length while keeping the no-announce-without-call rule
  and the search → fetch discipline.

### 2026-05-02 (later) — RAG `rag_append` + user-focus prompts

* **`rag_append` — new RAG tool.** Adds new content to the end of
  an existing memory without losing the previous body. Read-modify-
  write under one `unique_lock` on the store's `shared_mutex`, so
  concurrent appenders queue cleanly (no lost appendix, no torn
  merge for any reader); on disk the new content is separated from
  the old by a Markdown horizontal rule (`---`) so the operator
  reading the `.md` file sees exactly where each appendix starts.
  Refuses on titles that don't exist (use `rag_save`), on fixed
  memories (`fix-easyai-*`), and when the merged size would exceed
  256 KiB. Optional `keywords[]` parameter merges into the existing
  keyword list (deduped, capped at 8). Wired into every consumer
  (server, MCP server, CLI, local backend) and the experimental
  single-tool dispatcher (`rag(action="append", ...)`). Full doc:
  [`RAG.md`](RAG.md) §4.
* **User-focus prompt update.** `rag_save` and `rag_append` tool
  descriptions now explicitly tell the model to prioritise notes
  about the user themselves — name, role, hardware, projects,
  working style, corrections, likes, dislikes — and to grow that
  memory across sessions with `rag_append` instead of rewriting it
  with `rag_save`. The next conversation (tomorrow, three months
  from now) starts with the user already known, so they don't have
  to explain themselves twice. The lib went from 5/6 RAG tools to
  the canonical seven (`rag_save`, `rag_append`, `rag_search`,
  `rag_load`, `rag_list`, `rag_delete`, `rag_keywords`); all CLI
  help text, help comments, and docs updated to match.

### 2026-05-02 — Fourth-pass security audit + readability batch

* **`/tmp` log file hardened (security, MEDIUM).** The auto-generated
  raw transaction log at `/tmp/easyai-<pid>-<epoch>.log` is now
  created with `O_EXCL | O_NOFOLLOW | O_CLOEXEC` and mode `0600`. The
  predictable path used to follow symlinks on `fopen("w")`, so a
  local attacker on a multi-tenant host could plant a symlink
  pointing at any user-writable file (`~/.bashrc`, `~/.ssh/…`) and
  have the next `easyai-*` process truncate-and-overwrite it.
  Mode `0644` (process umask) also leaked prompts — which can
  contain API keys or PII — to other accounts on the same box.
  `O_EXCL` makes the create atomic-or-fail and `0600` keeps logs
  private. Caller-supplied paths (`--log-file PATH`) keep `O_TRUNC`
  for log rotation but still gain `O_NOFOLLOW + 0600`. Full
  write-up in [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) §19.
* **Internal readability batch (no public API change).** Three
  inline patterns were lifted into named helpers so the call sites
  read top-to-bottom: `file_mtime_unix()` (replaces three copies of
  the C++17 file_clock→system_clock idiom in `rag_tools.cpp`),
  `glob_to_regex()` + `kGlobRegexMetachars` (lifts the wildcard
  state machine out of `fs_glob` in `builtin_tools.cpp`), and
  `looks_like_announce_phrase()` (lifts the 30-line retry predicate
  out of `Engine::chat_continue` in `engine.cpp`, where it was
  used twice). All seven binaries build clean.

### 2026-05-01 — MCP CLIENT, RAG memory framing, web_google, macOS installer fix

* **`easyai-server` is now also an MCP client.** Pass `--mcp <url>`
  (and `--mcp-token <token>` if needed) and at startup the server
  connects to the upstream's `/mcp`, runs `tools/list`, and merges
  the catalogue into its own. Each remote tool's handler proxies
  `tools/call` over HTTP. Local tool names win on collision. The
  implementation is `easyai::mcp::fetch_remote_tools()` in libeasyai
  — public API, so anything built on the engine library can stack
  remote MCP catalogues. See [`MCP.md`](MCP.md) §9.5.
* **`--no-tools` renamed to `--no-local-tools` (server only).** Now
  that the server can be both an MCP server AND an MCP client, the
  flag's scope had to be unambiguous: it disables only the LOCAL
  built-in toolbelt. RAG, external tools, and tools fetched via
  `--mcp` are unaffected. INI key `load_tools` → `local_tools` to
  match. The `easyai-local` and `easyai-mcp-server` binaries keep
  their `--no-tools` spelling — they have no MCP client, so the
  original name is still accurate.
* **RAG reframed as memory + fixed memories.** Tool descriptions
  rewritten in memory verbs (search / store / recall / update /
  forget). New `fix=true` argument on `rag_save` mints an immutable
  memory: title is auto-prefixed with `fix-easyai-`, and from then
  on `rag_save` refuses to overwrite it and `rag_delete` refuses to
  remove it. Use this to seed system designs, hard rules, ground-
  truth definitions the model must not rewrite. Search / load /
  list output gain a human-readable `modified` date and a `[FIXED]`
  / `fixed: yes/no` marker. See [`RAG.md`](RAG.md).
* **Single-tool RAG dispatcher is the default.** One
  `rag(action=...)` tool exposes save / append / search / load /
  list / delete / keywords as sub-actions. Same store, same
  handlers, same on-disk format. Saves a few hundred catalog tokens
  per turn and keeps the model's tool list short. Pass `--split-rag`
  (or `[SERVER] split_rag = on` in the INI) to opt back into the
  legacy seven separate `rag_*` tools — useful for weak / 1-bit-
  quant tool callers (Bonsai-class) that handle many flat schemas
  more reliably than one discriminated schema.
* **`web_google` builtin.** Google Custom Search JSON API. Gated by
  `--use-google` (also `[SERVER] use_google`). Reads
  `GOOGLE_API_KEY` and `GOOGLE_CSE_ID` from env at call time so a
  rotation doesn't drop the tool. Free tier is 100 queries/day.
* **macOS installer fix: OpenSSL via brew.** Modern macOS no longer
  ships usable libssl in `/usr/lib`, so `find_package(OpenSSL)`
  half-detected and broke configure for both `easyai_cli` and the
  vendored `cpp-httplib`. The installer + `build_macos.sh` now pass
  `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)` and the cmake
  guards `TARGET OpenSSL::SSL` so a half-detected OpenSSL degrades
  to "HTTPS not in this build" instead of erroring out.

### 2026-04-30 — `easyai-mcp-server` (standalone MCP provider)

* **New binary `easyai-mcp-server`.** Same tool catalogue as
  `easyai-server` (built-ins + RAG + operator-defined external-tools)
  exposed over `POST /mcp` with **no GGUF model loaded** — designed
  for high-concurrency multi-client deployments. Configurable
  cpp-httplib worker pool (`--threads`, default 256) and a separate
  in-flight `tools/call` cap (`--max-concurrent-calls`, default 256)
  that returns 503 + `Retry-After` on saturation instead of unbounded
  queueing. Full doc: [`easyai-mcp-server.md`](easyai-mcp-server.md).
* **RAG concurrency upgrade.** `RagStore::mu` is now
  `std::shared_mutex`; `rag_search` / `rag_load` / `rag_list` /
  `rag_keywords` take `std::shared_lock` so parallel readers don't
  serialise on the write path. Benefits every consumer of libeasyai
  — `easyai-server`, `easyai-cli` with `--RAG`, any third-party
  program calling `make_rag_tools()`. Atomic-rename writes already
  made on-disk reads tear-free; the lock relaxation is safe.
* **Doc restructure.** `INI_KFlags.md` content has moved to the top
  of the new [`easyai-server.md`](easyai-server.md) so the chat
  server's INI / CLI / API / persona / hardening reference lives in
  one file. `LINUX_SERVER.md` is unchanged — it remains the
  systemd-installer-specific operator's guide.

### 2026-04-30 — Tunable incomplete-retry budget + live retry visibility

* **`--max-incomplete-retries N` (also `[ENGINE] max_incomplete_retries`).**
  Default 10 — how many times the engine discards + nudges + retries
  when the model finishes a turn announcing an action ("Let me…",
  "I'll…") without actually emitting the tool_call. Bump to 15-20
  for weak / 1-bit-quant models (Bonsai-8B-Q1_0 frequently needs
  the extra budget); set to 0 to disable retries entirely.
* **Retries now visible in the Thinking panel.** Engine fires a new
  `on_incomplete_retry(attempt, max, reason)` callback per retry,
  the server pipes it into the SSE `reasoning_content` channel, and
  the webui renders `↻ Retry 3/10: model said: "Let me search…" (no
  tool_call) — nudging.` while it happens. No more frozen UI for 10
  silent retries followed by a blank bubble.
* **Engine warnings always log** (regardless of `--verbose`):
  cancellation, thought-only retry, reasoning→content fallback,
  incomplete-retry, empty final content. `--verbose` is for raw
  per-token / per-hop diagnostic noise; actionable warnings stay on
  so operators see them in `journalctl` without flipping a flag.

### 2026-04-30 — Bonsai 8B Q1_0 onboarding + security pass

* **One-shot installers for macOS and Raspberry Pi 4/5.**
  `scripts/install_easyai_macos.sh` builds with Metal/AMX, drops the
  model, prints the run command. `scripts/install_easyai_pi.sh` does
  the full Pi appliance: systemd unit, mDNS so the box answers as
  **`pi-ai.local`** on your LAN, port 80 with
  `CAP_NET_BIND_SERVICE`. Both clone the **PrismML fork** of
  llama.cpp (the only one with the Q1_0 kernel — upstream loads the
  GGUF then fails at decode).
* **Security third-pass audit** — 3 HIGH and 7 MEDIUM findings fixed.
  The INI overlay used to be silently ignored (every `[ENGINE]` /
  `[SERVER]` key was a no-op); `--no-mcp-auth` was disconnected from
  the gate; the sandbox could be escaped by a symlink planted via
  `bash`. All closed. The `bash` tool now gets the same
  fork-hardening as external tools — `PR_SET_PDEATHSIG`, fd
  close-loop bounded against `RLIMIT_NOFILE = unlimited`, process-
  group kill on timeout. Plus JSON-depth caps on every parser, a
  bounded INI parser, mode 0600 on RAG entries, and a
  body-size-bounded auth header. See [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) §18.
* **MCP server.** `easyai-server` is now a Model Context Protocol
  provider on `POST /mcp` (protocol 2024-11-05). Claude Desktop,
  Cursor, Continue list and dispatch every registered tool — your
  built-ins, your RAG, your `--external-tools` manifests — over a
  single endpoint. Bearer auth via `[MCP_USER]` in the INI; a
  Python stdio bridge ships at `scripts/mcp-stdio-bridge.py` for
  Claude Desktop. See [`MCP.md`](MCP.md).
* **Single INI config — `/etc/easyai/easyai.ini`.** Every CLI flag
  has an INI key (FlagDef table refactor); precedence is CLI > INI
  > hardcoded default. Edit the file, `systemctl restart`, done.
  Full reference in [`easyai-server.md`](easyai-server.md) §1.
* **RAG: persistent memory.** Seven tools (`rag_save`, `rag_append`,
  `rag_search`, `rag_load`, `rag_list`, `rag_delete`, `rag_keywords`).
  Multi-keyword search (first keyword required, rest rank by overlap)
  + pagination. One Markdown file per entry — operator-readable,
  hand-editable. See [`RAG.md`](RAG.md).

### 2026-04-29 — External tools v2

* **Operator-defined tool packs** via `EASYAI-<name>.tools` JSON
  manifests dropped in `/etc/easyai/external-tools/`. Per-file
  fault isolation, sanity warnings (shell-wrapper detection,
  world-writable binaries, `LD_*` env passthrough), full
  `fork`+`execve` hardening — never a shell. Give the model
  focused powers without flipping `--allow-bash`. See
  [`EXTERNAL_TOOLS.md`](EXTERNAL_TOOLS.md).
* **`get_current_dir` builtin** — the model can ask where it is,
  so relative paths in `bash` / `fs_*` calls land where you expect.
* **Cancel-on-disconnect on the server** — closing the browser
  tab actually stops the decode loop. No more zombie generation
  eating tokens after the user walked away.
* **Tolerant tool output** — non-UTF-8 bytes in tool results no
  longer abort the SSE stream; the bytes get a U+FFFD substitute
  and the stream stays alive.

---

## All options at a glance

Every CLI flag, INI key, and library setter the project ships
today, in tables. Skim once to learn the surface; come back when
you want to tune something specific. Deeper reference is linked
per row.

This repo builds seven binaries. Two are production daemons
(`easyai-server`, `easyai-mcp-server`), two are user CLIs
(`easyai-cli`, `easyai-local`), three are example apps the lib
ships to demonstrate the API (`easyai-chat`, `easyai-agent`,
`easyai-recipes`).

### `easyai-server` — chat HTTP server (also speaks MCP)

Full reference: [`easyai-server.md`](easyai-server.md).
INI defaults under `/etc/easyai/easyai.ini` — every flag below
has a matching INI key (see [`easyai-server.md`](easyai-server.md) §1).

| Flag | Default | What it does |
|---|---|---|
| `-m, --model PATH` | (required) | GGUF model file. |
| `--config PATH` | `/etc/easyai/easyai.ini` | Central INI; CLI > INI > hardcoded. |
| `--host ADDR` | `127.0.0.1` | Bind address (`0.0.0.0` = any iface). |
| `--port N` | `8080` | TCP port. |
| `--max-body N` | 8 MiB | Cap on request body. |
| `-s, --system-file PATH` | — | Default system prompt, from file. |
| `--system TEXT` | — | Default system prompt, inline. |
| `--no-local-tools` | off | Don't expose the local built-in toolbelt. |
| `--mcp URL` | — | Connect upstream MCP server as client; merge catalogue. |
| `--mcp-token TOK` | — | Bearer for `--mcp`. |
| `--no-mcp-auth` | off | Force `/mcp` open even with `[MCP_USER]` populated. |
| `--http-retries N` | 5 | Extra attempts on transient HTTP failures (MCP client + web tools). 0 disables. Logged on stderr. |
| `--http-timeout SECONDS` | 600 | Read/write timeout for the listen socket AND the MCP-client connection. Bumped from llama-server's 60 s default to accommodate long thinking turns. |
| `--sandbox DIR` | server cwd | Root for `fs` / `bash` / `python3` / external `$SANDBOX`. |
| `--allow-fs` | off | Register the unified `fs` tool (action=read / write / list / glob / grep / check_path / cwd / sandbox). |
| `--allow-bash` | off | Register `bash` (NOT a hardened sandbox). |
| `--no-python` | python3 on | Drop the `python3` tool. By default it's auto-registered alongside `fs` whenever `--sandbox` is set or `--allow-bash` is on. Stdlib-only interpreter; disk access auto-restricted to the sandbox root. |
| `--use-google` | off | Enable engine=`"google"` inside the unified `web` tool (needs `GOOGLE_API_KEY` + `GOOGLE_CSE_ID`). |
| `--external-tools DIR` | — | Load every `EASYAI-*.tools` manifest in `DIR`. |
| `--memory DIR` | — | Enable persistent memory: registers one `memory(action=…)` tool with sub-actions save / append / search / load / list / delete / keywords — a passive RAG technique. `--RAG` is still accepted as a back-compat alias. |
| `--preset NAME` | `precise` | Ambient sampling preset. See [Sampling presets](#sampling-presets) for what each implies. |
| `--temperature F` | per preset | Override temperature (0.0–2.0). |
| `--top-p F` | per preset | Nucleus sampling p. |
| `--top-k N` | per preset | Top-k cutoff. |
| `--min-p F` | per preset | Min-p threshold. |
| `--repeat-penalty F` | 1.04 | Repetition penalty (multiplicative on recent logits) — anti-loop safety net for thinking models that lock into rephrasing their own intent. `--repeat-penalty 1.0` disables. |
| `--frequency-penalty F` | 0.05 | Frequency penalty (additive, scales with count of each token already generated, OpenAI semantics, `[0.0, 2.0]`). Discourages verbatim repetition proportionally to how often a token has already appeared. |
| `--presence-penalty F` | 0.1 | Presence penalty (additive, fixed cost per token-already-seen, OpenAI semantics, `[-2.0, 2.0]`). Discourages topic stickiness without penalising literal tool-name repetition; pairs well with `--repeat-penalty 1.0` on long agentic flows. See [`design.md` §4b](design.md#4b-sampling-and-the-penalty-stack). |
| `--max-tokens N` | 12288 | Cap tokens per request. |
| `--seed U32` | random | RNG seed (0 = random). |
| `--max-incomplete-retries N` | 10 | Retry budget for "announce-only" turns; 0 disables. |
| `-c, --ctx N` | 262144 | Context size. |
| `--batch N` | = ctx | Logical batch size. |
| `--ngl N` | 99 | GPU layers (0 = CPU only). |
| `--split-mode, -sm MODE` | `none` | Multi-GPU split strategy: `none`, `layer`, `row`, `tensor`. |
| `--rope-scaling MODE` | `yarn` | RoPE scaling method: `none`, `linear`, `yarn`. |
| `--rope-scale F` | 2 | RoPE frequency scale factor. |
| `--yarn-orig-ctx N` | 131072 | YaRN original context size for scaling. |
| `-t, --threads N` | hw cores | CPU threads. |
| `-ctk, --cache-type-k TYPE` | `f16` | K-cache dtype (`f32`,`f16`,`bf16`,`q8_0`,`q4_0`,`q4_1`,`q5_0`,`q5_1`,`iq4_nl`). |
| `-ctv, --cache-type-v TYPE` | `f16` | V-cache dtype (same set). |
| `-nkvo, --no-kv-offload` | off | Keep KV cache on CPU even with GPU layers. |
| `--kv-unified` | off | Single unified KV buffer across sequences. |
| `--override-kv K=T:V` | — | GGUF metadata override (`int`,`float`,`bool`,`str`); repeatable. |
| `-a, --alias NAME` | `easyai` | Public model id reported by `/v1/models`. |
| `--api-key KEY` | — | Require Bearer auth on every `/v1` route. |
| `-fa, --flash-attn` | auto | Force flash attention on. |
| `-tb, --threads-batch N` | = threads | Threads for prompt-eval batches. |
| `-np, --parallel N` | 1 | Compat-only; warns when >1. |
| `--mlock` | off | mlock model weights into RAM. |
| `--no-mmap` | off | Disable mmap (read GGUF into RAM). |
| `--numa STRATEGY` | off | `distribute`,`isolate`,`numactl`,`mirror`. |
| `--metrics` | off | Expose Prometheus `/metrics`. |
| `--reasoning on\|off` | on | Enable model thinking. |
| `--no-think` | off | Strip `<think>…</think>` from replies. |
| `--inject-datetime on\|off` | on | Append authoritative date/time to system prompt. |
| `--knowledge-cutoff YYYY-MM` | `2024-10` | Cutoff hint used by `--inject-datetime`. |
| `-v, --verbose` | off | Engine logs raw model output + parser actions. |
| `--webui MODE` | `modern` | `modern` (embedded SvelteKit) or `minimal` (inline). |
| `--webui-title TEXT` | `Box EasyAI` | Browser tab + sidebar brand. |
| `--webui-icon PATH` | — | Favicon (`.ico`,`.png`,`.svg`,`.gif`,`.jpg`,`.webp`). |
| `--webui-placeholder S` | `Type a message…` | Input box placeholder. |

### `easyai-mcp-server` — standalone MCP provider (no model)

Same tool catalogue as `easyai-server` but no GGUF loaded —
designed for high-concurrency multi-client deployments. Full
reference: [`easyai-mcp-server.md`](easyai-mcp-server.md).

| Flag | Default | What it does |
|---|---|---|
| `--config PATH` | `/etc/easyai/easyai-mcp.ini` | Central INI. |
| `--host ADDR` | `127.0.0.1` | Bind address. |
| `--port N` | `8089` | TCP port. |
| `-n, --name ID` | `easyai-mcp` | Server identity on `/health` + MCP `initialize`. |
| `--max-body N` | 1 MiB | Cap on request body. |
| `-t, --threads N` | 256 | cpp-httplib worker pool. |
| `--max-concurrent-calls N` | 256 | In-flight `tools/call` cap (503 on saturation). |
| `--sandbox DIR` | cwd | Root for `fs_*` / `bash` / `$SANDBOX`. |
| `--allow-fs` | off | Register `fs_*` tools. |
| `--allow-bash` | off | Register `bash`. |
| `--no-tools` | off | Skip the built-in toolbelt entirely. |
| `--external-tools DIR` | — | Load `EASYAI-*.tools` manifests. |
| `--memory DIR` | — | Enable the unified `memory` tool (alias `--RAG`). |
| `--api-key TOK` | — | Bearer required for `/health`, `/metrics`, `/v1/tools`. |
| `--no-mcp-auth` | off | Force `/mcp` open. |
| `--metrics` | off | Enable Prometheus `/metrics`. |
| `-v, --verbose` | off | Log every dispatch to stderr. |

### `easyai-cli` — interactive remote CLI

Talks to any OpenAI-compatible endpoint (our `easyai-server`,
upstream `llama-server`, OpenAI itself, etc.).

| Flag | Default | What it does |
|---|---|---|
| `--url URL` | `$EASYAI_URL` | OpenAI-compat endpoint. |
| `--api-key KEY` | `$EASYAI_API_KEY` | Bearer auth. |
| `--model NAME` | `$EASYAI_MODEL` | Request body `model` field. |
| `--timeout SECONDS` | 86400 (24h) | Read+write timeout — sized for multi-hour agentic sessions. Only fires on TRUE silence (every SSE delta resets it). `EASYAI_TIMEOUT` env also accepted. |
| `--http-retries N` | 5 | Extra attempts on transient HTTP failures (connect refused, read timeout, 5xx). 0 disables. Logged on stderr without `--verbose`. `EASYAI_HTTP_RETRIES` env also accepted. |
| `--insecure-tls` | off | Skip peer cert check (DEV ONLY). |
| `--ca-cert PATH` | system | Custom CA bundle (PEM). |
| `--system TEXT` | — | Inline system prompt. |
| `--system-file PATH` | — | System prompt from file. |
| `--temperature F` | server | Sampling temperature. |
| `--top-p F` | server | Nucleus top-p. |
| `--top-k N` | server | Top-k cutoff. |
| `--min-p F` | server | min-p (llama-server / easyai). |
| `--repeat-penalty F` | 1.04 | Repetition penalty — anti-loop default; pass 1.0 to disable. |
| `--frequency-penalty F` | server | Frequency penalty (OpenAI standard, `[0.0, 2.0]`). |
| `--presence-penalty F` | server | Presence penalty (OpenAI standard, `[-2.0, 2.0]`). |
| `--seed N` | random | Deterministic sampling seed. |
| `--max-tokens N` | server | Cap reply length. |
| `--stop SEQ` | — | Add a stop string (repeatable). |
| `--extra-json '{…}'` | — | Free-form JSON merged into the request body. |
| `--tools LIST` | datetime,plan,web,system_* | Comma list of locally-registered tools. |
| `--sandbox DIR` | — | Enable the unified `fs` tool (action=read/write/list/glob/grep/check_path/cwd/sandbox) scoped to `DIR`. |
| `--allow-bash` | off | Register `bash` (uses `--sandbox` as cwd, else current dir). |
| `--no-python` | python3 on | Drop the auto-registered `python3` tool (default-on whenever `--sandbox` or `--allow-bash` is set). |
| `--use-google` | off | Enable engine=`"google"` inside the unified `web` tool. |
| `--external-tools DIR` | — | Load `EASYAI-*.tools` manifests. |
| `--memory DIR` | — | Enable persistent memory (one `memory(action=…)` tool; alias `--RAG`). |
| `--tools-mode MODE` | `split` | How `fs` / `web` / `memory` are exposed. Default `split` (since 2026-05-15): one focused tool per action — `fs_read`, `fs_edit`, …, `memory_save`, …, `web_search`, `web_fetch`. `unified` registers the legacy single dispatcher per family with `action=`. `both` registers both surfaces. INI: `[cli] tools_mode`. |
| `--no-plan` | off | Don't auto-register the planning tool. |
| `-p, --prompt TEXT` | (REPL) | One-shot prompt; without it you get a REPL. |
| `--no-reasoning` | shown | Hide `delta.reasoning_content`. |
| `--max-reasoning N` | 0 (off) | Abort SSE when accumulated reasoning > N chars. |
| `--no-retry-on-incomplete` | retry on | Disable auto-retry-with-nudge. |
| `--verbose` | off | Log HTTP+SSE traffic to stderr (stderr only — no file). |
| `-q, --quiet` | off | Disable spinner glyph + ctx-fill gauge. |
| `--log-file PATH` | off | Opt in to a raw transaction log at PATH (mode 0600). Implies `--verbose`. No `/tmp` file is created by default. |
| `--continue` | off | Load `.easyai_session` from cwd before the first prompt. Default OFF (since 2026-05-13): without this flag any existing session file is ignored and overwritten on the first turn. Session is always saved per turn regardless. INI: `[cli] auto_continue`. |
| `--no-continue` | — | Explicit form of the default — ignore any existing `.easyai_session` and overwrite on the first turn. Useful to override `[cli] auto_continue = on` set in INI. |
| `--compress` | off | Ask the model for a lossless recap, replace history with it, save. No-op without `--continue` (nothing in memory to recap). Also `/compress` mid-REPL. INI: `[cli] auto_compress`. |
| `--list-tools` | — | Print local tools (no chat). |
| `--list-remote-tools` | — | `GET /v1/tools` (no chat). |
| `--list-models` | — | `GET /v1/models`. |
| `--health` | — | `GET /health`. |
| `--props` | — | `GET /props`. |
| `--metrics` | — | `GET /metrics` (Prometheus text). |
| `--set-preset NAME` | — | `POST /v1/preset {preset:NAME}`. |

### `easyai-local` — local-engine REPL

Loads a GGUF model in-process (no server). For remote endpoints
use `easyai-cli`.

| Flag | Default | What it does |
|---|---|---|
| `-m, --model PATH` | (required) | GGUF file. |
| `-p, --prompt TEXT` | (REPL) | One-shot: run prompt, print, exit. |
| `-s, --system-file PATH` | — | System prompt from file. |
| `--system TEXT` | — | Inline system prompt. |
| `--preset NAME` | `precise` | Initial preset. See [Sampling presets](#sampling-presets). |
| `--no-think` | off | Strip `<think>…</think>` from output. |
| `-q, --quiet` | off | Disable spinner glyph + ctx-fill gauge. |
| `--temperature F` | per preset | Override temperature. |
| `--top-p F` | per preset | top-p. |
| `--top-k N` | per preset | top-k. |
| `--min-p F` | per preset | min-p. |
| `--repeat-penalty F` | 1.04 | Repetition penalty — anti-loop default; pass 1.0 to disable. |
| `--frequency-penalty F` | 0.05 | Frequency penalty (`[0.0, 2.0]`). |
| `--presence-penalty F` | 0.1 | Presence penalty (`[-2.0, 2.0]`). |
| `--max-tokens N` | 12288 | Cap tokens per turn. |
| `--seed U32` | random | RNG seed. |
| `-c, --ctx N` | 262144 | Context size. |
| `--batch N` | = ctx | Logical batch size. |
| `--ngl N` | 99 | GPU layers. |
| `--split-mode, -sm MODE` | `none` | Multi-GPU split strategy: `none`, `layer`, `row`, `tensor`. |
| `--rope-scaling MODE` | `yarn` | RoPE scaling method: `none`, `linear`, `yarn`. |
| `--rope-scale F` | 2 | RoPE frequency scale factor. |
| `--yarn-orig-ctx N` | 131072 | YaRN original context size for scaling. |
| `-t, --threads N` | hw cores | CPU threads. |
| `--no-tools` | off | Skip the built-in toolbelt. |
| `--sandbox DIR` | — | Enable the unified `fs` tool scoped to `DIR`. |
| `--allow-bash` | off | Register `bash`. |
| `--no-python` | python3 on | Drop the auto-registered `python3` tool. |
| `--external-tools DIR` | — | Load `EASYAI-*.tools` manifests. |
| `--memory DIR` | — | Enable persistent memory (alias `--RAG`). |
| `-ctk, --cache-type-k TYPE` | `f16` | K-cache dtype. |
| `-ctv, --cache-type-v TYPE` | `f16` | V-cache dtype. |
| `-nkvo, --no-kv-offload` | off | Keep KV cache on CPU. |
| `--kv-unified` | off | Single unified KV buffer. |
| `--override-kv K=T:V` | — | GGUF metadata override (repeatable). |

### Example apps (lib API demos)

Three small binaries under `examples/` show the lib API in
context. They take minimal flags — the real config happens in
the C++ source as fluent setter chains. Read these as the
canonical "how do I use the lib?" answer.

| Binary | Min flags | Purpose |
|---|---|---|
| `easyai-chat` | `-m PATH` OR `--url BASE`, `[--system TEXT]` | One-shot chat over Engine OR Client (auto-picks). |
| `easyai-agent` | `-m PATH`, `[-c CTX]`, `[-ngl N]` | Tiny agentic-loop demo with tool registration. |
| `easyai-recipes` | `-m PATH` | Five recipes (chat, persona, REPL, tools, agent loop). |

### Library API — `easyai::Agent`

The 30-second front door. Construct, optionally chain a few
fluent setters, call `ask()`. Header:
[`include/easyai/agent.hpp`](include/easyai/agent.hpp).

| Method | Type | Default | What it does |
|---|---|---|---|
| `Agent(model_path)` | ctor | — | Local model. |
| `Agent::remote(base_url, api_key="")` | static | — | Remote endpoint. |
| `.system(prompt)` | `string` | — | System prompt. |
| `.sandbox(dir)` | `string` | — | Enable `fs_*` scoped to `dir`. |
| `.allow_bash(on=true)` | `bool` | off | Register `bash`. |
| `.preset(name)` | `string` | `precise` | Sampling profile. |
| `.remote_model(id)` | `string` | — | Remote model id (remote mode only). |
| `.temperature(t) / .top_p(p) / .top_k(k) / .min_p(p)` | scalar | per preset | Sampling overrides. |
| `.on_token(cb)` | `function` | — | Streaming-token callback. |
| `.ask(text)` | call | — | One-shot turn; runs tool dispatch inline. |
| `.reset()` | call | — | Wipe history. |
| `.last_error()` | accessor | — | Diagnostic. |
| `.backend()` | accessor | — | Escape hatch to the underlying `Backend &`. |

### Library API — `easyai::Engine` (local llama.cpp)

Full local engine. Header:
[`include/easyai/engine.hpp`](include/easyai/engine.hpp).

| Method | Type | Default | What it does |
|---|---|---|---|
| `.model(gguf_path)` | `string` | — | GGUF file. |
| `.context(n) / .batch(n)` | `int` | 262144 / = ctx | KV / logical batch size. |
| `.gpu_layers(n)` | `int` | 99 | 99 = all layers offloaded, 0 = CPU only. |
| `.threads(n) / .threads_batch(n)` | `int` | hw / = threads | CPU threads. |
| `.seed(u32)` | `uint32_t` | random | RNG seed. |
| `.system(prompt)` | `string` | — | System prompt. |
| `.temperature(t) / .top_p(p) / .top_k(k) / .min_p(p)` | scalar | 0.2 / 0.92 / 50 / 0.03 | Sampling. |
| `.repeat_penalty(r)` | `float` | 1.04 | Repetition penalty (multiplicative on recent logits) — anti-loop default. Set to 1.0 to disable. |
| `.frequency_penalty(f)` | `float` | 0.05 | Frequency penalty (additive, scales with count, `[0.0, 2.0]`). |
| `.presence_penalty(p)` | `float` | 0.1 | Presence penalty (additive, fixed cost per token-already-seen, OpenAI semantics, range `[-2.0, 2.0]`). Pairs well with `repeat_penalty=1.0` on long agentic flows. See [`design.md` §4b](design.md#4b-sampling-and-the-penalty-stack). |
| `.max_tokens(n)` | `int` | 12288 | Per-turn cap. |
| `.tool_choice_auto / .tool_choice_required / .tool_choice_none` | call | auto | Tool-choice mode. |
| `.parallel_tool_calls(on)` | `bool` | off | Allow parallel tool calls. |
| `.verbose(on)` | `bool` | off | Engine debug logs. |
| `.max_tool_hops(n)` | `int` | 8 | Agentic-loop cap (bumped to 99999 with `bash`). |
| `.retry_on_incomplete(on)` | `bool` | on | Auto-retry "announce-only" turns. |
| `.max_incomplete_retries(n)` | `int` | 10 | Retry budget; 0 disables. |
| `.stop_at_ctx_pct(pct)` | `int` | 100 | Hard ceiling on context fill; 0 disables. |
| `.cache_type_k(name) / .cache_type_v(name)` | `string` | `f16` | KV-cache dtype. |
| `.no_kv_offload(on) / .kv_unified(on)` | `bool` | off | KV placement / layout. |
| `.add_kv_override(spec)` | `string` | — | GGUF metadata override (repeatable). |
| `.flash_attn(on) / .use_mlock(on) / .use_mmap(on)` | `bool` | auto/off/on | Compute / memory. |
| `.numa(strategy)` | `string` | off | `distribute` / `isolate` / `numactl` / `""`. |
| `.split_mode(mode)` | `string` | `none` | Multi-GPU split: `none`, `layer`, `row`, `tensor`. |
| `.rope_scaling(mode)` | `string` | `yarn` | RoPE scaling: `none`, `linear`, `yarn`. |
| `.rope_freq_scale(f)` | `float` | 2 | RoPE frequency scale factor. |
| `.yarn_orig_ctx(n)` | `int` | 131072 | YaRN original context size. |
| `.enable_thinking(on)` | `bool` | on | Chat-template thinking flag. |
| `.add_tool(t) / .clear_tools()` | call | — | Tool registration. |
| `.on_token(cb) / .on_tool(cb) / .on_hop_reset(cb) / .on_incomplete_retry(cb)` | callback | — | Streaming hooks. |
| `.load() / .reset() / .clear_kv()` | call | — | Lifecycle. |
| `.set_sampling(t,p,k,m)` | call | — | Re-sample mid-conversation. |
| `.push_message(role, content, [tool_name, tool_call_id])` | call | — | Append history without generating. |
| `.replace_history(messages)` | call | — | Full-fidelity history replay. |
| `.chat(text) / .chat_continue() / .generate_one() / .generate()` | call | — | Inference primitives. |
| `.request_cancel() / .clear_cancel() / .cancel_requested()` | call | — | Thread-safe cancel. |
| `.last_error() / .last_was_ctx_full() / .turns() / .tools() / .backend_summary() / .n_ctx() / .model_path() / .perf_data() / .perf_reset()` | accessor | — | Introspection. |

### Library API — `easyai::Client` (remote OpenAI-compat)

Remote counterpart of `Engine`. Tools execute LOCALLY in the
consumer process. Header:
[`include/easyai/client.hpp`](include/easyai/client.hpp).

| Method | Type | Default | What it does |
|---|---|---|---|
| `.endpoint(url)` | `string` | — | `http(s)://host[:port]`. |
| `.api_key(key)` | `string` | — | Bearer token. |
| `.timeout_seconds(s)` | `int` | 86400 (24h) | Connect+read timeout — sized for multi-hour agentic sessions. |
| `.http_retries(n)` | `int` | 5 | Extra attempts on transient HTTP failures (pre-stream only — never retries mid-stream). 0 disables. Each retry logs to stderr. |
| `.verbose(v)` | `bool` | off | Log SSE lines to stderr. |
| `.log_file(fp)` | `FILE*` | — | Tee every HTTP transaction. |
| `.max_reasoning_chars(n)` | `int` | 0 (off) | Abort SSE when reasoning > N chars. |
| `.retry_on_incomplete(v)` | `bool` | on | Auto-retry "announce-only" turns. |
| `.stop_at_ctx_pct(pct)` | `int` | 100 | Bail when server-reported `ctx_used/n_ctx` exceeds. |
| `.max_tool_hops(n)` | `int` | 8 | Agentic-loop cap. |
| `.tls_insecure(v) / .ca_cert_path(path)` | `bool` / `string` | off / system | HTTPS-only TLS knobs. |
| `.model(id)` | `string` | — | Request body `model` field. |
| `.system(prompt)` | `string` | — | System prompt(s). |
| `.temperature(t) / .top_p(v) / .top_k(v) / .min_p(v)` | scalar | server | Sampling. |
| `.repeat_penalty(v)` | float | 1.04 | Repetition penalty — anti-loop default; `1.0` disables. |
| `.frequency_penalty(v) / .presence_penalty(v)` | float | server | OpenAI-shape penalties. |
| `.seed(s)` | `long long` | -1 | -1 = randomise. |
| `.max_tokens(n)` | `int` | server | Cap. |
| `.stop(sequences)` | `vector<string>` | — | Stop strings. |
| `.extra_body_json(raw)` | `string` | — | Free-form JSON merged into request body. |
| `.add_tool(t) / .clear_tools() / .tools()` | call | — | Tool registration. |
| `.on_token(cb) / .on_reason(cb) / .on_tool(cb)` | callback | — | Streaming hooks. |
| `.chat(text) / .chat_continue() / .clear_history()` | call | — | Inference + history. |
| `.list_models / .list_remote_tools / .health / .metrics / .props / .set_preset` | call | — | Direct endpoint helpers. |
| `.request_cancel() / .clear_cancel() / .cancel_requested()` | call | — | Thread-safe cancel. |
| `.last_error() / .last_turn_was_incomplete() / .last_ctx_used() / .last_n_ctx() / .last_ctx_pct() / .last_was_ctx_full()` | accessor | — | Introspection. |

### Library API — `easyai::cli::Toolbelt`

Canonical agent toolset, fluently configured. Replaces the
"copy the same `if (sandbox.empty()) … else …` block five times"
pattern. Header: [`include/easyai/cli.hpp`](include/easyai/cli.hpp).

| Method | Default | What it does |
|---|---|---|
| `.sandbox(dir)` | `""` | Root for the unified `fs` tool (empty = no fs tool). |
| `.allow_fs(on)` | on | Register the unified `fs` tool (off in server unless `--allow-fs`). |
| `.allow_bash(on)` | off | Register `bash` (also bumps `max_tool_hops` to 99999). |
| `.with_plan(plan)` | — | Register the planning tool backed by a `Plan&`. |
| `.no_web(on)` | off | Drop the unified `web` tool. |
| `.no_datetime(on)` | off | Drop `datetime`. |
| `.use_google(on)` | off | Enable engine=`"google"` inside `web` (env vars required at apply-time). |
| `.tools()` | — | Materialise `vector<Tool>`. |
| `.apply(engine) / .apply(client)` | — | Register on the consumer + bump hops if bash. |

### Sampling — what each knob does

At every step the model emits a probability distribution over the whole
vocabulary (~100k+ tokens). These knobs decide how a token is picked
from it. They work in sequence: the *cutters* (`top_k`, `top_p`,
`min_p`) narrow the candidate pool over the raw distribution, then
`temperature` controls how randomly the final token is drawn from the
survivors.

* **`temperature`** — the focus-vs-risk dial; divides the logits before
  softmax. `→ 0` is greedy (always the top token: deterministic, can
  repeat). `0.2–0.5` keeps the model tight on format, syntax, and
  facts. `1.0` is the model's unmodified distribution. `> 1.0` flattens
  the curve so unlikely tokens get a real chance — more varied and
  creative, but more prone to error and incoherence. This is the main
  *behaviour* dial.
* **`top_k`** — a *fixed* cut of the tail: keep only the K
  most-probable tokens, discard the rest. Non-adaptive — it always cuts
  at K whether the model is certain or unsure. A cheap guardrail
  against ever picking junk from the long tail.
* **`top_p`** (nucleus) — an *adaptive* cut: keep the smallest set of
  top tokens whose probabilities sum to P. Adapts to confidence — when
  the model is sure (one token at 0.9) the nucleus is tiny; when it's
  unsure (mass spread wide) the nucleus is large. Cuts the tail
  proportionally.
* **`min_p`** — also adaptive, but anchored to the *top* token instead
  of cumulative mass: keep tokens with `prob ≥ min_p × prob_of_top`.
  `min_p 0.1` keeps anything within 10× of the best; `min_p 0.5` keeps
  only what's within 2× — aggressive, very focused output.

**How they interact.** They stack. Tightening all of them at once (low
`top_k` + low `top_p` + low `temperature`) is redundant — they do the
same job and you over-constrain into robotic output. Practical rule:
pick *one* adaptive cutter (`top_p ~0.9–0.95` **or** `min_p ~0.05–0.1`),
leave `top_k` generous as a cheap backstop, and use `temperature` as
the real behaviour dial.

**How to tune.**
* *Code, agentic / tool-calling, structured output, factual Q&A* — low
  `temperature` (0.2–0.6) and a tight tail cut. High temperature on
  code means syntax errors, hallucinated APIs, broken tool calls.
* *Creative writing, brainstorming* — higher `temperature` (0.8–1.2),
  looser cutters.
* *Heavily quantised models* — be more conservative (lower
  `temperature`, tighter cut). Quantisation already adds noise to the
  logits; high temperature amplifies that noise into real errors.

The presets below are just curated combinations of these four knobs —
e.g. `precise` (the project default) encodes `temp 0.2, top_p 0.92,
top_k 50, min_p 0.03`.

### Sampling presets

Named profiles applied via `--preset NAME` (binaries) or
`Engine::set_sampling()` / `easyai::find_preset()` (lib). Numbers are
baselines; `<preset> <number>` overrides temperature only. The
project-wide **default is `precise`** — tuned for code, math, and
factual Q&A, the dominant use case for a tool-calling agent. Override
when you need looser sampling.

| Name | temp | top_p | top_k | min_p | Behaviour | Pick when… |
|---|---|---|---|---|---|---|
| `deterministic` | 0.0 | 1.0 | 1 | 0.00 | Greedy: always picks the single most likely token. Same prompt → byte-identical answer every time. No randomness, no exploration. | You need reproducibility (CI, benchmarks, eval harnesses), or when even tiny variation breaks downstream parsing. |
| `precise` (default) | 0.2 | 0.92 | 50 | 0.03 | Sticks to high-confidence tokens. Concise, follows instructions tightly, rarely contradicts itself or invents facts. Slightly wider candidate pool than before (top_k 50, min_p 0.03) for more natural phrasing while staying deterministic enough for stable tool calls. | Code generation, math, factual Q&A, the `memory` tool, tool-calling agents, structured output (JSON/SQL/cypher), anything you'd want to be "right" rather than "interesting". |
| `balanced` | 0.7 | 0.92 | 50 | 0.03 | A bit of variety while still mostly committing to the most-likely answer. Phrasing varies between runs; the substance shouldn't. | General-purpose chat, summarisation, casual Q&A, anywhere you want natural-sounding prose without surprises. |
| `creative` | 1.0 | 0.92 | 50 | 0.03 | More phrasing variety, occasional surprising word choices, willingness to take a less-obvious angle. | Brainstorming, fiction, marketing copy, ideation, anything where "interesting" beats "literal". |
| `wild` | 1.4 | 0.98 | 60 | 0.00 | Maximum entropy. Frequently picks low-probability tokens; can wander off-topic, contradict itself, hallucinate. | Pure exploration, "show me something I wouldn't have thought of", stylistic experiments. Don't ship it. |

Aliases (case-insensitive) recognised by `find_preset()`:
`exact`→`precise`, `default`→`balanced`, `fun`→`creative`,
`chaos`→`wild`, `greedy`→`deterministic`.

Switching at runtime — three paths, same effect:

```bash
# CLI flag (start or restart)
easyai-server --preset creative
easyai-local  --preset balanced

# Server endpoint (live, no restart)
curl -s -X POST http://localhost:8080/v1/preset \
     -H 'Content-Type: application/json' \
     -d '{"preset":"creative"}'

# easyai-cli helper
easyai-cli --url $URL --set-preset creative
```

The webui's preset bar uses the same endpoint — clicking a button
shifts every subsequent request server-wide. INI form for persistence
is `[ENGINE] preset = precise` (see
[`easyai-server.md`](easyai-server.md) §1).

Header: [`include/easyai/presets.hpp`](include/easyai/presets.hpp).

---

## Why try it

**Your assistant. Your tools. Your hardware. No cloud subscription,
no API bill, no data leaving the box.**

* **Runs on a Raspberry Pi.** Bonsai 8B Q1_0 weighs in at ~1.2 GB
  resident. A Pi 4 (8 GB) or any Pi 5 holds it with a 4 K context
  comfortably — and one install script puts a chat server at
  `http://pi-ai.local` for everyone on your home network.

* **Runs on your Mac.** Same one-script flow, Metal on Apple
  Silicon, full webui at `http://localhost:8080`. No Docker, no
  Conda, no Python venv. Uninstall is `rm -rf` of the checkout.

* **Plugs into the AI apps you already use.** OpenAI-compatible
  (`/v1/chat/completions`) — Claude Code, the OpenAI SDK,
  LiteLLM, LangChain, LobeChat, OpenWebUI all point at it without
  any easyai-specific configuration. Ollama-compat shims
  (`/api/tags`, `/api/show`) cover clients that prefer that shape.

* **Speaks MCP.** Claude Desktop, Cursor, Continue and any other
  Model Context Protocol client auto-discovers the tool catalogue.
  **Write one tool — every AI app on your machine can call it.**

* **Long-term memory built in.** The `memory` tool: one
  `memory(action=...)` tool (sub-actions save / append / search /
  load / list / delete / keywords) the agent uses to save, append
  (grow what you already know about the user without losing the
  previous body), search, load, list, delete, and inventory its own
  knowledge. It's a passive RAG technique — one human-readable
  Markdown file per entry, `cat`, `vim`, `grep` it. No vector DB to
  babysit.

* **Operator-defined tool packs.** Drop a JSON manifest in
  `/etc/easyai/external-tools/`, the agent picks it up at startup.
  Give the model exactly the powers it needs (a database probe, a
  deploy command, a metrics query) without ever flipping
  `--allow-bash`.

* **Safe defaults.** No filesystem, no shell, no writes — until
  you opt in. Every privileged opt-in is logged at startup with
  sanity warnings (shell wrappers, world-writable binaries,
  dynamic-linker env passthrough). Three rounds of security
  audits in [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md).

* **A C++17 framework, not a wrapper.** Three lines wrap
  llama.cpp into a real agent. Fluent builder for tools, full
  sampling control, streaming callbacks, plan tool, named
  sampling presets. Link `libeasyai`, ship one binary.

* **Ops-ready.** Prometheus `/metrics`, Bearer auth, systemd unit
  with `mlock` + `LimitMEMLOCK=infinity`, flash-attn, KV-cache
  quantisation (`q8_0` / `q4_0` / `iq4_nl`), per-request body
  cap, slow-loris timeouts. The Linux installer handles the whole
  Debian/Ubuntu deploy in one command.

### Get going in 60 seconds

```sh
# Raspberry Pi 4 / Pi 5 (Pi OS 64-bit) — your LAN's AI appliance:
git clone https://github.com/solariun/easy && cd easy
sudo ./scripts/install_easyai_pi.sh
# → http://pi-ai.local on every device on your network

# Mac (Apple Silicon or Intel):
git clone https://github.com/solariun/easy && cd easy
./scripts/install_easyai_macos.sh
# → http://localhost:8080

# Linux server (Debian / Ubuntu):
git clone https://github.com/solariun/easy && cd easy
sudo ./scripts/install_easyai_server.sh --model /path/to/your.gguf
# → http://0.0.0.0:80 with full systemd + auth + Prometheus metrics
```

Then open the URL in any browser, or point your favourite OpenAI
client at the same address. That's it.

---

## At a glance

The pitch in three lines:

```cpp
#include "easyai/easyai.hpp"

int main() {
    easyai::Agent a("models/qwen2.5-1.5b-instruct.gguf");
    std::cout << a.ask("What time is it in Tokyo right now?") << "\n";
}
```

That's the whole thing.  Construct an `Agent`, ask, print.  Default
toolset (datetime + the unified `web` tool) is already wired in;
the `fs` tool and `bash` stay off until you opt in.  Remote endpoints
work the same way:

```cpp
auto a = easyai::Agent::remote("http://127.0.0.1:8080/v1");
a.system("Be terse.")
 .on_token([](auto p){ std::cout << p << std::flush; });
a.ask("Summarise this commit.");
```

When you outgrow the 3-line shape, the same library exposes every
layer below — Tier 2 fluent builders (`Toolbelt`, `Streaming`),
Tier 3 explicit composables (`Engine`, `Client`, `Backend`,
`Tool::builder`), Tier 4 raw escape hatches (`Agent::backend()`,
llama.cpp handles).  Higher tiers are implemented on top of lower
ones — no parallel codepaths — so Tier 1 stays trustworthy as the
project evolves.

```cpp
// Tier 2 example: wire the canonical toolset onto an Engine in
// three fluent lines instead of seven add_tool calls.
easyai::Engine engine;
engine.model("models/qwen2.5-1.5b-instruct.gguf").gpu_layers(99).context(4096);

easyai::cli::Toolbelt()
    .sandbox   ("/srv/data")    // enables the unified `fs` tool (read/write/list/glob/grep)
    .allow_bash()                // enables bash + bumps max_tool_hops to 99999
    .apply     (engine);

engine.load();
engine.chat("Find all .md files larger than 1 KB and summarise them.");
```

`Engine::chat()` runs the **full** tool-call/tool-result loop for you — up to
8 hops by default (lift the cap with `engine.max_tool_hops(N)` for shell-driven
flows, or just register `bash` and the helpers do it for you).

Tool definitions are 6 lines:

```cpp
engine.add_tool(
    easyai::Tool::builder("flip_coin")
        .describe("Returns 'heads' or 'tails' uniformly at random.")
        .handle([](const easyai::ToolCall &) {
            return easyai::ToolResult::ok((std::rand() & 1) ? "heads" : "tails");
        })
        .build());
```

---

## What's in the box

### Library (`libeasyai`, link target `easyai::engine`)

* `easyai::Engine` — high-level wrapper around llama.cpp's model + context +
  sampler + chat templates. Fluent setters, RAII-owned native resources.
* `easyai::Tool` — name + description + JSON-schema params + handler. Builder
  API generates the schema for you.
* `easyai::Plan` — agent-friendly checklist with one multi-action tool
  (add / start / done / list).  Pluggable into `Engine` or `Client`; fires a
  callback on every mutation so you can render live.
* `easyai::tools::*` — built-in tools:
  * `datetime` (no deps)
  * `web` — unified search + fetch (`action="search"` / `"fetch"`).
    Search engine selectable: `"auto"` (default; cascades google →
    brave → ddg-lite → bing → ddg, returning the first that succeeds
    — Brave carries the keyless niche-query case since Bing RSS
    ignores quoted phrases and rare named entities, DDG Lite picks up
    when Brave's burst budget is gone, and Bing carries the keyless
    workhorse case for ordinary queries), or pin one explicitly:
    `"google"` (Google Custom Search JSON API, opt-in via the
    `google_enabled` ctor flag and the GOOGLE_API_KEY + GOOGLE_CSE_ID
    env vars), `"brave"` (Brave HTML scrape, keyless), `"ddg-lite"`
    (DuckDuckGo Lite endpoint with a Netscape Communicator 4.79 UA,
    keyless, page 1 only), `"bing"` (Bing RSS feed, keyless),
    `"ddg"` (DuckDuckGo HTML scrape, keyless). Page-based pagination
    on search; byte-window pagination on fetch. libcurl required at
    build time.
  * `fs` — unified filesystem (`action="read"` / `"write"` / `"list"`
    / `"glob"` / `"grep"` / `"check_path"` / `"cwd"` / `"sandbox"`),
    sandboxed to a root directory you provide; the model sees a
    virtual `/`-rooted filesystem (real sandbox path is hidden).
  * `bash` — shell command runner. `/bin/sh -c`, cwd pinned to the
    sandbox root, stdout/stderr merged + capped, configurable timeout.
    Honest about what it is: NOT a hardened sandbox — runs with your
    user privileges. Opt-in.
* `easyai::presets` — named sampling profiles
  (`deterministic / precise / balanced / creative / wild`) plus a tiny parser
  that turns chat lines like `"creative 0.9"` or `"/temp 0.5"` into sampling
  overrides.
* `easyai::ui` — terminal UI helpers (`Style`, `Spinner`, `StreamStats`).
  Auto-detect TTY, honour `NO_COLOR`, heartbeat-driven spinner so the
  glyph keeps animating during long tool calls.
* `easyai::text` — small string helpers (`punctuate_think_tags`,
  `slurp_file`, `prompt_wants_file_write` heuristic).
* `easyai::log` — `set_file(FILE*)` + `write(fmt, ...)`: tee diagnostic
  output to stderr **and** an optional log file.
* `easyai::cli` — CLI infrastructure:
  * `Toolbelt` — fluent builder that registers the canonical agent
    toolset on an `Engine` or `Client` and bumps `max_tool_hops` to
    99999 when bash is enabled.
  * `open_log_tee / close_log_tee` — open `/tmp/<prefix>-<pid>-<epoch>.log`
    with header, register as the global log sink.
  * `validate_sandbox(path, &err)` — uniform "exists? is a dir?" check.
  * `client_has_tool(client, name)`, `print_models / print_local_tools /
    print_remote_tools / print_health / print_props / print_metrics /
    set_preset` — management subcommand helpers that drive an
    `easyai-server` from a one-line dispatcher.
* `easyai::Backend` (+ `LocalBackend`, `RemoteBackend`) — common
  interface for "give me a model, local or remote, with the same
  chat/reset/set_system shape".  Linking only `easyai::engine` gets
  you LocalBackend; adding `easyai::cli` adds RemoteBackend without
  duplicating the abstraction.
* `easyai::Agent` — the friendly Tier-1 façade over Backend.  3-line
  hello-world, fluent setters for system/sandbox/allow_bash/preset,
  and `backend()` as the escape hatch back to Tier 3 power.

### Library (`libeasyai-cli`, link target `easyai::cli`)

* `easyai::Client` — same fluent API shape as `Engine`, but the model runs on
  a remote `/v1/chat/completions` endpoint and **tools execute locally**.
  Configures HTTP transport (`endpoint`, `api_key`, `timeout_seconds`,
  `verbose`) plus the full sampling/penalty surface (`temperature`, `top_p`,
  `top_k`, `min_p`, `repeat_penalty`, `frequency_penalty`, `presence_penalty`,
  `seed`, `max_tokens`, `stop(vector)`, `extra_body_json`).
  Engine-side also exposes `split_mode`, `rope_scaling`, `rope_freq_scale`,
  `yarn_orig_ctx` for multi-GPU and context-extension tuning.  Streaming
  callbacks (`on_token`, `on_reason`, `on_tool`) and an agentic multi-hop
  loop mirror `Engine::chat_continue` semantics.
* Direct-endpoint helpers — `list_models`, `list_remote_tools`, `health`,
  `metrics`, `props`, `set_preset` — let downstream apps script and
  introspect an `easyai-server` without ever touching curl.

### Binaries

#### Tool gating across all three CLIs

All three example CLIs (`easyai-local`, `easyai-cli`, `easyai-server`)
follow the same gating model. Default is **safe**: no filesystem access,
no shell.

| Flag                  | What it enables                                                         |
|-----------------------|-------------------------------------------------------------------------|
| (no flag)             | `datetime` and the unified `web` tool (action=`search` / `fetch`) only. |
| `--sandbox <dir>`     | The unified `fs` tool (action=`read` / `write` / `list` / `glob` / `grep` / `check_path` / `cwd` / `sandbox`), all scoped to `<dir>`. The CLIs `chdir` into `<dir>` so `fs(action="cwd")` reports the sandbox path back to the model. |
| `--allow-bash`        | `bash` (run `/bin/sh -c`). cwd = `--sandbox <dir>` if given, otherwise the binary's CWD. NOT a hardened sandbox — runs with your user privileges. Also bumps the agentic-loop `max_tool_hops` to 99999 (bash flows naturally span many turns). |
| `--use-google`        | Enables `engine="google"` inside the unified `web` tool (Google Custom Search JSON API), and lets the default `engine="auto"` cascade try google as its first hop. Requires `GOOGLE_API_KEY` and `GOOGLE_CSE_ID` env vars. Counts against your Google quota — free tier is 100 queries/day per key. Without this flag (or without the env vars), the auto cascade silently skips google and falls through to brave → ddg-lite → bing → ddg. |
| `--external-tools <dir>` | Load every `EASYAI-<name>.tools` file in `<dir>` as an operator-defined tool pack. Per-file fault isolation (a bad file is logged + skipped, the agent still starts). Spawns via `fork`+`execve` — never a shell. **This is the supported way to give the model focused powers without flipping `--allow-bash`.** See [`EXTERNAL_TOOLS.md`](EXTERNAL_TOOLS.md). |
| `--memory <dir>`      | Enable the agent's persistent **memory** (search / store / append / recall / update / forget) — a passive RAG technique over keyword-indexed Markdown files. Registers ONE `memory(action=...)` tool with sub-actions `save`, `append` (grow an existing memory without losing its body), `search`, `load`, `list`, `delete`, `keywords` — each memory one Markdown file in `<dir>`. Memories whose title starts with `fix-easyai-` are immutable: pass `fix=true` (sub-action `save`) to mint one. `--RAG` is still accepted as a back-compat alias. The systemd-installed server passes this by default (`/var/lib/easyai/rag`). See [`RAG.md`](RAG.md). |
| `--mcp <url>`         | Connect to a remote MCP server as a CLIENT (e.g. another `easyai-server` or `easyai-mcp-server`). The upstream's tool catalogue is fetched via `tools/list` and merged into the local one; each remote tool's handler proxies `tools/call` back to it. Local tool names win on collision (remote dup skipped with a warning). Pair with `--mcp-token <token>` when the upstream requires bearer auth. |
| `--no-local-tools`    | Skip the LOCAL built-in toolbelt entirely (datetime, web, fs, bash, ...). Useful when you want ONLY external tools, ONLY the `memory` tool, or ONLY tools fetched via `--mcp`. Does NOT disable the MCP client — that's controlled by `--mcp`. **Renamed from `--no-tools`.** |

#### Single config file: `/etc/easyai/easyai.ini`

The systemd-installed server reads every operator-tunable knob —
host, port, alias, sandbox, memory dir, KV cache types, mlock, flash-attn,
threads, MCP auth, the works — from one INI file. **CLI flags on the
unit override INI values; INI overrides hardcoded defaults.** So
tweak the file + restart, no `systemctl edit` cadence:

```ini
[SERVER]
host       = 0.0.0.0
port       = 80
alias      = EasyAi
mcp_auth   = on              ; require Bearer on /mcp

[ENGINE]
ngl        = 99              ; offload all layers to GPU
flash_attn = on
mlock      = on
ctx_size   = 262144
cache_type_k = q8_0
cache_type_v = q8_0
frequency_penalty = 0.05
split_mode     = none         ; none | layer | row | tensor
rope_scaling   = yarn         ; none | linear | yarn
rope_freq_scale = 2
yarn_orig_ctx  = 131072

[MODEL_deepseek]              ; per-model profile — matches any GGUF
temperature = 0.15            ; whose resolved filename contains
top_k       = 30              ; "deepseek" (case-insensitive)

[MODEL_qwen]
temperature = 0.25
repeat_penalty = 1.08

[MCP_USER]
gustavo    = REPLACE-WITH-OPENSSL-RAND-HEX-32
```

`[MODEL_<pattern>]` sections let you override any `[ENGINE]` key for
specific models. The `<pattern>` is matched case-insensitively as a
substring against the resolved model filename; when multiple sections
match, the longest pattern wins. Precedence: CLI > MODEL_ > ENGINE >
hardcoded default.

Full key reference + worked examples: [`easyai-server.md`](easyai-server.md) §1.

#### easyai-server speaks **MCP** — every tool also reachable from Claude Desktop / Cursor / Continue

`easyai-server` exposes its full tool catalogue (built-ins + the `memory` tool + every operator-defined `--external-tools` pack) via the **Model Context Protocol** at `POST /mcp`. Other AI applications connect, list, and dispatch:

```
Claude Desktop ──► [stdio bridge] ──► POST /mcp ──┐
Cursor          ─────────────────────► POST /mcp ──┤── easyai-server
Continue        ─────────────────────► POST /mcp ──┘   (one tool catalogue,
                                                        many consumers)
```

You build the tools once. Your `memory` tool, your deploy CLI, your monitoring queries — written ONCE for your easyai-server — become available in every AI app you already use. No plugin per app.

```sh
# List tools the server is exposing right now
curl -fsS http://localhost/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | jq '.result.tools[] | .name'
```

It also speaks the **OpenAI** (`/v1/chat/completions`, `/v1/models`) and **Ollama** (`/api/tags`, `/api/show`) list-models APIs so OpenAI-SDK, LangChain, LiteLLM, LobeChat, OpenWebUI, etc. auto-discover the loaded model and chat without any easyai-specific configuration.

Full guide: [`MCP.md`](MCP.md). Bridge script for Claude Desktop: `scripts/mcp-stdio-bridge.py`.

#### Why `--memory` makes the agent useful

Without long-term memory, every session starts from zero: the model
re-derives your preferences, re-learns your project, re-asks the same
questions. With `--memory`, the model decides what's worth remembering
and writes it to a directory of small Markdown files — a passive RAG
technique, no embedding model or vector store. Next session, it
searches by keyword, finds what its past self saved, and picks up
where you left off.

```
> "I prefer terse responses in PT-BR."
[model: memory(action="save", title="user-prefs", keywords=["user","prefs","locale"], content="...")]

[next session]
> "build easyai on the AI box"
[model: memory(action="search", keywords=["easyai"]) → finds your saved build recipe]
[model loads it and answers in your style]
```

The dir is at `/var/lib/easyai/rag/` on the installed server. You
can `cat`, `vim`, `grep`, hand-author entries, back it up with `tar`
— it's a directory of plain text files. The model is the curator;
you, the operator, can read and edit anything it decided to keep.
(`--RAG` still works as a back-compat alias for `--memory`.)

Future evolution (see `RAG.md`): progressive recall on session start,
automatic document ingestion, per-user namespaces. The on-disk format
won't change.

#### Why `--external-tools` is the answer to "give the model more power"

Most agent frameworks force a binary choice: either you ship the model
with the tools the framework's authors thought of, or you give it a
generic shell. The framework's authors don't know about your internal
deploy CLI, your jq wrappers, your monitoring queries — and a generic
shell is a structurally unsafe surface no matter how careful you are.

easyai's `--external-tools` is the missing third option. Drop a JSON
file in the configured directory:

```json
{
  "version": 1,
  "tools": [
    {
      "name": "deploy_status",
      "description": "Status of one of our services in the control plane.",
      "command": "/opt/internal/bin/deploy-cli",
      "argv": ["status", "--", "{service}"],
      "parameters": {
        "type": "object",
        "properties": { "service": {"type":"string"} },
        "required": ["service"]
      },
      "timeout_ms": 10000,
      "max_output_bytes": 32768,
      "cwd": "$SANDBOX",
      "env_passthrough": ["DEPLOY_TOKEN"]
    }
  ]
}
```

Restart the server. The model can now ask for `deploy_status(service:"billing-api")`. The framework guarantees:

- **No shell.** `fork` + `execve` directly. The model's argument fills exactly one argv slot — `; rm -rf /` cannot escape it.
- **No PATH-hijack.** Absolute command paths are mandatory and validated at load.
- **No quoting bugs.** Whole-element placeholders only; `--flag={x}` is rejected at load (split into `["--flag","{x}"]`).
- **Schema-validated arguments.** Type errors rejected before `fork()`.
- **Bounded resources.** Timeout, output size, env-var inheritance, fd inheritance — every channel capped.
- **Per-file fault isolation.** A typo in `EASYAI-experimental.tools` doesn't prevent `EASYAI-system.tools` from loading.
- **Operator/user collaboration.** Drop additional `EASYAI-*.tools` files in the dir and they appear after a restart. Different teams can own different files. `chmod o-w` enforced at the directory level.
- **Sanity-check warnings at load.** Wrap a shell? Let the model influence `LD_PRELOAD`? Manifest world-writable? You'll see it in the startup log.

The default install creates `/etc/easyai/external-tools/` empty — drop your first `.tools` file in and you're live. Full guide and ten worked recipes in [`EXTERNAL_TOOLS.md`](EXTERNAL_TOOLS.md).

#### `easyai-local`

```
easyai-local -m model.gguf [-s system.txt] [--ngl 99] [--no-tools]
              [--sandbox DIR] [--allow-bash]
```

Local-only REPL.  Type any line to talk; type any of these to control the engine:

| Command                | Effect                                                                |
|------------------------|-----------------------------------------------------------------------|
| `precise`              | Switch to the `precise` preset                                        |
| `creative 0.9`         | Switch to `creative`, override temperature to 0.9                     |
| `/temp 0.5`            | Set temperature only                                                  |
| `/system <text>`       | Replace system prompt and clear history                               |
| `/reset`               | Clear conversation history                                            |
| `/tools`               | List currently-registered tools                                       |
| `/help`                | Show all presets                                                      |
| `/quit`                | Leave                                                                 |

Loads a `system.txt` if you pass `-s`; this is the *server-default* system
prompt (in the CLI's case, just the system prompt for that REPL session).

#### `easyai-server`

```
easyai-server -m model.gguf [-s system.txt] [--port 8080] [--ngl 99]
              [--sandbox DIR] [--allow-bash]
```

OpenAI-compatible HTTP server. Endpoints:

| Verb | Path                       | Notes                                                                                         |
|------|----------------------------|-----------------------------------------------------------------------------------------------|
| GET  | `/`                        | Embedded single-file webui (chat + preset bar)                                                |
| GET  | `/health`                  | JSON status (model, backend, tool count, ambient preset)                                      |
| GET  | `/v1/models`               | Lists the loaded model in OpenAI format                                                       |
| POST | `/v1/chat/completions`     | OpenAI-shape request, including optional `tools`, `temperature`, `top_p`, `top_k` overrides   |
| POST | `/v1/preset`               | `{"preset":"creative"}` — change the ambient preset for the webui                             |

**The killer feature** — when a *client* (Claude Code, an OpenAI SDK, LiteLLM,
LangChain…) posts its **own** `system` message and/or **own** `tools` to
`/v1/chat/completions`, those win for that single request:

* Client provides `tools` → easyai forwards generated tool calls back to the
  client and *does not* dispatch them locally. The client controls the loop.
* Client provides no `tools` → easyai uses its own toolbelt and runs the
  multi-hop loop server-side, returning the final assistant message.

Either way the server-supplied `system.txt` is used **only** when the request
doesn't already include a `system` message.

This makes `easyai-server` look like a real OpenAI-compatible backend to any
client that expects one.

---

## Meet Deep — the default assistant persona

A fresh `easyai-server` boots up as **Deep** — an expert system
engineer who answers from CHECKED FACTS, not impressions.  Built into
the default system prompt so a small open-weights model behaves like
an engineer instead of a chatbot from minute one.

Deep's operating loop is: **TIME → THINK → PLAN → EXECUTE → VERIFY**.

- **Time first.** Any question that touches "now", "today", a
  deadline, a release version, or a fact that could have changed
  since training cutoff → `datetime` is the first tool call.  Anchors
  the rest of the turn to the real wall clock.
- **Think.** State the goal, identify what's known vs. needs lookup,
  what could go wrong.
- **Plan.** Multi-step tasks call `plan(action='add', text=…)` first
  so the user can see and intervene live.  The model uses
  `plan(action='update', id=…, status='working'|'done'|'error')`
  to advance steps and `action='delete'` to retire abandoned ones
  (rendered struck through, not removed). Statuses:
  `pending | working | done | error | deleted`. Batch via the
  `items` array (max 20).
- **Execute.** Every registered tool is fair game.
- **Verify.** Before claiming success — does the file exist? does
  the test pass? does the URL really say that?  When in doubt, run
  another tool instead of guessing.

Old behaviour rules carry over: `RULE 1` (execute or answer, never
just announce), `web(action="search") → web(action="fetch")`
mandatory, citations stick to the URL actually fetched.

Operators who want a different persona pass `--system "<text>"` or
`-s persona.txt` — Deep is the default, not a hardcoded identity.

---

## Quick start

```
develop/
├── easyai/        # this project
└── llama.cpp/     # cloned next to it (https://github.com/ggml-org/llama.cpp)
```

```bash
cd easyai
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # see "Build for your hardware" below
cmake --build build -j

# Local REPL with everything wired up
./build/easyai-local -m models/qwen2.5-1.5b-instruct-q4_k_m.gguf

# Agentic REPL talking to a remote OpenAI-compatible endpoint
./build/easyai-cli --url http://127.0.0.1:8080
./build/easyai-cli --url https://api.openai.com/v1 \
                   --api-key $OPENAI_API_KEY --model gpt-4o-mini

# One-shot mode (great in scripts — banners on stderr, model text on stdout)
./build/easyai-local -m models/qwen2.5-1.5b-instruct-q4_k_m.gguf -p "What is 2+2?"
result=$(./build/easyai-cli --url http://127.0.0.1:8080 --no-reasoning -p "summarise this commit")

# Open http://127.0.0.1:8080 in a browser
./build/easyai-server -m models/qwen2.5-1.5b-instruct-q4_k_m.gguf
```

Point any OpenAI client at it:

```bash
curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"easyai","messages":[{"role":"user","content":"Hi!"}]}'
```

For Claude Code (or any tool that takes an OpenAI-compatible base URL), set
`http://127.0.0.1:8080/v1` as the base. Any tools the client declares will
be forwarded; any tools it doesn't declare will use the server's toolbelt.

### Selective builds — only what you need

Every target is independent.  Configure once, then build whichever
subset matters for your situation:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release           # configure once

# Just the engine library (libeasyai.so + headers):
cmake --build build -j --target easyai

# Just the OpenAI-protocol client library (libeasyai-cli.so):
cmake --build build -j --target easyai_cli

# Just the agentic remote CLI (links libeasyai-cli):
cmake --build build -j --target easyai-cli

# Just the local-only REPL (links libeasyai):
cmake --build build -j --target easyai-local

# Just the server:
cmake --build build -j --target easyai-server

# Drop the examples entirely (lib-only consumers):
cmake -S . -B build -DEASYAI_BUILD_EXAMPLES=OFF
cmake --build build -j

# Drop the embedded webui from easyai-server (smaller binary):
cmake -S . -B build -DEASYAI_BUILD_WEBUI=OFF
cmake --build build -j

# Drop libcurl-using tools (the `web` tool's search and fetch actions):
cmake -S . -B build -DEASYAI_WITH_CURL=OFF
cmake --build build -j

# Clean rebuild from scratch:
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Just delete object files but keep configuration:
cmake --build build --target clean
```

After `cmake --install build --prefix /usr/local`, downstream projects
can `find_package(easyai 0.1 REQUIRED)`:

```cmake
# Your CMakeLists.txt:
find_package(easyai 0.1 REQUIRED)
add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE
    easyai::engine    # libeasyai.so — local llama.cpp wrapper
    easyai::cli       # libeasyai-cli.so — OpenAI-protocol client
)
```

Both targets export their public include directory and `cxx_std_17`
feature, so consumers don't need any extra include flags.

### Build for your hardware

Pick the matching configure command for your machine; rebuild with
`cmake --build build -j`.

| Hardware                                  | Configure command                                                                  | Notes                                                                                |
|-------------------------------------------|------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------|
| **Apple Silicon / Intel Mac (Metal)**     | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`                                   | Metal is auto-detected on macOS — nothing extra to set.                              |
| **NVIDIA GPU (CUDA)**                     | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON`                    | Needs the CUDA Toolkit (`nvcc`). Optionally pin GPU arch with `-DCMAKE_CUDA_ARCHITECTURES=89`. |
| **AMD / Intel / cross-vendor (Vulkan)**   | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_VULKAN=ON`                  | Needs the Vulkan SDK on Linux/Windows. Works on AMD RX/Pro, Intel Arc, NVIDIA too.   |
| **AMD on Linux (ROCm/HIP)**               | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_HIP=ON -DAMDGPU_TARGETS=gfx1100` | Replace the gfx ID with your card's. Requires ROCm 6+.                                |
| **CPU-only (any OS)**                     | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`                                   | Then run with `-ngl 0` (CLI/server) or `.gpu_layers(0)` (lib).                       |

Add `-DGGML_OPENBLAS=ON` (Linux) or `-DGGML_BLAS=ON` (macOS uses Accelerate
automatically) for a faster CPU prompt-eval path.

If both Metal and CUDA libraries are present (rare), keep one and disable
the other explicitly with `-DGGML_METAL=OFF` / `-DGGML_CUDA=OFF`.

### Web search

`web(action="search")` with the default `engine="auto"` cascades through
five backends and returns the first one that succeeds:

1. **Google CSE** — only if `--use-google` is passed AND `GOOGLE_API_KEY`
   + `GOOGLE_CSE_ID` are set; if any are missing this hop is silently
   skipped (not a failure).
2. **Brave HTML** — `search.brave.com/search?q=…`. Keyless HTML SSR,
   ~20 results per page. The keyless engine that best understands the
   full query — unlike Bing RSS (which strips quoted phrases and rare
   named entities, returning Wikipedia about Santiago de Compostela
   for `"Santiago Cavalcante" PNUD`), Brave honours the whole query.
   Downside: throttles single IPs aggressively (HTTP 429 after a
   small burst), and its Svelte CSS classes rotate between deploys
   (the scraper anchors on stable hooks, so hash rotation alone
   won't break it; a structural markup rewrite will).
3. **DDG Lite** — `lite.duckduckgo.com/lite/?q=…` accessed with a
   Netscape Communicator 4.79 (Windows NT 5.0) User-Agent. Keyless,
   ~10 results per query, **page 1 only**. The Netscape UA matters:
   DDG Lite is the no-JS endpoint maintained for old browsers and
   accessibility, so DDG serves it without the anti-bot challenge
   when the UA obviously can't run JS. Result quality is comparable
   to Brave for entity queries (returns the actual LinkedIn / Google
   Scholar / Brazilian profile hits for `"Santiago Cavalcante" PNUD`)
   and isn't rate-limited the way Brave is — so it's the workhorse
   when Brave's burst budget runs out.
4. **Bing RSS** — `www.bing.com/search?q=…&format=rss`. Keyless,
   captcha-free XML feed maintained for legitimate feed consumers.
   Caps at ~10 results per query and ignores pagination, but stable
   and fast for ordinary keyword queries.
5. **DuckDuckGo HTML scrape** — `html.duckduckgo.com/html/`. Keyless,
   the historical default, kept as last resort because DDG's anti-bot
   heuristics now return an "anomaly" page (HTTP 202, no results) for
   most server IPs (the modern endpoint is gated even though the Lite
   endpoint isn't).

Pin a specific backend with `engine="google"` / `"brave"` /
`"ddg-lite"` / `"bing"` / `"ddg"` when you want to bypass the cascade
(useful for diagnosis: "does ddg still work from this box?"). The
output's `engine: <name>` header line tells the model which backend
actually answered.

---

## Documentation

* [`manual.md`](manual.md) — hands-on developer manual.  Includes a
  step-by-step **"Recipe book — write your first tools"** chapter
  (section 3.8) that walks through `examples/recipes.cpp` line by
  line in a friendly, accessible style.  Best place to start if you
  want to extend easyai with your own services.
* [`design.md`](design.md) — architecture, data flow, why we build on top of
  `common/` instead of just `include/llama.h`.
* [`scripts/install_easyai_server.sh`](scripts/install_easyai_server.sh) —
  one-shot Debian/Ubuntu installer; **drop-in replacement** for the
  `install_llama_server.sh` workflow. Clones llama.cpp + easyai, builds
  with the right backend (auto-detects Vulkan / CUDA / ROCm / CPU),
  creates a system user + `/var/lib/easyai`, drops a hardened systemd
  unit with mlock + flash-attn + q8_0 KV cache + Bearer auth +
  Prometheus `/metrics`. Accepts every flag the original took
  (`--with-mcp`, `--draft-model`, `--webui-title`, etc.) — built-in
  features become no-ops with a friendly warning so existing automation
  keeps working.

---

## Memory hygiene

Every native resource is owned by a smart pointer or a value type with a
custom destructor:

* `Engine` — `std::unique_ptr<Impl>` pImpl pattern. The `Impl` destructor
  frees the sampler explicitly; the model, context, and chat-templates are
  unique-pointer-owned.
* `easyai-server` — single `std::unique_ptr<ServerCtx>` lives for the
  process lifetime. A `std::mutex` serialises the engine across httplib's
  worker threads.
* HTTP handlers cap request bodies at 8 MiB (configurable via `--max-body`)
  and catch every `std::exception` at the boundary so a malformed request
  cannot tear down the server.
* No raw `new`/`delete` anywhere in `src/` or `examples/`.

---

## License

Inherits the MIT license of llama.cpp. See `LICENSE`.
