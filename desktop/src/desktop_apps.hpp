#pragma once

#include "app_state.hpp"

#include <string>
#include <vector>

struct DesktopAppInfo {
    std::string id;
    std::string name;
    std::string icon_name;
    std::string icon_path;
    std::string icon_uri;
    std::string category;
};

std::vector<DesktopAppInfo> list_desktop_apps();
void push_desktop_apps(AppState *state);
void launch_desktop_app_id(const std::string &desktop_id);

std::string get_wallpaper_pref();
void push_wallpaper_pref(AppState *state);
void set_wallpaper_pref(AppState *state, const std::string &theme_id);
