#include "app_store.hpp"
#include "common.hpp"
#include "desktop_apps.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <filesystem>
#include <memory>
#include <sstream>
#include <thread>
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
    {"chrome", "Google Chrome", "浏览器", "flatpak", "com.google.Chrome",
     "Google 官方浏览器，支持账号同步、硬件视频解码与扩展生态", "flatpak run com.google.Chrome", "Flatpak 免密"},
    {"chromium", "Chromium", "浏览器", "pacman", "chromium",
     "Arch 官方仓库高速开源浏览器，国内镜像源直接拉取", "chromium --ozone-platform-hint=auto", "Pacman 原生"},
    {"firefox", "Firefox", "浏览器", "pacman", "firefox",
     "Mozilla 开源隐私浏览器，原生 Wayland 流畅滚动", "firefox", "Pacman 原生"},
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

int run_streaming_command(AppState *state,
                          const std::vector<std::string> &args,
                          const std::string &stdin_data = "") {
    if (args.empty()) return 1;
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
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execvp(args[0].c_str(), argv.data());
        _exit(127);
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
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
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

    if (state->store_thread.joinable()) {
        state->store_thread.join();
    }
    state->store_busy.store(true);

    state->store_thread = std::thread([state, action, app_id, backend, pkg, password]() {
        int rc = 1;
        if (backend == "flatpak") {
            if (action == "install") {
                emit_store_line(state, "==> 确保 Flathub 用户仓库已配置...");
                rc = run_streaming_command(state, {
                    "flatpak", "remote-add", "--user", "--if-not-exists",
                    "flathub", "https://dl.flathub.org/repo/flathub.flatpakrepo"
                });
                if (rc == 0) {
                    emit_store_line(state, "==> 正在从 Flathub 安装 " + pkg + " ...");
                    rc = run_streaming_command(state, {
                        "flatpak", "install", "--user", "-y", "--noninteractive", "flathub", pkg
                    });
                }
            } else {
                emit_store_line(state, "==> 正在卸载 Flatpak 应用 " + pkg + " ...");
                rc = run_streaming_command(state, {
                    "flatpak", "uninstall", "--user", "-y", "--noninteractive", pkg
                });
            }
        } else {
            bool nopass = check_sudo_nopasswd();
            if (!nopass && password.empty()) {
                emit_store_line(state, "[ERROR] 安装系统级 Pacman 软件需要当前用户管理员密码。");
                emit_store_done(state, 2, app_id, action);
                return;
            }
            std::vector<std::string> cmd = nopass
                ? std::vector<std::string>{"sudo", "-n", "pacman"}
                : std::vector<std::string>{"sudo", "-S", "-p", "", "pacman"};

            if (action == "install") {
                emit_store_line(state, "==> 正在同步数据库并安装 " + pkg + " ...");
                cmd.insert(cmd.end(), {"-Sy", "--noconfirm", "--needed", pkg});
            } else {
                emit_store_line(state, "==> 正在卸载软件包 " + pkg + " ...");
                cmd.insert(cmd.end(), {"-Rns", "--noconfirm", pkg});
            }
            rc = run_streaming_command(state, cmd, nopass ? "" : password);
        }

        emit_store_done(state, rc, app_id, action);
    });
}

void launch_store_app(AppState *state, const std::string &app_id) {
    (void)state;
    const StoreAppEntry *entry = find_catalog_entry(app_id);
    if (!entry) return;
    if (std::string(entry->backend) == "flatpak") {
        spawn_argv_logged({"flatpak", "run", entry->pkg}, "Store:" + app_id);
        return;
    }
    if (app_id == "chromium") {
        spawn_argv_logged({"chromium", "--ozone-platform-hint=auto"}, "Store:" + app_id);
    } else if (app_id == "obs") {
        spawn_argv_logged({"obs"}, "Store:" + app_id);
    } else {
        spawn_argv_logged({entry->pkg}, "Store:" + app_id);
    }
}
