#include "installer.hpp"
#include "common.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct IdleMessage {
    AppState *state;
    std::string line;
    int exit_code = 0;
    bool is_done = false;
};

struct InstallConfig {
    std::string mode = "disk";
    std::string disk;
    std::string root_part;
    std::string efi_part;
    std::string username;
    std::string hostname;
    std::string timezone;
    std::string autologin = "true";
    std::string os_prober = "false";
    std::string password;
};

gboolean on_installer_idle(gpointer data) {
    std::unique_ptr<IdleMessage> msg(static_cast<IdleMessage *>(data));
    if (msg->state->shutting_down.load() || !msg->state->installer_view) {
        return G_SOURCE_REMOVE;
    }

    if (msg->is_done) {
        msg->state->install_running.store(false);
        std::ostringstream js;
        js << "window.__spinelInstallerDone && window.__spinelInstallerDone(" << msg->exit_code << ");";
        eval_js(msg->state->installer_view, js.str());
        return G_SOURCE_REMOVE;
    }

    const std::string &line = msg->line;
    constexpr const char *kProgressPrefix = "@@PROGRESS:";
    if (line.rfind(kProgressPrefix, 0) == 0) {
        std::string rest = line.substr(std::strlen(kProgressPrefix));
        auto colon = rest.find(':');
        if (colon != std::string::npos) {
            int pct = std::atoi(rest.substr(0, colon).c_str());
            std::string label = rest.substr(colon + 1);
            std::ostringstream js;
            js << "window.__spinelInstallerProgress && window.__spinelInstallerProgress("
               << pct << ",\"" << json_escape(label) << "\");";
            eval_js(msg->state->installer_view, js.str());
            return G_SOURCE_REMOVE;
        }
    }

    std::ostringstream js;
    js << "window.__spinelInstallerLog && window.__spinelInstallerLog(\"" << json_escape(line) << "\");";
    eval_js(msg->state->installer_view, js.str());
    return G_SOURCE_REMOVE;
}

void emit_installer_error(AppState *state, const std::string &err) {
    g_idle_add(on_installer_idle, new IdleMessage{state, "[ERROR] " + err, 0, false});
    g_idle_add(on_installer_idle, new IdleMessage{state, "", 1, true});
}

bool is_valid_disk(const std::string &s) {
    if (s.rfind("/dev/", 0) != 0 || s.size() <= 5 || s.find("..") != std::string::npos) return false;
    for (size_t i = 5; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (!std::isalnum(c) && c != '_' && c != '-' && c != '/') return false;
    }
    return true;
}

bool is_valid_username(const std::string &s) {
    if (s.empty() || s.size() > 31) return false;
    unsigned char first = static_cast<unsigned char>(s[0]);
    if (!(std::islower(first) || first == '_')) return false;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::islower(c) && !std::isdigit(c) && c != '_' && c != '-') return false;
    }
    return true;
}

bool is_valid_hostname(const std::string &s) {
    if (s.empty() || s.size() > 63) return false;
    if (!std::isalnum(static_cast<unsigned char>(s[0]))) return false;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && c != '-') return false;
    }
    return true;
}

bool is_valid_timezone(const std::string &s) {
    if (s.empty() || s.size() > 64 || s.find("..") != std::string::npos) return false;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && c != '/' && c != '_' && c != '-' && c != '+') return false;
    }
    return true;
}

InstallConfig parse_install_payload(const std::string &payload) {
    InstallConfig cfg;
    auto lines = split_lines(payload);
    for (size_t i = 1; i < lines.size(); ++i) {
        const auto &line = lines[i];
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "mode") cfg.mode = (val == "partition" ? "partition" : "disk");
        else if (key == "disk") cfg.disk = val;
        else if (key == "root_part") cfg.root_part = val;
        else if (key == "efi_part") cfg.efi_part = val;
        else if (key == "username") cfg.username = val;
        else if (key == "hostname") cfg.hostname = val;
        else if (key == "timezone") cfg.timezone = val;
        else if (key == "autologin") cfg.autologin = (val == "true" ? "true" : "false");
        else if (key == "os_prober") cfg.os_prober = (val == "true" ? "true" : "false");
        else if (key == "password") cfg.password = val;
    }
    return cfg;
}

}  // namespace

void push_installer_env(AppState *state) {
    if (!state || !state->installer_view) return;
    bool is_live = is_live_iso_env();
    bool is_uefi = fs::exists("/sys/firmware/efi/efivars");
    std::string boot_disk = run_cmd_capture(
        "src=$(findmnt -n -o SOURCE /run/archiso/bootmnt 2>/dev/null || true); "
        "[ -n \"$src\" ] && lsblk -no PKNAME \"$src\" 2>/dev/null | head -n1 || true"
    );
    std::string lsblk_json = run_cmd_capture(
        "lsblk -J -b -o NAME,PATH,SIZE,MODEL,TRAN,RM,TYPE,FSTYPE,PARTTYPENAME,MOUNTPOINT 2>/dev/null"
    );
    if (lsblk_json.empty() || lsblk_json.front() != '{') {
        lsblk_json = "null";
    }

    std::ostringstream js;
    js << "window.__spinelInstallerEnv && window.__spinelInstallerEnv({"
       << "\"is_live\":" << (is_live ? "true" : "false") << ","
       << "\"uefi\":" << (is_uefi ? "true" : "false") << ","
       << "\"boot_disk\":\"" << json_escape(boot_disk) << "\","
       << "\"installing\":" << (state->install_running.load() ? "true" : "false") << ","
       << "\"lsblk\":" << lsblk_json
       << "});";
    eval_js(state->installer_view, js.str());
}

void start_disk_install(AppState *state, const std::string &payload) {
    if (state->install_running.exchange(true)) {
        g_idle_add(on_installer_idle, new IdleMessage{state, "[WARN] 安装任务正在进行中，请勿重复启动。", 0, false});
        return;
    }

    if (state->install_thread.joinable()) {
        state->install_thread.join();
    }

    InstallConfig cfg = parse_install_payload(payload);
    if (cfg.mode == "disk") {
        if (!is_valid_disk(cfg.disk)) {
            emit_installer_error(state, "无效的目标磁盘路径: " + cfg.disk);
            return;
        }
    } else {
        if (!is_valid_disk(cfg.root_part)) {
            emit_installer_error(state, "无效的目标根分区路径: " + cfg.root_part);
            return;
        }
        if (fs::exists("/sys/firmware/efi/efivars") && !is_valid_disk(cfg.efi_part)) {
            emit_installer_error(state, "UEFI 多分区安装需要选择有效的 FAT32 EFI 分区。");
            return;
        }
    }

    if (!is_valid_username(cfg.username)) {
        emit_installer_error(state, "用户名只能包含小写字母、数字、下划线或连字符。");
        return;
    }
    if (!is_valid_hostname(cfg.hostname)) {
        emit_installer_error(state, "主机名格式无效（仅支持字母、数字和连字符）。");
        return;
    }
    if (!is_valid_timezone(cfg.timezone)) {
        emit_installer_error(state, "时区格式无效: " + cfg.timezone);
        return;
    }
    if (cfg.password.size() < 4) {
        emit_installer_error(state, "密码长度至少需要 4 位。");
        return;
    }

    int stdin_pipe[2]{-1, -1};
    int stdout_pipe[2]{-1, -1};
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        if (stdin_pipe[0] >= 0) { close(stdin_pipe[0]); close(stdin_pipe[1]); }
        if (stdout_pipe[0] >= 0) { close(stdout_pipe[0]); close(stdout_pipe[1]); }
        emit_installer_error(state, "无法创建安装进程管道。");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        emit_installer_error(state, "fork() 创建安装进程失败。");
        return;
    }

    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);

        std::vector<std::string> args = {
            "sudo", "-n", "/usr/local/bin/spinel-install",
            "--mode", cfg.mode,
            "--username", cfg.username,
            "--hostname", cfg.hostname,
            "--timezone", cfg.timezone,
            "--autologin", cfg.autologin,
            "--os-prober", cfg.os_prober
        };
        if (cfg.mode == "disk") {
            args.push_back("--disk");
            args.push_back(cfg.disk);
        } else {
            args.push_back("--root-part");
            args.push_back(cfg.root_part);
            if (!cfg.efi_part.empty()) {
                args.push_back("--efi-part");
                args.push_back(cfg.efi_part);
            }
        }

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);

        execvp("sudo", argv.data());
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    state->install_pid.store(pid);

    std::string pass_line = cfg.password + "\n";
    size_t written = 0;
    while (written < pass_line.size()) {
        ssize_t n = write(stdin_pipe[1], pass_line.data() + written, pass_line.size() - written);
        if (n <= 0) break;
        written += static_cast<size_t>(n);
    }
    close(stdin_pipe[1]);

    int read_fd = stdout_pipe[0];
    state->install_thread = std::thread([state, pid, read_fd]() {
        FILE *fp = fdopen(read_fd, "r");
        if (fp) {
            std::array<char, 1024> buf{};
            while (fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr) {
                std::string line(buf.data());
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                    line.pop_back();
                }
                if (!line.empty()) {
                    g_idle_add(on_installer_idle, new IdleMessage{state, std::move(line), 0, false});
                }
            }
            fclose(fp);
        } else {
            close(read_fd);
        }

        int status = 0;
        waitpid(pid, &status, 0);
        state->install_pid.store(-1);
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        g_idle_add(on_installer_idle, new IdleMessage{state, "", code, true});
    });
}
