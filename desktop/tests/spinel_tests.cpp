#include "test_harness.hpp"

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

namespace spinel_test {

int g_failures = 0;

namespace {

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

}  // namespace

}  // namespace spinel_test

int main(int argc, char **argv) {
    spinel_test::TempHomeGuard guard;
    std::string suite = "all";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--suite" && i + 1 < argc) {
            suite = argv[++i];
        }
    }

    if (suite == "all" || suite == "core") spinel_test::run_core_tests();
    if (suite == "all" || suite == "sys") spinel_test::run_sys_tests();
    if (suite == "all" || suite == "ai") spinel_test::run_sandbox_ai_tests();
    if (suite != "all" && suite != "core" && suite != "sys" && suite != "ai") {
        std::cerr << "Unknown test suite: " << suite << "\n";
        return 2;
    }

    if (spinel_test::g_failures > 0) {
        std::cerr << spinel_test::g_failures << " assertion(s) failed.\n";
        return 1;
    }
    std::cout << "SpinelOS unit tests (" << suite << ") passed.\n";
    return 0;
}
