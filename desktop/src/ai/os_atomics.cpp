#include "ai/os_atomics.hpp"
#include "ai/agent_perms.hpp"
#include "ai/task_inbox.hpp"
#include "common.hpp"
#include "desktop_apps.hpp"
#include "sandbox_manager.hpp"
#include "system_control.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;

namespace spinel::ai {

namespace {

std::mutex g_hooks_mu;
DesktopContextHooks g_hooks;

struct PendingApprovalSlot {
    ApprovalRequestInfo info;
    bool resolved = false;
    std::condition_variable cv;
};

std::mutex g_approval_mu;
std::map<std::string, std::shared_ptr<PendingApprovalSlot>> g_approvals;
std::atomic<unsigned long long> g_approval_seq{0};
std::function<void()> g_approval_cb;

bool is_allowed_service_name(const std::string &svc) {
    return svc == "ollama" || svc == "vllm" || svc == "docker" || svc == "cups";
}

void notify_approval_cb() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(g_approval_mu);
        cb = g_approval_cb;
    }
    if (cb) cb();
}

}  // namespace

void set_desktop_context_hooks(DesktopContextHooks hooks) {
    std::lock_guard<std::mutex> lock(g_hooks_mu);
    g_hooks = std::move(hooks);
}

std::vector<WindowBindingRef> query_desktop_windows() {
    std::function<std::vector<WindowBindingRef>()> fn;
    {
        std::lock_guard<std::mutex> lock(g_hooks_mu);
        fn = g_hooks.list_windows;
    }
    return fn ? fn() : std::vector<WindowBindingRef>{};
}

std::string collect_desktop_context_json() {
    DesktopContextHooks hooks_copy;
    {
        std::lock_guard<std::mutex> lock(g_hooks_mu);
        hooks_copy = g_hooks;
    }

    auto windows = hooks_copy.list_windows ? hooks_copy.list_windows() : std::vector<WindowBindingRef>{};
    auto workspaces = hooks_copy.list_workspaces ? hooks_copy.list_workspaces() : std::vector<WorkspaceBindingRef>{};
    TelemetrySnapshot tel = collect_telemetry();
    MediaSnapshot media = collect_media_state();

    const WindowBindingRef *active_win = nullptr;
    for (const auto &w : windows) {
        if (w.activated) {
            active_win = &w;
            break;
        }
    }

    int active_ws = -1;
    for (const auto &ws : workspaces) {
        if (ws.active) {
            active_ws = ws.index + 1;
            break;
        }
    }

    std::ostringstream oss;
    oss << "{"
        << "\"active_workspace\":" << active_ws << ","
        << "\"active_window\":"
        << (active_win
                ? ("{\"id\":" + std::to_string(active_win->id) + ",\"app_id\":\"" +
                   json_escape(active_win->app_id) + "\",\"title\":\"" + json_escape(active_win->title) + "\"}")
                : "null")
        << ","
        << "\"windows\":[";
    for (size_t i = 0; i < windows.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "{\"id\":" << windows[i].id
            << ",\"app_id\":\"" << json_escape(windows[i].app_id)
            << "\",\"title\":\"" << json_escape(windows[i].title)
            << "\",\"activated\":" << (windows[i].activated ? "true" : "false") << "}";
    }
    oss << "],\"workspaces\":[";
    for (size_t i = 0; i < workspaces.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "{\"index\":" << (workspaces[i].index + 1)
            << ",\"name\":\"" << json_escape(workspaces[i].name)
            << "\",\"active\":" << (workspaces[i].active ? "true" : "false")
            << ",\"urgent\":" << (workspaces[i].urgent ? "true" : "false") << "}";
    }
    oss << "],\"telemetry\":{"
        << "\"gpu\":\"" << json_escape(tel.gpu) << "\","
        << "\"mem\":\"" << json_escape(tel.mem) << "\","
        << "\"net\":\"" << json_escape(tel.net) << "\","
        << "\"kernel\":\"" << json_escape(tel.kernel) << "\","
        << "\"spinel_version\":\"" << json_escape(tel.spinel_version) << "\","
        << "\"battery_pct\":" << tel.battery_pct << ","
        << "\"power_profile\":\"" << json_escape(media.power_profile) << "\","
        << "\"is_live\":" << (tel.is_live ? "true" : "false")
        << "}}";
    return oss.str();
}

std::string collect_clipboard_recent_json(size_t limit) {
    auto items = load_clipboard_history();
    if (limit > 0 && items.size() > limit) items.resize(limit);
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "{\"index\":" << i
            << ",\"ts\":" << items[i].ts
            << ",\"text\":\"" << json_escape(items[i].text) << "\"}";
    }
    oss << "]";
    return oss.str();
}

std::string collect_notifications_recent_json(size_t limit) {
    auto items = fetch_notification_history();
    if (limit > 0 && items.size() > limit) items.resize(limit);
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "{\"id\":" << items[i].id
            << ",\"app_name\":\"" << json_escape(items[i].app_name)
            << "\",\"summary\":\"" << json_escape(items[i].summary)
            << "\",\"body\":\"" << json_escape(items[i].body) << "\"}";
    }
    oss << "]";
    return oss.str();
}

std::string collect_logs_tail_json(const std::string &source, size_t max_lines) {
    std::string clean_src = (source == "shell" || source == "compositor" || source == "system") ? source : "apps";
    size_t clamped = std::clamp<size_t>(max_lines, 10, 400);
    std::string txt = collect_redacted_logs_tail(clean_src, clamped);
    std::ostringstream oss;
    oss << "{\"source\":\"" << clean_src << "\",\"text\":\"" << json_escape(txt) << "\"}";
    return oss.str();
}

std::string collect_doctor_context_json() {
    return collect_doctor_summary_json();
}

bool capture_headless_screenshot(const std::string &mode, std::string &out_path, std::string &error_out) {
    out_path.clear();
    error_out.clear();
    if (!has_executable("grim")) {
        error_out = "系统未安装 grim 截图工具。";
        return false;
    }

    const char *xdg = std::getenv("XDG_RUNTIME_DIR");
    fs::path cap_dir = (xdg && *xdg) ? (fs::path(xdg) / "spinelos/captures")
                                     : fs::path("/tmp/spinelos-" + std::to_string(::getuid()) + "/captures");
    std::error_code ec;
    fs::create_directories(cap_dir, ec);
    if (ec) {
        error_out = "无法创建截图目录: " + ec.message();
        return false;
    }

    long long ts = static_cast<long long>(std::time(nullptr));
    static std::atomic<unsigned long long> seq{0};
    fs::path target = cap_dir / ("cap-" + std::to_string(ts) + "-" + std::to_string(seq.fetch_add(1) + 1) + ".png");

    std::vector<std::string> args = {"grim"};
    if (mode == "area" && has_executable("slurp")) {
        std::string geom;
        int rc_slurp = 1;
        if (!run_argv_capture({"slurp"}, geom, rc_slurp) || rc_slurp != 0 || trim_copy(geom).empty()) {
            error_out = "区域选择已取消或当前无可用图形会话。";
            return false;
        }
        args.push_back("-g");
        args.push_back(trim_copy(geom));
    }
    args.push_back(target.string());

    std::string cmd_out;
    int rc = 1;
    if (!run_argv_capture(args, cmd_out, rc) || rc != 0 || !fs::exists(target, ec)) {
        error_out = cmd_out.empty() ? "grim 截图失败（请检查 WAYLAND_DISPLAY）。" : trim_copy(cmd_out);
        return false;
    }
    out_path = target.string();

    // Keep at most 20 recent captures in tmpfs to avoid unbounded growth.
    std::vector<fs::path> old_caps;
    for (const auto &de : fs::directory_iterator(cap_dir, ec)) {
        if (!de.is_regular_file(ec)) continue;
        std::string fn = de.path().filename().string();
        if (fn.rfind("cap-", 0) == 0 && de.path().extension() == ".png" && de.path() != target) {
            old_caps.push_back(de.path());
        }
    }
    if (old_caps.size() > 19) {
        std::sort(old_caps.begin(), old_caps.end());
        for (size_t i = 0; i + 19 < old_caps.size(); ++i) {
            fs::remove(old_caps[i], ec);
        }
    }
    return true;
}

bool create_guarded_snapshot(const std::string &desc, std::string &out_json, std::string &error_out) {
    out_json.clear();
    error_out.clear();
    if (!has_executable("snapper")) {
        error_out = "系统未安装 snapper。";
        return false;
    }
    std::string clean_desc = trim_copy(desc);
    if (clean_desc.empty()) clean_desc = "Spinel Agent Checkpoint";
    if (!is_valid_snapper_desc(clean_desc)) {
        error_out = "快照描述包含非法字符或过长（仅支持常见中英文、数字及 -_.:/ 空格）。";
        return false;
    }

    std::vector<std::string> cmd = {
        "snapper", "-c", "root", "create", "--print-number",
        "--cleanup-algorithm", "number", "--description", clean_desc
    };
    if (::geteuid() != 0) {
        cmd.insert(cmd.begin(), {"sudo", "-n"});
    }

    std::string cmd_out;
    int rc = 1;
    if (!run_argv_capture(cmd, cmd_out, rc) || rc != 0) {
        error_out = trim_copy(cmd_out).empty()
                        ? "创建 Snapper 快照失败（需要 root/sudo -n 权限且已初始化 root 配置）。"
                        : trim_copy(cmd_out);
        return false;
    }
    std::string num = trim_copy(cmd_out);
    std::ostringstream oss;
    oss << "{\"snapshot_id\":\"" << json_escape(num)
        << "\",\"description\":\"" << json_escape(clean_desc) << "\"}";
    out_json = oss.str();
    return true;
}

bool launch_in_guarded_sandbox(const std::string &cmd, std::string &error_out) {
    std::string clean = trim_copy(cmd);
    if (clean.empty()) {
        error_out = "命令不能为空。";
        return false;
    }
    return launch_cmd_in_sandbox(clean, error_out);
}

std::string query_sandbox_status_json() {
    return sandbox_state_json();
}

std::string build_work_context_bundle(const std::string &task_id) {
    std::ostringstream oss;
    oss << "# SpinelOS 工作现场上下文\n\n";
    if (!task_id.empty()) {
        for (const auto &t : list_tasks()) {
            if (t.id == task_id) {
                oss << "## 关联任务\n"
                    << "- ID: " << t.id << "\n"
                    << "- 标题: " << t.title << "\n"
                    << "- 状态: " << task_status_str(t.status) << "\n";
                if (!t.repo_path.empty()) oss << "- 仓库目录: " << t.repo_path << "\n";
                if (!t.notes.empty()) oss << "- 备注: " << t.notes << "\n";
                oss << "\n";
                break;
            }
        }
    }
    oss << "## 当前桌面状态 (JSON)\n```json\n"
        << collect_desktop_context_json()
        << "\n```\n\n## 近期剪贴板 (最多 2 条)\n```json\n"
        << collect_clipboard_recent_json(2)
        << "\n```\n";
    return oss.str();
}

std::string build_ai_os_live_snapshot_json() {
    AgentPermConfig perms = load_agent_perms();
    auto sessions = reconcile_agent_sessions(0, 300, true, query_desktop_windows());
    auto approvals = list_pending_approvals();
    auto tasks = list_tasks();
    auto audits = tail_agent_audit(30);

    std::ostringstream oss;
    oss << "{"
        << "\"perms\":" << serialize_agent_perms_json(perms) << ","
        << "\"sessions\":" << serialize_agent_sessions_json(sessions) << ","
        << "\"approvals\":" << serialize_approvals_json(approvals) << ","
        << "\"tasks\":" << serialize_tasks_json(tasks) << ","
        << "\"audit\":" << serialize_audit_entries_json(audits) << ","
        << "\"mcp_clients\":" << serialize_mcp_clients_status_json()
        << "}";
    return oss.str();
}

bool evaluate_vram_yield_for_gamemode(bool gamemode_active,
                                      bool vram_yield_enabled,
                                      bool vllm_is_active,
                                      std::string &out_action) {
    if (!gamemode_active || !vram_yield_enabled || !vllm_is_active) {
        out_action = "none";
        return false;
    }
    out_action = "stop_vllm";
    return true;
}

std::string format_agent_island_pill_summary(const std::vector<AgentSessionInfo> &sessions,
                                             const std::vector<ApprovalRequestInfo> &approvals,
                                             const std::string &toast_text,
                                             std::string &out_state_cls) {
    if (!toast_text.empty()) {
        out_state_cls = "toast";
        return toast_text;
    }
    int waiting = 0, running = 0, errored = 0;
    std::string active_name;
    for (const auto &s : sessions) {
        if (s.status == AgentSessionStatus::WaitingUser) {
            ++waiting;
            if (active_name.empty()) active_name = s.agent;
        } else if (s.status == AgentSessionStatus::Running) {
            ++running;
            if (active_name.empty()) active_name = s.agent;
        } else if (s.status == AgentSessionStatus::Error) {
            ++errored;
        }
    }
    int total_wait = waiting + static_cast<int>(approvals.size());
    if (total_wait > 0) {
        out_state_cls = "alert";
        return std::to_string(total_wait) + " 待确认";
    }
    if (running > 0) {
        out_state_cls = "running";
        if (running == 1 && !active_name.empty()) {
            return active_name + " · 运行中";
        }
        return std::to_string(running) + " 运行中";
    }
    if (errored > 0) {
        out_state_cls = "alert";
        return std::to_string(errored) + " 异常";
    }
    out_state_cls = "idle";
    return "AI";
}

void sync_vram_with_gamemode(bool gamemode_active) {
    AgentPermConfig perms = load_agent_perms();
    std::string status_out;
    int rc = 1;
    bool vllm_active = run_argv_capture({"systemctl", "--user", "is-active", "--quiet", "spinel-vllm.service"},
                                        status_out, rc) && rc == 0;
    std::string act;
    if (!evaluate_vram_yield_for_gamemode(gamemode_active, perms.vram_yield_on_gamemode, vllm_active, act)) {
        return;
    }
    std::string stop_out;
    int stop_rc = 1;
    bool stopped = run_argv_capture({"systemctl", "--user", "stop", "spinel-vllm.service"}, stop_out, stop_rc) &&
                   stop_rc == 0;
    append_agent_audit("gamemode", "action.vram_yield", PermDecision::Allow,
                       stopped ? "yielded spinel-vllm.service for GameMode"
                               : ("failed to stop spinel-vllm.service: " + trim_copy(stop_out)));
}

bool focus_workspace_or_window(int workspace_1based, uint64_t win_id, std::string &error_out) {
    error_out.clear();
    DesktopContextHooks hooks_copy;
    {
        std::lock_guard<std::mutex> lock(g_hooks_mu);
        hooks_copy = g_hooks;
    }
    bool did_any = false;
    if (workspace_1based >= 1) {
        if (!hooks_copy.activate_workspace || !hooks_copy.activate_workspace(workspace_1based)) {
            error_out = "无法切换工作区（未连接桌面合成器）。";
            return false;
        }
        did_any = true;
    }
    if (win_id > 0) {
        if (!hooks_copy.activate_window || !hooks_copy.activate_window(win_id)) {
            error_out = "无法激活目标窗口（未找到窗口句柄或未连接合成器）。";
            return false;
        }
        did_any = true;
    }
    if (!did_any) {
        error_out = "请至少提供 workspace 或 win_id。";
        return false;
    }
    return true;
}

std::string query_proxy_status_json() {
    ProxyConfig p = resolve_effective_proxy();
    std::ostringstream oss;
    oss << "{"
        << "\"enabled\":" << (p.enabled ? "true" : "false") << ","
        << "\"http_proxy\":\"" << json_escape(p.http_proxy) << "\","
        << "\"socks_proxy\":\"" << json_escape(p.socks_proxy) << "\","
        << "\"no_proxy\":\"" << json_escape(p.no_proxy) << "\""
        << "}";
    return oss.str();
}

bool query_service_status_json(const std::string &service, std::string &out_json, std::string &error_out) {
    if (!is_allowed_service_name(service)) {
        error_out = "仅支持查询白名单服务: ollama | vllm | docker | cups";
        return false;
    }
    std::vector<std::string> args = {"systemctl"};
    if (service == "vllm") {
        args.insert(args.end(), {"--user", "is-active", "--quiet", "spinel-vllm.service"});
    } else {
        args.insert(args.end(), {"is-active", "--quiet", service + ".service"});
    }
    std::string out;
    int rc = 1;
    bool active = run_argv_capture(args, out, rc) && rc == 0;
    out_json = "{\"service\":\"" + json_escape(service) + "\",\"active\":" + (active ? "true" : "false") + "}";
    return true;
}

bool control_host_service(const std::string &service, const std::string &action, std::string &error_out) {
    if (!is_allowed_service_name(service)) {
        error_out = "不支持的服务名: " + service;
        return false;
    }
    if (action != "start" && action != "stop") {
        error_out = "action 仅支持 start 或 stop";
        return false;
    }
    std::vector<std::string> args;
    if (service == "vllm") {
        args = {"systemctl", "--user", action, "spinel-vllm.service"};
    } else {
        if (::geteuid() != 0) args = {"sudo", "-n"};
        args.insert(args.end(), {"systemctl", action, service + ".service"});
    }
    std::string out;
    int rc = 1;
    if (!run_argv_capture(args, out, rc) || rc != 0) {
        error_out = trim_copy(out).empty() ? "执行 systemctl 操作失败（可能需要免密授权）" : trim_copy(out);
        return false;
    }
    return true;
}

ApprovalRequestInfo create_pending_approval(const std::string &client_id,
                                            const std::string &capability,
                                            const std::string &summary,
                                            const std::string &detail) {
    auto slot = std::make_shared<PendingApprovalSlot>();
    long long now = static_cast<long long>(std::time(nullptr));
    unsigned long long seq = g_approval_seq.fetch_add(1) + 1;
    slot->info.id = "apr_" + std::to_string(now) + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq);
    slot->info.client_id = client_id.empty() ? "agent" : client_id;
    slot->info.capability = capability;
    slot->info.summary = summary;
    slot->info.detail = detail;
    slot->info.state = "pending";
    slot->info.created_at = now;

    {
        std::lock_guard<std::mutex> lock(g_approval_mu);
        g_approvals[slot->info.id] = slot;
    }
    notify_approval_cb();
    return slot->info;
}

ApprovalRequestInfo request_approval_blocking(const std::string &client_id,
                                              const std::string &capability,
                                              const std::string &summary,
                                              const std::string &detail,
                                              int timeout_ms) {
    ApprovalRequestInfo initial = create_pending_approval(client_id, capability, summary, detail);
    std::shared_ptr<PendingApprovalSlot> slot;
    {
        std::lock_guard<std::mutex> lock(g_approval_mu);
        auto it = g_approvals.find(initial.id);
        if (it == g_approvals.end()) return initial;
        slot = it->second;
    }

    int clamped_ms = std::clamp(timeout_ms, 50, 300000);
    ApprovalRequestInfo result;
    {
        std::unique_lock<std::mutex> lock(g_approval_mu);
        bool done = slot->cv.wait_for(lock, std::chrono::milliseconds(clamped_ms), [&]() {
            return slot->resolved;
        });
        if (!done && !slot->resolved) {
            slot->info.state = "timeout";
            slot->resolved = true;
        }
        result = slot->info;
        g_approvals.erase(initial.id);
    }
    notify_approval_cb();
    return result;
}

bool resolve_pending_approval(const std::string &approval_id, const std::string &decision) {
    std::string d = trim_copy(decision);
    if (d != "allow" && d != "sandbox" && d != "deny") return false;

    std::shared_ptr<PendingApprovalSlot> slot;
    {
        std::lock_guard<std::mutex> lock(g_approval_mu);
        auto it = g_approvals.find(trim_copy(approval_id));
        if (it == g_approvals.end()) return false;
        slot = it->second;
        slot->info.state = d;
        slot->resolved = true;
        slot->cv.notify_all();
    }
    notify_approval_cb();
    return true;
}

std::vector<ApprovalRequestInfo> list_pending_approvals() {
    std::lock_guard<std::mutex> lock(g_approval_mu);
    std::vector<ApprovalRequestInfo> out;
    for (const auto &kv : g_approvals) {
        if (!kv.second->resolved && kv.second->info.state == "pending") {
            out.push_back(kv.second->info);
        }
    }
    return out;
}

std::string serialize_approvals_json(const std::vector<ApprovalRequestInfo> &items) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) oss << ",";
        const auto &a = items[i];
        oss << "{"
            << "\"id\":\"" << json_escape(a.id) << "\","
            << "\"client_id\":\"" << json_escape(a.client_id) << "\","
            << "\"capability\":\"" << json_escape(a.capability) << "\","
            << "\"summary\":\"" << json_escape(a.summary) << "\","
            << "\"detail\":\"" << json_escape(a.detail) << "\","
            << "\"state\":\"" << json_escape(a.state) << "\","
            << "\"created_at\":" << a.created_at
            << "}";
    }
    oss << "]";
    return oss.str();
}

void set_approval_changed_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(g_approval_mu);
    g_approval_cb = std::move(cb);
}

void cancel_all_pending_approvals() {
    bool any = false;
    {
        std::lock_guard<std::mutex> lock(g_approval_mu);
        for (auto &kv : g_approvals) {
            if (!kv.second->resolved) {
                kv.second->info.state = "deny";
                kv.second->resolved = true;
                kv.second->cv.notify_all();
                any = true;
            }
        }
    }
    if (any) notify_approval_cb();
}

void reset_os_atomics_for_tests() {
    {
        std::lock_guard<std::mutex> lock(g_hooks_mu);
        g_hooks = DesktopContextHooks{};
    }
    {
        std::lock_guard<std::mutex> lock(g_approval_mu);
        for (auto &kv : g_approvals) {
            kv.second->info.state = "deny";
            kv.second->resolved = true;
            kv.second->cv.notify_all();
        }
        g_approvals.clear();
        g_approval_cb = nullptr;
    }
}

}  // namespace spinel::ai
