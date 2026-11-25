#include "steam_library.hpp"
#include "common.hpp"
#include "native_shell.hpp"
#include "system_control.hpp"

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
    write_text_file(cfg_file.string(), content);
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
    auto roots = discover_steam_roots();
    auto libs = discover_library_folders(roots);
    auto overrides = load_game_overrides();

    auto has_compat = [&](const std::string &appid) -> bool {
        std::error_code ec;
        for (const auto &lib : libs) {
            if (fs::is_directory(lib / "steamapps/compatdata" / appid, ec)) return true;
        }
        return false;
    };

    std::ostringstream js;
    js << "window.__spinelSteamGames && window.__spinelSteamGames([";
    for (size_t i = 0; i < games.size(); ++i) {
        if (i > 0) js << ",";
        auto ov_it = overrides.find(games[i].appid);
        bool has_ov = (ov_it != overrides.end() && ov_it->second.enabled);
        js << "{"
           << "\"appid\":\"" << json_escape(games[i].appid) << "\","
           << "\"name\":\"" << json_escape(games[i].name) << "\","
           << "\"size_bytes\":" << games[i].size_bytes << ","
           << "\"cover_uri\":\"" << json_escape(games[i].cover_uri) << "\","
           << "\"compatdata\":" << (has_compat(games[i].appid) ? "true" : "false") << ","
           << "\"has_override\":" << (has_ov ? "true" : "false");
        if (has_ov) {
            const auto &ov = ov_it->second;
            js << ",\"override\":{"
               << "\"mangohud\":\"" << json_escape(ov.mangohud) << "\","
               << "\"fps_limit\":" << ov.fps_limit << ","
               << "\"fsr\":" << (ov.fsr ? "true" : "false") << ","
               << "\"gamemode\":" << (ov.gamemode ? "true" : "false") << ","
               << "\"render_res\":\"" << json_escape(ov.render_res) << "\""
               << "}";
        }
        js << "}";
    }
    js << "]);";
    std::string script = js.str();
    eval_js(state->desktop_view, script);
    eval_js(state->games_view, script);
    native_shell_sync_desktop(state);
}

namespace {

struct AlertPrompt {
    AppState *state;
    std::string title;
    std::string detail;
};

GtkWindow *pick_alert_parent(AppState *state) {
    if (!state) return nullptr;
    GtkWindow *cands[] = {state->games_win, state->settings_win, state->store_win, state->installer_win};
    for (GtkWindow *w : cands) {
        if (w && gtk_widget_get_visible(GTK_WIDGET(w))) return w;
    }
    return nullptr;
}

void show_steam_alert(AppState *state, const std::string &title, const std::string &detail) {
    auto *msg = new AlertPrompt{state, title, detail};
    g_idle_add([](gpointer data) -> gboolean {
        std::unique_ptr<AlertPrompt> prompt(static_cast<AlertPrompt *>(data));
        GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", prompt->title.c_str());
        gtk_alert_dialog_set_detail(dlg, prompt->detail.c_str());
        const char *buttons[] = {"知道了", nullptr};
        gtk_alert_dialog_set_buttons(dlg, buttons);
        gtk_alert_dialog_choose(dlg, pick_alert_parent(prompt->state), nullptr, nullptr, nullptr);
        g_object_unref(dlg);
        return G_SOURCE_REMOVE;
    }, msg);
}

void on_steam_install_prompt_reply(GObject *source, GAsyncResult *res, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, nullptr);
    if (choice == 1 && state && !state->shutting_down.load()) {
        handle_shell_command(state, "store:open:steam");
    }
}

void show_steam_install_prompt(AppState *state, const std::string &title, const std::string &detail) {
    auto *msg = new AlertPrompt{state, title, detail};
    g_idle_add([](gpointer data) -> gboolean {
        std::unique_ptr<AlertPrompt> prompt(static_cast<AlertPrompt *>(data));
        GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", prompt->title.c_str());
        gtk_alert_dialog_set_detail(dlg, prompt->detail.c_str());
        const char *buttons[] = {"取消", "去应用商店一键安装", nullptr};
        gtk_alert_dialog_set_buttons(dlg, buttons);
        gtk_alert_dialog_set_cancel_button(dlg, 0);
        gtk_alert_dialog_set_default_button(dlg, 1);
        gtk_alert_dialog_choose(dlg, pick_alert_parent(prompt->state), nullptr,
                                on_steam_install_prompt_reply, prompt->state);
        g_object_unref(dlg);
        return G_SOURCE_REMOVE;
    }, msg);
}

std::vector<std::pair<std::string, std::string>> build_proxy_envs() {
    return build_effective_proxy_envs();
}

bool steam_running_without_proxy() {
    ProxyConfig p = resolve_effective_proxy();
    if (!p.enabled || (p.http_proxy.empty() && p.socks_proxy.empty())) return false;
    std::string pid_out;
    int pid_rc = 1;
    if (!run_argv_capture({"pgrep", "-f", "ubuntu12_32/steam"}, pid_out, pid_rc) || pid_rc != 0) {
        return false;
    }
    std::istringstream iss(pid_out);
    std::string pid_s;
    while (iss >> pid_s) {
        std::ifstream ifs("/proc/" + pid_s + "/environ", std::ios::binary);
        if (!ifs.is_open()) continue;
        std::string env((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (env.find("http_proxy=") == std::string::npos && env.find("HTTP_PROXY=") == std::string::npos &&
            env.find("ALL_PROXY=") == std::string::npos) {
            return true;
        }
    }
    return false;
}

bool is_steam_bootstrapping(std::string &progress_line) {
    progress_line.clear();
    std::string upd_out;
    int upd_rc = 1;
    bool updating = run_argv_capture({"pgrep", "-f", "child-update-ui"}, upd_out, upd_rc) && upd_rc == 0;

    std::string pid_out;
    int pid_rc = 1;
    bool steam_proc = run_argv_capture({"pgrep", "-f", "ubuntu12_32/steam"}, pid_out, pid_rc) && pid_rc == 0;

    fs::path steamui = fs::path(home_dir()) / ".local/share/Steam/ubuntu12_32/steamui.so";
    std::error_code ec;
    if (!updating && (!steam_proc || fs::exists(steamui, ec))) {
        return false;
    }
    std::string raw = run_cmd_capture("tail -n 3 ~/.local/share/Steam/logs/bootstrap_log.txt 2>/dev/null | tr '\\r' '\\n' | grep -v '^$' | tail -n 1");
    progress_line = raw.empty() ? "正在初始化 Steam 运行环境与下载客户端更新..." : raw;
    return true;
}

}  // namespace

void launch_steam_client(AppState *state) {
    if (!has_executable("steam")) {
        append_app_log("[Steam] 尚未安装客户端，引导前往应用商店一键安装");
        show_steam_install_prompt(state, "尚未安装 Steam 客户端",
                                  "出于开源许可合规，系统出厂镜像未预捆绑 Steam 客户端。\n点击下方按钮即可打开应用商店一键在线安装 Steam。");
        return;
    }
    if (steam_running_without_proxy()) {
        append_app_log("[Steam] 检测到旧 Steam 进程未加载新代理，正在重启 Steam 以应用系统代理...");
        run_cmd_capture("pkill -f 'ubuntu12_32/steam' 2>/dev/null; pkill -f 'steam.sh' 2>/dev/null; sleep 0.3");
    }
    std::string prog;
    if (is_steam_bootstrapping(prog)) {
        append_app_log("[Steam] 后台正在首次更新下载: " + prog);
        ProxyConfig eff_p = resolve_effective_proxy();
        std::string proxy_hint = eff_p.enabled
            ? ("\n当前已通过系统代理: " + eff_p.http_proxy)
            : "\n提示：若下载缓慢，可在「系统设置 → Wi-Fi 与网络」启用系统代理后重启 Steam。";
        show_steam_alert(state, "Steam 正在后台下载客户端更新",
                         "当前进度: " + prog + proxy_hint + "\n\n完成后会自动弹出登录窗口，请勿重复启动多个 Steam 进程。");
        return;
    }
    std::error_code ec;
    if (!fs::exists(fs::path(home_dir()) / ".local/share/Steam/ubuntu12_32/steamui.so", ec)) {
        append_app_log("[Steam] 检测到首次运行环境，正在后台下载 Steam 官方运行时，请稍候...");
        show_steam_alert(state, "正在首次初始化 Steam",
                         "首次启动 Steam 需要在后台联网下载官方运行时与客户端更新，完成后会自动弹出登录界面。可在「系统设置 → 系统日志」查看实时进度。");
    }
    std::fprintf(stderr, "[spinel-shell] Launching Steam desktop client\n");
    spawn_argv_logged({"steam"}, "Steam", build_proxy_envs());
}

void launch_steam_game(const std::string &appid, const PerfConfig &perf) {
    if (!is_digits(appid)) return;

    PerfConfig eff = resolve_perf_for_appid(appid, perf);
    std::vector<std::pair<std::string, std::string>> envs = build_proxy_envs();
    if (eff.mangohud != "off") {
        envs.emplace_back("MANGOHUD", "1");
    }
    if (eff.gamemode) {
        envs.emplace_back("GAMEMODERUN", "1");
    }

    spawn_argv_logged({"steam", "steam://rungameid/" + appid}, "SteamGame:" + appid, envs);
}

void launch_steam_gamepadui(AppState *state, const PerfConfig &perf) {
    if (!has_executable("steam")) {
        append_app_log("[GamepadUI] 尚未安装 Steam，引导前往应用商店一键安装");
        show_steam_install_prompt(state, "需要安装 Steam 才能进入主机大屏",
                                  "主机大屏模式依赖官方 Steam 客户端。点击下方按钮即可一键前往应用商店在线安装。");
        return;
    }
    if (steam_running_without_proxy()) {
        append_app_log("[GamepadUI] 检测到旧 Steam 进程未加载新代理，正在重启以应用系统代理...");
        run_cmd_capture("pkill -f 'ubuntu12_32/steam' 2>/dev/null; pkill -f 'steam.sh' 2>/dev/null; sleep 0.3");
    }
    std::string prog;
    if (is_steam_bootstrapping(prog)) {
        append_app_log("[GamepadUI] 后台正在首次更新 Steam: " + prog);
        show_steam_alert(state, "Steam 正在后台下载更新",
                         "当前进度: " + prog + "\n\n请等待客户端更新完成后再进入主机大屏（不要同时开启多个更新进程）。");
        return;
    }

    std::vector<std::pair<std::string, std::string>> envs = build_proxy_envs();
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

namespace {

bool find_vdf_section_bounds(const std::string &txt,
                             const std::string &section_name,
                             size_t search_from,
                             size_t search_end,
                             size_t &open_brace,
                             size_t &close_brace) {
    std::string key = "\"" + section_name + "\"";
    size_t pos = search_from;
    while (pos < search_end) {
        size_t hit = txt.find(key, pos);
        if (hit == std::string::npos || hit >= search_end) return false;
        size_t cur = hit + key.size();
        while (cur < search_end && (txt[cur] == ' ' || txt[cur] == '\t' || txt[cur] == '\r' || txt[cur] == '\n')) {
            ++cur;
        }
        if (cur < search_end && txt[cur] == '{') {
            open_brace = cur;
            int depth = 1;
            bool in_str = false;
            for (size_t i = cur + 1; i < txt.size(); ++i) {
                char c = txt[i];
                if (c == '\\' && in_str) { ++i; continue; }
                if (c == '"') { in_str = !in_str; continue; }
                if (in_str) continue;
                if (c == '{') ++depth;
                else if (c == '}') {
                    --depth;
                    if (depth == 0) {
                        close_brace = i;
                        return true;
                    }
                }
            }
            return false;
        }
        pos = hit + key.size();
    }
    return false;
}

std::string vdf_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out;
}

bool patch_single_appid_in_apps(std::string &txt,
                                size_t apps_open,
                                size_t &apps_close,
                                const std::string &appid,
                                const std::string &escaped_opts) {
    size_t app_open = 0, app_close = 0;
    if (find_vdf_section_bounds(txt, appid, apps_open + 1, apps_close, app_open, app_close)) {
        size_t lpos = txt.find("\"LaunchOptions\"", app_open + 1);
        if (lpos != std::string::npos && lpos < app_close) {
            size_t line_start = txt.rfind('\n', lpos);
            line_start = (line_start == std::string::npos) ? app_open + 1 : line_start + 1;
            size_t line_end = txt.find('\n', lpos);
            if (line_end == std::string::npos || line_end > app_close) line_end = app_close;
            std::string repl = "\t\t\t\t\t\"LaunchOptions\"\t\t\"" + escaped_opts + "\"";
            std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(repl.size()) - static_cast<std::ptrdiff_t>(line_end - line_start);
            txt.replace(line_start, line_end - line_start, repl);
            apps_close = static_cast<size_t>(static_cast<std::ptrdiff_t>(apps_close) + delta);
            return true;
        }
        std::string ins = "\n\t\t\t\t\t\"LaunchOptions\"\t\t\"" + escaped_opts + "\"";
        txt.insert(app_open + 1, ins);
        apps_close += ins.size();
        return true;
    }
    std::string block = "\n\t\t\t\t\"" + appid + "\"\n\t\t\t\t{\n\t\t\t\t\t\"LaunchOptions\"\t\t\"" + escaped_opts + "\"\n\t\t\t\t}";
    txt.insert(apps_close, block);
    apps_close += block.size();
    return true;
}

}  // namespace

std::string build_steam_launch_options(const PerfConfig &perf) {
    std::vector<std::string> parts;
    if (perf.mangohud != "off") parts.push_back("MANGOHUD=1");
    if (perf.gamemode) parts.push_back("gamemoderun");

    bool need_gs = (perf.render_res != "native" || perf.fps_limit > 0 || perf.fsr);
    if (need_gs) {
        parts.push_back("gamescope");
        parts.push_back("-f");
        if (perf.render_res == "720p") { parts.push_back("-w"); parts.push_back("1280"); parts.push_back("-h"); parts.push_back("720"); }
        else if (perf.render_res == "900p") { parts.push_back("-w"); parts.push_back("1600"); parts.push_back("-h"); parts.push_back("900"); }
        else if (perf.render_res == "1080p") { parts.push_back("-w"); parts.push_back("1920"); parts.push_back("-h"); parts.push_back("1080"); }
        if (perf.fps_limit > 0) { parts.push_back("-r"); parts.push_back(std::to_string(perf.fps_limit)); }
        if (perf.fsr) { parts.push_back("-F"); parts.push_back("fsr"); }
        if (perf.mangohud != "off") parts.push_back("--mangoapp");
        parts.push_back("--");
    }
    if (parts.empty()) return "";
    parts.push_back("%command%");

    std::ostringstream oss;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) oss << ' ';
        oss << parts[i];
    }
    return oss.str();
}

SteamPerfApplyResult apply_perf_to_steam_configs(const PerfConfig &perf) {
    SteamPerfApplyResult res;
    res.launch_options = build_steam_launch_options(perf);

    std::string pid_out;
    int pid_rc = 1;
    res.steam_running = run_argv_capture({"pgrep", "-f", "ubuntu12_32/steam"}, pid_out, pid_rc) && pid_rc == 0;
    if (res.steam_running) {
        return res;
    }

    auto games = list_steam_games();
    if (games.empty()) return res;
    std::vector<std::string> appids;
    appids.reserve(games.size());
    for (const auto &g : games) appids.push_back(g.appid);

    std::string escaped = vdf_escape(res.launch_options);
    for (const auto &root : discover_steam_roots()) {
        fs::path ud = root / "userdata";
        std::error_code ec;
        if (!fs::is_directory(ud, ec)) continue;
        for (const auto &entry : fs::directory_iterator(ud, ec)) {
            if (!entry.is_directory(ec)) continue;
            fs::path vdf = entry.path() / "config/localconfig.vdf";
            if (!fs::exists(vdf, ec)) continue;

            std::ifstream ifs(vdf);
            if (!ifs.is_open()) continue;
            std::ostringstream buf;
            buf << ifs.rdbuf();
            ifs.close();
            std::string txt = buf.str();

            size_t apps_open = 0, apps_close = 0;
            if (!find_vdf_section_bounds(txt, "apps", 0, txt.size(), apps_open, apps_close)) continue;

            fs::copy_file(vdf, vdf.string() + ".bak", fs::copy_options::overwrite_existing, ec);
            int file_updates = 0;
            for (const auto &appid : appids) {
                std::string per_game_escaped = vdf_escape(build_steam_launch_options(resolve_perf_for_appid(appid, perf)));
                if (patch_single_appid_in_apps(txt, apps_open, apps_close, appid, per_game_escaped)) {
                    ++file_updates;
                }
            }
            (void)escaped;
            if (file_updates > 0) {
                fs::path tmp = vdf.string() + ".tmp";
                std::ofstream ofs(tmp, std::ios::trunc);
                if (ofs.is_open()) {
                    ofs << txt;
                    ofs.close();
                    fs::rename(tmp, vdf, ec);
                    ++res.configs_updated;
                    res.games_updated = std::max(res.games_updated, file_updates);
                }
            }
        }
    }
    return res;
}

namespace {

fs::path game_overrides_path() {
    return fs::path(home_dir()) / ".config/spinelos/game_overrides.conf";
}

bool write_game_overrides(const std::map<std::string, GamePerfOverride> &map) {
    std::ostringstream oss;
    for (const auto &kv : map) {
        const auto &ov = kv.second;
        if (!is_digits(ov.appid) || !ov.enabled) continue;
        oss << ov.appid << "|"
            << ov.mangohud << "|"
            << ov.fps_limit << "|"
            << (ov.fsr ? "1" : "0") << "|"
            << (ov.gamemode ? "1" : "0") << "|"
            << ov.render_res << "\n";
    }
    return write_text_file(game_overrides_path().string(), oss.str());
}

long long dir_size_bytes(const fs::path &dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    long long total = 0;
    for (const auto &entry : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (entry.is_regular_file(ec)) {
            total += static_cast<long long>(entry.file_size(ec));
        }
    }
    return total;
}

long long clear_dir_contents(const fs::path &dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    long long before = dir_size_bytes(dir);
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        fs::remove_all(entry.path(), ec);
    }
    return std::max(0LL, before - dir_size_bytes(dir));
}

std::string format_bytes_human(long long bytes) {
    char buf[48];
    if (bytes >= 1073741824LL) {
        std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / 1073741824.0);
    } else if (bytes >= 1048576LL) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / 1048576.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%lld KB", bytes / 1024LL);
    }
    return buf;
}

}  // namespace

std::map<std::string, GamePerfOverride> load_game_overrides() {
    std::map<std::string, GamePerfOverride> out;
    std::ifstream ifs(game_overrides_path());
    if (!ifs.is_open()) return out;
    std::string line;
    while (std::getline(ifs, line)) {
        line = trim_copy(line);
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string item;
        while (std::getline(ss, item, '|')) cols.push_back(trim_copy(item));
        if (cols.size() < 6 || !is_digits(cols[0])) continue;
        GamePerfOverride ov;
        ov.appid = cols[0];
        ov.enabled = true;
        ov.mangohud = (cols[1] == "off" || cols[1] == "compact" || cols[1] == "full") ? cols[1] : "compact";
        ov.fps_limit = std::clamp(std::atoi(cols[2].c_str()), 0, 240);
        ov.fsr = (cols[3] == "1" || cols[3] == "true");
        ov.gamemode = (cols[4] == "1" || cols[4] == "true");
        ov.render_res = (cols[5] == "720p" || cols[5] == "900p" || cols[5] == "1080p" || cols[5] == "native") ? cols[5] : "native";
        out[ov.appid] = ov;
    }
    return out;
}

bool save_game_override(const GamePerfOverride &ov) {
    if (!is_digits(ov.appid)) return false;
    auto map = load_game_overrides();
    if (ov.enabled) {
        map[ov.appid] = ov;
    } else {
        map.erase(ov.appid);
    }
    return write_game_overrides(map);
}

bool clear_game_override(const std::string &appid) {
    auto map = load_game_overrides();
    map.erase(appid);
    return write_game_overrides(map);
}

PerfConfig resolve_perf_for_appid(const std::string &appid, const PerfConfig &global_perf) {
    auto map = load_game_overrides();
    auto it = map.find(appid);
    if (it == map.end() || !it->second.enabled) return global_perf;
    PerfConfig eff = global_perf;
    eff.mangohud = it->second.mangohud;
    eff.fps_limit = it->second.fps_limit;
    eff.fsr = it->second.fsr;
    eff.gamemode = it->second.gamemode;
    eff.render_res = it->second.render_res;
    return eff;
}

void handle_game_override_command(AppState *state, const std::string &payload, bool is_clear) {
    if (is_clear) {
        std::string appid = trim_copy(payload);
        if (!is_digits(appid)) return;
        clear_game_override(appid);
        apply_perf_to_steam_configs(load_perf_config());
        push_steam_library(state);
        return;
    }

    GamePerfOverride ov;
    ov.enabled = true;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        if (k == "appid") ov.appid = v;
        else if (k == "mangohud" && (v == "off" || v == "compact" || v == "full")) ov.mangohud = v;
        else if (k == "fps_limit") ov.fps_limit = std::clamp(std::atoi(v.c_str()), 0, 240);
        else if (k == "fsr") ov.fsr = (v == "true" || v == "1");
        else if (k == "gamemode") ov.gamemode = (v == "true" || v == "1");
        else if (k == "render_res" && (v == "native" || v == "720p" || v == "900p" || v == "1080p")) ov.render_res = v;
    }
    if (!save_game_override(ov)) return;
    auto sr = apply_perf_to_steam_configs(load_perf_config());
    append_app_log("[Games] 已保存 AppID=" + ov.appid + " 独立性能配置（" + build_steam_launch_options(resolve_perf_for_appid(ov.appid, load_perf_config())) + "）");
    if (sr.steam_running && state) {
        show_steam_alert(state, "独立参数已保存",
                         "SpinelOS 已保存该游戏的独立配置；当前 Steam 正在运行，重启 Steam 后启动器将自动同步到 Steam 客户端配置。");
    }
    push_steam_library(state);
}

void open_steam_compatdata(AppState *state, const std::string &appid) {
    if (!is_digits(appid)) return;
    auto libs = discover_library_folders(discover_steam_roots());
    std::error_code ec;
    for (const auto &lib : libs) {
        fs::path compat = lib / "steamapps/compatdata" / appid;
        if (fs::is_directory(compat, ec)) {
            fs::path pfx = compat / "pfx/drive_c/users/steamuser";
            fs::path target = fs::is_directory(pfx, ec) ? pfx : compat;
            spawn_argv_detached({has_executable("spinel-files") ? "spinel-files" : "thunar", target.string()});
            return;
        }
    }
    show_steam_alert(state, "暂未生成 Proton 兼容目录",
                     "未找到 AppID " + appid + " 的 compatdata 目录（可能是 Linux 原生游戏，或尚未通过 Proton 启动过）。");
}

CacheStorageStats scan_storage_caches() {
    CacheStorageStats stats;
    auto libs = discover_library_folders(discover_steam_roots());
    for (const auto &lib : libs) {
        stats.shadercache_bytes += dir_size_bytes(lib / "steamapps/shadercache");
        stats.downloading_bytes += dir_size_bytes(lib / "steamapps/downloading");
    }
    std::error_code ec;
    fs::path state_dir = fs::path(spinel_state_dir());
    if (fs::is_directory(state_dir, ec)) {
        for (const auto &entry : fs::directory_iterator(state_dir, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".log") {
                stats.logs_bytes += static_cast<long long>(entry.file_size(ec));
            }
        }
    }
    stats.pacman_bytes = dir_size_bytes("/var/cache/pacman/pkg");
    return stats;
}

void push_storage_caches(AppState *state) {
    if (!state || !state->games_view) return;
    CacheStorageStats stats = scan_storage_caches();
    std::ostringstream js;
    js << "window.__spinelStorageCaches && window.__spinelStorageCaches({"
       << "\"shadercache_bytes\":" << stats.shadercache_bytes << ","
       << "\"downloading_bytes\":" << stats.downloading_bytes << ","
       << "\"logs_bytes\":" << stats.logs_bytes << ","
       << "\"pacman_bytes\":" << stats.pacman_bytes
       << "});";
    eval_js(state->games_view, js.str());
}

void clean_storage_caches(AppState *state, const std::string &payload) {
    long long freed = 0;
    bool clean_shader = (payload.find("shadercache") != std::string::npos);
    bool clean_dl = (payload.find("downloading") != std::string::npos);
    bool clean_logs = (payload.find("logs") != std::string::npos);

    auto libs = discover_library_folders(discover_steam_roots());
    for (const auto &lib : libs) {
        if (clean_shader) freed += clear_dir_contents(lib / "steamapps/shadercache");
        if (clean_dl) freed += clear_dir_contents(lib / "steamapps/downloading");
    }
    if (clean_logs) {
        std::error_code ec;
        fs::path state_dir = fs::path(spinel_state_dir());
        if (fs::is_directory(state_dir, ec)) {
            for (const auto &entry : fs::directory_iterator(state_dir, ec)) {
                if (entry.is_regular_file(ec) && entry.path().extension() == ".log") {
                    freed += static_cast<long long>(entry.file_size(ec));
                    write_text_file(entry.path().string(), "");
                }
            }
        }
    }

    if (!clean_logs) {
        append_app_log("[Storage] 已清理缓存，释放空间 " + format_bytes_human(freed));
    }
    push_storage_caches(state);
    if (state && state->games_view) {
        eval_js(state->games_view,
                "window.__spinelStorageFeedback && window.__spinelStorageFeedback(true, \"已安全清理缓存并释放 " +
                    json_escape(format_bytes_human(freed)) + "\");");
    }
}
