// easyai/models_dashboard.hpp — the MODELS dashboard engine.
//
// Powers the "/models" web UI in easyai-server. Everything here is NATIVE
// C++ — there is no dependency on the external `llmfit` binary. It does
// three jobs:
//
//   1. RECOMMEND — detect this machine's hardware (RAM / CPU / GPU+VRAM via
//      ggml), score a bundled catalogue of LLMs (data/hf_models.json) for
//      fit / speed / quality / context against that hardware (or a simulated
//      one), and answer the model/system/plan queries the UI makes. This is
//      a native re-implementation of llmfit's scoring engine.
//
//   2. LOCAL MODELS — list the .gguf files in the download directory, read
//      each one's parameters straight from the GGUF header (no weights
//      loaded), compute whether it fits this hardware, and surface the
//      matching `[MODEL_*]` INI profile (if any) with its values — so the
//      operator can inspect a model before running it.
//
//   3. DOWNLOAD MANAGER — fetch GGUF weights from HuggingFace (libcurl) into
//      the download directory with progress / cancel, and list / delete what
//      is already there.
//
// The "Run" hot-swap (re-point the ai.gguf symlink + reload the engine) lives
// in the server because it touches easyai::Engine; this class only resolves +
// validates the target path.
//
// The header stays free of httplib / curl / json / llama types so
// examples/server.cpp can include it cheaply; all of that lives in
// src/models_dashboard.cpp (compiled into libeasyai).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace easyai {
namespace config { struct Ini; }   // borrowed by the engine for [MODEL_*] lookups

class ModelsEngine {
public:
    // A .gguf file in the download directory.
    struct LocalModel {
        std::string   name;
        std::uint64_t size_bytes = 0;
        std::int64_t  mtime      = 0;      // unix seconds
        bool          is_current = false;  // == the model the engine is serving
    };

    // A downloadable .gguf inside a HuggingFace repo.
    struct RepoFile {
        std::string   path;
        std::uint64_t size_bytes = 0;
    };

    // Snapshot of the single active/last download.
    struct DownloadStatus {
        int           id               = 0;
        std::string   repo;
        std::string   filename;
        std::string   state            = "idle";   // idle|downloading|done|error
        std::uint64_t downloaded_bytes = 0;
        std::uint64_t total_bytes      = 0;
        double        percent          = 0.0;
        std::string   error;
    };

    // download_dir  — where GGUF weights are downloaded / listed / deleted.
    // catalog_path  — data/hf_models.json (resolved by the server); empty or
    //                 missing ⇒ recommendations are unavailable but local
    //                 models + downloads still work.
    // ini           — borrowed pointer to the server's parsed INI (for
    //                 [MODEL_*] profile lookups); may be null.
    // current_model — getter returning the absolute path of the model the
    //                 engine is currently serving (to flag the active model).
    ModelsEngine(std::string download_dir, std::string catalog_path,
                 const config::Ini * ini,
                 std::function<std::string()> current_model);
    ~ModelsEngine();

    ModelsEngine(const ModelsEngine &)             = delete;
    ModelsEngine & operator=(const ModelsEngine &) = delete;

    bool        catalog_loaded() const;
    std::size_t catalog_size()   const;
    std::string status_message() const;

    // ---- JSON endpoints (raw query string / body in, JSON out) ----
    // `query` is the request's URL query (e.g. "search=qwen&min_fit=good&ram_gb=64").
    std::string system_json(const std::string & query);
    std::string models_json(const std::string & query);
    std::string plan_json(const std::string & body);          // POST body
    std::string local_models_json();                          // list
    std::string local_model_json(const std::string & filename,
                                 const std::string & query);  // detail panel

    // ---- download manager (native libcurl → HuggingFace) ----
    bool hf_repo_files(const std::string & repo,
                       std::vector<RepoFile> & out, std::string & err);
    int  start_download(const std::string & repo, const std::string & filename,
                        std::string & err);
    void           cancel_download();
    DownloadStatus download_status();

    // ---- local model directory ----
    std::vector<LocalModel> list_local();
    bool delete_local(const std::string & name, std::string & err);
    // Validate a bare filename and resolve it to its absolute path inside the
    // download dir (used by the server's Run/hot-swap). False + err if unsafe.
    bool resolve_local(const std::string & name, std::string & abs_path,
                       std::string & err);

    const std::string & download_dir() const { return download_dir_; }

private:
    void download_worker(int id, std::string repo, std::vector<RepoFile> files);

    std::string                   download_dir_;
    std::string                   catalog_path_;
    const config::Ini *           ini_ = nullptr;
    std::function<std::string()>  current_model_;
    std::string                   status_;

    // catalogue (opaque pImpl-ish vector lives in the .cpp via a forward type)
    struct Catalog;
    std::unique_ptr<Catalog>      catalog_;

    // download manager
    std::mutex         dl_start_mu_;     // serializes start_download()
    std::mutex         dl_mu_;           // guards st_
    DownloadStatus     st_;
    std::atomic<bool>  cancel_{false};
    std::thread        worker_;
    int                next_id_ = 0;
};

}  // namespace easyai
