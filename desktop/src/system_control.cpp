#include "system_control.hpp"
#include "common.hpp"

#include <sys/stat.h>

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

std::string detect_mem_usage() {
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
}

void push_telemetry_to_view(WebKitWebView *view) {
    if (!view) return;
    std::string gpu = json_escape(detect_gpu_summary());
    std::string mem = json_escape(detect_mem_usage());
    std::string net = json_escape(detect_network_status());
    std::string kernel = json_escape(read_first_line("/proc/sys/kernel/osrelease", "Linux"));
    bool is_live = is_live_iso_env();
    bool low_fx = detect_low_fx_mode();

    std::ostringstream js;
    js << "window.__spinelUpdateTelemetry && window.__spinelUpdateTelemetry({"
       << "\"gpu\":\"" << gpu << "\","
       << "\"mem\":\"" << mem << "\","
       << "\"net\":\"" << net << "\","
       << "\"kernel\":\"" << kernel << "\","
       << "\"is_live\":" << (is_live ? "true" : "false") << ","
       << "\"low_fx\":" << (low_fx ? "true" : "false")
       << "});";
    eval_js(view, js.str());
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
