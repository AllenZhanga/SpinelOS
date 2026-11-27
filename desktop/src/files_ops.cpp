#include "files_ops.hpp"
#include "common.hpp"

#include <cctype>
#include <fstream>
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

bool is_image_file(const fs::path &p) {
    std::string ext = to_lower_copy(p.extension().string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" ||
           ext == ".bmp" || ext == ".gif" || ext == ".svg" || ext == ".ico" ||
           ext == ".avif" || ext == ".tif" || ext == ".tiff";
}

bool is_text_previewable_file(const fs::path &p) {
    std::string name = to_lower_copy(p.filename().string());
    static const char *names[] = {
        "readme", "license", "copying", "changelog", "authors", "makefile",
        "cmakelists.txt", "dockerfile", "pkgbuild", "meson.build", ".gitignore",
        ".bashrc", ".zshrc", ".profile", ".bash_profile"
    };
    for (const char *n : names) {
        if (name == n) return true;
    }
    std::string ext = to_lower_copy(p.extension().string());
    static const char *exts[] = {
        ".txt", ".md", ".markdown", ".json", ".jsonc", ".yaml", ".yml", ".toml",
        ".ini", ".conf", ".cfg", ".log", ".csv", ".tsv", ".xml", ".html", ".htm",
        ".css", ".scss", ".js", ".mjs", ".cjs", ".ts", ".tsx", ".jsx", ".py",
        ".sh", ".bash", ".zsh", ".fish", ".c", ".cc", ".cpp", ".cxx", ".h",
        ".hh", ".hpp", ".rs", ".go", ".java", ".kt", ".lua", ".sql", ".cmake",
        ".desktop", ".service", ".timer", ".rules", ".env", ".diff", ".patch",
        ".trashinfo"
    };
    for (const char *e : exts) {
        if (ext == e) return true;
    }
    return false;
}

std::string read_text_preview(const fs::path &p,
                              std::size_t max_bytes,
                              bool &out_truncated,
                              bool &out_is_binary) {
    out_truncated = false;
    out_is_binary = false;
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs.is_open()) return "";

    std::string raw(max_bytes + 1, '\0');
    ifs.read(&raw[0], static_cast<std::streamsize>(max_bytes + 1));
    std::size_t n = static_cast<std::size_t>(ifs.gcount());
    if (n > max_bytes) {
        out_truncated = true;
        n = max_bytes;
    }
    raw.resize(n);

    std::size_t ctrl_bad = 0;
    for (unsigned char ch : raw) {
        if (ch == 0) {
            out_is_binary = true;
            return "";
        }
        if (ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') {
            ++ctrl_bad;
        }
    }
    if (!raw.empty() && ctrl_bad * 10 > raw.size()) {
        out_is_binary = true;
        return "";
    }

    // Drop any incomplete trailing multi-byte UTF-8 sequence when truncated.
    if (out_truncated && !raw.empty()) {
        std::size_t tail = raw.size();
        while (tail > 0 && (static_cast<unsigned char>(raw[tail - 1]) & 0xC0) == 0x80) {
            --tail;
        }
        if (tail > 0 && (static_cast<unsigned char>(raw[tail - 1]) & 0xC0) == 0xC0) {
            unsigned char lead = static_cast<unsigned char>(raw[tail - 1]);
            std::size_t expected = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : 2;
            if (raw.size() - (tail - 1) < expected) {
                raw.resize(tail - 1);
            }
        }
    }

    // Sanitize invalid UTF-8 sequences so GTK labels/text buffers stay valid.
    std::string out;
    out.reserve(raw.size());
    std::size_t i = 0;
    while (i < raw.size()) {
        unsigned char b = static_cast<unsigned char>(raw[i]);
        if (b < 0x80) {
            out.push_back(static_cast<char>(b));
            ++i;
            continue;
        }
        std::size_t len = 0;
        if ((b & 0xE0) == 0xC0 && b >= 0xC2) len = 2;
        else if ((b & 0xF0) == 0xE0) len = 3;
        else if ((b & 0xF8) == 0xF0 && b <= 0xF4) len = 4;
        bool valid = (len > 0 && i + len <= raw.size());
        for (std::size_t j = 1; valid && j < len; ++j) {
            if ((static_cast<unsigned char>(raw[i + j]) & 0xC0) != 0x80) valid = false;
        }
        if (valid) {
            out.append(raw, i, len);
            i += len;
        } else {
            out.push_back('?');
            ++i;
        }
    }
    return out;
}

std::string encode_drag_paths(const std::vector<fs::path> &paths) {
    if (paths.empty()) return "";
    std::string out = "spinel/paths\n";
    for (const auto &p : paths) {
        if (p.empty()) continue;
        std::string s = p.string();
        if (s.find('\n') != std::string::npos || s.find('\r') != std::string::npos) continue;
        out += s;
        out.push_back('\n');
    }
    return out;
}

std::vector<fs::path> decode_drag_paths(const std::string &payload) {
    std::vector<fs::path> out;
    auto lines = split_lines(payload);
    if (lines.empty()) return out;
    std::size_t start = 0;
    if (trim_copy(lines[0]) == "spinel/paths") {
        start = 1;
    }
    for (std::size_t i = start; i < lines.size(); ++i) {
        std::string line = trim_copy(lines[i]);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("file://", 0) == 0) {
            std::string decoded;
            std::string raw = line.substr(7);
            for (std::size_t j = 0; j < raw.size(); ++j) {
                if (raw[j] == '%' && j + 2 < raw.size() &&
                    std::isxdigit(static_cast<unsigned char>(raw[j + 1])) &&
                    std::isxdigit(static_cast<unsigned char>(raw[j + 2]))) {
                    int val = std::stoi(raw.substr(j + 1, 2), nullptr, 16);
                    decoded.push_back(static_cast<char>(val));
                    j += 2;
                } else {
                    decoded.push_back(raw[j]);
                }
            }
            line = std::move(decoded);
        }
        fs::path p(line);
        if (p.is_absolute()) out.push_back(p);
    }
    return out;
}

bool is_same_or_subpath(const fs::path &candidate, const fs::path &base) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(candidate, ec);
    if (ec) c = candidate.lexically_normal();
    fs::path b = fs::weakly_canonical(base, ec);
    if (ec) b = base.lexically_normal();
    std::string cs = c.string();
    std::string bs = b.string();
    if (cs == bs) return true;
    if (bs == "/") return !cs.empty() && cs.front() == '/';
    return cs.size() > bs.size() && cs.compare(0, bs.size(), bs) == 0 && cs[bs.size()] == '/';
}

bool move_or_copy_paths_to_dir(const std::vector<fs::path> &sources,
                               const fs::path &target_dir,
                               bool copy_mode,
                               std::vector<fs::path> &out_results,
                               std::string &out_error) {
    out_results.clear();
    out_error.clear();
    std::error_code ec;
    if (target_dir.empty() || !fs::is_directory(target_dir, ec)) {
        out_error = "目标文件夹不存在";
        return false;
    }

    for (const auto &src : sources) {
        if (!fs::exists(src, ec)) continue;
        fs::path parent = fs::weakly_canonical(src.parent_path(), ec);
        if (ec) {
            ec.clear();
            parent = src.parent_path().lexically_normal();
        }
        fs::path target_norm = fs::weakly_canonical(target_dir, ec);
        if (ec) {
            ec.clear();
            target_norm = target_dir.lexically_normal();
        }
        if (is_same_or_subpath(target_norm, src)) {
            out_error = "不能将文件夹移动或复制到其自身或子目录中";
            return false;
        }
        if (!copy_mode && parent == target_norm) {
            continue;
        }

        std::string stem = src.stem().string();
        std::string ext = src.extension().string();
        if (fs::is_directory(src, ec)) {
            stem = src.filename().string();
            ext.clear();
        }
        fs::path dst = target_dir / src.filename();
        int idx = 1;
        while (fs::exists(dst, ec) && idx < 200) {
            std::string suffix = (idx == 1) ? " (副本)" : (" (副本 " + std::to_string(idx) + ")");
            dst = target_dir / (stem + suffix + ext);
            ++idx;
        }

        if (copy_mode) {
            fs::copy(src, dst, fs::copy_options::recursive, ec);
        } else {
            fs::rename(src, dst, ec);
            if (ec) {
                ec.clear();
                fs::copy(src, dst, fs::copy_options::recursive, ec);
                if (!ec) fs::remove_all(src, ec);
            }
        }
        if (ec) {
            out_error = ec.message();
            return false;
        }
        out_results.push_back(dst);
    }
    return true;
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
