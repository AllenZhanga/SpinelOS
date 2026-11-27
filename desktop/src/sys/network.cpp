#include "system_control.hpp"
#include "sys_internal.hpp"
#include "ai/os_atomics.hpp"
#include "common.hpp"
#include "native_shell.hpp"
#include "steam_library.hpp"

#include <arpa/inet.h>

#include <algorithm>
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
    eval_js(call->state->monitor_view, call->script);
    if (call->also_bar) {
        eval_js(call->state->bar_view, call->script);
        eval_js(call->state->desktop_view, call->script);
    }
    return G_SOURCE_REMOVE;
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

void sync_mangohud_config(const PerfConfig &cfg) {
    std::error_code ec;
    fs::path mh_dir = fs::path(home_dir()) / ".config/MangoHud";
    fs::create_directories(mh_dir, ec);
    std::ofstream ofs(mh_dir / "MangoHud.conf");
    if (!ofs.is_open()) return;

    ofs << "# Managed by SpinelOS Performance Control\n";
    if (cfg.mangohud == "off") {
        ofs << "no_display\n";
        ofs << "toggle_hud=Shift_R+F12\n";
        return;
    }
    ofs << "position=top-left\n";
    ofs << "background_alpha=0.45\n";
    ofs << "font_size=20\n";
    ofs << "toggle_hud=Shift_R+F12\n";
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

namespace sys_internal {

fs::path config_dir() {
    return fs::path(home_dir()) / ".config/spinelos";
}

void post_js_to_main(AppState *state, std::string script, bool also_bar) {
    if (!state || state->shutting_down.load()) return;
    g_idle_add(run_js_idle, new JsIdleCall{state, std::move(script), also_bar});
}

std::string detect_mem_usage(int *used_mb_out) {
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

}  // namespace sys_internal

PerfConfig load_perf_config() {
    PerfConfig cfg;
    std::ifstream ifs(sys_internal::config_dir() / "perf.conf");
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
    std::ifstream ifs(sys_internal::config_dir() / "proxy.conf");
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

std::string strip_single_quotes(std::string s) {
    s = trim_copy(s);
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

std::string gsettings_proxy_get(const char *schema, const char *key) {
    std::string out;
    int rc = 1;
    if (!run_argv_capture({"gsettings", "get", schema, key}, out, rc) || rc != 0) return "";
    return strip_single_quotes(out);
}

void sync_gsettings_proxy(const ProxyConfig &cfg) {
    if (!has_executable("gsettings")) return;
    auto parse_host_port = [](const std::string &url, std::string &host_out, std::string &port_out) {
        auto pos = url.find("://");
        std::string rest = (pos != std::string::npos) ? url.substr(pos + 3) : url;
        auto slash = rest.find('/');
        if (slash != std::string::npos) rest = rest.substr(0, slash);
        auto colon = rest.rfind(':');
        if (colon == std::string::npos) return false;
        host_out = rest.substr(0, colon);
        port_out = rest.substr(colon + 1);
        return !host_out.empty() && !port_out.empty();
    };
    std::string out;
    int rc = 1;
    if (!cfg.enabled || (cfg.http_proxy.empty() && cfg.socks_proxy.empty())) {
        run_argv_capture({"gsettings", "set", "org.gnome.system.proxy", "mode", "none"}, out, rc);
        return;
    }
    run_argv_capture({"gsettings", "set", "org.gnome.system.proxy", "mode", "manual"}, out, rc);
    std::string h, p;
    if (parse_host_port(cfg.http_proxy, h, p)) {
        run_argv_capture({"gsettings", "set", "org.gnome.system.proxy.http", "enabled", "true"}, out, rc);
        run_argv_capture({"gsettings", "set", "org.gnome.system.proxy.http", "host", h}, out, rc);
        run_argv_capture({"gsettings", "set", "org.gnome.system.proxy.http", "port", p}, out, rc);
        run_argv_capture({"gsettings", "set", "org.gnome.system.proxy.https", "host", h}, out, rc);
        run_argv_capture({"gsettings", "set", "org.gnome.system.proxy.https", "port", p}, out, rc);
    }
    if (parse_host_port(cfg.socks_proxy, h, p)) {
        run_argv_capture({"gsettings", "set", "org.gnome.system.proxy.socks", "host", h}, out, rc);
        run_argv_capture({"gsettings", "set", "org.gnome.system.proxy.socks", "port", p}, out, rc);
    }
}

ProxyConfig resolve_effective_proxy() {
    ProxyConfig cfg = load_proxy_config();
    if (cfg.enabled && (!cfg.http_proxy.empty() || !cfg.socks_proxy.empty())) {
        return cfg;
    }
    if (!has_executable("gsettings")) return cfg;
    if (gsettings_proxy_get("org.gnome.system.proxy", "mode") != "manual") return cfg;

    // Pick up desktop-wide GSettings proxies written by Clash/Monocloud/v2rayN even if http.enabled was left false
    auto read_hp = [&](const char *subschema, const char *scheme) -> std::string {
        std::string host = gsettings_proxy_get(subschema, "host");
        std::string port = gsettings_proxy_get(subschema, "port");
        int pnum = std::atoi(port.c_str());
        if (host.empty() || pnum <= 0 || pnum > 65535) return "";
        std::string url = std::string(scheme) + "://" + host + ":" + std::to_string(pnum);
        return is_valid_proxy_url(url) ? url : "";
    };

    std::string http_u = read_hp("org.gnome.system.proxy.http", "http");
    std::string https_u = read_hp("org.gnome.system.proxy.https", "http");
    std::string socks_u = read_hp("org.gnome.system.proxy.socks", "socks5");

    cfg.http_proxy = !http_u.empty() ? http_u : https_u;
    cfg.socks_proxy = socks_u;
    cfg.enabled = (!cfg.http_proxy.empty() || !cfg.socks_proxy.empty());
    return cfg;
}

std::vector<std::pair<std::string, std::string>> build_effective_proxy_envs() {
    ProxyConfig cfg = resolve_effective_proxy();
    if (!cfg.enabled || (cfg.http_proxy.empty() && cfg.socks_proxy.empty())) return {};
    std::string http_val = !cfg.http_proxy.empty() ? cfg.http_proxy : cfg.socks_proxy;
    std::string all_val = !cfg.socks_proxy.empty() ? cfg.socks_proxy : cfg.http_proxy;
    return {
        {"http_proxy", http_val},
        {"https_proxy", http_val},
        {"HTTP_PROXY", http_val},
        {"HTTPS_PROXY", http_val},
        {"all_proxy", all_val},
        {"ALL_PROXY", all_val},
        {"no_proxy", cfg.no_proxy},
        {"NO_PROXY", cfg.no_proxy}
    };
}

void launch_browser_command(std::vector<std::string> args, const std::string &tag,
                            const std::string &working_dir, std::size_t program_index) {
    if (program_index >= args.size()) return;
    std::string bin_lower = fs::path(args[program_index]).filename().string();
    for (char &ch : bin_lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    bool is_chromium_like = bin_lower.find("chromium") != std::string::npos ||
                            bin_lower.find("chrome") != std::string::npos ||
                            bin_lower.find("brave") != std::string::npos ||
                            bin_lower.find("msedge") != std::string::npos;

    auto has_arg = [&](const std::string &prefix) {
        return std::any_of(args.begin(), args.end(), [&](const std::string &a) {
            return a == prefix || a.rfind(prefix, 0) == 0;
        });
    };

    if (is_chromium_like) {
        if (!has_arg("--ozone-platform=")) args.push_back("--ozone-platform=wayland");
        if (!has_arg("--enable-wayland-ime")) args.push_back("--enable-wayland-ime");
        if (!has_arg("--wayland-text-input-version=")) args.push_back("--wayland-text-input-version=3");
    }

    auto envs = build_effective_proxy_envs();
    envs.emplace_back("XDG_CURRENT_DESKTOP", "SpinelOS:GNOME");

    ProxyConfig p = resolve_effective_proxy();
    // When GSettings is present, Chromium dynamically watches org.gnome.system.proxy via XDG_CURRENT_DESKTOP=SpinelOS:GNOME.
    // Only pin --proxy-server if GSettings is unavailable so live proxy toggling works without restarting Chromium.
    if (is_chromium_like && !has_executable("gsettings") && p.enabled && (!p.http_proxy.empty() || !p.socks_proxy.empty())) {
        std::string srv = !p.http_proxy.empty() ? p.http_proxy : p.socks_proxy;
        if (!has_arg("--proxy-server=")) args.push_back("--proxy-server=" + srv);
        if (!p.no_proxy.empty() && !has_arg("--proxy-bypass-list=")) {
            args.push_back("--proxy-bypass-list=" + p.no_proxy);
        }
    }

    spawn_argv_logged(args, tag, envs, working_dir);
}

bool open_url_in_browser(const std::string &url, const std::string &tag) {
    std::string clean = trim_copy(url);
    if (clean.rfind("https://", 0) != 0 && clean.rfind("http://", 0) != 0) {
        return false;
    }
    for (const char *ch : {" ", "\t", "\r", "\n", "\"", "'"}) {
        if (clean.find(ch) != std::string::npos) return false;
    }

    static const char *kBrowsers[] = {
        "google-chrome-stable",
        "chromium",
        "firefox",
        "brave",
        "microsoft-edge-stable",
        "vivaldi-stable"
    };
    for (const char *bin : kBrowsers) {
        if (has_executable(bin)) {
            launch_browser_command({bin, clean}, tag);
            return true;
        }
    }

    if (has_executable("xdg-open") && has_executable("xdg-mime")) {
        std::string handler_out;
        int rc = 1;
        if (run_argv_capture({"xdg-mime", "query", "default", "x-scheme-handler/https"}, handler_out, rc) &&
            rc == 0 && !trim_copy(handler_out).empty()) {
            auto envs = build_effective_proxy_envs();
            envs.emplace_back("XDG_CURRENT_DESKTOP", "SpinelOS:GNOME");
            spawn_argv_logged({"xdg-open", clean}, tag, envs);
            return true;
        }
    }
    return false;
}

void sync_process_proxy_env() {
    ProxyConfig cfg = resolve_effective_proxy();
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
    ProxyConfig eff = resolve_effective_proxy();
    std::string ext_addr = (!cfg.enabled && eff.enabled) ? (!eff.http_proxy.empty() ? eff.http_proxy : eff.socks_proxy) : "";
    std::string gw = run_cmd_capture("ip route 2>/dev/null | awk '/default/ {print $3; exit}'");
    std::ostringstream js;
    js << "window.__spinelProxyConfig && window.__spinelProxyConfig({"
       << "\"enabled\":" << (cfg.enabled ? "true" : "false") << ","
       << "\"external_proxy\":\"" << json_escape(ext_addr) << "\","
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
    write_text_file((sys_internal::config_dir() / "proxy.conf").string(), out.str());
    sync_gsettings_proxy(cfg);
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
    write_text_file((sys_internal::config_dir() / "perf.conf").string(), perf_out.str());
    sync_mangohud_config(cfg);
    spinel::ai::sync_vram_with_gamemode(cfg.gamemode);
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
    snap.mem = sys_internal::detect_mem_usage(&snap.mem_used_mb);
    snap.net = detect_network_status();
    snap.kernel = cached_kernel;
    snap.spinel_version = trim_copy(read_first_line("/etc/spinelos/version", "0.0.0-dev"));
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
    snap.volume_muted = (vol_out.find("MUTED") != std::string::npos);

    if (has_executable("brightnessctl")) {
        std::string b_out;
        int b_rc = 1;
        if (run_argv_capture({"brightnessctl", "-m"}, b_out, b_rc) && b_rc == 0 && !b_out.empty()) {
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
    std::string spinel_ver = json_escape(snap.spinel_version);

    std::ostringstream js;
    js << "window.__spinelUpdateTelemetry && window.__spinelUpdateTelemetry({"
       << "\"gpu\":\"" << gpu << "\","
       << "\"mem\":\"" << mem << "\","
       << "\"net\":\"" << net << "\","
       << "\"kernel\":\"" << kernel << "\","
       << "\"spinel_version\":\"" << spinel_ver << "\","
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
            const std::string &dev = cols[0];
            const std::string &con_name = cols[3];

            std::string ip_cidr, gw, dns_csv, ip_method = "auto";
            if (is_valid_iface(dev)) {
                std::string dinfo;
                int drc = 0;
                if (run_argv_capture({"nmcli", "-t", "-f", "IP4.ADDRESS,IP4.GATEWAY,IP4.DNS", "dev", "show", dev}, dinfo, drc) && drc == 0) {
                    std::vector<std::string> dns_list;
                    for (const auto &dl : split_lines(dinfo)) {
                        auto p = dl.find(':');
                        if (p == std::string::npos) continue;
                        std::string k = dl.substr(0, p);
                        std::string v = trim_copy(dl.substr(p + 1));
                        if (k.rfind("IP4.ADDRESS", 0) == 0 && ip_cidr.empty()) ip_cidr = v;
                        else if (k.rfind("IP4.GATEWAY", 0) == 0 && gw.empty() && v != "--") gw = v;
                        else if (k.rfind("IP4.DNS", 0) == 0 && !v.empty() && v != "--") dns_list.push_back(v);
                    }
                    for (size_t i = 0; i < dns_list.size(); ++i) {
                        if (i) dns_csv += ", ";
                        dns_csv += dns_list[i];
                    }
                }
                if (!con_name.empty() && con_name != "--") {
                    std::string cinfo;
                    int crc = 0;
                    if (run_argv_capture({"nmcli", "-t", "-f", "ipv4.method,ipv4.dns", "con", "show", con_name}, cinfo, crc) && crc == 0) {
                        for (const auto &cl : split_lines(cinfo)) {
                            auto p = cl.find(':');
                            if (p == std::string::npos) continue;
                            std::string k = cl.substr(0, p);
                            std::string v = trim_copy(cl.substr(p + 1));
                            if (k == "ipv4.method" && v == "manual") ip_method = "manual";
                            else if (k == "ipv4.dns" && !v.empty() && v != "--") dns_csv = v;
                        }
                    }
                }
            }

            std::string ip_only = ip_cidr;
            std::string prefix_only = "24";
            auto sl = ip_cidr.find('/');
            if (sl != std::string::npos) {
                ip_only = ip_cidr.substr(0, sl);
                prefix_only = ip_cidr.substr(sl + 1);
            }

            if (!first_dev) js << ",";
            first_dev = false;
            js << "{\"device\":\"" << json_escape(dev) << "\","
               << "\"type\":\"" << json_escape(cols[1]) << "\","
               << "\"state\":\"" << json_escape(cols[2]) << "\","
               << "\"connection\":\"" << json_escape(con_name == "--" ? "" : con_name) << "\","
               << "\"method\":\"" << json_escape(ip_method) << "\","
               << "\"ip\":\"" << json_escape(ip_only) << "\","
               << "\"prefix\":\"" << json_escape(prefix_only) << "\","
               << "\"gateway\":\"" << json_escape(gw) << "\","
               << "\"dns\":\"" << json_escape(dns_csv) << "\"}";
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
        sys_internal::post_js_to_main(state, js.str());
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
        sys_internal::post_js_to_main(state, "window.__spinelWifiAction && window.__spinelWifiAction(false, \"Wi-Fi 名称不能为空。\");");
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
        sys_internal::post_js_to_main(state, js.str());
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
        sys_internal::post_js_to_main(state, js.str());
        push_wifi_state(state, false);
    }).detach();
}

bool is_valid_ipv4_addr(const std::string &ip) {
    if (ip.empty() || ip.size() > 15) return false;
    struct in_addr addr{};
    return inet_pton(AF_INET, ip.c_str(), &addr) == 1;
}

bool parse_ipv4_mask_prefix(const std::string &mask_or_prefix, int &prefix_out) {
    std::string s = trim_copy(mask_or_prefix);
    if (s.empty()) return false;
    if (s.front() == '/') s.erase(s.begin());
    if (s.empty()) return false;

    bool all_digits = std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    if (all_digits) {
        int p = std::atoi(s.c_str());
        if (p >= 1 && p <= 32) {
            prefix_out = p;
            return true;
        }
        return false;
    }

    struct in_addr addr{};
    if (inet_pton(AF_INET, s.c_str(), &addr) != 1) return false;
    uint32_t m = ntohl(addr.s_addr);
    if (m == 0) return false;
    // Verify contiguous leading 1s: ~m + 1 must be a power of two (including 0 when m == 0xffffffff)
    uint32_t inv = ~m;
    if ((inv & (inv + 1)) != 0) return false;
    int bits = 0;
    while (m & 0x80000000u) {
        ++bits;
        m <<= 1;
    }
    prefix_out = bits;
    return bits >= 1 && bits <= 32;
}

void apply_iface_ip_config(AppState *state, const std::string &payload) {
    if (!state || state->shutting_down.load() || state->wifi_busy.exchange(true)) return;

    std::string device, con_name, method = "auto", ip, mask = "24", gateway, dns_raw;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        if (k == "device") device = v;
        else if (k == "connection") con_name = v;
        else if (k == "method") method = v;
        else if (k == "ip") ip = v;
        else if (k == "mask") mask = v;
        else if (k == "gateway") gateway = v;
        else if (k == "dns") dns_raw = v;
    }

    auto fail_fast = [&](const std::string &msg) {
        state->wifi_busy.store(false);
        sys_internal::post_js_to_main(state,
            "window.__spinelWifiAction && window.__spinelWifiAction(false,\"" + json_escape(msg) + "\");");
    };

    if (!is_valid_iface(device)) {
        fail_fast("无效的网络接口名称: " + device);
        return;
    }

    // Parse & validate DNS list (comma or whitespace separated)
    std::string normalized_dns;
    {
        std::string tmp = dns_raw;
        for (char &ch : tmp) {
            if (ch == ',' || ch == ';') ch = ' ';
        }
        std::istringstream iss(tmp);
        std::string tok;
        while (iss >> tok) {
            if (!is_valid_ipv4_addr(tok)) {
                fail_fast("DNS 地址格式无效: " + tok);
                return;
            }
            if (!normalized_dns.empty()) normalized_dns += ' ';
            normalized_dns += tok;
        }
    }

    bool manual = (method == "manual");
    int prefix = 24;
    if (manual) {
        if (!is_valid_ipv4_addr(ip)) {
            fail_fast("静态 IPv4 地址格式无效: " + ip);
            return;
        }
        if (!parse_ipv4_mask_prefix(mask, prefix)) {
            fail_fast("子网掩码或前缀格式无效: " + mask);
            return;
        }
        if (!gateway.empty() && !is_valid_ipv4_addr(gateway)) {
            fail_fast("默认网关 IPv4 格式无效: " + gateway);
            return;
        }
    }

    std::thread([state, device, con_name, manual, ip, prefix, gateway, normalized_dns]() {
        std::string target_con = con_name;
        if (target_con.empty() || target_con == "--") {
            std::string out;
            int rc = 0;
            if (run_argv_capture({"nmcli", "-t", "-f", "GENERAL.CONNECTION", "dev", "show", device}, out, rc) && rc == 0) {
                for (const auto &l : split_lines(out)) {
                    auto p = l.find(':');
                    if (p != std::string::npos) {
                        std::string v = trim_copy(l.substr(p + 1));
                        if (!v.empty() && v != "--") target_con = v;
                    }
                }
            }
        }

        std::string out;
        int rc = 1;
        if (target_con.empty()) {
            target_con = "wired-" + device;
            run_argv_capture({"nmcli", "con", "add", "type", "ethernet", "ifname", device, "con-name", target_con}, out, rc);
        }

        std::vector<std::string> mod_args = {"nmcli", "con", "modify", target_con};
        if (manual) {
            mod_args.insert(mod_args.end(), {
                "ipv4.method", "manual",
                "ipv4.addresses", ip + "/" + std::to_string(prefix),
                "ipv4.gateway", gateway
            });
        } else {
            mod_args.insert(mod_args.end(), {
                "ipv4.method", "auto",
                "ipv4.addresses", "",
                "ipv4.gateway", ""
            });
        }
        if (!normalized_dns.empty()) {
            mod_args.insert(mod_args.end(), {
                "ipv4.dns", normalized_dns,
                "ipv4.ignore-auto-dns", "yes"
            });
        } else {
            mod_args.insert(mod_args.end(), {
                "ipv4.dns", "",
                "ipv4.ignore-auto-dns", "no"
            });
        }

        bool ok = run_argv_capture(mod_args, out, rc) && rc == 0;
        if (ok) {
            std::string up_out;
            int up_rc = 1;
            ok = run_argv_capture({"nmcli", "con", "up", target_con, "ifname", device}, up_out, up_rc) && up_rc == 0;
            if (!ok) out = up_out;
        }

        state->wifi_busy.store(false);
        std::string msg = ok
            ? ("已应用网络配置 (" + device + ": " + (manual ? (ip + "/" + std::to_string(prefix)) : "DHCP 自动获取") +
               (normalized_dns.empty() ? "" : (" · DNS " + normalized_dns)) + ")")
            : ("应用网络配置失败: " + (out.empty() ? target_con : trim_copy(out)));
        sys_internal::post_js_to_main(state,
            "window.__spinelWifiAction && window.__spinelWifiAction(" +
                std::string(ok ? "true" : "false") + ",\"" + json_escape(msg) + "\");");
        push_wifi_state(state, false);
        push_proxy_config(state);
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
        sys_internal::post_js_to_main(state, js.str());
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
        sys_internal::post_js_to_main(state, js.str());
        push_bt_state(state, false);
    }).detach();
}
