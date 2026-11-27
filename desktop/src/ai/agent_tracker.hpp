#pragma once

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spinel::ai {

enum class AgentSessionStatus {
    Running,
    WaitingUser,
    Done,
    Error
};

struct AgentSessionInfo {
    std::string id;
    std::string agent;
    std::string title;
    std::string repo_path;
    pid_t pid = 0;
    AgentSessionStatus status = AgentSessionStatus::Running;
    std::string summary;
    uint64_t win_id = 0;
    int workspace_idx = -1;
    bool in_sandbox = false;
    std::string task_id;
    std::string approval_id;
    std::string source = "file";  // "file" | "ctl" | "proc"
    long long updated_at = 0;
};

struct AgentTransitionEvent {
    std::string session_id;
    bool has_prev = false;
    AgentSessionStatus prev_status = AgentSessionStatus::Running;
    AgentSessionStatus new_status = AgentSessionStatus::Running;
    AgentSessionInfo session;
};

struct WindowBindingRef {
    uint64_t id = 0;
    std::string app_id;
    std::string title;
    bool activated = false;
};

using AgentTransitionFn = std::function<void(const AgentTransitionEvent &)>;

const char *agent_status_str(AgentSessionStatus s);
AgentSessionStatus parse_agent_status(const std::string &s);

/// Sanitize a session identifier into a safe filename stem.
std::string sanitize_agent_session_id(const std::string &raw);

/// Directory storing drop-in session files ($SPINEL_AGENT_RUNTIME_DIR or $XDG_RUNTIME_DIR/spinelos/agents).
std::string agent_sessions_runtime_dir();

/// Match a /proc/<pid>/cmdline string against known Coding / AI Agents.
bool match_known_agent_cmdline(const std::string &cmdline, std::string &agent_out);

/// Insert or update an agent session (optionally persisting its drop-in JSON file) and return transition info.
AgentTransitionEvent upsert_agent_session(const AgentSessionInfo &info, bool persist_file = true);

/// Remove a specific session (or all finished Done/Error sessions if id is empty).
bool clear_agent_session(const std::string &session_id);

/// Register a callback fired whenever any session changes status.
void set_agent_transition_callback(AgentTransitionFn cb);

/// Load drop-in JSON files, scan /proc (unless skipped), expire stale sessions, and return active snapshot.
std::vector<AgentSessionInfo> reconcile_agent_sessions(long long now_sec = 0,
                                                     long long finished_ttl_sec = 300,
                                                     bool include_proc_scan = true,
                                                     const std::vector<WindowBindingRef> &windows = {});

/// Bind sessions without an explicit win_id to matching tracked windows using agent/repo heuristics.
void bind_sessions_to_windows(std::vector<AgentSessionInfo> &sessions,
                              const std::vector<WindowBindingRef> &windows);

/// Reset in-memory session tracker state (for deterministic unit tests).
void reset_agent_tracker_for_tests();

std::string serialize_agent_session_json(const AgentSessionInfo &s);
std::string serialize_agent_sessions_json(const std::vector<AgentSessionInfo> &sessions);

}  // namespace spinel::ai
