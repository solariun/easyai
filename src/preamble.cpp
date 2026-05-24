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
        //
        // Auto-derive has_memory from memory_root so callers that
        // already pass memory_root don't also need to set the bool —
        // the two flags are virtually always in sync. Explicit
        // opt.has_memory still wins (lets a caller force the memory
        // bullets on even with no memory_root, e.g. for a client-tools
        // proxy that exposes memory via a remote name).
        const bool has_memory = opt.has_memory || !opt.memory_root.empty();
        out << "\n\n" << cite_sources_block(has_memory);
    }

    return out.str();
}

std::string cite_sources_block(bool has_memory) {
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
    std::ostringstream out;
    out <<
        "# CITE SOURCES — MANDATORY, NON-NEGOTIABLE\n"
        "If you used ANY external lookup this turn, your reply is "
        "INVALID without a `Sources:` block at the very end.\n"
        "\n"
        "TRIGGERING CATEGORIES (if you called any tool in these "
        "categories, Sources is required). The CALLABLE tool names "
        "are in AVAILABLE TOOLS above — never copy the descriptions "
        "below as tool names, they are categories, not callables:\n"
        "  - WEB TOOLS — anything that fetched a URL or searched the "
        "internet (e.g. web_search, web_fetch, browse, fetch_url, or "
        "a unified web dispatcher)\n";
    if (has_memory) {
        // Gated on memory being registered: when memory is off, telling
        // the model memory tools trigger Sources is a lie that nudges
        // it to invent calls to a non-existent memory tool.
        out <<
            "  - MEMORY / RAG TOOLS — anything that searched or loaded "
            "persistent memory (e.g. memory_search, memory_load, or a "
            "unified memory dispatcher)\n";
    }
    out <<
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
    return out.str();
}

namespace {

// Pull the `action` enum out of a JSON-schema parameters blob via a
// loose substring scan — we deliberately avoid pulling nlohmann into
// this translation unit. Returns the values verbatim, in order, or
// empty if there is no `action` enum at all (i.e. the tool is not a
// composite multi-action dispatcher).
//
// The scan is intentionally conservative: it locates `"action"` then
// looks for `"enum"` within the next ~400 chars (the action property
// is virtually always declared as `{"type":"string","enum":[…]}` and
// the schema strings we ship are compact). False positives — picking
// up an unrelated enum on a different property that happens to follow
// `action` — are acceptable: the worst case is we render an extra
// `(action="…")` annotation on the tool, which still helps the model.
std::vector<std::string> extract_action_enum(const std::string & params_json) {
    std::vector<std::string> out;
    size_t a = params_json.find("\"action\"");
    if (a == std::string::npos) return out;
    size_t e = params_json.find("\"enum\"", a);
    if (e == std::string::npos || e - a > 400) return out;
    size_t lb = params_json.find('[', e);
    if (lb == std::string::npos) return out;
    size_t rb = params_json.find(']', lb);
    if (rb == std::string::npos) return out;
    for (size_t i = lb + 1; i < rb; ) {
        size_t q1 = params_json.find('"', i);
        if (q1 == std::string::npos || q1 >= rb) break;
        size_t q2 = params_json.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 >= rb) break;
        out.push_back(params_json.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return out;
}

// Return the first non-empty line of a multi-line description, with
// trailing whitespace trimmed. Tool descriptions in easyai are
// "one-line summary, blank line, detail rules" so the first line is a
// good one-liner; we don't want to dump the entire description into
// the catalogue (would blow the token budget).
std::string first_description_line(const std::string & d) {
    size_t start = 0;
    while (start < d.size() && (d[start] == '\n' || d[start] == '\r' ||
                                d[start] == ' '  || d[start] == '\t')) {
        ++start;
    }
    size_t nl = d.find('\n', start);
    std::string r = (nl == std::string::npos)
                        ? d.substr(start)
                        : d.substr(start, nl - start);
    while (!r.empty() && (r.back() == ' ' || r.back() == '\t' ||
                          r.back() == '\r')) {
        r.pop_back();
    }
    return r;
}

}  // anonymous

std::string build_session_info(const std::vector<easyai::Tool> & tools) {
    if (tools.empty()) return std::string();

    std::ostringstream out;
    out << "\n\n# AVAILABLE TOOLS — call ONLY these names this session\n"
           "These are the EXACT tools registered in your session. The "
           "names are case-sensitive. Calling a name NOT in this list "
           "returns `unknown tool` and wastes the turn.\n\n";

    for (const auto & t : tools) {
        out << "  - " << t.name;
        const auto actions = extract_action_enum(t.parameters_json);
        if (!actions.empty()) {
            out << "(action=";
            for (size_t i = 0; i < actions.size(); ++i) {
                if (i) out << '|';
                out << '"' << actions[i] << '"';
            }
            out << ")";
        }
        const std::string fl = first_description_line(t.description);
        if (!fl.empty()) {
            // Cap the one-liner so a verbose first line doesn't blow up
            // the catalogue (some descriptions cram a whole sentence in
            // line 1; 140 bytes is enough to convey purpose without
            // turning the block into a wall).
            //
            // UTF-8 safety: substr() works on bytes, so a naive cut can
            // split a multi-byte codepoint and produce invalid UTF-8.
            // The eventual JSON serialisation
            // (client.cpp / build_chat_body via nlohmann) then aborts
            // with json.exception.type_error.316. Walk back from the
            // cut point to the previous codepoint boundary — a UTF-8
            // continuation byte has top bits 10xxxxxx, lead bytes do
            // not. Worst case we lose a few extra bytes from the
            // one-liner; the catalogue stays valid.
            if (fl.size() > 140) {
                size_t cut = 137;
                while (cut > 0
                       && (static_cast<unsigned char>(fl[cut]) & 0xC0) == 0x80) {
                    --cut;
                }
                out << ": " << fl.substr(0, cut) << "...";
            } else {
                out << ": " << fl;
            }
        }
        out << '\n';
    }

    out << "\n# VERIFY BEFORE YOU CALL — UNBREAKABLE RULE\n"
           "The AVAILABLE TOOLS list above is the COMPLETE set of "
           "callable names this session. The tool name you put in a "
           "tool_call MUST be one of those names — exactly, "
           "case-sensitive, with NOTHING else attached.\n"
           "\n"
           "Two failure modes that waste the turn:\n"
           "\n"
           "1. Calling a SUB-ACTION as if it were a tool. Composite "
           "tools (those shown above with `action=\"…\"`) dispatch "
           "via the `action` parameter; the sub-action is NEVER "
           "callable on its own. Example:\n"
           "  WRONG: update(items=[…])\n"
           "  RIGHT: plan(action=\"update\", items=[…])\n"
           "\n"
           "2. Putting CALL SYNTAX into the tool name field. The "
           "tool_call name slot takes ONLY the bare tool name; "
           "arguments go in the arguments field. Never include "
           "parentheses, never include `action=\"…\"`, never include "
           "argument values in the name. Example:\n"
           "  WRONG: name=\"memory_search(keywords=[\\\"x\\\"])\"\n"
           "  RIGHT: name=\"memory_search\", "
           "arguments={\"keywords\":[\"x\"]}\n"
           "\n"
           "If you are about to invoke a tool name you have NOT seen "
           "in the AVAILABLE TOOLS list above THIS turn, call "
           "`tool_lookup()` FIRST to confirm it is registered. A "
           "no-match result from `tool_lookup` is authoritative — do "
           "NOT retry variations of the same name.\n";

    return out.str();
}

}  // namespace easyai::preamble
