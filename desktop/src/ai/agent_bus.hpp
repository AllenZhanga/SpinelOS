#pragma once

#include <string>

namespace spinel::ai {

/// Resolve Unix socket path ($SPINEL_AGENT_BUS_SOCK or $XDG_RUNTIME_DIR/spinelos/agent.sock).
std::string agent_bus_socket_path();

/// Dispatch a single JSON-RPC request (`{"id":...,"method":"...","params":{...},"client":"..."}`)
/// through the Tier 0-3 permission gate, audit log, and AI OS atomic handlers.
std::string dispatch_agent_bus_json(const std::string &req_json,
                                    bool enforce_perms = true,
                                    bool allow_blocking_approval = true);

/// Start the AF_UNIX stream socket server (idempotent). Returns bound socket path or empty on error.
bool start_agent_bus_server(std::string *bound_path_out = nullptr);

/// Stop the AF_UNIX socket server and clean up the socket file.
void stop_agent_bus_server();

/// Whether the local bus server thread is currently running.
bool is_agent_bus_running();

/// Send a single newline-delimited JSON-RPC request over Unix socket and wait up to `timeout_ms`.
bool call_agent_bus_socket(const std::string &sock_path,
                           const std::string &req_json,
                           std::string &resp_json_out,
                           int timeout_ms = 5000);

}  // namespace spinel::ai
