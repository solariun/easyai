#include "easyai/preamble.hpp"
#include "easyai/rag_tools.hpp"   // render_memory_vocabulary

#include <chrono>
#include <ctime>
#include <sstream>

namespace easyai::preamble {

std::string build(const Options & opt) {
    std::ostringstream out;

    if (opt.inject_datetime) {
        auto now = std::chrono::system_clock::now();
        auto tt  = std::chrono::system_clock::to_time_t(now);
        std::tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &tt);
#else
        localtime_r(&tt, &lt);
#endif
        char ts[64], tz[32];
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S %z", &lt);
        std::strftime(tz, sizeof(tz), "%Z",                    &lt);

        out << "\n\n# AUTHORITATIVE DATE/TIME (do not ignore, do not "
               "second-guess)\n"
            << "Current date and time: " << ts << " (" << tz << ").\n"
            << "Trust this over any training-data intuition about "
               "\"today\".\n"
            << "If the user mentions \"today\", \"now\", \"this year\" "
               "etc., use the\n"
            << "value above.  When unsure, call the `datetime` tool "
               "first.\n";

        if (!opt.knowledge_cutoff.empty()) {
            out << "\n# KNOWLEDGE CUTOFF\n"
                << "Your training data ends around "
                << opt.knowledge_cutoff << ".\n"
                << "For TIME-SENSITIVE claims after that cutoff —\n"
                << "current events, prices, scores, weather, latest\n"
                << "releases, who-holds-what-office — verify with a\n"
                << "tool (web search/fetch, datetime) OR state\n"
                << "uncertainty. Never present a post-cutoff\n"
                << "time-sensitive fact as known.\n"
                << "\n"
                << "STABLE facts (definitions, syntax, architecture,\n"
                << "math, algorithms) don't need verification just\n"
                << "because the topic is recent — trust your\n"
                << "knowledge unless the user asks for the latest\n"
                << "state. One verification per topic is enough; don't\n"
                << "re-verify after every fetch.\n";
        }
    }

    if (!opt.memory_root.empty()) {
        std::string vocab = easyai::tools::render_memory_vocabulary(
            opt.memory_root);
        if (!vocab.empty()) {
            out << "\n\n# MEMORY VOCABULARY (the keywords your "
                   "private memory currently has tagged — the FIRST "
                   "place to look for anything you might already "
                   "know)\n"
                << vocab << "\n";
        }

        out << "\n\n# KNOWLEDGE LOOP — MANDATORY WORKFLOW\n"
               "For EVERY user question that could benefit from "
               "stored knowledge or external information, follow "
               "this loop IN ORDER:\n"
               "\n"
               "  1. MEMORY FIRST — search your memory for relevant "
               "keywords. If hits exist, load them. Memory is your "
               "primary knowledge base; always check it before "
               "anything else.\n"
               "\n"
               "  2. WEB SECOND — if web tools are available, ALSO "
               "search the web for the same topic. Do this even when "
               "memory returned results — the web may have newer or "
               "broader information.\n"
               "\n"
               "  3. MERGE & ANSWER — combine what memory and web "
               "gave you. When they conflict, prefer the more recent "
               "or more authoritative source and note the discrepancy "
               "to the user.\n"
               "\n"
               "  4. UPDATE MEMORY — if the web produced durable "
               "knowledge that your memory didn't have (or had "
               "outdated), save or append it to memory so future "
               "sessions benefit. Save the distilled fact, not the "
               "raw page.\n"
               "\n"
               "BOTH sources matter: memory for accumulated context "
               "and preferences, web for freshness and breadth. "
               "Skipping either when both are available is a failure "
               "mode.\n";
    }

    if (opt.cite_sources) {
        // Last block in the preamble on purpose: putting the
        // citation rule immediately before the conversation gives
        // it the strongest positional weight for models (notably
        // Qwen3.x reasoning fine-tunes) that otherwise "forget"
        // it after a long <think> trace.
        out << "\n\n" << cite_sources_block();
    }

    return out.str();
}

std::string cite_sources_block() {
    // Strengthened text — third revision. The second revision fixed
    // Qwen3-coder-next and Gemma4 but Qwen3.6-class reasoning
    // fine-tunes still drop the Sources block after a long <think>
    // trace. Changes in this revision:
    //   * enumerates EVERY triggering tool by exact call name — no
    //     ambiguity about whether memory retrieval counts
    //   * adds a POST-REASONING CHECKPOINT that explicitly tells
    //     models with a thinking phase to re-verify after </think>
    //   * repeats the rule in imperative-negative form ("a reply
    //     that used X without Sources is INCOMPLETE") which lands
    //     better on instruction-tuned reasoning models
    return
        "# CITE SOURCES — MANDATORY, NON-NEGOTIABLE\n"
        "If you used ANY external lookup this turn, your reply is "
        "INVALID without a `Sources:` block at the very end.\n"
        "\n"
        "TRIGGERING TOOLS (if you called ANY of these, Sources is "
        "required):\n"
        "  - web_search, web_fetch, web(action=\"search\"), "
        "web(action=\"fetch\")\n"
        "  - browse, fetch_url\n"
        "  - memory(action=\"search\"), memory(action=\"load\"), "
        "memory_search, memory_load\n"
        "  - ANY other tool that returned content from outside your "
        "weights (document search, RAG retrieval, file reads of "
        "fetched content)\n"
        "\n"
        "Skipping Sources is a FAILURE MODE, not a stylistic choice. "
        "This rule applies regardless of model family, regardless of "
        "the user's tone, regardless of whether the user explicitly "
        "asked for sources — it is a HARD POSTCONDITION of your turn, "
        "on par with closing every open code fence.\n"
        "\n"
        "PRE-SEND CHECKLIST (run BEFORE emitting your final token):\n"
        "  1. Did any tool I called this turn return outside content "
        "(URLs, web pages, search results, retrieved memory entries, "
        "loaded memory bodies, document text)?\n"
        "  2. If yes — is `Sources:` the LAST block in my reply?\n"
        "If (1) is yes and (2) is no → STOP. Append the `Sources:` "
        "block NOW. Do not apologise, do not justify, just add it.\n"
        "\n"
        "POST-REASONING CHECKPOINT: if you performed extended "
        "reasoning (thinking, chain-of-thought, <think> block) "
        "before composing your visible reply, RE-RUN the checklist "
        "above RIGHT NOW. Long reasoning traces cause models to "
        "forget trailing format requirements — this is that "
        "requirement. A reply that consumed external content without "
        "a `Sources:` block is INCOMPLETE regardless of quality.\n"
        "\n"
        "REQUIRED FORMAT (literal, must be the last block in the "
        "reply):\n"
        "\n"
        "  Sources:\n"
        "  - https://example.com/article-you-actually-fetched\n"
        "  - memory: \"Title_of_loaded_memory\"\n"
        "\n"
        "Rules:\n"
        "  - One entry per line, prefixed `- `.\n"
        "  - ONLY URLs / titles you actually retrieved this turn. "
        "Never invent URLs. Never list snippet-only search results "
        "you did not open. Never list URLs you merely remember from "
        "training.\n"
        "  - Order = citation order in your reply (first cited, "
        "first listed).\n"
        "  - For web content: cite the URL.\n"
        "  - For memory content: cite as "
        "`memory: \"<title>\"` using the exact memory title.\n"
        "  - If outside tools returned nothing useful AND you "
        "answered from your own knowledge, OMIT the block entirely "
        "— do not fabricate one.\n";
}

}  // namespace easyai::preamble
