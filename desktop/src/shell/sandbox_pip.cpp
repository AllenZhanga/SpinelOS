#include "sandbox_pip.hpp"

#include "../common.hpp"
#include "../sandbox_manager.hpp"

extern "C" {
#include <glib-unix.h>
#include <gtk4-layer-shell.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

struct SandboxPipState {
    NativeShellState *ns = nullptr;

    GtkWindow *win = nullptr;
    GtkWidget *card = nullptr;
    GtkWidget *header_box = nullptr;
    GtkWidget *dot_lbl = nullptr;
    GtkWidget *title_lbl = nullptr;
    GtkWidget *net_lbl = nullptr;
    GtkWidget *btn_launch = nullptr;
    GtkWidget *btn_reset = nullptr;
    GtkWidget *btn_min = nullptr;
    GtkWidget *btn_expand = nullptr;
    GtkWidget *btn_stop = nullptr;
    GtkWidget *video_da = nullptr;

    bool visible = false;
    bool minimized = false;
    bool takeover = false;
    bool last_running = false;
    int margin_right = 18;
    int margin_bottom = 86;
    int drag_start_right = 18;
    int drag_start_bottom = 86;

    guint poll_timer_id = 0;
    guint capture_timer_id = 0;
    guint fd_watch_id = 0;

    // Wayland sandbox client objects
    struct wl_display *wl_dpy = nullptr;
    struct wl_registry *wl_reg = nullptr;
    struct wl_shm *wl_shm = nullptr;
    struct wl_output *wl_output = nullptr;
    struct wl_seat *wl_seat = nullptr;
    struct zwlr_screencopy_manager_v1 *sc_mgr = nullptr;
    struct zwlr_virtual_pointer_manager_v1 *vp_mgr = nullptr;
    struct zwlr_virtual_pointer_v1 *vptr = nullptr;
    struct zwp_virtual_keyboard_manager_v1 *vk_mgr = nullptr;
    struct zwp_virtual_keyboard_v1 *vkbd = nullptr;
    bool keymap_sent = false;

    // Screencopy SHM & rendered frame buffer
    bool capturing = false;
    struct zwlr_screencopy_frame_v1 *cur_frame = nullptr;
    uint32_t frame_format = 0;
    uint32_t frame_w = 0;
    uint32_t frame_h = 0;
    uint32_t frame_stride = 0;
    uint32_t frame_flags = 0;

    int shm_fd = -1;
    void *shm_map = nullptr;
    size_t shm_size = 0;
    struct wl_shm_pool *shm_pool = nullptr;
    struct wl_buffer *shm_buf = nullptr;
    uint32_t shm_w = 0;
    uint32_t shm_h = 0;
    uint32_t shm_stride = 0;
    uint32_t shm_format = 0;

    cairo_surface_t *surface = nullptr;
    int surface_w = 0;
    int surface_h = 0;
};

namespace {

uint32_t now_msec() {
    return static_cast<uint32_t>(g_get_monotonic_time() / 1000);
}

void release_shm_buffer(SandboxPipState *pip) {
    if (pip->shm_buf) {
        wl_buffer_destroy(pip->shm_buf);
        pip->shm_buf = nullptr;
    }
    if (pip->shm_pool) {
        wl_shm_pool_destroy(pip->shm_pool);
        pip->shm_pool = nullptr;
    }
    if (pip->shm_map && pip->shm_map != MAP_FAILED) {
        ::munmap(pip->shm_map, pip->shm_size);
        pip->shm_map = nullptr;
    }
    if (pip->shm_fd >= 0) {
        ::close(pip->shm_fd);
        pip->shm_fd = -1;
    }
    pip->shm_size = 0;
    pip->shm_w = 0;
    pip->shm_h = 0;
    pip->shm_stride = 0;
    pip->shm_format = 0;
}

bool ensure_shm_buffer(SandboxPipState *pip, uint32_t w, uint32_t h, uint32_t stride, uint32_t fmt) {
    if (!pip->wl_shm || w == 0 || h == 0 || stride == 0) return false;
    if (pip->shm_buf && pip->shm_w == w && pip->shm_h == h && pip->shm_stride == stride && pip->shm_format == fmt) {
        return true;
    }
    release_shm_buffer(pip);
    size_t sz = static_cast<size_t>(stride) * static_cast<size_t>(h);
    int fd = ::memfd_create("spinel-sandbox-frame", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (::ftruncate(fd, static_cast<off_t>(sz)) != 0) {
        ::close(fd);
        return false;
    }
    void *map = ::mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ::close(fd);
        return false;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(pip->wl_shm, fd, static_cast<int32_t>(sz));
    if (!pool) {
        ::munmap(map, sz);
        ::close(fd);
        return false;
    }
    struct wl_buffer *buf = wl_shm_pool_create_buffer(
        pool, 0, static_cast<int32_t>(w), static_cast<int32_t>(h), static_cast<int32_t>(stride), fmt);
    if (!buf) {
        wl_shm_pool_destroy(pool);
        ::munmap(map, sz);
        ::close(fd);
        return false;
    }
    pip->shm_fd = fd;
    pip->shm_map = map;
    pip->shm_size = sz;
    pip->shm_pool = pool;
    pip->shm_buf = buf;
    pip->shm_w = w;
    pip->shm_h = h;
    pip->shm_stride = stride;
    pip->shm_format = fmt;
    return true;
}

void send_virtual_keymap(SandboxPipState *pip) {
    if (!pip || !pip->vkbd || pip->keymap_sent) return;
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) return;
    struct xkb_rule_names names{};
    names.layout = "us";
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        xkb_context_unref(ctx);
        return;
    }
    char *str = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (str) {
        size_t len = std::strlen(str) + 1;
        int fd = ::memfd_create("spinel-sandbox-keymap", MFD_CLOEXEC);
        if (fd >= 0) {
            if (::ftruncate(fd, static_cast<off_t>(len)) == 0) {
                size_t off = 0;
                while (off < len) {
                    ssize_t w = ::write(fd, str + off, len - off);
                    if (w <= 0) break;
                    off += static_cast<size_t>(w);
                }
                ::lseek(fd, 0, SEEK_SET);
                zwp_virtual_keyboard_v1_keymap(
                    pip->vkbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, static_cast<uint32_t>(len));
                pip->keymap_sent = true;
            }
            ::close(fd);
        }
        std::free(str);
    }
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);
}

void copy_shm_to_surface(SandboxPipState *pip) {
    if (!pip || !pip->shm_map || pip->frame_w == 0 || pip->frame_h == 0) return;
    int w = static_cast<int>(pip->frame_w);
    int h = static_cast<int>(pip->frame_h);
    if (!pip->surface || pip->surface_w != w || pip->surface_h != h) {
        if (pip->surface) cairo_surface_destroy(pip->surface);
        pip->surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
        pip->surface_w = w;
        pip->surface_h = h;
    }
    if (!pip->surface || cairo_surface_status(pip->surface) != CAIRO_STATUS_SUCCESS) return;

    cairo_surface_flush(pip->surface);
    unsigned char *dst = cairo_image_surface_get_data(pip->surface);
    int dst_stride = cairo_image_surface_get_stride(pip->surface);
    const unsigned char *src = static_cast<const unsigned char *>(pip->shm_map);
    int src_stride = static_cast<int>(pip->frame_stride);
    bool y_invert = (pip->frame_flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
    bool is_bgr = (pip->frame_format == WL_SHM_FORMAT_XBGR8888 || pip->frame_format == WL_SHM_FORMAT_ABGR8888);

    for (int y = 0; y < h; ++y) {
        int sy = y_invert ? (h - 1 - y) : y;
        const uint32_t *srow = reinterpret_cast<const uint32_t *>(src + static_cast<size_t>(sy) * src_stride);
        uint32_t *drow = reinterpret_cast<uint32_t *>(dst + static_cast<size_t>(y) * dst_stride);
        if (!is_bgr) {
            std::memcpy(drow, srow, static_cast<size_t>(w) * 4);
        } else {
            for (int x = 0; x < w; ++x) {
                uint32_t p = srow[x];
                uint32_t r = (p & 0x000000FFu) << 16;
                uint32_t g = (p & 0x0000FF00u);
                uint32_t b = (p & 0x00FF0000u) >> 16;
                drow[x] = 0xFF000000u | r | g | b;
            }
        }
    }
    cairo_surface_mark_dirty(pip->surface);
}

void on_frame_buffer(void *data,
                     struct zwlr_screencopy_frame_v1 *frame,
                     uint32_t format,
                     uint32_t width,
                     uint32_t height,
                     uint32_t stride) {
    auto *pip = static_cast<SandboxPipState *>(data);
    if (!pip || frame != pip->cur_frame) return;
    pip->frame_format = format;
    pip->frame_w = width;
    pip->frame_h = height;
    pip->frame_stride = stride;
    pip->frame_flags = 0;
    if (ensure_shm_buffer(pip, width, height, stride, format)) {
        zwlr_screencopy_frame_v1_copy(frame, pip->shm_buf);
    } else {
        zwlr_screencopy_frame_v1_destroy(frame);
        pip->cur_frame = nullptr;
        pip->capturing = false;
    }
}

void on_frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags) {
    auto *pip = static_cast<SandboxPipState *>(data);
    if (!pip || frame != pip->cur_frame) return;
    pip->frame_flags = flags;
}

void on_frame_ready(void *data,
                    struct zwlr_screencopy_frame_v1 *frame,
                    uint32_t,
                    uint32_t,
                    uint32_t) {
    auto *pip = static_cast<SandboxPipState *>(data);
    if (!pip || frame != pip->cur_frame) return;
    copy_shm_to_surface(pip);
    zwlr_screencopy_frame_v1_destroy(frame);
    pip->cur_frame = nullptr;
    pip->capturing = false;
    if (pip->video_da) gtk_widget_queue_draw(pip->video_da);
}

void on_frame_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) {
    auto *pip = static_cast<SandboxPipState *>(data);
    if (!pip || frame != pip->cur_frame) return;
    zwlr_screencopy_frame_v1_destroy(frame);
    pip->cur_frame = nullptr;
    pip->capturing = false;
}

const struct zwlr_screencopy_frame_v1_listener kFrameListener = {
    on_frame_buffer,
    on_frame_flags,
    on_frame_ready,
    on_frame_failed,
    nullptr,  // damage (since 2, unused at v1)
    nullptr,  // linux_dmabuf (since 3, unused at v1)
    nullptr,  // buffer_done (since 3, unused at v1)
};

void on_reg_global(void *data,
                   struct wl_registry *reg,
                   uint32_t name,
                   const char *iface,
                   uint32_t version) {
    auto *pip = static_cast<SandboxPipState *>(data);
    if (!pip || !iface) return;
    if (std::strcmp(iface, wl_shm_interface.name) == 0 && !pip->wl_shm) {
        pip->wl_shm = static_cast<struct wl_shm *>(
            wl_registry_bind(reg, name, &wl_shm_interface, 1));
    } else if (std::strcmp(iface, wl_output_interface.name) == 0 && !pip->wl_output) {
        pip->wl_output = static_cast<struct wl_output *>(
            wl_registry_bind(reg, name, &wl_output_interface, std::min(version, 2u)));
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0 && !pip->wl_seat) {
        pip->wl_seat = static_cast<struct wl_seat *>(
            wl_registry_bind(reg, name, &wl_seat_interface, 1));
    } else if (std::strcmp(iface, zwlr_screencopy_manager_v1_interface.name) == 0 && !pip->sc_mgr) {
        pip->sc_mgr = static_cast<struct zwlr_screencopy_manager_v1 *>(
            wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, 1));
    } else if (std::strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0 && !pip->vp_mgr) {
        pip->vp_mgr = static_cast<struct zwlr_virtual_pointer_manager_v1 *>(
            wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface, 1));
    } else if (std::strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0 && !pip->vk_mgr) {
        pip->vk_mgr = static_cast<struct zwp_virtual_keyboard_manager_v1 *>(
            wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
    }
}

void on_reg_remove(void *, struct wl_registry *, uint32_t) {}

const struct wl_registry_listener kRegListener = {
    on_reg_global,
    on_reg_remove,
};

void disconnect_sandbox_wl(SandboxPipState *pip) {
    if (!pip) return;
    if (pip->capture_timer_id > 0) {
        g_source_remove(pip->capture_timer_id);
        pip->capture_timer_id = 0;
    }
    if (pip->fd_watch_id > 0) {
        g_source_remove(pip->fd_watch_id);
        pip->fd_watch_id = 0;
    }
    if (pip->cur_frame) {
        zwlr_screencopy_frame_v1_destroy(pip->cur_frame);
        pip->cur_frame = nullptr;
    }
    pip->capturing = false;
    release_shm_buffer(pip);
    if (pip->vptr) {
        zwlr_virtual_pointer_v1_destroy(pip->vptr);
        pip->vptr = nullptr;
    }
    if (pip->vp_mgr) {
        zwlr_virtual_pointer_manager_v1_destroy(pip->vp_mgr);
        pip->vp_mgr = nullptr;
    }
    if (pip->vkbd) {
        zwp_virtual_keyboard_v1_destroy(pip->vkbd);
        pip->vkbd = nullptr;
    }
    if (pip->vk_mgr) {
        zwp_virtual_keyboard_manager_v1_destroy(pip->vk_mgr);
        pip->vk_mgr = nullptr;
    }
    pip->keymap_sent = false;
    if (pip->sc_mgr) {
        zwlr_screencopy_manager_v1_destroy(pip->sc_mgr);
        pip->sc_mgr = nullptr;
    }
    if (pip->wl_output) {
        wl_output_destroy(pip->wl_output);
        pip->wl_output = nullptr;
    }
    if (pip->wl_seat) {
        wl_seat_destroy(pip->wl_seat);
        pip->wl_seat = nullptr;
    }
    if (pip->wl_shm) {
        wl_shm_destroy(pip->wl_shm);
        pip->wl_shm = nullptr;
    }
    if (pip->wl_reg) {
        wl_registry_destroy(pip->wl_reg);
        pip->wl_reg = nullptr;
    }
    if (pip->wl_dpy) {
        wl_display_disconnect(pip->wl_dpy);
        pip->wl_dpy = nullptr;
    }
}

gboolean on_wl_fd_ready(gint, GIOCondition cond, gpointer data) {
    auto *pip = static_cast<SandboxPipState *>(data);
    if (!pip || !pip->wl_dpy) return G_SOURCE_REMOVE;
    if (cond & (G_IO_ERR | G_IO_HUP)) {
        pip->fd_watch_id = 0;
        disconnect_sandbox_wl(pip);
        return G_SOURCE_REMOVE;
    }
    if (wl_display_dispatch(pip->wl_dpy) < 0) {
        pip->fd_watch_id = 0;
        disconnect_sandbox_wl(pip);
        return G_SOURCE_REMOVE;
    }
    wl_display_flush(pip->wl_dpy);
    return G_SOURCE_CONTINUE;
}

bool ensure_sandbox_wl_connected(SandboxPipState *pip) {
    if (!pip) return false;
    if (pip->wl_dpy) return true;

    std::string sock = sandbox_runtime_dir() + "/" + sandbox_socket_name();
    struct wl_display *dpy = wl_display_connect(sock.c_str());
    if (!dpy) return false;

    pip->wl_dpy = dpy;
    pip->wl_reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(pip->wl_reg, &kRegListener, pip);
    wl_display_roundtrip(dpy);

    if (pip->vp_mgr && !pip->vptr) {
        pip->vptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pip->vp_mgr, pip->wl_seat);
    }
    if (pip->vk_mgr && pip->wl_seat && !pip->vkbd) {
        pip->vkbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(pip->vk_mgr, pip->wl_seat);
        send_virtual_keymap(pip);
    }
    wl_display_flush(dpy);

    int fd = wl_display_get_fd(dpy);
    if (fd >= 0) {
        pip->fd_watch_id = g_unix_fd_add(fd, static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP),
                                         on_wl_fd_ready, pip);
    }
    return pip->sc_mgr != nullptr && pip->wl_output != nullptr;
}

void trigger_screencopy_frame(SandboxPipState *pip) {
    if (!pip || !pip->visible || pip->minimized || pip->capturing) return;
    if (!ensure_sandbox_wl_connected(pip)) return;
    if (!pip->sc_mgr || !pip->wl_output || !pip->wl_dpy) return;

    pip->cur_frame = zwlr_screencopy_manager_v1_capture_output(pip->sc_mgr, 1, pip->wl_output);
    if (!pip->cur_frame) return;
    pip->capturing = true;
    zwlr_screencopy_frame_v1_add_listener(pip->cur_frame, &kFrameListener, pip);
    wl_display_flush(pip->wl_dpy);
}

void refresh_capture_timer(SandboxPipState *pip) {
    if (!pip) return;
    if (pip->capture_timer_id > 0) {
        g_source_remove(pip->capture_timer_id);
        pip->capture_timer_id = 0;
    }
    if (!pip->visible || pip->minimized || !pip->last_running) return;
    guint interval_ms = pip->takeover ? 33u : 66u;
    pip->capture_timer_id = g_timeout_add(interval_ms, +[](gpointer d) -> gboolean {
        trigger_screencopy_frame(static_cast<SandboxPipState *>(d));
        return G_SOURCE_CONTINUE;
    }, pip);
    trigger_screencopy_frame(pip);
}

void apply_pip_window_geometry(SandboxPipState *pip) {
    if (!pip || !pip->win) return;
    GtkWindow *win = pip->win;

    if (pip->takeover) {
        gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_BOTTOM, FALSE);
        gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, FALSE);
        gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
        gtk_widget_set_size_request(pip->video_da, 1024, 576);
        gtk_widget_add_css_class(pip->card, "is-takeover");
        gtk_button_set_label(GTK_BUTTON(pip->btn_expand), "缩小画中画");
        gtk_widget_set_visible(pip->btn_launch, TRUE);
        gtk_widget_set_visible(pip->btn_reset, TRUE);
        gtk_label_set_text(GTK_LABEL(pip->title_lbl), "正在操控平行桌面 · 按 Ctrl+Shift+Esc 退出接管");
    } else {
        gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_RIGHT, pip->margin_right);
        gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_BOTTOM, pip->margin_bottom);
        gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        gtk_widget_set_size_request(pip->video_da, 360, 202);
        gtk_widget_remove_css_class(pip->card, "is-takeover");
        gtk_button_set_label(GTK_BUTTON(pip->btn_expand), "放大接管");
        gtk_widget_set_visible(pip->btn_launch, FALSE);
        gtk_widget_set_visible(pip->btn_reset, FALSE);
        gtk_label_set_text(GTK_LABEL(pip->title_lbl), "平行沙箱桌面");
    }

    SandboxConfig cfg = load_sandbox_config();
    gtk_label_set_text(GTK_LABEL(pip->net_lbl), cfg.allow_network ? "共享网络" : "离线隔离");

    bool should_show = pip->visible && !pip->minimized && pip->last_running;
    gtk_widget_set_visible(GTK_WIDGET(win), should_show ? TRUE : FALSE);
    if (should_show) {
        gtk_window_present(win);
        if (pip->takeover && pip->video_da) {
            gtk_widget_grab_focus(pip->video_da);
        }
    }
    refresh_capture_timer(pip);
}

void draw_pip_video_cb(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer data) {
    auto *pip = static_cast<SandboxPipState *>(data);
    if (!pip || w <= 0 || h <= 0) return;

    cairo_save(cr);
    shell_internal::cairo_rounded_rect(cr, 0, 0, w, h, 10.0);
    cairo_clip(cr);

    cairo_set_source_rgb(cr, 0.06, 0.08, 0.14);
    cairo_paint(cr);

    if (pip->surface && pip->surface_w > 0 && pip->surface_h > 0) {
        double sx = static_cast<double>(w) / static_cast<double>(pip->surface_w);
        double sy = static_cast<double>(h) / static_cast<double>(pip->surface_h);
        double scale = std::min(sx, sy);
        double dw = pip->surface_w * scale;
        double dh = pip->surface_h * scale;
        double ox = (w - dw) * 0.5;
        double oy = (h - dh) * 0.5;
        cairo_translate(cr, ox, oy);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, pip->surface, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
    } else {
        cairo_set_source_rgba(cr, 0.78, 0.85, 0.96, 0.82);
        cairo_select_font_face(cr, "Noto Sans CJK SC", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, pip->takeover ? 15.0 : 12.0);
        const char *hint = "平行桌面已就绪 · 点击下方或在“+ 启动应用”中运行程序";
        if (!pip->takeover) hint = "平行桌面等待窗口打开…";
        cairo_text_extents_t ext{};
        cairo_text_extents(cr, hint, &ext);
        cairo_move_to(cr, (w - ext.width) * 0.5, (h + ext.height) * 0.5);
        cairo_show_text(cr, hint);
    }
    cairo_restore(cr);
}

bool compute_video_norm_coords(SandboxPipState *pip, double wx, double wy, uint32_t &out_x, uint32_t &out_y) {
    if (!pip || !pip->video_da) return false;
    int w = gtk_widget_get_width(pip->video_da);
    int h = gtk_widget_get_height(pip->video_da);
    if (w <= 0 || h <= 0) return false;

    double fw = pip->surface_w > 0 ? pip->surface_w : 1600.0;
    double fh = pip->surface_h > 0 ? pip->surface_h : 900.0;
    double scale = std::min(static_cast<double>(w) / fw, static_cast<double>(h) / fh);
    double dw = fw * scale;
    double dh = fh * scale;
    double ox = (w - dw) * 0.5;
    double oy = (h - dh) * 0.5;
    double nx = std::clamp((wx - ox) / std::max(1.0, dw), 0.0, 1.0);
    double ny = std::clamp((wy - oy) / std::max(1.0, dh), 0.0, 1.0);
    out_x = static_cast<uint32_t>(std::round(nx * 10000.0));
    out_y = static_cast<uint32_t>(std::round(ny * 10000.0));
    return true;
}

uint32_t gdk_mods_to_xkb(GdkModifierType st) {
    uint32_t m = 0;
    if (st & GDK_SHIFT_MASK) m |= (1u << 0);
    if (st & GDK_LOCK_MASK) m |= (1u << 1);
    if (st & GDK_CONTROL_MASK) m |= (1u << 2);
    if (st & GDK_ALT_MASK) m |= (1u << 3);
    if (st & GDK_SUPER_MASK) m |= (1u << 6);
    return m;
}

uint32_t gdk_button_to_evdev(guint btn) {
    switch (btn) {
    case 1: return BTN_LEFT;
    case 2: return BTN_MIDDLE;
    case 3: return BTN_RIGHT;
    case 8: return BTN_SIDE;
    case 9: return BTN_EXTRA;
    default: return BTN_LEFT;
    }
}

void build_launch_popover(SandboxPipState *pip) {
    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
    gtk_widget_add_css_class(pop, "spinel-ctx-popover");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);

    struct AppItem {
        const char *label;
        const char *bin;
    };
    static const AppItem kApps[] = {
        {"Spinel 终端", "spinel-terminal"},
        {"Spinel 文件管理器", "spinel-files"},
        {"Google Chrome", "google-chrome-stable"},
        {"Chromium 浏览器", "chromium"},
        {"Antigravity", "antigravity"},
        {"VS Code", "code"},
    };

    for (const auto &app : kApps) {
        if (!has_executable(app.bin)) continue;
        GtkWidget *b = gtk_button_new_with_label(app.label);
        gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
        gtk_widget_add_css_class(b, "spinel-ctx-item");
        g_object_set_data_full(G_OBJECT(b), "sb-bin", g_strdup(app.bin), g_free);
        g_object_set_data(G_OBJECT(b), "sb-pop", pop);
        g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton *btn, gpointer) {
            const char *bin = static_cast<const char *>(g_object_get_data(G_OBJECT(btn), "sb-bin"));
            GtkWidget *p = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(btn), "sb-pop"));
            if (p) gtk_popover_popdown(GTK_POPOVER(p));
            if (bin && *bin) {
                std::string err;
                launch_in_sandbox({bin}, "", err);
            }
        }), nullptr);
        gtk_box_append(GTK_BOX(box), b);
    }

    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    GtkWidget *cfg_b = gtk_button_new_with_label("目录授权与网络设置…");
    gtk_button_set_has_frame(GTK_BUTTON(cfg_b), FALSE);
    gtk_widget_add_css_class(cfg_b, "spinel-ctx-item");
    g_object_set_data(G_OBJECT(cfg_b), "sb-pop", pop);
    g_signal_connect(cfg_b, "clicked", G_CALLBACK(+[](GtkButton *btn, gpointer d) {
        auto *p = static_cast<SandboxPipState *>(d);
        GtkWidget *pop_w = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(btn), "sb-pop"));
        if (pop_w) gtk_popover_popdown(GTK_POPOVER(pop_w));
        if (p) {
            p->takeover = false;
            apply_pip_window_geometry(p);
            if (p->ns && p->ns->app_state) {
                shell_internal::trigger_cmd(p->ns->app_state, "settings:open:perms");
            }
        }
    }), pip);
    gtk_box_append(GTK_BOX(box), cfg_b);

    gtk_popover_set_child(GTK_POPOVER(pop), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(pip->btn_launch), pop);
}

void sync_pip_state(SandboxPipState *pip) {
    if (!pip) return;
    SandboxStatus st = query_sandbox_status();
    bool changed = (st.running != pip->last_running);
    pip->last_running = st.running;
    if (!st.running) {
        pip->takeover = false;
        pip->visible = false;
        disconnect_sandbox_wl(pip);
        if (pip->surface) {
            cairo_surface_destroy(pip->surface);
            pip->surface = nullptr;
            pip->surface_w = 0;
            pip->surface_h = 0;
        }
        apply_pip_window_geometry(pip);
    } else if (changed) {
        apply_pip_window_geometry(pip);
    }
    if (pip->ns && pip->ns->bar_sandbox_btn && pip->ns->bar_sandbox_lbl) {
        gtk_widget_set_visible(pip->ns->bar_sandbox_btn, st.running ? TRUE : FALSE);
        if (st.running) {
            SandboxConfig cfg = load_sandbox_config();
            std::string txt = "沙箱";
            if (!cfg.allow_network) txt += " · 离线";
            else if (pip->minimized) txt += " · 后台";
            gtk_label_set_text(GTK_LABEL(pip->ns->bar_sandbox_lbl), txt.c_str());
        }
    }
}

}  // namespace

void init_sandbox_pip(NativeShellState *ns) {
    if (!ns || ns->sandbox_pip) return;
    auto *pip = new SandboxPipState();
    pip->ns = ns;
    ns->sandbox_pip = pip;

    GtkWindow *win = GTK_WINDOW(gtk_window_new());
    gtk_window_set_decorated(win, FALSE);
    gtk_window_set_resizable(win, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(win), "spinel-transparent-win");
    gtk_layer_init_for_window(win);
    gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(win, "spinel-sandbox-pip");
    gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_RIGHT, pip->margin_right);
    gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_BOTTOM, pip->margin_bottom);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(outer, 16);
    gtk_widget_set_margin_bottom(outer, 18);
    gtk_widget_set_margin_start(outer, 16);
    gtk_widget_set_margin_end(outer, 16);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card, "spinel-sandbox-pip-card");

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(header, "spinel-sandbox-pip-head");

    GtkWidget *dot = gtk_label_new("●");
    gtk_widget_add_css_class(dot, "spinel-sandbox-dot");
    GtkWidget *title = gtk_label_new("平行沙箱桌面");
    gtk_widget_add_css_class(title, "spinel-sandbox-title");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);

    GtkWidget *net_lbl = gtk_label_new("共享网络");
    gtk_widget_add_css_class(net_lbl, "spinel-sandbox-badge");

    GtkWidget *btn_launch = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(btn_launch), "+ 启动应用");
    gtk_widget_add_css_class(btn_launch, "spinel-sandbox-btn");
    gtk_widget_set_visible(btn_launch, FALSE);

    GtkWidget *btn_reset = gtk_button_new_with_label("重置");
    gtk_widget_add_css_class(btn_reset, "spinel-sandbox-btn");
    gtk_widget_set_visible(btn_reset, FALSE);

    GtkWidget *btn_expand = gtk_button_new_with_label("放大接管");
    gtk_widget_add_css_class(btn_expand, "spinel-sandbox-btn");

    GtkWidget *btn_min = gtk_button_new_with_label("收起");
    gtk_widget_add_css_class(btn_min, "spinel-sandbox-btn");

    GtkWidget *btn_stop = gtk_button_new_with_label("停止");
    gtk_widget_add_css_class(btn_stop, "spinel-sandbox-btn");
    gtk_widget_add_css_class(btn_stop, "danger");

    gtk_box_append(GTK_BOX(header), dot);
    gtk_box_append(GTK_BOX(header), title);
    gtk_box_append(GTK_BOX(header), net_lbl);
    gtk_box_append(GTK_BOX(header), btn_launch);
    gtk_box_append(GTK_BOX(header), btn_reset);
    gtk_box_append(GTK_BOX(header), btn_expand);
    gtk_box_append(GTK_BOX(header), btn_min);
    gtk_box_append(GTK_BOX(header), btn_stop);

    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, 360, 202);
    gtk_widget_set_focusable(da, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_pip_video_cb, pip, nullptr);

    gtk_box_append(GTK_BOX(card), header);
    gtk_box_append(GTK_BOX(card), da);
    gtk_box_append(GTK_BOX(outer), card);
    gtk_window_set_child(win, outer);
    gtk_widget_set_visible(GTK_WIDGET(win), FALSE);

    pip->win = win;
    pip->card = card;
    pip->header_box = header;
    pip->dot_lbl = dot;
    pip->title_lbl = title;
    pip->net_lbl = net_lbl;
    pip->btn_launch = btn_launch;
    pip->btn_reset = btn_reset;
    pip->btn_min = btn_min;
    pip->btn_expand = btn_expand;
    pip->btn_stop = btn_stop;
    pip->video_da = da;

    build_launch_popover(pip);

    // Drag gesture on compact PiP header
    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(+[](GtkGestureDrag *, double, double, gpointer d) {
        auto *p = static_cast<SandboxPipState *>(d);
        p->drag_start_right = p->margin_right;
        p->drag_start_bottom = p->margin_bottom;
    }), pip);
    g_signal_connect(drag, "drag-update", G_CALLBACK(+[](GtkGestureDrag *, double dx, double dy, gpointer d) {
        auto *p = static_cast<SandboxPipState *>(d);
        if (!p || p->takeover || !p->win) return;
        p->margin_right = std::clamp(p->drag_start_right - static_cast<int>(std::round(dx)), 8, 1400);
        p->margin_bottom = std::clamp(p->drag_start_bottom - static_cast<int>(std::round(dy)), 12, 900);
        gtk_layer_set_margin(p->win, GTK_LAYER_SHELL_EDGE_RIGHT, p->margin_right);
        gtk_layer_set_margin(p->win, GTK_LAYER_SHELL_EDGE_BOTTOM, p->margin_bottom);
    }), pip);
    gtk_widget_add_controller(header, GTK_EVENT_CONTROLLER(drag));

    // Buttons
    g_signal_connect(btn_expand, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        auto *p = static_cast<SandboxPipState *>(d);
        p->takeover = !p->takeover;
        apply_pip_window_geometry(p);
    }), pip);
    g_signal_connect(btn_min, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        auto *p = static_cast<SandboxPipState *>(d);
        p->takeover = false;
        p->minimized = true;
        apply_pip_window_geometry(p);
        sync_pip_state(p);
    }), pip);
    g_signal_connect(btn_stop, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        auto *p = static_cast<SandboxPipState *>(d);
        std::string err;
        stop_sandbox_session(err);
        sync_pip_state(p);
    }), pip);
    g_signal_connect(btn_reset, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        auto *p = static_cast<SandboxPipState *>(d);
        std::string err;
        reset_sandbox_home(err);
        sync_pip_state(p);
    }), pip);

    // Mouse / Pointer forwarding in takeover mode, or click compact preview -> takeover
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(+[](GtkEventControllerMotion *, double x, double y, gpointer d) {
        auto *p = static_cast<SandboxPipState *>(d);
        if (!p || !p->takeover || !p->vptr || !p->wl_dpy) return;
        uint32_t nx = 0;
        uint32_t ny = 0;
        if (compute_video_norm_coords(p, x, y, nx, ny)) {
            zwlr_virtual_pointer_v1_motion_absolute(p->vptr, now_msec(), nx, ny, 10000, 10000);
            zwlr_virtual_pointer_v1_frame(p->vptr);
            wl_display_flush(p->wl_dpy);
        }
    }), pip);
    gtk_widget_add_controller(da, motion);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick *g, int, double x, double y, gpointer d) {
        auto *p = static_cast<SandboxPipState *>(d);
        if (!p) return;
        if (!p->takeover) {
            p->takeover = true;
            apply_pip_window_geometry(p);
            return;
        }
        gtk_widget_grab_focus(p->video_da);
        if (!p->vptr || !p->wl_dpy) return;
        uint32_t nx = 0;
        uint32_t ny = 0;
        if (compute_video_norm_coords(p, x, y, nx, ny)) {
            zwlr_virtual_pointer_v1_motion_absolute(p->vptr, now_msec(), nx, ny, 10000, 10000);
        }
        guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
        zwlr_virtual_pointer_v1_button(p->vptr, now_msec(), gdk_button_to_evdev(btn), WL_POINTER_BUTTON_STATE_PRESSED);
        zwlr_virtual_pointer_v1_frame(p->vptr);
        wl_display_flush(p->wl_dpy);
    }), pip);
    g_signal_connect(click, "released", G_CALLBACK(+[](GtkGestureClick *g, int, double, double, gpointer d) {
        auto *p = static_cast<SandboxPipState *>(d);
        if (!p || !p->takeover || !p->vptr || !p->wl_dpy) return;
        guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
        zwlr_virtual_pointer_v1_button(p->vptr, now_msec(), gdk_button_to_evdev(btn), WL_POINTER_BUTTON_STATE_RELEASED);
        zwlr_virtual_pointer_v1_frame(p->vptr);
        wl_display_flush(p->wl_dpy);
    }), pip);
    gtk_widget_add_controller(da, GTK_EVENT_CONTROLLER(click));

    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll", G_CALLBACK(+[](GtkEventControllerScroll *, double dx, double dy, gpointer d) -> gboolean {
        auto *p = static_cast<SandboxPipState *>(d);
        if (!p || !p->takeover || !p->vptr || !p->wl_dpy) return FALSE;
        uint32_t t = now_msec();
        if (std::abs(dy) > 1e-4) {
            zwlr_virtual_pointer_v1_axis(p->vptr, t, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(dy * 15.0));
        }
        if (std::abs(dx) > 1e-4) {
            zwlr_virtual_pointer_v1_axis(p->vptr, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(dx * 15.0));
        }
        zwlr_virtual_pointer_v1_frame(p->vptr);
        wl_display_flush(p->wl_dpy);
        return TRUE;
    }), pip);
    gtk_widget_add_controller(da, scroll);

    // Keyboard forwarding during takeover mode (Ctrl+Shift+Esc exits takeover cleanly)
    GtkEventController *key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey *, guint keyval, guint keycode, GdkModifierType mods, gpointer d) -> gboolean {
        auto *p = static_cast<SandboxPipState *>(d);
        if (!p || !p->takeover) return FALSE;
        if (keyval == GDK_KEY_Escape && (mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
            if (p->vkbd && p->wl_dpy) {
                zwp_virtual_keyboard_v1_modifiers(p->vkbd, 0, 0, 0, 0);
                wl_display_flush(p->wl_dpy);
            }
            p->takeover = false;
            apply_pip_window_geometry(p);
            return TRUE;
        }
        if (!p->vkbd || !p->wl_dpy) return FALSE;
        send_virtual_keymap(p);
        uint32_t evdev = keycode >= 8 ? keycode - 8 : keycode;
        zwp_virtual_keyboard_v1_modifiers(p->vkbd, gdk_mods_to_xkb(mods), 0, 0, 0);
        zwp_virtual_keyboard_v1_key(p->vkbd, now_msec(), evdev, WL_KEYBOARD_KEY_STATE_PRESSED);
        wl_display_flush(p->wl_dpy);
        return TRUE;
    }), pip);
    g_signal_connect(key, "key-released", G_CALLBACK(+[](GtkEventControllerKey *, guint, guint keycode, GdkModifierType mods, gpointer d) {
        auto *p = static_cast<SandboxPipState *>(d);
        if (!p || !p->takeover || !p->vkbd || !p->wl_dpy) return;
        uint32_t evdev = keycode >= 8 ? keycode - 8 : keycode;
        zwp_virtual_keyboard_v1_modifiers(p->vkbd, gdk_mods_to_xkb(mods), 0, 0, 0);
        zwp_virtual_keyboard_v1_key(p->vkbd, now_msec(), evdev, WL_KEYBOARD_KEY_STATE_RELEASED);
        wl_display_flush(p->wl_dpy);
    }), pip);
    gtk_widget_add_controller(GTK_WIDGET(win), key);

    pip->poll_timer_id = g_timeout_add_seconds(2, +[](gpointer d) -> gboolean {
        sync_pip_state(static_cast<SandboxPipState *>(d));
        return G_SOURCE_CONTINUE;
    }, pip);
    sync_pip_state(pip);
}

void shutdown_sandbox_pip(NativeShellState *ns) {
    if (!ns || !ns->sandbox_pip) return;
    SandboxPipState *pip = ns->sandbox_pip;
    ns->sandbox_pip = nullptr;
    if (pip->poll_timer_id > 0) {
        g_source_remove(pip->poll_timer_id);
        pip->poll_timer_id = 0;
    }
    disconnect_sandbox_wl(pip);
    if (pip->surface) {
        cairo_surface_destroy(pip->surface);
        pip->surface = nullptr;
    }
    if (pip->win) {
        gtk_window_destroy(pip->win);
        pip->win = nullptr;
    }
    delete pip;
}

void sandbox_pip_sync_action(NativeShellState *ns, const std::string &action) {
    if (!ns || !ns->sandbox_pip) return;
    SandboxPipState *pip = ns->sandbox_pip;
    SandboxStatus st = query_sandbox_status();
    pip->last_running = st.running;

    if (action == "show") {
        if (st.running) {
            pip->visible = true;
            pip->minimized = false;
        }
    } else if (action == "hide") {
        pip->takeover = false;
        pip->minimized = true;
    } else if (action == "toggle") {
        if (!st.running) {
            std::thread([ns]() {
                std::string err;
                if (start_sandbox_session(err)) {
                    g_idle_add(+[](gpointer d) -> gboolean {
                        auto *n = static_cast<NativeShellState *>(d);
                        if (n && n->sandbox_pip) sandbox_pip_sync_action(n, "show");
                        return G_SOURCE_REMOVE;
                    }, ns);
                }
            }).detach();
            return;
        } else if (pip->visible && !pip->minimized) {
            pip->takeover = false;
            pip->minimized = true;
        } else {
            pip->visible = true;
            pip->minimized = false;
        }
    }
    apply_pip_window_geometry(pip);
    sync_pip_state(pip);
}

void sandbox_pip_on_theme_changed(NativeShellState *ns) {
    if (!ns || !ns->sandbox_pip) return;
    sync_pip_state(ns->sandbox_pip);
}

std::string sandbox_pip_css(const NativeShellState *ns) {
    const ThemeSpec &t = ns->theme;
    const char *pill_bg = t.is_light ? "rgba(15, 23, 42, 0.08)" : "rgba(255, 255, 255, 0.12)";
    const char *hover_bg = t.is_light ? "rgba(15, 23, 42, 0.14)" : "rgba(255, 255, 255, 0.20)";
    const char *sub_fg = t.is_light ? "rgba(15, 23, 42, 0.65)" : "rgba(226, 232, 240, 0.72)";
    return
        ".spinel-sandbox-pip-card {\n"
        "  background-color: " + t.popover_bg + ";\n"
        "  border: 1px solid " + t.cc_border + ";\n"
        "  border-radius: 14px;\n"
        "  padding: 8px;\n"
        "  box-shadow: 0 12px 30px rgba(0, 0, 0, 0.42);\n"
        "}\n"
        ".spinel-sandbox-pip-card.is-takeover {\n"
        "  border: 1.5px solid #22c55e;\n"
        "  box-shadow: 0 18px 42px rgba(0, 0, 0, 0.56);\n"
        "}\n"
        ".spinel-sandbox-pip-head { min-height: 24px; }\n"
        ".spinel-sandbox-dot { color: #22c55e; font-size: 11px; }\n"
        ".spinel-sandbox-title { color: " + t.bar_fg + "; font-size: 12px; font-weight: 600; }\n"
        ".spinel-sandbox-badge {\n"
        "  color: " + std::string(sub_fg) + ";\n"
        "  background-color: " + std::string(pill_bg) + ";\n"
        "  border-radius: 999px;\n"
        "  padding: 2px 8px;\n"
        "  font-size: 11px;\n"
        "}\n"
        ".spinel-sandbox-btn {\n"
        "  min-height: 22px;\n"
        "  padding: 2px 8px;\n"
        "  border-radius: 7px;\n"
        "  font-size: 11px;\n"
        "  background-color: " + std::string(pill_bg) + ";\n"
        "  color: " + t.bar_fg + ";\n"
        "  border: 1px solid " + t.cc_border + ";\n"
        "}\n"
        ".spinel-sandbox-btn:hover { background-color: " + std::string(hover_bg) + "; }\n"
        ".spinel-sandbox-btn.danger:hover { color: #ef4444; }\n";
}

bool sandbox_pip_is_running(const NativeShellState *ns) {
    return ns && ns->sandbox_pip && ns->sandbox_pip->last_running;
}

bool sandbox_pip_is_visible(const NativeShellState *ns) {
    return ns && ns->sandbox_pip && ns->sandbox_pip->visible && !ns->sandbox_pip->minimized && ns->sandbox_pip->last_running;
}
