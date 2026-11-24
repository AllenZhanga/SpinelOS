#include "app_state.hpp"
#include "common.hpp"
#include "desktop_apps.hpp"
#include "native_shell.hpp"
#include "steam_library.hpp"
#include "system_control.hpp"
#include "theme_manager.hpp"

#if SPINEL_HAS_WEBKIT
#include "app_store.hpp"
#include "installer.hpp"
#include <malloc.h>
#include <webkit/webkit.h>
#else
#include "window_tracker.hpp"
#endif

#include <gtk/gtk.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace {

GtkApplication *g_app = nullptr;

void notify_dbus_action(const char *bus_name, const char *obj_path, const char *action_name) {
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus) return;
    GVariantBuilder empty_params;
    g_variant_builder_init(&empty_params, G_VARIANT_TYPE("av"));
    GVariantBuilder empty_platform;
    g_variant_builder_init(&empty_platform, G_VARIANT_TYPE("a{sv}"));
    g_dbus_connection_call(
        bus,
        bus_name,
        obj_path,
        "org.gtk.Actions",
        "Activate",
        g_variant_new("(s@av@a{sv})",
                      action_name,
                      g_variant_builder_end(&empty_params),
                      g_variant_builder_end(&empty_platform)),
        nullptr,
        G_DBUS_CALL_FLAGS_NO_AUTO_START,
        1000,
        nullptr,
        nullptr,
        nullptr
    );
    g_object_unref(bus);
}

#if SPINEL_HAS_WEBKIT

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
    std::string msg = std::string("WebKit load-failed (") + (uri ? uri : "") + "): " + (err ? err->message : "unknown");
    std::fprintf(stderr, "[spinel-webui] %s\n", msg.c_str());
    append_app_log("[WebKit] " + msg);
}

void on_web_process_terminated(WebKitWebView *, WebKitWebProcessTerminationReason reason, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    std::string msg = "WebKitWebProcess 渲染进程异常终止 (reason=" + std::to_string(static_cast<int>(reason)) + ")";
    std::fprintf(stderr, "[spinel-webui] %s\n", msg.c_str());
    append_app_log("[WebKit] " + msg);
    if (state && !state->shutting_down.load()) {
        open_native_log_window(state, msg + "。可查看下方日志与环境参数定位原因。");
    }
}

WebKitWebView *create_configured_webview(AppState *state) {
    static bool cache_configured = false;
    if (!cache_configured) {
        if (WebKitWebContext *def_ctx = webkit_web_context_get_default()) {
            webkit_web_context_set_cache_model(def_ctx, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
        }
        cache_configured = true;
    }

    WebKitWebView *web = WEBKIT_WEB_VIEW(webkit_web_view_new());

    ThemeSpec active = get_active_theme();
    GdkRGBA bg = active.is_light
        ? GdkRGBA{0.945f, 0.961f, 0.976f, 1.0f}
        : GdkRGBA{0.031f, 0.043f, 0.071f, 1.0f};
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

    g_signal_connect(web, "load-failed", G_CALLBACK(on_webview_load_failed), state);
    g_signal_connect(web, "web-process-terminated", G_CALLBACK(on_web_process_terminated), state);

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
    return FALSE;  // Destroy window & reclaim RAM
}

void on_app_window_destroy(GtkWidget *, gpointer user_data) {
    auto *ctx = static_cast<AppWindowSlotCtx *>(user_data);
    if (ctx) {
        if (ctx->win_slot) *ctx->win_slot = nullptr;
        if (ctx->view_slot) *ctx->view_slot = nullptr;
        delete ctx;
    }
    malloc_trim(0);
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
        std::fprintf(stderr, "[spinel-webui] Opening app window: %s -> %s\n", title, uri.c_str());
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
                      "SpinelOS 系统设置", "?mode=settings&tab=" + tab, 960, 660);
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

void on_script_message(WebKitUserContentManager *, JSCValue *value, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    if (!jsc_value_is_string(value)) return;
    g_autofree char *msg = jsc_value_to_string(value);
    if (msg) {
        handle_shell_command(state, msg);
    }
}

#else

std::string resolve_webui_bin() {
    std::error_code ec;
    fs::path self_dir = fs::read_symlink("/proc/self/exe", ec).parent_path();
    if (!self_dir.empty() && fs::exists(self_dir / "spinel-webui", ec)) {
        return (self_dir / "spinel-webui").string();
    }
    return has_executable("spinel-webui") ? "spinel-webui" : "";
}

void launch_webui_cmd(const std::string &cmd) {
    std::string bin = resolve_webui_bin();
    if (bin.empty()) {
        append_app_log("[Shell] 未找到 spinel-webui 可执行文件，无法打开: " + cmd);
        return;
    }
    spawn_argv_detached({bin, cmd});
}

void open_games_window(AppState *) { launch_webui_cmd("games:open"); }
void open_store_window(AppState *) { launch_webui_cmd("store:open"); }
void open_installer_window(AppState *) { launch_webui_cmd("installer:open"); }
void open_settings_window(AppState *, const std::string &tab) {
    launch_webui_cmd("settings:open:" + tab);
}

#endif  // SPINEL_HAS_WEBKIT

void push_all_telemetry(AppState *state) {
#if SPINEL_HAS_WEBKIT
    if (state->games_win && gtk_widget_get_visible(GTK_WIDGET(state->games_win))) {
        push_telemetry_to_view(state->games_view);
    }
    if (state->store_win && gtk_widget_get_visible(GTK_WIDGET(state->store_win))) {
        push_telemetry_to_view(state->store_view);
    }
    if (state->settings_win && gtk_widget_get_visible(GTK_WIDGET(state->settings_win))) {
        push_telemetry_to_view(state->settings_view);
    }
#else
    native_shell_sync_telemetry(state);
#endif
}

void on_power_confirm_reply(GObject *source, GAsyncResult *res, gpointer user_data) {
    bool is_shutdown = (user_data != nullptr);
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, nullptr);
    if (choice == 1) {
        append_app_log(is_shutdown ? "[Power] 用户确认关机 (systemctl poweroff)" : "[Power] 用户确认重启 (systemctl reboot)");
        spawn_argv_detached({"systemctl", is_shutdown ? "poweroff" : "reboot"});
    }
}

void confirm_power_action(AppState *state, bool is_shutdown) {
    native_shell_set_cc_visible(state, false);
    GtkAlertDialog *dlg = gtk_alert_dialog_new(
        "%s", is_shutdown ? "确定要关闭 SpinelOS 电源吗？" : "确定要立即重启系统吗？"
    );
    gtk_alert_dialog_set_detail(
        dlg, is_shutdown ? "关机前请确认正在运行的游戏或文档已保存。" : "重启会关闭所有打开的窗口与应用。"
    );
    const char *buttons[] = {"取消", is_shutdown ? "确定关机" : "确定重启", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_choose(dlg, nullptr, nullptr, on_power_confirm_reply,
                            is_shutdown ? GINT_TO_POINTER(1) : nullptr);
    g_object_unref(dlg);
}

gboolean on_telemetry_tick(gpointer user_data) {
    push_all_telemetry(static_cast<AppState *>(user_data));
    return G_SOURCE_CONTINUE;
}

#if SPINEL_HAS_WEBKIT

void register_webui_actions(GtkApplication *app, AppState *state) {
    GSimpleAction *sync_theme = g_simple_action_new("sync-theme", nullptr);
    g_signal_connect(sync_theme, "activate", G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer d) {
        push_theme_state(static_cast<AppState *>(d));
    }), state);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sync_theme));
    g_object_unref(sync_theme);

    GSimpleAction *sync_perf = g_simple_action_new("sync-perf", nullptr);
    g_signal_connect(sync_perf, "activate", G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer d) {
        push_perf_config(static_cast<AppState *>(d));
    }), state);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sync_perf));
    g_object_unref(sync_perf);
}

int on_webui_command_line(GtkApplication *app, GApplicationCommandLine *cmdline, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    if (!g_app) {
        g_app = app;
        ensure_steam_play_preconfig();
        register_webui_actions(app, state);
        g_timeout_add_seconds(3, on_telemetry_tick, state);
    }

    int argc = 0;
    gchar **argv = g_application_command_line_get_arguments(cmdline, &argc);
    std::string cmd = (argc >= 2 && argv && argv[1]) ? argv[1] : "games:open";
    handle_shell_command(state, cmd);
    if (argv) g_strfreev(argv);
    return 0;
}

void on_webui_activate(GtkApplication *app, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    if (!g_app) {
        g_app = app;
        ensure_steam_play_preconfig();
        register_webui_actions(app, state);
        g_timeout_add_seconds(3, on_telemetry_tick, state);
    }
    open_games_window(state);
}

#else

void register_shell_actions(GtkApplication *app, AppState *state) {
    auto add_act = [&](const char *name, GCallback cb) {
        GSimpleAction *act = g_simple_action_new(name, nullptr);
        g_signal_connect(act, "activate", cb, state);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(act));
        g_object_unref(act);
    };
    add_act("sync-theme", G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer d) {
        native_shell_sync_theme(static_cast<AppState *>(d));
    }));
    add_act("sync-perf", G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer d) {
        native_shell_sync_perf(static_cast<AppState *>(d));
    }));
    add_act("sync-desktop", G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer d) {
        native_shell_sync_desktop(static_cast<AppState *>(d));
    }));
    add_act("sync-telemetry", G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer d) {
        push_all_telemetry(static_cast<AppState *>(d));
    }));
}

void on_activate(GtkApplication *app, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    g_app = app;
    ensure_steam_play_preconfig();
    register_shell_actions(app, state);

    init_native_shell(app, state);
    init_window_tracker(state);
    start_gamepad_hotkey_listener(state);
    start_clipboard_watcher();

    g_timeout_add_seconds(3, on_telemetry_tick, state);
}

#endif  // SPINEL_HAS_WEBKIT

}  // namespace

#if SPINEL_HAS_WEBKIT
void init_native_shell(GtkApplication *, AppState *) {}
void shutdown_native_shell(AppState *) {}
void native_shell_sync_telemetry(AppState *) { notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "sync-telemetry"); }
void native_shell_sync_windows(AppState *) {}
void native_shell_sync_desktop(AppState *) { notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "sync-desktop"); }
void native_shell_sync_wallpaper(AppState *) { notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "sync-theme"); }
void native_shell_sync_theme(AppState *) { notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "sync-theme"); }
void native_shell_sync_perf(AppState *) { notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "sync-perf"); }
void native_shell_toggle_cc(AppState *) {}
void native_shell_set_cc_visible(AppState *, bool) {}
#endif

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
        std::string term_bin = has_executable("spinel-terminal")
            ? "spinel-terminal"
            : (has_executable("ptyxis") ? "ptyxis" : "foot");
        spawn_argv_logged({term_bin}, "Terminal");
    } else if (cmd == "launch:files") {
        native_shell_set_cc_visible(state, false);
        spawn_argv_logged({"thunar"}, "Files");
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
            spawn_argv_detached({"google-chrome-stable", "--ozone-platform=wayland", "--enable-wayland-ime", "--wayland-text-input-version=3"});
        } else if (run_argv_capture({"which", "chromium"}, tmp_out, rc) && rc == 0) {
            spawn_argv_detached({"chromium", "--ozone-platform=wayland", "--enable-wayland-ime", "--wayland-text-input-version=3"});
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
#if SPINEL_HAS_WEBKIT
    } else if (cmd == "store:refresh") {
        push_desktop_apps(state);
        push_store_catalog(state);
    } else if (cmd.rfind("store:install\n", 0) == 0) {
        start_store_action(state, "install", cmd);
    } else if (cmd.rfind("store:remove\n", 0) == 0) {
        start_store_action(state, "remove", cmd);
    } else if (cmd.rfind("store:sys-update\n", 0) == 0) {
        start_store_action(state, "sys-update", cmd);
    } else if (cmd.rfind("store:mirror-test\n", 0) == 0) {
        start_store_action(state, "mirror-test", cmd);
    } else if (cmd.rfind("store:keyring-fix\n", 0) == 0) {
        start_store_action(state, "keyring-fix", cmd);
    } else if (cmd.rfind("store:launch:", 0) == 0) {
        native_shell_set_cc_visible(state, false);
        launch_store_app(state, cmd.substr(13));
#endif
    } else if (cmd == "installer:open") {
        native_shell_set_cc_visible(state, false);
        open_installer_window(state);
    } else if (cmd.rfind("settings:open:", 0) == 0) {
        std::string tab = cmd.substr(14);
        if (tab == "wifi" || tab == "bt" || tab == "display" || tab == "audio" || tab == "shortcuts" ||
            tab == "perf" || tab == "appearance" || tab == "perms" || tab == "procs" || tab == "logs") {
            native_shell_set_cc_visible(state, false);
            open_settings_window(state, tab);
        }
    } else if (cmd == "logs:native") {
        native_shell_set_cc_visible(state, false);
        open_native_log_window(state);
    } else if (cmd == "cc:toggle") {
        native_shell_toggle_cc(state);
    } else if (cmd == "cc:close") {
        native_shell_set_cc_visible(state, false);
    } else if (cmd.rfind("system:audio-set:", 0) == 0) {
        int pct = std::clamp(std::atoi(cmd.substr(17).c_str()), 0, 100);
        spawn_argv_detached({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", std::to_string(pct) + "%"});
    } else if (cmd.rfind("system:brightness-set:", 0) == 0) {
        int pct = std::clamp(std::atoi(cmd.substr(22).c_str()), 5, 100);
        if (has_executable("brightnessctl")) {
            spawn_argv_detached({"brightnessctl", "-q", "set", std::to_string(pct) + "%"});
        } else {
            spawn_argv_detached({
                "sh", "-c",
                "for d in /sys/class/backlight/*; do "
                "[ -w \"$d/brightness\" ] && [ -r \"$d/max_brightness\" ] || continue; "
                "max=$(cat \"$d/max_brightness\"); "
                "echo $(( max * " + std::to_string(pct) + " / 100 )) > \"$d/brightness\"; break; done"
            });
        }
    } else if (cmd == "system:lock") {
        native_shell_set_cc_visible(state, false);
        spawn_argv_detached({has_executable("spinel-lock") ? "spinel-lock" : "swaylock"});
    } else if (cmd == "system:suspend") {
        native_shell_set_cc_visible(state, false);
        spawn_argv_detached({"sh", "-c", "(spinel-lock || true); systemctl suspend"});
    } else if (cmd.rfind("system:screenshot:", 0) == 0) {
        std::string smode = cmd.substr(18) == "full" ? "full" : "area";
        native_shell_set_cc_visible(state, false);
        auto *shot_arg = new std::string(smode);
        g_timeout_add(180, [](gpointer d) -> gboolean {
            std::unique_ptr<std::string> m(static_cast<std::string *>(d));
            if (has_executable("spinel-screenshot")) {
                spawn_argv_detached({"spinel-screenshot", *m});
            } else if (has_executable("grim")) {
                spawn_argv_detached({"grim"});
            }
            return G_SOURCE_REMOVE;
        }, shot_arg);
    } else if (cmd.rfind("system:power-profile:", 0) == 0) {
        set_power_profile(state, cmd.substr(21));
    } else if (cmd == "settings:audio-get") {
        push_audio_power_state(state);
    } else if (cmd.rfind("settings:audio-sink:", 0) == 0) {
        set_audio_default_node(state, std::atoi(cmd.substr(20).c_str()), true);
    } else if (cmd.rfind("settings:audio-source:", 0) == 0) {
        set_audio_default_node(state, std::atoi(cmd.substr(22).c_str()), false);
    } else if (cmd.rfind("settings:mic-vol:", 0) == 0) {
        set_mic_volume(state, std::atoi(cmd.substr(17).c_str()));
    } else if (cmd == "settings:mic-mute") {
        toggle_mic_mute(state);
    } else if (cmd == "settings:shortcuts-get") {
        push_shortcuts_config(state);
    } else if (cmd.rfind("settings:shortcuts-set\n", 0) == 0) {
        update_shortcuts_config(state, cmd);
    } else if (cmd == "settings:shortcuts-reset") {
        reset_shortcuts_config(state);
#if !SPINEL_HAS_WEBKIT
    } else if (cmd.rfind("window:toggle:", 0) == 0) {
        uint64_t wid = std::strtoull(cmd.substr(14).c_str(), nullptr, 10);
        if (wid > 0) toggle_or_activate_window(wid);
    } else if (cmd.rfind("window:close:", 0) == 0) {
        uint64_t wid = std::strtoull(cmd.substr(13).c_str(), nullptr, 10);
        if (wid > 0) close_tracked_window(wid);
#endif
    } else if (cmd.rfind("wallpaper:set:", 0) == 0) {
        std::string wp = cmd.substr(14);
        if (wp == "crystal-blue") wp = "crystal-aurora";
        else if (wp == "obsidian-purple") wp = "obsidian-neon";
        else if (wp == "emerald-aurora") wp = "emerald-nordic";
        set_active_theme(state, wp);
#if !SPINEL_HAS_WEBKIT
        notify_dbus_action("org.spinelos.webui", "/org/spinelos/webui", "sync-theme");
#endif
    } else if (cmd.rfind("theme:set:", 0) == 0) {
        set_active_theme(state, cmd.substr(10));
#if !SPINEL_HAS_WEBKIT
        notify_dbus_action("org.spinelos.webui", "/org/spinelos/webui", "sync-theme");
#endif
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
        confirm_power_action(state, false);
    } else if (cmd == "power:shutdown") {
        confirm_power_action(state, true);
    } else if (cmd == "telemetry:refresh") {
        push_all_telemetry(state);
        push_theme_state(state);
        push_perf_config(state);
        push_steam_library(state);
        push_desktop_apps(state);
        push_wallpaper_pref(state);
#if !SPINEL_HAS_WEBKIT
        push_windows_to_ui(state);
#endif
    } else if (cmd == "perf:get") {
        push_perf_config(state);
    } else if (cmd == "perf:toggle-gamemode") {
        PerfConfig cfg = load_perf_config();
        update_perf_config(state, std::string("gamemode=") + (cfg.gamemode ? "false" : "true"));
#if !SPINEL_HAS_WEBKIT
        notify_dbus_action("org.spinelos.webui", "/org/spinelos/webui", "sync-perf");
#endif
    } else if (cmd == "perf:toggle-fsr") {
        PerfConfig cfg = load_perf_config();
        update_perf_config(state, std::string("fsr=") + (cfg.fsr ? "false" : "true"));
#if !SPINEL_HAS_WEBKIT
        notify_dbus_action("org.spinelos.webui", "/org/spinelos/webui", "sync-perf");
#endif
    } else if (cmd == "perf:cycle-mangohud") {
        PerfConfig cfg = load_perf_config();
        std::string next = cfg.mangohud == "off" ? "compact" : (cfg.mangohud == "compact" ? "full" : "off");
        update_perf_config(state, "mangohud=" + next);
#if !SPINEL_HAS_WEBKIT
        notify_dbus_action("org.spinelos.webui", "/org/spinelos/webui", "sync-perf");
#endif
    } else if (cmd.rfind("perf:set\n", 0) == 0) {
        update_perf_config(state, cmd);
    } else if (cmd == "perf:apply-steam") {
        PerfConfig cfg = load_perf_config();
        SteamPerfApplyResult sr = apply_perf_to_steam_configs(cfg);
        std::string msg;
        bool ok = true;
        if (sr.steam_running) {
            ok = false;
            msg = "检测到 Steam 正在运行中（退出时 Steam 会覆盖本地配置）。请先完全退出 Steam 后再点击「写入全部 Steam 游戏」。";
        } else if (sr.games_updated > 0) {
            msg = "已成功将当前性能参数直接写入 " + std::to_string(sr.games_updated) + " 款已安装 Steam 游戏的启动配置！";
        } else {
            msg = "当前尚未检测到本地已安装的 Steam 游戏；系统全局预设与 MangoHud.conf 已准备就绪。";
        }
        eval_js(state->settings_view,
                "window.__spinelPerfFeedback && window.__spinelPerfFeedback(" +
                    std::string(ok ? "true" : "false") + ",\"" + json_escape(msg) + "\");");
    } else if (cmd == "settings:wifi-list") {
        push_wifi_state(state, false);
        push_proxy_config(state);
    } else if (cmd == "settings:proxy-get") {
        push_proxy_config(state);
    } else if (cmd.rfind("settings:proxy-save\n", 0) == 0) {
        update_proxy_config(state, cmd);
    } else if (cmd == "settings:proxy-clear") {
        update_proxy_config(state, "enabled=false");
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
    } else if (cmd.rfind("settings:logs:", 0) == 0) {
        push_system_logs(state, cmd.substr(14));
    } else if (cmd.rfind("settings:logs-clear:", 0) == 0) {
        clear_system_logs(state, cmd.substr(20));
#if SPINEL_HAS_WEBKIT
    } else if (cmd == "installer:scan-disks") {
        push_installer_env(state);
#endif
    } else if (cmd == "ime:status") {
        push_ime_state(state);
    } else if (cmd == "ime:mode:pinyin") {
        set_ime_engine(state, "pinyin");
    } else if (cmd == "ime:mode:rime") {
        set_ime_engine(state, "rime");
    } else if (cmd == "ime:install-rime-ice") {
        install_rime_ice_dict(state);
    } else if (cmd == "ime:restart") {
        restart_fcitx5_daemon(state);
    } else if (cmd == "ime:config") {
        spawn_argv_logged({"fcitx5-configtool"}, "IME-Config");
    } else if (cmd == "settings:perms-get") {
        push_perms_state(state);
    } else if (cmd == "settings:perms-polkit") {
        restart_polkit_agent(state);
    } else if (cmd == "settings:perms-portal") {
        reload_desktop_portals(state);
    } else if (cmd.rfind("perms:set\n", 0) == 0) {
        apply_flatpak_perm(state, cmd);
    } else if (cmd.rfind("perms:reset:", 0) == 0) {
        reset_flatpak_app_perms(state, cmd.substr(12));
    } else if (cmd.rfind("perms:portal-reset\n", 0) == 0) {
        reset_portal_perm(state, cmd);
    } else if (cmd == "settings:procs-get") {
        push_process_list(state);
    } else if (cmd.rfind("procs:kill:", 0) == 0) {
        kill_process_by_pid(state, std::atoi(cmd.substr(11).c_str()), false);
    } else if (cmd.rfind("procs:force:", 0) == 0) {
        kill_process_by_pid(state, std::atoi(cmd.substr(12).c_str()), true);
    } else if (cmd.rfind("clip:copy:", 0) == 0) {
        size_t idx = static_cast<size_t>(std::strtoul(cmd.substr(10).c_str(), nullptr, 10));
        auto items = load_clipboard_history();
        if (idx < items.size()) {
            copy_text_to_clipboard(items[idx].text);
        }
    } else if (cmd == "clip:clear") {
        clear_clipboard_history();
#if SPINEL_HAS_WEBKIT
    } else if (cmd.rfind("installer:start\n", 0) == 0) {
        start_disk_install(state, cmd);
#endif
    }
}

int main(int argc, char **argv) {
#if !SPINEL_HAS_WEBKIT
    if (argc > 1 && std::string(argv[1]) == "--clip-hook") {
        return run_clipboard_hook_from_stdin();
    }
#endif
    ensure_display_env();
    const char *wlr = std::getenv("WLR_RENDERER");
    bool is_pixman = (wlr && std::string(wlr) == "pixman");
#if !SPINEL_HAS_WEBKIT
    // Native GTK4/Cairo shell avoids Mesa/LLVMpipe/GL shader bloat entirely
    setenv("GSK_RENDERER", "cairo", 0);
    (void)is_pixman;
#else
    if (is_pixman) {
        setenv("GSK_RENDERER", "cairo", 0);
        setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
        setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 0);
    } else if (collect_telemetry().low_fx) {
        setenv("GSK_RENDERER", "cairo", 0);
        setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
        setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 0);
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);
    }
#endif

    sync_process_proxy_env();

#if SPINEL_HAS_WEBKIT
    AppState state;
    GtkApplication *app = gtk_application_new("org.spinelos.webui", G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "command-line", G_CALLBACK(on_webui_command_line), &state);
    g_signal_connect(app, "activate", G_CALLBACK(on_webui_activate), &state);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    state.shutting_down.store(true);
    if (state.install_thread.joinable()) {
        state.install_thread.join();
    }
    if (state.store_thread.joinable()) {
        state.store_thread.join();
    }
    g_object_unref(app);
    return status;
#else
    ThemeSpec startup_theme = get_active_theme();
    sync_labwc_window_theme(startup_theme.is_light);
    ensure_fcitx5_defaults(startup_theme.is_light);
    ensure_terminal_defaults(startup_theme.is_light);
    sync_notification_theme(startup_theme.is_light);

    AppState state;
    GtkApplication *app = gtk_application_new("org.spinelos.shell", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &state);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    state.shutting_down.store(true);
    stop_clipboard_watcher();
    stop_gamepad_hotkey_listener();
    shutdown_window_tracker();
    shutdown_native_shell(&state);
    g_object_unref(app);
    return status;
#endif
}
