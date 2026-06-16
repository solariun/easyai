#include "easyai/ui.hpp"

#include "easyai/engine.hpp"
#include "easyai/log.hpp"
#include "easyai/plan.hpp"
#include "easyai/presets.hpp"
#include "easyai/text.hpp"
#include "easyai/tool.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace easyai::ui {

// ---------------------------------------------------------------- Style
Style detect_style() {
    Style s;
    s.color = ::isatty(STDOUT_FILENO) != 0
              && std::getenv("NO_COLOR") == nullptr;
    return s;
}

// ---------------------------------------------------------------- Spinner
Spinner::Spinner(bool enabled)
    : enabled_(enabled && ::isatty(fileno(stdout)) != 0),
      // Mirror Style::detect_style: NO_COLOR=any disables coloured
      // output entirely (the "thinking" label falls back to plain
      // text in that case).  isatty is already covered by enabled_
      // above, but kept here for explicit pairing with the colour
      // decision so reading the field tells the whole story.
      color_(enabled_ && std::getenv("NO_COLOR") == nullptr) {}

Spinner::~Spinner() { stop_heartbeat(); }

Spinner::WriteScope::WriteScope(Spinner & s)
    : s_(s.enabled_ ? &s : nullptr) {
    if (!s_) return;
    s_->mu_.lock();
    s_->erase_active_locked_();
}

Spinner::WriteScope::~WriteScope() {
    if (!s_) return;
    std::fflush(stdout);
    s_->maybe_advance_locked_();
    s_->draw_locked_();
    s_->mu_.unlock();
}

Spinner::WriteScope Spinner::scoped() { return WriteScope(*this); }

void Spinner::write(const std::string & text) {
    if (enabled_) {
        auto _g = scoped();
        std::fputs(text.c_str(), stdout);
    } else {
        std::fputs(text.c_str(), stdout);
        std::fflush(stdout);
    }
}

void Spinner::initial_draw() {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lg(mu_);
    if (!active_) draw_locked_();
}

void Spinner::emit_speed_report_locked_() {
    const double speed = (token_speed_ > 0.1)
                             ? token_speed_ : last_nonzero_speed_;
    if (speed <= 0.1) return;
    erase_active_locked_();
    char buf[128];
    int n;
    if (context_pct_ >= 0 && ctx_used_ >= 0) {
        n = std::snprintf(buf, sizeof(buf),
            "\n● %d%% / %d tokens  avg: %.1ftk/s\n",
            context_pct_, ctx_used_, speed);
    } else if (ctx_used_ >= 0) {
        n = std::snprintf(buf, sizeof(buf),
            "\n● %d tokens  avg: %.1ftk/s\n",
            ctx_used_, speed);
    } else if (context_pct_ >= 0) {
        n = std::snprintf(buf, sizeof(buf),
            "\n● %d%%  avg: %.1ftk/s\n",
            context_pct_, speed);
    } else {
        n = std::snprintf(buf, sizeof(buf),
            "\n● avg: %.1ftk/s\n", speed);
    }
    if (n > 0) {
        if (color_) std::fputs("\033[34m", stdout);
        std::fwrite(buf, 1, (size_t) n, stdout);
        if (color_) std::fputs("\033[0m",  stdout);
    }
    std::fflush(stdout);
    token_speed_        = 0.0;
    last_nonzero_speed_ = 0.0;
    active_       = false;
    active_width_ = 0;
}

void Spinner::emit_speed_report() {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lg(mu_);
    emit_speed_report_locked_();
}

void Spinner::finish() {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lg(mu_);
    emit_speed_report_locked_();
    if (active_) erase_active_locked_();
    std::fflush(stdout);
    frame_               = 0;
    token_speed_         = 0.0;
    last_nonzero_speed_  = 0.0;
    last_tok_count_ = tok_count_.load(std::memory_order_relaxed);
    last_speed_time_ = std::chrono::steady_clock::now();
}

void Spinner::notify_token() {
    tok_count_.fetch_add(1, std::memory_order_relaxed);
}

void Spinner::set_context_pct(int pct) {
    if (!enabled_) return;
    if (pct < 0)         pct = -1;
    else if (pct > 100)  pct = 100;
    std::lock_guard<std::mutex> lg(mu_);
    if (context_pct_ == pct) return;
    context_pct_ = pct;
    // Repaint immediately if a glyph is currently visible so the
    // suffix updates without waiting for the next heartbeat tick.
    if (active_) {
        erase_active_locked_();
        draw_locked_();
        std::fflush(stdout);
    }
}

void Spinner::set_context_tokens(int used, int total) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lg(mu_);
    ctx_used_  = used;
    ctx_total_ = total;
}

void Spinner::set_thinking(bool on) {
    if (!enabled_) return;
    if (thinking_.load(std::memory_order_relaxed) == on) return;
    {
        std::lock_guard<std::mutex> lg(mu_);

        if (on) emit_speed_report_locked_();

        thinking_.store(on, std::memory_order_relaxed);
        if (on) {
            shimmer_phase_ = 0;
            thinking_pct_  = -1;
            token_speed_         = 0.0;
            last_nonzero_speed_  = 0.0;
            last_tok_count_ = tok_count_.load(std::memory_order_relaxed);
            last_speed_time_ = std::chrono::steady_clock::now();
        }
        if (active_) {
            erase_active_locked_();
            draw_locked_();
            std::fflush(stdout);
        }
    }
    hb_cv_.notify_all();
}

void Spinner::set_thinking_pct(int pct) {
    if (!enabled_) return;
    if (pct < 0)        pct = -1;
    else if (pct > 100) pct = 100;
    std::lock_guard<std::mutex> lg(mu_);
    if (thinking_pct_ == pct) return;
    thinking_pct_ = pct;
    // Repaint immediately if the shimmer is currently visible so the
    // operator sees the gauge tick up between heartbeats — server
    // emits one progress event per n_batch which is faster than the
    // 100 ms heartbeat for big prompts.
    if (active_ && thinking_.load(std::memory_order_relaxed)) {
        erase_active_locked_();
        draw_locked_();
        std::fflush(stdout);
    }
}

void Spinner::start_heartbeat(int interval_ms) {
    if (!enabled_) return;
    if (hb_running_.exchange(true)) return;
    interval_ms_ = interval_ms;
    hb_thread_   = std::thread(&Spinner::heartbeat_loop_, this);
}

void Spinner::stop_heartbeat() {
    if (!hb_running_.exchange(false)) return;
    hb_cv_.notify_all();
    if (hb_thread_.joinable()) hb_thread_.join();
}

void Spinner::maybe_advance_locked_() {
    const auto now = std::chrono::steady_clock::now();
    const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - last_advance_).count();
    if (ms >= kFrameAdvanceMs) {
        ++frame_;
        last_advance_ = now;
    }
}

void Spinner::erase_active_locked_() {
    if (!active_ || active_width_ <= 0) return;
    // Wipe `active_width_` chars to the left of the cursor.  We can't
    // assume \b is repeatable across all terminals when crossing a
    // line boundary, but the spinner is always drawn after the most
    // recent newline, so a simple backspace-space-backspace per char
    // is safe.
    for (int i = 0; i < active_width_; ++i) std::fputc('\b', stdout);
    for (int i = 0; i < active_width_; ++i) std::fputc(' ',  stdout);
    for (int i = 0; i < active_width_; ++i) std::fputc('\b', stdout);
    active_       = false;
    active_width_ = 0;
}

void Spinner::draw_locked_() {
    if (thinking_.load(std::memory_order_relaxed)) {
        draw_thinking_locked_();
        return;
    }
    static const char frames[] = { '|', '/', '-', '\\' };
    char buf[32];
    int  n;
    const bool has_pct   = context_pct_ >= 0;
    const bool has_speed = token_speed_ > 0.1;
    if (has_pct && has_speed) {
        n = std::snprintf(buf, sizeof(buf), "%c(%d%%, %.1ft/s)",
                          frames[frame_ % 4], context_pct_, token_speed_);
    } else if (has_pct) {
        n = std::snprintf(buf, sizeof(buf), "%c(%d%%)",
                          frames[frame_ % 4], context_pct_);
    } else if (has_speed) {
        n = std::snprintf(buf, sizeof(buf), "%c(%.1ft/s)",
                          frames[frame_ % 4], token_speed_);
    } else {
        buf[0] = frames[frame_ % 4];
        n = 1;
    }
    if (n < 0)                  n = 1;
    if (n >= (int) sizeof(buf)) n = (int) sizeof(buf) - 1;
    std::fwrite(buf, 1, (size_t) n, stdout);
    std::fflush(stdout);
    active_       = true;
    active_width_ = n;
}

// Render `thinking[ <pct>%]` with a bright spotlight that sweeps
// left-to-right across the letters, one cell per heartbeat tick.  The
// sweep period is text_len + kTrailing so the spotlight fully exits the
// word before the next pass begins (otherwise consecutive passes glue
// together and read as a flicker, not a sweep).  When color_ is off
// (NO_COLOR / piped stdout) we emit plain ASCII — same width, no escape
// codes — so the spinner still tracks correctly under `tee` etc.
void Spinner::draw_thinking_locked_() {
    // Brightness ramp from the bright peak outward.  256-colour
    // grayscale: 232 (near-black) … 255 (near-white).  Five values
    // give a soft falloff that reads as "spotlight" rather than
    // "blinking".
    static const int kBrightness[] = { 255, 253, 251, 249, 246 };
    static constexpr int kRampLen  = sizeof(kBrightness) / sizeof(int);
    // 244 is true mid-gray (RGB 128,128,128).  Out-of-spotlight chars
    // stay always readable on both dark and light terminals — the
    // sweep highlights motion ON TOP of an already-visible label
    // instead of pulling near-black letters into legibility one by
    // one.  Previous base of 235 was nearly invisible on dark
    // terminals between sweeps.
    static const int kBaseColor    = 244;

    // Build the visible text first so we know its cell width for the
    // sweep period and the active_width_ tracking.  The suffix is the
    // REAL prompt-eval progress fed by Client::on_prompt_progress
    // (server's easyai.prompt_progress events, mirroring llama-server's
    // `prompt_progress` field).  We deliberately do NOT fall back to
    // ctx_pct_ here because that's stale data from the prior turn —
    // showing it would mislead the operator into thinking the prompt
    // is mostly processed when only ctx fill is high.  No suffix is
    // honest until the first progress tick arrives.
    // Suffix shapes (since 2026-05-26):
    //   thinking_pct only            → " <N>%"
    //   thinking_pct + context_pct   → " <N>% · ctx <M>%"
    //   neither                      → ""
    // ctx-% during thinking is the LIVE value — the caller computes it
    // as (cached + processed) / n_ctx in the on_prompt_progress
    // handler, so it reflects where the KV cache will land at the end
    // of this prompt-eval pass, not stale data from the prior turn.
    char suffix[40] = {0};
    int  suffix_len = 0;
    if (thinking_pct_ >= 0 && context_pct_ >= 0) {
        suffix_len = std::snprintf(suffix, sizeof(suffix),
                                   " %d%% · ctx %d%%",
                                   thinking_pct_, context_pct_);
    } else if (thinking_pct_ >= 0) {
        suffix_len = std::snprintf(suffix, sizeof(suffix),
                                   " %d%%", thinking_pct_);
    }
    if (suffix_len < 0) suffix_len = 0;
    if (suffix_len >= (int) sizeof(suffix)) suffix_len = sizeof(suffix) - 1;
    static const char kWord[] = "thinking";
    static constexpr int kWordLen = sizeof(kWord) - 1;
    const int text_len = kWordLen + suffix_len;

    // kTrailing leaves the spotlight off-screen for a few frames between
    // sweeps so the cycle has visible "rest" — without it the sweep
    // restarts immediately and looks like a strobe.
    constexpr int kTrailing = 6;
    const int period = text_len + kTrailing;
    const int peak   = shimmer_phase_ % period;

    auto emit_char = [&](char c, int idx) {
        if (color_) {
            int dist = std::abs(peak - idx);
            int color = (dist < kRampLen)
                            ? kBrightness[dist]
                            : kBaseColor;
            std::fprintf(stdout, "\x1b[38;5;%dm%c", color, c);
        } else {
            std::fputc(c, stdout);
        }
    };
    for (int i = 0; i < kWordLen; ++i) emit_char(kWord[i], i);
    for (int i = 0; i < suffix_len; ++i) emit_char(suffix[i], kWordLen + i);
    if (color_) std::fputs("\x1b[0m", stdout);
    std::fflush(stdout);

    active_       = true;
    active_width_ = text_len;   // visible cells only — escapes don't move the cursor
}

void Spinner::heartbeat_loop_() {
    while (hb_running_) {
        // Pick the cadence based on the current state — shimmer needs
        // ~10 Hz to read as smooth, the idle |/-\ glyph is fine at 4 Hz
        // and avoids gratuitous redraws while the operator is reading.
        // Re-evaluated every loop so set_thinking() flipping the flag
        // mid-wait takes effect on the next tick (notify_all wakes us).
        const int sleep_ms = thinking_.load(std::memory_order_relaxed)
                                 ? kThinkingIntervalMs
                                 : interval_ms_;
        {
            std::unique_lock<std::mutex> wait_lk(hb_wait_mu_);
            hb_cv_.wait_for(wait_lk,
                            std::chrono::milliseconds(sleep_ms),
                            [this]{ return !hb_running_; });
        }
        if (!hb_running_) break;
        std::lock_guard<std::mutex> lg(mu_);
        if (!active_) continue;            // nothing drawn yet
        erase_active_locked_();
        {
            auto now_t = std::chrono::steady_clock::now();
            int cur = tok_count_.load(std::memory_order_relaxed);
            int delta = cur - last_tok_count_;
            double elapsed_s = std::chrono::duration<double>(
                now_t - last_speed_time_).count();
            if (elapsed_s > 0.05) {
                if (delta > 0) {
                    token_speed_    = delta / elapsed_s;
                    last_nonzero_speed_ = token_speed_;
                    last_tok_count_ = cur;
                    last_speed_time_ = now_t;
                } else if (elapsed_s > 2.0 && token_speed_ > 0.1) {
                    emit_speed_report_locked_();
                }
            }
        }
        if (thinking_.load(std::memory_order_relaxed)) {
            ++shimmer_phase_;              // sweep one cell to the right
        } else {
            ++frame_;
        }
        last_advance_ = std::chrono::steady_clock::now();
        draw_locked_();
    }
}

// ---------------------------------------------------------------- pretty-print
void print_presets(const Style & st, std::FILE * out) {
    for (const auto & p : easyai::all_presets()) {
        std::fprintf(out, "  %s%s%s  %s%s%s\n",
                     st.bold(),  p.name.c_str(),        st.reset(),
                     st.dim(),   p.description.c_str(), st.reset());
    }
}

void render_plan(const Plan & plan, const Style & st, std::FILE * out) {
    std::fprintf(out, "\n%s── plan ──%s\n", st.yellow(), st.reset());
    std::ostringstream ss;
    plan.render(ss, st.color);
    std::fputs(ss.str().c_str(), out);
    std::fputc('\n', out);
    std::fflush(out);
}

void print_tool_row(const std::string & name,
                    const std::string & description,
                    const Style & st,
                    std::FILE * out) {
    std::fprintf(out, "%s%s%s\n", st.bold(), name.c_str(), st.reset());
    std::size_t i = 0;
    while (i < description.size()) {
        std::size_t nl = description.find('\n', i);
        std::string line = (nl == std::string::npos)
                               ? description.substr(i)
                               : description.substr(i, nl - i);
        std::fprintf(out, "  %s%s%s\n",
                     st.dim(), line.c_str(), st.reset());
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
}

// ---------------------------------------------------------------- Markdown tables
namespace {

// Visible width: count UTF-8 code points (skip 0b10xxxxxx continuation
// bytes).  Good enough for Latin / accented text; CJK double-width
// glyphs are undercounted, an accepted edge for a terminal table.
std::size_t disp_width(const std::string & s) {
    std::size_t w = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++w;
    return w;
}

std::string md_rstrip_nl(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::string md_trim(const std::string & s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

bool md_is_blank(const std::string & s) {
    for (char c : s) if (c != ' ' && c != '\t') return false;
    return true;
}

// A line is a table-row candidate iff its first non-blank char is `|`.
// This is the leading-pipe GFM form LLMs emit; restricting to it keeps
// prose (which may contain an inline `a | b`) streaming uninterrupted.
bool md_is_row(const std::string & line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return i < line.size() && line[i] == '|';
}

// Split a `| a | b |` row into trimmed cells, dropping the bordering
// pipes and honouring a `\|` escape inside a cell.
std::vector<std::string> md_cells(const std::string & line) {
    std::string s = md_trim(md_rstrip_nl(line));
    if (!s.empty() && s.front() == '|') s.erase(0, 1);
    if (!s.empty() && s.back()  == '|') s.pop_back();
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size() && s[i + 1] == '|') {
            cur.push_back('|');
            ++i;
        } else if (c == '|') {
            out.push_back(md_trim(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(md_trim(cur));
    return out;
}

// A separator row: every cell is `:?-+:?` (e.g. ---, :--, --:, :-:).
bool md_is_separator(const std::string & line) {
    std::vector<std::string> cells = md_cells(line);
    if (cells.empty()) return false;
    for (const std::string & c : cells) {
        std::size_t i = 0, n = c.size();
        if (n == 0) return false;
        if (c[i] == ':') ++i;
        std::size_t dashes = 0;
        while (i < n && c[i] == '-') { ++i; ++dashes; }
        if (i < n && c[i] == ':') ++i;
        if (i != n || dashes == 0) return false;
    }
    return true;
}

// Alignment from a separator cell: 'l' left (default), 'r' right, 'c' centre.
char md_align(const std::string & c) {
    bool l = !c.empty() && c.front() == ':';
    bool r = !c.empty() && c.back()  == ':';
    if (l && r) return 'c';
    if (r)      return 'r';
    return 'l';
}

std::string md_pad(const std::string & s, std::size_t width, char align) {
    std::size_t w   = disp_width(s);
    std::size_t pad = width > w ? width - w : 0;
    if (align == 'r') return std::string(pad, ' ') + s;
    if (align == 'c') { std::size_t l = pad / 2; return std::string(l, ' ') + s + std::string(pad - l, ' '); }
    return s + std::string(pad, ' ');
}

// Render rows[0]=header, rows[1]=separator, rows[2..]=body as a boxed,
// column-aligned table.  Only called in colour mode (see MdTableStream).
std::string render_md_table(const std::vector<std::string> & rows, const Style & st) {
    std::vector<std::string>              header = md_cells(rows[0]);
    std::vector<std::string>              sep    = md_cells(rows[1]);
    std::vector<std::vector<std::string>> body;
    for (std::size_t i = 2; i < rows.size(); ++i) body.push_back(md_cells(rows[i]));

    std::size_t ncols = header.size();
    for (const auto & b : body) ncols = std::max(ncols, b.size());
    if (ncols == 0) return std::string();

    std::vector<char>        aligns(ncols, 'l');
    for (std::size_t c = 0; c < ncols && c < sep.size(); ++c) aligns[c] = md_align(sep[c]);

    std::vector<std::size_t> w(ncols, 1);
    for (std::size_t c = 0; c < ncols; ++c) {
        std::size_t mx = c < header.size() ? disp_width(header[c]) : 0;
        for (const auto & b : body) if (c < b.size()) mx = std::max(mx, disp_width(b[c]));
        w[c] = std::max<std::size_t>(mx, 1);
    }

    const std::string bar = std::string(st.dim()) + "│" + st.reset();
    auto hbar = [&](const char * l, const char * m, const char * r) {
        std::string s = st.dim();
        s += l;
        for (std::size_t c = 0; c < ncols; ++c) {
            for (std::size_t k = 0; k < w[c] + 2; ++k) s += "─";
            s += (c + 1 < ncols) ? m : r;
        }
        s += st.reset();
        s += "\n";
        return s;
    };
    auto rowline = [&](const std::vector<std::string> & cells, bool head) {
        std::string s;
        for (std::size_t c = 0; c < ncols; ++c) {
            std::string cell = c < cells.size() ? cells[c] : std::string();
            std::string p    = md_pad(cell, w[c], aligns[c]);
            s += bar;
            s += " ";
            if (head) { s += st.bold(); s += p; s += st.reset(); }
            else      { s += p; }
            s += " ";
        }
        s += bar;
        s += "\n";
        return s;
    };

    std::string out;
    out += hbar("┌", "┬", "┐");
    out += rowline(header, true);
    out += hbar("├", "┼", "┤");
    for (const auto & b : body) out += rowline(b, false);
    out += hbar("└", "┴", "┘");
    return out;
}

}  // namespace

std::string MdTableStream::step_line_(const std::string & line) {
    switch (state_) {
    case State::NORMAL:
        if (md_is_row(line)) {
            rows_.clear();
            rows_.push_back(line);
            state_ = State::HEADER_SEEN;
            return std::string();
        }
        return line;
    case State::HEADER_SEEN:
        if (md_is_separator(line)) {
            rows_.push_back(line);
            state_ = State::IN_TABLE;
            return std::string();
        } else {
            // Header candidate was a false alarm — emit it raw, then
            // reprocess this line from the neutral state.
            std::string out = rows_.empty() ? std::string() : rows_[0];
            rows_.clear();
            state_ = State::NORMAL;
            out += step_line_(line);
            return out;
        }
    case State::IN_TABLE:
        if (md_is_row(line)) {
            rows_.push_back(line);
            return std::string();
        } else {
            std::string out = render_md_table(rows_, st_);
            rows_.clear();
            state_ = State::NORMAL;
            out += step_line_(line);
            return out;
        }
    }
    return std::string();
}

std::string MdTableStream::finalize_() {
    std::string out;
    if (state_ == State::IN_TABLE)                          out = render_md_table(rows_, st_);
    else if (state_ == State::HEADER_SEEN && !rows_.empty()) out = rows_[0];
    rows_.clear();
    state_ = State::NORMAL;
    return out;
}

std::string MdTableStream::feed(const std::string & seg) {
    if (!color_) return seg;
    pending_ += seg;
    std::string out;
    for (;;) {
        std::size_t nl = pending_.find('\n');
        if (nl == std::string::npos) {
            // Incomplete trailing line.  Hold it only if it could belong
            // to a table (we're already inside one, or it starts like a
            // row, or it's leading whitespace that might precede a row);
            // otherwise stream the prose immediately.
            if (state_ != State::NORMAL || md_is_row(pending_) || md_is_blank(pending_))
                break;
            out += pending_;
            pending_.clear();
            break;
        }
        std::string line = pending_.substr(0, nl + 1);
        pending_.erase(0, nl + 1);
        out += step_line_(line);
    }
    return out;
}

std::string MdTableStream::flush() {
    if (!color_) return std::string();
    std::string out;
    if (!pending_.empty()) {
        out += step_line_(pending_);   // fold the trailing partial as a final line
        pending_.clear();
    }
    out += finalize_();
    return out;
}

// ---------------------------------------------------------------- Streaming
Streaming::Streaming(Spinner & spinner, StreamStats & stats, const Style & style)
    : spinner_(spinner), stats_(stats), style_(style), md_(style) {}

Streaming & Streaming::show_reasoning(bool v) { show_reasoning_ = v; return *this; }
Streaming & Streaming::verbose       (bool v) { verbose_        = v; return *this; }

void Streaming::emit_content_(const std::string & seg) {
    if (seg.empty()) return;
    std::string in = seg;
    if (last_kind_ == StreamKind::REASON) in.insert(0, "\n");
    last_kind_ = StreamKind::CONTENT;
    // Run the content through the markdown-table filter: prose passes
    // straight through, table blocks are buffered and rendered once
    // complete.  flush_md_() (end of turn / reasoning / tool boundary)
    // drains anything still buffered.
    std::string out = md_.feed(in);
    if (!out.empty()) spinner_.write(out);
}

void Streaming::flush_md_() {
    std::string out = md_.flush();
    if (!out.empty()) spinner_.write(out);
}

void Streaming::flush_pending() { flush_md_(); }

void Streaming::emit_reason_(const std::string & seg) {
    if (seg.empty()) return;
    // Reasoning interrupts the content stream — close any open table
    // first so it renders above the reasoning rather than swallowing it.
    flush_md_();
    if (show_reasoning_) {
        std::string buf;
        buf.reserve(seg.size() + 16);
        if (last_kind_ == StreamKind::CONTENT) buf += "\n";
        buf += style_.dim();
        buf += seg;
        buf += style_.reset();
        spinner_.write(buf);
    }
    last_kind_ = StreamKind::REASON;
    // When show_reasoning is off, the spinner heartbeat keeps the glyph
    // ticking on its own — no explicit refresh needed here.
}

namespace {

// Strip bare <think>/<thinking> open and close markers from `s`. Used
// on the reasoning_content stream as a defense in depth — the parser
// usually removes the wrappers, but a few templates leak them through.
std::string strip_think_markers(const std::string & s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ) {
        if (s.compare(i, 7,  "<think>")     == 0) { i += 7;  continue; }
        if (s.compare(i, 10, "<thinking>")  == 0) { i += 10; continue; }
        if (s.compare(i, 8,  "</think>")    == 0) { i += 8;  continue; }
        if (s.compare(i, 11, "</thinking>") == 0) { i += 11; continue; }
        out.push_back(s[i++]);
    }
    return out;
}

// True if `tail` could still extend into one of the four think markers.
// Used to decide which trailing bytes of the content buffer to hold for
// the next chunk vs. flush immediately.
bool tail_is_partial_think_marker(const std::string & tail) {
    static const char * const kTags[] = {
        "<think>", "<thinking>", "</think>", "</thinking>",
    };
    for (const char * tag : kTags) {
        std::string_view tv(tag);
        if (tail.size() < tv.size() && tv.compare(0, tail.size(), tail) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

void Streaming::on_token_(const std::string & piece_in) {
    spinner_.set_thinking(false);
    spinner_.notify_token();
    ++stats_.content_pieces;
    if (stats_.ms_to_first_tok < 0) stats_.ms_to_first_tok = stats_.elapsed_ms();

    // Stateful filter: strip <think>/</think> markers from the content
    // stream and reroute any text between them through emit_reason_ so
    // it gets dim styling (or is dropped when --no-reasoning).  Defends
    // against servers/parsers that occasionally let the bare tags slip
    // into delta.content — typically the first piece after a tool call,
    // when the partial-parse state has just been reset.
    think_buf_ += piece_in;
    while (true) {
        if (in_think_) {
            std::size_t a = think_buf_.find("</think>");
            std::size_t b = think_buf_.find("</thinking>");
            std::size_t close = std::min(a, b);  // npos == -1 stays largest
            if (close == std::string::npos) {
                // No close marker yet.  Flush as reasoning everything
                // up to the last byte sequence that could still be the
                // start of a "</thinking>" / "</think>"; hold the rest.
                std::size_t hold = 0;
                for (std::size_t k = std::min<std::size_t>(think_buf_.size(), 11);
                     k > 0; --k) {
                    if (tail_is_partial_think_marker(
                            think_buf_.substr(think_buf_.size() - k))) {
                        hold = k;
                        break;
                    }
                }
                emit_reason_(think_buf_.substr(0, think_buf_.size() - hold));
                think_buf_.erase(0, think_buf_.size() - hold);
                return;
            }
            emit_reason_(think_buf_.substr(0, close));
            std::size_t end = think_buf_.find('>', close);
            if (end == std::string::npos) return;  // wait for tag close
            think_buf_.erase(0, end + 1);
            in_think_ = false;
        } else {
            std::size_t a = think_buf_.find("<think>");
            std::size_t b = think_buf_.find("<thinking>");
            std::size_t open = std::min(a, b);
            if (open == std::string::npos) {
                // No open marker.  Same trailing-bytes-might-be-tag dance.
                std::size_t hold = 0;
                for (std::size_t k = std::min<std::size_t>(think_buf_.size(), 10);
                     k > 0; --k) {
                    if (tail_is_partial_think_marker(
                            think_buf_.substr(think_buf_.size() - k))) {
                        hold = k;
                        break;
                    }
                }
                emit_content_(think_buf_.substr(0, think_buf_.size() - hold));
                think_buf_.erase(0, think_buf_.size() - hold);
                return;
            }
            emit_content_(think_buf_.substr(0, open));
            std::size_t end = think_buf_.find('>', open);
            if (end == std::string::npos) return;  // wait for tag close
            think_buf_.erase(0, end + 1);
            in_think_ = true;
        }
    }
}

void Streaming::on_reason_(const std::string & piece_in) {
    spinner_.set_thinking(false);
    spinner_.notify_token();
    ++stats_.reason_pieces;
    emit_reason_(strip_think_markers(piece_in));
}

void Streaming::on_tool_(const ToolCall & call, const ToolResult & result) {
    // A tool marker interrupts the content stream — render any buffered
    // table before the ●/✗ line so the table doesn't absorb it.
    flush_md_();
    spinner_.emit_speed_report();
    spinner_.set_thinking(false);
    ++stats_.tool_calls;
    if (result.is_error) ++stats_.tool_errors;

    const char * marker = result.is_error ? "✗" : "●";
    const char * color  = result.is_error ? style_.red() : style_.green();
    std::ostringstream ss;
    ss << "\n" << color << marker << " " << call.name;
    if (result.duration_ms >= 0) {
        ss << " " << style_.dim() << "(" << result.duration_ms << "ms)" << style_.reset()
           << color;
    }
    ss << "(" << easyai::text::trim_for_log(call.arguments_json, 80) << ")"
       << style_.reset();
    if (result.is_error) {
        ss << " " << style_.red()
           << easyai::text::trim_for_log(result.content, 100)
           << style_.reset();
    }
    ss << "\n";
    if (!result.display.empty() && style_.color) ss << result.display;
    spinner_.write(ss.str());

    if (verbose_) {
        easyai::log::write(
            "%s[tool %s name=%s args_bytes=%zu result_bytes=%zu +%ldms]%s\n",
            style_.dim(),
            result.is_error ? "FAIL" : "ok",
            call.name.c_str(),
            call.arguments_json.size(),
            result.content.size(),
            stats_.elapsed_ms(),
            style_.reset());
        std::string args_prev = easyai::text::trim_for_log(call.arguments_json, 240);
        easyai::log::write("%s         args=%s%s\n",
                           style_.dim(), args_prev.c_str(), style_.reset());
        std::string res_prev = easyai::text::trim_for_log(result.content, 240);
        easyai::log::write("%s         result=%s%s\n",
                           style_.dim(), res_prev.c_str(), style_.reset());
    }
}

void Streaming::notify_tool(const ToolCall & c, const ToolResult & r) {
    on_tool_(c, r);
}

Streaming & Streaming::attach(Engine & engine) {
    engine.on_token([this](const std::string & p){ this->on_token_(p); });
    engine.on_tool ([this](const ToolCall & c, const ToolResult & r){
        this->on_tool_(c, r);
    });
    return *this;
}

// Streaming::attach(Client &) lives in src/cli_client.cpp — libeasyai-cli.

Streaming & Streaming::attach(Plan & plan) {
    plan.on_change([this](const Plan & p){
        std::ostringstream ss;
        ss << "\n" << style_.yellow() << "── plan ──" << style_.reset() << "\n";
        p.render(ss, style_.color);
        ss << "\n";
        spinner_.write(ss.str());
    });
    return *this;
}

}  // namespace easyai::ui
