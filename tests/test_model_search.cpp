// Model finder matching (spec.md "Model finder mode").
#include "easyai/models_dashboard.hpp"
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } else { std::printf("ok   %s\n", #cond); } } while (0)

int main() {
    using easyai::search_terms_match;
    const std::string hay = "unsloth/qwen3-30b-a3b-gguf qwen3-30b-a3b unsloth 30b";
    CHECK(search_terms_match(hay, ""));
    CHECK(search_terms_match(hay, "qwen3"));
    CHECK(search_terms_match(hay, "unsloth"));
    CHECK(search_terms_match(hay, "qwen3 unsloth"));          // any order
    CHECK(search_terms_match(hay, "  unsloth   qwen3  "));    // extra spaces
    CHECK(search_terms_match(hay, "unsloth/qwen3"));          // owner/name prefix
    CHECK(!search_terms_match(hay, "qwen3 google"));          // one term missing
    CHECK(!search_terms_match(hay, "llama"));
    CHECK(!search_terms_match("", "x"));
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
