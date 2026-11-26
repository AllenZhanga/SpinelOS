#include "sandbox_manager.hpp"

#include "common.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace {

std::string config_dir_path() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base = (xdg && *xdg) ? std::string(xdg) : (home_dir() + "/.config");
    return base + "/spinelos";
}

std::string sandbox_pid_file() { return sandbox_runtime_dir() + "/compositor.pid"; }
std::string sandbox_dbus_pid_file() { return sandbox_runtime_dir() + "/dbus.pid"; }
std::string sandbox_clip_pid_file() { return sandbox_runtime_dir() + "/clip-sync.pid"; }
std::string sandbox_clip_guard_file() { return sandbox_runtime_dir() + "/clip-last.hash"; }

int read_pid_file(const std::string &path) {
    std::string first = trim_copy(read_first_line(path));
    if (first.empty()) return -1;
    try {
        int pid = std::stoi(first);
        return pid > 1 ? pid : -1;
    } catch (...) {
        return -1;
    }
}

bool is_pid_alive(int pid) {
    if (pid <= 1) return false;
    int st = 0;
    pid_t reaped = ::waitpid(pid, &st, WNOHANG);
    if (reaped == pid) return false;
    return ::kill(pid, 0) == 0 || errno == EPERM;
}

bool spawn_headless_compositor_attempt(const std::string &rt_dir,
                                       const std::string &cfg_dir,
                                       const SandboxConfig &cfg,
                                       bool use_systemd_run,
                                       bool force_pixman,
                                       pid_t &out_pid) {
    std::vector<std::string> cmd;
    if (use_systemd_run && has_executable("systemd-run")) {
        bool host_prio = (cfg.perf_mode != "balanced");
        cmd = {
            "systemd-run", "--user", "--scope",
            "--slice=spinel-sandbox.slice",
            std::string("-pCPUWeight=") + (host_prio ? "20" : "50"),
            std::string("-pIOWeight=") + (host_prio ? "20" : "50"),
            std::string("-pMemoryHigh=") + (host_prio ? "8G" : "12G"),
            "--"
        };
    }

    cmd.push_back("/usr/bin/env");
    cmd.push_back("-u");
    cmd.push_back("WAYLAND_DISPLAY");
    cmd.push_back("-u");
    cmd.push_back("DISPLAY");
    cmd.push_back("XDG_RUNTIME_DIR=" + rt_dir);
    cmd.push_back("WLR_BACKENDS=headless");
    cmd.push_back("WLR_LIBINPUT_NO_DEVICES=1");
    cmd.push_back("WLR_HEADLESS_OUTPUTS=1");
    cmd.push_back("LABWC_CONFIG_DIR=" + cfg_dir);
    cmd.push_back("XCURSOR_THEME=Adwaita");
    if (force_pixman) {
        cmd.push_back("WLR_RENDERER=pixman");
        cmd.push_back("WLR_NO_HARDWARE_CURSORS=1");
        cmd.push_back("LIBGL_ALWAYS_SOFTWARE=1");
    }
    cmd.push_back("labwc");
    cmd.push_back("-C");
    cmd.push_back(cfg_dir);

    std::string log_path = rt_dir + "/labwc.log";
    pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();
        int log_fd = ::open(log_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (log_fd >= 0) {
            ::dup2(log_fd, STDOUT_FILENO);
            ::dup2(log_fd, STDERR_FILENO);
            if (log_fd > STDERR_FILENO) ::close(log_fd);
        }
        std::vector<char *> cargs;
        for (auto &a : cmd) cargs.push_back(const_cast<char *>(a.c_str()));
        cargs.push_back(nullptr);
        ::execvp(cargs[0], cargs.data());
        _exit(127);
    }
    if (pid < 0) return false;
    out_pid = pid;
    write_text_file(sandbox_pid_file(), std::to_string(pid) + "\n");

    std::error_code ec;
    std::string sock_path = rt_dir + "/" + sandbox_socket_name();
    for (int i = 0; i < 60; ++i) {
        if (fs::exists(sock_path, ec)) return true;
        if (!is_pid_alive(pid)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::kill(-pid, SIGTERM);
    ::kill(pid, SIGTERM);
    int st = 0;
    ::waitpid(pid, &st, WNOHANG);
    fs::remove(sandbox_pid_file(), ec);
    return false;
}

std::string host_runtime_dir() {
    const char *rt = std::getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) return rt;
    return "/run/user/" + std::to_string(static_cast<unsigned int>(::getuid()));
}

void ensure_private_dir(const std::string &path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    ::chmod(path.c_str(), 0700);
}

uint64_t fnv1a_64(const std::string &data) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hash_hex(const std::string &data) {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << fnv1a_64(data) << ":" << data.size();
    return oss.str();
}

long long calc_dir_size_mb(const std::string &path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return 0;
    uint64_t bytes = 0;
    size_t visited = 0;
    fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
    for (const auto &entry : it) {
        if (++visited > 12000) break;
        if (entry.is_regular_file(ec)) {
            uint64_t sz = entry.file_size(ec);
            if (!ec) bytes += sz;
        }
    }
    return static_cast<long long>((bytes + 524288ULL) / 1048576ULL);
}

int count_sandbox_processes(const std::string &sb_home) {
    int count = 0;
    std::string out;
    int rc = 1;
    if (!run_argv_capture({"pgrep", "-u", std::to_string(::getuid()), "-f", "spinel-sandbox"}, out, rc)) {
        return 0;
    }
    (void)sb_home;
    for (const auto &line : split_lines(out)) {
        if (!trim_copy(line).empty()) ++count;
    }
    return count;
}

bool start_sandbox_dbus_daemon(const std::string &rt_dir) {
    std::string bus_sock = rt_dir + "/bus";
    std::error_code ec;
    if (fs::exists(bus_sock, ec)) {
        int existing = read_pid_file(sandbox_dbus_pid_file());
        if (is_pid_alive(existing)) return true;
        fs::remove(bus_sock, ec);
    }
    if (!has_executable("dbus-daemon")) return false;

    std::string addr = "unix:path=" + bus_sock;
    std::string pid_str;
    int rc = 1;
    int pipefd[2]{-1, -1};
    if (::pipe(pipefd) != 0) return false;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::close(pipefd[1]);
        ::execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork", "--address", addr.c_str(), "--print-pid", nullptr);
        _exit(127);
    }
    ::close(pipefd[1]);
    if (pid > 0) {
        write_text_file(sandbox_dbus_pid_file(), std::to_string(pid) + "\n");
        // Wait briefly for bus socket creation (read --print-pid first line)
        char buf[64]{};
        ssize_t n = ::read(pipefd[0], buf, sizeof(buf) - 1);
        (void)n;
        ::close(pipefd[0]);
        for (int i = 0; i < 20; ++i) {
            if (fs::exists(bus_sock, ec)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        return fs::exists(bus_sock, ec);
    }
    ::close(pipefd[0]);
    (void)pid_str;
    (void)rc;
    return false;
}

void seed_sandbox_home_basics(const std::string &fake_home) {
    ensure_private_dir(fake_home);
    ensure_private_dir(fake_home + "/.config");
    ensure_private_dir(fake_home + "/.local/share");
    ensure_private_dir(fake_home + "/Desktop");
    ensure_private_dir(fake_home + "/Downloads");
    ensure_private_dir(fake_home + "/Workspace");

    // Copy GTK / font / IME basics silently if missing so Chinese fonts & dark/light CSS match
    std::string host = home_dir();
    std::error_code ec;
    for (const char *rel : {"/.config/gtk-4.0", "/.config/gtk-3.0", "/.config/fontconfig"}) {
        fs::path src = fs::path(host + rel);
        fs::path dst = fs::path(fake_home + rel);
        if (fs::exists(src, ec) && !fs::exists(dst, ec)) {
            fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
        }
    }
}

std::string resolve_sandbox_bin() {
    if (fs::exists("/usr/local/bin/spinel-sandbox")) return "/usr/local/bin/spinel-sandbox";
    if (fs::exists("/usr/bin/spinel-sandbox")) return "/usr/bin/spinel-sandbox";
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !exe.empty()) {
        fs::path sibling = exe.parent_path() / "spinel-sandbox";
        if (fs::exists(sibling, ec)) return sibling.string();
    }
    return "spinel-sandbox";
}

}  // namespace

std::string sandbox_config_path() { return config_dir_path() + "/sandbox.conf"; }
std::string sandbox_root_dir() { return home_dir() + "/.local/share/spinelos/sandboxes/default"; }
std::string sandbox_home_dir() { return sandbox_root_dir() + "/home"; }
std::string sandbox_labwc_dir() { return sandbox_root_dir() + "/labwc"; }
std::string sandbox_runtime_dir() { return host_runtime_dir() + "/spinel-sandbox"; }
std::string sandbox_socket_name() { return "wayland-0"; }

bool is_valid_sandbox_perf_mode(const std::string &mode) {
    return mode == "host-priority" || mode == "balanced";
}

bool is_valid_sandbox_resolution(const std::string &res, int &out_w, int &out_h) {
    auto x = res.find('x');
    if (x == std::string::npos) return false;
    try {
        int w = std::stoi(res.substr(0, x));
        int h = std::stoi(res.substr(x + 1));
        if (w < 800 || w > 3840 || h < 600 || h > 2160) return false;
        out_w = w;
        out_h = h;
        return true;
    } catch (...) {
        return false;
    }
}

bool is_valid_sandbox_mount_dir(const std::string &raw_path, std::string &normalized_out, std::string &error_out) {
    std::string p = trim_copy(raw_path);
    if (p.empty() || p.front() != '/') {
        error_out = "路径必须为绝对路径";
        return false;
    }
    if (p.find('\n') != std::string::npos || p.find('\r') != std::string::npos || p.find('\0') != std::string::npos) {
        error_out = "路径包含非法字符";
        return false;
    }
    while (p.size() > 1 && p.back() == '/') p.pop_back();

    std::error_code ec;
    fs::path fp(p);
    if (!fs::exists(fp, ec) || !fs::is_directory(fp, ec)) {
        error_out = "目标文件夹不存在";
        return false;
    }
    fs::path canon = fs::weakly_canonical(fp, ec);
    std::string norm = ec ? fp.lexically_normal().string() : canon.string();
    while (norm.size() > 1 && norm.back() == '/') norm.pop_back();

    std::string host = home_dir();
    while (host.size() > 1 && host.back() == '/') host.pop_back();

    // Forbid mounting root, system trees, host home directly, or the sandbox store itself
    static const char *const kForbiddenExact[] = {
        "/", "/home", "/root", "/etc", "/usr", "/bin", "/lib", "/lib64", "/var", "/dev", "/proc", "/sys", "/run", "/tmp"
    };
    for (const char *bad : kForbiddenExact) {
        if (norm == bad) {
            error_out = "出于安全隔离策略，不能把系统根目录或整个 /home 映射进沙箱";
            return false;
        }
    }
    if (norm == host) {
        error_out = "不能直接映射整个宿主用户主目录，请选择具体的工作子目录（如 ~/Projects）";
        return false;
    }
    if (norm == host + "/.ssh" || norm.rfind(host + "/.ssh/", 0) == 0 ||
        norm == host + "/.gnupg" || norm.rfind(host + "/.gnupg/", 0) == 0) {
        error_out = "出于安全考虑，禁止映射 ~/.ssh 或 ~/.gnupg 密钥目录";
        return false;
    }
    std::string sb_root = sandbox_root_dir();
    if (norm == sb_root || norm.rfind(sb_root + "/", 0) == 0) {
        error_out = "不能把沙箱内部存储目录再次映射到沙箱内";
        return false;
    }

    normalized_out = norm;
    error_out.clear();
    return true;
}

SandboxConfig load_sandbox_config() {
    SandboxConfig cfg;
    std::ifstream in(sandbox_config_path());
    auto push_unique = [](std::vector<std::string> &vec, const std::string &v) {
        if (v.empty()) return;
        if (std::find(vec.begin(), vec.end(), v) == vec.end()) vec.push_back(v);
    };
    auto sanitize_field = [](std::string s) {
        s = trim_copy(s);
        for (char &ch : s) {
            if (ch == '|' || ch == '\n' || ch == '\r') ch = ' ';
        }
        return s;
    };
    if (in) {
        std::string line;
        while (std::getline(in, line)) {
            line = trim_copy(line);
            if (line.empty() || line.front() == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim_copy(line.substr(0, eq));
            std::string v = trim_copy(line.substr(eq + 1));
            if (k == "allow_network") {
                cfg.allow_network = (v != "false" && v != "0" && v != "no");
            } else if (k == "sync_clipboard") {
                cfg.sync_clipboard = (v != "false" && v != "0" && v != "no");
            } else if (k == "perf_mode" && is_valid_sandbox_perf_mode(v)) {
                cfg.perf_mode = v;
            } else if (k == "resolution") {
                int w = 0, h = 0;
                if (is_valid_sandbox_resolution(v, w, h)) cfg.resolution = std::to_string(w) + "x" + std::to_string(h);
            } else if (k == "rw_dir") {
                std::string norm, err;
                if (is_valid_sandbox_mount_dir(v, norm, err)) {
                    push_unique(cfg.rw_dirs, norm);
                }
            } else if (k == "ro_dir") {
                std::string norm, err;
                if (is_valid_sandbox_mount_dir(v, norm, err)) {
                    push_unique(cfg.ro_dirs, norm);
                }
            } else if (k == "apps_configured") {
                cfg.apps_configured = (v == "true" || v == "1");
            } else if (k == "app") {
                size_t p1 = v.find('|');
                size_t p2 = (p1 == std::string::npos) ? std::string::npos : v.find('|', p1 + 1);
                if (p1 != std::string::npos && p2 != std::string::npos) {
                    SandboxAppEntry ent;
                    ent.auto_start = (v.substr(0, p1) == "1" || v.substr(0, p1) == "true");
                    ent.name = sanitize_field(v.substr(p1 + 1, p2 - p1 - 1));
                    ent.cmd = sanitize_field(v.substr(p2 + 1));
                    if (!ent.cmd.empty()) {
                        if (ent.name.empty()) ent.name = ent.cmd;
                        cfg.apps.push_back(std::move(ent));
                    }
                }
            }
        }
    }
    if (!cfg.apps_configured && cfg.apps.empty()) {
        struct DefaultApp {
            const char *name;
            const char *bin;
            const char *cmd;
        };
        static const DefaultApp kDefaults[] = {
            {"Spinel 终端", "spinel-terminal", "launch:terminal"},
            {"Spinel 文件管理器", "spinel-files", "launch:files"},
            {"Google Chrome", "google-chrome-stable", "launch:browser"},
            {"Chromium 浏览器", "chromium", "chromium"},
            {"Antigravity", "antigravity", "antigravity"},
            {"VS Code", "code", "code"},
        };
        for (const auto &d : kDefaults) {
            if (has_executable(d.bin)) {
                cfg.apps.push_back({d.name, d.cmd, false});
            }
        }
    }
    return cfg;
}

bool save_sandbox_config(const SandboxConfig &cfg) {
    ensure_private_dir(config_dir_path());
    auto clean = [](std::string s) {
        s = trim_copy(s);
        for (char &ch : s) {
            if (ch == '|' || ch == '\n' || ch == '\r') ch = ' ';
        }
        return s;
    };
    std::ostringstream oss;
    oss << "# SpinelOS Agent Parallel Sandbox Configuration\n";
    oss << "allow_network=" << (cfg.allow_network ? "true" : "false") << "\n";
    oss << "sync_clipboard=" << (cfg.sync_clipboard ? "true" : "false") << "\n";
    oss << "perf_mode=" << (is_valid_sandbox_perf_mode(cfg.perf_mode) ? cfg.perf_mode : "host-priority") << "\n";
    int w = 1600, h = 900;
    if (!is_valid_sandbox_resolution(cfg.resolution, w, h)) {
        w = 1600;
        h = 900;
    }
    oss << "resolution=" << w << "x" << h << "\n";
    oss << "apps_configured=" << (cfg.apps_configured ? "true" : "false") << "\n";
    for (const auto &d : cfg.rw_dirs) {
        std::string norm, err;
        if (is_valid_sandbox_mount_dir(d, norm, err)) {
            oss << "rw_dir=" << norm << "\n";
        }
    }
    for (const auto &d : cfg.ro_dirs) {
        std::string norm, err;
        if (is_valid_sandbox_mount_dir(d, norm, err)) {
            if (std::find(cfg.rw_dirs.begin(), cfg.rw_dirs.end(), norm) == cfg.rw_dirs.end()) {
                oss << "ro_dir=" << norm << "\n";
            }
        }
    }
    for (const auto &app : cfg.apps) {
        std::string cname = clean(app.name);
        std::string ccmd = clean(app.cmd);
        if (ccmd.empty()) continue;
        if (cname.empty()) cname = ccmd;
        oss << "app=" << (app.auto_start ? "1" : "0") << "|" << cname << "|" << ccmd << "\n";
    }
    return write_text_file(sandbox_config_path(), oss.str());
}

bool add_sandbox_app(const std::string &name, const std::string &cmd, bool auto_start, std::string &error_out) {
    std::string ccmd = trim_copy(cmd);
    std::string cname = trim_copy(name);
    if (ccmd.empty()) {
        error_out = "无效的应用命令";
        return false;
    }
    if (cname.empty()) cname = ccmd;
    SandboxConfig cfg = load_sandbox_config();
    cfg.apps_configured = true;
    for (auto &a : cfg.apps) {
        if (a.cmd == ccmd) {
            a.name = cname;
            a.auto_start = auto_start;
            if (!save_sandbox_config(cfg)) {
                error_out = "保存沙箱配置失败";
                return false;
            }
            return true;
        }
    }
    cfg.apps.push_back({cname, ccmd, auto_start});
    if (!save_sandbox_config(cfg)) {
        error_out = "保存沙箱配置失败";
        return false;
    }
    return true;
}

bool remove_sandbox_app(const std::string &cmd) {
    std::string ccmd = trim_copy(cmd);
    SandboxConfig cfg = load_sandbox_config();
    cfg.apps_configured = true;
    size_t before = cfg.apps.size();
    cfg.apps.erase(std::remove_if(cfg.apps.begin(), cfg.apps.end(),
                                  [&](const SandboxAppEntry &a) { return a.cmd == ccmd; }),
                   cfg.apps.end());
    if (cfg.apps.size() == before) return false;
    return save_sandbox_config(cfg);
}

bool set_sandbox_app_autostart(const std::string &cmd, bool auto_start) {
    std::string ccmd = trim_copy(cmd);
    SandboxConfig cfg = load_sandbox_config();
    cfg.apps_configured = true;
    bool found = false;
    for (auto &a : cfg.apps) {
        if (a.cmd == ccmd) {
            a.auto_start = auto_start;
            found = true;
            break;
        }
    }
    if (!found) return false;
    return save_sandbox_config(cfg);
}

bool add_sandbox_mount_dir(const std::string &path, bool writable, std::string &error_out) {
    std::string norm;
    if (!is_valid_sandbox_mount_dir(path, norm, error_out)) return false;
    SandboxConfig cfg = load_sandbox_config();
    cfg.rw_dirs.erase(std::remove(cfg.rw_dirs.begin(), cfg.rw_dirs.end(), norm), cfg.rw_dirs.end());
    cfg.ro_dirs.erase(std::remove(cfg.ro_dirs.begin(), cfg.ro_dirs.end(), norm), cfg.ro_dirs.end());
    if (writable) cfg.rw_dirs.push_back(norm);
    else cfg.ro_dirs.push_back(norm);
    if (!save_sandbox_config(cfg)) {
        error_out = "写入沙箱配置失败";
        return false;
    }
    return true;
}

bool remove_sandbox_mount_dir(const std::string &path) {
    std::string target = trim_copy(path);
    while (target.size() > 1 && target.back() == '/') target.pop_back();
    SandboxConfig cfg = load_sandbox_config();
    size_t before = cfg.rw_dirs.size() + cfg.ro_dirs.size();
    cfg.rw_dirs.erase(std::remove(cfg.rw_dirs.begin(), cfg.rw_dirs.end(), target), cfg.rw_dirs.end());
    cfg.ro_dirs.erase(std::remove(cfg.ro_dirs.begin(), cfg.ro_dirs.end(), target), cfg.ro_dirs.end());
    if (cfg.rw_dirs.size() + cfg.ro_dirs.size() == before) return false;
    return save_sandbox_config(cfg);
}

std::string build_sandbox_rc_xml(const SandboxConfig &cfg) {
    (void)cfg;
    return R"(<?xml version="1.0"?>
<labwc_config>
  <core>
    <decoration>server</decoration>
    <gap>8</gap>
  </core>
  <theme>
    <name>SpinelOS</name>
    <cornerRadius>10</cornerRadius>
  </theme>
  <focus>
    <followMouse>no</followMouse>
    <raiseOnFocus>yes</raiseOnFocus>
  </focus>
  <keyboard>
    <keybind key="A-F4"><action name="Close"/></keybind>
    <keybind key="A-Tab"><action name="NextWindow"/></keybind>
    <keybind key="W-Up"><action name="ToggleMaximize"/></keybind>
  </keyboard>
  <windowRules>
    <windowRule identifier="*"><action name="Maximize"/></windowRule>
  </windowRules>
</labwc_config>
)";
}

std::vector<std::string> build_sandbox_bwrap_argv(const SandboxConfig &cfg,
                                                  const std::string &runtime_dir,
                                                  const std::string &fake_home,
                                                  const std::string &host_home,
                                                  unsigned int uid,
                                                  const std::vector<std::string> &cmd_args,
                                                  const std::string &cwd) {
    std::vector<std::string> args;
    std::string run_user = "/run/user/" + std::to_string(uid);
    args.push_back("bwrap");
    args.push_back("--die-with-parent");
    args.push_back("--new-session");
    args.push_back("--unshare-pid");
    args.push_back("--unshare-ipc");
    args.push_back("--unshare-uts");
    args.push_back("--unshare-cgroup-try");
    if (!cfg.allow_network) {
        args.push_back("--unshare-net");
    }

    // Minimal clean root filesystem: host OS tools read-only, host home/drives hidden
    args.push_back("--ro-bind");
    args.push_back("/usr");
    args.push_back("/usr");
    args.push_back("--symlink");
    args.push_back("usr/bin");
    args.push_back("/bin");
    args.push_back("--symlink");
    args.push_back("usr/lib");
    args.push_back("/lib");
    if (fs::exists("/lib64")) {
        args.push_back("--symlink");
        args.push_back("usr/lib");
        args.push_back("/lib64");
    }
    args.push_back("--ro-bind");
    args.push_back("/etc");
    args.push_back("/etc");
    args.push_back("--ro-bind-try");
    args.push_back("/opt");
    args.push_back("/opt");
    args.push_back("--ro-bind-try");
    args.push_back("/sys");
    args.push_back("/sys");
    args.push_back("--ro-bind-try");
    args.push_back("/var/cache/fontconfig");
    args.push_back("/var/cache/fontconfig");
    args.push_back("--ro-bind-try");
    args.push_back("/var/lib/dbus");
    args.push_back("/var/lib/dbus");

    if (cfg.allow_network) {
        args.push_back("--ro-bind-try");
        args.push_back("/run/systemd/resolve");
        args.push_back("/run/systemd/resolve");
        args.push_back("--ro-bind-try");
        args.push_back("/run/NetworkManager");
        args.push_back("/run/NetworkManager");
    }

    args.push_back("--proc");
    args.push_back("/proc");
    args.push_back("--dev");
    args.push_back("/dev");
    args.push_back("--dev-bind-try");
    args.push_back("/dev/dri");
    args.push_back("/dev/dri");
    args.push_back("--tmpfs");
    args.push_back("/tmp");
    args.push_back("--tmpfs");
    args.push_back("/var/tmp");
    args.push_back("--tmpfs");
    args.push_back("/home");

    // Mount isolated fake home onto exact host_home path so apps run natively without custom paths
    args.push_back("--bind");
    args.push_back(fake_home);
    args.push_back(host_home);

    // Mount isolated sandbox Wayland/D-Bus runtime dir onto /run/user/<uid> (hides host wayland-0 & keyring)
    args.push_back("--bind");
    args.push_back(runtime_dir);
    args.push_back(run_user);

    // Explicitly mount approved persistent directories over their exact host paths
    std::error_code ec;
    for (const auto &d : cfg.ro_dirs) {
        if (fs::is_directory(d, ec)) {
            args.push_back("--ro-bind");
            args.push_back(d);
            args.push_back(d);
        }
    }
    for (const auto &d : cfg.rw_dirs) {
        if (fs::is_directory(d, ec)) {
            args.push_back("--bind");
            args.push_back(d);
            args.push_back(d);
        }
    }

    // Set isolated display/environment variables inside the container
    args.push_back("--setenv");
    args.push_back("HOME");
    args.push_back(host_home);
    args.push_back("--setenv");
    args.push_back("XDG_RUNTIME_DIR");
    args.push_back(run_user);
    args.push_back("--setenv");
    args.push_back("WAYLAND_DISPLAY");
    args.push_back(sandbox_socket_name());
    args.push_back("--setenv");
    args.push_back("DBUS_SESSION_BUS_ADDRESS");
    args.push_back("unix:path=" + run_user + "/bus");
    args.push_back("--setenv");
    args.push_back("XDG_CURRENT_DESKTOP");
    args.push_back("SpinelOS:GNOME");
    args.push_back("--setenv");
    args.push_back("QT_QPA_PLATFORM");
    args.push_back("wayland");
    args.push_back("--setenv");
    args.push_back("GDK_BACKEND");
    args.push_back("wayland");
    args.push_back("--setenv");
    args.push_back("MOZ_ENABLE_WAYLAND");
    args.push_back("1");
    args.push_back("--setenv");
    args.push_back("ELECTRON_OZONE_PLATFORM_HINT");
    args.push_back("wayland");
    args.push_back("--unsetenv");
    args.push_back("DISPLAY");
    args.push_back("--unsetenv");
    args.push_back("SSH_AUTH_SOCK");

    if (!cwd.empty()) {
        args.push_back("--chdir");
        args.push_back(cwd);
    } else {
        args.push_back("--chdir");
        args.push_back(host_home);
    }

    args.push_back("--");
    for (const auto &a : cmd_args) args.push_back(a);
    return args;
}

SandboxStatus query_sandbox_status() {
    SandboxStatus st;
    st.bwrap_installed = has_executable("bwrap");
    st.labwc_installed = has_executable("labwc");
    st.runtime_dir = sandbox_runtime_dir();
    st.home_path = sandbox_home_dir();
    st.home_size_mb = calc_dir_size_mb(st.home_path);

    int pid = read_pid_file(sandbox_pid_file());
    std::error_code ec;
    bool sock = fs::exists(st.runtime_dir + "/" + sandbox_socket_name(), ec);
    st.socket_ready = sock;
    st.running = is_pid_alive(pid) && sock;
    st.compositor_pid = st.running ? pid : -1;

    int cpid = read_pid_file(sandbox_clip_pid_file());
    st.clip_sync_running = is_pid_alive(cpid);
    if (st.running) {
        st.app_count = std::max(0, count_sandbox_processes(st.home_path) - 1);
    }
    return st;
}

std::string sandbox_state_json() {
    SandboxConfig cfg = load_sandbox_config();
    SandboxStatus st = query_sandbox_status();
    std::ostringstream oss;
    oss << "{";
    oss << "\"bwrap_installed\":" << (st.bwrap_installed ? "true" : "false") << ",";
    oss << "\"labwc_installed\":" << (st.labwc_installed ? "true" : "false") << ",";
    oss << "\"running\":" << (st.running ? "true" : "false") << ",";
    oss << "\"compositor_pid\":" << st.compositor_pid << ",";
    oss << "\"socket_ready\":" << (st.socket_ready ? "true" : "false") << ",";
    oss << "\"clip_sync_running\":" << (st.clip_sync_running ? "true" : "false") << ",";
    oss << "\"home_size_mb\":" << st.home_size_mb << ",";
    oss << "\"home_path\":\"" << json_escape(st.home_path) << "\",";
    oss << "\"runtime_dir\":\"" << json_escape(st.runtime_dir) << "\",";
    oss << "\"allow_network\":" << (cfg.allow_network ? "true" : "false") << ",";
    oss << "\"sync_clipboard\":" << (cfg.sync_clipboard ? "true" : "false") << ",";
    oss << "\"perf_mode\":\"" << json_escape(cfg.perf_mode) << "\",";
    oss << "\"resolution\":\"" << json_escape(cfg.resolution) << "\",";
    oss << "\"rw_dirs\":[";
    for (size_t i = 0; i < cfg.rw_dirs.size(); ++i) {
        if (i) oss << ",";
        oss << "\"" << json_escape(cfg.rw_dirs[i]) << "\"";
    }
    oss << "],\"ro_dirs\":[";
    for (size_t i = 0; i < cfg.ro_dirs.size(); ++i) {
        if (i) oss << ",";
        oss << "\"" << json_escape(cfg.ro_dirs[i]) << "\"";
    }
    oss << "],\"apps\":[";
    for (size_t i = 0; i < cfg.apps.size(); ++i) {
        if (i) oss << ",";
        oss << "{"
            << "\"name\":\"" << json_escape(cfg.apps[i].name) << "\","
            << "\"cmd\":\"" << json_escape(cfg.apps[i].cmd) << "\","
            << "\"auto_start\":" << (cfg.apps[i].auto_start ? "true" : "false")
            << "}";
    }
    oss << "]}";
    return oss.str();
}

void stop_sandbox_clipboard_sync() {
    int pid = read_pid_file(sandbox_clip_pid_file());
    if (is_pid_alive(pid)) {
        ::kill(-pid, SIGTERM);
        ::kill(pid, SIGTERM);
    }
    std::error_code ec;
    fs::remove(sandbox_clip_pid_file(), ec);
}

void ensure_sandbox_clipboard_sync(bool enabled) {
    if (!enabled) {
        stop_sandbox_clipboard_sync();
        return;
    }
    SandboxStatus st = query_sandbox_status();
    if (!st.running || !has_executable("wl-paste") || !has_executable("wl-copy")) return;
    if (st.clip_sync_running) return;

    std::string bin = resolve_sandbox_bin();
    pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();
        ::execl(bin.c_str(), bin.c_str(), "--clip-sync", nullptr);
        ::execlp("spinel-sandbox", "spinel-sandbox", "--clip-sync", nullptr);
        _exit(127);
    }
    if (pid > 0) {
        write_text_file(sandbox_clip_pid_file(), std::to_string(pid) + "\n");
    }
}

bool start_sandbox_session(std::string &error_out) {
    if (!has_executable("bwrap")) {
        error_out = "未安装 bubblewrap (bwrap)，请执行 ./scripts/dev-deploy.sh --full 安装。";
        return false;
    }
    if (!has_executable("labwc")) {
        error_out = "未安装 labwc 合成器，无法启动平行桌面。";
        return false;
    }

    SandboxStatus cur = query_sandbox_status();
    SandboxConfig cfg = load_sandbox_config();
    if (cur.running) {
        ensure_sandbox_clipboard_sync(cfg.sync_clipboard);
        return true;
    }

    std::string rt_dir = sandbox_runtime_dir();
    std::string fake_home = sandbox_home_dir();
    std::string cfg_dir = sandbox_labwc_dir();
    ensure_private_dir(rt_dir);
    seed_sandbox_home_basics(fake_home);
    ensure_private_dir(cfg_dir);

    // Clear stale sockets/lockfiles left over after crashes
    std::error_code ec;
    for (const char *stale : {"wayland-0", "wayland-0.lock", "wayland-1", "wayland-1.lock"}) {
        fs::remove(rt_dir + "/" + stale, ec);
    }
    write_text_file(cfg_dir + "/rc.xml", build_sandbox_rc_xml(cfg));

    int res_w = 1600, res_h = 900;
    is_valid_sandbox_resolution(cfg.resolution, res_w, res_h);

    start_sandbox_dbus_daemon(rt_dir);

    const char *host_renderer = std::getenv("WLR_RENDERER");
    bool host_pixman = (host_renderer && std::string(host_renderer) == "pixman");
    bool has_systemd = has_executable("systemd-run");
    pid_t pid = -1;
    bool ready = false;

    if (has_systemd) {
        ready = spawn_headless_compositor_attempt(rt_dir, cfg_dir, cfg, true, host_pixman, pid);
        if (!ready && !host_pixman) {
            ready = spawn_headless_compositor_attempt(rt_dir, cfg_dir, cfg, true, true, pid);
        }
    }
    if (!ready) {
        ready = spawn_headless_compositor_attempt(rt_dir, cfg_dir, cfg, false, host_pixman, pid);
        if (!ready && !host_pixman) {
            ready = spawn_headless_compositor_attempt(rt_dir, cfg_dir, cfg, false, true, pid);
        }
    }
    if (!ready) {
        std::string err_line = trim_copy(read_first_line(rt_dir + "/labwc.log"));
        error_out = err_line.empty() ? "平行桌面 headless 合成器启动超时" : ("平行桌面启动失败: " + err_line);
        return false;
    }

    // Apply requested headless resolution via wlr-randr if available inside that socket
    if (has_executable("wlr-randr")) {
        std::string out;
        int rc = 1;
        pid_t wp = ::fork();
        if (wp == 0) {
            ::setenv("XDG_RUNTIME_DIR", rt_dir.c_str(), 1);
            ::setenv("WAYLAND_DISPLAY", sandbox_socket_name().c_str(), 1);
            std::string mode = std::to_string(res_w) + "x" + std::to_string(res_h);
            ::execlp("wlr-randr", "wlr-randr", "--output", "HEADLESS-1", "--custom-mode", mode.c_str(), nullptr);
            _exit(127);
        }
        if (wp > 0) {
            int st = 0;
            ::waitpid(wp, &st, 0);
        }
        (void)out;
        (void)rc;
    }

    ensure_sandbox_clipboard_sync(cfg.sync_clipboard);
    append_app_log("Started SpinelOS parallel sandbox compositor");
    return true;
}

bool stop_sandbox_session(std::string &error_out) {
    (void)error_out;
    stop_sandbox_clipboard_sync();

    int pid = read_pid_file(sandbox_pid_file());
    if (is_pid_alive(pid)) {
        ::kill(-pid, SIGTERM);
        ::kill(pid, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            if (!is_pid_alive(pid)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (is_pid_alive(pid)) {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
        }
    }

    int dpid = read_pid_file(sandbox_dbus_pid_file());
    if (is_pid_alive(dpid)) {
        ::kill(dpid, SIGTERM);
    }

    std::error_code ec;
    fs::remove(sandbox_pid_file(), ec);
    fs::remove(sandbox_dbus_pid_file(), ec);
    fs::remove(sandbox_runtime_dir() + "/" + sandbox_socket_name(), ec);
    fs::remove(sandbox_runtime_dir() + "/" + sandbox_socket_name() + ".lock", ec);
    fs::remove(sandbox_runtime_dir() + "/bus", ec);
    fs::remove(sandbox_clip_guard_file(), ec);
    return true;
}

bool reset_sandbox_home(std::string &error_out) {
    stop_sandbox_session(error_out);
    std::error_code ec;
    fs::remove_all(sandbox_home_dir(), ec);
    if (ec) {
        error_out = "清理沙箱临时主目录失败: " + ec.message();
        return false;
    }
    seed_sandbox_home_basics(sandbox_home_dir());
    append_app_log("Reset SpinelOS parallel sandbox temporary home");
    return true;
}

bool launch_in_sandbox(const std::vector<std::string> &cmd_args,
                       const std::string &workspace_dir,
                       std::string &error_out) {
    if (cmd_args.empty()) {
        error_out = "未指定要启动的命令";
        return false;
    }

    std::string cwd;
    if (!workspace_dir.empty()) {
        std::string norm;
        if (!is_valid_sandbox_mount_dir(workspace_dir, norm, error_out)) return false;
        SandboxConfig cur = load_sandbox_config();
        bool in_rw = std::find(cur.rw_dirs.begin(), cur.rw_dirs.end(), norm) != cur.rw_dirs.end();
        bool in_ro = std::find(cur.ro_dirs.begin(), cur.ro_dirs.end(), norm) != cur.ro_dirs.end();
        if (!in_rw && !in_ro) {
            if (!add_sandbox_mount_dir(norm, true, error_out)) return false;
        }
        cwd = norm;
    }

    if (!start_sandbox_session(error_out)) return false;

    SandboxConfig cfg = load_sandbox_config();
    std::string rt_dir = sandbox_runtime_dir();
    std::string fake_home = sandbox_home_dir();
    std::string host = home_dir();
    unsigned int uid = static_cast<unsigned int>(::getuid());
    std::vector<std::string> bwrap_args = build_sandbox_bwrap_argv(cfg, rt_dir, fake_home, host, uid, cmd_args, cwd);

    if (has_executable("systemd-run")) {
        bool host_prio = (cfg.perf_mode != "balanced");
        std::vector<std::string> wrapped = {
            "systemd-run", "--user", "--scope",
            "--slice=spinel-sandbox.slice",
            std::string("-pCPUWeight=") + (host_prio ? "20" : "50"),
            std::string("-pIOWeight=") + (host_prio ? "20" : "50"),
            "--"
        };
        wrapped.insert(wrapped.end(), bwrap_args.begin(), bwrap_args.end());
        bwrap_args.swap(wrapped);
    }

    spawn_argv_detached(bwrap_args);
    return true;
}

int handle_sandbox_clipboard_push(const std::string &side) {
    if (side != "host" && side != "sandbox") return 1;
    SandboxConfig cfg = load_sandbox_config();
    if (!cfg.sync_clipboard) return 0;

    std::ostringstream oss;
    oss << std::cin.rdbuf();
    std::string text = oss.str();
    if (text.empty()) return 0;
    if (text.size() > 1024 * 1024) return 0;  // 1MB safety cap

    std::string digest = hash_hex(text);
    std::string guard_path = sandbox_clip_guard_file();
    int lock_fd = ::open((guard_path + ".lock").c_str(), O_CREAT | O_RDWR, 0600);
    if (lock_fd >= 0) ::flock(lock_fd, LOCK_EX);

    std::string prev = trim_copy(read_first_line(guard_path));
    if (prev == digest) {
        if (lock_fd >= 0) {
            ::flock(lock_fd, LOCK_UN);
            ::close(lock_fd);
        }
        return 0;
    }
    write_text_file(guard_path, digest + "\n");
    if (lock_fd >= 0) {
        ::flock(lock_fd, LOCK_UN);
        ::close(lock_fd);
    }

    // side == "host" means host clipboard changed -> push to sandbox
    // side == "sandbox" means sandbox clipboard changed -> push to host
    const char *host_wd = std::getenv("WAYLAND_DISPLAY");
    std::string target_rt = (side == "host") ? sandbox_runtime_dir() : host_runtime_dir();
    std::string target_wd = (side == "host") ? sandbox_socket_name() : ((host_wd && *host_wd) ? host_wd : "wayland-1");

    int pipefd[2]{-1, -1};
    if (::pipe(pipefd) != 0) return 1;
    pid_t pid = ::fork();
    if (pid == 0) {
        ::close(pipefd[1]);
        ::dup2(pipefd[0], STDIN_FILENO);
        ::close(pipefd[0]);
        ::setenv("XDG_RUNTIME_DIR", target_rt.c_str(), 1);
        ::setenv("WAYLAND_DISPLAY", target_wd.c_str(), 1);
        ::execlp("wl-copy", "wl-copy", "--type", "text/plain;charset=utf-8", nullptr);
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
        int st = 0;
        ::waitpid(pid, &st, 0);
        return 0;
    }
    ::close(pipefd[1]);
    return 1;
}

int run_sandbox_clipboard_daemon() {
    ensure_private_dir(sandbox_runtime_dir());
    std::string bin = resolve_sandbox_bin();
    const char *host_wd_env = std::getenv("WAYLAND_DISPLAY");
    std::string host_wd = (host_wd_env && *host_wd_env) ? host_wd_env : "wayland-1";
    std::string host_rt = host_runtime_dir();
    std::string sb_rt = sandbox_runtime_dir();
    std::string sb_wd = sandbox_socket_name();

    auto spawn_watcher = [&](const std::string &rt, const std::string &wd, const char *side) -> pid_t {
        pid_t p = ::fork();
        if (p == 0) {
            ::setenv("XDG_RUNTIME_DIR", rt.c_str(), 1);
            ::setenv("WAYLAND_DISPLAY", wd.c_str(), 1);
            ::setenv("SPINEL_HOST_RUNTIME_DIR", host_rt.c_str(), 1);
            ::setenv("SPINEL_HOST_WAYLAND_DISPLAY", host_wd.c_str(), 1);
            ::execlp("wl-paste", "wl-paste", "--no-newline", "--type", "text",
                     "--watch", bin.c_str(), "--clip-push", side, nullptr);
            _exit(127);
        }
        return p;
    };

    pid_t h2s = spawn_watcher(host_rt, host_wd, "host");
    pid_t s2h = spawn_watcher(sb_rt, sb_wd, "sandbox");

    signal(SIGTERM, +[](int) { _exit(0); });
    signal(SIGINT, +[](int) { _exit(0); });

    while (true) {
        int st = 0;
        pid_t done = ::wait(&st);
        if (done < 0 && errno == EINTR) continue;
        SandboxConfig cfg = load_sandbox_config();
        SandboxStatus sb = query_sandbox_status();
        if (!cfg.sync_clipboard || !sb.running) break;
        if (done == h2s) h2s = spawn_watcher(host_rt, host_wd, "host");
        else if (done == s2h) s2h = spawn_watcher(sb_rt, sb_wd, "sandbox");
        else std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    if (h2s > 1) ::kill(h2s, SIGTERM);
    if (s2h > 1) ::kill(s2h, SIGTERM);
    return 0;
}
