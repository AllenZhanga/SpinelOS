#include "common.hpp"

#include <fcntl.h>
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
    return (h && *h) ? std::string(h) : "/home/gamer";
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

bool run_argv_capture(const std::vector<std::string> &args, std::string &out, int &exit_code) {
    out.clear();
    exit_code = -1;
    if (args.empty()) return false;

    int pipefd[2]{-1, -1};
    if (pipe(pipefd) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);

        execvp(args[0].c_str(), argv.data());
        _exit(127);
    }

    close(pipefd[1]);
    std::array<char, 512> buf{};
    ssize_t n = 0;
    while ((n = read(pipefd[0], buf.data(), buf.size())) > 0) {
        out.append(buf.data(), static_cast<size_t>(n));
    }
    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return true;
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
    std::string wrapped = "nohup " + cmd + " >/dev/null 2>&1 &";
    int rc = std::system(wrapped.c_str());
    (void)rc;
}

void spawn_argv_detached(const std::vector<std::string> &args,
                         const std::vector<std::pair<std::string, std::string>> &env_overrides) {
    if (args.empty()) return;
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
                       const std::vector<std::pair<std::string, std::string>> &env_overrides) {
    if (args.empty()) return;

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
