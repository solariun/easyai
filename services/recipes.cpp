// services/recipes.cpp
// ----------------------------------------------------------------------------
// "Recipe book" sample agent.  Companion to manual.md → "Writing your own
// tools".  Demonstrates how a regular C++ developer (no llama.cpp / no JSON
// schema knowledge) builds custom tools and hands them to the engine.
//
// Two custom tools live in this file:
//
//   * today_is   — zero-parameter tool. Returns today's date in YYYY-MM-DD.
//                  The simplest possible shape.
//
//   * weather    — one-parameter tool ("city").  Calls https://wttr.in (free,
//                  no API key) and returns its one-line summary.  Shows how
//                  to talk to an external HTTP service from inside a tool.
//
// Run it after a build:
//
//   ./build/easyai-recipes path/to/model.gguf
//
// Without an argument it tries ./models/qwen2.5-1.5b-instruct-q4_k_m.gguf.
// ----------------------------------------------------------------------------

#include "easyai/easyai.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <string>

#include <curl/curl.h>   // example needs libcurl explicitly (we link to it).

// ============================================================================
// Recipe 1 — the tiniest possible tool
//
// Zero parameters.  No JSON to parse.  Returns a string.
// ============================================================================
static easyai::Tool today_is() {
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

// ============================================================================
// Recipe 2 — calling an external HTTP service
//
// One required string parameter ("city").  Pulls the value via the helper
// `args::get_string_or`, calls wttr.in, returns whatever wttr.in says (or a
// nicely-worded error if the network call fails).
// ============================================================================

// libcurl writes the response body in chunks; this captures them into a
// std::string.  Pure boilerplate — copy-paste it into your own tools.
static size_t capture_body(char * ptr, size_t sz, size_t nm, void * ud) {
    auto * out = static_cast<std::string *>(ud);
    out->append(ptr, sz * nm);
    return sz * nm;
}

static easyai::Tool weather() {
    return easyai::Tool::builder("weather")
        .describe("Returns the current weather for a city. "
                  "Backed by wttr.in — free, no API key, plain-text reply.")
        .param("city", "string",
               "City name, e.g. 'Berlin' or 'Sao Paulo'.  Required.",
               /*required=*/true)
        .handle([](const easyai::ToolCall & call) {
            // The model puts arguments in a JSON blob; pluck the string out
            // with a one-liner.  Falls back to "" if missing.
            std::string city = easyai::args::get_string_or(
                call.arguments_json, "city", "");
            if (city.empty()) {
                return easyai::ToolResult::error("missing 'city' argument");
            }

            CURL * h = curl_easy_init();
            if (!h) return easyai::ToolResult::error("curl init failed");

            // URL-escape the city so spaces / accented chars survive.
            char * escaped = curl_easy_escape(h, city.c_str(), 0);
            std::string url = "https://wttr.in/";
            url += escaped ? escaped : city.c_str();
            url += "?format=3";   // one-line summary, e.g. "Berlin: ☀ +18°C"
            if (escaped) curl_free(escaped);

            std::string body;
            curl_easy_setopt(h, CURLOPT_URL,            url.c_str());
            curl_easy_setopt(h, CURLOPT_USERAGENT,      "easyai-recipes/0.1");
            curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(h, CURLOPT_TIMEOUT,        15L);
            curl_easy_setopt(h, CURLOPT_WRITEFUNCTION,  capture_body);
            curl_easy_setopt(h, CURLOPT_WRITEDATA,      &body);

            CURLcode rc = curl_easy_perform(h);
            long      code = 0;
            curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
            curl_easy_cleanup(h);

            if (rc != CURLE_OK) {
                return easyai::ToolResult::error(
                    std::string("HTTP transport error: ") + curl_easy_strerror(rc));
            }
            if (code >= 400) {
                return easyai::ToolResult::error(
                    "wttr.in returned HTTP " + std::to_string(code));
            }
            // Trim the trailing newline wttr.in always sends.
            while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
                body.pop_back();
            }
            return easyai::ToolResult::ok(body);
        })
        .build();
}

// ============================================================================
// main — wire the tools, load the model, ask one question.
// ============================================================================
int main(int argc, char ** argv) {
    const char * model_path =
        (argc > 1) ? argv[1]
                   : "models/qwen2.5-1.5b-instruct-q4_k_m.gguf";

    easyai::Engine engine;
    engine.model(model_path)
          .context(4096)
          .gpu_layers(99)                      // -1 = auto-fit; 0 = CPU only
          .system("You are a concise assistant.  Use tools whenever they "
                  "help and cite the tool's reply in your answer.")
          .add_tool(today_is())
          .add_tool(weather())
          .add_tool(easyai::tools::tool_lookup([&engine]() {
              // Last-registered: snapshot includes today_is, weather,
              // and tool_lookup itself.  Lets the model verify what's
              // wired up before guessing tool names.
              easyai::tools::ToolCatalog v;
              v.reserve(engine.tools().size());
              for (const auto & t : engine.tools()) {
                  v.push_back({ t.name, t.wire_description(), t.description });
              }
              return v;
          }))
          .on_token([](const std::string & piece) {
              std::cout << piece << std::flush;
          });

    if (!engine.load()) {
        std::fprintf(stderr, "[recipes] load failed: %s\n",
                     engine.last_error().c_str());
        return 1;
    }

    std::cout << "[recipes] backend=" << engine.backend_summary()
              << "  ctx=" << engine.n_ctx()
              << "  tools=" << engine.tools().size() << "\n\n";

    // One canned question that exercises both tools.  Replace with whatever
    // you want, or wrap in a loop to make a REPL — see services/chat.cpp.
    engine.chat("What's today's date, and what's the weather in Sao Paulo right now?");
    std::cout << "\n";
    return 0;
}
