#include "shell/sni_tray.hpp"
#include "common.hpp"

#include <gio/gio.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char *kWatcherBusName = "org.kde.StatusNotifierWatcher";
constexpr const char *kWatcherObjPath = "/StatusNotifierWatcher";
constexpr const char *kWatcherIface = "org.kde.StatusNotifierWatcher";
constexpr const char *kItemIface = "org.kde.StatusNotifierItem";
constexpr const char *kDbusMenuIface = "com.canonical.dbusmenu";

constexpr const char *kWatcherIntrospectionXml = R"(
<node>
  <interface name='org.kde.StatusNotifierWatcher'>
    <method name='RegisterStatusNotifierItem'>
      <arg type='s' name='service' direction='in'/>
    </method>
    <method name='RegisterStatusNotifierHost'>
      <arg type='s' name='service' direction='in'/>
    </method>
    <signal name='StatusNotifierItemRegistered'>
      <arg type='s' name='service'/>
    </signal>
    <signal name='StatusNotifierItemUnregistered'>
      <arg type='s' name='service'/>
    </signal>
    <signal name='StatusNotifierHostRegistered'/>
    <property name='RegisteredStatusNotifierItems' type='as' access='read'/>
    <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>
    <property name='ProtocolVersion' type='i' access='read'/>
  </interface>
</node>
)";

struct DbusMenuNode {
    int32_t id = 0;
    std::string label;
    std::string type;
    std::string toggle_type;
    int32_t toggle_state = -1;
    std::string children_display;
    bool enabled = true;
    bool visible = true;
    std::vector<DbusMenuNode> children;
};

struct SniItemRecord {
    SniTrayItemSnapshot info;
    guint props_sub_id = 0;
    bool loading = false;
    bool alive = true;
};

struct SniHostState {
    bool initialized = false;
    GDBusConnection *bus = nullptr;
    GDBusNodeInfo *watcher_node = nullptr;
    guint watcher_owner_id = 0;
    guint watcher_reg_id = 0;
    guint host_owner_id = 0;
    guint name_owner_sub_id = 0;
    guint ext_watcher_sub_id = 0;

    std::string host_service_name;
    std::vector<std::string> hosts;
    std::map<std::string, std::unique_ptr<SniItemRecord>> items;  // keyed by service_key

    SniTrayChangedFn on_changed = nullptr;
    void *on_changed_user = nullptr;
    guint notify_idle_id = 0;
};

SniHostState g_sni;

void emit_changed_signal_dbus(const char *signal_name, const std::string &arg) {
    if (!g_sni.bus || g_sni.watcher_reg_id == 0) return;
    GVariant *params = arg.empty() ? nullptr : g_variant_new("(s)", arg.c_str());
    g_dbus_connection_emit_signal(g_sni.bus, nullptr, kWatcherObjPath, kWatcherIface,
                                  signal_name, params, nullptr);
}

void schedule_notify_changed() {
    if (!g_sni.initialized || !g_sni.on_changed) return;
    if (g_sni.notify_idle_id != 0) return;
    g_sni.notify_idle_id = g_idle_add(+[](gpointer) -> gboolean {
        g_sni.notify_idle_id = 0;
        if (g_sni.initialized && g_sni.on_changed) {
            g_sni.on_changed(g_sni.on_changed_user);
        }
        return G_SOURCE_REMOVE;
    }, nullptr);
}

void fetch_sni_item_properties(const std::string &service_key);

void register_sni_item(const std::string &service_arg, const std::string &sender_bus) {
    std::string bus_name, obj_path, key;
    if (!parse_sni_service_id(service_arg, sender_bus, bus_name, obj_path, key)) {
        return;
    }

    auto existing = g_sni.items.find(key);
    if (existing != g_sni.items.end()) {
        fetch_sni_item_properties(key);
        return;
    }

    auto rec = std::make_unique<SniItemRecord>();
    rec->info.service_key = key;
    rec->info.bus_name = bus_name;
    rec->info.object_path = obj_path;

    if (g_sni.bus) {
        auto *key_copy = new std::string(key);
        rec->props_sub_id = g_dbus_connection_signal_subscribe(
            g_sni.bus,
            bus_name.c_str(),
            kItemIface,
            nullptr,
            obj_path.c_str(),
            nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            +[](GDBusConnection *, const gchar *, const gchar *, const gchar *,
                const gchar *sig_name, GVariant *, gpointer d) {
                const auto *sk = static_cast<const std::string *>(d);
                if (!sk || !sig_name) return;
                if (std::strcmp(sig_name, "NewTitle") == 0 ||
                    std::strcmp(sig_name, "NewIcon") == 0 ||
                    std::strcmp(sig_name, "NewAttentionIcon") == 0 ||
                    std::strcmp(sig_name, "NewStatus") == 0 ||
                    std::strcmp(sig_name, "NewToolTip") == 0) {
                    fetch_sni_item_properties(*sk);
                }
            },
            key_copy,
            +[](gpointer d) { delete static_cast<std::string *>(d); });
    }

    g_sni.items[key] = std::move(rec);
    emit_changed_signal_dbus("StatusNotifierItemRegistered", key);
    fetch_sni_item_properties(key);
    schedule_notify_changed();
}

void unregister_sni_item(const std::string &key) {
    auto it = g_sni.items.find(key);
    if (it == g_sni.items.end()) return;
    it->second->alive = false;
    if (g_sni.bus && it->second->props_sub_id != 0) {
        g_dbus_connection_signal_unsubscribe(g_sni.bus, it->second->props_sub_id);
        it->second->props_sub_id = 0;
    }
    g_sni.items.erase(it);
    emit_changed_signal_dbus("StatusNotifierItemUnregistered", key);
    schedule_notify_changed();
}

std::vector<SniPixmap> decode_pixmap_array(GVariant *arr_val) {
    std::vector<SniPixmap> out;
    if (!arr_val || !g_variant_is_of_type(arr_val, G_VARIANT_TYPE("a(iiay)"))) {
        return out;
    }
    GVariantIter iter;
    g_variant_iter_init(&iter, arr_val);
    gint32 w = 0, h = 0;
    GVariant *bytes_v = nullptr;
    while (g_variant_iter_loop(&iter, "(ii@ay)", &w, &h, &bytes_v)) {
        if (w > 0 && h > 0 && w <= 512 && h <= 512 && bytes_v) {
            gsize n_bytes = 0;
            const auto *raw = static_cast<const uint8_t *>(g_variant_get_fixed_array(bytes_v, &n_bytes, 1));
            const size_t expected = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
            if (raw && n_bytes >= expected) {
                SniPixmap pm;
                pm.width = w;
                pm.height = h;
                pm.argb_be.assign(raw, raw + expected);
                out.push_back(std::move(pm));
            }
        }
    }
    return out;
}

void apply_sni_properties_variant(SniItemRecord &rec, GVariant *dict_v) {
    if (!dict_v || !g_variant_is_of_type(dict_v, G_VARIANT_TYPE("a{sv}"))) return;
    GVariantIter iter;
    const gchar *prop_name = nullptr;
    GVariant *val = nullptr;
    g_variant_iter_init(&iter, dict_v);
    while (g_variant_iter_loop(&iter, "{&sv}", &prop_name, &val)) {
        if (!prop_name || !val) continue;
        if (std::strcmp(prop_name, "Id") == 0 && g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
            rec.info.id = g_variant_get_string(val, nullptr);
        } else if (std::strcmp(prop_name, "Title") == 0 && g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
            rec.info.title = g_variant_get_string(val, nullptr);
        } else if (std::strcmp(prop_name, "Status") == 0 && g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
            rec.info.status = g_variant_get_string(val, nullptr);
        } else if (std::strcmp(prop_name, "Category") == 0 && g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
            rec.info.category = g_variant_get_string(val, nullptr);
        } else if (std::strcmp(prop_name, "IconName") == 0 && g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
            rec.info.icon_name = g_variant_get_string(val, nullptr);
        } else if (std::strcmp(prop_name, "IconThemePath") == 0 && g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
            rec.info.icon_theme_path = g_variant_get_string(val, nullptr);
        } else if (std::strcmp(prop_name, "AttentionIconName") == 0 && g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
            rec.info.attention_icon_name = g_variant_get_string(val, nullptr);
        } else if (std::strcmp(prop_name, "Menu") == 0 &&
                   (g_variant_is_of_type(val, G_VARIANT_TYPE_OBJECT_PATH) ||
                    g_variant_is_of_type(val, G_VARIANT_TYPE_STRING))) {
            rec.info.menu_path = g_variant_get_string(val, nullptr);
        } else if (std::strcmp(prop_name, "ItemIsMenu") == 0 && g_variant_is_of_type(val, G_VARIANT_TYPE_BOOLEAN)) {
            rec.info.item_is_menu = g_variant_get_boolean(val) != FALSE;
        } else if (std::strcmp(prop_name, "IconPixmap") == 0) {
            rec.info.icon_pixmaps = decode_pixmap_array(val);
        } else if (std::strcmp(prop_name, "AttentionIconPixmap") == 0) {
            rec.info.attention_pixmaps = decode_pixmap_array(val);
        } else if (std::strcmp(prop_name, "ToolTip") == 0 &&
                   g_variant_is_of_type(val, G_VARIANT_TYPE("(sa(iiay)ss)"))) {
            const gchar *tt_title = nullptr;
            const gchar *tt_body = nullptr;
            g_variant_get(val, "(&s@a(iiay)&s&s)", nullptr, nullptr, &tt_title, &tt_body);
            if (tt_title) rec.info.tooltip_title = tt_title;
            if (tt_body) rec.info.tooltip_body = tt_body;
        }
    }
}

void fetch_sni_item_properties(const std::string &service_key) {
    if (!g_sni.bus) return;
    auto it = g_sni.items.find(service_key);
    if (it == g_sni.items.end() || it->second->loading) return;
    it->second->loading = true;
    auto *key_copy = new std::string(service_key);

    g_dbus_connection_call(
        g_sni.bus,
        it->second->info.bus_name.c_str(),
        it->second->info.object_path.c_str(),
        "org.freedesktop.DBus.Properties",
        "GetAll",
        g_variant_new("(s)", kItemIface),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        3000,
        nullptr,
        +[](GObject *src, GAsyncResult *res, gpointer d) {
            std::unique_ptr<std::string> key(static_cast<std::string *>(d));
            GError *err = nullptr;
            GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
            if (!g_sni.initialized) {
                if (ret) g_variant_unref(ret);
                if (err) g_error_free(err);
                return;
            }
            auto found = g_sni.items.find(*key);
            if (found != g_sni.items.end()) {
                found->second->loading = false;
                if (ret) {
                    GVariant *dict = g_variant_get_child_value(ret, 0);
                    apply_sni_properties_variant(*found->second, dict);
                    g_variant_unref(dict);
                    schedule_notify_changed();
                } else if (err && g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)) {
                    unregister_sni_item(*key);
                }
            }
            if (ret) g_variant_unref(ret);
            if (err) g_error_free(err);
        },
        key_copy);
}

void on_watcher_method_call(GDBusConnection *,
                            const gchar *sender,
                            const gchar *,
                            const gchar *,
                            const gchar *method_name,
                            GVariant *parameters,
                            GDBusMethodInvocation *invocation,
                            gpointer) {
    if (std::strcmp(method_name, "RegisterStatusNotifierItem") == 0) {
        const gchar *service = nullptr;
        g_variant_get(parameters, "(&s)", &service);
        register_sni_item(service ? service : "", sender ? sender : "");
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }
    if (std::strcmp(method_name, "RegisterStatusNotifierHost") == 0) {
        const gchar *service = nullptr;
        g_variant_get(parameters, "(&s)", &service);
        if (service && *service &&
            std::find(g_sni.hosts.begin(), g_sni.hosts.end(), service) == g_sni.hosts.end()) {
            g_sni.hosts.emplace_back(service);
            emit_changed_signal_dbus("StatusNotifierHostRegistered", "");
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method");
}

GVariant *on_watcher_get_property(GDBusConnection *,
                                  const gchar *,
                                  const gchar *,
                                  const gchar *,
                                  const gchar *property_name,
                                  GError **,
                                  gpointer) {
    if (std::strcmp(property_name, "RegisteredStatusNotifierItems") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        for (const auto &kv : g_sni.items) {
            g_variant_builder_add(&b, "s", kv.first.c_str());
        }
        return g_variant_builder_end(&b);
    }
    if (std::strcmp(property_name, "IsStatusNotifierHostRegistered") == 0) {
        return g_variant_new_boolean(TRUE);
    }
    if (std::strcmp(property_name, "ProtocolVersion") == 0) {
        return g_variant_new_int32(0);
    }
    return nullptr;
}

const GDBusInterfaceVTable kWatcherVTable = {
    on_watcher_method_call,
    on_watcher_get_property,
    nullptr,
    {nullptr}
};

void sync_items_from_external_watcher() {
    if (!g_sni.bus) return;
    g_dbus_connection_call(
        g_sni.bus,
        kWatcherBusName,
        kWatcherObjPath,
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", kWatcherIface, "RegisteredStatusNotifierItems"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        3000,
        nullptr,
        +[](GObject *src, GAsyncResult *res, gpointer) {
            GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
            if (!ret) return;
            if (g_sni.initialized) {
                GVariant *boxed = nullptr;
                g_variant_get(ret, "(v)", &boxed);
                if (boxed && g_variant_is_of_type(boxed, G_VARIANT_TYPE("as"))) {
                    GVariantIter it;
                    const gchar *svc = nullptr;
                    g_variant_iter_init(&it, boxed);
                    while (g_variant_iter_loop(&it, "&s", &svc)) {
                        if (svc && *svc) register_sni_item(svc, "");
                    }
                }
                if (boxed) g_variant_unref(boxed);
            }
            g_variant_unref(ret);
        },
        nullptr);
}

/* ==================== Tray Icon Rendering Helpers ==================== */

const SniPixmap *pick_best_pixmap(const std::vector<SniPixmap> &pixmaps, int target_px) {
    if (pixmaps.empty()) return nullptr;
    const SniPixmap *best = nullptr;
    int best_score = 1e9;
    for (const auto &pm : pixmaps) {
        if (pm.width <= 0 || pm.height <= 0) continue;
        int sz = std::max(pm.width, pm.height);
        int diff = std::abs(sz - target_px * 2);  // Prefer >=2x for HiDPI crispness
        if (sz < target_px) diff += 500;
        if (diff < best_score) {
            best_score = diff;
            best = &pm;
        }
    }
    return best;
}

cairo_surface_t *create_cairo_surface_from_pixmap(const SniPixmap &pm) {
    if (pm.width <= 0 || pm.height <= 0) return nullptr;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pm.width, pm.height);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return nullptr;
    }
    uint8_t *dst = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    if (!convert_argb32_be_to_cairo_argb32(pm.argb_be.data(), pm.width, pm.height, stride, dst)) {
        cairo_surface_destroy(surf);
        return nullptr;
    }
    cairo_surface_mark_dirty(surf);
    return surf;
}

std::string find_tray_icon_file(const std::string &icon_name, const std::string &theme_path) {
    if (icon_name.empty()) return "";
    std::error_code ec;
    if (icon_name.front() == '/' && fs::is_regular_file(icon_name, ec)) {
        return icon_name;
    }
    auto probe_dir = [&](const fs::path &base) -> std::string {
        if (base.empty() || !fs::is_directory(base, ec)) return "";
        static const char *kExts[] = {".svg", ".png", ".xpm"};
        for (const char *ext : kExts) {
            fs::path direct = base / (icon_name + ext);
            if (fs::is_regular_file(direct, ec)) return direct.string();
        }
        static const char *kSizes[] = {"scalable", "32x32", "24x24", "22x22", "48x48", "16x16", "64x64"};
        static const char *kCats[] = {"apps", "status", "panel", "devices", "actions"};
        for (const char *sz : kSizes) {
            for (const char *cat : kCats) {
                for (const char *ext : kExts) {
                    fs::path p = base / sz / cat / (icon_name + ext);
                    if (fs::is_regular_file(p, ec)) return p.string();
                }
            }
        }
        return "";
    };
    if (!theme_path.empty()) {
        std::string found = probe_dir(theme_path);
        if (!found.empty()) return found;
        found = probe_dir(fs::path(theme_path) / "hicolor");
        if (!found.empty()) return found;
    }
    for (const fs::path &sys_dir : {fs::path("/usr/share/icons/hicolor"), fs::path("/usr/share/pixmaps")}) {
        std::string found = probe_dir(sys_dir);
        if (!found.empty()) return found;
    }
    return "";
}

struct TrayIconDrawData {
    cairo_surface_t *pixmap_surf = nullptr;
    std::string fallback_title;
    bool needs_attention = false;
};

void free_tray_draw_data(gpointer d) {
    auto *td = static_cast<TrayIconDrawData *>(d);
    if (!td) return;
    if (td->pixmap_surf) cairo_surface_destroy(td->pixmap_surf);
    delete td;
}

void draw_tray_icon_cb(GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer d) {
    auto *td = static_cast<TrayIconDrawData *>(d);
    if (!td) return;
    if (td->pixmap_surf) {
        int sw = cairo_image_surface_get_width(td->pixmap_surf);
        int sh = cairo_image_surface_get_height(td->pixmap_surf);
        if (sw > 0 && sh > 0) {
            double target = std::min(w, h) - 1.0;
            double sc = std::min(target / sw, target / sh);
            double dw = sw * sc;
            double dh = sh * sc;
            cairo_save(cr);
            cairo_translate(cr, (w - dw) * 0.5, (h - dh) * 0.5);
            cairo_scale(cr, sc, sc);
            cairo_set_source_surface(cr, td->pixmap_surf, 0.0, 0.0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
            cairo_paint(cr);
            cairo_restore(cr);
        }
    } else {
        GdkRGBA fg{1.0f, 1.0f, 1.0f, 0.92f};
        gtk_widget_get_color(GTK_WIDGET(da), &fg);
        cairo_save(cr);
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.18);
        cairo_arc(cr, w * 0.5, h * 0.5, std::min(w, h) * 0.44, 0, 2 * M_PI);
        cairo_fill(cr);
        char ch = '?';
        for (unsigned char c : td->fallback_title) {
            if (std::isalnum(c)) {
                ch = static_cast<char>(std::toupper(c));
                break;
            }
        }
        char buf[2]{ch, '\0'};
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.95);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 10.5);
        cairo_text_extents_t ext{};
        cairo_text_extents(cr, buf, &ext);
        cairo_move_to(cr, (w - ext.width) * 0.5 - ext.x_bearing,
                      (h - ext.height) * 0.5 - ext.y_bearing);
        cairo_show_text(cr, buf);
        cairo_restore(cr);
    }
    if (td->needs_attention) {
        cairo_save(cr);
        cairo_set_source_rgba(cr, 0.96, 0.25, 0.37, 0.96);
        cairo_arc(cr, w - 3.0, 3.0, 2.8, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_restore(cr);
    }
}

cairo_surface_t *create_cairo_surface_from_file(const std::string &path, int target_px) {
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path.c_str(), target_px, target_px, TRUE, nullptr);
    if (!pb) return nullptr;
    int pw = gdk_pixbuf_get_width(pb);
    int ph = gdk_pixbuf_get_height(pb);
    int n_ch = gdk_pixbuf_get_n_channels(pb);
    int rowstride = gdk_pixbuf_get_rowstride(pb);
    const guchar *src = gdk_pixbuf_read_pixels(pb);

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        g_object_unref(pb);
        return nullptr;
    }
    unsigned char *dst = cairo_image_surface_get_data(surf);
    int dst_stride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < ph; ++y) {
        const guchar *srow = src + y * rowstride;
        auto *drow = reinterpret_cast<uint32_t *>(dst + y * dst_stride);
        for (int x = 0; x < pw; ++x) {
            uint32_t r = srow[x * n_ch + 0];
            uint32_t g = srow[x * n_ch + 1];
            uint32_t b = srow[x * n_ch + 2];
            uint32_t a = (n_ch >= 4) ? srow[x * n_ch + 3] : 255u;
            uint32_t pr = (r * a + 127u) / 255u;
            uint32_t pg = (g * a + 127u) / 255u;
            uint32_t pb_v = (b * a + 127u) / 255u;
            drow[x] = (a << 24) | (pr << 16) | (pg << 8) | pb_v;
        }
    }
    cairo_surface_mark_dirty(surf);
    g_object_unref(pb);
    return surf;
}

GtkWidget *build_tray_icon_widget(const SniTrayItemSnapshot &item, int icon_px) {
    bool use_att = (item.status == "NeedsAttention");
    cairo_surface_t *surf = nullptr;
    const SniPixmap *pm = pick_best_pixmap(
        use_att && !item.attention_pixmaps.empty() ? item.attention_pixmaps : item.icon_pixmaps,
        icon_px);
    if (pm) {
        surf = create_cairo_surface_from_pixmap(*pm);
    }

    const std::string &icon_name = (use_att && !item.attention_icon_name.empty())
                                       ? item.attention_icon_name
                                       : item.icon_name;
    if (!surf) {
        std::string direct_path = find_tray_icon_file(icon_name, item.icon_theme_path);
        if (!direct_path.empty()) {
            surf = create_cairo_surface_from_file(direct_path, icon_px * 2);
        }
    }
    if (surf) {
        auto *td = new TrayIconDrawData();
        td->pixmap_surf = surf;
        td->needs_attention = use_att;
        GtkWidget *da = gtk_drawing_area_new();
        gtk_widget_set_size_request(da, icon_px, icon_px);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_tray_icon_cb, td, free_tray_draw_data);
        return da;
    }

    if (!icon_name.empty()) {
        if (GdkDisplay *disp = gdk_display_get_default()) {
            GtkIconTheme *theme = gtk_icon_theme_get_for_display(disp);
            if (theme && !item.icon_theme_path.empty()) {
                gtk_icon_theme_add_search_path(theme, item.icon_theme_path.c_str());
            }
            if (theme && gtk_icon_theme_has_icon(theme, icon_name.c_str())) {
                GtkWidget *img = gtk_image_new_from_icon_name(icon_name.c_str());
                gtk_image_set_pixel_size(GTK_IMAGE(img), icon_px);
                if (!use_att) return img;

                GtkWidget *ov = gtk_overlay_new();
                gtk_overlay_set_child(GTK_OVERLAY(ov), img);
                GtkWidget *dot = gtk_drawing_area_new();
                gtk_widget_set_size_request(dot, 7, 7);
                gtk_widget_set_halign(dot, GTK_ALIGN_END);
                gtk_widget_set_valign(dot, GTK_ALIGN_START);
                gtk_widget_set_can_target(dot, FALSE);
                gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(dot),
                    +[](GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer) {
                        cairo_set_source_rgba(cr, 0.96, 0.25, 0.37, 0.98);
                        cairo_arc(cr, w * 0.5, h * 0.5, std::min(w, h) * 0.42, 0, 2 * M_PI);
                        cairo_fill(cr);
                    }, nullptr, nullptr);
                gtk_overlay_add_overlay(GTK_OVERLAY(ov), dot);
                return ov;
            }
        }
    }

    auto *td = new TrayIconDrawData();
    td->fallback_title = !item.title.empty() ? item.title : (!item.id.empty() ? item.id : item.bus_name);
    td->needs_attention = use_att;
    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, icon_px, icon_px);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_tray_icon_cb, td, free_tray_draw_data);
    return da;
}

/* ==================== DBusMenu Client & GtkStack Popover ==================== */

DbusMenuNode parse_dbusmenu_node(GVariant *node_v) {
    DbusMenuNode node;
    if (!node_v) return node;
    GVariant *props_v = nullptr;
    GVariant *children_v = nullptr;
    g_variant_get(node_v, "(i@a{sv}@av)", &node.id, &props_v, &children_v);
    if (props_v) {
        GVariantIter it;
        const gchar *k = nullptr;
        GVariant *v = nullptr;
        g_variant_iter_init(&it, props_v);
        while (g_variant_iter_loop(&it, "{&sv}", &k, &v)) {
            if (!k || !v) continue;
            if (std::strcmp(k, "label") == 0 && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
                node.label = sanitize_sni_menu_label(g_variant_get_string(v, nullptr));
            } else if (std::strcmp(k, "type") == 0 && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
                node.type = g_variant_get_string(v, nullptr);
            } else if (std::strcmp(k, "toggle-type") == 0 && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
                node.toggle_type = g_variant_get_string(v, nullptr);
            } else if (std::strcmp(k, "toggle-state") == 0 && g_variant_is_of_type(v, G_VARIANT_TYPE_INT32)) {
                node.toggle_state = g_variant_get_int32(v);
            } else if (std::strcmp(k, "children-display") == 0 && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
                node.children_display = g_variant_get_string(v, nullptr);
            } else if (std::strcmp(k, "enabled") == 0 && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) {
                node.enabled = g_variant_get_boolean(v) != FALSE;
            } else if (std::strcmp(k, "visible") == 0 && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) {
                node.visible = g_variant_get_boolean(v) != FALSE;
            }
        }
        g_variant_unref(props_v);
    }
    if (children_v) {
        GVariantIter it;
        GVariant *child_box = nullptr;
        g_variant_iter_init(&it, children_v);
        while (g_variant_iter_loop(&it, "v", &child_box)) {
            if (child_box) {
                node.children.push_back(parse_dbusmenu_node(child_box));
            }
        }
        g_variant_unref(children_v);
    }
    return node;
}

struct MenuInvokeReq {
    std::string bus_name;
    std::string menu_path;
    int32_t item_id = 0;
    GtkWidget *popover = nullptr;
};

void send_dbusmenu_click(const std::string &bus, const std::string &menu_path, int32_t item_id) {
    if (!g_sni.bus || bus.empty() || menu_path.empty()) return;
    guint32 ts = static_cast<guint32>(std::time(nullptr));
    g_dbus_connection_call(
        g_sni.bus,
        bus.c_str(),
        menu_path.c_str(),
        kDbusMenuIface,
        "Event",
        g_variant_new("(isvu)", item_id, "clicked", g_variant_new_string(""), ts),
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        2000,
        nullptr,
        nullptr,
        nullptr);
}

void build_dbusmenu_stack_page(GtkStack *stack,
                               const std::string &page_name,
                               const std::string &parent_page,
                               const std::string &header_title,
                               const DbusMenuNode &node,
                               const std::string &bus_name,
                               const std::string &menu_path,
                               GtkWidget *popover) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_size_request(box, 196, -1);

    if (!parent_page.empty()) {
        GtkWidget *back = gtk_button_new();
        gtk_widget_add_css_class(back, "spinel-ctx-item");
        std::string btxt = "‹ " + (header_title.empty() ? std::string("返回") : header_title);
        GtkWidget *blbl = gtk_label_new(btxt.c_str());
        gtk_widget_set_halign(blbl, GTK_ALIGN_START);
        gtk_button_set_child(GTK_BUTTON(back), blbl);
        g_object_set_data_full(G_OBJECT(back), "target-page", g_strdup(parent_page.c_str()), g_free);
        g_object_set_data(G_OBJECT(back), "menu-stack", stack);
        g_signal_connect(back, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer) {
            auto *stk = GTK_STACK(g_object_get_data(G_OBJECT(b), "menu-stack"));
            const char *pg = static_cast<const char *>(g_object_get_data(G_OBJECT(b), "target-page"));
            if (stk && pg) gtk_stack_set_visible_child_name(stk, pg);
        }), nullptr);
        gtk_box_append(GTK_BOX(box), back);
        gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    }

    int sub_idx = 0;
    for (const auto &ch : node.children) {
        if (!ch.visible) continue;
        if (ch.type == "separator") {
            gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
            continue;
        }
        bool has_sub = (ch.children_display == "submenu" || !ch.children.empty());
        std::string display = ch.label.empty() ? ("#" + std::to_string(ch.id)) : ch.label;
        if (ch.toggle_type == "checkmark") {
            display = (ch.toggle_state == 1 ? "✓ " : "   ") + display;
        } else if (ch.toggle_type == "radio") {
            display = (ch.toggle_state == 1 ? "● " : "○ ") + display;
        }

        GtkWidget *btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "spinel-ctx-item");
        gtk_widget_set_sensitive(btn, ch.enabled ? TRUE : FALSE);

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *lbl = gtk_label_new(display.c_str());
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(lbl), 28);
        gtk_box_append(GTK_BOX(row), lbl);
        if (has_sub) {
            GtkWidget *arr = gtk_label_new("›");
            gtk_box_append(GTK_BOX(row), arr);
        }
        gtk_button_set_child(GTK_BUTTON(btn), row);

        if (has_sub) {
            std::string sub_page = page_name + "/" + std::to_string(sub_idx++);
            build_dbusmenu_stack_page(stack, sub_page, page_name, ch.label, ch, bus_name, menu_path, popover);
            g_object_set_data_full(G_OBJECT(btn), "target-page", g_strdup(sub_page.c_str()), g_free);
            g_object_set_data(G_OBJECT(btn), "menu-stack", stack);
            g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer) {
                auto *stk = GTK_STACK(g_object_get_data(G_OBJECT(b), "menu-stack"));
                const char *pg = static_cast<const char *>(g_object_get_data(G_OBJECT(b), "target-page"));
                if (stk && pg) gtk_stack_set_visible_child_name(stk, pg);
            }), nullptr);
        } else {
            auto *req = new MenuInvokeReq{bus_name, menu_path, ch.id, popover};
            g_object_set_data_full(G_OBJECT(btn), "invoke-req", req, +[](gpointer d) {
                delete static_cast<MenuInvokeReq *>(d);
            });
            g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer) {
                auto *ir = static_cast<MenuInvokeReq *>(g_object_get_data(G_OBJECT(b), "invoke-req"));
                if (!ir) return;
                if (ir->popover) gtk_popover_popdown(GTK_POPOVER(ir->popover));
                send_dbusmenu_click(ir->bus_name, ir->menu_path, ir->item_id);
            }), nullptr);
        }
        gtk_box_append(GTK_BOX(box), btn);
    }

    gtk_stack_add_named(stack, box, page_name.c_str());
}

struct TrayActionCtx {
    std::string bus_name;
    std::string object_path;
    std::string menu_path;
    bool item_is_menu = false;
    GtkWidget *anchor_btn = nullptr;
    GtkWidget *popover = nullptr;
};

void open_sni_menu_popover(TrayActionCtx *ctx);

void call_sni_simple_method(const TrayActionCtx &ctx, const char *method_name, bool fallback_to_menu) {
    if (!g_sni.bus || ctx.bus_name.empty() || ctx.object_path.empty()) return;
    auto *copy = fallback_to_menu ? new TrayActionCtx(ctx) : nullptr;
    g_dbus_connection_call(
        g_sni.bus,
        ctx.bus_name.c_str(),
        ctx.object_path.c_str(),
        kItemIface,
        method_name,
        g_variant_new("(ii)", 0, 0),
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        2000,
        nullptr,
        +[](GObject *src, GAsyncResult *res, gpointer d) {
            std::unique_ptr<TrayActionCtx> cb_ctx(static_cast<TrayActionCtx *>(d));
            GError *err = nullptr;
            GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
            if (ret) g_variant_unref(ret);
            if (err) {
                if (cb_ctx && g_sni.initialized && !cb_ctx->menu_path.empty()) {
                    open_sni_menu_popover(cb_ctx.get());
                }
                g_error_free(err);
            }
        },
        copy);
}

void fetch_dbusmenu_and_show(TrayActionCtx *ctx) {
    if (!g_sni.bus || !ctx || ctx->menu_path.empty()) return;
    auto *ctx_copy = new TrayActionCtx(*ctx);
    GVariantBuilder prop_b;
    g_variant_builder_init(&prop_b, G_VARIANT_TYPE("as"));
    const char *kProps[] = {"label", "enabled", "visible", "type", "toggle-type", "toggle-state", "children-display"};
    for (const char *p : kProps) g_variant_builder_add(&prop_b, "s", p);

    g_dbus_connection_call(
        g_sni.bus,
        ctx->bus_name.c_str(),
        ctx->menu_path.c_str(),
        kDbusMenuIface,
        "GetLayout",
        g_variant_new("(iias)", 0, -1, &prop_b),
        G_VARIANT_TYPE("(u(ia{sv}av))"),
        G_DBUS_CALL_FLAGS_NONE,
        2500,
        nullptr,
        +[](GObject *src, GAsyncResult *res, gpointer d) {
            std::unique_ptr<TrayActionCtx> mc(static_cast<TrayActionCtx *>(d));
            GError *err = nullptr;
            GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
            if (!g_sni.initialized || !mc->anchor_btn || !GTK_IS_WIDGET(mc->anchor_btn) ||
                !gtk_widget_get_realized(mc->anchor_btn)) {
                if (ret) g_variant_unref(ret);
                if (err) g_error_free(err);
                return;
            }
            if (!ret) {
                if (err) g_error_free(err);
                call_sni_simple_method(*mc, "ContextMenu", false);
                return;
            }
            GVariant *root_v = g_variant_get_child_value(ret, 1);
            DbusMenuNode root = parse_dbusmenu_node(root_v);
            g_variant_unref(root_v);
            g_variant_unref(ret);

            bool has_any = false;
            for (const auto &ch : root.children) {
                if (ch.visible && ch.type != "separator") {
                    has_any = true;
                    break;
                }
            }
            if (!has_any) {
                call_sni_simple_method(*mc, "ContextMenu", false);
                return;
            }

            GtkWidget *old_pop = GTK_WIDGET(g_object_get_data(G_OBJECT(mc->anchor_btn), "active-tray-pop"));
            if (old_pop && GTK_IS_POPOVER(old_pop)) {
                gtk_popover_popdown(GTK_POPOVER(old_pop));
                gtk_widget_unparent(old_pop);
                g_object_set_data(G_OBJECT(mc->anchor_btn), "active-tray-pop", nullptr);
            }
            GtkWidget *pop = gtk_popover_new();
            gtk_widget_add_css_class(pop, "spinel-ctx-popover");
            gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
            gtk_widget_set_parent(pop, mc->anchor_btn);
            g_object_set_data(G_OBJECT(mc->anchor_btn), "active-tray-pop", pop);

            GtkWidget *stack = gtk_stack_new();
            gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
            gtk_stack_set_transition_duration(GTK_STACK(stack), 140);
            gtk_stack_set_vhomogeneous(GTK_STACK(stack), FALSE);
            build_dbusmenu_stack_page(GTK_STACK(stack), "root", "", "", root,
                                      mc->bus_name, mc->menu_path, pop);
            gtk_stack_set_visible_child_name(GTK_STACK(stack), "root");

            gtk_popover_set_child(GTK_POPOVER(pop), stack);
            gtk_popover_popup(GTK_POPOVER(pop));
        },
        ctx_copy);
}

void open_sni_menu_popover(TrayActionCtx *ctx) {
    if (!ctx) return;
    if (ctx->menu_path.empty()) {
        call_sni_simple_method(*ctx, "ContextMenu", false);
        return;
    }
    if (!g_sni.bus) return;
    auto *ctx_copy = new TrayActionCtx(*ctx);
    g_dbus_connection_call(
        g_sni.bus,
        ctx->bus_name.c_str(),
        ctx->menu_path.c_str(),
        kDbusMenuIface,
        "AboutToShow",
        g_variant_new("(i)", 0),
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        1200,
        nullptr,
        +[](GObject *src, GAsyncResult *res, gpointer d) {
            std::unique_ptr<TrayActionCtx> mc(static_cast<TrayActionCtx *>(d));
            if (GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr)) {
                g_variant_unref(r);
            }
            if (g_sni.initialized) fetch_dbusmenu_and_show(mc.get());
        },
        ctx_copy);
}

}  // namespace

bool parse_sni_service_id(const std::string &service_arg,
                          const std::string &sender_bus,
                          std::string &out_bus_name,
                          std::string &out_obj_path,
                          std::string &out_service_key) {
    out_bus_name.clear();
    out_obj_path.clear();
    out_service_key.clear();

    std::string trimmed = trim_copy(service_arg);
    if (trimmed.empty()) return false;

    if (trimmed.front() == '/') {
        out_bus_name = trim_copy(sender_bus);
        out_obj_path = trimmed;
    } else {
        size_t slash = trimmed.find('/');
        if (slash != std::string::npos) {
            out_bus_name = trimmed.substr(0, slash);
            out_obj_path = trimmed.substr(slash);
        } else {
            out_bus_name = trimmed;
            out_obj_path = "/StatusNotifierItem";
        }
    }

    if (out_bus_name.empty() || out_obj_path.empty() || out_obj_path.front() != '/') {
        out_bus_name.clear();
        out_obj_path.clear();
        return false;
    }
    out_service_key = out_bus_name + out_obj_path;
    return true;
}

std::string sanitize_sni_menu_label(const std::string &raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '_') {
            if (i + 1 < raw.size() && raw[i + 1] == '_') {
                out += '_';
                ++i;
            }
            // Single '_' is a GTK/Qt mnemonic marker; strip from rendered label.
        } else {
            out += raw[i];
        }
    }
    return out;
}

bool convert_argb32_be_to_cairo_argb32(const uint8_t *src_be,
                                       int width,
                                       int height,
                                       int dst_stride_bytes,
                                       uint8_t *dst) {
    if (!src_be || !dst || width <= 0 || height <= 0 || dst_stride_bytes < width * 4) {
        return false;
    }
    for (int y = 0; y < height; ++y) {
        const uint8_t *srow = src_be + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
        auto *drow = reinterpret_cast<uint32_t *>(dst + static_cast<size_t>(y) * static_cast<size_t>(dst_stride_bytes));
        for (int x = 0; x < width; ++x) {
            uint32_t a = srow[x * 4 + 0];
            uint32_t r = srow[x * 4 + 1];
            uint32_t g = srow[x * 4 + 2];
            uint32_t b = srow[x * 4 + 3];
            uint32_t pr = (r * a + 127u) / 255u;
            uint32_t pg = (g * a + 127u) / 255u;
            uint32_t pb = (b * a + 127u) / 255u;
            drow[x] = (a << 24) | (pr << 16) | (pg << 8) | pb;
        }
    }
    return true;
}

void init_sni_tray(SniTrayChangedFn on_changed, void *user_data) {
    if (g_sni.initialized) return;
    g_sni.initialized = true;
    g_sni.on_changed = on_changed;
    g_sni.on_changed_user = user_data;

    GError *err = nullptr;
    g_sni.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!g_sni.bus) {
        if (err) g_error_free(err);
        return;
    }

    g_sni.watcher_node = g_dbus_node_info_new_for_xml(kWatcherIntrospectionXml, nullptr);
    g_sni.watcher_reg_id = g_dbus_connection_register_object(
        g_sni.bus,
        kWatcherObjPath,
        g_sni.watcher_node->interfaces[0],
        &kWatcherVTable,
        nullptr,
        nullptr,
        nullptr);

    g_sni.host_service_name = "org.kde.StatusNotifierHost-" + std::to_string(getpid()) + "-1";
    g_sni.hosts.push_back(g_sni.host_service_name);

    g_sni.watcher_owner_id = g_bus_own_name_on_connection(
        g_sni.bus,
        kWatcherBusName,
        G_BUS_NAME_OWNER_FLAGS_NONE,
        +[](GDBusConnection *, const gchar *, gpointer) {
            if (!g_sni.initialized) return;
            emit_changed_signal_dbus("StatusNotifierHostRegistered", "");
        },
        +[](GDBusConnection *conn, const gchar *, gpointer) {
            if (!g_sni.initialized) return;
            // Another watcher already exists: register ourselves as host & subscribe.
            g_dbus_connection_call(
                conn, kWatcherBusName, kWatcherObjPath, kWatcherIface,
                "RegisterStatusNotifierHost",
                g_variant_new("(s)", g_sni.host_service_name.c_str()),
                nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr, nullptr);
            sync_items_from_external_watcher();
        },
        nullptr,
        nullptr);

    g_sni.host_owner_id = g_bus_own_name_on_connection(
        g_sni.bus,
        g_sni.host_service_name.c_str(),
        G_BUS_NAME_OWNER_FLAGS_NONE,
        nullptr, nullptr, nullptr, nullptr);

    g_sni.name_owner_sub_id = g_dbus_connection_signal_subscribe(
        g_sni.bus,
        "org.freedesktop.DBus",
        "org.freedesktop.DBus",
        "NameOwnerChanged",
        "/org/freedesktop/DBus",
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        +[](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *,
            GVariant *params, gpointer) {
            if (!g_sni.initialized || !params) return;
            const gchar *name = nullptr;
            const gchar *old_owner = nullptr;
            const gchar *new_owner = nullptr;
            g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
            if (!name || (new_owner && *new_owner)) return;

            std::vector<std::string> stale;
            for (const auto &kv : g_sni.items) {
                if (kv.second->info.bus_name == name) stale.push_back(kv.first);
            }
            for (const auto &k : stale) unregister_sni_item(k);
        },
        nullptr,
        nullptr);

    g_sni.ext_watcher_sub_id = g_dbus_connection_signal_subscribe(
        g_sni.bus,
        kWatcherBusName,
        kWatcherIface,
        nullptr,
        kWatcherObjPath,
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        +[](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *sig,
            GVariant *params, gpointer) {
            if (!g_sni.initialized || !sig || !params) return;
            const gchar *svc = nullptr;
            g_variant_get(params, "(&s)", &svc);
            if (!svc || !*svc) return;
            if (std::strcmp(sig, "StatusNotifierItemRegistered") == 0) {
                register_sni_item(svc, "");
            } else if (std::strcmp(sig, "StatusNotifierItemUnregistered") == 0) {
                unregister_sni_item(svc);
            }
        },
        nullptr,
        nullptr);
}

void shutdown_sni_tray() {
    if (!g_sni.initialized) return;
    g_sni.initialized = false;
    g_sni.on_changed = nullptr;
    g_sni.on_changed_user = nullptr;
    if (g_sni.notify_idle_id != 0) {
        g_source_remove(g_sni.notify_idle_id);
        g_sni.notify_idle_id = 0;
    }
    if (g_sni.bus) {
        for (auto &kv : g_sni.items) {
            if (kv.second->props_sub_id != 0) {
                g_dbus_connection_signal_unsubscribe(g_sni.bus, kv.second->props_sub_id);
            }
        }
        g_sni.items.clear();
        if (g_sni.name_owner_sub_id != 0) {
            g_dbus_connection_signal_unsubscribe(g_sni.bus, g_sni.name_owner_sub_id);
            g_sni.name_owner_sub_id = 0;
        }
        if (g_sni.ext_watcher_sub_id != 0) {
            g_dbus_connection_signal_unsubscribe(g_sni.bus, g_sni.ext_watcher_sub_id);
            g_sni.ext_watcher_sub_id = 0;
        }
        if (g_sni.watcher_reg_id != 0) {
            g_dbus_connection_unregister_object(g_sni.bus, g_sni.watcher_reg_id);
            g_sni.watcher_reg_id = 0;
        }
        if (g_sni.watcher_owner_id != 0) {
            g_bus_unown_name(g_sni.watcher_owner_id);
            g_sni.watcher_owner_id = 0;
        }
        if (g_sni.host_owner_id != 0) {
            g_bus_unown_name(g_sni.host_owner_id);
            g_sni.host_owner_id = 0;
        }
        g_object_unref(g_sni.bus);
        g_sni.bus = nullptr;
    }
    if (g_sni.watcher_node) {
        g_dbus_node_info_unref(g_sni.watcher_node);
        g_sni.watcher_node = nullptr;
    }
    g_sni.hosts.clear();
}

std::vector<SniTrayItemSnapshot> list_sni_tray_items() {
    std::vector<SniTrayItemSnapshot> out;
    out.reserve(g_sni.items.size());
    for (const auto &kv : g_sni.items) {
        if (kv.second->info.status == "Passive") continue;
        out.push_back(kv.second->info);
    }
    std::stable_sort(out.begin(), out.end(), [](const SniTrayItemSnapshot &a, const SniTrayItemSnapshot &b) {
        std::string ak = a.id.empty() ? a.service_key : a.id;
        std::string bk = b.id.empty() ? b.service_key : b.id;
        return ak < bk;
    });
    return out;
}

void rebuild_sni_tray_box(GtkWidget *box, int icon_px) {
    if (!box) return;
    while (GtkWidget *c = gtk_widget_get_first_child(box)) {
        GtkWidget *old_pop = GTK_WIDGET(g_object_get_data(G_OBJECT(c), "active-tray-pop"));
        if (old_pop && GTK_IS_POPOVER(old_pop)) {
            gtk_popover_popdown(GTK_POPOVER(old_pop));
            gtk_widget_unparent(old_pop);
            g_object_set_data(G_OBJECT(c), "active-tray-pop", nullptr);
        }
        gtk_box_remove(GTK_BOX(box), c);
    }

    auto items = list_sni_tray_items();
    gtk_widget_set_visible(box, !items.empty());
    if (items.empty()) return;

    for (const auto &item : items) {
        GtkWidget *btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "spinel-bar-icon-btn");
        gtk_widget_add_css_class(btn, "spinel-tray-btn");
        if (item.status == "NeedsAttention") {
            gtk_widget_add_css_class(btn, "attention");
        }

        std::string tip = !item.tooltip_title.empty()
                              ? (item.tooltip_body.empty()
                                     ? item.tooltip_title
                                     : (item.tooltip_title + "\n" + item.tooltip_body))
                              : (!item.title.empty() ? item.title : item.id);
        if (!tip.empty()) {
            gtk_widget_set_tooltip_text(btn, tip.c_str());
        }

        gtk_button_set_child(GTK_BUTTON(btn), build_tray_icon_widget(item, icon_px));

        auto *ctx = new TrayActionCtx{
            item.bus_name, item.object_path, item.menu_path, item.item_is_menu, btn, nullptr
        };
        g_object_set_data_full(G_OBJECT(btn), "sni-ctx", ctx, +[](gpointer d) {
            delete static_cast<TrayActionCtx *>(d);
        });

        g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer) {
            auto *c = static_cast<TrayActionCtx *>(g_object_get_data(G_OBJECT(b), "sni-ctx"));
            if (!c) return;
            if (c->item_is_menu) {
                open_sni_menu_popover(c);
            } else {
                call_sni_simple_method(*c, "Activate", true);
            }
        }), nullptr);

        GtkGesture *rclick = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), GDK_BUTTON_SECONDARY);
        g_signal_connect(rclick, "pressed", G_CALLBACK(+[](GtkGestureClick *g, int, double, double, gpointer d) {
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
            auto *b = GTK_BUTTON(d);
            auto *c = static_cast<TrayActionCtx *>(g_object_get_data(G_OBJECT(b), "sni-ctx"));
            if (c) open_sni_menu_popover(c);
        }), btn);
        gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(rclick));

        GtkGesture *mclick = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(mclick), GDK_BUTTON_MIDDLE);
        g_signal_connect(mclick, "pressed", G_CALLBACK(+[](GtkGestureClick *g, int, double, double, gpointer d) {
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
            auto *b = GTK_BUTTON(d);
            auto *c = static_cast<TrayActionCtx *>(g_object_get_data(G_OBJECT(b), "sni-ctx"));
            if (c) call_sni_simple_method(*c, "SecondaryActivate", false);
        }), btn);
        gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(mclick));

        gtk_box_append(GTK_BOX(box), btn);
    }
}
