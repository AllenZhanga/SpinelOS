#include "system_control.hpp"
#include "sys_internal.hpp"
#include "common.hpp"
#include "native_shell.hpp"

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
#include <sstream>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
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
    if (!state || !state->settings_view) return;
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
    gtk_window_set_title(ctx->win, "SpinelOS 系统日志与诊断 (原生排障视图)");
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
           << "SpinelOS Doctor Diagnostic Report (Redacted)\n"
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
    if (!state || !state->settings_view) return;
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
    eval_js(state->settings_view, js.str());
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
    if (state && state->settings_view) {
        eval_js(state->settings_view,
                "window.__spinelDoctorExported && window.__spinelDoctorExported(" +
                    std::string(ok ? "true" : "false") + ",\"" + json_escape(out_path.string()) + "\");");
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
    spawn_argv_detached({"sh", "-c", "pkill -f polkit-gnome-authentication-agent-1 2>/dev/null || true; sleep 0.1; setsid -f /usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1 >/tmp/spinel-polkit.log 2>&1 </dev/null"});
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
    if (!state || !state->settings_view) return;
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
    eval_js(state->settings_view, js.str());
}

void kill_process_by_pid(AppState *state, int pid, bool force) {
    if (pid <= 1 || pid == getpid()) {
        if (state && state->settings_view) {
            eval_js(state->settings_view,
                    "window.__spinelProcsFeedback && window.__spinelProcsFeedback(false, \"无法结束系统核心自身进程。\");");
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
    if (state && state->settings_view) {
        eval_js(state->settings_view,
                "window.__spinelProcsFeedback && window.__spinelProcsFeedback(" +
                    std::string(ok ? "true" : "false") + ",\"" + json_escape(msg) + "\");");
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
