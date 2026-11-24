#include "desktop_apps.hpp"
#include "common.hpp"
#include "native_shell.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct DesktopApp {
    std::string id;
    std::string name;
    std::string exec;
    std::string icon_name;
    std::string icon_path;
    std::string icon_uri;
    std::string category;
    bool terminal = false;
};

std::string resolve_icon_path(const std::string &icon) {
    if (icon.empty()) return "";
    std::error_code ec;
    if (icon.front() == '/' && fs::exists(icon, ec)) {
        return icon;
    }

    fs::path local_icons = fs::path(home_dir()) / ".local/share/icons/hicolor";
    fs::path flatpak_user_icons = fs::path(home_dir()) / ".local/share/flatpak/exports/share/icons/hicolor";
    fs::path flatpak_sys_icons = "/var/lib/flatpak/exports/share/icons/hicolor";
    const std::vector<fs::path> search_dirs = {
        local_icons / "scalable/apps",
        local_icons / "128x128/apps",
        local_icons / "64x64/apps",
        local_icons / "256x256/apps",
        flatpak_user_icons / "scalable/apps",
        flatpak_user_icons / "128x128/apps",
        flatpak_user_icons / "64x64/apps",
        flatpak_user_icons / "256x256/apps",
        flatpak_sys_icons / "scalable/apps",
        flatpak_sys_icons / "128x128/apps",
        flatpak_sys_icons / "64x64/apps",
        flatpak_sys_icons / "256x256/apps",
        "/usr/share/icons/hicolor/scalable/apps",
        "/usr/share/icons/hicolor/128x128/apps",
        "/usr/share/icons/hicolor/64x64/apps",
        "/usr/share/icons/hicolor/256x256/apps",
        "/usr/share/icons/hicolor/512x512/apps",
        "/usr/share/icons/hicolor/48x48/apps",
        "/usr/share/icons/Adwaita/scalable/apps",
        "/usr/share/icons/Adwaita/scalable/places",
        "/usr/share/icons/Adwaita/128x128/apps",
        "/usr/share/icons/Adwaita/256x256/apps",
        "/usr/share/pixmaps"
    };
    const std::vector<std::string> exts = {".svg", ".png", ""};

    for (const auto &dir : search_dirs) {
        for (const auto &ext : exts) {
            fs::path p = dir / (icon + ext);
            if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
                return p.string();
            }
        }
    }
    return "";
}

std::vector<std::string> split_exec_args(const std::string &exec_line) {
    std::vector<std::string> args;
    std::string cur;
    bool in_single = false;
    bool in_double = false;
    for (size_t i = 0; i < exec_line.size(); ++i) {
        char c = exec_line[i];
        if (c == '\'' && !in_double) {
            in_single = !in_single;
        } else if (c == '"' && !in_single) {
            in_double = !in_double;
        } else if (std::isspace(static_cast<unsigned char>(c)) && !in_single && !in_double) {
            if (!cur.empty()) {
                if (!(cur.size() == 2 && cur[0] == '%') && cur.rfind("@@", 0) != 0) {
                    args.push_back(std::move(cur));
                }
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty() && !(cur.size() == 2 && cur[0] == '%') && cur.rfind("@@", 0) != 0) {
        args.push_back(std::move(cur));
    }
    return args;
}

bool parse_desktop_file(const fs::path &path, DesktopApp &app) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return false;

    bool in_entry = false;
    bool no_display = false;
    bool hidden = false;
    std::string type;
    std::string name_zh;
    std::string name_en;
    std::string only_show_in;

    std::string line;
    while (std::getline(ifs, line)) {
        line = trim_copy(line);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            in_entry = (line == "[Desktop Entry]");
            continue;
        }
        if (!in_entry) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));

        if (k == "Type") type = v;
        else if (k == "NoDisplay") no_display = (v == "true");
        else if (k == "Hidden") hidden = (v == "true");
        else if (k == "Name") name_en = v;
        else if (k == "Name[zh_CN]") name_zh = v;
        else if (k == "Exec") app.exec = v;
        else if (k == "Icon") app.icon_name = v;
        else if (k == "Categories") app.category = v;
        else if (k == "Terminal") app.terminal = (v == "true");
        else if (k == "OnlyShowIn") only_show_in = v;
    }

    if (type != "Application" || no_display || hidden || app.exec.empty()) return false;
    if (!only_show_in.empty() && only_show_in.find("GNOME") == std::string::npos &&
        only_show_in.find("XFCE") == std::string::npos) {
        return false;
    }

    app.id = path.stem().string();
    app.name = !name_zh.empty() ? name_zh : name_en;
    if (app.name.empty()) app.name = app.id;
    app.icon_path = resolve_icon_path(app.icon_name);
    app.icon_uri = app.icon_path.empty() ? "" : ("file://" + app.icon_path);
    return true;
}

std::vector<DesktopApp> scan_all_apps() {
    std::vector<fs::path> dirs = {
        fs::path(home_dir()) / ".local/share/applications",
        fs::path(home_dir()) / ".local/share/flatpak/exports/share/applications",
        fs::path("/var/lib/flatpak/exports/share/applications"),
        fs::path("/usr/local/share/applications"),
        fs::path("/usr/share/applications")
    };

    std::map<std::string, DesktopApp> by_id;
    std::error_code ec;
    for (const auto &d : dirs) {
        if (!fs::is_directory(d, ec)) continue;
        for (const auto &entry : fs::directory_iterator(d, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".desktop") continue;
            DesktopApp app;
            if (parse_desktop_file(entry.path(), app)) {
                by_id.try_emplace(app.id, std::move(app));
            }
        }
    }

    std::vector<DesktopApp> out;
    out.reserve(by_id.size());
    for (auto &kv : by_id) out.push_back(std::move(kv.second));
    std::sort(out.begin(), out.end(), [](const DesktopApp &a, const DesktopApp &b) {
        return a.name < b.name;
    });
    return out;
}

}  // namespace

std::vector<DesktopAppInfo> list_desktop_apps() {
    auto apps = scan_all_apps();
    std::vector<DesktopAppInfo> out;
    out.reserve(apps.size());
    for (auto &a : apps) {
        out.push_back(DesktopAppInfo{
            std::move(a.id), std::move(a.name), std::move(a.icon_name),
            std::move(a.icon_path), std::move(a.icon_uri), std::move(a.category)
        });
    }
    return out;
}

void push_desktop_apps(AppState *state) {
    if (!state) return;
    auto apps = scan_all_apps();

    std::ostringstream js;
    js << "window.__spinelDesktopApps && window.__spinelDesktopApps([";
    for (size_t i = 0; i < apps.size(); ++i) {
        if (i > 0) js << ",";
        js << "{"
           << "\"id\":\"" << json_escape(apps[i].id) << "\","
           << "\"name\":\"" << json_escape(apps[i].name) << "\","
           << "\"icon\":\"" << json_escape(apps[i].icon_name) << "\","
           << "\"icon_uri\":\"" << json_escape(apps[i].icon_uri) << "\","
           << "\"category\":\"" << json_escape(apps[i].category) << "\""
           << "}";
    }
    js << "]);";
    std::string script = js.str();
    eval_js(state->desktop_view, script);
    eval_js(state->games_view, script);
    eval_js(state->dock_view, script);
    eval_js(state->store_view, script);
    native_shell_sync_desktop(state);
    native_shell_sync_windows(state);
}

void launch_desktop_app_id(const std::string &desktop_id) {
    if (desktop_id.empty()) return;
    for (const auto &app : scan_all_apps()) {
        if (app.id == desktop_id) {
            auto args = split_exec_args(app.exec);
            if (args.empty()) return;
            if (app.terminal) {
                args.insert(args.begin(), "-e");
                args.insert(args.begin(), "foot");
            }
            // Prefer Wayland ozone & IME for Chromium if launched without flag
            if (args[0].find("chromium") != std::string::npos || args[0].find("chrome") != std::string::npos) {
                args.push_back("--ozone-platform-hint=auto");
                args.push_back("--enable-wayland-ime");
            }
            spawn_argv_logged(args, "App:" + app.id);
            return;
        }
    }
}

std::string get_wallpaper_pref() {
    std::string wp = trim_copy(read_first_line(
        (fs::path(home_dir()) / ".config/spinelos/wallpaper.conf").string(), "crystal-blue"
    ));
    if (wp != "crystal-blue" && wp != "obsidian-purple" && wp != "emerald-aurora" && wp != "amber-dusk") {
        wp = "crystal-blue";
    }
    return wp;
}

void push_wallpaper_pref(AppState *state) {
    if (!state) return;
    std::string wp = get_wallpaper_pref();
    std::string js = "window.__spinelWallpaper && window.__spinelWallpaper(\"" + wp + "\");";
    eval_js(state->desktop_view, js);
    eval_js(state->settings_view, js);
    native_shell_sync_wallpaper(state);
}

void set_wallpaper_pref(AppState *state, const std::string &theme_id) {
    if (theme_id != "crystal-blue" && theme_id != "obsidian-purple" &&
        theme_id != "emerald-aurora" && theme_id != "amber-dusk") {
        return;
    }
    std::error_code ec;
    fs::path dir = fs::path(home_dir()) / ".config/spinelos";
    fs::create_directories(dir, ec);
    std::ofstream ofs(dir / "wallpaper.conf");
    if (ofs.is_open()) {
        ofs << theme_id << "\n";
    }
    push_wallpaper_pref(state);
}
