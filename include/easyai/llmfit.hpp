// easyai/llmfit.hpp — LLMFit integration bridge for easyai-server.
//
// easyai-server hosts a sober web UI ("/llmfit") that gives the
// hardware-aware model recommendations of LLMFit
// (https://github.com/AlexsJones/llmfit) plus a native model download
// manager. This class is the engine behind that UI and has three jobs:
//
//   1. CHILD PROCESS — spawn the external `llmfit` binary in its REST
//      mode (`llmfit serve --host 127.0.0.1 --port <n>`), pointed at the
//      download directory via the LLMFIT_MODELS_DIR env var, and reap it
//      cleanly on shutdown. llmfit is the authoritative source of fit /
//      hardware / model-catalog data — we never fake it.
//
//   2. PROXY — forward the browser's `/llmfit/api/v1/*` calls to that
//      child's REST API (system, models, plan, runtimes, installed) and
//      hand the JSON straight back. If the child is missing/unspawnable
//      the proxy degrades to HTTP 502 with a clear message — the server
//      itself never crashes.
//
//   3. DOWNLOAD MANAGER — fetch GGUF weights directly from HuggingFace
//      (libcurl) into the configured download directory, with progress
//      and cancellation, and list/delete the .gguf files already there.
//      This is native (not delegated to llmfit) so the operator fully
//      controls where weights land and can manage them from the UI.
//
// The header is intentionally free of httplib/curl/json types so it can
// be included by examples/server.cpp without dragging those in; all of
// that lives in src/llmfit.cpp (compiled into libeasyai, where libcurl
// is linked).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace easyai {

class LlmfitBridge {
public:
    // Lifecycle state of the child `llmfit serve` process.
    enum class State { Stopped, Starting, Running, Unavailable };

    // Result of a proxied REST call to the llmfit child.
    struct ProxyResult {
        int         status       = 502;            // HTTP status to return to the browser
        std::string body;                          // response body (JSON, usually)
        std::string content_type = "application/json";
    };

    // A .gguf file sitting in the download directory.
    struct LocalModel {
        std::string name;                          // bare filename
        std::uint64_t size_bytes = 0;
        std::int64_t  mtime      = 0;              // unix seconds, last-modified
    };

    // A downloadable .gguf file inside a HuggingFace repo.
    struct RepoFile {
        std::string   path;                        // path within the repo
        std::uint64_t size_bytes = 0;
    };

    // Snapshot of the (single) active/last download. Copy-returned under
    // the lock so callers never see a torn read.
    struct DownloadStatus {
        int           id               = 0;
        std::string   repo;
        std::string   filename;                    // family label / first file
        std::string   state            = "idle";   // idle|downloading|done|error
        std::uint64_t downloaded_bytes = 0;
        std::uint64_t total_bytes      = 0;
        double        percent          = 0.0;
        std::string   error;
    };

    // bin           — `llmfit` binary (bare name resolved via PATH, or an
    //                 absolute/relative path).
    // download_dir  — where GGUF weights are downloaded and listed from.
    // port          — loopback port the child's REST API binds to.
    LlmfitBridge(std::string bin, std::string download_dir, int port);
    ~LlmfitBridge();

    LlmfitBridge(const LlmfitBridge &)             = delete;
    LlmfitBridge & operator=(const LlmfitBridge &) = delete;

    // ---- child process -------------------------------------------------
    // Spawn `llmfit serve`. Returns false (and sets State::Unavailable)
    // if the binary can't be found or fork/exec fails. Non-fatal: the
    // download manager works regardless of the child.
    bool  start();
    // Poll the child's /health until it answers or we give up. Updates
    // state to Running on success, Unavailable otherwise.
    bool  wait_ready(int attempts = 40, int per_try_ms = 250);
    // SIGTERM -> grace -> SIGKILL -> reap. Idempotent.
    void  stop();
    State state() const { return state_.load(); }
    bool  available() const { return state_.load() == State::Running; }
    // Human-readable note about the bridge state (e.g. why it's down).
    std::string status_message();

    // ---- proxy ---------------------------------------------------------
    // `path_and_query` is the upstream path, e.g. "/api/v1/models?limit=20".
    ProxyResult proxy_get(const std::string & path_and_query);
    ProxyResult proxy_post(const std::string & path, const std::string & body,
                           const std::string & content_type);

    // ---- download manager (native, libcurl -> HuggingFace) -------------
    // List the .gguf files inside a HF repo (for the quant picker).
    bool hf_repo_files(const std::string & repo,
                       std::vector<RepoFile> & out, std::string & err);
    // Begin downloading. If `filename` is empty the best-quality quant is
    // auto-selected; if it names one shard the whole shard family is
    // fetched. Returns the new download id, or -1 (err set) when busy,
    // when curl is unavailable, or on a bad request.
    int  start_download(const std::string & repo, const std::string & filename,
                        std::string & err);
    void           cancel_download();
    DownloadStatus download_status();

    // ---- local model directory ----------------------------------------
    std::vector<LocalModel> list_local();
    bool delete_local(const std::string & name, std::string & err);

    const std::string & download_dir() const { return download_dir_; }
    int                 port() const { return port_; }

private:
    // Background download entry point (runs on worker_).
    void download_worker(int id, std::string repo,
                         std::vector<RepoFile> files);

    // config
    std::string bin_;
    std::string download_dir_;
    int         port_ = 8788;

    // child process
    std::atomic<State> state_{State::Stopped};
    long               pid_ = -1;               // pid_t; kept as long to avoid <sys/types.h> here
    std::mutex         proc_mu_;
    std::string        status_msg_;

    // download manager
    std::mutex         dl_start_mu_;             // serializes start_download()
    std::mutex         dl_mu_;                   // guards st_ (read by status())
    DownloadStatus     st_;
    std::atomic<bool>  cancel_{false};
    std::thread        worker_;
    int                next_id_ = 0;
};

}  // namespace easyai
