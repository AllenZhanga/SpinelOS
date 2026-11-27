#include "app_store.hpp"
#include "common.hpp"
#include "desktop_apps.hpp"
#include "steam_library.hpp"
#include "system_control.hpp"

#include <pwd.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct StoreAppEntry {
    const char *id;
    const char *name;
    const char *category;
    const char *backend;  // "pacman" | "native" | "link" | "flatpak"
    const char *pkg;
    const char *summary;
    const char *launch_cmd;
    const char *badge;
    const char *service_unit;   // "" or "ollama" | "vllm" | "docker" | "cups"
    bool webapp_capable;
};

constexpr StoreAppEntry kCatalog[] = {
    // 1. 办公文档
    {"libreoffice", "LibreOffice 办公套件", "办公文档", "native", "libreoffice-fresh",
     "兼容 Word / Excel / PPT 的开源办公全家桶（自动安装中文语言包）", "libreoffice", "原生安装", "", false},
    {"papers", "Papers PDF 阅读器", "办公文档", "pacman", "papers",
     "GNOME 原生 GTK4 高速 PDF 与多格式文档阅读器", "papers", "Pacman 原生", "", false},
    {"xournalpp", "Xournal++ PDF 批注", "办公文档", "pacman", "xournalpp",
     "PDF 文档高亮签批、手写标注与课堂/会议笔记白板工具", "xournalpp", "Pacman 原生", "", false},
    {"printing", "打印服务 (CUPS)", "办公文档", "native", "cups",
     "CUPS 物理/网络与 PDF 虚拟打印底座 + 打印机管理面板（支持启停）", "system-config-printer", "系统服务", "cups", false},
    {"obsidian", "Obsidian 笔记", "办公文档", "pacman", "obsidian",
     "本地优先的 Markdown 双链知识库与结构化笔记工具", "obsidian", "Pacman 原生", "", false},
    {"thunderbird", "Thunderbird 邮件与日程", "办公文档", "pacman", "thunderbird",
     "Mozilla 开源桌面邮箱、日历与联系人多账户客户端", "thunderbird", "Pacman 原生", "", false},

    // 2. 影音图像
    {"mpv", "MPV 播放器", "影音图像", "pacman", "mpv",
     "极速轻量硬件硬解播放器，4K/HDR 高帧率影片播放首选", "mpv", "Pacman 原生", "", false},
    {"vlc", "VLC 媒体播放器", "影音图像", "pacman", "vlc",
     "全格式本地影片、网络串流与音频解码播放器", "vlc", "Pacman 原生", "", false},
    {"amberol", "Amberol 音乐播放器", "影音图像", "pacman", "amberol",
     "纯净现代 GTK4 本地高音质音乐播放器", "amberol", "Pacman 原生", "", false},
    {"obs", "OBS Studio", "影音图像", "pacman", "obs-studio",
     "低开销游戏高帧率录屏与直播推流工作站", "obs", "Pacman 原生", "", false},
    {"kdenlive", "Kdenlive 视频剪辑", "影音图像", "pacman", "kdenlive",
     "开源多轨非线性视频剪辑、特效合成与调色工作站", "kdenlive", "Pacman 原生", "", false},
    {"gimp", "GIMP 图像编辑", "影音图像", "pacman", "gimp",
     "开源专业级图像编辑、截图处理与素材修图工具", "gimp", "Pacman 原生", "", false},

    // 3. 开发工具
    {"vscode", "Visual Studio Code", "开发工具", "native", "vscode",
     "微软官方 Linux 原生版 VS Code（完整扩展市场，自动优化 Wayland）", "code", "官方原生", "", false},
    {"docker", "Docker 容器引擎", "开发工具", "native", "docker",
     "Docker Engine + Compose v2（自动配置当前用户组，支持启停服务）", "docker", "原生服务", "docker", false},
    {"podman", "Podman 无守护容器", "开发工具", "pacman", "podman",
     "Rootless 免守护进程 OCI 容器引擎，与 Docker CLI 兼容", "podman", "Pacman 原生", "", false},
    {"git", "Git 版本控制", "开发工具", "pacman", "git",
     "分布式代码版本控制基础工具", "git", "Pacman 原生", "", false},
    {"nodejs", "Node.js + npm", "开发工具", "native", "nodejs",
     "Node.js 运行时与 npm 包管理器，前端及 AI CLI 工具必备", "node", "原生环境", "", false},
    {"python", "Python + uv 工具链", "开发工具", "native", "python",
     "Python 3、pip 与极速环境管理器 uv（vLLM / AI 脚本必备）", "python3", "原生环境", "", false},

    // 4. AI 与大模型
    {"ollama", "Ollama 本地大模型", "AI 与大模型", "native", "ollama",
     "自动匹配 NVIDIA CUDA / AMD ROCm 加速（支持一键启停后台服务）", "ollama", "AI 底座", "ollama", false},
    {"vllm", "vLLM 高并发推理", "AI 与大模型", "native", "vllm",
     "基于 uv 隔离环境部署至 /opt/spinelos-vllm（支持启停 OpenAI API 服务）", "spinel-vllm-serve", "AI 底座", "vllm", false},
    {"llamacpp", "llama.cpp 推理引擎", "AI 与大模型", "pacman", "llama-cpp",
     "轻量 C/C++ GGUF 大模型本地推理与显卡性能测试工具", "llama-cli", "AI 底座", "", false},
    {"chatgpt", "ChatGPT (OpenAI)", "AI 与大模型", "link", "https://chatgpt.com",
     "OpenAI 官方网页端（Linux 建议网页端或官网查看预览版；可固定为独立桌面窗口）", "https://chatgpt.com", "官方直达", "", true},
    {"antigravity", "Google Antigravity", "AI 与大模型", "link", "https://antigravity.google",
     "Google 官方 AI 编程与智能体工作台（前往官网下载最新原生客户端）", "https://antigravity.google", "官网下载", "", false},
    {"grok", "Grok (xAI)", "AI 与大模型", "link", "https://grok.com",
     "xAI 官方 Grok 网页端（无官方 Linux 客户端，支持固定为独立桌面窗口）", "https://grok.com", "官方直达", "", true},
    {"claude", "Claude (Anthropic)", "AI 与大模型", "link", "https://claude.ai",
     "Anthropic 官方 Claude 网页端（支持一键固定为独立桌面窗口应用）", "https://claude.ai", "官方直达", "", true},

    // 5. 游戏与性能
    {"steam", "Steam 客户端", "游戏与性能", "pacman", "steam",
     "Valve 官方 PC 游戏与 Proton 平台（自动启用 multilib 与系统代理）", "steam", "Pacman 原生", "", false},
    {"heroic", "Heroic Games Launcher", "游戏与性能", "native", "heroic",
     "Epic Games、GOG 与 Amazon Prime 游戏库（官方 Arch 原生包）", "heroic", "原生安装", "", false},
    {"lutris", "Lutris", "游戏与性能", "pacman", "lutris",
     "统一管理独立游戏、Wine 环境与主机模拟器", "lutris", "Pacman 原生", "", false},
    {"protonup", "ProtonUp-Qt", "游戏与性能", "native", "protonup-qt",
     "图形化下载并管理 Steam Proton-GE / Wine-GE（宿主机原生版）", "protonup-qt", "原生安装", "", false},
    {"gravitymark", "GravityMark GPU", "游戏与性能", "native", "gravitymark",
     "跨平台 GPU 显卡基准与压力测试（支持 Vulkan / 光追跑分出分）", "gravitymark", "原生安装", "", false},
    {"glmark2", "glmark2 基准测试", "游戏与性能", "pacman", "glmark2",
     "开源轻量 OpenGL / Wayland 显卡跑分工具，官方源极速直装", "glmark2", "Pacman 原生", "", false},

    // 6. 浏览器与实用
    {"chrome", "Google Chrome", "浏览器与实用", "native", "google-chrome-stable",
     "Google 官方浏览器（宿主机原生安装，直接调用主机显卡驱动）", "google-chrome-stable", "原生安装", "", false},
    {"chromium", "Chromium", "浏览器与实用", "pacman", "chromium",
     "官方软件源高速开源浏览器，国内镜像源极速直下", "chromium --ozone-platform-hint=auto", "Pacman 原生", "", false},
    {"firefox", "Firefox", "浏览器与实用", "pacman", "firefox",
     "Mozilla 开源隐私浏览器，国内镜像极速直下", "firefox", "Pacman 原生", "", false},
    {"discord", "Discord", "浏览器与实用", "pacman", "discord",
     "玩家组队语音开黑与游戏社区聊天客户端", "discord", "Pacman 原生", "", false},
    {"telegram", "Telegram Desktop", "浏览器与实用", "pacman", "telegram-desktop",
     "跨平台加密即时通讯与开发者社区客户端", "telegram-desktop", "Pacman 原生", "", false},
    {"qbittorrent", "qBittorrent", "浏览器与实用", "pacman", "qbittorrent",
     "轻量开源、无广告的 BT 与磁力高速下载器", "qbittorrent", "Pacman 原生", "", false},
    {"wine", "Wine 兼容层", "浏览器与实用", "pacman", "wine",
     "运行 Windows 程序和安装包，安装后可通过「Windows 应用」管理", "spinel-files", "Pacman 原生", "", false},
};

bool is_valid_pacman_pkg(const std::string &pkg) {
    if (pkg.empty() || pkg.size() > 64) return false;
    for (char ch : pkg) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.' && c != '+') return false;
    }
    return true;
}

bool is_valid_flatpak_id(const std::string &id) {
    if (id.empty() || id.size() > 96 || id.find('.') == std::string::npos) return false;
    for (char ch : id) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.') return false;
    }
    return true;
}

bool is_pacman_pkg_installed(const std::string &pkg) {
    if (!is_valid_pacman_pkg(pkg)) return false;
    std::string out;
    int rc = 1;
    return run_argv_capture({"pacman", "-Qq", pkg}, out, rc) && rc == 0;
}

bool is_native_app_installed(const std::string &app_id) {
    std::error_code ec;
    if (app_id == "chrome") {
        return has_executable("google-chrome-stable") ||
               fs::exists("/opt/google/chrome/google-chrome", ec);
    }
    if (app_id == "libreoffice") {
        return has_executable("libreoffice") ||
               is_pacman_pkg_installed("libreoffice-fresh");
    }
    if (app_id == "printing") {
        return has_executable("cupsd") ||
               is_pacman_pkg_installed("cups");
    }
    if (app_id == "vscode") {
        return has_executable("code") ||
               fs::exists("/opt/vscode/code", ec);
    }
    if (app_id == "docker") {
        return has_executable("docker") ||
               is_pacman_pkg_installed("docker");
    }
    if (app_id == "nodejs") {
        return has_executable("node") && has_executable("npm");
    }
    if (app_id == "python") {
        return has_executable("python3") && has_executable("uv");
    }
    if (app_id == "ollama") {
        return has_executable("ollama") ||
               is_pacman_pkg_installed("ollama") ||
               is_pacman_pkg_installed("ollama-cuda") ||
               is_pacman_pkg_installed("ollama-rocm") ||
               is_pacman_pkg_installed("ollama-vulkan");
    }
    if (app_id == "vllm") {
        return fs::exists("/opt/spinelos-vllm/bin/vllm", ec);
    }
    if (app_id == "heroic") {
        return is_pacman_pkg_installed("heroic") ||
               is_pacman_pkg_installed("heroic-games-launcher-bin") ||
               has_executable("heroic");
    }
    if (app_id == "protonup") {
        return has_executable("protonup-qt") ||
               fs::exists("/opt/protonup-qt/AppRun", ec);
    }
    if (app_id == "gravitymark") {
        return has_executable("gravitymark") ||
               fs::exists("/opt/gravitymark/run_browser.sh", ec);
    }
    return false;
}

bool is_supported_native_app_id(const std::string &app_id) {
    return app_id == "chrome" || app_id == "heroic" || app_id == "protonup" || app_id == "gravitymark" ||
           app_id == "libreoffice" || app_id == "printing" || app_id == "vscode" || app_id == "docker" ||
           app_id == "nodejs" || app_id == "python" || app_id == "ollama" || app_id == "vllm";
}

void detect_gpu_vendors(bool &has_nvidia, bool &has_amd) {
    std::error_code ec;
    has_nvidia = fs::exists("/proc/driver/nvidia", ec) ||
                 fs::exists("/dev/nvidia0", ec) ||
                 has_executable("nvidia-smi");
    has_amd = false;
    for (const auto &entry : fs::directory_iterator("/sys/class/drm", ec)) {
        std::string name = entry.path().filename().string();
        if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos) continue;
        std::string vendor = trim_copy(read_first_line((entry.path() / "device" / "vendor").string()));
        if (vendor == "0x1002") {
            has_amd = true;
            break;
        }
    }
}

std::string current_username_safe() {
    if (const struct passwd *pw = getpwuid(getuid())) {
        if (pw->pw_name && pw->pw_name[0] != '\0') {
            std::string u(pw->pw_name);
            bool valid = !u.empty() && u.size() <= 32;
            for (char ch : u) {
                unsigned char c = static_cast<unsigned char>(ch);
                if (!std::isalnum(c) && ch != '_' && ch != '-') valid = false;
            }
            if (valid) return u;
        }
    }
    return {};
}

bool is_store_service_active(const std::string &service_unit) {
    if (!is_valid_store_service_unit(service_unit)) return false;
    std::string out;
    int rc = 1;
    if (is_user_systemd_service_unit(service_unit)) {
        return run_argv_capture({"systemctl", "--user", "is-active", "--quiet", "spinel-vllm.service"}, out, rc) && rc == 0;
    }
    return run_argv_capture({"systemctl", "is-active", "--quiet", service_unit + ".service"}, out, rc) && rc == 0;
}

fs::path webapp_desktop_path(const std::string &app_id) {
    return fs::path(home_dir()) / ".local/share/applications" / ("spinel-ai-" + app_id + ".desktop");
}

bool write_webapp_desktop_file(const StoreAppEntry &entry) {
    if (!entry.webapp_capable || !is_valid_https_url(entry.pkg)) return false;
    fs::path path = webapp_desktop_path(entry.id);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) return false;
    ofs << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=" << entry.name << "\n"
        << "Comment=" << entry.summary << "\n"
        << "Exec=sh -c 'if command -v google-chrome-stable >/dev/null 2>&1; then exec google-chrome-stable --app=\"" << entry.pkg << "\"; "
        << "elif command -v chromium >/dev/null 2>&1; then exec chromium --ozone-platform-hint=auto --app=\"" << entry.pkg << "\"; "
        << "elif command -v firefox >/dev/null 2>&1; then exec firefox \"" << entry.pkg << "\"; "
        << "else exec xdg-open \"" << entry.pkg << "\"; fi'\n"
        << "Icon=applications-internet\n"
        << "Categories=Network;ArtificialIntelligence;Utility;\n"
        << "StartupNotify=true\n"
        << "Terminal=false\n";
    ofs.close();
    std::string out;
    int rc = 0;
    run_argv_capture({"update-desktop-database", path.parent_path().string()}, out, rc);
    return true;
}

bool remove_webapp_desktop_file(const std::string &app_id) {
    std::error_code ec;
    fs::path path = webapp_desktop_path(app_id);
    fs::remove(path, ec);
    std::string out;
    int rc = 0;
    run_argv_capture({"update-desktop-database", path.parent_path().string()}, out, rc);
    return true;
}

bool ensure_vllm_user_service() {
    std::error_code ec;
    fs::path home(home_dir());
    fs::path conf_dir = home / ".config/spinelos";
    fs::path conf_path = conf_dir / "vllm.conf";
    fs::create_directories(conf_dir, ec);
    if (!fs::exists(conf_path, ec)) {
        std::ofstream cfg(conf_path, std::ios::trunc);
        if (cfg.is_open()) {
            cfg << "# SpinelOS vLLM 后台服务配置 (OpenAI 兼容 API)\n"
                << "VLLM_MODEL=Qwen/Qwen2.5-1.5B-Instruct\n"
                << "VLLM_PORT=8000\n"
                << "VLLM_EXTRA_ARGS=--gpu-memory-utilization 0.85\n";
        }
    }

    fs::path unit_dir = home / ".config/systemd/user";
    fs::create_directories(unit_dir, ec);
    std::ofstream unit(unit_dir / "spinel-vllm.service", std::ios::trunc);
    if (!unit.is_open()) return false;
    unit << "[Unit]\n"
         << "Description=SpinelOS vLLM OpenAI-Compatible Inference Server\n"
         << "After=network-online.target\n\n"
         << "[Service]\n"
         << "Type=simple\n"
         << "EnvironmentFile=-%h/.config/spinelos/vllm.conf\n"
         << "ExecStart=/usr/bin/spinel-vllm-serve\n"
         << "Restart=on-failure\n"
         << "RestartSec=3\n\n"
         << "[Install]\n"
         << "WantedBy=default.target\n";
    unit.close();
    return true;
}

bool is_flatpak_app_installed(const std::string &app_id) {
    if (!is_valid_flatpak_id(app_id)) return false;
    std::error_code ec;
    fs::path user_dir = fs::path(home_dir()) / ".local/share/flatpak/app" / app_id;
    fs::path sys_dir = fs::path("/var/lib/flatpak/app") / app_id;
    return fs::is_directory(user_dir, ec) || fs::is_directory(sys_dir, ec);
}

bool has_flatpak_cli() {
    std::string out;
    int rc = 1;
    return run_argv_capture({"which", "flatpak"}, out, rc) && rc == 0;
}

bool check_sudo_nopasswd() {
    std::string out;
    int rc = 1;
    return run_argv_capture({"sudo", "-n", "true"}, out, rc) && rc == 0;
}

enum class StoreIdleKind { Line, Progress, Done };

struct StoreIdleMsg {
    AppState *state;
    StoreIdleKind kind = StoreIdleKind::Line;
    std::string line;
    int exit_code = 0;
    std::string target_id;
    std::string action;
};

void eval_store_and_settings_views(AppState *state, const std::string &js) {
    if (!state) return;
    if (state->store_view) eval_js(state->store_view, js);
    if (state->settings_view) eval_js(state->settings_view, js);
}

gboolean on_store_idle(gpointer data) {
    std::unique_ptr<StoreIdleMsg> msg(static_cast<StoreIdleMsg *>(data));
    if (msg->state->shutting_down.load()) return G_SOURCE_REMOVE;

    if (msg->kind == StoreIdleKind::Done) {
        msg->state->store_busy.store(false);
        push_desktop_apps(msg->state);
        push_store_catalog(msg->state);

        if (msg->state->store_view || msg->state->settings_view) {
            std::ostringstream js;
            js << "window.__spinelStoreDone && window.__spinelStoreDone("
               << (msg->exit_code == 0 ? "true" : "false") << ",\""
               << json_escape(msg->target_id) << "\",\""
               << json_escape(msg->action) << "\");";
            eval_store_and_settings_views(msg->state, js.str());
        }
        return G_SOURCE_REMOVE;
    }

    if (msg->kind == StoreIdleKind::Progress) {
        if (msg->state->store_view || msg->state->settings_view) {
            std::ostringstream js;
            js << "window.__spinelStoreProgress && window.__spinelStoreProgress("
               << msg->exit_code << ",\"" << json_escape(msg->line) << "\");";
            eval_store_and_settings_views(msg->state, js.str());
        }
        return G_SOURCE_REMOVE;
    }

    if (msg->state->store_view || msg->state->settings_view) {
        std::ostringstream js;
        js << "window.__spinelStoreLog && window.__spinelStoreLog(\""
           << json_escape(msg->line) << "\");";
        eval_store_and_settings_views(msg->state, js.str());
    }
    return G_SOURCE_REMOVE;
}

void emit_store_line(AppState *state, const std::string &line) {
    if (!state || state->shutting_down.load()) return;
    g_idle_add(on_store_idle, new StoreIdleMsg{state, StoreIdleKind::Line, line, 0, "", ""});
}

void emit_store_progress(AppState *state, int pct, const std::string &label) {
    if (!state || state->shutting_down.load()) return;
    g_idle_add(on_store_idle, new StoreIdleMsg{state, StoreIdleKind::Progress, label, pct, "", ""});
}

void emit_store_done(AppState *state, int exit_code, const std::string &target_id, const std::string &action) {
    if (!state || state->shutting_down.load()) return;
    g_idle_add(on_store_idle, new StoreIdleMsg{state, StoreIdleKind::Done, "", exit_code, target_id, action});
}

bool has_installed_flatpak_apps() {
    if (!has_flatpak_cli()) return false;
    std::string out;
    int rc = 1;
    if (!run_argv_capture({"flatpak", "list", "--app", "--columns=application"}, out, rc) || rc != 0) {
        return false;
    }
    return !trim_copy(out).empty();
}

void ensure_flathub_china_mirror(AppState *state,
                                 const std::vector<std::string> *base_sudo,
                                 const std::string &pwd_in) {
    if (!has_flatpak_cli()) return;
    static constexpr const char *kSjtuFlathubUrl = "https://mirror.sjtu.edu.cn/flathub";
    std::string remotes_out;
    int rc = 1;
    bool changed = false;
    if (run_argv_capture({"flatpak", "remotes", "--user", "--columns=name,url"}, remotes_out, rc) && rc == 0) {
        if (remotes_out.find("flathub") != std::string::npos &&
            remotes_out.find(kSjtuFlathubUrl) == std::string::npos) {
            std::string mod_out;
            int mod_rc = 1;
            if (run_argv_capture({"flatpak", "remote-modify", "--user", "flathub",
                                  std::string("--url=") + kSjtuFlathubUrl}, mod_out, mod_rc) && mod_rc == 0) {
                changed = true;
            }
        }
    }
    remotes_out.clear();
    if (base_sudo && run_argv_capture({"flatpak", "remotes", "--system", "--columns=name,url"}, remotes_out, rc) && rc == 0) {
        if (remotes_out.find("flathub") != std::string::npos &&
            remotes_out.find(kSjtuFlathubUrl) == std::string::npos) {
            std::vector<std::string> cmd = *base_sudo;
            cmd.insert(cmd.end(), {"flatpak", "remote-modify", "--system", "flathub",
                                   std::string("--url=") + kSjtuFlathubUrl});
            std::string mod_out;
            int mod_rc = 1;
            if (run_argv_stdin_capture(cmd, pwd_in, mod_out, mod_rc) && mod_rc == 0) {
                changed = true;
            }
        }
    }
    if (changed && state) {
        emit_store_line(state, "==> 已将 Flathub 切换为上海交大国内镜像（https://mirror.sjtu.edu.cn/flathub，注：索引元数据仍回源官方，大文件走国内镜像加速）。");
    }
}

std::vector<std::pair<std::string, std::string>> build_proxy_env_pairs() {
    return build_effective_proxy_envs();
}

void append_proxy_env_prefix(std::vector<std::string> &cmd) {
    auto envs = build_proxy_env_pairs();
    if (envs.empty()) return;
    cmd.push_back("env");
    for (const auto &[k, v] : envs) {
        cmd.push_back(k + "=" + v);
    }
}

bool is_multilib_enabled() {
    std::ifstream in("/etc/pacman.conf");
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim_copy(line);
        if (t == "[multilib]") return true;
    }
    return false;
}

bool extract_percent_hint(const std::string &line, int &pct_out) {
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] != '%' || i == 0) continue;
        size_t j = i;
        while (j > 0 && std::isdigit(static_cast<unsigned char>(line[j - 1]))) {
            --j;
        }
        if (j < i) {
            int v = std::atoi(line.substr(j, i - j).c_str());
            if (v >= 0 && v <= 100) {
                pct_out = v;
                return true;
            }
        }
    }
    return false;
}

void dispatch_stream_segment(AppState *state, const std::string &raw, bool from_cr) {
    std::string line = trim_copy(raw);
    if (line.empty()) return;

    const std::string prog_prefix = "__SPINEL_PROG__:";
    if (line.rfind(prog_prefix, 0) == 0) {
        std::string rest = line.substr(prog_prefix.size());
        auto sep = rest.find(':');
        if (sep != std::string::npos) {
            int pct = std::atoi(rest.substr(0, sep).c_str());
            std::string label = trim_copy(rest.substr(sep + 1));
            emit_store_progress(state, pct, label);
        }
        return;
    }

    int pct = -1;
    if (extract_percent_hint(line, pct)) {
        emit_store_progress(state, pct, from_cr ? "正在下载/处理软件包..." : line);
    }
    // Suppress raw carriage-return progress redraws (curl/pacman/flatpak) from spamming the log box
    if (from_cr) return;
    if (line.find("% Total") != std::string::npos || line.find("Dload  Upload") != std::string::npos) return;

    emit_store_line(state, line);
}

int run_streaming_command(AppState *state,
                          const std::vector<std::string> &args,
                          const std::string &stdin_data = "",
                          const std::vector<std::pair<std::string, std::string>> &env_overrides = {}) {
    if (args.empty()) return 1;
    if (state && state->store_cancel_requested.load()) return 130;

    int stdin_pipe[2]{-1, -1};
    int stdout_pipe[2]{-1, -1};
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        if (stdin_pipe[0] >= 0) { close(stdin_pipe[0]); close(stdin_pipe[1]); }
        if (stdout_pipe[0] >= 0) { close(stdout_pipe[0]); close(stdout_pipe[1]); }
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        return 1;
    }

    if (pid == 0) {
        setpgid(0, 0);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);

        for (const auto &[k, v] : env_overrides) {
            setenv(k.c_str(), v.c_str(), 1);
        }

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execvp(args[0].c_str(), argv.data());
        _exit(127);
    }

    setpgid(pid, pid);
    if (state) {
        state->store_child_pgid.store(pid);
        if (state->store_cancel_requested.load()) {
            kill(-pid, SIGTERM);
        }
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    if (!stdin_data.empty()) {
        std::string payload = stdin_data + "\n";
        size_t written = 0;
        while (written < payload.size()) {
            ssize_t n = write(stdin_pipe[1], payload.data() + written, payload.size() - written);
            if (n <= 0) break;
            written += static_cast<size_t>(n);
        }
    }
    close(stdin_pipe[1]);

    std::string pending;
    std::array<char, 1024> buf{};
    while (true) {
        ssize_t n = read(stdout_pipe[0], buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            char c = buf[static_cast<size_t>(i)];
            if (c == '\r' || c == '\n') {
                dispatch_stream_segment(state, pending, c == '\r');
                pending.clear();
            } else {
                pending.push_back(c);
            }
        }
    }
    if (!pending.empty()) {
        dispatch_stream_segment(state, pending, false);
    }
    close(stdout_pipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (state) {
        pid_t expected = pid;
        state->store_child_pgid.compare_exchange_strong(expected, -1);
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

const StoreAppEntry *find_catalog_entry(const std::string &id) {
    for (const auto &entry : kCatalog) {
        if (id == entry.id) return &entry;
    }
    return nullptr;
}

}  // namespace

void push_store_catalog(AppState *state) {
    if (!state || (!state->store_view && !state->settings_view)) return;
    bool flatpak_ok = has_flatpak_cli();
    bool sudo_nopass = check_sudo_nopasswd();
    GpuDriverUpdateInfo gi = collect_gpu_driver_update_info();

    std::ostringstream js;
    js << "window.__spinelStoreData && window.__spinelStoreData({"
       << "\"flatpak_ready\":" << (flatpak_ok ? "true" : "false") << ","
       << "\"sudo_nopass\":" << (sudo_nopass ? "true" : "false") << ","
       << "\"busy\":" << (state->store_busy.load() ? "true" : "false") << ","
       << "\"driver_summary\":\"" << json_escape(gi.driver_summary) << "\","
       << "\"kernel_summary\":\"" << json_escape(gi.kernel_running) << "\","
       << "\"reboot_required\":" << (gi.reboot_required ? "true" : "false") << ","
       << "\"updates_total\":" << gi.updates_total << ","
       << "\"gpu_updates\":[";
    for (size_t i = 0; i < gi.gpu_updates.size(); ++i) {
        if (i > 0) js << ",";
        js << "\"" << json_escape(gi.gpu_updates[i]) << "\"";
    }
    js << "],"
       << "\"apps\":[";

    bool first = true;
    for (const auto &app : kCatalog) {
        if (!first) js << ",";
        first = false;
        bool installed = false;
        std::string bkend(app.backend);
        std::error_code ec;
        bool webapp_pinned = app.webapp_capable && fs::exists(webapp_desktop_path(app.id), ec);
        if (bkend == "flatpak") {
            installed = is_flatpak_app_installed(app.pkg);
        } else if (bkend == "native") {
            installed = is_native_app_installed(app.id);
        } else if (bkend == "link") {
            installed = webapp_pinned;
        } else {
            installed = is_pacman_pkg_installed(app.pkg);
        }
        bool svc_active = (app.service_unit && app.service_unit[0] != '\0' && installed)
                              ? is_store_service_active(app.service_unit)
                              : false;
        js << "{"
           << "\"id\":\"" << json_escape(app.id) << "\","
           << "\"name\":\"" << json_escape(app.name) << "\","
           << "\"category\":\"" << json_escape(app.category) << "\","
           << "\"backend\":\"" << json_escape(app.backend) << "\","
           << "\"pkg\":\"" << json_escape(app.pkg) << "\","
           << "\"summary\":\"" << json_escape(app.summary) << "\","
           << "\"badge\":\"" << json_escape(app.badge) << "\","
           << "\"service_unit\":\"" << json_escape(app.service_unit ? app.service_unit : "") << "\","
           << "\"service_active\":" << (svc_active ? "true" : "false") << ","
           << "\"webapp_capable\":" << (app.webapp_capable ? "true" : "false") << ","
           << "\"webapp_pinned\":" << (webapp_pinned ? "true" : "false") << ","
           << "\"installed\":" << (installed ? "true" : "false")
           << "}";
    }
    js << "]});";
    eval_store_and_settings_views(state, js.str());
}

void start_store_action(AppState *state, const std::string &action, const std::string &payload) {
    if (!state || state->store_busy.load()) return;

    std::string app_id;
    std::string backend;
    std::string pkg;
    std::string password;

    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "id") app_id = v;
        else if (k == "backend") backend = v;
        else if (k == "pkg") pkg = v;
        else if (k == "password") password = v;
    }

    const StoreAppEntry *catalog_entry = find_catalog_entry(app_id);
    if (catalog_entry) {
        backend = catalog_entry->backend;
        if (action != "svc-start" && action != "svc-stop") {
            pkg = catalog_entry->pkg;
        }
    } else if (app_id.empty()) {
        app_id = pkg;
    }

    bool is_sys_maint = (action == "sys-update" || action == "spinel-ota" || action == "spinel-ota-full" ||
                         action == "spinel-ota-offline" || action == "mirror-test" || action == "keyring-fix");
    bool is_svc_op = (action == "svc-start" || action == "svc-stop");
    bool is_webapp_op = (action == "webapp-pin" || action == "webapp-unpin");

    std::string svc_unit = (catalog_entry && catalog_entry->service_unit[0] != '\0') ? catalog_entry->service_unit : pkg;

    if (is_webapp_op) {
        if (!catalog_entry || !catalog_entry->webapp_capable) {
            emit_store_line(state, "[ERROR] 该应用不支持桌面 WebApp 快捷方式: " + app_id);
            emit_store_done(state, 1, app_id, action);
            return;
        }
    } else if (is_svc_op) {
        if (!is_valid_store_service_unit(svc_unit)) {
            emit_store_line(state, "[ERROR] 不受支持或非法的服务名: " + svc_unit);
            emit_store_done(state, 1, app_id, action);
            return;
        }
    } else if (!is_sys_maint) {
        if (backend == "flatpak") {
            if (!is_valid_flatpak_id(pkg)) {
                emit_store_line(state, "[ERROR] 无效的 Flatpak 应用 ID: " + pkg);
                emit_store_done(state, 1, app_id, action);
                return;
            }
            if (!has_flatpak_cli()) {
                emit_store_line(state, "[ERROR] 系统尚未安装 flatpak。请先通过系统更新 (--full) 或在商店安装 flatpak 包。");
                emit_store_done(state, 1, app_id, action);
                return;
            }
        } else if (backend == "native") {
            if (!is_supported_native_app_id(app_id)) {
                emit_store_line(state, "[ERROR] 不支持的原生应用 ID: " + app_id);
                emit_store_done(state, 1, app_id, action);
                return;
            }
        } else {
            backend = "pacman";
            if (!is_valid_pacman_pkg(pkg)) {
                emit_store_line(state, "[ERROR] 无效的 Pacman 软件包名: " + pkg);
                emit_store_done(state, 1, app_id, action);
                return;
            }
        }
    }

    if (state->store_thread.joinable()) {
        state->store_thread.join();
    }
    state->store_cancel_requested.store(false);
    state->store_child_pgid.store(-1);
    state->store_busy.store(true);

    state->store_thread = std::thread([state, action, app_id, backend, pkg, password,
                                       is_sys_maint, is_svc_op, is_webapp_op, svc_unit, catalog_entry]() {
        int rc = 1;
        auto proxy_envs = build_proxy_env_pairs();
        auto make_sudo_cmd = [](bool nopass) {
            return nopass ? std::vector<std::string>{"sudo", "-n"}
                          : std::vector<std::string>{"sudo", "-S", "-p", ""};
        };
        auto ensure_multilib = [&](const std::vector<std::string> &base_sudo, const std::string &pwd_in) {
            if (is_multilib_enabled()) return true;
            emit_store_line(state, "==> 检测到 [multilib] 尚未开启，正在自动启用 32 位游戏软件源...");
            std::vector<std::string> mcmd = base_sudo;
            mcmd.insert(mcmd.end(), {
                "sh", "-c",
                "if grep -q '^#\\[multilib\\]' /etc/pacman.conf; then "
                "sed -i '/^#\\[multilib\\]/{s/^#//;n;s/^#//}' /etc/pacman.conf; "
                "elif ! grep -q '^\\[multilib\\]' /etc/pacman.conf; then "
                "printf '\\n[multilib]\\nInclude = /etc/pacman.d/mirrorlist\\n' >> /etc/pacman.conf; fi"
            });
            return run_streaming_command(state, mcmd, pwd_in) == 0;
        };

        if (is_webapp_op && catalog_entry) {
            bool ok = (action == "webapp-pin") ? write_webapp_desktop_file(*catalog_entry)
                                               : remove_webapp_desktop_file(catalog_entry->id);
            emit_store_line(state, ok ? ("==> 已更新桌面快捷方式: " + std::string(catalog_entry->name))
                                      : "[ERROR] 写入或移除快捷方式失败");
            rc = ok ? 0 : 1;
        } else if (is_svc_op) {
            bool start_flag = (action == "svc-start");
            if (is_user_systemd_service_unit(svc_unit)) {
                std::error_code ec;
                if (start_flag && !fs::exists("/opt/spinelos-vllm/bin/vllm", ec)) {
                    emit_store_line(state, "[ERROR] 尚未安装 vLLM，请先在应用商店点击下载安装。");
                    emit_store_done(state, 1, app_id, action);
                    return;
                }
                if (start_flag) ensure_vllm_user_service();
                emit_store_line(state, std::string("==> 正在") + (start_flag ? "启动并启用" : "停止并禁用") + " 用户服务 spinel-vllm.service ...");
                std::vector<std::string> cmd = {
                    "sh", "-c",
                    start_flag
                        ? "systemctl --user daemon-reload && systemctl --user enable --now spinel-vllm.service"
                        : "systemctl --user disable --now spinel-vllm.service"
                };
                rc = run_streaming_command(state, cmd, "");
            } else {
                bool nopass = check_sudo_nopasswd();
                if (!nopass && password.empty()) {
                    emit_store_line(state, "[ERROR] 管理系统服务需要当前用户管理员密码。");
                    emit_store_done(state, 2, app_id, action);
                    return;
                }
                std::vector<std::string> base_sudo = make_sudo_cmd(nopass);
                std::string pwd_in = nopass ? "" : password;
                std::string unit_full = svc_unit + ".service";
                emit_store_line(state, std::string("==> 正在") + (start_flag ? "启动并设置开机启用 " : "停止并取消开机启用 ") + unit_full + " ...");
                std::vector<std::string> cmd = base_sudo;
                if (start_flag && svc_unit == "docker") {
                    std::string uname = current_username_safe();
                    if (!uname.empty()) {
                        cmd.insert(cmd.end(), {"sh", "-c", "usermod -aG docker '" + uname + "' >/dev/null 2>&1 || true; systemctl enable --now docker.service"});
                    } else {
                        cmd.insert(cmd.end(), {"systemctl", "enable", "--now", unit_full});
                    }
                } else {
                    cmd.insert(cmd.end(), {"systemctl", start_flag ? "enable" : "disable", "--now", unit_full});
                }
                rc = run_streaming_command(state, cmd, pwd_in);
            }
        } else if (is_sys_maint) {
            bool nopass = check_sudo_nopasswd();
            if (!nopass && password.empty()) {
                emit_store_line(state, "[ERROR] 系统维护操作需要当前用户管理员密码。");
                emit_store_done(state, 2, app_id, action);
                return;
            }
            std::vector<std::string> base_sudo = make_sudo_cmd(nopass);
            std::string pwd_in = nopass ? "" : password;

            if (action == "sys-update") {
                ensure_multilib(base_sudo, pwd_in);
                std::vector<std::string> cmd = base_sudo;
                append_proxy_env_prefix(cmd);
                cmd.insert(cmd.end(), {"pacman", "-Syu", "--noconfirm"});
                emit_store_line(state, "==> 正在全面更新 Spinel OS 系统软件包...");
                rc = run_streaming_command(state, cmd, pwd_in);
                if (rc == 0 && !state->store_cancel_requested.load() && has_flatpak_cli()) {
                    if (!has_installed_flatpak_apps()) {
                        emit_store_line(state, "==> 未安装 Flatpak 应用，已跳过 Flatpak 同步。");
                    } else {
                        if (proxy_envs.empty()) {
                            ensure_flathub_china_mirror(state, &base_sudo, pwd_in);
                        }
                        emit_store_line(state, "==> 正在同步更新 Flatpak 应用（限时 90 秒，防止海外元数据卡死）...");
                        int f_rc = run_streaming_command(
                            state, {"timeout", "90", "flatpak", "update", "-y", "--noninteractive"}, "", proxy_envs);
                        if (f_rc == 124) {
                            emit_store_line(state, "[提示] Flatpak 更新超时（Flathub 索引元数据仍需访问海外服务器）；Arch 系统软件包已升级完成，开启系统代理后可再更新 Flatpak。");
                        }
                    }
                }
            } else if (action == "spinel-ota" || action == "spinel-ota-full" || action == "spinel-ota-offline") {
                std::string updater = has_executable("spinel-update") ? "spinel-update" : "";
                if (updater.empty() && fs::exists("/usr/local/bin/spinel-update")) {
                    updater = "/usr/local/bin/spinel-update";
                }
                if (updater.empty()) {
                    emit_store_line(state, "[ERROR] 未检测到 spinel-update 在线更新器，请先使用 dev-deploy.sh 或 bootstrap.sh 初始化。");
                    rc = 1;
                } else if (action == "spinel-ota-offline") {
                    std::error_code ec;
                    bool valid_path = !pkg.empty() && pkg.find('\n') == std::string::npos &&
                                      pkg.find('\r') == std::string::npos &&
                                      fs::is_regular_file(pkg, ec);
                    if (!valid_path) {
                        emit_store_line(state, "[ERROR] 离线更新包路径无效或文件不存在: " + pkg);
                        rc = 1;
                    } else {
                        std::vector<std::string> cmd = base_sudo;
                        append_proxy_env_prefix(cmd);
                        cmd.insert(cmd.end(), {updater, "offline", pkg, "--full", "--yes", "--no-kill-webui"});
                        emit_store_line(state, "==> 正在使用离线包执行 Spinel OS 完整升级 (--full): " + pkg);
                        rc = run_streaming_command(state, cmd, pwd_in);
                    }
                } else {
                    std::vector<std::string> cmd = base_sudo;
                    append_proxy_env_prefix(cmd);
                    cmd.insert(cmd.end(), {updater, "upgrade",
                                           action == "spinel-ota-full" ? "--full" : "--app",
                                           "--yes", "--no-kill-webui"});
                    emit_store_line(state, action == "spinel-ota-full"
                                               ? "==> 正在执行 Spinel OS 完整在线升级 (--full)..."
                                               : "==> 正在执行 Spinel OS 组件与桌面在线升级...");
                    rc = run_streaming_command(state, cmd, pwd_in);
                }
                if (rc == 0 && !state->store_cancel_requested.load()) {
                    emit_store_line(state, "==> Spinel OS 升级完成！桌面外壳已热更新；若包含内核/引导改动可点击上方重启系统。");
                }
            } else if (action == "mirror-test") {
                if (!has_executable("reflector")) {
                    emit_store_line(state, "[ERROR] 未检测到 reflector 工具，请先安装 reflector。");
                    rc = 1;
                } else {
                    std::vector<std::string> cmd = base_sudo;
                    cmd.insert(cmd.end(), {
                        "reflector", "--country", "China", "--protocol", "https",
                        "--latest", "8", "--sort", "rate", "--save", "/etc/pacman.d/mirrorlist"
                    });
                    emit_store_line(state, "==> 正在测速中国区 HTTPS 镜像站（约需 10~20 秒）...");
                    rc = run_streaming_command(state, cmd, pwd_in);
                    if (rc == 0 && !state->store_cancel_requested.load()) {
                        std::vector<std::string> sync_cmd = base_sudo;
                        sync_cmd.insert(sync_cmd.end(), {"pacman", "-Syy", "--noconfirm"});
                        emit_store_line(state, "==> 测速完成，正在刷新软件包索引...");
                        rc = run_streaming_command(state, sync_cmd, pwd_in);
                        if (rc == 0) {
                            ensure_flathub_china_mirror(state, &base_sudo, pwd_in);
                        }
                    }
                }
            } else if (action == "keyring-fix") {
                std::vector<std::string> cmd1 = base_sudo;
                cmd1.insert(cmd1.end(), {"pacman", "-Sy", "--noconfirm", "archlinux-keyring"});
                emit_store_line(state, "==> 正在更新系统签名密钥环...");
                rc = run_streaming_command(state, cmd1, pwd_in);
                if (rc == 0 && !state->store_cancel_requested.load()) {
                    std::vector<std::string> cmd2 = base_sudo;
                    cmd2.insert(cmd2.end(), {"pacman-key", "--populate", "archlinux"});
                    emit_store_line(state, "==> 正在重新校验并信任密钥环...");
                    rc = run_streaming_command(state, cmd2, pwd_in);
                }
            }
        } else if (backend == "flatpak") {
            if (action == "install") {
                if (proxy_envs.empty()) {
                    emit_store_line(state, "[提示] Flathub 索引元数据需访问海外服务器，正在配置上海交大国内镜像加速软件包下载；若超时卡住建议在「系统设置 → 网络」开启代理。");
                }
                emit_store_line(state, "==> 确保 Flathub 用户仓库已配置...");
                const char *repo_url = proxy_envs.empty()
                    ? "https://mirror.sjtu.edu.cn/flathub/flathub.flatpakrepo"
                    : "https://dl.flathub.org/repo/flathub.flatpakrepo";
                rc = run_streaming_command(state, {
                    "flatpak", "remote-add", "--user", "--if-not-exists",
                    "flathub", repo_url
                }, "", proxy_envs);
                if (rc == 0 && proxy_envs.empty()) {
                    ensure_flathub_china_mirror(state, nullptr, "");
                }
                if (rc == 0 && !state->store_cancel_requested.load()) {
                    emit_store_line(state, "==> 正在从 Flathub 安装 " + pkg + " ...");
                    rc = run_streaming_command(state, {
                        "flatpak", "install", "--user", "-y", "--noninteractive", "flathub", pkg
                    }, "", proxy_envs);
                }
            } else {
                emit_store_line(state, "==> 正在卸载 Flatpak 应用 " + pkg + " ...");
                rc = run_streaming_command(state, {
                    "flatpak", "uninstall", "--user", "-y", "--noninteractive", pkg
                }, "", proxy_envs);
            }
        } else if (backend == "native") {
            bool nopass = check_sudo_nopasswd();
            if (!nopass && password.empty()) {
                emit_store_line(state, "[ERROR] 原生安装/卸载应用需要当前用户管理员密码。");
                emit_store_done(state, 2, app_id, action);
                return;
            }
            std::vector<std::string> base_sudo = make_sudo_cmd(nopass);
            std::string pwd_in = nopass ? "" : password;
            std::vector<std::string> cmd = base_sudo;
            append_proxy_env_prefix(cmd);

            const std::string dl_fn =
                "dl_progress() { "
                "  _url=\"$1\"; _out=\"$2\"; _label=\"$3\"; "
                "  _total=$(curl -fsSLI --connect-timeout 10 \"$_url\" 2>/dev/null | awk 'tolower($1)==\"content-length:\"{gsub(\"\\r\",\"\",$2); len=$2} END{print len+0}'); "
                "  _cur=$(wc -c < \"$_out\" 2>/dev/null || echo 0); "
                "  if [ \"${_total:-0}\" -gt 0 ] && [ \"$_cur\" -eq \"$_total\" ]; then "
                "    echo \"__SPINEL_PROG__:92:${_label} 已缓存，准备安装...\"; return 0; "
                "  fi; "
                "  [ \"${_total:-0}\" -gt 0 ] && [ \"$_cur\" -gt \"$_total\" ] && rm -f \"$_out\"; "
                "  curl -fL --silent --show-error --continue-at - --connect-timeout 15 --retry 2 -o \"$_out\" \"$_url\" & "
                "  _pid=$!; "
                "  _last_sz=$(wc -c < \"$_out\" 2>/dev/null || echo 0); "
                "  while kill -0 \"$_pid\" 2>/dev/null && ! grep -q '^State:[[:space:]]*Z' \"/proc/$_pid/status\" 2>/dev/null; do "
                "    sleep 1; "
                "    _cur=$(wc -c < \"$_out\" 2>/dev/null || echo 0); "
                "    _spd=$(( _cur - _last_sz )); [ \"$_spd\" -lt 0 ] && _spd=0; _last_sz=$_cur; "
                "    _cur_mb=$(awk -v b=\"$_cur\" 'BEGIN{printf \"%.1f\", b/1048576}'); "
                "    _spd_str=$(awk -v s=\"$_spd\" 'BEGIN{if(s>=1048576) printf \"%.1f MB/s\", s/1048576; else printf \"%.0f KB/s\", s/1024}'); "
                "    if [ \"${_total:-0}\" -gt 0 ]; then "
                "      _pct=$(awk -v c=\"$_cur\" -v t=\"$_total\" 'BEGIN{p=int(c*88/t)+4; if(p>92)p=92; print p}'); "
                "      _tot_mb=$(awk -v b=\"$_total\" 'BEGIN{printf \"%.1f\", b/1048576}'); "
                "      echo \"__SPINEL_PROG__:${_pct}:${_label} ${_cur_mb} / ${_tot_mb} MB (${_spd_str})\"; "
                "    else "
                "      echo \"__SPINEL_PROG__:25:${_label} ${_cur_mb} MB (${_spd_str})\"; "
                "    fi; "
                "  done; "
                "  wait \"$_pid\"; "
                "}; ";

            if (app_id == "chrome") {
                if (action == "install") {
                    emit_store_line(state, "==> 正在确保 Google Chrome 宿主机运行依赖已就绪...");
                    emit_store_line(state, "==> 将直接下载 Google 官方稳定版并原生安装到 /opt/google/chrome（不经过 Flatpak）...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; " + dl_fn +
                        "STG=$(mktemp -d /tmp/spinel-chrome-XXXXXX); "
                        "trap 'rm -rf \"$STG\"' EXIT; "
                        "echo '__SPINEL_PROG__:3:正在检查 Chrome 运行依赖...'; "
                        "pacman -Sy --noconfirm --needed alsa-lib gtk3 libcups libdrm libxkbcommon nss xdg-utils ttf-liberation curl; "
                        "echo '==> 正在下载 google-chrome-stable_current_amd64.deb ...'; "
                        "dl_progress 'https://dl.google.com/linux/direct/google-chrome-stable_current_amd64.deb' \"$STG/chrome.deb\" '正在下载 Chrome'; "
                        "echo '__SPINEL_PROG__:94:正在解包安装 Google Chrome...'; "
                        "echo '==> 正在解包并安装到宿主机系统...'; "
                        "bsdtar -xf \"$STG/chrome.deb\" -C \"$STG\"; "
                        "mkdir -p \"$STG/root\"; "
                        "bsdtar -xf \"$STG\"/data.tar.* -C \"$STG/root\"; "
                        "rm -rf /opt/google/chrome; "
                        "mkdir -p /opt/google; "
                        "cp -a \"$STG/root/opt/google/chrome\" /opt/google/chrome; "
                        "[ -f /opt/google/chrome/chrome-sandbox ] && chmod 4755 /opt/google/chrome/chrome-sandbox || true; "
                        "ln -sf /opt/google/chrome/google-chrome /usr/bin/google-chrome-stable; "
                        "ln -sf /opt/google/chrome/google-chrome /usr/bin/google-chrome; "
                        "install -Dm644 \"$STG/root/usr/share/applications/google-chrome.desktop\" /usr/share/applications/google-chrome.desktop; "
                        "for sz in 16 24 32 48 64 128 256; do "
                        "  [ -f \"/opt/google/chrome/product_logo_${sz}.png\" ] && install -Dm644 \"/opt/google/chrome/product_logo_${sz}.png\" \"/usr/share/icons/hicolor/${sz}x${sz}/apps/google-chrome.png\"; "
                        "done; "
                        "update-desktop-database /usr/share/applications >/dev/null 2>&1 || true; "
                        "gtk-update-icon-cache -f /usr/share/icons/hicolor >/dev/null 2>&1 || true; "
                        "echo '==> Google Chrome 宿主机原生安装完成！'"
                    });
                } else {
                    emit_store_line(state, "==> 正在卸载宿主机原生 Google Chrome ...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; "
                        "rm -rf /opt/google/chrome /usr/bin/google-chrome-stable /usr/bin/google-chrome /usr/share/applications/google-chrome.desktop; "
                        "rm -f /usr/share/icons/hicolor/*/apps/google-chrome.png; "
                        "update-desktop-database /usr/share/applications >/dev/null 2>&1 || true; "
                        "gtk-update-icon-cache -f /usr/share/icons/hicolor >/dev/null 2>&1 || true; "
                        "echo '==> 已卸载 Google Chrome。'"
                    });
                }
            } else if (app_id == "heroic") {
                if (action == "install") {
                    emit_store_line(state, "==> 正在查询 Heroic Games Launcher 最新官方 Arch (.pacman) 原生包...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; " + dl_fn +
                        "STG=$(mktemp -d /tmp/spinel-heroic-XXXXXX); "
                        "trap 'rm -rf \"$STG\"' EXIT; "
                        "echo '__SPINEL_PROG__:4:正在获取 Heroic 最新版本信息...'; "
                        "URL=$(curl -fsSL --connect-timeout 15 'https://api.github.com/repos/Heroic-Games-Launcher/HeroicGamesLauncher/releases/latest' | grep -m1 -oE 'https://[^\"]+-linux-x64\\.pacman'); "
                        "[ -n \"$URL\" ] || { echo '[ERROR] 未获取到 Heroic 官方 .pacman 下载地址'; exit 1; }; "
                        "echo \"==> 正在下载 $URL ...\"; "
                        "dl_progress \"$URL\" \"$STG/heroic.pacman\" '正在下载 Heroic'; "
                        "echo '__SPINEL_PROG__:94:正在通过 Pacman 安装 Heroic...'; "
                        "echo '==> 正在通过 Pacman 原生安装 Heroic ...'; "
                        "pacman -Sy --noconfirm --needed >/dev/null 2>&1 || true; "
                        "pacman -U --noconfirm \"$STG/heroic.pacman\"; "
                        "echo '==> Heroic Games Launcher 原生安装完成！'"
                    });
                } else {
                    emit_store_line(state, "==> 正在卸载原生 Heroic Games Launcher ...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; "
                        "PKG=$(pacman -Qq heroic 2>/dev/null || pacman -Qq heroic-games-launcher-bin 2>/dev/null || pacman -Qqo /usr/bin/heroic 2>/dev/null || true); "
                        "[ -n \"$PKG\" ] && pacman -Rns --noconfirm \"$PKG\" || echo '==> 未检测到 Pacman 中的 Heroic 包。'"
                    });
                }
            } else if (app_id == "protonup") {
                if (action == "install") {
                    emit_store_line(state, "==> 正在查询 ProtonUp-Qt 最新官方版本（宿主机原生解包安装）...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; " + dl_fn +
                        "STG=$(mktemp -d /tmp/spinel-protonup-XXXXXX); "
                        "trap 'rm -rf \"$STG\"' EXIT; "
                        "echo '__SPINEL_PROG__:4:正在获取 ProtonUp-Qt 最新版本信息...'; "
                        "URL=$(curl -fsSL --connect-timeout 15 'https://api.github.com/repos/DavidoTek/ProtonUp-Qt/releases/latest' | grep -m1 -oE 'https://[^\"]+-x86_64\\.AppImage'); "
                        "[ -n \"$URL\" ] || { echo '[ERROR] 未获取到 ProtonUp-Qt 官方下载地址'; exit 1; }; "
                        "echo \"==> 正在下载 $URL ...\"; "
                        "dl_progress \"$URL\" \"$STG/ProtonUp-Qt.AppImage\" '正在下载 ProtonUp-Qt'; "
                        "chmod +x \"$STG/ProtonUp-Qt.AppImage\"; "
                        "echo '__SPINEL_PROG__:94:正在解包安装 ProtonUp-Qt...'; "
                        "echo '==> 正在解包安装到 /opt/protonup-qt ...'; "
                        "(cd \"$STG\" && ./ProtonUp-Qt.AppImage --appimage-extract >/dev/null); "
                        "rm -rf /opt/protonup-qt; "
                        "mv \"$STG/squashfs-root\" /opt/protonup-qt; "
                        "printf '#!/bin/sh\\nexec /opt/protonup-qt/AppRun \"$@\"\\n' > /usr/bin/protonup-qt; "
                        "chmod 755 /usr/bin/protonup-qt; "
                        "ICON_SRC=$(find /opt/protonup-qt -maxdepth 3 \\( -name '*pupgui2*.png' -o -name '.DirIcon' \\) | head -n1 || true); "
                        "[ -n \"$ICON_SRC\" ] && install -Dm644 \"$ICON_SRC\" /usr/share/icons/hicolor/256x256/apps/protonup-qt.png || true; "
                        "printf '[Desktop Entry]\\nType=Application\\nName=ProtonUp-Qt\\nComment=Install Wine-GE and Proton-GE for Steam and Lutris\\nExec=/usr/bin/protonup-qt\\nIcon=protonup-qt\\nCategories=Game;Utility;\\nTerminal=false\\n' > /usr/share/applications/protonup-qt.desktop; "
                        "update-desktop-database /usr/share/applications >/dev/null 2>&1 || true; "
                        "gtk-update-icon-cache -f /usr/share/icons/hicolor >/dev/null 2>&1 || true; "
                        "echo '==> ProtonUp-Qt 原生安装完成！'"
                    });
                } else {
                    emit_store_line(state, "==> 正在卸载宿主机原生 ProtonUp-Qt ...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; "
                        "rm -rf /opt/protonup-qt /usr/bin/protonup-qt /usr/share/applications/protonup-qt.desktop /usr/share/icons/hicolor/*/apps/protonup-qt.*; "
                        "update-desktop-database /usr/share/applications >/dev/null 2>&1 || true; "
                        "gtk-update-icon-cache -f /usr/share/icons/hicolor >/dev/null 2>&1 || true; "
                        "echo '==> 已卸载 ProtonUp-Qt。'"
                    });
                }
            } else if (app_id == "gravitymark") {
                if (action == "install") {
                    if (proxy_envs.empty()) {
                        emit_store_line(state, "[提示] GravityMark 安装包（约 184 MB）托管于海外官方源（tellusim.com），支持断点续传；若国内直连较慢，建议先在「系统设置 → 网络」开启代理，或先试用轻量版 glmark2。");
                    }
                    emit_store_line(state, "==> 正在确保 GravityMark 宿主机运行依赖已就绪...");
                    emit_store_line(state, "==> 正在查询 Tellusim 官方最新 Linux 原生安装包...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; " + dl_fn +
                        "CACHE_DIR=/var/cache/spinelos/gravitymark; "
                        "STG=$(mktemp -d /tmp/spinel-gravitymark-XXXXXX); "
                        "trap 'rm -rf \"$STG\"' EXIT; "
                        "mkdir -p \"$CACHE_DIR\"; "
                        "echo '__SPINEL_PROG__:3:正在检查 GravityMark 运行依赖...'; "
                        "pacman -Sy --noconfirm --needed libglvnd libx11 libxext libxrandr libxcb vulkan-icd-loader curl libarchive; "
                        "ARCH=$(uname -m); "
                        "if [ \"$ARCH\" = \"aarch64\" ]; then "
                        "  URL=$(curl -fsSL --connect-timeout 15 'https://gravitymark.tellusim.com/' | grep -m1 -oE 'https://tellusim\\.com/download/GravityMark_[0-9.]+_arm64\\.run' || true); "
                        "  [ -n \"$URL\" ] || URL='https://tellusim.com/download/GravityMark_1.89_arm64.run'; "
                        "else "
                        "  URL=$(curl -fsSL --connect-timeout 15 'https://gravitymark.tellusim.com/' | grep -m1 -oE 'https://tellusim\\.com/download/GravityMark_[0-9.]+\\.run' || true); "
                        "  [ -n \"$URL\" ] || URL='https://tellusim.com/download/GravityMark_1.89.run'; "
                        "fi; "
                        "RUN_FILE=\"$CACHE_DIR/$(basename \"$URL\")\"; "
                        "echo \"==> 正在下载 $URL（支持断点续传）...\"; "
                        "dl_progress \"$URL\" \"$RUN_FILE\" '正在下载 GravityMark'; "
                        "mkdir -p \"$STG/src\"; "
                        "echo '__SPINEL_PROG__:94:正在解包安装 GravityMark 到 /opt/gravitymark...'; "
                        "echo '==> 正在解包并安装到 /opt/gravitymark ...'; "
                        "sh \"$RUN_FILE\" --tar xf -C \"$STG/src\"; "
                        "rm -rf /opt/gravitymark; "
                        "mkdir -p /opt/gravitymark/bin; "
                        "for f in \"$STG/src/bin\"/*; do "
                        "  [ -e \"$f\" ] || continue; "
                        "  case \"$f\" in "
                        "    *.so) install -m644 \"$f\" /opt/gravitymark/bin/ ;; "
                        "    *) install -m755 \"$f\" /opt/gravitymark/bin/ ;; "
                        "  esac; "
                        "done; "
                        "install -m644 \"$STG/src/browser.zip\" \"$STG/src/data.zip\" /opt/gravitymark/; "
                        "for s in \"$STG/src\"/run_*.sh; do "
                        "  [ -f \"$s\" ] && install -m755 \"$s\" /opt/gravitymark/ || true; "
                        "done; "
                        "printf '#!/bin/sh\\ncd /opt/gravitymark && exec /opt/gravitymark/run_browser.sh \"$@\"\\n' > /usr/bin/gravitymark; "
                        "chmod 755 /usr/bin/gravitymark; "
                        "mkdir -p /usr/share/icons/hicolor/scalable/apps; "
                        "bsdtar -xOf \"$STG/src/browser.zip\" browser/images/gravitymark.svg > /usr/share/icons/hicolor/scalable/apps/gravitymark.svg 2>/dev/null || true; "
                        "printf '[Desktop Entry]\\nType=Application\\nName=GravityMark\\nComment=Cross-platform GPU benchmark and stress test\\nComment[zh_CN]=跨平台 GPU 显卡性能基准与压力测试\\nExec=/usr/bin/gravitymark\\nIcon=gravitymark\\nCategories=Game;Utility;System;\\nTerminal=false\\n' > /usr/share/applications/gravitymark.desktop; "
                        "update-desktop-database /usr/share/applications >/dev/null 2>&1 || true; "
                        "gtk-update-icon-cache -f /usr/share/icons/hicolor >/dev/null 2>&1 || true; "
                        "rm -rf \"$CACHE_DIR\"; "
                        "echo '==> GravityMark 宿主机原生安装完成！'"
                    });
                } else {
                    emit_store_line(state, "==> 正在卸载宿主机原生 GravityMark ...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; "
                        "rm -rf /opt/gravitymark /var/cache/spinelos/gravitymark /usr/bin/gravitymark /usr/share/applications/gravitymark.desktop /usr/share/icons/hicolor/scalable/apps/gravitymark.svg; "
                        "update-desktop-database /usr/share/applications >/dev/null 2>&1 || true; "
                        "gtk-update-icon-cache -f /usr/share/icons/hicolor >/dev/null 2>&1 || true; "
                        "echo '==> 已卸载 GravityMark。'"
                    });
                }
            } else if (app_id == "vscode") {
                if (action == "install") {
                    emit_store_line(state, "==> 将下载微软官方 Visual Studio Code 原生安装包到 /opt/vscode ...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; " + dl_fn +
                        "STG=$(mktemp -d /tmp/spinel-vscode-XXXXXX); "
                        "trap 'rm -rf \"$STG\"' EXIT; "
                        "echo '__SPINEL_PROG__:3:正在安装 VS Code 系统运行依赖...'; "
                        "pacman -Sy --noconfirm --needed alsa-lib gtk3 libnotify libxkbfile libcups nss xdg-utils curl libsecret; "
                        "dl_progress 'https://code.visualstudio.com/sha/download?build=stable&os=linux-x64' \"$STG/vscode.tar.gz\" '正在下载 VS Code'; "
                        "echo '__SPINEL_PROG__:94:正在解包安装 Visual Studio Code...'; "
                        "rm -rf /opt/vscode; "
                        "mkdir -p /opt/vscode; "
                        "bsdtar -xf \"$STG/vscode.tar.gz\" --strip-components=1 -C /opt/vscode; "
                        "ln -sf /opt/vscode/bin/code /usr/bin/code; "
                        "install -Dm644 /opt/vscode/resources/app/resources/linux/code.png /usr/share/icons/hicolor/512x512/apps/vscode.png || true; "
                        "printf '[Desktop Entry]\\nName=Visual Studio Code\\nComment=Code Editing. Redefined.\\nGenericName=Text Editor\\nExec=/usr/bin/code --enable-features=UseOzonePlatform --ozone-platform=wayland %%F\\nIcon=vscode\\nType=Application\\nStartupNotify=false\\nStartupWMClass=Code\\nCategories=TextEditor;Development;IDE;\\nMimeType=application/x-code-workspace;\\n' > /usr/share/applications/vscode.desktop; "
                        "update-desktop-database /usr/share/applications >/dev/null 2>&1 || true; "
                        "gtk-update-icon-cache -f /usr/share/icons/hicolor >/dev/null 2>&1 || true; "
                        "echo '==> Visual Studio Code 原生安装完成！'"
                    });
                } else {
                    emit_store_line(state, "==> 正在卸载 Visual Studio Code ...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; "
                        "rm -rf /opt/vscode /usr/bin/code /usr/share/applications/vscode.desktop /usr/share/icons/hicolor/512x512/apps/vscode.png; "
                        "update-desktop-database /usr/share/applications >/dev/null 2>&1 || true; "
                        "echo '==> 已卸载 Visual Studio Code。'"
                    });
                }
            } else if (app_id == "libreoffice") {
                if (action == "install") {
                    emit_store_line(state, "==> 正在安装 LibreOffice 完整办公套件及中文语言包...");
                    cmd.insert(cmd.end(), {"pacman", "-Sy", "--noconfirm", "--needed", "libreoffice-fresh", "libreoffice-fresh-zh-cn"});
                } else {
                    cmd.insert(cmd.end(), {"sh", "-c", "pacman -Rns --noconfirm libreoffice-fresh-zh-cn libreoffice-fresh || true"});
                }
            } else if (app_id == "printing") {
                if (action == "install") {
                    emit_store_line(state, "==> 正在安装 CUPS 打印底座、PDF 虚拟打印与图形管理工具并启用服务...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; "
                        "pacman -Sy --noconfirm --needed cups cups-pdf system-config-printer; "
                        "systemctl enable --now cups.service; "
                        "echo '==> CUPS 打印服务已安装并启动！'"
                    });
                } else {
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "systemctl disable --now cups.service 2>/dev/null || true; "
                        "pacman -Rns --noconfirm system-config-printer cups-pdf cups || true"
                    });
                }
            } else if (app_id == "docker") {
                std::string uname = current_username_safe();
                if (action == "install") {
                    emit_store_line(state, "==> 正在安装 Docker + Compose v2 并配置服务与用户组...");
                    std::string grp_cmd = uname.empty() ? "" : ("usermod -aG docker '" + uname + "' >/dev/null 2>&1 || true; ");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; "
                        "pacman -Sy --noconfirm --needed docker docker-compose; "
                        + grp_cmd +
                        "systemctl enable --now docker.service; "
                        "echo '==> Docker 安装并启动完成！'"
                    });
                } else {
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "systemctl disable --now docker.service docker.socket 2>/dev/null || true; "
                        "pacman -Rns --noconfirm docker-compose docker || true"
                    });
                }
            } else if (app_id == "nodejs") {
                if (action == "install") {
                    cmd.insert(cmd.end(), {"pacman", "-Sy", "--noconfirm", "--needed", "nodejs", "npm"});
                } else {
                    cmd.insert(cmd.end(), {"sh", "-c", "pacman -Rns --noconfirm npm nodejs || true"});
                }
            } else if (app_id == "python") {
                if (action == "install") {
                    cmd.insert(cmd.end(), {"pacman", "-Sy", "--noconfirm", "--needed", "python", "python-pip", "uv"});
                } else {
                    cmd.insert(cmd.end(), {"sh", "-c", "pacman -Rns --noconfirm uv python-pip || true"});
                }
            } else if (app_id == "ollama") {
                if (action == "install") {
                    bool has_nvidia = false, has_amd = false;
                    detect_gpu_vendors(has_nvidia, has_amd);
                    std::string opkg = select_ollama_pacman_pkg(has_nvidia, has_amd);
                    emit_store_line(state, "==> 检测到显卡环境，自动选择安装包: " + opkg + " ...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; "
                        "pacman -Sy --noconfirm --needed '" + opkg + "'; "
                        "systemctl enable --now ollama.service; "
                        "echo '==> Ollama 已安装并启动服务（可用商店按钮随时启停）。'"
                    });
                } else {
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "systemctl disable --now ollama.service 2>/dev/null || true; "
                        "for p in ollama-cuda ollama-rocm ollama-vulkan ollama; do "
                        "  pacman -Qq \"$p\" >/dev/null 2>&1 && pacman -Rns --noconfirm \"$p\" || true; "
                        "done"
                    });
                }
            } else if (app_id == "vllm") {
                if (action == "install") {
                    emit_store_line(state, "==> 正在使用 uv 为 vLLM 构建独立运行环境 (/opt/spinelos-vllm) ...");
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "set -eu; "
                        "pacman -Sy --noconfirm --needed python uv curl; "
                        "echo '__SPINEL_PROG__:15:正在初始化 /opt/spinelos-vllm 虚拟环境...'; "
                        "rm -rf /opt/spinelos-vllm; "
                        "uv venv /opt/spinelos-vllm; "
                        "echo '__SPINEL_PROG__:35:正在通过 uv 安装 vllm（首次下载较大请耐心等待）...'; "
                        "uv pip install --python /opt/spinelos-vllm/bin/python vllm; "
                        "chmod -R a+rX /opt/spinelos-vllm; "
                        "cat > /usr/bin/spinel-vllm-serve <<'EOF'\n"
                        "#!/bin/sh\n"
                        "MODEL=\"${VLLM_MODEL:-Qwen/Qwen2.5-1.5B-Instruct}\"\n"
                        "PORT=\"${VLLM_PORT:-8000}\"\n"
                        "exec /opt/spinelos-vllm/bin/vllm serve \"$MODEL\" --host 127.0.0.1 --port \"$PORT\" ${VLLM_EXTRA_ARGS:-}\n"
                        "EOF\n"
                        "chmod 755 /usr/bin/spinel-vllm-serve; "
                        "echo '==> vLLM 独立环境安装完成！'"
                    });
                } else {
                    emit_store_line(state, "==> 正在停止并卸载 vLLM 独立环境 ...");
                    std::string out_u;
                    int rc_u = 0;
                    run_argv_capture({"systemctl", "--user", "disable", "--now", "spinel-vllm.service"}, out_u, rc_u);
                    cmd.insert(cmd.end(), {
                        "sh", "-c",
                        "rm -rf /opt/spinelos-vllm /usr/bin/spinel-vllm-serve; "
                        "echo '==> 已卸载 vLLM 独立环境。'"
                    });
                }
            }
            rc = run_streaming_command(state, cmd, pwd_in);
            if (rc == 0 && app_id == "vllm") {
                if (action == "install") {
                    ensure_vllm_user_service();
                } else {
                    std::error_code ec;
                    fs::remove(fs::path(home_dir()) / ".config/systemd/user/spinel-vllm.service", ec);
                }
            }
        } else {
            bool nopass = check_sudo_nopasswd();
            if (!nopass && password.empty()) {
                emit_store_line(state, "[ERROR] 安装系统级 Pacman 软件需要当前用户管理员密码。");
                emit_store_done(state, 2, app_id, action);
                return;
            }
            std::vector<std::string> base_sudo = make_sudo_cmd(nopass);
            std::string pwd_in = nopass ? "" : password;
            if (action == "install" && pkg == "steam") {
                ensure_multilib(base_sudo, pwd_in);
            }
            std::vector<std::string> cmd = base_sudo;
            append_proxy_env_prefix(cmd);
            cmd.push_back("pacman");

            if (action == "install") {
                emit_store_line(state, "==> 正在同步数据库并安装 " + pkg + " ...");
                cmd.insert(cmd.end(), {"-Sy", "--noconfirm", "--needed", pkg});
            } else {
                emit_store_line(state, "==> 正在卸载软件包 " + pkg + " ...");
                cmd.insert(cmd.end(), {"-Rns", "--noconfirm", pkg});
            }
            rc = run_streaming_command(state, cmd, pwd_in);
        }

        if (state->store_cancel_requested.load()) {
            emit_store_line(state, "[已终止] 用户已取消当前操作。");
            emit_store_done(state, 130, app_id, "cancel");
            return;
        }
        emit_store_done(state, rc, app_id, action);
    });
}

void cancel_store_action(AppState *state) {
    if (!state || !state->store_busy.load()) return;
    state->store_cancel_requested.store(true);
    pid_t pgid = state->store_child_pgid.load();
    if (pgid <= 1) {
        emit_store_line(state, "[已取消] 已请求中止当前任务。");
        return;
    }

    emit_store_line(state, "[已取消] 正在终止下载/安装进程...");
    if (kill(-pgid, SIGTERM) != 0 && errno == EPERM && check_sudo_nopasswd()) {
        std::string out;
        int rc = 1;
        run_argv_capture({"sudo", "-n", "kill", "-TERM", "--", "-" + std::to_string(pgid)}, out, rc);
    }

    std::thread([state, pgid]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        if (!state->shutting_down.load() && state->store_child_pgid.load() == pgid) {
            if (kill(-pgid, SIGKILL) != 0 && errno == EPERM && check_sudo_nopasswd()) {
                std::string out;
                int rc = 1;
                run_argv_capture({"sudo", "-n", "kill", "-KILL", "--", "-" + std::to_string(pgid)}, out, rc);
            }
        }
    }).detach();
}

void launch_store_app(AppState *state, const std::string &app_id) {
    const StoreAppEntry *entry = find_catalog_entry(app_id);
    if (!entry) return;
    if (std::string(entry->backend) == "link") {
        if (is_valid_https_url(entry->pkg)) {
            if (!open_url_in_browser(entry->pkg, "StoreLink:" + app_id)) {
                std::string msg = "未检测到已安装浏览器，请先在应用中心安装 Chrome、Chromium 或 Firefox。";
                emit_store_line(state, "[ERROR] " + msg);
                notify_app_launch_failure(entry->name, msg);
                if (state && state->store_view) {
                    eval_js(state->store_view,
                            "window.__spinelStoreFeedback && window.__spinelStoreFeedback(false,\"" +
                                json_escape(msg) + "\");");
                }
            }
        }
        return;
    }
    if (std::string(entry->backend) == "flatpak") {
        spawn_argv_logged({"flatpak", "run", entry->pkg}, "Store:" + app_id);
        return;
    }
    if (app_id == "steam") {
        launch_steam_client(state);
    } else if (app_id == "wine") {
        spawn_argv_logged({entry->launch_cmd, "--windows-apps"}, "Store:wine");
    } else if (app_id == "chromium" || app_id == "firefox" || app_id == "chrome") {
        launch_browser_command({entry->pkg}, "Store:" + app_id);
    } else if (app_id == "vscode") {
        spawn_argv_logged({"code", "--enable-features=UseOzonePlatform", "--ozone-platform=wayland"}, "Store:vscode");
    } else if (app_id == "libreoffice") {
        spawn_argv_logged({"libreoffice"}, "Store:libreoffice");
    } else if (app_id == "printing") {
        spawn_argv_logged({"system-config-printer"}, "Store:printing");
    } else if (app_id == "ollama") {
        const char *cmd =
            "ollama list 2>/dev/null || echo '提示：请先在应用商店点击开启 Ollama 服务'; "
            "echo ''; echo '常用命令: ollama run qwen2.5:7b'; exec bash";
        if (has_executable("spinel-terminal")) {
            spawn_argv_logged({"spinel-terminal", "-T", "Ollama 本地模型终端", "--", "sh", "-c", cmd}, "Store:ollama");
        }
    } else if (app_id == "vllm") {
        const char *cmd =
            "systemctl --user status spinel-vllm.service --no-pager || true; "
            "echo ''; echo '配置文件: ~/.config/spinelos/vllm.conf'; exec bash";
        if (has_executable("spinel-terminal")) {
            spawn_argv_logged({"spinel-terminal", "-T", "vLLM 推理服务状态", "--", "sh", "-c", cmd}, "Store:vllm");
        }
    } else if (app_id == "docker" || app_id == "podman" || app_id == "git" || app_id == "nodejs" || app_id == "python" || app_id == "llamacpp") {
        std::string info = std::string(entry->name) + " 已就绪（命令: " + entry->launch_cmd + "）";
        std::string sh_cmd = "printf '%s\\n\\n' '" + info + "'; " + std::string(entry->launch_cmd) + " --version 2>/dev/null || true; exec bash";
        if (has_executable("spinel-terminal")) {
            spawn_argv_logged({"spinel-terminal", "-T", entry->name, "--", "sh", "-c", sh_cmd}, "Store:" + app_id);
        }
    } else if (app_id == "heroic") {
        spawn_argv_logged({has_executable("heroic") ? "heroic" : "/opt/Heroic/heroic"}, "Store:heroic");
    } else if (app_id == "gravitymark") {
        spawn_argv_logged({has_executable("gravitymark") ? "gravitymark" : "/opt/gravitymark/run_browser.sh"}, "Store:gravitymark");
    } else if (app_id == "glmark2") {
        const char *cmd =
            "if command -v glmark2-wayland >/dev/null 2>&1 && [ -n \"${WAYLAND_DISPLAY:-}\" ]; then "
            "glmark2-wayland; else glmark2; fi; "
            "printf '\\n测试完成，按 Enter 键关闭窗口...'; read -r _";
        if (has_executable("spinel-terminal")) {
            spawn_argv_logged({"spinel-terminal", "-T", "glmark2 显卡基准测试", "--", "sh", "-c", cmd}, "Store:glmark2");
        } else {
            spawn_argv_logged({"sh", "-c", cmd}, "Store:glmark2");
        }
    } else if (app_id == "obs") {
        spawn_argv_logged({"obs"}, "Store:" + app_id);
    } else {
        spawn_argv_logged({entry->launch_cmd && entry->launch_cmd[0] ? entry->launch_cmd : entry->pkg}, "Store:" + app_id);
    }
}
