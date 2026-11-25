#pragma once

#include <gtk/gtk.h>

#include <cstdint>
#include <string>
#include <vector>

struct SniPixmap {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> argb_be;  // Big-endian ARGB32 (network byte order per SNI spec)
};

struct SniTrayItemSnapshot {
    std::string service_key;       // Unique "<bus_name><object_path>"
    std::string bus_name;
    std::string object_path;
    std::string id;
    std::string title;
    std::string status = "Active"; // "Passive", "Active", "NeedsAttention"
    std::string category;
    std::string icon_name;
    std::string icon_theme_path;
    std::string attention_icon_name;
    std::string menu_path;
    std::string tooltip_title;
    std::string tooltip_body;
    bool item_is_menu = false;
    std::vector<SniPixmap> icon_pixmaps;
    std::vector<SniPixmap> attention_pixmaps;
};

using SniTrayChangedFn = void (*)(void *user_data);

// Starts or attaches to org.kde.StatusNotifierWatcher on the session bus and registers host.
void init_sni_tray(SniTrayChangedFn on_changed, void *user_data);
void shutdown_sni_tray();

// Thread-safe / main-loop snapshot accessors & top-bar container builder.
std::vector<SniTrayItemSnapshot> list_sni_tray_items();
void rebuild_sni_tray_box(GtkWidget *box, int icon_px = 16);

// Pure testable helpers for SNI service resolution, DBusMenu mnemonics & ARGB pixel conversion.
bool parse_sni_service_id(const std::string &service_arg,
                          const std::string &sender_bus,
                          std::string &out_bus_name,
                          std::string &out_obj_path,
                          std::string &out_service_key);

std::string sanitize_sni_menu_label(const std::string &raw);

bool convert_argb32_be_to_cairo_argb32(const uint8_t *src_be,
                                       int width,
                                       int height,
                                       int dst_stride_bytes,
                                       uint8_t *dst);
