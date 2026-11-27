#include "system_control.hpp"
#include "sys_internal.hpp"
#include "common.hpp"
#include "desktop_apps.hpp"
#include "native_shell.hpp"
#include "sandbox_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtk/gtk.h>

namespace fs = std::filesystem;

using sys_internal::post_js_to_main;

/* ==================== Btrfs Snapper Snapshots ==================== */

namespace {

std::vector<std::string> split_csv_row(const std::string &line) {
    std::vector<std::string> fields;
    std::string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                cur.push_back('"');
                ++i;
            } else {
                in_quotes = !in_quotes;
            }
        } else if (c == ',' && !in_quotes) {
            fields.push_back(trim_copy(cur));
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    fields.push_back(trim_copy(cur));
    return fields;
}

bool check_snap_sudo_nopass() {
    std::string out;
    int rc = 1;
    return run_argv_capture({"sudo", "-n", "true"}, out, rc) && rc == 0;
}

void emit_snapshots_feedback(AppState *state, bool ok, const std::string &msg, bool reboot_required = false) {
    append_app_log(std::string("[Snapper] ") + (ok ? "" : "[ERR] ") + msg);
    std::ostringstream js;
    js << "window.__spinelSnapshotsFeedback && window.__spinelSnapshotsFeedback("
       << (ok ? "true" : "false") << ",\"" << json_escape(msg) << "\","
       << (reboot_required ? "true" : "false") << ");";
    post_js_to_main(state, js.str(), false);
}

bool run_snapper_sudo_cmd(const std::vector<std::string> &args,
                          const std::string &password,
                          std::string &out,
                          int &rc) {
    bool nopass = check_snap_sudo_nopass();
    if (!nopass && password.empty()) {
        out = "需要当前用户管理员密码（sudo）才能操作 Btrfs 系统快照。";
        rc = 2;
        return false;
    }
    std::vector<std::string> cmd = nopass
        ? std::vector<std::string>{"sudo", "-n"}
        : std::vector<std::string>{"sudo", "-S", "-p", ""};
    cmd.insert(cmd.end(), args.begin(), args.end());
    return run_argv_stdin_capture(cmd, nopass ? "" : password, out, rc) && rc == 0;
}

std::string sanitize_snapper_desc(const std::string &raw) {
    std::string s = trim_copy(raw);
    std::string clean;
    clean.reserve(std::min<size_t>(s.size(), 80));
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f || c == '"' || c == '`' || c == '$') continue;
        clean.push_back(static_cast<char>(c));
        if (clean.size() >= 80) break;
    }
    return trim_copy(clean);
}

}  // namespace

bool is_valid_snapshot_id(const std::string &s, int &out_id) {
    std::string t = trim_copy(s);
    if (t.empty() || t.size() > 10) return false;
    for (unsigned char c : t) {
        if (!std::isdigit(c)) return false;
    }
    long long v = std::atoll(t.c_str());
    if (v <= 0 || v > 100000000) return false;
    out_id = static_cast<int>(v);
    return true;
}

bool is_valid_snapper_desc(const std::string &desc) {
    std::string clean = sanitize_snapper_desc(desc);
    return !clean.empty();
}

std::vector<SnapperSnapshotInfo> parse_snapper_csv(const std::string &csv) {
    std::vector<SnapperSnapshotInfo> items;
    bool header_seen = false;
    for (const auto &raw_line : split_lines(csv)) {
        std::string line = trim_copy(raw_line);
        if (line.empty()) continue;
        auto cols = split_csv_row(line);
        if (cols.empty()) continue;
        if (!header_seen && (cols[0] == "number" || cols[0] == "#")) {
            header_seen = true;
            continue;
        }
        std::string num_str = cols[0];
        bool is_cur = (num_str.find('*') != std::string::npos);
        std::string digits;
        for (unsigned char c : num_str) {
            if (std::isdigit(c)) digits.push_back(static_cast<char>(c));
        }
        if (digits.empty()) continue;
        int id = std::atoi(digits.c_str());
        if (id == 0) continue;  // Snapper internal snapshot #0 represents live subvolume baseline

        SnapperSnapshotInfo info;
        info.id = id;
        info.is_current = is_cur;
        if (cols.size() > 1) info.type = cols[1];
        if (cols.size() > 2) info.date = cols[2];
        if (cols.size() > 3) info.user = cols[3];
        if (cols.size() > 4) info.cleanup = cols[4];
        if (cols.size() > 5) info.description = cols[5];
        items.push_back(std::move(info));
    }
    std::reverse(items.begin(), items.end());
    return items;
}

void push_snapshots_state(AppState *state) {
    if (!state) return;
    std::thread([state]() {
        std::string root_fs = "unknown";
        std::string f_out;
        int f_rc = 1;
        if (run_argv_capture({"findmnt", "-n", "-o", "FSTYPE", "/"}, f_out, f_rc) && f_rc == 0) {
            std::string trimmed = trim_copy(f_out);
            if (!trimmed.empty()) root_fs = trimmed;
        }
        bool live_iso = is_live_iso_env();
        bool snapper_bin = has_executable("snapper");
        std::error_code ec;
        bool config_ready = fs::exists("/etc/snapper/configs/root", ec);
        bool sudo_nopass = check_snap_sudo_nopass();

        std::vector<SnapperSnapshotInfo> snaps;
        if (!live_iso && root_fs == "btrfs" && snapper_bin) {
            std::string csv_out;
            int s_rc = 1;
            const std::vector<std::string> cmd = {
                "snapper", "-c", "root", "--csvout", "list", "--columns", "number,type,date,user,cleanup,description"
            };
            bool ok = run_argv_capture(cmd, csv_out, s_rc) && s_rc == 0;
            if (!ok && sudo_nopass) {
                std::vector<std::string> scmd = {"sudo", "-n"};
                scmd.insert(scmd.end(), cmd.begin(), cmd.end());
                ok = run_argv_capture(scmd, csv_out, s_rc) && s_rc == 0;
            }
            if (ok) {
                config_ready = true;
                snaps = parse_snapper_csv(csv_out);
            }
        }

        std::ostringstream js;
        js << "window.__spinelSnapshotsData && window.__spinelSnapshotsData({"
           << "\"root_fs\":\"" << json_escape(root_fs) << "\","
           << "\"is_live\":" << (live_iso ? "true" : "false") << ","
           << "\"snapper_installed\":" << (snapper_bin ? "true" : "false") << ","
           << "\"config_ready\":" << (config_ready ? "true" : "false") << ","
           << "\"sudo_nopass\":" << (sudo_nopass ? "true" : "false") << ","
           << "\"snapshots\":[";
        for (size_t i = 0; i < snaps.size(); ++i) {
            if (i > 0) js << ",";
            const auto &s = snaps[i];
            js << "{"
               << "\"id\":" << s.id << ","
               << "\"type\":\"" << json_escape(s.type) << "\","
               << "\"date\":\"" << json_escape(s.date) << "\","
               << "\"user\":\"" << json_escape(s.user) << "\","
               << "\"cleanup\":\"" << json_escape(s.cleanup) << "\","
               << "\"description\":\"" << json_escape(s.description) << "\","
               << "\"is_current\":" << (s.is_current ? "true" : "false")
               << "}";
        }
        js << "]});";

        post_js_to_main(state, js.str(), false);
    }).detach();
}

void create_system_snapshot(AppState *state, const std::string &payload) {
    std::string desc = "SpinelOS 手动还原点";
    std::string password;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "desc") desc = v;
        else if (k == "password") password = v;
    }
    std::string clean_desc = sanitize_snapper_desc(desc);
    if (clean_desc.empty()) clean_desc = "SpinelOS 手动还原点";

    std::thread([state, clean_desc, password]() {
        if (!has_executable("snapper")) {
            emit_snapshots_feedback(state, false, "系统未安装 snapper。");
            return;
        }
        std::string out;
        int rc = 1;
        bool ok = run_snapper_sudo_cmd(
            {"snapper", "-c", "root", "create", "--cleanup-algorithm", "number", "--description", clean_desc},
            password, out, rc);
        if (!ok) {
            emit_snapshots_feedback(state, false, "创建系统快照失败: " + trim_copy(out));
        } else {
            emit_snapshots_feedback(state, true, "已成功创建 Btrfs 系统快照：「" + clean_desc + "」");
        }
        push_snapshots_state(state);
    }).detach();
}

void rollback_system_snapshot(AppState *state, const std::string &payload) {
    std::string id_raw;
    std::string password;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "id") id_raw = v;
        else if (k == "password") password = v;
    }
    int snap_id = 0;
    if (!is_valid_snapshot_id(id_raw, snap_id)) {
        emit_snapshots_feedback(state, false, "无效的快照编号。");
        return;
    }

    std::thread([state, snap_id, password]() {
        std::string out;
        int rc = 1;
        std::string id_s = std::to_string(snap_id);
        bool ok = run_snapper_sudo_cmd({"snapper", "-c", "root", "rollback", id_s}, password, out, rc);
        if (!ok) {
            emit_snapshots_feedback(state, false, "回滚快照 #" + id_s + " 失败: " + trim_copy(out));
        } else {
            emit_snapshots_feedback(
                state, true,
                "已准备回滚到快照 #" + id_s + "！请立即重启电脑以切换到回滚后的系统状态。", true);
        }
        push_snapshots_state(state);
    }).detach();
}

void delete_system_snapshot(AppState *state, const std::string &payload) {
    std::string id_raw;
    std::string password;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "id") id_raw = v;
        else if (k == "password") password = v;
    }
    int snap_id = 0;
    if (!is_valid_snapshot_id(id_raw, snap_id)) {
        emit_snapshots_feedback(state, false, "无效的快照编号。");
        return;
    }

    std::thread([state, snap_id, password]() {
        std::string out;
        int rc = 1;
        std::string id_s = std::to_string(snap_id);
        bool ok = run_snapper_sudo_cmd({"snapper", "-c", "root", "delete", id_s}, password, out, rc);
        if (!ok) {
            emit_snapshots_feedback(state, false, "删除快照 #" + id_s + " 失败: " + trim_copy(out));
        } else {
            emit_snapshots_feedback(state, true, "已删除快照 #" + id_s + "。");
        }
        push_snapshots_state(state);
    }).detach();
}

/* ==================== Agent Parallel Sandbox Settings & IPC ==================== */

namespace {

void emit_sandbox_feedback(AppState *state, bool ok, const std::string &msg) {
    if (!state || !state->settings_view) return;
    eval_js(state->settings_view,
            "window.__spinelSandboxFeedback && window.__spinelSandboxFeedback(" +
                std::string(ok ? "true" : "false") + ",\"" + json_escape(msg) + "\");");
}

}  // namespace

void push_sandbox_state(AppState *state) {
    if (!state || !state->settings_view) return;
    eval_js(state->settings_view,
            "window.__spinelSandboxData && window.__spinelSandboxData(" + sandbox_state_json() + ");");

    std::ostringstream cat;
    cat << "window.__spinelSandboxCatalog && window.__spinelSandboxCatalog([";
    struct BuiltinOpt {
        const char *name;
        const char *cmd;
    };
    static const BuiltinOpt kBuiltins[] = {
        {"Spinel 终端", "launch:terminal"},
        {"Spinel 文件管理器", "launch:files"},
        {"默认浏览器", "launch:browser"},
    };
    bool first = true;
    std::vector<std::string> seen;
    auto push_opt = [&](const std::string &name, const std::string &cmd) {
        if (name.empty() || cmd.empty()) return;
        if (std::find(seen.begin(), seen.end(), cmd) != seen.end()) return;
        seen.push_back(cmd);
        if (!first) cat << ",";
        first = false;
        cat << "{\"name\":\"" << json_escape(name) << "\",\"cmd\":\"" << json_escape(cmd) << "\"}";
    };
    for (const auto &b : kBuiltins) push_opt(b.name, b.cmd);
    for (const auto &app : list_desktop_apps()) {
        if (app.id.empty()) continue;
        std::string cmd = "app:launch:" + app.id;
        std::string label = app.name.empty() ? app.id : app.name;
        push_opt(label, cmd);
    }
    cat << "]);";
    eval_js(state->settings_view, cat.str());
}

void set_sandbox_config(AppState *state, const std::string &payload) {
    SandboxConfig cfg = load_sandbox_config();
    bool clip_changed = false;
    bool net_changed = false;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        if (k == "allow_network") {
            bool nxt = (v != "false" && v != "0" && v != "no");
            net_changed = (nxt != cfg.allow_network);
            cfg.allow_network = nxt;
        } else if (k == "sync_clipboard") {
            bool nxt = (v != "false" && v != "0" && v != "no");
            clip_changed = (nxt != cfg.sync_clipboard);
            cfg.sync_clipboard = nxt;
        } else if (k == "perf_mode" && is_valid_sandbox_perf_mode(v)) {
            cfg.perf_mode = v;
        }
    }
    if (!save_sandbox_config(cfg)) {
        emit_sandbox_feedback(state, false, "保存平行沙箱配置失败。");
        push_sandbox_state(state);
        return;
    }
    if (clip_changed) {
        ensure_sandbox_clipboard_sync(cfg.sync_clipboard);
    }
    native_shell_sync_sandbox(state, "sync");
    if (net_changed && query_sandbox_status().running) {
        emit_sandbox_feedback(state, true, "已更新网络开关，新启动的沙箱应用将立即应用此策略（可重启沙箱使全部应用生效）。");
    } else {
        emit_sandbox_feedback(state, true, "平行沙箱配置已保存。");
    }
    push_sandbox_state(state);
}

void add_sandbox_dir(AppState *state, const std::string &payload) {
    std::string path;
    bool writable = true;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        if (k == "path") path = v;
        else if (k == "writable") writable = (v != "false" && v != "0");
    }

    if (path.empty()) {
        std::thread([state, writable]() {
            std::string bin = has_executable("spinel-files") ? "spinel-files" : "/usr/local/bin/spinel-files";
            std::string picked;
            int rc = 1;
            bool ran = run_argv_capture({bin, "--pick-dir", home_dir()}, picked, rc);
            std::string chosen = trim_copy(picked);

            struct PickDoneCtx {
                AppState *state;
                bool writable;
                bool ran;
                int rc;
                std::string chosen;
            };
            auto *ctx = new PickDoneCtx{state, writable, ran, rc, std::move(chosen)};
            g_idle_add(+[](gpointer d) -> gboolean {
                std::unique_ptr<PickDoneCtx> c(static_cast<PickDoneCtx *>(d));
                if (c->state && !c->state->shutting_down.load()) {
                    if (c->ran && c->rc == 0 && !c->chosen.empty()) {
                        std::string err;
                        if (add_sandbox_mount_dir(c->chosen, c->writable, err)) {
                            emit_sandbox_feedback(c->state, true,
                                                  std::string("已通过 Spinel Files 加入白名单（") +
                                                      (c->writable ? "读写" : "只读") +
                                                      "），下次在沙箱启动应用自动生效。");
                        } else {
                            emit_sandbox_feedback(c->state, false, err);
                        }
                        push_sandbox_state(c->state);
                    }
                }
                return G_SOURCE_REMOVE;
            }, ctx);
        }).detach();
        return;
    }

    std::string err;
    if (add_sandbox_mount_dir(path, writable, err)) {
        emit_sandbox_feedback(state, true, "已更新沙箱白名单目录。");
    } else {
        emit_sandbox_feedback(state, false, err);
    }
    push_sandbox_state(state);
}

void remove_sandbox_dir(AppState *state, const std::string &payload) {
    std::string path;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (trim_copy(line.substr(0, eq)) == "path") path = trim_copy(line.substr(eq + 1));
    }
    if (path.empty()) return;
    if (remove_sandbox_mount_dir(path)) {
        emit_sandbox_feedback(state, true, "已从沙箱白名单移除该目录。");
    } else {
        emit_sandbox_feedback(state, false, "未在白名单中找到该目录。");
    }
    push_sandbox_state(state);
}

void sandbox_session_action(AppState *state, const std::string &action) {
    std::thread([state, action]() {
        std::string err;
        bool ok = true;
        std::string msg;
        std::string shell_act;
        if (action == "start") {
            bool was_running = query_sandbox_status().running;
            ok = start_sandbox_session(err);
            if (ok) {
                if (!was_running) launch_sandbox_autostart_apps();
                shell_act = "show";
                msg = "平行沙箱桌面已启动，已在右下角显示实时画中画悬浮窗。";
            } else {
                msg = err;
            }
        } else if (action == "stop") {
            stop_sandbox_session(err);
            shell_act = "sync";
            msg = "已停止平行沙箱桌面，白名单目录文件已安全保留。";
        } else if (action == "reset") {
            ok = reset_sandbox_home(err);
            shell_act = "sync";
            msg = ok ? "已清空沙箱临时环境并恢复出厂干净状态（白名单目录不受影响）。" : err;
        } else if (action == "toggle-pip") {
            if (!query_sandbox_status().running) {
                ok = start_sandbox_session(err);
                if (ok) {
                    launch_sandbox_autostart_apps();
                    shell_act = "show";
                }
            } else {
                shell_act = "toggle";
            }
        }
        struct UiUpdate {
            AppState *state;
            bool ok;
            std::string msg;
            std::string shell_act;
        };
        auto *u = new UiUpdate{state, ok, std::move(msg), std::move(shell_act)};
        g_idle_add(+[](gpointer d) -> gboolean {
            std::unique_ptr<UiUpdate> up(static_cast<UiUpdate *>(d));
            if (up->state && !up->state->shutting_down.load()) {
                if (!up->shell_act.empty()) native_shell_sync_sandbox(up->state, up->shell_act);
                if (!up->msg.empty()) emit_sandbox_feedback(up->state, up->ok, up->msg);
                push_sandbox_state(up->state);
            }
            return G_SOURCE_REMOVE;
        }, u);
    }).detach();
}

void add_sandbox_app_ipc(AppState *state, const std::string &payload) {
    std::string name, cmd;
    bool auto_start = false;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        if (k == "name") name = v;
        else if (k == "cmd") cmd = v;
        else if (k == "auto_start") auto_start = (v == "true" || v == "1");
    }
    std::string err;
    if (add_sandbox_app(name, cmd, auto_start, err)) {
        native_shell_sync_sandbox(state, "sync");
        emit_sandbox_feedback(state, true, "已添加应用「" + (name.empty() ? cmd : name) + "」到沙箱快捷菜单。");
    } else {
        emit_sandbox_feedback(state, false, err);
    }
    push_sandbox_state(state);
}

void remove_sandbox_app_ipc(AppState *state, const std::string &payload) {
    std::string cmd;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (trim_copy(line.substr(0, eq)) == "cmd") cmd = trim_copy(line.substr(eq + 1));
    }
    if (cmd.empty()) return;
    if (remove_sandbox_app(cmd)) {
        native_shell_sync_sandbox(state, "sync");
        emit_sandbox_feedback(state, true, "已从沙箱快捷菜单移除该应用。");
    }
    push_sandbox_state(state);
}

void set_sandbox_app_autostart_ipc(AppState *state, const std::string &payload) {
    std::string cmd;
    bool auto_start = false;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        if (k == "cmd") cmd = v;
        else if (k == "auto_start") auto_start = (v == "true" || v == "1");
    }
    if (set_sandbox_app_autostart(cmd, auto_start)) {
        native_shell_sync_sandbox(state, "sync");
        emit_sandbox_feedback(state, true, auto_start ? "已设为启动沙箱时默认打开。" : "已取消默认自启。");
    }
    push_sandbox_state(state);
}

void launch_sandbox_app_ipc(AppState *state, const std::string &payload) {
    std::string cmd;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (trim_copy(line.substr(0, eq)) == "cmd") cmd = trim_copy(line.substr(eq + 1));
    }
    if (cmd.empty()) return;
    std::thread([state, cmd]() {
        std::string err;
        bool ok = launch_cmd_in_sandbox(cmd, err);
        struct LaunchRes {
            AppState *state;
            bool ok;
            std::string err;
        };
        auto *r = new LaunchRes{state, ok, std::move(err)};
        g_idle_add(+[](gpointer d) -> gboolean {
            std::unique_ptr<LaunchRes> p(static_cast<LaunchRes *>(d));
            if (p->state && !p->state->shutting_down.load()) {
                if (p->ok) {
                    native_shell_sync_sandbox(p->state, "show");
                    emit_sandbox_feedback(p->state, true, "已在平行沙箱中打开应用。");
                } else {
                    emit_sandbox_feedback(p->state, false, p->err);
                }
                push_sandbox_state(p->state);
            }
            return G_SOURCE_REMOVE;
        }, r);
    }).detach();
}
