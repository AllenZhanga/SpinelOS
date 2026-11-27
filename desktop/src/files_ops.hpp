#pragma once

#include <ctime>
#include <filesystem>
#include <string>

namespace spinel::files {

std::string to_lower_copy(std::string s);
std::string format_size(std::uintmax_t bytes);
std::string format_time(std::time_t t);
std::string format_perms(std::filesystem::perms p);
bool is_archive_file(const std::filesystem::path &p);
std::string strip_archive_ext(const std::string &fname);
bool is_script_or_appimage(const std::filesystem::path &p);
std::string sanitize_app_slug(const std::string &raw);
std::string guess_friendly_app_name(const std::filesystem::path &target);
std::string sanitize_url_scheme(const std::string &raw);

}  // namespace spinel::files
