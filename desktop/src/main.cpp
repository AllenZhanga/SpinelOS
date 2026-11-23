#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <webkit/webkit.h>

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct AppState {
    WebKitWebView *bar_view = nullptr;
    WebKitWebView *hub_view = nullptr;
    GtkWindow *hub_window = nullptr;
    bool hub_visible = true;
    std::atomic<bool> install_running{false};
    std::atomic<bool> shutting_down{false};
    std::atomic<pid_t> install_pid{-1};
    std::thread install_thread;
};

struct IdleMessage {
    AppState *state;
    std::string line;
    int exit_code = 0;
    bool is_done = false;
};

struct InstallConfig {
    std::string disk;
    std::string username;
    std::string hostname;
    std::string timezone;
    std::string autologin = "true";
    std::string password;
};

std::string json_escape(const std::string &input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += ' ';
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string read_first_line(const std::string &path, const std::string &fallback = "") {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return fallback;
    std::string line;
    std::getline(ifs, line);
    return line;
}

std::string run_cmd_capture(const char *cmd) {
    std::array<char, 512> buffer{};
    std::string result;
    FILE *pipe = popen(cmd, "r");
    if (!pipe) return "";
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }
    pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

void spawn_detached(const std::string &cmd) {
    std::string wrapped = "nohup " + cmd + " >/dev/null 2>&1 &";
    std::system(wrapped.c_str());
}

bool is_live_iso_env() {
    return fs::exists("/run/archiso/bootmnt") || fs::exists("/run/archiso/airootfs");
}

std::string detect_gpu_summary() {
    std::string gpu = run_cmd_capture(
        "lspci -mm 2>/dev/null | awk -F'\"' '/VGA|3D|Display/ {print $6; exit}'"
    );
    if (gpu.empty()) return "Generic DRM/Mesa";
    return gpu;
}

std::string detect_mem_usage() {
    std::ifstream ifs("/proc/meminfo");
    if (!ifs.is_open()) return "N/A";
    long total_kb = 0;
    long avail_kb = 0;
    std::string key;
    long val = 0;
    std::string unit;
    while (ifs >> key >> val >> unit) {
        if (key == "MemTotal:") total_kb = val;
        else if (key == "MemAvailable:") avail_kb = val;
    }
    if (total_kb <= 0) return "N/A";
    long used_mb = (total_kb - avail_kb) / 1024;
    long total_mb = total_kb / 1024;
    std::ostringstream oss;
    oss << used_mb << " / " << total_mb << " MB";
    return oss.str();
}

std::string detect_network_status() {
    std::string net = run_cmd_capture(
        "nmcli -t -f DEVICE,TYPE,STATE dev status 2>/dev/null | awk -F: '$3==\"connected\" {print $1\" (\"$2\")\"; exit}'"
    );
    if (net.empty()) return "Offline";
    return net;
}

std::string resolve_ui_uri(const std::string &page_query) {
    const char *env_dir = std::getenv("SPINEL_UI_DIR");
    fs::path base = env_dir ? fs::path(env_dir) : fs::path("/usr/share/spinel-shell/ui");
    if (!fs::exists(base / "index.html")) {
        base = fs::current_path() / "ui";
    }
    return "file://" + fs::absolute(base / "index.html").string() + page_query;
}

void push_telemetry_to_view(WebKitWebView *view) {
    if (!view) return;
    std::string gpu = json_escape(detect_gpu_summary());
    std::string mem = json_escape(detect_mem_usage());
    std::string net = json_escape(detect_network_status());
    std::string kernel = json_escape(read_first_line("/proc/sys/kernel/osrelease", "Linux"));
    bool is_live = is_live_iso_env();

    std::ostringstream js;
    js << "window.__spinelUpdateTelemetry && window.__spinelUpdateTelemetry({"
       << "\"gpu\":\"" << gpu << "\","
       << "\"mem\":\"" << mem << "\","
       << "\"net\":\"" << net << "\","
       << "\"kernel\":\"" << kernel << "\","
       << "\"is_live\":" << (is_live ? "true" : "false")
       << "});";

    webkit_web_view_evaluate_javascript(view, js.str().c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr);
}

void push_installer_env(AppState *state) {
    if (!state || !state->hub_view) return;
    bool is_live = is_live_iso_env();
    bool is_uefi = fs::exists("/sys/firmware/efi/efivars");
    std::string boot_disk = run_cmd_capture(
        "src=$(findmnt -n -o SOURCE /run/archiso/bootmnt 2>/dev/null || true); "
        "[ -n \"$src\" ] && lsblk -no PKNAME \"$src\" 2>/dev/null | head -n1 || true"
    );
    std::string lsblk_json = run_cmd_capture("lsblk -J -d -b -o NAME,SIZE,MODEL,TRAN,RM,TYPE 2>/dev/null");
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

    webkit_web_view_evaluate_javascript(state->hub_view, js.str().c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr);
}

gboolean on_installer_idle(gpointer data) {
    std::unique_ptr<IdleMessage> msg(static_cast<IdleMessage *>(data));
    if (msg->state->shutting_down.load() || !msg->state->hub_view) {
        return G_SOURCE_REMOVE;
    }

    if (msg->is_done) {
        msg->state->install_running.store(false);
        std::ostringstream js;
        js << "window.__spinelInstallerDone && window.__spinelInstallerDone(" << msg->exit_code << ");";
        webkit_web_view_evaluate_javascript(
            msg->state->hub_view, js.str().c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr
        );
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
            webkit_web_view_evaluate_javascript(
                msg->state->hub_view, js.str().c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr
            );
            return G_SOURCE_REMOVE;
        }
    }

    std::ostringstream js;
    js << "window.__spinelInstallerLog && window.__spinelInstallerLog(\"" << json_escape(line) << "\");";
    webkit_web_view_evaluate_javascript(
        msg->state->hub_view, js.str().c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr
    );
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
    std::istringstream iss(payload);
    std::string line;
    // Skip header line ("installer:start")
    std::getline(iss, line);
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "disk") cfg.disk = val;
        else if (key == "username") cfg.username = val;
        else if (key == "hostname") cfg.hostname = val;
        else if (key == "timezone") cfg.timezone = val;
        else if (key == "autologin") cfg.autologin = (val == "true" ? "true" : "false");
        else if (key == "password") cfg.password = val;
    }
    return cfg;
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
    if (!is_valid_disk(cfg.disk)) {
        emit_installer_error(state, "无效的目标磁盘路径: " + cfg.disk);
        return;
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
            "--disk", cfg.disk,
            "--username", cfg.username,
            "--hostname", cfg.hostname,
            "--timezone", cfg.timezone,
            "--autologin", cfg.autologin
        };
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

gboolean on_telemetry_tick(gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    push_telemetry_to_view(state->bar_view);
    if (state->hub_visible) {
        push_telemetry_to_view(state->hub_view);
    }
    return G_SOURCE_CONTINUE;
}

void handle_command(AppState *state, const std::string &cmd) {
    if (cmd == "launch:steam") {
        spawn_detached("steam");
    } else if (cmd == "launch:steam-gamepadui") {
        spawn_detached("gamescope -f -- steam -gamepadui");
    } else if (cmd == "launch:terminal") {
        spawn_detached("foot");
    } else if (cmd == "launch:files") {
        spawn_detached("foot -e lf || foot");
    } else if (cmd == "system:toggle-hub") {
        if (state->hub_window) {
            state->hub_visible = !state->hub_visible;
            gtk_widget_set_visible(GTK_WIDGET(state->hub_window), state->hub_visible);
            if (state->hub_visible) {
                push_telemetry_to_view(state->hub_view);
            }
        }
    } else if (cmd == "system:wifi-ui") {
        spawn_detached("foot -e nmtui");
    } else if (cmd == "system:bt-ui") {
        spawn_detached("foot -e bluetoothctl");
    } else if (cmd == "system:audio-down") {
        spawn_detached("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-");
    } else if (cmd == "system:audio-up") {
        spawn_detached("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+");
    } else if (cmd == "power:reboot") {
        spawn_detached("systemctl reboot");
    } else if (cmd == "power:shutdown") {
        spawn_detached("systemctl poweroff");
    } else if (cmd == "telemetry:refresh") {
        push_telemetry_to_view(state->bar_view);
        push_telemetry_to_view(state->hub_view);
    } else if (cmd == "installer:scan-disks") {
        push_installer_env(state);
    } else if (cmd.rfind("installer:start\n", 0) == 0) {
        start_disk_install(state, cmd);
    }
}

void on_script_message(WebKitUserContentManager *, JSCValue *value, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    if (!jsc_value_is_string(value)) return;
    g_autofree char *msg = jsc_value_to_string(value);
    if (msg) {
        handle_command(state, msg);
    }
}

WebKitWebView *create_configured_webview(AppState *state) {
    WebKitWebView *web = WEBKIT_WEB_VIEW(webkit_web_view_new());

    GdkRGBA transparent{0.0, 0.0, 0.0, 0.0};
    webkit_web_view_set_background_color(web, &transparent);

    WebKitSettings *settings = webkit_web_view_get_settings(web);
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_allow_file_access_from_file_urls(settings, TRUE);
    webkit_settings_set_hardware_acceleration_policy(
        settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_ON_DEMAND
    );

    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(web);
    g_signal_connect(ucm, "script-message-received::sysbridge", G_CALLBACK(on_script_message), state);
    webkit_user_content_manager_register_script_message_handler(ucm, "sysbridge", nullptr);

    return web;
}

GtkWindow *create_top_bar(GtkApplication *app, AppState *state) {
    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(win, "SpinelTopBar");
    gtk_window_set_default_size(win, -1, 48);

    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "spinel-bar");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_auto_exclusive_zone_enable(win);
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    state->bar_view = create_configured_webview(state);
    gtk_widget_set_size_request(GTK_WIDGET(state->bar_view), -1, 48);
    gtk_window_set_child(win, GTK_WIDGET(state->bar_view));

    std::string uri = resolve_ui_uri("?mode=bar");
    webkit_web_view_load_uri(state->bar_view, uri.c_str());
    return win;
}

GtkWindow *create_desktop_hub(GtkApplication *app, AppState *state) {
    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(win, "SpinelDesktopHub");

    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "spinel-hub");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_BOTTOM);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

    state->hub_view = create_configured_webview(state);
    gtk_window_set_child(win, GTK_WIDGET(state->hub_view));

    std::string uri = resolve_ui_uri("?mode=hub");
    webkit_web_view_load_uri(state->hub_view, uri.c_str());
    return win;
}

void on_activate(GtkApplication *app, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    GtkWindow *bar = create_top_bar(app, state);
    state->hub_window = create_desktop_hub(app, state);

    gtk_window_present(state->hub_window);
    gtk_window_present(bar);

    g_timeout_add_seconds(3, on_telemetry_tick, state);
}

}  // namespace

int main(int argc, char **argv) {
    AppState state;
    GtkApplication *app = gtk_application_new("org.spinelos.shell", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &state);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    state.shutting_down.store(true);
    if (state.install_thread.joinable()) {
        state.install_thread.join();
    }
    g_object_unref(app);
    return status;
}
