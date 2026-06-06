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
#include <cstdint>
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

// Filename prefix that marks an entry as IMMUTABLE. Files starting
// with `fix-` cannot be overwritten or deleted by the model.
constexpr char        kFixedPrefix[]    = "fix-";
constexpr std::size_t kFixedPrefixLen   = sizeof(kFixedPrefix) - 1;

bool key_is_fixed(const std::string & key) {
    return key.size() > kFixedPrefixLen
        && key.compare(0, kFixedPrefixLen, kFixedPrefix) == 0;
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
// Keyword normalization — the new entry-identity model.
// ---------------------------------------------------------------------------
// Keywords are the sole identifier: sorted + joined by `_` = filename stem.
// Input is split on `_ , / <space> \t`, each token lowercased and stripped
// to alphanumeric + `-.+`.

std::string normalize_keyword(const std::string & s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c))                       out += (char) std::tolower(c);
        else if (c == '-' || c == '.' || c == '+')  out += (char) c;
    }
    while (!out.empty() && !std::isalnum((unsigned char) out.back()))
        out.pop_back();
    return out;
}

std::vector<std::string> split_and_normalize_keywords(const std::string & raw) {
    std::vector<std::string> out;
    auto is_sep = [](char c) {
        return c == '_' || c == ' ' || c == ',' || c == '/'
            || c == '\t' || c == '\r' || c == '\n';
    };
    std::string cur;
    for (char c : raw) {
        if (is_sep(c)) {
            std::string n = normalize_keyword(cur);
            if (!n.empty()) out.push_back(std::move(n));
            cur.clear();
        } else {
            cur += c;
        }
    }
    std::string n = normalize_keyword(cur);
    if (!n.empty()) out.push_back(std::move(n));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string keywords_to_key(const std::vector<std::string> & kw) {
    std::string key;
    for (std::size_t i = 0; i < kw.size(); ++i) {
        if (i > 0) key += '_';
        key += kw[i];
    }
    return key;
}

// Human-readable keyword phrase for result messages — space-joined in
// the caller's order. Keeps tool output reading as a piece of knowledge
// ("learned \"python async\"") rather than a filename.
std::string keywords_display(const std::vector<std::string> & kw) {
    std::string s;
    for (std::size_t i = 0; i < kw.size(); ++i) {
        if (i > 0) s += ' ';
        s += kw[i];
    }
    return s;
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
// The index is populated EAGERLY by `build_rag_store()` under a unique
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
// Lenient keyword/title-list parser. Accepts ANY of these shapes:
//
//   1. JSON array:      "keywords": ["a", "b", "c"]
//   2. Stringified arr: "keywords": "[\"a\", \"b\"]"
//   3. Plain string:    "keywords": "a, b, c"
//                        "keywords": "a b c"
//                        "keywords": "a/b/c"
//                        "keywords": "a.b.c"
//
// Shape 3 splits on comma, space, slash, or dot — the delimiters
// weaker models naturally reach for. Mixed delimiters work too
// ("a, b/c d.e" → ["a","b","c","d","e"]). Empty tokens after
// splitting are silently dropped.
// ---------------------------------------------------------------------------
static std::vector<std::string> split_delimited(const std::string & s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ' ' || c == '/' || c == '.') {
            auto t = trim_inline(cur);
            if (!t.empty()) out.push_back(std::move(t));
            cur.clear();
        } else {
            cur += c;
        }
    }
    auto t = trim_inline(cur);
    if (!t.empty()) out.push_back(std::move(t));
    return out;
}

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

        // Plain string → split on delimiters (comma, space, slash, dot).
        // Also handles stringified arrays: if the string parses as a
        // JSON array we use that instead.
        if (v.is_string()) {
            std::string s = v.get<std::string>();
            try {
                json reparsed = json::parse(s);
                if (reparsed.is_array()) { v = std::move(reparsed); goto as_array; }
            } catch (const std::exception &) {}
            out = split_delimited(s);
            return true;
        }

    as_array:
        if (!v.is_array()) {
            err = "argument " + key + ": expected a string like "
                  "\"alpha, beta\" or an array [\"alpha\", \"beta\"]";
            return false;
        }
        out.clear();
        out.reserve(v.size());
        for (const auto & e : v) {
            if (e.is_string()) {
                out.push_back(e.get<std::string>());
            } else {
                err = "argument " + key + ": every element must be a string";
                return false;
            }
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

// Parse keywords from tool arguments: handles JSON arrays, stringified
// arrays, and plain strings. Returns sorted, deduped, normalized keywords.
bool parse_keywords_arg(const std::string & args_json,
                        std::vector<std::string> & out,
                        std::string & err) {
    std::vector<std::string> raw;
    if (!parse_string_array(args_json, "keywords", raw, err)) return false;
    out.clear();
    for (const auto & r : raw) {
        for (const auto & kw : split_and_normalize_keywords(r))
            out.push_back(kw);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return true;
}

// ---------------------------------------------------------------------------
// Tool handler factories
// ---------------------------------------------------------------------------

ToolHandler make_save_handler(std::shared_ptr<RagStore> store) {
    return [store](const ToolCall & c) -> ToolResult {
        std::vector<std::string> keywords;
        std::string err;
        if (!parse_keywords_arg(c.arguments_json, keywords, err))
            return ToolResult::error(err);
        if (keywords.empty())
            return ToolResult::error(
                "keywords required (e.g. \"python async sockets\").");
        if (keywords.size() > kMaxKeywordsPerEntry)
            keywords.resize(kMaxKeywordsPerEntry);

        std::string key = keywords_to_key(keywords);

        bool fix = false;
        args::get_bool(c.arguments_json, "fix", fix);
        if (fix && !key_is_fixed(key))
            key = std::string(kFixedPrefix) + key;

        std::string content;
        if (!args::get_string(c.arguments_json, "content", content))
            return ToolResult::error("missing required argument: content");
        if (content.size() > kMaxContentBytes)
            return ToolResult::error(
                "content exceeds " + std::to_string(kMaxContentBytes)
                + " bytes; split into multiple entries.");

        std::unique_lock<std::shared_mutex> lock(store->mu);

        if (key_is_fixed(key) && store->index.count(key) > 0)
            return ToolResult::error(
                "\"" + keywords_display(keywords) + "\" is pinned — cannot overwrite.");

        if (!store->save_locked(key, keywords, content, err))
            return ToolResult::error(err);

        std::ostringstream o;
        o << "learned \"" << keywords_display(keywords) << "\" ("
          << keywords.size() << " keyword" << (keywords.size() == 1 ? "" : "s")
          << (key_is_fixed(key) ? ", pinned" : "") << ")";
        return ToolResult::ok(o.str());
    };
}

ToolHandler make_append_handler(std::shared_ptr<RagStore> store) {
    return [store](const ToolCall & c) -> ToolResult {
        std::vector<std::string> keywords;
        std::string err;
        if (!parse_keywords_arg(c.arguments_json, keywords, err))
            return ToolResult::error(err);
        if (keywords.empty())
            return ToolResult::error(
                "keywords required (e.g. \"python async\").");
        if (keywords.size() > kMaxKeywordsPerEntry)
            keywords.resize(kMaxKeywordsPerEntry);

        std::string key = keywords_to_key(keywords);

        std::string suffix;
        if (!args::get_string(c.arguments_json, "content", suffix))
            return ToolResult::error("missing required argument: content");
        if (suffix.empty())
            return ToolResult::error("content is empty — nothing to append.");

        std::unique_lock<std::shared_mutex> lock(store->mu);

        const bool is_new = (store->index.count(key) == 0);
        if (is_new) {
            if (suffix.size() > kMaxContentBytes)
                return ToolResult::error(
                    "content exceeds " + std::to_string(kMaxContentBytes) + " bytes.");
            if (!store->save_locked(key, keywords, suffix, err))
                return ToolResult::error(err);
            std::ostringstream o;
            o << "learned \"" << keywords_display(keywords) << "\" (new)";
            return ToolResult::ok(o.str());
        }

        if (key_is_fixed(key))
            return ToolResult::error(
                "\"" + keywords_display(keywords) + "\" is pinned — cannot add to it.");

        std::vector<std::string> old_keywords;
        std::string              old_body;
        std::int64_t             old_mtime = 0;
        if (!store->load_one(key, old_keywords, old_body, old_mtime, err))
            return ToolResult::error(err);

        std::string merged = old_body;
        while (!merged.empty() && (merged.back() == '\n' || merged.back() == '\r'))
            merged.pop_back();
        if (!merged.empty()) merged += "\n\n---\n\n";
        merged += suffix;

        if (merged.size() > kMaxContentBytes)
            return ToolResult::error(
                "merged content would exceed "
                + std::to_string(kMaxContentBytes) + " bytes.");

        if (!store->save_locked(key, old_keywords, merged, err))
            return ToolResult::error(err);

        std::ostringstream o;
        o << "expanded \"" << keywords_display(keywords) << "\" — "
          << "added to what you already knew";
        return ToolResult::ok(o.str());
    };
}

ToolHandler make_search_handler(std::shared_ptr<RagStore> store) {
    return [store](const ToolCall & c) -> ToolResult {
        std::vector<std::string> keywords;
        std::string err;
        if (!parse_keywords_arg(c.arguments_json, keywords, err))
            return ToolResult::error(err);
        if (keywords.empty())
            return ToolResult::error(
                "keywords required (e.g. \"python async\").");

        long long max_results = (long long) kSearchResultsDflt;
        args::get_int(c.arguments_json, "max_results", max_results);
        if (max_results < 1) max_results = 1;
        if ((std::size_t) max_results > kSearchResultsMax)
            max_results = (long long) kSearchResultsMax;

        struct Hit {
            std::string key;
            EntryMeta   meta;
            std::size_t matched = 0;
            std::vector<std::string> matched_keywords;
        };
        std::vector<Hit> hits;
        {
            std::shared_lock<std::shared_mutex> lock(store->mu);
            for (const auto & [k, m] : store->index) {
                Hit h;
                h.key  = k;
                h.meta = m;
                for (const auto & q : keywords) {
                    for (const auto & ek : m.keywords) {
                        if (ek == q) {
                            h.matched_keywords.push_back(q);
                            break;
                        }
                    }
                }
                h.matched = h.matched_keywords.size();
                if (h.matched > 0)
                    hits.push_back(std::move(h));
            }
        }
        std::sort(hits.begin(), hits.end(), [](const Hit & a, const Hit & b) {
            if (a.matched != b.matched) return a.matched > b.matched;
            return a.meta.modified_unix > b.meta.modified_unix;
        });

        long long page = 1;
        args::get_int(c.arguments_json, "page", page);
        if (page < 1) page = 1;

        const std::size_t total    = hits.size();
        const std::size_t per_page = (std::size_t) max_results;
        const std::size_t pages    =
            total == 0 ? 0 : (total + per_page - 1) / per_page;
        const std::size_t off      =
            (std::size_t)((page - 1) * (long long) per_page);

        if (hits.empty()) {
            return ToolResult::ok(
                "no matches. Use knowledge_browse to see everything you know.");
        }
        if (off >= total) {
            return ToolResult::ok(
                "page " + std::to_string(page) + " is past the end ("
                + std::to_string(pages) + " pages).");
        }

        const std::size_t end     = std::min(off + per_page, total);
        const bool        more    = end < total;

        std::ostringstream o;
        o << "results: " << total
          << "  page: " << page << "/" << pages << "\n\n";

        for (std::size_t i = off; i < end; ++i) {
            const auto & h = hits[i];
            std::vector<std::string> bk;
            std::string              body;
            std::int64_t             mt = 0;
            std::string              le;
            std::string preview;
            if (store->load_one(h.key, bk, body, mt, le))
                preview = make_preview(body, kSearchPreviewBytes);
            else
                preview = "(read error)";

            o << (i + 1) << ". [" << h.key << "]";
            if (key_is_fixed(h.key)) o << " (pinned)";
            o << "  matched " << h.matched << "/" << keywords.size() << "\n";
            o << "   keywords: ";
            for (std::size_t k = 0; k < h.meta.keywords.size(); ++k) {
                if (k) o << " ";
                o << h.meta.keywords[k];
            }
            o << "\n   " << preview << "\n\n";
        }
        if (more)
            o << "next: knowledge_search with page=" << (page + 1) << "\n";
        o << "use knowledge_recall with the same keywords for the full content.\n";
        return ToolResult::ok(o.str());
    };
}

ToolHandler make_load_handler(std::shared_ptr<RagStore> store) {
    return [store](const ToolCall & c) -> ToolResult {
        std::vector<std::string> keywords;
        std::string err;
        if (!parse_keywords_arg(c.arguments_json, keywords, err))
            return ToolResult::error(err);
        if (keywords.empty())
            return ToolResult::error(
                "keywords required (e.g. \"python async\").");

        std::string key = keywords_to_key(keywords);

        std::vector<std::string> file_kw;
        std::string body;
        std::int64_t mtime = 0;
        {
            std::shared_lock<std::shared_mutex> lock(store->mu);
            if (!store->load_one(key, file_kw, body, mtime, err))
                return ToolResult::error(err);
        }

        std::ostringstream o;
        o << "--- " << key << " ---\n";
        o << "keywords:";
        for (const auto & k : file_kw) o << " " << k;
        o << "\nmodified: " << format_local_time(mtime) << "\n";
        if (key_is_fixed(key)) o << "pinned: yes\n";
        o << "\n" << body;
        if (!body.empty() && body.back() != '\n') o << '\n';
        return ToolResult::ok(o.str());
    };
}

ToolHandler make_list_handler(std::shared_ptr<RagStore> store) {
    return [store](const ToolCall & c) -> ToolResult {
        std::string prefix_raw;
        args::get_string(c.arguments_json, "prefix", prefix_raw);
        std::string prefix = normalize_keyword(prefix_raw);

        long long max = (long long) kListResultsDflt;
        args::get_int(c.arguments_json, "max", max);
        if (max < 1) max = 1;
        if ((std::size_t) max > kListResultsMax) max = (long long) kListResultsMax;

        struct Row { std::string key; EntryMeta meta; };
        std::vector<Row> rows;
        {
            std::shared_lock<std::shared_mutex> lock(store->mu);
            for (const auto & [k, m] : store->index) {
                if (!prefix.empty() &&
                    (k.size() < prefix.size() ||
                     k.compare(0, prefix.size(), prefix) != 0))
                    continue;
                rows.push_back({ k, m });
                if ((long long) rows.size() >= max) break;
            }
        }

        if (rows.empty())
            return ToolResult::ok(prefix.empty()
                ? "nothing learned yet. Use knowledge_learning to remember something."
                : "nothing learned matches prefix \"" + prefix + "\".");

        std::ostringstream o;
        o << rows.size() << " topic" << (rows.size() == 1 ? "" : "s")
          << ":\n\n";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto & r = rows[i];
            o << (i + 1) << ". " << r.key;
            if (key_is_fixed(r.key)) o << " (pinned)";
            o << "  [";
            for (std::size_t k = 0; k < r.meta.keywords.size(); ++k) {
                if (k) o << " ";
                o << r.meta.keywords[k];
            }
            o << "]  " << format_local_time(r.meta.modified_unix) << "\n";
        }
        return ToolResult::ok(o.str());
    };
}

ToolHandler make_delete_handler(std::shared_ptr<RagStore> store) {
    return [store](const ToolCall & c) -> ToolResult {
        std::vector<std::string> keywords;
        std::string err;
        if (!parse_keywords_arg(c.arguments_json, keywords, err))
            return ToolResult::error(err);
        if (keywords.empty())
            return ToolResult::error(
                "keywords required (e.g. \"python async\").");

        std::string key = keywords_to_key(keywords);

        if (key_is_fixed(key))
            return ToolResult::error(
                "\"" + keywords_display(keywords) + "\" is pinned — cannot forget it.");

        std::unique_lock<std::shared_mutex> lock(store->mu);
        bool existed = false;
        if (!store->delete_locked(key, existed, err))
            return ToolResult::error(err);
        if (!existed)
            return ToolResult::ok(
                "nothing known about \"" + keywords_display(keywords) + "\" — nothing to forget.");
        return ToolResult::ok("forgot \"" + keywords_display(keywords) + "\"");
    };
}

// Vocabulary overview — keyword usage counts across all entries.
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
                o << "nothing learned yet. Use knowledge_learning to remember something.";
            } else if (min_count > 1) {
                o << "no keywords reach min_count=" << min_count
                  << ". You know " << total_entries
                  << " topic" << (total_entries == 1 ? "" : "s")
                  << " but every keyword is below the threshold. "
                  << "Try min_count=1 (default) to see the full list.";
            } else {
                o << "no keywords found. Some knowledge may be untagged "
                  << "(no `keywords:` header) — those don't appear here. "
                  << "Use knowledge_browse to see them.";
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
// Public entry point — knowledge_split_tools
// ---------------------------------------------------------------------------
// Seven single-responsibility tools, named so the model treats the store
// as MEMORY rather than a filesystem (no save/load/delete file verbs).
// The knowledge_ prefix groups them in the flat tool list:
//   knowledge_learning       — remember a piece of knowledge  (make_save_handler)
//   knowledge_learning_more  — add to existing knowledge       (make_append_handler)
//   knowledge_search         — find knowledge by keywords       (make_search_handler)
//   knowledge_recall         — return a topic's full content     (make_load_handler)
//   knowledge_browse         — list all remembered topics         (make_list_handler)
//   knowledge_forget         — drop a piece of knowledge           (make_delete_handler)
//   knowledge_keywords       — show the keyword vocabulary          (make_keywords_handler)
// Each handler reads its parameters straight out of `arguments_json`.
// ----------------------------------------------------------------------------
std::vector<Tool> knowledge_split_tools(std::string root_dir) {
    auto store = build_rag_store(std::move(root_dir));

    std::vector<Tool> out;
    out.reserve(7);

    out.push_back(Tool::builder("knowledge_learning")
        .describe(
            "Remember a piece of knowledge for later recall. Keywords "
            "label it and make it findable. Learn AFTER answering the "
            "user.\n"
            "Store ONLY knowledge and information — facts, concepts, "
            "decisions, how-tos. This is your memory, NOT a file store: "
            "never put files or file content here, EVER. To read or "
            "write files use the fs tool.\n"
            "Example: {\"keywords\": \"python async\", "
            "\"content\": \"Use asyncio for concurrent IO.\"}")
        .param("keywords", "string",  "Keywords that label this knowledge, e.g. \"python async sockets\".", true)
        .param("content",  "string",  "The knowledge to remember.", true)
        .param("fix",      "boolean", "Pin it so it can't be overwritten or forgotten (default false).", false)
        .handle(make_save_handler(store))
        .build());

    out.push_back(Tool::builder("knowledge_learning_more")
        .describe(
            "Add more to something you already learned (learns it fresh "
            "if new). Keywords pick which knowledge to extend.\n"
            "Add ONLY knowledge and information — never files or file "
            "content, EVER. To read or write files use the fs tool.\n"
            "Example: {\"keywords\": \"python async\", "
            "\"content\": \"Also supports gather().\"}")
        .param("keywords", "string", "Keywords of the knowledge to extend.", true)
        .param("content",  "string", "Knowledge to add.", true)
        .handle(make_append_handler(store))
        .build());

    out.push_back(Tool::builder("knowledge_search")
        .describe(
            "Search your knowledge by keywords. Returns ranked matches "
            "with previews.\n"
            "Example: {\"keywords\": \"python\"}")
        .param("keywords",    "string",  "Search keywords, e.g. \"python async\".", true)
        .param("max_results", "integer", "Results per page (default 10, max 20).", false)
        .param("page",        "integer", "Page number (default 1).", false)
        .handle(make_search_handler(store))
        .build());

    out.push_back(Tool::builder("knowledge_recall")
        .describe(
            "Recall everything you know on a topic — returns the full "
            "remembered content. Keywords pick which.\n"
            "Example: {\"keywords\": \"python async\"}")
        .param("keywords", "string", "Keywords of the knowledge to recall (same used to learn it).", true)
        .handle(make_load_handler(store))
        .build());

    out.push_back(Tool::builder("knowledge_browse")
        .describe(
            "Browse everything you've learned — lists all remembered "
            "topics. Optionally filter by keyword prefix.\n"
            "Example: {}")
        .param("prefix", "string",  "Filter remembered topics by keyword prefix.", false)
        .param("max",    "integer", "Max results (default 50).", false)
        .handle(make_list_handler(store))
        .build());

    out.push_back(Tool::builder("knowledge_forget")
        .describe(
            "Forget a piece of knowledge. Pinned knowledge is protected "
            "and cannot be forgotten.\n"
            "Example: {\"keywords\": \"python async\"}")
        .param("keywords", "string", "Keywords of the knowledge to forget.", true)
        .handle(make_delete_handler(store))
        .build());

    out.push_back(Tool::builder("knowledge_keywords")
        .describe(
            "Show all keywords across everything you've learned — your "
            "knowledge vocabulary.\n"
            "Example: {}")
        .param("min_count", "integer", "Hide keywords used fewer than N times (default 1).", false)
        .param("max",       "integer", "Max results (default 200).", false)
        .handle(make_keywords_handler(store))
        .build());

    return out;
}

// ---------------------------------------------------------------------------
// Compact vocabulary snapshot for per-prompt injection.
//
// Hot path on `easyai-server`: re-rendered for EVERY chat request so
// the model always sees the current keyword index.  A naïve impl
// re-scans the directory each call (~10-50ms on typical stores, more
// on large ones — measurable next to inference start).
//
// Cache strategy: keyed on (root_dir, directory mtime, file count).
// The mtime of the memory root tracks atomic save/append/delete
// because those all rename(2) a file into the directory, which bumps
// the dir's mtime.  File count catches the edge case where two saves
// in the same second cancel out the mtime change.  On a hit we return
// the cached string with no disk traversal — cost drops to one
// stat(2) per request.
//
// Cache shape: at most one entry per unique root_dir (the binaries
// only configure one).  Bounded; no eviction policy needed.  Mutex
// guards the map; the rendered string is shared by value so callers
// observe their own copy.
//
// Empty store → empty string; the caller decides whether to inject.
// Caps at the top kCap keywords (currently 40) so token cost stays
// bounded even for very large memories. The sort key is
// (count desc, name asc) — same order memory(action="keywords") uses,
// so the model sees a familiar ranking.
std::string render_knowledge_vocabulary(const std::string & root_dir) {
    if (root_dir.empty()) return std::string();

    namespace fs = std::filesystem;

    // Probe the directory once.  Both signals come from one stat-like
    // syscall path (filesystem::last_write_time + directory_iterator
    // for the count); on a miss we'd do the directory walk anyway, so
    // this stays cheap on the warm path.
    std::int64_t dir_mtime_ns = 0;
    std::size_t  file_count   = 0;
    {
        std::error_code ec;
        auto wt = fs::last_write_time(root_dir, ec);
        if (!ec) {
            dir_mtime_ns = wt.time_since_epoch().count();
        }
        for (auto it = fs::directory_iterator(root_dir, ec);
             !ec && it != fs::directory_iterator{};
             it.increment(ec)) {
            ++file_count;
        }
    }

    struct CacheEntry {
        std::int64_t mtime_ns = 0;
        std::size_t  count    = 0;
        std::string  rendered;
    };
    static std::mutex                            cache_mu;
    static std::map<std::string, CacheEntry>     cache;

    {
        std::lock_guard<std::mutex> g(cache_mu);
        auto it = cache.find(root_dir);
        if (it != cache.end()
            && it->second.mtime_ns == dir_mtime_ns
            && it->second.count    == file_count) {
            return it->second.rendered;        // warm-path copy
        }
    }

    // --- miss: do the full scan ---

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

    std::string rendered;
    if (!counts.empty()) {
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
          << " (most-common first; use your knowledge-search tool with "
          << "these keywords to recall — the exact callable name is "
          << "in your AVAILABLE TOOLS list):\n";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i) o << ' ';
            o << rows[i].keyword << '(' << rows[i].count << ')';
        }
        if (truncated) {
            o << " …(+" << (total_kw - kCap)
              << " more; use the knowledge-keywords tool for the full list)";
        }
        rendered = o.str();
    }

    {
        std::lock_guard<std::mutex> g(cache_mu);
        CacheEntry & e = cache[root_dir];
        e.mtime_ns = dir_mtime_ns;
        e.count    = file_count;
        e.rendered = rendered;
    }
    return rendered;
}

}  // namespace easyai::tools
