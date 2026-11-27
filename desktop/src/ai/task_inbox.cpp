#include "ai/task_inbox.hpp"
#include "ai/json_util.hpp"
#include "common.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;

namespace spinel::ai {

namespace {

std::mutex g_inbox_mu;
std::atomic<unsigned long long> g_task_seq{0};

std::string normalize_priority(const std::string &p) {
    std::string t = trim_copy(p);
    if (t == "low" || t == "high") return t;
    return "normal";
}

int task_status_order(TaskStatus s) {
    switch (s) {
        case TaskStatus::Review: return 0;
        case TaskStatus::InProgress: return 1;
        case TaskStatus::Backlog: return 2;
        case TaskStatus::Done: return 3;
    }
    return 4;
}

std::vector<AgentTaskItem> load_tasks_unlocked() {
    std::vector<AgentTaskItem> items;
    std::ifstream ifs(task_inbox_path());
    if (!ifs.is_open()) return items;
    std::string raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    raw = trim_copy(raw);
    if (raw.empty() || raw.front() != '[') return items;

    json_parse_array_str(raw, [&](const std::string &elem) {
        AgentTaskItem t;
        bool ok = json_parse_object_str(elem, [&](const std::string &k, const std::string &v, const std::string &) {
            if (k == "id") t.id = v;
            else if (k == "title") t.title = v;
            else if (k == "notes") t.notes = v;
            else if (k == "repo_path") t.repo_path = v;
            else if (k == "priority") t.priority = normalize_priority(v);
            else if (k == "status") t.status = parse_task_status(v);
            else if (k == "preferred_agent") t.preferred_agent = v;
            else if (k == "assigned_session_id") t.assigned_session_id = v;
            else if (k == "result_summary") t.result_summary = v;
            else if (k == "created_at") t.created_at = json_scalar_i64(v, 0);
            else if (k == "updated_at") t.updated_at = json_scalar_i64(v, 0);
        });
        if (ok && !t.id.empty() && !t.title.empty()) {
            items.push_back(std::move(t));
        }
    });
    return items;
}

bool save_tasks_unlocked(const std::vector<AgentTaskItem> &items) {
    return write_text_file(task_inbox_path(), serialize_tasks_json(items) + "\n");
}

AgentTaskItem *find_task_unlocked(std::vector<AgentTaskItem> &items, const std::string &task_id) {
    for (auto &t : items) {
        if (t.id == task_id) return &t;
    }
    return nullptr;
}

}  // namespace

const char *task_status_str(TaskStatus s) {
    switch (s) {
        case TaskStatus::Backlog: return "backlog";
        case TaskStatus::InProgress: return "in_progress";
        case TaskStatus::Review: return "review";
        case TaskStatus::Done: return "done";
    }
    return "backlog";
}

TaskStatus parse_task_status(const std::string &s) {
    std::string t = trim_copy(s);
    if (t == "in_progress" || t == "running") return TaskStatus::InProgress;
    if (t == "review" || t == "pending_review") return TaskStatus::Review;
    if (t == "done" || t == "completed") return TaskStatus::Done;
    return TaskStatus::Backlog;
}

std::string task_inbox_path() {
    if (const char *ov = std::getenv("SPINEL_TASK_INBOX_FILE"); ov && *ov) {
        return ov;
    }
    return home_dir() + "/.local/share/spinelos/tasks.json";
}

std::vector<AgentTaskItem> list_tasks(const std::string &status_filter,
                                      const std::string &repo_filter) {
    std::lock_guard<std::mutex> lock(g_inbox_mu);
    auto all = load_tasks_unlocked();
    std::string sf = trim_copy(status_filter);
    std::string rf = trim_copy(repo_filter);

    std::vector<AgentTaskItem> out;
    for (auto &t : all) {
        if (!sf.empty() && task_status_str(t.status) != sf) continue;
        if (!rf.empty() && t.repo_path != rf && t.repo_path.find(rf) == std::string::npos) continue;
        out.push_back(std::move(t));
    }
    std::stable_sort(out.begin(), out.end(), [](const AgentTaskItem &a, const AgentTaskItem &b) {
        int oa = task_status_order(a.status);
        int ob = task_status_order(b.status);
        if (oa != ob) return oa < ob;
        return a.updated_at > b.updated_at;
    });
    return out;
}

bool create_task(const std::string &title,
                 const std::string &repo_path,
                 const std::string &preferred_agent,
                 const std::string &priority,
                 const std::string &notes,
                 AgentTaskItem &out_task) {
    std::string clean_title = trim_copy(title);
    if (clean_title.empty()) return false;

    std::lock_guard<std::mutex> lock(g_inbox_mu);
    auto items = load_tasks_unlocked();
    long long now = static_cast<long long>(std::time(nullptr));
    unsigned long long seq = g_task_seq.fetch_add(1) + 1;

    AgentTaskItem item;
    item.id = "t_" + std::to_string(now) + "_" + std::to_string(seq);
    item.title = clean_title;
    item.notes = trim_copy(notes);
    item.repo_path = trim_copy(repo_path);
    item.priority = normalize_priority(priority);
    item.status = TaskStatus::Backlog;
    item.preferred_agent = trim_copy(preferred_agent);
    item.created_at = now;
    item.updated_at = now;

    items.push_back(item);
    if (!save_tasks_unlocked(items)) return false;
    out_task = std::move(item);
    return true;
}

bool claim_task(const std::string &task_id,
                const std::string &session_id,
                const std::string &agent_name,
                AgentTaskItem &out_task) {
    std::lock_guard<std::mutex> lock(g_inbox_mu);
    auto items = load_tasks_unlocked();
    AgentTaskItem *t = find_task_unlocked(items, trim_copy(task_id));
    if (!t) return false;
    t->status = TaskStatus::InProgress;
    if (!trim_copy(session_id).empty()) t->assigned_session_id = sanitize_agent_session_id(session_id);
    if (!trim_copy(agent_name).empty()) t->preferred_agent = trim_copy(agent_name);
    t->updated_at = static_cast<long long>(std::time(nullptr));
    if (!save_tasks_unlocked(items)) return false;
    out_task = *t;
    return true;
}

bool complete_task(const std::string &task_id,
                   const std::string &summary,
                   bool to_review,
                   AgentTaskItem &out_task) {
    std::lock_guard<std::mutex> lock(g_inbox_mu);
    auto items = load_tasks_unlocked();
    AgentTaskItem *t = find_task_unlocked(items, trim_copy(task_id));
    if (!t) return false;
    t->status = to_review ? TaskStatus::Review : TaskStatus::Done;
    if (!trim_copy(summary).empty()) t->result_summary = trim_copy(summary);
    t->updated_at = static_cast<long long>(std::time(nullptr));
    if (!save_tasks_unlocked(items)) return false;
    out_task = *t;
    return true;
}

bool accept_task(const std::string &task_id, AgentTaskItem &out_task) {
    return complete_task(task_id, "", false, out_task);
}

bool reopen_task(const std::string &task_id, AgentTaskItem &out_task) {
    std::lock_guard<std::mutex> lock(g_inbox_mu);
    auto items = load_tasks_unlocked();
    AgentTaskItem *t = find_task_unlocked(items, trim_copy(task_id));
    if (!t) return false;
    t->status = TaskStatus::Backlog;
    t->assigned_session_id.clear();
    t->updated_at = static_cast<long long>(std::time(nullptr));
    if (!save_tasks_unlocked(items)) return false;
    out_task = *t;
    return true;
}

bool delete_task(const std::string &task_id) {
    std::lock_guard<std::mutex> lock(g_inbox_mu);
    auto items = load_tasks_unlocked();
    std::string id = trim_copy(task_id);
    size_t before = items.size();
    items.erase(std::remove_if(items.begin(), items.end(), [&](const AgentTaskItem &t) {
        return t.id == id;
    }), items.end());
    if (items.size() == before) return false;
    return save_tasks_unlocked(items);
}

bool sync_task_with_agent_transition(const AgentTransitionEvent &ev) {
    if (ev.new_status != AgentSessionStatus::Done && ev.new_status != AgentSessionStatus::Error) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_inbox_mu);
    auto items = load_tasks_unlocked();
    bool changed = false;
    long long now = static_cast<long long>(std::time(nullptr));

    for (auto &t : items) {
        bool match = (!ev.session.task_id.empty() && t.id == ev.session.task_id) ||
                     (!t.assigned_session_id.empty() && t.assigned_session_id == ev.session_id);
        if (!match) continue;
        if (ev.new_status == AgentSessionStatus::Done && t.status == TaskStatus::InProgress) {
            t.status = TaskStatus::Review;
            if (!ev.session.summary.empty()) t.result_summary = ev.session.summary;
            t.updated_at = now;
            changed = true;
        } else if (ev.new_status == AgentSessionStatus::Error && t.status == TaskStatus::InProgress) {
            t.result_summary = "[错误] " + (ev.session.summary.empty() ? "Agent 会话执行失败" : ev.session.summary);
            t.updated_at = now;
            changed = true;
        }
    }
    return changed ? save_tasks_unlocked(items) : false;
}

std::string serialize_task_json(const AgentTaskItem &t) {
    std::ostringstream oss;
    oss << "{"
        << "\"id\":\"" << json_escape(t.id) << "\","
        << "\"title\":\"" << json_escape(t.title) << "\","
        << "\"notes\":\"" << json_escape(t.notes) << "\","
        << "\"repo_path\":\"" << json_escape(t.repo_path) << "\","
        << "\"priority\":\"" << json_escape(t.priority) << "\","
        << "\"status\":\"" << task_status_str(t.status) << "\","
        << "\"preferred_agent\":\"" << json_escape(t.preferred_agent) << "\","
        << "\"assigned_session_id\":\"" << json_escape(t.assigned_session_id) << "\","
        << "\"result_summary\":\"" << json_escape(t.result_summary) << "\","
        << "\"created_at\":" << t.created_at << ","
        << "\"updated_at\":" << t.updated_at
        << "}";
    return oss.str();
}

std::string serialize_tasks_json(const std::vector<AgentTaskItem> &tasks) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (i > 0) oss << ",";
        oss << serialize_task_json(tasks[i]);
    }
    oss << "]";
    return oss.str();
}

}  // namespace spinel::ai
