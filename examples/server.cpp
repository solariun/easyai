// =============================================================================
//  easyai-server — OpenAI-compatible HTTP server backed by an easyai::Engine.
//
//  Goals
//  -----
//   * Drop-in compatibility with anything that speaks /v1/chat/completions
//     (Claude Code's `--api-base`, OpenAI clients, LiteLLM, LangChain…).
//   * "Competitor" semantics: when the *client* posts its own `system` message
//     and `tools`, those win for that single request — the server-side defaults
//     are only used when the request omits them.
//   * Built-in toolbelt (datetime / web / fs / bash / rag) auto-
//     dispatched server-side when the client does NOT provide tools.
//   * Friendly preset commands ("precise", "creative 0.9", "/temp 0.5") inside
//     the user message — the server peels them off, applies them, then
//     forwards the rest of the message to the model.
//   * Tiny single-file webui at GET /  with a slider + preset buttons.
//
//  Memory hygiene
//  --------------
//   * One Engine, guarded by std::mutex so requests serialise cleanly across
//     httplib's worker threads (no concurrent llama_decode allowed).
//   * Per-request: history is *replaced*, not appended — no unbounded growth
//     across HTTP calls.
//   * Default tool list is owned by the Server (a vector<Tool> built once),
//     and we never share Tool objects across mutated requests; we copy the
//     vector contents into the engine before each call.
//   * Maximum request body size is clamped (default 8 MiB) so a hostile or
//     buggy client cannot RAM-bomb the process.
//   * Catches std::exception at every HTTP boundary so a malformed request
//     can never tear down the server.
//   * No raw new/delete anywhere in this file.  shared_ptr only where lambdas
//     actually need to extend lifetime past handler return.
// =============================================================================

#include "easyai/easyai.hpp"

// llama.cpp common — for incremental chat parsing during SSE streaming.
// Not part of easyai's public API; the server is allowed to reach in.
#include "chat.h"
#include "common.h"

#include "httplib.h"          // vendored by llama.cpp
#include "nlohmann/json.hpp"  // vendored by llama.cpp

#if defined(EASYAI_BUILD_WEBUI)
// Each *.hpp declares  unsigned char NAME[]  +  unsigned int NAME_len
// where NAME is the filename with '.' / '-' replaced by '_' (xxd convention).
#include "webui_index.html.hpp"
#include "webui_bundle.js.hpp"
#include "webui_bundle.css.hpp"
#include "webui_loading.html.hpp"
#endif

// Brand asset — the AI Box favicon, served at /favicon[.ico|.svg]. Kept
// inline as a raw string literal (not xxd-generated) so the bytes are
// auditable in source and the build skips one codegen step. The canonical
// copy lives at `webui/AI-brain.svg` for external use (docs, external
// rebrand, etc.); this constant is the build-baked source of truth.
constexpr std::string_view kBrandSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="-50 -50 300 300" width="300" height="300" role="img" aria-label="AI BOX">
  <title>AI BOX</title>
  <defs>
    <linearGradient id="ai_box_gradient" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0%"   stop-color="#1de9b6"/>
      <stop offset="100%" stop-color="#2196f3"/>
    </linearGradient>
    <filter id="ai_box_aura" x="-10%" y="-10%" width="120%" height="120%">
      <feGaussianBlur in="SourceAlpha" stdDeviation="1" result="haloBlur"/>
      <feFlood flood-color="#00bcd4" flood-opacity="0.6" result="haloColor"/>
      <feComposite in="haloColor" in2="haloBlur" operator="in" result="halo"/>
      <feMerge>
        <feMergeNode in="halo"/>
        <feMergeNode in="SourceGraphic"/>
      </feMerge>
    </filter>
  </defs>

  <g filter="url(#ai_box_aura)">
    <!-- Outer rounded-square ring (transparent middle via even-odd fill) -->
    <path fill="url(#ai_box_gradient)" fill-rule="evenodd" d="M 50 0 H 150 A 50 50 0 0 1 200 50 V 150 A 50 50 0 0 1 150 200 H 50 A 50 50 0 0 1 0 150 V 50 A 50 50 0 0 1 50 0 Z M 56 31 H 144 A 25 25 0 0 1 169 56 V 144 A 25 25 0 0 1 144 169 H 56 A 25 25 0 0 1 31 144 V 56 A 25 25 0 0 1 56 31 Z"/>

    <!-- Center mark -->
    <rect x="69" y="69" width="62" height="62" rx="13" ry="13" fill="url(#ai_box_gradient)"/>
  </g>
</svg>
)SVG";

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <dirent.h>      // opendir/readdir for fd-count
#include <sys/resource.h>// getrusage, RLIMIT_NOFILE
#include <sys/time.h>    // gettimeofday for /proc/stat deltas
#include <unistd.h>      // chdir
#include <vector>

// (chat.h pulls in nlohmann::json + a `using json = nlohmann::ordered_json`
// alias, so we don't redeclare those here.)
using nlohmann::ordered_json;

// ============================================================================
// Inline webui
// ----------------------------------------------------------------------------
// A self-contained <500-line single-file chat UI. Talks to /v1/chat/completions
// via the OpenAI shape (no streaming for simplicity — it polls one full reply
// per submit).  The preset bar dispatches to /v1/preset which sets server-wide
// defaults for *this UI session only*.
// ============================================================================
constexpr char kWebUI[] = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>__EASYAI_TITLE__</title>
<link rel="icon" href="/favicon">
<!-- Optional rich-rendering libs (used when reachable; fallback otherwise) -->
<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github-dark.min.css">
<style>
:root {
  --bg: #0b0d10;
  --bg-elev: #14181e;
  --bg-elev2: #1a1f27;
  --bg-input: #0f1318;
  --bg-hover: #1c2129;
  --border: #2a313b;
  --border-hi: #3a414b;
  --text: #e6edf3;
  --text-dim: #8b949e;
  --text-faint: #5b626a;
  --accent: #1f6feb;
  --accent-hi: #2f7ffb;
  --accent-dim: #1f6feb33;
  --tool: #4fb0ff;
  --tool-bg: #102230;
  --think: #b97df3;
  --think-bg: #1a1330;
  --good: #3fb950;
  --bad: #f85149;
  --warn: #d29922;
  --shadow: 0 4px 20px rgba(0,0,0,.4);
}
*, *::before, *::after { box-sizing: border-box; }
html, body { margin: 0; height: 100vh; overflow: hidden; }
body {
  font: 14px/1.5 -apple-system, BlinkMacSystemFont, "SF Pro Text", system-ui,
        "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  background: var(--bg);
  color: var(--text);
  -webkit-font-smoothing: antialiased;
}

/* ---------- layout shell ------------------------------------------------ */
#app { display: grid; grid-template-columns: 260px 1fr; height: 100vh; }
#app.no-sidebar { grid-template-columns: 0 1fr; }
@media (max-width: 760px) {
  #app { grid-template-columns: 0 1fr; }
  #app.show-sidebar { grid-template-columns: 240px 1fr; }
}

/* ---------- sidebar ----------------------------------------------------- */
aside#sidebar {
  background: var(--bg-elev);
  border-right: 1px solid var(--border);
  display: flex; flex-direction: column;
  overflow: hidden;
  transition: width .15s ease;
}
.no-sidebar #sidebar { display: none; }
@media (max-width: 760px) { #sidebar { display: none; } .show-sidebar #sidebar { display: flex; } }

#sidebar .head {
  padding: .55rem .55rem; border-bottom: 1px solid var(--border);
  display: flex; gap: .4rem; align-items: center;
}
#newChatBtn {
  flex: 1; background: transparent; color: var(--text);
  border: 1px dashed var(--border-hi); border-radius: 6px;
  padding: .45rem .6rem; font: inherit; cursor: pointer;
  display: flex; align-items: center; gap: .4rem;
  transition: background .12s, border-color .12s;
}
#newChatBtn:hover { background: var(--bg-hover); border-color: var(--accent); }
#newChatBtn .plus { font-weight: 700; color: var(--accent); }

#convList { list-style: none; margin: 0; padding: .35rem; overflow-y: auto; flex: 1; }
.conv-item {
  display: flex; align-items: center; gap: .4rem;
  padding: .45rem .55rem; border-radius: 6px;
  cursor: pointer; color: var(--text-dim); font-size: .85rem;
  position: relative;
}
.conv-item:hover { background: var(--bg-hover); color: var(--text); }
.conv-item.active { background: var(--accent-dim); color: var(--text); }
.conv-item .title { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.conv-item .stamp { font-size: .68rem; color: var(--text-faint); flex-shrink: 0; }
.conv-item .del {
  display: none; background: none; border: 0; color: var(--text-dim);
  font-size: .9rem; cursor: pointer; padding: 0 .2rem; line-height: 1;
}
.conv-item:hover .del { display: inline-block; }
.conv-item .del:hover { color: var(--bad); }

#sidebar .foot {
  border-top: 1px solid var(--border); padding: .45rem .55rem;
  font-size: .7rem; color: var(--text-faint);
  display: flex; align-items: center; gap: .4rem;
}

/* ---------- main / header ----------------------------------------------- */
main { display: flex; flex-direction: column; min-width: 0; }
header.topbar {
  padding: .55rem .85rem;
  background: var(--bg-elev);
  border-bottom: 1px solid var(--border);
  display: flex; align-items: center; gap: .55rem; flex-wrap: wrap;
}
header.topbar h1 { font-size: 1rem; margin: 0; font-weight: 600; letter-spacing: .01em; }
.pill {
  font-size: .7rem; color: var(--text-dim);
  padding: .15rem .55rem; border: 1px solid var(--border); border-radius: 999px;
  white-space: nowrap;
}
.icon-btn {
  background: transparent; color: var(--text-dim); border: 1px solid transparent;
  border-radius: 6px; padding: .25rem .45rem; cursor: pointer; font-size: 1rem;
  transition: background .12s, color .12s, border-color .12s;
}
.icon-btn:hover { background: var(--bg-hover); color: var(--text); border-color: var(--border); }
.spacer { flex: 1; }

/* ---------- chat area --------------------------------------------------- */
#chat { flex: 1; overflow-y: auto; padding: 1.2rem 0 .8rem; }
.empty {
  height: 100%; display: flex; flex-direction: column; align-items: center; justify-content: center;
  color: var(--text-faint); padding: 2rem; text-align: center; gap: .6rem;
}
.empty h2 { color: var(--text-dim); font-weight: 500; margin: 0; }
.empty .ex { display: flex; flex-wrap: wrap; gap: .4rem; justify-content: center; max-width: 540px; }
.empty .ex button {
  background: var(--bg-elev); color: var(--text-dim);
  border: 1px solid var(--border); border-radius: 999px;
  padding: .35rem .8rem; font-size: .8rem; cursor: pointer;
}
.empty .ex button:hover { color: var(--text); border-color: var(--accent); }

.msg { max-width: 78ch; margin: 0 auto 1.4rem; padding: 0 1.2rem; }
.msg .role {
  font-size: .68rem; color: var(--text-faint); text-transform: uppercase;
  letter-spacing: .08em; margin-bottom: .25rem;
  display: flex; align-items: center; gap: .5rem;
}
.msg .role .actions { margin-left: auto; opacity: 0; transition: opacity .12s; }
.msg:hover .role .actions { opacity: 1; }
.msg .role .actions button {
  background: none; border: 0; color: var(--text-dim); padding: 0 .3rem;
  cursor: pointer; font-size: .75rem; border-radius: 3px;
}
.msg .role .actions button:hover { color: var(--text); background: var(--bg-hover); }

.msg.user .body {
  background: #0d2543; border: 1px solid #1f4a77;
  padding: .6rem .85rem; border-radius: 8px;
  white-space: pre-wrap; word-wrap: break-word;
}

.msg.assistant .content { padding: .15rem 0; word-wrap: break-word; overflow-wrap: anywhere; }
.msg.assistant .content p { margin: .35rem 0; }
.msg.assistant .content p:first-child { margin-top: 0; }
.msg.assistant .content p:last-child  { margin-bottom: 0; }
.msg.assistant .content h1, .msg.assistant .content h2, .msg.assistant .content h3 {
  font-size: 1rem; margin: 1rem 0 .35rem; font-weight: 600;
}
.msg.assistant .content h1 { font-size: 1.15rem; }
.msg.assistant .content h2 { font-size: 1.05rem; }
.msg.assistant .content ul, .msg.assistant .content ol { padding-left: 1.4rem; margin: .35rem 0; }
.msg.assistant .content li { margin: .15rem 0; }
.msg.assistant .content blockquote {
  border-left: 3px solid var(--border); margin: .4rem 0; padding: .1rem .8rem; color: var(--text-dim);
}
.msg.assistant .content pre {
  background: #0a0d11; border: 1px solid var(--border);
  padding: .65rem .8rem; border-radius: 6px;
  overflow-x: auto; font-size: .85em; line-height: 1.45; margin: .5rem 0;
}
.msg.assistant .content code {
  background: var(--bg-input); padding: .1rem .35rem;
  border-radius: 3px; font-size: .88em;
  font-family: ui-monospace, "SF Mono", Menlo, monospace;
}
.msg.assistant .content pre code { background: transparent; padding: 0; font-size: 1em; }
.msg.assistant .content a { color: var(--accent); text-decoration: none; }
.msg.assistant .content a:hover { text-decoration: underline; }
.msg.assistant .content table { border-collapse: collapse; margin: .5rem 0; }
.msg.assistant .content th, .msg.assistant .content td {
  border: 1px solid var(--border); padding: .25rem .5rem; font-size: .9em;
}
.msg.assistant .content th { background: var(--bg-elev); font-weight: 600; }

/* thinking + tool cards keep their own visual identity */
.thinking {
  border: 1px solid var(--think-bg); background: var(--think-bg);
  border-left: 3px solid var(--think); border-radius: 6px;
  margin: .5rem 0; padding: .35rem .6rem; font-size: .85em; color: var(--text-dim);
}
.thinking summary {
  cursor: pointer; color: var(--think); font-weight: 600;
  outline: none; user-select: none; list-style: none;
}
.thinking summary::-webkit-details-marker { display: none; }
.thinking summary::before { content: "▸ "; color: var(--think); font-weight: 700; }
.thinking[open] summary::before { content: "▾ "; }
.thinking[open] summary { margin-bottom: .4rem; }
.thinking .think-body {
  white-space: pre-wrap; font-family: ui-monospace, "SF Mono", Menlo, monospace;
  font-size: .82rem; line-height: 1.5; max-height: 24em; overflow-y: auto;
}

.tool-card {
  border: 1px solid var(--tool-bg); background: var(--tool-bg);
  border-left: 3px solid var(--tool); border-radius: 6px;
  margin: .5rem 0; padding: .45rem .65rem;
  font-family: ui-monospace, "SF Mono", Menlo, monospace; font-size: .8rem;
}
.tool-card.error { border-left-color: var(--bad); background: #22151a; }
.tool-card .head { display: flex; align-items: baseline; gap: .4rem; flex-wrap: wrap; }
.tool-card .name { color: var(--tool); font-weight: 600; }
.tool-card.error .name { color: var(--bad); }
.tool-card .args { color: var(--text-dim); word-break: break-all; }
.tool-card .result {
  color: var(--text); margin-top: .35rem; white-space: pre-wrap;
  max-height: 16em; overflow-y: auto; padding-top: .35rem;
  border-top: 1px dashed var(--border);
}
.tool-card .result.pending { color: var(--text-dim); font-style: italic; }

.cursor {
  display: inline-block; width: .5em; height: 1em;
  background: var(--text); animation: blink 1s steps(1, end) infinite;
  vertical-align: text-bottom; margin-left: 1px;
}
@keyframes blink { 50% { opacity: 0; } }

.stats {
  font-size: .68rem; color: var(--text-faint); margin-top: .35rem;
  font-family: ui-monospace, "SF Mono", Menlo, monospace;
}
.error-msg {
  color: var(--bad); background: #22151a;
  border: 1px solid #6a2a31; border-radius: 6px;
  padding: .5rem .7rem; font-size: .9em;
}

/* ---------- input bar --------------------------------------------------- */
#inputWrap {
  border-top: 1px solid var(--border); background: var(--bg-elev);
  padding: .55rem .85rem .7rem; position: relative;
}
#inputBar { display: flex; gap: .45rem; align-items: flex-end; }
#textArea {
  flex: 1; resize: none; min-height: 2.4rem; max-height: 30vh;
  padding: .55rem .7rem;
  background: var(--bg-input); color: var(--text);
  border: 1px solid var(--border); border-radius: 8px;
  font: inherit; line-height: 1.5;
}
#textArea:focus { outline: 2px solid var(--accent-dim); border-color: var(--accent); }
#sendBtn, #stopBtn {
  background: var(--accent); color: #fff; border: 0;
  border-radius: 8px; padding: 0 1.05rem; cursor: pointer; font-weight: 600;
  height: 2.4rem; align-self: flex-end;
}
#sendBtn:hover { background: var(--accent-hi); }
#sendBtn:disabled { opacity: .55; cursor: wait; }
#stopBtn { background: var(--bad); }
#stopBtn:hover { filter: brightness(1.1); }
#settingsToggle, #sidebarToggle {
  background: var(--bg-input); color: var(--text-dim);
  border: 1px solid var(--border); border-radius: 8px;
  padding: 0 .65rem; cursor: pointer; font-size: 1rem;
  height: 2.4rem; align-self: flex-end;
}
#settingsToggle:hover, #sidebarToggle:hover { background: var(--bg-hover); color: var(--text); }
#settingsToggle.active { color: var(--accent); border-color: var(--accent); }

#settingsHint {
  color: var(--text-faint); font-size: .68rem; padding-top: .3rem;
  display: flex; gap: 1rem; flex-wrap: wrap;
}
#settingsHint .pill { font-size: .65rem; padding: .05rem .45rem; cursor: pointer; }
#settingsHint .pill:hover { color: var(--text); border-color: var(--border-hi); }

/* settings popover */
#settingsPanel {
  position: absolute; bottom: calc(100% + 2px); right: .85rem;
  width: min(380px, calc(100vw - 1.7rem));
  background: var(--bg-elev2); color: var(--text);
  border: 1px solid var(--border-hi); border-radius: 10px;
  box-shadow: var(--shadow); padding: .7rem .85rem;
  display: none; z-index: 50;
}
#settingsPanel.open { display: block; }
#settingsPanel h3 {
  font-size: .82rem; margin: 0 0 .55rem; font-weight: 600;
  color: var(--text-dim); text-transform: uppercase; letter-spacing: .06em;
}
.preset-row {
  display: flex; flex-wrap: wrap; gap: .25rem;
  margin-bottom: .65rem;
}
.preset-row button {
  background: transparent; color: var(--text-dim);
  border: 1px solid var(--border); border-radius: 999px;
  padding: .25rem .65rem; cursor: pointer; font-size: .75rem;
}
.preset-row button:hover { background: var(--bg-hover); color: var(--text); }
.preset-row button.active { background: var(--accent); border-color: var(--accent); color: white; }
.slider-row {
  display: grid; grid-template-columns: 80px 1fr 56px;
  gap: .55rem; align-items: center; margin: .35rem 0;
  font-size: .82rem;
}
.slider-row label { color: var(--text-dim); }
.slider-row input[type=range] { width: 100%; accent-color: var(--accent); }
.slider-row input[type=number] {
  width: 100%; background: var(--bg-input); color: var(--text);
  border: 1px solid var(--border); border-radius: 4px; padding: .15rem .3rem;
  font: inherit; font-size: .82rem; text-align: right;
}
#settingsPanel .reset-link {
  font-size: .72rem; color: var(--text-dim); text-decoration: underline;
  background: none; border: 0; cursor: pointer; padding: 0; margin-top: .3rem;
}

/* small helper: invisible on narrow screens */
.hide-narrow { display: inline-flex; }
@media (max-width: 760px) { .hide-narrow { display: none; } }
</style></head>
<body>
<div id="app" class="show-sidebar">
  <aside id="sidebar">
    <div class="head">
      <button id="newChatBtn"><span class="plus">+</span> New chat</button>
    </div>
    <ul id="convList"></ul>
    <div class="foot">
      <span id="model" class="pill">…</span>
      <span id="backend" class="pill"></span>
      <span id="ntools" class="pill"></span>
    </div>
  </aside>

  <main>
    <header class="topbar">
      <button class="icon-btn" id="sidebarToggle" title="Toggle sidebar">☰</button>
      <h1>__EASYAI_TITLE__</h1>
      <span class="spacer"></span>
      <button class="icon-btn hide-narrow" id="thinkToggleHdr" title="Show / hide reasoning blocks">◐ thinking</button>
    </header>

    <div id="chat"></div>

    <div id="inputWrap">
      <div id="settingsPanel">
        <h3>preset</h3>
        <div class="preset-row" id="presetRow">
          <button data-p="deterministic">deterministic</button>
          <button data-p="precise" class="active">precise</button>
          <button data-p="balanced">balanced</button>
          <button data-p="creative">creative</button>
          <button data-p="wild">wild</button>
        </div>
        <h3>sampling</h3>
        <div class="slider-row">
          <label for="sTemp">temperature</label>
          <input type="range" id="sTemp" min="0" max="2" step="0.05" value="0.7">
          <input type="number" id="sTempN" min="0" max="2" step="0.05" value="0.7">
        </div>
        <div class="slider-row">
          <label for="sTopP">top_p</label>
          <input type="range" id="sTopP" min="0" max="1" step="0.01" value="0.95">
          <input type="number" id="sTopPN" min="0" max="1" step="0.01" value="0.95">
        </div>
        <div class="slider-row">
          <label for="sTopK">top_k</label>
          <input type="range" id="sTopK" min="1" max="100" step="1" value="40">
          <input type="number" id="sTopKN" min="1" max="100" step="1" value="40">
        </div>
        <div class="slider-row">
          <label for="sMinP">min_p</label>
          <input type="range" id="sMinP" min="0" max="0.5" step="0.01" value="0.05">
          <input type="number" id="sMinPN" min="0" max="0.5" step="0.01" value="0.05">
        </div>
        <div class="slider-row">
          <label for="sMaxTok">max_tokens</label>
          <input type="range" id="sMaxTok" min="0" max="4096" step="64" value="0">
          <input type="number" id="sMaxTokN" min="0" max="32768" step="1" value="0">
        </div>
        <button class="reset-link" id="settingsReset">reset to balanced</button>
      </div>

      <div id="inputBar">
        <button id="settingsToggle" title="Sampling settings">⚙</button>
        <textarea id="textArea" rows="1" placeholder="Type a message…"></textarea>
        <button id="stopBtn" hidden>stop</button>
        <button id="sendBtn">send</button>
      </div>

      <div id="settingsHint">
        <span>Ctrl/⌘+Enter to send</span>
        <span class="pill" data-quick="0">temp 0.0 (deterministic)</span>
        <span class="pill" data-quick="0.7">temp 0.7 (balanced)</span>
        <span class="pill" data-quick="1.0">temp 1.0 (creative)</span>
        <span style="color:var(--text-faint)">or inline: <code>creative 0.9 …</code></span>
      </div>
    </div>
  </main>
</div>

<!-- markdown lib (loaded async, with graceful fallback) -->
<script src="https://cdn.jsdelivr.net/npm/marked@12/marked.min.js" defer></script>
<script src="https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/highlight.min.js" defer></script>

<script>
"use strict";

// ============================================================================
//  helpers
// ============================================================================
const $  = (s, r=document) => r.querySelector(s);
const $$ = (s, r=document) => Array.from(r.querySelectorAll(s));
const escHTML = s => String(s).replace(/[&<>"']/g, c => ({
  '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'
}[c]));

function relTime(ts){
  const s = (Date.now() - ts) / 1000;
  if (s < 60)        return Math.floor(s) + 's';
  if (s < 3600)      return Math.floor(s/60) + 'm';
  if (s < 86400)     return Math.floor(s/3600) + 'h';
  if (s < 86400*7)   return Math.floor(s/86400) + 'd';
  return new Date(ts).toLocaleDateString();
}

// Tiny markdown fallback used when marked.js failed to load (offline boxes).
function tinyMD(s){
  if (!s) return '';
  let html = escHTML(s);
  const parked = [];
  const park = f => (parked.push(f), `\u0001${parked.length-1}\u0001`);
  html = html.replace(/```([a-zA-Z0-9_-]*)\n([\s\S]*?)```/g, (_, l, c) =>
    park(`<pre><code class="lang-${escHTML(l)}">${c}</code></pre>`));
  html = html.replace(/`([^`\n]+)`/g, (_, c) => park(`<code>${c}</code>`));
  html = html.replace(/\[([^\]]+)\]\(([^)\s]+)\)/g, (_, t, u) =>
    park(`<a href="${u}" target="_blank" rel="noopener">${t}</a>`));
  html = html.replace(/\*\*([^\*\n]+?)\*\*/g, '<strong>$1</strong>');
  html = html.replace(/(^|[^\*])\*([^\*\n]+?)\*(?!\*)/g, '$1<em>$2</em>');
  html = html.replace(/(\bhttps?:\/\/[^\s<]+?)([.,;:!?)\]]*)(?=\s|$)/g,
    (_, u, t) => park(`<a href="${u}" target="_blank" rel="noopener">${u}</a>`) + t);
  html = html.replace(/\n/g, '<br>');
  html = html.replace(/\u0001(\d+)\u0001/g, (_, i) => parked[+i]);
  return html;
}

function renderMD(s){
  if (window.marked) {
    try {
      // Configure once.
      if (!window._mdReady) {
        window.marked.setOptions({ breaks: true, gfm: true });
        window._mdReady = true;
      }
      let html = window.marked.parse(s || '');
      // Run highlight.js on freshly built code blocks.
      if (window.hljs) {
        const tmp = document.createElement('div');
        tmp.innerHTML = html;
        $$('pre code', tmp).forEach(b => {
          try { window.hljs.highlightElement(b); } catch {}
        });
        html = tmp.innerHTML;
      }
      return html;
    } catch (e) {
      console.warn('markdown fallback', e);
    }
  }
  return tinyMD(s);
}

// ============================================================================
//  IndexedDB-backed conversation storage
// ============================================================================
const DB_NAME = 'easyai';
const STORE   = 'conversations';

function openDB(){
  return new Promise((res, rej) => {
    const req = indexedDB.open(DB_NAME, 1);
    req.onupgradeneeded = () => req.result.createObjectStore(STORE, { keyPath: 'id' });
    req.onsuccess = () => res(req.result);
    req.onerror   = () => rej(req.error);
  });
}
async function dbAll(){
  const db = await openDB();
  return new Promise((res, rej) => {
    const tx = db.transaction(STORE, 'readonly');
    const req = tx.objectStore(STORE).getAll();
    req.onsuccess = () => res(req.result || []);
    req.onerror   = () => rej(req.error);
  });
}
async function dbPut(conv){
  const db = await openDB();
  return new Promise((res, rej) => {
    const tx = db.transaction(STORE, 'readwrite');
    tx.objectStore(STORE).put(conv);
    tx.oncomplete = () => res();
    tx.onerror    = () => rej(tx.error);
  });
}
async function dbDel(id){
  const db = await openDB();
  return new Promise((res, rej) => {
    const tx = db.transaction(STORE, 'readwrite');
    tx.objectStore(STORE).delete(id);
    tx.oncomplete = () => res();
    tx.onerror    = () => rej(tx.error);
  });
}

// ============================================================================
//  app state
// ============================================================================
const state = {
  convs:        [],           // [{id, title, created, updated, messages, settings}]
  currentId:    null,
  showThinking: true,
  abort:        null,         // current AbortController
  settings: loadSettings(),   // sampling defaults persisted in localStorage
};

const PRESETS = {
  deterministic: { temperature: 0.0, top_p: 1.0, top_k: 1,  min_p: 0.0  },
  precise:       { temperature: 0.2, top_p: 0.95, top_k: 40, min_p: 0.10 },
  balanced:      { temperature: 0.7, top_p: 0.95, top_k: 40, min_p: 0.05 },
  creative:      { temperature: 1.0, top_p: 0.95, top_k: 40, min_p: 0.05 },
  wild:          { temperature: 1.4, top_p: 0.98, top_k: 60, min_p: 0.0  },
};

function loadSettings(){
  try {
    const j = JSON.parse(localStorage.getItem('easyai-settings') || '{}');
    return Object.assign({
      preset:       'precise',
      temperature:  PRESETS.precise.temperature,
      top_p:        PRESETS.precise.top_p,
      top_k:        PRESETS.precise.top_k,
      min_p:        PRESETS.precise.min_p,
      max_tokens:   0,            // 0 = no client cap
    }, j);
  } catch { return loadDefaultSettings(); }
}
function loadDefaultSettings(){
  return Object.assign({ preset:'precise', max_tokens: 0 }, PRESETS.precise);
}
function saveSettings(){ localStorage.setItem('easyai-settings', JSON.stringify(state.settings)); }

function newConv(){
  return {
    id: (crypto && crypto.randomUUID) ? crypto.randomUUID() : (Date.now() + '-' + Math.random()),
    title: 'New chat',
    created: Date.now(),
    updated: Date.now(),
    messages: [],
  };
}

async function loadAllConvs(){
  state.convs = (await dbAll().catch(() => [])).sort((a,b) => b.updated - a.updated);
  if (state.convs.length === 0) {
    const c = newConv();
    state.convs.push(c);
    state.currentId = c.id;
    await dbPut(c).catch(()=>{});
  } else {
    state.currentId = state.convs[0].id;
  }
}

const currentConv = () => state.convs.find(c => c.id === state.currentId);

// ============================================================================
//  rendering — sidebar
// ============================================================================
function renderSidebar(){
  const list = $('#convList');
  list.innerHTML = '';
  for (const c of state.convs) {
    const li = document.createElement('li');
    li.className = 'conv-item' + (c.id === state.currentId ? ' active' : '');
    li.innerHTML = `
      <span class="title"></span>
      <span class="stamp"></span>
      <button class="del" title="Delete">×</button>`;
    $('.title', li).textContent = c.title || 'untitled';
    $('.stamp', li).textContent = relTime(c.updated);
    li.onclick = e => {
      if (e.target.classList.contains('del')) return;
      switchConv(c.id);
    };
    $('.del', li).onclick = async () => {
      if (!confirm('Delete this conversation?')) return;
      await dbDel(c.id).catch(()=>{});
      state.convs = state.convs.filter(x => x.id !== c.id);
      if (state.currentId === c.id) {
        if (state.convs.length === 0) {
          const nc = newConv(); state.convs.push(nc); state.currentId = nc.id;
          await dbPut(nc).catch(()=>{});
        } else state.currentId = state.convs[0].id;
      }
      renderSidebar(); renderChat();
    };
    list.appendChild(li);
  }
}

function switchConv(id){
  if (state.abort) state.abort.abort();
  state.currentId = id;
  renderSidebar();
  renderChat();
}

async function startNewConv(){
  if (state.abort) state.abort.abort();
  const c = newConv();
  state.convs.unshift(c);
  state.currentId = c.id;
  await dbPut(c).catch(()=>{});
  renderSidebar();
  renderChat();
  $('#textArea').focus();
}

// ============================================================================
//  rendering — chat
// ============================================================================
const chatEl = $('#chat');

function renderChat(){
  const c = currentConv();
  chatEl.innerHTML = '';
  if (!c || c.messages.length === 0) {
    chatEl.innerHTML = `
      <div class="empty">
        <h2>How can I help?</h2>
        <div class="ex">
          <button data-pp="What time is it now?">What time is it now?</button>
          <button data-pp="Search the web for the latest llama.cpp release and summarise.">Latest llama.cpp release</button>
          <button data-pp="creative 0.9 write me a haiku about silicon">creative 0.9 haiku</button>
          <button data-pp="List the files in the current directory.">List files</button>
        </div>
      </div>`;
    $$('.empty .ex button').forEach(b => b.onclick = () => {
      $('#textArea').value = b.dataset.pp;
      $('#textArea').focus();
    });
    return;
  }
  for (const m of c.messages) renderMessage(m);
  chatEl.scrollTop = chatEl.scrollHeight;
}

function appendUserMsg(text){
  const el = document.createElement('div');
  el.className = 'msg user';
  el.innerHTML = `<div class="role">you</div><div class="body"></div>`;
  $('.body', el).textContent = text;
  chatEl.appendChild(el);
  chatEl.scrollTop = chatEl.scrollHeight;
  return el;
}

function renderMessage(m){
  if (m.role === 'user') {
    appendUserMsg(m.content);
    return;
  }
  if (m.role === 'assistant') {
    const turn = new AssistantTurn(state.showThinking, /*live=*/false);
    turn.raw = m.content || '';
    if (m.toolCards) {
      for (const t of m.toolCards) turn.replayToolCard(t);
    }
    turn.finish('stop');
  }
}

// ============================================================================
//  AssistantTurn — one streaming reply
// ============================================================================
class AssistantTurn {
  constructor(showThinking, live=true){
    this.raw = '';
    this.tokens = 0;
    this.startMs = performance.now();
    this.showThinking = showThinking;
    this.toolCards = [];          // {name, args, result, error}
    this.pendingByName = new Map();
    this._done = !live;

    this.el = document.createElement('div');
    this.el.className = 'msg assistant';
    this.el.innerHTML = `
      <div class="role">
        <span>assistant</span>
        <span class="actions">
          <button class="copy" title="Copy">copy</button>
        </span>
      </div>
      <div class="content"></div>
      <div class="stats"></div>`;
    this.contentEl = $('.content', this.el);
    this.statsEl   = $('.stats',   this.el);
    chatEl.appendChild(this.el);
    chatEl.scrollTop = chatEl.scrollHeight;

    $('.copy', this.el).onclick = () => {
      const t = this.finalText();
      navigator.clipboard?.writeText(t).catch(()=>{});
    };
  }

  addContent(piece){
    this.tokens += 1;
    this.raw += piece;
    this.render();
  }

  render(){
    // Split out <think>...</think> blocks; render content with markdown.
    let cleaned = '';
    const thinks = [];
    const re = /<think(?:ing)?>([\s\S]*?)(?:<\/think(?:ing)?>|$)/g;
    let m, last = 0;
    while ((m = re.exec(this.raw)) !== null) {
      cleaned += this.raw.slice(last, m.index);
      thinks.push(m[1]);
      last = m.index + m[0].length;
    }
    cleaned += this.raw.slice(last);

    let thinkEl = $(':scope > .thinking', this.el);
    if (thinks.length) {
      const txt = thinks.join('\n\n— —\n\n');
      if (!thinkEl) {
        thinkEl = document.createElement('details');
        thinkEl.className = 'thinking';
        if (this.showThinking) thinkEl.open = true;
        thinkEl.innerHTML = '<summary>Thinking</summary><div class="think-body"></div>';
        this.el.insertBefore(thinkEl, this.contentEl);
      }
      $('.think-body', thinkEl).textContent = txt;
    }

    this.contentEl.innerHTML = renderMD(cleaned) +
        (this._done ? '' : '<span class="cursor"></span>');
    chatEl.scrollTop = chatEl.scrollHeight;
  }

  // server-side prompt-eval completion (mirrors llama-server's
  // "prompt eval time" stderr line).  Renders a single dim line
  // above the content area so the user sees the moment ingestion
  // finished and generation starts.  Replaces a previous prompt-
  // eval line in the same turn (multi-hop turns get one event per
  // generate() call; only the most recent matters for the user).
  addPromptEval(evt){
    let el = $(':scope > .prompt-eval', this.el);
    if (!el) {
      el = document.createElement('div');
      el.className = 'prompt-eval';
      el.style.cssText = 'margin:.4rem 0;padding:.2rem .55rem;'
        + 'border-left:2px solid #d29922;'
        + 'background:rgba(210,153,34,.08);'
        + 'border-radius:4px;font-size:.75rem;color:#d29922;'
        + 'font-family:ui-monospace,SFMono-Regular,Menlo,monospace;';
      this.el.insertBefore(el, this.contentEl);
    }
    const tps = evt.tokens_per_second || 0;
    const ms  = evt.prompt_ms || 0;
    const n   = evt.n_tokens   || 0;
    const cached = evt.n_cached || 0;
    el.textContent = '📝 prompt eval: ' + n + ' tok'
      + (cached > 0 ? ' (' + cached + ' cached)' : '')
      + ' · ' + Math.round(ms) + ' ms · ' + tps.toFixed(1) + ' t/s';
  }

  // server-side tool dispatch — card with running placeholder
  addToolCall(evt){
    const card = document.createElement('div');
    card.className = 'tool-card';
    let argStr = '';
    try { argStr = JSON.stringify(JSON.parse(evt.arguments)); }
    catch { argStr = evt.arguments || ''; }
    card.innerHTML = `
      <div class="head">
        <span class="name"></span>
        <span class="args"></span>
      </div>
      <div class="result pending">…running…</div>`;
    $('.name', card).textContent = evt.name;
    $('.args', card).textContent = '(' + argStr + ')';
    this.el.insertBefore(card, this.contentEl);
    this.toolCards.push({ el: card, name: evt.name, args: argStr });
    if (!this.pendingByName.has(evt.name)) this.pendingByName.set(evt.name, []);
    this.pendingByName.get(evt.name).push(card);
    chatEl.scrollTop = chatEl.scrollHeight;
  }

  addToolResult(evt){
    const stack = this.pendingByName.get(evt.name);
    if (!stack || !stack.length) return;
    const card = stack.shift();
    const resEl = $('.result', card);
    resEl.classList.remove('pending');
    resEl.textContent = evt.content || '';
    if (evt.is_error) card.classList.add('error');
    // Keep a record for replay/persistence.
    const rec = this.toolCards.find(t => t.el === card);
    if (rec) { rec.result = evt.content || ''; rec.error = !!evt.is_error; }
  }

  // For client-tools mode (rare from the webui).
  addClientToolCalls(tcs){
    for (const tc of tcs) {
      if (!tc.function) continue;
      this.addToolCall({
        name: tc.function.name || '?',
        arguments: tc.function.arguments || '{}',
        id: tc.id || ''
      });
      const stack = this.pendingByName.get(tc.function.name);
      if (stack && stack.length) {
        const card = stack.shift();
        $('.result', card).classList.remove('pending');
        $('.result', card).textContent = '(forwarded to client for execution)';
        $('.result', card).style.fontStyle = 'italic';
      }
    }
  }

  // Used during conversation replay (loading from IDB).
  replayToolCard(t){
    const card = document.createElement('div');
    card.className = 'tool-card' + (t.error ? ' error' : '');
    card.innerHTML = `
      <div class="head">
        <span class="name"></span>
        <span class="args"></span>
      </div>
      <div class="result"></div>`;
    $('.name', card).textContent = t.name;
    $('.args', card).textContent = '(' + (t.args || '') + ')';
    $('.result', card).textContent = t.result || '';
    this.el.insertBefore(card, this.contentEl);
    this.toolCards.push(t);
  }

  finish(reason){
    this._done = true;
    const elapsed = (performance.now() - this.startMs) / 1000;
    if (this.tokens > 0) {
      const tps = this.tokens / Math.max(elapsed, 0.001);
      this.statsEl.textContent = `${this.tokens} chunks · ${elapsed.toFixed(2)}s · ${tps.toFixed(1)} chunks/s · ${reason}`;
    }
    this.render();
  }
  fail(msg){
    this._done = true;
    this.contentEl.innerHTML = `<div class="error-msg">⚠︎ ${escHTML(msg)}</div>`;
  }

  // Strip <think> blocks — what we save into history for the next request.
  finalText(){
    return this.raw.replace(/<think(?:ing)?>[\s\S]*?<\/think(?:ing)?>/g, '').trim();
  }
}

// ============================================================================
//  SSE consumer
// ============================================================================
async function streamChat(messages, settings, handlers, signal){
  const body = {
    model: 'easyai',
    messages,
    stream: true,
    temperature: settings.temperature,
    top_p:       settings.top_p,
    top_k:       settings.top_k,
    min_p:       settings.min_p,
  };
  if (settings.max_tokens > 0) body.max_tokens = settings.max_tokens;

  const res = await fetch('/v1/chat/completions', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
    signal,
  });
  if (!res.ok) {
    const txt = await res.text();
    throw new Error(`HTTP ${res.status}: ${txt.slice(0, 400)}`);
  }
  const reader = res.body.getReader();
  const decoder = new TextDecoder();
  let buf = '';
  while (true) {
    const { value, done } = await reader.read();
    if (done) break;
    buf += decoder.decode(value, { stream: true });
    while (true) {
      const idx = buf.indexOf('\n\n');
      if (idx === -1) break;
      const event = buf.slice(0, idx);
      buf = buf.slice(idx + 2);
      let evtType = 'message';
      const dataLines = [];
      for (let line of event.split('\n')) {
        if (line.endsWith('\r')) line = line.slice(0, -1);
        if (line.startsWith('event:')) evtType = line.slice(6).trim();
        else if (line.startsWith('data:')) dataLines.push(line.slice(5).replace(/^ /, ''));
      }
      const data = dataLines.join('\n');
      if (!data) continue;
      if (data === '[DONE]') return;
      try {
        const j = JSON.parse(data);
        if (evtType === 'easyai.tool_call')   { handlers.onToolCall && handlers.onToolCall(j); continue; }
        if (evtType === 'easyai.tool_result') { handlers.onToolResult && handlers.onToolResult(j); continue; }
        if (evtType === 'easyai.prompt_eval') { handlers.onPromptEval && handlers.onPromptEval(j); continue; }
        const ch = j?.choices?.[0];
        if (!ch) continue;
        if (ch.delta?.content)    handlers.onContent && handlers.onContent(ch.delta.content);
        if (ch.delta?.tool_calls) handlers.onClientToolCalls && handlers.onClientToolCalls(ch.delta.tool_calls);
        if (ch.finish_reason)     handlers.onFinish && handlers.onFinish(ch.finish_reason);
        if (j.error) throw new Error(j.error.message || 'server error');
      } catch (e) {
        if (e.name === 'AbortError') throw e;
        console.warn('SSE parse', data, e);
      }
    }
  }
}

// ============================================================================
//  send pipeline
// ============================================================================
async function send(text){
  const c = currentConv();
  if (!c) return;
  c.messages.push({ role: 'user', content: text });
  if (c.messages.length === 1) c.title = (text.slice(0, 40) || 'New chat');
  c.updated = Date.now();
  appendUserMsg(text);
  // remove the empty placeholder if present
  $$('.empty', chatEl).forEach(e => e.remove());
  await dbPut(c).catch(()=>{});
  renderSidebar();

  const turn = new AssistantTurn(state.showThinking);
  $('#sendBtn').hidden = true;
  $('#stopBtn').hidden = false;
  state.abort = new AbortController();

  // Build the API messages: include past assistant content sans tool cards.
  const apiMsgs = c.messages.map(m =>
    m.role === 'assistant' ? { role: 'assistant', content: m.content || '' }
                           : { role: m.role, content: m.content });

  try {
    await streamChat(apiMsgs, state.settings, {
      onContent:         p => turn.addContent(p),
      onToolCall:        e => turn.addToolCall(e),
      onToolResult:      e => turn.addToolResult(e),
      onPromptEval:      e => turn.addPromptEval(e),
      onClientToolCalls: t => turn.addClientToolCalls(t),
      onFinish:          r => turn.finish(r),
    }, state.abort.signal);
    c.messages.push({
      role: 'assistant',
      content: turn.finalText(),
      toolCards: turn.toolCards.map(t => ({ name: t.name, args: t.args, result: t.result, error: t.error })),
    });
    c.updated = Date.now();
    await dbPut(c).catch(()=>{});
    renderSidebar();
  } catch (e) {
    if (e.name === 'AbortError') {
      turn.fail('stopped');
    } else {
      turn.fail(e.message || String(e));
    }
  } finally {
    $('#sendBtn').hidden = false;
    $('#stopBtn').hidden = true;
    state.abort = null;
    $('#textArea').focus();
  }
}

function stopGeneration(){
  if (state.abort) state.abort.abort();
}

// ============================================================================
//  settings UI wiring
// ============================================================================
function syncSettingsUI(){
  $('#sTemp').value  = $('#sTempN').value  = state.settings.temperature;
  $('#sTopP').value  = $('#sTopPN').value  = state.settings.top_p;
  $('#sTopK').value  = $('#sTopKN').value  = state.settings.top_k;
  $('#sMinP').value  = $('#sMinPN').value  = state.settings.min_p;
  $('#sMaxTok').value = $('#sMaxTokN').value = state.settings.max_tokens;
  $$('#presetRow button').forEach(b => b.classList.toggle('active', b.dataset.p === state.settings.preset));
}

function applyPreset(name){
  const p = PRESETS[name];
  if (!p) return;
  state.settings.preset = name;
  Object.assign(state.settings, p);
  saveSettings();
  syncSettingsUI();
}

function bindSlider(rangeId, numId, key, min, max){
  const r = $('#' + rangeId), n = $('#' + numId);
  const update = v => {
    let f = parseFloat(v);
    if (isNaN(f)) return;
    if (f < min) f = min; if (f > max) f = max;
    state.settings[key] = f;
    state.settings.preset = 'custom';
    $$('#presetRow button').forEach(b => b.classList.remove('active'));
    saveSettings();
    r.value = f; n.value = f;
  };
  r.addEventListener('input', () => update(r.value));
  n.addEventListener('change', () => update(n.value));
}
bindSlider('sTemp',  'sTempN',  'temperature', 0,    2);
bindSlider('sTopP',  'sTopPN',  'top_p',       0,    1);
bindSlider('sTopK',  'sTopKN',  'top_k',       1,    1024);
bindSlider('sMinP',  'sMinPN',  'min_p',       0,    0.5);
bindSlider('sMaxTok','sMaxTokN','max_tokens',  0,    65536);

$$('#presetRow button').forEach(b => {
  b.onclick = () => applyPreset(b.dataset.p);
});

$('#settingsReset').onclick = () => applyPreset('balanced');

$('#settingsToggle').onclick = e => {
  $('#settingsPanel').classList.toggle('open');
  e.currentTarget.classList.toggle('active');
};
document.addEventListener('click', e => {
  if (!e.target.closest('#settingsPanel') && !e.target.closest('#settingsToggle')) {
    $('#settingsPanel').classList.remove('open');
    $('#settingsToggle').classList.remove('active');
  }
});

// quick-temp pills under the textarea
$$('#settingsHint .pill[data-quick]').forEach(p => {
  p.onclick = () => {
    const v = parseFloat(p.dataset.quick);
    state.settings.temperature = v;
    state.settings.preset = 'custom';
    saveSettings(); syncSettingsUI();
  };
});

// ============================================================================
//  app wiring
// ============================================================================
const ta = $('#textArea');

ta.addEventListener('input', () => {
  ta.style.height = 'auto';
  ta.style.height = Math.min(ta.scrollHeight, window.innerHeight * 0.3) + 'px';
});
ta.addEventListener('keydown', e => {
  if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) {
    e.preventDefault();
    submitMsg();
  }
});

function submitMsg(){
  const text = ta.value.trim();
  if (!text || state.abort) return;
  ta.value = ''; ta.style.height = 'auto';
  send(text);
}

$('#sendBtn').onclick = submitMsg;
$('#stopBtn').onclick = stopGeneration;
$('#newChatBtn').onclick = startNewConv;
$('#sidebarToggle').onclick = () => {
  $('#app').classList.toggle('show-sidebar');
  $('#app').classList.toggle('no-sidebar');
};
$('#thinkToggleHdr').onclick = () => {
  state.showThinking = !state.showThinking;
  $$('details.thinking').forEach(d => d.open = state.showThinking);
  $('#thinkToggleHdr').textContent = (state.showThinking ? '◐' : '○') + ' thinking';
};

// /health pills
fetch('/health').then(r => r.json()).then(j => {
  $('#model').textContent   = j.model || '?';
  $('#backend').textContent = j.backend ? 'backend: ' + j.backend : '';
  $('#ntools').textContent  = (j.tools ?? 0) + ' tools';
}).catch(()=>{});

// ============================================================================
//  bootstrap
// ============================================================================
(async () => {
  syncSettingsUI();
  await loadAllConvs();
  renderSidebar();
  renderChat();
  ta.focus();
})();
</script>
</body></html>
)HTML";

// ============================================================================
// Helpers
// ============================================================================

// ---------------------------------------------------------------------------
// Read a small text file fully into a string. Capped to 1 MiB to avoid the
// "oops --system pointed at /dev/random" footgun.
// ---------------------------------------------------------------------------
static std::string read_text_file(const std::string & path,
                                  size_t max_bytes = 1u << 20) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    if (sz <= 0) return {};
    if ((size_t) sz > max_bytes) sz = (std::streamoff) max_bytes;
    f.seekg(0, std::ios::beg);
    std::string out((size_t) sz, '\0');
    f.read(out.data(), sz);
    out.resize((size_t) f.gcount());
    return out;
}

// ---------------------------------------------------------------------------
// HTML escape — used to keep an operator-supplied --webui-title string from
// breaking out of the <title> / <h1> elements (defense in depth: only the
// admin can set this flag, but better safe than embarrassed).
// ---------------------------------------------------------------------------
static std::string html_escape(const std::string & s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '&':  out += "&amp;";  break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;
        }
    }
    return out;
}

// Substitute every occurrence of `from` with `to`. Returns a copy.
static std::string str_replace_all(std::string s,
                                   const std::string & from,
                                   const std::string & to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// Read a small binary file fully (icon).  Capped at 256 KiB — anything
// larger is almost certainly a mistake (a webfont or a hi-res splash).
static std::string read_binary_file(const std::string & path,
                                    size_t max_bytes = 256u * 1024u) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    if (sz <= 0) return {};
    if ((size_t) sz > max_bytes) sz = (std::streamoff) max_bytes;
    f.seekg(0, std::ios::beg);
    std::string out((size_t) sz, '\0');
    f.read(out.data(), sz);
    out.resize((size_t) f.gcount());
    return out;
}

// Lower-case the file extension to pick a Content-Type for the favicon.
static std::string mime_for_icon(const std::string & path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot + 1);
    for (auto & c : ext) c = (char) std::tolower((unsigned char) c);
    if (ext == "ico")           return "image/x-icon";
    if (ext == "png")           return "image/png";
    if (ext == "svg")           return "image/svg+xml";
    if (ext == "gif")           return "image/gif";
    if (ext == "jpg" ||
        ext == "jpeg")          return "image/jpeg";
    if (ext == "webp")          return "image/webp";
    return "application/octet-stream";
}

// ---------------------------------------------------------------------------
// Build an OpenAI-compatible error envelope.
// ---------------------------------------------------------------------------
static std::string error_json(const std::string & msg, const char * type = "invalid_request_error") {
    json j;
    j["error"]["message"] = msg;
    j["error"]["type"]    = type;
    return j.dump();
}

// ---------------------------------------------------------------------------
// Serialize a generated turn into an OpenAI chat-completion response.
// ---------------------------------------------------------------------------
static std::string build_chat_response(const std::string & model_id,
                                       const std::string & content,
                                       const std::vector<std::pair<std::string, std::string>> & tool_calls,
                                       const std::vector<std::string> & tool_call_ids,
                                       const std::string & finish_reason) {
    using clock = std::chrono::system_clock;
    ordered_json msg;
    msg["role"] = "assistant";
    msg["content"] = content;
    if (!tool_calls.empty()) {
        ordered_json tcs = json::array();
        for (size_t i = 0; i < tool_calls.size(); ++i) {
            ordered_json tc;
            tc["id"] = i < tool_call_ids.size() && !tool_call_ids[i].empty()
                          ? tool_call_ids[i]
                          : ("call_" + std::to_string(i));
            tc["type"] = "function";
            tc["function"]["name"]      = tool_calls[i].first;
            tc["function"]["arguments"] = tool_calls[i].second;
            tcs.push_back(tc);
        }
        msg["tool_calls"] = tcs;
    }
    ordered_json choice;
    choice["index"]         = 0;
    choice["message"]       = msg;
    choice["finish_reason"] = finish_reason;

    ordered_json env;
    env["id"]      = "chatcmpl-easyai";
    env["object"]  = "chat.completion";
    env["created"] = std::chrono::duration_cast<std::chrono::seconds>(
                        clock::now().time_since_epoch()).count();
    env["model"]   = model_id;
    env["choices"] = json::array({choice});
    env["usage"]   = { {"prompt_tokens", 0}, {"completion_tokens", 0}, {"total_tokens", 0} };
    return env.dump();
}

// ---------------------------------------------------------------------------
// Build a "tool" with a stub handler so the engine can register the schema
// without ever invoking it.  Used when the *client* provides its own tools —
// we only forward tool_calls back to the client, never dispatch them here.
// ---------------------------------------------------------------------------
static easyai::Tool make_stub_tool(const std::string & name,
                                   const std::string & description,
                                   const std::string & params_json) {
    return easyai::Tool::make(name, description, params_json,
        [name](const easyai::ToolCall &) {
            return easyai::ToolResult::error(
                "tool '" + name + "' must be executed by the client");
        });
}

// ============================================================================
// ServerCtx — owns the engine and the HTTP server. RAII & mutexed.
// ============================================================================
struct ServerCtx {
    easyai::Engine             engine;
    std::mutex                 engine_mu;       // protects every engine_* call
    std::vector<easyai::Tool>  default_tools;   // copied into engine per request
    std::string                default_system;
    bool                       inject_datetime    = true;        // server-side authoritative date/time injection
    std::string                knowledge_cutoff   = "2024-10";   // model training-data cutoff hint
    std::string                memory_root;     // --memory dir; empty if no memory tool; used to render the per-request vocabulary
    bool                       verbose            = false;       // mirror of args.verbose for HTTP-layer logs
    easyai::Preset             default_preset;  // current "ambient" preset
    std::string                model_id;        // basename of model file
    std::string                api_key;         // empty = auth disabled
    bool                       no_think = false;// strip <think> from responses

    // Webui customisation (built once at start-up so the / handler can just
    // hand back the prebuilt buffer).
    std::string                webui_html;      // HTML with title substituted
    std::string                webui_bundle_js; // bundle.js with brand strings substituted
    std::string                webui_icon;      // raw icon bytes (empty = none)
    std::string                webui_icon_mime; // "image/x-icon" etc.

    // INI config (default: /etc/easyai/easyai.ini). Loaded at start-up;
    // empty if the file is missing. The MCP auth lookup table is built
    // from `[MCP_USER]` once and cached in `mcp_keys` so the auth check
    // doesn't re-hash the section on every request.
    easyai::config::Ini                  ini_config;
    std::map<std::string, std::string>   mcp_keys;   // token -> username

    // /metrics counters (atomics so /metrics can read without holding engine_mu)
    std::atomic<uint64_t>      n_requests{0};
    std::atomic<uint64_t>      n_errors{0};
    std::atomic<uint64_t>      n_tool_calls{0};
    std::atomic<uint64_t>      n_in_flight{0};   // currently being served
    std::atomic<uint64_t>      n_bytes_in{0};    // total request bodies received
    std::atomic<uint64_t>      n_bytes_out{0};   // total response bodies sent (streamed = 0)

    // sampling defaults that survive across requests (set via /v1/preset)
    float def_temperature = 0.7f;
    float def_top_p       = 0.95f;
    int   def_top_k       = 40;
    float def_min_p       = 0.05f;

    // ---------------------------------------------------------------------
    // Apply server defaults to the engine. Always called BEFORE handling
    // a request so we start from a known state.
    // ---------------------------------------------------------------------
    void reset_engine_defaults() {
        engine.clear_history();
        engine.system(default_system);
        engine.set_sampling(def_temperature, def_top_p, def_top_k, def_min_p);

        engine.clear_tools();
        for (const auto & t : default_tools) engine.add_tool(t);
    }

    // ---------------------------------------------------------------------
    // Update the *ambient* defaults (used by the webui preset bar).
    // ---------------------------------------------------------------------
    void apply_preset(const easyai::Preset & p) {
        def_temperature = p.temperature;
        def_top_p       = p.top_p;
        def_top_k       = p.top_k;
        def_min_p       = p.min_p;
        default_preset  = p;
    }
};

// ============================================================================
// Request handlers
// ============================================================================

// ---------------------------------------------------------------------------
// Chat-completions request — parsed once, used by both sync and stream paths.
// ---------------------------------------------------------------------------
struct ChatRequest {
    // FULL-FIDELITY history.  Each entry preserves tool_calls (for
    // assistant), tool_call_id + name (for tool messages), and
    // reasoning_content (when persisted).  The chat template renders
    // <tool_call>…</tool_call> markup off `tool_calls`, so dropping
    // it flattens the conversation into "tool result with no preceding
    // call" which Qwen3 / Hermes / DeepSeek templates do not handle
    // well — that was the root cause of malformed multi-hop turns
    // from external clients (cli-remote, OpenAI SDK, Claude-Code).
    std::vector<easyai::Engine::HistoryMessage>       hist;
    std::string                                       last_user;    // peeled-off (only valid when !last_is_tool)
    bool                                              last_is_tool = false;  // tool-result-injection turn
    bool                                              client_tools = false;
    json                                              tools_blob;   // raw OpenAI tools[]
    double                                            temp_override  = -1.0;
    double                                            top_p_override = -1.0;
    double                                            top_k_override = -1.0;
    bool                                              stream = false;
    easyai::PresetResult                              preset_inline; // applied=true if peeled
    // Per-request override of the server-side --inject-datetime flag,
    // populated from the X-Easyai-Inject HTTP header.  Empty leaves
    // the server default in effect; "on" forces injection on; "off"
    // forces it off.  Use sparingly — disabling the preamble removes
    // a real safety net (the model will start hallucinating "today"
    // and post-cutoff facts without it).
    std::string                                       inject_override;
};

// Returns false on bad request (and writes the error to `res`).
static bool parse_chat_request(const httplib::Request & req,
                               httplib::Response & res,
                               ChatRequest & out) {
    json body;
    try {
        body = json::parse(req.body);
        // Reject pathologically-deep JSON. nlohmann's recursive descent
        // would otherwise stack-overflow on a hostile body like
        // `{"a":{"a":...}}` 100 000 levels deep before we get to
        // typecheck anything. 64 levels is far past any legitimate
        // OpenAI / MCP / chat shape; iterative walk so the validator
        // itself doesn't recurse.
        constexpr int kMaxJsonDepth = 64;
        struct Frame { const json * v; int d; };
        std::vector<Frame> stk;
        stk.push_back({ &body, 1 });
        while (!stk.empty()) {
            Frame f = stk.back(); stk.pop_back();
            if (f.d > kMaxJsonDepth) {
                throw std::runtime_error("JSON nesting exceeds "
                    + std::to_string(kMaxJsonDepth) + " levels");
            }
            if (f.v->is_object()) {
                for (auto it = f.v->begin(); it != f.v->end(); ++it) {
                    stk.push_back({ &it.value(), f.d + 1 });
                }
            } else if (f.v->is_array()) {
                for (const auto & e : *f.v) {
                    stk.push_back({ &e, f.d + 1 });
                }
            }
        }
    } catch (const std::exception & e) {
        res.status = 400;
        res.set_content(error_json(std::string("invalid JSON: ") + e.what()),
                        "application/json");
        return false;
    }

    if (!body.contains("messages") || !body["messages"].is_array() ||
        body["messages"].empty()) {
        res.status = 400;
        res.set_content(error_json("'messages' is required and must be a non-empty array"),
                        "application/json");
        return false;
    }

    out.hist.reserve(body["messages"].size());
    // OpenAI-style request shape: messages MUST end with role='user' on a
    // fresh turn, OR with role='tool' when the client has already dispatched
    // a previous turn's tool_calls and is now feeding the results back so
    // the model can produce the next visible reply.
    //
    // FULL-FIDELITY parse — mirrors llama-server's
    // common_chat_msgs_parse_oaicompat.  Assistant `tool_calls` and
    // tool `tool_call_id` / `name` are preserved so the chat template
    // can render <tool_call>…</tool_call> markup against the matching
    // tool result.  This is the upstream-tested behaviour; without it,
    // Qwen3 / Hermes / DeepSeek templates render an orphan tool result
    // and the model produces malformed output.
    std::string last_role;
    for (const auto & m : body["messages"]) {
        easyai::Engine::HistoryMessage hm{};
        hm.role = m.value("role", "user");

        if (m.contains("content")) {
            const auto & content = m["content"];
            if (content.is_string()) {
                hm.content = content.get<std::string>();
            } else if (content.is_array()) {
                for (const auto & part : content) {
                    if (part.value("type", "") == "text") hm.content += part.value("text", "");
                }
            }
            // content.is_null() → leave hm.content empty (assistant
            // turn with tool_calls only).
        }

        if (hm.role == "assistant"
                && m.contains("tool_calls")
                && m["tool_calls"].is_array()) {
            for (const auto & tc : m["tool_calls"]) {
                easyai::Engine::ToolCallSpec spec{};
                if (tc.contains("function") && tc["function"].is_object()) {
                    const auto & fn = tc["function"];
                    if (fn.contains("name") && fn["name"].is_string()) {
                        spec.name = fn["name"].get<std::string>();
                    }
                    if (fn.contains("arguments")) {
                        // OpenAI spec says arguments is a string, but
                        // tolerant clients pass an object — accept both.
                        if (fn["arguments"].is_string()) {
                            spec.arguments_json = fn["arguments"].get<std::string>();
                        } else {
                            spec.arguments_json = fn["arguments"].dump();
                        }
                    }
                }
                if (tc.contains("id") && tc["id"].is_string()) {
                    spec.id = tc["id"].get<std::string>();
                }
                if (!spec.name.empty()) hm.tool_calls.push_back(std::move(spec));
            }
        }

        if (hm.role == "tool") {
            if (m.contains("tool_call_id") && m["tool_call_id"].is_string()) {
                hm.tool_call_id = m["tool_call_id"].get<std::string>();
            }
            if (m.contains("name") && m["name"].is_string()) {
                hm.tool_name = m["name"].get<std::string>();
            }
        }

        if (m.contains("reasoning_content") && m["reasoning_content"].is_string()) {
            hm.reasoning_content = m["reasoning_content"].get<std::string>();
        }

        last_role = hm.role;
        if (hm.role == "user") out.last_user = hm.content;
        out.hist.push_back(std::move(hm));
    }
    if (last_role != "user" && last_role != "tool") {
        res.status = 400;
        res.set_content(error_json(
            "the final message must have role='user' or role='tool' "
            "(got role='" + last_role + "')"),
                        "application/json");
        return false;
    }
    out.last_is_tool = (last_role == "tool");

    out.client_tools = body.contains("tools") && body["tools"].is_array() &&
                        !body["tools"].empty();
    if (out.client_tools) out.tools_blob = body["tools"];

    // Light per-request log of multi-hop carryover so the operator can
    // confirm full-fidelity round-trip on long sessions.  No banner —
    // this is the healthy case; we just record the counts.
    if (auto * fp = easyai::log::file()) {
        size_t carried_tool_calls = 0;
        size_t carried_tool_msgs  = 0;
        for (const auto & hm : out.hist) {
            carried_tool_calls += hm.tool_calls.size();
            if (hm.role == "tool") ++carried_tool_msgs;
        }
        if (carried_tool_calls || carried_tool_msgs) {
            std::fprintf(fp,
                "history carryover: assistant tool_calls=%zu tool messages=%zu\n",
                carried_tool_calls, carried_tool_msgs);
            std::fflush(fp);
        }
    }

    auto get_num = [&](const char * k, double dflt) -> double {
        if (body.contains(k) && body[k].is_number()) return body[k].get<double>();
        return dflt;
    };
    out.temp_override  = get_num("temperature", -1.0);
    out.top_p_override = get_num("top_p",       -1.0);
    out.top_k_override = get_num("top_k",       -1.0);
    out.stream         = body.value("stream", false);

    // Inline preset prefix in the last user message ("creative 0.9 …").
    out.preset_inline = easyai::parse_preset(out.last_user);
    if (!out.preset_inline.applied.empty()) {
        out.last_user = out.last_user.substr(out.preset_inline.consumed);
        out.hist.back().content = out.last_user;
    }

    // Optional per-request override of the authoritative-datetime
    // injection.  The header lookup uses httplib's case-insensitive
    // Headers comparator, so any casing reaches us.
    auto hi = req.headers.find("X-Easyai-Inject");
    if (hi != req.headers.end()) {
        std::string v = hi->second;
        for (auto & c : v) c = (char) std::tolower((unsigned char) c);
        if (v == "on" || v == "off") out.inject_override = v;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Apply request-level overrides + replace history. Caller already holds
// engine_mu and has called reset_engine_defaults().
// ---------------------------------------------------------------------------
// Build an authoritative preamble that's appended to the SERVER's
// default system prompt on every request.  Per user request: pass the
// model the current date/time + timezone, and tell it about its
// knowledge cutoff so it knows when to defer to tools instead of
// hallucinating post-cutoff facts.
//
// We rebuild this per request so the timestamp is always fresh.
static std::string build_authoritative_preamble(const ServerCtx & ctx) {
    // The preamble builder lives in libeasyai now (preamble.hpp) so
    // server, local, and cli all share one implementation. This thin
    // wrapper just maps from ServerCtx into the library's Options
    // struct.
    return easyai::preamble::build({
        /* inject_datetime  = */ true,
        /* knowledge_cutoff = */ ctx.knowledge_cutoff,
        /* memory_root      = */ ctx.memory_root,
        /* cite_sources     = */ true,
    });
}

static void prepare_engine_for_request(ServerCtx & ctx, const ChatRequest & req) {
    if (req.client_tools) {
        ctx.engine.clear_tools();
        for (const auto & t : req.tools_blob) {
            const json & f = t.contains("function") ? t["function"] : t;
            std::string name        = f.value("name", "");
            std::string description = f.value("description", "");
            std::string params_json = f.contains("parameters")
                                          ? f["parameters"].dump()
                                          : std::string("{\"type\":\"object\",\"properties\":{}}");
            if (name.empty()) continue;
            ctx.engine.add_tool(make_stub_tool(name, description, params_json));
        }
    }
    ctx.engine.set_sampling(
        req.temp_override  >= 0 ? (float) req.temp_override  : -1.0f,
        req.top_p_override >= 0 ? (float) req.top_p_override : -1.0f,
        req.top_k_override >= 0 ? (int)   req.top_k_override : -1,
        -1.0f);
    if (!req.preset_inline.applied.empty()) {
        ctx.engine.set_sampling(req.preset_inline.temperature,
                                 req.preset_inline.top_p,
                                 req.preset_inline.top_k,
                                 req.preset_inline.min_p);
    }

    // Build the request's history (everything except the final user
    // message — that one is pushed by the chat loop).  We work on a
    // mutable copy so we can splice the authoritative-datetime
    // preamble into whichever system message exists, regardless of
    // whether the CLIENT supplied one or we fall back to the
    // server's default.
    //
    // Critical scenario: opencode / Claude-Code style clients send
    // their OWN system message in the messages array.  That message
    // *replaces* the server's default in practice (the chat template
    // would render two system blocks otherwise, which most templates
    // collapse into one with unpredictable order).  We want the
    // datetime + cutoff hint to ride along regardless of who owns
    // the system prompt — so we APPEND the preamble to whichever
    // system message goes into the prompt:
    //
    //   * Client sent a system message → append to its content.
    //   * Client sent no system message → append to ctx.default_system.
    //
    // Either way, the preamble is in there exactly once and lands as
    // part of the model's actual context.
    // When last is "tool" (the client is feeding a previous turn's tool
    // result back), keep the WHOLE conversation in history — the model
    // needs to see the tool message to produce its next reply.  When
    // last is "user", peel it off so the chat loop can push it after
    // the chat-params render (see chat_continue's user-message
    // requirement for Qwen3-style templates).
    std::vector<easyai::Engine::HistoryMessage> hist_minus_last;
    if (req.last_is_tool) {
        hist_minus_last = req.hist;          // include the tool message
    } else {
        hist_minus_last.assign(req.hist.begin(), req.hist.end() - 1);
    }

    // Resolve the effective inject toggle: server default + optional
    // X-Easyai-Inject header override.  Default is on; QA / regression
    // suites that want to A/B the preamble pass `X-Easyai-Inject: off`.
    bool inject_now = ctx.inject_datetime;
    if      (req.inject_override == "on")  inject_now = true;
    else if (req.inject_override == "off") inject_now = false;

    if (inject_now) {
        const std::string preamble = build_authoritative_preamble(ctx);
        // Find the LAST system message in the client-supplied history.
        // (Most clients put it at index 0, but we walk backwards to be
        // safe — multi-system histories pick the most recent.)
        auto it = std::find_if(hist_minus_last.rbegin(), hist_minus_last.rend(),
            [](const easyai::Engine::HistoryMessage & m) {
                return m.role == "system";
            });
        if (it != hist_minus_last.rend()) {
            it->content += preamble;
            ctx.engine.system(ctx.default_system);
        } else {
            ctx.engine.system(ctx.default_system + preamble);
        }
    } else {
        ctx.engine.system(ctx.default_system);
    }

    ctx.engine.replace_history(hist_minus_last);
}

// ---------------------------------------------------------------------------
// Strip <think>…</think> blocks (server-side).  Used for the non-streaming
// reply when --no-think was passed.  The streaming path uses a state-machine
// version inline so it can act per-token.
// ---------------------------------------------------------------------------
static std::string strip_think_blocks(const std::string & content) {
    if (content.empty()) return content;
    std::string out;
    out.reserve(content.size());
    size_t i = 0;
    while (i < content.size()) {
        size_t a = content.find("<think>",    i);
        size_t b = content.find("<thinking>", i);
        size_t open = std::min(a == std::string::npos ? std::string::npos : a,
                               b == std::string::npos ? std::string::npos : b);
        if (open == std::string::npos) { out.append(content, i, std::string::npos); break; }
        out.append(content, i, open - i);
        size_t ca = content.find("</think>",    open);
        size_t cb = content.find("</thinking>", open);
        size_t close = std::min(ca == std::string::npos ? std::string::npos : ca,
                                cb == std::string::npos ? std::string::npos : cb);
        if (close == std::string::npos) break;
        size_t close_end = content.find('>', close) + 1;
        i = close_end;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Non-streaming path — kept exactly as before. Caller guarantees ctx.engine_mu
// is held.
// ---------------------------------------------------------------------------
static void handle_chat_sync(ServerCtx & ctx, ChatRequest & req,
                             httplib::Response & res) {
    std::string content;
    std::vector<std::pair<std::string, std::string>> tool_calls;
    std::vector<std::string> tool_call_ids;
    std::string finish_reason = "stop";

    // Symmetric with the streaming path — drop any cancel flag left
    // over from a prior turn (the streaming path can race a disconnect
    // between turns; the sync path stays consistent for free).
    ctx.engine.clear_cancel();

    try {
        if (req.client_tools) {
            // Only push the user message when this is a fresh turn — when
            // last_is_tool, the tool message is already part of history
            // (replace_history above) and the model is expected to consume
            // it directly.
            if (!req.last_is_tool) {
                ctx.engine.push_message("user", req.last_user);
            }
            auto turn = ctx.engine.generate_one();
            content       = std::move(turn.content);
            tool_calls    = std::move(turn.tool_calls);
            tool_call_ids = std::move(turn.tool_call_ids);
            finish_reason = std::move(turn.finish_reason);
        } else {
            content = ctx.engine.chat(req.last_user);
        }
    } catch (const std::exception & e) {
        ctx.n_errors.fetch_add(1, std::memory_order_relaxed);
        res.status = 500;
        res.set_content(error_json(std::string("engine error: ") + e.what(),
                                   "internal_error"),
                        "application/json");
        easyai::log::error(
            "[easyai-server] handle_chat_sync engine threw: %s "
            "(client_tools=%s last_user_bytes=%zu)",
            e.what(), req.client_tools ? "yes" : "no", req.last_user.size());
        return;
    }
    if (!tool_calls.empty()) ctx.n_tool_calls.fetch_add(tool_calls.size(),
                                                        std::memory_order_relaxed);
    if (ctx.no_think) content = strip_think_blocks(content);

    res.status = 200;
    const std::string body = build_chat_response(ctx.model_id, content,
                                                  tool_calls, tool_call_ids,
                                                  finish_reason);
    res.set_content(body, "application/json");
    if (auto * fp = easyai::log::file()) {
        std::fprintf(fp,
            "----- RESPONSE (sync) -----\n"
            "http_status=200 finish_reason=%s content_bytes=%zu tool_calls=%zu\n"
            "----- ASSISTANT CONTENT -----\n%s\n"
            "----- BODY (json) -----\n%s\n"
            "==========\n",
            finish_reason.c_str(), content.size(), tool_calls.size(),
            content.c_str(), body.c_str());
        std::fflush(fp);
    }
}

// ---------------------------------------------------------------------------
// Streaming path (Server-Sent Events).
//
// Standard OpenAI delta envelope is emitted for each generated piece:
//
//   data: {"choices":[{"index":0,"delta":{"content":"hi"},"finish_reason":null}]}
//
// Plus two custom event types so the embedded webui can render tool activity
// inline:
//
//   event: easyai.tool_call
//   data: {"name":"web","arguments":"{...}","id":"call_1"}
//
//   event: easyai.tool_result
//   data: {"name":"web","content":"...","is_error":false}
//
// Generic OpenAI clients ignore unknown event types, so this is additive.
//
// All engine work happens INSIDE the chunked-content lambda so we can hold
// the engine mutex from there.  shared_ptr<ChatRequest> keeps the parsed
// state alive past `route_chat_completions`'s return.
// ---------------------------------------------------------------------------
static void handle_chat_stream(ServerCtx & ctx,
                               std::shared_ptr<ChatRequest> req_state,
                               httplib::Response & res) {
    res.set_header("Cache-Control",     "no-cache");
    res.set_header("X-Accel-Buffering", "no");

    res.set_chunked_content_provider("text/event-stream",
        [&ctx, req_state](size_t /*offset*/, httplib::DataSink & sink) -> bool {
            std::lock_guard<std::mutex> lock(ctx.engine_mu);

            ctx.reset_engine_defaults();
            prepare_engine_for_request(ctx, *req_state);

            // Drop any cancel flag left over from a prior turn before we
            // start decoding. Without this, a single dropped client would
            // poison the engine for every subsequent request.
            ctx.engine.clear_cancel();

            // Snapshot perf counters BEFORE this request so we can diff
            // them out at the end (llama_perf_context() returns cumulative
            // values across the lifetime of the llama_context).
            const auto perf_before = ctx.engine.perf_data();

            // ---- emit helpers --------------------------------------------
            //
            // Both helpers honour cpp-httplib's DataSink::write contract:
            // it returns false when the underlying socket can no longer
            // accept bytes — i.e. the client dropped. We surface that as
            // engine.request_cancel() so the decode loop bails out at the
            // next token boundary (otherwise the engine happily generates
            // for minutes against a dead socket; that's the bug Gustavo
            // hit, where two turns kept running after Ctrl+C and required
            // a server restart).
            //
            // Once cancelled, subsequent emits short-circuit to no-ops so
            // the unwinding agentic loop doesn't keep retrying the dead
            // sink and doesn't keep flipping the cancel flag back on (the
            // checks above already broke the decode loop).
            auto emit_data = [&sink, &ctx](const std::string & ev) {
                if (ctx.engine.cancel_requested()) return;
                std::string s = "data: " + ev + "\n\n";
                if (!sink.write(s.data(), s.size())) {
                    ctx.engine.request_cancel();
                }
            };
            auto emit_event = [&sink, &ctx](const std::string & evt_type, const std::string & ev) {
                if (ctx.engine.cancel_requested()) return;
                std::string s = "event: " + evt_type + "\ndata: " + ev + "\n\n";
                if (!sink.write(s.data(), s.size())) {
                    ctx.engine.request_cancel();
                }
            };
            // Dump JSON tolerantly: a multi-byte UTF-8 character can split
            // across two model tokens (one piece ends with the lead byte,
            // the next starts with the continuation byte 0x80-0xBF), and
            // strict-mode dump throws on an isolated continuation byte
            // ("invalid UTF-8 byte at index 0: 0x8B"), aborting the
            // service.  ::replace substitutes a U+FFFD glyph for invalid
            // bytes and keeps the stream alive.
            auto safe_dump = [](const ordered_json & j) {
                return j.dump(-1, ' ', false,
                              ordered_json::error_handler_t::replace);
            };

            // ---- streaming pipeline (llama-server-compatible) -----------
            // Mirror llama-server's emit pipeline:
            //   1. accumulate the model's raw text as it streams
            //   2. after each token, common_chat_parse(text, is_partial=true)
            //      with the current chat template's reasoning_format/parser,
            //      which extracts msg.reasoning_content + msg.content
            //   3. compute common_chat_msg_diff vs the previous parse
            //   4. emit standard OpenAI-shape SSE deltas with the new
            //      reasoning_content_delta / content_delta fields
            //
            // This is exactly what tools/server/server-task.cpp does, so the
            // embedded webui's native parsers (.choices[0].delta.content +
            // .choices[0].delta.reasoning_content) light up correctly.
            //
            // IMPORTANT: many chat templates (Qwen3-* in particular) raise
            // a Jinja exception if there's no user message in history, so
            // we must push the last user message BEFORE calling
            // chat_params_for_current_state — otherwise the render throws.
            // Once it's pushed we call chat_continue() (or generate_one
            // for client-tool mode) which doesn't re-push.
            //
            // EXCEPTION: when the request's last message is a tool result
            // (cli-remote / OpenAI-SDK feeding back a previous turn's
            // tool_call output), there's already a tool message in
            // history AND a user message earlier in the same history.
            // We don't push anything extra in that case.
            if (!req_state->last_is_tool) {
                ctx.engine.push_message("user", req_state->last_user);
            }
            const bool user_already_pushed = true;

            common_chat_params chat_p;
            try {
                chat_p = ctx.engine.chat_params_for_current_state(true);
            } catch (const std::exception & e) {
                std::fprintf(stderr,
                    "[easyai-server] chat_params render threw: %s — "
                    "falling back to generic parser\n", e.what());
            }
            common_chat_parser_params parser_params(chat_p);
            parser_params.parse_tool_calls = true;
            parser_params.reasoning_format = COMMON_REASONING_FORMAT_AUTO;
            if (!chat_p.parser.empty()) {
                try { parser_params.parser.load(chat_p.parser); }
                catch (...) { /* will fall through to non-PEG parse */ }
            }

            std::string accumulated;
            common_chat_msg prev_msg;
            prev_msg.role = "assistant";
            const bool drop_thinking = ctx.no_think;
            // Tracks whether ANY content delta has been emitted during
            // this request.  When the parser blows up on malformed
            // tool_call syntax (Qwen-style doubled-brace JSON, etc.) the
            // partial parses all throw and the engine silently falls
            // back to msg.content=raw.  Without this flag the client
            // would see an empty bubble — see the post-chat_continue
            // last-resort emit below.
            bool        any_content_emitted   = false;
            size_t      content_bytes_emitted = 0;   // raw delta.content cumulative
            std::string content_text_emitted;        // accumulated visible text
                                                      // (for announce-pattern check)
            // Engine-dispatched tool count for THIS request. The local
            // `tool_calls` vector below is only populated in client_tools
            // mode; on the agentic path the engine runs tools internally
            // and that vector stays empty — so we can't use it to gate
            // the "incomplete response" warning. This counter is bumped
            // by the on_tool callback (which the engine fires for every
            // dispatch) and means "we did SOME useful work this request"
            // even if the final visible reply was empty.
            int         engine_tool_dispatches = 0;
            // Per-retry incomplete events the engine fires (announce-
            // only path or thought-only path). Counted so the final
            // incomplete log can say "engine retried N times before
            // giving up" instead of leaving the operator guessing.
            int         engine_incomplete_retries = 0;

            auto emit_diff = [&](const common_chat_msg_diff & d) {
                ordered_json delta = ordered_json::object();
                if (!d.reasoning_content_delta.empty() && !drop_thinking) {
                    delta["reasoning_content"] = d.reasoning_content_delta;
                }
                if (!d.content_delta.empty()) {
                    delta["content"] = d.content_delta;
                    any_content_emitted = true;
                    content_bytes_emitted += d.content_delta.size();
                    content_text_emitted += d.content_delta;
                }
                // tool_call deltas: skipped — we already surface them via
                // the easyai.tool_call / easyai.tool_result custom events.
                if (delta.empty()) return;
                ordered_json env;
                env["choices"] = json::array({{
                    {"index", 0},
                    {"delta", delta},
                    {"finish_reason", nullptr},
                }});
                emit_data(safe_dump(env));
            };

            // Defensive: re-route any <think>...</think> spans the partial
            // parser left inside msg.content into msg.reasoning_content.
            // Triggered when the model emits content BEFORE a reasoning
            // block (recap-thoughts pattern).  The auto-extract logic in
            // common_chat_parse only fires for a leading reasoning block,
            // so secondary <think> blocks leak into the bubble unless we
            // catch them here.  Keeps both content and reasoning monotonic
            // so compute_diffs doesn't throw on \"content shrunk\".
            auto extract_think_into_reasoning = [](common_chat_msg & m) {
                static const std::string kOpen  = "<think>";
                static const std::string kClose = "</think>";
                auto & c = m.content;
                auto & r = m.reasoning_content;
                size_t scan = 0;
                while (scan < c.size()) {
                    size_t open = c.find(kOpen, scan);
                    if (open == std::string::npos) break;
                    size_t close = c.find(kClose, open + kOpen.size());
                    if (close == std::string::npos) {
                        // Open without close yet — move tail to reasoning
                        // so it doesn't appear in content while streaming.
                        // Next pass picks up the close marker normally.
                        r.append(c, open + kOpen.size(), std::string::npos);
                        c.erase(open);
                        break;
                    }
                    r.append(c, open + kOpen.size(),
                             close - open - kOpen.size());
                    c.erase(open, close + kClose.size() - open);
                    scan = open;
                }
            };

            ctx.engine.on_token([&](const std::string & piece) {
                accumulated += piece;
                common_chat_msg new_msg;
                try {
                    new_msg = common_chat_parse(accumulated,
                                                /*is_partial=*/true,
                                                parser_params);
                } catch (const std::exception &) {
                    // Incomplete tag mid-stream — wait for more bytes.
                    return;
                }
                new_msg.role = "assistant";
                extract_think_into_reasoning(new_msg);
                std::vector<common_chat_msg_diff> diffs;
                try {
                    diffs = common_chat_msg_diff::compute_diffs(prev_msg, new_msg);
                } catch (const std::exception &) {
                    // compute_diffs is strict about monotonic tool_calls
                    // growth; incremental parses sometimes assemble a
                    // tool_call and then "unassemble" it as later tokens
                    // arrive (e.g. the parser briefly treats partial
                    // arguments as complete).  Rather than killing the
                    // stream, hold prev_msg on the last good state and
                    // wait for the next token to settle.
                    return;
                }
                prev_msg = new_msg;
                for (const auto & d : diffs) emit_diff(d);
            });

            // ---- on_tool: surface server-side dispatches to the client ----
            // Two channels:
            //   easyai.tool_call / easyai.tool_result custom SSE events for
            //     our own webui (drives the per-message status pill and any
            //     sidecar tool log in the future)
            //   a one-line inline markdown indicator so OpenAI-shape clients
            //     (incl. the bundle's own renderer) at least show that
            //     something happened
            // After firing we reset the incremental-parse state because
            // the next agentic hop is a fresh model turn — its text starts
            // from scratch even though it shares the same on_token callback.
            ctx.engine.on_tool([&](const easyai::ToolCall & c, const easyai::ToolResult & r) {
                ctx.n_tool_calls.fetch_add(1, std::memory_order_relaxed);
                ++engine_tool_dispatches;

                // The embedded webui's stream parser ignores the SSE
                // `event:` field and inspects every `data:` line as if it
                // were an OpenAI chunk — i.e. it does parsed.choices[0]
                // unguarded.  Our custom payloads have no .choices, so the
                // bundle throws "undefined is not an object" on every one.
                // Add an empty choices[] stub so the bundle's destructure
                // is harmless; our own monitorSSE keys off `evtType` and
                // never reads .choices on these payloads.
                ordered_json call_evt;
                call_evt["choices"]   = json::array({{
                    {"index", 0},
                    {"delta", json::object()},
                    {"finish_reason", nullptr},
                }});
                call_evt["name"]      = c.name;
                call_evt["arguments"] = c.arguments_json;
                call_evt["id"]        = c.id;
                emit_event("easyai.tool_call", safe_dump(call_evt));

                ordered_json res_evt;
                res_evt["choices"]  = json::array({{
                    {"index", 0},
                    {"delta", json::object()},
                    {"finish_reason", nullptr},
                }});
                res_evt["name"]        = c.name;
                res_evt["content"]     = r.content;
                res_evt["is_error"]    = r.is_error;
                res_evt["duration_ms"] = r.duration_ms;
                emit_event("easyai.tool_result", safe_dump(res_evt));

                // Tool-call status line goes into the reasoning channel
                // (`delta.reasoning_content`) instead of the visible
                // content body, so the bundle paints it inside the
                // collapsible Reasoning panel — same place the model's
                // thoughts live, since a tool dispatch is part of the
                // reasoning flow, not the user-facing answer.  Plain text
                // (no markdown asterisks) because the bundle renders
                // reasoning with whitespace-pre-wrap, not as markdown.
                //
                // Best-effort target hint: the most useful field of the
                // arguments JSON for "what is the model trying to reach".
                // We pick `url` (web action=fetch) first, then `query`
                // (web action=search), then `path` (fs_*), then `command`
                // (bash).
                // Falls back to a short prefix of the raw args blob so
                // lesser-known tools still get *something* on the line.
                // Truncated to 80 chars to keep the log a single line.
                std::string target;
                try {
                    auto j = json::parse(c.arguments_json);
                    static constexpr const char * kKeys[] = {
                        "url", "query", "path", "command", "expr", "name",
                    };
                    for (const char * k : kKeys) {
                        if (j.contains(k) && j[k].is_string()) {
                            target = j[k].get<std::string>();
                            break;
                        }
                    }
                } catch (...) { /* leave target empty */ }
                if (target.empty() && !c.arguments_json.empty()
                                   && c.arguments_json != "{}") {
                    target = c.arguments_json;
                }
                if (target.size() > 80) { target.resize(80); target += "…"; }

                std::ostringstream tlog;
                tlog << "\n" << (r.is_error ? "❌ " : "🔧 ") << c.name;
                tlog << " (" << r.duration_ms << "ms)";
                if (!target.empty()) tlog << " " << target;
                if (r.is_error) {
                    std::string reason = r.content;
                    if (reason.size() > 80) { reason.resize(80); reason += "…"; }
                    tlog << " — " << reason;
                }
                tlog << "\n";

                ordered_json delta;
                delta["choices"] = json::array({{
                    {"index", 0},
                    {"delta", {{"reasoning_content", tlog.str()}}},
                    {"finish_reason", nullptr},
                }});
                emit_data(safe_dump(delta));

                // Reset incremental-parse state for the next iteration.
                accumulated.clear();
                prev_msg = common_chat_msg{};
                prev_msg.role = "assistant";
            });

            // Engine fires this when chat_continue() throws away a turn and
            // restarts (e.g. thought-only retry path for Qwen3 fine-tunes
            // that terminate after </think> without an answer).  We need to
            // drop our incremental-parse state so the next hop's tokens are
            // diff'd from a clean baseline instead of being concatenated to
            // the discarded turn's reasoning.
            ctx.engine.on_hop_reset([&]() {
                accumulated.clear();
                prev_msg = common_chat_msg{};
                prev_msg.role = "assistant";
            });

            // Engine fires this between every batch of the prompt-eval
            // llama_decode loop (default n_batch=512 → one tick per
            // 512 tokens, plus a final tick at processed==total). We
            // surface it as `event: easyai.prompt_progress` carrying
            // {total, cache, processed, time_ms} — same shape
            // llama-server emits in its own `prompt_progress` field
            // (return_progress=true) so any client that already speaks
            // that contract works without changes. Drives the cli
            // shimmer's real "thinking N%" gauge and the webui chip's
            // "processing N%" label.
            //
            // Throttle: skip ticks that are < 80 ms apart from the
            // previous emit AND < 5 percentage points further along.
            // Keeps the SSE stream cheap on huge prompts (a 32 K-token
            // prompt at n_batch=512 generates 64 ticks; throttling
            // collapses them to ~12 visible updates) without losing
            // the final tick (always emitted).
            auto last_emit_ms  = std::make_shared<double>(-1.0);
            auto last_emit_pct = std::make_shared<int>(-1);
            ctx.engine.on_prompt_progress(
                [&, last_emit_ms, last_emit_pct]
                (const easyai::PromptProgressReport & r) {
                    if (r.total <= 0) return;
                    const int pct = (int)(100.0 * r.processed / r.total);
                    const bool is_final = (r.processed >= r.total);
                    if (!is_final) {
                        if (*last_emit_ms >= 0
                                && r.ms - *last_emit_ms < 80.0
                                && pct - *last_emit_pct < 5) {
                            return;
                        }
                    }
                    *last_emit_ms  = r.ms;
                    *last_emit_pct = pct;
                    ordered_json evt;
                    evt["choices"] = json::array({{
                        {"index", 0},
                        {"delta", json::object()},
                        {"finish_reason", nullptr},
                    }});
                    evt["total"]     = r.total;
                    evt["cache"]     = r.cached;
                    evt["processed"] = r.processed;
                    evt["time_ms"]   = r.ms;
                    evt["pct"]       = pct;   // convenience for clients
                    emit_event("easyai.prompt_progress", safe_dump(evt));
                });

            // Engine fires this once per generate() AFTER the prompt-
            // eval llama_decode loop completes and BEFORE the first
            // token is sampled. We surface it as:
            //   1) A custom SSE event (easyai.prompt_eval) carrying
            //      n_tokens / n_cached / prompt_ms / tokens_per_second
            //      — read by our embedded webui's monitorSSE block to
            //      flip the per-message chip into a 'processing' state
            //      with the live counters, and by libeasyai-cli's
            //      Client to fire its own on_prompt_eval callback so
            //      easyai-cli (non-quiet mode) can print the same
            //      "prompt eval" status line llama-server reports.
            //   2) A reasoning_content delta with the same one-line
            //      summary, so generic OpenAI clients (anything that
            //      ignores custom event types) still see the timing
            //      inside the Thinking panel rather than nothing.
            //   3) A stderr log mirroring llama-server's `prompt eval
            //      time = X ms / N tokens / T t/s` line.
            // Fires once per agentic hop (= once per model turn) so
            // a multi-tool conversation gets one "prompt eval" line
            // per round-trip, matching what llama-server does.
            ctx.engine.on_prompt_eval(
                [&](const easyai::PromptEvalReport & r) {
                    if (r.n_tokens <= 0) return;
                    const double tps = r.prompt_ms > 0.0
                        ? (r.n_tokens * 1000.0 / r.prompt_ms) : 0.0;
                    ordered_json evt;
                    evt["choices"] = json::array({{
                        {"index", 0},
                        {"delta", json::object()},
                        {"finish_reason", nullptr},
                    }});
                    evt["n_tokens"]            = r.n_tokens;
                    evt["n_cached"]            = r.n_cached;
                    evt["prompt_ms"]           = r.prompt_ms;
                    evt["tokens_per_second"]   = tps;
                    emit_event("easyai.prompt_eval", safe_dump(evt));

                    std::ostringstream line;
                    line << "\n📝 prompt eval: " << r.n_tokens << " tok";
                    if (r.n_cached > 0) line << " (" << r.n_cached << " cached)";
                    line << " · " << (long long) r.prompt_ms << " ms · "
                         << std::fixed << std::setprecision(1) << tps
                         << " t/s\n";
                    ordered_json delta;
                    delta["choices"] = json::array({{
                        {"index", 0},
                        {"delta", {{"reasoning_content", line.str()}}},
                        {"finish_reason", nullptr},
                    }});
                    emit_data(safe_dump(delta));

                    std::fprintf(stderr,
                        "[easyai-server] prompt eval: %d tok (%d cached) "
                        "%.0f ms %.1f t/s\n",
                        r.n_tokens, r.n_cached, r.prompt_ms, tps);
                });

            // Engine fires this every time it discards an "announce-only"
            // turn and is about to nudge + retry (and once more with
            // attempt=-1 when the retry budget is exhausted). We push a
            // reasoning_content delta so the webui's Thinking panel shows
            // the retry attempts live — without it, the UI looks frozen
            // for 10 silent retries before a final empty bubble appears.
            ctx.engine.on_incomplete_retry(
                [&](int attempt, int max, const std::string & reason) {
                    if (attempt > 0) ++engine_incomplete_retries;
                    std::ostringstream line;
                    if (attempt < 0) {
                        // Give-up signal — different visual cue.
                        line << "\n⚠ " << reason << "\n";
                    } else {
                        line << "\n↻ Retry " << attempt << "/" << max
                             << ": " << reason << " — nudging.\n";
                    }
                    ordered_json delta;
                    delta["choices"] = json::array({{
                        {"index", 0},
                        {"delta", {{"reasoning_content", line.str()}}},
                        {"finish_reason", nullptr},
                    }});
                    emit_data(safe_dump(delta));
                    // Stderr log so the operator sees retries in
                    // journalctl. The engine itself logs at retry
                    // time too; this line is the server-layer mirror
                    // so journalctl filters on `[easyai-server]` see
                    // the events without combing engine internals.
                    if (attempt < 0) {
                        std::fprintf(stderr,
                            "[easyai-server] retry budget exhausted: %s\n",
                            reason.c_str());
                    } else {
                        std::fprintf(stderr,
                            "[easyai-server] retry %d/%d: %s\n",
                            attempt, max, reason.c_str());
                    }
                });

            // ---- run the engine ----------------------------------------
            std::string finish_reason = "stop";
            std::vector<std::pair<std::string, std::string>> tool_calls;
            std::vector<std::string> tool_call_ids;
            std::string engine_final_content;     // captured from chat_continue
            try {
                // The user message has ALREADY been pushed above (so the
                // chat_params render works for templates that require it).
                // Use generate_one / chat_continue here so we don't push
                // it a second time.
                (void) user_already_pushed;
                if (req_state->client_tools) {
                    // Pure transport: emit one turn faithfully.  Detection
                    // of "model didn't deliver" (no tool_calls + tiny
                    // content) lives in the empty/incomplete-response
                    // signal computed AFTER the engine returns; both the
                    // webui and libeasyai-cli read that single flag from
                    // timings.incomplete and render their own placeholder.
                    // We do NOT retry, do NOT nudge, do NOT promote
                    // reasoning to content here — that's policy and
                    // belongs to the agent / app layer (where the user
                    // can opt in via --retry-on-incomplete or ignore it).
                    auto turn = ctx.engine.generate_one();
                    tool_calls    = std::move(turn.tool_calls);
                    tool_call_ids = std::move(turn.tool_call_ids);
                    finish_reason = tool_calls.empty() ? "stop" : "tool_calls";
                } else {
                    engine_final_content = ctx.engine.chat_continue();
                }
            } catch (const std::exception & e) {
                ctx.n_errors.fetch_add(1, std::memory_order_relaxed);
                ordered_json err;
                err["error"] = { {"message", e.what()}, {"type", "internal_error"} };
                emit_data(safe_dump(err));
                easyai::log::error(
                    "[easyai-server] handle_chat_stream engine threw: %s "
                    "(client_tools=%s last_user_bytes=%zu)",
                    e.what(), req_state->client_tools ? "yes" : "no",
                    req_state->last_user.size());
            }

            // Final non-partial parse to flush any reasoning/content the
            // partial parses may have held back due to incomplete tags.
            try {
                auto final_msg = common_chat_parse(accumulated,
                                                   /*is_partial=*/false,
                                                   parser_params);
                final_msg.role = "assistant";
                extract_think_into_reasoning(final_msg);
                auto diffs = common_chat_msg_diff::compute_diffs(prev_msg, final_msg);
                for (const auto & d : diffs) emit_diff(d);
            } catch (...) { /* best-effort drain */ }

            // Last-resort fallback: when EVERY partial parse threw (which
            // happens when the model emits malformed Qwen-style tool_call
            // syntax), on_token returned silently for every token and the
            // client never got a content delta.  The engine itself fell
            // back to msg.content=raw inside parse_assistant; surface that
            // as one synthesised delta so the bubble isn't empty.
            //
            // DEFENSIVE: strip any reasoning that we ALREADY streamed via
            // reasoning_content_delta — otherwise the user sees the same
            // thinking text twice (in our blue brain panel AND in the
            // message body).  We do this by removing any <think>…</think>
            // span from the synthesised content; if the engine already
            // separated reasoning out (post-2026-04-26 fix in
            // src/engine.cpp) this is a no-op.
            if (!any_content_emitted && !engine_final_content.empty()
                    && !req_state->client_tools) {
                std::string body = engine_final_content;
                // Drop everything between <think> and </think> (inclusive)
                // since reasoning has already been streamed.  Plain string
                // scan — std::regex blew the stack here once already
                // (commit 3dec718) so we keep it iterative.
                {
                    static const std::string open_tag  = "<think>";
                    static const std::string close_tag = "</think>";
                    size_t close_pos = body.find(close_tag);
                    if (close_pos != std::string::npos) {
                        size_t open_pos = body.find(open_tag);
                        size_t span_begin = (open_pos != std::string::npos
                                              && open_pos < close_pos)
                                               ? open_pos : 0;
                        body.erase(span_begin,
                                   close_pos + close_tag.size() - span_begin);
                        size_t lead = 0;
                        while (lead < body.size() &&
                               (body[lead] == '\n' || body[lead] == '\r' ||
                                body[lead] == ' '  || body[lead] == '\t')) ++lead;
                        if (lead) body.erase(0, lead);
                    }
                }
                if (!body.empty()) {
                    ordered_json env;
                    env["choices"] = json::array({{
                        {"index", 0},
                        {"delta", {{"content", body}}},
                        {"finish_reason", nullptr},
                    }});
                    emit_data(safe_dump(env));
                    any_content_emitted = true;
                }
            }

            // For client-tools mode, emit the assembled tool_calls array as
            // a single delta so OpenAI clients (and our webui) see them.
            if (!tool_calls.empty()) {
                ordered_json tc_arr = json::array();
                for (size_t i = 0; i < tool_calls.size(); ++i) {
                    ordered_json tc;
                    tc["index"]    = (int) i;
                    tc["id"]       = i < tool_call_ids.size() && !tool_call_ids[i].empty()
                                         ? tool_call_ids[i]
                                         : ("call_" + std::to_string(i));
                    tc["type"]     = "function";
                    tc["function"] = { {"name", tool_calls[i].first},
                                       {"arguments", tool_calls[i].second} };
                    tc_arr.push_back(tc);
                }
                ordered_json delta;
                delta["choices"] = json::array({{
                    {"index", 0},
                    {"delta", {{"tool_calls", tc_arr}}},
                    {"finish_reason", nullptr},
                }});
                emit_data(safe_dump(delta));
            }

            // Final close-out chunk — empty delta + finish_reason + the
            // llama-server-style `timings` block the embedded webui
            // consumes to show tokens/s, Reading/Generation phases, and
            // context usage.  We also include OpenAI-standard `usage` so
            // generic clients see the prompt/completion counts.
            const auto perf_after = ctx.engine.perf_data();
            const int  prompt_n     = std::max(0, perf_after.n_prompt_tokens    - perf_before.n_prompt_tokens);
            const int  predicted_n  = std::max(0, perf_after.n_predicted_tokens - perf_before.n_predicted_tokens);
            const double prompt_ms    = std::max(0.0, perf_after.prompt_ms    - perf_before.prompt_ms);
            const double predicted_ms = std::max(0.0, perf_after.predicted_ms - perf_before.predicted_ms);

            // INCOMPLETE-RESPONSE SIGNAL — single transport-level fact
            // shared between webui and libeasyai-cli.  Two thresholds,
            // because "tiny" depends on whether we're mid-task or not:
            //
            //   incomplete := tool_calls.empty()
            //              && ( content_bytes < kAnnounceFloor
            //                || (last_is_tool && content_bytes < kPostToolFloor) )
            //
            //  - kAnnounceFloor (80 B) catches the FIRST-turn pattern
            //    "Let me search…" / "I'll write the file now" — tiny
            //    reply, no tool_call, the user got nothing useful.
            //
            //  - kPostToolFloor (350 B) catches the MID-LOOP pattern
            //    where the model has already received tool results
            //    and is expected to either dispatch the next tool or
            //    synthesise a real deliverable, but instead emits a
            //    medium-size narration like "I've gathered substantial
            //    content from AP and BBC.  Let me fetch one more
            //    source before compiling the report." (152 bytes).
            //    The 80-byte floor missed those — this raised floor,
            //    gated on last_is_tool, catches them without ever
            //    flagging a normal short answer (chitchat turn with
            //    no tools registered → last_is_tool=false → 80 B
            //    floor only).
            //
            // Both are pure byte counts — NO natural-language regex,
            // no announcement pattern matching.  When this fires the
            // server just sets timings.incomplete=true; webui and
            // libeasyai-cli decide policy (Client::retry_on_incomplete
            // or render-placeholder).
            constexpr size_t kAnnounceFloor = 80;
            constexpr size_t kPostToolFloor = 350;

            // Announce-pattern detector — same intent as Engine's
            // retry_on_incomplete heuristic.  A *short* reply alone is
            // not enough to call something "incomplete" (a greeting like
            // "Hi! How can I help today?" is a valid 30-byte turn); it
            // only counts when content is empty OR the visible text is
            // a tool-announce narration ("Let me search…", "I'll look
            // that up…").
            auto looks_like_announce = [](const std::string & s) -> bool {
                if (s.empty()) return true;
                std::string lc;
                lc.reserve(s.size());
                for (char c : s) lc.push_back((char) std::tolower((unsigned char) c));
                static const char * patterns[] = {
                    "let me ",        "i'll ",          "i will ",
                    "i'm going to ",  "i am going to ", "let's ",
                    "one moment",     "hold on",        "give me a moment",
                    "give me a sec",  "searching ",     "looking up",
                    "looking that up","checking the",   "fetching ",
                    "i'll check",     "i'll look",      "i'll search",
                };
                for (const char * p : patterns) {
                    if (lc.find(p) != std::string::npos) return true;
                }
                return false;
            };

            // The local `tool_calls` vector is only populated in
            // client_tools mode; on the agentic path the engine
            // dispatches tools internally and that vector stays empty.
            // Use `engine_tool_dispatches` to also exclude requests
            // where the engine DID call tools — even an empty final
            // visible reply is not "incomplete" if the model spent
            // five web fetches getting there. The user typically sees
            // the tool dispatches in the Reasoning panel and the
            // server's last-resort fallback also paints the bubble
            // with the engine's promoted reasoning. Flagging that as
            // "incomplete" with a "no tool_call" warning is just
            // misleading.
            const bool incomplete =
                tool_calls.empty()
                && engine_tool_dispatches == 0
                && (content_bytes_emitted < kAnnounceFloor
                    || (req_state->last_is_tool
                        && content_bytes_emitted < kPostToolFloor))
                && looks_like_announce(content_text_emitted);
            if (incomplete) {
                std::fprintf(stderr,
                    "[easyai-server] WARN incomplete response (content_bytes=%zu, "
                    "engine_tools=%d, engine_retries=%d, prompt_n=%d, "
                    "predicted_n=%d, finish_reason=%s).  "
                    "Common causes: model announced a tool but didn't emit it, "
                    "tool-error chain (rate-limit), over-prescriptive system "
                    "prompt, model exhausted on a niche question. Bump "
                    "--max-incomplete-retries (default 10) if the model needs "
                    "more chances on every turn.\n",
                    content_bytes_emitted, engine_tool_dispatches,
                    engine_incomplete_retries, prompt_n, predicted_n,
                    finish_reason.c_str());
                std::string content_snippet = content_text_emitted;
                if (content_snippet.size() > 400)
                    content_snippet = content_snippet.substr(0, 400) + "…";
                easyai::log::mark_problem(
                    "Server: stream marked incomplete=true\n"
                    "content_bytes=%zu predicted_n=%d prompt_n=%d "
                    "finish_reason=%s last_is_tool=%s\n"
                    "content text: %s",
                    content_bytes_emitted, predicted_n, prompt_n,
                    finish_reason.c_str(),
                    req_state->last_is_tool ? "yes" : "no",
                    content_snippet.c_str());
            }

            ordered_json done_delta;
            done_delta["choices"] = json::array({{
                {"index", 0},
                {"delta", json::object()},
                {"finish_reason", finish_reason},
            }});
            // NOTE: do NOT set "agentic" as a boolean — the embedded webui
            // expects timings.agentic to be an object of shape
            // {llm:{prompt_n,prompt_ms,predicted_n,predicted_ms},perTurn:[…]}
            // and crashes the stats component when it gets a primitive.
            // Leaving the field out makes the renderer fall back to the
            // top-level timings, which is what we want.
            // Cumulative session usage for the bar's ctx counter.
            // perf_after.n_ctx_used is the live KV-cache fill — exactly
            // how many tokens of n_ctx the chat history is occupying
            // RIGHT NOW, including system prompt + every prior turn.
            // We also send n_ctx so the webui doesn't need /props for
            // the denominator.  cache_n keeps its prior meaning (tokens
            // already in cache before THIS turn started) for callers
            // that cared about the prefix-match cache hit ratio.
            const int n_ctx_total = ctx.engine.n_ctx();
            const int n_ctx_used  = perf_after.n_ctx_used;
            done_delta["timings"] = {
                {"prompt_n",     prompt_n},
                {"prompt_ms",    prompt_ms},
                {"predicted_n",  predicted_n},
                {"predicted_ms", predicted_ms},
                {"cache_n",      perf_before.n_ctx_used},
                {"ctx_used",     n_ctx_used},
                {"n_ctx",        n_ctx_total},
                // Single transport-level signal (see above).  The webui
                // injection (block 4 SSE handler) and libeasyai-cli's
                // Client both read `timings.incomplete` to decide
                // whether to surface a placeholder / trigger an opt-in
                // retry.  Server stays policy-free.
                {"incomplete",   incomplete},
            };
            done_delta["usage"] = {
                {"prompt_tokens",     prompt_n},
                {"completion_tokens", predicted_n},
                {"total_tokens",      prompt_n + predicted_n},
            };
            emit_data(safe_dump(done_delta));
            emit_data("[DONE]");

            // ALWAYS log the produced assistant turn — exactly the data
            // the operator needs to correlate "model misbehaved" reports
            // against what actually crossed the wire.  Stays off stderr
            // (still gated on --verbose); only the /tmp raw log gets
            // this block.
            if (auto * fp = easyai::log::file()) {
                std::fprintf(fp,
                    "----- RESPONSE (stream) -----\n"
                    "finish_reason=%s incomplete=%s prompt_n=%d predicted_n=%d "
                    "content_bytes=%zu accumulated_bytes=%zu tool_calls=%zu\n"
                    "----- ASSISTANT CONTENT -----\n%s\n"
                    "----- ACCUMULATED RAW (model output incl. tool_call markup) -----\n%s\n",
                    finish_reason.c_str(), incomplete ? "yes" : "no",
                    prompt_n, predicted_n,
                    content_bytes_emitted, accumulated.size(),
                    tool_calls.size(),
                    content_text_emitted.c_str(),
                    accumulated.c_str());
                if (!tool_calls.empty()) {
                    std::fputs("----- TOOL CALLS -----\n", fp);
                    for (size_t i = 0; i < tool_calls.size(); ++i) {
                        const std::string & tid = i < tool_call_ids.size()
                                                       ? tool_call_ids[i]
                                                       : std::string();
                        std::fprintf(fp,
                            "tool_call[%zu]: name=%s id=%s args=%s\n",
                            i, tool_calls[i].first.c_str(),
                            tid.empty() ? "(none)" : tid.c_str(),
                            tool_calls[i].second.c_str());
                    }
                }
                std::fputs("==========\n", fp);
                std::fflush(fp);
            }

            // If we got here because the client dropped, leave a clear
            // marker in the raw log — operators looking at "why did the
            // server suddenly stop generating" should not have to guess.
            if (ctx.engine.cancel_requested()) {
                std::fprintf(stderr,
                    "[easyai-server] turn cancelled (client disconnected)\n");
                if (auto * fp = easyai::log::file()) {
                    std::fputs("----- CANCELLED (client dropped) -----\n", fp);
                    std::fflush(fp);
                }
            }

            sink.done();
            return true;
        });
}

// ---------------------------------------------------------------------------
// POST /v1/chat/completions — dispatch to sync or stream path.
// ---------------------------------------------------------------------------
static void route_chat_completions(ServerCtx & ctx, const httplib::Request & req,
                                   httplib::Response & res) {
    // ALWAYS log the full incoming POST body + summary to the raw
    // transaction log (auto-opened by Engine::load).  No verbose flag
    // gate: this is exactly the data the operator needs when a turn
    // misbehaves — we suspect the OpenAI-protocol round-trip is
    // dropping tool_calls / tool_call_id from history (see audit
    // notes in the commit), and capturing the wire-level body lets us
    // confirm what every client actually sent.  Stderr is left alone
    // (still gated on --verbose) so the journal doesn't drown.
    if (auto * fp = easyai::log::file()) {
        char ts[32] = {0};
        const auto t = std::time(nullptr);
        std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
        std::fprintf(fp,
            "\n========== REQUEST %s ==========\n"
            "POST %s  remote=%s  body_bytes=%zu  ct=%s\n"
            "----- HEADERS -----\n",
            ts, req.path.c_str(),
            req.remote_addr.c_str(), req.body.size(),
            req.get_header_value("Content-Type").c_str());
        for (const auto & h : req.headers) {
            // Redact bearer auth so log files are safe to share.
            const std::string & k = h.first;
            std::string v = h.second;
            if (k == "Authorization" || k == "X-Easyai-Api-Key") v = "<redacted>";
            std::fprintf(fp, "%s: %s\n", k.c_str(), v.c_str());
        }
        std::fprintf(fp, "----- BODY -----\n%s\n", req.body.c_str());
        std::fflush(fp);
    }

    auto state = std::make_shared<ChatRequest>();
    if (!parse_chat_request(req, res, *state)) {
        easyai::log::mark_problem(
            "Server: parse_chat_request rejected POST /v1/chat/completions "
            "(http_status=%d body_bytes=%zu)\nbody: %.*s",
            res.status, req.body.size(),
            (int) std::min<size_t>(2048, req.body.size()),
            req.body.c_str());
        return;
    }

    ctx.n_requests.fetch_add(1, std::memory_order_relaxed);

    if (auto * fp = easyai::log::file()) {
        std::fprintf(fp,
            "----- PARSED REQUEST -----\n"
            "client_tools=%s stream=%s tools=%zu hist=%zu "
            "last_user_bytes=%zu last_is_tool=%s inject_override=%s\n",
            state->client_tools ? "yes" : "no",
            state->stream       ? "yes" : "no",
            state->client_tools ? state->tools_blob.size() : ctx.default_tools.size(),
            state->hist.size(),
            state->last_user.size(),
            state->last_is_tool ? "yes" : "no",
            state->inject_override.empty() ? "(default)" : state->inject_override.c_str());
        std::fflush(fp);
    }

    if (ctx.verbose) {
        std::fprintf(stderr,
            "[easyai-server] POST /v1/chat/completions  client_tools=%s "
            "stream=%s tools=%zu hist=%zu last_user_bytes=%zu inject_override=%s\n",
            state->client_tools ? "yes" : "no",
            state->stream       ? "yes" : "no",
            state->client_tools ? state->tools_blob.size() : ctx.default_tools.size(),
            state->hist.size(),
            state->last_user.size(),
            state->inject_override.empty() ? "(default)" : state->inject_override.c_str());
    }

    if (state->stream) {
        // Streaming path: lock + engine work happen inside the chunked
        // content provider lambda (which runs after this function returns).
        handle_chat_stream(ctx, state, res);
    } else {
        std::lock_guard<std::mutex> lock(ctx.engine_mu);
        ctx.reset_engine_defaults();
        prepare_engine_for_request(ctx, *state);
        handle_chat_sync(ctx, *state, res);
    }
}

// ---------------------------------------------------------------------------
// GET /v1/models
// ---------------------------------------------------------------------------
static void route_models(ServerCtx & ctx, const httplib::Request &,
                         httplib::Response & res) {
    ordered_json env;
    env["object"] = "list";
    env["data"]   = json::array({{
        {"id",      ctx.model_id},
        {"object",  "model"},
        {"created", 0},
        {"owned_by","easyai"},
    }});
    res.set_content(env.dump(), "application/json");
}

// ---------------------------------------------------------------------------
// GET /api/tags  →  Ollama-compat list-models response.
// ---------------------------------------------------------------------------
// Some clients (LobeChat, OpenWebUI in Ollama mode, Continue's
// Ollama provider, custom integrations) discover available models
// by hitting /api/tags. We expose the same single model that
// /v1/models exposes — easyai-server is single-model — but
// rendered in Ollama's shape so those clients work out of the box.
//
// Ollama's response is:
//   { "models": [ { "name", "modified_at", "size", "digest", "details": {...} } ] }
//
// We don't compute a real digest / size from the GGUF (would
// require a stat at startup); placeholders are fine — the field is
// only used by Ollama clients to decide whether to re-pull, which
// doesn't apply to us.
static void route_ollama_tags(ServerCtx & ctx, const httplib::Request &,
                              httplib::Response & res) {
    ordered_json m;
    m["name"]        = ctx.model_id;
    m["model"]       = ctx.model_id;       // newer Ollama clients prefer this
    m["modified_at"] = "1970-01-01T00:00:00Z";
    m["size"]        = 0;
    m["digest"]      = "";
    ordered_json details;
    details["format"]              = "gguf";
    details["family"]              = "easyai";
    details["families"]            = json::array({"easyai"});
    details["parameter_size"]      = "";
    details["quantization_level"]  = "";
    m["details"] = std::move(details);

    ordered_json env;
    env["models"] = json::array({m});
    res.set_content(env.dump(), "application/json");
}

// GET /api/show  →  Ollama-compat single-model detail.
// ---------------------------------------------------------------------------
// Some clients post `{"name": "<id>"}` to /api/show to inspect a
// model's metadata. We return the same details block /api/tags
// emits, plus a `modelfile` placeholder.
static void route_ollama_show(ServerCtx & ctx, const httplib::Request & req,
                              httplib::Response & res) {
    // The body may be {"name": "..."} or {"model": "..."}; we don't
    // care which — we only ever serve the one model loaded.
    (void) req;
    ordered_json env;
    env["modelfile"]   = "# easyai-server (single model loaded)";
    env["parameters"]  = "";
    env["template"]    = "";
    ordered_json details;
    details["format"]              = "gguf";
    details["family"]              = "easyai";
    details["families"]            = json::array({"easyai"});
    details["parameter_size"]      = "";
    details["quantization_level"]  = "";
    env["details"] = std::move(details);
    env["model_info"] = json::object();
    res.set_content(env.dump(), "application/json");
}

// POST /mcp  →  Model Context Protocol (JSON-RPC 2.0).
// ---------------------------------------------------------------------------
// Stateless request/response. Other AI applications (Claude
// Desktop via stdio bridge, Cursor / Continue over HTTP, custom
// JSON-RPC clients) call here to enumerate easyai's tool catalogue
// and dispatch tools by name. The protocol's full surface is
// documented in MCP.md; this handler is just the JSON-RPC pipe.
//
// Auth posture is opt-in via the [MCP_USER] section of
// /etc/easyai/easyai.ini (or whatever --config points at):
//
//   - section absent / empty           → MCP open (no auth)
//   - section has at least one user    → Bearer token required;
//                                         the username on the
//                                         matching entry is logged
//                                         per request for audit
//
// This kept zero-friction local-dev open by default while letting
// production ops drop a key in the INI without a restart cadence
// other than the SAME `systemctl restart easyai-server` that
// applies to every config change.

// check_mcp_auth — returns true if the request is authorised (or if
// MCP auth is OPEN). On 401, fills `res` and returns false. On
// success, sets `user_out` to the username from [MCP_USER] (or
// empty when MCP is open).
//
// Thin wrapper around easyai::mcp::check_bearer (in libeasyai) so
// this binary and easyai-mcp-server share the same auth logic. The
// transport-specific bits (header read, response write) stay here.
static bool check_mcp_auth(ServerCtx &                ctx,
                           const httplib::Request &   req,
                           std::string &              user_out,
                           httplib::Response &        res) {
    auto verdict = easyai::mcp::check_bearer(
        ctx.mcp_keys, req.get_header_value("Authorization"));
    if (verdict.ok) {
        user_out = std::move(verdict.user);
        return true;
    }
    res.status = verdict.status;
    if (!verdict.www_authenticate.empty()) {
        res.set_header("WWW-Authenticate", verdict.www_authenticate);
    }
    res.set_content(verdict.body, "application/json");
    user_out.clear();
    return false;
}

static void route_mcp(ServerCtx & ctx, const httplib::Request & req,
                      httplib::Response & res) {
    std::string mcp_user;
    if (!check_mcp_auth(ctx, req, mcp_user, res)) return;

    if (!mcp_user.empty()) {
        // Audit log — operator can `journalctl -u easyai-server | grep "[mcp]"`
        // to see who hit the endpoint. We log per-request without the
        // method body because tool arguments may carry sensitive
        // values (file contents, secrets the model was told to save).
        std::fprintf(stderr,
            "[mcp] request from user '%s'\n", mcp_user.c_str());
    }

    easyai::mcp::ServerInfo info;
    info.name             = "easyai-server";
    info.version          = "0.1.0";
    info.protocol_version = "2024-11-05";

    // Hold the engine mutex briefly only if a tool dispatch ends up
    // touching engine state — we DON'T need the lock for the
    // dispatcher itself (pure function over `default_tools`). Tool
    // handlers serialise themselves where they need to.
    std::string body = easyai::mcp::handle_request(
        req.body, ctx.default_tools, info);

    if (body.empty()) {
        // Notification (request without `id`) per JSON-RPC 2.0:
        // server MUST NOT respond. Use 204 No Content.
        res.status = 204;
        return;
    }
    res.set_content(body, "application/json");
}

// GET /v1/tools  →  tool catalog so the webui can render a popover with
// {name, description} pairs.  Read-only; no auth needed when the rest of
// /v1/* is open, gated by require_auth otherwise (wired at route mount).
// ---------------------------------------------------------------------------
static void route_tools(ServerCtx & ctx, const httplib::Request &,
                        httplib::Response & res) {
    ordered_json arr = json::array();
    for (const auto & t : ctx.default_tools) {
        ordered_json e;
        e["name"]        = t.name;
        e["description"] = t.description;
        arr.push_back(std::move(e));
    }
    ordered_json env;
    env["object"] = "list";
    env["data"]   = std::move(arr);
    res.set_content(env.dump(), "application/json");
}

// ---------------------------------------------------------------------------
// POST /v1/preset  body: {"preset": "creative"}  → ambient defaults change.
// ---------------------------------------------------------------------------
static void route_preset(ServerCtx & ctx, const httplib::Request & req,
                         httplib::Response & res) {
    try {
        json b = json::parse(req.body);
        std::string name = b.value("preset", "precise");
        const easyai::Preset * p = easyai::find_preset(name);
        if (!p) {
            res.status = 400;
            res.set_content(error_json("unknown preset: " + name), "application/json");
            return;
        }
        std::lock_guard<std::mutex> lock(ctx.engine_mu);
        ctx.apply_preset(*p);
        res.set_content(json{{"applied", p->name}}.dump(), "application/json");
    } catch (const std::exception & e) {
        res.status = 400;
        res.set_content(error_json(std::string("bad request: ") + e.what()),
                        "application/json");
    }
}

// ---------------------------------------------------------------------------
// GET /health   →  small status JSON.
// ---------------------------------------------------------------------------
static void route_health(ServerCtx & ctx, const httplib::Request &,
                         httplib::Response & res) {
    ordered_json j;
    j["status"]  = "ok";
    j["model"]   = ctx.model_id;
    j["backend"] = ctx.engine.backend_summary();
    j["tools"]   = ctx.default_tools.size();
    j["preset"]  = ctx.default_preset.name;

    // Compatibility shims this server speaks. Lets clients
    // auto-detect which API they can use without trial requests.
    ordered_json compat;
    compat["openai"]   = "/v1/chat/completions";
    compat["ollama"]   = "/api/tags";
    compat["mcp"]      = "/mcp";
    compat["mcp_protocol"] = "2024-11-05";
    j["compat"] = std::move(compat);

    res.set_content(j.dump(), "application/json");
}

// ---------------------------------------------------------------------------
// GET /metrics  →  Prometheus-style text exposition.
// Always available when --metrics was passed at start-up.
// ---------------------------------------------------------------------------
static void route_metrics(ServerCtx & ctx, const httplib::Request &,
                          httplib::Response & res) {
    std::ostringstream o;
    o << "# HELP easyai_requests_total Total /v1/chat/completions requests received.\n"
      << "# TYPE easyai_requests_total counter\n"
      << "easyai_requests_total " << ctx.n_requests.load() << "\n"
      << "# HELP easyai_errors_total Total chat-completion handler errors.\n"
      << "# TYPE easyai_errors_total counter\n"
      << "easyai_errors_total " << ctx.n_errors.load() << "\n"
      << "# HELP easyai_tool_calls_total Total tool_calls returned to clients.\n"
      << "# TYPE easyai_tool_calls_total counter\n"
      << "easyai_tool_calls_total " << ctx.n_tool_calls.load() << "\n";
    res.set_content(o.str(), "text/plain; version=0.0.4");
}

// ---------------------------------------------------------------------------
// Bearer-token auth check.  Returns true to continue; on failure the response
// is filled with 401 and the caller should bail.
// ---------------------------------------------------------------------------
static bool require_auth(const ServerCtx & ctx, const httplib::Request & req,
                         httplib::Response & res) {
    if (ctx.api_key.empty()) return true;  // auth disabled
    auto it = req.headers.find("Authorization");
    // Bound the header value before string-comparing against the expected
    // token. Without this, a hostile client could send a multi-megabyte
    // Authorization header on every probe and force a comparable-sized
    // string allocation + scan per request. Same cap as easyai::mcp's
    // /mcp gate uses.
    if (it != req.headers.end() &&
            it->second.size() > easyai::mcp::kMaxAuthHeaderBytes) {
        res.status = 401;
        res.set_content(error_json("Authorization header too large",
                                   "authentication_error"),
                        "application/json");
        return false;
    }
    std::string expected = "Bearer " + ctx.api_key;
    if (it == req.headers.end() || it->second != expected) {
        res.status = 401;
        res.set_content(error_json("missing or invalid Bearer token",
                                   "authentication_error"),
                        "application/json");
        return false;
    }
    return true;
}

// ============================================================================
// main
// ============================================================================

[[noreturn]] static void die_usage(const char * argv0) {
    std::fprintf(stderr,
        "Usage: %s -m model.gguf [options]\n\n"
        "Required:\n"
        "  -m, --model <path>           GGUF model file\n"
        "\nNetwork:\n"
        "      --host <addr>            Bind address (default 127.0.0.1).\n"
        "                                Use 0.0.0.0 to listen on every IPv4\n"
        "                                interface (LAN, docker, etc.).\n"
        "      --port <n>               TCP port (default 8080)\n"
        "      --max-body <bytes>       Max request body size (default 8 MiB)\n"
        "      --config <path>          Central INI config file. Default\n"
        "                                /etc/easyai/easyai.ini. Provides\n"
        "                                defaults for almost every flag\n"
        "                                below — explicit CLI flags override\n"
        "                                the INI value. Sections: [SERVER],\n"
        "                                [ENGINE], [MCP_USER] (Bearer-token\n"
        "                                auth for /mcp). Missing file = use\n"
        "                                hardcoded defaults + open MCP.\n"
        "      --no-mcp-auth            Force /mcp to accept requests with\n"
        "                                NO Bearer token even if the INI's\n"
        "                                [MCP_USER] section has entries.\n"
        "                                Emergency/dev override — disables\n"
        "                                what the INI explicitly enabled.\n"
        "\nDefault system prompt + tools:\n"
        "  -s, --system-file <path>     Server-default system prompt from file\n"
        "      --system <text>          Inline system prompt\n"
        "      --no-local-tools         Don't expose the LOCAL built-in\n"
        "                                toolbelt (datetime, web_*, fs_*,\n"
        "                                bash, etc). Has no effect on memory\n"
        "                                (--memory), external tools\n"
        "                                (--external-tools), or remote\n"
        "                                tools fetched via --mcp — those\n"
        "                                are governed by their own flags.\n"
        "      --mcp <url>              Connect to a remote MCP server as\n"
        "                                a CLIENT and merge its tool\n"
        "                                catalogue into ours. Format:\n"
        "                                http(s)://host:port (the /mcp\n"
        "                                endpoint is appended). Local-tool\n"
        "                                names take precedence on collision.\n"
        "      --mcp-token <token>      Bearer token for --mcp. Empty (the\n"
        "                                default) sends no Authorization\n"
        "                                header — use this when the\n"
        "                                upstream is in open mode.\n"
        "      --http-retries N         Extra attempts on transient HTTP\n"
        "                                failures (connect refused, read\n"
        "                                timeout, 5xx). Applies to the MCP\n"
        "                                client (--mcp), the unified `web`\n"
        "                                tool, and any other libcurl-based\n"
        "                                tool. 0 disables. Default 5.\n"
        "                                Each retry logs to stderr.\n"
        "      --http-timeout SECONDS   HTTP read/write timeout for the\n"
        "                                listen socket AND the MCP-client\n"
        "                                connection. Bumped from llama-server's\n"
        "                                60 s to 600 s default to accommodate\n"
        "                                long thinking turns. INI key:\n"
        "                                [SERVER] http_timeout. Logged\n"
        "                                unconditionally at startup.\n"
        "      --sandbox <dir>          Set the working root for the unified\n"
        "                                `fs` tool and `bash` AND the cwd /\n"
        "                                external-tools root / fs(action=\n"
        "                                \"sandbox\") target. Setting --sandbox\n"
        "                                alone does NOT register fs / bash —\n"
        "                                pass --allow-fs / --allow-bash for that.\n"
        "                                Without --sandbox the tools default\n"
        "                                to the server's cwd.\n"
        "      --allow-fs               Register the unified `fs` tool\n"
        "                                (action=read / write / list / glob /\n"
        "                                grep / check_path / cwd / sandbox).\n"
        "                                Scoped to --sandbox dir if given,\n"
        "                                otherwise the server's cwd.\n"
        "                                Required even when --sandbox is set;\n"
        "                                INI [SERVER] allow_fs=on is equivalent.\n"
        "      --allow-bash             Register the `bash` tool (run shell\n"
        "                                commands). cwd = --sandbox dir if\n"
        "                                given, otherwise the server's cwd.\n"
        "                                NOT a hardened sandbox — the\n"
        "                                command runs with the server's\n"
        "                                user privileges.\n"
        "      --no-python              Drop the `python3` tool. By default\n"
        "                                it auto-registers when --sandbox is\n"
        "                                set or --allow-bash is on. Stdlib-\n"
        "                                only interpreter (no PYTHON* env,\n"
        "                                no site-packages, no cwd on\n"
        "                                sys.path); disk access is auto-\n"
        "                                restricted to the sandbox root via\n"
        "                                a Python preamble. NOT a hardened\n"
        "                                sandbox — the interpreter has full\n"
        "                                uid/gid and `import os` /\n"
        "                                `import socket` / `import\n"
        "                                subprocess` all work. Use\n"
        "                                --no-python (or [SERVER]\n"
        "                                allow_python = off) to opt out.\n"
        "      --use-google             Enable engine=\"google\" inside the\n"
        "                                unified `web` tool (Google Custom\n"
        "                                Search JSON API). Requires both\n"
        "                                GOOGLE_API_KEY and GOOGLE_CSE_ID env\n"
        "                                vars; counts against Google's quota\n"
        "                                (free tier: 100 queries/day per key).\n"
        "                                The default engine \"ddg\" works\n"
        "                                without env vars and any opt-in.\n"
        "      --external-tools <dir>   Load every EASYAI-*.tools file in <dir>\n"
        "                                as an external-tools manifest. Empty\n"
        "                                directory is a normal state (no extra\n"
        "                                tools). Per-file errors are logged and\n"
        "                                skipped; other files still load. The\n"
        "                                server logs every load error AND every\n"
        "                                security sanity-check warning to stderr.\n"
        "                                See EXTERNAL_TOOLS.md for the schema and\n"
        "                                collaboration workflow.\n"
        "      --memory <dir>           Enable the agent's persistent memory\n"
        "                                store (alias: --RAG). Each entry is\n"
        "                                one Markdown file in <dir>.\n"
        "                                Registers ONE `memory(action=...)` tool\n"
        "                                with sub-actions save / append /\n"
        "                                search / load / list / delete /\n"
        "                                keywords. The installed systemd unit\n"
        "                                always passes this flag; manual\n"
        "                                invocations need it explicitly. See\n"
        "                                RAG.md.\n"
        "\nModel tuning (apply on top of --preset):\n"
        "      --preset <name>          Ambient preset (default 'precise').\n"
        "                                Choices: deterministic, precise,\n"
        "                                balanced, creative, wild. See\n"
        "                                README.md for what each implies.\n"
        "      --temperature <f>        Override temperature (0.0-2.0)\n"
        "      --top-p <f>              Override nucleus sampling p\n"
        "      --top-k <n>              Override top-k\n"
        "      --min-p <f>              Override min-p\n"
        "      --repeat-penalty <f>     Repetition penalty (default 1.15 —\n"
        "                                anti-loop safety net; pass 1.0 to\n"
        "                                disable).  INI: [ENGINE] repeat_penalty.\n"
        "      --presence-penalty <f>   Presence penalty ([-2.0, 2.0]; OpenAI\n"
        "                                semantics — fixed amount per token\n"
        "                                that has already appeared at all,\n"
        "                                regardless of frequency). Default 0.0\n"
        "                                (disabled). INI: [ENGINE] presence_penalty.\n"
        "      --max-tokens <n>         Cap tokens generated per request\n"
        "      --seed <u32>             RNG seed (0 = random)\n"
        "      --max-incomplete-retries <n>\n"
        "                               How many times the engine retries when\n"
        "                                the model finishes a turn with no\n"
        "                                tool_call and only an 'announce'\n"
        "                                snippet ('Let me…', 'I'll…'). Each\n"
        "                                retry discards the bad turn, appends\n"
        "                                a corrective user message, and runs\n"
        "                                the model again. Default 10 — sane\n"
        "                                floor for 1-bit quants (Bonsai,\n"
        "                                BitNet); 0 disables retries; bump to\n"
        "                                15-20 for weak models that keep\n"
        "                                announcing-without-acting. Each retry\n"
        "                                surfaces in the webui Thinking panel.\n"
        "\nCompute / memory:\n"
        "  -c, --ctx <n>                Context size (default 8192)\n"
        "      --batch <n>              Logical batch size (default = ctx)\n"
        "      --ngl <n>                GPU layers (-1=auto, 0=CPU)\n"
        "  -t, --threads <n>            CPU threads\n"
        "\nKV cache (all optional):\n"
        " -ctk, --cache-type-k <type>   K-cache dtype (f32|f16|bf16|q8_0|q4_0|q4_1|q5_0|q5_1|iq4_nl)\n"
        " -ctv, --cache-type-v <type>   V-cache dtype (same options)\n"
        "-nkvo, --no-kv-offload         Keep KV cache on CPU even with GPU layers\n"
        "      --kv-unified             Use a single unified KV buffer across sequences\n"
        "      --override-kv <k=t:v>    Override a GGUF metadata entry (repeatable)\n"
        "                                Types: int|float|bool|str\n"
        "\nSpeculative decoding (all optional):\n"
        "      --spec-type <type>       none (default; off), draft-mtp (MTP-trained\n"
        "                                 model, no draft file needed), draft-simple,\n"
        "                                 draft-eagle3, ngram-simple|map-k|map-k4v|\n"
        "                                 mod|cache. INI key: [ENGINE] spec_type.\n"
        "      --spec-draft-n-max <n>   Max draft tokens per speculation step. Typical\n"
        "                                 MTP: 6. Default 16 when --spec-type is set,\n"
        "                                 unused when --spec-type=none. INI key:\n"
        "                                 [ENGINE] spec_draft_n_max.\n"
        "      --draft-model <path>     GGUF path for the draft model (draft-simple /\n"
        "                                 draft-eagle3). Must share vocabulary with the\n"
        "                                 target. INI key: [ENGINE] spec_draft_model.\n"
        "\nChat template / reasoning (llama-server compat):\n"
        "      --chat-template-file <p> Override the GGUF's embedded chat template\n"
        "                                 with a Jinja file on disk (e.g. qwen3-\n"
        "                                 think.jinja). Empty = use the model's\n"
        "                                 template. INI key: [ENGINE]\n"
        "                                 chat_template_file.\n"
        "      --reasoning-format <fmt> Reasoning-content extraction format:\n"
        "                                 none|auto|deepseek|deepseek-legacy.\n"
        "                                 Default auto (≡ deepseek for Qwen3 /\n"
        "                                 R1 templates). INI key: [ENGINE]\n"
        "                                 reasoning_format.\n"
        "\nllama-server compatibility:\n"
        "  -a,  --alias <name>          Public model id reported by /v1/models\n"
        "       --api-key <key>         Require Bearer auth on every /v1 route\n"
        "  -fa, --flash-attn            Force flash attention on\n"
        "  -tb, --threads-batch <n>     Threads used for prompt-eval batches\n"
        "  -np, --parallel <n>          Accepted for compat; warns when >1\n"
        "       --mlock                 mlock model weights into RAM\n"
        "       --no-mmap               Disable mmap (read GGUF straight in)\n"
        "       --numa <strategy>       distribute|isolate|numactl|mirror\n"
        "       --metrics               Expose Prometheus /metrics endpoint\n"
        "       --metrics-interval SECS Periodic METRICS log line every SECS\n"
        "                                seconds (CPU%%, mem, GPU GTT, load\n"
        "                                avg, iowait, fd usage, TCP states\n"
        "                                incl. explicit TIME_WAIT count + %%\n"
        "                                of ephemeral port range with\n"
        "                                elevated/HIGH/CRITICAL tag).\n"
        "                                ALWAYS ON regardless of --verbose so\n"
        "                                journalctl always carries the\n"
        "                                telemetry. 0 disables. Default 300\n"
        "                                (5 min). INI: [SERVER]\n"
        "                                metrics_interval.\n"
        "       --reasoning <on|off>    Enable model thinking (default on)\n"
        "       --no-think              Strip <think>...</think> from replies\n"
        "       --inject-datetime <on|off>\n"
        "                               Append authoritative date/time + TZ + a\n"
        "                               knowledge-cutoff hint to the system prompt\n"
        "                               on EVERY request (default on).  Tells the\n"
        "                               model to trust the server's clock and to\n"
        "                               verify post-cutoff facts via tools.\n"
        "       --knowledge-cutoff <YYYY-MM>\n"
        "                               Model training-data cutoff hint used by\n"
        "                               --inject-datetime.  Default '2024-10'.\n"
        "  -v,  --verbose               Engine logs raw model output + parser\n"
        "                                 actions to stderr — useful for debugging\n"
        "                                 'why did it stop?' moments. Also logs\n"
        "                                 HTTP-level → arrival / ← completion\n"
        "                                 lines per request with running totals\n"
        "                                 (in-flight, bytes in/out, errors).\n"
        "                                 The periodic METRICS line is ALWAYS\n"
        "                                 on (default every 5 min, see\n"
        "                                 --metrics-interval) — independent of\n"
        "                                 verbose.\n"
        "       --show-system-prompt    Print the resolved persona (built-in\n"
        "                                Deep default OR --system OR --system-\n"
        "                                file content, in the same precedence\n"
        "                                the running server uses) and exit\n"
        "                                before any port is bound. Useful for\n"
        "                                operators inspecting the persona\n"
        "                                without bouncing the service.\n"
        "\nWebui:\n"
        "       --webui <mode>          'modern' (default — embedded llama-server-\n"
        "                                derived bundle) or 'minimal' (small inline UI)\n"
        "       --webui-title <text>    Title in the browser tab AND the sidebar\n"
        "                                brand (default 'Box EasyAI')\n"
        "       --webui-icon <path>     Favicon file (.ico|.png|.svg|.gif|.jpg|.webp);\n"
        "                                also rendered before the brand title in the\n"
        "                                sidebar / topbar\n"
        "       --webui-placeholder <s> Input box placeholder (default 'Type a message…')\n"
        "\n  -h, --help                   Show this help and exit\n",
        argv0);
    std::exit(1);
}

struct ServerArgs {
    std::string model_path;
    std::string system_path;
    std::string config_path = "/etc/easyai/easyai.ini";   // INI; missing = open MCP
    std::string system_inline;
    std::string host       = "127.0.0.1";   // pass 0.0.0.0 for any-iface
    int         port       = 8080;
    int         n_ctx      = 8192;
    int         n_batch    = 0;             // 0 = follow ctx
    int         ngl        = -1;
    int         n_threads  = 0;
    bool        local_tools = true;  // master switch for the LOCAL built-in
                                     // toolbelt (datetime, web_*, fs_*, bash,
                                     // RAG when --RAG is set, ...). Has no
                                     // effect on remote tools fetched via
                                     // --mcp; those are governed only by
                                     // whether --mcp is set.
    std::string mcp_url;             // optional MCP server URL to connect to
                                     // as a CLIENT (consumes its tools).
    std::string mcp_token;           // Bearer token for the upstream MCP
                                     // server; empty → no auth header.
    int         http_retries = 5;    // extra attempts on transient HTTP
                                     // failures (MCP client + web tools)
    int         http_timeout = 600;  // listen socket + MCP-client read/write
                                     // timeout in seconds.  Bumped from
                                     // llama-server's 60 s default to give
                                     // long-thinking models room to breathe
    int         metrics_interval = 300; // periodic metrics tick (seconds).
                                       // ALWAYS ON (no longer gated on
                                       // --verbose) so operators see
                                       // CPU / mem / TCP-state / TIME_WAIT
                                       // pressure in journalctl without
                                       // having to flip the noise level.
                                       // 0 disables. Default 300 (5 min)
                                       // — low-overhead enough to leave
                                       // on permanently. CLI:
                                       // --metrics-interval N. INI:
                                       // [SERVER] metrics_interval.
                                     // before the network drops them.
    std::string sandbox;            // optional: scope `fs` / `bash` to this dir
    bool        allow_fs     = false; // explicit opt-in for the unified `fs` tool
    bool        allow_bash   = false; // explicit opt-in for the `bash` tool
    // python3 defaults ON: stdlib-only interpreter with disk access
    // auto-restricted to the sandbox root via a Python preamble.
    // Auto-registers when sandbox is set OR allow_bash is on. The
    // server's webui inherits it for free since the systemd unit
    // ships with --sandbox /var/lib/easyai/workspace. Operators who
    // want to disable it pass --no-python or set [SERVER]
    // allow_python = off.
    bool        allow_python = true;
    bool        use_google = false; // enable engine="google" inside the unified
                                    // `web` tool (also requires GOOGLE_API_KEY +
                                    // GOOGLE_CSE_ID env vars)
    std::string external_tools_dir; // optional: dir of EASYAI-*.tools files
    std::string rag_dir;             // optional: RAG persistent-registry dir
    // Default preset: "precise" (temp=0.2, top_p=0.95, top_k=40, min_p=0.10).
    // Tuned for code, math, and factual Q&A — the dominant use case for
    // a tool-calling agent. Override with --preset / `[ENGINE] preset` /
    // POST /v1/preset (the webui's preset bar) at runtime.
    std::string preset     = "precise";
    size_t      max_body   = 8u * 1024u * 1024u;

    // Authoritative date/time injection — see build_authoritative_preamble
    // in this file.  ON by default because most users want the model to
    // trust the wall clock instead of guessing about "today".
    bool        inject_datetime  = true;
    std::string knowledge_cutoff = "2024-10";

    // Auto-retry-with-nudge cap. When the model finishes a turn with no
    // tool_call AND only an "announce" snippet ("Let me…", "I'll…"),
    // the engine discards that turn, appends a corrective synthetic
    // user message, and retries. Default 10 — high enough to recover
    // a 1-bit quant (Bonsai, BitNet) that's struggling, low enough
    // not to burn tokens on a hopeless prompt. Set 0 to disable.
    int         max_incomplete_retries = 10;

    // sampling overrides (apply on top of preset)
    float       temperature    = -1.0f;
    float       top_p          = -1.0f;
    int         top_k          = -1;
    float       min_p          = -1.0f;
    // 1.15 by default — anti-loop safety net for thinking models that
    // sometimes rephrase their own intent ("I'll write X / Let me write
    // X / OK, creating X" forever). Set repeat_penalty = 1.0 in the INI
    // (or pass --repeat-penalty 1.0) to disable.
    float       repeat_penalty = 1.15f;
    // Presence penalty (OpenAI semantics: fixed token-already-seen
    // penalty, range [-2.0, 2.0], default 0.0 = disabled). Sentinel
    // -2.0f leaves the engine default (no penalty); any value strictly
    // greater than -2.0 wins (so -2.0 itself is treated as "unset" —
    // matches the Client's convention in src/client.cpp).
    float       presence_penalty = -2.0f;
    int         max_tokens     = -1;
    uint32_t    seed           = 0u;

    // KV cache controls
    std::string cache_type_k;
    std::string cache_type_v;
    bool        no_kv_offload = false;
    bool        kv_unified    = false;
    std::vector<std::string> kv_overrides;

    // Speculative decoding — see Engine::spec_type for the accepted
    // string values. "none" (default) is autoregressive. The MTP path
    // (--spec-type draft-mtp + --spec-draft-n-max 6) needs an MTP-
    // trained model (DeepSeek V3, MimoVL, etc.); other models will
    // refuse to load or run autoregressive anyway.
    std::string spec_type;             // empty → leave Engine default ("none")
    int         spec_draft_n_max = 0;  // 0 → leave Engine default (16)
    std::string spec_draft_model;      // GGUF path for draft-simple / eagle3

    // Chat-template override (--chat-template-file). Empty = use the
    // template embedded in the GGUF. Mirrors llama-server's flag of the
    // same name; the file is loaded once at Engine::load() time.
    std::string chat_template_file;
    // Reasoning extraction format (--reasoning-format). Accepts
    // "none" | "auto" | "deepseek" | "deepseek-legacy". Empty = keep the
    // Engine default ("auto", which behaves like "deepseek").
    std::string reasoning_format;

    // llama-server compatibility / production knobs
    std::string alias;            // exposed as model id in /v1/models
    std::string api_key;          // Bearer token required if set
    std::string numa;             // distribute|isolate|numactl|mirror
    int         threads_batch  = 0;
    int         parallel       = 1;
    bool        flash_attn     = false;
    bool        mlock          = false;
    bool        no_mmap        = false;
    bool        metrics        = false;
    bool        reasoning      = true;   // enable_thinking flag default ON
    bool        no_think       = false;  // strip <think>…</think> from /v1 responses
    bool        verbose        = false;  // engine.verbose(true) — log model raw output
    bool        show_system_prompt = false;  // print resolved persona and exit

    // webui rebrand
    std::string webui_title    = "Deep";   // the assistant's default name
    std::string webui_icon;              // optional path to .ico/.png/.svg
    std::string webui_mode     = "modern"; // "modern" (embedded llama-server fork) | "minimal" (inline)
    std::string webui_placeholder = "Type a message…";

    // /mcp auth — by INI's [MCP_USER] when populated, OPEN otherwise.
    // `--no-mcp-auth` forces OPEN even if [MCP_USER] has entries
    // (useful for emergency / dev). Set on the command line ONLY;
    // the INI cannot disable its own auth (that would be the
    // operator shooting themselves in the foot).
    bool        no_mcp_auth    = false;

    // ----- which CLI flags were explicitly passed -------------------------
    // The INI overlay applies as DEFAULTS — if the operator passed a CLI
    // flag for the same setting, the CLI wins. We track that with a set
    // populated as the parser consumes flags, then `apply_ini_to_args`
    // skips any field already in the set.
    std::set<std::string>      cli_set;
};

// =============================================================================
// FlagDef — single source of truth shared by the CLI parser and the INI
// overlay. Each entry declares ONE setting: its CLI aliases, its INI
// section/key, the canonical name used for cli_set tracking, whether
// it takes a value on the command line, and a setter lambda that
// applies a string value to the right field of `ServerArgs`.
//
// Adding a new setting = ONE entry in the kFlags() table, period.
// The CLI parser and the INI overlay both walk this table; they never
// hold flag-specific knowledge.
// =============================================================================
struct FlagDef {
    std::vector<std::string> cli;       // {"--port"} or {"-c","--ctx"}; empty = INI-only
    std::string ini_section;            // "SERVER" / "ENGINE" / "" for INI-skip
    std::string ini_key;
    std::string canonical;              // entry in cli_set when CLI sets this
    bool        takes_value = true;     // false → no-value boolean flag
    // Setter: parses `v` and writes the right field in `a`. For no-value
    // CLI flags, `v` is empty; the setter must handle that as "set true".
    std::function<void(ServerArgs &, const std::string & v)> set;
};

// ---- typed setter helpers ---------------------------------------------------
//
// Bind a member-pointer to a setter that parses a string and assigns
// the right type. Each helper handles ITS OWN parsing — INI values
// arrive as strings, CLI values arrive as strings, all flow through
// the same path.
namespace {

bool str_to_bool(const std::string & s_raw, bool def) {
    std::string s = s_raw;
    for (auto & c : s) c = (char) std::tolower((unsigned char) c);
    if (s.empty()) return true;   // CLI no-value → true
    if (s == "on" || s == "true" || s == "yes" || s == "1" ||
        s == "enable" || s == "enabled") return true;
    if (s == "off" || s == "false" || s == "no" || s == "0" ||
        s == "disable" || s == "disabled") return false;
    return def;
}

auto SET_STR(std::string ServerArgs::* f) {
    return [f](ServerArgs & a, const std::string & v) { if (!v.empty()) a.*f = v; };
}
auto SET_INT(int ServerArgs::* f) {
    return [f](ServerArgs & a, const std::string & v) {
        if (v.empty()) return;
        try { a.*f = std::stoi(v); } catch (...) {}
    };
}
auto SET_UINT32(uint32_t ServerArgs::* f) {
    return [f](ServerArgs & a, const std::string & v) {
        if (v.empty()) return;
        try { a.*f = (uint32_t) std::stoul(v); } catch (...) {}
    };
}
auto SET_SIZE(size_t ServerArgs::* f) {
    return [f](ServerArgs & a, const std::string & v) {
        if (v.empty()) return;
        try { a.*f = (size_t) std::stoll(v); } catch (...) {}
    };
}
auto SET_FLOAT(float ServerArgs::* f) {
    return [f](ServerArgs & a, const std::string & v) {
        if (v.empty()) return;
        // std::stof accepts "nan", "inf", "+inf", "-inf" without throwing.
        // Those values (passed straight through to llama.cpp's sampler)
        // are out-of-spec for every float knob we expose (temperature,
        // top_p, min_p, repeat/presence/frequency_penalty), and silently
        // taking them would let a malformed INI / typo'd CLI flag put
        // the sampler in an undefined state. Reject non-finite — the
        // INI key is then dropped (default stays in effect) and the
        // operator sees no surprise.
        try {
            float parsed = std::stof(v);
            if (!std::isfinite(parsed)) return;
            a.*f = parsed;
        } catch (...) {}
    };
}
// SET_BOOL_TRUE: CLI no-value → field=true; INI/value → parse string.
// Used for the typical flag-only bool (--metrics, --mlock, -fa, …).
auto SET_BOOL_TRUE(bool ServerArgs::* f) {
    return [f](ServerArgs & a, const std::string & v) {
        a.*f = str_to_bool(v, a.*f);
    };
}
// SET_BOOL_FALSE: CLI no-value → field=false; INI/value → parse string.
// Used for negative-polarity flags (e.g. --no-local-tools sets
// local_tools=false). INI key in this case is the POSITIVE name; INI
// parses normally.
auto SET_BOOL_FALSE(bool ServerArgs::* f) {
    return [f](ServerArgs & a, const std::string & v) {
        // CLI no-value: empty string → field=false.
        if (v.empty()) { a.*f = false; return; }
        a.*f = str_to_bool(v, a.*f);
    };
}
// SET_LIST_APPEND: each invocation appends to a vector<string>.
// INI side: comma-separated value, split + append.
auto SET_LIST_APPEND(std::vector<std::string> ServerArgs::* f) {
    return [f](ServerArgs & a, const std::string & v) {
        if (v.empty()) return;
        // Split on commas to support INI like `key = a,b,c`. CLI
        // typically sends one value per --override-kv; multi-value
        // INI form is a convenience.
        std::size_t start = 0;
        while (start <= v.size()) {
            std::size_t comma = v.find(',', start);
            std::string piece = v.substr(start, comma - start);
            // trim
            while (!piece.empty() && std::isspace((unsigned char) piece.front())) piece.erase(piece.begin());
            while (!piece.empty() && std::isspace((unsigned char) piece.back()))  piece.pop_back();
            if (!piece.empty()) (a.*f).push_back(std::move(piece));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    };
}

}  // namespace

// ---- The flag table ---------------------------------------------------------
//
// One table, two consumers (parse_args + apply_ini_to_args). Adding a
// new flag is one row. Order matters only cosmetically — the CLI
// parser does linear search per argv token (negligible at ~50 entries
// × small N).
static const std::vector<FlagDef> & kFlags() {
    static const std::vector<FlagDef> table = {
        // ----- core paths / identity (SERVER) -----
        { {"-m","--model"},        "SERVER", "model",          "model",          true,  SET_STR(&ServerArgs::model_path) },
        { {"--config"},            "",       "",               "config",         true,  SET_STR(&ServerArgs::config_path) },
        { {"--host"},              "SERVER", "host",           "host",           true,  SET_STR(&ServerArgs::host) },
        { {"--port"},              "SERVER", "port",           "port",           true,  SET_INT(&ServerArgs::port) },
        { {"-a","--alias"},        "SERVER", "alias",          "alias",          true,  SET_STR(&ServerArgs::alias) },
        { {"--sandbox"},           "SERVER", "sandbox",        "sandbox",        true,  SET_STR(&ServerArgs::sandbox) },
        { {"-s","--system-file"},  "SERVER", "system_file",    "system_file",    true,  SET_STR(&ServerArgs::system_path) },
        { {"--system"},            "SERVER", "system_inline",  "system_inline",  true,  SET_STR(&ServerArgs::system_inline) },
        { {"--external-tools"},    "SERVER", "external_tools", "external_tools", true,  SET_STR(&ServerArgs::external_tools_dir) },
        // `memory` (formerly `rag`): the legacy INI key `rag` is still
        // read (first row, INI-only) so existing easyai.ini files keep
        // working; the `memory` row is second so the new key wins when
        // both appear. Both share canonical `memory` so either CLI flag
        // suppresses both INI rows.
        { {},                      "SERVER", "rag",            "memory",         true,  SET_STR(&ServerArgs::rag_dir) },
        { {"--memory","--RAG"},    "SERVER", "memory",         "memory",         true,  SET_STR(&ServerArgs::rag_dir) },
        { {"--api-key"},           "SERVER", "api_key",        "api_key",        true,  SET_STR(&ServerArgs::api_key) },
        { {"--max-body"},          "SERVER", "max_body",       "max_body",       true,  SET_SIZE(&ServerArgs::max_body) },
        // ----- toggles (SERVER) -----
        { {"--metrics"},           "SERVER", "metrics",        "metrics",        false, SET_BOOL_TRUE(&ServerArgs::metrics) },
        { {"-v","--verbose"},      "SERVER", "verbose",        "verbose",        false, SET_BOOL_TRUE(&ServerArgs::verbose) },
        { {"--show-system-prompt"},"",       "",               "show_system_prompt", false, SET_BOOL_TRUE(&ServerArgs::show_system_prompt) },
        { {"--allow-fs"},          "SERVER", "allow_fs",       "allow_fs",       false, SET_BOOL_TRUE(&ServerArgs::allow_fs) },
        { {"--allow-bash"},        "SERVER", "allow_bash",     "allow_bash",     false, SET_BOOL_TRUE(&ServerArgs::allow_bash) },
        { {"--no-python"},         "",       "",               "no_python",      false, SET_BOOL_FALSE(&ServerArgs::allow_python) },
        // [SERVER] allow_python = on/off — INI-only path. Defaults ON;
        // CLI override path is --no-python (above). No --allow-python
        // because the flag defaults on.
        { {},                      "SERVER", "allow_python",   "allow_python",   true,  SET_BOOL_TRUE(&ServerArgs::allow_python) },
        { {"--use-google"},        "SERVER", "use_google",     "use_google",     false, SET_BOOL_TRUE(&ServerArgs::use_google) },
        // --no-local-tools (formerly --no-tools): disables only the
        // LOCAL built-in toolbelt; remote tools fetched via --mcp are
        // unaffected. The INI key was renamed accordingly so the YAML
        // / INI form reads naturally with the MCP-client addition.
        { {"--no-local-tools"},    "SERVER", "local_tools",    "local_tools",    false, SET_BOOL_FALSE(&ServerArgs::local_tools) },
        { {"--mcp"},               "SERVER", "mcp",            "mcp",            true,  SET_STR(&ServerArgs::mcp_url) },
        { {"--mcp-token"},         "SERVER", "mcp_token",      "mcp_token",      true,  SET_STR(&ServerArgs::mcp_token) },
        { {"--http-retries"},      "SERVER", "http_retries",   "http_retries",   true,  SET_INT(&ServerArgs::http_retries) },
        { {"--http-timeout"},      "SERVER", "http_timeout",   "http_timeout",   true,  SET_INT(&ServerArgs::http_timeout) },
        { {"--metrics-interval"},  "SERVER", "metrics_interval","metrics_interval",true, SET_INT(&ServerArgs::metrics_interval) },
        { {"--no-think"},          "SERVER", "no_think",       "no_think",       false, SET_BOOL_TRUE(&ServerArgs::no_think) },
        { {"--inject-datetime"},   "SERVER", "inject_datetime","inject_datetime",true,  SET_BOOL_TRUE(&ServerArgs::inject_datetime) },
        { {"--knowledge-cutoff"},  "SERVER", "knowledge_cutoff","knowledge_cutoff",true, SET_STR(&ServerArgs::knowledge_cutoff) },
        { {"--reasoning"},         "SERVER", "reasoning",      "reasoning",      true,  SET_BOOL_TRUE(&ServerArgs::reasoning) },
        { {"--no-mcp-auth"},       "",       "",               "no_mcp_auth",    false, SET_BOOL_TRUE(&ServerArgs::no_mcp_auth) },
        // mcp_auth has NO CLI alias — INI-only; the override path is --no-mcp-auth above.
        { {},                      "SERVER", "mcp_auth",       "mcp_auth",       true,
          [](ServerArgs & a, const std::string & v) {
              std::string s = v;
              for (auto & c : s) c = (char) std::tolower((unsigned char) c);
              if (s == "off" || s == "open" || s == "disabled" || s == "disable" ||
                  s == "false" || s == "no" || s == "0") {
                  a.no_mcp_auth = true;
              } else if (s == "on" || s == "required" || s == "enabled" ||
                         s == "enable" || s == "true" || s == "yes" || s == "1") {
                  a.no_mcp_auth = false;
              }
          } },
        // ----- webui (SERVER) -----
        { {"--webui-title"},       "SERVER", "webui_title",    "webui_title",    true,  SET_STR(&ServerArgs::webui_title) },
        { {"--webui-icon"},        "SERVER", "webui_icon",     "webui_icon",     true,  SET_STR(&ServerArgs::webui_icon) },
        { {"--webui-placeholder"}, "SERVER", "webui_placeholder","webui_placeholder",true, SET_STR(&ServerArgs::webui_placeholder) },
        { {"--webui"},             "SERVER", "webui_mode",     "webui_mode",     true,  SET_STR(&ServerArgs::webui_mode) },

        // ----- ENGINE -----
        { {"-c","--ctx"},          "ENGINE", "context",        "context",        true,  SET_INT(&ServerArgs::n_ctx) },
        { {"--ngl"},               "ENGINE", "ngl",            "ngl",            true,  SET_INT(&ServerArgs::ngl) },
        { {"-t","--threads"},      "ENGINE", "threads",        "threads",        true,  SET_INT(&ServerArgs::n_threads) },
        { {"-tb","--threads-batch"},"ENGINE","threads_batch",  "threads_batch",  true,  SET_INT(&ServerArgs::threads_batch) },
        { {"--batch"},             "ENGINE", "batch",          "batch",          true,  SET_INT(&ServerArgs::n_batch) },
        { {"-np","--parallel"},    "ENGINE", "parallel",       "parallel",       true,  SET_INT(&ServerArgs::parallel) },
        { {"--preset"},            "ENGINE", "preset",         "preset",         true,  SET_STR(&ServerArgs::preset) },
        { {"-fa","--flash-attn"},  "ENGINE", "flash_attn",     "flash_attn",     false, SET_BOOL_TRUE(&ServerArgs::flash_attn) },
        { {"--mlock"},             "ENGINE", "mlock",          "mlock",          false, SET_BOOL_TRUE(&ServerArgs::mlock) },
        { {"--no-mmap"},           "ENGINE", "no_mmap",        "no_mmap",        false, SET_BOOL_TRUE(&ServerArgs::no_mmap) },
        { {"-nkvo","--no-kv-offload"},"ENGINE","no_kv_offload","no_kv_offload",  false, SET_BOOL_TRUE(&ServerArgs::no_kv_offload) },
        { {"--kv-unified"},        "ENGINE", "kv_unified",     "kv_unified",     false, SET_BOOL_TRUE(&ServerArgs::kv_unified) },
        { {"-ctk","--cache-type-k"},"ENGINE","cache_type_k",   "cache_type_k",   true,  SET_STR(&ServerArgs::cache_type_k) },
        { {"-ctv","--cache-type-v"},"ENGINE","cache_type_v",   "cache_type_v",   true,  SET_STR(&ServerArgs::cache_type_v) },
        { {"--numa"},              "ENGINE", "numa",           "numa",           true,  SET_STR(&ServerArgs::numa) },
        { {"--override-kv"},       "ENGINE", "override_kv",    "override_kv",    true,  SET_LIST_APPEND(&ServerArgs::kv_overrides) },
        { {"--spec-type"},         "ENGINE", "spec_type",      "spec_type",      true,  SET_STR(&ServerArgs::spec_type) },
        { {"--spec-draft-n-max"},  "ENGINE", "spec_draft_n_max","spec_draft_n_max",true, SET_INT(&ServerArgs::spec_draft_n_max) },
        { {"--draft-model"},       "ENGINE", "spec_draft_model","spec_draft_model",true, SET_STR(&ServerArgs::spec_draft_model) },
        // --chat-template-file / --reasoning-format: mirror llama-server's
        // flags. Both take effect at Engine::load() time. INI keys are the
        // snake_case forms under [ENGINE].
        { {"--chat-template-file"},"ENGINE", "chat_template_file","chat_template_file",true, SET_STR(&ServerArgs::chat_template_file) },
        { {"--reasoning-format"},  "ENGINE", "reasoning_format","reasoning_format",true,  SET_STR(&ServerArgs::reasoning_format) },
        // sampling
        { {"--temperature","--temp"},"ENGINE","temperature",   "temperature",    true,  SET_FLOAT(&ServerArgs::temperature) },
        { {"--top-p"},             "ENGINE", "top_p",          "top_p",          true,  SET_FLOAT(&ServerArgs::top_p) },
        { {"--top-k"},             "ENGINE", "top_k",          "top_k",          true,  SET_INT(&ServerArgs::top_k) },
        { {"--min-p"},             "ENGINE", "min_p",          "min_p",          true,  SET_FLOAT(&ServerArgs::min_p) },
        { {"--repeat-penalty"},    "ENGINE", "repeat_penalty", "repeat_penalty", true,  SET_FLOAT(&ServerArgs::repeat_penalty) },
        { {"--presence-penalty"},  "ENGINE", "presence_penalty","presence_penalty",true, SET_FLOAT(&ServerArgs::presence_penalty) },
        { {"--max-tokens"},        "ENGINE", "max_tokens",     "max_tokens",     true,  SET_INT(&ServerArgs::max_tokens) },
        { {"--max-incomplete-retries"}, "ENGINE", "max_incomplete_retries", "max_incomplete_retries", true, SET_INT(&ServerArgs::max_incomplete_retries) },
        { {"--seed"},              "ENGINE", "seed",           "seed",           true,  SET_UINT32(&ServerArgs::seed) },
    };
    return table;
}

// ---- CLI parser — iterates kFlags() ----------------------------------------
static ServerArgs parse_args(int argc, char ** argv) {
    ServerArgs a;
    const auto & flags = kFlags();
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "-h" || s == "--help") die_usage(argv[0]);

        const FlagDef * matched = nullptr;
        for (const auto & f : flags) {
            for (const auto & alias : f.cli) {
                if (alias == s) { matched = &f; break; }
            }
            if (matched) break;
        }
        if (!matched) {
            std::fprintf(stderr, "unknown arg: %s\n", s.c_str());
            die_usage(argv[0]);
        }
        std::string value;
        if (matched->takes_value) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", s.c_str());
                die_usage(argv[0]);
            }
            value = argv[++i];
        }
        matched->set(a, value);
        a.cli_set.insert(matched->canonical);
    }
    return a;
}

// ---- INI overlay — iterates the SAME kFlags() table -------------------------
//
// For every entry that has an INI section/key declared AND was NOT passed
// on the command line, fetch the INI value and feed it to the same setter
// the CLI uses. Empty INI values are skipped — the field keeps its
// hardcoded default.
static void apply_ini_to_args(const easyai::config::Ini & ini, ServerArgs & a) {
    for (const auto & f : kFlags()) {
        if (f.ini_section.empty() || f.ini_key.empty()) continue;
        if (a.cli_set.count(f.canonical))               continue;
        std::string v = ini.get(f.ini_section, f.ini_key);
        if (v.empty()) continue;
        f.set(a, v);
    }
}

// Graceful shutdown — flag set by SIGINT/SIGTERM, polled by main loop.
static std::atomic<httplib::Server *> g_server{nullptr};
static void on_signal(int) {
    httplib::Server * s = g_server.load();
    if (s) s->stop();
}

// ---------------------------------------------------------------------------
// Periodic metrics sampler — always on.
//
// One background thread that wakes every `interval_s` seconds and prints
// a single line summarising:
//   - CPU memory: process RSS + peak (VmHWM), system used / total / pct
//   - GPU memory: AMD GTT used + peak / total (Linux + AMD only); skipped
//                 on systems without /sys/class/drm/card*/device/mem_info_*
//   - CPU usage: process %, system % (from /proc/stat deltas)
//   - I/O wait: system iowait %
//   - Load average: 1 / 5 / 15 min
//   - TCP states (system-wide): ESTABLISHED / TIME_WAIT / CLOSE_WAIT /
//                FIN_WAIT / LISTEN counts from /proc/net/tcp{,6}; the
//                TIME_WAIT count is reported as a percentage of the
//                kernel's ephemeral port range so socket exhaustion
//                surfaces before connections start failing.
//   - Process: in-flight requests, cumulative requests / errors / bytes.
//
// All sources are best-effort POSIX; missing files yield "n/a" rather
// than aborting the tick. The thread joins on g_metrics_stop.
// ---------------------------------------------------------------------------
namespace metrics {

struct CpuTicks {
    uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
    uint64_t irq = 0,  softirq = 0, steal = 0;
    uint64_t total() const {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }
    uint64_t busy()  const { return total() - idle - iowait; }
};

static bool read_proc_stat(CpuTicks & out) {
    std::ifstream f("/proc/stat");
    if (!f) return false;
    std::string label;
    f >> label;
    if (label != "cpu") return false;
    f >> out.user >> out.nice >> out.system >> out.idle >> out.iowait
      >> out.irq >> out.softirq >> out.steal;
    return f.good() || f.eof();
}

static void read_meminfo(uint64_t & total_kb, uint64_t & avail_kb) {
    total_kb = 0; avail_kb = 0;
    std::ifstream f("/proc/meminfo");
    if (!f) return;
    std::string key;
    uint64_t val;
    std::string unit;
    while (f >> key >> val >> unit) {
        if (key == "MemTotal:")     total_kb = val;
        else if (key == "MemAvailable:") { avail_kb = val; break; }
    }
}

static void read_proc_self_status(uint64_t & rss_kb, uint64_t & hwm_kb) {
    rss_kb = 0; hwm_kb = 0;
    std::ifstream f("/proc/self/status");
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 6, "VmRSS:") == 0) {
            sscanf(line.c_str() + 6, " %llu",
                   (unsigned long long *) &rss_kb);
        } else if (line.compare(0, 6, "VmHWM:") == 0) {
            sscanf(line.c_str() + 6, " %llu",
                   (unsigned long long *) &hwm_kb);
        }
    }
}

// AMD iGPU: /sys/class/drm/card*/device/mem_info_gtt_used and _total.
// Picks the first card found. Returns false if no AMD card is exposed.
static bool read_amd_gtt(uint64_t & used_b, uint64_t & total_b) {
    used_b = 0; total_b = 0;
    for (int i = 0; i < 4; ++i) {
        char p1[128], p2[128];
        std::snprintf(p1, sizeof(p1),
            "/sys/class/drm/card%d/device/mem_info_gtt_used", i);
        std::snprintf(p2, sizeof(p2),
            "/sys/class/drm/card%d/device/mem_info_gtt_total", i);
        std::ifstream fu(p1), ft(p2);
        if (!fu || !ft) continue;
        fu >> used_b;
        ft >> total_b;
        if (total_b > 0) return true;
    }
    return false;
}

static double load_avg_1() {
    double l[3] = {0,0,0};
    if (::getloadavg(l, 3) > 0) return l[0];
    return -1.0;
}

static void load_avg_3(double out[3]) {
    out[0] = out[1] = out[2] = -1.0;
    ::getloadavg(out, 3);
}

static uint64_t open_fd_count() {
    // /proc/self/fd entries; falls back to 0 if unreadable.
    DIR * d = ::opendir("/proc/self/fd");
    if (!d) return 0;
    uint64_t n = 0;
    while (::readdir(d) != nullptr) ++n;
    ::closedir(d);
    return n > 2 ? n - 2 : 0;  // skip "." and ".."
}

static uint64_t fd_soft_limit() {
    struct rlimit rl{};
    if (::getrlimit(RLIMIT_NOFILE, &rl) == 0) return (uint64_t) rl.rlim_cur;
    return 0;
}

// TCP state breakdown from /proc/net/tcp and /proc/net/tcp6.
// State codes (kernel hex):  01=ESTABLISHED  06=TIME_WAIT  08=CLOSE_WAIT
// 0A=LISTEN  others=transition.  Counted system-wide because TIME_WAIT
// pressure on the host is what exhausts the ephemeral-port range, not
// just our process's slice.
struct TcpStates {
    uint64_t established = 0;
    uint64_t time_wait   = 0;
    uint64_t close_wait  = 0;
    uint64_t fin_wait    = 0;   // FIN_WAIT1 + FIN_WAIT2
    uint64_t listen      = 0;
    uint64_t total       = 0;
    bool     have        = false;
};

static void count_tcp_states_one(const char * path, TcpStates & out) {
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        // Format: "sl local_address rem_address st ..."
        // Find the 4th whitespace-separated column, which is the state.
        const char * p = line.c_str();
        // Skip leading spaces.
        while (*p == ' ' || *p == '\t') ++p;
        // Walk 3 fields.
        for (int i = 0; i < 3 && *p; ++i) {
            while (*p && *p != ' ' && *p != '\t') ++p;
            while (*p == ' ' || *p == '\t') ++p;
        }
        if (!*p) continue;
        unsigned st = 0;
        if (std::sscanf(p, "%2X", &st) != 1) continue;
        ++out.total;
        out.have = true;
        switch (st) {
            case 0x01: ++out.established; break;
            case 0x04:  // FIN_WAIT1
            case 0x05:  // FIN_WAIT2
                ++out.fin_wait; break;
            case 0x06: ++out.time_wait;  break;
            case 0x08: ++out.close_wait; break;
            case 0x0A: ++out.listen;     break;
            default: break;
        }
    }
}

static TcpStates count_tcp_states() {
    TcpStates s;
    count_tcp_states_one("/proc/net/tcp",  s);
    count_tcp_states_one("/proc/net/tcp6", s);
    return s;
}

// Linux ephemeral port range (used by outbound connect()s — the web
// tool, MCP client, etc.). TIME_WAIT pressure is meaningful as a percentage
// of THIS range, not of the full 65535 port space, because that's what
// the kernel actually pulls from for ephemeral allocations.
static uint64_t ephemeral_port_range() {
    std::ifstream f("/proc/sys/net/ipv4/ip_local_port_range");
    if (!f) return 28232;  // typical default 32768..60999 = 28232
    uint64_t lo = 0, hi = 0;
    f >> lo >> hi;
    return hi > lo ? hi - lo + 1 : 28232;
}

static std::string fmt_bytes(uint64_t b) {
    char buf[32];
    if (b >= (1ull << 30)) {
        std::snprintf(buf, sizeof(buf), "%.2fGiB", (double) b / (1ull << 30));
    } else if (b >= (1ull << 20)) {
        std::snprintf(buf, sizeof(buf), "%.1fMiB", (double) b / (1ull << 20));
    } else if (b >= (1ull << 10)) {
        std::snprintf(buf, sizeof(buf), "%.1fKiB", (double) b / (1ull << 10));
    } else {
        std::snprintf(buf, sizeof(buf), "%lluB", (unsigned long long) b);
    }
    return buf;
}

}  // namespace metrics

// Background ticker — set true on shutdown so the thread joins promptly.
static std::atomic<bool>        g_metrics_stop{false};
static std::condition_variable  g_metrics_cv;
static std::mutex               g_metrics_mu;

// Build the built-in system prompt with tool-notes bullets gated on which
// tools will actually be registered. Naming an unregistered tool here makes
// models try to call it ("bash"/"fs" hallucinations), so each bullet is
// conditional on the same flag that controls registration.
static std::string build_builtin_system_prompt(const ServerArgs & args) {
    const bool tools_on    = args.local_tools;
    const bool fs_on       = tools_on && args.allow_fs;
    const bool bash_on     = tools_on && args.allow_bash;
    // python3 defaults ON; auto-on under same gate as Toolbelt
    // registration: sandbox-or-bash, plus the operator-respected
    // allow_python opt-out. --sandbox alone enables python3 even
    // without --allow-fs (the python3 tool brings its own sandbox-
    // restricted disk surface and explicitly forbids file IO).
    const bool python_on   = tools_on && args.allow_python
                          && (!args.sandbox.empty() || args.allow_bash);
    const bool sandbox_path_on = tools_on
                          && (args.allow_fs || args.allow_bash || python_on);
    const bool web_on      = tools_on;        // unified web tool is default-on
    const bool datetime_on = tools_on;        // datetime is default-on
    const bool rag_on      = !args.rag_dir.empty();

    std::string s;
    s.reserve(4096);

    s +=
        "You are Deep — a clear, honest assistant. Lead with the answer;\n"
        "add detail only when the user needs it.\n"
        "\n"
        "## Think SHARP, not LONG\n"
        "Reasoning decides the next move — not for rehearsing the answer\n"
        "or exploring tangents.\n"
        "  - 3-5 short sentences before the first tool call or answer; up\n"
        "    to ~10 short bullets for genuinely complex tasks. Never\n"
        "    paragraphs.\n"
        "  - Telegraph style: one claim or decision per line; drop \"I\n"
        "    think\", \"Let me consider\", \"It seems\".\n"
        "  - Don't enumerate options you immediately reject — pick the\n"
        "    move and go; tool results correct wrong moves.\n"
        "  - Don't pre-compute the answer in reasoning then restate it\n"
        "    visibly. Reasoning drives the agent loop; the visible reply\n"
        "    is for the user.\n"
        "  - >5 sentences without a decision → STOP and act.\n"
        "\n"
        "Answer directly for greetings, chitchat, math, and anything you\n"
        "already know — no tool needed. When a request truly needs work,\n"
        "run a tight loop:\n"
        "  1. Plan ONE small concrete next step (not a roadmap).\n"
        "  2. Act — call the tool in the SAME turn. Announcing a call\n"
        "     without making it (\"I'll search…\", \"Let me fetch…\") is\n"
        "     forbidden.\n"
        "  3. Read the result, then finish or take ONE more step.\n"
        "Stop as soon as you have something useful; a short answer the\n"
        "user can refine beats a long pre-committed plan.\n"
        "\n"
        "## Tools — closed set\n"
        "Your tools are EXACTLY those in your tools schema this session.\n"
        "Do NOT invent tools or use paraphrases (`read_file` is not `fs`;\n"
        "use `fs(action=\"read\")`; `shell` is not `bash`). Unsure a name\n"
        "is registered? Call `tool_lookup` — no argument returns the full\n"
        "catalogue, `name=\"<substring>\"` confirms or denies one name; a\n"
        "no-match result is authoritative, do not retry variations.\n"
        "\n"
        "If a request needs a capability with no matching tool, do the\n"
        "work in your visible reply. No write tool but asked to write a\n"
        "file / save a document? Put the content DIRECTLY in the chat\n"
        "reply — never paste it into a non-existent tool call; every\n"
        "hallucinated call returns `unknown tool` and wastes the turn.\n"
        "\n";

    const bool any_tool_note = datetime_on || web_on || fs_on || bash_on
                            || python_on || sandbox_path_on || rag_on;
    if (any_tool_note) {
        // One-line trigger index. Each tool's full rules live in its
        // own description — these lines are just "when to reach for it".
        s += "Active tools (one-line triggers — see each tool's "
             "description for details):\n";
        if (datetime_on) {
            if (args.inject_datetime) {
                s += "  - datetime: use the AUTHORITATIVE DATE/TIME "
                     "below for 'today' / 'now'. Call the tool only "
                     "for date math or other timezones.\n";
            } else {
                s += "  - datetime: call for 'now' / 'today' / "
                     "'latest'.\n";
            }
        }
        if (web_on) {
            s += "  - web: search → fetch top 1-3 URLs → answer from "
                 "fetched text. REPLY MUST END WITH `Sources:` block "
                 "(see Cite sources rule below).\n";
        }
        if (rag_on) {
            s += "  - memory: search first for STABLE facts (vocab "
                 "appended below); save only DURABLE info, one "
                 "comprehensive entry per topic. REPLY MUST END WITH "
                 "`Sources:` block citing memory titles when you use "
                 "retrieved content (see Cite sources rule below).\n";
        }
        if (fs_on) {
            s += "  - fs: sandbox + check_path before any file work. "
                 "RELATIVE paths only.\n";
        }
        if (bash_on) {
            s += "  - bash: only for shell features fs/python can't "
                 "do (pipes, build runners, git, sed/awk).\n";
        }
        if (python_on) {
            s += "  - python3: stdlib-only; print() what you want "
                 "returned.\n";
        }
        s += "\n";
    }

    s +=
        "## Information pipeline (AUTHORITATIVE)\n"
        "When the request needs facts you don't already know,\n"
        "follow this order — strictly:\n"
        "\n";
    if (rag_on) {
        s +=
            "  1. MEMORY FIRST. memory(action=\"search\") with\n"
            "     keywords from the vocabulary appended below. If\n"
            "     memory returns enough to answer, SKIP the web\n"
            "     and go straight to step 3.\n"
            "  2. WEB only if memory had nothing or was insufficient.\n"
            "     ONE web search, then web_fetch the top 1-3 URLs.\n"
            "  3. ANSWER. As soon as steps 1-2 give you enough,\n"
            "     answer the user. Don't re-search memory, don't\n"
            "     re-search the web, don't save more memories first.\n";
    } else {
        s +=
            "  1. WEB if you don't already know. ONE web search,\n"
            "     then web_fetch the top 1-3 URLs.\n"
            "  2. ANSWER. As soon as the fetched text gives you\n"
            "     enough, answer. Don't re-search the same query.\n";
    }
    s +=
        "\n"
        "STOP SIGNAL. After each tool result, ask: do I have enough\n"
        "now? Yes → answer immediately. No → ONE more focused tool\n"
        "call, then re-check. Three or more tool calls in a row\n"
        "without re-checking is a bug — you're exploring instead of\n"
        "answering.\n"
        "\n"
        "BUGS TO AVOID:\n";
    if (rag_on) {
        s +=
            "  - Skipping memory and going straight to web when\n"
            "    memory is enabled.\n"
            "  - After a memory load returns a stable fact\n"
            "    (definition, syntax, architecture), re-verifying\n"
            "    with the web — only do this when the user asked\n"
            "    for \"latest\" / \"current\" / dated info.\n"
            "  - After saving a memory, re-searching the web on the\n"
            "    same topic in the same turn — the save means you\n"
            "    already learned what you needed.\n";
    }
    s +=
        "  - Looping verify → save → re-verify.\n"
        "  - Running the same web query twice in a row.\n"
        "\n"
        "Saving new memories (when the info is durable — see the\n"
        "memory tool's GUIDELINES) happens AFTER your reply is\n"
        "written, as a final tool call. It's not another\n"
        "verification step.\n"
        "\n"
        "## Stop when you have enough — the user can refine (AUTHORITATIVE)\n"
        "Go only as far as the searches you need to give a useful\n"
        "answer to THIS question. Not the perfect answer, not the\n"
        "exhaustive one — a useful one.\n"
        "\n"
        "The user is on the other side of a chat box. They CAN send\n"
        "you another message — refining the query, narrowing the\n"
        "scope, asking for more depth on one point. They CANNOT\n"
        "interrupt your tool loop or skim while you fetch URL #8.\n"
        "Their cost of asking a follow-up is one sentence; your cost\n"
        "of an extra round of searches is their wait time.\n"
        "\n"
        "Practical shape: 1-3 targeted searches, then answer with\n"
        "what you have. If something's missing, name it in the reply\n"
        "(\"I couldn't find X — want me to check Y instead?\"). Let\n"
        "the user steer the next step.\n"
        "\n"
        "## Stay strictly in scope (AUTHORITATIVE)\n"
        "Do EXACTLY what the user asked — no more, no less. No extra\n"
        "features, no defensive scaffolding for cases they didn't\n"
        "mention, no \"while I'm at it\" cleanups, no proactive refactors.\n"
        "The request is the ceiling, not a starting point. If genuinely\n"
        "unsure what's in scope, ASK before acting — don't expand the\n"
        "task to be safe.\n"
        "\n";
    // Citation rule lives in libeasyai now (preamble.hpp) so server,
    // local, and cli render the exact same text. The per-request
    // preamble also re-emits this block at the END of the prompt for
    // Qwen3.x-style models that drop sources after a long <think>.
    s += easyai::preamble::cite_sources_block();
    s +=
        "\n"
        "Be terse. Be honest about uncertainty: \"I'm not sure — let me\n"
        "check\"";
    s += any_tool_note ? " → call a tool." : ".";
    return s;
}

int main(int argc, char ** argv) {
    ServerArgs args = parse_args(argc, argv);

    // -------- INI overlay (CLI > INI > hardcoded) ------------------------
    // Load early so EVERY downstream `args.*` read already has the merged
    // value. The INI section/key declared in `kFlags()` is the contract:
    // any flag present there picks up its INI default unless the operator
    // also passed it on the command line. The merged `Ini` is preserved
    // for use below (MCP auth user table) and for any future code that
    // needs to introspect a non-flag-mapped section.
    easyai::config::Ini ini_config;
    {
        std::string ini_err;
        ini_config = easyai::config::load_ini_file(args.config_path, ini_err);
        if (!ini_err.empty()) {
            std::fprintf(stderr,
                "easyai-server: %s warnings:\n%s\n",
                args.config_path.c_str(), ini_err.c_str());
        }
        apply_ini_to_args(ini_config, args);
    }

    // -------- resolve system prompt --------------------------------------
    // Precedence: --system inline > -s file > built-in default. The default
    // exists because a *small* model with NO system prompt and a tool list
    // is very likely to over-eagerly call tools on simple greetings ("hi"
    // → web action=search). Operators can fully replace it via -s.
    //
    // The built-in prompt only mentions tools that are actually registered
    // for THIS server invocation. Listing names of unregistered tools (e.g.
    // bash with allow_bash=off) makes models hallucinate calls to them, so
    // each "Tool notes:" bullet is gated on the corresponding flag.
    std::string default_system = args.system_inline;
    if (default_system.empty() && !args.system_path.empty()) {
        default_system = read_text_file(args.system_path);
        if (default_system.empty()) {
            std::fprintf(stderr, "[easyai-server] WARN: failed to read system file '%s'\n",
                         args.system_path.c_str());
        }
    }
    if (default_system.empty()) default_system = build_builtin_system_prompt(args);

    // --show-system-prompt: dump the resolved persona and exit before
    // any model loads or any port binds. Resolves --system / --system-file
    // / built-in default in the same precedence the running server would.
    // Useful for operators inspecting their persona without bouncing the
    // service.
    if (args.show_system_prompt) {
        std::fputs(default_system.c_str(), stdout);
        // Preview the memory-vocab portion of the per-request
        // preamble so the operator can verify the keyword index
        // without bouncing the service. The date/time half is
        // dynamic (changes every request); we render only the
        // memory vocabulary by passing inject_datetime=false.
        if (!args.rag_dir.empty()) {
            std::string vocab = easyai::preamble::build({
                /* inject_datetime  = */ false,
                /* knowledge_cutoff = */ std::string(),
                /* memory_root      = */ args.rag_dir,
            });
            if (!vocab.empty()) std::fputs(vocab.c_str(), stdout);
        }
        std::fputc('\n', stdout);
        return 0;
    }

    // Anchor process cwd to --sandbox before loading the external-tools
    // manifest: $SANDBOX placeholders in the manifest are resolved
    // against getcwd at load time, so chdir-ing here makes those
    // placeholders mean "the dir the operator handed me", not
    // "wherever systemd happened to start me from". Also lets
    // fs(action="cwd") surface the sandbox path to the model.
    if (!args.sandbox.empty()) {
        if (::chdir(args.sandbox.c_str()) != 0) {
            std::fprintf(stderr,
                "easyai-server: chdir(%s): %s\n",
                args.sandbox.c_str(), std::strerror(errno));
            return 2;
        }
    }

    // -------- build the context (heap-allocated so HTTP lambdas can capture
    // a stable reference; lifetime tied to main()'s scope via unique_ptr).
    auto ctx = std::make_unique<ServerCtx>();
    ctx->default_system   = default_system;
    ctx->inject_datetime  = args.inject_datetime;
    ctx->knowledge_cutoff = args.knowledge_cutoff;
    ctx->verbose          = args.verbose;
    {
        // Compute model_id = basename(path) without extension.
        std::string p = args.model_path;
        auto slash = p.find_last_of("/\\");
        if (slash != std::string::npos) p = p.substr(slash + 1);
        auto dot = p.find_last_of('.');
        if (dot != std::string::npos) p = p.substr(0, dot);
        ctx->model_id = p;
    }

    // Start with ambient preset, then overlay any explicit numeric overrides.
    {
        // Fallback baseline matches args.preset's default ("precise") so an
        // unknown preset name still lands somewhere sensible without
        // diverging from the documented default.
        easyai::Preset base = *easyai::find_preset("precise");
        if (const easyai::Preset * pp = easyai::find_preset(args.preset)) base = *pp;
        if (args.temperature >= 0) base.temperature = args.temperature;
        if (args.top_p       >= 0) base.top_p       = args.top_p;
        if (args.top_k       >= 0) base.top_k       = args.top_k;
        if (args.min_p       >= 0) base.min_p       = args.min_p;
        ctx->apply_preset(base);
    }

    // Default toolbelt — opt-out via --no-local-tools.
    //
    // The unified `fs` tool and `bash` are SHIPPED OFF by default. The
    // server requires explicit opt-in: --allow-fs registers `fs`,
    // --allow-bash adds `bash` on top. --sandbox alone DOES NOT imply
    // `fs` — the sandbox is also the cwd / external-tools root /
    // fs(action="sandbox") target, so operators legitimately set it
    // while keeping fs / bash off.
    if (args.local_tools) {
        // Determine the working root: --sandbox if given, else cwd
        // when ANY filesystem-flavoured tool is on. We only pass a
        // root through to Toolbelt when SOMETHING is about to use it,
        // so the engine doesn't spuriously hold onto a sandbox dir the
        // operator never intended to use. python3 alone does NOT
        // trigger the cwd fallback — its registration gates on
        // sandbox-or-bash anyway, and falling back here would expose
        // ambient cwd to the python3 interpreter when the operator
        // never set --sandbox.
        std::string sb = args.sandbox;
        const bool any_fs_like = args.allow_fs || args.allow_bash;
        if (sb.empty() && any_fs_like) sb = ".";
        auto tb = easyai::cli::Toolbelt()
                      .sandbox     (sb)
                      .allow_fs    (args.allow_fs)
                      .allow_bash  (args.allow_bash)
                      .allow_python(args.allow_python)
                      .use_google  (args.use_google);
        for (auto & t : tb.tools()) ctx->default_tools.push_back(std::move(t));
    }

    // RAG — the agent's persistent registry / long-term memory.
    // Seven tools (rag_save / rag_append / rag_search / rag_load /
    // rag_list / rag_delete / rag_keywords) registered when
    // --RAG <dir> is given. The dir does NOT have to exist yet;
    // rag_save creates it on first call. The systemd-installed
    // server passes --RAG by default (see
    // scripts/install_easyai_server.sh). See RAG.md.
    if (!args.rag_dir.empty()) {
        ctx->default_tools.push_back(
            easyai::tools::make_rag_tool(args.rag_dir));
        // Remember the root so build_authoritative_preamble can render
        // the current keyword vocabulary into every request — the model
        // sees what keywords it has tagged without having to call
        // memory(action="keywords") itself.
        ctx->memory_root = args.rag_dir;
        std::fprintf(stderr,
            "easyai-server: memory enabled (single memory tool), "
            "root = %s\n",
            args.rag_dir.c_str());
    }

    // MCP client — connect to a remote MCP server and merge its tool
    // catalogue into ours. Runs AFTER the local toolbelt + RAG so we
    // can reject any remote tool whose name collides with one we've
    // already registered. The collision rule is "first wins": local
    // tools are authoritative, remote dups are skipped with a
    // warning. Operators who want the remote tool to take precedence
    // can pass --no-local-tools.
    //
    // Failure modes (network down, auth rejected, server returns a
    // bad response) log a warning and continue with whatever local
    // / RAG tools we have. We deliberately don't refuse to start —
    // a transient outage at the upstream shouldn't take down a chat
    // server, especially when the local tools alone are useful.
    if (!args.mcp_url.empty()) {
        // Pre-validate the scheme.  libcurl's CURLOPT_PROTOCOLS_STR also
        // pins http(s) inside fetch_remote_tools(), but rejecting the
        // bad URL here lets us emit a clear operator-level error
        // ("--mcp requires http:// or https://") instead of a curl
        // diagnostic, AND closes a hypothetical defence-in-depth gap
        // if the curl filter ever regresses on an exotic build.
        const auto is_http_scheme = [](const std::string & u) {
            return u.compare(0, 7,  "http://")  == 0
                || u.compare(0, 8,  "https://") == 0;
        };
        if (!is_http_scheme(args.mcp_url)) {
            std::fprintf(stderr,
                "easyai-server: --mcp <url> must start with http:// or "
                "https:// (got: %s)\n", args.mcp_url.c_str());
            return 1;
        }
        easyai::mcp::ClientOptions opts;
        opts.url             = args.mcp_url;
        opts.bearer_token    = args.mcp_token;
        // Honour the operator's --http-timeout and --http-retries for
        // the MCP client too, so a flaky upstream MCP server doesn't
        // wedge the chat path.  The retry budget logs each attempt to
        // stderr unconditionally — operators see drops in journalctl
        // even without --verbose.
        opts.timeout_seconds = args.http_timeout > 0 ? args.http_timeout : 20;
        opts.retries         = args.http_retries < 0 ? 0 : args.http_retries;

        std::string err;
        auto remote = easyai::mcp::fetch_remote_tools(opts, err);
        if (!err.empty()) {
            std::fprintf(stderr,
                "easyai-server: MCP client failed against %s: %s\n",
                args.mcp_url.c_str(), err.c_str());
        } else {
            std::size_t added = 0, skipped = 0;
            for (auto & t : remote) {
                bool collides = false;
                for (const auto & local : ctx->default_tools) {
                    if (local.name == t.name) { collides = true; break; }
                }
                if (collides) {
                    std::fprintf(stderr,
                        "easyai-server: MCP client: skipping remote "
                        "tool \"%s\" (collides with local tool of "
                        "the same name)\n", t.name.c_str());
                    ++skipped;
                    continue;
                }
                ctx->default_tools.push_back(std::move(t));
                ++added;
            }
            std::fprintf(stderr,
                "easyai-server: MCP client connected to %s "
                "(%zu tool%s registered%s%s)\n",
                args.mcp_url.c_str(),
                added, added == 1 ? "" : "s",
                skipped > 0 ? ", " : "",
                skipped > 0
                    ? (std::to_string(skipped) + " skipped").c_str()
                    : "");
        }
    }

    // External tools directory. Loaded AFTER the built-in toolbelt so
    // the loader can reject any manifest entry that collides with a
    // name we already registered. Per-file fault isolation: a bad
    // file is logged and skipped, the server still starts.
    //
    // The server is a daemon — we always log warnings AND errors to
    // stderr (which lands in journalctl). Operators read those when
    // the model misbehaves or when a sysadmin asks "what's that
    // tool?". No quiet mode here.
    if (!args.external_tools_dir.empty()) {
        std::vector<std::string> reserved;
        reserved.reserve(ctx->default_tools.size());
        for (const auto & t : ctx->default_tools) reserved.push_back(t.name);
        auto loaded = easyai::load_external_tools_from_dir(
            args.external_tools_dir, reserved);

        for (const auto & e_msg : loaded.errors) {
            std::fprintf(stderr,
                "easyai-server: [external-tools] error: %s\n",
                e_msg.c_str());
        }
        for (const auto & w : loaded.warnings) {
            std::fprintf(stderr,
                "easyai-server: [external-tools] warn: %s\n",
                w.c_str());
        }
        for (auto & t : loaded.tools) ctx->default_tools.push_back(std::move(t));
        std::fprintf(stderr,
            "easyai-server: loaded %zu external tool(s) from %zu file(s) in %s\n",
            loaded.tools.size(), loaded.loaded_files.size(),
            args.external_tools_dir.c_str());
    }

    // tool_lookup MUST be added last so its snapshot covers every other
    // tool registered above (built-ins, RAG, MCP-fetched, external).
    // The getter captures a pointer to ctx->default_tools, which is the
    // authoritative server-side registry — copied into per-request
    // engine state in handle_chat. Tools dynamically added by a client
    // (`tools` field on the request body) are NOT seen by tool_lookup;
    // that's deliberate — the model gets a stable view of what THIS
    // server's deployment actually offers, not a per-request whim.
    {
        auto * tools_ptr = &ctx->default_tools;
        ctx->default_tools.push_back(easyai::tools::tool_lookup(
            [tools_ptr]() {
                std::vector<std::pair<std::string, std::string>> v;
                v.reserve(tools_ptr->size());
                for (const auto & t : *tools_ptr) {
                    v.emplace_back(t.name, t.description);
                }
                return v;
            }));
    }

    // The webui drives long agentic flows (search → fetch → search → fetch
    // → write_file → bash → …) and should never bump the per-turn safety
    // cap.  Lift the engine's hop limit unconditionally; we still get
    // bounded behaviour from the model itself ending its turn and from
    // the per-hop retry budgets inside chat_continue.
    ctx->engine.max_tool_hops(99999);
    // retry-on-incomplete defaults ON in libeasyai but be explicit here:
    // the webui relies on chat_continue's announce-pattern retry to
    // recover from "Let me search…" / "I'll look that up…" turns the
    // model never finishes.  Setting it here makes the contract visible
    // in the server's startup config (see banner below).
    ctx->engine.retry_on_incomplete(true);
    // Operator-tunable retry budget — see ServerArgs::max_incomplete_retries.
    // Default 10 is a sane floor for weak / 1-bit quants; bump higher
    // (--max-incomplete-retries 20) if your model frequently announces
    // without acting, drop to 0 to disable the retry loop entirely.
    ctx->engine.max_incomplete_retries(args.max_incomplete_retries);

    // -------- production knobs / auth --------------------------------------
    ctx->api_key  = args.api_key;
    ctx->no_think = args.no_think;
    if (!args.alias.empty()) ctx->model_id = args.alias;

    // -------- MCP auth user table from already-loaded INI -----------------
    // The INI itself was loaded at the top of main() (so apply_ini_to_args
    // can overlay defaults BEFORE every `args.*` read). Here we only need
    // to pull the [MCP_USER] section into a fast token→username map and
    // log the resulting auth posture for the operator.
    //
    // Three-way precedence on the gate:
    //   * `--no-mcp-auth` on the CLI (or `[SERVER] mcp_auth = off`) →
    //     force open; clear the table even if [MCP_USER] had entries.
    //     The operator explicitly asked.
    //   * [MCP_USER] populated → Bearer required.
    //   * [MCP_USER] empty/missing → open by data (zero-friction
    //     local-dev default).
    {
        ctx->ini_config = ini_config;
        ctx->mcp_keys = easyai::mcp::load_mcp_users(
            ctx->ini_config.section_or_empty("MCP_USER"));
        if (args.no_mcp_auth && !ctx->mcp_keys.empty()) {
            std::fprintf(stderr,
                "easyai-server: MCP auth OVERRIDDEN OPEN — `--no-mcp-auth` "
                "(or [SERVER] mcp_auth=off) discards %zu [MCP_USER] entry(ies)\n",
                ctx->mcp_keys.size());
            ctx->mcp_keys.clear();
        }
        if (!ctx->mcp_keys.empty()) {
            std::fprintf(stderr,
                "easyai-server: MCP auth ENABLED — %zu user(s) loaded from %s\n",
                ctx->mcp_keys.size(), args.config_path.c_str());
        } else if (!ctx->ini_config.sections.empty()) {
            std::fprintf(stderr,
                "easyai-server: MCP auth OPEN — [MCP_USER] section in %s "
                "is empty (or absent)\n", args.config_path.c_str());
        } else {
            std::fprintf(stderr,
                "easyai-server: MCP auth OPEN — no INI config at %s\n",
                args.config_path.c_str());
        }
    }

    // ----- webui rebrand: build the served HTML once and load any custom
    //       favicon into memory.
    {
        std::string title = args.webui_title.empty() ? std::string("easyai")
                                                     : args.webui_title;

#if defined(EASYAI_BUILD_WEBUI)
        if (args.webui_mode != "minimal") {
            // Start from the llama-server-derived index.html and inject:
            //   - a <script> that pins document.title to our --webui-title
            //     even if the Svelte app tries to set it later (MutationObserver)
            //   - a <link rel="icon"> that points to our /favicon route when a
            //     --webui-icon was given
            //   - a <style> that hides MCP-related UI sections
            std::string html(reinterpret_cast<const char *>(index_html),
                              index_html_len);

            // Strip the bundle's hard-coded inline favicon: the embedded
            // index.html ships a giant <link rel="icon" href="data:...">
            // base64 data URL with the orange llama-cpp logo.  We want
            // /favicon (which serves either --webui-icon or our embedded
            // brain SVG) to win, so erase the bundle's link tag entirely.
            // We then unconditionally inject our own link below — this
            // also covers the case where the operator did NOT pass
            // --webui-icon (default brain SVG).
            {
                const std::string needle = "<link rel=\"icon\"";
                size_t p = html.find(needle);
                if (p != std::string::npos) {
                    size_t end = html.find('>', p);
                    if (end != std::string::npos) {
                        html.erase(p, end + 1 - p);
                    }
                }
            }

            std::ostringstream inj;

            // ----- title pin (HARD) -----------------------------------------
            // The bundle has `document.title="llama.cpp - AI Chat Interface"`
            // hard-coded in several places.  A MutationObserver isn't enough
            // because the property is also re-assigned imperatively.  We
            // intercept *every* write to document.title via Object.defineProperty
            // and force our own value back.  Also pre-populates the localStorage
            // flag the bundle uses for "is MCP enabled".
            inj << "<script>(()=>{"
                // Diagnostic marker — if you see all five [easyai-inject]
                // logs in DevTools Console, every <script> block parsed.
                // Missing logs indicate a SyntaxError that killed that block.
                << "console.log('[easyai-inject] block1 title-pin');"
                << "const T=" << json(title).dump() << ";"
                << "try{Object.defineProperty(document,'title',{"
                <<   "configurable:true,"
                <<   "get(){return T;},"
                <<   "set(_){const e=document.querySelector('title');"
                <<        "if(e)e.textContent=T;}});}catch(e){}"
                << "try{localStorage.setItem('LlamaCppWebui.mcpDefaultEnabled','false');}"
                <<   "catch(e){}"
                // Force the bundle's keepStatsVisible flag to true so its
                // native per-message stats line stops flickering off after
                // generation completes.  We still inject our own chip
                // because we want the stats to live alongside copy/edit/...
                << "try{"
                <<   "const c=JSON.parse(localStorage.getItem('LlamaCppWebui.config')||'{}');"
                <<   "c.keepStatsVisible=true;c.showMessageStats=true;"
                <<   "localStorage.setItem('LlamaCppWebui.config',JSON.stringify(c));"
                << "}catch(e){}"
                << "const apply=()=>{const e=document.querySelector('title');"
                <<   "if(e&&e.textContent!==T)e.textContent=T;};"
                << "apply();"
                << "document.addEventListener('DOMContentLoaded',()=>{apply();"
                <<   "new MutationObserver(apply).observe(document.head,"
                <<     "{subtree:true,characterData:true,childList:true});"
                << "});"
                << "})();</script>";

            // ----- runtime DOM scrubber for MCP and other unsupported UI ----
            // The Svelte build produces hashed class names, so [class*=mcp]
            // selectors don't match anything.  Instead, identify offending
            // elements by their visible text (the UI strings are stable) and
            // hide their containing card / list-item / dialog / menu-item.
            inj <<
              "<script>(()=>{"
                "console.log('[easyai-inject] block2 mcp-scrubber');"
                "const NEEDLES=["
                  // We do NOT use end-anchored MCP regexes
                  // (e.g. `MCP Servers?$/i`) — those match the bundle's
                  // "+" button whose accessible-name / sr-only span ends
                  // with "...or MCP Servers", and hiding its closest
                  // ancestor takes the whole popup with it. Use
                  // start-anchored regexes against the actual MCP menu
                  // ITEMS instead.
                  "/^MCP\\b/i,"
                  "/^MCP Server/i,"
                  "/^MCP Prompt/i,"
                  "/^MCP Resource/i,"
                  "/^Configure MCP/i,"
                  "/No MCP /i,"
                  "/All MCP server connections/i,"
                  "/^Sign in/i,/^Log in$/i,/^Login$/i,"
                  "/^Authorize/i,"
                  "/^Load model/i,/^Unload model/i,/^Manage models/i,"
                  "/^Use Pyodide/i,/^Python interpreter/i"
                "];"
                "const isMatch=(el)=>{"
                  "const t=(el.innerText||el.textContent||'').trim();"
                  "if(!t||t.length>200)return false;"
                  "for(const re of NEEDLES)if(re.test(t))return true;"
                  "return false;"
                "};"
                "const ANCESTORS=["
                  "'[role=\"dialog\"]',"
                  "'[role=\"menuitem\"]',"
                  "'[role=\"tab\"]',"
                  "'[role=\"tabpanel\"]',"
                  "'fieldset',"
                  "'.menu li','.menu-item',"
                  "'li','.collapse','.card','.tab','.tab-content'"
                "];"
                "const hide=(el)=>{"
                  "for(const sel of ANCESTORS){"
                    "const a=el.closest(sel);"
                    "if(a){a.style.setProperty('display','none','important');return;}"
                  "}"
                  "el.style.setProperty('display','none','important');"
                "};"
                "const scrub=()=>{"
                  "document.querySelectorAll("
                    "'h1,h2,h3,h4,label,a,button,summary,[role=\"menuitem\"],[role=\"tab\"]'"
                  ").forEach(e=>{if(isMatch(e))hide(e);});"
                "};"
                "let tries=0;"
                "const tick=()=>{"
                  "scrub();"
                  "if(++tries<60)setTimeout(tick,300);"
                "};"
                "document.addEventListener('DOMContentLoaded',()=>{"
                  "tick();"
                  // keep an observer running for late-mounted dialogs
                  "new MutationObserver(scrub).observe(document.body,"
                    "{childList:true,subtree:true});"
                "});"
              "})();</script>";

            // ----- hide the bundle's native Reasoning panel -----------------
            // We render our own blue brain-iconed custom panel above the
            // message body during streaming, so the bundle's copy would be
            // a duplicate — hide it.  BUT: our panel is ephemeral DOM that
            // gets wiped when the bundle re-mounts the message (e.g. on
            // page reload from history).  In that case the bundle's panel
            // is the ONLY remaining copy of the reasoning text, so hiding
            // it would erase the user's reasoning history entirely.
            //
            // Strategy: only hide when our `__easyai-thinking` panel is
            // present in the same assistant message.  Otherwise leave the
            // bundle's panel visible so past messages still show their
            // reasoning.
            inj <<
              "<script>(()=>{"
                "console.log('[easyai-inject] block3 hide-bundle-reasoning (custom-only)');"
                "const isReasoning=(t)=>{"
                  "if(!t)return false;t=t.trim();"
                  "if(t.length>500)return false;"
                  "return /^Reasoning(?:\\.\\.\\.|\\u2026|\\b)/i.test(t);"
                "};"
                "const hasOurPanel=(el)=>{"
                  "const msg=el.closest"
                    "?el.closest('[aria-label=\"Assistant message with actions\"]')"
                    ":null;"
                  "return !!(msg&&msg.querySelector('.__easyai-thinking'));"
                "};"
                "const hide=()=>{"
                  // First sweep: anything with my-2 (the wrapper class
                  // CollapsibleContentBlock receives).
                  "document.querySelectorAll('[class*=\"my-2\"]').forEach(el=>{"
                    "if(el.dataset.easyaiHidden)return;"
                    "const txt=(el.innerText||el.textContent||'').trim();"
                    "if(!isReasoning(txt))return;"
                    "if(!hasOurPanel(el))return;"
                    "el.dataset.easyaiHidden='1';"
                    "el.style.setProperty('display','none','important');"
                  "});"
                  // Fallback sweep: any leaf element whose trimmed text
                  // is exactly the Reasoning label.  Walks up to find the
                  // nearest plausible card wrapper and hides that.
                  "document.querySelectorAll('span,div,h1,h2,h3,h4,p').forEach(el=>{"
                    "if(el.children.length)return;"
                    "const t=(el.innerText||el.textContent||'').trim();"
                    "if(!t||!/^Reasoning(\\.\\.\\.|\\u2026)?$/i.test(t))return;"
                    "if(!hasOurPanel(el))return;"
                    "let p=el;"
                    "for(let i=0;i<8&&p;i++){"
                      "if(p.dataset&&p.dataset.easyaiHidden)return;"
                      "if(p.className&&/\\bmy-2\\b|\\bcollapse\\b|\\bcard\\b/i.test(p.className+'')){"
                        "p.dataset.easyaiHidden='1';"
                        "p.style.setProperty('display','none','important');"
                        "return;"
                      "}"
                      "p=p.parentElement;"
                    "}"
                    // Last-resort: hide just the label so the empty
                    // shell doesn't take vertical space.
                    "if(!el.dataset.easyaiHidden){"
                      "el.dataset.easyaiHidden='1';"
                      "el.style.setProperty('display','none','important');"
                    "}"
                  "});"
                "};"
                // Kept as a no-op so existing call sites in monitorSSE
                // (window.__easyaiCollapseReasoning) keep linking; our
                // custom panel handles its own collapse via closeThinking.
                "window.__easyaiCollapseReasoning=()=>{};"
                "let n=0;"
                "const tick=()=>{hide();if(++n<120)setTimeout(tick,300);};"
                "document.addEventListener('DOMContentLoaded',()=>{"
                  "tick();"
                  "new MutationObserver(hide).observe(document.body,"
                    "{childList:true,subtree:true,characterData:true});"
                "});"
              "})();</script>";

            // ----- fetch interceptor: stub unsupported endpoints AND inject
            //       sampling overrides from the tone dropdown ----------------
            inj <<
              "<script>(()=>{"
                "console.log('[easyai-inject] block4 fetch-interceptor + monitorSSE + thinking-panel');"
                "const orig=window.fetch.bind(window);"
                "const stub=(b,s=200)=>new Response(JSON.stringify(b),{"
                  "status:s,headers:{'Content-Type':'application/json'}"
                "});"
                "const reject=stub({error:{message:'not supported on easyai-server'}},501);"
                "const TONES={"
                  // "default" resolves to whichever preset the server
                  // was launched with (--preset CLI / [ENGINE] preset
                  // INI). Values baked at template-render time below
                  // so the fetch-interceptor doesn't need a round-trip
                  // to find them.
                  "default:{t:"
                  << ctx->default_preset.temperature
                  << ",top_p:" << ctx->default_preset.top_p
                  << ",top_k:" << ctx->default_preset.top_k
                  << "},"
                  "deterministic:{t:0.0,top_p:1.0,top_k:1},"
                  "precise:{t:0.2,top_p:0.95,top_k:40},"
                  "balanced:{t:0.7,top_p:0.95,top_k:40},"
                  "creative:{t:1.0,top_p:0.95,top_k:40},"
                  "wild:{t:1.4,top_p:0.98,top_k:60}"
                "};"
                // Load the previous tone from localStorage; fall back to
                // 'default' (= the server's configured preset) for fresh
                // sessions / cleared storage. Existing choices persist
                // across reloads — no force-overwrite.
                "try{window.__easyaiTone=localStorage.getItem('easyai-tone')||'default';}"
                  "catch(e){window.__easyaiTone='default';}"
                "window.fetch=async(input,init)=>{"
                  "let url=typeof input==='string'?input:(input&&input.url)||'';"
                  "try{const u=new URL(url,location.origin);url=u.pathname;}catch(e){}"
                  "if(url==='/authorize'||url==='/token'||url==='/register'"
                    "||url.startsWith('/.well-known/')) return reject;"
                  "if(url==='/models/load'||url==='/models/unload') return reject;"
                  "if(url==='/cors-proxy'||url.startsWith('/home/web_user/')"
                    "||url==='/dev/poll') return reject;"
                  "if(url==='/properties') return stub({});"
                  // Inject tone-based sampling when the request body doesn't
                  // already pin those fields, and tee the response so we
                  // can drive the live activity pill from the SSE stream.
                  "if(url.endsWith('/v1/chat/completions')&&init&&init.body){"
                    "try{"
                      "const body=JSON.parse(init.body);"
                      "const t=TONES[window.__easyaiTone];"
                      "if(t){"
                        "if(body.temperature===undefined)body.temperature=t.t;"
                        "if(body.top_p===undefined)body.top_p=t.top_p;"
                        "if(body.top_k===undefined)body.top_k=t.top_k;"
                      "}"
                      "init={...init,body:JSON.stringify(body)};"
                    "}catch(e){}"
                    "const r=await orig(input,init);"
                    "if(r.body&&typeof r.body.tee==='function'){"
                      "const [a,b]=r.body.tee();"
                      "monitorSSE(b);"   // a goes to caller, b to pill
                      "return new Response(a,{headers:r.headers,status:r.status,statusText:r.statusText});"
                    "}"
                    "return r;"
                  "}"
                  "return orig(input,init);"
                "};"

                // SSE → activity pill state machine.
                // ---- per-message thinking panel (DISABLED) -----------------
                // We used to render our own collapsible reasoning panel; now
                // we drive the bundle's native one (data-slot="collapsible-
                // trigger" / "text-xs leading-relaxed ...") via DOM clicks.
                // Open on the first reasoning_content delta of a phase,
                // collapse on the first content delta after reasoning.  If
                // the model alternates think → speak → think the controller
                // re-opens / re-collapses around each transition.  Block 3
                // (hide-bundle-reasoning) is gated on `hasOurPanel` so it
                // becomes a no-op once the custom panel is gone.
                "window.__easyaiCustomThink=false;"
                "const THINK=new WeakMap();"      // msg → currently-open panel
                "const findLastAssistantMsg=()=>{"
                  "const all=document.querySelectorAll("
                    "'[aria-label=\"Assistant message with actions\"]');"
                  "return all.length?all[all.length-1]:null;"
                "};"
                "const ensureThinkPanel=(msg)=>{"
                  "let p=THINK.get(msg);"
                  "if(p&&document.contains(p)&&(p.parentNode===msg||msg.contains(p))){"
                    // Re-opening case: a previous thinking phase ran,
                    // closeThinking() collapsed the panel, then a new
                    // <think>...</think> arrived (e.g. agentic multi-hop
                    // turn).  Re-open the same panel and reset its
                    // summary label so the user sees the new thinking.
                    "if(!p.open){"
                      "p.open=true;"
                      "const lab=p.querySelector('.sumlabel');"
                      "if(lab)lab.textContent='reasoning…';"
                    "}"
                    "return p;"
                  "}"
                  // Svelte sometimes re-mounts the assistant bubble during a
                  // generation (state-store flush), giving us a brand-new
                  // `msg` node.  When that happens our WeakMap entry is
                  // stale, but the previous panel may already have been
                  // re-parented into the new msg.  Dedupe by DOM query: if
                  // ANY __easyai-thinking already lives inside the current
                  // msg, reuse it; if MORE than one exist, keep the first
                  // and remove duplicates so the user never sees stacked
                  // empty panels.
                  "const existing=msg.querySelectorAll(':scope .__easyai-thinking');"
                  "if(existing.length>0){"
                    "for(let i=1;i<existing.length;i++)existing[i].remove();"
                    "const e0=existing[0];"
                    // Same re-opening logic as above.
                    "if(!e0.open){"
                      "e0.open=true;"
                      "const lab=e0.querySelector('.sumlabel');"
                      "if(lab)lab.textContent='reasoning…';"
                    "}"
                    "THINK.set(msg,e0);"
                    "return e0;"
                  "}"
                  "p=document.createElement('details');"
                  "p.className='__easyai-thinking';"
                  "p.open=true;"
                  // Outer panel: only the visual wrapper (border, background,
                  // text color).  Padding lives on <summary> via p-3 (per the
                  // bundle's reasoning-header structure) and on the body div.
                  "p.style.cssText="
                    "'margin:.5rem 0;"
                    " border-left:2px solid #5b8dee;"
                    " background:rgba(91,141,238,.06);"
                    " border-radius:4px;"
                    " font-size:.7rem;color:#8b949e;"
                    " font-family:ui-monospace,SFMono-Regular,Menlo,monospace;"
                    " line-height:1.5;';"
                  // Summary mirrors the bundle's CollapsibleContentBlock
                  // header layout: flex, full width, cursor-pointer, items-
                  // centered, justify-between, p-3.  Order on the left:
                  // lucide brain icon → "Thinking" → live state label
                  // ("reasoning…" while streaming, "reasoning" when closed).
                  "p.innerHTML="
                    "'<summary class=\"flex w-full cursor-pointer "
                      "items-center justify-between p-3\" "
                      "style=\"list-style:none;outline:none;user-select:none;"
                      "color:#5b8dee\">"
                      "<span style=\"display:inline-flex;align-items:center;"
                        "gap:.5rem\">"
                        "<svg xmlns=\"http://www.w3.org/2000/svg\" "
                          "width=\"16\" height=\"16\" viewBox=\"0 0 24 24\" "
                          "fill=\"none\" stroke=\"currentColor\" "
                          "stroke-width=\"2\" stroke-linecap=\"round\" "
                          "stroke-linejoin=\"round\" "
                          "class=\"lucide-icon lucide lucide-brain h-4 w-4\">"
                          "<path d=\"M12 5a3 3 0 1 0-5.997.125 4 4 0 0 0-2.526 5.77 4 4 0 0 0 .556 6.588A4 4 0 1 0 12 18Z\"></path>"
                          "<path d=\"M12 5a3 3 0 1 1 5.997.125 4 4 0 0 1 2.526 5.77 4 4 0 0 1-.556 6.588A4 4 0 1 1 12 18Z\"></path>"
                          "<path d=\"M15 13a4.5 4.5 0 0 1-3-4 4.5 4.5 0 0 1-3 4\"></path>"
                          "<path d=\"M17.599 6.5a3 3 0 0 0 .399-1.375\"></path>"
                          "<path d=\"M6.003 5.125A3 3 0 0 0 6.401 6.5\"></path>"
                          "<path d=\"M3.477 10.896a4 4 0 0 1 .585-.396\"></path>"
                          "<path d=\"M19.938 10.5a4 4 0 0 1 .585.396\"></path>"
                          "<path d=\"M6 18a4 4 0 0 1-1.967-.516\"></path>"
                          "<path d=\"M19.967 17.484A4 4 0 0 1 18 18\"></path>"
                        "</svg>"
                        "<span style=\"font-weight:600;font-size:.72rem;"
                          "letter-spacing:.02em\">Thinking</span>"
                        "<span class=\"sumlabel\" style=\"font-weight:600;"
                          "font-size:.72rem;opacity:.75\">reasoning…</span>"
                      "</span>"
                    "</summary>'"
                    "+'<div class=\"t\" style=\"margin:0;"
                      "padding:0 .75rem .6rem .75rem;"
                      "white-space:pre-wrap;max-height:18em;"
                      "overflow-y:auto\"></div>';"
                  "msg.insertBefore(p,msg.firstChild);"
                  "THINK.set(msg,p);"
                  "return p;"
                "};"
                // Find the bundle's native reasoning collapsible inside an
                // assistant message.  Bundle uses Radix-style triggers with
                // data-slot="collapsible-trigger"; multiple may exist if the
                // model alternates phases — pick the LAST one (most recent).
                "const findReasoningTrigger=(msg)=>{"
                  "if(!msg)return null;"
                  "const all=msg.querySelectorAll("
                    "'[data-slot=\"collapsible-trigger\"]');"
                  "return all.length?all[all.length-1]:null;"
                "};"
                "const isTriggerOpen=(tr)=>{"
                  "if(!tr)return false;"
                  "return tr.getAttribute('data-state')==='open'"
                    "||tr.getAttribute('aria-expanded')==='true';"
                "};"
                "const setReasoningOpen=(want)=>{"
                  "const msg=findLastAssistantMsg();"
                  "if(!msg)return false;"
                  "const tr=findReasoningTrigger(msg);"
                  "if(!tr)return false;"
                  "if(isTriggerOpen(tr)!==want){"
                    // Click toggles — the bundle collapsible exposes no
                    // imperative open() so this is the only handle.
                    "tr.click();"
                  "}"
                  "return true;"
                "};"
                // appendThinking is fired per reasoning_content delta.  The
                // bundle paints the text itself; our job is just to keep
                // the panel open while the model is mid-think.  Retry until
                // the bundle has rendered the trigger (first delta may
                // arrive before mount).
                "function appendThinking(text){"
                  "if(window.__easyaiCustomThink)return;"
                  "if(!setReasoningOpen(true)){"
                    "let tries=0;"
                    "const id=setInterval(()=>{"
                      "if(setReasoningOpen(true)||++tries>20)clearInterval(id);"
                    "},50);"
                  "}"
                "};"
                // closeThinking is fired on first content delta after a
                // reasoning phase AND on stream finish.  Collapse the
                // bundle panel; if the model resumes thinking later, the
                // next appendThinking re-opens it.
                "function closeThinking(){"
                  "if(window.__easyaiCustomThink)return;"
                  "setReasoningOpen(false);"
                "};"
                "async function monitorSSE(stream){"
                  "const set=window.__easyaiSetStatus||(()=>{});"
                  "set('answering');"
                  "const reader=stream.getReader();"
                  "const dec=new TextDecoder();"
                  "let buf='',inThink=false,inToolCallTag=false,startMs=performance.now();"
                  "let lastTimings=null;"
                  // Live metrics: count emitted chunks (close enough to
                  // tokens for UX) and clock the elapsed time so the chip
                  // and the bar can show "234 tok · 4.2s · 55 t/s" while
                  // generation is still in flight.  Updated on every
                  // delta below; rendered through liveTick().
                  "let liveTok=0,liveStart=0;"
                  "const liveExtra=()=>{"
                    "const elapsed=Math.max(1,performance.now()-liveStart);"
                    "const tps=liveTok/(elapsed/1000);"
                    "return{"
                      "tokens:liveTok,"
                      "elapsedMs:elapsed,"
                      "tps:tps,"
                      "live:true,"
                    "};"
                  "};"
                  "let lastState='answering';"
                  "const liveTick=setInterval(()=>{"
                    "if(liveTok>0&&lastState!=='complete'&&lastState!=='error')"
                      "set(lastState,liveExtra());"
                  "},200);"
                  "const setLive=(s,e)=>{lastState=s;set(s,e);};"
                  "try{while(true){"
                    "const{value,done}=await reader.read();"
                    "if(done)break;"
                    "buf+=dec.decode(value,{stream:true});"
                    "while(true){"
                      "const idx=buf.indexOf('\\n\\n');"
                      "if(idx<0)break;"
                      "const ev=buf.slice(0,idx);buf=buf.slice(idx+2);"
                      "let evtType='message';const dataLines=[];"
                      "for(let line of ev.split('\\n')){"
                        "if(line.endsWith('\\r'))line=line.slice(0,-1);"
                        "if(line.startsWith('event:'))evtType=line.slice(6).trim();"
                        "else if(line.startsWith('data:'))dataLines.push(line.slice(5).replace(/^ /,''));"
                      "}"
                      "const data=dataLines.join('\\n');"
                      "if(!data||data==='[DONE]')continue;"
                      "if(evtType==='easyai.tool_call'){"
                        "try{const j=JSON.parse(data);setLive('fetching',j.name||'tool');}"
                        "catch(e){setLive('fetching');}"
                        "continue;"
                      "}"
                      "if(evtType==='easyai.tool_result'){"
                        // Back to whatever generation phase we're in.
                        "setLive(inThink?'thinking':'answering',liveExtra());"
                        "continue;"
                      "}"
                      // easyai.prompt_eval — server-side llama-server-style
                      // 'prompt eval time' report fired right after the
                      // prompt-eval llama_decode loop completes and BEFORE
                      // the first token is sampled.  Drives the chip into
                      // a distinct 'processing' state so the user sees the
                      // moment ingestion finished, with live n_tokens /
                      // prompt_ms / t/s metrics.  The matching dim line
                      // also arrives via a reasoning_content delta so it
                      // shows in the Thinking panel for posterity.
                      "if(evtType==='easyai.prompt_eval'){"
                        "try{"
                          "const j=JSON.parse(data);"
                          "const tps=(j.tokens_per_second||0).toFixed(1);"
                          "const ms=Math.round(j.prompt_ms||0);"
                          "setLive('processing',j.n_tokens+'tok·'+ms+'ms·'+tps+'t/s');"
                        "}catch(e){setLive('processing');}"
                        "continue;"
                      "}"
                      "try{"
                        "const j=JSON.parse(data);"
                        "if(j.timings){lastTimings=j.timings;"
                        "if(window.__easyaiPushTimings)window.__easyaiPushTimings(j.timings);}"
                        "const ch=j.choices&&j.choices[0];"
                        "if(ch&&ch.delta){"
                          // Any reasoning or content delta = a new chunk.
                          // Counting CHUNKS (not characters) is closer to
                          // token count for UX purposes — the model emits
                          // ~one chunk per token in the streaming path.
                          "if(liveStart===0)liveStart=performance.now();"
                          // -- THINKING PANEL --------------------------------
                          "if(typeof ch.delta.reasoning_content==='string'){"
                            "liveTok++;"
                            "appendThinking(ch.delta.reasoning_content);"
                            "setLive('thinking',liveExtra());"
                          "}"
                          // -- VISIBLE CONTENT -------------------------------
                          "if(typeof ch.delta.content==='string'){"
                            "const c=ch.delta.content;"
                            "liveTok++;"
                            "if(c.length>0)closeThinking();"
                            "if(!inThink&&/<think(?:ing)?>/i.test(c)){inThink=true;setLive('thinking',liveExtra());}"
                            "else if(inThink&&/<\\/think(?:ing)?>/i.test(c)){inThink=false;setLive('answering',liveExtra());}"
                            "else if(!inThink)setLive('answering',liveExtra());"
                          "}"
                          // Push our own synthetic timings so the bar's
                          // 'last' field tracks live tok/s mid-stream.
                          "if(window.__easyaiPushTimings){"
                            "const now=performance.now();"
                            "if(liveStart>0)window.__easyaiPushTimings({"
                              "predicted_n:liveTok,"
                              "predicted_ms:now-liveStart,"
                              "live:true,"
                            "});"
                          "}"
                        "}"
                        "if(ch&&ch.finish_reason){"
                          "clearInterval(liveTick);"
                          "closeThinking();"
                          "if(window.__easyaiCollapseReasoning)window.__easyaiCollapseReasoning();"
                          "const t=lastTimings;"
                          "let msg='';"
                          "if(t&&t.predicted_n&&t.predicted_ms){"
                            "const tps=(t.predicted_n/(t.predicted_ms/1000)).toFixed(1);"
                            "msg=t.predicted_n+'tok·'+(t.predicted_ms/1000).toFixed(1)+'s·'+tps+'t/s';"
                          "}"
                          "setLive('complete',msg);"
                          // Server-side incomplete-turn signal — single
                          // boolean coming through on `timings.incomplete`
                          // when the model produced no tool_call AND only
                          // a tiny amount of visible content (typically
                          // an announcement).  Append an amber notice
                          // into the assistant message body so the user
                          // sees what happened instead of a silent stall.
                          // Both this webui and libeasyai-cli read the
                          // SAME flag, so behaviour is consistent across
                          // surfaces.
                          "if(t&&t.incomplete){"
                            "const all=document.querySelectorAll("
                              "'[aria-label=\"Assistant message with actions\"]');"
                            "const last=all.length?all[all.length-1]:null;"
                            "if(last&&!last.querySelector('.__easyai-empty')){"
                              "const ph=document.createElement('div');"
                              "ph.className='__easyai-empty';"
                              "ph.style.cssText="
                                "'margin:.5rem 0;padding:.45rem .65rem;"
                                "border-left:2px solid #d29922;"
                                "background:rgba(210,153,34,.08);"
                                "border-radius:4px;"
                                "font-size:.78rem;color:#d29922;"
                                "font-family:ui-monospace,SFMono-Regular,Menlo,monospace;';"
                              "ph.textContent='(incomplete response \\u2014 "
                                "the model produced no tool_call and only "
                                "a tiny visible reply.  Common causes: it "
                                "announced a tool but never emitted it, "
                                "tool-error chain (rate-limit), over-"
                                "prescriptive system prompt, model gave up.  "
                                "Try rephrasing more specifically, or check "
                                "journalctl -u easyai-server for the matching "
                                "[easyai-server] WARN incomplete response line.)';"
                              "last.appendChild(ph);"
                            "}"
                          "}"
                        "}"
                      "}catch(e){}"
                    "}"
                  "}}catch(e){clearInterval(liveTick);setLive('error');}"
                "}"
              "})();</script>";

            // ----- live metrics + tone chip + per-message status ------------
            //   Metrics    — painted directly inside the bundle's existing
            //                `.chat-processing-info-detail` element via
            //                renderOverview() — no custom host of our own.
            //                Live-updates ctx / token count / elapsed / t/s
            //                while the model is streaming, then freezes on
            //                finish_reason.  A MutationObserver re-injects
            //                whenever Svelte re-renders the anchor.
            //   Tone chip  — separate Shadow DOM host, mounted as a sibling
            //                of the bundle's model-name badge inside the
            //                prompt form, so layout follows hydration.
            //   Per-msg    — for each assistant message, we append a small
            //   status       live status indicator inline next to the
            //                copy/edit/fork/delete actions; chip text is
            //                updated by window.__easyaiSetStatus.
            inj <<
              "<script>(()=>{"
                "console.log('[easyai-inject] block5 metrics + tone-badge + chip');"

                // Theme-aware SVG icons.  The bundle ships some icons with
                // a hardcoded dark fill/stroke (e.g. fill=\"#0d1117\") that
                // only reads on light backgrounds — invisible in dark mode.
                // We inject a CSS rule that forces SVGs inside buttons,
                // headings, and any element whose fill/stroke is a known
                // dark hex value to use `currentColor` instead.  This makes
                // the icon track the active foreground (text-foreground via
                // CSS variable) so it flips automatically with the theme.
                //
                // Applied EAGERLY at script start (before DOMContentLoaded)
                // so the empty-state landing page (just the form, no chat
                // yet) also benefits.
                "if(!document.getElementById('__easyaiSvgThemeStyle')){"
                  "const st=document.createElement('style');"
                  "st.id='__easyaiSvgThemeStyle';"
                  "st.textContent="
                    // Narrow rule: only icons that hardcode a near-black
                    // fill / stroke get re-pointed at currentColor.  The
                    // broader rules we tried earlier (anything with an
                    // explicit non-none fill, plus paths with neither fill
                    // nor stroke) flattened sidebar icons that have
                    // intentional multi-color fills (e.g. the New Chat
                    // bubble with a light interior).  Targeting the
                    // specific dark hex values that read invisibly on the
                    // dark theme avoids the collateral damage while still
                    // rescuing legacy icons with hardcoded #000.  The
                    // favicon ships its own brand colors via media query,
                    // so it doesn't need this rule.
                    "'[fill=\"#0d1117\"],[fill=\"#000\"],[fill=\"black\"],"
                    "[fill=\"#000000\"],[fill=\"#15191f\"],[fill=\"#1a1f29\"]"
                    "{fill:currentColor!important}"
                    "[stroke=\"#0d1117\"],[stroke=\"#000\"],[stroke=\"black\"],"
                    "[stroke=\"#000000\"],[stroke=\"#15191f\"],[stroke=\"#1a1f29\"]"
                    "{stroke:currentColor!important}';"
                  "(document.head||document.documentElement).appendChild(st);"
                "}"

                // Hide the floating top-right Settings gear button.  The
                // bundle's ChatScreenHeader is a `position:fixed` header
                // whose sole content is a round Settings-icon button
                // wired to chatSettingsDialog.open().  Server operators
                // who want the model to be the only surface (no in-page
                // sampling tweaks, no theme switching from the chat tab)
                // need it gone — this hides the whole header so the
                // gear doesn't even register a click target.  Settings
                // are still reachable from the bundle's sidebar menu if
                // an operator un-hides this rule.
                "if(!document.getElementById('__easyaiHideSettingsStyle')){"
                  "const st=document.createElement('style');"
                  "st.id='__easyaiHideSettingsStyle';"
                  "st.textContent="
                    "'header.fixed.top-0.right-0.left-0.z-50.justify-end"
                      "{display:none!important}';"
                  "(document.head||document.documentElement).appendChild(st);"
                "}"

                // Lock the bundle's reasoning / collapsible panel to ~15
                // lines when open.  The bundle's CollapsibleContentBlock
                // body uses --max-message-height (24rem to 80dvh), which
                // means a long chain-of-thought scrolls the whole chat
                // out of view.  We cap the inner scroll at ~15 lines and
                // let the panel scroll internally — the message body stays
                // anchored and the user can scrub through reasoning
                // without losing the answer below.
                //
                // 18em ≈ 15 lines for the bundle's font-mono.text-xs.
                // leading-relaxed (text-xs=.75rem * 1.625 = 1.22rem/line,
                // 15 * 1.22 ≈ 18.3rem; 18em is plenty close and matches
                // the panel's existing font-size scale).  Scoped to
                // assistant messages so we don't accidentally clamp other
                // collapsibles (e.g. the system-message preview).
                "if(!document.getElementById('__easyaiThinkLockStyle')){"
                  "const st=document.createElement('style');"
                  "st.id='__easyaiThinkLockStyle';"
                  "st.textContent="
                    "'[aria-label=\"Assistant message with actions\"] "
                      "[data-slot=\"collapsible-content\"]{"
                      "max-height:18em!important;"
                      "overflow-y:auto!important}'"
                    // Same lock for our own custom thinking panel body
                    // (still present as a fallback when the bundle's
                    // collapsible isn't mounted yet) — ~15 lines.
                    "+'.__easyai-thinking>div.t{"
                      "max-height:18em!important;"
                      "overflow-y:auto!important}';"
                  "(document.head||document.documentElement).appendChild(st);"
                "}"

                // Metrics moved out of a custom pill — we now paint them
                // directly into the bundle's own `.chat-processing-info-detail`
                // element (see renderOverview() below).  That element is
                // already correctly anchored by the SvelteKit layout, so the
                // pill / its Shadow DOM host / SHARED_STYLE were removed.

                // === TONE + TOOLS — cloned from the bundle's pill button =
                //
                // Instead of building bespoke shadow-DOM badges with copied
                // palettes, we clone the bundle's existing pill button
                // (cloneNode(false) — shallow, attrs+classes only) and inject
                // our own innerHTML + click handler.  Result: pixel-identical
                // to the bundle's pill, with zero PILL_CLASS strings to
                // maintain, zero theme-mode palette copying, and zero shadow
                // DOM.  If the bundle ever rethemes the pill, our clones
                // follow automatically because Tailwind class semantics
                // come straight from :root CSS variables.
                //
                // Caveats:
                //   * the clones are detached from Svelte (no reactivity) —
                //     fine, we attach our own click handlers;
                //   * the bundle's reconciler may rip the clones out of its
                //     managed row on re-renders — handled by the idempotent
                //     reposition() below + a MutationObserver on body
                //     childList (no subtree, to avoid feedback loops).
                "const PILL_SEL="
                  "'button[class*=\"bg-muted-foreground\"]"
                  "[class*=\"rounded-sm\"][class*=\"px-1.5\"]';"
                "const TONE_ID='__easyaiToneHost';"
                "const TOOLS_ID='__easyaiToolsHost';"
                // 'default' first (cycle starts on the operator's preset);
                // 'wild' last (exploratory escape hatch — keep it out of
                // the way during normal cycling).
                "const TONE_ORDER="
                  "['default','deterministic','precise','balanced','creative','wild'];"
                "let toneBtn=null,toolsBtn=null,toolsPop=null;"
                // findPill MUST skip our own clones — `cloneNode(false)`
                // copies the pill's class string, so toneBtn and toolsBtn
                // also match PILL_SEL.  Without this guard, querySelector
                // would return whichever clone happens to be first in DOM
                // order, and reposition() would treat it as the anchor on
                // the next tick — causing tone+tools to swap positions on
                // every reposition (visible as a continuous flicker).
                "const findPill=()=>{"
                  "const all=document.querySelectorAll(PILL_SEL);"
                  "for(const p of all){"
                    "if(p.id===TONE_ID||p.id===TOOLS_ID)continue;"
                    "return p;"
                  "}"
                  "return null;"
                "};"
                "const setToneLabel=(b)=>{"
                  "const lbl=b.querySelector('[data-tone-current]');"
                  "if(lbl)lbl.textContent=window.__easyaiTone||'default';"
                "};"

                "const buildToneFromPill=(pill)=>{"
                  "if(toneBtn)return toneBtn;"
                  // Shallow clone — outer <button> with the pill's exact
                  // classes / attrs, but no children.  We provide our own.
                  "const b=pill.cloneNode(false);"
                  "b.id=TONE_ID;"
                  "b.type='button';"
                  "b.removeAttribute('disabled');"
                  "b.removeAttribute('aria-disabled');"
                  "b.removeAttribute('data-svelte-h');"
                  "b.setAttribute('aria-label','tone');"
                  "b.innerHTML="
                    "'<svg width=\"14\" height=\"14\" viewBox=\"0 0 24 24\" "
                      "fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" "
                      "stroke-linecap=\"round\" stroke-linejoin=\"round\" "
                      "style=\"opacity:.75;flex-shrink:0\">"
                      "<line x1=\"4\" y1=\"7\" x2=\"20\" y2=\"7\"/>"
                      "<line x1=\"4\" y1=\"17\" x2=\"20\" y2=\"17\"/>"
                      "<circle cx=\"10\" cy=\"7\" r=\"2.4\" fill=\"currentColor\"/>"
                      "<circle cx=\"15\" cy=\"17\" r=\"2.4\" fill=\"currentColor\"/>"
                    "</svg>"
                    "<span data-tone-current "
                      "style=\"font-variant-numeric:tabular-nums\">balanced</span>';"
                  "setToneLabel(b);"
                  "b.addEventListener('click',(e)=>{"
                    "e.preventDefault();e.stopPropagation();"
                    "const cur=window.__easyaiTone||'default';"
                    "const i=TONE_ORDER.indexOf(cur);"
                    "const next=TONE_ORDER[(i+1)%TONE_ORDER.length];"
                    "window.__easyaiTone=next;"
                    "try{localStorage.setItem('easyai-tone',next);}catch(_){}"
                    "setToneLabel(b);"
                  "});"
                  "toneBtn=b;"
                  "return b;"
                "};"

                "const buildToolsFromPill=(pill)=>{"
                  "if(toolsBtn)return toolsBtn;"
                  "const b=pill.cloneNode(false);"
                  "b.id=TOOLS_ID;"
                  "b.type='button';"
                  "b.removeAttribute('disabled');"
                  "b.removeAttribute('aria-disabled');"
                  "b.removeAttribute('data-svelte-h');"
                  "b.setAttribute('aria-label','available tools');"
                  "b.setAttribute('aria-haspopup','dialog');"
                  "b.innerHTML="
                    "'<svg width=\"14\" height=\"14\" viewBox=\"0 0 24 24\" "
                      "fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" "
                      "stroke-linecap=\"round\" stroke-linejoin=\"round\" "
                      "style=\"opacity:.75;flex-shrink:0\">"
                      "<path d=\"M14.7 6.3a3.5 3.5 0 0 1 4.6 4.6l-2.8-2.8-1.8 1.8 2.8 2.8a3.5 3.5 0 0 1-4.6-4.6\"/>"
                      "<path d=\"M5 19l8.5-8.5\"/>"
                    "</svg>"
                    "<span>tools</span>"
                    "<span data-count "
                      "style=\"opacity:.7;font-variant-numeric:tabular-nums\"></span>';"

                  // Popover at document.body — escapes ancestor backdrop-
                  // filter containing block, same as before.
                  "let pop=document.getElementById('__easyaiToolsPop');"
                  "if(pop)pop.remove();"
                  "pop=document.createElement('div');"
                  "pop.id='__easyaiToolsPop';"
                  "pop.setAttribute('role','dialog');"
                  "pop.setAttribute('aria-label','available tools');"
                  "pop.style.cssText="
                    "'position:fixed;display:none;pointer-events:auto;"
                    "min-width:14rem;max-width:22rem;"
                    "background:#0f1318;border:1px solid #2a313b;"
                    "border-radius:.5rem;padding:.45rem .5rem;"
                    "box-shadow:0 6px 24px rgba(0,0,0,.5);"
                    "color:#c9d1d9;font-size:.72rem;"
                    "font-family:-apple-system,system-ui,sans-serif;"
                    "max-height:60vh;overflow-y:auto;"
                    "z-index:2147483646;';"

                  "if(!document.getElementById('__easyaiToolsPopStyle')){"
                    "const ps=document.createElement('style');"
                    "ps.id='__easyaiToolsPopStyle';"
                    "ps.textContent="
                      "'#__easyaiToolsPop .row{padding:.32rem .35rem;"
                        "border-radius:.3rem;cursor:default;display:flex;"
                        "flex-direction:column;gap:.15rem;line-height:1.35}"
                      "#__easyaiToolsPop .row+.row{border-top:1px solid #1f242b}"
                      "#__easyaiToolsPop .row:hover{background:rgba(91,141,238,.08)}"
                      "#__easyaiToolsPop .name{font-family:ui-monospace,"
                        "SFMono-Regular,Menlo,monospace;font-size:.72rem;"
                        "color:#5b8dee;font-weight:600}"
                      "#__easyaiToolsPop .desc{color:#8b949e;font-size:.68rem;"
                        "white-space:pre-wrap;line-height:1.4}"
                      "#__easyaiToolsPop .empty{color:#8b949e;padding:.4rem .35rem}';"
                    "(document.head||document.documentElement).appendChild(ps);"
                  "}"

                  "const ensurePopAttached=()=>{"
                    "if(!pop.isConnected&&document.body)"
                      "document.body.appendChild(pop);"
                  "};"
                  "ensurePopAttached();"
                  "if(!pop.isConnected){"
                    "document.addEventListener('DOMContentLoaded',"
                      "ensurePopAttached,{once:true});"
                  "}"

                  "const cnt=b.querySelector('[data-count]');"
                  "const renderList=(tools)=>{"
                    "if(!tools||!tools.length){"
                      "pop.innerHTML='<div class=\"empty\">no tools registered</div>';"
                      "cnt.textContent='0';return;"
                    "}"
                    "cnt.textContent=String(tools.length);"
                    "let html='';"
                    "for(const t of tools){"
                      "const name=(t.name||'').replace(/[<>&]/g,c=>"
                        "c==='<'?'&lt;':c==='>'?'&gt;':'&amp;');"
                      "const desc=(t.description||'').replace(/[<>&]/g,c=>"
                        "c==='<'?'&lt;':c==='>'?'&gt;':'&amp;');"
                      "html+='<div class=\"row\" title=\"'+desc+'\">'+"
                        "'<span class=\"name\">'+name+'</span>'+"
                        "'<span class=\"desc\">'+desc+'</span></div>';"
                    "}"
                    "pop.innerHTML=html;"
                  "};"
                  "renderList([]);"
                  "fetch('/v1/tools').then(r=>r.json()).then(j=>{"
                    "renderList((j&&j.data)||[]);"
                  "}).catch(()=>{renderList([]);});"

                  "const placePop=()=>{"
                    "const r=b.getBoundingClientRect();"
                    "const vw=window.innerWidth;"
                    "pop.style.left=Math.min(vw-16,Math.max(8,r.left))+'px';"
                    "pop.style.bottom=(window.innerHeight-r.top+6)+'px';"
                  "};"

                  "b.addEventListener('click',(e)=>{"
                    "e.preventDefault();e.stopPropagation();"
                    "ensurePopAttached();"
                    "const willOpen=pop.style.display!=='block';"
                    "if(willOpen){placePop();pop.style.display='block';}"
                    "else{pop.style.display='none';}"
                  "});"

                  "window.addEventListener('resize',()=>{"
                    "if(pop.style.display==='block')placePop();"
                  "});"
                  "window.addEventListener('scroll',()=>{"
                    "if(pop.style.display==='block')placePop();"
                  "},true);"
                  "document.addEventListener('click',(e)=>{"
                    "if(!toolsBtn)return;"
                    "if(toolsBtn.contains(e.target))return;"
                    "if(pop.contains(e.target))return;"
                    "pop.style.display='none';"
                  "});"

                  "toolsBtn=b;toolsPop=pop;"
                  "return b;"
                "};"

                // Reposition: walk UP from the pill until we hit a
                // horizontal flex container (display:flex, flex-direction
                // anything but column).  The pill itself often lives in
                // a vertical-flex column on the right side of the prompt
                // form — its immediate parent stacks children top-down,
                // which is exactly what just put tone+tools above the pill
                // last time.  By walking up to the enclosing horizontal
                // row and inserting tone+tools as siblings of the column
                // (the "pillBranch"), we get true left-to-right placement:
                //
                //     [ ...left content ][ tone ][ tools ][ pill-col ][ send ]
                //
                // Fast-path early-return keeps the MutationObserver from
                // feeding itself.
                "const findHorizontalRow=(pill)=>{"
                  "let p=pill.parentElement;"
                  "while(p&&p!==document.body){"
                    "const cs=getComputedStyle(p);"
                    "if(cs.display==='flex'"
                       "&&cs.flexDirection!=='column'"
                       "&&cs.flexDirection!=='column-reverse'){"
                      "return p;"
                    "}"
                    "p=p.parentElement;"
                  "}"
                  "return null;"
                "};"
                "const reposition=()=>{"
                  "const pill=findPill();"
                  "if(!pill)return;"
                  "if(!toneBtn)buildToneFromPill(pill);"
                  "if(!toolsBtn)buildToolsFromPill(pill);"
                  "if(!toneBtn||!toolsBtn)return;"
                  "const row=findHorizontalRow(pill);"
                  "if(!row)return;"
                  // The pill may be nested several levels deep inside a
                  // flex-column.  Find the direct child of `row` that
                  // contains the pill — that's where tone+tools must land
                  // as siblings, ahead of it.
                  "let pillBranch=pill;"
                  "while(pillBranch&&pillBranch.parentElement!==row){"
                    "pillBranch=pillBranch.parentElement;"
                  "}"
                  "if(!pillBranch)return;"
                  "if(toneBtn.parentElement===row&&"
                     "toolsBtn.parentElement===row&&"
                     "toneBtn.nextSibling===toolsBtn&&"
                     "toolsBtn.nextSibling===pillBranch){"
                    "return;"
                  "}"
                  "row.insertBefore(toneBtn,pillBranch);"
                  "row.insertBefore(toolsBtn,pillBranch);"
                "};"

                "if(document.documentElement){reposition();}"
                "document.addEventListener('DOMContentLoaded',()=>{"
                  "reposition();"
                  // Lightweight observer — body's direct children only,
                  // no subtree, so streaming-text mutations don't trigger
                  // us.  Combined with the fast-path return above, CPU is
                  // negligible.
                  "if(window.MutationObserver){"
                    "const mo=new MutationObserver(reposition);"
                    "mo.observe(document.body,{childList:true});"
                  "}"
                "});"
                "window.addEventListener('resize',reposition);"
                // Safety-net interval: covers cases the observer misses
                // (e.g. attribute-only changes that swap row identity).
                "setInterval(()=>{"
                  "reposition();"
                  // Re-paint metrics: the bundle re-mounts
                  // .chat-processing-info-detail on stream lifecycle.
                  "renderOverview();"
                "},250);"

                // --- per-message inline status chip -----------------------
                // For every <... aria-label='Assistant message with actions'>
                // we attach a small status indicator inside the action row
                // (next to copy/edit/fork/delete).  The chip is updated by
                // window.__easyaiSetStatus(state, extra) called from the
                // SSE monitor below.  Bundle re-renders are handled by the
                // MutationObserver re-attaching when the chip is missing.
                "const CHIP_MARK='__eaiChip';"
                "const STATE_DOT={"
                  "thinking:'#b97df3',answering:'#1f6feb',"
                  "fetching:'#4fb0ff',error:'#f85149',"
                  "complete:'#3fb950',idle:'#5b626a',"
                  // 'processing' = prompt-eval phase (llama-server's
                  // "prompt eval time").  Amber so it visually stands
                  // apart from blue answering / purple thinking.
                  "processing:'#d29922',"
                  // 'answered' = past assistant message, no live SSE; the
                  // chip renders an EMPTY circle (transparent fill +
                  // subtle border) so it visually distinguishes "this is
                  // history" from "this is the current turn".
                  "answered:'transparent'"
                "};"
                "const findActionRow=(msg)=>{"
                  // The action toolbar is the parent whose DIRECT children
                  // are buttons (>=2). Walk depth-first and pick the
                  // deepest match so we don't accidentally land on the
                  // whole message wrapper (which has lots of descendant
                  // buttons via code-block 'copy code' etc.).
                  "let best=null,bestDepth=-1;"
                  "const walk=(el,d)=>{"
                    "let direct=0;"
                    "for(const c of el.children){"
                      "if(c.tagName==='BUTTON'||c.getAttribute&&c.getAttribute('role')==='button')direct++;"
                    "}"
                    "if(direct>=2&&d>bestDepth){best=el;bestDepth=d;}"
                    "for(const c of el.children)walk(c,d+1);"
                  "};"
                  "walk(msg,0);"
                  "return best;"
                "};"
                // Inject a stylesheet ONCE for the pulsing dot keyframes.
                // Using a single global rule beats inlining @keyframes per
                // chip and lets us toggle the animation just by adding /
                // removing the .pulse class on the dot.
                "if(!document.getElementById('__easyaiChipStyles')){"
                  "const st=document.createElement('style');"
                  "st.id='__easyaiChipStyles';"
                  "st.textContent="
                    "'@keyframes __easyaiPulse{"
                      "0%,100%{opacity:1}"
                      "50%{opacity:.25}"
                    "}"
                    ".__easyaiDot.pulse{"
                      "animation:__easyaiPulse 1.05s ease-in-out infinite}';"
                  "document.head&&document.head.appendChild(st);"
                "}"
                // Build a chip element matching the response-metrics text
                // style (Output / t/s).  No background, no border, no pill
                // wrapper — just inline text with a small status dot, using
                // the bundle's Tailwind utilities so it tracks the active
                // light/dark theme automatically:
                //
                //   * `text-xs`              — same size as ctx/last/Output
                //   * `text-muted-foreground` — labels (matches Output's tone)
                //   * `inline-flex items-center gap-1.5` — layout
                //
                // The dot color is state-specific (set inline by setStatus);
                // base text color comes from text-muted-foreground so the
                // label dims naturally on both themes.  No applyBarPalette
                // anymore — CSS variables do the theme work for us.
                "const buildChip=(initialState)=>{"
                  "const chip=document.createElement('span');"
                  "chip.className="
                    "'text-xs inline-flex items-center gap-1.5 text-muted-foreground';"
                  "chip.style.marginLeft='.5rem';"
                  "chip.style.flexShrink='0';"
                  "chip.style.whiteSpace='nowrap';"
                  "const isAnswered=initialState==='answered';"
                  // Past assistant messages render as an empty circle (1px
                  // currentColor ring, transparent fill).  Active turns
                  // render as a solid dot in the appropriate state color.
                  "const dotStyle=isAnswered"
                    "?'width:.5rem;height:.5rem;border-radius:50%;'+"
                      "'background:transparent;border:1px solid currentColor;'+"
                      "'flex-shrink:0;box-sizing:border-box;opacity:.6'"
                    ":'width:.48rem;height:.48rem;border-radius:50%;'+"
                      "'background:#5b8dee;flex-shrink:0';"
                  "const initialLabel=isAnswered?'answered':'starting…';"
                  "chip.innerHTML="
                    "'<span class=\"d __easyaiDot\" style=\"'+dotStyle+'\"></span>"
                    "<span class=\"l\">'+initialLabel+'</span>';"
                  "return chip;"
                "};"
                // No-op kept as a stub for backwards-compatibility with the
                // attach/update sites — the chip's palette is now handled
                // by Tailwind's text-muted-foreground / text-foreground
                // utilities, which read CSS variables that switch with
                // the active theme.  Removing the calls entirely would
                // also work; keeping the stub lets the surrounding code
                // stay structurally identical.
                "const applyBarPalette=(_chip)=>{};"
                // attachChip(msg): place the chip in the best available
                // anchor.  During streaming the bundle has NOT yet rendered
                // the copy/edit/fork/delete row, so findActionRow returns
                // null — in that case overlay the chip on the assistant
                // bubble using position:absolute (top-right, anchored to
                // msg whose position becomes relative).  That guarantees
                // visibility regardless of the bubble's internal layout.
                // Once a real action row appears, we migrate the chip
                // into it inline and drop the absolute positioning.
                "const attachChip=(msg)=>{"
                  "let chip=msg[CHIP_MARK];"
                  "const row=findActionRow(msg);"
                  "if(chip&&document.contains(chip)){"
                    "if(row&&chip.dataset.fallback==='1'&&!row.contains(chip)){"
                      "chip.style.position='static';"
                      "chip.style.top='';chip.style.right='';"
                      "chip.style.margin='0 0 0 .5rem';"
                      "row.appendChild(chip);"
                      "delete chip.dataset.fallback;"
                    "}"
                    "applyBarPalette(chip);"
                    "return;"
                  "}"
                  // The bundle renders the action toolbar (copy/edit/etc)
                  // ONLY after a message finishes streaming.  When we see
                  // a row at chip-creation time the message is already
                  // past — render the chip in the passive 'answered'
                  // state (empty circle + 'answered' label) so it doesn't
                  // misleadingly say 'starting…'.  Active streams keep
                  // the original 'starting…' default until the SSE handler
                  // updates it.
                  "chip=buildChip(row?'answered':null);"
                  "if(row){"
                    "chip.style.margin='0 0 0 .5rem';"
                    "row.appendChild(chip);"
                  "}else{"
                    "chip.dataset.fallback='1';"
                    // Anchor the bubble for absolute positioning if it
                    // doesn't already have a positioning context.
                    "const cs=getComputedStyle(msg);"
                    "if(cs.position==='static')msg.style.position='relative';"
                    "chip.style.position='absolute';"
                    "chip.style.top='.4rem';"
                    "chip.style.right='.6rem';"
                    "chip.style.margin='0';"
                    "chip.style.zIndex='10';"
                    "msg.appendChild(chip);"
                  "}"
                  "msg[CHIP_MARK]=chip;"
                  "applyBarPalette(chip);"
                "};"
                // Inline tool-log entries are emitted by the server as
                // markdown italics ("\n*🔧 web*\n") so they end up
                // as <em> nodes inside the assistant's content body.  We
                // render them at the same size as the per-message chip
                // (.5rem mono, dim) — they're status traces, not prose.
                "if(!document.getElementById('__easyaiToolLogStyle')){"
                  "const ts=document.createElement('style');"
                  "ts.id='__easyaiToolLogStyle';"
                  "ts.textContent="
                    "'.__ea-tlog{font-size:.5rem;opacity:.65;"
                      "font-family:ui-monospace,SFMono-Regular,Menlo,monospace;"
                      "font-style:normal;font-weight:400;letter-spacing:0}"
                    ".__ea-tlog-p{margin:.15rem 0;line-height:1.2;"
                      "font-size:.5rem;opacity:.7}';"
                  "(document.head||document.documentElement).appendChild(ts);"
                "}"
                "const TLOG_RE=/^\\s*[\\u{1F527}\\u{274C}]\\s/u;"
                "const shrinkToolLogs=(root)=>{"
                  "if(!root||!root.querySelectorAll)return;"
                  "const ems=root.querySelectorAll('em:not(.__ea-tlog),"
                    "strong:not(.__ea-tlog)');"
                  "ems.forEach(el=>{"
                    "const t=el.textContent||'';"
                    "if(TLOG_RE.test(t)){"
                      "el.classList.add('__ea-tlog');"
                      "const p=el.closest('p,li,div');"
                      "if(p)p.classList.add('__ea-tlog-p');"
                    "}"
                  "});"
                "};"
                "const scanMessages=()=>{"
                  "const msgs=document.querySelectorAll("
                    "'[aria-label=\"Assistant message with actions\"]'"
                  ");"
                  "msgs.forEach(m=>{attachChip(m);shrinkToolLogs(m);});"
                "};"
                "const mo=new MutationObserver(scanMessages);"
                "if(document.body)mo.observe(document.body,{childList:true,subtree:true});"
                "document.addEventListener('DOMContentLoaded',()=>{"
                  "scanMessages();"
                  "mo.observe(document.body,{childList:true,subtree:true});"
                "});"

                // Public helper: drives the inline chip on the LATEST
                // assistant message (the one currently streaming or just
                // finished).  After completion the chip stays visible with
                // the stats text, so the user always sees per-response
                // performance numbers next to copy/edit/fork/delete.
                "window.__easyaiSetStatus=(state,extra)=>{"
                  "scanMessages();"
                  "const all=document.querySelectorAll("
                    "'[aria-label=\"Assistant message with actions\"]'"
                  ");"
                  "if(!all.length)return;"
                  "const last=all[all.length-1];"
                  "const chip=last[CHIP_MARK];"
                  "if(!chip)return;"
                  "if(state==='idle'){"
                    "chip.style.display='none';"
                    "return;"
                  "}"
                  "chip.style.display='inline-flex';"
                  "applyBarPalette(chip);"
                  "const dot=chip.querySelector('.d');"
                  "const lab=chip.querySelector('.l');"
                  "if(dot){"
                    "dot.style.background=STATE_DOT[state]||STATE_DOT.idle;"
                    // Pulse while we're actively working; stop on
                    // terminal states so the user sees the difference at
                    // a glance.
                    "const active=(state==='thinking'||state==='answering'||state==='fetching'||state==='processing');"
                    "if(active)dot.classList.add('pulse');"
                    "else dot.classList.remove('pulse');"
                  "}"
                  "if(lab){"
                    "let txt=state;"
                    "if(state==='complete'&&typeof extra==='string'&&extra){"
                      "txt=extra;"
                    "}else if(state==='fetching'&&typeof extra==='string'&&extra){"
                      "txt='fetching·'+extra;"
                    "}else if(state==='processing'&&typeof extra==='string'&&extra){"
                      "txt='prompt·'+extra;"
                    "}else if(extra&&typeof extra==='object'&&extra.live){"
                      // Live metrics: state + tok count + elapsed + t/s.
                      "const sec=(extra.elapsedMs/1000).toFixed(1);"
                      "const tps=extra.tps?extra.tps.toFixed(1):'0.0';"
                      "txt=state+'·'+extra.tokens+'tok·'+sec+'s·'+tps+'t/s';"
                    "}else if(state==='fetching'&&extra&&extra.tokens){"
                      "txt='fetching';"
                    "}"
                    "lab.textContent=txt;"
                  "}"
                "};"

                // --- overview values painted inside the bundle's
                //     .chat-processing-info-detail element -------------------
                // Fetch n_ctx once at boot so we can render Context: X/Y%
                // even before the first reply.  Then monitorSSE feeds us
                // timings via __easyaiPushTimings, and we paint into the
                // .__easyai-ovr span we inject (and re-inject) inside the
                // bundle's processing-info element.  That element is already
                // anchored correctly by the SvelteKit layout, so we don't
                // own a custom pill anymore.
                "let lastTimings=null;"
                "let lastSessionUsed=0;"   // sticky cumulative ctx used
                "window.__easyaiNCtx=0;"
                "fetch('/props').then(r=>r.json()).then(j=>{"
                  "window.__easyaiNCtx="
                    "(j.default_generation_settings&&j.default_generation_settings.n_ctx)||0;"
                  "renderOverview();"
                "}).catch(()=>{});"
                // Soft + hard thresholds for the context-window guard.
                // Soft (>=85%): bar turns amber as a warning.
                // Hard (>=98%): bar turns red AND we disable the textarea
                //               + Send button so the user can't fire a
                //               request that would overflow.  The user
                //               clears the conversation (or shrinks ctx)
                //               via the bundle's own UI to reset.
                "const CTX_SOFT=0.85,CTX_HARD=0.98;"
                "const setInputLocked=(locked,reason)=>{"
                  "const ta=document.querySelector('textarea');"
                  "if(!ta)return;"
                  "if(locked){"
                    "window.__easyaiCtxFull=true;"
                    "if(ta.dataset.easyaiLockedReason===reason)return;"
                    "ta.dataset.easyaiLockedReason=reason;"
                    "ta.dataset.easyaiPrevPlaceholder=ta.placeholder||'';"
                    "ta.dataset.easyaiPrevBorder=ta.style.border||'';"
                    "ta.dataset.easyaiPrevOpacity=ta.style.opacity||'';"
                    "ta.placeholder=reason;"
                    "ta.disabled=true;"
                    "ta.style.border='1px solid #f85149';"
                    "ta.style.opacity='.55';"
                    "const form=ta.closest('form');"
                    "if(form){"
                      "form.querySelectorAll('button').forEach(b=>{"
                        "if(b.type==='submit'||/send|enviar/i.test(b.getAttribute('aria-label')||''))"
                          "b.disabled=true;"
                      "});"
                    "}"
                  "}else if(ta.dataset.easyaiLockedReason){"
                    "window.__easyaiCtxFull=false;"
                    "ta.placeholder=ta.dataset.easyaiPrevPlaceholder||'';"
                    "ta.style.border=ta.dataset.easyaiPrevBorder||'';"
                    "ta.style.opacity=ta.dataset.easyaiPrevOpacity||'';"
                    "delete ta.dataset.easyaiLockedReason;"
                    "delete ta.dataset.easyaiPrevPlaceholder;"
                    "delete ta.dataset.easyaiPrevBorder;"
                    "delete ta.dataset.easyaiPrevOpacity;"
                    "ta.disabled=false;"
                    "const form=ta.closest('form');"
                    "if(form)form.querySelectorAll('button[disabled]').forEach(b=>{b.disabled=false;});"
                  "}"
                "};"
                // Observer tracks the bundle's `.chat-processing-info-detail`:
                // when Svelte re-renders or replaces it, our content gets
                // overwritten — the observer fires renderOverview again so we
                // re-inject.  An equality check prevents an infinite ping-pong
                // because re-setting the same innerHTML triggers the observer,
                // which then no-ops on the next call.
                "let __eaiOvrTarget=null,__eaiOvrObs=null;"
                "function __eaiBindOvr(t){"
                  "if(t===__eaiOvrTarget)return;"
                  "if(__eaiOvrObs){__eaiOvrObs.disconnect();__eaiOvrObs=null;}"
                  "__eaiOvrTarget=t;"
                  "if(!t)return;"
                  "__eaiOvrObs=new MutationObserver(()=>{renderOverview();});"
                  "__eaiOvrObs.observe(t,{childList:true,characterData:true,subtree:true});"
                "}"
                "function renderOverview(){"
                  "const t=lastTimings;"
                  // Cumulative session usage:
                  //   Prefer ctx_used (post-turn KV fill from the engine).
                  //   Fall back to cache_n + prompt_n + predicted_n only
                  //   if the server didn't send ctx_used (older build).
                  //   During live streaming, show lastSessionUsed PLUS
                  //   the live token count so the user sees the bar move
                  //   while generation runs — snaps to the real value
                  //   when the final timings arrive at finish_reason.
                  "let used=lastSessionUsed;"
                  "if(t&&typeof t.ctx_used==='number'&&t.ctx_used>0){"
                    "used=t.ctx_used;lastSessionUsed=used;"
                  "}else if(t&&!t.live){"
                    "const u=(t.cache_n||0)+(t.prompt_n||0)+(t.predicted_n||0);"
                    "if(u>used){used=u;lastSessionUsed=used;}"
                  "}else if(t&&t.live&&typeof t.predicted_n==='number'){"
                    // Live overlay: previous-session base + this turn's
                    // running token count.  Doesn't account for the new
                    // turn's prompt_n (we won't know that until finish),
                    // so we visually under-report by prompt_n during
                    // streaming.  Acceptable trade-off for live motion.
                    "used=lastSessionUsed+t.predicted_n;"
                  "}"
                  "const total=(t&&t.n_ctx)||window.__easyaiNCtx||0;"
                  "if(total&&!window.__easyaiNCtx)window.__easyaiNCtx=total;"
                  "const pct=total?used/total:0;"
                  "const ctxText=total?"
                    "(used.toLocaleString()+' / '+total.toLocaleString()+"
                      "' ('+Math.round(pct*100)+'%)'):"
                    "used.toLocaleString();"
                  // ctx threshold colors are inline-only when crossed —
                  // otherwise we leave the value uncolored so the bundle's
                  // text-foreground class (set on the wrapping span below)
                  // drives the color, which switches automatically with
                  // the light/dark theme.
                  "const ctxColor=pct>=CTX_HARD?'#f85149':"
                    "pct>=CTX_SOFT?'#d29922':'';"
                  // Lock input if we've crossed the hard threshold.
                  "if(total){"
                    "if(pct>=CTX_HARD){"
                      "setInputLocked(true,'context full ('+Math.round(pct*100)+"
                        "'%) — clear the chat to continue');"
                    "}else{"
                      "setInputLocked(false);"
                    "}"
                  "}"
                  "let lastText='—';"
                  "if(t&&t.predicted_n&&t.predicted_ms){"
                    "const tps=(t.predicted_n/(t.predicted_ms/1000));"
                    "lastText=t.predicted_n+' tok · '+(t.predicted_ms/1000).toFixed(1)+"
                      "'s · '+tps.toFixed(1)+' t/s';"
                  "}"
                  // Inject directly inside the bundle's processing-info
                  // detail.  We render with Tailwind utility classes the
                  // bundle already ships:
                  //   * `text-xs`            — same size as Output / t/s
                  //   * `text-muted-foreground` — labels + separator
                  //                              (dimmed in both themes)
                  //   * `text-foreground`    — values (theme-aware fg)
                  // No font-family override: the bundle's sans stack
                  // inherits, matching the Output text exactly.
                  // Threshold colors (ctxColor) are applied inline only
                  // when crossed; otherwise the value uses text-foreground
                  // and tracks the active light/dark theme automatically.
                  "const target=document.querySelector('.chat-processing-info-detail');"
                  "if(target!==__eaiOvrTarget)__eaiBindOvr(target);"
                  "if(!target)return;"
                  "const ctxValStyle=ctxColor?"
                    "' style=\"color:'+ctxColor+'\"':'';"
                  "const html="
                    "'<span class=\"text-xs inline-flex items-center gap-1.5\">'+"
                      "'<span class=\"text-muted-foreground\">ctx</span>'+"
                      "'<span class=\"text-foreground\"'+ctxValStyle+'>'+ctxText+'</span>'+"
                      "'<span class=\"text-muted-foreground opacity-60\">·</span>'+"
                      "'<span class=\"text-muted-foreground\">last</span>'+"
                      "'<span class=\"text-foreground\">'+lastText+'</span>'+"
                    "'</span>';"
                  "if(target.innerHTML!==html)target.innerHTML=html;"
                "};"
                "window.__easyaiPushTimings=(t)=>{"
                  "if(t)lastTimings=t;"
                  "renderOverview();"
                "};"

                // Reached the end of block5 — if you see this in the
                // console you know the metrics/tone/chip IIFE parsed and
                // executed completely.  No log == SyntaxError further up.
                "console.log('[easyai-inject] block5 OK — metrics/tone/chip live');"
              "})();</script>";

            // ----- CSS hiding for features we don't support ------------------
            // The Svelte build minifies class names but keeps human-readable
            // ARIA labels, data-testids, and a few stable kebab-case classnames
            // (e.g. "mcp-*", "model-card-*").  We hide aggressively by name
            // patterns; if upstream renames things we'll need to revisit.
            inj <<
              "<style>"
                // MCP — explicit user request.
                "[class*=\"mcp\" i],[class*=\"Mcp\"],"
                "[data-testid*=\"mcp\" i],"
                "a[href*=\"mcp\" i],button[aria-label*=\"mcp\" i],"
                // Model load/unload (we serve a single fixed model).
                "[data-testid*=\"model-load\" i],[data-testid*=\"model-unload\" i],"
                "[aria-label*=\"load model\" i],[aria-label*=\"unload model\" i],"
                "[aria-label*=\"manage models\" i],"
                // OAuth / login (we use --api-key Bearer auth).
                "[data-testid*=\"oauth\" i],[data-testid*=\"login\" i],"
                "[data-testid*=\"authorize\" i],[aria-label*=\"sign in\" i],"
                // Pyodide / Python interpreter.
                "[class*=\"pyodide\" i],[data-testid*=\"pyodide\" i],"
                "[class*=\"python\" i][role],[aria-label*=\"python\" i],"
                // Slot usage display (we have one slot).
                "[data-testid*=\"slot\" i]:not([data-testid*=\"slot-machine\"]),"
                "[aria-label*=\"slot usage\" i],"
                // Audio recording / microphone / camera.
                "[aria-label*=\"microphone\" i],[aria-label*=\"record audio\" i],"
                "[aria-label*=\"camera\" i],[data-testid*=\"audio-record\" i]"
                "{display:none !important;visibility:hidden !important;"
                " width:0 !important;height:0 !important;overflow:hidden !important;}"

                // Style thinking blocks: smaller, dimmer, monospace.  Catch
                // both the bundle's own thinking renderer (whichever class
                // it ends up minified to) and a literal &lt;think&gt; tag if
                // it ever survives sanitisation.
                " details[data-thinking],details.thinking,"
                " [data-testid*=\"thinking\" i],[class*=\"reasoning\" i],"
                " [class*=\"think-block\" i]{"
                "  font-size:.78rem !important;color:#8b949e !important;"
                "  font-family:ui-monospace,SFMono-Regular,Menlo,monospace !important;"
                "  border-left:2px solid #b97df3 !important;"
                "  padding-left:.6rem !important;margin:.4rem 0 !important;"
                "  opacity:.85;"
                " }"

                // Shrink the assistant message body to a comfortable read
                // size, closer to our chip / tone-badge font.  We target
                // the prose-style markdown wrapper inside the assistant
                // bubble while leaving the user message and code blocks
                // alone (code blocks have their own monospace size that
                // we don't want to mess with).
                " [aria-label=\"Assistant message with actions\"]{"
                "  font-size:.85rem !important;"
                "  line-height:1.55 !important;"
                " }"
                " [aria-label=\"Assistant message with actions\"] p,"
                " [aria-label=\"Assistant message with actions\"] li,"
                " [aria-label=\"Assistant message with actions\"] blockquote{"
                "  font-size:.85rem !important;line-height:1.55 !important;"
                " }"
                " [aria-label=\"Assistant message with actions\"] h1{"
                "  font-size:1.05rem !important;"
                " }"
                " [aria-label=\"Assistant message with actions\"] h2{"
                "  font-size:.98rem !important;"
                " }"
                " [aria-label=\"Assistant message with actions\"] h3,"
                " [aria-label=\"Assistant message with actions\"] h4{"
                "  font-size:.92rem !important;"
                " }"
                // Don't shrink code/pre — they have their own visual rules.
                " [aria-label=\"Assistant message with actions\"] code,"
                " [aria-label=\"Assistant message with actions\"] pre{"
                "  font-size:.78rem !important;"
                " }"
              "</style>";

            // Always point the browser at our /favicon route — that
            // route serves either --webui-icon (when the operator gave
            // one) or the embedded AI-brain.svg as the default.  We also
            // keep an observer running that re-asserts our link if the
            // Svelte app later mounts its own (Svelte often replaces
            // <head> children on hydration).
            inj << "<link id=\"__easyaiFavicon\" rel=\"icon\" href=\"/favicon\">";
            inj << "<script>(()=>{"
                << "console.log('[easyai-inject] block6 favicon-keeper');"
                << "const want='/favicon';"
                << "const ours=()=>document.getElementById('__easyaiFavicon');"
                << "const nuke=()=>{"
                  << "document.querySelectorAll('link[rel~=\"icon\"]').forEach(l=>{"
                    << "if(l.id!=='__easyaiFavicon')l.remove();"
                  << "});"
                  << "let our=ours();"
                  << "if(!our){"
                    << "our=document.createElement('link');"
                    << "our.id='__easyaiFavicon';"
                    << "our.rel='icon';"
                    << "our.href=want;"
                    << "document.head&&document.head.appendChild(our);"
                  << "}else if(our.getAttribute('href')!==want){"
                    << "our.setAttribute('href',want);"
                  << "}"
                << "};"
                << "nuke();"
                << "if(document.head){"
                  << "new MutationObserver(nuke).observe("
                    << "document.head,{childList:true,subtree:false,attributes:true,"
                    << "attributeFilter:['href','rel']});"
                << "}"
                << "document.addEventListener('DOMContentLoaded',()=>{"
                  << "nuke();"
                  << "if(document.head){"
                    << "new MutationObserver(nuke).observe("
                      << "document.head,{childList:true,subtree:false,attributes:true,"
                      << "attributeFilter:['href','rel']});"
                  << "}"
                << "});"
                << "})();</script>";

            // ----- block7: reasoning panel — flatten all internal
            //               scrolling so it grows with content -------
            // Previous attempt pinned the inner scroll to the bottom on
            // every mutation, but lost the race to the bundle's Svelte
            // reactive update — each reasoning_content delta replaces
            // the inner element, so a captured `scrollEl` reference is
            // stale by the time the pin fires.  User saw the inner
            // scrollbar parked partway up while content streamed below.
            //
            // New strategy: walk the entire reasoning-panel subtree and
            // force `overflow:visible` + `max-height:none` on every
            // descendant.  The panel grows to fit; the chat conversation
            // list (which already auto-scrolls to bottom on new content)
            // becomes the single scrollbar that always shows the latest
            // streamed line.  No inner pin, no race, one scrollbar.
            //
            // We re-flatten on every subtree mutation (childList +
            // attributes on style/class) so the bundle can't sneak the
            // overflow back in via a Svelte rerender that resets our
            // inline styles.  Scoped to reasoning panels only — by the
            // trigger sibling's text matching `^Reasoning` — so other
            // collapsibles (tools, citations, future panels) keep
            // their default behaviour.
            inj <<
              "<script>(()=>{"
                "console.log('[easyai-inject] block7 reasoning-flatten');"
                "const isReasoningPanel=(panel)=>{"
                  // Match by the trigger sibling/ancestor's text. Walk
                  // up a few levels because the trigger may live in a
                  // grand-parent wrapper depending on bundle layout.
                  "let p=panel;"
                  "for(let i=0;i<5&&p;i++){"
                    "const tr=p.querySelector"
                      "?p.querySelector('[data-slot=\"collapsible-trigger\"]')"
                      ":null;"
                    "const t=tr?(tr.innerText||tr.textContent||'').trim():'';"
                    "if(/^Reasoning/i.test(t))return true;"
                    "p=p.parentElement;"
                  "}"
                  "return false;"
                "};"
                "const flatten=(panel)=>{"
                  // Apply to the panel itself + every descendant.
                  // overflow visible: no internal scrollbars.
                  // max-height none: no height clamp; panel grows
                  // naturally with content.
                  "const all=[panel,...panel.querySelectorAll('*')];"
                  "for(const el of all){"
                    "const cs=getComputedStyle(el);"
                    "if(cs.overflowY==='auto'||cs.overflowY==='scroll'){"
                      "el.style.setProperty('overflow-y','visible','important');"
                    "}"
                    "if(cs.overflowX==='auto'||cs.overflowX==='scroll'){"
                      "el.style.setProperty('overflow-x','visible','important');"
                    "}"
                    "const mh=cs.maxHeight;"
                    "if(mh&&mh!=='none'&&mh!=='0px'){"
                      "el.style.setProperty('max-height','none','important');"
                    "}"
                  "}"
                "};"
                "const seen=new WeakSet();"
                "const handle=(panel)=>{"
                  "if(seen.has(panel))return;"
                  "if(!isReasoningPanel(panel))return;"
                  "seen.add(panel);"
                  "flatten(panel);"
                  // Re-flatten on every reactive update — the bundle
                  // may rerender inner blocks (per-token reasoning
                  // delta) and reset our inline styles to its hashed
                  // class CSS.  Watching style/class attributes too
                  // catches rerenders that just reattach classes.
                  "new MutationObserver(()=>flatten(panel)).observe(panel,{"
                    "childList:true,subtree:true,attributes:true,"
                    "attributeFilter:['style','class']"
                  "});"
                "};"
                "const sweep=()=>{"
                  "document.querySelectorAll("
                    "'[data-slot=\"collapsible-content\"]'"
                  ").forEach(handle);"
                "};"
                "document.addEventListener('DOMContentLoaded',()=>{"
                  "sweep();"
                  "new MutationObserver(sweep).observe(document.body,"
                    "{childList:true,subtree:true});"
                "});"
                "sweep();"
              "})();</script>";

            // Splice immediately after <head>.
            const std::string head_open = "<head>";
            size_t pos = html.find(head_open);
            if (pos != std::string::npos) {
                html.insert(pos + head_open.size(), inj.str());
            }
            ctx->webui_html = std::move(html);

            // ----- bundle.js text-substitute (real, not just CSS) ------
            // The Svelte bundle has hard-coded brand strings inside string
            // literals — DOM scrubbers can't catch the welcome <h1> or the
            // input placeholder.  Patch them now, once, into a buffer
            // we'll serve from /bundle.js.
            std::string js(reinterpret_cast<const char *>(bundle_js),
                            bundle_js_len);

            // Page-title surface (already pinned by document.title interceptor,
            // but we also clean these up so view-source / window.title is correct).
            js = str_replace_all(js, "llama.cpp - AI Chat Interface", title);
            js = str_replace_all(js,
                "Initializing connection to llama.cpp server...",
                "Initializing connection to " + title + " server…");
            js = str_replace_all(js, "} - llama.cpp", "} - " + title);

            // Sidebar / topbar brand markup.  We prepend an <img> pointing
            // at /favicon (which serves either the operator's --webui-icon
            // or the bundled AI-brain.svg) so the brand reads as
            // "<icon> EasyAi" instead of just text.  The styling keeps the
            // icon vertically aligned with the title text and sized to the
            // h1's line-height; brand h1 in the bundle is small (about 1rem),
            // so 1.575em (≈50% larger than the text) gives the logo clear
            // visual prominence.  No external image lookup, no CDN.
            const std::string brand_html =
                "><img src=\"/favicon\" alt=\"\" style=\"height:1.575em;"
                "width:1.575em;vertical-align:-.18em;margin-right:.35em;"
                "border-radius:4px;background:transparent;display:inline-block\">"
                + title + "</h1>";
            js = str_replace_all(js, ">llama.cpp</h1>", brand_html);

            // Input placeholder.
            if (!args.webui_placeholder.empty()) {
                js = str_replace_all(js, "Type a message...", args.webui_placeholder);
                js = str_replace_all(js, "Type a message",    args.webui_placeholder);
            }

            ctx->webui_bundle_js = std::move(js);
        } else
#endif
        {
            // Minimal inline webui — substitute the title placeholder we
            // baked into the kWebUI string.
            ctx->webui_html = str_replace_all(kWebUI, "__EASYAI_TITLE__",
                                              html_escape(title));
        }

        if (!args.webui_icon.empty()) {
            ctx->webui_icon = read_binary_file(args.webui_icon);
            if (ctx->webui_icon.empty()) {
                std::fprintf(stderr,
                    "[easyai-server] WARN: failed to read --webui-icon '%s'\n",
                    args.webui_icon.c_str());
            } else {
                ctx->webui_icon_mime = mime_for_icon(args.webui_icon);
                std::fprintf(stderr,
                    "[easyai-server] webui icon loaded: %s (%zu bytes, %s)\n",
                    args.webui_icon.c_str(), ctx->webui_icon.size(),
                    ctx->webui_icon_mime.c_str());
            }
        }
    }

    if (args.parallel > 1) {
        std::fprintf(stderr,
            "[easyai-server] note: --parallel %d requested but the engine is "
            "single-context; requests are still serialised.\n", args.parallel);
    }

    // -------- configure & load engine ------------------------------------
    ctx->engine.model      (args.model_path)
               .context    (args.n_ctx)
               .gpu_layers (args.ngl)
               .system     (default_system)
               .verbose    (args.verbose);
    if (args.n_threads > 0)     ctx->engine.threads(args.n_threads);
    if (args.threads_batch > 0) ctx->engine.threads_batch(args.threads_batch);
    if (args.n_batch   > 0)  ctx->engine.batch  (args.n_batch);
    if (args.seed      > 0)  ctx->engine.seed   (args.seed);
    if (args.max_tokens >= 0) ctx->engine.max_tokens(args.max_tokens);
    if (args.repeat_penalty > 0) ctx->engine.repeat_penalty(args.repeat_penalty);
    if (args.presence_penalty > -2.0f) ctx->engine.presence_penalty(args.presence_penalty);
    if (!args.cache_type_k.empty()) ctx->engine.cache_type_k(args.cache_type_k);
    if (!args.cache_type_v.empty()) ctx->engine.cache_type_v(args.cache_type_v);
    if (args.no_kv_offload)  ctx->engine.no_kv_offload(true);
    if (args.kv_unified)     ctx->engine.kv_unified(true);
    if (!args.spec_type.empty())         ctx->engine.spec_type(args.spec_type);
    if (args.spec_draft_n_max > 0)     ctx->engine.spec_draft_n_max(args.spec_draft_n_max);
    if (!args.spec_draft_model.empty()) ctx->engine.spec_draft_model(args.spec_draft_model);
    if (!args.chat_template_file.empty()) ctx->engine.chat_template_file(args.chat_template_file);
    if (!args.reasoning_format.empty()) {
        ctx->engine.reasoning_format(args.reasoning_format);
        // The setter catches the unknown-name throw from common_reasoning_
        // format_from_name and records last_error. Surface it here so a typo
        // like --reasoning-format deepseak aborts startup instead of
        // silently falling back to the default.
        if (!ctx->engine.last_error().empty()) {
            std::fprintf(stderr, "[easyai-server] %s\n"
                                 "                accepted values: none, auto, deepseek, deepseek-legacy\n",
                         ctx->engine.last_error().c_str());
            return 1;
        }
    }
    if (args.flash_attn)     ctx->engine.flash_attn(true);
    if (args.mlock)          ctx->engine.use_mlock(true);
    if (args.no_mmap)        ctx->engine.use_mmap(false);
    if (!args.numa.empty())  ctx->engine.numa(args.numa);
    if (!args.reasoning)     ctx->engine.enable_thinking(false);
    for (const auto & ov : args.kv_overrides) ctx->engine.add_kv_override(ov);
    for (const auto & t : ctx->default_tools) ctx->engine.add_tool(t);
    ctx->engine.set_sampling(ctx->def_temperature, ctx->def_top_p,
                             ctx->def_top_k, ctx->def_min_p);

    if (!ctx->engine.load()) {
        std::fprintf(stderr, "[easyai-server] load failed: %s\n",
                     ctx->engine.last_error().c_str());
        return 1;
    }

    std::fprintf(stderr,
        "[easyai-server] %s loaded\n"
        "                backend=%s  ctx=%d  tools=%zu  preset=%s\n"
        "                listening on http://%s:%d  (webui at /)\n"
        "                inject_datetime=%s  cutoff=%s  verbose=%s\n"
        "                max_tool_hops=99999  retry_on_incomplete=ON\n"
        "                http_timeout=%ds (read/write)  http_retries=%d\n",
        ctx->model_id.c_str(), ctx->engine.backend_summary().c_str(),
        ctx->engine.n_ctx(), ctx->default_tools.size(),
        ctx->default_preset.name.c_str(),
        args.host.c_str(), args.port,
        ctx->inject_datetime ? "ON" : "OFF",
        ctx->knowledge_cutoff.c_str(),
        args.verbose ? "ON" : "OFF",
        args.http_timeout > 0 ? args.http_timeout : 600,
        args.http_retries < 0 ? 0 : args.http_retries);
    if (args.verbose) {
        std::fprintf(stderr,
            "[easyai-server] VERBOSE: per-request POST line + per-hop "
            "generate_one/chat_continue dumps + thought-only retry trace + "
            "HTTP \xe2\x86\x92 arrival / \xe2\x86\x90 completion lines per "
            "request will appear in this stream. (The periodic METRICS line "
            "ships unconditionally — see --metrics-interval.)\n");
    }

    // -------- http server -------------------------------------------------
    httplib::Server svr;
    svr.set_payload_max_length(args.max_body);
    // Apply the configured HTTP timeout to BOTH directions.  Default is
    // 600 s (10 min) so long thinking turns don't trip a mid-stream cut
    // when the model goes quiet for minutes between visible tokens —
    // see ServerArgs::http_timeout.  Logged below in the startup
    // banner so an operator reading journalctl knows the value in
    // effect without grepping --verbose output.
    //
    // Idle-keep-alive timeout: pinned to AT LEAST 1 hour so the
    // TCP socket survives between agentic-session hops.  cpp-httplib's
    // 5 s default would kill the connection between any two POSTs
    // separated by a tool dispatch (web fetch, bash compile, fs grep)
    // — server-side TIME_WAIT pile-up that undoes the client-side
    // persistent-Client win from commit 841dd47.  Floor of 3600 s
    // applies even if http_timeout is set lower (e.g. an operator who
    // wants aggressive slow-loris hardening on the request payload
    // still gets generous idle re-use between requests).  When
    // http_timeout > 1 h (the installer default is 86400 s = 24 h),
    // the idle timeout follows it.
    {
        const int t = args.http_timeout > 0 ? args.http_timeout : 600;
        svr.set_read_timeout (t);
        svr.set_write_timeout(t);
        constexpr int kMinKeepAliveSec = 3600;   // 1 hour minimum
        const int ka = t > kMinKeepAliveSec ? t : kMinKeepAliveSec;
        svr.set_keep_alive_timeout(ka);
    }

    // CORS — permissive to be friendly with browser-based clients. Tighten if
    // exposing on a public network.
    svr.set_default_headers({
        {"Access-Control-Allow-Origin",  "*"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
    });
    svr.Options(R"(.*)", [](const httplib::Request &, httplib::Response & res) {
        res.status = 204;
    });

    // Routes — every handler captures `ctx_ref` by reference.  Lifetime is
    // safe because main() does not return until svr.listen() exits.
    auto & ctx_ref = *ctx;

    // ---- verbose connection log -------------------------------------------
    // In --verbose mode, log a `→` line on request arrival and a `←` line on
    // completion.  The `←` line carries per-request stats (status, duration,
    // bytes) plus running totals so an operator tailing journalctl can see
    // load + per-request behaviour without enabling /metrics.  Streaming
    // responses (chat completions) write bytes through chunked transfer and
    // bypass res.body — those show `out=streamed` instead of a byte count.
    //
    // Per-request start time lives in a thread_local; each cpp-httplib
    // worker thread serves one request at a time, so this correlates the
    // pre-routing arrival with the post-response logger callback without
    // a per-socket map.
    if (args.verbose) {
        struct ReqTiming {
            std::chrono::steady_clock::time_point start;
        };
        static thread_local ReqTiming t_req{};

        svr.set_pre_routing_handler(
            [&ctx_ref](const httplib::Request & req, httplib::Response &) {
                t_req.start = std::chrono::steady_clock::now();
                ctx_ref.n_in_flight.fetch_add(1, std::memory_order_relaxed);
                ctx_ref.n_bytes_in.fetch_add(req.body.size(),
                                             std::memory_order_relaxed);
                std::fprintf(stderr,
                    "[easyai-server] → %s %s  from=%s:%d  body=%zuB  in_flight=%llu\n",
                    req.method.c_str(),
                    req.path.c_str(),
                    req.remote_addr.c_str(),
                    req.remote_port,
                    req.body.size(),
                    (unsigned long long) ctx_ref.n_in_flight.load(
                        std::memory_order_relaxed));
                std::fflush(stderr);
                return httplib::Server::HandlerResponse::Unhandled;
            });

        svr.set_logger(
            [&ctx_ref](const httplib::Request & req, const httplib::Response & res) {
                const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t_req.start).count();
                const std::size_t out_bytes = res.body.size();
                ctx_ref.n_bytes_out.fetch_add(out_bytes, std::memory_order_relaxed);
                const auto in_flight_after = ctx_ref.n_in_flight.fetch_sub(
                    1, std::memory_order_relaxed) - 1;
                char out_buf[32];
                if (out_bytes == 0) {
                    std::snprintf(out_buf, sizeof(out_buf), "streamed");
                } else {
                    std::snprintf(out_buf, sizeof(out_buf), "%zuB", out_bytes);
                }
                std::fprintf(stderr,
                    "[easyai-server] ← %s %s  status=%d  dur=%lldms  out=%s  "
                    "totals: req=%llu err=%llu tools=%llu in_flight=%llu  "
                    "bytes: in=%lluB out=%lluB\n",
                    req.method.c_str(),
                    req.path.c_str(),
                    res.status,
                    (long long) dur_ms,
                    out_buf,
                    (unsigned long long) ctx_ref.n_requests.load(std::memory_order_relaxed),
                    (unsigned long long) ctx_ref.n_errors.load(std::memory_order_relaxed),
                    (unsigned long long) ctx_ref.n_tool_calls.load(std::memory_order_relaxed),
                    (unsigned long long) in_flight_after,
                    (unsigned long long) ctx_ref.n_bytes_in.load(std::memory_order_relaxed),
                    (unsigned long long) ctx_ref.n_bytes_out.load(std::memory_order_relaxed));
                std::fflush(stderr);
            });
    }

    // ---- periodic metrics ticker (always on) ----------------------------
    // Wakes every args.metrics_interval seconds and emits one METRICS line
    // covering CPU/GPU/mem/load/iowait/net.  Joined cleanly via
    // g_metrics_stop + g_metrics_cv so SIGINT/SIGTERM doesn't leave a
    // dangling worker.
    //
    // Runs unconditionally (not just under --verbose) — operations folks
    // need this telemetry in journalctl regardless of log noise level.
    // metrics_interval=0 disables; default is 300s (5 minutes), low-
    // overhead enough to leave on permanently in production.
    std::thread metrics_thread;
    if (args.metrics_interval > 0) {
        const int interval_s = args.metrics_interval;
        metrics_thread = std::thread([&ctx_ref, interval_s]() {
            using namespace metrics;
            const auto t_start = std::chrono::steady_clock::now();
            CpuTicks prev{};
            bool have_prev = read_proc_stat(prev);
            while (!g_metrics_stop.load(std::memory_order_relaxed)) {
                std::unique_lock<std::mutex> lk(g_metrics_mu);
                if (g_metrics_cv.wait_for(
                        lk, std::chrono::seconds(interval_s),
                        []{ return g_metrics_stop.load(std::memory_order_relaxed); })) {
                    break;
                }
                lk.unlock();

                // CPU + iowait deltas
                double cpu_pct = -1.0, iowait_pct = -1.0;
                CpuTicks cur{};
                if (read_proc_stat(cur) && have_prev) {
                    const auto dt = cur.total() > prev.total() ? cur.total() - prev.total() : 0;
                    if (dt > 0) {
                        const auto db = cur.busy() > prev.busy() ? cur.busy() - prev.busy() : 0;
                        const auto dw = cur.iowait > prev.iowait ? cur.iowait - prev.iowait : 0;
                        cpu_pct    = 100.0 * (double) db / (double) dt;
                        iowait_pct = 100.0 * (double) dw / (double) dt;
                    }
                    prev = cur;
                }
                // Mem
                uint64_t total_kb = 0, avail_kb = 0;
                read_meminfo(total_kb, avail_kb);
                double sys_mem_pct = -1.0;
                uint64_t used_kb = 0;
                if (total_kb > 0) {
                    used_kb = total_kb > avail_kb ? total_kb - avail_kb : 0;
                    sys_mem_pct = 100.0 * (double) used_kb / (double) total_kb;
                }
                uint64_t rss_kb = 0, hwm_kb = 0;
                read_proc_self_status(rss_kb, hwm_kb);
                // GPU (AMD GTT)
                uint64_t gpu_used = 0, gpu_total = 0;
                const bool have_gpu = read_amd_gtt(gpu_used, gpu_total);
                // Load
                double load[3] = {-1, -1, -1};
                load_avg_3(load);
                // FD / TCP
                const uint64_t fd_open = open_fd_count();
                const uint64_t fd_lim  = fd_soft_limit();
                const TcpStates tcp    = count_tcp_states();
                const uint64_t eph_range = ephemeral_port_range();
                const auto in_flight   = ctx_ref.n_in_flight.load(std::memory_order_relaxed);
                const auto reqs        = ctx_ref.n_requests.load(std::memory_order_relaxed);
                const auto errs        = ctx_ref.n_errors.load(std::memory_order_relaxed);
                const auto bytes_in    = ctx_ref.n_bytes_in.load(std::memory_order_relaxed);
                const auto bytes_out   = ctx_ref.n_bytes_out.load(std::memory_order_relaxed);
                const auto uptime_s    = std::chrono::duration_cast<std::chrono::seconds>(
                                             std::chrono::steady_clock::now() - t_start).count();

                std::fprintf(stderr,
                    "[easyai-server] METRICS uptime=%llds  "
                    "cpu: usage=%.1f%% iowait=%.1f%% load=%.2f %.2f %.2f  "
                    "mem: rss=%s peak=%s sys=%.1f%% (%s/%s)  ",
                    (long long) uptime_s,
                    cpu_pct, iowait_pct, load[0], load[1], load[2],
                    fmt_bytes(rss_kb << 10).c_str(),
                    fmt_bytes(hwm_kb << 10).c_str(),
                    sys_mem_pct,
                    fmt_bytes(used_kb << 10).c_str(),
                    fmt_bytes(total_kb << 10).c_str());
                if (have_gpu) {
                    const double gpu_pct = gpu_total > 0
                        ? 100.0 * (double) gpu_used / (double) gpu_total : 0;
                    std::fprintf(stderr, "gpu: gtt=%s/%s (%.1f%%)  ",
                        fmt_bytes(gpu_used).c_str(),
                        fmt_bytes(gpu_total).c_str(),
                        gpu_pct);
                } else {
                    std::fprintf(stderr, "gpu: n/a  ");
                }
                std::fprintf(stderr,
                    "http: in_flight=%llu reqs=%llu err=%llu in=%lluB out=%lluB  "
                    "fd: %llu/%llu (%.1f%%)",
                    (unsigned long long) in_flight,
                    (unsigned long long) reqs,
                    (unsigned long long) errs,
                    (unsigned long long) bytes_in,
                    (unsigned long long) bytes_out,
                    (unsigned long long) fd_open,
                    (unsigned long long) fd_lim,
                    fd_lim > 0 ? 100.0 * (double) fd_open / (double) fd_lim : 0.0);
                if (tcp.have) {
                    // System-wide TCP state breakdown.  TIME_WAIT counted
                    // against the ephemeral port range — that's what the
                    // kernel allocates from for outbound connect()s, so
                    // it's the actual choke-point. >50% sustained means
                    // SO_REUSEADDR / shorter tcp_fin_timeout / connection
                    // pooling on the calling side is overdue.
                    const double tw_pct = eph_range > 0
                        ? 100.0 * (double) tcp.time_wait / (double) eph_range : 0.0;
                    const char * pressure =
                        tw_pct >= 80.0 ? " CRITICAL" :
                        tw_pct >= 50.0 ? " HIGH"     :
                        tw_pct >= 20.0 ? " elevated" : "";
                    std::fprintf(stderr,
                        "  tcp: estab=%llu time_wait=%llu close_wait=%llu fin_wait=%llu "
                        "listen=%llu  TIME_WAIT %llu/%llu ephemeral ports (%.1f%%%s)",
                        (unsigned long long) tcp.established,
                        (unsigned long long) tcp.time_wait,
                        (unsigned long long) tcp.close_wait,
                        (unsigned long long) tcp.fin_wait,
                        (unsigned long long) tcp.listen,
                        (unsigned long long) tcp.time_wait,
                        (unsigned long long) eph_range,
                        tw_pct,
                        pressure);
                } else {
                    std::fprintf(stderr, "  tcp: n/a");
                }
                std::fputc('\n', stderr);
                std::fflush(stderr);
            }
        });
        std::fprintf(stderr,
            "[easyai-server] metrics ticker every %ds (set --metrics-interval N or [SERVER] metrics_interval, 0 disables)\n",
            interval_s);
    }

    // ---- webui routes ---------------------------------------------------
    // We serve the embedded llama-server-derived bundle by default. The
    // operator can fall back to the small inline kWebUI by passing
    // --webui minimal at startup.  When EASYAI_BUILD_WEBUI was OFF at
    // configure time, the modern path simply isn't available and we
    // always serve the minimal one.
    const bool serve_modern = (args.webui_mode != "minimal");
#if defined(EASYAI_BUILD_WEBUI)
    if (serve_modern) {
        svr.Get("/", [&](const httplib::Request &, httplib::Response & res) {
            // Required by some embedded resources (matches llama-server defaults).
            res.set_header("Cross-Origin-Embedder-Policy", "require-corp");
            res.set_header("Cross-Origin-Opener-Policy",   "same-origin");
            res.set_content(ctx_ref.webui_html, "text/html; charset=utf-8");
        });
        svr.Get("/bundle.js", [&](const httplib::Request &, httplib::Response & res) {
            // Serve the customised bundle (brand / placeholder / icon
            // substitutions baked in at startup) when available; fall back
            // to the raw embedded bytes otherwise.
            if (!ctx_ref.webui_bundle_js.empty()) {
                res.set_content(ctx_ref.webui_bundle_js,
                                "application/javascript; charset=utf-8");
            } else {
                res.set_content(reinterpret_cast<const char*>(bundle_js),
                                bundle_js_len,
                                "application/javascript; charset=utf-8");
            }
        });
        svr.Get("/bundle.css", [](const httplib::Request &, httplib::Response & res) {
            res.set_content(reinterpret_cast<const char*>(bundle_css), bundle_css_len,
                            "text/css; charset=utf-8");
        });
        svr.Get("/loading.html", [](const httplib::Request &, httplib::Response & res) {
            res.set_content(reinterpret_cast<const char*>(loading_html), loading_html_len,
                            "text/html; charset=utf-8");
        });
        // Minimal /props stub — tells the webui what the server can do.
        // We advertise thinking + tool calls so the webui's <think>...</think>
        // and tool-call renderers light up.  No image / audio modalities, no
        // model swapping, no slots.
        //
        // Gated behind require_auth: the response includes model_path and
        // a few internal capability hints that an unauthenticated client
        // shouldn't enumerate when --api-key is set.
        svr.Get("/props", [&](const httplib::Request & q, httplib::Response & res) {
            if (!require_auth(ctx_ref, q, res)) return;
            ordered_json p;
            p["model_alias"]   = ctx_ref.model_id;
            p["model_path"]    = ctx_ref.engine.model_path();
            p["total_slots"]   = 1;
            p["modalities"]    = { {"vision", false}, {"audio", false} };
            p["chat_template"] = "";
            p["chat_template_caps"] = {
                {"supports_tools",                true},
                {"supports_tool_calls",           true},
                {"supports_system_role",          true},
                {"supports_parallel_tool_calls",  false},
                {"supports_preserve_reasoning",   true},   // pass <think>...</think> through
            };
            p["bos_token"]     = "";
            p["eos_token"]     = "";
            p["build_info"]    = "easyai-server";
            p["webui"]         = "easyai";
            p["webui_settings"] = json::object();
            p["default_generation_settings"] = {
                {"params", {
                    {"temperature", ctx_ref.def_temperature},
                    {"top_p",       ctx_ref.def_top_p},
                    {"top_k",       ctx_ref.def_top_k},
                    {"min_p",       ctx_ref.def_min_p},
                }},
                {"n_ctx", ctx_ref.engine.n_ctx()},
            };
            p["endpoint_props"]   = false;
            p["endpoint_slots"]   = false;
            p["endpoint_metrics"] = ctx_ref.api_key.empty();   // hint
            p["is_sleeping"]      = false;
            res.set_content(p.dump(), "application/json");
        });
    } else
#endif
    {
        // Minimal inline webui (legacy / fallback / when EASYAI_BUILD_WEBUI=OFF)
        svr.Get("/", [&](const httplib::Request &, httplib::Response & res) {
            res.set_content(ctx_ref.webui_html, "text/html; charset=utf-8");
        });
    }

    // Favicon: serve the operator-supplied icon when --webui-icon was
    // given; otherwise serve the embedded brain SVG that ships with the
    // binary so the page is never branded as a generic globe.
    auto serve_favicon = [&](const httplib::Request &, httplib::Response & res){
        if (!ctx_ref.webui_icon.empty()) {
            res.set_content(ctx_ref.webui_icon, ctx_ref.webui_icon_mime.c_str());
            return;
        }
        // Embedded default — the AI Box favicon, inlined as kBrandSvg
        // at the top of this file (canonical copy at webui/AI-brain.svg).
        res.set_content(kBrandSvg.data(), kBrandSvg.size(), "image/svg+xml");
    };
    svr.Get("/favicon",     serve_favicon);
    svr.Get("/favicon.ico", serve_favicon);
    svr.Get("/favicon.svg", serve_favicon);
    svr.Get ("/health",               [&](const auto & q, auto & r){ route_health  (ctx_ref, q, r); });
    if (args.metrics) {
        svr.Get ("/metrics",          [&](const auto & q, auto & r){ route_metrics (ctx_ref, q, r); });
    }
    svr.Get ("/v1/models",            [&](const auto & q, auto & r){
        if (!require_auth(ctx_ref, q, r)) return;
        route_models(ctx_ref, q, r);
    });
    svr.Get ("/v1/tools",             [&](const auto & q, auto & r){
        if (!require_auth(ctx_ref, q, r)) return;
        route_tools(ctx_ref, q, r);
    });
    svr.Post("/v1/chat/completions",  [&](const auto & q, auto & r){
        if (!require_auth(ctx_ref, q, r)) return;
        route_chat_completions(ctx_ref, q, r);
    });
    svr.Post("/v1/preset",            [&](const auto & q, auto & r){
        if (!require_auth(ctx_ref, q, r)) return;
        route_preset(ctx_ref, q, r);
    });

    // Ollama-compat list-models. Same auth posture as /v1/models.
    svr.Get ("/api/tags",             [&](const auto & q, auto & r){
        if (!require_auth(ctx_ref, q, r)) return;
        route_ollama_tags(ctx_ref, q, r);
    });
    svr.Get ("/api/show",             [&](const auto & q, auto & r){
        if (!require_auth(ctx_ref, q, r)) return;
        route_ollama_show(ctx_ref, q, r);
    });
    svr.Post("/api/show",             [&](const auto & q, auto & r){
        if (!require_auth(ctx_ref, q, r)) return;
        route_ollama_show(ctx_ref, q, r);
    });

    // Model Context Protocol. AUTH OPEN by V1 design — see route_mcp
    // comment. Both POST (request/response) and GET (placeholder for
    // future SSE notification stream) are wired; GET currently
    // returns 405 to be explicit.
    svr.Post("/mcp",                  [&](const auto & q, auto & r){
        route_mcp(ctx_ref, q, r);
    });
    svr.Get ("/mcp",                  [](const auto &, auto & r){
        r.status = 405;
        r.set_header("Allow", "POST");
        r.set_content(
            "{\"error\":\"GET /mcp is not yet implemented; "
            "use POST with a JSON-RPC 2.0 request body. "
            "Server-pushed notifications via SSE will land in a "
            "future version.\"}",
            "application/json");
    });

    // Last-chance error handler — never let a thrown exception propagate
    // out of the HTTP layer (httplib would close the socket abruptly).
    svr.set_exception_handler([](const auto & req, auto & res, std::exception_ptr ep) {
        try { if (ep) std::rethrow_exception(ep); }
        catch (const std::exception & e) {
            // Always log to stderr — operators reading journalctl need
            // to see uncaught exceptions / timeouts even without
            // --verbose. The mark_problem() call below also drops a
            // greppable banner into the raw log file.
            std::fprintf(stderr,
                "[easyai-server] EXCEPTION on %s %s from %s:%d : %s\n",
                req.method.c_str(), req.path.c_str(),
                req.remote_addr.c_str(), req.remote_port,
                e.what());
            easyai::log::mark_problem(
                "Server: uncaught exception on %s %s from %s:%d : %s",
                req.method.c_str(), req.path.c_str(),
                req.remote_addr.c_str(), req.remote_port,
                e.what());
            res.status = 500;
            res.set_content(error_json(std::string("uncaught: ") + e.what(),
                                        "internal_error"),
                            "application/json");
        }
    });

    // cpp-httplib hands any read/write timeout (including the long
    // SSE-stream cuts that prompt a "Failed to read connection" on the
    // client side) to its error handler with status 408 Request Timeout
    // when no other handler set the body. Hook it so a timeout doesn't
    // disappear silently from the operator's stderr.
    svr.set_error_handler([](const auto & req, auto & res) {
        if (res.status == 408 || res.status == 504) {
            std::fprintf(stderr,
                "[easyai-server] WARN HTTP %d timeout on %s %s from %s:%d "
                "(check --http-timeout, currently logged in startup banner)\n",
                res.status,
                req.method.c_str(), req.path.c_str(),
                req.remote_addr.c_str(), req.remote_port);
        } else if (res.status >= 500) {
            std::fprintf(stderr,
                "[easyai-server] ERROR HTTP %d on %s %s from %s:%d\n",
                res.status,
                req.method.c_str(), req.path.c_str(),
                req.remote_addr.c_str(), req.remote_port);
        }
    });

    g_server.store(&svr);
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    bool ok = svr.listen(args.host.c_str(), args.port);
    g_server.store(nullptr);

    // Wake the metrics ticker so it exits its wait_for promptly, then join.
    g_metrics_stop.store(true, std::memory_order_relaxed);
    g_metrics_cv.notify_all();
    if (metrics_thread.joinable()) metrics_thread.join();

    std::fprintf(stderr, "[easyai-server] %s\n", ok ? "stopped cleanly" : "listen failed");
    return ok ? 0 : 1;
}
