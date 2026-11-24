#include "app_state.hpp"
#include "common.hpp"
#include "desktop_apps.hpp"
#include "installer.hpp"
#include "steam_library.hpp"
#include "system_control.hpp"
#include "window_tracker.hpp"

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <webkit/webkit.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

GtkApplication *g_app = nullptr;

std::string resolve_ui_uri(const std::string &page_query) {
    const char *env_dir = std::getenv("SPINEL_UI_DIR");
    fs::path base = env_dir ? fs::path(env_dir) : fs::path("/usr/share/spinel-shell/ui");
    if (!fs::exists(base / "index.html")) {
        base = fs::current_path() / "ui";
    }
    return "file://" + fs::absolute(base / "index.html").string() + page_query;
}

void on_script_message(WebKitUserContentManager *, JSCValue *value, gpointer user_data);

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

gboolean on_hide_window_instead_of_destroy(GtkWindow *win, gpointer) {
    gtk_widget_set_visible(GTK_WIDGET(win), FALSE);
    return TRUE;
}

void ensure_app_window(AppState *state,
                       GtkWindow **win_slot,
                       WebKitWebView **view_slot,
                       const char *title,
                       const std::string &query,
                       int width,
                       int height) {
    if (!*win_slot) {
        GtkWindow *win = GTK_WINDOW(gtk_application_window_new(g_app));
        gtk_window_set_title(win, title);
        gtk_window_set_default_size(win, width, height);
        g_signal_connect(win, "close-request", G_CALLBACK(on_hide_window_instead_of_destroy), nullptr);

        WebKitWebView *view = create_configured_webview(state, true);
        gtk_window_set_child(win, GTK_WIDGET(view));
        std::string uri = resolve_ui_uri(query);
        webkit_web_view_load_uri(view, uri.c_str());

        *win_slot = win;
        *view_slot = view;
    }
    gtk_widget_set_visible(GTK_WIDGET(*win_slot), TRUE);
    gtk_window_present(*win_slot);
}

void open_games_window(AppState *state) {
    ensure_app_window(state, &state->games_win, &state->games_view,
                      "SpinelOS 游戏与启动中心", "?mode=games", 1040, 680);
    push_perf_config(state);
    push_steam_library(state);
    push_desktop_apps(state);
}

void open_settings_window(AppState *state, const std::string &tab) {
    bool existed = (state->settings_win != nullptr);
    ensure_app_window(state, &state->settings_win, &state->settings_view,
                      "SpinelOS 系统设置", "?mode=settings&tab=" + tab, 920, 640);
    if (existed) {
        eval_js(state->settings_view, "window.__spinelOpenSettings && window.__spinelOpenSettings('" + tab + "');");
    }
    push_wallpaper_pref(state);
}

void open_installer_window(AppState *state) {
    ensure_app_window(state, &state->installer_win, &state->installer_view,
                      "安装 SpinelOS 到硬盘", "?mode=installer", 860, 660);
    push_installer_env(state);
}

void push_all_telemetry(AppState *state) {
    push_telemetry_to_view(state->bar_view);
    push_telemetry_to_view(state->desktop_view);
    push_telemetry_to_view(state->dock_view);
    if (state->games_win && gtk_widget_get_visible(GTK_WIDGET(state->games_win))) {
        push_telemetry_to_view(state->games_view);
    }
    if (state->settings_win && gtk_widget_get_visible(GTK_WIDGET(state->settings_win))) {
        push_telemetry_to_view(state->settings_view);
    }
}

gboolean on_telemetry_tick(gpointer user_data) {
    push_all_telemetry(static_cast<AppState *>(user_data));
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
    } else if (cmd == "launch:files") {
        spawn_argv_detached({"thunar"});
    } else if (cmd == "launch:browser") {
        spawn_argv_detached({"chromium", "--ozone-platform-hint=auto"});
    } else if (cmd.rfind("app:launch:", 0) == 0) {
        launch_desktop_app_id(cmd.substr(11));
    } else if (cmd == "games:open") {
        open_games_window(state);
    } else if (cmd == "installer:open") {
        open_installer_window(state);
    } else if (cmd.rfind("settings:open:", 0) == 0) {
        std::string tab = cmd.substr(14);
        if (tab == "wifi" || tab == "bt" || tab == "display" || tab == "appearance") {
            open_settings_window(state, tab);
        }
    } else if (cmd.rfind("window:toggle:", 0) == 0) {
        uint64_t wid = std::strtoull(cmd.substr(14).c_str(), nullptr, 10);
        if (wid > 0) toggle_or_activate_window(wid);
    } else if (cmd.rfind("window:close:", 0) == 0) {
        uint64_t wid = std::strtoull(cmd.substr(13).c_str(), nullptr, 10);
        if (wid > 0) close_tracked_window(wid);
    } else if (cmd.rfind("dock:resize:", 0) == 0) {
        int w = std::clamp(std::atoi(cmd.substr(12).c_str()), 380, 1600);
        if (state->dock_window && state->dock_view) {
            gtk_widget_set_size_request(GTK_WIDGET(state->dock_view), w, 112);
            gtk_window_set_default_size(state->dock_window, w, 112);
        }
    } else if (cmd.rfind("wallpaper:set:", 0) == 0) {
        set_wallpaper_pref(state, cmd.substr(14));
    } else if (cmd == "system:audio-down") {
        spawn_argv_detached({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-"});
    } else if (cmd == "system:audio-up") {
        spawn_argv_detached({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%+"});
    } else if (cmd == "power:reboot") {
        spawn_argv_detached({"systemctl", "reboot"});
    } else if (cmd == "power:shutdown") {
        spawn_argv_detached({"systemctl", "poweroff"});
    } else if (cmd == "telemetry:refresh") {
        push_all_telemetry(state);
        push_perf_config(state);
        push_steam_library(state);
        push_desktop_apps(state);
        push_wallpaper_pref(state);
        push_windows_to_ui(state);
    } else if (cmd == "perf:get") {
        push_perf_config(state);
    } else if (cmd.rfind("perf:set\n", 0) == 0) {
        update_perf_config(state, cmd);
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

GtkWindow *create_top_bar(GtkApplication *app, AppState *state) {
    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(win, "SpinelTopBar");
    gtk_widget_add_css_class(GTK_WIDGET(win), "spinel-transparent");
    gtk_window_set_default_size(win, -1, 28);

    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "spinel-bar");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_auto_exclusive_zone_enable(win);
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    state->bar_view = create_configured_webview(state, false);
    gtk_widget_set_size_request(GTK_WIDGET(state->bar_view), -1, 28);
    gtk_window_set_child(win, GTK_WIDGET(state->bar_view));

    webkit_web_view_load_uri(state->bar_view, resolve_ui_uri("?mode=bar").c_str());
    return win;
}

GtkWindow *create_desktop_layer(GtkApplication *app, AppState *state) {
    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(win, "SpinelDesktop");

    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "spinel-desktop");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_BOTTOM);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

    state->desktop_view = create_configured_webview(state, true);
    gtk_window_set_child(win, GTK_WIDGET(state->desktop_view));

    webkit_web_view_load_uri(state->desktop_view, resolve_ui_uri("?mode=desktop").c_str());
    return win;
}

GtkWindow *create_dock_layer(GtkApplication *app, AppState *state) {
    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(win, "SpinelDock");
    gtk_widget_add_css_class(GTK_WIDGET(win), "spinel-transparent");
    gtk_window_set_default_size(win, 580, 112);

    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "spinel-dock");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_BOTTOM, 6);
    gtk_layer_set_exclusive_zone(win, 74);
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    state->dock_view = create_configured_webview(state, false);
    gtk_widget_set_size_request(GTK_WIDGET(state->dock_view), 580, 112);
    gtk_window_set_child(win, GTK_WIDGET(state->dock_view));

    webkit_web_view_load_uri(state->dock_view, resolve_ui_uri("?mode=dock").c_str());
    return win;
}

void on_activate(GtkApplication *app, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    g_app = app;
    ensure_steam_play_preconfig();

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        css,
        "window.spinel-transparent, window.spinel-transparent.background {\n"
        "  background-color: transparent;\n"
        "  background-image: none;\n"
        "  box-shadow: none;\n"
        "  border: none;\n"
        "}\n");
    if (GdkDisplay *display = gdk_display_get_default()) {
        gtk_style_context_add_provider_for_display(
            display, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    g_object_unref(css);

    GtkWindow *desktop = create_desktop_layer(app, state);
    GtkWindow *bar = create_top_bar(app, state);
    state->dock_window = create_dock_layer(app, state);

    init_window_tracker(state);

    gtk_window_present(desktop);
    gtk_window_present(bar);
    gtk_window_present(state->dock_window);

    g_timeout_add_seconds(3, on_telemetry_tick, state);
}

}  // namespace

int main(int argc, char **argv) {
    AppState state;
    GtkApplication *app = gtk_application_new("org.spinelos.shell", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &state);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    state.shutting_down.store(true);
    shutdown_window_tracker();
    if (state.install_thread.joinable()) {
        state.install_thread.join();
    }
    g_object_unref(app);
    return status;
}
