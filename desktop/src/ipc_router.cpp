#include "ipc_router.hpp"
#include "common.hpp"
#include "desktop_apps.hpp"
#include "native_shell.hpp"
#include "steam_library.hpp"
#include "system_control.hpp"
#include "theme_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

void skip_ws(const std::string &s, size_t &pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
}

bool parse_json_string(const std::string &s, size_t &pos, std::string &out) {
    skip_ws(s, pos);
    if (pos >= s.size() || s[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < s.size()) {
        char c = s[pos++];
        if (c == '"') return true;
        if (c == '\\') {
            if (pos >= s.size()) return false;
            char esc = s[pos++];
            switch (esc) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (pos + 4 > s.size()) return false;
                    unsigned int cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = s[pos + i];
                        if (!std::isxdigit(static_cast<unsigned char>(h))) return false;
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned int>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned int>(h - 'a' + 10);
                        else cp |= static_cast<unsigned int>(h - 'A' + 10);
                    }
                    pos += 4;
                    if (cp < 0x80) {
                        out += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out += esc; break;
            }
        } else {
            out += c;
        }
    }
    return false;
}

bool skip_json_value(const std::string &s, size_t &pos, std::string &raw_slice);

bool parse_json_scalar_or_raw(const std::string &s, size_t &pos, std::string &scalar_str, std::string &raw_slice) {
    skip_ws(s, pos);
    if (pos >= s.size()) return false;
    size_t start = pos;
    char c = s[pos];
    if (c == '"') {
        if (!parse_json_string(s, pos, scalar_str)) return false;
        raw_slice = s.substr(start, pos - start);
        return true;
    }
    if (c == '{' || c == '[') {
        if (!skip_json_value(s, pos, raw_slice)) return false;
        scalar_str = raw_slice;
        return true;
    }
    while (pos < s.size()) {
        char ch = s[pos];
        if (ch == ',' || ch == '}' || ch == ']' || std::isspace(static_cast<unsigned char>(ch))) {
            break;
        }
        ++pos;
    }
    if (pos == start) return false;
    raw_slice = s.substr(start, pos - start);
    scalar_str = (raw_slice == "null") ? "" : raw_slice;
    return true;
}

bool skip_json_value(const std::string &s, size_t &pos, std::string &raw_slice) {
    skip_ws(s, pos);
    if (pos >= s.size()) return false;
    size_t start = pos;
    char open = s[pos];
    if (open != '{' && open != '[') {
        std::string tmp;
        return parse_json_scalar_or_raw(s, pos, tmp, raw_slice);
    }
    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    bool in_str = false;
    while (pos < s.size()) {
        char ch = s[pos++];
        if (in_str) {
            if (ch == '\\' && pos < s.size()) ++pos;
            else if (ch == '"') in_str = false;
        } else {
            if (ch == '"') in_str = true;
            else if (ch == open) ++depth;
            else if (ch == close) {
                --depth;
                if (depth == 0) {
                    raw_slice = s.substr(start, pos - start);
                    return true;
                }
            }
        }
    }
    return false;
}

bool parse_json_object(const std::string &s, size_t &pos,
                       const std::function<void(const std::string &key, const std::string &val, const std::string &raw_val, size_t val_start)> &on_field) {
    skip_ws(s, pos);
    if (pos >= s.size() || s[pos] != '{') return false;
    ++pos;
    skip_ws(s, pos);
    if (pos < s.size() && s[pos] == '}') {
        ++pos;
        return true;
    }
    while (pos < s.size()) {
        std::string key;
        if (!parse_json_string(s, pos, key)) return false;
        skip_ws(s, pos);
        if (pos >= s.size() || s[pos] != ':') return false;
        ++pos;
        skip_ws(s, pos);
        size_t val_start = pos;
        std::string val, raw_val;
        if (!parse_json_scalar_or_raw(s, pos, val, raw_val)) return false;
        on_field(key, val, raw_val, val_start);
        skip_ws(s, pos);
        if (pos >= s.size()) return false;
        if (s[pos] == ',') {
            ++pos;
            continue;
        }
        if (s[pos] == '}') {
            ++pos;
            return true;
        }
        return false;
    }
    return false;
}

// Converts dotted JSON RPC method names ("perf.set") to canonical colon form ("perf:set")
// while preserving explicit ':' method names as-is.
std::string normalize_method_name(std::string m) {
    if (m.find(':') == std::string::npos) {
        std::replace(m.begin(), m.end(), '.', ':');
    }
    return m;
}

std::string build_kv_payload(const std::string &head, const std::map<std::string, std::string> &params) {
    std::ostringstream oss;
    oss << head;
    for (const auto &kv : params) {
        oss << '\n' << kv.first << '=' << kv.second;
    }
    return oss.str();
}

std::string arg_or_param(const IpcRequest &req, const std::string &prefix, const std::vector<std::string> &param_keys) {
    if (req.method.rfind(prefix, 0) == 0) {
        return req.method.substr(prefix.size());
    }
    for (const auto &k : param_keys) {
        auto it = req.params.find(k);
        if (it != req.params.end() && !it->second.empty()) {
            return it->second;
        }
    }
    return "";
}

void post_ipc_reply_to_views(AppState *state, const std::string &reply_json) {
    if (!state || reply_json.empty()) return;
    std::string js = "window.__spinelIpcReply && window.__spinelIpcReply(" + reply_json + ");";
    eval_js(state->settings_view, js);
    eval_js(state->games_view, js);
    eval_js(state->store_view, js);
    eval_js(state->installer_view, js);
    eval_js(state->welcome_view, js);
}

using HandlerFn = void (*)(AppState *state, const IpcRequest &req, const IpcHostActions &actions);

struct PrefixRoute {
    const char *prefix;
    HandlerFn fn;
};

}  // namespace

bool parse_ipc_request(const std::string &raw, IpcRequest &out) {
    out = IpcRequest{};
    std::string trimmed = trim_copy(raw);
    if (trimmed.empty()) return false;

    if (trimmed.front() == '{') {
        size_t pos = 0;
        std::string raw_method;
        bool parsed_ok = parse_json_object(trimmed, pos, [&](const std::string &key, const std::string &val, const std::string &raw_val, size_t) {
            if (key == "id") {
                out.has_id = true;
                out.id_json = raw_val;
            } else if (key == "method") {
                raw_method = val;
            } else if (key == "params" && !raw_val.empty() && raw_val.front() == '{') {
                size_t ppos = 0;
                parse_json_object(raw_val, ppos, [&](const std::string &pk, const std::string &pv, const std::string &, size_t) {
                    out.params[pk] = pv;
                });
            }
        });
        if (!parsed_ok || raw_method.empty()) {
            return false;
        }
        out.is_json = true;
        out.method = normalize_method_name(raw_method);
        out.legacy_raw = build_kv_payload(out.method, out.params);
        return true;
    }

    out.is_json = false;
    out.legacy_raw = raw;
    auto nl = raw.find('\n');
    out.method = (nl == std::string::npos) ? raw : trim_copy(raw.substr(0, nl));
    if (nl != std::string::npos) {
        std::istringstream iss(raw.substr(nl + 1));
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim_copy(line.substr(0, eq));
            std::string v = line.substr(eq + 1);
            if (!k.empty()) out.params[k] = v;
        }
    }
    return true;
}

bool dispatch_ipc_command(AppState *state,
                          const std::string &raw,
                          const IpcHostActions &actions,
                          std::string *reply_json_out) {
    IpcRequest req;
    if (!parse_ipc_request(raw, req)) {
        if (reply_json_out) {
            *reply_json_out = "{\"ok\":false,\"error\":\"invalid_ipc_request\"}";
        }
        return false;
    }

    // Exact match handlers (keyed by canonical command name)
    static const std::unordered_map<std::string, HandlerFn> kExactRoutes = {
        {"launch:steam", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            native_shell_set_cc_visible(s, false);
            ensure_steam_play_preconfig();
            launch_steam_client(s);
        }},
        {"launch:steam-gamepadui", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            native_shell_set_cc_visible(s, false);
            ensure_steam_play_preconfig();
            launch_steam_gamepadui(s, load_perf_config());
        }},
        {"steam:scan", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_steam_library(s);
            push_storage_caches(s);
        }},
        {"steam:scan-caches", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_storage_caches(s);
        }},
        {"steam:clean-caches", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            clean_storage_caches(s, r.legacy_raw);
        }},
        {"steam:override-set", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            handle_game_override_command(s, r.legacy_raw, false);
        }},
        {"welcome:open", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            native_shell_set_cc_visible(s, false);
            if (a.open_welcome) a.open_welcome(s);
        }},
        {"welcome:complete", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            mark_welcome_completed();
            if (a.close_welcome) a.close_welcome(s);
        }},
        {"launch:terminal", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            native_shell_set_cc_visible(s, false);
            std::string term_bin = has_executable("spinel-terminal")
                ? "spinel-terminal"
                : (has_executable("ptyxis") ? "ptyxis" : "foot");
            spawn_argv_logged({term_bin}, "Terminal");
        }},
        {"launch:files", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            native_shell_set_cc_visible(s, false);
            spawn_argv_logged({"spinel-files"}, "Files");
        }},
        {"desktop:open-folder", +[](AppState *, const IpcRequest &, const IpcHostActions &) {
            std::error_code ec;
            fs::path desk = fs::path(home_dir()) / "Desktop";
            fs::create_directories(desk, ec);
            spawn_argv_detached({"spinel-files", desk.string()});
        }},
        {"desktop:new-folder", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            std::error_code ec;
            fs::path desk = fs::path(home_dir()) / "Desktop";
            fs::create_directories(desk, ec);
            fs::path target = desk / "新建文件夹";
            int idx = 2;
            while (fs::exists(target, ec) && idx < 100) {
                target = desk / ("新建文件夹 " + std::to_string(idx++));
            }
            fs::create_directory(target, ec);
            native_shell_sync_desktop(s);
        }},
        {"ime:toggle", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            if (has_executable("fcitx5-remote")) {
                std::string out;
                int rc = 1;
                run_argv_capture({"fcitx5-remote", "-t"}, out, rc);
            }
            native_shell_sync_telemetry(s);
        }},
        {"launch:browser", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            native_shell_set_cc_visible(s, false);
            if (has_executable("google-chrome-stable")) {
                launch_browser_command({"google-chrome-stable"}, "Browser:Chrome");
            } else if (has_executable("chromium")) {
                launch_browser_command({"chromium"}, "Browser:Chromium");
            } else if (has_executable("firefox")) {
                launch_browser_command({"firefox"}, "Browser:Firefox");
            } else if (a.open_store) {
                a.open_store(s, "");
            }
        }},
        {"games:open", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            native_shell_set_cc_visible(s, false);
            if (a.open_games) a.open_games(s);
        }},
        {"store:open", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            native_shell_set_cc_visible(s, false);
            if (a.open_store) a.open_store(s, r.param("focus", r.param("id")));
        }},
        {"store:refresh", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            if (a.store_refresh) a.store_refresh(s);
        }},
        {"store:install", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            if (a.store_action) a.store_action(s, "install", r.legacy_raw);
        }},
        {"store:remove", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            if (a.store_action) a.store_action(s, "remove", r.legacy_raw);
        }},
        {"store:sys-update", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            if (a.store_action) a.store_action(s, "sys-update", r.legacy_raw);
        }},
        {"store:mirror-test", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            if (a.store_action) a.store_action(s, "mirror-test", r.legacy_raw);
        }},
        {"store:keyring-fix", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            if (a.store_action) a.store_action(s, "keyring-fix", r.legacy_raw);
        }},
        {"store:svc-start", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            if (a.store_action) a.store_action(s, "svc-start", r.legacy_raw);
        }},
        {"store:svc-stop", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            if (a.store_action) a.store_action(s, "svc-stop", r.legacy_raw);
        }},
        {"store:webapp-pin", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            if (a.store_action) a.store_action(s, "webapp-pin", r.legacy_raw);
        }},
        {"store:webapp-unpin", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            if (a.store_action) a.store_action(s, "webapp-unpin", r.legacy_raw);
        }},
        {"store:cancel", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            if (a.store_cancel) a.store_cancel(s);
        }},
        {"installer:open", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            native_shell_set_cc_visible(s, false);
            if (a.open_installer) a.open_installer(s);
        }},
        {"installer:scan-disks", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            if (a.installer_scan) a.installer_scan(s);
        }},
        {"installer:start", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            if (a.installer_start) a.installer_start(s, r.legacy_raw);
        }},
        {"logs:native", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            native_shell_set_cc_visible(s, false);
            open_native_log_window(s);
        }},
        {"cc:toggle", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            native_shell_toggle_cc(s);
        }},
        {"cc:close", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            native_shell_set_cc_visible(s, false);
        }},
        {"launcher:toggle", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            native_shell_set_cc_visible(s, false);
            native_shell_toggle_launcher(s);
        }},
        {"launcher:open", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            native_shell_set_cc_visible(s, false);
            native_shell_toggle_launcher(s);
        }},
        {"wallpaper:clear-image", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            set_custom_wallpaper_image(s, "");
            if (a.notify_webui_sync) a.notify_webui_sync("sync-theme");
        }},
        {"wallpaper:pick-image", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            if (a.pick_wallpaper) a.pick_wallpaper(s);
        }},
        {"system:audio-mute-toggle", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            trigger_media_osd(s, "mute");
        }},
        {"dnd:toggle", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            toggle_dnd_mode(s);
        }},
        {"system:idle-lock:on", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            set_idle_lock_enabled(s, true);
        }},
        {"system:idle-lock:off", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            set_idle_lock_enabled(s, false);
        }},
        {"system:lock", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            native_shell_set_cc_visible(s, false);
            spawn_argv_detached({has_executable("spinel-lock") ? "spinel-lock" : "swaylock"});
        }},
        {"system:suspend", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            native_shell_set_cc_visible(s, false);
            spawn_argv_detached({"sh", "-c", "(spinel-lock || true); systemctl suspend"});
        }},
        {"settings:audio-get", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_audio_power_state(s);
        }},
        {"settings:mic-mute", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            toggle_mic_mute(s);
        }},
        {"settings:shortcuts-get", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_shortcuts_config(s);
        }},
        {"settings:shortcuts-set", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            update_shortcuts_config(s, r.legacy_raw);
        }},
        {"settings:shortcuts-reset", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            reset_shortcuts_config(s);
        }},
        {"theme:refresh", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_theme_state(s);
            native_shell_sync_theme(s);
        }},
        {"theme:export", +[](AppState *, const IpcRequest &, const IpcHostActions &) {
            export_custom_theme_template();
        }},
        {"system:audio-down", +[](AppState *, const IpcRequest &, const IpcHostActions &) {
            spawn_argv_detached({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-"});
        }},
        {"system:audio-up", +[](AppState *, const IpcRequest &, const IpcHostActions &) {
            spawn_argv_detached({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%+"});
        }},
        {"power:reboot", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            if (a.confirm_power) a.confirm_power(s, false);
        }},
        {"power:shutdown", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            if (a.confirm_power) a.confirm_power(s, true);
        }},
        {"telemetry:refresh", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            if (a.push_all_telemetry) a.push_all_telemetry(s);
            native_shell_sync_desktop(s);
            native_shell_sync_telemetry(s);
            push_theme_state(s);
            push_perf_config(s);
            push_steam_library(s);
            push_desktop_apps(s);
            push_wallpaper_pref(s);
            push_storage_caches(s);
            push_welcome_status(s);
            if (a.push_windows) a.push_windows(s);
        }},
        {"perf:get", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_perf_config(s);
        }},
        {"perf:toggle-gamemode", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            PerfConfig cfg = load_perf_config();
            update_perf_config(s, std::string("gamemode=") + (cfg.gamemode ? "false" : "true"));
            if (a.notify_webui_sync) a.notify_webui_sync("sync-perf");
        }},
        {"perf:toggle-fsr", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            PerfConfig cfg = load_perf_config();
            update_perf_config(s, std::string("fsr=") + (cfg.fsr ? "false" : "true"));
            if (a.notify_webui_sync) a.notify_webui_sync("sync-perf");
        }},
        {"perf:cycle-mangohud", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            PerfConfig cfg = load_perf_config();
            std::string next = cfg.mangohud == "off" ? "compact" : (cfg.mangohud == "compact" ? "full" : "off");
            update_perf_config(s, "mangohud=" + next);
            if (a.notify_webui_sync) a.notify_webui_sync("sync-perf");
        }},
        {"perf:set", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            update_perf_config(s, r.legacy_raw);
        }},
        {"perf:apply-steam", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            PerfConfig cfg = load_perf_config();
            SteamPerfApplyResult sr = apply_perf_to_steam_configs(cfg);
            std::string msg;
            bool ok = true;
            if (sr.steam_running) {
                ok = false;
                msg = "检测到 Steam 正在运行中（退出时 Steam 会覆盖本地配置）。请先完全退出 Steam 后再点击「写入全部 Steam 游戏」。";
            } else if (sr.games_updated > 0) {
                msg = "已成功将当前性能参数直接写入 " + std::to_string(sr.games_updated) + " 款已安装 Steam 游戏的启动配置！";
            } else {
                msg = "当前尚未检测到本地已安装的 Steam 游戏；系统全局预设与 MangoHud.conf 已准备就绪。";
            }
            if (s && s->settings_view) {
                eval_js(s->settings_view,
                        "window.__spinelPerfFeedback && window.__spinelPerfFeedback(" +
                            std::string(ok ? "true" : "false") + ",\"" + json_escape(msg) + "\");");
            }
        }},
        {"settings:wifi-list", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_wifi_state(s, false);
            push_proxy_config(s);
        }},
        {"settings:proxy-get", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_proxy_config(s);
        }},
        {"settings:proxy-save", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            update_proxy_config(s, r.legacy_raw);
        }},
        {"settings:proxy-clear", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            update_proxy_config(s, "enabled=false");
        }},
        {"settings:wifi-scan", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_wifi_state(s, true);
        }},
        {"settings:wifi-connect", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            connect_wifi(s, r.legacy_raw);
        }},
        {"settings:net-ip-apply", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            apply_iface_ip_config(s, r.legacy_raw);
        }},
        {"settings:bt-status", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_bt_state(s, false);
        }},
        {"settings:bt-scan", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_bt_state(s, true);
        }},
        {"settings:bt-power:on", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            set_bt_power(s, true);
        }},
        {"settings:bt-power:off", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            set_bt_power(s, false);
        }},
        {"settings:display-scan", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_display_state(s);
        }},
        {"settings:display-apply", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            apply_display_mode(s, r.legacy_raw);
        }},
        {"settings:doctor-scan", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_doctor_report(s);
        }},
        {"settings:doctor-export", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            export_diagnostic_report(s);
        }},
        {"ime:status", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_ime_state(s);
        }},
        {"ime:mode:pinyin", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            set_ime_engine(s, "pinyin");
        }},
        {"ime:mode:rime", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            set_ime_engine(s, "rime");
        }},
        {"ime:install-rime-ice", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            install_rime_ice_dict(s);
        }},
        {"ime:restart", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            restart_fcitx5_daemon(s);
        }},
        {"ime:config", +[](AppState *, const IpcRequest &, const IpcHostActions &) {
            spawn_argv_logged({"fcitx5-configtool"}, "IME-Config");
        }},
        {"settings:perms-get", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_perms_state(s);
        }},
        {"settings:perms-polkit", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            restart_polkit_agent(s);
        }},
        {"settings:perms-portal", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            reload_desktop_portals(s);
        }},
        {"perms:set", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            apply_flatpak_perm(s, r.legacy_raw);
        }},
        {"perms:portal-reset", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            reset_portal_perm(s, r.legacy_raw);
        }},
        {"settings:procs-get", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_process_list(s);
        }},
        {"monitor:procs-get", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_process_list(s);
        }},
        {"settings:devices-get", +[](AppState *s, const IpcRequest &, const IpcHostActions &) {
            push_devices_state(s);
        }},
        {"doctor:open", +[](AppState *s, const IpcRequest &, const IpcHostActions &a) {
            native_shell_set_cc_visible(s, false);
            if (a.open_doctor) a.open_doctor(s);
        }},
        {"monitor:open", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            native_shell_set_cc_visible(s, false);
            std::string tab = r.param("tab", r.param("id", "procs"));
            if (a.open_monitor) a.open_monitor(s, tab == "logs" ? "logs" : "procs");
        }},
        {"devices:audio-disable", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            disable_audio_device(s, r.legacy_raw);
        }},
        {"clip:clear", +[](AppState *, const IpcRequest &, const IpcHostActions &) {
            clear_clipboard_history();
        }},
        {"workspace:next", +[](AppState *, const IpcRequest &, const IpcHostActions &a) {
            if (a.workspace_step) a.workspace_step(1);
        }},
        {"workspace:prev", +[](AppState *, const IpcRequest &, const IpcHostActions &a) {
            if (a.workspace_step) a.workspace_step(-1);
        }},
    };

    // Prefix / parameterized routes (support both "prefix:val" and JSON params)
    static const PrefixRoute kPrefixRoutes[] = {
        {"steam:launch:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            std::string appid = arg_or_param(r, "steam:launch:", {"appid", "id"});
            if (appid.empty()) return;
            native_shell_set_cc_visible(s, false);
            ensure_steam_play_preconfig();
            launch_steam_game(appid, load_perf_config());
        }},
        {"steam:compat:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            open_steam_compatdata(s, arg_or_param(r, "steam:compat:", {"appid", "id"}));
        }},
        {"steam:override-clear:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            handle_game_override_command(s, arg_or_param(r, "steam:override-clear:", {"appid", "id"}), true);
        }},
        {"app:launch:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            native_shell_set_cc_visible(s, false);
            launch_desktop_app_id(arg_or_param(r, "app:launch:", {"id", "app"}));
        }},
        {"store:open:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            native_shell_set_cc_visible(s, false);
            if (a.open_store) a.open_store(s, arg_or_param(r, "store:open:", {"focus", "id"}));
        }},
        {"store:launch:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            native_shell_set_cc_visible(s, false);
            if (a.store_launch) a.store_launch(s, arg_or_param(r, "store:launch:", {"id"}));
        }},
        {"settings:open:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            std::string tab = arg_or_param(r, "settings:open:", {"tab", "id"});
            if (tab == "procs" || tab == "logs") {
                native_shell_set_cc_visible(s, false);
                if (a.open_monitor) a.open_monitor(s, tab);
                return;
            }
            if (tab == "wifi" || tab == "bt" || tab == "display" || tab == "devices" || tab == "audio" || tab == "shortcuts" ||
                tab == "perf" || tab == "appearance" || tab == "perms") {
                native_shell_set_cc_visible(s, false);
                if (a.open_settings) a.open_settings(s, tab);
            }
        }},
        {"monitor:open:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            std::string tab = arg_or_param(r, "monitor:open:", {"tab", "id"});
            native_shell_set_cc_visible(s, false);
            if (a.open_monitor) a.open_monitor(s, tab == "logs" ? "logs" : "procs");
        }},
        {"devices:mount:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            mount_block_device(s, arg_or_param(r, "devices:mount:", {"dev", "path"}), false);
        }},
        {"devices:mount-ro:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            mount_block_device(s, arg_or_param(r, "devices:mount-ro:", {"dev", "path"}), true);
        }},
        {"devices:unmount:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            unmount_block_device(s, arg_or_param(r, "devices:unmount:", {"dev", "path"}));
        }},
        {"devices:eject:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            eject_block_device(s, arg_or_param(r, "devices:eject:", {"disk", "dev", "path"}));
        }},
        {"devices:open:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            open_mount_folder(s, arg_or_param(r, "devices:open:", {"path"}));
        }},
        {"devices:fstab-set", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            set_block_fstab_mount(s, r.legacy_raw);
        }},
        {"devices:fstab-remove", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            remove_block_fstab_mount(s, r.legacy_raw);
        }},
        {"devices:audio-enable:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            enable_audio_device(s, arg_or_param(r, "devices:audio-enable:", {"node", "name"}));
        }},
        {"system:audio-set:", +[](AppState *, const IpcRequest &r, const IpcHostActions &) {
            std::string v = arg_or_param(r, "system:audio-set:", {"value", "pct"});
            int pct = std::clamp(std::atoi(v.c_str()), 0, 100);
            spawn_argv_detached({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", std::to_string(pct) + "%"});
        }},
        {"system:brightness-set:", +[](AppState *, const IpcRequest &r, const IpcHostActions &) {
            std::string v = arg_or_param(r, "system:brightness-set:", {"value", "pct"});
            int pct = std::clamp(std::atoi(v.c_str()), 5, 100);
            set_system_brightness_pct(pct);
        }},
        {"system:screenshot:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            std::string smode = arg_or_param(r, "system:screenshot:", {"mode"}) == "full" ? "full" : "area";
            native_shell_set_cc_visible(s, false);
            auto *shot_arg = new std::string(smode);
            g_timeout_add(180, [](gpointer d) -> gboolean {
                std::unique_ptr<std::string> m(static_cast<std::string *>(d));
                if (has_executable("spinel-screenshot")) {
                    spawn_argv_detached({"spinel-screenshot", *m});
                } else if (has_executable("grim")) {
                    spawn_argv_detached({"grim"});
                }
                return G_SOURCE_REMOVE;
            }, shot_arg);
        }},
        {"system:power-profile:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            set_power_profile(s, arg_or_param(r, "system:power-profile:", {"profile", "value"}));
        }},
        {"settings:audio-sink:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            set_audio_default_node(s, std::atoi(arg_or_param(r, "settings:audio-sink:", {"id"}).c_str()), true);
        }},
        {"settings:audio-source:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            set_audio_default_node(s, std::atoi(arg_or_param(r, "settings:audio-source:", {"id"}).c_str()), false);
        }},
        {"settings:mic-vol:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            set_mic_volume(s, std::atoi(arg_or_param(r, "settings:mic-vol:", {"value", "pct"}).c_str()));
        }},
        {"settings:audio-test-speaker:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            test_audio_speaker(s, arg_or_param(r, "settings:audio-test-speaker:", {"channel", "value"}));
        }},
        {"settings:audio-test-mic:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            toggle_mic_test(s, arg_or_param(r, "settings:audio-test-mic:", {"mode", "action"}));
        }},
        {"window:toggle:", +[](AppState *, const IpcRequest &r, const IpcHostActions &a) {
            uint64_t wid = std::strtoull(arg_or_param(r, "window:toggle:", {"id", "wid"}).c_str(), nullptr, 10);
            if (wid > 0 && a.window_toggle) a.window_toggle(wid);
        }},
        {"window:close:", +[](AppState *, const IpcRequest &r, const IpcHostActions &a) {
            uint64_t wid = std::strtoull(arg_or_param(r, "window:close:", {"id", "wid"}).c_str(), nullptr, 10);
            if (wid > 0 && a.window_close) a.window_close(wid);
        }},
        {"window:quit:", +[](AppState *, const IpcRequest &r, const IpcHostActions &a) {
            uint64_t wid = std::strtoull(arg_or_param(r, "window:quit:", {"id", "wid"}).c_str(), nullptr, 10);
            if (wid > 0 && a.window_quit) a.window_quit(wid);
        }},
        {"wallpaper:set:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            std::string wp = arg_or_param(r, "wallpaper:set:", {"id", "theme"});
            if (wp == "crystal-blue") wp = "crystal-aurora";
            else if (wp == "obsidian-purple") wp = "obsidian-neon";
            else if (wp == "emerald-aurora") wp = "emerald-nordic";
            set_active_theme(s, wp);
            if (a.notify_webui_sync) a.notify_webui_sync("sync-theme");
        }},
        {"theme:set:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &a) {
            set_active_theme(s, arg_or_param(r, "theme:set:", {"id", "theme"}));
            if (a.notify_webui_sync) a.notify_webui_sync("sync-theme");
        }},
        {"settings:wifi-disconnect:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            disconnect_iface(s, arg_or_param(r, "settings:wifi-disconnect:", {"device", "iface"}));
        }},
        {"settings:bt-connect:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            bt_device_action(s, "connect", arg_or_param(r, "settings:bt-connect:", {"mac", "id"}));
        }},
        {"settings:bt-disconnect:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            bt_device_action(s, "disconnect", arg_or_param(r, "settings:bt-disconnect:", {"mac", "id"}));
        }},
        {"settings:bt-remove:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            bt_device_action(s, "remove", arg_or_param(r, "settings:bt-remove:", {"mac", "id"}));
        }},
        {"settings:logs:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            push_system_logs(s, arg_or_param(r, "settings:logs:", {"source"}));
        }},
        {"monitor:logs:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            push_system_logs(s, arg_or_param(r, "monitor:logs:", {"source"}));
        }},
        {"settings:logs-clear:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            clear_system_logs(s, arg_or_param(r, "settings:logs-clear:", {"source"}));
        }},
        {"monitor:logs-clear:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            clear_system_logs(s, arg_or_param(r, "monitor:logs-clear:", {"source"}));
        }},
        {"perms:reset:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            reset_flatpak_app_perms(s, arg_or_param(r, "perms:reset:", {"app", "id"}));
        }},
        {"procs:kill:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            kill_process_by_pid(s, std::atoi(arg_or_param(r, "procs:kill:", {"pid"}).c_str()), false);
        }},
        {"procs:force:", +[](AppState *s, const IpcRequest &r, const IpcHostActions &) {
            kill_process_by_pid(s, std::atoi(arg_or_param(r, "procs:force:", {"pid"}).c_str()), true);
        }},
        {"clip:copy:", +[](AppState *, const IpcRequest &r, const IpcHostActions &) {
            size_t idx = static_cast<size_t>(std::strtoul(arg_or_param(r, "clip:copy:", {"index", "idx"}).c_str(), nullptr, 10));
            auto items = load_clipboard_history();
            if (idx < items.size()) {
                copy_text_to_clipboard(items[idx].text);
            }
        }},
        {"workspace:switch:", +[](AppState *, const IpcRequest &r, const IpcHostActions &a) {
            int idx1 = std::atoi(arg_or_param(r, "workspace:switch:", {"index", "num"}).c_str());
            if (idx1 >= 1 && a.workspace_switch) {
                a.workspace_switch(idx1);
            }
        }},
    };

    bool handled = false;
    if (auto it = kExactRoutes.find(req.method); it != kExactRoutes.end()) {
        it->second(state, req, actions);
        handled = true;
    } else {
        for (const auto &pr : kPrefixRoutes) {
            if (req.method.rfind(pr.prefix, 0) == 0 ||
                (req.is_json && (req.method + ":") == pr.prefix)) {
                pr.fn(state, req, actions);
                handled = true;
                break;
            }
        }
    }

    if (req.has_id) {
        std::ostringstream rep;
        rep << "{\"id\":" << req.id_json
            << ",\"ok\":" << (handled ? "true" : "false")
            << ",\"method\":\"" << json_escape(req.method) << "\"";
        if (!handled) {
            rep << ",\"error\":\"unknown_method\"";
        }
        rep << "}";
        std::string reply = rep.str();
        if (reply_json_out) {
            *reply_json_out = reply;
        } else {
            post_ipc_reply_to_views(state, reply);
        }
    }
    return handled;
}
