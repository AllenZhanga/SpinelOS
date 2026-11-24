#include "app_state.hpp"
#include "app_store.hpp"
#include "common.hpp"
#include "desktop_apps.hpp"
#include "installer.hpp"
#include "native_shell.hpp"
#include "steam_library.hpp"
#include "system_control.hpp"
#include "theme_manager.hpp"
#include "window_tracker.hpp"

#include <gtk/gtk.h>
#include <webkit/webkit.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

GtkApplication *g_app = nullptr;

struct AppWindowSlotCtx {
    AppState *state = nullptr;
    GtkWindow **win_slot = nullptr;
    WebKitWebView **view_slot = nullptr;
};

std::string resolve_ui_uri(const std::string &page_query) {
    const char *env_dir = std::getenv("SPINEL_UI_DIR");
    fs::path base = env_dir ? fs::path(env_dir) : fs::path("/usr/share/spinel-shell/ui");
    if (!fs::exists(base / "index.html")) {
        base = fs::current_path() / "ui";
    }
    return "file://" + fs::absolute(base / "index.html").string() + page_query;
}

void on_script_message(WebKitUserContentManager *, JSCValue *value, gpointer user_data);

void on_webview_load_failed(WebKitWebView *, WebKitLoadEvent, gchar *uri, GError *err, gpointer) {
    std::fprintf(stderr, "[spinel-shell] WebKit load-failed (%s): %s\n",
                 uri ? uri : "", err ? err->message : "unknown");
}

void on_web_process_terminated(WebKitWebView *, WebKitWebProcessTerminationReason reason, gpointer) {
    std::fprintf(stderr, "[spinel-shell] WebKitWebProcess terminated (reason=%d)\n", static_cast<int>(reason));
}

WebKitWebView *create_configured_webview(AppState *state) {
    WebKitWebView *web = WEBKIT_WEB_VIEW(webkit_web_view_new());

    GdkRGBA bg{0.031, 0.043, 0.071, 1.0};
    webkit_web_view_set_background_color(web, &bg);

    WebKitSettings *settings = webkit_web_view_get_settings(web);
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_allow_file_access_from_file_urls(settings, TRUE);

    const char *wlr_renderer = std::getenv("WLR_RENDERER");
    const char *no_dmabuf = std::getenv("WEBKIT_DISABLE_DMABUF_RENDERER");
    bool sw_mode = collect_telemetry().low_fx ||
                   (wlr_renderer && std::string(wlr_renderer) == "pixman") ||
                   (no_dmabuf && std::string(no_dmabuf) == "1");
    webkit_settings_set_hardware_acceleration_policy(
        settings,
        sw_mode ? WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER
                : WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS
    );

    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(web);
    g_signal_connect(ucm, "script-message-received::sysbridge", G_CALLBACK(on_script_message), state);
    webkit_user_content_manager_register_script_message_handler(ucm, "sysbridge", nullptr);

    g_signal_connect(web, "load-failed", G_CALLBACK(on_webview_load_failed), nullptr);
    g_signal_connect(web, "web-process-terminated", G_CALLBACK(on_web_process_terminated), nullptr);

    return web;
}

gboolean on_app_window_close_request(GtkWindow *win, gpointer user_data) {
    auto *ctx = static_cast<AppWindowSlotCtx *>(user_data);
    if (ctx && ctx->state) {
        if ((win == ctx->state->installer_win && ctx->state->install_running.load()) ||
            (win == ctx->state->store_win && ctx->state->store_busy.load())) {
            gtk_widget_set_visible(GTK_WIDGET(win), FALSE);
            return TRUE;  // Keep alive while disk install / package install runs
        }
    }
    return FALSE;  // Destroy window & terminate its WebKitWebProcess to reclaim RAM
}

void on_app_window_destroy(GtkWidget *, gpointer user_data) {
    auto *ctx = static_cast<AppWindowSlotCtx *>(user_data);
    if (ctx) {
        if (ctx->win_slot) *ctx->win_slot = nullptr;
        if (ctx->view_slot) *ctx->view_slot = nullptr;
        delete ctx;
    }
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

        auto *ctx = new AppWindowSlotCtx{state, win_slot, view_slot};
        g_signal_connect(win, "close-request", G_CALLBACK(on_app_window_close_request), ctx);
        g_signal_connect(win, "destroy", G_CALLBACK(on_app_window_destroy), ctx);

        WebKitWebView *view = create_configured_webview(state);
        gtk_window_set_child(win, GTK_WIDGET(view));
        std::string uri = resolve_ui_uri(query);
        std::fprintf(stderr, "[spinel-shell] Opening app window: %s -> %s\n", title, uri.c_str());
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
    push_theme_state(state);
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
    push_theme_state(state);
    push_wallpaper_pref(state);
}

void open_installer_window(AppState *state) {
    bool existed = (state->installer_win != nullptr);
    ensure_app_window(state, &state->installer_win, &state->installer_view,
                      "安装 SpinelOS 到硬盘", "?mode=installer", 860, 660);
    if (existed) {
        push_theme_state(state);
        push_installer_env(state);
    }
}

void open_store_window(AppState *state) {
    bool existed = (state->store_win != nullptr);
    ensure_app_window(state, &state->store_win, &state->store_view,
                      "SpinelOS 应用商店", "?mode=store", 1020, 680);
    if (existed) {
        push_theme_state(state);
        push_desktop_apps(state);
        push_store_catalog(state);
    }
}

void push_all_telemetry(AppState *state) {
    native_shell_sync_telemetry(state);
    if (state->games_win && gtk_widget_get_visible(GTK_WIDGET(state->games_win))) {
        push_telemetry_to_view(state->games_view);
    }
    if (state->store_win && gtk_widget_get_visible(GTK_WIDGET(state->store_win))) {
        push_telemetry_to_view(state->store_view);
    }
    if (state->settings_win && gtk_widget_get_visible(GTK_WIDGET(state->settings_win))) {
        push_telemetry_to_view(state->settings_view);
    }
}

gboolean on_telemetry_tick(gpointer user_data) {
    push_all_telemetry(static_cast<AppState *>(user_data));
    return G_SOURCE_CONTINUE;
}

void on_script_message(WebKitUserContentManager *, JSCValue *value, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    if (!jsc_value_is_string(value)) return;
    g_autofree char *msg = jsc_value_to_string(value);
    if (msg) {
        handle_shell_command(state, msg);
    }
}

void on_activate(GtkApplication *app, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    g_app = app;
    ensure_steam_play_preconfig();

    init_native_shell(app, state);
    init_window_tracker(state);

    g_timeout_add_seconds(3, on_telemetry_tick, state);
}

}  // namespace

void handle_shell_command(AppState *state, const std::string &cmd) {
    if (!state) return;

    if (cmd == "launch:steam") {
        native_shell_set_cc_visible(state, false);
        ensure_steam_play_preconfig();
        launch_steam_client(state);
    } else if (cmd == "launch:steam-gamepadui") {
        native_shell_set_cc_visible(state, false);
        ensure_steam_play_preconfig();
        launch_steam_gamepadui(state, load_perf_config());
    } else if (cmd.rfind("steam:launch:", 0) == 0) {
        native_shell_set_cc_visible(state, false);
        ensure_steam_play_preconfig();
        launch_steam_game(cmd.substr(13), load_perf_config());
    } else if (cmd == "steam:scan") {
        push_steam_library(state);
    } else if (cmd == "launch:terminal") {
        native_shell_set_cc_visible(state, false);
        spawn_argv_detached({"foot"});
    } else if (cmd == "launch:files") {
        native_shell_set_cc_visible(state, false);
        spawn_argv_detached({"thunar"});
    } else if (cmd == "desktop:open-folder") {
        std::error_code ec;
        fs::path desk = fs::path(home_dir()) / "Desktop";
        fs::create_directories(desk, ec);
        spawn_argv_detached({"thunar", desk.string()});
    } else if (cmd == "desktop:new-folder") {
        std::error_code ec;
        fs::path desk = fs::path(home_dir()) / "Desktop";
        fs::create_directories(desk, ec);
        fs::path target = desk / "新建文件夹";
        int idx = 2;
        while (fs::exists(target, ec) && idx < 100) {
            target = desk / ("新建文件夹 " + std::to_string(idx++));
        }
        fs::create_directory(target, ec);
        spawn_argv_detached({"thunar", desk.string()});
    } else if (cmd == "launch:browser") {
        native_shell_set_cc_visible(state, false);
        std::string tmp_out;
        int rc = 1;
        if (run_argv_capture({"which", "google-chrome-stable"}, tmp_out, rc) && rc == 0) {
            spawn_argv_detached({"google-chrome-stable", "--ozone-platform-hint=auto"});
        } else if (run_argv_capture({"which", "chromium"}, tmp_out, rc) && rc == 0) {
            spawn_argv_detached({"chromium", "--ozone-platform-hint=auto"});
        } else if (run_argv_capture({"which", "firefox"}, tmp_out, rc) && rc == 0) {
            spawn_argv_detached({"firefox"});
        } else {
            open_store_window(state);
        }
    } else if (cmd.rfind("app:launch:", 0) == 0) {
        native_shell_set_cc_visible(state, false);
        launch_desktop_app_id(cmd.substr(11));
    } else if (cmd == "games:open") {
        native_shell_set_cc_visible(state, false);
        open_games_window(state);
    } else if (cmd == "store:open") {
        native_shell_set_cc_visible(state, false);
        open_store_window(state);
    } else if (cmd == "store:refresh") {
        push_desktop_apps(state);
        push_store_catalog(state);
    } else if (cmd.rfind("store:install\n", 0) == 0) {
        start_store_action(state, "install", cmd);
    } else if (cmd.rfind("store:remove\n", 0) == 0) {
        start_store_action(state, "remove", cmd);
    } else if (cmd.rfind("store:launch:", 0) == 0) {
        native_shell_set_cc_visible(state, false);
        launch_store_app(state, cmd.substr(13));
    } else if (cmd == "installer:open") {
        native_shell_set_cc_visible(state, false);
        open_installer_window(state);
    } else if (cmd.rfind("settings:open:", 0) == 0) {
        std::string tab = cmd.substr(14);
        if (tab == "wifi" || tab == "bt" || tab == "display" || tab == "appearance") {
            native_shell_set_cc_visible(state, false);
            open_settings_window(state, tab);
        }
    } else if (cmd == "cc:toggle") {
        native_shell_toggle_cc(state);
    } else if (cmd == "cc:close") {
        native_shell_set_cc_visible(state, false);
    } else if (cmd.rfind("system:audio-set:", 0) == 0) {
        int pct = std::clamp(std::atoi(cmd.substr(17).c_str()), 0, 100);
        spawn_argv_detached({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", std::to_string(pct) + "%"});
    } else if (cmd.rfind("system:brightness-set:", 0) == 0) {
        int pct = std::clamp(std::atoi(cmd.substr(22).c_str()), 10, 100);
        spawn_argv_detached({
            "sh", "-c",
            "for d in /sys/class/backlight/*; do "
            "[ -w \"$d/brightness\" ] && [ -r \"$d/max_brightness\" ] || continue; "
            "max=$(cat \"$d/max_brightness\"); "
            "echo $(( max * " + std::to_string(pct) + " / 100 )) > \"$d/brightness\"; break; done"
        });
    } else if (cmd.rfind("window:toggle:", 0) == 0) {
        uint64_t wid = std::strtoull(cmd.substr(14).c_str(), nullptr, 10);
        if (wid > 0) toggle_or_activate_window(wid);
    } else if (cmd.rfind("window:close:", 0) == 0) {
        uint64_t wid = std::strtoull(cmd.substr(13).c_str(), nullptr, 10);
        if (wid > 0) close_tracked_window(wid);
    } else if (cmd.rfind("wallpaper:set:", 0) == 0) {
        std::string wp = cmd.substr(14);
        if (wp == "crystal-blue") wp = "crystal-aurora";
        else if (wp == "obsidian-purple") wp = "obsidian-neon";
        else if (wp == "emerald-aurora") wp = "emerald-nordic";
        set_active_theme(state, wp);
    } else if (cmd.rfind("theme:set:", 0) == 0) {
        set_active_theme(state, cmd.substr(10));
    } else if (cmd == "theme:refresh") {
        push_theme_state(state);
        native_shell_sync_theme(state);
    } else if (cmd == "theme:export") {
        export_custom_theme_template();
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
        push_theme_state(state);
        push_perf_config(state);
        push_steam_library(state);
        push_desktop_apps(state);
        push_wallpaper_pref(state);
        push_windows_to_ui(state);
    } else if (cmd == "perf:get") {
        push_perf_config(state);
    } else if (cmd == "perf:toggle-gamemode") {
        PerfConfig cfg = load_perf_config();
        update_perf_config(state, std::string("gamemode=") + (cfg.gamemode ? "false" : "true"));
    } else if (cmd == "perf:toggle-fsr") {
        PerfConfig cfg = load_perf_config();
        update_perf_config(state, std::string("fsr=") + (cfg.fsr ? "false" : "true"));
    } else if (cmd == "perf:cycle-mangohud") {
        PerfConfig cfg = load_perf_config();
        std::string next = cfg.mangohud == "off" ? "compact" : (cfg.mangohud == "compact" ? "full" : "off");
        update_perf_config(state, "mangohud=" + next);
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

int main(int argc, char **argv) {
    const char *wlr = std::getenv("WLR_RENDERER");
    if (wlr && std::string(wlr) == "pixman") {
        setenv("GSK_RENDERER", "cairo", 0);
        setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
        setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 0);
    } else if (collect_telemetry().low_fx) {
        setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
    }

    AppState state;
    GtkApplication *app = gtk_application_new("org.spinelos.shell", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &state);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    state.shutting_down.store(true);
    shutdown_window_tracker();
    shutdown_native_shell(&state);
    if (state.install_thread.joinable()) {
        state.install_thread.join();
    }
    if (state.store_thread.joinable()) {
        state.store_thread.join();
    }
    g_object_unref(app);
    return status;
}
