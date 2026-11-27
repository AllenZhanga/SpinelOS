#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace spinel::files {

std::string to_lower_copy(std::string s);
std::string format_size(std::uintmax_t bytes);
std::string format_time(std::time_t t);
std::string format_perms(std::filesystem::perms p);
bool is_archive_file(const std::filesystem::path &p);
std::string strip_archive_ext(const std::string &fname);
bool is_script_or_appimage(const std::filesystem::path &p);
bool is_image_file(const std::filesystem::path &p);
bool is_text_previewable_file(const std::filesystem::path &p);
std::string read_text_preview(const std::filesystem::path &p,
                              std::size_t max_bytes,
                              bool &out_truncated,
                              bool &out_is_binary);
std::string encode_drag_paths(const std::vector<std::filesystem::path> &paths);
std::vector<std::filesystem::path> decode_drag_paths(const std::string &payload);
bool is_same_or_subpath(const std::filesystem::path &candidate, const std::filesystem::path &base);
bool move_or_copy_paths_to_dir(const std::vector<std::filesystem::path> &sources,
                               const std::filesystem::path &target_dir,
                               bool copy_mode,
                               std::vector<std::filesystem::path> &out_results,
                               std::string &out_error);
std::string sanitize_app_slug(const std::string &raw);
std::string guess_friendly_app_name(const std::filesystem::path &target);
std::string sanitize_url_scheme(const std::string &raw);

}  // namespace spinel::files
