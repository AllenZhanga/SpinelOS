#include "app_store.hpp"
#include "common.hpp"
#include "desktop_apps.hpp"
#include "steam_library.hpp"
#include "system_control.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct StoreAppEntry {
    const char *id;
    const char *name;
    const char *category;
    const char *backend;  // "pacman" | "flatpak"
    const char *pkg;
    const char *summary;
    const char *launch_cmd;
    const char *badge;
};

constexpr StoreAppEntry kCatalog[] = {
    {"steam", "Steam 客户端", "游戏平台", "pacman", "steam",
     "Valve 官方 PC 游戏与 Proton 平台（自动启用 multilib 与系统代理）", "steam", "一键安装"},
    {"chrome", "Google Chrome", "浏览器", "flatpak", "com.google.Chrome",
     "Google 官方浏览器（Flathub 海外源，国内直连较慢建议开启代理）", "flatpak run com.google.Chrome", "Flatpak · 需代理"},
    {"chromium", "Chromium", "浏览器", "pacman", "chromium",
     "官方软件源高速开源浏览器，国内镜像源极速直下", "chromium --ozone-platform-hint=auto", "Pacman 高速"},
    {"firefox", "Firefox", "浏览器", "pacman", "firefox",
     "Mozilla 开源隐私浏览器，国内镜像极速直下", "firefox", "Pacman 高速"},
    {"heroic", "Heroic Games Launcher", "游戏平台", "flatpak", "com.heroicgameslauncher.hgl",
     "Epic Games、GOG 与 Amazon Prime 游戏库跨平台启动器", "flatpak run com.heroicgameslauncher.hgl", "Flatpak 免密"},
    {"lutris", "Lutris", "游戏平台", "pacman", "lutris",
     "统一管理独立游戏、Wine 环境与主机模拟器", "lutris", "Pacman 原生"},
    {"protonup", "ProtonUp-Qt", "游戏工具", "flatpak", "net.davidotek.pupgui2",
     "图形化下载并管理 Steam Proton-GE / Wine-GE 兼容层", "flatpak run net.davidotek.pupgui2", "Flatpak 免密"},
    {"discord", "Discord", "社交语音", "pacman", "discord",
     "玩家组队语音开黑与游戏社区聊天客户端", "discord", "Pacman 原生"},
    {"obs", "OBS Studio", "直播录屏", "pacman", "obs-studio",
     "低开销游戏高帧率录屏与直播推流工作站", "obs", "Pacman 原生"},
    {"vlc", "VLC 媒体播放器", "影音播放", "pacman", "vlc",
     "全格式本地影片与音频解码播放器", "vlc", "Pacman 原生"},
    {"qbittorrent", "qBittorrent", "实用工具", "pacman", "qbittorrent",
     "轻量开源、无广告的 BT 与磁力高速下载器", "qbittorrent", "Pacman 原生"},
    {"gimp", "GIMP", "实用工具", "pacman", "gimp",
     "开源专业级图像编辑、截图处理与游戏贴图工具", "gimp", "Pacman 原生"}
};

bool is_valid_pacman_pkg(const std::string &pkg) {
    if (pkg.empty() || pkg.size() > 64) return false;
    for (char ch : pkg) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.' && c != '+') return false;
    }
    return true;
}

bool is_valid_flatpak_id(const std::string &id) {
    if (id.empty() || id.size() > 96 || id.find('.') == std::string::npos) return false;
    for (char ch : id) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.') return false;
    }
    return true;
}

bool is_pacman_pkg_installed(const std::string &pkg) {
    if (!is_valid_pacman_pkg(pkg)) return false;
    std::string out;
    int rc = 1;
    return run_argv_capture({"pacman", "-Qq", pkg}, out, rc) && rc == 0;
}

bool is_flatpak_app_installed(const std::string &app_id) {
    if (!is_valid_flatpak_id(app_id)) return false;
    std::error_code ec;
    fs::path user_dir = fs::path(home_dir()) / ".local/share/flatpak/app" / app_id;
    fs::path sys_dir = fs::path("/var/lib/flatpak/app") / app_id;
    return fs::is_directory(user_dir, ec) || fs::is_directory(sys_dir, ec);
}

bool has_flatpak_cli() {
    std::string out;
    int rc = 1;
    return run_argv_capture({"which", "flatpak"}, out, rc) && rc == 0;
}

bool check_sudo_nopasswd() {
    std::string out;
    int rc = 1;
    return run_argv_capture({"sudo", "-n", "true"}, out, rc) && rc == 0;
}

struct StoreIdleMsg {
    AppState *state;
    std::string line;
    bool is_done = false;
    int exit_code = 0;
    std::string target_id;
    std::string action;
};

gboolean on_store_idle(gpointer data) {
    std::unique_ptr<StoreIdleMsg> msg(static_cast<StoreIdleMsg *>(data));
    if (msg->state->shutting_down.load()) return G_SOURCE_REMOVE;

    if (msg->is_done) {
        msg->state->store_busy.store(false);
        push_desktop_apps(msg->state);
        push_store_catalog(msg->state);

        if (msg->state->store_view) {
            std::ostringstream js;
            js << "window.__spinelStoreDone && window.__spinelStoreDone("
               << (msg->exit_code == 0 ? "true" : "false") << ",\""
               << json_escape(msg->target_id) << "\",\""
               << json_escape(msg->action) << "\");";
            eval_js(msg->state->store_view, js.str());
        }
        return G_SOURCE_REMOVE;
    }

    if (msg->state->store_view) {
        std::ostringstream js;
        js << "window.__spinelStoreLog && window.__spinelStoreLog(\""
           << json_escape(msg->line) << "\");";
        eval_js(msg->state->store_view, js.str());
    }
    return G_SOURCE_REMOVE;
}

void emit_store_line(AppState *state, const std::string &line) {
    if (!state || state->shutting_down.load()) return;
    g_idle_add(on_store_idle, new StoreIdleMsg{state, line, false, 0, "", ""});
}

void emit_store_done(AppState *state, int exit_code, const std::string &target_id, const std::string &action) {
    if (!state || state->shutting_down.load()) return;
    g_idle_add(on_store_idle, new StoreIdleMsg{state, "", true, exit_code, target_id, action});
}

std::vector<std::pair<std::string, std::string>> build_proxy_env_pairs() {
    ProxyConfig pc = load_proxy_config();
    if (!pc.enabled || (pc.http_proxy.empty() && pc.socks_proxy.empty())) return {};
    std::string hv = !pc.http_proxy.empty() ? pc.http_proxy : pc.socks_proxy;
    std::string av = !pc.socks_proxy.empty() ? pc.socks_proxy : pc.http_proxy;
    return {
        {"http_proxy", hv},
        {"https_proxy", hv},
        {"all_proxy", av},
        {"no_proxy", pc.no_proxy}
    };
}

void append_proxy_env_prefix(std::vector<std::string> &cmd) {
    auto envs = build_proxy_env_pairs();
    if (envs.empty()) return;
    cmd.push_back("env");
    for (const auto &[k, v] : envs) {
        cmd.push_back(k + "=" + v);
    }
}

bool is_multilib_enabled() {
    std::ifstream in("/etc/pacman.conf");
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim_copy(line);
        if (t == "[multilib]") return true;
    }
    return false;
}

int run_streaming_command(AppState *state,
                          const std::vector<std::string> &args,
                          const std::string &stdin_data = "",
                          const std::vector<std::pair<std::string, std::string>> &env_overrides = {}) {
    if (args.empty()) return 1;
    if (state && state->store_cancel_requested.load()) return 130;

    int stdin_pipe[2]{-1, -1};
    int stdout_pipe[2]{-1, -1};
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        if (stdin_pipe[0] >= 0) { close(stdin_pipe[0]); close(stdin_pipe[1]); }
        if (stdout_pipe[0] >= 0) { close(stdout_pipe[0]); close(stdout_pipe[1]); }
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        return 1;
    }

    if (pid == 0) {
        setpgid(0, 0);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);

        for (const auto &[k, v] : env_overrides) {
            setenv(k.c_str(), v.c_str(), 1);
        }

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execvp(args[0].c_str(), argv.data());
        _exit(127);
    }

    setpgid(pid, pid);
    if (state) {
        state->store_child_pgid.store(pid);
        if (state->store_cancel_requested.load()) {
            kill(-pid, SIGTERM);
        }
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    if (!stdin_data.empty()) {
        std::string payload = stdin_data + "\n";
        size_t written = 0;
        while (written < payload.size()) {
            ssize_t n = write(stdin_pipe[1], payload.data() + written, payload.size() - written);
            if (n <= 0) break;
            written += static_cast<size_t>(n);
        }
    }
    close(stdin_pipe[1]);

    FILE *fp = fdopen(stdout_pipe[0], "r");
    if (fp) {
        std::array<char, 1024> buf{};
        while (fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr) {
            std::string line(buf.data());
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }
            if (!line.empty()) {
                emit_store_line(state, line);
            }
        }
        fclose(fp);
    } else {
        close(stdout_pipe[0]);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (state) {
        pid_t expected = pid;
        state->store_child_pgid.compare_exchange_strong(expected, -1);
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

const StoreAppEntry *find_catalog_entry(const std::string &id) {
    for (const auto &entry : kCatalog) {
        if (id == entry.id) return &entry;
    }
    return nullptr;
}

}  // namespace

void push_store_catalog(AppState *state) {
    if (!state || !state->store_view) return;
    bool flatpak_ok = has_flatpak_cli();
    bool sudo_nopass = check_sudo_nopasswd();

    std::ostringstream js;
    js << "window.__spinelStoreData && window.__spinelStoreData({"
       << "\"flatpak_ready\":" << (flatpak_ok ? "true" : "false") << ","
       << "\"sudo_nopass\":" << (sudo_nopass ? "true" : "false") << ","
       << "\"busy\":" << (state->store_busy.load() ? "true" : "false") << ","
       << "\"apps\":[";

    bool first = true;
    for (const auto &app : kCatalog) {
        if (!first) js << ",";
        first = false;
        bool installed = std::string(app.backend) == "flatpak"
            ? is_flatpak_app_installed(app.pkg)
            : is_pacman_pkg_installed(app.pkg);
        js << "{"
           << "\"id\":\"" << json_escape(app.id) << "\","
           << "\"name\":\"" << json_escape(app.name) << "\","
           << "\"category\":\"" << json_escape(app.category) << "\","
           << "\"backend\":\"" << json_escape(app.backend) << "\","
           << "\"pkg\":\"" << json_escape(app.pkg) << "\","
           << "\"summary\":\"" << json_escape(app.summary) << "\","
           << "\"badge\":\"" << json_escape(app.badge) << "\","
           << "\"installed\":" << (installed ? "true" : "false")
           << "}";
    }
    js << "]});";
    eval_js(state->store_view, js.str());
}

void start_store_action(AppState *state, const std::string &action, const std::string &payload) {
    if (!state || state->store_busy.load()) return;

    std::string app_id;
    std::string backend;
    std::string pkg;
    std::string password;

    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "id") app_id = v;
        else if (k == "backend") backend = v;
        else if (k == "pkg") pkg = v;
        else if (k == "password") password = v;
    }

    if (const StoreAppEntry *entry = find_catalog_entry(app_id)) {
        backend = entry->backend;
        pkg = entry->pkg;
    } else if (app_id.empty()) {
        app_id = pkg;
    }

    bool is_sys_maint = (action == "sys-update" || action == "mirror-test" || action == "keyring-fix");
    if (!is_sys_maint) {
        if (backend == "flatpak") {
            if (!is_valid_flatpak_id(pkg)) {
                emit_store_line(state, "[ERROR] 无效的 Flatpak 应用 ID: " + pkg);
                emit_store_done(state, 1, app_id, action);
                return;
            }
            if (!has_flatpak_cli()) {
                emit_store_line(state, "[ERROR] 系统尚未安装 flatpak。请先通过系统更新 (--full) 或在商店安装 flatpak 包。");
                emit_store_done(state, 1, app_id, action);
                return;
            }
        } else {
            backend = "pacman";
            if (!is_valid_pacman_pkg(pkg)) {
                emit_store_line(state, "[ERROR] 无效的 Pacman 软件包名: " + pkg);
                emit_store_done(state, 1, app_id, action);
                return;
            }
        }
    }

    if (state->store_thread.joinable()) {
        state->store_thread.join();
    }
    state->store_cancel_requested.store(false);
    state->store_child_pgid.store(-1);
    state->store_busy.store(true);

    state->store_thread = std::thread([state, action, app_id, backend, pkg, password, is_sys_maint]() {
        int rc = 1;
        auto proxy_envs = build_proxy_env_pairs();
        auto make_sudo_cmd = [](bool nopass) {
            return nopass ? std::vector<std::string>{"sudo", "-n"}
                          : std::vector<std::string>{"sudo", "-S", "-p", ""};
        };
        auto ensure_multilib = [&](const std::vector<std::string> &base_sudo, const std::string &pwd_in) {
            if (is_multilib_enabled()) return true;
            emit_store_line(state, "==> 检测到 [multilib] 尚未开启，正在自动启用 32 位游戏软件源...");
            std::vector<std::string> mcmd = base_sudo;
            mcmd.insert(mcmd.end(), {
                "sh", "-c",
                "if grep -q '^#\\[multilib\\]' /etc/pacman.conf; then "
                "sed -i '/^#\\[multilib\\]/{s/^#//;n;s/^#//}' /etc/pacman.conf; "
                "elif ! grep -q '^\\[multilib\\]' /etc/pacman.conf; then "
                "printf '\\n[multilib]\\nInclude = /etc/pacman.d/mirrorlist\\n' >> /etc/pacman.conf; fi"
            });
            return run_streaming_command(state, mcmd, pwd_in) == 0;
        };

        if (is_sys_maint) {
            bool nopass = check_sudo_nopasswd();
            if (!nopass && password.empty()) {
                emit_store_line(state, "[ERROR] 系统维护操作需要当前用户管理员密码。");
                emit_store_done(state, 2, app_id, action);
                return;
            }
            std::vector<std::string> base_sudo = make_sudo_cmd(nopass);
            std::string pwd_in = nopass ? "" : password;

            if (action == "sys-update") {
                ensure_multilib(base_sudo, pwd_in);
                std::vector<std::string> cmd = base_sudo;
                append_proxy_env_prefix(cmd);
                cmd.insert(cmd.end(), {"pacman", "-Syu", "--noconfirm"});
                emit_store_line(state, "==> 正在全面更新 SpinelOS 系统软件包...");
                rc = run_streaming_command(state, cmd, pwd_in);
                if (rc == 0 && !state->store_cancel_requested.load() && has_flatpak_cli()) {
                    emit_store_line(state, "==> 正在同步更新 Flatpak 应用...");
                    run_streaming_command(state, {"flatpak", "update", "-y", "--noninteractive"}, "", proxy_envs);
                }
            } else if (action == "mirror-test") {
                if (!has_executable("reflector")) {
                    emit_store_line(state, "[ERROR] 未检测到 reflector 工具，请先安装 reflector。");
                    rc = 1;
                } else {
                    std::vector<std::string> cmd = base_sudo;
                    cmd.insert(cmd.end(), {
                        "reflector", "--country", "China", "--protocol", "https",
                        "--latest", "8", "--sort", "rate", "--save", "/etc/pacman.d/mirrorlist"
                    });
                    emit_store_line(state, "==> 正在测速中国区 HTTPS 镜像站（约需 10~20 秒）...");
                    rc = run_streaming_command(state, cmd, pwd_in);
                    if (rc == 0 && !state->store_cancel_requested.load()) {
                        std::vector<std::string> sync_cmd = base_sudo;
                        sync_cmd.insert(sync_cmd.end(), {"pacman", "-Syy", "--noconfirm"});
                        emit_store_line(state, "==> 测速完成，正在刷新软件包索引...");
                        rc = run_streaming_command(state, sync_cmd, pwd_in);
                    }
                }
            } else if (action == "keyring-fix") {
                std::vector<std::string> cmd1 = base_sudo;
                cmd1.insert(cmd1.end(), {"pacman", "-Sy", "--noconfirm", "archlinux-keyring"});
                emit_store_line(state, "==> 正在更新系统签名密钥环...");
                rc = run_streaming_command(state, cmd1, pwd_in);
                if (rc == 0 && !state->store_cancel_requested.load()) {
                    std::vector<std::string> cmd2 = base_sudo;
                    cmd2.insert(cmd2.end(), {"pacman-key", "--populate", "archlinux"});
                    emit_store_line(state, "==> 正在重新校验并信任密钥环...");
                    rc = run_streaming_command(state, cmd2, pwd_in);
                }
            }
        } else if (backend == "flatpak") {
            if (action == "install") {
                if (proxy_envs.empty()) {
                    emit_store_line(state, "[提示] Flathub 为海外软件源，国内直连较慢；可随时点击「停止下载」，并在「系统设置 → 网络」开启代理或优先安装 Chromium / Firefox。");
                }
                emit_store_line(state, "==> 确保 Flathub 用户仓库已配置...");
                rc = run_streaming_command(state, {
                    "flatpak", "remote-add", "--user", "--if-not-exists",
                    "flathub", "https://dl.flathub.org/repo/flathub.flatpakrepo"
                }, "", proxy_envs);
                if (rc == 0 && !state->store_cancel_requested.load()) {
                    emit_store_line(state, "==> 正在从 Flathub 安装 " + pkg + " ...");
                    rc = run_streaming_command(state, {
                        "flatpak", "install", "--user", "-y", "--noninteractive", "flathub", pkg
                    }, "", proxy_envs);
                }
            } else {
                emit_store_line(state, "==> 正在卸载 Flatpak 应用 " + pkg + " ...");
                rc = run_streaming_command(state, {
                    "flatpak", "uninstall", "--user", "-y", "--noninteractive", pkg
                }, "", proxy_envs);
            }
        } else {
            bool nopass = check_sudo_nopasswd();
            if (!nopass && password.empty()) {
                emit_store_line(state, "[ERROR] 安装系统级 Pacman 软件需要当前用户管理员密码。");
                emit_store_done(state, 2, app_id, action);
                return;
            }
            std::vector<std::string> base_sudo = make_sudo_cmd(nopass);
            std::string pwd_in = nopass ? "" : password;
            if (action == "install" && pkg == "steam") {
                ensure_multilib(base_sudo, pwd_in);
            }
            std::vector<std::string> cmd = base_sudo;
            append_proxy_env_prefix(cmd);
            cmd.push_back("pacman");

            if (action == "install") {
                emit_store_line(state, "==> 正在同步数据库并安装 " + pkg + " ...");
                cmd.insert(cmd.end(), {"-Sy", "--noconfirm", "--needed", pkg});
            } else {
                emit_store_line(state, "==> 正在卸载软件包 " + pkg + " ...");
                cmd.insert(cmd.end(), {"-Rns", "--noconfirm", pkg});
            }
            rc = run_streaming_command(state, cmd, pwd_in);
        }

        if (state->store_cancel_requested.load()) {
            emit_store_line(state, "[已终止] 用户已取消当前操作。");
            emit_store_done(state, 130, app_id, "cancel");
            return;
        }
        emit_store_done(state, rc, app_id, action);
    });
}

void cancel_store_action(AppState *state) {
    if (!state || !state->store_busy.load()) return;
    state->store_cancel_requested.store(true);
    pid_t pgid = state->store_child_pgid.load();
    if (pgid <= 1) {
        emit_store_line(state, "[已取消] 已请求中止当前任务。");
        return;
    }

    emit_store_line(state, "[已取消] 正在终止下载/安装进程...");
    if (kill(-pgid, SIGTERM) != 0 && errno == EPERM && check_sudo_nopasswd()) {
        std::string out;
        int rc = 1;
        run_argv_capture({"sudo", "-n", "kill", "-TERM", "--", "-" + std::to_string(pgid)}, out, rc);
    }

    std::thread([state, pgid]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        if (!state->shutting_down.load() && state->store_child_pgid.load() == pgid) {
            if (kill(-pgid, SIGKILL) != 0 && errno == EPERM && check_sudo_nopasswd()) {
                std::string out;
                int rc = 1;
                run_argv_capture({"sudo", "-n", "kill", "-KILL", "--", "-" + std::to_string(pgid)}, out, rc);
            }
        }
    }).detach();
}

void launch_store_app(AppState *state, const std::string &app_id) {
    (void)state;
    const StoreAppEntry *entry = find_catalog_entry(app_id);
    if (!entry) return;
    if (std::string(entry->backend) == "flatpak") {
        spawn_argv_logged({"flatpak", "run", entry->pkg}, "Store:" + app_id);
        return;
    }
    if (app_id == "steam") {
        launch_steam_client(state);
    } else if (app_id == "chromium") {
        spawn_argv_logged({"chromium", "--ozone-platform-hint=auto", "--enable-wayland-ime"}, "Store:" + app_id);
    } else if (app_id == "obs") {
        spawn_argv_logged({"obs"}, "Store:" + app_id);
    } else {
        spawn_argv_logged({entry->pkg}, "Store:" + app_id);
    }
}
