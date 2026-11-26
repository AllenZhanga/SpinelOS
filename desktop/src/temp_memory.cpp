#include "temp_memory.hpp"

#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <linux/magic.h>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr char cleanup_marker[] = ".spinelos-temp-cleanup";

bool mark_directory(int fd) {
    g_autofd int marker = openat(fd, cleanup_marker, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    return marker >= 0;
}

bool is_marked_private_directory(int fd, dev_t device) {
    struct stat directory{}, marker{};
    return fstat(fd, &directory) == 0 && directory.st_dev == device &&
        directory.st_uid == getuid() && (directory.st_mode & 0777) == 0700 &&
        fstatat(fd, cleanup_marker, &marker, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(marker.st_mode) && marker.st_uid == getuid() && (marker.st_mode & 0777) == 0600 &&
        marker.st_size == 0;
}

} // namespace

int register_temp_directory(const fs::path &directory) {
    g_autofd int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0 && mark_directory(fd)) return g_steal_fd(&fd);
    return -1;
}

std::optional<std::uint64_t> tmpfs_used_bytes(const fs::path &root) {
    struct statfs info{};
    if (statfs(root.c_str(), &info) != 0 || info.f_type != TMPFS_MAGIC) return std::nullopt;
    return static_cast<std::uint64_t>(info.f_blocks - info.f_bfree) * info.f_bsize;
}

TempMemoryCleanup clean_temp_memory(const fs::path &root) {
    TempMemoryCleanup result;
    const auto before = tmpfs_used_bytes(root);
    if (!before) return result;
    struct stat mount_root{};
    std::ifstream mount_info("/proc/self/mountinfo");
    if (!mount_info || lstat(root.c_str(), &mount_root) != 0 || !S_ISDIR(mount_root.st_mode)) {
        result.incomplete = true;
        return result;
    }
    std::vector<std::string> submounts;
    const std::string root_prefix = root.string() + '/';
    for (std::string line; std::getline(mount_info, line);) {
        std::istringstream fields(line);
        std::string mountpoint;
        // The fifth field is the mount point, including bind mounts on the same filesystem.
        for (int field = 0; field < 5 && fields >> mountpoint; ++field) {}
        if (mountpoint.starts_with(root_prefix)) submounts.push_back(std::move(mountpoint));
    }
    if (mount_info.bad()) {
        result.incomplete = true;
        return result;
    }

    std::error_code ec;
    for (fs::directory_iterator dir(root, ec), end; !ec && dir != end; dir.increment(ec)) {
        const fs::path &path = dir->path();
        const std::string name = path.filename().string();
        if (!name.starts_with("spinel-") || !std::ranges::all_of(name, [](char c) {
                return g_ascii_isalnum(c) || c == '-' || c == '_' || c == '.';
            })) continue;
        g_autofd int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0 || !is_marked_private_directory(fd, mount_root.st_dev) || flock(fd, LOCK_EX | LOCK_NB) != 0) continue;
        if (!is_marked_private_directory(fd, mount_root.st_dev)) continue;
        const std::string prefix = path.string() + '/';
        if (std::ranges::any_of(submounts, [&](const std::string &mount) {
                return mount == path.native() || mount.starts_with(prefix);
            })) {
            result.incomplete = true;
            continue;
        }

        g_autoptr(GError) remove_error = nullptr;
        g_autoptr(GSubprocess) remove = g_subprocess_new(G_SUBPROCESS_FLAGS_STDERR_SILENCE, &remove_error,
            "/usr/bin/rm", "-r", "--one-file-system", "--preserve-root=all", "--", path.c_str(), nullptr);
        if (remove && g_subprocess_wait_check(remove, nullptr, &remove_error)) {
            ++result.directories_removed;
        } else {
            // Retain the opt-in marker if only part of the directory could be removed.
            mark_directory(fd);
            result.incomplete = true;
        }
    }
    result.incomplete |= static_cast<bool>(ec);
    const auto after = tmpfs_used_bytes(root);
    if (after) result.released_bytes = *before > *after ? *before - *after : 0;
    return result;
}
