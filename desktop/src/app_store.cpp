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
     "Valve 官方 PC 游戏与 Proton 平台（自动启用 multilib 与系统代理）", "steam", "Pacman 原生"},
    {"chrome", "Google Chrome", "浏览器", "native", "google-chrome-stable",
     "Google 官方浏览器（宿主机原生安装，直接调用主机显卡驱动）", "google-chrome-stable", "原生安装"},
    {"chromium", "Chromium", "浏览器", "pacman", "chromium",
     "官方软件源高速开源浏览器，国内镜像源极速直下", "chromium --ozone-platform-hint=auto", "Pacman 原生"},
    {"firefox", "Firefox", "浏览器", "pacman", "firefox",
     "Mozilla 开源隐私浏览器，国内镜像极速直下", "firefox", "Pacman 原生"},
    {"heroic", "Heroic Games Launcher", "游戏平台", "native", "heroic",
     "Epic Games、GOG 与 Amazon Prime 游戏库（官方 Arch 原生包）", "heroic", "原生安装"},
    {"lutris", "Lutris", "游戏平台", "pacman", "lutris",
     "统一管理独立游戏、Wine 环境与主机模拟器", "lutris", "Pacman 原生"},
    {"protonup", "ProtonUp-Qt", "游戏工具", "native", "protonup-qt",
     "图形化下载并管理 Steam Proton-GE / Wine-GE（宿主机原生版）", "protonup-qt", "原生安装"},
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

bool is_native_app_installed(const std::string &app_id) {
    std::error_code ec;
    if (app_id == "chrome") {
        return has_executable("google-chrome-stable") ||
               fs::exists("/opt/google/chrome/google-chrome", ec);
    }
    if (app_id == "heroic") {
        return is_pacman_pkg_installed("heroic") ||
               is_pacman_pkg_installed("heroic-games-launcher-bin") ||
               has_executable("heroic");
    }
    if (app_id == "protonup") {
        return has_executable("protonup-qt") ||
               fs::exists("/opt/protonup-qt/AppRun", ec);
    }
    return false;
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
    return build_effective_proxy_envs();
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
        bool installed = false;
        std::string bkend(app.backend);
        if (bkend == "flatpak") {
            installed = is_flatpak_app_installed(app.pkg);
        } else if (bkend == "native") {
            installed = is_native_app_installed(app.id);
        } else {
            installed = is_pacman_pkg_installed(app.pkg);
        }
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
        } else if (backend == "native") {
            if (app_id != "chrome" && app_id != "heroic" && app_id != "protonup") {
                emit_store_line(state, "[ERROR] 不支持的原生应用 ID: " + app_id);
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
        } else if (backend == "native") {
            bool nopass = check_sudo_nopasswd();
            if (!nopass && password.empty()) {
                emit_store_line(state, "[ERROR] 原生安装/卸载应用需要当前用户管理员密码。");
                emit_store_done(state, 2, app_id, action);
                return;
            }
            std::vector<std::string> base_sudo = make_sudo_cmd(nopass);
            std::string pwd_in = nopass ? "" : password;
            std::vector<std::string> cmd = base_sudo;
            append_proxy_env_prefix(cmd);

            if (app_id == "chrome") {
                if (action == "install") {
                    emit_store_line(state, "==> 正在确保 Google Chrome 宿主机运行依赖已就绪...");
                    emit_store_line(state, "==> 将直接下载 Google 官方稳定版并原生安装到 /opt/google/chrome（不经过 Flatpak）...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; "
                        "STG=$(mktemp -d /tmp/spinel-chrome-XXXXXX); "
                        "trap 'rm -rf \"$STG\"' EXIT; "
                        "pacman -Sy --noconfirm --needed alsa-lib gtk3 libcups libdrm libxkbcommon nss xdg-utils ttf-liberation curl; "
                        "echo '==> 正在下载 google-chrome-stable_current_amd64.deb ...'; "
                        "curl -fL --connect-timeout 15 --retry 2 -o \"$STG/chrome.deb\" 'https://dl.google.com/linux/direct/google-chrome-stable_current_amd64.deb'; "
                        "echo '==> 正在解包并安装到宿主机系统...'; "
                        "bsdtar -xf \"$STG/chrome.deb\" -C \"$STG\"; "
                        "mkdir -p \"$STG/root\"; "
                        "bsdtar -xf \"$STG\"/data.tar.* -C \"$STG/root\"; "
                        "rm -rf /opt/google/chrome; "
                        "mkdir -p /opt/google; "
                        "cp -a \"$STG/root/opt/google/chrome\" /opt/google/chrome; "
                        "[ -f /opt/google/chrome/chrome-sandbox ] && chmod 4755 /opt/google/chrome/chrome-sandbox || true; "
                        "ln -sf /opt/google/chrome/google-chrome /usr/bin/google-chrome-stable; "
                        "ln -sf /opt/google/chrome/google-chrome /usr/bin/google-chrome; "
                        "install -Dm644 \"$STG/root/usr/share/applications/google-chrome.desktop\" /usr/share/applications/google-chrome.desktop; "
                        "for sz in 16 24 32 48 64 128 256; do "
                        "  [ -f \"/opt/google/chrome/product_logo_${sz}.png\" ] && install -Dm644 \"/opt/google/chrome/product_logo_${sz}.png\" \"/usr/share/icons/hicolor/${sz}x${sz}/apps/google-chrome.png\"; "
                        "done; "
                        "update-desktop-database /usr/share/applications >/dev/null 2>&1 || true; "
                        "gtk-update-icon-cache -f /usr/share/icons/hicolor >/dev/null 2>&1 || true; "
                        "echo '==> Google Chrome 宿主机原生安装完成！'"
                    });
                } else {
                    emit_store_line(state, "==> 正在卸载宿主机原生 Google Chrome ...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; "
                        "rm -rf /opt/google/chrome /usr/bin/google-chrome-stable /usr/bin/google-chrome /usr/share/applications/google-chrome.desktop; "
                        "rm -f /usr/share/icons/hicolor/*/apps/google-chrome.png; "
                        "update-desktop-database /usr/share/applications >/dev/null 2>&1 || true; "
                        "gtk-update-icon-cache -f /usr/share/icons/hicolor >/dev/null 2>&1 || true; "
                        "echo '==> 已卸载 Google Chrome。'"
                    });
                }
            } else if (app_id == "heroic") {
                if (action == "install") {
                    emit_store_line(state, "==> 正在查询 Heroic Games Launcher 最新官方 Arch (.pacman) 原生包...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; "
                        "STG=$(mktemp -d /tmp/spinel-heroic-XXXXXX); "
                        "trap 'rm -rf \"$STG\"' EXIT; "
                        "URL=$(curl -fsSL --connect-timeout 15 'https://api.github.com/repos/Heroic-Games-Launcher/HeroicGamesLauncher/releases/latest' | grep -m1 -oE 'https://[^\"]+-linux-x64\\.pacman'); "
                        "[ -n \"$URL\" ] || { echo '[ERROR] 未获取到 Heroic 官方 .pacman 下载地址'; exit 1; }; "
                        "echo \"==> 正在下载 $URL ...\"; "
                        "curl -fL --connect-timeout 15 --retry 2 -o \"$STG/heroic.pacman\" \"$URL\"; "
                        "echo '==> 正在通过 Pacman 原生安装 Heroic ...'; "
                        "pacman -Sy --noconfirm --needed >/dev/null 2>&1 || true; "
                        "pacman -U --noconfirm \"$STG/heroic.pacman\"; "
                        "echo '==> Heroic Games Launcher 原生安装完成！'"
                    });
                } else {
                    emit_store_line(state, "==> 正在卸载原生 Heroic Games Launcher ...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; "
                        "PKG=$(pacman -Qq heroic 2>/dev/null || pacman -Qq heroic-games-launcher-bin 2>/dev/null || pacman -Qqo /usr/bin/heroic 2>/dev/null || true); "
                        "[ -n \"$PKG\" ] && pacman -Rns --noconfirm \"$PKG\" || echo '==> 未检测到 Pacman 中的 Heroic 包。'"
                    });
                }
            } else if (app_id == "protonup") {
                if (action == "install") {
                    emit_store_line(state, "==> 正在查询 ProtonUp-Qt 最新官方版本（宿主机原生解包安装）...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; "
                        "STG=$(mktemp -d /tmp/spinel-protonup-XXXXXX); "
                        "trap 'rm -rf \"$STG\"' EXIT; "
                        "URL=$(curl -fsSL --connect-timeout 15 'https://api.github.com/repos/DavidoTek/ProtonUp-Qt/releases/latest' | grep -m1 -oE 'https://[^\"]+-x86_64\\.AppImage'); "
                        "[ -n \"$URL\" ] || { echo '[ERROR] 未获取到 ProtonUp-Qt 官方下载地址'; exit 1; }; "
                        "echo \"==> 正在下载 $URL ...\"; "
                        "curl -fL --connect-timeout 15 --retry 2 -o \"$STG/ProtonUp-Qt.AppImage\" \"$URL\"; "
                        "chmod +x \"$STG/ProtonUp-Qt.AppImage\"; "
                        "echo '==> 正在解包安装到 /opt/protonup-qt ...'; "
                        "(cd \"$STG\" && ./ProtonUp-Qt.AppImage --appimage-extract >/dev/null); "
                        "rm -rf /opt/protonup-qt; "
                        "mv \"$STG/squashfs-root\" /opt/protonup-qt; "
                        "printf '#!/bin/sh\\nexec /opt/protonup-qt/AppRun \"$@\"\\n' > /usr/bin/protonup-qt; "
                        "chmod 755 /usr/bin/protonup-qt; "
                        "ICON_SRC=$(find /opt/protonup-qt -maxdepth 3 \\( -name '*pupgui2*.png' -o -name '.DirIcon' \\) | head -n1 || true); "
                        "[ -n \"$ICON_SRC\" ] && install -Dm644 \"$ICON_SRC\" /usr/share/icons/hicolor/256x256/apps/protonup-qt.png || true; "
                        "printf '[Desktop Entry]\\nType=Application\\nName=ProtonUp-Qt\\nComment=Install Wine-GE and Proton-GE for Steam and Lutris\\nExec=/usr/bin/protonup-qt\\nIcon=protonup-qt\\nCategories=Game;Utility;\\nTerminal=false\\n' > /usr/share/applications/protonup-qt.desktop; "
                        "update-desktop-database /usr/share/applications >/dev/null 2>&1 || true; "
                        "gtk-update-icon-cache -f /usr/share/icons/hicolor >/dev/null 2>&1 || true; "
                        "echo '==> ProtonUp-Qt 原生安装完成！'"
                    });
                } else {
                    emit_store_line(state, "==> 正在卸载宿主机原生 ProtonUp-Qt ...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; "
                        "rm -rf /opt/protonup-qt /usr/bin/protonup-qt /usr/share/applications/protonup-qt.desktop /usr/share/icons/hicolor/*/apps/protonup-qt.*; "
                        "update-desktop-database /usr/share/applications >/dev/null 2>&1 || true; "
                        "gtk-update-icon-cache -f /usr/share/icons/hicolor >/dev/null 2>&1 || true; "
                        "echo '==> 已卸载 ProtonUp-Qt。'"
                    });
                }
            }
            rc = run_streaming_command(state, cmd, pwd_in);
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
    } else if (app_id == "chromium" || app_id == "firefox" || app_id == "chrome") {
        launch_browser_command({entry->pkg}, "Store:" + app_id);
    } else if (app_id == "heroic") {
        spawn_argv_logged({has_executable("heroic") ? "heroic" : "/opt/Heroic/heroic"}, "Store:heroic");
    } else if (app_id == "obs") {
        spawn_argv_logged({"obs"}, "Store:" + app_id);
    } else {
        spawn_argv_logged({entry->pkg}, "Store:" + app_id);
    }
}
