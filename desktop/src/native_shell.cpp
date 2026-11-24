#include "native_shell.hpp"
#include "common.hpp"
#include "desktop_apps.hpp"
#include "steam_library.hpp"
#include "system_control.hpp"
#include "theme_manager.hpp"
#include "window_tracker.hpp"

#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace {

struct DockItem {
    std::string label;
    std::string cmd;
    std::string icon_path;
    std::string fallback_kind;
    uint64_t win_id = 0;
    bool running = false;
    bool activated = false;
    bool minimized = false;
    bool closable = false;
    double anim_size = 48.0;
    double anim_lift = 0.0;
};

struct CcSliderCtx {
    AppState *state = nullptr;
    bool is_volume = true;
};

}  // namespace

struct NativeShellState {
    AppState *app_state = nullptr;

    // Top Bar
    GtkWindow *bar_win = nullptr;
    GtkWidget *bar_active_lbl = nullptr;
    GtkWidget *bar_app_menus = nullptr;
    std::string bar_menu_key;
    GtkWidget *bar_mem_lbl = nullptr;
    GtkWidget *bar_bat_lbl = nullptr;
    GtkWidget *clip_popover = nullptr;
    GtkWidget *clip_list_box = nullptr;
    GtkWidget *bar_net_btn = nullptr;
    GtkWidget *bar_cc_btn = nullptr;
    GtkWidget *bar_clock_lbl = nullptr;
    guint clock_timer_id = 0;

    // Desktop
    GtkWindow *desktop_win = nullptr;
    GtkWidget *desktop_overlay = nullptr;
    GtkWidget *desktop_bg_da = nullptr;
    GtkWidget *desktop_flow = nullptr;
    GtkWidget *desktop_popover = nullptr;
    std::string wallpaper = "crystal-blue";

    // Dock
    GtkWindow *dock_win = nullptr;
    GtkWidget *dock_da = nullptr;
    std::vector<DockItem> pinned_items;
    std::vector<DockItem> extra_items;
    int hover_index = -1;  // 0..pinned-1, or pinned..pinned+extra-1
    guint dock_anim_timer = 0;
    int dock_width = 580;

    // Control Center
    GtkWindow *cc_backdrop_win = nullptr;
    GtkWindow *cc_win = nullptr;
    bool cc_was_active = false;
    GtkWidget *cc_net_val = nullptr;
    GtkWidget *cc_gpu_badge = nullptr;
    GtkWidget *cc_mem_val = nullptr;
    GtkWidget *cc_gm_circle = nullptr;
    GtkWidget *cc_gm_val = nullptr;
    GtkWidget *cc_mh_circle = nullptr;
    GtkWidget *cc_mh_val = nullptr;
    GtkWidget *cc_bright_pct = nullptr;
    GtkWidget *cc_bright_scale = nullptr;
    GtkWidget *cc_vol_pct = nullptr;
    GtkWidget *cc_vol_scale = nullptr;
    bool cc_updating_sliders = false;

    // Cached states
    ThemeSpec theme;
    GtkCssProvider *css_provider = nullptr;
    TelemetrySnapshot telemetry;
    std::vector<DesktopAppInfo> desktop_apps;
    std::map<std::string, cairo_surface_t *> surface_cache;
    std::vector<CcSliderCtx *> slider_ctxs;
};

namespace {

void cairo_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    double rad = std::min({r, w * 0.5, h * 0.5});
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - rad, y + rad, rad, -M_PI_2, 0.0);
    cairo_arc(cr, x + w - rad, y + h - rad, rad, 0.0, M_PI_2);
    cairo_arc(cr, x + rad, y + h - rad, rad, M_PI_2, M_PI);
    cairo_arc(cr, x + rad, y + rad, rad, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
}

std::string to_lower_copy(std::string s) {
    for (char &ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

bool contains_ci(const std::string &hay, const std::string &needle) {
    return to_lower_copy(hay).find(to_lower_copy(needle)) != std::string::npos;
}

std::string match_app_kind(const std::string &s) {
    std::string l = to_lower_copy(s);
    if (l.find("steam") != std::string::npos) return "steam";
    if (l.find("gamescope") != std::string::npos) return "gamepad";
    if (l.find("chromium") != std::string::npos || l.find("chrome") != std::string::npos ||
        l.find("firefox") != std::string::npos) return "browser";
    if (l.find("thunar") != std::string::npos || l.find("files") != std::string::npos) return "files";
    if (l.find("ptyxis") != std::string::npos || l.find("foot") != std::string::npos || l.find("terminal") != std::string::npos) return "terminal";
    if (l.find("应用商店") != std::string::npos || l.find("app-store") != std::string::npos) return "store";
    if (l.find("游戏与启动中心") != std::string::npos || l.find("app-games") != std::string::npos) return "games";
    if (l.find("系统设置") != std::string::npos || l.find("settings") != std::string::npos) return "settings";
    if (l.find("安装") != std::string::npos || l.find("installer") != std::string::npos) return "installer";
    return "app";
}

std::string find_app_icon_path(const std::vector<DesktopAppInfo> &apps,
                               const std::vector<std::string> &keys) {
    for (const auto &k : keys) {
        std::string lk = to_lower_copy(k);
        if (lk.empty()) continue;
        for (const auto &a : apps) {
            if (!a.icon_path.empty() && to_lower_copy(a.id) == lk) return a.icon_path;
        }
    }
    for (const auto &k : keys) {
        std::string lk = to_lower_copy(k);
        if (lk.empty()) continue;
        for (const auto &a : apps) {
            if (a.icon_path.empty()) continue;
            std::string hay = to_lower_copy(a.id + " " + a.icon_name + " " + a.name);
            if (hay.find(lk) != std::string::npos) return a.icon_path;
        }
    }
    return "";
}

cairo_surface_t *pixbuf_to_cairo_surface(GdkPixbuf *pb) {
    if (!pb) return nullptr;
    int pw = gdk_pixbuf_get_width(pb);
    int ph = gdk_pixbuf_get_height(pb);
    int n_ch = gdk_pixbuf_get_n_channels(pb);
    int rowstride = gdk_pixbuf_get_rowstride(pb);
    const guchar *src = gdk_pixbuf_read_pixels(pb);

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return nullptr;
    }
    unsigned char *dst = cairo_image_surface_get_data(surf);
    int dst_stride = cairo_image_surface_get_stride(surf);

    for (int y = 0; y < ph; ++y) {
        const guchar *srow = src + y * rowstride;
        uint32_t *drow = reinterpret_cast<uint32_t *>(dst + y * dst_stride);
        for (int x = 0; x < pw; ++x) {
            uint32_t r = srow[x * n_ch + 0];
            uint32_t g = srow[x * n_ch + 1];
            uint32_t b = srow[x * n_ch + 2];
            uint32_t a = (n_ch >= 4) ? srow[x * n_ch + 3] : 255u;
            uint32_t pr = (r * a + 127u) / 255u;
            uint32_t pg = (g * a + 127u) / 255u;
            uint32_t pb_v = (b * a + 127u) / 255u;
            drow[x] = (a << 24) | (pr << 16) | (pg << 8) | pb_v;
        }
    }
    cairo_surface_mark_dirty(surf);
    return surf;
}

cairo_surface_t *get_cached_surface(NativeShellState *ns, const std::string &path, int target_px) {
    if (!ns || path.empty() || target_px <= 0) return nullptr;
    std::string key = path + "@" + std::to_string(target_px);
    auto it = ns->surface_cache.find(key);
    if (it != ns->surface_cache.end()) return it->second;

    GdkPixbuf *pb = nullptr;
    if (contains_ci(path, ".svg")) {
        pb = gdk_pixbuf_new_from_file_at_scale(path.c_str(), target_px, target_px, TRUE, nullptr);
    } else {
        GdkPixbuf *raw = gdk_pixbuf_new_from_file(path.c_str(), nullptr);
        if (raw) {
            int rw = gdk_pixbuf_get_width(raw);
            int rh = gdk_pixbuf_get_height(raw);
            if (rw == target_px && rh == target_px) {
                pb = raw;
            } else {
                double aspect = (rh > 0) ? static_cast<double>(rw) / rh : 1.0;
                int dw = aspect >= 1.0 ? target_px : std::max(1, static_cast<int>(std::round(target_px * aspect)));
                int dh = aspect >= 1.0 ? std::max(1, static_cast<int>(std::round(target_px / aspect))) : target_px;
                pb = gdk_pixbuf_scale_simple(raw, dw, dh, GDK_INTERP_HYPER);
                g_object_unref(raw);
            }
        }
    }
    cairo_surface_t *surf = pixbuf_to_cairo_surface(pb);
    if (pb) g_object_unref(pb);
    ns->surface_cache[key] = surf;
    return surf;
}

void set_rgb_hex(cairo_pattern_t *pat, double offset, uint32_t hex, double alpha = 1.0) {
    double r = ((hex >> 16) & 0xFF) / 255.0;
    double g = ((hex >> 8) & 0xFF) / 255.0;
    double b = (hex & 0xFF) / 255.0;
    cairo_pattern_add_color_stop_rgba(pat, offset, r, g, b, alpha);
}

void parse_css_rgba(const std::string &css, double &r, double &g, double &b, double &a) {
    r = 0.1; g = 0.14; b = 0.22; a = 0.85;
    std::string s = trim_copy(css);
    if (!s.empty() && s.front() == '#' && s.size() >= 7) {
        unsigned long hex = std::strtoul(s.substr(1, 6).c_str(), nullptr, 16);
        r = ((hex >> 16) & 0xFF) / 255.0;
        g = ((hex >> 8) & 0xFF) / 255.0;
        b = (hex & 0xFF) / 255.0;
        a = 1.0;
        return;
    }
    if (s.rfind("rgba(", 0) == 0 || s.rfind("rgb(", 0) == 0) {
        auto lp = s.find('(');
        auto rp = s.rfind(')');
        if (lp != std::string::npos && rp != std::string::npos && rp > lp) {
            double vals[4]{20, 28, 44, 0.85};
            int idx = 0;
            const char *p = s.c_str() + lp + 1;
            while (*p && idx < 4) {
                while (*p == ' ' || *p == ',') ++p;
                if (!*p || *p == ')') break;
                char *endp = nullptr;
                vals[idx++] = std::strtod(p, &endp);
                if (endp == p) break;
                p = endp;
            }
            r = std::clamp(vals[0] / 255.0, 0.0, 1.0);
            g = std::clamp(vals[1] / 255.0, 0.0, 1.0);
            b = std::clamp(vals[2] / 255.0, 0.0, 1.0);
            a = (idx >= 4) ? std::clamp(vals[3], 0.0, 1.0) : 1.0;
        }
    }
}

void draw_system_squircle(cairo_t *cr, double x, double y, double sz, const std::string &kind) {
    double rad = sz * 0.235;
    cairo_save(cr);

    // Subtle soft drop shadow below squircle
    cairo_rounded_rect(cr, x, y + sz * 0.04, sz, sz, rad);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.34);
    cairo_fill(cr);

    // Gradient body
    cairo_pattern_t *bg = cairo_pattern_create_linear(x, y, x + sz, y + sz);
    if (kind == "store") {
        set_rgb_hex(bg, 0.0, 0x38BDF8);
        set_rgb_hex(bg, 0.55, 0x0EA5E9);
        set_rgb_hex(bg, 1.0, 0x1D4ED8);
    } else if (kind == "gamepad") {
        set_rgb_hex(bg, 0.0, 0xF43F5E);
        set_rgb_hex(bg, 0.55, 0xA855F7);
        set_rgb_hex(bg, 1.0, 0x6366F1);
    } else if (kind == "files") {
        set_rgb_hex(bg, 0.0, 0x7DD3FC);
        set_rgb_hex(bg, 0.55, 0x38BDF8);
        set_rgb_hex(bg, 1.0, 0x0284C7);
    } else if (kind == "terminal") {
        set_rgb_hex(bg, 0.0, 0xE2E8F0);
        set_rgb_hex(bg, 1.0, 0x94A3B8);
    } else if (kind == "settings") {
        set_rgb_hex(bg, 0.0, 0xE5E7EB);
        set_rgb_hex(bg, 1.0, 0x9CA3AF);
    } else if (kind == "installer") {
        set_rgb_hex(bg, 0.0, 0x34D399);
        set_rgb_hex(bg, 1.0, 0x059669);
    } else {
        // "games" / "app"
        set_rgb_hex(bg, 0.0, 0x38BDF8);
        set_rgb_hex(bg, 0.55, 0x6366F1);
        set_rgb_hex(bg, 1.0, 0x4338CA);
    }
    cairo_rounded_rect(cr, x, y, sz, sz, rad);
    cairo_set_source(cr, bg);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(bg);

    // Top sheen
    cairo_pattern_t *sheen = cairo_pattern_create_linear(x, y, x, y + sz);
    cairo_pattern_add_color_stop_rgba(sheen, 0.0, 1.0, 1.0, 1.0, 0.25);
    cairo_pattern_add_color_stop_rgba(sheen, 0.45, 1.0, 1.0, 1.0, 0.02);
    cairo_pattern_add_color_stop_rgba(sheen, 1.0, 0.0, 0.0, 0.0, 0.12);
    cairo_set_source(cr, sheen);
    cairo_fill(cr);
    cairo_pattern_destroy(sheen);

    // Crisp rim highlight
    cairo_rounded_rect(cr, x + 0.6, y + 0.6, sz - 1.2, sz - 1.2, rad - 0.5);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.34);
    cairo_set_line_width(cr, 1.1);
    cairo_stroke(cr);

    // Vector foreground artwork scaled to 64x64 logical units
    double s = sz / 64.0;
    cairo_translate(cr, x, y);
    cairo_scale(cr, s, s);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    if (kind == "store") {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.96);
        cairo_set_line_width(cr, 5.0);
        cairo_move_to(cr, 32, 16);
        cairo_line_to(cr, 20, 44);
        cairo_move_to(cr, 32, 16);
        cairo_line_to(cr, 44, 44);
        cairo_stroke(cr);
        cairo_set_line_width(cr, 4.5);
        cairo_move_to(cr, 17, 37.5);
        cairo_line_to(cr, 47, 37.5);
        cairo_stroke(cr);
    } else if (kind == "gamepad") {
        cairo_rounded_rect(cr, 13, 17, 38, 30, 9);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.94);
        cairo_set_line_width(cr, 2.5);
        cairo_stroke(cr);

        cairo_set_line_width(cr, 3.0);
        cairo_move_to(cr, 24, 27);
        cairo_line_to(cr, 24, 37);
        cairo_move_to(cr, 19, 32);
        cairo_line_to(cr, 29, 32);
        cairo_stroke(cr);

        cairo_arc(cr, 41.5, 29.5, 2.3, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.99, 0.88, 0.28, 1.0);
        cairo_arc(cr, 45, 34.5, 2.3, 0, 2 * M_PI);
        cairo_fill(cr);
    } else if (kind == "settings") {
        // Metallic gear teeth
        cairo_save(cr);
        cairo_translate(cr, 32, 32);
        cairo_set_source_rgba(cr, 0.28, 0.34, 0.43, 1.0);
        for (int i = 0; i < 8; ++i) {
            cairo_save(cr);
            cairo_rotate(cr, i * (M_PI / 4.0));
            cairo_rounded_rect(cr, -3.5, -22.5, 7, 10, 2.2);
            cairo_fill(cr);
            cairo_restore(cr);
        }
        cairo_arc(cr, 0, 0, 17.5, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0.34, 0.41, 0.51, 1.0);
        cairo_fill(cr);
        cairo_arc(cr, 0, 0, 12.5, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0.82, 0.85, 0.90, 1.0);
        cairo_fill(cr);
        cairo_arc(cr, 0, 0, 6.2, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0.20, 0.25, 0.33, 1.0);
        cairo_fill(cr);
        cairo_restore(cr);
    } else if (kind == "files") {
        // Generic layered folder card file manager icon
        cairo_rounded_rect(cr, 11, 14, 21, 10, 4.5);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.45);
        cairo_fill(cr);

        cairo_rounded_rect(cr, 15, 19, 34, 9, 2.5);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.75);
        cairo_fill(cr);

        cairo_pattern_t *folder = cairo_pattern_create_linear(11, 22, 53, 50);
        set_rgb_hex(folder, 0.0, 0xFFFFFF, 0.98);
        set_rgb_hex(folder, 1.0, 0xDBEAFE, 0.96);
        cairo_rounded_rect(cr, 11, 22, 42, 28, 7.0);
        cairo_set_source(cr, folder);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(folder);

        cairo_set_source_rgba(cr, 0.06, 0.35, 0.72, 0.20);
        cairo_set_line_width(cr, 1.4);
        cairo_stroke(cr);

        cairo_set_source_rgba(cr, 0.01, 0.52, 0.78, 0.88);
        cairo_set_line_width(cr, 2.6);
        cairo_move_to(cr, 20.5, 32.5);
        cairo_line_to(cr, 43.5, 32.5);
        cairo_move_to(cr, 20.5, 39.5);
        cairo_line_to(cr, 35.5, 39.5);
        cairo_stroke(cr);
    } else if (kind == "terminal") {
        // macOS Terminal sleek dark screen inside metallic squircle frame
        cairo_pattern_t *scr = cairo_pattern_create_linear(6, 7, 58, 57);
        set_rgb_hex(scr, 0.0, 0x1E293B);
        set_rgb_hex(scr, 1.0, 0x090D16);
        cairo_rounded_rect(cr, 6.5, 7.0, 51, 50, 11);
        cairo_set_source(cr, scr);
        cairo_fill(cr);
        cairo_pattern_destroy(scr);

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
        cairo_set_line_width(cr, 3.8);
        cairo_move_to(cr, 16, 23);
        cairo_line_to(cr, 25, 30);
        cairo_line_to(cr, 16, 37);
        cairo_stroke(cr);

        cairo_set_source_rgba(cr, 0.22, 0.74, 0.97, 1.0);
        cairo_set_line_width(cr, 3.8);
        cairo_move_to(cr, 29, 37);
        cairo_line_to(cr, 43, 37);
        cairo_stroke(cr);
    } else if (kind == "installer") {
        cairo_rounded_rect(cr, 15, 16, 34, 32, 8);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.2);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.94);
        cairo_set_line_width(cr, 2.4);
        cairo_stroke(cr);

        cairo_set_line_width(cr, 3.0);
        cairo_move_to(cr, 32, 22);
        cairo_line_to(cr, 32, 37);
        cairo_move_to(cr, 26, 31.5);
        cairo_line_to(cr, 32, 37.5);
        cairo_line_to(cr, 38, 31.5);
        cairo_move_to(cr, 22, 42.5);
        cairo_line_to(cr, 42, 42.5);
        cairo_stroke(cr);
    } else if (kind == "steam" || kind == "games") {
        // Sleek Gamepad silhouette
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
        cairo_move_to(cr, 21, 23);
        cairo_line_to(cr, 43, 23);
        cairo_curve_to(cr, 48, 23, 51, 26.5, 52, 31);
        cairo_line_to(cr, 53.5, 39);
        cairo_curve_to(cr, 54, 43, 49.5, 45.5, 46, 42.5);
        cairo_line_to(cr, 42, 38.5);
        cairo_line_to(cr, 22, 38.5);
        cairo_line_to(cr, 18, 42.5);
        cairo_curve_to(cr, 14.5, 45.5, 10, 43, 10.5, 39);
        cairo_line_to(cr, 12, 31);
        cairo_curve_to(cr, 13, 26.5, 16, 23, 21, 23);
        cairo_close_path(cr);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, 0.31, 0.27, 0.90, 1.0);
        cairo_set_line_width(cr, 2.8);
        cairo_move_to(cr, 21.5, 28);
        cairo_line_to(cr, 21.5, 34);
        cairo_move_to(cr, 18.5, 31);
        cairo_line_to(cr, 24.5, 31);
        cairo_stroke(cr);

        cairo_arc(cr, 42.5, 28.5, 2.0, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_arc(cr, 46.5, 32.0, 2.0, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_arc(cr, 38.5, 32.0, 2.0, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_arc(cr, 42.5, 35.5, 2.0, 0, 2 * M_PI);
        cairo_fill(cr);
    } else {
        cairo_rounded_rect(cr, 17, 17, 30, 30, 8);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
        cairo_set_line_width(cr, 2.5);
        cairo_stroke(cr);
        cairo_arc(cr, 32, 32, 5.5, 0, 2 * M_PI);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

void draw_app_icon(NativeShellState *ns,
                   cairo_t *cr,
                   double x,
                   double y,
                   double sz,
                   const std::string &icon_path,
                   const std::string &fallback_kind) {
    int req = std::max(24, static_cast<int>(std::round(sz)));
    cairo_surface_t *surf = get_cached_surface(ns, icon_path, req);
    if (surf) {
        int pw = cairo_image_surface_get_width(surf);
        int ph = cairo_image_surface_get_height(surf);
        double dx = x + (sz - pw) * 0.5;
        double dy = y + (sz - ph) * 0.5;
        cairo_save(cr);
        cairo_set_source_surface(cr, surf, dx, dy);
        cairo_paint(cr);
        cairo_restore(cr);
        return;
    }
    draw_system_squircle(cr, x, y, sz, fallback_kind);
}

void draw_pango_text(cairo_t *cr,
                     const std::string &text,
                     double x,
                     double y,
                     double font_px,
                     bool bold,
                     double r,
                     double g,
                     double b,
                     double a,
                     int *out_w = nullptr,
                     int *out_h = nullptr,
                     bool measure_only = false) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, "Noto Sans CJK SC, Inter, Sans");
    pango_font_description_set_absolute_size(desc, font_px * PANGO_SCALE);
    pango_font_description_set_weight(desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_MEDIUM);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, text.c_str(), -1);
    pango_font_description_free(desc);

    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    if (out_w) *out_w = tw;
    if (out_h) *out_h = th;

    if (!measure_only) {
        cairo_save(cr);
        cairo_move_to(cr, x, y);
        cairo_set_source_rgba(cr, r, g, b, a);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
    }
    g_object_unref(layout);
}

void trigger_cmd(AppState *state, const std::string &cmd) {
    if (state && !cmd.empty()) {
        handle_shell_command(state, cmd);
    }
}

void on_cmd_button_clicked(GtkButton *btn, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    const char *cmd = static_cast<const char *>(g_object_get_data(G_OBJECT(btn), "spinel-cmd"));
    if (cmd) trigger_cmd(state, cmd);
}

void on_popover_cmd_clicked(GtkButton *btn, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (ns->desktop_popover) gtk_popover_popdown(GTK_POPOVER(ns->desktop_popover));
    const char *cmd = static_cast<const char *>(g_object_get_data(G_OBJECT(btn), "spinel-cmd"));
    if (cmd) trigger_cmd(ns->app_state, cmd);
}

GtkWidget *make_cmd_button(AppState *state, const char *label, const char *css_class, const char *cmd) {
    GtkWidget *btn = label ? gtk_button_new_with_label(label) : gtk_button_new();
    if (css_class && *css_class) gtk_widget_add_css_class(btn, css_class);
    if (cmd) {
        g_object_set_data_full(G_OBJECT(btn), "spinel-cmd", g_strdup(cmd), g_free);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_cmd_button_clicked), state);
    }
    return btn;
}

/* ==================== Native Monochrome Vector Glyphs (Bar & CC) ==================== */

void draw_gem_glyph(GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer) {
    GdkRGBA fg{1.0f, 1.0f, 1.0f, 0.94f};
    gtk_widget_get_color(GTK_WIDGET(da), &fg);
    double s = std::min(w, h) / 18.0;
    cairo_save(cr);
    cairo_translate(cr, (w - 18.0 * s) * 0.5, (h - 18.0 * s) * 0.5);
    cairo_scale(cr, s, s);
    cairo_move_to(cr, 9, 1.8);
    cairo_line_to(cr, 15.5, 6.5);
    cairo_line_to(cr, 9, 16.2);
    cairo_line_to(cr, 2.5, 6.5);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, fg.alpha);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.08, 0.12, 0.22, 0.25);
    cairo_set_line_width(cr, 0.9);
    cairo_stroke(cr);
    cairo_restore(cr);
}

enum class BarGlyph { Wifi, Search, ControlCenter, Reboot, Power, Bluetooth, Gamepad, Hud, Appearance, Settings, Lock, Screenshot, Clipboard };

void draw_monochrome_glyph(cairo_t *cr, int w, int h, BarGlyph kind, const GdkRGBA &fg) {
    double s = std::min(w, h) / 16.0;
    cairo_save(cr);
    cairo_translate(cr, (w - 16.0 * s) * 0.5, (h - 16.0 * s) * 0.5);
    cairo_scale(cr, s, s);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, fg.alpha);
    cairo_set_line_width(cr, 1.45);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    switch (kind) {
        case BarGlyph::Wifi:
            cairo_arc(cr, 8, 12.6, 1.1, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_arc(cr, 8, 12.6, 3.6, -2.35, -0.79);
            cairo_stroke(cr);
            cairo_arc(cr, 8, 12.6, 6.4, -2.32, -0.82);
            cairo_stroke(cr);
            cairo_arc(cr, 8, 12.6, 9.2, -2.28, -0.86);
            cairo_stroke(cr);
            break;
        case BarGlyph::Search:
            cairo_arc(cr, 7, 7, 4.3, 0, 2 * M_PI);
            cairo_stroke(cr);
            cairo_move_to(cr, 10.4, 10.4);
            cairo_line_to(cr, 13.5, 13.5);
            cairo_stroke(cr);
            break;
        case BarGlyph::ControlCenter:
            cairo_set_line_width(cr, 1.25);
            cairo_rounded_rect(cr, 2.2, 3.0, 11.6, 4.2, 2.1);
            cairo_stroke(cr);
            cairo_arc(cr, 11.5, 5.1, 1.4, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_rounded_rect(cr, 2.2, 8.8, 11.6, 4.2, 2.1);
            cairo_stroke(cr);
            cairo_arc(cr, 4.5, 10.9, 1.4, 0, 2 * M_PI);
            cairo_fill(cr);
            break;
        case BarGlyph::Reboot:
            cairo_arc(cr, 8, 8, 4.8, -0.75, 1.8 * M_PI);
            cairo_stroke(cr);
            cairo_move_to(cr, 12.6, 2.8);
            cairo_line_to(cr, 12.6, 5.5);
            cairo_line_to(cr, 9.9, 5.5);
            cairo_stroke(cr);
            break;
        case BarGlyph::Power:
            cairo_move_to(cr, 8, 2.4);
            cairo_line_to(cr, 8, 7.2);
            cairo_stroke(cr);
            cairo_arc(cr, 8, 8.6, 4.8, -1.0, M_PI + 1.0);
            cairo_stroke(cr);
            break;
        case BarGlyph::Bluetooth:
            cairo_move_to(cr, 5, 4.8);
            cairo_line_to(cr, 11.2, 10.5);
            cairo_line_to(cr, 8, 13.5);
            cairo_line_to(cr, 8, 2.5);
            cairo_line_to(cr, 11.2, 5.5);
            cairo_line_to(cr, 5, 11.2);
            cairo_stroke(cr);
            break;
        case BarGlyph::Gamepad:
            cairo_rounded_rect(cr, 2.2, 5.0, 11.6, 6.6, 3.0);
            cairo_stroke(cr);
            cairo_move_to(cr, 4.8, 8.3);
            cairo_line_to(cr, 6.8, 8.3);
            cairo_move_to(cr, 5.8, 7.3);
            cairo_line_to(cr, 5.8, 9.3);
            cairo_stroke(cr);
            cairo_arc(cr, 10.6, 7.6, 0.85, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_arc(cr, 10.6, 9.4, 0.85, 0, 2 * M_PI);
            cairo_fill(cr);
            break;
        case BarGlyph::Hud:
            cairo_arc(cr, 8, 10.5, 5.2, M_PI, 2 * M_PI);
            cairo_stroke(cr);
            cairo_move_to(cr, 8, 10.5);
            cairo_line_to(cr, 11.0, 7.2);
            cairo_stroke(cr);
            break;
        case BarGlyph::Appearance:
            cairo_arc(cr, 8, 8, 5.0, 0, 2 * M_PI);
            cairo_stroke(cr);
            cairo_arc(cr, 8, 8, 5.0, M_PI_2, 1.5 * M_PI);
            cairo_fill(cr);
            break;
        case BarGlyph::Settings:
            cairo_rounded_rect(cr, 2.2, 3.2, 11.6, 7.6, 1.5);
            cairo_stroke(cr);
            cairo_move_to(cr, 5.5, 13.0);
            cairo_line_to(cr, 10.5, 13.0);
            cairo_move_to(cr, 8, 10.8);
            cairo_line_to(cr, 8, 13.0);
            cairo_stroke(cr);
            break;
        case BarGlyph::Lock:
            cairo_rounded_rect(cr, 3.4, 7.2, 9.2, 6.4, 1.8);
            cairo_stroke(cr);
            cairo_arc(cr, 8.0, 6.2, 2.7, M_PI, 2 * M_PI);
            cairo_stroke(cr);
            cairo_arc(cr, 8.0, 10.4, 0.9, 0, 2 * M_PI);
            cairo_fill(cr);
            break;
        case BarGlyph::Screenshot:
            cairo_move_to(cr, 2.5, 5.5);
            cairo_line_to(cr, 2.5, 3.0);
            cairo_line_to(cr, 5.2, 3.0);
            cairo_move_to(cr, 10.8, 3.0);
            cairo_line_to(cr, 13.5, 3.0);
            cairo_line_to(cr, 13.5, 5.5);
            cairo_move_to(cr, 13.5, 10.5);
            cairo_line_to(cr, 13.5, 13.0);
            cairo_line_to(cr, 10.8, 13.0);
            cairo_move_to(cr, 5.2, 13.0);
            cairo_line_to(cr, 2.5, 13.0);
            cairo_line_to(cr, 2.5, 10.5);
            cairo_stroke(cr);
            cairo_arc(cr, 8.0, 8.0, 2.3, 0, 2 * M_PI);
            cairo_stroke(cr);
            break;
        case BarGlyph::Clipboard:
            cairo_rounded_rect(cr, 3.2, 3.4, 9.6, 10.4, 1.8);
            cairo_stroke(cr);
            cairo_rounded_rect(cr, 5.6, 1.8, 4.8, 2.6, 1.0);
            cairo_stroke(cr);
            cairo_move_to(cr, 5.5, 7.2);
            cairo_line_to(cr, 10.5, 7.2);
            cairo_move_to(cr, 5.5, 10.0);
            cairo_line_to(cr, 9.2, 10.0);
            cairo_stroke(cr);
            break;
    }
    cairo_restore(cr);
}

void draw_glyph_cb(GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer data) {
    BarGlyph kind = static_cast<BarGlyph>(reinterpret_cast<intptr_t>(data));
    GdkRGBA fg{1.0f, 1.0f, 1.0f, 0.94f};
    gtk_widget_get_color(GTK_WIDGET(da), &fg);
    draw_monochrome_glyph(cr, w, h, kind, fg);
}

GtkWidget *make_glyph_icon_btn(AppState *state, BarGlyph kind, const char *tooltip, const char *cmd, const char *extra_cls = nullptr) {
    GtkWidget *btn = make_cmd_button(state, nullptr, "spinel-bar-icon-btn", cmd);
    if (extra_cls) gtk_widget_add_css_class(btn, extra_cls);
    gtk_widget_set_tooltip_text(btn, tooltip);
    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, 16, 16);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_glyph_cb,
                                   reinterpret_cast<gpointer>(static_cast<intptr_t>(kind)), nullptr);
    gtk_button_set_child(GTK_BUTTON(btn), da);
    return btn;
}

/* ==================== 1. Native Top Bar ==================== */

gboolean on_clock_tick(gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (!ns || !ns->bar_clock_lbl) return G_SOURCE_REMOVE;
    std::time_t now = std::time(nullptr);
    std::tm tm_now{};
    localtime_r(&now, &tm_now);
    static const char *kDays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d月%d日 %s %02d:%02d",
                  tm_now.tm_mon + 1, tm_now.tm_mday,
                  kDays[tm_now.tm_wday % 7], tm_now.tm_hour, tm_now.tm_min);
    gtk_label_set_text(GTK_LABEL(ns->bar_clock_lbl), buf);
    return G_SOURCE_CONTINUE;
}

struct TopBarMenuEntry {
    const char *label;
    std::string cmd;
};

GtkWidget *make_topbar_popover_menu(AppState *state,
                                    const std::vector<TopBarMenuEntry> &entries) {
    GtkWidget *pop = gtk_popover_new();
    gtk_widget_add_css_class(pop, "spinel-ctx-popover");
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    for (const auto &e : entries) {
        if (!e.label) {
            gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
            continue;
        }
        GtkWidget *b = gtk_button_new_with_label(e.label);
        gtk_widget_add_css_class(b, "spinel-ctx-item");
        GtkWidget *c = gtk_button_get_child(GTK_BUTTON(b));
        if (GTK_IS_LABEL(c)) gtk_widget_set_halign(c, GTK_ALIGN_START);
        g_object_set_data_full(G_OBJECT(b), "spinel-cmd", g_strdup(e.cmd.c_str()), g_free);
        g_object_set_data(G_OBJECT(b), "spinel-pop", pop);
        g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton *btn, gpointer u) {
            GtkWidget *p = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(btn), "spinel-pop"));
            if (p) gtk_popover_popdown(GTK_POPOVER(p));
            on_cmd_button_clicked(btn, u);
        }), state);
        gtk_box_append(GTK_BOX(box), b);
    }

    g_signal_connect(pop, "show", G_CALLBACK(+[](GtkWidget *, gpointer u) {
        native_shell_set_cc_visible(static_cast<AppState *>(u), false);
    }), state);

    gtk_popover_set_child(GTK_POPOVER(pop), box);
    return pop;
}

void add_topbar_menu(NativeShellState *ns,
                     const char *title,
                     const std::vector<TopBarMenuEntry> &entries) {
    if (!ns || !ns->bar_app_menus) return;
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(mb), FALSE);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(mb), gtk_label_new(title));
    gtk_widget_add_css_class(mb, "spinel-bar-menubtn");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), make_topbar_popover_menu(ns->app_state, entries));
    gtk_box_append(GTK_BOX(ns->bar_app_menus), mb);
}

void sync_top_bar_app_menus(NativeShellState *ns,
                            const TrackedWindowInfo *active_win,
                            std::string &out_display_name) {
    if (!ns || !ns->bar_app_menus) return;

    std::string profile = "finder";
    uint64_t wid = 0;
    out_display_name = "访达";

    if (active_win) {
        wid = active_win->id;
        std::string hay = to_lower_copy(active_win->app_id + " " + active_win->title);
        if (hay.find("系统设置") != std::string::npos || hay.find("settings") != std::string::npos) {
            profile = "settings";
            out_display_name = "系统设置";
        } else if (hay.find("游戏与启动中心") != std::string::npos || hay.find("app-games") != std::string::npos) {
            profile = "games";
            out_display_name = "游戏中心";
        } else if (hay.find("应用商店") != std::string::npos || hay.find("app-store") != std::string::npos) {
            profile = "store";
            out_display_name = "应用商店";
        } else if (hay.find("安装") != std::string::npos || hay.find("installer") != std::string::npos) {
            profile = "installer";
            out_display_name = "系统安装";
        } else if (hay.find("chromium") != std::string::npos || hay.find("chrome") != std::string::npos ||
                   hay.find("firefox") != std::string::npos) {
            profile = "browser";
            out_display_name = hay.find("firefox") != std::string::npos ? "Firefox" :
                               (hay.find("chrome") != std::string::npos ? "Chrome" : "Chromium");
        } else if (hay.find("ptyxis") != std::string::npos || hay.find("foot") != std::string::npos ||
                   hay.find("terminal") != std::string::npos || hay.find("终端") != std::string::npos) {
            profile = "terminal";
            out_display_name = "终端";
        } else if (hay.find("thunar") != std::string::npos || hay.find("文件管理") != std::string::npos) {
            profile = "files";
            out_display_name = "文件管理";
        } else if (hay.find("steam") != std::string::npos || hay.find("gamescope") != std::string::npos) {
            profile = "steam";
            out_display_name = hay.find("gamescope") != std::string::npos ? "主机大屏" : "Steam";
        } else {
            profile = "generic";
            out_display_name = !active_win->title.empty() ? active_win->title :
                               (!active_win->app_id.empty() ? active_win->app_id : "应用");
        }
    }

    std::string key = profile + ":" + std::to_string(wid);
    if (key == ns->bar_menu_key) return;
    ns->bar_menu_key = key;

    while (GtkWidget *child = gtk_widget_get_first_child(ns->bar_app_menus)) {
        gtk_box_remove(GTK_BOX(ns->bar_app_menus), child);
    }

    std::string close_cmd = "window:close:" + std::to_string(wid);
    std::string toggle_cmd = "window:toggle:" + std::to_string(wid);

    if (profile == "finder") {
        add_topbar_menu(ns, "文件", {
            {"新建桌面文件夹", "desktop:new-folder"},
            {"在文件管理中打开桌面", "desktop:open-folder"},
            {nullptr, ""},
            {"打开文件管理", "launch:files"},
            {"打开终端", "launch:terminal"}
        });
        add_topbar_menu(ns, "显示", {
            {"更换壁纸与主题...", "settings:open:appearance"},
            {"显示器与缩放设置...", "settings:open:display"},
            {"刷新桌面", "telemetry:refresh"}
        });
        add_topbar_menu(ns, "前往", {
            {"游戏与启动中心", "games:open"},
            {"应用商店", "store:open"},
            {"浏览器", "launch:browser"},
            {"Steam", "launch:steam"}
        });
    } else if (profile == "games") {
        add_topbar_menu(ns, "游戏库", {
            {"扫描本地 Steam 游戏", "steam:scan"},
            {"启动 Steam 客户端", "launch:steam"},
            {"启动主机大屏 (Gamescope)", "launch:steam-gamepadui"}
        });
        add_topbar_menu(ns, "性能", {
            {"游戏性能与 MangoHud 设置...", "settings:open:perf"},
            {"蓝牙手柄配对...", "settings:open:bt"}
        });
        add_topbar_menu(ns, "窗口", {
            {"应用商店", "store:open"},
            {nullptr, ""},
            {"关闭游戏中心", close_cmd}
        });
    } else if (profile == "store") {
        add_topbar_menu(ns, "商店", {
            {"刷新应用目录", "store:refresh"},
            {"游戏与启动中心", "games:open"}
        });
        add_topbar_menu(ns, "网络", {
            {"网络与代理设置...", "settings:open:wifi"}
        });
        add_topbar_menu(ns, "窗口", {
            {"关闭应用商店", close_cmd}
        });
    } else if (profile == "settings") {
        add_topbar_menu(ns, "设置", {
            {"Wi-Fi 与网络代理", "settings:open:wifi"},
            {"蓝牙与手柄", "settings:open:bt"},
            {"显示器分辨率", "settings:open:display"},
            {"声音设备与电源", "settings:open:audio"},
            {"键盘与快捷键", "settings:open:shortcuts"},
            {"游戏性能优化", "settings:open:perf"},
            {"进程与资源监控", "settings:open:procs"},
            {"外观主题与输入法", "settings:open:appearance"},
            {"应用权限与安全", "settings:open:perms"},
            {"系统日志与排障", "settings:open:logs"}
        });
        add_topbar_menu(ns, "窗口", {
            {"关闭系统设置", close_cmd}
        });
    } else if (profile == "browser") {
        add_topbar_menu(ns, "文件", {
            {"新建浏览器窗口", "launch:browser"},
            {"打开文件管理", "launch:files"},
            {nullptr, ""},
            {"关闭当前窗口", close_cmd}
        });
        add_topbar_menu(ns, "网络与输入", {
            {"网络代理设置...", "settings:open:wifi"},
            {"中文输入法设置...", "settings:open:appearance"}
        });
    } else if (profile == "terminal") {
        add_topbar_menu(ns, "Shell", {
            {"新建终端窗口", "launch:terminal"},
            {"打开文件管理", "launch:files"},
            {nullptr, ""},
            {"关闭当前终端", close_cmd}
        });
        add_topbar_menu(ns, "工具", {
            {"网络代理设置...", "settings:open:wifi"},
            {"查看系统日志", "settings:open:logs"}
        });
    } else if (profile == "files") {
        add_topbar_menu(ns, "文件", {
            {"新建文件管理窗口", "launch:files"},
            {"打开桌面文件夹", "desktop:open-folder"},
            {"打开终端", "launch:terminal"},
            {nullptr, ""},
            {"关闭当前窗口", close_cmd}
        });
        add_topbar_menu(ns, "前往", {
            {"游戏与启动中心", "games:open"},
            {"应用商店", "store:open"}
        });
    } else if (profile == "steam") {
        add_topbar_menu(ns, "Steam", {
            {"打开游戏中心", "games:open"},
            {"启动主机大屏 (Gamescope)", "launch:steam-gamepadui"},
            {nullptr, ""},
            {"关闭当前窗口", close_cmd}
        });
        add_topbar_menu(ns, "性能", {
            {"MangoHud 与锁帧设置...", "settings:open:perf"},
            {"蓝牙手柄配对...", "settings:open:bt"}
        });
    } else {
        add_topbar_menu(ns, "应用", {
            {"最小化 / 切换", toggle_cmd},
            {"关闭当前窗口", close_cmd}
        });
        add_topbar_menu(ns, "前往", {
            {"游戏与启动中心", "games:open"},
            {"应用商店", "store:open"},
            {"系统设置...", "settings:open:display"}
        });
    }
}

void build_native_top_bar(GtkApplication *app, NativeShellState *ns) {
    AppState *state = ns->app_state;
    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(win, "SpinelTopBar");
    gtk_widget_add_css_class(GTK_WIDGET(win), "spinel-transparent");
    gtk_window_set_default_size(win, -1, 28);

    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "spinel-bar");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_auto_exclusive_zone_enable(win);
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(root, "spinel-topbar");
    gtk_widget_set_size_request(root, -1, 28);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_hexpand(left, TRUE);
    gtk_widget_set_halign(left, GTK_ALIGN_START);
    gtk_widget_set_valign(left, GTK_ALIGN_CENTER);

    GtkWidget *brand_btn = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(brand_btn), FALSE);
    gtk_widget_add_css_class(brand_btn, "spinel-bar-menubtn");
    gtk_widget_set_tooltip_text(brand_btn, "SpinelOS 系统菜单");
    GtkWidget *gem_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(gem_da, 16, 16);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(gem_da), draw_gem_glyph, nullptr, nullptr);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(brand_btn), gem_da);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(brand_btn), make_topbar_popover_menu(state, {
        {"关于 SpinelOS", "settings:open:appearance"},
        {"系统设置...", "settings:open:display"},
        {"声音与电源...", "settings:open:audio"},
        {nullptr, ""},
        {"区域截图 (Print/Ctrl+Shift+4)", "system:screenshot:area"},
        {"游戏与启动中心", "games:open"},
        {"应用商店", "store:open"},
        {"进程与资源监控", "settings:open:procs"},
        {"权限与安全...", "settings:open:perms"},
        {"查看系统日志", "settings:open:logs"},
        {nullptr, ""},
        {"锁定屏幕", "system:lock"},
        {"睡眠", "system:suspend"},
        {"重新启动...", "power:reboot"},
        {"关机...", "power:shutdown"}
    }));

    ns->bar_active_lbl = gtk_label_new("访达");
    gtk_widget_add_css_class(ns->bar_active_lbl, "spinel-bar-active");
    gtk_label_set_ellipsize(GTK_LABEL(ns->bar_active_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(ns->bar_active_lbl), 24);

    ns->bar_app_menus = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 1);

    gtk_box_append(GTK_BOX(left), brand_btn);
    gtk_box_append(GTK_BOX(left), ns->bar_active_lbl);
    gtk_box_append(GTK_BOX(left), ns->bar_app_menus);

    std::string init_name;
    sync_top_bar_app_menus(ns, nullptr, init_name);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_widget_set_halign(right, GTK_ALIGN_END);
    gtk_widget_set_valign(right, GTK_ALIGN_CENTER);

    GtkWidget *mem_btn = make_cmd_button(state, nullptr, "spinel-bar-btn", "settings:open:procs");
    ns->bar_mem_lbl = gtk_label_new("--");
    gtk_widget_add_css_class(ns->bar_mem_lbl, "spinel-bar-mem");
    gtk_button_set_child(GTK_BUTTON(mem_btn), ns->bar_mem_lbl);
    gtk_widget_set_tooltip_text(mem_btn, "内存占用（点击打开进程监控）");

    auto rebuild_clip_popover = +[](NativeShellState *n) {
        if (!n || !n->clip_list_box) return;
        while (GtkWidget *c = gtk_widget_get_first_child(n->clip_list_box)) {
            gtk_box_remove(GTK_BOX(n->clip_list_box), c);
        }

        GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *title = gtk_label_new("剪贴板历史（最多 10 条）");
        gtk_widget_add_css_class(title, "spinel-clip-head");
        gtk_widget_set_hexpand(title, TRUE);
        gtk_widget_set_halign(title, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(head), title);

        GtkWidget *clr = gtk_button_new_with_label("清空");
        gtk_widget_add_css_class(clr, "spinel-clip-clr");
        g_signal_connect(clr, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
            auto *st_ns = static_cast<NativeShellState *>(d);
            trigger_cmd(st_ns->app_state, "clip:clear");
            if (st_ns->clip_popover) gtk_popover_popdown(GTK_POPOVER(st_ns->clip_popover));
        }), n);
        gtk_box_append(GTK_BOX(head), clr);
        gtk_box_append(GTK_BOX(n->clip_list_box), head);
        gtk_box_append(GTK_BOX(n->clip_list_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

        auto items = load_clipboard_history();
        if (items.empty()) {
            GtkWidget *empty = gtk_label_new("暂无记录 · 复制文字后自动保留于此\n（关闭原应用窗口内容仍不会丢失）");
            gtk_widget_add_css_class(empty, "spinel-clip-empty");
            gtk_label_set_justify(GTK_LABEL(empty), GTK_JUSTIFY_CENTER);
            gtk_box_append(GTK_BOX(n->clip_list_box), empty);
            return;
        }

        for (size_t i = 0; i < items.size(); ++i) {
            std::string preview;
            preview.reserve(items[i].text.size());
            for (char ch : items[i].text) {
                if (ch == '\n' || ch == '\r' || ch == '\t') {
                    if (preview.empty() || preview.back() != ' ') preview += ' ';
                } else {
                    preview += ch;
                }
            }
            preview = trim_copy(preview);
            if (preview.empty()) preview = "(空白文本)";

            char tbuf[16]{};
            if (items[i].ts > 0) {
                std::time_t tt = static_cast<std::time_t>(items[i].ts);
                std::tm tmv{};
                localtime_r(&tt, &tmv);
                std::strftime(tbuf, sizeof(tbuf), "%H:%M", &tmv);
            }

            std::string cmd = "clip:copy:" + std::to_string(i);
            GtkWidget *btn = make_cmd_button(n->app_state, nullptr, "spinel-clip-row", cmd.c_str());
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GtkWidget *idx_lbl = gtk_label_new(std::to_string(i + 1).c_str());
            gtk_widget_add_css_class(idx_lbl, "spinel-clip-idx");

            GtkWidget *txt_lbl = gtk_label_new(preview.c_str());
            gtk_widget_add_css_class(txt_lbl, "spinel-clip-txt");
            gtk_widget_set_hexpand(txt_lbl, TRUE);
            gtk_widget_set_halign(txt_lbl, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(txt_lbl), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(txt_lbl), 34);

            GtkWidget *meta_lbl = gtk_label_new(tbuf);
            gtk_widget_add_css_class(meta_lbl, "spinel-clip-meta");

            gtk_box_append(GTK_BOX(row), idx_lbl);
            gtk_box_append(GTK_BOX(row), txt_lbl);
            gtk_box_append(GTK_BOX(row), meta_lbl);
            gtk_button_set_child(GTK_BUTTON(btn), row);
            g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
                auto *st_ns = static_cast<NativeShellState *>(d);
                if (st_ns && st_ns->clip_popover) gtk_popover_popdown(GTK_POPOVER(st_ns->clip_popover));
            }), n);
            gtk_box_append(GTK_BOX(n->clip_list_box), btn);
        }
    };

    GtkWidget *clip_btn = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(clip_btn), FALSE);
    gtk_widget_add_css_class(clip_btn, "spinel-bar-menubtn");
    gtk_widget_set_tooltip_text(clip_btn, "剪贴板历史（最多 10 条）");
    GtkWidget *clip_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(clip_da, 15, 15);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(clip_da), draw_glyph_cb,
                                   reinterpret_cast<gpointer>(static_cast<intptr_t>(BarGlyph::Clipboard)), nullptr);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(clip_btn), clip_da);

    GtkWidget *cpop = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(cpop), FALSE);
    gtk_widget_add_css_class(cpop, "spinel-ctx-popover");
    GtkWidget *cbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_size_request(cbox, 310, -1);
    gtk_popover_set_child(GTK_POPOVER(cpop), cbox);
    ns->clip_popover = cpop;
    ns->clip_list_box = cbox;
    g_object_set_data(G_OBJECT(cpop), "rebuild_fn", reinterpret_cast<gpointer>(rebuild_clip_popover));
    g_signal_connect(cpop, "notify::visible", G_CALLBACK(+[](GObject *obj, GParamSpec *, gpointer d) {
        if (gtk_widget_get_visible(GTK_WIDGET(obj))) {
            auto fn = reinterpret_cast<void (*)(NativeShellState *)>(g_object_get_data(obj, "rebuild_fn"));
            if (fn) fn(static_cast<NativeShellState *>(d));
        }
    }), ns);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(clip_btn), cpop);

    ns->bar_net_btn = make_glyph_icon_btn(state, BarGlyph::Wifi, "网络: 检测中", "settings:open:wifi");
    GtkWidget *search_btn = make_glyph_icon_btn(state, BarGlyph::Search, "游戏与启动中心", "games:open");
    ns->bar_cc_btn = make_glyph_icon_btn(state, BarGlyph::ControlCenter, "控制中心", "cc:toggle");

    GtkWidget *power_btn = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(power_btn), FALSE);
    gtk_widget_add_css_class(power_btn, "spinel-bar-menubtn");
    gtk_widget_add_css_class(power_btn, "danger-hover");
    gtk_widget_set_tooltip_text(power_btn, "电源菜单（重启 / 关机）");
    GtkWidget *power_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(power_da, 15, 15);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(power_da), draw_glyph_cb,
                                   reinterpret_cast<gpointer>(static_cast<intptr_t>(BarGlyph::Power)), nullptr);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(power_btn), power_da);

    gtk_menu_button_set_popover(GTK_MENU_BUTTON(power_btn), make_topbar_popover_menu(state, {
        {"锁定屏幕", "system:lock"},
        {"睡眠", "system:suspend"},
        {nullptr, ""},
        {"重新启动...", "power:reboot"},
        {"关机...", "power:shutdown"}
    }));

    ns->bar_bat_lbl = gtk_label_new("");
    gtk_widget_add_css_class(ns->bar_bat_lbl, "spinel-bar-mem");
    gtk_widget_set_visible(ns->bar_bat_lbl, FALSE);

    ns->bar_clock_lbl = gtk_label_new("--:--");
    gtk_widget_add_css_class(ns->bar_clock_lbl, "spinel-bar-clock");

    gtk_box_append(GTK_BOX(right), mem_btn);
    gtk_box_append(GTK_BOX(right), clip_btn);
    gtk_box_append(GTK_BOX(right), ns->bar_net_btn);
    gtk_box_append(GTK_BOX(right), search_btn);
    gtk_box_append(GTK_BOX(right), ns->bar_cc_btn);
    gtk_box_append(GTK_BOX(right), power_btn);
    gtk_box_append(GTK_BOX(right), ns->bar_bat_lbl);
    gtk_box_append(GTK_BOX(right), ns->bar_clock_lbl);

    gtk_box_append(GTK_BOX(root), left);
    gtk_box_append(GTK_BOX(root), right);
    gtk_window_set_child(win, root);

    ns->bar_win = win;
    on_clock_tick(ns);
    ns->clock_timer_id = g_timeout_add_seconds(5, on_clock_tick, ns);
    gtk_window_present(win);
}

/* ==================== 2. Native Desktop Wallpaper + Shortcuts + Context Menu ==================== */

void draw_desktop_wallpaper(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    const ThemeSpec &t = ns->theme;

    cairo_pattern_t *base = cairo_pattern_create_linear(0, 0, w, h);
    set_rgb_hex(base, 0.0, t.wp_c0);
    set_rgb_hex(base, 0.52, t.wp_c1);
    set_rgb_hex(base, 1.0, t.wp_c2);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_set_source(cr, base);
    cairo_fill(cr);
    cairo_pattern_destroy(base);

    double glow_a1 = t.is_light ? 0.22 : 0.26;
    double glow_a2 = t.is_light ? 0.18 : 0.24;

    double r1 = std::max(w, h) * 0.48;
    cairo_pattern_t *rad1 = cairo_pattern_create_radial(w * 0.22, h * 0.22, 0, w * 0.22, h * 0.22, r1);
    set_rgb_hex(rad1, 0.0, t.wp_glow1, glow_a1);
    set_rgb_hex(rad1, 1.0, t.wp_glow1, 0.0);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_set_source(cr, rad1);
    cairo_fill(cr);
    cairo_pattern_destroy(rad1);

    double r2 = std::max(w, h) * 0.50;
    cairo_pattern_t *rad2 = cairo_pattern_create_radial(w * 0.80, h * 0.78, 0, w * 0.80, h * 0.78, r2);
    set_rgb_hex(rad2, 0.0, t.wp_glow2, glow_a2);
    set_rgb_hex(rad2, 1.0, t.wp_glow2, 0.0);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_set_source(cr, rad2);
    cairo_fill(cr);
    cairo_pattern_destroy(rad2);
}

struct DesktopIconDrawCtx {
    NativeShellState *ns = nullptr;
    std::string path;
    std::string kind;
};

void draw_desktop_shortcut_icon(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer user_data) {
    auto *ctx = static_cast<DesktopIconDrawCtx *>(user_data);
    double sz = std::min(w, h);
    draw_app_icon(ctx->ns, cr, (w - sz) * 0.5, (h - sz) * 0.5, sz, ctx->path, ctx->kind);
}

void free_desktop_icon_ctx(gpointer data) {
    delete static_cast<DesktopIconDrawCtx *>(data);
}

void rebuild_desktop_shortcuts(NativeShellState *ns) {
    if (!ns || !ns->desktop_flow) return;

    while (GtkWidget *child = gtk_widget_get_first_child(ns->desktop_flow)) {
        gtk_flow_box_remove(GTK_FLOW_BOX(ns->desktop_flow), child);
    }

    struct ShortcutDef {
        std::string label;
        std::string cmd;
        std::string kind;
        std::string icon_path;
    };

    std::vector<ShortcutDef> items = {
        {"游戏中心", "games:open", "games", ""},
        {"应用商店", "store:open", "store", ""},
        {"Steam", "launch:steam", "steam", find_app_icon_path(ns->desktop_apps, {"steam"})},
        {"主机大屏", "launch:steam-gamepadui", "gamepad", ""},
        {"文件管理", "launch:files", "files", ""},
        {"终端", "launch:terminal", "terminal", ""},
        {"系统设置", "settings:open:display", "settings", ""}
    };
    if (ns->telemetry.is_live) {
        items.push_back({"安装 SpinelOS", "installer:open", "installer", ""});
    }

    static const char *kFeaturedStoreIds[] = {
        "com.google.chrome", "chromium", "firefox", "com.heroicgameslauncher.hgl",
        "lutris", "discord", "com.obsproject.studio"
    };
    for (const auto &app : ns->desktop_apps) {
        std::string lid = to_lower_copy(app.id);
        for (const char *fid : kFeaturedStoreIds) {
            if (lid == fid) {
                items.push_back({app.name, "app:launch:" + app.id, match_app_kind(app.id), app.icon_path});
                break;
            }
        }
    }

    auto steam_games = list_steam_games();
    int added_games = 0;
    for (const auto &g : steam_games) {
        if (added_games >= 6) break;
        items.push_back({
            g.name, "steam:launch:" + g.appid, "steam",
            !g.cover_path.empty() ? g.cover_path : find_app_icon_path(ns->desktop_apps, {"steam"})
        });
        ++added_games;
    }

    for (const auto &sc : items) {
        GtkWidget *btn = make_cmd_button(ns->app_state, nullptr, "spinel-shortcut-btn", sc.cmd.c_str());
        gtk_widget_set_size_request(btn, 84, 84);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_widget_set_halign(vbox, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(vbox, GTK_ALIGN_START);

        GtkWidget *da = gtk_drawing_area_new();
        gtk_widget_set_size_request(da, 52, 52);
        gtk_widget_set_halign(da, GTK_ALIGN_CENTER);
        auto *ctx = new DesktopIconDrawCtx{ns, sc.icon_path, sc.kind};
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_desktop_shortcut_icon, ctx, free_desktop_icon_ctx);

        GtkWidget *lbl = gtk_label_new(sc.label.c_str());
        gtk_widget_add_css_class(lbl, "spinel-shortcut-label");
        gtk_label_set_max_width_chars(GTK_LABEL(lbl), 10);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_CENTER);
        gtk_label_set_lines(GTK_LABEL(lbl), 2);
        gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);

        gtk_box_append(GTK_BOX(vbox), da);
        gtk_box_append(GTK_BOX(vbox), lbl);
        gtk_button_set_child(GTK_BUTTON(btn), vbox);
        gtk_flow_box_append(GTK_FLOW_BOX(ns->desktop_flow), btn);
    }
}

void on_desktop_right_click(GtkGestureClick *gesture, int, double x, double y, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    if (btn != GDK_BUTTON_SECONDARY || !ns->desktop_popover) return;
    GdkRectangle rect{static_cast<int>(x), static_cast<int>(y), 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(ns->desktop_popover), &rect);
    gtk_popover_popup(GTK_POPOVER(ns->desktop_popover));
}

void build_desktop_popover(NativeShellState *ns) {
    GtkWidget *pop = gtk_popover_new();
    gtk_widget_set_parent(pop, ns->desktop_overlay);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_widget_add_css_class(pop, "spinel-ctx-popover");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    struct MenuItem { const char *label; const char *cmd; };
    const MenuItem entries[] = {
        {"新建文件夹", "desktop:new-folder"},
        {"在文件管理器中打开桌面", "desktop:open-folder"},
        {"打开终端", "launch:terminal"},
        {nullptr, nullptr},
        {"打开应用商店", "store:open"},
        {"打开游戏中心", "games:open"},
        {nullptr, nullptr},
        {"更换壁纸与外观...", "settings:open:appearance"},
        {"显示与缩放设置...", "settings:open:display"},
        {"查看系统日志与诊断...", "settings:open:logs"},
        {"原生日志窗口 (排障)", "logs:native"},
        {"刷新桌面", "telemetry:refresh"}
    };

    for (const auto &m : entries) {
        if (!m.label) {
            gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
            continue;
        }
        GtkWidget *b = gtk_button_new_with_label(m.label);
        gtk_widget_add_css_class(b, "spinel-ctx-item");
        GtkWidget *child = gtk_button_get_child(GTK_BUTTON(b));
        if (GTK_IS_LABEL(child)) gtk_widget_set_halign(child, GTK_ALIGN_START);
        g_object_set_data_full(G_OBJECT(b), "spinel-cmd", g_strdup(m.cmd), g_free);
        g_signal_connect(b, "clicked", G_CALLBACK(on_popover_cmd_clicked), ns);
        gtk_box_append(GTK_BOX(box), b);
    }

    gtk_popover_set_child(GTK_POPOVER(pop), box);
    ns->desktop_popover = pop;
}

void build_native_desktop(GtkApplication *app, NativeShellState *ns) {
    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(win, "SpinelDesktop");

    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "spinel-desktop");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_BOTTOM);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_exclusive_zone(win, -1);
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

    ns->desktop_overlay = gtk_overlay_new();
    ns->desktop_bg_da = gtk_drawing_area_new();
    gtk_widget_set_hexpand(ns->desktop_bg_da, TRUE);
    gtk_widget_set_vexpand(ns->desktop_bg_da, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ns->desktop_bg_da), draw_desktop_wallpaper, ns, nullptr);
    gtk_overlay_set_child(GTK_OVERLAY(ns->desktop_overlay), ns->desktop_bg_da);

    ns->desktop_flow = gtk_flow_box_new();
    gtk_orientable_set_orientation(GTK_ORIENTABLE(ns->desktop_flow), GTK_ORIENTATION_VERTICAL);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(ns->desktop_flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(ns->desktop_flow), 4);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(ns->desktop_flow), 6);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(ns->desktop_flow), TRUE);
    gtk_widget_set_halign(ns->desktop_flow, GTK_ALIGN_START);
    gtk_widget_set_valign(ns->desktop_flow, GTK_ALIGN_START);
    gtk_widget_set_margin_top(ns->desktop_flow, 40);
    gtk_widget_set_margin_start(ns->desktop_flow, 16);
    gtk_widget_set_margin_bottom(ns->desktop_flow, 88);
    gtk_overlay_add_overlay(GTK_OVERLAY(ns->desktop_overlay), ns->desktop_flow);

    build_desktop_popover(ns);
    GtkGesture *rclick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), GDK_BUTTON_SECONDARY);
    g_signal_connect(rclick, "pressed", G_CALLBACK(on_desktop_right_click), ns);
    gtk_widget_add_controller(ns->desktop_overlay, GTK_EVENT_CONTROLLER(rclick));

    gtk_window_set_child(win, ns->desktop_overlay);
    ns->desktop_win = win;
    ns->wallpaper = get_wallpaper_pref();
    rebuild_desktop_shortcuts(ns);
    gtk_window_present(win);
}

/* ==================== 3. Native Cairo Frosted Bottom Dock ==================== */

constexpr double kDockHeight = 124.0;
constexpr double kDockBottomPad = 6.0;
constexpr double kDockIconBase = 48.0;
constexpr double kDockGap = 8.0;
constexpr double kDockPadX = 14.0;
constexpr double kDockDividerW = 14.0;

int dock_total_items(const NativeShellState *ns) {
    return static_cast<int>(ns->pinned_items.size() + ns->extra_items.size());
}

const DockItem *dock_item_at(const NativeShellState *ns, int idx) {
    int p = static_cast<int>(ns->pinned_items.size());
    if (idx >= 0 && idx < p) return &ns->pinned_items[idx];
    int e = idx - p;
    if (e >= 0 && e < static_cast<int>(ns->extra_items.size())) return &ns->extra_items[e];
    return nullptr;
}

DockItem *dock_item_at_mut(NativeShellState *ns, int idx) {
    int p = static_cast<int>(ns->pinned_items.size());
    if (idx >= 0 && idx < p) return &ns->pinned_items[idx];
    int e = idx - p;
    if (e >= 0 && e < static_cast<int>(ns->extra_items.size())) return &ns->extra_items[e];
    return nullptr;
}

void compute_dock_layout(const NativeShellState *ns,
                         int widget_w,
                         double &pill_x,
                         double &pill_w,
                         std::vector<double> &centers) {
    int p = static_cast<int>(ns->pinned_items.size());
    int e = static_cast<int>(ns->extra_items.size());
    int total = p + e;

    double content_w = 0.0;
    for (int i = 0; i < total; ++i) {
        if (i > 0) content_w += kDockGap;
        if (e > 0 && i == p) content_w += kDockDividerW;
        content_w += kDockIconBase;
    }
    pill_w = content_w + kDockPadX * 2.0;
    pill_x = (widget_w - pill_w) * 0.5;

    centers.clear();
    centers.reserve(total);
    double cur = pill_x + kDockPadX;
    for (int i = 0; i < total; ++i) {
        if (i > 0) cur += kDockGap;
        if (e > 0 && i == p) cur += kDockDividerW;
        centers.push_back(cur + kDockIconBase * 0.5);
        cur += kDockIconBase;
    }
}

gboolean on_dock_anim_tick(gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    int total = dock_total_items(ns);
    bool moving = false;

    for (int i = 0; i < total; ++i) {
        DockItem *item = dock_item_at_mut(ns, i);
        double target_sz = kDockIconBase;
        double target_lift = 0.0;
        if (ns->hover_index == i) {
            target_sz = 58.0;
            target_lift = 8.0;
        } else if (ns->hover_index >= 0 && std::abs(ns->hover_index - i) == 1) {
            target_sz = 52.0;
            target_lift = 3.0;
        }
        double d_sz = target_sz - item->anim_size;
        double d_lf = target_lift - item->anim_lift;
        if (std::abs(d_sz) > 0.25 || std::abs(d_lf) > 0.2) {
            item->anim_size += d_sz * 0.34;
            item->anim_lift += d_lf * 0.34;
            moving = true;
        } else {
            item->anim_size = target_sz;
            item->anim_lift = target_lift;
        }
    }

    if (ns->dock_da) gtk_widget_queue_draw(ns->dock_da);
    if (!moving) {
        ns->dock_anim_timer = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void ensure_dock_anim(NativeShellState *ns) {
    if (ns->dock_anim_timer == 0) {
        ns->dock_anim_timer = g_timeout_add(16, on_dock_anim_tick, ns);
    }
}

void draw_dock_cb(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    double pill_x = 0.0, pill_w = 0.0;
    std::vector<double> centers;
    compute_dock_layout(ns, w, pill_x, pill_w, centers);

    double pill_h = 66.0;
    double pill_y = h - kDockBottomPad - pill_h;
    double pill_rad = 20.0;

    // Soft glass shadow
    cairo_rounded_rect(cr, pill_x, pill_y + 3.0, pill_w, pill_h, pill_rad);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, ns->theme.is_light ? 0.16 : 0.36);
    cairo_fill(cr);

    // Translucent frosted macOS glass body driven by active ThemeSpec
    double tr, tg, tb, ta, br, bg, bb, ba, rr, rg, rb, ra;
    parse_css_rgba(ns->theme.dock_top, tr, tg, tb, ta);
    parse_css_rgba(ns->theme.dock_bot, br, bg, bb, ba);
    parse_css_rgba(ns->theme.dock_border, rr, rg, rb, ra);

    cairo_pattern_t *glass = cairo_pattern_create_linear(0, pill_y, 0, pill_y + pill_h);
    cairo_pattern_add_color_stop_rgba(glass, 0.0, tr, tg, tb, ta);
    cairo_pattern_add_color_stop_rgba(glass, 1.0, br, bg, bb, ba);
    cairo_rounded_rect(cr, pill_x, pill_y, pill_w, pill_h, pill_rad);
    cairo_set_source(cr, glass);
    cairo_fill(cr);
    cairo_pattern_destroy(glass);

    // Outer glass rim + top specular edge
    cairo_rounded_rect(cr, pill_x + 0.5, pill_y + 0.5, pill_w - 1.0, pill_h - 1.0, pill_rad - 0.5);
    cairo_set_source_rgba(cr, rr, rg, rb, ra);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // Divider before extra windows
    int p = static_cast<int>(ns->pinned_items.size());
    int e = static_cast<int>(ns->extra_items.size());
    if (e > 0 && p > 0 && p < static_cast<int>(centers.size())) {
        double dx = (centers[p - 1] + centers[p]) * 0.5;
        cairo_move_to(cr, dx, pill_y + 13.0);
        cairo_line_to(cr, dx, pill_y + pill_h - 15.0);
        if (ns->theme.is_light) cairo_set_source_rgba(cr, 0.06, 0.09, 0.16, 0.22);
        else cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.24);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    }

    // Draw non-hovered icons first, then hovered icon on top
    double base_bottom_y = pill_y + pill_h - 11.0;
    auto draw_one = [&](int idx) {
        const DockItem *item = dock_item_at(ns, idx);
        if (!item) return;
        double cx = centers[idx];
        double sz = item->anim_size;
        double ix = cx - sz * 0.5;
        double iy = base_bottom_y - item->anim_lift - sz;
        draw_app_icon(ns, cr, ix, iy, sz, item->icon_path, item->fallback_kind);

        if (item->running) {
            double dot_r = item->activated ? 2.4 : 2.0;
            double dot_y = pill_y + pill_h - 5.2;
            cairo_arc(cr, cx, dot_y, dot_r, 0, 2 * M_PI);
            if (item->minimized) {
                cairo_set_source_rgba(cr, 0.55, 0.62, 0.72, 0.72);
            } else if (ns->theme.is_light) {
                cairo_set_source_rgba(cr, 0.06, 0.09, 0.16, 0.88);
            } else {
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.96);
            }
            cairo_fill(cr);
        }
    };

    int total = dock_total_items(ns);
    for (int i = 0; i < total; ++i) {
        if (i != ns->hover_index) draw_one(i);
    }
    if (ns->hover_index >= 0 && ns->hover_index < total) {
        draw_one(ns->hover_index);

        const DockItem *hitem = dock_item_at(ns, ns->hover_index);
        if (hitem && !hitem->label.empty()) {
            int tw = 0, th = 0;
            draw_pango_text(cr, hitem->label, 0, 0, 12.0, false, 1, 1, 1, 1, &tw, &th, true);
            double pad_x = 11.0;
            double pad_y = 5.0;
            double tip_w = tw + pad_x * 2.0;
            double tip_h = th + pad_y * 2.0;
            double tip_x = std::clamp(centers[ns->hover_index] - tip_w * 0.5, 4.0, w - tip_w - 4.0);
            double tip_y = std::max(4.0, base_bottom_y - hitem->anim_lift - hitem->anim_size - tip_h - 10.0);

            cairo_rounded_rect(cr, tip_x, tip_y + 2.0, tip_w, tip_h, 8.0);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, ns->theme.is_light ? 0.14 : 0.35);
            cairo_fill(cr);

            double tbg_r, tbg_g, tbg_b, tbg_a, tfg_r, tfg_g, tfg_b, tfg_a;
            parse_css_rgba(ns->theme.dock_tip_bg, tbg_r, tbg_g, tbg_b, tbg_a);
            parse_css_rgba(ns->theme.dock_tip_fg, tfg_r, tfg_g, tfg_b, tfg_a);

            cairo_rounded_rect(cr, tip_x, tip_y, tip_w, tip_h, 8.0);
            cairo_set_source_rgba(cr, tbg_r, tbg_g, tbg_b, tbg_a);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, rr, rg, rb, ra);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);

            draw_pango_text(cr, hitem->label, tip_x + pad_x, tip_y + pad_y, 12.0, false, tfg_r, tfg_g, tfg_b, tfg_a);
        }
    }
}

int hit_test_dock(const NativeShellState *ns, double x, double y) {
    if (!ns || !ns->dock_da) return -1;
    int w = gtk_widget_get_width(ns->dock_da);
    int h = gtk_widget_get_height(ns->dock_da);
    if (y < h - 78.0 || y > h - 2.0) return -1;

    double pill_x = 0.0, pill_w = 0.0;
    std::vector<double> centers;
    compute_dock_layout(ns, w, pill_x, pill_w, centers);
    if (x < pill_x || x > pill_x + pill_w) return -1;

    int best = -1;
    double best_d = 30.0;
    for (size_t i = 0; i < centers.size(); ++i) {
        double d = std::abs(x - centers[i]);
        if (d < best_d) {
            best_d = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void on_dock_motion(GtkEventControllerMotion *, double x, double y, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    int idx = hit_test_dock(ns, x, y);
    if (idx != ns->hover_index) {
        ns->hover_index = idx;
        ensure_dock_anim(ns);
    }
}

void on_dock_leave(GtkEventControllerMotion *, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (ns->hover_index != -1) {
        ns->hover_index = -1;
        ensure_dock_anim(ns);
    }
}

void on_dock_click(GtkGestureClick *gesture, int, double x, double y, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (ns && ns->app_state && ns->cc_win && gtk_widget_get_visible(GTK_WIDGET(ns->cc_win))) {
        native_shell_set_cc_visible(ns->app_state, false);
    }
    int idx = hit_test_dock(ns, x, y);
    const DockItem *item = dock_item_at(ns, idx);
    if (!item) return;

    guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    if (btn == GDK_BUTTON_SECONDARY) {
        if (item->running && item->win_id > 0) {
            close_tracked_window(item->win_id);
        }
        return;
    }
    if (btn == GDK_BUTTON_PRIMARY) {
        if (item->running && item->win_id > 0) {
            toggle_or_activate_window(item->win_id);
        } else {
            trigger_cmd(ns->app_state, item->cmd);
        }
    }
}

void rebuild_dock_items(NativeShellState *ns, const std::vector<TrackedWindowInfo> &windows) {
    if (!ns) return;

    struct PinnedDef {
        std::string title;
        std::string cmd;
        std::string kind;
        std::vector<std::string> icon_keys;
        std::vector<std::string> win_match;
    };

    std::vector<PinnedDef> defs = {
        {"游戏与应用中心", "games:open", "games", {}, {"游戏与启动中心"}},
        {"应用商店", "store:open", "store", {}, {"spinelos 应用商店"}},
        {"Steam", "launch:steam", "steam", {"steam"}, {"steam"}},
        {"Gamescope 主机大屏", "launch:steam-gamepadui", "gamepad", {}, {"gamescope"}},
        {"文件管理", "launch:files", "files", {}, {"thunar"}},
        {"终端", "launch:terminal", "terminal", {}, {"ptyxis", "org.gnome.ptyxis", "foot"}},
        {"系统设置", "settings:open:display", "settings", {}, {"spinelos 系统设置"}}
    };
    if (ns->telemetry.is_live) {
        defs.push_back({"安装 SpinelOS 到硬盘", "installer:open", "installer", {}, {"安装 spinel"}});
    }

    // Preserve previous anim states cleanly
    std::map<std::string, std::pair<double, double>> prev_anim;
    for (const auto &it : ns->pinned_items) prev_anim[it.cmd] = {it.anim_size, it.anim_lift};
    for (const auto &it : ns->extra_items) prev_anim[it.cmd] = {it.anim_size, it.anim_lift};

    std::vector<uint64_t> matched_ids;
    ns->pinned_items.clear();
    for (const auto &d : defs) {
        const TrackedWindowInfo *matched = nullptr;
        for (const auto &w : windows) {
            std::string hay = to_lower_copy(w.app_id + " " + w.title);
            for (const auto &m : d.win_match) {
                if (hay.find(m) != std::string::npos) {
                    matched = &w;
                    break;
                }
            }
            if (matched) break;
        }
        DockItem item;
        item.label = d.title;
        item.cmd = d.cmd;
        item.icon_path = find_app_icon_path(ns->desktop_apps, d.icon_keys);
        item.fallback_kind = d.kind;
        if (matched) {
            matched_ids.push_back(matched->id);
            item.win_id = matched->id;
            item.running = true;
            item.activated = matched->activated;
            item.minimized = matched->minimized;
        }
        if (auto it = prev_anim.find(item.cmd); it != prev_anim.end()) {
            item.anim_size = it->second.first;
            item.anim_lift = it->second.second;
        }
        ns->pinned_items.push_back(std::move(item));
    }

    ns->extra_items.clear();
    for (const auto &w : windows) {
        if (std::find(matched_ids.begin(), matched_ids.end(), w.id) != matched_ids.end()) continue;
        DockItem item;
        item.label = !w.title.empty() ? w.title : (!w.app_id.empty() ? w.app_id : "应用窗口");
        item.cmd = "window:toggle:" + std::to_string(w.id);
        item.icon_path = find_app_icon_path(ns->desktop_apps, {w.app_id, w.title});
        item.fallback_kind = match_app_kind(w.app_id + " " + w.title);
        item.win_id = w.id;
        item.running = true;
        item.activated = w.activated;
        item.minimized = w.minimized;
        item.closable = true;
        ns->extra_items.push_back(std::move(item));
    }

    int total = dock_total_items(ns);
    int target_w = std::clamp(total * 58 + (ns->extra_items.empty() ? 48 : 64) + 120, 480, 1600);
    if (ns->dock_win && ns->dock_da && target_w != ns->dock_width) {
        ns->dock_width = target_w;
        gtk_widget_set_size_request(ns->dock_da, target_w, static_cast<int>(kDockHeight));
        gtk_window_set_default_size(ns->dock_win, target_w, static_cast<int>(kDockHeight));
    }
    if (ns->dock_da) gtk_widget_queue_draw(ns->dock_da);
}

void build_native_dock(GtkApplication *app, NativeShellState *ns) {
    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(win, "SpinelDock");
    gtk_widget_add_css_class(GTK_WIDGET(win), "spinel-transparent");
    gtk_window_set_default_size(win, ns->dock_width, static_cast<int>(kDockHeight));

    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "spinel-dock");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_BOTTOM, static_cast<int>(kDockBottomPad));
    gtk_layer_set_exclusive_zone(win, 74);
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    ns->dock_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(ns->dock_da, ns->dock_width, static_cast<int>(kDockHeight));
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ns->dock_da), draw_dock_cb, ns, nullptr);

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_dock_motion), ns);
    g_signal_connect(motion, "leave", G_CALLBACK(on_dock_leave), ns);
    gtk_widget_add_controller(ns->dock_da, motion);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(on_dock_click), ns);
    gtk_widget_add_controller(ns->dock_da, GTK_EVENT_CONTROLLER(click));

    gtk_window_set_child(win, ns->dock_da);
    ns->dock_win = win;
    rebuild_dock_items(ns, {});
    gtk_window_present(win);
}

/* ==================== 4. Native Liquid Glass Control Center ==================== */

GtkWidget *make_cc_circle_icon(BarGlyph kind, const char *circle_cls) {
    GtkWidget *wrap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(wrap, "spinel-cc-circle");
    if (circle_cls) gtk_widget_add_css_class(wrap, circle_cls);
    gtk_widget_set_size_request(wrap, 28, 28);
    gtk_widget_set_halign(wrap, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(wrap, GTK_ALIGN_CENTER);

    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, 14, 14);
    gtk_widget_set_hexpand(da, TRUE);
    gtk_widget_set_vexpand(da, TRUE);
    gtk_widget_set_halign(da, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(da, GTK_ALIGN_CENTER);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_glyph_cb,
                                   reinterpret_cast<gpointer>(static_cast<intptr_t>(kind)), nullptr);
    gtk_box_append(GTK_BOX(wrap), da);
    return wrap;
}

GtkWidget *make_cc_pill(AppState *state,
                        BarGlyph glyph,
                        const char *circle_cls,
                        const char *title,
                        const char *sub_init,
                        const char *cmd,
                        bool has_chevron,
                        GtkWidget **out_circle,
                        GtkWidget **out_sub) {
    GtkWidget *btn = make_cmd_button(state, nullptr, "spinel-cc-pill", cmd);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);

    GtkWidget *circle = make_cc_circle_icon(glyph, circle_cls);
    if (out_circle) *out_circle = circle;

    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(col, TRUE);
    gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "spinel-cc-title");
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    GtkWidget *s = gtk_label_new(sub_init);
    gtk_widget_add_css_class(s, "spinel-cc-sub");
    gtk_widget_set_halign(s, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(s), PANGO_ELLIPSIZE_END);
    if (out_sub) *out_sub = s;
    gtk_box_append(GTK_BOX(col), t);
    gtk_box_append(GTK_BOX(col), s);

    gtk_box_append(GTK_BOX(row), circle);
    gtk_box_append(GTK_BOX(row), col);
    if (has_chevron) {
        GtkWidget *chev = gtk_label_new("›");
        gtk_widget_add_css_class(chev, "spinel-cc-chev");
        gtk_box_append(GTK_BOX(row), chev);
    }
    gtk_button_set_child(GTK_BUTTON(btn), row);
    return btn;
}

void on_cc_scale_changed(GtkRange *range, gpointer user_data) {
    auto *ctx = static_cast<CcSliderCtx *>(user_data);
    NativeShellState *ns = ctx->state->native;
    if (!ns || ns->cc_updating_sliders) return;
    int pct = static_cast<int>(std::round(gtk_range_get_value(range)));
    std::string pct_str = std::to_string(pct) + "%";
    if (ctx->is_volume) {
        if (ns->cc_vol_pct) gtk_label_set_text(GTK_LABEL(ns->cc_vol_pct), pct_str.c_str());
        trigger_cmd(ctx->state, "system:audio-set:" + std::to_string(pct));
    } else {
        if (ns->cc_bright_pct) gtk_label_set_text(GTK_LABEL(ns->cc_bright_pct), pct_str.c_str());
        trigger_cmd(ctx->state, "system:brightness-set:" + std::to_string(pct));
    }
}

gboolean on_cc_key_pressed(GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (keyval == GDK_KEY_Escape) {
        native_shell_set_cc_visible(ns->app_state, false);
        return TRUE;
    }
    return FALSE;
}

GtkWidget *make_cc_slider_card(NativeShellState *ns,
                               const char *title,
                               double min_val,
                               double def_val,
                               bool is_volume,
                               GtkWidget **out_pct,
                               GtkWidget **out_scale) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card, "spinel-cc-card");

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *lbl = gtk_label_new(title);
    gtk_widget_add_css_class(lbl, "spinel-cc-title");
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);

    GtkWidget *pct = gtk_label_new(is_volume ? "75%" : "85%");
    gtk_widget_add_css_class(pct, "spinel-cc-sub");
    *out_pct = pct;
    gtk_box_append(GTK_BOX(head), lbl);
    gtk_box_append(GTK_BOX(head), pct);

    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min_val, 100.0, 1.0);
    gtk_widget_add_css_class(scale, "spinel-cc-scale");
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_range_set_value(GTK_RANGE(scale), def_val);
    auto *ctx = new CcSliderCtx{ns->app_state, is_volume};
    ns->slider_ctxs.push_back(ctx);
    g_signal_connect(scale, "value-changed", G_CALLBACK(on_cc_scale_changed), ctx);
    *out_scale = scale;

    gtk_box_append(GTK_BOX(card), head);
    gtk_box_append(GTK_BOX(card), scale);
    return card;
}

void build_native_control_center(GtkApplication *app, NativeShellState *ns) {
    AppState *state = ns->app_state;

    // Transparent backdrop layer (strictly below OVERLAY Control Center) to dismiss on click outside
    GtkWindow *bd = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(bd, "SpinelControlCenterBackdrop");
    gtk_widget_add_css_class(GTK_WIDGET(bd), "spinel-transparent");
    gtk_layer_init_for_window(bd);
    gtk_layer_set_namespace(bd, "spinel-cc-backdrop");
    gtk_layer_set_layer(bd, GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_anchor(bd, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(bd, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(bd, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(bd, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_margin(bd, GTK_LAYER_SHELL_EDGE_TOP, 28);
    gtk_layer_set_exclusive_zone(bd, -1);
    gtk_layer_set_keyboard_mode(bd, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    g_signal_connect(bd, "close-request", G_CALLBACK(+[](GtkWindow *, gpointer d) -> gboolean {
        auto *n = static_cast<NativeShellState *>(d);
        if (n && n->app_state) native_shell_set_cc_visible(n->app_state, false);
        return TRUE;
    }), ns);
    GtkWidget *bd_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(bd_area, TRUE);
    gtk_widget_set_vexpand(bd_area, TRUE);
    GtkGesture *bd_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(bd_click), 0);
    g_signal_connect(bd_click, "pressed", G_CALLBACK(+[](GtkGestureClick *, int, double, double, gpointer d) {
        auto *n = static_cast<NativeShellState *>(d);
        if (n && n->app_state) native_shell_set_cc_visible(n->app_state, false);
    }), ns);
    gtk_widget_add_controller(bd_area, GTK_EVENT_CONTROLLER(bd_click));
    gtk_window_set_child(bd, bd_area);
    ns->cc_backdrop_win = bd;

    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(win, "SpinelControlCenter");
    gtk_widget_add_css_class(GTK_WIDGET(win), "spinel-transparent");
    gtk_window_set_default_size(win, 344, -1);

    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "spinel-controlcenter");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_TOP, 34);
    gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_RIGHT, 12);
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    g_signal_connect(win, "close-request", G_CALLBACK(+[](GtkWindow *, gpointer d) -> gboolean {
        auto *n = static_cast<NativeShellState *>(d);
        if (n && n->app_state) native_shell_set_cc_visible(n->app_state, false);
        return TRUE;
    }), ns);
    g_signal_connect(win, "notify::is-active", G_CALLBACK(+[](GObject *obj, GParamSpec *, gpointer d) {
        auto *w = GTK_WINDOW(obj);
        auto *n = static_cast<NativeShellState *>(d);
        if (!n) return;
        if (gtk_window_is_active(w)) {
            n->cc_was_active = true;
        } else if (n->cc_was_active && gtk_widget_get_visible(GTK_WIDGET(w))) {
            native_shell_set_cc_visible(n->app_state, false);
        }
    }), ns);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_cc_key_pressed), ns);
    gtk_widget_add_controller(GTK_WIDGET(win), key);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(root, "spinel-cc-panel");

    // Top Bento Grid: Left grouped connectivity card + Right status & MangoHud column
    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(top_row), TRUE);

    GtkWidget *conn_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(conn_card, "spinel-cc-card");
    gtk_widget_add_css_class(conn_card, "spinel-cc-group");
    gtk_widget_set_hexpand(conn_card, TRUE);
    gtk_box_append(GTK_BOX(conn_card), make_cc_pill(state, BarGlyph::Wifi, "active-blue", "网络", "检测中", "settings:open:wifi", true, nullptr, &ns->cc_net_val));
    gtk_box_append(GTK_BOX(conn_card), make_cc_pill(state, BarGlyph::Bluetooth, "active-blue", "蓝牙", "设备与手柄", "settings:open:bt", true, nullptr, nullptr));
    gtk_box_append(GTK_BOX(conn_card), make_cc_pill(state, BarGlyph::Gamepad, "active-blue", "GameMode", "已开启", "perf:toggle-gamemode", false, &ns->cc_gm_circle, &ns->cc_gm_val));

    GtkWidget *right_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(right_col, TRUE);

    GtkWidget *hub = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_add_css_class(hub, "spinel-cc-card");
    gtk_widget_set_vexpand(hub, TRUE);

    GtkWidget *hub_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *hub_gem = gtk_drawing_area_new();
    gtk_widget_set_size_request(hub_gem, 22, 22);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(hub_gem), draw_gem_glyph, nullptr, nullptr);
    ns->cc_gpu_badge = gtk_label_new("GPU");
    gtk_widget_add_css_class(ns->cc_gpu_badge, "spinel-cc-badge");
    gtk_widget_set_hexpand(ns->cc_gpu_badge, TRUE);
    gtk_widget_set_halign(ns->cc_gpu_badge, GTK_ALIGN_END);
    gtk_label_set_ellipsize(GTK_LABEL(ns->cc_gpu_badge), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(ns->cc_gpu_badge), 12);
    gtk_box_append(GTK_BOX(hub_top), hub_gem);
    gtk_box_append(GTK_BOX(hub_top), ns->cc_gpu_badge);

    GtkWidget *hub_title = gtk_label_new("SpinelOS");
    gtk_widget_add_css_class(hub_title, "spinel-cc-title");
    gtk_widget_set_halign(hub_title, GTK_ALIGN_START);
    ns->cc_mem_val = gtk_label_new("内存 --");
    gtk_widget_add_css_class(ns->cc_mem_val, "spinel-cc-sub");
    gtk_widget_set_halign(ns->cc_mem_val, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(hub), hub_top);
    gtk_box_append(GTK_BOX(hub), hub_title);
    gtk_box_append(GTK_BOX(hub), ns->cc_mem_val);

    GtkWidget *mh_card = make_cc_pill(state, BarGlyph::Hud, "active-indigo", "MangoHud", "精简监控", "perf:cycle-mangohud", false, &ns->cc_mh_circle, &ns->cc_mh_val);
    gtk_widget_add_css_class(mh_card, "spinel-cc-card-btn");

    gtk_box_append(GTK_BOX(right_col), hub);
    gtk_box_append(GTK_BOX(right_col), mh_card);

    gtk_box_append(GTK_BOX(top_row), conn_card);
    gtk_box_append(GTK_BOX(top_row), right_col);

    // Sliders
    GtkWidget *br_card = make_cc_slider_card(ns, "显示器亮度", 15.0, 85.0, false, &ns->cc_bright_pct, &ns->cc_bright_scale);
    GtkWidget *vo_card = make_cc_slider_card(ns, "声音输出", 0.0, 75.0, true, &ns->cc_vol_pct, &ns->cc_vol_scale);

    // Bottom Clean Quick Nav Row (no ugly circle orbs)
    GtkWidget *foot_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(foot_row), TRUE);
    gtk_box_append(GTK_BOX(foot_row), make_cmd_button(state, "游戏中心", "spinel-cc-chip-btn", "games:open"));
    gtk_box_append(GTK_BOX(foot_row), make_cmd_button(state, "应用商店", "spinel-cc-chip-btn", "store:open"));
    gtk_box_append(GTK_BOX(foot_row), make_cmd_button(state, "主机大屏", "spinel-cc-chip-btn", "launch:steam-gamepadui"));
    gtk_box_append(GTK_BOX(foot_row), make_cmd_button(state, "系统设置", "spinel-cc-chip-btn", "settings:open:audio"));

    gtk_box_append(GTK_BOX(root), top_row);
    gtk_box_append(GTK_BOX(root), br_card);
    gtk_box_append(GTK_BOX(root), vo_card);
    gtk_box_append(GTK_BOX(root), foot_row);

    gtk_window_set_child(win, root);
    ns->cc_win = win;
}

void apply_native_css(NativeShellState *ns) {
    if (!ns) return;
    const ThemeSpec &t = ns->theme;
    const char *hover_bg = t.is_light ? "rgba(15, 23, 42, 0.10)" : "rgba(255, 255, 255, 0.18)";
    const char *open_bg = t.is_light ? "rgba(15, 23, 42, 0.16)" : "rgba(255, 255, 255, 0.28)";
    const char *sc_hover = t.is_light ? "rgba(255, 255, 255, 0.45)" : "rgba(255, 255, 255, 0.12)";
    const char *sc_lbl_shadow = t.is_light ? "0 1px 2px rgba(255, 255, 255, 0.75)" : "0 1px 3px rgba(0, 0, 0, 0.85)";
    const char *cc_sub_fg = t.is_light ? "rgba(15, 23, 42, 0.72)" : "rgba(255, 255, 255, 0.78)";
    const char *cc_chip_bg = t.is_light ? "rgba(15, 23, 42, 0.05)" : "rgba(255, 255, 255, 0.08)";
    const char *cc_chip_hover = t.is_light ? "rgba(15, 23, 42, 0.10)" : "rgba(255, 255, 255, 0.14)";
    const char *trough_bg = t.is_light ? "rgba(15, 23, 42, 0.14)" : "rgba(255, 255, 255, 0.22)";
    const char *fill_bg = t.is_light ? t.cc_accent.c_str() : "rgba(255, 255, 255, 0.90)";

    std::string css_str;
    css_str.reserve(4096);
    css_str +=
        "window.spinel-transparent, window.spinel-transparent.background {\n"
        "  background-color: transparent;\n"
        "  background-image: none;\n"
        "  box-shadow: none;\n"
        "  border: none;\n"
        "}\n"
        ".spinel-topbar {\n"
        "  background-color: " + t.bar_bg + ";\n"
        "  border-bottom: 1px solid " + t.bar_border + ";\n"
        "  color: " + t.bar_fg + ";\n"
        "  padding: 0 12px;\n"
        "}\n"
        ".spinel-bar-active {\n"
        "  color: " + t.bar_fg + ";\n"
        "  font-weight: 700;\n"
        "  font-size: 12.5px;\n"
        "  padding: 0 8px 0 4px;\n"
        "}\n"
        ".spinel-bar-btn, .spinel-bar-icon-btn, .spinel-bar-menubtn > button {\n"
        "  background: transparent;\n"
        "  background-image: none;\n"
        "  border: none;\n"
        "  box-shadow: none;\n"
        "  color: " + t.bar_fg + ";\n"
        "  font-size: 12px;\n"
        "  font-weight: 500;\n"
        "  padding: 2px 8px;\n"
        "  min-height: 20px;\n"
        "  min-width: 20px;\n"
        "  border-radius: 5px;\n"
        "}\n"
        ".spinel-bar-icon-btn, .spinel-bar-menubtn > button { padding: 2px 6px; }\n"
        ".spinel-bar-btn:hover, .spinel-bar-icon-btn:hover, .spinel-bar-menubtn > button:hover { background-color: " + std::string(hover_bg) + "; }\n"
        ".spinel-bar-icon-btn.cc-open { background-color: " + std::string(open_bg) + "; }\n"
        ".spinel-bar-icon-btn.danger-hover:hover, .spinel-bar-menubtn.danger-hover > button:hover { background-color: rgba(244, 63, 94, 0.34); }\n"
        ".spinel-bar-mem, .spinel-bar-clock {\n"
        "  color: " + t.bar_fg + ";\n"
        "  font-size: 12px;\n"
        "  font-weight: 500;\n"
        "}\n"
        ".spinel-bar-clock { padding-left: 6px; }\n"
        ".spinel-shortcut-btn {\n"
        "  background: transparent;\n"
        "  background-image: none;\n"
        "  border: 1px solid transparent;\n"
        "  box-shadow: none;\n"
        "  border-radius: 12px;\n"
        "  padding: 5px 4px;\n"
        "}\n"
        ".spinel-shortcut-btn:hover {\n"
        "  background-color: " + std::string(sc_hover) + ";\n"
        "  border-color: " + t.bar_border + ";\n"
        "}\n"
        ".spinel-shortcut-label {\n"
        "  color: " + t.bar_fg + ";\n"
        "  font-size: 11.5px;\n"
        "  font-weight: 600;\n"
        "  text-shadow: " + std::string(sc_lbl_shadow) + ";\n"
        "}\n"
        ".spinel-ctx-popover contents {\n"
        "  background-color: " + t.popover_bg + ";\n"
        "  background-image: none;\n"
        "  border: 1px solid " + t.cc_border + ";\n"
        "  border-radius: 12px;\n"
        "  padding: 6px;\n"
        "  box-shadow: 0 14px 32px rgba(0, 0, 0, 0.42);\n"
        "}\n"
        ".spinel-ctx-item {\n"
        "  background: transparent;\n"
        "  background-image: none;\n"
        "  border: none;\n"
        "  box-shadow: none;\n"
        "  color: " + t.bar_fg + ";\n"
        "  font-size: 12.5px;\n"
        "  min-width: 190px;\n"
        "  padding: 6px 10px;\n"
        "  border-radius: 8px;\n"
        "}\n"
        ".spinel-ctx-item:hover { background-color: " + std::string(hover_bg) + "; }\n"
        ".spinel-clip-head { font-size: 12px; font-weight: 700; color: " + t.bar_fg + "; padding: 2px 4px; }\n"
        ".spinel-clip-clr { background: transparent; border: none; box-shadow: none; font-size: 11.5px; padding: 2px 8px; border-radius: 6px; color: " + t.cc_accent + "; }\n"
        ".spinel-clip-clr:hover { background-color: " + std::string(hover_bg) + "; }\n"
        ".spinel-clip-empty { font-size: 11.5px; color: " + std::string(cc_sub_fg) + "; padding: 14px 8px; }\n"
        ".spinel-clip-row { background: transparent; background-image: none; border: none; box-shadow: none; padding: 6px 8px; border-radius: 8px; color: " + t.bar_fg + "; }\n"
        ".spinel-clip-row:hover { background-color: " + std::string(hover_bg) + "; }\n"
        ".spinel-clip-idx { font-size: 11px; font-weight: 700; min-width: 18px; padding: 1px 5px; border-radius: 5px; background-color: " + std::string(cc_chip_bg) + "; color: " + t.bar_fg + "; }\n"
        ".spinel-clip-txt { font-size: 12px; color: " + t.bar_fg + "; }\n"
        ".spinel-clip-meta { font-size: 11px; color: " + std::string(cc_sub_fg) + "; }\n"
        ".spinel-cc-panel {\n"
        "  background-color: " + t.popover_bg + ";\n"
        "  border: 1px solid " + t.cc_border + ";\n"
        "  border-radius: 18px;\n"
        "  padding: 12px;\n"
        "  box-shadow: 0 18px 40px rgba(0, 0, 0, 0.36);\n"
        "  color: " + t.bar_fg + ";\n"
        "}\n"
        ".spinel-cc-card, .spinel-cc-card-btn {\n"
        "  background-color: " + std::string(cc_chip_bg) + ";\n"
        "  background-image: none;\n"
        "  border: 1px solid " + t.bar_border + ";\n"
        "  box-shadow: none;\n"
        "  border-radius: 12px;\n"
        "  padding: 10px 11px;\n"
        "  color: " + t.bar_fg + ";\n"
        "}\n"
        ".spinel-cc-group { padding: 5px; }\n"
        ".spinel-cc-pill {\n"
        "  background: transparent;\n"
        "  background-image: none;\n"
        "  border: none;\n"
        "  box-shadow: none;\n"
        "  border-radius: 8px;\n"
        "  padding: 5px 6px;\n"
        "  color: " + t.bar_fg + ";\n"
        "}\n"
        ".spinel-cc-pill:hover, .spinel-cc-card-btn:hover { background-color: " + std::string(hover_bg) + "; }\n"
        ".spinel-cc-card-btn { padding: 8px 10px; }\n"
        ".spinel-cc-circle {\n"
        "  border-radius: 999px;\n"
        "  background-color: " + std::string(cc_chip_bg) + ";\n"
        "  color: " + t.bar_fg + ";\n"
        "}\n"
        ".spinel-cc-circle.active-blue, .spinel-cc-circle.active-indigo {\n"
        "  background-color: " + t.cc_accent + ";\n"
        "  color: #ffffff;\n"
        "}\n"
        ".spinel-cc-title { font-size: 12px; font-weight: 600; color: " + t.bar_fg + "; }\n"
        ".spinel-cc-sub { font-size: 10.5px; color: " + std::string(cc_sub_fg) + "; }\n"
        ".spinel-cc-chev { font-size: 14px; color: " + std::string(cc_sub_fg) + "; }\n"
        ".spinel-cc-badge {\n"
        "  font-size: 10px; font-weight: 600; padding: 2px 7px; border-radius: 999px;\n"
        "  background-color: " + std::string(cc_chip_bg) + "; color: " + t.bar_fg + ";\n"
        "}\n"
        ".spinel-cc-chip-btn {\n"
        "  background-color: " + std::string(cc_chip_bg) + ";\n"
        "  background-image: none;\n"
        "  border: 1px solid " + t.bar_border + ";\n"
        "  box-shadow: none;\n"
        "  border-radius: 10px; color: " + t.bar_fg + ";\n"
        "  font-size: 11.5px; font-weight: 600; padding: 6px 4px; min-height: 28px;\n"
        "}\n"
        ".spinel-cc-chip-btn:hover { background-color: " + std::string(cc_chip_hover) + "; }\n"
        ".spinel-cc-scale trough { min-height: 12px; border-radius: 999px; background-color: " + std::string(trough_bg) + "; }\n"
        ".spinel-cc-scale highlight { min-height: 12px; border-radius: 999px; background-color: " + std::string(fill_bg) + "; }\n"
        ".spinel-cc-scale slider { min-width: 15px; min-height: 15px; border-radius: 999px; background-color: #ffffff; box-shadow: 0 2px 5px rgba(0, 0, 0, 0.30); }\n";

    GdkDisplay *display = gdk_display_get_default();
    if (!display) return;
    if (!ns->css_provider) {
        ns->css_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            display, GTK_STYLE_PROVIDER(ns->css_provider), GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    gtk_css_provider_load_from_string(ns->css_provider, css_str.c_str());
}

}  // namespace

void init_native_shell(GtkApplication *app, AppState *state) {
    if (!state || state->native) return;
    auto *ns = new NativeShellState();
    ns->app_state = state;
    state->native = ns;

    ns->theme = get_active_theme();
    apply_native_css(ns);
    ns->telemetry = collect_telemetry();
    ns->desktop_apps = list_desktop_apps();

    build_native_desktop(app, ns);
    build_native_top_bar(app, ns);
    build_native_dock(app, ns);
    build_native_control_center(app, ns);

    native_shell_sync_telemetry(state);
    native_shell_sync_perf(state);
}

void shutdown_native_shell(AppState *state) {
    if (!state || !state->native) return;
    NativeShellState *ns = state->native;
    if (ns->clock_timer_id) g_source_remove(ns->clock_timer_id);
    if (ns->dock_anim_timer) g_source_remove(ns->dock_anim_timer);
    if (ns->css_provider) {
        if (GdkDisplay *display = gdk_display_get_default()) {
            gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(ns->css_provider));
        }
        g_object_unref(ns->css_provider);
    }
    for (auto &kv : ns->surface_cache) {
        if (kv.second) cairo_surface_destroy(kv.second);
    }
    for (auto *ctx : ns->slider_ctxs) delete ctx;
    delete ns;
    state->native = nullptr;
}

void native_shell_sync_telemetry(AppState *state) {
    if (!state || !state->native) return;
    NativeShellState *ns = state->native;
    bool prev_live = ns->telemetry.is_live;
    ns->telemetry = collect_telemetry();

    if (ns->bar_mem_lbl) {
        std::string m = ns->telemetry.mem_used_mb > 0
            ? (std::to_string(ns->telemetry.mem_used_mb) + "M")
            : ns->telemetry.mem;
        gtk_label_set_text(GTK_LABEL(ns->bar_mem_lbl), m.c_str());
    }
    if (ns->bar_bat_lbl) {
        if (ns->telemetry.battery_pct >= 0) {
            std::string b = (ns->telemetry.battery_charging ? "充电 " : "电量 ") +
                            std::to_string(ns->telemetry.battery_pct) + "%";
            gtk_label_set_text(GTK_LABEL(ns->bar_bat_lbl), b.c_str());
            gtk_widget_set_visible(ns->bar_bat_lbl, TRUE);
        } else {
            gtk_widget_set_visible(ns->bar_bat_lbl, FALSE);
        }
    }
    if (ns->bar_net_btn) {
        std::string tip = "网络: " + ns->telemetry.net;
        gtk_widget_set_tooltip_text(ns->bar_net_btn, tip.c_str());
    }
    if (ns->cc_net_val) gtk_label_set_text(GTK_LABEL(ns->cc_net_val), ns->telemetry.net.c_str());
    if (ns->cc_gpu_badge) gtk_label_set_text(GTK_LABEL(ns->cc_gpu_badge), ns->telemetry.gpu.c_str());
    if (ns->cc_mem_val) {
        std::string mem_s = "内存 " + ns->telemetry.mem;
        gtk_label_set_text(GTK_LABEL(ns->cc_mem_val), mem_s.c_str());
    }
    if (prev_live != ns->telemetry.is_live) {
        rebuild_desktop_shortcuts(ns);
        rebuild_dock_items(ns, list_tracked_windows());
    }
}

void native_shell_sync_windows(AppState *state) {
    if (!state || !state->native) return;
    NativeShellState *ns = state->native;
    auto windows = list_tracked_windows();

    const TrackedWindowInfo *active_win = nullptr;
    for (const auto &w : windows) {
        if (w.activated) {
            active_win = &w;
            break;
        }
    }
    if (!active_win && !windows.empty()) {
        active_win = &windows.front();
    }

    std::string active_name = "访达";
    sync_top_bar_app_menus(ns, active_win, active_name);
    if (ns->bar_active_lbl) gtk_label_set_text(GTK_LABEL(ns->bar_active_lbl), active_name.c_str());

    rebuild_dock_items(ns, windows);
}

void native_shell_sync_desktop(AppState *state) {
    if (!state || !state->native) return;
    state->native->desktop_apps = list_desktop_apps();
    rebuild_desktop_shortcuts(state->native);
}

void queue_draw_tree(GtkWidget *w) {
    if (!w) return;
    if (GTK_IS_DRAWING_AREA(w)) gtk_widget_queue_draw(w);
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
        queue_draw_tree(c);
    }
}

void native_shell_sync_theme(AppState *state) {
    if (!state || !state->native) return;
    NativeShellState *ns = state->native;
    ns->theme = get_active_theme();
    apply_native_css(ns);
    if (ns->desktop_bg_da) gtk_widget_queue_draw(ns->desktop_bg_da);
    if (ns->dock_da) gtk_widget_queue_draw(ns->dock_da);
    if (ns->desktop_win) gtk_widget_queue_draw(GTK_WIDGET(ns->desktop_win));
    if (ns->dock_win) gtk_widget_queue_draw(GTK_WIDGET(ns->dock_win));
    if (ns->bar_win) {
        queue_draw_tree(GTK_WIDGET(ns->bar_win));
        gtk_widget_queue_draw(GTK_WIDGET(ns->bar_win));
    }
    if (ns->cc_win) {
        queue_draw_tree(GTK_WIDGET(ns->cc_win));
        gtk_widget_queue_draw(GTK_WIDGET(ns->cc_win));
    }
}

void native_shell_sync_wallpaper(AppState *state) {
    native_shell_sync_theme(state);
}

void native_shell_sync_perf(AppState *state) {
    if (!state || !state->native) return;
    NativeShellState *ns = state->native;
    PerfConfig cfg = load_perf_config();

    if (ns->cc_gm_circle) {
        if (cfg.gamemode) gtk_widget_add_css_class(ns->cc_gm_circle, "active-blue");
        else gtk_widget_remove_css_class(ns->cc_gm_circle, "active-blue");
    }
    if (ns->cc_gm_val) gtk_label_set_text(GTK_LABEL(ns->cc_gm_val), cfg.gamemode ? "已开启" : "已关闭");

    if (ns->cc_mh_circle) {
        if (cfg.mangohud != "off") gtk_widget_add_css_class(ns->cc_mh_circle, "active-indigo");
        else gtk_widget_remove_css_class(ns->cc_mh_circle, "active-indigo");
    }
    if (ns->cc_mh_val) {
        const char *lbl = cfg.mangohud == "full" ? "详细监控" : (cfg.mangohud == "off" ? "已关闭" : "精简监控");
        gtk_label_set_text(GTK_LABEL(ns->cc_mh_val), lbl);
    }
}

void native_shell_set_cc_visible(AppState *state, bool visible) {
    if (!state || !state->native || !state->native->cc_win) return;
    NativeShellState *ns = state->native;
    ns->cc_was_active = false;
    if (ns->cc_backdrop_win) {
        gtk_widget_set_visible(GTK_WIDGET(ns->cc_backdrop_win), visible ? TRUE : FALSE);
        if (visible) gtk_window_present(ns->cc_backdrop_win);
    }
    gtk_widget_set_visible(GTK_WIDGET(ns->cc_win), visible ? TRUE : FALSE);
    if (ns->bar_cc_btn) {
        if (visible) gtk_widget_add_css_class(ns->bar_cc_btn, "cc-open");
        else gtk_widget_remove_css_class(ns->bar_cc_btn, "cc-open");
    }
    if (visible) {
        native_shell_sync_telemetry(state);
        native_shell_sync_perf(state);
        MediaSnapshot media = collect_media_state();
        ns->cc_updating_sliders = true;
        if (ns->cc_vol_scale && ns->cc_vol_pct) {
            gtk_range_set_value(GTK_RANGE(ns->cc_vol_scale), media.volume);
            std::string s = std::to_string(media.volume) + "%";
            gtk_label_set_text(GTK_LABEL(ns->cc_vol_pct), s.c_str());
        }
        if (ns->cc_bright_scale && ns->cc_bright_pct) {
            if (media.brightness >= 0) {
                gtk_widget_set_sensitive(ns->cc_bright_scale, TRUE);
                gtk_range_set_value(GTK_RANGE(ns->cc_bright_scale), media.brightness);
                std::string s = std::to_string(media.brightness) + "%";
                gtk_label_set_text(GTK_LABEL(ns->cc_bright_pct), s.c_str());
            } else {
                gtk_widget_set_sensitive(ns->cc_bright_scale, FALSE);
                gtk_label_set_text(GTK_LABEL(ns->cc_bright_pct), "外接/VM");
            }
        }
        ns->cc_updating_sliders = false;
        gtk_window_present(ns->cc_win);
    }
}

void native_shell_toggle_cc(AppState *state) {
    if (!state || !state->native || !state->native->cc_win) return;
    bool vis = gtk_widget_get_visible(GTK_WIDGET(state->native->cc_win));
    native_shell_set_cc_visible(state, !vis);
}
