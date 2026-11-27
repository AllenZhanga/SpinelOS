#pragma once

#include "app_state.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct TrackedWindowInfo {
    uint64_t id = 0;
    std::string title;
    std::string app_id;
    bool activated = false;
    bool minimized = false;
    bool maximized = false;
    bool fullscreen = false;
};

struct TrackedWorkspaceInfo {
    int index = 0;       // 0-based order index
    std::string id;
    std::string name;
    bool active = false;
    bool urgent = false;
    bool hidden = false;
    bool can_activate = true;
};

void init_window_tracker(AppState *state);
void shutdown_window_tracker();
std::vector<TrackedWindowInfo> list_tracked_windows();
std::vector<TrackedWindowInfo> list_tracked_windows_mru();
void push_windows_to_ui(AppState *state);
void toggle_or_activate_window(uint64_t id);
void toggle_maximize_tracked_window(uint64_t id);
void minimize_tracked_window(uint64_t id);
void activate_tracked_window(uint64_t id);
void close_tracked_window(uint64_t id);
bool quit_tracked_window_app(uint64_t id);

std::vector<TrackedWorkspaceInfo> list_tracked_workspaces();
bool activate_tracked_workspace(int index_0based);
bool step_tracked_workspace(int delta);
std::string format_workspace_badge_label(int index_0based, const std::string &ws_name);
