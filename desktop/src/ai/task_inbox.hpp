#pragma once

#include "ai/agent_tracker.hpp"

#include <string>
#include <vector>

namespace spinel::ai {

enum class TaskStatus {
    Backlog,
    InProgress,
    Review,
    Done
};

struct AgentTaskItem {
    std::string id;
    std::string title;
    std::string notes;
    std::string repo_path;
    std::string priority = "normal";  // "low" | "normal" | "high"
    TaskStatus status = TaskStatus::Backlog;
    std::string preferred_agent;
    std::string assigned_session_id;
    std::string result_summary;
    long long created_at = 0;
    long long updated_at = 0;
};

const char *task_status_str(TaskStatus s);
TaskStatus parse_task_status(const std::string &s);

/// Task storage path ($SPINEL_TASK_INBOX_FILE or ~/.local/share/spinelos/tasks.json).
std::string task_inbox_path();

/// List tasks matching optional status ("backlog"|"in_progress"|"review"|"done") and repo substring/path.
std::vector<AgentTaskItem> list_tasks(const std::string &status_filter = "",
                                      const std::string &repo_filter = "");

/// Create a new Backlog task and persist atomically.
bool create_task(const std::string &title,
                 const std::string &repo_path,
                 const std::string &preferred_agent,
                 const std::string &priority,
                 const std::string &notes,
                 AgentTaskItem &out_task);

/// Transition task to InProgress bound to `session_id`.
bool claim_task(const std::string &task_id,
                const std::string &session_id,
                const std::string &agent_name,
                AgentTaskItem &out_task);

/// Transition task to Review (or Done when `to_review=false`) with optional summary.
bool complete_task(const std::string &task_id,
                   const std::string &summary,
                   bool to_review,
                   AgentTaskItem &out_task);

/// Accept a Review/InProgress task into Done.
bool accept_task(const std::string &task_id, AgentTaskItem &out_task);

/// Reopen any task back to Backlog and clear assigned session.
bool reopen_task(const std::string &task_id, AgentTaskItem &out_task);

/// Delete a task permanently.
bool delete_task(const std::string &task_id);

/// Synchronize Task Inbox state machine when a bound Agent session changes status.
bool sync_task_with_agent_transition(const AgentTransitionEvent &ev);

std::string serialize_task_json(const AgentTaskItem &t);
std::string serialize_tasks_json(const std::vector<AgentTaskItem> &tasks);

}  // namespace spinel::ai
