#include "shell_internal.hpp"
#include "steam_library.hpp"

#include <gtk4-layer-shell.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace shell_internal {

namespace {

cairo_surface_t *get_cached_wallpaper_surface(NativeShellState *ns, int w, int h) {
    if (!ns || ns->custom_wallpaper_path.empty() || w <= 0 || h <= 0) return nullptr;
    std::string key = ns->custom_wallpaper_path + "@" + std::to_string(w) + "x" + std::to_string(h);
    auto it = ns->wallpaper_surfs.find(key);
    if (it != ns->wallpaper_surfs.end()) return it->second;

    GdkPixbuf *raw = gdk_pixbuf_new_from_file(ns->custom_wallpaper_path.c_str(), nullptr);
    if (!raw) return nullptr;
    int rw = gdk_pixbuf_get_width(raw);
    int rh = gdk_pixbuf_get_height(raw);
    if (rw <= 0 || rh <= 0) {
        g_object_unref(raw);
        return nullptr;
    }

    double scale = std::max(static_cast<double>(w) / rw, static_cast<double>(h) / rh);
    int sw = std::max(w, static_cast<int>(std::ceil(rw * scale)));
    int sh = std::max(h, static_cast<int>(std::ceil(rh * scale)));
    GdkPixbuf *scaled = (rw == sw && rh == sh) ? raw : gdk_pixbuf_scale_simple(raw, sw, sh, GDK_INTERP_BILINEAR);
    if (scaled != raw) g_object_unref(raw);
    if (!scaled) return nullptr;

    int off_x = std::max(0, (sw - w) / 2);
    int off_y = std::max(0, (sh - h) / 2);
    GdkPixbuf *cropped = (sw == w && sh == h)
        ? scaled
        : gdk_pixbuf_new_subpixbuf(scaled, off_x, off_y, std::min(w, sw - off_x), std::min(h, sh - off_y));
    cairo_surface_t *surf = pixbuf_to_cairo_surface(cropped);
    if (cropped != scaled) g_object_unref(cropped);
    g_object_unref(scaled);

    if (surf) {
        ns->wallpaper_surfs[key] = surf;
    }
    return surf;
}

void draw_desktop_wallpaper(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (cairo_surface_t *wp = get_cached_wallpaper_surface(ns, w, h)) {
        cairo_set_source_surface(cr, wp, 0.0, 0.0);
        cairo_paint(cr);
        return;
    }

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

void clear_desktop_shortcut_selection(NativeShellState *ns) {
    if (!ns || !ns->desktop_flow) return;
    for (GtkWidget *fc = gtk_widget_get_first_child(ns->desktop_flow); fc; fc = gtk_widget_get_next_sibling(fc)) {
        GtkWidget *tile = GTK_IS_FLOW_BOX_CHILD(fc) ? gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(fc)) : fc;
        if (tile) gtk_widget_remove_css_class(tile, "selected");
    }
}

void on_desktop_shortcut_pressed(GtkGestureClick *gesture, int n_press, double, double, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    GtkWidget *tile = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    if (!ns || !tile) return;

    if (ns->app_state && ns->cc_win && gtk_widget_get_visible(GTK_WIDGET(ns->cc_win))) {
        native_shell_set_cc_visible(ns->app_state, false);
    }

    clear_desktop_shortcut_selection(ns);
    gtk_widget_add_css_class(tile, "selected");

    if (n_press == 2) {
        const char *cmd = static_cast<const char *>(g_object_get_data(G_OBJECT(tile), "spinel-cmd"));
        if (cmd) trigger_cmd(ns->app_state, cmd);
    }
}

void on_desktop_bg_click(GtkGestureClick *, int, double, double, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (!ns) return;
    if (ns->app_state && ns->cc_win && gtk_widget_get_visible(GTK_WIDGET(ns->cc_win))) {
        native_shell_set_cc_visible(ns->app_state, false);
    }
    clear_desktop_shortcut_selection(ns);
}

void on_popover_cmd_clicked(GtkButton *btn, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (ns->desktop_popover) gtk_popover_popdown(GTK_POPOVER(ns->desktop_popover));
    const char *cmd = static_cast<const char *>(g_object_get_data(G_OBJECT(btn), "spinel-cmd"));
    if (cmd) trigger_cmd(ns->app_state, cmd);
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

}  // namespace

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
        GtkWidget *tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_widget_add_css_class(tile, "spinel-shortcut-btn");
        gtk_widget_set_size_request(tile, 84, 84);
        gtk_widget_set_halign(tile, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(tile, GTK_ALIGN_START);
        g_object_set_data_full(G_OBJECT(tile), "spinel-cmd", g_strdup(sc.cmd.c_str()), g_free);

        GtkGesture *dbl = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(dbl), GDK_BUTTON_PRIMARY);
        g_signal_connect(dbl, "pressed", G_CALLBACK(on_desktop_shortcut_pressed), ns);
        gtk_widget_add_controller(tile, GTK_EVENT_CONTROLLER(dbl));

        GtkWidget *da = gtk_drawing_area_new();
        gtk_widget_set_size_request(da, 52, 52);
        gtk_widget_set_halign(da, GTK_ALIGN_CENTER);
        auto *ctx = new DesktopIconDrawCtx{ns, sc.icon_path, sc.kind};
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_desktop_icon_cb, ctx, free_desktop_icon_ctx);

        GtkWidget *lbl = gtk_label_new(sc.label.c_str());
        gtk_widget_add_css_class(lbl, "spinel-shortcut-label");
        gtk_label_set_max_width_chars(GTK_LABEL(lbl), 10);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_CENTER);
        gtk_label_set_lines(GTK_LABEL(lbl), 2);
        gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);

        gtk_box_append(GTK_BOX(tile), da);
        gtk_box_append(GTK_BOX(tile), lbl);
        gtk_flow_box_append(GTK_FLOW_BOX(ns->desktop_flow), tile);
    }
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
    GtkGesture *bg_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(bg_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(bg_click, "pressed", G_CALLBACK(on_desktop_bg_click), ns);
    gtk_widget_add_controller(ns->desktop_bg_da, GTK_EVENT_CONTROLLER(bg_click));

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

void sync_monitor_wallpapers(GtkApplication *app, NativeShellState *ns) {
    if (!app || !ns) return;
    GdkDisplay *display = gdk_display_get_default();
    GListModel *monitors = display ? gdk_display_get_monitors(display) : nullptr;
    guint n_mons = monitors ? g_list_model_get_n_items(monitors) : 0;

    GdkMonitor *primary = (n_mons > 0) ? GDK_MONITOR(g_list_model_get_item(monitors, 0)) : nullptr;
    if (primary) {
        GtkWindow *shell_wins[] = {
            ns->desktop_win, ns->bar_win, ns->dock_win,
            ns->cc_backdrop_win, ns->cc_win,
            ns->launcher_backdrop_win, ns->launcher_win,
            ns->osd_win
        };
        for (GtkWindow *w : shell_wins) {
            if (w && gtk_layer_is_layer_window(w)) {
                gtk_layer_set_monitor(w, primary);
            }
        }
        g_object_unref(primary);
    }

    std::vector<GdkMonitor *> extra_mons;
    for (guint i = 1; i < n_mons; ++i) {
        GdkMonitor *m = GDK_MONITOR(g_list_model_get_item(monitors, i));
        if (m) extra_mons.push_back(m);
    }

    for (auto it = ns->extra_desktop_bgs.begin(); it != ns->extra_desktop_bgs.end();) {
        bool still_present = std::find(extra_mons.begin(), extra_mons.end(), it->mon) != extra_mons.end();
        if (!still_present) {
            if (it->win) gtk_window_destroy(it->win);
            if (it->mon) g_object_unref(it->mon);
            it = ns->extra_desktop_bgs.erase(it);
        } else {
            ++it;
        }
    }

    for (GdkMonitor *m : extra_mons) {
        bool exists = false;
        for (const auto &ebg : ns->extra_desktop_bgs) {
            if (ebg.mon == m) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
            gtk_window_set_title(win, "SpinelDesktopSecondary");
            gtk_layer_init_for_window(win);
            gtk_layer_set_namespace(win, "spinel-desktop-extra");
            gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_BOTTOM);
            gtk_layer_set_monitor(win, m);
            gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
            gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
            gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
            gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
            gtk_layer_set_exclusive_zone(win, -1);
            gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

            GtkWidget *da = gtk_drawing_area_new();
            gtk_widget_set_hexpand(da, TRUE);
            gtk_widget_set_vexpand(da, TRUE);
            gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_desktop_wallpaper, ns, nullptr);
            gtk_window_set_child(win, da);
            gtk_window_present(win);

            g_object_ref(m);
            ns->extra_desktop_bgs.push_back(NativeShellState::ExtraMonitorBg{m, win, da});
        }
        g_object_unref(m);
    }
}

}  // namespace shell_internal
