// easyai.hpp — single-include convenience header for the unified
// easyai library.
//
// One library (libeasyai) ships everything: the local llama.cpp Engine,
// the remote OpenAI-protocol Client, every built-in / external / RAG /
// MCP tool, the system-prompt composer, the Backend abstraction +
// LocalBackend / RemoteBackend, and the high-level Session (the
// OpenAI-Python-SDK-shaped one-call entry point).  Link `easyai`
// (alias `easyai::easyai`) and you have it all.
//
// Legacy split aliases (easyai::engine, easyai::cli) still resolve to
// the same unified target so existing CMakeLists keep working.
#pragma once
#include "engine.hpp"
#include "client.hpp"
#include "session.hpp"
#include "tool.hpp"
#include "builtin_tools.hpp"
#include "remote_model_tool.hpp"
#include "external_tools.hpp"
#include "rag_tools.hpp"
#include "preamble.hpp"
#include "mcp.hpp"
#include "mcp_client.hpp"
#include "config.hpp"
#include "presets.hpp"
#include "plan.hpp"
#include "ui.hpp"
#include "text.hpp"
#include "log.hpp"
#include "cli.hpp"
#include "backend.hpp"
#include "agent.hpp"
