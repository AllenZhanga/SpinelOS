#include "desktop_apps.hpp"
#include "common.hpp"
#include "native_shell.hpp"
#include "system_control.hpp"

#include <gtk/gtk.h>

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

std::string resolve_icon_path(const std::string &icon_raw) {
    std::string icon = trim_copy(icon_raw);
    if (icon.empty()) return "";
    if (icon.size() >= 2 && ((icon.front() == '"' && icon.back() == '"') ||
                              (icon.front() == '\'' && icon.back() == '\''))) {
        icon = icon.substr(1, icon.size() - 2);
    }
    std::error_code ec;
    if (icon.front() == '/' && fs::exists(icon, ec) && fs::is_regular_file(icon, ec)) {
        return icon;
    }

    const std::string home = home_dir();
    const fs::path local_icons = fs::path(home) / ".local/share/icons/hicolor";
    const fs::path flatpak_user_icons = fs::path(home) / ".local/share/flatpak/exports/share/icons/hicolor";
    const fs::path flatpak_sys_icons = "/var/lib/flatpak/exports/share/icons/hicolor";
    const std::vector<fs::path> icon_roots = {
        local_icons,
        flatpak_user_icons,
        flatpak_sys_icons,
        "/usr/local/share/icons/hicolor",
        "/usr/share/icons/hicolor",
        "/usr/share/icons/Adwaita"
    };
    const std::vector<const char *> subdirs = {
        "scalable/apps", "512x512/apps", "256x256/apps", "192x192/apps",
        "128x128/apps", "96x96/apps", "64x64/apps", "48x48/apps",
        "32x32/apps", "0x0/apps", "scalable/places"
    };
    const std::vector<std::string> exts = {"", ".svg", ".png"};

    for (const auto &root : icon_roots) {
        if (!fs::is_directory(root, ec)) continue;
        for (const char *sub : subdirs) {
            fs::path dir = root / sub;
            for (const auto &ext : exts) {
                fs::path p = dir / (icon + ext);
                if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
                    return p.string();
                }
            }
        }
    }

    for (const fs::path &pdir : {fs::path(home) / ".local/share/pixmaps", fs::path("/usr/share/pixmaps")}) {
        if (!fs::is_directory(pdir, ec)) continue;
        for (const auto &ext : exts) {
            fs::path p = pdir / (icon + ext);
            if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
                return p.string();
            }
        }
    }

    if (GdkDisplay *disp = gdk_display_get_default()) {
        if (GtkIconTheme *theme = gtk_icon_theme_get_for_display(disp)) {
            std::string lookup_name = icon;
            for (const char *sfx : {".png", ".svg", ".xpm"}) {
                std::string s(sfx);
                if (lookup_name.size() > s.size() &&
                    lookup_name.compare(lookup_name.size() - s.size(), s.size(), s) == 0) {
                    lookup_name = lookup_name.substr(0, lookup_name.size() - s.size());
                    break;
                }
            }
            GtkIconPaintable *paintable = gtk_icon_theme_lookup_icon(
                theme, lookup_name.c_str(), nullptr, 128, 1, GTK_TEXT_DIR_NONE, GTK_ICON_LOOKUP_PRELOAD);
            if (paintable) {
                std::string resolved;
                if (GFile *gf = gtk_icon_paintable_get_file(paintable)) {
                    g_autofree char *fpath = g_file_get_path(gf);
                    if (fpath && fs::exists(fpath, ec) && fs::is_regular_file(fpath, ec)) {
                        resolved = fpath;
                    }
                    g_object_unref(gf);
                }
                g_object_unref(paintable);
                if (!resolved.empty()) return resolved;
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

bool is_hidden_utility_app(const std::string &id, bool is_terminal) {
    if (is_terminal) return true;
    if ((id == "foot" || id == "org.codeberg.dnkl.foot" ||
         id == "ptyxis" || id == "org.gnome.Ptyxis" || id == "app.devsuite.Ptyxis") &&
        has_executable("spinel-terminal")) {
        return true;
    }
    if ((id == "thunar" || id == "Thunar") && has_executable("spinel-files")) {
        return true;
    }
    static const std::set<std::string> hidden_ids = {
        "footclient",
        "foot-server",
        "thunar-settings",
        "thunar-bulk-rename",
        "org.fcitx.fcitx5-migrator",
        "org.fcitx.Fcitx5",
        "org.fcitx.fcitx5-config-qt",
        "org.fcitx.fcitx5-qt5-gui-wrapper",
        "org.fcitx.fcitx5-qt6-gui-wrapper",
        "fcitx5-wayland-launcher",
        "kcm_fcitx5",
        "kbd-layout-viewer5",
        "qv4l2",
        "qvidcap",
        "lstopo",
        "stoken-gui",
        "stoken-gui-small",
        "lftp",
        "vim",
        "xfce4-about",
        "avahi-discover",
        "bssh",
        "bvnc",
        "cmake-gui",
        "xterm",
        "uxterm"
    };
    return hidden_ids.count(id) > 0;
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
        only_show_in.find("SpinelOS") == std::string::npos) {
        return false;
    }

    app.id = path.stem().string();
    if (is_hidden_utility_app(app.id, app.terminal)) return false;

    if (app.id == "thunar") app.name = "文件管理";
    else if (app.id == "foot" || app.id == "ptyxis" || app.id == "org.gnome.Ptyxis" || app.id == "app.devsuite.Ptyxis") {
        app.name = "终端";
    }
    else if (app.id == "fcitx5-configtool") app.name = "输入法设置";
    else app.name = !name_zh.empty() ? name_zh : name_en;

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
                if (has_executable("spinel-terminal")) {
                    args.insert(args.begin(), "--");
                    args.insert(args.begin(), "spinel-terminal");
                } else if (has_executable("ptyxis")) {
                    args.insert(args.begin(), "--");
                    args.insert(args.begin(), "ptyxis");
                } else {
                    args.insert(args.begin(), "-e");
                    args.insert(args.begin(), "foot");
                }
            }
            // Launch browsers through unified Wayland + system-proxy helper
            std::string bin_l = args[0];
            for (char &ch : bin_l) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (bin_l.find("chromium") != std::string::npos || bin_l.find("chrome") != std::string::npos ||
                bin_l.find("firefox") != std::string::npos || bin_l.find("brave") != std::string::npos ||
                bin_l.find("msedge") != std::string::npos) {
                launch_browser_command(std::move(args), "App:" + app.id);
                return;
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
    fs::path wp_file = fs::path(home_dir()) / ".config/spinelos/wallpaper.conf";
    write_text_file(wp_file.string(), theme_id + "\n");
    push_wallpaper_pref(state);
}
