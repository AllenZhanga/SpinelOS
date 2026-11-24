#pragma once

#include "app_state.hpp"

#include <string>

void push_desktop_apps(AppState *state);
void launch_desktop_app_id(const std::string &desktop_id);

void push_wallpaper_pref(AppState *state);
void set_wallpaper_pref(AppState *state, const std::string &theme_id);
