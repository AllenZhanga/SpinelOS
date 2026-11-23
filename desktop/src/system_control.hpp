#pragma once

#include "app_state.hpp"

#include <string>

struct PerfConfig {
    std::string mangohud = "compact";  // off | compact | full
    int fps_limit = 0;                 // 0 | 60 | 90 | 120 | 144
    bool fsr = false;
    bool gamemode = true;
    std::string render_res = "native"; // native | 720p | 900p | 1080p
};

PerfConfig load_perf_config();
void push_perf_config(AppState *state);
void update_perf_config(AppState *state, const std::string &payload);

void push_telemetry_to_view(WebKitWebView *view);

void push_wifi_state(AppState *state, bool rescan);
void connect_wifi(AppState *state, const std::string &payload);
void disconnect_iface(AppState *state, const std::string &iface);

void push_bt_state(AppState *state, bool do_scan);
void set_bt_power(AppState *state, bool on);
void bt_device_action(AppState *state, const std::string &action, const std::string &mac);

void push_display_state(AppState *state);
void apply_display_mode(AppState *state, const std::string &payload);
