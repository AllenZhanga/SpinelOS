#include "steam_library.hpp"
#include "common.hpp"
#include "native_shell.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct SteamGame {
    std::string appid;
    std::string name;
    long long size_bytes = 0;
    long long last_updated = 0;
    std::string cover_uri;
};

bool is_digits(const std::string &s) {
    if (s.empty()) return false;
    for (char ch : s) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

bool parse_vdf_kv_line(const std::string &line, std::string &key, std::string &val) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < line.size()) {
        if (line[i] == '"') {
            ++i;
            std::string tok;
            while (i < line.size() && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    tok += line[i + 1];
                    i += 2;
                } else {
                    tok += line[i];
                    ++i;
                }
            }
            tokens.push_back(std::move(tok));
            if (i < line.size() && line[i] == '"') ++i;
        } else {
            ++i;
        }
    }
    if (tokens.size() >= 2) {
        key = tokens[0];
        val = tokens[1];
        return true;
    }
    return false;
}

bool is_runtime_or_tool(const std::string &appid, const std::string &name) {
    if (appid == "228980") return true;  // Steamworks Common Redistributables
    if (name.rfind("Proton ", 0) == 0 || name == "Proton Experimental" || name == "Proton Hotfix") {
        return true;
    }
    if (name.rfind("Steam Linux Runtime", 0) == 0) return true;
    return false;
}

std::vector<fs::path> discover_steam_roots() {
    std::string h = home_dir();
    std::vector<fs::path> candidates = {
        fs::path(h) / ".local/share/Steam",
        fs::path(h) / ".steam/steam",
        fs::path(h) / ".var/app/com.valvesoftware.Steam/.local/share/Steam"
    };
    std::vector<fs::path> valid;
    std::set<std::string> seen;
    for (const auto &p : candidates) {
        std::error_code ec;
        if (fs::exists(p / "steamapps", ec)) {
            fs::path canon = fs::weakly_canonical(p, ec);
            std::string key = canon.empty() ? p.string() : canon.string();
            if (seen.insert(key).second) {
                valid.push_back(p);
            }
        }
    }
    return valid;
}

std::vector<fs::path> discover_library_folders(const std::vector<fs::path> &roots) {
    std::vector<fs::path> libs;
    std::set<std::string> seen;

    auto add_lib = [&](const fs::path &p) {
        std::error_code ec;
        if (!fs::exists(p / "steamapps", ec)) return;
        fs::path canon = fs::weakly_canonical(p, ec);
        std::string key = canon.empty() ? p.string() : canon.string();
        if (seen.insert(key).second) {
            libs.push_back(p);
        }
    };

    for (const auto &root : roots) {
        add_lib(root);
        fs::path vdf = root / "steamapps/libraryfolders.vdf";
        std::ifstream ifs(vdf);
        if (!ifs.is_open()) continue;
        std::string line;
        while (std::getline(ifs, line)) {
            std::string k, v;
            if (parse_vdf_kv_line(line, k, v) && k == "path" && !v.empty()) {
                add_lib(fs::path(v));
            }
        }
    }
    return libs;
}

std::string find_local_cover_path(const std::vector<fs::path> &roots, const std::string &appid) {
    for (const auto &root : roots) {
        fs::path cache = root / "appcache/librarycache";
        std::vector<fs::path> candidates = {
            cache / (appid + "_library_600x900.jpg"),
            cache / appid / "library_600x900.jpg",
            cache / (appid + "_library_capsule.jpg"),
            cache / appid / "library_capsule.jpg",
            cache / (appid + "_header.jpg"),
            cache / appid / "header.jpg"
        };
        std::error_code ec;
        for (const auto &c : candidates) {
            if (fs::exists(c, ec) && fs::is_regular_file(c, ec)) {
                return fs::absolute(c, ec).string();
            }
        }
        // Scan per-appid directory for hashed/variant capsule image if present
        fs::path app_dir = cache / appid;
        if (fs::is_directory(app_dir, ec)) {
            for (const auto &entry : fs::directory_iterator(app_dir, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                std::string ext = entry.path().extension().string();
                if (ext == ".jpg" || ext == ".png" || ext == ".webp") {
                    std::string fname = entry.path().filename().string();
                    if (fname.find("library") != std::string::npos || fname.find("header") != std::string::npos) {
                        return fs::absolute(entry.path(), ec).string();
                    }
                }
            }
        }
    }
    return "";
}

bool parse_app_manifest(const fs::path &manifest, SteamGame &game) {
    std::ifstream ifs(manifest);
    if (!ifs.is_open()) return false;
    std::string line;
    while (std::getline(ifs, line)) {
        std::string k, v;
        if (!parse_vdf_kv_line(line, k, v)) continue;
        if (k == "appid") game.appid = v;
        else if (k == "name") game.name = v;
        else if (k == "SizeOnDisk") game.size_bytes = std::atoll(v.c_str());
        else if (k == "LastUpdated") game.last_updated = std::atoll(v.c_str());
    }
    return is_digits(game.appid) && !game.name.empty() && !is_runtime_or_tool(game.appid, game.name);
}

}  // namespace

void ensure_steam_play_preconfig() {
    std::string pgrep_out;
    int pgrep_rc = 1;
    run_argv_capture({"pgrep", "-x", "steam"}, pgrep_out, pgrep_rc);
    if (pgrep_rc == 0) {
        return;  // Never touch config.vdf while Steam is running
    }

    std::error_code ec;
    fs::path cfg_dir = fs::path(home_dir()) / ".local/share/Steam/config";
    fs::path cfg_file = cfg_dir / "config.vdf";
    fs::create_directories(cfg_dir, ec);

    const std::string compat_block =
        "\t\t\t\t\"CompatToolMapping\"\n"
        "\t\t\t\t{\n"
        "\t\t\t\t\t\"0\"\n"
        "\t\t\t\t\t{\n"
        "\t\t\t\t\t\t\"name\"\t\t\"proton_experimental\"\n"
        "\t\t\t\t\t\t\"config\"\t\t\"\"\n"
        "\t\t\t\t\t\t\"Priority\"\t\t\"75\"\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t}\n";

    if (!fs::exists(cfg_file, ec)) {
        std::ofstream ofs(cfg_file);
        if (ofs.is_open()) {
            ofs << "\"InstallConfigStore\"\n"
                << "{\n"
                << "\t\"Software\"\n"
                << "\t{\n"
                << "\t\t\"Valve\"\n"
                << "\t\t{\n"
                << "\t\t\t\"Steam\"\n"
                << "\t\t\t{\n"
                << compat_block
                << "\t\t\t}\n"
                << "\t\t}\n"
                << "\t}\n"
                << "}\n";
        }
        return;
    }

    std::ifstream ifs(cfg_file);
    if (!ifs.is_open()) return;
    std::ostringstream buf;
    buf << ifs.rdbuf();
    std::string content = buf.str();
    ifs.close();

    if (content.find("\"CompatToolMapping\"") != std::string::npos) {
        return;
    }

    auto steam_pos = content.find("\"Steam\"");
    if (steam_pos == std::string::npos) return;
    auto brace_pos = content.find('{', steam_pos);
    if (brace_pos == std::string::npos) return;

    content.insert(brace_pos + 1, "\n" + compat_block);
    std::ofstream ofs(cfg_file, std::ios::trunc);
    if (ofs.is_open()) {
        ofs << content;
    }
}

std::vector<SteamGameInfo> list_steam_games() {
    auto roots = discover_steam_roots();
    auto libs = discover_library_folders(roots);

    std::vector<SteamGameInfo> games;
    std::set<std::string> seen_appids;

    for (const auto &lib : libs) {
        std::error_code ec;
        fs::path steamapps = lib / "steamapps";
        for (const auto &entry : fs::directory_iterator(steamapps, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            std::string fname = entry.path().filename().string();
            if (fname.rfind("appmanifest_", 0) != 0 || entry.path().extension() != ".acf") continue;

            SteamGame g;
            if (parse_app_manifest(entry.path(), g) && seen_appids.insert(g.appid).second) {
                std::string cover_path = find_local_cover_path(roots, g.appid);
                SteamGameInfo info;
                info.appid = std::move(g.appid);
                info.name = std::move(g.name);
                info.size_bytes = g.size_bytes;
                info.last_updated = g.last_updated;
                info.cover_path = cover_path;
                info.cover_uri = cover_path.empty() ? "" : ("file://" + cover_path);
                games.push_back(std::move(info));
            }
        }
    }

    std::sort(games.begin(), games.end(), [](const SteamGameInfo &a, const SteamGameInfo &b) {
        if (a.last_updated != b.last_updated) return a.last_updated > b.last_updated;
        return a.name < b.name;
    });
    return games;
}

void push_steam_library(AppState *state) {
    if (!state) return;

    auto games = list_steam_games();

    std::ostringstream js;
    js << "window.__spinelSteamGames && window.__spinelSteamGames([";
    for (size_t i = 0; i < games.size(); ++i) {
        if (i > 0) js << ",";
        js << "{"
           << "\"appid\":\"" << json_escape(games[i].appid) << "\","
           << "\"name\":\"" << json_escape(games[i].name) << "\","
           << "\"size_bytes\":" << games[i].size_bytes << ","
           << "\"cover_uri\":\"" << json_escape(games[i].cover_uri) << "\""
           << "}";
    }
    js << "]);";
    std::string script = js.str();
    eval_js(state->desktop_view, script);
    eval_js(state->games_view, script);
    native_shell_sync_desktop(state);
}

namespace {

struct AlertPrompt {
    std::string title;
    std::string detail;
};

void show_steam_alert(const std::string &title, const std::string &detail) {
    auto *msg = new AlertPrompt{title, detail};
    g_idle_add([](gpointer data) -> gboolean {
        std::unique_ptr<AlertPrompt> prompt(static_cast<AlertPrompt *>(data));
        GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", prompt->title.c_str());
        gtk_alert_dialog_set_detail(dlg, prompt->detail.c_str());
        const char *buttons[] = {"知道了", nullptr};
        gtk_alert_dialog_set_buttons(dlg, buttons);
        gtk_alert_dialog_choose(dlg, nullptr, nullptr, nullptr, nullptr);
        g_object_unref(dlg);
        return G_SOURCE_REMOVE;
    }, msg);
}

bool is_steam_bootstrapping(std::string &progress_line) {
    progress_line.clear();
    std::string pid_out;
    int pid_rc = 1;
    if (!run_argv_capture({"pgrep", "-f", "ubuntu12_32/steam"}, pid_out, pid_rc) || pid_rc != 0) {
        return false;
    }
    fs::path steamui = fs::path(home_dir()) / ".local/share/Steam/ubuntu12_32/steamui.so";
    std::error_code ec;
    if (fs::exists(steamui, ec)) {
        return false;  // Already bootstrapped, real Steam client is running
    }
    std::string raw = run_cmd_capture("tail -n 3 ~/.local/share/Steam/logs/bootstrap_log.txt 2>/dev/null | tr '\\r' '\\n' | grep -v '^$' | tail -n 1");
    progress_line = raw.empty() ? "正在初始化 Steam 运行环境与下载客户端更新..." : raw;
    return true;
}

}  // namespace

void launch_steam_client(AppState *state) {
    (void)state;
    if (!has_executable("steam")) {
        append_app_log("[Steam] 启动失败: 未检测到 /usr/bin/steam");
        show_steam_alert("未检测到 Steam 客户端",
                         "当前系统尚未安装 steam。请联网后在终端执行 sudo pacman -Sy steam 安装。");
        return;
    }
    std::string prog;
    if (is_steam_bootstrapping(prog)) {
        append_app_log("[Steam] 后台正在首次更新下载: " + prog);
        show_steam_alert("Steam 正在后台首次初始化与下载更新",
                         "当前进度: " + prog + "\n\n首次运行需要下载约 497MB 官方客户端，完成后会自动打开 Steam 窗口（无需重复点击）。可在「系统设置 → 系统日志」查看实时进度。");
        return;
    }
    std::error_code ec;
    if (!fs::exists(fs::path(home_dir()) / ".local/share/Steam/ubuntu12_32/steamui.so", ec)) {
        append_app_log("[Steam] 检测到首次运行环境，正在后台下载 Steam 官方运行时（约 497MB），请稍候...");
        show_steam_alert("正在首次初始化 Steam",
                         "首次启动 Steam 需要在后台联网下载约 497MB 客户端运行时（约 1~3 分钟），完成后会自动弹出登录界面。可在「系统设置 → 系统日志」查看实时下载进度。");
    }
    std::fprintf(stderr, "[spinel-shell] Launching Steam desktop client\n");
    spawn_argv_logged({"steam"}, "Steam");
}

void launch_steam_game(const std::string &appid, const PerfConfig &perf) {
    if (!is_digits(appid)) return;

    std::vector<std::pair<std::string, std::string>> envs;
    if (perf.mangohud != "off") {
        envs.emplace_back("MANGOHUD", "1");
    }
    if (perf.gamemode) {
        envs.emplace_back("GAMEMODERUN", "1");
    }

    spawn_argv_logged({"steam", "steam://rungameid/" + appid}, "SteamGame:" + appid, envs);
}

void launch_steam_gamepadui(AppState *state, const PerfConfig &perf) {
    (void)state;
    if (!has_executable("steam")) {
        append_app_log("[GamepadUI] 启动失败: 未检测到 /usr/bin/steam");
        show_steam_alert("无法启动主机大屏模式",
                         "当前系统未检测到 Steam 客户端（可在终端执行 sudo pacman -Sy steam 安装，或在应用商店安装 Heroic / Lutris）。");
        return;
    }
    std::string prog;
    if (is_steam_bootstrapping(prog)) {
        append_app_log("[GamepadUI] 后台正在首次更新 Steam: " + prog);
        show_steam_alert("Steam 正在后台首次初始化与下载更新",
                         "当前进度: " + prog + "\n\n请等待首次约 497MB 客户端更新完成后，再进入主机大屏（可在「系统设置 → 系统日志」查看进度）。");
        return;
    }

    std::vector<std::pair<std::string, std::string>> envs;
    if (perf.mangohud != "off") {
        envs.emplace_back("MANGOHUD", "1");
    }

    std::vector<std::string> direct_args = {"steam", "-gamepadui"};
    TelemetrySnapshot tel = collect_telemetry();
    if (tel.low_fx || !has_executable("gamescope")) {
        std::fprintf(stderr,
                     "[spinel-shell] Launching Steam GamepadUI directly (low_fx=%d, gamescope=%d)\n",
                     tel.low_fx ? 1 : 0, has_executable("gamescope") ? 1 : 0);
        spawn_argv_logged(direct_args, "SteamGamepadUI", envs);
        return;
    }

    std::vector<std::string> gs_args;
    if (perf.gamemode && has_executable("gamemoderun")) {
        gs_args.push_back("gamemoderun");
    }
    gs_args.push_back("gamescope");
    gs_args.push_back("-f");

    if (perf.render_res == "720p") {
        gs_args.push_back("-w"); gs_args.push_back("1280");
        gs_args.push_back("-h"); gs_args.push_back("720");
    } else if (perf.render_res == "900p") {
        gs_args.push_back("-w"); gs_args.push_back("1600");
        gs_args.push_back("-h"); gs_args.push_back("900");
    } else if (perf.render_res == "1080p") {
        gs_args.push_back("-w"); gs_args.push_back("1920");
        gs_args.push_back("-h"); gs_args.push_back("1080");
    }

    if (perf.fps_limit > 0) {
        gs_args.push_back("-r");
        gs_args.push_back(std::to_string(perf.fps_limit));
    }
    if (perf.fsr) {
        gs_args.push_back("-F");
        gs_args.push_back("fsr");
    }
    if (perf.mangohud != "off" && has_executable("mangoapp")) {
        gs_args.push_back("--mangoapp");
    }
    gs_args.push_back("--");
    gs_args.push_back("steam");
    gs_args.push_back("-gamepadui");

    append_app_log("[Gamescope] 启动主机大屏: gamescope + steam -gamepadui");
    std::fprintf(stderr, "[spinel-shell] Launching Gamescope + Steam GamepadUI\n");
    std::string log_path = (fs::path(spinel_state_dir()) / "apps.log").string();
    std::thread([gs_args, direct_args, envs, log_path]() {
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            for (const auto &kv : envs) {
                setenv(kv.first.c_str(), kv.second.c_str(), 1);
            }
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                if (devnull > 2) close(devnull);
            }
            int logfd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (logfd >= 0) {
                dup2(logfd, STDOUT_FILENO);
                dup2(logfd, STDERR_FILENO);
                if (logfd > 2) close(logfd);
            }
            std::vector<char *> argv;
            argv.reserve(gs_args.size() + 1);
            for (const auto &a : gs_args) argv.push_back(const_cast<char *>(a.c_str()));
            argv.push_back(nullptr);
            execvp(gs_args[0].c_str(), argv.data());
            _exit(127);
        }
        if (pid < 0) return;

        // If gamescope aborts immediately (e.g., Vulkan WSI unsupported), fall back to steam -gamepadui
        int status = 0;
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
                if (code != 0) {
                    append_app_log("[Gamescope] 提前退出 (code=" + std::to_string(code) + ")，自动回退到直连 steam -gamepadui");
                    std::fprintf(stderr,
                                 "[spinel-shell] gamescope exited early (code=%d), falling back to steam -gamepadui\n",
                                 code);
                    spawn_argv_logged(direct_args, "SteamGamepadUI", envs);
                }
                return;
            }
        }
        waitpid(pid, &status, 0);
    }).detach();
}
