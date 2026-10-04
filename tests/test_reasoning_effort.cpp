// Reasoning-effort fallback ladder (spec.md "Reasoning-effort fallback ladder").
// Renders real Jinja templates through llama.cpp without loading a model.
#include "easyai/engine.hpp"
#include "chat.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } else { std::printf("ok   %s\n", #cond); } } while (0)

// Mirrors the production template that only knows xhigh / medium / low.
static const char * kStrict =
    "{%- set reasoning_effort = reasoning_effort | default('xhigh') %}"
    "{%- if reasoning_effort not in ['xhigh', 'medium', 'low'] %}"
    "{{- raise_exception('Unexpected reasoning effort ' ~ reasoning_effort ~ '. Supported types are xhigh (default), medium, and low.') }}"
    "{%- endif %}"
    "effort={{ reasoning_effort }}\n"
    "{%- for m in messages %}<|{{ m.role }}|>{{ m.content }}{% endfor %}"
    "{%- if add_generation_prompt %}<|assistant|>{% endif %}";

// Rejects every effort, including the absent one.
static const char * kRejectAll =
    "{{- raise_exception('reasoning effort unsupported here') }}";

// Throws for a reason unrelated to effort.
static const char * kUnrelated =
    "{{- raise_exception('history is malformed') }}";

static common_chat_templates_inputs inputs() {
    common_chat_templates_inputs in;
    common_chat_msg u; u.role = "user"; u.content = "hi";
    in.messages.push_back(u);
    in.add_generation_prompt = true;
    in.use_jinja = true;
    return in;
}

static std::string render(const char * tmpl, const std::string & level,
                          std::map<std::string, std::string> & remap) {
    auto t = common_chat_templates_init(nullptr, tmpl);
    auto in = inputs();
    return easyai::apply_reasoning_effort(t.get(), in, level, remap).prompt;
}

int main() {
    {   // max → xhigh (nearest lower neighbour), cached
        std::map<std::string, std::string> remap;
        CHECK(render(kStrict, "max", remap).find("effort=xhigh") == 0);
        CHECK(remap.at("max") == "xhigh");
        CHECK(render(kStrict, "max", remap).find("effort=xhigh") == 0);
        CHECK(remap.size() == 1);
    }
    {   // high → medium (lower before higher on ties)
        std::map<std::string, std::string> remap;
        CHECK(render(kStrict, "high", remap).find("effort=medium") == 0);
        CHECK(remap.at("high") == "medium");
    }
    {   // minimal → low (nothing below, climb)
        std::map<std::string, std::string> remap;
        CHECK(render(kStrict, "minimal", remap).find("effort=low") == 0);
    }
    {   // accepted level: no remap entry, no retry
        std::map<std::string, std::string> remap;
        CHECK(render(kStrict, "low", remap).find("effort=low") == 0);
        CHECK(remap.empty());
    }
    {   // unknown word → ladder from the top: max rejected, xhigh accepted
        std::map<std::string, std::string> remap;
        CHECK(render(kStrict, "turbo", remap).find("effort=xhigh") == 0);
        CHECK(remap.at("turbo") == "xhigh");
    }
    {   // every candidate rejected → original message rethrown
        std::map<std::string, std::string> remap;
        bool threw = false;
        try { render(kRejectAll, "max", remap); }
        catch (const std::exception & e) {
            threw = std::string(e.what()).find("reasoning effort unsupported here") != std::string::npos;
        }
        CHECK(threw);
        CHECK(remap.empty());
    }
    {   // unrelated exception propagates untouched, no ladder walk
        std::map<std::string, std::string> remap;
        bool threw = false;
        try { render(kUnrelated, "max", remap); }
        catch (const std::exception & e) {
            threw = std::string(e.what()).find("history is malformed") != std::string::npos;
        }
        CHECK(threw);
        CHECK(remap.empty());
    }
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
