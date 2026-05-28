// src/external_tools.cpp — JSON-manifest tool loader.
//
// Threat model and guarantees are documented in
// include/easyai/external_tools.hpp; read that first if you are
// reviewing this file. The implementation below is the enforcement
// of those guarantees. Every check has a reason.
//
// The structure is:
//
//   * Pure parsing/validation (load_external_tools_from_json) runs in
//     the parent before any process is spawned. It produces
//     ExternalToolSpec values that own all the strings the spawn path
//     will need.
//
//   * Per-call dispatch (the handler lambda captured by each Tool)
//     re-validates the model's arguments against the spec, builds a
//     concrete argv + envp, and calls run_external_command — which is
//     the ONLY place where fork()+execve() happens.
//
//   * run_external_command is signal-safe in the child: between fork
//     and execve we touch only async-signal-safe APIs (close, dup2,
//     chdir, _exit, write, execve) and pre-built C-string buffers
//     allocated in the parent.

#include "easyai/external_tools.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace easyai {

namespace {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Compile-time / load-time limits
// ---------------------------------------------------------------------------
//
// Hard caps applied at load time. Each of these has been chosen as
// "cannot conceivably be needed by a legitimate manifest, can plausibly
// be tried by a hostile one". Tighter than necessary on purpose.
//
// Don't relax these without a written reason. Each one closes a class
// of attack:
//
//   kMaxManifestBytes      pathological-JSON DoS (heap exhaustion).
//   kMaxArgvElements       argv overflow / kernel ARG_MAX exhaustion.
//   kMaxArgElementBytes    individual argv string blowing past sane size.
//   kMaxToolsPerManifest   reflective-add denial of service.
//   kMaxParamsPerTool      schema explosion (validators are O(n*m)).
//   kMaxEnvPassthrough     env table size cap.
//   kPathMaxFallback       PATH_MAX fallback for platforms that don't
//                          define one (POSIX makes it optional).
//   kTimeoutMin/Max        timeout floor and ceiling (ms).
//   kOutputCapMin/Max      stdout capture floor and ceiling (bytes).
constexpr std::size_t kMaxManifestBytes      = 1u  << 20;   // 1 MiB
constexpr std::size_t kMaxArgvElements       = 256;
constexpr std::size_t kMaxArgElementBytes    = 4096;
constexpr std::size_t kMaxToolsPerManifest   = 128;
constexpr std::size_t kMaxParamsPerTool      = 32;
constexpr std::size_t kMaxEnvPassthrough     = 16;
constexpr std::size_t kPathMaxFallback       = 4096;
constexpr int         kTimeoutMin            = 100;
constexpr int         kTimeoutMax            = 300000;
constexpr int         kTimeoutDefault        = 10000;
constexpr std::size_t kOutputCapMin          = 1024;
constexpr std::size_t kOutputCapMax          = 4u  << 20;   // 4 MiB
constexpr std::size_t kOutputCapDefault      = 64u << 10;   // 64 KiB

// Identifier and description size caps. The tool-name cap mirrors the
// validator regex `^[a-zA-Z][a-zA-Z0-9_]{0,63}$` (1 lead + up to 63 = 64
// total). Description caps are generous enough to fit a paragraph but
// small enough that a hostile manifest can't blow up the schema
// serialiser.
constexpr std::size_t kMaxToolNameBytes      = 64;
constexpr std::size_t kMaxToolDescBytes      = 4096;
constexpr std::size_t kMaxParamDescBytes     = 2048;

// Child-side fd-close cap. Used as the upper bound on the close() loop
// that runs between fork() and execve() so the spawned command doesn't
// inherit the parent's HTTP transport, log file, KV cache mmap, etc.
//
// We cannot just trust RLIMIT_NOFILE: on systems with `ulimit -n
// unlimited` `rlim_cur` is RLIM_INFINITY (≈ULONG_MAX), which when cast
// to `int` becomes -1 and silently disables the loop. Capping at a
// large-but-finite number keeps the protection effective AND keeps the
// loop fast even when rlim_cur is set to something huge like 1 << 20.
constexpr long        kMaxFdScan             = 65536;

// Exit codes for child-side failure paths between fork() and execve().
// POSIX shells use 126 for "found but not executable" and 127 for "not
// found"; we re-use the convention so the parent can distinguish
// chdir failure (post-validation race) from execve failure (binary
// vanished or got chmod -x'd between manifest load and call).
constexpr int         kExitChdirFailed       = 126;
constexpr int         kExitExecveFailed      = 127;

// Parent poll/kill timing.
//
//   kParentPollMs       cadence at which the parent re-checks the
//                       deadline (200 ms = 5 wakeups/sec — cheap, and
//                       a SIGTERM sent within 200 ms of deadline is
//                       still well within human-perceptible latency).
//   kKillGraceMs        SIGTERM → SIGKILL window. Long enough for a
//                       cooperative process to flush; short enough
//                       that a hung process gets killed promptly.
//   kUnkillableWaitSec  After SIGKILL, if waitpid still hasn't reaped
//                       the child within this many seconds something
//                       is very wrong (uninterruptible sleep, kernel
//                       bug). Fall back to blocking waitpid so we
//                       don't spin.
constexpr int         kParentPollMs          = 200;
constexpr int         kKillGraceMs           = 1000;
constexpr int         kUnkillableWaitSec     = 5;

// Output drain plumbing.
//
//   kDrainBufBytes        per-read() syscall buffer; 4 KiB matches the
//                         kernel pipe buffer chunk size on Linux.
//   kInitialOutputReserve initial std::string::reserve cap so a
//                         "Hello\n"-sized response doesn't allocate
//                         the full max_output_bytes up front.
constexpr std::size_t kDrainBufBytes         = 4096;
constexpr std::size_t kInitialOutputReserve  = 8192;

// Manifest-file naming convention for directory-mode loads. Files
// whose name doesn't match `EASYAI-<at-least-1-char>.tools` are
// silently skipped at scan time — that's how operators "disable" a
// pack without deleting it (rename to `.tools.disabled` etc).
constexpr char        kManifestPrefix[]      = "EASYAI-";
constexpr char        kManifestSuffix[]      = ".tools";
constexpr std::size_t kManifestPrefixLen     = sizeof(kManifestPrefix) - 1;
constexpr std::size_t kManifestSuffixLen     = sizeof(kManifestSuffix) - 1;
constexpr std::size_t kManifestMinFilename   =
    kManifestPrefixLen + 1 + kManifestSuffixLen;   // e.g. "EASYAI-x.tools"

// PATH_MAX is technically optional in POSIX — fall back to a sane number
// on platforms that don't define it (Linux defines it via <linux/limits.h>
// pulled in by <climits>).
constexpr std::size_t kPathMax =
#ifdef PATH_MAX
    PATH_MAX
#else
    kPathMaxFallback
#endif
    ;

// ---------------------------------------------------------------------------
// Built-in name reservation
// ---------------------------------------------------------------------------
//
// Manifest tools cannot collide with these. The check is in addition to
// the caller-supplied `reserved_names` list (which carries any other
// already-registered tools). We hard-code the built-in names here so
// that even a caller who forgets to pre-populate `reserved_names`
// can't accidentally let a manifest shadow `bash`.
const std::unordered_set<std::string> kBuiltInNames = {
    "datetime", "web", "fs", "bash", "python3", "knowledge", "tool_lookup",
};

// ---------------------------------------------------------------------------
// Validators (pure functions — no I/O)
// ---------------------------------------------------------------------------

// Tool name must match ^[a-zA-Z][a-zA-Z0-9_]{0,63}$. We don't pull in
// std::regex because it's overkill and has worst-case stack-recursion
// behaviour on adversarial input (the same reason the rest of the
// codebase avoids it). Hand-rolled char scan is O(n), bounded.
bool is_valid_tool_name(const std::string & s) {
    if (s.empty() || s.size() > kMaxToolNameBytes) return false;
    auto is_alpha = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    auto is_alnum_us = [&](char c) {
        return is_alpha(c) || (c >= '0' && c <= '9') || c == '_';
    };
    if (!is_alpha(s[0])) return false;
    for (std::size_t i = 1; i < s.size(); ++i) {
        if (!is_alnum_us(s[i])) return false;
    }
    return true;
}

// Parameter names follow the same rule as tool names — they're keys in
// the JSON-Schema `properties` map and we use them as placeholder
// identifiers in argv templates.
bool is_valid_param_name(const std::string & s) {
    return is_valid_tool_name(s);
}

// True iff `s` is exactly "{name}" where `name` is a valid identifier.
// On success, writes the bare name (without braces) to `out`. On
// failure, `out` is unchanged. We forbid placeholders inside larger
// strings (e.g. "--flag={x}") because string interpolation invites
// quoting/escaping mistakes — a placeholder that survives validation
// must flow through as one whole argv element so the model's value is
// always exactly one execve argument.
bool parse_placeholder(const std::string & s, std::string & out) {
    if (s.size() < 3) return false;
    if (s.front() != '{' || s.back() != '}') return false;
    std::string inner = s.substr(1, s.size() - 2);
    if (!is_valid_param_name(inner)) return false;
    out = std::move(inner);
    return true;
}

// True iff `s` contains no '{' or '}' anywhere. We use this to reject
// half-placeholders embedded in literal strings ("--flag={x}", "{x}y").
// A literal that legitimately contains a brace can still be expressed
// — it just has to be split at the brace into two separate argv
// elements, which is the right design (the brace then has no
// interpolation behaviour).
bool has_no_braces(const std::string & s) {
    return s.find_first_of("{}") == std::string::npos;
}

// True iff `path` is an absolute path to a regular, executable file.
// We require absolute (starts with '/') so we never do a PATH search —
// PATH-hijack is one of the easiest remote-priv-escalation patterns
// and we close that door at load time.
bool is_executable_absolute_path(const std::string & path, std::string & why) {
    if (path.empty() || path[0] != '/') {
        why = "must be an absolute path (start with '/')";
        return false;
    }
    if (path.size() >= kPathMax) {
        why = "path too long";
        return false;
    }
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        why = std::string("stat failed: ") + std::strerror(errno);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        why = "not a regular file";
        return false;
    }
    if (::access(path.c_str(), X_OK) != 0) {
        why = "not executable for the current user";
        return false;
    }
    return true;
}

// Resolve a manifest `cwd` field. Either a literal absolute path or
// the magic token "$SANDBOX" which means "the process's CWD at load
// time". Returns false with `why` populated on failure.
bool resolve_cwd(const std::string & raw, std::string & out, std::string & why) {
    if (raw == "$SANDBOX") {
        // We capture getcwd at load time, not at call time, so the
        // tool's cwd is fixed by the deploy environment regardless of
        // any later chdir the application might do.
        char buf[kPathMax];
        if (::getcwd(buf, sizeof(buf)) == nullptr) {
            why = std::string("getcwd failed: ") + std::strerror(errno);
            return false;
        }
        out.assign(buf, ::strnlen(buf, sizeof(buf)));
        return true;
    }
    if (raw.empty() || raw[0] != '/') {
        why = "cwd must be an absolute path or \"$SANDBOX\"";
        return false;
    }
    if (raw.size() >= kPathMax) {
        why = "cwd path too long";
        return false;
    }
    struct stat st{};
    if (::stat(raw.c_str(), &st) != 0) {
        why = std::string("cwd stat failed: ") + std::strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        why = "cwd is not a directory";
        return false;
    }
    out = raw;
    return true;
}

// ---------------------------------------------------------------------------
// ExternalToolSpec — the validated-once shape we keep alive per tool.
// ---------------------------------------------------------------------------
//
// One of these is created per manifest entry, captured by shared_ptr
// inside the Tool's handler lambda. Strings are owned (no string_views
// pointing into the original JSON document, which we drop after parse).
struct ParamSpec {
    std::string name;
    std::string type;        // "string" | "integer" | "number" | "boolean"
    std::string description;
    bool        required = false;
};

// One of three things an argv element can be:
//   - a literal string passed straight to execve
//   - a placeholder reference to a parameter
//   - (validated away at load — we never store half-placeholders)
struct ArgvSegment {
    bool        is_placeholder = false;
    std::string text;        // literal text OR the placeholder name
};

struct ExternalToolSpec {
    std::string                 name;
    std::string                 description;
    std::string                 command;       // absolute path
    std::vector<ArgvSegment>    argv_template;
    std::vector<ParamSpec>      params;
    int                         timeout_ms       = kTimeoutDefault;
    std::size_t                 max_output_bytes = kOutputCapDefault;
    std::string                 cwd;            // resolved absolute path
    std::vector<std::string>    env_passthrough; // var names; values read at call time
    bool                        merge_stderr    = true;
    bool                        nonzero_is_error = true;
};

// Build a short JSON-schema object string for the model from the
// validated parameter list. Hand-crafted so we don't depend on JSON
// serialisation order or ordered_json — keeps the public schema shape
// stable across nlohmann releases.
std::string build_parameters_schema_json(const std::vector<ParamSpec> & params) {
    json schema;
    schema["type"]       = "object";
    schema["properties"] = json::object();
    json required = json::array();
    for (const auto & p : params) {
        json prop;
        prop["type"]        = p.type;
        prop["description"] = p.description;
        schema["properties"][p.name] = std::move(prop);
        if (p.required) required.push_back(p.name);
    }
    schema["required"] = std::move(required);
    return schema.dump();
}

// ---------------------------------------------------------------------------
// Per-call argument validation
// ---------------------------------------------------------------------------
//
// The model's arguments arrive as a JSON string in ToolCall.arguments_json.
// Parse it once, then for every parameter declared on this tool:
//
//   - if required and missing: error
//   - if present, type must match the declared schema
//   - extras (keys not in the schema) are silently ignored — typical
//     OpenAI tool-call streams sometimes include leftover keys
//
// On success, fills `values` with the validated stringified form of
// each parameter (this is what we substitute into placeholders).
bool validate_call_arguments(const std::string &                args_json,
                             const std::vector<ParamSpec> &     params,
                             std::vector<std::string> &         values_out,
                             std::string &                      err) {
    json doc;
    try {
        doc = json::parse(args_json.empty() ? std::string("{}") : args_json);
    } catch (const std::exception & e) {
        err = std::string("arguments are not valid JSON: ") + e.what();
        return false;
    }
    if (!doc.is_object()) {
        err = "arguments must be a JSON object";
        return false;
    }
    values_out.clear();
    values_out.reserve(params.size());
    for (const auto & p : params) {
        auto it = doc.find(p.name);
        if (it == doc.end() || it->is_null()) {
            if (p.required) {
                err = "missing required argument: " + p.name;
                return false;
            }
            values_out.emplace_back();   // empty placeholder; spec promises required ones are filled
            continue;
        }
        const json & v = *it;
        std::string s;
        if (p.type == "string") {
            if (!v.is_string()) {
                err = "argument " + p.name + ": expected string";
                return false;
            }
            s = v.get<std::string>();
        } else if (p.type == "integer") {
            if (!v.is_number_integer()) {
                err = "argument " + p.name + ": expected integer";
                return false;
            }
            s = std::to_string(v.get<long long>());
        } else if (p.type == "number") {
            if (!v.is_number()) {
                err = "argument " + p.name + ": expected number";
                return false;
            }
            const double d = v.get<double>();
            // nlohmann::json accepts non-finite extensions (NaN, Inf);
            // std::to_string would render them as "nan" / "inf" and the
            // wrapped command would receive that as an argv literal.
            // Reject — finite numbers are the only contract worth
            // exposing to a model.
            if (!std::isfinite(d)) {
                err = "argument " + p.name + ": must be a finite number";
                return false;
            }
            s = std::to_string(d);
        } else if (p.type == "boolean") {
            if (!v.is_boolean()) {
                err = "argument " + p.name + ": expected boolean";
                return false;
            }
            s = v.get<bool>() ? "true" : "false";
        } else {
            err = "argument " + p.name + ": unsupported declared type "
                + p.type;
            return false;
        }
        if (s.size() > kMaxArgElementBytes) {
            err = "argument " + p.name + ": value exceeds "
                + std::to_string(kMaxArgElementBytes) + " bytes";
            return false;
        }
        values_out.push_back(std::move(s));
    }
    return true;
}

// ---------------------------------------------------------------------------
// fork/execve runner — the only place we actually spawn.
// ---------------------------------------------------------------------------
//
// Inputs are owned, contiguous std::strings sitting in the parent's
// heap. We freeze them into C-string vectors before fork; after fork
// the child only reads them, never allocates.
//
// Returns a captured-output string. `exit_status` carries the wait()
// status word; `timed_out` is set if we initiated SIGTERM/SIGKILL.
struct RunResult {
    std::string output;
    int         exit_status = 0;
    bool        timed_out   = false;
    bool        truncated   = false;
};

RunResult run_external_command(const std::string &              command,
                               const std::vector<std::string> & argv,
                               const std::vector<std::string> & envp,
                               const std::string &              cwd,
                               int                              timeout_ms,
                               std::size_t                      max_output_bytes,
                               bool                             merge_stderr) {
    RunResult res;

    // Build C-string arrays in the PARENT, before fork. Pointers point
    // into the existing std::string buffers (which survive into the
    // child). Adding an extra '\0' is unnecessary — std::string already
    // null-terminates.
    std::vector<char *> argv_c;
    argv_c.reserve(argv.size() + 1);
    for (const auto & s : argv) argv_c.push_back(const_cast<char *>(s.c_str()));
    argv_c.push_back(nullptr);

    std::vector<char *> envp_c;
    envp_c.reserve(envp.size() + 1);
    for (const auto & s : envp) envp_c.push_back(const_cast<char *>(s.c_str()));
    envp_c.push_back(nullptr);

    int pipefd[2] = {-1, -1};
    if (::pipe(pipefd) != 0) {
        res.output = std::string("pipe() failed: ") + std::strerror(errno);
        res.exit_status = -1;
        return res;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        res.output = std::string("fork() failed: ") + std::strerror(errno);
        res.exit_status = -1;
        return res;
    }

    if (pid == 0) {
        // ---- CHILD ----
        // Only async-signal-safe operations from here to execve. Do
        // not allocate, do not call into the C++ runtime, do not
        // touch any mutex.

        // Put us in our own process group so the parent can SIGTERM
        // the whole group (including any grandchildren the command
        // spawns). The race-free idiom is to setpgid in BOTH child
        // (immediately) and parent (right after fork), so whichever
        // runs first wins.
        ::setpgid(0, 0);

#if defined(__linux__)
        // Tie our lifetime to the parent: if the agent process dies
        // (segfault, kill -9, OOM-killer) before we exec or while we
        // run, the kernel sends us SIGKILL. Without this an orphaned
        // tool subprocess would survive after reparenting to PID 1
        // and keep consuming resources until its own timeout. prctl
        // is async-signal-safe on Linux. Setting fires after execve
        // too, so it covers the long-running case as well.
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif

        // stdin → /dev/null (we do not give the model an stdin path).
        int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devnull >= 0) {
            ::dup2(devnull, 0);
            ::close(devnull);
        } else {
            ::close(0);
        }

        // stdout → pipe write end. stderr same iff merge_stderr.
        ::dup2(pipefd[1], 1);
        if (merge_stderr) ::dup2(pipefd[1], 2);
        else {
            int devnull2 = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (devnull2 >= 0) { ::dup2(devnull2, 2); ::close(devnull2); }
        }

        // Close the original pipe fds (we already dup'd what we need).
        ::close(pipefd[0]);
        ::close(pipefd[1]);

        // Close every other inherited fd. The parent might have files,
        // sockets, the agent's HTTP transport etc. open without
        // O_CLOEXEC; we don't want the spawned command inheriting any
        // of them.
        //
        // Three pitfalls to avoid here:
        //
        //   1. RLIMIT_NOFILE = RLIM_INFINITY. `rlim_cur` is rlim_t
        //      (unsigned), `(long) RLIM_INFINITY` wraps to -1 on most
        //      systems, `(int) -1` makes the loop body never run, and
        //      every parent fd silently leaks into the child. Cap at
        //      kMaxFdScan to defeat that.
        //   2. Reasonable-but-large rlim_cur (e.g. 1<<20 set by some
        //      container runtimes). Looping a million close() syscalls
        //      delays exec by tens of milliseconds. Same cap helps.
        //   3. Some systems set `rlim_cur = 0` after sandboxing —
        //      treat that as "use kMaxFdScan" too rather than skipping
        //      the loop.
        struct rlimit rl{};
        long maxfd = kMaxFdScan;
        if (::getrlimit(RLIMIT_NOFILE, &rl) == 0
                && rl.rlim_cur != RLIM_INFINITY
                && rl.rlim_cur > 0
                && rl.rlim_cur < (rlim_t) kMaxFdScan) {
            maxfd = (long) rl.rlim_cur;
        }
        for (int fd = 3; fd < (int) maxfd; ++fd) {
            ::close(fd);
        }

        // chdir into the resolved cwd. Failure here is fatal — the
        // operator declared a cwd and getting it wrong is a security
        // boundary, not a recoverable condition.
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
            // Best-effort error message into the pipe (which we just
            // wired into stdout/stderr) before we die. write() is
            // signal-safe; no formatting library used. We genuinely
            // can't recover from a write failure in this corner so we
            // use a real ssize_t sink to silence -Wunused-result (a
            // bare (void) cast is ignored by glibc's warn_unused_result
            // attribute).
            const char msg[] = "external_tool: chdir failed\n";
            ssize_t w = ::write(1, msg, sizeof(msg) - 1);
            (void) w;
            ::_exit(kExitChdirFailed);
        }

        // execve. On success, never returns.
        ::execve(command.c_str(), argv_c.data(), envp_c.data());

        // execve failure path. Same signal-safety rules apply.
        const char msg[] = "external_tool: execve failed\n";
        ssize_t w = ::write(1, msg, sizeof(msg) - 1);
        (void) w;
        ::_exit(kExitExecveFailed);
    }

    // ---- PARENT ----
    // Mirror the child's setpgid so the kill below targets the right
    // group regardless of scheduling order.
    ::setpgid(pid, pid);

    ::close(pipefd[1]);
    ::fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    res.output.reserve(std::min<std::size_t>(max_output_bytes,
                                             kInitialOutputReserve));

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    auto kill_deadline = std::chrono::steady_clock::time_point::max();
    bool sent_term  = false;
    bool sent_kill  = false;
    bool reaped     = false;
    int  status     = 0;

    auto drain = [&]() {
        char buf[kDrainBufBytes];
        ssize_t n;
        while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) {
            if (res.output.size() < max_output_bytes) {
                std::size_t take = std::min(
                    (std::size_t) n,
                    max_output_bytes - res.output.size());
                res.output.append(buf, take);
                if (take < (std::size_t) n) res.truncated = true;
            } else {
                res.truncated = true;
            }
        }
    };

    for (;;) {
        auto now = std::chrono::steady_clock::now();
        if (!sent_term && now >= deadline) {
            // Send SIGTERM to the process group. Negative pid in
            // ::kill targets the group whose leader has |pid|.
            ::kill(-pid, SIGTERM);
            sent_term     = true;
            res.timed_out = true;
            kill_deadline = now + std::chrono::milliseconds(kKillGraceMs);
        }
        if (sent_term && !sent_kill && now >= kill_deadline) {
            ::kill(-pid, SIGKILL);
            sent_kill = true;
        }

        // Re-check the timeout deadlines on every poll cadence —
        // kParentPollMs is the longest we'll sleep between checks.
        struct pollfd pfd{ pipefd[0], POLLIN, 0 };
        int prc = ::poll(&pfd, 1, kParentPollMs);
        if (prc < 0 && errno == EINTR) continue;
        if (prc > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            drain();
        }

        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            drain();
            reaped = true;
            break;
        }

        // Safety: if we already SIGKILLed and the child still hasn't
        // been reaped after kUnkillableWaitSec, fall back to blocking
        // waitpid so we don't spin forever on an unkillable scenario
        // (uninterruptible D state, kernel bug, exotic LSM policy).
        if (sent_kill) {
            auto since_kill = std::chrono::steady_clock::now() - kill_deadline;
            if (since_kill > std::chrono::seconds(kUnkillableWaitSec)) {
                drain();
                ::waitpid(pid, &status, 0);
                reaped = true;
                break;
            }
        }
    }
    if (!reaped) ::waitpid(pid, &status, 0);
    ::close(pipefd[0]);

    res.exit_status = status;
    return res;
}

// ---------------------------------------------------------------------------
// JSON parsing helpers
// ---------------------------------------------------------------------------
//
// Field-by-field with explicit error paths. Every error message names
// the offending tool and field path so the operator can fix the
// manifest without digging.

bool require_field(const json & obj, const char * name, std::string & err,
                   const std::string & ctx) {
    if (!obj.contains(name)) {
        err = ctx + ": missing required field \"" + name + "\"";
        return false;
    }
    return true;
}

bool parse_param(const json & j, const std::string & name,
                 ParamSpec & out, std::string & err,
                 const std::string & ctx) {
    if (!j.is_object()) {
        err = ctx + ".parameters.properties." + name + ": must be an object";
        return false;
    }
    if (!is_valid_param_name(name)) {
        err = ctx + ".parameters.properties." + name
            + ": invalid parameter name (must match [a-zA-Z][a-zA-Z0-9_]{0,63})";
        return false;
    }
    if (!require_field(j, "type", err,
                       ctx + ".parameters.properties." + name)) return false;
    auto t = j["type"];
    if (!t.is_string()) {
        err = ctx + ".parameters.properties." + name + ".type: must be a string";
        return false;
    }
    std::string type = t.get<std::string>();
    if (type != "string" && type != "integer" &&
        type != "number" && type != "boolean") {
        err = ctx + ".parameters.properties." + name
            + ".type: must be one of string|integer|number|boolean";
        return false;
    }
    std::string desc;
    if (j.contains("description")) {
        if (!j["description"].is_string()) {
            err = ctx + ".parameters.properties." + name
                + ".description: must be a string";
            return false;
        }
        desc = j["description"].get<std::string>();
        if (desc.size() > kMaxParamDescBytes) {
            err = ctx + ".parameters.properties." + name
                + ".description: exceeds "
                + std::to_string(kMaxParamDescBytes) + " chars";
            return false;
        }
    }
    out.name        = name;
    out.type        = std::move(type);
    out.description = std::move(desc);
    out.required    = false;   // filled in by the caller from the `required` array
    return true;
}

// Parse a single tool entry from the manifest. On success, populates
// `spec`. On failure, returns false with `err` set.
bool parse_tool_entry(const json & j, ExternalToolSpec & spec,
                      std::string & err,
                      const std::string & ctx0,
                      const std::unordered_set<std::string> & reserved) {
    if (!j.is_object()) {
        err = ctx0 + ": must be an object";
        return false;
    }
    if (!require_field(j, "name", err, ctx0))        return false;
    if (!require_field(j, "description", err, ctx0)) return false;
    if (!require_field(j, "command", err, ctx0))     return false;
    if (!require_field(j, "argv", err, ctx0))        return false;

    if (!j["name"].is_string()) {
        err = ctx0 + ".name: must be a string"; return false;
    }
    std::string name = j["name"].get<std::string>();
    if (!is_valid_tool_name(name)) {
        err = ctx0 + ".name: \"" + name
            + "\" is not a valid tool name (must match [a-zA-Z][a-zA-Z0-9_]{0,63})";
        return false;
    }
    if (kBuiltInNames.count(name) || reserved.count(name)) {
        err = ctx0 + ".name: \"" + name
            + "\" collides with a built-in or already-registered tool";
        return false;
    }
    const std::string ctx = ctx0 + " (\"" + name + "\")";

    if (!j["description"].is_string()) {
        err = ctx + ".description: must be a string"; return false;
    }
    std::string desc = j["description"].get<std::string>();
    if (desc.empty() || desc.size() > kMaxToolDescBytes) {
        err = ctx + ".description: must be 1.."
            + std::to_string(kMaxToolDescBytes) + " chars";
        return false;
    }

    if (!j["command"].is_string()) {
        err = ctx + ".command: must be a string"; return false;
    }
    std::string command = j["command"].get<std::string>();
    {
        std::string why;
        if (!is_executable_absolute_path(command, why)) {
            err = ctx + ".command: " + why;
            return false;
        }
    }

    if (!j["argv"].is_array()) {
        err = ctx + ".argv: must be an array of strings"; return false;
    }
    if (j["argv"].size() > kMaxArgvElements) {
        err = ctx + ".argv: exceeds " + std::to_string(kMaxArgvElements)
            + " elements";
        return false;
    }

    std::vector<ParamSpec> params;
    if (j.contains("parameters")) {
        const auto & p = j["parameters"];
        if (!p.is_object()) {
            err = ctx + ".parameters: must be an object"; return false;
        }
        if (p.contains("type") && p["type"] != "object") {
            err = ctx + ".parameters.type: must be \"object\""; return false;
        }
        if (p.contains("properties")) {
            const auto & props = p["properties"];
            if (!props.is_object()) {
                err = ctx + ".parameters.properties: must be an object";
                return false;
            }
            if (props.size() > kMaxParamsPerTool) {
                err = ctx + ".parameters.properties: exceeds "
                    + std::to_string(kMaxParamsPerTool) + " params";
                return false;
            }
            for (auto it = props.begin(); it != props.end(); ++it) {
                ParamSpec ps;
                if (!parse_param(it.value(), it.key(), ps, err, ctx)) {
                    return false;
                }
                params.push_back(std::move(ps));
            }
        }
        if (p.contains("required")) {
            const auto & req = p["required"];
            if (!req.is_array()) {
                err = ctx + ".parameters.required: must be a string array";
                return false;
            }
            for (const auto & rn : req) {
                if (!rn.is_string()) {
                    err = ctx + ".parameters.required: must be string array";
                    return false;
                }
                std::string nm = rn.get<std::string>();
                bool found = false;
                for (auto & p2 : params) {
                    if (p2.name == nm) { p2.required = true; found = true; break; }
                }
                if (!found) {
                    err = ctx + ".parameters.required: \""
                        + nm + "\" is not declared in properties";
                    return false;
                }
            }
        }
    }

    // argv: each element is either a literal (no '{' or '}') or
    // exactly "{name}" where `name` is a declared parameter.
    std::vector<ArgvSegment> argv_template;
    argv_template.reserve(j["argv"].size());
    for (std::size_t i = 0; i < j["argv"].size(); ++i) {
        const auto & e = j["argv"][i];
        if (!e.is_string()) {
            err = ctx + ".argv[" + std::to_string(i) + "]: must be a string";
            return false;
        }
        std::string s = e.get<std::string>();
        if (s.size() > kMaxArgElementBytes) {
            err = ctx + ".argv[" + std::to_string(i) + "]: exceeds "
                + std::to_string(kMaxArgElementBytes) + " bytes";
            return false;
        }
        std::string ph;
        if (parse_placeholder(s, ph)) {
            // Verify the named parameter is declared.
            bool found = false;
            for (const auto & p : params) {
                if (p.name == ph) { found = true; break; }
            }
            if (!found) {
                err = ctx + ".argv[" + std::to_string(i) + "]: placeholder {"
                    + ph + "} is not a declared parameter";
                return false;
            }
            argv_template.push_back({ true, std::move(ph) });
        } else if (has_no_braces(s)) {
            argv_template.push_back({ false, std::move(s) });
        } else {
            err = ctx + ".argv[" + std::to_string(i)
                + "]: braces are only allowed as full placeholders "
                  "(\"{name}\" — not embedded inside larger strings). "
                  "Split the element if you need a literal brace.";
            return false;
        }
    }

    int timeout_ms = kTimeoutDefault;
    if (j.contains("timeout_ms")) {
        if (!j["timeout_ms"].is_number_integer()) {
            err = ctx + ".timeout_ms: must be an integer"; return false;
        }
        long long t = j["timeout_ms"].get<long long>();
        if (t < kTimeoutMin || t > kTimeoutMax) {
            err = ctx + ".timeout_ms: must be in ["
                + std::to_string(kTimeoutMin) + ", "
                + std::to_string(kTimeoutMax) + "]";
            return false;
        }
        timeout_ms = (int) t;
    }

    std::size_t max_output_bytes = kOutputCapDefault;
    if (j.contains("max_output_bytes")) {
        if (!j["max_output_bytes"].is_number_integer()) {
            err = ctx + ".max_output_bytes: must be an integer"; return false;
        }
        long long b = j["max_output_bytes"].get<long long>();
        if (b < (long long) kOutputCapMin || b > (long long) kOutputCapMax) {
            err = ctx + ".max_output_bytes: must be in ["
                + std::to_string(kOutputCapMin) + ", "
                + std::to_string(kOutputCapMax) + "]";
            return false;
        }
        max_output_bytes = (std::size_t) b;
    }

    std::string cwd;
    if (j.contains("cwd")) {
        if (!j["cwd"].is_string()) {
            err = ctx + ".cwd: must be a string"; return false;
        }
        std::string raw_cwd = j["cwd"].get<std::string>();
        std::string why;
        if (!resolve_cwd(raw_cwd, cwd, why)) {
            err = ctx + ".cwd: " + why;
            return false;
        }
    } else {
        // Default cwd is $SANDBOX (the process's CWD at load time).
        std::string why;
        if (!resolve_cwd("$SANDBOX", cwd, why)) {
            err = ctx + ".cwd (default $SANDBOX): " + why;
            return false;
        }
    }

    std::vector<std::string> env_passthrough;
    if (j.contains("env_passthrough")) {
        const auto & arr = j["env_passthrough"];
        if (!arr.is_array()) {
            err = ctx + ".env_passthrough: must be a string array";
            return false;
        }
        if (arr.size() > kMaxEnvPassthrough) {
            err = ctx + ".env_passthrough: exceeds "
                + std::to_string(kMaxEnvPassthrough) + " entries";
            return false;
        }
        for (std::size_t i = 0; i < arr.size(); ++i) {
            if (!arr[i].is_string()) {
                err = ctx + ".env_passthrough[" + std::to_string(i)
                    + "]: must be a string";
                return false;
            }
            std::string vn = arr[i].get<std::string>();
            // Env var names follow the same shape as parameter names —
            // POSIX is even stricter (no lowercase recommended) but we
            // accept the broader C-identifier shape.
            if (!is_valid_param_name(vn)) {
                err = ctx + ".env_passthrough[" + std::to_string(i)
                    + "]: \"" + vn + "\" is not a valid env var name";
                return false;
            }
            env_passthrough.push_back(std::move(vn));
        }
    }

    bool merge_stderr = true;
    if (j.contains("stderr")) {
        if (!j["stderr"].is_string()) {
            err = ctx + ".stderr: must be \"merge\" or \"discard\"";
            return false;
        }
        std::string m = j["stderr"].get<std::string>();
        if (m == "merge")        merge_stderr = true;
        else if (m == "discard") merge_stderr = false;
        else {
            err = ctx + ".stderr: must be \"merge\" or \"discard\"";
            return false;
        }
    }

    bool nonzero_is_error = true;
    if (j.contains("treat_nonzero_exit_as_error")) {
        if (!j["treat_nonzero_exit_as_error"].is_boolean()) {
            err = ctx + ".treat_nonzero_exit_as_error: must be a boolean";
            return false;
        }
        nonzero_is_error = j["treat_nonzero_exit_as_error"].get<bool>();
    }

    spec.name             = std::move(name);
    spec.description      = std::move(desc);
    spec.command          = std::move(command);
    spec.argv_template    = std::move(argv_template);
    spec.params           = std::move(params);
    spec.timeout_ms       = timeout_ms;
    spec.max_output_bytes = max_output_bytes;
    spec.cwd              = std::move(cwd);
    spec.env_passthrough  = std::move(env_passthrough);
    spec.merge_stderr     = merge_stderr;
    spec.nonzero_is_error = nonzero_is_error;
    return true;
}

// Construct the handler closure that the agent will dispatch on every
// model call. Captures a shared_ptr to the validated spec.
ToolHandler make_handler(std::shared_ptr<const ExternalToolSpec> spec) {
    return [spec](const ToolCall & call) -> ToolResult {
        std::vector<std::string> values;
        std::string err;
        if (!validate_call_arguments(call.arguments_json, spec->params,
                                     values, err)) {
            return ToolResult::error(err);
        }

        // Build argv. Element 0 is the basename of the command (so
        // ps shows something nicer than the absolute path).
        std::vector<std::string> argv;
        argv.reserve(spec->argv_template.size() + 1);
        std::string argv0 = spec->command;
        if (auto pos = argv0.find_last_of('/'); pos != std::string::npos) {
            argv0.erase(0, pos + 1);
        }
        argv.push_back(std::move(argv0));
        for (const auto & seg : spec->argv_template) {
            if (!seg.is_placeholder) {
                argv.push_back(seg.text);
                continue;
            }
            // Look up parameter by name; values is parallel to spec->params.
            std::size_t idx = 0;
            bool found = false;
            for (; idx < spec->params.size(); ++idx) {
                if (spec->params[idx].name == seg.text) { found = true; break; }
            }
            if (!found) {
                // Should be impossible — we validated this at load. Defence in depth.
                return ToolResult::error(
                    "internal error: placeholder " + seg.text + " not in spec");
            }
            // For non-required missing args we substitute empty string —
            // operators who want different behaviour should mark the
            // parameter required.
            argv.push_back(values[idx]);
        }

        // Build envp from the allowlist. Each entry is "KEY=VALUE";
        // missing vars are skipped silently (the operator opted in
        // but the var doesn't exist in this process). Values longer
        // than kMaxArgElementBytes are also skipped — a hostile env
        // (e.g. HOME set to a multi-megabyte string) would otherwise
        // push the execve table toward the kernel ARG_MAX ceiling and
        // make every spawn slower for no legitimate reason.
        std::vector<std::string> envp;
        envp.reserve(spec->env_passthrough.size());
        for (const auto & vn : spec->env_passthrough) {
            const char * v = ::getenv(vn.c_str());
            if (v == nullptr) continue;
            const std::size_t vlen = std::strlen(v);
            if (vlen > kMaxArgElementBytes) continue;
            std::string entry;
            entry.reserve(vn.size() + 1 + vlen);
            entry.append(vn).append("=").append(v, vlen);
            envp.push_back(std::move(entry));
        }

        RunResult rr = run_external_command(
            spec->command, argv, envp, spec->cwd,
            spec->timeout_ms, spec->max_output_bytes, spec->merge_stderr);

        // Build the response message the model will read.
        std::ostringstream oss;
        if (rr.timed_out) {
            oss << "exit=-1  [killed: timeout after "
                << spec->timeout_ms << "ms]\n";
        } else if (WIFEXITED(rr.exit_status)) {
            oss << "exit=" << WEXITSTATUS(rr.exit_status) << "\n";
        } else if (WIFSIGNALED(rr.exit_status)) {
            oss << "exit=signal:" << WTERMSIG(rr.exit_status) << "\n";
        } else {
            oss << "exit=?\n";
        }
        std::string body = std::move(rr.output);
        if (rr.truncated) {
            body += "\n[truncated at "
                  + std::to_string(spec->max_output_bytes) + " bytes]\n";
        }
        std::string content = oss.str() + body;

        bool is_error =
            rr.timed_out
            || (spec->nonzero_is_error
                && WIFEXITED(rr.exit_status)
                && WEXITSTATUS(rr.exit_status) != 0)
            || (WIFSIGNALED(rr.exit_status));

        return is_error ? ToolResult::error(std::move(content))
                        : ToolResult::ok(std::move(content));
    };
}

// Slurp a file with a hard size cap.
//
// stat()-first so we reject directories, FIFOs, devices, and sockets
// up front with a precise error message. Without that pre-check
// `ifstream` + `seekg(end)` on /dev/zero / a named pipe / a directory
// produces seekable-but-meaningless tellg() values, and the cap
// merely limits the damage instead of preventing it.
bool slurp(const std::string & path, std::string & out, std::string & err) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        err = "manifest stat failed: " + path
            + ": " + std::strerror(errno);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        err = "manifest is not a regular file: " + path;
        return false;
    }
    if ((std::size_t) st.st_size > kMaxManifestBytes) {
        err = "manifest exceeds " + std::to_string(kMaxManifestBytes)
            + " bytes";
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open: " + path;
        return false;
    }
    out.assign((std::size_t) st.st_size, '\0');
    f.read(out.data(), st.st_size);
    out.resize((std::size_t) f.gcount());
    return true;
}

// ---------------------------------------------------------------------------
// Sanity-check pass — security warnings, not errors.
// ---------------------------------------------------------------------------
//
// We run this once per successfully-parsed spec. The output is a
// list of human-readable warning strings (one per finding) prefixed
// with `<manifest_path>: tool "<name>": …` so the operator can grep
// the startup log and find the offending entry quickly.
//
// Findings are classes of decision the operator probably wants to
// make consciously rather than by accident:
//
//   1. Shell wrapper. The whole point of the manifest subsystem is
//      structural absence of a shell parser. Wrapping `/bin/sh -c
//      "{cmd}"` reintroduces the entire shell-injection surface;
//      it's functionally equivalent to `--allow-bash`. Sometimes
//      this is what the operator wants, sometimes it's a mistake.
//      We flag it either way.
//
//   2. Dynamic-linker env passthrough. `LD_PRELOAD`,
//      `LD_LIBRARY_PATH`, `LD_AUDIT`, `DYLD_INSERT_LIBRARIES`,
//      `DYLD_LIBRARY_PATH` let the model influence which shared
//      objects get loaded into the subprocess. Almost always wrong.
//
//   3. World-writable command binary. If the wrapped binary's mode
//      bits include S_IWOTH, anyone with shell on this box can
//      replace it — and the agent will run their replacement next
//      time the model calls the tool. Acceptable on a single-user
//      dev box; very much not in production.
//
//   4. World-writable manifest file. Same reasoning, one level up:
//      anyone with write access to the manifest can declare new
//      tools that survive the next restart.
//
// `manifest_mode` is the st_mode reported by stat() on the
// manifest file — the dir loader passes it in; the single-file
// loader stat()s the file itself and passes its mode.
std::vector<std::string> audit_spec(const ExternalToolSpec & spec,
                                    const std::string &      manifest_path,
                                    mode_t                   manifest_mode) {
    std::vector<std::string> out;
    auto pfx = [&](const std::string & body) {
        return manifest_path + ": tool \"" + spec.name + "\": " + body;
    };

    // 1. Shell wrapper detection — command basename in known-shell set.
    {
        std::string base = spec.command;
        if (auto pos = base.find_last_of('/'); pos != std::string::npos) {
            base.erase(0, pos + 1);
        }
        const bool is_shell =
            base == "sh"   || base == "bash" || base == "dash" ||
            base == "zsh"  || base == "ksh"  || base == "ash"  ||
            base == "fish";
        if (is_shell) {
            // If argv contains "-c" followed by a placeholder, the
            // model directly controls a full shell command line.
            bool dash_c_placeholder = false;
            for (std::size_t i = 0; i + 1 < spec.argv_template.size(); ++i) {
                const auto & a = spec.argv_template[i];
                const auto & b = spec.argv_template[i + 1];
                if (!a.is_placeholder && a.text == "-c" && b.is_placeholder) {
                    dash_c_placeholder = true;
                    break;
                }
            }
            if (dash_c_placeholder) {
                out.push_back(pfx(
                    "wraps a shell with -c {placeholder}; this is "
                    "functionally equivalent to --allow-bash. "
                    "Prefer declaring focused tools that wrap the "
                    "specific binaries you need."));
            } else {
                out.push_back(pfx(
                    "command is a shell binary (" + base + "); "
                    "the structural \"no shell\" guarantee of the "
                    "manifest subsystem doesn't apply if argv reaches "
                    "shell parsing."));
            }
        }
    }

    // 2. Dynamic-linker env passthrough.
    {
        static const std::vector<std::string> kDangerEnv = {
            "LD_PRELOAD", "LD_LIBRARY_PATH", "LD_AUDIT",
            "DYLD_INSERT_LIBRARIES", "DYLD_LIBRARY_PATH",
        };
        for (const auto & vn : spec.env_passthrough) {
            for (const auto & danger : kDangerEnv) {
                if (vn == danger) {
                    out.push_back(pfx(
                        "env_passthrough includes \"" + vn + "\" which "
                        "lets the model influence dynamic linking in the "
                        "subprocess. Almost never the right thing; remove "
                        "it unless you have a specific, documented reason."));
                }
            }
        }
    }

    // 3. World-writable command binary.
    {
        struct stat st{};
        if (::stat(spec.command.c_str(), &st) == 0
                && (st.st_mode & S_IWOTH)) {
            out.push_back(pfx(
                "command \"" + spec.command + "\" is world-writable "
                "(mode includes S_IWOTH). Anyone with shell on this "
                "host can replace the binary; the agent will run the "
                "replacement on next call. Fix: chmod o-w."));
        }
    }

    // 4. World-writable manifest file.
    if (manifest_mode & S_IWOTH) {
        out.push_back(pfx(
            "manifest file \"" + manifest_path + "\" is world-writable. "
            "Anyone with write access can register additional tools "
            "that survive the next restart. Fix: chmod o-w."));
    }

    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------
ExternalToolsLoad load_external_tools_from_json(
    const std::string &              json_path,
    const std::vector<std::string> & reserved_names) {
    ExternalToolsLoad out;

    if (json_path.empty()) {
        out.error = "external tools manifest path is empty";
        return out;
    }

    // Capture manifest mode so the sanity-check pass can warn on
    // world-writable manifest files. slurp() already stat()s the
    // file; we re-stat here to keep the data accessible without
    // changing slurp's signature.
    mode_t manifest_mode = 0;
    {
        struct stat st{};
        if (::stat(json_path.c_str(), &st) == 0) {
            manifest_mode = st.st_mode;
        }
    }

    std::string raw;
    {
        std::string err;
        if (!slurp(json_path, raw, err)) {
            out.error = err;
            return out;
        }
    }

    json doc;
    try {
        doc = json::parse(raw);
    } catch (const std::exception & e) {
        out.error = std::string("manifest is not valid JSON: ") + e.what();
        return out;
    }
    if (!doc.is_object()) {
        out.error = "manifest root must be a JSON object";
        return out;
    }

    if (doc.contains("version")) {
        if (!doc["version"].is_number_integer()) {
            out.error = "manifest .version: must be an integer";
            return out;
        }
        long long v = doc["version"].get<long long>();
        if (v != 1) {
            out.error = "manifest .version: only version 1 is supported "
                        "(got " + std::to_string(v) + ")";
            return out;
        }
    }
    if (!doc.contains("tools") || !doc["tools"].is_array()) {
        out.error = "manifest .tools: must be an array";
        return out;
    }
    if (doc["tools"].size() > kMaxToolsPerManifest) {
        out.error = "manifest .tools: exceeds "
                  + std::to_string(kMaxToolsPerManifest) + " entries";
        return out;
    }

    std::unordered_set<std::string> reserved(reserved_names.begin(),
                                             reserved_names.end());
    std::unordered_set<std::string> seen_names;
    out.tools.reserve(doc["tools"].size());
    for (std::size_t i = 0; i < doc["tools"].size(); ++i) {
        const std::string ctx = ".tools[" + std::to_string(i) + "]";
        ExternalToolSpec spec;
        if (!parse_tool_entry(doc["tools"][i], spec, out.error, ctx, reserved)) {
            out.tools.clear();
            return out;
        }
        if (!seen_names.insert(spec.name).second) {
            out.error = ctx + ": duplicate tool name \"" + spec.name + "\"";
            out.tools.clear();
            out.warnings.clear();
            return out;
        }
        // Reserve this name so subsequent entries collide cleanly.
        reserved.insert(spec.name);

        // Sanity-check audit (security warnings, not errors). The
        // tool still loads; the operator gets to decide whether to
        // accept the surface they exposed.
        for (auto & w : audit_spec(spec, json_path, manifest_mode)) {
            out.warnings.push_back(std::move(w));
        }

        Tool t;
        t.name            = spec.name;
        t.description     = spec.description;
        t.parameters_json = build_parameters_schema_json(spec.params);
        t.handler         = make_handler(
            std::make_shared<const ExternalToolSpec>(std::move(spec)));
        out.tools.push_back(std::move(t));
    }

    return out;
}

ExternalToolsDirLoad load_external_tools_from_dir(
    const std::string &              dir,
    const std::vector<std::string> & reserved_names) {
    ExternalToolsDirLoad out;
    namespace fs = std::filesystem;

    // Empty path is a programmer error, not a deploy state we
    // should silently swallow.
    if (dir.empty()) {
        out.errors.push_back("external-tools dir path is empty");
        return out;
    }

    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) {
        out.errors.push_back(
            "external-tools dir does not exist: " + dir);
        return out;
    }
    if (!fs::is_directory(dir, ec) || ec) {
        out.errors.push_back(
            "external-tools path is not a directory: " + dir);
        return out;
    }

    // Two passes through the directory: first collect filenames
    // matching the EASYAI-*.tools pattern, then load them in
    // sorted order. Sorting makes load order deterministic across
    // machines and reboots, so duplicate-name resolution is stable
    // (the alphabetically-earlier file wins).
    std::vector<fs::path> matched;
    for (const auto & entry : fs::directory_iterator(dir, ec)) {
        if (ec) {
            out.errors.push_back(
                "external-tools dir iteration failed: " + ec.message());
            return out;
        }
        // Skip subdirectories silently — operators use them for
        // organisation (a `disabled/` folder, an `archive/`).
        if (!entry.is_regular_file()) continue;

        const std::string fname = entry.path().filename().string();
        // Pattern: starts with "EASYAI-", ends with ".tools",
        // at least one char in between. Anything else is silently
        // skipped (operators drop READMEs, sample files with
        // `.disabled` extensions, etc. in this directory).
        if (fname.size() < kManifestMinFilename
            || fname.compare(0, kManifestPrefixLen, kManifestPrefix) != 0
            || fname.compare(fname.size() - kManifestSuffixLen,
                             kManifestSuffixLen, kManifestSuffix) != 0) {
            out.skipped_files.push_back(entry.path().string());
            continue;
        }
        matched.push_back(entry.path());
    }

    std::sort(matched.begin(), matched.end());

    // An empty result set is a normal deploy state: the dir exists,
    // the operator just hasn't dropped any tool packs in yet. Do
    // not surface this as an error or warning. Quiet success.
    if (matched.empty()) {
        return out;
    }

    // Load each file independently. Per-file fault isolation: a
    // syntax error or schema violation in one file does not
    // prevent the others from loading. The reserved-names set
    // grows as we go, so a name from `EASYAI-aaa.tools` blocks
    // the same name in `EASYAI-zzz.tools` (load error localised
    // to the second file; the first file's tools are still live).
    std::vector<std::string> reserved = reserved_names;
    for (const auto & p : matched) {
        const std::string path_s = p.string();
        ExternalToolsLoad one = load_external_tools_from_json(path_s, reserved);
        if (!one.error.empty()) {
            out.errors.push_back(path_s + ": " + one.error);
            continue;
        }
        for (auto & t : one.tools) {
            reserved.push_back(t.name);
            out.tools.push_back(std::move(t));
        }
        for (auto & w : one.warnings) {
            out.warnings.push_back(std::move(w));
        }
        out.loaded_files.push_back(path_s);
    }

    return out;
}

}  // namespace easyai
