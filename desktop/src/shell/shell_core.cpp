#include "shell_internal.hpp"
#include "shell/sni_tray.hpp"
#include "shell/sandbox_pip.hpp"
#include "ai/agent_bus.hpp"
#include "ai/os_atomics.hpp"
#include "common.hpp"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>

namespace shell_internal {

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
    if (l.find("gamescope") != std::string::npos || l.find("gravitymark") != std::string::npos ||
        l.find("glmark2") != std::string::npos) return "gamepad";
    if (l.find("chromium") != std::string::npos || l.find("chrome") != std::string::npos ||
        l.find("firefox") != std::string::npos) return "browser";
    if (l.find("spinel-files") != std::string::npos || l.find("org.spinelos.files") != std::string::npos ||
        l.find("thunar") != std::string::npos || l.find("files") != std::string::npos) return "files";
    if (l.find("spinel-terminal") != std::string::npos || l.find("org.spinelos.terminal") != std::string::npos ||
        l.find("ptyxis") != std::string::npos || l.find("foot") != std::string::npos ||
        l.find("terminal") != std::string::npos || l.find("终端") != std::string::npos) return "terminal";
    if (l.find("应用中心") != std::string::npos || l.find("应用商店") != std::string::npos ||
        l.find("app-store") != std::string::npos || l.find("org.spinelos.store") != std::string::npos) return "store";
    if (l.find("游戏中心") != std::string::npos || l.find("游戏与启动中心") != std::string::npos ||
        l.find("游戏与应用中心") != std::string::npos || l.find("app-games") != std::string::npos ||
        l.find("org.spinelos.games") != std::string::npos) return "games";
    if (l.find("系统设置") != std::string::npos || l.find("settings") != std::string::npos) return "settings";
    if (l.find("安装") != std::string::npos || l.find("installer") != std::string::npos) return "installer";
    return "app";
}

std::string find_app_icon_path(const std::vector<DesktopAppInfo> &apps,
                               const std::vector<std::string> &keys) {
    for (const auto &k : keys) {
        std::string lk = to_lower_copy(trim_copy(k));
        if (lk.empty()) continue;
        for (const auto &a : apps) {
            if (a.icon_path.empty()) continue;
            if (to_lower_copy(a.id) == lk || to_lower_copy(a.icon_name) == lk || to_lower_copy(a.name) == lk) {
                return a.icon_path;
            }
        }
    }
    for (const auto &k : keys) {
        std::string lk = to_lower_copy(trim_copy(k));
        if (lk.size() < 2) continue;
        for (const auto &a : apps) {
            if (a.icon_path.empty()) continue;
            std::string lid = to_lower_copy(a.id);
            std::string licon = to_lower_copy(a.icon_name);
            std::string lname = to_lower_copy(a.name);
            std::string hay = lid + " " + licon + " " + lname;
            if (hay.find(lk) != std::string::npos ||
                (lid.size() >= 3 && lk.find(lid) != std::string::npos) ||
                (licon.size() >= 3 && lk.find(licon) != std::string::npos) ||
                (lname.size() >= 3 && lk.find(lname) != std::string::npos)) {
                return a.icon_path;
            }
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

void set_rgb_hex(cairo_pattern_t *pat, double offset, uint32_t hex, double alpha) {
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

namespace {

void draw_system_squircle(cairo_t *cr, double x, double y, double sz, const std::string &kind) {
    double rad = sz * 0.235;
    cairo_save(cr);

    cairo_rounded_rect(cr, x, y + sz * 0.04, sz, sz, rad);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.34);
    cairo_fill(cr);

    cairo_pattern_t *bg = cairo_pattern_create_linear(x, y, x + sz, y + sz);
    if (kind == "store") {
        set_rgb_hex(bg, 0.0, 0x38BDF8);
        set_rgb_hex(bg, 0.55, 0x0EA5E9);
        set_rgb_hex(bg, 1.0, 0x1D4ED8);
    } else if (kind == "gamepad" || kind == "games") {
        set_rgb_hex(bg, 0.0, 0xF43F5E);
        set_rgb_hex(bg, 0.55, 0xA855F7);
        set_rgb_hex(bg, 1.0, 0x4F46E5);
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
    } else if (kind == "browser") {
        set_rgb_hex(bg, 0.0, 0x38BDF8);
        set_rgb_hex(bg, 0.55, 0x2563EB);
        set_rgb_hex(bg, 1.0, 0x1D4ED8);
    } else if (kind == "browser") {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.94);
        cairo_set_line_width(cr, 2.4);
        cairo_arc(cr, 32, 32, 14, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_move_to(cr, 18, 32);
        cairo_line_to(cr, 46, 32);
        cairo_stroke(cr);
        cairo_save(cr);
        cairo_translate(cr, 32, 32);
        cairo_scale(cr, 0.46, 1.0);
        cairo_arc(cr, 0, 0, 14, 0, 2 * M_PI);
        cairo_restore(cr);
        cairo_stroke(cr);
    } else if (kind == "installer") {
        set_rgb_hex(bg, 0.0, 0x34D399);
        set_rgb_hex(bg, 1.0, 0x059669);
    } else {
        set_rgb_hex(bg, 0.0, 0x38BDF8);
        set_rgb_hex(bg, 0.55, 0x6366F1);
        set_rgb_hex(bg, 1.0, 0x4338CA);
    }
    cairo_rounded_rect(cr, x, y, sz, sz, rad);
    cairo_set_source(cr, bg);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(bg);

    cairo_pattern_t *sheen = cairo_pattern_create_linear(x, y, x, y + sz);
    cairo_pattern_add_color_stop_rgba(sheen, 0.0, 1.0, 1.0, 1.0, 0.25);
    cairo_pattern_add_color_stop_rgba(sheen, 0.45, 1.0, 1.0, 1.0, 0.02);
    cairo_pattern_add_color_stop_rgba(sheen, 1.0, 0.0, 0.0, 0.0, 0.12);
    cairo_set_source(cr, sheen);
    cairo_fill(cr);
    cairo_pattern_destroy(sheen);

    cairo_rounded_rect(cr, x + 0.6, y + 0.6, sz - 1.2, sz - 1.2, rad - 0.5);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.34);
    cairo_set_line_width(cr, 1.1);
    cairo_stroke(cr);

    double s = sz / 64.0;
    cairo_translate(cr, x, y);
    cairo_scale(cr, s, s);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    if (kind == "store") {
        cairo_move_to(cr, 32, 10);
        cairo_line_to(cr, 51, 24.5);
        cairo_line_to(cr, 32, 54);
        cairo_line_to(cr, 13, 24.5);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.14);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.32);
        cairo_set_line_width(cr, 1.2);
        cairo_stroke(cr);

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.96);
        cairo_rounded_rect(cr, 17, 17, 13, 13, 3.8);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.88);
        cairo_rounded_rect(cr, 34, 17, 13, 13, 3.8);
        cairo_fill(cr);
        cairo_rounded_rect(cr, 17, 34, 13, 13, 3.8);
        cairo_fill(cr);

        cairo_move_to(cr, 40.5, 33.5);
        cairo_line_to(cr, 47, 40.5);
        cairo_line_to(cr, 40.5, 47.5);
        cairo_line_to(cr, 34, 40.5);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, 0.73, 0.90, 0.99, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
        cairo_set_line_width(cr, 1.4);
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
    } else if (kind == "browser") {
        set_rgb_hex(bg, 0.0, 0x38BDF8);
        set_rgb_hex(bg, 0.55, 0x2563EB);
        set_rgb_hex(bg, 1.0, 0x1D4ED8);
    } else if (kind == "browser") {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.94);
        cairo_set_line_width(cr, 2.4);
        cairo_arc(cr, 32, 32, 14, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_move_to(cr, 18, 32);
        cairo_line_to(cr, 46, 32);
        cairo_stroke(cr);
        cairo_save(cr);
        cairo_translate(cr, 32, 32);
        cairo_scale(cr, 0.46, 1.0);
        cairo_arc(cr, 0, 0, 14, 0, 2 * M_PI);
        cairo_restore(cr);
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

void draw_desktop_item_icon(cairo_t *cr, double x, double y, double sz, const std::string &kind) {
    double s = sz / 64.0;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, s, s);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    if (kind == "desk-dir") {
        cairo_rounded_rect(cr, 7, 13, 50, 39, 7.5);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.25);
        cairo_fill(cr);

        cairo_pattern_t *back = cairo_pattern_create_linear(7, 9, 7, 48);
        set_rgb_hex(back, 0.0, 0x38BDF8);
        set_rgb_hex(back, 1.0, 0x0284C7);
        cairo_move_to(cr, 12, 10);
        cairo_line_to(cr, 25, 10);
        cairo_line_to(cr, 30, 15.5);
        cairo_line_to(cr, 52, 15.5);
        cairo_curve_to(cr, 55, 15.5, 57, 17.5, 57, 20.5);
        cairo_line_to(cr, 57, 45);
        cairo_curve_to(cr, 57, 48.5, 54, 51, 50.5, 51);
        cairo_line_to(cr, 13.5, 51);
        cairo_curve_to(cr, 10, 51, 7, 48.5, 7, 45);
        cairo_line_to(cr, 7, 15);
        cairo_curve_to(cr, 7, 12, 9, 10, 12, 10);
        cairo_close_path(cr);
        cairo_set_source(cr, back);
        cairo_fill(cr);
        cairo_pattern_destroy(back);

        cairo_pattern_t *front = cairo_pattern_create_linear(7, 20, 7, 51);
        set_rgb_hex(front, 0.0, 0x7DD3FC);
        set_rgb_hex(front, 1.0, 0x0EA5E9);
        cairo_rounded_rect(cr, 7, 20, 50, 31, 6.5);
        cairo_set_source(cr, front);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(front);

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.34);
        cairo_set_line_width(cr, 1.2);
        cairo_stroke(cr);
    } else {
        cairo_rounded_rect(cr, 13, 8, 38, 49, 6.5);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.25);
        cairo_fill(cr);

        cairo_pattern_t *pg = cairo_pattern_create_linear(13, 6, 51, 55);
        if (kind == "desk-exec") {
            set_rgb_hex(pg, 0.0, 0x1E293B);
            set_rgb_hex(pg, 1.0, 0x0F172A);
        } else {
            set_rgb_hex(pg, 0.0, 0xFFFFFF);
            set_rgb_hex(pg, 1.0, 0xE2E8F0);
        }
        cairo_move_to(cr, 19, 6);
        cairo_line_to(cr, 40, 6);
        cairo_line_to(cr, 51, 17);
        cairo_line_to(cr, 51, 49);
        cairo_curve_to(cr, 51, 52.5, 48.5, 55, 45, 55);
        cairo_line_to(cr, 19, 55);
        cairo_curve_to(cr, 15.5, 55, 13, 52.5, 13, 49);
        cairo_line_to(cr, 13, 12);
        cairo_curve_to(cr, 13, 8.5, 15.5, 6, 19, 6);
        cairo_close_path(cr);
        cairo_set_source(cr, pg);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(pg);

        cairo_set_source_rgba(cr, kind == "desk-exec" ? 1.0 : 0.06, kind == "desk-exec" ? 1.0 : 0.14, kind == "desk-exec" ? 1.0 : 0.28, 0.20);
        cairo_set_line_width(cr, 1.2);
        cairo_stroke(cr);

        cairo_move_to(cr, 40, 6);
        cairo_line_to(cr, 40, 14.5);
        cairo_curve_to(cr, 40, 16.0, 41.0, 17.0, 42.5, 17.0);
        cairo_line_to(cr, 51, 17);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, kind == "desk-exec" ? 0.35 : 0.75, kind == "desk-exec" ? 0.45 : 0.82, kind == "desk-exec" ? 0.58 : 0.90, 0.72);
        cairo_fill(cr);

        if (kind == "desk-arc") {
            cairo_set_source_rgba(cr, 0.96, 0.62, 0.04, 0.92);
            cairo_rounded_rect(cr, 27.5, 18, 9, 26, 2.5);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
            cairo_set_line_width(cr, 2.0);
            for (int y0 = 22; y0 <= 38; y0 += 4) {
                cairo_move_to(cr, 29.5, y0);
                cairo_line_to(cr, 34.5, y0);
            }
            cairo_stroke(cr);
        } else if (kind == "desk-img") {
            cairo_rounded_rect(cr, 19, 23, 26, 22, 4);
            cairo_set_source_rgba(cr, 0.55, 0.36, 0.96, 0.18);
            cairo_fill(cr);
            cairo_move_to(cr, 21, 42);
            cairo_line_to(cr, 29, 32);
            cairo_line_to(cr, 35, 38);
            cairo_line_to(cr, 39, 34);
            cairo_line_to(cr, 44, 42);
            cairo_close_path(cr);
            cairo_set_source_rgba(cr, 0.49, 0.23, 0.93, 0.85);
            cairo_fill(cr);
            cairo_arc(cr, 26, 28.5, 2.5, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 0.96, 0.62, 0.04, 0.9);
            cairo_fill(cr);
        } else if (kind == "desk-exec") {
            cairo_set_source_rgba(cr, 0.20, 0.83, 0.60, 0.95);
            cairo_set_line_width(cr, 3.0);
            cairo_move_to(cr, 21, 29);
            cairo_line_to(cr, 28, 35);
            cairo_line_to(cr, 21, 41);
            cairo_stroke(cr);
            cairo_set_source_rgba(cr, 0.22, 0.74, 0.97, 0.95);
            cairo_move_to(cr, 32, 41);
            cairo_line_to(cr, 42, 41);
            cairo_stroke(cr);
        } else {
            cairo_set_source_rgba(cr, 0.02, 0.52, 0.78, 0.75);
            cairo_set_line_width(cr, 2.6);
            cairo_move_to(cr, 21, 27);
            cairo_line_to(cr, 43, 27);
            cairo_move_to(cr, 21, 34);
            cairo_line_to(cr, 43, 34);
            cairo_set_source_rgba(cr, 0.39, 0.45, 0.55, 0.65);
            cairo_move_to(cr, 21, 41);
            cairo_line_to(cr, 35, 41);
            cairo_stroke(cr);
        }
    }
    cairo_restore(cr);
}

std::string find_spinel_logo_path() {
    static const char *kCandidates[] = {
        "/usr/share/spinel-shell/ui/spinel-logo.png",
        "/usr/share/pixmaps/spinelos.png",
        "/usr/local/share/spinel-shell/ui/spinel-logo.png",
        "desktop/ui/spinel-logo.png"
    };
    for (const char *p : kCandidates) {
        if (g_file_test(p, G_FILE_TEST_EXISTS)) return p;
    }
    return "";
}

void draw_spinel_gem_vector(cairo_t *cr, double x, double y, double sz) {
    double s = sz / 128.0;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, s, s);

    auto tri = [&](double x1, double y1, double x2, double y2, double x3, double y3,
                   double r, double g, double b, double a = 1.0) {
        cairo_move_to(cr, x1, y1);
        cairo_line_to(cr, x2, y2);
        cairo_line_to(cr, x3, y3);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, r, g, b, a);
        cairo_fill(cr);
    };
    auto quad = [&](double x1, double y1, double x2, double y2, double x3, double y3, double x4, double y4,
                    double r, double g, double b) {
        cairo_move_to(cr, x1, y1);
        cairo_line_to(cr, x2, y2);
        cairo_line_to(cr, x3, y3);
        cairo_line_to(cr, x4, y4);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, r, g, b, 1.0);
        cairo_fill(cr);
    };

    quad(64, 8, 28, 44, 45, 64, 64, 64, 0.98, 0.38, 0.50);
    tri(64, 8, 45, 64, 64, 64, 1.00, 0.45, 0.56);
    tri(64, 8, 64, 64, 83, 64, 0.94, 0.14, 0.39);
    quad(64, 8, 83, 64, 100, 44, 64, 8, 0.49, 0.23, 0.93);
    tri(28, 44, 10, 64, 45, 64, 0.94, 0.22, 0.43);
    tri(100, 44, 118, 64, 83, 64, 0.22, 0.74, 0.97);
    tri(10, 64, 45, 64, 64, 120, 0.30, 0.11, 0.58);
    tri(45, 64, 64, 64, 64, 120, 0.93, 0.18, 0.43);
    tri(64, 64, 83, 64, 64, 120, 0.36, 0.13, 0.71);
    tri(83, 64, 118, 64, 64, 120, 0.23, 0.51, 0.96);

    cairo_set_line_width(cr, 2.0);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_source_rgba(cr, 0.99, 0.81, 0.91, 0.85);
    cairo_move_to(cr, 10, 64);
    cairo_line_to(cr, 118, 64);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 0.96, 0.82, 1.00, 0.78);
    cairo_move_to(cr, 64, 8);
    cairo_line_to(cr, 45, 64);
    cairo_line_to(cr, 64, 120);
    cairo_line_to(cr, 83, 64);
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void queue_draw_tree(GtkWidget *w) {
    if (!w) return;
    if (GTK_IS_DRAWING_AREA(w)) gtk_widget_queue_draw(w);
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
        queue_draw_tree(c);
    }
}

}  // namespace

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
    if (fallback_kind == "desk-dir" || fallback_kind == "desk-file" ||
        fallback_kind == "desk-arc" || fallback_kind == "desk-img" || fallback_kind == "desk-exec") {
        draw_desktop_item_icon(cr, x, y, sz, fallback_kind);
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
                     int *out_w,
                     int *out_h,
                     bool measure_only) {
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

void draw_desktop_icon_cb(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer user_data) {
    auto *ctx = static_cast<DesktopIconDrawCtx *>(user_data);
    double sz = std::min(w, h);
    draw_app_icon(ctx->ns, cr, (w - sz) * 0.5, (h - sz) * 0.5, sz, ctx->path, ctx->kind);
}

void free_desktop_icon_ctx(gpointer data) {
    delete static_cast<DesktopIconDrawCtx *>(data);
}

void trigger_cmd(AppState *state, const std::string &cmd) {
    if (!state || cmd.empty()) return;
    if (cmd.rfind("dock:exec:", 0) == 0) {
        std::string bin = cmd.substr(10);
        if (!bin.empty() && has_executable(bin)) {
            spawn_argv_detached({bin});
        } else if (!bin.empty()) {
            notify_app_launch_failure(bin, "找不到可执行文件。应用可能已移动或卸载。");
        }
        return;
    }
    handle_shell_command(state, cmd);
}

void on_cmd_button_clicked(GtkButton *btn, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    const char *cmd = static_cast<const char *>(g_object_get_data(G_OBJECT(btn), "spinel-cmd"));
    if (cmd) trigger_cmd(state, cmd);
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

void draw_spinel_logo_at(NativeShellState *ns, cairo_t *cr, double x, double y, double sz) {
    std::string logo_path = find_spinel_logo_path();
    int target_px = std::max(24, static_cast<int>(std::round(sz * 2.0)));
    cairo_surface_t *surf = ns && !logo_path.empty() ? get_cached_surface(ns, logo_path, target_px) : nullptr;
    if (surf) {
        int sw = cairo_image_surface_get_width(surf);
        int sh = cairo_image_surface_get_height(surf);
        if (sw > 0 && sh > 0) {
            cairo_save(cr);
            cairo_translate(cr, x, y);
            cairo_scale(cr, sz / static_cast<double>(sw), sz / static_cast<double>(sh));
            cairo_set_source_surface(cr, surf, 0.0, 0.0);
            cairo_paint(cr);
            cairo_restore(cr);
            return;
        }
    }
    draw_spinel_gem_vector(cr, x, y, sz);
}

void draw_gem_glyph(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    double sz = std::max(12.0, static_cast<double>(std::min(w, h)) - 1.0);
    draw_spinel_logo_at(ns, cr, (w - sz) * 0.5, (h - sz) * 0.5, sz);
}

void draw_monochrome_glyph(cairo_t *cr, int w, int h, BarGlyph kind, const GdkRGBA &fg) {
    // 24x24 optical design grid with consistent SF-style stroke & proportions
    double s = std::min(w, h) / 24.0;
    cairo_save(cr);
    cairo_translate(cr, (w - 24.0 * s) * 0.5, (h - 24.0 * s) * 0.5);
    cairo_scale(cr, s, s);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, fg.alpha);
    cairo_set_line_width(cr, 1.85);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    switch (kind) {
        case BarGlyph::Wifi:
            cairo_arc(cr, 12.0, 18.6, 1.55, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_arc(cr, 12.0, 18.6, 4.9, -2.34, -0.80);
            cairo_stroke(cr);
            cairo_arc(cr, 12.0, 18.6, 8.6, -2.32, -0.82);
            cairo_stroke(cr);
            cairo_arc(cr, 12.0, 18.6, 12.3, -2.29, -0.85);
            cairo_stroke(cr);
            break;
        case BarGlyph::Ethernet:
            cairo_rounded_rect(cr, 4.0, 4.2, 16.0, 12.2, 2.8);
            cairo_stroke(cr);
            cairo_move_to(cr, 8.4, 7.6);
            cairo_line_to(cr, 8.4, 10.6);
            cairo_move_to(cr, 12.0, 7.6);
            cairo_line_to(cr, 12.0, 10.6);
            cairo_move_to(cr, 15.6, 7.6);
            cairo_line_to(cr, 15.6, 10.6);
            cairo_move_to(cr, 12.0, 16.4);
            cairo_line_to(cr, 12.0, 19.6);
            cairo_move_to(cr, 7.2, 19.6);
            cairo_line_to(cr, 16.8, 19.6);
            cairo_stroke(cr);
            break;
        case BarGlyph::Offline:
            cairo_save(cr);
            cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, fg.alpha * 0.45);
            cairo_arc(cr, 12.0, 18.6, 1.5, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_arc(cr, 12.0, 18.6, 5.2, -2.30, -0.84);
            cairo_stroke(cr);
            cairo_arc(cr, 12.0, 18.6, 9.4, -2.28, -0.86);
            cairo_stroke(cr);
            cairo_restore(cr);
            cairo_move_to(cr, 4.4, 4.4);
            cairo_line_to(cr, 19.6, 19.6);
            cairo_stroke(cr);
            break;
        case BarGlyph::Search:
            cairo_arc(cr, 10.4, 10.4, 6.1, 0, 2 * M_PI);
            cairo_stroke(cr);
            cairo_set_line_width(cr, 2.15);
            cairo_move_to(cr, 15.0, 15.0);
            cairo_line_to(cr, 19.8, 19.8);
            cairo_stroke(cr);
            break;
        case BarGlyph::ControlCenter:
            cairo_set_line_width(cr, 1.65);
            cairo_rounded_rect(cr, 3.0, 4.2, 18.0, 6.6, 3.3);
            cairo_stroke(cr);
            cairo_arc(cr, 17.5, 7.5, 2.25, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_rounded_rect(cr, 3.0, 13.2, 18.0, 6.6, 3.3);
            cairo_stroke(cr);
            cairo_arc(cr, 6.5, 16.5, 2.25, 0, 2 * M_PI);
            cairo_fill(cr);
            break;
        case BarGlyph::Reboot:
            cairo_arc(cr, 12.0, 12.2, 7.0, -0.68, 1.78 * M_PI);
            cairo_stroke(cr);
            cairo_move_to(cr, 18.8, 4.4);
            cairo_line_to(cr, 18.8, 8.6);
            cairo_line_to(cr, 14.6, 8.6);
            cairo_stroke(cr);
            break;
        case BarGlyph::Power:
            cairo_move_to(cr, 12.0, 3.6);
            cairo_line_to(cr, 12.0, 11.2);
            cairo_stroke(cr);
            cairo_arc(cr, 12.0, 13.0, 7.1, -0.96, M_PI + 0.96);
            cairo_stroke(cr);
            break;
        case BarGlyph::Bluetooth:
            cairo_set_line_width(cr, 1.75);
            cairo_move_to(cr, 7.8, 7.4);
            cairo_line_to(cr, 16.4, 15.6);
            cairo_line_to(cr, 12.0, 19.8);
            cairo_line_to(cr, 12.0, 4.2);
            cairo_line_to(cr, 16.4, 8.4);
            cairo_line_to(cr, 7.8, 16.6);
            cairo_stroke(cr);
            break;
        case BarGlyph::Gamepad:
            cairo_rounded_rect(cr, 3.0, 6.8, 18.0, 10.6, 4.8);
            cairo_stroke(cr);
            cairo_move_to(cr, 7.0, 12.1);
            cairo_line_to(cr, 10.2, 12.1);
            cairo_move_to(cr, 8.6, 10.5);
            cairo_line_to(cr, 8.6, 13.7);
            cairo_stroke(cr);
            cairo_arc(cr, 15.6, 10.7, 1.05, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_arc(cr, 17.4, 12.3, 1.05, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_arc(cr, 15.6, 13.7, 1.05, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_arc(cr, 13.8, 12.3, 1.05, 0, 2 * M_PI);
            cairo_fill(cr);
            break;
        case BarGlyph::Hud:
            cairo_arc(cr, 12.0, 15.6, 8.0, M_PI, 2 * M_PI);
            cairo_stroke(cr);
            cairo_move_to(cr, 12.0, 15.6);
            cairo_line_to(cr, 16.6, 10.4);
            cairo_stroke(cr);
            cairo_arc(cr, 12.0, 15.6, 1.5, 0, 2 * M_PI);
            cairo_fill(cr);
            break;
        case BarGlyph::Appearance:
            cairo_arc(cr, 12.0, 12.0, 7.4, 0, 2 * M_PI);
            cairo_stroke(cr);
            cairo_arc(cr, 12.0, 12.0, 7.4, M_PI_2, 1.5 * M_PI);
            cairo_fill(cr);
            break;
        case BarGlyph::Settings:
            cairo_rounded_rect(cr, 3.2, 4.8, 17.6, 11.4, 2.4);
            cairo_stroke(cr);
            cairo_move_to(cr, 8.4, 19.4);
            cairo_line_to(cr, 15.6, 19.4);
            cairo_move_to(cr, 12.0, 16.2);
            cairo_line_to(cr, 12.0, 19.4);
            cairo_stroke(cr);
            break;
        case BarGlyph::Lock:
            cairo_rounded_rect(cr, 5.2, 10.6, 13.6, 9.6, 2.8);
            cairo_stroke(cr);
            cairo_arc(cr, 12.0, 9.2, 4.0, M_PI, 2 * M_PI);
            cairo_stroke(cr);
            cairo_arc(cr, 12.0, 15.4, 1.35, 0, 2 * M_PI);
            cairo_fill(cr);
            break;
        case BarGlyph::Screenshot:
            cairo_move_to(cr, 3.8, 8.4);
            cairo_line_to(cr, 3.8, 4.4);
            cairo_line_to(cr, 7.8, 4.4);
            cairo_move_to(cr, 16.2, 4.4);
            cairo_line_to(cr, 20.2, 4.4);
            cairo_line_to(cr, 20.2, 8.4);
            cairo_move_to(cr, 20.2, 15.6);
            cairo_line_to(cr, 20.2, 19.6);
            cairo_line_to(cr, 16.2, 19.6);
            cairo_move_to(cr, 7.8, 19.6);
            cairo_line_to(cr, 3.8, 19.6);
            cairo_line_to(cr, 3.8, 15.6);
            cairo_stroke(cr);
            cairo_arc(cr, 12.0, 12.0, 3.4, 0, 2 * M_PI);
            cairo_stroke(cr);
            break;
        case BarGlyph::Clipboard:
            cairo_rounded_rect(cr, 5.0, 5.2, 14.0, 15.4, 2.6);
            cairo_stroke(cr);
            cairo_rounded_rect(cr, 8.4, 2.8, 7.2, 3.8, 1.4);
            cairo_stroke(cr);
            cairo_move_to(cr, 8.4, 11.0);
            cairo_line_to(cr, 15.6, 11.0);
            cairo_move_to(cr, 8.4, 15.0);
            cairo_line_to(cr, 13.8, 15.0);
            cairo_stroke(cr);
            break;
        case BarGlyph::Volume:
            cairo_move_to(cr, 3.6, 9.6);
            cairo_line_to(cr, 7.2, 9.6);
            cairo_line_to(cr, 11.8, 5.4);
            cairo_line_to(cr, 11.8, 18.6);
            cairo_line_to(cr, 7.2, 14.4);
            cairo_line_to(cr, 3.6, 14.4);
            cairo_close_path(cr);
            cairo_stroke(cr);
            cairo_arc(cr, 13.4, 12.0, 3.8, -0.78, 0.78);
            cairo_stroke(cr);
            cairo_arc(cr, 13.4, 12.0, 6.8, -0.74, 0.74);
            cairo_stroke(cr);
            break;
        case BarGlyph::Mute:
            cairo_move_to(cr, 3.6, 9.6);
            cairo_line_to(cr, 7.2, 9.6);
            cairo_line_to(cr, 11.8, 5.4);
            cairo_line_to(cr, 11.8, 18.6);
            cairo_line_to(cr, 7.2, 14.4);
            cairo_line_to(cr, 3.6, 14.4);
            cairo_close_path(cr);
            cairo_stroke(cr);
            cairo_move_to(cr, 15.6, 8.6);
            cairo_line_to(cr, 20.6, 15.4);
            cairo_move_to(cr, 20.6, 8.6);
            cairo_line_to(cr, 15.6, 15.4);
            cairo_stroke(cr);
            break;
        case BarGlyph::Brightness:
            cairo_arc(cr, 12.0, 12.0, 4.2, 0, 2 * M_PI);
            cairo_stroke(cr);
            for (int i = 0; i < 8; ++i) {
                double ang = i * (M_PI / 4.0);
                cairo_move_to(cr, 12.0 + std::cos(ang) * 6.8, 12.0 + std::sin(ang) * 6.8);
                cairo_line_to(cr, 12.0 + std::cos(ang) * 9.0, 12.0 + std::sin(ang) * 9.0);
            }
            cairo_stroke(cr);
            break;
        case BarGlyph::Battery:
            cairo_rounded_rect(cr, 2.8, 6.8, 16.4, 10.4, 2.4);
            cairo_stroke(cr);
            cairo_rounded_rect(cr, 5.0, 9.0, 11.2, 6.0, 1.2);
            cairo_fill(cr);
            cairo_move_to(cr, 21.0, 9.8);
            cairo_line_to(cr, 21.0, 14.2);
            cairo_stroke(cr);
            break;
        case BarGlyph::Dnd:
            cairo_arc(cr, 11.4, 12.4, 7.2, 0.56 * M_PI, 1.84 * M_PI);
            cairo_arc_negative(cr, 15.8, 8.2, 5.5, 1.36 * M_PI, 0.24 * M_PI);
            cairo_close_path(cr);
            cairo_stroke(cr);
            break;
        case BarGlyph::Bell:
            cairo_move_to(cr, 5.0, 16.8);
            cairo_line_to(cr, 19.0, 16.8);
            cairo_move_to(cr, 6.6, 16.8);
            cairo_line_to(cr, 6.6, 10.6);
            cairo_arc(cr, 12.0, 10.6, 5.4, M_PI, 2.0 * M_PI);
            cairo_line_to(cr, 17.4, 16.8);
            cairo_stroke(cr);
            cairo_arc(cr, 12.0, 19.4, 1.5, 0, 2 * M_PI);
            cairo_fill(cr);
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

GtkWidget *make_glyph_icon_btn(AppState *state, BarGlyph kind, const char *tooltip, const char *cmd, const char *extra_cls) {
    GtkWidget *btn = make_cmd_button(state, nullptr, "spinel-bar-icon-btn", cmd);
    if (extra_cls) gtk_widget_add_css_class(btn, extra_cls);
    gtk_widget_set_tooltip_text(btn, tooltip);
    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, 17, 17);
    gtk_widget_set_halign(da, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(da, GTK_ALIGN_CENTER);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_glyph_cb,
                                   reinterpret_cast<gpointer>(static_cast<intptr_t>(kind)), nullptr);
    gtk_button_set_child(GTK_BUTTON(btn), da);
    return btn;
}

}  // namespace shell_internal

/* ==================== Public Native Shell Lifecycle & Sync ==================== */

void init_native_shell(GtkApplication *app, AppState *state) {
    if (!state || state->native) return;
    auto *ns = new NativeShellState();
    ns->app_state = state;
    state->native = ns;

    ns->theme = get_active_theme();
    ns->custom_wallpaper_path = get_custom_wallpaper_image();
    shell_internal::apply_native_css(ns);
    ns->telemetry = collect_telemetry();
    ns->desktop_apps = list_desktop_apps();

    bool sb_mode = state->sandbox_shell_mode;
    shell_internal::build_native_desktop(app, ns);
    shell_internal::build_native_top_bar(app, ns);
    shell_internal::build_native_dock(app, ns);
    if (!sb_mode) {
        shell_internal::build_native_control_center(app, ns);
    }
    shell_internal::build_native_launcher(app, ns);
    shell_internal::build_native_switcher(app, ns);
    if (!sb_mode) {
        shell_internal::build_osd_window(app, ns);
        init_sandbox_pip(ns);
        init_sni_tray(+[](void *d) {
            shell_internal::sync_top_bar_tray(static_cast<NativeShellState *>(d));
        }, ns);

        spinel::ai::DesktopContextHooks ai_hooks;
        auto invoke_main_sync = [](auto fn, auto fallback) {
            GMainContext *mc = g_main_context_default();
            if (mc && g_main_context_is_owner(mc)) {
                return fn();
            }
            using R = decltype(fn());
            struct SyncCall {
                std::mutex mu;
                std::condition_variable cv;
                bool done = false;
                R result;
                decltype(fn) work;
                SyncCall(R init_res, decltype(fn) w)
                    : result(std::move(init_res)), work(std::move(w)) {}
            };
            auto pkt = std::make_shared<SyncCall>(std::move(fallback), std::move(fn));
            auto *holder = new std::shared_ptr<SyncCall>(pkt);
            g_main_context_invoke(nullptr, +[](gpointer data) -> gboolean {
                auto *h = static_cast<std::shared_ptr<SyncCall> *>(data);
                R val = (*h)->work();
                {
                    std::lock_guard<std::mutex> lock((*h)->mu);
                    (*h)->result = std::move(val);
                    (*h)->done = true;
                }
                (*h)->cv.notify_all();
                delete h;
                return G_SOURCE_REMOVE;
            }, holder);
            std::unique_lock<std::mutex> lock(pkt->mu);
            if (!pkt->cv.wait_for(lock, std::chrono::milliseconds(800), [&]() { return pkt->done; })) {
                return fallback;
            }
            return std::move(pkt->result);
        };
        ai_hooks.list_windows = [invoke_main_sync]() {
            return invoke_main_sync([]() {
                std::vector<spinel::ai::WindowBindingRef> out;
                for (const auto &w : list_tracked_windows()) {
                    out.push_back({w.id, w.app_id, w.title, w.activated});
                }
                return out;
            }, std::vector<spinel::ai::WindowBindingRef>{});
        };
        ai_hooks.list_workspaces = [invoke_main_sync]() {
            return invoke_main_sync([]() {
                std::vector<spinel::ai::WorkspaceBindingRef> out;
                for (const auto &ws : list_tracked_workspaces()) {
                    out.push_back({ws.index, ws.name, ws.active, ws.urgent});
                }
                return out;
            }, std::vector<spinel::ai::WorkspaceBindingRef>{});
        };
        ai_hooks.activate_window = [](uint64_t wid) -> bool {
            if (wid == 0) return false;
            g_idle_add(+[](gpointer d) -> gboolean {
                activate_tracked_window(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(d)));
                return G_SOURCE_REMOVE;
            }, reinterpret_cast<gpointer>(static_cast<uintptr_t>(wid)));
            return true;
        };
        ai_hooks.activate_workspace = [](int idx1) -> bool {
            if (idx1 < 1) return false;
            g_idle_add(+[](gpointer d) -> gboolean {
                activate_tracked_workspace(GPOINTER_TO_INT(d) - 1);
                return G_SOURCE_REMOVE;
            }, GINT_TO_POINTER(idx1));
            return true;
        };
        spinel::ai::set_desktop_context_hooks(std::move(ai_hooks));
        spinel::ai::start_agent_bus_server();
    }
    shell_internal::sync_top_bar_workspaces(ns);
    if (!sb_mode) {
        shell_internal::sync_top_bar_tray(ns);
    }
    shell_internal::sync_monitor_wallpapers(app, ns);
    if (GdkDisplay *disp = gdk_display_get_default()) {
        if (GListModel *mons = gdk_display_get_monitors(disp)) {
            ns->monitor_model = G_LIST_MODEL(g_object_ref(mons));
            ns->monitor_sig_id = g_signal_connect(mons, "items-changed",
                G_CALLBACK(+[](GListModel *, guint, guint, guint, gpointer d) {
                    auto *n = static_cast<NativeShellState *>(d);
                    if (!n || !n->desktop_win) return;
                    shell_internal::sync_monitor_wallpapers(gtk_window_get_application(n->desktop_win), n);
                }), ns);
        }
    }
    if (!sb_mode) {
        shell_internal::show_desktop_startup_splash(app, ns);
    }

    native_shell_sync_telemetry(state);
    if (!sb_mode) {
        native_shell_sync_perf(state);
    }
}

void shutdown_native_shell(AppState *state) {
    if (!state || !state->native) return;
    NativeShellState *ns = state->native;
    shutdown_sandbox_pip(ns);
    spinel::ai::stop_agent_bus_server();
    shutdown_sni_tray();
    shell_internal::close_startup_splash(ns);
    if (ns->osd_timer) g_source_remove(ns->osd_timer);
    if (ns->clock_timer_id) g_source_remove(ns->clock_timer_id);
    if (ns->dock_anim_timer) g_source_remove(ns->dock_anim_timer);
    if (ns->monitor_model) {
        if (ns->monitor_sig_id) g_signal_handler_disconnect(ns->monitor_model, ns->monitor_sig_id);
        g_object_unref(ns->monitor_model);
        ns->monitor_model = nullptr;
    }
    if (ns->desktop_refresh_timer) {
        g_source_remove(ns->desktop_refresh_timer);
        ns->desktop_refresh_timer = 0;
    }
    if (ns->desktop_dir_monitor) {
        g_file_monitor_cancel(ns->desktop_dir_monitor);
        g_object_unref(ns->desktop_dir_monitor);
        ns->desktop_dir_monitor = nullptr;
    }
    for (auto &ebg : ns->extra_desktop_bgs) {
        if (ebg.win) gtk_window_destroy(ebg.win);
        if (ebg.mon) g_object_unref(ebg.mon);
    }
    ns->extra_desktop_bgs.clear();
    if (ns->css_provider) {
        if (GdkDisplay *display = gdk_display_get_default()) {
            gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(ns->css_provider));
        }
        g_object_unref(ns->css_provider);
    }
    for (auto &kv : ns->wallpaper_surfs) {
        if (kv.second) cairo_surface_destroy(kv.second);
    }
    ns->wallpaper_surfs.clear();
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

    static int app_refresh_tick = 0;
    if (++app_refresh_tick >= 5) {
        app_refresh_tick = 0;
        auto fresh = list_desktop_apps();
        if (fresh.size() != ns->desktop_apps.size()) {
            ns->desktop_apps = std::move(fresh);
            shell_internal::rebuild_desktop_shortcuts(ns);
            shell_internal::rebuild_dock_items(ns, list_tracked_windows());
        }
    }

    if (ns->bar_mem_lbl) {
        std::string m = ns->telemetry.mem;
        if (ns->telemetry.mem_used_mb >= 1024) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1fG", static_cast<double>(ns->telemetry.mem_used_mb) / 1024.0);
            m = buf;
        } else if (ns->telemetry.mem_used_mb > 0) {
            m = std::to_string(ns->telemetry.mem_used_mb) + "M";
        }
        gtk_label_set_text(GTK_LABEL(ns->bar_mem_lbl), m.c_str());
    }
    if (ns->bar_ime_lbl && has_executable("fcitx5-remote")) {
        std::string ime = trim_copy(run_cmd_capture("fcitx5-remote 2>/dev/null"));
        gtk_label_set_text(GTK_LABEL(ns->bar_ime_lbl), ime == "2" ? "中" : "EN");
    }
    if (state->sandbox_shell_mode) {
        return;
    }
    {
        MediaSnapshot media = collect_media_state();
        if (ns->bar_vol_da) {
            ns->bar_vol_muted = media.volume_muted;
            gtk_widget_queue_draw(ns->bar_vol_da);
        }
        if (ns->bar_vol_btn) {
            std::string tip = media.volume_muted
                ? "声音已静音（点击取消静音）"
                : ("音量 " + std::to_string(media.volume) + "%（点击静音切换）");
            gtk_widget_set_tooltip_text(ns->bar_vol_btn, tip.c_str());
        }
    }
    {
        bool dnd = get_dnd_enabled();
        if (ns->bar_notif_da && ns->bar_notif_dnd != dnd) {
            ns->bar_notif_dnd = dnd;
            gtk_widget_queue_draw(ns->bar_notif_da);
        }
    }
    if (ns->bar_bat_box && ns->bar_bat_lbl) {
        if (ns->telemetry.battery_pct >= 0) {
            std::string b = (ns->telemetry.battery_charging ? "⚡" : "") +
                            std::to_string(ns->telemetry.battery_pct) + "%";
            gtk_label_set_text(GTK_LABEL(ns->bar_bat_lbl), b.c_str());
            gtk_widget_set_visible(ns->bar_bat_box, TRUE);
        } else {
            gtk_widget_set_visible(ns->bar_bat_box, FALSE);
        }
    }
    if (ns->bar_net_da) {
        std::string next_kind = "wifi";
        if (ns->telemetry.net == "Offline") next_kind = "offline";
        else if (ns->telemetry.net.find("ethernet") != std::string::npos) next_kind = "ethernet";
        if (ns->bar_net_kind != next_kind) {
            ns->bar_net_kind = next_kind;
            gtk_widget_queue_draw(ns->bar_net_da);
        }
    }
    if (ns->bar_net_btn) {
        std::string tip = ns->telemetry.net == "Offline"
            ? "未连接网络（点击打开网络设置）"
            : ("网络: " + ns->telemetry.net);
        gtk_widget_set_tooltip_text(ns->bar_net_btn, tip.c_str());
    }
    if (ns->cc_net_val) gtk_label_set_text(GTK_LABEL(ns->cc_net_val), ns->telemetry.net.c_str());
    if (ns->cc_net_circle) {
        if (ns->telemetry.net != "Offline") gtk_widget_add_css_class(ns->cc_net_circle, "active-blue");
        else gtk_widget_remove_css_class(ns->cc_net_circle, "active-blue");
    }
    if (ns->cc_bt_circle || ns->cc_bt_val) {
        std::error_code bt_ec;
        bool has_hci = std::filesystem::exists("/sys/class/bluetooth/hci0", bt_ec);
        bool bt_soft_blocked = false;
        if (has_hci) {
            for (const auto &e : std::filesystem::directory_iterator("/sys/class/rfkill", bt_ec)) {
                if (trim_copy(read_first_line((e.path() / "type").string())) == "bluetooth") {
                    if (trim_copy(read_first_line((e.path() / "soft").string())) == "1") {
                        bt_soft_blocked = true;
                        break;
                    }
                }
            }
        }
        bool bt_on = has_hci && !bt_soft_blocked;
        if (ns->cc_bt_circle) {
            if (bt_on) gtk_widget_add_css_class(ns->cc_bt_circle, "active-blue");
            else gtk_widget_remove_css_class(ns->cc_bt_circle, "active-blue");
        }
        if (ns->cc_bt_val) {
            gtk_label_set_text(GTK_LABEL(ns->cc_bt_val), !has_hci ? "无硬件" : (bt_on ? "已开启" : "已关闭"));
        }
    }
    if (ns->cc_gpu_badge) gtk_label_set_text(GTK_LABEL(ns->cc_gpu_badge), ns->telemetry.gpu.c_str());
    if (ns->cc_mem_val) {
        std::string mem_s = "内存 " + ns->telemetry.mem;
        gtk_label_set_text(GTK_LABEL(ns->cc_mem_val), mem_s.c_str());
    }
    if (prev_live != ns->telemetry.is_live) {
        shell_internal::rebuild_desktop_shortcuts(ns);
        shell_internal::rebuild_dock_items(ns, list_tracked_windows());
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

    std::string active_name = "桌面";
    shell_internal::sync_top_bar_app_menus(ns, active_win, active_name);
    if (ns->bar_active_lbl) gtk_label_set_text(GTK_LABEL(ns->bar_active_lbl), active_name.c_str());

    shell_internal::rebuild_dock_items(ns, windows);

    bool hide_for_fullscreen = false;
    if (active_win && !active_win->minimized && active_win->fullscreen) {
        hide_for_fullscreen = true;
    } else if (!active_win) {
        for (const auto &w : windows) {
            if (!w.minimized && w.fullscreen) {
                hide_for_fullscreen = true;
                break;
            }
        }
    }
    if (hide_for_fullscreen != ns->fullscreen_hidden) {
        ns->fullscreen_hidden = hide_for_fullscreen;
        if (hide_for_fullscreen) {
            if (ns->dock_popover && gtk_widget_get_visible(ns->dock_popover)) {
                gtk_popover_popdown(GTK_POPOVER(ns->dock_popover));
            }
            if (ns->clip_popover && gtk_widget_get_visible(ns->clip_popover)) {
                gtk_popover_popdown(GTK_POPOVER(ns->clip_popover));
            }
            if (ns->notif_popover && gtk_widget_get_visible(ns->notif_popover)) {
                gtk_popover_popdown(GTK_POPOVER(ns->notif_popover));
            }
            if (ns->bar_win) gtk_widget_set_visible(GTK_WIDGET(ns->bar_win), FALSE);
            if (ns->dock_win) gtk_widget_set_visible(GTK_WIDGET(ns->dock_win), FALSE);
        } else {
            if (ns->bar_win) gtk_widget_set_visible(GTK_WIDGET(ns->bar_win), TRUE);
            if (ns->dock_win) gtk_widget_set_visible(GTK_WIDGET(ns->dock_win), TRUE);
        }
    }
}

void native_shell_sync_desktop(AppState *state) {
    if (!state || !state->native) return;
    state->native->desktop_apps = list_desktop_apps();
    shell_internal::rebuild_desktop_shortcuts(state->native);
}

void native_shell_sync_workspaces(AppState *state) {
    if (!state || !state->native) return;
    shell_internal::sync_top_bar_workspaces(state->native);
}

void native_shell_sync_theme(AppState *state) {
    if (!state || !state->native) return;
    NativeShellState *ns = state->native;
    ns->theme = get_active_theme();
    std::string next_wp = get_custom_wallpaper_image();
    if (next_wp != ns->custom_wallpaper_path) {
        ns->custom_wallpaper_path = next_wp;
        for (auto &kv : ns->wallpaper_surfs) {
            if (kv.second) cairo_surface_destroy(kv.second);
        }
        ns->wallpaper_surfs.clear();
    }
    shell_internal::apply_native_css(ns);
    if (ns->desktop_bg_da) gtk_widget_queue_draw(ns->desktop_bg_da);
    for (const auto &ebg : ns->extra_desktop_bgs) {
        if (ebg.da) gtk_widget_queue_draw(ebg.da);
        if (ebg.win) gtk_widget_queue_draw(GTK_WIDGET(ebg.win));
    }
    if (ns->dock_da) gtk_widget_queue_draw(ns->dock_da);
    if (ns->desktop_win) gtk_widget_queue_draw(GTK_WIDGET(ns->desktop_win));
    if (ns->dock_win) gtk_widget_queue_draw(GTK_WIDGET(ns->dock_win));
    if (ns->bar_win) {
        shell_internal::queue_draw_tree(GTK_WIDGET(ns->bar_win));
        gtk_widget_queue_draw(GTK_WIDGET(ns->bar_win));
    }
    if (ns->cc_win) {
        shell_internal::queue_draw_tree(GTK_WIDGET(ns->cc_win));
        gtk_widget_queue_draw(GTK_WIDGET(ns->cc_win));
    }
    sandbox_pip_on_theme_changed(ns);
}

void native_shell_sync_sandbox(AppState *state, const std::string &action) {
    if (!state || !state->native) return;
    sandbox_pip_sync_action(state->native, action);
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
    if (ns->cc_gm_dot) {
        if (cfg.gamemode) gtk_widget_add_css_class(ns->cc_gm_dot, "active-dot");
        else gtk_widget_remove_css_class(ns->cc_gm_dot, "active-dot");
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

    bool dnd = get_dnd_enabled();
    if (ns->bar_notif_da && ns->bar_notif_dnd != dnd) {
        ns->bar_notif_dnd = dnd;
        gtk_widget_queue_draw(ns->bar_notif_da);
    }
    if (ns->cc_dnd_circle) {
        if (dnd) gtk_widget_add_css_class(ns->cc_dnd_circle, "active-indigo");
        else gtk_widget_remove_css_class(ns->cc_dnd_circle, "active-indigo");
    }
    if (ns->cc_dnd_dot) {
        if (dnd) gtk_widget_add_css_class(ns->cc_dnd_dot, "active-dot");
        else gtk_widget_remove_css_class(ns->cc_dnd_dot, "active-dot");
    }
    if (ns->cc_dnd_val) gtk_label_set_text(GTK_LABEL(ns->cc_dnd_val), dnd ? "已开启" : "已关闭");
}
