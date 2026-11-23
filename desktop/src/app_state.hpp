#pragma once

#include <gtk/gtk.h>
#include <webkit/webkit.h>

#include <sys/types.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

struct AppState {
    WebKitWebView *bar_view = nullptr;
    WebKitWebView *hub_view = nullptr;
    GtkWindow *hub_window = nullptr;
    bool hub_visible = true;

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
