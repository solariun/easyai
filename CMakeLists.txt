cmake_minimum_required(VERSION 3.18)

# Single-source-of-truth version — bumped via `git tag` + this line.
project(easyai
    VERSION   0.1.0
    LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# RPATH for installed binaries.  Without this, `cmake --install` strips
# the build-time RPATH and leaves the installed easyai-* binaries
# unable to locate libeasyai{,-cli}.{so,dylib} unless they happen to
# be on the system loader path.  We point the binary at its sibling
# lib dir relative to its own location, so a `--prefix /opt/easyai`
# (or any prefix) Just Works without LD_LIBRARY_PATH gymnastics.
if (APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path/../lib")
else()
    set(CMAKE_INSTALL_RPATH "\$ORIGIN/../lib")
endif()
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

option(EASYAI_BUILD_EXAMPLES "Build easyai example agents"             ON)
option(EASYAI_WITH_CURL      "Build web_fetch / web_search"            ON)
option(EASYAI_BUILD_WEBUI    "Embed the llama-server-style webui"      ON)
option(EASYAI_INSTALL        "Generate install + find_package targets" ON)
# Also build the standard llama.cpp tool binaries (llama-cli, llama-server,
# llama-gguf-split, llama-quantize, llama-bench, ...) alongside easyai.
# OFF keeps configure/build fast; the installer flips this ON so the
# AI box ships with the full tool set under $prefix/bin.
option(EASYAI_BUILD_LLAMA_TOOLS "Also build llama.cpp tool binaries"   OFF)

# ----- 64-bit std::atomic probe ---------------------------------------------
# Some example binaries declare `std::atomic<int64_t>` /
# `std::atomic<uint64_t>` (server.cpp's /metrics counters,
# local.cpp's signal-debounce timestamp). On aarch64 / x86_64 those
# compile to native instructions and need no extra link; on 32-bit
# ARM (`arm-linux-gnueabihf`, the default Pi OS 32-bit toolchain
# AND any aarch64 host whose default compiler still defaults to
# armhf) the compiler emits __atomic_load_8 / __atomic_fetch_add_8
# calls that live in libatomic.so.1.
#
# Probe once here; targets that use 64-bit atomics link `atomic` iff
# the probe failed. Variable is cached so reconfigures don't re-run
# the test.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
    #include <atomic>
    #include <cstdint>
    int main() {
        std::atomic<uint64_t> x{0};
        return (int) x.fetch_add(1);
    }
" EASYAI_HAVE_NATIVE_64BIT_ATOMICS)
if (NOT EASYAI_HAVE_NATIVE_64BIT_ATOMICS)
    set(EASYAI_ATOMIC_LIBS atomic)
    message(STATUS "easyai: 32-bit-ARM-style 64-bit atomics — linking libatomic on server + local")
else()
    set(EASYAI_ATOMIC_LIBS "")
endif()

# ------------------------------------------------------------------ llama.cpp
# Pulled in as a sibling directory; we disable its own apps so the build
# is fast and we only get the libraries we need.
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
if (EASYAI_BUILD_LLAMA_TOOLS)
    set(LLAMA_BUILD_TOOLS    ON  CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER   ON  CACHE BOOL "" FORCE)
else()
    set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
endif()
set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
# We DO need the `common` static library — it provides chat templates,
# tool-call parsing and the high-level sampler used by easyai.
set(LLAMA_BUILD_COMMON   ON  CACHE BOOL "" FORCE)

# GPU backends — auto-enable per platform, user can override via -D...=OFF.
if (APPLE)
    set(GGML_METAL              ON  CACHE BOOL "" FORCE)
    set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
endif()
option(GGML_VULKAN "ggml: enable Vulkan backend" OFF)

if (NOT TARGET llama)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../llama.cpp
                     ${CMAKE_BINARY_DIR}/_deps/llama.cpp
                     EXCLUDE_FROM_ALL)
endif()

# llama.cpp is added EXCLUDE_FROM_ALL, so its tool/server targets exist but
# aren't built by `cmake --build` unless we depend on them.  This aggregate
# target pulls in every llama-* tool target that actually got defined, so a
# normal build produces llama-cli / llama-server / llama-gguf-split / ... in
# build/bin alongside the libraries.  Skipped tools (toggled off in
# llama.cpp upstream, removed between versions) are silently ignored.
if (EASYAI_BUILD_LLAMA_TOOLS)
    set(_easyai_llama_tool_targets
        llama-cli
        llama-server
        llama-gguf-split
        llama-quantize
        llama-bench
        llama-batched-bench
        llama-tokenize
        llama-imatrix
        llama-perplexity
        llama-cvector-generator
        llama-export-lora
        llama-tts)
    add_custom_target(easyai_llama_tools ALL)
    foreach(_tool IN LISTS _easyai_llama_tool_targets)
        if (TARGET ${_tool})
            add_dependencies(easyai_llama_tools ${_tool})
        endif()
    endforeach()
endif()

# ============================================================================
# libeasyai — UNIFIED easyai library
# ----------------------------------------------------------------------------
# Single library target.  Carries everything: the local llama.cpp Engine,
# the OpenAI-protocol Client, the canonical built-in / external / RAG /
# MCP tools, the system-prompt composer (preamble), the Backend
# abstraction + LocalBackend/RemoteBackend, the high-level Session, and
# the CLI helpers (Toolbelt, log tee, sandbox validation).
#
# We deliberately do NOT split into libeasyai + libeasyai-cli.  Splitting
# was a layering convenience that asked downstream agents to pick the
# right link line per feature.  The product is the library; demos
# (easyai-cli, easyai-server, easyai-mcp-server) consume the same single
# library.  Anything pulled in by HTTPS / cpp-httplib is conditional on
# CMake-time finds (libcurl, OpenSSL), not on linkage choice.
#
# NOTE: cpp-httplib's httplib.cpp is compiled DIRECTLY into libeasyai
# instead of consuming the vendored cpp-httplib static target.  Reason:
# llama.cpp's cpp-httplib build doesn't define CPPHTTPLIB_OPENSSL_SUPPORT,
# so the static lib has no SSLClient symbols — defining the macro on our
# side and instantiating SSLClient would fail the link.  Re-compiling
# the .cpp here lets us add the macro and link OpenSSL ourselves, without
# touching the upstream vendor build that easyai-server already relies on.
# ============================================================================
set(EASYAI_SOURCES
    # core
    src/engine.cpp
    src/tool.cpp
    src/builtin_tools.cpp
    src/remote_model_tool.cpp
    src/config.cpp
    src/external_tools.cpp
    src/rag_tools.cpp
    src/preamble.cpp
    src/mcp.cpp
    src/mcp_client.cpp
    src/presets.cpp
    src/plan.cpp
    src/ui.cpp
    src/tui.cpp
    src/log.cpp
    src/cli.cpp
    src/backend.cpp
    # remote-side (Client, RemoteBackend, agent runners, Session)
    src/client.cpp
    src/cli_client.cpp
    src/agent.cpp
    src/session.cpp
    # vendored httplib (see header comment above)
    ${CMAKE_CURRENT_SOURCE_DIR}/../llama.cpp/vendor/cpp-httplib/httplib.cpp
)

add_library(easyai ${EASYAI_SOURCES})
# Public alias — `easyai::easyai` is the canonical link target.
# Legacy aliases (`easyai::engine`, `easyai::cli`) kept as no-op aliases
# for one release so existing consumers of the split layout keep
# linking; both now point at the same unified library.
add_library(easyai::easyai ALIAS easyai)
add_library(easyai::engine ALIAS easyai)
add_library(easyai::cli    ALIAS easyai)
set_target_properties(easyai PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME "easyai")
target_include_directories(easyai
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/../llama.cpp/vendor             # nlohmann/json.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../llama.cpp/vendor/cpp-httplib) # httplib.h
# BUILD_INTERFACE wraps the link so the dep doesn't leak into the
# exported `easyaiTargets.cmake` — downstream consumers don't see
# llama / llama-common as required CMake targets, they just link
# libllama.so / libllama-common.so transparently at runtime via the
# installed libeasyai.so's RPATH/system loader.
target_link_libraries(easyai PRIVATE
    "$<BUILD_INTERFACE:llama>"
    "$<BUILD_INTERFACE:llama-common>")
target_compile_features(easyai PUBLIC cxx_std_17)

if (EASYAI_WITH_CURL)
    find_package(CURL QUIET)
    if (CURL_FOUND)
        target_link_libraries(easyai PRIVATE CURL::libcurl)
        target_compile_definitions(easyai PRIVATE EASYAI_HAVE_CURL=1)
        message(STATUS "easyai: libcurl found — web_fetch / web_search enabled")
    else()
        message(WARNING "easyai: libcurl not found — web tools will return errors at runtime")
    endif()
endif()

# OpenSSL -> HTTPS support inside libeasyai.  Optional: when libssl
# isn't available we skip the define and Client returns a clear
# "HTTPS not in this build" error at request time.
#
# We additionally guard on `TARGET OpenSSL::SSL` because on macOS without
# brew openssl@3 (keg-only), FindOpenSSL can half-detect the system libs:
# it sets OpenSSL_FOUND=TRUE but fails to create the imported targets,
# which would then break the configure with "Target ... links to
# OpenSSL::SSL but the target was not found". The guard makes that case
# degrade to "HTTPS not in this build" instead of a hard error — point
# the build at brew openssl via -DOPENSSL_ROOT_DIR=$(brew --prefix
# openssl@3) (the install + build_macos scripts do this automatically).
find_package(OpenSSL QUIET)
if (OpenSSL_FOUND AND TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    target_compile_definitions(easyai PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT=1)
    target_link_libraries(easyai PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    # On macOS, cpp-httplib's SSL build calls CFArray* / SecCertificate* /
    # SecTrust* to load the system keychain — those live in the
    # CoreFoundation and Security frameworks.  No-op on other platforms.
    if (APPLE)
        find_library(EASYAI_COREFOUNDATION CoreFoundation)
        find_library(EASYAI_SECURITY       Security)
        if (EASYAI_COREFOUNDATION AND EASYAI_SECURITY)
            target_link_libraries(easyai PRIVATE
                ${EASYAI_COREFOUNDATION} ${EASYAI_SECURITY})
        else()
            message(WARNING "easyai: CoreFoundation / Security frameworks "
                            "not found — HTTPS link may fail on macOS")
        endif()
    endif()
    message(STATUS "easyai: OpenSSL found — HTTPS endpoints enabled")
elseif (OpenSSL_FOUND)
    message(WARNING "easyai: OpenSSL detected but imported targets "
                    "OpenSSL::SSL/Crypto were not created — HTTPS disabled. "
                    "On macOS, install brew openssl@3 and pass "
                    "-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3).")
else()
    message(STATUS "easyai: OpenSSL NOT found — HTTPS endpoints will be rejected at runtime")
endif()

# ============================================================================
# examples (binaries)
# ============================================================================
if (EASYAI_BUILD_EXAMPLES)
    add_executable(easyai-chat examples/chat.cpp)
    target_link_libraries(easyai-chat PRIVATE easyai)

    add_executable(easyai-agent examples/agent.cpp)
    target_link_libraries(easyai-agent PRIVATE easyai)

    # Tutorial agent paired with manual.md → "Writing your own tools".
    if (CURL_FOUND)
        add_executable(easyai-recipes examples/recipes.cpp)
        target_link_libraries(easyai-recipes PRIVATE easyai CURL::libcurl)
    endif()

    # easyai-local — local-only REPL: loads a GGUF model in-process via
    # easyai::Engine.  No HTTP transport here.  For talking to a remote
    # OpenAI-compatible endpoint, see easyai-cli below.
    add_executable(easyai-local examples/local.cpp)
    # local.cpp uses std::atomic<int64_t> for the SIGINT debounce
    # timestamp; same 32-bit-ARM caveat as server.cpp — link
    # libatomic when the probe failed.
    target_link_libraries(easyai-local PRIVATE easyai ${EASYAI_ATOMIC_LIBS})
    target_include_directories(easyai-local PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../llama.cpp/vendor)

    # ----- embed the llama-server-derived webui (xxd-style binary embed) ---
    set(EASYAI_WEBUI_HEADERS "")
    if (EASYAI_BUILD_WEBUI)
        set(WEBUI_ASSETS index.html bundle.js bundle.css loading.html)
        foreach(asset ${WEBUI_ASSETS})
            set(_in  "${CMAKE_CURRENT_SOURCE_DIR}/webui/${asset}")
            set(_out "${CMAKE_CURRENT_BINARY_DIR}/webui_${asset}.hpp")
            list(APPEND EASYAI_WEBUI_HEADERS ${_out})
            add_custom_command(
                DEPENDS "${_in}"
                OUTPUT  "${_out}"
                COMMAND ${CMAKE_COMMAND} -DINPUT=${_in} -DOUTPUT=${_out}
                        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/xxd.cmake
                COMMENT "[easyai] embed webui asset: ${asset}"
            )
            set_source_files_properties(${_out} PROPERTIES GENERATED TRUE)
        endforeach()
    endif()

    # The brand SVG (AI Box favicon) is no longer xxd-generated; it
    # lives inline as `kBrandSvg` in examples/server.cpp.  Canonical
    # copy stays at webui/AI-brain.svg for external use / rebrand.

    add_executable(easyai-server examples/server.cpp ${EASYAI_WEBUI_HEADERS})
    target_link_libraries(easyai-server PRIVATE easyai cpp-httplib llama-common ${EASYAI_ATOMIC_LIBS})

    # easyai-server's VerboseServer subclass overrides the
    # httplib::Server::process_and_close_socket virtual to log per-TCP
    # connect/close events. Upstream declares that virtual `private:`;
    # we widen it to `protected` for our TU only via a tiny macro trick
    # local to examples/server.cpp (`#define private protected` around
    # the httplib.h include, scoped by httplib.h's own include guard).
    # That keeps llama.cpp's source untouched and produces no derivative
    # copy of cpp-httplib in our build tree.
    target_include_directories(easyai-server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../llama.cpp/vendor/cpp-httplib
        ${CMAKE_CURRENT_SOURCE_DIR}/../llama.cpp/vendor
        ${CMAKE_CURRENT_BINARY_DIR})
    if (EASYAI_BUILD_WEBUI)
        target_compile_definitions(easyai-server PRIVATE EASYAI_BUILD_WEBUI=1)
    endif()

    # easyai-cli — agentic OpenAI-protocol client.  Talks to any
    # /v1/chat/completions endpoint (our easyai-server, llama-server, etc.).
    # No local model — pair with easyai-local when you want to run a GGUF
    # in-process instead.
    add_executable(easyai-cli examples/cli.cpp)
    target_link_libraries(easyai-cli PRIVATE easyai)
    target_include_directories(easyai-cli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../llama.cpp/vendor)

    # easyai-library-demo — minimum-viable Session demo (OpenAI-Python-
    # SDK shape).  Paired with LIB_GUIDE.md: shows third-party authors
    # the smallest "build an agent, register a tool, chat" program.
    add_executable(easyai-library-demo examples/library_demo.cpp)
    target_link_libraries(easyai-library-demo PRIVATE easyai)

    # easyai-mcp-server — standalone Model Context Protocol provider.
    # Links libeasyai (for tools, RAG, external-tools, MCP dispatcher,
    # INI parser) and cpp-httplib for HTTP transport.  Deliberately
    # does NOT link llama-common: there is no engine, no chat template,
    # no GGUF loader — the binary's job is to dispatch tool calls over
    # /mcp, nothing else.  Designed for high-concurrency multi-client
    # deployments (configurable cpp-httplib ThreadPool + per-tool
    # concurrent-call cap inside the binary).
    add_executable(easyai-mcp-server examples/mcp_server.cpp)
    # mcp_server.cpp uses std::atomic<uint64_t> for the /metrics
    # counters and std::atomic<int> for the in-flight limiter; the
    # 64-bit case hits the same libatomic-missing error on 32-bit ARM
    # toolchains as server.cpp / local.cpp, so link `${EASYAI_ATOMIC_LIBS}`
    # (empty on hosts with native atomics — see the probe at the top
    # of this file).
    target_link_libraries(easyai-mcp-server PRIVATE easyai cpp-httplib ${EASYAI_ATOMIC_LIBS})
    target_include_directories(easyai-mcp-server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../llama.cpp/vendor/cpp-httplib
        ${CMAKE_CURRENT_SOURCE_DIR}/../llama.cpp/vendor)
endif()

# ============================================================================
# install rules + find_package(easyai) support
# ----------------------------------------------------------------------------
# After `cmake --install build`, downstream projects can do:
#
#   find_package(easyai 0.1 REQUIRED)
#   target_link_libraries(myapp PRIVATE easyai::easyai)
#
# (Legacy aliases easyai::engine and easyai::cli still resolve to the
# same unified library for one release.)
#
# The lib target is exported with VERSION/SOVERSION matching the project
# version, public include dir relocated to <prefix>/include, and the
# cmake config files written to <prefix>/lib/cmake/easyai/.
# ============================================================================
if (EASYAI_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    # We deliberately DON'T use install(EXPORT) — easyai's transitive
    # link to llama / llama-common from llama.cpp (added via
    # EXCLUDE_FROM_ALL) confuses CMake's exported-target tracker.  Hand-
    # rolling the config + IMPORTED targets sidesteps that and gives
    # downstream projects a clean find_package(easyai) experience.
    install(TARGETS easyai
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # The example binaries are installed too when EASYAI_BUILD_EXAMPLES
    # is on (default).  This is what makes `cmake --install` give you a
    # ready-to-run easyai-server / easyai-cli / easyai-local on the PATH
    # instead of leaving them under build/.  Skip this block when
    # consumers only want the libs.
    if (EASYAI_BUILD_EXAMPLES)
        set(_easyai_install_bins
            easyai-local
            easyai-cli
            easyai-server
            easyai-mcp-server
            easyai-chat
            easyai-agent)
        if (CURL_FOUND)
            list(APPEND _easyai_install_bins easyai-recipes)
        endif()
        install(TARGETS ${_easyai_install_bins}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    install(DIRECTORY include/easyai
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")

    # llama.cpp's libraries — built under build/bin/ via EXCLUDE_FROM_ALL
    # add_subdirectory, so its own install rules don't run.  We ship
    # them into <prefix>/lib alongside libeasyai{,-cli} so the
    # installed binaries (which carry RUNPATH=$ORIGIN/../lib or
    # @loader_path/../lib) load them transparently — no need for
    # LD_LIBRARY_PATH or an explicit "find_package(llama)" downstream.
    install(CODE "
        file(GLOB _easyai_runtime_libs
            \"\${CMAKE_BINARY_DIR}/bin/libggml*.so*\"
            \"\${CMAKE_BINARY_DIR}/bin/libggml*.dylib\"
            \"\${CMAKE_BINARY_DIR}/bin/libllama*.so*\"
            \"\${CMAKE_BINARY_DIR}/bin/libllama*.dylib\")
        if (NOT _easyai_runtime_libs)
            file(GLOB _easyai_runtime_libs
                \"${CMAKE_BINARY_DIR}/bin/libggml*.so*\"
                \"${CMAKE_BINARY_DIR}/bin/libggml*.dylib\"
                \"${CMAKE_BINARY_DIR}/bin/libllama*.so*\"
                \"${CMAKE_BINARY_DIR}/bin/libllama*.dylib\")
        endif()
        foreach(_lib IN LISTS _easyai_runtime_libs)
            file(INSTALL DESTINATION
                \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}\"
                TYPE SHARED_LIBRARY FILES \"\${_lib}\")
        endforeach()
    ")

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/easyaiConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/easyaiConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/easyai
        PATH_VARS CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_INCLUDEDIR)

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/easyaiConfigVersion.cmake
        VERSION       ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/easyaiConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/easyaiConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/easyai)
endif()
