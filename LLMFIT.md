# LLMFit dashboard (`/llmfit`)

A sober, dependency-free web UI built into **easyai-server** that answers two
questions for an operator standing up a local-AI box:

1. **What models will actually run well on this hardware?** — powered by
   [LLMFit](https://github.com/AlexsJones/llmfit), which scores hundreds of
   models across *quality*, *speed*, *fit*, and *context* for the detected (or
   simulated) RAM / VRAM / CPU.
2. **Get me one** — a native GGUF **download manager** that pulls weights
   straight from HuggingFace into the server's model directory, with progress,
   cancel, listing, and delete.

It is reachable at `http://<server>:<port>/llmfit` and is linked from the chat
web UI by a small **"LLMFit"** pill injected at the top-right.

> TL;DR for the impatient: the installer ships it on with password **`0000`**.
> Open `/llmfit`, log in with `0000`, browse the model table, paste a HuggingFace
> GGUF repo on the **Downloads** tab, and click **Download**. **Change the
> password** (`webui_password` in `/etc/easyai/easyai.ini`) before the box leaves
> your LAN.

---

## 1. How it works

```
Browser ──/llmfit (page) ─────────────► embedded webui/llmfit.html (vanilla JS)
        ──/llmfit/api/login,/auth ─────► cookie-session gate (examples/server.cpp)
        ──/llmfit/api/v1/* ────proxy───► LlmfitBridge ──httplib──► llmfit serve (child process)
        ──/llmfit/api/download* ───────► LlmfitBridge ──libcurl──► HuggingFace
        ──/llmfit/api/local-models* ──► LlmfitBridge (list / delete in download_dir)
chat webui ──(injected "LLMFit" pill)─► /llmfit
```

- **Recommendations are real llmfit output, never mocked.** On startup
  easyai-server spawns the external `llmfit` binary in its REST mode
  (`llmfit serve --host 127.0.0.1 --port <llmfit_port>`), with the environment
  variable `LLMFIT_MODELS_DIR=<download_dir>`, as a **child process**. Requests
  to `/llmfit/api/v1/*` are proxied to that child and the JSON is returned
  verbatim. The child is bound to loopback (never exposed) and is reaped on
  shutdown (SIGTERM → grace → SIGKILL).
- **Downloads are native to easyai-server** (C++/libcurl), not delegated to
  llmfit, so the operator fully controls where weights land and can list/delete
  them from the same UI.
- **Graceful degradation.** If the `llmfit` binary can't be found or the child
  dies, the page still loads and the download manager still works — only the
  recommendation panels report the engine offline (HTTP 502 under the hood, a
  banner in the UI).

Implementation: [`src/llmfit.cpp`](src/llmfit.cpp) +
[`include/easyai/llmfit.hpp`](include/easyai/llmfit.hpp) (the `LlmfitBridge`,
compiled into `libeasyai` where libcurl is linked), the routes / cookie auth /
nav-injection in [`examples/server.cpp`](examples/server.cpp), and the page in
[`webui/llmfit.html`](webui/llmfit.html) (embedded into the binary at build
time via `cmake/xxd.cmake`).

---

## 2. Quick start

### Installed via `install_easyai_server.sh`

The Linux installer writes the four keys below into `/etc/easyai/easyai.ini`
with `webui_password = 0000` by default and `download_dir` pointed at the
server's models directory. Override the password at install time:

```sh
./install_easyai_server.sh --webui-password 's3cret'
```

You still need the **llmfit engine binary** on the service's `PATH` — see §4.

### Running by hand

```sh
easyai-server -m models/your-model.gguf \
  --download-dir ./models \
  --webui-password 's3cret' \
  --llmfit-bin ~/.cargo/bin/llmfit \
  --llmfit-port 8788
```

Then open `/llmfit` (or click the LLMFit pill in the chat UI).

---

## 3. Installing the llmfit engine

The recommendation engine is the separate, open-source `llmfit` Rust binary.
easyai-server does not bundle it; install it once:

```sh
# Homebrew (macOS / Linuxbrew) — prebuilt:
brew install AlexsJones/llmfit/llmfit

# From source (Rust toolchain required):
git clone https://github.com/AlexsJones/llmfit && cd llmfit
cargo install --path llmfit-tui          # installs `llmfit` to ~/.cargo/bin
```

**systemd PATH caveat.** The `easyai-server.service` runs with the default
service `PATH` (`/usr/local/bin:/usr/bin:…`). `cargo install` drops the binary
in `~/.cargo/bin`, which is **not** on that PATH. Either copy it onto the
service PATH or point `llmfit_bin` at an absolute path:

```sh
sudo cp ~/.cargo/bin/llmfit /usr/local/bin/      # simplest
# …or in /etc/easyai/easyai.ini:
#   llmfit_bin = /home/you/.cargo/bin/llmfit
```

Verify the engine works on its own:

```sh
llmfit serve --port 8788 &
curl -s 127.0.0.1:8788/api/v1/system | jq .
```

---

## 4. Configuration reference

All keys live in the `[SERVER]` section of `easyai.ini` and each has a matching
CLI flag. Precedence is **CLI flag > INI value > built-in default**.

| INI key | CLI flag | Default | Meaning |
| --- | --- | --- | --- |
| `webui_password` | `--webui-password` | (empty — open) | Password for the `/llmfit` dashboard and **all** its API routes. A session cookie, separate from `api_key` (which still guards `/v1/*`). Empty leaves the dashboard open. The installer sets it to `0000`. |
| `download_dir` | `--download-dir` | directory of `--model` | Where the download manager writes GGUF weights and lists/deletes them. Also passed to the llmfit child as `LLMFIT_MODELS_DIR`. |
| `llmfit_bin` | `--llmfit-bin` | `llmfit` | The llmfit binary: a bare name resolved via `PATH`, or an absolute path. |
| `llmfit_port` | `--llmfit-port` | `8788` | Loopback port the child `llmfit serve` binds to (127.0.0.1 only; never exposed). |

Example `[SERVER]` block:

```ini
[SERVER]
model          = /var/lib/easyai/models/ai.gguf
webui_password = 0000
download_dir   = /var/lib/easyai/models
llmfit_bin     = llmfit
llmfit_port    = 8788
```

---

## 5. The password gate

The `/llmfit` dashboard is more powerful than the chat UI — it can download
multi-gigabyte files and delete model files — so it has its own gate, distinct
from the server's `api_key` Bearer auth on `/v1/*`.

- **Default from the installer: `0000`.** This is deliberately trivial so a
  freshly-flashed LAN appliance is usable out of the box. **Change it** before
  exposing the box to anything you don't trust.
- **How it works.** When `webui_password` is non-empty, a correct
  `POST /llmfit/api/login {"password":"…"}` sets an `HttpOnly`, `SameSite=Strict`
  session cookie (`easyai_llmfit=<random-token>`). The token is a random
  32-byte value generated once per server start (so restarting the server logs
  everyone out). The password is compared in constant time and never stored in
  the cookie. Empty `webui_password` disables the gate entirely.
- **Changing it.** Edit `webui_password` in `/etc/easyai/easyai.ini` and restart
  the service:

  ```sh
  sudo sed -i 's/^webui_password .*/webui_password  = my-new-pass/' /etc/easyai/easyai.ini
  sudo systemctl restart easyai-server
  ```

  (Or re-run the installer with `--webui-password '…'`.)
- **Caveat:** the installer's `--webui-password` value must not contain `=`,
  `[`, or `]` (it is written into the INI); a password with those characters
  should be set by editing the INI directly. There is no such restriction on
  the runtime `--webui-password` flag.

> The dashboard is served over plain HTTP like the rest of easyai-server. On an
> untrusted network, front it with TLS (a reverse proxy) — the cookie is
> `SameSite=Strict` but not `Secure`.

---

## 6. Using the dashboard

### Models tab

- **Detected hardware** — four cards (CPU, total RAM, available RAM, GPU) read
  live from llmfit.
- **Hardware simulation** — enter RAM / VRAM / CPU-core values and click
  **Simulate** to re-score every model *as if* the box had that hardware
  (handy before you buy or upgrade). **Reset** clears it.
- **Filters** — search, minimum fit (`Any`/`Marginal+`/`Good+`/`Perfect`),
  runtime (`llama.cpp`/`MLX`/`vLLM`), use case, provider, sort, and limit.
- **Model table** — name, provider, params, **fit** (colour-coded), run mode,
  runtime, score, est. tok/s, memory utilisation, and context. Click a row for
  detail.
- **Detail panel** — fit/quant/memory grid; a **score breakdown** (quality /
  speed / fit / context bars); a **hardware plan** (enter context / quant /
  target tok/s → minimum & recommended hardware, per-run-path feasibility, KV
  cache alternatives); the model's **GGUF sources** each with a **Download…**
  button (jumps to the Downloads tab pre-filled); and any notes.

### Downloads tab

- **Download a model** — paste a HuggingFace GGUF repo (e.g.
  `bartowski/Qwen2.5-7B-Instruct-GGUF`) and click **List quants** to see every
  `.gguf` in the repo with its size, then **Download** the one you want. Picking
  one shard of a sharded model fetches the whole shard set automatically.
- **Active download** — a live progress bar (percentage + bytes), with
  **Cancel**.
- **Downloaded models** — every `.gguf` in `download_dir` with its size and date,
  each with a **Delete** button (confirmation required).

---

## 7. Download manager details

- **Destination.** Files are written to `download_dir` (default: the directory
  of the loaded `--model`). A download streams to `<name>.gguf.part` and is
  atomically renamed on success; partial files are removed on cancel/error.
- **Quant auto-selection.** When you don't pick a specific file, the best
  available quant is chosen by the same preference order llmfit's llama.cpp
  provider uses (`Q8_0 > Q6_K > Q5_K_M > Q4_K_M > … > IQ*`).
- **Sharded models.** `*-NNNNN-of-MMMMM.gguf` shard sets are detected and all
  members are fetched in order; a standalone file that merely shares the stem is
  **not** pulled in.
- **Single active download.** GGUF files are large, so downloads are serialised
  — one at a time. A second request while one is running is rejected with
  HTTP 409.
- **Path safety.** Download and delete targets are validated against
  `download_dir` (no `/`, `\`, `..`, must end `.gguf`, must resolve inside the
  directory); delete additionally refuses anything that isn't a regular file
  (no symlinks/dirs).

---

## 8. REST API reference

All routes are under `/llmfit`. The page itself is unguarded (its JS shows a
login overlay); every **data** route requires the session cookie when
`webui_password` is set.

| Method | Path | Kind | Notes |
| --- | --- | --- | --- |
| GET | `/llmfit` | page | The dashboard HTML. |
| GET | `/llmfit/api/auth` | open | `{authed, required, llmfit_available, status, download_dir}`. |
| POST | `/llmfit/api/login` | open | `{"password":"…"}` → sets the `easyai_llmfit` cookie. |
| POST | `/llmfit/api/logout` | open | Clears the cookie. |
| GET | `/llmfit/api/v1/system` | proxy | Detected/simulated hardware. Query: `ram_gb`, `vram_gb`, `cpu_cores`. |
| GET | `/llmfit/api/v1/models` | proxy | Scored models. Query: `search`, `min_fit`, `runtime`, `use_case`, `provider`, `sort`, `limit`, `include_too_tight`, plus the sim params. |
| GET | `/llmfit/api/v1/models/top` | proxy | Top-N runnable models. |
| GET | `/llmfit/api/v1/models/{name}` | proxy | Search scoped to a name. |
| GET | `/llmfit/api/v1/runtimes` | proxy | Installed inference runtimes. |
| GET | `/llmfit/api/v1/installed` | proxy | Models installed in local runtimes. |
| POST | `/llmfit/api/v1/plan` | proxy | Hardware-plan estimate: `{model, context, quant?, kv_quant?, target_tps?, ram_gb?, vram_gb?, cpu_cores?}`. |
| GET | `/llmfit/api/hf/files?repo=<repo>` | native | List `.gguf` files (`{path, size_bytes}`) in a HuggingFace repo. |
| POST | `/llmfit/api/download` | native | `{"repo":"…","filename":"…"?}` → start a download. Returns `{id, repo, state}`. 409 if one is already running. |
| GET | `/llmfit/api/download/status` | native | `{id, repo, filename, state, downloaded_bytes, total_bytes, percent, error}` — `state ∈ idle/downloading/done/error`. |
| POST | `/llmfit/api/download/cancel` | native | Cancel the active download. |
| GET | `/llmfit/api/local-models` | native | `{dir, models:[{name, size_bytes, mtime}]}`. |
| POST | `/llmfit/api/local-models/delete` | native | `{"name":"…"}` → delete one `.gguf` in `download_dir`. |

The proxied `/api/v1/*` shapes are exactly llmfit's own REST API — see llmfit's
`API.md` for the authoritative field list. Unknown fields are forward-compatible.

Example:

```sh
B=http://127.0.0.1:8080
# log in, keep the cookie
curl -s -c cj.txt -X POST $B/llmfit/api/login -H 'Content-Type: application/json' -d '{"password":"0000"}'
# top models for this hardware
curl -s -b cj.txt "$B/llmfit/api/v1/models?limit=10&min_fit=good&sort=score" | jq '.models[].name'
# list quants in a repo, then download one
curl -s -b cj.txt "$B/llmfit/api/hf/files?repo=bartowski/Qwen2.5-7B-Instruct-GGUF" | jq .
curl -s -b cj.txt -X POST $B/llmfit/api/download -H 'Content-Type: application/json' \
  -d '{"repo":"bartowski/Qwen2.5-7B-Instruct-GGUF","filename":"Qwen2.5-7B-Instruct-Q4_K_M.gguf"}'
curl -s -b cj.txt $B/llmfit/api/download/status | jq .
```

---

## 9. Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| Dashboard banner "llmfit engine offline" / `/api/v1/*` returns 502 | The `llmfit` binary isn't found or the child died. Check the startup log for `[llmfit] …`; install llmfit on the service PATH or set `llmfit_bin` to an absolute path (§3). Downloads still work meanwhile. |
| `llmfit exited during startup … port … is free?` in the log | `llmfit_port` (default 8788) is already in use. Change it. |
| Login always fails | `webui_password` mismatch. Confirm the value in `/etc/easyai/easyai.ini` and that you restarted the service after changing it. |
| Download 409 "a download is already in progress" | Only one download runs at a time; wait or cancel it. |
| Downloaded file not appearing | Confirm `download_dir` is writable by the service user (`easyai`). The installer chowns `/var/lib/easyai/models` to it. |
| Want to expose to the internet | Put a TLS reverse proxy in front, set a strong `webui_password`, and consider `api_key` for `/v1/*` too. |

---

## 10. Cross-references

- [`easyai-server.md`](easyai-server.md) §1 (config keys) and §7 ("LLMFit
  dashboard") — server-side reference.
- [`resources/easyai.ini.example`](resources/easyai.ini.example) — the
  `[SERVER]` keys with inline comments.
- [`scripts/install_easyai_server.sh`](scripts/install_easyai_server.sh) —
  `--webui-password` (default `0000`), `download_dir`, `llmfit_bin`,
  `llmfit_port` baked into the generated INI.
- [LLMFit upstream](https://github.com/AlexsJones/llmfit) — the recommendation
  engine and its `API.md`.
