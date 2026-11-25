#include "shell_internal.hpp"
#include "common.hpp"

#include <gtk4-layer-shell.h>

#include <ctime>
#include <string>
#include <vector>

namespace shell_internal {

namespace {

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

}  // namespace

void sync_top_bar_app_menus(NativeShellState *ns,
                            const TrackedWindowInfo *active_win,
                            std::string &out_display_name) {
    if (!ns || !ns->bar_app_menus) return;

    std::string profile = "finder";
    uint64_t wid = 0;
    out_display_name = "桌面";

    if (active_win) {
        wid = active_win->id;
        std::string hay = to_lower_copy(active_win->app_id + " " + active_win->title);
        if (hay.find("系统设置") != std::string::npos || hay.find("settings") != std::string::npos) {
            profile = "settings";
            out_display_name = "系统设置";
        } else if (hay.find("系统监视器") != std::string::npos || hay.find("spinel monitor") != std::string::npos) {
            profile = "monitor";
            out_display_name = "系统监视器";
        } else if (hay.find("spinel doctor") != std::string::npos) {
            profile = "doctor";
            out_display_name = "Spinel Doctor";
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
        } else if (hay.find("spinel-terminal") != std::string::npos || hay.find("org.spinelos.terminal") != std::string::npos ||
                   hay.find("ptyxis") != std::string::npos || hay.find("foot") != std::string::npos ||
                   hay.find("terminal") != std::string::npos || hay.find("终端") != std::string::npos) {
            profile = "terminal";
            out_display_name = "终端";
        } else if (hay.find("spinel-files") != std::string::npos || hay.find("org.spinelos.files") != std::string::npos ||
                   hay.find("thunar") != std::string::npos || hay.find("文件管理") != std::string::npos) {
            profile = "files";
            out_display_name = "Spinel Files";
        } else if (hay.find("steam") != std::string::npos || hay.find("gamescope") != std::string::npos) {
            profile = "steam";
            out_display_name = hay.find("gamescope") != std::string::npos ? "主机大屏" : "Steam";
        } else {
            profile = "generic";
            std::string short_name;
            std::string laid = to_lower_copy(trim_copy(active_win->app_id));
            if (!laid.empty()) {
                for (const auto &a : ns->desktop_apps) {
                    if (to_lower_copy(a.id) == laid && !a.name.empty()) {
                        short_name = a.name;
                        break;
                    }
                }
                if (short_name.empty()) {
                    std::string s = trim_copy(active_win->app_id);
                    if (s.size() > 8 && to_lower_copy(s.substr(s.size() - 8)) == ".desktop") {
                        s = s.substr(0, s.size() - 8);
                    }
                    auto dot = s.rfind('.');
                    if (dot != std::string::npos && dot + 1 < s.size()) s = s.substr(dot + 1);
                    if (!s.empty()) {
                        s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
                        short_name = s;
                    }
                }
            }
            out_display_name = !short_name.empty() ? short_name :
                               (!active_win->title.empty() ? active_win->title : "应用");
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
        add_topbar_menu(ns, "桌面", {
            {"新建文件夹", "desktop:new-folder"},
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
            {"设备管理器与外置存储", "settings:open:devices"},
            {"声音设备与电源", "settings:open:audio"},
            {"键盘与快捷键", "settings:open:shortcuts"},
            {"游戏性能优化", "settings:open:perf"},
            {"外观主题与输入法", "settings:open:appearance"},
            {"应用权限与安全", "settings:open:perms"}
        });
        add_topbar_menu(ns, "工具", {
            {"系统监视器 (进程与日志)", "monitor:open:procs"},
            {"Spinel Doctor 健康体检", "doctor:open"}
        });
        add_topbar_menu(ns, "窗口", {
            {"关闭系统设置", close_cmd}
        });
    } else if (profile == "monitor") {
        add_topbar_menu(ns, "监视器", {
            {"进程与资源监控", "monitor:open:procs"},
            {"刷新进程列表", "monitor:procs-get"},
            {"系统日志与诊断", "monitor:open:logs"},
            {nullptr, ""},
            {"Spinel Doctor 健康体检", "doctor:open"}
        });
        add_topbar_menu(ns, "窗口", {
            {"关闭系统监视器", close_cmd}
        });
    } else if (profile == "doctor") {
        add_topbar_menu(ns, "体检", {
            {"重新体检", "settings:doctor-scan"},
            {"导出脱敏诊断报告", "settings:doctor-export"},
            {nullptr, ""},
            {"打开系统监视器", "monitor:open:procs"}
        });
        add_topbar_menu(ns, "窗口", {
            {"关闭 Spinel Doctor", close_cmd}
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
            {"系统监视器与日志...", "monitor:open:logs"}
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
    gtk_window_set_default_size(win, -1, 30);

    gtk_layer_init_for_window(win);
    gtk_layer_set_namespace(win, "spinel-bar");
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_auto_exclusive_zone_enable(win);
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(root, "spinel-topbar");
    gtk_widget_set_size_request(root, -1, 30);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_hexpand(left, TRUE);
    gtk_widget_set_halign(left, GTK_ALIGN_START);
    gtk_widget_set_valign(left, GTK_ALIGN_CENTER);

    GtkWidget *brand_btn = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(brand_btn), FALSE);
    gtk_widget_add_css_class(brand_btn, "spinel-bar-brandbtn");
    gtk_widget_set_tooltip_text(brand_btn, "SpinelOS 系统菜单");
    GtkWidget *gem_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(gem_da, 17, 17);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(gem_da), draw_gem_glyph, ns, nullptr);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(brand_btn), gem_da);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(brand_btn), make_topbar_popover_menu(state, {
        {"关于 SpinelOS", "settings:open:appearance"},
        {nullptr, ""},
        {"系统设置...", "settings:open:display"},
        {"应用商店", "store:open"},
        {"游戏与启动中心", "games:open"},
        {"系统监视器", "monitor:open:procs"},
        {"Spinel Doctor 体检", "doctor:open"},
        {nullptr, ""},
        {"锁定屏幕", "system:lock"},
        {"睡眠", "system:suspend"},
        {"重新启动...", "power:reboot"},
        {"关机...", "power:shutdown"}
    }));

    ns->bar_active_lbl = gtk_label_new("桌面");
    gtk_widget_add_css_class(ns->bar_active_lbl, "spinel-bar-active");
    gtk_label_set_ellipsize(GTK_LABEL(ns->bar_active_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(ns->bar_active_lbl), 16);

    ns->bar_app_menus = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 1);

    gtk_box_append(GTK_BOX(left), brand_btn);
    gtk_box_append(GTK_BOX(left), ns->bar_active_lbl);
    gtk_box_append(GTK_BOX(left), ns->bar_app_menus);

    std::string init_name;
    sync_top_bar_app_menus(ns, nullptr, init_name);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_halign(right, GTK_ALIGN_END);
    gtk_widget_set_valign(right, GTK_ALIGN_CENTER);

    GtkWidget *mem_btn = make_cmd_button(state, nullptr, "spinel-bar-pill-btn", "monitor:open:procs");
    ns->bar_mem_lbl = gtk_label_new("--");
    gtk_widget_add_css_class(ns->bar_mem_lbl, "spinel-bar-mem");
    gtk_button_set_child(GTK_BUTTON(mem_btn), ns->bar_mem_lbl);
    gtk_widget_set_tooltip_text(mem_btn, "当前内存占用（点击打开系统监视器）");

    GtkWidget *ime_btn = make_cmd_button(state, nullptr, "spinel-bar-pill-btn", "ime:toggle");
    ns->bar_ime_lbl = gtk_label_new("EN");
    gtk_widget_add_css_class(ns->bar_ime_lbl, "spinel-bar-ime");
    gtk_button_set_child(GTK_BUTTON(ime_btn), ns->bar_ime_lbl);
    gtk_widget_set_tooltip_text(ime_btn, "输入法状态（点击切换中/英输入法）");

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
    gtk_widget_set_size_request(clip_da, 16, 16);
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

    auto rebuild_notif_popover = +[](NativeShellState *n) {
        if (!n || !n->notif_list_box) return;
        while (GtkWidget *c = gtk_widget_get_first_child(n->notif_list_box)) {
            gtk_box_remove(GTK_BOX(n->notif_list_box), c);
        }

        bool dnd = get_dnd_enabled();
        GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        std::string title_txt = dnd ? "通知历史（勿扰已开启）" : "系统通知历史";
        GtkWidget *title = gtk_label_new(title_txt.c_str());
        gtk_widget_add_css_class(title, "spinel-clip-head");
        gtk_widget_set_hexpand(title, TRUE);
        gtk_widget_set_halign(title, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(head), title);

        GtkWidget *dnd_btn = gtk_button_new_with_label(dnd ? "关闭勿扰" : "勿扰");
        gtk_widget_add_css_class(dnd_btn, "spinel-clip-clr");
        g_signal_connect(dnd_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
            auto *st_ns = static_cast<NativeShellState *>(d);
            trigger_cmd(st_ns->app_state, "dnd:toggle");
            if (st_ns->notif_popover) gtk_popover_popdown(GTK_POPOVER(st_ns->notif_popover));
        }), n);
        gtk_box_append(GTK_BOX(head), dnd_btn);

        GtkWidget *clr = gtk_button_new_with_label("清空");
        gtk_widget_add_css_class(clr, "spinel-clip-clr");
        g_signal_connect(clr, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
            auto *st_ns = static_cast<NativeShellState *>(d);
            auto all = fetch_notification_history();
            for (const auto &it : all) {
                if (it.id > st_ns->notif_cleared_max_id) st_ns->notif_cleared_max_id = it.id;
            }
            dismiss_all_notifications();
            if (st_ns->notif_popover) gtk_popover_popdown(GTK_POPOVER(st_ns->notif_popover));
        }), n);
        gtk_box_append(GTK_BOX(head), clr);
        gtk_box_append(GTK_BOX(n->notif_list_box), head);
        gtk_box_append(GTK_BOX(n->notif_list_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

        auto raw_items = fetch_notification_history();
        std::vector<NotificationHistoryItem> items;
        for (auto &it : raw_items) {
            if (it.id == 0 || it.id > n->notif_cleared_max_id) {
                items.push_back(std::move(it));
            }
        }
        if (items.empty()) {
            GtkWidget *empty = gtk_label_new("暂无历史通知\n（系统通知与截图/安装提示会保留在此）");
            gtk_widget_add_css_class(empty, "spinel-clip-empty");
            gtk_label_set_justify(GTK_LABEL(empty), GTK_JUSTIFY_CENTER);
            gtk_box_append(GTK_BOX(n->notif_list_box), empty);
            return;
        }

        for (const auto &it : items) {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_add_css_class(row, "spinel-clip-row");
            std::string top_s = (it.app_name.empty() ? "系统" : it.app_name) + " · " +
                                (it.summary.empty() ? "通知" : it.summary);
            GtkWidget *t_lbl = gtk_label_new(top_s.c_str());
            gtk_widget_add_css_class(t_lbl, "spinel-clip-txt");
            gtk_widget_set_halign(t_lbl, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(t_lbl), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(t_lbl), 38);
            gtk_box_append(GTK_BOX(row), t_lbl);

            if (!it.body.empty()) {
                GtkWidget *b_lbl = gtk_label_new(it.body.c_str());
                gtk_widget_add_css_class(b_lbl, "spinel-clip-meta");
                gtk_widget_set_halign(b_lbl, GTK_ALIGN_START);
                gtk_label_set_ellipsize(GTK_LABEL(b_lbl), PANGO_ELLIPSIZE_END);
                gtk_label_set_max_width_chars(GTK_LABEL(b_lbl), 42);
                gtk_box_append(GTK_BOX(row), b_lbl);
            }
            gtk_box_append(GTK_BOX(n->notif_list_box), row);
        }
    };

    GtkWidget *search_btn = make_glyph_icon_btn(state, BarGlyph::Search, "全局搜索 / 启动器 (Alt+Space)", "launcher:toggle");

    ns->bar_vol_btn = make_cmd_button(state, nullptr, "spinel-bar-icon-btn", "system:audio-mute-toggle");
    gtk_widget_set_tooltip_text(ns->bar_vol_btn, "声音状态（点击静音切换）");
    GtkWidget *vol_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(vol_da, 16, 16);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(vol_da),
        +[](GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer d) {
            auto *n = static_cast<NativeShellState *>(d);
            GdkRGBA fg{1.0f, 1.0f, 1.0f, 0.94f};
            gtk_widget_get_color(GTK_WIDGET(da), &fg);
            draw_monochrome_glyph(cr, w, h, (n && n->bar_vol_muted) ? BarGlyph::Mute : BarGlyph::Volume, fg);
        }, ns, nullptr);
    gtk_button_set_child(GTK_BUTTON(ns->bar_vol_btn), vol_da);
    ns->bar_vol_da = vol_da;

    ns->bar_net_btn = make_cmd_button(state, nullptr, "spinel-bar-icon-btn", "settings:open:wifi");
    gtk_widget_set_tooltip_text(ns->bar_net_btn, "网络状态（点击打开网络设置）");
    GtkWidget *net_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(net_da, 16, 16);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(net_da),
        +[](GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer d) {
            auto *n = static_cast<NativeShellState *>(d);
            GdkRGBA fg{1.0f, 1.0f, 1.0f, 0.94f};
            gtk_widget_get_color(GTK_WIDGET(da), &fg);
            BarGlyph g = BarGlyph::Wifi;
            if (n && n->bar_net_kind == "ethernet") g = BarGlyph::Ethernet;
            else if (n && n->bar_net_kind == "offline") g = BarGlyph::Offline;
            draw_monochrome_glyph(cr, w, h, g, fg);
        }, ns, nullptr);
    gtk_button_set_child(GTK_BUTTON(ns->bar_net_btn), net_da);
    ns->bar_net_da = net_da;

    ns->bar_cc_btn = make_glyph_icon_btn(state, BarGlyph::ControlCenter, "控制中心", "cc:toggle");

    ns->bar_bat_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(ns->bar_bat_box, "spinel-bar-bat");
    gtk_widget_set_valign(ns->bar_bat_box, GTK_ALIGN_CENTER);
    GtkWidget *bat_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(bat_da, 16, 16);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(bat_da), draw_glyph_cb,
                                   reinterpret_cast<gpointer>(static_cast<intptr_t>(BarGlyph::Battery)), nullptr);
    ns->bar_bat_lbl = gtk_label_new("");
    gtk_widget_add_css_class(ns->bar_bat_lbl, "spinel-bar-mem");
    gtk_box_append(GTK_BOX(ns->bar_bat_box), bat_da);
    gtk_box_append(GTK_BOX(ns->bar_bat_box), ns->bar_bat_lbl);
    gtk_widget_set_visible(ns->bar_bat_box, FALSE);

    GtkWidget *notif_clock_btn = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(notif_clock_btn), FALSE);
    gtk_widget_add_css_class(notif_clock_btn, "spinel-bar-menubtn");
    gtk_widget_add_css_class(notif_clock_btn, "spinel-bar-clock-btn");
    gtk_widget_set_tooltip_text(notif_clock_btn, "通知历史、勿扰模式与日期时间");

    GtkWidget *nc_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(nc_row, GTK_ALIGN_CENTER);
    GtkWidget *notif_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(notif_da, 15, 15);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(notif_da),
        +[](GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer d) {
            auto *n = static_cast<NativeShellState *>(d);
            GdkRGBA fg{1.0f, 1.0f, 1.0f, 0.94f};
            gtk_widget_get_color(GTK_WIDGET(da), &fg);
            draw_monochrome_glyph(cr, w, h, (n && n->bar_notif_dnd) ? BarGlyph::Dnd : BarGlyph::Bell, fg);
        }, ns, nullptr);
    ns->bar_notif_da = notif_da;

    ns->bar_clock_lbl = gtk_label_new("--:--");
    gtk_widget_add_css_class(ns->bar_clock_lbl, "spinel-bar-clock");
    gtk_box_append(GTK_BOX(nc_row), notif_da);
    gtk_box_append(GTK_BOX(nc_row), ns->bar_clock_lbl);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(notif_clock_btn), nc_row);

    GtkWidget *npop = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(npop), FALSE);
    gtk_widget_add_css_class(npop, "spinel-ctx-popover");
    GtkWidget *nbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_size_request(nbox, 320, -1);
    gtk_popover_set_child(GTK_POPOVER(npop), nbox);
    ns->notif_popover = npop;
    ns->notif_list_box = nbox;
    g_object_set_data(G_OBJECT(npop), "rebuild_fn", reinterpret_cast<gpointer>(rebuild_notif_popover));
    g_signal_connect(npop, "notify::visible", G_CALLBACK(+[](GObject *obj, GParamSpec *, gpointer d) {
        if (gtk_widget_get_visible(GTK_WIDGET(obj))) {
            auto fn = reinterpret_cast<void (*)(NativeShellState *)>(g_object_get_data(obj, "rebuild_fn"));
            if (fn) fn(static_cast<NativeShellState *>(d));
        }
    }), ns);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(notif_clock_btn), npop);

    auto make_bar_sep = +[]() -> GtkWidget * {
        GtkWidget *s = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(s, "spinel-bar-sep");
        gtk_widget_set_size_request(s, 1, 12);
        gtk_widget_set_valign(s, GTK_ALIGN_CENTER);
        return s;
    };

    gtk_box_append(GTK_BOX(right), mem_btn);
    gtk_box_append(GTK_BOX(right), ime_btn);
    gtk_box_append(GTK_BOX(right), clip_btn);
    gtk_box_append(GTK_BOX(right), search_btn);
    gtk_box_append(GTK_BOX(right), make_bar_sep());
    gtk_box_append(GTK_BOX(right), ns->bar_vol_btn);
    gtk_box_append(GTK_BOX(right), ns->bar_net_btn);
    gtk_box_append(GTK_BOX(right), ns->bar_cc_btn);
    gtk_box_append(GTK_BOX(right), ns->bar_bat_box);
    gtk_box_append(GTK_BOX(right), make_bar_sep());
    gtk_box_append(GTK_BOX(right), notif_clock_btn);

    gtk_box_append(GTK_BOX(root), left);
    gtk_box_append(GTK_BOX(root), right);
    gtk_window_set_child(win, root);

    ns->bar_win = win;
    on_clock_tick(ns);
    ns->clock_timer_id = g_timeout_add_seconds(5, on_clock_tick, ns);
    gtk_window_present(win);
}

}  // namespace shell_internal
