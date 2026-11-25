#pragma once

#include "app_state.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace sys_internal {

std::filesystem::path config_dir();
void post_js_to_main(AppState *state, std::string script, bool also_bar = true);
std::string detect_mem_usage(int *used_mb_out = nullptr);
std::string read_tail_lines(const std::filesystem::path &p, size_t max_lines);
std::string collect_logs_bundle_text(const std::string &source);

}  // namespace sys_internal
