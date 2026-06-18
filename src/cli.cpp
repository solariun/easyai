// libeasyai-side bits of easyai::cli — the Client overload of
// Toolbelt::apply lives in src/cli_client.cpp (libeasyai-cli) so the
// engine-only library doesn't end up depending on the HTTP client.
#include "easyai/cli.hpp"

#include "easyai/builtin_tools.hpp"
#include "easyai/engine.hpp"
#include "easyai/log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>    // open, O_*
#include <filesystem>
#include <iterator>   // make_move_iterator
#include <sys/stat.h> // mode_t
#include <unistd.h>   // getpid

namespace fs = std::filesystem;

namespace easyai::cli {

// ============================================================================
// Toolbelt
// ============================================================================
Toolbelt & Toolbelt::sandbox     (std::string dir) { sandbox_      = std::move(dir); return *this; }
Toolbelt & Toolbelt::allow_fs    (bool on)         { allow_fs_     = on;             return *this; }
Toolbelt & Toolbelt::allow_bash  (bool on)         { allow_bash_   = on;             return *this; }
Toolbelt & Toolbelt::allow_python(bool on)         { allow_python_ = on;             return *this; }
Toolbelt & Toolbelt::show_bash   (bool on)         { show_bash_    = on;             return *this; }
Toolbelt & Toolbelt::show_python (bool on)         { show_python_  = on;             return *this; }
Toolbelt & Toolbelt::with_plan   (Plan & plan)     { plan_         = &plan;          return *this; }
Toolbelt & Toolbelt::no_web      (bool on)         { no_web_       = on;             return *this; }
Toolbelt & Toolbelt::no_datetime (bool on)         { no_datetime_  = on;             return *this; }
Toolbelt & Toolbelt::use_google  (bool on)         { use_google_   = on;             return *this; }
Toolbelt & Toolbelt::tool_mode   (ToolMode m)      { tool_mode_    = m;              return *this; }

std::vector<Tool> Toolbelt::tools() const {
    std::vector<Tool> out;
    out.reserve(8);
    if (!no_datetime_) out.push_back(easyai::tools::datetime());
    if (plan_)         out.push_back(plan_->tool());
    // Unified `web` tool: action="search" (engine ddg/google) +
    // action="fetch". google_enabled is the operator opt-in gate
    // (--use-google) that keeps the billed Google CSE off by default;
    // env vars are still re-read on every call so key rotation works
    // without a restart and a missing key surfaces a clear error.
    if (!no_web_) {
        bool google = false;
        if (use_google_) {
            const char * gk = std::getenv("GOOGLE_API_KEY");
            const char * gx = std::getenv("GOOGLE_CSE_ID");
            google = (gk && *gk && gx && *gx);
        }
        if (tool_mode_ == ToolMode::Unified || tool_mode_ == ToolMode::Both) {
            out.push_back(easyai::tools::web(google));
        }
        if (tool_mode_ == ToolMode::Split   || tool_mode_ == ToolMode::Both) {
            auto v = easyai::tools::web_split(google);
            out.insert(out.end(),
                       std::make_move_iterator(v.begin()),
                       std::make_move_iterator(v.end()));
        }
    }
    // fs / bash / python3 share a working root: the configured sandbox
    // if set, otherwise ".". The unified `fs` and `python3` are
    // auto-on whenever the operator has signalled "the model can
    // touch this filesystem" — either by setting a sandbox, or by
    // enabling bash (which strictly subsumes both). Without that
    // signal we register neither, even with allow_python_=true;
    // python3 defaults on so a server with --sandbox gets it for
    // free, but pointing it at the operator's bare cwd would expose
    // ambient files we never meant to expose.
    const bool bash_on   = allow_bash_;
    const bool python_on = allow_python_ && (!sandbox_.empty() || bash_on);
    const bool fs_on     = allow_fs_     && (!sandbox_.empty() || bash_on || python_on);
    const std::string fs_root = sandbox_.empty() ? "." : sandbox_;
    if (fs_on) {
        // The fs surface — Unified (single dispatcher with `action`),
        // Split (one tool per action: read_file, edit_file, …), or Both
        // (registers both surfaces so the model can pick).
        if (tool_mode_ == ToolMode::Unified || tool_mode_ == ToolMode::Both) {
            out.push_back(easyai::tools::fs(fs_root));
        }
        if (tool_mode_ == ToolMode::Split   || tool_mode_ == ToolMode::Both) {
            auto v = easyai::tools::fs_split(fs_root);
            out.insert(out.end(),
                       std::make_move_iterator(v.begin()),
                       std::make_move_iterator(v.end()));
        }
    }
    if (bash_on) {
        out.push_back(easyai::tools::bash(fs_root, show_bash_));
    }
    if (python_on) {
        out.push_back(easyai::tools::python3(fs_root, show_python_));
    }
    return out;
}

void Toolbelt::apply(Engine & engine) const {
    for (auto & t : tools()) engine.add_tool(t);
    // bash and python3 are both interactive subprocess executors whose
    // flows naturally span many turns — bump the agentic-loop ceiling
    // when either is enabled so the default 8-hop cap doesn't truncate
    // real work.
    if (allow_bash_ || allow_python_) engine.max_tool_hops(99999);
}
// Toolbelt::apply(Client &) lives in src/cli_client.cpp — libeasyai-cli.

// ============================================================================
// open_log_tee / close_log_tee
// ============================================================================
std::FILE * open_log_tee(const std::string & path,
                         const std::string & prefix,
                         int argc, char ** argv,
                         std::string * resolved_path) {
    std::string resolved;
    const bool auto_path = path.empty();
    if (!auto_path) {
        resolved = path;
    } else {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "/tmp/%s-%d-%ld.log",
                      prefix.empty() ? "easyai" : prefix.c_str(),
                      (int) ::getpid(),
                      (long) std::time(nullptr));
        resolved = buf;
    }
    if (resolved_path) *resolved_path = resolved;

    // Auto-generated /tmp paths are predictable (pid + epoch), so we
    // refuse atomically if the path already exists (O_EXCL) — a local
    // attacker who pre-creates the path as a symlink would otherwise
    // redirect our truncating write to an arbitrary user-writable file.
    // Caller-supplied paths skip O_EXCL because operators legitimately
    // want overwrite semantics for log rotation, but we still pin
    // O_NOFOLLOW (refuse if the leaf is a symlink — operators planting
    // a symlink there is suspicious) and mode 0600 (logs may echo
    // prompts that contain secrets).
    int flags = O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC;
    flags |= auto_path ? O_EXCL : O_TRUNC;
    const int fd = ::open(resolved.c_str(), flags, 0600);
    if (fd < 0) return nullptr;
    std::FILE * fp = ::fdopen(fd, "w");
    if (!fp) { ::close(fd); return nullptr; }

    // Header — keeps a stray log self-describing.
    char ts[32] = {0};
    const auto t = std::time(nullptr);
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    std::fprintf(fp,
        "%s raw transaction log\n"
        "started: %s   pid: %d\nargv:",
        prefix.empty() ? "easyai" : prefix.c_str(),
        ts, (int) ::getpid());
    for (int k = 0; k < argc; ++k) std::fprintf(fp, " %s", argv[k]);
    std::fputc('\n', fp);
    std::fflush(fp);

    easyai::log::set_file(fp);
    return fp;
}

void close_log_tee(std::FILE * fp) {
    if (!fp) return;
    std::fputs("\n========== END OF LOG ==========\n", fp);
    std::fclose(fp);
    easyai::log::set_file(nullptr);
}

// ============================================================================
// validate_sandbox
// ============================================================================
bool validate_sandbox(const std::string & path, std::string & err) {
    if (path.empty()) return true;
    std::error_code ec;
    auto stat = fs::status(path, ec);
    if (ec || !fs::exists(stat)) {
        err = "--sandbox " + path + " does not exist";
        return false;
    }
    if (!fs::is_directory(stat)) {
        err = "--sandbox " + path + " is not a directory";
        return false;
    }
    return true;
}

}  // namespace easyai::cli
