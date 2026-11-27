#include "ai/agent_bus.hpp"
#include "ai/agent_perms.hpp"
#include "ai/agent_tracker.hpp"
#include "ai/json_util.hpp"
#include "ai/task_inbox.hpp"
#include "common.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void print_usage() {
    std::cout <<
        "Spinel AI OS Control CLI (spinel-agent-ctl)\n\n"
        "Usage:\n"
        "  spinel-agent-ctl ping\n"
        "  spinel-agent-ctl report --agent <name> --status <running|waiting_user|done|error>\n"
        "                          [--id <sid>] [--title <text>] [--repo <dir>] [--summary <text>]\n"
        "                          [--pid <pid>] [--task <task_id>] [--sandbox]\n"
        "  spinel-agent-ctl list [--json]\n"
        "  spinel-agent-ctl clear [session_id]\n"
        "  spinel-agent-ctl todo list [--status <s|...>] [--repo <dir>] [--json]\n"
        "  spinel-agent-ctl todo add <title> [--repo <dir>] [--agent <name>] [--priority <low|normal|high>]\n"
        "  spinel-agent-ctl todo claim <task_id> [--session <sid>] [--agent <name>]\n"
        "  spinel-agent-ctl todo done <task_id> [--summary <text>] [--accept]\n"
        "  spinel-agent-ctl todo accept <task_id>\n"
        "  spinel-agent-ctl context [--json]\n"
        "  spinel-agent-ctl perms [set <key=val> ...]\n"
        "  spinel-agent-ctl snapshot <description>\n"
        "  spinel-agent-ctl approvals\n"
        "  spinel-agent-ctl approve <approval_id> <allow|sandbox|deny>\n"
        "  spinel-agent-ctl mcp-snippet [codex|claude|gemini|deepseek|json]\n"
        "  spinel-agent-ctl wrap --agent <name> [--repo <dir>] [--task <task_id>] [--title <text>] [--sandbox] -- <cmd...>\n";
}

bool try_bus_or_print(const std::string &req_json, std::string &resp_out, int timeout_ms = 5000) {
    return spinel::ai::call_agent_bus_socket(spinel::ai::agent_bus_socket_path(), req_json, resp_out, timeout_ms);
}

bool bus_response_ok(const std::string &resp_json) {
    bool ok = false;
    spinel::ai::json_parse_object_str(resp_json, [&](const std::string &k, const std::string &v, const std::string &) {
        if (k == "ok") ok = spinel::ai::json_scalar_bool(v, false);
    });
    return ok;
}

std::string build_rpc(const std::string &method, const std::string &params_json) {
    return "{\"id\":1,\"client\":\"spinel-agent-ctl\",\"method\":\"" +
           json_escape(method) + "\",\"params\":" + params_json + "}";
}

void report_session_hybrid(const spinel::ai::AgentSessionInfo &info, std::string *resp_out = nullptr) {
    std::ostringstream p;
    p << "{"
      << "\"id\":\"" << json_escape(info.id) << "\","
      << "\"agent\":\"" << json_escape(info.agent) << "\","
      << "\"title\":\"" << json_escape(info.title) << "\","
      << "\"repo\":\"" << json_escape(info.repo_path) << "\","
      << "\"status\":\"" << spinel::ai::agent_status_str(info.status) << "\","
      << "\"summary\":\"" << json_escape(info.summary) << "\","
      << "\"pid\":" << static_cast<long long>(info.pid) << ","
      << "\"task_id\":\"" << json_escape(info.task_id) << "\","
      << "\"in_sandbox\":" << (info.in_sandbox ? "true" : "false") << ","
      << "\"source\":\"ctl\""
      << "}";
    std::string resp;
    if (try_bus_or_print(build_rpc("agent.report", p.str()), resp, 3000)) {
        if (resp_out) *resp_out = resp;
        return;
    }
    auto ev = spinel::ai::upsert_agent_session(info, true);
    spinel::ai::sync_task_with_agent_transition(ev);
    if (resp_out) *resp_out = spinel::ai::serialize_agent_session_json(ev.session);
}

void print_mcp_snippet(const std::string &target) {
    std::string mcp_bin = "/usr/local/bin/spinel-os-mcp";
    if (target == "codex") {
        std::cout << "# ~/.codex/config.toml\n"
                  << "[mcp_servers.spinel_os]\n"
                  << "command = \"" << mcp_bin << "\"\n"
                  << "env = { SPINEL_AGENT_CLIENT = \"Codex\" }\n";
        return;
    }
    if (target == "claude") {
        std::cout << "{\n"
                  << "  \"mcpServers\": {\n"
                  << "    \"spinel-os\": {\n"
                  << "      \"command\": \"" << mcp_bin << "\",\n"
                  << "      \"env\": { \"SPINEL_AGENT_CLIENT\": \"Claude\" }\n"
                  << "    }\n"
                  << "  }\n"
                  << "}\n";
        return;
    }
    std::string client = target.empty() ? "Agent" : target;
    if (client == "gemini") client = "GeminiCLI";
    else if (client == "deepseek") client = "DeepSeekHarness";
    else if (client == "json") client = "Agent";
    std::cout << "{\n"
              << "  \"mcpServers\": {\n"
              << "    \"spinel-os\": {\n"
              << "      \"type\": \"stdio\",\n"
              << "      \"command\": \"" << mcp_bin << "\",\n"
              << "      \"args\": [],\n"
              << "      \"env\": { \"SPINEL_AGENT_CLIENT\": \"" << json_escape(client) << "\" }\n"
              << "    }\n"
              << "  }\n"
              << "}\n";
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string sub = argv[1];
    if (sub == "-h" || sub == "--help") {
        print_usage();
        return 0;
    }

    if (sub == "ping") {
        std::string resp;
        if (!try_bus_or_print(build_rpc("ping", "{}"), resp, 2000)) {
            std::cerr << "[offline] agent_bus 未运行: " << spinel::ai::agent_bus_socket_path() << "\n";
            return 1;
        }
        std::cout << resp << "\n";
        return 0;
    }

    if (sub == "report") {
        spinel::ai::AgentSessionInfo info;
        info.pid = ::getppid();
        info.source = "ctl";
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if ((a == "--agent" || a == "-a") && i + 1 < argc) info.agent = argv[++i];
            else if ((a == "--status" || a == "-s") && i + 1 < argc) info.status = spinel::ai::parse_agent_status(argv[++i]);
            else if ((a == "--id") && i + 1 < argc) info.id = argv[++i];
            else if ((a == "--title" || a == "-t") && i + 1 < argc) info.title = argv[++i];
            else if ((a == "--repo" || a == "-r") && i + 1 < argc) info.repo_path = argv[++i];
            else if ((a == "--summary" || a == "-m") && i + 1 < argc) info.summary = argv[++i];
            else if ((a == "--pid" || a == "-p") && i + 1 < argc) info.pid = std::atoi(argv[++i]);
            else if ((a == "--task") && i + 1 < argc) info.task_id = argv[++i];
            else if (a == "--sandbox") info.in_sandbox = true;
        }
        if (info.agent.empty()) info.agent = "Agent";

        std::ostringstream p;
        p << "{"
          << "\"id\":\"" << json_escape(info.id) << "\","
          << "\"agent\":\"" << json_escape(info.agent) << "\","
          << "\"title\":\"" << json_escape(info.title) << "\","
          << "\"repo\":\"" << json_escape(info.repo_path) << "\","
          << "\"status\":\"" << spinel::ai::agent_status_str(info.status) << "\","
          << "\"summary\":\"" << json_escape(info.summary) << "\","
          << "\"pid\":" << static_cast<long long>(info.pid) << ","
          << "\"task_id\":\"" << json_escape(info.task_id) << "\","
          << "\"in_sandbox\":" << (info.in_sandbox ? "true" : "false") << ","
          << "\"source\":\"ctl\""
          << "}";
        std::string resp;
        if (try_bus_or_print(build_rpc("agent.report", p.str()), resp, 3000)) {
            std::cout << resp << "\n";
            return bus_response_ok(resp) ? 0 : 1;
        }
        // Offline / fallback path: still persist drop-in JSON and update Task Inbox directly.
        auto ev = spinel::ai::upsert_agent_session(info, true);
        spinel::ai::sync_task_with_agent_transition(ev);
        std::cout << spinel::ai::serialize_agent_session_json(ev.session) << "\n";
        return 0;
    }

    if (sub == "list") {
        std::string resp;
        if (try_bus_or_print(build_rpc("agent.list", "{}"), resp, 3000)) {
            std::cout << resp << "\n";
            return bus_response_ok(resp) ? 0 : 1;
        }
        auto list = spinel::ai::reconcile_agent_sessions();
        std::cout << spinel::ai::serialize_agent_sessions_json(list) << "\n";
        return 0;
    }

    if (sub == "clear") {
        std::string sid = (argc >= 3) ? argv[2] : "";
        std::string resp;
        if (try_bus_or_print(build_rpc("agent.clear", "{\"id\":\"" + json_escape(sid) + "\"}"), resp, 3000)) {
            std::cout << resp << "\n";
            return bus_response_ok(resp) ? 0 : 1;
        }
        return spinel::ai::clear_agent_session(sid) ? 0 : 1;
    }

    if (sub == "todo") {
        if (argc < 3) {
            print_usage();
            return 1;
        }
        std::string op = argv[2];
        if (op == "list") {
            std::string status_f, repo_f;
            for (int i = 3; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--status" && i + 1 < argc) status_f = argv[++i];
                else if (a == "--repo" && i + 1 < argc) repo_f = argv[++i];
            }
            std::string resp;
            std::string pj = "{\"status\":\"" + json_escape(status_f) + "\",\"repo\":\"" + json_escape(repo_f) + "\"}";
            if (try_bus_or_print(build_rpc("todo.list", pj), resp, 3000)) {
                std::cout << resp << "\n";
                return bus_response_ok(resp) ? 0 : 1;
            }
            std::cout << spinel::ai::serialize_tasks_json(spinel::ai::list_tasks(status_f, repo_f)) << "\n";
            return 0;
        }
        if (op == "add" && argc >= 4) {
            std::string title = argv[3], repo, agent, prio = "normal", notes;
            for (int i = 4; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--repo" && i + 1 < argc) repo = argv[++i];
                else if (a == "--agent" && i + 1 < argc) agent = argv[++i];
                else if (a == "--priority" && i + 1 < argc) prio = argv[++i];
                else if (a == "--notes" && i + 1 < argc) notes = argv[++i];
            }
            std::ostringstream pj;
            pj << "{\"title\":\"" << json_escape(title) << "\",\"repo\":\"" << json_escape(repo)
               << "\",\"agent\":\"" << json_escape(agent) << "\",\"priority\":\"" << json_escape(prio)
               << "\",\"notes\":\"" << json_escape(notes) << "\"}";
            std::string resp;
            if (try_bus_or_print(build_rpc("todo.create", pj.str()), resp, 3000)) {
                std::cout << resp << "\n";
                return bus_response_ok(resp) ? 0 : 1;
            }
            spinel::ai::AgentTaskItem out;
            if (!spinel::ai::create_task(title, repo, agent, prio, notes, out)) return 1;
            std::cout << spinel::ai::serialize_task_json(out) << "\n";
            return 0;
        }
        if (op == "claim" && argc >= 4) {
            std::string tid = argv[3], sid, agent;
            for (int i = 4; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--session" && i + 1 < argc) sid = argv[++i];
                else if (a == "--agent" && i + 1 < argc) agent = argv[++i];
            }
            std::string resp;
            std::string pj = "{\"id\":\"" + json_escape(tid) + "\",\"session_id\":\"" + json_escape(sid) +
                             "\",\"agent\":\"" + json_escape(agent) + "\"}";
            if (try_bus_or_print(build_rpc("todo.claim", pj), resp, 3000)) {
                std::cout << resp << "\n";
                return bus_response_ok(resp) ? 0 : 1;
            }
            spinel::ai::AgentTaskItem out;
            if (!spinel::ai::claim_task(tid, sid, agent, out)) return 1;
            std::cout << spinel::ai::serialize_task_json(out) << "\n";
            return 0;
        }
        if ((op == "done" || op == "accept") && argc >= 4) {
            std::string tid = argv[3], summary;
            bool accept_directly = (op == "accept");
            for (int i = 4; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--summary" && i + 1 < argc) summary = argv[++i];
                else if (a == "--accept") accept_directly = true;
            }
            std::string method = accept_directly ? "todo.accept" : "todo.complete";
            std::string pj = "{\"id\":\"" + json_escape(tid) + "\",\"summary\":\"" + json_escape(summary) + "\"}";
            std::string resp;
            if (try_bus_or_print(build_rpc(method, pj), resp, 3000)) {
                std::cout << resp << "\n";
                return bus_response_ok(resp) ? 0 : 1;
            }
            spinel::ai::AgentTaskItem out;
            bool ok = accept_directly ? spinel::ai::accept_task(tid, out)
                                      : spinel::ai::complete_task(tid, summary, true, out);
            if (!ok) return 1;
            std::cout << spinel::ai::serialize_task_json(out) << "\n";
            return 0;
        }
        print_usage();
        return 1;
    }

    if (sub == "context") {
        std::string resp;
        if (!try_bus_or_print(build_rpc("context.desktop", "{}"), resp, 4000)) {
            std::cerr << "[offline] context.desktop 需要运行中的 agent_bus。\n";
            return 1;
        }
        std::cout << resp << "\n";
        return bus_response_ok(resp) ? 0 : 1;
    }

    if (sub == "perms") {
        if (argc >= 4 && std::string(argv[2]) == "set") {
            std::ostringstream kv;
            std::ostringstream pj;
            pj << "{";
            for (int i = 3; i < argc; ++i) {
                std::string arg = argv[i];
                auto eq = arg.find('=');
                if (eq == std::string::npos) continue;
                std::string k = arg.substr(0, eq);
                std::string v = arg.substr(eq + 1);
                kv << k << "=" << v << "\n";
                if (pj.str().size() > 1) pj << ",";
                pj << "\"" << json_escape(k) << "\":\"" << json_escape(v) << "\"";
            }
            pj << "}";
            std::string resp;
            if (try_bus_or_print(build_rpc("perms.set", pj.str()), resp, 5000)) {
                std::cout << resp << "\n";
                return bus_response_ok(resp) ? 0 : 1;
            }
            spinel::ai::AgentPermConfig updated;
            if (!spinel::ai::update_agent_perms_kv(kv.str(), updated)) return 1;
            std::cout << spinel::ai::serialize_agent_perms_json(updated) << "\n";
            return 0;
        }
        std::cout << spinel::ai::serialize_agent_perms_json(spinel::ai::load_agent_perms()) << "\n";
        return 0;
    }

    if (sub == "snapshot" && argc >= 3) {
        std::string resp;
        if (!try_bus_or_print(build_rpc("action.snapshot", "{\"desc\":\"" + json_escape(argv[2]) + "\"}"), resp, 15000)) {
            std::cerr << "[offline] snapshot 需要运行中的 agent_bus。\n";
            return 1;
        }
        std::cout << resp << "\n";
        return bus_response_ok(resp) ? 0 : 1;
    }

    if (sub == "approvals") {
        std::string resp;
        if (!try_bus_or_print(build_rpc("approval.list", "{}"), resp, 3000)) {
            std::cerr << "[offline] approvals 需要运行中的 agent_bus。\n";
            return 1;
        }
        std::cout << resp << "\n";
        return bus_response_ok(resp) ? 0 : 1;
    }

    if (sub == "approve" && argc >= 4) {
        std::string resp;
        std::string pj = "{\"id\":\"" + json_escape(argv[2]) + "\",\"decision\":\"" + json_escape(argv[3]) + "\"}";
        if (!try_bus_or_print(build_rpc("approval.resolve", pj), resp, 3000)) {
            std::cerr << "[offline] approve 需要运行中的 agent_bus。\n";
            return 1;
        }
        std::cout << resp << "\n";
        return bus_response_ok(resp) ? 0 : 1;
    }

    if (sub == "mcp-snippet") {
        std::string target = (argc >= 3) ? argv[2] : "json";
        print_mcp_snippet(target);
        return 0;
    }

    if (sub == "wrap") {
        std::string agent = "Agent", repo, task_id, title;
        bool in_sandbox = false;
        int cmd_idx = -1;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--") {
                cmd_idx = i + 1;
                break;
            }
            if ((a == "--agent" || a == "-a") && i + 1 < argc) agent = argv[++i];
            else if ((a == "--repo" || a == "-r") && i + 1 < argc) repo = argv[++i];
            else if (a == "--task" && i + 1 < argc) task_id = argv[++i];
            else if ((a == "--title" || a == "-t") && i + 1 < argc) title = argv[++i];
            else if (a == "--sandbox") in_sandbox = true;
            else if (!a.empty() && a[0] != '-') {
                cmd_idx = i;
                break;
            }
        }
        if (cmd_idx < 0 || cmd_idx >= argc) {
            print_usage();
            return 1;
        }
        if (repo.empty()) {
            char cwd_buf[4096];
            if (::getcwd(cwd_buf, sizeof(cwd_buf))) repo = cwd_buf;
        }
        std::string sid = spinel::ai::sanitize_agent_session_id(
            agent + "-" + std::to_string(::getpid()) + "-" + std::to_string(std::time(nullptr)));

        if (!task_id.empty()) {
            std::string resp;
            std::string pj = "{\"id\":\"" + json_escape(task_id) + "\",\"session_id\":\"" +
                             json_escape(sid) + "\",\"agent\":\"" + json_escape(agent) + "\"}";
            if (!try_bus_or_print(build_rpc("todo.claim", pj), resp, 2000)) {
                spinel::ai::AgentTaskItem ignored;
                spinel::ai::claim_task(task_id, sid, agent, ignored);
            }
        }

        std::vector<char *> exec_argv;
        for (int i = cmd_idx; i < argc; ++i) exec_argv.push_back(argv[i]);
        exec_argv.push_back(nullptr);

        pid_t child = ::fork();
        if (child < 0) {
            std::cerr << "wrap: fork failed\n";
            return 1;
        }
        if (child == 0) {
            ::setenv("SPINEL_AGENT_CLIENT", agent.c_str(), 1);
            ::setenv("SPINEL_AGENT_SESSION_ID", sid.c_str(), 1);
            if (!task_id.empty()) ::setenv("SPINEL_TASK_ID", task_id.c_str(), 1);
            if (!repo.empty()) ::chdir(repo.c_str());
            ::execvp(exec_argv[0], exec_argv.data());
            std::perror("wrap execvp");
            ::_exit(127);
        }

        spinel::ai::AgentSessionInfo info;
        info.id = sid;
        info.agent = agent;
        info.title = title.empty() ? (agent + " · " + std::string(argv[cmd_idx])) : title;
        info.repo_path = repo;
        info.pid = child;
        info.task_id = task_id;
        info.in_sandbox = in_sandbox;
        info.status = spinel::ai::AgentSessionStatus::Running;
        info.summary = "运行中";
        info.source = "ctl";
        report_session_hybrid(info);

        int st = 0;
        while (::waitpid(child, &st, 0) < 0 && errno == EINTR) {}
        int exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : (WIFSIGNALED(st) ? (128 + WTERMSIG(st)) : 1);

        info.status = (exit_code == 0) ? spinel::ai::AgentSessionStatus::Done : spinel::ai::AgentSessionStatus::Error;
        info.summary = (exit_code == 0) ? "执行完成" : ("退出码 " + std::to_string(exit_code));
        info.updated_at = static_cast<long long>(std::time(nullptr));
        report_session_hybrid(info);
        return exit_code;
    }

    print_usage();
    return 1;
}
