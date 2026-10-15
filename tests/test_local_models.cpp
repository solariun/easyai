// /models local listing: "running" (what the engine holds) vs "default"
// (what the --model symlink resolves to). spec.md "Running vs default".
// Scenario from the AI box: ai.gguf -> gemma.gguf at boot, the engine resolved
// the link once; later the operator re-links ai.gguf -> qwen.gguf (Slink) or
// hot-swaps. Both truths must be reported, on the right files.
#include "easyai/models_dashboard.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } else { std::printf("ok   %s\n", #cond); } } while (0)

static void touch(const fs::path & p) { std::ofstream(p) << "GGUF"; }

static const easyai::ModelsEngine::LocalModel * find(const std::vector<easyai::ModelsEngine::LocalModel> & v, const std::string & n) {
    for (const auto & m : v) if (m.name == n) return &m;
    return nullptr;
}

int main() {
    fs::path dir = fs::temp_directory_path() / "easyai-test-local-models";
    fs::remove_all(dir); fs::create_directories(dir);
    touch(dir / "gemma.gguf"); touch(dir / "qwen.gguf");
    fs::create_symlink("gemma.gguf", dir / "ai.gguf");

    // The engine resolves the symlink at load time, so its model_path is the
    // real file it loaded; the server passes --model (the link) as default.
    std::string engine_path = fs::canonical(dir / "gemma.gguf").string();
    std::string link_path   = (dir / "ai.gguf").string();
    easyai::ModelsEngine eng(dir.string(), nullptr,
                             [&]() { return engine_path; }, 1, dir.string(),
                             [&]() { return link_path; });

    // --- boot state: link and memory agree ---------------------------------
    auto v = eng.list_local();
    CHECK(v.size() == 3);
    CHECK(find(v, "gemma.gguf")->is_current && find(v, "gemma.gguf")->is_default);
    CHECK(find(v, "ai.gguf")->is_current && find(v, "ai.gguf")->is_default);
    CHECK(find(v, "ai.gguf")->link_target == "gemma.gguf");
    CHECK(!find(v, "qwen.gguf")->is_current && !find(v, "qwen.gguf")->is_default);
    CHECK(find(v, "gemma.gguf")->link_target.empty());

    // --- Slink moved the link to qwen; the engine still holds gemma --------
    fs::remove(dir / "ai.gguf"); fs::create_symlink("qwen.gguf", dir / "ai.gguf");
    v = eng.list_local();
    CHECK(find(v, "gemma.gguf")->is_current && !find(v, "gemma.gguf")->is_default);
    CHECK(!find(v, "qwen.gguf")->is_current && find(v, "qwen.gguf")->is_default);
    CHECK(!find(v, "ai.gguf")->is_current && find(v, "ai.gguf")->is_default);
    std::string j = eng.local_models_json();
    CHECK(j.find("\"default_target\":\"" + fs::canonical(dir / "qwen.gguf").string() + "\"") != std::string::npos);
    CHECK(j.find("\"running\":\"" + engine_path + "\"") != std::string::npos);

    // --- hot-swap to qwen: memory follows the link again -------------------
    engine_path = fs::canonical(dir / "qwen.gguf").string();
    v = eng.list_local();
    CHECK(find(v, "qwen.gguf")->is_current && find(v, "qwen.gguf")->is_default);
    CHECK(!find(v, "gemma.gguf")->is_current);

    // --- no --model configured: nothing is "default" -----------------------
    easyai::ModelsEngine bare(dir.string(), nullptr, [&]() { return engine_path; }, 1, dir.string());
    v = bare.list_local();
    CHECK(find(v, "qwen.gguf")->is_current && !find(v, "qwen.gguf")->is_default);

    fs::remove_all(dir);
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
