#!/usr/bin/env bash
# Every command-line flag that has an INI key (a kFlags() row in
# services/server.cpp with a section) must be:
#   1. written, commented out or not, in its own section of
#      resources/easyai.ini.example, the reference INI;
#   2. named in easyai-server.md, the reference of every key.
# Rows with no CLI flag (legacy INI-only aliases such as `rag`) are skipped.
# Added 2026-11-21 when memory_mode / expert_cache / no_expert_prefetch and
# five older keys (tls_cert, tls_key, data_dir, catalog_size,
# reasoning_effort) were found missing from the example.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../services/server.cpp"
ini="$here/../resources/easyai.ini.example"
doc="$here/../easyai-server.md"
fail=0; n=0

rows="$(awk '/static const std::vector<FlagDef> & kFlags\(\) \{/{p=1} p{print} p&&/return table;/{exit}' "$src" \
        | grep -oE '\{ *\{[^}]*\}, *"[A-Z_]+", *"[a-z0-9_]+"' \
        | sed -E 's/^\{ *\{([^}]*)\}, *"([A-Z_]+)", *"([a-z0-9_]+)"$/\2 \3 \1/')"
[[ -n "$rows" ]] || { echo "FAIL no kFlags() rows parsed from $src"; exit 1; }

section_lines() { awk -v s="$1" '/^[[:space:]]*\[/{in_s = ($0 ~ "^[[:space:]]*\\[" s "\\]"); next} in_s' "$ini"; }

while read -r sec key cli; do
    [[ -z "$cli" ]] && continue
    n=$((n + 1))
    if ! section_lines "$sec" | grep -qE "^[#;[:space:]]*$key[[:space:]]*="; then
        echo "FAIL [$sec] $key ($cli) is not in resources/easyai.ini.example"; fail=1
    fi
    if ! grep -qw -- "$key" "$doc"; then
        echo "FAIL [$sec] $key ($cli) is not in easyai-server.md"; fail=1
    fi
done <<< "$rows"

[[ $n -ge 80 ]] || { echo "FAIL only $n flag rows parsed (expected 80+)"; fail=1; }
[[ $fail -eq 0 ]] && echo "ok   $n INI keys in the example and in easyai-server.md"
exit $fail
