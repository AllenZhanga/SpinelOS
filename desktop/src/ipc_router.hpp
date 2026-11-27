#pragma once

#include "app_state.hpp"

#include <cstdint>
#include <map>
#include <string>

struct IpcRequest {
    bool is_json = false;
    bool has_id = false;
    std::string id_json;
    std::string method;       // Canonical method or command header (before '\n')
    std::string legacy_raw;   // Original raw payload or synthesized "method\nkey=val..."
    std::map<std::string, std::string> params;

    std::string param(const std::string &key, const std::string &fallback = "") const {
        auto it = params.find(key);
        return (it != params.end()) ? it->second : fallback;
    }
};

struct IpcHostActions {
    void (*open_games)(AppState *) = nullptr;
    void (*open_store)(AppState *, const std::string &focus_id) = nullptr;
    void (*open_installer)(AppState *) = nullptr;
    void (*open_settings)(AppState *, const std::string &tab) = nullptr;
    void (*open_welcome)(AppState *) = nullptr;
    void (*close_welcome)(AppState *) = nullptr;
    void (*open_doctor)(AppState *) = nullptr;
    void (*open_monitor)(AppState *, const std::string &tab) = nullptr;
    void (*store_refresh)(AppState *) = nullptr;
    void (*store_action)(AppState *, const std::string &action, const std::string &payload) = nullptr;
    void (*store_cancel)(AppState *) = nullptr;
    void (*store_launch)(AppState *, const std::string &id) = nullptr;
    void (*installer_scan)(AppState *) = nullptr;
    void (*installer_start)(AppState *, const std::string &payload) = nullptr;
    void (*push_all_telemetry)(AppState *) = nullptr;
    void (*push_windows)(AppState *) = nullptr;
    void (*confirm_power)(AppState *, bool is_shutdown) = nullptr;
    void (*notify_webui_sync)(const char *action) = nullptr;
    void (*pick_wallpaper)(AppState *) = nullptr;
    void (*pick_offline_ota)(AppState *) = nullptr;
    void (*window_toggle)(uint64_t wid) = nullptr;
    void (*window_maximize)(uint64_t wid) = nullptr;
    void (*window_minimize)(uint64_t wid) = nullptr;
    void (*window_close)(uint64_t wid) = nullptr;
    void (*window_quit)(uint64_t wid) = nullptr;
    void (*workspace_switch)(int index_1based) = nullptr;
    void (*workspace_step)(int delta) = nullptr;
};

// Parses either a structured JSON request {"id", "method", "params"} or a
// legacy colon/newline sysbridge command string into a normalized IpcRequest.
bool parse_ipc_request(const std::string &raw, IpcRequest &out);

// Dispatches an IPC request against the handler registry. Returns true if a
// matching handler was found. If reply_json_out is non-null and the request
// provided a JSON id, populates the JSON reply envelope.
bool dispatch_ipc_command(AppState *state,
                          const std::string &raw,
                          const IpcHostActions &actions,
                          std::string *reply_json_out = nullptr);
