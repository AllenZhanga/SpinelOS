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

void rebuild_launcher_index(NativeShellState *ns) {
    if (!ns) return;
    ns->launcher_entries.clear();
    ns->desktop_apps = list_desktop_apps();
    const bool sb = ns->app_state && ns->app_state->sandbox_shell_mode;
    bool steam_installed = has_executable("steam");

    if (sb) {
        ns->launcher_entries = {
            {"Spinel 终端", "原生多标签 Wayland 终端", "terminal ptyxis shell console bash", "launch:terminal", "", "terminal"},
            {"文件管理 (Spinel Files)", "轻量原生文件管理、目录浏览、解压与压缩", "files manager folder spinel-files", "launch:files", "", "files"},
            {"浏览器", "启动沙箱内隔离浏览器 (Chromium / Chrome / Firefox)", "browser chrome chromium firefox web", "launch:browser",
             find_app_icon_path(ns->desktop_apps, {"google-chrome", "chromium", "firefox"}), "browser"}
        };
    } else {
        ns->launcher_entries = {
        {"Spinel 终端", "原生多标签 Wayland 终端 (Super+Enter)", "terminal ptyxis shell console bash", "launch:terminal", "", "terminal"},
        {"应用中心", "安装原生软件、办公工具、AI 大模型与管理全部应用", "store app center market launchpad pacman", "store:open", "", "store"},
        {"游戏中心", "浏览 Steam 游戏库、单游戏调优与清理游戏缓存", "games library steam launcher center", "games:open", "", "games"},
        {"文件管理 (Spinel Files)", "轻量原生文件管理、外置盘、解压与压缩", "files manager folder spinel-files", "launch:files", "", "files"},
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
        {"系统设置 · 权限与安全", "AI Agent 权限、平行沙箱与 Portal 弹窗管理", "settings permissions security flatpak polkit ai mcp", "settings:open:perms", "", "settings"},
        {"系统监视器 · 进程与资源监控", "查看实时 CPU/PSS 内存占用、搜索并结束进程", "monitor activity process cpu memory kill task", "monitor:open:procs", "", "settings"},
        {"系统监视器 · 系统日志与诊断", "查看应用、Steam、桌面壳与 AI/MCP 审计日志", "monitor console logs journalctl dmesg debug mcp", "monitor:open:logs", "", "settings"}
    };

    if (ns->telemetry.is_live) {
        ns->launcher_entries.push_back({
            "安装 Spinel OS 到硬盘", "启动系统图形安装向导", "installer disk setup live", "installer:open", "", "installer"
        });
    }
    }

    for (const auto &app : ns->desktop_apps) {
        std::string lid = to_lower_copy(app.id);
        if (lid == "thunar" || lid == "steam" || lid == "spinel-terminal" || lid == "org.spinelos.terminal" ||
            lid == "spinel-files" || lid == "org.spinelos.files" ||
            lid == "org.spinelos.store" || lid == "org.spinelos.games") continue;
        if (sb && lid.rfind("org.spinelos.", 0) == 0 && lid != "org.spinelos.windowsapps") continue;
        const bool wine = app.id.starts_with("wine:");
        const bool windows_manager = lid == "org.spinelos.windowsapps";
        ns->launcher_entries.push_back({
            app.name,
            windows_manager ? "启动和卸载 Wine 软件" : wine ? "Windows 应用 · Wine" :
                (!app.category.empty() ? ("已安装应用 · " + app.category) : "已安装桌面应用"),
            app.id + " " + app.category + (windows_manager ? " wine windows applications" : ""),
            "app:launch:" + app.id,
            app.icon_path,
            match_app_kind(app.id + " " + app.name)
        });
    }

    if (!sb) {
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
}

void scroll_launcher_row_into_view(NativeShellState *ns, GtkListBoxRow *row) {
    if (!ns || !ns->launcher_scroll || !ns->launcher_list || !row) return;
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ns->launcher_scroll));
    if (!vadj) return;

    double row_top = 0.0;
    double row_bot = 0.0;
    graphene_rect_t bounds{};
    if (gtk_widget_compute_bounds(GTK_WIDGET(row), ns->launcher_list, &bounds) && bounds.size.height > 1.0f) {
        row_top = static_cast<double>(bounds.origin.y);
        row_bot = row_top + static_cast<double>(bounds.size.height);
    } else {
        int idx = std::max(0, gtk_list_box_row_get_index(row));
        double est_h = 52.0;
        row_top = idx * est_h;
        row_bot = row_top + est_h;
    }

    const double pad = 6.0;
    row_top = std::max(0.0, row_top - pad);
    row_bot += pad;

    double val = gtk_adjustment_get_value(vadj);
    double page = gtk_adjustment_get_page_size(vadj);
    double upper = gtk_adjustment_get_upper(vadj);
    double max_val = std::max(0.0, upper - page);
    if (page <= 1.0) return;

    if (row_top < val) {
        gtk_adjustment_set_value(vadj, std::clamp(row_top, 0.0, max_val));
    } else if (row_bot > val + page) {
        gtk_adjustment_set_value(vadj, std::clamp(row_bot - page, 0.0, max_val));
    }
}

int count_launcher_rows(GtkListBox *list) {
    int count = 0;
    for (GtkWidget *c = gtk_widget_get_first_child(GTK_WIDGET(list)); c; c = gtk_widget_get_next_sibling(c)) {
        ++count;
    }
    return count;
}

int compute_launcher_top_margin(GtkWindow *win) {
    GdkMonitor *mon = (win && gtk_layer_is_layer_window(win)) ? gtk_layer_get_monitor(win) : nullptr;
    if (!mon) {
        if (GdkDisplay *display = gdk_display_get_default()) {
            if (GListModel *mons = gdk_display_get_monitors(display)) {
                if (g_list_model_get_n_items(mons) > 0) {
                    mon = GDK_MONITOR(g_list_model_get_item(mons, 0));
                    if (mon) {
                        GdkRectangle geom{};
                        gdk_monitor_get_geometry(mon, &geom);
                        g_object_unref(mon);
                        if (geom.height > 0) {
                            return std::clamp(static_cast<int>(std::round(geom.height * 0.25)) - 28, 150, 420);
                        }
                        return 240;
                    }
                }
            }
        }
        return 240;
    }
    GdkRectangle geom{};
    gdk_monitor_get_geometry(mon, &geom);
    if (geom.height > 0) {
        return std::clamp(static_cast<int>(std::round(geom.height * 0.25)) - 28, 150, 420);
    }
    return 240;
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
    if (matches.size() > 24) matches.resize(24);

    for (const auto &m : matches) {
        const auto *e = m.entry;
        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget *da = gtk_drawing_area_new();
        gtk_widget_set_size_request(da, 30, 30);
        auto *ctx = new DesktopIconDrawCtx{ns, e->icon_path, e->kind};
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_desktop_icon_cb, ctx, free_desktop_icon_ctx);

        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(col, TRUE);
        GtkWidget *t = gtk_label_new(e->title.c_str());
        gtk_widget_add_css_class(t, "spinel-launcher-title");
        gtk_widget_set_halign(t, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
        GtkWidget *s = gtk_label_new(e->subtitle.c_str());
        gtk_widget_add_css_class(s, "spinel-launcher-sub");
        gtk_widget_set_halign(s, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(s), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(col), t);
        gtk_box_append(GTK_BOX(col), s);

        GtkWidget *enter_lbl = gtk_label_new("打开 ↵");
        gtk_widget_add_css_class(enter_lbl, "spinel-launcher-enter");
        gtk_widget_set_valign(enter_lbl, GTK_ALIGN_CENTER);

        gtk_box_append(GTK_BOX(row_box), da);
        gtk_box_append(GTK_BOX(row_box), col);
        gtk_box_append(GTK_BOX(row_box), enter_lbl);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_box);
        g_object_set_data_full(G_OBJECT(row), "launcher-cmd", g_strdup(e->cmd.c_str()), g_free);
        gtk_list_box_append(GTK_LIST_BOX(ns->launcher_list), row);
    }

    if (ns->launcher_scroll) {
        gtk_widget_set_visible(ns->launcher_scroll, !matches.empty() ? TRUE : FALSE);
        if (GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ns->launcher_scroll))) {
            gtk_adjustment_set_value(vadj, 0.0);
        }
    }

    if (GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(ns->launcher_list), 0)) {
        gtk_list_box_select_row(GTK_LIST_BOX(ns->launcher_list), first);
    }
}

void activate_selected_launcher_row(NativeShellState *ns, GtkListBoxRow *row = nullptr, bool in_sandbox = false) {
    if (!ns || !ns->launcher_list) return;
    if (!row) row = gtk_list_box_get_selected_row(GTK_LIST_BOX(ns->launcher_list));
    if (!row) return;
    const char *cmd = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "launcher-cmd"));
    std::string cmd_copy = cmd ? cmd : "";
    set_launcher_visible(ns, false);
    if (!cmd_copy.empty()) {
        if (in_sandbox) trigger_cmd(ns->app_state, "sandbox:launch:" + cmd_copy);
        else trigger_cmd(ns->app_state, cmd_copy);
    }
}

}  // namespace

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
        gtk_layer_set_margin(ns->launcher_win, GTK_LAYER_SHELL_EDGE_TOP, compute_launcher_top_margin(ns->launcher_win));
        gtk_layer_set_keyboard_mode(ns->launcher_win, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
        gtk_widget_set_visible(GTK_WIDGET(ns->launcher_win), TRUE);
        gtk_window_present(ns->launcher_win);
        gtk_widget_grab_focus(ns->launcher_entry);
    } else {
        gtk_layer_set_keyboard_mode(ns->launcher_win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_widget_set_visible(GTK_WIDGET(ns->launcher_win), FALSE);
    }
}

void build_native_launcher(GtkApplication *app, NativeShellState *ns) {
    GtkWindow *backdrop = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(backdrop, "SpinelLauncherBackdrop");
    gtk_window_set_decorated(backdrop, FALSE);
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
    gtk_window_set_decorated(win, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(win), "spinel-transparent");
    gtk_window_set_default_size(win, -1, -1);
    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "spinel-launcher");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_TOP, 240);
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_widget_set_visible(GTK_WIDGET(win), FALSE);

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(panel, "spinel-launcher-panel");
    gtk_widget_set_size_request(panel, 620, -1);
    gtk_widget_set_margin_top(panel, 28);
    gtk_widget_set_margin_bottom(panel, 48);
    gtk_widget_set_margin_start(panel, 36);
    gtk_widget_set_margin_end(panel, 36);

    GtkWidget *search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(search_row, "spinel-launcher-search");
    GtkWidget *ico = gtk_drawing_area_new();
    gtk_widget_set_size_request(ico, 20, 20);
    gtk_widget_set_valign(ico, GTK_ALIGN_CENTER);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ico), draw_glyph_cb,
                                   reinterpret_cast<gpointer>(static_cast<intptr_t>(BarGlyph::Search)), nullptr);

    GtkWidget *entry = gtk_entry_new();
    gtk_widget_add_css_class(entry, "spinel-launcher-entry");
    gtk_entry_set_has_frame(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "搜索应用、Steam 游戏、系统设置或命令...");
    gtk_widget_set_hexpand(entry, TRUE);

    GtkWidget *esc_hint = gtk_label_new("Esc");
    gtk_widget_add_css_class(esc_hint, "spinel-launcher-esc");
    gtk_widget_set_valign(esc_hint, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(search_row), ico);
    gtk_box_append(GTK_BOX(search_row), entry);
    gtk_box_append(GTK_BOX(search_row), esc_hint);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 392);
    gtk_widget_set_margin_top(scroll, 6);
    gtk_widget_set_margin_bottom(scroll, 8);
    gtk_widget_set_margin_start(scroll, 8);
    gtk_widget_set_margin_end(scroll, 8);

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
    ns->launcher_scroll = scroll;
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
    g_signal_connect(key_ctl, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey *, guint keyval, guint, GdkModifierType mods, gpointer d) -> gboolean {
        auto *n = static_cast<NativeShellState *>(d);
        if (keyval == GDK_KEY_Escape) {
            set_launcher_visible(n, false);
            return TRUE;
        }
        if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && (mods & GDK_SHIFT_MASK)) {
            activate_selected_launcher_row(n, nullptr, true);
            return TRUE;
        }
        int step = 0;
        if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) step = 1;
        else if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) step = -1;
        else if (keyval == GDK_KEY_Page_Down || keyval == GDK_KEY_KP_Page_Down) step = 5;
        else if (keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_KP_Page_Up) step = -5;
        if (step != 0) {
            int total = count_launcher_rows(GTK_LIST_BOX(n->launcher_list));
            if (total > 0) {
                GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(n->launcher_list));
                int cur = sel ? gtk_list_box_row_get_index(sel) : 0;
                int next = std::clamp(cur + step, 0, total - 1);
                if (GtkListBoxRow *target = gtk_list_box_get_row_at_index(GTK_LIST_BOX(n->launcher_list), next)) {
                    gtk_list_box_select_row(GTK_LIST_BOX(n->launcher_list), target);
                    scroll_launcher_row_into_view(n, target);
                    gtk_widget_grab_focus(GTK_WIDGET(n->launcher_entry));
                }
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

}  // namespace shell_internal

void native_shell_toggle_launcher(AppState *state) {
    if (!state || !state->native || !state->native->launcher_win) return;
    bool vis = gtk_widget_get_visible(GTK_WIDGET(state->native->launcher_win));
    if (!vis) shell_internal::close_switcher(state->native, false);
    shell_internal::set_launcher_visible(state->native, !vis);
}
