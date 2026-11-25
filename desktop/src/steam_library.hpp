#pragma once

#include "app_state.hpp"
#include "system_control.hpp"

#include <map>
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

struct GamePerfOverride {
    std::string appid;
    bool enabled = false;
    std::string mangohud = "compact";
    int fps_limit = 0;
    bool fsr = false;
    bool gamemode = true;
    std::string render_res = "native";
};

struct CacheStorageStats {
    long long shadercache_bytes = 0;
    long long downloading_bytes = 0;
    long long logs_bytes = 0;
    long long pacman_bytes = 0;
};

struct SteamPerfApplyResult {
    int games_updated = 0;
    int configs_updated = 0;
    bool steam_running = false;
    std::string launch_options;
};

void ensure_steam_play_preconfig();
std::vector<SteamGameInfo> list_steam_games();
void push_steam_library(AppState *state);
void launch_steam_client(AppState *state);
void launch_steam_game(const std::string &appid, const PerfConfig &perf);
void launch_steam_gamepadui(AppState *state, const PerfConfig &perf);
std::string build_steam_launch_options(const PerfConfig &perf);
SteamPerfApplyResult apply_perf_to_steam_configs(const PerfConfig &perf);

std::map<std::string, GamePerfOverride> load_game_overrides();
bool save_game_override(const GamePerfOverride &ov);
bool clear_game_override(const std::string &appid);
PerfConfig resolve_perf_for_appid(const std::string &appid, const PerfConfig &global_perf);
void handle_game_override_command(AppState *state, const std::string &payload, bool is_clear = false);
void open_steam_compatdata(AppState *state, const std::string &appid);

CacheStorageStats scan_storage_caches();
void push_storage_caches(AppState *state);
void clean_storage_caches(AppState *state, const std::string &payload);
