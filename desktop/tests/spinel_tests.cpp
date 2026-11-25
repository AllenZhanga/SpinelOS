#include "common.hpp"
#include "desktop_apps.hpp"
#include "native_shell.hpp"
#include "steam_library.hpp"
#include "system_control.hpp"
#include "theme_manager.hpp"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Stubs for native_shell / main entrypoints not exercised headlessly
void init_native_shell(GtkApplication *, AppState *) {}
void shutdown_native_shell(AppState *) {}
void native_shell_sync_telemetry(AppState *) {}
void native_shell_sync_windows(AppState *) {}
void native_shell_sync_desktop(AppState *) {}
void native_shell_sync_wallpaper(AppState *) {}
void native_shell_sync_theme(AppState *) {}
void native_shell_sync_perf(AppState *) {}
void native_shell_toggle_cc(AppState *) {}
void native_shell_set_cc_visible(AppState *, bool) {}
void native_shell_toggle_launcher(AppState *) {}
void native_shell_show_osd(AppState *, const std::string &, int, bool) {}
void handle_shell_command(AppState *, const std::string &) {}

namespace {

int g_failures = 0;

void expect(bool cond, const char *expr, int line) {
    if (!cond) {
        std::cerr << "[FAIL] line " << line << ": " << expr << "\n";
        ++g_failures;
    }
}

#define EXPECT(cond) expect((cond), #cond, __LINE__)

struct TempHomeGuard {
    std::string old_home;
    bool had_home = false;
    fs::path tmp_dir;

    TempHomeGuard() {
        const char *h = std::getenv("HOME");
        if (h) {
            had_home = true;
            old_home = h;
        }
        tmp_dir = fs::temp_directory_path() / ("spinel-test-" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(tmp_dir, ec);
        fs::create_directories(tmp_dir, ec);
        setenv("HOME", tmp_dir.c_str(), 1);
    }

    ~TempHomeGuard() {
        if (had_home) {
            setenv("HOME", old_home.c_str(), 1);
        } else {
            unsetenv("HOME");
        }
        std::error_code ec;
        fs::remove_all(tmp_dir, ec);
    }
};

void test_common_utils() {
    EXPECT(trim_copy("  hello \t\r\n") == "hello");
    EXPECT(trim_copy("   ") == "");
    EXPECT(json_escape("a\"b\\c\nd\te") == "a\\\"b\\\\c\\nd\\te");
    EXPECT(json_escape(std::string("x") + '\x05' + "y") == "x y");

    auto lines = split_lines("alpha\r\nbeta\ngamma");
    EXPECT(lines.size() == 3);
    EXPECT(lines[0] == "alpha");
    EXPECT(lines[1] == "beta");
    EXPECT(lines[2] == "gamma");

    fs::path p = fs::path(home_dir()) / "nested/dir/sample.conf";
    EXPECT(write_text_file(p.string(), "first line\nsecond line\n"));
    EXPECT(read_first_line(p.string(), "fallback") == "first line");
    EXPECT(read_first_line((fs::path(home_dir()) / "missing.conf").string(), "fallback") == "fallback");
}

void test_theme_and_wallpaper() {
    auto themes = list_available_themes();
    EXPECT(themes.size() >= 4);
    EXPECT(get_active_theme_id() == "tahoe-light");
    EXPECT(get_active_theme().is_light == true);

    set_active_theme(nullptr, "obsidian-neon");
    EXPECT(get_active_theme_id() == "obsidian-neon");
    EXPECT(get_active_theme().is_light == false);
    EXPECT(get_active_theme().wp_c0 == 0x070510);

    // Custom .theme file parsing
    fs::path custom_theme = fs::path(home_dir()) / ".config/spinelos/themes/midnight-test.theme";
    EXPECT(write_text_file(custom_theme.string(),
                           "name=Midnight Test\n"
                           "subtitle=Unit Test Theme\n"
                           "is_light=false\n"
                           "wp_c0=#112233\n"
                           "wp_glow1=#445566\n"));
    auto with_custom = list_available_themes();
    bool found_custom = false;
    for (const auto &t : with_custom) {
        if (t.id == "midnight-test") {
            found_custom = true;
            EXPECT(t.name == "Midnight Test");
            EXPECT(t.wp_c0 == 0x112233);
            EXPECT(t.wp_glow1 == 0x445566);
        }
    }
    EXPECT(found_custom);

    // Custom wallpaper image persistence + missing file guard
    EXPECT(get_custom_wallpaper_image().empty());
    fs::path fake_img = fs::path(home_dir()) / "Pictures/spinel-demo-wallpaper.jpg";
    EXPECT(write_text_file(fake_img.string(), "fake-jpg-bytes"));
    set_custom_wallpaper_image(nullptr, fake_img.string());
    EXPECT(get_custom_wallpaper_image() == fake_img.string());
    fs::remove(fake_img);
    EXPECT(get_custom_wallpaper_image().empty());
    set_custom_wallpaper_image(nullptr, "");
    EXPECT(get_custom_wallpaper_image().empty());
}

void test_desktop_apps() {
    fs::path app_dir = fs::path(home_dir()) / ".local/share/applications";
    fs::create_directories(app_dir);

    EXPECT(write_text_file((app_dir / "spinel-demo-app.desktop").string(),
                           "[Desktop Entry]\n"
                           "Type=Application\n"
                           "Name=Demo App\n"
                           "Name[zh_CN]=示例应用\n"
                           "Exec=demo-bin %U\n"
                           "Categories=Game;Utility;\n"));
    EXPECT(write_text_file((app_dir / "spinel-hidden-app.desktop").string(),
                           "[Desktop Entry]\n"
                           "Type=Application\n"
                           "Name=Hidden App\n"
                           "NoDisplay=true\n"
                           "Exec=hidden-bin\n"));

    auto apps = list_desktop_apps();
    bool saw_demo = false;
    bool saw_hidden = false;
    for (const auto &a : apps) {
        if (a.id == "spinel-demo-app") {
            saw_demo = true;
            EXPECT(a.name == "示例应用");
            EXPECT(a.category == "Game;Utility;");
        }
        if (a.id == "spinel-hidden-app") {
            saw_hidden = true;
        }
    }
    EXPECT(saw_demo);
    EXPECT(!saw_hidden);
}

void test_steam_library() {
    PerfConfig compact;
    compact.gamemode = true;
    compact.mangohud = "compact";
    compact.fps_limit = 60;
    compact.fsr = false;
    compact.render_res = "native";
    std::string opts_compact = build_steam_launch_options(compact);
    EXPECT(opts_compact.find("gamemoderun") != std::string::npos);
    EXPECT(opts_compact.find("MANGOHUD=1") != std::string::npos);
    EXPECT(opts_compact.find("-r 60") != std::string::npos);
    EXPECT(opts_compact.find("--mangoapp") != std::string::npos);
    EXPECT(opts_compact.find("%command%") != std::string::npos);

    PerfConfig fsr_cfg;
    fsr_cfg.gamemode = false;
    fsr_cfg.mangohud = "off";
    fsr_cfg.fsr = true;
    fsr_cfg.render_res = "720p";
    std::string opts_fsr = build_steam_launch_options(fsr_cfg);
    EXPECT(opts_fsr.find("MANGOHUD=1") == std::string::npos);
    EXPECT(opts_fsr.find("gamescope") != std::string::npos);
    EXPECT(opts_fsr.find("-w 1280 -h 720") != std::string::npos);
    EXPECT(opts_fsr.find("-F fsr") != std::string::npos);

    // Mock primary + secondary Steam library discovery & Proton filtering
    fs::path root1 = fs::path(home_dir()) / ".local/share/Steam";
    fs::path root2 = fs::path(home_dir()) / "Games/SteamLib2";
    fs::create_directories(root1 / "steamapps");
    fs::create_directories(root2 / "steamapps");

    std::ostringstream vdf;
    vdf << "\"libraryfolders\"\n{\n"
        << "\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" << root1.string() << "\"\n\t}\n"
        << "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" << root2.string() << "\"\n\t}\n}\n";
    EXPECT(write_text_file((root1 / "steamapps/libraryfolders.vdf").string(), vdf.str()));

    EXPECT(write_text_file((root1 / "steamapps/appmanifest_730.acf").string(),
                           "\"AppState\"\n{\n\t\"appid\"\t\"730\"\n\t\"name\"\t\"Counter-Strike 2\"\n\t\"SizeOnDisk\"\t\"35000000000\"\n}\n"));
    EXPECT(write_text_file((root2 / "steamapps/appmanifest_228980.acf").string(),
                           "\"AppState\"\n{\n\t\"appid\"\t\"228980\"\n\t\"name\"\t\"Steamworks Common Redistributables\"\n}\n"));
    EXPECT(write_text_file((root2 / "steamapps/appmanifest_1493710.acf").string(),
                           "\"AppState\"\n{\n\t\"appid\"\t\"1493710\"\n\t\"name\"\t\"Proton Experimental\"\n}\n"));

    auto games = list_steam_games();
    EXPECT(games.size() == 1);
    if (!games.empty()) {
        EXPECT(games[0].appid == "730");
        EXPECT(games[0].name == "Counter-Strike 2");
        EXPECT(games[0].size_bytes == 35000000000LL);
    }

    fs::path localcfg = root1 / "userdata/12345/config/localconfig.vdf";
    EXPECT(write_text_file(localcfg.string(),
                           "\"UserLocalConfigStore\"\n{\n\t\"Software\"\n\t{\n\t\t\"Valve\"\n\t\t{\n\t\t\t\"Steam\"\n\t\t\t{\n\t\t\t\t\"apps\"\n\t\t\t\t{\n\t\t\t\t\t\"730\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"LastPlayed\"\t\"0\"\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n"));
    auto applied = apply_perf_to_steam_configs(compact);
    EXPECT(applied.games_updated == 1);
    EXPECT(applied.configs_updated == 1);

    // Per-game performance override & resolution check
    GamePerfOverride ov;
    ov.appid = "730";
    ov.enabled = true;
    ov.mangohud = "off";
    ov.fps_limit = 90;
    ov.fsr = true;
    ov.gamemode = false;
    ov.render_res = "720p";
    EXPECT(save_game_override(ov));
    PerfConfig eff = resolve_perf_for_appid("730", compact);
    EXPECT(eff.mangohud == "off");
    EXPECT(eff.fps_limit == 90);
    EXPECT(eff.fsr == true);
    EXPECT(eff.gamemode == false);
    EXPECT(eff.render_res == "720p");
    EXPECT(resolve_perf_for_appid("999999", compact).fps_limit == 60);

    auto ov_applied = apply_perf_to_steam_configs(compact);
    EXPECT(ov_applied.games_updated == 1);
    std::ifstream lcfg_ifs(localcfg);
    std::ostringstream lcfg_buf;
    lcfg_buf << lcfg_ifs.rdbuf();
    EXPECT(lcfg_buf.str().find("-r 90 -F fsr") != std::string::npos);

    EXPECT(clear_game_override("730"));
    EXPECT(resolve_perf_for_appid("730", compact).fps_limit == 60);

    // Storage & cache scanning / cleanup
    EXPECT(write_text_file((root1 / "steamapps/shadercache/730/shader.bin").string(), "0123456789"));
    EXPECT(write_text_file((root1 / "steamapps/downloading/730/chunk.part").string(), "abcdef"));
    EXPECT(write_text_file((fs::path(spinel_state_dir()) / "apps.log").string(), "log-line\n"));
    CacheStorageStats before = scan_storage_caches();
    EXPECT(before.shadercache_bytes == 10);
    EXPECT(before.downloading_bytes == 6);
    EXPECT(before.logs_bytes == 9);
    clean_storage_caches(nullptr, "steam:clean-caches\ntargets=shadercache,downloading,logs");
    CacheStorageStats after = scan_storage_caches();
    EXPECT(after.shadercache_bytes == 0);
    EXPECT(after.downloading_bytes == 0);
    EXPECT(after.logs_bytes == 0);
}

void test_system_control_configs() {
    // Shortcuts config + invalid injection rejection
    fs::path sc_path = fs::path(home_dir()) / ".config/spinelos/shortcuts.conf";
    EXPECT(write_text_file(sc_path.string(),
                           "screenshot_area=C-S-a\n"
                           "screenshot_full=bad;rm -rf /\n"
                           "lock_screen=W-Escape\n"
                           "open_terminal=W-t\n"));
    ShortcutConfig sc = load_shortcuts_config();
    EXPECT(sc.screenshot_area == "C-S-a");
    EXPECT(sc.screenshot_full == "Print");  // invalid value rejected, keeps default
    EXPECT(sc.lock_screen == "W-Escape");
    EXPECT(sc.open_terminal == "W-t");

    // Idle lock & DND config persistence
    EXPECT(get_idle_lock_enabled() == false);
    set_idle_lock_enabled(nullptr, true);
    EXPECT(get_idle_lock_enabled() == true);
    set_idle_lock_enabled(nullptr, false);
    EXPECT(get_idle_lock_enabled() == false);

    EXPECT(get_dnd_enabled() == false);
    toggle_dnd_mode(nullptr);
    EXPECT(get_dnd_enabled() == true);
    toggle_dnd_mode(nullptr);
    EXPECT(get_dnd_enabled() == false);

    // Mako config sync includes DND rule
    sync_notification_theme(true);
    std::ifstream mako_ifs(fs::path(home_dir()) / ".config/mako/config");
    std::ostringstream mako_buf;
    mako_buf << mako_ifs.rdbuf();
    EXPECT(mako_buf.str().find("[mode=do-not-disturb]") != std::string::npos);

    // Clipboard clear removes history cleanly
    EXPECT(write_text_file((fs::path(spinel_state_dir()) / "clipboard_history.tsv").string(),
                           "1000\thello\\nworld\n"));
    auto clips = load_clipboard_history();
    EXPECT(clips.size() == 1);
    if (!clips.empty()) {
        EXPECT(clips[0].text == "hello\nworld");
    }
    clear_clipboard_history();
    EXPECT(load_clipboard_history().empty());

    // labwc config generation (workspaces + snapping + media OSD keys)
    EXPECT(write_labwc_rc(true, sc));
    std::ifstream rc_ifs(fs::path(home_dir()) / ".config/labwc/rc.xml");
    std::ostringstream rc_buf;
    rc_buf << rc_ifs.rdbuf();
    std::string rc_xml = rc_buf.str();
    EXPECT(rc_xml.find("<desktops number=\"4\">") != std::string::npos);
    EXPECT(rc_xml.find("SnapToEdge") != std::string::npos);
    EXPECT(rc_xml.find("GoToDesktop") != std::string::npos);
    EXPECT(rc_xml.find("XF86AudioRaiseVolume") != std::string::npos);

    // OOBE welcome state persistence
    EXPECT(is_welcome_completed() == false);
    mark_welcome_completed();
    EXPECT(is_welcome_completed() == true);
}

}  // namespace

int main() {
    TempHomeGuard guard;
    test_common_utils();
    test_theme_and_wallpaper();
    test_desktop_apps();
    test_steam_library();
    test_system_control_configs();

    if (g_failures > 0) {
        std::cerr << g_failures << " assertion(s) failed.\n";
        return 1;
    }
    std::cout << "All SpinelOS unit tests passed.\n";
    return 0;
}
