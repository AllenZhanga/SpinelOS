#pragma once

#include "app_state.hpp"
#include "system_control.hpp"

#include <string>

void push_steam_library(AppState *state);
void launch_steam_game(const std::string &appid, const PerfConfig &perf);
void launch_steam_gamepadui(const PerfConfig &perf);
