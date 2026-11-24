#pragma once

#include "app_state.hpp"

void init_native_shell(GtkApplication *app, AppState *state);
void shutdown_native_shell(AppState *state);

void native_shell_sync_telemetry(AppState *state);
void native_shell_sync_windows(AppState *state);
void native_shell_sync_desktop(AppState *state);
void native_shell_sync_wallpaper(AppState *state);
void native_shell_sync_theme(AppState *state);
void native_shell_sync_perf(AppState *state);
void native_shell_toggle_cc(AppState *state);
void native_shell_set_cc_visible(AppState *state, bool visible);
