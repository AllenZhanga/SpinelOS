#include "files_theme.hpp"
#include "common.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

struct FilesTheme {
    std::string id;
    bool is_light = false;
    std::string fg = "#f1f5f9";
    std::string muted = "#94a3b8";
    std::string accent = "#38bdf8";
    bool operator==(const FilesTheme &) const = default;
};

struct FilesThemeBinding {
    explicit FilesThemeBinding(GdkDisplay *owner)
        : display(GDK_DISPLAY(g_object_ref(owner))), css(gtk_css_provider_new()) {
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(css),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    ~FilesThemeBinding() {
        if (timer) g_source_remove(timer);
        gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(css));
        g_object_unref(css);
        g_object_unref(display);
    }

    GdkDisplay *display;
    GtkCssProvider *css;
    FilesTheme applied;
    guint timer = 0;
};

FilesTheme load_active_files_theme() {
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

    FilesTheme th;
    th.id = id;
    if (id == "tahoe-light") {
        th.is_light = true;
        th.fg = "#0f172a";
        th.muted = "#64748b";
        th.accent = "#0a84ff";
    } else if (id == "emerald-nordic") {
        th.fg = "#ecfdf5";
        th.muted = "#6ee7b7";
        th.accent = "#10b981";
    } else if (id == "obsidian-neon") {
        th.fg = "#f5f3ff";
        th.muted = "#c4b5fd";
        th.accent = "#a855f7";
    } else if (id != "crystal-aurora") {
        fs::path custom = fs::path(home_dir()) / ".config/spinelos/themes" / (id + ".theme");
        std::ifstream ifs(custom);
        std::string line;
        while (std::getline(ifs, line)) {
            line = trim_copy(line);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim_copy(line.substr(0, eq));
            std::string v = trim_copy(line.substr(eq + 1));
            if (k == "is_light") th.is_light = (v == "true" || v == "1");
            else if (k == "ui_text") th.fg = v;
            else if (k == "ui_muted") th.muted = v;
            else if (k == "ui_accent") th.accent = v;
        }
    }
    return th;
}

void sync_gtk_color_scheme(GdkDisplay *display, bool is_light) {
    GtkSettings *settings = gtk_settings_get_for_display(display);
#if GTK_CHECK_VERSION(4, 20, 0)
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(settings), "gtk-interface-color-scheme")) {
        g_object_set(settings, "gtk-interface-color-scheme",
                     is_light ? GTK_INTERFACE_COLOR_SCHEME_LIGHT : GTK_INTERFACE_COLOR_SCHEME_DARK, nullptr);
        return;
    }
#endif
    g_object_set(settings, "gtk-application-prefer-dark-theme", !is_light, nullptr);
}

void update_theme(FilesThemeBinding &binding) {
    FilesTheme th = load_active_files_theme();
    if (binding.applied == th) return;
    if (binding.applied.id.empty() || binding.applied.is_light != th.is_light) {
        sync_gtk_color_scheme(binding.display, th.is_light);
    }
    binding.applied = th;

    std::string win_bg = th.is_light ? "#f5f7fb" : "#0b111e";
    std::string bar_bg = th.is_light ? "#eef2f8" : "#11192b";
    std::string side_bg = th.is_light ? "#eaf0f8" : "#0e1627";
    std::string card_bg = th.is_light ? "#ffffff" : "#152038";
    std::string hover_bg = th.is_light ? "rgba(10, 132, 255, 0.08)" : "rgba(56, 189, 248, 0.12)";
    std::string sel_bg = th.is_light ? "rgba(10, 132, 255, 0.16)" : "rgba(56, 189, 248, 0.22)";
    std::string border = th.is_light ? "rgba(15, 23, 42, 0.10)" : "rgba(255, 255, 255, 0.10)";
    std::string fg = th.fg;
    std::string muted = th.muted;
    std::string accent = th.accent;
    const char *danger = th.is_light ? "#a91f3d" : "#fda4af";

    std::ostringstream css;
    css << "window.sf-win { background: " << win_bg << "; color: " << fg << "; }\n"
        << ".sf-toolbar { background: " << bar_bg << "; border-bottom: 1px solid " << border << "; padding: 8px 12px; }\n"
        << ".sf-sidebar { background: " << side_bg << "; border-right: 1px solid " << border << "; padding: 10px 8px; }\n"
        << ".sf-side-title { color: " << muted << "; font-size: 11px; font-weight: 700; padding: 6px 8px 4px 8px; }\n"
        << ".sf-nav-btn { background: transparent; color: " << fg << "; border: none; border-radius: 8px; padding: 6px 10px; font-size: 13px; }\n"
        << ".sf-nav-btn:hover { background: " << hover_bg << "; }\n"
        << ".sf-nav-btn.is-active { background: " << sel_bg << "; color: " << accent << "; font-weight: 600; }\n"
        << ".sf-tool-btn { background: " << card_bg << "; color: " << fg << "; border: 1px solid " << border << "; border-radius: 8px; padding: 4px 10px; min-height: 28px; }\n"
        << ".sf-tool-btn:hover { border-color: " << accent << "; }\n"
        << ".sf-tool-btn.is-on { background: " << sel_bg << "; border-color: " << accent << "; color: " << accent << "; }\n"
        << ".sf-tool-danger { background: rgba(244, 63, 94, 0.14); color: " << danger << "; border: 1px solid rgba(244, 63, 94, 0.35); border-radius: 8px; padding: 4px 10px; min-height: 28px; font-weight: 600; }\n"
        << ".sf-tool-danger:hover { background: rgba(244, 63, 94, 0.24); border-color: #f43f5e; }\n"
        << ".sf-crumb-pill { background: " << card_bg << "; color: " << fg << "; border: 1px solid " << border << "; border-radius: 7px; padding: 3px 9px; font-size: 12px; }\n"
        << ".sf-crumb-pill:hover { border-color: " << accent << "; }\n"
        << ".sf-entry { background: " << card_bg << "; color: " << fg << "; border: 1px solid " << border << "; border-radius: 8px; padding: 4px 10px; min-height: 28px; }\n"
        << "listview.sf-listview, gridview.sf-gridview { background: transparent; padding: 6px; }\n"
        << "listview.sf-listview > row { border-radius: 8px; margin: 1px 4px; }\n"
        << "listview.sf-listview > row:hover { background: " << hover_bg << "; }\n"
        << "listview.sf-listview > row:selected { background: " << sel_bg << "; }\n"
        << ".sf-row { padding: 6px 10px; }\n"
        << "gridview.sf-gridview > child { border-radius: 10px; margin: 4px; }\n"
        << ".sf-grid-card { background: " << card_bg << "; border: 1px solid " << border << "; border-radius: 10px; padding: 10px 8px; min-width: 118px; }\n"
        << "gridview.sf-gridview > child:hover .sf-grid-card { border-color: " << accent << "; }\n"
        << "gridview.sf-gridview > child:selected .sf-grid-card { background: " << sel_bg << "; border-color: " << accent << "; }\n"
        << ".sf-muted { color: " << muted << "; font-size: 12px; }\n"
        << ".sf-status { background: " << bar_bg << "; border-top: 1px solid " << border << "; padding: 5px 14px; font-size: 12px; color: " << muted << "; }\n"
        << ".sf-ico-dir { color: #38bdf8; }\n"
        << ".sf-ico-img { color: #f472b6; }\n"
        << ".sf-ico-vid { color: #a78bfa; }\n"
        << ".sf-ico-aud { color: #34d399; }\n"
        << ".sf-ico-arc { color: #fbbf24; }\n"
        << ".sf-ico-code { color: #60a5fa; }\n"
        << ".sf-ico-exec { color: #fb7185; }\n"
        << ".sf-ico-file { color: " << muted << "; }\n"
        << ".sf-menu-item { background: transparent; color: " << fg << "; border: none; border-radius: 6px; padding: 6px 12px; font-size: 13px; }\n"
        << ".sf-menu-item:hover { background: " << hover_bg << "; }\n"
        << ".sf-menu-danger { color: " << danger << "; }\n"
        << ".windows-app-list { background: " << card_bg << "; border: 1px solid " << border << "; border-radius: 14px; }\n"
        << ".windows-app-list > row + row { border-top: 1px solid " << border << "; }\n"
        << ".windows-app-row { padding: 8px 14px; }\n"
        << ".windows-app-name { font-size: 13px; font-weight: 600; }\n"
        << ".windows-app-row button:disabled { opacity: 0.45; }\n"
        << ".sf-drop-hover, .sf-drop-hover .sf-grid-card { background: " << sel_bg << "; border: 1px dashed " << accent << "; }\n"
        << ".sf-ql-header { background: " << bar_bg << "; border-bottom: 1px solid " << border << "; padding: 12px 16px; }\n"
        << ".sf-ql-title { font-size: 15px; font-weight: 700; }\n"
        << ".sf-ql-body { padding: 16px; }\n"
        << ".sf-ql-frame { background: " << card_bg << "; border: 1px solid " << border << "; border-radius: 12px; padding: 12px; }\n"
        << ".sf-ql-code { font-family: \"JetBrains Mono\", ui-monospace, monospace; font-size: 12.5px; background: transparent; color: " << fg << "; }\n";

    gtk_css_provider_load_from_string(binding.css, css.str().c_str());
}

} // namespace

void bind_files_window_theme(GtkWindow *window) {
    auto *binding = new FilesThemeBinding(gtk_widget_get_display(GTK_WIDGET(window)));
    update_theme(*binding);
    binding->timer = g_timeout_add(600, +[](gpointer data) -> gboolean {
        update_theme(*static_cast<FilesThemeBinding *>(data));
        return G_SOURCE_CONTINUE;
    }, binding);
    g_object_set_data_full(G_OBJECT(window), "spinel-files-theme", binding, +[](gpointer data) {
        delete static_cast<FilesThemeBinding *>(data);
    });
}
