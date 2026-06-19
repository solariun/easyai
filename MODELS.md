# MODELS dashboard (`/models`)

A sober, dependency-free web UI built into **easyai-server** for picking, running,
and downloading local models. Everything is **native C++** — there is no external
binary and no network service to babysit. Reachable at
`http://<server>:<port>/models`, and linked from the chat web UI by a small
**"MODELS"** pill at the top-right.

It does four things:

1. **Recommend** — detects this machine's hardware (RAM / CPU / GPU + VRAM, via
   ggml) and keeps a **static list of the top GGUF models on HuggingFace**
   (rebuilt on startup, lazily after 1 hour, and on a **Refresh** button),
   scoring each for **fit / speed / quality** against that hardware (or a
   *simulated* one). List-level params/quant are estimated from the repo name;
   **clicking a model reads its remote GGUF header (an HTTP range request) for a
   precise fit**. Scoring is a native re-implementation of
   [LLMFit](https://github.com/AlexsJones/llmfit)'s. The runtime is fixed to
   **llama.cpp / GGUF**.
2. **Local models** — lists the `.gguf` files in your model directory; click one
   to open a panel with **all of its parameters** (read straight from the GGUF
   header — architecture, params, layers, heads, quant, context…), whether it
   **fits** this hardware, and the matching **`[MODEL_*]` INI profile** with its
   values.
3. **Run (hot-swap)** — one click **releases the running model and loads the
   chosen one in place**, with no server restart, updating the `ai.gguf` symlink
   so the choice survives reboots.
4. **Download manager** — fetches GGUF weights from HuggingFace into the model
   directory with progress / cancel, and lists / deletes what's already there.

> TL;DR: the installer ships it on with password **`0000`**. Open `/models`, log
> in, browse the **Recommend** table, or go to **Local models**, click a model,
> and hit **Run**. **Change the password** (`webui_password` in
> `/etc/easyai/easyai.ini`) before the box leaves your LAN.

---

## 1. How it works

```
Browser ──/models (page) ────────────► embedded webui/models.html (vanilla JS)
        ──/models/api/login,/auth ────► cookie-session gate (services/server.cpp)
        ──/models/api/system,/models ─► ModelsEngine: ggml hardware + live HF + scoring
        ──/models/api/local{,/detail} ► ModelsEngine: GGUF header introspection + [MODEL_*]
        ──/models/api/run ────────────► Engine::reload() in-process + ai.gguf symlink
        ──/models/api/download* ──────► ModelsEngine: libcurl → HuggingFace
chat webui ──(injected "MODELS" pill)─► /models
```

- **No external dependency; a small on-disk catalog cache.** Hardware is detected
  through ggml (`ggml_backend_dev_memory` for VRAM) + the OS (RAM/CPU). The
  Recommend list is the **1000 most-recently-updated GGUF repos** on HuggingFace
  (`/api/models?filter=gguf&sort=lastModified`, cursor-paged until 1000 or the
  listing is exhausted), persisted to `data_dir/easyai_hf_catalog.json` and
  refreshed from HuggingFace on request once the snapshot is >1h old — so a
  restart shows the list instantly. Per-repo specifics (exact params / quant /
  fit) are read live from the remote GGUF header on demand. Scoring, the hardware
  plan, and GGUF introspection are all native C++.
- **Hot-swap is in-process.** `Engine::reload()` tears down the current model
  (model, context, sampler, chat templates) and loads the new one under the
  engine lock — in-flight chats finish first, then the swap happens; the HTTP
  server and the dashboard you're looking at stay up.
- **Graceful degradation.** If HuggingFace is unreachable the Recommend tab
  shows an error, but **Local models** + **Downloads** + **Run** work fully.

Implementation: [`src/models_dashboard.cpp`](src/models_dashboard.cpp) +
[`include/easyai/models_dashboard.hpp`](include/easyai/models_dashboard.hpp) (the
`ModelsEngine`, in `libeasyai`), `Engine::reload()` in
[`src/engine.cpp`](src/engine.cpp), the routes / cookie auth / nav-injection in
[`services/server.cpp`](services/server.cpp), and the page in
[`webui/models.html`](webui/models.html) (xxd-embedded at build time).

---

## 2. Quick start

### Installed via `install_easyai_server.sh`

The Linux installer writes the keys below into `/etc/easyai/easyai.ini` with
`webui_password = 0000` by default and points `download_dir` at the models
directory. Override the password at install time:

```sh
./install_easyai_server.sh --webui-password 's3cret'
```

### Running by hand

```sh
easyai-server -m models/your-model.gguf \
  --download-dir ./models \
  --data-dir ./data \
  --webui-password 's3cret'
# Recommend caches the 1000 most-recent GGUF repos in --data-dir, refreshed >1h on request.
```

Then open `/models` (or click the MODELS pill in the chat UI).

---

## 3. Configuration reference

All keys live in `[SERVER]`; each has a matching CLI flag. Precedence is
**CLI flag > INI value > built-in default**.

| INI key | CLI flag | Default | Meaning |
| --- | --- | --- | --- |
| `webui_password` | `--webui-password` | (empty — open) | Password for `/models` and **all** its API routes. A session cookie, separate from `api_key` (which still guards `/v1/*`). The installer sets it to `0000`. |
| `download_dir` | `--download-dir` | directory of `--model` | Where GGUF weights are downloaded / listed / deleted, and the directory the dashboard introspects + hot-swaps from. |
| `data_dir` | `--data-dir` | `download_dir` | Where the dashboard persists its HuggingFace catalog snapshot (`easyai_hf_catalog.json`). Loaded on startup so the list shows instantly, and the 1-hour refresh clock survives a restart. The server creates it if missing. |
| `catalog_size` | `--catalog-size` | `1000` | How many of the **most-recently-updated** GGUF repos to keep in the searchable catalog, paged from HuggingFace (cursor-followed until this many or the listing runs out) and refreshed on request once the snapshot is >1h old. Clamped to `[1, 1000]`. |

---

## 4. The password gate

The dashboard can download multi-gigabyte files, delete model files, and **swap
the model the whole server is serving** — so it has its own gate, distinct from
the server's `api_key` Bearer auth on `/v1/*`.

- **Default from the installer: `0000`.** Trivial on purpose so a freshly-flashed
  LAN appliance is usable out of the box. **Change it** before exposing the box.
- **How it works.** A correct `POST /models/api/login {"password":"…"}` sets an
  `HttpOnly`, `SameSite=Strict` cookie `easyai_models=<random-token>` (a token
  generated once per server start; restarting logs everyone out). The password
  is compared in constant time and never stored in the cookie. Empty
  `webui_password` disables the gate.
- **Change it:** edit `webui_password` in `/etc/easyai/easyai.ini`, then
  `sudo systemctl restart easyai-server` (or re-run the installer with
  `--webui-password '…'`).

> The dashboard is plain HTTP like the rest of easyai-server. On an untrusted
> network, front it with TLS — the cookie is `SameSite=Strict` but not `Secure`.

---

## 5. Using the dashboard

### Recommend tab

- **Detected hardware** chips (CPU / RAM / GPU / backend) up top.
- **Hardware simulation** — enter RAM / VRAM / CPU-core values to re-score every
  model *as if* the box had that hardware (handy before you buy or upgrade).
  **Reset sim** clears it.
- **Filters** — search, minimum fit, use case, **quantization** (Auto = best
  fitting, or force a specific quant), sort, limit. **Refresh list** rebuilds the
  snapshot from HuggingFace.
- **Table** — params, **quant**, **fit** (colour-coded), run mode, score, est.
  tok/s, memory utilisation, HF downloads. **Fit is judged at 128K context** (or
  the model's native context if smaller). Click a row → the detail drawer, which
  reads the model's **remote GGUF header** for a precise fit and shows, right
  below the chips, a **hardware plan** that runs automatically: a **context**
  selector (default 128K), a **quant** selector listing **all quants available**
  for that model, and minimum/recommended hardware + KV-cache alternatives. Also
  shows the model's date and parameters, and a **Download** link.

### Local models tab

- A table of every `.gguf` in `download_dir`, with size, date, and a **running**
  badge on the active model.
- Click one to open the **parameter panel**: architecture, parameters,
  quantization, context length, layers, heads (q/kv), embedding, vocab, experts
  (for MoE), file size — read directly from the GGUF header — plus the computed
  **fit** on this hardware and the matching **`[MODEL_*]` INI profile** (the
  exact keys/values that will apply), or a note that engine defaults apply.
- **Run** hot-swaps to it (see §6); **Delete** removes it from disk (with
  confirmation; the running model can't be deleted).

### Downloads tab

- Paste a HuggingFace GGUF repo → **List quants** to see every `.gguf` with its
  size → **Download** the one you want (or **Download best quant**). Picking one
  shard of a sharded model fetches the whole set. A live progress bar with
  **Cancel**. Below, the same **Downloaded models** list with Run / Delete.

---

## 6. Run / hot-swap mechanics

Clicking **Run** on a local model:

1. Validates the filename and resolves it inside `download_dir`.
2. If the server's configured `--model` path is a **symlink** (the `ai.gguf`
   convention from the installer), re-points it at the chosen file so the choice
   **persists across restarts**.
3. Calls `Engine::reload(<file>)` **under the engine lock** — any in-flight chat
   finishes first, then the current model/context/sampler/templates are freed and
   the new model is loaded with the same context/ngl/sampling settings.
4. Re-applies the server's default system prompt + tools and updates the model id
   advertised by `/v1/models`.

No process restart; the dashboard stays live and reflects the new running model.

---

## 7. Download manager details

- **Destination:** `download_dir`. A download streams to `<name>.gguf.part` and
  is atomically renamed on success; partials are removed on cancel/error.
- **Quant auto-select** (Download best quant) follows the preference order
  `Q8_0 > Q6_K > Q5_K_M > Q4_K_M > … > IQ*`.
- **Sharded models** (`*-NNNNN-of-MMMMM.gguf`) are detected and all members
  fetched in order.
- **Single active download** (GGUF files are large); a second request returns 409.
- **Path safety:** download/delete targets must be a bare `.gguf` filename that
  resolves inside `download_dir`; delete refuses anything that isn't a regular
  file (no symlinks/dirs), and the currently-running model.

---

## 8. REST API reference

All routes are under `/models`. The page is unguarded (its JS shows a login
overlay); every **data** route requires the session cookie when `webui_password`
is set.

| Method | Path | Notes |
| --- | --- | --- |
| GET | `/models` | The dashboard HTML. |
| GET | `/models/api/auth` | `{authed, required, source, status, download_dir}` (open). |
| POST | `/models/api/login` / `logout` | `{"password":"…"}` → sets / clears the cookie. |
| GET | `/models/api/system` | Detected/simulated hardware. Query: `ram_gb`, `vram_gb`, `cpu_cores`. |
| GET | `/models/api/models` | The scored model snapshot. Query: `search`, `min_fit`, `use_case`, `sort` (`score`/`tps`/`params`/`mem`/`downloads`/`likes`), `limit`, + sim params. Envelope also carries `refreshing`, `last_refresh`, `stale`. |
| POST | `/models/api/refresh` | Rebuild the static model list from HuggingFace (runs in the background). |
| GET | `/models/api/hf/detail?repo=<repo>&context=&quant=` | Precise fit for a HF model — reads its remote GGUF header (HTTP range). Returns real params, the **available quants**, the fit, and a **hardware plan** at the chosen context (default 128K) + quant. |
| POST | `/models/api/plan` | `{model, context, quant?, kv_quant?, ram_gb?, vram_gb?, cpu_cores?}` → min/recommended hardware + KV alternatives. |
| GET | `/models/api/local` | `{dir, models:[{name, size_bytes, mtime, is_current}]}`. |
| GET | `/models/api/local/detail?file=<name>` | GGUF params + fit + `[MODEL_*]` profile for one local model. |
| POST | `/models/api/local/delete` | `{"name":"…"}` → delete one `.gguf`. |
| POST | `/models/api/run` | `{"name":"…"}` → hot-swap to that local model. |
| GET | `/models/api/hf/files?repo=<repo>` | List `.gguf` files (`{path, size_bytes}`) in a HF repo. |
| POST | `/models/api/download` | `{"repo":"…","filename":"…"?}` → start a download (409 if one is running). |
| GET | `/models/api/download/status` | `{id, repo, filename, state, downloaded_bytes, total_bytes, percent, error}`. |
| POST | `/models/api/download/cancel` | Cancel the active download. |

Example:

```sh
B=http://127.0.0.1:8080
curl -s -c cj -X POST $B/models/api/login -H 'Content-Type: application/json' -d '{"password":"0000"}'
curl -s -b cj "$B/models/api/models?limit=10&min_fit=good&sort=score" | jq '.models[].name'
curl -s -b cj "$B/models/api/local" | jq .
curl -s -b cj -X POST $B/models/api/run -H 'Content-Type: application/json' -d '{"name":"qwen2.5-0.5b-instruct-q4_k_m.gguf"}'
```

---

## 9. The model source & scoring fidelity

- **Source:** the Recommend list is a snapshot of the **1000 most-recently-updated
  GGUF repos** on HuggingFace (`/api/models?filter=gguf&sort=lastModified`),
  followed across the listing cursor until it has 1000 or the listing is
  exhausted (`--catalog-size` caps it, default/max 1000). It is **persisted to
  `data_dir/easyai_hf_catalog.json`** so a restart serves it instantly, refreshed
  **lazily when accessed if >1 h old**, and on demand via the **Refresh list**
  button. While a rebuild runs the UI shows a "Rebuilding…" banner.
- **List vs detail accuracy:** in the list, **params are inferred from the repo
  name (e.g. `…-7B`)** and quant defaults to Q4_K_M (the scorer still picks the
  best quant that fits). **Clicking a model** fetches its repo's best GGUF and
  **reads the remote GGUF header via an HTTP range request** (no full download),
  recovering the real architecture / params / context length / layers / heads
  for a **precise fit** — shown in the "Precise fit" card of the detail panel.
- **Scoring** is a faithful native port of llmfit's memory model, fit levels,
  quant selection, tok/s estimation and the four score components (quality /
  speed / fit / context), with llmfit's constants. A few exotic branches (full
  MoE bandwidth decomposition, the complete GPU bandwidth table) are
  approximated — numbers may differ from llmfit at the margins, which is expected.

---

## 10. Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| Recommend tab shows "HuggingFace: …" error | The server couldn't reach the HuggingFace API (offline / rate-limited / DNS). Local models, Downloads, and Run still work. Retry, or narrow the search. |
| GPU shows as CPU / wrong VRAM | ggml didn't detect a GPU backend (driver / build). On Apple Silicon VRAM == system RAM (unified). Use the simulation inputs to model target hardware. |
| Run fails | The new GGUF couldn't load (corrupt / incompatible). The error is returned; the previous model stays unloaded — re-run a known-good model. |
| Download 409 | One download at a time; wait or cancel. |
| Downloaded file not appearing | Confirm `download_dir` is writable by the service user. |

---

## 11. Cross-references

- [`easyai-server.md`](easyai-server.md) §7 — server-side reference.
- [`resources/easyai.ini.example`](resources/easyai.ini.example) — the `[SERVER]` keys.
- [`scripts/install_easyai_server.sh`](scripts/install_easyai_server.sh) — `--webui-password` (default `0000`), `download_dir`.
- [LLMFit upstream](https://github.com/AlexsJones/llmfit) — the original scoring tool this is ported from.
