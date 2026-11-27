#include "test_harness.hpp"

namespace spinel_test {

namespace {

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

    // Verify ensure_display_env() recovers DBUS_SESSION_BUS_ADDRESS from $XDG_RUNTIME_DIR/bus even when DISPLAY is set
    const char *old_rt = std::getenv("XDG_RUNTIME_DIR");
    const std::string saved_rt = old_rt ? old_rt : "";
    const bool had_rt = old_rt != nullptr;
    const char *old_dbus = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    const std::string saved_dbus = old_dbus ? old_dbus : "";
    const bool had_dbus = old_dbus != nullptr;
    const char *old_disp = std::getenv("DISPLAY");
    const std::string saved_disp = old_disp ? old_disp : "";
    const bool had_disp = old_disp != nullptr;
    const char *old_xdg = std::getenv("XDG_CURRENT_DESKTOP");
    const std::string saved_xdg = old_xdg ? old_xdg : "";
    const bool had_xdg = old_xdg != nullptr;

    fs::path bus_sock = fs::path(home_dir()) / "bus";
    int sock_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    EXPECT(sock_fd >= 0);
    if (sock_fd >= 0) {
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", bus_sock.c_str());
        EXPECT(::bind(sock_fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0);

        setenv("XDG_RUNTIME_DIR", home_dir().c_str(), 1);
        unsetenv("DBUS_SESSION_BUS_ADDRESS");
        setenv("DISPLAY", ":99", 1);
        unsetenv("XDG_CURRENT_DESKTOP");

        ensure_display_env();

        const char *got_dbus = std::getenv("DBUS_SESSION_BUS_ADDRESS");
        EXPECT(got_dbus != nullptr && std::string(got_dbus) == ("unix:path=" + bus_sock.string()));
        const char *got_xdg = std::getenv("XDG_CURRENT_DESKTOP");
        EXPECT(got_xdg != nullptr && std::string(got_xdg).find("GNOME") != std::string::npos);

        ::close(sock_fd);
    }

    if (had_rt) setenv("XDG_RUNTIME_DIR", saved_rt.c_str(), 1); else unsetenv("XDG_RUNTIME_DIR");
    if (had_dbus) setenv("DBUS_SESSION_BUS_ADDRESS", saved_dbus.c_str(), 1); else unsetenv("DBUS_SESSION_BUS_ADDRESS");
    if (had_disp) setenv("DISPLAY", saved_disp.c_str(), 1); else unsetenv("DISPLAY");
    if (had_xdg) setenv("XDG_CURRENT_DESKTOP", saved_xdg.c_str(), 1); else unsetenv("XDG_CURRENT_DESKTOP");
}

void test_wine_launch() {
    const fs::path base = fs::path(home_dir()) / "wine-test";
    const fs::path program = base / "应用 1 & setup.ExE";
    const fs::path runner = base / "bin" / "wine";
    const fs::path font_matcher = base / "bin" / "fc-match";
    const fs::path args_file = base / "launch.args";
    const fs::path prefix = spinel_wine_prefix();
    const fs::path registry = prefix / "mock-registry";
    EXPECT(write_text_file(program.string(), "Windows program placeholder"));
    EXPECT(write_text_file((registry / "SimSun").string(), "User-picked font\n"));
    EXPECT(write_text_file(font_matcher.string(), "#!/bin/sh\nprintf '%s' 'Noto Sans CJK SC'\n"));
    EXPECT(write_text_file(runner.string(),
                           "#!/bin/sh\n"
                           "test -d \"$(dirname -- \"$WINEPREFIX\")\" || exit 1\n"
                           "if [ \"$1\" = 'C:\\windows\\system32\\reg.exe' ]; then\n"
                           "  value=\"$WINEPREFIX/mock-registry/$5\"\n"
                           "  case \"$2\" in\n"
                           "    query) test -f \"$value\";;\n"
                           "    add) printf '%s\\n' \"$9\" > \"$value\";;\n"
                           "  esac\n"
                           "  exit $?\n"
                           "fi\n"
                           "printf '%s\\n' \"$#\" \"$1\" \"$PWD\" \"$WINEPREFIX\" > launch.args\n"));
    fs::permissions(runner, fs::perms::owner_exec, fs::perm_options::add);
    fs::permissions(font_matcher, fs::perms::owner_exec, fs::perm_options::add);
    fs::create_directory(base / "directory.exe");
    EXPECT(is_windows_executable(program));
    EXPECT(!is_windows_executable(base / "missing.exe"));
    EXPECT(!is_windows_executable(base / "directory.exe"));
    EXPECT(!is_windows_executable(runner));

    const char *path_env = std::getenv("PATH");
    const bool had_path = path_env != nullptr;
    const std::string original_path = had_path ? path_env : "";
    setenv("PATH", (runner.parent_path().string() + ":" SPINEL_TEST_WINE_HELPER_DIR ":/usr/bin").c_str(), 1);
    g_autoptr(GError) spawn_error = nullptr;
    g_autoptr(GSubprocess) process = launch_windows_executable(program, &spawn_error);
    if (had_path) setenv("PATH", original_path.c_str(), 1);
    else unsetenv("PATH");
    EXPECT(process != nullptr);
    if (!process) return;
    EXPECT(g_subprocess_wait_check(process, nullptr, nullptr));

    const fs::path canonical_program = fs::canonical(program);
    const fs::path working_dir = canonical_program.parent_path();
    std::ifstream args(args_file);
    std::vector<std::string> actual;
    for (std::string line; std::getline(args, line);) actual.push_back(std::move(line));
    const std::vector<std::string> expected = {
        "1", canonical_program.string(), working_dir.string(), prefix.string()
    };
    EXPECT(actual == expected);
    EXPECT(read_first_line((registry / "SimSun").string(), "") == "User-picked font");
    EXPECT(read_first_line((registry / "NSimSun").string(), "") == "Noto Sans CJK SC");
    EXPECT(read_first_line((registry / "Microsoft YaHei UI").string(), "") == "Noto Sans CJK SC");
    EXPECT(fs::is_regular_file(prefix / ".spinelos-cjk-fonts-v1"));
}

void test_fcitx_notification_defaults() {
    const fs::path path = fs::path(g_get_user_config_dir()) / "fcitx5/conf/notifications.conf";
    EXPECT(write_text_file(path.string(), "[HiddenNotifications]\n0=existing-tip\n"));
    ensure_fcitx5_defaults(true);
    ensure_fcitx5_defaults(true);

    g_autoptr(GKeyFile) config = g_key_file_new();
    EXPECT(g_key_file_load_from_file(config, path.c_str(), G_KEY_FILE_NONE, nullptr));
    g_autofree char *original = g_key_file_get_string(config, "HiddenNotifications", "0", nullptr);
    g_autofree char *hidden = g_key_file_get_string(config, "HiddenNotifications", "1", nullptr);
    EXPECT(g_strcmp0(original, "existing-tip") == 0);
    EXPECT(g_strcmp0(hidden, "wayland-diagnose-gnome") == 0);
    EXPECT(!g_key_file_has_key(config, "HiddenNotifications", "2", nullptr));
}

void test_file_associations() {
    const fs::path base = fs::path(home_dir()) / "associated-files";
    EXPECT(write_text_file((base / "notes.txt").string(), "a plain text document\n"));
    EXPECT(write_text_file((base / "second.txt").string(), "another document\n"));
    EXPECT(write_text_file((base / "document.PDF").string(), "%PDF-1.4\n"));
    EXPECT(write_text_file((base / "Application.EXE").string(), "fixture with an uppercase extension\n"));
    auto query = [&](const char *name) {
        g_autoptr(GFile) file = g_file_new_for_path((base / name).c_str());
        return query_file_open_type(file, nullptr);
    };
    const auto text = query("notes.txt");
    const auto pdf = query("document.PDF");
    const auto exe = query("Application.EXE");
    EXPECT(text && text->content_type == "text/plain" && !text->windows_exe && text->can_set_default());
    EXPECT(pdf && pdf->content_type == "application/pdf" && !pdf->windows_exe);
    EXPECT(exe && exe->content_type == "application/vnd.microsoft.portable-executable" && exe->windows_exe);
    EXPECT(!query("."));
    if (!text) return;

    const fs::path apps_dir = fs::path(g_get_user_data_dir()) / "applications";
    EXPECT(write_text_file((apps_dir / "spinel-test-editor.desktop").string(),
        "[Desktop Entry]\nType=Application\nName=Test editor\nExec=/usr/bin/true %f\nMimeType=text/plain;\n"));
    g_autoptr(GAppInfo) editor = nullptr;
    GList *apps = g_app_info_get_all();
    for (auto *entry = apps; entry; entry = entry->next) {
        auto *candidate = G_APP_INFO(entry->data);
        if (g_strcmp0(g_app_info_get_id(candidate), "spinel-test-editor.desktop") == 0) {
            editor = G_APP_INFO(g_object_ref(candidate));
            break;
        }
    }
    g_list_free_full(apps, g_object_unref);
    EXPECT(editor != nullptr);
    if (!editor) return;
    EXPECT(text->set_default(editor, nullptr));
    const auto second = query("second.txt");
    EXPECT(second && second->content_type == text->content_type);
    g_autoptr(GAppInfo) default_editor = g_app_info_get_default_for_type(text->content_type.c_str(), false);
    EXPECT(default_editor && g_app_info_equal(default_editor, editor));
}

void test_trash_restore() {
    const fs::path base = fs::path(home_dir()) / "restore-test";
    const fs::path source = base / "trash" / "report.txt";
    const fs::path original = base / "documents" / "report.txt";
    EXPECT(write_text_file(source.string(), "trashed content"));
    EXPECT(write_text_file(original.string(), "existing content"));
    const fs::path dangling_link = original.parent_path() / "report (恢复 2).txt";
    fs::create_symlink("missing.txt", dangling_link);

    std::string error;
    fs::path target = original;
    EXPECT(restore_trashed_path(source, target, error));
    EXPECT(target == original.parent_path() / "report (恢复 3).txt");
    EXPECT(read_first_line(target.string()) == "trashed content");
    EXPECT(read_first_line(original.string()) == "existing content");
    EXPECT(fs::is_symlink(dangling_link));
    EXPECT(!fs::exists(source));

    EXPECT(write_text_file(source.string(), "next trashed content"));
    for (int i = 4; i < 100; ++i) {
        fs::path occupied = original.parent_path() / ("report (恢复 " + std::to_string(i) + ").txt");
        EXPECT(write_text_file(occupied.string(), "occupied"));
    }
    target = original;
    EXPECT(!restore_trashed_path(source, target, error));
    EXPECT(!error.empty());
    EXPECT(read_first_line(source.string()) == "next trashed content");
    EXPECT(read_first_line(original.string()) == "existing content");
    EXPECT(read_first_line((original.parent_path() / "report (恢复 99).txt").string()) == "occupied");

    const fs::path source_dir = base / "trash" / "my.dir";
    const fs::path original_dir = base / "documents" / "my.dir";
    EXPECT(write_text_file((source_dir / "payload").string(), "trashed directory content"));
    EXPECT(write_text_file((original_dir / "payload").string(), "existing directory content"));
    target = original_dir;
    EXPECT(restore_trashed_path(source_dir, target, error));
    EXPECT(target == original_dir.parent_path() / "my.dir (恢复 2)");
    EXPECT(read_first_line((target / "payload").string()) == "trashed directory content");
    EXPECT(read_first_line((original_dir / "payload").string()) == "existing directory content");
    EXPECT(!fs::exists(source_dir));
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

    const fs::path portable = fs::path(home_dir()) / "Mono Cloud & 中文.AppImage";
    const fs::path shortcut = app_dir / "spinel-portable-demo.desktop";
    EXPECT(write_text_file(shortcut.string(), "[Desktop Entry]\nType=Application\nName=Portable Demo\nExec=\"" +
                           portable.string() + "\" --sample %U\n"));

    std::vector<std::string> argv;
    std::string error;
    EXPECT(!resolve_desktop_app_argv("spinel-portable-demo", argv, error));
    EXPECT(argv.empty() && error.find("找不到可执行文件") != std::string::npos);
    EXPECT(error.find(portable.string()) != std::string::npos);
    EXPECT(find_local_app_shortcut("spinel-portable-demo").reveal_path == portable.parent_path().string());

    EXPECT(write_text_file(portable.string(), "#!/bin/sh\nexit 0\n"));
    fs::permissions(portable, fs::perms::owner_read | fs::perms::owner_write);
    EXPECT(!resolve_desktop_app_argv("spinel-portable-demo", argv, error));
    EXPECT(argv.empty() && error.find("执行权限") != std::string::npos);

    fs::permissions(portable, fs::perms::owner_all);
    EXPECT(resolve_desktop_app_argv("spinel-portable-demo", argv, error));
    EXPECT((argv == std::vector<std::string>{portable.string(), "--sample"} && error.empty()));
    EXPECT(find_local_app_shortcut("spinel-portable-demo").reveal_path == portable.string());
    EXPECT(find_local_app_shortcut("../spinel-portable-demo").shortcut_path.empty());

    EXPECT(write_text_file(shortcut.string(), "[Desktop Entry]\nType=Application\nName=Portable Demo\nExec=env TEST_FLAG=one \"" +
                           portable.string() + "\" %U\n"));
    EXPECT(resolve_desktop_app_argv("spinel-portable-demo", argv, error));
    EXPECT((argv == std::vector<std::string>{"env", "TEST_FLAG=one", portable.string()}));
    const fs::path moved = portable.parent_path() / "saved-portable";
    fs::rename(portable, moved);
    EXPECT(!resolve_desktop_app_argv("spinel-portable-demo", argv, error));
    EXPECT(argv.empty() && error.find(portable.string()) != std::string::npos);
    fs::rename(moved, portable);

    const fs::path bin = fs::path(home_dir()) / "shortcut-test-bin";
    fs::create_directories(bin);
    const auto mock = [&](const std::string &name, const std::string &script) {
        const auto p = bin / name;
        EXPECT(write_text_file(p.string(), "#!/bin/sh\n" + script));
        fs::permissions(p, fs::perms::owner_all);
        return p;
    };
    const char *path_env = std::getenv("PATH");
    const bool had_path = path_env != nullptr;
    const std::string old_path = path_env ? path_env : "";
    setenv("PATH", (bin.string() + ":" + old_path).c_str(), 1);
    mock("spinel-files", "printf '%s\\n' \"$1\" > \"$HOME/location-capture\"\n");
    mock("spinel-terminal", "printf '%s\\n' \"$@\" > \"$HOME/terminal-capture\"\n");
    mock("notify-send", "printf '%s\\n' \"$@\" > \"$HOME/launch-error-capture\"\n");
    const fs::path browser = mock("chrome-test", "printf '%s\\n' \"$TEST_FLAG\" \"$PWD\" \"$@\" > \"$HOME/browser-capture\"\n");
    const auto capture = [](const std::string &name, const std::string &last) {
        const auto path = fs::path(home_dir()) / name;
        for (int i = 0; i < 100; ++i) {
            std::ifstream file(path);
            const std::string value{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            if (value.ends_with(last + "\n")) return value;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return std::string{};
    };
    EXPECT(execute_launchpad_app_action("spinel-portable-demo", false, error));
    EXPECT(capture("location-capture", portable.string()) == portable.string() + "\n");
    EXPECT(execute_launchpad_app_action("spinel-portable-demo", true, error));
    EXPECT(error.find("已将快捷方式移到回收站") != std::string::npos);
    EXPECT(!fs::exists(shortcut) && fs::exists(portable));
    EXPECT(!execute_launchpad_app_action("spinel-portable-demo", false, error));
    EXPECT(error.find("找不到应用所在目录") != std::string::npos);

    const auto terminal_file = fs::path(home_dir()) / "shortcut-terminal.desktop";
    EXPECT(write_text_file(terminal_file.string(), "[Desktop Entry]\nType=Application\nName=Terminal Test\n"
                           "Terminal=true\nIcon=terminal-test\nExec=\"" + portable.string() + "\" %i %c %k %% %f\n"));
    launch_desktop_app_file(terminal_file.string());
    EXPECT(capture("terminal-capture", "%") == "--\n" + portable.string() + "\n--icon\nterminal-test\nTerminal Test\n" +
           terminal_file.string() + "\n%\n");

    const auto browser_file = fs::path(home_dir()) / "shortcut-browser.desktop";
    EXPECT(write_text_file(browser_file.string(), "[Desktop Entry]\nType=Application\nName=Browser Test\nPath=" + bin.string() +
                           "\nExec=env \"PATH=" + bin.string() + "\" TEST_FLAG=one chrome-test %U\n"));
    launch_desktop_app_file(browser_file.string());
    const auto browser_capture = capture("browser-capture", "--wayland-text-input-version=3");
    EXPECT(browser_capture.starts_with("one\n" + bin.string() + "\n"));
    EXPECT(browser_capture.find("--ozone-platform=wayland\n--enable-wayland-ime\n") != std::string::npos);

    fs::remove(browser);
    launch_desktop_app_file(browser_file.string());
    const auto launch_error = capture("launch-error-capture", "应用可能已移动或卸载，请重新安装或更新快捷方式。");
    EXPECT(launch_error.find("无法启动「Browser Test」") != std::string::npos);
    EXPECT(launch_error.find("找不到可执行文件：chrome-test") != std::string::npos);

    mock("gamemoderun", "export TEST_GAMEMODE=on\nexec \"$@\"\n");
    mock("mangohud", "export TEST_MANGOHUD=on\nexec \"$@\"\n");
    const auto game = mock("perf-test", "printf '%s\\n' \"$TEST_GAMEMODE\" \"$TEST_MANGOHUD\" ok > \"$HOME/game-capture\"\n");
    const auto game_file = fs::path(home_dir()) / "shortcut-game.desktop";
    EXPECT(write_text_file(game_file.string(), "[Desktop Entry]\nType=Application\nName=Game Test\nCategories=Game;\nExec=\"" +
                           game.string() + "\"\n"));
    launch_desktop_app_file(game_file.string());
    EXPECT(capture("game-capture", "ok") == "on\non\nok\n");

    if (had_path) setenv("PATH", old_path.c_str(), 1);
    else unsetenv("PATH");

    EXPECT(!resolve_desktop_app_argv("spinel-portable-demo", argv, error));
    EXPECT(argv.empty() && error.find("找不到应用快捷方式") != std::string::npos);
}

void test_wine_app_catalog() {
    const auto prefix = spinel_wine_prefix();
    const auto drive = prefix / "drive_c";
    const auto shortcut_dir = fs::path(g_get_user_data_dir()) / "applications/wine/Programs/7-Zip";
    const auto link = drive / "ProgramData/Start Menu/7-Zip File Manager.lnk";
    const auto shortcut = shortcut_dir / "7-Zip File Manager.desktop";
    const std::string exec_tail = R"(" wine "C:\\\\ProgramData\\\\Start Menu\\\\7-Zip File Manager.lnk")";
    const std::string contents = "[Desktop Entry]\nType=Application\nName=7-Zip File Manager\n"
                                 "StartupWMClass=7zfm.exe\nExec=env \"WINEPREFIX=";
    EXPECT(write_text_file(link.string(), "Wine start-menu shortcut"));
    EXPECT(write_text_file(shortcut.string(), contents + prefix.string() + exec_tail + "\n"));
    EXPECT(write_text_file((shortcut_dir / "Other Prefix.desktop").string(),
                          contents + prefix.string() + "-other" + exec_tail + "\n"));

    const auto installed = parse_wine_uninstall_registry(R"registry(Windows Registry Editor Version 5.00

[HKEY_LOCAL_MACHINE\Software\Uninstall\7-Zip]
"DisplayName"="7-Zip 26.06 (x64)"
"UninstallString"="uninstall.exe"

[HKEY_LOCAL_MACHINE\Software\Uninstall\应用]
"DisplayName"="示例应用"
"WindowsInstaller"=dword:00000001

[HKEY_LOCAL_MACHINE\Software\Uninstall\System]
"DisplayName"="System component"
"WindowsInstaller"=dword:00000001
"SystemComponent"=dword:00000001

[HKEY_LOCAL_MACHINE\Software\Uninstall\7-Zip\Features]
"DisplayName"="Not an installed application"
"UninstallString"="uninstall.exe"
)registry", R"(HKEY_LOCAL_MACHINE\Software\Uninstall)");
    EXPECT(installed.size() == 2);
    if (installed.size() != 2) return;
    EXPECT(installed[1].name == "示例应用");
    EXPECT(installed[1].key == "应用");
    const auto shortcuts = list_wine_shortcuts();
    EXPECT(shortcuts.size() == 1);
    if (shortcuts.size() != 1) return;
    EXPECT(shortcuts[0].link == fs::canonical(link));
    EXPECT(find_wine_app_shortcut(installed[0], shortcuts) == &shortcuts[0]);
    EXPECT(find_wine_app_shortcut(installed[1], shortcuts) == nullptr);
    EXPECT(std::ranges::any_of(list_desktop_apps(), [&](const auto &app) {
        return app.id == shortcuts[0].id && app.name == "7-Zip File Manager";
    }));
    fs::remove(link);
    EXPECT(list_wine_shortcuts().empty());
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


}  // namespace

void run_core_tests() {
    test_common_utils();
    test_wine_launch();
    test_fcitx_notification_defaults();
    test_file_associations();
    test_trash_restore();
    test_theme_and_wallpaper();
    test_desktop_apps();
    test_wine_app_catalog();
    test_steam_library();
}

}  // namespace spinel_test
