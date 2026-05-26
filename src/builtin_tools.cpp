#include "easyai/builtin_tools.hpp"
#include "easyai/log.hpp"
#include "easyai/tool.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>     // PATH_MAX
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <list>
#include <map>
#include <mutex>
#include <unordered_map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

#if defined(EASYAI_HAVE_CURL)
#include <curl/curl.h>
#endif

// nlohmann/json reaches us transitively through llama-common's interface
// includes (vendor copy at llama.cpp/vendor/nlohmann/json.hpp). Used by
// web_google to parse the Custom Search JSON API response.
#include <nlohmann/json.hpp>

namespace easyai::tools {

// `stdfs` rather than the conventional `fs` alias because this file
// also exposes a public `Tool fs(...)` factory in the same namespace,
// and `namespace fs = std::filesystem` would collide with it.
namespace stdfs = std::filesystem;

// ---------------------------------------------------------------- helpers
static std::string trim(std::string s) {
    auto issp = [](unsigned char c){ return std::isspace(c); };
    while (!s.empty() && issp(s.front())) s.erase(s.begin());
    while (!s.empty() && issp(s.back()))  s.pop_back();
    return s;
}

// Strip HTML to plain text without ever touching std::regex.  The
// libstdc++ regex implementation is recursive on backtracking patterns
// like   <(script|style)[^>]*>[\s\S]*?</\1>   and crashes the process
// (SIGSEGV from stack overflow) on adversarial / oversized HTML — which
// is exactly what web_fetch sees when an LLM picks a beefy news page.
// Forward-only scanning + char-by-char whitespace collapse: O(n), zero
// recursion, no stack risk regardless of input.
static std::string strip_html(const std::string & html) {
    auto ieq = [](char a, char b) {
        return std::tolower((unsigned char) a) == std::tolower((unsigned char) b);
    };
    auto starts_with_ci = [&](size_t i, const char * needle) {
        size_t n = std::strlen(needle);
        if (i + n > html.size()) return false;
        for (size_t k = 0; k < n; ++k) {
            if (!ieq(html[i + k], needle[k])) return false;
        }
        return true;
    };

    std::string out;
    out.reserve(html.size());

    size_t i = 0;
    while (i < html.size()) {
        char c = html[i];

        // <script ...>...</script>  and  <style ...>...</style>
        // Skip the entire block, including any tag-internal noise, by
        // searching forward for the closing tag.  If we never find it
        // (truncated HTML), drop the rest.
        if (c == '<' && i + 1 < html.size()) {
            const char * tag = nullptr;
            const char * end = nullptr;
            if (starts_with_ci(i + 1, "script")
                    && (i + 7 >= html.size() ||
                        !std::isalnum((unsigned char) html[i + 7]))) {
                tag = "<script"; end = "</script>";
            } else if (starts_with_ci(i + 1, "style")
                    && (i + 6 >= html.size() ||
                        !std::isalnum((unsigned char) html[i + 6]))) {
                tag = "<style"; end = "</style>";
            }
            if (tag) {
                size_t after_open = i + 1;
                // skip until the '>' of the opening tag
                while (after_open < html.size() && html[after_open] != '>') {
                    ++after_open;
                }
                if (after_open >= html.size()) break;
                // search closing tag (case-insensitive)
                size_t end_len = std::strlen(end);
                size_t j = after_open + 1;
                bool found = false;
                while (j + end_len <= html.size()) {
                    if (html[j] == '<' && starts_with_ci(j, end)) {
                        i = j + end_len;
                        found = true;
                        break;
                    }
                    ++j;
                }
                if (!found) break;        // unterminated → drop the rest
                out += ' ';
                continue;
            }
            // Generic tag: skip to next '>'.  Tags don't nest.
            size_t j = i + 1;
            while (j < html.size() && html[j] != '>') ++j;
            i = (j < html.size()) ? j + 1 : html.size();
            out += ' ';
            continue;
        }

        // Entity decode — small fixed table.
        if (c == '&') {
            struct E { const char * from; const char * to; };
            static const E ents[] = {
                {"&nbsp;", " "}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
                {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"},
            };
            bool matched = false;
            for (const auto & e : ents) {
                size_t flen = std::strlen(e.from);
                if (i + flen <= html.size()
                        && html.compare(i, flen, e.from) == 0) {
                    out += e.to;
                    i += flen;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }

        // Whitespace collapse: turn any run of [ \t\r\n] into a single space.
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!out.empty() && out.back() != ' ') out += ' ';
            ++i;
            continue;
        }

        out += c;
        ++i;
    }

    return trim(out);
}

static std::string clip(std::string s, size_t n) {
    if (s.size() > n) {
        s.resize(n);
        s += "\n…[truncated]";
    }
    return s;
}

// Sanitize one chunk of child-bash output for safe display on the
// operator's terminal. We allow CR / LF / TAB through (formatting), but
// strip every other control byte and the ESC character — preventing a
// model's bash command from emitting ANSI/VT/iTerm2 escape sequences
// that hijack the operator's terminal (window-title injection, screen
// wipe, key-rebind, OSC payloads). The model's own copy of the output
// is unaffected; this only governs what goes to fd=2.
static std::string sanitize_for_operator_tty(const char * buf, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c == '\n' || c == '\r' || c == '\t') {
            out += static_cast<char>(c);
        } else if (c == 0x1b) {
            // Hard-strip ESC. Render a visible marker so the operator
            // notices the model tried to emit an escape sequence
            // (rather than silently swallowing — useful for debugging
            // a misbehaving prompt).
            out += "^[";
        } else if (c < 0x20 || c == 0x7f) {
            // Other C0 controls + DEL: drop. Keeping them risks
            // partial-sequence reassembly into something hostile.
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

static std::string url_encode(const std::string & v) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(v.size() * 3);
    for (unsigned char c : v) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// Decodes a percent-encoded form-urlencoded string ('+' → space, '%XX' → byte).
// Tolerant of malformed input — leaves bad sequences as-is rather than failing.
static std::string url_decode(const std::string & s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < s.size() &&
                   std::isxdigit((unsigned char) s[i + 1]) &&
                   std::isxdigit((unsigned char) s[i + 2])) {
            char hex[3] = { s[i + 1], s[i + 2], 0 };
            out += static_cast<char>(std::strtol(hex, nullptr, 16));
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

// DuckDuckGo wraps result URLs in `/l/?uddg=ENCODED_URL[&rut=...]` redirects.
// If `href` carries that wrapper, extract and decode the uddg param.
// Falls back to returning the href unchanged (or with the protocol prefixed if
// it's protocol-relative) when there's no wrapper.
static std::string decode_ddg_redirect(const std::string & href) {
    static const std::string marker = "uddg=";
    size_t k = href.find(marker);
    if (k == std::string::npos) {
        if (href.compare(0, 4, "http") == 0) return href;
        if (href.compare(0, 2, "//")  == 0)  return "https:" + href;
        return href;
    }
    k += marker.size();
    size_t end = href.find('&', k);
    std::string enc = href.substr(k, end == std::string::npos
                                       ? std::string::npos
                                       : end - k);
    return url_decode(enc);
}

#if defined(EASYAI_HAVE_CURL)

// Capped write callback — bails out the moment the body exceeds the
// configured max.  We can't trust the server's Content-Length (or
// chunked-transfer absence of one), so policing in the callback is the
// only way to keep RAM bounded against an adversarial endpoint.
struct HttpSink {
    std::string * body;
    size_t        max_bytes;
    bool          truncated = false;
};

static size_t curl_write_cb(void * buf, size_t sz, size_t n, void * ud) {
    auto * sink = static_cast<HttpSink *>(ud);
    const size_t incoming = sz * n;
    const size_t already  = sink->body->size();
    if (already >= sink->max_bytes) {
        // Already at the cap — pretend we accepted to keep curl happy
        // (returning 0 here would force curl to abort with an error,
        // which we don't want — we just want to truncate).
        sink->truncated = true;
        return incoming;
    }
    const size_t room = sink->max_bytes - already;
    const size_t take = (incoming <= room) ? incoming : room;
    sink->body->append(static_cast<char *>(buf), take);
    if (take < incoming) sink->truncated = true;
    return incoming;
}

// Microsoft Edge on Windows 11 — current-stable User-Agent string.  Many
// sites (Cloudflare-fronted, news outlets, search engines) gate or
// degrade content for "easyai/0.1" and similar non-browser UAs; this
// pretends to be a vanilla Edge install so the bot heuristics treat us
// like an interactive user.  Bumped manually as Edge releases — kept
// in one place so the web tool's search and fetch actions share the same persona.
static const char * const kEdgeUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36 Edg/130.0.0.0";

// Google Chrome on Windows 11 — current-stable User-Agent.  Kept for
// reference; current search code prefers kNetscapeUserAgent (see below)
// because the modern Chrome persona triggers DDG/Bing/Brave anti-bot
// gating from server IPs while a vintage Netscape UA gets the lo-fi
// HTML / no-JS path the search engines maintain for legacy clients.
static const char * const kChromeUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/133.0.0.0 Safari/537.36";

// Netscape Communicator 4.79 on Windows 2000 — the persona used for ALL
// search backends (web_search_ddg, web_search_ddg_lite, web_search_bing,
// web_search_brave). The reasoning: every keyless search endpoint we
// scrape now treats a contemporary Chrome/Edge UA as suspect (DDG
// returns its "anomaly" page, Brave throttles harder, Bing degrades the
// HTML) but maintains a no-JS / accessibility path for clients that
// obviously can't run a JS challenge. Posing as a 2001-era browser
// trips that path: the search engines serve the simple table-based
// results without a captcha challenge. Verified bypassing the DDG
// "anomaly" wall on html.duckduckgo.com from server IPs that get
// blocked under the modern UA.
//
// NOT used by web_handle_fetch — that path keeps kEdgeUserAgent because
// (a) the model usually wants the modern version of a fetched page,
// (b) some sites refuse to serve content at all to a Netscape UA, and
// (c) fetch isn't the surface bot-gating us.
static const char * const kNetscapeUserAgent =
    "Mozilla/4.79 [en] (Windows NT 5.0; U)";

// Companion headers Edge always sends.  Appended to whatever header list
// the caller is already building so we don't clobber Content-Type for
// the search POST.
static curl_slist * append_edge_browser_headers(curl_slist * headers) {
    headers = curl_slist_append(headers,
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
        "image/avif,image/webp,image/apng,*/*;q=0.8,"
        "application/signed-exchange;v=b3;q=0.7");
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");
    headers = curl_slist_append(headers,
        "sec-ch-ua: \"Chromium\";v=\"130\", \"Microsoft Edge\";v=\"130\", "
        "\"Not?A_Brand\";v=\"99\"");
    headers = curl_slist_append(headers, "sec-ch-ua-mobile: ?0");
    headers = curl_slist_append(headers, "sec-ch-ua-platform: \"Windows\"");
    headers = curl_slist_append(headers, "Upgrade-Insecure-Requests: 1");
    headers = curl_slist_append(headers, "Sec-Fetch-Dest: document");
    headers = curl_slist_append(headers, "Sec-Fetch-Mode: navigate");
    headers = curl_slist_append(headers, "Sec-Fetch-Site: none");
    headers = curl_slist_append(headers, "Sec-Fetch-User: ?1");
    return headers;
}

// Netscape-era request headers for ALL search backends. Returned as a
// std::vector<std::string> rather than a curl_slist so it composes
// cleanly with both http_get's extra_headers parameter (slist built
// internally) and the manual slist construction in http_post_form.
//
// What we DON'T send: sec-ch-ua client-hint trio, Sec-Fetch-* metadata,
// Origin/Referer, X-Requested-With, Upgrade-Insecure-Requests. A real
// Netscape 4.79 from 2001 didn't have any of those; sending them would
// give DDG/Bing/Brave a contradictory fingerprint (vintage UA + modern
// metadata) and they'd likely gate us anyway. The whole point of the
// Netscape persona is to look like a browser that genuinely can't run
// modern JS, so sending only the headers that browser would have sent
// is part of the deception.
static std::vector<std::string> netscape_search_headers() {
    return {
        std::string("User-Agent: ") + kNetscapeUserAgent,
        "Accept: text/html, */*",
        "Accept-Language: en-US,en;q=0.9",
    };
}

// SECURITY: refuse non-http(s) URLs at the top of every fetch.  Without
// this gate, the model could ask for `file:///etc/passwd`, `gopher://...`,
// or `dict://internal`, all of which curl supports by default.  We also
// pin CURLOPT_PROTOCOLS to belt-and-braces the same restriction at the
// transport layer.
static bool url_is_safe_scheme(const std::string & url) {
    auto lower_starts_with = [&](const char * p) {
        size_t n = std::strlen(p);
        if (url.size() < n) return false;
        for (size_t i = 0; i < n; ++i) {
            char a = url[i], b = p[i];
            if (a >= 'A' && a <= 'Z') a = (char) (a + 32);
            if (a != b) return false;
        }
        return true;
    };
    return lower_starts_with("http://") || lower_starts_with("https://");
}

// Default extra-attempts on transient curl/HTTP failures for web tools.
// Web fetches go to the open internet so transient failures are normal —
// 5 retries gives operator-grade resilience without spending forever on
// a hard-down site.  Each retry logs through easyai::log::error so it
// surfaces on stderr without --verbose.
static constexpr int kWebHttpRetries = 5;

static bool web_curl_retryable(CURLcode rc) {
    switch (rc) {
        case CURLE_COULDNT_CONNECT:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
            return true;
        default:
            return false;
    }
}

static int web_retry_backoff_ms(int attempt) {
    int ms = 250 << (attempt > 4 ? 4 : attempt);
    return ms > 4000 ? 4000 : ms;
}

static bool http_get(const std::string & url,
                     const std::vector<std::string> & extra_headers,
                     std::string & body, std::string & err,
                     long timeout_s = 20, long max_bytes = 2 * 1024 * 1024,
                     int retries = kWebHttpRetries) {
    if (!url_is_safe_scheme(url)) {
        err = "only http:// and https:// URLs are allowed";
        return false;
    }

    CURL * c = curl_easy_init();
    if (!c) { err = "curl_easy_init failed"; return false; }

    body.clear();
    HttpSink sink{ &body, (size_t) max_bytes, false };
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    // CURLOPT_PROTOCOLS / _REDIR_PROTOCOLS deprecated in libcurl 7.85.0 in
    // favour of the *_STR variants. The new ones are enums (not macros), so
    // an `#ifdef CURLOPT_PROTOCOLS_STR` test silently falls through; gate on
    // the version macro instead.
#if defined(LIBCURL_VERSION_NUM) && LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR,         "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR,   "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS,
                     (long) (CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS,
                     (long) (CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
    // Impersonate Microsoft Edge — Cloudflare/news/etc gate generic UAs.
    curl_easy_setopt(c, CURLOPT_USERAGENT, kEdgeUserAgent);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");

    curl_slist * headers = nullptr;
    for (const auto & h : extra_headers) headers = curl_slist_append(headers, h.c_str());
    headers = append_edge_browser_headers(headers);
    if (headers) curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);

    const int max_attempts = (retries < 0 ? 0 : retries) + 1;
    CURLcode rc = CURLE_OK;
    long http_code = 0;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        body.clear();
        sink.body      = &body;
        sink.truncated = false;
        rc = curl_easy_perform(c);
        http_code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);

        const bool curl_ok = (rc == CURLE_OK);
        const bool http_ok = (http_code >= 200 && http_code < 300) || http_code == 0;
        if (curl_ok && http_ok) break;

        const bool retryable = (!curl_ok && web_curl_retryable(rc))
                            || (curl_ok && http_code >= 500 && http_code < 600);
        if (!retryable) break;
        if (attempt >= max_attempts) {
            easyai::log::error(
                "[easyai-web] GET %s attempt %d/%d failed (%s) — "
                "retry budget exhausted\n",
                url.c_str(), attempt, max_attempts,
                curl_ok ? ("HTTP " + std::to_string(http_code)).c_str()
                        : curl_easy_strerror(rc));
            break;
        }
        const int backoff = web_retry_backoff_ms(attempt - 1);
        easyai::log::error(
            "[easyai-web] GET %s attempt %d/%d failed (%s); retrying in %dms\n",
            url.c_str(), attempt, max_attempts,
            curl_ok ? ("HTTP " + std::to_string(http_code)).c_str()
                    : curl_easy_strerror(rc),
            backoff);
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
    }
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        err = curl_easy_strerror(rc);
        return false;
    }
    if (http_code >= 400) {
        err = "HTTP " + std::to_string(http_code);
        return false;
    }
    return true;
}

// POST application/x-www-form-urlencoded.  Used by the DDG search backend
// because POST is far less likely than GET to hit the bot/captcha gate.
static bool http_post_form(const std::string & url,
                           const std::string & form_body,
                           std::string & body, std::string & err,
                           long timeout_s = 20,
                           long max_bytes = 4 * 1024 * 1024,
                           int  retries   = kWebHttpRetries) {
    if (!url_is_safe_scheme(url)) {
        err = "only http:// and https:// URLs are allowed";
        return false;
    }
    CURL * c = curl_easy_init();
    if (!c) { err = "curl_easy_init failed"; return false; }

    body.clear();
    HttpSink sink{ &body, (size_t) max_bytes, false };
    curl_easy_setopt(c, CURLOPT_URL,             url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION,  1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS,       5L);
    // CURLOPT_PROTOCOLS / _REDIR_PROTOCOLS deprecated in libcurl 7.85.0 in
    // favour of the *_STR variants. The new ones are enums (not macros), so
    // an `#ifdef CURLOPT_PROTOCOLS_STR` test silently falls through; gate on
    // the version macro instead.
#if defined(LIBCURL_VERSION_NUM) && LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR,         "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR,   "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS,
                     (long) (CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS,
                     (long) (CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(c, CURLOPT_TIMEOUT,         timeout_s);
    // Pose as Netscape 4.79 — html.duckduckgo.com gates the modern
    // Chrome/Edge persona with its anti-bot "anomaly" page from
    // server IPs, but the same query with a vintage UA gets the lo-fi
    // HTML response without challenge. The User-Agent header in the
    // slist below overrides this CURLOPT_USERAGENT setting (libcurl
    // prefers the slist entry); the setopt is here so a default UA
    // is in place even if the slist construction is later changed.
    curl_easy_setopt(c, CURLOPT_USERAGENT,       kNetscapeUserAgent);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,   curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,       &sink);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL,        1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_POST,            1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,      form_body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE,   (long) form_body.size());

    // Form POST + Netscape persona (no Sec-Fetch / client-hint / Origin
    // headers — a real Netscape 4.79 wouldn't have sent them, and the
    // contradictory fingerprint would defeat the point of the vintage UA).
    curl_slist * headers = nullptr;
    headers = curl_slist_append(headers,
        "Content-Type: application/x-www-form-urlencoded");
    for (const auto & h : netscape_search_headers()) {
        headers = curl_slist_append(headers, h.c_str());
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);

    const int max_attempts = (retries < 0 ? 0 : retries) + 1;
    CURLcode rc = CURLE_OK;
    long http_code = 0;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        body.clear();
        sink.body      = &body;
        sink.truncated = false;
        rc = curl_easy_perform(c);
        http_code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);

        const bool curl_ok = (rc == CURLE_OK);
        const bool http_ok = (http_code >= 200 && http_code < 300) || http_code == 0;
        if (curl_ok && http_ok) break;

        const bool retryable = (!curl_ok && web_curl_retryable(rc))
                            || (curl_ok && http_code >= 500 && http_code < 600);
        if (!retryable) break;
        if (attempt >= max_attempts) {
            easyai::log::error(
                "[easyai-web] POST %s attempt %d/%d failed (%s) — "
                "retry budget exhausted\n",
                url.c_str(), attempt, max_attempts,
                curl_ok ? ("HTTP " + std::to_string(http_code)).c_str()
                        : curl_easy_strerror(rc));
            break;
        }
        const int backoff = web_retry_backoff_ms(attempt - 1);
        easyai::log::error(
            "[easyai-web] POST %s attempt %d/%d failed (%s); retrying in %dms\n",
            url.c_str(), attempt, max_attempts,
            curl_ok ? ("HTTP " + std::to_string(http_code)).c_str()
                    : curl_easy_strerror(rc),
            backoff);
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK)   { err = curl_easy_strerror(rc); return false; }
    if (http_code >= 400) { err = "HTTP " + std::to_string(http_code); return false; }
    return true;
}
#endif

// ============================================================================
// datetime
// ============================================================================
Tool datetime() {
    return Tool::builder("datetime")
        .short_describe(
            "Return the current UTC and local date/time. Call for "
            "'now'/'today'/'latest' or before date math.")
        .describe(
            "Current date and time. No parameters.\n"
            "\n"
            "Returns JSON: "
            "{\"utc\":\"YYYY-MM-DDTHH:MM:SSZ\","
            "\"local\":\"YYYY-MM-DDTHH:MM:SS+HHMM\"}.\n"
            "\n"
            "Call before any date math, or when the user says "
            "'now' / 'today' / 'latest'.")
        .handle([](const ToolCall &) {
            using namespace std::chrono;
            auto now = system_clock::now();
            std::time_t t = system_clock::to_time_t(now);
            std::tm utc{}, loc{};
#if defined(_WIN32)
            gmtime_s(&utc, &t);
            localtime_s(&loc, &t);
#else
            gmtime_r(&t, &utc);
            localtime_r(&t, &loc);
#endif
            char b1[64], b2[64];
            std::strftime(b1, sizeof(b1), "%Y-%m-%dT%H:%M:%SZ",      &utc);
            std::strftime(b2, sizeof(b2), "%Y-%m-%dT%H:%M:%S%z",     &loc);
            std::ostringstream o;
            o << "{\"utc\":\"" << b1 << "\",\"local\":\"" << b2 << "\"}";
            return ToolResult::ok(o.str());
        })
        .build();
}

// ============================================================================
// web fetch cache (process-wide, used by web action="fetch")
// ----------------------------------------------------------------------------
// LRU cache for fetched bodies. The model often re-asks for the same URL
// within a single conversation (it digests results, then circles back).
// Caching avoids:
//  - re-hitting the source (politeness + DDG rate-limit avoidance)
//  - waste of latency / tokens on the model side
//
// Cache key:  "<url>?as_html=<bool>"
// Capacity:   16 entries (well within RAM for ~16 KiB clipped bodies)
// TTL:        5 minutes (long enough to survive a multi-hop research,
//                        short enough to keep "live" pages fresh)
// ============================================================================
#if defined(EASYAI_HAVE_CURL)
namespace {

struct WebFetchCache {
    struct Entry {
        std::string                                 body;     // already stripped + clipped
        std::chrono::steady_clock::time_point       inserted;
        bool                                        as_html;
    };
    static constexpr size_t kCapacity = 16;
    static constexpr int    kTtlMs    = 5 * 60 * 1000;  // 5 min

    std::mutex                                  mu;
    std::list<std::pair<std::string, Entry>>    items;             // MRU at front
    std::unordered_map<std::string,
        std::list<std::pair<std::string, Entry>>::iterator> index;

    bool get(const std::string & key, std::string & out_body) {
        std::lock_guard<std::mutex> lock(mu);
        auto it = index.find(key);
        if (it == index.end()) return false;
        const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - it->second->second.inserted).count();
        if (age_ms > kTtlMs) {
            items.erase(it->second);
            index.erase(it);
            return false;
        }
        // promote to MRU
        items.splice(items.begin(), items, it->second);
        out_body = it->second->second.body;
        return true;
    }
    void put(const std::string & key, std::string body, bool as_html) {
        std::lock_guard<std::mutex> lock(mu);
        auto existing = index.find(key);
        if (existing != index.end()) {
            items.erase(existing->second);
            index.erase(existing);
        }
        items.emplace_front(key, Entry{
            std::move(body), std::chrono::steady_clock::now(), as_html});
        index[key] = items.begin();
        while (items.size() > kCapacity) {
            index.erase(items.back().first);
            items.pop_back();
        }
    }
};

WebFetchCache & web_fetch_cache() {
    static WebFetchCache c;
    return c;
}

}  // namespace
#endif

// ============================================================================
// web — unified search + fetch dispatcher
// ----------------------------------------------------------------------------
// Two actions:
//
//   action="search"  query → numbered title/url/snippet list
//                    engine: "ddg" (default, no key) | "google" (CSE; opt-in
//                    + env vars). page-based pagination over the engine's
//                    own ordering.
//
//   action="fetch"   url   → text content (HTML stripped, or raw with
//                    as_html=true). Pagination via start (byte offset) +
//                    limit (window size).
//
// Handlers below are private; the unified Tool factory at the bottom owns
// the schema + dispatches by action. Same flat-everything-optional schema
// shape as the unified rag tool so weak / 1-bit-quant callers don't trip
// on discriminated unions.
// ============================================================================

namespace {

// ---------- search: DuckDuckGo HTML scraper ----------
// POST html.duckduckgo.com/html/ as Netscape 4.79. The modern Chrome
// XHR persona we used to send was reliably gated to DDG's anti-bot
// "anomaly" page from server IPs; switching to the vintage Netscape
// UA + a stripped-down request (no Sec-Fetch / client-hints / Origin)
// gets the lo-fi HTML response instead. Same trick that powers the
// ddg-lite engine — see kNetscapeUserAgent and http_post_form.
//
// DDG wraps result URLs in a tracking redirect
// (`//duckduckgo.com/l/?uddg=ENCODED_URL&rut=...`); we unwrap via
// decode_ddg_redirect() before handing them back so the model gets
// real fetchable URLs.
ToolResult web_search_ddg(const std::string & query, long long page,
                          long long max_results) {
#if !defined(EASYAI_HAVE_CURL)
    (void) query; (void) page; (void) max_results;
    return ToolResult::error(
        "web search unavailable: easyai built without libcurl");
#else
    if (max_results < 1)  max_results = 1;
    if (max_results > 20) max_results = 20;
    if (page < 1)         page = 1;

    const std::string url       = "https://html.duckduckgo.com/html/";
    const std::string post_body = "q=" + url_encode(query) + "&kl=us-en";
    std::string body, err;
    if (!http_post_form(url, post_body, body, err)) {
        return ToolResult::error("search failed: " + err);
    }
    if (body.empty()) {
        return ToolResult::error("search failed: empty response");
    }

    static const std::regex re_title(
        R"DDG(<a[^>]*class\s*=\s*"[^"]*result__a[^"]*"[^>]*href\s*=\s*"([^"]+)"[^>]*>([\s\S]*?)</a>)DDG",
        std::regex::icase);
    static const std::regex re_snippet(
        R"DDG(<(?:a|div)[^>]*class\s*=\s*"[^"]*result__snippet[^"]*"[^>]*>([\s\S]*?)</(?:a|div)>)DDG",
        std::regex::icase);

    // Two-pass: collect ALL parseable results, then page-slice. DDG returns
    // ~30 per scrape; with default max_results=5 that gives the model up to
    // page=6 before exhausting a single fetch.
    struct Hit { std::string title, url, snippet; };
    std::vector<Hit> hits;
    hits.reserve(40);

    auto t_end = std::sregex_iterator();
    for (auto it = std::sregex_iterator(body.begin(), body.end(), re_title);
         it != t_end; ++it) {
        std::string href  = (*it)[1].str();
        std::string title = strip_html((*it)[2].str());
        std::string real  = decode_ddg_redirect(href);
        if (real.empty() || title.empty()) continue;
        size_t after = it->position(0) + it->length(0);
        std::string tail = body.substr(after,
            std::min<size_t>(2048, body.size() - after));
        std::string snippet;
        std::smatch sm;
        if (std::regex_search(tail, sm, re_snippet)) {
            snippet = strip_html(sm[1].str());
        }
        hits.push_back({std::move(title), std::move(real), std::move(snippet)});
    }

    if (hits.empty()) {
        // The Netscape UA usually bypasses DDG's anti-bot "anomaly"
        // page, but if DDG ever closes that loophole it would come
        // back here — HTTP 202 with a non-empty body that carries
        // zero result__a anchors. Detect that explicitly so the
        // error is actionable: anomaly = IP/client-level block (a
        // different egress IP is the real fix, not a retry).
        if (body.find("anomaly") != std::string::npos) {
            return ToolResult::error(
                "DuckDuckGo blocked this request — it returned its "
                "anti-bot \"anomaly\" page instead of results, even "
                "with the Netscape UA bypass. This is an IP/client-"
                "level block, not a transient rate-limit, so retrying "
                "soon won't help. The default engine=\"auto\" already "
                "cascades through brave / ddg-lite / bing before "
                "falling here; if you reached this via explicit "
                "engine=\"ddg\", switch to auto or another engine.");
        }
        return ToolResult::error(
            "no results parsed — DuckDuckGo returned a page with no "
            "recognisable result entries (possible HTML markup change, "
            "or a genuinely empty result set for this query). The "
            "default engine=\"auto\" already tries brave / ddg-lite / "
            "bing before this; if you reached this via explicit "
            "engine=\"ddg\", switch engines.");
    }

    const long long total = (long long) hits.size();
    const long long total_pages =
        (total + max_results - 1) / max_results;
    if (page > total_pages) {
        std::ostringstream o;
        o << "[page " << page << " is past last page (" << total_pages
          << "); total_entries: " << total << "]";
        return ToolResult::ok(o.str());
    }

    const long long start_idx = (page - 1) * max_results;
    const long long end_idx   = std::min(total, start_idx + max_results);

    std::ostringstream out;
    out << "total_entries: " << total << "\n";
    out << "page: "          << page << " of " << total_pages << "\n";
    out << "has_more: "      << (page < total_pages ? "yes" : "no") << "\n";
    out << "engine: ddg\n";
    out << "Top web results for: " << query << "\n";
    for (long long i = start_idx; i < end_idx; ++i) {
        out << "\n" << (i + 1) << ". " << hits[i].title
            << "\n   " << hits[i].url
            << "\n   " << clip(hits[i].snippet, 300) << "\n";
    }
    return ToolResult::ok(clip(out.str(), 8 * 1024));
#endif
}

// ---------- search: DuckDuckGo Lite ----------
// GET lite.duckduckgo.com/lite/?q=… while posing as Netscape
// Communicator 4.79 on Windows 2000. DDG Lite is the no-JS,
// accessibility-friendly variant DDG maintains for old browsers and
// screen readers — it bypasses the anti-bot "anomaly" gate the main
// html.duckduckgo.com endpoint applies to scripted clients, because
// an old browser legitimately can't run a JS challenge. The signal
// is the User-Agent: a contemporary Chrome UA gets gated; an obvious
// pre-2002 browser UA gets the simple table-based HTML.
//
// FIRST PAGE ONLY
//   The Lite endpoint accepts an `s=` offset parameter for deeper
//   pages, but this wrapper deliberately doesn't expose it — the
//   user-facing contract is "first page, no pagination." `page > 1`
//   returns a past-end marker so the model knows to switch engine
//   instead of looping uselessly. For deeper pagination use
//   engine="google".
//
// FORMAT QUIRKS
//   - URLs are wrapped in DDG's `/l/?uddg=ENCODED_URL` redirects;
//     decode_ddg_redirect() unwraps them so the model gets real URLs.
//   - DDG randomly mixes single and double quotes on HTML attributes
//     — the regex's [\"'] character class accepts either.
//   - First "result" is often a Microsoft sponsored ad (URL contains
//     `ad_provider=` / `bingv7aa` / `y.js?`); we drop those.
ToolResult web_search_ddg_lite(const std::string & query, long long page,
                               long long max_results) {
#if !defined(EASYAI_HAVE_CURL)
    (void) query; (void) page; (void) max_results;
    return ToolResult::error(
        "web search unavailable: easyai built without libcurl");
#else
    if (max_results < 1)  max_results = 1;
    if (max_results > 10) max_results = 10;
    if (page < 1)         page = 1;

    // Page 1 only — refuse pagination requests cleanly.
    if (page > 1) {
        std::ostringstream o;
        o << "[page " << page << " is past last page (1); ddg-lite "
             "returns first-page results only — for deeper pagination "
             "use engine=\"google\"]";
        return ToolResult::ok(o.str());
    }

    const std::string url = "https://lite.duckduckgo.com/lite/?q="
        + url_encode(query);

    // Netscape persona — see netscape_search_headers() for the why
    // (same persona is shared across all search backends now).
    std::string body, err;
    if (!http_get(url, netscape_search_headers(), body, err,
                  20, 1024 * 1024)) {
        return ToolResult::error("ddg-lite search failed: " + err);
    }
    if (body.empty()) {
        return ToolResult::error("ddg-lite search failed: empty response");
    }

    static const std::regex re_link(
        R"DDG(<a rel=["']nofollow["'] href=["']([^"']+)["'] class=["']result-link["']>([^<]+)</a>)DDG",
        std::regex::icase);
    static const std::regex re_snip(
        R"DDG(<td class=["']result-snippet["'][^>]*>([\s\S]*?)</td>)DDG",
        std::regex::icase);

    struct Hit { std::string title, url, snippet; };
    std::vector<Hit> hits;
    hits.reserve(16);

    auto t_end = std::sregex_iterator();
    for (auto it = std::sregex_iterator(body.begin(), body.end(), re_link);
         it != t_end; ++it) {
        std::string href  = (*it)[1].str();
        std::string title = strip_html((*it)[2].str());
        if (href.empty() || title.empty()) continue;
        // Skip the "more info" footer anchors on sponsored links —
        // they reuse class="result-link" but point at DDG's help page.
        if (title == "more info") continue;
        std::string real = decode_ddg_redirect(href);
        if (real.empty()) continue;
        // Drop Microsoft sponsored ad results — their wrapped URL
        // contains the ad-provider sentinel.
        if (real.find("ad_provider=") != std::string::npos ||
            real.find("bingv7aa")     != std::string::npos ||
            real.find("y.js?")        != std::string::npos) {
            continue;
        }
        // Snippet is in the next few rows; same look-ahead window
        // shape as web_search_ddg.
        size_t after = it->position(0) + it->length(0);
        std::string tail = body.substr(after,
            std::min<size_t>(2048, body.size() - after));
        std::string snippet;
        std::smatch sm;
        if (std::regex_search(tail, sm, re_snip)) {
            snippet = strip_html(sm[1].str());
        }
        hits.push_back({std::move(title), std::move(real), std::move(snippet)});
        if ((long long) hits.size() >= 12) break;
    }

    if (hits.empty()) {
        return ToolResult::error(
            "no results parsed — DDG Lite returned a page with no "
            "`class=\"result-link\"` organic anchors. Likely causes: "
            "the Netscape UA loophole was closed (DDG started gating "
            "Lite the same way as the main HTML endpoint), or "
            "genuinely empty result set. Auto cascade falls to bing "
            "next; if you reached this via explicit engine=\"ddg-lite\", "
            "switch engines.");
    }

    const long long total = (long long) hits.size();
    const long long emit  = std::min(total, max_results);

    std::ostringstream out;
    out << "total_entries: " << total << "\n";
    out << "page: 1 of 1\n";
    out << "has_more: no\n";
    out << "engine: ddg-lite\n";
    out << "Top web results for: " << query << "\n";
    for (long long i = 0; i < emit; ++i) {
        out << "\n" << (i + 1) << ". " << hits[i].title
            << "\n   " << hits[i].url
            << "\n   " << clip(hits[i].snippet, 300) << "\n";
    }
    return ToolResult::ok(clip(out.str(), 8 * 1024));
#endif
}

// ---------- search: Brave Search HTML scrape ----------
// GET search.brave.com/search?q=… — Brave SSRs ~20 result blocks per
// page wrapped in `data-type="web"`. Unlike Bing's RSS endpoint
// (which runs a stripped-down ranking that ignores quoted phrases and
// rare named entities), Brave honours the full query — it'll find a
// niche Brazilian person record for `"Santiago Cavalcante" PNUD` where
// Bing RSS returns a Wikipedia page about Santiago de Compostela.
//
// LIMITS
//   - Pagination via the `&offset=N` URL parameter is intentionally
//     NOT used — instead we fetch page 1 once (~20 results) and
//     page-slice locally, same shape as the Bing handler. This keeps
//     the request count down (Brave throttles single IPs aggressively)
//     and avoids ambiguity about whether `offset` is page-indexed or
//     result-indexed (Brave's docs are inconsistent). For deeper
//     pagination, use engine="google".
//   - Brave's Svelte-generated CSS classes carry build hashes
//     (`svelte-1cwdgg3` etc.) that rotate between deployments. The
//     scraper anchors only on the stable substrings — `data-type="web"`
//     and the literal class-name prefixes "title" / "content" — so a
//     hash rotation alone won't break it. A structural rewrite of the
//     SERP markup will, though, and at that point the auto cascade
//     simply falls through to bing → ddg until this scraper is updated.
ToolResult web_search_brave(const std::string & query, long long page,
                            long long max_results) {
#if !defined(EASYAI_HAVE_CURL)
    (void) query; (void) page; (void) max_results;
    return ToolResult::error(
        "web search unavailable: easyai built without libcurl");
#else
    if (max_results < 1)  max_results = 1;
    if (max_results > 20) max_results = 20;
    if (page < 1)         page = 1;

    const std::string url = "https://search.brave.com/search?q="
        + url_encode(query);

    std::string body, err;
    if (!http_get(url, netscape_search_headers(), body, err,
                  20, 4 * 1024 * 1024)) {
        return ToolResult::error("brave search failed: " + err);
    }
    if (body.empty()) {
        return ToolResult::error("brave search failed: empty response");
    }

    static const std::regex re_href(
        R"BRV(<a[^>]+href="(https?://[^"]+)")BRV", std::regex::icase);
    static const std::regex re_title(
        R"BRV(class="title[^"]*"[^>]*>([\s\S]*?)<)BRV", std::regex::icase);
    static const std::regex re_content(
        R"BRV(class="content[^"]*"[^>]*>([\s\S]*?)</div>)BRV", std::regex::icase);

    struct Hit { std::string title, url, snippet; };
    std::vector<Hit> hits;
    hits.reserve(24);

    const std::string marker = "data-type=\"web\"";
    size_t pos = 0;
    while (true) {
        size_t start = body.find(marker, pos);
        if (start == std::string::npos) break;
        size_t next = body.find(marker, start + marker.size());
        size_t end = (next == std::string::npos)
            ? std::min<size_t>(body.size(), start + 8000)
            : next;
        std::string block = body.substr(start, end - start);
        pos = start + marker.size();

        std::smatch m;
        std::string url_, title_, snippet_;
        if (std::regex_search(block, m, re_href))    url_     = m[1].str();
        if (std::regex_search(block, m, re_title))   title_   = strip_html(m[1].str());
        if (std::regex_search(block, m, re_content)) snippet_ = strip_html(m[1].str());

        if (url_.empty() || title_.empty()) continue;
        // Defensively skip Brave's own internal navigation links — should
        // never appear inside a data-type="web" block, but rotating
        // markup could theoretically slip one in.
        if (url_.find("search.brave.com") != std::string::npos) continue;

        hits.push_back({std::move(title_), std::move(url_), std::move(snippet_)});
        if ((long long) hits.size() >= 24) break;
    }

    if (hits.empty()) {
        return ToolResult::error(
            "no results parsed — Brave returned a page with no "
            "`data-type=\"web\"` result blocks. Likely causes: rate "
            "limit (Brave throttles single IPs aggressively, often "
            "after only a handful of queries in a short window), an "
            "anti-bot challenge, or markup change (Brave's Svelte "
            "classes rotate between deployments). The default "
            "engine=\"auto\" already cascades to bing after this; "
            "if you reached this via explicit engine=\"brave\", "
            "switch to auto or another engine.");
    }

    const long long total = (long long) hits.size();
    const long long total_pages =
        (total + max_results - 1) / max_results;
    if (page > total_pages) {
        std::ostringstream o;
        o << "[page " << page << " is past last page (" << total_pages
          << "); total_entries: " << total
          << "; brave caps at ~20 results per query without re-fetching "
             "with &offset= — for deeper pagination use engine=\"google\"]";
        return ToolResult::ok(o.str());
    }

    const long long start_idx = (page - 1) * max_results;
    const long long end_idx   = std::min(total, start_idx + max_results);

    std::ostringstream out;
    out << "total_entries: " << total << "\n";
    out << "page: "          << page << " of " << total_pages << "\n";
    out << "has_more: "      << (page < total_pages ? "yes" : "no") << "\n";
    out << "engine: brave\n";
    out << "Top web results for: " << query << "\n";
    for (long long i = start_idx; i < end_idx; ++i) {
        out << "\n" << (i + 1) << ". " << hits[i].title
            << "\n   " << hits[i].url
            << "\n   " << clip(hits[i].snippet, 300) << "\n";
    }
    return ToolResult::ok(clip(out.str(), 8 * 1024));
#endif
}

// ---------- search: Bing RSS feed ----------
// GET www.bing.com/search?q=...&format=rss — Bing's RSS endpoint returns
// a clean XML feed of ~10 results per query. Unlike the HTML page (which
// degrades to an empty layout for non-browser UAs even with a Chrome
// persona), RSS is a first-class output Microsoft maintains for feed
// consumers, so it stays keyless, captcha-free, and stable.
//
// LIMITS
//   - ~10 results per query, hard. The `&first=N` pagination parameter
//     is accepted by the URL but the server ignores it for RSS — every
//     request returns the same first page. So `page > 1` returns a
//     past-end marker rather than fabricating duplicates; for deeper
//     pagination use engine="google".
//   - No regional bias by default. Pass `mkt=`/`cc=` in the query string
//     yourself if needed (the wrapper doesn't expose a region knob).
ToolResult web_search_bing(const std::string & query, long long page,
                           long long max_results) {
#if !defined(EASYAI_HAVE_CURL)
    (void) query; (void) page; (void) max_results;
    return ToolResult::error(
        "web search unavailable: easyai built without libcurl");
#else
    if (max_results < 1)  max_results = 1;
    if (max_results > 10) max_results = 10;
    if (page < 1)         page = 1;

    const std::string url = "https://www.bing.com/search?q="
        + url_encode(query) + "&format=rss";

    std::string body, err;
    if (!http_get(url, netscape_search_headers(), body, err)) {
        return ToolResult::error("bing search failed: " + err);
    }
    if (body.empty()) {
        return ToolResult::error("bing search failed: empty response");
    }

    // Parse <item>…</item> blocks. Bing's RSS doesn't use CDATA; the
    // <description> field carries HTML entities (&lt;, &amp;, &#39;, …)
    // and sometimes inline <strong> tags around hit terms. strip_html()
    // handles both.
    static const std::regex re_item(
        R"RSS(<item>([\s\S]*?)</item>)RSS",
        std::regex::icase);
    static const std::regex re_title(
        R"RSS(<title>([\s\S]*?)</title>)RSS",
        std::regex::icase);
    static const std::regex re_link(
        R"RSS(<link>([\s\S]*?)</link>)RSS",
        std::regex::icase);
    static const std::regex re_desc(
        R"RSS(<description>([\s\S]*?)</description>)RSS",
        std::regex::icase);

    struct Hit { std::string title, url, snippet; };
    std::vector<Hit> hits;
    hits.reserve(16);

    auto end_it = std::sregex_iterator();
    for (auto it = std::sregex_iterator(body.begin(), body.end(), re_item);
         it != end_it; ++it) {
        const std::string item = (*it)[1].str();
        std::smatch m;
        std::string title, link, desc;
        if (std::regex_search(item, m, re_title)) title = strip_html(m[1].str());
        if (std::regex_search(item, m, re_link))  link  = trim(m[1].str());
        if (std::regex_search(item, m, re_desc))  desc  = strip_html(m[1].str());
        if (link.empty() || title.empty()) continue;
        hits.push_back({std::move(title), std::move(link), std::move(desc)});
    }

    if (hits.empty()) {
        return ToolResult::error(
            "no results parsed — Bing RSS returned a feed with no "
            "items (possible empty result set, IP-level block, or "
            "schema drift). The default engine=\"auto\" already "
            "cascades to ddg after this; if you reached this via "
            "explicit engine=\"bing\", switch to auto.");
    }

    const long long total = (long long) hits.size();
    const long long total_pages =
        (total + max_results - 1) / max_results;
    if (page > total_pages) {
        std::ostringstream o;
        o << "[page " << page << " is past last page (" << total_pages
          << "); total_entries: " << total
          << "; bing RSS caps at ~10 results per query — for deeper "
             "pagination use engine=\"google\"]";
        return ToolResult::ok(o.str());
    }

    const long long start_idx = (page - 1) * max_results;
    const long long end_idx   = std::min(total, start_idx + max_results);

    std::ostringstream out;
    out << "total_entries: " << total << "\n";
    out << "page: "          << page << " of " << total_pages << "\n";
    out << "has_more: "      << (page < total_pages ? "yes" : "no") << "\n";
    out << "engine: bing\n";
    out << "Top web results for: " << query << "\n";
    for (long long i = start_idx; i < end_idx; ++i) {
        out << "\n" << (i + 1) << ". " << hits[i].title
            << "\n   " << hits[i].url
            << "\n   " << clip(hits[i].snippet, 300) << "\n";
    }
    return ToolResult::ok(clip(out.str(), 8 * 1024));
#endif
}

// ---------- search: Google Custom Search JSON API ----------
// Env vars read at CALL time (not registration) so a long-running server
// picks up rotated keys without restart, and missing-env errors at the
// time the model actually invokes the tool tell the user which variable
// to set.
ToolResult web_search_google(const std::string & query, long long page,
                             long long max_results) {
#if !defined(EASYAI_HAVE_CURL)
    (void) query; (void) page; (void) max_results;
    return ToolResult::error(
        "web search unavailable: easyai built without libcurl");
#else
    if (max_results < 1)  max_results = 1;
    if (max_results > 10) max_results = 10;   // CSE per-call ceiling
    if (page < 1)         page = 1;

    const char * api_key = std::getenv("GOOGLE_API_KEY");
    const char * cse_id  = std::getenv("GOOGLE_CSE_ID");
    if (!api_key || !*api_key) {
        return ToolResult::error(
            "GOOGLE_API_KEY env var not set — get one at "
            "https://console.cloud.google.com/apis/credentials "
            "(enable 'Custom Search API' first). Or set engine=\"ddg\" "
            "(no key required).");
    }
    if (!cse_id || !*cse_id) {
        return ToolResult::error(
            "GOOGLE_CSE_ID env var not set — create a Programmable "
            "Search Engine at https://programmablesearchengine.google.com "
            "and copy the 'cx' value. Or set engine=\"ddg\" (no key "
            "required).");
    }

    // Google CSE pagination uses `start=` (1-based offset). page=1 → start=1,
    // page=2 with max_results=10 → start=11, etc. The API caps total
    // pageable results at 100 (start ≤ 91 with max_results=10).
    const long long start = (page - 1) * max_results + 1;
    if (start > 91) {
        return ToolResult::error(
            "page is past Google CSE's pagination ceiling (start>91; "
            "the API caps total pageable results at 100)");
    }

    std::string url = "https://www.googleapis.com/customsearch/v1?";
    url += "key="   + url_encode(api_key);
    url += "&cx="   + url_encode(cse_id);
    url += "&q="    + url_encode(query);
    url += "&num="  + std::to_string(max_results);
    url += "&start=" + std::to_string(start);
    url += "&safe=active";

    std::string body, err;
    if (!http_get(url, {}, body, err, 20, 1024 * 1024)) {
        return ToolResult::error("google search failed: " + err
            + " (verify GOOGLE_API_KEY, GOOGLE_CSE_ID, and that "
              "Custom Search API is enabled in your Cloud project)");
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(body);
    } catch (const std::exception & e) {
        return ToolResult::error(
            std::string("google search: malformed JSON response: ") + e.what());
    }
    if (j.contains("error") && j["error"].is_object()) {
        std::string msg = j["error"].value("message", "unknown error");
        return ToolResult::error("google search rejected request: " + msg);
    }

    // CSE response includes `queries.nextPage` when more results exist.
    bool has_more = false;
    if (j.contains("queries") && j["queries"].is_object()
            && j["queries"].contains("nextPage")
            && j["queries"]["nextPage"].is_array()
            && !j["queries"]["nextPage"].empty()) {
        has_more = true;
    }

    std::ostringstream out;
    out << "page: "     << page << "\n";
    out << "has_more: " << (has_more ? "yes" : "no") << "\n";
    out << "engine: google\n";
    out << "Top web results for: " << query << "\n";
    int count = 0;
    if (j.contains("items") && j["items"].is_array()) {
        for (const auto & item : j["items"]) {
            if (count >= max_results) break;
            std::string title = item.value("title",   std::string{});
            std::string link  = item.value("link",    std::string{});
            std::string snip  = item.value("snippet", std::string{});
            std::replace(snip.begin(), snip.end(), '\n', ' ');
            if (link.empty() || title.empty()) continue;
            out << "\n" << (start + count) << ". " << title
                << "\n   " << link
                << "\n   " << clip(snip, 300) << "\n";
            ++count;
        }
    }
    if (count == 0) {
        return ToolResult::error(
            "no results — try a broader query, or verify the CSE "
            "is configured to search the entire web (not just a "
            "single site) at https://programmablesearchengine.google.com");
    }
    return ToolResult::ok(clip(out.str(), 8 * 1024));
#endif
}

// ---------- search: dispatch ----------
// Default engine is "auto" — a fixed-order cascade through five
// backends, picking the first that returns a non-error response:
//
//     google  →  brave  →  ddg-lite  →  bing  →  ddg
//
// Why this order:
//   - Google CSE is the highest-quality result set, but it's billed
//     and requires both an operator opt-in (`use_google=true` at
//     register time) and two env vars (GOOGLE_API_KEY, GOOGLE_CSE_ID).
//     When all of those are in place we prefer it. When any are
//     missing we SKIP — not error — and move on, so a deployment
//     without Google credentials silently falls through.
//   - Brave HTML (`search.brave.com/search?q=…`) is the keyless
//     engine that best understands the full query. Bing RSS, by
//     contrast, runs a stripped-down ranking that ignores quoted
//     phrases and rare named entities — it returns Wikipedia about
//     Santiago de Compostela for `"Santiago Cavalcante" PNUD`. So we
//     try Brave first among the keyless engines whenever the query
//     might contain a niche term. The downside: Brave throttles
//     single IPs aggressively (HTTP 429 after a small burst) and its
//     Svelte classes rotate between deploys.
//   - DDG Lite (`lite.duckduckgo.com/lite/`) with a Netscape 4.79 UA
//     is the second keyless engine. It's also good at niche entity
//     queries (returns the LinkedIn / Google Scholar / Brazilian
//     profile hits for "Santiago Cavalcante" PNUD) and isn't rate-
//     limited the way Brave is — so it carries the keyless workhorse
//     case whenever Brave's burst budget is gone. The Netscape UA
//     matters: a contemporary UA gets gated by the same anti-bot
//     wall the main html.duckduckgo.com endpoint applies, but the
//     Lite endpoint serves no-JS HTML to clients that obviously
//     can't run JS. Page 1 only.
//   - Bing RSS (`/search?q=…&format=rss`) is keyless, captcha-free,
//     and stable — Microsoft maintains it for legitimate feed
//     consumers. Caps at ~10 results, no pagination, weak query
//     understanding for niche entities — useful as a fallback for
//     ordinary keyword queries.
//   - DDG HTML scrape is the last fallback — same backend as DDG Lite
//     but the modern endpoint (html.duckduckgo.com), which DDG now
//     gates aggressively from server IPs with the "anomaly" page.
//     Kept because (a) no key, (b) occasionally still works even
//     when ddg-lite doesn't (different rate-limit / IP-block paths).
//
// If all five fail, the error aggregates each engine's reason, so the
// operator can tell at a glance whether it's a credential issue, a
// network issue, or genuinely no results for the query.
//
// Explicit engine= overrides skip the cascade entirely and run that
// engine alone — useful for diagnosis ("does ddg still work from this
// box?") or for pinning a known-good engine when the cascade order is
// not what the caller wants.
ToolResult web_handle_search(const ToolCall & c, bool google_enabled) {
    std::string query;
    if (!args::get_string(c.arguments_json, "query", query) || query.empty()) {
        return ToolResult::error(
            "missing required arg: query (web action=\"search\")");
    }
    long long max_results = 5;
    args::get_int(c.arguments_json, "max_results", max_results);
    long long page = 1;
    args::get_int(c.arguments_json, "page", page);
    std::string engine;
    args::get_string(c.arguments_json, "engine", engine);

    // Explicit engine — run only that engine, no fallback.
    if (engine == "ddg")      return web_search_ddg(query, page, max_results);
    if (engine == "ddg-lite") return web_search_ddg_lite(query, page, max_results);
    if (engine == "bing")     return web_search_bing(query, page, max_results);
    if (engine == "brave")    return web_search_brave(query, page, max_results);
    if (engine == "google") {
        if (!google_enabled) {
            return ToolResult::error(
                "engine=\"google\" not enabled on this server — operator "
                "must pass --use-google (or Toolbelt::use_google()) to "
                "allow Google CSE. Default engine \"auto\" cascades "
                "google → brave → ddg-lite → bing → ddg and works "
                "without any opt-in.");
        }
        return web_search_google(query, page, max_results);
    }
    if (!engine.empty() && engine != "auto") {
        return ToolResult::error(
            "unknown engine \"" + engine + "\" — valid: \"auto\" "
            "(default; cascades google → brave → ddg-lite → bing → "
            "ddg), \"google\" (needs GOOGLE_API_KEY + GOOGLE_CSE_ID "
            "env vars and operator opt-in), \"brave\" (HTML scrape, "
            "keyless, best query understanding), \"ddg-lite\" "
            "(no-JS DDG with Netscape UA, keyless, page 1 only), "
            "\"bing\" (RSS, keyless), \"ddg\" (HTML scrape, keyless).");
    }

    // Auto cascade.
    std::vector<std::string> tried;

    // 1. Google — only if operator opted in AND env vars are present.
    //    Missing creds is "skip", not "fail".
    if (google_enabled) {
        const char * gk = std::getenv("GOOGLE_API_KEY");
        const char * gc = std::getenv("GOOGLE_CSE_ID");
        if (gk && *gk && gc && *gc) {
            ToolResult r = web_search_google(query, page, max_results);
            if (!r.is_error) return r;
            tried.push_back("google: " + r.content);
        } else {
            tried.push_back(
                "google: skipped (GOOGLE_API_KEY / GOOGLE_CSE_ID env "
                "vars not set)");
        }
    } else {
        tried.push_back(
            "google: skipped (operator did not enable engine=\"google\" "
            "at registration)");
    }

    // 2. Brave HTML — keyless, best query understanding when not 429ed.
    {
        ToolResult r = web_search_brave(query, page, max_results);
        if (!r.is_error) return r;
        tried.push_back("brave: " + r.content);
    }

    // 3. DDG Lite (Netscape UA) — keyless workhorse, also good at
    //    niche entity queries, page-1-only.
    {
        ToolResult r = web_search_ddg_lite(query, page, max_results);
        if (!r.is_error) return r;
        tried.push_back("ddg-lite: " + r.content);
    }

    // 4. Bing RSS — keyless, captcha-free, weak entity ranking.
    {
        ToolResult r = web_search_bing(query, page, max_results);
        if (!r.is_error) return r;
        tried.push_back("bing: " + r.content);
    }

    // 5. DDG HTML — last resort, often blocked from server IPs.
    {
        ToolResult r = web_search_ddg(query, page, max_results);
        if (!r.is_error) return r;
        tried.push_back("ddg: " + r.content);
    }

    std::ostringstream o;
    o << "all search engines failed:";
    for (const auto & e : tried) o << "\n  - " << e;
    return ToolResult::error(o.str());
}

// ---------- fetch: GET a URL, return text (or raw HTML), with paging ----------
ToolResult web_handle_fetch(const ToolCall & c) {
#if !defined(EASYAI_HAVE_CURL)
    (void) c;
    return ToolResult::error(
        "web fetch unavailable: easyai built without libcurl");
#else
    std::string url; bool as_html = false;
    if (!args::get_string(c.arguments_json, "url", url) || url.empty()) {
        return ToolResult::error("missing required arg: url (web action=\"fetch\")");
    }
    args::get_bool(c.arguments_json, "as_html", as_html);
    long long start = std::max<long long>(0,
        args::get_int_or(c.arguments_json, "start", 0));
    long long limit = args::get_int_or(c.arguments_json, "limit", 8 * 1024);
    if (limit < 256)            limit = 256;
    if (limit > 64 * 1024)      limit = 64 * 1024;

    // Cache key: url + as_html flag. Pagination is applied AFTER cache
    // hit so we don't multiply storage by every (start, limit) combo.
    const std::string key = url + (as_html ? "|html" : "|text");
    std::string processed;
    if (!web_fetch_cache().get(key, processed)) {
        std::string body, err;
        if (!http_get(url, {}, body, err)) {
            return ToolResult::error("fetch failed: " + err);
        }
        processed = as_html ? body : strip_html(body);
        web_fetch_cache().put(key, processed, as_html);
    }

    if ((size_t) start >= processed.size()) {
        std::ostringstream oss;
        oss << "[start=" << start << " is past end of body (size="
            << processed.size() << "); nothing to return]";
        return ToolResult::ok(oss.str());
    }
    std::string slice = processed.substr(start, (size_t) limit);
    const size_t remaining = processed.size() - start - slice.size();
    static const char * const kFetchCiteHint =
        "\n\n[CITE: your reply MUST end with a Sources: block "
        "listing this URL.]";
    if (remaining > 0) {
        std::ostringstream oss;
        oss << slice
            << "\n\n[truncated: " << remaining
            << " more bytes; pass start="
            << (start + slice.size())
            << " to continue]"
            << kFetchCiteHint;
        return ToolResult::ok(oss.str());
    }
    return ToolResult::ok(slice + kFetchCiteHint);
#endif
}

}  // namespace

// Focused per-action variants of the `web` tool. Same handlers as the
// unified surface — smaller models perform notably better with a
// single verb per tool rather than a discriminated `action`-string.
std::vector<Tool> web_split(bool google_enabled) {
    std::vector<Tool> out;
    out.reserve(2);

    out.push_back(Tool::builder("web_search")
        .short_describe(
            "Search the web — returns title/url/snippet list. Fetch "
            "the top 1-3 URLs after. Reply MUST cite Sources.")
        .describe(
            "Search the web. Returns a numbered title/url/snippet "
            "list. After searching, fetch the top 1-3 URLs — "
            "snippets alone are too short to answer from.\n"
            "Arguments are PLAIN JSON — no XML tags, no markup.\n"
            "Example: {\"query\":\"BitNet quantization\"}\n"
            "\n"
            "KNOWLEDGE LOOP: always search memory FIRST for what "
            "you already know, then search the web for freshness. "
            "If the web returns durable facts memory didn't have, "
            "update memory after answering.\n"
            "\n"
            "CITATION (INVIOLABLE): after ANY web_search / web_fetch "
            "this turn, your final reply MUST end with a `Sources:` "
            "block listing the URLs you actually fetched, one per "
            "line, prefixed `- `.")
        .param("query",       "string", "Search query.", true)
        .param("max_results", "integer",
               "Default 5, max 20.", false)
        .param("page",        "integer",
               "1-based page index, default 1.", false)
        .param("engine",      "string",
               "Default \"auto\" cascades google → brave → ddg-lite "
               "→ bing → ddg and returns the first that works. "
               "Pin one only for diagnosis.", false)
        .handle([google_enabled](const ToolCall & c) -> ToolResult {
            return web_handle_search(c, google_enabled);
        })
        .build());

    out.push_back(Tool::builder("web_fetch")
        .short_describe(
            "Fetch one URL — returns its stripped text. Cite the URL "
            "in your reply's `Sources:` block.")
        .describe(
            "Fetch a URL and return its text (HTML stripped by "
            "default). When the response is truncated the marker "
            "tells you the next `start=` value. Same URL is cached "
            "5 min.\n"
            "\n"
            "CITATION (INVIOLABLE): after ANY web_search / web_fetch "
            "this turn, your final reply MUST end with a `Sources:` "
            "block listing the URLs you actually fetched, one per "
            "line, prefixed `- `.")
        .param("url",     "string",
               "Absolute http(s) URL to fetch.", true)
        .param("as_html", "boolean",
               "Keep raw HTML instead of stripped text. Default "
               "false.", false)
        .param("start",   "integer",
               "Byte offset, default 0. Use the previous call's "
               "`start=` marker to continue.", false)
        .param("limit",   "integer",
               "Window size in bytes. Default 8192, min 256, max "
               "65536.", false)
        .handle([](const ToolCall & c) -> ToolResult {
            return web_handle_fetch(c);
        })
        .build());

    return out;
}

Tool web(bool google_enabled) {
    return Tool::builder("web")
        .short_describe(
            "Search the web and fetch URLs. action=search|fetch. "
            "Reply MUST end with a `Sources:` block listing URLs used.")
        .describe(
            "Web search and fetch — pick an action.\n"
            "Arguments are PLAIN JSON — no XML tags, no markup.\n"
            "\n"
            "KNOWLEDGE LOOP: always search memory FIRST for what "
            "you already know, then search the web for freshness. "
            "If the web returns durable facts memory didn't have, "
            "update memory after answering.\n"
            "\n"
            "CITATION (INVIOLABLE): after ANY call to this tool in a "
            "turn, your final reply MUST end with a `Sources:` block "
            "listing the URLs you actually fetched, one per line, "
            "prefixed `- `. Skipping it makes the reply incomplete.\n"
            "\n"
            "CALL EXAMPLES (exact JSON):\n"
            "  {\"action\":\"search\",\"query\":\"BitNet quantization\"}\n"
            "  {\"action\":\"fetch\",\"url\":\"https://example.com/page\"}\n"
            "\n"
            "action=\"search\"\n"
            "  Required: query.\n"
            "  Optional: max_results (default 5, max 20), page "
            "(default 1, 1-based), engine (default \"auto\" cascades "
            "google → brave → ddg-lite → bing → ddg).\n"
            "  Returns: numbered title/url/snippet list with "
            "total_entries, page, has_more, engine header lines.\n"
            "\n"
            "action=\"fetch\"\n"
            "  Required: url.\n"
            "  Optional: as_html (default false; true keeps raw HTML), "
            "start (default 0, byte offset), limit (default 8192, "
            "max 65536).\n"
            "  Returns: page text. When truncated, the marker tells "
            "you the next `start=` value. Same URL cached 5 min.\n"
            "\n"
            "After a search, ALWAYS fetch the top 1-3 URLs to "
            "answer — snippets are too short. Cite the URL you "
            "actually fetched.")
        .param("action",      "string",
               "\"search\" or \"fetch\".", true)
        .param("query",       "string",
               "Search query (with action=search).", false)
        .param("url",         "string",
               "Absolute http(s) URL (with action=fetch).", false)
        .param("max_results", "integer",
               "Search page size. Default 5, max 20.", false)
        .param("page",        "integer",
               "Search page index, 1-based. Default 1.", false)
        .param("engine",      "string",
               "Search backend. Default \"auto\" cascades google → "
               "brave → ddg-lite → bing → ddg. Pin one only for "
               "diagnosis.", false)
        .param("start",       "integer",
               "Fetch byte offset. Default 0.", false)
        .param("limit",       "integer",
               "Fetch window size in bytes. Default 8192, max 65536.",
               false)
        .param("as_html",     "boolean",
               "Keep raw HTML in fetch. Default false.", false)
        .handle([google_enabled](const ToolCall & c) -> ToolResult {
            std::string action;
            if (!args::get_string(c.arguments_json, "action", action)
                    || action.empty()) {
                return ToolResult::error(
                    "missing required argument: action. Use \"search\" "
                    "or \"fetch\".");
            }
            if (action == "search") return web_handle_search(c, google_enabled);
            if (action == "fetch")  return web_handle_fetch(c);
            return ToolResult::error(
                "unknown action \"" + action + "\". Valid: \"search\", "
                "\"fetch\".");
        })
        .build();
}

// ============================================================================
// filesystem tools — sandboxed under `root`
// ============================================================================
namespace {

struct Sandbox {
    stdfs::path root;
    explicit Sandbox(std::string r) {
        if (r.empty()) r = ".";
        root = stdfs::weakly_canonical(stdfs::absolute(r));
    }
    // Resolve a user-supplied path inside the sandbox; returns false if it
    // escapes the root.
    //
    // SECURITY: the containment check must be PATH-COMPONENT aware, not a
    // raw string-prefix.  Otherwise a sandbox at "/srv/user" would be
    // happily satisfied by "/srv/userMALICIOUS/secrets" (same prefix,
    // different directory tree).  We require canonical to either equal
    // root or have root + path-separator as its prefix.
    // Always succeeds.  We never reject — instead we ALWAYS anchor
    // the resulting path inside the sandbox by:
    //   1. iterating the input's path components,
    //   2. dropping any "..", ".", and absolute markers ("/", "\"),
    //   3. joining the survivors onto the sandbox root.
    //
    // This means:
    //   "/news.md"          → <sandbox>/news.md
    //   "../etc/passwd"     → <sandbox>/etc/passwd        (contained)
    //   "a/../b/news.md"    → <sandbox>/a/b/news.md       (contained)
    //   "C:\\foo\\bar"      → <sandbox>/C:/foo/bar        (Windows-y but
    //                                                       still inside)
    //
    // Trade-off vs the previous canonical-resolution + containment
    // check: a malicious symlink already inside the sandbox could
    // be followed out of it, since we no longer call weakly_canonical.
    // Acceptable because the sandbox is user-owned (they pick the dir
    // with --sandbox); we don't expose symlink creation to the model.
    // In return: the model can NEVER produce a path that "escapes" —
    // any input becomes a real file path under the sandbox.
    //
    // The `err` out-param is kept for API stability (callers still
    // check the return value) but is never written; this method
    // always returns true.
    bool resolve(const std::string & in, stdfs::path & out, std::string & /*err*/) const {
        auto is_only_dots = [](const std::string & s) {
            if (s.empty()) return false;
            for (char c : s) if (c != '.') return false;
            return true;
        };
        stdfs::path raw = in;
        stdfs::path rel;
        for (const auto & part : raw) {
            const std::string s = part.string();
            if (s.empty())                  continue;
            if (s == "/" || s == "\\")      continue;
            if (is_only_dots(s))            continue;   // ".", "..", "...", "....", …
            rel /= part;
        }
        out = (root / rel).lexically_normal();
        return true;
    }
    // Render a real on-disk path back into the model's relative view.
    // The sandbox base is hidden; the model sees `report.md`,
    // `src/main.cpp`, `.` for the root itself.  Relative form matches
    // what every fs_* / bash description tells the model to USE as
    // input — so grep/glob output can be fed straight back into
    // read_file without a leading-slash dance.
    std::string virtual_path(const stdfs::path & real) const {
        std::error_code ec;
        stdfs::path rel = stdfs::relative(real, root, ec);
        if (ec || rel.empty() || rel == ".") return ".";
        return rel.generic_string();
    }
    // Belt-and-braces containment check, called at file-access time.
    // resolve() mechanically anchors the input under root; this method
    // runs the canonical-resolution check once we hold the final path
    // so a malicious symlink already inside the sandbox (e.g. planted
    // by the bash tool) cannot redirect a follow-up open() outside.
    //
    // Two-layer strategy:
    //   1. Lexical containment — cheap, no FS access. resolve() always
    //      anchors its output under root by construction, so any path
    //      we got from resolve() passes this. Required: false here is
    //      a definitive "not under root, not even on paper".
    //   2. Symlink-safe canonical check via weakly_canonical. Catches
    //      symlinks anywhere in the path that would redirect us out of
    //      root. When canonicalisation FAILS (typically EACCES on a
    //      0000 parent dir, or ENOENT racing rmdir), we fall back to
    //      the lexical answer instead of rejecting — failing closed
    //      here used to break fs_check_path on exactly the paths the
    //      operator most wanted to probe (e.g. files inside an
    //      0000-perm parent that the model wants to know about).
    bool inside_sandbox(const stdfs::path & p) const {
        // Path-component prefix match — avoids the string-prefix bug
        // where "/srv/user" prefix-matches "/srv/userMALICIOUS/secret".
        auto component_prefix = [](const stdfs::path & prefix,
                                   const stdfs::path & full) {
            auto it_pre = prefix.begin();
            auto it_ful = full.begin();
            for (; it_pre != prefix.end(); ++it_pre, ++it_ful) {
                if (it_ful == full.end() || *it_ful != *it_pre) return false;
            }
            return true;
        };
        // Layer 1: lexical containment. Fail-closed if the path on
        // paper is outside the sandbox (would only happen if a caller
        // constructed `p` themselves rather than going through resolve()).
        if (!component_prefix(root, p.lexically_normal())) return false;

        // Layer 2: bonus symlink defence. If canonicalisation fails
        // (perm error, race), trust layer 1 — we know the path is
        // lexically inside, and the subsequent open() / lstat() will
        // surface the real errno far more usefully than a blanket
        // "escapes sandbox" rejection.
        std::error_code ec;
        stdfs::path canon_p = stdfs::weakly_canonical(p, ec);
        if (ec) return true;
        stdfs::path canon_r = stdfs::weakly_canonical(root, ec);
        if (ec) return true;
        return component_prefix(canon_r, canon_p);
    }
};

// Regex metacharacters that need escaping when we lift a glob pattern
// into a regex. Curly braces and brackets are included even though they
// only become metachars in some POSIX dialects — std::regex (ECMAScript
// by default) tolerates the over-escape and we'd rather be conservative
// than leak a syntax error from a model-supplied pattern.
constexpr const char * kGlobRegexMetachars = ".+()|^$\\{}[]";

// Convert a shell-style glob into an anchored ECMAScript regex.
//   *   -> [^/]*    (matches anything except a path separator)
//   **  -> .*       (matches anything, including separators)
//   ?   -> [^/]     (one char except a separator)
//   regex metachars are escaped; everything else passes through.
// The output is wrapped in ^...$ so std::regex_match treats it as a
// whole-string predicate — same semantics as ::fnmatch(3) without the
// platform's globbing edge cases.
inline std::string glob_to_regex(const std::string & pattern) {
    // If the pattern has no '/' at all, the model is naming a file
    // by basename only ("*.c", "ADELIDE*") — match it anywhere in the
    // tree, the same way `find -name PAT` does. Without this, a
    // top-level file in the sandbox root never matches because the
    // pattern implies a path component but the rel-path has none.
    const bool implicit_recursive =
        pattern.find('/') == std::string::npos;

    std::string re = "^";
    re.reserve(pattern.size() * 2 + 4);
    if (implicit_recursive) re += "(?:.*/)?";

    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char ch = pattern[i];
        if (ch == '*') {
            const bool is_double_star =
                (i + 1 < pattern.size() && pattern[i + 1] == '*');
            if (is_double_star) {
                // `**/` should match zero-or-more path segments
                // (including none), so `**/foo` matches both
                // `foo` and `a/b/foo`. The previous translation
                // (`.*/`) required at least one slash and missed
                // top-level matches.
                const bool has_trailing_slash =
                    (i + 2 < pattern.size() && pattern[i + 2] == '/');
                if (has_trailing_slash) {
                    re += "(?:.*/)?";
                    i += 2;          // consume '*' + '/'
                } else {
                    re += ".*"; ++i; // bare `**` — match any chars
                }
            } else {
                re += "[^/]*";
            }
        } else if (ch == '?') {
            re += "[^/]";
        } else if (std::strchr(kGlobRegexMetachars, ch)) {
            re += '\\';
            re += ch;
        } else {
            re += ch;
        }
    }
    re += '$';
    return re;
}

}  // namespace

// ============================================================================
// fs — unified filesystem dispatcher
// ----------------------------------------------------------------------------
// Eight actions: read, write, list, glob, grep, check_path, cwd, sandbox.
// Each handler factory below takes a shared Sandbox (capturing the pinned
// root) and returns a ToolHandler the unified Tool dispatches to.
//
// The rest of this file kept the legacy seven-tool surface
// (fs_read_file/fs_write_file/...) until 2026-05-09, when this dispatcher
// replaced them. Same on-disk semantics, same Sandbox containment, same
// O_NOFOLLOW + post-mkdir TOCTOU defenses — only the catalog shape
// changes.
// ============================================================================

namespace {

ToolHandler make_fs_read_handler(std::shared_ptr<Sandbox> sb) {
    return [sb](const ToolCall & c) -> ToolResult {
        std::string path;
        long long offset = 0, limit = 0, start_line = 0;
        bool line_numbers = true;
        if (!args::get_string(c.arguments_json, "path", path))
            return ToolResult::error("missing arg: path (fs action=\"read\")");
        bool has_limit = args::get_int(c.arguments_json, "limit",  limit);
        args::get_int(c.arguments_json, "offset", offset);
        args::get_int(c.arguments_json, "start_line", start_line);
        args::get_bool(c.arguments_json, "line_numbers", line_numbers);

        stdfs::path p; std::string err;
        if (!sb->resolve(path, p, err)) return ToolResult::error(err);
        if (!sb->inside_sandbox(p)) {
            return ToolResult::error("path escapes sandbox via symlink: "
                                     + sb->virtual_path(p));
        }
        int fd = ::open(p.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            return ToolResult::error(std::string("cannot open: ")
                                     + sb->virtual_path(p)
                                     + " (" + std::strerror(errno) + ")");
        }
        {
            struct stat st_kind {};
            if (::fstat(fd, &st_kind) == 0 && S_ISDIR(st_kind.st_mode)) {
                ::close(fd);
                return ToolResult::error(
                    std::string("path is a directory: ")
                    + sb->virtual_path(p)
                    + " — use action=\"list\" to enumerate entries, "
                    "or action=\"glob\" / \"grep\" for recursive search.");
            }
        }

        // ----- line-based read (start_line set) -------------------------
        // When start_line is present, `limit` means number of lines
        // (default 200, max 2000). Line numbers are always on.
        if (start_line > 0) {
            constexpr size_t kLineReadMax = 8 * 1024 * 1024;
            std::string body;
            {
                char chunk[64 * 1024];
                for (;;) {
                    ssize_t n = ::read(fd, chunk, sizeof(chunk));
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        ::close(fd);
                        return ToolResult::error(
                            std::string("read failed: ")
                            + std::strerror(errno));
                    }
                    if (n == 0) break;
                    if (body.size() + (size_t) n > kLineReadMax) {
                        ::close(fd);
                        return ToolResult::error(
                            "file too large for line-based read (>8 MiB); "
                            "use offset/limit (byte-based) instead.");
                    }
                    body.append(chunk, (size_t) n);
                }
            }
            ::close(fd);

            // Split into line offsets.
            std::vector<std::pair<size_t, size_t>> lns; // (start, len-incl-\n)
            lns.reserve(body.size() / 40 + 1);
            size_t ls = 0;
            for (size_t i = 0; i < body.size(); ++i) {
                if (body[i] == '\n') {
                    lns.emplace_back(ls, i - ls + 1);
                    ls = i + 1;
                }
            }
            if (ls < body.size()) lns.emplace_back(ls, body.size() - ls);

            const long long total = (long long) lns.size();
            if (start_line > total) {
                return ToolResult::error(
                    "start_line " + std::to_string(start_line)
                    + " past end of file (total lines: "
                    + std::to_string(total) + ")");
            }

            long long line_limit = has_limit ? limit : 200;
            if (line_limit < 1)    line_limit = 1;
            if (line_limit > 2000) line_limit = 2000;

            long long end = std::min(start_line + line_limit - 1, total);

            std::string out;
            out.reserve((size_t)(end - start_line + 1) * 90);
            char numbuf[16];
            for (long long ln = start_line; ln <= end; ++ln) {
                auto & seg = lns[(size_t)(ln - 1)];
                std::string_view sv(body.data() + seg.first, seg.second);
                while (!sv.empty()
                        && (sv.back() == '\n' || sv.back() == '\r'))
                    sv.remove_suffix(1);
                int nn = std::snprintf(numbuf, sizeof(numbuf),
                                       "%6lld| ", ln);
                if (nn > 0) out.append(numbuf, (size_t) nn);
                out.append(sv.data(), sv.size());
                out.push_back('\n');
            }
            if (end < total) {
                out.append("[" + std::to_string(total - end)
                           + " more lines; pass start_line="
                           + std::to_string(end + 1) + " to continue]\n");
            }
            char mbuf[128];
            std::snprintf(mbuf, sizeof(mbuf),
                "\033[2m%s — %lld lines, %zu bytes, showing %lld-%lld"
                " of %lld\033[0m\n",
                sb->virtual_path(p).c_str(),
                total, body.size(), start_line, end, total);
            return ToolResult::ok_display(std::move(out), mbuf);
        }

        // ----- byte-based read (existing behaviour) ---------------------
        if (!has_limit) limit = 64 * 1024;
        if (offset < 0) offset = 0;
        if (limit  < 1) limit  = 1;
        if (limit > 1024 * 1024) limit = 1024 * 1024;

        struct stat st_sz {};
        long long file_size = -1;
        if (::fstat(fd, &st_sz) == 0 && S_ISREG(st_sz.st_mode))
            file_size = (long long) st_sz.st_size;

        if (offset > 0 && ::lseek(fd, offset, SEEK_SET) < 0) {
            ::close(fd);
            return ToolResult::error(std::string("seek failed: ")
                                     + std::strerror(errno));
        }
        std::string buf((size_t) limit, '\0');
        ssize_t n = ::read(fd, buf.data(), (size_t) limit);
        if (n < 0) {
            ::close(fd);
            return ToolResult::error(std::string("read failed: ")
                                     + std::strerror(errno));
        }
        buf.resize((size_t) n);

        // Count lines in what we read.
        long long lines_read = 0;
        for (char ch : buf) if (ch == '\n') ++lines_read;
        if (!buf.empty() && buf.back() != '\n') ++lines_read;

        // Count total lines in file (for files <= 8 MiB).
        long long total_lines = -1;
        if (file_size > 0 && file_size <= 8LL * 1024 * 1024) {
            if (::lseek(fd, 0, SEEK_SET) >= 0) {
                total_lines = 0;
                char cnt[64 * 1024];
                char last = 0;
                for (;;) {
                    ssize_t r = ::read(fd, cnt, sizeof(cnt));
                    if (r <= 0) break;
                    for (ssize_t i = 0; i < r; ++i)
                        if (cnt[i] == '\n') ++total_lines;
                    last = cnt[r - 1];
                }
                if (last != '\n') ++total_lines;
            }
        }
        ::close(fd);

        auto make_byte_metric = [&]() -> std::string {
            char mbuf[160];
            std::snprintf(mbuf, sizeof(mbuf),
                "\033[2m%s — %lld bytes read (offset %lld, ~%lld lines)",
                sb->virtual_path(p).c_str(), (long long) n, offset,
                lines_read);
            std::string s = mbuf;
            if (file_size >= 0) {
                std::snprintf(mbuf, sizeof(mbuf),
                    ", file %lld bytes", file_size);
                s += mbuf;
            }
            if (total_lines >= 0) {
                std::snprintf(mbuf, sizeof(mbuf),
                    ", %lld total lines", total_lines);
                s += mbuf;
            }
            s += "\033[0m\n";
            return s;
        };

        if (line_numbers && !buf.empty()) {
            std::string out;
            out.reserve(buf.size() + buf.size() / 32);
            if (offset > 0) {
                out.append("[note: numbering restarts at this chunk; "
                           "for absolute line numbers, use start_line "
                           "or action=\"grep\"]\n");
            }
            long long lineno = 1;
            char numbuf[16];
            size_t line_start = 0;
            for (size_t i = 0; i < buf.size(); ++i) {
                if (buf[i] == '\n') {
                    int nn = std::snprintf(numbuf, sizeof(numbuf),
                                           "%6lld| ", lineno);
                    if (nn > 0) out.append(numbuf, (size_t) nn);
                    out.append(buf, line_start, i - line_start + 1);
                    line_start = i + 1;
                    ++lineno;
                }
            }
            if (line_start < buf.size()) {
                int nn = std::snprintf(numbuf, sizeof(numbuf),
                                       "%6lld| ", lineno);
                if (nn > 0) out.append(numbuf, (size_t) nn);
                out.append(buf, line_start, buf.size() - line_start);
            }
            return ToolResult::ok_display(std::move(out), make_byte_metric());
        }

        return ToolResult::ok_display(std::move(buf), make_byte_metric());
    };
}

ToolHandler make_fs_write_handler(std::shared_ptr<Sandbox> sb) {
    return [sb](const ToolCall & c) -> ToolResult {
        std::string path, content; bool append = false;
        if (!args::get_string(c.arguments_json, "path",    path))
            return ToolResult::error("missing arg: path (fs action=\"write\")");
        if (!args::get_string(c.arguments_json, "content", content))
            return ToolResult::error("missing arg: content (fs action=\"write\")");
        args::get_bool(c.arguments_json, "append", append);

        stdfs::path p; std::string err;
        if (!sb->resolve(path, p, err)) return ToolResult::error(err);
        if (!sb->inside_sandbox(p)) {
            return ToolResult::error("path escapes sandbox via symlink: "
                                     + sb->virtual_path(p));
        }

        std::error_code ec;
        stdfs::create_directories(p.parent_path(), ec);

        // Re-check containment AFTER create_directories. The pre-check
        // runs against the canonical path of `p`; create_directories
        // follows symlinks in existing parents and could have
        // materialised intermediate dirs along a path that — racing
        // with concurrent symlink creation, or via a parent whose
        // canonicalisation just failed-open in inside_sandbox() —
        // actually points outside root. O_NOFOLLOW already protects
        // the leaf write; this second check rejects the rare case
        // where a parent dir was just created outside the sandbox.
        if (!sb->inside_sandbox(p)) {
            return ToolResult::error(
                "path escapes sandbox via symlink (post-mkdir): "
                + sb->virtual_path(p));
        }

        int flags = O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC
                  | (append ? O_APPEND : O_TRUNC);
        int fd = ::open(p.c_str(), flags, 0600);
        if (fd < 0) {
            return ToolResult::error(std::string("cannot open for write: ")
                                     + sb->virtual_path(p)
                                     + " (" + std::strerror(errno) + ")");
        }
        const char * data = content.data();
        size_t       left = content.size();
        while (left > 0) {
            ssize_t n = ::write(fd, data, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                ::close(fd);
                return ToolResult::error(std::string("write failed: ")
                                         + std::strerror(errno));
            }
            data += n;
            left -= (size_t) n;
        }
        ::close(fd);
        return ToolResult::ok("wrote " + std::to_string(content.size())
                              + " bytes to " + sb->virtual_path(p));
    };
}

// append: write `content` to the end of `path`, creating the file
// (and any missing parent dirs) if needed. Same hardening as write
// (sandbox containment, post-mkdir re-check, O_NOFOLLOW, mode 0600).
// Equivalent to action="write" with append=true, but exists as a
// first-class verb so ops batches can stack sequential appends
// without per-op `append:true` boilerplate.
ToolHandler make_fs_append_handler(std::shared_ptr<Sandbox> sb) {
    return [sb](const ToolCall & c) -> ToolResult {
        std::string path, content;
        if (!args::get_string(c.arguments_json, "path", path))
            return ToolResult::error("missing arg: path (fs action=\"append\")");
        if (!args::get_string(c.arguments_json, "content", content))
            return ToolResult::error("missing arg: content (fs action=\"append\")");

        stdfs::path p; std::string err;
        if (!sb->resolve(path, p, err)) return ToolResult::error(err);
        if (!sb->inside_sandbox(p)) {
            return ToolResult::error("path escapes sandbox via symlink: "
                                     + sb->virtual_path(p));
        }

        std::error_code ec;
        stdfs::create_directories(p.parent_path(), ec);
        // Re-check containment AFTER create_directories — same TOCTOU
        // defense as write.
        if (!sb->inside_sandbox(p)) {
            return ToolResult::error(
                "path escapes sandbox via symlink (post-mkdir): "
                + sb->virtual_path(p));
        }

        const int flags = O_WRONLY | O_CREAT | O_APPEND
                        | O_NOFOLLOW | O_CLOEXEC;
        int fd = ::open(p.c_str(), flags, 0600);
        if (fd < 0) {
            return ToolResult::error(std::string("cannot open for append: ")
                                     + sb->virtual_path(p)
                                     + " (" + std::strerror(errno) + ")");
        }
        const char * data = content.data();
        size_t       left = content.size();
        while (left > 0) {
            ssize_t n = ::write(fd, data, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                ::close(fd);
                return ToolResult::error(std::string("write failed: ")
                                         + std::strerror(errno));
            }
            data += n;
            left -= (size_t) n;
        }
        ::close(fd);
        return ToolResult::ok("appended " + std::to_string(content.size())
                              + " bytes to " + sb->virtual_path(p));
    };
}

// edit: replace lines [start_line..end_line] (1-based, inclusive) with
// `content`. Pure insert is end_line = start_line - 1 (zero-width
// range). Pure delete is content="". File must already exist (use
// action="write" to create). Atomic via tempfile + rename, same
// O_NOFOLLOW + post-mkdir TOCTOU defenses as write.
ToolHandler make_fs_edit_handler(std::shared_ptr<Sandbox> sb) {
    return [sb](const ToolCall & c) -> ToolResult {
        std::string path, content;
        long long start_line = 0, end_line = 0;
        if (!args::get_string(c.arguments_json, "path", path))
            return ToolResult::error("missing arg: path (fs action=\"edit\")");
        if (!args::get_int(c.arguments_json, "start_line", start_line))
            return ToolResult::error("missing arg: start_line (fs action=\"edit\")");
        if (!args::get_int(c.arguments_json, "end_line", end_line))
            return ToolResult::error("missing arg: end_line (fs action=\"edit\")");
        // content is required but allowed to be empty string (pure
        // delete). args::get_string returns false when the key is
        // absent; an explicit "" present in the JSON returns true.
        if (!args::get_string(c.arguments_json, "content", content))
            return ToolResult::error(
                "missing arg: content (fs action=\"edit\"). "
                "Pass content=\"\" for a pure delete.");

        stdfs::path p; std::string err;
        if (!sb->resolve(path, p, err)) return ToolResult::error(err);
        if (!sb->inside_sandbox(p)) {
            return ToolResult::error("path escapes sandbox via symlink: "
                                     + sb->virtual_path(p));
        }

        // Read the file. action="edit" never creates — that's
        // action="write"'s job. ENOENT is a clear error so the model
        // doesn't accidentally turn a missing-file into an empty file
        // by editing line 1.
        int rfd = ::open(p.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (rfd < 0) {
            return ToolResult::error(std::string("cannot open for edit: ")
                                     + sb->virtual_path(p)
                                     + " (" + std::strerror(errno) + ")."
                                     + " Use fs(action=\"write\") to create.");
        }
        std::string body;
        {
            constexpr size_t kEditMaxBytes = 8 * 1024 * 1024;  // 8 MiB
            char chunk[64 * 1024];
            for (;;) {
                ssize_t n = ::read(rfd, chunk, sizeof(chunk));
                if (n < 0) {
                    if (errno == EINTR) continue;
                    ::close(rfd);
                    return ToolResult::error(std::string("read failed: ")
                                             + std::strerror(errno));
                }
                if (n == 0) break;
                if (body.size() + (size_t) n > kEditMaxBytes) {
                    ::close(rfd);
                    return ToolResult::error(
                        "file too large for edit (>8 MiB); use action=\"write\" "
                        "with the new full content instead.");
                }
                body.append(chunk, (size_t) n);
            }
        }
        ::close(rfd);

        // Slice into lines. We track a "newline-terminated" flag for
        // each line so the rebuild preserves whether the file ended
        // in '\n' or not. Lines = 0 only if the file is empty.
        std::vector<std::string_view> lines;
        lines.reserve(body.size() / 32 + 1);
        size_t line_start = 0;
        for (size_t i = 0; i < body.size(); ++i) {
            if (body[i] == '\n') {
                lines.emplace_back(body.data() + line_start, i - line_start + 1);
                line_start = i + 1;
            }
        }
        if (line_start < body.size()) {
            lines.emplace_back(body.data() + line_start, body.size() - line_start);
        }
        const long long line_count = (long long) lines.size();

        // Validate range. Conventions:
        //   start_line ∈ [1, line_count + 1]   (line_count+1 = append at EOF)
        //   end_line   ∈ [start_line - 1, line_count]   (start_line-1 = pure insert)
        if (start_line < 1) {
            return ToolResult::error(
                "start_line must be >= 1 (got " + std::to_string(start_line) + ")");
        }
        if (start_line > line_count + 1) {
            return ToolResult::error(
                "start_line " + std::to_string(start_line)
                + " is past end of file (line_count=" + std::to_string(line_count)
                + "). Max is " + std::to_string(line_count + 1)
                + " (= append at EOF).");
        }
        if (end_line < start_line - 1) {
            return ToolResult::error(
                "end_line " + std::to_string(end_line)
                + " < start_line - 1 = " + std::to_string(start_line - 1)
                + ". For a pure insert before line N, pass start_line=N, "
                + "end_line=N-1.");
        }
        if (end_line > line_count) {
            return ToolResult::error(
                "end_line " + std::to_string(end_line)
                + " is past end of file (line_count=" + std::to_string(line_count)
                + ").");
        }

        const long long deleted = std::max<long long>(0, end_line - start_line + 1);

        // Capture old lines for the diff display.
        std::vector<std::string> old_lines_text;
        old_lines_text.reserve((size_t) deleted);
        for (long long i = start_line; i <= end_line && i <= line_count; ++i) {
            std::string_view sv = lines[(size_t)(i - 1)];
            while (!sv.empty()
                    && (sv.back() == '\n' || sv.back() == '\r'))
                sv.remove_suffix(1);
            old_lines_text.emplace_back(sv.data(), sv.size());
        }

        // Build new body: lines[0..start_line-2] + content + lines[end_line..]
        // (1-based to 0-based conversion: line N is lines[N-1]).
        std::string new_body;
        new_body.reserve(body.size() + content.size() + 32);
        for (long long i = 0; i < start_line - 1 && i < line_count; ++i) {
            new_body.append(lines[(size_t) i].data(), lines[(size_t) i].size());
        }
        // Auto-insert a '\n' on each side of `content` if the boundary
        // would otherwise glue two lines together.  The tool's line-level
        // contract says "replace lines [start..end] with content," so a
        // model passing `content="foo"` to replace one line expects the
        // result to occupy ONE line — not to fuse onto the next.  Without
        // these two guards, content that lacks a trailing '\n' silently
        // corrupts the file (a missing '}' on the seam-line is a common
        // model-induced compile failure).  Both guards no-op when the
        // contract is already satisfied (content with trailing '\n', or
        // a pure delete with content="", or append-at-EOF after a file
        // that already ended with '\n').
        if (!content.empty()
                && !new_body.empty()
                && new_body.back() != '\n') {
            new_body.push_back('\n');
        }
        new_body.append(content);
        const bool has_tail = (end_line < line_count);
        if (!content.empty()
                && content.back() != '\n'
                && has_tail) {
            new_body.push_back('\n');
        }
        for (long long i = end_line; i < line_count; ++i) {
            new_body.append(lines[(size_t) i].data(), lines[(size_t) i].size());
        }

        // Count inserted lines for the report. Counts a trailing
        // partial line as one line (consistent with how `lines`
        // splits the file).
        long long inserted = 0;
        if (!content.empty()) {
            inserted = 1;
            for (char ch : content) if (ch == '\n') ++inserted;
            if (content.back() == '\n') --inserted;
        }

        // Atomic write — same dance as fs write: tempfile + rename(2),
        // mode 0600, O_NOFOLLOW, re-check sandbox containment after
        // create_directories on the parent.
        std::error_code ec;
        stdfs::create_directories(p.parent_path(), ec);
        if (!sb->inside_sandbox(p)) {
            return ToolResult::error(
                "path escapes sandbox via symlink (post-mkdir): "
                + sb->virtual_path(p));
        }
        std::string tmp_path = p.string() + ".easyai-edit-tmp";
        int wfd = ::open(tmp_path.c_str(),
                         O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                         0600);
        if (wfd < 0) {
            return ToolResult::error(std::string("cannot open tempfile: ")
                                     + tmp_path + " (" + std::strerror(errno) + ")");
        }
        const char * data = new_body.data();
        size_t left = new_body.size();
        while (left > 0) {
            ssize_t n = ::write(wfd, data, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                ::close(wfd);
                ::unlink(tmp_path.c_str());
                return ToolResult::error(std::string("write failed: ")
                                         + std::strerror(errno));
            }
            data += n;
            left -= (size_t) n;
        }
        ::close(wfd);
        if (::rename(tmp_path.c_str(), p.c_str()) != 0) {
            const int e = errno;
            ::unlink(tmp_path.c_str());
            return ToolResult::error(std::string("rename failed: ")
                                     + std::strerror(e));
        }

        std::ostringstream o;
        o << "edited " << sb->virtual_path(p) << ": ";
        if (deleted == 0) {
            o << "inserted " << inserted << " line"
              << (inserted == 1 ? "" : "s")
              << " before line " << start_line;
        } else {
            o << "replaced lines " << start_line << "-" << end_line
              << " (" << deleted << " deleted, " << inserted << " inserted)";
        }
        o << "\n";

        // Show the old deleted lines so the model sees what it replaced.
        if (!old_lines_text.empty()) {
            for (long long i = 0; i < (long long) old_lines_text.size(); ++i) {
                o << "- " << std::setw(5) << (start_line + i) << ": "
                  << old_lines_text[(size_t) i] << "\n";
            }
        }

        std::vector<std::string_view> new_lines;
        {
            size_t ls = 0;
            for (size_t i = 0; i < new_body.size(); ++i) {
                if (new_body[i] == '\n') {
                    new_lines.emplace_back(new_body.data() + ls,
                                            i - ls + 1);
                    ls = i + 1;
                }
            }
            if (ls < new_body.size()) {
                new_lines.emplace_back(new_body.data() + ls,
                                        new_body.size() - ls);
            }
        }
        const long long new_line_count = (long long) new_lines.size();

        constexpr int kCtxLines        = 3;
        constexpr int kPerLineCap      = 200;

        long long win_start = std::max<long long>(1, start_line - kCtxLines);
        long long win_end;
        if (inserted > 0) {
            win_end = std::min<long long>(
                new_line_count, start_line + inserted - 1 + kCtxLines);
        } else {
            win_end = std::min<long long>(
                new_line_count, start_line + kCtxLines);
            if (start_line > new_line_count) {
                win_start = std::max<long long>(
                    1, new_line_count - 2 * kCtxLines);
                win_end = new_line_count;
            }
        }

        o << "; file now has " << new_line_count << " line"
          << (new_line_count == 1 ? "" : "s")
          << " (was " << line_count << ")";

        // Model-facing post-edit window (plain text, no ANSI).
        if (new_line_count > 0 && win_end >= win_start) {
            o << "; window [" << win_start << ".." << win_end << "]:\n";
            const long long inserted_first = start_line;
            const long long inserted_last  = start_line + inserted - 1;
            for (long long ln = win_start; ln <= win_end; ++ln) {
                std::string_view sv = new_lines[(size_t)(ln - 1)];
                while (!sv.empty()
                        && (sv.back() == '\n' || sv.back() == '\r'))
                    sv.remove_suffix(1);
                const bool is_ins =
                    (inserted > 0
                     && ln >= inserted_first
                     && ln <= inserted_last);
                o << (is_ins ? "+ " : "  ")
                  << std::setw(5) << ln << ": ";
                if ((long long) sv.size() > kPerLineCap) {
                    o.write(sv.data(), kPerLineCap);
                    o << "...[truncated]";
                } else {
                    o.write(sv.data(), (std::streamsize) sv.size());
                }
                o << "\n";
            }
        } else if (new_line_count == 0) {
            o << "; file is now empty";
        }

        // ANSI diff display for the terminal (ToolResult::display).
        // Dark red background for deleted lines, dark green for added,
        // dim background for context.  All lines padded to the same
        // width so the backgrounds form a solid rectangular block.
        static const char * kBgRed   = "\033[48;5;52m";
        static const char * kBgGreen = "\033[48;5;22m";
        static const char * kBgCtx   = "\033[48;5;236m\033[2m";
        static const char * kRst     = "\033[0m";
        static constexpr long long kBlockPad = 10;

        // First pass: find the widest content across all displayed lines.
        long long max_content = 0;
        auto measure = [&](std::string_view sv) {
            long long w = (long long) sv.size();
            if (w > kPerLineCap) w = kPerLineCap;
            if (w > max_content) max_content = w;
        };
        for (long long ln = win_start; ln < start_line && ln <= new_line_count; ++ln) {
            std::string_view sv = new_lines[(size_t)(ln - 1)];
            while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r'))
                sv.remove_suffix(1);
            measure(sv);
        }
        for (size_t i = 0; i < old_lines_text.size(); ++i)
            measure(old_lines_text[i]);
        if (inserted > 0) {
            for (long long ln = start_line;
                    ln < start_line + inserted && ln <= new_line_count; ++ln) {
                std::string_view sv = new_lines[(size_t)(ln - 1)];
                while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r'))
                    sv.remove_suffix(1);
                measure(sv);
            }
        }
        {
            long long ctx_after = start_line + inserted;
            for (long long ln = ctx_after; ln <= win_end && ln <= new_line_count; ++ln) {
                std::string_view sv = new_lines[(size_t)(ln - 1)];
                while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r'))
                    sv.remove_suffix(1);
                measure(sv);
            }
        }
        const long long block_content_w = max_content + kBlockPad;
        auto pad = [&](std::ostringstream & s, long long content_len) {
            long long n = block_content_w - content_len;
            for (long long p = 0; p < n; ++p) s << ' ';
        };

        std::ostringstream d;
        // Context before the edit point.
        for (long long ln = win_start; ln < start_line && ln <= new_line_count; ++ln) {
            std::string_view sv = new_lines[(size_t)(ln - 1)];
            while (!sv.empty()
                    && (sv.back() == '\n' || sv.back() == '\r'))
                sv.remove_suffix(1);
            char nb[32];
            long long clen = (long long) sv.size() > kPerLineCap ? kPerLineCap : (long long) sv.size();
            std::snprintf(nb, sizeof(nb), "%5lld", ln);
            d << kBgCtx << "  " << nb << ": ";
            d.write(sv.data(), (std::streamsize) clen);
            pad(d, clen);
            d << kRst << "\n";
        }
        // Deleted lines (old, with original line numbers).
        for (long long i = 0; i < (long long) old_lines_text.size(); ++i) {
            long long orig_ln = start_line + i;
            char nb[32];
            std::snprintf(nb, sizeof(nb), "%5lld", orig_ln);
            auto & sv = old_lines_text[(size_t) i];
            long long clen = (long long) sv.size() > kPerLineCap ? kPerLineCap : (long long) sv.size();
            d << kBgRed << "- " << nb << ": ";
            d.write(sv.data(), (std::streamsize) clen);
            pad(d, clen);
            d << kRst << "\n";
        }
        // Inserted lines (new content, with new line numbers).
        if (inserted > 0) {
            for (long long ln = start_line;
                    ln < start_line + inserted && ln <= new_line_count; ++ln) {
                std::string_view sv = new_lines[(size_t)(ln - 1)];
                while (!sv.empty()
                        && (sv.back() == '\n' || sv.back() == '\r'))
                    sv.remove_suffix(1);
                char nb[32];
                long long clen = (long long) sv.size() > kPerLineCap ? kPerLineCap : (long long) sv.size();
                std::snprintf(nb, sizeof(nb), "%5lld", ln);
                d << kBgGreen << "+ " << nb << ": ";
                d.write(sv.data(), (std::streamsize) clen);
                pad(d, clen);
                d << kRst << "\n";
            }
        }
        // Context after the edit point.
        long long ctx_after_start = start_line + inserted;
        for (long long ln = ctx_after_start;
                ln <= win_end && ln <= new_line_count; ++ln) {
            std::string_view sv = new_lines[(size_t)(ln - 1)];
            while (!sv.empty()
                    && (sv.back() == '\n' || sv.back() == '\r'))
                sv.remove_suffix(1);
            char nb[32];
            long long clen = (long long) sv.size() > kPerLineCap ? kPerLineCap : (long long) sv.size();
            std::snprintf(nb, sizeof(nb), "%5lld", ln);
            d << kBgCtx << "  " << nb << ": ";
            d.write(sv.data(), (std::streamsize) clen);
            pad(d, clen);
            d << kRst << "\n";
        }

        return ToolResult::ok_display(o.str(), d.str());
    };
}

ToolHandler make_fs_list_handler(std::shared_ptr<Sandbox> sb) {
    return [sb](const ToolCall & c) -> ToolResult {
        std::string path;
        // path is OPTIONAL — empty / missing defaults to ".", the
        // sandbox root.  Matches the glob / grep convention so the
        // model can ask "what's in the workspace" without remembering
        // to spell out the dot.
        args::get_string(c.arguments_json, "path", path);
        if (path.empty()) path = ".";

        stdfs::path p; std::string err;
        if (!sb->resolve(path, p, err)) return ToolResult::error(err);
        if (!sb->inside_sandbox(p)) {
            return ToolResult::error("path escapes sandbox via symlink: "
                                     + sb->virtual_path(p));
        }
        // Dispatch on what `path` actually is.  A regular file isn't
        // listable but the model probably wanted action="read"; saying
        // so explicitly saves a retry round-trip.  Anything else (missing
        // / FIFO / device) gets a clean error too.
        std::error_code ec_kind;
        if (stdfs::is_regular_file(p, ec_kind)) {
            return ToolResult::error(
                std::string("path is a file, not a directory: ")
                + sb->virtual_path(p)
                + " — use action=\"read\" to view its contents, or "
                "action=\"check_path\" for metadata.");
        }
        if (!stdfs::is_directory(p, ec_kind)) {
            return ToolResult::error(
                std::string("not a directory: ") + sb->virtual_path(p)
                + (ec_kind ? std::string(" (") + ec_kind.message() + ")"
                           : std::string()));
        }

        std::ostringstream o;
        for (auto & e : stdfs::directory_iterator(p)) {
            o << (e.is_directory() ? "d " : "f ") << e.path().filename().string();
            if (e.is_regular_file()) {
                std::error_code ec;
                auto sz = stdfs::file_size(e.path(), ec);
                if (!ec) o << "  (" << sz << " B)";
            }
            o << "\n";
        }
        return ToolResult::ok(clip(o.str(), 16 * 1024));
    };
}

ToolHandler make_fs_glob_handler(std::shared_ptr<Sandbox> sb) {
    return [sb](const ToolCall & c) -> ToolResult {
        std::string pattern, sub;
        if (!args::get_string(c.arguments_json, "pattern", pattern))
            return ToolResult::error("missing arg: pattern (fs action=\"glob\")");
        args::get_string(c.arguments_json, "path", sub);

        stdfs::path start = sb->root; std::string err;
        if (!sub.empty() && !sb->resolve(sub, start, err))
            return ToolResult::error(err);
        if (!sb->inside_sandbox(start)) {
            return ToolResult::error("start dir escapes sandbox via symlink: "
                                     + sb->virtual_path(start));
        }
        // Precheck: a clear `error:` line beats a stray
        // `filesystem_error` exception when the model passes a path
        // that doesn't exist or names a regular file.
        std::error_code ec_pre;
        if (!stdfs::is_directory(start, ec_pre)) {
            return ToolResult::error(
                std::string("not a directory: ") + sb->virtual_path(start)
                + (ec_pre ? std::string(" (") + ec_pre.message() + ")"
                          : std::string()));
        }

        std::regex rx;
        try { rx = std::regex(glob_to_regex(pattern)); }
        catch (const std::regex_error & e) {
            return ToolResult::error(std::string("bad glob pattern: ") + e.what());
        }

        // skip_permission_denied lets the iterator silently step past
        // 0700/0000 subdirs the running user can't enumerate instead
        // of throwing mid-loop. Combined with the ec constructor and
        // the ec-overloads on per-entry queries below, glob degrades
        // gracefully across mixed-perms sandboxes (e.g. `--sandbox
        // $HOME` with a ~/.gnupg in it).
        constexpr auto kIterOpts =
            stdfs::directory_options::skip_permission_denied;
        std::error_code ec_it;
        stdfs::recursive_directory_iterator it(start, kIterOpts, ec_it);
        if (ec_it) {
            return ToolResult::error(
                std::string("cannot iterate: ") + sb->virtual_path(start)
                + " (" + ec_it.message() + ")");
        }
        const stdfs::recursive_directory_iterator end;

        std::ostringstream o;
        int n = 0;
        // Hand-rolled loop with ec-aware increment so a flake mid-
        // traversal (vanished entry, race) skips the bad subtree
        // instead of tearing down the whole call. Ranged-for would
        // call the throwing operator++ overload.
        for (; it != end; ) {
            std::error_code ec_q;
            if (it->is_regular_file(ec_q) && !ec_q) {
                std::string rel =
                    stdfs::relative(it->path(), sb->root, ec_q).generic_string();
                if (!ec_q) {
                    bool m = false;
                    try { m = std::regex_match(rel, rx); }
                    catch (const std::regex_error &) { /* skip entry */ }
                    if (m) {
                        o << sb->virtual_path(it->path()) << "\n";
                        if (++n >= 500) {
                            o << "...[stopped at 500 matches]\n";
                            break;
                        }
                    }
                }
            }
            std::error_code ec_step;
            it.increment(ec_step);
            if (ec_step) {
                if (it == end) break;
                it.pop();
                if (it == end) break;
            }
        }
        if (n == 0) return ToolResult::ok("No matches.");
        return ToolResult::ok(o.str());
    };
}

ToolHandler make_fs_grep_handler(std::shared_ptr<Sandbox> sb) {
    return [sb](const ToolCall & c) -> ToolResult {
        std::string pattern, sub, file_glob;
        long long max_matches = 100;
        bool ci = false;
        if (!args::get_string(c.arguments_json, "pattern", pattern))
            return ToolResult::error("missing arg: pattern (fs action=\"grep\")");
        args::get_string(c.arguments_json, "path",      sub);
        args::get_string(c.arguments_json, "file_glob", file_glob);
        args::get_int   (c.arguments_json, "max_matches", max_matches);
        args::get_bool  (c.arguments_json, "case_insensitive", ci);

        stdfs::path start = sb->root; std::string err;
        if (!sub.empty() && !sb->resolve(sub, start, err))
            return ToolResult::error(err);
        if (!sb->inside_sandbox(start)) {
            return ToolResult::error("path escapes sandbox via symlink: "
                                     + sb->virtual_path(start));
        }
        // Dispatch on what `start` actually is — mirroring `grep -r`'s
        // behaviour: a file is searched as one file, a directory is
        // walked recursively.  Earlier versions of this handler required
        // a directory and erred on a regular-file path, which surprised
        // models that called `fs(action="grep", path="foo.c", ...)`
        // intending to search that specific file.
        std::error_code ec_kind;
        const bool start_is_file = stdfs::is_regular_file(start, ec_kind);
        const bool start_is_dir  = !start_is_file
                                && stdfs::is_directory(start, ec_kind);
        if (!start_is_file && !start_is_dir) {
            return ToolResult::error(
                std::string("not a regular file or directory: ")
                + sb->virtual_path(start)
                + (ec_kind ? std::string(" (") + ec_kind.message() + ")"
                           : std::string()));
        }

        std::regex::flag_type rf = std::regex::ECMAScript;
        if (ci) rf |= std::regex::icase;
        std::regex rx;
        try { rx = std::regex(pattern, rf); }
        catch (const std::regex_error & e) {
            return ToolResult::error(std::string("bad regex: ") + e.what());
        }

        std::regex glob_rx(".*");
        if (!file_glob.empty()) {
            std::string r = "^";
            for (char ch : file_glob) {
                if      (ch == '*') r += "[^/]*";
                else if (ch == '?') r += "[^/]";
                else if (std::strchr(".+()|^$\\{}[]", ch)) { r += '\\'; r += ch; }
                else r += ch;
            }
            r += "$";
            try { glob_rx = std::regex(r); }
            catch (const std::regex_error & e) {
                return ToolResult::error(std::string("bad file_glob: ") + e.what());
            }
        }

        std::ostringstream o;
        int  n         = 0;
        bool budget_hit = false;

        // Scan a single regular file against the compiled regex.  Shared
        // between the single-file and recursive-directory dispatch paths
        // so the matching, size cap, line cap, and output formatting are
        // identical regardless of how `start` was reached.
        auto scan_file = [&](const stdfs::path & p) {
            std::error_code ec_q;
            const auto sz = stdfs::file_size(p, ec_q);
            if (ec_q || sz > 4 * 1024 * 1024) return;
            std::ifstream f(p);
            if (!f) return;
            std::string line; int lineno = 0;
            const std::string vpath = sb->virtual_path(p);
            while (std::getline(f, line)) {
                ++lineno;
                // libstdc++'s regex engine is recursive; running a
                // user-supplied pattern against a multi-megabyte
                // single line (binary blob, minified JS, base64
                // dump) is a DoS vector via catastrophic
                // backtracking. 64 KiB is plenty for source code
                // and short of the failure regime.
                if (line.size() > 64 * 1024) continue;
                if (std::regex_search(line, rx)) {
                    o << vpath << ":" << lineno << ": "
                      << clip(line, 240) << "\n";
                    if (++n >= max_matches) { budget_hit = true; return; }
                }
            }
        };

        if (start_is_file) {
            // `file_glob` is a directory-walk filter — irrelevant when
            // the caller has already pointed `path` at a specific file.
            // Apply it only as a name guard so a typo'd call like
            // path="foo.c" file_glob="*.py" still produces "no matches"
            // instead of silently grepping foo.c against a Python
            // filter the caller probably intended for a dir walk.
            std::string fname = start.filename().string();
            if (file_glob.empty()
                    || std::regex_match(fname, glob_rx)) {
                scan_file(start);
            }
        } else {
            constexpr auto kIterOpts =
                stdfs::directory_options::skip_permission_denied;
            std::error_code ec_it;
            stdfs::recursive_directory_iterator it(start, kIterOpts, ec_it);
            if (ec_it) {
                return ToolResult::error(
                    std::string("cannot iterate: ") + sb->virtual_path(start)
                    + " (" + ec_it.message() + ")");
            }
            const stdfs::recursive_directory_iterator end;
            for (; it != end && !budget_hit; ) {
                std::error_code ec_q;
                const bool is_reg = it->is_regular_file(ec_q);
                if (!ec_q && is_reg) {
                    std::string fname = it->path().filename().string();
                    if (file_glob.empty()
                            || std::regex_match(fname, glob_rx)) {
                        scan_file(it->path());
                    }
                }
                std::error_code ec_step;
                it.increment(ec_step);
                if (ec_step) {
                    if (it == end) break;
                    it.pop();
                    if (it == end) break;
                }
            }
        }

        if (n == 0) return ToolResult::ok("No matches.");
        return ToolResult::ok(clip(o.str(), 32 * 1024));
    };
}

ToolHandler make_fs_check_path_handler(std::shared_ptr<Sandbox> sb) {
    return [sb](const ToolCall & c) -> ToolResult {
        std::string path; bool touch = false;
        if (!args::get_string(c.arguments_json, "path", path))
            return ToolResult::error("missing arg: path (fs action=\"check_path\")");
        args::get_bool(c.arguments_json, "touch", touch);

        stdfs::path p; std::string err;
        if (!sb->resolve(path, p, err)) return ToolResult::error(err);
        if (!sb->inside_sandbox(p)) {
            return ToolResult::error("path escapes sandbox via symlink: "
                                     + sb->virtual_path(p));
        }

        // Touch path if requested AND it doesn't exist. Symmetric with
        // the write handler: O_NOFOLLOW guards against a last-component
        // symlink, parent dirs are created, mode 0600 same as model-
        // written files.
        bool created = false;
        std::error_code ec_exist;
        const bool exists_pre = stdfs::exists(p, ec_exist);
        if (touch && !exists_pre) {
            std::error_code ec_mk;
            if (!p.parent_path().empty()) {
                stdfs::create_directories(p.parent_path(), ec_mk);
            }
            if (!sb->inside_sandbox(p)) {
                return ToolResult::error(
                    "path escapes sandbox via symlink (post-mkdir): "
                    + sb->virtual_path(p));
            }
            int fd = ::open(p.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL
                              | O_NOFOLLOW | O_CLOEXEC,
                            0600);
            if (fd < 0) {
                return ToolResult::error(
                    std::string("touch failed: ") + sb->virtual_path(p)
                    + " (" + std::strerror(errno) + ")");
            }
            ::close(fd);
            created = true;
        }

        struct stat st {};
        const int    lstat_rc  = ::lstat(p.c_str(), &st);
        const int    lstat_err = (lstat_rc == 0) ? 0 : errno;
        const bool   exists_now = (lstat_rc == 0);

        std::ostringstream o;
        o << "path: "     << path << "\n";
        o << "absolute: " << p.string() << "\n";

        if (!exists_now) {
            if (lstat_err == ENOENT) {
                o << "exists: no\n";
                o << "type: missing\n";
            } else {
                o << "exists: unknown\n";
                o << "type: unknown\n";
                o << "error: lstat: " << std::strerror(lstat_err) << "\n";
            }
            if (touch && !created && lstat_err == ENOENT) {
                o << "note: vanished between exists check and lstat\n";
            }
            return ToolResult::ok(o.str());
        }

        o << "exists: yes\n";
        const mode_t m = st.st_mode;
        const char * type_str = "other";
        if      (S_ISREG (m)) type_str = "file";
        else if (S_ISDIR (m)) type_str = "directory";
        else if (S_ISLNK (m)) type_str = "symlink";
        else if (S_ISCHR (m)) type_str = "char-device";
        else if (S_ISBLK (m)) type_str = "block-device";
        else if (S_ISFIFO(m)) type_str = "fifo";
        else if (S_ISSOCK(m)) type_str = "socket";
        o << "type: " << type_str << "\n";

        if (S_ISREG(m)) {
            o << "size: " << (long long) st.st_size << "\n";
        }
        char modebuf[8];
        std::snprintf(modebuf, sizeof(modebuf), "0%o",
                      (unsigned) (m & 07777));
        o << "mode: " << modebuf << "\n";

        o << "readable: "   << (::access(p.c_str(), R_OK) == 0 ? "yes" : "no") << "\n";
        o << "writable: "   << (::access(p.c_str(), W_OK) == 0 ? "yes" : "no") << "\n";
        o << "executable: " << (::access(p.c_str(), X_OK) == 0 ? "yes" : "no") << "\n";

        char tbuf[40] = {0};
        struct tm tmv;
        const time_t mt = (time_t) st.st_mtime;
        if (::gmtime_r(&mt, &tmv) != nullptr) {
            std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
        }
        if (tbuf[0]) o << "mtime: " << tbuf << "\n";

        if (created) o << "created: yes\n";
        return ToolResult::ok(o.str());
    };
}

ToolHandler make_fs_cwd_handler() {
    return [](const ToolCall & /*c*/) -> ToolResult {
        char buf[PATH_MAX];
        if (::getcwd(buf, sizeof(buf)) == nullptr) {
            return ToolResult::error(
                std::string("getcwd failed: ") + std::strerror(errno));
        }
        const size_t n = ::strnlen(buf, sizeof(buf));
        return ToolResult::ok(std::string(buf, n));
    };
}

ToolHandler make_fs_sandbox_handler(const std::string & root) {
    // Pin the canonical root at registration. Falls back to absolute(root)
    // if weakly_canonical fails (transient ENOENT etc.); never to the raw
    // input because relative strings like "./" leak the operator's cwd
    // shape and break the model's "absolute path" expectation.
    std::string resolved;
    if (!root.empty() && root != ".") {
        std::error_code ec;
        stdfs::path canon = stdfs::weakly_canonical(stdfs::path(root), ec);
        if (ec || canon.empty()) {
            ec.clear();
            canon = stdfs::absolute(stdfs::path(root), ec);
        }
        if (!ec && !canon.empty()) {
            resolved = canon.string();
        }
    }
    return [resolved](const ToolCall & /*c*/) -> ToolResult {
        if (!resolved.empty()) return ToolResult::ok(resolved);
        // Fallback: no sandbox configured — answer with the process's
        // current cwd, which is what `bash` actually uses with root=".".
        char buf[PATH_MAX];
        if (::getcwd(buf, sizeof(buf)) == nullptr) {
            return ToolResult::error(
                std::string("getcwd failed: ") + std::strerror(errno));
        }
        const size_t n = ::strnlen(buf, sizeof(buf));
        return ToolResult::ok(std::string(buf, n));
    };
}

}  // namespace

Tool fs(std::string root) {
    auto sb = std::make_shared<Sandbox>(root);
    auto h_read       = make_fs_read_handler      (sb);
    auto h_write      = make_fs_write_handler     (sb);
    auto h_append     = make_fs_append_handler    (sb);
    auto h_edit       = make_fs_edit_handler      (sb);
    auto h_list       = make_fs_list_handler      (sb);
    auto h_glob       = make_fs_glob_handler      (sb);
    auto h_grep       = make_fs_grep_handler      (sb);
    auto h_check_path = make_fs_check_path_handler(sb);
    auto h_cwd        = make_fs_cwd_handler       ();
    auto h_sandbox    = make_fs_sandbox_handler   (root);

    // Per-action dispatcher used by both the single-call and the
    // ops-batch paths. `args_json` is the per-op JSON object — for
    // single-call it's c.arguments_json verbatim; for batch it's
    // ops[i].dump().
    auto dispatch_single =
        [h_read, h_write, h_append, h_edit, h_list, h_glob, h_grep,
         h_check_path, h_cwd, h_sandbox]
        (const std::string & action,
         const std::string & args_json) -> ToolResult {
            ToolCall sub;
            sub.name = "fs";
            sub.arguments_json = args_json;
            if (action == "read")        return h_read(sub);
            if (action == "write")       return h_write(sub);
            if (action == "append")      return h_append(sub);
            if (action == "edit")        return h_edit(sub);
            if (action == "list")        return h_list(sub);
            if (action == "glob")        return h_glob(sub);
            if (action == "grep")        return h_grep(sub);
            if (action == "check_path")  return h_check_path(sub);
            if (action == "cwd")         return h_cwd(sub);
            if (action == "sandbox")     return h_sandbox(sub);
            return ToolResult::error(
                "unknown action \"" + action + "\". Valid: \"read\", "
                "\"write\", \"append\", \"edit\", \"list\", \"glob\", "
                "\"grep\", \"check_path\", \"cwd\", \"sandbox\".");
        };

    return Tool::builder("fs")
        .short_describe(
            "Filesystem: read/write/edit/list/glob/grep/cwd/sandbox in "
            "sandbox. Batch with action=\"ops\". The ONLY write tool besides bash.")
        .describe(
            "Filesystem — one tool, ten actions + batch mode.\n"
            "\n"
            "PATHS are RELATIVE to the sandbox root (`report.md`, "
            "`src/main.cpp`, `.` for root). Never prefix with `/`.\n"
            "\n"
            "PRE-FLIGHT: at the start of any file work run "
            "action=\"sandbox\" once (absolute root) then "
            "action=\"check_path\" on the target. Skipping causes "
            "guess-and-fail loops.\n"
            "\n"
            "action=\"read\"        path → file content. Output "
            "prefixes every line with `<n>| ` and reports total line "
            "count — read before edit to get accurate line refs.\n"
            "  Line mode (recommended for editing): start_line (1-based), "
            "limit = lines (default 200, max 2000).\n"
            "  Byte mode: offset (default 0), limit (default 65536 bytes, "
            "max 1048576). line_numbers on by default.\n"
            "\n"
            "action=\"write\"       path, content → overwrites the "
            "file. Creates parent dirs.\n"
            "  Optional: append (true → append instead of overwrite).\n"
            "\n"
            "action=\"append\"      path, content → adds to end of "
            "file (creates if missing).\n"
            "\n"
            "action=\"edit\"        path, start_line, end_line, "
            "content → replaces lines [start..end] (1-based, "
            "inclusive). Atomic.\n"
            "  content=\"\" deletes the range; end_line=start-1 "
            "inserts before start_line; start=line_count+1 appends "
            "at EOF. File must exist — use write to create.\n"
            "\n"
            "action=\"list\"        path → entries one per line "
            "(`d`/`f` prefix + size). Non-recursive.\n"
            "\n"
            "action=\"glob\"        pattern → matching files, "
            "recursive. `*` single segment, `**` crosses dirs, `?` "
            "one char, `[abc]` a set.\n"
            "  Optional: path (start directory, default sandbox root).\n"
            "\n"
            "action=\"grep\"        pattern → file contents matching "
            "an ECMAScript regex.\n"
            "  Optional: path, file_glob, max_matches (default 100), "
            "case_insensitive.\n"
            "\n"
            "action=\"check_path\"  path → existence, type, size, "
            "r/w/x rights.\n"
            "  Optional: touch (create empty file if missing — probes "
            "write access).\n"
            "\n"
            "action=\"cwd\"         → current working directory.\n"
            "\n"
            "action=\"sandbox\"     → absolute sandbox root. Use it "
            "only for user-facing paths or external commands; for "
            "fs/bash work pass RELATIVE paths.\n"
            "\n"
            "BATCH: pass `ops` (array of 1..50 op objects, each with "
            "`action` plus its params) instead of `action`. Hard "
            "caps per call: 50 ops total AND 20 distinct file paths "
            "(only ops that name a `path` count — `cwd`/`sandbox`/"
            "`glob`/`grep`/`list` are free). Same-path edits "
            "auto-reorder bottom-up so each `start_line` refers to "
            "the file's ORIGINAL line numbers (no manual offset math). "
            "Pass continue_on_error=true to keep going after a failed "
            "op; default is stop-on-first.\n"
            "\n"
            "BATCH EXAMPLE — three edits to one file plus a write:\n"
            "  {\"ops\":[\n"
            "    {\"action\":\"edit\",\"path\":\"a.c\",\"start_line\":5,\"end_line\":5,\"content\":\"  int x = 0;\\n\"},\n"
            "    {\"action\":\"edit\",\"path\":\"a.c\",\"start_line\":20,\"end_line\":25,\"content\":\"\"},\n"
            "    {\"action\":\"edit\",\"path\":\"a.c\",\"start_line\":42,\"end_line\":42,\"content\":\"// done\\n\"},\n"
            "    {\"action\":\"write\",\"path\":\"NOTES.md\",\"content\":\"refactor pass 1\\n\"}\n"
            "  ]}\n"
            "\n"
            "BATCH RESPONSE: header line names the touched files; each "
            "op gets one report line `[i/N] ok|err action path: …`. "
            "Successful reads are clipped to 2 KiB per op (re-issue a "
            "single read for full content). Failed ops show the full "
            "error so you can self-correct without re-running. "
            "Hitting the 50-op or 20-file cap → split into multiple "
            "batches (e.g. one batch per file, or chunks of 25)."
        )
        .param("action",            "string",
               "Required unless `ops` is set. One of: \"read\", "
               "\"write\", \"append\", \"edit\", \"list\", \"glob\", "
               "\"grep\", \"check_path\", \"cwd\", \"sandbox\".",
               false)
        .param("ops",               "array",
               "Batch mode: 1..50 op objects, each with `action` plus "
               "its params. Set `ops` OR `action`, not both. Hard "
               "caps: 50 ops AND 20 distinct file paths per call. "
               "Same-path edits auto-reorder bottom-up.", false)
        .param("continue_on_error", "boolean",
               "Batch only. Keep running after a failed op. Default "
               "false.", false)
        .param("path",              "string",
               "RELATIVE path under the sandbox root. `.` for root. "
               "No leading `/`.", false)
        .param("content",           "string",
               "UTF-8 text for write/append/edit. Use `\\n` for "
               "newlines. \"\" deletes the range for edit.", false)
        .param("append",            "boolean",
               "write only. Append instead of overwrite. Default "
               "false.", false)
        .param("start_line",        "integer",
               "read+edit. 1-based. read: first line to return "
               "(switches to line mode — limit becomes line count). "
               "edit: first line to replace; line_count+1 appends "
               "at EOF.", false)
        .param("end_line",          "integer",
               "edit only. 1-based, inclusive. start_line-1 inserts "
               "before start_line.", false)
        .param("offset",            "integer",
               "read byte-mode only. Byte offset, default 0.", false)
        .param("limit",             "integer",
               "read: line count when start_line set (default 200, "
               "max 2000); byte count otherwise (default 65536, "
               "max 1048576).",
               false)
        .param("line_numbers",      "boolean",
               "read only. Prefix each line `<n>| `. Default false.",
               false)
        .param("pattern",           "string",
               "glob: wildcard. grep: ECMAScript regex (matched per "
               "line; anchor with `^`/`$`).", false)
        .param("file_glob",         "string",
               "grep only. Restrict filenames by basename pattern "
               "(e.g. `*.cpp`).", false)
        .param("max_matches",       "integer",
               "grep only. Stop after N matches. Default 100.", false)
        .param("case_insensitive",  "boolean",
               "grep only. Default false.", false)
        .param("touch",             "boolean",
               "check_path only. Create an empty file if missing "
               "(probes write access). Default false.", false)
        .handle([dispatch_single]
                (const ToolCall & c) -> ToolResult {
            // Detect the batch shape via raw nlohmann::json: an `ops`
            // key whose value is an array. The args helpers in
            // easyai::args don't have an array getter, so we parse
            // once here and route accordingly.
            nlohmann::json parsed;
            try {
                parsed = nlohmann::json::parse(c.arguments_json);
            } catch (const std::exception & e) {
                return ToolResult::error(
                    std::string("invalid JSON arguments: ") + e.what());
            }
            const bool has_action = parsed.contains("action")
                && parsed["action"].is_string()
                && !parsed["action"].get<std::string>().empty();
            const bool has_ops = parsed.contains("ops")
                && parsed["ops"].is_array();

            // Single-op path (default).
            if (!has_ops) {
                if (!has_action) {
                    return ToolResult::error(
                        "missing required argument: either \"action\" "
                        "(for a single op) or \"ops\" (for a batch). "
                        "Single-op valid action values: \"read\", "
                        "\"write\", \"append\", \"edit\", \"list\", "
                        "\"glob\", \"grep\", \"check_path\", \"cwd\", "
                        "\"sandbox\".");
                }
                return dispatch_single(parsed["action"].get<std::string>(),
                                       c.arguments_json);
            }

            // Batch path. Validate.
            if (has_action) {
                return ToolResult::error(
                    "\"action\" and \"ops\" are mutually exclusive. "
                    "Pass `action` for a single op OR `ops` for a "
                    "batch, not both.");
            }
            const auto & ops_arr = parsed["ops"];
            if (ops_arr.empty()) {
                return ToolResult::error("ops array is empty");
            }
            // Per-call hard caps. Raised from 20→50 ops on 2026-05-26
            // and a NEW 20-distinct-files cap added so a runaway model
            // can't blast 50 different files in one call by accident.
            // The file cap is computed only over ops that actually
            // name a `path` — cwd / sandbox / glob / grep / list are
            // "free" (they don't bind to a single file).
            constexpr size_t kMaxBatch         = 50;
            constexpr size_t kMaxDistinctFiles = 20;
            if (ops_arr.size() > kMaxBatch) {
                return ToolResult::error(
                    "ops array has " + std::to_string(ops_arr.size())
                    + " items; cap is " + std::to_string(kMaxBatch)
                    + " per call. Split into multiple calls "
                    "(e.g. one per file, or chunks of "
                    + std::to_string(kMaxBatch / 2) + ").");
            }
            const bool continue_on_error =
                parsed.contains("continue_on_error")
                && parsed["continue_on_error"].is_boolean()
                && parsed["continue_on_error"].get<bool>();

            // Pre-validate every op so we can surface schema errors
            // up front (no half-batched state from a 5th malformed op).
            // Each op must be an object with a string `action` field.
            // Also collect the set of distinct paths so the file-cap
            // and the batch-header summary share one walk of the array.
            const size_t N = ops_arr.size();
            std::vector<std::string> file_order;     // insertion order
            std::set<std::string>    file_seen;
            for (size_t i = 0; i < N; ++i) {
                if (!ops_arr[i].is_object()) {
                    return ToolResult::error(
                        "ops[" + std::to_string(i) + "] is not an object");
                }
                if (!ops_arr[i].contains("action")
                        || !ops_arr[i]["action"].is_string()
                        || ops_arr[i]["action"].get<std::string>().empty()) {
                    return ToolResult::error(
                        "ops[" + std::to_string(i)
                        + "] is missing string field \"action\"");
                }
                if (ops_arr[i].contains("path")
                        && ops_arr[i]["path"].is_string()) {
                    const std::string p = ops_arr[i]["path"].get<std::string>();
                    if (!p.empty() && file_seen.insert(p).second) {
                        file_order.push_back(p);
                    }
                }
            }
            if (file_order.size() > kMaxDistinctFiles) {
                return ToolResult::error(
                    "ops touches " + std::to_string(file_order.size())
                    + " distinct files; cap is "
                    + std::to_string(kMaxDistinctFiles)
                    + " per call. Split into multiple calls — group "
                    "ops by file so each batch stays under the cap.");
            }

            // Compute execute order. Same-path edits are reordered
            // bottom-up (highest start_line first) so each edit's
            // line numbers refer to the file's ORIGINAL state. The
            // position of every NON-edit op is preserved; edit slots
            // get refilled with the appropriate same-path edit.
            std::map<std::string, std::vector<size_t>> edits_by_path;
            for (size_t i = 0; i < N; ++i) {
                const auto & op = ops_arr[i];
                if (op["action"].get<std::string>() == "edit"
                        && op.contains("path")
                        && op["path"].is_string()) {
                    edits_by_path[op["path"].get<std::string>()].push_back(i);
                }
            }
            for (auto & kv : edits_by_path) {
                std::sort(kv.second.begin(), kv.second.end(),
                    [&](size_t a, size_t b) {
                        long long sa = ops_arr[a].value("start_line", 0LL);
                        long long sb = ops_arr[b].value("start_line", 0LL);
                        return sa > sb;  // descending
                    });
            }
            std::vector<size_t> exec_order;
            exec_order.reserve(N);
            std::map<std::string, size_t> next_for_path;
            for (size_t i = 0; i < N; ++i) {
                const auto & op = ops_arr[i];
                const std::string action = op["action"].get<std::string>();
                if (action == "edit" && op.contains("path")
                        && op["path"].is_string()) {
                    const std::string p = op["path"].get<std::string>();
                    auto & sorted = edits_by_path[p];
                    exec_order.push_back(sorted[next_for_path[p]++]);
                } else {
                    exec_order.push_back(i);
                }
            }

            // Run.
            std::ostringstream out;

            // Batch header — names the touched files in insertion
            // order so the model can verify it dispatched against the
            // right set before reading per-op statuses. Listing 1..20
            // file names is bounded so we render them all.
            out << "batch: " << N << " op" << (N == 1 ? "" : "s")
                << " across " << file_order.size()
                << " file" << (file_order.size() == 1 ? "" : "s");
            if (!file_order.empty()) {
                out << " (";
                for (size_t i = 0; i < file_order.size(); ++i) {
                    if (i) out << ", ";
                    out << file_order[i];
                }
                out << ")";
            }
            out << "\n";

            // Per-op output rules:
            //   * Successful read in a batch is clipped at 2 KiB so a
            //     50-op batch of reads can't blow up to multi-MB; the
            //     model re-issues a standalone read for full content.
            //   * Failed ops show the full error body (small models
            //     can self-correct without re-running).
            //   * All other ok bodies fit in a few hundred bytes
            //     naturally (write returns "wrote N bytes", edit
            //     returns "replaced lines …"), no clipping needed.
            constexpr std::size_t kBatchReadClip = 2048;

            int ok_count = 0;
            int err_count = 0;
            size_t ran = 0;
            for (size_t step = 0; step < N; ++step) {
                const size_t i = exec_order[step];
                const auto & op = ops_arr[i];
                const std::string action = op["action"].get<std::string>();
                const std::string args_json = op.dump();
                ToolResult r = dispatch_single(action, args_json);

                // Build a short target hint for the report line so
                // operators reading the transcript can see at a
                // glance which path/pattern each op touched. Order
                // mirrors what's actually meaningful per action.
                std::string target;
                if (op.contains("path") && op["path"].is_string()) {
                    target = op["path"].get<std::string>();
                } else if (op.contains("pattern")
                        && op["pattern"].is_string()) {
                    target = op["pattern"].get<std::string>();
                }
                if (!target.empty()) target = " " + target;

                out << "[" << (step + 1) << "/" << N << "] "
                    << (r.is_error ? "err " : "ok ")
                    << action << target;
                if (i != step) {
                    // Edit-reorder happened: tell the operator
                    // which submission index this slot came from.
                    out << " (submitted as ops[" << i << "])";
                }
                out << ": ";

                // Apply per-op clipping rules.
                std::string body = r.content;
                bool clipped = false;
                if (!r.is_error && action == "read"
                        && body.size() > kBatchReadClip) {
                    body.resize(kBatchReadClip);
                    clipped = true;
                }

                // First line of the op's body — keep the per-op line
                // tight; the model sees the full op content via the
                // continuation block below for edits / read /
                // check_path that produce multi-line output. Errors
                // ALWAYS show the full body (the model needs the
                // diagnostic to self-correct on the next call).
                size_t nl = body.find('\n');
                if (nl == std::string::npos) {
                    out << body << "\n";
                } else {
                    out << body.substr(0, nl) << "\n";
                    // Multi-line bodies are indented and shown in
                    // full so the model can read e.g. a check_path
                    // dump or a read result inline.
                    std::string rest = body.substr(nl + 1);
                    if (!rest.empty()) {
                        // Indent every continuation line by 4 spaces.
                        std::string indented;
                        indented.reserve(rest.size() + rest.size() / 64);
                        size_t pos = 0;
                        while (pos < rest.size()) {
                            size_t e = rest.find('\n', pos);
                            indented.append("    ");
                            if (e == std::string::npos) {
                                indented.append(rest, pos, std::string::npos);
                                pos = rest.size();
                            } else {
                                indented.append(rest, pos, e - pos + 1);
                                pos = e + 1;
                            }
                        }
                        out << indented;
                        if (!indented.empty()
                                && indented.back() != '\n') {
                            out << "\n";
                        }
                    }
                }
                if (clipped) {
                    out << "    [read clipped at "
                        << kBatchReadClip
                        << " bytes; re-issue a standalone read for "
                        "full content]\n";
                }

                ++ran;
                if (r.is_error) {
                    ++err_count;
                    if (!continue_on_error) {
                        const size_t skipped = N - step - 1;
                        if (skipped > 0) {
                            out << "[stopped at op " << (step + 1)
                                << "; " << skipped
                                << " op" << (skipped == 1 ? "" : "s")
                                << " skipped — pass "
                                "continue_on_error=true to run them "
                                "anyway]\n";
                        }
                        break;
                    }
                } else {
                    ++ok_count;
                }
            }
            // Footer: one-line summary so the model can see at a
            // glance how many ops succeeded vs failed vs were
            // skipped after a stop-on-first-error abort.
            const size_t skipped_total = N - ran;
            out << "summary: " << ok_count << " ok, "
                << err_count << " err";
            if (skipped_total > 0)
                out << ", " << skipped_total << " skipped";
            if (continue_on_error && err_count > 0)
                out << " (continue_on_error)";
            out << "\n";

            // The whole batch is reported as ToolResult::ok — the
            // model parses per-op success/failure from the report
            // body. Returning ::error here would lose the partial
            // success information when stop-on-first-error fires.
            return ToolResult::ok(out.str());
        })
        .build();
}

// ----------------------------------------------------------------------------
// fs_split — one focused tool per fs action.
// ----------------------------------------------------------------------------
// Smaller / quantised tool-callers consistently work better with
// one-purpose tools than with a `fs(action="x")` dispatcher: every name
// is its own semantic anchor, the params schema is flat (no
// discriminated union), and there is no "unknown action" failure mode.
// The handlers are reused verbatim, so behaviour is identical to the
// unified `fs` — the only difference is surface.
//
// Same Sandbox instance shared across the bundle so the tools observe
// a consistent root (and the operator pays the inode probe once).
std::vector<Tool> fs_split(std::string root) {
    auto sb = std::make_shared<Sandbox>(root);
    std::vector<Tool> out;
    out.reserve(10);

    out.push_back(Tool::builder("fs_read")
        .describe(
            "Read a UTF-8 text file. RELATIVE path under the sandbox "
            "root. Output prefixes every line with `<n>| ` and reports "
            "the total line count — use before fs_edit to get accurate "
            "line references.\n"
            "  Line mode (recommended): pass start_line (1-based). "
            "limit = lines (default 200, max 2000).\n"
            "  Byte mode: offset + limit (bytes, default 65536). "
            "line_numbers on by default.")
        .param("path",         "string",
               "Relative path. `.` for root.", true)
        .param("start_line",   "integer",
               "1-based first line. Switches to line mode — limit "
               "becomes line count, output always numbered.", false)
        .param("offset",       "integer",
               "Byte mode only. Byte offset, default 0.", false)
        .param("limit",        "integer",
               "Line mode: line count (default 200, max 2000). "
               "Byte mode: max bytes (default 65536, max 1048576).",
               false)
        .param("line_numbers", "boolean",
               "Byte mode only. Prefix each line `<n>| `. "
               "Default true (always on in line mode).", false)
        .handle(make_fs_read_handler(sb))
        .build());

    out.push_back(Tool::builder("fs_write")
        .describe(
            "Write UTF-8 text to a file (OVERWRITES existing "
            "content). Creates parent dirs.")
        .param("path",    "string",
               "Relative path. `.` for root.", true)
        .param("content", "string",
               "Full file content. Use `\\n` for newlines.", true)
        .param("append",  "boolean",
               "Append instead of overwrite. Default false.", false)
        .handle(make_fs_write_handler(sb))
        .build());

    out.push_back(Tool::builder("fs_append")
        .describe(
            "Append UTF-8 text to the END of a file (creates if "
            "missing).")
        .param("path",    "string",
               "Relative path. `.` for root.", true)
        .param("content", "string",
               "Text to append. Use `\\n` for newlines.", true)
        .handle(make_fs_append_handler(sb))
        .build());

    out.push_back(Tool::builder("fs_edit")
        .describe(
            "Replace lines [start_line..end_line] (1-based, "
            "inclusive) in an existing file. Atomic.\n"
            "  content=\"\" deletes the range; end_line=start_line-1 "
            "inserts before start_line; start_line=line_count+1 "
            "appends at EOF.\n"
            "  Plan with fs_read(line_numbers=true).")
        .param("path",       "string",
               "Relative path under the sandbox root.", true)
        .param("start_line", "integer",
               "1-based, inclusive. line_count+1 appends at EOF.",
               true)
        .param("end_line",   "integer",
               "1-based, inclusive. start_line-1 inserts before "
               "start_line.", true)
        .param("content",    "string",
               "Replacement text. \"\" deletes.", true)
        .handle(make_fs_edit_handler(sb))
        .build());

    out.push_back(Tool::builder("fs_list")
        .describe(
            "Non-recursive directory listing. One entry per line "
            "(`d`/`f` prefix + size).")
        .param("path", "string",
               "Relative path; default sandbox root.", false)
        .handle(make_fs_list_handler(sb))
        .build());

    out.push_back(Tool::builder("fs_glob")
        .describe(
            "Recursive wildcard file search. `*` single segment, "
            "`**` crosses dirs, `?` one char, `[abc]` a set.")
        .param("pattern", "string",
               "Wildcard pattern (e.g. `*.cpp`, `**/test_*.py`).",
               true)
        .param("path",    "string",
               "Start directory; default sandbox root.", false)
        .handle(make_fs_glob_handler(sb))
        .build());

    out.push_back(Tool::builder("fs_grep")
        .describe(
            "Recursive regex content search. Output is "
            "`<path>:<lineno>:<line>`.")
        .param("pattern",          "string",
               "ECMAScript regex, matched per line. Anchor with "
               "`^`/`$` for full-line matches.", true)
        .param("path",             "string",
               "Start file or directory; default sandbox root.",
               false)
        .param("file_glob",        "string",
               "Restrict filenames by basename pattern (e.g. "
               "`*.cpp`).", false)
        .param("max_matches",      "integer",
               "Stop after N matches. Default 100.", false)
        .param("case_insensitive", "boolean",
               "Default false.", false)
        .handle(make_fs_grep_handler(sb))
        .build());

    out.push_back(Tool::builder("fs_check_path")
        .describe(
            "Pre-flight: existence, type, size, r/w/x rights. Run "
            "before reading/writing any unfamiliar path.")
        .param("path",  "string",
               "Relative path to probe.", true)
        .param("touch", "boolean",
               "Create empty file if missing (probes write access). "
               "Default false.", false)
        .handle(make_fs_check_path_handler(sb))
        .build());

    out.push_back(Tool::builder("fs_cwd")
        .describe(
            "Current working directory at call time (getcwd). For "
            "day-to-day work use fs_sandbox. No parameters.")
        .handle(make_fs_cwd_handler())
        .build());

    out.push_back(Tool::builder("fs_sandbox")
        .describe(
            "Absolute sandbox root, pinned at registration. The "
            "anchor every fs_* / bash relative path resolves "
            "against. Run once at the start of any filesystem task. "
            "No parameters.")
        .handle(make_fs_sandbox_handler(root))
        .build());

    return out;
}

// ============================================================================
// run_capped_subprocess — shared spawn-and-cap machinery for shell-class tools
// ----------------------------------------------------------------------------
// Used by `bash` (`/bin/sh -c <cmd>`) and `python3` (`python3 -I -S -E -c
// <code>`). Same fork / fd-close / chdir / exec / drain / timeout / output-cap
// discipline; only the exec call in the child differs.
//
//   - Output is stdout+stderr merged through a single pipe.
//   - 32 KB cap on the model-facing capture buffer.
//   - 128 KB cap on the operator-facing live mirror (when show_output).
//   - SIGTERM at deadline, SIGKILL +2s grace (negative pid → whole pgrp,
//     so grandchildren the spawned process forked also receive the signal).
//   - O_NOFOLLOW everywhere we open files in the child, fds 3..maxfd
//     forcibly closed before exec so the parent's HTTP listener / log /
//     mmap'd model stays out of the subprocess.
// ============================================================================
namespace {

enum class CappedExecKind { Bash, Python3 };

// `tool_label` is the prefix that goes into the operator-facing banner
// (`[bash] $ ...`, `[python3] $ ...`) and into the diagnostic strings
// the child writes on chdir/exec failure. Must be NUL-terminated and
// short — async-signal-safe writes copy the literal verbatim.
//
// `banner_display` is what gets printed in the opening banner so the
// operator sees what's about to run. Distinct from `body_arg` (what the
// child actually executes) for python3, where `body_arg` carries the
// sandbox-preamble + user code and the banner should only show the
// user-authored part to keep the transcript readable. Sanitized through
// `sanitize_for_operator_tty` before write so model-supplied ANSI/OSC
// escapes embedded in the command/code can't repaint the terminal.
ToolResult run_capped_subprocess(
        const std::shared_ptr<Sandbox> & sb,
        CappedExecKind            kind,
        const std::string &       body_arg,        // shell command (Bash) or Python source (Python3)
        const std::string &       banner_display,  // what to show the operator on the opening banner
        long long                 timeout_sec,
        bool                      show_output,
        const char *              tool_label) {
    if (show_output) {
        std::string safe_banner = sanitize_for_operator_tty(
            banner_display.data(), banner_display.size());
        std::fprintf(stderr, "\n[%s] $ %s\n", tool_label, safe_banner.c_str());
        std::fflush(stderr);
    }

    int pipefd[2];
    if (::pipe(pipefd) < 0)
        return ToolResult::error(std::string("pipe() failed: ") + std::strerror(errno));

    // Cap on the inherited-fd close loop in the child. Mirrors
    // external_tools.cpp's kMaxFdScan: RLIMIT_NOFILE = unlimited
    // wraps to -1 and silently disables a naive loop, leaking
    // every parent fd into the child. 65 536 is more than enough
    // for any well-behaved process.
    constexpr long kMaxFdScan = 65536;

    const std::string cwd = sb->root.string();
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]); ::close(pipefd[1]);
        return ToolResult::error(std::string("fork() failed: ") + std::strerror(errno));
    }
    if (pid == 0) {
        // ---- CHILD ----
        // Async-signal-safe operations only until exec.
        ::setpgid(0, 0);   // own process group → kill(-pgid) reaches grandchildren
#if defined(__linux__)
        // Tie our lifetime to the parent: if the agent crashes (segfault,
        // OOM-kill, kill -9) before we exec or while we run, the kernel
        // sends us SIGKILL. Without this an orphaned subprocess would
        // survive (reparented to PID 1) and keep running until its own
        // timeout.
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
        ::close(pipefd[0]);
        ::dup2(pipefd[1], 1);
        ::dup2(pipefd[1], 2);
        ::close(pipefd[1]);

        // stdin → /dev/null. The model has no way to feed bytes into the
        // subprocess, but anything that reads stdin (`cat`, `input()`,
        // etc.) would otherwise inherit our controlling-terminal stdin
        // and block.
        int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devnull >= 0) {
            ::dup2(devnull, 0);
            ::close(devnull);
        } else {
            ::close(0);
        }

        // Close every other inherited fd. The parent has the HTTP
        // listener, log file, llama.cpp's mmap'd model, etc. None of
        // those should be visible to the subprocess.
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

        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
            // Hand-written async-signal-safe diagnostic; can't use
            // fprintf because its mutex state is shared with the parent.
            const char prefix[] = ": chdir failed\n";
            ssize_t w1 = ::write(1, tool_label, std::strlen(tool_label));
            ssize_t w2 = ::write(1, prefix, sizeof(prefix) - 1);
            (void) w1; (void) w2;
            ::_exit(126);
        }

        if (kind == CappedExecKind::Bash) {
            ::execl("/bin/sh", "sh", "-c", body_arg.c_str(), (char *) nullptr);
        } else {
            // python3 -I -S -E -c <code>:
            //   -I  isolated mode (implies -E -s, plus drops sys.path[0])
            //   -S  don't run site.py at startup (no .pth files / site-packages auto-load)
            //   -E  ignore PYTHON* env vars
            //   -c  run the code passed as argv[5]
            // execvp does PATH lookup so we don't have to hardcode an
            // absolute path; the operator's environment picks the right
            // python3 (Homebrew, system, conda — whichever is on PATH).
            const char * argv[] = {
                "python3", "-I", "-S", "-E", "-c",
                body_arg.c_str(), nullptr
            };
            ::execvp("python3", const_cast<char **>(argv));
        }

        const char prefix[] = ": exec failed\n";
        ssize_t w1 = ::write(1, tool_label, std::strlen(tool_label));
        ssize_t w2 = ::write(1, prefix, sizeof(prefix) - 1);
        (void) w1; (void) w2;
        ::_exit(127);
    }
    // ---- PARENT ----
    // Mirror the child's setpgid so kill(-pid) reaches the right group
    // regardless of scheduling order.
    ::setpgid(pid, pid);
    ::close(pipefd[1]);
    ::fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    constexpr size_t kCap = 32 * 1024;
    // Mirror cap is intentionally larger than the model-facing buffer
    // (4×) so the operator still sees most of a long build's output,
    // but bounded so a hostile/runaway command can't paint the
    // operator's terminal indefinitely.
    constexpr size_t kMirrorCap = 128 * 1024;
    std::string out;
    out.reserve(4096);
    size_t mirror_written = 0;
    bool   mirror_truncated_warned = false;

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(timeout_sec);
    bool sent_term = false;
    bool killed_for_timeout = false;
    int  status = 0;
    bool reaped = false;

    auto drain_pipe = [&]() {
        char buf[4096];
        ssize_t n;
        while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) {
            if (out.size() < kCap) {
                size_t take = std::min((size_t) n, kCap - out.size());
                out.append(buf, take);
            }
            // Live mirror: every byte the child writes (stdout and
            // stderr were dup2'd onto the same pipe in the child) goes
            // to the parent's stderr so the operator can watch a
            // long-running build / test suite / Python computation
            // scroll by. The model still receives the full captured
            // buffer; this is a parallel "human-facing" channel.
            //
            // Two safety layers before the bytes hit the operator's
            // terminal:
            //   1. byte-budget: cap at kMirrorCap so a runaway / hostile
            //      command can't flood the terminal indefinitely.
            //   2. control-byte strip: strip ESC and other C0 control
            //      bytes so a malicious child cannot inject ANSI/OSC
            //      escape sequences (window-title hijack, screen wipe,
            //      iTerm2 payloads, key-rebinding, etc.) into the
            //      operator's terminal. CR / LF / TAB are preserved.
            if (show_output && mirror_written < kMirrorCap) {
                std::string safe = sanitize_for_operator_tty(buf, (size_t) n);
                size_t room = kMirrorCap - mirror_written;
                size_t take = std::min(safe.size(), room);
                if (take > 0) {
                    ::fwrite(safe.data(), 1, take, stderr);
                    mirror_written += take;
                }
                if (mirror_written >= kMirrorCap && !mirror_truncated_warned) {
                    char trunc[128];
                    int tn = std::snprintf(trunc, sizeof(trunc),
                        "\n[%s mirror truncated at 128 KB; "
                        "model still receives the captured output]\n",
                        tool_label);
                    if (tn > 0) ::fwrite(trunc, 1, (size_t) tn, stderr);
                    mirror_truncated_warned = true;
                }
                std::fflush(stderr);
            }
        }
    };

    for (;;) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            if (!sent_term) {
                // Negative pid in ::kill targets the whole process group
                // (we set setpgid above) so any grandchildren the
                // process spawned receive the signal too. A bare
                // kill(pid, …) on the leader leaves grandchildren
                // behind.
                ::kill(-pid, SIGTERM);
                sent_term          = true;
                killed_for_timeout = true;
                deadline           = now + std::chrono::seconds(2); // grace
            } else {
                ::kill(-pid, SIGKILL);
                break;
            }
        }
        int wait_ms = (int) std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - now).count();
        if (wait_ms < 0)   wait_ms = 0;
        if (wait_ms > 200) wait_ms = 200;

        struct pollfd pfd { pipefd[0], POLLIN, 0 };
        int rc = ::poll(&pfd, 1, wait_ms);
        if (rc < 0 && errno == EINTR) continue;
        if (rc > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            drain_pipe();
        }

        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            drain_pipe();
            reaped = true;
            break;
        }
    }

    if (!reaped) {
        drain_pipe();
        ::waitpid(pid, &status, 0);
    }
    ::close(pipefd[0]);

    std::ostringstream oss;
    if (killed_for_timeout) {
        oss << "exit=-1  [killed: timeout after " << timeout_sec << "s]\n";
    } else if (WIFEXITED(status)) {
        oss << "exit=" << WEXITSTATUS(status) << "\n";
    } else if (WIFSIGNALED(status)) {
        oss << "exit=signal:" << WTERMSIG(status) << "\n";
    } else {
        oss << "exit=?\n";
    }
    std::string body = std::move(out);
    if (body.size() >= kCap) body += "\n[truncated at 32 KB]\n";
    // Closing banner mirrors the opening one: the operator sees the
    // exit status and a trailing newline so the next agent line
    // doesn't run into the subprocess output. Newline first in case
    // the child's last byte wasn't a '\n'.
    if (show_output) {
        std::fprintf(stderr, "\n[%s] %s", tool_label, oss.str().c_str());
        std::fflush(stderr);
    }
    return ToolResult::ok(oss.str() + body);
}

}  // namespace

Tool bash(std::string root, bool show_output) {
    auto sb = std::make_shared<Sandbox>(std::move(root));
    return Tool::builder("bash")
        .short_describe(
            "Run a shell command (`/bin/sh -c`). Allowed to write files "
            "(redirects, sed -i, mkdir). Use for pipes/build/git/sed/awk.")
        .describe(
            "Run a shell command via `/bin/sh -c`. stdout and stderr "
            "are merged. cwd is pinned to the sandbox root; use "
            "RELATIVE paths.\n"
            "\n"
            "Use bash ONLY for shell features fs/python3 can't do:\n"
            "  - pipelines (`grep | xargs`, `find -exec`)\n"
            "  - build runners (make, cmake, cargo, npm)\n"
            "  - git, package managers, sed/awk in-place edits\n"
            "\n"
            "For file work use fs (no `cat > file`, `cat <<EOF`, "
            "`echo >`, `mkdir`). For compute use python3.\n"
            "\n"
            "NOT a hardened sandbox — runs with caller's full uid/gid. "
            "Output capped at 32 KB; SIGTERM/SIGKILL deadline "
            "(default 30s, max 300s)."
        )
        .param("command", "string",
               "Shell command line, written as you'd type it in a "
               "terminal. RELATIVE paths.", true)
        .param("timeout_sec", "integer",
               "Max seconds before SIGTERM/SIGKILL. Default 30, max "
               "300.", false)
        .handle([sb, show_output](const ToolCall & c) {
            std::string cmd;
            long long timeout_sec = 30;
            if (!args::get_string(c.arguments_json, "command", cmd) || cmd.empty())
                return ToolResult::error("missing arg: command");
            args::get_int(c.arguments_json, "timeout_sec", timeout_sec);
            if (timeout_sec < 1)   timeout_sec = 1;
            if (timeout_sec > 300) timeout_sec = 300;
            return run_capped_subprocess(
                sb, CappedExecKind::Bash, cmd, cmd,
                timeout_sec, show_output, "bash");
        })
        .build();
}

// ============================================================================
// python3 — run a Python 3 snippet via `python3 -I -S -E -c <code>`
// ----------------------------------------------------------------------------
// Same hardening as `bash` (cwd pinned to sandbox, fds 3+ closed before
// exec, SIGTERM/SIGKILL deadline, output cap, optional operator mirror).
// The interpreter starts in *isolated mode* — no PYTHON* env vars, no
// site-packages auto-load, no cwd on sys.path — so the snippet runs
// against the bare standard library every time, regardless of the host
// user's Python configuration.
//
// SANDBOX-ROOTED FILESYSTEM (defense-in-depth)
// --------------------------------------------
// Every snippet is auto-prefixed with a short Python preamble that
// monkey-patches `builtins.open`, `io.open`, and `os.open` to reject
// any path resolving outside the sandbox root (the cwd Python is
// chdir'd into before exec). Attempts to read `/etc/passwd`, write
// `~/.ssh/foo`, or open `../escape` raise PermissionError.
//
// This is NOT a hardened sandbox — the model can still escape via
// `import ctypes; ctypes.CDLL("libc.so.6").open(...)`, `os.system`,
// `subprocess.run`, raw socket reads of UNIX-domain server sockets,
// etc. The protection is against ACCIDENT, not adversarial intent:
// the description tells the model "never use python3 for disk; use
// fs(action=...)", and the preamble enforces that contract for the
// common open() / pathlib paths so a stray `open("/etc/hosts")` in
// generated code fails loudly instead of silently leaking host data.
// ============================================================================

// Python preamble auto-prepended to every snippet.  Enforces two
// invariants:
//
//   1. SANDBOX ROOT — open() / io.open() / os.open() reject any path
//      that resolves outside the sandbox root (the cwd Python is chdir'd
//      into before exec).
//
//   2. READ-ONLY — even inside the sandbox, any write-mode open is
//      rejected.  builtins.open / io.open: any mode containing `w`,
//      `a`, `x`, or `+`.  os.open: any flags with O_WRONLY, O_RDWR,
//      O_CREAT, O_TRUNC, or O_APPEND set.  The python3 tool is for
//      COMPUTE and ALGORITHM TESTING only; disk writes/edits flow
//      through fs(action=...) or bash.
//
// Identifiers are `_e_*`-prefixed to avoid colliding with reasonable
// user code.  Module-scope helpers (`_e_root`, `_e_open_orig`,
// `_e_os_open_orig`, `_e_make_wrappers`) are deleted after wiring so
// a snippet cannot reach them by name to bypass the check.
//
// This is still NOT a hardened sandbox: a determined snippet can bypass
// via `import ctypes; ctypes.CDLL("libc.so.6").open(...)`, `os.system`,
// `subprocess`, `_io.FileIO("/etc/passwd", "wb")`, or by re-importing
// modules and mutating their internals.  The preamble defends against
// ACCIDENT (a stray `open("out.txt", "w")` in generated code) and
// against the trivial discoverable-by-name bypass — not against
// adversarial intent.
static const char * const kPythonSandboxPreamble =
    "import os as _e_os, builtins as _e_b, io as _e_io\n"
    "def _e_make_wrappers(_e_root, _e_open_orig, _e_os_open_orig):\n"
    "    _e_wmask = (_e_os.O_WRONLY | _e_os.O_RDWR | _e_os.O_CREAT |\n"
    "                _e_os.O_TRUNC  | _e_os.O_APPEND)\n"
    "    def _e_chk_path(p):\n"
    "        if isinstance(p, int): return\n"
    "        try: s = _e_os.fspath(p)\n"
    "        except TypeError: return\n"
    "        if _e_os.path.isabs(s):\n"
    "            a = _e_os.path.realpath(s)\n"
    "        else:\n"
    "            a = _e_os.path.realpath(_e_os.path.join(_e_root, s))\n"
    "        if a != _e_root and not a.startswith(_e_root + _e_os.sep):\n"
    "            raise PermissionError(\n"
    "                'easyai sandbox: disk access to ' + repr(s) +\n"
    "                ' denied (resolves to ' + repr(a) + ', outside sandbox '\n"
    "                'root ' + repr(_e_root) + '). The python3 tool is for '\n"
    "                'compute / network / data only — use fs(action=...) for '\n"
    "                'disk work.')\n"
    "    def _e_chk_mode(mode):\n"
    "        if not isinstance(mode, str): return\n"
    "        for ch in mode:\n"
    "            if ch in 'waWAxX+':\n"
    "                raise PermissionError(\n"
    "                    'easyai sandbox: write-mode open(' + repr(mode) +\n"
    "                    \") denied. python3 is READ-ONLY on disk — use \"\n"
    "                    \"fs(action='write'|'edit'|'append') or bash for \"\n"
    "                    'writes.')\n"
    "    def _e_chk_flags(flags):\n"
    "        try: f = int(flags)\n"
    "        except (TypeError, ValueError): return\n"
    "        if f & _e_wmask:\n"
    "            raise PermissionError(\n"
    "                'easyai sandbox: os.open with write flags denied. '\n"
    "                \"python3 is READ-ONLY on disk — use \"\n"
    "                \"fs(action='write'|'edit'|'append') or bash for \"\n"
    "                'writes.')\n"
    "    def _e_open(f, mode='r', *a, **k):\n"
    "        _e_chk_path(f); _e_chk_mode(mode)\n"
    "        return _e_open_orig(f, mode, *a, **k)\n"
    "    def _e_os_open(p, flags, *a, **k):\n"
    "        _e_chk_path(p); _e_chk_flags(flags)\n"
    "        return _e_os_open_orig(p, flags, *a, **k)\n"
    "    return _e_open, _e_os_open\n"
    "_e_o, _e_oo = _e_make_wrappers(\n"
    "    _e_os.path.realpath(_e_os.getcwd()), _e_b.open, _e_os.open)\n"
    "_e_b.open = _e_o\n"
    "_e_io.open = _e_o\n"
    "_e_os.open = _e_oo\n"
    "del _e_make_wrappers, _e_o, _e_oo\n"
    "# --- end preamble; user code follows ---\n";

Tool python3(std::string root, bool show_output) {
    auto sb = std::make_shared<Sandbox>(std::move(root));
    return Tool::builder("python3")
        .short_describe(
            "COMPUTE-ONLY Python 3 sandbox. CANNOT write/create/delete "
            "files — use fs or bash for that. Stdlib only.")
        .describe(
            "Run a Python 3 snippet via `python3 -I -S -E -c <code>`. "
            "Captured stdout+stderr is the tool output.\n"
            "\n"
            "COMPUTE & ALGORITHM TESTING ONLY. NEVER use this tool to "
            "create, write, modify, append, or delete files — every "
            "write-mode open() is rejected even inside the sandbox. "
            "For any disk write/edit use `fs(action=\"write\"|\"edit\"|"
            "\"append\")`; for shell-level edits use `bash`. Read-only "
            "open() works inside the sandbox so you can still load a "
            "CSV/JSON, compute, and print the result.\n"
            "\n"
            "STDLIB ONLY — no third-party packages, no PYTHON* env, "
            "no cwd on sys.path. Available: json, re, statistics, "
            "datetime, decimal, urllib.request, socket, hashlib, csv, "
            "math, etc. `import numpy` / `import requests` will fail "
            "with ModuleNotFoundError.\n"
            "\n"
            "Use for: arithmetic, JSON/CSV wrangling, regex, date "
            "math, hashing, HTTP fetches (urllib.request), socket "
            "probes, sanity-checking an algorithm before you put it "
            "into source via fs/bash.\n"
            "\n"
            "ALWAYS print() what you want returned. Snippets that "
            "don't print come back with just `exit=0`.\n"
            "\n"
            "Examples:\n"
            "  {code: \"from decimal import Decimal as D; "
            "print(D('0.1')+D('0.2'))\"}\n"
            "  {code: \"import json, urllib.request; "
            "r=urllib.request.urlopen('https://api.github.com/repos/"
            "torvalds/linux'); print(json.load(r)['stargazers_count'])\"}\n"
            "\n"
            "NOT a hardened sandbox — has caller's full uid/gid; "
            "`import os`/`socket`/`subprocess` still work. Output "
            "capped at 32 KB; SIGTERM/SIGKILL deadline (default 30s, "
            "max 300s)."
        )
        .param("code", "string",
               "Python 3 source, passed verbatim to `-c` (no shell "
               "layer). Use `\\n` in the JSON string for multi-line "
               "code.", true)
        .param("timeout_sec", "integer",
               "Max seconds before SIGTERM/SIGKILL. Default 30, max "
               "300.", false)
        .handle([sb, show_output](const ToolCall & c) {
            std::string code;
            long long timeout_sec = 30;
            if (!args::get_string(c.arguments_json, "code", code) || code.empty())
                return ToolResult::error("missing arg: code");
            args::get_int(c.arguments_json, "timeout_sec", timeout_sec);
            if (timeout_sec < 1)   timeout_sec = 1;
            if (timeout_sec > 300) timeout_sec = 300;

            // Prepend the sandbox preamble. The model's `code` is
            // appended verbatim after the preamble's `# --- end
            // preamble ---` marker; line-number errors in user code
            // will be offset by the preamble's line count, but the
            // model rarely reads tracebacks line-by-line and the
            // PermissionError messages are descriptive enough to be
            // actionable on their own.
            std::string wrapped;
            wrapped.reserve(std::strlen(kPythonSandboxPreamble) + code.size() + 1);
            wrapped.append(kPythonSandboxPreamble);
            wrapped.append(code);

            // Banner shows the operator the user-authored `code` only —
            // the sandbox preamble is implementation detail and would
            // just dump 25 lines of `_e_*` plumbing on every call.
            ToolResult r = run_capped_subprocess(
                sb, CappedExecKind::Python3, wrapped, code,
                timeout_sec, show_output, "python3");

            // Spawn-side errors (pipe / fork failure) leave the
            // interpreter never having run — return them unaltered
            // so the operator's message stays the actual cause and
            // doesn't get dressed up with a deceptive "executed"
            // notice.
            if (r.is_error) return r;

            // Wrap the result so any chat UI rendering markdown
            // shows the executed snippet as a syntax-highlighted
            // Python block, with an explicit "[python3 executed]"
            // notification before the captured exit / output. The
            // model already sees its own `code` argument in the
            // tool_call but the rendered transcript a human reads
            // typically only shows tool RESULTS, so dropping the
            // code into the result is what makes it visible to the
            // operator without expanding raw tool-call JSON.
            //
            // The preamble (kPythonSandboxPreamble) is intentionally
            // NOT included in the rendered block — it's an
            // implementation detail and would just clutter the
            // transcript with the same 25 lines on every call.
            std::string view;
            view.reserve(code.size() + r.content.size() + 64);
            view.append("```python\n");
            view.append(code);
            if (code.empty() || code.back() != '\n') view.push_back('\n');
            view.append("```\n");
            view.append("[python3 executed]\n");
            view.append(r.content);
            return ToolResult::ok(std::move(view));
        })
        .build();
}

// tool_lookup — read-only introspection over the live tool registry.
// See easyai/builtin_tools.hpp for the full contract.
//
// Implementation notes:
//   - Match is case-insensitive substring on tool NAME only (not
//     description).  Description-search would surface generic tools
//     for keyword queries and confuse the model — the contract is
//     "look up a name you think exists".
//   - Output is plain numbered text, not JSON, because (a) the model
//     reads it as prose and (b) numbers + colons render fine inside
//     the chat-template tool-result wrapper.  No need for the model
//     to parse anything structured.
//   - The getter is invoked at every call so the snapshot always
//     reflects whatever's currently registered (useful in webui
//     deployments where remote MCP tools may have arrived after
//     startup).
Tool tool_lookup(ToolListGetter get_tools) {
    if (!get_tools) {
        // Fail-closed factory: a missing getter is a wiring bug, not a
        // runtime condition.  Return a tool whose handler always errors
        // so the model gets a clear signal instead of silent emptiness.
        return Tool::builder("tool_lookup")
            .describe("(disabled — host did not provide a tool registry getter)")
            .handle([](const ToolCall &) {
                return ToolResult::error(
                    "tool_lookup is registered but the host didn't wire up "
                    "a tool-list getter. This is a deployment bug; report it.");
            })
            .build();
    }
    return Tool::builder("tool_lookup")
        .short_describe(
            "List or inspect tools registered this session. No args → "
            "index; name=\"<substring>\" → full manual for matches.")
        .describe(
            "List or inspect every tool wired up in THIS session. The "
            "single source of truth for what you can call AND what each "
            "tool actually does.\n"
            "\n"
            "Two modes:\n"
            "  - No arguments → INDEX: numbered list of `name: short "
            "description` (one line per tool). Cheap, scannable.\n"
            "  - name=\"<substring>\" → MANUAL: full multi-line "
            "description for every tool whose name matches the "
            "substring (case-insensitive, partial). This is the "
            "expanded help text — schema rules, examples, edge cases. "
            "Use this when the short description in the tools block "
            "isn't enough to call confidently.\n"
            "\n"
            "If a name is NOT in this list, IT DOES NOT EXIST here — "
            "no fallback, no implicit import. Calling an unlisted "
            "name wastes a hop. When you reach for a generic name "
            "(`write`, `ls`, `curl`, `python`, `sed`, etc.) and "
            "haven't seen it registered, call this first instead of "
            "guessing.\n"
            "\n"
            "Read-only, never spawns a process. Cheap — call freely "
            "when uncertain. A no-match result is authoritative; "
            "don't retry variations."
        )
        .param("name", "string",
               "Substring filter over tool names (case-insensitive). "
               "Omit or pass \"\" for the index view.",
               false)
        .handle([get_tools](const ToolCall & c) -> ToolResult {
            std::string filter = args::get_string_or(c.arguments_json, "name", "");
            // Lower-case both sides for case-insensitive match.
            auto lower = [](std::string s) {
                for (auto & ch : s) {
                    ch = static_cast<char>(std::tolower(
                        static_cast<unsigned char>(ch)));
                }
                return s;
            };
            const std::string flt = lower(filter);

            ToolCatalog catalog;
            try {
                catalog = get_tools();
            } catch (const std::exception & e) {
                return ToolResult::error(
                    std::string("tool_lookup: registry getter threw: ") + e.what());
            } catch (...) {
                return ToolResult::error(
                    "tool_lookup: registry getter threw a non-std exception");
            }

            if (catalog.empty()) {
                return ToolResult::ok("(no tools registered in this session)");
            }

            // Pick the description style based on the call shape:
            //   * No filter → INDEX view (short trigger, one line each).
            //   * With filter → MANUAL view (full multi-line description
            //     for every match). Falls back to short_description when
            //     a tool didn't author a full one.
            const bool manual_view = !flt.empty();

            std::ostringstream out;
            int n = 0;
            for (const auto & e : catalog) {
                if (manual_view && lower(e.name).find(flt) == std::string::npos) {
                    continue;
                }
                ++n;
                if (manual_view) {
                    const std::string & body =
                        !e.full_description.empty() ? e.full_description
                                                    : e.short_description;
                    out << "## " << e.name << "\n";
                    if (body.empty()) out << "(no description)\n";
                    else {
                        out << body;
                        if (body.back() != '\n') out << '\n';
                    }
                    out << '\n';
                } else {
                    std::string summary = e.short_description;
                    if (summary.empty()) {
                        // Pre-Shape-C tool — synthesise a one-liner
                        // from the full description so the index
                        // still renders meaningfully.
                        summary = e.full_description;
                        std::size_t nl = summary.find('\n');
                        if (nl != std::string::npos) summary.erase(nl);
                        summary = trim(summary);
                    }
                    if (summary.empty()) summary = "(no description)";
                    out << n << ". " << e.name << ": " << summary << '\n';
                }
            }

            if (n == 0) {
                return ToolResult::ok(
                    "(no tools match: \"" + filter + "\")\n"
                    "(call tool_lookup with no `name` to see the index)");
            }
            if (manual_view) {
                out << "(manual view for name=\"" << filter << "\"; "
                    << "call tool_lookup with no `name` for the full index)";
            }
            return ToolResult::ok(out.str());
        })
        .build();
}

}  // namespace easyai::tools
