#pragma once

#include <gtk/gtk.h>
#include <webkit/webkit.h>

#include <sys/types.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

struct NativeShellState;

struct AppState {
    // Pure Native GTK4/Cairo Resident Shell (zero WebKit processes at idle)
    NativeShellState *native = nullptr;

    // Legacy pointers retained as nullptr for callers / optional views
    WebKitWebView *bar_view = nullptr;
    WebKitWebView *desktop_view = nullptr;
    WebKitWebView *dock_view = nullptr;
    GtkWindow *dock_window = nullptr;
    WebKitWebView *cc_view = nullptr;
    GtkWindow *cc_window = nullptr;

    // On-Demand Rich-Content WebKitGTK6 Application Windows (destroyed on close)
    GtkWindow *games_win = nullptr;
    WebKitWebView *games_view = nullptr;

    GtkWindow *settings_win = nullptr;
    WebKitWebView *settings_view = nullptr;

    GtkWindow *installer_win = nullptr;
    WebKitWebView *installer_view = nullptr;

    GtkWindow *store_win = nullptr;
    WebKitWebView *store_view = nullptr;

    std::atomic<bool> install_running{false};
    std::atomic<bool> store_busy{false};
    std::atomic<bool> shutting_down{false};
    std::atomic<pid_t> install_pid{-1};
    std::thread install_thread;
    std::thread store_thread;

    std::atomic<bool> bt_busy{false};
    std::atomic<bool> wifi_busy{false};
};

inline void on_eval_js_done(GObject *source, GAsyncResult *res, gpointer) {
    GError *err = nullptr;
    if (!source || !WEBKIT_IS_WEB_VIEW(source)) return;
    JSCValue *val = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), res, &err);
    if (val) g_object_unref(val);
    if (err) {
        std::fprintf(stderr, "[spinel-shell] eval_js error: %s\n", err->message ? err->message : "unknown");
        g_error_free(err);
    }
    if (GTK_IS_WIDGET(source) && gtk_widget_get_realized(GTK_WIDGET(source))) {
        gtk_widget_queue_draw(GTK_WIDGET(source));
    }
}

inline void eval_js(WebKitWebView *view, const std::string &script) {
    if (!view) return;
    webkit_web_view_evaluate_javascript(
        view, script.c_str(), -1, nullptr, nullptr, nullptr, on_eval_js_done, nullptr
    );
}

void handle_shell_command(AppState *state, const std::string &cmd);
