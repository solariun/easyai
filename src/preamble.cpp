#include "easyai/preamble.hpp"
#include "easyai/rag_tools.hpp"   // render_memory_vocabulary

#include <chrono>
#include <cstddef>
#include <ctime>
#include <sstream>

namespace easyai::preamble {

namespace {

// Strip C0 control bytes (0x00–0x1f) and DEL (0x7f) from `s`, collapse
// any run of stripped bytes into a single space, and clamp at `cap`
// chars (cap counts the output, not the input).  UTF-8 multi-byte
// sequences (0x80+) pass through unchanged.
//
// Used when rendering tool names and descriptions into the structured
// system prompt — see SECURITY_AUDIT §23.1.  A hostile or buggy field
// containing embedded `\n` would otherwise break the bulleted "Active
// tools" section, looking to the model like a new authoritative
// section.  Same class as §20.1 (bash mirror) and §20.3 (plan render)
// — control bytes from a less-trusted source landing on a structured
// output channel.
//
// The same threat applies to MCP's `initialize.instructions` and the
// HTTP-level `/v1/tools` payload, but those carry static / operator-
// controlled strings only and don't need this filter today.
std::string sanitize_for_prompt(const std::string & s, std::size_t cap) {
    std::string out;
    out.reserve(s.size() < cap ? s.size() : cap);
    bool pending_space = false;
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7f) {
            pending_space = true;
            continue;
        }
        if (pending_space) {
            if (out.size() < cap) out += ' ';
            pending_space = false;
        }
        if (out.size() >= cap) break;
        out += c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace

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

    // Block order (stable prefix first, volatile content last) is chosen
    // so prompt-eval KV cache survives a memory write:
    //
    //   STABLE   : date/time + knowledge cutoff + KNOWLEDGE LOOP rules
    //              + CITE SOURCES.  These change at most once per
    //              process start.
    //   VOLATILE : the MEMORY VOCABULARY snapshot.  Re-rendered on every
    //              `memory(action="save"|"append"|"delete")` because the
    //              keyword count map shifts.  Putting it AT THE TAIL of
    //              the system message means the cache hit covers the
    //              stable prefix; only the suffix needs re-eval after a
    //              memory write.
    if (!opt.memory_root.empty()) {
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
        // Last block in the preamble on purpose (memory-vocab comes
        // after this, but the citation rule has stronger positional
        // weight against the cite-sources block since it's adjacent
        // to the assistant turn).
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

    // Memory vocabulary — TAIL of the preamble on purpose (see the
    // KV-cache comment above).  The renderer is cached by directory
    // mtime; cost on a hot path is one stat() per request.
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

std::string tools_block(const ToolsetView & view) {
    std::ostringstream s;

    // Closed-set rule first — applies regardless of which tools are on.
    // Generic deny ("only the names listed below"), no enumeration of
    // hallucinated names (deliberately, per the Shape-C decision: keep
    // it generic to save tokens).
    s << "## Tools — closed set\n"
         "Your tools are EXACTLY the ones listed below (and emitted in "
         "your `<tools>` schema this turn). Do NOT invent tools. Do NOT "
         "call paraphrases of names you remember from other systems — "
         "use only the EXACT names listed below. Anything you remember "
         "from other AI systems or training that isn't in this list is "
         "NOT available.\n"
         "\n"
         "Each tool below shows a SHORT trigger; the full manual (rules, "
         "examples, edge cases) is available via `tool_lookup(name=\"<x>\")`. "
         "Call it whenever the short line isn't enough to call confidently. "
         "A no-match result from `tool_lookup` is authoritative — do not "
         "retry variations.\n"
         "\n"
         "If a request needs a capability with no matching tool, do the "
         "work in your visible reply. Asked to write a file and have no "
         "write tool? Put the content DIRECTLY in the chat reply — never "
         "paste it into a non-existent tool call.\n"
         "\n";

    // Body — list every active tool with its short trigger.  Source of
    // truth is `view.active_tools` when populated (ground truth from
    // the registry); otherwise fall back to the per-tool booleans with
    // hardcoded trigger lines so a partially-populated view still
    // renders something useful (mostly relevant to easyai-cli before
    // it has its server-fetched catalogue).
    // Write/edit policy is emitted BEFORE the active-tools list on
    // purpose: a model reading the catalogue must see the rule first
    // so the rule frames the trigger lines that follow. Putting the
    // policy AFTER the bullet list left the model picking the compute
    // tool for file writes (strong training prior on "python = write
    // files") before it ever read the "compute is read-only" rule.
    if (view.python_on) {
        s << "## Write/edit policy (AUTHORITATIVE)\n"
             "`evaluate` is for COMPUTE and ALGORITHM PROTOTYPING ONLY "
             "(it runs Python 3 code in a sandbox). FORBIDDEN: "
             "filesystem writes, subprocess launches, network I/O, "
             "ctypes. Every write-mode `open(...)` is rejected by the "
             "sandbox, even inside the sandbox root.\n"
             "\n"
             "All disk writes/edits go through your filesystem "
             "write/edit tool";
        if (view.bash_on) {
            s << " or, when shell features are needed (redirects, "
                 "`sed -i`, `mkdir`, `cat <<EOF`), through `bash`";
        }
        s << ". On the first `PermissionError` from `evaluate`, switch "
             "to the filesystem tool";
        if (view.bash_on) s << " or `bash`";
        s << " — do not retry the evaluate call. The exact callable "
             "name(s) are in your AVAILABLE TOOLS list below.\n\n";
    } else if (view.fs_on || view.bash_on) {
        // No evaluate registered: still useful to spell out which
        // tools can write, so the model doesn't reach for a
        // hallucinated `python` / `code_interpreter` / `evaluate` call.
        s << "## Write/edit policy\n"
             "Disk writes/edits go through ";
        if (view.fs_on && view.bash_on) {
            s << "your filesystem write/edit tool (preferred) or "
                 "`bash` (for shell features fs can't do).";
        } else if (view.fs_on) {
            s << "your filesystem write/edit tool — the only "
                 "write surface registered this session.";
        } else {
            s << "`bash` (the only write tool registered this session).";
        }
        s << " Do not call any other name for disk work — there is "
             "no `evaluate`, `python`, `python3`, `code_interpreter`, "
             "`write_file`, etc. wired up this turn. The exact "
             "callable name(s) are in your AVAILABLE TOOLS list "
             "below.\n\n";
    }

    if (!view.active_tools.empty()) {
        s << "Active tools this session:\n";
        std::size_t n = 0;
        // Tool names are validated upstream (regex-bounded for external
        // tools, hardcoded for builtins) but the cli's --url flow
        // populates `active_tools` from /v1/tools — a malicious server
        // could return a tool with embedded control bytes that would
        // break the bullet structure.  Sanitize both fields at the
        // render boundary; see SECURITY_AUDIT §23.1.
        constexpr std::size_t kNameCap = 64;
        constexpr std::size_t kDescCap = 200;
        for (const auto & t : view.active_tools) {
            ++n;
            const std::string name = sanitize_for_prompt(t.name, kNameCap);
            const std::string wd   = sanitize_for_prompt(t.wire_description(), kDescCap);
            if (name.empty()) continue;       // unrenderable entry
            s << "  " << n << ". " << name;
            if (!wd.empty()) s << " — " << wd;
            s << '\n';
        }
        s << '\n';
    } else if (view.any()) {
        s << "Active tools this session:\n";
        if (view.datetime_on)
            s << "  - datetime — return the current UTC and local "
                 "date/time. Call for 'now'/'today'/'latest' or before "
                 "date math.\n";
        if (view.web_on)
            s << "  - web — search the web and fetch URLs "
                 "(action=search|fetch). Reply MUST end with a "
                 "`Sources:` block listing URLs used.\n";
        if (view.memory_on)
            s << "  - memory — private memory store "
                 "(action=search|load|append|save|list|delete|keywords). "
                 "Search BEFORE answering from knowledge.\n";
        if (view.fs_on)
            s << "  - fs — filesystem: read/write/edit/list/glob/grep/"
                 "cwd/sandbox in sandbox. Batch with action=\"ops\" "
                 "(50 ops / 20 files per call).\n";
        if (view.bash_on)
            s << "  - bash — run a shell command (`/bin/sh -c`). "
                 "Allowed to write files (redirects, sed -i, mkdir). "
                 "Use for pipes/build/git/sed/awk.\n";
        if (view.python_on)
            s << "  - evaluate — evaluate Python 3 code for compute / "
                 "algorithm prototyping. FORBIDDEN: filesystem, "
                 "subprocess, network, ctypes. Stdlib compute only.\n";
        if (view.tool_lookup_on)
            s << "  - tool_lookup — list or inspect registered tools. "
                 "Call when in doubt about a name or its full manual.\n";
        s << '\n';
    } else {
        s << "NO TOOLS ARE REGISTERED THIS SESSION. Do not call any "
             "tool — answer from your own knowledge. If you cannot, "
             "say so directly.\n\n";
    }

    return s.str();
}

std::string build_builtin_system_prompt(const ToolsetView & view) {
    std::ostringstream s;

    s << "You are a clear, honest assistant. Lead with the answer; add "
         "detail only when the user needs it.\n"
         "\n"
         "## Think SHARP, not LONG\n"
         "Reasoning decides the next move — not for rehearsing the "
         "answer or exploring tangents.\n"
         "  - 3-5 short sentences before the first tool call or "
         "answer; up to ~10 short bullets for genuinely complex tasks. "
         "Never paragraphs.\n"
         "  - Telegraph style: one claim or decision per line; drop "
         "\"I think\", \"Let me consider\", \"It seems\".\n"
         "  - Don't enumerate options you immediately reject — pick "
         "the move and go; tool results correct wrong moves.\n"
         "  - Don't pre-compute the answer in reasoning then restate "
         "it visibly. Reasoning drives the agent loop; the visible "
         "reply is for the user.\n"
         "  - >5 sentences without a decision → STOP and act.\n"
         "\n"
         "Answer directly for greetings, chitchat, math, and anything "
         "you already know — no tool needed. When a request truly "
         "needs work, run a tight loop:\n"
         "  1. Plan ONE small concrete next step (not a roadmap).\n"
         "  2. Act — call the tool in the SAME turn. Announcing a "
         "call without making it (\"I'll search…\", \"Let me "
         "fetch…\") is forbidden.\n"
         "  3. Read the result, then finish or take ONE more step.\n"
         "Stop as soon as you have something useful; a short answer "
         "the user can refine beats a long pre-committed plan.\n"
         "\n";

    // Tools block — closed-set + per-tool triggers + write policy.
    s << tools_block(view);

    // Information pipeline — varies with memory presence.
    s << "## Information pipeline (AUTHORITATIVE)\n"
         "When the request needs facts you don't already know, follow "
         "this order — strictly:\n"
         "\n";
    if (view.memory_on) {
        s << "  1. MEMORY FIRST. Use your memory-search tool with "
             "keywords from the vocabulary appended below (exact "
             "callable name in your AVAILABLE TOOLS list). If memory "
             "returns enough to answer, SKIP the web and go straight "
             "to step 3.\n"
             "  2. WEB only if memory had nothing or was "
             "insufficient. ONE web search, then web_fetch the top "
             "1-3 URLs.\n"
             "  3. ANSWER. As soon as steps 1-2 give you enough, "
             "answer the user. Don't re-search memory, don't "
             "re-search the web, don't save more memories first.\n";
    } else if (view.web_on) {
        s << "  1. WEB if you don't already know. ONE web search, "
             "then web_fetch the top 1-3 URLs.\n"
             "  2. ANSWER. As soon as the fetched text gives you "
             "enough, answer. Don't re-search the same query.\n";
    } else {
        s << "  1. ANSWER from your own knowledge. No retrieval tool "
             "is wired up this session, so don't invent calls — say "
             "so directly if you don't know.\n";
    }
    s << "\n"
         "STOP SIGNAL. After each tool result, ask: do I have enough "
         "now? Yes → answer immediately. No → ONE more focused tool "
         "call, then re-check. Three or more tool calls in a row "
         "without re-checking is a bug — you're exploring instead of "
         "answering.\n"
         "\n";

    if (view.memory_on || view.web_on) {
        s << "BUGS TO AVOID:\n";
        if (view.memory_on) {
            s << "  - Skipping memory and going straight to web when "
                 "memory is enabled.\n"
                 "  - After a memory load returns a stable fact "
                 "(definition, syntax, architecture), re-verifying "
                 "with the web — only do this when the user asked "
                 "for \"latest\" / \"current\" / dated info.\n"
                 "  - After saving a memory, re-searching the web on "
                 "the same topic in the same turn — the save means "
                 "you already learned what you needed.\n";
        }
        s << "  - Looping verify → save → re-verify.\n"
             "  - Running the same web query twice in a row.\n"
             "\n";
    }

    if (view.memory_on) {
        s << "Saving new memories (when the info is durable — see the "
             "memory tool's GUIDELINES) happens AFTER your reply is "
             "written, as a final tool call. It's not another "
             "verification step.\n"
             "\n";
    }

    s << "## Stop when you have enough — the user can refine "
         "(AUTHORITATIVE)\n"
         "Go only as far as the searches you need to give a useful "
         "answer to THIS question. Not the perfect answer, not the "
         "exhaustive one — a useful one.\n"
         "\n"
         "The user is on the other side of a chat box. They CAN send "
         "you another message — refining the query, narrowing the "
         "scope, asking for more depth on one point. They CANNOT "
         "interrupt your tool loop or skim while you fetch URL #8. "
         "Their cost of asking a follow-up is one sentence; your "
         "cost of an extra round of searches is their wait time.\n"
         "\n"
         "Practical shape: 1-3 targeted searches, then answer with "
         "what you have. If something's missing, name it in the "
         "reply (\"I couldn't find X — want me to check Y "
         "instead?\"). Let the user steer the next step.\n"
         "\n"
         "## Stay strictly in scope (AUTHORITATIVE)\n"
         "Do EXACTLY what the user asked — no more, no less. No "
         "extra features, no defensive scaffolding for cases they "
         "didn't mention, no \"while I'm at it\" cleanups, no "
         "proactive refactors. The request is the ceiling, not a "
         "starting point. If genuinely unsure what's in scope, ASK "
         "before acting — don't expand the task to be safe.\n"
         "\n"
      << cite_sources_block(view.memory_on);

    return s.str();
}

// Public helper — see preamble.hpp for the rationale.  Differs from
// the anonymous `sanitize_for_prompt` above in that this one PRESERVES
// `\n` (0x0a) and `\t` (0x09), because addenda are multi-paragraph
// blocks where line structure carries meaning.  Everything else in
// C0 (incl. ESC 0x1b, bell 0x07) and DEL (0x7f) is stripped.
std::string sanitize_addendum(const std::string & s, std::size_t cap) {
    std::string out;
    out.reserve(s.size() < cap ? s.size() : cap);
    bool pending_space = false;
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        // Allow legitimate whitespace control bytes.
        if (uc == '\n' || uc == '\t') {
            if (pending_space) {
                if (out.size() < cap) out += ' ';
                pending_space = false;
            }
            if (out.size() >= cap) break;
            out += c;
            continue;
        }
        // Strip the rest of C0 plus DEL.
        if (uc < 0x20 || uc == 0x7f) {
            pending_space = true;
            continue;
        }
        if (pending_space) {
            if (out.size() < cap) out += ' ';
            pending_space = false;
        }
        if (out.size() >= cap) break;
        out += c;
    }
    // Trim trailing spaces (newlines are kept — they delimit the
    // addendum block from the next section).
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace easyai::preamble
