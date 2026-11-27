#include "ai/agent_bus.hpp"
#include "ai/agent_perms.hpp"
#include "ai/agent_tracker.hpp"
#include "ai/json_util.hpp"
#include "ai/mcp_server.hpp"
#include "ai/os_atomics.hpp"
#include "ai/task_inbox.hpp"
#include "app_store.hpp"
#include "common.hpp"
#include "desktop_apps.hpp"
#include "file_associations.hpp"
#include "ipc_router.hpp"
#include "native_shell.hpp"
#include "relay_client.hpp"
#include "sandbox_manager.hpp"
#include "sandbox_stream_codec.hpp"
#include "shell/sni_tray.hpp"
#include "steam_library.hpp"
#include "system_control.hpp"
#include "temp_memory.hpp"
#include "theme_manager.hpp"
#include "wine_launcher.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
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
void native_shell_sync_dock(AppState *) {}
void native_shell_sync_perf(AppState *) {}
void native_shell_toggle_cc(AppState *) {}
void native_shell_set_cc_visible(AppState *, bool) {}
void native_shell_toggle_launcher(AppState *) {}
void native_shell_sync_sandbox(AppState *, const std::string &) {}
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
    std::string old_data_home;
    bool had_data_home = false;
    std::string old_config_home;
    bool had_config_home = false;
    fs::path tmp_dir;

    TempHomeGuard() {
        const char *h = std::getenv("HOME");
        if (h) {
            had_home = true;
            old_home = h;
        }
        if (const char *data_home = std::getenv("XDG_DATA_HOME")) {
            had_data_home = true;
            old_data_home = data_home;
        }
        if (const char *config_home = std::getenv("XDG_CONFIG_HOME")) {
            had_config_home = true;
            old_config_home = config_home;
        }
        tmp_dir = fs::temp_directory_path() / ("spinel-test-" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(tmp_dir, ec);
        fs::create_directories(tmp_dir, ec);
        setenv("HOME", tmp_dir.c_str(), 1);
        setenv("XDG_DATA_HOME", (tmp_dir / "data").c_str(), 1);
        setenv("XDG_CONFIG_HOME", (tmp_dir / "config").c_str(), 1);
    }

    ~TempHomeGuard() {
        if (had_home) {
            setenv("HOME", old_home.c_str(), 1);
        } else {
            unsetenv("HOME");
        }
        if (had_data_home) setenv("XDG_DATA_HOME", old_data_home.c_str(), 1);
        else unsetenv("XDG_DATA_HOME");
        if (had_config_home) setenv("XDG_CONFIG_HOME", old_config_home.c_str(), 1);
        else unsetenv("XDG_CONFIG_HOME");
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

void test_parallel_sandbox() {
    std::string norm, err;
    EXPECT(!is_valid_sandbox_mount_dir("/", norm, err));
    EXPECT(!is_valid_sandbox_mount_dir(home_dir(), norm, err));
    EXPECT(!is_valid_sandbox_mount_dir("relative/path", norm, err));

    const fs::path ws = fs::path(home_dir()) / "ai-workspace";
    fs::create_directories(ws);
    EXPECT(is_valid_sandbox_mount_dir(ws.string() + "/", norm, err));
    EXPECT(norm == fs::weakly_canonical(ws).string());

    EXPECT(add_sandbox_mount_dir(ws.string(), true, err));
    SandboxConfig loaded = load_sandbox_config();
    EXPECT(loaded.allow_network);
    EXPECT(loaded.sync_clipboard);
    EXPECT(loaded.perf_mode == "host-priority");
    EXPECT(loaded.rw_dirs.size() == 1);
    EXPECT(loaded.rw_dirs[0] == norm);

    const std::string fake_home = sandbox_home_dir();
    const std::string rt_dir = sandbox_runtime_dir();
    auto argv_shared = build_sandbox_bwrap_argv(loaded, rt_dir, fake_home, home_dir(), 1000, {"spinel-terminal"}, norm);
    auto has_arg = [&](const std::vector<std::string> &v, const std::string &a) {
        return std::find(v.begin(), v.end(), a) != v.end();
    };
    EXPECT(!has_arg(argv_shared, "--unshare-net"));
    EXPECT(has_arg(argv_shared, "/run/user/1000"));
    EXPECT(has_arg(argv_shared, fake_home));
    EXPECT(has_arg(argv_shared, norm));

    loaded.allow_network = false;
    auto argv_offline = build_sandbox_bwrap_argv(loaded, rt_dir, fake_home, home_dir(), 1000, {"chromium"});
    EXPECT(has_arg(argv_offline, "--unshare-net"));

    std::string rc = build_sandbox_rc_xml(loaded);
    EXPECT(rc.find("<labwc_config>") != std::string::npos);
    EXPECT(rc.find("spinel-shell --sandbox-launcher") != std::string::npos);
    EXPECT(has_arg(argv_shared, "SPINEL_SANDBOX_SHELL"));

    EXPECT(remove_sandbox_mount_dir(norm));
    EXPECT(load_sandbox_config().rw_dirs.empty());

    EXPECT(add_sandbox_app("My Terminal", "launch:terminal", true, err));
    EXPECT(add_sandbox_app("Browser", "launch:browser", false, err));
    auto apps = load_sandbox_config().apps;
    bool found_term = false, found_browser = false;
    for (const auto &a : apps) {
        if (a.cmd == "launch:terminal" && a.auto_start) found_term = true;
        if (a.cmd == "launch:browser" && !a.auto_start) found_browser = true;
    }
    EXPECT(found_term && found_browser);
    EXPECT(set_sandbox_app_autostart("launch:browser", true));
    EXPECT(remove_sandbox_app("launch:terminal"));
    EXPECT(load_sandbox_config().apps.size() == 1);
    EXPECT(load_sandbox_config().apps[0].cmd == "launch:browser" && load_sandbox_config().apps[0].auto_start);

    AppState dummy;
    std::string ipc_rep;
    EXPECT(dispatch_ipc_command(&dummy, "{\"id\":99,\"method\":\"sandbox:state-get\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("\"ok\":true") != std::string::npos);

    AppState sb_state;
    sb_state.sandbox_shell_mode = true;
    ipc_rep.clear();
    EXPECT(!dispatch_ipc_command(&sb_state, "{\"id\":100,\"method\":\"settings:open:display\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("sandbox_mode_restricted") != std::string::npos);
    ipc_rep.clear();
    EXPECT(!dispatch_ipc_command(&sb_state, "{\"id\":101,\"method\":\"power:shutdown\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("sandbox_mode_restricted") != std::string::npos);
    ipc_rep.clear();
    EXPECT(!dispatch_ipc_command(&sb_state, "{\"id\":103,\"method\":\"ota:check\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("sandbox_mode_restricted") != std::string::npos);
    ipc_rep.clear();
    EXPECT(!dispatch_ipc_command(&sb_state, "{\"id\":104,\"method\":\"store:spinel-ota-full\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("sandbox_mode_restricted") != std::string::npos);
    ipc_rep.clear();
    EXPECT(!dispatch_ipc_command(&sb_state, "{\"id\":105,\"method\":\"ota:pick-offline\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("sandbox_mode_restricted") != std::string::npos);
    ipc_rep.clear();
    EXPECT(!dispatch_ipc_command(&sb_state, "{\"id\":106,\"method\":\"store:spinel-ota-offline\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("sandbox_mode_restricted") != std::string::npos);
    ipc_rep.clear();
    EXPECT(dispatch_ipc_command(&sb_state, "{\"id\":102,\"method\":\"launcher:toggle\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("\"ok\":true") != std::string::npos);

    // Regression guard: querying sandbox status must never kill sandbox shell.pid
    {
        std::error_code ec;
        std::string sh_pid_path = sandbox_runtime_dir() + "/shell.pid";
        fs::create_directories(sandbox_runtime_dir(), ec);
        pid_t child = ::fork();
        if (child == 0) {
            for (;;) ::pause();
            _exit(0);
        }
        EXPECT(child > 1);
        std::ofstream pf(sh_pid_path);
        pf << child << "\n";
        pf.close();
        (void)query_sandbox_status();
        int wst = 0;
        EXPECT(::waitpid(child, &wst, WNOHANG) == 0);
        EXPECT(::kill(child, 0) == 0);
        ::kill(child, SIGKILL);
        ::waitpid(child, &wst, 0);
        fs::remove(sh_pid_path, ec);
    }
}

void test_relay_crypto_and_stream() {
    // 1. Ed25519 sign/verify roundtrip & tamper rejection
    std::string pub_hex, err;
    EXPECT(ensure_device_ed25519_key(pub_hex, err));
    EXPECT(pub_hex.size() == 64);
    std::string sig;
    EXPECT(sign_ed25519_hex("sess123|pubabc", sig));
    EXPECT(verify_ed25519_hex(pub_hex, "sess123|pubabc", sig));
    EXPECT(!verify_ed25519_hex(pub_hex, "sess123|tampered", sig));

    // 2. X25519 ECDH agreement & ChaCha20-Poly1305 AEAD
    std::vector<uint8_t> a_priv, b_priv, a_key, b_key;
    std::string a_pub, b_pub;
    EXPECT(generate_x25519_keypair(a_priv, a_pub));
    EXPECT(generate_x25519_keypair(b_priv, b_pub));
    EXPECT(derive_x25519_session_key(a_priv, b_pub, "sid001", a_key));
    EXPECT(derive_x25519_session_key(b_priv, a_pub, "sid001", b_key));
    EXPECT(a_key == b_key && a_key.size() == 32);

    std::vector<uint8_t> plain = {1, 2, 3, 4, 5, 6, 7}, ct, dec;
    EXPECT(aead_encrypt(a_key, 42, 1, plain, ct));
    EXPECT(aead_decrypt(b_key, 42, 1, ct, dec));
    EXPECT(dec == plain);
    ct[0] ^= 0xFF;
    EXPECT(!aead_decrypt(b_key, 42, 1, ct, dec));

    // 3. Tile-Delta zlib video codec roundtrip
    const int w = 128, h = 128;
    std::vector<uint8_t> fb(w * h * 4, 0x20);
    sandbox_stream::TileEncoderState enc;
    sandbox_stream::TileDecoderState dec_st;
    std::vector<uint8_t> pkt;
    EXPECT(sandbox_stream::encode_tile_frame(enc, fb.data(), w, h, w * 4, false, 0, pkt));
    EXPECT(sandbox_stream::decode_tile_frame(dec_st, pkt.data(), pkt.size()));
    EXPECT(dec_st.width == w && dec_st.height == h && dec_st.bgra == fb);

    // Unchanged frame produces 0 bytes
    EXPECT(!sandbox_stream::encode_tile_frame(enc, fb.data(), w, h, w * 4, false, 0, pkt));

    // Modify single pixel -> delta decodes identically
    fb[(70 * w + 70) * 4 + 1] = 0xEE;
    EXPECT(sandbox_stream::encode_tile_frame(enc, fb.data(), w, h, w * 4, false, 0, pkt));
    EXPECT(sandbox_stream::decode_tile_frame(dec_st, pkt.data(), pkt.size()));
    EXPECT(dec_st.bgra == fb);

    // 4. Relay Config, unique device naming & IPC routing
    RelayConfig rc = load_relay_config();
    EXPECT(!rc.device_name.empty());
    std::string fallback_nm = default_device_name_for_id("a1b2c3d4e5f60000");
    EXPECT(!fallback_nm.empty() && fallback_nm != "spinelos");

    rc.server_addr = "127.0.0.1:7443";
    rc.allow_remote = true;
    EXPECT(save_relay_config(rc));
    EXPECT(load_relay_config().server_addr == "127.0.0.1:7443");

    rc.account.clear();
    rc.token.clear();
    save_relay_config(rc);
    std::string rename_err;
    EXPECT(relay_set_device_name("客厅测试机", rename_err));
    RelayConfig renamed = load_relay_config();
    EXPECT(renamed.device_name == "客厅测试机" && renamed.device_name_custom);

    AppState dummy;
    std::string ipc_rep;
    EXPECT(dispatch_ipc_command(&dummy, "{\"id\":101,\"method\":\"relay:state-get\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("\"ok\":true") != std::string::npos);
}

void test_ai_os_stage1_atomics() {
    using namespace spinel::ai;
    reset_agent_tracker_for_tests();
    reset_os_atomics_for_tests();

    fs::path ai_tmp = fs::path(home_dir()) / "ai-test-run";
    std::error_code ec;
    fs::create_directories(ai_tmp, ec);
    setenv("SPINEL_AGENT_RUNTIME_DIR", (ai_tmp / "agents").c_str(), 1);
    setenv("SPINEL_TASK_INBOX_FILE", (ai_tmp / "tasks.json").c_str(), 1);
    setenv("SPINEL_AGENT_PERMS_FILE", (ai_tmp / "agent_perms.conf").c_str(), 1);
    setenv("SPINEL_AGENT_BUS_SOCK", (ai_tmp / "agent.sock").c_str(), 1);

    // 1. Permission gate & audit log
    std::string reason;
    EXPECT(check_agent_capability("agent.report", reason) == PermDecision::Allow);
    EXPECT(check_agent_capability("context.clipboard", reason) == PermDecision::Deny);
    EXPECT(check_agent_capability("action.snapshot", reason) == PermDecision::Allow);
    EXPECT(check_agent_capability("action.workspace_focus", reason) == PermDecision::RequireApproval);
    EXPECT(check_agent_capability("unknown.evil", reason) == PermDecision::Deny);

    AgentPermConfig updated_perms;
    EXPECT(update_agent_perms_kv("tier1_clipboard=true\ntier3_policy=deny\n", updated_perms));
    EXPECT(updated_perms.tier1_clipboard == true);
    EXPECT(updated_perms.tier3_policy == Tier3Policy::Deny);
    EXPECT(check_agent_capability("context.clipboard", reason) == PermDecision::Allow);
    EXPECT(check_agent_capability("action.workspace_focus", reason) == PermDecision::Deny);
    EXPECT(update_agent_perms_kv("tier3_policy=ask\n", updated_perms));

    // 2. Agent command line matching & drop-in lifecycle + window binding
    std::string matched;
    EXPECT(match_known_agent_cmdline("/usr/bin/codex --repo /tmp/demo", matched) && matched == "Codex");
    EXPECT(match_known_agent_cmdline("node /opt/antigravity/cli.js", matched) && matched == "Antigravity");
    EXPECT(match_known_agent_cmdline("python3 /opt/DeepSeekHarness/run.py", matched) && matched == "DeepSeekHarness");
    EXPECT(!match_known_agent_cmdline("spinel-agent-ctl report --agent codex", matched));
    EXPECT(!match_known_agent_cmdline("grep codex /tmp/foo", matched));

    // 3. Task Inbox state machine + bound session auto-transition
    AgentTaskItem created_task;
    EXPECT(create_task("修复登录超时重试", "/tmp/SpinelOS", "DeepSeekHarness", "high", "加上指数退避", created_task));
    EXPECT(created_task.status == TaskStatus::Backlog);

    AgentTaskItem claimed;
    EXPECT(claim_task(created_task.id, "ds-sess-1", "DeepSeekHarness", claimed));
    EXPECT(claimed.status == TaskStatus::InProgress);
    EXPECT(claimed.assigned_session_id == "ds-sess-1");

    // 4. Bus dispatch: report Running -> Done automatically promotes bound Task to Review
    std::string r1 = dispatch_agent_bus_json(
        R"({"id":1,"client":"deepseek","method":"agent.report","params":{"id":"ds-sess-1","agent":"DeepSeekHarness","repo":"/tmp/SpinelOS","status":"running","task_id":")" +
        created_task.id + R"("}})");
    EXPECT(r1.find("\"ok\":true") != std::string::npos);

    std::string r2 = dispatch_agent_bus_json(
        R"({"id":2,"client":"deepseek","method":"agent.report","params":{"id":"ds-sess-1","agent":"DeepSeekHarness","status":"done","summary":"单测全部通过","task_id":")" +
        created_task.id + R"("}})");
    EXPECT(r2.find("\"ok\":true") != std::string::npos);

    auto review_list = list_tasks("review");
    EXPECT(review_list.size() == 1 && review_list[0].id == created_task.id);
    EXPECT(review_list[0].result_summary == "单测全部通过");

    AgentTaskItem accepted;
    EXPECT(accept_task(created_task.id, accepted));
    EXPECT(accepted.status == TaskStatus::Done);
    AgentTaskItem reopened;
    EXPECT(reopen_task(created_task.id, reopened));
    EXPECT(reopened.status == TaskStatus::Backlog && reopened.assigned_session_id.empty());
    EXPECT(delete_task(created_task.id));

    // Stale finished session eviction + window binding
    auto list_now = reconcile_agent_sessions(
        static_cast<long long>(std::time(nullptr)), 300, false,
        {{88, "spinel-terminal", "SpinelOS - DeepSeekHarness", true}});
    EXPECT(list_now.size() == 1 && list_now[0].win_id == 88);
    auto list_expired = reconcile_agent_sessions(static_cast<long long>(std::time(nullptr)) + 600, 300, false, {});
    EXPECT(list_expired.empty());

    // 5. Mocked desktop context + AF_UNIX agent_bus socket + interactive approval gate
    DesktopContextHooks mock_ctx;
    mock_ctx.list_windows = []() -> std::vector<WindowBindingRef> {
        return {{77, "org.spinelos.terminal", "Spinel 终端", true}};
    };
    mock_ctx.list_workspaces = []() -> std::vector<WorkspaceBindingRef> {
        return {{0, "工作区 1", true, false}, {1, "工作区 2", false, false}};
    };
    static int focused_ws = 0;
    static uint64_t focused_wid = 0;
    mock_ctx.activate_workspace = [](int ws1) { focused_ws = ws1; return true; };
    mock_ctx.activate_window = [](uint64_t wid) { focused_wid = wid; return true; };
    set_desktop_context_hooks(std::move(mock_ctx));

    std::string bound_sock;
    EXPECT(start_agent_bus_server(&bound_sock));
    EXPECT(is_agent_bus_running());

    std::string ping_resp;
    EXPECT(call_agent_bus_socket(bound_sock, R"({"id":"p1","method":"ping","params":{}})", ping_resp, 2000));
    EXPECT(ping_resp.find("\"pong\":true") != std::string::npos);

    std::string ctx_resp;
    EXPECT(call_agent_bus_socket(bound_sock, R"({"id":"c1","method":"context.desktop","params":{}})", ctx_resp, 2000));
    EXPECT(ctx_resp.find("\"active_workspace\":1") != std::string::npos);
    EXPECT(ctx_resp.find("Spinel 终端") != std::string::npos);

    // Trigger concurrent Tier 3 interactive approval over socket and resolve from a second connection
    std::string focus_resp;
    std::thread requester([&]() {
        call_agent_bus_socket(
            bound_sock,
            R"({"id":"f1","client":"codex","interactive":true,"method":"action.workspace_focus","params":{"workspace":2,"win_id":77,"summary":"切换验证","timeout_ms":5000}})",
            focus_resp, 6000);
    });
    std::string pending_id;
    for (int i = 0; i < 40; ++i) {
        auto pend = list_pending_approvals();
        if (!pend.empty()) {
            pending_id = pend[0].id;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT(!pending_id.empty());
    std::string resolve_resp;
    EXPECT(call_agent_bus_socket(
        bound_sock,
        "{\"id\":\"r1\",\"client\":\"ui\",\"method\":\"approval.resolve\",\"params\":{\"id\":\"" +
            pending_id + "\",\"decision\":\"allow\"}}",
        resolve_resp, 2000));
    EXPECT(resolve_resp.find("\"resolved\":true") != std::string::npos);
    requester.join();
    EXPECT(focus_resp.find("\"focused\":true") != std::string::npos);
    EXPECT(focused_ws == 2 && focused_wid == 77);

    auto audits = tail_agent_audit(20);
    EXPECT(!audits.empty());

    stop_agent_bus_server();
    EXPECT(!is_agent_bus_running());

    unsetenv("SPINEL_AGENT_RUNTIME_DIR");
    unsetenv("SPINEL_TASK_INBOX_FILE");
    unsetenv("SPINEL_AGENT_PERMS_FILE");
    unsetenv("SPINEL_AGENT_BUS_SOCK");
    reset_agent_tracker_for_tests();
    reset_os_atomics_for_tests();
}

void test_ai_os_stage2_mcp() {
    using namespace spinel::ai;
    reset_agent_tracker_for_tests();
    reset_os_atomics_for_tests();

    fs::path base = fs::path(home_dir()) / ".spinel_ai_stage2_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    setenv("SPINEL_AGENT_RUNTIME_DIR", (base / "rt/agents").c_str(), 1);
    setenv("SPINEL_TASK_INBOX_FILE", (base / "tasks.json").c_str(), 1);
    setenv("SPINEL_AGENT_PERMS_FILE", (base / "agent_perms.conf").c_str(), 1);
    setenv("SPINEL_AGENT_BUS_SOCK", (base / "rt/agent.sock").c_str(), 1);

    AgentPermConfig perms;
    EXPECT(update_agent_perms_kv("tier1_clipboard=false\ntier3_policy=deny\n", perms));

    // Mock desktop compositor context + start live AF_UNIX bus
    DesktopContextHooks mock_ctx;
    mock_ctx.list_windows = []() -> std::vector<WindowBindingRef> {
        return {{99, "codex", "Codex CLI - SpinelOS", true}};
    };
    mock_ctx.list_workspaces = []() -> std::vector<WorkspaceBindingRef> {
        return {{0, "工作区 1", true, false}};
    };
    set_desktop_context_hooks(std::move(mock_ctx));

    std::string bound_sock;
    EXPECT(start_agent_bus_server(&bound_sock));

    // 1. Initialize + notification + ping + tools/list
    std::string init_r = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}})");
    EXPECT(init_r.find("\"spinel-os-mcp\"") != std::string::npos);
    EXPECT(init_r.find("\"protocolVersion\":\"2024-11-05\"") != std::string::npos);

    std::string notif_r = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})");
    EXPECT(notif_r.empty());

    std::string ping_r = dispatch_mcp_jsonrpc(R"({"jsonrpc":"2.0","id":"p","method":"ping"})");
    EXPECT(ping_r.find("\"result\":{}") != std::string::npos);

    std::string list_r = dispatch_mcp_jsonrpc(R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})");
    for (const char *tool : {
             "agent_report", "agent_list", "todo_list", "todo_create", "todo_claim", "todo_complete",
             "desktop_get_context", "clipboard_recent", "screenshot_capture", "doctor_diagnose",
             "logs_tail", "snapshot_checkpoint", "sandbox_launch", "sandbox_status",
             "workspace_focus", "service_control", "proxy_status", "request_user_approval"}) {
        EXPECT(list_r.find(std::string("\"") + tool + "\"") != std::string::npos);
    }

    // 2. End-to-end live MCP -> AF_UNIX bus -> Task Inbox + Agent Tracker flow
    std::string create_r = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"todo_create","arguments":{"title":"通过 MCP 联调 Stage 2","repo":"/tmp/SpinelOS","preferred_agent":"Codex"}}})");
    EXPECT(create_r.find("\"isError\":false") != std::string::npos);
    EXPECT(create_r.find("通过 MCP 联调 Stage 2") != std::string::npos);

    auto backlog = list_tasks("backlog");
    EXPECT(backlog.size() == 1);
    std::string tid = backlog[0].id;

    std::string claim_r = dispatch_mcp_jsonrpc(
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"todo_claim\",\"arguments\":{\"id\":\"" +
        tid + "\",\"session_id\":\"codex-mcp-1\",\"agent\":\"Codex\"}}}");
    EXPECT(claim_r.find("\"isError\":false") != std::string::npos);

    std::string rep_r = dispatch_mcp_jsonrpc(
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"agent_report\",\"arguments\":{\"id\":\"codex-mcp-1\",\"agent\":\"Codex\",\"status\":\"done\",\"summary\":\"MCP 链路验收成功\",\"task_id\":\"" +
        tid + "\"}}}");
    EXPECT(rep_r.find("\"isError\":false") != std::string::npos);

    std::string todo_rev = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"todo_list","arguments":{"status":"review"}}})");
    EXPECT(todo_rev.find("\"isError\":false") != std::string::npos);
    EXPECT(todo_rev.find("MCP 链路验收成功") != std::string::npos);

    // 3. Context / Sandbox / Proxy / Permission gate enforcement via MCP
    std::string ctx_r = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"desktop_get_context","arguments":{}}})");
    EXPECT(ctx_r.find("\"isError\":false") != std::string::npos);
    EXPECT(ctx_r.find("Codex CLI - SpinelOS") != std::string::npos);

    std::string sb_r = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"sandbox_status","arguments":{}}})");
    EXPECT(sb_r.find("\"isError\":false") != std::string::npos);

    std::string clip_denied = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"clipboard_recent","arguments":{"limit":3}}})");
    EXPECT(clip_denied.find("\"isError\":true") != std::string::npos);

    std::string svc_denied = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"service_control","arguments":{"service":"ollama","action":"stop"}}})");
    EXPECT(svc_denied.find("\"isError\":true") != std::string::npos);

    // 4. Multimodal screenshot PNG base64 image return via mocked capture response
    fs::path fake_png = base / "cap-test.png";
    const unsigned char tiny_png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 1};
    {
        std::ofstream pfs(fake_png, std::ios::binary);
        pfs.write(reinterpret_cast<const char *>(tiny_png), sizeof(tiny_png));
    }
    std::string shot_r = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"screenshot_capture","arguments":{"mode":"full"}}})",
        [&](const std::string &, std::string &resp_out, int) {
            resp_out = "{\"id\":\"mcp\",\"ok\":true,\"decision\":\"allow\",\"result\":{\"path\":\"" +
                       json_escape(fake_png.string()) + "\"}}";
            return true;
        });
    EXPECT(shot_r.find("\"type\":\"image\"") != std::string::npos);
    EXPECT(shot_r.find("\"mimeType\":\"image/png\"") != std::string::npos);
    EXPECT(shot_r.find("iVBORw0K") != std::string::npos);

    // 5. Stdio loop framing: both NDJSON and Content-Length framing
    std::string frame_msg = R"({"jsonrpc":"2.0","id":99,"method":"ping"})";
    std::ostringstream in_stream;
    in_stream << frame_msg << "\n";
    in_stream << "Content-Length: " << frame_msg.size() << "\r\n\r\n" << frame_msg;
    std::istringstream iss(in_stream.str());
    std::ostringstream oss;
    EXPECT(run_mcp_stdio_loop(iss, oss) == 0);
    std::string stdio_out = oss.str();
    EXPECT(stdio_out.find("\"id\":99,\"result\":{}}\nContent-Length: ") != std::string::npos);
    EXPECT(stdio_out.rfind("\"id\":99,\"result\":{}}") > stdio_out.find("Content-Length: "));

    stop_agent_bus_server();
    unsetenv("SPINEL_AGENT_RUNTIME_DIR");
    unsetenv("SPINEL_TASK_INBOX_FILE");
    unsetenv("SPINEL_AGENT_PERMS_FILE");
    unsetenv("SPINEL_AGENT_BUS_SOCK");
    reset_agent_tracker_for_tests();
    reset_os_atomics_for_tests();
}

void test_ai_os_stage3_integration() {
    using namespace spinel::ai;
    reset_agent_tracker_for_tests();
    reset_os_atomics_for_tests();

    fs::path base = fs::path(home_dir()) / ".spinel_ai_stage3_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    setenv("SPINEL_AGENT_RUNTIME_DIR", (base / "rt/agents").c_str(), 1);
    setenv("SPINEL_TASK_INBOX_FILE", (base / "tasks.json").c_str(), 1);
    setenv("SPINEL_AGENT_PERMS_FILE", (base / "agent_perms.conf").c_str(), 1);
    setenv("SPINEL_AGENT_BUS_SOCK", (base / "rt/agent.sock").c_str(), 1);

    // 1. Dynamic Island pill summary formatting across states
    std::string cls;
    EXPECT(format_agent_island_pill_summary({}, {}, "", cls) == "◆ AI" && cls == "idle");
    AgentSessionInfo s1;
    s1.id = "s1";
    s1.agent = "Codex";
    s1.status = AgentSessionStatus::Running;
    EXPECT(format_agent_island_pill_summary({s1}, {}, "", cls) == "● Codex" && cls == "running");

    AgentSessionInfo s2 = s1;
    s2.id = "s2";
    s2.agent = "DeepSeekHarness";
    EXPECT(format_agent_island_pill_summary({s1, s2}, {}, "", cls) == "● 2 运行中" && cls == "running");

    ApprovalRequestInfo ap1;
    ap1.id = "apr_1";
    EXPECT(format_agent_island_pill_summary({s1}, {ap1}, "", cls).find("1 待确认") != std::string::npos && cls == "alert");
    EXPECT(format_agent_island_pill_summary({s1}, {ap1}, "✓ Codex · 完成", cls) == "✓ Codex · 完成" && cls == "toast");

    // 2. GameMode <-> vLLM VRAM yield arbitration & persistence
    std::string vram_act;
    EXPECT(evaluate_vram_yield_for_gamemode(true, true, true, vram_act) && vram_act == "stop_vllm");
    EXPECT(!evaluate_vram_yield_for_gamemode(true, false, true, vram_act) && vram_act == "none");
    EXPECT(!evaluate_vram_yield_for_gamemode(false, true, true, vram_act) && vram_act == "none");

    // 3. Work context bundle & consolidated live snapshot JSON
    AgentTaskItem task;
    EXPECT(create_task("Stage 3 联调", "/tmp/SpinelOS", "Antigravity", "high", "检查热路径", task));
    std::string bundle = build_work_context_bundle(task.id);
    EXPECT(bundle.find("Stage 3 联调") != std::string::npos);
    EXPECT(bundle.find("\"windows\":") != std::string::npos);

    std::string snap = build_ai_os_live_snapshot_json();
    EXPECT(snap.find("\"vram_yield_on_gamemode\":true") != std::string::npos);
    EXPECT(snap.find("Stage 3 联调") != std::string::npos);

    // 4. WebUI IPC router routes for AI Settings (ai:perms-set, ai:state-get, ai:copy-snippet:)
    AppState dummy_state;
    IpcHostActions acts;
    std::string reply;
    EXPECT(dispatch_ipc_command(
        &dummy_state,
        R"({"id":701,"method":"ai:perms-set","params":{"tier1_clipboard":"true","vram_yield_on_gamemode":"false"}})",
        acts, &reply));
    EXPECT(reply.find("\"ok\":true") != std::string::npos);
    AgentPermConfig loaded = load_agent_perms();
    EXPECT(loaded.tier1_clipboard == true);
    EXPECT(loaded.vram_yield_on_gamemode == false);

    reply.clear();
    EXPECT(dispatch_ipc_command(&dummy_state, R"({"id":702,"method":"ai:state-get","params":{}})", acts, &reply));
    EXPECT(reply.find("\"ok\":true") != std::string::npos);

    // 5. Spinel Doctor includes AI OS Bus & MCP Gateway health item
    auto doc_checks = run_system_health_checks();
    bool has_ai_doc = std::any_of(doc_checks.begin(), doc_checks.end(), [](const DoctorCheckItem &c) {
        return c.id == "ai_os_mcp" && !c.name.empty();
    });
    EXPECT(has_ai_doc);

    unsetenv("SPINEL_AGENT_RUNTIME_DIR");
    unsetenv("SPINEL_TASK_INBOX_FILE");
    unsetenv("SPINEL_AGENT_PERMS_FILE");
    unsetenv("SPINEL_AGENT_BUS_SOCK");
    reset_agent_tracker_for_tests();
    reset_os_atomics_for_tests();
}

}  // namespace

int main() {
    TempHomeGuard guard;
    test_common_utils();
    test_wine_launch();
    test_fcitx_notification_defaults();
    test_file_associations();
    test_trash_restore();
    test_theme_and_wallpaper();
    test_desktop_apps();
    test_wine_app_catalog();
    test_steam_library();
    test_system_control_configs();
    test_ipc_router();
    test_dock_quit_and_store_helpers();
    test_temp_memory_cleanup();
    test_parallel_sandbox();
    test_relay_crypto_and_stream();
    test_ai_os_stage1_atomics();
    test_ai_os_stage2_mcp();
    test_ai_os_stage3_integration();

    if (g_failures > 0) {
        std::cerr << g_failures << " assertion(s) failed.\n";
        return 1;
    }
    std::cout << "All SpinelOS unit tests passed.\n";
    return 0;
}
