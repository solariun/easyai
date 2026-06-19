// services/chat.cpp — minimal REPL.  Showcases easyai::Agent: the
// "easy" front door over Engine/Client/Backend.  No spinner, no
// fancy formatting — just the smallest thing that actually works.
//
//   ./easyai-chat -m models/qwen2.5-0.5b-instruct.gguf
//   ./easyai-chat --url http://127.0.0.1:8080/v1
//
#include "easyai/easyai.hpp"

#include <cstdio>
#include <iostream>
#include <string>

int main(int argc, char ** argv) {
    std::string model_path, url;
    std::string system_prompt = "You are a helpful, concise assistant.";
    bool show_system_prompt = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-m"       && i + 1 < argc) model_path     = argv[++i];
        else if (a == "--url"    && i + 1 < argc) url            = argv[++i];
        else if (a == "--system" && i + 1 < argc) system_prompt  = argv[++i];
        else if (a == "--show-system-prompt")     show_system_prompt = true;
        else {
            std::fprintf(stderr,
                "usage: %s (-m model.gguf | --url base) [--system PROMPT] "
                "[--show-system-prompt]\n",
                argv[0]);
            return 1;
        }
    }

    // --show-system-prompt: dump the resolved prompt and exit before any
    // backend is loaded or any network call is made. Doesn't need -m / --url.
    if (show_system_prompt) {
        std::fputs(system_prompt.c_str(), stdout);
        std::fputc('\n', stdout);
        return 0;
    }

    if (model_path.empty() && url.empty()) {
        std::fprintf(stderr, "need -m model.gguf or --url base\n");
        return 1;
    }

    // Three-line agent: pick a backend, set system, stream tokens.
    easyai::Agent agent = url.empty()
                              ? easyai::Agent(model_path)
                              : easyai::Agent::remote(url);
    agent.system  (system_prompt)
         .on_token([](const std::string & p){ std::cout << p << std::flush; });

    std::string line;
    while (std::cout << "\n\033[32m> \033[0m" && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::cout << "\033[33m";
        agent.ask(line);
        std::cout << "\033[0m\n";
    }
    return 0;
}
