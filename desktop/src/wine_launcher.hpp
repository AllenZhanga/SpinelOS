#pragma once

#include <gio/gio.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct WineShortcut {
    std::string id;
    std::string name;
    std::string icon;
    std::string folder;
    std::filesystem::path link;
    std::filesystem::path working_dir;
};

struct WineInstalledApp {
    std::string key;
    std::string name;
};

bool is_windows_executable(const std::filesystem::path &path);
bool wine_is_available();
GSubprocess *launch_windows_executable(const std::filesystem::path &path, GError **error);
std::filesystem::path spinel_wine_prefix();
std::vector<WineShortcut> list_wine_shortcuts();
GSubprocess *launch_wine_shortcut(const WineShortcut &shortcut, GError **error);
bool list_wine_installed_apps(std::vector<WineInstalledApp> &apps, GError **error);
std::vector<WineInstalledApp> parse_wine_uninstall_registry(std::string_view text, std::string_view root);
const WineShortcut *find_wine_app_shortcut(const WineInstalledApp &app, const std::vector<WineShortcut> &shortcuts);
GSubprocess *uninstall_wine_app(const std::string &key, GError **error);
