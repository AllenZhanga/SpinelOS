#include "app_state.hpp"
#include "common.hpp"
#include "installer.hpp"
#include "steam_library.hpp"
#include "system_control.hpp"

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <webkit/webkit.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string resolve_ui_uri(const std::string &page_query) {
    const char *env_dir = std::getenv("SPINEL_UI_DIR");
    fs::path base = env_dir ? fs::path(env_dir) : fs::path("/usr/share/spinel-shell/ui");
    if (!fs::exists(base / "index.html")) {
        base = fs::current_path() / "ui";
    }
    return "file://" + fs::absolute(base / "index.html").string() + page_query;
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
        ensure_steam_play_preconfig();
        spawn_argv_detached({"steam"});
    } else if (cmd == "launch:steam-gamepadui") {
        ensure_steam_play_preconfig();
        launch_steam_gamepadui(load_perf_config());
    } else if (cmd.rfind("steam:launch:", 0) == 0) {
        ensure_steam_play_preconfig();
        launch_steam_game(cmd.substr(13), load_perf_config());
    } else if (cmd == "steam:scan") {
        push_steam_library(state);
    } else if (cmd == "launch:terminal") {
        spawn_argv_detached({"foot"});
    } else if (cmd == "system:toggle-hub") {
        if (state->hub_window) {
            state->hub_visible = !state->hub_visible;
            gtk_widget_set_visible(GTK_WIDGET(state->hub_window), state->hub_visible);
            if (state->hub_visible) {
                push_telemetry_to_view(state->hub_view);
                push_steam_library(state);
            }
        }
    } else if (cmd == "system:audio-down") {
        spawn_argv_detached({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-"});
    } else if (cmd == "system:audio-up") {
        spawn_argv_detached({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%+"});
    } else if (cmd == "power:reboot") {
        spawn_argv_detached({"systemctl", "reboot"});
    } else if (cmd == "power:shutdown") {
        spawn_argv_detached({"systemctl", "poweroff"});
    } else if (cmd == "telemetry:refresh") {
        push_telemetry_to_view(state->bar_view);
        push_telemetry_to_view(state->hub_view);
        push_perf_config(state);
        push_steam_library(state);
    } else if (cmd == "perf:get") {
        push_perf_config(state);
    } else if (cmd.rfind("perf:set\n", 0) == 0) {
        update_perf_config(state, cmd);
    } else if (cmd.rfind("settings:open:", 0) == 0) {
        std::string tab = cmd.substr(14);
        if (tab == "wifi" || tab == "bt" || tab == "display") {
            if (state->hub_window && !state->hub_visible) {
                state->hub_visible = true;
                gtk_widget_set_visible(GTK_WIDGET(state->hub_window), TRUE);
            }
            eval_js(state->hub_view, "window.__spinelOpenSettings && window.__spinelOpenSettings('" + tab + "');");
        }
    } else if (cmd == "settings:wifi-list") {
        push_wifi_state(state, false);
    } else if (cmd == "settings:wifi-scan") {
        push_wifi_state(state, true);
    } else if (cmd.rfind("settings:wifi-connect\n", 0) == 0) {
        connect_wifi(state, cmd);
    } else if (cmd.rfind("settings:wifi-disconnect:", 0) == 0) {
        disconnect_iface(state, cmd.substr(25));
    } else if (cmd == "settings:bt-status") {
        push_bt_state(state, false);
    } else if (cmd == "settings:bt-scan") {
        push_bt_state(state, true);
    } else if (cmd == "settings:bt-power:on") {
        set_bt_power(state, true);
    } else if (cmd == "settings:bt-power:off") {
        set_bt_power(state, false);
    } else if (cmd.rfind("settings:bt-connect:", 0) == 0) {
        bt_device_action(state, "connect", cmd.substr(20));
    } else if (cmd.rfind("settings:bt-disconnect:", 0) == 0) {
        bt_device_action(state, "disconnect", cmd.substr(23));
    } else if (cmd.rfind("settings:bt-remove:", 0) == 0) {
        bt_device_action(state, "remove", cmd.substr(19));
    } else if (cmd == "settings:display-scan") {
        push_display_state(state);
    } else if (cmd.rfind("settings:display-apply\n", 0) == 0) {
        apply_display_mode(state, cmd);
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

WebKitWebView *create_configured_webview(AppState *state, bool opaque_dark) {
    WebKitWebView *web = WEBKIT_WEB_VIEW(webkit_web_view_new());

    GdkRGBA bg = opaque_dark ? GdkRGBA{0.031, 0.043, 0.071, 1.0} : GdkRGBA{0.0, 0.0, 0.0, 0.0};
    webkit_web_view_set_background_color(web, &bg);

    WebKitSettings *settings = webkit_web_view_get_settings(web);
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_allow_file_access_from_file_urls(settings, TRUE);
    webkit_settings_set_hardware_acceleration_policy(
        settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS
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

    state->bar_view = create_configured_webview(state, false);
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

    state->hub_view = create_configured_webview(state, true);
    gtk_window_set_child(win, GTK_WIDGET(state->hub_view));

    std::string uri = resolve_ui_uri("?mode=hub");
    webkit_web_view_load_uri(state->hub_view, uri.c_str());
    return win;
}

void on_activate(GtkApplication *app, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    ensure_steam_play_preconfig();
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
