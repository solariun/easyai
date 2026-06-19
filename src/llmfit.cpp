// src/llmfit.cpp — LLMFit bridge implementation.
//
// See include/easyai/llmfit.hpp for the contract. This file is compiled
// into libeasyai so it inherits the libcurl link + EASYAI_HAVE_CURL guard
// (curl is PRIVATE to libeasyai; examples/server.cpp can't use it directly)
// and the vendored httplib include path.
//
// Three concerns, mirroring the header:
//   * child process  — fork/execve a `llmfit serve`, modelled on the
//     canonical runner in src/external_tools.cpp (signal-safe child,
//     setpgid + PR_SET_PDEATHSIG, fd hygiene, SIGTERM->SIGKILL reap).
//   * proxy          — a throwaway httplib::Client per call to the
//     loopback REST port, modelled on src/client.cpp:get_http().
//   * downloads      — libcurl streaming to a .part file then atomic
//     rename, modelled on src/mcp_client.cpp's curl usage.

#include "easyai/llmfit.hpp"
#include "easyai/log.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <regex>
#include <utility>

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

#if defined(EASYAI_HAVE_CURL)
#include <curl/curl.h>
#endif

extern char ** environ;

namespace easyai {

namespace fs = std::filesystem;

namespace {

// Upper-bound on fds to close in the child when RLIMIT_NOFILE is
// unbounded/huge — same defence as external_tools.cpp's kMaxFdScan.
constexpr int kMaxFdScan = 4096;

// Cap on buffered HF-API JSON (the model tree). Real listings are a few
// KiB; this just stops an adversarial response exhausting memory.
constexpr std::size_t kMaxApiBytes = 8u * 1024u * 1024u;

std::string to_upper(std::string s) {
    for (char & c : s) c = (char) std::toupper((unsigned char) c);
    return s;
}

bool ends_with_ci(const std::string & s, const std::string & suf) {
    if (s.size() < suf.size()) return false;
    return std::equal(suf.rbegin(), suf.rend(), s.rbegin(),
                      [](char a, char b) {
                          return std::tolower((unsigned char) a) ==
                                 std::tolower((unsigned char) b);
                      });
}

// Resolve a program name to an absolute path. If `bin` contains a slash
// it is used directly (absolutised); otherwise PATH is searched. Returns
// "" when nothing executable is found. Done in the PARENT, before fork,
// so the child can execve() an absolute path (no PATH-search needed in
// the signal-safe window — and macOS lacks execvpe()).
std::string resolve_executable(const std::string & bin) {
    if (bin.empty()) return "";
    if (bin.find('/') != std::string::npos) {
        std::error_code ec;
        fs::path p = fs::absolute(bin, ec);
        if (ec) p = bin;
        if (::access(p.c_str(), X_OK) == 0) return p.string();
        return "";
    }
    const char * path = ::getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    std::string ps(path), dir;
    std::size_t start = 0;
    while (start <= ps.size()) {
        std::size_t colon = ps.find(':', start);
        dir = ps.substr(start, colon == std::string::npos ? std::string::npos
                                                          : colon - start);
        if (!dir.empty()) {
            std::string cand = dir + "/" + bin;
            if (::access(cand.c_str(), X_OK) == 0) {
                std::error_code ec;
                fs::path ab = fs::absolute(cand, ec);
                return ec ? cand : ab.string();
            }
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return "";
}

// The download/quant heuristics replicate llmfit's llamacpp provider
// (llmfit-core/src/providers.rs): prefer the highest-quality quant. The
// list is ordered best->worst AND respects the rule "a longer token that
// has a shorter token as a prefix comes first", so substring matching in
// quant_rank() identifies the most specific quant correctly.
const char * const kQuantOrder[] = {
    "Q8_0",
    "Q6_K_L", "Q6_K",
    "Q5_K_M", "Q5_K_S", "Q5_1", "Q5_0",
    "Q4_K_M", "Q4_K_S", "Q4_1", "Q4_0",
    "Q3_K_L", "Q3_K_M", "Q3_K_S", "Q3_K",
    "Q2_K_L", "Q2_K",
    "IQ4_XS", "IQ4_NL",
    "IQ3_M", "IQ3_S", "IQ3_XXS", "IQ3_XS",
    "IQ2_M", "IQ2_S", "IQ2_XXS", "IQ2_XS",
    "IQ1_M", "IQ1_S",
    "BF16", "F16", "F32",
};

// Lower is better. Unknown quants sort last.
int quant_rank(const std::string & filename) {
    std::string uc = to_upper(filename);
    int n = (int) (sizeof(kQuantOrder) / sizeof(kQuantOrder[0]));
    for (int i = 0; i < n; ++i) {
        if (uc.find(kQuantOrder[i]) != std::string::npos) return i;
    }
    return n + 1;
}

// "foo-Q4_K_M-00002-of-00005.gguf" -> "foo-Q4_K_M"  (shard family base).
// "foo-Q4_K_M.gguf"                -> "foo-Q4_K_M".
std::string family_base(const std::string & path) {
    static const std::regex shard(R"(^(.*)-\d{5}-of-\d{5}\.gguf$)",
                                  std::regex::icase);
    std::smatch m;
    if (std::regex_match(path, m, shard)) return m[1].str();
    if (ends_with_ci(path, ".gguf")) return path.substr(0, path.size() - 5);
    return path;
}

std::string base_name(const std::string & path) {
    std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// True for a "-NNNNN-of-MMMMM.gguf" shard member. A standalone .gguf with
// the same stem (e.g. stories15M.gguf vs stories15M-00001-of-00003.gguf)
// is NOT a shard and must not be lumped into the shard family.
bool is_shard(const std::string & path) {
    static const std::regex shard(R"(-\d{5}-of-\d{5}\.gguf$)", std::regex::icase);
    return std::regex_search(path, shard);
}

// Validate a bare GGUF filename and resolve it to a path that is provably
// inside `dir`. Mirrors Sandbox::inside_sandbox (builtin_tools.cpp): a
// component-wise prefix check on weakly_canonical paths, which avoids the
// "/srv/user" vs "/srv/userEVIL" string-prefix bug.
bool safe_dest(const std::string & dir, const std::string & filename,
               fs::path & out, std::string & err) {
    if (filename.empty()
        || filename.find('/')  != std::string::npos
        || filename.find('\\') != std::string::npos
        || filename.find("..") != std::string::npos) {
        err = "invalid filename"; return false;
    }
    if (!ends_with_ci(filename, ".gguf")) { err = "not a .gguf file"; return false; }

    std::error_code ec;
    fs::path root = fs::weakly_canonical(fs::absolute(dir, ec), ec);
    fs::path cand = fs::weakly_canonical(root / filename, ec);
    if (ec) { err = "path resolution failed"; return false; }

    auto it_r = root.begin();
    auto it_c = cand.begin();
    for (; it_r != root.end(); ++it_r, ++it_c) {
        if (it_c == cand.end() || *it_c != *it_r) {
            err = "path escapes download directory"; return false;
        }
    }
    out = cand;
    return true;
}

#if defined(EASYAI_HAVE_CURL)

// One-time curl_global_init. libcurl's lazy init is not thread-safe
// before 7.84 and this server now drives curl from a background download
// thread alongside the existing mcp/web curl callers — initialise once,
// up front. (No global_cleanup: process-lifetime, like most servers.)
void ensure_curl_global() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// Append-to-string write callback, capped — identical in spirit to
// mcp_client.cpp:curl_write_cb.
size_t str_write_cb(void * buf, size_t sz, size_t n, void * ud) {
    auto * out = static_cast<std::string *>(ud);
    const std::size_t incoming = sz * n;
    if (out->size() + incoming > kMaxApiBytes) return 0;  // abort
    out->append(static_cast<char *>(buf), incoming);
    return incoming;
}

void apply_common_curl(CURL * c) {
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS,      8L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL,       1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT,      "easyai-llmfit/1.0");
#if defined(LIBCURL_VERSION_NUM) && LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR,       "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS,
                     (long) (CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS,
                     (long) (CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
}

bool curl_get_string(const std::string & url, std::string & out,
                     std::string & err) {
    ensure_curl_global();
    CURL * c = curl_easy_init();
    if (!c) { err = "curl_easy_init failed"; return false; }
    out.clear();
    curl_easy_setopt(c, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, str_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       30L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR,   1L);
    apply_common_curl(c);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) { err = std::string("curl: ") + curl_easy_strerror(rc);
                          return false; }
    if (code >= 400)    { err = "HTTP " + std::to_string(code); return false; }
    return true;
}

// Context handed to the streaming download callbacks.
struct DlCb {
    std::FILE *                     fp     = nullptr;
    std::mutex *                    mu     = nullptr;
    LlmfitBridge::DownloadStatus *  st     = nullptr;
    std::atomic<bool> *             cancel = nullptr;
    std::uint64_t                   base   = 0;   // bytes from finished files
    std::uint64_t                   total  = 0;   // grand total across family
};

size_t file_write_cb(char * buf, size_t sz, size_t n, void * ud) {
    auto * c = static_cast<DlCb *>(ud);
    return std::fwrite(buf, 1, sz * n, c->fp);   // short write -> curl aborts
}

int xfer_cb(void * ud, curl_off_t dltotal, curl_off_t dlnow,
            curl_off_t, curl_off_t) {
    auto * c = static_cast<DlCb *>(ud);
    if (c->cancel->load(std::memory_order_relaxed)) return 1;  // abort
    std::lock_guard<std::mutex> lk(*c->mu);
    c->st->downloaded_bytes = c->base + (std::uint64_t) dlnow;
    if (c->total > 0) {
        c->st->percent = (double) c->st->downloaded_bytes * 100.0 /
                         (double) c->total;
    } else if (dltotal > 0) {
        c->st->percent = (double) dlnow * 100.0 / (double) dltotal;
    }
    if (c->st->percent > 100.0) c->st->percent = 100.0;
    return 0;
}

#endif  // EASYAI_HAVE_CURL

}  // namespace

// ===========================================================================
// LlmfitBridge
// ===========================================================================

LlmfitBridge::LlmfitBridge(std::string bin, std::string download_dir, int port)
    : bin_(std::move(bin)),
      download_dir_(std::move(download_dir)),
      port_(port > 0 ? port : 8788) {
#if defined(EASYAI_HAVE_CURL)
    ensure_curl_global();
#endif
}

LlmfitBridge::~LlmfitBridge() {
    stop();
    cancel_.store(true);
    if (worker_.joinable()) worker_.join();
}

// ---- child process --------------------------------------------------------

bool LlmfitBridge::start() {
    std::lock_guard<std::mutex> lk(proc_mu_);
    if (pid_ > 0) return true;  // already running

    std::string exe = resolve_executable(bin_);
    if (exe.empty()) {
        state_.store(State::Unavailable);
        status_msg_ = "llmfit binary not found: '" + bin_ +
                      "'. Set llmfit_bin in the [SERVER] config or install llmfit.";
        easyai::log::error("[llmfit] %s\n", status_msg_.c_str());
        return false;
    }

    // Ensure the download dir exists so LLMFIT_MODELS_DIR is valid.
    if (!download_dir_.empty()) {
        std::error_code ec;
        fs::create_directories(download_dir_, ec);
    }

    // Build argv + envp in the PARENT (pointers into these strings survive
    // into the child, which only reads them).
    std::string port_arg = std::to_string(port_);
    std::vector<std::string> argv = {
        exe, "serve", "--host", "127.0.0.1", "--port", port_arg
    };

    std::vector<std::string> envp;
    const std::string inject = "LLMFIT_MODELS_DIR=" + download_dir_;
    for (char ** e = environ; e && *e; ++e) {
        if (std::strncmp(*e, "LLMFIT_MODELS_DIR=", 18) == 0) continue;  // ours wins
        envp.emplace_back(*e);
    }
    if (!download_dir_.empty()) envp.push_back(inject);

    std::vector<char *> argv_c;
    argv_c.reserve(argv.size() + 1);
    for (auto & s : argv) argv_c.push_back(const_cast<char *>(s.c_str()));
    argv_c.push_back(nullptr);

    std::vector<char *> envp_c;
    envp_c.reserve(envp.size() + 1);
    for (auto & s : envp) envp_c.push_back(const_cast<char *>(s.c_str()));
    envp_c.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        state_.store(State::Unavailable);
        status_msg_ = std::string("fork() failed: ") + std::strerror(errno);
        return false;
    }

    if (pid == 0) {
        // ---- CHILD: async-signal-safe only until execve ----
        ::setpgid(0, 0);
#if defined(__linux__)
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
        ::signal(SIGINT,  SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);

        int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devnull >= 0) { ::dup2(devnull, 0); ::close(devnull); }
        // Keep stdout/stderr on ours so the operator sees llmfit's log.

        // Close every other inherited fd — crucially the easyai-server
        // listening socket must not leak into llmfit.
        struct rlimit rl{};
        long maxfd = kMaxFdScan;
        if (::getrlimit(RLIMIT_NOFILE, &rl) == 0
                && rl.rlim_cur != RLIM_INFINITY
                && rl.rlim_cur > 0
                && rl.rlim_cur < (rlim_t) kMaxFdScan) {
            maxfd = (long) rl.rlim_cur;
        }
        for (int fd = 3; fd < (int) maxfd; ++fd) ::close(fd);

        ::execve(exe.c_str(), argv_c.data(), envp_c.data());
        const char msg[] = "llmfit: execve failed\n";
        ssize_t w = ::write(2, msg, sizeof(msg) - 1);
        (void) w;
        ::_exit(127);
    }

    // ---- PARENT ----
    ::setpgid(pid, pid);
    pid_ = pid;
    state_.store(State::Starting);
    status_msg_ = "starting llmfit (" + exe + ") on 127.0.0.1:" + port_arg;
    easyai::log::write("[llmfit] %s\n", status_msg_.c_str());
    return true;
}

bool LlmfitBridge::wait_ready(int attempts, int per_try_ms) {
    long pid;
    {
        std::lock_guard<std::mutex> lk(proc_mu_);
        pid = pid_;
    }
    if (pid <= 0) return false;

    for (int i = 0; i < attempts; ++i) {
        httplib::Client probe("http://127.0.0.1:" + std::to_string(port_));
        probe.set_connection_timeout(0, per_try_ms * 1000);
        probe.set_read_timeout(0, per_try_ms * 1000);
        auto r = probe.Get("/health");
        if (r && r->status >= 200 && r->status < 500) {
            state_.store(State::Running);
            std::lock_guard<std::mutex> lk(proc_mu_);
            status_msg_ = "llmfit running on 127.0.0.1:" + std::to_string(port_);
            easyai::log::write("[llmfit] %s\n", status_msg_.c_str());
            return true;
        }
        // Did the child die already? (e.g. port in use, bad binary)
        int wst = 0;
        if (::waitpid((pid_t) pid, &wst, WNOHANG) == (pid_t) pid) {
            std::lock_guard<std::mutex> lk(proc_mu_);
            pid_ = -1;
            state_.store(State::Unavailable);
            status_msg_ = "llmfit exited during startup (check the binary and "
                          "that port " + std::to_string(port_) + " is free)";
            easyai::log::error("[llmfit] %s\n", status_msg_.c_str());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(per_try_ms));
    }
    state_.store(State::Unavailable);
    std::lock_guard<std::mutex> lk(proc_mu_);
    status_msg_ = "llmfit did not become ready on port " + std::to_string(port_);
    easyai::log::error("[llmfit] %s\n", status_msg_.c_str());
    return false;
}

void LlmfitBridge::stop() {
    std::lock_guard<std::mutex> lk(proc_mu_);
    if (pid_ <= 0) return;
    pid_t pid = (pid_t) pid_;

    ::kill(-pid, SIGTERM);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    int status = 0;
    bool reaped = false;
    for (;;) {
        if (::waitpid(pid, &status, WNOHANG) == pid) { reaped = true; break; }
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!reaped) {
        ::kill(-pid, SIGKILL);
        ::waitpid(pid, &status, 0);
    }
    pid_ = -1;
    state_.store(State::Stopped);
    easyai::log::write("[llmfit] stopped\n");
}

std::string LlmfitBridge::status_message() {
    std::lock_guard<std::mutex> lk(proc_mu_);
    return status_msg_;
}

// ---- proxy ----------------------------------------------------------------

LlmfitBridge::ProxyResult LlmfitBridge::proxy_get(const std::string & path_and_query) {
    ProxyResult out;
    httplib::Client cli("http://127.0.0.1:" + std::to_string(port_));
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(30, 0);
    auto r = cli.Get(path_and_query);
    if (!r) {
        out.status = 502;
        out.body   = "{\"error\":\"llmfit unavailable: " +
                     std::string(httplib::to_string(r.error())) + "\"}";
        return out;
    }
    out.status       = r->status;
    out.body         = r->body;
    out.content_type = r->get_header_value("Content-Type", "application/json");
    return out;
}

LlmfitBridge::ProxyResult LlmfitBridge::proxy_post(const std::string & path,
                                                   const std::string & body,
                                                   const std::string & content_type) {
    ProxyResult out;
    httplib::Client cli("http://127.0.0.1:" + std::to_string(port_));
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(30, 0);
    auto r = cli.Post(path, body, content_type);
    if (!r) {
        out.status = 502;
        out.body   = "{\"error\":\"llmfit unavailable: " +
                     std::string(httplib::to_string(r.error())) + "\"}";
        return out;
    }
    out.status       = r->status;
    out.body         = r->body;
    out.content_type = r->get_header_value("Content-Type", "application/json");
    return out;
}

// ---- download manager -----------------------------------------------------

bool LlmfitBridge::hf_repo_files(const std::string & repo,
                                 std::vector<RepoFile> & out, std::string & err) {
    out.clear();
#if !defined(EASYAI_HAVE_CURL)
    err = "server built without libcurl";
    return false;
#else
    if (repo.empty() || repo.find("..") != std::string::npos) {
        err = "invalid repo"; return false;
    }
    const std::string url =
        "https://huggingface.co/api/models/" + repo + "/tree/main?recursive=true";
    std::string body;
    if (!curl_get_string(url, body, err)) return false;

    try {
        auto j = nlohmann::json::parse(body);
        if (!j.is_array()) { err = "unexpected HF response"; return false; }
        for (const auto & e : j) {
            if (e.value("type", "") != "file") continue;
            std::string path = e.value("path", "");
            if (!ends_with_ci(path, ".gguf")) continue;
            RepoFile f;
            f.path       = path;
            f.size_bytes = e.value("size", (std::uint64_t) 0);
            out.push_back(std::move(f));
        }
    } catch (const std::exception & e) {
        err = std::string("parse error: ") + e.what();
        return false;
    }
    if (out.empty()) err = "no .gguf files found in " + repo;
    return !out.empty();
#endif
}

int LlmfitBridge::start_download(const std::string & repo,
                                 const std::string & filename, std::string & err) {
#if !defined(EASYAI_HAVE_CURL)
    (void) repo; (void) filename;
    err = "server built without libcurl";
    return -1;
#else
    // Serialize the whole start: only one download may be set up (and at
    // most one worker exists) at a time. Joining the previous worker is
    // done below while holding ONLY this mutex, never dl_mu_, so the
    // finishing worker can still take dl_mu_ to publish its final state.
    std::lock_guard<std::mutex> start_lk(dl_start_mu_);
    {
        std::lock_guard<std::mutex> lk(dl_mu_);
        if (st_.state == "downloading") {
            err = "a download is already in progress";
            return -1;
        }
    }

    std::vector<RepoFile> files;
    if (!hf_repo_files(repo, files, err)) return -1;

    // Choose which file(s) to fetch. A sharded pick expands to its whole
    // shard set; a standalone pick is exactly that one file (a standalone
    // .gguf sharing a stem with a shard set is NOT pulled in).
    std::vector<RepoFile> target;
    std::string label;
    if (!filename.empty()) {
        if (filename.find('/') != std::string::npos ||
            filename.find("..") != std::string::npos ||
            !ends_with_ci(filename, ".gguf")) {
            err = "invalid filename"; return -1;
        }
        if (is_shard(filename)) {
            std::string fam = family_base(filename);
            for (auto & f : files) {
                std::string bn = base_name(f.path);
                if (is_shard(bn) && family_base(bn) == fam) target.push_back(f);
            }
        } else {
            for (auto & f : files)
                if (base_name(f.path) == filename) target.push_back(f);
        }
        if (target.empty()) {  // not in tree listing — fetch exactly that path
            RepoFile f; f.path = filename; target.push_back(f);
        }
        label = filename;
    } else {
        // Auto-select the best-quality file by quant rank (ties: larger).
        const RepoFile * best = nullptr;
        int best_rank = 1 << 30;
        for (auto & f : files) {
            int rank = quant_rank(base_name(f.path));
            std::uint64_t best_size = best ? best->size_bytes : 0;
            if (rank < best_rank || (rank == best_rank && f.size_bytes > best_size)) {
                best_rank = rank; best = &f;
            }
        }
        if (best) {
            std::string bn = base_name(best->path);
            if (is_shard(bn)) {
                std::string fam = family_base(bn);
                for (auto & f : files) {
                    std::string n = base_name(f.path);
                    if (is_shard(n) && family_base(n) == fam) target.push_back(f);
                }
                label = fam + ".gguf";
            } else {
                target.push_back(*best);
                label = bn;
            }
        }
    }
    if (target.empty()) { err = "no matching .gguf file"; return -1; }

    std::sort(target.begin(), target.end(),
              [](const RepoFile & a, const RepoFile & b) { return a.path < b.path; });

    // Validate every destination up front so we fail before spawning.
    for (auto & f : target) {
        fs::path dest;
        std::string verr;
        if (!safe_dest(download_dir_, base_name(f.path), dest, verr)) {
            err = verr + " (" + base_name(f.path) + ")";
            return -1;
        }
        std::error_code ec;
        if (fs::exists(dest, ec)) {
            err = "already downloaded: " + base_name(f.path) +
                  " — delete it first to re-download";
            return -1;
        }
    }

    // Reap the previous (already-finished) worker before reusing the handle.
    // Safe here: state != "downloading" was confirmed above, so the prior
    // worker has published its final state and released dl_mu_.
    if (worker_.joinable()) worker_.join();

    int id;
    {
        std::lock_guard<std::mutex> lk(dl_mu_);
        cancel_.store(false);
        id = ++next_id_;
        st_ = DownloadStatus{};
        st_.id       = id;
        st_.repo     = repo;
        st_.filename = label;
        st_.state    = "downloading";
    }
    worker_ = std::thread(&LlmfitBridge::download_worker, this, id, repo, target);
    easyai::log::write("[llmfit] download #%d %s (%zu file(s)) -> %s\n",
                      id, repo.c_str(), target.size(), download_dir_.c_str());
    return id;
#endif
}

void LlmfitBridge::download_worker(int id, std::string repo,
                                   std::vector<RepoFile> files) {
#if defined(EASYAI_HAVE_CURL)
    (void) id;
    std::uint64_t grand_total = 0;
    for (auto & f : files) grand_total += f.size_bytes;
    {
        std::lock_guard<std::mutex> lk(dl_mu_);
        st_.total_bytes = grand_total;
    }

    std::uint64_t completed = 0;
    bool ok = true;
    std::string err;

    for (auto & f : files) {
        if (cancel_.load()) { ok = false; err = "cancelled"; break; }

        std::string name = base_name(f.path);
        fs::path dest;
        if (!safe_dest(download_dir_, name, dest, err)) { ok = false; break; }
        std::string part = dest.string() + ".part";

        {
            std::lock_guard<std::mutex> lk(dl_mu_);
            st_.filename = name;
        }

        std::FILE * fp = std::fopen(part.c_str(), "wb");
        if (!fp) { ok = false; err = "cannot open " + part; break; }

        CURL * c = curl_easy_init();
        if (!c) { std::fclose(fp); ok = false; err = "curl_easy_init failed"; break; }

        DlCb cb;
        cb.fp = fp; cb.mu = &dl_mu_; cb.st = &st_; cb.cancel = &cancel_;
        cb.base = completed; cb.total = grand_total;

        const std::string url =
            "https://huggingface.co/" + repo + "/resolve/main/" + f.path;
        curl_easy_setopt(c, CURLOPT_URL,              url.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,    file_write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA,        &cb);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS,       0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA,     &cb);
        curl_easy_setopt(c, CURLOPT_FAILONERROR,      1L);
        apply_common_curl(c);

        CURLcode rc = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(c);
        std::fclose(fp);

        if (rc == CURLE_ABORTED_BY_CALLBACK) {
            std::remove(part.c_str()); ok = false; err = "cancelled"; break;
        }
        if (rc != CURLE_OK) {
            std::remove(part.c_str()); ok = false;
            err = std::string("curl: ") + curl_easy_strerror(rc); break;
        }
        if (code >= 400) {
            std::remove(part.c_str()); ok = false;
            err = "HTTP " + std::to_string(code); break;
        }
        std::error_code ec;
        fs::rename(part, dest, ec);
        if (ec) {
            std::remove(part.c_str()); ok = false;
            err = "rename failed: " + ec.message(); break;
        }

        std::uint64_t this_size = f.size_bytes;
        if (this_size == 0) {
            std::error_code se;
            this_size = (std::uint64_t) fs::file_size(dest, se);
            if (se) this_size = 0;
        }
        completed += this_size;
        std::lock_guard<std::mutex> lk(dl_mu_);
        st_.downloaded_bytes = completed;
    }

    std::lock_guard<std::mutex> lk(dl_mu_);
    if (ok) {
        st_.state            = "done";
        st_.percent          = 100.0;
        st_.downloaded_bytes = grand_total ? grand_total : st_.downloaded_bytes;
        easyai::log::write("[llmfit] download #%d done: %s\n", id, repo.c_str());
    } else if (err == "cancelled") {
        st_.state = "idle";
        st_.error.clear();
        easyai::log::write("[llmfit] download #%d cancelled\n", id);
    } else {
        st_.state = "error";
        st_.error = err;
        easyai::log::error("[llmfit] download #%d failed: %s\n", id, err.c_str());
    }
#else
    (void) id; (void) repo; (void) files;
    std::lock_guard<std::mutex> lk(dl_mu_);
    st_.state = "error";
    st_.error = "server built without libcurl";
#endif
}

void LlmfitBridge::cancel_download() {
    cancel_.store(true, std::memory_order_relaxed);
}

LlmfitBridge::DownloadStatus LlmfitBridge::download_status() {
    std::lock_guard<std::mutex> lk(dl_mu_);
    return st_;
}

// ---- local model directory ------------------------------------------------

std::vector<LlmfitBridge::LocalModel> LlmfitBridge::list_local() {
    std::vector<LocalModel> out;
    std::error_code ec;
    if (download_dir_.empty() || !fs::is_directory(download_dir_, ec)) return out;

    for (const auto & e : fs::directory_iterator(download_dir_, ec)) {
        if (ec) break;
        std::error_code fe;
        if (!e.is_regular_file(fe)) continue;
        std::string name = e.path().filename().string();
        if (!ends_with_ci(name, ".gguf")) continue;

        LocalModel m;
        m.name       = name;
        m.size_bytes = (std::uint64_t) fs::file_size(e.path(), fe);
        if (fe) m.size_bytes = 0;
        auto wt = fs::last_write_time(e.path(), fe);
        if (!fe) {
            // file_time_type -> system_clock isn't standard before C++20;
            // use the portable clock-difference idiom for a display value.
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                wt - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            m.mtime = (std::int64_t) std::chrono::system_clock::to_time_t(sctp);
        }
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(),
              [](const LocalModel & a, const LocalModel & b) {
                  return a.name < b.name;
              });
    return out;
}

bool LlmfitBridge::delete_local(const std::string & name, std::string & err) {
    fs::path dest;
    if (!safe_dest(download_dir_, name, dest, err)) return false;

    std::error_code ec;
    if (!fs::exists(dest, ec)) { err = "not found: " + name; return false; }
    if (!fs::is_regular_file(dest, ec)) {  // refuse symlinks / dirs
        err = "not a regular file: " + name; return false;
    }
    if (!fs::remove(dest, ec) || ec) {
        err = "delete failed: " + (ec ? ec.message() : std::string("unknown"));
        return false;
    }
    easyai::log::write("[llmfit] deleted %s\n", name.c_str());
    return true;
}

}  // namespace easyai
