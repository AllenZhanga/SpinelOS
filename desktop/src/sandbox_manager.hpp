#pragma once

#include <string>
#include <vector>

struct SandboxAppEntry {
    std::string name;
    std::string cmd;
    bool auto_start = false;
};

struct SandboxConfig {
    bool allow_network = true;
    bool sync_clipboard = true;
    std::string perf_mode = "host-priority";  // "host-priority" | "balanced"
    std::string resolution = "1600x900";
    std::vector<std::string> rw_dirs;
    std::vector<std::string> ro_dirs;
    bool apps_configured = false;
    std::vector<SandboxAppEntry> apps;
};

struct SandboxStatus {
    bool bwrap_installed = false;
    bool labwc_installed = false;
    bool running = false;
    int compositor_pid = -1;
    bool socket_ready = false;
    bool clip_sync_running = false;
    std::string runtime_dir;
    std::string wayland_display = "wayland-0";
    std::string home_path;
    long long home_size_mb = 0;
    int app_count = 0;
};

std::string sandbox_config_path();
std::string sandbox_root_dir();
std::string sandbox_home_dir();
std::string sandbox_labwc_dir();
std::string sandbox_runtime_dir();
std::string sandbox_socket_name();

bool is_valid_sandbox_perf_mode(const std::string &mode);
bool is_valid_sandbox_resolution(const std::string &res, int &out_w, int &out_h);
bool is_valid_sandbox_mount_dir(const std::string &raw_path, std::string &normalized_out, std::string &error_out);
void compute_pip_takeover_size(int req_w,
                               int req_h,
                               int surf_w,
                               int surf_h,
                               int mon_w,
                               int mon_h,
                               int &out_w,
                               int &out_h);
bool apply_sandbox_resolution_live(int w, int h);

SandboxConfig load_sandbox_config();
bool save_sandbox_config(const SandboxConfig &cfg);
bool add_sandbox_mount_dir(const std::string &path, bool writable, std::string &error_out);
bool remove_sandbox_mount_dir(const std::string &path);
bool add_sandbox_app(const std::string &name, const std::string &cmd, bool auto_start, std::string &error_out);
bool remove_sandbox_app(const std::string &cmd);
bool set_sandbox_app_autostart(const std::string &cmd, bool auto_start);

std::string build_sandbox_rc_xml(const SandboxConfig &cfg);
std::vector<std::string> build_sandbox_bwrap_argv(const SandboxConfig &cfg,
                                                  const std::string &runtime_dir,
                                                  const std::string &fake_home,
                                                  const std::string &host_home,
                                                  unsigned int uid,
                                                  const std::vector<std::string> &cmd_args,
                                                  const std::string &cwd = "",
                                                  bool die_with_parent = true);
std::vector<std::string> build_sandbox_dbus_bwrap_argv(const SandboxConfig &cfg,
                                                       const std::string &runtime_dir,
                                                       const std::string &fake_home,
                                                       const std::string &host_home,
                                                       unsigned int uid);

SandboxStatus query_sandbox_status();
std::string sandbox_state_json();

bool start_sandbox_session(std::string &error_out);
bool stop_sandbox_session(std::string &error_out);
bool reset_sandbox_home(std::string &error_out);
bool launch_in_sandbox(const std::vector<std::string> &cmd_args,
                       const std::string &workspace_dir,
                       std::string &error_out);

void ensure_sandbox_clipboard_sync(bool enabled);
void stop_sandbox_clipboard_sync();
void ensure_sandbox_shell_running(const SandboxConfig &cfg,
                                  const std::string &rt_dir,
                                  const std::string &fake_home);
int run_sandbox_clipboard_daemon();
int handle_sandbox_clipboard_push(const std::string &side);
