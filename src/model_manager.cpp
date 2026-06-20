// src/model_manager.cpp — the /models dashboard, in the terminal.
//
// Drives the server's /models/api/* endpoints over an easyai::Client to
// provide, in a full-screen TUI: a comprehensive live Status view, a local
// GGUF manager (run / hot-swap / delete / symlink), a HuggingFace
// recommendation browser with hardware-fit scoring, and a GGUF downloader
// with a live progress bar. See include/easyai/model_manager.hpp.
//
// Self-contained: its own ANSI palette / width helpers / raw-mode terminal /
// byte→key parser (mirroring the proven sequences in src/tui.cpp) so it
// doesn't entangle with the chat TUI's internals.

#include "easyai/model_manager.hpp"
#include "easyai/client.hpp"
#include "easyai/ui.hpp"

#include <nlohmann/json.hpp>

#include <termios.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

using nlohmann::json;

namespace easyai::manager {
namespace {

// ===========================================================================
// Colour
// ===========================================================================
struct RGB { unsigned char r = 0, g = 0, b = 0; };
RGB rgb(unsigned x) { return { (unsigned char)(x >> 16), (unsigned char)(x >> 8), (unsigned char)x }; }

struct Theme {
    RGB primary, accent, secondary, info;
    RGB text, muted;
    RGB success, warning, error;
    RGB border, sel_bg, sel_fg;
};

Theme theme_dark() {
    Theme t;
    t.primary   = rgb(0xfab283); t.accent  = rgb(0x9d7cd8);
    t.secondary = rgb(0x5c9cf5); t.info    = rgb(0x56b6c2);
    t.text      = rgb(0xeeeeee); t.muted   = rgb(0x808080);
    t.success   = rgb(0x7fd88f); t.warning = rgb(0xf5a742); t.error = rgb(0xe06c75);
    t.border    = rgb(0x484848); t.sel_bg  = rgb(0x2a2a40); t.sel_fg = rgb(0xffffff);
    return t;
}
Theme theme_light() {
    Theme t;
    t.primary   = rgb(0x3b7dd8); t.accent  = rgb(0xd68c27);
    t.secondary = rgb(0x7b5bb6); t.info    = rgb(0x318795);
    t.text      = rgb(0x1a1a1a); t.muted   = rgb(0x8a8a8a);
    t.success   = rgb(0x3d9a57); t.warning = rgb(0xb0851f); t.error = rgb(0xd1383d);
    t.border    = rgb(0xb8b8b8); t.sel_bg  = rgb(0xdfe6f5); t.sel_fg = rgb(0x000000);
    return t;
}
Theme pick_theme(const std::string & n) {
    return (n == "opencode-light" || n == "light") ? theme_light() : theme_dark();
}

bool g_color     = true;   // emit ANSI at all
bool g_truecolor = true;   // 24-bit vs 256-colour cube

int cube_index(RGB c) {
    auto q = [](unsigned char v) { return v < 48 ? 0 : v < 115 ? 1 : (v - 35) / 40; };
    if (std::abs(c.r - c.g) < 12 && std::abs(c.g - c.b) < 12) {
        int gray = (c.r + c.g + c.b) / 3;
        if (gray < 4) return 16;
        if (gray > 246) return 231;
        return 232 + std::min(23, (gray - 4) / 10);
    }
    return 16 + 36 * q(c.r) + 6 * q(c.g) + q(c.b);
}
std::string fg(RGB c) {
    if (!g_color) return "";
    char b[32];
    if (g_truecolor) std::snprintf(b, sizeof(b), "\033[38;2;%u;%u;%um", c.r, c.g, c.b);
    else             std::snprintf(b, sizeof(b), "\033[38;5;%dm", cube_index(c));
    return b;
}
std::string bg(RGB c) {
    if (!g_color) return "";
    char b[32];
    if (g_truecolor) std::snprintf(b, sizeof(b), "\033[48;2;%u;%u;%um", c.r, c.g, c.b);
    else             std::snprintf(b, sizeof(b), "\033[48;5;%dm", cube_index(c));
    return b;
}
std::string RESET() { return g_color ? "\033[0m" : ""; }
std::string BOLD()  { return g_color ? "\033[1m" : ""; }
std::string DIM()   { return g_color ? "\033[2m" : ""; }

void detect_term() {
    const char * ct = ::getenv("COLORTERM");
    g_truecolor = ct && (std::strstr(ct, "truecolor") || std::strstr(ct, "24bit"));
    if (::getenv("NO_COLOR")) g_color = false;
}

// ===========================================================================
// UTF-8 display width (codepoint-counted — good enough for model metadata)
// ===========================================================================
size_t u8len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xe) return 3;
    if ((c >> 3) == 0x1e) return 4;
    return 1;
}
int disp_width(const std::string & s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) { i += u8len((unsigned char) s[i]); ++w; }
    return w;
}
// Clip+pad `s` to EXACTLY `w` display columns (plain text, no ANSI).
std::string fit(const std::string & s, int w) {
    if (w <= 0) return "";
    std::string out; int width = 0; size_t i = 0;
    while (i < s.size() && width < w) {
        size_t l = u8len((unsigned char) s[i]); if (i + l > s.size()) l = 1;
        out.append(s, i, l); i += l; ++width;
    }
    if (i < s.size()) {                       // truncated → w-1 cps + ellipsis
        out.clear(); width = 0; i = 0;
        while (i < s.size() && width < w - 1) {
            size_t l = u8len((unsigned char) s[i]); if (i + l > s.size()) l = 1;
            out.append(s, i, l); i += l; ++width;
        }
        out += "\xe2\x80\xa6"; ++width;       // …
    }
    while (width < w) { out += ' '; ++width; }
    return out;
}
std::string repeat(const char * cp, int n) {
    std::string s; for (int i = 0; i < n; ++i) s += cp; return s;
}

// ===========================================================================
// JSON readers (type-guarded; never throw)
// ===========================================================================
const json & jobj(const json & j, const char * k) {
    static const json null_j;
    if (j.is_object()) { auto it = j.find(k); if (it != j.end()) return *it; }
    return null_j;
}
std::string js(const json & j, const char * k, const std::string & def = "") {
    const json & v = jobj(j, k);
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_float())  { char b[32]; std::snprintf(b, sizeof b, "%g", v.get<double>()); return b; }
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return def;
}
long long ji(const json & j, const char * k, long long def = 0) {
    const json & v = jobj(j, k);
    if (v.is_number()) return (long long) v.get<double>();
    if (v.is_string()) { try { return std::stoll(v.get<std::string>()); } catch (...) {} }
    return def;
}
double jd(const json & j, const char * k, double def = 0) {
    const json & v = jobj(j, k);
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) { try { return std::stod(v.get<std::string>()); } catch (...) {} }
    return def;
}
bool jb(const json & j, const char * k, bool def = false) {
    const json & v = jobj(j, k);
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number())  return v.get<double>() != 0;
    return def;
}

// ===========================================================================
// Formatting
// ===========================================================================
std::string fmt_bytes(double b) {
    const char * u[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    int i = 0; while (b >= 1024.0 && i < 5) { b /= 1024.0; ++i; }
    char out[40];
    std::snprintf(out, sizeof out, (i == 0 ? "%.0f %s" : "%.1f %s"), b, u[i]);
    return out;
}
std::string fmt_int(long long n) {
    std::string s = std::to_string(n < 0 ? -n : n), out;
    int c = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (c && c % 3 == 0) out += ',';
        out += *it; ++c;
    }
    if (n < 0) out += '-';
    std::reverse(out.begin(), out.end());
    return out;
}
std::string fmt_dur(long long s) {
    if (s < 0) s = 0;
    long long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60, sec = s % 60;
    char b[48];
    if (d) std::snprintf(b, sizeof b, "%lldd %lldh", d, h);
    else if (h) std::snprintf(b, sizeof b, "%lldh %lldm", h, m);
    else if (m) std::snprintf(b, sizeof b, "%lldm %llds", m, sec);
    else std::snprintf(b, sizeof b, "%llds", sec);
    return b;
}
std::string fmt_ago(long long epoch_s) {
    if (epoch_s <= 0) return "never";
    long long now = (long long) ::time(nullptr);
    long long d = now - epoch_s;
    if (d < 0) d = 0;
    if (d < 5) return "just now";
    return fmt_dur(d) + " ago";
}
std::string f1(double x) { char b[32]; std::snprintf(b, sizeof b, "%.1f", x); return b; }
std::string f2(double x) { char b[32]; std::snprintf(b, sizeof b, "%.2f", x); return b; }
std::string base_name(const std::string & p) {
    auto s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// ===========================================================================
// Status panels
// ===========================================================================
enum { NORMAL = 0, GOOD, WARN, BAD, MUTED, ACCENT };
struct Row { std::string label, value; int tone = NORMAL; };
struct Panel { std::string title; std::vector<Row> rows; };
struct StatusView { std::vector<Panel> colA, colB, colC; };

RGB tone_rgb(int tone, const Theme & th) {
    switch (tone) {
        case GOOD:   return th.success;
        case WARN:   return th.warning;
        case BAD:    return th.error;
        case MUTED:  return th.muted;
        case ACCENT: return th.accent;
        default:     return th.text;
    }
}
// on/off chip — `on` is GOOD by default, or WARN when the on-state is a
// security-relevant surface (fs, bash, mcp_auth=off, etc.).
Row chip(const std::string & label, bool on, bool warn_on = false) {
    return { label, on ? "on" : "off",
             on ? (warn_on ? WARN : GOOD) : MUTED };
}

StatusView build_status(const json & s) {
    StatusView v;
    const json & server = jobj(s, "server");
    const json & model  = jobj(s, "model");
    const json & par    = jobj(s, "parameters");
    const json & svc    = jobj(s, "services");
    const json & met    = jobj(s, "metrics");
    const json & mo     = jobj(s, "models");
    const json & sys    = jobj(mo, "system");
    const json & cat    = jobj(mo, "catalog");
    const json & dl     = jobj(mo, "download");

    // --- Column A: Server / Model / Parameters ----------------------------
    {
        Panel p; p.title = "Server";
        bool exec = js(server, "status") == "executing";
        long long inflight = ji(model, "in_flight");
        p.rows.push_back({ "State", exec ? ("executing (" + std::to_string(inflight) + ")") : "idle",
                           exec ? WARN : GOOD });
        p.rows.push_back({ "Uptime",  fmt_dur(ji(server, "uptime_seconds")) });
        p.rows.push_back({ "Backend", js(server, "backend", "?"), ACCENT });
        p.rows.push_back({ "Listen",  js(server, "host") + ":" + js(server, "port") });
        p.rows.push_back({ "PID",     js(server, "pid") });
        p.rows.push_back({ "Version", js(server, "version") });
        v.colA.push_back(std::move(p));
    }
    {
        Panel p; p.title = "Running model";
        p.rows.push_back({ "Model", js(model, "model_id", "?"), ACCENT });
        if (js(model, "alias") != js(model, "model_id") && !js(model, "alias").empty())
            p.rows.push_back({ "Alias", js(model, "alias") });
        p.rows.push_back({ "File", base_name(js(model, "model_path")) });
        long long nctx = ji(model, "context_window");
        long long kv   = ji(model, "kv_cache_used");
        p.rows.push_back({ "Context", fmt_int(nctx) });
        std::string kvs = fmt_int(kv) + " / " + fmt_int(nctx);
        if (nctx > 0) kvs += "  (" + std::to_string((int) std::lround(100.0 * kv / nctx)) + "%)";
        p.rows.push_back({ "KV cache", kvs });
        p.rows.push_back({ "Last gen", f1(jd(model, "last_gen_tps")) + " tok/s" });
        p.rows.push_back({ "Last tokens", fmt_int(ji(model, "last_prompt_tokens")) + " in / "
                           + fmt_int(ji(model, "last_predicted_tokens")) + " out" });
        v.colA.push_back(std::move(p));
    }
    {
        Panel p; p.title = "Active parameters";
        p.rows.push_back({ "Context",   fmt_int(ji(par, "context")) });
        p.rows.push_back({ "GPU layers", js(par, "gpu_layers") });
        p.rows.push_back({ "Threads",   js(par, "threads") + " / " + js(par, "threads_batch") + " batch" });
        p.rows.push_back({ "Batch",     js(par, "batch") });
        p.rows.push_back({ "Parallel",  js(par, "parallel") });
        p.rows.push_back({ "Max tokens", js(par, "max_tokens") });
        p.rows.push_back({ "RoPE",      js(par, "rope_scaling", "auto") });
        p.rows.push_back({ "Temp / top-p", f2(jd(par, "temperature")) + " / " + f2(jd(par, "top_p")) });
        p.rows.push_back({ "top-k / min-p", js(par, "top_k") + " / " + f2(jd(par, "min_p")) });
        std::string preset = js(par, "preset", "—");
        if (jb(par, "preset_authoritative")) preset += " (forced)";
        p.rows.push_back({ "Preset", preset });
        p.rows.push_back({ "Reasoning", js(par, "reasoning_effort", "auto") });
        p.rows.push_back(chip("flash_attn", jb(par, "flash_attn")));
        p.rows.push_back(chip("mlock", jb(par, "mlock")));
        p.rows.push_back(chip("mmap", jb(par, "mmap")));
        p.rows.push_back(chip("strip-think", jb(par, "no_think"), /*warn_on=*/true));
        v.colA.push_back(std::move(p));
    }

    // --- Column B: Services & tools ---------------------------------------
    {
        Panel p; p.title = "Services & tools";
        p.rows.push_back(chip("MCP",        jb(svc, "mcp", true)));
        p.rows.push_back(chip("MCP auth",   jb(svc, "mcp_auth"), true));
        p.rows.push_back(chip("API auth",   jb(svc, "api_auth")));
        p.rows.push_back(chip("/models gate", jb(svc, "models_auth")));
        p.rows.push_back(chip("RAG",        jb(svc, "memory_rag")));
        p.rows.push_back(chip("fs",         jb(svc, "allow_fs"), true));
        p.rows.push_back(chip("bash",       jb(svc, "allow_bash"), true));
        p.rows.push_back(chip("google",     jb(svc, "google_search")));
        p.rows.push_back(chip("metrics",    jb(svc, "metrics")));
        p.rows.push_back(chip("datetime",   jb(svc, "datetime_injection")));
        p.rows.push_back(chip("verbose",    jb(svc, "verbose"), true));
        p.rows.push_back({ "WebUI", js(svc, "webui_mode", "?") });
        p.rows.push_back({ "Knowledge", js(svc, "knowledge_cutoff", "—") });
        if (!js(svc, "sandbox").empty())    p.rows.push_back({ "Sandbox", js(svc, "sandbox"), WARN });
        if (!js(svc, "memory_dir").empty()) p.rows.push_back({ "Memory dir", js(svc, "memory_dir") });
        const json & tools = jobj(svc, "tools");
        p.rows.push_back({ "Tools", std::to_string(ji(svc, "tools_count")) + " registered", ACCENT });
        if (tools.is_array())
            for (const auto & t : tools)
                p.rows.push_back({ "· " + js(t, "name"), js(t, "description"), MUTED });
        v.colB.push_back(std::move(p));
    }

    // --- Column C: Hardware / Catalog / Download / Metrics ----------------
    {
        Panel p; p.title = "Hardware";
        std::string cpu = std::to_string(ji(sys, "cpu_cores")) + " cores";
        if (!js(sys, "cpu_name").empty()) cpu = js(sys, "cpu_name") + " (" + cpu + ")";
        p.rows.push_back({ "CPU", cpu });
        p.rows.push_back({ "RAM", f1(jd(sys, "total_ram_gb")) + " GB  ("
                           + f1(jd(sys, "available_ram_gb")) + " free)" });
        if (jb(sys, "has_gpu"))
            p.rows.push_back({ "GPU", js(sys, "gpu_name", "GPU") + "  "
                               + f1(jd(sys, "gpu_vram_gb")) + " GB", GOOD });
        else
            p.rows.push_back({ "GPU", "none", MUTED });
        p.rows.push_back({ "Backend", js(sys, "backend", "?"), ACCENT });
        v.colC.push_back(std::move(p));
    }
    {
        Panel p; p.title = "Catalog";
        p.rows.push_back({ "Models", fmt_int(ji(cat, "models")) + " / " + fmt_int(ji(cat, "catalog_size")) });
        p.rows.push_back({ "Updated", jb(cat, "refreshing") ? "refreshing…" : fmt_ago(ji(cat, "last_refresh")),
                           jb(cat, "refreshing") ? WARN : NORMAL });
        p.rows.push_back({ "Local models", fmt_int(ji(mo, "local_models")) });
        if (!js(cat, "data_dir").empty())   p.rows.push_back({ "Data dir", js(cat, "data_dir"), MUTED });
        if (!js(cat, "cache_file").empty()) p.rows.push_back({ "Cache", base_name(js(cat, "cache_file")), MUTED });
        if (!js(cat, "error").empty())      p.rows.push_back({ "Error", js(cat, "error"), BAD });
        v.colC.push_back(std::move(p));
    }
    {
        Panel p; p.title = "Download";
        std::string state = js(dl, "state", "idle");
        if (state == "downloading") {
            p.rows.push_back({ "State", "downloading", WARN });
            p.rows.push_back({ "File", base_name(js(dl, "filename")).empty() ? js(dl, "repo")
                                                                             : base_name(js(dl, "filename")) });
            p.rows.push_back({ "Progress", f1(jd(dl, "percent")) + "%  ("
                               + fmt_bytes(jd(dl, "downloaded_bytes")) + " / "
                               + fmt_bytes(jd(dl, "total_bytes")) + ")", ACCENT });
        } else if (state == "error") {
            p.rows.push_back({ "State", "error", BAD });
            p.rows.push_back({ "Error", js(dl, "error"), BAD });
        } else if (state == "done") {
            p.rows.push_back({ "State", "done", GOOD });
            p.rows.push_back({ "Last", base_name(js(dl, "filename")) });
        } else {
            p.rows.push_back({ "State", "idle — no active download", MUTED });
        }
        v.colC.push_back(std::move(p));
    }
    {
        Panel p; p.title = "Request metrics";
        p.rows.push_back({ "Requests",  fmt_int(ji(met, "requests")) });
        long long errs = ji(met, "errors");
        p.rows.push_back({ "Errors",    fmt_int(errs), errs > 0 ? BAD : NORMAL });
        p.rows.push_back({ "Tool calls", fmt_int(ji(met, "tool_calls")) });
        p.rows.push_back({ "In flight", fmt_int(ji(met, "in_flight")) });
        p.rows.push_back({ "Traffic",   fmt_bytes(jd(met, "bytes_in")) + " in / "
                           + fmt_bytes(jd(met, "bytes_out")) + " out" });
        if (ji(met, "rss_bytes") > 0)
            p.rows.push_back({ "RSS", fmt_bytes(jd(met, "rss_bytes")) });
        const json & la = jobj(met, "load_avg");
        if (la.is_array() && la.size() == 3 && la[0].is_number() && la[0].get<double>() >= 0)
            p.rows.push_back({ "Load avg", f2(la[0].get<double>()) + " " + f2(la[1].get<double>())
                               + " " + f2(la[2].get<double>()) });
        if (ji(met, "fd_limit") > 0)
            p.rows.push_back({ "Open FDs", fmt_int(ji(met, "open_fds")) + " / " + fmt_int(ji(met, "fd_limit")) });
        v.colC.push_back(std::move(p));
    }
    return v;
}

// Render one column of panels to lines that are EXACTLY `innerw` plain cols.
std::vector<std::string> render_column(const std::vector<Panel> & ps, int innerw, const Theme & th) {
    std::vector<std::string> out;
    const std::string blank(innerw, ' ');
    for (size_t pi = 0; pi < ps.size(); ++pi) {
        const Panel & p = ps[pi];
        out.push_back(BOLD() + fg(th.primary) + fit(p.title, innerw) + RESET());
        out.push_back(fg(th.border) + repeat("\xe2\x94\x80", innerw) + RESET());  // ─
        int labelw = 6;
        for (const auto & r : p.rows) labelw = std::max(labelw, disp_width(r.label));
        labelw = std::min(labelw, std::max(6, innerw - 8));
        int valw = innerw - labelw - 1;
        for (const auto & r : p.rows) {
            if (valw <= 0) { out.push_back(fg(th.muted) + fit(r.label, innerw) + RESET()); continue; }
            out.push_back(fg(th.muted) + fit(r.label, labelw) + RESET() + " "
                          + fg(tone_rgb(r.tone, th)) + fit(r.value, valw) + RESET());
        }
        if (pi + 1 < ps.size()) out.push_back(blank);
    }
    return out;
}
std::vector<Panel> concat(std::vector<Panel> a, const std::vector<Panel> & b) {
    for (const auto & p : b) a.push_back(p);
    return a;
}
// Lay the three status column-groups out into 1/2/3 terminal columns by width.
std::vector<std::string> render_status(const StatusView & v, int width, const Theme & th) {
    const int gap = 2;
    std::vector<std::vector<Panel>> cols;
    if (width >= 112)      cols = { v.colA, v.colB, v.colC };
    else if (width >= 74)  cols = { concat(v.colA, v.colB), v.colC };
    else                   cols = { concat(concat(v.colA, v.colB), v.colC) };
    int n = (int) cols.size();
    int innerw = (width - gap * (n - 1)) / n;
    if (innerw < 16) innerw = 16;
    std::vector<std::vector<std::string>> rendered;
    size_t maxrows = 0;
    for (auto & c : cols) { rendered.push_back(render_column(c, innerw, th)); maxrows = std::max(maxrows, rendered.back().size()); }
    const std::string blank(innerw, ' ');
    std::vector<std::string> out;
    for (size_t r = 0; r < maxrows; ++r) {
        std::string line;
        for (int c = 0; c < n; ++c) {
            line += (r < rendered[c].size()) ? rendered[c][r] : blank;
            if (c + 1 < n) line += std::string(gap, ' ');
        }
        out.push_back(line);
    }
    return out;
}

int term_cols() { winsize w{}; return (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) ? w.ws_col : 100; }
int term_rows() { winsize w{}; return (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) ? w.ws_row : 30; }

// ===========================================================================
// print_status — one-shot, to a FILE* (--status / REPL /status)
// ===========================================================================
}  // namespace

int print_status(Client & cli, const ui::Style & st, std::FILE * out) {
    detect_term();
    g_color = st.color;
    std::string body;
    if (!cli.get_json("/models/api/status", body)) {
        std::fprintf(stderr, "%serror:%s %s\n", st.red(), st.reset(), cli.last_error().c_str());
        return 1;
    }
    json s;
    try { s = json::parse(body); }
    catch (const std::exception & e) {
        std::fprintf(stderr, "%serror:%s could not parse status: %s\n", st.red(), st.reset(), e.what());
        return 1;
    }
    Theme th = theme_dark();
    int w = std::min(std::max(term_cols(), 40), 160);
    for (const auto & line : render_status(build_status(s), w, th)) {
        std::fputs(line.c_str(), out);
        std::fputc('\n', out);
    }
    return 0;
}

namespace {
// ===========================================================================
// Raw-mode terminal + byte→key parser (trimmed from src/tui.cpp)
// ===========================================================================
void tw(const std::string & s) { ::fwrite(s.data(), 1, s.size(), stdout); }

struct Term {
    termios saved{}; bool raw = false;
    bool enter() {
        if (::tcgetattr(STDIN_FILENO, &saved) != 0) return false;
        termios t = saved;
        t.c_lflag &= ~(ICANON | ECHO | ISIG);
        t.c_iflag &= ~(IXON | ICRNL);
        t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
        if (::tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) return false;
        raw = true;
        tw("\033[?1049h\033[?25l\033[2J\033[H"); ::fflush(stdout);
        return true;
    }
    void leave() {
        if (!raw) return;
        tw("\033[?25h\033[?1049l"); ::fflush(stdout);
        ::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        raw = false;
    }
};

struct Key {
    enum Type { None, Char, Enter, Backspace, Up, Down, Left, Right,
                Home, End, PgUp, PgDn, Tab, ShiftTab, Esc, CtrlC } type = None;
    std::string text;
};
struct InputParser {
    std::string buf;
    std::vector<Key> feed(const char * data, size_t n) {
        buf.append(data, n);
        std::vector<Key> out; size_t i = 0;
        auto emit = [&](Key::Type t, std::string s = "") { out.push_back({ t, std::move(s) }); };
        while (i < buf.size()) {
            unsigned char c = (unsigned char) buf[i];
            if (c == 0x1b) {
                if (i + 1 >= buf.size()) break;
                unsigned char c1 = (unsigned char) buf[i + 1];
                if (c1 == '[' || c1 == 'O') {
                    size_t j = i + 2;
                    while (j < buf.size() && !((unsigned char) buf[j] >= 0x40 && (unsigned char) buf[j] <= 0x7e)) ++j;
                    if (j >= buf.size()) break;
                    std::string seq = buf.substr(i + 2, j - (i + 2));
                    char fin = buf[j]; i = j + 1;
                    auto p0 = [&]() -> long { try { return seq.empty() ? 1 : std::stol(seq); } catch (...) { return 1; } };
                    switch (fin) {
                        case 'A': emit(Key::Up); break;
                        case 'B': emit(Key::Down); break;
                        case 'C': emit(Key::Right); break;
                        case 'D': emit(Key::Left); break;
                        case 'H': emit(Key::Home); break;
                        case 'F': emit(Key::End); break;
                        case 'Z': emit(Key::ShiftTab); break;
                        case '~':
                            switch (p0()) {
                                case 1: case 7: emit(Key::Home); break;
                                case 4: case 8: emit(Key::End); break;
                                case 5: emit(Key::PgUp); break;
                                case 6: emit(Key::PgDn); break;
                            }
                            break;
                        default: break;
                    }
                    continue;
                }
                emit(Key::Esc); ++i; continue;
            }
            if (c == 0x0d || c == 0x0a) { emit(Key::Enter); ++i; continue; }
            if (c == 0x7f || c == 0x08) { emit(Key::Backspace); ++i; continue; }
            if (c == 0x09) { emit(Key::Tab); ++i; continue; }
            if (c == 0x03) { emit(Key::CtrlC); ++i; continue; }
            if (c < 0x20) { ++i; continue; }
            size_t adv = u8len(c);
            if (i + adv > buf.size()) break;
            emit(Key::Char, buf.substr(i, adv)); i += adv;
        }
        buf.erase(0, i);
        return out;
    }
};
std::vector<Key> poll_keys(InputParser & ip, int timeout_ms) {
    pollfd pfd{ STDIN_FILENO, POLLIN, 0 };
    int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr > 0 && (pfd.revents & POLLIN)) {
        char b[4096]; ssize_t n = ::read(STDIN_FILENO, b, sizeof b);
        if (n > 0) return ip.feed(b, (size_t) n);
    }
    return {};
}

using clock_ = std::chrono::steady_clock;
long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_::now().time_since_epoch()).count();
}

// ===========================================================================
// Fetch helpers
// ===========================================================================
bool get(Client & cli, const std::string & path, json & out, std::string & err) {
    std::string body;
    if (!cli.get_json(path, body)) { err = cli.last_error(); return false; }
    try { out = json::parse(body); } catch (const std::exception & e) { err = std::string("parse: ") + e.what(); return false; }
    return true;
}
bool post(Client & cli, const std::string & path, const json & b, json & out, std::string & err) {
    std::string body;
    if (!cli.post_json(path, b.dump(), body)) { err = cli.last_error(); return false; }
    if (body.empty()) { out = json::object(); return true; }
    try { out = json::parse(body); } catch (...) { out = json::object(); }
    return true;
}
std::string urlenc(const std::string & s) {
    static const char * hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char) c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 0xf]; }
    }
    return o;
}

// Blit a full frame (vector of already-width-bounded, colour-annotated lines).
void blit(const std::vector<std::string> & frame, int rows) {
    std::string out = "\033[?2026h\033[H";
    for (int i = 0; i < rows; ++i) {
        if (i < (int) frame.size()) out += frame[i];
        out += "\033[K";
        if (i < rows - 1) out += "\r\n";
    }
    out += "\033[J\033[?2026l";
    tw(out); ::fflush(stdout);
}
// Transient one-line note on the bottom row (cursor is hidden).
void note(const std::string & msg, const Theme & th, int rows) {
    tw("\033[" + std::to_string(rows) + ";1H\033[K" + fg(th.info) + " " + msg + RESET());
    ::fflush(stdout);
}

// ===========================================================================
// show_status_screen — live, read-only Status view
// ===========================================================================
}  // namespace

int show_status_screen(Client & cli, const Options & opt) {
    detect_term();
    g_color = true;
    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) {
        ui::Style st = ui::detect_style();
        return print_status(cli, st, stdout);
    }
    Theme th = pick_theme(opt.theme);
    Term term;
    if (!term.enter()) { ui::Style st = ui::detect_style(); return print_status(cli, st, stdout); }

    InputParser ip;
    json status; std::string err;
    long long last_fetch = 0;
    bool first = true, quit = false;
    while (!quit) {
        if (first || now_ms() - last_fetch > 2000) {
            if (first) { note("loading…", th, term_rows()); first = false; }
            get(cli, "/models/api/status", status, err);
            last_fetch = now_ms();
        }
        int cols = term_cols(), rows = term_rows();
        std::vector<std::string> frame;
        std::string title = " " + fg(th.accent) + BOLD() + "easyai status" + RESET()
                          + fg(th.muted) + "  " + opt.url + RESET();
        frame.push_back(title);
        frame.push_back(fg(th.border) + repeat("\xe2\x94\x80", cols) + RESET());
        if (!err.empty()) frame.push_back(fg(th.error) + fit(" " + err, cols) + RESET());
        else for (auto & l : render_status(build_status(status), cols, th)) frame.push_back(l);
        while ((int) frame.size() < rows - 1) frame.push_back("");
        frame.resize(std::max(0, rows - 1));
        frame.push_back(fg(th.muted) + fit("  q quit · r refresh · auto-refresh 2s", cols) + RESET());
        blit(frame, rows);

        for (auto & k : poll_keys(ip, 300)) {
            if (k.type == Key::CtrlC || k.type == Key::Esc) { quit = true; break; }
            if (k.type == Key::Char && (k.text == "q" || k.text == "Q")) { quit = true; break; }
            if (k.type == Key::Char && (k.text == "r" || k.text == "R")) last_fetch = 0;
        }
    }
    term.leave();
    return 0;
}

namespace {

// ===========================================================================
// The tabbed manager
// ===========================================================================
enum Tab { TAB_STATUS = 0, TAB_LOCAL, TAB_RECOMMEND, TAB_DOWNLOADS, TAB_COUNT };
const char * tab_name(int t) {
    switch (t) { case TAB_STATUS: return "Status"; case TAB_LOCAL: return "Local";
                 case TAB_RECOMMEND: return "Recommend"; case TAB_DOWNLOADS: return "Downloads"; }
    return "?";
}

struct Manager {
    Client & cli;
    Options  opt;
    Theme    th;
    Term     term;
    InputParser ip;

    int  tab = TAB_STATUS;
    bool quit = false;
    std::string err, info_msg;
    int cols = 100, rows = 30;

    // per-tab scroll / selection
    int sel_local = 0, top_local = 0;
    int sel_rec = 0, top_rec = 0;
    int scroll_status = 0;

    // data
    json status, local, recommend, hf_files, dlstatus;
    long long status_fetched = 0, dl_fetched = 0;
    bool local_loaded = false, rec_loaded = false;

    // recommend filters
    std::string rec_search;
    int rec_minfit = 0;       // index into fit_opts
    int rec_usecase = 0;      // index into uc_opts

    // download tab
    std::string dl_repo;
    int sel_file = 0, top_file = 0;

    // editing (text input)
    bool editing = false;
    std::string * edit_buf = nullptr;
    std::string edit_label;
    std::function<void()> edit_commit;

    // overlay (detail / confirm / help)
    enum Overlay { OV_NONE, OV_DETAIL, OV_CONFIRM, OV_HELP } overlay = OV_NONE;
    std::vector<std::string> detail_lines;
    int detail_scroll = 0;
    std::string confirm_msg;
    std::function<void()> confirm_action;

    Manager(Client & c, const Options & o) : cli(c), opt(o), th(pick_theme(o.theme)) {}

    // ---- small helpers ----
    void flash(const std::string & m) { note(m, th, rows); }
    void set_info(const std::string & m) { info_msg = m; err.clear(); }
    void set_err(const std::string & m) { err = m; }

    static const std::vector<std::string> & fit_opts() {
        static const std::vector<std::string> v = { "good", "marginal", "perfect", "all" }; return v;
    }
    static const std::vector<std::string> & uc_opts() {
        static const std::vector<std::string> v = { "all", "general", "coding", "reasoning", "chat", "multimodal", "embedding" }; return v;
    }

    // ---- fetching ----
    void fetch_status(bool force = false) {
        if (!force && now_ms() - status_fetched < 2000) return;
        get(cli, "/models/api/status", status, err);
        status_fetched = now_ms();
    }
    void fetch_local() {
        flash("loading local models…");
        json out; std::string e;
        if (get(cli, "/models/api/local", out, e)) { local = out; err.clear(); }
        else set_err(e);
        local_loaded = true;
        clamp_sel(sel_local, top_local, local_rows());
    }
    void fetch_recommend() {
        flash("loading catalogue…");
        std::string p = "/models/api/models?limit=60&sort=score";
        p += "&min_fit=" + urlenc(fit_opts()[rec_minfit]);
        p += "&use_case=" + urlenc(uc_opts()[rec_usecase]);
        if (!rec_search.empty()) p += "&search=" + urlenc(rec_search);
        json out; std::string e;
        if (get(cli, p, out, e)) { recommend = out; err.clear(); }
        else set_err(e);
        rec_loaded = true;
        clamp_sel(sel_rec, top_rec, rec_rows());
    }
    void fetch_dlstatus(bool force = false) {
        if (!force && now_ms() - dl_fetched < 800) return;
        json out; std::string e;
        if (get(cli, "/models/api/download/status", out, e)) dlstatus = out;
        dl_fetched = now_ms();
    }

    int local_rows() {
        const json & m = jobj(local, "models");
        return m.is_array() ? (int) m.size() : 0;
    }
    int rec_rows() {
        const json & m = jobj(recommend, "models");
        return m.is_array() ? (int) m.size() : 0;
    }
    int file_rows() {
        const json & f = jobj(hf_files, "files");
        return f.is_array() ? (int) f.size() : 0;
    }
    void clamp_sel(int & sel, int & top, int n) {
        if (n <= 0) { sel = 0; top = 0; return; }
        if (sel >= n) sel = n - 1;
        if (sel < 0) sel = 0;
        int body = body_h() - 2;
        if (body < 1) body = 1;
        if (sel < top) top = sel;
        if (sel >= top + body) top = sel - body + 1;
        if (top < 0) top = 0;
    }
    int body_h() { return std::max(1, rows - 4); }   // header(2) + footer(2)

    // ---- actions ----
    void run_model(const std::string & name) {
        flash("switching to " + name + " …");
        // Plain hot-swap (in-memory; reverts to --model on restart). The
        // "set as default" symlink is the separate 's' action, mirroring
        // the webui's Run vs. Run+slink split.
        json out, b; b["name"] = name; b["link"] = false;
        std::string e;
        if (post(cli, "/models/api/run", b, out, e)) {
            if (jb(out, "ok", true)) set_info("now running: " + js(out, "model_id", name));
            else set_err(js(out, "error", "run failed"));
        } else set_err(e);
        fetch_local();
        fetch_status(true);
    }
    void symlink_model(const std::string & name) {
        json out, b; b["name"] = name; std::string e;
        if (post(cli, "/models/api/symlink", b, out, e))
            set_info(jb(out, "ok", true) ? "symlinked " + name : js(out, "error", "symlink failed"));
        else set_err(e);
        fetch_local();
    }
    void delete_model(const std::string & name) {
        json out, b; b["name"] = name; std::string e;
        if (post(cli, "/models/api/local/delete", b, out, e)) {
            if (jb(out, "ok", true)) set_info("deleted " + name);
            else set_err(js(out, "error", "delete failed"));
        } else set_err(e);
        fetch_local();
    }
    void refresh_catalog() {
        json out; std::string e;
        post(cli, "/models/api/refresh", json::object(), out, e);
        set_info("catalogue refresh requested…");
        fetch_recommend();
    }
    void start_download(const std::string & repo, const std::string & filename) {
        json out, b; b["repo"] = repo; if (!filename.empty()) b["filename"] = filename;
        std::string e;
        if (post(cli, "/models/api/download", b, out, e)) set_info("download started: " + repo);
        else set_err(e);
        fetch_dlstatus(true);
    }
    void cancel_download() {
        json out; std::string e;
        post(cli, "/models/api/download/cancel", json::object(), out, e);
        set_info("download cancel requested");
        fetch_dlstatus(true);
    }

    // ---- detail overlays ----
    void flatten(std::vector<std::string> & out, const json & o, const std::string & prefix) {
        if (!o.is_object()) return;
        for (auto it = o.begin(); it != o.end(); ++it) {
            const json & v = it.value();
            std::string key = prefix + it.key();
            if (v.is_object()) {
                out.push_back(fg(th.accent) + fit(key, cols - 8) + RESET());
                flatten(out, v, "  ");
            } else if (v.is_array()) {
                out.push_back(fg(th.muted) + fit(key + " [" + std::to_string(v.size()) + "]", cols - 8) + RESET());
            } else {
                std::string val = v.is_string() ? v.get<std::string>() : v.dump();
                out.push_back(fg(th.muted) + fit(key, 24) + RESET() + " " + fg(th.text) + val + RESET());
            }
        }
    }
    void open_local_detail() {
        const json & m = jobj(local, "models");
        if (!m.is_array() || sel_local >= (int) m.size()) return;
        std::string name = js(m[sel_local], "name");
        flash("loading " + name + " …");
        json d; std::string e;
        if (!get(cli, "/models/api/local/detail?file=" + urlenc(name), d, e)) { set_err(e); return; }
        detail_lines.clear(); detail_scroll = 0;
        detail_lines.push_back(BOLD() + fg(th.primary) + fit(js(d, "display_name", name), cols - 8) + RESET());
        detail_lines.push_back("");
        const char * secs[] = { "params", "fit", "system", "ini_profile" };
        for (const char * s : secs) {
            const json & o = jobj(d, s);
            if (o.is_object() && !o.empty()) {
                detail_lines.push_back(BOLD() + fg(th.accent) + s + RESET());
                flatten(detail_lines, o, "  ");
                detail_lines.push_back("");
            }
        }
        overlay = OV_DETAIL;
    }
    void open_rec_detail() {
        const json & m = jobj(recommend, "models");
        if (!m.is_array() || sel_rec >= (int) m.size()) return;
        std::string repo = js(m[sel_rec], "name");
        flash("loading " + repo + " …");
        json d; std::string e;
        if (!get(cli, "/models/api/hf/detail?repo=" + urlenc(repo), d, e)) { set_err(e); return; }
        detail_lines.clear(); detail_scroll = 0;
        detail_lines.push_back(BOLD() + fg(th.primary) + fit(repo, cols - 8) + RESET());
        detail_lines.push_back("");
        const char * secs[] = { "params", "fit", "plan" };
        for (const char * s : secs) {
            const json & o = jobj(d, s);
            if (o.is_object() && !o.empty()) {
                detail_lines.push_back(BOLD() + fg(th.accent) + s + RESET());
                flatten(detail_lines, o, "  ");
                detail_lines.push_back("");
            }
        }
        const json & srcs = jobj(d, "gguf_sources");
        if (srcs.is_array() && !srcs.empty()) {
            detail_lines.push_back(BOLD() + fg(th.accent) + "GGUF sources" + RESET());
            for (const auto & s : srcs)
                detail_lines.push_back("  " + fg(th.text) + (s.is_string() ? s.get<std::string>() : js(s, "repo")) + RESET());
            detail_lines.push_back("");
        }
        detail_lines.push_back(fg(th.muted) + "d: download best quant from this repo · esc: back" + RESET());
        overlay = OV_DETAIL;
    }

    // ---- text input ----
    void start_edit(const std::string & label, std::string & buf, std::function<void()> commit) {
        editing = true; edit_label = label; edit_buf = &buf; edit_commit = std::move(commit);
    }
    void handle_edit_key(const Key & k) {
        if (k.type == Key::Esc) { editing = false; edit_buf = nullptr; return; }
        if (k.type == Key::Enter) { editing = false; auto c = edit_commit; edit_buf = nullptr; if (c) c(); return; }
        if (k.type == Key::Backspace) { if (edit_buf && !edit_buf->empty()) {
            // pop one UTF-8 codepoint
            size_t n = edit_buf->size(); size_t s = n - 1;
            while (s > 0 && ((unsigned char)(*edit_buf)[s] & 0xc0) == 0x80) --s;
            edit_buf->erase(s); } return; }
        if (k.type == Key::Char && edit_buf) *edit_buf += k.text;
    }

    void render();
    void render_header(std::vector<std::string> & f);
    void render_status_tab(std::vector<std::string> & f);
    void render_local_tab(std::vector<std::string> & f);
    void render_recommend_tab(std::vector<std::string> & f);
    void render_downloads_tab(std::vector<std::string> & f);
    void render_footer(std::vector<std::string> & f, const std::string & hint);
    void render_overlay(std::vector<std::string> & f);
    void handle_key(const Key & k);
    void on_tab_enter();
    void list_files();
    int  loop();
};

void Manager::render_header(std::vector<std::string> & f) {
    std::string bar = " " + fg(th.accent) + BOLD() + "easyai" + RESET() + fg(th.muted) + " model manager  " + RESET();
    for (int t = 0; t < TAB_COUNT; ++t) {
        if (t == tab) bar += bg(th.sel_bg) + fg(th.primary) + BOLD() + " " + tab_name(t) + " " + RESET();
        else          bar += fg(th.muted) + " " + tab_name(t) + " " + RESET();
    }
    f.push_back(bar);
    f.push_back(fg(th.border) + repeat("\xe2\x94\x80", cols) + RESET());
}
void Manager::render_footer(std::vector<std::string> & f, const std::string & hint) {
    while ((int) f.size() < rows - 2) f.push_back("");
    f.resize(std::max(0, rows - 2));
    f.push_back(fg(th.border) + repeat("\xe2\x94\x80", cols) + RESET());
    std::string left = "  " + hint;
    std::string status_line;
    if (editing) status_line = fg(th.warning) + "  " + edit_label + ": " + RESET() + fg(th.text) + *edit_buf + "_" + RESET();
    else if (!err.empty()) status_line = fg(th.error) + fit("  " + err, cols) + RESET();
    else if (!info_msg.empty()) status_line = fg(th.success) + fit("  " + info_msg, cols) + RESET();
    else status_line = fg(th.muted) + fit(left + "   ·   tab: switch · q: quit", cols) + RESET();
    f.push_back(status_line);
}

void Manager::render_status_tab(std::vector<std::string> & f) {
    auto lines = render_status(build_status(status), cols, th);
    int h = body_h();
    if (scroll_status > (int) lines.size() - h) scroll_status = std::max(0, (int) lines.size() - h);
    if (scroll_status < 0) scroll_status = 0;
    for (int i = 0; i < h; ++i) {
        int idx = i + scroll_status;
        f.push_back(idx < (int) lines.size() ? lines[idx] : "");
    }
}

void Manager::render_local_tab(std::vector<std::string> & f) {
    const json & m = jobj(local, "models");
    f.push_back(fg(th.muted) + fit("  dir: " + js(local, "dir"), cols) + RESET());
    int n = m.is_array() ? (int) m.size() : 0;
    if (n == 0) { f.push_back(""); f.push_back(fg(th.muted) + "  (no local models — use the Downloads tab)" + RESET()); return; }
    int namew = std::max(20, cols - 2 - 3 - 12 - 13);
    f.push_back("  " + fg(th.muted) + fit("name", namew) + " " + fit("size", 11) + " " + fit("modified", 12) + RESET());
    int h = body_h() - 1;
    clamp_sel(sel_local, top_local, n);
    for (int i = 0; i < h && top_local + i < n; ++i) {
        int idx = top_local + i;
        const json & mm = m[idx];
        bool cur = jb(mm, "is_current");
        bool selrow = (idx == sel_local);
        std::string marker = cur ? (fg(th.success) + "●" + RESET()) : " ";
        std::string nm = js(mm, "name");
        time_t mt = (time_t) ji(mm, "mtime");
        char date[16] = "—"; if (mt > 0) { struct tm tmv; localtime_r(&mt, &tmv); std::strftime(date, sizeof date, "%Y-%m-%d", &tmv); }
        std::string row = " " + marker + " "
            + fg(selrow ? th.sel_fg : th.text) + fit(nm, namew) + RESET() + " "
            + fg(th.muted) + fit(fmt_bytes(jd(mm, "size_bytes")), 11) + " " + fit(date, 12) + RESET();
        if (selrow) row = bg(th.sel_bg) + "\033[K" + row + RESET();
        f.push_back(row);
    }
}

void Manager::render_recommend_tab(std::vector<std::string> & f) {
    bool refreshing = jb(recommend, "refreshing");
    std::string head = "  filter: " + fg(th.accent) + fit(rec_search.empty() ? "(none)" : rec_search, 22) + RESET()
        + fg(th.muted) + "  fit≥" + RESET() + fg(th.accent) + fit_opts()[rec_minfit] + RESET()
        + fg(th.muted) + "  use:" + RESET() + fg(th.accent) + uc_opts()[rec_usecase] + RESET();
    if (refreshing) head += fg(th.warning) + "   rebuilding from HuggingFace…" + RESET();
    f.push_back(head);
    const json & m = jobj(recommend, "models");
    int n = m.is_array() ? (int) m.size() : 0;
    if (n == 0) { f.push_back(""); f.push_back(fg(th.muted) + "  (no matches — '/' search, 'f' fit, 'u' use-case, 'R' refresh)" + RESET()); return; }
    int parw = 6, qw = 8, fitw = 9, mw = 11, scw = 5, tpsw = 6, dlw = 7;
    int namew = std::max(16, cols - 2 - (parw + qw + fitw + mw + scw + tpsw + dlw + 8));
    f.push_back("  " + fg(th.muted)
        + fit("model", namew) + " " + fit("params", parw) + " " + fit("quant", qw) + " "
        + fit("fit", fitw) + " " + fit("mode", mw) + " " + fit("score", scw) + " "
        + fit("tok/s", tpsw) + " " + fit("↓ hf", dlw) + RESET());
    int h = body_h() - 1;
    clamp_sel(sel_rec, top_rec, n);
    for (int i = 0; i < h && top_rec + i < n; ++i) {
        int idx = top_rec + i;
        const json & mm = m[idx];
        bool selrow = (idx == sel_rec);
        std::string fitlabel = js(mm, "fit_level");
        int fittone = fitlabel == "perfect" ? GOOD : fitlabel == "good" ? GOOD
                    : fitlabel == "marginal" ? WARN : BAD;
        std::string row = "  "
            + fg(selrow ? th.sel_fg : th.text) + fit(js(mm, "name"), namew) + RESET() + " "
            + fg(th.text) + fit(f1(jd(mm, "params_b")) + "B", parw) + " " + fit(js(mm, "best_quant"), qw) + " "
            + fg(tone_rgb(fittone, th)) + fit(fitlabel, fitw) + RESET() + " "
            + fg(th.muted) + fit(js(mm, "run_mode_label"), mw) + " "
            + fg(th.text) + fit(std::to_string((int) std::lround(jd(mm, "score"))), scw) + " "
            + fit(f1(jd(mm, "estimated_tps")), tpsw) + " "
            + fg(th.muted) + fit(fmt_int(ji(mm, "hf_downloads")), dlw) + RESET();
        if (selrow) row = bg(th.sel_bg) + "\033[K" + row + RESET();
        f.push_back(row);
    }
}

void Manager::render_downloads_tab(std::vector<std::string> & f) {
    // active download status bar
    std::string state = js(dlstatus, "state", "idle");
    if (state == "downloading") {
        double pct = jd(dlstatus, "percent");
        int barw = std::max(10, cols - 30);
        int filled = (int) std::lround(pct / 100.0 * barw);
        std::string bar = fg(th.success) + repeat("\xe2\x96\x88", filled) + RESET()
                        + fg(th.border) + repeat("\xe2\x96\x91", barw - filled) + RESET();
        f.push_back("  " + fg(th.warning) + "downloading " + RESET() + fg(th.text)
                    + base_name(js(dlstatus, "filename")) + RESET());
        f.push_back("  " + bar + " " + fg(th.accent) + f1(pct) + "%" + RESET());
        f.push_back(fg(th.muted) + "  " + fmt_bytes(jd(dlstatus, "downloaded_bytes")) + " / "
                    + fmt_bytes(jd(dlstatus, "total_bytes")) + "    ('c' to cancel)" + RESET());
    } else if (state == "error") {
        f.push_back("  " + fg(th.error) + "last download error: " + js(dlstatus, "error") + RESET());
    } else if (state == "done") {
        f.push_back("  " + fg(th.success) + "last: " + base_name(js(dlstatus, "filename")) + " — completed" + RESET());
    } else {
        f.push_back(fg(th.muted) + "  no active download" + RESET());
    }
    f.push_back(fg(th.border) + repeat("\xe2\x94\x80", cols) + RESET());
    f.push_back("  repo: " + fg(th.accent) + (dl_repo.empty() ? "(press 'e' to enter a HuggingFace repo)" : dl_repo) + RESET());

    const json & files = jobj(hf_files, "files");
    int n = files.is_array() ? (int) files.size() : 0;
    if (n == 0) { f.push_back(""); f.push_back(fg(th.muted) + "  'e' edit repo · 'l' list GGUF files · enter/'d' download selected" + RESET()); return; }
    int szw = 12; int namew = std::max(16, cols - 4 - szw);
    f.push_back("  " + fg(th.muted) + fit("file", namew) + " " + fit("size", szw) + RESET());
    int h = body_h() - 5;
    clamp_sel(sel_file, top_file, n);
    for (int i = 0; i < h && top_file + i < n; ++i) {
        int idx = top_file + i;
        const json & ff = files[idx];
        bool selrow = (idx == sel_file);
        std::string row = "  " + fg(selrow ? th.sel_fg : th.text) + fit(js(ff, "path"), namew) + RESET()
            + " " + fg(th.muted) + fit(fmt_bytes(jd(ff, "size_bytes")), szw) + RESET();
        if (selrow) row = bg(th.sel_bg) + "\033[K" + row + RESET();
        f.push_back(row);
    }
}

void Manager::render_overlay(std::vector<std::string> & f) {
    // overlays draw a full body; header/footer added by render()
    if (overlay == OV_CONFIRM) {
        for (int i = 0; i < body_h() / 2 - 1; ++i) f.push_back("");
        f.push_back(fg(th.warning) + BOLD() + fit("  " + confirm_msg, cols) + RESET());
        f.push_back(fg(th.muted) + "  y: confirm   ·   n / esc: cancel" + RESET());
        return;
    }
    if (overlay == OV_HELP) {
        const char * help[] = {
            "  Keys",
            "    tab / shift-tab / 1-4   switch tabs",
            "    ↑ ↓ / pgup pgdn         move selection / scroll",
            "    enter                   open detail (Local / Recommend)",
            "  Local models",
            "    r  run / hot-swap      s  symlink as current      d  delete",
            "  Recommend",
            "    /  search   f  min-fit   u  use-case   R  refresh catalogue",
            "    d  (in detail) download best quant",
            "  Downloads",
            "    e  edit repo   l  list GGUF files   enter/d  download   c  cancel",
            "  q / ctrl-c               quit",
            "",
            "  esc: close this help",
        };
        for (const char * l : help) f.push_back(fg(th.text) + l + RESET());
        return;
    }
    // OV_DETAIL
    int h = body_h();
    if (detail_scroll > (int) detail_lines.size() - h) detail_scroll = std::max(0, (int) detail_lines.size() - h);
    if (detail_scroll < 0) detail_scroll = 0;
    for (int i = 0; i < h; ++i) {
        int idx = i + detail_scroll;
        f.push_back(idx < (int) detail_lines.size() ? ("  " + detail_lines[idx]) : "");
    }
}

void Manager::render() {
    std::vector<std::string> f;
    render_header(f);
    std::string hint;
    if (overlay != OV_NONE) {
        render_overlay(f);
        hint = (overlay == OV_DETAIL) ? "↑↓ scroll · esc back" : "esc close";
    } else switch (tab) {
        case TAB_STATUS:    render_status_tab(f);    hint = "↑↓ scroll · r refresh"; break;
        case TAB_LOCAL:     render_local_tab(f);     hint = "↑↓ · enter detail · r run · s symlink · d delete"; break;
        case TAB_RECOMMEND: render_recommend_tab(f); hint = "↑↓ · enter detail · / search · f fit · u use · R refresh"; break;
        case TAB_DOWNLOADS: render_downloads_tab(f); hint = "e repo · l list · enter download · c cancel"; break;
    }
    render_footer(f, hint);
    blit(f, rows);
}

void Manager::handle_key(const Key & k) {
    if (editing) { handle_edit_key(k); return; }

    // overlay key handling
    if (overlay != OV_NONE) {
        if (overlay == OV_CONFIRM) {
            if (k.type == Key::Char && (k.text == "y" || k.text == "Y")) { auto a = confirm_action; overlay = OV_NONE; if (a) a(); }
            else if (k.type == Key::Esc || (k.type == Key::Char && (k.text == "n" || k.text == "N"))) overlay = OV_NONE;
            return;
        }
        if (overlay == OV_DETAIL) {
            if (k.type == Key::Esc) { overlay = OV_NONE; }
            else if (k.type == Key::Up) detail_scroll--;
            else if (k.type == Key::Down) detail_scroll++;
            else if (k.type == Key::PgUp) detail_scroll -= body_h();
            else if (k.type == Key::PgDn) detail_scroll += body_h();
            else if (tab == TAB_RECOMMEND && k.type == Key::Char && (k.text == "d" || k.text == "D")) {
                const json & m = jobj(recommend, "models");
                if (m.is_array() && sel_rec < (int) m.size()) { start_download(js(m[sel_rec], "name"), ""); overlay = OV_NONE; tab = TAB_DOWNLOADS; }
            }
            return;
        }
        if (overlay == OV_HELP) { if (k.type == Key::Esc || (k.type == Key::Char && k.text == "?")) overlay = OV_NONE; return; }
    }

    // global
    if (k.type == Key::CtrlC) { quit = true; return; }
    if (k.type == Key::Char && (k.text == "q" || k.text == "Q")) { quit = true; return; }
    if (k.type == Key::Char && k.text == "?") { overlay = OV_HELP; return; }
    if (k.type == Key::Tab)      { tab = (tab + 1) % TAB_COUNT; on_tab_enter(); return; }
    if (k.type == Key::ShiftTab) { tab = (tab + TAB_COUNT - 1) % TAB_COUNT; on_tab_enter(); return; }
    if (k.type == Key::Char && k.text >= "1" && k.text <= "4") { tab = k.text[0] - '1'; on_tab_enter(); return; }
    info_msg.clear();

    switch (tab) {
        case TAB_STATUS:
            if (k.type == Key::Up) scroll_status--;
            else if (k.type == Key::Down) scroll_status++;
            else if (k.type == Key::PgUp) scroll_status -= body_h();
            else if (k.type == Key::PgDn) scroll_status += body_h();
            else if (k.type == Key::Char && (k.text == "r" || k.text == "R")) fetch_status(true);
            break;
        case TAB_LOCAL: {
            int n = local_rows();
            if (k.type == Key::Up) { sel_local--; clamp_sel(sel_local, top_local, n); }
            else if (k.type == Key::Down) { sel_local++; clamp_sel(sel_local, top_local, n); }
            else if (k.type == Key::Enter) open_local_detail();
            else if (k.type == Key::Char && (k.text == "r" || k.text == "R")) {
                const json & m = jobj(local, "models");
                if (m.is_array() && sel_local < (int) m.size()) run_model(js(m[sel_local], "name"));
            } else if (k.type == Key::Char && (k.text == "s" || k.text == "S")) {
                const json & m = jobj(local, "models");
                if (m.is_array() && sel_local < (int) m.size()) symlink_model(js(m[sel_local], "name"));
            } else if (k.type == Key::Char && (k.text == "d" || k.text == "D")) {
                const json & m = jobj(local, "models");
                if (m.is_array() && sel_local < (int) m.size()) {
                    std::string name = js(m[sel_local], "name");
                    confirm_msg = "delete " + name + " ?";
                    confirm_action = [this, name] { delete_model(name); };
                    overlay = OV_CONFIRM;
                }
            }
            break;
        }
        case TAB_RECOMMEND: {
            int n = rec_rows();
            if (k.type == Key::Up) { sel_rec--; clamp_sel(sel_rec, top_rec, n); }
            else if (k.type == Key::Down) { sel_rec++; clamp_sel(sel_rec, top_rec, n); }
            else if (k.type == Key::Enter) open_rec_detail();
            else if (k.type == Key::Char && k.text == "/") start_edit("search", rec_search, [this] { fetch_recommend(); });
            else if (k.type == Key::Char && (k.text == "f" || k.text == "F")) { rec_minfit = (rec_minfit + 1) % (int) fit_opts().size(); fetch_recommend(); }
            else if (k.type == Key::Char && (k.text == "u" || k.text == "U")) { rec_usecase = (rec_usecase + 1) % (int) uc_opts().size(); fetch_recommend(); }
            else if (k.type == Key::Char && k.text == "R") refresh_catalog();
            break;
        }
        case TAB_DOWNLOADS: {
            int n = file_rows();
            if (k.type == Key::Up) { sel_file--; clamp_sel(sel_file, top_file, n); }
            else if (k.type == Key::Down) { sel_file++; clamp_sel(sel_file, top_file, n); }
            else if (k.type == Key::Char && (k.text == "e" || k.text == "E"))
                start_edit("repo", dl_repo, [this] { if (!dl_repo.empty()) list_files(); });
            else if (k.type == Key::Char && (k.text == "l" || k.text == "L")) { if (!dl_repo.empty()) list_files(); }
            else if (k.type == Key::Char && (k.text == "c" || k.text == "C")) cancel_download();
            else if (k.type == Key::Enter || (k.type == Key::Char && (k.text == "d" || k.text == "D"))) {
                const json & files = jobj(hf_files, "files");
                if (files.is_array() && sel_file < (int) files.size())
                    start_download(js(hf_files, "repo", dl_repo), js(files[sel_file], "path"));
                else if (!dl_repo.empty()) start_download(dl_repo, "");
            }
            break;
        }
    }
}

void Manager::list_files() {
    flash("listing files…");
    json out; std::string e;
    if (get(cli, "/models/api/hf/files?repo=" + urlenc(dl_repo), out, e)) { hf_files = out; err.clear(); sel_file = top_file = 0; }
    else set_err(e);
}
void Manager::on_tab_enter() {
    info_msg.clear(); err.clear();
    if (tab == TAB_LOCAL && !local_loaded) fetch_local();
    if (tab == TAB_RECOMMEND && !rec_loaded) fetch_recommend();
    if (tab == TAB_DOWNLOADS) fetch_dlstatus(true);
}

int Manager::loop() {
    detect_term();
    g_color = true;
    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) {
        ui::Style st = ui::detect_style();
        return print_status(cli, st, stdout);
    }
    if (!term.enter()) { ui::Style st = ui::detect_style(); return print_status(cli, st, stdout); }
    fetch_status(true);
    while (!quit) {
        cols = term_cols(); rows = term_rows();
        // background refreshes
        if (tab == TAB_STATUS) fetch_status(false);
        if (tab == TAB_DOWNLOADS) fetch_dlstatus(false);
        render();
        int timeout = (tab == TAB_STATUS || tab == TAB_DOWNLOADS) ? 500 : 1000;
        for (auto & k : poll_keys(ip, timeout)) { handle_key(k); if (quit) break; }
    }
    term.leave();
    return 0;
}

}  // namespace

int run(Client & cli, const Options & opt) {
    Manager m(cli, opt);
    return m.loop();
}

}  // namespace easyai::manager
