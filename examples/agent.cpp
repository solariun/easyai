// examples/agent.cpp — agent with the full built-in toolbelt + a custom tool.
//
//   ./easyai-agent -m models/qwen2.5-0.5b-instruct.gguf
//
// Try prompts like:
//   "What time is it right now in UTC?"
//   "Search the web for 'llama.cpp release notes' and summarize the top hit."
//   "List the files in the current directory."
//
#include "easyai/easyai.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char ** argv) {
    std::string model_path;
    int n_ctx = 8192, ngl = -1;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-m"   && i + 1 < argc) model_path = argv[++i];
        else if (a == "-c"   && i + 1 < argc) n_ctx      = std::stoi(argv[++i]);
        else if (a == "-ngl" && i + 1 < argc) ngl        = std::stoi(argv[++i]);
        else { std::fprintf(stderr, "usage: %s -m model.gguf [-c ctx] [-ngl n]\n", argv[0]); return 1; }
    }
    if (model_path.empty()) { std::fprintf(stderr, "missing -m\n"); return 1; }

    easyai::Engine engine;
    engine.model(model_path)
          .context(n_ctx)
          .gpu_layers(ngl)
          .system(
            "You are a helpful agent with access to tools. "
            "Use them to answer the user's question, then reply concisely. "
            "When unsure of facts, prefer web(action=\"search\") + web(action=\"fetch\").")
          .add_tool(easyai::tools::datetime())
          .add_tool(easyai::tools::web())
          .add_tool(easyai::tools::fs("."))
          // Example of a custom tool defined inline — fewer than 10 lines.
          .add_tool(
              easyai::Tool::builder("flip_coin")
                  .describe("Returns 'heads' or 'tails' uniformly at random.")
                  .handle([](const easyai::ToolCall &) {
                      return easyai::ToolResult::ok((std::rand() & 1) ? "heads" : "tails");
                  })
                  .build())
          // tool_lookup — last, so its snapshot covers everything above
          // (including itself).  The lambda re-reads engine.tools() at
          // every call, so no race with later additions.
          .add_tool(easyai::tools::tool_lookup([&engine]() {
              std::vector<std::pair<std::string, std::string>> v;
              v.reserve(engine.tools().size());
              for (const auto & t : engine.tools()) {
                  v.emplace_back(t.name, t.description);
              }
              return v;
          }))
          .on_token([](const std::string & p){ std::cout << p << std::flush; })
          .on_tool([](const easyai::ToolCall & c, const easyai::ToolResult & r) {
              std::fprintf(stderr, "\n\033[32m● %s(%s) -> %s%s\033[0m\n",
                           c.name.c_str(),
                           c.arguments_json.c_str(),
                           r.is_error ? "ERR: " : "",
                           r.content.substr(0, 200).c_str());
          });

    if (!engine.load()) {
        std::fprintf(stderr, "load failed: %s\n", engine.last_error().c_str());
        return 1;
    }
    std::fprintf(stderr, "[easyai] loaded; backend = %s; %zu tools\n",
                 engine.backend_summary().c_str(), engine.tools().size());

    std::string line;
    while (true) {
        std::cout << "\n\033[32m> \033[0m";
        if (!std::getline(std::cin, line) || line.empty()) break;
        std::cout << "\033[33m";
        engine.chat(line);
        std::cout << "\033[0m\n";
    }
    return 0;
}
