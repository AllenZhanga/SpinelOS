#include "system_control.hpp"
#include "sys_internal.hpp"
#include "common.hpp"
#include "native_shell.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <gio/gio.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

namespace fs = std::filesystem;

namespace {

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
