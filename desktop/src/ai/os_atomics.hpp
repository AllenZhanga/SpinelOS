#pragma once

#include "ai/agent_tracker.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spinel::ai {

struct WorkspaceBindingRef {
    int index = 0;
    std::string name;
    bool active = false;
    bool urgent = false;
};

struct DesktopContextHooks {
    std::function<std::vector<WindowBindingRef>()> list_windows;
    std::function<std::vector<WorkspaceBindingRef>()> list_workspaces;
    std::function<bool(uint64_t wid)> activate_window;
    std::function<bool(int idx1)> activate_workspace;
};

struct ApprovalRequestInfo {
    std::string id;
    std::string client_id;
    std::string capability;
    std::string summary;
    std::string detail;
    std::string state = "pending";  // "pending" | "allow" | "sandbox" | "deny" | "timeout"
    long long created_at = 0;
};

/// Install compositor/window hooks (from spinel-shell or unit-test mocks).
void set_desktop_context_hooks(DesktopContextHooks hooks);

/// Current tracked windows (via installed hooks, or empty in headless mode).
std::vector<WindowBindingRef> query_desktop_windows();

/// Collect read-only desktop context (active app, windows, workspaces, telemetry summary).
std::string collect_desktop_context_json();

/// Read up to `limit` recent clipboard entries cleanly formatted as JSON.
std::string collect_clipboard_recent_json(size_t limit = 5);

/// Read up to `limit` recent desktop notifications as JSON.
std::string collect_notifications_recent_json(size_t limit = 8);

/// Read up to `max_lines` redacted log tail lines for `source` ("apps"|"shell"|"compositor"|"system").
std::string collect_logs_tail_json(const std::string &source, size_t max_lines = 80);

/// Collect Spinel Doctor system health summary JSON.
std::string collect_doctor_context_json();

/// Headless screen capture ("full"|"area") into $XDG_RUNTIME_DIR/spinelos/captures/.
bool capture_headless_screenshot(const std::string &mode, std::string &out_path, std::string &error_out);

/// Create a guarded Btrfs Snapper checkpoint (create-only; never deletes or rolls back).
bool create_guarded_snapshot(const std::string &desc, std::string &out_json, std::string &error_out);

/// Launch a command or desktop URL inside the isolated Spinel parallel sandbox.
bool launch_in_guarded_sandbox(const std::string &cmd, std::string &error_out);

/// Query current Spinel parallel sandbox status as JSON.
std::string query_sandbox_status_json();

/// Package active window, workspace, clipboard tail, and optional Task Inbox item into a pasteable prompt bundle.
std::string build_work_context_bundle(const std::string &task_id = "");

/// Build consolidated live AI OS state JSON (perms, sessions, approvals, tasks, recent audit logs).
std::string build_ai_os_live_snapshot_json();

/// Pure decision logic for GameMode <-> local inference VRAM coordination.
bool evaluate_vram_yield_for_gamemode(bool gamemode_active,
                                      bool vram_yield_enabled,
                                      bool vllm_is_active,
                                      std::string &out_action);

/// Compute Dynamic Island pill display label and visual state class ("idle"|"running"|"alert"|"toast").
std::string format_agent_island_pill_summary(const std::vector<AgentSessionInfo> &sessions,
                                             const std::vector<ApprovalRequestInfo> &approvals,
                                             const std::string &toast_text,
                                             std::string &out_state_cls);

/// Yield local user-scoped heavy inference (`spinel-vllm.service`) when GameMode activates.
void sync_vram_with_gamemode(bool gamemode_active);

/// Focus a tracked workspace (1-based) and/or window via installed desktop hooks.
bool focus_workspace_or_window(int workspace_1based, uint64_t win_id, std::string &error_out);

/// Query effective system proxy config as JSON.
std::string query_proxy_status_json();

/// Query whitelisted service status ("ollama"|"vllm"|"docker"|"cups").
bool query_service_status_json(const std::string &service, std::string &out_json, std::string &error_out);

/// Start or stop a whitelisted service ("ollama"|"vllm"|"docker"|"cups").
bool control_host_service(const std::string &service, const std::string &action, std::string &error_out);

/// Create a pending user approval request and block until resolved or `timeout_ms` expires.
ApprovalRequestInfo request_approval_blocking(const std::string &client_id,
                                              const std::string &capability,
                                              const std::string &summary,
                                              const std::string &detail,
                                              int timeout_ms = 60000);

/// Create a non-blocking pending approval item (for UI / async flows).
ApprovalRequestInfo create_pending_approval(const std::string &client_id,
                                            const std::string &capability,
                                            const std::string &summary,
                                            const std::string &detail);

/// Resolve a pending approval ("allow" | "sandbox" | "deny").
bool resolve_pending_approval(const std::string &approval_id, const std::string &decision);

/// Snapshot all currently pending approvals.
std::vector<ApprovalRequestInfo> list_pending_approvals();
std::string serialize_approvals_json(const std::vector<ApprovalRequestInfo> &items);

void set_approval_changed_callback(std::function<void()> cb);
void cancel_all_pending_approvals();
void reset_os_atomics_for_tests();

}  // namespace spinel::ai
