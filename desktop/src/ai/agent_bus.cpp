#include "ai/agent_bus.hpp"
#include "ai/agent_perms.hpp"
#include "ai/agent_tracker.hpp"
#include "ai/json_util.hpp"
#include "ai/os_atomics.hpp"
#include "ai/task_inbox.hpp"
#include "common.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace spinel::ai {

namespace {

std::mutex g_srv_mu;
std::atomic<bool> g_srv_running{false};
std::thread g_srv_thread;
int g_listen_fd = -1;
int g_wake_pipe[2]{-1, -1};
std::string g_bound_sock;

std::string make_envelope(const std::string &id_raw,
                          bool ok,
                          const std::string &decision,
                          const std::string &result_json,
                          const std::string &error_msg,
                          const std::string &extra_json = "") {
    std::ostringstream oss;
    oss << "{";
    if (!id_raw.empty()) oss << "\"id\":" << id_raw << ",";
    oss << "\"ok\":" << (ok ? "true" : "false") << ","
        << "\"decision\":\"" << json_escape(decision) << "\"";
    if (!result_json.empty()) oss << ",\"result\":" << result_json;
    if (!error_msg.empty()) oss << ",\"error\":\"" << json_escape(error_msg) << "\"";
    if (!extra_json.empty()) oss << "," << extra_json;
    oss << "}";
    return oss.str();
}

std::string param_str(const std::map<std::string, std::string> &p, const char *k, const std::string &def = "") {
    auto it = p.find(k);
    return (it != p.end() && !it->second.empty()) ? it->second : def;
}

void handle_client_socket(int cfd) {
    std::string req_buf;
    char chunk[4096];
    while (g_srv_running.load()) {
        pollfd pfd{cfd, POLLIN, 0};
        int pr = ::poll(&pfd, 1, 2000);
        if (pr <= 0) {
            if (pr < 0 && errno == EINTR) continue;
            break;
        }
        ssize_t r = ::recv(cfd, chunk, sizeof(chunk), 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            break;
        }
        req_buf.append(chunk, static_cast<size_t>(r));
        auto nl = req_buf.find('\n');
        if (nl != std::string::npos) {
            std::string line = req_buf.substr(0, nl);
            std::string resp = dispatch_agent_bus_json(line, true, true) + "\n";
            size_t off = 0;
            while (off < resp.size()) {
                ssize_t w = ::send(cfd, resp.data() + off, resp.size() - off, MSG_NOSIGNAL);
                if (w <= 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                off += static_cast<size_t>(w);
            }
            break;
        }
        if (req_buf.size() > 262144) break;
    }
    ::close(cfd);
}

void bus_accept_loop() {
    while (g_srv_running.load()) {
        pollfd pfds[2] = {
            {g_listen_fd, POLLIN, 0},
            {g_wake_pipe[0], POLLIN, 0}
        };
        int pr = ::poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfds[1].revents & POLLIN) {
            break;
        }
        if (pfds[0].revents & POLLIN) {
            int cfd = ::accept(g_listen_fd, nullptr, nullptr);
            if (cfd >= 0) {
                std::thread(handle_client_socket, cfd).detach();
            }
        }
    }
}

}  // namespace

std::string dispatch_agent_bus_json(const std::string &req_json,
                                    bool enforce_perms,
                                    bool allow_blocking_approval) {
    std::string id_raw;
    std::string method;
    std::string client_id = "local";
    std::string params_raw = "{}";
    bool interactive_flag = false;

    bool parsed = json_parse_object_str(req_json, [&](const std::string &k, const std::string &v, const std::string &raw) {
        if (k == "id") id_raw = raw;
        else if (k == "method") method = trim_copy(v);
        else if (k == "client" || k == "client_id") client_id = trim_copy(v);
        else if (k == "params" && !raw.empty() && raw.front() == '{') params_raw = raw;
        else if (k == "interactive") interactive_flag = json_scalar_bool(v, false);
    });
    if (!parsed || method.empty()) {
        return make_envelope(id_raw, false, "deny", "", "请求格式错误或缺少 method 字段");
    }

    std::map<std::string, std::string> p;
    json_parse_flat_object(params_raw, p);

    PermDecision decision = PermDecision::Allow;
    std::string deny_reason;
    if (enforce_perms) {
        decision = check_agent_capability(method, deny_reason);
        if (decision == PermDecision::Deny) {
            append_agent_audit(client_id, method, decision, deny_reason);
            return make_envelope(id_raw, false, perm_decision_str(decision), "", deny_reason);
        }
        if (decision == PermDecision::RequireApproval) {
            std::string summary = param_str(p, "summary", method);
            std::string detail = param_str(p, "detail", params_raw);
            if (allow_blocking_approval && interactive_flag) {
                int timeout_ms = static_cast<int>(json_scalar_i64(param_str(p, "timeout_ms", "60000"), 60000));
                ApprovalRequestInfo apr = request_approval_blocking(client_id, method, summary, detail, timeout_ms);
                if (apr.state == "allow") {
                    decision = PermDecision::Allow;
                } else {
                    append_agent_audit(client_id, method, PermDecision::Deny, "approval=" + apr.state);
                    return make_envelope(
                        id_raw, false, apr.state, "",
                        "用户审批结果: " + apr.state,
                        "\"approval_id\":\"" + json_escape(apr.id) + "\"");
                }
            } else {
                ApprovalRequestInfo apr = create_pending_approval(client_id, method, summary, detail);
                append_agent_audit(client_id, method, decision, "pending=" + apr.id);
                return make_envelope(
                    id_raw, false, perm_decision_str(decision), "", deny_reason,
                    "\"approval_id\":\"" + json_escape(apr.id) + "\"");
            }
        }
    }

    // Dispatch verified method
    if (method == "ping") {
        append_agent_audit(client_id, method, PermDecision::Allow, "pong");
        return make_envelope(id_raw, true, "allow", "{\"pong\":true}", "");
    }

    if (method == "agent.report") {
        AgentSessionInfo info;
        info.id = param_str(p, "id");
        info.agent = param_str(p, "agent", "Agent");
        info.title = param_str(p, "title");
        info.repo_path = param_str(p, "repo", param_str(p, "repo_path"));
        info.pid = static_cast<pid_t>(json_scalar_i64(param_str(p, "pid", "0"), 0));
        info.status = parse_agent_status(param_str(p, "status", "running"));
        info.summary = param_str(p, "summary");
        info.win_id = static_cast<uint64_t>(std::max(0LL, json_scalar_i64(param_str(p, "win_id", "0"), 0)));
        info.workspace_idx = static_cast<int>(json_scalar_i64(param_str(p, "workspace_idx", "-1"), -1));
        info.in_sandbox = json_scalar_bool(param_str(p, "in_sandbox", "false"), false);
        info.task_id = param_str(p, "task_id");
        info.approval_id = param_str(p, "approval_id");
        info.source = param_str(p, "source", "ctl");

        AgentTransitionEvent ev = upsert_agent_session(info, true);
        sync_task_with_agent_transition(ev);
        append_agent_audit(client_id, method, PermDecision::Allow, ev.session_id + ":" + agent_status_str(ev.new_status));
        return make_envelope(id_raw, true, "allow", serialize_agent_session_json(ev.session), "");
    }

    if (method == "agent.list") {
        bool do_proc = json_scalar_bool(param_str(p, "include_proc", "true"), true);
        auto sessions = reconcile_agent_sessions(0, 300, do_proc, query_desktop_windows());
        append_agent_audit(client_id, method, PermDecision::Allow, std::to_string(sessions.size()));
        return make_envelope(id_raw, true, "allow", serialize_agent_sessions_json(sessions), "");
    }

    if (method == "agent.clear") {
        bool ok = clear_agent_session(param_str(p, "id"));
        append_agent_audit(client_id, method, PermDecision::Allow, param_str(p, "id"));
        return make_envelope(id_raw, ok, "allow", ok ? "{\"cleared\":true}" : "", ok ? "" : "未找到指定会话");
    }

    if (method == "todo.list") {
        auto items = list_tasks(param_str(p, "status"), param_str(p, "repo", param_str(p, "repo_path")));
        append_agent_audit(client_id, method, PermDecision::Allow, std::to_string(items.size()));
        return make_envelope(id_raw, true, "allow", serialize_tasks_json(items), "");
    }

    if (method == "todo.create") {
        AgentTaskItem item;
        bool ok = create_task(
            param_str(p, "title"),
            param_str(p, "repo", param_str(p, "repo_path")),
            param_str(p, "preferred_agent", param_str(p, "agent")),
            param_str(p, "priority", "normal"),
            param_str(p, "notes"),
            item);
        append_agent_audit(client_id, method, PermDecision::Allow, ok ? item.id : "invalid");
        return make_envelope(id_raw, ok, "allow", ok ? serialize_task_json(item) : "", ok ? "" : "任务标题不能为空或无法写盘");
    }

    if (method == "todo.claim") {
        AgentTaskItem item;
        bool ok = claim_task(param_str(p, "id", param_str(p, "task_id")),
                             param_str(p, "session_id"),
                             param_str(p, "agent"),
                             item);
        append_agent_audit(client_id, method, PermDecision::Allow, param_str(p, "id"));
        return make_envelope(id_raw, ok, "allow", ok ? serialize_task_json(item) : "", ok ? "" : "未找到指定任务");
    }

    if (method == "todo.complete") {
        AgentTaskItem item;
        bool to_review = json_scalar_bool(param_str(p, "review", "true"), true);
        bool ok = complete_task(param_str(p, "id", param_str(p, "task_id")),
                                param_str(p, "summary"), to_review, item);
        append_agent_audit(client_id, method, PermDecision::Allow, param_str(p, "id"));
        return make_envelope(id_raw, ok, "allow", ok ? serialize_task_json(item) : "", ok ? "" : "未找到指定任务");
    }

    if (method == "todo.accept") {
        AgentTaskItem item;
        bool ok = accept_task(param_str(p, "id", param_str(p, "task_id")), item);
        append_agent_audit(client_id, method, PermDecision::Allow, param_str(p, "id"));
        return make_envelope(id_raw, ok, "allow", ok ? serialize_task_json(item) : "", ok ? "" : "未找到指定任务");
    }

    if (method == "todo.reopen") {
        AgentTaskItem item;
        bool ok = reopen_task(param_str(p, "id", param_str(p, "task_id")), item);
        append_agent_audit(client_id, method, PermDecision::Allow, param_str(p, "id"));
        return make_envelope(id_raw, ok, "allow", ok ? serialize_task_json(item) : "", ok ? "" : "未找到指定任务");
    }

    if (method == "todo.delete") {
        bool ok = delete_task(param_str(p, "id", param_str(p, "task_id")));
        append_agent_audit(client_id, method, PermDecision::Allow, param_str(p, "id"));
        return make_envelope(id_raw, ok, "allow", ok ? "{\"deleted\":true}" : "", ok ? "" : "未找到指定任务");
    }

    if (method == "perms.get") {
        append_agent_audit(client_id, method, PermDecision::Allow, "");
        return make_envelope(id_raw, true, "allow", serialize_agent_perms_json(load_agent_perms()), "");
    }

    if (method == "perms.set") {
        std::ostringstream kv;
        for (const auto &item : p) kv << item.first << "=" << item.second << "\n";
        AgentPermConfig updated;
        bool ok = update_agent_perms_kv(kv.str(), updated);
        append_agent_audit(client_id, method, PermDecision::Allow, serialize_agent_perms_json(updated));
        return make_envelope(id_raw, ok, "allow", ok ? serialize_agent_perms_json(updated) : "", ok ? "" : "写入权限配置失败");
    }

    if (method == "audit.tail") {
        size_t limit = static_cast<size_t>(std::max(1LL, json_scalar_i64(param_str(p, "limit", "30"), 30)));
        auto entries = tail_agent_audit(limit);
        return make_envelope(id_raw, true, "allow", serialize_audit_entries_json(entries), "");
    }

    if (method == "context.desktop") {
        append_agent_audit(client_id, method, PermDecision::Allow, "");
        return make_envelope(id_raw, true, "allow", collect_desktop_context_json(), "");
    }

    if (method == "context.clipboard") {
        size_t limit = static_cast<size_t>(std::max(1LL, json_scalar_i64(param_str(p, "limit", "5"), 5)));
        append_agent_audit(client_id, method, PermDecision::Allow, "");
        return make_envelope(id_raw, true, "allow", collect_clipboard_recent_json(limit), "");
    }

    if (method == "context.notifications") {
        size_t limit = static_cast<size_t>(std::max(1LL, json_scalar_i64(param_str(p, "limit", "8"), 8)));
        append_agent_audit(client_id, method, PermDecision::Allow, "");
        return make_envelope(id_raw, true, "allow", collect_notifications_recent_json(limit), "");
    }

    if (method == "context.logs") {
        std::string src = param_str(p, "source", "apps");
        size_t lines = static_cast<size_t>(std::max(10LL, json_scalar_i64(param_str(p, "lines", "80"), 80)));
        append_agent_audit(client_id, method, PermDecision::Allow, src);
        return make_envelope(id_raw, true, "allow", collect_logs_tail_json(src, lines), "");
    }

    if (method == "context.doctor") {
        append_agent_audit(client_id, method, PermDecision::Allow, "");
        return make_envelope(id_raw, true, "allow", collect_doctor_context_json(), "");
    }

    if (method == "context.screenshot") {
        std::string out_path, err;
        bool ok = capture_headless_screenshot(param_str(p, "mode", "full"), out_path, err);
        append_agent_audit(client_id, method, PermDecision::Allow, ok ? out_path : err);
        return make_envelope(id_raw, ok, "allow", ok ? ("{\"path\":\"" + json_escape(out_path) + "\"}") : "", err);
    }

    if (method == "action.snapshot") {
        std::string out_json, err;
        bool ok = create_guarded_snapshot(param_str(p, "description", param_str(p, "desc")), out_json, err);
        append_agent_audit(client_id, method, PermDecision::Allow, ok ? out_json : err);
        return make_envelope(id_raw, ok, "allow", out_json, err);
    }

    if (method == "action.sandbox_launch") {
        std::string cmd = param_str(p, "cmd", param_str(p, "command"));
        std::string err;
        bool ok = launch_in_guarded_sandbox(cmd, err);
        append_agent_audit(client_id, method, PermDecision::Allow, cmd);
        return make_envelope(id_raw, ok, "allow", ok ? "{\"launched\":true}" : "", err);
    }

    if (method == "action.proxy_status") {
        append_agent_audit(client_id, method, PermDecision::Allow, "");
        return make_envelope(id_raw, true, "allow", query_proxy_status_json(), "");
    }

    if (method == "action.service_status") {
        std::string out_json, err;
        bool ok = query_service_status_json(param_str(p, "service"), out_json, err);
        append_agent_audit(client_id, method, PermDecision::Allow, param_str(p, "service"));
        return make_envelope(id_raw, ok, "allow", out_json, err);
    }

    if (method == "action.service_control") {
        std::string svc = param_str(p, "service");
        std::string act = param_str(p, "action");
        std::string err;
        bool ok = control_host_service(svc, act, err);
        append_agent_audit(client_id, method, PermDecision::Allow, svc + ":" + act);
        return make_envelope(id_raw, ok, "allow", ok ? "{\"ok\":true}" : "", err);
    }

    if (method == "action.workspace_focus") {
        int ws = static_cast<int>(json_scalar_i64(param_str(p, "workspace", "0"), 0));
        uint64_t wid = static_cast<uint64_t>(std::max(0LL, json_scalar_i64(param_str(p, "win_id", "0"), 0)));
        std::string err;
        bool ok = focus_workspace_or_window(ws, wid, err);
        append_agent_audit(client_id, method, PermDecision::Allow, "ws=" + std::to_string(ws) + ",wid=" + std::to_string(wid));
        return make_envelope(id_raw, ok, "allow", ok ? "{\"focused\":true}" : "", err);
    }

    if (method == "approval.request") {
        std::string cap = param_str(p, "capability", "approval.request");
        std::string sum = param_str(p, "summary", cap);
        std::string det = param_str(p, "detail");
        bool async_mode = json_scalar_bool(param_str(p, "async", "false"), false);
        if (async_mode || !allow_blocking_approval) {
            ApprovalRequestInfo apr = create_pending_approval(client_id, cap, sum, det);
            append_agent_audit(client_id, method, PermDecision::RequireApproval, apr.id);
            return make_envelope(id_raw, true, "require_approval", serialize_approvals_json({apr}), "");
        }
        int timeout_ms = static_cast<int>(json_scalar_i64(param_str(p, "timeout_ms", "60000"), 60000));
        ApprovalRequestInfo apr = request_approval_blocking(client_id, cap, sum, det, timeout_ms);
        bool granted = (apr.state == "allow" || apr.state == "sandbox");
        append_agent_audit(client_id, method, granted ? PermDecision::Allow : PermDecision::Deny, apr.id + "=" + apr.state);
        return make_envelope(id_raw, granted, apr.state, serialize_approvals_json({apr}),
                             granted ? "" : ("审批未通过: " + apr.state));
    }

    if (method == "approval.list") {
        auto items = list_pending_approvals();
        return make_envelope(id_raw, true, "allow", serialize_approvals_json(items), "");
    }

    if (method == "approval.resolve") {
        std::string aid = param_str(p, "id", param_str(p, "approval_id"));
        std::string dec = param_str(p, "decision");
        bool ok = resolve_pending_approval(aid, dec);
        append_agent_audit(client_id, method, PermDecision::Allow, aid + "=" + dec);
        return make_envelope(id_raw, ok, "allow", ok ? "{\"resolved\":true}" : "", ok ? "" : "未找到挂起的审批或 decision 无效");
    }

    return make_envelope(id_raw, false, "deny", "", "未实现的方法: " + method);
}

bool start_agent_bus_server(std::string *bound_path_out) {
    std::lock_guard<std::mutex> lock(g_srv_mu);
    if (g_srv_running.load()) {
        if (bound_path_out) *bound_path_out = g_bound_sock;
        return true;
    }

    std::string sock_path = agent_bus_socket_path();
    if (sock_path.size() >= sizeof(sockaddr_un::sun_path)) {
        return false;
    }

    std::error_code ec;
    fs::create_directories(fs::path(sock_path).parent_path(), ec);
    fs::remove(sock_path, ec);

    if (::pipe(g_wake_pipe) != 0) {
        g_wake_pipe[0] = g_wake_pipe[1] = -1;
        return false;
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        ::close(g_wake_pipe[0]);
        ::close(g_wake_pipe[1]);
        g_wake_pipe[0] = g_wake_pipe[1] = -1;
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path.c_str());

    if (::bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 16) != 0) {
        ::close(fd);
        ::close(g_wake_pipe[0]);
        ::close(g_wake_pipe[1]);
        g_wake_pipe[0] = g_wake_pipe[1] = -1;
        return false;
    }

    ::chmod(sock_path.c_str(), 0600);
    g_listen_fd = fd;
    g_bound_sock = sock_path;
    g_srv_running.store(true);
    g_srv_thread = std::thread(bus_accept_loop);

    if (bound_path_out) *bound_path_out = g_bound_sock;
    return true;
}

void stop_agent_bus_server() {
    std::lock_guard<std::mutex> lock(g_srv_mu);
    if (!g_srv_running.exchange(false)) return;
    if (g_wake_pipe[1] >= 0) {
        const char b = 'x';
        ssize_t unused = ::write(g_wake_pipe[1], &b, 1);
        (void)unused;
    }
    if (g_srv_thread.joinable()) {
        g_srv_thread.join();
    }
    if (g_listen_fd >= 0) {
        ::close(g_listen_fd);
        g_listen_fd = -1;
    }
    if (g_wake_pipe[0] >= 0) {
        ::close(g_wake_pipe[0]);
        g_wake_pipe[0] = -1;
    }
    if (g_wake_pipe[1] >= 0) {
        ::close(g_wake_pipe[1]);
        g_wake_pipe[1] = -1;
    }
    if (!g_bound_sock.empty()) {
        std::error_code ec;
        fs::remove(g_bound_sock, ec);
        g_bound_sock.clear();
    }
}

bool is_agent_bus_running() {
    return g_srv_running.load();
}

}  // namespace spinel::ai
