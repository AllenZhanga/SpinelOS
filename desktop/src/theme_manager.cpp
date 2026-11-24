#include "theme_manager.hpp"
#include "common.hpp"
#include "native_shell.hpp"
#include "system_control.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace {

uint32_t parse_hex_color(const std::string &s, uint32_t fallback) {
    std::string clean = trim_copy(s);
    if (!clean.empty() && clean.front() == '#') clean.erase(0, 1);
    if (clean.rfind("0x", 0) == 0 || clean.rfind("0X", 0) == 0) clean.erase(0, 2);
    if (clean.size() != 6) return fallback;
    char *endp = nullptr;
    unsigned long val = std::strtoul(clean.c_str(), &endp, 16);
    if (!endp || *endp != '\0') return fallback;
    return static_cast<uint32_t>(val & 0xFFFFFFu);
}

std::string hex6(uint32_t rgb) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%06x", rgb & 0xFFFFFFu);
    return buf;
}

std::vector<ThemeSpec> builtin_themes() {
    ThemeSpec crystal;
    crystal.id = "crystal-aurora";
    crystal.name = "尖晶石深蓝极光";
    crystal.subtitle = "通透深海蓝磨砂玻璃 · 平衡对比度与高光";
    crystal.is_light = false;
    crystal.wp_c0 = 0x071124;
    crystal.wp_c1 = 0x11264A;
    crystal.wp_c2 = 0x09152B;
    crystal.wp_glow1 = 0x38BDF8;
    crystal.wp_glow2 = 0x6366F1;
    crystal.bar_bg = "rgba(14, 22, 40, 0.72)";
    crystal.bar_border = "rgba(255, 255, 255, 0.12)";
    crystal.bar_fg = "#f8fafc";
    crystal.dock_top = "rgba(44, 58, 86, 0.62)";
    crystal.dock_bot = "rgba(22, 30, 48, 0.68)";
    crystal.dock_border = "rgba(255, 255, 255, 0.28)";
    crystal.dock_tip_bg = "rgba(18, 25, 42, 0.94)";
    crystal.dock_tip_fg = "#f8fafc";
    crystal.cc_top = "rgba(54, 68, 96, 0.92)";
    crystal.cc_bot = "rgba(30, 40, 62, 0.95)";
    crystal.cc_border = "rgba(255, 255, 255, 0.32)";
    crystal.cc_accent = "#0a84ff";
    crystal.popover_bg = "rgba(16, 23, 40, 0.95)";
    crystal.ui_bg = "#081020";
    crystal.ui_win_bg = "radial-gradient(circle at 18% 15%, #132548 0%, #081020 62%)";
    crystal.ui_toolbar_bg = "rgba(11, 18, 34, 0.84)";
    crystal.ui_surface = "rgba(16, 25, 45, 0.78)";
    crystal.ui_card = "rgba(20, 31, 56, 0.72)";
    crystal.ui_input_bg = "#0c1528";
    crystal.ui_border = "rgba(255, 255, 255, 0.13)";
    crystal.ui_text = "#f1f5f9";
    crystal.ui_muted = "#94a3b8";
    crystal.ui_accent = "#38bdf8";
    crystal.ui_accent2 = "#818cf8";

    ThemeSpec light;
    light.id = "tahoe-light";
    light.name = "macOS 浅色液态玻璃（默认）";
    light.subtitle = "白昼冰晶透亮质感 · 默认系统风格";
    light.is_light = true;
    light.wp_c0 = 0xDCEAFE;
    light.wp_c1 = 0xEEF2FF;
    light.wp_c2 = 0xE0F2FE;
    light.wp_glow1 = 0x60A5FA;
    light.wp_glow2 = 0xA78BFA;
    light.bar_bg = "rgba(255, 255, 255, 0.76)";
    light.bar_border = "rgba(15, 23, 42, 0.10)";
    light.bar_fg = "#0f172a";
    light.dock_top = "rgba(255, 255, 255, 0.74)";
    light.dock_bot = "rgba(234, 242, 255, 0.80)";
    light.dock_border = "rgba(255, 255, 255, 0.90)";
    light.dock_tip_bg = "rgba(255, 255, 255, 0.96)";
    light.dock_tip_fg = "#0f172a";
    light.cc_top = "rgba(255, 255, 255, 0.90)";
    light.cc_bot = "rgba(241, 245, 249, 0.94)";
    light.cc_border = "rgba(15, 23, 42, 0.14)";
    light.cc_accent = "#0284c7";
    light.popover_bg = "rgba(255, 255, 255, 0.96)";
    light.ui_bg = "#f1f5f9";
    light.ui_win_bg = "radial-gradient(circle at 18% 15%, #e0f2fe 0%, #f1f5f9 65%)";
    light.ui_toolbar_bg = "rgba(255, 255, 255, 0.88)";
    light.ui_surface = "rgba(255, 255, 255, 0.86)";
    light.ui_card = "rgba(255, 255, 255, 0.94)";
    light.ui_input_bg = "#ffffff";
    light.ui_border = "rgba(15, 23, 42, 0.13)";
    light.ui_text = "#0f172a";
    light.ui_muted = "#475569";
    light.ui_accent = "#0284c7";
    light.ui_accent2 = "#4f46e5";

    ThemeSpec obsidian;
    obsidian.id = "obsidian-neon";
    obsidian.name = "黑曜星云霓虹";
    obsidian.subtitle = "纯黑 OLED 电竞暗夜 · 紫粉霓虹高光";
    obsidian.is_light = false;
    obsidian.wp_c0 = 0x070510;
    obsidian.wp_c1 = 0x1B0F2E;
    obsidian.wp_c2 = 0x090713;
    obsidian.wp_glow1 = 0xA855F7;
    obsidian.wp_glow2 = 0xEC4899;
    obsidian.bar_bg = "rgba(14, 10, 24, 0.78)";
    obsidian.bar_border = "rgba(216, 180, 254, 0.14)";
    obsidian.bar_fg = "#faf5ff";
    obsidian.dock_top = "rgba(46, 30, 72, 0.64)";
    obsidian.dock_bot = "rgba(20, 14, 34, 0.74)";
    obsidian.dock_border = "rgba(216, 180, 254, 0.30)";
    obsidian.dock_tip_bg = "rgba(22, 15, 36, 0.95)";
    obsidian.dock_tip_fg = "#faf5ff";
    obsidian.cc_top = "rgba(56, 38, 86, 0.92)";
    obsidian.cc_bot = "rgba(28, 19, 46, 0.95)";
    obsidian.cc_border = "rgba(216, 180, 254, 0.30)";
    obsidian.cc_accent = "#a855f7";
    obsidian.popover_bg = "rgba(20, 14, 34, 0.96)";
    obsidian.ui_bg = "#070510";
    obsidian.ui_win_bg = "radial-gradient(circle at 20% 15%, #231242 0%, #070510 62%)";
    obsidian.ui_toolbar_bg = "rgba(14, 10, 24, 0.88)";
    obsidian.ui_surface = "rgba(22, 15, 36, 0.80)";
    obsidian.ui_card = "rgba(29, 20, 48, 0.76)";
    obsidian.ui_input_bg = "#120c22";
    obsidian.ui_border = "rgba(216, 180, 254, 0.16)";
    obsidian.ui_text = "#f5f3ff";
    obsidian.ui_muted = "#a78bfa";
    obsidian.ui_accent = "#c084fc";
    obsidian.ui_accent2 = "#f472b6";

    ThemeSpec emerald;
    emerald.id = "emerald-nordic";
    emerald.name = "森境翡翠极光";
    emerald.subtitle = "北欧深森青绿调 · 柔和护眼沉浸感";
    emerald.is_light = false;
    emerald.wp_c0 = 0x05110E;
    emerald.wp_c1 = 0x0B2622;
    emerald.wp_c2 = 0x061318;
    emerald.wp_glow1 = 0x10B981;
    emerald.wp_glow2 = 0x0EA5E9;
    emerald.bar_bg = "rgba(8, 22, 19, 0.76)";
    emerald.bar_border = "rgba(167, 243, 208, 0.12)";
    emerald.bar_fg = "#ecfdf5";
    emerald.dock_top = "rgba(22, 56, 48, 0.62)";
    emerald.dock_bot = "rgba(10, 28, 24, 0.72)";
    emerald.dock_border = "rgba(167, 243, 208, 0.28)";
    emerald.dock_tip_bg = "rgba(10, 28, 24, 0.95)";
    emerald.dock_tip_fg = "#ecfdf5";
    emerald.cc_top = "rgba(24, 62, 54, 0.92)";
    emerald.cc_bot = "rgba(12, 34, 30, 0.95)";
    emerald.cc_border = "rgba(167, 243, 208, 0.28)";
    emerald.cc_accent = "#10b981";
    emerald.popover_bg = "rgba(10, 28, 24, 0.96)";
    emerald.ui_bg = "#05110e";
    emerald.ui_win_bg = "radial-gradient(circle at 20% 15%, #0c2e28 0%, #05110e 62%)";
    emerald.ui_toolbar_bg = "rgba(8, 22, 19, 0.88)";
    emerald.ui_surface = "rgba(12, 32, 28, 0.80)";
    emerald.ui_card = "rgba(16, 42, 36, 0.76)";
    emerald.ui_input_bg = "#081b17";
    emerald.ui_border = "rgba(167, 243, 208, 0.14)";
    emerald.ui_text = "#ecfdf5";
    emerald.ui_muted = "#6ee7b7";
    emerald.ui_accent = "#34d399";
    emerald.ui_accent2 = "#2dd4bf";

    return {light, crystal, obsidian, emerald};
}

bool parse_theme_file(const fs::path &file, const ThemeSpec &base_template, ThemeSpec &out) {
    std::ifstream ifs(file);
    if (!ifs.is_open()) return false;
    out = base_template;
    out.builtin = false;
    out.id = file.stem().string();
    out.name = out.id;
    out.subtitle = "用户自定义主题 (" + file.filename().string() + ")";

    std::string line;
    while (std::getline(ifs, line)) {
        line = trim_copy(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));

        if (k == "name" && !v.empty()) out.name = v;
        else if (k == "subtitle") out.subtitle = v;
        else if (k == "is_light") out.is_light = (v == "true" || v == "1");
        else if (k == "wp_c0") out.wp_c0 = parse_hex_color(v, out.wp_c0);
        else if (k == "wp_c1") out.wp_c1 = parse_hex_color(v, out.wp_c1);
        else if (k == "wp_c2") out.wp_c2 = parse_hex_color(v, out.wp_c2);
        else if (k == "wp_glow1") out.wp_glow1 = parse_hex_color(v, out.wp_glow1);
        else if (k == "wp_glow2") out.wp_glow2 = parse_hex_color(v, out.wp_glow2);
        else if (k == "bar_bg") out.bar_bg = v;
        else if (k == "bar_border") out.bar_border = v;
        else if (k == "bar_fg") out.bar_fg = v;
        else if (k == "dock_top") out.dock_top = v;
        else if (k == "dock_bot") out.dock_bot = v;
        else if (k == "dock_border") out.dock_border = v;
        else if (k == "dock_tip_bg") out.dock_tip_bg = v;
        else if (k == "dock_tip_fg") out.dock_tip_fg = v;
        else if (k == "cc_top") out.cc_top = v;
        else if (k == "cc_bot") out.cc_bot = v;
        else if (k == "cc_border") out.cc_border = v;
        else if (k == "cc_accent") out.cc_accent = v;
        else if (k == "popover_bg") out.popover_bg = v;
        else if (k == "ui_bg") out.ui_bg = v;
        else if (k == "ui_win_bg") out.ui_win_bg = v;
        else if (k == "ui_toolbar_bg") out.ui_toolbar_bg = v;
        else if (k == "ui_surface") out.ui_surface = v;
        else if (k == "ui_card") out.ui_card = v;
        else if (k == "ui_input_bg") out.ui_input_bg = v;
        else if (k == "ui_border") out.ui_border = v;
        else if (k == "ui_text") out.ui_text = v;
        else if (k == "ui_muted") out.ui_muted = v;
        else if (k == "ui_accent") out.ui_accent = v;
        else if (k == "ui_accent2") out.ui_accent2 = v;
    }
    return true;
}

std::string theme_to_json(const ThemeSpec &t) {
    std::ostringstream oss;
    oss << "{"
        << "\"id\":\"" << json_escape(t.id) << "\","
        << "\"name\":\"" << json_escape(t.name) << "\","
        << "\"subtitle\":\"" << json_escape(t.subtitle) << "\","
        << "\"is_light\":" << (t.is_light ? "true" : "false") << ","
        << "\"builtin\":" << (t.builtin ? "true" : "false") << ","
        << "\"wp_c0\":\"" << hex6(t.wp_c0) << "\","
        << "\"wp_c1\":\"" << hex6(t.wp_c1) << "\","
        << "\"wp_glow1\":\"" << hex6(t.wp_glow1) << "\","
        << "\"wp_glow2\":\"" << hex6(t.wp_glow2) << "\","
        << "\"ui_bg\":\"" << json_escape(t.ui_bg) << "\","
        << "\"ui_win_bg\":\"" << json_escape(t.ui_win_bg) << "\","
        << "\"ui_toolbar_bg\":\"" << json_escape(t.ui_toolbar_bg) << "\","
        << "\"ui_surface\":\"" << json_escape(t.ui_surface) << "\","
        << "\"ui_card\":\"" << json_escape(t.ui_card) << "\","
        << "\"ui_input_bg\":\"" << json_escape(t.ui_input_bg) << "\","
        << "\"ui_border\":\"" << json_escape(t.ui_border) << "\","
        << "\"ui_text\":\"" << json_escape(t.ui_text) << "\","
        << "\"ui_muted\":\"" << json_escape(t.ui_muted) << "\","
        << "\"ui_accent\":\"" << json_escape(t.ui_accent) << "\","
        << "\"ui_accent2\":\"" << json_escape(t.ui_accent2) << "\""
        << "}";
    return oss.str();
}

}  // namespace

std::vector<ThemeSpec> list_available_themes() {
    std::vector<ThemeSpec> list = builtin_themes();
    const ThemeSpec &base = list.front();

    std::vector<fs::path> dirs = {
        fs::path("/usr/share/spinel-shell/themes"),
        fs::path(home_dir()) / ".config/spinelos/themes"
    };
    std::error_code ec;
    for (const auto &d : dirs) {
        if (!fs::is_directory(d, ec)) continue;
        for (const auto &entry : fs::directory_iterator(d, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".theme") continue;
            ThemeSpec custom;
            if (parse_theme_file(entry.path(), base, custom)) {
                bool replaced = false;
                for (auto &t : list) {
                    if (t.id == custom.id) { t = custom; replaced = true; break; }
                }
                if (!replaced) list.push_back(std::move(custom));
            }
        }
    }
    return list;
}

std::string get_active_theme_id() {
    std::string id = trim_copy(read_first_line(
        (fs::path(home_dir()) / ".config/spinelos/theme.conf").string(), ""
    ));
    if (!id.empty()) return id;
    // Migrate legacy wallpaper.conf only if user explicitly picked a non-default legacy theme
    std::string legacy = trim_copy(read_first_line(
        (fs::path(home_dir()) / ".config/spinelos/wallpaper.conf").string(), ""
    ));
    if (legacy == "obsidian-purple") return "obsidian-neon";
    if (legacy == "emerald-aurora") return "emerald-nordic";
    return "tahoe-light";
}

ThemeSpec get_active_theme() {
    std::string active = get_active_theme_id();
    auto list = list_available_themes();
    for (const auto &t : list) {
        if (t.id == active) return t;
    }
    return list.front();
}

void sync_labwc_window_theme(bool is_light) {
    write_labwc_rc(is_light, load_shortcuts_config());
}

void set_active_theme(AppState *state, const std::string &theme_id) {
    auto list = list_available_themes();
    bool valid = false;
    for (const auto &t : list) {
        if (t.id == theme_id) { valid = true; break; }
    }
    if (!valid) {
        std::fprintf(stderr, "[Theme] 忽略无效主题 ID: '%s'\n", theme_id.c_str());
        append_app_log("[Theme] 忽略无效主题 ID: " + theme_id);
        return;
    }

    fs::path theme_file = fs::path(home_dir()) / ".config/spinelos/theme.conf";
    write_text_file(theme_file.string(), theme_id + "\n");
    ThemeSpec active = get_active_theme();
    std::fprintf(stderr, "[Theme] 切换主题 request='%s' -> active='%s' (is_light=%s)\n",
                 theme_id.c_str(), active.id.c_str(), active.is_light ? "true" : "false");
    append_app_log("[Theme] 切换主题 -> " + active.id + (active.is_light ? " (浅色)" : " (深色)"));
    sync_labwc_window_theme(active.is_light);
    sync_fcitx5_theme(active.is_light);
    sync_terminal_theme(active.is_light);
    sync_notification_theme(active.is_light);
    push_theme_state(state);
    native_shell_sync_theme(state);
}

void push_theme_state(AppState *state) {
    if (!state) return;
    ThemeSpec active = get_active_theme();
    auto list = list_available_themes();

    std::ostringstream js;
    js << "window.__spinelThemeData && window.__spinelThemeData({"
       << "\"active\":" << theme_to_json(active) << ","
       << "\"themes\":[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i > 0) js << ",";
        js << theme_to_json(list[i]);
    }
    js << "]});";
    std::string script = js.str();
#if SPINEL_HAS_WEBKIT
    const GdkRGBA bg = active.is_light
        ? GdkRGBA{0.96f, 0.965f, 0.98f, 1.0f}
        : GdkRGBA{0.031f, 0.043f, 0.071f, 1.0f};
    auto sync_view = [&](WebKitWebView *v) {
        if (!v) return;
        webkit_web_view_set_background_color(v, &bg);
        eval_js(v, script);
    };
    sync_view(state->games_view);
    sync_view(state->store_view);
    sync_view(state->settings_view);
    sync_view(state->installer_view);
#else
    (void)script;
#endif
}

void export_custom_theme_template() {
    std::error_code ec;
    fs::path dir = fs::path(home_dir()) / ".config/spinelos/themes";
    fs::create_directories(dir, ec);
    fs::path target = dir / "custom-sample.theme";
    if (!fs::exists(target, ec)) {
        std::ofstream ofs(target);
        if (ofs.is_open()) {
            ofs << "# SpinelOS 可扩展主题文件 (放入 ~/.config/spinelos/themes/*.theme 即可生效)\n"
                << "name=我的自定义主题\n"
                << "subtitle=基于深海琥珀扩展的自定义配色\n"
                << "is_light=false\n"
                << "wp_c0=#120b08\n"
                << "wp_c1=#261611\n"
                << "wp_c2=#0d0a14\n"
                << "wp_glow1=#f59e0b\n"
                << "wp_glow2=#f43f5e\n"
                << "bar_bg=rgba(22, 13, 10, 0.76)\n"
                << "bar_fg=#fff7ed\n"
                << "dock_top=rgba(64, 38, 28, 0.62)\n"
                << "dock_bot=rgba(28, 16, 14, 0.70)\n"
                << "dock_border=rgba(253, 186, 116, 0.30)\n"
                << "ui_bg=#120b08\n"
                << "ui_win_bg=radial-gradient(circle at 20% 15%, #2f1810 0%, #120b08 62%)\n"
                << "ui_toolbar_bg=rgba(22, 13, 10, 0.88)\n"
                << "ui_surface=rgba(32, 19, 15, 0.80)\n"
                << "ui_card=rgba(42, 25, 20, 0.76)\n"
                << "ui_input_bg=#1c110d\n"
                << "ui_border=rgba(253, 186, 116, 0.18)\n"
                << "ui_text=#fff7ed\n"
                << "ui_muted=#fdba74\n"
                << "ui_accent=#f59e0b\n"
                << "ui_accent2=#fb7185\n";
        }
    }
    spawn_argv_detached({"thunar", dir.string()});
}
