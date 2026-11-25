#pragma once

#include "app_state.hpp"

#include <string>

void push_store_catalog(AppState *state);
void start_store_action(AppState *state, const std::string &action, const std::string &payload);
void cancel_store_action(AppState *state);
void launch_store_app(AppState *state, const std::string &app_id);
