#pragma once

#include "app_state.hpp"
#include "system_control.hpp"

#include <string>
#include <vector>

struct SteamGameInfo {
    std::string appid;
    std::string name;
    long long size_bytes = 0;
    long long last_updated = 0;
    std::string cover_path;
    std::string cover_uri;
};

void ensure_steam_play_preconfig();
std::vector<SteamGameInfo> list_steam_games();
void push_steam_library(AppState *state);
void launch_steam_client(AppState *state);
void launch_steam_game(const std::string &appid, const PerfConfig &perf);
void launch_steam_gamepadui(AppState *state, const PerfConfig &perf);
