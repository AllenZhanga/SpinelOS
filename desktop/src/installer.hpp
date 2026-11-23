#pragma once

#include "app_state.hpp"

#include <string>

void push_installer_env(AppState *state);
void start_disk_install(AppState *state, const std::string &payload);
