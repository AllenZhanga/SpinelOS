#include "system_control.hpp"
#include "sys_internal.hpp"
#include "common.hpp"
#include "theme_manager.hpp"

#include <sys/stat.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

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

void write_fcitx5_classicui(bool is_light, bool force_overwrite) {
    fs::path conf_dir = fs::path(home_dir()) / ".config/fcitx5/conf";
    std::error_code ec;
    fs::create_directories(conf_dir, ec);
    fs::path cui = conf_dir / "classicui.conf";
    std::string theme_name = is_light ? "SpinelLight" : "SpinelDark";
    if (!fs::exists(cui, ec) || force_overwrite) {
        std::ostringstream out;
        out << "Vertical Candidate List=False\n"
            << "PerScreenDPI=True\n"
            << "Font=\"Noto Sans CJK SC 12\"\n"
            << "Theme=" << theme_name << "\n";
        write_text_file(cui.string(), out.str());
        return;
    }
    std::ifstream ifs(cui);
    if (!ifs.is_open()) return;
    std::ostringstream out;
    std::string line;
    bool replaced_theme = false;
    bool replaced_font = false;
    while (std::getline(ifs, line)) {
        if (line.rfind("Theme=", 0) == 0) {
            out << "Theme=" << theme_name << "\n";
            replaced_theme = true;
        } else if (line == "Font=\"Noto Sans CJK SC 11\"") {
            out << "Font=\"Noto Sans CJK SC 12\"\n";
            replaced_font = true;
        } else {
            out << line << "\n";
        }
    }
    ifs.close();
    if (!replaced_theme) out << "Theme=" << theme_name << "\n";
    if (!replaced_font && out.str().find("Font=") == std::string::npos) {
        out << "Font=\"Noto Sans CJK SC 12\"\n";
    }
    write_text_file(cui.string(), out.str());
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
    const std::string schema = prefer_rime_ice ? "rime_ice" : "luna_pinyin_simp";
    std::string content =
        "patch:\n"
        "  schema_list:\n"
        "    - schema: " + schema + "\n"
        "  \"menu/page_size\": 7\n";
    write_text_file(custom.string(), content);
}

bool has_pinyin_addon_installed() {
    std::error_code ec;
    return fs::exists("/usr/lib/fcitx5/libpinyin.so", ec) ||
           fs::exists("/usr/share/fcitx5/addon/pinyin.conf", ec);
}

bool has_rime_addon_installed() {
    std::error_code ec;
    return has_executable("fcitx5-rime") ||
           fs::exists("/usr/lib/fcitx5/librime.so", ec) ||
           fs::exists("/usr/share/fcitx5/addon/rime.conf", ec);
}

std::string default_ime_engine() {
    if (has_rime_addon_installed() || !has_pinyin_addon_installed()) {
        return "rime";
    }
    return "pinyin";
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

bool is_valid_labwc_key(const std::string &k) {
    if (k.empty() || k.size() > 40) return false;
    for (char ch : k) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && c != '-' && c != '_') return false;
    }
    return true;
}

fs::path shortcuts_conf_path() {
    return sys_internal::config_dir() / "shortcuts.conf";
}

}  // namespace

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
        fs::path script_path = sys_internal::config_dir() / "display.sh";
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

void ensure_fcitx5_defaults(bool is_light) {
    std::error_code ec;
    fs::path fdir = fs::path(home_dir()) / ".config/fcitx5";
    fs::create_directories(fdir / "conf", ec);

    ensure_chromium_ime_flags();

    std::string active_im = detect_active_ime_engine();
    bool has_pinyin = has_pinyin_addon_installed();
    bool has_ice = fs::exists(fs::path(home_dir()) / ".local/share/fcitx5/rime/rime_ice.schema.yaml", ec);
    write_rime_default_custom(has_ice);

    if (active_im == "none" || (active_im == "pinyin" && !has_pinyin)) {
        std::string pg_out;
        int pg_rc = 1;
        bool was_running = run_argv_capture({"pgrep", "-x", "fcitx5"}, pg_out, pg_rc) && pg_rc == 0;
        if (was_running) stop_fcitx5_sync();
        write_fcitx5_profile(default_ime_engine());
        if (was_running && has_executable("fcitx5")) {
            spawn_argv_detached({"sh", "-c", "setsid -f fcitx5 >/tmp/spinel-fcitx5.log 2>&1 </dev/null || true"});
        }
    }
    write_fcitx5_classicui(is_light, !fs::exists(fdir / "conf/classicui.conf", ec));
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
    fs::path stamp = sys_internal::config_dir() / "ptyxis-v1.stamp";
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
    bool rime_ok = has_rime_addon_installed();
    bool has_pinyin = has_pinyin_addon_installed();
    bool has_configtool = has_executable("fcitx5-configtool");
    std::error_code ec;
    bool rime_ice = fs::exists(fs::path(home_dir()) / ".local/share/fcitx5/rime/rime_ice.schema.yaml", ec);
    std::string pid_out;
    int pid_rc = 1;
    bool running = installed && run_argv_capture({"pgrep", "-x", "fcitx5"}, pid_out, pid_rc) && pid_rc == 0;
    int rss_mb = 0;
    if (running) {
        int pid = std::atoi(trim_copy(pid_out).c_str());
        if (pid > 0) {
            std::ifstream sifs("/proc/" + std::to_string(pid) + "/status");
            std::string line;
            while (std::getline(sifs, line)) {
                if (line.rfind("VmRSS:", 0) == 0) {
                    rss_mb = std::atoi(line.c_str() + 6) / 1024;
                    break;
                }
            }
        }
    }
    std::string engine = detect_active_ime_engine();

    std::ostringstream js;
    js << "window.__spinelImeState && window.__spinelImeState({"
       << "\"installed\":" << (installed ? "true" : "false") << ","
       << "\"running\":" << (running ? "true" : "false") << ","
       << "\"rime_installed\":" << (rime_ok ? "true" : "false") << ","
       << "\"has_pinyin\":" << (has_pinyin ? "true" : "false") << ","
       << "\"has_configtool\":" << (has_configtool ? "true" : "false") << ","
       << "\"rime_ice\":" << (rime_ice ? "true" : "false") << ","
       << "\"rss_mb\":" << rss_mb << ","
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
        write_fcitx5_profile(default_ime_engine());
    }
    spawn_argv_detached({"sh", "-c", "setsid -f fcitx5 >/tmp/spinel-fcitx5.log 2>&1 </dev/null || true"});
    append_app_log("[IME] 已重启 Fcitx5 输入法服务");
    if (state) {
        std::thread([state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            sys_internal::post_js_to_main(state, "window.__spinelImeFeedback && window.__spinelImeFeedback(true, \"Fcitx5 输入法服务已重启，按 Ctrl+Space 或 Shift 即可切换中英文。\");", false);
        }).detach();
    }
}

void set_ime_engine(AppState *state, const std::string &engine) {
    std::error_code ec;
    bool has_ice = fs::exists(fs::path(home_dir()) / ".local/share/fcitx5/rime/rime_ice.schema.yaml", ec);
    if (engine == "rime") {
        if (!has_ice) {
            install_rime_ice_dict(state);
            return;
        }
        write_rime_default_custom(true);
        stop_fcitx5_sync();
        write_fcitx5_profile("rime");
        append_app_log("[IME] 切换中文输入法方案为: rime (rime_ice)");
        restart_fcitx5_daemon(state);
        if (state && state->settings_view) push_ime_state(state);
        return;
    }

    // "pinyin" / built-in offline Chinese Pinyin: prefer fcitx5-chinese-addons if installed,
    // otherwise switch Rime cleanly to built-in Simplified Luna Pinyin (luna_pinyin_simp).
    std::string target = has_pinyin_addon_installed() ? "pinyin" : "rime";
    if (target == "rime") {
        write_rime_default_custom(false);
    }
    stop_fcitx5_sync();
    write_fcitx5_profile(target);
    append_app_log("[IME] 切换中文输入法方案为: " + target + (target == "rime" ? " (luna_pinyin_simp)" : ""));
    restart_fcitx5_daemon(state);
    if (state && state->settings_view) {
        push_ime_state(state);
    }
}

void install_rime_ice_dict(AppState *state) {
    if (!state) return;
    sys_internal::post_js_to_main(state, "window.__spinelImeFeedback && window.__spinelImeFeedback(true, \"正在下载雾凇拼音 (rime-ice) 词库并切换为 Rime，请稍候...\");", false);
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
            spawn_argv_detached({"sh", "-c", "setsid -f fcitx5 >/tmp/spinel-fcitx5.log 2>&1 </dev/null || true"});
            append_app_log("[IME] 雾凇拼音 (rime-ice) 下载安装成功并已激活 Rime");
            sys_internal::post_js_to_main(state, "window.__spinelImeFeedback && window.__spinelImeFeedback(true, \"雾凇拼音 (rime-ice) 安装完成！已自动切换为 Rime 并重启输入法，按 Ctrl+Space 即可输入。\");", false);
        } else {
            append_app_log("[IME] 雾凇拼音下载失败 (code=" + std::to_string(rc) + "): " + out);
            sys_internal::post_js_to_main(state, "window.__spinelImeFeedback && window.__spinelImeFeedback(false, \"雾凇拼音下载失败（网络访问 GitHub 超时），可先在「Wi-Fi / 网络」里开启代理后再试，或直接使用内置中文拼音。\");", false);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        g_idle_add([](gpointer d) -> gboolean {
            auto *st = static_cast<AppState *>(d);
            if (!st->shutting_down.load()) push_ime_state(st);
            return G_SOURCE_REMOVE;
        }, state);
    }).detach();
}

ShortcutConfig load_shortcuts_config() {
    ShortcutConfig sc;
    std::ifstream ifs(shortcuts_conf_path());
    if (!ifs.is_open()) return sc;
    std::string line;
    while (std::getline(ifs, line)) {
        line = trim_copy(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        if (!is_valid_labwc_key(v)) continue;
        if (k == "screenshot_area") sc.screenshot_area = v;
        else if (k == "screenshot_full") sc.screenshot_full = v;
        else if (k == "lock_screen") sc.lock_screen = v;
        else if (k == "open_terminal") sc.open_terminal = v;
    }
    return sc;
}

bool write_labwc_rc(bool is_light, const ShortcutConfig &sc) {
    const std::string theme_name = is_light ? "SpinelLight" : "SpinelDark";
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\"?>\n"
        << "<labwc_config>\n"
        << "  <core>\n"
        << "    <gap>0</gap>\n"
        << "    <adaptiveSync>yes</adaptiveSync>\n"
        << "    <reuseOutputMode>yes</reuseOutputMode>\n"
        << "  </core>\n\n"
        << "  <theme>\n"
        << "    <name>" << theme_name << "</name>\n"
        << "    <cornerRadius>12</cornerRadius>\n"
        << "    <dropShadows>yes</dropShadows>\n"
        << "    <titlebar>\n"
        << "      <layout>close,iconify,max:</layout>\n"
        << "    </titlebar>\n"
        << "  </theme>\n\n"
        << "  <desktops number=\"4\">\n"
        << "    <popupTime>450</popupTime>\n"
        << "    <prefix>工作区</prefix>\n"
        << "  </desktops>\n\n"
        << "  <snapping>\n"
        << "    <range inner=\"10\" outer=\"10\" />\n"
        << "    <topMaximize>yes</topMaximize>\n"
        << "  </snapping>\n\n"
        << "  <keyboard>\n"
        << "    <default />\n"
        << "    <keybind key=\"" << sc.open_terminal << "\">\n"
        << "      <action name=\"Execute\" command=\"spinel-terminal\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"" << sc.screenshot_full << "\">\n"
        << "      <action name=\"Execute\" command=\"spinel-screenshot full\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"" << sc.screenshot_area << "\">\n"
        << "      <action name=\"Execute\" command=\"spinel-screenshot area\" />\n"
        << "    </keybind>\n";
    if (sc.screenshot_area != "S-Print" && sc.screenshot_full != "S-Print") {
        xml << "    <keybind key=\"S-Print\">\n"
            << "      <action name=\"Execute\" command=\"spinel-screenshot area\" />\n"
            << "    </keybind>\n";
    }
    xml << "    <keybind key=\"" << sc.lock_screen << "\">\n"
        << "      <action name=\"Execute\" command=\"spinel-lock\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"A-space\">\n"
        << "      <action name=\"Execute\" command=\"spinel-shell --launcher\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"XF86AudioRaiseVolume\">\n"
        << "      <action name=\"Execute\" command=\"spinel-shell --osd volume-up\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"XF86AudioLowerVolume\">\n"
        << "      <action name=\"Execute\" command=\"spinel-shell --osd volume-down\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"XF86AudioMute\">\n"
        << "      <action name=\"Execute\" command=\"spinel-shell --osd mute\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"XF86MonBrightnessUp\">\n"
        << "      <action name=\"Execute\" command=\"spinel-shell --osd bright-up\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"XF86MonBrightnessDown\">\n"
        << "      <action name=\"Execute\" command=\"spinel-shell --osd bright-down\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"A-Tab\">\n"
        << "      <action name=\"Execute\" command=\"spinel-shell --switcher next\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"A-S-Tab\">\n"
        << "      <action name=\"Execute\" command=\"spinel-shell --switcher prev\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"A-F4\">\n"
        << "      <action name=\"Close\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"W-Left\">\n"
        << "      <action name=\"SnapToEdge\" direction=\"left\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"W-Right\">\n"
        << "      <action name=\"SnapToEdge\" direction=\"right\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"W-Up\">\n"
        << "      <action name=\"ToggleMaximize\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"W-Down\">\n"
        << "      <action name=\"Iconify\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"C-A-Left\">\n"
        << "      <action name=\"GoToDesktop\" to=\"left\" wrap=\"yes\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"C-A-Right\">\n"
        << "      <action name=\"GoToDesktop\" to=\"right\" wrap=\"yes\" />\n"
        << "    </keybind>\n"
        << "  </keyboard>\n\n"
        << "  <mouse>\n"
        << "    <default />\n"
        << "  </mouse>\n"
        << "</labwc_config>\n";

    std::string desired = xml.str();
    fs::path user_rc = fs::path(home_dir()) / ".config/labwc/rc.xml";
    std::error_code ec;
    if (fs::exists(user_rc, ec)) {
        std::ifstream cur_ifs(user_rc);
        if (cur_ifs.is_open()) {
            std::string existing((std::istreambuf_iterator<char>(cur_ifs)), std::istreambuf_iterator<char>());
            if (existing == desired) return true;
        }
    }
    if (write_text_file(user_rc.string(), desired)) {
        spawn_argv_detached({"pkill", "-HUP", "-x", "labwc"});
        return true;
    }
    return false;
}

void push_shortcuts_config(AppState *state) {
    if (!state || !state->settings_view) return;
    ShortcutConfig sc = load_shortcuts_config();
    std::ostringstream js;
    js << "window.__spinelShortcutsData && window.__spinelShortcutsData({"
       << "\"screenshot_area\":\"" << json_escape(sc.screenshot_area) << "\","
       << "\"screenshot_full\":\"" << json_escape(sc.screenshot_full) << "\","
       << "\"lock_screen\":\"" << json_escape(sc.lock_screen) << "\","
       << "\"open_terminal\":\"" << json_escape(sc.open_terminal) << "\""
       << "});";
    eval_js(state->settings_view, js.str());
}

void update_shortcuts_config(AppState *state, const std::string &payload) {
    ShortcutConfig sc = load_shortcuts_config();
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        if (!is_valid_labwc_key(v)) continue;
        if (k == "screenshot_area") sc.screenshot_area = v;
        else if (k == "screenshot_full") sc.screenshot_full = v;
        else if (k == "lock_screen") sc.lock_screen = v;
        else if (k == "open_terminal") sc.open_terminal = v;
    }

    std::ostringstream oss;
    oss << "screenshot_area=" << sc.screenshot_area << "\n"
        << "screenshot_full=" << sc.screenshot_full << "\n"
        << "lock_screen=" << sc.lock_screen << "\n"
        << "open_terminal=" << sc.open_terminal << "\n";
    bool ok = write_text_file(shortcuts_conf_path().string(), oss.str()) &&
              write_labwc_rc(get_active_theme().is_light, sc);

    append_app_log(std::string("[Shortcuts] 更新全局快捷键 (区域截图=") + sc.screenshot_area +
                   ", 全屏截图=" + sc.screenshot_full + ", 锁屏=" + sc.lock_screen + ", 终端=" + sc.open_terminal + ")");
    push_shortcuts_config(state);
    if (state && state->settings_view) {
        eval_js(state->settings_view,
                std::string("window.__spinelShortcutsFeedback && window.__spinelShortcutsFeedback(") +
                    (ok ? "true" : "false") + ",\"" +
                    (ok ? "快捷键已保存并实时重载到桌面合成器 (labwc)！" : "保存快捷键配置失败。") + "\");");
    }
}

void reset_shortcuts_config(AppState *state) {
    ShortcutConfig def;
    std::ostringstream oss;
    oss << "screenshot_area=" << def.screenshot_area << "\n"
        << "screenshot_full=" << def.screenshot_full << "\n"
        << "lock_screen=" << def.lock_screen << "\n"
        << "open_terminal=" << def.open_terminal << "\n";
    write_text_file(shortcuts_conf_path().string(), oss.str());
    write_labwc_rc(get_active_theme().is_light, def);
    append_app_log("[Shortcuts] 已恢复默认系统快捷键");
    push_shortcuts_config(state);
    if (state && state->settings_view) {
        eval_js(state->settings_view,
                "window.__spinelShortcutsFeedback && window.__spinelShortcutsFeedback(true, \"已恢复默认快捷键并立即生效！\");");
    }
}
