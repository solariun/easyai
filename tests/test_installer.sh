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
exit $fail
