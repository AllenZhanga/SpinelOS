#pragma once

#include "native_shell.hpp"
#include "desktop_apps.hpp"
#include "system_control.hpp"
#include "theme_manager.hpp"
#include "window_tracker.hpp"

#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace shell_internal {

struct DockPinSpec {
    std::string title;
    std::string cmd;
    std::string kind;
    std::vector<std::string> icon_keys;
    std::vector<std::string> win_match;
};

struct DockItem {
    std::string label;
    std::string cmd;
    std::string icon_path;
    std::string fallback_kind;
    std::string app_id;
    uint64_t win_id = 0;
    bool pinned = false;
    bool running = false;
    bool activated = false;
    bool minimized = false;
    bool closable = false;
    double anim_size = 48.0;
    double anim_lift = 0.0;
};

struct CcSliderCtx {
    AppState *state = nullptr;
    bool is_volume = true;
};

struct DesktopIconDrawCtx {
    NativeShellState *ns = nullptr;
    std::string path;
    std::string kind;
};

enum class BarGlyph {
    Wifi,
    Ethernet,
    Offline,
    Search,
    ControlCenter,
    Reboot,
    Power,
    Bluetooth,
    Gamepad,
    Hud,
    Appearance,
    Settings,
    Lock,
    Screenshot,
    Clipboard,
    Volume,
    Mute,
    Brightness,
    Battery,
    Dnd,
    Bell
};

}  // namespace shell_internal

struct NativeShellState {
    AppState *app_state = nullptr;

    // Top Bar
    GtkWindow *bar_win = nullptr;
    GtkWidget *bar_active_lbl = nullptr;
    GtkWidget *bar_app_menus = nullptr;
    std::string bar_menu_key;
    GtkWidget *bar_ws_sep = nullptr;
    GtkWidget *bar_ws_box = nullptr;
    GtkWidget *bar_tray_box = nullptr;
    GtkWidget *bar_tray_sep = nullptr;
    GtkWidget *bar_sandbox_btn = nullptr;
    GtkWidget *bar_sandbox_lbl = nullptr;
    GtkWidget *bar_ai_btn = nullptr;
    GtkWidget *bar_ai_dot_da = nullptr;
    GtkWidget *bar_ai_lbl = nullptr;
    GtkWidget *bar_ai_popover = nullptr;
    GtkWidget *bar_ai_list_box = nullptr;
    GFileMonitor *ai_dir_monitor = nullptr;
    guint ai_refresh_timer = 0;
    guint ai_toast_timer = 0;
    std::string ai_state_cls = "idle";
    std::string ai_toast_text;
    GtkWidget *bar_mem_lbl = nullptr;
    GtkWidget *bar_ime_lbl = nullptr;
    GtkWidget *bar_vol_btn = nullptr;
    GtkWidget *bar_vol_da = nullptr;
    bool bar_vol_muted = false;
    GtkWidget *bar_bat_box = nullptr;
    GtkWidget *bar_bat_lbl = nullptr;
    GtkWidget *clip_popover = nullptr;
    GtkWidget *clip_list_box = nullptr;
    GtkWidget *notif_popover = nullptr;
    GtkWidget *notif_list_box = nullptr;
    GtkWidget *bar_notif_da = nullptr;
    bool bar_notif_dnd = false;
    uint32_t notif_cleared_max_id = 0;
    GtkWidget *bar_net_btn = nullptr;
    GtkWidget *bar_net_da = nullptr;
    std::string bar_net_kind = "wifi";
    GtkWidget *bar_cc_btn = nullptr;
    GtkWidget *bar_clock_lbl = nullptr;
    guint clock_timer_id = 0;
    bool fullscreen_hidden = false;

    // Desktop
    GtkWindow *desktop_win = nullptr;
    GtkWidget *desktop_overlay = nullptr;
    GtkWidget *desktop_bg_da = nullptr;
    GtkWidget *desktop_flow = nullptr;
    GtkWidget *desktop_popover = nullptr;
    GtkWidget *desktop_entry_popover = nullptr;
    std::string desktop_ctx_path;
    bool desktop_ctx_is_dir = false;
    bool desktop_ctx_is_desktop_file = false;
    bool desktop_ctx_is_exec = false;
    GFileMonitor *desktop_dir_monitor = nullptr;
    guint desktop_refresh_timer = 0;
    std::string wallpaper = "crystal-blue";
    struct ExtraMonitorBg {
        GdkMonitor *mon = nullptr;
        GtkWindow *win = nullptr;
        GtkWidget *da = nullptr;
    };
    std::vector<ExtraMonitorBg> extra_desktop_bgs;
    GListModel *monitor_model = nullptr;
    gulong monitor_sig_id = 0;

    // Dock
    GtkWindow *dock_win = nullptr;
    GtkWidget *dock_da = nullptr;
    GtkWidget *dock_popover = nullptr;
    GtkWidget *dock_popover_box = nullptr;
    std::vector<shell_internal::DockPinSpec> dock_pins;
    bool dock_pins_loaded = false;
    std::vector<shell_internal::DockItem> pinned_items;
    std::vector<shell_internal::DockItem> extra_items;
    int hover_index = -1;  // 0..pinned-1, or pinned..pinned+extra-1
    int drag_src_index = -1;
    int drag_cur_index = -1;
    double drag_press_x = 0.0;
    double drag_grab_offset = 0.0;
    double drag_visual_cx = 0.0;
    bool drag_active = false;
    bool drag_just_finished = false;
    guint dock_anim_timer = 0;
    int dock_width = 620;
    std::string dock_mode = "magnify";  // "magnify" | "fixed" | "autohide"
    bool dock_revealed = true;
    double dock_hide_progress = 0.0;    // 0.0 = visible, 1.0 = hidden off bottom
    guint dock_autohide_timer = 0;
    guint dock_hide_grace_timer = 0;

    // Control Center
    GtkWindow *cc_backdrop_win = nullptr;
    GtkWindow *cc_win = nullptr;
    bool cc_was_active = false;

    // Startup Logo Splash
    GtkWindow *splash_win = nullptr;
    GtkWidget *splash_da = nullptr;
    guint splash_timer = 0;
    double splash_elapsed = 0.0;

    // Spotlight Global Launcher
    struct LauncherEntry {
        std::string title;
        std::string subtitle;
        std::string keywords;
        std::string cmd;
        std::string icon_path;
        std::string kind;
    };
    GtkWindow *launcher_backdrop_win = nullptr;
    GtkWindow *launcher_win = nullptr;
    GtkWidget *launcher_entry = nullptr;
    GtkWidget *launcher_scroll = nullptr;
    GtkWidget *launcher_list = nullptr;
    std::vector<LauncherEntry> launcher_entries;
    bool launcher_was_active = false;

    // Alt+Tab App / Window Switcher
    struct SwitcherItem {
        uint64_t win_id = 0;
        std::string app_name;
        std::string title;
        std::string icon_path;
        std::string kind;
        bool minimized = false;
        bool activated = false;
        bool maximized = false;
    };
    GtkWindow *switcher_backdrop_win = nullptr;
    GtkWindow *switcher_win = nullptr;
    GtkWidget *switcher_flow = nullptr;
    GtkWidget *switcher_hint_lbl = nullptr;
    std::vector<SwitcherItem> switcher_items;
    std::vector<GtkWidget *> switcher_cards;
    int switcher_index = 0;
    bool switcher_was_active = false;

    // Custom desktop wallpaper image cache (keyed by path@WxH for multi-monitor support)
    std::string custom_wallpaper_path;
    std::map<std::string, cairo_surface_t *> wallpaper_surfs;

    // Floating Hardware OSD Pill (Volume / Brightness)
    GtkWindow *osd_win = nullptr;
    GtkWidget *osd_da = nullptr;
    guint osd_timer = 0;
    std::string osd_kind = "volume";
    int osd_value = 0;
    bool osd_muted = false;
    GtkWidget *cc_net_circle = nullptr;
    GtkWidget *cc_net_val = nullptr;
    GtkWidget *cc_bt_circle = nullptr;
    GtkWidget *cc_bt_val = nullptr;
    GtkWidget *cc_gpu_badge = nullptr;
    GtkWidget *cc_mem_val = nullptr;
    GtkWidget *cc_gm_circle = nullptr;
    GtkWidget *cc_gm_val = nullptr;
    GtkWidget *cc_gm_dot = nullptr;
    GtkWidget *cc_dnd_circle = nullptr;
    GtkWidget *cc_dnd_val = nullptr;
    GtkWidget *cc_dnd_dot = nullptr;
    GtkWidget *cc_mh_circle = nullptr;
    GtkWidget *cc_mh_val = nullptr;
    GtkWidget *cc_bright_pct = nullptr;
    GtkWidget *cc_bright_scale = nullptr;
    GtkWidget *cc_vol_mute_da = nullptr;
    bool cc_vol_muted = false;
    GtkWidget *cc_vol_pct = nullptr;
    GtkWidget *cc_vol_scale = nullptr;
    bool cc_updating_sliders = false;

    // Cached states
    ThemeSpec theme;
    GtkCssProvider *css_provider = nullptr;
    struct SandboxPipState *sandbox_pip = nullptr;
    TelemetrySnapshot telemetry;
    std::vector<DesktopAppInfo> desktop_apps;
    std::map<std::string, cairo_surface_t *> surface_cache;
    std::vector<shell_internal::CcSliderCtx *> slider_ctxs;
};

namespace shell_internal {

// Geometry, Color & String Helpers
void cairo_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r);
std::string to_lower_copy(std::string s);
bool contains_ci(const std::string &hay, const std::string &needle);
std::string match_app_kind(const std::string &s);
std::string find_app_icon_path(const std::vector<DesktopAppInfo> &apps,
                               const std::vector<std::string> &keys);
cairo_surface_t *pixbuf_to_cairo_surface(GdkPixbuf *pb);
cairo_surface_t *get_cached_surface(NativeShellState *ns, const std::string &path, int target_px);
void set_rgb_hex(cairo_pattern_t *pat, double offset, uint32_t hex, double alpha = 1.0);
void parse_css_rgba(const std::string &css, double &r, double &g, double &b, double &a);

// App Icons, Typography & Monochrome Vector Glyphs
void draw_app_icon(NativeShellState *ns,
                   cairo_t *cr,
                   double x,
                   double y,
                   double sz,
                   const std::string &icon_path,
                   const std::string &fallback_kind);
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
                     int *out_w = nullptr,
                     int *out_h = nullptr,
                     bool measure_only = false);
void draw_desktop_icon_cb(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer user_data);
void free_desktop_icon_ctx(gpointer data);

void draw_spinel_logo_at(NativeShellState *ns, cairo_t *cr, double x, double y, double sz);
void draw_gem_glyph(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer user_data);
void draw_monochrome_glyph(cairo_t *cr, int w, int h, BarGlyph kind, const GdkRGBA &fg);
void draw_glyph_cb(GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer data);

// Shared Command Helpers
void trigger_cmd(AppState *state, const std::string &cmd);
void on_cmd_button_clicked(GtkButton *btn, gpointer user_data);
GtkWidget *make_cmd_button(AppState *state, const char *label, const char *css_class, const char *cmd);
GtkWidget *make_glyph_icon_btn(AppState *state, BarGlyph kind, const char *tooltip, const char *cmd, const char *extra_cls = nullptr);

// Top Bar
void sync_top_bar_app_menus(NativeShellState *ns,
                            const TrackedWindowInfo *active_win,
                            std::string &out_display_name);
void sync_top_bar_workspaces(NativeShellState *ns);
void sync_top_bar_tray(NativeShellState *ns);
void build_native_top_bar(GtkApplication *app, NativeShellState *ns);

// Desktop & Multi-Monitor
void rebuild_desktop_shortcuts(NativeShellState *ns);
void build_native_desktop(GtkApplication *app, NativeShellState *ns);
void sync_monitor_wallpapers(GtkApplication *app, NativeShellState *ns);

// Dock
void rebuild_dock_items(NativeShellState *ns, const std::vector<TrackedWindowInfo> &windows);
void build_native_dock(GtkApplication *app, NativeShellState *ns);

// Overlays (Control Center, Launcher, Splash, OSD & CSS)
void build_native_control_center(GtkApplication *app, NativeShellState *ns);
void apply_native_css(NativeShellState *ns);
void build_native_launcher(GtkApplication *app, NativeShellState *ns);
void build_native_switcher(GtkApplication *app, NativeShellState *ns);
void close_startup_splash(NativeShellState *ns);
void show_desktop_startup_splash(GtkApplication *app, NativeShellState *ns);
void build_osd_window(GtkApplication *app, NativeShellState *ns);
void sync_cc_media_sliders(NativeShellState *ns);

}  // namespace shell_internal
