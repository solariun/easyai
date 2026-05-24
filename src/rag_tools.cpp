// src/rag_tools.cpp — implementation of the RAG persistent registry.
//
// On-disk format and constraints are documented in the public
// header. This file enforces those contracts plus the four tool
// handlers.
//
// Design choices worth calling out:
//
//   * The format is INTENTIONALLY trivial. One header line
//     (`keywords: a, b, c`), one blank line, then a free-form body.
//     The operator can `cat` an entry, edit it with `vim`, drop
//     a hand-authored note in the dir — no JSON parser, no
//     escape rules, no "did I get the structure right?".
//
//   * The keyword index lives in process memory, lazily built on
//     first use. Each entry's metadata (keywords + mtime) is small,
//     so even 10 000 entries is well under a megabyte resident.
//
//   * Saves are atomic via tempfile + rename(2). Readers either
//     see the old content or the new content, never a partial
//     write.
//
//   * Listing/searching never reads the body from disk — only
//     the title (= filename) and the cached keywords from the index.
//     `rag_load` does the single read for the body when the
//     model picks an entry.
//
//   * The title is the filesystem name. Validating it with a
//     strict regex (`^[A-Za-z0-9._+-]+$`, ≤ 64 bytes) is what
//     closes the path-traversal door — there is no way for the
//     model to write `..`, slashes, NUL bytes, etc.

#include "easyai/rag_tools.hpp"
#include "easyai/tool.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace easyai::tools {

namespace {

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ---------------------------------------------------------------------------
// Constants — every limit named, every cap explained.
// ---------------------------------------------------------------------------

// Title and keyword identifier shape. The title is the filesystem name
// of the entry's file, so banning slashes / dots / spaces also
// closes path traversal as a side effect.
constexpr std::size_t kMaxTitleBytes      = 64;
// Roomy caps for keywords: small models naturally extract 10-20
// terms when prompted, and longer compound keywords (e.g.
// "bitnet_ternary_quantization") read better than two cramped halves.
// On-disk impact is negligible — one extra header line per memory.
constexpr std::size_t kMaxKeywordBytes        = 64;
constexpr std::size_t kMaxKeywordsPerEntry    = 24;

// Content cap. 256 KiB is generous for "a piece of knowledge worth
// remembering" — long enough to fit a code recipe + commentary, short
// enough that 1 000 entries fit comfortably on disk and in memory.
constexpr std::size_t kMaxContentBytes    = 256u * 1024u;

// rag_load can fan out to up to 20 entries per call — matching the
// search result cap so the model can load every hit in one round
// trip when it needs broad context.
constexpr std::size_t kMaxLoadAtOnce      = 20;

// Search result cap. The model gets a list of (title, keywords,
// preview) and picks titles to rag_load — same cap so a full page
// of search results can be loaded at once.
constexpr std::size_t kSearchResultsMax   = 20;
constexpr std::size_t kSearchResultsDflt  = 10;

// Bytes from the body we render in a search-result preview. Long
// enough for the model to recognise "yes, that's the entry I want",
// short enough to keep the prompt slim.
constexpr std::size_t kSearchPreviewBytes = 240;

// rag_list cap. Browsing-only; no body read.
constexpr std::size_t kListResultsMax     = 200;
constexpr std::size_t kListResultsDflt    = 50;

// rag_keywords cap. Vocabulary overview — the model uses this to
// see which keywords it's already been using before saving a new
// entry. A typical RAG converges to a few dozen stable keywords;
// the higher cap is for power users with deep vocabularies.
constexpr std::size_t kKeywordsResultsMax  = 500;
constexpr std::size_t kKeywordsResultsDflt = 200;

// On-disk file extension. `.md` keeps entries human-readable in
// any text editor, lets the operator `cat` / `vim` / `grep`, and
// renders nicely as markdown when the body uses it.
constexpr char        kEntrySuffix[]      = ".md";
constexpr std::size_t kEntrySuffixLen     = sizeof(kEntrySuffix) - 1;

// Title prefix that marks a memory as IMMUTABLE. Memories with a
// title starting `fix-easyai-` cannot be overwritten by rag_save and
// cannot be removed by rag_delete — they survive every session until
// the operator deletes the file from disk by hand. Used to seed the
// agent with system designs, hard rules, domain knowledge that must
// not drift. The prefix is part of the title (so it shows in every
// search/list/load result) and lives in the keyword namespace
// `[A-Za-z0-9._+-]` so existing validation still applies.
constexpr char        kFixedTitlePrefix[]    = "fix-easyai-";
constexpr std::size_t kFixedTitlePrefixLen   = sizeof(kFixedTitlePrefix) - 1;

bool title_is_fixed(const std::string & title) {
    return title.size() > kFixedTitlePrefixLen
        && title.compare(0, kFixedTitlePrefixLen, kFixedTitlePrefix) == 0;
}

// Render a unix timestamp as "YYYY-MM-DD HH:MM:SS" in local time. Used
// in search/list/load output so the model can reason about recency
// without doing the math on a raw integer. Local time matches what the
// operator sees in `ls -l`. Returns "?" on a zero/negative epoch (e.g.
// last_write_time failed) so output stays parseable either way.
std::string format_local_time(std::int64_t unix_seconds) {
    if (unix_seconds <= 0) return "?";
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Validators — pure, no I/O, no allocation past the input.
// ---------------------------------------------------------------------------
//
// Allowed character set for keywords AND titles:
//   [a-zA-Z0-9._+-]
//
// Why each non-alnum is allowed (or not):
//
//   `-` `_`  classic word separators
//   `.`      versions ("v1.0"), namespaces ("project.easyai"),
//            file references ("nginx.conf"). REQUIRES extra title
//            validation (see is_valid_title) so `.` / `..` /
//            leading-dot can't sneak through to the filesystem.
//   `+`      niche but real: "c++", "git+ssh", "a+b" recipes.
//   space    NO — filesystem ambiguity, shell-quoting trap.
//   `/` `\`  NO — path-component separators on every OS.
//   `:`      NO — reserved on Windows + ADS-style abuse.
//   anything else (quotes, $, `, etc.): NO — shell / display traps.
//
// Keywords use is_valid_id directly. Titles use is_valid_title,
// which adds filesystem-specific rejections on top.
bool is_valid_id(const std::string & s, std::size_t max_len) {
    if (s.empty() || s.size() > max_len) return false;
    for (char c : s) {
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '+';
        if (!ok) return false;
    }
    return true;
}

// is_valid_title — id rules PLUS filesystem-safety constraints:
//
//   * exact "." and ".." are rejected (POSIX path-traversal aliases —
//     even though our regex already blocks slashes, allowing ".."
//     as a title means a hand-edited dir with `..md` is ambiguous
//     and confuses operators).
//   * leading "." is rejected — dotfiles are easy to miss in `ls`,
//     and the agent's persistent memory shouldn't be hidden by
//     accident.
//   * the title must contain at least one alnum — purely-symbol
//     titles like "...", "+--", "_._" are valid by character set
//     but useless and confusing on disk.
//
// Returns false in all those cases; passes anything is_valid_id
// would pass that doesn't trip the extras.
bool is_valid_title(const std::string & s, std::size_t max_len) {
    if (!is_valid_id(s, max_len)) return false;
    if (s == "." || s == "..")    return false;
    if (s.front() == '.')         return false;
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            return true;   // contains alnum, all good
        }
    }
    return false;          // no alnum → reject
}

// normalize_id — map a free-form string onto is_valid_id's charset.
// Goal: let the model say "neural network" or "C++17 features" and
// have us store a sane on-disk identifier without bouncing the call.
//
// Rules (deterministic, no allocation surprises):
//   * alnum / `-` / `_` / `.` / `+` are kept verbatim.
//   * whitespace and path-component separators (` `, `\t`, `/`, `\`,
//     `:`) collapse to a single `_` (runs of separators become one).
//   * every other byte (quotes, parens, `?`, `!`, multi-byte UTF-8
//     leading bytes, etc.) is dropped — keeping them as separators
//     would explode common natural-language input into nonsense.
//   * trailing `_` is trimmed (a string ending in punctuation that
//     normalised to `_` shouldn't keep the empty-trailing).
//   * the result is truncated to max_len, then re-trimmed.
//
// Returns the normalised string. Empty result is possible (caller
// must check) when the input was pure noise (e.g. "???" → "").
std::string normalize_id(const std::string & s, std::size_t max_len) {
    std::string out;
    out.reserve(s.size());
    auto is_kept = [](char c) {
        return (c >= 'a' && c <= 'z') ||
               (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') ||
               c == '-' || c == '_' || c == '.' || c == '+';
    };
    auto is_separator = [](char c) {
        return c == ' '  || c == '\t' || c == '\r' || c == '\n'
            || c == '/'  || c == '\\' || c == ':';
    };
    for (char c : s) {
        if (is_kept(c)) {
            out += c;
        } else if (is_separator(c)) {
            if (!out.empty() && out.back() != '_') out += '_';
        }
        // else: drop silently
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.size() > max_len) {
        out.resize(max_len);
        while (!out.empty() && out.back() == '_') out.pop_back();
    }
    return out;
}

// normalize_title — normalize_id + extra FS-safety constraints:
// strip leading dots (dotfiles), reject pure-symbol results, reject
// the path aliases "." and "..". Empty return means "could not
// produce a usable title" — caller emits an error to the model.
std::string normalize_title(const std::string & s, std::size_t max_len) {
    std::string n = normalize_id(s, max_len);
    while (!n.empty() && n.front() == '.') n.erase(n.begin());
    if (n == "." || n == "..") return {};
    bool has_alnum = false;
    for (char c : n) {
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            has_alnum = true;
            break;
        }
    }
    return has_alnum ? n : std::string();
}

// ---------------------------------------------------------------------------
// Format helpers — read/write the tiny "keywords: a, b, c\n\n<body>" shape.
// ---------------------------------------------------------------------------

// Trim ASCII whitespace (space/tab) from both ends. Newlines / CR
// are NOT stripped here — callers handle those on a line basis.
std::string trim_inline(std::string s) {
    auto issp = [](unsigned char c) { return c == ' ' || c == '\t'; };
    while (!s.empty() && issp(s.front())) s.erase(s.begin());
    while (!s.empty() && issp(s.back()))  s.pop_back();
    return s;
}

// Split `value` on commas, trim each piece, drop empties. Caller
// validates each keyword against `is_valid_id` separately.
std::vector<std::string> split_keywords(const std::string & value) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : value) {
        if (c == ',') {
            cur = trim_inline(cur);
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    cur = trim_inline(cur);
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

// Read a whole file with a hard size cap so a malicious / corrupt
// entry doesn't pull the process into oversized allocations.
bool slurp_capped(const fs::path & p, std::size_t max_bytes,
                  std::string & out, std::string & err) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { err = "cannot open: " + p.string(); return false; }
    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    if (sz < 0) { err = "tellg failed: " + p.string(); return false; }
    if ((std::size_t) sz > max_bytes) {
        err = "entry exceeds " + std::to_string(max_bytes) + " bytes";
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.assign((std::size_t) sz, '\0');
    f.read(out.data(), sz);
    out.resize((std::size_t) f.gcount());
    return true;
}

// Parse the on-disk text into keywords + body. The grammar is:
//
//     <header-line>*
//     <blank-line>
//     <body>
//
// where each header is `key: value` (we currently recognise `keywords`).
// A file with NO blank line is treated as body-only (no header).
//
// Headers ARE optional. A file like:
//
//     just a free-form note dropped here by the operator
//
// is loaded as `keywords = []`, `body = "<entire content>"`.
// `rag_search` won't find it (no keywords) but `rag_list` will.
struct ParsedEntry {
    std::vector<std::string> keywords;
    std::string              body;
};

ParsedEntry parse_entry(const std::string & raw) {
    ParsedEntry out;
    if (raw.empty()) return out;

    // Look at the first line. If it doesn't look like a header
    // (`<key>: ...`), treat the entire file as body.
    auto looks_like_header = [](const std::string & line) {
        const auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0) return false;
        for (std::size_t i = 0; i < colon; ++i) {
            const unsigned char c = static_cast<unsigned char>(line[i]);
            if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
        }
        return true;
    };

    std::size_t pos = 0;
    auto next_line = [&](std::string & line) -> bool {
        if (pos >= raw.size()) return false;
        std::size_t end = raw.find('\n', pos);
        if (end == std::string::npos) {
            line.assign(raw, pos, raw.size() - pos);
            pos = raw.size();
        } else {
            line.assign(raw, pos, end - pos);
            pos = end + 1;
        }
        // Strip CR for CRLF inputs.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    };

    std::string first;
    std::size_t first_line_pos = pos;
    if (!next_line(first)) return out;
    if (!looks_like_header(first)) {
        // Body-only file. Restore position and consume everything.
        out.body.assign(raw, first_line_pos, raw.size() - first_line_pos);
        return out;
    }

    // Process the header lines until a blank line or non-header.
    auto try_consume_header = [&](const std::string & line) {
        if (line.empty()) return;
        if (!looks_like_header(line)) return;
        const auto colon = line.find(':');
        std::string key = line.substr(0, colon);
        std::string val = trim_inline(line.substr(colon + 1));
        // Lowercase the key for case-insensitive matching.
        for (auto & c : key) c = (char) std::tolower((unsigned char) c);
        if (key == "keywords") {
            out.keywords = split_keywords(val);
        }
        // Future header keys land here. Unknown keys are silently
        // ignored — a hand-edit can leave stray fields without
        // breaking the load.
    };

    try_consume_header(first);

    std::string line;
    bool body_started = false;
    while (next_line(line)) {
        if (!body_started) {
            if (line.empty()) { body_started = true; continue; }
            if (looks_like_header(line)) { try_consume_header(line); continue; }
            // Non-header, non-blank → start body here, keeping this line.
            out.body = line;
            if (pos < raw.size()) {
                out.body += '\n';
                out.body.append(raw, pos, raw.size() - pos);
            }
            return out;
        }
        // body_started — append.
        if (!out.body.empty()) out.body += '\n';
        out.body += line;
    }
    return out;
}

// Render the on-disk text from keywords + body. Keeps the trailing
// newline so `cat` doesn't print without one.
std::string render_entry(const std::vector<std::string> & keywords,
                         const std::string & body) {
    std::string out;
    out.reserve(body.size() + 64);
    out += "keywords:";
    for (std::size_t i = 0; i < keywords.size(); ++i) {
        out += (i == 0 ? " " : ", ");
        out += keywords[i];
    }
    out += "\n\n";
    out += body;
    if (out.empty() || out.back() != '\n') out += '\n';
    return out;
}

// Filesystem mtime as Unix seconds. fs::file_time_type's epoch isn't
// guaranteed by the standard, so we shift it onto system_clock via the
// C++17 idiom (subtract the file_clock "now", add the system_clock
// "now"). Returns 0 when the path can't be stat'd — every caller pairs
// this with other index fields, so a "stat failed" zero keeps the
// in-memory index self-consistent without forcing an error channel
// through three otherwise-different read sites.
inline std::int64_t file_mtime_unix(const fs::path & p) {
    std::error_code ec;
    const auto ftime = fs::last_write_time(p, ec);
    if (ec) return 0;
    const auto sctp = std::chrono::time_point_cast<
        std::chrono::system_clock::duration>(
        ftime - decltype(ftime)::clock::now()
              + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(
        sctp.time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// EntryMeta — what the in-memory index stores per title.
// ---------------------------------------------------------------------------
struct EntryMeta {
    std::vector<std::string> keywords;
    std::int64_t             modified_unix = 0;
    std::size_t              content_bytes = 0;
};

// ---------------------------------------------------------------------------
// RagStore — the shared state for all seven tools.
// ---------------------------------------------------------------------------
//
// `mu` is a shared_mutex so high-concurrency MCP traffic (rag_search /
// rag_list / rag_load / rag_keywords reads from many in-flight requests)
// doesn't serialise on the write path (rag_save, rag_delete). Readers
// take std::shared_lock; writers take std::unique_lock.
//
// The index is populated EAGERLY by `make_rag_tools()` under a unique
// lock so every subsequent reader can rely on `index_loaded == true`
// without the upgrade dance. Single-process is the supported model
// (header §"Concurrency"), so we never re-scan the directory after
// startup; save/delete keep the index in sync as the model edits.
struct RagStore {
    fs::path                            root;
    std::shared_mutex                   mu;
    std::map<std::string, EntryMeta>    index;
    bool                                index_loaded = false;

    explicit RagStore(std::string r) {
        if (!r.empty()) {
            std::error_code ec;
            root = fs::absolute(r, ec);
            if (ec) root = fs::path(std::move(r));
        }
    }

    bool root_set() const { return !root.empty(); }

    // Ensure root exists and is a directory. Called from anywhere
    // that wants to write; reads tolerate a missing dir (returns
    // empty list).
    bool ensure_dir(std::string & err) {
        if (!root_set()) { err = "RAG root path is empty"; return false; }
        std::error_code ec;
        if (fs::exists(root, ec)) {
            if (!fs::is_directory(root, ec)) {
                err = "RAG root is not a directory: " + root.string();
                return false;
            }
            return true;
        }
        fs::create_directories(root, ec);
        if (ec) {
            err = "create RAG dir failed: " + ec.message();
            return false;
        }
        return true;
    }

    // Walk the directory, parse each entry's header for keywords, build
    // the in-memory index. Files that don't match the suffix or whose
    // title is invalid are silently skipped.
    void load_index_locked() {
        if (index_loaded) return;
        index.clear();
        index_loaded = true;
        if (!root_set()) return;
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
            return;
        }
        for (const auto & e : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!e.is_regular_file()) continue;
            const auto fname = e.path().filename().string();
            if (fname.size() <= kEntrySuffixLen) continue;
            if (fname.compare(fname.size() - kEntrySuffixLen,
                              kEntrySuffixLen, kEntrySuffix) != 0) {
                continue;
            }
            const std::string title =
                fname.substr(0, fname.size() - kEntrySuffixLen);
            if (!is_valid_title(title, kMaxTitleBytes)) continue;

            std::string raw, err;
            if (!slurp_capped(e.path(),
                              kMaxContentBytes + 4096 /* header room */,
                              raw, err)) {
                continue;
            }
            ParsedEntry pe = parse_entry(raw);

            EntryMeta m;
            for (auto & t : pe.keywords) {
                if (is_valid_id(t, kMaxKeywordBytes)) {
                    m.keywords.push_back(std::move(t));
                    if (m.keywords.size() == kMaxKeywordsPerEntry) break;
                }
            }
            m.content_bytes = pe.body.size();
            m.modified_unix = file_mtime_unix(e.path());
            index[title] = std::move(m);
        }
    }

    // Atomic write: tempfile + rename(2). Updates the in-memory
    // index on success.
    bool save_locked(const std::string &              title,
                     const std::vector<std::string> & keywords,
                     const std::string &              content,
                     std::string &                    err) {
        if (!ensure_dir(err)) return false;
        const auto target = root / (title + kEntrySuffix);
        const auto tmp    = root / (title + std::string(kEntrySuffix)
                                          + ".tmp." + std::to_string(::getpid()));

        const std::string text = render_entry(keywords, content);

        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                err = "cannot open RAG tempfile in " + root.string();
                return false;
            }
            out.write(text.data(), (std::streamsize) text.size());
            out.flush();
            if (!out) {
                err = "write to RAG tempfile failed";
                std::error_code ec;
                fs::remove(tmp, ec);
                return false;
            }
        }
        // Tighten permissions BEFORE rename so the new entry is never
        // visible to other users on disk, even briefly. The process
        // umask defaults to 022 on most systems → files end up 0644
        // (world-readable). RAG entries can carry sensitive content
        // the model was told to memorise; lock to owner-only.
        {
            std::error_code ec;
            fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write,
                            fs::perm_options::replace, ec);
            (void) ec;   // best-effort; rename below is the durable step
        }
        std::error_code ec;
        fs::rename(tmp, target, ec);
        if (ec) {
            err = "atomic rename failed: " + ec.message();
            fs::remove(tmp, ec);
            return false;
        }

        EntryMeta m;
        m.keywords = keywords;
        // Read mtime back from the file we just wrote so the index
        // matches what the FS will report.
        m.modified_unix = file_mtime_unix(target);
        m.content_bytes = content.size();
        index[title] = std::move(m);
        return true;
    }

    // Read the full entry off disk. Returns the parsed body in
    // `body_out`. Caller doesn't need to hold the mutex — file
    // content is written via atomic rename, so reads always see
    // a complete file.
    bool load_one(const std::string & title,
                  std::vector<std::string> & keywords_out,
                  std::string & body_out,
                  std::int64_t & modified_unix_out,
                  std::string & err) {
        if (!root_set()) { err = "RAG root path is empty"; return false; }
        const auto target = root / (title + kEntrySuffix);
        std::error_code ec;
        if (!fs::exists(target, ec)) {
            err = "no RAG entry titled \"" + title + "\"";
            return false;
        }
        std::string raw;
        if (!slurp_capped(target, kMaxContentBytes + 4096, raw, err)) {
            return false;
        }
        ParsedEntry pe = parse_entry(raw);
        keywords_out.clear();
        for (auto & t : pe.keywords) {
            if (is_valid_id(t, kMaxKeywordBytes)) keywords_out.push_back(std::move(t));
        }
        body_out = std::move(pe.body);
        modified_unix_out = file_mtime_unix(target);
        return true;
    }

    // Delete an entry. Idempotent: deleting a non-existent title
    // returns true with `existed = false` so the caller can decide
    // whether to surface "no such entry" or stay quiet. Removes the
    // index entry too.
    bool delete_locked(const std::string & title, bool & existed,
                       std::string & err) {
        existed = false;
        if (!root_set()) { err = "RAG root path is empty"; return false; }
        const auto target = root / (title + kEntrySuffix);
        std::error_code ec;
        if (!fs::exists(target, ec)) {
            // Drop any stale index entry just in case.
            index.erase(title);
            return true;   // idempotent success
        }
        existed = true;
        if (!fs::remove(target, ec)) {
            err = "delete failed: " + ec.message();
            return false;
        }
        index.erase(title);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Helper: parse a JSON array of strings out of the model's tool args.
// args::get_string handles flat strings; arrays we parse with
// nlohmann directly so the model can pass proper structured input.
//
// Lenient on one common small-model mistake: the array escaped into a
// JSON string — `"keywords": "[\"a\", \"b\"]"` instead of the spec
// form `"keywords": ["a", "b"]`. We unwrap one level: if the value is
// a string that itself parses to a JSON array, we use that. Mirrors
// args::get_array()'s stringified-array tolerance (see src/tool.cpp).
// ---------------------------------------------------------------------------
bool parse_string_array(const std::string & args_json,
                        const std::string & key,
                        std::vector<std::string> & out,
                        std::string & err) {
    try {
        auto j = json::parse(args_json.empty() ? std::string("{}") : args_json);
        if (!j.is_object()) {
            err = "arguments must be a JSON object";
            return false;
        }
        if (!j.contains(key)) return true;          // absent → empty
        json v = j[key];
        if (v.is_null()) return true;
        // Stringified-array unwrap: a quoted JSON array becomes a real
        // one. If the string isn't JSON we leave it alone and let the
        // is_array() check below produce the proper error.
        if (v.is_string()) {
            try {
                json reparsed = json::parse(v.get<std::string>());
                if (reparsed.is_array()) v = std::move(reparsed);
            } catch (const std::exception &) {
                // not JSON — fall through to the array-type error
            }
        }
        if (!v.is_array()) {
            err = "argument " + key + ": expected an array of strings, "
                  "e.g. [\"alpha\", \"beta\"] — pass a real JSON array, "
                  "not a quoted string";
            return false;
        }
        out.clear();
        out.reserve(v.size());
        for (const auto & e : v) {
            if (!e.is_string()) {
                err = "argument " + key + ": every element must be a string";
                return false;
            }
            out.push_back(e.get<std::string>());
        }
        return true;
    } catch (const std::exception & e) {
        err = std::string("invalid JSON: ") + e.what();
        return false;
    }
}

// Render a UTF-8 preview of `content`, capped at `max_bytes` AND
// snapped back to a code-point boundary so we never split a
// multi-byte character. A trailing "…" marker tells the model the
// snippet is truncated.
std::string make_preview(const std::string & content, std::size_t max_bytes) {
    if (content.size() <= max_bytes) return content;
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(content[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    std::string out(content, 0, cut);
    out += "…";
    return out;
}

// ---------------------------------------------------------------------------
// Tool handler factories
// ---------------------------------------------------------------------------

ToolHandler make_save_handler(std::shared_ptr<RagStore> store) {
    return [store](const ToolCall & c) -> ToolResult {
        std::string title_raw;
        if (!args::get_string(c.arguments_json, "title", title_raw) || title_raw.empty()) {
            return ToolResult::error("missing required argument: title");
        }

        // Normalize first so the model can pass natural strings
        // ("BitNet ternary research") without bouncing on regex.
        // The normalised form is what lives on disk; we report any
        // change back in the success message so the model knows the
        // canonical key for a later append / search / load.
        std::string title = normalize_title(title_raw, kMaxTitleBytes);
        if (title.empty()) {
            return ToolResult::error(
                "title \"" + title_raw + "\" could not be normalised to a "
                "valid identifier (alphanumerics + .-_+ only). Try a title "
                "with at least one letter or digit.");
        }

        // `fix=true` promotes the memory to immutable. Two ways to ask
        // for it: (a) pass fix=true and any title — we auto-prepend the
        // kFixedTitlePrefix so the immutability invariant lives in the
        // filename itself. (b) pass a title that already starts with
        // kFixedTitlePrefix — we honour that, fix=true is implied. The
        // invariant we maintain: an entry is fixed IFF its title starts
        // with kFixedTitlePrefix, so search / load / delete only need
        // the title to know.
        bool fix = false;
        args::get_bool(c.arguments_json, "fix", fix);
        if (fix && !title_is_fixed(title)) {
            title = std::string(kFixedTitlePrefix) + title;
            if (title.size() > kMaxTitleBytes) {
                title.resize(kMaxTitleBytes);
                while (!title.empty() && title.back() == '_') title.pop_back();
            }
        }

        std::vector<std::string> keywords_raw;
        std::string err;
        if (!parse_string_array(c.arguments_json, "keywords", keywords_raw, err)) {
            return ToolResult::error(err);
        }
        if (keywords_raw.empty()) {
            return ToolResult::error(
                "keywords must be a non-empty array (1.."
                + std::to_string(kMaxKeywordsPerEntry)
                + " short keywords). Why: keywords are how rag_search finds this "
                  "memory later — a memory with no keywords is unreachable by "
                  "keyword search (only rag_list can find it).");
        }
        // Normalize each keyword. Drop empties (e.g. "???" → ""),
        // dedup after normalisation, cap at kMaxKeywordsPerEntry.
        std::vector<std::string> keywords;
        std::vector<std::pair<std::string,std::string>> kw_changes;  // (orig → norm)
        std::vector<std::string>                         kw_dropped;
        keywords.reserve(keywords_raw.size());
        for (const auto & raw : keywords_raw) {
            std::string n = normalize_id(raw, kMaxKeywordBytes);
            if (n.empty()) {
                kw_dropped.push_back(raw);
                continue;
            }
            if (std::find(keywords.begin(), keywords.end(), n) != keywords.end()) {
                // already present (post-normalisation duplicate)
                if (raw != n) kw_changes.emplace_back(raw, n);
                continue;
            }
            if (raw != n) kw_changes.emplace_back(raw, n);
            keywords.push_back(std::move(n));
            if (keywords.size() >= kMaxKeywordsPerEntry) break;
        }
        if (keywords.empty()) {
            return ToolResult::error(
                "no usable keywords after normalisation (every one was empty "
                "or pure punctuation). Try keywords like \"bitnet\", "
                "\"ternary\", \"quantization\" — alphanumerics plus .-_+.");
        }

        std::string content;
        if (!args::get_string(c.arguments_json, "content", content)) {
            return ToolResult::error("missing required argument: content");
        }
        if (content.size() > kMaxContentBytes) {
            return ToolResult::error(
                "content exceeds " + std::to_string(kMaxContentBytes)
                + " bytes; split into multiple memories");
        }

        // WRITE: takes unique_lock so concurrent readers can't observe a
        // half-updated index. The on-disk write inside save_locked is
        // already atomic (tempfile + rename) so reads through the
        // FILESYSTEM are tear-free regardless of this lock.
        std::unique_lock<std::shared_mutex> lock(store->mu);

        // Immutability gate: an existing fixed entry can never be
        // overwritten — not even by another fix=true save. The check
        // runs UNDER the unique_lock so the index is authoritative
        // (load_index_locked ran at startup; saves keep it in sync).
        if (title_is_fixed(title) && store->index.count(title) > 0) {
            return ToolResult::error(
                "memory \"" + title + "\" is fixed (immutable) — cannot "
                "overwrite. To replace it, the operator must remove the "
                "file from disk manually. Pick a different title for a "
                "new memory.");
        }

        if (!store->save_locked(title, keywords, content, err)) {
            return ToolResult::error(err);
        }

        std::ostringstream o;
        o << "saved \"" << title << kEntrySuffix << "\" ("
          << content.size() << " bytes, "
          << keywords.size() << " keyword" << (keywords.size() == 1 ? "" : "s")
          << (title_is_fixed(title) ? ", FIXED — immutable from now on" : "")
          << ")";
        // Surface what we changed so the model can address the entry
        // by its canonical key later. Silent normalisation is friendly
        // for the first hop but invisible drift is bad over a session.
        const bool title_changed = (title_raw != title)
            && (std::string(kFixedTitlePrefix) + title_raw != title);
        if (title_changed || !kw_changes.empty() || !kw_dropped.empty()) {
            o << "\nnormalised:";
            if (title_changed) {
                o << "\n  title \"" << title_raw << "\" -> \"" << title << "\"";
            }
            for (const auto & ch : kw_changes) {
                o << "\n  keyword \"" << ch.first << "\" -> \"" << ch.second << "\"";
            }
            if (!kw_dropped.empty()) {
                o << "\n  dropped (empty after normalising):";
                for (const auto & d : kw_dropped) o << " \"" << d << "\"";
            }
        }
        return ToolResult::ok(o.str());
    };
}

// rag_append — read-modify-write an existing memory.
//
// Concurrency contract (this is the part that has to be right):
//   * The whole RMW (existence check + load_one + merge +
//     save_locked) runs under ONE std::unique_lock<shared_mutex>,
//     same writer-discipline as rag_save / rag_delete. So all
//     four scenarios serialise correctly:
//       - two threads appending to the SAME title  → ordered;
//         both appendices land, last writer's appendix is last.
//       - two threads appending to DIFFERENT titles → still
//         serialised (one shared_mutex per RagStore); cheap
//         compared to disk I/O.
//       - append vs concurrent rag_save / rag_delete → also
//         under unique_lock; whichever lands first wins, the
//         other sees the post-state (not-found or merged-content).
//       - append vs concurrent reads (rag_search / rag_load /
//         rag_list / rag_keywords) → readers hold shared_lock,
//         block until our write commits, then proceed.
//   * save_locked writes via tempfile + rename(2), so a reader
//     that obtains the file path some other way (e.g. another
//     process slurp_capped'ing the .md directly) sees either the
//     old full body or the new merged body — never a half-applied
//     append. The single-process invariant in the header
//     ("Concurrency: ... single-process is the supported model")
//     means cross-process semantics are best-effort, not contract.
//   * load_one and save_locked are designed to be called WITH the
//     unique_lock already held — they don't re-acquire, so there
//     is no upgrade dance and no risk of self-deadlock.
ToolHandler make_append_handler(std::shared_ptr<RagStore> store) {
    return [store](const ToolCall & c) -> ToolResult {
        std::string title_raw;
        if (!args::get_string(c.arguments_json, "title", title_raw) || title_raw.empty()) {
            return ToolResult::error("missing required argument: title");
        }
        // Apply the same normalisation as rag_save so the model can
        // reach the existing entry without exact-match anxiety.
        std::string title = normalize_title(title_raw, kMaxTitleBytes);
        if (title.empty()) {
            return ToolResult::error(
                "title \"" + title_raw + "\" could not be normalised to a "
                "valid identifier.");
        }

        std::string suffix;
        if (!args::get_string(c.arguments_json, "content", suffix)) {
            return ToolResult::error("missing required argument: content");
        }
        if (suffix.empty()) {
            return ToolResult::error(
                "content is empty — nothing to append. If you want to "
                "replace the whole memory, use rag_save with the same title.");
        }

        // Optional: extra keywords to merge into the existing list.
        // Normalised + deduped the same way rag_save does.
        std::vector<std::string> extra_raw;
        std::string err;
        if (args::has(c.arguments_json, "keywords")
                && !parse_string_array(c.arguments_json, "keywords",
                                       extra_raw, err)) {
            return ToolResult::error(err);
        }
        std::vector<std::string> extra_keywords;
        std::vector<std::pair<std::string,std::string>> kw_changes;
        std::vector<std::string>                         kw_dropped;
        for (const auto & raw : extra_raw) {
            std::string n = normalize_id(raw, kMaxKeywordBytes);
            if (n.empty()) { kw_dropped.push_back(raw); continue; }
            if (std::find(extra_keywords.begin(), extra_keywords.end(), n)
                    != extra_keywords.end()) {
                if (raw != n) kw_changes.emplace_back(raw, n);
                continue;
            }
            if (raw != n) kw_changes.emplace_back(raw, n);
            extra_keywords.push_back(std::move(n));
        }

        // WRITE: unique_lock for the whole RMW so a concurrent
        // rag_save (also unique_lock) can't slip between our read of
        // the existing body and the rewrite of the merged content.
        std::unique_lock<std::shared_mutex> lock(store->mu);

        // If the title doesn't exist yet, create it as a new memory
        // (save semantics) instead of erroring — the caller's intent
        // is clearly "make sure this content ends up under this title".
        const bool is_new = (store->index.count(title) == 0);
        if (is_new) {
            // keywords are required for a brand-new memory (search
            // needs at least one). If the caller didn't supply any,
            // error with a helpful message.
            if (extra_keywords.empty()) {
                return ToolResult::error(
                    "no memory titled \"" + title + "\" — memory_append "
                    "can create it, but keywords[] is required for new "
                    "memories (search needs at least one).");
            }
            if (suffix.size() > kMaxContentBytes) {
                return ToolResult::error(
                    "content exceeds " + std::to_string(kMaxContentBytes)
                    + " bytes; split into multiple memories");
            }
            if (!store->save_locked(title, extra_keywords, suffix, err)) {
                return ToolResult::error(err);
            }
            std::ostringstream o;
            o << "new memory saved as \"" << title << kEntrySuffix << "\" ("
              << suffix.size() << " bytes, "
              << extra_keywords.size() << " keyword"
              << (extra_keywords.size() == 1 ? "" : "s") << ")";
            const bool title_changed = (title_raw != title);
            if (title_changed || !kw_changes.empty() || !kw_dropped.empty()) {
                o << "\nnormalised:";
                if (title_changed)
                    o << "\n  title \"" << title_raw << "\" -> \"" << title << "\"";
                for (const auto & ch : kw_changes)
                    o << "\n  keyword \"" << ch.first << "\" -> \"" << ch.second << "\"";
                if (!kw_dropped.empty()) {
                    o << "\n  dropped (empty after normalising):";
                    for (const auto & d : kw_dropped) o << " \"" << d << "\"";
                }
            }
            return ToolResult::ok(o.str());
        }

        // Immutability gate: fixed memories live forever as written.
        if (title_is_fixed(title)) {
            return ToolResult::error(
                "memory \"" + title + "\" is fixed (immutable) — cannot "
                "append. Pick a different title for a related memory, "
                "or have the operator remove the file from disk first.");
        }

        // Read the existing body + keywords back from disk via the
        // store helper (slurp + parse_entry, capped at
        // kMaxContentBytes + 4 KiB header room — same cap rag_load
        // uses, so an oversized hand-edited file is rejected with
        // the same message the rest of the surface produces).
        std::vector<std::string> old_keywords;
        std::string              old_body;
        std::int64_t             old_mtime = 0;   // unused but required by the API
        if (!store->load_one(title, old_keywords, old_body, old_mtime, err)) {
            return ToolResult::error(err);
        }

        // Compose the merged body. We insert a Markdown horizontal
        // rule as the separator so the operator opening the .md file
        // sees exactly where the appendix begins. Trim any trailing
        // newlines from the existing body first so the rule sits on
        // a clean blank line regardless of how the previous save
        // happened to terminate.
        std::string merged = old_body;
        while (!merged.empty() && (merged.back() == '\n' || merged.back() == '\r')) {
            merged.pop_back();
        }
        if (!merged.empty()) merged += "\n\n---\n\n";
        merged += suffix;

        if (merged.size() > kMaxContentBytes) {
            return ToolResult::error(
                "appended memory would exceed " + std::to_string(kMaxContentBytes)
                + " bytes (existing " + std::to_string(old_body.size())
                + " B + appendix " + std::to_string(suffix.size())
                + " B + separator). Split into a new memory with rag_save "
                  "instead, or condense the appendix.");
        }

        // Merge keywords: keep the existing order (the model relies on
        // search ranking that's stable across appends), then append any
        // extras the model passed that weren't already there. Cap at
        // kMaxKeywordsPerEntry; oldest stays.
        std::vector<std::string> merged_keywords = old_keywords;
        for (const auto & k : extra_keywords) {
            const bool already = std::find(merged_keywords.begin(),
                                           merged_keywords.end(), k)
                                 != merged_keywords.end();
            if (!already && merged_keywords.size() < kMaxKeywordsPerEntry) {
                merged_keywords.push_back(k);
            }
        }
        if (merged_keywords.empty()) {
            // Defensive — every saved memory has at least one keyword
            // (rag_save enforces it). If somehow we read back an entry
            // with none (operator hand-edited the keywords: header out),
            // require the model to supply them on append rather than
            // writing a search-invisible memory.
            return ToolResult::error(
                "existing memory has no keywords (someone may have hand-"
                "edited it); pass keywords[] to rag_append so the merged "
                "memory remains searchable.");
        }

        if (!store->save_locked(title, merged_keywords, merged, err)) {
            return ToolResult::error(err);
        }

        std::ostringstream o;
        o << "updated \"" << title << kEntrySuffix << "\" ("
          << "+" << suffix.size() << " B → " << merged.size() << " B total, "
          << merged_keywords.size() << " keyword"
          << (merged_keywords.size() == 1 ? "" : "s") << ")";
        const bool title_changed = (title_raw != title);
        if (title_changed || !kw_changes.empty() || !kw_dropped.empty()) {
            o << "\nnormalised:";
            if (title_changed) {
                o << "\n  title \"" << title_raw << "\" -> \"" << title << "\"";
            }
            for (const auto & ch : kw_changes) {
                o << "\n  keyword \"" << ch.first << "\" -> \"" << ch.second << "\"";
            }
            if (!kw_dropped.empty()) {
                o << "\n  dropped (empty after normalising):";
                for (const auto & d : kw_dropped) o << " \"" << d << "\"";
            }
        }
        return ToolResult::ok(o.str());
    };
}

ToolHandler make_search_handler(std::shared_ptr<RagStore> store) {
    return [store](const ToolCall & c) -> ToolResult {
        // Multi-keyword search.
        //   - keywords[0] is REQUIRED: every result is guaranteed to
        //     carry the first keyword the model passed.
        //   - keywords[1..] are OPTIONAL: they never exclude a result,
        //     they only lift its rank (more overlap → higher).
        //
        // So a result is returned iff it carries keyword[0]; the rest
        // just sort the matches. Each result reports how many of the
        // queried keywords it matched (`[matched N/M]`) so the model
        // can see the overlap and pick. A single-keyword query is the
        // degenerate case — that one keyword is the required one.
        std::vector<std::string> keywords_raw;
        std::string err;
        if (!parse_string_array(c.arguments_json, "keywords", keywords_raw, err)) {
            return ToolResult::error(err);
        }
        if (keywords_raw.empty()) {
            return ToolResult::error(
                "missing required argument: keywords (non-empty array)");
        }
        if (keywords_raw.size() > kMaxKeywordsPerEntry) {
            return ToolResult::error(
                "too many keywords (max "
                + std::to_string(kMaxKeywordsPerEntry) + " per query)");
        }
        // Normalise the query keywords with the same rules rag_save
        // uses, so "neural network" finds entries indexed under
        // "neural_network". Order-preserving dedup — keywords[0] is
        // the required keyword and must keep its slot.
        std::vector<std::string> keywords;
        keywords.reserve(keywords_raw.size());
        for (const auto & raw : keywords_raw) {
            std::string n = normalize_id(raw, kMaxKeywordBytes);
            if (n.empty()) continue;
            if (std::find(keywords.begin(), keywords.end(), n) == keywords.end()) {
                keywords.push_back(std::move(n));
            }
        }
        if (keywords.empty()) {
            return ToolResult::error(
                "no usable keywords after normalisation — try alphanumerics "
                "plus .-_+ (e.g. \"bitnet\", \"ternary\").");
        }
        const std::string & required_kw = keywords.front();

        long long max_results = (long long) kSearchResultsDflt;
        args::get_int(c.arguments_json, "max_results", max_results);
        if (max_results < 1) max_results = 1;
        if ((std::size_t) max_results > kSearchResultsMax) {
            max_results = (long long) kSearchResultsMax;
        }

        struct Hit {
            std::string title;
            EntryMeta   meta;
            std::size_t matched = 0;
            bool        has_required = false;
            std::vector<std::string> matched_keywords;
        };
        std::vector<Hit> hits;
        {
            // READ: shared_lock — many concurrent searches can iterate
            // the index in parallel. The index was populated eagerly by
            // make_rag_tools() under a unique lock, so we never need to
            // upgrade here.
            std::shared_lock<std::shared_mutex> lock(store->mu);
            for (const auto & [t, m] : store->index) {
                Hit h;
                h.title = t;
                h.meta  = m;
                for (std::size_t qi = 0; qi < keywords.size(); ++qi) {
                    const auto & q = keywords[qi];
                    for (const auto & et : m.keywords) {
                        if (et == q) {
                            h.matched_keywords.push_back(q);
                            if (qi == 0) h.has_required = true;
                            break;
                        }
                    }
                }
                h.matched = h.matched_keywords.size();
                // keywords[0] is mandatory; the rest only rank.
                if (h.has_required) {
                    hits.push_back(std::move(h));
                }
            }
        }
        // Rank: more overlap first, ties broken by recency.
        std::sort(hits.begin(), hits.end(), [](const Hit & a, const Hit & b) {
            if (a.matched != b.matched) return a.matched > b.matched;
            return a.meta.modified_unix > b.meta.modified_unix;
        });

        // Pagination — the model can ask for `page=N` to walk the rest
        // of a large result set without re-issuing a different query.
        // We compute totals on the FULL ranked list, then slice.
        long long page = 1;
        args::get_int(c.arguments_json, "page", page);
        if (page < 1) page = 1;

        const std::size_t total       = hits.size();
        const std::size_t per_page    = (std::size_t) max_results;
        const std::size_t total_pages =
            total == 0 ? 0 : (total + per_page - 1) / per_page;
        const std::size_t offset      =
            (std::size_t)((page - 1) * (long long) per_page);

        // Build a "queried [a, b, c]" string once for the response prose.
        std::string queried_str;
        for (std::size_t i = 0; i < keywords.size(); ++i) {
            queried_str += (i ? ", " : "");
            queried_str += keywords[i];
        }

        if (hits.empty()) {
            std::ostringstream o;
            o << "total_entries: 0\n";
            o << "page: " << page << " of 0\n\n";
            o << "no entries carry the required keyword \""
              << required_kw << "\"";
            if (keywords.size() > 1) {
                o << " (the other queried keywords only affect ranking, "
                     "they don't broaden the match)";
            }
            o << ". Use rag_list to browse, or rag_save to add new entries.";
            return ToolResult::ok(o.str());
        }

        if (offset >= total) {
            std::ostringstream o;
            o << "total_entries: " << total << "\n";
            o << "page: " << page << " of " << total_pages
              << "  (past the end)\n\n";
            o << "page " << page << " is past the last page ("
              << total_pages << "). The full result set is "
              << total << " entr" << (total == 1 ? "y" : "ies")
              << " — use page=1.." << total_pages << ".";
            return ToolResult::ok(o.str());
        }

        const std::size_t slice_end = std::min(offset + per_page, total);
        const std::size_t shown     = slice_end - offset;
        const bool        has_more  = slice_end < total;

        // Render plain-text + structured (markdown-friendly) output.
        // The model parses this with no JSON dependency on its side
        // and the operator can `cat` it from a journal log.
        //
        // Header layout (machine-readable lines first, then prose) so
        // the model can grep `total_entries:` / `page:` / `has_more:`
        // without parsing the body.
        std::ostringstream o;
        o << "total_entries: " << total << "\n";
        o << "page: "          << page << " of " << total_pages << "\n";
        o << "showing: "       << shown
          << "  (entries " << (offset + 1) << ".." << slice_end << ")\n";
        o << "has_more: "      << (has_more ? "true" : "false") << "\n\n";

        if (keywords.size() == 1) {
            o << "match keyword \"" << queried_str
              << "\" (newest first):\n\n";
        } else {
            o << "required keyword \"" << required_kw
              << "\" — queried [" << queried_str
              << "], ranked by overlap (best first, then newest):\n\n";
        }
        for (std::size_t i = offset; i < slice_end; ++i) {
            const auto & h = hits[i];
            std::vector<std::string> body_keywords;
            std::string              body_text;
            std::int64_t             mtime = 0;
            std::string              load_err;
            std::string              preview;
            if (store->load_one(h.title, body_keywords, body_text, mtime, load_err)) {
                preview = make_preview(body_text, kSearchPreviewBytes);
            } else {
                preview = "(could not read body: " + load_err + ")";
            }

            // Number entries by their absolute position in the ranked
            // list so the model can correlate across pages.
            o << (i + 1) << ". " << h.title;
            if (title_is_fixed(h.title)) o << "  [FIXED]";
            if (keywords.size() > 1) {
                o << "  [matched " << h.matched << "/" << keywords.size()
                  << ": ";
                for (std::size_t k = 0; k < h.matched_keywords.size(); ++k) {
                    if (k) o << ", ";
                    o << h.matched_keywords[k];
                }
                o << "]";
            }
            o << "\n";
            o << "   keywords: ";
            for (std::size_t k = 0; k < h.meta.keywords.size(); ++k) {
                if (k) o << ", ";
                o << h.meta.keywords[k];
            }
            o << "  (" << h.meta.content_bytes << " bytes)\n";
            o << "   modified: " << format_local_time(h.meta.modified_unix)
              << "  (unix=" << h.meta.modified_unix << ")\n";
            // Indent preview lines by 3 so it's easy to skim.
            std::string preview_in;
            preview_in.reserve(preview.size() + preview.size() / 60 * 3);
            for (char ch : preview) {
                preview_in += ch;
                if (ch == '\n') preview_in += "   ";
            }
            o << "   " << preview_in << "\n\n";
        }
        if (has_more) {
            o << "Use rag_search with the same keywords + page="
              << (page + 1) << " to see the next "
              << std::min(per_page, total - slice_end)
              << " result" << (total - slice_end == 1 ? "" : "s")
              << " (page " << (page + 1) << " of " << total_pages << ").\n";
        }
        o << "Use rag_load with up to " << kMaxLoadAtOnce
          << " of these titles for full content.\n"
          << "\n[CITE: if you use any of these results in your reply, "
             "end with a Sources: block citing memory: \"<title>\" "
             "per entry used.]\n";
        return ToolResult::ok(o.str());
    };
}

ToolHandler make_load_handler(std::shared_ptr<RagStore> store) {
    return [store](const ToolCall & c) -> ToolResult {
        std::vector<std::string> titles;
        std::string err;
        if (!parse_string_array(c.arguments_json, "titles", titles, err)) {
            return ToolResult::error(err);
        }
        if (titles.empty()) {
            return ToolResult::error("missing required argument: titles "
                                     "(non-empty array)");
        }
        if (titles.size() > kMaxLoadAtOnce) {
            return ToolResult::error(
                "too many titles requested (max "
                + std::to_string(kMaxLoadAtOnce)
                + " per call); narrow your rag_search first");
        }
        // Normalise titles silently so the model can ask for
        // "BitNet ternary research" and reach the on-disk
        // "BitNet_ternary_research". A title that can't normalise
        // (pure punctuation, "..") errors out — that's a real
        // mistake, not just a formatting nit.
        for (auto & t : titles) {
            std::string n = normalize_title(t, kMaxTitleBytes);
            if (n.empty()) {
                return ToolResult::error(
                    "title \"" + t + "\" could not be normalised to a valid "
                    "identifier (alphanumerics + .-_+; not '.' / '..').");
            }
            t = std::move(n);
        }

        std::ostringstream o;
        o << "loaded " << titles.size() << " entr"
          << (titles.size() == 1 ? "y" : "ies") << ":\n";
        for (const auto & title : titles) {
            std::vector<std::string> keywords;
            std::string body;
            std::int64_t mtime = 0;
            std::string e_err;
            // READ: shared_lock — load_one() reads the file off disk
            // (atomic-rename guarantees a consistent view) and never
            // touches the index. Multiple parallel rag_load calls run
            // concurrently with no contention on the mutex itself.
            std::shared_lock<std::shared_mutex> lock(store->mu);
            if (!store->load_one(title, keywords, body, mtime, e_err)) {
                o << "\n--- " << title << " ---\n"
                  << "ERROR: " << e_err << "\n";
                continue;
            }
            o << "\n--- " << title << " ---\n";
            o << "keywords: ";
            for (std::size_t i = 0; i < keywords.size(); ++i) {
                if (i) o << ", ";
                o << keywords[i];
            }
            o << "\nmodified: " << format_local_time(mtime)
              << "  (unix=" << mtime << ")\n";
            o << "fixed: " << (title_is_fixed(title) ? "yes" : "no") << "\n";
            if (title_is_fixed(title)) {
                o << "note: this memory is immutable — rag_save and rag_delete "
                     "will refuse to change or remove it.\n";
            }
            o << "\n";
            o << body;
            if (!body.empty() && body.back() != '\n') o << '\n';
        }
        o << "\n[CITE: if you use loaded content in your reply, end "
             "with a Sources: block citing memory: \"<title>\" per "
             "entry used.]\n";
        return ToolResult::ok(o.str());
    };
}

ToolHandler make_list_handler(std::shared_ptr<RagStore> store) {
    return [store](const ToolCall & c) -> ToolResult {
        std::string prefix_raw;
        args::get_string(c.arguments_json, "prefix", prefix_raw);
        std::string prefix;
        if (!prefix_raw.empty()) {
            // normalize_id (not normalize_title) — a prefix may end
            // partway through a title and the FS-safety rules
            // (no leading dot, must contain alnum) don't apply to
            // a partial match. Empty after normalisation = ignore
            // the filter entirely.
            prefix = normalize_id(prefix_raw, kMaxTitleBytes);
        }
        long long max = (long long) kListResultsDflt;
        args::get_int(c.arguments_json, "max", max);
        if (max < 1) max = 1;
        if ((std::size_t) max > kListResultsMax) max = (long long) kListResultsMax;

        struct Row { std::string title; EntryMeta meta; };
        std::vector<Row> rows;
        {
            // READ: shared_lock — same justification as rag_search.
            std::shared_lock<std::shared_mutex> lock(store->mu);
            for (const auto & [t, m] : store->index) {
                if (!prefix.empty() &&
                    (t.size() < prefix.size() ||
                     t.compare(0, prefix.size(), prefix) != 0)) {
                    continue;
                }
                rows.push_back({ t, m });
                if ((long long) rows.size() >= max) break;
            }
        }

        if (rows.empty()) {
            return ToolResult::ok(prefix.empty()
                ? "Memory is empty. Use rag_save to add entries."
                : "no titles match prefix \"" + prefix + "\".");
        }

        std::ostringstream o;
        o << rows.size() << " entr" << (rows.size() == 1 ? "y" : "ies");
        if (!prefix.empty()) o << " matching prefix \"" << prefix << "\"";
        o << ":\n\n";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto & r = rows[i];
            o << (i + 1) << ". " << r.title;
            if (title_is_fixed(r.title)) o << "  [FIXED]";
            if (!r.meta.keywords.empty()) {
                o << "  [";
                for (std::size_t k = 0; k < r.meta.keywords.size(); ++k) {
                    if (k) o << ", ";
                    o << r.meta.keywords[k];
                }
                o << "]";
            } else {
                o << "  (no keywords)";
            }
            o << "  " << r.meta.content_bytes << " bytes";
            o << "  modified=" << format_local_time(r.meta.modified_unix) << "\n";
        }
        return ToolResult::ok(o.str());
    };
}

ToolHandler make_delete_handler(std::shared_ptr<RagStore> store) {
    return [store](const ToolCall & c) -> ToolResult {
        std::string title_raw;
        if (!args::get_string(c.arguments_json, "title", title_raw) || title_raw.empty()) {
            return ToolResult::error("missing required argument: title");
        }
        std::string title = normalize_title(title_raw, kMaxTitleBytes);
        if (title.empty()) {
            return ToolResult::error(
                "title \"" + title_raw + "\" could not be normalised to a "
                "valid identifier.");
        }
        // Fixed memories are immutable by design — refuse the delete
        // before we even take the write lock. The operator can still
        // remove the file from disk by hand if they truly need to;
        // exposing that path through a tool defeats the whole point of
        // the prefix.
        if (title_is_fixed(title)) {
            return ToolResult::error(
                "memory \"" + title + "\" is fixed (immutable) — cannot "
                "be forgotten through this tool. The operator can remove "
                "the file from disk manually if it really needs to go.");
        }
        // WRITE: unique_lock — delete_locked mutates the index AND
        // removes the on-disk file. fs::remove is itself atomic, so
        // parallel readers either see the entry or don't, never a
        // half-deleted state.
        std::unique_lock<std::shared_mutex> lock(store->mu);
        bool existed = false;
        std::string err;
        if (!store->delete_locked(title, existed, err)) {
            return ToolResult::error(err);
        }
        if (!existed) {
            return ToolResult::ok(
                "no memory titled \"" + title + "\" — nothing to forget");
        }
        return ToolResult::ok(
            "forgot \"" + title + kEntrySuffix + "\"");
    };
}

// rag_keywords — vocabulary overview. Returns each distinct
// keyword used across the RAG together with how many entries
// reference it. The model uses this to:
//
//   - learn its own vocabulary before saving (avoid creating a
//     new keyword like `user_pref` when `user-prefs` already
//     exists)
//   - discover dimensions of stored knowledge it forgot about
//   - frame rag_search queries against keywords that actually
//     return results
//
// Output: header lines (`total_keywords:`, `total_entries:`,
// `showing:`) followed by sorted rows. Sort order: count
// descending, then keyword name ascending so the response is
// stable across calls.
ToolHandler make_keywords_handler(std::shared_ptr<RagStore> store) {
    return [store](const ToolCall & c) -> ToolResult {
        long long min_count = 1;
        args::get_int(c.arguments_json, "min_count", min_count);
        if (min_count < 1) min_count = 1;

        long long max = (long long) kKeywordsResultsDflt;
        args::get_int(c.arguments_json, "max", max);
        if (max < 1) max = 1;
        if ((std::size_t) max > kKeywordsResultsMax) {
            max = (long long) kKeywordsResultsMax;
        }

        // Phase 1: under shared_lock, collect counts from the index.
        // READ — same justification as rag_search; many concurrent
        // rag_keywords calls run in parallel without serialising.
        std::map<std::string, std::size_t> counts;
        std::size_t total_entries = 0;
        {
            std::shared_lock<std::shared_mutex> lock(store->mu);
            total_entries = store->index.size();
            for (const auto & [_, m] : store->index) {
                for (const auto & k : m.keywords) {
                    counts[k] += 1;
                }
            }
        }

        // Phase 2: filter by min_count and sort by (count desc, name asc).
        struct Row { std::string keyword; std::size_t count; };
        std::vector<Row> rows;
        rows.reserve(counts.size());
        for (const auto & [k, n] : counts) {
            if (n >= (std::size_t) min_count) {
                rows.push_back({ k, n });
            }
        }
        std::sort(rows.begin(), rows.end(), [](const Row & a, const Row & b) {
            if (a.count != b.count) return a.count > b.count;
            return a.keyword < b.keyword;
        });
        const std::size_t total_kw = rows.size();
        if ((long long) rows.size() > max) {
            rows.resize((std::size_t) max);
        }

        // Render.
        std::ostringstream o;
        o << "total_keywords: " << total_kw << "\n";
        o << "total_entries: "  << total_entries << "\n";
        o << "showing: "        << rows.size();
        if (min_count > 1) {
            o << "  (min_count=" << min_count << ")";
        }
        o << "\n\n";

        if (rows.empty()) {
            if (total_entries == 0) {
                o << "Memory is empty. Use rag_save to add the first entry.";
            } else if (min_count > 1) {
                o << "no keywords reach min_count=" << min_count
                  << ". Memory has " << total_entries
                  << " entr" << (total_entries == 1 ? "y" : "ies")
                  << " but every keyword is below the threshold. "
                  << "Try min_count=1 (default) to see the full list.";
            } else {
                o << "no keywords found. Some entries may be untagged "
                  << "(no `keywords:` header) — those don't appear here. "
                  << "Use rag_list to see them.";
            }
            return ToolResult::ok(o.str());
        }

        // Pad the keyword column for legibility. Cap at the longest
        // keyword in the result so we don't waste tokens.
        std::size_t pad = 0;
        for (const auto & r : rows) pad = std::max(pad, r.keyword.size());
        if (pad > kMaxKeywordBytes) pad = kMaxKeywordBytes;

        for (const auto & r : rows) {
            o << r.keyword;
            for (std::size_t i = r.keyword.size(); i < pad + 2; ++i) {
                o << ' ';
            }
            o << r.count
              << " entr" << (r.count == 1 ? "y" : "ies") << "\n";
        }
        return ToolResult::ok(o.str());
    };
}

// Build a RagStore at `root_dir` and eager-load its index. After this
// returns, every read path can take a shared_lock and observe
// `index_loaded == true` without racing.
std::shared_ptr<RagStore> build_rag_store(std::string root_dir) {
    auto store = std::make_shared<RagStore>(std::move(root_dir));
    {
        std::unique_lock<std::shared_mutex> init_lock(store->mu);
        store->load_index_locked();
    }
    return store;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry point — single-tool dispatcher
// ---------------------------------------------------------------------------
// One `rag` tool with an `action` parameter selecting one of "save" /
// "append" / "search" / "load" / "list" / "delete" / "keywords". The
// per-action handlers (make_save_handler / make_search_handler / etc.)
// read every other parameter directly out of `arguments_json`, so the
// dispatcher just picks the right closure by `action` and forwards the
// original ToolCall.
//
// Schema is a kitchen sink (every parameter optional except `action`)
// because JSON Schema's discriminated-union shapes (oneOf with a
// discriminator) trip up smaller / quantised tool-callers far more
// often than a flat "everything optional" schema does. Validation
// stays runtime: each handler rejects calls missing its required
// fields with a crisp message naming the dispatch form.
//
// (Until 2026-05-09 there was also a seven-tool split factory
// `make_rag_tools` exposed behind `--split-rag`. It was removed
// alongside the unification of the web and fs tool surfaces. The
// on-disk format and locking discipline are unchanged.)
Tool make_rag_tool(std::string root_dir) {
    auto store = build_rag_store(std::move(root_dir));

    // Capture each per-action handler once; the dispatcher closes
    // over the resulting std::function set. Same store is shared, so
    // index updates from `save` / `delete` are visible to subsequent
    // `search` / `load` / `list` / `keywords` calls inside the same
    // process.
    auto h_save     = make_save_handler    (store);
    auto h_append   = make_append_handler  (store);
    auto h_search   = make_search_handler  (store);
    auto h_load     = make_load_handler    (store);
    auto h_list     = make_list_handler    (store);
    auto h_delete   = make_delete_handler  (store);
    auto h_keywords = make_keywords_handler(store);

    return Tool::builder("memory")
        .describe(
            "Your private memory — one tool, seven actions.\n"
            "Arguments are PLAIN JSON — no XML tags, no markup.\n"
            "\n"
            "PRIVATE: the user can't see, list, or browse this store. "
            "Never say \"check memory\" / \"I saved it\" — load it "
            "yourself and put the body in your reply.\n"
            "\n"
            "action=\"save\"     title, keywords, content → store or "
            "overwrite.\n"
            "  Optional: fix=true → immutable, title gets "
            "`fix-easyai-` prefix.\n"
            "\n"
            "action=\"append\"   title, content → add to existing "
            "memory (a Markdown `---` separates additions). If the "
            "title does not exist, creates a new memory (keywords "
            "required). Returns whether saved (new) or updated "
            "(appended). Refused on fix-easyai-*.\n"
            "  Optional: keywords (merged into existing, deduped, "
            "cap 8; required if title is new).\n"
            "\n"
            "action=\"search\"   keywords (JSON array) → ranked "
            "matches.\n"
            "  keywords[0] is MANDATORY (every result carries it); "
            "the rest rank but don't exclude.\n"
            "  Optional: max_results (default 10, max 20), page "
            "(default 1).\n"
            "\n"
            "action=\"load\"     titles (1..20 exact) → full content.\n"
            "\n"
            "action=\"list\"     → titles only.\n"
            "  Optional: prefix (e.g. `fix-easyai-`), max (default "
            "50, max 200).\n"
            "\n"
            "action=\"delete\"   title → forget. fix-easyai-* are "
            "immutable.\n"
            "\n"
            "action=\"keywords\" → vocabulary overview (every keyword "
            "+ count).\n"
            "  Optional: min_count (default 1), max (default 200, "
            "max 500).\n"
            "\n"
            "CALL EXAMPLES (exact JSON):\n"
            "  {\"action\":\"search\",\"keywords\":[\"BitNet\",\"binary\"]}\n"
            "  {\"action\":\"load\",\"titles\":[\"BitNet\"]}\n"
            "  {\"action\":\"save\",\"title\":\"BitNet\","
            "\"keywords\":[\"BitNet\",\"quantization\"],"
            "\"content\":\"...\"}\n"
            "  {\"action\":\"list\"}\n"
            "  {\"action\":\"keywords\"}\n"
            "\n"
            "KNOWLEDGE LOOP (MANDATORY when memory + web are "
            "available):\n"
            "\n"
            "  1. MEMORY FIRST — search memory for relevant keywords "
            "before anything else. Load hits. Memory is your primary "
            "knowledge base across sessions.\n"
            "\n"
            "  2. WEB SECOND — also search the web, even when memory "
            "had results. The web may have newer or broader info.\n"
            "\n"
            "  3. MERGE & ANSWER — combine both sources. Prefer the "
            "more recent or authoritative one when they conflict; "
            "note the discrepancy to the user.\n"
            "\n"
            "  4. UPDATE MEMORY — if the web produced durable "
            "knowledge that memory lacked or had outdated, save or "
            "append it now. Save the distilled fact, not raw page "
            "content.\n"
            "\n"
            "Skipping either source when both are available is a "
            "failure mode.\n"
            "\n"
            "SAVE GUIDELINES:\n"
            "\n"
            "  - Save what's DURABLE — facts you'll want next "
            "session: preferences, architecture, commands, fixes, "
            "recipes. NOT transient research the user is already "
            "reading.\n"
            "\n"
            "  - ONE comprehensive memory per topic, not fragments. "
            "Append to existing entries instead of creating parallel "
            "ones.\n"
            "\n"
            "  - Reusable procedures → keyword \"skill\". Search "
            "keywords=[\"skill\", ...] before working a procedure "
            "out from scratch.\n"
            "\n"
            "CITATION (INVIOLABLE): after ANY memory search or load "
            "this turn that returns content you use in your reply, "
            "your reply MUST end with a `Sources:` block citing the "
            "memory title(s) as `memory: \"<title>\"`. This applies "
            "even when no web tools were used — memory retrieval is "
            "an external lookup."
        )
        .param("action",      "string",
               "\"save\", \"append\", \"search\", \"load\", \"list\", "
               "\"delete\", or \"keywords\".", true)
        .param("title",       "string",
               "1..64 chars [A-Za-z0-9._+-]. save/append/delete.",
               false)
        .param("titles",      "array",
               "1..20 exact titles. load only.", false)
        .param("keywords",    "array",
               "JSON array of 1..8 short strings [A-Za-z0-9._+-]. "
               "On search: first keyword is required, rest rank.",
               false)
        .param("content",     "string",
               "UTF-8 body. save/append. Total on disk capped at "
               "256 KB.", false)
        .param("fix",         "boolean",
               "save only. Immutable + `fix-easyai-` prefix. Default "
               "false.", false)
        .param("prefix",      "string",
               "list only. Filter titles by prefix.", false)
        .param("max",         "integer",
               "list/keywords cap. list default 50 (max 200); "
               "keywords default 200 (max 500).", false)
        .param("max_results", "integer",
               "search page size. Default 10, max 20.", false)
        .param("page",        "integer",
               "search page index, 1-based. Default 1.", false)
        .param("min_count",   "integer",
               "keywords only. Hide keywords used by fewer than N "
               "memories. Default 1.", false)
        .handle([h_save, h_append, h_search, h_load, h_list, h_delete, h_keywords]
                (const ToolCall & c) -> ToolResult {
            std::string action;
            if (!args::get_string(c.arguments_json, "action", action)
                    || action.empty()) {
                return ToolResult::error(
                    "missing required argument: action. Use one of "
                    "\"save\", \"append\", \"search\", \"load\", "
                    "\"list\", \"delete\", \"keywords\".");
            }
            // Each branch forwards the original ToolCall — the per-
            // action handlers parse their own params out of
            // arguments_json with the same helpers (args::get_string,
            // parse_string_array, etc.) the legacy seven-tool flow uses,
            // so error messages and validation stay byte-identical.
            ToolResult r;
            if      (action == "save")     r = h_save(c);
            else if (action == "append")   r = h_append(c);
            else if (action == "search")   r = h_search(c);
            else if (action == "load")     r = h_load(c);
            else if (action == "list")     r = h_list(c);
            else if (action == "delete")   r = h_delete(c);
            else if (action == "keywords") r = h_keywords(c);
            else {
                return ToolResult::error(
                    "unknown action \"" + action + "\". Valid: \"save\", "
                    "\"append\", \"search\", \"load\", \"list\", "
                    "\"delete\", \"keywords\".");
            }

            // The inner handlers still cite the legacy seven-tool
            // names (rag_save, rag_append, rag_search, ...) in their
            // guidance prose. The model only has `memory` in its
            // catalog, so any literal `rag_<verb>` reference would be
            // a dangling identifier. Rewrite each occurrence in place
            // to the dispatch form the model can actually call. Cheap
            // (O(n) over a small message); the set of substitutions is
            // closed and stable.
            struct Sub { const char * from; const char * to; };
            static const Sub kSubs[] = {
                // Order matters: rag_append must come before rag_a... siblings
                // would, but only rag_save shares the leading 'rag_' so any
                // order works. Keep alphabetical for grep-ability.
                { "rag_append",   "memory(action=\"append\")"   },
                { "rag_delete",   "memory(action=\"delete\")"   },
                { "rag_keywords", "memory(action=\"keywords\")" },
                { "rag_list",     "memory(action=\"list\")"     },
                { "rag_load",     "memory(action=\"load\")"     },
                { "rag_save",     "memory(action=\"save\")"     },
                { "rag_search",   "memory(action=\"search\")"   },
            };
            for (const auto & s : kSubs) {
                std::string from = s.from;
                std::string to   = s.to;
                size_t pos = 0;
                while ((pos = r.content.find(from, pos)) != std::string::npos) {
                    r.content.replace(pos, from.size(), to);
                    pos += to.size();
                }
            }
            return r;
        })
        .build();
}

// ----------------------------------------------------------------------------
// memory_split_tools — focused alternative to the unified `memory` tool.
// ----------------------------------------------------------------------------
// Same per-action handlers, same on-disk store; the only difference is
// surface. Smaller models avoid the "unknown action" / "wrong action for
// these args" failure mode when the verb IS the tool name.
std::vector<Tool> memory_split_tools(std::string root_dir) {
    auto store = build_rag_store(std::move(root_dir));
    auto h_save     = make_save_handler    (store);
    auto h_append   = make_append_handler  (store);
    auto h_search   = make_search_handler  (store);
    auto h_load     = make_load_handler    (store);
    auto h_list     = make_list_handler    (store);
    auto h_delete   = make_delete_handler  (store);
    auto h_keywords = make_keywords_handler(store);

    // Inner handlers cite the legacy seven-tool names (rag_save,
    // rag_append, rag_search, ...) in their guidance prose because
    // those were the original tool names. With memory_split_tools
    // registered, the actual callable names are memory_save /
    // memory_search / etc. — leaving rag_<verb> in handler output
    // would point the model at non-existent tool names. Wrap every
    // handler with a substitution that rewrites occurrences in place.
    // Mirrors the same idiom make_rag_tool uses (line ~1850); the
    // unified dispatcher there maps to `memory(action="...")` form
    // because that IS its tool surface, while we map to the split
    // names. Cheap O(n) string scan per call.
    struct Sub { const char * from; const char * to; };
    static const Sub kSubs[] = {
        { "rag_append",   "memory_append"   },
        { "rag_delete",   "memory_delete"   },
        { "rag_keywords", "memory_keywords" },
        { "rag_list",     "memory_list"     },
        { "rag_load",     "memory_load"     },
        { "rag_save",     "memory_save"     },
        { "rag_search",   "memory_search"   },
    };
    auto rewrite_for_split = [](ToolResult r) -> ToolResult {
        for (const auto & s : kSubs) {
            std::string from = s.from;
            std::string to   = s.to;
            size_t pos = 0;
            while ((pos = r.content.find(from, pos)) != std::string::npos) {
                r.content.replace(pos, from.size(), to);
                pos += to.size();
            }
        }
        return r;
    };
    auto wrap = [&](auto inner) {
        return [inner, rewrite_for_split](const ToolCall & c) -> ToolResult {
            return rewrite_for_split(inner(c));
        };
    };
    h_save     = wrap(h_save);
    h_append   = wrap(h_append);
    h_search   = wrap(h_search);
    h_load     = wrap(h_load);
    h_list     = wrap(h_list);
    h_delete   = wrap(h_delete);
    h_keywords = wrap(h_keywords);

    std::vector<Tool> out;
    out.reserve(7);

    out.push_back(Tool::builder("memory_save")
        .describe(
            "Store a new memory or overwrite an existing one. Title "
            "and keywords are normalised (spaces → `_`, punctuation "
            "dropped); the canonical key is reported back.\n"
            "Arguments are PLAIN JSON — no XML tags, no markup.\n"
            "Example: {\"title\":\"BitNet\","
            "\"keywords\":[\"BitNet\",\"quantization\"],"
            "\"content\":\"...\"}")
        .param("title",    "string",
               "1..64 chars after normalisation.", true)
        .param("keywords", "array",
               "1..24 short strings. Lead with the must-have term — "
               "search treats keywords[0] as required.", true)
        .param("content",  "string",
               "Memory body. UTF-8, capped at 256 KB.", true)
        .param("fix",      "boolean",
               "Immutable + `fix-easyai-` prefix. Default false.",
               false)
        .handle(h_save)
        .build());

    out.push_back(Tool::builder("memory_append")
        .describe(
            "Append to an existing memory (a Markdown `---` "
            "separates each addition). If the title does not "
            "exist, creates a new memory (keywords required). "
            "Returns whether the memory was saved (new) or "
            "updated (appended). Refused on fix-easyai-*.")
        .param("title",    "string",
               "Memory title — existing or new.", true)
        .param("content",  "string",
               "Text added after the current body (or full body "
               "if new).", true)
        .param("keywords", "array",
               "Extra keywords merged in (deduped, cap 24). "
               "Required when creating a new memory.", false)
        .handle(h_append)
        .build());

    out.push_back(Tool::builder("memory_search")
        .describe(
            "Find memories by keyword. Returns ranked matches "
            "tagged `[matched N/M]`. Search memory FIRST before "
            "the web — then also web_search for freshness. Update "
            "memory with any durable new facts the web provided.\n"
            "Arguments are PLAIN JSON — no XML tags, no markup.\n"
            "Example: {\"keywords\":[\"BitNet\",\"binary\"]}\n"
            "\n"
            "CITATION: if you use retrieved content in your reply, "
            "it MUST end with a `Sources:` block citing "
            "`memory: \"<title>\"` per entry.")
        .param("keywords",    "array",
               "1..24 strings. keywords[0] is required (every hit "
               "carries it); the rest rank.", true)
        .param("max_results", "integer",
               "Page size, default 10, max 20.", false)
        .param("page",        "integer",
               "1-based, default 1.", false)
        .handle(h_search)
        .build());

    out.push_back(Tool::builder("memory_load")
        .describe(
            "Read the full content of 1..20 memories by exact title.\n"
            "Arguments are PLAIN JSON — no XML tags, no markup.\n"
            "Example: {\"titles\":[\"BitNet\",\"Preferences\"]}\n"
            "\n"
            "CITATION: if you use loaded content in your reply, "
            "it MUST end with a `Sources:` block citing "
            "`memory: \"<title>\"` per entry.")
        .param("titles", "array",
               "1..20 exact titles.", true)
        .handle(h_load)
        .build());

    out.push_back(Tool::builder("memory_list")
        .describe(
            "Titles only (no bodies). Pass prefix=\"fix-easyai-\" "
            "for immutable memories.")
        .param("prefix", "string",
               "Filter titles by prefix.", false)
        .param("max",    "integer",
               "Result cap, default 50, max 200.", false)
        .handle(h_list)
        .build());

    out.push_back(Tool::builder("memory_delete")
        .describe(
            "Forget a memory. fix-easyai-* are immutable and cannot "
            "be deleted here.")
        .param("title", "string",
               "Exact title.", true)
        .handle(h_delete)
        .build());

    out.push_back(Tool::builder("memory_keywords")
        .describe(
            "Vocabulary overview — every distinct keyword + count. "
            "Call before save/search when unsure what vocabulary "
            "you've already established.")
        .param("min_count", "integer",
               "Hide keywords used by fewer than N memories. Default "
               "1.", false)
        .param("max",       "integer",
               "Result cap, default 200, max 500.", false)
        .handle(h_keywords)
        .build());

    return out;
}

// ---------------------------------------------------------------------------
// Compact vocabulary snapshot for per-prompt injection.
//
// Reuses build_rag_store to load the keyword index off disk on every
// call (no persistent state held by this function — callers that
// already have a live RagStore via make_rag_tool pay the duplication
// in exchange for the simpler API). Fine for the hot path since the
// directory scan is O(N files) at ~10-50ms for typical stores, while
// the inference that follows takes seconds.
//
// Empty store → empty string; the caller decides whether to inject.
// Caps at the top kCap keywords (currently 40) so token cost stays
// bounded even for very large memories. The sort key is
// (count desc, name asc) — same order memory(action="keywords") uses,
// so the model sees a familiar ranking.
std::string render_memory_vocabulary(const std::string & root_dir) {
    if (root_dir.empty()) return std::string();

    auto store = build_rag_store(root_dir);

    std::map<std::string, std::size_t> counts;
    std::size_t total_entries = 0;
    {
        std::shared_lock<std::shared_mutex> lock(store->mu);
        total_entries = store->index.size();
        for (const auto & [_, m] : store->index) {
            for (const auto & k : m.keywords) {
                counts[k] += 1;
            }
        }
    }
    if (counts.empty()) return std::string();

    struct Row { std::string keyword; std::size_t count; };
    std::vector<Row> rows;
    rows.reserve(counts.size());
    for (const auto & [k, n] : counts) rows.push_back({k, n});
    std::sort(rows.begin(), rows.end(), [](const Row & a, const Row & b) {
        if (a.count != b.count) return a.count > b.count;
        return a.keyword < b.keyword;
    });

    constexpr std::size_t kCap = 40;
    const std::size_t total_kw = rows.size();
    const bool truncated = total_kw > kCap;
    if (truncated) rows.resize(kCap);

    std::ostringstream o;
    o << total_entries << " entr"
      << (total_entries == 1 ? "y" : "ies")
      << " (most-common first; use your memory-search tool with these "
      << "keywords to recall — the exact callable name is in your "
      << "AVAILABLE TOOLS list):\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i) o << ' ';
        o << rows[i].keyword << '(' << rows[i].count << ')';
    }
    if (truncated) {
        o << " …(+" << (total_kw - kCap)
          << " more; use the memory-keywords tool for the full list)";
    }
    return o.str();
}

}  // namespace easyai::tools
