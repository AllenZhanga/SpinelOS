#pragma once

#include "app_state.hpp"

#include <string>
#include <vector>

struct DesktopAppInfo {
    std::string id;
    std::string name;
    std::string icon_name;
    std::string icon_path;
    std::string icon_uri;
    std::string category;
};

std::vector<DesktopAppInfo> list_desktop_apps();
void push_desktop_apps(AppState *state);
void launch_desktop_app_id(const std::string &desktop_id);
void launch_desktop_app_file(const std::string &path);
void notify_app_launch_failure(const std::string &name, const std::string &detail);
struct LocalAppShortcut {
    std::string shortcut_path;
    std::string reveal_path;
};
LocalAppShortcut find_local_app_shortcut(const std::string &id);
bool trash_local_app_shortcut(const std::string &id, std::string &error);
bool execute_launchpad_app_action(const std::string &id, bool remove_shortcut, std::string &feedback);
void handle_launchpad_app_action(AppState *state, const std::string &id, bool remove_shortcut);
bool resolve_desktop_app_argv(const std::string &desktop_id, std::vector<std::string> &out_argv,
                              std::string &error);
bool launch_cmd_in_sandbox(const std::string &cmd, std::string &error_out);
void launch_sandbox_autostart_apps();

std::vector<std::string> derive_app_quit_process_names(const std::string &app_id,
                                                       const std::string &title,
                                                       const std::string &exec_hint = "");
bool is_protected_system_process(const std::string &proc_name);
bool quit_app_processes(const std::string &app_id, const std::string &title);

std::string get_wallpaper_pref();
void push_wallpaper_pref(AppState *state);
void set_wallpaper_pref(AppState *state, const std::string &theme_id);
