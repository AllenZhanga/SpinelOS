#pragma once

#include <functional>
#include <istream>
#include <ostream>
#include <string>

namespace spinel::ai {

/// Bus invocation hook (defaults to `call_agent_bus_socket(agent_bus_socket_path(), ...)`).
using McpBusCallFn = std::function<bool(const std::string &req_json, std::string &resp_json_out, int timeout_ms)>;

/// Base64-encode arbitrary bytes (RFC 4648 standard alphabet, no line wrapping).
std::string base64_encode_bytes(const unsigned char *data, size_t len);

/// Dispatch a single MCP JSON-RPC 2.0 request. Returns empty string for notifications.
std::string dispatch_mcp_jsonrpc(const std::string &req_json, McpBusCallFn bus_call = nullptr);

/// Run the stdio MCP JSON-RPC loop, auto-detecting Content-Length vs newline-delimited JSON framing.
int run_mcp_stdio_loop(std::istream &in, std::ostream &out, McpBusCallFn bus_call = nullptr);

}  // namespace spinel::ai
