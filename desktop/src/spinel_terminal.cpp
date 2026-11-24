#include "common.hpp"

#include <gtk/gtk.h>
#include <vte/vte.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr double kMinZoom = 0.6;
constexpr double kMaxZoom = 2.2;

struct TermTheme {
    std::string id;
    bool is_light = false;
    std::string bar_bg;
    std::string bar_border;
    std::string tab_bg;
    std::string tab_active_bg;
    std::string tab_fg;
    std::string tab_muted;
    std::string accent;
    GdkRGBA term_bg{};
    GdkRGBA term_fg{};
    std::array<GdkRGBA, 16> palette{};
};

GdkRGBA make_rgba(uint32_t rgb, double alpha = 1.0) {
    return GdkRGBA{
        static_cast<float>(((rgb >> 16) & 0xFFu) / 255.0),
        static_cast<float>(((rgb >> 8) & 0xFFu) / 255.0),
        static_cast<float>((rgb & 0xFFu) / 255.0),
        static_cast<float>(alpha)
    };
}

TermTheme load_spinel_term_theme() {
    std::string id = trim_copy(read_first_line(
        (fs::path(home_dir()) / ".config/spinelos/theme.conf").string(), ""
    ));
    if (id.empty()) {
        std::string legacy = trim_copy(read_first_line(
            (fs::path(home_dir()) / ".config/spinelos/wallpaper.conf").string(), ""
        ));
        if (legacy == "obsidian-purple") id = "obsidian-neon";
        else if (legacy == "emerald-aurora") id = "emerald-nordic";
        else id = "tahoe-light";
    }

    bool is_light = (id == "tahoe-light");
    if (!is_light && id != "crystal-aurora" && id != "obsidian-neon" && id != "emerald-nordic") {
        fs::path custom = fs::path(home_dir()) / ".config/spinelos/themes" / (id + ".conf");
        std::ifstream ifs(custom);
        std::string line;
        while (std::getline(ifs, line)) {
            line = trim_copy(line);
            if (line.rfind("is_light=", 0) == 0) {
                std::string v = trim_copy(line.substr(9));
                is_light = (v == "true" || v == "1");
                break;
            }
        }
    }

    TermTheme t;
    t.id = id;
    t.is_light = is_light;

    if (is_light) {
        t.bar_bg = "#eef2f8";
        t.bar_border = "rgba(15, 23, 42, 0.12)";
        t.tab_bg = "rgba(15, 23, 42, 0.05)";
        t.tab_active_bg = "#ffffff";
        t.tab_fg = "#0f172a";
        t.tab_muted = "#64748b";
        t.accent = "#0a84ff";
        t.term_bg = make_rgba(0xFAFBFD);
        t.term_fg = make_rgba(0x1E293B);
        t.palette = {
            make_rgba(0x1E293B), make_rgba(0xDC2626), make_rgba(0x16A34A), make_rgba(0xCA8A04),
            make_rgba(0x2563EB), make_rgba(0x9333EA), make_rgba(0x0891B2), make_rgba(0x64748B),
            make_rgba(0x475569), make_rgba(0xEF4444), make_rgba(0x22C55E), make_rgba(0xEAB308),
            make_rgba(0x3B82F6), make_rgba(0xA855F7), make_rgba(0x06B6D4), make_rgba(0x0F172A)
        };
    } else if (id == "emerald-nordic") {
        t.bar_bg = "#071815";
        t.bar_border = "rgba(167, 243, 208, 0.14)";
        t.tab_bg = "rgba(255, 255, 255, 0.05)";
        t.tab_active_bg = "#0d2923";
        t.tab_fg = "#ecfdf5";
        t.tab_muted = "#6ee7b7";
        t.accent = "#10b981";
        t.term_bg = make_rgba(0x071411);
        t.term_fg = make_rgba(0xECFDF5);
        t.palette = {
            make_rgba(0x132A24), make_rgba(0xF87171), make_rgba(0x34D399), make_rgba(0xFBBF24),
            make_rgba(0x38BDF8), make_rgba(0xC084FC), make_rgba(0x2DD4BF), make_rgba(0xCBD5E1),
            make_rgba(0x4B6B62), make_rgba(0xFCA5A5), make_rgba(0x6EE7B7), make_rgba(0xFDE68A),
            make_rgba(0x7DD3FC), make_rgba(0xD8B4FE), make_rgba(0x5EEAD4), make_rgba(0xF8FAFC)
        };
    } else if (id == "obsidian-neon") {
        t.bar_bg = "#120c22";
        t.bar_border = "rgba(233, 213, 255, 0.14)";
        t.tab_bg = "rgba(255, 255, 255, 0.05)";
        t.tab_active_bg = "#1e1638";
        t.tab_fg = "#f5f3ff";
        t.tab_muted = "#c4b5fd";
        t.accent = "#a855f7";
        t.term_bg = make_rgba(0x0D0919);
        t.term_fg = make_rgba(0xF5F3FF);
        t.palette = {
            make_rgba(0x22193D), make_rgba(0xFB7185), make_rgba(0x4ADE80), make_rgba(0xFACC15),
            make_rgba(0x60A5FA), make_rgba(0xC084FC), make_rgba(0x22D3EE), make_rgba(0xE2E8F0),
            make_rgba(0x5B4B8A), make_rgba(0xFDA4AF), make_rgba(0x86EFAC), make_rgba(0xFDE047),
            make_rgba(0x93C5FD), make_rgba(0xE9D5FF), make_rgba(0x67E8F9), make_rgba(0xFFFFFF)
        };
    } else {
        t.bar_bg = "#0c1426";
        t.bar_border = "rgba(255, 255, 255, 0.12)";
        t.tab_bg = "rgba(255, 255, 255, 0.06)";
        t.tab_active_bg = "#15233e";
        t.tab_fg = "#f1f5f9";
        t.tab_muted = "#94a3b8";
        t.accent = "#38bdf8";
        t.term_bg = make_rgba(0x0A111F);
        t.term_fg = make_rgba(0xF1F5F9);
        t.palette = {
            make_rgba(0x1E293B), make_rgba(0xF87171), make_rgba(0x4ADE80), make_rgba(0xFBBF24),
            make_rgba(0x60A5FA), make_rgba(0xC084FC), make_rgba(0x22D3EE), make_rgba(0xCBD5E1),
            make_rgba(0x475569), make_rgba(0xFCA5A5), make_rgba(0x86EFAC), make_rgba(0xFDE68A),
            make_rgba(0x93C5FD), make_rgba(0xD8B4FE), make_rgba(0x67E8F9), make_rgba(0xF8FAFC)
        };
    }
    return t;
}

struct TermWin;

struct TermTab {
    uint64_t id = 0;
    TermWin *win = nullptr;
    GtkWidget *page = nullptr;
    VteTerminal *vte = nullptr;
    GtkWidget *tab_btn = nullptr;
    GtkWidget *tab_title_lbl = nullptr;
    GPid child_pid = -1;
    std::string title = "终端";
    bool closing = false;
};

struct TermWin {
    GtkApplication *app = nullptr;
    GtkWindow *window = nullptr;
    GtkWidget *root = nullptr;
    GtkWidget *tab_bar = nullptr;
    GtkWidget *tab_strip = nullptr;
    GtkWidget *zoom_btn = nullptr;
    GtkWidget *search_bar = nullptr;
    GtkWidget *search_entry = nullptr;
    GtkWidget *stack = nullptr;
    GtkWidget *ctx_popover = nullptr;
    GtkCssProvider *css = nullptr;

    std::vector<std::unique_ptr<TermTab>> tabs;
    uint64_t next_tab_id = 1;
    uint64_t active_tab_id = 0;
    double zoom = 1.0;
    bool search_visible = false;
    TermTheme theme;
    guint theme_timer = 0;
};

TermTab *find_tab(TermWin *w, uint64_t id) {
    if (!w) return nullptr;
    for (auto &t : w->tabs) {
        if (t && t->id == id) return t.get();
    }
    return nullptr;
}

TermTab *active_tab(TermWin *w) {
    return find_tab(w, w->active_tab_id);
}

std::string get_tab_cwd(const TermTab *t) {
    if (t && t->vte) {
        GUri *uri = vte_terminal_ref_termprop_uri_by_id(t->vte, VTE_PROPERTY_ID_CURRENT_DIRECTORY_URI);
        if (uri) {
            const char *path = g_uri_get_path(uri);
            std::string res = path ? path : "";
            g_uri_unref(uri);
            std::error_code ec;
            if (!res.empty() && fs::is_directory(res, ec)) return res;
        }
        if (t->child_pid > 0) {
            std::error_code ec;
            fs::path proc_cwd = fs::path("/proc") / std::to_string(t->child_pid) / "cwd";
            fs::path resolved = fs::read_symlink(proc_cwd, ec);
            if (!ec && fs::is_directory(resolved, ec)) return resolved.string();
        }
    }
    std::string h = home_dir();
    return h.empty() ? "/" : h;
}

void apply_css_theme(TermWin *w) {
    if (!w) return;
    const auto &t = w->theme;
    std::string css_str =
        ".spinel-term-win {\n"
        "  background-color: " + t.bar_bg + ";\n"
        "}\n"
        ".spinel-term-tabbar {\n"
        "  background-color: " + t.bar_bg + ";\n"
        "  border-bottom: 1px solid " + t.bar_border + ";\n"
        "  padding: 6px 8px;\n"
        "}\n"
        ".spinel-term-tab {\n"
        "  background-color: " + t.tab_bg + ";\n"
        "  background-image: none;\n"
        "  border: 1px solid transparent;\n"
        "  border-radius: 8px;\n"
        "  box-shadow: none;\n"
        "  padding: 2px 6px 2px 10px;\n"
        "  min-height: 28px;\n"
        "  color: " + t.tab_muted + ";\n"
        "}\n"
        ".spinel-term-tab:hover {\n"
        "  border-color: " + t.bar_border + ";\n"
        "  color: " + t.tab_fg + ";\n"
        "}\n"
        ".spinel-term-tab.active {\n"
        "  background-color: " + t.tab_active_bg + ";\n"
        "  border-color: " + t.bar_border + ";\n"
        "  color: " + t.tab_fg + ";\n"
        "}\n"
        ".spinel-term-tab-btn {\n"
        "  background: transparent;\n"
        "  background-image: none;\n"
        "  border: none;\n"
        "  box-shadow: none;\n"
        "  padding: 0 4px;\n"
        "  min-height: 24px;\n"
        "  color: inherit;\n"
        "  font-size: 12px;\n"
        "  font-weight: 600;\n"
        "}\n"
        ".spinel-term-tab-close {\n"
        "  background: transparent;\n"
        "  background-image: none;\n"
        "  border: none;\n"
        "  box-shadow: none;\n"
        "  border-radius: 5px;\n"
        "  min-width: 18px;\n"
        "  min-height: 18px;\n"
        "  padding: 0;\n"
        "  color: " + t.tab_muted + ";\n"
        "  font-size: 11px;\n"
        "}\n"
        ".spinel-term-tab-close:hover {\n"
        "  background-color: rgba(239, 68, 68, 0.22);\n"
        "  color: #ef4444;\n"
        "}\n"
        ".spinel-term-tool {\n"
        "  background-color: " + t.tab_bg + ";\n"
        "  background-image: none;\n"
        "  border: 1px solid " + t.bar_border + ";\n"
        "  border-radius: 8px;\n"
        "  box-shadow: none;\n"
        "  min-width: 28px;\n"
        "  min-height: 28px;\n"
        "  padding: 0 8px;\n"
        "  color: " + t.tab_fg + ";\n"
        "  font-size: 12px;\n"
        "  font-weight: 600;\n"
        "}\n"
        ".spinel-term-tool:hover {\n"
        "  border-color: " + t.accent + ";\n"
        "}\n"
        ".spinel-term-searchbar {\n"
        "  background-color: " + t.tab_active_bg + ";\n"
        "  border-bottom: 1px solid " + t.bar_border + ";\n"
        "  padding: 6px 10px;\n"
        "}\n"
        ".spinel-term-ctx contents {\n"
        "  background-color: " + t.tab_active_bg + ";\n"
        "  border: 1px solid " + t.bar_border + ";\n"
        "  border-radius: 10px;\n"
        "  padding: 6px;\n"
        "  box-shadow: 0 12px 28px rgba(0, 0, 0, 0.35);\n"
        "}\n"
        ".spinel-term-ctx-item {\n"
        "  background: transparent;\n"
        "  background-image: none;\n"
        "  border: none;\n"
        "  box-shadow: none;\n"
        "  color: " + t.tab_fg + ";\n"
        "  font-size: 12.5px;\n"
        "  min-width: 170px;\n"
        "  padding: 6px 10px;\n"
        "  border-radius: 7px;\n"
        "}\n"
        ".spinel-term-ctx-item:hover {\n"
        "  background-color: " + t.tab_bg + ";\n"
        "}\n";

    GdkDisplay *display = gdk_display_get_default();
    if (!display) return;
    if (!w->css) {
        w->css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            display, GTK_STYLE_PROVIDER(w->css), GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    gtk_css_provider_load_from_string(w->css, css_str.c_str());

    for (auto &tptr : w->tabs) {
        if (tptr && tptr->vte) {
            vte_terminal_set_colors(tptr->vte, &t.term_fg, &t.term_bg, t.palette.data(), t.palette.size());
        }
    }
}

void update_window_title_and_tabs(TermWin *w) {
    if (!w) return;
    for (auto &t : w->tabs) {
        if (!t || !t->tab_btn) continue;
        if (t->id == w->active_tab_id) gtk_widget_add_css_class(t->tab_btn, "active");
        else gtk_widget_remove_css_class(t->tab_btn, "active");
    }
    if (TermTab *cur = active_tab(w)) {
        std::string wt = cur->title.empty() ? "Spinel 终端" : (cur->title + " — Spinel 终端");
        gtk_window_set_title(w->window, wt.c_str());
    } else {
        gtk_window_set_title(w->window, "Spinel 终端");
    }
}

void select_tab(TermWin *w, uint64_t tab_id) {
    TermTab *tab = find_tab(w, tab_id);
    if (!w || !tab || tab->closing) return;
    w->active_tab_id = tab_id;
    std::string page_name = "tab-" + std::to_string(tab_id);
    gtk_stack_set_visible_child_name(GTK_STACK(w->stack), page_name.c_str());
    update_window_title_and_tabs(w);
    if (tab->vte) gtk_widget_grab_focus(GTK_WIDGET(tab->vte));
}

void close_tab(TermWin *w, uint64_t tab_id) {
    if (!w) return;
    auto it = std::find_if(w->tabs.begin(), w->tabs.end(), [tab_id](const std::unique_ptr<TermTab> &t) {
        return t && t->id == tab_id;
    });
    if (it == w->tabs.end()) return;
    TermTab *tab = it->get();
    if (tab->closing) return;
    tab->closing = true;

    std::size_t idx = static_cast<std::size_t>(std::distance(w->tabs.begin(), it));
    bool was_active = (w->active_tab_id == tab_id);

    if (tab->tab_btn && w->tab_strip) {
        gtk_box_remove(GTK_BOX(w->tab_strip), tab->tab_btn);
        tab->tab_btn = nullptr;
    }
    if (tab->page && w->stack) {
        if (tab->vte) {
            g_signal_handlers_disconnect_by_data(tab->vte, g_object_get_data(G_OBJECT(tab->vte), "vte-exit-ref"));
            g_signal_handlers_disconnect_by_data(tab->vte, g_object_get_data(G_OBJECT(tab->vte), "vte-prop-ref"));
            g_signal_handlers_disconnect_by_data(tab->vte, g_object_get_data(G_OBJECT(tab->vte), "vte-rclick-ref"));
        }
        gtk_stack_remove(GTK_STACK(w->stack), tab->page);
        tab->page = nullptr;
        tab->vte = nullptr;
    }
    w->tabs.erase(it);

    if (w->tabs.empty()) {
        gtk_window_close(w->window);
        return;
    }

    if (was_active) {
        std::size_t next_idx = std::min(idx, w->tabs.size() - 1);
        select_tab(w, w->tabs[next_idx]->id);
    } else {
        update_window_title_and_tabs(w);
    }
}

void set_zoom(TermWin *w, double zoom) {
    if (!w) return;
    w->zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    for (auto &t : w->tabs) {
        if (t && t->vte) vte_terminal_set_font_scale(t->vte, w->zoom);
    }
    if (w->zoom_btn) {
        int pct = static_cast<int>(std::round(w->zoom * 100.0));
        std::string s = std::to_string(pct) + "%";
        gtk_button_set_label(GTK_BUTTON(w->zoom_btn), s.c_str());
    }
}

void run_search(TermWin *w, bool forward) {
    if (!w || !w->search_entry) return;
    TermTab *tab = active_tab(w);
    if (!tab || !tab->vte) return;

    const char *q = gtk_editable_get_text(GTK_EDITABLE(w->search_entry));
    if (!q || !*q) {
        vte_terminal_search_set_regex(tab->vte, nullptr, 0);
        return;
    }

    gchar *escaped = g_regex_escape_string(q, -1);
    GError *err = nullptr;
    constexpr guint32 kPcre2Caseless = 0x00000008u;
    VteRegex *rx = vte_regex_new_for_search(escaped, -1, VTE_REGEX_FLAGS_DEFAULT | kPcre2Caseless, &err);
    g_free(escaped);
    if (err) g_error_free(err);

    if (rx) {
        vte_terminal_search_set_regex(tab->vte, rx, 0);
        vte_terminal_search_set_wrap_around(tab->vte, TRUE);
        vte_regex_unref(rx);
        if (forward) vte_terminal_search_find_next(tab->vte);
        else vte_terminal_search_find_previous(tab->vte);
    }
}

void toggle_search(TermWin *w, bool show) {
    if (!w || !w->search_bar) return;
    w->search_visible = show;
    gtk_widget_set_visible(w->search_bar, show ? TRUE : FALSE);
    if (show) {
        gtk_widget_grab_focus(w->search_entry);
        gtk_editable_select_region(GTK_EDITABLE(w->search_entry), 0, -1);
    } else if (TermTab *t = active_tab(w); t && t->vte) {
        gtk_widget_grab_focus(GTK_WIDGET(t->vte));
    }
}

struct TabRef {
    TermWin *win = nullptr;
    uint64_t tab_id = 0;
};

void free_tab_ref(gpointer p) {
    delete static_cast<TabRef *>(p);
}

void on_tab_select_clicked(GtkButton *btn, gpointer) {
    auto *ref = static_cast<TabRef *>(g_object_get_data(G_OBJECT(btn), "tab-ref"));
    if (ref) select_tab(ref->win, ref->tab_id);
}

void on_tab_close_clicked(GtkButton *btn, gpointer) {
    auto *ref = static_cast<TabRef *>(g_object_get_data(G_OBJECT(btn), "tab-ref"));
    if (ref) close_tab(ref->win, ref->tab_id);
}

void on_vte_spawned(VteTerminal *, GPid pid, GError *err, gpointer user_data) {
    std::unique_ptr<TabRef> ref(static_cast<TabRef *>(user_data));
    TermTab *tab = find_tab(ref->win, ref->tab_id);
    if (!tab || tab->closing) return;
    if (err) {
        std::string msg = std::string("\r\n[Spinel 终端] 启动命令失败: ") + err->message + "\r\n";
        vte_terminal_feed(tab->vte, msg.c_str(), static_cast<gssize>(msg.size()));
        return;
    }
    tab->child_pid = pid;
}

void on_vte_child_exited(VteTerminal *, gint, gpointer user_data) {
    auto *ref = static_cast<TabRef *>(user_data);
    if (ref) close_tab(ref->win, ref->tab_id);
}

void on_vte_termprop_changed(VteTerminal *vte, const char *prop_name, gpointer user_data) {
    auto *ref = static_cast<TabRef *>(user_data);
    if (!ref || !prop_name) return;
    if (std::strcmp(prop_name, VTE_TERMPROP_XTERM_TITLE) != 0) return;
    TermTab *tab = find_tab(ref->win, ref->tab_id);
    if (!tab || tab->closing) return;
    const char *t = vte_terminal_get_termprop_string_by_id(vte, VTE_PROPERTY_ID_XTERM_TITLE, nullptr);
    tab->title = (t && *t) ? t : "终端";
    if (tab->tab_title_lbl) gtk_label_set_text(GTK_LABEL(tab->tab_title_lbl), tab->title.c_str());
    if (tab->id == ref->win->active_tab_id) update_window_title_and_tabs(ref->win);
}

void on_vte_ctx_action(GtkButton *btn, gpointer user_data) {
    auto *w = static_cast<TermWin *>(user_data);
    if (w->ctx_popover) gtk_popover_popdown(GTK_POPOVER(w->ctx_popover));
    const char *act = static_cast<const char *>(g_object_get_data(G_OBJECT(btn), "term-action"));
    if (!act) return;
    TermTab *tab = active_tab(w);
    std::string a(act);
    if (a == "copy" && tab && tab->vte) {
        vte_terminal_copy_clipboard_format(tab->vte, VTE_FORMAT_TEXT);
    } else if (a == "paste" && tab && tab->vte) {
        vte_terminal_paste_clipboard(tab->vte);
    } else if (a == "select-all" && tab && tab->vte) {
        vte_terminal_select_all(tab->vte);
    } else if (a == "clear" && tab && tab->vte) {
        vte_terminal_reset(tab->vte, FALSE, TRUE);
    } else if (a == "search") {
        toggle_search(w, true);
    } else if (a == "zoom-reset") {
        set_zoom(w, 1.0);
    } else if (a == "close-tab" && tab) {
        close_tab(w, tab->id);
    }
    if (a != "search" && a != "close-tab" && tab && tab->vte) {
        gtk_widget_grab_focus(GTK_WIDGET(tab->vte));
    }
}

void add_tab(TermWin *w, const std::string &cwd, const std::vector<std::string> &cmd_argv = {});

void on_vte_ctx_new_tab(GtkButton *, gpointer user_data) {
    auto *w = static_cast<TermWin *>(user_data);
    if (w->ctx_popover) gtk_popover_popdown(GTK_POPOVER(w->ctx_popover));
    add_tab(w, get_tab_cwd(active_tab(w)));
}

void on_vte_right_click(GtkGestureClick *gesture, int, double x, double y, gpointer user_data) {
    auto *ref = static_cast<TabRef *>(user_data);
    if (!ref || !ref->win) return;
    TermWin *w = ref->win;
    TermTab *tab = find_tab(w, ref->tab_id);
    if (!tab || !tab->vte || !w->stack) return;

    if (!w->ctx_popover) {
        GtkWidget *pop = gtk_popover_new();
        gtk_widget_set_parent(pop, w->stack);
        gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
        gtk_popover_set_autohide(GTK_POPOVER(pop), TRUE);
        gtk_widget_add_css_class(pop, "spinel-term-ctx");

        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        auto add_btn = [&](const char *label, const char *act, GCallback cb) {
            GtkWidget *b = gtk_button_new_with_label(label);
            gtk_widget_add_css_class(b, "spinel-term-ctx-item");
            GtkWidget *lbl = gtk_button_get_child(GTK_BUTTON(b));
            if (GTK_IS_LABEL(lbl)) gtk_widget_set_halign(lbl, GTK_ALIGN_START);
            if (act) g_object_set_data_full(G_OBJECT(b), "term-action", g_strdup(act), g_free);
            g_signal_connect(b, "clicked", cb, w);
            gtk_box_append(GTK_BOX(box), b);
        };
        add_btn("复制  (Ctrl+Shift+C)", "copy", G_CALLBACK(on_vte_ctx_action));
        add_btn("粘贴  (Ctrl+Shift+V)", "paste", G_CALLBACK(on_vte_ctx_action));
        add_btn("全选  (Ctrl+Shift+A)", "select-all", G_CALLBACK(on_vte_ctx_action));
        add_btn("清空缓冲", "clear", G_CALLBACK(on_vte_ctx_action));
        gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
        add_btn("新建标签页  (Ctrl+T)", nullptr, G_CALLBACK(on_vte_ctx_new_tab));
        add_btn("查找内容  (Ctrl+F)", "search", G_CALLBACK(on_vte_ctx_action));
        add_btn("重置缩放  (Ctrl+0)", "zoom-reset", G_CALLBACK(on_vte_ctx_action));
        gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
        add_btn("关闭当前标签页  (Ctrl+W)", "close-tab", G_CALLBACK(on_vte_ctx_action));

        gtk_popover_set_child(GTK_POPOVER(pop), box);
        w->ctx_popover = pop;
    }

    double tx = x, ty = y;
    graphene_point_t src{static_cast<float>(x), static_cast<float>(y)};
    graphene_point_t dst{};
    if (gtk_widget_compute_point(GTK_WIDGET(tab->vte), w->stack, &src, &dst)) {
        tx = dst.x;
        ty = dst.y;
    }
    (void)gesture;
    GdkRectangle rect{static_cast<int>(tx), static_cast<int>(ty), 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(w->ctx_popover), &rect);
    gtk_popover_popup(GTK_POPOVER(w->ctx_popover));
}

void add_tab(TermWin *w, const std::string &cwd, const std::vector<std::string> &cmd_argv) {
    if (!w) return;
    auto tab = std::make_unique<TermTab>();
    tab->id = w->next_tab_id++;
    tab->win = w;

    // Terminal widget + scrollbar inside plain GtkBox (avoids offscreen GPU glitches on close)
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *vte_w = vte_terminal_new();
    VteTerminal *vte = VTE_TERMINAL(vte_w);
    gtk_widget_set_hexpand(vte_w, TRUE);
    gtk_widget_set_vexpand(vte_w, TRUE);

    PangoFontDescription *font_desc = pango_font_description_from_string("Noto Sans Mono CJK SC 11");
    vte_terminal_set_font(vte, font_desc);
    pango_font_description_free(font_desc);

    vte_terminal_set_font_scale(vte, w->zoom);
    vte_terminal_set_scrollback_lines(vte, 10000);
    vte_terminal_set_scroll_on_output(vte, FALSE);
    vte_terminal_set_scroll_on_keystroke(vte, TRUE);
    vte_terminal_set_mouse_autohide(vte, TRUE);
    vte_terminal_set_allow_hyperlink(vte, TRUE);
    vte_terminal_set_bold_is_bright(vte, TRUE);
    vte_terminal_set_cursor_blink_mode(vte, VTE_CURSOR_BLINK_ON);
    vte_terminal_set_colors(vte, &w->theme.term_fg, &w->theme.term_bg,
                            w->theme.palette.data(), w->theme.palette.size());

    GtkAdjustment *vadj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte_w));
    GtkWidget *sb = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, vadj);

    gtk_box_append(GTK_BOX(page), vte_w);
    gtk_box_append(GTK_BOX(page), sb);

    std::string page_name = "tab-" + std::to_string(tab->id);
    gtk_stack_add_named(GTK_STACK(w->stack), page, page_name.c_str());

    // Tab pill button in top strip
    GtkWidget *pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(pill, "spinel-term-tab");

    GtkWidget *sel_btn = gtk_button_new();
    gtk_widget_add_css_class(sel_btn, "spinel-term-tab-btn");
    GtkWidget *title_lbl = gtk_label_new(tab->title.c_str());
    gtk_label_set_ellipsize(GTK_LABEL(title_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(title_lbl), 22);
    gtk_label_set_width_chars(GTK_LABEL(title_lbl), 10);
    gtk_button_set_child(GTK_BUTTON(sel_btn), title_lbl);
    g_object_set_data_full(G_OBJECT(sel_btn), "tab-ref", new TabRef{w, tab->id}, free_tab_ref);
    g_signal_connect(sel_btn, "clicked", G_CALLBACK(on_tab_select_clicked), nullptr);

    GtkWidget *cls_btn = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(cls_btn, "spinel-term-tab-close");
    g_object_set_data_full(G_OBJECT(cls_btn), "tab-ref", new TabRef{w, tab->id}, free_tab_ref);
    g_signal_connect(cls_btn, "clicked", G_CALLBACK(on_tab_close_clicked), nullptr);

    gtk_box_append(GTK_BOX(pill), sel_btn);
    gtk_box_append(GTK_BOX(pill), cls_btn);
    gtk_box_append(GTK_BOX(w->tab_strip), pill);

    tab->page = page;
    tab->vte = vte;
    tab->tab_btn = pill;
    tab->tab_title_lbl = title_lbl;

    // Right click menu
    GtkGesture *rclick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), GDK_BUTTON_SECONDARY);
    auto *rref = new TabRef{w, tab->id};
    g_object_set_data_full(G_OBJECT(vte_w), "vte-rclick-ref", rref, free_tab_ref);
    g_signal_connect(rclick, "pressed", G_CALLBACK(on_vte_right_click), rref);
    gtk_widget_add_controller(vte_w, GTK_EVENT_CONTROLLER(rclick));

    // Lifecycle signals
    auto *exit_ref = new TabRef{w, tab->id};
    g_object_set_data_full(G_OBJECT(vte_w), "vte-exit-ref", exit_ref, free_tab_ref);
    g_signal_connect(vte_w, "child-exited", G_CALLBACK(on_vte_child_exited), exit_ref);

    auto *prop_ref = new TabRef{w, tab->id};
    g_object_set_data_full(G_OBJECT(vte_w), "vte-prop-ref", prop_ref, free_tab_ref);
    g_signal_connect(vte_w, "termprop-changed", G_CALLBACK(on_vte_termprop_changed), prop_ref);

    // Spawn shell or requested command
    std::vector<std::string> argv_storage = cmd_argv;
    if (argv_storage.empty()) {
        const char *env_shell = std::getenv("SHELL");
        std::string sh = (env_shell && *env_shell) ? env_shell : "/bin/bash";
        argv_storage.push_back(sh);
    }
    std::vector<char *> argv_c;
    for (auto &s : argv_storage) argv_c.push_back(const_cast<char *>(s.c_str()));
    argv_c.push_back(nullptr);

    char **envp = g_get_environ();
    envp = g_environ_setenv(envp, "TERM", "xterm-256color", TRUE);
    envp = g_environ_setenv(envp, "COLORTERM", "truecolor", TRUE);

    uint64_t tid = tab->id;
    w->tabs.push_back(std::move(tab));
    select_tab(w, tid);

    const char *work_dir = cwd.empty() ? home_dir().c_str() : cwd.c_str();
    vte_terminal_spawn_async(
        vte,
        VTE_PTY_DEFAULT,
        work_dir,
        argv_c.data(),
        envp,
        G_SPAWN_SEARCH_PATH,
        nullptr, nullptr, nullptr,
        -1,
        nullptr,
        on_vte_spawned,
        new TabRef{w, tid}
    );
    g_strfreev(envp);
}

gboolean on_win_key_pressed(GtkEventControllerKey *, guint keyval, guint, GdkModifierType state, gpointer user_data) {
    auto *w = static_cast<TermWin *>(user_data);
    if (!w) return FALSE;

    bool ctrl = (state & GDK_CONTROL_MASK) != 0;
    bool shift = (state & GDK_SHIFT_MASK) != 0;
    bool alt = (state & GDK_ALT_MASK) != 0;

    if (w->search_visible && keyval == GDK_KEY_Escape) {
        toggle_search(w, false);
        return TRUE;
    }

    if (alt && !ctrl && keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9) {
        std::size_t idx = static_cast<std::size_t>(keyval - GDK_KEY_1);
        if (idx < w->tabs.size()) {
            select_tab(w, w->tabs[idx]->id);
            return TRUE;
        }
    }

    if (!ctrl) return FALSE;

    // New tab: Ctrl+T / Ctrl+Shift+T
    if (keyval == GDK_KEY_t || keyval == GDK_KEY_T) {
        add_tab(w, get_tab_cwd(active_tab(w)));
        return TRUE;
    }
    // Close tab: Ctrl+W / Ctrl+Shift+W
    if (keyval == GDK_KEY_w || keyval == GDK_KEY_W) {
        if (TermTab *cur = active_tab(w)) close_tab(w, cur->id);
        return TRUE;
    }
    // Switch tabs: Ctrl+Tab / Ctrl+Shift+Tab / Ctrl+PageUp / Ctrl+PageDown
    if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab ||
        keyval == GDK_KEY_Page_Down || keyval == GDK_KEY_Page_Up) {
        if (w->tabs.size() > 1) {
            auto it = std::find_if(w->tabs.begin(), w->tabs.end(), [w](const std::unique_ptr<TermTab> &t) {
                return t && t->id == w->active_tab_id;
            });
            std::size_t idx = (it == w->tabs.end()) ? 0 : static_cast<std::size_t>(std::distance(w->tabs.begin(), it));
            bool prev = shift || keyval == GDK_KEY_ISO_Left_Tab || keyval == GDK_KEY_Page_Up;
            std::size_t next_idx = prev
                ? (idx == 0 ? w->tabs.size() - 1 : idx - 1)
                : ((idx + 1) % w->tabs.size());
            select_tab(w, w->tabs[next_idx]->id);
        }
        return TRUE;
    }
    // Copy / Paste / Select All
    if (shift && (keyval == GDK_KEY_C || keyval == GDK_KEY_c)) {
        if (TermTab *cur = active_tab(w); cur && cur->vte) {
            vte_terminal_copy_clipboard_format(cur->vte, VTE_FORMAT_TEXT);
        }
        return TRUE;
    }
    if (shift && (keyval == GDK_KEY_V || keyval == GDK_KEY_v)) {
        if (TermTab *cur = active_tab(w); cur && cur->vte) {
            vte_terminal_paste_clipboard(cur->vte);
        }
        return TRUE;
    }
    if (shift && (keyval == GDK_KEY_A || keyval == GDK_KEY_a)) {
        if (TermTab *cur = active_tab(w); cur && cur->vte) {
            vte_terminal_select_all(cur->vte);
        }
        return TRUE;
    }
    if (shift && (keyval == GDK_KEY_N || keyval == GDK_KEY_n)) {
        spawn_argv_detached({"spinel-terminal"});
        return TRUE;
    }
    // Find: Ctrl+F / Ctrl+Shift+F
    if (keyval == GDK_KEY_f || keyval == GDK_KEY_F) {
        toggle_search(w, !w->search_visible || !gtk_widget_has_focus(w->search_entry));
        return TRUE;
    }
    // Zoom: Ctrl+Plus/Equal, Ctrl+Minus, Ctrl+0
    if (keyval == GDK_KEY_plus || keyval == GDK_KEY_equal || keyval == GDK_KEY_KP_Add) {
        set_zoom(w, w->zoom + 0.1);
        return TRUE;
    }
    if (keyval == GDK_KEY_minus || keyval == GDK_KEY_underscore || keyval == GDK_KEY_KP_Subtract) {
        set_zoom(w, w->zoom - 0.1);
        return TRUE;
    }
    if (keyval == GDK_KEY_0 || keyval == GDK_KEY_KP_0) {
        set_zoom(w, 1.0);
        return TRUE;
    }
    return FALSE;
}

gboolean on_theme_poll(gpointer user_data) {
    auto *w = static_cast<TermWin *>(user_data);
    if (!w) return G_SOURCE_REMOVE;
    TermTheme latest = load_spinel_term_theme();
    if (latest.id != w->theme.id || latest.is_light != w->theme.is_light) {
        w->theme = latest;
        apply_css_theme(w);
    }
    return G_SOURCE_CONTINUE;
}

struct LaunchOptions {
    std::string cwd;
    std::string title;
    std::vector<std::string> argv;
};

void build_terminal_window(GtkApplication *app, const LaunchOptions &opts) {
    auto *w = new TermWin();
    w->app = app;
    w->theme = load_spinel_term_theme();

    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(win, opts.title.empty() ? "Spinel 终端" : opts.title.c_str());
    gtk_window_set_default_size(win, 920, 580);
    gtk_widget_add_css_class(GTK_WIDGET(win), "spinel-term-win");
    w->window = win;

    w->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // Top Tab Bar
    w->tab_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(w->tab_bar, "spinel-term-tabbar");

    GtkWidget *scroll_tabs = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_tabs), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_widget_set_hexpand(scroll_tabs, TRUE);
    w->tab_strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_tabs), w->tab_strip);

    GtkWidget *add_btn = gtk_button_new_with_label("+");
    gtk_widget_add_css_class(add_btn, "spinel-term-tool");
    gtk_widget_set_tooltip_text(add_btn, "新建标签页 (Ctrl+T)");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
        auto *tw = static_cast<TermWin *>(data);
        add_tab(tw, get_tab_cwd(active_tab(tw)));
    }), w);

    GtkWidget *find_btn = gtk_button_new_with_label("查找");
    gtk_widget_add_css_class(find_btn, "spinel-term-tool");
    gtk_widget_set_tooltip_text(find_btn, "查找内容 (Ctrl+F)");
    g_signal_connect(find_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
        auto *tw = static_cast<TermWin *>(data);
        toggle_search(tw, !tw->search_visible);
    }), w);

    w->zoom_btn = gtk_button_new_with_label("100%");
    gtk_widget_add_css_class(w->zoom_btn, "spinel-term-tool");
    gtk_widget_set_tooltip_text(w->zoom_btn, "点击重置字体缩放 (Ctrl+0)");
    g_signal_connect(w->zoom_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
        set_zoom(static_cast<TermWin *>(data), 1.0);
    }), w);

    gtk_box_append(GTK_BOX(w->tab_bar), scroll_tabs);
    gtk_box_append(GTK_BOX(w->tab_bar), add_btn);
    gtk_box_append(GTK_BOX(w->tab_bar), find_btn);
    gtk_box_append(GTK_BOX(w->tab_bar), w->zoom_btn);

    // Inline Search Bar
    w->search_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(w->search_bar, "spinel-term-searchbar");
    gtk_widget_set_visible(w->search_bar, FALSE);
    w->search_entry = gtk_search_entry_new();
    gtk_widget_set_hexpand(w->search_entry, TRUE);
    g_signal_connect(w->search_entry, "activate", G_CALLBACK(+[](GtkSearchEntry *, gpointer data) {
        run_search(static_cast<TermWin *>(data), true);
    }), w);
    g_signal_connect(w->search_entry, "search-changed", G_CALLBACK(+[](GtkSearchEntry *, gpointer data) {
        run_search(static_cast<TermWin *>(data), false);
    }), w);

    GtkWidget *prev_btn = gtk_button_new_with_label("上一个");
    gtk_widget_add_css_class(prev_btn, "spinel-term-tool");
    g_signal_connect(prev_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
        run_search(static_cast<TermWin *>(data), false);
    }), w);

    GtkWidget *next_btn = gtk_button_new_with_label("下一个");
    gtk_widget_add_css_class(next_btn, "spinel-term-tool");
    g_signal_connect(next_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
        run_search(static_cast<TermWin *>(data), true);
    }), w);

    GtkWidget *close_search_btn = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(close_search_btn, "spinel-term-tool");
    g_signal_connect(close_search_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
        toggle_search(static_cast<TermWin *>(data), false);
    }), w);

    gtk_box_append(GTK_BOX(w->search_bar), w->search_entry);
    gtk_box_append(GTK_BOX(w->search_bar), prev_btn);
    gtk_box_append(GTK_BOX(w->search_bar), next_btn);
    gtk_box_append(GTK_BOX(w->search_bar), close_search_btn);

    // Content stack (transition=NONE guarantees clean redraw on tab add/close)
    w->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(w->stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_widget_set_hexpand(w->stack, TRUE);
    gtk_widget_set_vexpand(w->stack, TRUE);

    gtk_box_append(GTK_BOX(w->root), w->tab_bar);
    gtk_box_append(GTK_BOX(w->root), w->search_bar);
    gtk_box_append(GTK_BOX(w->root), w->stack);
    gtk_window_set_child(win, w->root);

    apply_css_theme(w);

    GtkEventController *key_ctl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctl, "key-pressed", G_CALLBACK(on_win_key_pressed), w);
    gtk_widget_add_controller(GTK_WIDGET(win), key_ctl);

    w->theme_timer = g_timeout_add(1200, on_theme_poll, w);
    g_signal_connect(win, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
        auto *tw = static_cast<TermWin *>(data);
        for (auto &t : tw->tabs) {
            if (t && t->vte) {
                t->closing = true;
                g_signal_handlers_disconnect_by_data(t->vte, g_object_get_data(G_OBJECT(t->vte), "vte-exit-ref"));
                g_signal_handlers_disconnect_by_data(t->vte, g_object_get_data(G_OBJECT(t->vte), "vte-prop-ref"));
            }
        }
        if (tw->theme_timer) g_source_remove(tw->theme_timer);
        if (tw->css) {
            if (GdkDisplay *d = gdk_display_get_default()) {
                gtk_style_context_remove_provider_for_display(d, GTK_STYLE_PROVIDER(tw->css));
            }
            g_object_unref(tw->css);
        }
        delete tw;
    }), w);

    add_tab(w, opts.cwd.empty() ? home_dir() : opts.cwd, opts.argv);
    gtk_window_present(win);
}

LaunchOptions parse_launch_args(int argc, char **argv) {
    LaunchOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "-d" || a == "--working-directory") && i + 1 < argc) {
            opts.cwd = argv[++i];
        } else if (a.rfind("--working-directory=", 0) == 0) {
            opts.cwd = a.substr(20);
        } else if ((a == "-T" || a == "--title") && i + 1 < argc) {
            opts.title = argv[++i];
        } else if (a == "--" || a == "-e" || a == "-x") {
            for (int j = i + 1; j < argc; ++j) opts.argv.emplace_back(argv[j]);
            break;
        }
    }
    return opts;
}

}  // namespace

int main(int argc, char **argv) {
    ensure_display_env();
    const char *wlr = std::getenv("WLR_RENDERER");
    if (wlr && std::string(wlr) == "pixman") {
        setenv("GSK_RENDERER", "cairo", 0);
    }

    LaunchOptions opts = parse_launch_args(argc, argv);
    GtkApplication *app = gtk_application_new("org.spinelos.terminal", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication *gapp, gpointer data) {
        build_terminal_window(gapp, *static_cast<LaunchOptions *>(data));
    }), &opts);
    int rc = g_application_run(G_APPLICATION(app), 0, nullptr);
    g_object_unref(app);
    return rc;
}
