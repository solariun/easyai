// src/tui.cpp — opencode-style full-screen chat TUI (see easyai/tui.hpp).
//
// Plain ANSI, hand-rolled renderer: we keep a previous-frame row cache,
// repaint only rows that changed, and wrap every paint in CSI ?2026
// synchronized-update brackets so streaming never flickers. All input
// parsing is a forward scanner (no std::regex anywhere in this file —
// terminal bytes are outside input).
#include "easyai/tui.hpp"

#include "easyai/client.hpp"
#include "easyai/plan.hpp"
#include "easyai/tool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace stdfs = std::filesystem;

namespace easyai::tui {
namespace {

long long now_ms() {
    using namespace std::chrono;
    return (long long) duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- theme ----
struct RGB { unsigned char r = 0, g = 0, b = 0; };

constexpr RGB rgb(unsigned v) {
    return RGB{ (unsigned char)((v >> 16) & 0xff),
                (unsigned char)((v >> 8)  & 0xff),
                (unsigned char)( v        & 0xff) };
}

RGB lerp(RGB a, RGB b, float t) {
    if (t < 0) t = 0; if (t > 1) t = 1;
    auto mix = [t](unsigned char x, unsigned char y) {
        return (unsigned char)(x + (y - x) * t + 0.5f);
    };
    return RGB{ mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b) };
}

// Palette role names follow opencode's theme JSON so the mapping stays
// auditable. Hex values are the stock "opencode" theme (MIT).
struct Theme {
    std::string name;
    bool dark = true;
    RGB primary, secondary, accent, error, warning, success, info;
    RGB text, text_muted;
    RGB background, bg_panel, bg_element;
    RGB border, border_active, border_subtle;
    RGB diff_added_bg, diff_removed_bg, diff_ctx_bg;
    RGB diff_line_nr, diff_add_ln_bg, diff_rem_ln_bg;
    RGB diff_add_sign, diff_rem_sign;
    RGB md_heading, md_link, md_link_text, md_code, md_quote;
    RGB md_emph, md_strong, md_hr, md_bullet, md_enum;
};

Theme theme_opencode() {
    Theme t;
    t.name = "opencode"; t.dark = true;
    t.primary    = rgb(0xfab283); t.secondary = rgb(0x5c9cf5);
    t.accent     = rgb(0x9d7cd8); t.error     = rgb(0xe06c75);
    t.warning    = rgb(0xf5a742); t.success   = rgb(0x7fd88f);
    t.info       = rgb(0x56b6c2);
    t.text       = rgb(0xeeeeee); t.text_muted = rgb(0x808080);
    t.background = rgb(0x0a0a0a); t.bg_panel   = rgb(0x141414);
    t.bg_element = rgb(0x1e1e1e);
    t.border = rgb(0x484848); t.border_active = rgb(0x606060);
    t.border_subtle = rgb(0x3c3c3c);
    t.diff_added_bg = rgb(0x20303b); t.diff_removed_bg = rgb(0x37222c);
    t.diff_ctx_bg   = t.bg_panel;
    t.diff_line_nr  = rgb(0x8f8f8f);
    t.diff_add_ln_bg = rgb(0x1b2b34); t.diff_rem_ln_bg = rgb(0x2d1f26);
    t.diff_add_sign  = rgb(0xb8db87); t.diff_rem_sign  = rgb(0xe26a75);
    t.md_heading = t.accent;  t.md_link = t.primary;
    t.md_link_text = t.info;  t.md_code = t.success;
    t.md_quote = rgb(0xe5c07b); t.md_emph = rgb(0xe5c07b);
    t.md_strong = t.warning;  t.md_hr = t.text_muted;
    t.md_bullet = t.primary;  t.md_enum = t.info;
    return t;
}

Theme theme_opencode_light() {
    Theme t = theme_opencode();
    t.name = "opencode-light"; t.dark = false;
    t.primary    = rgb(0x3b7dd8); t.secondary = rgb(0x7b5bb6);
    t.accent     = rgb(0xd68c27); t.error     = rgb(0xd1383d);
    t.warning    = rgb(0xd68c27); t.success   = rgb(0x3d9a57);
    t.info       = rgb(0x318795);
    t.text       = rgb(0x1a1a1a); t.text_muted = rgb(0x8a8a8a);
    t.background = rgb(0xffffff); t.bg_panel   = rgb(0xfafafa);
    t.bg_element = rgb(0xf5f5f5);
    t.border = rgb(0xb8b8b8); t.border_active = rgb(0xa0a0a0);
    t.border_subtle = rgb(0xd4d4d4);
    t.diff_added_bg = rgb(0xd5e5d5); t.diff_removed_bg = rgb(0xf7d8db);
    t.diff_ctx_bg = t.bg_panel;
    t.diff_line_nr = rgb(0x595959);
    t.diff_add_ln_bg = rgb(0xc5d5c5); t.diff_rem_ln_bg = rgb(0xe7c8cb);
    t.diff_add_sign = rgb(0x4db380); t.diff_rem_sign = rgb(0xf52a65);
    t.md_heading = t.accent; t.md_link = t.primary; t.md_link_text = t.info;
    t.md_code = t.success; t.md_quote = rgb(0xb0851f); t.md_emph = rgb(0xb0851f);
    t.md_strong = t.warning; t.md_hr = t.text_muted;
    t.md_bullet = t.primary; t.md_enum = t.info;
    return t;
}

std::vector<std::string> theme_names() { return { "opencode", "opencode-light" }; }
Theme theme_by_name(const std::string & n) {
    if (n == "opencode-light" || n == "light") return theme_opencode_light();
    return theme_opencode();
}

// ------------------------------------------------------------- SGR emit ----
// Truecolor when COLORTERM advertises it; otherwise map to the 6x6x6
// xterm-256 cube so the palette still reads correctly.
bool g_truecolor = true;

int cube_index(RGB c) {
    auto q = [](unsigned char v) {
        return v < 48 ? 0 : v < 115 ? 1 : (v - 35) / 40;
    };
    int r = q(c.r), g = q(c.g), b = q(c.b);
    // grayscale ramp often matches dark UI tones better
    if (std::abs(c.r - c.g) < 12 && std::abs(c.g - c.b) < 12) {
        int gray = (c.r + c.g + c.b) / 3;
        if (gray < 4)   return 16;
        if (gray > 246) return 231;
        return 232 + std::min(23, (gray - 4) / 10);
    }
    return 16 + 36 * r + 6 * g + b;
}

std::string fg(RGB c) {
    char b[32];
    if (g_truecolor)
        std::snprintf(b, sizeof(b), "\033[38;2;%u;%u;%um", c.r, c.g, c.b);
    else
        std::snprintf(b, sizeof(b), "\033[38;5;%dm", cube_index(c));
    return b;
}
std::string bg(RGB c) {
    char b[32];
    if (g_truecolor)
        std::snprintf(b, sizeof(b), "\033[48;2;%u;%u;%um", c.r, c.g, c.b);
    else
        std::snprintf(b, sizeof(b), "\033[48;5;%dm", cube_index(c));
    return b;
}
constexpr const char * RESET = "\033[0m";
constexpr const char * BOLD  = "\033[1m";
constexpr const char * ITAL  = "\033[3m";
constexpr const char * UNDER = "\033[4m";
constexpr const char * STRIKE= "\033[9m";

// ----------------------------------------------------------- utf8/width ----
size_t u8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xe) return 3;
    if ((c >> 3) == 0x1e) return 4;
    return 1;  // invalid byte — treat as one column of mojibake
}

char32_t u8_cp(const std::string & s, size_t i, size_t * adv) {
    unsigned char c = (unsigned char) s[i];
    size_t n = u8_len(c);
    if (i + n > s.size()) n = 1;
    *adv = n;
    switch (n) {
        case 2: return ((c & 0x1f) << 6)  | (s[i+1] & 0x3f);
        case 3: return ((c & 0x0f) << 12) | ((s[i+1] & 0x3f) << 6)
                                          | (s[i+2] & 0x3f);
        case 4: return ((c & 0x07) << 18) | ((s[i+1] & 0x3f) << 12)
                     | ((s[i+2] & 0x3f) << 6) | (s[i+3] & 0x3f);
        default: return c;
    }
}

// Minimal wcwidth: combining marks → 0, common CJK/emoji blocks → 2.
int cp_width(char32_t u) {
    if (u == 0) return 0;
    if (u < 32 || (u >= 0x7f && u < 0xa0)) return 1;  // never emitted
    if ((u >= 0x0300 && u <= 0x036f) || (u >= 0x1ab0 && u <= 0x1aff) ||
        (u >= 0x20d0 && u <= 0x20ff) || (u >= 0xfe00 && u <= 0xfe0f))
        return 0;
    if ((u >= 0x1100 && u <= 0x115f) || (u >= 0x2e80 && u <= 0xa4cf) ||
        (u >= 0xac00 && u <= 0xd7a3) || (u >= 0xf900 && u <= 0xfaff) ||
        (u >= 0xfe30 && u <= 0xfe4f) || (u >= 0xff00 && u <= 0xff60) ||
        (u >= 0xffe0 && u <= 0xffe6) ||
        (u >= 0x1f300 && u <= 0x1f9ff) || (u >= 0x20000 && u <= 0x3fffd))
        return 2;
    return 1;
}

int disp_width(const std::string & s) {
    int w = 0;
    for (size_t i = 0; i < s.size(); ) {
        size_t adv; char32_t u = u8_cp(s, i, &adv);
        w += cp_width(u); i += adv;
    }
    return w;
}

// Cut `s` to at most `maxw` columns; appends "…" when cut.
std::string clip_w(const std::string & s, int maxw) {
    if (maxw <= 0) return "";
    int w = 0; size_t i = 0;
    for (; i < s.size(); ) {
        size_t adv; char32_t u = u8_cp(s, i, &adv);
        int cw = cp_width(u);
        if (w + cw > maxw - 1) {
            if (disp_width(s) <= maxw) return s;  // it fits whole
            return s.substr(0, i) + "…";
        }
        w += cw; i += adv;
    }
    return s;
}

// Word-wrap plain text (no ANSI inside) to `width` columns.
std::vector<std::string> wrap_text(const std::string & text, int width) {
    std::vector<std::string> out;
    if (width < 4) width = 4;
    size_t line_start = 0;
    auto flush_hard = [&](size_t from, size_t to) {  // one logical line
        // greedy wrap on spaces; fall back to hard cuts for long runs
        size_t i = from;
        while (i < to) {
            int w = 0; size_t last_sp = std::string::npos; size_t j = i;
            while (j < to) {
                size_t adv; char32_t u = u8_cp(text, j, &adv);
                int cw = cp_width(u);
                if (w + cw > width) break;
                if (u == U' ') last_sp = j;
                w += cw; j += adv;
            }
            if (j >= to) { out.push_back(text.substr(i, to - i)); break; }
            size_t cut = (last_sp != std::string::npos && last_sp > i)
                             ? last_sp : j;
            out.push_back(text.substr(i, cut - i));
            i = (cut == last_sp) ? cut + 1 : cut;
        }
        if (from == to) out.push_back("");
    };
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            flush_hard(line_start, i);
            line_start = i + 1;
        }
    }
    if (out.empty()) out.push_back("");
    return out;
}

// Pad an already-styled row string to exactly `width` columns of
// visible content. `vis` is the precomputed visible width.
void pad_row(std::string & row, int vis, int width, const std::string & bgc) {
    if (vis >= width) return;
    if (!bgc.empty()) row += bgc;
    row.append((size_t)(width - vis), ' ');
}

// ------------------------------------------------------- transcript model --
enum class ToolState { Running, Done, Error };

struct ToolCell {
    std::string name;          // canonical tool name
    std::string args;          // raw arguments_json
    ToolState   state = ToolState::Running;
    std::string result;        // model-facing content (preview source)
    bool        denied = false;
    long long   t0 = 0, t1 = 0;
    bool        expanded = false;
};

enum class PartKind { Text, Reasoning, Tool };

struct Part {
    PartKind  kind;
    std::string text;          // Text / Reasoning payload
    ToolCell  tool;            // Tool payload
    long long t0 = 0, t1 = 0;  // reasoning duration tracking
};

struct Message {
    enum Role { User, Assistant, Info } role = User;
    std::vector<Part> parts;
    std::string model;         // assistant footer
    long long  t_created = 0, t_done = 0;
    bool       interrupted = false;
    bool       done = false;
    std::string error;         // assistant-level error banner
    bool       queued = false; // user message waiting its turn
    uint64_t   rev = 1;        // bump on any mutation (render cache key)
};

// ---------------------------------------------------------------- dialogs --
struct DialogItem {
    std::string title, desc, value;
    bool        current = false;
};
struct Dialog {
    std::string title;
    std::vector<DialogItem> items;
    std::string filter;
    int  sel = 0;
    std::function<void(const DialogItem &)> on_pick;
};

struct Toast {
    enum Variant { Info, Success, Warning, Error } variant = Info;
    std::string title, text;
    long long until = 0;
};

// Completion popup (slash commands / @ files).
struct Completion {
    bool active = false;
    char kind = 0;             // '/' or '@'
    size_t anchor = 0;         // byte index of trigger char in input
    std::vector<DialogItem> all, view;
    int sel = 0;
};

// Modal question state (question-tool bridge).
struct QuestionUi {
    bool active = false;
    std::vector<QuestionItem> items;
    size_t idx = 0;
    std::vector<std::vector<std::string>> answers;
    std::set<int> chosen;      // for multi-select
    int  sel = 0;
    bool custom_mode = false;
    std::string custom;
    bool finished = false, dismissed = false;
};

struct SlashCmd {
    std::string name, desc;
};

// -------------------------------------------------------------- UI state ---
struct Ui {
    // immutable-ish config
    Theme theme;
    Options opt;
    Hooks hooks;
    Client * cli = nullptr;
    Plan   * plan = nullptr;
    std::vector<SlashCmd> commands;

    // terminal
    int rows = 24, cols = 80;
    termios saved_tio{};
    bool raw = false;

    // shared model (mutex-guarded; ver bumps trigger repaint)
    std::mutex mu;
    std::condition_variable cv;          // question bridge wakeups
    std::vector<Message> msgs;
    std::deque<std::string> queue;       // prompts queued while busy
    bool busy = false;
    bool compacting = false;
    std::string spin_label;              // "Thinking" / tool pending label
    int  thinking_pct = -1;
    int  ctx_pct = -1, ctx_used = -1, ctx_total = -1;
    double tps = 0.0;
    long long tok_t0 = 0; int tok_n = 0;  // t/s window
    std::atomic<uint64_t> ver{1};

    // editor
    std::string input;
    size_t cur = 0;
    std::vector<std::string> history;
    int hist_pos = -1;
    std::string hist_stash;

    // view state
    int  scroll = 0;                     // lines above the bottom
    bool expand_tools = false;
    bool expand_thinking = false;
    int  spin_frame = 0;
    int  esc_hits = 0;  long long esc_t = 0;
    int  ctrlc_hits = 0; long long ctrlc_t = 0;
    int  placeholder_idx = 0;

    // overlays
    std::optional<Dialog> dialog;
    Completion comp;
    QuestionUi q;
    std::vector<Toast> toasts;

    // worker
    std::thread worker;
    std::atomic<bool> worker_live{false};

    // paint cache
    std::vector<std::string> frame_prev;
    bool force_full = true;

    // chat render cache: one entry per message, keyed by rev/width/toggles
    struct MsgCache { uint64_t key = 0; std::vector<std::string> lines; };
    std::vector<MsgCache> cache;

    uint64_t bump() { return ver.fetch_add(1) + 1; }
};

Ui * g_ui = nullptr;                      // question bridge + SIGWINCH
std::atomic<bool> g_resized{false};
void on_winch(int) { g_resized.store(true); }

// --------------------------------------------------------- terminal ctl ----
void term_write(const std::string & s) {
    ::fwrite(s.data(), 1, s.size(), stdout);
}

bool enter_terminal(Ui & ui) {
    if (::tcgetattr(STDIN_FILENO, &ui.saved_tio) != 0) return false;
    termios t = ui.saved_tio;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_iflag &= ~(IXON | ICRNL);
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) return false;
    ui.raw = true;
    // alt screen + bracketed paste + kitty keyboard (disambiguate) +
    // SGR mouse reporting (wheel scroll). Unsupported terminals ignore
    // what they don't speak.
    term_write("\033[?1049h\033[?2004h\033[>1u\033[?1000h\033[?1006h");
    term_write("\033]0;easyai\007");
    ::fflush(stdout);
    return true;
}

void leave_terminal(Ui & ui) {
    if (!ui.raw) return;
    term_write("\033[?1006l\033[?1000l\033[<u\033[?2004l\033[?2026l");
    term_write("\033[?25h\033[?1049l");
    ::fflush(stdout);
    ::tcsetattr(STDIN_FILENO, TCSANOW, &ui.saved_tio);
    ui.raw = false;
}

void term_size(Ui & ui) {
    winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        ui.cols = ws.ws_col; ui.rows = ws.ws_row;
    }
    if (ui.cols < 20) ui.cols = 20;
    if (ui.rows < 8)  ui.rows = 8;
}

// ------------------------------------------------------------ key events ---
struct Key {
    enum Type {
        None, Char, Enter, ShiftEnter, Newline, Backspace, Delete,
        Up, Down, Left, Right, Home, End, PgUp, PgDn, Tab, ShiftTab,
        Esc, CtrlA, CtrlC, CtrlD, CtrlE, CtrlK, CtrlL, CtrlO, CtrlP,
        CtrlR, CtrlU, CtrlW, WheelUp, WheelDown, Paste, WordLeft, WordRight,
    } type = None;
    std::string text;      // Char (one cp) / Paste payload
};

// Stateful byte → Key parser. Buffers escape sequences across reads.
struct InputParser {
    std::string buf;
    bool in_paste = false;
    std::string paste;

    // Returns parsed keys; leaves incomplete tails in buf.
    std::vector<Key> feed(const char * data, size_t n) {
        buf.append(data, n);
        std::vector<Key> out;
        size_t i = 0;
        auto emit = [&](Key::Type t, std::string s = "") {
            out.push_back(Key{ t, std::move(s) });
        };
        while (i < buf.size()) {
            unsigned char c = (unsigned char) buf[i];
            if (in_paste) {
                // consume until ESC[201~
                size_t end = buf.find("\033[201~", i);
                if (end == std::string::npos) {
                    paste.append(buf, i, buf.size() - i);
                    i = buf.size();
                    break;
                }
                paste.append(buf, i, end - i);
                i = end + 6;
                in_paste = false;
                // normalize CRLF / CR → LF at the boundary
                std::string norm; norm.reserve(paste.size());
                for (size_t k = 0; k < paste.size(); ++k) {
                    if (paste[k] == '\r') {
                        norm.push_back('\n');
                        if (k + 1 < paste.size() && paste[k+1] == '\n') ++k;
                    } else norm.push_back(paste[k]);
                }
                emit(Key::Paste, norm);
                paste.clear();
                continue;
            }
            if (c == 0x1b) {
                if (i + 1 >= buf.size()) break;  // maybe more coming
                unsigned char c1 = (unsigned char) buf[i + 1];
                if (c1 == '[' || c1 == 'O') {
                    // CSI / SS3 — find final byte (0x40..0x7e)
                    size_t j = i + 2;
                    while (j < buf.size()
                           && !((unsigned char) buf[j] >= 0x40
                                && (unsigned char) buf[j] <= 0x7e)) ++j;
                    if (j >= buf.size()) break;  // incomplete
                    std::string seq = buf.substr(i + 2, j - (i + 2));
                    char fin = buf[j];
                    i = j + 1;
                    parse_csi(c1, seq, fin, emit);
                    continue;
                }
                if (c1 == 0x0d) { emit(Key::Newline); i += 2; continue; } // alt+enter
                // lone ESC (or alt+<key> — treat as ESC then key)
                emit(Key::Esc);
                ++i;
                continue;
            }
            if (c == 0x0d) { emit(Key::Enter);     ++i; continue; }
            if (c == 0x0a) { emit(Key::Newline);   ++i; continue; }
            if (c == 0x7f || c == 0x08) { emit(Key::Backspace); ++i; continue; }
            if (c == 0x09) { emit(Key::Tab);       ++i; continue; }
            if (c == 0x01) { emit(Key::CtrlA);     ++i; continue; }
            if (c == 0x03) { emit(Key::CtrlC);     ++i; continue; }
            if (c == 0x04) { emit(Key::CtrlD);     ++i; continue; }
            if (c == 0x05) { emit(Key::CtrlE);     ++i; continue; }
            if (c == 0x0b) { emit(Key::CtrlK);     ++i; continue; }
            if (c == 0x0c) { emit(Key::CtrlL);     ++i; continue; }
            if (c == 0x0f) { emit(Key::CtrlO);     ++i; continue; }
            if (c == 0x10) { emit(Key::CtrlP);     ++i; continue; }
            if (c == 0x12) { emit(Key::CtrlR);     ++i; continue; }
            if (c == 0x15) { emit(Key::CtrlU);     ++i; continue; }
            if (c == 0x17) { emit(Key::CtrlW);     ++i; continue; }
            if (c < 0x20)  { ++i; continue; }      // other ctrl — ignore
            size_t adv = u8_len(c);
            if (i + adv > buf.size()) break;       // partial utf-8
            emit(Key::Char, buf.substr(i, adv));
            i += adv;
        }
        buf.erase(0, i);
        return out;
    }

    template <class Emit>
    void parse_csi(unsigned char intro, const std::string & seq, char fin,
                   Emit emit) {
        auto field = [&](int idx, long def) -> long {
            long v = def; int f = 0; long acc = -1;
            for (char ch : seq) {
                if (ch == ';') { if (f == idx && acc >= 0) return acc;
                                 ++f; acc = -1; continue; }
                if (ch >= '0' && ch <= '9')
                    acc = (acc < 0 ? 0 : acc) * 10 + (ch - '0');
            }
            if (f == idx && acc >= 0) return acc;
            return v;
        };
        if (intro == 'O') {  // SS3: arrows/home/end in application mode
            switch (fin) {
                case 'A': emit(Key::Up); return;
                case 'B': emit(Key::Down); return;
                case 'C': emit(Key::Right); return;
                case 'D': emit(Key::Left); return;
                case 'H': emit(Key::Home); return;
                case 'F': emit(Key::End); return;
            }
            return;
        }
        const long p0 = field(0, 1), p1 = field(1, 1);
        switch (fin) {
            case 'A': emit(Key::Up); return;
            case 'B': emit(Key::Down); return;
            case 'C': emit(p1 >= 3 ? Key::WordRight : Key::Right); return;
            case 'D': emit(p1 >= 3 ? Key::WordLeft  : Key::Left);  return;
            case 'H': emit(Key::Home); return;
            case 'F': emit(Key::End); return;
            case 'Z': emit(Key::ShiftTab); return;
            case '~':
                switch (p0) {
                    case 1: case 7: emit(Key::Home); return;
                    case 4: case 8: emit(Key::End);  return;
                    case 3: emit(Key::Delete); return;
                    case 5: emit(Key::PgUp);  return;
                    case 6: emit(Key::PgDn);  return;
                    case 200: in_paste = true; paste.clear(); return;
                    case 27: {  // modifyOtherKeys: 27;mod;key~
                        long mod = field(1, 1), key = field(2, 0);
                        if (key == 13) {
                            emit(mod >= 2 ? Key::ShiftEnter : Key::Enter);
                        }
                        return;
                    }
                }
                return;
            case 'u': {  // kitty CSI-u: key;mods u
                long key = p0, mod = p1;
                if (key == 13) {
                    emit(mod >= 2 ? Key::ShiftEnter : Key::Enter);
                    return;
                }
                if (key == 27) { emit(Key::Esc); return; }
                if (key == 9)  { emit(mod >= 2 ? Key::ShiftTab : Key::Tab); return; }
                if (key == 127 || key == 8) { emit(Key::Backspace); return; }
                if (key >= 32 && mod <= 1) {
                    // plain printable delivered via CSI-u
                    std::string s;
                    char32_t u = (char32_t) key;
                    if (u < 0x80) s.push_back((char) u);
                    else if (u < 0x800) {
                        s.push_back((char)(0xc0 | (u >> 6)));
                        s.push_back((char)(0x80 | (u & 0x3f)));
                    } else if (u < 0x10000) {
                        s.push_back((char)(0xe0 | (u >> 12)));
                        s.push_back((char)(0x80 | ((u >> 6) & 0x3f)));
                        s.push_back((char)(0x80 | (u & 0x3f)));
                    } else {
                        s.push_back((char)(0xf0 | (u >> 18)));
                        s.push_back((char)(0x80 | ((u >> 12) & 0x3f)));
                        s.push_back((char)(0x80 | ((u >> 6) & 0x3f)));
                        s.push_back((char)(0x80 | (u & 0x3f)));
                    }
                    emit(Key::Char, s);
                }
                if (key >= 'a' && key <= 'z' && mod == 5) {  // ctrl+<letter>
                    switch (key) {
                        case 'a': emit(Key::CtrlA); return;
                        case 'c': emit(Key::CtrlC); return;
                        case 'd': emit(Key::CtrlD); return;
                        case 'e': emit(Key::CtrlE); return;
                        case 'k': emit(Key::CtrlK); return;
                        case 'l': emit(Key::CtrlL); return;
                        case 'o': emit(Key::CtrlO); return;
                        case 'p': emit(Key::CtrlP); return;
                        case 'r': emit(Key::CtrlR); return;
                        case 'u': emit(Key::CtrlU); return;
                        case 'w': emit(Key::CtrlW); return;
                    }
                }
                return;
            }
            case 'M': case 'm': {  // SGR mouse: btn;x;y
                if (seq.empty() || seq[0] != '<') return;
                // re-parse skipping '<'
                long btn = -1, acc = -1; int f = 0;
                for (size_t k = 1; k < seq.size(); ++k) {
                    char ch = seq[k];
                    if (ch == ';') { if (f == 0) btn = acc; ++f; acc = -1; continue; }
                    if (ch >= '0' && ch <= '9')
                        acc = (acc < 0 ? 0 : acc) * 10 + (ch - '0');
                }
                if (btn == 64) emit(Key::WheelUp);
                if (btn == 65) emit(Key::WheelDown);
                return;
            }
        }
    }
};

// ---------------------------------------------------------- markdown -------
// Small forward-only renderer: headings, fenced code, lists, quotes,
// hr, inline `code` / **bold** / *italic* / [text](url). Each output
// line is a fully styled row fragment (no trailing reset needed by the
// caller). Width-aware wrapping happens here so styles survive wraps.
struct MdCtx {
    const Theme & th;
    int width;
    std::string base_fg;
};

std::string md_inline(const MdCtx & cx, const std::string & s) {
    std::string out;
    out.reserve(s.size() + 16);
    size_t i = 0;
    auto starts = [&](const char * pat, size_t at) {
        size_t n = std::strlen(pat);
        return s.compare(at, n, pat) == 0;
    };
    while (i < s.size()) {
        if (s[i] == '`') {
            size_t end = s.find('`', i + 1);
            if (end != std::string::npos) {
                out += fg(cx.th.md_code);
                out.append(s, i + 1, end - i - 1);
                out += RESET; out += cx.base_fg;
                i = end + 1;
                continue;
            }
        }
        if (starts("**", i)) {
            size_t end = s.find("**", i + 2);
            if (end != std::string::npos) {
                out += fg(cx.th.md_strong); out += BOLD;
                out += md_inline(cx, s.substr(i + 2, end - i - 2));
                out += RESET; out += cx.base_fg;
                i = end + 2;
                continue;
            }
        }
        if (s[i] == '*' && i + 1 < s.size() && s[i+1] != ' ' && s[i+1] != '*') {
            size_t end = s.find('*', i + 1);
            if (end != std::string::npos) {
                out += fg(cx.th.md_emph); out += ITAL;
                out += md_inline(cx, s.substr(i + 1, end - i - 1));
                out += RESET; out += cx.base_fg;
                i = end + 1;
                continue;
            }
        }
        if (s[i] == '[') {
            size_t mid = s.find("](", i + 1);
            size_t end = (mid == std::string::npos)
                             ? std::string::npos : s.find(')', mid + 2);
            if (mid != std::string::npos && end != std::string::npos) {
                std::string label = s.substr(i + 1, mid - i - 1);
                std::string url   = s.substr(mid + 2, end - mid - 2);
                out += fg(cx.th.md_link_text); out += UNDER;
                out += label;
                out += RESET; out += cx.base_fg;
                if (!url.empty() && url != label) {
                    out += fg(cx.th.text_muted);
                    out += " (" + url + ")";
                    out += RESET; out += cx.base_fg;
                }
                i = end + 1;
                continue;
            }
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

// Render markdown to rows. Every row is prefixed with `indent` spaces
// and styled; rows do NOT include bg fill (caller pads).
std::vector<std::string> render_markdown(const Theme & th, int width,
                                         int indent,
                                         const std::string & text) {
    std::vector<std::string> rows;
    const std::string pad((size_t) indent, ' ');
    const int w = std::max(8, width - indent);
    MdCtx cx{ th, w, fg(th.text) };

    bool in_code = false;
    std::string code_lang;
    std::istringstream ss(text);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    if (!text.empty() && text.back() == '\n') lines.push_back("");
    if (lines.empty()) lines.push_back("");

    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string & ln = lines[li];
        std::string t = ln;
        // fences
        size_t ns = t.find_first_not_of(' ');
        if (ns != std::string::npos && t.compare(ns, 3, "```") == 0) {
            if (!in_code) { in_code = true; code_lang = t.substr(ns + 3); }
            else          { in_code = false; }
            continue;
        }
        if (in_code) {
            // panel-bg code row, preserved verbatim (clipped, not wrapped)
            std::string row = pad + bg(th.bg_element) + fg(th.text) + " ";
            std::string body = clip_w(t, w - 2);
            row += body;
            int vis = 1 + disp_width(body);
            pad_row(row, vis + indent, indent + w, bg(th.bg_element));
            row += RESET;
            rows.push_back(row);
            continue;
        }
        if (ns == std::string::npos) { rows.push_back(""); continue; }

        // headings
        int hl = 0; while (ns + (size_t) hl < t.size() && t[ns + hl] == '#') ++hl;
        if (hl >= 1 && hl <= 6 && ns + hl < t.size() && t[ns + hl] == ' ') {
            std::string body = t.substr(ns + hl + 1);
            for (auto & wln : wrap_text(body, w)) {
                std::string row = pad;
                row += fg(th.md_heading); row += BOLD;
                if (hl == 1) row += UNDER;
                row += md_inline(cx, wln);
                row += RESET;
                rows.push_back(row);
            }
            continue;
        }
        // hr
        if (t.compare(ns, 3, "---") == 0 || t.compare(ns, 3, "***") == 0) {
            std::string row = pad + fg(th.md_hr);
            for (int k = 0; k < w; ++k) row += "─";
            row += RESET;
            rows.push_back(row);
            continue;
        }
        // blockquote
        if (t[ns] == '>') {
            std::string body = t.substr(ns + 1);
            if (!body.empty() && body[0] == ' ') body.erase(0, 1);
            for (auto & wln : wrap_text(body, w - 2)) {
                std::string row = pad;
                row += fg(th.md_quote); row += "▌ "; row += ITAL;
                row += md_inline({ th, w, fg(th.md_quote) }, wln);
                row += RESET;
                rows.push_back(row);
            }
            continue;
        }
        // bullets
        std::string lead(t, 0, ns);
        if ((t[ns] == '-' || t[ns] == '*' || t[ns] == '+')
            && ns + 1 < t.size() && t[ns + 1] == ' ') {
            std::string body = t.substr(ns + 2);
            auto wls = wrap_text(body, w - (int) ns - 2);
            for (size_t k = 0; k < wls.size(); ++k) {
                std::string row = pad + lead;
                if (k == 0) { row += fg(th.md_bullet); row += "- "; }
                else        row += "  ";
                row += fg(th.text);
                row += md_inline(cx, wls[k]);
                row += RESET;
                rows.push_back(row);
            }
            continue;
        }
        // ordered list "N. "
        {
            size_t d = ns;
            while (d < t.size() && t[d] >= '0' && t[d] <= '9') ++d;
            if (d > ns && d + 1 < t.size() && t[d] == '.' && t[d+1] == ' ') {
                std::string num = t.substr(ns, d - ns + 1);
                std::string body = t.substr(d + 2);
                auto wls = wrap_text(body, w - (int) ns - (int) num.size() - 1);
                for (size_t k = 0; k < wls.size(); ++k) {
                    std::string row = pad + lead;
                    if (k == 0) { row += fg(th.md_enum); row += num; row += " "; }
                    else row += std::string(num.size() + 1, ' ');
                    row += fg(th.text);
                    row += md_inline(cx, wls[k]);
                    row += RESET;
                    rows.push_back(row);
                }
                continue;
            }
        }
        // paragraph
        for (auto & wln : wrap_text(t, w)) {
            std::string row = pad;
            row += fg(th.text);
            row += md_inline(cx, wln);
            row += RESET;
            rows.push_back(row);
        }
    }
    // trim trailing blank rows
    while (!rows.empty() && rows.back().empty()) rows.pop_back();
    return rows;
}

// ----------------------------------------------------------- line diff -----
// Myers-lite: LCS DP capped at 200x200 lines; beyond that fall back to
// "all removed / all added" which is what big rewrites look like anyway.
struct DiffRow { char tag; std::string text; int ln_old, ln_new; };

std::vector<DiffRow> diff_lines(const std::string & a, const std::string & b) {
    auto split = [](const std::string & s) {
        std::vector<std::string> v; size_t st = 0;
        for (size_t i = 0; i <= s.size(); ++i)
            if (i == s.size() || s[i] == '\n') {
                v.push_back(s.substr(st, i - st));
                st = i + 1;
            }
        if (!v.empty() && v.back().empty() && !s.empty() && s.back() == '\n')
            v.pop_back();
        return v;
    };
    auto A = split(a), B = split(b);
    std::vector<DiffRow> out;
    const size_t n = A.size(), m = B.size();
    if (n > 200 || m > 200 || n * m > 40000) {
        int lo = 1, ln = 1;
        for (auto & s : A) out.push_back({ '-', s, lo++, 0 });
        for (auto & s : B) out.push_back({ '+', s, 0, ln++ });
        return out;
    }
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = n; i-- > 0; )
        for (size_t j = m; j-- > 0; )
            dp[i][j] = (A[i] == B[j]) ? dp[i+1][j+1] + 1
                                      : std::max(dp[i+1][j], dp[i][j+1]);
    size_t i = 0, j = 0; int lo = 1, ln = 1;
    while (i < n && j < m) {
        if (A[i] == B[j]) { out.push_back({ ' ', A[i], lo++, ln++ }); ++i; ++j; }
        else if (dp[i+1][j] >= dp[i][j+1])
            { out.push_back({ '-', A[i], lo++, 0 }); ++i; }
        else
            { out.push_back({ '+', B[j], 0, ln++ }); ++j; }
    }
    while (i < n) { out.push_back({ '-', A[i], lo++, 0 }); ++i; }
    while (j < m) { out.push_back({ '+', B[j], 0, ln++ }); ++j; }
    return out;
}

// Render a unified diff block (opencode's stacked view): line-number
// gutter on tinted bg, sign column, full-row added/removed tints.
std::vector<std::string> render_diff(const Theme & th, int width,
                                     const std::string & old_s,
                                     const std::string & new_s) {
    std::vector<std::string> rows;
    auto d = diff_lines(old_s, new_s);
    // context squeeze: keep 2 lines around changes
    std::vector<char> keep(d.size(), 0);
    for (size_t i = 0; i < d.size(); ++i)
        if (d[i].tag != ' ')
            for (int k = -2; k <= 2; ++k) {
                long j = (long) i + k;
                if (j >= 0 && j < (long) d.size()) keep[(size_t) j] = 1;
            }
    const int gut = 4;
    const int body_w = std::max(8, width - gut - 2);
    bool gap = false;
    for (size_t i = 0; i < d.size(); ++i) {
        if (!keep[i]) { gap = true; continue; }
        if (gap && !rows.empty()) {
            std::string row = bg(th.diff_ctx_bg) + fg(th.text_muted) + " ⋯";
            pad_row(row, 2, width, bg(th.diff_ctx_bg));
            row += RESET;
            rows.push_back(row);
            gap = false;
        }
        const auto & r = d[i];
        RGB row_bg  = r.tag == '+' ? th.diff_added_bg
                    : r.tag == '-' ? th.diff_removed_bg : th.diff_ctx_bg;
        RGB gut_bg  = r.tag == '+' ? th.diff_add_ln_bg
                    : r.tag == '-' ? th.diff_rem_ln_bg : th.diff_ctx_bg;
        RGB sign_fg = r.tag == '+' ? th.diff_add_sign
                    : r.tag == '-' ? th.diff_rem_sign : th.text_muted;
        char nb[8];
        int ln = r.tag == '-' ? r.ln_old : r.ln_new;
        if (ln > 0) std::snprintf(nb, sizeof(nb), "%4d", ln);
        else        std::snprintf(nb, sizeof(nb), "    ");
        std::string row = bg(gut_bg) + fg(th.diff_line_nr) + nb;
        row += bg(row_bg);
        row += fg(sign_fg);
        row += (r.tag == '+') ? " +" : (r.tag == '-') ? " -" : "  ";
        row += fg(th.text);
        std::string body = clip_w(r.text, body_w);
        row += body;
        pad_row(row, gut + 2 + disp_width(body), width, bg(row_bg));
        row += RESET;
        rows.push_back(row);
    }
    if (rows.empty()) {
        std::string row = bg(th.diff_ctx_bg) + fg(th.text_muted)
                        + " (no changes)";
        pad_row(row, 13, width, bg(th.diff_ctx_bg));
        row += RESET;
        rows.push_back(row);
    }
    return rows;
}

// ------------------------------------------------------------- fuzzy -------
// Subsequence scorer: consecutive hits and word starts score higher;
// returns <0 when `pat` is not a subsequence of `s`.
int fuzzy_score(const std::string & pat, const std::string & s) {
    if (pat.empty()) return 0;
    int score = 0, streak = 0;
    size_t si = 0;
    for (char pc : pat) {
        char pl = (char) std::tolower((unsigned char) pc);
        bool found = false;
        while (si < s.size()) {
            char sl = (char) std::tolower((unsigned char) s[si]);
            if (sl == pl) {
                bool word_start = si == 0 || s[si-1] == '/' || s[si-1] == '_'
                               || s[si-1] == '-' || s[si-1] == ' '
                               || s[si-1] == '.';
                score += 1 + streak * 2 + (word_start ? 4 : 0);
                ++streak; ++si; found = true;
                break;
            }
            streak = 0;
            ++si;
        }
        if (!found) return -1;
    }
    // shorter targets win ties
    return score * 100 - (int) std::min<size_t>(s.size(), 9999);
}

// ----------------------------------------------------------- file index ----
// @-completion source: walk cwd, skip bulky/VCS dirs, cap entries.
std::vector<std::string> scan_files(const std::string & root, size_t cap) {
    static const char * kSkip[] = {
        ".git", "node_modules", "build", "dist", ".cache", "target",
        "__pycache__", ".venv", "venv", ".idea", ".vscode",
    };
    std::vector<std::string> out;
    std::error_code ec;
    stdfs::recursive_directory_iterator it(
        root, stdfs::directory_options::skip_permission_denied, ec);
    if (ec) return out;
    const stdfs::recursive_directory_iterator end;
    while (it != end) {
        std::error_code q;
        const auto & p = it->path();
        std::string base = p.filename().string();
        bool skip = false;
        for (auto * s : kSkip) if (base == s) { skip = true; break; }
        if (skip && it->is_directory(q)) { it.disable_recursion_pending(); }
        else if (!skip && it->is_regular_file(q)) {
            std::string rel = stdfs::relative(p, root, q).generic_string();
            if (!q && !rel.empty()) out.push_back(rel);
            if (out.size() >= cap) break;
        }
        it.increment(q);
        if (q) break;
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ------------------------------------------------------------ rendering ----
constexpr const char * kSpin[] = { "⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏" };
constexpr int kSpinN = 10;
constexpr int kMargin = 2;   // scroll-area left padding (opencode: 2)
constexpr int kIndent = 3;   // message-part padding (opencode: 3)

const char * kPlaceholders[] = {
    "fix the failing test in src/",
    "explain this codebase",
    "add error handling to the parser",
    "what changed in the last 5 commits?",
    "write a README for this project",
};

std::string spin_glyph(const Ui & ui) { return kSpin[ui.spin_frame % kSpinN]; }

std::string short_dur(long long ms) {
    char b[32];
    if (ms < 1000) { std::snprintf(b, sizeof(b), "%lldms", ms); return b; }
    if (ms < 60'000) { std::snprintf(b, sizeof(b), "%.1fs", ms / 1000.0); return b; }
    std::snprintf(b, sizeof(b), "%lldm%llds", ms / 60000, (ms % 60000) / 1000);
    return b;
}

// First line of a string, clipped.
std::string first_line(const std::string & s, int maxw) {
    size_t e = s.find('\n');
    return clip_w(e == std::string::npos ? s : s.substr(0, e), maxw);
}

std::string args_suffix(const std::string & json,
                        std::initializer_list<const char *> keys) {
    std::string out;
    for (auto * k : keys) {
        std::string v;
        long long n;
        bool b;
        if (easyai::args::get_string(json, k, v)) {
            if (!out.empty()) out += ", ";
            out += std::string(k) + "=" + v;
        } else if (easyai::args::get_int(json, k, n)) {
            if (!out.empty()) out += ", ";
            out += std::string(k) + "=" + std::to_string(n);
        } else if (easyai::args::get_bool(json, k, b)) {
            if (!out.empty()) out += ", ";
            out += std::string(k) + "=" + (b ? "true" : "false");
        }
    }
    return out.empty() ? out : "[" + out + "]";
}

// One inline tool row (+ optional error lines). opencode shape:
// 3-space pad, 2-col icon, body; muted when finished, red on error.
void inline_tool_rows(std::vector<std::string> & rows, const Ui & ui,
                      const ToolCell & tc, const std::string & icon,
                      const std::string & body) {
    const Theme & th = ui.theme;
    std::string row(kMargin + kIndent, ' ');
    RGB c = tc.state == ToolState::Error ? th.error
          : tc.state == ToolState::Running ? th.text : th.text_muted;
    row += fg(c);
    if (tc.denied) row += STRIKE;
    if (tc.state == ToolState::Running) row += spin_glyph(ui) + " ";
    else row += icon + " ";
    int w = ui.cols - kMargin - kIndent - 2;
    row += clip_w(body, std::max(8, w));
    row += RESET;
    rows.push_back(row);
    if (tc.state == ToolState::Error && !tc.result.empty()) {
        for (auto & l : wrap_text(first_line(tc.result, 4000),
                                  std::max(8, ui.cols - kMargin - kIndent - 2))) {
            std::string er(kMargin + kIndent + 2, ' ');
            er += fg(th.error) + l + RESET;
            rows.push_back(er);
        }
    }
}

// Panel block (opencode BlockTool): full-width tinted panel with a
// muted "# title" row and body rows, indented under the chat margin.
void block_rows(std::vector<std::string> & rows, const Ui & ui,
                const std::string & title,
                const std::vector<std::string> & body_plain,
                const std::vector<std::string> & body_styled = {}) {
    const Theme & th = ui.theme;
    const int x = kMargin + 1;
    const int w = ui.cols - x - 1;
    auto panel_row = [&](const std::string & styled, int vis) {
        std::string r(x, ' ');
        r += bg(th.bg_panel) + styled;
        pad_row(r, vis, w, bg(th.bg_panel));
        r += RESET;
        rows.push_back(r);
    };
    panel_row("", 0);
    {
        std::string t = clip_w(title, w - 4);
        panel_row("   " + fg(th.text_muted) + t, 3 + disp_width(t));
    }
    panel_row("", 0);
    for (auto & l : body_plain) {
        std::string b = clip_w(l, w - 3);
        panel_row("  " + fg(th.text) + b, 2 + disp_width(b));
    }
    for (auto & l : body_styled) {
        // pre-styled rows are exactly w-2 wide already
        std::string r(x, ' ');
        r += bg(th.bg_panel) + "  " + l;
        rows.push_back(r);
    }
    panel_row("", 0);
}

std::string tool_path_arg(const std::string & json) {
    std::string p;
    if (!easyai::args::get_string(json, "path", p))
        easyai::args::get_string(json, "filePath", p);
    return p.empty() ? "." : p;
}

int count_lines(const std::string & s) {
    if (s.empty()) return 0;
    int n = 1;
    for (char c : s) if (c == '\n') ++n;
    return n;
}

// Render one tool cell — the heart of opencode tool look & feel.
void render_tool(std::vector<std::string> & rows, const Ui & ui, Message & m,
                 ToolCell & tc) {
    const Theme & th = ui.theme;
    const std::string name = easyai::canonical_tool_name(tc.name);
    const std::string & a = tc.args;
    const int bw = ui.cols - kMargin - 6;
    auto S = [](const std::string & j, const char * k) {
        std::string v; easyai::args::get_string(j, k, v); return v;
    };

    auto body_collapse = [&](const std::string & text, int max_lines)
        -> std::vector<std::string> {
        std::vector<std::string> out;
        std::istringstream ss(text);
        std::string l;
        int n = 0, total = count_lines(text);
        bool expand = tc.expanded || ui.expand_tools;
        while (std::getline(ss, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!expand && n >= max_lines) break;
            out.push_back(l);
            ++n;
        }
        if (!expand && total > max_lines)
            out.push_back("… +" + std::to_string(total - max_lines)
                          + " lines (ctrl+o expands)");
        return out;
    };

    if (name == "bash" || name == "evaluate") {
        std::string cmd = name == "bash" ? S(a, "command") : S(a, "code");
        if (tc.state == ToolState::Running || tc.result.empty()) {
            inline_tool_rows(rows, ui, tc, "$",
                             first_line(cmd.empty() ? "…" : cmd, bw));
            return;
        }
        if (tc.state == ToolState::Error) {
            inline_tool_rows(rows, ui, tc, "$", first_line(cmd, bw));
            return;
        }
        std::string desc = S(a, "description");
        if (desc.empty()) desc = name == "bash" ? "Shell" : "Evaluate";
        std::vector<std::string> body;
        body.push_back("$ " + first_line(cmd, bw - 2));
        for (auto & l : body_collapse(tc.result, 10)) body.push_back(l);
        block_rows(rows, ui, "# " + desc, body);
        return;
    }
    if (name == "fs_read") {
        std::string extra = args_suffix(a, { "start_line", "offset", "limit" });
        inline_tool_rows(rows, ui, tc, "→",
            "Read " + tool_path_arg(a) + (extra.empty() ? "" : " " + extra));
        return;
    }
    if (name == "fs_write" || name == "fs_append") {
        inline_tool_rows(rows, ui, tc, "←",
            std::string(name == "fs_write" ? "Write " : "Append ")
            + tool_path_arg(a));
        return;
    }
    if (name == "fs_edit") {
        std::string olds = S(a, "oldString"), news = S(a, "newString");
        if (tc.state == ToolState::Done && (!olds.empty() || !news.empty())) {
            std::vector<std::string> styled =
                render_diff(th, ui.cols - kMargin - 3, olds, news);
            block_rows(rows, ui, "← Edit " + tool_path_arg(a), {}, styled);
            return;
        }
        std::string extra = args_suffix(a, { "start_line", "end_line",
                                             "replaceAll" });
        inline_tool_rows(rows, ui, tc, "←",
            "Edit " + tool_path_arg(a) + (extra.empty() ? "" : " " + extra));
        return;
    }
    if (name == "fs_glob" || name == "fs_grep") {
        const bool grep = name == "fs_grep";
        std::string pat = S(a, "pattern");
        std::string where = S(a, "path");
        int n = 0;
        if (tc.state == ToolState::Done) {
            if (grep) {
                // "Found N matches" header
                if (tc.result.compare(0, 6, "Found ") == 0)
                    n = std::atoi(tc.result.c_str() + 6);
            } else if (tc.result != "No files found")
                n = count_lines(tc.result);
        }
        std::string body = std::string(grep ? "Grep" : "Glob")
            + " \"" + pat + "\"";
        if (!where.empty()) body += " in " + where;
        if (tc.state == ToolState::Done)
            body += " (" + std::to_string(n)
                  + (n == 1 ? " match)" : " matches)");
        inline_tool_rows(rows, ui, tc, "✱", body);
        return;
    }
    if (name == "fs_list") {
        inline_tool_rows(rows, ui, tc, "→", "List " + tool_path_arg(a));
        return;
    }
    if (name == "web_fetch"
        || (name == "web" && S(a, "action") == "fetch")) {
        inline_tool_rows(rows, ui, tc, "%", "WebFetch " + S(a, "url"));
        return;
    }
    if (name == "web_search" || name == "web") {
        std::string q = S(a, "query");
        std::string body = "Search \"" + q + "\"";
        if (tc.state == ToolState::Done) {
            int n = 0;  // count numbered results "N. " at line starts
            std::istringstream ss(tc.result);
            std::string l;
            while (std::getline(ss, l))
                if (!l.empty() && l[0] >= '0' && l[0] <= '9') ++n;
            if (n) body += " (" + std::to_string(n) + " results)";
        }
        inline_tool_rows(rows, ui, tc, "◈", body);
        return;
    }
    if (name == "plan") {
        // Render the live checklist like opencode's "# Todos" block.
        if (ui.plan && !ui.plan->empty()
            && tc.state == ToolState::Done) {
            std::vector<std::string> styled;
            const int w = ui.cols - kMargin - 6;
            for (const auto & it : ui.plan->items()) {
                if (it.status == "deleted") continue;
                RGB c = it.status == "working" ? th.warning
                      : it.status == "error"   ? th.error : th.text_muted;
                const char * box = it.status == "done"    ? "[✓] "
                                 : it.status == "working" ? "[•] "
                                 : it.status == "error"   ? "[✗] " : "[ ] ";
                std::string r = fg(c) + box + clip_w(it.text, w - 4);
                int vis = 4 + disp_width(clip_w(it.text, w - 4));
                pad_row(r, vis, w, bg(th.bg_panel));
                r += RESET;
                styled.push_back(r);
            }
            block_rows(rows, ui, "# Todos", {}, styled);
            return;
        }
        inline_tool_rows(rows, ui, tc, "⚙", "Updating todos…");
        return;
    }
    if (name == "question") {
        if (tc.state == ToolState::Done) {
            std::vector<std::string> body;
            std::istringstream ss(tc.result);
            std::string l;
            while (std::getline(ss, l)) body.push_back(l);
            block_rows(rows, ui, "# Questions", body);
        } else {
            inline_tool_rows(rows, ui, tc, "→", "Asking questions…");
        }
        return;
    }
    if (name.rfind("knowledge", 0) == 0) {
        std::string act = S(a, "action");
        if (act.empty()) {
            // split form: knowledge_save / knowledge_search / …
            size_t us = name.find('_');
            if (us != std::string::npos) act = name.substr(us + 1);
        }
        std::string q = S(a, "query");
        if (q.empty()) q = S(a, "title");
        inline_tool_rows(rows, ui, tc, "→",
            "Knowledge " + act + (q.empty() ? "" : " \"" + q + "\""));
        return;
    }
    if (name == "datetime") {
        inline_tool_rows(rows, ui, tc, "⚙", "datetime");
        return;
    }
    // generic fallback — name + primitive args, opencode style
    std::string extra;
    {
        // show up to 3 short string/number args
        static const char * common[] = { "action", "url", "query", "name",
                                         "id", "text", "title" };
        int shown = 0;
        for (auto * k : common) {
            std::string v;
            if (easyai::args::get_string(a, k, v) && shown < 3) {
                if (!extra.empty()) extra += ", ";
                extra += std::string(k) + "=" + clip_w(v, 32);
                ++shown;
            }
        }
        if (!extra.empty()) extra = "[" + extra + "]";
    }
    inline_tool_rows(rows, ui, tc, "⚙", name + (extra.empty() ? "" : " " + extra));
    if ((tc.expanded || ui.expand_tools) && tc.state == ToolState::Done
        && !tc.result.empty()) {
        for (auto & l : body_collapse(tc.result, 3)) {
            std::string r(kMargin + kIndent + 2, ' ');
            r += fg(th.text_muted) + clip_w(l, ui.cols - kMargin - 8) + RESET;
            rows.push_back(r);
        }
    }
}

// Reasoning summary: first sentence-ish of the thought text.
std::string reasoning_title(const std::string & s, int maxw) {
    std::string t = first_line(s, maxw);
    return t;
}

std::vector<std::string> render_message(Ui & ui, Message & m, bool last_msg) {
    const Theme & th = ui.theme;
    std::vector<std::string> rows;
    const int w = ui.cols;

    if (m.role == Message::Info) {
        // centered "── title ──" divider (compaction marker etc.)
        std::string title = " " + m.parts[0].text + " ";
        int tw = disp_width(title);
        int side = std::max(2, (w - 2 * kMargin - tw) / 2);
        std::string row(kMargin, ' ');
        row += fg(th.border_active);
        for (int i = 0; i < side; ++i) row += "─";
        row += title;
        for (int i = 0; i < side; ++i) row += "─";
        row += RESET;
        rows.push_back(row);
        return rows;
    }

    if (m.role == Message::User) {
        const std::string & text = m.parts.empty() ? std::string()
                                                   : m.parts[0].text;
        RGB bc = th.primary;
        auto prow = [&](const std::string & content, int vis) {
            std::string r(kMargin, ' ');
            r += fg(bc); r += "┃"; r += RESET;
            r += bg(th.bg_panel);
            r += "  ";
            r += content;
            pad_row(r, vis + 2, w - kMargin - 1, bg(th.bg_panel));
            r += RESET;
            rows.push_back(r);
        };
        prow("", 0);
        for (auto & l : wrap_text(text, w - kMargin - 6)) {
            std::string body = fg(th.text) + l;
            prow(body, disp_width(l));
        }
        if (m.queued) {
            std::string badge = bg(bc) + fg(th.background) + BOLD
                              + " QUEUED " + RESET + bg(th.bg_panel);
            prow(badge, 8);
        }
        prow("", 0);
        return rows;
    }

    // assistant
    for (size_t pi = 0; pi < m.parts.size(); ++pi) {
        Part & p = m.parts[pi];
        if (!rows.empty()) rows.push_back("");
        switch (p.kind) {
            case PartKind::Reasoning: {
                if (!ui.opt.show_reasoning) { rows.pop_back(); break; }
                bool streaming = p.t1 == 0;
                std::string head(kMargin + kIndent, ' ');
                if (streaming) {
                    head += fg(th.warning) + spin_glyph(ui) + " Thinking";
                    std::string t = reasoning_title(p.text, w / 2);
                    if (!t.empty()) head += ": " + t;
                    head += RESET;
                    rows.push_back(head);
                } else {
                    long long dur = p.t1 - p.t0;
                    head += fg(th.warning);
                    head += ui.expand_thinking ? "- " : "+ ";
                    head += "Thought";
                    std::string t = reasoning_title(p.text, w / 2);
                    if (!t.empty()) head += ": " + t;
                    if (dur > 0) head += (t.empty() ? ": " : " · ")
                                       + short_dur(dur);
                    head += RESET;
                    rows.push_back(head);
                    if (ui.expand_thinking) {
                        rows.push_back("");
                        for (auto & l : render_markdown(
                                 th, w - kMargin - kIndent - 2,
                                 kMargin + kIndent + 2, p.text)) {
                            // body re-tinted muted
                            rows.push_back(l);
                        }
                    }
                }
                break;
            }
            case PartKind::Text: {
                for (auto & l : render_markdown(th, w - kMargin - kIndent,
                                                kMargin + kIndent, p.text))
                    rows.push_back(l);
                break;
            }
            case PartKind::Tool: {
                rows.pop_back();  // tools stack without the blank gap
                render_tool(rows, ui, m, p.tool);
                break;
            }
        }
    }

    if (!m.error.empty()) {
        rows.push_back("");
        std::string r(kMargin, ' ');
        r += fg(th.error); r += "┃"; r += RESET;
        r += bg(th.bg_panel) + "  ";
        std::string body = clip_w(m.error, w - kMargin - 6);
        r += fg(th.text_muted) + body;
        pad_row(r, 2 + disp_width(body), w - kMargin - 1, bg(th.bg_panel));
        r += RESET;
        rows.push_back(r);
    }

    if (m.done) {
        rows.push_back("");
        std::string r(kMargin + kIndent, ' ');
        r += fg(m.interrupted ? th.text_muted : th.primary);
        r += "▣ ";
        r += RESET;
        r += " ";
        r += fg(th.text) + ui.opt.agent;
        r += fg(th.text_muted) + " · " + (m.model.empty() ? ui.opt.model
                                                          : m.model);
        if (m.t_done > m.t_created)
            r += " · " + short_dur(m.t_done - m.t_created);
        if (m.interrupted) r += " · interrupted";
        r += RESET;
        rows.push_back(r);
    }
    (void) last_msg;
    return rows;
}

// Visible width of a styled row (skips SGR/OSC sequences).
int styled_width(const std::string & b) {
    int v = 0;
    for (size_t i = 0; i < b.size(); ) {
        if (b[i] == '\033') {
            size_t j = i + 1;
            if (j < b.size() && (b[j] == '[' || b[j] == ']')) {
                ++j;
                while (j < b.size() && !((unsigned char) b[j] >= 0x40
                                         && (unsigned char) b[j] <= 0x7e))
                    ++j;
                i = j + 1;
                continue;
            }
            ++i;
            continue;
        }
        size_t adv; char32_t u = u8_cp(b, i, &adv);
        v += cp_width(u); i += adv;
    }
    return v;
}

// ------------------------------------------------------------ home view ----
// Block-letter wordmark: "easy" muted + "ai" bright, opencode-style
// two-tone home screen.
void render_home(Ui & ui, std::vector<std::string> & out, int avail_rows) {
    const Theme & th = ui.theme;
    static const char * L[3] = {
        "█▀▀ ▄▀▄ ▄▀▀ █ █",
        "█▀▀ █▀█ ▀▀▄ ▀█▀",
        "▀▀▀ ▀ ▀ ▀▀▀  █ ",
    };
    static const char * R[3] = {
        " ▄▀▄ █",
        " █▀█ █",
        " ▀ ▀ ▀",
    };
    std::vector<std::string> body;
    for (int i = 0; i < 3; ++i) {
        std::string row = fg(th.text_muted) + L[i]
                        + fg(th.text) + BOLD + R[i] + RESET;
        body.push_back(row);
    }
    body.push_back("");
    if (!ui.opt.version.empty())
        body.push_back(fg(th.text_muted) + ui.opt.version + RESET);
    if (!ui.opt.url.empty())
        body.push_back(fg(th.text_muted) + ui.opt.url + RESET);
    body.push_back("");
    body.push_back(fg(th.text_muted) + "/ commands   @ files   /help keys"
                   + RESET);

    int top = std::max(0, (avail_rows - (int) body.size()) / 2);
    for (int i = 0; i < top; ++i) out.push_back("");
    const int logo_w = disp_width(std::string(L[0]) + R[0]);
    for (auto & b : body) {
        int vis = styled_width(b);
        int left = std::max(0, (ui.cols - std::max(vis, logo_w)) / 2);
        out.push_back(std::string((size_t) left, ' ') + b);
    }
}

// -------------------------------------------------------------- prompt -----
// Returns the editor cursor (row offset within the prompt block, col)
// via out-params so the painter can park the terminal cursor there.
struct PromptLayout {
    std::vector<std::string> rows;
    int cursor_row = 0, cursor_col = 0;   // relative to first prompt row
    int input_rows = 1;
};

PromptLayout render_prompt(Ui & ui) {
    const Theme & th = ui.theme;
    PromptLayout pl;
    const int w = ui.cols;
    const int text_w = std::max(8, w - 5);   // ┃ + 2 pad … 2 right pad

    // wrap input; track cursor
    std::vector<std::string> lines;
    std::vector<size_t> line_off;
    {
        size_t start = 0;
        for (size_t i = 0; i <= ui.input.size(); ++i) {
            if (i == ui.input.size() || ui.input[i] == '\n') {
                std::string logical = ui.input.substr(start, i - start);
                // hard-wrap logical line into text_w chunks
                size_t off = 0;
                if (logical.empty()) {
                    lines.push_back("");
                    line_off.push_back(start);
                } else
                    while (off < logical.size() || off == 0) {
                        int cw = 0; size_t j = off;
                        while (j < logical.size()) {
                            size_t adv; char32_t u = u8_cp(logical, j, &adv);
                            int x = cp_width(u);
                            if (cw + x > text_w) break;
                            cw += x; j += adv;
                        }
                        lines.push_back(logical.substr(off, j - off));
                        line_off.push_back(start + off);
                        if (j == off) break;
                        off = j;
                        if (off >= logical.size()) break;
                    }
                start = i + 1;
            }
        }
        if (lines.empty()) { lines.push_back(""); line_off.push_back(0); }
    }
    // cursor row/col
    {
        int crow = 0, ccol = 0;
        for (size_t li = 0; li < lines.size(); ++li) {
            size_t lo = line_off[li];
            size_t hi = lo + lines[li].size();
            bool last = li + 1 == lines.size();
            if (ui.cur >= lo && (ui.cur < hi + 1 || last)) {
                crow = (int) li;
                ccol = disp_width(ui.input.substr(lo,
                          std::min(ui.cur, hi) - lo));
                if (ui.cur > hi && !last) { crow++; ccol = 0; }
            }
        }
        pl.cursor_row = 1 + crow;             // +1: top pad row
        pl.cursor_col = 3 + ccol;             // ┃ + 2 pad
    }
    pl.input_rows = (int) lines.size();

    const int max_input_rows = std::max(3, ui.rows / 3);
    int first_vis = 0;
    if ((int) lines.size() > max_input_rows) {
        // keep cursor visible
        int crow = pl.cursor_row - 1;
        first_vis = std::max(0, std::min(crow - max_input_rows + 1,
                                         (int) lines.size() - max_input_rows));
        pl.cursor_row -= first_vis;
    }

    RGB border_c = th.border_subtle;
    {
        std::lock_guard<std::mutex> lk(ui.mu);
        if (ui.busy) {
            float t = (float)((now_ms() / 120) % 20) / 19.0f;
            if (t > 0.5f) t = 1.0f - t;
            border_c = lerp(th.border_subtle, th.primary, t * 2);
        } else {
            border_c = th.primary;
        }
    }

    auto box_row = [&](const std::string & styled, int vis) {
        std::string r;
        r += fg(border_c); r += "┃"; r += RESET;
        r += bg(th.bg_element) + "  ";
        r += styled;
        pad_row(r, vis + 2, w - 1, bg(th.bg_element));
        r += RESET;
        pl.rows.push_back(r);
    };

    box_row("", 0);
    bool busy_now;
    size_t queue_n;
    {
        std::lock_guard<std::mutex> lk(ui.mu);
        busy_now = ui.busy;
        queue_n = ui.queue.size();
    }
    if (ui.input.empty() && !ui.comp.active) {
        std::string ph = std::string("Ask anything… \"")
            + kPlaceholders[ui.placeholder_idx
                            % (int)(sizeof(kPlaceholders)/sizeof(*kPlaceholders))]
            + "\"";
        box_row(fg(th.text_muted) + clip_w(ph, text_w) + RESET,
                disp_width(clip_w(ph, text_w)));
    } else {
        int shown = 0;
        for (size_t li = (size_t) first_vis;
             li < lines.size() && shown < max_input_rows; ++li, ++shown)
            box_row(fg(th.text) + lines[li] + RESET, disp_width(lines[li]));
    }
    // meta row: Agent · model  ·  right: queue count
    {
        std::string left = fg(th.primary) + ui.opt.agent + RESET;
        left += fg(th.text_muted) + " · " + RESET;
        left += fg(th.text) + ui.opt.model + RESET;
        std::string right;
        if (queue_n)
            right = fg(th.text_muted) + std::to_string(queue_n)
                  + " queued" + RESET;
        int lw = styled_width(left), rw = styled_width(right);
        std::string row = left;
        int fill = (w - 5) - lw - rw;
        if (fill < 1) fill = 1;
        row += bg(th.bg_element);
        row.append((size_t) fill, ' ');
        row += right;
        box_row(row, lw + fill + rw);
    }
    pl.rows.push_back("");  // gets the ╹ cap painted below
    {
        std::string & cap = pl.rows.back();
        cap = fg(border_c);
        cap += "╹";
        cap += RESET;
    }
    (void) busy_now;
    return pl;
}

// ------------------------------------------------------------ status row ---
std::string render_status(Ui & ui) {
    const Theme & th = ui.theme;
    std::string left, right;
    bool busy; std::string label; int tpct, cpct; double tps;
    {
        std::lock_guard<std::mutex> lk(ui.mu);
        busy = ui.busy; label = ui.spin_label;
        tpct = ui.thinking_pct; cpct = ui.ctx_pct; tps = ui.tps;
    }
    std::string lplain, rplain;
    RGB lcol = th.text_muted;
    if (busy) {
        if (label.empty()) label = "Thinking";
        lplain = spin_glyph(ui) + " " + label;
        if (tpct >= 0) lplain += " " + std::to_string(tpct) + "%";
        lplain += "…";
        if (ui.esc_hits > 0)
            right = fg(th.primary) + "esc again to interrupt" + RESET;
        else
            right = fg(th.text) + "esc" + RESET + fg(th.text_muted)
                  + " interrupt" + RESET;
    } else {
        lplain = "enter send · shift+enter newline · / commands · @ files";
        std::string usage;
        if (cpct >= 0) usage = std::to_string(cpct) + "%";
        if (tps > 0.1) {
            char b[32];
            std::snprintf(b, sizeof(b), "%.1f t/s", tps);
            usage += (usage.empty() ? "" : " · ") + std::string(b);
        }
        if (!usage.empty())
            right = fg(th.text_muted) + usage + RESET;
    }
    int rw = styled_width(right);
    int lmax = ui.cols - 2 * kMargin - rw - 1;
    lplain = clip_w(lplain, std::max(4, lmax));
    left = fg(lcol) + lplain + RESET;
    int lw = disp_width(lplain);
    std::string row(kMargin, ' ');
    row += left;
    int fill = ui.cols - kMargin - lw - rw - kMargin;
    if (fill < 1) fill = 1;
    row += std::string((size_t) fill, ' ');
    row += right;
    return row;
}

// --------------------------------------------------------------- footer ----
std::string render_footer(Ui & ui) {
    const Theme & th = ui.theme;
    std::string cwd = ui.opt.cwd;
    const char * home = ::getenv("HOME");
    if (home && cwd.rfind(home, 0) == 0)
        cwd = "~" + cwd.substr(std::strlen(home));
    size_t ntools = ui.cli ? ui.cli->tools().size() : 0;
    std::string right;
    right += fg(ntools ? th.success : th.text_muted) + "•" + RESET;
    right += fg(th.text) + " " + std::to_string(ntools) + " tools" + RESET;
    right += "  ";
    right += fg(th.text_muted) + "/help" + RESET;
    int rw = styled_width(right);
    std::string left = fg(th.text_muted)
        + clip_w(cwd, ui.cols - rw - 2 * kMargin - 2) + RESET;
    int lw = styled_width(left);
    std::string row(kMargin, ' ');
    row += left;
    int fill = ui.cols - kMargin - lw - rw - kMargin;
    if (fill < 1) fill = 1;
    row += std::string((size_t) fill, ' ');
    row += right;
    return row;
}

// ------------------------------------------------------------- overlays ----
// Completion popup rows (drawn directly above the prompt block).
std::vector<std::string> render_completion(Ui & ui) {
    std::vector<std::string> rows;
    if (!ui.comp.active || ui.comp.view.empty()) return rows;
    const Theme & th = ui.theme;
    const int maxn = 8;
    int n = std::min((int) ui.comp.view.size(), maxn);
    int first = 0;
    if (ui.comp.sel >= maxn) first = ui.comp.sel - maxn + 1;
    const int w = ui.cols - 2 * kMargin;
    for (int i = 0; i < n; ++i) {
        int idx = first + i;
        if (idx >= (int) ui.comp.view.size()) break;
        const auto & it = ui.comp.view[(size_t) idx];
        bool sel = idx == ui.comp.sel;
        std::string r(kMargin, ' ');
        RGB rb = sel ? th.primary : th.bg_element;
        RGB tf = sel ? th.background : th.text;
        RGB df = sel ? th.background : th.text_muted;
        r += bg(rb);
        r += fg(tf) + " " + it.title;
        int vis = 1 + disp_width(it.title);
        if (!it.desc.empty()) {
            std::string d = "  " + it.desc;
            d = clip_w(d, w - vis - 1);
            r += fg(df) + d;
            vis += disp_width(d);
        }
        pad_row(r, vis, w, bg(rb));
        r += RESET;
        rows.push_back(r);
    }
    return rows;
}

// Modal select dialog → full replacement rows for its band.
std::vector<std::string> render_dialog(Ui & ui) {
    std::vector<std::string> rows;
    if (!ui.dialog) return rows;
    const Theme & th = ui.theme;
    Dialog & d = *ui.dialog;
    const int bw = std::min(60, ui.cols - 4);
    const int lx = (ui.cols - bw) / 2;

    std::vector<DialogItem *> vis;
    for (auto & it : d.items) {
        if (d.filter.empty()
            || fuzzy_score(d.filter, it.title + " " + it.desc) >= 0)
            vis.push_back(&it);
    }
    if (d.sel >= (int) vis.size()) d.sel = (int) vis.size() - 1;
    if (d.sel < 0) d.sel = 0;

    auto box = [&](const std::string & styled, int visw) {
        std::string r(lx, ' ');
        r += bg(th.bg_panel) + " " + styled;
        pad_row(r, visw + 1, bw, bg(th.bg_panel));
        r += RESET;
        rows.push_back(r);
    };
    box("", 0);
    {
        std::string t = fg(th.text) + std::string(BOLD) + d.title + RESET
                      + bg(th.bg_panel);
        std::string esc = fg(th.text_muted) + "esc" + RESET + bg(th.bg_panel);
        int fill = bw - 2 - disp_width(d.title) - 3 - 1;
        if (fill < 1) fill = 1;
        box(t + std::string((size_t) fill, ' ') + esc,
            disp_width(d.title) + fill + 3);
    }
    box("", 0);
    {
        std::string f = fg(th.text_muted) + "> " + RESET + bg(th.bg_panel)
                      + fg(th.text) + d.filter;
        box(f, 2 + disp_width(d.filter));
    }
    box("", 0);
    const int maxn = std::min(10, std::max(3, ui.rows - 12));
    int first = 0;
    if (d.sel >= maxn) first = d.sel - maxn + 1;
    for (int i = 0; i < maxn && first + i < (int) vis.size(); ++i) {
        const auto & it = *vis[(size_t)(first + i)];
        bool sel = first + i == d.sel;
        std::string r;
        RGB rb = sel ? th.primary : th.bg_panel;
        RGB tf = sel ? th.background : th.text;
        r += bg(rb) + fg(tf) + " " + it.title;
        int visw = 1 + disp_width(it.title);
        if (it.current) {
            r += fg(sel ? th.background : th.success) + " ●";
            visw += 2;
        }
        if (!it.desc.empty()) {
            std::string ds = clip_w("  " + it.desc, bw - 3 - visw);
            r += fg(sel ? th.background : th.text_muted) + ds;
            visw += disp_width(ds);
        }
        pad_row(r, visw, bw - 2, bg(rb));
        r += RESET;
        std::string outer(lx, ' ');
        outer += bg(th.bg_panel) + " " + r + bg(th.bg_panel) + " " + RESET;
        rows.push_back(outer);
    }
    if (vis.empty()) box(fg(th.text_muted) + "no matches" + RESET
                         + bg(th.bg_panel), 10);
    box("", 0);
    box(fg(th.text_muted) + "enter select · esc close" + RESET
        + bg(th.bg_panel), 25);
    box("", 0);
    return rows;
}

// Question modal (question-tool bridge). Locks ui.mu — the worker
// thread creates/destroys the QuestionUi under the same mutex.
std::vector<std::string> render_question(Ui & ui) {
    std::vector<std::string> rows;
    std::lock_guard<std::mutex> lk(ui.mu);
    if (!ui.q.active || ui.q.idx >= ui.q.items.size()) return rows;
    const Theme & th = ui.theme;
    const QuestionItem & qi = ui.q.items[ui.q.idx];
    const int bw = std::min(72, ui.cols - 4);
    const int lx = (ui.cols - bw) / 2;
    auto box = [&](const std::string & styled, int visw) {
        std::string r(lx, ' ');
        r += bg(th.bg_panel) + " " + styled;
        pad_row(r, visw + 1, bw, bg(th.bg_panel));
        r += RESET;
        rows.push_back(r);
    };
    box("", 0);
    {
        std::string hdr = qi.header.empty() ? "Question" : qi.header;
        std::string t = bg(th.secondary) + fg(th.background) + " " + hdr + " "
                      + RESET + bg(th.bg_panel);
        std::string pos = std::to_string(ui.q.idx + 1) + "/"
                        + std::to_string(ui.q.items.size());
        int fill = bw - 2 - (int) disp_width(hdr) - 2 - (int) pos.size() - 1;
        if (fill < 1) fill = 1;
        box(t + std::string((size_t) fill, ' ')
              + fg(th.text_muted) + pos + RESET + bg(th.bg_panel),
            (int) disp_width(hdr) + 2 + fill + (int) pos.size());
    }
    box("", 0);
    for (auto & l : wrap_text(qi.question, bw - 4))
        box(fg(th.text) + l + RESET + bg(th.bg_panel), disp_width(l));
    box("", 0);
    const int total = (int) qi.options.size() + (qi.custom ? 1 : 0);
    for (int i = 0; i < total; ++i) {
        bool is_custom = i == (int) qi.options.size();
        bool sel = i == ui.q.sel;
        std::string label = is_custom ? "Type your own answer…"
                                      : qi.options[(size_t) i].label;
        std::string desc  = is_custom ? "" : qi.options[(size_t) i].description;
        std::string mark;
        if (qi.multiple && !is_custom)
            mark = ui.q.chosen.count(i) ? "[x] " : "[ ] ";
        RGB rb = sel ? th.primary : th.bg_panel;
        RGB tf = sel ? th.background : th.text;
        std::string r = bg(rb) + fg(tf) + " " + mark + label;
        int visw = 1 + (int) mark.size() + disp_width(label);
        if (!desc.empty()) {
            std::string ds = clip_w("  " + desc, bw - 4 - visw);
            r += fg(sel ? th.background : th.text_muted) + ds;
            visw += disp_width(ds);
        }
        pad_row(r, visw, bw - 2, bg(rb));
        r += RESET;
        std::string outer(lx, ' ');
        outer += bg(th.bg_panel) + " " + r + bg(th.bg_panel) + " " + RESET;
        rows.push_back(outer);
    }
    if (ui.q.custom_mode) {
        box("", 0);
        std::string f = fg(th.text_muted) + "> " + RESET + bg(th.bg_panel)
                      + fg(th.text) + ui.q.custom;
        box(f, 2 + disp_width(ui.q.custom));
    }
    box("", 0);
    std::string hint = qi.multiple
        ? "space toggle · enter confirm · esc dismiss"
        : "enter select · esc dismiss";
    box(fg(th.text_muted) + hint + RESET + bg(th.bg_panel),
        (int) hint.size());
    box("", 0);
    return rows;
}

// Toast rows (top-right floating box, newest last). Locks ui.mu —
// the worker thread pushes toasts under the same mutex.
std::vector<std::string> render_toasts(Ui & ui) {
    std::vector<std::string> rows;
    const Theme & th = ui.theme;
    long long now = now_ms();
    std::lock_guard<std::mutex> lk(ui.mu);
    ui.toasts.erase(std::remove_if(ui.toasts.begin(), ui.toasts.end(),
                        [&](const Toast & t) { return t.until <= now; }),
                    ui.toasts.end());
    for (const auto & t : ui.toasts) {
        RGB bc = t.variant == Toast::Error   ? th.error
               : t.variant == Toast::Warning ? th.warning
               : t.variant == Toast::Success ? th.success : th.info;
        const int bw = std::min(48, ui.cols / 2);
        auto lines = wrap_text(t.text, bw - 6);
        auto trow = [&](const std::string & styled, int visw) {
            std::string r((size_t) std::max(0, ui.cols - bw - 2), ' ');
            r += fg(bc) + "┃" + RESET + bg(th.bg_panel) + " ";
            r += styled;
            pad_row(r, visw + 1, bw - 2, bg(th.bg_panel));
            r += fg(bc) + bg(th.bg_panel) + "" + RESET;
            r += fg(bc) + "┃" + RESET;
            rows.push_back(r);
        };
        trow("", 0);
        if (!t.title.empty())
            trow(std::string(BOLD) + fg(th.text) + t.title + RESET
                 + bg(th.bg_panel), disp_width(t.title));
        for (auto & l : lines)
            trow(fg(th.text) + l + RESET + bg(th.bg_panel), disp_width(l));
        trow("", 0);
    }
    return rows;
}

// ---------------------------------------------------------- frame painter --
bool has_running_tool(const Message & m) {
    for (auto & p : m.parts)
        if (p.kind == PartKind::Tool && p.tool.state == ToolState::Running)
            return true;
    for (auto & p : m.parts)
        if (p.kind == PartKind::Reasoning && p.t1 == 0) return true;
    return false;
}

void rebuild_chat(Ui & ui, std::vector<std::string> & chat) {
    std::lock_guard<std::mutex> lk(ui.mu);
    if (ui.cache.size() != ui.msgs.size()) {
        ui.cache.clear();
        ui.cache.resize(ui.msgs.size());
    }
    chat.clear();
    for (size_t i = 0; i < ui.msgs.size(); ++i) {
        Message & m = ui.msgs[i];
        bool animated = !m.done || has_running_tool(m);
        uint64_t key = (m.rev * 1315423911ull) ^ (uint64_t) ui.cols
                     ^ ((uint64_t) ui.expand_tools << 40)
                     ^ ((uint64_t) ui.expand_thinking << 41)
                     ^ ((animated ? (uint64_t) ui.spin_frame : 0ull) << 42);
        if (ui.cache[i].key != key || ui.cache[i].lines.empty()) {
            ui.cache[i].lines = render_message(ui, m, i + 1 == ui.msgs.size());
            ui.cache[i].key = key;
        }
        if (i) chat.push_back("");
        for (auto & l : ui.cache[i].lines) chat.push_back(l);
    }
}

void paint(Ui & ui) {
    term_size(ui);
    const Theme & th = ui.theme;
    std::vector<std::string> frame;
    frame.reserve((size_t) ui.rows);

    // --- bottom-up: footer, status, prompt
    PromptLayout pl = render_prompt(ui);
    std::string status = render_status(ui);
    std::string footer = render_footer(ui);
    int bottom_rows = (int) pl.rows.size() + 2;

    // --- chat area
    std::vector<std::string> chat;
    rebuild_chat(ui, chat);
    int avail = ui.rows - bottom_rows;
    if (avail < 1) avail = 1;
    std::vector<std::string> top;
    bool empty_chat;
    {
        std::lock_guard<std::mutex> lk(ui.mu);
        empty_chat = ui.msgs.empty();
    }
    if (empty_chat) {
        render_home(ui, top, avail);
    } else {
        int total = (int) chat.size();
        int max_scroll = std::max(0, total - avail);
        if (ui.scroll > max_scroll) ui.scroll = max_scroll;
        int first = std::max(0, total - avail - ui.scroll);
        for (int i = first; i < total && (int) top.size() < avail; ++i)
            top.push_back(chat[(size_t) i]);
        // scroll indicator
        if (ui.scroll > 0 && !top.empty()) {
            std::string ind = fg(th.text_muted) + "↓ "
                + std::to_string(ui.scroll) + " lines below" + RESET;
            top.back() = std::string(kMargin, ' ') + ind;
        }
    }
    while ((int) top.size() < avail) top.push_back("");

    // --- completion popup overlays the bottom of the chat area
    auto comp_rows = render_completion(ui);
    if (!comp_rows.empty()) {
        int n = (int) comp_rows.size();
        int at = avail - n;
        if (at < 0) at = 0;
        for (int i = 0; i < n && at + i < avail; ++i)
            top[(size_t)(at + i)] = comp_rows[(size_t) i];
    }
    // --- toasts overlay top-right
    auto toast_rows = render_toasts(ui);
    for (size_t i = 0; i < toast_rows.size() && (int) i + 1 < avail; ++i)
        top[i + 1] = toast_rows[i];

    // --- modal dialogs replace the middle band
    std::vector<std::string> modal =
        ui.q.active ? render_question(ui) : render_dialog(ui);
    if (!modal.empty()) {
        int at = std::max(1, (avail - (int) modal.size()) / 3);
        for (size_t i = 0; i < modal.size() && at + (int) i < avail; ++i)
            top[(size_t) at + i] = modal[i];
    }

    for (auto & r : top) frame.push_back(r);
    for (auto & r : pl.rows) frame.push_back(r);
    frame.push_back(status);
    frame.push_back(footer);
    while ((int) frame.size() < ui.rows) frame.push_back("");
    frame.resize((size_t) ui.rows);

    // --- diff & emit
    std::string out;
    out += "\033[?2026h\033[?25l";
    bool full = ui.force_full
             || ui.frame_prev.size() != frame.size();
    std::string bgfill = bg(th.background);
    for (size_t r = 0; r < frame.size(); ++r) {
        if (!full && ui.frame_prev[r] == frame[r]) continue;
        char mv[24];
        std::snprintf(mv, sizeof(mv), "\033[%zu;1H", r + 1);
        out += mv;
        out += bgfill;
        out += "\033[2K";
        out += frame[r];
        out += RESET;
    }
    // cursor: in the prompt block unless a modal owns the screen
    bool show_cursor = !ui.q.active && !ui.dialog;
    if (show_cursor) {
        int prow = avail + pl.cursor_row + 1;          // 1-based
        char mv[24];
        std::snprintf(mv, sizeof(mv), "\033[%d;%dH", prow,
                      pl.cursor_col + 1);
        out += mv;
        out += "\033[?25h";
    }
    out += "\033[?2026l";
    term_write(out);
    ::fflush(stdout);
    ui.frame_prev = std::move(frame);
    ui.force_full = false;
}

// --------------------------------------------------------------- editor ----
void ed_insert(Ui & ui, const std::string & s) {
    ui.input.insert(ui.cur, s);
    ui.cur += s.size();
    ui.hist_pos = -1;
}
void ed_backspace(Ui & ui) {
    if (ui.cur == 0) return;
    size_t p = ui.cur - 1;
    while (p > 0 && ((unsigned char) ui.input[p] & 0xc0) == 0x80) --p;
    ui.input.erase(p, ui.cur - p);
    ui.cur = p;
}
void ed_delete(Ui & ui) {
    if (ui.cur >= ui.input.size()) return;
    size_t n = u8_len((unsigned char) ui.input[ui.cur]);
    ui.input.erase(ui.cur, n);
}
void ed_left(Ui & ui) {
    if (ui.cur == 0) return;
    --ui.cur;
    while (ui.cur > 0 && ((unsigned char) ui.input[ui.cur] & 0xc0) == 0x80)
        --ui.cur;
}
void ed_right(Ui & ui) {
    if (ui.cur >= ui.input.size()) return;
    ui.cur += u8_len((unsigned char) ui.input[ui.cur]);
    if (ui.cur > ui.input.size()) ui.cur = ui.input.size();
}
bool word_ch(char c) {
    return std::isalnum((unsigned char) c) || c == '_' || c == '-';
}
void ed_word_left(Ui & ui) {
    while (ui.cur > 0 && !word_ch(ui.input[ui.cur - 1])) --ui.cur;
    while (ui.cur > 0 && word_ch(ui.input[ui.cur - 1])) --ui.cur;
}
void ed_word_right(Ui & ui) {
    while (ui.cur < ui.input.size() && !word_ch(ui.input[ui.cur])) ++ui.cur;
    while (ui.cur < ui.input.size() && word_ch(ui.input[ui.cur])) ++ui.cur;
}
void ed_kill_eol(Ui & ui) {
    size_t e = ui.input.find('\n', ui.cur);
    if (e == std::string::npos) e = ui.input.size();
    if (e == ui.cur && e < ui.input.size()) e += 1;
    ui.input.erase(ui.cur, e - ui.cur);
}
void ed_kill_bol(Ui & ui) {
    size_t b = ui.input.rfind('\n', ui.cur ? ui.cur - 1 : 0);
    b = (b == std::string::npos) ? 0 : b + 1;
    ui.input.erase(b, ui.cur - b);
    ui.cur = b;
}
void ed_del_word(Ui & ui) {
    size_t e = ui.cur;
    ed_word_left(ui);
    ui.input.erase(ui.cur, e - ui.cur);
}
// line-wise up/down inside a multiline buffer; returns false when the
// cursor is already on the first/last logical line (→ history nav).
bool ed_vmove(Ui & ui, int dir) {
    size_t bol = ui.input.rfind('\n', ui.cur ? ui.cur - 1 : 0);
    bol = (bol == std::string::npos) ? 0 : bol + 1;
    if (dir < 0) {
        if (bol == 0) return false;
        size_t pbol = ui.input.rfind('\n', bol >= 2 ? bol - 2 : 0);
        pbol = (pbol == std::string::npos) ? 0 : pbol + 1;
        size_t col = ui.cur - bol;
        size_t plen = (bol - 1) - pbol;
        ui.cur = pbol + std::min(col, plen);
        return true;
    }
    size_t eol = ui.input.find('\n', ui.cur);
    if (eol == std::string::npos) return false;
    size_t nbol = eol + 1;
    size_t neol = ui.input.find('\n', nbol);
    if (neol == std::string::npos) neol = ui.input.size();
    size_t col = ui.cur - bol;
    ui.cur = nbol + std::min(col, neol - nbol);
    return true;
}

// ---------------------------------------------------------- completions ----
void comp_refresh(Ui & ui) {
    Completion & c = ui.comp;
    if (!c.active) return;
    // text between anchor+1 and cursor is the filter
    if (ui.cur <= c.anchor || c.anchor >= ui.input.size()) {
        c.active = false;
        return;
    }
    std::string filter = ui.input.substr(c.anchor + 1,
                                         ui.cur - c.anchor - 1);
    if (filter.find(' ') != std::string::npos
        || filter.find('\n') != std::string::npos) {
        c.active = false;
        return;
    }
    c.view.clear();
    std::vector<std::pair<int, const DialogItem *>> scored;
    for (const auto & it : c.all) {
        int s = fuzzy_score(filter, it.title);
        if (s >= 0) scored.push_back({ s, &it });
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](auto & a, auto & b) { return a.first > b.first; });
    for (auto & p : scored) c.view.push_back(*p.second);
    if (c.sel >= (int) c.view.size()) c.sel = 0;
    if (c.view.empty()) c.active = false;
}

void comp_open(Ui & ui, char kind) {
    Completion & c = ui.comp;
    c.active = true;
    c.kind = kind;
    c.anchor = ui.cur;          // caller inserts the trigger char after
    c.sel = 0;
    c.all.clear();
    if (kind == '/') {
        for (const auto & sc : ui.commands)
            c.all.push_back({ "/" + sc.name, sc.desc, sc.name, false });
    } else {
        std::string root = ui.opt.cwd.empty() ? "." : ui.opt.cwd;
        for (auto & f : scan_files(root, 4000))
            c.all.push_back({ f, "", f, false });
    }
}

void comp_accept(Ui & ui) {
    Completion & c = ui.comp;
    if (!c.active || c.view.empty()) { c.active = false; return; }
    const DialogItem & it = c.view[(size_t) c.sel];
    std::string repl = (c.kind == '/')
                           ? "/" + it.value + " "
                           : "@" + it.value + " ";
    ui.input.replace(c.anchor, ui.cur - c.anchor, repl);
    ui.cur = c.anchor + repl.size();
    c.active = false;
}

// ----------------------------------------------------------- toasts api ----
void add_toast(Ui & ui, Toast::Variant v, const std::string & title,
               const std::string & text, int ms = 5000) {
    std::lock_guard<std::mutex> lk(ui.mu);
    ui.toasts.push_back(Toast{ v, title, text, now_ms() + ms });
    ui.bump();
}

// ------------------------------------------------------- question bridge ---
std::mutex g_ask_mu;   // serializes concurrent question tools

bool asker_active_impl() { return g_ui != nullptr; }

bool ask_questions_impl(const std::vector<QuestionItem> & items,
                        std::vector<std::vector<std::string>> & answers) {
    Ui * ui = g_ui;
    if (!ui || items.empty()) return false;
    std::lock_guard<std::mutex> serial(g_ask_mu);
    std::unique_lock<std::mutex> lk(ui->mu);
    ui->q = QuestionUi{};
    ui->q.active = true;
    ui->q.items = items;
    ui->q.answers.assign(items.size(), {});
    ui->bump();
    ui->cv.wait(lk, [&] { return ui->q.finished || ui->q.dismissed; });
    bool ok = ui->q.finished && !ui->q.dismissed;
    if (ok) answers = ui->q.answers;
    ui->q = QuestionUi{};
    ui->bump();
    return ok;
}

// Handle a key while the question modal is up. Runs on the UI thread
// with ui.mu held by caller.
void question_key(Ui & ui, const Key & k) {
    QuestionUi & q = ui.q;
    if (q.idx >= q.items.size()) return;
    const QuestionItem & qi = q.items[q.idx];
    const int total = (int) qi.options.size() + (qi.custom ? 1 : 0);
    auto commit_current = [&] {
        std::vector<std::string> ans;
        if (q.custom_mode) {
            if (!q.custom.empty()) ans.push_back(q.custom);
        } else if (qi.multiple) {
            for (int i : q.chosen)
                if (i < (int) qi.options.size())
                    ans.push_back(qi.options[(size_t) i].label);
        } else if (q.sel < (int) qi.options.size()) {
            ans.push_back(qi.options[(size_t) q.sel].label);
        }
        q.answers[q.idx] = std::move(ans);
        q.custom.clear(); q.custom_mode = false;
        q.chosen.clear(); q.sel = 0;
        if (++q.idx >= q.items.size()) {
            q.finished = true;
            ui.cv.notify_all();
        }
    };
    if (q.custom_mode) {
        switch (k.type) {
            case Key::Char:  q.custom += k.text; break;
            case Key::Paste: q.custom += k.text; break;
            case Key::Backspace:
                while (!q.custom.empty()
                       && ((unsigned char) q.custom.back() & 0xc0) == 0x80)
                    q.custom.pop_back();
                if (!q.custom.empty()) q.custom.pop_back();
                break;
            case Key::Enter: commit_current(); break;
            case Key::Esc:   q.custom_mode = false; q.custom.clear(); break;
            default: break;
        }
        ui.bump();
        return;
    }
    switch (k.type) {
        case Key::Up:   q.sel = (q.sel + total - 1) % std::max(1, total); break;
        case Key::Down: q.sel = (q.sel + 1) % std::max(1, total); break;
        case Key::Char:
            if (k.text == " " && qi.multiple
                && q.sel < (int) qi.options.size()) {
                if (!q.chosen.insert(q.sel).second) q.chosen.erase(q.sel);
            }
            break;
        case Key::Enter:
            if (qi.custom && q.sel == (int) qi.options.size())
                q.custom_mode = true;
            else commit_current();
            break;
        case Key::Esc:
        case Key::CtrlC:
            q.dismissed = true;
            ui.cv.notify_all();
            break;
        default: break;
    }
    ui.bump();
}

// ---------------------------------------------------------- turn driver ----
// All Client callbacks run on the worker thread (inside cli.chat()),
// so every mutation below takes ui.mu and bumps the version counter.

Message & last_assistant(Ui & ui) {
    for (auto it = ui.msgs.rbegin(); it != ui.msgs.rend(); ++it)
        if (it->role == Message::Assistant && !it->done) return *it;
    // insert before any trailing queued-user placeholders so the
    // streaming reply lands under the prompt that started this turn
    size_t pos = ui.msgs.size();
    while (pos > 0 && ui.msgs[pos - 1].role == Message::User
           && ui.msgs[pos - 1].queued)
        --pos;
    Message m;
    m.role = Message::Assistant;
    m.t_created = now_ms();
    m.model = ui.opt.model;
    ui.msgs.insert(ui.msgs.begin() + (long) pos, std::move(m));
    return ui.msgs[pos];
}

void note_token_rate(Ui & ui) {
    long long t = now_ms();
    if (ui.tok_t0 == 0) { ui.tok_t0 = t; ui.tok_n = 0; }
    ++ui.tok_n;
    long long dt = t - ui.tok_t0;
    if (dt >= 1500) {
        ui.tps = ui.tok_n * 1000.0 / (double) dt;
        ui.tok_t0 = t;
        ui.tok_n = 0;
    }
}

void install_callbacks(Ui & ui) {
    Client & cli = *ui.cli;
    cli.on_token([&ui](const std::string & piece) {
        if (piece.empty()) return;
        std::lock_guard<std::mutex> lk(ui.mu);
        Message & m = last_assistant(ui);
        // close an open reasoning part
        for (auto & p : m.parts)
            if (p.kind == PartKind::Reasoning && p.t1 == 0) p.t1 = now_ms();
        if (m.parts.empty() || m.parts.back().kind != PartKind::Text) {
            Part p; p.kind = PartKind::Text; p.t0 = now_ms();
            m.parts.push_back(std::move(p));
        }
        m.parts.back().text += piece;
        m.rev++;
        ui.spin_label = "Responding";
        ui.thinking_pct = -1;
        note_token_rate(ui);
        ui.bump();
    });
    cli.on_reason([&ui](const std::string & piece) {
        if (piece.empty()) return;
        std::lock_guard<std::mutex> lk(ui.mu);
        Message & m = last_assistant(ui);
        if (m.parts.empty() || m.parts.back().kind != PartKind::Reasoning
            || m.parts.back().t1 != 0) {
            // reuse a still-open reasoning part if any, else new one
            bool open = !m.parts.empty()
                     && m.parts.back().kind == PartKind::Reasoning
                     && m.parts.back().t1 == 0;
            if (!open) {
                Part p; p.kind = PartKind::Reasoning; p.t0 = now_ms();
                m.parts.push_back(std::move(p));
            }
        }
        m.parts.back().text += piece;
        m.rev++;
        ui.spin_label = "Thinking";
        ui.thinking_pct = -1;
        note_token_rate(ui);
        ui.bump();
    });
    cli.on_prompt_progress([&ui](int processed, int total, int /*cached*/,
                                 double /*ms*/) {
        std::lock_guard<std::mutex> lk(ui.mu);
        ui.spin_label = "Thinking";
        ui.thinking_pct = total > 0 ? (int)(processed * 100.0 / total) : -1;
        ui.bump();
    });
    // session checkpoint after every tool round-trip (same contract as
    // the legacy REPL's on_tool handler).
    cli.on_tool([&ui](const ToolCall &, const ToolResult &) {
        if (!ui.hooks.save_session) return;
        std::string err;
        ui.hooks.save_session(&err);  // best-effort; errors surface at turn end
    });
}

// Wrap every registered tool handler so the TUI can show live
// running → done/error state. Must run BEFORE the first chat() call.
void wrap_tools(Ui & ui) {
    Client & cli = *ui.cli;
    std::vector<Tool> wrapped = cli.tools();
    for (auto & t : wrapped) {
        ToolHandler orig = t.handler;
        std::string tname = t.name;
        t.handler = [&ui, orig, tname](const ToolCall & c) -> ToolResult {
            {
                std::lock_guard<std::mutex> lk(ui.mu);
                Message & m = last_assistant(ui);
                for (auto & p : m.parts)
                    if (p.kind == PartKind::Reasoning && p.t1 == 0)
                        p.t1 = now_ms();
                Part p; p.kind = PartKind::Tool;
                p.tool.name = tname;
                p.tool.args = c.arguments_json;
                p.tool.state = ToolState::Running;
                p.tool.t0 = now_ms();
                m.parts.push_back(std::move(p));
                m.rev++;
                // opencode-style pending labels
                std::string n = easyai::canonical_tool_name(tname);
                ui.spin_label =
                    n == "bash"      ? "Writing command" :
                    n == "fs_read"   ? "Reading file" :
                    n == "fs_edit"   ? "Preparing edit" :
                    n == "fs_write"  ? "Preparing write" :
                    n == "fs_glob"   ? "Finding files" :
                    n == "fs_grep"   ? "Searching content" :
                    n == "web_fetch" ? "Fetching from the web" :
                    n == "web_search"? "Searching web" :
                    n == "question"  ? "Asking questions" :
                    n == "plan"      ? "Updating todos" : ("Running " + n);
                ui.bump();
            }
            ToolResult r = orig ? orig(c)
                                : ToolResult::error("tool has no handler");
            {
                std::lock_guard<std::mutex> lk(ui.mu);
                Message & m = last_assistant(ui);
                for (auto it = m.parts.rbegin(); it != m.parts.rend(); ++it) {
                    if (it->kind == PartKind::Tool
                        && it->tool.state == ToolState::Running
                        && it->tool.name == tname) {
                        it->tool.state = r.is_error ? ToolState::Error
                                                    : ToolState::Done;
                        it->tool.result = r.content;
                        it->tool.t1 = now_ms();
                        break;
                    }
                }
                m.rev++;
                ui.spin_label.clear();
                ui.bump();
            }
            return r;
        };
    }
    cli.clear_tools();
    for (auto & t : wrapped) cli.add_tool(std::move(t));
}

// Start one model turn on the worker thread.
void start_turn(Ui & ui, const std::string & prompt) {
    if (ui.worker.joinable()) ui.worker.join();
    {
        std::lock_guard<std::mutex> lk(ui.mu);
        ui.busy = true;
        ui.spin_label = "Thinking";
        ui.thinking_pct = -1;
        ui.tok_t0 = 0; ui.tok_n = 0;
        Message u;
        u.role = Message::User;
        Part p; p.kind = PartKind::Text; p.text = prompt;
        u.parts.push_back(std::move(p));
        u.t_created = now_ms();
        ui.msgs.push_back(std::move(u));
        ui.bump();
    }
    ui.scroll = 0;
    ui.cli->clear_cancel();
    ui.worker_live.store(true);
    ui.worker = std::thread([&ui, prompt] {
        std::string answer = ui.cli->chat(prompt);
        std::string err = ui.cli->last_error();
        bool ctx_full = ui.cli->last_was_ctx_full();
        bool incomplete = ui.cli->last_turn_was_incomplete();
        bool cancelled = ui.cli->cancel_requested();
        {
            std::lock_guard<std::mutex> lk(ui.mu);
            Message & m = last_assistant(ui);
            for (auto & p : m.parts)
                if (p.kind == PartKind::Reasoning && p.t1 == 0)
                    p.t1 = now_ms();
            m.done = true;
            m.t_done = now_ms();
            m.interrupted = cancelled;
            if (ctx_full)
                m.error = "Context full — start a new conversation "
                          "(/new) to keep going.";
            else if (answer.empty() && !err.empty() && !cancelled)
                m.error = err;
            else if ((answer.empty() || incomplete) && !cancelled)
                m.error = "Incomplete response — the model announced a "
                          "tool without calling it. Rephrase or check "
                          "/tools.";
            m.rev++;
            ui.busy = false;
            ui.spin_label.clear();
            ui.thinking_pct = -1;
            ui.ctx_pct  = ui.cli->last_ctx_pct();
            ui.ctx_used = ui.cli->last_ctx_used();
            ui.ctx_total= ui.cli->last_n_ctx();
            ui.bump();
        }
        if (ui.hooks.save_session) {
            std::string serr;
            if (!ui.hooks.save_session(&serr)) {
                std::lock_guard<std::mutex> lk(ui.mu);
                ui.toasts.push_back(Toast{ Toast::Warning, "session",
                                           "could not save: " + serr,
                                           now_ms() + 6000 });
                ui.bump();
            }
        }
        ui.worker_live.store(false);
        ui.bump();
    });
}

// /compress on the worker thread with a busy status.
void start_compress(Ui & ui) {
    if (!ui.hooks.compress) return;
    if (ui.worker.joinable()) ui.worker.join();
    {
        std::lock_guard<std::mutex> lk(ui.mu);
        ui.busy = true;
        ui.compacting = true;
        ui.spin_label = "Compacting";
        ui.bump();
    }
    ui.worker_live.store(true);
    ui.worker = std::thread([&ui] {
        std::string err;
        bool ok = ui.hooks.compress(&err);
        {
            std::lock_guard<std::mutex> lk(ui.mu);
            ui.busy = false;
            ui.compacting = false;
            ui.spin_label.clear();
            if (ok) {
                Message d;
                d.role = Message::Info;
                Part p; p.kind = PartKind::Text; p.text = "Compaction";
                d.parts.push_back(std::move(p));
                ui.msgs.push_back(std::move(d));
                ui.toasts.push_back(Toast{ Toast::Success, "",
                    "conversation compacted", now_ms() + 4000 });
            } else {
                ui.toasts.push_back(Toast{ Toast::Error, "compact failed",
                    err.empty() ? "see logs" : err, now_ms() + 6000 });
            }
            ui.bump();
        }
        if (ui.hooks.save_session) {
            std::string serr;
            ui.hooks.save_session(&serr);
        }
        ui.worker_live.store(false);
        ui.bump();
    });
}

// ---------------------------------------------------------- slash cmds -----
std::vector<SlashCmd> default_commands(Ui & ui) {
    std::vector<SlashCmd> v = {
        { "help",     "show keybindings and commands" },
        { "new",      "start a new conversation (clears history + todos)" },
        { "clear",    "clear conversation history" },
        { "compress", "compact the conversation into a recap" },
        { "plan",     "show the current todo list" },
        { "tools",    "list registered tools" },
        { "models",   "switch the served model" },
        { "theme",    "switch color theme" },
        { "status",   "connection / session details" },
        { "editor",   "edit the prompt in $EDITOR" },
        { "export",   "write the transcript to a markdown file" },
        { "exit",     "quit" },
        { "quit",     "quit" },
    };
    if (!ui.hooks.compress)
        v.erase(std::remove_if(v.begin(), v.end(),
                  [](const SlashCmd & c) { return c.name == "compress"; }),
                v.end());
    if (!ui.hooks.list_models)
        v.erase(std::remove_if(v.begin(), v.end(),
                  [](const SlashCmd & c) { return c.name == "models"; }),
                v.end());
    return v;
}

void open_help_dialog(Ui & ui) {
    Dialog d;
    d.title = "Help";
    for (const auto & c : ui.commands)
        d.items.push_back({ "/" + c.name, c.desc, "", false });
    d.items.push_back({ "enter", "send · shift+enter / ctrl+j newline", "", false });
    d.items.push_back({ "esc esc", "interrupt generation", "", false });
    d.items.push_back({ "ctrl+c ctrl+c", "quit (first clears input)", "", false });
    d.items.push_back({ "ctrl+p", "command palette", "", false });
    d.items.push_back({ "ctrl+r", "toggle thinking detail", "", false });
    d.items.push_back({ "ctrl+o", "toggle tool output detail", "", false });
    d.items.push_back({ "pgup/pgdn · wheel", "scroll the transcript", "", false });
    d.items.push_back({ "@", "insert a file path", "", false });
    d.on_pick = [](const DialogItem &) {};
    ui.dialog = std::move(d);
    ui.bump();
}

void run_slash(Ui & ui, const std::string & line);

void open_palette(Ui & ui) {
    Dialog d;
    d.title = "Commands";
    for (const auto & c : ui.commands)
        d.items.push_back({ "/" + c.name, c.desc, c.name, false });
    d.on_pick = [&ui](const DialogItem & it) {
        run_slash(ui, "/" + it.value);
    };
    ui.dialog = std::move(d);
    ui.bump();
}

void transcript_to_markdown(Ui & ui, std::string & out) {
    std::lock_guard<std::mutex> lk(ui.mu);
    for (auto & m : ui.msgs) {
        if (m.role == Message::User) {
            out += "## User\n\n";
            if (!m.parts.empty()) out += m.parts[0].text + "\n\n";
        } else if (m.role == Message::Assistant) {
            out += "## Assistant";
            if (!m.model.empty()) out += " (" + m.model + ")";
            out += "\n\n";
            for (auto & p : m.parts) {
                if (p.kind == PartKind::Text) out += p.text + "\n\n";
                if (p.kind == PartKind::Tool) {
                    out += "`" + p.tool.name + "` ";
                    out += p.tool.state == ToolState::Error ? "✗" : "✓";
                    out += "\n\n";
                }
            }
        }
    }
}

void run_slash(Ui & ui, const std::string & line) {
    std::string cmd = line.substr(1);
    size_t sp = cmd.find(' ');
    std::string arg = sp == std::string::npos ? "" : cmd.substr(sp + 1);
    if (sp != std::string::npos) cmd.resize(sp);

    auto clear_history = [&](bool plan_too) {
        ui.cli->clear_history();
        if (plan_too && ui.plan) ui.plan->clear();
        {
            std::lock_guard<std::mutex> lk(ui.mu);
            ui.msgs.clear();
            ui.cache.clear();
            ui.ctx_pct = ui.ctx_used = ui.ctx_total = -1;
            ui.tps = 0;
        }
        if (ui.hooks.save_session) {
            std::string err;
            ui.hooks.save_session(&err);
        }
        ui.force_full = true;
        ui.bump();
    };

    if (cmd == "exit" || cmd == "quit") { ui.ctrlc_hits = -1; return; }
    if (cmd == "help") { open_help_dialog(ui); return; }
    if (cmd == "new" || cmd == "reset") { clear_history(true);  return; }
    if (cmd == "clear")                 { clear_history(false); return; }
    if (cmd == "compress") {
        bool busy;
        { std::lock_guard<std::mutex> lk(ui.mu); busy = ui.busy; }
        if (busy) add_toast(ui, Toast::Warning, "", "busy — try after this turn");
        else start_compress(ui);
        return;
    }
    if (cmd == "plan" || cmd == "todos") {
        std::lock_guard<std::mutex> lk(ui.mu);
        Message m;
        m.role = Message::Assistant;
        m.done = false;
        Part p; p.kind = PartKind::Tool;
        p.tool.name = "plan";
        p.tool.state = ToolState::Done;
        p.tool.args = "{}";
        m.parts.push_back(std::move(p));
        ui.msgs.push_back(std::move(m));
        ui.bump();
        return;
    }
    if (cmd == "tools") {
        Dialog d;
        d.title = "Tools";
        for (const auto & t : ui.cli->tools())
            d.items.push_back({ t.name, t.wire_description(), "", false });
        d.on_pick = [](const DialogItem &) {};
        ui.dialog = std::move(d);
        ui.bump();
        return;
    }
    if (cmd == "models" && ui.hooks.list_models) {
        Dialog d;
        d.title = "Models";
        for (auto & id : ui.hooks.list_models())
            d.items.push_back({ id, "", id, id == ui.opt.model });
        d.on_pick = [&ui](const DialogItem & it) {
            if (ui.hooks.set_model) ui.hooks.set_model(it.value);
            ui.opt.model = it.value;
            add_toast(ui, Toast::Success, "", "model → " + it.value);
        };
        ui.dialog = std::move(d);
        ui.bump();
        return;
    }
    if (cmd == "theme") {
        Dialog d;
        d.title = "Themes";
        for (auto & n : theme_names())
            d.items.push_back({ n, "", n, n == ui.theme.name });
        d.on_pick = [&ui](const DialogItem & it) {
            ui.theme = theme_by_name(it.value);
            ui.cache.clear();
            ui.force_full = true;
            add_toast(ui, Toast::Success, "",
                      "theme → " + it.value
                      + "  ([cli] theme = " + it.value + " to persist)");
        };
        ui.dialog = std::move(d);
        ui.bump();
        return;
    }
    if (cmd == "status") {
        std::string txt = "url: " + ui.opt.url + "\nmodel: " + ui.opt.model;
        if (!ui.opt.session_path.empty())
            txt += "\nsession: " + ui.opt.session_path;
        {
            std::lock_guard<std::mutex> lk(ui.mu);
            if (ui.ctx_used >= 0 && ui.ctx_total > 0)
                txt += "\nctx: " + std::to_string(ui.ctx_used) + "/"
                     + std::to_string(ui.ctx_total)
                     + " (" + std::to_string(ui.ctx_pct) + "%)";
        }
        add_toast(ui, Toast::Info, "status", txt, 8000);
        return;
    }
    if (cmd == "editor") {
        const char * ed = ::getenv("EDITOR");
        if (!ed || !*ed) { add_toast(ui, Toast::Warning, "", "$EDITOR not set"); return; }
        char tmpl[] = "/tmp/easyai-prompt-XXXXXX";
        int fd = ::mkstemp(tmpl);
        if (fd < 0) { add_toast(ui, Toast::Error, "", "mkstemp failed"); return; }
        {
            ssize_t w = ::write(fd, ui.input.data(), ui.input.size());
            (void) w;
            ::close(fd);
        }
        leave_terminal(ui);
        std::string sh = std::string(ed) + " " + tmpl;
        int rc = std::system(sh.c_str());
        enter_terminal(ui);
        ui.force_full = true;
        if (rc == 0) {
            std::ifstream f(tmpl);
            std::stringstream ss; ss << f.rdbuf();
            ui.input = ss.str();
            while (!ui.input.empty()
                   && (ui.input.back() == '\n' || ui.input.back() == '\r'))
                ui.input.pop_back();
            ui.cur = ui.input.size();
        }
        ::unlink(tmpl);
        ui.bump();
        return;
    }
    if (cmd == "export") {
        std::string md;
        transcript_to_markdown(ui, md);
        std::string path = arg.empty() ? "easyai-session.md" : arg;
        std::ofstream f(path, std::ios::trunc);
        if (f) { f << md; add_toast(ui, Toast::Success, "", "wrote " + path); }
        else add_toast(ui, Toast::Error, "", "cannot write " + path);
        return;
    }
    add_toast(ui, Toast::Warning, "", "unknown command: /" + cmd + " — /help");
}

// ------------------------------------------------------------- run loop ----
void submit(Ui & ui) {
    std::string text = ui.input;
    // trim
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n'))
        text.pop_back();
    size_t b = 0;
    while (b < text.size() && (text[b] == ' ')) ++b;
    text.erase(0, b);
    if (text.empty()) return;
    ui.history.push_back(text);
    ui.hist_pos = -1;
    ui.input.clear();
    ui.cur = 0;
    ui.comp.active = false;
    if (text[0] == '/') { run_slash(ui, text); return; }
    bool busy;
    { std::lock_guard<std::mutex> lk(ui.mu); busy = ui.busy; }
    if (busy) {
        std::lock_guard<std::mutex> lk(ui.mu);
        ui.queue.push_back(text);
        Message u;
        u.role = Message::User;
        u.queued = true;
        Part p; p.kind = PartKind::Text; p.text = text;
        u.parts.push_back(std::move(p));
        u.t_created = now_ms();
        ui.msgs.push_back(std::move(u));
        ui.bump();
        return;
    }
    start_turn(ui, text);
}

void handle_key(Ui & ui, const Key & k) {
    // modal layers first
    if (ui.q.active) {
        std::lock_guard<std::mutex> lk(ui.mu);
        question_key(ui, k);
        return;
    }
    if (ui.dialog) {
        Dialog & d = *ui.dialog;
        switch (k.type) {
            case Key::Esc: case Key::CtrlC:
                ui.dialog.reset(); ui.force_full = true; break;
            case Key::Up:   d.sel--; break;
            case Key::Down: d.sel++; break;
            case Key::Backspace:
                while (!d.filter.empty()
                       && ((unsigned char) d.filter.back() & 0xc0) == 0x80)
                    d.filter.pop_back();
                if (!d.filter.empty()) d.filter.pop_back();
                break;
            case Key::Char:  d.filter += k.text; d.sel = 0; break;
            case Key::Paste: d.filter += k.text; break;
            case Key::Enter: {
                std::vector<DialogItem *> vis;
                for (auto & it : d.items)
                    if (d.filter.empty()
                        || fuzzy_score(d.filter, it.title + " " + it.desc) >= 0)
                        vis.push_back(&it);
                if (d.sel >= 0 && d.sel < (int) vis.size()) {
                    DialogItem picked = *vis[(size_t) d.sel];
                    auto cb = d.on_pick;
                    ui.dialog.reset();
                    ui.force_full = true;
                    if (cb) cb(picked);
                } else {
                    ui.dialog.reset();
                    ui.force_full = true;
                }
                break;
            }
            default: break;
        }
        ui.bump();
        return;
    }

    bool busy;
    { std::lock_guard<std::mutex> lk(ui.mu); busy = ui.busy; }

    switch (k.type) {
        case Key::Esc:
            if (ui.comp.active) { ui.comp.active = false; break; }
            if (busy) {
                long long t = now_ms();
                if (ui.esc_hits > 0 && t - ui.esc_t < 1200) {
                    ui.cli->request_cancel();
                    ui.esc_hits = 0;
                } else { ui.esc_hits = 1; ui.esc_t = t; }
            }
            break;
        case Key::CtrlC: {
            long long t = now_ms();
            if (!ui.input.empty()) {
                ui.input.clear(); ui.cur = 0; ui.comp.active = false;
                ui.ctrlc_hits = 0;
                break;
            }
            if (ui.ctrlc_hits > 0 && t - ui.ctrlc_t < 1200) {
                if (busy) ui.cli->request_cancel();
                ui.ctrlc_hits = -1;  // sentinel: quit
                break;
            }
            ui.ctrlc_hits = 1; ui.ctrlc_t = t;
            add_toast(ui, Toast::Info, "", "ctrl+c again to quit", 1500);
            break;
        }
        case Key::CtrlD:
            if (ui.input.empty()) ui.ctrlc_hits = -1;  // quit
            else ed_delete(ui);
            break;
        case Key::Enter:
            if (ui.comp.active) { comp_accept(ui); break; }
            submit(ui);
            break;
        case Key::ShiftEnter:
        case Key::Newline:
            ed_insert(ui, "\n");
            break;
        case Key::Tab:
            if (ui.comp.active) comp_accept(ui);
            break;
        case Key::Up:
            if (ui.comp.active) { ui.comp.sel--; if (ui.comp.sel < 0) ui.comp.sel = 0; break; }
            if (!ed_vmove(ui, -1)) {
                if (ui.history.empty()) break;
                if (ui.hist_pos < 0) {
                    ui.hist_stash = ui.input;
                    ui.hist_pos = (int) ui.history.size();
                }
                if (ui.hist_pos > 0) {
                    --ui.hist_pos;
                    ui.input = ui.history[(size_t) ui.hist_pos];
                    ui.cur = ui.input.size();
                }
            }
            break;
        case Key::Down:
            if (ui.comp.active) {
                ui.comp.sel++;
                if (ui.comp.sel >= (int) ui.comp.view.size())
                    ui.comp.sel = (int) ui.comp.view.size() - 1;
                break;
            }
            if (!ed_vmove(ui, 1)) {
                if (ui.hist_pos >= 0) {
                    ++ui.hist_pos;
                    if (ui.hist_pos >= (int) ui.history.size()) {
                        ui.input = ui.hist_stash;
                        ui.hist_pos = -1;
                    } else
                        ui.input = ui.history[(size_t) ui.hist_pos];
                    ui.cur = ui.input.size();
                }
            }
            break;
        case Key::Left:  ed_left(ui);  break;
        case Key::Right: ed_right(ui); break;
        case Key::WordLeft:  ed_word_left(ui);  break;
        case Key::WordRight: ed_word_right(ui); break;
        case Key::Home: case Key::CtrlA: {
            size_t b2 = ui.input.rfind('\n', ui.cur ? ui.cur - 1 : 0);
            ui.cur = (b2 == std::string::npos) ? 0 : b2 + 1;
            break;
        }
        case Key::End: case Key::CtrlE: {
            size_t e = ui.input.find('\n', ui.cur);
            ui.cur = (e == std::string::npos) ? ui.input.size() : e;
            break;
        }
        case Key::CtrlK: ed_kill_eol(ui); break;
        case Key::CtrlU: ed_kill_bol(ui); break;
        case Key::CtrlW: ed_del_word(ui); break;
        case Key::Backspace: {
            bool was_trigger = ui.comp.active && ui.cur == ui.comp.anchor + 1;
            ed_backspace(ui);
            if (was_trigger) ui.comp.active = false;
            break;
        }
        case Key::Delete: ed_delete(ui); break;
        case Key::PgUp:  ui.scroll += std::max(1, ui.rows / 2); break;
        case Key::PgDn:  ui.scroll -= std::max(1, ui.rows / 2);
                         if (ui.scroll < 0) ui.scroll = 0; break;
        case Key::WheelUp:   ui.scroll += 3; break;
        case Key::WheelDown: ui.scroll -= 3;
                             if (ui.scroll < 0) ui.scroll = 0; break;
        case Key::CtrlL: ui.force_full = true; break;
        case Key::CtrlP: open_palette(ui); break;
        case Key::CtrlR:
            ui.expand_thinking = !ui.expand_thinking;
            ui.cache.clear();
            break;
        case Key::CtrlO:
            ui.expand_tools = !ui.expand_tools;
            ui.cache.clear();
            break;
        case Key::Paste:
            ed_insert(ui, k.text);
            break;
        case Key::Char: {
            bool at_start_word =
                ui.cur == 0 || ui.input[ui.cur - 1] == ' '
                || ui.input[ui.cur - 1] == '\n';
            if (k.text == "/" && ui.cur == 0) {
                comp_open(ui, '/');
                ed_insert(ui, k.text);
            } else if (k.text == "@" && at_start_word) {
                comp_open(ui, '@');
                ed_insert(ui, k.text);
            } else {
                ed_insert(ui, k.text);
            }
            break;
        }
        default: break;
    }
    if (ui.comp.active) comp_refresh(ui);
    ui.bump();
}

}  // namespace

// ------------------------------------------------------------ public API ---
bool terminal_capable() {
    return ::isatty(STDIN_FILENO) == 1 && ::isatty(STDOUT_FILENO) == 1;
}

bool asker_active() { return asker_active_impl(); }

bool ask_questions(const std::vector<QuestionItem> & items,
                   std::vector<std::vector<std::string>> & answers_out) {
    return ask_questions_impl(items, answers_out);
}

// EASYAI_TUI_DEBUG=1 appends startup/shutdown breadcrumbs to
// /tmp/easyai-tui-debug.log — the TUI owns the terminal and parks
// stderr, so printf debugging needs a side channel.
void tui_debug(const char * msg) {
    if (!::getenv("EASYAI_TUI_DEBUG")) return;
    if (std::FILE * f = ::fopen("/tmp/easyai-tui-debug.log", "a")) {
        std::fprintf(f, "[%lld] %s (errno=%d %s)\n",
                     now_ms(), msg, errno, std::strerror(errno));
        ::fclose(f);
    }
}

int run(Client & cli, Plan & plan, const Options & opt, const Hooks & hooks) {
    if (!terminal_capable()) { tui_debug("terminal_capable=false"); return 2; }

    Ui ui;
    ui.cli = &cli;
    ui.plan = &plan;
    ui.opt = opt;
    ui.hooks = hooks;
    ui.theme = theme_by_name(opt.theme);
    if (ui.opt.cwd.empty()) {
        char buf[4096];
        if (::getcwd(buf, sizeof(buf))) ui.opt.cwd = buf;
    }
    {
        const char * ct = ::getenv("COLORTERM");
        g_truecolor = ct && (std::strstr(ct, "truecolor")
                             || std::strstr(ct, "24bit"));
        if (!g_truecolor) {
            const char * tp = ::getenv("TERM_PROGRAM");
            // Terminals that do 24-bit without setting COLORTERM.
            // Apple_Terminal stays on the 256-color fallback.
            if (tp && (!std::strcmp(tp, "iTerm.app")
                       || !std::strcmp(tp, "ghostty")
                       || !std::strcmp(tp, "WezTerm")
                       || !std::strcmp(tp, "vscode")
                       || !std::strcmp(tp, "kitty")))
                g_truecolor = true;
        }
    }
    ui.commands = default_commands(ui);
    ui.placeholder_idx = (int)(now_ms() / 1000)
        % (int)(sizeof(kPlaceholders) / sizeof(*kPlaceholders));

    install_callbacks(ui);
    wrap_tools(ui);

    if (!enter_terminal(ui)) {
        tui_debug("enter_terminal failed");
        std::fprintf(stderr, "easyai-tui: cannot enter raw mode\n");
        return 2;
    }
    tui_debug("entered terminal");
    g_ui = &ui;
    struct sigaction sa{};
    sa.sa_handler = on_winch;
    ::sigaction(SIGWINCH, &sa, nullptr);

    term_size(ui);
    ui.force_full = true;
    paint(ui);

    InputParser parser;
    uint64_t painted = 0;
    long long last_anim = 0;
    int rc = 0;

    while (true) {
        if (ui.ctrlc_hits == -1) break;  // quit sentinel

        pollfd pfd{ STDIN_FILENO, POLLIN, 0 };
        int pr = ::poll(&pfd, 1, 40);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            char buf[4096];
            ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0)
                for (auto & k : parser.feed(buf, (size_t) n))
                    handle_key(ui, k);
            else if (n == 0) break;  // stdin closed
        }

        if (g_resized.exchange(false)) {
            term_size(ui);
            ui.cache.clear();
            ui.force_full = true;
            ui.bump();
        }

        // animation tick while busy / toasts pending
        long long t = now_ms();
        bool busy;
        size_t queued;
        {
            std::lock_guard<std::mutex> lk(ui.mu);
            busy = ui.busy;
            queued = ui.queue.size();
        }
        if ((busy || !ui.toasts.empty()) && t - last_anim >= 80) {
            ui.spin_frame++;
            last_anim = t;
            ui.bump();
        }
        if (!busy && ui.esc_hits) ui.esc_hits = 0;
        if (ui.ctrlc_hits > 0 && t - ui.ctrlc_t > 1500) ui.ctrlc_hits = 0;

        // drain the queue: send the next prompt when idle
        if (!busy && queued && !ui.worker_live.load()) {
            std::string next;
            {
                std::lock_guard<std::mutex> lk(ui.mu);
                next = ui.queue.front();
                ui.queue.pop_front();
                // un-badge the matching queued user message; start_turn
                // pushes its own copy, so drop the placeholder.
                for (auto it = ui.msgs.rbegin(); it != ui.msgs.rend(); ++it)
                    if (it->role == Message::User && it->queued
                        && !it->parts.empty() && it->parts[0].text == next) {
                        ui.msgs.erase(std::next(it).base());
                        break;
                    }
                ui.cache.clear();
            }
            start_turn(ui, next);
        }

        if (ui.ver.load() != painted) {
            paint(ui);
            painted = ui.ver.load();
        }
    }

    // shutdown: cancel any in-flight turn, join the worker
    cli.request_cancel();
    if (ui.worker.joinable()) ui.worker.join();
    {
        // unblock a stuck question dialog (tool waiting on cv)
        std::lock_guard<std::mutex> lk(ui.mu);
        if (ui.q.active) { ui.q.dismissed = true; ui.cv.notify_all(); }
    }
    g_ui = nullptr;
    leave_terminal(ui);
    return rc;
}

}  // namespace easyai::tui
