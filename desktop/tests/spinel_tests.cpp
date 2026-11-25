#include "common.hpp"
#include "desktop_apps.hpp"
#include "ipc_router.hpp"
#include "native_shell.hpp"
#include "shell/sni_tray.hpp"
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
    EXPECT(opts_compact.find("-r 60") != std::string::npos);
    EXPECT(opts_compact.find("--mangoapp") != std::string::npos);
    EXPECT(opts_compact.find("%command%") != std::string::npos);

    PerfConfig compact_direct;
    compact_direct.gamemode = true;
    compact_direct.mangohud = "compact";
    compact_direct.fps_limit = 0;
    compact_direct.fsr = false;
    compact_direct.render_res = "native";
    std::string opts_direct = build_steam_launch_options(compact_direct);
    EXPECT(opts_direct == "gamemoderun mangohud %command%");
    EXPECT(merge_steam_launch_options(
               "VKD3D_CONFIG=dxr,dxr11 PROTON_ENABLE_NVAPI=1 gamemoderun mangohud %command% -dx12",
               "gamemoderun %command%") ==
           "VKD3D_CONFIG=dxr,dxr11 PROTON_ENABLE_NVAPI=1 gamemoderun %command% -dx12");
    EXPECT(merge_steam_launch_options(
               "PROTON_ENABLE_NVAPI=1 gamescope -f -w 1280 -h 720 --mangoapp -- %command%",
               "gamemoderun mangohud %command%") ==
           "PROTON_ENABLE_NVAPI=1 gamemoderun mangohud %command%");

    PerfConfig fsr_cfg;
    fsr_cfg.gamemode = false;
    fsr_cfg.mangohud = "off";
    fsr_cfg.fsr = true;
    fsr_cfg.render_res = "720p";
    std::string opts_fsr = build_steam_launch_options(fsr_cfg);
    EXPECT(opts_fsr.find("MANGOHUD=1") == std::string::npos);
    EXPECT(opts_fsr.find("mangohud") == std::string::npos);
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

    // Fcitx5 Rime Simplified Chinese offline defaults (zero qt6-webengine dependency)
    ensure_fcitx5_defaults(true);
    std::ifstream prof_ifs(fs::path(home_dir()) / ".config/fcitx5/profile");
    std::ostringstream prof_buf;
    prof_buf << prof_ifs.rdbuf();
    EXPECT(prof_buf.str().find("DefaultIM=rime") != std::string::npos);

    std::ifstream rime_ifs(fs::path(home_dir()) / ".local/share/fcitx5/rime/default.custom.yaml");
    std::ostringstream rime_buf;
    rime_buf << rime_ifs.rdbuf();
    EXPECT(rime_buf.str().find("schema: luna_pinyin_simp") != std::string::npos);

    // Spinel Doctor checks + log/credential redaction
    std::string raw_secret =
        "Proxy=http://alice:TopSecret99@127.0.0.1:7890\n"
        "Home=/home/alice/.config/spinelos/apps.log\n"
        "Uri=file:///home/alice/Desktop/shot.png\n"
        "wifi-sec.psk:MyWifiPass42 password=hunter2 token=abc123xyz\n"
        "BSSID=12:34:56:78:9A:BC\n";
    std::string redacted = redact_sensitive_text(raw_secret);
    EXPECT(redacted.find("TopSecret99") == std::string::npos);
    EXPECT(redacted.find("MyWifiPass42") == std::string::npos);
    EXPECT(redacted.find("hunter2") == std::string::npos);
    EXPECT(redacted.find("abc123xyz") == std::string::npos);
    EXPECT(redacted.find("alice") == std::string::npos);
    EXPECT(redacted.find("78:9A:BC") == std::string::npos);
    EXPECT(redacted.find("http://***@127.0.0.1:7890") != std::string::npos);
    EXPECT(redacted.find("/home/<REDACTED>/.config") != std::string::npos);
    EXPECT(redacted.find("12:34:56:**:**:**") != std::string::npos);

    auto doc_checks = run_system_health_checks();
    EXPECT(doc_checks.size() == 9);
    bool has_drv_check = false, has_upd_check = false;
    for (const auto &c : doc_checks) {
        if (c.id == "gpu_driver_ver") has_drv_check = true;
        if (c.id == "sys_updates") has_upd_check = true;
    }
    EXPECT(has_drv_check && has_upd_check);
    std::string doc_report = build_redacted_diagnostic_report();
    EXPECT(doc_report.find("Spinel OS Doctor Diagnostic Report") != std::string::npos);
    EXPECT(doc_report.find("[Health Checks]") != std::string::npos);
}

void test_ipc_router() {
    // Legacy key=value payload parsing
    IpcRequest leg;
    EXPECT(parse_ipc_request("perf:set\nmangohud=full\nfps_limit=120\n", leg));
    EXPECT(!leg.is_json);
    EXPECT(!leg.has_id);
    EXPECT(leg.method == "perf:set");
    EXPECT(leg.param("mangohud") == "full");
    EXPECT(leg.param("fps_limit") == "120");

    // Structured JSON-RPC request parsing (dotted method + escaped chars)
    IpcRequest jr;
    EXPECT(parse_ipc_request(R"({"id":42,"method":"settings.proxy-save","params":{"http_proxy":"http://127.0.0.1:7890","enabled":true}})", jr));
    EXPECT(jr.is_json);
    EXPECT(jr.has_id);
    EXPECT(jr.id_json == "42");
    EXPECT(jr.method == "settings:proxy-save");
    EXPECT(jr.param("http_proxy") == "http://127.0.0.1:7890");
    EXPECT(jr.param("enabled") == "true");
    EXPECT(jr.legacy_raw.find("settings:proxy-save\n") == 0);

    // Malformed / empty JSON rejection
    IpcRequest bad;
    EXPECT(!parse_ipc_request("", bad));
    EXPECT(!parse_ipc_request("{bad json", bad));
    EXPECT(!parse_ipc_request(R"({"id":1})", bad));

    // Mock dispatch verification (legacy + JSON + unknown method)
    static std::string last_tab;
    static std::string last_store_focus;
    static bool cancel_called;
    last_tab.clear();
    last_store_focus.clear();
    cancel_called = false;

    IpcHostActions mock_actions;
    mock_actions.open_settings = +[](AppState *, const std::string &tab) { last_tab = tab; };
    mock_actions.open_store = +[](AppState *, const std::string &focus) { last_store_focus = focus; };
    mock_actions.store_cancel = +[](AppState *) { cancel_called = true; };

    AppState dummy;
    std::string reply;
    EXPECT(dispatch_ipc_command(&dummy, R"({"id":"r1","method":"settings.open","params":{"tab":"appearance"}})", mock_actions, &reply));
    EXPECT(last_tab == "appearance");
    EXPECT(reply.find("\"id\":\"r1\"") != std::string::npos);
    EXPECT(reply.find("\"ok\":true") != std::string::npos);

    reply.clear();
    EXPECT(dispatch_ipc_command(&dummy, "store:open:steam", mock_actions, &reply));
    EXPECT(last_store_focus == "steam");
    EXPECT(reply.empty());  // Legacy string without id produces no reply envelope

    reply.clear();
    EXPECT(!dispatch_ipc_command(&dummy, R"({"id":9,"method":"nonexistent.op"})", mock_actions, &reply));
    EXPECT(reply.find("\"id\":9") != std::string::npos);
    EXPECT(reply.find("\"ok\":false") != std::string::npos);
    EXPECT(reply.find("\"error\":\"unknown_method\"") != std::string::npos);

    // Structured JSON perf.set end-to-end config persistence
    reply.clear();
    EXPECT(dispatch_ipc_command(&dummy, R"({"id":10,"method":"perf.set","params":{"mangohud":"off","fps_limit":144,"fsr":true,"gamemode":false,"render_res":"900p"}})", mock_actions, &reply));
    EXPECT(reply.find("\"ok\":true") != std::string::npos);
    PerfConfig saved_perf = load_perf_config();
    EXPECT(saved_perf.mangohud == "off");
    EXPECT(saved_perf.fps_limit == 144);
    EXPECT(saved_perf.fsr == true);
    EXPECT(saved_perf.gamemode == false);
    EXPECT(saved_perf.render_res == "900p");

    // MangoHud config sync ('off' -> no_display; 'full' -> frametime=1) & GPU/driver info query
    auto slurp_file = [](const fs::path &p) -> std::string {
        std::ifstream ifs(p);
        std::ostringstream oss;
        oss << ifs.rdbuf();
        return oss.str();
    };
    sync_mangohud_config(saved_perf);
    std::string mh_off = slurp_file(fs::path(home_dir()) / ".config/MangoHud/MangoHud.conf");
    EXPECT(mh_off.find("no_display") != std::string::npos);
    EXPECT(mh_off.find("toggle_hud=Shift_R+F12") != std::string::npos);
    PerfConfig full_perf;
    full_perf.mangohud = "full";
    sync_mangohud_config(full_perf);
    std::string mh_full = slurp_file(fs::path(home_dir()) / ".config/MangoHud/MangoHud.conf");
    EXPECT(mh_full.find("frametime=1") != std::string::npos);
    EXPECT(mh_full.find("no_display") == std::string::npos);

    GpuDriverUpdateInfo gpu_info = collect_gpu_driver_update_info();
    EXPECT(!gpu_info.driver_summary.empty());
    EXPECT(!gpu_info.kernel_running.empty());

    reply.clear();
    EXPECT(dispatch_ipc_command(&dummy, "store:cancel", mock_actions, &reply));
    EXPECT(cancel_called);

    // Device manager path validation & IPC dispatch
    EXPECT(is_valid_block_dev_path("/dev/sda1"));
    EXPECT(is_valid_block_dev_path("/dev/nvme0n1p2"));
    EXPECT(!is_valid_block_dev_path("/dev/../etc/passwd"));
    EXPECT(!is_valid_block_dev_path("/dev/sda1;rm -rf /"));
    EXPECT(!is_valid_block_dev_path("/tmp/sda1"));
    EXPECT(is_valid_audio_node_name("alsa_output.pci-0000_01_00.1.hdmi-stereo"));
    EXPECT(!is_valid_audio_node_name("alsa_output bad\nnode"));
    EXPECT(is_valid_audio_test_channel("left"));
    EXPECT(is_valid_audio_test_channel("right"));
    EXPECT(is_valid_audio_test_channel("stereo"));
    EXPECT(is_valid_audio_test_channel("stop"));
    EXPECT(!is_valid_audio_test_channel("left;rm -rf /"));
    {
        auto left_wav = build_speaker_test_wav("left", 48000);
        auto right_wav = build_speaker_test_wav("right", 48000);
        auto stereo_wav = build_speaker_test_wav("stereo", 48000);
        EXPECT(left_wav.size() > 44 && left_wav[0] == 'R' && left_wav[1] == 'I' && left_wav[8] == 'W');
        EXPECT(right_wav.size() > 44 && stereo_wav.size() > left_wav.size());
        long long l_energy = 0, r_energy = 0;
        for (size_t off = 44; off + 3 < left_wav.size(); off += 4) {
            int16_t ls = static_cast<int16_t>(left_wav[off] | (left_wav[off + 1] << 8));
            int16_t rs = static_cast<int16_t>(left_wav[off + 2] | (left_wav[off + 3] << 8));
            l_energy += std::abs(static_cast<int>(ls));
            r_energy += std::abs(static_cast<int>(rs));
        }
        EXPECT(l_energy > 0 && r_energy == 0);
        long long rl_energy = 0, rr_energy = 0;
        for (size_t off = 44; off + 3 < right_wav.size(); off += 4) {
            int16_t ls = static_cast<int16_t>(right_wav[off] | (right_wav[off + 1] << 8));
            int16_t rs = static_cast<int16_t>(right_wav[off + 2] | (right_wav[off + 3] << 8));
            rl_energy += std::abs(static_cast<int>(ls));
            rr_energy += std::abs(static_cast<int>(rs));
        }
        EXPECT(rl_energy == 0 && rr_energy > 0);
    }
    EXPECT(is_valid_mnt_leaf("jd_ssd3000"));
    EXPECT(is_valid_mnt_leaf("nvme1n1p2"));
    EXPECT(!is_valid_mnt_leaf("../etc"));
    EXPECT(!is_valid_mnt_leaf("bad name"));
    EXPECT(unescape_lsblk_value("\\xe4\\xba\\xac\\xe4\\xb8\\x9cSSD3000") == "京东SSD3000");
    EXPECT(unescape_lsblk_value("SSD360GB\\xef\\xbc\\x88\\xe4\\xb8\\x8d\\xe5\\x86\\x8d\\xe4\\xbd\\xbf\\xe7\\x94\\xa8\\xef\\xbc\\x89") == "SSD360GB（不再使用）");
    EXPECT(unescape_lsblk_value("My\\x20Disk") == "My Disk");
    EXPECT(dispatch_ipc_command(&dummy, "devices:mount-ro:/dev/bad", mock_actions, &reply));
    EXPECT(dispatch_ipc_command(&dummy, "devices:fstab-set\ndev=/dev/bad\nname=x\nro=1", mock_actions, &reply));
    EXPECT(dispatch_ipc_command(&dummy, "devices:fstab-remove\ndev=/dev/bad", mock_actions, &reply));
    last_tab.clear();
    EXPECT(dispatch_ipc_command(&dummy, "settings:open:devices", mock_actions, &reply));
    EXPECT(last_tab == "devices");
    static bool doctor_opened = false;
    mock_actions.open_doctor = +[](AppState *) { doctor_opened = true; };
    EXPECT(dispatch_ipc_command(&dummy, "doctor:open", mock_actions, &reply));
    EXPECT(doctor_opened);
    static std::string monitor_tab;
    mock_actions.open_monitor = +[](AppState *, const std::string &t) { monitor_tab = t; };
    EXPECT(dispatch_ipc_command(&dummy, "monitor:open", mock_actions, &reply));
    EXPECT(monitor_tab == "procs");
    EXPECT(dispatch_ipc_command(&dummy, "monitor:open:logs", mock_actions, &reply));
    EXPECT(monitor_tab == "logs");
    EXPECT(dispatch_ipc_command(&dummy, "settings:open:procs", mock_actions, &reply));
    EXPECT(monitor_tab == "procs");

    // Network IPv4 & Netmask validation + safe IPC dispatch
    EXPECT(is_valid_ipv4_addr("192.168.31.189"));
    EXPECT(!is_valid_ipv4_addr("999.168.31.1"));
    int pfx = 0;
    EXPECT(parse_ipv4_mask_prefix("255.255.255.0", pfx) && pfx == 24);
    EXPECT(parse_ipv4_mask_prefix("/16", pfx) && pfx == 16);
    EXPECT(!parse_ipv4_mask_prefix("255.0.255.0", pfx));
    dummy.shutting_down.store(true);
    EXPECT(dispatch_ipc_command(&dummy, "settings:devices-get", mock_actions, &reply));
    EXPECT(dispatch_ipc_command(&dummy, "settings:audio-test-speaker:stop", mock_actions, &reply));
    EXPECT(dispatch_ipc_command(&dummy, "settings:audio-test-mic:stop", mock_actions, &reply));
    EXPECT(dispatch_ipc_command(&dummy, "ime:toggle", mock_actions, &reply));
    EXPECT(dispatch_ipc_command(&dummy, "desktop:new-folder", mock_actions, &reply));
    EXPECT(fs::exists(fs::path(home_dir()) / "Desktop" / "新建文件夹"));
    EXPECT(dispatch_ipc_command(&dummy, "settings:net-ip-apply\ndevice=eth0\nmethod=auto", mock_actions, &reply));

    // Workspace IPC dispatch (legacy string & structured JSON-RPC)
    static int ws_switched = 0;
    static int ws_step_total = 0;
    mock_actions.workspace_switch = +[](int idx1) { ws_switched = idx1; };
    mock_actions.workspace_step = +[](int delta) { ws_step_total += delta; };
    EXPECT(dispatch_ipc_command(&dummy, "workspace:switch:3", mock_actions, &reply));
    EXPECT(ws_switched == 3);
    reply.clear();
    EXPECT(dispatch_ipc_command(&dummy, R"({"id":21,"method":"workspace:switch","params":{"index":2}})", mock_actions, &reply));
    EXPECT(ws_switched == 2 && reply.find("\"ok\":true") != std::string::npos);
    EXPECT(dispatch_ipc_command(&dummy, "workspace:next", mock_actions, &reply));
    EXPECT(dispatch_ipc_command(&dummy, "workspace:prev", mock_actions, &reply));
    EXPECT(dispatch_ipc_command(&dummy, "workspace:next", mock_actions, &reply));
    EXPECT(ws_step_total == 1);

    // SNI StatusNotifierItem service parsing, DBusMenu label cleanup, and ARGB conversion
    std::string bus_n, obj_p, skey;
    EXPECT(parse_sni_service_id("/org/ayatana/NotificationItem/wechat/Menu", ":1.42", bus_n, obj_p, skey));
    EXPECT(bus_n == ":1.42" && obj_p == "/org/ayatana/NotificationItem/wechat/Menu");
    EXPECT(parse_sni_service_id("org.kde.StatusNotifierItem-99-1", ":1.99", bus_n, obj_p, skey));
    EXPECT(bus_n == "org.kde.StatusNotifierItem-99-1" && obj_p == "/StatusNotifierItem");
    EXPECT(parse_sni_service_id(":1.55/StatusNotifierItem", "", bus_n, obj_p, skey));
    EXPECT(bus_n == ":1.55" && obj_p == "/StatusNotifierItem" && skey == ":1.55/StatusNotifierItem");
    EXPECT(!parse_sni_service_id("/StatusNotifierItem", "", bus_n, obj_p, skey));
    EXPECT(!parse_sni_service_id("", ":1.1", bus_n, obj_p, skey));

    EXPECT(sanitize_sni_menu_label("_File") == "File");
    EXPECT(sanitize_sni_menu_label("__Help") == "_Help");
    EXPECT(sanitize_sni_menu_label("退出(_Q)") == "退出(Q)");

    const uint8_t be_px[8] = {128, 255, 0, 64, 255, 10, 20, 30};
    uint32_t out_px[2]{};
    EXPECT(convert_argb32_be_to_cairo_argb32(be_px, 2, 1, 8, reinterpret_cast<uint8_t *>(out_px)));
    EXPECT(out_px[0] == ((128u << 24) | (128u << 16) | (0u << 8) | 32u));
    EXPECT(out_px[1] == ((255u << 24) | (10u << 16) | (20u << 8) | 30u));
}

}  // namespace

int main() {
    TempHomeGuard guard;
    test_common_utils();
    test_theme_and_wallpaper();
    test_desktop_apps();
    test_steam_library();
    test_system_control_configs();
    test_ipc_router();

    if (g_failures > 0) {
        std::cerr << g_failures << " assertion(s) failed.\n";
        return 1;
    }
    std::cout << "All SpinelOS unit tests passed.\n";
    return 0;
}
