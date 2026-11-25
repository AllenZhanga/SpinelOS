#pragma once

#include <string>
#include <vector>

std::string json_escape(const std::string &input);
std::string trim_copy(const std::string &s);
std::string home_dir();
void ensure_display_env();
std::string spinel_state_dir();
std::string read_first_line(const std::string &path, const std::string &fallback = "");
bool write_text_file(const std::string &path, const std::string &content);
std::string run_cmd_capture(const char *cmd);
bool run_argv_capture(const std::vector<std::string> &args, std::string &out, int &exit_code);
bool run_argv_stdin_capture(const std::vector<std::string> &args,
                            const std::string &stdin_data,
                            std::string &out,
                            int &exit_code);
bool has_executable(const std::string &cmd);
void append_app_log(const std::string &line);
void spawn_detached(const std::string &cmd);
void spawn_argv_detached(const std::vector<std::string> &args,
                         const std::vector<std::pair<std::string, std::string>> &env_overrides = {});
void spawn_argv_logged(const std::vector<std::string> &args,
                       const std::string &tag,
                       const std::vector<std::pair<std::string, std::string>> &env_overrides = {});
bool is_live_iso_env();
std::vector<std::string> split_lines(const std::string &text);
