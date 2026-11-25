#include "shell_internal.hpp"
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

void rebuild_launcher_index(NativeShellState *ns) {
    if (!ns) return;
    ns->launcher_entries.clear();
    ns->desktop_apps = list_desktop_apps();
    bool steam_installed = has_executable("steam");

    ns->launcher_entries = {
        {"Spinel 终端", "原生多标签 Wayland 终端 (Super+Enter)", "terminal ptyxis shell console bash", "launch:terminal", "", "terminal"},
        {"应用商店", "一键安装 Steam / 浏览器 / Flatpak / 系统更新", "store app market pacman flatpak", "store:open", "", "store"},
        {"游戏与启动中心", "浏览 Steam 游戏库、启动大屏模式与调节游戏性能", "games library launcher center", "games:open", "", "games"},
        {"文件管理 (Thunar)", "浏览本地文件、压缩包与挂载盘", "files thunar folder manager", "launch:files", "", "files"},
        {steam_installed ? "Steam 客户端" : "安装 Steam 客户端",
         steam_installed ? "启动 Valve Steam 游戏平台" : "前往应用商店一键安装 Steam",
         "steam valve game", steam_installed ? "launch:steam" : "store:open:steam",
         find_app_icon_path(ns->desktop_apps, {"steam"}), "steam"},
        {"主机大屏模式 (Gamescope)", "全屏主机手柄界面与游戏性能调优", "gamepad gamescope bigpicture console", "launch:steam-gamepadui", "", "gamepad"},
        {"控制中心", "快速调整音量、亮度、GameMode 与 MangoHud", "control center audio volume brightness", "cc:toggle", "", "settings"},
        {"区域截图", "框选屏幕区域并复制到剪贴板", "screenshot area capture snip", "system:screenshot:area", "", "settings"},
        {"全屏截图", "保存全屏截图到 ~/Pictures", "screenshot full screen capture", "system:screenshot:full", "", "settings"},
        {"锁定屏幕", "锁定当前 Wayland 桌面会话 (Super+L)", "lock screen swaylock", "system:lock", "", "settings"},
        {"首次开机向导 (OOBE)", "主题外观、网络代理、Steam 安装与快捷键引导", "welcome tour oobe guide setup", "welcome:open", "", "settings"},
        {"系统设置 · Wi-Fi 与代理", "无线网络与 HTTP/SOCKS 系统代理", "settings wifi network proxy", "settings:open:wifi", "", "settings"},
        {"系统设置 · 蓝牙与设备", "手柄、耳机与蓝牙外设配对", "settings bluetooth bt gamepad", "settings:open:bt", "", "settings"},
        {"系统设置 · 显示与缩放", "屏幕分辨率、刷新率与 HiDPI 缩放", "settings display resolution scale monitor", "settings:open:display", "", "settings"},
        {"系统设置 · 设备管理器", "U 盘挂载安全弹出、USB 外设、手柄与硬件驱动", "settings devices usb disk mount unmount eject pci hardware", "settings:open:devices", "", "settings"},
        {"Spinel Doctor · 系统健康体检", "全链路检测显卡、网络、代理、输入法、声音与游戏运行库", "doctor health check diagnostic scan", "doctor:open", "", "settings"},
        {"系统设置 · 声音与电源", "输出设备、麦克风与功耗模式", "settings audio sound power battery", "settings:open:audio", "", "settings"},
        {"系统设置 · 键盘与快捷键", "自定义截图、锁屏、终端等全局热键", "settings shortcuts keyboard hotkeys", "settings:open:shortcuts", "", "settings"},
        {"系统设置 · 游戏性能预设", "MangoHud / GameMode / 帧率上限 / FSR", "settings perf mangohud gamemode fsr", "settings:open:perf", "", "settings"},
        {"系统设置 · 主题壁纸与输入法", "切换浅深色主题、自定义壁纸与 Fcitx5 拼音", "settings theme wallpaper appearance ime fcitx5 rime", "settings:open:appearance", "", "settings"},
        {"系统设置 · 权限与安全", "Flatpak 应用权限与 Portal 弹窗管理", "settings permissions security flatpak polkit", "settings:open:perms", "", "settings"},
        {"系统设置 · 进程与内存监控", "查看系统 PSS 内存与实时 CPU 占用", "settings process monitor memory cpu", "settings:open:procs", "", "settings"},
        {"系统设置 · 日志与诊断", "导出与排查桌面及启动日志", "settings logs debug diagnostic", "settings:open:logs", "", "settings"}
    };

    if (ns->telemetry.is_live) {
        ns->launcher_entries.push_back({
            "安装 SpinelOS 到硬盘", "启动系统图形安装向导", "installer disk setup live", "installer:open", "", "installer"
        });
    }

    for (const auto &app : ns->desktop_apps) {
        std::string lid = to_lower_copy(app.id);
        if (lid == "thunar" || lid == "steam" || lid == "spinel-terminal" || lid == "org.spinelos.terminal") continue;
        ns->launcher_entries.push_back({
            app.name,
            !app.category.empty() ? ("已安装应用 · " + app.category) : "已安装桌面应用",
            app.id + " " + app.category,
            "app:launch:" + app.id,
            app.icon_path,
            match_app_kind(app.id + " " + app.name)
        });
    }

    for (const auto &g : list_steam_games()) {
        ns->launcher_entries.push_back({
            g.name,
            "Steam 本地游戏",
            "steam game " + g.appid,
            "steam:launch:" + g.appid,
            !g.cover_path.empty() ? g.cover_path : find_app_icon_path(ns->desktop_apps, {"steam"}),
            "steam"
        });
    }
}

void populate_launcher_results(NativeShellState *ns, const std::string &query) {
    if (!ns || !ns->launcher_list) return;
    while (GtkWidget *c = gtk_widget_get_first_child(ns->launcher_list)) {
        gtk_list_box_remove(GTK_LIST_BOX(ns->launcher_list), c);
    }

    std::string q = to_lower_copy(trim_copy(query));
    struct Scored {
        int score = 0;
        const NativeShellState::LauncherEntry *entry = nullptr;
    };
    std::vector<Scored> matches;

    for (const auto &e : ns->launcher_entries) {
        if (q.empty()) {
            matches.push_back({100, &e});
            continue;
        }
        std::string lt = to_lower_copy(e.title);
        std::string ls = to_lower_copy(e.subtitle);
        std::string lk = to_lower_copy(e.keywords);
        int sc = 0;
        if (lt.rfind(q, 0) == 0) sc = 300;
        else if (lt.find(q) != std::string::npos) sc = 220;
        else if (lk.find(q) != std::string::npos) sc = 160;
        else if (ls.find(q) != std::string::npos) sc = 120;
        if (sc > 0) matches.push_back({sc, &e});
    }

    std::stable_sort(matches.begin(), matches.end(), [](const Scored &a, const Scored &b) {
        return a.score > b.score;
    });
    if (matches.size() > 10) matches.resize(10);

    for (const auto &m : matches) {
        const auto *e = m.entry;
        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *da = gtk_drawing_area_new();
        gtk_widget_set_size_request(da, 28, 28);
        auto *ctx = new DesktopIconDrawCtx{ns, e->icon_path, e->kind};
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_desktop_icon_cb, ctx, free_desktop_icon_ctx);

        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        gtk_widget_set_hexpand(col, TRUE);
        GtkWidget *t = gtk_label_new(e->title.c_str());
        gtk_widget_add_css_class(t, "spinel-cc-title");
        gtk_widget_set_halign(t, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
        GtkWidget *s = gtk_label_new(e->subtitle.c_str());
        gtk_widget_add_css_class(s, "spinel-cc-sub");
        gtk_widget_set_halign(s, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(s), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(col), t);
        gtk_box_append(GTK_BOX(col), s);

        GtkWidget *enter_lbl = gtk_label_new("↵");
        gtk_widget_add_css_class(enter_lbl, "spinel-cc-badge");

        gtk_box_append(GTK_BOX(row_box), da);
        gtk_box_append(GTK_BOX(row_box), col);
        gtk_box_append(GTK_BOX(row_box), enter_lbl);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_box);
        g_object_set_data_full(G_OBJECT(row), "launcher-cmd", g_strdup(e->cmd.c_str()), g_free);
        gtk_list_box_append(GTK_LIST_BOX(ns->launcher_list), row);
    }

    if (GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(ns->launcher_list), 0)) {
        gtk_list_box_select_row(GTK_LIST_BOX(ns->launcher_list), first);
    }
}

void set_launcher_visible(NativeShellState *ns, bool visible) {
    if (!ns || !ns->launcher_win) return;
    ns->launcher_was_active = false;
    if (ns->launcher_backdrop_win) {
        gtk_widget_set_visible(GTK_WIDGET(ns->launcher_backdrop_win), visible ? TRUE : FALSE);
        if (visible) gtk_window_present(ns->launcher_backdrop_win);
    }
    if (visible) {
        rebuild_launcher_index(ns);
        gtk_editable_set_text(GTK_EDITABLE(ns->launcher_entry), "");
        populate_launcher_results(ns, "");
        gtk_layer_set_keyboard_mode(ns->launcher_win, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
        gtk_widget_set_visible(GTK_WIDGET(ns->launcher_win), TRUE);
        gtk_window_present(ns->launcher_win);
        gtk_widget_grab_focus(ns->launcher_entry);
    } else {
        gtk_layer_set_keyboard_mode(ns->launcher_win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_widget_set_visible(GTK_WIDGET(ns->launcher_win), FALSE);
    }
}

void activate_selected_launcher_row(NativeShellState *ns, GtkListBoxRow *row = nullptr) {
    if (!ns || !ns->launcher_list) return;
    if (!row) row = gtk_list_box_get_selected_row(GTK_LIST_BOX(ns->launcher_list));
    if (!row) return;
    const char *cmd = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "launcher-cmd"));
    std::string cmd_copy = cmd ? cmd : "";
    set_launcher_visible(ns, false);
    if (!cmd_copy.empty()) trigger_cmd(ns->app_state, cmd_copy);
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
    draw_pango_text(cr, "SpinelOS", 0, 0, 19.0, true, fg_r, fg_g, fg_b, 1.0, &tw, &th, true);
    draw_pango_text(cr, "SpinelOS", (w - tw) * 0.5, pad + 128.0, 19.0, true, fg_r, fg_g, fg_b, 1.0);

    int sw = 0, sh = 0;
    draw_pango_text(cr, "Gaming Linux · Wayland", 0, 0, 11.5, false, fg_r, fg_g, fg_b, 0.65, &sw, &sh, true);
    draw_pango_text(cr, "Gaming Linux · Wayland", (w - sw) * 0.5, pad + 156.0, 11.5, false, fg_r, fg_g, fg_b, 0.65);

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

    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(top_row), TRUE);

    GtkWidget *conn_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(conn_card, "spinel-cc-card");
    gtk_widget_add_css_class(conn_card, "spinel-cc-group");
    gtk_widget_set_hexpand(conn_card, TRUE);
    gtk_box_append(GTK_BOX(conn_card), make_cc_pill(state, BarGlyph::Wifi, "active-blue", "网络", "检测中", "settings:open:wifi", true, nullptr, &ns->cc_net_val));
    gtk_box_append(GTK_BOX(conn_card), make_cc_pill(state, BarGlyph::Bluetooth, "active-blue", "蓝牙", "设备与手柄", "settings:open:bt", true, nullptr, nullptr));
    gtk_box_append(GTK_BOX(conn_card), make_cc_pill(state, BarGlyph::Gamepad, "active-blue", "GameMode", "已开启", "perf:toggle-gamemode", false, &ns->cc_gm_circle, &ns->cc_gm_val));
    gtk_box_append(GTK_BOX(conn_card), make_cc_pill(state, BarGlyph::Dnd, "active-indigo", "勿扰模式", "已关闭", "dnd:toggle", false, &ns->cc_dnd_circle, &ns->cc_dnd_val));

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

    GtkWidget *br_card = make_cc_slider_card(ns, "显示器亮度", 15.0, 85.0, false, &ns->cc_bright_pct, &ns->cc_bright_scale);
    GtkWidget *vo_card = make_cc_slider_card(ns, "声音输出", 0.0, 75.0, true, &ns->cc_vol_pct, &ns->cc_vol_scale);

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
        ".spinel-shortcut-btn.selected {\n"
        "  background-color: " + std::string(sc_hover) + ";\n"
        "  border-color: " + t.cc_accent + ";\n"
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
        ".spinel-cc-scale slider { min-width: 15px; min-height: 15px; border-radius: 999px; background-color: #ffffff; box-shadow: 0 2px 5px rgba(0, 0, 0, 0.30); }\n"
        ".spinel-launcher-panel {\n"
        "  background-color: " + t.popover_bg + ";\n"
        "  border: 1px solid " + t.cc_border + ";\n"
        "  border-radius: 18px;\n"
        "  padding: 12px;\n"
        "  box-shadow: 0 22px 50px rgba(0, 0, 0, 0.45);\n"
        "  color: " + t.bar_fg + ";\n"
        "}\n"
        ".spinel-launcher-search {\n"
        "  background-color: " + std::string(cc_chip_bg) + ";\n"
        "  border: 1px solid " + t.bar_border + ";\n"
        "  border-radius: 12px;\n"
        "  padding: 6px 12px;\n"
        "}\n"
        ".spinel-launcher-entry {\n"
        "  background: transparent;\n"
        "  border: none;\n"
        "  box-shadow: none;\n"
        "  color: " + t.bar_fg + ";\n"
        "  font-size: 15px;\n"
        "  font-weight: 500;\n"
        "}\n"
        ".spinel-launcher-list {\n"
        "  background: transparent;\n"
        "}\n"
        ".spinel-launcher-list row {\n"
        "  background: transparent;\n"
        "  border-radius: 10px;\n"
        "  padding: 6px 10px;\n"
        "  margin: 2px 0;\n"
        "}\n"
        ".spinel-launcher-list row:hover { background-color: " + std::string(cc_chip_bg) + "; }\n"
        ".spinel-launcher-list row:selected {\n"
        "  background-color: " + t.cc_accent + ";\n"
        "  color: #ffffff;\n"
        "}\n"
        ".spinel-launcher-list row:selected .spinel-cc-title,\n"
        ".spinel-launcher-list row:selected .spinel-cc-sub,\n"
        ".spinel-launcher-list row:selected .spinel-cc-badge {\n"
        "  color: #ffffff;\n"
        "}\n";

    GdkDisplay *display = gdk_display_get_default();
    if (!display) return;
    if (!ns->css_provider) {
        ns->css_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            display, GTK_STYLE_PROVIDER(ns->css_provider), GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    gtk_css_provider_load_from_string(ns->css_provider, css_str.c_str());
}

void build_native_launcher(GtkApplication *app, NativeShellState *ns) {
    GtkWindow *backdrop = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(backdrop, "SpinelLauncherBackdrop");
    gtk_widget_add_css_class(GTK_WIDGET(backdrop), "spinel-transparent");
    gtk_layer_init_for_window(backdrop);
    gtk_layer_set_namespace(backdrop, "spinel-launcher-backdrop");
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
        set_launcher_visible(static_cast<NativeShellState *>(d), false);
    }), ns);
    gtk_widget_add_controller(bg, GTK_EVENT_CONTROLLER(bclick));
    gtk_window_set_child(backdrop, bg);
    gtk_widget_set_visible(GTK_WIDGET(backdrop), FALSE);
    ns->launcher_backdrop_win = backdrop;

    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(win, "SpinelLauncher");
    gtk_widget_add_css_class(GTK_WIDGET(win), "spinel-transparent");
    gtk_window_set_default_size(win, 580, -1);
    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "spinel-launcher");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_TOP, 130);
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_widget_set_visible(GTK_WIDGET(win), FALSE);

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(panel, "spinel-launcher-panel");
    gtk_widget_set_size_request(panel, 580, -1);
    gtk_widget_set_margin_top(panel, 6);
    gtk_widget_set_margin_bottom(panel, 10);
    gtk_widget_set_margin_start(panel, 10);
    gtk_widget_set_margin_end(panel, 10);

    GtkWidget *search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(search_row, "spinel-launcher-search");
    GtkWidget *ico = gtk_drawing_area_new();
    gtk_widget_set_size_request(ico, 18, 18);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ico), draw_glyph_cb,
                                   reinterpret_cast<gpointer>(static_cast<intptr_t>(BarGlyph::Search)), nullptr);

    GtkWidget *entry = gtk_entry_new();
    gtk_widget_add_css_class(entry, "spinel-launcher-entry");
    gtk_entry_set_has_frame(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "搜索应用、Steam 游戏、系统设置或命令...");
    gtk_widget_set_hexpand(entry, TRUE);

    GtkWidget *esc_hint = gtk_label_new("Esc 关闭");
    gtk_widget_add_css_class(esc_hint, "spinel-cc-badge");

    gtk_box_append(GTK_BOX(search_row), ico);
    gtk_box_append(GTK_BOX(search_row), entry);
    gtk_box_append(GTK_BOX(search_row), esc_hint);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 360);

    GtkWidget *list = gtk_list_box_new();
    gtk_widget_add_css_class(list, "spinel-launcher-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);

    gtk_box_append(GTK_BOX(panel), search_row);
    gtk_box_append(GTK_BOX(panel), scroll);
    gtk_window_set_child(win, panel);

    ns->launcher_win = win;
    ns->launcher_entry = entry;
    ns->launcher_list = list;

    g_signal_connect(entry, "changed", G_CALLBACK(+[](GtkEditable *ed, gpointer d) {
        auto *n = static_cast<NativeShellState *>(d);
        populate_launcher_results(n, gtk_editable_get_text(ed));
    }), ns);
    g_signal_connect(entry, "activate", G_CALLBACK(+[](GtkEntry *, gpointer d) {
        activate_selected_launcher_row(static_cast<NativeShellState *>(d));
    }), ns);
    g_signal_connect(list, "row-activated", G_CALLBACK(+[](GtkListBox *, GtkListBoxRow *row, gpointer d) {
        activate_selected_launcher_row(static_cast<NativeShellState *>(d), row);
    }), ns);

    GtkEventController *key_ctl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctl, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer d) -> gboolean {
        auto *n = static_cast<NativeShellState *>(d);
        if (keyval == GDK_KEY_Escape) {
            set_launcher_visible(n, false);
            return TRUE;
        }
        if (keyval == GDK_KEY_Down || keyval == GDK_KEY_Up) {
            GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(n->launcher_list));
            int cur = sel ? gtk_list_box_row_get_index(sel) : 0;
            int next = (keyval == GDK_KEY_Down) ? (cur + 1) : std::max(0, cur - 1);
            if (GtkListBoxRow *target = gtk_list_box_get_row_at_index(GTK_LIST_BOX(n->launcher_list), next)) {
                gtk_list_box_select_row(GTK_LIST_BOX(n->launcher_list), target);
                gtk_widget_grab_focus(GTK_WIDGET(n->launcher_entry));
            }
            return TRUE;
        }
        return FALSE;
    }), ns);
    gtk_widget_add_controller(GTK_WIDGET(win), key_ctl);

    g_signal_connect(win, "notify::is-active", G_CALLBACK(+[](GObject *obj, GParamSpec *, gpointer d) {
        auto *n = static_cast<NativeShellState *>(d);
        if (!n || !gtk_widget_get_visible(GTK_WIDGET(obj))) return;
        bool active = gtk_window_is_active(GTK_WINDOW(obj));
        if (active) n->launcher_was_active = true;
        else if (n->launcher_was_active) set_launcher_visible(n, false);
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

void native_shell_toggle_launcher(AppState *state) {
    if (!state || !state->native || !state->native->launcher_win) return;
    bool vis = gtk_widget_get_visible(GTK_WIDGET(state->native->launcher_win));
    shell_internal::set_launcher_visible(state->native, !vis);
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
