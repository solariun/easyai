// easyai/agent.hpp — friendly-front-door facade.
//
// The 30-second pitch:
//
//   #include "easyai/easyai.hpp"
//
//   int main() {
//       easyai::Agent a("models/qwen2.5-1.5b-instruct.gguf");
//       std::cout << a.ask("What time is it in Tokyo right now?") << "\n";
//   }
//
// That's it.  Three lines: construct, ask, print.  No model load
// boilerplate, no template wiring, no manual tool registration —
// datetime + the unified `web` tool are wired in by default. Stream the
// reply with `.on_token([](auto p){ std::cout << p << std::flush; })`.
//
// All the power of the underlying Engine/Client/Backend is still one
// step away: `Agent::backend()` exposes the configured `Backend &`,
// and any field you care about can be set fluently before the first
// ask().  Pay-as-you-go: easy when you want easy, full control when
// you need it.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace easyai {

class Backend;

class Agent {
public:
    using Tokenizer = std::function<void(const std::string &)>;

    // -----------------------------------------------------------------
    // Construction.
    // -----------------------------------------------------------------

    // Local model — pass a GGUF file path.
    //
    //   easyai::Agent a("models/qwen3-3B-instruct-q4_k_m.gguf");
    explicit Agent(std::string model_path);

    // Remote endpoint — pass an OpenAI-compatible base URL.
    // Optional bearer token if the server requires auth.
    //
    //   auto a = easyai::Agent::remote("http://127.0.0.1:8080/v1");
    //   auto a = easyai::Agent::remote("https://api.openai.com/v1",
    //                                  std::getenv("OPENAI_API_KEY"));
    static Agent remote(std::string base_url, std::string api_key = {});

    ~Agent();
    Agent(Agent &&) noexcept;
    Agent & operator=(Agent &&) noexcept;
    Agent(const Agent &)             = delete;
    Agent & operator=(const Agent &) = delete;

    // -----------------------------------------------------------------
    // Fluent customisation — applied lazily, before the first ask().
    // After the first ask(), only set_system() / sampling / on_token
    // changes propagate; structural config (sandbox, allow_bash,
    // model, url) is locked in.
    // -----------------------------------------------------------------
    Agent & system     (std::string prompt);     // system prompt
    Agent & sandbox    (std::string dir);        // enable *_file tools, scoped here
    Agent & allow_bash (bool on = true);         // enable the bash tool
    Agent & preset     (std::string name);       // "deterministic"…"wild"
    Agent & remote_model (std::string id);       // remote-mode only ("gpt-4o-mini" etc)
    Agent & temperature(float t);
    Agent & top_p      (float p);
    Agent & top_k      (int   k);
    Agent & min_p      (float p);
    Agent & on_token   (Tokenizer cb);           // streaming callback

    // -----------------------------------------------------------------
    // Conversation.
    // -----------------------------------------------------------------

    // One-shot: send `text`, run any tool calls inline, return the
    // agent's final visible reply.  Throws std::runtime_error on
    // model load / transport failure (caught + last_error()-able for
    // those who prefer the C-style flow).
    std::string ask(const std::string & text);

    // Wipe history — next ask() starts a fresh conversation.
    void reset();

    // Diagnostic: most recent error (model load, HTTP, tool dispatch).
    std::string last_error() const;

    // Escape hatch — full Backend access for everything Agent doesn't
    // surface directly (custom tools, telemetry, multi-turn workflows
    // with tool_calls visible mid-stream, etc).
    Backend & backend();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace easyai
