#include "common.hpp"

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

std::string json_escape(const std::string &input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += ' ';
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string trim_copy(const std::string &s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string home_dir() {
    const char *h = std::getenv("HOME");
    if (h && *h) return std::string(h);
    struct passwd pwd {};
    struct passwd *res = nullptr;
    std::array<char, 1024> buf{};
    if (getpwuid_r(getuid(), &pwd, buf.data(), buf.size(), &res) == 0 && res && res->pw_dir && *res->pw_dir) {
        return std::string(res->pw_dir);
    }
    return "/tmp";
}

void ensure_display_env() {
    const char *xdg_cur = std::getenv("XDG_CURRENT_DESKTOP");
    if (!xdg_cur || !*xdg_cur || std::string(xdg_cur).find("GNOME") == std::string::npos) {
        setenv("XDG_CURRENT_DESKTOP", "SpinelOS:GNOME", 1);
    }

    const char *xdg_runtime = std::getenv("XDG_RUNTIME_DIR");
    std::string runtime_dir = (xdg_runtime && *xdg_runtime)
        ? std::string(xdg_runtime)
        : ("/run/user/" + std::to_string(static_cast<unsigned long>(::getuid())));
    if (!xdg_runtime || !*xdg_runtime) {
        struct stat rt_st{};
        if (::stat(runtime_dir.c_str(), &rt_st) == 0 && S_ISDIR(rt_st.st_mode)) {
            setenv("XDG_RUNTIME_DIR", runtime_dir.c_str(), 1);
        }
    }

    const char *dbus_addr = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    if (!dbus_addr || !*dbus_addr) {
        std::string bus_path = runtime_dir + "/bus";
        struct stat bus_st{};
        if (::stat(bus_path.c_str(), &bus_st) == 0 && S_ISSOCK(bus_st.st_mode)) {
            std::string addr = "unix:path=" + bus_path;
            setenv("DBUS_SESSION_BUS_ADDRESS", addr.c_str(), 1);
        }
    }

    const char *disp = std::getenv("DISPLAY");
    if (disp && *disp) return;

    std::error_code ec;
    int best = -1;
    for (const auto &entry : fs::directory_iterator("/tmp/.X11-unix", ec)) {
        std::string name = entry.path().filename().string();
        if (name.size() < 2 || name[0] != 'X') continue;
        bool digits = true;
        for (size_t i = 1; i < name.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
                digits = false;
                break;
            }
        }
        if (!digits) continue;
        struct stat st{};
        if (lstat(entry.path().c_str(), &st) != 0 || !S_ISSOCK(st.st_mode)) continue;
        int num = std::stoi(name.substr(1));
        if (best < 0 || num < best) best = num;
    }
    if (best >= 0) {
        std::string val = ":" + std::to_string(best);
        setenv("DISPLAY", val.c_str(), 1);
    }
}

std::string spinel_state_dir() {
    fs::path dir = fs::path(home_dir()) / ".local/state/spinelos";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.string();
}

std::string read_first_line(const std::string &path, const std::string &fallback) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return fallback;
    std::string line;
    std::getline(ifs, line);
    return line;
}

bool write_text_file(const std::string &path, const std::string &content) {
    std::error_code ec;
    fs::path p(path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
    }
    fs::path tmp = p.string() + ".tmp." + std::to_string(getpid());
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        ofs.flush();
        if (!ofs.good()) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        ofs.flush();
        return ofs.good();
    }
    return true;
}

std::string run_cmd_capture(const char *cmd) {
    std::array<char, 512> buffer{};
    std::string result;
    FILE *pipe = popen(cmd, "r");
    if (!pipe) return "";
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }
    pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

bool run_argv_stdin_capture(const std::vector<std::string> &args,
                            const std::string &stdin_data,
                            std::string &out,
                            int &exit_code) {
    out.clear();
    exit_code = -1;
    if (args.empty()) return false;

    int in_pipe[2]{-1, -1};
    int out_pipe[2]{-1, -1};
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        if (in_pipe[0] >= 0) { close(in_pipe[0]); close(in_pipe[1]); }
        if (out_pipe[0] >= 0) { close(out_pipe[0]); close(out_pipe[1]); }
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return false;
    }

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);

        execvp(args[0].c_str(), argv.data());
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    if (!stdin_data.empty()) {
        std::string payload = stdin_data + "\n";
        size_t written = 0;
        while (written < payload.size()) {
            ssize_t n = write(in_pipe[1], payload.data() + written, payload.size() - written);
            if (n <= 0) break;
            written += static_cast<size_t>(n);
        }
    }
    close(in_pipe[1]);

    std::array<char, 512> buf{};
    ssize_t n = 0;
    while ((n = read(out_pipe[0], buf.data(), buf.size())) > 0) {
        out.append(buf.data(), static_cast<size_t>(n));
    }
    close(out_pipe[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return true;
}

bool run_argv_capture(const std::vector<std::string> &args, std::string &out, int &exit_code) {
    return run_argv_stdin_capture(args, "", out, exit_code);
}

bool restore_trashed_path(const fs::path &source, fs::path &target, std::string &error) {
    const fs::path parent = target.parent_path();
    std::error_code ec;
    const bool is_dir = fs::is_directory(fs::symlink_status(source, ec));
    if (ec) {
        error = ec.message();
        return false;
    }
    fs::create_directories(parent, ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    const std::string stem = is_dir ? target.filename().string() : target.stem().string();
    const std::string extension = is_dir ? "" : target.extension().string();
    fs::path candidate = target;
    for (int attempt = 1; attempt < 100; ++attempt) {
        if (attempt > 1) {
            candidate = parent / (stem + " (恢复 " + std::to_string(attempt) + ")" + extension);
        }
        // Dangling symlinks also occupy a destination name.
        if (fs::symlink_status(candidate, ec).type() != fs::file_type::not_found) {
            if (!ec) continue;
            error = ec.message();
            return false;
        }

        // Refuse a destination created after the check, including on another filesystem.
        int rc = 1;
        if (!run_argv_capture({"mv", "--update=none-fail", "--no-target-directory", "--",
                               source.string(), candidate.string()}, error, rc) || rc != 0) {
            if (error.empty()) error = "无法移动回收站项目。";
            return false;
        }
        target = std::move(candidate);
        return true;
    }
    error = "目标位置已有同名项目，且备用名称均已被占用。";
    return false;
}

bool has_executable(const std::string &cmd) {
    if (cmd.empty()) return false;
    if (cmd.find('/') != std::string::npos) {
        return access(cmd.c_str(), X_OK) == 0;
    }
    const char *path_env = std::getenv("PATH");
    if (!path_env) return false;
    std::stringstream ss(path_env);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        fs::path cand = fs::path(dir) / cmd;
        if (access(cand.c_str(), X_OK) == 0) return true;
    }
    return false;
}

void append_app_log(const std::string &line) {
    fs::path log_path = fs::path(spinel_state_dir()) / "apps.log";
    std::error_code ec;
    auto sz = fs::file_size(log_path, ec);
    if (!ec && sz > 1024 * 1024) {
        fs::resize_file(log_path, 0, ec);
    }

    std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    std::ofstream ofs(log_path, std::ios::app);
    if (ofs.is_open()) {
        ofs << "[" << ts << "] " << line << "\n";
    }
}

void spawn_detached(const std::string &cmd) {
    ensure_display_env();
    std::string wrapped = "nohup " + cmd + " >/dev/null 2>&1 &";
    int rc = std::system(wrapped.c_str());
    (void)rc;
}

void spawn_argv_detached(const std::vector<std::string> &args,
                         const std::vector<std::pair<std::string, std::string>> &env_overrides) {
    if (args.empty()) return;
    ensure_display_env();
    pid_t pid = fork();
    if (pid != 0) {
        if (pid > 0) {
            // Double-fork avoids zombies without blocking caller
            waitpid(pid, nullptr, 0);
        }
        return;
    }

    pid_t grand = fork();
    if (grand != 0) {
        _exit(0);
    }

    setsid();
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > 2) close(devnull);
    }

    for (const auto &kv : env_overrides) {
        setenv(kv.first.c_str(), kv.second.c_str(), 1);
    }

    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);

    execvp(args[0].c_str(), argv.data());
    _exit(127);
}

void spawn_argv_logged(const std::vector<std::string> &args,
                       const std::string &tag,
                       const std::vector<std::pair<std::string, std::string>> &env_overrides,
                       const std::string &working_dir) {
    if (args.empty()) return;
    ensure_display_env();

    std::ostringstream cmdline;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) cmdline << ' ';
        cmdline << args[i];
    }
    append_app_log("[" + tag + "] $ " + cmdline.str());

    fs::path log_path = fs::path(spinel_state_dir()) / "apps.log";

    pid_t pid = fork();
    if (pid != 0) {
        if (pid > 0) {
            waitpid(pid, nullptr, 0);
        }
        return;
    }

    pid_t grand = fork();
    if (grand != 0) {
        _exit(0);
    }

    setsid();
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        if (devnull > 2) close(devnull);
    }

    int logfd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logfd >= 0) {
        dup2(logfd, STDOUT_FILENO);
        dup2(logfd, STDERR_FILENO);
        if (logfd > 2) close(logfd);
    }

    for (const auto &kv : env_overrides) {
        setenv(kv.first.c_str(), kv.second.c_str(), 1);
    }

    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);

    if (!working_dir.empty() && chdir(working_dir.c_str()) != 0) {
        dprintf(STDERR_FILENO, "[%s] chdir(%s) failed\n", tag.c_str(), working_dir.c_str());
        _exit(126);
    }
    execvp(args[0].c_str(), argv.data());
    dprintf(STDERR_FILENO, "[%s] execvp(%s) failed\n", tag.c_str(), args[0].c_str());
    _exit(127);
}

bool is_live_iso_env() {
    return fs::exists("/run/archiso/bootmnt") || fs::exists("/run/archiso/airootfs");
}

std::vector<std::string> split_lines(const std::string &text) {
    std::vector<std::string> lines;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

namespace {

bool is_valid_proxy_url_common(const std::string &s) {
    if (s.empty()) return true;
    if (s.size() > 256) return false;
    bool ok_scheme = s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0 ||
                     s.rfind("socks5://", 0) == 0 || s.rfind("socks5h://", 0) == 0 ||
                     s.rfind("socks4://", 0) == 0;
    if (!ok_scheme) return false;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c == '"' || c == '\'' || c == '\\' || c == ';') return false;
    }
    return true;
}

std::string sanitize_no_proxy_common(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || ch == '.' || ch == ',' || ch == ':' || ch == '/' ||
            ch == '-' || ch == '_' || ch == '*') {
            out += ch;
        }
        if (out.size() >= 256) break;
    }
    return out;
}

std::string gsettings_proxy_get_common(const char *schema, const char *key) {
    std::string out;
    int rc = 1;
    if (!run_argv_capture({"gsettings", "get", schema, key}, out, rc) || rc != 0) return "";
    out = trim_copy(out);
    if (out.size() >= 2 && out.front() == '\'' && out.back() == '\'') {
        return out.substr(1, out.size() - 2);
    }
    return out;
}

}  // namespace

bool resolve_active_proxy_fields(std::string &out_http, std::string &out_socks, std::string &out_no_proxy) {
    out_http.clear();
    out_socks.clear();
    out_no_proxy = "localhost,127.0.0.1,::1,192.168.0.0/16,10.0.0.0/8";

    bool enabled = false;
    {
        std::ifstream ifs(fs::path(home_dir()) / ".config/spinelos/proxy.conf");
        std::string line;
        while (std::getline(ifs, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim_copy(line.substr(0, eq));
            std::string v = trim_copy(line.substr(eq + 1));
            if (k == "enabled") enabled = (v == "true" || v == "1");
            else if (k == "http_proxy" && is_valid_proxy_url_common(v)) out_http = v;
            else if (k == "socks_proxy" && is_valid_proxy_url_common(v)) out_socks = v;
            else if (k == "no_proxy") {
                std::string clean = sanitize_no_proxy_common(v);
                if (!clean.empty()) out_no_proxy = clean;
            }
        }
    }

    if (enabled && (!out_http.empty() || !out_socks.empty())) {
        return true;
    }

    out_http.clear();
    out_socks.clear();
    if (!has_executable("gsettings")) return false;
    if (gsettings_proxy_get_common("org.gnome.system.proxy", "mode") != "manual") return false;

    auto read_hp = [&](const char *subschema, const char *scheme) -> std::string {
        std::string host = gsettings_proxy_get_common(subschema, "host");
        std::string port = gsettings_proxy_get_common(subschema, "port");
        int pnum = std::atoi(port.c_str());
        if (host.empty() || pnum <= 0 || pnum > 65535) return "";
        std::string url = std::string(scheme) + "://" + host + ":" + std::to_string(pnum);
        return is_valid_proxy_url_common(url) ? url : "";
    };

    std::string http_u = read_hp("org.gnome.system.proxy.http", "http");
    std::string https_u = read_hp("org.gnome.system.proxy.https", "http");
    std::string socks_u = read_hp("org.gnome.system.proxy.socks", "socks5");

    out_http = !http_u.empty() ? http_u : https_u;
    out_socks = socks_u;
    return !out_http.empty() || !out_socks.empty();
}

std::vector<std::pair<std::string, std::string>> collect_active_proxy_envs() {
    std::string http_p, socks_p, no_p;
    if (!resolve_active_proxy_fields(http_p, socks_p, no_p)) return {};
    std::string http_val = !http_p.empty() ? http_p : socks_p;
    std::string all_val = !socks_p.empty() ? socks_p : http_p;
    return {
        {"http_proxy", http_val},
        {"https_proxy", http_val},
        {"HTTP_PROXY", http_val},
        {"HTTPS_PROXY", http_val},
        {"all_proxy", all_val},
        {"ALL_PROXY", all_val},
        {"no_proxy", no_p},
        {"NO_PROXY", no_p}
    };
}
