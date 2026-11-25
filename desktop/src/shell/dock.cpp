#include "shell_internal.hpp"
#include "common.hpp"

#include <gtk4-layer-shell.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace shell_internal {

namespace {

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

std::string dock_pins_path() {
    return home_dir() + "/.config/spinelos/dock_pins.conf";
}

std::string sanitize_pin_field(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) {
        return c == '\t' || c == '\r' || c == '\n';
    }), s.end());
    return trim_copy(s);
}

std::string join_pin_csv(const std::vector<std::string> &items) {
    std::string out;
    for (const auto &raw : items) {
        std::string s = sanitize_pin_field(raw);
        std::replace(s.begin(), s.end(), ',', ' ');
        s = trim_copy(s);
        if (s.empty()) continue;
        if (!out.empty()) out += ",";
        out += s;
    }
    return out;
}

std::vector<std::string> split_pin_csv(const std::string &text) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        tok = trim_copy(tok);
        if (!tok.empty()) out.push_back(tok);
    }
    return out;
}

std::vector<DockPinSpec> default_dock_pins(bool is_live) {
    std::vector<DockPinSpec> defs = {
        {"游戏与应用中心", "games:open", "games", {}, {"游戏与启动中心"}},
        {"应用商店", "store:open", "store", {}, {"spinelos 应用商店"}},
        {"Steam", "launch:steam", "steam", {"steam"}, {"steam"}},
        {"Gamescope 主机大屏", "launch:steam-gamepadui", "gamepad", {}, {"gamescope"}},
        {"文件管理", "launch:files", "files", {}, {"thunar"}},
        {"终端", "launch:terminal", "terminal", {}, {"spinel-terminal", "org.spinelos.terminal", "spinel 终端", "ptyxis", "org.gnome.ptyxis", "foot"}},
        {"系统设置", "settings:open:display", "settings", {}, {"spinelos 系统设置"}}
    };
    if (is_live) {
        defs.push_back({"安装 SpinelOS 到硬盘", "installer:open", "installer", {}, {"安装 spinel"}});
    }
    return defs;
}

void save_dock_pins(const NativeShellState *ns) {
    if (!ns) return;
    std::string out = "# SpinelOS Dock Pins v1\n";
    for (const auto &p : ns->dock_pins) {
        std::string cmd = sanitize_pin_field(p.cmd);
        if (cmd.empty()) continue;
        out += cmd + "\t" + sanitize_pin_field(p.title) + "\t" +
               sanitize_pin_field(p.kind) + "\t" + join_pin_csv(p.icon_keys) + "\t" +
               join_pin_csv(p.win_match) + "\n";
    }
    write_text_file(dock_pins_path(), out);
}

void load_dock_pins(NativeShellState *ns) {
    if (!ns) return;
    ns->dock_pins_loaded = true;
    std::ifstream ifs(dock_pins_path());
    if (!ifs.is_open()) {
        ns->dock_pins = default_dock_pins(ns->telemetry.is_live);
        save_dock_pins(ns);
        return;
    }

    ns->dock_pins.clear();
    std::string line;
    while (std::getline(ifs, line)) {
        std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string col;
        while (std::getline(ss, col, '\t')) cols.push_back(col);
        if (cols.empty()) continue;
        DockPinSpec spec;
        spec.cmd = sanitize_pin_field(cols[0]);
        if (spec.cmd.empty()) continue;
        if (!ns->telemetry.is_live && spec.cmd == "installer:open") continue;
        spec.title = cols.size() > 1 ? sanitize_pin_field(cols[1]) : spec.cmd;
        spec.kind = cols.size() > 2 ? sanitize_pin_field(cols[2]) : "app";
        if (cols.size() > 3) spec.icon_keys = split_pin_csv(cols[3]);
        if (cols.size() > 4) spec.win_match = split_pin_csv(cols[4]);
        ns->dock_pins.push_back(std::move(spec));
    }
}

DockPinSpec resolve_pin_spec_from_item(const NativeShellState *ns, const DockItem &item) {
    std::string hay = to_lower_copy(item.app_id + " " + item.label);
    if (hay.find("游戏与启动中心") != std::string::npos) {
        return {"游戏与应用中心", "games:open", "games", {}, {"游戏与启动中心"}};
    }
    if (hay.find("spinelos 应用商店") != std::string::npos || hay.find("app-store") != std::string::npos) {
        return {"应用商店", "store:open", "store", {}, {"spinelos 应用商店"}};
    }
    if (hay.find("spinelos 系统设置") != std::string::npos) {
        return {"系统设置", "settings:open:display", "settings", {}, {"spinelos 系统设置"}};
    }
    if (hay.find("安装 spinel") != std::string::npos) {
        return {"安装 SpinelOS 到硬盘", "installer:open", "installer", {}, {"安装 spinel"}};
    }
    if (hay.find("gamescope") != std::string::npos) {
        return {"Gamescope 主机大屏", "launch:steam-gamepadui", "gamepad", {}, {"gamescope"}};
    }
    if (hay.find("steam") != std::string::npos) {
        return {"Steam", "launch:steam", "steam", {"steam"}, {"steam"}};
    }
    if (hay.find("thunar") != std::string::npos) {
        return {"文件管理", "launch:files", "files", {}, {"thunar"}};
    }
    if (hay.find("spinel-terminal") != std::string::npos || hay.find("org.spinelos.terminal") != std::string::npos ||
        hay.find("ptyxis") != std::string::npos || hay.find("foot") != std::string::npos) {
        return {"终端", "launch:terminal", "terminal", {}, {"spinel-terminal", "org.spinelos.terminal", "spinel 终端", "ptyxis", "org.gnome.ptyxis", "foot"}};
    }

    if (ns) {
        std::string low_id = to_lower_copy(item.app_id);
        std::string low_lbl = to_lower_copy(item.label);
        const DesktopAppInfo *found = nullptr;
        for (const auto &a : ns->desktop_apps) {
            std::string aid = to_lower_copy(a.id);
            std::string aname = to_lower_copy(a.name);
            if ((!low_id.empty() && (aid == low_id || aid == low_id + ".desktop")) ||
                (!low_lbl.empty() && aname == low_lbl)) {
                found = &a;
                break;
            }
        }
        if (!found && !low_id.empty()) {
            for (const auto &a : ns->desktop_apps) {
                std::string aid = to_lower_copy(a.id);
                if (aid.find(low_id) != std::string::npos) {
                    found = &a;
                    break;
                }
            }
        }
        if (found) {
            DockPinSpec spec;
            spec.title = !found->name.empty() ? found->name : item.label;
            spec.cmd = "app:launch:" + found->id;
            spec.kind = match_app_kind(found->id + " " + found->name);
            if (!found->id.empty()) spec.icon_keys.push_back(found->id);
            if (!found->icon_name.empty()) spec.icon_keys.push_back(found->icon_name);
            if (!item.app_id.empty()) spec.win_match.push_back(to_lower_copy(item.app_id));
            if (!found->name.empty()) spec.win_match.push_back(to_lower_copy(found->name));
            if (spec.win_match.empty() && !item.label.empty()) spec.win_match.push_back(to_lower_copy(item.label));
            return spec;
        }
    }

    DockPinSpec fallback;
    fallback.title = item.label;
    fallback.kind = item.fallback_kind.empty() ? "app" : item.fallback_kind;
    if (hay.find("chromium") != std::string::npos || hay.find("chrome") != std::string::npos ||
        hay.find("firefox") != std::string::npos) {
        fallback.cmd = "launch:browser";
    } else {
        std::string bin = to_lower_copy(item.app_id);
        if (!bin.empty() && has_executable(bin)) {
            fallback.cmd = "dock:exec:" + bin;
        }
    }
    if (!item.app_id.empty()) {
        fallback.icon_keys.push_back(item.app_id);
        fallback.win_match.push_back(to_lower_copy(item.app_id));
    }
    if (!item.label.empty()) {
        fallback.win_match.push_back(to_lower_copy(item.label));
    }
    return fallback;
}

void draw_dock_cb(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    double pill_x = 0.0, pill_w = 0.0;
    std::vector<double> centers;
    compute_dock_layout(ns, w, pill_x, pill_w, centers);

    double pill_h = 66.0;
    double pill_y = h - kDockBottomPad - pill_h;
    double pill_rad = 20.0;

    cairo_rounded_rect(cr, pill_x, pill_y + 3.0, pill_w, pill_h, pill_rad);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, ns->theme.is_light ? 0.16 : 0.36);
    cairo_fill(cr);

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

    cairo_rounded_rect(cr, pill_x + 0.5, pill_y + 0.5, pill_w - 1.0, pill_h - 1.0, pill_rad - 0.5);
    cairo_set_source_rgba(cr, rr, rg, rb, ra);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

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

    double base_bottom_y = pill_y + pill_h - 11.0;
    auto draw_one = [&](int idx, bool is_dragged) {
        const DockItem *item = dock_item_at(ns, idx);
        if (!item) return;
        double cx = is_dragged ? ns->drag_visual_cx : centers[idx];
        double sz = is_dragged ? 56.0 : item->anim_size;
        double lift = is_dragged ? 10.0 : item->anim_lift;
        double ix = cx - sz * 0.5;
        double iy = base_bottom_y - lift - sz;

        if (is_dragged) {
            cairo_rounded_rect(cr, ix + 2.0, iy + 6.0, sz - 4.0, sz - 2.0, 14.0);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.34);
            cairo_fill(cr);
        }

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
    int top_idx = (ns->drag_active && ns->drag_cur_index >= 0 && ns->drag_cur_index < total)
                      ? ns->drag_cur_index
                      : ns->hover_index;
    for (int i = 0; i < total; ++i) {
        if (i != top_idx) draw_one(i, false);
    }
    if (top_idx >= 0 && top_idx < total) {
        draw_one(top_idx, ns->drag_active && top_idx == ns->drag_cur_index);
    }

    bool popover_open = ns->dock_popover && gtk_widget_get_visible(ns->dock_popover);
    if (!ns->drag_active && !popover_open && ns->hover_index >= 0 && ns->hover_index < total) {
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
    if (ns->drag_active) return;
    int idx = hit_test_dock(ns, x, y);
    if (idx != ns->hover_index) {
        ns->hover_index = idx;
        ensure_dock_anim(ns);
    }
}

void on_dock_leave(GtkEventControllerMotion *, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (ns->drag_active) return;
    if (ns->hover_index != -1) {
        ns->hover_index = -1;
        ensure_dock_anim(ns);
    }
}

void on_dock_popover_cmd(GtkButton *btn, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (ns->dock_popover) gtk_popover_popdown(GTK_POPOVER(ns->dock_popover));
    const char *cmd = static_cast<const char *>(g_object_get_data(G_OBJECT(btn), "spinel-cmd"));
    if (cmd) trigger_cmd(ns->app_state, cmd);
}

void on_dock_popover_unpin(GtkButton *btn, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (ns->dock_popover) gtk_popover_popdown(GTK_POPOVER(ns->dock_popover));
    const char *cmd = static_cast<const char *>(g_object_get_data(G_OBJECT(btn), "dock-unpin-cmd"));
    if (!ns || !cmd) return;
    std::string target(cmd);
    auto it = std::remove_if(ns->dock_pins.begin(), ns->dock_pins.end(), [&](const DockPinSpec &p) {
        return p.cmd == target;
    });
    if (it != ns->dock_pins.end()) {
        ns->dock_pins.erase(it, ns->dock_pins.end());
        save_dock_pins(ns);
        ns->hover_index = -1;
        rebuild_dock_items(ns, list_tracked_windows());
    }
}

struct DockPinCtx {
    NativeShellState *ns = nullptr;
    DockPinSpec spec;
};

void free_dock_pin_ctx(gpointer data) {
    delete static_cast<DockPinCtx *>(data);
}

void on_dock_popover_pin(GtkButton *btn, gpointer) {
    auto *ctx = static_cast<DockPinCtx *>(g_object_get_data(G_OBJECT(btn), "dock-pin-ctx"));
    if (!ctx || !ctx->ns) return;
    NativeShellState *ns = ctx->ns;
    if (ns->dock_popover) gtk_popover_popdown(GTK_POPOVER(ns->dock_popover));
    if (ctx->spec.cmd.empty()) return;
    auto dup = std::find_if(ns->dock_pins.begin(), ns->dock_pins.end(), [&](const DockPinSpec &p) {
        return p.cmd == ctx->spec.cmd;
    });
    if (dup == ns->dock_pins.end()) {
        ns->dock_pins.push_back(ctx->spec);
        save_dock_pins(ns);
        ns->hover_index = -1;
        rebuild_dock_items(ns, list_tracked_windows());
    }
}

GtkWidget *make_dock_menu_btn(const char *label, const char *css_cls = "spinel-ctx-item") {
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(b, css_cls);
    GtkWidget *child = gtk_button_get_child(GTK_BUTTON(b));
    if (GTK_IS_LABEL(child)) {
        gtk_widget_set_halign(child, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(child), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(child), 28);
    }
    return b;
}

void show_dock_context_menu(NativeShellState *ns, int idx, double x) {
    if (!ns || !ns->dock_da) return;
    const DockItem *item = dock_item_at(ns, idx);
    if (!item) return;

    if (!ns->dock_popover) {
        GtkWidget *pop = gtk_popover_new();
        gtk_widget_set_parent(pop, ns->dock_da);
        gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_TOP);
        gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
        gtk_popover_set_autohide(GTK_POPOVER(pop), TRUE);
        gtk_widget_add_css_class(pop, "spinel-ctx-popover");

        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_popover_set_child(GTK_POPOVER(pop), box);
        ns->dock_popover = pop;
        ns->dock_popover_box = box;
    }

    while (GtkWidget *c = gtk_widget_get_first_child(ns->dock_popover_box)) {
        gtk_box_remove(GTK_BOX(ns->dock_popover_box), c);
    }

    GtkWidget *title_lbl = gtk_label_new(item->label.c_str());
    gtk_widget_add_css_class(title_lbl, "spinel-clip-head");
    gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
    gtk_widget_set_margin_start(title_lbl, 6);
    gtk_widget_set_margin_end(title_lbl, 6);
    gtk_widget_set_margin_bottom(title_lbl, 2);
    gtk_label_set_ellipsize(GTK_LABEL(title_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title_lbl), 24);
    gtk_box_append(GTK_BOX(ns->dock_popover_box), title_lbl);
    gtk_box_append(GTK_BOX(ns->dock_popover_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    DockPinSpec pin_spec = resolve_pin_spec_from_item(ns, *item);
    std::string launch_cmd = item->pinned ? item->cmd : pin_spec.cmd;

    if (item->running && item->win_id > 0) {
        const char *toggle_lbl = item->minimized ? "显示窗口" : "显示 / 最小化窗口";
        GtkWidget *b_toggle = make_dock_menu_btn(toggle_lbl);
        std::string toggle_cmd = "window:toggle:" + std::to_string(item->win_id);
        g_object_set_data_full(G_OBJECT(b_toggle), "spinel-cmd", g_strdup(toggle_cmd.c_str()), g_free);
        g_signal_connect(b_toggle, "clicked", G_CALLBACK(on_dock_popover_cmd), ns);
        gtk_box_append(GTK_BOX(ns->dock_popover_box), b_toggle);

        if (!launch_cmd.empty() && launch_cmd.rfind("window:", 0) != 0) {
            GtkWidget *b_open = make_dock_menu_btn("打开新窗口");
            g_object_set_data_full(G_OBJECT(b_open), "spinel-cmd", g_strdup(launch_cmd.c_str()), g_free);
            g_signal_connect(b_open, "clicked", G_CALLBACK(on_dock_popover_cmd), ns);
            gtk_box_append(GTK_BOX(ns->dock_popover_box), b_open);
        }
    } else if (!launch_cmd.empty()) {
        GtkWidget *b_open = make_dock_menu_btn("打开");
        g_object_set_data_full(G_OBJECT(b_open), "spinel-cmd", g_strdup(launch_cmd.c_str()), g_free);
        g_signal_connect(b_open, "clicked", G_CALLBACK(on_dock_popover_cmd), ns);
        gtk_box_append(GTK_BOX(ns->dock_popover_box), b_open);
    }

    gtk_box_append(GTK_BOX(ns->dock_popover_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    if (item->pinned) {
        GtkWidget *b_unpin = make_dock_menu_btn("从程序坞中移除");
        g_object_set_data_full(G_OBJECT(b_unpin), "dock-unpin-cmd", g_strdup(item->cmd.c_str()), g_free);
        g_signal_connect(b_unpin, "clicked", G_CALLBACK(on_dock_popover_unpin), ns);
        gtk_box_append(GTK_BOX(ns->dock_popover_box), b_unpin);
    } else if (!pin_spec.cmd.empty()) {
        GtkWidget *b_pin = make_dock_menu_btn("保留在程序坞");
        auto *ctx = new DockPinCtx{ns, pin_spec};
        g_object_set_data_full(G_OBJECT(b_pin), "dock-pin-ctx", ctx, free_dock_pin_ctx);
        g_signal_connect(b_pin, "clicked", G_CALLBACK(on_dock_popover_pin), nullptr);
        gtk_box_append(GTK_BOX(ns->dock_popover_box), b_pin);
    }

    if (item->running && item->win_id > 0) {
        gtk_box_append(GTK_BOX(ns->dock_popover_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
        GtkWidget *b_close = make_dock_menu_btn("关闭应用程序");
        std::string close_cmd = "window:close:" + std::to_string(item->win_id);
        g_object_set_data_full(G_OBJECT(b_close), "spinel-cmd", g_strdup(close_cmd.c_str()), g_free);
        g_signal_connect(b_close, "clicked", G_CALLBACK(on_dock_popover_cmd), ns);
        gtk_box_append(GTK_BOX(ns->dock_popover_box), b_close);
    }

    int w = gtk_widget_get_width(ns->dock_da);
    int h = gtk_widget_get_height(ns->dock_da);
    double pill_x = 0.0, pill_w = 0.0;
    std::vector<double> centers;
    compute_dock_layout(ns, w, pill_x, pill_w, centers);
    double anchor_x = (idx >= 0 && idx < static_cast<int>(centers.size())) ? centers[idx] : x;
    double pill_y = h - kDockBottomPad - 66.0;

    GdkRectangle rect{static_cast<int>(std::round(anchor_x)), static_cast<int>(std::round(pill_y)), 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(ns->dock_popover), &rect);
    gtk_popover_popup(GTK_POPOVER(ns->dock_popover));
    gtk_widget_queue_draw(ns->dock_da);
}

void on_dock_click_pressed(GtkGestureClick *gesture, int, double x, double y, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (!ns) return;
    if (ns->app_state && ns->cc_win && gtk_widget_get_visible(GTK_WIDGET(ns->cc_win))) {
        native_shell_set_cc_visible(ns->app_state, false);
    }
    guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    if (btn == GDK_BUTTON_PRIMARY) {
        ns->drag_just_finished = false;
        return;
    }
    if (btn == GDK_BUTTON_SECONDARY) {
        int idx = hit_test_dock(ns, x, y);
        if (idx >= 0) {
            show_dock_context_menu(ns, idx, x);
        } else if (ns->dock_popover && gtk_widget_get_visible(ns->dock_popover)) {
            gtk_popover_popdown(GTK_POPOVER(ns->dock_popover));
        }
    }
}

void on_dock_click_released(GtkGestureClick *gesture, int, double x, double y, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (!ns) return;
    guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    if (btn != GDK_BUTTON_PRIMARY) return;
    if (ns->drag_active || ns->drag_just_finished) {
        ns->drag_just_finished = false;
        return;
    }
    int idx = hit_test_dock(ns, x, y);
    const DockItem *item = dock_item_at(ns, idx);
    if (!item) return;
    if (item->running && item->win_id > 0) {
        toggle_or_activate_window(item->win_id);
    } else {
        trigger_cmd(ns->app_state, item->cmd);
    }
}

void on_dock_drag_begin(GtkGestureDrag *, double start_x, double start_y, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (!ns || !ns->dock_da) return;
    if (ns->dock_popover && gtk_widget_get_visible(ns->dock_popover)) {
        gtk_popover_popdown(GTK_POPOVER(ns->dock_popover));
    }
    int idx = hit_test_dock(ns, start_x, start_y);
    int p = static_cast<int>(ns->pinned_items.size());
    if (idx < 0 || idx >= p || p <= 1) {
        ns->drag_src_index = -1;
        ns->drag_cur_index = -1;
        ns->drag_active = false;
        return;
    }

    int w = gtk_widget_get_width(ns->dock_da);
    double pill_x = 0.0, pill_w = 0.0;
    std::vector<double> centers;
    compute_dock_layout(ns, w, pill_x, pill_w, centers);

    ns->drag_src_index = idx;
    ns->drag_cur_index = idx;
    ns->drag_press_x = start_x;
    ns->drag_grab_offset = (idx < static_cast<int>(centers.size())) ? (start_x - centers[idx]) : 0.0;
    ns->drag_visual_cx = (idx < static_cast<int>(centers.size())) ? centers[idx] : start_x;
    ns->drag_active = false;
}

void on_dock_drag_update(GtkGestureDrag *, double offset_x, double offset_y, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (!ns || !ns->dock_da || ns->drag_src_index < 0) return;

    int p = static_cast<int>(ns->pinned_items.size());
    if (p <= 1 || ns->drag_cur_index < 0 || ns->drag_cur_index >= p) return;

    if (!ns->drag_active) {
        if (std::hypot(offset_x, offset_y) < 7.0) return;
        ns->drag_active = true;
        ns->hover_index = -1;
    }

    int w = gtk_widget_get_width(ns->dock_da);
    double pill_x = 0.0, pill_w = 0.0;
    std::vector<double> centers;
    compute_dock_layout(ns, w, pill_x, pill_w, centers);
    if (static_cast<int>(centers.size()) < p) return;

    double pointer_x = ns->drag_press_x + offset_x;
    double desired_cx = std::clamp(pointer_x - ns->drag_grab_offset, centers[0] - 18.0, centers[p - 1] + 18.0);
    ns->drag_visual_cx = desired_cx;

    while (ns->drag_cur_index > 0 &&
           desired_cx < (centers[ns->drag_cur_index - 1] + centers[ns->drag_cur_index]) * 0.5) {
        std::swap(ns->pinned_items[ns->drag_cur_index], ns->pinned_items[ns->drag_cur_index - 1]);
        if (ns->dock_pins.size() == ns->pinned_items.size()) {
            std::swap(ns->dock_pins[ns->drag_cur_index], ns->dock_pins[ns->drag_cur_index - 1]);
        }
        ns->drag_cur_index--;
    }
    while (ns->drag_cur_index + 1 < p &&
           desired_cx > (centers[ns->drag_cur_index] + centers[ns->drag_cur_index + 1]) * 0.5) {
        std::swap(ns->pinned_items[ns->drag_cur_index], ns->pinned_items[ns->drag_cur_index + 1]);
        if (ns->dock_pins.size() == ns->pinned_items.size()) {
            std::swap(ns->dock_pins[ns->drag_cur_index], ns->dock_pins[ns->drag_cur_index + 1]);
        }
        ns->drag_cur_index++;
    }

    gtk_widget_queue_draw(ns->dock_da);
}

void finish_dock_drag(NativeShellState *ns, bool save_if_dragged) {
    if (!ns) return;
    bool did_drag = ns->drag_active;
    ns->drag_src_index = -1;
    ns->drag_cur_index = -1;
    ns->drag_active = false;
    ns->drag_visual_cx = 0.0;
    if (did_drag) {
        ns->drag_just_finished = true;
        if (save_if_dragged) save_dock_pins(ns);
        rebuild_dock_items(ns, list_tracked_windows());
        ensure_dock_anim(ns);
    }
}

void on_dock_drag_end(GtkGestureDrag *, double, double, gpointer user_data) {
    finish_dock_drag(static_cast<NativeShellState *>(user_data), true);
}

void on_dock_drag_cancel(GtkGesture *, GdkEventSequence *, gpointer user_data) {
    finish_dock_drag(static_cast<NativeShellState *>(user_data), false);
}

}  // namespace

void rebuild_dock_items(NativeShellState *ns, const std::vector<TrackedWindowInfo> &windows) {
    if (!ns || ns->drag_active) return;

    if (!ns->dock_pins_loaded) {
        load_dock_pins(ns);
    }

    std::map<std::string, std::pair<double, double>> prev_anim;
    for (const auto &it : ns->pinned_items) prev_anim[it.cmd] = {it.anim_size, it.anim_lift};
    for (const auto &it : ns->extra_items) prev_anim[it.cmd] = {it.anim_size, it.anim_lift};

    std::vector<uint64_t> matched_ids;
    ns->pinned_items.clear();
    for (const auto &d : ns->dock_pins) {
        const TrackedWindowInfo *matched = nullptr;
        for (const auto &w : windows) {
            std::string hay = to_lower_copy(w.app_id + " " + w.title);
            for (const auto &m : d.win_match) {
                std::string lm = to_lower_copy(m);
                if (!lm.empty() && hay.find(lm) != std::string::npos) {
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
        item.pinned = true;
        if (matched) {
            matched_ids.push_back(matched->id);
            item.app_id = matched->app_id;
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
        item.app_id = w.app_id;
        item.pinned = false;
        item.win_id = w.id;
        item.running = true;
        item.activated = w.activated;
        item.minimized = w.minimized;
        item.closable = true;
        if (auto it = prev_anim.find(item.cmd); it != prev_anim.end()) {
            item.anim_size = it->second.first;
            item.anim_lift = it->second.second;
        }
        ns->extra_items.push_back(std::move(item));
    }

    int total = dock_total_items(ns);
    int target_w = std::clamp(total * 58 + (ns->extra_items.empty() ? 48 : 64) + 120, 260, 1600);
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

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_dock_drag_begin), ns);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_dock_drag_update), ns);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_dock_drag_end), ns);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_dock_drag_cancel), ns);
    gtk_widget_add_controller(ns->dock_da, GTK_EVENT_CONTROLLER(drag));

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(on_dock_click_pressed), ns);
    g_signal_connect(click, "released", G_CALLBACK(on_dock_click_released), ns);
    gtk_widget_add_controller(ns->dock_da, GTK_EVENT_CONTROLLER(click));

    gtk_window_set_child(win, ns->dock_da);
    ns->dock_win = win;
    rebuild_dock_items(ns, {});
    gtk_window_present(win);
}

}  // namespace shell_internal
