#pragma once

#include <string>
#include <vector>

namespace spinel::ai {

enum class AgentCapabilityTier {
    Tier0Observe = 0,
    Tier1Context = 1,
    Tier2SandboxSafe = 2,
    Tier3HostControl = 3
};

enum class PermDecision {
    Allow,
    RequireApproval,
    Deny
};

enum class Tier3Policy {
    Ask,
    Allow,
    Deny
};

struct AgentPermConfig {
    bool tier0_observe = true;
    bool tier1_clipboard = false;
    bool tier1_screenshot = false;
    bool tier1_logs = false;
    bool tier2_sandbox = true;
    bool tier2_snapshot = true;
    bool vram_yield_on_gamemode = true;
    Tier3Policy tier3_policy = Tier3Policy::Ask;
};

struct CapabilityMeta {
    const char *capability;
    AgentCapabilityTier tier;
    const char *flag_name;
    const char *label_zh;
};

struct AuditEntry {
    long long ts = 0;
    std::string client_id;
    std::string capability;
    std::string decision;
    std::string detail;
};

/// Resolve the config file path (~/.config/spinelos/agent_perms.conf or $SPINEL_AGENT_PERMS_FILE).
std::string agent_perms_conf_path();

/// Resolve the MCP/Agent audit log path (spinel_state_dir()/mcp_audit.log).
std::string agent_audit_log_path();

/// Load Tier 0-3 authorization configuration (defaults applied if absent).
AgentPermConfig load_agent_perms();

/// Persist Tier 0-3 authorization configuration atomically.
bool save_agent_perms(const AgentPermConfig &cfg);

/// Apply partial `key=val` lines to the saved permission config.
bool update_agent_perms_kv(const std::string &payload, AgentPermConfig &out_cfg);

/// Look up capability metadata. Returns false for unknown capabilities (fail-closed).
bool classify_agent_capability(const std::string &capability, CapabilityMeta &out);

/// Evaluate permission policy for `capability` under current config.
PermDecision check_agent_capability(const std::string &capability, std::string &reason_out);

/// Serialize current permission config as a JSON object.
std::string serialize_agent_perms_json(const AgentPermConfig &cfg);

/// Append a structured audit line to mcp_audit.log.
void append_agent_audit(const std::string &client_id,
                        const std::string &capability,
                        PermDecision decision,
                        const std::string &detail = "");

/// Read up to `max_entries` newest entries from mcp_audit.log.
std::vector<AuditEntry> tail_agent_audit(size_t max_entries = 50);

/// Serialize audit entries as a JSON array.
std::string serialize_audit_entries_json(const std::vector<AuditEntry> &entries);

const char *perm_decision_str(PermDecision d);
const char *tier3_policy_str(Tier3Policy p);
Tier3Policy parse_tier3_policy(const std::string &s);

}  // namespace spinel::ai
