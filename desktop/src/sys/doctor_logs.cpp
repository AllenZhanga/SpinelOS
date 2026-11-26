#include "system_control.hpp"
#include "sys_internal.hpp"
#include "common.hpp"
#include "desktop_apps.hpp"
#include "native_shell.hpp"
#include "relay_client.hpp"
#include "sandbox_manager.hpp"
#include "temp_memory.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#if SPINEL_HAS_WEBKIT
#include <malloc.h>
#endif
#include <signal.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

using sys_internal::detect_mem_usage;
using sys_internal::post_js_to_main;

namespace sys_internal {

std::string read_tail_lines(const fs::path &p, size_t max_lines) {
    std::ifstream ifs(p);
    if (!ifs.is_open()) return "";
    std::vector<std::string> ring;
    std::string line;
    while (std::getline(ifs, line)) {
        ring.push_back(std::move(line));
        if (ring.size() > max_lines) {
            ring.erase(ring.begin());
        }
    }
    std::ostringstream oss;
    for (size_t i = 0; i < ring.size(); ++i) {
        if (i > 0) oss << '\n';
        oss << ring[i];
    }
    return oss.str();
}

std::string collect_logs_bundle_text(const std::string &source) {
    if (source == "shell") {
        std::string txt = read_tail_lines("/tmp/spinel-shell.log", 260);
        if (txt.empty()) {
            txt = "暂无 /tmp/spinel-shell.log 记录（请确认从桌面自启或 dev-deploy 启动）。";
        }
        return "=== /tmp/spinel-shell.log ===\n" + txt;
    }

    if (source == "compositor") {
        std::string txt = read_tail_lines("/tmp/spinel-labwc.log", 220);
        if (txt.empty()) {
            txt = "暂无 /tmp/spinel-labwc.log 记录。";
        }
        return "=== /tmp/spinel-labwc.log ===\n" + txt;
    }

    if (source == "system") {
        std::ostringstream oss;
        oss << "=== 运行环境与图形诊断 ===\n";
        oss << "User: " << run_cmd_capture("id 2>/dev/null") << "\n";
        oss << "Kernel: " << read_first_line("/proc/sys/kernel/osrelease", "unknown") << "\n";
        oss << "Session: WAYLAND_DISPLAY=" << (std::getenv("WAYLAND_DISPLAY") ? std::getenv("WAYLAND_DISPLAY") : "")
            << " | WLR_RENDERER=" << (std::getenv("WLR_RENDERER") ? std::getenv("WLR_RENDERER") : "auto")
            << " | GSK_RENDERER=" << (std::getenv("GSK_RENDERER") ? std::getenv("GSK_RENDERER") : "auto") << "\n";
        oss << "WebKit: DMABUF_DISABLED=" << (std::getenv("WEBKIT_DISABLE_DMABUF_RENDERER") ? std::getenv("WEBKIT_DISABLE_DMABUF_RENDERER") : "0")
            << " | COMPOSITING_DISABLED=" << (std::getenv("WEBKIT_DISABLE_COMPOSITING_MODE") ? std::getenv("WEBKIT_DISABLE_COMPOSITING_MODE") : "0") << "\n";
        oss << "GPU:\n" << run_cmd_capture("lspci -k 2>/dev/null | grep -EA3 'VGA|3D|Display' || true") << "\n";
        oss << "DRM:\n" << run_cmd_capture("ls -l /dev/dri 2>/dev/null || true") << "\n\n";
        oss << "=== journalctl -b (最近 100 条) ===\n";
        std::string j = run_cmd_capture("journalctl -b -n 100 --no-pager 2>&1 || dmesg 2>&1 | tail -n 80");
        oss << (j.empty() ? "(无可用系统日志)" : j);
        return oss.str();
    }

    fs::path app_log = fs::path(spinel_state_dir()) / "apps.log";
    std::string txt = read_tail_lines(app_log, 240);
    if (txt.empty()) {
        txt = "暂无应用启动日志。\n点击桌面或游戏中心中的 Steam、主机大屏、终端或应用商店软件后，输出与报错会实时记录在:\n" + app_log.string();
    }
    std::ostringstream out;
    out << "=== " << app_log.string() << " ===\n" << txt;

    fs::path steam_console = fs::path(home_dir()) / ".local/share/Steam/logs/console-linux.txt";
    std::string st = read_tail_lines(steam_console, 45);
    if (!st.empty()) {
        out << "\n\n=== Steam console-linux.txt (最近 45 行) ===\n" << st;
    }
    fs::path steam_boot = fs::path(home_dir()) / ".local/share/Steam/logs/bootstrap_log.txt";
    std::string sb = read_tail_lines(steam_boot, 20);
    if (!sb.empty()) {
        out << "\n\n=== Steam bootstrap_log.txt (最近 20 行) ===\n" << sb;
    }
    return out.str();
}

}  // namespace sys_internal

using sys_internal::collect_logs_bundle_text;
using sys_internal::read_tail_lines;

namespace {

struct NativeLogWindowCtx {
    AppState *state = nullptr;
    GtkWindow *win = nullptr;
    GtkTextView *text_view = nullptr;
    std::string current_source = "apps";
    std::string banner;
};

void refresh_native_log_view(NativeLogWindowCtx *ctx) {
    if (!ctx || !ctx->text_view) return;
    std::string content;
    if (!ctx->banner.empty()) {
        content = "[提示] " + ctx->banner + "\n\n";
    }
    content += collect_logs_bundle_text(ctx->current_source);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(ctx->text_view);
    gtk_text_buffer_set_text(buf, content.c_str(), -1);
}

void on_native_log_tab_clicked(GtkButton *btn, gpointer user_data) {
    auto *ctx = static_cast<NativeLogWindowCtx *>(user_data);
    const char *src = static_cast<const char *>(g_object_get_data(G_OBJECT(btn), "log-src"));
    if (ctx && src) {
        ctx->current_source = src;
        refresh_native_log_view(ctx);
    }
}

void on_native_log_refresh_clicked(GtkButton *, gpointer user_data) {
    refresh_native_log_view(static_cast<NativeLogWindowCtx *>(user_data));
}

void on_native_log_copy_clicked(GtkButton *btn, gpointer user_data) {
    auto *ctx = static_cast<NativeLogWindowCtx *>(user_data);
    if (!ctx || !ctx->text_view) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(ctx->text_view);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    g_autofree char *txt = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    if (txt) {
        GdkDisplay *display = gdk_display_get_default();
        if (display) {
            gdk_clipboard_set_text(gdk_display_get_clipboard(display), txt);
            gtk_button_set_label(btn, "已复制!");
        }
    }
}

bool is_hex_pair(const std::string &s, size_t pos) {
    return pos + 1 < s.size() &&
           std::isxdigit(static_cast<unsigned char>(s[pos])) &&
           std::isxdigit(static_cast<unsigned char>(s[pos + 1]));
}

bool is_mac_at(const std::string &s, size_t pos) {
    if (pos + 17 > s.size()) return false;
    if (pos > 0 && std::isxdigit(static_cast<unsigned char>(s[pos - 1]))) return false;
    if (pos + 17 < s.size() && std::isxdigit(static_cast<unsigned char>(s[pos + 17]))) return false;
    for (int g = 0; g < 6; ++g) {
        if (!is_hex_pair(s, pos + g * 3)) return false;
        if (g < 5 && s[pos + g * 3 + 2] != ':') return false;
    }
    return true;
}

bool proc_running_name(const char *exact_name) {
    std::string out;
    int rc = 1;
    return run_argv_capture({"pgrep", "-x", exact_name}, out, rc) && rc == 0;
}

bool proc_running_pattern(const char *pat) {
    std::string out;
    int rc = 1;
    return run_argv_capture({"pgrep", "-f", pat}, out, rc) && rc == 0;
}

bool valid_app_id(const std::string &s) {
    if (s.empty() || s.size() > 160) return false;
    for (unsigned char c : s) {
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

std::vector<std::string> split_semi(const std::string &v) {
    std::vector<std::string> out;
    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ';')) {
        item = trim_copy(item);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

bool list_has_token(const std::vector<std::string> &tokens, const std::string &tok) {
    bool has = false;
    for (const auto &t : tokens) {
        if (t == tok) has = true;
        else if (t == ("!" + tok)) has = false;
    }
    return has;
}

struct FlatpakPermEntry {
    std::string id;
    std::string name;
    bool home = false;
    bool host = false;
    bool network = false;
    bool devices = false;
    bool dri = false;
    bool audio = false;
};

FlatpakPermEntry inspect_flatpak_app_perms(const std::string &id, const std::string &name) {
    FlatpakPermEntry e;
    e.id = id;
    e.name = name;
    std::string out;
    int rc = 1;
    if (!run_argv_capture({"flatpak", "info", "--show-permissions", id}, out, rc) || rc != 0) {
        return e;
    }
    std::istringstream iss(out);
    std::string line;
    std::string section;
    std::vector<std::string> fs_toks, sh_toks, dev_toks, sock_toks;
    while (std::getline(iss, line)) {
        line = trim_copy(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line;
            continue;
        }
        if (section != "[Context]") continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        auto toks = split_semi(v);
        if (k == "filesystems") fs_toks.insert(fs_toks.end(), toks.begin(), toks.end());
        else if (k == "shared") sh_toks.insert(sh_toks.end(), toks.begin(), toks.end());
        else if (k == "devices") dev_toks.insert(dev_toks.end(), toks.begin(), toks.end());
        else if (k == "sockets") sock_toks.insert(sock_toks.end(), toks.begin(), toks.end());
    }
    e.host = list_has_token(fs_toks, "host");
    e.home = e.host || list_has_token(fs_toks, "home");
    e.network = list_has_token(sh_toks, "network");
    e.devices = list_has_token(dev_toks, "all");
    e.dri = e.devices || list_has_token(dev_toks, "dri");
    e.audio = list_has_token(sock_toks, "pulseaudio") || list_has_token(sock_toks, "pipewire");
    return e;
}

struct ProcRow {
    int pid = 0;
    std::string name;
    std::string cmd;
    double cpu_pct = 0.0;
    long rss_mb = 0;
    bool is_core = false;
    bool own = false;
};

bool read_total_cpu_jiffies(unsigned long long &total_out, unsigned long long &idle_out) {
    std::ifstream ifs("/proc/stat");
    if (!ifs.is_open()) return false;
    std::string tag;
    unsigned long long u = 0, n = 0, s = 0, id = 0, io = 0, irq = 0, sirq = 0, st = 0;
    if (!(ifs >> tag >> u >> n >> s >> id >> io >> irq >> sirq >> st) || tag != "cpu") return false;
    idle_out = id + io;
    total_out = u + n + s + id + io + irq + sirq + st;
    return true;
}

constexpr size_t kMaxClipboardItems = 10;
constexpr size_t kMaxClipboardBytes = 65536;

std::string clip_history_path() {
    return (fs::path(spinel_state_dir()) / "clipboard_history.tsv").string();
}

std::string escape_clip_field(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

std::string unescape_clip_field(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char nx = s[++i];
            if (nx == 'n') out += '\n';
            else if (nx == 'r') out += '\r';
            else if (nx == 't') out += '\t';
            else out += nx;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<ClipboardItem> read_clip_items_unlocked() {
    std::vector<ClipboardItem> items;
    std::ifstream ifs(clip_history_path());
    if (!ifs.is_open()) return items;
    std::string line;
    while (std::getline(ifs, line) && items.size() < kMaxClipboardItems) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        long long ts = std::strtoll(line.substr(0, tab).c_str(), nullptr, 10);
        std::string txt = unescape_clip_field(line.substr(tab + 1));
        if (!txt.empty()) {
            items.push_back({std::move(txt), ts});
        }
    }
    return items;
}

void write_clip_items_unlocked(const std::vector<ClipboardItem> &items) {
    std::ostringstream oss;
    size_t count = std::min(items.size(), kMaxClipboardItems);
    for (size_t i = 0; i < count; ++i) {
        oss << items[i].ts << "\t" << escape_clip_field(items[i].text) << "\n";
    }
    write_text_file(clip_history_path(), oss.str());
}

void push_clip_item_locked(const std::string &text) {
    if (text.empty() || text.size() > kMaxClipboardBytes) return;
    bool has_non_ws = false;
    for (unsigned char c : text) {
        if (!std::isspace(c)) { has_non_ws = true; break; }
    }
    if (!has_non_ws) return;

    std::string lock_path = (fs::path(spinel_state_dir()) / "clipboard.lock").string();
    int lfd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
    if (lfd >= 0) ::flock(lfd, LOCK_EX);

    std::vector<ClipboardItem> items = read_clip_items_unlocked();
    long long now_ts = static_cast<long long>(std::time(nullptr));
    items.erase(std::remove_if(items.begin(), items.end(), [&](const ClipboardItem &it) {
        return it.text == text;
    }), items.end());
    items.insert(items.begin(), ClipboardItem{text, now_ts});
    if (items.size() > kMaxClipboardItems) items.resize(kMaxClipboardItems);
    write_clip_items_unlocked(items);

    if (lfd >= 0) {
        ::flock(lfd, LOCK_UN);
        ::close(lfd);
    }
}

pid_t g_clip_watcher_pid = -1;

}  // namespace

/* ==================== Logs & Native Log Window ==================== */

void push_system_logs(AppState *state, const std::string &source) {
    if (!state || (!state->monitor_view && !state->settings_view)) return;
    std::string src = (source == "shell" || source == "compositor" || source == "system") ? source : "apps";
    std::thread([state, src]() {
        std::string txt = collect_logs_bundle_text(src);
        std::ostringstream js;
        js << "window.__spinelLogsData && window.__spinelLogsData({"
           << "\"source\":\"" << src << "\","
           << "\"text\":\"" << json_escape(txt) << "\""
           << "});";
        post_js_to_main(state, js.str(), false);
    }).detach();
}

void clear_system_logs(AppState *state, const std::string &source) {
    if (!state) return;
    fs::path app_log = fs::path(spinel_state_dir()) / "apps.log";
    std::error_code ec;
    fs::resize_file(app_log, 0, ec);
    append_app_log("[Logs] 已清空应用启动日志");
    push_system_logs(state, source);
}

void open_native_log_window(AppState *state, const std::string &banner) {
    auto *ctx = new NativeLogWindowCtx{state, nullptr, nullptr, "apps", banner};

    GtkWidget *win = gtk_window_new();
    ctx->win = GTK_WINDOW(win);
    gtk_window_set_title(ctx->win, "Spinel OS 系统日志与诊断 (原生排障视图)");
    gtk_window_set_default_size(ctx->win, 880, 580);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    struct SrcBtn { const char *label; const char *src; };
    const SrcBtn btns[] = {
        {"应用 / Steam", "apps"},
        {"桌面壳 (spinel-shell)", "shell"},
        {"合成器 (labwc)", "compositor"},
        {"系统与环境", "system"}
    };
    for (const auto &b : btns) {
        GtkWidget *w = gtk_button_new_with_label(b.label);
        g_object_set_data_full(G_OBJECT(w), "log-src", g_strdup(b.src), g_free);
        g_signal_connect(w, "clicked", G_CALLBACK(on_native_log_tab_clicked), ctx);
        gtk_box_append(GTK_BOX(bar), w);
    }

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer);

    GtkWidget *refresh_btn = gtk_button_new_with_label("刷新");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_native_log_refresh_clicked), ctx);
    gtk_box_append(GTK_BOX(bar), refresh_btn);

    GtkWidget *copy_btn = gtk_button_new_with_label("复制日志");
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_native_log_copy_clicked), ctx);
    gtk_box_append(GTK_BOX(bar), copy_btn);

    gtk_box_append(GTK_BOX(root), bar);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);

    GtkWidget *tv = gtk_text_view_new();
    ctx->text_view = GTK_TEXT_VIEW(tv);
    gtk_text_view_set_editable(ctx->text_view, FALSE);
    gtk_text_view_set_monospace(ctx->text_view, TRUE);
    gtk_text_view_set_wrap_mode(ctx->text_view, GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), tv);
    gtk_box_append(GTK_BOX(root), scroll);

    gtk_window_set_child(ctx->win, root);
    g_signal_connect_swapped(win, "destroy", G_CALLBACK(+[](gpointer data) {
        delete static_cast<NativeLogWindowCtx *>(data);
    }), ctx);

    refresh_native_log_view(ctx);
    gtk_window_present(ctx->win);
}

/* ==================== Spinel Doctor & Redaction ==================== */

std::string redact_sensitive_text(const std::string &raw) {
    std::string out;
    out.reserve(raw.size());

    auto is_delim = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
               ch == '"' || ch == '\'' || ch == '&' || ch == ',' || ch == ')';
    };

    std::string lower;
    lower.resize(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(raw[i])));
    }

    static const char *kSecretKeys[] = {
        "password=", "password:", "passwd=", "psk=", "wifi-sec.psk:", "token=", "secret="
    };

    for (size_t i = 0; i < raw.size();) {
        // 1. Mask /home/<user>
        if (raw.compare(i, 6, "/home/") == 0 && (i == 0 || !std::isalnum(static_cast<unsigned char>(raw[i - 1])))) {
            size_t end = i + 6;
            while (end < raw.size() && raw[end] != '/' && !is_delim(raw[end]) && raw[end] != ':') {
                ++end;
            }
            if (end > i + 6) {
                out += "/home/<REDACTED>";
                i = end;
                continue;
            }
        }

        // 2. Mask URL user[:pass]@host credentials
        if (raw.compare(i, 3, "://") == 0) {
            out += "://";
            i += 3;
            size_t scan = i;
            size_t at_pos = std::string::npos;
            while (scan < raw.size() && raw[scan] != '/' && !is_delim(raw[scan])) {
                if (raw[scan] == '@') {
                    at_pos = scan;
                    break;
                }
                ++scan;
            }
            if (at_pos != std::string::npos && at_pos > i) {
                out += "***@";
                i = at_pos + 1;
            }
            continue;
        }

        // 3. Mask key=secret / key:secret tokens
        bool matched_secret = false;
        for (const char *k : kSecretKeys) {
            size_t klen = std::strlen(k);
            if (lower.compare(i, klen, k) == 0 &&
                (i == 0 || !std::isalnum(static_cast<unsigned char>(lower[i - 1])))) {
                out.append(raw, i, klen);
                i += klen;
                while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) {
                    out += raw[i++];
                }
                while (i < raw.size() && !is_delim(raw[i])) {
                    ++i;
                }
                out += "<REDACTED>";
                matched_secret = true;
                break;
            }
        }
        if (matched_secret) continue;

        // 4. Mask MAC / BSSID addresses
        if (is_mac_at(raw, i)) {
            out.append(raw, i, 8);  // keep OUI XX:XX:XX
            out += ":**:**:**";
            i += 17;
            continue;
        }

        out += raw[i++];
    }
    return out;
}

GpuDriverUpdateInfo collect_gpu_driver_update_info() {
    GpuDriverUpdateInfo info;
    std::error_code ec;
    info.kernel_running = trim_copy(read_first_line("/proc/sys/kernel/osrelease", "Linux"));
    if (!info.kernel_running.empty() && !fs::exists("/usr/lib/modules/" + info.kernel_running, ec)) {
        info.reboot_required = true;
    }

    std::map<std::string, std::string> pkg_vers;
    if (has_executable("pacman")) {
        std::string q_out;
        int q_rc = 1;
        run_argv_capture({"pacman", "-Q", "linux", "nvidia-open", "nvidia", "nvidia-utils", "mesa", "vulkan-radeon", "vulkan-intel"}, q_out, q_rc);
        for (const auto &line : split_lines(q_out)) {
            std::istringstream iss(line);
            std::string pkg, ver;
            if (iss >> pkg >> ver) pkg_vers[pkg] = ver;
        }
        if (auto it = pkg_vers.find("linux"); it != pkg_vers.end()) {
            info.kernel_installed = it->second;
        }

        std::string qu_out;
        int qu_rc = 1;
        if (run_argv_capture({"pacman", "-Qu"}, qu_out, qu_rc) && qu_rc == 0) {
            for (const auto &line : split_lines(qu_out)) {
                std::string t = trim_copy(line);
                if (t.empty()) continue;
                ++info.updates_total;
                std::istringstream iss(t);
                std::string pname;
                iss >> pname;
                if (pname == "linux" || pname.rfind("linux-firmware", 0) == 0 ||
                    pname.rfind("nvidia", 0) == 0 || pname.rfind("lib32-nvidia", 0) == 0 ||
                    pname == "mesa" || pname == "lib32-mesa" ||
                    pname.rfind("vulkan-", 0) == 0 || pname.rfind("lib32-vulkan-", 0) == 0) {
                    info.gpu_updates.push_back(t);
                }
            }
        }
    }

    std::vector<std::string> parts;
    std::string nv_mod = trim_copy(read_first_line("/sys/module/nvidia/version", ""));
    if (auto it = pkg_vers.find("nvidia-open"); it != pkg_vers.end()) {
        parts.push_back("NVIDIA Open " + (!nv_mod.empty() ? nv_mod : it->second));
    } else if (auto it2 = pkg_vers.find("nvidia"); it2 != pkg_vers.end()) {
        parts.push_back("NVIDIA " + (!nv_mod.empty() ? nv_mod : it2->second));
    } else if (auto it3 = pkg_vers.find("nvidia-utils"); it3 != pkg_vers.end()) {
        parts.push_back("NVIDIA " + it3->second);
    }
    if (auto it = pkg_vers.find("mesa"); it != pkg_vers.end()) {
        parts.push_back("Mesa " + it->second);
    }
    if (parts.empty()) parts.push_back("DRM / Mesa");
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) info.driver_summary += " · ";
        info.driver_summary += parts[i];
    }
    return info;
}

std::vector<DoctorCheckItem> run_system_health_checks() {
    std::vector<DoctorCheckItem> checks;
    std::error_code ec;

    // 1. Wayland & DRM / Pixman Stack
    {
        const char *wd = std::getenv("WAYLAND_DISPLAY");
        const char *wlr = std::getenv("WLR_RENDERER");
        bool has_dri = false;
        if (fs::is_directory("/dev/dri", ec)) {
            for (const auto &e : fs::directory_iterator("/dev/dri", ec)) {
                if (e.path().filename().string().rfind("renderD", 0) == 0) {
                    has_dri = true;
                    break;
                }
            }
        }
        bool pixman = (wlr && std::string(wlr) == "pixman");
        DoctorCheckItem it;
        it.id = "wayland_drm";
        it.name = "Wayland 与图形渲染";
        if (has_dri) {
            it.status = "ok";
            it.detail = std::string("DRM 硬件渲染节点就绪") + (wd ? (" · WAYLAND=" + std::string(wd)) : "");
        } else if (pixman) {
            it.status = "warn";
            it.detail = "当前运行在虚拟机软件 Pixman 兼容模式（无 3D 硬件加速节点）";
            it.fix_hint = "如需 3D 游戏加速，请在虚拟机开启 VirGL / 显卡直通并在物理机启动";
        } else {
            it.status = "warn";
            it.detail = "未检测到 /dev/dri/renderD* 渲染节点";
            it.fix_hint = "检查显卡驱动加载（amdgpu / i915 / nvidia_drm.modeset=1）";
        }
        checks.push_back(std::move(it));
    }

    // 2. Vulkan 64-bit / 32-bit ICDs
    {
        std::vector<std::string> icds;
        bool has_32 = false;
        fs::path icd_dir = "/usr/share/vulkan/icd.d";
        if (fs::is_directory(icd_dir, ec)) {
            for (const auto &e : fs::directory_iterator(icd_dir, ec)) {
                if (e.path().extension() == ".json") {
                    std::string fn = e.path().filename().string();
                    icds.push_back(fn);
                    if (fn.find("i686") != std::string::npos || fn.find("32") != std::string::npos) {
                        has_32 = true;
                    }
                }
            }
        }
        if (fs::exists("/usr/lib32/libvulkan.so.1", ec) && !icds.empty()) {
            if (fs::exists("/usr/lib32/libGLX_nvidia.so.0", ec)) has_32 = true;
        }
        DoctorCheckItem it;
        it.id = "vulkan_icd";
        it.name = "Vulkan 64/32 位驱动";
        if (icds.empty()) {
            it.status = "fail";
            it.detail = "未发现 /usr/share/vulkan/icd.d/*.json";
            it.fix_hint = "请确认已安装 vulkan-radeon / vulkan-intel / nvidia-utils";
        } else if (!has_32) {
            it.status = "warn";
            it.detail = "已检测到 " + std::to_string(icds.size()) + " 个 ICD，但未确认 32 位 Vulkan 运行时";
            it.fix_hint = "确保启用 [multilib] 并安装 lib32-vulkan-*（影响 Steam/Proton 32 位游戏）";
        } else {
            it.status = "ok";
            it.detail = "已就绪（" + std::to_string(icds.size()) + " 个 ICD，含 32 位兼容库）";
        }
        checks.push_back(std::move(it));
    }

    // 2.5 GPU Driver Versions, Pending Reboot & Package Updates
    {
        GpuDriverUpdateInfo gi = collect_gpu_driver_update_info();
        DoctorCheckItem drv;
        drv.id = "gpu_driver_ver";
        drv.name = "显卡驱动与内核版本";
        drv.detail = gi.driver_summary + " · 内核 " + gi.kernel_running +
                     (!gi.kernel_installed.empty() ? (" (包 " + gi.kernel_installed + ")") : "");
        if (gi.reboot_required) {
            drv.status = "warn";
            drv.fix_hint = "检测到硬盘已升级新版内核/驱动，请重启系统以加载新内核模块";
        } else {
            drv.status = "ok";
        }
        checks.push_back(std::move(drv));

        DoctorCheckItem upd;
        upd.id = "sys_updates";
        upd.name = "系统与显卡驱动更新";
        if (gi.updates_total > 0) {
            upd.status = "warn";
            upd.detail = "发现 " + std::to_string(gi.updates_total) + " 项可升级软件包";
            if (!gi.gpu_updates.empty()) {
                upd.detail += "（含显卡驱动/内核 " + std::to_string(gi.gpu_updates.size()) + " 项）";
            }
            upd.fix_hint = "前往「应用商店 → 一键更新系统」同步升级至最新版驱动与内核";
        } else {
            upd.status = "ok";
            upd.detail = "已对照本地软件源索引，内核与显卡驱动均为最新";
        }
        checks.push_back(std::move(upd));
    }

    // 3. PipeWire Audio Stack
    {
        bool has_wp = has_executable("wpctl");
        bool pw_up = proc_running_name("pipewire") && proc_running_name("wireplumber");
        DoctorCheckItem it;
        it.id = "pipewire";
        it.name = "PipeWire 音频与 WirePlumber";
        if (has_wp && pw_up) {
            it.status = "ok";
            it.detail = "PipeWire 与 WirePlumber 运行正常";
        } else if (has_wp) {
            it.status = "warn";
            it.detail = "wpctl 已安装，但用户态 pipewire/wireplumber 守护未运行";
            it.fix_hint = "运行 systemctl --user restart pipewire wireplumber";
        } else {
            it.status = "fail";
            it.detail = "缺少 WirePlumber (wpctl) 组件";
            it.fix_hint = "请通过 pacman 安装 pipewire pipewire-pulse wireplumber";
        }
        checks.push_back(std::move(it));
    }

    // 4. Polkit & XDG Portals
    {
        bool pk_up = proc_running_pattern("polkit-gnome-authentication-agent-1");
        bool pt_up = proc_running_pattern("xdg-desktop-portal");
        DoctorCheckItem it;
        it.id = "portals_polkit";
        it.name = "权限弹窗与 Portal 授权";
        if (pk_up && pt_up) {
            it.status = "ok";
            it.detail = "Polkit 提权弹窗与 xdg-desktop-portal 运行中";
        } else {
            it.status = "warn";
            it.detail = std::string(pk_up ? "Polkit 正常" : "Polkit Agent 未运行") + " · " +
                        (pt_up ? "Portal 正常" : "xdg-desktop-portal 未运行");
            it.fix_hint = "可在「权限与安全」页点击重启 Polkit 或刷新 Portal";
        }
        checks.push_back(std::move(it));
    }

    // 5. Fcitx5 IME & Mako Notifications
    {
        bool ime_up = proc_running_name("fcitx5");
        bool mako_up = proc_running_name("mako");
        DoctorCheckItem it;
        it.id = "ime_notify";
        it.name = "中文输入法与通知服务";
        if (ime_up && mako_up) {
            it.status = "ok";
            it.detail = "Fcitx5 Wayland 与 Mako 通知守护均在运行";
        } else {
            it.status = "warn";
            it.detail = std::string(ime_up ? "Fcitx5 正常" : "Fcitx5 未运行") + " · " +
                        (mako_up ? "Mako 正常" : "Mako 未运行");
            it.fix_hint = "可在「外观与输入法」点重启输入法，或执行 ./scripts/dev-deploy.sh --full";
        }
        checks.push_back(std::move(it));
    }

    // 6. zram & Power Profiles
    {
        bool zram = !trim_copy(run_cmd_capture("swapon --noheadings --show=NAME 2>/dev/null")).empty();
        bool ppd = has_executable("powerprofilesctl");
        DoctorCheckItem it;
        it.id = "mem_power";
        it.name = "zram 内存压缩与电源调度";
        if (zram && ppd) {
            it.status = "ok";
            it.detail = "zram 已激活 · power-profiles-daemon 可用";
        } else {
            it.status = "warn";
            it.detail = std::string(zram ? "zram 已激活" : "zram 未激活") + " · " +
                        (ppd ? "电源调度可用" : "缺少 powerprofilesctl");
            it.fix_hint = "检查 systemd-zram-setup@zram0 与 power-profiles-daemon 服务状态";
        }
        checks.push_back(std::move(it));
    }

    // 7. Network & Steam
    {
        TelemetrySnapshot tel = collect_telemetry();
        bool has_steam = has_executable("steam");
        bool has_net = (tel.net != "未连接" && !tel.net.empty());
        DoctorCheckItem it;
        it.id = "net_steam";
        it.name = "网络连接与 Steam 就绪度";
        if (has_net && has_steam) {
            it.status = "ok";
            it.detail = "网络 (" + tel.net + ") · Steam 客户端已安装";
        } else if (has_net) {
            it.status = "warn";
            it.detail = "网络 (" + tel.net + ") · 专有 Steam 尚未安装（开源合规定制）";
            it.fix_hint = "在应用商店点击「Steam」一键在线安装即可畅玩";
        } else {
            it.status = "warn";
            it.detail = std::string("当前网络未连接") + (has_steam ? " · Steam 已安装" : " · Steam 未安装");
            it.fix_hint = "请在「Wi-Fi 与网络」连接网络或配置系统代理";
        }
        checks.push_back(std::move(it));
    }

    return checks;
}

std::string build_redacted_diagnostic_report() {
    auto checks = run_system_health_checks();
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&now, &tmv);
    char tbuf[64]{};
    std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S %Z", &tmv);

    TelemetrySnapshot tel = collect_telemetry();
    ProxyConfig proxy = load_proxy_config();

    std::ostringstream report;
    report << "=====================================================\n"
           << "Spinel OS Doctor Diagnostic Report (Redacted)\n"
           << "Generated: " << tbuf << "\n"
           << "Kernel:    " << tel.kernel << "\n"
           << "GPU:       " << tel.gpu << "\n"
           << "Memory:    " << tel.mem << "\n"
           << "Mode:      " << (tel.is_live ? "Live ISO" : "Installed Disk") << "\n"
           << "Proxy:     " << (proxy.enabled ? ("enabled (" + proxy.http_proxy + " / " + proxy.socks_proxy + ")") : "disabled") << "\n"
           << "=====================================================\n\n"
           << "[Health Checks]\n";
    for (const auto &c : checks) {
        report << " - [" << c.status << "] " << c.name << ": " << c.detail << "\n";
        if (!c.fix_hint.empty()) {
            report << "     hint: " << c.fix_hint << "\n";
        }
    }
    report << "\n" << collect_logs_bundle_text("system")
           << "\n\n" << collect_logs_bundle_text("compositor")
           << "\n\n" << collect_logs_bundle_text("shell")
           << "\n\n" << collect_logs_bundle_text("apps") << "\n";
    return redact_sensitive_text(report.str());
}

void push_doctor_report(AppState *state) {
    if (!state || (!state->doctor_view && !state->settings_view)) return;
    auto checks = run_system_health_checks();
    int ok_c = 0, warn_c = 0, fail_c = 0;
    std::ostringstream js;
    js << "window.__spinelDoctorData && window.__spinelDoctorData({\"checks\":[";
    for (size_t i = 0; i < checks.size(); ++i) {
        if (i > 0) js << ",";
        const auto &c = checks[i];
        if (c.status == "ok") ++ok_c;
        else if (c.status == "warn") ++warn_c;
        else ++fail_c;
        js << "{"
           << "\"id\":\"" << json_escape(c.id) << "\","
           << "\"name\":\"" << json_escape(c.name) << "\","
           << "\"status\":\"" << json_escape(c.status) << "\","
           << "\"detail\":\"" << json_escape(c.detail) << "\","
           << "\"fix_hint\":\"" << json_escape(c.fix_hint) << "\""
           << "}";
    }
    js << "],\"ok_count\":" << ok_c
       << ",\"warn_count\":" << warn_c
       << ",\"fail_count\":" << fail_c << "});";
    if (state->doctor_view) eval_js(state->doctor_view, js.str());
    if (state->settings_view) eval_js(state->settings_view, js.str());
}

void export_diagnostic_report(AppState *state) {
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&now, &tmv);
    char fname[64]{};
    std::strftime(fname, sizeof(fname), "SpinelOS-Doctor-%Y%m%d-%H%M%S.txt", &tmv);

    fs::path out_path = fs::path(home_dir()) / "Desktop" / fname;
    bool ok = write_text_file(out_path.string(), build_redacted_diagnostic_report());
    append_app_log(ok ? ("[Doctor] 已导出脱敏诊断报告: " + out_path.string()) : "[Doctor] 导出诊断报告失败");
    if (state) {
        std::string script = "window.__spinelDoctorExported && window.__spinelDoctorExported(" +
            std::string(ok ? "true" : "false") + ",\"" + json_escape(out_path.string()) + "\");";
        if (state->doctor_view) eval_js(state->doctor_view, script);
        if (state->settings_view) eval_js(state->settings_view, script);
    }
}

/* ==================== Security, Perms & Processes ==================== */

void push_perms_state(AppState *state) {
    if (!state || !state->settings_view) return;
    std::string out;
    int rc = 1;
    bool polkit_installed = fs::exists("/usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1");
    bool polkit_running = run_argv_capture({"pgrep", "-f", "polkit-gnome-authentication-agent-1"}, out, rc) && rc == 0;
    bool portal_running = run_argv_capture({"pgrep", "-x", "xdg-desktop-portal"}, out, rc) && rc == 0;
    bool portal_gtk = run_argv_capture({"pgrep", "-x", "xdg-desktop-portal-gtk"}, out, rc) && rc == 0;
    bool portal_wlr = run_argv_capture({"pgrep", "-x", "xdg-desktop-portal-wlr"}, out, rc) && rc == 0;
    std::error_code ec;
    bool portal_conf = fs::exists("/etc/xdg/xdg-desktop-portal/spinelos-portals.conf", ec) ||
                       fs::exists("/etc/xdg/xdg-desktop-portal/portals.conf", ec);

    std::vector<FlatpakPermEntry> apps;
    if (has_executable("flatpak")) {
        std::string list_out;
        int list_rc = 1;
        if (run_argv_capture({"flatpak", "list", "--app", "--columns=application,name"}, list_out, list_rc) && list_rc == 0) {
            std::istringstream iss(list_out);
            std::string line;
            while (std::getline(iss, line)) {
                line = trim_copy(line);
                if (line.empty()) continue;
                auto tab = line.find('\t');
                std::string id = trim_copy(tab == std::string::npos ? line : line.substr(0, tab));
                std::string name = trim_copy(tab == std::string::npos ? line : line.substr(tab + 1));
                if (!valid_app_id(id)) continue;
                apps.push_back(inspect_flatpak_app_perms(id, name.empty() ? id : name));
            }
        }
    }

    struct PortalEntry {
        std::string table;
        std::string id;
        std::string app;
        std::string perms;
    };
    std::vector<PortalEntry> portal_entries;
    if (has_executable("flatpak")) {
        std::string p_out;
        int p_rc = 1;
        if (run_argv_capture({"flatpak", "permission-show"}, p_out, p_rc) && p_rc == 0) {
            std::istringstream iss(p_out);
            std::string line;
            while (std::getline(iss, line)) {
                line = trim_copy(line);
                if (line.empty() || line.rfind("Table", 0) == 0) continue;
                std::istringstream ls(line);
                std::string tbl, pid, papp, rest;
                if (!(ls >> tbl >> pid >> papp)) continue;
                std::getline(ls, rest);
                rest = trim_copy(rest);
                if (valid_app_id(papp) || papp == "all") {
                    portal_entries.push_back({tbl, pid, papp, rest});
                }
            }
        }
    }

    std::ostringstream js;
    js << "window.__spinelPermsData && window.__spinelPermsData({"
       << "\"polkit_installed\":" << (polkit_installed ? "true" : "false") << ","
       << "\"polkit_running\":" << (polkit_running ? "true" : "false") << ","
       << "\"portal_running\":" << (portal_running ? "true" : "false") << ","
       << "\"portal_gtk\":" << (portal_gtk ? "true" : "false") << ","
       << "\"portal_wlr\":" << (portal_wlr ? "true" : "false") << ","
       << "\"portal_conf\":" << (portal_conf ? "true" : "false") << ","
       << "\"apps\":[";
    for (size_t i = 0; i < apps.size(); ++i) {
        if (i > 0) js << ",";
        js << "{"
           << "\"id\":\"" << json_escape(apps[i].id) << "\","
           << "\"name\":\"" << json_escape(apps[i].name) << "\","
           << "\"home\":" << (apps[i].home ? "true" : "false") << ","
           << "\"host\":" << (apps[i].host ? "true" : "false") << ","
           << "\"network\":" << (apps[i].network ? "true" : "false") << ","
           << "\"devices\":" << (apps[i].devices ? "true" : "false") << ","
           << "\"dri\":" << (apps[i].dri ? "true" : "false") << ","
           << "\"audio\":" << (apps[i].audio ? "true" : "false")
           << "}";
    }
    js << "],\"portal_entries\":[";
    for (size_t i = 0; i < portal_entries.size(); ++i) {
        if (i > 0) js << ",";
        js << "{"
           << "\"table\":\"" << json_escape(portal_entries[i].table) << "\","
           << "\"id\":\"" << json_escape(portal_entries[i].id) << "\","
           << "\"app\":\"" << json_escape(portal_entries[i].app) << "\","
           << "\"perms\":\"" << json_escape(portal_entries[i].perms) << "\""
           << "}";
    }
    js << "]});";
    eval_js(state->settings_view, js.str());
    push_sandbox_state(state);
}

void apply_flatpak_perm(AppState *state, const std::string &payload) {
    if (!has_executable("flatpak")) return;
    std::string app_id, key, val;
    std::istringstream iss(payload);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim_copy(line);
        if (line.rfind("app=", 0) == 0) app_id = line.substr(4);
        else if (line.rfind("key=", 0) == 0) key = line.substr(4);
        else if (line.rfind("val=", 0) == 0) val = line.substr(4);
    }
    if (!valid_app_id(app_id)) return;
    bool enable = (val == "true" || val == "1");
    std::string flag;
    if (key == "home") flag = enable ? "--filesystem=home" : "--nofilesystem=home";
    else if (key == "host") flag = enable ? "--filesystem=host" : "--nofilesystem=host";
    else if (key == "network") flag = enable ? "--share=network" : "--unshare=network";
    else if (key == "devices") flag = enable ? "--device=all" : "--nodevice=all";
    else if (key == "dri") flag = enable ? "--device=dri" : "--nodevice=dri";
    else if (key == "audio") flag = enable ? "--socket=pulseaudio" : "--nosocket=pulseaudio";
    else return;

    std::string out;
    int rc = 1;
    run_argv_capture({"flatpak", "override", "--user", flag, app_id}, out, rc);
    append_app_log("[Perms] flatpak override " + flag + " " + app_id + (rc == 0 ? " (OK)" : " (ERR)"));
    if (state && state->settings_view) {
        std::string msg = rc == 0 ? ("已更新 " + app_id + " 权限 (" + flag + ")，重启该应用生效。") : ("权限修改失败: " + out);
        eval_js(state->settings_view,
                "window.__spinelPermsFeedback && window.__spinelPermsFeedback(" +
                    std::string(rc == 0 ? "true" : "false") + ",\"" + json_escape(msg) + "\");");
    }
    push_perms_state(state);
}

void reset_flatpak_app_perms(AppState *state, const std::string &app_id) {
    if (!valid_app_id(app_id) || !has_executable("flatpak")) return;
    std::string out;
    int rc = 1;
    run_argv_capture({"flatpak", "override", "--user", "--reset", app_id}, out, rc);
    append_app_log("[Perms] 恢复应用默认权限: " + app_id);
    if (state && state->settings_view) {
        std::string msg = rc == 0 ? ("已将 " + app_id + " 权限恢复为应用默认。") : ("重置失败: " + out);
        eval_js(state->settings_view,
                "window.__spinelPermsFeedback && window.__spinelPermsFeedback(" +
                    std::string(rc == 0 ? "true" : "false") + ",\"" + json_escape(msg) + "\");");
    }
    push_perms_state(state);
}

void reset_portal_perm(AppState *state, const std::string &payload) {
    if (!has_executable("flatpak")) return;
    std::string table, pid, app_id;
    std::istringstream iss(payload);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim_copy(line);
        if (line.rfind("table=", 0) == 0) table = line.substr(6);
        else if (line.rfind("id=", 0) == 0) pid = line.substr(3);
        else if (line.rfind("app=", 0) == 0) app_id = line.substr(4);
    }
    if (table.empty() || pid.empty() || app_id.empty()) return;
    std::string out;
    int rc = 1;
    run_argv_capture({"flatpak", "permission-remove", table, pid, app_id}, out, rc);
    append_app_log("[Perms] 撤销动态授权记录: " + table + " / " + pid + " / " + app_id);
    push_perms_state(state);
}

void restart_polkit_agent(AppState *state) {
    const char *agent = "/usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1";
    if (!fs::exists(agent)) {
        if (state && state->settings_view) {
            eval_js(state->settings_view,
                    "window.__spinelPermsFeedback && window.__spinelPermsFeedback(false, \"尚未安装 polkit-gnome，请先运行 ./scripts/dev-deploy.sh --full 安装权限代理组件。\");");
        }
        return;
    }
    spawn_argv_detached({"sh", "-c", "pkill -f '/usr/lib/polkit-gnome/[p]olkit-gnome-authentication-agent-1' 2>/dev/null || true; sleep 0.1; setsid -f /usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1 >/tmp/spinel-polkit.log 2>&1 </dev/null"});
    append_app_log("[Perms] 已启动 Polkit 管理员授权弹窗代理");
    if (state) {
        std::thread([state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            post_js_to_main(state, "window.__spinelPermsFeedback && window.__spinelPermsFeedback(true, \"Polkit 管理员授权弹窗代理已就绪。\");", false);
            g_idle_add([](gpointer d) -> gboolean {
                auto *st = static_cast<AppState *>(d);
                if (!st->shutting_down.load()) push_perms_state(st);
                return G_SOURCE_REMOVE;
            }, state);
        }).detach();
    }
}

void reload_desktop_portals(AppState *state) {
    spawn_argv_detached({"sh", "-c", "dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP GTK_THEME 2>/dev/null || true; systemctl --user restart xdg-desktop-portal xdg-desktop-portal-gtk xdg-desktop-portal-wlr >/dev/null 2>&1 || true"});
    append_app_log("[Perms] 已重载 xdg-desktop-portal 权限弹窗网关");
    if (state) {
        std::thread([state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            post_js_to_main(state, "window.__spinelPermsFeedback && window.__spinelPermsFeedback(true, \"已重载权限弹窗服务。\");", false);
            g_idle_add([](gpointer d) -> gboolean {
                auto *st = static_cast<AppState *>(d);
                if (!st->shutting_down.load()) push_perms_state(st);
                return G_SOURCE_REMOVE;
            }, state);
        }).detach();
    }
}

void push_process_list(AppState *state) {
    if (!state || (!state->monitor_view && !state->settings_view)) return;
    static unsigned long long prev_total = 0;
    static unsigned long long prev_idle = 0;
    static std::map<int, unsigned long long> prev_proc_ticks;

    unsigned long long cur_total = 0, cur_idle = 0;
    read_total_cpu_jiffies(cur_total, cur_idle);
    unsigned long long delta_total = (prev_total > 0 && cur_total > prev_total) ? (cur_total - prev_total) : 0;
    unsigned long long delta_idle = (prev_idle > 0 && cur_idle >= prev_idle) ? (cur_idle - prev_idle) : 0;
    double sys_cpu = (delta_total > 0)
        ? std::clamp(100.0 * static_cast<double>(delta_total - delta_idle) / static_cast<double>(delta_total), 0.0, 100.0)
        : 0.0;
    long num_cpus = std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
    long page_kb = std::max(1L, sysconf(_SC_PAGESIZE) / 1024);

    uid_t my_uid = getuid();
    std::map<int, unsigned long long> next_ticks;
    std::vector<ProcRow> rows;
    int total_procs = 0;

    std::error_code ec;
    for (const auto &entry : fs::directory_iterator("/proc", ec)) {
        std::string fname = entry.path().filename().string();
        if (fname.empty() || !std::all_of(fname.begin(), fname.end(), ::isdigit)) continue;
        int pid = std::atoi(fname.c_str());
        if (pid <= 0) continue;
        ++total_procs;

        std::ifstream stat_ifs(entry.path() / "stat");
        if (!stat_ifs.is_open()) continue;
        std::string stat_line;
        std::getline(stat_ifs, stat_line);
        auto lp = stat_line.find('(');
        auto rp = stat_line.rfind(')');
        if (lp == std::string::npos || rp == std::string::npos || rp <= lp) continue;

        std::string comm = stat_line.substr(lp + 1, rp - lp - 1);
        std::istringstream rest(stat_line.substr(rp + 2));
        std::string state_ch;
        long ppid = 0, pgrp = 0, sess = 0, tty = 0, tpgid = 0;
        unsigned long flags = 0, minflt = 0, cminflt = 0, majflt = 0, cmajflt = 0;
        unsigned long long utime = 0, stime = 0;
        long cutime = 0, cstime = 0, prio = 0, nicev = 0, nthreads = 0, itreal = 0;
        unsigned long long start = 0, vsize = 0;
        long rss_pages = 0;
        rest >> state_ch >> ppid >> pgrp >> sess >> tty >> tpgid >> flags >> minflt >> cminflt
             >> majflt >> cmajflt >> utime >> stime >> cutime >> cstime >> prio >> nicev
             >> nthreads >> itreal >> start >> vsize >> rss_pages;

        unsigned long long p_ticks = utime + stime;
        next_ticks[pid] = p_ticks;
        double cpu_pct = 0.0;
        if (delta_total > 0) {
            auto it = prev_proc_ticks.find(pid);
            if (it != prev_proc_ticks.end() && p_ticks >= it->second) {
                cpu_pct = 100.0 * static_cast<double>(p_ticks - it->second) * static_cast<double>(num_cpus) /
                          static_cast<double>(delta_total);
            }
        }
        long rss_mb = (rss_pages * page_kb) / 1024;
        std::ifstream rollup_ifs(entry.path() / "smaps_rollup");
        if (rollup_ifs.is_open()) {
            std::string rline;
            while (std::getline(rollup_ifs, rline)) {
                if (rline.rfind("Pss:", 0) == 0) {
                    long pss_kb = std::strtol(rline.c_str() + 4, nullptr, 10);
                    if (pss_kb > 0) rss_mb = pss_kb / 1024;
                    break;
                }
            }
        }
        if (rss_mb < 2 && cpu_pct < 0.2) continue;

        struct stat st_buf{};
        bool own = (::stat(entry.path().c_str(), &st_buf) == 0 && st_buf.st_uid == my_uid);

        std::string cmdline;
        std::ifstream cmd_ifs(entry.path() / "cmdline", std::ios::binary);
        if (cmd_ifs.is_open()) {
            std::string raw((std::istreambuf_iterator<char>(cmd_ifs)), std::istreambuf_iterator<char>());
            for (char &c : raw) if (c == '\0') c = ' ';
            cmdline = trim_copy(raw);
            if (cmdline.size() > 96) cmdline = cmdline.substr(0, 93) + "...";
        }
        if (cmdline.empty()) cmdline = comm;

        bool is_core = (pid <= 2 || comm == "labwc" || comm == "spinel-shell" || comm == "systemd" ||
                        comm == "dbus-daemon" || comm == "dbus-broker");
        rows.push_back({pid, comm, cmdline, cpu_pct, rss_mb, is_core, own});
    }

    prev_total = cur_total;
    prev_idle = cur_idle;
    prev_proc_ticks = std::move(next_ticks);

    std::sort(rows.begin(), rows.end(), [](const ProcRow &a, const ProcRow &b) {
        if (std::abs(a.cpu_pct - b.cpu_pct) > 0.4) return a.cpu_pct > b.cpu_pct;
        return a.rss_mb > b.rss_mb;
    });
    if (rows.size() > 60) rows.resize(60);

    int mem_used = 0;
    std::string mem_str = detect_mem_usage(&mem_used);

    std::ostringstream js;
    js << "window.__spinelProcessList && window.__spinelProcessList({"
       << "\"cpu_pct\":" << static_cast<int>(sys_cpu + 0.5) << ","
       << "\"mem\":\"" << json_escape(mem_str) << "\","
       << "\"tmpfs_bytes\":";
    if (const auto bytes = tmpfs_used_bytes("/tmp")) js << *bytes;
    else js << "null";
    js << ",\"memory_cleanup_running\":" << (state->memory_cleanup_running ? "true" : "false") << ","
       << "\"total_procs\":" << total_procs << ","
       << "\"procs\":[";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) js << ",";
        char cbuf[16];
        std::snprintf(cbuf, sizeof(cbuf), "%.1f", rows[i].cpu_pct);
        js << "{"
           << "\"pid\":" << rows[i].pid << ","
           << "\"name\":\"" << json_escape(rows[i].name) << "\","
           << "\"cmd\":\"" << json_escape(rows[i].cmd) << "\","
           << "\"cpu\":" << cbuf << ","
           << "\"mem_mb\":" << rows[i].rss_mb << ","
           << "\"is_core\":" << (rows[i].is_core ? "true" : "false") << ","
           << "\"own\":" << (rows[i].own ? "true" : "false")
           << "}";
    }
    js << "]});";
    eval_js(state->monitor_view, js.str());
    eval_js(state->settings_view, js.str());
}

void cleanup_monitor_memory(AppState *state) {
#if SPINEL_HAS_WEBKIT
    GApplication *app = g_application_get_default();
    if (!state || !state->monitor_view || !app || state->memory_cleanup_running) return;
    struct Cleanup {
        GApplication *app;
        AppState *state;
        TempMemoryCleanup temporary;
    };
    auto *cleanup = new Cleanup{app, state, {}};
    state->memory_cleanup_running = true;
    g_application_hold(app);
    g_autoptr(GTask) task = g_task_new(app, nullptr,
        +[](GObject *, GAsyncResult *async, gpointer data) {
            auto *cleanup = static_cast<Cleanup *>(data);
            std::unique_ptr<TempMemoryCleanup> result(
                static_cast<TempMemoryCleanup *>(g_task_propagate_pointer(G_TASK(async), nullptr)));
            if (result) cleanup->temporary = *result;
            else cleanup->temporary.incomplete = true;

            auto *session = webkit_network_session_get_default();
            auto *manager = webkit_network_session_get_website_data_manager(session);
            webkit_website_data_manager_clear(manager, WEBKIT_WEBSITE_DATA_MEMORY_CACHE, 0, nullptr,
                +[](GObject *source, GAsyncResult *async, gpointer data) {
                    std::unique_ptr<Cleanup> cleanup(static_cast<Cleanup *>(data));
                    const bool cache_cleared = webkit_website_data_manager_clear_finish(
                        WEBKIT_WEBSITE_DATA_MANAGER(source), async, nullptr);
                    const bool heap_released = malloc_trim(0) != 0;
                    cleanup->state->memory_cleanup_running = false;

                    const auto &temporary = cleanup->temporary;
                    std::ostringstream script;
                    script << "(window.__spinelMemoryCleanup || window.__spinelTempCleanup)?.("
                           << temporary.directories_removed << ",";
                    if (temporary.released_bytes) script << *temporary.released_bytes;
                    else script << "null";
                    script << "," << (temporary.incomplete ? "true" : "false")
                           << "," << (cache_cleared ? "true" : "false")
                           << "," << (heap_released ? "true" : "false") << ");";
                    eval_js(cleanup->state->monitor_view, script.str());
                    g_application_release(cleanup->app);
                }, cleanup);
        }, cleanup);
    g_task_run_in_thread(task, +[](GTask *task, gpointer, gpointer, GCancellable *) {
        g_task_return_pointer(task, new TempMemoryCleanup(clean_temp_memory("/tmp")), +[](gpointer result) {
            delete static_cast<TempMemoryCleanup *>(result);
        });
    });
#else
    (void)state;
#endif
}

void kill_process_by_pid(AppState *state, int pid, bool force) {
    if (pid <= 1 || pid == getpid()) {
        if (state) {
            const char *self_err =
                "window.__spinelProcsFeedback && window.__spinelProcsFeedback(false, \"无法结束系统核心自身进程。\");";
            eval_js(state->monitor_view, self_err);
            eval_js(state->settings_view, self_err);
        }
        return;
    }
    int sig = force ? SIGKILL : SIGTERM;
    int rc = ::kill(static_cast<pid_t>(pid), sig);
    bool ok = (rc == 0);
    std::string msg = ok
        ? ("已向进程 PID " + std::to_string(pid) + (force ? " 发送强制结束 (SIGKILL)" : " 发送结束信号 (SIGTERM)"))
        : ("无法结束 PID " + std::to_string(pid) + "（可能需要管理员权限或进程已退出）");
    append_app_log("[Proc] " + msg);
    if (state) {
        std::string script =
            "window.__spinelProcsFeedback && window.__spinelProcsFeedback(" +
            std::string(ok ? "true" : "false") + ",\"" + json_escape(msg) + "\");";
        eval_js(state->monitor_view, script);
        eval_js(state->settings_view, script);
    }
    if (state) {
        std::thread([state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(180));
            g_idle_add([](gpointer d) -> gboolean {
                auto *st = static_cast<AppState *>(d);
                if (!st->shutting_down.load()) push_process_list(st);
                return G_SOURCE_REMOVE;
            }, state);
        }).detach();
    }
}

/* ==================== Notifications, DND & OOBE ==================== */

void sync_notification_theme(bool is_light) {
    fs::path mako_cfg = fs::path(home_dir()) / ".config/mako/config";
    std::ostringstream oss;
    oss << "# SpinelOS Wayland Notification Daemon (mako)\n"
        << "font=Noto Sans CJK SC 11\n"
        << "border-radius=12\n"
        << "border-size=1\n"
        << "padding=12,14\n"
        << "margin=12,12,38,12\n"
        << "width=340\n"
        << "default-timeout=5000\n"
        << "layer=overlay\n"
        << "anchor=top-right\n";
    if (is_light) {
        oss << "background-color=#fffffff0\n"
            << "text-color=#0f172a\n"
            << "border-color=#0f172a24\n"
            << "progress-color=over #0284c733\n";
    } else {
        oss << "background-color=#111827f0\n"
            << "text-color=#f8fafc\n"
            << "border-color=#ffffff2e\n"
            << "progress-color=over #38bdf833\n";
    }
    oss << "\n[mode=do-not-disturb]\n"
        << "invisible=1\n";
    if (write_text_file(mako_cfg.string(), oss.str()) && has_executable("makoctl")) {
        spawn_argv_detached({"makoctl", "reload"});
    }
}

bool get_dnd_enabled() {
    std::string val = trim_copy(read_first_line(
        (fs::path(home_dir()) / ".config/spinelos/dnd.conf").string(), "false"
    ));
    return val == "true" || val == "1";
}

void toggle_dnd_mode(AppState *state) {
    bool next = !get_dnd_enabled();
    fs::path p = fs::path(home_dir()) / ".config/spinelos/dnd.conf";
    write_text_file(p.string(), next ? "true\n" : "false\n");
    if (has_executable("makoctl")) {
        spawn_argv_detached({"makoctl", "mode", next ? "-a" : "-r", "do-not-disturb"});
    }
    append_app_log(std::string("[Notify] 勿扰模式已") + (next ? "开启" : "关闭"));
    native_shell_sync_perf(state);
}

std::vector<NotificationHistoryItem> fetch_notification_history() {
    std::vector<NotificationHistoryItem> items;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus) return items;

    auto collect_from = [&](const char *method) {
        GVariant *outer = g_dbus_connection_call_sync(
            bus, "org.freedesktop.Notifications", "/fr/emersion/Mako", "fr.emersion.Mako",
            method, nullptr, G_VARIANT_TYPE("(aa{sv})"),
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 250, nullptr, nullptr);
        if (!outer) {
            outer = g_dbus_connection_call_sync(
                bus, "fr.emersion.Mako", "/fr/emersion/Mako", "fr.emersion.Mako",
                method, nullptr, G_VARIANT_TYPE("(aa{sv})"),
                G_DBUS_CALL_FLAGS_NO_AUTO_START, 250, nullptr, nullptr);
        }
        if (!outer) return;
        GVariant *arr = g_variant_get_child_value(outer, 0);
        gsize n = g_variant_n_children(arr);
        for (gsize i = 0; i < n && items.size() < 20; ++i) {
            GVariant *dict = g_variant_get_child_value(arr, i);
            auto get_str = [&](const char *k) -> std::string {
                GVariant *v = g_variant_lookup_value(dict, k, G_VARIANT_TYPE_STRING);
                if (!v) return "";
                const char *s = g_variant_get_string(v, nullptr);
                std::string out = s ? s : "";
                g_variant_unref(v);
                return out;
            };
            NotificationHistoryItem it;
            if (GVariant *vid = g_variant_lookup_value(dict, "id", nullptr)) {
                if (g_variant_is_of_type(vid, G_VARIANT_TYPE_UINT32)) it.id = g_variant_get_uint32(vid);
                else if (g_variant_is_of_type(vid, G_VARIANT_TYPE_INT32)) it.id = static_cast<uint32_t>(std::max(0, g_variant_get_int32(vid)));
                g_variant_unref(vid);
            }
            it.app_name = get_str("app-name");
            it.summary = get_str("summary");
            it.body = get_str("body");
            if (!it.summary.empty() || !it.body.empty()) {
                bool dup = false;
                for (const auto &ex : items) {
                    if (ex.id != 0 && ex.id == it.id) { dup = true; break; }
                }
                if (!dup) items.push_back(std::move(it));
            }
            g_variant_unref(dict);
        }
        g_variant_unref(arr);
        g_variant_unref(outer);
    };

    collect_from("ListNotifications");
    collect_from("ListHistory");
    g_object_unref(bus);
    return items;
}

void dismiss_all_notifications() {
    if (has_executable("makoctl")) {
        std::string out;
        int rc = 1;
        if (run_argv_capture({"makoctl", "dismiss", "-a"}, out, rc) && rc == 0) {
            return;
        }
    }
    if (GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr)) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "all", g_variant_new_boolean(TRUE));
        GVariant *args = g_variant_new("(@a{sv})", g_variant_builder_end(&b));
        GVariant *res = g_dbus_connection_call_sync(
            bus, "org.freedesktop.Notifications", "/fr/emersion/Mako", "fr.emersion.Mako",
            "DismissNotifications", args, nullptr,
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 250, nullptr, nullptr);
        if (res) g_variant_unref(res);
        g_object_unref(bus);
    }
}

bool is_welcome_completed() {
    std::error_code ec;
    return fs::exists(fs::path(home_dir()) / ".config/spinelos/welcome_done", ec);
}

void mark_welcome_completed() {
    fs::path p = fs::path(home_dir()) / ".config/spinelos/welcome_done";
    write_text_file(p.string(), "completed=1\n");
    append_app_log("[OOBE] 首次开机引导已完成");
}

void push_welcome_status(AppState *state) {
    if (!state || !state->welcome_view) return;
    TelemetrySnapshot tel = collect_telemetry();
    ProxyConfig p = load_proxy_config();
    bool steam_installed = has_executable("steam");
    bool completed = is_welcome_completed();
    std::ostringstream js;
    js << "window.__spinelWelcomeStatus && window.__spinelWelcomeStatus({"
       << "\"steam_installed\":" << (steam_installed ? "true" : "false") << ","
       << "\"proxy_enabled\":" << (p.enabled ? "true" : "false") << ","
       << "\"completed\":" << (completed ? "true" : "false") << ","
       << "\"is_live\":" << (tel.is_live ? "true" : "false") << ","
       << "\"gpu\":\"" << json_escape(tel.gpu) << "\","
       << "\"net\":\"" << json_escape(tel.net) << "\""
       << "});";
    eval_js(state->welcome_view, js.str());
}

/* ==================== Clipboard History & Watcher ==================== */

int run_clipboard_hook_from_stdin() {
    std::string buf;
    std::array<char, 4096> chunk{};
    ssize_t n = 0;
    while ((n = ::read(STDIN_FILENO, chunk.data(), chunk.size())) > 0) {
        if (buf.size() + static_cast<size_t>(n) > kMaxClipboardBytes) {
            return 0;  // Skip huge clipboard blobs
        }
        buf.append(chunk.data(), static_cast<size_t>(n));
    }
    push_clip_item_locked(buf);
    return 0;
}

std::vector<ClipboardItem> load_clipboard_history() {
    std::string lock_path = (fs::path(spinel_state_dir()) / "clipboard.lock").string();
    int lfd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
    if (lfd >= 0) ::flock(lfd, LOCK_SH);
    auto items = read_clip_items_unlocked();
    if (lfd >= 0) {
        ::flock(lfd, LOCK_UN);
        ::close(lfd);
    }
    return items;
}

void clear_clipboard_history() {
    std::string lock_path = (fs::path(spinel_state_dir()) / "clipboard.lock").string();
    int lfd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
    if (lfd >= 0) ::flock(lfd, LOCK_EX);
    std::error_code ec;
    fs::remove(clip_history_path(), ec);
    if (lfd >= 0) {
        ::flock(lfd, LOCK_UN);
        ::close(lfd);
    }
}

void copy_text_to_clipboard(const std::string &text) {
    if (text.empty()) return;
    push_clip_item_locked(text);
    if (!has_executable("wl-copy")) return;

    int pipefd[2]{-1, -1};
    if (::pipe(pipefd) != 0) return;

    pid_t pid = ::fork();
    if (pid == 0) {
        pid_t grand = ::fork();
        if (grand != 0) _exit(0);
        ::setsid();
        ::dup2(pipefd[0], STDIN_FILENO);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > 2) ::close(devnull);
        }
        ::execlp("wl-copy", "wl-copy", nullptr);
        _exit(127);
    }
    ::close(pipefd[0]);
    if (pid > 0) {
        size_t off = 0;
        while (off < text.size()) {
            ssize_t w = ::write(pipefd[1], text.data() + off, text.size() - off);
            if (w <= 0) break;
            off += static_cast<size_t>(w);
        }
        ::close(pipefd[1]);
        ::waitpid(pid, nullptr, 0);
    } else {
        ::close(pipefd[1]);
    }
}

void start_clipboard_watcher() {
    if (g_clip_watcher_pid > 0 || !has_executable("wl-paste")) return;
    std::array<char, 512> self_buf{};
    ssize_t slen = ::readlink("/proc/self/exe", self_buf.data(), self_buf.size() - 1);
    std::string self_exe = (slen > 0) ? std::string(self_buf.data(), static_cast<size_t>(slen))
                                      : "/usr/local/bin/spinel-shell";

    pid_t pid = ::fork();
    if (pid == 0) {
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > 2) ::close(devnull);
        }
        ::execlp("wl-paste", "wl-paste", "--no-newline", "--type", "text", "--watch",
                 self_exe.c_str(), "--clip-hook", nullptr);
        _exit(127);
    }
    if (pid > 0) {
        g_clip_watcher_pid = pid;
    }
}

void stop_clipboard_watcher() {
    if (g_clip_watcher_pid > 0) {
        ::kill(g_clip_watcher_pid, SIGTERM);
        ::waitpid(g_clip_watcher_pid, nullptr, 0);
        g_clip_watcher_pid = -1;
    }
}

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

/* ---------------- Agent Parallel Sandbox Settings & IPC ---------------- */

namespace {

void emit_sandbox_feedback(AppState *state, bool ok, const std::string &msg) {
    if (!state || !state->settings_view) return;
    eval_js(state->settings_view,
            "window.__spinelSandboxFeedback && window.__spinelSandboxFeedback(" +
                std::string(ok ? "true" : "false") + ",\"" + json_escape(msg) + "\");");
}

struct SandboxDirPickCtx {
    AppState *state = nullptr;
    bool writable = true;
};

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

/* ---------------- Cloud Relay & Cross-Device Continuity IPC ---------------- */

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
