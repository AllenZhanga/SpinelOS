#include "files_ops.hpp"
#include "common.hpp"

#include <cctype>
#include <iomanip>
#include <ios>
#include <sstream>

namespace fs = std::filesystem;

namespace spinel::files {

std::string to_lower_copy(std::string s) {
    for (char &ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string format_size(std::uintmax_t bytes) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int idx = 0;
    while (v >= 1024.0 && idx < 4) {
        v /= 1024.0;
        ++idx;
    }
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(idx == 0 ? 0 : 1);
    oss << v << " " << units[idx];
    return oss.str();
}

std::string format_time(std::time_t t) {
    if (t <= 0) return "-";
    std::tm tm_val{};
    localtime_r(&t, &tm_val);
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_val);
    return buf;
}

std::string format_perms(fs::perms p) {
    auto bit = [&](fs::perms f, char c) { return (p & f) != fs::perms::none ? c : '-'; };
    std::string s;
    s += bit(fs::perms::owner_read, 'r');
    s += bit(fs::perms::owner_write, 'w');
    s += bit(fs::perms::owner_exec, 'x');
    s += bit(fs::perms::group_read, 'r');
    s += bit(fs::perms::group_write, 'w');
    s += bit(fs::perms::group_exec, 'x');
    s += bit(fs::perms::others_read, 'r');
    s += bit(fs::perms::others_write, 'w');
    s += bit(fs::perms::others_exec, 'x');
    return s;
}

bool is_archive_file(const fs::path &p) {
    std::string name = to_lower_copy(p.filename().string());
    static const char *exts[] = {
        ".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst", ".tar.lz4",
        ".tgz", ".txz", ".tbz2", ".tzst", ".tlz", ".tar",
        ".zip", ".7z", ".rar", ".iso", ".deb", ".cab", ".jar", ".whl", ".cpio"
    };
    for (const char *ext : exts) {
        std::string e(ext);
        if (name.size() > e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0) {
            return true;
        }
    }
    return false;
}

std::string strip_archive_ext(const std::string &fname) {
    std::string lower = to_lower_copy(fname);
    for (const char *multi : {".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst", ".tar.lz4"}) {
        std::string e(multi);
        if (lower.size() > e.size() && lower.compare(lower.size() - e.size(), e.size(), e) == 0) {
            return fname.substr(0, fname.size() - e.size());
        }
    }
    auto dot = fname.rfind('.');
    if (dot != std::string::npos && dot > 0) return fname.substr(0, dot);
    return fname + "_extracted";
}

bool is_script_or_appimage(const fs::path &p) {
    std::string ext = to_lower_copy(p.extension().string());
    return ext == ".sh" || ext == ".appimage" || ext == ".run" || ext == ".bin";
}

std::string sanitize_app_slug(const std::string &raw) {
    std::string out;
    for (char ch : to_lower_copy(raw)) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-') out += ch;
        else if (c == '_' || c == '.' || c == ' ') {
            if (!out.empty() && out.back() != '-') out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) out = "custom-app";
    return out;
}

std::string guess_friendly_app_name(const fs::path &target) {
    std::string name = target.filename().string();
    std::string lower = to_lower_copy(name);
    for (const char *sfx : {".appimage", ".x86_64", ".amd64", ".sh", ".run", ".bin"}) {
        std::string s(sfx);
        if (lower.size() > s.size() && lower.compare(lower.size() - s.size(), s.size(), s) == 0) {
            name = name.substr(0, name.size() - s.size());
            lower = to_lower_copy(name);
        }
    }
    if (name.empty()) name = "Custom App";
    if (std::islower(static_cast<unsigned char>(name[0]))) {
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    }
    return name;
}

std::string sanitize_url_scheme(const std::string &raw) {
    std::string s = trim_copy(to_lower_copy(raw));
    auto colon = s.find("://");
    if (colon != std::string::npos) s = s.substr(0, colon);
    std::string out;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '+' || c == '-' || c == '.') out += ch;
    }
    return out;
}

}  // namespace spinel::files
