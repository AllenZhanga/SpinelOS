#pragma once

#include "app_state.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct ThemeSpec {
    std::string id;
    std::string name;
    std::string subtitle;
    bool is_light = false;
    bool builtin = true;

    // Desktop wallpaper gradient + glows (0xRRGGBB)
    uint32_t wp_c0 = 0x071124;
    uint32_t wp_c1 = 0x11264A;
    uint32_t wp_c2 = 0x09152B;
    uint32_t wp_glow1 = 0x38BDF8;
    uint32_t wp_glow2 = 0x6366F1;

    // Native Shell surfaces (CSS rgba/hex strings)
    std::string bar_bg = "rgba(14, 22, 40, 0.72)";
    std::string bar_border = "rgba(255, 255, 255, 0.12)";
    std::string bar_fg = "#f8fafc";
    std::string dock_top = "rgba(44, 58, 86, 0.62)";
    std::string dock_bot = "rgba(22, 30, 48, 0.68)";
    std::string dock_border = "rgba(255, 255, 255, 0.28)";
    std::string dock_tip_bg = "rgba(18, 25, 42, 0.94)";
    std::string dock_tip_fg = "#f8fafc";
    std::string cc_top = "rgba(54, 68, 96, 0.92)";
    std::string cc_bot = "rgba(30, 40, 62, 0.95)";
    std::string cc_border = "rgba(255, 255, 255, 0.32)";
    std::string cc_accent = "#0a84ff";
    std::string popover_bg = "rgba(16, 23, 40, 0.95)";

    // WebKit UI tokens
    std::string ui_bg = "#081020";
    std::string ui_win_bg = "radial-gradient(circle at 18% 15%, #132548 0%, #081020 62%)";
    std::string ui_toolbar_bg = "rgba(11, 18, 34, 0.84)";
    std::string ui_surface = "rgba(16, 25, 45, 0.78)";
    std::string ui_card = "rgba(20, 31, 56, 0.72)";
    std::string ui_input_bg = "#0c1528";
    std::string ui_border = "rgba(255, 255, 255, 0.13)";
    std::string ui_text = "#f1f5f9";
    std::string ui_muted = "#94a3b8";
    std::string ui_accent = "#38bdf8";
    std::string ui_accent2 = "#818cf8";
};

std::vector<ThemeSpec> list_available_themes();
ThemeSpec get_active_theme();
std::string get_active_theme_id();
void set_active_theme(AppState *state, const std::string &theme_id);
void push_theme_state(AppState *state);
void export_custom_theme_template();
