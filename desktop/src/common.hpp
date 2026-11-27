#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

std::string json_escape(const std::string &input);
std::string trim_copy(const std::string &s);
std::string home_dir();
void ensure_display_env();
std::string spinel_state_dir();
std::string read_first_line(const std::string &path, const std::string &fallback = "");
bool write_text_file(const std::string &path, const std::string &content);
bool restore_trashed_path(const std::filesystem::path &source,
                          std::filesystem::path &target,
                          std::string &error);
std::string run_cmd_capture(const char *cmd);
bool run_argv_capture(const std::vector<std::string> &args, std::string &out, int &exit_code);
bool run_argv_stdin_capture(const std::vector<std::string> &args,
                            const std::string &stdin_data,
                            std::string &out,
                            int &exit_code);
bool has_executable(const std::string &cmd);
void append_app_log(const std::string &line);
void spawn_argv_detached(const std::vector<std::string> &args,
                         const std::vector<std::pair<std::string, std::string>> &env_overrides = {});

/// Launch a tracked background task; skips and returns false if `dedup_key` is already active.
bool launch_async_task(const std::string &dedup_key, std::function<void()> fn);

/// Wait up to `timeout_ms` for tracked background tasks to drain before tearing down app state.
void wait_all_async_tasks(int timeout_ms = 2000);
void spawn_argv_logged(const std::vector<std::string> &args,
                       const std::string &tag,
                       const std::vector<std::pair<std::string, std::string>> &env_overrides = {},
                       const std::string &working_dir = {});
bool is_live_iso_env();
std::vector<std::string> split_lines(const std::string &text);

/// Resolve active proxy fields from ~/.config/spinelos/proxy.conf or GSettings org.gnome.system.proxy.
bool resolve_active_proxy_fields(std::string &out_http, std::string &out_socks, std::string &out_no_proxy);

/// Build active proxy environment variables (http_proxy/https_proxy/HTTP_PROXY/HTTPS_PROXY/all_proxy/ALL_PROXY/no_proxy/NO_PROXY).
std::vector<std::pair<std::string, std::string>> collect_active_proxy_envs();
