#include "window_tracker.hpp"
#include "common.hpp"
#include "desktop_apps.hpp"
#include "native_shell.hpp"
#include "ext-workspace-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#include <wayland-client.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct TrackedWindow {
    uint64_t id = 0;
    uint64_t last_active_seq = 0;
    zwlr_foreign_toplevel_handle_v1 *handle = nullptr;
    std::string title;
    std::string app_id;
    bool activated = false;
    bool minimized = false;
    bool maximized = false;
    bool fullscreen = false;
    bool ready = false;
};

struct TrackedWorkspaceGroup {
    ext_workspace_group_handle_v1 *handle = nullptr;
};

struct TrackedWorkspace {
    uint64_t seq = 0;
    ext_workspace_handle_v1 *handle = nullptr;
    std::string id;
    std::string name;
    std::vector<uint32_t> coordinates;
    bool active = false;
    bool urgent = false;
    bool hidden = false;
    bool can_activate = true;
};

struct WaylandSource {
    GSource base;
    GPollFD poll_fd;
    wl_display *display = nullptr;
};

struct TrackerCtx {
    AppState *state = nullptr;
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_seat *seat = nullptr;
    zwlr_foreign_toplevel_manager_v1 *manager = nullptr;
    ext_workspace_manager_v1 *ws_manager = nullptr;
    uint64_t next_id = 1;
    uint64_t next_ws_seq = 1;
    std::vector<std::unique_ptr<TrackedWindow>> windows;
    std::vector<std::unique_ptr<TrackedWorkspaceGroup>> ws_groups;
    std::vector<std::unique_ptr<TrackedWorkspace>> workspaces;
    GSource *glib_source = nullptr;
};

TrackerCtx g_ctx;

void sort_tracked_workspaces() {
    std::stable_sort(g_ctx.workspaces.begin(), g_ctx.workspaces.end(),
                     [](const std::unique_ptr<TrackedWorkspace> &a, const std::unique_ptr<TrackedWorkspace> &b) {
                         if (!a->coordinates.empty() && !b->coordinates.empty() && a->coordinates != b->coordinates) {
                             return a->coordinates < b->coordinates;
                         }
                         return a->seq < b->seq;
                     });
}

void on_ws_group_caps(void *, ext_workspace_group_handle_v1 *, uint32_t) {}
void on_ws_group_output_enter(void *, ext_workspace_group_handle_v1 *, wl_output *) {}
void on_ws_group_output_leave(void *, ext_workspace_group_handle_v1 *, wl_output *) {}
void on_ws_group_ws_enter(void *, ext_workspace_group_handle_v1 *, ext_workspace_handle_v1 *) {}
void on_ws_group_ws_leave(void *, ext_workspace_group_handle_v1 *, ext_workspace_handle_v1 *) {}
void on_ws_group_removed(void *, ext_workspace_group_handle_v1 *group) {
    g_ctx.ws_groups.erase(
        std::remove_if(g_ctx.ws_groups.begin(), g_ctx.ws_groups.end(),
                       [group](const std::unique_ptr<TrackedWorkspaceGroup> &g) {
                           return g->handle == group;
                       }),
        g_ctx.ws_groups.end());
    ext_workspace_group_handle_v1_destroy(group);
}

const ext_workspace_group_handle_v1_listener kWsGroupListener = {
    on_ws_group_caps,
    on_ws_group_output_enter,
    on_ws_group_output_leave,
    on_ws_group_ws_enter,
    on_ws_group_ws_leave,
    on_ws_group_removed
};

void on_ws_id(void *data, ext_workspace_handle_v1 *, const char *id) {
    if (auto *ws = static_cast<TrackedWorkspace *>(data)) {
        ws->id = id ? id : "";
    }
}

void on_ws_name(void *data, ext_workspace_handle_v1 *, const char *name) {
    if (auto *ws = static_cast<TrackedWorkspace *>(data)) {
        ws->name = name ? name : "";
    }
}

void on_ws_coordinates(void *data, ext_workspace_handle_v1 *, wl_array *coords) {
    auto *ws = static_cast<TrackedWorkspace *>(data);
    if (!ws || !coords) return;
    ws->coordinates.clear();
    const auto *vals = static_cast<const uint32_t *>(coords->data);
    size_t n = coords->size / sizeof(uint32_t);
    ws->coordinates.assign(vals, vals + n);
}

void on_ws_state(void *data, ext_workspace_handle_v1 *, uint32_t state_bits) {
    auto *ws = static_cast<TrackedWorkspace *>(data);
    if (!ws) return;
    ws->active = (state_bits & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) != 0;
    ws->urgent = (state_bits & EXT_WORKSPACE_HANDLE_V1_STATE_URGENT) != 0;
    ws->hidden = (state_bits & EXT_WORKSPACE_HANDLE_V1_STATE_HIDDEN) != 0;
}

void on_ws_caps(void *data, ext_workspace_handle_v1 *, uint32_t caps) {
    auto *ws = static_cast<TrackedWorkspace *>(data);
    if (!ws) return;
    ws->can_activate = (caps & EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE) != 0;
}

void on_ws_removed(void *, ext_workspace_handle_v1 *handle) {
    g_ctx.workspaces.erase(
        std::remove_if(g_ctx.workspaces.begin(), g_ctx.workspaces.end(),
                       [handle](const std::unique_ptr<TrackedWorkspace> &item) {
                           return item->handle == handle;
                       }),
        g_ctx.workspaces.end());
    ext_workspace_handle_v1_destroy(handle);
}

const ext_workspace_handle_v1_listener kWsHandleListener = {
    on_ws_id,
    on_ws_name,
    on_ws_coordinates,
    on_ws_state,
    on_ws_caps,
    on_ws_removed
};

void on_ws_mgr_group(void *, ext_workspace_manager_v1 *, ext_workspace_group_handle_v1 *group) {
    auto grp = std::make_unique<TrackedWorkspaceGroup>();
    grp->handle = group;
    ext_workspace_group_handle_v1_add_listener(group, &kWsGroupListener, grp.get());
    g_ctx.ws_groups.push_back(std::move(grp));
}

void on_ws_mgr_workspace(void *, ext_workspace_manager_v1 *, ext_workspace_handle_v1 *ws_handle) {
    auto ws = std::make_unique<TrackedWorkspace>();
    ws->seq = g_ctx.next_ws_seq++;
    ws->handle = ws_handle;
    ext_workspace_handle_v1_add_listener(ws_handle, &kWsHandleListener, ws.get());
    g_ctx.workspaces.push_back(std::move(ws));
}

void on_ws_mgr_done(void *, ext_workspace_manager_v1 *) {
    sort_tracked_workspaces();
    native_shell_sync_workspaces(g_ctx.state);
}

void on_ws_mgr_finished(void *, ext_workspace_manager_v1 *mgr) {
    if (g_ctx.ws_manager == mgr) {
        ext_workspace_manager_v1_destroy(mgr);
        g_ctx.ws_manager = nullptr;
    }
}

const ext_workspace_manager_v1_listener kWsMgrListener = {
    on_ws_mgr_group,
    on_ws_mgr_workspace,
    on_ws_mgr_done,
    on_ws_mgr_finished
};

TrackedWindow *find_window_by_id(uint64_t id) {
    for (auto &w : g_ctx.windows) {
        if (w->id == id) return w.get();
    }
    return nullptr;
}

void on_title(void *data, zwlr_foreign_toplevel_handle_v1 *, const char *title) {
    auto *w = static_cast<TrackedWindow *>(data);
    w->title = title ? title : "";
}

void on_app_id(void *data, zwlr_foreign_toplevel_handle_v1 *, const char *app_id) {
    auto *w = static_cast<TrackedWindow *>(data);
    w->app_id = app_id ? app_id : "";
}

void on_output_enter(void *, zwlr_foreign_toplevel_handle_v1 *, wl_output *) {}
void on_output_leave(void *, zwlr_foreign_toplevel_handle_v1 *, wl_output *) {}

void on_state(void *data, zwlr_foreign_toplevel_handle_v1 *, wl_array *state_arr) {
    auto *w = static_cast<TrackedWindow *>(data);
    static uint64_t s_active_seq = 0;
    bool was_active = w->activated;
    w->activated = false;
    w->minimized = false;
    w->maximized = false;
    w->fullscreen = false;

    const uint32_t *vals = static_cast<const uint32_t *>(state_arr->data);
    size_t count = state_arr->size / sizeof(uint32_t);
    for (size_t i = 0; i < count; ++i) {
        switch (vals[i]) {
            case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED: w->activated = true; break;
            case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED: w->minimized = true; break;
            case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED: w->maximized = true; break;
            case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN: w->fullscreen = true; break;
            default: break;
        }
    }
    if (w->activated && (!was_active || w->last_active_seq == 0)) {
        w->last_active_seq = ++s_active_seq;
    }
}

void on_done(void *data, zwlr_foreign_toplevel_handle_v1 *) {
    auto *w = static_cast<TrackedWindow *>(data);
    w->ready = true;
    push_windows_to_ui(g_ctx.state);
}

void on_closed(void *, zwlr_foreign_toplevel_handle_v1 *handle) {
    g_ctx.windows.erase(
        std::remove_if(g_ctx.windows.begin(), g_ctx.windows.end(),
                       [handle](const std::unique_ptr<TrackedWindow> &item) {
                           return item->handle == handle;
                       }),
        g_ctx.windows.end()
    );
    zwlr_foreign_toplevel_handle_v1_destroy(handle);
    push_windows_to_ui(g_ctx.state);
}

void on_parent(void *, zwlr_foreign_toplevel_handle_v1 *, zwlr_foreign_toplevel_handle_v1 *) {}

const zwlr_foreign_toplevel_handle_v1_listener kHandleListener = {
    on_title,
    on_app_id,
    on_output_enter,
    on_output_leave,
    on_state,
    on_done,
    on_closed,
    on_parent
};

void on_toplevel(void *, zwlr_foreign_toplevel_manager_v1 *, zwlr_foreign_toplevel_handle_v1 *handle) {
    auto win = std::make_unique<TrackedWindow>();
    win->id = g_ctx.next_id++;
    win->handle = handle;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &kHandleListener, win.get());
    g_ctx.windows.push_back(std::move(win));
}

void on_finished(void *, zwlr_foreign_toplevel_manager_v1 *manager) {
    if (g_ctx.manager == manager) {
        zwlr_foreign_toplevel_manager_v1_destroy(manager);
        g_ctx.manager = nullptr;
    }
}

const zwlr_foreign_toplevel_manager_v1_listener kManagerListener = {
    on_toplevel,
    on_finished
};

void on_registry_global(void *, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    if (std::strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0 && !g_ctx.manager) {
        uint32_t ver = std::min<uint32_t>(version, 3);
        g_ctx.manager = static_cast<zwlr_foreign_toplevel_manager_v1 *>(
            wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, ver)
        );
        zwlr_foreign_toplevel_manager_v1_add_listener(g_ctx.manager, &kManagerListener, nullptr);
    } else if (std::strcmp(interface, ext_workspace_manager_v1_interface.name) == 0 && !g_ctx.ws_manager) {
        uint32_t ver = std::min<uint32_t>(version, 1);
        g_ctx.ws_manager = static_cast<ext_workspace_manager_v1 *>(
            wl_registry_bind(registry, name, &ext_workspace_manager_v1_interface, ver)
        );
        ext_workspace_manager_v1_add_listener(g_ctx.ws_manager, &kWsMgrListener, nullptr);
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0 && !g_ctx.seat) {
        g_ctx.seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    }
}

void on_registry_remove(void *, wl_registry *, uint32_t) {}

const wl_registry_listener kRegistryListener = {
    on_registry_global,
    on_registry_remove
};

gboolean wl_source_prepare(GSource *base, gint *timeout) {
    auto *src = reinterpret_cast<WaylandSource *>(base);
    *timeout = -1;
    while (wl_display_prepare_read(src->display) != 0) {
        wl_display_dispatch_pending(src->display);
    }
    wl_display_flush(src->display);
    return FALSE;
}

gboolean wl_source_check(GSource *base) {
    auto *src = reinterpret_cast<WaylandSource *>(base);
    if (src->poll_fd.revents & G_IO_IN) {
        return TRUE;
    }
    wl_display_cancel_read(src->display);
    return FALSE;
}

gboolean wl_source_dispatch(GSource *base, GSourceFunc, gpointer) {
    auto *src = reinterpret_cast<WaylandSource *>(base);
    if (src->poll_fd.revents & G_IO_IN) {
        wl_display_read_events(src->display);
        wl_display_dispatch_pending(src->display);
    }
    return G_SOURCE_CONTINUE;
}

void wl_source_finalize(GSource *base) {
    auto *src = reinterpret_cast<WaylandSource *>(base);
    if (src->display) {
        wl_display_cancel_read(src->display);
    }
}

GSourceFuncs kWlSourceFuncs = {
    wl_source_prepare,
    wl_source_check,
    wl_source_dispatch,
    wl_source_finalize,
    nullptr,
    nullptr
};

}  // namespace

void init_window_tracker(AppState *state) {
    g_ctx.state = state;
    g_ctx.display = wl_display_connect(nullptr);
    if (!g_ctx.display) return;

    g_ctx.registry = wl_display_get_registry(g_ctx.display);
    wl_registry_add_listener(g_ctx.registry, &kRegistryListener, nullptr);
    wl_display_roundtrip(g_ctx.display);
    wl_display_roundtrip(g_ctx.display);
    native_shell_sync_workspaces(state);

    GSource *gs = g_source_new(&kWlSourceFuncs, sizeof(WaylandSource));
    auto *src = reinterpret_cast<WaylandSource *>(gs);
    src->display = g_ctx.display;
    src->poll_fd.fd = wl_display_get_fd(g_ctx.display);
    src->poll_fd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
    src->poll_fd.revents = 0;
    g_source_add_poll(gs, &src->poll_fd);
    g_source_set_can_recurse(gs, FALSE);
    g_source_attach(gs, nullptr);
    g_ctx.glib_source = gs;
}

void shutdown_window_tracker() {
    if (g_ctx.glib_source) {
        g_source_destroy(g_ctx.glib_source);
        g_source_unref(g_ctx.glib_source);
        g_ctx.glib_source = nullptr;
    }
    for (auto &w : g_ctx.windows) {
        if (w->handle) zwlr_foreign_toplevel_handle_v1_destroy(w->handle);
    }
    g_ctx.windows.clear();
    for (auto &ws : g_ctx.workspaces) {
        if (ws->handle) ext_workspace_handle_v1_destroy(ws->handle);
    }
    g_ctx.workspaces.clear();
    for (auto &grp : g_ctx.ws_groups) {
        if (grp->handle) ext_workspace_group_handle_v1_destroy(grp->handle);
    }
    g_ctx.ws_groups.clear();
    if (g_ctx.ws_manager) {
        ext_workspace_manager_v1_destroy(g_ctx.ws_manager);
        g_ctx.ws_manager = nullptr;
    }
    if (g_ctx.manager) {
        zwlr_foreign_toplevel_manager_v1_destroy(g_ctx.manager);
        g_ctx.manager = nullptr;
    }
    if (g_ctx.seat) {
        wl_seat_destroy(g_ctx.seat);
        g_ctx.seat = nullptr;
    }
    if (g_ctx.registry) {
        wl_registry_destroy(g_ctx.registry);
        g_ctx.registry = nullptr;
    }
    if (g_ctx.display) {
        wl_display_disconnect(g_ctx.display);
        g_ctx.display = nullptr;
    }
}

std::vector<TrackedWindowInfo> list_tracked_windows() {
    std::vector<TrackedWindowInfo> out;
    for (const auto &w : g_ctx.windows) {
        if (!w->ready) continue;
        TrackedWindowInfo info;
        info.id = w->id;
        info.title = w->title;
        info.app_id = w->app_id;
        info.activated = w->activated;
        info.minimized = w->minimized;
        info.maximized = w->maximized;
        info.fullscreen = w->fullscreen;
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<TrackedWindowInfo> list_tracked_windows_mru() {
    std::vector<const TrackedWindow *> refs;
    refs.reserve(g_ctx.windows.size());
    for (const auto &w : g_ctx.windows) {
        if (w->ready) refs.push_back(w.get());
    }
    std::stable_sort(refs.begin(), refs.end(), [](const TrackedWindow *a, const TrackedWindow *b) {
        if (a->activated != b->activated) return a->activated > b->activated;
        if (a->minimized != b->minimized) return !a->minimized;
        if (a->last_active_seq != b->last_active_seq) return a->last_active_seq > b->last_active_seq;
        return a->id > b->id;
    });
    std::vector<TrackedWindowInfo> out;
    out.reserve(refs.size());
    for (const auto *w : refs) {
        TrackedWindowInfo info;
        info.id = w->id;
        info.title = w->title;
        info.app_id = w->app_id;
        info.activated = w->activated;
        info.minimized = w->minimized;
        info.maximized = w->maximized;
        info.fullscreen = w->fullscreen;
        out.push_back(std::move(info));
    }
    return out;
}

void push_windows_to_ui(AppState *state) {
    if (!state || state->shutting_down.load()) return;

    native_shell_sync_windows(state);
    if (!state->bar_view && !state->dock_view) return;

    auto list = list_tracked_windows();
    std::ostringstream js;
    js << "window.__spinelWindows && window.__spinelWindows([";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i > 0) js << ",";
        js << "{"
           << "\"id\":" << list[i].id << ","
           << "\"title\":\"" << json_escape(list[i].title) << "\","
           << "\"app_id\":\"" << json_escape(list[i].app_id) << "\","
           << "\"activated\":" << (list[i].activated ? "true" : "false") << ","
           << "\"minimized\":" << (list[i].minimized ? "true" : "false") << ","
           << "\"maximized\":" << (list[i].maximized ? "true" : "false") << ","
           << "\"fullscreen\":" << (list[i].fullscreen ? "true" : "false")
           << "}";
    }
    js << "]);";
    std::string script = js.str();
    eval_js(state->bar_view, script);
    eval_js(state->dock_view, script);
}

void toggle_or_activate_window(uint64_t id) {
    TrackedWindow *w = find_window_by_id(id);
    if (!w || !w->handle || !g_ctx.display) return;
    if (w->activated && !w->minimized) {
        zwlr_foreign_toplevel_handle_v1_set_minimized(w->handle);
    } else {
        if (w->minimized) {
            zwlr_foreign_toplevel_handle_v1_unset_minimized(w->handle);
        }
        if (g_ctx.seat) {
            zwlr_foreign_toplevel_handle_v1_activate(w->handle, g_ctx.seat);
        }
    }
    wl_display_flush(g_ctx.display);
}

void toggle_maximize_tracked_window(uint64_t id) {
    TrackedWindow *w = find_window_by_id(id);
    if (!w || !w->handle || !g_ctx.display) return;
    if (w->maximized) {
        zwlr_foreign_toplevel_handle_v1_unset_maximized(w->handle);
    } else {
        if (w->minimized) {
            zwlr_foreign_toplevel_handle_v1_unset_minimized(w->handle);
        }
        zwlr_foreign_toplevel_handle_v1_set_maximized(w->handle);
    }
    wl_display_flush(g_ctx.display);
}

void minimize_tracked_window(uint64_t id) {
    TrackedWindow *w = find_window_by_id(id);
    if (!w || !w->handle || !g_ctx.display) return;
    zwlr_foreign_toplevel_handle_v1_set_minimized(w->handle);
    wl_display_flush(g_ctx.display);
}

void activate_tracked_window(uint64_t id) {
    TrackedWindow *w = find_window_by_id(id);
    if (!w || !w->handle || !g_ctx.display) return;
    if (w->minimized) {
        zwlr_foreign_toplevel_handle_v1_unset_minimized(w->handle);
    }
    if (g_ctx.seat) {
        zwlr_foreign_toplevel_handle_v1_activate(w->handle, g_ctx.seat);
    }
    wl_display_flush(g_ctx.display);
}

void close_tracked_window(uint64_t id) {
    TrackedWindow *w = find_window_by_id(id);
    if (!w || !w->handle || !g_ctx.display) return;
    zwlr_foreign_toplevel_handle_v1_close(w->handle);
    wl_display_flush(g_ctx.display);
}

bool quit_tracked_window_app(uint64_t id) {
    TrackedWindow *w = find_window_by_id(id);
    if (!w) return false;
    std::string aid = w->app_id;
    std::string ttl = w->title;
    close_tracked_window(id);
    return quit_app_processes(aid, ttl);
}

std::string format_workspace_badge_label(int index_0based, const std::string &ws_name) {
    std::string trimmed = trim_copy(ws_name);
    if (trimmed.empty()) {
        return std::to_string(index_0based + 1);
    }
    // Extract concise number for standard prefixes like "工作区 1", "Desktop 2", "Workspace 3"
    size_t i = trimmed.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(trimmed[i - 1]))) {
        --i;
    }
    if (i < trimmed.size() && (i == 0 || std::isspace(static_cast<unsigned char>(trimmed[i - 1])))) {
        return trimmed.substr(i);
    }
    if (trimmed.size() <= 8) return trimmed;
    return std::to_string(index_0based + 1);
}

std::vector<TrackedWorkspaceInfo> list_tracked_workspaces() {
    std::vector<TrackedWorkspaceInfo> out;
    int idx = 0;
    for (const auto &ws : g_ctx.workspaces) {
        if (ws->hidden) continue;
        TrackedWorkspaceInfo info;
        info.index = idx++;
        info.id = ws->id;
        info.name = ws->name;
        info.active = ws->active;
        info.urgent = ws->urgent;
        info.hidden = ws->hidden;
        info.can_activate = ws->can_activate;
        out.push_back(std::move(info));
    }
    return out;
}

bool activate_tracked_workspace(int index_0based) {
    if (index_0based < 0 || !g_ctx.ws_manager || !g_ctx.display) return false;
    int cur_idx = 0;
    for (const auto &ws : g_ctx.workspaces) {
        if (ws->hidden) continue;
        if (cur_idx == index_0based) {
            if (!ws->handle) return false;
            ext_workspace_handle_v1_activate(ws->handle);
            ext_workspace_manager_v1_commit(g_ctx.ws_manager);
            wl_display_flush(g_ctx.display);
            return true;
        }
        ++cur_idx;
    }
    return false;
}

bool step_tracked_workspace(int delta) {
    auto list = list_tracked_workspaces();
    if (list.empty()) return false;
    int active_idx = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].active) {
            active_idx = static_cast<int>(i);
            break;
        }
    }
    int n = static_cast<int>(list.size());
    int next_idx = ((active_idx + delta) % n + n) % n;
    return activate_tracked_workspace(next_idx);
}
