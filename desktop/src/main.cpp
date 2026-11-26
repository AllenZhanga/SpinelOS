#include "app_state.hpp"
#include "common.hpp"
#include "desktop_apps.hpp"
#include "ipc_router.hpp"
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

void notify_dbus_action(const char *bus_name,
                        const char *obj_path,
                        const char *action_name,
                        bool sync = false,
                        const char *str_param = nullptr) {
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus) return;
    GVariantBuilder params;
    g_variant_builder_init(&params, G_VARIANT_TYPE("av"));
    if (str_param) {
        g_variant_builder_add(&params, "v", g_variant_new_string(str_param));
    }
    GVariantBuilder empty_platform;
    g_variant_builder_init(&empty_platform, G_VARIANT_TYPE("a{sv}"));
    GVariant *args = g_variant_new("(s@av@a{sv})",
                                   action_name,
                                   g_variant_builder_end(&params),
                                   g_variant_builder_end(&empty_platform));
    if (sync) {
        GVariant *res = g_dbus_connection_call_sync(
            bus, bus_name, obj_path, "org.gtk.Actions", "Activate",
            args, nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, 600, nullptr, nullptr);
        if (res) g_variant_unref(res);
    } else {
        g_dbus_connection_call(
            bus, bus_name, obj_path, "org.gtk.Actions", "Activate",
            args, nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, nullptr, nullptr, nullptr);
    }
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
        if (ctx->state && ctx->view_slot == &ctx->state->settings_view) {
            stop_audio_tests(ctx->state);
        }
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
                      "Spinel OS 游戏与启动中心", "?mode=games", 1040, 680);
    push_theme_state(state);
    push_perf_config(state);
    push_steam_library(state);
    push_desktop_apps(state);
    push_storage_caches(state);
}

void open_settings_window(AppState *state, const std::string &tab) {
    bool existed = (state->settings_win != nullptr);
    ensure_app_window(state, &state->settings_win, &state->settings_view,
                      "Spinel OS 系统设置", "?mode=settings&tab=" + tab, 960, 660);
    if (existed) {
        eval_js(state->settings_view, "window.__spinelOpenSettings && window.__spinelOpenSettings('" + tab + "');");
    }
    push_theme_state(state);
    push_wallpaper_pref(state);
}

void open_installer_window(AppState *state) {
    bool existed = (state->installer_win != nullptr);
    ensure_app_window(state, &state->installer_win, &state->installer_view,
                      "安装 Spinel OS 到硬盘", "?mode=installer", 860, 660);
    if (existed) {
        push_theme_state(state);
        push_installer_env(state);
    }
}

void open_store_window(AppState *state, const std::string &focus_id = "") {
    bool existed = (state->store_win != nullptr);
    std::string query = focus_id.empty() ? "?mode=store" : ("?mode=store&focus=" + focus_id);
    ensure_app_window(state, &state->store_win, &state->store_view,
                      "Spinel OS 应用商店", query, 1020, 680);
    if (existed) {
        if (!focus_id.empty()) {
            eval_js(state->store_view, "window.__spinelFocusStoreApp && window.__spinelFocusStoreApp('" + focus_id + "');");
        }
        push_theme_state(state);
        push_desktop_apps(state);
        push_store_catalog(state);
    }
}

void open_welcome_window(AppState *state) {
    bool existed = (state->welcome_win != nullptr);
    ensure_app_window(state, &state->welcome_win, &state->welcome_view,
                      "欢迎使用 Spinel OS", "?mode=welcome", 820, 620);
    if (existed) {
        push_theme_state(state);
        push_telemetry_to_view(state->welcome_view);
        push_desktop_apps(state);
        push_welcome_status(state);
    }
}

void open_doctor_window(AppState *state) {
    bool existed = (state->doctor_win != nullptr);
    ensure_app_window(state, &state->doctor_win, &state->doctor_view,
                      "Spinel Doctor · 系统全链路健康体检", "?mode=doctor", 880, 640);
    if (existed) {
        push_theme_state(state);
        push_doctor_report(state);
    }
}

void open_monitor_window(AppState *state, const std::string &tab) {
    std::string clean_tab = (tab == "logs") ? "logs" : "procs";
    bool existed = (state->monitor_win != nullptr);
    ensure_app_window(state, &state->monitor_win, &state->monitor_view,
                      "系统监视器 · Spinel Monitor", "?mode=monitor&tab=" + clean_tab, 1020, 680);
    if (existed) {
        eval_js(state->monitor_view,
                "window.__spinelOpenMonitor && window.__spinelOpenMonitor('" + clean_tab + "');");
    }
    push_theme_state(state);
    push_telemetry_to_view(state->monitor_view);
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
void open_store_window(AppState *, const std::string &focus_id = "") {
    launch_webui_cmd(focus_id.empty() ? "store:open" : ("store:open:" + focus_id));
}
void open_installer_window(AppState *) { launch_webui_cmd("installer:open"); }
void open_settings_window(AppState *, const std::string &tab) {
    launch_webui_cmd("settings:open:" + tab);
}
void open_welcome_window(AppState *) { launch_webui_cmd("welcome:open"); }
void open_doctor_window(AppState *) { launch_webui_cmd("doctor:open"); }
void open_monitor_window(AppState *, const std::string &tab) {
    launch_webui_cmd("monitor:open:" + (tab == "logs" ? std::string("logs") : std::string("procs")));
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
    if (state->welcome_win && gtk_widget_get_visible(GTK_WIDGET(state->welcome_win))) {
        push_telemetry_to_view(state->welcome_view);
        push_welcome_status(state);
    }
    if (state->doctor_win && gtk_widget_get_visible(GTK_WIDGET(state->doctor_win))) {
        push_telemetry_to_view(state->doctor_view);
    }
    if (state->monitor_win && gtk_widget_get_visible(GTK_WIDGET(state->monitor_win))) {
        push_telemetry_to_view(state->monitor_view);
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
        "%s", is_shutdown ? "确定要关闭 Spinel OS 电源吗？" : "确定要立即重启系统吗？"
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
    add_act("sync-dock", G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer d) {
        native_shell_sync_dock(static_cast<AppState *>(d));
    }));
    add_act("sync-perf", G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer d) {
        native_shell_sync_perf(static_cast<AppState *>(d));
    }));
    add_act("sync-desktop", G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer d) {
        auto *state = static_cast<AppState *>(d);
        native_shell_sync_desktop(state);
        native_shell_sync_windows(state);
    }));
    add_act("sync-telemetry", G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer d) {
        push_all_telemetry(static_cast<AppState *>(d));
    }));
    add_act("toggle-launcher", G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer d) {
        native_shell_toggle_launcher(static_cast<AppState *>(d));
    }));
    GSimpleAction *switcher_act = g_simple_action_new("switcher", G_VARIANT_TYPE_STRING);
    g_signal_connect(switcher_act, "activate", G_CALLBACK(+[](GSimpleAction *, GVariant *param, gpointer d) {
        const char *dir = param ? g_variant_get_string(param, nullptr) : nullptr;
        bool prev = (dir && std::string(dir) == "prev");
        native_shell_step_switcher(static_cast<AppState *>(d), prev);
    }), state);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(switcher_act));
    g_object_unref(switcher_act);
    GSimpleAction *osd_act = g_simple_action_new("osd", G_VARIANT_TYPE_STRING);
    g_signal_connect(osd_act, "activate", G_CALLBACK(+[](GSimpleAction *, GVariant *param, gpointer d) {
        const char *op = param ? g_variant_get_string(param, nullptr) : nullptr;
        if (op && *op) {
            trigger_media_osd(static_cast<AppState *>(d), op);
        }
    }), state);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(osd_act));
    g_object_unref(osd_act);
    GSimpleAction *sandbox_act = g_simple_action_new("sync-sandbox", G_VARIANT_TYPE_STRING);
    g_signal_connect(sandbox_act, "activate", G_CALLBACK(+[](GSimpleAction *, GVariant *param, gpointer d) {
        const char *act = param ? g_variant_get_string(param, nullptr) : nullptr;
        native_shell_sync_sandbox(static_cast<AppState *>(d), (act && *act) ? act : "sync");
    }), state);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(sandbox_act));
    g_object_unref(sandbox_act);
}

void on_activate(GtkApplication *app, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    g_app = app;
    ensure_steam_play_preconfig();
    register_shell_actions(app, state);

    if (state->sandbox_shell_mode) {
        init_native_shell(app, state);
        init_window_tracker(state);
        g_timeout_add_seconds(3, on_telemetry_tick, state);
        return;
    }

    init_native_shell(app, state);
    init_window_tracker(state);
    start_gamepad_hotkey_listener(state);
    start_clipboard_watcher();

    g_timeout_add_seconds(3, on_telemetry_tick, state);
    if (is_live_iso_env() || !is_welcome_completed()) {
        g_timeout_add(1800, +[](gpointer d) -> gboolean {
            auto *st = static_cast<AppState *>(d);
            if (!st || st->shutting_down.load()) {
                return G_SOURCE_REMOVE;
            }
            if (is_live_iso_env()) {
                launch_webui_cmd("installer:open");
            } else if (!is_welcome_completed()) {
                launch_webui_cmd("welcome:open");
            }
            return G_SOURCE_REMOVE;
        }, state);
    }
}

#endif  // SPINEL_HAS_WEBKIT

}  // namespace

#if SPINEL_HAS_WEBKIT
void init_native_shell(GtkApplication *, AppState *) {}
void shutdown_native_shell(AppState *) {}
void native_shell_sync_telemetry(AppState *) { notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "sync-telemetry"); }
void native_shell_sync_windows(AppState *) {}
void native_shell_sync_workspaces(AppState *) {}
void native_shell_sync_desktop(AppState *) { notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "sync-desktop"); }
void native_shell_sync_wallpaper(AppState *) { notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "sync-theme"); }
void native_shell_sync_theme(AppState *) { notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "sync-theme"); }
void native_shell_sync_dock(AppState *) { notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "sync-dock"); }
void native_shell_sync_perf(AppState *) { notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "sync-perf"); }
void native_shell_toggle_cc(AppState *) {}
void native_shell_set_cc_visible(AppState *, bool) {}
void native_shell_toggle_launcher(AppState *) { notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "toggle-launcher"); }
void native_shell_sync_sandbox(AppState *, const std::string &action) {
    notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "sync-sandbox", false,
                       action.empty() ? "sync" : action.c_str());
}
void native_shell_step_switcher(AppState *, bool prev) {
    notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "switcher", false, prev ? "prev" : "next");
}
void native_shell_show_osd(AppState *, const std::string &, int, bool) {}
#endif

void handle_shell_command(AppState *state, const std::string &cmd) {
    if (!state) return;

    IpcHostActions actions;
    actions.open_games = open_games_window;
    actions.open_store = open_store_window;
    actions.open_installer = open_installer_window;
    actions.open_settings = open_settings_window;
    actions.open_welcome = open_welcome_window;
    actions.open_doctor = open_doctor_window;
    actions.open_monitor = open_monitor_window;
    actions.push_all_telemetry = push_all_telemetry;
    actions.confirm_power = confirm_power_action;

#if SPINEL_HAS_WEBKIT
    actions.close_welcome = +[](AppState *s) {
        if (s && s->welcome_win) gtk_window_close(s->welcome_win);
    };
    actions.store_refresh = +[](AppState *s) {
        push_desktop_apps(s);
        push_store_catalog(s);
    };
    actions.store_action = start_store_action;
    actions.store_cancel = cancel_store_action;
    actions.store_launch = launch_store_app;
    actions.installer_scan = push_installer_env;
    actions.installer_start = start_disk_install;
    actions.pick_wallpaper = +[](AppState *s) {
        GtkFileDialog *dlg = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dlg, "选择自定义桌面壁纸图片");
        GtkFileFilter *flt = gtk_file_filter_new();
        gtk_file_filter_set_name(flt, "图片文件 (PNG / JPG / WebP)");
        gtk_file_filter_add_mime_type(flt, "image/png");
        gtk_file_filter_add_mime_type(flt, "image/jpeg");
        gtk_file_filter_add_mime_type(flt, "image/webp");
        GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
        g_list_store_append(filters, flt);
        gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
        gtk_file_dialog_set_default_filter(dlg, flt);
        g_object_unref(flt);
        g_object_unref(filters);

        fs::path pic_dir = fs::path(home_dir()) / "Pictures";
        std::error_code ec;
        GFile *init_folder = g_file_new_for_path(
            fs::is_directory(pic_dir, ec) ? pic_dir.c_str() : home_dir().c_str());
        gtk_file_dialog_set_initial_folder(dlg, init_folder);
        g_object_unref(init_folder);

        gtk_file_dialog_open(dlg, s->settings_win, nullptr,
            +[](GObject *src, GAsyncResult *res, gpointer data) {
                auto *st = static_cast<AppState *>(data);
                GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, nullptr);
                if (file) {
                    char *path = g_file_get_path(file);
                    if (path && *path && st && !st->shutting_down.load()) {
                        set_custom_wallpaper_image(st, path);
                    }
                    g_free(path);
                    g_object_unref(file);
                }
            }, s);
        g_object_unref(dlg);
    };
#else
    actions.notify_webui_sync = +[](const char *action) {
        notify_dbus_action("org.spinelos.webui", "/org/spinelos/webui", action);
    };
    actions.push_windows = push_windows_to_ui;
    actions.window_toggle = toggle_or_activate_window;
    actions.window_close = close_tracked_window;
    actions.window_quit = +[](uint64_t wid) { quit_tracked_window_app(wid); };
    actions.workspace_switch = +[](int idx1) { activate_tracked_workspace(idx1 - 1); };
    actions.workspace_step = +[](int delta) { step_tracked_workspace(delta); };
#endif

    dispatch_ipc_command(state, cmd, actions);
}

int main(int argc, char **argv) {
#if !SPINEL_HAS_WEBKIT
    auto in_sandbox_env = []() {
        const char *sb = std::getenv("SPINEL_SANDBOX_SHELL");
        return sb && std::string(sb) == "1";
    };
    if (argc > 1 && std::string(argv[1]) == "--clip-hook") {
        return run_clipboard_hook_from_stdin();
    }
    if (argc > 1 && (std::string(argv[1]) == "--sandbox-launcher" ||
                     (std::string(argv[1]) == "--launcher" && in_sandbox_env()))) {
        notify_dbus_action("org.spinelos.shell.sandbox", "/org/spinelos/shell/sandbox", "toggle-launcher", true);
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--launcher") {
        notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "toggle-launcher", true);
        return 0;
    }
    if (argc > 1 && (std::string(argv[1]) == "--sandbox-switcher" ||
                     (std::string(argv[1]) == "--switcher" && in_sandbox_env()))) {
        const char *dir = (argc > 2 && argv[2] && std::string(argv[2]) == "prev") ? "prev" : "next";
        notify_dbus_action("org.spinelos.shell.sandbox", "/org/spinelos/shell/sandbox", "switcher", true, dir);
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--switcher") {
        const char *dir = (argc > 2 && argv[2] && std::string(argv[2]) == "prev") ? "prev" : "next";
        notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "switcher", true, dir);
        return 0;
    }
    if (argc > 2 && std::string(argv[1]) == "--osd") {
        notify_dbus_action("org.spinelos.shell", "/org/spinelos/shell", "osd", true, argv[2]);
        return 0;
    }
    bool sandbox_mode = in_sandbox_env();
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && std::string(argv[i]) == "--sandbox") {
            sandbox_mode = true;
            break;
        }
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
#if !SPINEL_HAS_WEBKIT
    if (!sandbox_mode) {
        spawn_argv_detached({"dbus-update-activation-environment", "--systemd", "XDG_CURRENT_DESKTOP=SpinelOS:GNOME"});
    }
#endif

#if SPINEL_HAS_WEBKIT
    AppState state;
    GtkApplication *app = gtk_application_new("org.spinelos.webui", G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "command-line", G_CALLBACK(on_webui_command_line), &state);
    g_signal_connect(app, "activate", G_CALLBACK(on_webui_activate), &state);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    state.shutting_down.store(true);
    stop_audio_tests(&state);
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
    if (!sandbox_mode) {
        sync_labwc_window_theme(startup_theme.is_light);
        ensure_fcitx5_defaults(startup_theme.is_light);
    }
    ensure_terminal_defaults(startup_theme.is_light);
    sync_notification_theme(startup_theme.is_light);

    AppState state;
    state.sandbox_shell_mode = sandbox_mode;
    const char *app_id = sandbox_mode ? "org.spinelos.shell.sandbox" : "org.spinelos.shell";
    GtkApplication *app = gtk_application_new(app_id, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &state);
    int status = g_application_run(G_APPLICATION(app), 1, argv);
    state.shutting_down.store(true);
    if (!sandbox_mode) {
        stop_clipboard_watcher();
        stop_gamepad_hotkey_listener();
    }
    shutdown_window_tracker();
    shutdown_native_shell(&state);
    g_object_unref(app);
    return status;
#endif
}
