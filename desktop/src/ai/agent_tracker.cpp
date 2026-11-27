#include "ai/agent_tracker.hpp"
#include "ai/json_util.hpp"
#include "common.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
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
    struct RawProcMatch {
        pid_t pid = 0;
        pid_t ppid = 0;
        std::string agent;
        std::string cwd;
        bool in_sandbox = false;
    };
    std::vector<RawProcMatch> candidates;
    std::map<pid_t, size_t> pid_to_idx;

    auto read_proc_ppid = [](const fs::path &pid_dir) -> pid_t {
        std::ifstream ifs(pid_dir / "stat");
        if (!ifs.is_open()) return 0;
        std::string line;
        std::getline(ifs, line);
        auto rparen = line.rfind(')');
        if (rparen == std::string::npos || rparen + 2 >= line.size()) return 0;
        std::istringstream iss(line.substr(rparen + 2));
        char state_ch = '?';
        pid_t ppid = 0;
        if (iss >> state_ch >> ppid) return ppid;
        return 0;
    };

    auto is_electron_gui_binary = [](const fs::path &pid_dir, const std::string &cmdline) -> bool {
        if (cmdline.find("--ozone-platform") != std::string::npos ||
            cmdline.find("--app-path=") != std::string::npos) {
            return true;
        }
        std::error_code exe_ec;
        fs::path exe = fs::read_symlink(pid_dir / "exe", exe_ec);
        if (!exe_ec && !exe.empty()) {
            fs::path app_asar = exe.parent_path() / "resources" / "app.asar";
            if (fs::exists(app_asar, exe_ec)) return true;
        }
        return false;
    };

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

        // An open Electron GUI IDE window (without any CLI/reported agent task) is not an active AI session.
        if (is_electron_gui_binary(entry.path(), cmdline)) continue;

        std::error_code cwd_ec;
        std::string cwd = fs::read_symlink(entry.path() / "cwd", cwd_ec).string();
        if (cwd_ec || cwd.rfind("/proc/", 0) == 0) cwd.clear();

        pid_to_idx[pid] = candidates.size();
        candidates.push_back({pid, read_proc_ppid(entry.path()), agent_name, std::move(cwd), proc_has_sandbox_env(entry.path())});
    }

    // Collapse child workers whose ancestor is also a matched CLI agent process.
    for (const auto &c : candidates) {
        bool has_matched_ancestor = false;
        pid_t cur = c.ppid;
        for (int depth = 0; depth < 6 && cur > 1; ++depth) {
            auto it = pid_to_idx.find(cur);
            if (it != pid_to_idx.end()) {
                has_matched_ancestor = true;
                break;
            }
            cur = read_proc_ppid(fs::path("/proc") / std::to_string(cur));
        }
        if (has_matched_ancestor) continue;

        AgentSessionInfo info;
        info.id = "proc-" + std::to_string(c.pid);
        info.agent = c.agent;
        info.title = !c.cwd.empty() ? (c.agent + " · " + fs::path(c.cwd).filename().string()) : c.agent;
        info.repo_path = c.cwd;
        info.pid = c.pid;
        info.status = AgentSessionStatus::Running;
        info.in_sandbox = c.in_sandbox;
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

std::string antigravity_daemon_dir() {
    if (const char *ov = std::getenv("SPINEL_ANTIGRAVITY_DAEMON_DIR"); ov && *ov) {
        return ov;
    }
    return (fs::path(home_dir()) / ".gemini/antigravity/daemon").string();
}

std::string decode_chunked_http_body(const std::string &raw) {
    std::string out;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t eol = raw.find("\r\n", pos);
        if (eol == std::string::npos) break;
        std::string hex_len = trim_copy(raw.substr(pos, eol - pos));
        auto semi = hex_len.find(';');
        if (semi != std::string::npos) hex_len = hex_len.substr(0, semi);
        if (hex_len.empty()) break;
        size_t chunk_sz = static_cast<size_t>(std::strtoul(hex_len.c_str(), nullptr, 16));
        pos = eol + 2;
        if (chunk_sz == 0) break;
        if (pos + chunk_sz > raw.size()) {
            out.append(raw.substr(pos));
            break;
        }
        out.append(raw, pos, chunk_sz);
        pos += chunk_sz;
        if (pos + 2 <= raw.size() && raw[pos] == '\r' && raw[pos + 1] == '\n') pos += 2;
    }
    return out;
}

bool http_post_loopback_json(int port,
                             const std::string &path,
                             const std::string &csrf_token,
                             const std::string &req_body,
                             std::string &body_out) {
    body_out.clear();
    if (port <= 0 || port > 65535) return false;

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int crc = ::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
    if (crc < 0) {
        if (errno != EINPROGRESS) {
            ::close(fd);
            return false;
        }
        pollfd pfd{fd, POLLOUT, 0};
        if (::poll(&pfd, 1, 150) <= 0 || !(pfd.revents & POLLOUT)) {
            ::close(fd);
            return false;
        }
        int so_err = 0;
        socklen_t len = sizeof(so_err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len) != 0 || so_err != 0) {
            ::close(fd);
            return false;
        }
    }

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: 127.0.0.1:" << port << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Connect-Protocol-Version: 1\r\n"
        << "x-antigravity-csrf-token: " << csrf_token << "\r\n"
        << "Content-Length: " << req_body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << req_body;
    const std::string wire = req.str();
    size_t sent = 0;
    while (sent < wire.size()) {
        ssize_t n = ::send(fd, wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd{fd, POLLOUT, 0};
            if (::poll(&pfd, 1, 150) <= 0) {
                ::close(fd);
                return false;
            }
        } else {
            ::close(fd);
            return false;
        }
    }

    std::string raw;
    char buf[4096];
    size_t header_end = std::string::npos;
    long long content_len = -1;
    bool chunked = false;
    while (raw.size() < 512 * 1024) {
        pollfd pfd{fd, POLLIN, 0};
        int pr = ::poll(&pfd, 1, 200);
        if (pr <= 0) break;
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            break;
        }
        raw.append(buf, static_cast<size_t>(n));
        if (header_end == std::string::npos) {
            header_end = raw.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::string hdrs = to_lower_ascii(raw.substr(0, header_end));
                if (hdrs.find("transfer-encoding: chunked") != std::string::npos) {
                    chunked = true;
                } else {
                    auto cl_pos = hdrs.find("content-length:");
                    if (cl_pos != std::string::npos) {
                        content_len = std::strtoll(hdrs.c_str() + cl_pos + 15, nullptr, 10);
                    }
                }
            }
        }
        if (header_end != std::string::npos && !chunked && content_len >= 0 &&
            raw.size() >= header_end + 4 + static_cast<size_t>(content_len)) {
            break;
        }
        if (chunked && raw.find("\r\n0\r\n\r\n", header_end + 4) != std::string::npos) {
            break;
        }
    }
    ::close(fd);

    if (header_end == std::string::npos) return false;
    std::string status_line = raw.substr(0, raw.find("\r\n"));
    if (status_line.find(" 200 ") == std::string::npos) return false;

    std::string body = raw.substr(header_end + 4);
    body_out = chunked ? decode_chunked_http_body(body) : body;
    return !body_out.empty();
}

std::vector<AgentSessionInfo> scan_antigravity_hub_sessions(long long now_sec, bool &queried_ok) {
    queried_ok = false;
    std::vector<AgentSessionInfo> active;
    std::error_code ec;
    fs::path ddir = antigravity_daemon_dir();
    if (!fs::is_directory(ddir, ec)) return active;

    bool any_live_hub = false;
    bool all_live_succeeded = true;

    for (const auto &de : fs::directory_iterator(ddir, fs::directory_options::skip_permission_denied, ec)) {
        if (!de.is_regular_file(ec)) continue;
        std::string fname = de.path().filename().string();
        if (fname.rfind("hub_", 0) != 0 || de.path().extension() != ".json") continue;

        std::ifstream ifs(de.path());
        if (!ifs.is_open()) continue;
        std::string raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        pid_t hub_pid = 0;
        int http_port = 0;
        std::string csrf_token;
        json_parse_object_str(raw, [&](const std::string &k, const std::string &v, const std::string &) {
            if (k == "pid") hub_pid = static_cast<pid_t>(json_scalar_i64(v, 0));
            else if (k == "httpPort") http_port = static_cast<int>(json_scalar_i64(v, 0));
            else if (k == "csrfToken") csrf_token = v;
        });
        if (hub_pid <= 1 || !pid_is_alive(hub_pid) || http_port <= 0 || csrf_token.empty()) continue;

        any_live_hub = true;
        std::string resp;
        if (!http_post_loopback_json(http_port,
                                     "/product.v1.ConversationService/ListConversations",
                                     csrf_token,
                                     R"({"pageSize":20,"includeArchived":false})",
                                     resp)) {
            all_live_succeeded = false;
            continue;
        }

        std::string convs_raw;
        json_parse_object_str(resp, [&](const std::string &k, const std::string &, const std::string &r) {
            if (k == "conversations" && !r.empty() && r.front() == '[') convs_raw = r;
        });
        if (convs_raw.empty()) continue;

        json_parse_array_str(convs_raw, [&](const std::string &item_raw) {
            std::string cid;
            std::string title;
            std::string summary;
            std::string run_state;
            json_parse_object_str(item_raw, [&](const std::string &k, const std::string &v, const std::string &obj_raw) {
                if (k == "id") {
                    cid = v;
                } else if (k == "annotations" && !obj_raw.empty() && obj_raw.front() == '{') {
                    json_parse_object_str(obj_raw, [&](const std::string &ak, const std::string &av, const std::string &) {
                        if (ak == "title") title = trim_copy(av);
                    });
                } else if (k == "state" && !obj_raw.empty() && obj_raw.front() == '{') {
                    json_parse_object_str(obj_raw, [&](const std::string &sk, const std::string &sv, const std::string &) {
                        if (sk == "runState") run_state = trim_copy(sv);
                        else if (sk == "summary") summary = trim_copy(sv);
                    });
                }
            });
            if (cid.empty()) return;
            if (run_state != "RUN_STATE_RUNNING" && run_state != "RUN_STATE_CANCELLING") return;

            AgentSessionInfo info;
            info.id = sanitize_agent_session_id("agy-" + cid);
            info.agent = "Antigravity";
            info.title = !title.empty() ? title : (summary.size() > 1 ? summary : "对话中");
            info.summary = (summary.size() > 1 && summary != info.title)
                               ? summary
                               : (run_state == "RUN_STATE_CANCELLING" ? "正在中止回合..." : "AI 回合执行中");
            info.pid = hub_pid;
            info.status = AgentSessionStatus::Running;
            info.source = "antigravity_hub";
            info.updated_at = now_sec;
            active.push_back(std::move(info));
        });
    }

    queried_ok = any_live_hub && all_live_succeeded;
    return active;
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

    // Exclude Chromium/Electron multi-process helpers & background sidecar hosts.
    if (low.find("--type=renderer") != std::string::npos ||
        low.find("--type=zygote") != std::string::npos ||
        low.find("--type=gpu-process") != std::string::npos ||
        low.find("--type=utility") != std::string::npos ||
        low.find("--type=crashpad-handler") != std::string::npos ||
        low.find("--utility-sub-type=") != std::string::npos ||
        low.find("--renderer-client-id=") != std::string::npos ||
        low.find("--gpu-preferences=") != std::string::npos ||
        low.find("sidecar_sdk") != std::string::npos) {
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

    const bool is_runtime_wrapper =
        (arg0 == "node" || arg0 == "nodejs" || arg0 == "python" ||
         arg0 == "python3" || arg0 == "uv" || arg0 == "npx" || arg0 == "bun");

    // Inspect binary basename first; for script runtimes inspect only the first non-flag entrypoint token.
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0 && !tokens[i].empty() && tokens[i][0] == '-') continue;
        std::string hay = (i == 0) ? arg0 : tokens[i];
        for (const auto &rule : kRules) {
            if (hay == rule.needle || hay.find(rule.needle) != std::string::npos) {
                agent_out = rule.canonical;
                return true;
            }
        }
        if (i == 0 && !is_runtime_wrapper) {
            break;
        }
        if (i > 0) {
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

    bool hub_queried_ok = false;
    std::vector<AgentSessionInfo> hub_running;
    if (include_proc_scan) {
        hub_running = scan_antigravity_hub_sessions(now_sec, hub_queried_ok);
    }

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
                // Process disappeared before reporting final status: mark done.
                AgentSessionStatus prev = s.status;
                s.status = AgentSessionStatus::Done;
                if (s.summary.empty()) s.summary = "会话进程已退出";
                s.updated_at = std::max(now_sec, s.updated_at + 1);
                if (s.source != "antigravity_hub") {
                    fs::create_directories(dir, ec);
                    write_text_file((dir / (s.id + ".json")).string(), serialize_agent_session_json(s));
                }
                transitions.push_back({s.id, true, prev, s.status, s});
            }
        }
        for (const auto &id : remove_ids) {
            g_sessions.erase(id);
            fs::remove(dir / (id + ".json"), ec);
        }

        if (include_proc_scan) {
            // 1. Live Antigravity ConversationService runs (authoritative running vs idle).
            std::map<std::string, AgentSessionInfo> hub_map;
            for (auto &hs : hub_running) hub_map[hs.id] = hs;
            if (hub_queried_ok) {
                for (auto &kv : g_sessions) {
                    if (kv.second.source == "antigravity_hub" &&
                        kv.second.status == AgentSessionStatus::Running &&
                        hub_map.find(kv.first) == hub_map.end()) {
                        AgentSessionStatus prev = kv.second.status;
                        kv.second.status = AgentSessionStatus::Done;
                        kv.second.summary = "回合已完成";
                        kv.second.updated_at = std::max(now_sec, kv.second.updated_at + 1);
                        transitions.push_back({kv.first, true, prev, kv.second.status, kv.second});
                    }
                }
            }
            for (const auto &kv : hub_map) {
                auto it = g_sessions.find(kv.first);
                if (it == g_sessions.end()) {
                    transitions.push_back({kv.first, false, kv.second.status, kv.second.status, kv.second});
                } else if (it->second.status != kv.second.status) {
                    transitions.push_back({kv.first, true, it->second.status, kv.second.status, kv.second});
                }
                g_sessions[kv.first] = kv.second;
            }

            // 2. CLI Agent /proc probe (Codex, Claude, GeminiCLI, OpenCode, Aider, DeepSeekHarness).
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
