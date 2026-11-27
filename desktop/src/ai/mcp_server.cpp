#include "ai/mcp_server.hpp"
#include "ai/agent_bus.hpp"
#include "ai/agent_perms.hpp"
#include "ai/agent_tracker.hpp"
#include "ai/json_util.hpp"
#include "ai/task_inbox.hpp"
#include "common.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace spinel::ai {

namespace {

struct McpToolDef {
    const char *name;
    const char *description;
    const char *schema_json;
};

constexpr McpToolDef kMcpTools[] = {
    {
        "agent_report",
        "Report or update the current AI coding agent session status (running, waiting_user, done, error) "
        "to SpinelOS Dynamic Island & Task Inbox.",
        R"({"type":"object","properties":{)"
        R"("id":{"type":"string","description":"Stable session identifier (optional; defaults to agent+pid)."},)"
        R"("agent":{"type":"string","description":"Agent name, e.g. Codex, Antigravity, Claude, DeepSeekHarness."},)"
        R"("status":{"type":"string","enum":["running","waiting_user","done","error"]},)"
        R"("title":{"type":"string","description":"Short human-readable session title."},)"
        R"("repo":{"type":"string","description":"Working repository directory path."},)"
        R"("summary":{"type":"string","description":"1-line progress or completion summary."},)"
        R"("pid":{"type":"integer","description":"Process ID of the agent runtime."},)"
        R"("task_id":{"type":"string","description":"Bound Spinel Task Inbox ID (auto-promotes to review on done)."},)"
        R"("in_sandbox":{"type":"boolean","description":"True if running inside spinel-sandbox."})"
        R"(},"required":["agent","status"]})"
    },
    {
        "agent_list",
        "List active and recently finished AI coding agent sessions observed by SpinelOS.",
        R"({"type":"object","properties":{)"
        R"("include_proc":{"type":"boolean","description":"Include /proc fallback discovery (default true)."})"
        R"(}})"
    },
    {
        "todo_list",
        "List items from the native Spinel Task Inbox (backlog, in_progress, review, done).",
        R"({"type":"object","properties":{)"
        R"("status":{"type":"string","enum":["backlog","in_progress","review","done"]},)"
        R"("repo":{"type":"string","description":"Optional repository path filter."})"
        R"(}})"
    },
    {
        "todo_create",
        "Create a new backlog task in Spinel Task Inbox for later or asynchronous agent execution.",
        R"({"type":"object","properties":{)"
        R"("title":{"type":"string","description":"Concise task title."},)"
        R"("repo":{"type":"string","description":"Target repository directory path."},)"
        R"("preferred_agent":{"type":"string","description":"Suggested agent, e.g. Codex or DeepSeekHarness."},)"
        R"("priority":{"type":"string","enum":["low","normal","high"]},)"
        R"("notes":{"type":"string","description":"Detailed instructions or context."})"
        R"(},"required":["title"]})"
    },
    {
        "todo_claim",
        "Claim a Spinel Task Inbox item and move it into in_progress bound to a session.",
        R"({"type":"object","properties":{)"
        R"("id":{"type":"string","description":"Task ID (e.g. t_...)."},)"
        R"("session_id":{"type":"string","description":"Bound agent session ID."},)"
        R"("agent":{"type":"string","description":"Agent name executing this task."})"
        R"(},"required":["id"]})"
    },
    {
        "todo_complete",
        "Mark a claimed Spinel Task Inbox item as completed and move it into review (or done).",
        R"({"type":"object","properties":{)"
        R"("id":{"type":"string","description":"Task ID."},)"
        R"("summary":{"type":"string","description":"Completion summary or verification result."},)"
        R"("review":{"type":"boolean","description":"True moves to review queue (default true); false marks done."})"
        R"(},"required":["id"]})"
    },
    {
        "desktop_get_context",
        "Get current SpinelOS desktop context: active workspace, focused app/title, open windows, and hardware telemetry.",
        R"({"type":"object","properties":{}})"
    },
    {
        "clipboard_recent",
        "Read recent clipboard history entries from SpinelOS (requires Tier 1 clipboard permission).",
        R"({"type":"object","properties":{)"
        R"("limit":{"type":"integer","minimum":1,"maximum":20,"description":"Maximum entries to return (default 5)."})"
        R"(}})"
    },
    {
        "screenshot_capture",
        "Capture the desktop or interactive region screenshot via grim and return image bytes + path (Tier 1 permission).",
        R"({"type":"object","properties":{)"
        R"("mode":{"type":"string","enum":["full","area"],"description":"Capture mode (default full)."},)"
        R"("include_image":{"type":"boolean","description":"Return base64 PNG image content in MCP response (default true)."})"
        R"(}})"
    },
    {
        "doctor_diagnose",
        "Run Spinel Doctor system health checks and return redacted diagnostics JSON (Tier 1 permission).",
        R"({"type":"object","properties":{}})"
    },
    {
        "logs_tail",
        "Read redacted tail lines from SpinelOS logs (apps, shell, compositor, or system; Tier 1 permission).",
        R"({"type":"object","properties":{)"
        R"("source":{"type":"string","enum":["apps","shell","compositor","system"]},)"
        R"("lines":{"type":"integer","minimum":10,"maximum":400})"
        R"(}})"
    },
    {
        "snapshot_checkpoint",
        "Create a protective Btrfs Snapper checkpoint before risky edits or system changes (Tier 2 create-only).",
        R"({"type":"object","properties":{)"
        R"("description":{"type":"string","description":"Short checkpoint description."})"
        R"(},"required":["description"]})"
    },
    {
        "sandbox_launch",
        "Launch a GUI app, browser verification URL, or test command inside the isolated Spinel parallel sandbox (Tier 2).",
        R"({"type":"object","properties":{)"
        R"("cmd":{"type":"string","description":"Command or URL to run inside spinel-sandbox."})"
        R"(},"required":["cmd"]})"
    },
    {
        "sandbox_status",
        "Inspect whether the isolated Spinel parallel sandbox is running, its socket, and Wayland display state.",
        R"({"type":"object","properties":{}})"
    },
    {
        "workspace_focus",
        "Switch workspace or focus a specific window on the host desktop (Tier 3 interactive approval if configured).",
        R"({"type":"object","properties":{)"
        R"("workspace":{"type":"integer","minimum":1,"description":"1-based workspace index."},)"
        R"("win_id":{"type":"integer","minimum":1,"description":"Tracked window handle ID."},)"
        R"("summary":{"type":"string","description":"Reason shown on user approval prompt."},)"
        R"("timeout_ms":{"type":"integer","minimum":500,"maximum":300000})"
        R"(}})"
    },
    {
        "service_control",
        "Query status or start/stop whitelisted local AI/infra services (ollama, vllm, docker, cups). start/stop uses Tier 3.",
        R"({"type":"object","properties":{)"
        R"("service":{"type":"string","enum":["ollama","vllm","docker","cups"]},)"
        R"("action":{"type":"string","enum":["status","start","stop"]},)"
        R"("summary":{"type":"string","description":"Reason shown when approval is required."},)"
        R"("timeout_ms":{"type":"integer","minimum":500,"maximum":300000})"
        R"(},"required":["service","action"]})"
    },
    {
        "proxy_status",
        "Query effective SpinelOS system proxy configuration (http_proxy, socks_proxy, no_proxy).",
        R"({"type":"object","properties":{}})"
    },
    {
        "request_user_approval",
        "Pause and ask the desktop user for an interactive decision (allow / sandbox / deny) on a sensitive step.",
        R"({"type":"object","properties":{)"
        R"("summary":{"type":"string","description":"Short prompt title shown to user."},)"
        R"("detail":{"type":"string","description":"Detailed context, diff, or command preview."},)"
        R"("capability":{"type":"string","description":"Logical capability tag."},)"
        R"("async":{"type":"boolean","description":"If true, queue non-blocking approval and return approval_id immediately."},)"
        R"("timeout_ms":{"type":"integer","minimum":500,"maximum":300000})"
        R"(},"required":["summary"]})"
    },
};

std::string mcp_client_label() {
    const char *env = std::getenv("SPINEL_AGENT_CLIENT");
    if (env && *env) return trim_copy(env);
    return "mcp";
}

std::string jsonrpc_ok(const std::string &id_raw, const std::string &result_json) {
    std::ostringstream oss;
    oss << "{\"jsonrpc\":\"2.0\",\"id\":" << (id_raw.empty() ? "null" : id_raw)
        << ",\"result\":" << result_json << "}";
    return oss.str();
}

std::string jsonrpc_err(const std::string &id_raw, int code, const std::string &msg) {
    std::ostringstream oss;
    oss << "{\"jsonrpc\":\"2.0\",\"id\":" << (id_raw.empty() ? "null" : id_raw)
        << ",\"error\":{\"code\":" << code << ",\"message\":\"" << json_escape(msg) << "\"}}";
    return oss.str();
}

std::string mcp_text_result(const std::string &text, bool is_error) {
    std::ostringstream oss;
    oss << "{\"content\":[{\"type\":\"text\",\"text\":\"" << json_escape(text) << "\"}],"
        << "\"isError\":" << (is_error ? "true" : "false") << "}";
    return oss.str();
}

std::string arg_str(const std::map<std::string, std::string> &m, const char *k, const std::string &def = "") {
    auto it = m.find(k);
    return (it != m.end() && !it->second.empty()) ? it->second : def;
}

bool read_file_bytes(const std::string &path, std::vector<unsigned char> &out, size_t max_bytes = 16 * 1024 * 1024) {
    out.clear();
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    if (ec || sz == 0 || sz > max_bytes) return false;
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;
    out.resize(static_cast<size_t>(sz));
    ifs.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(sz));
    return ifs.good() || ifs.gcount() == static_cast<std::streamsize>(sz);
}

bool is_offline_capable_method(const std::string &method) {
    return method == "ping" ||
           method == "agent.report" ||
           method == "agent.list" ||
           method == "todo.list" ||
           method == "todo.create" ||
           method == "todo.claim" ||
           method == "todo.complete";
}

// Local offline execution strictly mirrors agent_perms + audit logging so Tier 0 disable rules still apply.
std::string dispatch_offline_local(const std::string &client_id,
                                   const std::string &method,
                                   const std::map<std::string, std::string> &p) {
    std::string deny_reason;
    PermDecision dec = check_agent_capability(method, deny_reason);
    if (dec != PermDecision::Allow) {
        append_agent_audit(client_id, method, dec, deny_reason);
        return "{\"ok\":false,\"decision\":\"" + std::string(perm_decision_str(dec)) +
               "\",\"error\":\"" + json_escape(deny_reason) + "\"}";
    }

    if (method == "ping") {
        append_agent_audit(client_id, method, PermDecision::Allow, "offline-local");
        return "{\"ok\":true,\"decision\":\"allow\",\"result\":{\"pong\":true}}";
    }
    if (method == "agent.report") {
        AgentSessionInfo info;
        info.id = arg_str(p, "id");
        info.agent = arg_str(p, "agent", "Agent");
        info.title = arg_str(p, "title");
        info.repo_path = arg_str(p, "repo", arg_str(p, "repo_path"));
        info.pid = static_cast<pid_t>(json_scalar_i64(arg_str(p, "pid", "0"), 0));
        info.status = parse_agent_status(arg_str(p, "status", "running"));
        info.summary = arg_str(p, "summary");
        info.task_id = arg_str(p, "task_id");
        info.in_sandbox = json_scalar_bool(arg_str(p, "in_sandbox", "false"), false);
        info.source = "mcp";
        AgentTransitionEvent ev = upsert_agent_session(info, true);
        sync_task_with_agent_transition(ev);
        append_agent_audit(client_id, method, PermDecision::Allow, ev.session_id);
        return "{\"ok\":true,\"decision\":\"allow\",\"result\":" + serialize_agent_session_json(ev.session) + "}";
    }
    if (method == "agent.list") {
        bool do_proc = json_scalar_bool(arg_str(p, "include_proc", "true"), true);
        auto sessions = reconcile_agent_sessions(0, 300, do_proc, {});
        append_agent_audit(client_id, method, PermDecision::Allow, std::to_string(sessions.size()));
        return "{\"ok\":true,\"decision\":\"allow\",\"result\":" + serialize_agent_sessions_json(sessions) + "}";
    }
    if (method == "todo.list") {
        auto items = list_tasks(arg_str(p, "status"), arg_str(p, "repo", arg_str(p, "repo_path")));
        append_agent_audit(client_id, method, PermDecision::Allow, std::to_string(items.size()));
        return "{\"ok\":true,\"decision\":\"allow\",\"result\":" + serialize_tasks_json(items) + "}";
    }
    if (method == "todo.create") {
        AgentTaskItem item;
        bool ok = create_task(arg_str(p, "title"),
                              arg_str(p, "repo", arg_str(p, "repo_path")),
                              arg_str(p, "preferred_agent", arg_str(p, "agent")),
                              arg_str(p, "priority", "normal"),
                              arg_str(p, "notes"),
                              item);
        append_agent_audit(client_id, method, PermDecision::Allow, ok ? item.id : "invalid");
        return ok ? ("{\"ok\":true,\"decision\":\"allow\",\"result\":" + serialize_task_json(item) + "}")
                  : "{\"ok\":false,\"decision\":\"allow\",\"error\":\"任务标题不能为空或写盘失败\"}";
    }
    if (method == "todo.claim") {
        AgentTaskItem item;
        bool ok = claim_task(arg_str(p, "id", arg_str(p, "task_id")),
                             arg_str(p, "session_id"), arg_str(p, "agent"), item);
        append_agent_audit(client_id, method, PermDecision::Allow, arg_str(p, "id"));
        return ok ? ("{\"ok\":true,\"decision\":\"allow\",\"result\":" + serialize_task_json(item) + "}")
                  : "{\"ok\":false,\"decision\":\"allow\",\"error\":\"未找到指定任务\"}";
    }
    if (method == "todo.complete") {
        AgentTaskItem item;
        bool to_review = json_scalar_bool(arg_str(p, "review", "true"), true);
        bool ok = complete_task(arg_str(p, "id", arg_str(p, "task_id")),
                                arg_str(p, "summary"), to_review, item);
        append_agent_audit(client_id, method, PermDecision::Allow, arg_str(p, "id"));
        return ok ? ("{\"ok\":true,\"decision\":\"allow\",\"result\":" + serialize_task_json(item) + "}")
                  : "{\"ok\":false,\"decision\":\"allow\",\"error\":\"未找到指定任务\"}";
    }
    return "";
}

struct BusCallSpec {
    std::string method;
    std::string params_json = "{}";
    bool interactive = false;
    int timeout_ms = 5000;
    bool valid = true;
    std::string validation_err;
};

BusCallSpec map_tool_to_bus(const std::string &tool_name, const std::map<std::string, std::string> &args) {
    BusCallSpec spec;
    auto has_nonempty = [&](const char *k) { return !arg_str(args, k).empty(); };

    if (tool_name == "agent_report") {
        std::string agent = arg_str(args, "agent");
        std::string status = arg_str(args, "status");
        if (agent.empty() || status.empty()) {
            spec.valid = false;
            spec.validation_err = "agent_report 缺少必填参数 agent 或 status";
            return spec;
        }
        spec.method = "agent.report";
        std::ostringstream pj;
        pj << "{\"agent\":\"" << json_escape(agent) << "\""
           << ",\"status\":\"" << json_escape(status) << "\""
           << ",\"id\":\"" << json_escape(arg_str(args, "id")) << "\""
           << ",\"title\":\"" << json_escape(arg_str(args, "title")) << "\""
           << ",\"repo\":\"" << json_escape(arg_str(args, "repo")) << "\""
           << ",\"summary\":\"" << json_escape(arg_str(args, "summary")) << "\""
           << ",\"pid\":" << json_scalar_i64(arg_str(args, "pid", std::to_string(::getppid())), ::getppid())
           << ",\"task_id\":\"" << json_escape(arg_str(args, "task_id")) << "\""
           << ",\"in_sandbox\":" << (json_scalar_bool(arg_str(args, "in_sandbox", "false"), false) ? "true" : "false")
           << ",\"source\":\"mcp\"}";
        spec.params_json = pj.str();
        return spec;
    }

    if (tool_name == "agent_list") {
        spec.method = "agent.list";
        spec.params_json = std::string("{\"include_proc\":") +
                           (json_scalar_bool(arg_str(args, "include_proc", "true"), true) ? "true}" : "false}");
        return spec;
    }

    if (tool_name == "todo_list") {
        spec.method = "todo.list";
        spec.params_json = "{\"status\":\"" + json_escape(arg_str(args, "status")) +
                           "\",\"repo\":\"" + json_escape(arg_str(args, "repo")) + "\"}";
        return spec;
    }

    if (tool_name == "todo_create") {
        std::string title = arg_str(args, "title");
        if (title.empty()) {
            spec.valid = false;
            spec.validation_err = "todo_create 缺少必填参数 title";
            return spec;
        }
        spec.method = "todo.create";
        spec.params_json = "{\"title\":\"" + json_escape(title) +
                           "\",\"repo\":\"" + json_escape(arg_str(args, "repo")) +
                           "\",\"preferred_agent\":\"" + json_escape(arg_str(args, "preferred_agent")) +
                           "\",\"priority\":\"" + json_escape(arg_str(args, "priority", "normal")) +
                           "\",\"notes\":\"" + json_escape(arg_str(args, "notes")) + "\"}";
        return spec;
    }

    if (tool_name == "todo_claim") {
        std::string tid = arg_str(args, "id");
        if (tid.empty()) {
            spec.valid = false;
            spec.validation_err = "todo_claim 缺少必填参数 id";
            return spec;
        }
        spec.method = "todo.claim";
        spec.params_json = "{\"id\":\"" + json_escape(tid) +
                           "\",\"session_id\":\"" + json_escape(arg_str(args, "session_id")) +
                           "\",\"agent\":\"" + json_escape(arg_str(args, "agent")) + "\"}";
        return spec;
    }

    if (tool_name == "todo_complete") {
        std::string tid = arg_str(args, "id");
        if (tid.empty()) {
            spec.valid = false;
            spec.validation_err = "todo_complete 缺少必填参数 id";
            return spec;
        }
        spec.method = "todo.complete";
        spec.params_json = "{\"id\":\"" + json_escape(tid) +
                           "\",\"summary\":\"" + json_escape(arg_str(args, "summary")) +
                           "\",\"review\":" + (json_scalar_bool(arg_str(args, "review", "true"), true) ? "true}" : "false}");
        return spec;
    }

    if (tool_name == "desktop_get_context") {
        spec.method = "context.desktop";
        return spec;
    }

    if (tool_name == "clipboard_recent") {
        spec.method = "context.clipboard";
        long long lim = std::clamp(json_scalar_i64(arg_str(args, "limit", "5"), 5), 1LL, 20LL);
        spec.params_json = "{\"limit\":" + std::to_string(lim) + "}";
        return spec;
    }

    if (tool_name == "screenshot_capture") {
        spec.method = "context.screenshot";
        spec.timeout_ms = 15000;
        std::string mode = (arg_str(args, "mode") == "area") ? "area" : "full";
        spec.params_json = "{\"mode\":\"" + mode + "\"}";
        return spec;
    }

    if (tool_name == "doctor_diagnose") {
        spec.method = "context.doctor";
        spec.timeout_ms = 20000;
        return spec;
    }

    if (tool_name == "logs_tail") {
        spec.method = "context.logs";
        long long lines = std::clamp(json_scalar_i64(arg_str(args, "lines", "80"), 80), 10LL, 400LL);
        spec.params_json = "{\"source\":\"" + json_escape(arg_str(args, "source", "apps")) +
                           "\",\"lines\":" + std::to_string(lines) + "}";
        return spec;
    }

    if (tool_name == "snapshot_checkpoint") {
        std::string desc = arg_str(args, "description");
        if (desc.empty()) {
            spec.valid = false;
            spec.validation_err = "snapshot_checkpoint 缺少必填参数 description";
            return spec;
        }
        spec.method = "action.snapshot";
        spec.timeout_ms = 30000;
        spec.params_json = "{\"description\":\"" + json_escape(desc) + "\"}";
        return spec;
    }

    if (tool_name == "sandbox_launch") {
        std::string cmd = arg_str(args, "cmd");
        if (cmd.empty()) {
            spec.valid = false;
            spec.validation_err = "sandbox_launch 缺少必填参数 cmd";
            return spec;
        }
        spec.method = "action.sandbox_launch";
        spec.timeout_ms = 15000;
        spec.params_json = "{\"cmd\":\"" + json_escape(cmd) + "\"}";
        return spec;
    }

    if (tool_name == "sandbox_status") {
        spec.method = "action.sandbox_status";
        return spec;
    }

    if (tool_name == "workspace_focus") {
        if (!has_nonempty("workspace") && !has_nonempty("win_id")) {
            spec.valid = false;
            spec.validation_err = "workspace_focus 需要至少提供 workspace 或 win_id";
            return spec;
        }
        long long tms = std::clamp(json_scalar_i64(arg_str(args, "timeout_ms", "60000"), 60000), 500LL, 300000LL);
        spec.method = "action.workspace_focus";
        spec.interactive = true;
        spec.timeout_ms = static_cast<int>(tms + 5000);
        spec.params_json = "{\"workspace\":" + std::to_string(json_scalar_i64(arg_str(args, "workspace", "0"), 0)) +
                           ",\"win_id\":" + std::to_string(std::max(0LL, json_scalar_i64(arg_str(args, "win_id", "0"), 0))) +
                           ",\"summary\":\"" + json_escape(arg_str(args, "summary", "切换桌面焦点")) +
                           "\",\"timeout_ms\":" + std::to_string(tms) + "}";
        return spec;
    }

    if (tool_name == "service_control") {
        std::string svc = arg_str(args, "service");
        std::string act = arg_str(args, "action");
        if (svc.empty() || act.empty()) {
            spec.valid = false;
            spec.validation_err = "service_control 缺少必填参数 service 或 action";
            return spec;
        }
        if (act == "status") {
            spec.method = "action.service_status";
            spec.params_json = "{\"service\":\"" + json_escape(svc) + "\"}";
        } else {
            long long tms = std::clamp(json_scalar_i64(arg_str(args, "timeout_ms", "60000"), 60000), 500LL, 300000LL);
            spec.method = "action.service_control";
            spec.interactive = true;
            spec.timeout_ms = static_cast<int>(tms + 5000);
            spec.params_json = "{\"service\":\"" + json_escape(svc) +
                               "\",\"action\":\"" + json_escape(act) +
                               "\",\"summary\":\"" + json_escape(arg_str(args, "summary", svc + " -> " + act)) +
                               "\",\"timeout_ms\":" + std::to_string(tms) + "}";
        }
        return spec;
    }

    if (tool_name == "proxy_status") {
        spec.method = "action.proxy_status";
        return spec;
    }

    if (tool_name == "request_user_approval") {
        std::string sum = arg_str(args, "summary");
        if (sum.empty()) {
            spec.valid = false;
            spec.validation_err = "request_user_approval 缺少必填参数 summary";
            return spec;
        }
        long long tms = std::clamp(json_scalar_i64(arg_str(args, "timeout_ms", "60000"), 60000), 500LL, 300000LL);
        bool is_async = json_scalar_bool(arg_str(args, "async", "false"), false);
        spec.method = "approval.request";
        spec.interactive = !is_async;
        spec.timeout_ms = is_async ? 5000 : static_cast<int>(tms + 5000);
        spec.params_json = "{\"summary\":\"" + json_escape(sum) +
                           "\",\"detail\":\"" + json_escape(arg_str(args, "detail")) +
                           "\",\"capability\":\"" + json_escape(arg_str(args, "capability", "approval.request")) +
                           "\",\"async\":" + (is_async ? "true" : "false") +
                           ",\"timeout_ms\":" + std::to_string(tms) + "}";
        return spec;
    }

    spec.valid = false;
    return spec;
}

std::string build_tools_list_json() {
    std::ostringstream oss;
    oss << "{\"tools\":[";
    const size_t n = sizeof(kMcpTools) / sizeof(kMcpTools[0]);
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) oss << ",";
        oss << "{\"name\":\"" << json_escape(kMcpTools[i].name) << "\","
            << "\"description\":\"" << json_escape(kMcpTools[i].description) << "\","
            << "\"inputSchema\":" << kMcpTools[i].schema_json << "}";
    }
    oss << "]}";
    return oss.str();
}

}  // namespace

std::string base64_encode_bytes(const unsigned char *data, size_t len) {
    static constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!data || len == 0) return "";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int v = static_cast<unsigned int>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<unsigned int>(data[i + 1]) << 8;
        if (i + 2 < len) v |= static_cast<unsigned int>(data[i + 2]);
        out.push_back(kB64[(v >> 18) & 0x3F]);
        out.push_back(kB64[(v >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kB64[(v >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kB64[v & 0x3F] : '=');
    }
    return out;
}

std::string dispatch_mcp_jsonrpc(const std::string &req_json, McpBusCallFn bus_call) {
    bool has_id = false;
    std::string id_raw;
    std::string method;
    std::string params_raw = "{}";

    bool parsed = json_parse_object_str(req_json, [&](const std::string &k, const std::string &v, const std::string &raw) {
        if (k == "id") {
            has_id = true;
            id_raw = raw;
        } else if (k == "method") {
            method = trim_copy(v);
        } else if (k == "params" && !raw.empty() && raw.front() == '{') {
            params_raw = raw;
        }
    });
    if (!parsed || method.empty()) {
        return jsonrpc_err(has_id ? id_raw : "null", -32600, "Invalid JSON-RPC 2.0 request");
    }

    // Notifications (no id or notifications/*) produce no response per JSON-RPC / MCP spec.
    if (!has_id || method.rfind("notifications/", 0) == 0) {
        return "";
    }

    if (method == "initialize") {
        std::map<std::string, std::string> p;
        json_parse_flat_object(params_raw, p);
        std::string proto = arg_str(p, "protocolVersion", "2024-11-05");
        if (proto != "2024-11-05" && proto != "2025-03-26") {
            proto = "2024-11-05";
        }
        std::ostringstream res;
        res << "{"
            << "\"protocolVersion\":\"" << json_escape(proto) << "\","
            << "\"capabilities\":{\"tools\":{\"listChanged\":false}},"
            << "\"serverInfo\":{\"name\":\"spinel-os-mcp\",\"version\":\"1.0.0\"},"
            << "\"instructions\":\""
            << json_escape("SpinelOS native OS MCP gateway: use agent_report/todo_* to sync tasks with Dynamic Island, "
                           "desktop_get_context/screenshot_capture/doctor_diagnose/logs_tail for OS awareness, "
                           "and sandbox_launch/snapshot_checkpoint for guarded execution & Btrfs safety checkpoints.")
            << "\""
            << "}";
        return jsonrpc_ok(id_raw, res.str());
    }

    if (method == "ping") {
        return jsonrpc_ok(id_raw, "{}");
    }

    if (method == "tools/list") {
        return jsonrpc_ok(id_raw, build_tools_list_json());
    }

    if (method == "tools/call") {
        std::string tool_name;
        std::string args_raw = "{}";
        json_parse_object_str(params_raw, [&](const std::string &k, const std::string &v, const std::string &raw) {
            if (k == "name") tool_name = trim_copy(v);
            else if (k == "arguments" && !raw.empty() && raw.front() == '{') args_raw = raw;
        });
        if (tool_name.empty()) {
            return jsonrpc_err(id_raw, -32602, "Missing tool name");
        }

        std::map<std::string, std::string> args;
        json_parse_flat_object(args_raw, args);
        BusCallSpec spec = map_tool_to_bus(tool_name, args);
        if (!spec.valid) {
            if (!spec.validation_err.empty()) {
                return jsonrpc_ok(id_raw, mcp_text_result(spec.validation_err, true));
            }
            return jsonrpc_err(id_raw, -32602, "Unknown tool: " + tool_name);
        }

        std::string client_label = mcp_client_label();
        std::ostringstream bus_req;
        bus_req << "{\"id\":\"mcp\""
                << ",\"client\":\"" << json_escape(client_label) << "\""
                << ",\"interactive\":" << (spec.interactive ? "true" : "false")
                << ",\"method\":\"" << json_escape(spec.method) << "\""
                << ",\"params\":" << spec.params_json << "}";

        std::string bus_resp;
        bool connected = false;
        if (bus_call) {
            connected = bus_call(bus_req.str(), bus_resp, spec.timeout_ms);
        } else {
            connected = call_agent_bus_socket(agent_bus_socket_path(), bus_req.str(), bus_resp, spec.timeout_ms);
            if (!connected && is_offline_capable_method(spec.method)) {
                std::map<std::string, std::string> pmap;
                json_parse_flat_object(spec.params_json, pmap);
                bus_resp = dispatch_offline_local(client_label, spec.method, pmap);
                connected = !bus_resp.empty();
            }
        }

        if (!connected || bus_resp.empty()) {
            return jsonrpc_ok(
                id_raw,
                mcp_text_result("[SpinelOS agent_bus offline] 无法连接宿主机总线 " + agent_bus_socket_path() +
                                    "（请在图形会话中运行 spinel-shell）。",
                                true));
        }

        bool bus_ok = false;
        std::string decision;
        std::string result_raw;
        std::string err_msg;
        std::string approval_id;
        json_parse_object_str(bus_resp, [&](const std::string &k, const std::string &v, const std::string &raw) {
            if (k == "ok") bus_ok = json_scalar_bool(v, false);
            else if (k == "decision") decision = v;
            else if (k == "result") result_raw = raw;
            else if (k == "error") err_msg = v;
            else if (k == "approval_id") approval_id = v;
        });

        if (!bus_ok) {
            std::ostringstream msg;
            msg << "[" << (decision.empty() ? "error" : decision) << "] "
                << (err_msg.empty() ? "SpinelOS capability rejected or failed" : err_msg);
            if (!approval_id.empty()) {
                msg << " (approval_id=" << approval_id << ")";
            }
            return jsonrpc_ok(id_raw, mcp_text_result(msg.str(), true));
        }

        std::string text_payload = result_raw.empty() ? "{\"ok\":true}" : result_raw;
        if (tool_name == "screenshot_capture" &&
            json_scalar_bool(arg_str(args, "include_image", "true"), true)) {
            std::map<std::string, std::string> res_map;
            json_parse_flat_object(text_payload, res_map);
            std::string cap_path = arg_str(res_map, "path");
            std::vector<unsigned char> png_bytes;
            if (!cap_path.empty() && read_file_bytes(cap_path, png_bytes)) {
                std::string b64 = base64_encode_bytes(png_bytes.data(), png_bytes.size());
                std::ostringstream multi;
                multi << "{\"content\":["
                      << "{\"type\":\"text\",\"text\":\"" << json_escape(text_payload) << "\"},"
                      << "{\"type\":\"image\",\"data\":\"" << b64 << "\",\"mimeType\":\"image/png\"}"
                      << "],\"isError\":false}";
                return jsonrpc_ok(id_raw, multi.str());
            }
        }

        return jsonrpc_ok(id_raw, mcp_text_result(text_payload, false));
    }

    return jsonrpc_err(id_raw, -32601, "Method not found: " + method);
}

int run_mcp_stdio_loop(std::istream &in, std::ostream &out, McpBusCallFn bus_call) {
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = trim_copy(line);
        if (trimmed.empty()) continue;

        // Support LSP/MCP Content-Length framed messages when client sends HTTP-style headers.
        std::string low_head = trimmed;
        for (char &c : low_head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low_head.rfind("content-length:", 0) == 0) {
            size_t content_len = static_cast<size_t>(
                std::strtoull(trim_copy(trimmed.substr(15)).c_str(), nullptr, 10));
            std::string hdr;
            while (std::getline(in, hdr)) {
                if (!hdr.empty() && hdr.back() == '\r') hdr.pop_back();
                if (trim_copy(hdr).empty()) break;
            }
            if (content_len == 0 || content_len > 8 * 1024 * 1024) continue;
            std::string body(content_len, '\0');
            in.read(body.data(), static_cast<std::streamsize>(content_len));
            if (in.gcount() != static_cast<std::streamsize>(content_len)) break;
            std::string resp = dispatch_mcp_jsonrpc(body, bus_call);
            if (!resp.empty()) {
                out << "Content-Length: " << resp.size() << "\r\n\r\n" << resp;
                out.flush();
            }
            continue;
        }

        std::string resp = dispatch_mcp_jsonrpc(trimmed, bus_call);
        if (!resp.empty()) {
            out << resp << "\n";
            out.flush();
        }
    }
    return 0;
}

}  // namespace spinel::ai
