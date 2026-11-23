#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <webkit/webkit.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

struct AppState {
    WebKitWebView *bar_view = nullptr;
    WebKitWebView *hub_view = nullptr;
    GtkWindow *hub_window = nullptr;
    bool hub_visible = true;
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
    std::array<char, 256> buffer{};
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

    std::ostringstream js;
    js << "window.__spinelUpdateTelemetry && window.__spinelUpdateTelemetry({"
       << "\"gpu\":\"" << gpu << "\","
       << "\"mem\":\"" << mem << "\","
       << "\"net\":\"" << net << "\","
       << "\"kernel\":\"" << kernel << "\""
       << "});";

    webkit_web_view_evaluate_javascript(view, js.str().c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr);
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
    g_object_unref(app);
    return status;
}
