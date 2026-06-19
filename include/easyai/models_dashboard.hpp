// easyai/models_dashboard.hpp — the MODELS dashboard engine.
//
// Powers the "/models" web UI in easyai-server. Everything here is NATIVE
// C++ — there is no dependency on the external `llmfit` binary. It does
// three jobs:
//
//   1. RECOMMEND — detect this machine's hardware (RAM / CPU / GPU+VRAM via
//      ggml), search HuggingFace live for GGUF models, enrich each with its
//      repo's best GGUF (size → estimated params/quant), and score it for
//      fit / speed / quality against that hardware (or a simulated one). The
//      scoring is a native re-implementation of llmfit's; the params/quant are
//      estimated from the file, so the figures are approximate.
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
// services/server.cpp can include it cheaply; all of that lives in
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
    // ini           — borrowed pointer to the server's parsed INI (for
    //                 [MODEL_*] profile lookups); may be null.
    // current_model — getter returning the absolute path of the model the
    //                 engine is currently serving (to flag the active model).
    //
    // The Recommend tab sources models LIVE from the HuggingFace listing API
    // (/api/models?filter=gguf) rather than a bundled catalog: each result is
    // enriched with its repo's best GGUF (size → estimated params/quant) and
    // scored against detected hardware. Results are cached per repo for the
    // session.
    // catalog_size — how many of the most-recently-updated GGUF repos to pull
    // into the searchable snapshot on each refresh; clamped to [1, 1000]. The
    // fetch follows the HuggingFace listing cursor until it has this many or the
    // listing is exhausted.
    // data_dir — directory where the catalog snapshot is persisted
    // (easyai_hf_catalog.json), so a restart serves the last list instantly and
    // the 1-hour refresh clock survives. Empty → falls back to download_dir.
    ModelsEngine(std::string download_dir, const config::Ini * ini,
                 std::function<std::string()> current_model, int catalog_size = 1000,
                 std::string data_dir = "");
    ~ModelsEngine();

    ModelsEngine(const ModelsEngine &)             = delete;
    ModelsEngine & operator=(const ModelsEngine &) = delete;

    std::string status_message() const;

    // Rebuild the static model list from HuggingFace in the background.
    // `force` ignores the freshness check; otherwise it only refreshes when the
    // snapshot is empty or older than 1 hour. Idempotent (a refresh already in
    // flight is a no-op). Call on startup + from the Refresh button; models_json
    // also calls it lazily (force=false) so an access after >1h triggers one.
    void start_refresh(bool force);

    // ---- JSON endpoints (raw query string / body in, JSON out) ----
    // `query` is the request's URL query (e.g. "search=qwen&min_fit=good&ram_gb=64").
    std::string system_json(const std::string & query);
    std::string models_json(const std::string & query);       // static HF snapshot
    std::string plan_json(const std::string & body);          // POST body
    std::string local_models_json();                          // list
    std::string local_model_json(const std::string & filename,
                                 const std::string & query);  // detail panel
    // Precise fit for a HuggingFace model: reads the remote GGUF header (an HTTP
    // range request — no full download) to recover real params / context /
    // layers, then scores accurately. Falls back to the name/size estimate.
    std::string hf_detail_json(const std::string & repo, const std::string & query);

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
    const std::string & data_dir() const { return data_dir_; }

private:
    void download_worker(int id, std::string repo, std::vector<RepoFile> files);

    std::string                   download_dir_;
    const config::Ini *           ini_ = nullptr;
    std::function<std::string()>  current_model_;
    int                           catalog_size_ = 1000;  // most-recent GGUF repos in the snapshot
    std::string                   data_dir_;             // where the catalog cache is persisted
    std::string                   status_;

    void load_catalog_cache();    // populate the snapshot from disk at startup

    // The static model snapshot (opaque pImpl; real type in the .cpp): the
    // enriched HF entries + last-refresh time. Guarded by hf_cache_mu_; rebuilt
    // by a background refresh thread.
    struct HfCache;
    std::unique_ptr<HfCache>      hf_cache_;
    std::mutex                    hf_cache_mu_;
    std::atomic<bool>             refreshing_{false};
    std::thread                   refresh_thread_;

    // download manager
    std::mutex         dl_start_mu_;     // serializes start_download()
    std::mutex         dl_mu_;           // guards st_
    DownloadStatus     st_;
    std::atomic<bool>  cancel_{false};
    std::thread        worker_;
    int                next_id_ = 0;
};

}  // namespace easyai
