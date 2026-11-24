#include "system_control.hpp"
#include "common.hpp"
#include "native_shell.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
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
    std::string net = run_cmd_capture(
        "nmcli -t -f DEVICE,TYPE,STATE dev status 2>/dev/null | awk -F: '$3==\"connected\" {print $1\" (\"$2\")\"; exit}'"
    );
    return net.empty() ? "Offline" : net;
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
    eval_js(state->desktop_view, script);
    eval_js(state->bar_view, script);
    eval_js(state->cc_view, script);
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

    std::error_code ec;
    fs::create_directories(config_dir(), ec);
    std::ofstream ofs(config_dir() / "perf.conf");
    if (ofs.is_open()) {
        ofs << "mangohud=" << cfg.mangohud << "\n"
            << "fps_limit=" << cfg.fps_limit << "\n"
            << "fsr=" << (cfg.fsr ? "true" : "false") << "\n"
            << "gamemode=" << (cfg.gamemode ? "true" : "false") << "\n"
            << "render_res=" << cfg.render_res << "\n";
    }
    sync_mangohud_config(cfg);
    push_perf_config(state);
    native_shell_sync_perf(state);
}

TelemetrySnapshot collect_telemetry() {
    TelemetrySnapshot snap;
    snap.gpu = detect_gpu_summary();
    snap.mem = detect_mem_usage(&snap.mem_used_mb);
    snap.net = detect_network_status();
    snap.kernel = read_first_line("/proc/sys/kernel/osrelease", "Linux");
    snap.is_live = is_live_iso_env();
    snap.low_fx = detect_low_fx_mode();
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

    std::string br_out = run_cmd_capture(
        "for d in /sys/class/backlight/*; do "
        "[ -r \"$d/brightness\" ] && [ -r \"$d/max_brightness\" ] || continue; "
        "cur=$(cat \"$d/brightness\"); max=$(cat \"$d/max_brightness\"); "
        "[ \"$max\" -gt 0 ] && awk \"BEGIN{printf \\\"%d\\\", ($cur/$max)*100}\" && break; "
        "done 2>/dev/null");
    if (!br_out.empty()) {
        snap.brightness = std::clamp(std::atoi(br_out.c_str()), 5, 100);
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
        std::error_code ec;
        fs::create_directories(config_dir(), ec);
        fs::path script_path = config_dir() / "display.sh";
        std::ofstream ofs(script_path);
        if (ofs.is_open()) {
            ofs << "#!/usr/bin/env bash\n"
                << "wlr-randr --output \"" << output << "\" --mode \"" << mode << "\" --scale \"" << scale << "\"\n";
        }
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
    std::string txt = read_tail_lines(app_log, 300);
    if (txt.empty()) {
        txt = "暂无应用启动日志。\n点击桌面或游戏中心中的 Steam、主机大屏、终端或应用商店软件后，输出与报错会实时记录在:\n" + app_log.string();
    }
    return "=== " + app_log.string() + " ===\n" + txt;
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
