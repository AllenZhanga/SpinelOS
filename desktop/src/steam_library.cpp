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

std::vector<std::pair<std::string, std::string>> build_proxy_envs() {
    std::vector<std::pair<std::string, std::string>> envs;
    ProxyConfig p = load_proxy_config();
    if (!p.enabled || (p.http_proxy.empty() && p.socks_proxy.empty())) return envs;
    std::string http_val = !p.http_proxy.empty() ? p.http_proxy : p.socks_proxy;
    std::string all_val = !p.socks_proxy.empty() ? p.socks_proxy : p.http_proxy;
    envs.emplace_back("http_proxy", http_val);
    envs.emplace_back("https_proxy", http_val);
    envs.emplace_back("HTTP_PROXY", http_val);
    envs.emplace_back("HTTPS_PROXY", http_val);
    envs.emplace_back("all_proxy", all_val);
    envs.emplace_back("ALL_PROXY", all_val);
    envs.emplace_back("no_proxy", p.no_proxy);
    envs.emplace_back("NO_PROXY", p.no_proxy);
    return envs;
}

bool steam_running_without_proxy() {
    ProxyConfig p = load_proxy_config();
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
        append_app_log("[Steam] 启动失败: 未检测到 /usr/bin/steam");
        show_steam_alert(state, "未检测到 Steam 客户端",
                         "当前系统尚未安装 steam。请联网后在终端执行 sudo pacman -Sy steam 安装。");
        return;
    }
    if (steam_running_without_proxy()) {
        append_app_log("[Steam] 检测到旧 Steam 进程未加载新代理，正在重启 Steam 以应用系统代理...");
        run_cmd_capture("pkill -f 'ubuntu12_32/steam' 2>/dev/null; pkill -f 'steam.sh' 2>/dev/null; sleep 0.3");
    }
    std::string prog;
    if (is_steam_bootstrapping(prog)) {
        append_app_log("[Steam] 后台正在首次更新下载: " + prog);
        std::string proxy_hint = load_proxy_config().enabled
            ? ("\n当前已通过系统代理: " + load_proxy_config().http_proxy)
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

    std::vector<std::pair<std::string, std::string>> envs = build_proxy_envs();
    if (perf.mangohud != "off") {
        envs.emplace_back("MANGOHUD", "1");
    }
    if (perf.gamemode) {
        envs.emplace_back("GAMEMODERUN", "1");
    }

    spawn_argv_logged({"steam", "steam://rungameid/" + appid}, "SteamGame:" + appid, envs);
}

void launch_steam_gamepadui(AppState *state, const PerfConfig &perf) {
    if (!has_executable("steam")) {
        append_app_log("[GamepadUI] 启动失败: 未检测到 /usr/bin/steam");
        show_steam_alert(state, "无法启动主机大屏模式",
                         "当前系统未检测到 Steam 客户端（可在终端执行 sudo pacman -Sy steam 安装，或在应用商店安装 Heroic / Lutris）。");
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
                if (patch_single_appid_in_apps(txt, apps_open, apps_close, appid, escaped)) {
                    ++file_updates;
                }
            }
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
