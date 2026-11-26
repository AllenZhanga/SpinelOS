#pragma once

#include "app_state.hpp"

void init_native_shell(GtkApplication *app, AppState *state);
void shutdown_native_shell(AppState *state);

void native_shell_sync_telemetry(AppState *state);
void native_shell_sync_windows(AppState *state);
void native_shell_sync_workspaces(AppState *state);
void native_shell_sync_desktop(AppState *state);
void native_shell_sync_wallpaper(AppState *state);
void native_shell_sync_theme(AppState *state);
void native_shell_sync_dock(AppState *state);
void native_shell_sync_perf(AppState *state);
void native_shell_toggle_cc(AppState *state);
void native_shell_set_cc_visible(AppState *state, bool visible);
void native_shell_toggle_launcher(AppState *state);
void native_shell_sync_sandbox(AppState *state, const std::string &action = "sync");
void native_shell_step_switcher(AppState *state, bool prev);
void native_shell_show_osd(AppState *state, const std::string &kind, int value, bool muted = false);
