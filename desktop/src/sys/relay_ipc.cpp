#include "system_control.hpp"
#include "common.hpp"
#include "native_shell.hpp"
#include "relay_client.hpp"

#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtk/gtk.h>

namespace fs = std::filesystem;

/* ==================== Cloud Relay & OTA Check IPC ==================== */

namespace {

void emit_relay_feedback(AppState *state, bool ok, const std::string &msg) {
    if (!state || !state->settings_view) return;
    eval_js(state->settings_view,
            "window.__spinelRelayFeedback && window.__spinelRelayFeedback(" +
                std::string(ok ? "true" : "false") + ",\"" + json_escape(msg) + "\");");
}

std::string peer_devices_json(const std::vector<RelayPeerDevice> &devs) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < devs.size(); ++i) {
        if (i) oss << ",";
        const auto &d = devs[i];
        oss << "{"
            << "\"device_id\":\"" << json_escape(d.device_id) << "\","
            << "\"name\":\"" << json_escape(d.name) << "\","
            << "\"online\":" << (d.online ? "true" : "false") << ","
            << "\"allow_remote\":" << (d.allow_remote ? "true" : "false") << ","
            << "\"sandbox_running\":" << (d.sandbox_running ? "true" : "false") << ","
            << "\"udp_mapped\":\"" << json_escape(d.udp_mapped) << "\","
            << "\"lan_count\":" << d.lan_addrs.size() << ","
            << "\"ipv6_count\":" << d.ipv6_addrs.size()
            << "}";
    }
    oss << "]";
    return oss.str();
}

}  // namespace

void push_relay_state(AppState *state, bool fetch_peers) {
    if (!state || !state->settings_view) return;
    eval_js(state->settings_view,
            "window.__spinelRelayData && window.__spinelRelayData(" + relay_state_json() + ",[]);");
    if (!fetch_peers) return;
    RelayConfig cfg = load_relay_config();
    if (cfg.server_addr.empty() || cfg.account.empty() || cfg.token.empty()) return;

    std::thread([state]() {
        std::vector<RelayPeerDevice> devs;
        std::string err;
        bool ok = relay_fetch_devices(devs, err);
        struct PushCtx {
            AppState *state;
            bool ok;
            std::string json;
            std::string err;
        };
        auto *ctx = new PushCtx{
            state,
            ok,
            ok ? peer_devices_json(devs) : "[]",
            std::move(err)
        };
        g_idle_add(+[](gpointer d) -> gboolean {
            std::unique_ptr<PushCtx> p(static_cast<PushCtx *>(d));
            if (p->state && !p->state->shutting_down.load() && p->state->settings_view) {
                eval_js(p->state->settings_view,
                        "window.__spinelRelayData && window.__spinelRelayData(" +
                            relay_state_json() + "," + p->json + ");");
                if (!p->ok && !p->err.empty()) emit_relay_feedback(p->state, false, p->err);
            }
            return G_SOURCE_REMOVE;
        }, ctx);
    }).detach();
}

void relay_auth_ipc(AppState *state, const std::string &payload, bool is_register) {
    std::string server, fp, acc, pwd;
    int udp = 7444;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        if (k == "server") server = v;
        else if (k == "udp_port") { try { udp = std::stoi(v); } catch (...) {} }
        else if (k == "fp") fp = v;
        else if (k == "account") acc = v;
        else if (k == "password") pwd = v;
    }
    std::thread([state, is_register, server, udp, fp, acc, pwd]() {
        std::string err;
        bool ok = relay_login_or_register(is_register, server, udp, fp, acc, pwd, err);
        if (ok && load_relay_config().allow_remote) ensure_relay_hostd_running(true);
        struct AuthCtx {
            AppState *state;
            bool ok;
            std::string msg;
        };
        auto *ctx = new AuthCtx{
            state,
            ok,
            ok ? (is_register ? "Spinel ID 注册并绑定本设备成功！" : "Spinel ID 登录成功！") : err
        };
        g_idle_add(+[](gpointer d) -> gboolean {
            std::unique_ptr<AuthCtx> p(static_cast<AuthCtx *>(d));
            if (p->state && !p->state->shutting_down.load()) {
                emit_relay_feedback(p->state, p->ok, p->msg);
                push_relay_state(p->state, p->ok);
            }
            return G_SOURCE_REMOVE;
        }, ctx);
    }).detach();
}

void relay_pair_create_ipc(AppState *state) {
    std::thread([state]() {
        std::string code, err;
        long long exp = 0;
        bool ok = relay_create_pair_code(code, exp, err);
        struct PairCtx {
            AppState *state;
            bool ok;
            std::string code;
            std::string err;
        };
        auto *ctx = new PairCtx{state, ok, std::move(code), std::move(err)};
        g_idle_add(+[](gpointer d) -> gboolean {
            std::unique_ptr<PairCtx> p(static_cast<PairCtx *>(d));
            if (p->state && !p->state->shutting_down.load() && p->state->settings_view) {
                if (p->ok) {
                    eval_js(p->state->settings_view,
                            "window.__spinelRelayPairCode && window.__spinelRelayPairCode(\"" +
                                json_escape(p->code) + "\");");
                    emit_relay_feedback(p->state, true, "已生成 6 位配对码（5 分钟内有效），可在另一台设备直接加入同一账号。");
                } else {
                    emit_relay_feedback(p->state, false, p->err);
                }
            }
            return G_SOURCE_REMOVE;
        }, ctx);
    }).detach();
}

void relay_pair_claim_ipc(AppState *state, const std::string &payload) {
    std::string server, fp, code;
    int udp = 7444;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        if (k == "server") server = v;
        else if (k == "udp_port") { try { udp = std::stoi(v); } catch (...) {} }
        else if (k == "fp") fp = v;
        else if (k == "code") code = v;
    }
    std::thread([state, server, udp, fp, code]() {
        std::string err;
        bool ok = relay_claim_pair_code(server, udp, fp, code, err);
        struct ClaimCtx {
            AppState *state;
            bool ok;
            std::string msg;
        };
        auto *ctx = new ClaimCtx{state, ok, ok ? "设备配对成功，已接入同一 Spinel 账号！" : err};
        g_idle_add(+[](gpointer d) -> gboolean {
            std::unique_ptr<ClaimCtx> p(static_cast<ClaimCtx *>(d));
            if (p->state && !p->state->shutting_down.load()) {
                emit_relay_feedback(p->state, p->ok, p->msg);
                push_relay_state(p->state, p->ok);
            }
            return G_SOURCE_REMOVE;
        }, ctx);
    }).detach();
}

void relay_set_allow_remote_ipc(AppState *state, const std::string &payload) {
    bool allow = false;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (trim_copy(line.substr(0, eq)) == "allow") {
            std::string v = trim_copy(line.substr(eq + 1));
            allow = (v == "true" || v == "1");
        }
    }
    RelayConfig cfg = load_relay_config();
    cfg.allow_remote = allow;
    save_relay_config(cfg);
    ensure_relay_hostd_running(allow);
    emit_relay_feedback(state, true,
                        allow ? "已开启远程接管宿主服务，同一账号设备现在可远程连接本机平行沙箱。"
                              : "已关闭本机远程接管宿主服务。");
    push_relay_state(state, false);
}

void relay_set_device_name_ipc(AppState *state, const std::string &payload) {
    std::string name;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (trim_copy(line.substr(0, eq)) == "name") {
            name = trim_copy(line.substr(eq + 1));
        }
    }
    std::thread([state, name]() {
        std::string err;
        bool ok = relay_set_device_name(name, err);
        struct RenameCtx {
            AppState *state;
            bool ok;
            std::string msg;
        };
        auto *ctx = new RenameCtx{
            state,
            ok,
            ok ? ("本机设备名称已更新为「" + name + "」，并同步至同账号设备列表。") : err
        };
        g_idle_add(+[](gpointer d) -> gboolean {
            std::unique_ptr<RenameCtx> p(static_cast<RenameCtx *>(d));
            if (p->state && !p->state->shutting_down.load()) {
                emit_relay_feedback(p->state, p->ok, p->msg);
                push_relay_state(p->state, true);
            }
            return G_SOURCE_REMOVE;
        }, ctx);
    }).detach();
}

void relay_device_remove_ipc(AppState *state, const std::string &payload) {
    std::string id;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (trim_copy(line.substr(0, eq)) == "device_id") id = trim_copy(line.substr(eq + 1));
    }
    if (id.empty()) return;
    std::thread([state, id]() {
        std::string err;
        bool ok = relay_remove_device(id, err);
        struct RmCtx {
            AppState *state;
            bool ok;
            std::string msg;
        };
        auto *ctx = new RmCtx{state, ok, ok ? "已移除绑定设备。" : err};
        g_idle_add(+[](gpointer d) -> gboolean {
            std::unique_ptr<RmCtx> p(static_cast<RmCtx *>(d));
            if (p->state && !p->state->shutting_down.load()) {
                emit_relay_feedback(p->state, p->ok, p->msg);
                push_relay_state(p->state, p->ok);
            }
            return G_SOURCE_REMOVE;
        }, ctx);
    }).detach();
}

void relay_reset_fp_ipc(AppState *state) {
    RelayConfig cfg = load_relay_config();
    cfg.server_fp.clear();
    save_relay_config(cfg);
    emit_relay_feedback(state, true, "已清除旧 TLS 证书指纹，下次连接服务器将重新信任。");
    push_relay_state(state, false);
}

void relay_logout_ipc(AppState *state) {
    relay_logout();
    emit_relay_feedback(state, true, "已退出 Spinel 账号并停止远程宿主服务。");
    push_relay_state(state, false);
}

void relay_connect_remote_ipc(AppState *state, const std::string &device_id) {
    if (device_id.empty()) return;
    native_shell_sync_sandbox(state, "remote:" + device_id);
    emit_relay_feedback(state, true, "正在建立端到端加密隧道并开启远程平行沙箱画中画…");
}

void check_spinelos_ota_async(AppState *state) {
    if (!state || state->shutting_down.load()) return;
    std::thread([state]() {
        std::string bin = has_executable("spinel-update") ? "spinel-update" : "";
        if (bin.empty() && fs::exists("/usr/local/bin/spinel-update")) {
            bin = "/usr/local/bin/spinel-update";
        }
        std::string payload = "{\"ok\":false,\"error\":\"updater_missing\"}";
        if (!bin.empty()) {
            std::string out;
            int rc = 1;
            if (run_argv_capture({bin, "check", "--json"}, out, rc)) {
                std::string trimmed = trim_copy(out);
                auto line_pos = trimmed.rfind("\n{");
                if (line_pos != std::string::npos) trimmed = trimmed.substr(line_pos + 1);
                if (!trimmed.empty() && trimmed.front() == '{' && trimmed.back() == '}') {
                    payload = trimmed;
                }
            }
        }
        if (state->shutting_down.load()) return;
        struct OtaCbMsg {
            AppState *st;
            std::string json;
        };
        auto *msg = new OtaCbMsg{state, std::move(payload)};
        g_idle_add(+[](gpointer data) -> gboolean {
            std::unique_ptr<OtaCbMsg> m(static_cast<OtaCbMsg *>(data));
            if (m->st->shutting_down.load()) return G_SOURCE_REMOVE;
            std::string js = "window.__spinelOtaStatus && window.__spinelOtaStatus(" + m->json + ");";
            if (m->st->settings_view) eval_js(m->st->settings_view, js);
            if (m->st->store_view) eval_js(m->st->store_view, js);
            return G_SOURCE_REMOVE;
        }, msg);
    }).detach();
}
