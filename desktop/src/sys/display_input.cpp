#include "system_control.hpp"
#include "sys_internal.hpp"
#include "common.hpp"
#include "native_shell.hpp"
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

bool hide_fcitx5_gnome_diagnosis() {
    constexpr const char *group = "HiddenNotifications";
    constexpr const char *tip = "wayland-diagnose-gnome";
    const fs::path path = fs::path(g_get_user_config_dir()) / "fcitx5/conf/notifications.conf";
    g_autoptr(GKeyFile) config = g_key_file_new();
    g_autoptr(GError) error = nullptr;
    auto flags = static_cast<GKeyFileFlags>(G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS);
    if (!g_key_file_load_from_file(config, path.c_str(), flags, &error) &&
        !g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) return false;

    g_auto(GStrv) keys = g_key_file_get_keys(config, group, nullptr, nullptr);
    for (char **key = keys; key && *key; ++key) {
        g_autofree char *value = g_key_file_get_string(config, group, *key, nullptr);
        if (g_strcmp0(value, tip) == 0) return false;
    }
    std::size_t index = 0;
    while (g_key_file_has_key(config, group, std::to_string(index).c_str(), nullptr)) ++index;
    g_key_file_set_string(config, group, std::to_string(index).c_str(), tip);

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    return !ec && g_key_file_save_to_file(config, path.c_str(), nullptr);
}

struct ImePrefs {
    bool traditional = false;
};

fs::path ime_prefs_path() {
    return sys_internal::config_dir() / "ime.conf";
}

ImePrefs load_ime_prefs() {
    ImePrefs p;
    std::ifstream ifs(ime_prefs_path());
    if (!ifs.is_open()) return p;
    std::string line;
    while (std::getline(ifs, line)) {
        line = trim_copy(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        if (k == "traditional") {
            p.traditional = (v == "1" || v == "true" || v == "yes");
        }
    }
    return p;
}

bool save_ime_prefs(const ImePrefs &p) {
    std::error_code ec;
    fs::create_directories(ime_prefs_path().parent_path(), ec);
    return write_text_file(ime_prefs_path().string(),
                           std::string("traditional=") + (p.traditional ? "true\n" : "false\n"));
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
            << "EnableFractionalScale=True\n"
            << "WheelForPaging=True\n"
            << "PreferTextIcon=False\n"
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
    bool has_frac = false;
    bool has_wheel = false;
    while (std::getline(ifs, line)) {
        if (line.rfind("Theme=", 0) == 0) {
            out << "Theme=" << theme_name << "\n";
            replaced_theme = true;
        } else if (line == "Font=\"Noto Sans CJK SC 11\"") {
            out << "Font=\"Noto Sans CJK SC 12\"\n";
            replaced_font = true;
        } else {
            if (line.rfind("EnableFractionalScale=", 0) == 0) has_frac = true;
            if (line.rfind("WheelForPaging=", 0) == 0) has_wheel = true;
            out << line << "\n";
        }
    }
    ifs.close();
    if (!replaced_theme) out << "Theme=" << theme_name << "\n";
    if (!replaced_font && out.str().find("Font=") == std::string::npos) {
        out << "Font=\"Noto Sans CJK SC 12\"\n";
    }
    if (!has_frac) out << "EnableFractionalScale=True\n";
    if (!has_wheel) out << "WheelForPaging=True\n";
    write_text_file(cui.string(), out.str());
}

void write_fcitx5_global_config() {
    fs::path cfg_path = fs::path(home_dir()) / ".config/fcitx5/config";
    std::error_code ec;
    fs::create_directories(cfg_path.parent_path(), ec);
    // Keep Simple: single global EN<->ZH switch layer (Ctrl+Space & tap Shift), shared state, zero OSD noise.
    std::ostringstream out;
    out << "[Hotkey]\n"
        << "EnumerateWithTriggerKeys=True\n"
        << "ConsumeRedundantActivateKeys=False\n"
        << "EnumerateSkipFirst=False\n"
        << "ModifierOnlyKeyTimeout=250\n\n"
        << "[Hotkey/TriggerKeys]\n"
        << "0=Control+space\n\n"
        << "[Hotkey/AltTriggerKeys]\n"
        << "0=Shift_L\n"
        << "1=Shift_R\n\n"
        << "[Hotkey/ActivateKeys]\n\n"
        << "[Hotkey/DeactivateKeys]\n\n"
        << "[Hotkey/EnumerateForwardKeys]\n\n"
        << "[Hotkey/EnumerateBackwardKeys]\n\n"
        << "[Hotkey/EnumerateGroupForwardKeys]\n\n"
        << "[Hotkey/EnumerateGroupBackwardKeys]\n\n"
        << "[Hotkey/PrevPage]\n"
        << "0=Up\n"
        << "1=comma\n"
        << "2=minus\n\n"
        << "[Hotkey/NextPage]\n"
        << "0=Down\n"
        << "1=period\n"
        << "2=equal\n\n"
        << "[Behavior]\n"
        << "ActiveByDefault=False\n"
        << "resetStateWhenFocusIn=No\n"
        << "ShareInputState=All\n"
        << "PreeditEnabledByDefault=True\n"
        << "ShowInputMethodInformation=False\n"
        << "showInputMethodInformationWhenFocusIn=False\n"
        << "CompactInputMethodInformation=True\n"
        << "ShowFirstInputMethodInformation=False\n"
        << "DefaultPageSize=7\n"
        << "PreloadInputMethod=True\n"
        << "AllowInputMethodForPassword=False\n"
        << "ShowPreeditForPassword=False\n";
    write_text_file(cfg_path.string(), out.str());
}

void write_fcitx5_rime_addon_conf() {
    fs::path conf_dir = fs::path(home_dir()) / ".config/fcitx5/conf";
    std::error_code ec;
    fs::create_directories(conf_dir, ec);
    // Commit raw ASCII preedit immediately when switching to English via Shift / Ctrl+Space.
    std::ostringstream out;
    out << "PreeditMode=Composing text\n"
        << "InputState=All\n"
        << "PreeditCursorPositionAtBeginning=True\n"
        << "SwitchInputMethodBehavior=Commit raw input\n";
    write_text_file((conf_dir / "rime.conf").string(), out.str());
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
    const char *flag_files[] = {
        "chromium-flags.conf",
        "chrome-flags.conf",
        "code-flags.conf",
        "electron-flags.conf",
    };
    for (const char *name : flag_files) {
        fs::path flags = cfg_dir / name;
        if (!fs::exists(flags, ec)) {
            std::ofstream ofs(flags, std::ios::trunc);
            if (ofs.is_open()) {
                ofs << "--ozone-platform=wayland\n"
                    << "--enable-wayland-ime\n"
                    << "--wayland-text-input-version=3\n";
            }
        }
    }
}

void write_rime_default_custom(bool prefer_rime_ice, bool traditional = false) {
    fs::path rime_dir = fs::path(home_dir()) / ".local/share/fcitx5/rime";
    std::error_code ec;
    fs::create_directories(rime_dir, ec);
    fs::path custom = rime_dir / "default.custom.yaml";
    const std::string schema = prefer_rime_ice ? "rime_ice" : "luna_pinyin_simp";
    // Keep Simple: one Chinese schema, no F4 schema-menu popup, and let Fcitx5 own Shift/Ctrl+Space switching.
    std::string content =
        "# Managed by SpinelOS: simple English/Chinese switching without two-layer Shift conflicts\n"
        "patch:\n"
        "  schema_list:\n"
        "    - schema: " + schema + "\n"
        "  \"menu/page_size\": 7\n"
        "  \"switcher/hotkeys\": []\n"
        "  \"ascii_composer/good_old_caps_lock\": true\n"
        "  \"ascii_composer/switch_key/Shift_L\": noop\n"
        "  \"ascii_composer/switch_key/Shift_R\": noop\n"
        "  \"ascii_composer/switch_key/Caps_Lock\": noop\n"
        "  \"ascii_composer/switch_key/Control_L\": noop\n"
        "  \"ascii_composer/switch_key/Control_R\": noop\n";
    write_text_file(custom.string(), content);

    // Ensure Rime always stays in Chinese mode (ascii_mode=0) and honors Simplified/Traditional script choice.
    write_text_file((rime_dir / "rime_ice.custom.yaml").string(),
                    "patch:\n"
                    "  \"switches/@0/reset\": 0\n"
                    "  \"switches/@2/reset\": " +
                        std::string(traditional ? "1\n" : "0\n"));
    write_text_file((rime_dir / "luna_pinyin_simp.custom.yaml").string(),
                    "patch:\n"
                    "  \"switches/@0/reset\": 0\n"
                    "  \"switches/@2/reset\": " +
                        std::string(traditional ? "0\n" : "1\n"));
}

void ensure_rime_chinese_mode_dbus() {
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus) return;
    g_dbus_connection_call(bus, "org.fcitx.Fcitx5", "/rime", "org.fcitx.Fcitx.Rime1",
                           "SetAsciiMode", g_variant_new("(b)", FALSE), nullptr,
                           G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, nullptr, nullptr, nullptr);
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
    // The GNOME compatibility identifier also triggers an irrelevant Fcitx tip on labwc.
    if (hide_fcitx5_gnome_diagnosis()) {
        g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
        if (bus) {
            g_dbus_connection_call(bus, "org.fcitx.Fcitx5", "/controller", "org.fcitx.Fcitx.Controller1",
                                   "ReloadAddonConfig", g_variant_new("(s)", "notifications"), nullptr,
                                   G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, nullptr, nullptr, nullptr);
        }
    }

    ensure_chromium_ime_flags();
    write_fcitx5_global_config();
    write_fcitx5_rime_addon_conf();

    ImePrefs prefs = load_ime_prefs();
    std::string active_im = detect_active_ime_engine();
    bool has_pinyin = has_pinyin_addon_installed();
    bool has_ice = fs::exists(fs::path(home_dir()) / ".local/share/fcitx5/rime/rime_ice.schema.yaml", ec);
    write_rime_default_custom(has_ice, prefs.traditional);

    // Keep Simple: normalize profile strictly to English (keyboard-us) + Chinese (rime/pinyin).
    if (active_im == "none" || (active_im == "pinyin" && !has_pinyin)) {
        write_fcitx5_profile(default_ime_engine());
    }
    write_fcitx5_classicui(is_light, !fs::exists(fdir / "conf/classicui.conf", ec));

    if (has_executable("fcitx5-remote")) {
        std::string pg_out;
        int pg_rc = 1;
        if (run_argv_capture({"pgrep", "-x", "fcitx5"}, pg_out, pg_rc) && pg_rc == 0) {
            spawn_argv_detached({"fcitx5-remote", "-r"});
        }
    }
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

ImeStatusSnapshot query_ime_status() {
    ImeStatusSnapshot snap;
    snap.installed = has_executable("fcitx5");
    snap.traditional = load_ime_prefs().traditional;
    std::error_code ec;
    snap.rime_ice = fs::exists(fs::path(home_dir()) / ".local/share/fcitx5/rime/rime_ice.schema.yaml", ec);

    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (bus) {
        g_autoptr(GVariant) state_res = g_dbus_connection_call_sync(
            bus, "org.fcitx.Fcitx5", "/controller", "org.fcitx.Fcitx.Controller1", "State",
            nullptr, G_VARIANT_TYPE("(i)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 200, nullptr, nullptr);
        if (state_res) {
            snap.running = true;
            gint32 st = 0;
            g_variant_get(state_res, "(i)", &st);
            snap.active_zh = (st == 2);
            if (snap.active_zh) {
                g_autoptr(GVariant) ascii_res = g_dbus_connection_call_sync(
                    bus, "org.fcitx.Fcitx5", "/rime", "org.fcitx.Fcitx.Rime1", "IsAsciiMode",
                    nullptr, G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 150, nullptr, nullptr);
                if (ascii_res) {
                    gboolean is_ascii = FALSE;
                    g_variant_get(ascii_res, "(b)", &is_ascii);
                    if (is_ascii) snap.active_zh = false;
                }
            }
            return snap;
        }
    }

    if (snap.installed && has_executable("fcitx5-remote")) {
        std::string out;
        int rc = 1;
        if (run_argv_capture({"fcitx5-remote"}, out, rc) && rc == 0) {
            std::string trimmed = trim_copy(out);
            if (trimmed == "1" || trimmed == "2") {
                snap.running = true;
                snap.active_zh = (trimmed == "2");
            }
        }
    }
    return snap;
}

void set_ime_active_mode(AppState *state, bool chinese) {
    bool called_dbus = false;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (bus) {
        g_autoptr(GVariant) res = g_dbus_connection_call_sync(
            bus, "org.fcitx.Fcitx5", "/controller", "org.fcitx.Fcitx.Controller1",
            chinese ? "Activate" : "Deactivate", nullptr, nullptr,
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 300, nullptr, nullptr);
        if (res) {
            called_dbus = true;
            if (chinese) {
                ensure_rime_chinese_mode_dbus();
            }
        }
    }
    if (!called_dbus && has_executable("fcitx5-remote")) {
        std::string out;
        int rc = 1;
        run_argv_capture({"fcitx5-remote", chinese ? "-o" : "-c"}, out, rc);
        if (chinese) ensure_rime_chinese_mode_dbus();
    }
    native_shell_sync_telemetry(state);
    if (state && state->settings_view) {
        push_ime_state(state);
    }
}

void toggle_ime_active_mode(AppState *state) {
    ImeStatusSnapshot snap = query_ime_status();
    set_ime_active_mode(state, !snap.active_zh);
}

void set_ime_traditional(AppState *state, bool traditional) {
    ImePrefs p;
    p.traditional = traditional;
    save_ime_prefs(p);

    ImeStatusSnapshot cur = query_ime_status();
    bool was_zh = cur.active_zh;

    std::error_code ec;
    bool has_ice = fs::exists(fs::path(home_dir()) / ".local/share/fcitx5/rime/rime_ice.schema.yaml", ec);
    write_rime_default_custom(has_ice, traditional);

    if (has_executable("fcitx5")) {
        stop_fcitx5_sync();
        spawn_argv_detached({"sh", "-c", "setsid -f fcitx5 >/tmp/spinel-fcitx5.log 2>&1 </dev/null || true"});
    }
    append_app_log(std::string("[IME] 切换汉字输出模式为: ") + (traditional ? "繁体中文" : "简体中文"));

    if (state) {
        native_shell_sync_telemetry(state);
        if (state->settings_view) {
            push_ime_state(state);
            eval_js(state->settings_view,
                    std::string("window.__spinelImeFeedback && window.__spinelImeFeedback(true, \"") +
                        (traditional ? "已切换为繁体中文，按 Shift 或 Ctrl+Space 即可输入。"
                                     : "已切换为简体中文，按 Shift 或 Ctrl+Space 即可输入。") +
                        "\");");
        }
        if (was_zh) {
            g_timeout_add(450, +[](gpointer d) -> gboolean {
                auto *st = static_cast<AppState *>(d);
                if (st && !st->shutting_down.load()) {
                    set_ime_active_mode(st, true);
                }
                return G_SOURCE_REMOVE;
            }, state);
        }
    }
}

void push_ime_state(AppState *state) {
    if (!state || !state->settings_view) return;
    ImeStatusSnapshot snap = query_ime_status();
    bool installed = snap.installed;
    bool rime_ok = has_rime_addon_installed();
    bool has_pinyin = has_pinyin_addon_installed();
    bool has_configtool = has_executable("fcitx5-configtool");
    std::string engine = detect_active_ime_engine();

    std::ostringstream js;
    js << "window.__spinelImeState && window.__spinelImeState({"
       << "\"installed\":" << (installed ? "true" : "false") << ","
       << "\"running\":" << (snap.running ? "true" : "false") << ","
       << "\"active_zh\":" << (snap.active_zh ? "true" : "false") << ","
       << "\"traditional\":" << (snap.traditional ? "true" : "false") << ","
       << "\"rime_installed\":" << (rime_ok ? "true" : "false") << ","
       << "\"has_pinyin\":" << (has_pinyin ? "true" : "false") << ","
       << "\"has_configtool\":" << (has_configtool ? "true" : "false") << ","
       << "\"rime_ice\":" << (snap.rime_ice ? "true" : "false") << ","
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
    write_fcitx5_global_config();
    write_fcitx5_rime_addon_conf();
    ImePrefs prefs = load_ime_prefs();
    std::error_code ec;
    bool has_ice = fs::exists(fs::path(home_dir()) / ".local/share/fcitx5/rime/rime_ice.schema.yaml", ec);
    write_rime_default_custom(has_ice, prefs.traditional);
    if (detect_active_ime_engine() == "none") {
        write_fcitx5_profile(default_ime_engine());
    }
    spawn_argv_detached({"sh", "-c", "setsid -f fcitx5 >/tmp/spinel-fcitx5.log 2>&1 </dev/null || true"});
    append_app_log("[IME] 已重启 Fcitx5 输入法服务");
    if (state) {
        std::thread([state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            sys_internal::post_js_to_main(state, "window.__spinelImeFeedback && window.__spinelImeFeedback(true, \"输入法服务已重启，按 Shift 或 Ctrl+Space 即可流畅切换中英文。\");", false);
        }).detach();
    }
}

void set_ime_engine(AppState *state, const std::string &engine) {
    if (engine == "en" || engine == "zh") {
        set_ime_active_mode(state, engine == "zh");
        return;
    }
    std::error_code ec;
    bool has_ice = fs::exists(fs::path(home_dir()) / ".local/share/fcitx5/rime/rime_ice.schema.yaml", ec);
    bool trad = load_ime_prefs().traditional;
    if (engine == "rime") {
        if (!has_ice) {
            install_rime_ice_dict(state);
            return;
        }
        write_rime_default_custom(true, trad);
        stop_fcitx5_sync();
        write_fcitx5_profile("rime");
        append_app_log("[IME] 切换中文输入法方案为: rime (rime_ice)");
        restart_fcitx5_daemon(state);
        if (state && state->settings_view) push_ime_state(state);
        return;
    }

    // Built-in offline Chinese Pinyin: prefer fcitx5-chinese-addons if installed,
    // otherwise switch Rime cleanly to built-in Simplified Luna Pinyin (luna_pinyin_simp).
    std::string target = has_pinyin_addon_installed() ? "pinyin" : "rime";
    if (target == "rime") {
        write_rime_default_custom(false, trad);
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
    sys_internal::post_js_to_main(state, "window.__spinelImeFeedback && window.__spinelImeFeedback(true, \"正在下载雾凇拼音 (rime-ice) 词库，请稍候...\");", false);
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
            bool trad = load_ime_prefs().traditional;
            stop_fcitx5_sync();
            write_rime_default_custom(true, trad);
            write_fcitx5_profile("rime");
            spawn_argv_detached({"sh", "-c", "setsid -f fcitx5 >/tmp/spinel-fcitx5.log 2>&1 </dev/null || true"});
            append_app_log("[IME] 雾凇拼音 (rime-ice) 词库更新完成并已激活");
            sys_internal::post_js_to_main(state, "window.__spinelImeFeedback && window.__spinelImeFeedback(true, \"雾凇拼音 (rime-ice) 词库已就绪！按 Shift 或 Ctrl+Space 即可输入。\");", false);
        } else {
            append_app_log("[IME] 雾凇拼音下载失败 (code=" + std::to_string(rc) + "): " + out);
            sys_internal::post_js_to_main(state, "window.__spinelImeFeedback && window.__spinelImeFeedback(false, \"雾凇拼音下载失败（网络访问 GitHub 超时），可先开启代理后再试，或继续使用内置拼音。\");", false);
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
        << "    <keybind key=\"W-S-Left\">\n"
        << "      <action name=\"SendToDesktop\" to=\"left\" wrap=\"yes\" follow=\"yes\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"W-S-Right\">\n"
        << "      <action name=\"SendToDesktop\" to=\"right\" wrap=\"yes\" follow=\"yes\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"W-1\">\n"
        << "      <action name=\"GoToDesktop\" to=\"1\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"W-2\">\n"
        << "      <action name=\"GoToDesktop\" to=\"2\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"W-3\">\n"
        << "      <action name=\"GoToDesktop\" to=\"3\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"W-4\">\n"
        << "      <action name=\"GoToDesktop\" to=\"4\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"W-S-1\">\n"
        << "      <action name=\"SendToDesktop\" to=\"1\" follow=\"yes\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"W-S-2\">\n"
        << "      <action name=\"SendToDesktop\" to=\"2\" follow=\"yes\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"W-S-3\">\n"
        << "      <action name=\"SendToDesktop\" to=\"3\" follow=\"yes\" />\n"
        << "    </keybind>\n"
        << "    <keybind key=\"W-S-4\">\n"
        << "      <action name=\"SendToDesktop\" to=\"4\" follow=\"yes\" />\n"
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

/* ---------------- Dock Mode Configuration ---------------- */

bool is_valid_dock_mode(const std::string &mode) {
    return mode == "magnify" || mode == "fixed" || mode == "autohide";
}

DockConfig load_dock_config() {
    DockConfig cfg;
    fs::path p = fs::path(home_dir()) / ".config/spinelos/dock.conf";
    std::ifstream ifs(p);
    if (!ifs.is_open()) return cfg;
    std::string line;
    while (std::getline(ifs, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        if (k == "mode" && is_valid_dock_mode(v)) {
            cfg.mode = v;
        }
    }
    return cfg;
}

bool save_dock_config(const DockConfig &cfg) {
    if (!is_valid_dock_mode(cfg.mode)) return false;
    fs::path p = fs::path(home_dir()) / ".config/spinelos/dock.conf";
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    return write_text_file(p.string(), "mode=" + cfg.mode + "\n");
}

void push_dock_config(AppState *state) {
    if (!state || !state->settings_view) return;
    DockConfig cfg = load_dock_config();
    std::ostringstream js;
    js << "window.__spinelDockConfig && window.__spinelDockConfig({"
       << "\"mode\":\"" << json_escape(cfg.mode) << "\""
       << "});";
    eval_js(state->settings_view, js.str());
}

void set_dock_mode(AppState *state, const std::string &mode_raw) {
    std::string mode = trim_copy(mode_raw);
    if (!is_valid_dock_mode(mode)) {
        mode = "magnify";
    }
    DockConfig cfg;
    cfg.mode = mode;
    save_dock_config(cfg);
    append_app_log("[Dock] 切换 Dock 栏显示模式为: " + mode);
    push_dock_config(state);
    native_shell_sync_dock(state);
}
