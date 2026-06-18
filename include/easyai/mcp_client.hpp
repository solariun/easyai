// easyai/mcp_client.hpp — Model Context Protocol client.
//
// The mirror image of mcp.hpp: that file lets a process BE an MCP
// server (handle JSON-RPC, expose its tools); this one lets a
// process CONSUME a remote MCP server (run initialize + tools/list
// once at startup, then proxy tools/call on every invocation).
//
// What this is for
// ----------------
// easyai-server is already an MCP server. Pointing one easyai-server
// at another easyai-server's /mcp endpoint via this client gives the
// agent a transparent way to merge two tool catalogues — the local
// toolbelt plus whatever the upstream server exposes (RAG against a
// shared registry, an `EASYAI-*.tools` pack hosted on a teammate's
// box, an MCP server written by someone else entirely).
//
// The returned Tools have plain `name` / `description` /
// `parameters_json` fields so anything that consumes a Tool (Engine,
// libeasyai-cli's Client, easyai-server's MCP advertisement) works
// without changes — the dispatch happens inside each Tool's handler,
// which transparently POSTs `tools/call` over HTTP.
//
// Connection model
// ----------------
// One libcurl handle per ClientOptions, captured behind a mutex
// inside a shared `Conn` struct held by every emitted Tool's
// handler. fetch_remote_tools() does the initial handshake; the
// returned Tools keep that connection alive for the rest of the
// process's life. No reconnection logic — if the upstream goes
// away mid-session, calls return ToolResult::error with the curl
// error string and the operator decides what to do.
//
// Build dependency
// ----------------
// Gated on EASYAI_HAVE_CURL (the same flag that gates fetch_web /
// search_web). When easyai is built without libcurl, the function
// is still present but always returns an empty vector + an error
// explaining why.
#pragma once

#include "tool.hpp"

#include <string>
#include <vector>

namespace easyai::mcp {

struct ClientOptions {
    // http(s)://host:port (no path). The `/mcp` endpoint is appended
    // by the client. Trailing slashes on `url` are tolerated.
    std::string url;

    // Bearer token to send as `Authorization: Bearer <token>`. Empty
    // string sends no Authorization header — appropriate when the
    // upstream MCP server is in open mode (`[MCP_USER]` empty or
    // `--no-mcp-auth`).
    std::string bearer_token;

    // Per-request timeout for both the initial handshake and every
    // subsequent tools/call dispatch. 20s is plenty for MCP traffic
    // (tools/call returns once the remote handler is done; if you
    // want a long-running proxied tool, raise this).
    int         timeout_seconds = 20;

    // Number of EXTRA attempts on transient transport failures
    // (connect refused, DNS, send/recv error, timeout, 5xx). 4xx
    // (auth, bad request) is never retried.  Each retry logs through
    // easyai::log::error so it lands on stderr without --verbose.
    // Default 5; set 0 to disable.
    int         retries          = 5;
};

// Connect, run `initialize` + `tools/list`, and return one Tool per
// remote tool. Each returned Tool's handler proxies tools/call back
// to the same server.
//
// Failure modes (network error, auth rejected, malformed response,
// missing libcurl) all set `err` and return an empty vector. The
// caller is expected to log the error and continue without the
// remote tools rather than fail process startup — matching how
// external_tools handles a bad manifest.
std::vector<Tool> fetch_remote_tools(const ClientOptions & opts,
                                     std::string & err);

}  // namespace easyai::mcp
