#include "common.hpp"
#include "ai/agent_bus.hpp"
#include "relay_client.hpp"
#include "sandbox_manager.hpp"
#include "sandbox_stream_codec.hpp"

extern "C" {
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_stop_hostd{false};
std::atomic<uint64_t> g_session_epoch{0};

uint32_t now_msec() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint16_t be16(const uint8_t *p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t be32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

struct HostWayland {
    struct wl_display *dpy = nullptr;
    struct wl_registry *reg = nullptr;
    struct wl_shm *shm = nullptr;
    struct wl_output *output = nullptr;
    struct wl_seat *seat = nullptr;
    struct zwlr_screencopy_manager_v1 *sc_mgr = nullptr;
    struct zwlr_virtual_pointer_manager_v1 *vp_mgr = nullptr;
    struct zwlr_virtual_pointer_v1 *vptr = nullptr;
    struct zwp_virtual_keyboard_manager_v1 *vk_mgr = nullptr;
    struct zwp_virtual_keyboard_v1 *vkbd = nullptr;

    struct zwlr_screencopy_frame_v1 *cur_frame = nullptr;
    bool frame_done = false;
    bool frame_ok = false;
    uint32_t fmt = 0, w = 0, h = 0, stride = 0, flags = 0;

    int shm_fd = -1;
    void *shm_map = nullptr;
    size_t shm_sz = 0;
    struct wl_shm_pool *pool = nullptr;
    struct wl_buffer *buf = nullptr;
    uint32_t sw = 0, sh = 0, sstride = 0, sfmt = 0;

    std::vector<uint8_t> bgra;

    ~HostWayland() { cleanup(); }

    void release_shm() {
        if (buf) { wl_buffer_destroy(buf); buf = nullptr; }
        if (pool) { wl_shm_pool_destroy(pool); pool = nullptr; }
        if (shm_map && shm_map != MAP_FAILED) { ::munmap(shm_map, shm_sz); shm_map = nullptr; }
        if (shm_fd >= 0) { ::close(shm_fd); shm_fd = -1; }
        shm_sz = sw = sh = sstride = sfmt = 0;
    }

    bool ensure_shm(uint32_t nw, uint32_t nh, uint32_t nstride, uint32_t nfmt) {
        if (!shm || nw == 0 || nh == 0 || nstride == 0) return false;
        if (buf && sw == nw && sh == nh && sstride == nstride && sfmt == nfmt) return true;
        release_shm();
        size_t sz = static_cast<size_t>(nstride) * nh;
        int fd = ::memfd_create("spinel-hostd-frame", MFD_CLOEXEC);
        if (fd < 0 || ::ftruncate(fd, static_cast<off_t>(sz)) != 0) {
            if (fd >= 0) ::close(fd);
            return false;
        }
        void *map = ::mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { ::close(fd); return false; }
        struct wl_shm_pool *pl = wl_shm_create_pool(shm, fd, static_cast<int32_t>(sz));
        struct wl_buffer *bf = pl ? wl_shm_pool_create_buffer(pl, 0, static_cast<int32_t>(nw),
                                                              static_cast<int32_t>(nh),
                                                              static_cast<int32_t>(nstride), nfmt)
                                  : nullptr;
        if (!pl || !bf) {
            if (pl) wl_shm_pool_destroy(pl);
            ::munmap(map, sz);
            ::close(fd);
            return false;
        }
        shm_fd = fd; shm_map = map; shm_sz = sz; pool = pl; buf = bf;
        sw = nw; sh = nh; sstride = nstride; sfmt = nfmt;
        return true;
    }

    void cleanup() {
        if (cur_frame) { zwlr_screencopy_frame_v1_destroy(cur_frame); cur_frame = nullptr; }
        release_shm();
        if (vptr) { zwlr_virtual_pointer_v1_destroy(vptr); vptr = nullptr; }
        if (vp_mgr) { zwlr_virtual_pointer_manager_v1_destroy(vp_mgr); vp_mgr = nullptr; }
        if (vkbd) { zwp_virtual_keyboard_v1_destroy(vkbd); vkbd = nullptr; }
        if (vk_mgr) { zwp_virtual_keyboard_manager_v1_destroy(vk_mgr); vk_mgr = nullptr; }
        if (sc_mgr) { zwlr_screencopy_manager_v1_destroy(sc_mgr); sc_mgr = nullptr; }
        if (output) { wl_output_destroy(output); output = nullptr; }
        if (seat) { wl_seat_destroy(seat); seat = nullptr; }
        if (shm) { wl_shm_destroy(shm); shm = nullptr; }
        if (reg) { wl_registry_destroy(reg); reg = nullptr; }
        if (dpy) { wl_display_disconnect(dpy); dpy = nullptr; }
    }
};

void on_host_frame_buffer(void *d, struct zwlr_screencopy_frame_v1 *f,
                          uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride) {
    auto *hw = static_cast<HostWayland *>(d);
    if (!hw || f != hw->cur_frame) return;
    hw->fmt = fmt; hw->w = w; hw->h = h; hw->stride = stride; hw->flags = 0;
    if (hw->ensure_shm(w, h, stride, fmt)) {
        zwlr_screencopy_frame_v1_copy(f, hw->buf);
    } else {
        hw->frame_done = true;
        hw->frame_ok = false;
    }
}

void on_host_frame_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags) {
    auto *hw = static_cast<HostWayland *>(d);
    if (hw && f == hw->cur_frame) hw->flags = flags;
}

void on_host_frame_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t, uint32_t, uint32_t) {
    auto *hw = static_cast<HostWayland *>(d);
    if (!hw || f != hw->cur_frame) return;
    hw->bgra.resize(static_cast<size_t>(hw->w) * hw->h * 4);
    const unsigned char *src = static_cast<const unsigned char *>(hw->shm_map);
    bool y_inv = (hw->flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
    bool is_bgr = (hw->fmt == WL_SHM_FORMAT_XBGR8888 || hw->fmt == WL_SHM_FORMAT_ABGR8888);
    for (uint32_t y = 0; y < hw->h; ++y) {
        uint32_t sy = y_inv ? (hw->h - 1 - y) : y;
        const uint32_t *srow = reinterpret_cast<const uint32_t *>(src + static_cast<size_t>(sy) * hw->stride);
        uint32_t *drow = reinterpret_cast<uint32_t *>(hw->bgra.data() + static_cast<size_t>(y) * hw->w * 4);
        if (!is_bgr) {
            for (uint32_t x = 0; x < hw->w; ++x) {
                drow[x] = 0xFF000000u | (srow[x] & 0x00FFFFFFu);
            }
        } else {
            for (uint32_t x = 0; x < hw->w; ++x) {
                uint32_t p = srow[x];
                drow[x] = 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p & 0xFF0000u) >> 16);
            }
        }
    }
    hw->frame_ok = true;
    hw->frame_done = true;
}

void on_host_frame_failed(void *d, struct zwlr_screencopy_frame_v1 *f) {
    auto *hw = static_cast<HostWayland *>(d);
    if (hw && f == hw->cur_frame) { hw->frame_ok = false; hw->frame_done = true; }
}

const struct zwlr_screencopy_frame_v1_listener kHostFrameListener = {
    on_host_frame_buffer, on_host_frame_flags, on_host_frame_ready, on_host_frame_failed, nullptr, nullptr, nullptr
};

void on_host_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
    auto *hw = static_cast<HostWayland *>(d);
    if (!hw || !iface) return;
    if (std::strcmp(iface, wl_shm_interface.name) == 0 && !hw->shm) {
        hw->shm = static_cast<struct wl_shm *>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    } else if (std::strcmp(iface, wl_output_interface.name) == 0 && !hw->output) {
        hw->output = static_cast<struct wl_output *>(wl_registry_bind(reg, name, &wl_output_interface, std::min(version, 2u)));
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0 && !hw->seat) {
        hw->seat = static_cast<struct wl_seat *>(wl_registry_bind(reg, name, &wl_seat_interface, 1));
    } else if (std::strcmp(iface, zwlr_screencopy_manager_v1_interface.name) == 0 && !hw->sc_mgr) {
        hw->sc_mgr = static_cast<struct zwlr_screencopy_manager_v1 *>(
            wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, 1));
    } else if (std::strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0 && !hw->vp_mgr) {
        hw->vp_mgr = static_cast<struct zwlr_virtual_pointer_manager_v1 *>(
            wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface, 1));
    } else if (std::strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0 && !hw->vk_mgr) {
        hw->vk_mgr = static_cast<struct zwp_virtual_keyboard_manager_v1 *>(
            wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
    }
}

void on_host_remove(void *, struct wl_registry *, uint32_t) {}
const struct wl_registry_listener kHostRegListener = {on_host_global, on_host_remove};

void init_host_keymap(HostWayland &hw) {
    if (!hw.vkbd) return;
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) return;
    struct xkb_rule_names names{};
    names.layout = "us";
    struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (km) {
        if (char *str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1)) {
            size_t len = std::strlen(str) + 1;
            int fd = ::memfd_create("spinel-hostd-km", MFD_CLOEXEC);
            if (fd >= 0) {
                if (::ftruncate(fd, static_cast<off_t>(len)) == 0 &&
                    ::write(fd, str, len) == static_cast<ssize_t>(len)) {
                    ::lseek(fd, 0, SEEK_SET);
                    zwp_virtual_keyboard_v1_keymap(hw.vkbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, static_cast<uint32_t>(len));
                }
                ::close(fd);
            }
            std::free(str);
        }
        xkb_keymap_unref(km);
    }
    xkb_context_unref(ctx);
}

void serve_remote_session(std::unique_ptr<E2eeTunnel> tun) {
    if (!tun) return;
    const uint64_t my_epoch = g_session_epoch.fetch_add(1) + 1;
    std::string err;
    if (!start_sandbox_session(err)) {
        append_app_log("[RelayHostd] cannot start sandbox: " + err);
        return;
    }

    HostWayland hw;
    std::string sock = sandbox_runtime_dir() + "/" + sandbox_socket_name();
    hw.dpy = wl_display_connect(sock.c_str());
    if (!hw.dpy) {
        append_app_log("[RelayHostd] wl_display_connect failed: " + sock);
        return;
    }
    hw.reg = wl_display_get_registry(hw.dpy);
    wl_registry_add_listener(hw.reg, &kHostRegListener, &hw);
    wl_display_roundtrip(hw.dpy);
    if (!hw.sc_mgr || !hw.output) {
        append_app_log("[RelayHostd] missing screencopy manager or output");
        return;
    }
    if (hw.vp_mgr) hw.vptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(hw.vp_mgr, hw.seat);
    if (hw.vk_mgr && hw.seat) {
        hw.vkbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(hw.vk_mgr, hw.seat);
        init_host_keymap(hw);
    }
    wl_display_flush(hw.dpy);
    append_app_log("[RelayHostd] session started (" + tun->transport_label() + ") sock=" + sock);

    std::atomic<bool> sess_done{false};
    std::atomic<bool> takeover_mode{false};
    std::atomic<bool> keyframe_req{true};
    std::mutex wl_mu;

    // Input & control receiver thread
    std::thread rx([&]() {
        uint8_t mtype = 0;
        std::vector<uint8_t> data;
        while (!g_stop_hostd.load() && !sess_done.load() && g_session_epoch.load() == my_epoch && tun->is_open()) {
            if (!tun->recv_msg(mtype, data, 300)) continue;
            switch (mtype) {
                case sandbox_stream::MSG_MOUSE_MOVE:
                    if (data.size() >= 4) {
                        uint16_t nx = be16(data.data());
                        uint16_t ny = be16(data.data() + 2);
                        std::lock_guard<std::mutex> lk(wl_mu);
                        if (hw.vptr && hw.dpy) {
                            zwlr_virtual_pointer_v1_motion_absolute(hw.vptr, now_msec(), nx, ny, 10000, 10000);
                            zwlr_virtual_pointer_v1_frame(hw.vptr);
                            wl_display_flush(hw.dpy);
                        }
                    }
                    break;
                case sandbox_stream::MSG_MOUSE_BTN:
                    if (data.size() >= 5) {
                        uint32_t btn = be32(data.data());
                        uint32_t state = data[4] ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
                        std::lock_guard<std::mutex> lk(wl_mu);
                        if (hw.vptr && hw.dpy) {
                            zwlr_virtual_pointer_v1_button(hw.vptr, now_msec(), btn, state);
                            zwlr_virtual_pointer_v1_frame(hw.vptr);
                            wl_display_flush(hw.dpy);
                        }
                    }
                    break;
                case sandbox_stream::MSG_MOUSE_AXIS:
                    if (data.size() >= 8) {
                        int32_t dx = static_cast<int32_t>(be32(data.data()));
                        int32_t dy = static_cast<int32_t>(be32(data.data() + 4));
                        std::lock_guard<std::mutex> lk(wl_mu);
                        if (hw.vptr && hw.dpy) {
                            uint32_t t = now_msec();
                            if (dy != 0) zwlr_virtual_pointer_v1_axis(hw.vptr, t, WL_POINTER_AXIS_VERTICAL_SCROLL, dy);
                            if (dx != 0) zwlr_virtual_pointer_v1_axis(hw.vptr, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL, dx);
                            zwlr_virtual_pointer_v1_frame(hw.vptr);
                            wl_display_flush(hw.dpy);
                        }
                    }
                    break;
                case sandbox_stream::MSG_KEY_EVENT:
                    if (data.size() >= 9) {
                        uint32_t keycode = be32(data.data());
                        uint32_t pressed = data[4] ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
                        uint32_t mods = be32(data.data() + 5);
                        std::lock_guard<std::mutex> lk(wl_mu);
                        if (hw.vkbd && hw.dpy) {
                            zwp_virtual_keyboard_v1_modifiers(hw.vkbd, mods, 0, 0, 0);
                            zwp_virtual_keyboard_v1_key(hw.vkbd, now_msec(), keycode, pressed);
                            wl_display_flush(hw.dpy);
                        }
                    }
                    break;
                case sandbox_stream::MSG_SET_MODE:
                    if (!data.empty()) {
                        takeover_mode.store(data[0] != 0);
                        keyframe_req.store(true);
                    }
                    break;
                case sandbox_stream::MSG_KEYFRAME_REQ:
                    keyframe_req.store(true);
                    break;
                case sandbox_stream::MSG_CLIP_TEXT:
                    if (!data.empty() && has_executable("wl-copy")) {
                        std::string text(data.begin(), data.end());
                        int pipefd[2]{-1, -1};
                        if (::pipe(pipefd) == 0) {
                            pid_t p = ::fork();
                            if (p == 0) {
                                ::close(pipefd[1]);
                                ::dup2(pipefd[0], STDIN_FILENO);
                                ::close(pipefd[0]);
                                ::setenv("XDG_RUNTIME_DIR", sandbox_runtime_dir().c_str(), 1);
                                ::setenv("WAYLAND_DISPLAY", sandbox_socket_name().c_str(), 1);
                                ::execlp("wl-copy", "wl-copy", "--type", "text/plain;charset=utf-8", nullptr);
                                _exit(127);
                            }
                            ::close(pipefd[0]);
                            if (p > 0) {
                                ssize_t wr = ::write(pipefd[1], text.data(), text.size());
                                (void)wr;
                            }
                            ::close(pipefd[1]);
                        }
                    }
                    break;
                case sandbox_stream::MSG_LAUNCH_APP:
                    if (!data.empty()) {
                        std::string bin(data.begin(), data.end());
                        std::string lerr;
                        launch_in_sandbox({bin}, "", lerr);
                    }
                    break;
                case sandbox_stream::MSG_AI_ACTION:
                    if (!data.empty()) {
                        std::string req_json(data.begin(), data.end());
                        std::string resp;
                        spinel::ai::call_agent_bus_socket(
                            spinel::ai::agent_bus_socket_path(), req_json, resp, 4000);
                    }
                    break;
                case sandbox_stream::MSG_BYE:
                    sess_done.store(true);
                    break;
                default:
                    break;
            }
        }
        sess_done.store(true);
    });

    // Screen capture & tile encoding loop
    sandbox_stream::TileEncoderState enc;
    std::vector<uint8_t> pkt;
    bool logged_first_frame = false;
    auto last_ai_push = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    std::string last_ai_state;
    while (!g_stop_hostd.load() && !sess_done.load() && g_session_epoch.load() == my_epoch && tun->is_open()) {
        if (!query_sandbox_status().running) break;

        if (std::chrono::steady_clock::now() - last_ai_push >= std::chrono::milliseconds(1500)) {
            last_ai_push = std::chrono::steady_clock::now();
            std::string sess_r, apr_r;
            if (spinel::ai::call_agent_bus_socket(
                    spinel::ai::agent_bus_socket_path(),
                    R"({"id":"r1","client":"relay-hostd","method":"agent.list","params":{"include_proc":false}})",
                    sess_r, 800) &&
                spinel::ai::call_agent_bus_socket(
                    spinel::ai::agent_bus_socket_path(),
                    R"({"id":"r2","client":"relay-hostd","method":"approval.list","params":{}})",
                    apr_r, 800)) {
                std::string bundle = "{\"sessions\":" + sess_r + ",\"approvals\":" + apr_r + "}";
                if (bundle != last_ai_state) {
                    last_ai_state = bundle;
                    tun->send_msg(sandbox_stream::MSG_AI_STATE, bundle.data(), bundle.size());
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(wl_mu);
            hw.frame_done = false;
            hw.frame_ok = false;
            hw.cur_frame = zwlr_screencopy_manager_v1_capture_output(hw.sc_mgr, 1, hw.output);
            if (!hw.cur_frame) break;
            zwlr_screencopy_frame_v1_add_listener(hw.cur_frame, &kHostFrameListener, &hw);
            wl_display_flush(hw.dpy);
        }

        int fd = wl_display_get_fd(hw.dpy);
        auto cap_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (!hw.frame_done && !g_stop_hostd.load() && !sess_done.load() && g_session_epoch.load() == my_epoch) {
            if (std::chrono::steady_clock::now() >= cap_deadline) break;
            struct pollfd pfd{fd, POLLIN, 0};
            if (::poll(&pfd, 1, 50) > 0) {
                std::lock_guard<std::mutex> lk(wl_mu);
                if (wl_display_dispatch(hw.dpy) < 0) {
                    sess_done.store(true);
                    break;
                }
                wl_display_flush(hw.dpy);
            } else {
                std::lock_guard<std::mutex> lk(wl_mu);
                wl_display_dispatch_pending(hw.dpy);
                wl_display_flush(hw.dpy);
            }
        }
        {
            std::lock_guard<std::mutex> lk(wl_mu);
            if (hw.cur_frame) {
                zwlr_screencopy_frame_v1_destroy(hw.cur_frame);
                hw.cur_frame = nullptr;
            }
            wl_display_flush(hw.dpy);
        }

        if (hw.frame_ok && !hw.bgra.empty()) {
            bool kf = keyframe_req.exchange(false);
            int max_w = takeover_mode.load() ? 1600 : 640;
            if (sandbox_stream::encode_tile_frame(enc, hw.bgra.data(), static_cast<int>(hw.w),
                                                  static_cast<int>(hw.h), static_cast<int>(hw.w * 4),
                                                  kf, max_w, pkt)) {
                if (!logged_first_frame) {
                    logged_first_frame = true;
                    append_app_log("[RelayHostd] first frame " + std::to_string(hw.w) + "x" +
                                   std::to_string(hw.h) + " fmt=" + std::to_string(hw.fmt) +
                                   " pkt=" + std::to_string(pkt.size()) + "B");
                }
                if (!tun->send_msg(sandbox_stream::MSG_FRAME_TILE, pkt.data(), pkt.size())) {
                    break;
                }
            }
        }
        int interval_ms = takeover_mode.load() ? 35 : 100;
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }

    sess_done.store(true);
    tun->close();
    if (rx.joinable()) rx.join();
}

}  // namespace

int run_sandbox_host_daemon() {
    g_stop_hostd.store(false);
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, +[](int) { g_stop_hostd.store(true); });
    signal(SIGINT, +[](int) { g_stop_hostd.store(true); });

    const std::string my_pid = std::to_string(::getpid());
    write_text_file(relay_hostd_pid_file(), my_pid + "\n");
    run_relay_host_signaling(g_stop_hostd, [](std::unique_ptr<E2eeTunnel> tun) {
        serve_remote_session(std::move(tun));
    });

    std::error_code ec;
    if (trim_copy(read_first_line(relay_hostd_pid_file())) == my_pid) {
        fs::remove(relay_hostd_pid_file(), ec);
    }
    return 0;
}
