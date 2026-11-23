#include "steam_library.hpp"
#include "common.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
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

std::string find_local_cover_uri(const std::vector<fs::path> &roots, const std::string &appid) {
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
                return "file://" + fs::absolute(c, ec).string();
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
                        return "file://" + fs::absolute(entry.path(), ec).string();
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

void push_steam_library(AppState *state) {
    if (!state || !state->hub_view) return;

    auto roots = discover_steam_roots();
    auto libs = discover_library_folders(roots);

    std::vector<SteamGame> games;
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
                g.cover_uri = find_local_cover_uri(roots, g.appid);
                games.push_back(std::move(g));
            }
        }
    }

    std::sort(games.begin(), games.end(), [](const SteamGame &a, const SteamGame &b) {
        if (a.last_updated != b.last_updated) return a.last_updated > b.last_updated;
        return a.name < b.name;
    });

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
    eval_js(state->hub_view, js.str());
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

    spawn_argv_detached({"steam", "steam://rungameid/" + appid}, envs);
}

void launch_steam_gamepadui(const PerfConfig &perf) {
    std::vector<std::pair<std::string, std::string>> envs;
    if (perf.mangohud != "off") {
        envs.emplace_back("MANGOHUD", "1");
    }

    std::vector<std::string> args;
    if (perf.gamemode) {
        args.push_back("gamemoderun");
    }
    args.push_back("gamescope");
    args.push_back("-f");

    if (perf.render_res == "720p") {
        args.push_back("-w"); args.push_back("1280");
        args.push_back("-h"); args.push_back("720");
    } else if (perf.render_res == "900p") {
        args.push_back("-w"); args.push_back("1600");
        args.push_back("-h"); args.push_back("900");
    } else if (perf.render_res == "1080p") {
        args.push_back("-w"); args.push_back("1920");
        args.push_back("-h"); args.push_back("1080");
    }

    if (perf.fps_limit > 0) {
        args.push_back("-r");
        args.push_back(std::to_string(perf.fps_limit));
    }
    if (perf.fsr) {
        args.push_back("-F");
        args.push_back("fsr");
    }
    if (perf.mangohud != "off") {
        args.push_back("--mangoapp");
    }
    args.push_back("--");
    args.push_back("steam");
    args.push_back("-gamepadui");

    spawn_argv_detached(args, envs);
}
