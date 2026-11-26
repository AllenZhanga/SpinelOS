#include "sandbox_manager.hpp"

#include "common.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

void notify_shell_sandbox(const std::string &arg) {
    if (has_executable("gdbus")) {
        std::string out;
        int rc = 1;
        std::string param = "['" + arg + "']";
        run_argv_capture({
            "gdbus", "call", "--session",
            "--dest", "org.spinelos.shell",
            "--object-path", "/org/spinelos/shell",
            "--method", "org.gtk.Actions.Activate",
            "sync-sandbox", param, "{}"
        }, out, rc);
    }
}

void print_usage() {
    std::cout
        << "SpinelOS Agent 平行沙箱管理工具 (spinel-sandbox)\n\n"
        << "用法:\n"
        << "  spinel-sandbox start                         启动平行沙箱桌面\n"
        << "  spinel-sandbox stop                          停止平行沙箱会话\n"
        << "  spinel-sandbox reset                         停止并清空沙箱临时主目录\n"
        << "  spinel-sandbox status [--json]               查看沙箱状态与配置\n"
        << "  spinel-sandbox run [--workspace <dir>] <cmd...> 在平行沙箱中运行程序\n"
        << "  spinel-sandbox pip-show | pip-hide | pip-toggle 控制桌面画中画悬浮窗\n";
}

}  // namespace

int main(int argc, char **argv) {
    ensure_display_env();
    if (argc < 2) {
        print_usage();
        return 0;
    }

    std::string sub = argv[1];
    if (sub == "--help" || sub == "-h" || sub == "help") {
        print_usage();
        return 0;
    }
    if (sub == "--clip-sync") {
        return run_sandbox_clipboard_daemon();
    }
    if (sub == "--clip-push") {
        if (argc < 3) return 1;
        return handle_sandbox_clipboard_push(argv[2]);
    }

    std::string err;
    if (sub == "start") {
        if (!start_sandbox_session(err)) {
            std::cerr << "spinel-sandbox: " << err << "\n";
            return 1;
        }
        notify_shell_sandbox("show");
        return 0;
    }
    if (sub == "stop") {
        stop_sandbox_session(err);
        notify_shell_sandbox("sync");
        return 0;
    }
    if (sub == "reset") {
        if (!reset_sandbox_home(err)) {
            std::cerr << "spinel-sandbox: " << err << "\n";
            return 1;
        }
        notify_shell_sandbox("sync");
        return 0;
    }
    if (sub == "status" || sub == "info") {
        bool want_json = (argc >= 3 && std::string(argv[2]) == "--json");
        if (want_json) {
            std::cout << sandbox_state_json() << "\n";
        } else {
            SandboxStatus st = query_sandbox_status();
            SandboxConfig cfg = load_sandbox_config();
            std::cout << "状态: " << (st.running ? "运行中" : "已停止") << "\n"
                      << "Wayland Socket: " << st.runtime_dir << "/" << st.wayland_display << "\n"
                      << "沙箱主目录: " << st.home_path << " (" << st.home_size_mb << " MB)\n"
                      << "共享宿主网络: " << (cfg.allow_network ? "开启" : "关闭") << "\n"
                      << "剪贴板同步: " << (cfg.sync_clipboard ? "开启" : "关闭") << "\n"
                      << "算力配额: " << cfg.perf_mode << "\n"
                      << "可写授权目录 (" << cfg.rw_dirs.size() << "):\n";
            for (const auto &d : cfg.rw_dirs) std::cout << "  - " << d << "\n";
        }
        return 0;
    }
    if (sub == "pip-show") {
        if (start_sandbox_session(err)) notify_shell_sandbox("show");
        else {
            std::cerr << "spinel-sandbox: " << err << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "pip-hide") {
        notify_shell_sandbox("hide");
        return 0;
    }
    if (sub == "pip-toggle") {
        SandboxStatus st = query_sandbox_status();
        if (!st.running) {
            if (!start_sandbox_session(err)) {
                std::cerr << "spinel-sandbox: " << err << "\n";
                return 1;
            }
            notify_shell_sandbox("show");
        } else {
            notify_shell_sandbox("toggle");
        }
        return 0;
    }
    if (sub == "run") {
        std::string workspace;
        std::vector<std::string> cmd;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (cmd.empty() && a == "--workspace" && i + 1 < argc) {
                workspace = argv[++i];
            } else if (cmd.empty() && a == "--") {
                for (int j = i + 1; j < argc; ++j) cmd.emplace_back(argv[j]);
                break;
            } else {
                cmd.push_back(a);
            }
        }
        if (cmd.empty()) {
            std::cerr << "spinel-sandbox run: 请提供要运行的命令\n";
            return 1;
        }
        if (!launch_in_sandbox(cmd, workspace, err)) {
            std::cerr << "spinel-sandbox: " << err << "\n";
            return 1;
        }
        notify_shell_sandbox("show");
        return 0;
    }

    print_usage();
    return 1;
}
