#!/usr/bin/env bash
# ============================================================================
# easyai installer — replaces install_llama_server.sh
#
# What this script does:
#   1. Installs build deps (CMake/Ninja/git/pkg-config + libcurl) and the
#      backend SDK that matches your hardware (Vulkan / CUDA / ROCm-HIP).
#   2. Clones llama.cpp + easyai (or uses existing sibling dirs).
#   3. Builds easyai (libeasyai + easyai-local + easyai-cli + easyai-server
#      + easyai-agent + easyai-recipes + easyai-chat) with the selected
#      GPU backend.
#   4. Installs the binaries to $prefix/bin.
#   5. Creates a system user, /var/lib/easyai/{models,workspace,data}, and
#      /etc/easyai/{easyai.ini, system.txt_template, api_key} —
#      out-of-the-box uses the binary's built-in "Deep" prompt; copy
#      system.txt_template to system.txt to activate a custom persona.
#   6. Drops a hardened systemd unit that runs easyai-server with mlock,
#      flash-attn, q8_0 KV cache, Bearer auth, Prometheus /metrics, and
#      coredump capture (LimitCORE=infinity + systemd-coredump package).
#   7. (Linux only, optional) AMD-iGPU GTT kernel cmdline tweak; mDNS via
#      avahi; memlock+nofile limits; system swap off.
#
# What this script does NOT need to do (vs the old llama-server installer):
#   - No transparent proxy: easyai's HTTP layer already does the OpenAI-
#     compatible /v1/chat/completions itself.
#   - No SearXNG: search_web is a built-in tool that scrapes DuckDuckGo
#     directly via libcurl.
#   - No MCP bridge: tools live inside easyai-server and are auto-registered.
#   - No webui rebrand: the webui is a self-contained file embedded in the
#     binary (--webui-title would have nothing to patch).
#
# Linux / Debian target. The build itself works anywhere; this *installer*
# uses apt-get + systemd. macOS users: see the project README for the manual
# build matrix.
#
# Usage:
#   ./install_easyai_server.sh                       # full setup
#   ./install_easyai_server.sh --model /path/to.gguf # required for first run
#   ./install_easyai_server.sh --backend vulkan      # force backend
#   ./install_easyai_server.sh --service-port 8080
#   ./install_easyai_server.sh --service-host 0.0.0.0
#   ./install_easyai_server.sh --mdns-hostname my-ai # box becomes my-ai.local
#                                                    # default: current system
#                                                    # hostname (so <host>.local
#                                                    # — e.g. ai-pro → ai-pro.local)
#                                                    # ignored under --no-avahi
#   ./install_easyai_server.sh --ctx-size 32768   # default 524288 (512 K)
#   ./install_easyai_server.sh --ngl 99            # GPU layers (-1=auto, 0=CPU)
#   ./install_easyai_server.sh --no-mlock --use-mmap
#   ./install_easyai_server.sh --temperature 0.2 --top-k 50 --min-p 0.03
#   ./install_easyai_server.sh --preset auto --reasoning-effort max  # [ENGINE] defaults
#   ./install_easyai_server.sh --repeat-penalty 1.04 --frequency-penalty 0.05
#   ./install_easyai_server.sh --rope-scaling yarn --rope-scale 2 --yarn-orig-ctx 131072
#   ./install_easyai_server.sh --split-mode none       # none|layer|row|tensor
#   ./install_easyai_server.sh --http-timeout 86400   # default 24h, matches cli
#   ./install_easyai_server.sh --webui-title "AI Box"
#   ./install_easyai_server.sh --webui-icon /path/to/logo.svg   # ico|png|svg|gif|jpg|webp
#   ./install_easyai_server.sh --webui-password 's3cret'  # /models gate (default 0000)
#   ./install_easyai_server.sh --upgrade             # git pull + rebuild
#   ./install_easyai_server.sh --force               # CLEAN-SLATE rewrite:
#                                                    #   - easyai.ini backed up
#                                                    #     to .bak then replaced
#                                                    #   - systemd unit stopped,
#                                                    #     disabled, removed
#                                                    #   - ENTIRE drop-in dir
#                                                    #     (.service.d/*) wiped
#                                                    #     incl. operator-edited
#                                                    #     override.conf
#                                                    #   - systemctl reset-failed
#                                                    #     so a fresh unit isn't
#                                                    #     blocked by prior
#                                                    #     StartLimitBurst gate
#                                                    #   - new unit written
#                                                    #   - daemon-reload
#                                                    # Use this after changing
#                                                    # installer defaults OR to
#                                                    # recover a corrupted unit.
#                                                    # system.txt is NOT touched
#                                                    # (only the template ships).
#   ./install_easyai_server.sh --enable-now          # systemctl start now
#   ./install_easyai_server.sh --enable-verbose      # bake --verbose into ExecStart (noisy)
#   ./install_easyai_server.sh --no-llama-tools      # skip the llama.cpp tool
#                                                    # binaries (llama-cli,
#                                                    # llama-server,
#                                                    # llama-gguf-split,
#                                                    # llama-quantize,
#                                                    # llama-bench, ...).
#                                                    # default: they are
#                                                    # built and installed to
#                                                    # $prefix/bin alongside
#                                                    # easyai-server.
#   ./install_easyai_server.sh --mtp                 # bake --spec-type draft-mtp
#                                                    # --spec-draft-n-max 6 into
#                                                    # ExecStart. Only for MTP-
#                                                    # trained models (DeepSeek
#                                                    # V3, MimoVL, etc.); plain
#                                                    # models won't load.
#   ./install_easyai_server.sh --mtp --mtp-n-max 8   # override the draft window
#   ./install_easyai_server.sh --no-lemonade         # skip the Lemonade Server
#                                                    # install. Default: install
#                                                    # Lemonade Server (AMD's
#                                                    # NPU-capable LLM runtime)
#                                                    # alongside easyai-server,
#                                                    # but leave its systemd
#                                                    # unit DISABLED — operator
#                                                    # starts it manually via
#                                                    # `lemonade-server serve
#                                                    # --no-tray`. Default port
#                                                    # 13305. Ubuntu-only path
#                                                    # (PPA); on Debian we warn
#                                                    # and skip with a pointer
#                                                    # to the manual build.
#   ./install_easyai_server.sh --no-rocm-install     # skip auto-install of the
#                                                    # ROCm SDK when --backend
#                                                    # hip is used. Default:
#                                                    # ON for AMD-CPU boxes —
#                                                    # the installer adds the
#                                                    # repo.radeon.com APT repo
#                                                    # (noble pinned on non-LTS
#                                                    # Ubuntu like 25.10), then
#                                                    # apt-installs the minimal
#                                                    # HIP set (rocm-hip-runtime
#                                                    # + rocm-hip-sdk + rocblas
#                                                    # + hipblas + rocm-device-
#                                                    # libs, ~5 GB, NOT the full
#                                                    # ~25 GB ROCm meta-pkg).
#                                                    # /etc/profile.d/rocm.sh
#                                                    # is dropped so /opt/rocm/
#                                                    # bin is on PATH for every
#                                                    # shell. AMDGPU_TARGETS is
#                                                    # auto-detected from
#                                                    # rocminfo and passed to
#                                                    # cmake (e.g. gfx1151 on
#                                                    # Strix Point / 890M).
#                                                    # Skipped on non-AMD CPU,
#                                                    # non-hip backend, non-
#                                                    # Ubuntu distro, or if
#                                                    # hipcc is already on PATH.
#   ./install_easyai_server.sh --rocm-version 6.4    # override ROCm repo version
#                                                    # (default 6.4). Bump when
#                                                    # newer ROCm lands or pin
#                                                    # to an older series for
#                                                    # reproducibility.
#   ./install_easyai_server.sh --no-tdp-unlock       # skip the Ryzen TDP unlock.
#                                                    # Default: ON for AMD Ryzen
#                                                    # CPUs — the installer
#                                                    # builds ryzenadj from
#                                                    # source (no apt package
#                                                    # exists), drops a systemd
#                                                    # oneshot + a 60 s timer
#                                                    # under easyai-tdp.{service,
#                                                    # timer}, and reapplies
#                                                    # STAPM/PPT/SLOW/FAST = 54 W
#                                                    # plus Tctl = 95 °C on every
#                                                    # boot AND every 60 s
#                                                    # (ryzenadj limits drift back
#                                                    # after C6/sleep on some
#                                                    # platforms — the timer
#                                                    # keeps them pinned). The
#                                                    # 54 W cap matches the
#                                                    # HX 370's spec limit;
#                                                    # check `sensors` under
#                                                    # load and back off via
#                                                    # --tdp-watts if the box
#                                                    # throttles past 95 °C.
#                                                    # Skipped on non-AMD CPUs.
#   ./install_easyai_server.sh --tdp-watts 45        # cap STAPM/PPT/SLOW/FAST
#                                                    # in watts (default 54).
#                                                    # ryzenadj uses mW under
#                                                    # the hood — we multiply
#                                                    # by 1000 for you.
#   ./install_easyai_server.sh --tdp-tctl 90         # Tctl junction temp cap
#                                                    # in °C (default 95). Lower
#                                                    # values throttle sooner;
#                                                    # raise only if cooling is
#                                                    # confirmed adequate.
#   ./install_easyai_server.sh --no-service          # build/install only
#   ./install_easyai_server.sh -h                    # show this help
# ============================================================================

set -euo pipefail

# ---------- defaults --------------------------------------------------------
src_root="${src_root:-$HOME/opt}"
easyai_dir="$src_root/easyai"
llama_dir="$src_root/llama.cpp"
easyai_repo="${easyai_repo:-https://github.com/solariun/easy.git}"
llama_repo="${llama_repo:-https://github.com/ggml-org/llama.cpp.git}"
easyai_ref=""                                 # empty = main; pass --ref <sha|tag>
llama_ref=""                                  # empty = main; pass --llama-ref

install_prefix="/usr"
backend="auto"                                # auto|vulkan|cuda|hip|cpu
gtt_gb=29                                     # AMD iGPU GTT (only used by RDNA2/iGPU)
jobs="$(nproc 2>/dev/null || echo 4)"

do_install=1                                  # apt-get install deps
do_build=1
do_groups=1                                   # add user to render+video
do_limits=1                                   # /etc/security/limits.d for mlock
do_swap="off"                                 # off|tune|"" (keep)
do_kernel=1                                   # AMD iGPU GTT cmdline
do_service=1
do_force_service=0
do_force=0                                    # --force: superset of
                                              # --force-service; also rewrites
                                              # /etc/easyai/easyai.ini even if
                                              # it already exists.  Use after
                                              # changing installer defaults to
                                              # propagate them to the box.
                                              # The active system.txt is no
                                              # longer installed — only the
                                              # template — so --force does not
                                              # touch it.
do_enable_now=0
do_avahi=1
do_presets=1                                  # symlink easyai-cli → /usr/bin/ai
do_model=1
do_upgrade=0
copy_model=0
do_lemonade=1                                 # install Lemonade Server (AMD's
                                              # NPU-capable LLM runtime) via
                                              # the lemonade-team PPA on
                                              # Ubuntu. Skipped on non-Ubuntu
                                              # (Debian etc.) with a pointer
                                              # to the upstream manual build.
                                              # The systemd unit shipped by
                                              # the PPA is DISABLED + STOPPED
                                              # after install so the operator
                                              # runs Lemonade by hand — that
                                              # was the user's explicit ask
                                              # (manual NPU experimentation,
                                              # no auto-start). Default port
                                              # is upstream 13305 (not
                                              # configurable here). Set 0 via
                                              # --no-lemonade.
do_rocm_install=1                             # auto-install minimal ROCm SDK
                                              # (rocm-hip-runtime + rocm-hip-
                                              # sdk + rocblas + hipblas +
                                              # rocm-device-libs) when CPU is
                                              # AMD AND --backend hip ends up
                                              # selected. Adds repo.radeon.com
                                              # APT repo (pinned to noble on
                                              # non-LTS Ubuntu like 25.10
                                              # because AMD only publishes for
                                              # LTS). The installer's existing
                                              # "assuming rocm-dev installed"
                                              # warn becomes a real install.
                                              # Set 0 via --no-rocm-install.
rocm_version="6.4"                            # ROCm series for the APT repo
                                              # URL. Bump as new releases land
                                              # (https://repo.radeon.com/rocm/
                                              # apt/<ver>) or override with
                                              # --rocm-version.
do_tdp_unlock=1                               # auto-unlock TDP on AMD Ryzen.
                                              # Builds ryzenadj from source
                                              # (no apt package), installs to
                                              # /usr/local/bin, drops a oneshot
                                              # systemd unit + 60 s timer that
                                              # reapplies the limits on every
                                              # boot and every 60 s (ryzenadj
                                              # values can drift back after
                                              # C6/sleep on some platforms;
                                              # the timer keeps them pinned).
                                              # Skipped on non-AMD CPUs. Set
                                              # 0 via --no-tdp-unlock.
tdp_watts=54                                  # STAPM / PPT-SLOW / PPT-FAST
                                              # cap in W (HX 370 spec limit).
                                              # Converted to mW for ryzenadj.
                                              # Override with --tdp-watts.
tdp_tctl=95                                   # Tctl junction temp cap in °C.
                                              # 95 = chip max; lower throttles
                                              # earlier. Override --tdp-tctl.
do_llama_tools=1                              # also build + install
                                              # llama.cpp's CLI tools
                                              # (llama-cli, llama-server,
                                              # llama-gguf-split,
                                              # llama-quantize, llama-bench,
                                              # llama-tokenize,
                                              # llama-imatrix,
                                              # llama-perplexity, ...) so
                                              # the AI box has the full
                                              # llama.cpp tool surface
                                              # next to easyai-server. Set
                                              # to 0 via --no-llama-tools
                                              # for a faster rebuild when
                                              # iterating on easyai itself.

# easyai-server runtime config (compiled into the unit file)
service_user="easyai"
service_group="easyai"
service_home="/var/lib/easyai"
service_model_dir="$service_home/models"
service_model_link="ai.gguf"
service_workspace="$service_home/workspace"
service_data_dir="$service_home/data"          # /models dashboard catalog cache (easyai_hf_catalog.json)
service_host="0.0.0.0"
service_port=80                                # matches install_llama_server.sh default
service_alias="EasyAi"
service_name="easyai-server.service"
# mDNS / kernel hostname. We rename the system hostname here so the box
# advertises as `<mdns_hostname>.local` via avahi. Default mirrors the
# current system hostname so the box keeps the name the operator has
# already given it (e.g. host `ai-pro` → `ai-pro.local`, host `ai` →
# `ai.local`, host `xx` → `xx.local`). Pass --mdns-hostname to override.
# Skipped entirely when --no-avahi is passed (operator keeps their
# existing hostname and falls back to LAN-IP).
mdns_hostname="$(hostname -s 2>/dev/null || hostname)"

config_dir="/etc/easyai"
# By default the binary's built-in "Deep" prompt wins (no system_file
# set in the INI).  We drop a documented TEMPLATE next to it that the
# operator can copy/edit/rename to take over: `system.txt_template`.
# Out-of-the-box: only the template ships; the active system.txt is
# NOT created so the built-in default persona is what the server uses.
# Operators who want a custom prompt:
#     sudo cp system.txt_template system.txt
#     sudo $EDITOR system.txt                # tweak as needed
#     # uncomment SERVER.system_file in /etc/easyai/easyai.ini
#     sudo systemctl restart easyai-server
system_template_file="$config_dir/system.txt_template"
system_file="$config_dir/system.txt"  # NOT created by default; path used by INI hint only
api_key_file="$config_dir/api_key"
external_tools_dir="$config_dir/external-tools"
ini_file="$config_dir/easyai.ini"
# Central INI config — auth for /mcp ([MCP_USER]) plus reserved sections
# for future server-wide config. Always passed to the server via
# --config; missing-file = MCP open (default for fresh installs).
# We always pass --external-tools to the server. An empty dir is a
# normal state (no extra tools); operators add EASYAI-*.tools files
# without touching the systemd unit.

# RAG — the agent's persistent registry / long-term memory.
# Lives under /var/lib (mutable state) rather than /etc (config),
# because the AGENT writes here at runtime — operator config goes
# in /etc, agent-generated state goes in /var/lib (FHS).
rag_dir="/var/lib/easyai/rag"

# 512 K context window — sized for long agentic flows, deep research,
# and whole-codebase work on the AI box.  Exactly the window a 128 K-
# trained model reaches under the --rope-scaling yarn + --rope-scale 4
# + --yarn-orig-ctx 131072 defaults below (128 K × 4 = 512 K).  The
# shipped [MODEL_*] profiles inherit this; override with --ctx-size.
ctx_size=524288
# --ngl 99: force all layers onto GPU.  The research/coding agent
# workload assumes a GPU with enough VRAM to hold the full model.
# Use --ngl -1 for auto-fit or --ngl 0 for CPU-only.
ngl=99
webui_title="EasyAi"                          # --webui-title <text>
webui_icon=""                                 # --webui-icon <path/to/.ico|.png|.svg|.gif|.jpg|.webp>
webui_icon_dest="$config_dir/favicon"         # final installed path under /etc/easyai
webui_password="0000"                         # --webui-password <text>; gates the /models
                                              # dashboard. Default 0000 — CHANGE for
                                              # anything past a trusted LAN. Empty = open.
# Threads: 8 (sweet spot for Strix Point / Ryzen AI 9 HX 370 with most
# layers on iGPU). The production AI box was hardcoded at 16 originally;
# llama-bench on the 890M with an 80B MoE running ngl=99 reproduced the
# same tps at 8 threads as at 16 — the extra 8 just contend with
# sampling / KV management / network paths that still run on CPU.
# Operators with dense models running CPU-side (or no GPU offload at
# all) should override to physical core count via --threads /
# --threads-batch.
n_threads_default=8
n_threads_batch_default=8
preset="auto"                                 # written ACTIVE into [ENGINE]. "auto" =
                                              # impose no preset (model default); clients
                                              # drive sampling. A CONCRETE preset
                                              # (precise/balanced/creative/wild/
                                              # deterministic) is authoritative — it
                                              # overrides per-request temperature/top_p/
                                              # top_k and inline presets.
reasoning_effort="max"                        # written ACTIVE into [ENGINE]. auto|low|
                                              # medium|high|max|minimal. "auto" omits the
                                              # field (model default). A per-request
                                              # reasoning_effort body field always wins.
thinking="on"
enable_metrics=1
enable_flash_attn=1
enable_verbose=1                              # writes verbose=on into the INI;
                                              # operator's primary debug switch
mtp=0                                         # --mtp: bake --spec-type draft-mtp
                                              # --spec-draft-n-max 6 into ExecStart
                                              # (only meaningful for MTP-trained
                                              # models — DeepSeek V3, MimoVL, etc.)
mtp_n_max=6                                   # --mtp-n-max <n>: override the draft
                                              # window when --mtp is on
cache_type_k="q8_0"                           # K cache: q8_0 — attention scores
                                              # need precision; quantizing K hurts
                                              # more than V
cache_type_v="q8_0"                           # V cache: q8_0 too — symmetric with K.
                                              # The asymmetric q8/q4 split saves
                                              # ~25 % KV memory but loses quality
                                              # on long agentic flows; production
                                              # tuning on the AI box settled on
                                              # symmetric q8 for both halves
mlock=1                                       # pin small CPU residue (embeddings,
                                              # scratch) — most weights are on GPU.
# no_mmap=1 forces an anonymous-memory load of the GGUF (no kernel
# mmap).  Paired with mlock=1, every weight page is pinned in RAM with
# no file-backing — eliminates the kernel's hint-driven eviction
# during long-running agentic sessions on the production AI box.
# Costs a few GB of load-time RSS peak but stabilises latency once
# warm.  Set no_mmap=0 to reclaim that peak on hosts where RAM is
# tight or the model is loaded from a slow disk.
no_mmap=1
# HTTP read+write timeout for the listen socket AND the MCP-client connection.
# 86400 (24 h) matches easyai-cli's default --timeout, so multi-hour agentic
# sessions don't get cut by either side.  Reduce for public-facing servers
# where slow-loris resilience matters more than long-thinking-turn support.
http_timeout=86400
# RoPE / YaRN context extension — needed when ctx_size exceeds the
# model's native training context. "yarn" scaling with scale=4 and
# yarn_orig_ctx=131072 stretches a 128K-trained model to the 512K
# ctx_size above (128K × 4). The shipped [MODEL_*] profiles inherit
# these; uncomment a profile's knobs to scale it differently.
rope_scaling="yarn"
rope_freq_scale="4"
yarn_orig_ctx=131072
# GPU split mode: none=single GPU, layer=split layers across GPUs (default
# in llama.cpp), row=tensor parallelism, tensor=full tensor parallelism.
# "none" is correct for single-GPU / iGPU systems.
split_mode="none"
# Sampling defaults written into [ENGINE] ACTIVE (not commented).
# These are the BASELINE — intentionally loose so the engine works
# acceptably with any model out of the box. Per-model tuning lives
# in [MODEL_<pattern>] sections which override these when the loaded
# model name matches. presence_penalty=1.5 is the anti-loop safety
# net for generic MoE/thinking models; model-specific profiles
# (Qwen3-Coder-Next, Qwen3.6, DeepSeek) lower or zero it.
# max_tokens=12288 caps a single response turn (code rarely exceeds
# this; runaway loops are caught earlier).
# Override per-workload via --temperature / --top-p / --top-k /
# --min-p / --presence-penalty / --repeat-penalty / --frequency-penalty.
temperature="1.0"
top_p="0.95"
top_k=20
min_p="0.0"
repeat_penalty="1.0"
presence_penalty="1.5"
frequency_penalty="0.05"
max_tokens=12288
api_key=""                                    # leave empty to skip auth (open server)

model_src=""                                  # required when --no-model NOT passed

# ---------- arg parsing -----------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --src-root)         src_root="$2"; easyai_dir="$src_root/easyai"; llama_dir="$src_root/llama.cpp"; shift 2 ;;
        --easyai-dir)       easyai_dir="$2"; shift 2 ;;
        --llama-dir)        llama_dir="$2"; shift 2 ;;
        --easyai-repo)      easyai_repo="$2"; shift 2 ;;
        --llama-repo)       llama_repo="$2"; shift 2 ;;
        --ref)              easyai_ref="$2"; shift 2 ;;
        --llama-ref)        llama_ref="$2"; shift 2 ;;
        --prefix)           install_prefix="$2"; shift 2 ;;
        --backend)          backend="$2"; shift 2 ;;
        --gtt)              gtt_gb="$2"; shift 2 ;;
        -j|--jobs)          jobs="$2"; shift 2 ;;
        --no-install)       do_install=0; shift ;;
        --no-build)         do_build=0; shift ;;
        --no-groups)        do_groups=0; shift ;;
        --no-limits)        do_limits=0; shift ;;
        --no-kernel)        do_kernel=0; shift ;;
        --no-service)       do_service=0; shift ;;
        --no-model)         do_model=0; shift ;;
        --no-avahi)         do_avahi=0; shift ;;
        --no-presets)       do_presets=0; shift ;;
        --no-llama-tools)   do_llama_tools=0; shift ;;
        --with-llama-tools) do_llama_tools=1; shift ;;
        --no-lemonade)      do_lemonade=0; shift ;;
        --with-lemonade)    do_lemonade=1; shift ;;
        --no-rocm-install)  do_rocm_install=0; shift ;;
        --with-rocm-install) do_rocm_install=1; shift ;;
        --rocm-version)     rocm_version="$2"; shift 2 ;;
        --no-tdp-unlock)    do_tdp_unlock=0; shift ;;
        --with-tdp-unlock)  do_tdp_unlock=1; shift ;;
        --tdp-watts)        tdp_watts="$2"; shift 2 ;;
        --tdp-tctl)         tdp_tctl="$2"; shift 2 ;;
        --no-swap)          do_swap="off"; shift ;;
        --swap-tune)        do_swap="tune"; shift ;;
        --keep-swap)        do_swap=""; shift ;;
        --upgrade)          do_upgrade=1; shift ;;
        --enable-now)       do_enable_now=1; shift ;;
        --no-enable)        do_enable_now=0; shift ;;
        --force-service)    do_force_service=1; shift ;;
        --force)            do_force=1; do_force_service=1; shift ;;
        --service-host)     service_host="$2"; shift 2 ;;
        --service-port)     service_port="$2"; shift 2 ;;
        --mdns-hostname)    mdns_hostname="$2"; shift 2 ;;
        --alias)            service_alias="$2"; shift 2 ;;
        --ctx-size)         ctx_size="$2"; shift 2 ;;
        --ngl|--n-gpu-layers) ngl="$2"; shift 2 ;;
        --threads)          n_threads_default="$2"; shift 2 ;;
        --threads-batch)    n_threads_batch_default="$2"; shift 2 ;;
        --preset)           preset="$2"; shift 2 ;;
        --reasoning-effort) reasoning_effort="$2"; shift 2 ;;
        --thinking)         thinking="$2"; shift 2 ;;
        --no-metrics)       enable_metrics=0; shift ;;
        --no-flash-attn)    enable_flash_attn=0; shift ;;
        --enable-verbose)   enable_verbose=1; shift ;;
        --no-verbose)       enable_verbose=0; shift ;;
        --cache-type-k)     cache_type_k="$2"; shift 2 ;;
        --cache-type-v)     cache_type_v="$2"; shift 2 ;;
        --no-mlock)         mlock=0; shift ;;
        --use-mmap)         no_mmap=0; shift ;;
        --no-mmap)          no_mmap=1; shift ;;
        --repeat-penalty)   repeat_penalty="$2"; shift 2 ;;
        --presence-penalty) presence_penalty="$2"; shift 2 ;;
        --frequency-penalty) frequency_penalty="$2"; shift 2 ;;
        --temperature)      temperature="$2"; shift 2 ;;
        --top-p)            top_p="$2"; shift 2 ;;
        --top-k)            top_k="$2"; shift 2 ;;
        --min-p)            min_p="$2"; shift 2 ;;
        --max-tokens)       max_tokens="$2"; shift 2 ;;
        --http-timeout)     http_timeout="$2"; shift 2 ;;
        --rope-scaling)     rope_scaling="$2"; shift 2 ;;
        --rope-scale)       rope_freq_scale="$2"; shift 2 ;;
        --yarn-orig-ctx)    yarn_orig_ctx="$2"; shift 2 ;;
        --split-mode)       split_mode="$2"; shift 2 ;;
        # Numeric sampling/timeout values are written into easyai.ini
        # via heredoc.  We interpolate them as-is, so anything other
        # than a number could sneak a newline or extra "key = value"
        # pair into the INI (e.g. injecting "allow_bash = on" via a
        # crafted --temperature).  Validate up front; arg parsing
        # already scoped each value to one argv slot, so we only need
        # to constrain content shape here.
        --api-key)          api_key="$2"; shift 2 ;;
        --model)            model_src="$2"; shift 2 ;;
        --copy-model)       copy_model=1; shift ;;

        # ---- install_llama_server.sh drop-in compat ----------------------
        # These were the proxy / SearXNG / MCP / spec-decoding / webui-rebrand
        # knobs of the old installer.  They're accepted here so existing
        # provisioning scripts keep working unchanged; most are no-ops because
        # the corresponding feature is now built into easyai-server.
        --source-dir)       easyai_dir="$2"; shift 2 ;;   # alias for --easyai-dir
        --with-mcp|--no-mcp)
            warn "$1: ignored — easyai bundles search_web/fetch_web as built-in tools"
            shift ;;
        --webui-title)      webui_title="$2"; shift 2 ;;
        --webui-icon)       webui_icon="$2";  shift 2 ;;
        --webui-password)   webui_password="$2"; shift 2 ;;
        --thinking-budget)
            warn "--thinking-budget: not yet supported in easyai (use --thinking on/off + --max-tokens at runtime)"
            shift 2 ;;
        --draft-model|--draft-max|--draft-min)
            warn "$1: classic draft-model speculative decoding not wired up in easyai — flag ignored. For MTP-trained models, pass --mtp instead."
            shift 2 ;;
        --no-draft)
            warn "--no-draft: ignored (speculative decoding is off by default in easyai; pass --mtp to enable MTP)"
            shift ;;
        --mtp)
            # Bake `--spec-type draft-mtp --spec-draft-n-max $mtp_n_max`
            # into ExecStart. Only meaningful when the served model was
            # trained with MTP heads (DeepSeek V3, MimoVL, etc.); other
            # models will refuse to load or run plain autoregressive.
            mtp=1
            shift ;;
        --mtp-n-max)
            mtp_n_max="$2"
            if [[ ! "$mtp_n_max" =~ ^[0-9]+$ ]] || (( mtp_n_max < 1 )); then
                die "--mtp-n-max: expected positive integer, got: $(printf '%q' "$mtp_n_max")"
            fi
            shift 2 ;;
        --list-tags)
            # Mirror the original installer's behaviour: list recent tags of the
            # easyai repo instead of llama.cpp's.
            if [[ -d "$easyai_dir/.git" ]]; then
                git -C "$easyai_dir" fetch --tags --prune --force >&2 || true
                git -C "$easyai_dir" tag -l 2>/dev/null | tail -20 | sed 's/^/  /'
            else
                echo "  (no local clone yet — pass --easyai-dir or run a normal install first)"
            fi
            exit 0 ;;
        # -----------------------------------------------------------------

        -h|--help)          sed -n '2,110p' "$0"; exit 0 ;;
        *)
            echo "unknown arg: $1" >&2
            echo "run with --help for usage" >&2
            exit 2 ;;
    esac
done

# ---------- helpers ---------------------------------------------------------
log()  { printf '\033[1;32m[+]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }
ask()  { local r; read -rp "    $* [y/N] " r; [[ "$r" =~ ^[Yy]$ ]]; }

# Validate that a sampling/timeout value is plain numeric (int or float,
# optional leading minus).  We write these into easyai.ini via heredoc
# expansion; a value containing a newline or "=" would inject extra INI
# keys (e.g. an attacker passing --temperature $'0.3\nallow_bash = on'
# would flip allow_bash on).  Reject anything that isn't a number.
require_numeric() {
    local name="$1" value="$2"
    if [[ ! "$value" =~ ^-?[0-9]+(\.[0-9]+)?$ ]]; then
        die "$name: must be numeric, got: $(printf '%q' "$value")"
    fi
}

# Reject any value that could carve out a new INI line or section when
# expanded into the heredoc.  Used for non-numeric knobs (host, alias,
# webui_title, cache_type_*) where legitimate inputs contain letters /
# digits / dashes / dots / spaces but NEVER newlines, '=', '[', or ']'.
# Mirrors require_numeric for the threat model in §20.4 and 21.7.
require_no_injection() {
    local name="$1" value="$2"
    if [[ "$value" == *$'\n'* || "$value" == *$'\r'* ]]; then
        die "$name: must not contain newlines"
    fi
    if [[ "$value" == *'='* || "$value" == *'['* || "$value" == *']'* ]]; then
        die "$name: must not contain '=', '[' or ']' (would inject into easyai.ini)"
    fi
}

# ---------- pre-flight ------------------------------------------------------
[[ $EUID -eq 0 ]] && die "do not run as root — script calls sudo as needed"

# Validate numeric inputs before they flow into the INI heredoc.  The
# arg-parser scoped each value to a single argv slot already; this layer
# constrains the content shape.
require_numeric "--temperature"     "$temperature"
require_numeric "--top-p"           "$top_p"
require_numeric "--top-k"           "$top_k"
require_numeric "--min-p"           "$min_p"
require_numeric "--repeat-penalty"  "$repeat_penalty"
require_numeric "--presence-penalty" "$presence_penalty"
require_numeric "--frequency-penalty" "$frequency_penalty"
require_numeric "--max-tokens"      "$max_tokens"
require_numeric "--http-timeout"    "$http_timeout"
require_numeric "--ctx-size"        "$ctx_size"
require_numeric "--service-port"    "$service_port"
require_numeric "--threads"         "$n_threads_default"
require_numeric "--threads-batch"   "$n_threads_batch_default"
require_numeric "--ngl"             "$ngl"
require_numeric "--rope-scale"      "$rope_freq_scale"
require_numeric "--yarn-orig-ctx"   "$yarn_orig_ctx"

# Non-numeric knobs also flow into the heredoc; reject newline / '=' /
# '[' / ']' shapes so they can't carve a new section or override key.
require_no_injection "--service-host" "$service_host"
require_no_injection "--alias"        "$service_alias"
require_no_injection "--webui-title"  "$webui_title"
require_no_injection "--webui-password" "$webui_password"
require_no_injection "--cache-type-k" "$cache_type_k"
require_no_injection "--cache-type-v" "$cache_type_v"
require_no_injection "--rope-scaling" "$rope_scaling"
require_no_injection "--split-mode"   "$split_mode"

# Preset + reasoning-effort are baked verbatim into [ENGINE]; pin them to the
# known sets (rejects typos AND any INI-injection attempt via these knobs).
case "$preset" in
    auto|deterministic|precise|balanced|creative|wild) ;;
    *) die "--preset: must be one of auto|deterministic|precise|balanced|creative|wild, got: $(printf '%q' "$preset")" ;;
esac
case "$reasoning_effort" in
    auto|low|medium|high|max|minimal) ;;
    *) die "--reasoning-effort: must be one of auto|low|medium|high|max|minimal, got: $(printf '%q' "$reasoning_effort")" ;;
esac

# Hostname must be a valid RFC 1123 label: letters / digits / hyphens,
# no leading or trailing hyphen, max 63 chars. hostnamectl would reject
# malformed names anyway; catching it here gives a friendlier error.
if [[ ! "$mdns_hostname" =~ ^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$ ]]; then
    die "--mdns-hostname: must be a valid hostname label (letters/digits/hyphens, ≤63 chars), got: $(printf '%q' "$mdns_hostname")"
fi

if [[ "$(uname -s)" != "Linux" ]]; then
    die "this installer targets Linux. On macOS, build manually — see README.md (Build for your hardware)."
fi

command -v apt-get >/dev/null \
    || die "this script targets Debian/Ubuntu (apt). Adapt package names for other distros."

# ---------- backend auto-detect --------------------------------------------
detect_backend() {
    # honour explicit --backend
    if [[ "$backend" != "auto" ]]; then echo "$backend"; return; fi
    # NVIDIA?
    if command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi >/dev/null 2>&1; then
        echo cuda; return
    fi
    # AMD ROCm?
    if command -v rocminfo >/dev/null 2>&1; then
        echo hip; return
    fi
    # any GPU visible to Vulkan?
    if command -v vulkaninfo >/dev/null 2>&1 \
            && vulkaninfo --summary 2>/dev/null | grep -qiE 'deviceName'; then
        echo vulkan; return
    fi
    # AMD GPU PCI present but Vulkan not yet installed → still pick vulkan, we'll install the SDK
    if command -v lspci >/dev/null 2>&1 \
            && lspci 2>/dev/null | grep -qiE 'vga|display' \
            && lspci 2>/dev/null | grep -qiE 'amd|radeon'; then
        echo vulkan; return
    fi
    echo cpu
}
backend_resolved="$(detect_backend)"

case "$backend_resolved" in
    auto|vulkan|cuda|hip|cpu) ;;
    *) die "unknown backend: $backend_resolved" ;;
esac

# ---------- print effective config -----------------------------------------
gtt_pages=$(( gtt_gb * 262144 ))     # GTT page count, 4 KiB per page

log "effective config:"
printf '    src_root         = %s\n' "$src_root"
printf '    easyai_dir       = %s\n' "$easyai_dir"
printf '    llama_dir        = %s\n' "$llama_dir"
printf '    install_prefix   = %s\n' "$install_prefix"
printf '    backend          = %s\n' "$backend_resolved"
printf '    service_host     = %s\n' "$service_host"
printf '    service_port     = %s\n' "$service_port"
printf '    service_alias    = %s\n' "$service_alias"
printf '    mdns_hostname    = %s   (advertises as %s.local; skipped under --no-avahi)\n' \
    "$mdns_hostname" "$mdns_hostname"
printf '    ctx_size         = %s\n' "$ctx_size"
printf '    ngl              = %s   (-1=auto, 0=CPU only, 99=all GPU layers)\n' "$ngl"
printf '    threads / batch  = %s / %s\n' "$n_threads_default" "$n_threads_batch_default"
printf '    preset           = %s  reasoning_effort=%s  thinking=%s\n' "$preset" "$reasoning_effort" "$thinking"
printf '    KV cache         = K=%s  V=%s  flash_attn=%s\n' "$cache_type_k" "$cache_type_v" "$enable_flash_attn"
printf '    split_mode       = %s\n' "$split_mode"
printf '    rope             = scaling=%s  scale=%s  yarn_orig_ctx=%s\n' \
                                 "$rope_scaling" "$rope_freq_scale" "$yarn_orig_ctx"
printf '    memory           = mlock=%s  no_mmap=%s\n' "$mlock" "$no_mmap"
printf '    http_timeout     = %ss\n' "$http_timeout"
printf '    sampling         = temp=%s top_p=%s top_k=%s min_p=%s\n' \
                                 "$temperature" "$top_p" "$top_k" "$min_p"
printf '                       repeat_penalty=%s  presence_penalty=%s  frequency_penalty=%s\n' \
                                 "$repeat_penalty" "$presence_penalty" "$frequency_penalty"
printf '                       max_tokens=%s\n' "$max_tokens"
printf '    metrics          = %s\n' "$enable_metrics"
printf '    verbose          = %s\n' "$enable_verbose"
printf '    llama_tools      = %s   (llama-cli/server/gguf-split/quantize/bench/... in $prefix/bin)\n' \
    "$([[ $do_llama_tools -eq 1 ]] && echo on || echo off)"
printf '    lemonade         = %s   (AMD Lemonade Server for NPU; systemd unit DISABLED, manual start)\n' \
    "$([[ $do_lemonade -eq 1 ]] && echo on || echo off)"
printf '    rocm_install     = %s   (auto-install minimal ROCm SDK for --backend hip on AMD CPU; version=%s)\n' \
    "$([[ $do_rocm_install -eq 1 ]] && echo on || echo off)" "$rocm_version"
printf '    tdp_unlock       = %s   (Ryzen TDP unlock via ryzenadj+systemd timer; cap=%sW tctl=%s°C)\n' \
    "$([[ $do_tdp_unlock -eq 1 ]] && echo on || echo off)" "$tdp_watts" "$tdp_tctl"
printf '    webui_title      = %s\n' "$webui_title"
printf '    webui_icon       = %s\n' "${webui_icon:-<default — no icon>}"
printf '    webui_password   = %s   (gates /models dashboard%s)\n' \
    "${webui_password:-<empty — /models is open>}" \
    "$([[ "$webui_password" == "0000" ]] && echo " — DEFAULT 0000, change for non-LAN" || echo "")"
printf '    api_key          = %s\n' "$([[ -n "$api_key" ]] && echo "<set>" || echo "<none — server is open>")"
printf '    model_src        = %s\n' "${model_src:-<none — pass --model PATH>}"
printf '    flags            = install:%s build:%s groups:%s limits:%s kernel:%s\n' \
    "$do_install" "$do_build" "$do_groups" "$do_limits" "$do_kernel"
printf '                       service:%s model:%s avahi:%s presets:%s\n' \
    "$do_service" "$do_model" "$do_avahi" "$do_presets"
printf '                       enable_now:%s force_service:%s upgrade:%s\n' \
    "$do_enable_now" "$do_force_service" "$do_upgrade"
echo

if [[ $do_model -eq 1 && -z "$model_src" ]]; then
    warn "no --model PATH given. Pass it later or rerun with --no-model to skip."
fi

# ---------- detected hardware ----------------------------------------------
log "detected hardware:"
printf '    CPU : %s\n' "$(lscpu | awk -F: '/Model name/ {gsub(/^ +/,"",$2); print $2; exit}')"
printf '    RAM : %s\n' "$(free -h | awk '/^Mem:/ {print $2}')"
if command -v lspci >/dev/null; then
    printf '    GPU : %s\n' \
        "$(lspci | grep -iE 'vga|display' | head -1 | cut -d: -f3- | sed 's/^ //')"
fi

# ---------- install dependencies -------------------------------------------
if [[ $do_install -eq 1 ]]; then
    log "installing common build deps + libcurl + system tools"
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ccache pkg-config curl ca-certificates \
        libcurl4-openssl-dev libomp-dev libcap2-bin jq \
        systemd-coredump patchelf

    case "$backend_resolved" in
        vulkan)
            log "installing Vulkan SDK + Mesa drivers"
            sudo apt-get install -y --no-install-recommends \
                mesa-vulkan-drivers vulkan-tools libvulkan-dev \
                glslc glslang-tools spirv-tools libshaderc-dev
            ;;
        cuda)
            log "CUDA backend selected — assuming nvidia-cuda-toolkit is installed."
            warn "If 'nvcc --version' fails, install the CUDA Toolkit from https://developer.nvidia.com/cuda-downloads"
            ;;
        hip)
            # Detect CPU vendor — auto-install is gated on AMD CPU because
            # on Intel boxes `--backend hip` usually means something custom
            # (eg. a slot-in AMD dGPU) and we shouldn't drop the
            # repo.radeon.com APT source unless we're clearly on an AMD
            # shop. Operator can pass --with-rocm-install to force.
            cpu_vendor="$(awk -F: '/vendor_id/{gsub(/^[[:space:]]+|[[:space:]]+$/,"",$2); print $2; exit}' /proc/cpuinfo 2>/dev/null)"

            # Distro detection — repo.radeon.com only publishes builds
            # for Ubuntu LTS releases (jammy / noble at time of writing).
            distro_id=""
            distro_codename=""
            if [[ -r /etc/os-release ]]; then
                # shellcheck disable=SC1091
                distro_id="$(. /etc/os-release && echo "${ID:-}")"
                # shellcheck disable=SC1091
                distro_codename="$(. /etc/os-release && echo "${UBUNTU_CODENAME:-${VERSION_CODENAME:-}}")"
            fi

            if [[ $do_rocm_install -ne 1 ]]; then
                log "ROCm/HIP backend selected — auto-install disabled (--no-rocm-install)."
                warn "Install the ROCm SDK manually if rocminfo / hipcc are missing."
            elif [[ "$cpu_vendor" != "AuthenticAMD" ]]; then
                log "ROCm/HIP backend selected — CPU vendor is '${cpu_vendor:-unknown}', not AMD; skipping auto-install."
                warn "Install the ROCm SDK manually, or pass --with-rocm-install to force on a non-AMD CPU."
            elif command -v hipcc >/dev/null 2>&1 && command -v rocminfo >/dev/null 2>&1; then
                log "ROCm/HIP backend selected — hipcc + rocminfo already on PATH; skipping auto-install."
            elif [[ "$distro_id" != "ubuntu" ]]; then
                warn "ROCm auto-install: distro '${distro_id:-unknown}' is not Ubuntu; skipping."
                warn "  install ROCm manually per https://rocm.docs.amd.com/projects/install-on-linux/en/latest/"
                warn "  or pass --no-rocm-install to silence this notice."
            else
                # On non-LTS Ubuntu (25.10 / questing etc.) AMD doesn't
                # publish a matching .deb — pin the APT source to the
                # latest LTS (noble). The ROCm runtime libs are decoupled
                # enough from the Ubuntu userspace that the LTS .deb runs
                # on non-LTS once apt resolves libssl3 / libssl3t64
                # transition packages. Tested path on the AI box.
                case "$distro_codename" in
                    jammy|noble) ;;
                    *)
                        log "ROCm: Ubuntu '${distro_codename:-unknown}' is non-LTS; pinning APT source to 'noble'"
                        distro_codename="noble"
                        ;;
                esac

                log "ROCm/HIP auto-install — minimal SDK from repo.radeon.com/rocm/apt/${rocm_version} (${distro_codename})"

                # gnupg may not be on minimal Ubuntu images (gpg dearmors the key below).
                if ! command -v gpg >/dev/null 2>&1; then
                    sudo apt-get install -y gnupg
                fi

                # GPG key + sources.list, both idempotent.
                sudo mkdir -p --mode=0755 /etc/apt/keyrings
                if [[ ! -r /etc/apt/keyrings/rocm.gpg ]]; then
                    log "  fetching ROCm GPG key"
                    wget -qO- https://repo.radeon.com/rocm/rocm.gpg.key \
                        | gpg --dearmor \
                        | sudo tee /etc/apt/keyrings/rocm.gpg > /dev/null
                fi
                if [[ ! -r /etc/apt/sources.list.d/rocm.list ]]; then
                    log "  adding APT source: rocm.list"
                    echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/rocm.gpg] https://repo.radeon.com/rocm/apt/${rocm_version} ${distro_codename} main" \
                        | sudo tee /etc/apt/sources.list.d/rocm.list > /dev/null
                    sudo apt-get update
                else
                    log "  /etc/apt/sources.list.d/rocm.list already present, skipping add"
                fi

                # The MINIMAL HIP set for llama.cpp's GGML_HIP backend
                # (~5 GB). Avoids the full `rocm` meta-package (~25 GB
                # with MIOpen / migraphx / rocFFT / hipSPARSE etc. that
                # llama.cpp's HIP path does not link against).
                log "  apt-get install minimal HIP set (~5 GB)"
                sudo apt-get install -y \
                    rocm-hip-runtime rocm-hip-sdk rocblas-dev hipblas-dev \
                    hip-dev rocm-device-libs

                # /etc/profile.d snippet so every login shell + the cmake
                # invocation below find hipcc and link against /opt/rocm/lib.
                if [[ ! -r /etc/profile.d/rocm.sh ]]; then
                    log "  writing /etc/profile.d/rocm.sh"
                    sudo tee /etc/profile.d/rocm.sh >/dev/null <<'PROF'
# Added by easyai installer — /opt/rocm/bin on PATH, /opt/rocm/lib on linker.
export PATH="/opt/rocm/bin${PATH:+:$PATH}"
export LD_LIBRARY_PATH="/opt/rocm/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
PROF
                fi
                # Source for the rest of this installer run (cmake call
                # below needs hipcc on PATH; new login shells get it via
                # /etc/profile.d).
                # shellcheck disable=SC1091
                [[ -r /etc/profile.d/rocm.sh ]] && source /etc/profile.d/rocm.sh

                # Sanity: confirm hipcc + rocminfo are now resolvable, and
                # that ROCm sees an actual gfx device. If rocminfo finds
                # nothing the operator likely needs to add their user to
                # render+video and relog (handled elsewhere by do_groups).
                if command -v hipcc >/dev/null 2>&1 && command -v rocminfo >/dev/null 2>&1; then
                    log "  hipcc:    $(hipcc --version 2>/dev/null | head -1)"
                    gfx_found="$(rocminfo 2>/dev/null | awk '/Name:[[:space:]]+gfx[0-9]+/{print $2; exit}')"
                    if [[ -n "$gfx_found" ]]; then
                        log "  AMD GPU visible to ROCm: $gfx_found"
                    else
                        warn "  ROCm installed but rocminfo sees no gfx device — confirm membership in render+video and relog."
                    fi
                else
                    warn "  ROCm install: hipcc or rocminfo still missing after apt — check apt output"
                fi
            fi
            ;;
        cpu)
            log "CPU-only backend — no GPU SDK to install."
            ;;
    esac

    if [[ $do_avahi -eq 1 ]]; then
        sudo apt-get install -y --no-install-recommends avahi-daemon avahi-utils
    fi
fi

# Sanity check (Vulkan only).
if [[ "$backend_resolved" == "vulkan" ]] && command -v vulkaninfo >/dev/null; then
    log "Vulkan device check"
    if ! vulkaninfo --summary 2>/dev/null \
            | grep -qiE 'deviceName'; then
        warn "vulkaninfo returned no device — Mesa/driver state may be wrong"
    else
        vulkaninfo --summary 2>/dev/null \
            | grep -iE 'deviceName|driverName' | sed 's/^/    /'
    fi
fi

# ---------- clone / fetch sources ------------------------------------------
fetch_repo() {
    local dir="$1" repo="$2" ref="$3"
    if [[ -d "$dir/.git" ]]; then
        if [[ $do_upgrade -eq 1 ]]; then
            log "git fetch + pull --ff-only in $dir"
            git -C "$dir" fetch --tags --prune --force
            # When the user pinned an explicit ref via --ref / --llama-ref,
            # the checkout below moves to it; otherwise fast-forward the
            # current branch to its upstream so the working tree actually
            # advances. --ff-only refuses to merge / rebase silently — if
            # the operator has local commits we want them to know, not have
            # us silently wipe them.
            if [[ -z "$ref" ]]; then
                if ! git -C "$dir" pull --ff-only; then
                    warn "git pull --ff-only failed in $dir — local changes block the upgrade"
                    warn "resolve manually (git status / git stash / git reset) or pass --ref <sha>"
                fi
            fi
        fi
    else
        log "cloning $repo → $dir"
        mkdir -p "$(dirname "$dir")"
        git clone --filter=blob:none "$repo" "$dir"
    fi
    if [[ -n "$ref" ]]; then
        log "checking out ref '$ref' in $dir"
        git -C "$dir" checkout "$ref"
    fi
    # Show the resulting HEAD so logs make the upgrade visible.
    local head_short
    head_short=$(git -C "$dir" rev-parse --short HEAD 2>/dev/null || echo "?")
    log "  $dir at $head_short"
}

# llama.cpp must sit next to easyai (CMakeLists looks at ../llama.cpp).
fetch_repo "$llama_dir"  "$llama_repo"  "$llama_ref"
fetch_repo "$easyai_dir" "$easyai_repo" "$easyai_ref"

# Symlink llama.cpp as a sibling of easyai if they aren't already.
expected_llama="$(dirname "$easyai_dir")/llama.cpp"
if [[ "$llama_dir" != "$expected_llama" ]]; then
    if [[ ! -e "$expected_llama" ]]; then
        log "symlinking $llama_dir → $expected_llama"
        ln -s "$llama_dir" "$expected_llama"
    elif [[ ! -L "$expected_llama" ]]; then
        warn "$expected_llama exists and is not a symlink; leaving it alone"
    fi
fi

# ---------- build easyai ----------------------------------------------------
if [[ $do_build -eq 1 ]]; then
    log "configuring easyai build (backend=$backend_resolved)"
    cmake_flags=( -DCMAKE_BUILD_TYPE=Release -DEASYAI_BUILD_SERVICES=ON )
    if [[ $do_llama_tools -eq 1 ]]; then
        cmake_flags+=( -DEASYAI_BUILD_LLAMA_TOOLS=ON )
    fi
    case "$backend_resolved" in
        vulkan)  cmake_flags+=( -DGGML_VULKAN=ON ) ;;
        cuda)    cmake_flags+=( -DGGML_CUDA=ON ) ;;
        hip)
            cmake_flags+=( -DGGML_HIP=ON )
            # Auto-detect the gfx target from rocminfo so cmake builds
            # kernels for the actual installed GPU instead of the broad
            # default set (which inflates build time and may emit code
            # that doesn't match this box, e.g. gfx1100 on a gfx1151
            # Strix Point iGPU). If rocminfo isn't available (because
            # --no-rocm-install was set and the operator hasn't sourced
            # /etc/profile.d/rocm.sh in this shell), we leave AMDGPU_TARGETS
            # unset and let llama.cpp's HIP cmake pick its default.
            if command -v rocminfo >/dev/null 2>&1; then
                gfx_target="$(rocminfo 2>/dev/null | awk '/Name:[[:space:]]+gfx[0-9]+/{print $2; exit}')"
                if [[ -n "$gfx_target" ]]; then
                    log "  AMDGPU_TARGETS=$gfx_target (auto-detected from rocminfo)"
                    cmake_flags+=( -DAMDGPU_TARGETS="$gfx_target" )
                fi
            fi
            ;;
        cpu)     ;;  # no GPU flag
    esac

    pushd "$easyai_dir" >/dev/null
    cmake -S . -B build "${cmake_flags[@]}"
    log "building (jobs=$jobs)"
    cmake --build build -j "$jobs"
    popd >/dev/null
fi

# ---------- post-build sanity: which GPU backend was actually compiled? ----
# We honour what the build produced over what the user asked for, so a CPU-
# only binary doesn't spam GPU-related errors at runtime via --ngl.
detected_backends=""
while IFS= read -r so; do
    name=$(basename "$so" | sed -E 's/^libggml-([a-z0-9]+)\.so.*/\1/')
    case "$name" in
        base|cpu) ;;
        *) detected_backends="$detected_backends $name" ;;
    esac
done < <(find "$easyai_dir/build" -maxdepth 8 -name 'libggml-*.so*' 2>/dev/null | sort -u)
detected_backends=$(echo "$detected_backends" | xargs -n1 2>/dev/null | sort -u | tr '\n' ',' | sed 's/,$//; s/,/, /g')

if [[ -z "$detected_backends" ]]; then
    if [[ "$ngl" -ne 0 ]]; then
        warn "build is CPU-only (no libggml-{vulkan,cuda,hip,metal}.so found)"
        warn "forcing --ngl 0 in the systemd unit"
        ngl=0
    fi
else
    log "GPU backends compiled into the build: $detected_backends"
    if [[ "$backend_resolved" != "cpu" ]]; then
        # Sanity: the chosen backend actually produced a library?
        case "$backend_resolved" in
            vulkan) [[ "$detected_backends" == *vulkan* ]] || warn "asked for vulkan but no libggml-vulkan.so was produced" ;;
            cuda)   [[ "$detected_backends" == *cuda*   ]] || warn "asked for cuda but no libggml-cuda.so was produced" ;;
            hip)    [[ "$detected_backends" == *hip*    ]] || warn "asked for hip but no libggml-hip.so was produced" ;;
        esac
    fi
fi

# ---------- install binaries ------------------------------------------------
if [[ $do_build -eq 1 ]]; then
    log "installing binaries to $install_prefix/bin"
    sudo install -Dm755 "$easyai_dir/build/easyai-server"  "$install_prefix/bin/easyai-server"
    sudo install -Dm755 "$easyai_dir/build/easyai-cli"     "$install_prefix/bin/easyai-cli"
    sudo install -Dm755 "$easyai_dir/build/easyai-local"   "$install_prefix/bin/easyai-local"   || true
    sudo install -Dm755 "$easyai_dir/build/easyai-agent"   "$install_prefix/bin/easyai-agent"   || true
    sudo install -Dm755 "$easyai_dir/build/easyai-chat"    "$install_prefix/bin/easyai-chat"    || true
    # Tutorial agent (today_is + weather).  Only built when libcurl is
    # present, hence the || true so a CPU-only / no-curl build still
    # finishes the install step.
    sudo install -Dm755 "$easyai_dir/build/easyai-recipes" "$install_prefix/bin/easyai-recipes" || true
    # libeasyai.so + dynamically-loaded llama / ggml libs.
    #
    # We install everything into an ISOLATED directory ($prefix/lib/easyai)
    # rather than dumping into $prefix/lib.  Two wins:
    #   1. No risk of clobbering / being clobbered by another package's
    #      libllama / libggml install on the same box.
    #   2. The runtime loader doesn't need ldconfig to find them — the
    #      systemd unit sets LD_LIBRARY_PATH explicitly, so a fresh install
    #      works even before any cache refresh.
    #
    # llama.cpp's targets land under several subdirs of build/_deps/llama.cpp/
    # (src/, common/, ggml/src/, ggml/src/ggml-*/) with versioned SONAMEs
    # like libllama.so.0.  We need ALL of them — both the bare *.so symlinks
    # and the actual *.so.N files they point at.  cp -P preserves the
    # symlink chain so libllama.so → libllama.so.0 → libllama.so.0.10.0
    # all land alongside each other.
    LIB_DEST="$install_prefix/lib/easyai"
    log "installing shared libs to $LIB_DEST"
    sudo install -d -m755 "$LIB_DEST"
    sudo install -Dm644 "$easyai_dir/build/libeasyai.so" "$LIB_DEST/libeasyai.so" || true

    copied=0
    while IFS= read -r so; do
        [[ -f "$so" || -L "$so" ]] || continue
        sudo cp -Pf "$so" "$LIB_DEST/$(basename "$so")"
        copied=$((copied+1))
    done < <(find "$easyai_dir/build" \
                  \( -name 'libllama*.so*' -o -name 'libggml*.so*' \
                  -o -name 'libllama-common*.so*' -o -name 'libllava*.so*' \
                  -o -name 'libcpp-httplib*.so*' \) -print 2>/dev/null)
    log "copied $copied shared library entries"

    # Sanity: every SONAME the binary needs must resolve under $LIB_DEST.
    # Print any missing ones now, before the systemd unit tries (and fails)
    # to start the service.
    if command -v ldd >/dev/null 2>&1; then
        missing=$(LD_LIBRARY_PATH="$LIB_DEST" ldd "$install_prefix/bin/easyai-server" 2>/dev/null \
                  | awk '/=> not found/ {print $1}')
        if [[ -n "$missing" ]]; then
            warn "the following SONAMEs the binary needs are MISSING from $LIB_DEST:"
            echo "$missing" | sed 's/^/    /' >&2
            warn "the service will refuse to start until these are present"
        fi
    fi

    # ldconfig is no longer load-bearing (we use LD_LIBRARY_PATH on the
    # unit), but refresh it anyway so easyai-server invoked outside the
    # service still finds the libs if /etc/ld.so.conf.d ever picks up
    # $LIB_DEST.
    if [[ -d /etc/ld.so.conf.d ]]; then
        echo "$LIB_DEST" | sudo tee /etc/ld.so.conf.d/easyai.conf >/dev/null
        sudo ldconfig || true
    fi

    if [[ $do_presets -eq 1 ]]; then
        log "installing easyai-cli as 'ai' shortcut → $install_prefix/bin/ai"
        sudo ln -sf "$install_prefix/bin/easyai-cli" "$install_prefix/bin/ai"
    fi

    # ---- llama.cpp tool binaries ------------------------------------------
    # When EASYAI_BUILD_LLAMA_TOOLS=ON the build dropped llama-cli /
    # llama-server / llama-gguf-split / ... into build/bin/ next to the
    # shared libraries.  We ship them to $install_prefix/bin and patch the
    # RPATH so they pick up libllama / libggml from $install_prefix/lib/easyai
    # (where the easyai install above placed them), independent of
    # LD_LIBRARY_PATH or ldconfig state.
    if [[ $do_llama_tools -eq 1 ]]; then
        tools_installed=0
        tools_skipped=0
        while IFS= read -r tool_path; do
            [[ -f "$tool_path" ]] || continue
            tool_name="$(basename "$tool_path")"
            sudo install -Dm755 "$tool_path" "$install_prefix/bin/$tool_name"
            # Point the binary at $prefix/lib/easyai (where easyai's libllama
            # lives). patchelf is a Linux-only ELF tool — on macOS dev boxes
            # the binary already carries an @rpath that the build set, so we
            # skip silently if patchelf isn't around.
            if command -v patchelf >/dev/null 2>&1; then
                sudo patchelf --set-rpath "\$ORIGIN/../lib/easyai" \
                    "$install_prefix/bin/$tool_name" 2>/dev/null || true
            fi
            tools_installed=$((tools_installed+1))
        done < <(find "$easyai_dir/build/bin" -maxdepth 1 -type f \
                       -name 'llama-*' -not -name 'lib*' \
                       -perm -u+x 2>/dev/null)

        if (( tools_installed > 0 )); then
            log "installed $tools_installed llama.cpp tool binaries to $install_prefix/bin"
            # Quick sanity: do the freshly-installed tools resolve their
            # libllama dependency? If not, the RPATH patch failed and the
            # operator will hit "cannot open shared object" at runtime.
            if command -v ldd >/dev/null 2>&1; then
                first_tool=$(find "$easyai_dir/build/bin" -maxdepth 1 -type f \
                                 -name 'llama-cli' -perm -u+x 2>/dev/null \
                                 | head -n1)
                if [[ -n "$first_tool" ]] && \
                   ldd "$install_prefix/bin/llama-cli" 2>/dev/null \
                       | grep -q 'libllama.*not found'; then
                    warn "llama-cli can't resolve libllama — RPATH patch may have failed (patchelf installed? $(command -v patchelf || echo no))"
                fi
            fi
        else
            warn "EASYAI_BUILD_LLAMA_TOOLS was on but build/bin/llama-* came up empty"
            warn "  re-run with --upgrade after fixing $easyai_dir/build (or pass --no-llama-tools to skip)"
            tools_skipped=1
        fi
    fi
fi

# ---------- system user + dirs ---------------------------------------------
if [[ $do_service -eq 1 ]]; then
    if ! id -u "$service_user" >/dev/null 2>&1; then
        log "creating system user '$service_user'"
        sudo useradd --system --home-dir "$service_home" --shell /usr/sbin/nologin \
            --comment "easyai server" "$service_user"
    fi

    log "creating $service_home + subdirs"
    sudo install -d -o "$service_user" -g "$service_group" -m 750 \
        "$service_home" "$service_model_dir" "$service_workspace" "$service_data_dir"

    log "creating $config_dir"
    sudo install -d -o root -g "$service_group" -m 750 "$config_dir"

    # We drop ONLY the TEMPLATE file:
    #   * system.txt_template — refreshed on every upgrade, the canonical
    #     reference the operator copies to system.txt when they want a
    #     custom persona.  Active system.txt is NOT created by default,
    #     so the binary's built-in "Deep" prompt (gated on actually-
    #     registered tools) is what the server uses out of the box.
    #
    # The Deep prompt lives in $system_prompt_body (defined below);
    # the template file gets a short header explaining how to activate.
    system_prompt_body=$(cat <<'PROMPT'
You are Deep — a clear, honest assistant. Answer briefly and let the
user steer. Lead with the answer; show work only when the user would
need it.

## Think SHARP, not LONG
Reasoning is for deciding the next move, not for rehearsing the answer
or exploring tangents. Hard caps on the reasoning phase:
  - 3-5 short sentences before the first tool call or final answer; up
    to ~10 short bullets for genuinely complex tasks. Never paragraphs.
  - Telegraph style — drop "I think", "Let me consider", "It seems".
    One claim or decision per line.
  - Don't enumerate options you immediately reject. Pick the move and
    go; wrong moves get corrected from tool results.
  - Don't pre-compute the answer in reasoning then restate it visibly.
    Reasoning is for the agent loop; the visible reply is for the user.
  - More than ~5 sentences without a decision → STOP and act.

Answer directly for greetings, chitchat, math, and anything you already
know — no tool needed.

When a request truly needs work, run a tight loop:
  1. Plan ONE small concrete next step (not a roadmap).
  2. Act — call the tool in the same turn. Never announce a tool call
     without making it ("I'll search…", "Let me fetch…", "Now I'll…"
     without the call is forbidden).
  3. Read the result, then finish or take ONE more step.
Stop as soon as you have something useful. Prefer a short answer the
user can refine over a long pre-committed plan.

## Tools — closed set
Your tools are EXACTLY those listed in your tools schema for this
session. Do NOT invent tools. Anything you remember from other AI
systems or training that isn't in the schema is NOT available —
including paraphrases or invented names (`shell` is not `bash`).

If a request needs a capability with no matching tool, do the work
in your visible reply. Asked to write a file / save a document /
produce a manual and you have no write tool? Put the content
DIRECTLY in the chat response — never paste it into a tool call
that doesn't exist. Every hallucinated call returns `unknown tool`
and wastes the turn.

Tool notes:
  - 'now' / 'today' / 'latest' → datetime first.
  - search_web returns snippets only; after ONE search, fetch_web the
    top 1-3 URLs and answer from the fetched body. Two searches in a
    row is wrong.
  - Long-term memory: memory(action=…) save / append / search / load.
  - Cite URLs you actually fetched. Attach dates to dated facts
    ("released April 2026" beats "recently released").

## Stay strictly in scope
Build the simplest thing that does EXACTLY what the user asked. No
extra features. No defensive scaffolding for cases they didn't
mention. No "while I'm at it" cleanups. The user's request is the
ceiling, not a starting point — they steer, you implement what they
pick. A concrete in-scope result beats a thorough comparison of
three abstractions every time.

Be terse. Be honest about uncertainty: "I'm not sure — let me check"
→ call a tool.
PROMPT
)

    # Always refresh the template (the operator's "factory reset" copy).
    log "writing $system_template_file (reference / restore-from copy, refreshed on upgrade)"
    sudo bash -c "cat > '$system_template_file'" <<TEMPLATE
# easyai-server — system prompt TEMPLATE (Deep persona)
#
# This file is REFRESHED on every --upgrade.  It's the canonical copy
# of the default Deep prompt — copy it to activate a CUSTOM persona:
#
#     sudo cp $system_template_file $system_file
#     sudo \$EDITOR $system_file
#     # then uncomment SERVER.system_file in /etc/easyai/easyai.ini
#     sudo systemctl restart easyai-server
#
# By default $system_file does NOT exist — easyai.ini ships with
# [SERVER] system_file commented out and no active prompt file on
# disk, so the binary's built-in prompt (gated on actually-registered
# tools) is what the model sees.
#
# Lines starting with # are NOT stripped from the prompt; they go
# straight to the model — keep edits clean.
#
# This template lists ONLY tools the default install registers
# (datetime / web_* / rag).  If you flip allow_fs=on or allow_bash=on
# in easyai.ini, also paste the matching tool bullets back into your
# $system_file — operator-supplied prompts are NOT auto-gated.
# ----------------------------------------------------------------------

$system_prompt_body
TEMPLATE
    sudo chmod 644 "$system_template_file"
    sudo chown root:"$service_group" "$system_template_file"

    # The active system.txt is intentionally NOT created.  Out of the
    # box the binary's built-in "Deep" prompt (gated on actually-
    # registered tools) is what the server uses.  Operators who want
    # a custom persona run `sudo cp system.txt_template system.txt`,
    # edit, then uncomment SERVER.system_file in easyai.ini.
    if [[ -f "$system_file" ]]; then
        log "preserving existing $system_file (created by operator; not refreshed by installer)"
    fi

    if [[ -n "$api_key" ]]; then
        log "writing $api_key_file (mode 600)"
        sudo bash -c "printf '%s' '$api_key' > '$api_key_file'"
        sudo chmod 600 "$api_key_file"
        sudo chown "$service_user":"$service_group" "$api_key_file"
    elif [[ -f "$api_key_file" ]]; then
        warn "leaving existing $api_key_file in place (use --api-key '' to clear)"
    fi

    # ---- external-tools dir: empty by default --------------------------
    # Operators drop EASYAI-<name>.tools files here; the server picks them
    # up at startup. We intentionally leave the dir empty; an empty dir
    # is a normal state (no extra tools registered). A short README and a
    # disabled example land alongside so the operator knows what the
    # directory is for.
    log "creating $external_tools_dir (empty by default)"
    sudo install -d -o root -g "$service_group" -m 750 "$external_tools_dir"

    if [[ ! -f "$external_tools_dir/README.md" ]]; then
        sudo bash -c "cat > '$external_tools_dir/README.md'" <<'EXT_README'
# External tools — operator-defined commands the model can dispatch

Drop manifest files here named `EASYAI-<anything>.tools` (JSON). Each
file declares one or more tools with their absolute command path,
argv template, JSON-Schema parameters, timeout, output cap, cwd, and
env passthrough. The server picks up every matching file at startup;
restart the service after changes:

    sudo systemctl restart easyai-server

Filename pattern is exact and case-sensitive:

    EASYAI-system.tools         loaded
    EASYAI-deploy.tools         loaded
    EASYAI-blah.tools.disabled  skipped (rename to enable)
    notes.md                    skipped

Per-file fault isolation: a syntax/schema error in one file is logged
to journalctl and the file is skipped — other files still load.

Authoritative documentation is in EXTERNAL_TOOLS.md (root of the
easyai repo). See also:
  - manual.md §3.3.4 / §3.3.5
  - SECURITY_AUDIT.md §16
  - the disabled example next to this README

Treat this directory like sudoers — anyone who can write here can
make the model run arbitrary commands as the agent's user.
EXT_README
        sudo chmod 640 "$external_tools_dir/README.md"
        sudo chown root:"$service_group" "$external_tools_dir/README.md"
    fi

    # Disabled example — sufix `.disabled` keeps it out of the load path.
    # The operator can `mv EASYAI-example.tools.disabled EASYAI-example.tools`
    # to enable a sample read-only system inspector.
    example_disabled="$external_tools_dir/EASYAI-example.tools.disabled"
    if [[ ! -f "$example_disabled" ]]; then
        sudo bash -c "cat > '$example_disabled'" <<'EXT_EXAMPLE'
{
  "version": 1,
  "_comment": "This file is disabled. Rename to EASYAI-example.tools to enable. Read-only system inspector — no parameters, conservative caps.",
  "tools": [
    {
      "name": "host_uptime",
      "description": "Return the system uptime and load averages of the easyai-server host. Use when the user asks about how long the box has been up or its current load.",
      "command": "/usr/bin/uptime",
      "argv": [],
      "parameters": { "type": "object", "properties": {} },
      "timeout_ms": 2000,
      "max_output_bytes": 4096,
      "cwd": "$SANDBOX",
      "env_passthrough": [],
      "stderr": "discard"
    }
  ]
}
EXT_EXAMPLE
        sudo chmod 640 "$example_disabled"
        sudo chown root:"$service_group" "$example_disabled"
    fi

    # ---- chat-template reference: refreshed every upgrade, never active --
    # Mirrors the system.txt_template pattern: drops the upstream Qwen3
    # template into /etc/easyai/qwen3-think.jinja_template so operators
    # can `sudo cp` to /etc/easyai/qwen3-think.jinja and edit, then point
    # easyai.ini [ENGINE] chat_template_file at it. The _template suffix
    # is overwritten on every install/upgrade; the active .jinja next to
    # it is preserved (never touched after the operator activates it).
    qwen3_template_src="$easyai_dir/templates/qwen3-think.jinja"
    qwen3_template_dst="$config_dir/qwen3-think.jinja_template"
    if [[ -f "$qwen3_template_src" ]]; then
        log "installing $qwen3_template_dst (reference; copy to qwen3-think.jinja to activate)"
        sudo install -o root -g "$service_group" -m 640 \
            "$qwen3_template_src" "$qwen3_template_dst"
    fi

    # ---- RAG: agent's persistent registry / long-term memory ----------
    # Owned by the SERVICE user (not root) because the AGENT writes here
    # at runtime. mode 750 — the agent reads/writes; nobody else.
    log "creating $rag_dir (RAG / long-term memory, owned by $service_user)"
    sudo install -d -o "$service_user" -g "$service_group" -m 750 \
        "$(dirname "$rag_dir")"
    sudo install -d -o "$service_user" -g "$service_group" -m 750 "$rag_dir"

    if [[ ! -f "$rag_dir/README.md" ]]; then
        sudo bash -c "cat > '$rag_dir/README.md'" <<'RAG_README'
# RAG — easyai's persistent registry / long-term memory

This directory holds the agent's RAG entries. Each `<title>.md` file
is one piece of knowledge the agent decided to remember. The agent
reads / writes these files via the rag_save / rag_search / rag_load /
rag_list / rag_delete tools.

The format is intentionally trivial so you can `cat`, `vim`, `grep`,
or hand-author a file:

    keywords: tag1, tag2, tag3

    Body content. Free-form Markdown / prose / code, up to 256 KB.

A file with no `keywords:` header is "untagged" — it shows in
rag_list but not in rag_search. Drop a hand-authored note here and
the agent will see it on next restart.

This README itself has no `keywords:` header, so the agent will
list it (under rag_list) but never surface it as RAG content.
That's deliberate.

Authoritative documentation: RAG.md and LINUX_SERVER.md in the
easyai repo.
RAG_README
        sudo chmod 640 "$rag_dir/README.md"
        sudo chown "$service_user":"$service_group" "$rag_dir/README.md"
    fi

    # ---- INI config (the central operator-tunable file) --------------
    # We write a fully-populated easyai.ini reflecting the values the
    # installer was told to use. Operators edit it later + restart the
    # service to retune anything; CLI flags on the systemd unit
    # override these (precedence: CLI > INI > hardcoded default).
    #
    # On --upgrade: if easyai.ini already exists, leave it alone — we
    # don't overwrite operator edits. The operator has to manually
    # incorporate any new keys we ship in subsequent versions (we
    # may at some point grow a more polite "upsert" but for now,
    # leave-alone is the safe default).  Pass --force to override and
    # regenerate from current installer defaults; the previous file is
    # backed up to easyai.ini.bak before the rewrite.
    if [[ ! -f "$ini_file" || $do_force -eq 1 ]]; then
        if [[ -f "$ini_file" && $do_force -eq 1 ]]; then
            log "backing up existing $ini_file → ${ini_file}.bak (--force)"
            sudo cp -f "$ini_file" "${ini_file}.bak"
            # The original INI is mode 0640 (root:easyai); the backup
            # could inherit looser permissions if cp ran with a wider
            # umask.  The INI may contain MCP Bearer tokens, so pin
            # the backup to the same posture as the live file.
            sudo chmod 640 "${ini_file}.bak"
            sudo chown root:"$service_group" "${ini_file}.bak"
            log "rewriting $ini_file (--force)"
        else
            log "writing $ini_file (central config)"
        fi
        sudo bash -c "cat > '$ini_file'" <<INI_FILE
# easyai-server central configuration — every flag the binary
# understands has a corresponding entry here. Lines starting with
# # or ; are comments. Restart the service after any change:
#
#     sudo systemctl restart easyai-server
#
# Precedence is: CLI flag > value here > hardcoded default.
# The systemd unit only passes --config; everything else
# (including the model path) is read from this file.
# Tweaking this file + restart is the normal path.

# ============================================================
# [SERVER] — HTTP layer + tool gating + paths
# ============================================================
[SERVER]
model           = $service_model_dir/$service_model_link
host            = $service_host
port            = $service_port
# alias           = $service_alias
sandbox         = $service_workspace

# system_file: path to a custom persona that REPLACES the binary's
# built-in. Commented OUT by default — the built-in prompt is gated
# on actually-registered tools (allow_fs / allow_bash / rag), so it
# never advertises tools that aren't there. The installer ships ONLY
# the template at /etc/easyai/system.txt_template; the active
# system.txt is NOT created out of the box. To activate a custom
# persona:
#     sudo cp $system_template_file $system_file
#     sudo \$EDITOR $system_file
# then uncomment the line below. If you do, AND you flip allow_fs=on
# or allow_bash=on, also paste the matching tool bullets back into
# system.txt — the operator-supplied prompt is NOT auto-gated.
# system_file     = $system_file

external_tools  = $external_tools_dir
memory          = $rag_dir
webui_title     = $webui_title

# ---------- MODELS dashboard (/models) ----------
# Sober web UI (linked from the chat UI by an injected "MODELS" pill) that
# searches HuggingFace LIVE for GGUF models and scores them against this
# machine's hardware, introspects the local .gguf files, hot-swaps the running
# model, and downloads weights — all native to easyai-server (no external
# binary, no bundled catalog). Full reference: MODELS.md and easyai-server.md
# section 7.
#
# webui_password: gates /models and all of its API routes (a session cookie
#   separate from api_key, which still guards /v1/*). Installed default is
#   0000 for an out-of-the-box LAN appliance — CHANGE IT before exposing the
#   box anywhere untrusted. Set empty to leave the dashboard open.
webui_password  = $webui_password
# download_dir: where the download manager writes/lists/deletes GGUF weights,
#   and the directory the dashboard introspects + hot-swaps from. Defaults to
#   this server's models dir so downloads sit beside the model in use.
download_dir    = $service_model_dir
# data_dir: where the /models dashboard persists its HuggingFace catalog
#   snapshot (easyai_hf_catalog.json), so a restart serves the last list
#   instantly and the 1-hour refresh clock survives. The server creates it if
#   missing.
data_dir        = $service_data_dir
# catalog_size: how many of the most-recently-updated GGUF repos to keep in the
#   searchable catalog (paged from HuggingFace, refreshed on request once >1h
#   old). Clamped to [1, 1000].
catalog_size    = 1000
metrics         = $([[ "$enable_metrics" -eq 1 ]] && echo on || echo off)
allow_fs        = off
allow_bash      = off
max_body        = 8388608

# HTTP read+write timeout for BOTH the listen socket AND the MCP-client
# connection. 86400 (24 h) matches easyai-cli's --timeout default, so
# multi-hour agentic sessions don't get cut by either side. Reduce for
# public-facing servers where slow-loris resilience matters more than
# long-thinking-turn support.
http_timeout    = $http_timeout

# Verbose: ON by default in the installer so the journal carries enough
# detail to diagnose model misbehaviour, retries, and tool failures.
# Switch to off if the journalctl noise is a problem in production.
verbose         = $([[ "$enable_verbose" -eq 1 ]] && echo on || echo off)

# Periodic METRICS line: every metrics_interval seconds, log CPU%,
# iowait, load, process mem (rss + peak), system mem, GPU GTT (AMD),
# in-flight requests, cumulative requests / errors / bytes, fd usage, TCP
# state breakdown (ESTABLISHED / TIME_WAIT / CLOSE_WAIT / FIN_WAIT / LISTEN)
# with explicit TIME_WAIT-vs-ephemeral-port-range percentage tagged
# "elevated" / "HIGH" / "CRITICAL" so socket exhaustion shows up before
# connections start failing. 0 disables.
#
# ALWAYS ON regardless of \`verbose\` — operators need this telemetry in
# journalctl whether or not they're chasing a debug session.
#
# Default 300 (5 minutes) — low-overhead enough to leave on permanently
# in production; bump down (60, 30, 5) when actively troubleshooting.
metrics_interval = 300

# /mcp authentication
# ----------------------------------------------------------------
# off     : open (anyone reaching /mcp can dispatch any tool) — DEFAULT
# auto    : enabled iff [MCP_USER] below has at least one entry
# on      : require Bearer match — also overridable via --no-mcp-auth
#
# Default 'off' assumes a home-LAN appliance behind a router. If you
# expose port 80 on a public IP, switch to 'on' and populate
# [MCP_USER] below with a strong token (openssl rand -hex 32).
mcp_auth        = off

# ============================================================
# [ENGINE] — model loading and inference tunables
# ============================================================
[ENGINE]
context          = $ctx_size
ngl              = $ngl
threads          = $n_threads_default
threads_batch    = $n_threads_batch_default

# ------------------------------------------------------------
# Sampling preset — six built-ins. Written ACTIVE below (default
# "auto"). The webui exposes a "default" badge that always reflects
# whichever preset name is active on the server, so operators don't
# have to remember the specific numbers.
#
# A CONCRETE preset (anything but "auto") is AUTHORITATIVE: it
# overrides a request's temperature/top_p/top_k and any inline
# preset. "auto" is the only non-authoritative value — it imposes
# nothing and lets clients drive sampling per request. The
# reasoning_effort key below is NEVER affected by the preset.
#
# Each preset sets temperature / top_p / top_k / min_p as a unit;
# the explicit overrides further below WIN at startup when both are
# set (they shift the baseline — per-request precedence is separate).
#
#   auto           model default — impose no preset; the engine's
#                  own sampler defaults apply, clients drive per req.
#
#   deterministic  temp=0.0  top_p=1.00  top_k=1   min_p=0.00
#     Greedy. Same prompt → identical answer every time. For
#     reproducibility, tests, anything piped into a parser.
#
#   precise        temp=0.2  top_p=0.95  top_k=40  min_p=0.10
#     Default. Sticks to high-probability tokens. Best for code,
#     math, factual Q&A, tool-calling agents, structured output.
#
#   balanced       temp=0.7  top_p=0.95  top_k=40  min_p=0.05
#     General-purpose chat. Some phrasing variety, still focused.
#
#   creative       temp=1.0  top_p=0.95  top_k=40  min_p=0.05
#     Higher entropy. Brainstorming, fiction, marketing copy.
#     Code / math get notably worse.
#
#   wild           temp=1.4  top_p=0.98  top_k=60  min_p=0.00
#     Maximum entropy. Frequent off-topic / hallucination.
#     Pure exploration only.
#
# Where the preset name is also referenced:
#   * CLI flag --preset <name>          (one-shot override per launch)
#   * GET  /health  ".preset" field     (liveness probe reports active)
#   * POST /v1/preset                   (live swap, no restart)
#   * webui "default" badge             (reflects the server's preset;
#                                        clicking it applies the same
#                                        values, no need to remember
#                                        the specific numbers)
preset           = $preset

# Reasoning effort fed to the chat template as the reasoning_effort
# kwarg (GPT-OSS et al.): auto|low|medium|high|max|minimal. "auto"
# injects nothing (model default). A per-request reasoning_effort
# body field overrides this; templates that ignore the kwarg are
# unaffected.
reasoning_effort = $reasoning_effort
flash_attn       = $([[ "$enable_flash_attn" -eq 1 ]] && echo on || echo off)
cache_type_k     = $cache_type_k
cache_type_v     = $cache_type_v
mlock            = $([[ "$mlock" -eq 1 ]] && echo on || echo off)
no_mmap          = $([[ "$no_mmap" -eq 1 ]] && echo on || echo off)
split_mode       = $split_mode

# RoPE / YaRN context extension — required when ctx exceeds the model's
# native training context. "yarn" scaling with rope_freq_scale=4 and
# yarn_orig_ctx=131072 stretches a 128K-trained model to the 512K
# context above (128K × 4). Per-model [MODEL_*] sections may override
# all three; the shipped profiles inherit them for a uniform 512K.
rope_scaling      = $rope_scaling
rope_freq_scale   = $rope_freq_scale
yarn_orig_ctx     = $yarn_orig_ctx

# Sampling overrides — leave commented for engine defaults.
temperature      = $temperature
top_p            = $top_p
top_k            = $top_k
min_p            = $min_p
repeat_penalty   = $repeat_penalty
presence_penalty = $presence_penalty
frequency_penalty = $frequency_penalty
max_tokens       = $max_tokens

# ------------------------------------------------------------
# Speculative decoding — n-gram self-speculation ON by default
# (works with ANY model). Switch the active spec_type line to
# change backend, or comment both out to disable.
# ------------------------------------------------------------
#
# MTP — Multi-Token Prediction heads embedded in the main model.
# Only works with MTP-trained models (Qwen3.5/3.6, DeepSeek V3,
# MimoVL, etc.); plain models will refuse to load. Typical
# decode speedup: 1.5-2x tok/s. No second model loaded, zero
# extra VRAM. The installer's --mtp flag bakes the SAME pair
# into the unit's ExecStart — use INI here if you'd rather edit
# easyai.ini than re-run the installer.
#spec_type        = draft-mtp
spec_draft_n_max = 2
#
# Self-speculative via n-grams — works with ANY model, no MTP
# heads needed. Smaller speedup than MTP but free.
spec_type        = ngram-cache
#
# Classic standalone draft model — NOT yet wired in easyai-server.
# (--draft-model PATH is accepted by llama.cpp but easyai doesn't
# expose the path knob; leave commented until that lands.)
#spec_type        = draft-simple

# --- Chat template / reasoning extraction (llama-server compat) ---
# Override the GGUF's embedded chat template with a Jinja file on
# disk. Mirrors llama-server's --chat-template-file. Leave empty to
# use the model's template.
#
# The installer drops a refresh-on-upgrade reference template at
#   /etc/easyai/qwen3-think.jinja_template
# Activate by copying once and pointing this key at the active copy:
#   sudo cp /etc/easyai/qwen3-think.jinja_template /etc/easyai/qwen3-think.jinja
#chat_template_file = /etc/easyai/qwen3-think.jinja
#
# Reasoning-content extraction format. Accepts:
#   none            — leave <think>…</think> inline in content
#   auto            — default; currently behaves like deepseek
#   deepseek        — split <think>…</think> into reasoning_content
#                     (including in streaming deltas). Qwen3 / R1 default.
#   deepseek-legacy — split for sync, leave inline for streaming.
#reasoning_format  = deepseek

# ============================================================
# [MCP_USER] — Bearer-token auth for POST /mcp
# ============================================================
# Each line is \`username = bearer_token\`. The username appears in
# the audit log per request; the token is what clients send as
# \`Authorization: Bearer <token>\`. Generate strong tokens:
#
#     openssl rand -hex 32
# or
#     python3 -c 'import secrets; print(secrets.token_hex(32))'
#
# If this section is missing or empty (and SERVER.mcp_auth=auto),
# /mcp is OPEN. Useful for local dev / smoke. Production: define
# at least one user.
[MCP_USER]
# gustavo  = REPLACE-ME-WITH-OPENSSL-RAND-HEX-32
# claude   = different-token-for-claude-desktop
# ci       = different-token-for-the-ci-runner

# ============================================================
# [MODEL_<pattern>] — per-model ENGINE overrides
# ============================================================
# Define per-model tuning profiles that override [ENGINE] defaults
# when the loaded model matches. The server strips the GGUF path to
# basename-without-extension, then picks the section whose name is the
# LONGEST prefix of it (case-insensitive): '[MODEL_<pattern>]' matches
# when the model name STARTS WITH <pattern>, so one section covers every
# quant in a family (e.g. [MODEL_Qwen3.6] applies to
# Qwen3.6-25B-A38M-Q4_K_M, Qwen3.6-25B-A38M-Q6_K, ...) and a longer,
# more specific pattern wins over a shorter one. Loading
# "Qwen3-Coder-Next-Q6_K_M.gguf" matches both [MODEL_Qwen3] and
# [MODEL_Qwen3-Coder-Next] — the latter wins (longer prefix).
#
# Precedence: CLI flags > MODEL_<match> > [ENGINE] > hardcoded.
#
# Keys are the same as [ENGINE] — temperature, top_p, top_k,
# min_p, repeat_penalty, presence_penalty, frequency_penalty,
# max_tokens, context, ngl, flash_attn, cache_type_k, cache_type_v,
# rope_scaling, rope_freq_scale, yarn_orig_ctx, split_mode, etc.
# Only include keys you want to override; omitted keys keep the
# [ENGINE] value.
#
# 'alias' is NOT a match key — it is the public model-id this profile
# advertises (via /v1/models, the webui badge, chat responses) once
# matched, OVERRIDING the [SERVER] alias above. Each profile below
# ships its own public name (the family name + '+', e.g.
# 'Qwen3-Coder-Next+') so clients can tell which tuned profile is
# serving them; set a profile's alias to '$service_alias' if you'd
# rather keep one stable served name for whichever model the box
# loads. An explicit --alias on the command line still wins.

# Research + coding agent profile for Qwen3-Coder-Next.
# Low temperature, tight top_p/min_p, mild penalties — tuned for
# deterministic code output and structured tool-calling.
# Symmetric q8_0 KV cache (matches the [ENGINE] default posture).
[MODEL_Qwen3-Coder-Next]
alias            = Qwen3-Coder-Next+
temperature      = 0.2
top_p            = 0.92
top_k            = 50
min_p            = 0.03
repeat_penalty   = 1.04
presence_penalty = 0.1
frequency_penalty = 0.05
max_tokens       = 12288
# context + YaRN inherit [ENGINE] (512K via 128K × 4) — uncomment to pin.
#context          = 128000
cache_type_k     = q8_0
cache_type_v     = q8_0
#rope_scaling     = yarn
#rope_freq_scale  = 2
#yarn_orig_ctx    = 131072

# Abliterated Qwen3-Coder-Next variant (Huihui). Fully pinned in RAM
# (mlock + no_mmap), n-gram self-speculation, creative-leaning sampling
# with a strong presence penalty as the anti-loop net. Context/YaRN
# inherit [ENGINE] (512K via 128K × 4) — see commented overrides below.
[MODEL_Huihui-Qwen3-Coder-Next]
alias            = Huihui-Qwen3-Coder-Next+
flash_attn       = on
cache_type_k     = q8_0
cache_type_v     = q8_0
mlock            = on
no_mmap          = on
split_mode       = none

# Context + YaRN inherit [ENGINE] (512K via 128K × 4). Uncomment and
# set rope_freq_scale = 8 to pin this profile back to the full 1M window.
#rope_scaling      = yarn
#rope_freq_scale   = 8
#yarn_orig_ctx     = 131072

spec_type        = ngram-cache
spec_draft_n_max = 2

temperature      = 1.0
top_p            = 0.95
top_k            = 20
min_p            = 0.0
repeat_penalty   = 1.0
presence_penalty = 1.5
frequency_penalty = 0.05
max_tokens       = 12288

# Gemma-4 — same fully-pinned posture as the Huihui profile: n-gram
# self-speculation, creative sampling with the presence-penalty
# anti-loop net. Context/YaRN inherit [ENGINE] (512K via 128K × 4).
[MODEL_Gemma-4]
alias            = Gemma-4
flash_attn       = on
cache_type_k     = q8_0
cache_type_v     = q8_0
mlock            = on
no_mmap          = on
split_mode       = none

# Context + YaRN inherit [ENGINE] (512K via 128K × 4). Uncomment and
# set rope_freq_scale = 8 to pin this profile back to the full 1M window.
#rope_scaling      = yarn
#rope_freq_scale   = 8
#yarn_orig_ctx     = 131072

spec_type        = ngram-cache
spec_draft_n_max = 2

temperature      = 1.0
top_p            = 0.95
top_k            = 20
min_p            = 0.0
repeat_penalty   = 1.0
presence_penalty = 1.5
frequency_penalty = 0.05
max_tokens       = 12288

# Qwen3.6 family — balanced chat defaults.
# Moderate temperature for natural conversation, no presence penalty
# (the model's own MoE gating handles diversity), repeat off.
[MODEL_Qwen3.6]
alias            = Qwen3.6+
temperature      = 0.4
top_p            = 0.95
top_k            = 20
min_p            = 0.0
repeat_penalty   = 1.0
presence_penalty = 0.0
frequency_penalty = 0.05
max_tokens       = 12288
# context + YaRN inherit [ENGINE] (512K via 128K × 4) — uncomment to pin.
#context          = 128000
cache_type_k     = bf16
cache_type_v     = q8_0
#rope_scaling     = yarn
#rope_freq_scale  = 2
#yarn_orig_ctx    = 131072
reasoning        = off

# Add your own profiles below. Examples:
#
#[MODEL_DeepSeek]
#temperature      = 0.6
#top_p            = 0.95
#top_k            = 40
#min_p            = 0.05
#repeat_penalty   = 1.0
#presence_penalty = 0.0
#frequency_penalty = 0.0
#context          = 131072

# ----------------------------------------------------------------
# Full [MODEL_*] reference — every key the engine accepts.
# Copy, rename so [MODEL_<pattern>] is a prefix of your gguf basename,
# uncomment what you want to tweak.
# ----------------------------------------------------------------
#[MODEL_TEMPLATE]
## Public model-id this profile advertises once matched, overriding the
## [SERVER] alias (via /v1/models, the webui badge, chat responses).
## Single name; NOT a match key (matching is by the [MODEL_<pattern>]
## prefix above). An explicit --alias on the command line still wins.
#alias               = $service_alias
#
## --- core load ---
#context             = 32768
#ngl                 = -1
#threads             = 0
#threads_batch       = 0
#batch               = 0
#parallel            = 1
#preset              = precise
#
## --- compute / memory ---
#flash_attn          = true
#mlock               = false
#no_mmap             = false
#no_kv_offload       = false
#kv_unified          = false
#cache_type_k        = q8_0
#cache_type_v        = q8_0
#numa                =
#split_mode          = layer
#rope_scaling        = yarn
#rope_freq_scale     = 1.0
#yarn_orig_ctx       = 32768
#override_kv         = tokenizer.ggml.eos_token_id=int:151645
#
## --- speculative decoding ---
#spec_type           = none
#spec_draft_n_max    = 6
#spec_draft_model    =
#
## --- chat template / reasoning ---
#chat_template_file  = /etc/easyai/qwen3-think.jinja
#reasoning_format    = deepseek
#
## --- sampling (per-request overrides win) ---
#temperature         = 0.7
#top_p               = 0.95
#top_k               = 40
#min_p               = 0.05
#repeat_penalty      = 1.15
#presence_penalty    = 0.0
#frequency_penalty   = 0.0
#max_tokens          = -1
#max_incomplete_retries = 10
#seed                = 0

# ============================================================
# [REMOTE_MODEL_<name>] — peer-model tools (ai-<name>)
# ============================================================
# Each [REMOTE_MODEL_<name>] section becomes a server-side tool
# named ai-<name>. The model running on THIS server can call it
# to consult ANOTHER AI model as a peer: "check my work",
# "co-solve this", "second opinion before a risky step". One
# section = one tool; add as many as you like.
#
# The tool runs SERVER-SIDE — the server is the agent. It calls
# the peer endpoint, gets the reply, and feeds it back into the
# turn. So the webui and ANY /v1/chat/completions consumer get
# the peers for free (also listed on /v1/tools). Each peer URL
# must be reachable FROM THE SERVER.
#
# Gated by the local toolbelt master switch: SERVER.local_tools
# = off (or --no-local-tools) drops these along with the built-in
# tools.
#
# EVERY peer is OFF by default — set enabled = true to switch one
# on. The server dials out to NO peer until you do (so a box that
# IS ai.local never points a peer back at itself by accident).
#
# Two peers come pre-filled as named presets (url + description),
# so enabling one is a one-liner:
#   ai-local -> http://ai.local      (general purpose)
#   ai-pro   -> http://ai-pro.local  (harder problems)
# A section with the same <name> overrides the preset's fields.
#
# Keys (only enabled = true is required to switch a peer on;
# url is required unless a preset already supplies it):
#   enabled      true to switch this peer ON (default off)
#   url          http(s) endpoint (bare host[:port] gets http://)
#   key          Bearer token
#   model        request-body model id (default: easyai)
#   description  what this peer is good for (shown to the model)
#   temperature top_p top_k min_p max_tokens   sampling knobs
#   timeout      per-call seconds (default 300)
#   tls_insecure skip cert verify on https (dev only)
#   ca_cert_path custom CA bundle (PEM) for https

# Switch on the ai-pro preset and tweak it:
#[REMOTE_MODEL_pro]
#enabled      = true
#url          = https://ai-pro.local
#key          = sk-...
#model        = easyai
#description  = Larger reasoning model. Use for hard math, tricky logic, and final-answer checks.
#temperature  = 0.2
#timeout      = 600

# A brand-new peer (any name) — just give it enabled + url:
#[REMOTE_MODEL_bigbox]
#enabled      = true
#url          = http://bigbox.lan:9000
#description  = 72B model on the LAN for deep reasoning.

# ----------------------------------------------------------------
# Full [REMOTE_MODEL_*] reference — every key the loader accepts.
# Copy, rename TEMPLATE, uncomment what you want to tweak.
# Sampling knobs left commented use the peer's own defaults.
# ----------------------------------------------------------------
#[REMOTE_MODEL_TEMPLATE]
#enabled      = false
#url          = https://peer.example:8443
#key          = REPLACE-WITH-BEARER-TOKEN
#model        = easyai
#description  = What this peer is good for (shown to the model).
#temperature  = 0.2
#top_p        = 0.95
#top_k        = 40
#min_p        = 0.05
#max_tokens   = -1
#timeout      = 300
#tls_insecure = false
#ca_cert_path = /etc/easyai/peer-ca.pem

# ============================================================
# [TOOLS] — per-tool ACL (RESERVED for a future release)
# ============================================================
# Will let the operator filter which tools the MCP catalogue
# exposes. Today every registered tool is listed; a future
# version will respect mcp_allowed / mcp_denied glob patterns
# below. Keys are advisory and ignored by the current binary.
#
# [TOOLS]
# mcp_allowed = rag_*, datetime, search_web, fetch_web
# mcp_denied  = bash, write_file
INI_FILE
        sudo chmod 640 "$ini_file"
        sudo chown root:"$service_group" "$ini_file"
    else
        log "$ini_file exists — leaving operator edits in place (pass --force to overwrite)"
    fi

    # ---- favicon: copy operator-supplied icon to /etc/easyai/favicon
    #              and let the unit point easyai-server at it -----------
    if [[ -n "$webui_icon" ]]; then
        [[ -f "$webui_icon" ]] || die "favicon not found: $webui_icon"
        # preserve the original extension so easyai-server can pick the
        # right Content-Type at runtime.
        ext="${webui_icon##*.}"
        webui_icon_dest="$config_dir/favicon.$ext"
        log "installing favicon → $webui_icon_dest"
        sudo install -Dm644 -o root -g "$service_group" \
            "$webui_icon" "$webui_icon_dest"
    else
        webui_icon_dest=""
    fi
fi

# ---------- groups (render/video for GPU access) ---------------------------
if [[ $do_groups -eq 1 && $do_service -eq 1 ]]; then
    case "$backend_resolved" in
        vulkan|hip|cuda)
            for grp in render video; do
                if getent group "$grp" >/dev/null; then
                    log "adding $service_user to group '$grp'"
                    sudo usermod -aG "$grp" "$service_user" || true
                fi
            done
            ;;
    esac
fi

# ---------- memlock / nofile limits ----------------------------------------
if [[ $do_limits -eq 1 && $do_service -eq 1 ]]; then
    limits_file="/etc/security/limits.d/easyai.conf"
    log "writing $limits_file"
    sudo tee "$limits_file" >/dev/null <<EOF
$service_user soft memlock unlimited
$service_user hard memlock unlimited
$service_user soft nofile  1048576
$service_user hard nofile  1048576
EOF
fi

# ---------- swap tuning -----------------------------------------------------
case "$do_swap" in
    off)
        log "disabling swap (pair with --no-mlock to opt out)"
        sudo swapoff -a || true
        if [[ -f /etc/fstab ]]; then
            sudo sed -ri.bak '/^[^#].*\sswap\s/s/^/#/' /etc/fstab || true
        fi
        ;;
    tune)
        log "keeping swap, setting swappiness=1, vfs_cache_pressure=50"
        echo 1  | sudo tee /proc/sys/vm/swappiness        >/dev/null || true
        echo 50 | sudo tee /proc/sys/vm/vfs_cache_pressure >/dev/null || true
        sudo tee /etc/sysctl.d/60-easyai-swap.conf >/dev/null <<'EOF'
# easyai: keep swap as a safety net but make the kernel almost never use it,
# and don't aggressively reclaim VFS caches (large GGUFs benefit from it).
vm.swappiness = 1
vm.vfs_cache_pressure = 50
EOF
        sudo sysctl --system 2>&1 | grep -E 'swappiness|cache_pressure' | sed 's/^/    /'
        ;;
    "")
        log "leaving swap untouched"
        ;;
esac

# ---------- AMD iGPU GTT kernel cmdline ------------------------------------
# Only meaningful for RDNA2 iGPUs that need a large GTT to fit a model.
if [[ $do_kernel -eq 1 && "$backend_resolved" == "vulkan" ]]; then
    if grep -qiE 'amd|radeon' <(lspci 2>/dev/null) \
            && [[ -f /etc/default/grub ]]; then
        log "patching /etc/default/grub for ttm.pages_limit=$gtt_pages (GTT $gtt_gb GiB)"
        if grep -qE 'ttm\.pages_limit=' /etc/default/grub; then
            current=$(grep -oE 'ttm\.pages_limit=[0-9]+' /etc/default/grub | head -n1 | cut -d= -f2)
            if [[ "$current" == "$gtt_pages" ]]; then
                log "ttm.pages_limit=$gtt_pages already set in /etc/default/grub; nothing to do"
            else
                log "updating ttm.pages_limit: $current → $gtt_pages (GTT $(( current / 262144 )) → $gtt_gb GiB)"
                sudo sed -ri "s|ttm\.pages_limit=[0-9]+|ttm.pages_limit=$gtt_pages|g" /etc/default/grub
                sudo update-grub || sudo grub-mkconfig -o /boot/grub/grub.cfg || true
                warn "kernel cmdline updated — REBOOT required; verify with: cat /proc/cmdline"
            fi
        else
            sudo sed -ri \
                "s|^(GRUB_CMDLINE_LINUX_DEFAULT=\")(.*)\"|\\1\\2 ttm.pages_limit=$gtt_pages\"|" \
                /etc/default/grub
            sudo update-grub || sudo grub-mkconfig -o /boot/grub/grub.cfg || true
            warn "kernel cmdline updated — REBOOT required; verify with: cat /proc/cmdline"
        fi
    fi
fi

# ---------- model placement -------------------------------------------------
if [[ $do_service -eq 1 && $do_model -eq 1 && -n "$model_src" ]]; then
    [[ -f "$model_src" ]] || die "model not found: $model_src"
    dest="$service_model_dir/$(basename "$model_src")"
    if [[ "$copy_model" -eq 1 ]]; then
        log "copying model → $dest"
        sudo install -Dm640 -o "$service_user" -g "$service_group" "$model_src" "$dest"
    else
        log "moving model → $dest"
        sudo mv "$model_src" "$dest"
        sudo chown "$service_user":"$service_group" "$dest"
        sudo chmod 640 "$dest"
    fi
    log "symlinking $service_model_dir/$service_model_link → $(basename "$model_src")"
    sudo ln -sfn "$(basename "$model_src")" "$service_model_dir/$service_model_link"
fi

# ---------- systemd unit ----------------------------------------------------
if [[ $do_service -eq 1 ]]; then
    if [[ $do_force_service -eq 1 ]]; then
        # --force-service / --force: total clean-slate rewrite of the
        # unit. Stops the service, disables auto-start, removes the
        # main unit file AND any drop-in directory (operator-authored
        # override.conf included — the assumption is that the operator
        # asked for --force precisely to nuke any prior customisation
        # and start over). reset-failed clears the systemd "failed"
        # state so a freshly-written unit isn't blocked by the
        # StartLimitBurst gate from a previous bad config.
        log "stopping + removing existing $service_name + drop-ins (--force-service)"
        sudo systemctl stop    "$service_name" 2>/dev/null || true
        sudo systemctl disable "$service_name" 2>/dev/null || true
        sudo systemctl reset-failed "$service_name" 2>/dev/null || true
        sudo rm -f  "/etc/systemd/system/$service_name"
        sudo rm -rf "/etc/systemd/system/$service_name.d"
        sudo systemctl daemon-reload
    fi

    # ----- INI-ONLY: every setting (including the model path) lives
    # in $ini_file. The systemd ExecStart carries only --config and
    # flags that are operationally inconvenient to put in INI.
    # Operator edits $ini_file + restart to tune anything — no
    # `systemctl edit` cadence required.
    #
    # CLI > INI > hardcoded default precedence is honoured by the
    # binary. Operators who prefer the old all-flags-in-unit shape
    # (e.g. for ansible's drift detection) can still pass everything
    # explicitly — both CLI and INI converge on the same setting.
    args=( --config "$ini_file" )
    if [[ -f "$api_key_file" ]]; then
        # api-key sourced from a separate file at runtime so it's never
        # visible in `ps` (and the INI doesn't carry secrets either).
        args+=( --api-key '${EASYAI_API_KEY}' )
    fi
    [[ -n "$webui_icon_dest" ]] && args+=( --webui-icon "$webui_icon_dest" )
    [[ "$thinking" == "off" ]]  && args+=( --reasoning off )
    if (( mtp == 1 )); then
        # MTP speculative decoding. Only meaningful when the served model
        # was TRAINED with MTP heads (DeepSeek V3, MimoVL, etc.); for
        # other models the load will fail. Operators who pass --mtp are
        # taking that responsibility.
        args+=( --spec-type draft-mtp --spec-draft-n-max "$mtp_n_max" )
    fi

    # api-key file integration is wired upstream in the args[] block
    # above (`--api-key '${EASYAI_API_KEY}'` is appended only when the
    # api_key file exists). Nothing else to do here — env-file pickup
    # is handled by the api-key block below.
    exec_pre=""

    # Convert the args array to a single quoted line for ExecStart.
    arg_string=""
    for a in "${args[@]}"; do
        # quote every arg
        arg_string+=" $(printf '%q' "$a")"
    done

    log "writing /etc/systemd/system/$service_name"
    sudo tee "/etc/systemd/system/$service_name" >/dev/null <<UNIT
[Unit]
Description=easyai OpenAI-compatible LLM server
Documentation=https://github.com/solariun/easy
After=network-online.target
Wants=network-online.target

# Cap restart attempts: allow up to 2 starts within a 60-second window
# (initial start + 1 retry on failure), then give up and leave the unit
# in the "failed" state instead of looping forever on a broken model /
# bad config / missing GPU.  A long-running successful start that fails
# 2+ minutes later is NOT counted against this burst because the look-
# back window resets — only rapid back-to-back boot failures stop the
# service.  Inspect the journal (journalctl -u easyai-server) to see
# why the two attempts failed, then 'sudo systemctl reset-failed' +
# 'sudo systemctl start easyai-server' to try again.
StartLimitBurst=2
StartLimitIntervalSec=60

[Service]
Type=simple
User=$service_user
Group=$service_group
WorkingDirectory=$service_home

# Render / video groups give the service access to /dev/dri/* (GPU). The
# usermod step above adds them to the user, but baking them into the unit
# means they apply even if the user gets recreated.
SupplementaryGroups=render video

# Make the service much less likely to be picked by the OOM killer when
# RAM gets tight (model + KV + activations dominate the box's footprint).
OOMScoreAdjust=-700

# Run the inference loop at SCHED_FIFO priority — the original
# install_llama_server.sh used this for steady token throughput on the
# 680M iGPU.  Requires "RestrictRealtime=no" (default) and an unbounded
# rt budget on modern systemd, which is the kernel default.
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=50

# Mesa RADV graphics-pipeline-library: ~10-15% faster inference on RDNA2
# iGPUs (Radeon 680M, 780M, …). Harmless on other backends.
Environment=RADV_PERFTEST=gpl
Environment=HOME=$service_home
Environment=XDG_CACHE_HOME=$service_home/cache
# Where libllama / libggml / libeasyai live — kept out of /usr/lib so we
# never collide with another package's install.
Environment=LD_LIBRARY_PATH=$install_prefix/lib/easyai
ExecStartPre=/bin/sh -c 'test -f $install_prefix/lib/easyai/libllama-common.so.0 || (echo "missing $install_prefix/lib/easyai/libllama-common.so.0 — re-run install_easyai_server.sh --upgrade" >&2; exit 1)'
$( [[ -f "$api_key_file" ]] && echo "Environment=\"EASYAI_API_KEY_FILE=$api_key_file\"" )
$( [[ -f "$api_key_file" ]] && echo "ExecStartPre=/bin/sh -c 'test -r \"$api_key_file\"'" )
ExecStart=/bin/sh -c '$( [[ -f "$api_key_file" ]] && echo "EASYAI_API_KEY=\$(cat $api_key_file) " )exec $install_prefix/bin/easyai-server$arg_string'
# Restart=on-failure pairs with StartLimitBurst=2 in [Unit] above: the
# first failure triggers ONE retry after RestartSec; the second failure
# leaves the unit in the failed state (no infinite restart loop).
Restart=on-failure
RestartSec=10
KillSignal=SIGINT
# Loading a 30B+ GGUF over a slow disk can take a minute; don't time out.
TimeoutStartSec=0
TimeoutStopSec=20

# hardening
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ReadWritePaths=$service_home $config_dir
ProtectHome=true
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectControlGroups=yes
LockPersonality=yes
RestrictSUIDSGID=yes
LimitMEMLOCK=infinity
LimitNOFILE=1048576
# systemd default is 0, which silently drops crash dumps. Pair this with the
# systemd-coredump package (added to apt-get deps) so any crash gets captured
# under /var/lib/systemd/coredump and is inspectable via 'coredumpctl gdb'.
LimitCORE=infinity

# Allow binding to low ports (only relevant if --service-port < 1024)
AmbientCapabilities=CAP_NET_BIND_SERVICE
CapabilityBoundingSet=CAP_NET_BIND_SERVICE

[Install]
WantedBy=multi-user.target
UNIT

    # ---- legacy drop-in cleanup --------------------------------------------
    # Earlier versions of this script wrote SOME flags into separate drop-in
    # files under .service.d/ (e.g. verbose.conf carried `--verbose` via its
    # own ExecStart=). Those files survive an upgrade and silently MASK the
    # main unit's ExecStart, dropping every new flag the operator was meant
    # to inherit (--external-tools, --RAG, …). Concrete failure: RAG enabled
    # in the main unit, but reg_* tools never registered because the
    # legacy drop-in's ExecStart wins.
    #
    # We do TWO things every install:
    #
    #   1. Delete drop-ins that THIS script used to create. Those files are
    #      ours, we know what's in them, removing is safe. Any flag they
    #      carried (--verbose) is now baked into the main unit.
    #
    #   2. WARN about any other drop-in that contains `ExecStart=` —
    #      typically an operator-authored override.conf (`systemctl edit
    #      easyai-server`). We do NOT delete those; the operator chose to
    #      add them. The warning surfaces the silent-masking risk so they
    #      can either remove the override or merge our new flags into it.
    dropin_dir="/etc/systemd/system/$service_name.d"
    if [[ -d "$dropin_dir" ]]; then
        # Known-legacy files created by previous versions of THIS script.
        # Add new entries here when we deprecate a drop-in.
        legacy_dropins=(
            "verbose.conf"          # superseded by `args+=( --verbose )` in main unit
        )
        for f in "${legacy_dropins[@]}"; do
            if [[ -f "$dropin_dir/$f" ]]; then
                log "removing legacy drop-in $dropin_dir/$f (its content is now in the main unit)"
                sudo rm -f "$dropin_dir/$f"
            fi
        done

        # Warn about any remaining drop-in that carries ExecStart= — those
        # would override the main unit's command and silently drop our flags.
        # `find` instead of glob so an empty dir doesn't trip nullglob.
        while IFS= read -r f; do
            warn "drop-in '$f' contains ExecStart= and will OVERRIDE the main unit's ExecStart."
            warn "  This silently strips --external-tools / --RAG / … from the running server."
            warn "  Either remove it (sudo rm '$f') or merge the new flags into it manually."
        done < <(sudo grep -lE '^[[:space:]]*ExecStart=' "$dropin_dir"/*.conf 2>/dev/null || true)
    fi

    sudo systemctl daemon-reload

    if [[ $do_enable_now -eq 1 ]]; then
        log "enabling + starting $service_name"
        sudo systemctl enable --now "$service_name"
        sleep 1
        sudo systemctl --no-pager --full status "$service_name" || true
    else
        log "unit installed but not started. Start it with:"
        printf '      sudo systemctl enable --now %s\n' "$service_name"
        printf '      sudo journalctl -u %s -f\n' "$service_name"
    fi
fi

# ---------- avahi / mDNS ----------------------------------------------------
# Two things together so the box shows up on the LAN as
# `<mdns_hostname>.local`:
#   1. Rename the system hostname so the kernel's mDNS announcement
#      advertises the right A record. avahi-daemon auto-publishes
#      /etc/hostname; no avahi config edit is needed for the basic
#      <name>.local resolution. /etc/hosts is kept in sync so sudo
#      doesn't complain about "unable to resolve host" next boot.
#   2. Drop /etc/avahi/services/easyai.service so DNS-SD aware clients
#      discover the easyai HTTP endpoint as a service under
#      <name>.local._http._tcp.
# --no-avahi skips both — operator keeps their existing hostname.
if [[ $do_avahi -eq 1 && $do_service -eq 1 ]]; then
    if command -v avahi-daemon >/dev/null; then
        current_host="$(hostname)"
        if [[ "$current_host" != "$mdns_hostname" ]]; then
            log "renaming host: $current_host → $mdns_hostname (advertises as $mdns_hostname.local)"
            sudo hostnamectl set-hostname "$mdns_hostname"
            # Keep /etc/hosts loopback in sync so `sudo`, etc. resolve the new
            # name. Rewrite the 127.0.1.1 line if it exists; otherwise append.
            if grep -qE '^127\.0\.1\.1[[:space:]]' /etc/hosts; then
                sudo sed -i -E "s|^127\\.0\\.1\\.1[[:space:]].*|127.0.1.1\t$mdns_hostname|" /etc/hosts
            else
                printf '127.0.1.1\t%s\n' "$mdns_hostname" | sudo tee -a /etc/hosts >/dev/null
            fi
        else
            log "hostname already $mdns_hostname; not renaming"
        fi

        avahi_service="/etc/avahi/services/easyai.service"
        log "writing $avahi_service ($mdns_hostname.local advertisement)"
        sudo tee "$avahi_service" >/dev/null <<AVA
<?xml version="1.0" standalone='no'?>
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<service-group>
  <name replace-wildcards="yes">easyai @ %h</name>
  <service>
    <type>_http._tcp</type>
    <port>$service_port</port>
    <txt-record>path=/v1</txt-record>
    <txt-record>alias=$service_alias</txt-record>
  </service>
</service-group>
AVA
        sudo systemctl restart avahi-daemon || true
    fi
fi

# ---------- Ryzen TDP unlock (ryzenadj + systemd oneshot + 60s timer) ------
# Pushes the chip out of its conservative laptop-class default (28 W STAPM
# on the HX 370) up to the spec cap (54 W) so iGPU clocks and NPU power
# don't get budget-starved during sustained LLM inference. ryzenadj writes
# directly to the SMU registers via /dev/mem, so the change is immediate
# but VOLATILE — every C6 deep-idle entry, sleep/resume, and some kernel
# power-state transitions can reset the limits back to the BIOS default.
# Two systemd units cover this:
#
#   easyai-tdp.service  — Type=oneshot, runs ryzenadj with the configured
#                         caps. Fires at boot and is also the ExecStart
#                         the timer triggers.
#   easyai-tdp.timer    — OnBootSec=10s OnUnitInactiveSec=60s, so the
#                         service re-fires 60 s after it last exits. We use
#                         OnUnitInactiveSec (not OnUnitActiveSec) because
#                         OnUnitActiveSec only schedules from the activation
#                         instant, which on a oneshot service is one-shot
#                         even when paired with a timer — first fire happens,
#                         then no further schedule is queued. Pairing with
#                         OnUnitInactiveSec loops naturally: fire → run →
#                         exit → inactive → 60 s → fire again. ryzenadj is
#                         cheap (~5 ms wall time) so the overhead is
#                         negligible.
#
# No apt package ships ryzenadj on Ubuntu — we build it from source
# (FlyGoat/RyzenAdj on GitHub) and drop the binary at /usr/local/bin.
# Build is small (~5 s on the AI box) and deps (libpci-dev/cmake/g++)
# are minimal. Idempotent: rebuilt only if /usr/local/bin/ryzenadj is
# missing OR --force is set.
#
# Gating:
#   do_tdp_unlock=0           → skip
#   CPU vendor != AuthenticAMD → skip (ryzenadj fails on Intel anyway)
#   /sys/firmware/efi missing  → skip on bare-metal-non-EFI weirdness
#
# Safety: 54 W on a chassis with bad cooling will just throttle (no
# silicon damage — Tctl=95 is the chip's own thermal cap, ryzenadj does
# not raise that). Operator should run `sensors` under load; if Tctl
# spends >50 % of inference time at 95 °C, lower --tdp-watts to 45 (the
# safer default for tighter enclosures).
if [[ $do_tdp_unlock -eq 1 ]]; then
    cpu_vendor_tdp="$(awk -F: '/vendor_id/{gsub(/^[[:space:]]+|[[:space:]]+$/,"",$2); print $2; exit}' /proc/cpuinfo 2>/dev/null)"

    if [[ "$cpu_vendor_tdp" != "AuthenticAMD" ]]; then
        log "TDP unlock: CPU vendor '$cpu_vendor_tdp' is not AMD; skipping ryzenadj install."
    else
        log "TDP unlock: building + installing ryzenadj, dropping easyai-tdp.{service,timer}"
        log "  cap=${tdp_watts}W (stapm=fast=slow), Tctl=${tdp_tctl}°C"

        # ---- build ryzenadj from source if not already on PATH -----------
        if ! command -v ryzenadj >/dev/null 2>&1; then
            log "  ryzenadj not found — building from FlyGoat/RyzenAdj"
            sudo apt-get install -y --no-install-recommends \
                libpci-dev cmake g++ git
            ryzenadj_src="$src_root/RyzenAdj"
            if [[ ! -d "$ryzenadj_src/.git" ]]; then
                git clone --depth 1 https://github.com/FlyGoat/RyzenAdj.git "$ryzenadj_src"
            else
                git -C "$ryzenadj_src" pull --ff-only || true
            fi
            cmake -S "$ryzenadj_src" -B "$ryzenadj_src/build" \
                  -DCMAKE_BUILD_TYPE=Release
            cmake --build "$ryzenadj_src/build" -j "$jobs"
            sudo install -m 0755 "$ryzenadj_src/build/ryzenadj" /usr/local/bin/ryzenadj
            # ryzenadj has no --version flag (verified by `error: unknown
            # option --version` in the wild). Best we can do is log the
            # git describe of the source we just built — pins the build
            # to a specific commit/tag for debugging "why did this stop
            # working after a pull".
            ryzenadj_ver="$(git -C "$ryzenadj_src" describe --tags --always 2>/dev/null || echo unknown)"
            log "  installed: /usr/local/bin/ryzenadj ($ryzenadj_ver)"
        else
            log "  ryzenadj already on PATH at $(command -v ryzenadj); skipping build"
        fi

        # ---- compute mW values for ryzenadj ------------------------------
        # ryzenadj wants milliwatts on all three power limits. Multiply once
        # here so the unit + timer keep using a single constant.
        tdp_mw=$(( tdp_watts * 1000 ))

        # ---- systemd oneshot service -------------------------------------
        # Type=oneshot, NO RemainAfterExit — the service has to actually go
        # "inactive (dead)" after each fire so the .timer's
        # OnUnitInactiveSec=60s clock can tick. With RemainAfterExit=yes
        # the unit stays "active (exited)" forever and the timer never
        # gets a re-fire signal (silent break — looks "fine" in status but
        # the SMU drifts back). ExecStart is a single ryzenadj call; we
        # pass --stapm/slow/fast all equal to cap so the SMU collapses
        # the PPT staircase to a single rail (predictable under sustained
        # load — matters more than peak boost for LLM decode where we
        # want stable bandwidth, not bursty).
        log "  writing /etc/systemd/system/easyai-tdp.service"
        sudo tee /etc/systemd/system/easyai-tdp.service >/dev/null <<TDP_SVC
[Unit]
Description=easyai: unlock Ryzen TDP for sustained iGPU+NPU performance
Documentation=https://github.com/FlyGoat/RyzenAdj
After=multi-user.target
ConditionPathExists=/usr/local/bin/ryzenadj

[Service]
Type=oneshot
# No RemainAfterExit — must go inactive so .timer OnUnitInactiveSec ticks.
# Single ryzenadj invocation. Failure is non-fatal in effect (the timer
# retries 60 s later anyway), but we let it bubble up so journalctl
# records drift if a kernel upgrade breaks the SMU path.
ExecStart=/usr/local/bin/ryzenadj \\
    --stapm-limit=${tdp_mw} \\
    --fast-limit=${tdp_mw} \\
    --slow-limit=${tdp_mw} \\
    --tctl-temp=${tdp_tctl}

[Install]
WantedBy=multi-user.target
TDP_SVC

        # ---- systemd timer (60 s reapply) --------------------------------
        # OnBootSec=10s waits past the early-boot dust (let the kernel +
        # smu driver settle). OnUnitInactiveSec=60s reapplies 60 s AFTER
        # the service exits — pairs with the service's lack of
        # RemainAfterExit so the loop is fire → run → inactive → 60 s →
        # fire. Persistent=true so a missed firing during suspend gets
        # caught immediately on resume.
        log "  writing /etc/systemd/system/easyai-tdp.timer"
        sudo tee /etc/systemd/system/easyai-tdp.timer >/dev/null <<'TDP_TIMER'
[Unit]
Description=easyai: reapply Ryzen TDP unlock every 60 s (defeats C6/sleep drift)

[Timer]
OnBootSec=10s
OnUnitInactiveSec=60s
Persistent=true
Unit=easyai-tdp.service

[Install]
WantedBy=timers.target
TDP_TIMER

        # ---- kernel lockdown / Secure Boot guard ------------------------
        # ryzenadj reaches the SMU by writing PCI config space through
        # /dev/mem (or sysfs PCI config). The kernel forbids those writes
        # when lockdown is in 'integrity' or 'confidentiality' mode —
        # which Secure Boot turns on by default on Ubuntu. Symptom in the
        # journal (verified in the wild):
        #     pcilib: sysfs_write: write failed: Operation not permitted
        #     PCI Bus is not writeable, check secure boot
        # /proc/sys/kernel/lockdown is the authoritative signal here (a
        # kernel can be locked down without SB — kernel cmdline lockdown=
        # — and SB on some BIOSes leaves lockdown=none). When blocked we
        # still install the units (idempotent, future-ready) so the
        # operator can flip SB / install ryzen_smu and engage with one
        # systemctl call; we just skip enable+start to avoid spamming the
        # journal with a service that fails every 60 s.
        sb_blocked=0
        lockdown_mode=""
        if [[ -r /proc/sys/kernel/lockdown ]]; then
            lockdown_mode="$(cat /proc/sys/kernel/lockdown 2>/dev/null)"
            if ! echo "$lockdown_mode" | grep -q '\[none\]'; then
                sb_blocked=1
            fi
        fi

        # ---- enable + (re)start -----------------------------------------
        # daemon-reload picks up the new unit files from disk, but if the
        # timer was already running (re-install / upgrade path) the
        # in-memory instance keeps its OLD schedule until we explicitly
        # restart it. `enable --now` is a no-op on an already-active
        # timer — it does NOT re-read the unit file. So we do the dance
        # manually: enable (idempotent), then restart timer + service
        # unconditionally so both pick up whatever we just wrote.
        # Apply once immediately so the operator doesn't need to reboot
        # to feel the unlock; the timer then keeps it pinned.
        sudo systemctl daemon-reload
        if [[ $sb_blocked -eq 1 ]]; then
            warn "TDP unlock: kernel lockdown is '${lockdown_mode}' — ryzenadj will fail with"
            warn "  'PCI Bus is not writeable, check secure boot'. The .service / .timer"
            warn "  files are installed but the timer is NOT enabled (would fail every"
            warn "  60 s and spam the journal). Unblock via one of:"
            warn "    a) Disable Secure Boot in the BIOS, reboot, then:"
            warn "         sudo systemctl enable --now easyai-tdp.timer"
            warn "    b) Install leogx9r/ryzen_smu kernel module (DKMS + MOK signing"
            warn "       under SB). ryzenadj will then use sysfs and bypass /dev/mem"
            warn "       lockdown without disabling SB."
        else
            sudo systemctl enable easyai-tdp.timer
            sudo systemctl restart easyai-tdp.timer
            sudo systemctl restart easyai-tdp.service

            log "  TDP unlock active: cap=${tdp_watts}W tctl=${tdp_tctl}°C, reapplied every 60s"
            log "  verify:    sudo ryzenadj -i      # current SMU rails"
            log "  thermals:  sensors               # watch Tctl under load"
            log "  disable:   sudo systemctl disable --now easyai-tdp.timer easyai-tdp.service"
        fi
    fi
fi

# ---------- Lemonade Server (NPU sidecar, manual start) --------------------
# Installs AMD's Lemonade Server alongside easyai-server so the box has a
# second LLM runtime that can target the XDNA2 NPU (Ryzen AI). Per the
# operator's explicit ask: install everything BUT leave the lemonade-server
# systemd unit DISABLED + STOPPED — they want to run it by hand
# (`lemonade-server serve --no-tray --port 13305`) while they iterate on
# the NPU path. Default port is 13305 (upstream default; we don't override).
#
# Path matrix:
#   Ubuntu (any release the PPA supports): add ppa:lemonade-team/stable,
#     `apt install lemonade-server`, then disable+stop the unit.
#   Debian (no PPA): warn, skip, point at the upstream "build from source"
#     guide. Doing apt-add-repository against a Debian release silently
#     mismatches the dists/ tree and breaks future updates — not worth it.
#   Other distros (Arch / Fedora / ...): same warn-and-skip. The script's
#     overall target is Debian/Ubuntu; Lemonade has its own docs for the
#     rest.
#
# `do_lemonade=0` (set via --no-lemonade) skips the whole block.
if [[ $do_lemonade -eq 1 ]]; then
    log "Lemonade Server install (NPU sidecar — systemd unit will be left DISABLED for manual use)"

    distro_id=""
    if [[ -r /etc/os-release ]]; then
        # shellcheck disable=SC1091
        distro_id="$(. /etc/os-release && echo "${ID:-}")"
    fi

    case "$distro_id" in
        ubuntu)
            # software-properties-common gives us add-apt-repository on
            # minimal Ubuntu images. Idempotent: re-running is a no-op
            # once the PPA + package are in place. The whole script runs
            # as NON-root (see EUID check above) and uses `sudo` for
            # privileged commands — mirror that here.
            if ! command -v add-apt-repository >/dev/null 2>&1; then
                log "  installing software-properties-common (needed for add-apt-repository)"
                sudo apt-get install -y software-properties-common
            fi
            if ! grep -rq 'lemonade-team' /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null; then
                log "  adding PPA: ppa:lemonade-team/stable"
                sudo add-apt-repository -y ppa:lemonade-team/stable
                sudo apt-get update
            else
                log "  PPA ppa:lemonade-team/stable already present, skipping add"
            fi
            log "  apt-get install lemonade-server"
            sudo apt-get install -y lemonade-server

            # Per operator request: keep the PPA-shipped systemd unit
            # DISABLED + STOPPED. The .service file stays on disk so the
            # operator can `systemctl start lemonade-server` if they
            # decide to flip later. `|| true` because the unit name has
            # changed across PPA versions before and a not-found is fine.
            if systemctl list-unit-files 2>/dev/null | grep -q '^lemonade-server\.service'; then
                log "  disabling + stopping lemonade-server.service (manual-start per operator request)"
                sudo systemctl disable lemonade-server.service 2>/dev/null || true
                sudo systemctl stop    lemonade-server.service 2>/dev/null || true
            else
                log "  no lemonade-server.service registered (nothing to disable)"
            fi

            log "Lemonade Server installed. Manual start:  lemonade-server serve --no-tray --port 13305"
            ;;
        debian)
            warn "Lemonade Server: Debian path is 'build from source' upstream — skipping the PPA install"
            warn "  follow https://lemonade-server.ai/docs/guide/install/ for the source build,"
            warn "  or pass --no-lemonade next run to silence this notice."
            ;;
        *)
            warn "Lemonade Server: distro '${distro_id:-unknown}' is not a supported install path here"
            warn "  Ubuntu: PPA install runs automatically. Other distros: see"
            warn "  https://lemonade-server.ai/docs/guide/install/ . Pass --no-lemonade to skip."
            ;;
    esac
fi

# ---------- summary ---------------------------------------------------------
echo
log "DONE."
echo
printf '  binary    : %s\n' "$install_prefix/bin/easyai-server"
printf '  shortcut  : %s\n' "$install_prefix/bin/ai (-> easyai-cli)"
printf '  service   : %s\n' "$service_name"
printf '  webui     : http://%s:%s/\n' \
    "$([[ "$service_host" == "0.0.0.0" ]] && hostname || echo "$service_host")" "$service_port"
printf '  api base  : http://%s:%s/v1\n' \
    "$([[ "$service_host" == "0.0.0.0" ]] && hostname || echo "$service_host")" "$service_port"
printf '  health    : http://localhost:%s/health\n' "$service_port"
[[ $enable_metrics -eq 1 ]] && \
    printf '  metrics   : http://localhost:%s/metrics\n' "$service_port"
printf '  system    : %s   (TEMPLATE; refreshed every --upgrade)\n' "$system_template_file"
if [[ -f "$system_file" ]]; then
    printf '              %s   (active custom prompt — uncomment SERVER.system_file in easyai.ini)\n' "$system_file"
else
    printf '              %s   (NOT created; built-in "Deep" prompt is in use — `sudo cp system.txt_template system.txt` to customise)\n' "$system_file"
fi
printf '  webui     : title="%s"\n' "$webui_title"
[[ -n "$webui_icon_dest" ]] && \
    printf '              icon="%s" (served at /favicon and /favicon.ico)\n' "$webui_icon_dest"
[[ -f "$api_key_file" ]] && \
    printf '  api key   : %s   (Bearer auth required on /v1)\n' "$api_key_file"
echo
printf '  test (open server):\n'
printf '    curl http://localhost:%s/v1/chat/completions \\\n' "$service_port"
printf '         -H "Content-Type: application/json" \\\n'
if [[ -f "$api_key_file" ]]; then
    printf '         -H "Authorization: Bearer $(cat %s)" \\\n' "$api_key_file"
fi
printf '         -d '\''{"messages":[{"role":"user","content":"hi"}]}'\''\n'
echo
printf '  point any OpenAI client at:  http://%s:%s/v1\n' \
    "$([[ "$service_host" == "0.0.0.0" ]] && hostname || echo "$service_host")" "$service_port"
echo
printf '  if it crashes:\n'
printf '    sudo journalctl -u %s -n 200 --no-pager\n' "$service_name"
printf '    coredumpctl list %s   # then: coredumpctl gdb <PID>\n' "$service_name"
echo
if [[ $do_lemonade -eq 1 ]] && command -v lemonade-server >/dev/null 2>&1; then
    printf '  lemonade  : installed (NPU sidecar), systemd unit DISABLED for manual use.\n'
    printf '              start by hand:  lemonade-server serve --no-tray --port 13305\n'
    printf '              web UI:         http://localhost:13305/\n'
    printf '              flip to auto-start later: sudo systemctl enable --now lemonade-server\n'
    echo
fi
if [[ $do_tdp_unlock -eq 1 ]] && systemctl is-enabled easyai-tdp.timer >/dev/null 2>&1; then
    printf '  tdp unlock: %sW cap, Tctl=%s°C, reapplied every 60s via easyai-tdp.timer\n' \
        "$tdp_watts" "$tdp_tctl"
    printf '              verify:   sudo ryzenadj -i\n'
    printf '              thermals: sensors      # watch Tctl under load (target <%s°C)\n' "$tdp_tctl"
    printf '              disable:  sudo systemctl disable --now easyai-tdp.timer easyai-tdp.service\n'
    echo
elif [[ $do_tdp_unlock -eq 1 ]] && [[ -f /etc/systemd/system/easyai-tdp.timer ]]; then
    # Units written but timer not enabled — almost always kernel lockdown
    # (SB on). Echo the unblock recipe so it's visible in the DONE summary
    # too, not just buried in the install-time warns.
    printf '  tdp unlock: units INSTALLED but TIMER NOT ENABLED (kernel lockdown active)\n'
    printf '              ryzenadj cannot write SMU under Secure Boot / lockdown.\n'
    printf '              fix:  disable Secure Boot in BIOS, then:\n'
    printf '                    sudo systemctl enable --now easyai-tdp.timer\n'
    printf '              or:   install ryzen_smu kernel module (DKMS, MOK-signed under SB)\n'
    echo
fi
