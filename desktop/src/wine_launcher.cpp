#include "wine_launcher.hpp"
#include "common.hpp"
#include "temp_memory.hpp"

#include <glib/gstdio.h>

#include <fcntl.h>

#include <algorithm>
#include <sstream>

namespace fs = std::filesystem;

namespace {

GSubprocess *spawn_wine(const std::vector<std::string> &args, const fs::path &working_dir,
                       bool quiet, GError **error) {
    g_autofree char *runner = g_find_program_in_path("wine");
    if (!runner) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "请先在应用商店安装 Wine。");
        return nullptr;
    }
    g_autofree char *spinel_wine = g_find_program_in_path("spinel-wine");
    if (!spinel_wine) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "缺少 SpinelOS 的 Wine 启动组件。");
        return nullptr;
    }

    const fs::path prefix = spinel_wine_prefix();
    std::error_code ec;
    fs::create_directories(prefix.parent_path(), ec);
    if (ec) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "无法创建 Wine 数据目录：%s", ec.message().c_str());
        return nullptr;
    }

    const auto flags = quiet ?
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE) :
        G_SUBPROCESS_FLAGS_STDERR_MERGE;
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(flags);
    if (quiet) {
        g_subprocess_launcher_setenv(launcher, "WINEDEBUG", "-all", TRUE);
    } else {
        append_app_log("[Wine] " + args.front());
        const fs::path log_path = fs::path(spinel_state_dir()) / "apps.log";
        int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd >= 0) g_subprocess_launcher_take_stdout_fd(launcher, fd);
    }
    if (!working_dir.empty()) g_subprocess_launcher_set_cwd(launcher, working_dir.c_str());
    g_subprocess_launcher_setenv(launcher, "WINEPREFIX", prefix.c_str(), TRUE);

    std::vector<const char *> argv{spinel_wine, runner};
    for (const auto &arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    return g_subprocess_launcher_spawnv(launcher, argv.data(), error);
}

bool path_within(const fs::path &path, const fs::path &dir) {
    const fs::path relative = path.lexically_relative(dir);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

// Wine writes two levels of escaping: desktop-entry string syntax, then Exec quoting.
bool read_wine_shortcut(const fs::path &file, const fs::path &drive, WineShortcut &shortcut) {
    g_autoptr(GKeyFile) entry = g_key_file_new();
    if (!g_key_file_load_from_file(entry, file.c_str(), G_KEY_FILE_NONE, nullptr)) return false;
    constexpr auto group = G_KEY_FILE_DESKTOP_GROUP;
    g_autofree char *type = g_key_file_get_string(entry, group, "Type", nullptr);
    if (g_strcmp0(type, "Application") != 0 ||
        g_key_file_get_boolean(entry, group, "NoDisplay", nullptr) ||
        g_key_file_get_boolean(entry, group, "Hidden", nullptr)) return false;
    g_autofree char *name = g_key_file_get_locale_string(entry, group, "Name", nullptr, nullptr);
    g_autofree char *exec = g_key_file_get_string(entry, group, "Exec", nullptr);
    g_autofree char *wm_class = g_key_file_get_string(entry, group, "StartupWMClass", nullptr);
    if (!name || !*name || !exec) return false;
    if (wm_class && *wm_class && g_ascii_strcasecmp(fs::path(wm_class).extension().c_str(), ".exe") != 0) {
        return false;
    }

    int argc = 0;
    g_auto(GStrv) argv = nullptr;
    if (!g_shell_parse_argv(exec, &argc, &argv, nullptr) || argc != 4 ||
        std::string_view(argv[0]) != "env" || argv[1] != "WINEPREFIX=" + spinel_wine_prefix().string() ||
        std::string_view(argv[2]) != "wine" || g_ascii_strncasecmp(argv[3], "C:\\", 3) != 0) return false;

    std::string relative = argv[3] + 3;
    std::replace(relative.begin(), relative.end(), '\\', '/');
    std::error_code ec;
    const fs::path link = fs::canonical(drive / relative, ec);
    if (ec || !path_within(link, drive) || g_ascii_strcasecmp(link.extension().c_str(), ".lnk") != 0 ||
        !fs::is_regular_file(link, ec)) return false;

    g_autofree char *workdir = g_key_file_get_string(entry, group, "Path", nullptr);
    if (workdir && *workdir) {
        const fs::path dir = fs::canonical(workdir, ec);
        if (!ec && path_within(dir, drive) && fs::is_directory(dir, ec)) shortcut.working_dir = dir;
    }
    g_autofree char *icon = g_key_file_get_string(entry, group, "Icon", nullptr);
    if (icon) shortcut.icon = icon;
    shortcut.name = name;
    shortcut.link = link;
    return true;
}

std::string match_name(const std::string &name) {
    g_autofree char *folded = g_utf8_casefold(name.c_str(), -1);
    std::string result;
    for (const char *p = folded; *p; p = g_utf8_next_char(p)) {
        if (g_unichar_isalnum(g_utf8_get_char(p)) || *p == '+' || *p == '#') {
            result.append(p, g_utf8_next_char(p) - p);
        }
    }
    return result;
}

std::string registry_display_name(const std::string &value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        g_autofree char *name = g_strcompress(value.substr(1, value.size() - 2).c_str());
        return trim_copy(name);
    }
    if (!value.starts_with("hex(2):") && !value.starts_with("hex(1):")) return {};
    std::string bytes;
    std::istringstream pieces(value.substr(7));
    for (std::string piece; std::getline(pieces, piece, ',');) {
        piece = trim_copy(piece);
        if (piece.size() != 2 || !g_ascii_isxdigit(piece[0]) || !g_ascii_isxdigit(piece[1])) return {};
        bytes += static_cast<char>((g_ascii_xdigit_value(piece[0]) << 4) | g_ascii_xdigit_value(piece[1]));
    }
    if (bytes.size() % 2 || bytes.size() < 2) return {};
    if (bytes.ends_with(std::string_view("\0\0", 2))) bytes.resize(bytes.size() - 2);
    g_autofree char *name = g_convert(bytes.data(), bytes.size(), "UTF-8", "UTF-16LE", nullptr, nullptr, nullptr);
    return name ? trim_copy(name) : "";
}

} // namespace

bool is_windows_executable(const fs::path &path) {
    std::error_code ec;
    return g_ascii_strcasecmp(path.extension().c_str(), ".exe") == 0 &&
           fs::is_regular_file(path, ec);
}

bool wine_is_available() {
    g_autofree char *runner = g_find_program_in_path("wine");
    return runner != nullptr;
}

GSubprocess *launch_windows_executable(const fs::path &path, GError **error) {
    if (!is_windows_executable(path)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "请选择一个 Windows .exe 文件。");
        return nullptr;
    }

    std::error_code ec;
    const fs::path executable = fs::canonical(path, ec);
    if (ec) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "无法读取目标路径：%s", ec.message().c_str());
        return nullptr;
    }
    return spawn_wine({executable.string()}, executable.parent_path(), false, error);
}

fs::path spinel_wine_prefix() {
    return fs::path(g_get_user_data_dir()) / "spinelos" / "wine-prefix";
}

std::vector<WineShortcut> list_wine_shortcuts() {
    std::vector<WineShortcut> shortcuts;
    std::error_code ec;
    const fs::path drive = fs::canonical(spinel_wine_prefix() / "drive_c", ec);
    if (ec) return shortcuts;
    const fs::path entries = fs::path(g_get_user_data_dir()) / "applications" / "wine";
    for (fs::recursive_directory_iterator it(entries, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".desktop") continue;
        std::error_code stat_error;
        if (!it->is_regular_file(stat_error)) continue;
        WineShortcut app;
        if (!read_wine_shortcut(it->path(), drive, app)) continue;
        const fs::path relative = it->path().lexically_relative(entries);
        app.id = "wine:" + relative.string();
        if (relative.parent_path() != "Programs") app.folder = relative.parent_path().filename().string();
        shortcuts.push_back(std::move(app));
    }
    std::ranges::sort(shortcuts, {}, &WineShortcut::id);
    return shortcuts;
}

GSubprocess *launch_wine_shortcut(const WineShortcut &shortcut, GError **error) {
    return spawn_wine({"start", "/unix", shortcut.link.string()}, shortcut.working_dir, false, error);
}

std::vector<WineInstalledApp> parse_wine_uninstall_registry(std::string_view text, std::string_view root) {
    std::vector<WineInstalledApp> apps;
    const std::string prefix = "[" + std::string(root) + "\\";
    WineInstalledApp current;
    bool system_component = false;
    bool has_uninstaller = false;
    auto add_current = [&] {
        if (!current.key.empty() && !current.name.empty() && !system_component && has_uninstaller) {
            apps.push_back(std::move(current));
        }
    };
    std::istringstream lines{std::string(text)};
    for (std::string line; std::getline(lines, line);) {
        line = trim_copy(line);
        if (line.starts_with("[")) {
            add_current();
            current = {};
            system_component = false;
            has_uninstaller = false;
            if (line.ends_with("]") && g_ascii_strncasecmp(line.c_str(), prefix.c_str(), prefix.size()) == 0) {
                std::string key = line.substr(prefix.size(), line.size() - prefix.size() - 1);
                if (key.find('\\') == std::string::npos) current.key = std::move(key);
            }
        } else if (!current.key.empty()) {
            while (line.ends_with("\\")) {
                std::string next;
                if (!std::getline(lines, next)) break;
                line.pop_back();
                line += trim_copy(next);
            }
            const auto split = line.find('=');
            if (split == std::string::npos) continue;
            const std::string name = line.substr(0, split);
            const std::string value = line.substr(split + 1);
            if (g_ascii_strcasecmp(name.c_str(), "\"DisplayName\"") == 0) {
                current.name = registry_display_name(value);
            } else if (g_ascii_strcasecmp(name.c_str(), "\"SystemComponent\"") == 0) {
                system_component = value == "dword:00000001";
            } else if (g_ascii_strcasecmp(name.c_str(), "\"UninstallString\"") == 0 ||
                       (g_ascii_strcasecmp(name.c_str(), "\"WindowsInstaller\"") == 0 && value == "dword:00000001")) {
                has_uninstaller = true;
            }
        }
    }
    add_current();
    return apps;
}

bool list_wine_installed_apps(std::vector<WineInstalledApp> &apps, GError **error) {
    // Wine's uninstaller --list uses an en-US code page, dropping some Unicode names.
    constexpr const char *roots[] = {
        R"(HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Uninstall)",
        R"(HKEY_LOCAL_MACHINE\Software\Wow6432Node\Microsoft\Windows\CurrentVersion\Uninstall)",
        R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Uninstall)"
    };
    struct ExportDirectory {
        fs::path path;
        int lock = -1;
        ~ExportDirectory() {
            g_autofd int held_lock = lock;
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };
    g_autofree char *temporary_dir = g_dir_make_tmp("spinel-wine-apps.XXXXXX", error);
    if (!temporary_dir) return false;
    const ExportDirectory directory{temporary_dir, register_temp_directory(temporary_dir)};
    for (size_t index = 0; index < std::size(roots); ++index) {
        const fs::path file = directory.path / (std::to_string(index) + ".reg");
        g_autoptr(GSubprocess) process = spawn_wine({"regedit", "/E", file.string(), roots[index]}, {}, true, error);
        if (!process || !g_subprocess_wait_check(process, nullptr, error)) return false;
        g_autofree char *contents = nullptr;
        gsize length = 0;
        g_autoptr(GError) read_error = nullptr;
        if (!g_file_get_contents(file.c_str(), &contents, &length, &read_error)) {
            // An absent uninstall branch produces no export.
            if (g_error_matches(read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) continue;
            g_propagate_error(error, g_steal_pointer(&read_error));
            return false;
        }
        g_autofree char *utf8 = g_convert(contents, length, "UTF-8", "UTF-16", nullptr, nullptr, error);
        if (!utf8) return false;
        auto branch = parse_wine_uninstall_registry(utf8, roots[index]);
        std::ranges::move(branch, std::back_inserter(apps));
    }
    std::ranges::sort(apps, [](const auto &left, const auto &right) {
        return g_utf8_collate(left.name.c_str(), right.name.c_str()) < 0;
    });
    return true;
}

const WineShortcut *find_wine_app_shortcut(const WineInstalledApp &app, const std::vector<WineShortcut> &shortcuts) {
    const std::string app_name = match_name(app.name);
    const std::string key = g_utf8_validate(app.key.c_str(), -1, nullptr) ? match_name(app.key) : "";
    int best_score = 0;
    const WineShortcut *best = nullptr;
    for (const auto &shortcut : shortcuts) {
        const std::string name = match_name(shortcut.name);
        const std::string folder = match_name(shortcut.folder);
        if (name.empty() || name.starts_with("uninstall") || name.starts_with("卸载") ||
            name.ends_with("uninstall") || name.ends_with("卸载")) continue;
        int score = 0;
        if (name == app_name || (!key.empty() && name == key)) score = 3;
        else if (!folder.empty() && (folder == key || folder == app_name) && name.starts_with(folder)) score = 2;
        else if (name.size() >= 3 && app_name.starts_with(name)) score = 1;
        if (!score || score < best_score) continue;
        best = score > best_score ? &shortcut : nullptr;
        best_score = score;
    }
    return best;
}

GSubprocess *uninstall_wine_app(const std::string &key, GError **error) {
    return spawn_wine({"uninstaller", "--remove", key}, {}, false, error);
}
