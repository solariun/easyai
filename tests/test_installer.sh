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

# ---- Vulkan: every llama.cpp shader feature compiled in (2026-11-21) -------
eval "$(awk '/^glslc_missing_features\(\) \{/{p=1} p{print} p&&/^}/{exit}' "$script")"
eval "$(awk '/^vulkan_sdk_sha256\(\) \{/{p=1} p{print} p&&/^}/{exit}' "$script")"
mkdir -p "$tmpd/ft"; for t in coopmat integer_dot bfloat16; do echo "// $t" > "$tmpd/ft/$t.comp"; done
printf '#!/bin/sh\nexit 0\n' > "$tmpd/glslc-new"
printf '#!/bin/sh\nfor a; do last="$a"; done\ncase "$last" in *integer_dot*|*bfloat16*) echo "extension not supported" >&2; exit 1 ;; esac\nexit 0\n' > "$tmpd/glslc-old"
chmod +x "$tmpd/glslc-new" "$tmpd/glslc-old"
name="glslc that compiles all: nothing missing"; check test -z "$(glslc_missing_features "$tmpd/glslc-new" "$tmpd/ft")"
name="old glslc: int dot + bf16 missing";        check test "$(glslc_missing_features "$tmpd/glslc-old" "$tmpd/ft" | tr '\n' ' ')" = "bfloat16 integer_dot "
name="no glslc: every feature missing";          check test "$(glslc_missing_features "" "$tmpd/ft" | wc -l)" = 3
vk_default="$(awk -F'"' '/^vulkan_sdk_version=/{print $2; exit}' "$script")"
name="default Vulkan SDK has a sha256 pin";      check test "$(vulkan_sdk_sha256 "vulkansdk-linux-x86_64-$vk_default.tar.xz" | grep -cE '^[0-9a-f]{64}$')" = 1
name="SDK download is sha256-verified";          check grep -q 'verify_sha256 "\$vk_tmp/\$vk_tgz" "\$(vulkan_sdk_sha256 "\$vk_tgz")"' "$script"
name="only glslc is extracted";                  check grep -q 'tar -xJf "\$vk_tmp/\$vk_tgz" -C "\$src_root/vulkan-sdk" "\$vulkan_sdk_version/x86_64/bin/glslc"; then' "$script"
name="glslc passed to cmake";                    check grep -q 'cmake_flags+=( -DVulkan_GLSLC_EXECUTABLE="\$vk_glslc" )' "$script"
name="feature tests of the pinned llama.cpp";    check grep -q 'vk_tests="\$llama_dir/ggml/src/ggml-vulkan/vulkan-shaders/feature-tests"' "$script"
name="--no-vulkan-sdk flag";                     check grep -q -- '--no-vulkan-sdk)    do_vulkan_sdk=0' "$script"

# ---- A build dir follows its glslc (2026-11-22) -----------------------------
# llama.cpp keeps a shader header made by an earlier glslc → ggml-vulkan breaks.
eval "$(awk '/^vulkan_shader_reset\(\) \{/{p=1} p{print} p&&/^}/{exit}' "$script")"
vkb="$tmpd/vkbuild"; vkd="$vkb/_deps/llama.cpp/ggml/src/ggml-vulkan"
out="$(vulkan_shader_reset "$vkb" "/usr/bin/glslc shaderc v2025.2")"
name="fresh build dir: nothing removed";          check test -z "$out"
name="fresh build dir: glslc recorded";           check test "$(cat "$vkb/.easyai-vulkan-glslc")" = "/usr/bin/glslc shaderc v2025.2"
mkdir -p "$vkd"; echo old > "$vkd/ggml-vulkan-shaders.hpp"
out="$(vulkan_shader_reset "$vkb" "/usr/bin/glslc shaderc v2025.2")"
name="same glslc: nothing removed";               check test -z "$out" -a -f "$vkd/ggml-vulkan-shaders.hpp"
out="$(vulkan_shader_reset "$vkb" "/sdk/glslc shaderc v2026.4")"
name="other glslc: Vulkan dir removed";           check test ! -e "$vkd"
name="other glslc: says so";                      check grep -q 'regenerating every Vulkan shader' <<<"$out"
name="other glslc: new one recorded";             check test "$(cat "$vkb/.easyai-vulkan-glslc")" = "/sdk/glslc shaderc v2026.4"
name="other glslc: sibling build output kept";    check test -d "$vkb/_deps/llama.cpp"
mkdir -p "$vkd"; rm "$vkb/.easyai-vulkan-glslc"
out="$(vulkan_shader_reset "$vkb" "/sdk/glslc shaderc v2026.4")"
name="no record (pre-2026-11-22 dir): reset once"; check test ! -e "$vkd"
mkdir -p "$vkd"
out="$(vulkan_shader_reset "$vkb" "/usr/bin/glslc shaderc v2025.2")"
name="back to fewer features: reset";             check test -n "$out" -a ! -e "$vkd"
name="installer resets before configuring";      check grep -q 'vk_reset="\$(vulkan_shader_reset "\$easyai_dir/build" "\${vk_glslc:-none} \${vk_ver}")"' "$script"

# ---- Lemonade Server is no longer installed (2026-11-21) -------------------
name="no Lemonade PPA";             check test "$(grep -c 'lemonade-team' "$script")" = 0
name="no lemonade-server install";  check test "$(grep -cE 'apt-get install .*lemonade' "$script")" = 0
name="no do_lemonade switch";       check test "$(grep -c 'do_lemonade' "$script")" = 0
# Old command lines still run: the two flags are accepted and ignored under set -euo pipefail.
awk '/^while \[\[ \$# -gt 0 \]\]; do$/{p=1} p{print} p&&/^done$/{exit}' "$script" > "$tmpd/argloop.sh"
name="--no-lemonade / --with-lemonade accepted"; check bash -c 'set -euo pipefail; f="$1"; set -- --no-lemonade --with-lemonade; source "$f" 2>/dev/null; test $# -eq 0' _ "$tmpd/argloop.sh"

# ---- CI builds the installer's pinned llama.cpp (2026-11-21) ---------------
wf="$here/../.github/workflows"
ci_clone="$here/../scripts/ci_clone_llama.sh"
name="installer pins a 40-hex llama_ref"; check test "$(grep -cE '^llama_ref="[0-9a-f]{40}"' "$script")" = 1
name="ci_clone_llama.sh reads that pin"; check test "$(sed -n 's/^llama_ref="\([0-9a-f]\{40\}\)".*/\1/p' "$script" | head -1)" = "$(grep -oE '^llama_ref="[0-9a-f]{40}"' "$script" | grep -oE '[0-9a-f]{40}')"
name="ci_clone_llama.sh parses";       check bash -n "$ci_clone"
name="no unpinned llama.cpp clone in CI"; check test "$(grep -hE 'git clone .*llama\.cpp' "$wf"/*.yml | wc -l)" = 0
name="every CI job uses the pinned clone"; check test "$(grep -h 'bash scripts/ci_clone_llama.sh ../llama.cpp' "$wf"/*.yml | wc -l)" = 6
exit $fail
