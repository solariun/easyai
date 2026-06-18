# libeasyai — library guide

> **Audience.** Third-party developers who want to embed an AI agent in
> their own C++ application. If you only want to run our reference CLI
> or server, see `easyai-cli.md` / `easyai-server.md`.
>
> **Mental model.** The lib is the product. `easyai-cli` and
> `easyai-server` are demos that prove what the lib can do. Anything
> that talks to a model, registers a tool, or composes a system prompt
> lives in the lib so your code is as short as ours.

---

## 1. Five-line agent

```cpp
#include "easyai/session.hpp"

int main() {
    auto session = easyai::Session::remote("http://localhost:8080");
    session.with_default_tools()
           .system_append("Speak in plain English, max one paragraph.")
           .on_token([](const std::string & p){ std::fputs(p.c_str(), stdout); });

    std::string err;
    if (!session.init(err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    session.chat("what's the time in Tokyo?");
}
```

That is the entire program. The Session:

1. Opens a connection to the remote `/v1/chat/completions` endpoint.
2. Registers the canonical agent toolset (`datetime`, `web`,
   `tool_lookup`).
3. Composes the system prompt from the library's authoritative default
   + your `system_append` block + tool addenda + the per-turn
   date/time / memory-vocabulary preamble.
4. Streams tokens to your callback as they arrive.

Switch to local llama.cpp by changing the factory:

```cpp
auto session = easyai::Session::local("/path/to/model.gguf");
```

Everything else stays the same.

---

## 2. The Session shape

| Family | Methods | Notes |
|--------|---------|-------|
| Factories | `Session::local(path)` · `Session::local(LocalBackend::Config)` · `Session::remote(url, model="easyai")` | Pick the backend once; nothing else changes. |
| System prompt | `.system(text)` · `.no_builtin_system()` · `.system_append(text)` · `.system_append(callable)` · `.preamble_options(opt)` | Layered composition — see §4. |
| Tools | `.with_default_tools(bool)` · `.sandbox(dir)` · `.allow_bash()` · `.allow_python()` · `.no_web()` · `.use_google()` · `.tool_mode(Unified/Split/Both)` · `.memory(dir)` · `.external_tools(dir)` · `.add_tool(Tool)` | See §3. |
| Sampling | `.preset(name)` · `.temperature/.top_p/.top_k/.min_p/.repeat_penalty/.frequency_penalty/.max_tokens/.seed` | Same field names as OpenAI / llama-server. |
| Transport (remote only) | `.api_key` · `.model` · `.timeout_seconds` · `.tls_insecure` · `.ca_cert_path` | No-op when `Session::local`. |
| Engine (local only) | `.context` · `.gpu_layers` · `.threads` · `.batch` · `.split_mode` · `.rope_scaling` · `.rope_freq_scale` · `.yarn_orig_ctx` | No-op when `Session::remote`. |
| Streaming | `.on_token(callable)` | One callback for both backends. |
| Lifecycle | `.init(err)` · `.reset()` · `.refresh_system()` | `init` is once; `reset` clears history; `refresh_system` re-pushes the system prompt after a mid-session `.system_append`. |
| Chat | `.chat(user_message)` | Runs the agentic loop, returns the final visible reply. |
| Introspection | `.render_system()` · `.tools()` · `.mode()` · `.last_error()` · `.backend()` | `render_system` is the exact string the model will receive. |

All setters are fluent (`return *this;`) so the call site reads as one
chained statement. None of them throw.

---

## 3. Tools

### Built-in toolset

`with_default_tools()` (on by default) gives you:

| Tool | Gate | What it does |
|------|------|-------------|
| `datetime` | `!no_datetime()` | Wall-clock UTC + local time. |
| `web` (or `search_web` + `fetch_web`) | `!no_web()` | Search the web and fetch URLs. Engine cascade: google → brave → ddg-lite → bing → ddg. `use_google()` opts into Google's billed API (needs `GOOGLE_API_KEY` + `GOOGLE_CSE_ID`). |
| `fs` (or split `read_fs`/`write_fs`/…) | `.sandbox(dir)` set OR `.allow_bash` / `.allow_python` | Read/write/edit/list/glob/grep, scoped to the sandbox root. |
| `bash` | `.allow_bash()` | Shell command via `/bin/sh -c`. Not a hardened sandbox. |
| `evaluate` (legacy name `python3`) | `.allow_python()` (default ON when fs is on) | Read-only Python 3 stdlib evaluator, sandboxed. |
| `knowledge_save`/`search_knowledge`/`knowledge_load`/… (7 tools) | `.memory(dir)` | Persistent registry (markdown per entry). |
| External tools | `.external_tools(dir)` | Loads every `EASYAI-*.tools` manifest. |
| `tool_lookup` | always on when `with_default_tools()` | Catalogue + per-tool manual access. |

Tool-mode controls how multi-action tools are exposed:

| Mode | Schema shape |
|------|-------------|
| `Unified` | `fs(action="read")` — one dispatcher tool. |
| `Split` (default) | `read_fs`, `edit_fs`, … — one verb per tool. |
| `Both` | Registers both surfaces side-by-side. |

Smaller / quantised models dispatch more reliably against `Split`; that
is why it is the default. Bigger models tolerate `Unified` (and the
`fs(action="ops")` batch is only on the unified surface — see
`spec.md` §"fs Batch Mode").

### Writing your own tool

```cpp
easyai::Tool weather = easyai::Tool::builder("weather")
    .describe(
        "Return the forecast for the given city.\n"
        "\n"
        "ALWAYS confirm the city with the user before calling.")
    .short_describe("Forecast for a city — confirm city first.")
    .param("city", "string", "Target city", /*required=*/true)
    .param("units", "string", "metric | imperial (default metric)")
    .system_addendum(
        "## Weather guardrails\n"
        "Cite the source (the JSON `provider` field) every time you "
        "answer with a forecast.")
    .handle([](const easyai::ToolCall & c) {
        std::string city  = easyai::args::get_string_or(c.arguments_json, "city",  "");
        std::string units = easyai::args::get_string_or(c.arguments_json, "units", "metric");
        if (city.empty()) return easyai::ToolResult::error("missing city");
        // … call your weather provider …
        return easyai::ToolResult::ok("sunny, 22 °C  (provider: openweather)");
    })
    .build();

session.add_tool(std::move(weather));
```

Three fields the model sees:

| Field | Where it ships | Cost |
|-------|----------------|------|
| `short_description` | `<tools>` block sent on every turn | tokens × turns |
| `parameters_json` | `<tools>` block | tokens × turns |
| `description` (full) | `tool_lookup(name="weather")` on demand | tokens × lookups only |

And one the prompt sees:

| Field | Where it ships | Cost |
|-------|----------------|------|
| `system_addendum` | system prompt, ONCE at init | tokens × 1 |

That last one is the new convention: instead of asking the application
to add the tool, _and also_ add a paragraph to the system prompt
reminding the model what the tool is for, the tool ships its own
guardrails. The Session collects every registered tool's
`system_addendum` and concatenates them into the system prompt. No
drift; no "did we update both places?".

### Argument helpers

Use `easyai::args::*` to read flat keys out of the model's JSON
without a JSON dependency. They are deliberately lenient about model
typos:

```cpp
std::string q   = easyai::args::get_string_or(c.arguments_json, "query", "");
long long   k   = easyai::args::get_int_or   (c.arguments_json, "limit", 10);
bool        all = easyai::args::get_bool_or  (c.arguments_json, "all",   false);
```

For nested arrays the lenient parser unwraps stringified payloads
(models sometimes emit `"items":"[{...}]"` instead of
`"items":[{...}]`).

---

## 4. System-prompt composition

The Session composes the system prompt in five layers. Knowing the
order makes it obvious how to override exactly what you want.

```
┌──────────────────────────────────────────────────────────────┐
│ 1. BASE                                                       │
│    .system("…")          → operator's verbatim text           │
│    (no .system call)      → preamble::build_builtin_system_   │
│                              prompt(view)                     │
│    .no_builtin_system()   → empty                             │
├──────────────────────────────────────────────────────────────┤
│ 2. TOOL ADDENDA                                               │
│    for each registered Tool t: append t.system_addendum       │
├──────────────────────────────────────────────────────────────┤
│ 3. OPERATOR APPENDS                                           │
│    every .system_append(text) block, in call order            │
│    every .system_append(callable) block, called each refresh  │
├──────────────────────────────────────────────────────────────┤
│ 4. DYNAMIC PREAMBLE                                           │
│    preamble::build({inject_datetime, knowledge_cutoff,        │
│                     memory_root, cite_sources, has_memory})   │
├──────────────────────────────────────────────────────────────┤
│ 5. TOOLS CATALOGUE                                            │
│    Local sessions:  preamble::build_session_info(tools)       │
│    Remote sessions: server emits its own catalogue per request│
└──────────────────────────────────────────────────────────────┘
```

| Goal | What to call |
|------|-------------|
| Use our default + add a sentence | `.system_append("Your line")` |
| Use our default, drop the date block | `.preamble_options({.inject_datetime=false})` |
| Replace the prompt entirely | `.system("Your prompt")` |
| Author from scratch | `.no_builtin_system().system_append("…")` |
| Use a dynamic prompt | `.system_append([](){ return load_today(); })` |

`render_system()` returns the resolved string. Call it any time
(before or after `init`) to see exactly what the model will receive.

### Mid-session contracts

| Call | History | Use when |
|------|---------|----------|
| `session.system_append("...")` then `session.refresh_system()` | **preserved** | You want to add to the prompt without losing the conversation. |
| `session.set_system("new base")` | cleared (fresh start) | Operator-facing "/system <text>" — REPLACE + reset. |
| `session.add_tool(t)` post-init | preserved | Register a new tool mid-conversation; addendum + catalogue auto-refresh. |
| `session.add_tool(t)` pre-init | n/a | Normal setup; queued for `init()`. |
| `session.reset()` | cleared | Wipe history; keep tools + system. |

Two safety notes the lib enforces:

1. Every `Tool::system_addendum` and every `system_append(...)` is
   run through `easyai::preamble::sanitize_addendum` before splicing.
   C0 control bytes (NUL, ESC, bell, …) and DEL are stripped;
   `\n` and `\t` are preserved so paragraph structure survives.
   Caps: 8 KiB per tool addendum, 16 KiB per operator append.
   See `SECURITY_AUDIT.md` §25.1.
2. `engine_ptr()` / `client_ptr()` are read-only / additive escape
   hatches. If you mutate the wrapped Engine / Client directly
   (`engine_ptr()->add_tool(t)` instead of `session.add_tool(t)`),
   Session's cached state drifts: the tool shows in the next turn's
   `<tools>` block but its `system_addendum` never reaches the
   prompt. Use Session's own mutators for anything you want
   Session to track. See `SECURITY_AUDIT.md` §25.4.

---

## 5. Backends

The Session picks one of these at construction:

| Class | Wraps | Use when |
|-------|-------|----------|
| `LocalBackend` | `easyai::Engine` (llama.cpp in-process) | You want zero network hops, one process, full GPU/CPU control. |
| `RemoteBackend` | `easyai::Client` (OpenAI-protocol HTTP) | You already run `easyai-server` / `llama-server` / OpenAI / etc. |

Both ship in the unified `libeasyai`.

Both implement `easyai::Backend`, which gives a uniform `chat`,
`reset`, `set_system`, `set_sampling`, `last_was_ctx_full`,
`ctx_pct` surface. You can still construct one directly if you don't
need Session's fluent surface:

```cpp
easyai::LocalBackend::Config cfg;
cfg.model_path     = "...";
cfg.sandbox        = "/srv/work";
cfg.extra_tools    = { make_acme_tool() };       // new
cfg.system_appendix = "Use formal English.";      // new
easyai::LocalBackend be(cfg);
std::string err;
be.init(err);
be.chat("hello", [](const std::string & p){ std::fputs(p.c_str(), stdout); });
```

`extra_tools` + `system_appendix` were added at the same time as
`Session` so the lower-level Backend path stays at parity.

---

## 6. Linking

easyai ships as ONE library — `libeasyai.so` / `.dylib`. There is no
split between "engine" and "cli" libraries: the same shared object
carries the local Engine, the remote Client, every tool, and Session.

| You want | Headers | Link |
|----------|---------|------|
| Anything | `#include "easyai/easyai.hpp"` _or_ targeted headers (`easyai/session.hpp`, `easyai/engine.hpp`, `easyai/client.hpp`, `easyai/tool.hpp`, …) | `easyai` (alias `easyai::easyai`) |

Legacy aliases `easyai::engine` and `easyai::cli` resolve to the same
unified target so split-layout CMakeLists still work without change.

### CMake

```cmake
find_package(easyai 0.1 REQUIRED)
target_link_libraries(myapp PRIVATE easyai::easyai)
```

(See `easyai-server.md` for the CMake config files installed by
`cmake --install build`.)

---

## 7. Demo programs

| Binary | Source | What it demonstrates |
|--------|--------|---------------------|
| `easyai-library-demo` | `examples/library_demo.cpp` | Smallest possible Session program. Pair with this guide. |
| `easyai-chat` | `examples/chat.cpp` | One-shot remote chat. |
| `easyai-agent` | `examples/agent.cpp` | Custom tool + Engine direct (no Session). |
| `easyai-recipes` | `examples/recipes.cpp` | Tutorial agent — pairs with `manual.md`. |
| `easyai-local` | `examples/local.cpp` | Reference REPL on `LocalBackend`. |
| `easyai-cli` | `examples/cli.cpp` | Reference HTTP agent — REPL + shell mode. |
| `easyai-server` | `examples/server.cpp` | Reference HTTP server — `/v1/chat/completions`, web UI, MCP, metrics. |
| `easyai-mcp-server` | `examples/mcp_server.cpp` | Model Context Protocol provider. |

The bundled `easyai-cli` and `easyai-server` are intentionally
non-trivial — they include REPL polish, signal handling, web UI,
preset switching, etc. — but the AI-connection / tools / system-prompt
logic delegates to the lib in every case. When you write your own
binary, follow `easyai-library-demo`'s shape and reach for the larger
demos only when you need a piece of the polish.

---

## 8. Versioning, stability

Source layout is stable for the 0.1.x line:

| Path | Stability |
|------|-----------|
| `include/easyai/session.hpp` | **stable** — public OpenAI-Python-shape API. |
| `include/easyai/backend.hpp` | **stable** — Backend interface + Local/Remote Config. |
| `include/easyai/tool.hpp` | **stable** — Tool struct, Builder, args helpers. |
| `include/easyai/preamble.hpp` | semi-stable — building blocks may grow; existing fns won't change semantics. |
| `include/easyai/engine.hpp` · `client.hpp` | semi-stable — fluent setters may grow. |
| `include/easyai/cli.hpp` | mostly stable — `Toolbelt` knobs may grow. |
| `src/**` | internal — no API guarantee. |

We follow `spec.md` for behavioural contracts. When a contract changes
the spec gets updated in the same PR.
