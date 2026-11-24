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

struct TelemetrySnapshot {
    std::string gpu;
    std::string mem;
    std::string net;
    std::string kernel;
    int mem_used_mb = 0;
    bool is_live = false;
    bool low_fx = true;
};

struct MediaSnapshot {
    int volume = 75;
    int brightness = -1;
};

struct ProxyConfig {
    bool enabled = false;
    std::string http_proxy;
    std::string socks_proxy;
    std::string no_proxy = "localhost,127.0.0.1,::1,192.168.0.0/16,10.0.0.0/8";
};

ProxyConfig load_proxy_config();
void sync_process_proxy_env();
void push_proxy_config(AppState *state);
void update_proxy_config(AppState *state, const std::string &payload);

PerfConfig load_perf_config();
void push_perf_config(AppState *state);
void update_perf_config(AppState *state, const std::string &payload);

TelemetrySnapshot collect_telemetry();
MediaSnapshot collect_media_state();
void push_telemetry_to_view(WebKitWebView *view);
void push_cc_media_state(AppState *state);

void push_wifi_state(AppState *state, bool rescan);
void connect_wifi(AppState *state, const std::string &payload);
void disconnect_iface(AppState *state, const std::string &iface);

void push_bt_state(AppState *state, bool do_scan);
void set_bt_power(AppState *state, bool on);
void bt_device_action(AppState *state, const std::string &action, const std::string &mac);

void push_display_state(AppState *state);
void apply_display_mode(AppState *state, const std::string &payload);

void push_system_logs(AppState *state, const std::string &source);
void clear_system_logs(AppState *state, const std::string &source);
void open_native_log_window(AppState *state, const std::string &banner = "");

void ensure_fcitx5_defaults(bool is_light);
void sync_fcitx5_theme(bool is_light);
void push_ime_state(AppState *state);
void set_ime_engine(AppState *state, const std::string &engine);
void install_rime_ice_dict(AppState *state);
void restart_fcitx5_daemon(AppState *state);

void push_perms_state(AppState *state);
void apply_flatpak_perm(AppState *state, const std::string &payload);
void reset_flatpak_app_perms(AppState *state, const std::string &app_id);
void reset_portal_perm(AppState *state, const std::string &payload);
void restart_polkit_agent(AppState *state);
void reload_desktop_portals(AppState *state);

void push_process_list(AppState *state);
void kill_process_by_pid(AppState *state, int pid, bool force);
