#pragma once

#include <gtk/gtk.h>
#include <webkit/webkit.h>

#include <sys/types.h>

#include <atomic>
#include <string>
#include <thread>

struct AppState {
    // Persistent Layer-Shell surfaces
    WebKitWebView *bar_view = nullptr;
    WebKitWebView *desktop_view = nullptr;
    WebKitWebView *dock_view = nullptr;
    GtkWindow *dock_window = nullptr;

    // Normal macOS-style managed Application Windows
    GtkWindow *games_win = nullptr;
    WebKitWebView *games_view = nullptr;

    GtkWindow *settings_win = nullptr;
    WebKitWebView *settings_view = nullptr;

    GtkWindow *installer_win = nullptr;
    WebKitWebView *installer_view = nullptr;

    std::atomic<bool> install_running{false};
    std::atomic<bool> shutting_down{false};
    std::atomic<pid_t> install_pid{-1};
    std::thread install_thread;

    std::atomic<bool> bt_busy{false};
    std::atomic<bool> wifi_busy{false};
};

inline void eval_js(WebKitWebView *view, const std::string &script) {
    if (!view) return;
    webkit_web_view_evaluate_javascript(
        view, script.c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr
    );
}
