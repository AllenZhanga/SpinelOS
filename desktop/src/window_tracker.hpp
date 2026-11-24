#pragma once

#include "app_state.hpp"

#include <cstdint>

void init_window_tracker(AppState *state);
void shutdown_window_tracker();
void push_windows_to_ui(AppState *state);
void toggle_or_activate_window(uint64_t id);
void close_tracked_window(uint64_t id);
