# knowledge — the agent's persistent registry

> *"A model that forgets between sessions is an expensive autocomplete.
> A model that remembers is a colleague. The `knowledge` tools are the
> cheapest path from the first to the second."*

This document is the authoritative guide to easyai's `knowledge` tools:
a keyword-indexed, file-backed long-term memory the model can use to
remember things across sessions.

Under the hood the `knowledge` tools use **a passive RAG technique** — a
deliberately minimal take on Retrieval-Augmented Generation: no
embedding model, no vector store, no similarity index, no database —
just a directory of small Markdown files the agent reads, writes, and
curates by itself. The agent classifies its own memory with keywords;
we keep the directory and the index. That's the whole system.

---

## Table of contents

1. [What the `knowledge` tools are, and why](#1-what-the-knowledge-tools-are-and-why)
2. [Quickstart](#2-quickstart)
3. [The file format on disk](#3-the-file-format-on-disk)
4. [The seven `knowledge_*` tools](#4-the-seven-tools)
5. [How the model is encouraged to use them](#5-how-the-model-is-encouraged-to-use-them)
6. [Workflows](#6-workflows)
7. [Best practices](#7-best-practices)
8. [Corner cases](#8-corner-cases)
9. [Operator workflows](#9-operator-workflows)
10. [Roadmap — progressive recall, document ingestion, multi-user](#10-roadmap)
11. [Troubleshooting](#11-troubleshooting)

---

## 1. What the `knowledge` tools are, and why

The `knowledge` tools are a **keyword-indexed key/value store**, owned
by the agent, persisted on disk, accessible to the agent as **its own
memory** — something it can search, store, append to, recall, update,
and forget. They work by **a passive RAG technique**: keyword-indexed
Markdown files the agent saves and searches itself, with no embedding
model or vector store behind it. The tool surface is seven separate
tools:

```
knowledge_save(keywords, content, fix?)        store / overwrite; fix=true → immutable
knowledge_append(keywords, content)            grow an existing entry or create a new one
knowledge_search(keywords, page?, max_results?) find by keywords; matches ANY keyword, paginated
knowledge_load(keywords)                       recall a full entry by keywords
knowledge_list(prefix?, max?)                  browse entries
knowledge_delete(keywords)                     forget a stale entry (fixed entries refused)
knowledge_keywords(min_count?, max?)           vocabulary overview
```

That is the whole API. **Keywords ARE the identifier** — there is no
separate title. The sorted keywords joined by `_` become the filename
stem. Example: `knowledge_save(keywords="python async", ...)` writes
`async_python.md`. Keywords are parsed by splitting on `_ , / <space>`.

The model decides what to remember and how to classify it. Keywords
are both the identifier and the index — they determine the filename
and how entries are found. The directory is the index. There is no
embedding model, no similarity scoring, no neighbours. The model
already has everything it needs to reason about its own memory; we
just give it a place to put the bits it cared about.

### Fixed entries — immutable knowledge the model can't forget

Any entry whose filename starts with `fix-` is **immutable**:

* `knowledge_save` refuses to overwrite it.
* `knowledge_delete` refuses to remove it.
* `knowledge_search` and `knowledge_load` work normally — and
  tag the entry `[FIXED]` / `fixed: yes` so the model knows it's
  looking at ground-truth knowledge, not a working note.

To mint one, the model passes `fix=true` to `knowledge_save`; the
filename is auto-prepended with `fix-` if not already present, and
the file becomes read-only-via-tool from that point on. Use this when
the user explicitly asks to "learn this as a rule", "remember this as
the design", "this is the spec — memorise it". The only way to change
a fixed entry is for the operator to remove the file from disk by hand.

### Why seven separate tools

`--memory <dir>` registers seven `knowledge_*` tools via
`knowledge_split_tools()`. Each tool has its own flat schema with only
the parameters it needs — no discriminated union, no `action`
parameter. This makes tool-calling straightforward for every model
tier: each tool's schema is minimal, validation is compile-time
obvious, and the model never has to remember which parameters belong
to which action.

### How information flows

```
  ┌──────────────────────────────────────────────────────────────────┐
  │                            THE MODEL                              │
  │        (sees seven knowledge_* tools in its toolbelt)             │
  └──────────────────────────────────────────────────────────────────┘
        WRITE PATH (mutates state — unique_lock)
        ┌──────────────┐  ┌──────────────────┐  ┌──────────────────┐
        │knowledge_save│  │knowledge_append  │  │knowledge_delete  │
        │              │  │                  │  │                  │
        │new /         │  │ grow existing    │  │ prune stale      │
        │overwrite     │  │ or create new    │  │ entry            │
        └──────┬───────┘  └────────┬─────────┘  └────────┬─────────┘
               │                   │                     │
        READ PATH (parallel — shared_lock)
        ┌────────────────┐  ┌────────────────┐  ┌────────────────┐  ┌────────────────────┐
        │knowledge_search│  │knowledge_load  │  │knowledge_list  │  │knowledge_keywords  │
        │                │  │                │  │                │  │                    │
        │ find by        │  │ read full      │  │ browse         │  │ vocab overview     │
        │ keyword        │  │ entry          │  │ entries        │  │ (counts)           │
        └───────┬────────┘  └───────┬────────┘  └───────┬────────┘  └─────────┬──────────┘
                │                   │                   │                     │
                ▼                   ▼                   ▼                     ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │                       RagStore (in-process)                       │
  │  ┌──────────────────────────────────────────────────────────┐   │
  │  │  in-memory index: stem → { keywords, mtime, bytes }      │   │
  │  │  stem = sorted keywords joined by '_'                     │   │
  │  │  lazy-loaded from disk on first call, kept fresh by      │   │
  │  │  every save / append / delete; shared_mutex (multi-      │   │
  │  │  reader / single-writer)                                  │   │
  │  └──────────────────────────────────────────────────────────┘   │
  │                                                                   │
  │   search / list / keywords  ─→  index lookup, no disk read        │
  │   load                      ─→  one file read off disk            │
  │   save                      ─→  atomic tempfile + rename(2)       │
  │   append                    ─→  read body → merge → atomic write  │
  │                                  (whole RMW under unique_lock)    │
  │   delete                    ─→  unlink + index erase              │
  └──────────────────────────────┬───────────────────────────────────┘
                                 │
                                 ▼
  ┌──────────────────────────────────────────────────────────────┐
  │            /var/lib/easyai/rag/    (filesystem)               │
  │                                                               │
  │   locale_user-prefs.md  keywords: user-prefs, locale          │
  │   build_easyai_recipe.md keywords: easyai, build, recipe      │
  │   mqtt_protocol_qos.md  keywords: mqtt, qos, protocol        │
  │   README.md             (no keywords header — untagged)       │
  └──────────────────────────────────────────────────────────────┘
```

The lifecycle of a piece of knowledge:

```
  SESSION 1 (Mon)
  ───────────────
  user:  "I prefer terse PT-BR responses."
  model: knowledge_keywords()                ← sees "user-prefs" already
                                                exists in vocabulary
         knowledge_save(                     ← reuses existing keyword
                keywords="user-prefs locale",
                content="Prefers PT-BR, terse...")
                                              ← atomic write to
                                                locale_user-prefs.md
  ────────────────────────────────────  end of session ─────────

  SESSION 2 (Wed, fresh process)
  ──────────────────────────────
  user:  "build the project"
  model: knowledge_search(keywords="easyai build")  ← matches → build_easyai_recipe.md
         knowledge_load(keywords="build easyai recipe")  ← reads body
         knowledge_search(keywords="user-prefs")    ← finds locale_user-prefs.md
         knowledge_load(keywords="locale user-prefs")  ← reads body
                                              ← model now answers
                                                in PT-BR, terse, with
                                                the right build command

  ────────────────────────────────────  later that week ─────────

  user:  "we dropped the X feature"
  model: knowledge_search(keywords="x-feature")  ← finds 3 stale entries
         knowledge_delete(keywords="rationale x-feature")
         knowledge_delete(keywords="roadmap x-feature")    ← curation keeps the
         knowledge_delete(keywords="userflow x-feature")     vocabulary clean for
                                                future searches
```

Three things make this loop work:

1. **The model classifies its own memory.** It saw the conversation;
   it picks 2-5 stable, descriptive, reusable keywords. Keywords ARE
   the identifier — no separate title needed.
2. **Keywords are the index.** No embeddings, no GPU, no opaque
   ranking — exact-match lookup over a small in-memory map.
3. **The model curates.** `knowledge_keywords` lets it see what
   vocabulary it has built; `knowledge_delete` lets it prune.
   Without curation, the index drifts and old entries become
   unreachable.

### Why no vector store / embeddings?

The popular flavour of RAG plugs an embedding model + a vector store
in front of the LLM and ranks chunks by cosine similarity. That's
the right answer when you have a huge corpus that nobody
classified.

easyai's `knowledge` tools flip the assumption with a passive RAG
technique: **the agent IS the classifier**. When the model decides to
remember something, it tells you (in clear language, in the same call)
what the entry is about. Put that classification in the filename + a
small header and you can find the entry in O(1) per lookup, with no
GPU, no embedding inference, no opaque ranking, no schema migrations.

When easyai later grows progressive recall (auto-inject the N
most-relevant entries on every session start), THAT layer can add
similarity scoring on top of the `knowledge` tools without changing
what's on disk. The tools themselves stay simple: files and keywords.

### Why files, not a database?

So you, the operator, can `cat`, `vim`, `grep`, drop a hand-written
note into the dir, archive an entry by moving the file, share a
snippet by copying it. There is no ceremony. The agent's memory is
human-inspectable at all times.

---

## 2. Quickstart

### On the installed server (knowledge tools are on by default)

The systemd-installed easyai-server already passes `--memory
/var/lib/easyai/rag` for you. Verify:

```bash
sudo journalctl -u easyai-server | grep "memory enabled"
# easyai-server: memory enabled (7 knowledge tools), root = /var/lib/easyai/rag

ls -la /var/lib/easyai/rag/
# -rw-r----- root easyai 0 README.md   (empty initially)
```

Open the webui or hit the API. The model now has seven `knowledge_*`
tools (save / search / load / list / delete / append / keywords) in
its tool list. Tell it something memorable and it will save it without
further prompting.

### From easyai-cli (remote model, local memory)

```bash
mkdir -p ~/easyai-reg
easyai-cli --url http://127.0.0.1:8080 --memory ~/easyai-reg
```

Same seven `knowledge_*` tools, but the memory lives in your home
directory.

### From easyai-local (single-process REPL with `knowledge` tools)

```bash
easyai-local -m model.gguf --memory ~/easyai-reg
```

### Verifying it works

After a chat that should have triggered a save:

```bash
ls /var/lib/easyai/rag/
cat /var/lib/easyai/rag/<stem>.md
```

Or from the model:

```
> knowledge_list() everything I know
```

---

## 3. The file format on disk

Each entry is one file: `<stem>.md`, where `stem` is the sorted
keywords joined by `_`. Example: keywords `"python async"` produce
`async_python.md`. Format is intentionally trivial:

```
keywords: user-prefs, hardware, radv

Body content here.
Free-form UTF-8 text. Can be Markdown, code blocks, structured snippets,
plain prose — whatever the model wanted to remember. Up to 256 KB.
```

The grammar:

1. The first line, if it looks like `<key>: <value>`, is the header.
2. We currently recognise one key: `keywords:`. Comma-separated values.
3. A blank line ends the header.
4. Everything after is the body.

A file with NO header (no `keywords:` line) is treated as
**untagged**. It shows up in `knowledge_list` but never in
`knowledge_search`. This is by design — operators can drop
hand-written notes into the dir and the model will list them as
available context, but won't consider them "tagged knowledge" until
the operator (or the model) adds keywords.

### Hand-authoring an entry

```bash
sudo -u easyai bash -c 'cat > /var/lib/easyai/rag/welcome.md' <<'EOF'
keywords: user-prefs, language, locale

The user prefers responses in Brazilian Portuguese (PT-BR). Technical
jargon in English is fine. Keep responses terse — favour code or
commands over long explanations. Default code style: C++17, snake_case
identifiers, no exceptions in hot paths.
EOF
```

Restart the server (or wait for the next session) and the model has
it on its first `knowledge_search(keywords="user-prefs")`.

### Constraints

Keywords share the character set `[A-Za-z0-9._+-]`. The input string
is split on `_ , / <space>` to extract individual keywords. Sorted
keywords are then joined by `_` to form the filename stem. No
slashes, no shell metacharacters. The strict regex closes path-
traversal at parse time — there is no way for the model to write
outside the RAG dir.

| Field | Char set | Length | Extras |
| --- | --- | --- | --- |
| Keyword | `[A-Za-z0-9._+-]` | 1..32 | Up to 8 keywords per entry. Sorted and joined by `_` to produce the filename stem (which inherits filesystem-safety from the keyword charset). |
| Filename stem | `[A-Za-z0-9._+-]` | 1..64 | Derived from sorted keywords. Cannot be `.` or `..`, cannot start with `.`, must contain ≥1 alnum. |
| Body | UTF-8 | ≤ 256 KiB | Free-form. Markdown / code / prose / JSON — the model is the only reader. |

Why each non-alnum character is allowed:

- `-` `_`: classic word separators (`user-prefs`, `cmd_recipe`).
- `.`: versions (`v1.0`), namespaces (`project.easyai`), file
  references (`nginx.conf`).
- `+`: niche but real — `c++`, `git+ssh`, `a+b`-style recipes.

What's deliberately blocked:

- Spaces — used as keyword separator in input, not stored in filenames.
- `/` — used as keyword separator in input, path-component separator.
- `,` — used as keyword separator in input.
- `\` — path-component separator.
- `:` — reserved on Windows, ADS-style abuse on NTFS.
- Quotes, `$`, backticks, `#`, `&`, `|`, `;` — shell-metachar traps.

If you find yourself wanting one of the blocked characters, that's
usually a sign the keyword is trying to encode structure that should
be multiple keywords — split into separate keywords, or use `.` for
hierarchy.

---

## 4. The seven `knowledge_*` tools

The model sees seven separate `knowledge_*` tools in its toolbelt.
The descriptions below are what the MODEL reads; they were written to
actively encourage use — see §5. Each section header names the tool;
the parameter signature and behaviour follow.

### knowledge_save

```
knowledge_save(keywords: string, content: string, fix?: boolean) -> ok
```

Sorts the keywords, joins them with `_` to derive the filename stem,
and writes `<root>/<stem>.md`. Overwrites if the stem already exists
(this is how the model **replaces** an entry wholesale; for
**growing** an existing entry without losing its body, use
`knowledge_append`). Atomic on POSIX (tempfile + rename). Refuses
invalid keywords with a clear error.

Pass `fix=true` to mint a **fixed entry**: the filename is auto-
prepended with `fix-` if not already, and from then on the file is
immutable through the tool surface — `knowledge_save` will refuse to
overwrite it, `knowledge_append` will refuse to grow it, and
`knowledge_delete` will refuse to remove it. Use this to seed system
designs, hard rules, ground-truth definitions the model must not
rewrite mid-conversation. The on-disk content is plain Markdown like
any other entry; the immutability is enforced by the filename prefix,
so `ls fix-*` is the canonical "show me every fixed entry" listing.

### knowledge_append

```
knowledge_append(keywords: string, content: string) -> ok
```

Derives the filename stem from sorted keywords. If the entry exists,
reads the current body off disk, appends `content` after a Markdown
horizontal rule (`---`), and writes the merged file back via the same
atomic tempfile + rename(2) `knowledge_save` uses. If the entry is
new, creates it (behaves like `knowledge_save`). Use this when you've
**learned more about something you already wrote down** — refining a
user's preferences, accumulating a project's running log, growing a
debugging trail across sessions — without losing the previous content.

Why a separator? So the operator opening the `.md` file sees
exactly where each appendix begins. Multiple appends stack: old ->
rule -> newer -> rule -> newest. The format stays plain Markdown.

Return messages:
- **New entry**: `new entry saved as "stem.md" (N bytes, K keywords)`
- **Appended**: `updated "stem.md" (+N B -> M B total, K keywords)`

Refused on:
- filenames starting with `fix-` (immutable);
- merged content that would exceed 256 KiB (split into a new
  entry with `knowledge_save` instead).

Concurrency. The whole RMW (existence check + read + merge + write)
runs under one `unique_lock` on the store's `shared_mutex`, so
concurrent `knowledge_append` / `knowledge_save` / `knowledge_delete`
calls on the same store serialise. Two threads appending to the SAME
entry queue up; both appendices land. Concurrent reads
(`knowledge_search` / `knowledge_load` / `knowledge_list` /
`knowledge_keywords`) hold a shared_lock and parallelise except while
a writer holds the unique_lock — same discipline as the rest of the
RagStore.

### knowledge_search

```
knowledge_search(keywords: string, max_results?: integer = 10, page?: integer)
  -> list of {stem, keywords, preview, matched/total}
```

Pass keywords as a string (split on `_ , / <space>`). Matches entries
carrying ANY of the provided keywords. Each result reports
`matched N/M` so the model can rank: an entry that matched 3 of the
4 queried keywords is more relevant than one that matched only 1.
Best-overlap first, ties broken by recency.

Returns up to 20 entries (preview ~ 240 bytes per entry). The model
picks the most relevant entries and calls `knowledge_load` to read
their bodies.

**Optimisation pattern:** lead with the one keyword every relevant
entry must carry, then add 2-3 softer keywords to rank. If some
entries score `M/M` (full match), you've found exact hits; if the
best is `1/4`, only one keyword landed — the softer ones are too
specific, or your notes are sparser than you thought.

**Pagination.** Every response begins with three machine-readable
header lines:

```
total_entries: 47
page: 1 of 5
showing: 10  (entries 1..10)
has_more: true
```

When `has_more: true`, issue the SAME query with `page=P+1` to walk
the rest. The first page is already best-first, so you usually
don't need more — the model decides. Asking for `page=99` past the
end gets a clear "past the last page" message, not an error.

### knowledge_load

```
knowledge_load(keywords: string) -> entry with full body
```

Derives the filename stem from sorted keywords and recalls the full
entry off disk. The response includes keywords, a human-readable
`modified` timestamp + unix epoch, and a `fixed: yes/no` line so the
model knows whether the entry is immutable. If the derived filename
doesn't exist, an error message is returned.

### knowledge_list

```
knowledge_list(prefix?: string, max?: integer = 50) -> list of entries
```

Browse mode. Returns stem, keywords, content_bytes, and a human-
readable modified date for every entry (or every entry whose filename
starts with `prefix`). Entries whose filename starts with `fix-` are
tagged `[FIXED]`. Body NOT included — use `knowledge_load` for that.
`prefix='fix-'` lists every fixed entry in one call.

### knowledge_delete

```
knowledge_delete(keywords: string) -> ok
```

Permanent forget. Derives the filename stem from sorted keywords,
removes the file from disk and the in-memory index. Idempotent on
regular entries: forgetting a non-existent entry is not an error.
**Fixed entries are refused** — any filename starting with `fix-` is
rejected with a clear message; the operator must remove the file by
hand if it really needs to go.

### knowledge_keywords

```
knowledge_keywords(min_count?: integer = 1, max?: integer = 200)
  -> { total_keywords, total_entries, showing, [keyword, count]* }
```

Vocabulary overview. Lists every distinct keyword used across the
knowledge store with the number of entries that reference it. Sorted
by frequency (most-used first), tie-broken alphabetically.

**Why it matters.** Without `knowledge_keywords`, an agent that
doesn't check its own vocabulary creates near-duplicates over time —
`user-prefs` vs `user_pref` vs `preferences`, `cmd-recipe` vs
`command-recipe`, etc. — and the index slowly fragments. Old
entries become unreachable to new searches because the queries
target slightly-different keywords. Calling `knowledge_keywords`
before `knowledge_save` (or before `knowledge_search` when you don't
know what's in the store) keeps the vocabulary stable and the index
coherent.

**Filters.** `min_count=2` hides one-off keywords (those used by a
single entry), surfacing only the established vocabulary. `max`
caps the result count; the long tail is dropped first.

**Example output:**

```
total_keywords: 23
total_entries: 47
showing: 23

user-prefs       12 entries
project-easyai    9 entries
cmd-recipe        7 entries
fix-vulkan        5 entries
mqtt              4 entries
qos               3 entries
…
asyncio           1 entry
django            1 entry
```

The first lines tell the model which dimensions of knowledge it
has invested in; the long tail is candidates for either
consolidation (rename to a more general keyword + `knowledge_save`)
or deletion.

### Tool registration

`--memory <dir>` registers seven tools via `knowledge_split_tools()`:
`knowledge_save`, `knowledge_append`, `knowledge_search`,
`knowledge_load`, `knowledge_list`, `knowledge_delete`,
`knowledge_keywords`. Each tool has its own flat schema with only the
parameters it needs.

**On-disk layout, locking discipline, fix-entry rules, error
messages — all unchanged.** The directory is the source of truth;
the seven tools share one `RagStore` instance with the same
`shared_mutex` discipline described above.

---

## 5. How the model is encouraged to use them

Tool descriptions are not boilerplate. They are the most direct
incentive structure we have: the model reads them on every turn and
they shape its behaviour.

The `knowledge_*` tools' descriptions push three behaviours:

1. **Save aggressively.** The `save` action's description literally
   says "USE THIS AGGRESSIVELY for: the user's stated preferences and
   constraints, project structure and decisions you've learned,
   technical facts you had to look up, recipes / commands that
   worked, error patterns and their fixes, domain knowledge from
   documents the user fed you. The more carefully you populate the
   registry, the smarter you become over time."

2. **Search before assuming.** The `search` action's description says
   "USE THIS BEFORE assuming you don't know something the user might
   have told you in a past session — your past self may have already
   saved the answer."

3. **Tidy up.** The `delete` action's description encourages removing
   stale entries: "keeping the registry tidy makes future searches
   sharper."

The more often the model exercises these, the more useful the
`knowledge` tools become.

### Automatic vocabulary injection (since 2026-05-16)

The "search before assuming" rule only works if the model knows
WHAT keywords to search for. To close that gap, every binary that
loads memory (`easyai-server`, `easyai-local`, `easyai-cli`) now
auto-injects a compact vocabulary snapshot into the system prompt:

```
# MEMORY VOCABULARY (the keywords your private memory currently
has tagged — the FIRST place to look for anything you might
already know)
12 entries (most-common first; call knowledge_search(keywords="<name> ...")
to recall):
easyai(8) claude(5) bitnet(3) build(3) iteration(2) …
```

* Sorted count desc, name asc; capped at top 40 keywords.
* Empty memory store → no block, no wasted tokens.
* **server** refreshes the snapshot every request (fresh disk
  scan, ~10-50ms — rounding error vs. inference).
* **local** computes it once at startup and appends to the
  system prompt; long-running chats won't see new keywords until
  restart (acceptable for the one-shot / single-chat local
  pattern).
* **cli** computes it once when building the system prefix to
  send to the remote server.

The model no longer has to call `knowledge_keywords` to discover its
own vocabulary — it sees the list every turn and can dispatch the
right `knowledge_search` directly. The `knowledge_keywords` tool is
still available when the model wants fresh counts mid-task or needs
the full list past the top-40 cap.

The block is built by `easyai::preamble::build()` (see
`include/easyai/preamble.hpp`); the renderer is
`easyai::tools::render_memory_vocabulary()`. Both are public APIs
so third-party hosts of libeasyai get the same behaviour.

**Block position — tail (2026-05-26).** The vocab block lands AT
THE END of the preamble suffix, after AUTHORITATIVE DATE/TIME,
KNOWLEDGE CUTOFF, KNOWLEDGE LOOP, and CITE SOURCES. Reason:
prompt-eval KV cache. The vocab is the only block that mutates
between requests (any `knowledge_save` / `knowledge_append` /
`knowledge_delete` shifts the keyword count map). Putting it last means a memory
write only invalidates the SUFFIX of the cache — the stable
date/cutoff/rules prefix stays warm.

**Render cache (2026-05-26).** `render_memory_vocabulary` caches
the rendered string keyed on `(root_dir, directory mtime, file
count)`. Hot path is now one `stat(2)` per request instead of an
O(N) directory walk every chat. Memory writes go through
`rename(2)` which bumps the directory mtime — cache invalidates
automatically with no explicit signalling.

| Edge case | Behaviour |
|---|---|
| Two `knowledge_save` calls within one second on a second-resolution filesystem (HFS+, some NFS) | Up to one second of stale vocab can be served if the file-count delta is also zero. Acceptable — the vocab is advisory; the actual `knowledge_search` always hits the live, write-locked index. |
| Directory disappears | `stat(2)` fails, cache returns the last good string; next successful scan refreshes. |
| First-ever call | Cache miss → full directory walk + cache populate; one `stat(2)` thereafter. |

See SECURITY_AUDIT §23.3 for the formal residual.

---

## 6. Workflows

### A. The natural session loop

```
[user opens chat]
  ↓
model: knowledge_search(keywords="user-prefs") → finds "locale_user-prefs"
model: knowledge_load(keywords="locale user-prefs")  → reads the body
model: now knows the user prefers PT-BR, terse style, ...

[user asks a question]
  ↓
model answers in PT-BR, terse.

[user shares a new fact]
  ↓
model: knowledge_save(keywords="project foo", content="...")

[user corrects something]
  ↓
model: knowledge_search(keywords="foo") → finds the old note
model: knowledge_save(keywords=SAME keywords, ...)   ← overwrites with corrected version
                                     OR
model: knowledge_delete(keywords="foo old")
model: knowledge_save(keywords="foo new", ...)
```

### B. Document ingestion (the positive cycle)

You feed the model a long document — a manual, a runbook, a paper.
The model summarises it, then **chunks the summary into knowledge entries**:

```
You:    "Read this MQTT spec PDF. The link is /sandbox/mqtt-v5.pdf.
         Save the important parts to memory so future me doesn't have
         to re-read it."

Model:  [reads the PDF via fs(action="read") or web(action="fetch")]
        [identifies 6 key topics: connect, publish, subscribe,
         retained messages, will, qos]
        knowledge_save(keywords="mqtt connect protocol", content="...")
        knowledge_save(keywords="mqtt publish protocol", content="...")
        knowledge_save(keywords="mqtt qos protocol",     content="...")
        ...
        "Saved 6 entries under keywords 'mqtt' + 'protocol'. Future you
         can knowledge_search(keywords='mqtt') to find any of them, or
         narrow with knowledge_search(keywords='mqtt qos') to focus on
         QoS-related ones."
```

Now the next session, when you ask about MQTT, the model searches with
`knowledge_search(keywords="mqtt")`, finds the 6 entries, loads the
best matches, and answers from the saved knowledge — no re-reading.
If the question is more specific ("MQTT QoS levels"), the model uses
`knowledge_search(keywords="mqtt qos")` — still every `mqtt` entry,
but the ones also tagged `qos` rank first.

This is the **positive cycle**: feed knowledge, saved knowledge,
searched knowledge, recalled knowledge. Each ingestion makes the
agent smarter for every future session.

### C. The "remember that for next time" pattern

```
You:    "Whenever I ask about easyai builds on the AI box,
         the answer is: cmake -B build -DCMAKE_BUILD_TYPE=Release
         && cmake --build build -j. Remember this."

Model:  knowledge_save(keywords="easyai build ai-box",
                content="On the AI box (Linux/Vulkan), build easyai with:
                  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j")
        "Saved. I'll find this with keyword 'easyai' or 'build' next time."
```

Next session:

```
You:    "build easyai on ai box"
Model:  knowledge_search(keywords="easyai build")  →  finds ai-box_build_easyai (matched 2/2)
        knowledge_load(keywords="ai-box build easyai")
        "Use: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
```

### D. Curate (delete)

```
You:    "We dropped the X feature. Remove anything about it from your
         memory."

Model:  knowledge_search(keywords="x-feature")
        [3 entries match]
        knowledge_delete(keywords="rationale x-feature")
        knowledge_delete(keywords="roadmap x-feature")
        knowledge_delete(keywords="userflow x-feature")
        "Deleted 3 entries about the dropped X feature."
```

---

## 7. Best practices

### Choosing keywords

- **Include a domain keyword.** `easyai build recipe`, not just
  `build recipe`. Domain keywords group entries when you
  `knowledge_list(prefix="easyai")`.
- **Keep keyword sets unique by purpose.** If you have ten build
  recipes for ten projects, include the project name as a keyword
  (`easyai build recipe` vs `nginx build recipe`).
- **Prefer stable, reusable keywords.** `user-prefs`, `project-easyai`,
  `cmd-recipe`, `fix-vulkan-radv`, `mqtt`, `protocol`. Avoid
  one-off keywords (`note-from-monday`).
- **2-5 keywords is the sweet spot.** Fewer means harder to find;
  more dilutes the index.
- **Cross-tag aggressively.** A note about the AI box's RADV bug
  fixes belongs under `ai-box`, `vulkan`, `radv`, `bug-fix`. The
  model will search by any of those.
- **Don't dump-everything.** If you can't pick good keywords, the
  entry probably shouldn't exist as a single unit — break it up.

### Body content

- **Granularity matters more than anything else.** Save many small
  focused entries; not a few sprawling ones. `easyai build mac`
  and `easyai build linux` should be **two** entries, not one
  combined `easyai build`. Why: when `knowledge_load` returns an
  entry, the FULL body lands in the model's prompt — a 200-line
  note costs 1000+ tokens whether the model needed all of it or not.
  The search-then-load flow is built around this:
  `knowledge_search` ranks N candidates by overlap, the model picks
  the best, and each loaded body is small enough to fit comfortably.
  **It is always better to do two more loads than to swallow one
  giant one.** Rule of thumb: bodies over ~500 words are usually two
  or more entries pretending to be one.
- **Be specific.** "Use q8_0 KV cache" is vague; "Use `-ctk q8_0 -ctv
  q8_0` to halve the KV cache footprint at no measurable quality
  loss on the 35B model" is useful.
- **Include the answer, not the reasoning.** The model already
  reasoned its way to the conclusion; what future-you wants is the
  conclusion.
- **Markdown if structure helps.** Headers, lists, code fences. The
  body is plain text, so anything readable to a human is fine.

### When NOT to save

- Anything the user said only in passing ("I'm tired today" — not
  worth keeping).
- Anything that's already in the codebase (the model can `git grep`).
- Per-conversation scratch state — that's what conversation history
  is for.
- PII the user didn't authorise persisting.

### When to delete

- The user corrects a fact and the old entry is now wrong.
- A project ends, dies, or pivots.
- You realise you saved something at the wrong granularity (delete +
  `knowledge_save` with new keywords).

---

## 8. Corner cases

| Situation | What happens |
| --- | --- |
| `--memory` not given to the CLI | The `knowledge_*` tools aren't registered. Model has no long-term memory. |
| `--memory` points to a non-existent dir | Created on first `knowledge_save`. No error at startup. |
| `--memory` points to a file (not a dir) | First `knowledge_save` returns "RAG root is not a directory". Other tools also error. |
| Two processes share the same memory dir | Reads work; the in-memory index of one process won't see writes from the other until that process restarts. Single-process is the supported model. |
| Keywords match an existing entry | `knowledge_save` overwrites (atomic). Useful for refining notes. |
| Keyword used by no entry | `knowledge_search` returns "no entries match" (not an error). |
| `knowledge_load` asks for a non-existent entry | Returns an error message naming the derived filename. |
| Hand-authored file with no `keywords:` header | Loaded as untagged. Shows in `knowledge_list`, never in `knowledge_search`. The body is fully accessible via `knowledge_load`. |
| Hand-authored file with garbage in the body | Loaded fine. The body is opaque to the `knowledge` tools. |
| File > 256 KB | Skipped at index time; `knowledge_load` returns "entry exceeds 262144 bytes". Operator should split. |
| Filename with spaces / dots / slashes | Skipped at index time (doesn't match the stem regex). |
| Filename without `.md` extension | Skipped. The agent only sees `.md` files in the dir. |
| Subdirectory inside the memory dir | Ignored — only top-level scanned. |
| Empty memory dir | Normal state. `knowledge_list` returns "RAG is empty". |
| `knowledge_delete` on a non-existent entry | Returns ok with "nothing to delete". Idempotent. |

---

## 9. Operator workflows

### Backing up the memory store

```bash
tar -czf reg-backup-$(date +%Y%m%d).tar.gz /var/lib/easyai/rag/
```

The dir is tiny (KB-scale typically). Toss it in your normal backup.

### Sharing an entry between machines

```bash
scp /var/lib/easyai/rag/important.md other-host:/var/lib/easyai/rag/
sudo systemctl restart easyai-server  # on the other host (picks up new file)
```

### Auditing what the agent has saved

```bash
ls -lt /var/lib/easyai/rag/ | head -20      # newest first
grep -l "user-prefs" /var/lib/easyai/rag/*.md
cat /var/lib/easyai/rag/<entry>.md
```

### Pruning old entries

```bash
# entries not modified in 6 months
find /var/lib/easyai/rag/ -name "*.md" -mtime +180 -ls
# delete after review
find /var/lib/easyai/rag/ -name "*.md" -mtime +180 -delete
```

(Or use the model: tell it to `knowledge_list` and `knowledge_delete`
things it considers stale.)

### Bulk-importing notes

Drop hand-authored `.md` files into the dir. Each must follow the
format:

```
keywords: tag1, tag2

body
```

Restart the server. The model picks them up on next
`knowledge_search` / `knowledge_list`.

---

## 10. Roadmap

The `knowledge` tools today are the simplest thing that could work —
a passive RAG technique and nothing more. Future evolutions that fit
cleanly on top:

### Progressive recall on session start

The system prompt currently doesn't include any memory content.
Future: on session start, automatically load the K most-relevant
entries (by some heuristic — recency, keyword overlap with the current
prompt, semantic similarity if we add embeddings). This makes the
agent immediately aware of its own memory without needing a
conscious `knowledge_search`.

### Document ingestion helper

Today the model has to chunk a document into knowledge entries by
hand. Future: a `knowledge_ingest` tool that takes a long text,
segments it (semantic boundaries, fixed-size chunks, or model-driven
topics), and saves each chunk as an entry — returning a manifest of
what was saved.

### Cross-entry references

Today entries are independent. Future: a soft reference syntax
(e.g. `see-also: stem1, stem2` in the header) so the model can
build small knowledge graphs and `knowledge_load` resolves them
transitively.

### Entry expiry

Today entries live forever. Future: optional `expires:` header so
seasonal / temporary knowledge auto-cleans without operator action.

### Multi-user / multi-namespace

Today one process owns one RAG dir. Future: a per-user namespace
(scope by client id, by API key, by something) so a multi-tenant
server can give every user their own knowledge store without leaking.

### Encryption at rest

Today the dir is mode 750 — OS-level access control. Future:
optional symmetric encryption with a key from `EASYAI_REG_KEY` env
var, for sensitive deployments.

---

## 11. Troubleshooting

### "memory enabled" never appears in the journal

The systemd unit isn't passing `--memory`. Check:

```bash
systemctl cat easyai-server | grep -E -- '--memory|--RAG'
```

If missing, re-run `install_easyai_server.sh --upgrade --enable-now`
to refresh the unit.

### Model doesn't seem to use the `knowledge` tools

Two possible causes:

1. **The tools aren't registered.** Confirm:
   ```bash
   curl http://127.0.0.1:8080/health | jq .tool_count
   # should include the seven knowledge_* tools
   ```
   If not, re-check the `--memory` flag is reaching the binary.

2. **The model isn't being prompted to use them.** The descriptions
   already encourage it, but a strong system prompt overrides
   defaults. If your `system.txt` says "do not call tools", the
   model honours that. Edit `/etc/easyai/system.txt` to clarify
   that the `knowledge` tools are encouraged.

### "RAG root is not a directory"

A file exists at the path. Move it out of the way and recreate the
dir:

```bash
sudo mv /var/lib/easyai/rag /var/lib/easyai/rag.broken
sudo install -d -o easyai -g easyai -m 750 /var/lib/easyai/rag
sudo systemctl restart easyai-server
```

### "create RAG dir failed: Permission denied"

The agent runs as `easyai`. Verify:

```bash
ls -ld /var/lib/easyai/
# drwxr-x--- root easyai
ls -ld /var/lib/easyai/rag
# drwxr-x--- easyai easyai   (note: easyai, not root)
```

If owner is wrong, fix:

```bash
sudo chown -R easyai:easyai /var/lib/easyai/rag
sudo chmod 750 /var/lib/easyai/rag
```

### Entries vanish between sessions

Likely the agent ran with the wrong `--memory` path (e.g. CLI vs
server disagree). Confirm both invocations point at the same dir.

### Entry was saved but `knowledge_search` doesn't find it

Check the on-disk file:

```bash
cat /var/lib/easyai/rag/<stem>.md
```

The first line must be `keywords: <comma-separated>`. If the model
forgot keywords, the entry is untagged — visible in
`knowledge_list`, not `knowledge_search`.

### `cat`-ing an entry shows weird characters

The body is UTF-8 with no escaping. If you see `\n`, that's the
model's mistake — it wrote a literal backslash-n instead of a real
newline. Edit the file by hand to fix it.

---

*See also:* [`easyai-server.md`](easyai-server.md) (full chat-server
config + INI + CLI), [`easyai-mcp-server.md`](easyai-mcp-server.md)
(standalone MCP daemon — the `knowledge` tools work there too with the
same `std::shared_mutex` index for parallel reads),
[`LINUX_SERVER.md`](LINUX_SERVER.md) (operator's guide for the
systemd-installed chat server), [`manual.md`](manual.md) (general
easyai reference), [`EXTERNAL_TOOLS.md`](EXTERNAL_TOOLS.md) (the
operator-defined tools subsystem — different surface, similar
philosophy).
