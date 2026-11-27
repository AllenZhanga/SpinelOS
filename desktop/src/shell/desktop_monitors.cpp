#include "shell_internal.hpp"
#include "common.hpp"
#include "steam_library.hpp"

#include <gtk4-layer-shell.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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

void open_desktop_entry(const std::string &path, bool is_dir, bool is_exec, bool is_desktop_file) {
    if (path.empty()) return;
    if (is_dir) {
        spawn_argv_detached({"spinel-files", path});
    } else if (is_desktop_file) {
        launch_desktop_app_file(path);
    } else if (is_exec && ::access(path.c_str(), X_OK) == 0) {
        spawn_argv_detached({path});
    } else if (has_executable("xdg-open")) {
        spawn_argv_detached({"xdg-open", path});
    } else {
        spawn_argv_detached({"spinel-files", path});
    }
}

void trash_desktop_entry(NativeShellState *ns, const std::string &path) {
    if (path.empty()) return;
    g_autoptr(GFile) file = g_file_new_for_path(path.c_str());
    g_autoptr(GError) error = nullptr;
    if (g_file_trash(file, nullptr, &error)) {
        if (ns) rebuild_desktop_shortcuts(ns);
        return;
    }

    g_autoptr(GtkAlertDialog) dialog = gtk_alert_dialog_new("无法移到回收站");
    gtk_alert_dialog_set_detail(dialog, error ? error->message : "无法回收此项目。");
    gtk_alert_dialog_show(dialog, nullptr);
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
        const char *epath = static_cast<const char *>(g_object_get_data(G_OBJECT(tile), "spinel-entry-path"));
        if (epath && *epath) {
            bool is_dir = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tile), "spinel-entry-dir")) != 0;
            bool is_exec = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tile), "spinel-entry-exec")) != 0;
            bool is_df = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tile), "spinel-entry-desktop")) != 0;
            open_desktop_entry(epath, is_dir, is_exec, is_df);
            return;
        }
        const char *cmd = static_cast<const char *>(g_object_get_data(G_OBJECT(tile), "spinel-cmd"));
        if (cmd) trigger_cmd(ns->app_state, cmd);
    }
}

void on_desktop_tile_right_click(GtkGestureClick *gesture, int, double x, double y, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    GtkWidget *tile = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    if (!ns || !tile) return;
    const char *epath = static_cast<const char *>(g_object_get_data(G_OBJECT(tile), "spinel-entry-path"));
    if (!epath || !*epath || !ns->desktop_entry_popover) return;

    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    clear_desktop_shortcut_selection(ns);
    gtk_widget_add_css_class(tile, "selected");

    ns->desktop_ctx_path = epath;
    ns->desktop_ctx_is_dir = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tile), "spinel-entry-dir")) != 0;
    ns->desktop_ctx_is_exec = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tile), "spinel-entry-exec")) != 0;
    ns->desktop_ctx_is_desktop_file = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tile), "spinel-entry-desktop")) != 0;

    GtkWidget *term_btn = GTK_WIDGET(g_object_get_data(G_OBJECT(ns->desktop_entry_popover), "sf-term-btn"));
    if (term_btn) gtk_widget_set_visible(term_btn, ns->desktop_ctx_is_dir);

    graphene_point_t pt{static_cast<float>(x), static_cast<float>(y)};
    graphene_point_t out_pt{};
    if (!gtk_widget_compute_point(tile, ns->desktop_overlay, &pt, &out_pt)) {
        out_pt = pt;
    }
    GdkRectangle rect{static_cast<int>(out_pt.x), static_cast<int>(out_pt.y), 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(ns->desktop_entry_popover), &rect);
    gtk_popover_popup(GTK_POPOVER(ns->desktop_entry_popover));
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
    const bool sb = ns && ns->app_state && ns->app_state->sandbox_shell_mode;
    const std::vector<MenuItem> entries = sb ? std::vector<MenuItem>{
        {"新建文件夹", "desktop:new-folder"},
        {"在文件管理中打开桌面", "desktop:open-folder"},
        {"打开终端", "launch:terminal"},
        {nullptr, nullptr},
        {"搜索应用 (Alt+Space)", "launcher:toggle"},
        {"刷新桌面", "telemetry:refresh"}
    } : std::vector<MenuItem>{
        {"新建文件夹", "desktop:new-folder"},
        {"在文件管理中打开桌面", "desktop:open-folder"},
        {"打开终端", "launch:terminal"},
        {nullptr, nullptr},
        {"刷新桌面", "telemetry:refresh"},
        {"更换壁纸与外观...", "settings:open:appearance"},
        {"显示与缩放设置...", "settings:open:display"}
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

void build_desktop_entry_popover(NativeShellState *ns) {
    GtkWidget *pop = gtk_popover_new();
    gtk_widget_set_parent(pop, ns->desktop_overlay);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_widget_add_css_class(pop, "spinel-ctx-popover");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    auto make_btn = [&](const char *lbl, GCallback cb) {
        GtkWidget *b = gtk_button_new_with_label(lbl);
        gtk_widget_add_css_class(b, "spinel-ctx-item");
        GtkWidget *c = gtk_button_get_child(GTK_BUTTON(b));
        if (GTK_IS_LABEL(c)) gtk_widget_set_halign(c, GTK_ALIGN_START);
        g_signal_connect(b, "clicked", cb, ns);
        gtk_box_append(GTK_BOX(box), b);
        return b;
    };

    make_btn("打开", G_CALLBACK(+[](GtkButton *, gpointer d) {
        auto *n = static_cast<NativeShellState *>(d);
        if (n->desktop_entry_popover) gtk_popover_popdown(GTK_POPOVER(n->desktop_entry_popover));
        open_desktop_entry(n->desktop_ctx_path, n->desktop_ctx_is_dir, n->desktop_ctx_is_exec, n->desktop_ctx_is_desktop_file);
    }));
    make_btn("在 Spinel Files 中显示", G_CALLBACK(+[](GtkButton *, gpointer d) {
        auto *n = static_cast<NativeShellState *>(d);
        if (n->desktop_entry_popover) gtk_popover_popdown(GTK_POPOVER(n->desktop_entry_popover));
        if (!n->desktop_ctx_path.empty()) spawn_argv_detached({"spinel-files", n->desktop_ctx_path});
    }));
    GtkWidget *term_b = make_btn("在终端中打开", G_CALLBACK(+[](GtkButton *, gpointer d) {
        auto *n = static_cast<NativeShellState *>(d);
        if (n->desktop_entry_popover) gtk_popover_popdown(GTK_POPOVER(n->desktop_entry_popover));
        if (!n->desktop_ctx_path.empty()) {
            const std::string bin = has_executable("spinel-terminal") ? "spinel-terminal" : "ptyxis";
            spawn_argv_detached({bin, "-d", n->desktop_ctx_path});
        }
    }));
    g_object_set_data(G_OBJECT(pop), "sf-term-btn", term_b);
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    make_btn("移到回收站", G_CALLBACK(+[](GtkButton *, gpointer d) {
        auto *n = static_cast<NativeShellState *>(d);
        if (n->desktop_entry_popover) gtk_popover_popdown(GTK_POPOVER(n->desktop_entry_popover));
        trash_desktop_entry(n, n->desktop_ctx_path);
    }));

    gtk_popover_set_child(GTK_POPOVER(pop), box);
    ns->desktop_entry_popover = pop;
}

std::string classify_desktop_file_kind(const fs::path &p, bool is_dir, bool &out_exec, bool &out_desktop_file) {
    out_exec = false;
    out_desktop_file = false;
    if (is_dir) return "desk-dir";
    std::string ext = to_lower_copy(p.extension().string());
    if (ext == ".desktop") {
        out_desktop_file = true;
        return "app";
    }
    if (ext == ".zip" || ext == ".tar" || ext == ".gz" || ext == ".xz" ||
        ext == ".bz2" || ext == ".zst" || ext == ".7z" || ext == ".rar" ||
        ext == ".tgz" || ext == ".tbz2") {
        return "desk-arc";
    }
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".svg" || ext == ".bmp") {
        return "desk-img";
    }
    if (ext == ".appimage" || ext == ".sh" || ::access(p.c_str(), X_OK) == 0) {
        out_exec = true;
        return "desk-exec";
    }
    return "desk-file";
}

void on_desktop_dir_changed(GFileMonitor *, GFile *, GFile *, GFileMonitorEvent, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    if (!ns) return;
    if (ns->desktop_refresh_timer) {
        g_source_remove(ns->desktop_refresh_timer);
    }
    ns->desktop_refresh_timer = g_timeout_add(180, +[](gpointer d) -> gboolean {
        auto *n = static_cast<NativeShellState *>(d);
        if (n) {
            n->desktop_refresh_timer = 0;
            rebuild_desktop_shortcuts(n);
        }
        return G_SOURCE_REMOVE;
    }, ns);
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
        std::string entry_path;
        bool is_dir = false;
        bool is_exec = false;
        bool is_desktop_file = false;
    };

    std::vector<ShortcutDef> items;
    auto push_sc = [&](std::string lbl, std::string cmd, std::string kind, std::string ico) {
        items.push_back({std::move(lbl), std::move(cmd), std::move(kind), std::move(ico), "", false, false, false});
    };
    const bool sb = ns->app_state && ns->app_state->sandbox_shell_mode;
    if (sb) {
        push_sc("文件管理", "launch:files", "files", "");
        push_sc("终端", "launch:terminal", "terminal", "");
        if (has_executable("google-chrome-stable") || has_executable("chromium") || has_executable("firefox")) {
            push_sc("浏览器", "launch:browser", "browser",
                    find_app_icon_path(ns->desktop_apps, {"google-chrome", "chromium", "firefox"}));
        }
    } else {
        push_sc("游戏中心", "games:open", "games", find_app_icon_path(ns->desktop_apps, {"org.spinelos.games"}));
        push_sc("应用中心", "store:open", "store", find_app_icon_path(ns->desktop_apps, {"org.spinelos.store"}));
        push_sc("Steam", "launch:steam", "steam", find_app_icon_path(ns->desktop_apps, {"steam"}));
        push_sc("主机大屏", "launch:steam-gamepadui", "gamepad", "");
        push_sc("文件管理", "launch:files", "files", "");
        push_sc("终端", "launch:terminal", "terminal", "");
        push_sc("系统设置", "settings:open:display", "settings", "");
        if (ns->telemetry.is_live) {
            push_sc("安装 Spinel OS", "installer:open", "installer", "");
        }

        static const char *kFeaturedStoreIds[] = {
            "google-chrome", "com.google.chrome", "chromium", "firefox", "heroic", "com.heroicgameslauncher.hgl",
            "lutris", "protonup-qt", "gravitymark", "discord", "com.obsproject.studio"
        };
        for (const auto &app : ns->desktop_apps) {
            std::string lid = to_lower_copy(app.id);
            for (const char *fid : kFeaturedStoreIds) {
                if (lid == fid) {
                    push_sc(app.name, "app:launch:" + app.id, match_app_kind(app.id), app.icon_path);
                    break;
                }
            }
        }

        auto steam_games = list_steam_games();
        int added_games = 0;
        for (const auto &g : steam_games) {
            if (added_games >= 6) break;
            push_sc(g.name, "steam:launch:" + g.appid, "steam",
                    !g.cover_path.empty() ? g.cover_path : find_app_icon_path(ns->desktop_apps, {"steam"}));
            ++added_games;
        }
    }

    std::error_code ec;
    fs::path desk_dir = fs::path(home_dir()) / "Desktop";
    if (fs::is_directory(desk_dir, ec)) {
        std::vector<ShortcutDef> desk_entries;
        for (const auto &de : fs::directory_iterator(desk_dir, fs::directory_options::skip_permission_denied, ec)) {
            std::string nm = de.path().filename().string();
            if (nm.empty() || nm.front() == '.') continue;
            bool is_dir = de.is_directory(ec);
            bool is_exec = false;
            bool is_df = false;
            std::string k = classify_desktop_file_kind(de.path(), is_dir, is_exec, is_df);
            std::string ico;
            if (k == "desk-img" && !is_dir && de.file_size(ec) < 8 * 1024 * 1024) {
                ico = de.path().string();
            }
            desk_entries.push_back({nm, "", k, ico, de.path().string(), is_dir, is_exec, is_df});
        }
        std::sort(desk_entries.begin(), desk_entries.end(), [](const ShortcutDef &a, const ShortcutDef &b) {
            if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
            return to_lower_copy(a.label) < to_lower_copy(b.label);
        });
        for (auto &d : desk_entries) items.push_back(std::move(d));
    }

    for (const auto &sc : items) {
        GtkWidget *tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_widget_add_css_class(tile, "spinel-shortcut-btn");
        gtk_widget_set_size_request(tile, 84, 84);
        gtk_widget_set_halign(tile, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(tile, GTK_ALIGN_START);
        if (!sc.cmd.empty()) {
            g_object_set_data_full(G_OBJECT(tile), "spinel-cmd", g_strdup(sc.cmd.c_str()), g_free);
        }
        if (!sc.entry_path.empty()) {
            g_object_set_data_full(G_OBJECT(tile), "spinel-entry-path", g_strdup(sc.entry_path.c_str()), g_free);
            g_object_set_data(G_OBJECT(tile), "spinel-entry-dir", GINT_TO_POINTER(sc.is_dir ? 1 : 0));
            g_object_set_data(G_OBJECT(tile), "spinel-entry-exec", GINT_TO_POINTER(sc.is_exec ? 1 : 0));
            g_object_set_data(G_OBJECT(tile), "spinel-entry-desktop", GINT_TO_POINTER(sc.is_desktop_file ? 1 : 0));

            GtkGesture *tr = gtk_gesture_click_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(tr), GDK_BUTTON_SECONDARY);
            g_signal_connect(tr, "pressed", G_CALLBACK(on_desktop_tile_right_click), ns);
            gtk_widget_add_controller(tile, GTK_EVENT_CONTROLLER(tr));
        }

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
    build_desktop_entry_popover(ns);
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

    std::error_code ec;
    fs::path desk_path = fs::path(home_dir()) / "Desktop";
    fs::create_directories(desk_path, ec);
    GFile *gf = g_file_new_for_path(desk_path.c_str());
    ns->desktop_dir_monitor = g_file_monitor_directory(gf, G_FILE_MONITOR_NONE, nullptr, nullptr);
    g_object_unref(gf);
    if (ns->desktop_dir_monitor) {
        g_signal_connect(ns->desktop_dir_monitor, "changed", G_CALLBACK(on_desktop_dir_changed), ns);
    }
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
            ns->switcher_backdrop_win, ns->switcher_win,
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
