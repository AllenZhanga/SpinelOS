#include "ai/agent_perms.hpp"
#include "ai/json_util.hpp"
#include "common.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
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
    {"action.vram_yield", AgentCapabilityTier::Tier0Observe, "tier0_observe", "GameMode 自动协同释放推理显存"},
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
        else if (k == "vram_yield_on_gamemode") cfg.vram_yield_on_gamemode = parse_bool_str(v, cfg.vram_yield_on_gamemode);
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
        << "vram_yield_on_gamemode=" << (cfg.vram_yield_on_gamemode ? "true" : "false") << "\n"
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
        else if (k == "vram_yield_on_gamemode") next.vram_yield_on_gamemode = parse_bool_str(v, next.vram_yield_on_gamemode);
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
        << "\"vram_yield_on_gamemode\":" << (cfg.vram_yield_on_gamemode ? "true" : "false") << ","
        << "\"tier3_policy\":\"" << tier3_policy_str(cfg.tier3_policy) << "\""
        << "}";
    return oss.str();
}

bool should_audit_agent_event(const std::string &client_id,
                              const std::string &capability,
                              PermDecision decision) {
    if (decision != PermDecision::Allow) return true;
    if (capability.rfind("approval.", 0) == 0) return true;
    if (capability == "ping" || capability == "perms.get" ||
        capability == "audit.tail" || capability == "approval.list") {
        return false;
    }
    if (client_id == "settings" || client_id == "shell" || client_id == "doctor") {
        return false;
    }
    if ((client_id == "spinel-agent-ctl" || client_id == "local") &&
        (capability == "agent.list" || capability == "todo.list")) {
        return false;
    }
    return true;
}

void append_agent_audit(const std::string &client_id,
                        const std::string &capability,
                        PermDecision decision,
                        const std::string &detail) {
    if (!should_audit_agent_event(client_id, capability, decision)) return;
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

namespace {

struct McpTargetSpec {
    const char *id;
    const char *client_name;
    const char *rel_path;
    bool is_toml;
};

constexpr McpTargetSpec kMcpTargets[] = {
    {"antigravity", "Antigravity", ".gemini/config/mcp_config.json", false},
    {"codex", "Codex", ".codex/config.toml", true},
    {"claude", "Claude", ".claude.json", false},
    {"deepseek", "DeepSeekHarness", ".config/deepseek/mcp_config.json", false},
    {"gemini", "GeminiCLI", ".gemini/settings.json", false},
};

const McpTargetSpec *find_mcp_target(const std::string &target) {
    std::string low;
    for (char c : trim_copy(target)) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto &spec : kMcpTargets) {
        if (low == spec.id) return &spec;
    }
    return nullptr;
}

std::string build_mcp_json_entry(const std::string &client_name) {
    return "{\"command\":\"/usr/local/bin/spinel-os-mcp\",\"args\":[],\"env\":{\"SPINEL_AGENT_CLIENT\":\"" +
           json_escape(client_name) + "\"}}";
}

size_t skip_ws(const std::string &s, size_t i, size_t end) {
    while (i < end && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return i;
}

size_t scan_json_value_end(const std::string &s, size_t start, size_t limit) {
    if (start >= limit) return std::string::npos;
    char ch = s[start];
    if (ch == '"') {
        size_t i = start + 1;
        while (i < limit) {
            if (s[i] == '\\') {
                i += 2;
                continue;
            }
            if (s[i] == '"') return i + 1;
            ++i;
        }
        return std::string::npos;
    }
    if (ch == '{' || ch == '[') {
        int depth = 0;
        bool in_str = false;
        for (size_t i = start; i < limit; ++i) {
            char c = s[i];
            if (in_str) {
                if (c == '\\') {
                    ++i;
                    continue;
                }
                if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') in_str = true;
            else if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') {
                --depth;
                if (depth == 0) return i + 1;
            }
        }
        return std::string::npos;
    }
    size_t i = start;
    while (i < limit) {
        char c = s[i];
        if (c == ',' || c == '}' || c == ']' || std::isspace(static_cast<unsigned char>(c))) break;
        ++i;
    }
    return (i > start) ? i : std::string::npos;
}

struct JsonFieldSpan {
    std::string key;
    size_t val_start = 0;
    size_t val_end = 0;
};

bool collect_json_object_fields(const std::string &s,
                                size_t obj_start,
                                size_t obj_end,
                                std::vector<JsonFieldSpan> &out) {
    out.clear();
    if (obj_start >= obj_end || obj_end > s.size() || s[obj_start] != '{' || s[obj_end - 1] != '}') {
        return false;
    }
    size_t i = skip_ws(s, obj_start + 1, obj_end - 1);
    if (i == obj_end - 1) return true;  // empty object {}
    while (i < obj_end - 1) {
        if (s[i] != '"') return false;
        std::string key;
        if (!json_parse_string(s, i, key)) return false;
        i = skip_ws(s, i, obj_end - 1);
        if (i >= obj_end - 1 || s[i] != ':') return false;
        i = skip_ws(s, i + 1, obj_end - 1);
        size_t v_end = scan_json_value_end(s, i, obj_end - 1);
        if (v_end == std::string::npos) return false;
        out.push_back({key, i, v_end});
        i = skip_ws(s, v_end, obj_end - 1);
        if (i == obj_end - 1) return true;
        if (s[i] != ',') return false;
        i = skip_ws(s, i + 1, obj_end - 1);
        if (i == obj_end - 1) return true;  // tolerate trailing comma if any
    }
    return true;
}

bool merge_json_mcp_server(const std::string &existing,
                           const std::string &server_key,
                           const std::string &entry_json,
                           std::string &out_json,
                           std::string &err_out) {
    std::string t = trim_copy(existing);
    if (t.empty()) {
        out_json = "{\n  \"mcpServers\": {\n    \"" + server_key + "\": " + entry_json + "\n  }\n}\n";
        return true;
    }
    size_t obj_start = existing.find('{');
    size_t obj_end = existing.rfind('}');
    if (obj_start == std::string::npos || obj_end == std::string::npos || obj_end <= obj_start) {
        err_out = "现有配置不是有效的 JSON 对象，无法自动合并";
        return false;
    }
    ++obj_end;
    std::vector<JsonFieldSpan> top_fields;
    if (!collect_json_object_fields(existing, obj_start, obj_end, top_fields)) {
        err_out = "现有 JSON 配置文件格式无效，请检查后重试";
        return false;
    }

    for (const auto &tf : top_fields) {
        if (tf.key != "mcpServers") continue;
        if (tf.val_end <= tf.val_start || existing[tf.val_start] != '{' || existing[tf.val_end - 1] != '}') {
            err_out = "现有配置文件中 mcpServers 字段不是 JSON 对象";
            return false;
        }
        std::vector<JsonFieldSpan> srv_fields;
        if (!collect_json_object_fields(existing, tf.val_start, tf.val_end, srv_fields)) {
            err_out = "现有 mcpServers 对象格式无效";
            return false;
        }
        for (const auto &sf : srv_fields) {
            if (sf.key == server_key) {
                out_json = existing.substr(0, sf.val_start) + entry_json + existing.substr(sf.val_end);
                return true;
            }
        }
        if (srv_fields.empty()) {
            std::string rep = "{\n    \"" + server_key + "\": " + entry_json + "\n  }";
            out_json = existing.substr(0, tf.val_start) + rep + existing.substr(tf.val_end);
        } else {
            size_t ins = srv_fields.back().val_end;
            out_json = existing.substr(0, ins) + ",\n    \"" + server_key + "\": " + entry_json +
                       existing.substr(ins);
        }
        return true;
    }

    std::string block = "\"mcpServers\": {\n    \"" + server_key + "\": " + entry_json + "\n  }";
    if (top_fields.empty()) {
        out_json = "{\n  " + block + "\n}\n";
    } else {
        size_t ins = top_fields.back().val_end;
        out_json = existing.substr(0, ins) + ",\n  " + block + existing.substr(ins);
    }
    return true;
}

std::string merge_codex_toml_mcp(const std::string &existing) {
    const std::string section_block =
        "[mcp_servers.spinel_os]\n"
        "command = \"/usr/local/bin/spinel-os-mcp\"\n"
        "env = { SPINEL_AGENT_CLIENT = \"Codex\" }\n";

    std::vector<std::string> lines = split_lines(existing);
    std::vector<std::string> out_lines;
    bool replaced = false;
    bool skipping = false;
    for (const auto &ln : lines) {
        std::string t = trim_copy(ln);
        if (!t.empty() && t.front() == '[') {
            if (t == "[mcp_servers.spinel_os]") {
                skipping = true;
                if (!replaced) {
                    out_lines.push_back("[mcp_servers.spinel_os]");
                    out_lines.push_back("command = \"/usr/local/bin/spinel-os-mcp\"");
                    out_lines.push_back("env = { SPINEL_AGENT_CLIENT = \"Codex\" }");
                    replaced = true;
                }
                continue;
            }
            skipping = false;
        }
        if (!skipping) out_lines.push_back(ln);
    }

    std::ostringstream oss;
    for (size_t i = 0; i < out_lines.size(); ++i) {
        oss << out_lines[i] << "\n";
    }
    if (!replaced) {
        if (!out_lines.empty() && !trim_copy(out_lines.back()).empty()) oss << "\n";
        oss << section_block;
    }
    return oss.str();
}

}  // namespace

std::string mcp_client_config_path(const std::string &target) {
    const McpTargetSpec *spec = find_mcp_target(target);
    if (!spec) return "";
    return (fs::path(home_dir()) / spec->rel_path).string();
}

std::string build_mcp_client_snippet(const std::string &target) {
    const McpTargetSpec *spec = find_mcp_target(target);
    if (spec && spec->is_toml) {
        return "[mcp_servers.spinel_os]\n"
               "command = \"/usr/local/bin/spinel-os-mcp\"\n"
               "env = { SPINEL_AGENT_CLIENT = \"Codex\" }\n";
    }
    std::string client = spec ? spec->client_name : "Agent";
    return "{\n"
           "  \"mcpServers\": {\n"
           "    \"spinel-os\": {\n"
           "      \"command\": \"/usr/local/bin/spinel-os-mcp\",\n"
           "      \"args\": [],\n"
           "      \"env\": {\n"
           "        \"SPINEL_AGENT_CLIENT\": \"" + json_escape(client) + "\"\n"
           "      }\n"
           "    }\n"
           "  }\n"
           "}\n";
}

bool is_mcp_client_installed(const std::string &target) {
    const McpTargetSpec *spec = find_mcp_target(target);
    if (!spec) return false;
    std::string path = (fs::path(home_dir()) / spec->rel_path).string();
    std::ifstream ifs(path);
    if (!ifs.is_open()) return false;
    std::string raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (spec->is_toml) {
        return raw.find("[mcp_servers.spinel_os]") != std::string::npos &&
               raw.find("spinel-os-mcp") != std::string::npos;
    }
    std::string servers_raw;
    json_parse_object_str(raw, [&](const std::string &k, const std::string &, const std::string &r) {
        if (k == "mcpServers" && !r.empty() && r.front() == '{') servers_raw = r;
    });
    if (servers_raw.empty()) return false;
    std::string spinel_raw;
    json_parse_object_str(servers_raw, [&](const std::string &k, const std::string &, const std::string &r) {
        if (k == "spinel-os" && !r.empty() && r.front() == '{') spinel_raw = r;
    });
    return !spinel_raw.empty() && spinel_raw.find("spinel-os-mcp") != std::string::npos;
}

bool install_mcp_client_config(const std::string &target, std::string &out_path, std::string &error_out) {
    out_path.clear();
    error_out.clear();
    const McpTargetSpec *spec = find_mcp_target(target);
    if (!spec) {
        error_out = "不支持的客户端类型: " + target;
        return false;
    }
    fs::path p = fs::path(home_dir()) / spec->rel_path;
    out_path = p.string();

    std::string existing;
    {
        std::ifstream ifs(p);
        if (ifs.is_open()) {
            existing.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        }
    }

    std::string next_content;
    if (spec->is_toml) {
        next_content = merge_codex_toml_mcp(existing);
    } else {
        if (!merge_json_mcp_server(existing, "spinel-os", build_mcp_json_entry(spec->client_name),
                                   next_content, error_out)) {
            return false;
        }
    }

    if (!write_text_file(out_path, next_content)) {
        error_out = "写入配置文件失败: " + out_path;
        return false;
    }
    return true;
}

std::string serialize_mcp_clients_status_json() {
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto &spec : kMcpTargets) {
        if (!first) oss << ",";
        first = false;
        std::string path = (fs::path(home_dir()) / spec.rel_path).string();
        oss << "\"" << spec.id << "\":{"
            << "\"installed\":" << (is_mcp_client_installed(spec.id) ? "true" : "false") << ","
            << "\"path\":\"" << json_escape(path) << "\""
            << "}";
    }
    oss << "}";
    return oss.str();
}

}  // namespace spinel::ai
