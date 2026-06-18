# easyai — developer manual

This is the **hands-on** book. It assumes nothing beyond "I can compile a
C++17 program". By the end you will know how to:

* read the **Primer** below first if you've never traced a tool
  call end-to-end — it's the bottom-up walkthrough that makes the
  rest of the manual click
* compile easyai and download a model
* run `easyai-local` and talk to it
* host `easyai-server` and call it from Claude Code, OpenAI SDKs, or curl
* embed `easyai::Engine` in your own program (local llama.cpp)
* embed `easyai::Client` in your own program (remote OpenAI-compatible
  server, with local tools)
* drive a remote server end-to-end with `easyai-cli`, including
  a planning tool and live system-observability tools
* write a custom tool, with typed parameters
* tune the sampler with presets and runtime overrides
* deploy easyai-server as a hardened Linux service and operate it
* debug common issues (context overflow, malformed tool calls, GPU
  fallback, TLS, rate limits)

---

## Table of contents

| Part | Chapter | What you get |
|------|---------|--------------|
| **Primer** | How a tool call works on the wire | The 10 steps from your `Tool` declaration to the model's final answer — bytes, Jinja templates, parsing, dispatch, follow-up turn |
| **1** | Getting set up         | Prereqs, repo layout, building, GPUs, models |
| **2** | Using the binaries     | `easyai-local`, `easyai-server`, `easyai-mcp-server`, `easyai-cli`, `easyai-agent`, `easyai-chat`, `easyai-recipes` |
| **3** | Embedding `libeasyai`  | `Agent` (3-line hello), `Backend` (local↔remote), `Engine` API top-to-bottom, callbacks, presets, tools, escape hatches |
| **4** | Embedding `libeasyai-cli` | `Client` API top-to-bottom — your code drives a remote model with local tools |
| **5** | Authoring custom tools | Builder API, schemas, sandboxes, error handling, the `Plan` tool, `system_*` tools cookbook |
| **6** | Deploying easyai-server | Single-binary install, systemd unit, nginx TLS termination, per-model INI profiles, multiple-server fan-out |
| **7** | Operating the server   | `/health` and `/metrics`, presets at runtime, log rotation, crash capture |
| **8** | Performance & tuning   | KV cache types, flash-attn, mlock, ngl auto-fit, RoPE/YaRN context extension, GPU split mode, sampler choices |
| **9** | Recipes (cookbook)     | Real prompts + flag combinations, including the planning agent, papers digest, host triage |
| **10** | Troubleshooting       | Build, GPU, runtime, model, tool, network, TLS issues |
| **11** | Design references     | Pointers into `design.md` for the deeper "why" |

> If you're new, read **Primer** → 1 → 2 → 3.  If you want to ship
> something to a remote model right now, jump to **Part 4**.  If you
> want to write your own tool, **Part 5** is the cookbook.  If
> something's broken, **Part 10** has a triage matrix.

---

## Primer — how a tool call works on the wire

Read this first.  Everything else in the manual makes more sense
once you can see the bytes flowing between you, easyai, and the
model.  We trace one tool call end-to-end, bottom-up — your C++
declaration → Jinja-rendered prompt → model output → parse →
dispatch → result → next turn.  No magic.

The example: a `get_weather` tool the model invokes for "what's
the weather in Lisbon?".

### Step 1 — you declare the tool (C++)

```cpp
auto weather = easyai::Tool::builder("get_weather")
    .short_describe("Current weather for a city (metric units).")
    .describe("Return the current weather for a city, in metric "
              "units. Caches per-city for 5 minutes. Returns "
              "'<temp>°C, <conditions>' or an error.")
    .param("city", "string", "City name, e.g. 'Lisbon'", /*required=*/true)
    .handle([](const easyai::ToolCall & c) -> easyai::ToolResult {
        std::string city = easyai::args::get_string_or(c.arguments_json, "city", "");
        if (city.empty()) return easyai::ToolResult::error("'city' is required");
        return easyai::ToolResult::ok("23 °C, sunny");
    })
    .build();

engine.add_tool(weather);
engine.chat("what's the weather in Lisbon?");
```

The `Tool` carries five fields: `name`, `description` (the full
manual), `short_description` (one-line trigger), `parameters_json`
(JSON schema), `handler`.

> **Shape-C wire shape (since 2026-05-26).** Per-turn `<tools>`
> blocks ship `name + short_description + schema` (~2 000 tokens
> saved vs. the full description). Models reach for `tool_lookup
> (name="get_weather")` to pull the full multi-line body when they
> need it. Builders that set only `.describe(...)` keep working —
> `wire_description()` falls back to the first 120 chars of the
> full description.

At this point easyai does nothing except remember the tool — no
prompt has been built yet.

### Step 2 — easyai builds the chat-template inputs

When `chat()` runs, easyai hands the conversation + tool catalogue
to llama.cpp's `common_chat_templates_inputs`.  Conceptually:

```
inputs = {
  messages: [
    { role: "system",    content: "<your system prompt>"     },
    { role: "user",      content: "what's the weather in Lisbon?" }
  ],
  tools: [
    {
      type: "function",
      function: {
        name: "get_weather",
        description: "Return the current weather for a city, in metric units.",
        parameters: { type: "object",
                      properties: { city: { type:"string", description:"…" } },
                      required: ["city"] }
      }
    }
  ],
  add_generation_prompt: true
}
```

easyai itself never decides how to format this — the **chat
template baked into the GGUF** does.  The template is Jinja,
shipped by the model author.  Different model families use
different markup, which is why easyai needs three parallel
recovery layers (Step 5).

### Step 3 — Jinja renders the prompt the model actually sees

The Jinja template walks `inputs.messages` and `inputs.tools` and
emits a single string of bytes.  Here are three approximate
renderings — your model's exact bytes depend on its template,
but the shape is universal:

**Qwen3 / Qwen2.5 (`<tool_call>` + JSON):**

```
<|im_start|>system
<your system prompt>

# Tools
You have access to the following tools. To use one, emit a
<tool_call>...</tool_call> block with the call as JSON.

<tools>
{"type":"function","function":{"name":"get_weather","description":"…","parameters":{…}}}
</tools>
<|im_end|>
<|im_start|>user
what's the weather in Lisbon?<|im_end|>
<|im_start|>assistant
```

**Hermes-2-Pro (`<tool_call>` wrapping XML-ish):**

```
<|im_start|>system
<your system prompt>

You may call tools using:
<tool_call>
<function=NAME>
<parameter=KEY>VALUE</parameter>
</function>
</tool_call>

Tools available:
- get_weather(city: string): Return the current weather for a city.
<|im_end|>
<|im_start|>user
what's the weather in Lisbon?<|im_end|>
<|im_start|>assistant
```

**ChatML / OpenAI-style (function-call JSON in a fenced block):**

```
<|im_start|>system
<your system prompt>

# Functions
```json
[{"name":"get_weather","description":"…","parameters":{…}}]
```
Call a function by emitting a fenced JSON block with `name` +
`arguments`.
<|im_end|>
<|im_start|>user
what's the weather in Lisbon?<|im_end|>
<|im_start|>assistant
```

The same `Tool` you declared in C++ ends up in different markup
in each.  You don't need to care: easyai handles all three.

### Step 4 — the model emits a turn

The model decodes one token at a time.  Each token streams to
your `on_token` callback in real time.  When the model decides
to call the tool, the **raw text it produces** in a Qwen-family
model looks something like:

```
I'll check the current weather in Lisbon.
<tool_call>{"name":"get_weather","arguments":{"city":"Lisbon"}}</tool_call>
```

The model then emits an end-of-turn token (e.g. `<|im_end|>`) and
stops.  At this point easyai has the full raw turn as a string.

### Step 5 — easyai parses the tool call (PEG + 3 recovery layers)

`parse_assistant` (in `src/engine.cpp`) tries the parsers in
order, fastest path first:

1. **PEG parser** (llama.cpp's `common_chat_parse`).  Knows the
   model family (selected from the GGUF metadata at load time)
   and runs a grammar-driven parse.  Hits ~99 % of well-behaved
   turns.
2. **Qwen recovery scanner** (`recover_qwen_tool_calls`).  For
   `<tool_call>{…}</tool_call>` blocks the PEG dropped because
   of an inner-brace edge case.
3. **Hermes recovery scanner** (`recover_hermes_tool_calls`).
   For `<function=NAME><parameter=K>V</parameter></function>`
   markup that the PEG sometimes fails to assemble.
4. **Markdown recovery scanner** (`recover_markdown_tool_calls`).
   Last-resort heuristic for "🔧 get_weather(city='Lisbon')"
   and similar prose-style emissions some weak models prefer.

All four converge on the same shape — `common_chat_msg` with a
`content` field (visible reply, may be empty) and a
`tool_calls[]` array.  After parsing, easyai materialises:

```cpp
struct ParsedAssistantTurn {
    std::string content            = "I'll check the current weather in Lisbon.";
    std::string reasoning_content  = "";          // any <think>…</think> block
    std::vector<ToolCall> tool_calls = {
        ToolCall {
            .id              = "call_0",
            .name            = "get_weather",
            .arguments_json  = "{\"city\":\"Lisbon\"}"
        }
    };
    std::string finish_reason = "tool_calls";
};
```

### Step 6 — easyai dispatches to your handler

For each entry in `tool_calls`, easyai looks up the registered
`Tool` by name and invokes its handler.  The handler runs **on
the same thread that called `chat()`** — it's just a function
call.  No threads, no IPC, no fork.  Your handler is free to
block, do HTTP, hit the disk, anything.

```cpp
// Inside Engine::Impl::dispatch_tool (simplified):
auto * t = find_tool_by_name(call.name);             // Step 1
if (!t) {
    return ToolResult::error("unknown tool: " + call.name);
}
ToolResult result = t->handler(call);                 // Step 2 — your code
if (on_tool) on_tool(call, result);                   // Step 3 — observability hook
return result;
```

For our example:

```cpp
ToolCall  in  = { .name = "get_weather", .arguments_json = "{\"city\":\"Lisbon\"}", … };
ToolResult out = { .content = "23 °C, sunny", .is_error = false };
```

### Step 7 — easyai pushes the result back into history

A new message of role `tool` is appended:

```cpp
HistoryMessage {
    role          = "tool",
    content       = "23 °C, sunny",
    tool_name     = "get_weather",
    tool_call_id  = "call_0"   // matches the call's id
};
```

The `tool_call_id` is what links the result to the assistant's
preceding `tool_calls[i].id`.  Without it, the chat template
can't pair them up and weak models hallucinate the result back
to themselves.

### Step 8 — next turn renders with the result included

easyai re-runs Jinja over the now-extended history and feeds it
back into llama.cpp.  Approximate Qwen3 rendering:

```
…(system + user same as before)…
<|im_start|>assistant
I'll check the current weather in Lisbon.
<tool_call>{"name":"get_weather","arguments":{"city":"Lisbon"}}</tool_call><|im_end|>
<|im_start|>tool
<tool_response>
{"name":"get_weather","content":"23 °C, sunny"}
</tool_response><|im_end|>
<|im_start|>assistant
```

The model now sees its own previous tool_call AND the result it
got back.  It picks up where it left off.

### Step 9 — model produces the final answer

```
The current weather in Lisbon is 23 °C and sunny.<|im_end|>
```

`finish_reason` is `stop`.  No more tool_calls.  easyai returns
the visible content from `chat()`.

### Step 10 — the loop (if there are more tool calls)

If the model emits another `tool_call` instead of stopping,
easyai loops back to Step 6.  Each loop counts as one **hop**;
the budget is `Engine::max_tool_hops()` (default 8, bumped to
99999 when `bash` is enabled).  Hop exhaustion returns whatever
the latest visible content is, plus a `last_error()` string.

There's a second safety net: if the model produces an
"announce-only" turn ("Let me search…", "I'll look that up…")
without actually emitting a tool_call, easyai discards that
turn, appends a corrective synthetic user message ("don't
announce, execute"), and retries up to
`Engine::max_incomplete_retries()` times (default 10).  This is
the `looks_like_announce_phrase` predicate in `src/engine.cpp`,
and it's why weak / 1-bit-quant models still drive a
multi-tool flow reliably.

### What this buys you

* You write **one** `Tool` declaration; it works across model
  families.  The Jinja template + the recovery layers absorb the
  dialect differences.
* You can swap models freely.  Qwen → Hermes → DeepSeek-R1 → a
  Bonsai-class 1-bit quant — your tool code is unchanged.
* You can reason about correctness.  Every byte of every step
  above is reproducible: turn on `--verbose` (CLI) or open
  `/tmp/easyai-<pid>-<epoch>.log` and you see the rendered
  prompt, the raw model output, the parsed `tool_calls`, the
  dispatch summary, and the next-turn render — in order.

When something goes wrong — model never calls the tool, calls
the wrong tool, hallucinates an argument — you read the log
top-to-bottom against this primer and find the layer that
broke.  Most of the time it's Step 3 (your tool description
wasn't clear enough for the model) or Step 6 (your handler
returned an error the model couldn't recover from).

Now that you have the picture, the rest of the manual fills in
the practical details: Part 3 / Part 4 show how to wire this up
in C++; Part 5 is the cookbook for declaring tools that the
model actually calls correctly.

---

## Part 1 — getting set up

### 1.1 Layout

easyai expects llama.cpp as a sibling directory:

```
develop/
├── easyai/        # this project
└── llama.cpp/     # https://github.com/ggml-org/llama.cpp
```

Clone llama.cpp if you haven't:

```bash
cd ~/develop
git clone https://github.com/ggml-org/llama.cpp
```

### 1.2 Dependencies

| Required               | Why                                |
|------------------------|------------------------------------|
| CMake ≥ 3.18           | build system                       |
| A C++17 compiler       | the library is C++17               |
| (Apple) Xcode CLT      | Metal headers for GPU acceleration |
| (Linux/Win) Vulkan SDK | optional; pass `-DGGML_VULKAN=ON`  |

| Optional        | Used by             |
|-----------------|---------------------|
| libcurl         | the unified `web` tool (action=`search` / `fetch`) |

On macOS:

```bash
brew install cmake curl
```

### 1.3 First build

#### 1.3.1 Pick the right configure command for your hardware

| Target                            | Configure command                                                                  |
|-----------------------------------|------------------------------------------------------------------------------------|
| **Apple Silicon / Intel Mac (Metal)** | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`                                |
| **NVIDIA GPU (CUDA)**             | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON`                    |
| **AMD / Intel / cross-vendor (Vulkan)** | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_VULKAN=ON`            |
| **AMD on Linux (ROCm/HIP)**       | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_HIP=ON -DAMDGPU_TARGETS=gfx1100` |
| **CPU-only (any OS)**             | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` (then run with `-ngl 0`)          |

**NVIDIA / CUDA** — install the CUDA Toolkit so `nvcc` is on `PATH`. If
CMake complains about an unknown architecture, pin one explicitly:
`-DCMAKE_CUDA_ARCHITECTURES=89` (e.g. for RTX 4090) or use `native`.

**AMD / Vulkan** — install the Vulkan SDK
([LunarG](https://vulkan.lunarg.com/sdk/home) on Win/macOS, distro
`vulkan-tools libvulkan-dev` on Linux). On Linux, also install the GPU
driver's Vulkan ICD (`mesa-vulkan-drivers` for AMD/Intel, NVIDIA driver
ships its own).

**AMD / ROCm** — set `AMDGPU_TARGETS` to your card's gfx version.
Check with `rocminfo`.

**CPU-only** — same configure command as Metal but always pass `-ngl 0` at
runtime (or `engine.gpu_layers(0)` in code) so layers stay on CPU.

#### 1.3.2 Build

```bash
cd easyai
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # use the right line above
cmake --build build -j
```

Outputs land in `build/`:

```
build/easyai-local      # local-only REPL (loads a GGUF in-process)
build/easyai-cli        # agentic REPL talking to a remote OpenAI-compat endpoint
                        #   (full doc: easyai-cli.md)
build/easyai-server     # HTTP server + webui  (full doc: easyai-server.md)
build/easyai-mcp-server # standalone MCP-only HTTP daemon, no model loaded,
                        #   sized for thousands of parallel clients
                        #   (full doc: easyai-mcp-server.md)
build/easyai-agent      # demo agent (every tool + a custom one)
build/easyai-chat       # bare REPL (no tools)
build/libeasyai.dylib # the library
```

If the configure step says `easyai: libcurl found — web tool enabled`,
the unified `web` tool's search and fetch actions work out of the box
(no extra service to run).

### 1.4 Get a model

Tiny, fast, decent at tools — start here:

```bash
mkdir -p models
curl -L -o models/qwen2.5-1.5b-instruct-q4_k_m.gguf \
  'https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/qwen2.5-1.5b-instruct-q4_k_m.gguf?download=true'
```

For real work upgrade to Qwen2.5-7B-Instruct or Llama-3.1-8B-Instruct.

---

## Part 2 — using the binaries

### 2.1 Hello, REPL

```bash
./build/easyai-local -m models/qwen2.5-1.5b-instruct-q4_k_m.gguf
```

You'll see something like:

```
[easyai-local] loaded models/qwen2.5-1.5b-instruct-q4_k_m.gguf
               backend=MTL0  ctx=4096  tools=7  preset=precise
               type '/help' for commands, '/quit' to exit
> what's 2+2
2 + 2 equals 4.
>
```

Try a tool:

```
> What time is it right now in UTC?
[tool] datetime -> {"utc":"2026-04-25T10:20:49Z","local":"…"}
The current UTC time is 2026-04-25 10:20:49.
```

Try a preset:

```
> creative
[preset → creative]
> write a haiku about silicon
Quiet wafer hums,
moonlit traces drink the dawn —
glass dreams in the dust.
```

`creative 0.9 …` does both at once: switch preset, override temperature
just for this generation, then run the rest of the line as a prompt.

Use `/help` to list every preset; `/system <text>` to swap the system
prompt mid-session; `/reset` to wipe history.

### 2.2 Hello, server

```bash
./build/easyai-server -m models/qwen2.5-1.5b-instruct-q4_k_m.gguf
./build/easyai-server -m models/...gguf --sandbox ./work --allow-bash
./build/easyai-server -m models/...gguf -s system.txt
```

Without `-s`, the server boots up as **Deep** — an expert system
engineer persona built into the default system prompt.  Deep
operates a `TIME → THINK → PLAN → EXECUTE → VERIFY` loop and treats
`datetime` as the first tool call any time the answer touches "now"
or "today".  Operators who want a different voice supply their own
`--system "<text>"` or `-s persona.txt` — Deep is the default, not
hardcoded.

`--allow-fs` enables the unified `fs` tool (action=read / write /
list / glob / grep / check_path / cwd / sandbox); pair with
`--sandbox <dir>` to scope it under `<dir>` (otherwise it operates
against the process's cwd).  `--allow-bash` adds the shell tool,
also pinned to `<dir>` when `--sandbox` is set.  All three default
OFF — fresh installs don't expose write access or shell to the
model until the operator
opts in. Note that `--sandbox <dir>` alone does NOT register *_file;
prior versions implied it but as of 2026-05-08 the flags are
honoured independently so an operator can run with a sandbox
boundary and no *_file registered.

If you pass `-s system.txt`, that text becomes the default system
prompt for any request that doesn't already include one.

Open `http://127.0.0.1:8080` in a browser to use the bundled webui, or
talk to it via curl:

```bash
curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"easyai","messages":[{"role":"user","content":"Hi!"}]}'
```

#### 2.2.1 Pointing Claude Code at it

Use whatever Claude Code's "OpenAI-compatible base URL" setting is called in
your version (`--api-base`, env var, or settings file) and set it to
`http://127.0.0.1:8080/v1`. Anything Claude Code declares as a tool will be
forwarded; anything it doesn't declare will use easyai's built-in toolbelt.

#### 2.2.2 Pointing the OpenAI SDK at it

```python
from openai import OpenAI
client = OpenAI(base_url="http://127.0.0.1:8080/v1", api_key="not-checked")
print(client.chat.completions.create(
    model="easyai",
    messages=[{"role":"user","content":"Hi!"}]
).choices[0].message.content)
```

#### 2.2.3 Override temperature inline

Every request body can carry `temperature`, `top_p`, `top_k`. Or your user
can put a preset right in the message:

```json
{ "messages": [{"role":"user","content":"creative 0.9 write me a poem"}] }
```

The server peels `creative 0.9 ` off, applies the override, and the model
sees just `write me a poem`.

### 2.3 Demo agent (every tool, plus a custom one)

```bash
./build/easyai-agent -m models/qwen2.5-1.5b-instruct-q4_k_m.gguf
```

Look at `examples/agent.cpp` to see how the tools are registered. The
inline `flip_coin` example is six lines.

---

## Part 3 — embedding the library

### 3.0 Three-line hello world (`easyai::Agent`)

If you remember nothing else, remember this:

```cpp
// hello.cpp
#include "easyai/easyai.hpp"

int main() {
    easyai::Agent a("models/qwen2.5-1.5b-instruct-q4_k_m.gguf");
    std::cout << a.ask("What's 2+2?") << "\n";
}
```

`Agent` is the friendly Tier-1 façade.  Construct, ask, print.
Default toolset (datetime + the unified `web` tool) is wired in;
the unified `fs` tool and `bash` stay off until you opt in via
`.sandbox()` or `.allow_bash()`.  Streaming output is one chained
call away:

```cpp
easyai::Agent a("model.gguf");
a.system  ("Be terse.")
 .sandbox ("./workspace")
 .preset  ("creative")
 .on_token([](auto p){ std::cout << p << std::flush; });

a.ask("Read README.md and summarise it.");
```

A remote model works the same way:

```cpp
auto a = easyai::Agent::remote("http://127.0.0.1:8080/v1");
auto a = easyai::Agent::remote("https://api.openai.com/v1",
                               std::getenv("OPENAI_API_KEY"));
```

`Agent` is built on top of `Backend` (3.1.5) which is built on top
of `Engine` (3.1) and `Client` (3.10).  When you need access to the
underlying knobs, `agent.backend()` is the escape hatch — it returns
the materialised `Backend &` so you can reach into `Engine::*` /
`Client::*` setters that `Agent` doesn't surface directly.

CMake:

```cmake
find_package(easyai 0.1 REQUIRED)
add_executable(hello hello.cpp)
target_link_libraries(hello PRIVATE easyai::engine easyai::cli)
```

`Agent` lives in `libeasyai-cli` (because it can transparently
dispatch to either flavour of `Backend`), so link both targets.
If you only need the local engine, drop `easyai::cli` and use
`easyai::Engine` directly (3.1).

### 3.1 Minimal hello

```cpp
// hello.cpp
#include "easyai/easyai.hpp"

int main() {
    easyai::Engine engine;
    engine.model("models/qwen2.5-1.5b-instruct-q4_k_m.gguf")
          .gpu_layers(99)
          .system("Be concise.")
          .on_token([](const std::string & t){ std::cout << t << std::flush; });

    if (!engine.load()) { std::fprintf(stderr, "load failed: %s\n",
                                       engine.last_error().c_str()); return 1; }

    engine.chat("What's 2+2?");
    return 0;
}
```

Add it to `CMakeLists.txt`:

```cmake
add_executable(hello hello.cpp)
target_link_libraries(hello PRIVATE easyai)
```

If your project lives outside this tree and you've installed easyai
(`cmake --install build --prefix /usr/local`), use `find_package`
instead:

```cmake
find_package(easyai 0.1 REQUIRED)
add_executable(hello hello.cpp)
target_link_libraries(hello PRIVATE easyai::engine)
```

`easyai::engine` is the link target for `libeasyai.so` (local llama.cpp
wrapper).  For the OpenAI-protocol client described in 3.9, swap to
`easyai::cli` (or link both side by side).

### 3.1.5 Backend — local OR remote, same shape

If your program needs to handle EITHER a local `-m model.gguf`
flavour OR a remote `--url base` flavour without if-tree
duplication, the abstraction you want is `easyai::Backend`:

```cpp
std::unique_ptr<easyai::Backend> b;
if (!url.empty()) {
    easyai::RemoteBackend::Config rc;
    rc.base_url = url;
    rc.api_key  = api_key;
    rc.with_tools = true;             // dispatch tools locally
    b = std::make_unique<easyai::RemoteBackend>(std::move(rc));
} else {
    easyai::LocalBackend::Config lc;
    lc.model_path = model_path;
    lc.sandbox    = "./workspace";
    lc.allow_bash = true;
    b = std::make_unique<easyai::LocalBackend>(std::move(lc));
}

std::string err;
if (!b->init(err)) { std::cerr << err << "\n"; return 1; }

b->set_system("Be terse.");
auto reply = b->chat("hello?", [](auto p){ std::cout << p << std::flush; });
```

`Backend` is the Tier-3 abstraction `Agent` is built on top of.  Use
it when you want the local↔remote switch but still want to manage the
chat loop yourself, register custom tools, or hook tool callbacks.
The `Config` struct exposes every Engine/Client setting that's
relevant to "configuring an agent" (sampling preset, sandbox,
allow_bash, KV cache controls for local, TLS/timeout for remote).

`LocalBackend` ships in `libeasyai`; `RemoteBackend` in
`libeasyai-cli`.  Linking only the engine library gives you the
local flavour; adding `easyai::cli` adds the remote flavour without
duplicating the abstract base.

### 3.2 Adding a tool

The 6-line shape:

```cpp
engine.add_tool(
    easyai::Tool::builder("today_is")
        .describe("Returns the day of the week.")
        .handle([](const easyai::ToolCall &){
            return easyai::ToolResult::ok("Saturday"); })
        .build());
```

With typed parameters:

```cpp
engine.add_tool(
    easyai::Tool::builder("send_email")
        .describe("Send an email via the company SMTP relay.")
        .param("to",      "string", "Recipient address",  /*required=*/true)
        .param("subject", "string", "Subject line",       true)
        .param("body",    "string", "Plain-text body",    true)
        .param("cc",      "string", "Optional CC address",false)
        .handle([](const easyai::ToolCall & call){
            std::string to, subject, body, cc;
            easyai::args::get_string(call.arguments_json, "to",      to);
            easyai::args::get_string(call.arguments_json, "subject", subject);
            easyai::args::get_string(call.arguments_json, "body",    body);
            easyai::args::get_string(call.arguments_json, "cc",      cc);

            if (to.empty())      return easyai::ToolResult::error("missing 'to'");
            if (subject.empty()) return easyai::ToolResult::error("missing 'subject'");

            // … your real send code …
            return easyai::ToolResult::ok("sent.");
        })
        .build());
```

`Tool::builder` automatically synthesises the JSON schema. If you need
something fancier (nested objects, enums) build the schema string yourself
and use `Tool::make(name, description, schema_json, handler)`.

### 3.2.1 Writing reliable tool descriptions

> **Read this before shipping a tool.** The model never sees your
> handler — it sees only the tool's `name`, `description`, and
> `parameters_json`. Treat that text as the contract; vague or
> under-described tools produce malformed calls or the wrong action,
> and the model will not learn from a stack trace.

Two patterns ship in `src/`. Use whichever fits your tool's shape.

#### Pattern A — single-action tools

The default for a tool that does one thing. Examples in-tree:
`bash` and `datetime` (the standalone single-action tools). Recipe:

1. Open with one sentence: what does this tool do?
2. State Required vs. Optional parameters explicitly.
3. Describe the **output shape** — what does the model see back?
   ("one path per line, sorted", "two lines: UTC + local", etc.)
4. Show one or two concrete example payloads.
5. List error / edge-case behavior (truncation, missing path, regex
   flavor, …).
6. Each `.param()` description leads with `Required.` or `Optional.`,
   then the constraint and default.

Concrete: this is the polished `read_file` description (excerpted from
`src/builtin_tools.cpp`):

```cpp
Tool::builder("read_file")
    .describe(
        "Read a UTF-8 text file from disk and return its contents.\n"
        "\n"
        "The filesystem you see is rooted at `/`; use paths like "
        "`/report.md` or `/docs/spec.md`. Required: path. Optional: "
        "offset, limit (default returns the first 64 KB; pass offset to "
        "page through larger files).\n"
        "\n"
        "Examples:\n"
        "  {path:\"/report.md\"}\n"
        "  {path:\"/docs/spec.md\", offset:65536, limit:65536}\n"
        "\n"
        "Errors return a single-line message starting with `error:`. "
        "Reading a binary file returns the raw bytes — prefer the "
        "dedicated tools or `bash` (e.g. `file <path>`) for those.")
    .param("path",   "string",
           "Required. File path under the sandbox root, e.g. "
           "`/report.md` or `/docs/spec.md`.", true)
    .param("offset", "integer",
           "Optional. Skip this many bytes from the start of the file "
           "before reading. Default 0. Use the previous read's "
           "(offset + bytes_returned) to page forward.", false)
    .param("limit",  "integer",
           "Optional. Maximum bytes to return. Default 65536 (64 KB). "
           "Larger files are truncated; raise this only when you "
           "really need a bigger chunk.", false)
    .handle(...)
    .build();
```

Compare this to the original one-line `"Read a UTF-8 text file..."`:
the model now knows exactly what it gets back, has a paginating
example, and has been told what NOT to use this for.

#### Pattern B — multi-action tools

A single tool that dispatches on a top-level `action` field. Reach for
this when you have N closely-related operations that share state and
parameters: `plan` (add / update / delete / list). Recipe:

1. Open with the purpose sentence + `Pick an action; the parameters
   needed depend on which action you choose. N actions are supported:`
2. One section per action: `action="X"` heading, then Required /
   Optional, then 2–4 example payloads (literal, copy-pasteable).
3. Closing notes: shared semantics (status enum, id format, etc.).
4. Per-property `.description` strings lead with **which actions
   consume them**: `"Used by add / update / delete. ..."`. The model
   maps each parameter to the right action without re-reading the
   description body.

Concrete: this is how the `plan` tool's description is laid out — see
`src/plan.cpp:Plan::tool()`. The closing line — *"The 'items' array
MUST be a real JSON array, not a quoted string."* — is there because
real models repeatedly emitted `"items": "[...]"` in production. Bake
those lessons into the description.

#### Why the rich form matters

Models follow examples more reliably than they parse JSON-schema
constraints. A description that *shows* a valid call is worth ten that
describe the schema in prose. Three concrete failure modes that better
descriptions prevent:

* The model invents a parameter name (`"file"` instead of `"path"`)
  because the description used the word "file" without showing the
  exact key.
* The model omits a required field because the schema marked it
  required but the description didn't repeat that.
* The model mixes shapes from two actions (`{action:"add", id:"1"}`)
  because the description says `add` accepts `text` somewhere but
  doesn't show what an `add` payload looks like end-to-end.

#### Tolerance shims — what to do when models go off-spec anyway

A good description prevents most misfires. The rest are the cost of
running real models on tool calls. The library is built around the
assumption that tool handlers will see imperfect input.

**1. Use the lenient `args::*` helpers.** Don't roll your own JSON probe
in a handler — the shipped helpers already accept the shapes models
actually emit:

| Helper        | Accepts (beyond the spec form) |
|---------------|--------------------------------|
| `get_string`  | `42` (number → `"42"`), `true` / `false` (bool → string) |
| `get_int`     | `"42"` (quoted integer literal) |
| `get_bool`    | `"true"` / `"false"` / `"1"` / `"0"` |
| `get_array`   | `"[{...}]"` (stringified JSON array — unwrapped and re-parsed) |

**2. Infer required fields when the model omits them.** When the schema
requires `action` but the model leaves it out, look at the fields that
*are* present and pick the most likely intent. The pattern from
`src/plan.cpp`:

```cpp
if (action.empty()) {
    if (items present and first item has text)        action = "add";
    else if (items present and first item has status) action = "update";
    else if (items present and first item has id)     action = "delete";
    else if (top-level text is present)               action = "add";
    else if (top-level status is present)             action = "update";
    else if (top-level id is present)                 action = "delete";
    else                                              action = "list";
}
```

For multi-action tools where the same payload could mean different
things, disambiguate using **current state**: in `plan` we check
whether the supplied id already exists — if not, the model is
creating; if so, the model is updating.

**3. Map common synonyms.** Models pick near-miss verbs all the time.

```cpp
if      (action == "create" || action == "append" ||
         action == "insert" || action == "new")    action = "add";
else if (action == "modify" || action == "change" ||
         action == "edit"   || action == "set")    action = "update";
else if (action == "remove" || action == "rm")     action = "delete";
else if (action == "show"   || action == "get"  ||
         action == "view")                         action = "list";
```

**4. Errors that teach.** When you must reject a call, return an error
whose body shows the correct shape *inline*:

```cpp
return ToolResult::error(
    "plan: 'add' needs either text or items. Examples: "
    "{action:\"add\", text:\"my step\"} or "
    "{action:\"add\", items:[{text:\"a\"}, {text:\"b\"}]}. "
    "items must be a real JSON array, not a quoted string.");
```

The model receives a copy-pasteable example for its next call instead
of a cryptic hint.

**5. Coalesce notifications across batched mutations.** When a handler
mutates shared state in a loop (e.g. plan items) and that state has
subscribers (UI, telemetry), use an RAII guard to fold the per-item
callbacks into one fire at scope exit. Otherwise the UI re-renders
once per item:

```cpp
{
    Plan::Batch batch(*self);   // begin batch
    for (const auto & e : items) self->add(...);
}                                // single on_change here
```

The `Plan::Batch` guard is in `easyai/plan.hpp`; the same pattern
applies to any `on_change`-style observable in a tool.

#### Quick checklist before you ship a new tool

- [ ] One-sentence purpose at the top of `describe()`.
- [ ] Required vs. Optional parameters listed explicitly in prose.
- [ ] Output shape described (lines? sorted? errors?).
- [ ] At least one concrete example payload.
- [ ] Per-`.param()` description leads with `Required.`/`Optional.`,
      then constraints, then default.
- [ ] Errors reference the correct shape inline.
- [ ] Lenient `args::*` helpers used (no hand-rolled JSON parsing).
- [ ] If multi-action: action inference + synonym mapping in place.
- [ ] If batching: callbacks coalesced (RAII guard).

### 3.3 Sandboxed filesystem tool

A single unified `fs` tool with eight sub-actions covers every file
operation. Pass a root directory:

```cpp
engine.add_tool(easyai::tools::fs("./workspace"));
```

Sub-actions selected by the `action` parameter: `read`, `write`,
`list`, `glob`, `grep`, `check_path`, `cwd`, `sandbox`. The model
calls them as `fs(action="read", path="report.md")` etc.

Paths sent by the model are anchored to the root by **iterating path
components and dropping any `..`, `.`, or absolute markers** before
joining onto the root.  Total containment by construction — there is
no path the model can construct that escapes.

The model sees a virtual `/`-rooted filesystem (`/report.md`,
`/docs/spec.md`); the real sandbox path is hidden from descriptions
and result messages.

`fs(action="sandbox")` is the model's escape hatch when it does need
the real on-disk path (typing it back in chat, invoking bash with
absolute paths, etc.). It captures the configured root at
registration so the answer is pinned — distinct from
`fs(action="cwd")`, which reports the process's live cwd and can
drift.

### 3.3.1 Toolbelt — register the canonical agent toolset in 3 lines

Instead of hand-rolling the standard tool registration:

```cpp
easyai::cli::Toolbelt()
    .sandbox   ("./workspace")    // adds the unified `fs` tool
    .allow_bash()                  // adds bash; ALSO ensures `fs` is on
    .with_plan (plan)              // adds the plan tool
    .apply     (engine);           // or .apply(client) for the remote variant
```

The Toolbelt always includes `datetime` + the unified `web` tool.
The unified `fs` tool is enabled by **either** `.sandbox()` or
`.allow_bash()` — bash is strictly more permissive than `fs`, so
allowing bash without `fs` is incoherent (the model would fall back
to `cat > file` for ordinary writes). A fresh agent installation
that calls neither still can't expose write or shell.

### 3.3.2 Bash tool — when you need a real shell

```cpp
engine.add_tool(easyai::tools::bash("./workspace"));
engine.max_tool_hops(99999);   // bash flows span many turns
```

`bash` is a `/bin/sh -c` runner. Output (stdout + stderr) is captured
and capped at 50 KB / 2000 lines (head-biased — the truncation marker
says how many lines were dropped); per-command timeout defaults to
120 s, max 600 s (SIGTERM, then SIGKILL +2 s grace). The cwd is pinned
to the root. The optional `description` parameter (5-10 words, active
voice) is shown by the easyai-cli TUI as the running command's title.

This is **NOT** a hardened sandbox — the command runs with your user
privileges. It's appropriate for local single-user agents; for
anything multi-tenant or production, run easyai-server inside a
container / firejail / unprivileged user.

### 3.3.3 `fs(action="cwd")` — anchor relative paths

The unified `fs` tool's `cwd` action returns the absolute path of the
process's current working directory at call time. Pair it with
`--sandbox`: the CLIs and server `chdir` into the sandbox at startup,
so what `fs(action="cwd")` reports is exactly the directory `bash`
operates inside, and the same root every other `fs` action resolves
its RELATIVE paths against. Models that don't already know the path
should call it once at the start of a task; for any subsequent file
op, relative paths just work.

The `Toolbelt` adds the unified `fs` tool automatically when
`allow_fs` or `allow_bash` is on; the `cwd` action ships with it.

### 3.3.4 External tools — operator-defined commands via JSON manifests

> **The authoritative guide is [`EXTERNAL_TOOLS.md`](EXTERNAL_TOOLS.md)
> at the repo root.** It covers quickstart, ten recipes, anti-patterns,
> corner cases, sanity warnings, the collaboration workflow, and full
> troubleshooting. The sub-sections below are a quick reference; refer
> to that document when actually writing or reviewing manifests.

### 3.3.4a Schema reference

For tools that wrap an existing CLI binary (`uname`, `pgrep`, `git`,
internal scripts, etc.) you declare them in JSON manifest files
inside a directory. The `--external-tools DIR` flag is supported by
`easyai-local`, `easyai-cli`, and `easyai-server`. The directory is
scanned for files matching `EASYAI-<name>.tools` (top-level, exact,
case-sensitive); per-file fault isolation means a syntax error in
one file does NOT prevent the others from loading.

From C++:

```cpp
// Directory model (recommended):
auto loaded = easyai::load_external_tools_from_dir(dir, /*reserved=*/{});
for (const auto & e : loaded.errors)   std::fprintf(stderr, "error: %s\n", e.c_str());
for (const auto & w : loaded.warnings) std::fprintf(stderr, "warn:  %s\n", w.c_str());
for (auto & t : loaded.tools) engine.add_tool(std::move(t));

// Single-file (for unit tests / programmatic use):
auto one = easyai::load_external_tools_from_json(path, /*reserved=*/{});
if (!one.error.empty()) { std::fprintf(stderr, "%s\n", one.error.c_str()); return 1; }
for (auto & t : one.tools) engine.add_tool(std::move(t));
```

Manifest schema (one entry — see `examples/EASYAI-example.tools` and
`EXTERNAL_TOOLS.md` for more):

```json
{
  "version": 1,
  "tools": [
    {
      "name": "list_processes",
      "description": "List running processes whose name matches a regex pattern.",
      "command": "/usr/bin/pgrep",
      "argv": ["-a", "{pattern}"],
      "parameters": {
        "type": "object",
        "properties": {
          "pattern": { "type": "string", "description": "Regex." }
        },
        "required": ["pattern"]
      },
      "timeout_ms": 5000,
      "max_output_bytes": 65536,
      "cwd": "$SANDBOX",
      "env_passthrough": ["PATH"],
      "stderr": "discard",
      "treat_nonzero_exit_as_error": false
    }
  ]
}
```

**Field-by-field reference**

| Field | Required | Notes |
| --- | --- | --- |
| `name` | yes | `^[a-zA-Z][a-zA-Z0-9_]{0,63}$`. Must not collide with built-ins (`bash`, `read_file`, …) or already-registered tools. |
| `description` | yes | Plain-English text for the model. 1..4096 chars. The model uses this to decide *when* to call your tool, so write it well. |
| `command` | yes | **Absolute** path to a regular, executable file. Relative names are rejected at load (no PATH search → no PATH-hijack risk). |
| `argv` | yes | Array of strings. Each element is either a literal (no `{` or `}`) or exactly `"{paramname}"`. Embedded placeholders (`"--flag={x}"`) are rejected — split into two elements (`["--flag", "{x}"]`) instead. |
| `parameters` | optional | JSON-Schema-shaped: `{type:"object", properties:{...}, required:[...]}`. Types accepted: `string`, `integer`, `number`, `boolean`. |
| `timeout_ms` | optional | Default 10000. Clamped to [100, 300000]. |
| `max_output_bytes` | optional | Default 65536. Clamped to [1024, 4 MiB]. Excess output is silently discarded; the response notes the truncation. |
| `cwd` | optional | Either an absolute path or the magic token `"$SANDBOX"` which resolves to the process's CWD at load time. Default: `"$SANDBOX"`. |
| `env_passthrough` | optional | Allowlist of parent-process env vars to inherit. **Default empty** — the subprocess gets a clean env. Add `"PATH"`, `"HOME"`, etc. only when the wrapped command needs them. |
| `stderr` | optional | `"merge"` (default) or `"discard"`. |
| `treat_nonzero_exit_as_error` | optional | Default `true`. Set `false` for tools whose non-zero exit is informational (`pgrep` returns 1 when nothing matches). |

**Security guarantees** — these are enforced, not aspirational:

1. **No shell.** The runner uses `fork` + `execve` with an argv array.
   The model's argument never passes through a shell parser, so
   quoting / `;` / backticks / `$(…)` cannot escape its argv slot.
2. **Absolute command path.** Validated at load (regular file +
   executable bit). No PATH lookup, no PATH-hijack.
3. **Whole-element placeholders only.** A model argument fills exactly
   one argv element; it can't be concatenated into a literal.
4. **Schema-validated arguments.** Type errors are surfaced as a
   `ToolResult::error` *before* anything is spawned. Required-but-
   missing arguments are rejected.
5. **Hard caps.** Manifest size (1 MiB), tools per manifest (128),
   params per tool (32), env passthrough size (16), argv elements
   (256), per-arg bytes (4 KiB). Each cap closes a class of DoS.
6. **Clean env by default.** Only listed `env_passthrough` vars
   inherit. `LD_PRELOAD`, `PATH`, etc. don't leak in unless asked.
7. **Closed stdin.** No way to feed the subprocess from the model.
8. **Process-group timeout.** SIGTERM to the group on `timeout_ms`,
   SIGKILL after a 1 s grace — kills any grandchildren the command
   spawned, not just the top-level process.
9. **Inherited fds closed.** All fds ≥ 3 are closed in the child
   before exec, so the agent's HTTP transport / log files / database
   handles do not leak into the spawned command.

The manifest is the operator's deploy artefact — treat it like a
sudoers file. Anyone who can write it can run arbitrary commands as
the agent's user.

**Argv-injection via leading dashes.** The library guarantees that a
model's argument fills exactly one argv slot — quoting and shell
metacharacters can't escape it. What the library *cannot* know is
whether the wrapped command treats a value starting with `-` as a
flag. If your tool wraps a binary that accepts options
(`pgrep "-V"`, `grep "-r"`, `find "-delete"`, …), insert the
end-of-options sentinel `"--"` as a literal argv element BEFORE the
placeholder:

```json
"argv": ["-a", "--", "{pattern}"]
```

GNU coreutils, util-linux, git, grep, ripgrep, find, and pgrep all
honour `--`. Integer/number/boolean parameters are immune (they're
not strings) and don't need this. See
`examples/EASYAI-example.tools` and `EXTERNAL_TOOLS.md` for the
pattern.

```sh
# enable from the CLIs (DIR contains EASYAI-*.tools files)
easyai-local --sandbox ./work --external-tools ./tools.d
easyai-cli   --sandbox ./work --external-tools ./tools.d --url http://...
easyai-server -m model.gguf --sandbox /srv/agent --external-tools /etc/easyai/external-tools
```

The default install ships `/etc/easyai/external-tools/` empty;
operators drop `EASYAI-<name>.tools` files in to add tools. The
systemd unit always passes `--external-tools` so a restart picks up
new files.

### 3.3.5 External tools — recipes, corner cases, best practices

This section is the practical companion to §3.3.4. It assumes you've
read the schema and security model and now want to actually ship a
manifest.

#### Recipes

**Recipe 1 — read-only system inspector (no parameters).**

Useful for "give me the model the ability to talk about the host" without
any attack surface beyond reading public OS state.

```json
{
  "name": "host_status",
  "description": "Return uptime, load average, kernel name, and free memory of the host. Use when the user asks 'how is the box doing'.",
  "command": "/usr/bin/uptime",
  "argv": [],
  "parameters": { "type": "object", "properties": {} },
  "timeout_ms": 2000,
  "max_output_bytes": 4096,
  "cwd": "$SANDBOX",
  "env_passthrough": [],
  "stderr": "discard"
}
```

Teaches: zero-parameter tool, conservative `timeout_ms` and
`max_output_bytes`, empty env, `stderr: "discard"` keeps the model's
context clean of harmless noise.

**Recipe 2 — code search via ripgrep (with `--` sentinel).**

```json
{
  "name": "code_search",
  "description": "Search the project tree for a literal string or regex. Returns file:line:match. Limit yourself to specific patterns — broad searches are slow and noisy.",
  "command": "/usr/bin/rg",
  "argv": [
    "--no-heading",
    "--line-number",
    "--max-count", "100",
    "--",
    "{pattern}",
    "."
  ],
  "parameters": {
    "type": "object",
    "properties": {
      "pattern": {
        "type": "string",
        "description": "Literal string or regex to search for. Quote multi-word phrases."
      }
    },
    "required": ["pattern"]
  },
  "timeout_ms": 15000,
  "max_output_bytes": 262144,
  "cwd": "$SANDBOX",
  "env_passthrough": ["HOME"],
  "stderr": "merge",
  "treat_nonzero_exit_as_error": false
}
```

Teaches: `"--"` sentinel before the string placeholder so a model
prompt of `pattern = "-r"` or `"--type=cpp"` is interpreted as a
search pattern, not a flag. `treat_nonzero_exit_as_error: false`
because rg returns 1 when the pattern is not found — that's
informational, not a failure.

**Recipe 3 — JSON filter via jq (no shell escaping headaches).**

```json
{
  "name": "json_filter",
  "description": "Apply a jq expression to an existing JSON file in the sandbox. The 'filter' argument is the jq expression (e.g. '.users[] | .email').",
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
  "timeout_ms": 5000,
  "max_output_bytes": 65536,
  "cwd": "$SANDBOX"
}
```

Teaches: complex filter strings with quotes / pipes / `$()` are passed
through as a single argv element — no shell, no escaping. The library
guarantees `{filter}` fills exactly one argv slot regardless of its
contents.

**Recipe 4 — internal CLI with a credential.**

```json
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
  "timeout_ms": 10000,
  "max_output_bytes": 32768,
  "cwd": "$SANDBOX",
  "env_passthrough": ["DEPLOY_TOKEN", "HOME", "PATH"]
}
```

Teaches: opt-in env passthrough for credentials. `DEPLOY_TOKEN` is
read from the parent's environment at every call (so rotating it in
the systemd unit re-reads on next call without a restart). Without
the allowlist the subprocess gets a clean env.

**Recipe 5 — Python one-liner (no shell, but execve still works).**

```json
{
  "name": "python_eval",
  "description": "Evaluate a SHORT Python expression and return its repr. Single line, no imports beyond math/datetime/json. Use for arithmetic / date math the model would otherwise get wrong.",
  "command": "/usr/bin/python3",
  "argv": ["-c", "{expr}"],
  "parameters": {
    "type": "object",
    "properties": {
      "expr": { "type": "string", "description": "Python expression. Output goes to stdout via print(repr(...)). Example: 'print(repr(__import__(\"datetime\").datetime.now()))'." }
    },
    "required": ["expr"]
  },
  "timeout_ms": 3000,
  "max_output_bytes": 16384,
  "cwd": "$SANDBOX",
  "env_passthrough": []
}
```

Teaches: `-c "{expr}"` works because `{expr}` is one argv element.
`python3 -c 'print(1)'` and `python3 -c 'print(1); __import__("os").system("rm -rf /")'` reach
python the same way; whether to allow it is your policy decision
(this is essentially `bash` with a Python-shaped surface area —
narrower, but still arbitrary code execution).

**Recipe 6 — git porcelain (integer parameter, no leading-dash worry).**

```json
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
  "timeout_ms": 8000,
  "max_output_bytes": 131072,
  "cwd": "$SANDBOX",
  "env_passthrough": ["HOME", "PATH"]
}
```

Teaches: integer parameters skip the leading-dash worry — `1` cannot
be parsed as a flag. `HOME` is needed for `~/.gitconfig`; `PATH` is
needed because git invokes sub-commands like `git-log` via PATH.

**Recipe 7 — long-running task with cooperative timeout.**

```json
{
  "name": "build_project",
  "description": "Run the build script in the sandbox. Returns build log. May take up to 5 minutes.",
  "command": "/usr/bin/make",
  "argv": ["-j", "{jobs}"],
  "parameters": {
    "type": "object",
    "properties": {
      "jobs": { "type": "integer", "description": "Parallel jobs (1..16)." }
    },
    "required": ["jobs"]
  },
  "timeout_ms": 300000,
  "max_output_bytes": 4194304,
  "cwd": "$SANDBOX",
  "env_passthrough": ["HOME", "PATH", "CC", "CXX"],
  "stderr": "merge"
}
```

Teaches: `timeout_ms` at the 5-min ceiling, `max_output_bytes` at the
4-MiB ceiling. On timeout the runner sends SIGTERM to the process
group (so child compilers die too), then SIGKILL after 1 s. Make
gets one chance to flush — typical build systems handle this fine.

#### Corner cases

| Situation | What happens |
| --- | --- |
| Binary doesn't exist when manifest loads | Load fails with a precise error. Agent doesn't start. |
| Binary removed AFTER successful load | Tool call returns `exit=127  external_tool: execve failed`. |
| Binary on a stalled NFS mount | Call blocks until `timeout_ms`, then SIGTERM/SIGKILL the way a hung child gets killed anywhere. |
| Required parameter missing in model's call | `ToolResult::error` before `fork()`. |
| Optional parameter missing | Empty string substituted into the argv slot. (For tools that distinguish "" from "not present", make the param required.) |
| Extra keys in the model's JSON arguments | Silently ignored. |
| Two manifest entries with the same name | Load fails with `duplicate tool name`. |
| Manifest entry shadows a built-in (e.g. names itself `bash`) | Load fails with collision error. |
| `argv` is `[]` (empty array) | Valid. Command runs with `argv[0] = basename(command)` and nothing else. |
| Model sends `"true"` (string) for a `boolean` parameter | Validation rejects: `expected boolean`. |
| Model sends `1.5` for an `integer` parameter | Validation rejects: `expected integer`. nlohmann's `is_number_integer()` is strict. |
| Model sends `NaN` / `Infinity` for a `number` parameter | Rejected: `must be a finite number`. |
| Manifest is edited while the agent is running | No effect — loaded once at startup. Restart to pick up changes. |
| `cwd: "$SANDBOX"` but the agent didn't `chdir` | `$SANDBOX` is captured at LOAD time. The manifest's `cwd` resolves to whatever the process CWD was when `load_external_tools_from_json` ran. The CLIs `chdir(--sandbox)` BEFORE loading, so `$SANDBOX` ≡ `--sandbox`. |
| Want `LD_PRELOAD` to leak through | Can't, by design. Listed env vars are validated against `^[a-zA-Z][a-zA-Z0-9_]{0,63}$` — `LD_PRELOAD` matches the pattern, so technically allowed. But you have to opt in explicitly. |
| `stderr: "discard"` and the command writes to stderr | Bytes go to `/dev/null`. Model never sees them. |
| Command writes binary / non-UTF-8 bytes to stdout | Captured as-is. Returned as a `std::string` to the model — it'll see invalid bytes. Best-effort. |
| Command spawns a daemon (forks, parent exits) | Parent reaped immediately; daemon survives but inherits stdout=pipe with no reader; first write gets `SIGPIPE` and dies. Don't use this design for daemonising commands. |
| Subprocess takes 3 seconds; `timeout_ms = 5000` | Runs to completion, output captured normally. |
| Subprocess outputs more than `max_output_bytes` | Excess is silently discarded (drained, not buffered). Response notes `[truncated at N bytes]`. The child stays unblocked. |
| Subprocess writes 1 KB then sleeps 30 minutes | Output captured immediately; SIGTERM on `timeout_ms`, SIGKILL after 1 s grace. |
| Two concurrent calls to the same tool | Each `fork`s its own subprocess. No shared state on the library side. The wrapped command is responsible for its own concurrency. |
| Agent (parent) crashes mid-call | On Linux, `PR_SET_PDEATHSIG(SIGKILL)` ensures the subprocess dies with the agent. Otherwise it'd reparent to PID 1 and survive. |
| Manifest path is `/dev/zero` or a directory | `slurp()` rejects with `manifest is not a regular file`. |
| Manifest is 2 MB | Rejected: `manifest exceeds 1048576 bytes`. |

#### Best practices

**DO:**

- Use absolute paths in `command` (the loader requires it; don't fight it).
- Insert `"--"` literal element before any string placeholder for binaries that accept options (rg, grep, find, pgrep, kill, …).
- Set `treat_nonzero_exit_as_error: false` for tools where non-zero is informational (`pgrep`, `grep`, `diff`).
- Match `timeout_ms` and `max_output_bytes` to the worst plausible case for that tool — not a global default. Short-running status tools should have small caps so a hung command doesn't waste the 5-min ceiling.
- Use `env_passthrough` to pass exactly the env vars the wrapped command needs (`HOME`, `PATH`, sometimes `LANG`, `TZ`, a credential token). Default `[]` and grow only when something fails.
- Spend real time on `description` text — that string is how the model picks WHICH tool to call. Mention edge cases ("returns empty when nothing matches"), expected use ("call this AFTER `web(action=\"search\")`"), and units ("returns kilobytes").
- Name parameters to match the wrapped CLI's vocabulary (`pattern` if the binary calls it pattern, not `regex`).
- Group related tools in one manifest — the `--tools` allowlist applies after load, so a single big manifest is fine for the operator.
- Validate the manifest dir before deploy: `easyai-local --no-tools --external-tools ./tools.d` (no model call, just load — exits cleanly if valid, errors emitted to stderr if not).
- Run easyai-server as a dedicated unprivileged user when external tools are in play — the security guarantees stop shell injection, not "runs with your full uid".

**DON'T:**

- Don't put a placeholder inside a larger string — split into separate elements. `["--flag={x}"]` is rejected at load. Use `["--flag", "{x}"]`.
- Don't reach for the `bash` builtin to wrap a command you could declare in the manifest. The manifest gives you fork+execve safety, schema validation, hard timeouts, fd hygiene, env hygiene. `bash` gives you none of those.
- Don't rely on the agent's CWD matching what you think it is — be explicit with `cwd: "$SANDBOX"` or an absolute path.
- Don't put credentials in `argv` — they end up in `/proc/<pid>/cmdline` (world-readable on most distros). Use `env_passthrough` instead.
- Don't declare more than ~10–15 tools in a single manifest unless the model is large. Every tool's name + description + schema is serialised into the prompt on every turn — too many tools = too much token budget eaten before the user's actual question.
- Don't use the manifest for tools that need shared in-process state (a database connection pool, an HTTP client with a session cookie). Those are C++ tools — `Tool::builder().handle(...)`.
- Don't expose interactive tools (`vim`, `nano`, `more`) — stdin is closed; they'll behave strangely or hang until timeout.
- Don't expose tools that fork-and-exit (daemonising launchers) — the daemon's stdout is the now-orphaned pipe; first write SIGPIPEs.

#### Validating a manifest before deploy

```sh
# Loads the manifest, prints the resulting tool list, exits.
# Any load error fails the command — wire it into your CI.
easyai-local --no-tools --external-tools ./tools.d --print-models 2>&1 \
  | grep -E "(loaded|error)"
```

You can also unit-test a manifest from C++:

```cpp
auto loaded = easyai::load_external_tools_from_json("mytools.json", {});
assert(loaded.error.empty() && "manifest invalid");
assert(loaded.tools.size() == kExpectedToolCount);
for (const auto & t : loaded.tools) {
    // sanity-check the auto-generated parameter schema
    auto schema = nlohmann::json::parse(t.parameters_json);
    assert(schema["type"] == "object");
}
```

#### What this is NOT

- **Not a sandbox.** External tools run with the agent's full uid/gid.
  Network, FS, signals — everything the agent can do, the tool can
  do. The library closes inheritance leaks; it doesn't isolate.
- **Not a process supervisor.** No restart-on-failure, no PID file,
  no log rotation. Each call is a one-shot fork+exec.
- **Not async.** A tool call blocks the agent loop until it returns
  or times out. Latency budget = timeout_ms.
- **Not stateful.** Each call gets a fresh subprocess. If you need
  state, write a C++ tool with a captured `std::shared_ptr` to a
  state object.

### 3.3.6 knowledge — persistent registry / long-term memory

> The authoritative guide is [`RAG.md`](RAG.md). The summary below
> is a quick reference.

The knowledge tools give the agent a tool surface for remembering
things across sessions. Under the hood they use **a passive RAG
technique** — keyword-indexed Markdown files the agent saves and
searches itself, with no embedding model or vector store. Seven
independent tools:

```cpp
for (auto & t : easyai::tools::knowledge_split_tools("/var/lib/easyai/rag"))
    engine.add_tool(std::move(t));
// knowledge_save      keywords[], content, fix?
// knowledge_append    keywords[], content
// search_knowledge    keywords[], max_results=10
// knowledge_load      keywords[][1..4]
// knowledge_list      prefix?, max=50
// knowledge_delete    keywords[]
// keywords_knowledge  min_count=1, max=200
```

Or via the `--memory <dir>` flag in `easyai-server`, `easyai-cli`, and
`easyai-local` (the legacy `--RAG` flag is still accepted as an
alias). The systemd-installed server passes
`--memory /var/lib/easyai/rag` by default.

Keywords ARE the identifier — there is no separate `title` parameter.
Sorted keywords joined by `_` become the filename.
`"python async"` produces `async_python.md`. Immutable entries use
the `fix-` prefix (e.g. `fix-async_python.md`).

Each entry is one Markdown file in the configured directory:

```
keywords: async, python

Body content here. Free-form UTF-8 up to 256 KB.
Operator-readable, hand-editable, grep-able.
```

Constraints: keywords match `^[A-Za-z0-9._+-]+$` (≤ 32
bytes), 1..8 keywords per entry, content ≤ 256 KiB, max 4 loads per
call.

The model is encouraged (in the tool descriptions) to save
aggressively, search before assuming it doesn't know something, and
delete stale entries to keep the index sharp. See `RAG.md` for the
full workflow including document ingestion, the positive cycle, and
the operator's audit / backup recipes.

### 3.4 Streaming token output

Just register `on_token`:

```cpp
engine.on_token([](const std::string & piece){
    std::cout << piece << std::flush;
});
```

Pieces are *substrings of UTF-8 tokens*. Most pieces are full tokens, but
multi-byte characters can split across pieces — buffer if you need
character-precise rendering.

### 3.5 Listening for tool calls (telemetry / UI hooks)

```cpp
engine.on_tool([](const easyai::ToolCall & c, const easyai::ToolResult & r){
    log_metric("tool_call", { {"name", c.name}, {"is_error", r.is_error} });
});
```

The callback fires after every dispatched tool, success or failure.

### 3.6 Resetting between conversations

```cpp
engine.clear_history();   // wipes history + KV cache + sampler state
engine.system("You are now a different assistant.");
```

If you want to programmatically replay a conversation, e.g. when restoring
from a database:

```cpp
engine.replace_history({
    {"system",    "You are a helpful assistant."},
    {"user",      "What's the capital of Brazil?"},
    {"assistant", "Brasília."},
    {"user",      "And of France?"},
});
auto reply = engine.chat("");  // generate the next assistant turn
```

### 3.7 Switching presets at runtime

```cpp
const easyai::Preset * p = easyai::find_preset("creative");
if (p) engine.set_sampling(p->temperature, p->top_p, p->top_k, p->min_p);
```

Or to honour a chat-line command from your own UI:

```cpp
auto pr = easyai::parse_preset(user_line);
if (!pr.applied.empty()) {
    engine.set_sampling(pr.temperature, pr.top_p, pr.top_k, pr.min_p);
    user_line = user_line.substr(pr.consumed);   // strip prefix
}
engine.chat(user_line);
```

### 3.8 Recipe book — write your first tools, step by step

> **In this chapter, you'll learn to:**
> * understand what an "AI tool" really is (it's just a C++ function!)
> * write a tool that returns today's date
> * write a tool that fetches live weather from the internet
> * give your agent both tools and watch it answer real questions
> * recognise when to reach for the more advanced building blocks
>
> **You don't need to know:** llama.cpp, JSON Schema, Jinja templates,
> or anything about how language models work under the hood.

This is the chapter every other chapter has been pointing at.  When
people ask *"what's so cool about easyai?"* — this is the answer.
You're going to give a small AI model two new abilities in about
fifty lines of code, and at the end you'll have a working agent that
genuinely reaches out to the internet on your behalf.

There's a finished version of everything below in
**`examples/recipes.cpp`**.  Build it now so you can compare:

```bash
cmake --build build -j --target easyai-recipes
```

We'll come back to that binary at the end and run it.

---

#### Chapter opener — what *is* a tool, anyway?

Imagine you hire a brilliant intern.  They're fast, polite, and they
know almost everything — but they joined the company yesterday so
they don't know your customer database, they don't have your VPN, and
they can't see today's calendar.  How do you make them useful?

You give them a phone book of internal services and you tell them:
*"if anyone asks about a customer, call this number; if they ask about
billing, call that one."*

That's exactly what a tool is to an AI model.  Each tool you register
is a phone-book entry.  The model gets to read three things about it:

| Field          | What goes here                                              | Read by    |
|----------------|-------------------------------------------------------------|------------|
| **name**       | A short identifier — e.g. `today_is`, `weather`            | the model  |
| **description**| One sentence: *what does this do, when should I use it?*   | the model  |
| **handler**    | A normal C++ function that gets called for you             | easyai     |

When the model decides "I should use the weather tool", easyai catches
that intent, runs your handler with whatever arguments the model picked,
and feeds the result back so the model can finish its answer.

The whole dance, drawn out:

```
   user                   model                   easyai            your handler
    │                        │                       │                     │
    │  "What's the weather   │                       │                     │
    │   in São Paulo?"  ───▶ │                       │                     │
    │                        │  "I'll call            │                     │
    │                        │   weather(city=…)" ──▶│                     │
    │                        │                       │  weather(...)  ───▶ │
    │                        │                       │                     │ … HTTP call …
    │                        │                       │ ◀──── "São Paulo:   │
    │                        │ ◀──── tool result ────│       ⛅ +24°C"     │
    │                        │                       │                     │
    │  "São Paulo is a       │                       │                     │
    │   pleasant 24°C…" ◀────│                       │                     │
```

You write the handler.  Everything else is automatic.

---

#### Recipe 1 — your first tool: "what is today's date?"

Most tiny AI models have **no idea what today's date is**.  Their
training data ended months (sometimes years) ago.  Ask Qwen2.5-1.5B
"what's today's date?" and you'll usually get a confident-sounding
hallucination.

Let's fix that with eight lines.

##### Type this

```cpp
easyai::Tool today_is() {
    return easyai::Tool::builder("today_is")
        .describe("Returns today's date in ISO-8601 format (YYYY-MM-DD, UTC).")
        .handle([](const easyai::ToolCall &) {
            auto now = std::chrono::system_clock::now();
            auto t   = std::chrono::system_clock::to_time_t(now);
            char buf[16];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::gmtime(&t));
            return easyai::ToolResult::ok(buf);
        })
        .build();
}
```

##### What just happened?

Read it line by line — there's nothing magical:

1. **`Tool::builder("today_is")`** — pick a name for the tool.  Use
   `snake_case`.  This is the name the model will speak when it wants
   to use the tool.
2. **`.describe(...)`** — write a one-line description that you'd give
   a smart intern.  *"Returns today's date in ISO-8601 format"* is
   crystal-clear.  *"Useful for date stuff"* would not be.
3. **`.handle(...)`** — the C++ that does the real work.  Here it's a
   little lambda that calls the standard library.  No llama.cpp, no
   JSON, no AI-specific API.
4. **`ToolResult::ok(buf)`** — pack the string into a success result.
   Whatever you pass here is what the model sees back as the tool's
   reply.
5. **`.build()`** — turn the recipe into the actual `Tool` object.

> **Tip.**  The description is the *only* hint the model has about
> when to call your tool.  Write it for an LLM, not for your IDE.
> Be specific, give an example output, mention units.

##### Hand it to the engine

```cpp
easyai::Engine engine;
engine.model("models/qwen2.5-1.5b-instruct-q4_k_m.gguf")
      .add_tool(today_is())     // ← your new tool
      .load();
engine.chat("What's the date today?");
```

That's it.  Eight lines for the tool plus three for the wiring, and
your agent now has reliable date access.

> **Try it.**  Wrap the snippet above in a `main()`, link against
> `easyai`, build, and run.  Or just look at `examples/recipes.cpp` —
> it's the same code, ready to go.

---

#### Recipe 2 — talking to the internet: a "weather" tool

Today's date is fun, but the real point of giving an AI tools is so
it can reach out to systems you control: your database, your APIs,
your filesystem, the internet.

Let's write a `weather` tool.  We'll use **wttr.in** — a free,
no-signup service that takes a city name and replies in plain text:

```
$ curl 'https://wttr.in/Sao Paulo?format=3'
São Paulo: ⛅ +24°C
```

That's the whole API.  Our job is to wrap that in a tool.

We're going to do this in four small steps so nothing feels like a
leap.

##### Step 1 — Declare the input parameter

This time the tool needs a parameter (`city`).  The builder makes
that one extra line:

```cpp
easyai::Tool::builder("weather")
    .describe("Returns the current weather for a city.  Backed by wttr.in "
              "— free, no API key, plain-text reply.")
    .param("city", "string",
           "City name, e.g. 'Berlin' or 'Sao Paulo'.  Required.",
           /*required=*/true)
```

`param(name, type, description, required)` is all you ever need.
The valid `type` values are:

| `type`      | C++ in your handler             |
|-------------|---------------------------------|
| `"string"`  | `std::string` via `args::get_string_or(...)` |
| `"integer"` | `long long` via `args::get_int_or(...)` |
| `"number"`  | `double` via `args::get_double_or(...)` |
| `"boolean"` | `bool` via `args::get_bool_or(...)` |
| `"array"`   | parse the JSON yourself         |
| `"object"`  | parse the JSON yourself         |

> **Heads-up.**  Tiny models occasionally forget required parameters.
> Always validate inside your handler — see step 3.

##### Step 2 — Read the parameter inside the handler

The model packs the arguments into a JSON blob (e.g.
`{"city":"Sao Paulo"}`).  easyai gives you a tiny scanner so you
don't need a JSON library:

```cpp
.handle([](const easyai::ToolCall & call) {
    std::string city = easyai::args::get_string_or(
        call.arguments_json, "city", "");
    if (city.empty()) {
        return easyai::ToolResult::error("missing 'city' argument");
    }
    ...
```

That **one line** with `get_string_or` replaces the four lines of
"declare, get, check, default" pattern you'd write in plain C++.

The full helper menu:

| Helper                                              | Returns…                          |
|-----------------------------------------------------|-----------------------------------|
| `args::get_string_or(json, key, default)`           | the value, or your default        |
| `args::get_int_or   (json, key, default)`           | same idea, `long long`            |
| `args::get_double_or(json, key, default)`           | same idea, `double`               |
| `args::get_bool_or  (json, key, default)`           | same idea, `bool`                 |
| `args::has(json, key)`                              | `bool` — did the model fill it in?|

(There's an older `bool args::get_string(json, key, &out)` form
that's still around when you need to tell *"absent"* apart from
*"present but empty"*.)

##### Step 3 — Make the actual call

Anything you can do in C++ goes here: hit a REST API, query SQLite,
shell out to a Python script, send a Slack message, ring a bell on
the desk next to you.  In our case it's an HTTP GET, and libcurl
takes about ten lines:

```cpp
CURL * h = curl_easy_init();
char * escaped = curl_easy_escape(h, city.c_str(), 0);   // URL-safe
std::string url = "https://wttr.in/";
url += escaped ? escaped : city.c_str();
url += "?format=3";                                       // one-line summary
if (escaped) curl_free(escaped);

std::string body;
curl_easy_setopt(h, CURLOPT_URL,            url.c_str());
curl_easy_setopt(h, CURLOPT_USERAGENT,      "easyai-recipes/0.1");
curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
curl_easy_setopt(h, CURLOPT_TIMEOUT,        15L);
curl_easy_setopt(h, CURLOPT_WRITEFUNCTION,  capture_body);   // see recipes.cpp
curl_easy_setopt(h, CURLOPT_WRITEDATA,      &body);
CURLcode rc   = curl_easy_perform(h);
long     code = 0;
curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
curl_easy_cleanup(h);
```

> **Don't panic** at the libcurl block — copy and paste it into any
> tool that needs the network and tweak the URL.  The boilerplate is
> the same every time.  Half of your future tools will be exactly
> this shape.

##### Step 4 — Return success or a typed error

```cpp
if (rc != CURLE_OK) {
    return easyai::ToolResult::error(
        std::string("HTTP transport error: ") + curl_easy_strerror(rc));
}
if (code >= 400) {
    return easyai::ToolResult::error(
        "wttr.in returned HTTP " + std::to_string(code));
}
return easyai::ToolResult::ok(body);
```

Two return flavours, only:

* **`ToolResult::ok(text)`** — the model sees `text` as the reply.
* **`ToolResult::error(msg)`** — easyai marks the message as a failure
  so the model knows to recover (try a different tool, ask the user,
  apologise).

> **Why this matters.**  When a tool errors, well-trained models *do
> the right thing*.  They don't pretend the call worked.  They tell
> the user *"the weather service is unavailable, want me to try
> again later?"*  Use `error` for anything that isn't a success.

---

#### Putting it together — your first running agent

The whole `main()` is in `examples/recipes.cpp`:

```cpp
easyai::Engine engine;
engine.model(model_path)
      .context(4096)
      .gpu_layers(99)
      .system("You are a concise assistant.  Use tools whenever they help.")
      .add_tool(today_is())
      .add_tool(weather())
      .on_token([](const std::string & p){ std::cout << p << std::flush; });

if (!engine.load()) {
    std::fprintf(stderr, "load failed: %s\n", engine.last_error().c_str());
    return 1;
}
engine.chat("What's today's date, and what's the weather in Sao Paulo right now?");
```

Run it:

```
$ ./build/easyai-recipes models/qwen2.5-1.5b-instruct-q4_k_m.gguf
[recipes] backend=Metal  ctx=4096  tools=2

Today is 2026-04-26.  São Paulo currently shows ⛅ +24°C, so light
clothes with a thin layer for the evening should be perfect.
```

##### What just happened?

* The model received your one English sentence.
* It noticed it didn't know the date — so it called `today_is`.
* It noticed it didn't know the weather — so it called `weather`
  with `{"city":"Sao Paulo"}`.
* easyai ran both your handlers, captured both replies, and fed
  them back into the model.
* The model wove them into one fluent answer.

You wrote two C++ functions.  easyai did the rest.

---

#### Going further (when you're ready)

This sub-section is a quick tour of doors you can walk through next.
Each is fully optional.

##### More than one parameter

Mix and match types, mark some as optional, use `_or` helpers to
thread defaults right through:

```cpp
easyai::Tool::builder("send_alert")
    .describe("Push a one-line alert to the on-call channel.")
    .param("text",       "string",  "Message body.  Required.",                   true)
    .param("severity",   "string",  "info | warning | critical.  Default 'info'.", false)
    .param("notify_now", "boolean", "Page on-call immediately?",                   false)
    .handle([](const easyai::ToolCall & call) {
        auto text     = easyai::args::get_string_or(call.arguments_json, "text", "");
        auto severity = easyai::args::get_string_or(call.arguments_json, "severity", "info");
        auto pageNow  = easyai::args::get_bool_or  (call.arguments_json, "notify_now", false);

        if (text.empty()) return easyai::ToolResult::error("missing 'text'");
        // … your real send code …
        return easyai::ToolResult::ok("alert dispatched.");
    })
    .build();
```

##### When the builder isn't enough

The builder makes a flat JSON-Schema (just `properties` + `required`).
For 95% of tools that's plenty.  Need enums, nested objects, arrays?
Drop down to **`Tool::make()`** with a hand-written schema:

```cpp
engine.add_tool(easyai::Tool::make(
    "create_ticket",
    "Open a Jira ticket.",
    R"({
      "type": "object",
      "properties": {
        "project":  { "type": "string" },
        "summary":  { "type": "string" },
        "priority": { "type": "string", "enum": ["P0","P1","P2","P3"] },
        "labels":   { "type": "array", "items": { "type": "string" } }
      },
      "required": ["project","summary"]
    })",
    [](const easyai::ToolCall & call) {
        // parse with nlohmann::json (vendored at ../llama.cpp/vendor) …
        return easyai::ToolResult::ok("JRA-1234");
    }));
```

Same engine, same callback shape, full schema control.

##### Where to read more

* [3.2.1 Writing reliable tool descriptions](#321-writing-reliable-tool-descriptions)
  — the contract with the model. Single-action vs. multi-action
  patterns, the per-`param()` description style used in-tree, and the
  tolerance shims (synonym mapping, action inference, error messages
  that teach) that keep tools robust when the model goes off-spec.
* **`src/builtin_tools.cpp`** — the unified `web` and `fs` tools and
  `bash`. All written with the exact API you've been using. No
  internal magic; copy any of them as a starting point.
* **`examples/agent.cpp`** — every built-in plus a one-liner
  `flip_coin` for the shortest possible custom tool.
* [3.3 Sandboxed filesystem tools](#33-sandboxed-filesystem-tools) —
  expose a directory to the model without giving away the whole disk.
* [3.5 Listening for tool calls](#35-listening-for-tool-calls-telemetry--ui-hooks)
  — log every dispatch, light up a UI spinner, push to Prometheus.

##### Ten things you can build in an afternoon

If you want practice, pick one and tell us what you came up with:

1. `now()` — current time in any timezone (parameter `tz`).
2. `coin_flip()` — heads/tails (no parameters).
3. `roll_dice()` — `count` + `sides` parameters.
4. `unit_convert()` — temp/length/weight; HTTP-free.
5. `wikipedia_summary()` — calls `en.wikipedia.org/api/rest_v1/page/summary/<title>`.
6. `slack_post()` — your incoming-webhook URL goes in code.
7. `sqlite_query()` — read-only, parameter `sql`.  Sandbox to one DB.
8. `git_log()` — last N commits of a sandboxed repo.
9. `prometheus_query()` — point at your local `/api/v1/query` endpoint.
10. `home_assistant()` — toggle a light by entity ID.  Now you've
    built the front-end of a smart home.

> **You're done with the chapter.**  Anything you can call from C++,
> you can hand to your AI agent.  That's the entire promise of easyai
> as a framework — and you have everything you need.

### 3.9 The `generate_one()` escape hatch

Use this when you want **one** assistant turn out (no internal tool loop) so
*you* can decide what to do with any tool calls — exactly what the HTTP
server does when the client provides its own tools:

```cpp
engine.push_message("user", "Call get_weather for Tokyo.");
auto turn = engine.generate_one();

if (turn.finish_reason == "tool_calls") {
    for (size_t i = 0; i < turn.tool_calls.size(); ++i) {
        const auto & [name, args] = turn.tool_calls[i];
        std::string result = my_remote_executor(name, args);
        engine.push_message("tool", result, name, turn.tool_call_ids[i]);
    }
    auto final = engine.generate_one();   // model digests tool result
    std::cout << final.content;
} else {
    std::cout << turn.content;
}
```

### 3.10 Talking to a remote endpoint with `easyai::Client`

`libeasyai-cli` is the network-side counterpart of `libeasyai`.  Same
fluent API, same `Tool` registration model, same agentic loop — the
model runs on a remote `/v1/chat/completions` endpoint while your
tools execute locally.

```cpp
// remote.cpp
#include "easyai/client.hpp"
#include "easyai/builtin_tools.hpp"
#include "easyai/plan.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>

int main() {
    easyai::Client cli;
    cli.endpoint("http://ai.local:8080")
       .api_key(std::getenv("EASYAI_API_KEY") ? std::getenv("EASYAI_API_KEY") : "")
       .model("EasyAi")
       .system("You are a planning agent. Be concise.")
       .temperature(0.2f)
       .top_p(0.92f)
       .seed(42);

    cli.add_tool(easyai::tools::datetime());
    cli.add_tool(easyai::tools::search_web());
    cli.add_tool(easyai::tools::fetch_web());

    easyai::Plan plan;
    plan.on_change([](const easyai::Plan & p){
        std::cout << "\n[plan]\n";
        p.render(std::cout);
    });
    cli.add_tool(plan.tool());

    cli.on_token ([](const std::string & p){ std::cout << p << std::flush; });
    cli.on_reason([](const std::string & p){ std::cerr << p << std::flush; });
    cli.on_tool  ([](const easyai::ToolCall & call, const easyai::ToolResult & r){
        std::fprintf(stderr, "%s %s(%s)\n",
                     r.is_error ? "✗" : "🔧",
                     call.name.c_str(),
                     call.arguments_json.c_str());
    });

    std::string answer = cli.chat("Resumo dos 3 papers mais citados sobre Mamba este ano.");
    if (answer.empty() && !cli.last_error().empty()) {
        std::fprintf(stderr, "error: %s\n", cli.last_error().c_str());
        return 1;
    }
    std::cout << "\n";
    return 0;
}
```

CMake — find_package style (after `cmake --install`):

```cmake
find_package(easyai 0.1 REQUIRED)
add_executable(remote remote.cpp)
target_link_libraries(remote PRIVATE easyai::cli)
```

`easyai::cli` transitively pulls `easyai::engine` so `Tool` / `Plan` /
the `easyai::tools::*` factories are available without extra link
flags.

**Sampling and penalty knobs** are all there as fluent setters:
`temperature`, `top_p`, `top_k`, `min_p`, `repeat_penalty`,
`frequency_penalty`, `presence_penalty`, `seed`, `max_tokens`,
`stop(vector)`, `reasoning_effort` (sends the `reasoning_effort` body
field — `"low"` / `"medium"` / `"high"`; the sentinels `"auto"` /
`"none"` / `""` omit it so the model uses its default), `extra_body_json`
(free-form JSON merged last so it can override anything the typed setters
wrote, useful for other non-standard server extensions).

**Server management** without touching curl:

```cpp
std::vector<easyai::RemoteModel> models;
cli.list_models(models);

std::vector<easyai::RemoteTool> remote_tools;
cli.list_remote_tools(remote_tools);     // GET /v1/tools

if (!cli.health()) std::fprintf(stderr, "down: %s\n", cli.last_error().c_str());

std::string props_json;
cli.props(props_json);                    // GET /props (raw JSON)

std::string prom_text;
cli.metrics(prom_text);                   // GET /metrics (Prometheus)

cli.set_preset("creative");               // POST /v1/preset
```

The `easyai-cli` binary (`examples/cli.cpp`) is a
ready-to-run reference for all of the above — REPL or one-shot, every
sampling knob exposed as a flag, seven management subcommands
(`--list-models`, `--list-tools`, `--list-remote-tools`, `--health`,
`--props`, `--metrics`, `--set-preset NAME`).

---

## Part 4 — embedding `libeasyai-cli` (remote agent)

This part is the deep dive on `easyai::Client`.  Use this when the
*model* lives on another machine (or another process) and you want
*your* code to drive the conversation with locally-executed tools.
That's the canonical "agent" architecture — model is rented, brain
trusts itself, hands stay on your laptop.

### 4.1 What `Client` does for you

* Builds a valid OpenAI `/v1/chat/completions` request body.
* Streams the SSE response back, splitting `delta.content`,
  `delta.reasoning_content`, and incremental `delta.tool_calls` into
  your callbacks as they arrive.
* When the model emits `finish_reason="tool_calls"`, dispatches the
  matching `easyai::Tool` *in your process*, captures the result, and
  re-issues the request with the tool message appended — repeating
  until the model emits a non-tool `finish_reason`.
* Caps the agentic loop at 8 hops (matches `Engine::chat_continue`).
* Stores the conversation as raw OpenAI-shape JSON strings internally
  so no JSON type ever leaks through the public ABI.

### 4.2 Setting up the client (fluent)

```cpp
#include "easyai/client.hpp"
#include "easyai/builtin_tools.hpp"
#include "easyai/plan.hpp"

easyai::Client cli;
cli.endpoint("http://ai.local:8080")     // any /v1/chat/completions URL
   .api_key(std::getenv("OPENAI_API_KEY") ? std::getenv("OPENAI_API_KEY") : "")
   .model("EasyAi")                       // request body 'model' field
   .system("You are a planning agent. Be concise.")
   .timeout_seconds(86400)                // connect + read (24 h — multi-hour agentic sessions)
   .http_retries(5)                       // extra attempts on transient failures (default 5; 0 disables)
   .verbose(false);                       // true = log SSE traffic to stderr
```

`endpoint` accepts any HTTP or HTTPS URL.  When the build was linked
with OpenSSL (default if `libssl-dev` is present at configure time)
HTTPS just works.  For dev with a self-signed cert:

```cpp
cli.tls_insecure(true);                  // skip peer cert verification
// or:
cli.ca_cert_path("/etc/ssl/certs/internal-ca.pem");  // trust a custom CA
```

### 4.3 Sampling and penalty knobs

Every standard OpenAI / llama-server / easyai-server field is a
fluent setter.  Pin only the ones you care about — leaving any of
them alone keeps the server's default in effect.

```cpp
cli.temperature(0.2f)
   .top_p(0.92f)
   .top_k(50)
   .min_p(0.03f)               // llama-server / easyai
   .repeat_penalty(1.04f)      // anti-loop default; pass 1.0 to disable
   .frequency_penalty(0.05f)   // per-token count penalty, [0.0, 2.0]
   .presence_penalty(0.1f)     // per-token-seen penalty, [-2.0, 2.0]
   .seed(42)                   // deterministic; -1 = randomise
   .max_tokens(12288)
   .stop({ "\n\nUSER:", "\n\nQ:" });
```

`reasoning_effort` is now a first-class setter (`cli.reasoning_effort("high")`);
for other non-standard server fields (`tool_choice`, provider-specific
extensions) there's an escape hatch:

```cpp
cli.extra_body_json(R"({"logit_bias":{"50256":-100}})");
```

The string MUST parse as a JSON object; its keys merge into the
request body **last**, so they override anything the typed setters
wrote (handy for emergency one-offs).

### 4.4 Tools (registered locally)

Same `easyai::Tool` type used by `Engine`.  The handler runs in your
process when the model picks the tool.

```cpp
// Built-in tools (compiled into libeasyai):
cli.add_tool(easyai::tools::datetime());
cli.add_tool(easyai::tools::search_web());
cli.add_tool(easyai::tools::fetch_web());
cli.add_tool(easyai::tools::fs_read_file("/data"));   // sandbox to /data
cli.add_tool(easyai::tools::fs_list_dir ("/data"));

// Built-in plan tool — separate object so you can render its state.
easyai::Plan plan;
plan.on_change([](const easyai::Plan & p){
    std::cout << "\n[plan]\n";
    p.render(std::cout);
});
cli.add_tool(plan.tool());

// Your own tool, inline:
cli.add_tool(easyai::Tool::builder("flip_coin")
    .describe("Returns 'heads' or 'tails' with uniform probability.")
    .handle([](const easyai::ToolCall &){
        return easyai::ToolResult::ok((std::rand() & 1) ? "heads" : "tails");
    }).build());
```

There is **no API difference** between a `Tool` registered on `Engine`
and one registered on `Client` — your authoring code is portable
across "local model" and "remote model" deployments.

### 4.5 Streaming callbacks

```cpp
cli.on_token([](const std::string & piece) {
    std::fputs(piece.c_str(), stdout);
    std::fflush(stdout);
});
cli.on_reason([](const std::string & piece) {
    // Optional: render the model's hidden reasoning in dim grey.
    std::fprintf(stderr, "\033[2m%s\033[0m", piece.c_str());
});
cli.on_tool([](const easyai::ToolCall & call,
                const easyai::ToolResult & r) {
    std::fprintf(stderr, "[tool] %s%s -> %s\n",
                 r.is_error ? "FAIL " : "",
                 call.name.c_str(),
                 r.content.substr(0, 120).c_str());
});
```

`on_reason` is opt-in by design — many UIs hide reasoning by default
(it's noisy, and some servers don't emit it at all).  `on_token` is
the visible reply; `on_tool` fires once per dispatched tool round-trip
(call + result already paired).

**Composing extra behaviour onto `on_tool`.** Each callback slot is
single-valued — calling `cli.on_tool(...)` again **replaces** the
previous handler, it does not chain.  If you want to add a checkpoint
or audit step on top of the canonical UI handler that
`easyai::ui::Streaming::attach(cli)` installs, use the public
forwarder `Streaming::notify_tool(call, result)` and wrap both:

```cpp
easyai::ui::Streaming streaming(spinner, stats, style);
streaming.attach(cli);   // sets the canonical UI on_tool handler

cli.on_tool([&](const easyai::ToolCall & c,
                 const easyai::ToolResult & r) {
    streaming.notify_tool(c, r);   // canonical UI (tool indicator,
                                   // dim styling, plan re-render)
    checkpoint_to_disk(cli);       // your extra work
});
```

This pattern is how `easyai-cli` saves `.easyai_session` after every
tool dispatch (so a force-exit mid-turn still leaves the
conversation up to the last completed tool on disk).

### 4.6 Driving the conversation

```cpp
std::string answer = cli.chat("Resumo dos 3 papers mais citados sobre Mamba este ano.");

if (answer.empty() && !cli.last_error().empty()) {
    std::fprintf(stderr, "error: %s\n", cli.last_error().c_str());
    std::exit(1);
}
```

`chat()` pushes the user message into history, runs the agentic loop,
and returns the final visible content.  Successive `chat()` calls
keep the conversation going (history is preserved).  To start over:

```cpp
cli.clear_history();
```

For more control (e.g. injecting tool results from outside), use
`chat_continue()` after pushing your own messages onto history via
the lower-level shape — but `chat()` is what 99% of agents want.

### 4.7 Server-management endpoints

Each method maps 1:1 to the matching easyai-server route, returns
`true` on success, and writes diagnostic detail to `last_error()` on
failure.  Together they make the lib enough to script and recreate a
server's state from scratch.

```cpp
std::vector<easyai::RemoteModel> models;
cli.list_models(models);                  // GET /v1/models

std::vector<easyai::RemoteTool> tools;
cli.list_remote_tools(tools);             // GET /v1/tools (easyai extension)

if (!cli.health()) {                       // GET /health
    std::fprintf(stderr, "down: %s\n", cli.last_error().c_str());
}

std::string props;
cli.props(props);                          // GET /props (raw JSON)

std::string prom;
cli.metrics(prom);                         // GET /metrics (Prometheus text)

cli.set_preset("creative");                // POST /v1/preset
```

### 4.8 The `easyai-cli` binary as a reference

Everything above is exposed as flags on `examples/cli.cpp`.
Read its source to see one possible "wire it all up" pattern; lift
chunks into your own app verbatim.

```bash
# REPL with the default tool set (datetime, plan, search_web,
# fetch_web, system_*); EASYAI_URL / EASYAI_API_KEY env vars work too.
easyai-cli --url http://ai.local:8080

# One-shot scripted call with a custom tool whitelist:
easyai-cli --url https://api.openai.com \
  --api-key $OPENAI_API_KEY --model gpt-4o-mini \
  --tools datetime,plan,search_web,fetch_web \
  -p "Investigate today's most-cited mamba arxiv papers; produce a 5-bullet summary."

# Pin sampling + add stop sequences:
easyai-cli --url http://ai.local:8080 \
  --temperature 0.0 --top-p 0.9 --seed 42 --stop "USER:" --stop "Q:" \
  -p "Translate the next sentence to PT-BR: ..."

# reasoning_effort is first-class (default 'auto' = model default):
easyai-cli --url https://api.openai.com --api-key $K --model o1-preview \
  --reasoning-effort high \
  -p "Plan the Mars-mission trajectory."

# List local tools and exit (what the model will be told about):
easyai-cli --url http://x --list-tools

# List server-side tools (easyai-server-only extension):
easyai-cli --url http://ai.local:8080 --list-remote-tools
```

REPL specials inside the interactive mode:

| Command         | Effect                                                  |
|-----------------|---------------------------------------------------------|
| `/exit /quit`   | leave                                                   |
| `/clear`        | clear conversation history (keep tools + system)        |
| `/reset`        | clear history AND clear plan                            |
| `/plan`         | re-print the plan checklist                             |
| `/tools`        | list locally-registered tools                           |
| `/help`         | show specials                                           |

---

## Part 5 — authoring custom tools

This is the cookbook for adding tools the model can call.  Every tool
in `libeasyai`'s built-in set was written exactly the way you'll
write yours.

### 5.1 Anatomy of a Tool

```cpp
struct Tool {
    std::string name;
    std::string description;
    std::string parameters_json;      // JSON schema
    ToolHandler handler;              // std::function<ToolResult(const ToolCall &)>;
};
```

Four fields.  The first three feed the chat template's tool-call
section so the model knows what's available; the fourth is your
function pointer.

### 5.2 Two ways to build a Tool

**Builder** (the typed shorthand, generates the JSON schema for you):

```cpp
easyai::Tool::builder("weather")
    .describe("Return the current weather for a city, in metric units.")
    .param("city", "string", "Name of the city, e.g. 'Lisbon'", /*required=*/true)
    .param("units", "string", "'metric' (default) or 'imperial'.",  false)
    .handle([](const easyai::ToolCall & c) -> easyai::ToolResult {
        std::string city  = easyai::args::get_string_or(c.arguments_json, "city",  "");
        std::string units = easyai::args::get_string_or(c.arguments_json, "units", "metric");
        if (city.empty()) return easyai::ToolResult::error("'city' is required");
        // …call wttr.in…
        return easyai::ToolResult::ok("23 °C, sunny");
    })
    .build();
```

**`Tool::make`** (raw schema string, when you need nested objects /
enums / oneOf that the typed param API can't express):

```cpp
easyai::Tool::make(
    "rgba_set",
    "Set the LED RGBA at index.",
    R"({"type":"object",
        "properties":{
          "i":{"type":"integer","minimum":0,"maximum":31},
          "color":{"type":"object","properties":{
            "r":{"type":"integer"},"g":{"type":"integer"},
            "b":{"type":"integer"},"a":{"type":"integer"}
          },"required":["r","g","b"]}
        },
        "required":["i","color"]})",
    [](const easyai::ToolCall & c) -> easyai::ToolResult {
        // For nested args, parse the JSON yourself; nlohmann is vendored
        // by llama.cpp at vendor/nlohmann/json.hpp if you want it.
        return easyai::ToolResult::ok("set");
    });
```

### 5.3 Reading arguments without a JSON dependency

`easyai::args::*` are tiny single-level scanners.  They're enough for
~95% of tool authors:

```cpp
std::string  q   = args::get_string_or(c.arguments_json, "q", "");
long long    max = args::get_int_or   (c.arguments_json, "max", 10);
bool         dry = args::get_bool_or  (c.arguments_json, "dry_run", false);
double       t   = args::get_double_or(c.arguments_json, "threshold", 0.5);
bool         has = args::has          (c.arguments_json, "verbose");
```

For nested args (objects, arrays of objects), include
`<nlohmann/json.hpp>` in your handler and parse normally — no easyai
limitation there.

### 5.4 Returning results

```cpp
return easyai::ToolResult::ok("the answer is 42");
return easyai::ToolResult::error("network unreachable");
```

`error` results are tagged `is_error=true` so the streaming layer can
render them differently (`✗` instead of `🔧` in the cli-remote
output).  The model still sees the content — it's just hinted that
the call failed.

Best practices:

* Keep ok-content short and structured (the model reads it as plain
  text; line breaks are fine).
* Truncate raw output to a reasonable budget — 8–16 KB is plenty.
* Format errors as imperative ("missing 'path' argument") — the
  model will often retry with the fix.

### 5.4b The `tool_lookup` builtin — let the model verify itself

When the model emits `write(file_path=…)` and gets `unknown tool:
write` back, the rest of the turn is usually wasted retrying the
hallucinated name. `tool_lookup` is the read-only escape hatch
PLUS the on-demand "full manual" lookup (since 2026-05-26 it ships
with TWO modes):

```cpp
engine.add_tool(easyai::tools::tool_lookup([&engine]() {
    easyai::tools::ToolCatalog v;
    for (const auto & t : engine.tools()) {
        v.push_back({ t.name, t.wire_description(), t.description });
    }
    return v;
}));
```

Register it **last** so the snapshot it returns covers everything
else. The lambda re-reads `engine.tools()` at every call, so even
tools added dynamically after `tool_lookup` show up. (`Client` has
the same `tools()` accessor, so the wiring is identical.)

**Two modes:**

* **No arguments → INDEX view.** Numbered list of `name: short
  trigger` — one line per tool. Cheap, scannable. What the model
  sees:

  ```
  1. datetime: Return the current UTC and local date/time. Call for 'now'/'today'/'latest'…
  2. web: Search the web and fetch URLs. action=search|fetch. Reply MUST end with a `Sources:` block…
  3. fs: Filesystem: read/write/edit/list/glob/grep in sandbox. Batch with action="ops"…
  4. tool_lookup: List or inspect tools registered this session. No args → index; name="<substring>" → full manual…
  ```

* **`name="<substring>"` → MANUAL view.** Full multi-line
  description (rules, examples, edge cases) for every tool whose
  name matches the substring. This is the **expanded help text**
  the model drills into when the index trigger isn't enough.

The split is what makes the Shape-C wire format work end-to-end:
the per-turn `<tools>` block ships the short trigger only (saves
~2 000 tokens), and `tool_lookup(name="fs")` returns the full
manual on demand. See AI_TOOLS.md "Shape-C wire shape" for the
wire-side picture.

`name="<substring>"` is case-insensitive partial match. A no-match
result returns a clear "(no tools match: …)" string rather than an
empty list, so the model never confuses an empty filter with an
empty catalogue.

The companion is the system-prompt tools block — `easyai::preamble::
tools_block(view)` lives in libeasyai and is shared by server,
local, and cli. Together they take the model from "guess and retry"
to "verify first."

### 5.5 Sandboxing

The built-in `*_file` family takes a root directory and refuses to
escape it (`..` and absolute paths are rejected).  The check is
**path-component aware** — a sandbox at `/srv/user` rejects
`/srv/userMALICIOUS/secret` (no string-prefix match). Symlinks
resolve through `fs::weakly_canonical` and the resolved path must
contain the root as a prefix on path-component boundaries. Last-
millisecond symlink swaps (TOCTOU) are defeated by `O_NOFOLLOW`
on the open() call. Full details in
[`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) §1, §18.3.

If you're new to easyai's threat model, the operator-facing
60-second TL;DR lives at the top of the audit: what easyai blocks
for you, what's your responsibility, and the three knobs that
matter most. Read [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) §0
before going to production.

Pattern for your own filesystem-touching tools:

```cpp
easyai::Tool::builder("read_log")
    .describe("Read the last N lines of a service log under /var/log.")
    .param("name", "string", "Service name (e.g. 'easyai-server.service').", true)
    .param("n",    "integer", "How many lines (max 5000). Default 200.",     false)
    .handle([](const easyai::ToolCall & c) -> easyai::ToolResult {
        std::string name = args::get_string_or(c.arguments_json, "name", "");
        if (name.find('/') != std::string::npos)
            return easyai::ToolResult::error("name must not contain slashes");
        std::filesystem::path p = std::filesystem::path("/var/log") / (name + ".log");
        if (!std::filesystem::exists(p))
            return easyai::ToolResult::error("no log: " + p.string());
        // …tail the file…
        return easyai::ToolResult::ok("…");
    })
    .build();
```

### 5.6 The `Plan` tool

`easyai::Plan` is a checklist with four sub-actions exposed as a
single tool:

```cpp
easyai::Plan plan;
cli.add_tool(plan.tool());      // or engine.add_tool(...)

plan.on_change([](const easyai::Plan & p){
    std::cout << "\n=== plan ===\n";
    p.render(std::cout, /*color=*/true);   // ANSI-styled checklist
});
```

The model sees ONE tool with an `action` enum:

| Action   | Single-item shape                       | Batch shape (max 20)                                |
|----------|-----------------------------------------|-----------------------------------------------------|
| `add`    | `text="…"`                              | `items=[{text}, {text}, …]`                         |
| `update` | `id="3", text?="…", status?="working"`  | `items=[{id, text?, status?}, …]`                   |
| `delete` | `id="3"` (or `id="all"` to wipe)        | `items=[{id}, {id}, …]`                             |
| `list`   | (no fields)                             | —                                                   |

Statuses: **pending** (default on add) → **working** → **done**, plus
**error** (model flags the step as failed) and **deleted** (soft
delete — the entry stays in the list rendered struck-through, so
the user can see what was abandoned).  Terminal rendering with
`color=true`: bold for active items, dim for done, red for error,
strikethrough+dim for deleted.

The single-tool / multi-action shape is a deliberate trade — it
keeps the model's tool-pick fan-out small and lets weak / 1-bit-quant
models stay fluent. The tool description tells the model explicitly
*"never re-add to mutate a step — use update"*, which closes the
duplicate-id failure mode that earlier versions had.

Works reliably with any tool-call-capable model (Qwen 2.5+, Llama 3+,
DeepSeek, OpenAI o-series, Anthropic Claude via OpenAI-compat
proxies). On non-trivial multi-step tasks, prompt it to "use the
plan tool to break the task into steps and tick them off as you go".

You can also seed the plan from your code before letting the model
take over:

```cpp
plan.add("fetch arxiv listing");
plan.add("triage by citation count");
plan.add("draft 5-bullet digest");

// Or programmatically advance / mark error / soft-delete:
plan.update("1", /*text=*/"", /*status=*/"working");
plan.update("2", /*text=*/"triage by citation count + h-index", "");
plan.remove("3");        // marks "deleted" — stays visible, struck through
```

### 5.7 Cookbook — system observability tools

`examples/cli.cpp` ships four inline `system_*` tools that
read `/proc/*` and report back.  The whole pattern is:

1. Read a `/proc` file with `ifstream`.
2. Parse it (helper functions live in `namespace systools`).
3. Format a human-readable string.
4. Return `ToolResult::ok(text)`.

These tools turn the cli-remote process into an observability agent
that can answer "is the server paging?", "which CPU is hot?", "what
swap device is configured?" — entirely model-driven.  Look at the
file from the top (~line 60) for a guided tour with comments.

To add your own:

* `system_disk_usage` — `df -h` worth of info (read `/proc/mounts`,
  call `statvfs`).
* `system_processes` — `ps`-equivalent (walk `/proc/<pid>/stat`).
* `system_network` — interfaces + traffic counters
  (`/proc/net/dev`).

Copy the existing helpers and ship.

### 5.8 Hot-loading vs. registration

Once you call `cli.add_tool(...)` (or `engine.add_tool(...)`) the
tool is *registered for the lifetime of that object*.  There's no
"unregister" — destroy the Client/Engine to drop them.  This is by
design: the tool list is a property of the conversation contract
(the model was told what's available); changing it mid-flight would
confuse the chat-template renderer.

If you need conditional tools per-conversation, build a fresh
`Client` for that conversation.  `Client` is move-only; constructing
one is cheap (no I/O until `chat()`).

---

## Part 6 — deploying easyai-server

The official path on Linux is `scripts/install_easyai_server.sh`.
Run it from a fresh checkout:

```bash
git clone https://github.com/solariun/easy.git
git clone https://github.com/ggml-org/llama.cpp.git
cd easy
sudo scripts/install_easyai_server.sh \
    --model /path/to/your-model.gguf \
    --webui-title "Box AI" \
    --enable-now
```

It detects the GPU backend (`nvidia-smi` → CUDA, `rocminfo` → ROCm,
`vulkaninfo` / AMD `lspci` → Vulkan, else CPU), builds the right
flavour, installs the libs into `/usr/lib/easyai/` (isolated from
system), creates an `easyai` system user, and drops a hardened
systemd unit with `mlock`, flash-attn, q8_0 KV cache, Bearer auth,
and Prometheus `/metrics`.

### 6.1 Frontend TLS via nginx

`libeasyai-cli` already speaks HTTPS, but easyai-server itself is
plain HTTP by design.  Terminate TLS at nginx:

```nginx
server {
    listen 443 ssl http2;
    server_name ai.example.com;
    ssl_certificate     /etc/letsencrypt/live/ai.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/ai.example.com/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Connection "";
        # SSE keepalive — thinking models can hold a stream for many
        # minutes between visible tokens; pick a value at least as
        # high as the server's --http-timeout (default 600 s).
        proxy_buffering    off;
        proxy_read_timeout 1800;
        proxy_send_timeout 1800;
    }
}
```

Then point the client at `https://ai.example.com` and the build's
OpenSSL link will Just Work.

### 6.2 Multiple models

Run one easyai-server per model on different ports
(`--port 8080` / `--port 8081`).  Have your client switch between
them via `cli.endpoint(...)` — there is no notion of "model swap"
inside a single server process by design.

### 6.3 Updating without downtime

```bash
sudo scripts/install_easyai_server.sh --upgrade
```

The script does `git fetch` + `git pull --ff-only` + rebuild +
`systemctl restart easyai-server` in that order.  In-flight SSE
streams are aborted when the old process dies; the client gets a
`HTTP request failed: connection closed` error.  The client's
built-in HTTP retry layer (`--http-retries`, default 5) does NOT
re-issue mid-stream because the model has already produced visible
tokens — the upgrade window is briefly visible to active users.
For zero-downtime upgrades, run two backends behind a load
balancer and drain one at a time.

### 6.4 Per-model INI profiles — `[MODEL_<pattern>]`

The INI file supports per-model override sections.  When the server
loads a model, it resolves symlinks, strips the path to
basename-without-extension, and does a case-insensitive substring
match against all `[MODEL_*]` section names.  The longest match wins.

Keys are the same as `[ENGINE]` — `temperature`, `top_p`, `top_k`,
`min_p`, `repeat_penalty`, `presence_penalty`, `frequency_penalty`,
`max_tokens`, `context`, `ngl`, `flash_attn`, `cache_type_k`,
`cache_type_v`, `rope_scaling`, `rope_freq_scale`, `yarn_orig_ctx`,
`split_mode`, etc.  Only include keys you want to override; omitted
keys keep the `[ENGINE]` value.

Precedence: **CLI flags > MODEL_\<match\> > [ENGINE] > hardcoded**.

Example: loading `Qwen3-Coder-Next-Q6_K_M.gguf` (or a symlink
`ai.gguf` pointing at it) matches both `[MODEL_Qwen3]` and
`[MODEL_Qwen3-Coder-Next]` — the latter wins because
`"Qwen3-Coder-Next"` is a longer substring match.

```ini
[MODEL_Qwen3-Coder]
temperature      = 0.2
top_p            = 0.92
top_k            = 50
min_p            = 0.03
repeat_penalty   = 1.04
presence_penalty = 0.1
frequency_penalty = 0.05
context          = 262144
rope_scaling     = yarn
rope_freq_scale  = 2
yarn_orig_ctx    = 131072

[MODEL_DeepSeek]
temperature      = 0.6
top_p            = 0.95
top_k            = 40
min_p            = 0.05
repeat_penalty   = 1.0
presence_penalty = 0.0
frequency_penalty = 0.0
context          = 131072
```

The installer writes these as commented-out examples in
`/etc/easyai/easyai.ini`.

### 6.5 Backups

Stateless except for whatever you put in `/var/lib/easyai/`
(model files, sandboxed *_file roots).  Snapshot that directory.

---

## Part 7 — operating the server

### 7.1 Health & metrics

* `GET /health` — JSON status.  Cheap, use it as a liveness probe.
* `GET /metrics` — Prometheus text exposition (only when `--metrics`
  was passed).  Counters: `easyai_requests_total`,
  `easyai_tool_calls_total`, `easyai_errors_total`.
* `GET /props` — full server config snapshot (n_ctx, model alias,
  build info).

### 7.2 Live preset switching

```bash
curl -H 'Content-Type: application/json' \
     -d '{"preset":"creative"}' \
     http://ai.local:8080/v1/preset
```

Or from the lib:

```cpp
cli.set_preset("creative");
```

Affects subsequent requests until changed again.  Per-request
sampling (set in the request body) still wins for that one call.

### 7.3 Verbose mode

`--enable-verbose` (installer flag) or `--verbose` (binary flag) makes
the engine log raw model output, parser actions, and SSE events to
stderr.  Tail it with `journalctl -u easyai-server -f`.

### 7.4 Crash capture

`scripts/install_easyai_server.sh` installs `systemd-coredump` and
sets `LimitCORE=infinity` on the unit.  When the process dies:

```bash
coredumpctl list easyai-server.service
coredumpctl gdb <PID>      # opens gdb on the most recent core
```

---

## Part 8 — performance & tuning

### 8.1 Context size vs. throughput

`-c, --ctx N` sets the model's sequence window.  Bigger ctx = more
KV cache memory per token.  Rule of thumb on Vulkan/RADV with
gfx1035: keep ctx + n_predict ≤ what fits in `--ngl auto`.

### 8.2 KV cache quantisation

`--cache-type-k q8_0 --cache-type-v q8_0` cuts KV memory ~3× vs.
default `f16` with negligible quality loss for chat workloads.  The
installer ships `q8_0` by default.

### 8.3 flash-attn

`--flash-attn` enables fused attention — faster + less memory on
backends that support it.  CUDA and Metal: yes.  Vulkan: works on
RDNA2+ with recent llama.cpp (validated on gfx1035).

### 8.4 mlock

`--mlock` pins the model in RAM so the OS can't page it out under
pressure.  Required on the AI box because GTT-mapped pages would
otherwise be swap candidates.  Needs `LimitMEMLOCK=infinity` in the
systemd unit (the installer sets this).

### 8.5 Sampling

**What each knob does.** At every step the model emits a probability
distribution over the whole vocabulary (~100k+ tokens); these knobs
decide how a token is picked from it. They work in sequence — the
*cutters* (`top_k`, `top_p`, `min_p`) narrow the candidate pool over
the raw distribution, then `temperature` controls how randomly the
final token is drawn from the survivors.

* **`temperature`** — focus-vs-risk dial; divides the logits before
  softmax. `→ 0` is greedy (always the top token: deterministic).
  `0.2–0.5` keeps the model tight on format, syntax, and facts. `1.0`
  is the unmodified distribution. `> 1.0` flattens the curve so
  unlikely tokens get a real chance — more creative, more prone to
  error. The main *behaviour* dial.
* **`top_k`** — *fixed* tail cut: keep the K most-probable tokens,
  discard the rest. Non-adaptive; a cheap guardrail against junk from
  the long tail.
* **`top_p`** (nucleus) — *adaptive* tail cut: keep the smallest set of
  top tokens summing to P. Tiny nucleus when the model is confident,
  large when it's unsure.
* **`min_p`** — adaptive too, but anchored to the *top* token: keep
  tokens with `prob ≥ min_p × prob_of_top`. `min_p 0.5` keeps only
  what's within 2× of the best — aggressive, very focused.

They stack — tightening all of them at once is redundant. Practical
rule: pick *one* adaptive cutter (`top_p ~0.9–0.95` **or** `min_p
~0.03–0.1`), leave `top_k` generous as a backstop (`50` default), and
use `temperature` as the real behaviour dial. Low `temperature`
(0.2–0.6) for code / agentic / structured output; higher (0.8–1.2) for
creative work; lean conservative on heavily quantised models
(quantisation already adds logit noise, and high temperature amplifies
it into real errors).

Presets order (project-wide default is **`precise`**):
* `deterministic`  — temp 0.0, greedy.  Same prompt → byte-identical reply. Reproducibility / CI / eval harnesses.
* `precise`        — temp 0.2, min_p 0.10.  **Default.** Code, math, factual Q&A, tool-call workloads, structured output.
* `balanced`       — temp 0.7.  General-purpose chat, summarisation, casual Q&A.
* `creative`       — temp 1.0, top_p 0.95.  Brainstorming, fiction, marketing copy.
* `wild`           — temp 1.4 + relaxed.  Pure exploration; don't ship it.

See [`README.md` §Sampling presets](README.md#sampling-presets) for
the full Behaviour / Pick when… table.

Per-request: pin temp + top_p + top_k + min_p in the request body
(via the `--temperature` / `--top-p` / etc. flags on cli-remote, or
the matching `Client::*` setters in code).  These reset every turn.

### 8.6 Penalties — `repeat_penalty`, `frequency_penalty`, and `presence_penalty`

Penalties bias generation *against* tokens that have already been
produced.  Three knobs, three failure modes:

| Knob | Form | Default | What it bites on |
|---|---|---|---|
| `repeat_penalty` | multiplicative on logits in recent window | 1.04 | tight literal repetition ("I'll write X / Let me write X / OK, creating X") |
| `frequency_penalty` | additive, proportional to token *count* | 0.05 | over-use of common tokens ("the the the"); range [0.0, 2.0] |
| `presence_penalty` | additive, fixed cost per token-already-seen *at all* | 0.1 | topic stickiness without per-occurrence ramp-up |

`repeat_penalty` (default `1.04`) is a light anti-loop safety net for
thinking models that otherwise rephrase the same intent before
acting.  It works for short turns.  On *long agentic flows* (10+
tool hops) it starts misfiring — by the fifth `fs_read_file` call
the literal tokens of the tool name fall inside the window, the
model paraphrases ("read_file", "read_file"), the dispatcher fails
with "unknown tool".

`frequency_penalty` (default `0.05`) applies an additive cost
proportional to how many times a token has appeared.  Unlike
`repeat_penalty`, the cost grows with each occurrence, so it
penalises *frequent* re-use harder than a one-off repeat.
Set via `[ENGINE] frequency_penalty` / `--frequency-penalty`.

`presence_penalty` (default `0.1`) is the lever for the *other*
failure mode.  A fixed per-token-seen cost discourages re-introducing
the same vocabulary without the per-occurrence ramp of
`repeat_penalty`, so calling `fs_read_file` for the tenth time costs
the same as the second.

The production AI box ships `repeat_penalty=1.04` +
`frequency_penalty=0.05` + `presence_penalty=0.1` — a balanced
triple that tested better on long agentic flows than a single heavy
`repeat_penalty`.  Operators with shorter chat workloads can keep
a simpler pairing (`repeat_penalty=1.15`, others at `0`).

Persistence: penalties are set at startup via the INI / CLI flags
(`[ENGINE] repeat_penalty / presence_penalty / frequency_penalty`,
`--repeat-penalty / --presence-penalty / --frequency-penalty`) and
**persist across requests**.  Per-request `set_sampling()` only
resets the shapers (temp / top_p / top_k / min_p) — the penalties
stick.  This is deliberate: they're operator-tuned guardrails, not
per-call stylistic knobs.

The full design rationale (math, failure modes, layered API) lives
in [`design.md` §4b](design.md#4b-sampling-and-the-penalty-stack).

### 8.7 Tool budget

`Engine::chat()` caps at 8 tool hops; `Client::chat()` does the
same.  A model that runs away calling tools without converging will
hit the cap and bail out with the last partial answer.  Visible in
verbose mode as `[easyai] hop 7: …`.

### 8.8 RoPE scaling and context extension

When `ctx_size` exceeds the model's native training context, RoPE
scaling lets the model extrapolate.  Three Engine setters control it:

* **`rope_scaling(type)`** — `"none"` (default), `"linear"`, or
  `"yarn"`.  YaRN is the recommended method for large extensions
  (2x+).
* **`rope_freq_scale(scale)`** — frequency scale factor.
  `0.0` = model default.  Pass `2` to double the effective context.
* **`yarn_orig_ctx(ctx)`** — YaRN original context length.
  `0` = use model default.  Set to the model's training context
  (e.g. `131072` for a 128K-trained model) when extending with YaRN.

CLI / INI equivalents: `--rope-scaling`, `--rope-scale`,
`--yarn-orig-ctx` / `[ENGINE] rope_scaling`, `rope_freq_scale`,
`yarn_orig_ctx`.

The installer defaults ship `rope_scaling=yarn`, `rope_freq_scale=2`,
`yarn_orig_ctx=131072` — doubling a 128K model to the default
`ctx_size=262144`.

### 8.9 GPU split mode

Controls how model layers distribute across multiple GPUs:

* **`split_mode(mode)`** — `"none"` (single GPU), `"layer"` (default,
  split layers across GPUs), `"row"`, or `"tensor"`.

CLI / INI: `-sm` / `--split-mode` / `[ENGINE] split_mode`.

Single-GPU setups should use `"none"` (the installer default).
Multi-GPU rigs benefit from `"layer"` or `"row"` depending on the
model size vs. per-GPU VRAM.

---

## Part 9 — recipes (cookbook)

### 9.0 Local vs. remote — pick the right binary

Since the rename, **two independent binaries** cover the two use cases.
No more dual-mode flag juggling on a single binary.

| Binary         | What it loads                  | Library link    |
|----------------|--------------------------------|-----------------|
| `easyai-local` | a local GGUF (no HTTP at all)  | `libeasyai`     |
| `easyai-cli`   | a remote `/v1/chat/completions`| `libeasyai-cli` |

`easyai-cli` (remote) supports the standard TLS + agentic flags:

| Flag             | Default | Effect                                                                |
|------------------|---------|-----------------------------------------------------------------------|
| `--insecure-tls` | off | Skip peer certificate verification (DEV ONLY, https only). |
| `--ca-cert <path>` | system | Trust a custom CA bundle (PEM) for `https://` endpoints. |
| `--timeout SECONDS` | 86400 (24h) | Read+write timeout — sized for multi-hour agentic sessions. The timer only fires on TRUE silence; every SSE delta resets it, so the value isn't a wall-clock budget on the turn, just a "no progress for X seconds" cutoff. `EASYAI_TIMEOUT` env. |
| `--http-retries N` | 5 | Extra attempts on transient HTTP failures (connect refused, read timeout, 5xx). 0 disables. Logged on stderr. `EASYAI_HTTP_RETRIES` env. |

Both binaries share the same preset commands, the same `/help`, and the
same streaming-aware `<think>` stripper (`--no-reasoning` on `easyai-cli`,
`--no-think` on `easyai-local`).

```bash
# point at easyai-server running on the LAN
./build/easyai-cli --url http://10.0.0.5:8080

# point at openai.com (env vars EASYAI_API_KEY also work)
./build/easyai-cli --url https://api.openai.com/v1 \
                   --api-key sk-... \
                   --model gpt-4o-mini

# point at a llama-server / vLLM / ollama endpoint — anything that speaks /v1
./build/easyai-cli --url http://127.0.0.1:11434/v1 --model llama3.1:8b
```

One-shot mode for scripting:

```bash
# Local one-liner; banners go to stderr so capturing stdout is clean.
answer=$(./build/easyai-local -m model.gguf -p "summarise: $(cat file.txt)")

# Remote with reasoning suppressed:
./build/easyai-cli --url http://localhost:8080 --no-reasoning \
    -p "explain BGP route reflectors in two sentences" \
    > brief.md
```

`--no-think` (local) and `--no-reasoning` (remote) strip `<think>…</think>`
(and `<thinking>…</thinking>`) blocks from output. The filter is
streaming-aware and works even when the open or close tag is split across
two model-emitted token chunks.

### 9.1 Web search

`search_web` works out of the box — it talks to DuckDuckGo's HTML endpoint
directly via libcurl. There is nothing to configure and no API key.

If DDG starts rate-limiting your IP (rare), the tool returns an explicit
error message instead of silently failing. If you need a different backend
(Bing, Brave, your own SearXNG), the implementation lives in
`src/builtin_tools.cpp::search_web()` — copy that handler, swap the URL and
the regex pair, and register your variant via `engine.add_tool(my_search())`.

### 9.2 Forcing CPU-only

Pass `--ngl 0` (CLI/server) or `engine.gpu_layers(0)` (lib).

### 9.3 Force-disable a built-in tool

Just don't add it. There is no global "remove" — easyai has no global
state. To run `easyai-local` without any tools at all:

```bash
./build/easyai-local -m … --no-tools
```

For the server: `--no-local-tools` (renamed from `--no-tools` so the
flag's scope is unambiguous now that `easyai-server` can also be an
MCP client — `--no-local-tools` skips the LOCAL toolbelt only,
leaving the `knowledge_*` tools, external-tools, and any tools fetched via
`--mcp` intact).

### 9.4 Production deployment — replacing `llama-server`

`easyai-server` is a drop-in replacement for `llama-server` for almost
every flag a deployment script cares about. A long-running production
launch looks like:

```bash
./build/easyai-server \
    --model      /var/lib/easyai/models/ai.gguf \
    --alias      SolariunAI_Box \
    --host       0.0.0.0 --port 8080 \
    --ctx        262144 \
    --ngl        99 \
    --threads    8  --threads-batch 8 \
    --flash-attn \
    --cache-type-k q8_0 --cache-type-v q8_0 \
    --mlock --no-mmap \
    --preset balanced --temperature 0.2 --top-p 0.92 --top-k 50 \
    --api-key    "$EASYAI_API_KEY" \
    --metrics \
    --system-file /etc/easyai/system.txt \
    --sandbox    /var/lib/easyai/workspace
```

Flag map vs. `llama-server`:

| llama-server flag        | easyai-server flag       |
|--------------------------|--------------------------|
| `-m / --model`           | `-m / --model`           |
| `--host` / `--port`      | `--host` / `--port`      |
| `-a / --alias`           | `-a / --alias`           |
| `-c / --ctx-size`        | `-c / --ctx`             |
| `--n-gpu-layers`         | `--ngl`                  |
| `-t / --threads`         | `-t / --threads`         |
| `-tb / --threads-batch`  | `-tb / --threads-batch`  |
| `-fa / --flash-attn`     | `-fa / --flash-attn`     |
| `-ctk / -ctv`            | `-ctk / -ctv`            |
| `--mlock` / `--no-mmap`  | `--mlock` / `--no-mmap`  |
| `--api-key`              | `--api-key`              |
| `--metrics`              | `--metrics`              |
| `--reasoning <on/off>`   | `--reasoning <on/off>`   |
| `--override-kv`          | `--override-kv`          |
| `--frequency-penalty`    | `--frequency-penalty`    |
| `-sm / --split-mode`     | `-sm / --split-mode`     |
| `--rope-scaling`         | `--rope-scaling`         |
| `--rope-freq-scale`      | `--rope-scale`           |
| `--yarn-orig-ctx`        | `--yarn-orig-ctx`        |
| `-np / --parallel`       | accepted; warns since the engine is single-context |

When `--api-key` is set, every `/v1/*` request must carry
`Authorization: Bearer <key>`. `/`, `/health`, and `/metrics` stay open
(useful for liveness probes and Prometheus scrapes).

`/metrics` exposes Prometheus-style counters
(`easyai_requests_total`, `easyai_errors_total`, `easyai_tool_calls_total`)
that you can wire into Grafana or alertmanager.

### 9.5 Behind a reverse proxy

The server speaks plain HTTP and supports CORS. Stick nginx/Caddy in front
to add TLS, auth, and rate limiting. Example Caddyfile:

```
ai.example.com {
    reverse_proxy 127.0.0.1:8080
    basicauth {
        gus  $2a$14$…   # bcrypt hash of password
    }
}
```

### 9.6 Multiple models, one host

Run one `easyai-server` per model on different ports, then add a tiny
proxy that maps `model` field → upstream port. The single-mutex design
inside one server is the right unit; between servers you scale by process.

---

## Part 10 — troubleshooting

### "load failed: failed to load model"

* Did the GGUF download fully? Check the file size; small files often mean
  HTML 404 pages.
* Wrong architecture? llama.cpp prints the supported-arch list during load
  with `--verbose`. Add `engine.verbose(true)`.
* On macOS, run `xcode-select --install` once if Metal headers are missing.

### "context size exceeded"

Conversations grow until the KV cache fills. Either:

* `engine.clear_history()` between turns
* `engine.context(8192)` for a longer window (subject to model training)
* In the server, this can't happen because every request resets the engine.

### Model emits garbled tool calls (e.g. `{{` and `}}`)

Smaller models (≤ 1B parameters) often miss the chat template's tool-call
syntax. easyai catches the parser exception and returns the raw text as the
assistant message. To see what the model emitted, set `engine.verbose(true)`.

Move up to a 3-7B model with native tool-calling support (Qwen2.5-Instruct,
Llama-3.1-Instruct, Mistral-Nemo) and the issue disappears.

### "unknown tool: …"

The model invented a tool name that isn't registered. easyai injects a
`ToolResult::error("unknown tool: …")` into the conversation; usually the
model recovers next turn. If it doesn't, lower the temperature or be
more specific in your system prompt.

### "permission denied" / "path escapes sandbox"

A filesystem tool was called with a path outside the root you passed to
`fs_read_file("…")` etc. By design — pick a wider root or move the file in.

### Server returns 500 with `engine error: …`

Something inside `chat()` threw. The engine remains usable for the next
request (we lock + reset on every call). Check `engine.verbose(true)` and
re-run for stack-level detail in `stderr`.

### `pkill -INT easyai-server` gives exit code 1

It's actually fine — the printed line "stopped cleanly" tells the truth.
Some shells/wrappers report a non-zero code because of the signal, but
`main()` returned 0.

### Forcing the model to ignore the server-injected AUTHORITATIVE preamble (QA only)

The server appends an AUTHORITATIVE preamble to its OWN default
system prompt (`--inject-datetime on` is the default). Since
2026-06-12, a CLIENT-supplied `system` message is used **verbatim** —
it replaces the server default and nothing is spliced into it; pass
`X-Easyai-Inject: on` to opt the preamble back onto your own system
message. The preamble has up to three blocks:

* `# AUTHORITATIVE DATE/TIME` — current wall-clock + timezone.
* `# KNOWLEDGE CUTOFF` — training-cutoff hint + rule to verify
  post-cutoff facts.
* `# MEMORY VOCABULARY` — top-40 keyword index when `--memory`
  is set (so the model can dispatch `search_knowledge`
  without first calling `keywords_knowledge`).

For regression testing the preamble can be disabled per-request
without restarting the server:

```bash
curl http://ai.local:8080/v1/chat/completions \
     -H 'Content-Type: application/json' \
     -H 'X-Easyai-Inject: off' \
     -d '{"model":"easyai","messages":[{"role":"user","content":"What year is it?"}]}'
```

Header values:
* `off` — skip the preamble for this request only.
* `on`  — force injection on this request even when the server was
          launched with `--inject-datetime off`. With a client-supplied
          `system` message this is ALSO the opt-in that appends the
          preamble to it (the default for client prompts is verbatim,
          nothing appended).
* (anything else, or absent header) — defer to the server flag; a
          client-supplied `system` message stays verbatim.

WHY DEFAULT ON: most production deployments want the model to trust
the server clock, flag post-cutoff facts as uncertain, and know
what's in its persistent memory. Turning the preamble off removes a
real safety net — only do it for A/B QA runs where you're explicitly
comparing pre-injection behaviour.

### Calling the preamble builder from your own code

The same builder is exposed as a library API (since 2026-05-16) so
third-party hosts of libeasyai get the same behaviour without
copying the format:

```cpp
#include "easyai/preamble.hpp"

std::string preamble = easyai::preamble::build({
    /* inject_datetime  = */ true,
    /* knowledge_cutoff = */ "2024-10",
    /* memory_root      = */ "/var/lib/myapp/rag",
});

engine.system(default_system + preamble);
```

Each block is conditional: pass `inject_datetime=false` to skip the
date/time + cutoff blocks (useful when the remote server already
handles them), pass `memory_root=""` to skip the memory vocabulary
block. The function is stateless — recomputes every call (fresh
date, fresh directory scan for the memory index). Safe to call on
the hot path; the directory scan is ~10-50ms for typical stores.

### `easyai::Client` — "HTTPS endpoint requires OpenSSL"

Configure-time message in the cmake summary:

```
-- easyai-cli: OpenSSL NOT found — HTTPS endpoints will be rejected at runtime
```

Install `libssl-dev` (Debian / Ubuntu) or `openssl-devel` (Fedora /
RHEL), wipe the build dir, and reconfigure.  At runtime the
`Client::endpoint("https://…")` call will then succeed.

If your server uses a self-signed cert, either:

* `cli.tls_insecure(true);` — DEV ONLY, skips peer verification.
* `cli.ca_cert_path("/path/to/ca.pem");` — trust a custom CA bundle.

The `cli-remote` binary exposes the same as `--insecure-tls` and
`--ca-cert PATH`.

### `easyai::Client` — "HTTP request failed: …"

The full text after the colon is what cpp-httplib reported.  Note
that the client retries transient failures up to **5 times by
default** (configurable via `--http-retries N` / `cli.http_retries(n)`
/ `EASYAI_HTTP_RETRIES`); each retry logs to stderr without
`--verbose`:

```
[easyai-cli] HTTP attempt 1/6 failed (Could not establish connection); retrying in 250ms
[easyai-cli] HTTP attempt 2/6 failed (Could not establish connection); retrying in 500ms
…
[easyai-cli] HTTP attempt 6/6 failed (…) — retry budget exhausted
```

If you see the budget-exhausted line, the underlying cause is one of:

* `Connection refused` — the server isn't listening on that
  host/port. Check `--url` value and `nc -vz host port`.
* `SSL handshake failed` — TLS mismatch.  Check the cert hostname
  matches what you're connecting to, the chain is complete, and that
  your client's CA store has the issuer (or pass `--ca-cert`).
* `read timeout` / `Failed to read connection` — the model is taking
  longer than `--timeout`.  Default is now 86400 s (24 h), raised
  from the older 600 s default specifically to accommodate thinking
  models with long deliberation phases.  Bump further if needed
  (`--timeout 3600` or `cli.timeout_seconds(3600)`); also bump the
  server side (`easyai-server --http-timeout 3600`) so the listen
  socket matches.

Retries do NOT fire mid-stream — once the model has emitted any
visible token the layer surfaces the partial response instead of
re-issuing (which would duplicate output).  For mid-stream cuts the
fix is the timeout, not the retry budget.

### `cli-remote` — `Output: 0 / 128000 (0%)` even after a long reply

The cumulative ctx counter on `easyai-server`'s webui needs the new
`ctx_used` field that the server only added in commit `d7f638e`.
On older builds you'll see the per-request count instead — upgrade
the server or just ignore the bar's percentage.

### Model "abandons" `<tool_call>` mid-conversation, emits markdown

After 2-3 successful tool calls some Qwen3 fine-tunes give up on the
XML format and output `*🔧 toolname(args)*` in markdown instead.
Engine recovers automatically (commit `46903e3`); look for this line
in `journalctl -u easyai-server`:

```
[easyai] recovered N tool call(s) from markdown markers (model abandoned <tool_call> syntax — agentic loop continues)
```

If you see the message and the loop continues, you're fine.  If not,
add `--enable-verbose` and check `journalctl` for `[easyai] hop N
raw tail:` lines — those show what the model actually emitted, which
helps tune the system prompt.

### `search_web` — "no results parsed (DuckDuckGo may have rate-limited…)"

DuckDuckGo's HTML endpoint serves a CAPTCHA / "anomaly" page when it
suspects a bot.  Wait a minute, lower request rate, or use a
different network.  No API key option exists — that's the point of
the DDG-HTML approach.

### Conversation feels stale / model insists on outdated info

Knowledge cutoff is real and the model can't tell what date it is
unless told.  Easiest fix: enable `datetime` in the tool list and
prompt it to call that first when in doubt.  An even harder
constraint can be enforced at the server level — see the upcoming
"authoritative datetime injection" feature on easyai-server (commit
soon to follow).

---

## Part 11 — design references

If you want to go deeper:

* `design.md` — internal architecture and "why" decisions, including
  Section 0 (full dependency inventory: llama, cpp-httplib,
  nlohmann::json, libcurl, OpenSSL, …) and Section 5b (the
  OpenAI-protocol client agentic loop).
* `include/easyai/engine.hpp` — every public method of the local
  engine, with doc comments.
* `include/easyai/client.hpp` — every public method of the OpenAI
  client lib, mirroring `engine.hpp` shape.
* `include/easyai/tool.hpp` — `Tool`, `ToolCall`, `ToolResult`,
  `Tool::Builder` (used identically by `Engine` and `Client`).
* `include/easyai/plan.hpp` — `Plan` checklist + `Plan::tool()`
  factory.
* `include/easyai/builtin_tools.hpp` — factories for `datetime`,
  `search_web`, `fetch_web`, `*_file`.
* `include/easyai/presets.hpp` — sampling presets and the runtime
  override parser (`/temp`, `creative 0.9`, …).
* `src/engine.cpp` — the `chat()` loop is annotated step by step;
  three-layer tool-call recovery (Qwen / Hermes / markdown) lives in
  `parse_assistant`.
* `src/client.cpp` — HTTP/SSE transport, agentic loop mirroring
  `Engine::chat_continue`, request-body assembly with the full
  sampling/penalty surface.
* `src/plan.cpp` — multi-action plan tool with `add/start/done/list`.
* `examples/server.cpp` — the per-request flow is annotated; great
  starting point for a custom HTTP layer.
* `examples/cli.cpp` — REPL + management subcommands +
  inline `system_*` tools, doubles as the cookbook for adding your
  own tool to a `Client`-based agent.
* `scripts/install_easyai_server.sh` — production deployment as a
  hardened systemd unit on Linux (CUDA / ROCm / Vulkan / CPU
  auto-detect, mlock, flash-attn, q8_0 KV).
* `cmake/easyaiConfig.cmake.in` — the find_package shim;
  `find_package(easyai 0.1 REQUIRED)` returns
  `easyai::engine` (libeasyai) and `easyai::cli` (libeasyai-cli) as
  IMPORTED targets your project links against.
* `SESSION_NOTES.md` — running project journal: recent commits,
  pending validations, common pitfalls.  Useful for resuming context
  in a fresh chat.
* `README.md` — top-level pitch + selective-build cheatsheet.

Happy hacking.
