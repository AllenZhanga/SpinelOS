#include "test_harness.hpp"

namespace spinel_test {

namespace {

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

    // Fcitx5 Keep-Simple EN/ZH defaults (single global Ctrl+Space / Shift switching + Electron IME flags)
    ensure_fcitx5_defaults(true);
    std::ifstream prof_ifs(fs::path(home_dir()) / ".config/fcitx5/profile");
    std::ostringstream prof_buf;
    prof_buf << prof_ifs.rdbuf();
    EXPECT(prof_buf.str().find("DefaultIM=rime") != std::string::npos);

    std::ifstream gcfg_ifs(fs::path(home_dir()) / ".config/fcitx5/config");
    std::ostringstream gcfg_buf;
    gcfg_buf << gcfg_ifs.rdbuf();
    EXPECT(gcfg_buf.str().find("0=Control+space") != std::string::npos);
    EXPECT(gcfg_buf.str().find("0=Shift_L") != std::string::npos);
    EXPECT(gcfg_buf.str().find("1=Shift_R") != std::string::npos);
    EXPECT(gcfg_buf.str().find("ShowInputMethodInformation=False") != std::string::npos);
    EXPECT(gcfg_buf.str().find("ShareInputState=All") != std::string::npos);

    std::ifstream raddon_ifs(fs::path(home_dir()) / ".config/fcitx5/conf/rime.conf");
    std::ostringstream raddon_buf;
    raddon_buf << raddon_ifs.rdbuf();
    EXPECT(raddon_buf.str().find("SwitchInputMethodBehavior=Commit raw input") != std::string::npos);

    EXPECT(fs::exists(fs::path(home_dir()) / ".config/electron-flags.conf"));
    EXPECT(fs::exists(fs::path(home_dir()) / ".config/code-flags.conf"));

    std::ifstream rime_ifs(fs::path(home_dir()) / ".local/share/fcitx5/rime/default.custom.yaml");
    std::ostringstream rime_buf;
    rime_buf << rime_ifs.rdbuf();
    EXPECT(rime_buf.str().find("schema: luna_pinyin_simp") != std::string::npos);
    EXPECT(rime_buf.str().find("\"ascii_composer/switch_key/Shift_L\": noop") != std::string::npos);
    EXPECT(rime_buf.str().find("\"switcher/hotkeys\": []") != std::string::npos);

    // Traditional toggle persists and updates Rime schema custom switch
    set_ime_traditional(nullptr, true);
    EXPECT(query_ime_status().traditional == true);
    std::ifstream rice_cust_ifs(fs::path(home_dir()) / ".local/share/fcitx5/rime/rime_ice.custom.yaml");
    std::ostringstream rice_cust_buf;
    rice_cust_buf << rice_cust_ifs.rdbuf();
    EXPECT(rice_cust_buf.str().find("\"switches/@2/reset\": 1") != std::string::npos);
    set_ime_traditional(nullptr, false);
    EXPECT(query_ime_status().traditional == false);

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
    EXPECT(doc_checks.size() == 10);
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
    static uint64_t last_max_wid;
    static uint64_t last_min_wid;
    static int last_ws_switch;
    static int last_ws_step;
    last_tab.clear();
    last_store_focus.clear();
    cancel_called = false;
    last_max_wid = 0;
    last_min_wid = 0;
    last_ws_switch = 0;
    last_ws_step = 0;

    IpcHostActions mock_actions;
    mock_actions.open_settings = +[](AppState *, const std::string &tab) { last_tab = tab; };
    mock_actions.open_store = +[](AppState *, const std::string &focus) { last_store_focus = focus; };
    mock_actions.store_cancel = +[](AppState *) { cancel_called = true; };
    mock_actions.window_maximize = +[](uint64_t wid) { last_max_wid = wid; };
    mock_actions.window_minimize = +[](uint64_t wid) { last_min_wid = wid; };
    mock_actions.workspace_switch = +[](int idx1) { last_ws_switch = idx1; };
    mock_actions.workspace_step = +[](int delta) { last_ws_step = delta; };

    AppState dummy;
    std::string reply;
    EXPECT(dispatch_ipc_command(&dummy, "monitor:temp-clean", mock_actions, &reply));
    EXPECT(dispatch_ipc_command(&dummy, R"({"id":"mem","method":"monitor.memory-clean"})", mock_actions, &reply));
    EXPECT(reply.find("\"id\":\"mem\"") != std::string::npos && reply.find("\"ok\":true") != std::string::npos);
    EXPECT(dispatch_ipc_command(&dummy, R"({"id":"r1","method":"settings.open","params":{"tab":"appearance"}})", mock_actions, &reply));
    EXPECT(last_tab == "appearance");
    EXPECT(reply.find("\"id\":\"r1\"") != std::string::npos);
    EXPECT(reply.find("\"ok\":true") != std::string::npos);

    reply.clear();
    EXPECT(dispatch_ipc_command(&dummy, "store:open:steam", mock_actions, &reply));
    EXPECT(last_store_focus == "steam");
    EXPECT(reply.empty());  // Legacy string without id produces no reply envelope

    EXPECT(dispatch_ipc_command(&dummy, R"({"id":7,"method":"app.open-location","params":{"id":"Mono Cloud"}})", mock_actions, &reply));
    EXPECT(reply.find("\"id\":7") != std::string::npos && reply.find("\"ok\":true") != std::string::npos);
    EXPECT(dispatch_ipc_command(&dummy, R"({"id":8,"method":"app.remove-shortcut","params":{"id":"Mono Cloud"}})", mock_actions, &reply));
    EXPECT(reply.find("\"id\":8") != std::string::npos && reply.find("\"ok\":true") != std::string::npos);

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

    EXPECT(dispatch_ipc_command(&dummy, "window:maximize:42", mock_actions, &reply));
    EXPECT(last_max_wid == 42);
    EXPECT(dispatch_ipc_command(&dummy, "window:minimize:43", mock_actions, &reply));
    EXPECT(last_min_wid == 43);
    EXPECT(dispatch_ipc_command(&dummy, "workspace:switch:3", mock_actions, &reply));
    EXPECT(last_ws_switch == 3);
    EXPECT(dispatch_ipc_command(&dummy, "workspace:next", mock_actions, &reply));
    EXPECT(last_ws_step == 1);
    EXPECT(dispatch_ipc_command(&dummy, "workspace:prev", mock_actions, &reply));
    EXPECT(last_ws_step == -1);

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
    EXPECT(dispatch_ipc_command(&dummy, "ime:mode:en", mock_actions, &reply));
    EXPECT(dispatch_ipc_command(&dummy, "ime:mode:zh", mock_actions, &reply));
    EXPECT(dispatch_ipc_command(&dummy, "ime:trad:trad", mock_actions, &reply));
    EXPECT(dispatch_ipc_command(&dummy, "ime:trad:simp", mock_actions, &reply));
    EXPECT(dispatch_ipc_command(&dummy, "ime:toggle-trad", mock_actions, &reply));
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

    // Dock full app quit IPC & Store service/AI helpers
    static uint64_t quit_wid = 0;
    mock_actions.window_quit = +[](uint64_t wid) { quit_wid = wid; };
    EXPECT(dispatch_ipc_command(&dummy, "window:quit:42", mock_actions, &reply));
    EXPECT(quit_wid == 42);

    static std::string last_store_action;
    static std::string last_store_payload;
    mock_actions.store_action = +[](AppState *, const std::string &act, const std::string &pl) {
        last_store_action = act;
        last_store_payload = pl;
    };
    EXPECT(dispatch_ipc_command(&dummy, "store:svc-start\nid=ollama\npkg=ollama", mock_actions, &reply));
    EXPECT(last_store_action == "svc-start" && last_store_payload.find("id=ollama") != std::string::npos);
    EXPECT(dispatch_ipc_command(&dummy, "store:webapp-pin\nid=chatgpt", mock_actions, &reply));
    EXPECT(last_store_action == "webapp-pin");
}

void test_dock_quit_and_store_helpers() {
    EXPECT(is_protected_system_process("spinel-shell"));
    EXPECT(is_protected_system_process("labwc"));
    EXPECT(is_protected_system_process("Xwayland"));
    EXPECT(is_protected_system_process("pipewire"));
    EXPECT(!is_protected_system_process("steam"));
    EXPECT(!is_protected_system_process("steamwebhelper"));

    const auto steam_names = derive_app_quit_process_names("steam", "Steam", "");
    EXPECT(!steam_names.empty());
    bool has_steam = false, has_webhelper = false;
    for (const auto &n : steam_names) {
        if (n == "steam") has_steam = true;
        if (n == "steamwebhelper") has_webhelper = true;
    }
    EXPECT(has_steam && has_webhelper);

    const auto chrome_names = derive_app_quit_process_names("google-chrome", "Google Chrome", "");
    EXPECT(!chrome_names.empty());

    EXPECT(select_ollama_pacman_pkg(true, false) == "ollama-cuda");
    EXPECT(select_ollama_pacman_pkg(false, true) == "ollama-rocm");
    EXPECT(select_ollama_pacman_pkg(false, false) == "ollama");

    EXPECT(is_valid_store_service_unit("ollama"));
    EXPECT(is_valid_store_service_unit("vllm"));
    EXPECT(is_valid_store_service_unit("docker"));
    EXPECT(is_valid_store_service_unit("cups"));
    EXPECT(!is_valid_store_service_unit("systemd-journald"));
    EXPECT(is_user_systemd_service_unit("vllm"));
    EXPECT(!is_user_systemd_service_unit("ollama"));

    EXPECT(is_valid_https_url("https://chatgpt.com"));
    EXPECT(is_valid_https_url("https://antigravity.google"));
    EXPECT(!is_valid_https_url("http://example.com"));
    EXPECT(!is_valid_https_url("https://evil.com/\"rm -rf\""));
    EXPECT(is_supported_native_app_id("github-desktop"));
    EXPECT(is_supported_native_app_id("vscode"));
    EXPECT(!is_supported_native_app_id("unknown-native-app"));

    // Dock mode config & Btrfs Snapper parsing / validation
    EXPECT(is_valid_dock_mode("magnify"));
    EXPECT(is_valid_dock_mode("fixed"));
    EXPECT(is_valid_dock_mode("autohide"));
    EXPECT(!is_valid_dock_mode("invalid-mode"));

    DockConfig dcfg;
    dcfg.mode = "autohide";
    EXPECT(save_dock_config(dcfg));
    EXPECT(load_dock_config().mode == "autohide");

    int parsed_id = 0;
    EXPECT(!is_valid_snapshot_id("0", parsed_id));
    EXPECT(!is_valid_snapshot_id("-1", parsed_id));
    EXPECT(!is_valid_snapshot_id("12;rm", parsed_id));
    EXPECT(is_valid_snapshot_id("15", parsed_id) && parsed_id == 15);
    EXPECT(is_valid_snapper_desc("Before kernel upgrade"));

    const std::string csv =
        "number,type,date,user,cleanup,description\n"
        "0,single,,, ,current\n"
        "12,single,2026-11-26 10:00:00,root,number,Initial install\n"
        "15*,single,2026-11-26 11:30:00,root,number,\"Before GPU driver, v2\"\n";
    auto snaps = parse_snapper_csv(csv);
    EXPECT(snaps.size() == 2);
    EXPECT(snaps[0].id == 15 && snaps[0].is_current && snaps[0].description == "Before GPU driver, v2");
    EXPECT(snaps[1].id == 12 && !snaps[1].is_current && snaps[1].description == "Initial install");
}

void test_temp_memory_cleanup() {
    char path[] = "/dev/shm/spinel-temp-memory-test.XXXXXX";
    EXPECT(g_mkdtemp(path) != nullptr);
    if (!tmpfs_used_bytes(path)) {
        rmdir(path);
        return;
    }
    const fs::path root(path);

    auto make_directory = [&](const char *name) {
        const fs::path dir = root / name;
        fs::create_directory(dir);
        fs::permissions(dir, fs::perms::owner_all);
        std::ofstream(dir / "data") << std::string(2 * 1024 * 1024, 'x');
        return dir;
    };
    const auto stale = make_directory("spinel-stale");
    const auto busy = make_directory("spinel-in-use");
    const auto unrelated = make_directory("spinel-unmarked");
    fs::create_directory_symlink(unrelated, root / "spinel-link");
    fs::create_directory_symlink(unrelated, stale / "external-link");

    const int stale_lock = register_temp_directory(stale);
    EXPECT(stale_lock >= 0);
    if (stale_lock >= 0) close(stale_lock);
    const int busy_lock = register_temp_directory(busy);
    EXPECT(busy_lock >= 0);

    const auto result = clean_temp_memory(root);
    EXPECT(result.directories_removed == 1);
    EXPECT(result.released_bytes && *result.released_bytes >= 2 * 1024 * 1024);
    EXPECT(!result.incomplete);
    EXPECT(!fs::exists(stale));
    EXPECT(fs::exists(busy / "data"));
    EXPECT(fs::exists(unrelated / "data"));
    EXPECT(fs::is_symlink(root / "spinel-link"));

    if (busy_lock >= 0) close(busy_lock);
    EXPECT(clean_temp_memory(root).directories_removed == 1);
    EXPECT(clean_temp_memory(root).directories_removed == 0);
    fs::remove_all(root);
}


}  // namespace

void run_sys_tests() {
    test_system_control_configs();
    test_ipc_router();
    test_dock_quit_and_store_helpers();
    test_temp_memory_cleanup();
}

}  // namespace spinel_test
