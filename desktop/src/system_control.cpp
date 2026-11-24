#include "system_control.hpp"
#include "common.hpp"
#include "native_shell.hpp"
#include "steam_library.hpp"

#include <fcntl.h>
#include <linux/joystick.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct JsIdleCall {
    AppState *state;
    std::string script;
    bool also_bar = false;
};

gboolean run_js_idle(gpointer data) {
    std::unique_ptr<JsIdleCall> call(static_cast<JsIdleCall *>(data));
    if (call->state->shutting_down.load()) return G_SOURCE_REMOVE;
    eval_js(call->state->settings_view, call->script);
    if (call->also_bar) {
        eval_js(call->state->bar_view, call->script);
        eval_js(call->state->desktop_view, call->script);
    }
    return G_SOURCE_REMOVE;
}

void post_js_to_main(AppState *state, std::string script, bool also_bar = true) {
    if (!state || state->shutting_down.load()) return;
    g_idle_add(run_js_idle, new JsIdleCall{state, std::move(script), also_bar});
}

fs::path config_dir() {
    return fs::path(home_dir()) / ".config/spinelos";
}

void sync_mangohud_config(const PerfConfig &cfg) {
    if (cfg.mangohud == "off") return;
    std::error_code ec;
    fs::path mh_dir = fs::path(home_dir()) / ".config/MangoHud";
    fs::create_directories(mh_dir, ec);
    std::ofstream ofs(mh_dir / "MangoHud.conf");
    if (!ofs.is_open()) return;

    ofs << "# Managed by SpinelOS Performance Control\n";
    ofs << "position=top-left\n";
    ofs << "background_alpha=0.45\n";
    ofs << "font_size=20\n";
    ofs << "fps\n";
    ofs << "gpu_stats\n";
    ofs << "gpu_temp\n";
    ofs << "cpu_stats\n";
    ofs << "cpu_temp\n";
    if (cfg.mangohud == "full") {
        ofs << "frametime=1\n";
        ofs << "frame_timing=1\n";
        ofs << "ram\n";
        ofs << "vram\n";
        ofs << "gamemode\n";
    } else {
        ofs << "frametime=0\n";
        ofs << "legacy_layout=0\n";
        ofs << "horizontal\n";
    }
}

std::vector<std::string> split_nmcli_escaped(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            cur += line[i + 1];
            ++i;
        } else if (line[i] == ':') {
            out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += line[i];
        }
    }
    out.push_back(std::move(cur));
    return out;
}

bool is_valid_iface(const std::string &s) {
    if (s.empty() || s.size() > 32) return false;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

bool is_valid_mac(const std::string &s) {
    if (s.size() != 17) return false;
    for (size_t i = 0; i < 17; ++i) {
        if (i % 3 == 2) {
            if (s[i] != ':') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    return true;
}

bool is_valid_output_name(const std::string &s) {
    if (s.empty() || s.size() > 32) return false;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && c != '-' && c != '_') return false;
    }
    return true;
}

bool is_valid_mode_str(const std::string &s) {
    if (s.empty() || s.size() > 32) return false;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isdigit(c) && c != 'x' && c != '@' && c != '.' && c != 'H' && c != 'z') {
            return false;
        }
    }
    return true;
}

bool is_valid_scale_str(const std::string &s) {
    if (s.empty() || s.size() > 8) return false;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isdigit(c) && c != '.') return false;
    }
    return true;
}

bool is_valid_proxy_url(const std::string &s) {
    if (s.empty()) return true;
    if (s.size() > 256) return false;
    bool ok_scheme = s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0 ||
                     s.rfind("socks5://", 0) == 0 || s.rfind("socks5h://", 0) == 0 ||
                     s.rfind("socks4://", 0) == 0;
    if (!ok_scheme) return false;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c == '"' || c == '\'' || c == '\\' || c == ';') return false;
    }
    return true;
}

std::string sanitize_no_proxy(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == ',' || c == '.' || c == ':' || c == '/' || c == '-' || c == '_') {
            out += ch;
        }
    }
    return out;
}

std::string detect_gpu_summary() {
    std::string gpu = run_cmd_capture(
        "lspci -mm 2>/dev/null | awk -F'\"' '/VGA|3D|Display/ {print $6; exit}'"
    );
    return gpu.empty() ? "Generic DRM/Mesa" : gpu;
}

std::string detect_mem_usage(int *used_mb_out = nullptr) {
    std::ifstream ifs("/proc/meminfo");
    if (!ifs.is_open()) return "N/A";
    long total_kb = 0;
    long avail_kb = 0;
    std::string key;
    long val = 0;
    std::string unit;
    while (ifs >> key >> val >> unit) {
        if (key == "MemTotal:") total_kb = val;
        else if (key == "MemAvailable:") avail_kb = val;
    }
    if (total_kb <= 0) return "N/A";
    long used_mb = (total_kb - avail_kb) / 1024;
    long total_mb = total_kb / 1024;
    if (used_mb_out) *used_mb_out = static_cast<int>(used_mb);
    std::ostringstream oss;
    oss << used_mb << " / " << total_mb << " MB";
    return oss.str();
}

std::string detect_network_status() {
    std::error_code ec;
    fs::path net_dir("/sys/class/net");
    if (!fs::is_directory(net_dir, ec)) return "Offline";
    for (const auto &entry : fs::directory_iterator(net_dir, ec)) {
        std::string iface = entry.path().filename().string();
        if (iface == "lo" || iface.rfind("veth", 0) == 0 || iface.rfind("docker", 0) == 0 || iface.rfind("br-", 0) == 0) {
            continue;
        }
        std::string oper = trim_copy(read_first_line((entry.path() / "operstate").string(), ""));
        std::string carrier = trim_copy(read_first_line((entry.path() / "carrier").string(), ""));
        if (oper == "up" || carrier == "1") {
            bool is_wifi = fs::exists(entry.path() / "wireless", ec) || iface.rfind("wl", 0) == 0;
            return iface + (is_wifi ? " (wifi)" : " (ethernet)");
        }
    }
    return "Offline";
}

bool detect_low_fx_mode() {
    std::error_code ec;
    fs::path drm_dir("/sys/class/drm");
    if (!fs::is_directory(drm_dir, ec)) return true;

    for (const auto &entry : fs::directory_iterator(drm_dir, ec)) {
        std::string name = entry.path().filename().string();
        if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos) continue;
        std::string vendor = trim_copy(read_first_line((entry.path() / "device/vendor").string(), ""));
        if (vendor == "0x10de" || vendor == "0x1002" || vendor == "0x8086") {
            return false;  // Real NVIDIA, AMD, or Intel GPU present
        }
    }
    return true;
}

}  // namespace

PerfConfig load_perf_config() {
    PerfConfig cfg;
    std::ifstream ifs(config_dir() / "perf.conf");
    if (!ifs.is_open()) return cfg;

    std::string line;
    while (std::getline(ifs, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        if (k == "mangohud" && (v == "off" || v == "compact" || v == "full")) cfg.mangohud = v;
        else if (k == "fps_limit") {
            int fps = std::atoi(v.c_str());
            if (fps == 0 || fps == 60 || fps == 90 || fps == 120 || fps == 144) cfg.fps_limit = fps;
        } else if (k == "fsr") cfg.fsr = (v == "true");
        else if (k == "gamemode") cfg.gamemode = (v == "true");
        else if (k == "render_res" && (v == "native" || v == "720p" || v == "900p" || v == "1080p")) {
            cfg.render_res = v;
        }
    }
    return cfg;
}

ProxyConfig load_proxy_config() {
    ProxyConfig cfg;
    std::ifstream ifs(config_dir() / "proxy.conf");
    if (!ifs.is_open()) return cfg;
    std::string line;
    while (std::getline(ifs, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        if (k == "enabled") cfg.enabled = (v == "true" || v == "1");
        else if (k == "http_proxy" && is_valid_proxy_url(v)) cfg.http_proxy = v;
        else if (k == "socks_proxy" && is_valid_proxy_url(v)) cfg.socks_proxy = v;
        else if (k == "no_proxy") {
            std::string clean = sanitize_no_proxy(v);
            if (!clean.empty()) cfg.no_proxy = clean;
        }
    }
    return cfg;
}

void sync_process_proxy_env() {
    ProxyConfig cfg = load_proxy_config();
    const char *vars[] = {
        "http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY",
        "all_proxy", "ALL_PROXY", "no_proxy", "NO_PROXY"
    };
    if (!cfg.enabled || (cfg.http_proxy.empty() && cfg.socks_proxy.empty())) {
        for (const char *v : vars) unsetenv(v);
        return;
    }
    std::string http_val = !cfg.http_proxy.empty() ? cfg.http_proxy : cfg.socks_proxy;
    std::string all_val = !cfg.socks_proxy.empty() ? cfg.socks_proxy : cfg.http_proxy;
    setenv("http_proxy", http_val.c_str(), 1);
    setenv("https_proxy", http_val.c_str(), 1);
    setenv("HTTP_PROXY", http_val.c_str(), 1);
    setenv("HTTPS_PROXY", http_val.c_str(), 1);
    setenv("all_proxy", all_val.c_str(), 1);
    setenv("ALL_PROXY", all_val.c_str(), 1);
    setenv("no_proxy", cfg.no_proxy.c_str(), 1);
    setenv("NO_PROXY", cfg.no_proxy.c_str(), 1);
}

void push_proxy_config(AppState *state) {
    if (!state || !state->settings_view) return;
    ProxyConfig cfg = load_proxy_config();
    std::string gw = run_cmd_capture("ip route 2>/dev/null | awk '/default/ {print $3; exit}'");
    std::ostringstream js;
    js << "window.__spinelProxyConfig && window.__spinelProxyConfig({"
       << "\"enabled\":" << (cfg.enabled ? "true" : "false") << ","
       << "\"http_proxy\":\"" << json_escape(cfg.http_proxy) << "\","
       << "\"socks_proxy\":\"" << json_escape(cfg.socks_proxy) << "\","
       << "\"no_proxy\":\"" << json_escape(cfg.no_proxy) << "\","
       << "\"gateway\":\"" << json_escape(gw) << "\""
       << "});";
    eval_js(state->settings_view, js.str());
}

void update_proxy_config(AppState *state, const std::string &payload) {
    ProxyConfig cfg = load_proxy_config();
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        if (k == "enabled") cfg.enabled = (v == "true" || v == "1");
        else if (k == "http_proxy") {
            if (!is_valid_proxy_url(v)) {
                eval_js(state ? state->settings_view : nullptr,
                        "window.__spinelProxyAction && window.__spinelProxyAction(false, \"HTTP 代理地址格式无效（需以 http://、https:// 或 socks5:// 开头）\");");
                return;
            }
            cfg.http_proxy = v;
        } else if (k == "socks_proxy") {
            if (!is_valid_proxy_url(v)) {
                eval_js(state ? state->settings_view : nullptr,
                        "window.__spinelProxyAction && window.__spinelProxyAction(false, \"SOCKS 代理地址格式无效（需以 socks5:// 等开头）\");");
                return;
            }
            cfg.socks_proxy = v;
        } else if (k == "no_proxy") {
            std::string clean = sanitize_no_proxy(v);
            if (!clean.empty()) cfg.no_proxy = clean;
        }
    }
    if (cfg.http_proxy.empty() && cfg.socks_proxy.empty()) {
        cfg.enabled = false;
    }

    std::ostringstream out;
    out << "enabled=" << (cfg.enabled ? "true" : "false") << "\n"
        << "http_proxy=" << cfg.http_proxy << "\n"
        << "socks_proxy=" << cfg.socks_proxy << "\n"
        << "no_proxy=" << cfg.no_proxy << "\n";
    write_text_file((config_dir() / "proxy.conf").string(), out.str());
    sync_process_proxy_env();
    append_app_log(cfg.enabled
        ? ("[Proxy] 已启用系统代理: http=" + cfg.http_proxy + " socks=" + cfg.socks_proxy)
        : "[Proxy] 已关闭系统代理（直连模式）");
    if (state) {
        push_proxy_config(state);
        std::string pg_out;
        int pg_rc = 1;
        bool steam_running = run_argv_capture({"pgrep", "-f", "ubuntu12_32/steam"}, pg_out, pg_rc) && pg_rc == 0;
        std::string ok_msg = cfg.enabled
            ? (steam_running
                ? "系统代理已启用！检测到 Steam 正在运行，再次点击任意 Steam 启动按钮会自动用新代理重启它。"
                : "系统代理已启用，对新启动的 Steam / 商店 / 终端立即生效。")
            : "已关闭代理并恢复直连。";
        eval_js(state->settings_view,
                "window.__spinelProxyAction && window.__spinelProxyAction(true,\"" + json_escape(ok_msg) + "\");");
    }
}

void push_perf_config(AppState *state) {
    if (!state) return;
    PerfConfig cfg = load_perf_config();
    std::ostringstream js;
    js << "window.__spinelPerfConfig && window.__spinelPerfConfig({"
       << "\"mangohud\":\"" << cfg.mangohud << "\","
       << "\"fps_limit\":" << cfg.fps_limit << ","
       << "\"fsr\":" << (cfg.fsr ? "true" : "false") << ","
       << "\"gamemode\":" << (cfg.gamemode ? "true" : "false") << ","
       << "\"render_res\":\"" << cfg.render_res << "\""
       << "});";
    std::string script = js.str();
    eval_js(state->games_view, script);
    eval_js(state->settings_view, script);
    eval_js(state->desktop_view, script);
    eval_js(state->bar_view, script);
    eval_js(state->cc_view, script);
    if (state->settings_view) {
        gtk_widget_queue_draw(GTK_WIDGET(state->settings_view));
    }
}

void update_perf_config(AppState *state, const std::string &payload) {
    PerfConfig cfg = load_perf_config();
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "mangohud" && (v == "off" || v == "compact" || v == "full")) cfg.mangohud = v;
        else if (k == "fps_limit") {
            int fps = std::atoi(v.c_str());
            if (fps == 0 || fps == 60 || fps == 90 || fps == 120 || fps == 144) cfg.fps_limit = fps;
        } else if (k == "fsr") cfg.fsr = (v == "true");
        else if (k == "gamemode") cfg.gamemode = (v == "true");
        else if (k == "render_res" && (v == "native" || v == "720p" || v == "900p" || v == "1080p")) {
            cfg.render_res = v;
        }
    }

    std::ostringstream perf_out;
    perf_out << "mangohud=" << cfg.mangohud << "\n"
             << "fps_limit=" << cfg.fps_limit << "\n"
             << "fsr=" << (cfg.fsr ? "true" : "false") << "\n"
             << "gamemode=" << (cfg.gamemode ? "true" : "false") << "\n"
             << "render_res=" << cfg.render_res << "\n";
    write_text_file((config_dir() / "perf.conf").string(), perf_out.str());
    sync_mangohud_config(cfg);
    push_perf_config(state);
    native_shell_sync_perf(state);

    if (state && state->settings_view) {
        SteamPerfApplyResult sr = apply_perf_to_steam_configs(cfg);
        std::string msg;
        bool ok = true;
        if (sr.steam_running) {
            msg = "系统性能预设与 MangoHud.conf 已保存；检测到 Steam 正在运行，退出 Steam 后点击右侧「写入全部 Steam 游戏」即可同步进库内游戏属性。";
        } else if (sr.games_updated > 0) {
            msg = "已自动保存系统预设、MangoHud.conf，并直接写入 " + std::to_string(sr.games_updated) + " 款 Steam 游戏配置！";
        } else {
            msg = "已保存系统性能预设与 MangoHud.conf（主机大屏、游戏中心及后续安装的 Steam 游戏均自动生效）。";
        }
        eval_js(state->settings_view,
                "window.__spinelPerfFeedback && window.__spinelPerfFeedback(" +
                    std::string(ok ? "true" : "false") + ",\"" + json_escape(msg) + "\");");
    }
}

TelemetrySnapshot collect_telemetry() {
    static const std::string cached_gpu = detect_gpu_summary();
    static const std::string cached_kernel = read_first_line("/proc/sys/kernel/osrelease", "Linux");
    static const bool cached_live = is_live_iso_env();
    static const bool cached_low_fx = detect_low_fx_mode();

    TelemetrySnapshot snap;
    snap.gpu = cached_gpu;
    snap.mem = detect_mem_usage(&snap.mem_used_mb);
    snap.net = detect_network_status();
    snap.kernel = cached_kernel;
    snap.is_live = cached_live;
    snap.low_fx = cached_low_fx;

    std::error_code ec;
    for (const auto &entry : fs::directory_iterator("/sys/class/power_supply", ec)) {
        std::string nm = entry.path().filename().string();
        if (nm.rfind("BAT", 0) != 0) continue;
        std::string cap_s = trim_copy(read_first_line((entry.path() / "capacity").string(), ""));
        if (!cap_s.empty()) {
            snap.battery_pct = std::clamp(std::atoi(cap_s.c_str()), 0, 100);
            std::string st = trim_copy(read_first_line((entry.path() / "status").string(), ""));
            snap.battery_charging = (st == "Charging" || st == "Full");
            break;
        }
    }
    return snap;
}

MediaSnapshot collect_media_state() {
    MediaSnapshot snap;
    std::string vol_out = run_cmd_capture("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null");
    auto pos = vol_out.find("Volume:");
    if (pos != std::string::npos) {
        double v = std::atof(vol_out.c_str() + pos + 7);
        snap.volume = std::clamp(static_cast<int>(v * 100.0 + 0.5), 0, 100);
    }

    if (has_executable("brightnessctl")) {
        std::string b_out;
        int b_rc = 1;
        if (run_argv_capture({"brightnessctl", "-m"}, b_out, b_rc) && b_rc == 0 && !b_out.empty()) {
            // Machine format: device,class,current,pct%,max
            auto p = b_out.find('%');
            if (p != std::string::npos) {
                auto comma = b_out.rfind(',', p);
                if (comma != std::string::npos) {
                    snap.brightness = std::clamp(std::atoi(b_out.substr(comma + 1, p - comma - 1).c_str()), 5, 100);
                }
            }
        }
    }
    if (snap.brightness < 0) {
        std::string br_out = run_cmd_capture(
            "for d in /sys/class/backlight/*; do "
            "[ -r \"$d/brightness\" ] && [ -r \"$d/max_brightness\" ] || continue; "
            "cur=$(cat \"$d/brightness\"); max=$(cat \"$d/max_brightness\"); "
            "[ \"$max\" -gt 0 ] && awk \"BEGIN{printf \\\"%d\\\", ($cur/$max)*100}\" && break; "
            "done 2>/dev/null");
        if (!br_out.empty()) {
            snap.brightness = std::clamp(std::atoi(br_out.c_str()), 5, 100);
        }
    }

    std::error_code ec;
    for (const auto &entry : fs::directory_iterator("/sys/class/power_supply", ec)) {
        std::string nm = entry.path().filename().string();
        if (nm.rfind("BAT", 0) != 0) continue;
        std::string cap_s = trim_copy(read_first_line((entry.path() / "capacity").string(), ""));
        if (!cap_s.empty()) {
            snap.battery_pct = std::clamp(std::atoi(cap_s.c_str()), 0, 100);
            std::string st = trim_copy(read_first_line((entry.path() / "status").string(), ""));
            snap.battery_charging = (st == "Charging" || st == "Full");
            break;
        }
    }

    if (has_executable("powerprofilesctl")) {
        std::string pp_out;
        int pp_rc = 1;
        if (run_argv_capture({"powerprofilesctl", "get"}, pp_out, pp_rc) && pp_rc == 0) {
            snap.power_profile = trim_copy(pp_out);
        }
    }
    return snap;
}

void push_telemetry_to_view(WebKitWebView *view) {
    if (!view) return;
    TelemetrySnapshot snap = collect_telemetry();
    std::string gpu = json_escape(snap.gpu);
    std::string mem = json_escape(snap.mem);
    std::string net = json_escape(snap.net);
    std::string kernel = json_escape(snap.kernel);

    std::ostringstream js;
    js << "window.__spinelUpdateTelemetry && window.__spinelUpdateTelemetry({"
       << "\"gpu\":\"" << gpu << "\","
       << "\"mem\":\"" << mem << "\","
       << "\"net\":\"" << net << "\","
       << "\"kernel\":\"" << kernel << "\","
       << "\"is_live\":" << (snap.is_live ? "true" : "false") << ","
       << "\"low_fx\":" << (snap.low_fx ? "true" : "false")
       << "});";
    eval_js(view, js.str());
}

void push_cc_media_state(AppState *state) {
    if (!state || !state->cc_view) return;
    MediaSnapshot snap = collect_media_state();
    std::ostringstream js;
    js << "window.__spinelControlState && window.__spinelControlState({"
       << "\"volume\":" << snap.volume << ","
       << "\"brightness\":" << snap.brightness
       << "});";
    eval_js(state->cc_view, js.str());
}

void push_wifi_state(AppState *state, bool rescan) {
    if (!state || state->wifi_busy.exchange(true)) return;

    std::thread([state, rescan]() {
        std::string dev_out;
        int dev_rc = 0;
        run_argv_capture({"nmcli", "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "dev", "status"}, dev_out, dev_rc);

        std::vector<std::string> wifi_args = {"nmcli", "-t", "-f", "ACTIVE,SSID,SIGNAL,SECURITY", "dev", "wifi", "list"};
        if (rescan) {
            wifi_args.push_back("--rescan");
            wifi_args.push_back("yes");
        }
        std::string wifi_out;
        int wifi_rc = 0;
        run_argv_capture(wifi_args, wifi_out, wifi_rc);

        struct WifiNet {
            bool active = false;
            std::string ssid;
            int signal = 0;
            std::string security;
        };
        std::map<std::string, WifiNet> by_ssid;
        for (const auto &line : split_lines(wifi_out)) {
            if (line.empty()) continue;
            auto cols = split_nmcli_escaped(line);
            if (cols.size() < 4) continue;
            std::string ssid = cols[1];
            if (ssid.empty()) continue;
            bool active = (cols[0] == "yes");
            int sig = std::atoi(cols[2].c_str());
            std::string sec = cols[3];

            auto it = by_ssid.find(ssid);
            if (it == by_ssid.end() || active || (!it->second.active && sig > it->second.signal)) {
                by_ssid[ssid] = WifiNet{active, ssid, sig, sec};
            }
        }

        std::ostringstream js;
        js << "window.__spinelWifiState && window.__spinelWifiState({\"interfaces\":[";
        bool first_dev = true;
        for (const auto &line : split_lines(dev_out)) {
            if (line.empty()) continue;
            auto cols = split_nmcli_escaped(line);
            if (cols.size() < 4 || (cols[1] != "wifi" && cols[1] != "ethernet")) continue;
            if (!first_dev) js << ",";
            first_dev = false;
            js << "{\"device\":\"" << json_escape(cols[0]) << "\","
               << "\"type\":\"" << json_escape(cols[1]) << "\","
               << "\"state\":\"" << json_escape(cols[2]) << "\","
               << "\"connection\":\"" << json_escape(cols[3]) << "\"}";
        }
        js << "],\"networks\":[";
        bool first_net = true;
        for (const auto &kv : by_ssid) {
            const auto &n = kv.second;
            if (!first_net) js << ",";
            first_net = false;
            js << "{\"active\":" << (n.active ? "true" : "false") << ","
               << "\"ssid\":\"" << json_escape(n.ssid) << "\","
               << "\"signal\":" << n.signal << ","
               << "\"security\":\"" << json_escape(n.security) << "\"}";
        }
        js << "]});";

        state->wifi_busy.store(false);
        post_js_to_main(state, js.str());
    }).detach();
}

void connect_wifi(AppState *state, const std::string &payload) {
    if (!state || state->wifi_busy.exchange(true)) return;

    std::string ssid;
    std::string password;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "ssid") ssid = v;
        else if (k == "password") password = v;
    }

    if (ssid.empty()) {
        state->wifi_busy.store(false);
        post_js_to_main(state, "window.__spinelWifiAction && window.__spinelWifiAction(false, \"Wi-Fi 名称不能为空。\");");
        return;
    }

    std::thread([state, ssid, password]() {
        std::vector<std::string> args = {"nmcli", "dev", "wifi", "connect", ssid};
        if (!password.empty()) {
            args.push_back("password");
            args.push_back(password);
        }
        std::string out;
        int rc = 0;
        run_argv_capture(args, out, rc);
        state->wifi_busy.store(false);

        std::ostringstream js;
        js << "window.__spinelWifiAction && window.__spinelWifiAction("
           << (rc == 0 ? "true" : "false") << ",\""
           << json_escape(out.empty() ? (rc == 0 ? "已连接" : "连接失败") : out) << "\");";
        post_js_to_main(state, js.str());
        push_wifi_state(state, false);
    }).detach();
}

void disconnect_iface(AppState *state, const std::string &iface) {
    if (!state || !is_valid_iface(iface) || state->wifi_busy.exchange(true)) return;
    std::thread([state, iface]() {
        std::string out;
        int rc = 0;
        run_argv_capture({"nmcli", "dev", "disconnect", iface}, out, rc);
        state->wifi_busy.store(false);

        std::ostringstream js;
        js << "window.__spinelWifiAction && window.__spinelWifiAction("
           << (rc == 0 ? "true" : "false") << ",\"" << json_escape(out) << "\");";
        post_js_to_main(state, js.str());
        push_wifi_state(state, false);
    }).detach();
}

void push_bt_state(AppState *state, bool do_scan) {
    if (!state || state->bt_busy.exchange(true)) return;

    std::thread([state, do_scan]() {
        if (do_scan) {
            std::string scan_out;
            int scan_rc = 0;
            run_argv_capture({"bluetoothctl", "--timeout", "6", "scan", "on"}, scan_out, scan_rc);
        }

        std::string show_out;
        int show_rc = 0;
        run_argv_capture({"bluetoothctl", "show"}, show_out, show_rc);
        bool powered = (show_out.find("Powered: yes") != std::string::npos);

        std::string devs_out;
        int devs_rc = 0;
        run_argv_capture({"bluetoothctl", "devices"}, devs_out, devs_rc);

        std::ostringstream js;
        js << "window.__spinelBtState && window.__spinelBtState({\"powered\":"
           << (powered ? "true" : "false") << ",\"devices\":[";

        bool first = true;
        for (const auto &line : split_lines(devs_out)) {
            if (line.rfind("Device ", 0) != 0 || line.size() < 25) continue;
            std::string mac = line.substr(7, 17);
            if (!is_valid_mac(mac)) continue;
            std::string name = trim_copy(line.substr(24));

            std::string info_out;
            int info_rc = 0;
            run_argv_capture({"bluetoothctl", "info", mac}, info_out, info_rc);
            bool paired = (info_out.find("Paired: yes") != std::string::npos);
            bool connected = (info_out.find("Connected: yes") != std::string::npos);
            std::string icon;
            for (const auto &iline : split_lines(info_out)) {
                auto pos = iline.find("Icon:");
                if (pos != std::string::npos) {
                    icon = trim_copy(iline.substr(pos + 5));
                    break;
                }
            }

            if (!first) js << ",";
            first = false;
            js << "{\"mac\":\"" << mac << "\","
               << "\"name\":\"" << json_escape(name.empty() ? mac : name) << "\","
               << "\"paired\":" << (paired ? "true" : "false") << ","
               << "\"connected\":" << (connected ? "true" : "false") << ","
               << "\"icon\":\"" << json_escape(icon) << "\"}";
        }
        js << "]});";

        state->bt_busy.store(false);
        post_js_to_main(state, js.str());
    }).detach();
}

void set_bt_power(AppState *state, bool on) {
    if (!state || state->bt_busy.exchange(true)) return;
    std::thread([state, on]() {
        std::string out;
        int rc = 0;
        run_argv_capture({"bluetoothctl", "power", on ? "on" : "off"}, out, rc);
        state->bt_busy.store(false);
        push_bt_state(state, false);
    }).detach();
}

void bt_device_action(AppState *state, const std::string &action, const std::string &mac) {
    if (!state || !is_valid_mac(mac) || state->bt_busy.exchange(true)) return;
    std::thread([state, action, mac]() {
        std::string out;
        int rc = 0;
        if (action == "connect") {
            run_argv_capture({"bluetoothctl", "trust", mac}, out, rc);
            run_argv_capture({"bluetoothctl", "pair", mac}, out, rc);
            run_argv_capture({"bluetoothctl", "connect", mac}, out, rc);
        } else if (action == "disconnect") {
            run_argv_capture({"bluetoothctl", "disconnect", mac}, out, rc);
        } else if (action == "remove") {
            run_argv_capture({"bluetoothctl", "remove", mac}, out, rc);
        }
        state->bt_busy.store(false);

        std::ostringstream js;
        js << "window.__spinelBtAction && window.__spinelBtAction("
           << (rc == 0 ? "true" : "false") << ",\"" << json_escape(out) << "\");";
        post_js_to_main(state, js.str());
        push_bt_state(state, false);
    }).detach();
}

void push_display_state(AppState *state) {
    if (!state) return;
    std::string out;
    int rc = 0;
    run_argv_capture({"wlr-randr", "--json"}, out, rc);
    if (rc != 0 || out.empty() || out.front() != '[') {
        out = "[]";
    }
    eval_js(state->settings_view, "window.__spinelDisplayState && window.__spinelDisplayState(" + out + ");");
}

void apply_display_mode(AppState *state, const std::string &payload) {
    if (!state) return;
    std::string output;
    std::string mode;
    std::string scale = "1";
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "output") output = v;
        else if (k == "mode") mode = v;
        else if (k == "scale") scale = v;
    }

    if (!is_valid_output_name(output) || !is_valid_mode_str(mode) || !is_valid_scale_str(scale)) {
        eval_js(state->settings_view, "window.__spinelDisplayAction && window.__spinelDisplayAction(false, \"显示参数无效\");");
        return;
    }

    std::string out;
    int rc = 0;
    run_argv_capture({"wlr-randr", "--output", output, "--mode", mode, "--scale", scale}, out, rc);
    if (rc == 0) {
        fs::path script_path = config_dir() / "display.sh";
        std::string sh_body = "#!/usr/bin/env bash\nwlr-randr --output \"" + output +
                              "\" --mode \"" + mode + "\" --scale \"" + scale + "\"\n";
        write_text_file(script_path.string(), sh_body);
        chmod(script_path.c_str(), 0755);
    }

    std::ostringstream js;
    js << "window.__spinelDisplayAction && window.__spinelDisplayAction("
       << (rc == 0 ? "true" : "false") << ",\""
       << json_escape(rc == 0 ? "显示设置已应用并保存" : out) << "\");";
    eval_js(state->settings_view, js.str());
    push_display_state(state);
}

namespace {

std::string read_tail_lines(const fs::path &p, size_t max_lines) {
    std::ifstream ifs(p);
    if (!ifs.is_open()) return "";
    std::vector<std::string> ring;
    std::string line;
    while (std::getline(ifs, line)) {
        ring.push_back(std::move(line));
        if (ring.size() > max_lines) {
            ring.erase(ring.begin());
        }
    }
    std::ostringstream oss;
    for (size_t i = 0; i < ring.size(); ++i) {
        if (i > 0) oss << '\n';
        oss << ring[i];
    }
    return oss.str();
}

std::string collect_logs_bundle_text(const std::string &source) {
    if (source == "shell") {
        std::string txt = read_tail_lines("/tmp/spinel-shell.log", 260);
        if (txt.empty()) {
            txt = "暂无 /tmp/spinel-shell.log 记录（请确认从桌面自启或 dev-deploy 启动）。";
        }
        return "=== /tmp/spinel-shell.log ===\n" + txt;
    }

    if (source == "compositor") {
        std::string txt = read_tail_lines("/tmp/spinel-labwc.log", 220);
        if (txt.empty()) {
            txt = "暂无 /tmp/spinel-labwc.log 记录。";
        }
        return "=== /tmp/spinel-labwc.log ===\n" + txt;
    }

    if (source == "system") {
        std::ostringstream oss;
        oss << "=== 运行环境与图形诊断 ===\n";
        oss << "User: " << run_cmd_capture("id 2>/dev/null") << "\n";
        oss << "Kernel: " << read_first_line("/proc/sys/kernel/osrelease", "unknown") << "\n";
        oss << "Session: WAYLAND_DISPLAY=" << (std::getenv("WAYLAND_DISPLAY") ? std::getenv("WAYLAND_DISPLAY") : "")
            << " | WLR_RENDERER=" << (std::getenv("WLR_RENDERER") ? std::getenv("WLR_RENDERER") : "auto")
            << " | GSK_RENDERER=" << (std::getenv("GSK_RENDERER") ? std::getenv("GSK_RENDERER") : "auto") << "\n";
        oss << "WebKit: DMABUF_DISABLED=" << (std::getenv("WEBKIT_DISABLE_DMABUF_RENDERER") ? std::getenv("WEBKIT_DISABLE_DMABUF_RENDERER") : "0")
            << " | COMPOSITING_DISABLED=" << (std::getenv("WEBKIT_DISABLE_COMPOSITING_MODE") ? std::getenv("WEBKIT_DISABLE_COMPOSITING_MODE") : "0") << "\n";
        oss << "GPU:\n" << run_cmd_capture("lspci -k 2>/dev/null | grep -EA3 'VGA|3D|Display' || true") << "\n";
        oss << "DRM:\n" << run_cmd_capture("ls -l /dev/dri 2>/dev/null || true") << "\n\n";
        oss << "=== journalctl -b (最近 100 条) ===\n";
        std::string j = run_cmd_capture("journalctl -b -n 100 --no-pager 2>&1 || dmesg 2>&1 | tail -n 80");
        oss << (j.empty() ? "(无可用系统日志)" : j);
        return oss.str();
    }

    fs::path app_log = fs::path(spinel_state_dir()) / "apps.log";
    std::string txt = read_tail_lines(app_log, 240);
    if (txt.empty()) {
        txt = "暂无应用启动日志。\n点击桌面或游戏中心中的 Steam、主机大屏、终端或应用商店软件后，输出与报错会实时记录在:\n" + app_log.string();
    }
    std::ostringstream out;
    out << "=== " << app_log.string() << " ===\n" << txt;

    fs::path steam_console = fs::path(home_dir()) / ".local/share/Steam/logs/console-linux.txt";
    std::string st = read_tail_lines(steam_console, 45);
    if (!st.empty()) {
        out << "\n\n=== Steam console-linux.txt (最近 45 行) ===\n" << st;
    }
    fs::path steam_boot = fs::path(home_dir()) / ".local/share/Steam/logs/bootstrap_log.txt";
    std::string sb = read_tail_lines(steam_boot, 20);
    if (!sb.empty()) {
        out << "\n\n=== Steam bootstrap_log.txt (最近 20 行) ===\n" << sb;
    }
    return out.str();
}

struct NativeLogWindowCtx {
    AppState *state = nullptr;
    GtkWindow *win = nullptr;
    GtkTextView *text_view = nullptr;
    std::string current_source = "apps";
    std::string banner;
};

void refresh_native_log_view(NativeLogWindowCtx *ctx) {
    if (!ctx || !ctx->text_view) return;
    std::string content;
    if (!ctx->banner.empty()) {
        content = "[提示] " + ctx->banner + "\n\n";
    }
    content += collect_logs_bundle_text(ctx->current_source);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(ctx->text_view);
    gtk_text_buffer_set_text(buf, content.c_str(), -1);
}

void on_native_log_tab_clicked(GtkButton *btn, gpointer user_data) {
    auto *ctx = static_cast<NativeLogWindowCtx *>(user_data);
    const char *src = static_cast<const char *>(g_object_get_data(G_OBJECT(btn), "log-src"));
    if (ctx && src) {
        ctx->current_source = src;
        refresh_native_log_view(ctx);
    }
}

void on_native_log_refresh_clicked(GtkButton *, gpointer user_data) {
    refresh_native_log_view(static_cast<NativeLogWindowCtx *>(user_data));
}

void on_native_log_copy_clicked(GtkButton *btn, gpointer user_data) {
    auto *ctx = static_cast<NativeLogWindowCtx *>(user_data);
    if (!ctx || !ctx->text_view) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(ctx->text_view);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    g_autofree char *txt = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    if (txt) {
        GdkDisplay *display = gdk_display_get_default();
        if (display) {
            gdk_clipboard_set_text(gdk_display_get_clipboard(display), txt);
            gtk_button_set_label(btn, "已复制!");
        }
    }
}

}  // namespace

void push_system_logs(AppState *state, const std::string &source) {
    if (!state || !state->settings_view) return;
    std::string src = (source == "shell" || source == "compositor" || source == "system") ? source : "apps";
    std::thread([state, src]() {
        std::string txt = collect_logs_bundle_text(src);
        std::ostringstream js;
        js << "window.__spinelLogsData && window.__spinelLogsData({"
           << "\"source\":\"" << src << "\","
           << "\"text\":\"" << json_escape(txt) << "\""
           << "});";
        post_js_to_main(state, js.str(), false);
    }).detach();
}

void clear_system_logs(AppState *state, const std::string &source) {
    if (!state) return;
    fs::path app_log = fs::path(spinel_state_dir()) / "apps.log";
    std::error_code ec;
    fs::resize_file(app_log, 0, ec);
    append_app_log("[Logs] 已清空应用启动日志");
    push_system_logs(state, source);
}

void open_native_log_window(AppState *state, const std::string &banner) {
    (void)state;
    auto *ctx = new NativeLogWindowCtx{state, nullptr, nullptr, "apps", banner};

    GtkWidget *win = gtk_window_new();
    ctx->win = GTK_WINDOW(win);
    gtk_window_set_title(ctx->win, "SpinelOS 系统日志与诊断 (原生排障视图)");
    gtk_window_set_default_size(ctx->win, 880, 580);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    struct SrcBtn { const char *label; const char *src; };
    const SrcBtn btns[] = {
        {"应用 / Steam", "apps"},
        {"桌面壳 (spinel-shell)", "shell"},
        {"合成器 (labwc)", "compositor"},
        {"系统与环境", "system"}
    };
    for (const auto &b : btns) {
        GtkWidget *w = gtk_button_new_with_label(b.label);
        g_object_set_data_full(G_OBJECT(w), "log-src", g_strdup(b.src), g_free);
        g_signal_connect(w, "clicked", G_CALLBACK(on_native_log_tab_clicked), ctx);
        gtk_box_append(GTK_BOX(bar), w);
    }

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer);

    GtkWidget *refresh_btn = gtk_button_new_with_label("刷新");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_native_log_refresh_clicked), ctx);
    gtk_box_append(GTK_BOX(bar), refresh_btn);

    GtkWidget *copy_btn = gtk_button_new_with_label("复制日志");
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_native_log_copy_clicked), ctx);
    gtk_box_append(GTK_BOX(bar), copy_btn);

    gtk_box_append(GTK_BOX(root), bar);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);

    GtkWidget *tv = gtk_text_view_new();
    ctx->text_view = GTK_TEXT_VIEW(tv);
    gtk_text_view_set_editable(ctx->text_view, FALSE);
    gtk_text_view_set_monospace(ctx->text_view, TRUE);
    gtk_text_view_set_wrap_mode(ctx->text_view, GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), tv);
    gtk_box_append(GTK_BOX(root), scroll);

    gtk_window_set_child(ctx->win, root);
    g_signal_connect_swapped(win, "destroy", G_CALLBACK(+[](gpointer data) {
        delete static_cast<NativeLogWindowCtx *>(data);
    }), ctx);

    refresh_native_log_view(ctx);
    gtk_window_present(ctx->win);
}

namespace {

void write_fcitx5_classicui(bool is_light, bool force_overwrite) {
    fs::path conf_dir = fs::path(home_dir()) / ".config/fcitx5/conf";
    std::error_code ec;
    fs::create_directories(conf_dir, ec);
    fs::path cui = conf_dir / "classicui.conf";
    std::string theme_name = is_light ? "SpinelLight" : "SpinelDark";
    if (!fs::exists(cui, ec) || force_overwrite) {
        std::ofstream ofs(cui, std::ios::trunc);
        if (ofs.is_open()) {
            ofs << "Vertical Candidate List=False\n"
                << "PerScreenDPI=True\n"
                << "Font=\"Noto Sans CJK SC 11\"\n"
                << "Theme=" << theme_name << "\n";
        }
        return;
    }
    std::ifstream ifs(cui);
    if (!ifs.is_open()) return;
    std::ostringstream out;
    std::string line;
    bool replaced = false;
    while (std::getline(ifs, line)) {
        if (line.rfind("Theme=", 0) == 0) {
            out << "Theme=" << theme_name << "\n";
            replaced = true;
        } else {
            out << line << "\n";
        }
    }
    ifs.close();
    if (!replaced) out << "Theme=" << theme_name << "\n";
    std::ofstream ofs(cui, std::ios::trunc);
    if (ofs.is_open()) ofs << out.str();
}

void stop_fcitx5_sync() {
    if (!has_executable("pkill")) return;
    std::string out;
    int rc = 0;
    run_argv_capture({"pkill", "-x", "fcitx5"}, out, rc);
    for (int i = 0; i < 15; ++i) {
        std::string pg_out;
        int pg_rc = 1;
        if (!run_argv_capture({"pgrep", "-x", "fcitx5"}, pg_out, pg_rc) || pg_rc != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
}

void write_fcitx5_profile(const std::string &chinese_engine) {
    fs::path prof = fs::path(home_dir()) / ".config/fcitx5/profile";
    std::error_code ec;
    fs::create_directories(prof.parent_path(), ec);
    std::string engine = (chinese_engine == "rime") ? "rime" : "pinyin";
    std::ofstream ofs(prof, std::ios::trunc);
    if (ofs.is_open()) {
        ofs << "[Groups/0]\n"
            << "Name=Default\n"
            << "Default Layout=us\n"
            << "DefaultIM=" << engine << "\n\n"
            << "[Groups/0/Items/0]\n"
            << "Name=keyboard-us\n"
            << "Layout=\n\n"
            << "[Groups/0/Items/1]\n"
            << "Name=" << engine << "\n"
            << "Layout=\n\n"
            << "[GroupOrder]\n"
            << "0=Default\n";
    }
}

void ensure_chromium_ime_flags() {
    std::error_code ec;
    fs::path cfg_dir = fs::path(home_dir()) / ".config";
    fs::create_directories(cfg_dir, ec);
    fs::path flags = cfg_dir / "chromium-flags.conf";
    if (!fs::exists(flags, ec)) {
        std::ofstream ofs(flags, std::ios::trunc);
        if (ofs.is_open()) {
            ofs << "--ozone-platform=wayland\n"
                << "--enable-wayland-ime\n"
                << "--wayland-text-input-version=3\n";
        }
    }
}

void write_rime_default_custom(bool prefer_rime_ice) {
    fs::path rime_dir = fs::path(home_dir()) / ".local/share/fcitx5/rime";
    std::error_code ec;
    fs::create_directories(rime_dir, ec);
    fs::path custom = rime_dir / "default.custom.yaml";
    if (!prefer_rime_ice && fs::exists(custom, ec)) return;
    std::ofstream ofs(custom, std::ios::trunc);
    if (ofs.is_open()) {
        ofs << "patch:\n";
        if (prefer_rime_ice) {
            ofs << "  schema_list:\n"
                << "    - schema: rime_ice\n";
        } else {
            ofs << "  schema_list:\n"
                << "    - schema: pinyin_simp\n"
                << "    - schema: luna_pinyin_simp\n";
        }
        ofs << "  \"menu/page_size\": 7\n";
    }
}

std::string detect_active_ime_engine() {
    fs::path prof = fs::path(home_dir()) / ".config/fcitx5/profile";
    std::ifstream ifs(prof);
    if (!ifs.is_open()) return "none";
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (content.find("Name=rime") != std::string::npos || content.find("DefaultIM=rime") != std::string::npos) {
        return "rime";
    }
    if (content.find("Name=pinyin") != std::string::npos || content.find("DefaultIM=pinyin") != std::string::npos) {
        return "pinyin";
    }
    return "none";
}

}  // namespace

void ensure_fcitx5_defaults(bool is_light) {
    std::error_code ec;
    fs::path fdir = fs::path(home_dir()) / ".config/fcitx5";
    fs::create_directories(fdir / "conf", ec);

    ensure_chromium_ime_flags();

    std::string active_im = detect_active_ime_engine();
    if (active_im == "none") {
        // Stop running fcitx5 before writing profile so shutdown doesn't overwrite it with keyboard-us only
        std::string pg_out;
        int pg_rc = 1;
        bool was_running = run_argv_capture({"pgrep", "-x", "fcitx5"}, pg_out, pg_rc) && pg_rc == 0;
        if (was_running) stop_fcitx5_sync();
        write_fcitx5_profile("pinyin");
        if (was_running && has_executable("fcitx5")) {
            spawn_argv_detached({"sh", "-c", "nohup fcitx5 -d >/tmp/spinel-fcitx5.log 2>&1 </dev/null &"});
        }
    }
    if (!fs::exists(fdir / "conf/classicui.conf", ec)) {
        write_fcitx5_classicui(is_light, true);
    }
    fs::path pyconf = fdir / "conf/pinyin.conf";
    if (!fs::exists(pyconf, ec)) {
        std::ofstream ofs(pyconf);
        if (ofs.is_open()) {
            ofs << "PageSize=7\n"
                << "CloudPinyinEnabled=False\n";
        }
    }
    bool has_ice = fs::exists(fs::path(home_dir()) / ".local/share/fcitx5/rime/rime_ice.schema.yaml", ec);
    write_rime_default_custom(has_ice);
}

void sync_fcitx5_theme(bool is_light) {
    write_fcitx5_classicui(is_light, false);
    if (has_executable("fcitx5-remote")) {
        spawn_argv_detached({"fcitx5-remote", "-r"});
    }
}

void sync_terminal_theme(bool is_light) {
    if (!has_executable("gsettings")) return;
    std::string scheme = is_light ? "prefer-light" : "prefer-dark";
    std::string style = is_light ? "light" : "dark";
    std::string script =
        "gsettings set org.gnome.desktop.interface color-scheme '" + scheme + "' 2>/dev/null || true; "
        "gsettings set org.gnome.Ptyxis interface-style '" + style + "' 2>/dev/null || true";
    spawn_argv_detached({"sh", "-c", script});
}

void ensure_terminal_defaults(bool is_light) {
    sync_terminal_theme(is_light);
    if (!has_executable("ptyxis") || !has_executable("gsettings")) return;
    std::error_code ec;
    fs::path stamp = fs::path(home_dir()) / ".config/spinelos/ptyxis-v1.stamp";
    if (fs::exists(stamp, ec)) return;

    const char *script = R"SH(
if gsettings list-schemas 2>/dev/null | grep -qx 'org.gnome.Ptyxis.Shortcuts'; then
  CUR="$(gsettings get org.gnome.Ptyxis.Shortcuts new-tab 2>/dev/null || true)"
  case "$CUR" in
    \[*) gsettings set org.gnome.Ptyxis.Shortcuts new-tab "['<Control>t']" 2>/dev/null || true ;;
    *)   gsettings set org.gnome.Ptyxis.Shortcuts new-tab '<Control>t' 2>/dev/null || true ;;
  esac
  CUR_FONT="$(gsettings get org.gnome.Ptyxis font-name 2>/dev/null || true)"
  if [ -n "$CUR_FONT" ]; then
    gsettings set org.gnome.Ptyxis use-system-font false 2>/dev/null || true
    gsettings set org.gnome.Ptyxis font-name 'Noto Sans Mono CJK SC 11' 2>/dev/null || true
  fi
fi
)SH";
    spawn_argv_detached({"sh", "-c", script});
    write_text_file(stamp.string(), "ok\n");
}

void push_ime_state(AppState *state) {
    if (!state || !state->settings_view) return;
    bool installed = has_executable("fcitx5");
    bool rime_ok = has_executable("fcitx5-rime") ||
                   fs::exists("/usr/lib/fcitx5/librime.so") ||
                   fs::exists("/usr/share/fcitx5/addon/rime.conf");
    std::error_code ec;
    bool rime_ice = fs::exists(fs::path(home_dir()) / ".local/share/fcitx5/rime/rime_ice.schema.yaml", ec);
    std::string pid_out;
    int pid_rc = 1;
    bool running = installed && run_argv_capture({"pgrep", "-x", "fcitx5"}, pid_out, pid_rc) && pid_rc == 0;
    std::string engine = detect_active_ime_engine();

    std::ostringstream js;
    js << "window.__spinelImeState && window.__spinelImeState({"
       << "\"installed\":" << (installed ? "true" : "false") << ","
       << "\"running\":" << (running ? "true" : "false") << ","
       << "\"rime_installed\":" << (rime_ok ? "true" : "false") << ","
       << "\"rime_ice\":" << (rime_ice ? "true" : "false") << ","
       << "\"engine\":\"" << engine << "\""
       << "});";
    eval_js(state->settings_view, js.str());
}

void restart_fcitx5_daemon(AppState *state) {
    if (!has_executable("fcitx5")) {
        if (state && state->settings_view) {
            eval_js(state->settings_view,
                    "window.__spinelImeFeedback && window.__spinelImeFeedback(false, \"尚未安装 Fcitx5，请先运行 ./scripts/dev-deploy.sh --full 安装输入法组件。\");");
        }
        return;
    }
    stop_fcitx5_sync();
    if (detect_active_ime_engine() == "none") {
        write_fcitx5_profile("pinyin");
    }
    spawn_argv_detached({"sh", "-c", "nohup fcitx5 -d >/tmp/spinel-fcitx5.log 2>&1 </dev/null &"});
    append_app_log("[IME] 已重启 Fcitx5 输入法服务");
    if (state) {
        std::thread([state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            post_js_to_main(state, "window.__spinelImeFeedback && window.__spinelImeFeedback(true, \"Fcitx5 输入法服务已重启，按 Ctrl+Space 或 Shift 即可切换中英文。\");", false);
        }).detach();
    }
}

void set_ime_engine(AppState *state, const std::string &engine) {
    std::string target = (engine == "rime") ? "rime" : "pinyin";
    // Stop fcitx5 before writing profile so exit handler doesn't clobber the new profile
    stop_fcitx5_sync();
    write_fcitx5_profile(target);
    append_app_log("[IME] 切换中文输入法方案为: " + target);
    restart_fcitx5_daemon(state);
    if (state && state->settings_view) {
        push_ime_state(state);
    }
}

void install_rime_ice_dict(AppState *state) {
    if (!state) return;
    post_js_to_main(state, "window.__spinelImeFeedback && window.__spinelImeFeedback(true, \"正在下载雾凇拼音 (rime-ice) 词库并切换为 Rime，请稍候...\");", false);
    std::thread([state]() {
        fs::path rime_dir = fs::path(home_dir()) / ".local/share/fcitx5/rime";
        std::error_code ec;
        fs::create_directories(rime_dir, ec);
        std::string cmd =
            "tmp=$(mktemp -d); "
            "if curl -fsSL --connect-timeout 15 --max-time 180 https://github.com/iDvel/rime-ice/archive/refs/heads/main.tar.gz -o \"$tmp/ice.tar.gz\" && "
            "tar -xzf \"$tmp/ice.tar.gz\" -C \"$tmp\" && "
            "cp -a \"$tmp/rime-ice-main/.\" \"" + rime_dir.string() + "/\"; then "
            "rm -rf \"$tmp\"; exit 0; else rm -rf \"$tmp\"; exit 1; fi";
        std::string out;
        int rc = 1;
        run_argv_capture({"bash", "-c", cmd}, out, rc);
        if (rc == 0) {
            stop_fcitx5_sync();
            write_rime_default_custom(true);
            write_fcitx5_profile("rime");
            spawn_argv_detached({"sh", "-c", "nohup fcitx5 -d >/tmp/spinel-fcitx5.log 2>&1 </dev/null &"});
            append_app_log("[IME] 雾凇拼音 (rime-ice) 下载安装成功并已激活 Rime");
            post_js_to_main(state, "window.__spinelImeFeedback && window.__spinelImeFeedback(true, \"雾凇拼音 (rime-ice) 安装完成！已自动切换为 Rime 并重启输入法，按 Ctrl+Space 即可输入。\");", false);
        } else {
            append_app_log("[IME] 雾凇拼音下载失败 (code=" + std::to_string(rc) + "): " + out);
            post_js_to_main(state, "window.__spinelImeFeedback && window.__spinelImeFeedback(false, \"雾凇拼音下载失败（网络访问 GitHub 超时），可先在「Wi-Fi / 网络」里开启代理后再试，或直接使用内置中文拼音。\");", false);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        g_idle_add([](gpointer d) -> gboolean {
            auto *st = static_cast<AppState *>(d);
            if (!st->shutting_down.load()) push_ime_state(st);
            return G_SOURCE_REMOVE;
        }, state);
    }).detach();
}

namespace {

bool valid_app_id(const std::string &s) {
    if (s.empty() || s.size() > 160) return false;
    for (unsigned char c : s) {
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

std::vector<std::string> split_semi(const std::string &v) {
    std::vector<std::string> out;
    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ';')) {
        item = trim_copy(item);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

bool list_has_token(const std::vector<std::string> &tokens, const std::string &tok) {
    bool has = false;
    for (const auto &t : tokens) {
        if (t == tok) has = true;
        else if (t == ("!" + tok)) has = false;
    }
    return has;
}

struct FlatpakPermEntry {
    std::string id;
    std::string name;
    bool home = false;
    bool host = false;
    bool network = false;
    bool devices = false;
    bool dri = false;
    bool audio = false;
};

FlatpakPermEntry inspect_flatpak_app_perms(const std::string &id, const std::string &name) {
    FlatpakPermEntry e;
    e.id = id;
    e.name = name;
    std::string out;
    int rc = 1;
    if (!run_argv_capture({"flatpak", "info", "--show-permissions", id}, out, rc) || rc != 0) {
        return e;
    }
    std::istringstream iss(out);
    std::string line;
    std::string section;
    std::vector<std::string> fs_toks, sh_toks, dev_toks, sock_toks;
    while (std::getline(iss, line)) {
        line = trim_copy(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line;
            continue;
        }
        if (section != "[Context]") continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        auto toks = split_semi(v);
        if (k == "filesystems") fs_toks.insert(fs_toks.end(), toks.begin(), toks.end());
        else if (k == "shared") sh_toks.insert(sh_toks.end(), toks.begin(), toks.end());
        else if (k == "devices") dev_toks.insert(dev_toks.end(), toks.begin(), toks.end());
        else if (k == "sockets") sock_toks.insert(sock_toks.end(), toks.begin(), toks.end());
    }
    e.host = list_has_token(fs_toks, "host");
    e.home = e.host || list_has_token(fs_toks, "home");
    e.network = list_has_token(sh_toks, "network");
    e.devices = list_has_token(dev_toks, "all");
    e.dri = e.devices || list_has_token(dev_toks, "dri");
    e.audio = list_has_token(sock_toks, "pulseaudio") || list_has_token(sock_toks, "pipewire");
    return e;
}

}  // namespace

void push_perms_state(AppState *state) {
    if (!state || !state->settings_view) return;
    std::string out;
    int rc = 1;
    bool polkit_installed = fs::exists("/usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1");
    bool polkit_running = run_argv_capture({"pgrep", "-f", "polkit-gnome-authentication-agent-1"}, out, rc) && rc == 0;
    bool portal_running = run_argv_capture({"pgrep", "-x", "xdg-desktop-portal"}, out, rc) && rc == 0;
    bool portal_gtk = run_argv_capture({"pgrep", "-x", "xdg-desktop-portal-gtk"}, out, rc) && rc == 0;
    bool portal_wlr = run_argv_capture({"pgrep", "-x", "xdg-desktop-portal-wlr"}, out, rc) && rc == 0;
    std::error_code ec;
    bool portal_conf = fs::exists("/etc/xdg/xdg-desktop-portal/spinelos-portals.conf", ec) ||
                       fs::exists("/etc/xdg/xdg-desktop-portal/portals.conf", ec);

    std::vector<FlatpakPermEntry> apps;
    if (has_executable("flatpak")) {
        std::string list_out;
        int list_rc = 1;
        if (run_argv_capture({"flatpak", "list", "--app", "--columns=application,name"}, list_out, list_rc) && list_rc == 0) {
            std::istringstream iss(list_out);
            std::string line;
            while (std::getline(iss, line)) {
                line = trim_copy(line);
                if (line.empty()) continue;
                auto tab = line.find('\t');
                std::string id = trim_copy(tab == std::string::npos ? line : line.substr(0, tab));
                std::string name = trim_copy(tab == std::string::npos ? line : line.substr(tab + 1));
                if (!valid_app_id(id)) continue;
                apps.push_back(inspect_flatpak_app_perms(id, name.empty() ? id : name));
            }
        }
    }

    struct PortalEntry {
        std::string table;
        std::string id;
        std::string app;
        std::string perms;
    };
    std::vector<PortalEntry> portal_entries;
    if (has_executable("flatpak")) {
        std::string p_out;
        int p_rc = 1;
        if (run_argv_capture({"flatpak", "permission-show"}, p_out, p_rc) && p_rc == 0) {
            std::istringstream iss(p_out);
            std::string line;
            while (std::getline(iss, line)) {
                line = trim_copy(line);
                if (line.empty() || line.rfind("Table", 0) == 0) continue;
                std::istringstream ls(line);
                std::string tbl, pid, papp, rest;
                if (!(ls >> tbl >> pid >> papp)) continue;
                std::getline(ls, rest);
                rest = trim_copy(rest);
                if (valid_app_id(papp) || papp == "all") {
                    portal_entries.push_back({tbl, pid, papp, rest});
                }
            }
        }
    }

    std::ostringstream js;
    js << "window.__spinelPermsData && window.__spinelPermsData({"
       << "\"polkit_installed\":" << (polkit_installed ? "true" : "false") << ","
       << "\"polkit_running\":" << (polkit_running ? "true" : "false") << ","
       << "\"portal_running\":" << (portal_running ? "true" : "false") << ","
       << "\"portal_gtk\":" << (portal_gtk ? "true" : "false") << ","
       << "\"portal_wlr\":" << (portal_wlr ? "true" : "false") << ","
       << "\"portal_conf\":" << (portal_conf ? "true" : "false") << ","
       << "\"apps\":[";
    for (size_t i = 0; i < apps.size(); ++i) {
        if (i > 0) js << ",";
        js << "{"
           << "\"id\":\"" << json_escape(apps[i].id) << "\","
           << "\"name\":\"" << json_escape(apps[i].name) << "\","
           << "\"home\":" << (apps[i].home ? "true" : "false") << ","
           << "\"host\":" << (apps[i].host ? "true" : "false") << ","
           << "\"network\":" << (apps[i].network ? "true" : "false") << ","
           << "\"devices\":" << (apps[i].devices ? "true" : "false") << ","
           << "\"dri\":" << (apps[i].dri ? "true" : "false") << ","
           << "\"audio\":" << (apps[i].audio ? "true" : "false")
           << "}";
    }
    js << "],\"portal_entries\":[";
    for (size_t i = 0; i < portal_entries.size(); ++i) {
        if (i > 0) js << ",";
        js << "{"
           << "\"table\":\"" << json_escape(portal_entries[i].table) << "\","
           << "\"id\":\"" << json_escape(portal_entries[i].id) << "\","
           << "\"app\":\"" << json_escape(portal_entries[i].app) << "\","
           << "\"perms\":\"" << json_escape(portal_entries[i].perms) << "\""
           << "}";
    }
    js << "]});";
    eval_js(state->settings_view, js.str());
}

void apply_flatpak_perm(AppState *state, const std::string &payload) {
    if (!has_executable("flatpak")) return;
    std::string app_id, key, val;
    std::istringstream iss(payload);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim_copy(line);
        if (line.rfind("app=", 0) == 0) app_id = line.substr(4);
        else if (line.rfind("key=", 0) == 0) key = line.substr(4);
        else if (line.rfind("val=", 0) == 0) val = line.substr(4);
    }
    if (!valid_app_id(app_id)) return;
    bool enable = (val == "true" || val == "1");
    std::string flag;
    if (key == "home") flag = enable ? "--filesystem=home" : "--nofilesystem=home";
    else if (key == "host") flag = enable ? "--filesystem=host" : "--nofilesystem=host";
    else if (key == "network") flag = enable ? "--share=network" : "--unshare=network";
    else if (key == "devices") flag = enable ? "--device=all" : "--nodevice=all";
    else if (key == "dri") flag = enable ? "--device=dri" : "--nodevice=dri";
    else if (key == "audio") flag = enable ? "--socket=pulseaudio" : "--nosocket=pulseaudio";
    else return;

    std::string out;
    int rc = 1;
    run_argv_capture({"flatpak", "override", "--user", flag, app_id}, out, rc);
    append_app_log("[Perms] flatpak override " + flag + " " + app_id + (rc == 0 ? " (OK)" : " (ERR)"));
    if (state && state->settings_view) {
        std::string msg = rc == 0 ? ("已更新 " + app_id + " 权限 (" + flag + ")，重启该应用生效。") : ("权限修改失败: " + out);
        eval_js(state->settings_view,
                "window.__spinelPermsFeedback && window.__spinelPermsFeedback(" +
                    std::string(rc == 0 ? "true" : "false") + ",\"" + json_escape(msg) + "\");");
    }
    push_perms_state(state);
}

void reset_flatpak_app_perms(AppState *state, const std::string &app_id) {
    if (!valid_app_id(app_id) || !has_executable("flatpak")) return;
    std::string out;
    int rc = 1;
    run_argv_capture({"flatpak", "override", "--user", "--reset", app_id}, out, rc);
    append_app_log("[Perms] 恢复应用默认权限: " + app_id);
    if (state && state->settings_view) {
        std::string msg = rc == 0 ? ("已将 " + app_id + " 权限恢复为应用默认。") : ("重置失败: " + out);
        eval_js(state->settings_view,
                "window.__spinelPermsFeedback && window.__spinelPermsFeedback(" +
                    std::string(rc == 0 ? "true" : "false") + ",\"" + json_escape(msg) + "\");");
    }
    push_perms_state(state);
}

void reset_portal_perm(AppState *state, const std::string &payload) {
    if (!has_executable("flatpak")) return;
    std::string table, pid, app_id;
    std::istringstream iss(payload);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim_copy(line);
        if (line.rfind("table=", 0) == 0) table = line.substr(6);
        else if (line.rfind("id=", 0) == 0) pid = line.substr(3);
        else if (line.rfind("app=", 0) == 0) app_id = line.substr(4);
    }
    if (table.empty() || pid.empty() || app_id.empty()) return;
    std::string out;
    int rc = 1;
    run_argv_capture({"flatpak", "permission-remove", table, pid, app_id}, out, rc);
    append_app_log("[Perms] 撤销动态授权记录: " + table + " / " + pid + " / " + app_id);
    push_perms_state(state);
}

void restart_polkit_agent(AppState *state) {
    const char *agent = "/usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1";
    if (!fs::exists(agent)) {
        if (state && state->settings_view) {
            eval_js(state->settings_view,
                    "window.__spinelPermsFeedback && window.__spinelPermsFeedback(false, \"尚未安装 polkit-gnome，请先运行 ./scripts/dev-deploy.sh --full 安装权限代理组件。\");");
        }
        return;
    }
    spawn_argv_detached({"sh", "-c", "pkill -f polkit-gnome-authentication-agent-1 2>/dev/null || true; sleep 0.1; nohup /usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1 >/tmp/spinel-polkit.log 2>&1 </dev/null &"});
    append_app_log("[Perms] 已启动 Polkit 管理员授权弹窗代理");
    if (state) {
        std::thread([state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            post_js_to_main(state, "window.__spinelPermsFeedback && window.__spinelPermsFeedback(true, \"Polkit 管理员授权弹窗代理已就绪。\");", false);
            g_idle_add([](gpointer d) -> gboolean {
                auto *st = static_cast<AppState *>(d);
                if (!st->shutting_down.load()) push_perms_state(st);
                return G_SOURCE_REMOVE;
            }, state);
        }).detach();
    }
}

void reload_desktop_portals(AppState *state) {
    spawn_argv_detached({"sh", "-c", "dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP GTK_THEME 2>/dev/null || true; systemctl --user restart xdg-desktop-portal xdg-desktop-portal-gtk xdg-desktop-portal-wlr >/dev/null 2>&1 || true"});
    append_app_log("[Perms] 已重载 xdg-desktop-portal 权限弹窗网关");
    if (state) {
        std::thread([state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            post_js_to_main(state, "window.__spinelPermsFeedback && window.__spinelPermsFeedback(true, \"已重载权限弹窗服务。\");", false);
            g_idle_add([](gpointer d) -> gboolean {
                auto *st = static_cast<AppState *>(d);
                if (!st->shutting_down.load()) push_perms_state(st);
                return G_SOURCE_REMOVE;
            }, state);
        }).detach();
    }
}

namespace {

struct ProcRow {
    int pid = 0;
    std::string name;
    std::string cmd;
    double cpu_pct = 0.0;
    long rss_mb = 0;
    bool is_core = false;
    bool own = false;
};

bool read_total_cpu_jiffies(unsigned long long &total_out, unsigned long long &idle_out) {
    std::ifstream ifs("/proc/stat");
    if (!ifs.is_open()) return false;
    std::string tag;
    unsigned long long u = 0, n = 0, s = 0, id = 0, io = 0, irq = 0, sirq = 0, st = 0;
    if (!(ifs >> tag >> u >> n >> s >> id >> io >> irq >> sirq >> st) || tag != "cpu") return false;
    idle_out = id + io;
    total_out = u + n + s + id + io + irq + sirq + st;
    return true;
}

}  // namespace

void push_process_list(AppState *state) {
    if (!state || !state->settings_view) return;
    static unsigned long long prev_total = 0;
    static unsigned long long prev_idle = 0;
    static std::map<int, unsigned long long> prev_proc_ticks;

    unsigned long long cur_total = 0, cur_idle = 0;
    read_total_cpu_jiffies(cur_total, cur_idle);
    unsigned long long delta_total = (prev_total > 0 && cur_total > prev_total) ? (cur_total - prev_total) : 0;
    unsigned long long delta_idle = (prev_idle > 0 && cur_idle >= prev_idle) ? (cur_idle - prev_idle) : 0;
    double sys_cpu = (delta_total > 0)
        ? std::clamp(100.0 * static_cast<double>(delta_total - delta_idle) / static_cast<double>(delta_total), 0.0, 100.0)
        : 0.0;
    long num_cpus = std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
    long page_kb = std::max(1L, sysconf(_SC_PAGESIZE) / 1024);

    uid_t my_uid = getuid();
    std::map<int, unsigned long long> next_ticks;
    std::vector<ProcRow> rows;
    int total_procs = 0;

    std::error_code ec;
    for (const auto &entry : fs::directory_iterator("/proc", ec)) {
        std::string fname = entry.path().filename().string();
        if (fname.empty() || !std::all_of(fname.begin(), fname.end(), ::isdigit)) continue;
        int pid = std::atoi(fname.c_str());
        if (pid <= 0) continue;
        ++total_procs;

        std::ifstream stat_ifs(entry.path() / "stat");
        if (!stat_ifs.is_open()) continue;
        std::string stat_line;
        std::getline(stat_ifs, stat_line);
        auto lp = stat_line.find('(');
        auto rp = stat_line.rfind(')');
        if (lp == std::string::npos || rp == std::string::npos || rp <= lp) continue;

        std::string comm = stat_line.substr(lp + 1, rp - lp - 1);
        std::istringstream rest(stat_line.substr(rp + 2));
        std::string state_ch;
        long ppid = 0, pgrp = 0, sess = 0, tty = 0, tpgid = 0;
        unsigned long flags = 0, minflt = 0, cminflt = 0, majflt = 0, cmajflt = 0;
        unsigned long long utime = 0, stime = 0;
        long cutime = 0, cstime = 0, prio = 0, nicev = 0, nthreads = 0, itreal = 0;
        unsigned long long start = 0, vsize = 0;
        long rss_pages = 0;
        rest >> state_ch >> ppid >> pgrp >> sess >> tty >> tpgid >> flags >> minflt >> cminflt
             >> majflt >> cmajflt >> utime >> stime >> cutime >> cstime >> prio >> nicev
             >> nthreads >> itreal >> start >> vsize >> rss_pages;

        unsigned long long p_ticks = utime + stime;
        next_ticks[pid] = p_ticks;
        double cpu_pct = 0.0;
        if (delta_total > 0) {
            auto it = prev_proc_ticks.find(pid);
            if (it != prev_proc_ticks.end() && p_ticks >= it->second) {
                cpu_pct = 100.0 * static_cast<double>(p_ticks - it->second) * static_cast<double>(num_cpus) /
                          static_cast<double>(delta_total);
            }
        }
        long rss_mb = (rss_pages * page_kb) / 1024;
        if (rss_mb < 2 && cpu_pct < 0.2) continue;  // Skip tiny idle kernel/system threads for clean UX

        struct stat st_buf{};
        bool own = (::stat(entry.path().c_str(), &st_buf) == 0 && st_buf.st_uid == my_uid);

        std::string cmdline;
        std::ifstream cmd_ifs(entry.path() / "cmdline", std::ios::binary);
        if (cmd_ifs.is_open()) {
            std::string raw((std::istreambuf_iterator<char>(cmd_ifs)), std::istreambuf_iterator<char>());
            for (char &c : raw) if (c == '\0') c = ' ';
            cmdline = trim_copy(raw);
            if (cmdline.size() > 96) cmdline = cmdline.substr(0, 93) + "...";
        }
        if (cmdline.empty()) cmdline = comm;

        bool is_core = (pid <= 2 || comm == "labwc" || comm == "spinel-shell" || comm == "systemd" ||
                        comm == "dbus-daemon" || comm == "dbus-broker");
        rows.push_back({pid, comm, cmdline, cpu_pct, rss_mb, is_core, own});
    }

    prev_total = cur_total;
    prev_idle = cur_idle;
    prev_proc_ticks = std::move(next_ticks);

    std::sort(rows.begin(), rows.end(), [](const ProcRow &a, const ProcRow &b) {
        if (std::abs(a.cpu_pct - b.cpu_pct) > 0.4) return a.cpu_pct > b.cpu_pct;
        return a.rss_mb > b.rss_mb;
    });
    if (rows.size() > 60) rows.resize(60);

    int mem_used = 0;
    std::string mem_str = detect_mem_usage(&mem_used);

    std::ostringstream js;
    js << "window.__spinelProcessList && window.__spinelProcessList({"
       << "\"cpu_pct\":" << static_cast<int>(sys_cpu + 0.5) << ","
       << "\"mem\":\"" << json_escape(mem_str) << "\","
       << "\"total_procs\":" << total_procs << ","
       << "\"procs\":[";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) js << ",";
        char cbuf[16];
        std::snprintf(cbuf, sizeof(cbuf), "%.1f", rows[i].cpu_pct);
        js << "{"
           << "\"pid\":" << rows[i].pid << ","
           << "\"name\":\"" << json_escape(rows[i].name) << "\","
           << "\"cmd\":\"" << json_escape(rows[i].cmd) << "\","
           << "\"cpu\":" << cbuf << ","
           << "\"mem_mb\":" << rows[i].rss_mb << ","
           << "\"is_core\":" << (rows[i].is_core ? "true" : "false") << ","
           << "\"own\":" << (rows[i].own ? "true" : "false")
           << "}";
    }
    js << "]});";
    eval_js(state->settings_view, js.str());
}

void kill_process_by_pid(AppState *state, int pid, bool force) {
    if (pid <= 1 || pid == getpid()) {
        if (state && state->settings_view) {
            eval_js(state->settings_view,
                    "window.__spinelProcsFeedback && window.__spinelProcsFeedback(false, \"无法结束系统核心自身进程。\");");
        }
        return;
    }
    int sig = force ? SIGKILL : SIGTERM;
    int rc = ::kill(static_cast<pid_t>(pid), sig);
    bool ok = (rc == 0);
    std::string msg = ok
        ? ("已向进程 PID " + std::to_string(pid) + (force ? " 发送强制结束 (SIGKILL)" : " 发送结束信号 (SIGTERM)"))
        : ("无法结束 PID " + std::to_string(pid) + "（可能需要管理员权限或进程已退出）");
    append_app_log("[Proc] " + msg);
    if (state && state->settings_view) {
        eval_js(state->settings_view,
                "window.__spinelProcsFeedback && window.__spinelProcsFeedback(" +
                    std::string(ok ? "true" : "false") + ",\"" + json_escape(msg) + "\");");
    }
    if (state) {
        std::thread([state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(180));
            g_idle_add([](gpointer d) -> gboolean {
                auto *st = static_cast<AppState *>(d);
                if (!st->shutting_down.load()) push_process_list(st);
                return G_SOURCE_REMOVE;
            }, state);
        }).detach();
    }
}

void sync_notification_theme(bool is_light) {
    fs::path mako_cfg = fs::path(home_dir()) / ".config/mako/config";
    std::ostringstream oss;
    oss << "# SpinelOS Wayland Notification Daemon (mako)\n"
        << "font=Noto Sans CJK SC 11\n"
        << "border-radius=12\n"
        << "border-size=1\n"
        << "padding=12,14\n"
        << "margin=12,12,38,12\n"
        << "width=340\n"
        << "default-timeout=5000\n"
        << "layer=overlay\n"
        << "anchor=top-right\n";
    if (is_light) {
        oss << "background-color=#fffffff0\n"
            << "text-color=#0f172a\n"
            << "border-color=#0f172a24\n"
            << "progress-color=over #0284c733\n";
    } else {
        oss << "background-color=#111827f0\n"
            << "text-color=#f8fafc\n"
            << "border-color=#ffffff2e\n"
            << "progress-color=over #38bdf833\n";
    }
    if (write_text_file(mako_cfg.string(), oss.str()) && has_executable("makoctl")) {
        spawn_argv_detached({"makoctl", "reload"});
    }
}

namespace {

struct AudioDeviceItem {
    int id = 0;
    std::string name;
    bool is_default = false;
};

void parse_wpctl_audio(std::vector<AudioDeviceItem> &sinks, std::vector<AudioDeviceItem> &sources) {
    std::string out;
    int rc = 1;
    if (!run_argv_capture({"wpctl", "status"}, out, rc) || rc != 0) return;

    enum Sec { None, Sinks, Sources } sec = None;
    bool in_audio = false;
    for (const auto &raw : split_lines(out)) {
        if (raw.find("Audio") != std::string::npos && raw.rfind("Audio", 0) == 0) {
            in_audio = true;
            sec = None;
            continue;
        }
        if (raw.find("Video") != std::string::npos && raw.rfind("Video", 0) == 0) {
            break;
        }
        if (!in_audio) continue;

        if (raw.find("Sinks:") != std::string::npos) { sec = Sinks; continue; }
        if (raw.find("Sources:") != std::string::npos) { sec = Sources; continue; }
        if (raw.find("Filters:") != std::string::npos || raw.find("Streams:") != std::string::npos ||
            raw.find("Devices:") != std::string::npos) {
            sec = None;
            continue;
        }
        if (sec == None) continue;

        auto dot = raw.find('.');
        if (dot == std::string::npos || dot == 0) continue;
        size_t i = dot;
        while (i > 0 && std::isdigit(static_cast<unsigned char>(raw[i - 1]))) --i;
        if (i == dot) continue;

        int id = std::atoi(raw.substr(i, dot - i).c_str());
        if (id <= 0) continue;

        bool is_def = (raw.substr(0, i).find('*') != std::string::npos);
        std::string rest = raw.substr(dot + 1);
        auto br = rest.rfind('[');
        if (br != std::string::npos) rest = rest.substr(0, br);
        rest = trim_copy(rest);
        if (rest.empty()) continue;

        if (sec == Sinks) {
            sinks.push_back({id, rest, is_def});
        } else if (sec == Sources) {
            if (rest.find("Monitor of ") != std::string::npos) continue;
            sources.push_back({id, rest, is_def});
        }
    }
}

void write_audio_items_json(std::ostringstream &js, const std::vector<AudioDeviceItem> &items) {
    js << "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) js << ",";
        js << "{\"id\":" << items[i].id
           << ",\"name\":\"" << json_escape(items[i].name) << "\""
           << ",\"is_default\":" << (items[i].is_default ? "true" : "false") << "}";
    }
    js << "]";
}

std::atomic<bool> g_gamepad_stop{false};
std::thread g_gamepad_thread;

struct GamepadCmdIdle {
    AppState *state;
    std::string cmd;
};

void dispatch_gamepad_cmd(AppState *state, const std::string &cmd) {
    if (!state || state->shutting_down.load()) return;
    g_idle_add([](gpointer data) -> gboolean {
        std::unique_ptr<GamepadCmdIdle> call(static_cast<GamepadCmdIdle *>(data));
        if (!call->state->shutting_down.load()) {
            handle_shell_command(call->state, call->cmd);
        }
        return G_SOURCE_REMOVE;
    }, new GamepadCmdIdle{state, cmd});
}

}  // namespace

void push_audio_power_state(AppState *state) {
    if (!state || !state->settings_view) return;
    MediaSnapshot media = collect_media_state();

    std::vector<AudioDeviceItem> sinks, sources;
    if (has_executable("wpctl")) {
        parse_wpctl_audio(sinks, sources);
    }

    int mic_vol = 100;
    bool mic_muted = false;
    std::string mic_out = run_cmd_capture("wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>/dev/null");
    auto pos = mic_out.find("Volume:");
    if (pos != std::string::npos) {
        double v = std::atof(mic_out.c_str() + pos + 7);
        mic_vol = std::clamp(static_cast<int>(v * 100.0 + 0.5), 0, 100);
    }
    mic_muted = (mic_out.find("MUTED") != std::string::npos);

    bool zram_active = !trim_copy(run_cmd_capture("swapon --noheadings --show=NAME 2>/dev/null")).empty();
    bool ppd_available = !media.power_profile.empty();

    std::ostringstream js;
    js << "window.__spinelAudioPowerData && window.__spinelAudioPowerData({"
       << "\"volume\":" << media.volume << ","
       << "\"brightness\":" << media.brightness << ","
       << "\"mic_volume\":" << mic_vol << ","
       << "\"mic_muted\":" << (mic_muted ? "true" : "false") << ","
       << "\"battery_pct\":" << media.battery_pct << ","
       << "\"battery_charging\":" << (media.battery_charging ? "true" : "false") << ","
       << "\"power_profile\":\"" << json_escape(media.power_profile) << "\","
       << "\"ppd_available\":" << (ppd_available ? "true" : "false") << ","
       << "\"zram_active\":" << (zram_active ? "true" : "false") << ","
       << "\"sinks\":";
    write_audio_items_json(js, sinks);
    js << ",\"sources\":";
    write_audio_items_json(js, sources);
    js << "});";
    eval_js(state->settings_view, js.str());
}

void set_audio_default_node(AppState *state, int id, bool is_sink) {
    if (id <= 0 || !has_executable("wpctl")) return;
    std::string out;
    int rc = 1;
    run_argv_capture({"wpctl", "set-default", std::to_string(id)}, out, rc);
    append_app_log(std::string("[Audio] 切换默认") + (is_sink ? "输出设备" : "麦克风") + " ID=" + std::to_string(id));
    push_audio_power_state(state);
}

void set_mic_volume(AppState *state, int pct) {
    if (!has_executable("wpctl")) return;
    int clamped = std::clamp(pct, 0, 100);
    spawn_argv_detached({"wpctl", "set-volume", "@DEFAULT_AUDIO_SOURCE@", std::to_string(clamped) + "%"});
    (void)state;
}

void toggle_mic_mute(AppState *state) {
    if (!has_executable("wpctl")) return;
    std::string out;
    int rc = 1;
    run_argv_capture({"wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", "toggle"}, out, rc);
    push_audio_power_state(state);
}

void set_power_profile(AppState *state, const std::string &profile) {
    if (profile != "performance" && profile != "balanced" && profile != "power-saver") return;
    if (!has_executable("powerprofilesctl")) return;
    std::string out;
    int rc = 1;
    bool ok = run_argv_capture({"powerprofilesctl", "set", profile}, out, rc) && rc == 0;
    append_app_log("[Power] 切换性能电源模式 -> " + profile + (ok ? " (成功)" : " (失败: " + out + ")"));
    push_audio_power_state(state);
    native_shell_sync_telemetry(state);
}

void start_gamepad_hotkey_listener(AppState *state) {
    if (!state || g_gamepad_thread.joinable()) return;
    g_gamepad_stop.store(false);
    g_gamepad_thread = std::thread([state]() {
        int fd = -1;
        bool btn_state[32]{};
        auto last_trigger = std::chrono::steady_clock::now() - std::chrono::seconds(5);

        while (!g_gamepad_stop.load()) {
            if (fd < 0) {
                std::error_code ec;
                for (int idx = 0; idx < 4; ++idx) {
                    std::string dev = "/dev/input/js" + std::to_string(idx);
                    if (!fs::exists(dev, ec)) continue;
                    fd = ::open(dev.c_str(), O_RDONLY | O_NONBLOCK);
                    if (fd >= 0) {
                        std::fill(std::begin(btn_state), std::end(btn_state), false);
                        break;
                    }
                }
                if (fd < 0) {
                    for (int s = 0; s < 12 && !g_gamepad_stop.load(); ++s) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    }
                    continue;
                }
            }

            struct pollfd pfd{fd, POLLIN, 0};
            int pr = ::poll(&pfd, 1, 250);
            if (pr < 0) {
                ::close(fd);
                fd = -1;
                continue;
            }
            if (pr == 0) continue;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                ::close(fd);
                fd = -1;
                continue;
            }

            struct js_event ev{};
            ssize_t n = 0;
            while ((n = ::read(fd, &ev, sizeof(ev))) == sizeof(ev)) {
                if ((ev.type & ~JS_EVENT_INIT) != JS_EVENT_BUTTON) continue;
                if (ev.number >= 32) continue;
                bool pressed = (ev.value != 0);
                btn_state[ev.number] = pressed;
                if (!pressed) continue;

                auto now = std::chrono::steady_clock::now();
                if (now - last_trigger < std::chrono::milliseconds(1200)) continue;

                // Standard Xbox/PlayStation/Generic Linux joystick Guide button (8 or 10) -> Game Center
                if (ev.number == 8 || ev.number == 10) {
                    last_trigger = now;
                    dispatch_gamepad_cmd(state, "games:open");
                } else if ((btn_state[6] && btn_state[7]) || (btn_state[8] && btn_state[9])) {
                    last_trigger = now;
                    dispatch_gamepad_cmd(state, "cc:toggle");
                }
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                ::close(fd);
                fd = -1;
            }
        }
        if (fd >= 0) ::close(fd);
    });
}

void stop_gamepad_hotkey_listener() {
    g_gamepad_stop.store(true);
    if (g_gamepad_thread.joinable()) {
        g_gamepad_thread.join();
    }
}
