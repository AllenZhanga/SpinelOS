#include "shell_internal.hpp"
#include "shell/agent_island.hpp"
#include "shell/sandbox_pip.hpp"
#include "common.hpp"
#include "steam_library.hpp"

#include <gtk4-layer-shell.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace shell_internal {

namespace {

GtkWidget *make_cc_circle_icon(BarGlyph kind, const char *circle_cls) {
    GtkWidget *wrap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(wrap, "spinel-cc-circle");
    if (circle_cls) gtk_widget_add_css_class(wrap, circle_cls);
    gtk_widget_set_size_request(wrap, 30, 30);
    gtk_widget_set_halign(wrap, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(wrap, GTK_ALIGN_CENTER);

    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, 16, 16);
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
                        GtkWidget **out_sub,
                        GtkWidget **out_dot = nullptr) {
    GtkWidget *btn = make_cmd_button(state, nullptr, "spinel-cc-pill", cmd);
    gtk_widget_set_hexpand(btn, TRUE);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_hexpand(row, TRUE);
    gtk_widget_set_valign(row, GTK_ALIGN_CENTER);

    GtkWidget *circle = make_cc_circle_icon(glyph, circle_cls);
    if (out_circle) *out_circle = circle;

    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(col, TRUE);
    gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "spinel-cc-title");
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
    GtkWidget *s = gtk_label_new(sub_init);
    gtk_widget_add_css_class(s, "spinel-cc-sub");
    gtk_widget_set_halign(s, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(s), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(s), 14);
    if (out_sub) *out_sub = s;
    gtk_box_append(GTK_BOX(col), t);
    gtk_box_append(GTK_BOX(col), s);

    // Uniform fixed-width trailing slot so all rows align identically
    GtkWidget *trail = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(trail, 18, 18);
    gtk_widget_set_halign(trail, GTK_ALIGN_END);
    gtk_widget_set_valign(trail, GTK_ALIGN_CENTER);
    if (has_chevron) {
        GtkWidget *chev = gtk_label_new("›");
        gtk_widget_add_css_class(chev, "spinel-cc-chev");
        gtk_widget_set_hexpand(chev, TRUE);
        gtk_widget_set_halign(chev, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(chev, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(trail), chev);
    } else if (out_dot) {
        GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(dot, "spinel-cc-dot");
        gtk_widget_set_size_request(dot, 8, 8);
        gtk_widget_set_hexpand(dot, TRUE);
        gtk_widget_set_halign(dot, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
        *out_dot = dot;
        gtk_box_append(GTK_BOX(trail), dot);
    }

    gtk_box_append(GTK_BOX(row), circle);
    gtk_box_append(GTK_BOX(row), col);
    gtk_box_append(GTK_BOX(row), trail);
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
    if (is_volume) {
        GtkWidget *mute_btn = make_cmd_button(ns->app_state, nullptr, "spinel-bar-icon-btn", "system:audio-mute-toggle");
        gtk_widget_set_tooltip_text(mute_btn, "静音 / 取消静音");
        GtkWidget *mda = gtk_drawing_area_new();
        gtk_widget_set_size_request(mda, 14, 14);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(mda),
            +[](GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer d) {
                auto *n = static_cast<NativeShellState *>(d);
                GdkRGBA fg{1.0f, 1.0f, 1.0f, 0.94f};
                gtk_widget_get_color(GTK_WIDGET(da), &fg);
                draw_monochrome_glyph(cr, w, h, (n && n->cc_vol_muted) ? BarGlyph::Mute : BarGlyph::Volume, fg);
            }, ns, nullptr);
        gtk_button_set_child(GTK_BUTTON(mute_btn), mda);
        ns->cc_vol_mute_da = mda;
        gtk_box_append(GTK_BOX(head), mute_btn);
    }
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

void draw_startup_splash_cb(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (!ns) return;

    double fade = 1.0;
    if (ns->splash_elapsed > 1.05) {
        fade = std::clamp(1.0 - (ns->splash_elapsed - 1.05) / 0.35, 0.0, 1.0);
    }
    double prog = std::clamp(ns->splash_elapsed / 1.05, 0.05, 1.0);

    cairo_save(cr);
    cairo_push_group(cr);

    double pad = 12.0;
    double cw = w - pad * 2.0;
    double ch = h - pad * 2.0;
    cairo_rounded_rect(cr, pad, pad + 4.0, cw, ch, 24.0);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.36);
    cairo_fill(cr);

    cairo_pattern_t *bg = cairo_pattern_create_linear(0, pad, 0, pad + ch);
    if (ns->theme.is_light) {
        cairo_pattern_add_color_stop_rgba(bg, 0.0, 0.97, 0.98, 1.00, 0.94);
        cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.91, 0.95, 0.99, 0.94);
    } else {
        cairo_pattern_add_color_stop_rgba(bg, 0.0, 0.09, 0.13, 0.23, 0.94);
        cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.05, 0.08, 0.15, 0.96);
    }
    cairo_rounded_rect(cr, pad, pad, cw, ch, 24.0);
    cairo_set_source(cr, bg);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(bg);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, ns->theme.is_light ? 0.72 : 0.22);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);

    double logo_sz = 92.0;
    draw_spinel_logo_at(ns, cr, (w - logo_sz) * 0.5, pad + 26.0, logo_sz);

    int tw = 0, th = 0;
    double fg_r = ns->theme.is_light ? 0.06 : 0.96;
    double fg_g = ns->theme.is_light ? 0.09 : 0.98;
    double fg_b = ns->theme.is_light ? 0.16 : 1.00;
    draw_pango_text(cr, "Spinel OS", 0, 0, 19.0, true, fg_r, fg_g, fg_b, 1.0, &tw, &th, true);
    draw_pango_text(cr, "Spinel OS", (w - tw) * 0.5, pad + 128.0, 19.0, true, fg_r, fg_g, fg_b, 1.0);

    int sw = 0, sh = 0;
    draw_pango_text(cr, "Wayland Desktop", 0, 0, 11.5, false, fg_r, fg_g, fg_b, 0.65, &sw, &sh, true);
    draw_pango_text(cr, "Wayland Desktop", (w - sw) * 0.5, pad + 156.0, 11.5, false, fg_r, fg_g, fg_b, 0.65);

    double bar_w = 176.0;
    double bar_h = 4.0;
    double bar_x = (w - bar_w) * 0.5;
    double bar_y = pad + ch - 26.0;
    cairo_rounded_rect(cr, bar_x, bar_y, bar_w, bar_h, 2.0);
    cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, 0.14);
    cairo_fill(cr);

    cairo_pattern_t *gp = cairo_pattern_create_linear(bar_x, 0, bar_x + bar_w, 0);
    cairo_pattern_add_color_stop_rgba(gp, 0.0, 0.98, 0.36, 0.49, 1.0);
    cairo_pattern_add_color_stop_rgba(gp, 0.5, 0.55, 0.24, 1.00, 1.0);
    cairo_pattern_add_color_stop_rgba(gp, 1.0, 0.22, 0.74, 0.97, 1.0);
    cairo_rounded_rect(cr, bar_x, bar_y, std::max(6.0, bar_w * prog), bar_h, 2.0);
    cairo_set_source(cr, gp);
    cairo_fill(cr);
    cairo_pattern_destroy(gp);

    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, fade);
    cairo_restore(cr);
}

gboolean on_startup_splash_tick(gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (!ns || !ns->splash_win) return G_SOURCE_REMOVE;
    ns->splash_elapsed += 0.032;
    if (ns->splash_elapsed >= 1.42) {
        close_startup_splash(ns);
        return G_SOURCE_REMOVE;
    }
    if (ns->splash_da) gtk_widget_queue_draw(ns->splash_da);
    return G_SOURCE_CONTINUE;
}

void draw_osd_cb(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (!ns) return;

    const double pad = 10.0;
    const double cw = w - pad * 2.0;
    const double ch = h - pad * 2.0;

    cairo_save(cr);
    cairo_rounded_rect(cr, pad, pad + 3.0, cw, ch, 16.0);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.28);
    cairo_fill(cr);

    cairo_pattern_t *bg = cairo_pattern_create_linear(0, pad, 0, pad + ch);
    if (ns->theme.is_light) {
        cairo_pattern_add_color_stop_rgba(bg, 0.0, 0.97, 0.98, 1.00, 0.94);
        cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.91, 0.95, 0.99, 0.94);
    } else {
        cairo_pattern_add_color_stop_rgba(bg, 0.0, 0.09, 0.13, 0.23, 0.94);
        cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.05, 0.08, 0.15, 0.96);
    }
    cairo_rounded_rect(cr, pad, pad, cw, ch, 16.0);
    cairo_set_source(cr, bg);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(bg);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, ns->theme.is_light ? 0.72 : 0.22);
    cairo_set_line_width(cr, 1.1);
    cairo_stroke(cr);

    double fg_r = ns->theme.is_light ? 0.06 : 0.96;
    double fg_g = ns->theme.is_light ? 0.09 : 0.98;
    double fg_b = ns->theme.is_light ? 0.16 : 1.00;

    BarGlyph glyph = BarGlyph::Volume;
    std::string label = "音量";
    if (ns->osd_kind == "brightness") {
        glyph = BarGlyph::Brightness;
        label = "屏幕亮度";
    } else if (ns->osd_muted || ns->osd_value <= 0) {
        glyph = BarGlyph::Mute;
        label = ns->osd_muted ? "已静音" : "音量";
    }

    cairo_save(cr);
    cairo_translate(cr, pad + 14.0, pad + (ch - 22.0) * 0.5);
    GdkRGBA icon_fg{static_cast<float>(fg_r), static_cast<float>(fg_g), static_cast<float>(fg_b), 0.92f};
    draw_monochrome_glyph(cr, 22, 22, glyph, icon_fg);
    cairo_restore(cr);

    double tx = pad + 44.0;
    double tw_area = cw - 44.0 - 16.0;
    draw_pango_text(cr, label, tx, pad + 11.0, 12.5, true, fg_r, fg_g, fg_b, 0.94);

    std::string pct_str = std::to_string(std::clamp(ns->osd_value, 0, 100)) + "%";
    int pw = 0;
    draw_pango_text(cr, pct_str, 0, 0, 12.0, true, fg_r, fg_g, fg_b, 0.85, &pw, nullptr, true);
    draw_pango_text(cr, pct_str, tx + tw_area - pw, pad + 11.5, 12.0, true, fg_r, fg_g, fg_b, 0.85);

    double bar_y = pad + ch - 16.0;
    double bar_h = 5.0;
    cairo_rounded_rect(cr, tx, bar_y, tw_area, bar_h, 2.5);
    cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, 0.15);
    cairo_fill(cr);

    double ratio = (ns->osd_muted ? 0 : std::clamp(ns->osd_value, 0, 100)) / 100.0;
    if (ratio > 0.005) {
        cairo_pattern_t *gp = cairo_pattern_create_linear(tx, 0, tx + tw_area, 0);
        if (ns->osd_kind == "brightness") {
            cairo_pattern_add_color_stop_rgba(gp, 0.0, 0.96, 0.62, 0.04, 1.0);
            cairo_pattern_add_color_stop_rgba(gp, 1.0, 0.98, 0.80, 0.08, 1.0);
        } else {
            cairo_pattern_add_color_stop_rgba(gp, 0.0, 0.22, 0.74, 0.97, 1.0);
            cairo_pattern_add_color_stop_rgba(gp, 1.0, 0.55, 0.36, 0.96, 1.0);
        }
        cairo_rounded_rect(cr, tx, bar_y, std::max(6.0, tw_area * ratio), bar_h, 2.5);
        cairo_set_source(cr, gp);
        cairo_fill(cr);
        cairo_pattern_destroy(gp);
    }
    cairo_restore(cr);
}

gboolean on_osd_hide_timeout(gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (!ns) return G_SOURCE_REMOVE;
    ns->osd_timer = 0;
    if (ns->osd_win) gtk_widget_set_visible(GTK_WIDGET(ns->osd_win), FALSE);
    return G_SOURCE_REMOVE;
}

}  // namespace

void build_native_control_center(GtkApplication *app, NativeShellState *ns) {
    AppState *state = ns->app_state;

    GtkWindow *bd = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(bd, "SpinelControlCenterBackdrop");
    gtk_window_set_decorated(bd, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(bd), "spinel-transparent");
    gtk_layer_init_for_window(bd);
    gtk_layer_set_namespace(bd, "spinel-cc-backdrop");
    gtk_layer_set_layer(bd, GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_anchor(bd, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(bd, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(bd, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(bd, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_margin(bd, GTK_LAYER_SHELL_EDGE_TOP, 30);
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
    gtk_window_set_decorated(win, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(win), "spinel-transparent");
    gtk_window_set_default_size(win, -1, -1);

    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "spinel-controlcenter");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_TOP, 28);
    gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_RIGHT, 0);
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
    gtk_widget_set_size_request(root, 356, -1);
    gtk_widget_set_margin_top(root, 8);
    gtk_widget_set_margin_bottom(root, 40);
    gtk_widget_set_margin_start(root, 34);
    gtk_widget_set_margin_end(root, 18);

    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(top_row), TRUE);

    GtkWidget *conn_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(conn_card, "spinel-cc-card");
    gtk_widget_add_css_class(conn_card, "spinel-cc-group");
    gtk_widget_set_hexpand(conn_card, TRUE);
    gtk_box_append(GTK_BOX(conn_card), make_cc_pill(state, BarGlyph::Wifi, "active-blue", "网络", "检测中", "settings:open:wifi", true, &ns->cc_net_circle, &ns->cc_net_val));
    gtk_box_append(GTK_BOX(conn_card), make_cc_pill(state, BarGlyph::Bluetooth, "active-blue", "蓝牙", "设备与手柄", "settings:open:bt", true, &ns->cc_bt_circle, &ns->cc_bt_val));
    gtk_box_append(GTK_BOX(conn_card), make_cc_pill(state, BarGlyph::Gamepad, "active-blue", "GameMode", "已开启", "perf:toggle-gamemode", false, &ns->cc_gm_circle, &ns->cc_gm_val, &ns->cc_gm_dot));
    gtk_box_append(GTK_BOX(conn_card), make_cc_pill(state, BarGlyph::Dnd, "active-indigo", "勿扰模式", "已关闭", "dnd:toggle", false, &ns->cc_dnd_circle, &ns->cc_dnd_val, &ns->cc_dnd_dot));

    GtkWidget *right_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(right_col, TRUE);

    GtkWidget *hub = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_add_css_class(hub, "spinel-cc-card");
    gtk_widget_set_vexpand(hub, TRUE);

    GtkWidget *hub_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *hub_gem = gtk_drawing_area_new();
    gtk_widget_set_size_request(hub_gem, 24, 24);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(hub_gem), draw_gem_glyph, ns, nullptr);
    ns->cc_gpu_badge = gtk_label_new("GPU");
    gtk_widget_add_css_class(ns->cc_gpu_badge, "spinel-cc-badge");
    gtk_widget_set_hexpand(ns->cc_gpu_badge, TRUE);
    gtk_widget_set_halign(ns->cc_gpu_badge, GTK_ALIGN_END);
    gtk_label_set_ellipsize(GTK_LABEL(ns->cc_gpu_badge), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(ns->cc_gpu_badge), 12);
    gtk_box_append(GTK_BOX(hub_top), hub_gem);
    gtk_box_append(GTK_BOX(hub_top), ns->cc_gpu_badge);

    GtkWidget *hub_title = gtk_label_new("Spinel OS");
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

    GtkWidget *br_card = make_cc_slider_card(ns, "显示器亮度", 15.0, 85.0, false, &ns->cc_bright_pct, &ns->cc_bright_scale);
    GtkWidget *vo_card = make_cc_slider_card(ns, "声音输出", 0.0, 75.0, true, &ns->cc_vol_pct, &ns->cc_vol_scale);

    GtkWidget *foot_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(foot_row), TRUE);
    gtk_box_append(GTK_BOX(foot_row), make_cmd_button(state, "平行沙箱", "spinel-cc-chip-btn", "sandbox:toggle-pip"));
    gtk_box_append(GTK_BOX(foot_row), make_cmd_button(state, "游戏中心", "spinel-cc-chip-btn", "games:open"));
    gtk_box_append(GTK_BOX(foot_row), make_cmd_button(state, "应用中心", "spinel-cc-chip-btn", "store:open"));
    gtk_box_append(GTK_BOX(foot_row), make_cmd_button(state, "系统设置", "spinel-cc-chip-btn", "settings:open:display"));
    gtk_box_append(GTK_BOX(foot_row), make_cmd_button(state, "锁屏", "spinel-cc-chip-btn", "system:lock"));

    gtk_box_append(GTK_BOX(root), top_row);
    gtk_box_append(GTK_BOX(root), br_card);
    gtk_box_append(GTK_BOX(root), vo_card);
    gtk_box_append(GTK_BOX(root), foot_row);

    gtk_window_set_child(win, root);
    ns->cc_win = win;
}

std::string resolve_switcher_app_name(const NativeShellState *ns, const TrackedWindowInfo &w) {
    std::string hay = to_lower_copy(w.app_id + " " + w.title);
    if (hay.find("系统监视器") != std::string::npos || hay.find("spinel monitor") != std::string::npos) return "系统监视器";
    if (hay.find("spinel doctor") != std::string::npos) return "Spinel Doctor";
    if (hay.find("系统设置") != std::string::npos || hay.find("settings") != std::string::npos) return "系统设置";
    if (hay.find("游戏中心") != std::string::npos || hay.find("游戏与启动中心") != std::string::npos ||
        hay.find("游戏与应用中心") != std::string::npos || hay.find("app-games") != std::string::npos) return "游戏中心";
    if (hay.find("应用中心") != std::string::npos || hay.find("应用商店") != std::string::npos ||
        hay.find("app-store") != std::string::npos) return "应用中心";
    if (hay.find("安装") != std::string::npos || hay.find("installer") != std::string::npos) return "系统安装";
    if (hay.find("spinel-files") != std::string::npos || hay.find("org.spinelos.files") != std::string::npos ||
        hay.find("文件管理") != std::string::npos) return "Spinel Files";
    if (hay.find("spinel-terminal") != std::string::npos || hay.find("org.spinelos.terminal") != std::string::npos ||
        hay.find("终端") != std::string::npos) return "Spinel 终端";
    if (hay.find("libreoffice") != std::string::npos || hay.find("soffice") != std::string::npos) return "LibreOffice";
    if (hay.find("xournal") != std::string::npos) return "Xournal++";
    if (hay.find("papers") != std::string::npos) return "Papers PDF";
    if (hay.find("obsidian") != std::string::npos) return "Obsidian";
    if (hay.find("thunderbird") != std::string::npos) return "Thunderbird";
    if (hay.find("system-config-printer") != std::string::npos) return "打印机设置";
    if (hay == "code" || hay.find("vscode") != std::string::npos || hay.find("visual studio code") != std::string::npos) return "VS Code";
    if (hay.find("chromium") != std::string::npos) return "Chromium";
    if (hay.find("chrome") != std::string::npos) return "Google Chrome";
    if (hay.find("firefox") != std::string::npos) return "Firefox";
    if (hay.find("steam") != std::string::npos) return hay.find("gamescope") != std::string::npos ? "主机大屏" : "Steam";
    if (hay.find("antigravity") != std::string::npos) return "Antigravity";
    if (hay.find("monocloud") != std::string::npos) return "MonoCloud";

    if (ns) {
        std::string laid = to_lower_copy(trim_copy(w.app_id));
        std::string ltitle = to_lower_copy(trim_copy(w.title));
        for (const auto &a : ns->desktop_apps) {
            std::string aid = to_lower_copy(a.id);
            std::string aname = to_lower_copy(a.name);
            if ((!laid.empty() && (aid == laid || (!aid.empty() && laid.find(aid) != std::string::npos))) ||
                (!ltitle.empty() && aname.size() >= 3 && ltitle.find(aname) != std::string::npos)) {
                return a.name;
            }
        }
    }
    return !w.app_id.empty() ? w.app_id : (!w.title.empty() ? w.title : "应用窗口");
}

void update_switcher_selection_ui(NativeShellState *ns) {
    if (!ns) return;
    int n = static_cast<int>(ns->switcher_cards.size());
    for (int i = 0; i < n; ++i) {
        if (!ns->switcher_cards[i]) continue;
        if (i == ns->switcher_index) gtk_widget_add_css_class(ns->switcher_cards[i], "is-selected");
        else gtk_widget_remove_css_class(ns->switcher_cards[i], "is-selected");
    }
    if (ns->switcher_hint_lbl && ns->switcher_index >= 0 &&
        ns->switcher_index < static_cast<int>(ns->switcher_items.size())) {
        const auto &cur = ns->switcher_items[static_cast<size_t>(ns->switcher_index)];
        std::string txt = cur.app_name;
        if (!cur.title.empty() && cur.title != cur.app_name) {
            txt += " — " + cur.title;
        }
        if (cur.minimized) txt += " (已最小化)";
        else if (cur.activated) txt += " (当前)";
        else if (cur.maximized) txt += " (已最大化)";
        txt += "  ·  Alt+Tab 切换 / Enter 确认 / Del 关闭窗口 / Esc 取消";
        gtk_label_set_text(GTK_LABEL(ns->switcher_hint_lbl), txt.c_str());
    }
}

void rebuild_switcher_cards(NativeShellState *ns) {
    if (!ns || !ns->switcher_flow) return;
    while (GtkWidget *c = gtk_widget_get_first_child(ns->switcher_flow)) {
        gtk_flow_box_remove(GTK_FLOW_BOX(ns->switcher_flow), c);
    }
    ns->switcher_cards.clear();

    int total = static_cast<int>(ns->switcher_items.size());
    int per_row = std::clamp(total, 1, 7);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(ns->switcher_flow), static_cast<guint>(per_row));
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(ns->switcher_flow), 7);

    for (int i = 0; i < total; ++i) {
        const auto &it = ns->switcher_items[static_cast<size_t>(i)];
        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_add_css_class(card, "spinel-switcher-card");
        gtk_widget_set_size_request(card, 114, 108);
        gtk_widget_set_halign(card, GTK_ALIGN_CENTER);

        GtkWidget *da = gtk_drawing_area_new();
        gtk_widget_set_size_request(da, 58, 58);
        gtk_widget_set_halign(da, GTK_ALIGN_CENTER);
        auto *ctx = new DesktopIconDrawCtx{ns, it.icon_path, it.kind};
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_desktop_icon_cb, ctx, free_desktop_icon_ctx);

        GtkWidget *app_lbl = gtk_label_new(it.app_name.c_str());
        gtk_widget_add_css_class(app_lbl, "spinel-switcher-app");
        gtk_widget_set_halign(app_lbl, GTK_ALIGN_CENTER);
        gtk_label_set_ellipsize(GTK_LABEL(app_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(app_lbl), 13);

        std::string sub = (!it.title.empty() && it.title != it.app_name)
            ? it.title
            : (it.minimized ? "已最小化" : (it.activated ? "当前窗口" : (it.maximized ? "已最大化" : "运行中")));
        GtkWidget *sub_lbl = gtk_label_new(sub.c_str());
        gtk_widget_add_css_class(sub_lbl, "spinel-switcher-title");
        gtk_widget_set_halign(sub_lbl, GTK_ALIGN_CENTER);
        gtk_label_set_ellipsize(GTK_LABEL(sub_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(sub_lbl), 14);

        gtk_box_append(GTK_BOX(card), da);
        gtk_box_append(GTK_BOX(card), app_lbl);
        gtk_box_append(GTK_BOX(card), sub_lbl);

        GtkGesture *tap = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(tap), GDK_BUTTON_PRIMARY);
        g_object_set_data(G_OBJECT(card), "sw-idx", GINT_TO_POINTER(i));
        g_signal_connect(tap, "pressed", G_CALLBACK(+[](GtkGestureClick *g, int, double, double, gpointer d) {
            auto *n = static_cast<NativeShellState *>(d);
            GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
            int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "sw-idx"));
            n->switcher_index = idx;
            close_switcher(n, true);
        }), ns);
        gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(tap));

        ns->switcher_cards.push_back(card);
        gtk_flow_box_append(GTK_FLOW_BOX(ns->switcher_flow), card);
    }
}

void build_native_switcher(GtkApplication *app, NativeShellState *ns) {
    GtkWindow *backdrop = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(backdrop, "SpinelSwitcherBackdrop");
    gtk_window_set_decorated(backdrop, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(backdrop), "spinel-transparent");
    gtk_layer_init_for_window(backdrop);
    gtk_layer_set_namespace(backdrop, "spinel-switcher-backdrop");
    gtk_layer_set_layer(backdrop, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(backdrop, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(backdrop, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(backdrop, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(backdrop, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_exclusive_zone(backdrop, -1);
    gtk_layer_set_keyboard_mode(backdrop, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    GtkWidget *bg = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(bg, TRUE);
    gtk_widget_set_vexpand(bg, TRUE);
    GtkGesture *bclick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(bclick), 0);
    g_signal_connect(bclick, "pressed", G_CALLBACK(+[](GtkGestureClick *, int, double, double, gpointer d) {
        close_switcher(static_cast<NativeShellState *>(d), false);
    }), ns);
    gtk_widget_add_controller(bg, GTK_EVENT_CONTROLLER(bclick));
    gtk_window_set_child(backdrop, bg);
    gtk_widget_set_visible(GTK_WIDGET(backdrop), FALSE);
    ns->switcher_backdrop_win = backdrop;

    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(win, "SpinelSwitcher");
    gtk_window_set_decorated(win, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(win), "spinel-transparent");
    gtk_window_set_default_size(win, -1, -1);
    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "spinel-switcher");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_widget_set_visible(GTK_WIDGET(win), FALSE);

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(panel, "spinel-switcher-panel");
    gtk_widget_set_margin_top(panel, 28);
    gtk_widget_set_margin_bottom(panel, 48);
    gtk_widget_set_margin_start(panel, 36);
    gtk_widget_set_margin_end(panel, 36);

    GtkWidget *flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 10);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 10);
    gtk_widget_set_halign(flow, GTK_ALIGN_CENTER);

    GtkWidget *hint = gtk_label_new("Alt+Tab 切换 / Enter 确认 / Esc 取消");
    gtk_widget_add_css_class(hint, "spinel-switcher-hint");
    gtk_widget_set_halign(hint, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(hint), 68);

    gtk_box_append(GTK_BOX(panel), flow);
    gtk_box_append(GTK_BOX(panel), hint);
    gtk_window_set_child(win, panel);

    ns->switcher_win = win;
    ns->switcher_flow = flow;
    ns->switcher_hint_lbl = hint;

    GtkEventController *key_ctl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctl, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer d) -> gboolean {
        auto *n = static_cast<NativeShellState *>(d);
        int total = static_cast<int>(n->switcher_items.size());
        if (keyval == GDK_KEY_Escape) {
            close_switcher(n, false);
            return TRUE;
        }
        if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter || keyval == GDK_KEY_space) {
            close_switcher(n, true);
            return TRUE;
        }
        if (total > 0 && (keyval == GDK_KEY_Right || keyval == GDK_KEY_Down || keyval == GDK_KEY_Tab)) {
            n->switcher_index = (n->switcher_index + 1) % total;
            update_switcher_selection_ui(n);
            return TRUE;
        }
        if (total > 0 && (keyval == GDK_KEY_Left || keyval == GDK_KEY_Up || keyval == GDK_KEY_ISO_Left_Tab)) {
            n->switcher_index = (n->switcher_index + total - 1) % total;
            update_switcher_selection_ui(n);
            return TRUE;
        }
        if (total > 0 && (keyval == GDK_KEY_Delete || keyval == GDK_KEY_BackSpace) &&
            n->switcher_index >= 0 && n->switcher_index < total) {
            uint64_t close_id = n->switcher_items[static_cast<size_t>(n->switcher_index)].win_id;
            if (close_id > 0) close_tracked_window(close_id);
            n->switcher_items.erase(n->switcher_items.begin() + n->switcher_index);
            if (n->switcher_items.empty()) {
                close_switcher(n, false);
            } else {
                if (n->switcher_index >= static_cast<int>(n->switcher_items.size())) {
                    n->switcher_index = static_cast<int>(n->switcher_items.size()) - 1;
                }
                rebuild_switcher_cards(n);
                update_switcher_selection_ui(n);
            }
            return TRUE;
        }
        return FALSE;
    }), ns);
    g_signal_connect(key_ctl, "key-released", G_CALLBACK(+[](GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer d) {
        auto *n = static_cast<NativeShellState *>(d);
        if (!n || !n->switcher_win || !gtk_widget_get_visible(GTK_WIDGET(n->switcher_win))) return;
        if (keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R ||
            keyval == GDK_KEY_Meta_L || keyval == GDK_KEY_Meta_R) {
            close_switcher(n, true);
        }
    }), ns);
    gtk_widget_add_controller(GTK_WIDGET(win), key_ctl);

    g_signal_connect(win, "notify::is-active", G_CALLBACK(+[](GObject *obj, GParamSpec *, gpointer d) {
        auto *n = static_cast<NativeShellState *>(d);
        if (!n || !gtk_widget_get_visible(GTK_WIDGET(obj))) return;
        bool active = gtk_window_is_active(GTK_WINDOW(obj));
        if (active) n->switcher_was_active = true;
        else if (n->switcher_was_active) close_switcher(n, false);
    }), ns);
}

void close_startup_splash(NativeShellState *ns) {
    if (!ns) return;
    if (ns->splash_timer) {
        g_source_remove(ns->splash_timer);
        ns->splash_timer = 0;
    }
    if (ns->splash_win) {
        GtkWindow *w = ns->splash_win;
        ns->splash_win = nullptr;
        ns->splash_da = nullptr;
        gtk_window_destroy(w);
    }
}

void show_desktop_startup_splash(GtkApplication *app, NativeShellState *ns) {
    if (!app || !ns) return;
    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(win, "SpinelSplash");
    gtk_widget_add_css_class(GTK_WIDGET(win), "spinel-transparent");
    gtk_window_set_default_size(win, 336, 240);

    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "spinel-splash");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    ns->splash_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(ns->splash_da, 336, 240);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ns->splash_da), draw_startup_splash_cb, ns, nullptr);

    GtkGesture *tap = gtk_gesture_click_new();
    g_signal_connect(tap, "pressed", G_CALLBACK(+[](GtkGestureClick *, int, double, double, gpointer d) {
        close_startup_splash(static_cast<NativeShellState *>(d));
    }), ns);
    gtk_widget_add_controller(ns->splash_da, GTK_EVENT_CONTROLLER(tap));

    gtk_window_set_child(win, ns->splash_da);
    ns->splash_win = win;
    ns->splash_elapsed = 0.0;
    ns->splash_timer = g_timeout_add(32, on_startup_splash_tick, ns);
    gtk_window_present(win);
}

void build_osd_window(GtkApplication *app, NativeShellState *ns) {
    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(win, "SpinelOSD");
    gtk_widget_add_css_class(GTK_WIDGET(win), "spinel-transparent");
    gtk_window_set_default_size(win, 272, 74);
    gtk_widget_set_can_target(GTK_WIDGET(win), FALSE);

    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "spinel-osd");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_BOTTOM, 108);
    gtk_layer_set_exclusive_zone(win, -1);
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, 272, 74);
    gtk_widget_set_can_target(da, FALSE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_osd_cb, ns, nullptr);
    gtk_window_set_child(win, da);

    ns->osd_win = win;
    ns->osd_da = da;
}

void sync_cc_media_sliders(NativeShellState *ns) {
    if (!ns) return;
    MediaSnapshot media = collect_media_state();
    ns->cc_vol_muted = media.volume_muted;
    if (ns->cc_vol_mute_da) gtk_widget_queue_draw(ns->cc_vol_mute_da);
    ns->cc_updating_sliders = true;
    if (ns->cc_vol_scale && ns->cc_vol_pct) {
        gtk_range_set_value(GTK_RANGE(ns->cc_vol_scale), media.volume);
        std::string s = media.volume_muted ? "静音" : (std::to_string(media.volume) + "%");
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
}

void close_switcher(NativeShellState *ns, bool activate_selected) {
    if (!ns || !ns->switcher_win) return;
    uint64_t target_id = 0;
    if (activate_selected && ns->switcher_index >= 0 &&
        ns->switcher_index < static_cast<int>(ns->switcher_items.size())) {
        target_id = ns->switcher_items[static_cast<size_t>(ns->switcher_index)].win_id;
    }
    ns->switcher_was_active = false;
    gtk_layer_set_keyboard_mode(ns->switcher_win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_widget_set_visible(GTK_WIDGET(ns->switcher_win), FALSE);
    if (ns->switcher_backdrop_win) {
        gtk_widget_set_visible(GTK_WIDGET(ns->switcher_backdrop_win), FALSE);
    }
    if (target_id > 0) {
        activate_tracked_window(target_id);
    }
}

}  // namespace shell_internal

/* ==================== Public Overlay Visibility & OSD Entrypoints ==================== */

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
        shell_internal::sync_cc_media_sliders(ns);
        gtk_window_present(ns->cc_win);
    }
}

void native_shell_toggle_cc(AppState *state) {
    if (!state || !state->native || !state->native->cc_win) return;
    bool vis = gtk_widget_get_visible(GTK_WIDGET(state->native->cc_win));
    native_shell_set_cc_visible(state, !vis);
}

void native_shell_step_switcher(AppState *state, bool prev) {
    if (!state || !state->native || !state->native->switcher_win) return;
    NativeShellState *ns = state->native;
    if (gtk_widget_get_visible(GTK_WIDGET(ns->switcher_win))) {
        int total = static_cast<int>(ns->switcher_items.size());
        if (total > 0) {
            ns->switcher_index = (ns->switcher_index + (prev ? (total - 1) : 1)) % total;
            shell_internal::update_switcher_selection_ui(ns);
        }
        return;
    }

    auto wins = list_tracked_windows_mru();
    if (wins.empty()) return;

    shell_internal::set_launcher_visible(ns, false);
    native_shell_set_cc_visible(state, false);
    ns->desktop_apps = list_desktop_apps();
    ns->switcher_items.clear();
    for (const auto &w : wins) {
        NativeShellState::SwitcherItem item;
        item.win_id = w.id;
        item.app_name = shell_internal::resolve_switcher_app_name(ns, w);
        item.title = w.title;
        item.icon_path = shell_internal::find_app_icon_path(ns->desktop_apps, {w.app_id, item.app_name, w.title});
        item.kind = shell_internal::match_app_kind(w.app_id + " " + w.title);
        if (item.kind == "files" &&
            (shell_internal::contains_ci(item.icon_path, "system-file-manager") ||
             shell_internal::contains_ci(item.icon_path, "thunar"))) {
            item.icon_path.clear();
        }
        item.minimized = w.minimized;
        item.activated = w.activated;
        item.maximized = w.maximized;
        ns->switcher_items.push_back(std::move(item));
    }

    int total = static_cast<int>(ns->switcher_items.size());
    ns->switcher_index = prev ? (total - 1) : (total > 1 ? 1 : 0);
    ns->switcher_was_active = false;
    shell_internal::rebuild_switcher_cards(ns);
    shell_internal::update_switcher_selection_ui(ns);

    if (ns->switcher_backdrop_win) {
        gtk_widget_set_visible(GTK_WIDGET(ns->switcher_backdrop_win), TRUE);
        gtk_window_present(ns->switcher_backdrop_win);
    }
    gtk_layer_set_keyboard_mode(ns->switcher_win, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    gtk_widget_set_visible(GTK_WIDGET(ns->switcher_win), TRUE);
    gtk_window_present(ns->switcher_win);
    gtk_widget_grab_focus(GTK_WIDGET(ns->switcher_win));
}

void native_shell_show_osd(AppState *state, const std::string &kind, int value, bool muted) {
    if (!state || !state->native || !state->native->osd_win) return;
    NativeShellState *ns = state->native;
    ns->osd_kind = (kind == "brightness") ? "brightness" : "volume";
    ns->osd_value = std::clamp(value, 0, 100);
    ns->osd_muted = muted;
    if (ns->cc_win && gtk_widget_get_visible(GTK_WIDGET(ns->cc_win))) {
        shell_internal::sync_cc_media_sliders(ns);
    }
    if (ns->osd_timer) {
        g_source_remove(ns->osd_timer);
        ns->osd_timer = 0;
    }
    gtk_window_present(ns->osd_win);
    if (ns->osd_da) gtk_widget_queue_draw(ns->osd_da);
    ns->osd_timer = g_timeout_add(1500, shell_internal::on_osd_hide_timeout, ns);
}
