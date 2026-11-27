#include "ai/agent_perms.hpp"
#include "ai/json_util.hpp"
#include "common.hpp"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;

namespace spinel::ai {

namespace {

std::mutex g_perms_mu;

constexpr CapabilityMeta kCapabilityTable[] = {
    // Tier 0: Observe / Agent self-reporting / Task Inbox
    {"ping", AgentCapabilityTier::Tier0Observe, "tier0_observe", "总线连通性探测"},
    {"agent.report", AgentCapabilityTier::Tier0Observe, "tier0_observe", "上报 Agent 会话状态"},
    {"agent.list", AgentCapabilityTier::Tier0Observe, "tier0_observe", "查询活动 Agent 会话列表"},
    {"agent.clear", AgentCapabilityTier::Tier0Observe, "tier0_observe", "清理已结束会话"},
    {"todo.list", AgentCapabilityTier::Tier0Observe, "tier0_observe", "读取任务待办列表"},
    {"todo.create", AgentCapabilityTier::Tier0Observe, "tier0_observe", "新建任务待办"},
    {"todo.claim", AgentCapabilityTier::Tier0Observe, "tier0_observe", "认领并绑定待办任务"},
    {"todo.complete", AgentCapabilityTier::Tier0Observe, "tier0_observe", "标记待办任务待验收/完成"},
    {"todo.accept", AgentCapabilityTier::Tier0Observe, "tier0_observe", "验收通过待办任务"},
    {"todo.reopen", AgentCapabilityTier::Tier0Observe, "tier0_observe", "重新打开待办任务"},
    {"todo.delete", AgentCapabilityTier::Tier0Observe, "tier0_observe", "删除待办任务"},
    {"context.desktop", AgentCapabilityTier::Tier0Observe, "tier0_observe", "读取当前聚焦应用与工作区摘要"},
    {"perms.get", AgentCapabilityTier::Tier0Observe, "tier0_observe", "读取 AI Agent 权限配置"},
    {"audit.tail", AgentCapabilityTier::Tier0Observe, "tier0_observe", "查看近期 Agent 能力审计日志"},
    {"action.proxy_status", AgentCapabilityTier::Tier0Observe, "tier0_observe", "查询系统网络代理状态"},
    {"action.sandbox_status", AgentCapabilityTier::Tier0Observe, "tier0_observe", "查询平行沙箱运行状态"},
    {"action.service_status", AgentCapabilityTier::Tier0Observe, "tier0_observe", "查询本地 AI/容器/打印服务状态"},
    {"approval.list", AgentCapabilityTier::Tier0Observe, "tier0_observe", "查询待审批请求列表"},

    // Tier 1: Sensitive desktop context
    {"context.clipboard", AgentCapabilityTier::Tier1Context, "tier1_clipboard", "读取剪贴板近期历史"},
    {"context.notifications", AgentCapabilityTier::Tier1Context, "tier1_logs", "读取近期系统通知记录"},
    {"context.screenshot", AgentCapabilityTier::Tier1Context, "tier1_screenshot", "截取当前窗口或桌面画面"},
    {"context.doctor", AgentCapabilityTier::Tier1Context, "tier1_logs", "读取 Spinel Doctor 系统健康诊断"},
    {"context.logs", AgentCapabilityTier::Tier1Context, "tier1_logs", "读取脱敏系统/应用日志尾部"},

    // Tier 2: Guarded sandbox & CoW checkpoint protection
    {"action.sandbox_launch", AgentCapabilityTier::Tier2SandboxSafe, "tier2_sandbox", "在平行沙箱中运行应用或命令"},
    {"action.snapshot", AgentCapabilityTier::Tier2SandboxSafe, "tier2_snapshot", "创建 Btrfs 系统还原点快照"},

    // Tier 3: Host-modifying or interactive approval actions
    {"perms.set", AgentCapabilityTier::Tier3HostControl, "tier3_policy", "修改 AI Agent 权限分级配置"},
    {"action.service_control", AgentCapabilityTier::Tier3HostControl, "tier3_policy", "启停宿主机系统/AI 服务"},
    {"action.workspace_focus", AgentCapabilityTier::Tier3HostControl, "tier3_policy", "切换宿主机工作区或激活窗口"},
    {"approval.request", AgentCapabilityTier::Tier3HostControl, "tier3_policy", "发起人工交互审批请求"},
    {"approval.resolve", AgentCapabilityTier::Tier0Observe, "tier0_observe", "裁决挂起的审批请求"},
};

bool parse_bool_str(const std::string &v, bool fallback) {
    if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
    if (v == "false" || v == "0" || v == "no" || v == "off") return false;
    return fallback;
}

}  // namespace

const char *perm_decision_str(PermDecision d) {
    switch (d) {
        case PermDecision::Allow: return "allow";
        case PermDecision::RequireApproval: return "require_approval";
        case PermDecision::Deny: return "deny";
    }
    return "deny";
}

const char *tier3_policy_str(Tier3Policy p) {
    switch (p) {
        case Tier3Policy::Ask: return "ask";
        case Tier3Policy::Allow: return "allow";
        case Tier3Policy::Deny: return "deny";
    }
    return "ask";
}

Tier3Policy parse_tier3_policy(const std::string &s) {
    std::string v = trim_copy(s);
    if (v == "allow") return Tier3Policy::Allow;
    if (v == "deny") return Tier3Policy::Deny;
    return Tier3Policy::Ask;
}

std::string agent_perms_conf_path() {
    if (const char *ov = std::getenv("SPINEL_AGENT_PERMS_FILE"); ov && *ov) {
        return ov;
    }
    return home_dir() + "/.config/spinelos/agent_perms.conf";
}

std::string agent_audit_log_path() {
    return (fs::path(spinel_state_dir()) / "mcp_audit.log").string();
}

AgentPermConfig load_agent_perms() {
    std::lock_guard<std::mutex> lock(g_perms_mu);
    AgentPermConfig cfg;
    std::ifstream ifs(agent_perms_conf_path());
    if (!ifs.is_open()) return cfg;

    std::string line;
    while (std::getline(ifs, line)) {
        std::string t = trim_copy(line);
        if (t.empty() || t[0] == '#') continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(t.substr(0, eq));
        std::string v = trim_copy(t.substr(eq + 1));
        if (k == "tier0_observe") cfg.tier0_observe = parse_bool_str(v, cfg.tier0_observe);
        else if (k == "tier1_clipboard") cfg.tier1_clipboard = parse_bool_str(v, cfg.tier1_clipboard);
        else if (k == "tier1_screenshot") cfg.tier1_screenshot = parse_bool_str(v, cfg.tier1_screenshot);
        else if (k == "tier1_logs") cfg.tier1_logs = parse_bool_str(v, cfg.tier1_logs);
        else if (k == "tier2_sandbox") cfg.tier2_sandbox = parse_bool_str(v, cfg.tier2_sandbox);
        else if (k == "tier2_snapshot") cfg.tier2_snapshot = parse_bool_str(v, cfg.tier2_snapshot);
        else if (k == "tier3_policy") cfg.tier3_policy = parse_tier3_policy(v);
    }
    return cfg;
}

bool save_agent_perms(const AgentPermConfig &cfg) {
    std::lock_guard<std::mutex> lock(g_perms_mu);
    std::ostringstream oss;
    oss << "# SpinelOS AI Agent Capability Permissions (Tier 0-3)\n"
        << "tier0_observe=" << (cfg.tier0_observe ? "true" : "false") << "\n"
        << "tier1_clipboard=" << (cfg.tier1_clipboard ? "true" : "false") << "\n"
        << "tier1_screenshot=" << (cfg.tier1_screenshot ? "true" : "false") << "\n"
        << "tier1_logs=" << (cfg.tier1_logs ? "true" : "false") << "\n"
        << "tier2_sandbox=" << (cfg.tier2_sandbox ? "true" : "false") << "\n"
        << "tier2_snapshot=" << (cfg.tier2_snapshot ? "true" : "false") << "\n"
        << "tier3_policy=" << tier3_policy_str(cfg.tier3_policy) << "\n";
    return write_text_file(agent_perms_conf_path(), oss.str());
}

bool update_agent_perms_kv(const std::string &payload, AgentPermConfig &out_cfg) {
    AgentPermConfig next = load_agent_perms();
    for (const auto &raw_line : split_lines(payload)) {
        std::string t = trim_copy(raw_line);
        if (t.empty() || t[0] == '#') continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(t.substr(0, eq));
        std::string v = trim_copy(t.substr(eq + 1));
        if (k == "tier0_observe") next.tier0_observe = parse_bool_str(v, next.tier0_observe);
        else if (k == "tier1_clipboard") next.tier1_clipboard = parse_bool_str(v, next.tier1_clipboard);
        else if (k == "tier1_screenshot") next.tier1_screenshot = parse_bool_str(v, next.tier1_screenshot);
        else if (k == "tier1_logs") next.tier1_logs = parse_bool_str(v, next.tier1_logs);
        else if (k == "tier2_sandbox") next.tier2_sandbox = parse_bool_str(v, next.tier2_sandbox);
        else if (k == "tier2_snapshot") next.tier2_snapshot = parse_bool_str(v, next.tier2_snapshot);
        else if (k == "tier3_policy") next.tier3_policy = parse_tier3_policy(v);
    }
    if (!save_agent_perms(next)) return false;
    out_cfg = next;
    return true;
}

bool classify_agent_capability(const std::string &capability, CapabilityMeta &out) {
    for (const auto &entry : kCapabilityTable) {
        if (capability == entry.capability) {
            out = entry;
            return true;
        }
    }
    return false;
}

PermDecision check_agent_capability(const std::string &capability, std::string &reason_out) {
    reason_out.clear();
    CapabilityMeta meta{};
    if (!classify_agent_capability(capability, meta)) {
        reason_out = "未知能力，默认拒绝: " + capability;
        return PermDecision::Deny;
    }

    AgentPermConfig cfg = load_agent_perms();
    switch (meta.tier) {
        case AgentCapabilityTier::Tier0Observe:
            if (cfg.tier0_observe) return PermDecision::Allow;
            reason_out = "Tier 0 基础观测能力已被用户在「权限与安全」关闭。";
            return PermDecision::Deny;
        case AgentCapabilityTier::Tier1Context: {
            bool allowed = false;
            if (std::string(meta.flag_name) == "tier1_clipboard") allowed = cfg.tier1_clipboard;
            else if (std::string(meta.flag_name) == "tier1_screenshot") allowed = cfg.tier1_screenshot;
            else if (std::string(meta.flag_name) == "tier1_logs") allowed = cfg.tier1_logs;
            if (allowed) return PermDecision::Allow;
            reason_out = std::string("Tier 1 权限未开启（需要在「系统设置 → 权限与安全」启用 ") + meta.flag_name + "）。";
            return PermDecision::Deny;
        }
        case AgentCapabilityTier::Tier2SandboxSafe: {
            bool allowed = (std::string(meta.flag_name) == "tier2_sandbox") ? cfg.tier2_sandbox : cfg.tier2_snapshot;
            if (allowed) return PermDecision::Allow;
            reason_out = std::string("Tier 2 安全执行权限已被关闭: ") + meta.flag_name;
            return PermDecision::Deny;
        }
        case AgentCapabilityTier::Tier3HostControl:
            if (cfg.tier3_policy == Tier3Policy::Allow) return PermDecision::Allow;
            if (cfg.tier3_policy == Tier3Policy::Ask) {
                reason_out = std::string("Tier 3 宿主机控制需要用户审批: ") + meta.label_zh;
                return PermDecision::RequireApproval;
            }
            reason_out = "Tier 3 宿主机控制策略当前设为拒绝 (deny)。";
            return PermDecision::Deny;
    }
    return PermDecision::Deny;
}

std::string serialize_agent_perms_json(const AgentPermConfig &cfg) {
    std::ostringstream oss;
    oss << "{"
        << "\"tier0_observe\":" << (cfg.tier0_observe ? "true" : "false") << ","
        << "\"tier1_clipboard\":" << (cfg.tier1_clipboard ? "true" : "false") << ","
        << "\"tier1_screenshot\":" << (cfg.tier1_screenshot ? "true" : "false") << ","
        << "\"tier1_logs\":" << (cfg.tier1_logs ? "true" : "false") << ","
        << "\"tier2_sandbox\":" << (cfg.tier2_sandbox ? "true" : "false") << ","
        << "\"tier2_snapshot\":" << (cfg.tier2_snapshot ? "true" : "false") << ","
        << "\"tier3_policy\":\"" << tier3_policy_str(cfg.tier3_policy) << "\""
        << "}";
    return oss.str();
}

void append_agent_audit(const std::string &client_id,
                        const std::string &capability,
                        PermDecision decision,
                        const std::string &detail) {
    std::lock_guard<std::mutex> lock(g_perms_mu);
    fs::path p = agent_audit_log_path();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    auto sz = fs::file_size(p, ec);
    if (!ec && sz > 1024 * 1024) {
        fs::resize_file(p, 0, ec);
    }

    long long ts = static_cast<long long>(std::time(nullptr));
    std::ostringstream line;
    line << "{"
         << "\"ts\":" << ts << ","
         << "\"client\":\"" << json_escape(client_id.empty() ? "local" : client_id) << "\","
         << "\"capability\":\"" << json_escape(capability) << "\","
         << "\"decision\":\"" << perm_decision_str(decision) << "\","
         << "\"detail\":\"" << json_escape(detail) << "\""
         << "}\n";

    std::ofstream ofs(p, std::ios::app);
    if (ofs.is_open()) {
        ofs << line.str();
    }
}

std::vector<AuditEntry> tail_agent_audit(size_t max_entries) {
    std::lock_guard<std::mutex> lock(g_perms_mu);
    std::vector<AuditEntry> all;
    std::ifstream ifs(agent_audit_log_path());
    if (!ifs.is_open()) return all;

    std::string line;
    while (std::getline(ifs, line)) {
        std::string t = trim_copy(line);
        if (t.empty() || t.front() != '{') continue;
        AuditEntry e;
        bool parsed = json_parse_object_str(t, [&](const std::string &k, const std::string &v, const std::string &) {
            if (k == "ts") e.ts = json_scalar_i64(v, 0);
            else if (k == "client") e.client_id = v;
            else if (k == "capability") e.capability = v;
            else if (k == "decision") e.decision = v;
            else if (k == "detail") e.detail = v;
        });
        if (parsed && !e.capability.empty()) {
            all.push_back(std::move(e));
        }
    }
    if (all.size() > max_entries) {
        all.erase(all.begin(), all.end() - static_cast<std::ptrdiff_t>(max_entries));
    }
    return all;
}

std::string serialize_audit_entries_json(const std::vector<AuditEntry> &entries) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) oss << ",";
        const auto &e = entries[i];
        oss << "{"
            << "\"ts\":" << e.ts << ","
            << "\"client\":\"" << json_escape(e.client_id) << "\","
            << "\"capability\":\"" << json_escape(e.capability) << "\","
            << "\"decision\":\"" << json_escape(e.decision) << "\","
            << "\"detail\":\"" << json_escape(e.detail) << "\""
            << "}";
    }
    oss << "]";
    return oss.str();
}

}  // namespace spinel::ai
