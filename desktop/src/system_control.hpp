#pragma once

#include "app_state.hpp"

#include <cstdint>
#include <string>
#include <vector>

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
    int battery_pct = -1;
    bool battery_charging = false;
    bool is_live = false;
    bool low_fx = true;
};

struct MediaSnapshot {
    int volume = 75;
    bool volume_muted = false;
    int brightness = -1;
    int battery_pct = -1;
    bool battery_charging = false;
    std::string power_profile;
};

struct ProxyConfig {
    bool enabled = false;
    std::string http_proxy;
    std::string socks_proxy;
    std::string no_proxy = "localhost,127.0.0.1,::1,192.168.0.0/16,10.0.0.0/8";
};

ProxyConfig load_proxy_config();
ProxyConfig resolve_effective_proxy();
std::vector<std::pair<std::string, std::string>> build_effective_proxy_envs();
void launch_browser_command(std::vector<std::string> args, const std::string &tag);
void sync_process_proxy_env();
void push_proxy_config(AppState *state);
void update_proxy_config(AppState *state, const std::string &payload);

PerfConfig load_perf_config();
void sync_mangohud_config(const PerfConfig &cfg);
void push_perf_config(AppState *state);
void update_perf_config(AppState *state, const std::string &payload);

TelemetrySnapshot collect_telemetry();
MediaSnapshot collect_media_state();
void push_telemetry_to_view(WebKitWebView *view);
void push_cc_media_state(AppState *state);

void push_wifi_state(AppState *state, bool rescan);
void connect_wifi(AppState *state, const std::string &payload);
void disconnect_iface(AppState *state, const std::string &iface);
bool is_valid_ipv4_addr(const std::string &ip);
bool parse_ipv4_mask_prefix(const std::string &mask_or_prefix, int &prefix_out);
void apply_iface_ip_config(AppState *state, const std::string &payload);

void push_bt_state(AppState *state, bool do_scan);
void set_bt_power(AppState *state, bool on);
void bt_device_action(AppState *state, const std::string &action, const std::string &mac);

void push_display_state(AppState *state);
void apply_display_mode(AppState *state, const std::string &payload);

void push_system_logs(AppState *state, const std::string &source);
void clear_system_logs(AppState *state, const std::string &source);
void open_native_log_window(AppState *state, const std::string &banner = "");

struct DoctorCheckItem {
    std::string id;
    std::string name;
    std::string status;  // ok | warn | fail
    std::string detail;
    std::string fix_hint;
};

struct GpuDriverUpdateInfo {
    std::string driver_summary;
    std::string kernel_running;
    std::string kernel_installed;
    bool reboot_required = false;
    int updates_total = 0;
    std::vector<std::string> gpu_updates;
};

GpuDriverUpdateInfo collect_gpu_driver_update_info();
std::vector<DoctorCheckItem> run_system_health_checks();
std::string redact_sensitive_text(const std::string &raw);
std::string build_redacted_diagnostic_report();
void push_doctor_report(AppState *state);
void export_diagnostic_report(AppState *state);

void ensure_fcitx5_defaults(bool is_light);
void sync_fcitx5_theme(bool is_light);
void ensure_terminal_defaults(bool is_light);
void sync_terminal_theme(bool is_light);
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

struct ShortcutConfig {
    std::string screenshot_area = "C-S-4";
    std::string screenshot_full = "Print";
    std::string lock_screen = "W-l";
    std::string open_terminal = "W-Return";
};

ShortcutConfig load_shortcuts_config();
bool write_labwc_rc(bool is_light, const ShortcutConfig &sc);
void push_shortcuts_config(AppState *state);
void update_shortcuts_config(AppState *state, const std::string &payload);
void reset_shortcuts_config(AppState *state);

void push_process_list(AppState *state);
void cleanup_monitor_memory(AppState *state);
void kill_process_by_pid(AppState *state, int pid, bool force);

bool is_valid_block_dev_path(const std::string &path);
bool is_valid_audio_node_name(const std::string &node);
bool is_valid_mnt_leaf(const std::string &leaf);
std::string unescape_lsblk_value(const std::string &raw);
void push_devices_state(AppState *state);
void mount_block_device(AppState *state, const std::string &dev_path, bool read_only = false);
void unmount_block_device(AppState *state, const std::string &dev_path);
void eject_block_device(AppState *state, const std::string &disk_path);
void open_mount_folder(AppState *state, const std::string &mount_path);
void set_block_fstab_mount(AppState *state, const std::string &payload);
void remove_block_fstab_mount(AppState *state, const std::string &payload);
void disable_audio_device(AppState *state, const std::string &payload);
void enable_audio_device(AppState *state, const std::string &node_name);

void sync_notification_theme(bool is_light);
bool get_dnd_enabled();
void toggle_dnd_mode(AppState *state);

struct NotificationHistoryItem {
    uint32_t id = 0;
    std::string app_name;
    std::string summary;
    std::string body;
};
std::vector<NotificationHistoryItem> fetch_notification_history();
void dismiss_all_notifications();

bool is_welcome_completed();
void mark_welcome_completed();
void push_welcome_status(AppState *state);
void push_audio_power_state(AppState *state);
void set_audio_default_node(AppState *state, int id, bool is_sink);
void set_mic_volume(AppState *state, int pct);
void toggle_mic_mute(AppState *state);
bool is_valid_audio_test_channel(const std::string &ch);
std::vector<uint8_t> build_speaker_test_wav(const std::string &ch, int sample_rate = 48000);
void test_audio_speaker(AppState *state, const std::string &ch);
void toggle_mic_test(AppState *state, const std::string &mode = "toggle");
void stop_audio_tests(AppState *state);
void set_power_profile(AppState *state, const std::string &profile);
bool get_idle_lock_enabled();
void set_idle_lock_enabled(AppState *state, bool enabled);
bool set_system_brightness_pct(int pct);
void trigger_media_osd(AppState *state, const std::string &op);
void start_gamepad_hotkey_listener(AppState *state);
void stop_gamepad_hotkey_listener();

struct ClipboardItem {
    std::string text;
    long long ts = 0;
};

int run_clipboard_hook_from_stdin();
std::vector<ClipboardItem> load_clipboard_history();
void clear_clipboard_history();
void copy_text_to_clipboard(const std::string &text);
void start_clipboard_watcher();
void stop_clipboard_watcher();

// Dock behavior mode ("magnify" | "fixed" | "autohide")
struct DockConfig {
    std::string mode = "magnify";
};
bool is_valid_dock_mode(const std::string &mode);
DockConfig load_dock_config();
bool save_dock_config(const DockConfig &cfg);
void push_dock_config(AppState *state);
void set_dock_mode(AppState *state, const std::string &mode);

// Btrfs Snapper system snapshots
struct SnapperSnapshotInfo {
    int id = 0;
    std::string type;
    std::string date;
    std::string user;
    std::string cleanup;
    std::string description;
    bool is_current = false;
};
bool is_valid_snapshot_id(const std::string &s, int &out_id);
bool is_valid_snapper_desc(const std::string &desc);
std::vector<SnapperSnapshotInfo> parse_snapper_csv(const std::string &csv);
void push_snapshots_state(AppState *state);
void create_system_snapshot(AppState *state, const std::string &payload);
void rollback_system_snapshot(AppState *state, const std::string &payload);
void delete_system_snapshot(AppState *state, const std::string &payload);
