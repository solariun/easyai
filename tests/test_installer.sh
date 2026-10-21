#!/usr/bin/env bash
# Checks on scripts/install_easyai_server.sh that need no box:
#   1. bash -n parses it;
#   2. the INI template (the unquoted <<INI_FILE heredoc) holds no unescaped
#      backtick: on 2026-10-21 a comment with `easyai-assistant --list-options`
#      ran that command at install time and pasted its table into the INI
#      (every line "missing '=' in entry");
#   3. the self-upgrade guard is present (private copy + one restart).
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="$here/../scripts/install_easyai_server.sh"
fail=0
check() { if "$@"; then echo "ok   $name"; else echo "FAIL $name"; fail=1; fi; }

name="bash -n";              check bash -n "$script"
name="heredoc backticks";    check test "$(awk '/<<INI_FILE$/{p=1;next} /^INI_FILE$/{p=0} p' "$script" | grep -c '[^\\]`')" = 0
name="heredoc single open";  check test "$(grep -cE "<<'?INI_FILE'?$" "$script")" = 1
name="private copy guard";   check grep -q 'EASYAI_INSTALL_COPY="\$easyai_install_copy"' "$script"
name="self-upgrade restart"; check grep -q 'EASYAI_INSTALL_REEXEC=1' "$script"
name="ldconfig keeps links";  check grep -q 'sudo ldconfig -X' "$script"
name="live lib chain only";   check grep -q 'live_libs\["\$(readlink -f "\$so")"\]=1' "$script"
name="--help via the copy";  check test "$(bash "$script" --help | grep -c 'easyai installer')" = 1
exit $fail
