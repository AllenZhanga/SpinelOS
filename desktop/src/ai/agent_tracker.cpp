#include "ai/agent_tracker.hpp"
#include "ai/json_util.hpp"
#include "common.hpp"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
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

std::mutex g_tracker_mu;
std::map<std::string, AgentSessionInfo> g_sessions;
AgentTransitionFn g_transition_cb;

std::string to_lower_ascii(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool pid_is_alive(pid_t pid) {
    if (pid <= 1) return false;
    if (::kill(pid, 0) == 0) return true;
    return errno == EPERM;
}

std::string read_proc_cmdline(const fs::path &pid_dir) {
    std::ifstream ifs(pid_dir / "cmdline", std::ios::binary);
    if (!ifs.is_open()) return "";
    std::string raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    for (char &ch : raw) {
        if (ch == '\0') ch = ' ';
    }
    return trim_copy(raw);
}

bool proc_has_sandbox_env(const fs::path &pid_dir) {
    std::ifstream ifs(pid_dir / "environ", std::ios::binary);
    if (!ifs.is_open()) return false;
    std::string raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return raw.find("SPINEL_SANDBOX_SHELL=1") != std::string::npos ||
           raw.find("SPINEL_IN_SANDBOX=1") != std::string::npos;
}

bool parse_session_json(const std::string &raw, AgentSessionInfo &out) {
    AgentSessionInfo info;
    bool parsed = json_parse_object_str(raw, [&](const std::string &k, const std::string &v, const std::string &) {
        if (k == "id") info.id = sanitize_agent_session_id(v);
        else if (k == "agent") info.agent = v;
        else if (k == "title") info.title = v;
        else if (k == "repo" || k == "repo_path") info.repo_path = v;
        else if (k == "pid") info.pid = static_cast<pid_t>(json_scalar_i64(v, 0));
        else if (k == "status") info.status = parse_agent_status(v);
        else if (k == "summary") info.summary = v;
        else if (k == "win_id") info.win_id = static_cast<uint64_t>(std::max(0LL, json_scalar_i64(v, 0)));
        else if (k == "workspace_idx") info.workspace_idx = static_cast<int>(json_scalar_i64(v, -1));
        else if (k == "in_sandbox") info.in_sandbox = json_scalar_bool(v, false);
        else if (k == "task_id") info.task_id = v;
        else if (k == "approval_id") info.approval_id = v;
        else if (k == "source") info.source = v;
        else if (k == "updated_at") info.updated_at = json_scalar_i64(v, 0);
    });
    if (!parsed || (info.id.empty() && info.agent.empty())) return false;
    if (info.id.empty()) {
        info.id = sanitize_agent_session_id(info.agent + "-" + std::to_string(info.pid));
    }
    out = std::move(info);
    return true;
}

std::vector<AgentSessionInfo> scan_proc_agents_unlocked(long long now_sec) {
    std::vector<AgentSessionInfo> discovered;
    std::error_code ec;
    if (!fs::is_directory("/proc", ec)) return discovered;

    const uid_t my_uid = ::getuid();
    for (const auto &entry : fs::directory_iterator("/proc", fs::directory_options::skip_permission_denied, ec)) {
        std::string pname = entry.path().filename().string();
        if (pname.empty() || !std::all_of(pname.begin(), pname.end(), [](unsigned char c) { return std::isdigit(c); })) {
            continue;
        }
        pid_t pid = static_cast<pid_t>(std::strtol(pname.c_str(), nullptr, 10));
        if (pid <= 1 || pid == ::getpid()) continue;

        // Only inspect processes belonging to current user when statable.
        struct stat st{};
        if (::stat(entry.path().c_str(), &st) != 0 || st.st_uid != my_uid) continue;

        std::string cmdline = read_proc_cmdline(entry.path());
        std::string agent_name;
        if (!match_known_agent_cmdline(cmdline, agent_name)) continue;

        std::error_code cwd_ec;
        std::string cwd = fs::read_symlink(entry.path() / "cwd", cwd_ec).string();
        if (cwd_ec) cwd.clear();

        AgentSessionInfo info;
        info.id = "proc-" + std::to_string(pid);
        info.agent = agent_name;
        info.title = !cwd.empty() ? (agent_name + " · " + fs::path(cwd).filename().string()) : agent_name;
        info.repo_path = cwd;
        info.pid = pid;
        info.status = AgentSessionStatus::Running;
        info.in_sandbox = proc_has_sandbox_env(entry.path());
        info.source = "proc";
        info.updated_at = now_sec;
        discovered.push_back(std::move(info));
    }
    return discovered;
}

int status_priority(AgentSessionStatus s) {
    switch (s) {
        case AgentSessionStatus::WaitingUser: return 0;
        case AgentSessionStatus::Running: return 1;
        case AgentSessionStatus::Error: return 2;
        case AgentSessionStatus::Done: return 3;
    }
    return 4;
}

}  // namespace

const char *agent_status_str(AgentSessionStatus s) {
    switch (s) {
        case AgentSessionStatus::Running: return "running";
        case AgentSessionStatus::WaitingUser: return "waiting_user";
        case AgentSessionStatus::Done: return "done";
        case AgentSessionStatus::Error: return "error";
    }
    return "running";
}

AgentSessionStatus parse_agent_status(const std::string &s) {
    std::string l = to_lower_ascii(trim_copy(s));
    if (l == "waiting_user" || l == "waiting" || l == "approval" || l == "needs_input") {
        return AgentSessionStatus::WaitingUser;
    }
    if (l == "done" || l == "completed" || l == "finished" || l == "success") {
        return AgentSessionStatus::Done;
    }
    if (l == "error" || l == "failed" || l == "fail") {
        return AgentSessionStatus::Error;
    }
    return AgentSessionStatus::Running;
}

std::string sanitize_agent_session_id(const std::string &raw) {
    std::string out;
    out.reserve(std::min<size_t>(raw.size(), 80));
    for (char c : raw) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '-' || c == '_' || c == '.' || c == ':') {
            out += c;
        } else if (c == ' ' || c == '/') {
            out += '-';
        }
        if (out.size() >= 80) break;
    }
    while (!out.empty() && out.front() == '.') out.erase(out.begin());
    if (out.find("..") != std::string::npos) {
        std::replace(out.begin(), out.end(), '.', '_');
    }
    return out;
}

std::string agent_sessions_runtime_dir() {
    if (const char *ov = std::getenv("SPINEL_AGENT_RUNTIME_DIR"); ov && *ov) {
        return ov;
    }
    const char *xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) {
        return (fs::path(xdg) / "spinelos/agents").string();
    }
    std::error_code ec;
    fs::path run_user = fs::path("/run/user") / std::to_string(::getuid());
    if (fs::is_directory(run_user, ec)) {
        return (run_user / "spinelos/agents").string();
    }
    return "/tmp/spinelos-" + std::to_string(::getuid()) + "/agents";
}

bool match_known_agent_cmdline(const std::string &cmdline, std::string &agent_out) {
    agent_out.clear();
    std::string low = to_lower_ascii(trim_copy(cmdline));
    if (low.empty()) return false;

    // Exclude our own helpers or shell inspection commands.
    if (low.find("spinel-agent-ctl") != std::string::npos ||
        low.find("spinel-os-mcp") != std::string::npos ||
        low.find("spinel-tests") != std::string::npos) {
        return false;
    }

    std::istringstream iss(low);
    std::vector<std::string> tokens;
    std::string tok;
    while (iss >> tok && tokens.size() < 6) {
        tokens.push_back(tok);
    }
    if (tokens.empty()) return false;

    std::string arg0 = fs::path(tokens[0]).filename().string();
    if (arg0 == "grep" || arg0 == "rg" || arg0 == "pgrep" || arg0 == "ps" || arg0 == "pkill") {
        return false;
    }

    struct AgentRule {
        const char *needle;
        const char *canonical;
    };
    static constexpr AgentRule kRules[] = {
        {"deepseekharness", "DeepSeekHarness"},
        {"deepseek-harness", "DeepSeekHarness"},
        {"antigravity", "Antigravity"},
        {"codex", "Codex"},
        {"claude", "Claude"},
        {"opencode", "OpenCode"},
        {"aider", "Aider"},
        {"gemini", "GeminiCLI"},
    };

    // Inspect binary basename first, then runtime script/module arguments (for node/python wrapped CLIs).
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0 && !tokens[i].empty() && tokens[i][0] == '-') continue;
        std::string hay = (i == 0) ? arg0 : tokens[i];
        for (const auto &rule : kRules) {
            if (hay == rule.needle || hay.find(rule.needle) != std::string::npos) {
                agent_out = rule.canonical;
                return true;
            }
        }
        if (i == 0 && arg0 != "node" && arg0 != "nodejs" && arg0 != "python" &&
            arg0 != "python3" && arg0 != "uv" && arg0 != "npx" && arg0 != "bun") {
            break;
        }
    }
    return false;
}

AgentTransitionEvent upsert_agent_session(const AgentSessionInfo &info, bool persist_file) {
    AgentSessionInfo clean = info;
    if (clean.id.empty()) {
        std::string seed = (clean.agent.empty() ? "agent" : clean.agent) + "-" +
                           std::to_string(clean.pid > 0 ? clean.pid : ::getpid());
        clean.id = sanitize_agent_session_id(seed);
    } else {
        clean.id = sanitize_agent_session_id(clean.id);
    }
    if (clean.agent.empty()) clean.agent = "Agent";
    if (clean.updated_at <= 0) clean.updated_at = static_cast<long long>(std::time(nullptr));

    AgentTransitionEvent ev;
    AgentTransitionFn cb_copy;
    {
        std::lock_guard<std::mutex> lock(g_tracker_mu);
        auto it = g_sessions.find(clean.id);
        if (it != g_sessions.end()) {
            ev.has_prev = true;
            ev.prev_status = it->second.status;
            if (clean.pid <= 0) clean.pid = it->second.pid;
            if (clean.repo_path.empty()) clean.repo_path = it->second.repo_path;
            if (clean.title.empty()) clean.title = it->second.title;
            if (clean.win_id == 0) clean.win_id = it->second.win_id;
            if (clean.task_id.empty()) clean.task_id = it->second.task_id;
            if (!clean.in_sandbox) clean.in_sandbox = it->second.in_sandbox;
        }
        g_sessions[clean.id] = clean;
        ev.session_id = clean.id;
        ev.new_status = clean.status;
        ev.session = clean;
        cb_copy = g_transition_cb;
    }

    if (persist_file) {
        std::error_code ec;
        fs::path dir = agent_sessions_runtime_dir();
        fs::create_directories(dir, ec);
        write_text_file((dir / (clean.id + ".json")).string(), serialize_agent_session_json(clean));
    }

    if (cb_copy && (!ev.has_prev || ev.prev_status != ev.new_status)) {
        cb_copy(ev);
    }
    return ev;
}

bool clear_agent_session(const std::string &session_id) {
    std::string clean_id = sanitize_agent_session_id(session_id);
    std::error_code ec;
    fs::path dir = agent_sessions_runtime_dir();

    std::lock_guard<std::mutex> lock(g_tracker_mu);
    if (clean_id.empty()) {
        std::vector<std::string> to_remove;
        for (const auto &kv : g_sessions) {
            if (kv.second.status == AgentSessionStatus::Done ||
                kv.second.status == AgentSessionStatus::Error) {
                to_remove.push_back(kv.first);
            }
        }
        for (const auto &id : to_remove) {
            g_sessions.erase(id);
            fs::remove(dir / (id + ".json"), ec);
        }
        return true;
    }

    bool removed = g_sessions.erase(clean_id) > 0;
    bool file_removed = fs::remove(dir / (clean_id + ".json"), ec);
    return removed || file_removed;
}

void set_agent_transition_callback(AgentTransitionFn cb) {
    std::lock_guard<std::mutex> lock(g_tracker_mu);
    g_transition_cb = std::move(cb);
}

void bind_sessions_to_windows(std::vector<AgentSessionInfo> &sessions,
                              const std::vector<WindowBindingRef> &windows) {
    if (windows.empty()) return;
    for (auto &s : sessions) {
        if (s.win_id != 0) {
            bool still_open = std::any_of(windows.begin(), windows.end(), [&](const WindowBindingRef &w) {
                return w.id == s.win_id;
            });
            if (still_open) continue;
            s.win_id = 0;
        }
        std::string repo_leaf = s.repo_path.empty() ? "" : to_lower_ascii(fs::path(s.repo_path).filename().string());
        std::string low_agent = to_lower_ascii(s.agent);
        int best_score = 0;
        uint64_t best_id = 0;
        for (const auto &w : windows) {
            std::string hay = to_lower_ascii(w.app_id + " " + w.title);
            int score = 0;
            if (!low_agent.empty() && hay.find(low_agent) != std::string::npos) score += 4;
            if (!repo_leaf.empty() && repo_leaf.size() >= 2 && hay.find(repo_leaf) != std::string::npos) score += 3;
            if (score > 0 && w.activated) score += 1;
            if (score > best_score) {
                best_score = score;
                best_id = w.id;
            }
        }
        if (best_id != 0) {
            s.win_id = best_id;
        }
    }
}

std::vector<AgentSessionInfo> reconcile_agent_sessions(long long now_sec,
                                                     long long finished_ttl_sec,
                                                     bool include_proc_scan,
                                                     const std::vector<WindowBindingRef> &windows) {
    if (now_sec <= 0) now_sec = static_cast<long long>(std::time(nullptr));

    std::vector<AgentTransitionEvent> transitions;
    std::vector<AgentSessionInfo> result;
    AgentTransitionFn cb_copy;

    {
        std::lock_guard<std::mutex> lock(g_tracker_mu);
        cb_copy = g_transition_cb;
        std::error_code ec;
        fs::path dir = agent_sessions_runtime_dir();

        if (fs::is_directory(dir, ec)) {
            for (const auto &de : fs::directory_iterator(dir, ec)) {
                if (!de.is_regular_file(ec) || de.path().extension() != ".json") continue;
                std::ifstream ifs(de.path());
                if (!ifs.is_open()) continue;
                std::string raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                AgentSessionInfo file_info;
                if (!parse_session_json(raw, file_info)) continue;

                auto existing = g_sessions.find(file_info.id);
                if (file_info.updated_at <= 0) {
                    file_info.updated_at = (existing != g_sessions.end()) ? existing->second.updated_at : now_sec;
                }
                if (existing == g_sessions.end() ||
                    existing->second.source == "proc" ||
                    file_info.updated_at > existing->second.updated_at) {
                    if (existing != g_sessions.end() && existing->second.status != file_info.status) {
                        transitions.push_back({file_info.id, true, existing->second.status, file_info.status, file_info});
                    } else if (existing == g_sessions.end()) {
                        transitions.push_back({file_info.id, false, file_info.status, file_info.status, file_info});
                    }
                    g_sessions[file_info.id] = file_info;
                }
            }
        }

        // Remove expired Done/Error items and stale proc-only entries.
        std::vector<std::string> remove_ids;
        for (auto &kv : g_sessions) {
            auto &s = kv.second;
            if (s.source == "proc") {
                if (!pid_is_alive(s.pid)) {
                    remove_ids.push_back(kv.first);
                }
                continue;
            }
            if ((s.status == AgentSessionStatus::Done || s.status == AgentSessionStatus::Error) &&
                finished_ttl_sec > 0 && s.updated_at > 0 && (now_sec - s.updated_at) > finished_ttl_sec) {
                remove_ids.push_back(kv.first);
            } else if (s.status == AgentSessionStatus::Running && s.pid > 1 && !pid_is_alive(s.pid)) {
                // Process disappeared before reporting final status: mark done and persist.
                AgentSessionStatus prev = s.status;
                s.status = AgentSessionStatus::Done;
                if (s.summary.empty()) s.summary = "会话进程已退出";
                s.updated_at = std::max(now_sec, s.updated_at + 1);
                fs::create_directories(dir, ec);
                write_text_file((dir / (s.id + ".json")).string(), serialize_agent_session_json(s));
                transitions.push_back({s.id, true, prev, s.status, s});
            }
        }
        for (const auto &id : remove_ids) {
            g_sessions.erase(id);
            fs::remove(dir / (id + ".json"), ec);
        }

        if (include_proc_scan) {
            auto procs = scan_proc_agents_unlocked(now_sec);
            for (auto &p : procs) {
                // Skip /proc entry if a drop-in/ctl session already owns this PID.
                bool owned = false;
                for (const auto &kv : g_sessions) {
                    if (kv.second.pid == p.pid && kv.second.source != "proc") {
                        owned = true;
                        break;
                    }
                }
                if (!owned) {
                    auto it = g_sessions.find(p.id);
                    if (it == g_sessions.end()) {
                        transitions.push_back({p.id, false, p.status, p.status, p});
                    }
                    g_sessions[p.id] = p;
                }
            }
        }

        result.reserve(g_sessions.size());
        for (const auto &kv : g_sessions) {
            result.push_back(kv.second);
        }
    }

    if (cb_copy) {
        for (const auto &ev : transitions) {
            cb_copy(ev);
        }
    }

    bind_sessions_to_windows(result, windows);
    std::stable_sort(result.begin(), result.end(), [](const AgentSessionInfo &a, const AgentSessionInfo &b) {
        int pa = status_priority(a.status);
        int pb = status_priority(b.status);
        if (pa != pb) return pa < pb;
        return a.updated_at > b.updated_at;
    });
    return result;
}

void reset_agent_tracker_for_tests() {
    std::lock_guard<std::mutex> lock(g_tracker_mu);
    g_sessions.clear();
    g_transition_cb = nullptr;
}

std::string serialize_agent_session_json(const AgentSessionInfo &s) {
    std::ostringstream oss;
    oss << "{"
        << "\"id\":\"" << json_escape(s.id) << "\","
        << "\"agent\":\"" << json_escape(s.agent) << "\","
        << "\"title\":\"" << json_escape(s.title) << "\","
        << "\"repo_path\":\"" << json_escape(s.repo_path) << "\","
        << "\"pid\":" << static_cast<long long>(s.pid) << ","
        << "\"status\":\"" << agent_status_str(s.status) << "\","
        << "\"summary\":\"" << json_escape(s.summary) << "\","
        << "\"win_id\":" << s.win_id << ","
        << "\"workspace_idx\":" << s.workspace_idx << ","
        << "\"in_sandbox\":" << (s.in_sandbox ? "true" : "false") << ","
        << "\"task_id\":\"" << json_escape(s.task_id) << "\","
        << "\"approval_id\":\"" << json_escape(s.approval_id) << "\","
        << "\"source\":\"" << json_escape(s.source) << "\","
        << "\"updated_at\":" << s.updated_at
        << "}";
    return oss.str();
}

std::string serialize_agent_sessions_json(const std::vector<AgentSessionInfo> &sessions) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < sessions.size(); ++i) {
        if (i > 0) oss << ",";
        oss << serialize_agent_session_json(sessions[i]);
    }
    oss << "]";
    return oss.str();
}

}  // namespace spinel::ai
