#!/usr/bin/env bash
# Checks on scripts/install_easyai_server.sh that need no box:
#   1. bash -n parses it;
#   2. the INI template (the unquoted <<INI_FILE heredoc) holds no unescaped
#      backtick: on 2026-10-21 a comment with `easyai-assistant --list-options`
#      ran that command at install time and pasted its table into the INI
#      (every line "missing '=' in entry");
#   3. the self-upgrade guard is present (private copy + one restart);
#   4. the assistant is built into easyai-server (2026-11-14): no second unit
#      is written, the INI template carries [ASSISTANT] enabled, the server
#      unit has the audio group, the stale binary + units are purged, the
#      tests are built and run, the downloads are sha256-verified and the
#      Kokoro voice is checked with --list-voices.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="$here/../scripts/install_easyai_server.sh"
fail=0
check() { if "$@"; then echo "ok   $name"; else echo "FAIL $name"; fail=1; fi; }
ini_template() { awk '/<<INI_FILE$/{p=1;next} /^INI_FILE$/{p=0} p' "$script"; }
server_unit()  { awk '/<<UNIT$/{p=1;next} /^UNIT$/{p=0} p' "$script"; }

name="bash -n";              check bash -n "$script"
name="heredoc backticks";    check test "$(ini_template | grep -c '[^\\]`')" = 0
name="heredoc single open";  check test "$(grep -cE "<<'?INI_FILE'?$" "$script")" = 1
name="private copy guard";   check grep -q 'EASYAI_INSTALL_COPY="\$easyai_install_copy"' "$script"
name="self-upgrade restart"; check grep -q 'EASYAI_INSTALL_REEXEC=1' "$script"
name="ldconfig keeps links";  check grep -q 'sudo ldconfig -X' "$script"
name="live lib chain only";   check grep -q 'live_libs\["\$(readlink -f "\$so")"\]=1' "$script"
name="--help via the copy";  check test "$(bash "$script" --help | grep -c 'easyai installer')" = 1

# ---- the assistant lives inside easyai-server ------------------------------
name="no AUNIT heredoc";        check test "$(grep -c '<<AUNIT' "$script")" = 0
name="no assistant unit tee";   check test "$(grep -cE 'tee .*easyai-assistant\.service|\$assistant_service' "$script")" = 0
name="no assistant binary install"; check test "$(grep -c 'install .*easyai-assistant' "$script")" = 0
name="INI [ASSISTANT] enabled"; check test "$(ini_template | grep -cE '^enabled *=')" = 1
name="server unit audio group"; check test "$(server_unit | grep -cE '^SupplementaryGroups=.*\baudio\b')" = 1
name="server unit sound.target"; check test "$(server_unit | grep -cE '^After=.*sound\.target')" = 1
name="usermod audio";           check grep -q 'usermod -aG audio "\$service_user"' "$script"
name="purge names old units";   check grep -q 'for unit in easyai-assistant.service easyai-tts-kokoro.service' "$script"
name="purge names old binary";  check grep -q 'sudo rm -f "\$install_prefix/bin/easyai-assistant"' "$script"
name="polkit server unit only"; check test "$(grep -c 'unit == "\$service_name" ||' "$script")" = 0
name="tests built";             check grep -q -- '-DEASYAI_BUILD_TESTS=ON' "$script"
name="tests run (ctest)";       check grep -q 'ctest --test-dir build --output-on-failure' "$script"
name="voice check --list-voices"; check grep -q -- '--config "\$ini_file" --list-voices' "$script"
name="sha256 helper defined";   check grep -q '^verify_sha256()' "$script"
name="sha256 helper used >= 6"; check test "$(grep -cE '^[^#]*\bverify_sha256 ' "$script")" -ge 6
name="espeak runtime lib";      check grep -q 'libespeak-ng1' "$script"
name="libeasyai chain purged";  check grep -q '"\$LIB_DEST"/libeasyai\*\.so\*' "$script"
name="libeasyai chain copied";  check grep -q "\-name 'libeasyai\*\.so\*'" "$script"
name="no bare libeasyai.so install"; check test "$(grep -c 'install -Dm644 .*libeasyai\.so"' "$script")" = 0
name="post-install self-test";  check grep -q 'LD_LIBRARY_PATH="\$LIB_DEST" "\$install_prefix/bin/easyai-server" --config /dev/null --assistant-options' "$script"
name="re-exec the checkout installer"; check grep -q 'exec bash "\$checkout_installer"' "$script"
name="reset-failed before start"; check test "$(grep -c 'systemctl reset-failed "\$service_name"' "$script")" -ge 2
name="purge drops wants links"; check grep -qF '/etc/systemd/system/*.wants/"$unit"' "$script"
name="espeak never automatic";  check test "$(grep -c 'engine = espeak after apt' "$script")" = 0
name="model ownership pass";    check grep -q 'chown -h -c "\$service_user":"\$service_group"' "$script"
name="migrate name easy";       check grep -q '"\$ini_asst_name" == "easy"' "$script"
name="migrate wake tiny";       check grep -q 'ggml-tiny\\.bin' "$script"
name="--assistant-name on upgrade"; check grep -q 'assistant_name_set=1' "$script"

# ---- models larger than memory: the keys are in the INI (2026-11-21) -------
name="INI template memory_mode";  check test "$(ini_template | grep -cE '^memory_mode *= *auto$')" = 1
name="INI template expert_cache"; check test "$(ini_template | grep -cE '^expert_cache *= *auto$')" = 1
name="INI template no_expert_prefetch"; check test "$(ini_template | grep -cE '^#no_expert_prefetch *= *on$')" = 1
name="upgrade calls ini_add_engine_keys"; check grep -q 'declare -f ini_add_engine_keys); ini_add_engine_keys' "$script"
# Run the real helper (extracted from the installer) on temp INIs.
eval "$(awk '/^ini_add_engine_keys\(\) \{/{p=1} p{print} p&&/^}/{exit}' "$script")"
tmpd="$(mktemp -d)"; trap 'rm -rf "$tmpd"' EXIT
engine_block() { awk '/^[[:space:]]*\[/{s=($0 ~ /^[[:space:]]*\[ENGINE\]/); next} s' "$1"; }
# 1. an old INI: [ENGINE] without the keys, a [MODEL_*] that has one
printf '[SERVER]\nport = 80\n\n[ENGINE]\nngl = 99\nmlock = on\n\n[MODEL_Big]\nmemory_mode = paged\n' > "$tmpd/a.ini"
out="$(ini_add_engine_keys "$tmpd/a.ini")"
name="upgrade adds the three keys"; check test "$out" = "memory_mode expert_cache no_expert_prefetch"
name="added inside [ENGINE]";       check test "$(engine_block "$tmpd/a.ini" | grep -cE '^memory_mode *= *auto$|^expert_cache *= *auto$|^#no_expert_prefetch *= *on$')" = 3
name="[ENGINE] values kept";        check test "$(engine_block "$tmpd/a.ini" | grep -cE '^ngl = 99$|^mlock = on$')" = 2
name="[MODEL_*] untouched";         check grep -q '^memory_mode = paged$' "$tmpd/a.ini"
cp "$tmpd/a.ini" "$tmpd/a2.ini"; out="$(ini_add_engine_keys "$tmpd/a.ini")"
name="second upgrade adds nothing"; check test -z "$out"
name="second upgrade same bytes";   check cmp -s "$tmpd/a.ini" "$tmpd/a2.ini"
# 2. an operator's own value (and a commented key) is never touched
printf '[ENGINE] ; tuned\nmemory_mode = gpu\n;expert_cache = 4096\n' > "$tmpd/b.ini"
out="$(ini_add_engine_keys "$tmpd/b.ini")"
name="only the missing key added";  check test "$out" = "no_expert_prefetch"
name="operator value kept";         check test "$(grep -c '^memory_mode = gpu$' "$tmpd/b.ini")" = 1
name="no second memory_mode";       check test "$(grep -cE '^[#;]?memory_mode' "$tmpd/b.ini")" = 1
# 3. no [ENGINE] at all → one is appended
printf '[SERVER]\nport = 80\n' > "$tmpd/c.ini"
ini_add_engine_keys "$tmpd/c.ini" >/dev/null
name="[ENGINE] appended";           check test "$(grep -c '^\[ENGINE\]$' "$tmpd/c.ini")" = 1
name="appended keys in [ENGINE]";   check test "$(engine_block "$tmpd/c.ini" | grep -cE '^memory_mode|^expert_cache|^#no_expert_prefetch')" = 3
exit $fail
