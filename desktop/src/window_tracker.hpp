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
};

void init_window_tracker(AppState *state);
void shutdown_window_tracker();
std::vector<TrackedWindowInfo> list_tracked_windows();
void push_windows_to_ui(AppState *state);
void toggle_or_activate_window(uint64_t id);
void close_tracked_window(uint64_t id);
