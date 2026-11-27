#include "system_control.hpp"
#include "sys_internal.hpp"
#include "common.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using sys_internal::post_js_to_main;

namespace {

struct BlockDevEntry {
    std::string name;
    std::string path;
    std::string parent_disk;
    std::string size_str;
    std::string tran;
    bool removable = false;
    std::string fstype;
    std::string label;
    std::string uuid;
    std::string mountpoint;
    bool mounted_ro = false;
    std::string fstab_mount;
    bool fstab_ro = false;
    bool is_system = false;
};

struct UsbDevEntry {
    std::string bus_id;
    std::string vid_pid;
    std::string name;
    std::string speed_mbps;
    std::string driver;
};

struct InputDevEntry {
    std::string name;
    std::string bus;
    std::string kind;
    std::string handlers;
};

struct AudioDevEntry {
    std::string node;
    std::string label;
    std::string kind;  // "sink" | "source"
    bool enabled = true;
    bool is_default = false;
};

std::string to_lower_copy(std::string s) {
    for (char &ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool contains_any_ci(const std::string &haystack, const std::vector<const char *> &needles) {
    std::string lower = to_lower_copy(haystack);
    for (const char *needle : needles) {
        if (lower.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::string read_sys_attr(const fs::path &dir, const char *file) {
    return trim_copy(read_first_line((dir / file).string(), ""));
}

std::string format_bytes_human(unsigned long long bytes) {
    if (bytes == 0) return "0 B";
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int idx = 0;
    while (v >= 1000.0 && idx < 4) {
        v /= 1000.0;
        ++idx;
    }
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(idx >= 3 ? 1 : 0);
    oss << v << " " << units[idx];
    return oss.str();
}

std::string decode_mount_octal(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() &&
            s[i + 1] >= '0' && s[i + 1] <= '7' &&
            s[i + 2] >= '0' && s[i + 2] <= '7' &&
            s[i + 3] >= '0' && s[i + 3] <= '7') {
            int val = ((s[i + 1] - '0') << 6) | ((s[i + 2] - '0') << 3) | (s[i + 3] - '0');
            out.push_back(static_cast<char>(val));
            i += 3;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::map<std::string, std::string> parse_lsblk_kv_line(const std::string &line) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= n) break;
        size_t eq = line.find('=', i);
        if (eq == std::string::npos) break;
        std::string key = line.substr(i, eq - i);
        i = eq + 1;
        std::string raw;
        if (i < n && line[i] == '"') {
            ++i;
            while (i < n && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < n) {
                    raw.push_back(line[i++]);
                    raw.push_back(line[i++]);
                } else {
                    raw.push_back(line[i++]);
                }
            }
            if (i < n && line[i] == '"') ++i;
        } else {
            while (i < n && !std::isspace(static_cast<unsigned char>(line[i]))) {
                raw.push_back(line[i++]);
            }
        }
        out[std::move(key)] = unescape_lsblk_value(raw);
    }
    return out;
}

struct FstabEntryInfo {
    std::string mountpoint;
    bool read_only = false;
};

void load_fstab_map(std::map<std::string, FstabEntryInfo> &by_spec) {
    std::ifstream ifs("/etc/fstab");
    if (!ifs.is_open()) return;
    std::string line;
    while (std::getline(ifs, line)) {
        std::string t = trim_copy(line);
        if (t.empty() || t[0] == '#') continue;
        std::istringstream iss(t);
        std::string spec, mp, fstype, opts;
        if (!(iss >> spec >> mp >> fstype >> opts)) continue;
        mp = decode_mount_octal(mp);
        bool ro = false;
        std::stringstream oss(opts);
        std::string tok;
        while (std::getline(oss, tok, ',')) {
            if (tok == "ro") { ro = true; break; }
        }
        by_spec[spec] = {mp, ro};
    }
}

std::set<std::string> load_ro_mountpoints() {
    std::set<std::string> ro_mps;
    std::ifstream ifs("/proc/mounts");
    if (!ifs.is_open()) return ro_mps;
    std::string line;
    while (std::getline(ifs, line)) {
        std::istringstream iss(line);
        std::string dev, mp, fstype, opts;
        if (!(iss >> dev >> mp >> fstype >> opts)) continue;
        std::stringstream oss(opts);
        std::string tok;
        while (std::getline(oss, tok, ',')) {
            if (tok == "ro") {
                ro_mps.insert(decode_mount_octal(mp));
                break;
            }
        }
    }
    return ro_mps;
}

bool is_protected_mount(const std::string &mp) {
    return mp == "/" || mp == "/boot" || mp == "/boot/efi" || mp == "/efi" || mp == "[SWAP]";
}

std::vector<BlockDevEntry> collect_block_devices(bool include_system = false) {
    std::string out;
    int rc = 1;
    if (!run_argv_capture({
            "lsblk", "-P", "-b", "-o",
            "NAME,PATH,PKNAME,SIZE,TYPE,TRAN,RM,HOTPLUG,FSTYPE,LABEL,MOUNTPOINT,MODEL,UUID"
        }, out, rc) || rc != 0) {
        return {};
    }

    std::map<std::string, FstabEntryInfo> fstab_map;
    load_fstab_map(fstab_map);
    std::set<std::string> ro_mps = load_ro_mountpoints();

    struct DiskMeta {
        std::string path;
        std::string tran;
        std::string model;
        bool removable = false;
    };
    std::map<std::string, DiskMeta> disk_meta;
    std::vector<std::map<std::string, std::string>> rows;

    for (const auto &line : split_lines(out)) {
        auto kv = parse_lsblk_kv_line(line);
        if (kv.empty()) continue;
        if (kv["TYPE"] == "disk") {
            DiskMeta m;
            m.path = kv["PATH"];
            m.tran = kv["TRAN"];
            m.model = trim_copy(kv["MODEL"]);
            m.removable = (kv["RM"] == "1" || kv["HOTPLUG"] == "1" || m.tran == "usb");
            disk_meta[kv["NAME"]] = m;
        }
        rows.push_back(std::move(kv));
    }

    std::vector<BlockDevEntry> res;
    for (auto &kv : rows) {
        const std::string &type = kv["TYPE"];
        const std::string &fstype = kv["FSTYPE"];
        if (type == "loop" || type == "zram" || type == "rom") continue;
        if (type == "disk" && fstype.empty()) continue;
        if (fstype == "swap" || fstype == "crypto_LUKS") continue;
        if (type != "part" && type != "disk") continue;

        unsigned long long bytes = 0;
        try {
            if (!kv["SIZE"].empty()) bytes = std::stoull(kv["SIZE"]);
        } catch (...) {}

        std::string pk = kv["PKNAME"].empty() ? kv["NAME"] : kv["PKNAME"];
        auto dit = disk_meta.find(pk);

        BlockDevEntry e;
        e.name = kv["NAME"];
        e.path = kv["PATH"];
        e.parent_disk = (dit != disk_meta.end() && !dit->second.path.empty()) ? dit->second.path : e.path;
        e.size_str = format_bytes_human(bytes);
        e.tran = (dit != disk_meta.end() && !dit->second.tran.empty()) ? dit->second.tran : kv["TRAN"];
        e.removable = (kv["RM"] == "1" || kv["HOTPLUG"] == "1" || e.tran == "usb" ||
                       (dit != disk_meta.end() && dit->second.removable));
        e.fstype = fstype.empty() ? "未格式化" : fstype;
        e.label = trim_copy(kv["LABEL"]);
        if (e.label.empty() && dit != disk_meta.end()) e.label = dit->second.model;
        if (e.label.empty()) e.label = e.name;
        e.uuid = trim_copy(kv["UUID"]);
        e.mountpoint = kv["MOUNTPOINT"];
        e.mounted_ro = !e.mountpoint.empty() && ro_mps.count(e.mountpoint) > 0;
        if (!e.uuid.empty()) {
            auto fit = fstab_map.find("UUID=" + e.uuid);
            if (fit != fstab_map.end()) {
                e.fstab_mount = fit->second.mountpoint;
                e.fstab_ro = fit->second.read_only;
            }
        }
        if (e.fstab_mount.empty()) {
            auto fit = fstab_map.find(e.path);
            if (fit != fstab_map.end()) {
                e.fstab_mount = fit->second.mountpoint;
                e.fstab_ro = fit->second.read_only;
            }
        }
        e.is_system = is_protected_mount(e.mountpoint) ||
                      is_protected_mount(e.fstab_mount) ||
                      (fstype == "vfat" && !e.removable && bytes <= 1073741824ULL);
        if (!include_system && e.is_system) continue;
        res.push_back(std::move(e));
    }

    std::stable_sort(res.begin(), res.end(), [](const BlockDevEntry &a, const BlockDevEntry &b) {
        if (a.removable != b.removable) return a.removable > b.removable;
        return a.path < b.path;
    });
    return res;
}

bool is_internal_usb_noise(const std::string &dev_class, const std::string &name) {
    if (dev_class == "09") return true;  // USB Hub / Root Hub
    return contains_any_ci(name, {
        "root hub", "host controller", "motherboard", "aura",
        "rgb", "led controller", "fingerprint"
    });
}

std::vector<UsbDevEntry> collect_usb_devices() {
    std::vector<UsbDevEntry> res;
    std::error_code ec;
    fs::path root("/sys/bus/usb/devices");
    if (!fs::is_directory(root, ec)) return res;

    for (const auto &entry : fs::directory_iterator(root, ec)) {
        std::string fname = entry.path().filename().string();
        if (fname.rfind("usb", 0) == 0 || fname.find(':') != std::string::npos) continue;

        std::string vid = read_sys_attr(entry.path(), "idVendor");
        std::string pid = read_sys_attr(entry.path(), "idProduct");
        if (vid.empty() || pid.empty()) continue;

        std::string mfg = read_sys_attr(entry.path(), "manufacturer");
        std::string prod = read_sys_attr(entry.path(), "product");
        std::string dev_class = read_sys_attr(entry.path(), "bDeviceClass");
        std::string speed = read_sys_attr(entry.path(), "speed");

        std::string display_name;
        if (!mfg.empty() && !prod.empty()) {
            display_name = (prod.rfind(mfg, 0) == 0) ? prod : (mfg + " " + prod);
        } else {
            display_name = !prod.empty() ? prod : mfg;
        }
        if (display_name.empty()) display_name = "USB 外设";
        if (is_internal_usb_noise(dev_class, display_name)) continue;

        std::set<std::string> drivers;
        for (const auto &sub : fs::directory_iterator(entry.path(), ec)) {
            if (sub.path().filename().string().find(':') == std::string::npos) continue;
            fs::path drv_link = sub.path() / "driver";
            if (fs::is_symlink(drv_link, ec)) {
                fs::path target = fs::read_symlink(drv_link, ec);
                if (!ec && !target.empty()) {
                    drivers.insert(target.filename().string());
                }
            }
        }

        std::ostringstream doss;
        bool first = true;
        for (const auto &d : drivers) {
            if (!first) doss << ", ";
            first = false;
            doss << d;
        }
        res.push_back({fname, vid + ":" + pid, display_name,
                       speed.empty() ? "" : (speed + " Mbps"), doss.str()});
    }
    std::sort(res.begin(), res.end(), [](const UsbDevEntry &a, const UsbDevEntry &b) {
        return a.bus_id < b.bus_id;
    });
    return res;
}

std::vector<InputDevEntry> collect_input_devices() {
    std::vector<InputDevEntry> res;
    std::ifstream ifs("/proc/bus/input/devices");
    if (!ifs.is_open()) return res;

    std::set<std::string> seen;
    std::string line, name, bus_code, handlers;
    auto flush_dev = [&]() {
        if (name.empty()) return;
        if (contains_any_ci(name, {
                "power button", "sleep button", "video bus", "pc speaker",
                "wmi", "at translated", "consumer control", "system control",
                "hda intel", "hda nvidia", "wireless radio"
            })) {
            return;
        }
        std::string bus = "内置/其他";
        if (bus_code == "0003") bus = "USB";
        else if (bus_code == "0005") bus = "蓝牙";
        else if (bus_code == "0018") bus = "I2C 触控";

        std::string kind;
        if (handlers.find("js") != std::string::npos) kind = "游戏手柄";
        else if (handlers.find("mouse") != std::string::npos) kind = "鼠标/触控板";
        else if (handlers.find("kbd") != std::string::npos) kind = "键盘";
        else return;

        std::string key = name + "|" + kind;
        if (!seen.insert(key).second) return;
        res.push_back({name, bus, kind, handlers});
    };

    while (std::getline(ifs, line)) {
        if (line.empty()) {
            flush_dev();
            name.clear();
            bus_code.clear();
            handlers.clear();
            continue;
        }
        if (line.rfind("I: ", 0) == 0) {
            auto bp = line.find("Bus=");
            if (bp != std::string::npos && bp + 8 <= line.size()) {
                bus_code = line.substr(bp + 4, 4);
            }
        } else if (line.rfind("N: Name=\"", 0) == 0) {
            auto endq = line.rfind('"');
            if (endq > 9) name = line.substr(9, endq - 9);
        } else if (line.rfind("H: Handlers=", 0) == 0) {
            handlers = trim_copy(line.substr(12));
        }
    }
    flush_dev();
    return res;
}

fs::path disabled_audio_tsv_path() {
    return sys_internal::config_dir() / "disabled_audio.tsv";
}

fs::path wireplumber_rules_path() {
    return fs::path(home_dir()) / ".config/wireplumber/wireplumber.conf.d/90-spinel-audio-disabled.conf";
}

std::vector<AudioDevEntry> load_disabled_audio_entries() {
    std::vector<AudioDevEntry> res;
    std::ifstream ifs(disabled_audio_tsv_path());
    if (!ifs.is_open()) return res;
    std::string line;
    while (std::getline(ifs, line)) {
        line = trim_copy(line);
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string node, kind, label;
        if (!std::getline(iss, node, '\t')) continue;
        std::getline(iss, kind, '\t');
        std::getline(iss, label);
        if (!is_valid_audio_node_name(node)) continue;
        AudioDevEntry e;
        e.node = node;
        e.kind = (kind == "source") ? "source" : "sink";
        e.label = label.empty() ? node : label;
        e.enabled = false;
        res.push_back(std::move(e));
    }
    return res;
}

bool save_disabled_audio_and_apply(const std::vector<AudioDevEntry> &entries) {
    std::error_code ec;
    fs::create_directories(sys_internal::config_dir(), ec);
    {
        std::ofstream ofs(disabled_audio_tsv_path());
        if (!ofs.is_open()) return false;
        for (const auto &e : entries) {
            ofs << e.node << "\t" << e.kind << "\t" << e.label << "\n";
        }
    }

    fs::path wp_file = wireplumber_rules_path();
    fs::create_directories(wp_file.parent_path(), ec);
    if (entries.empty()) {
        fs::remove(wp_file, ec);
    } else {
        std::vector<std::string> alsa_nodes, bluez_nodes;
        for (const auto &e : entries) {
            if (e.node.rfind("bluez_", 0) == 0) bluez_nodes.push_back(e.node);
            else alsa_nodes.push_back(e.node);
        }
        std::ofstream wfs(wp_file);
        if (!wfs.is_open()) return false;
        wfs << "# Managed by SpinelOS Device Manager\n";
        auto write_section = [&](const char *section, const std::vector<std::string> &nodes) {
            if (nodes.empty()) return;
            wfs << section << " = [\n  {\n    matches = [\n";
            for (const auto &n : nodes) {
                wfs << "      { node.name = \"" << n << "\" }\n";
            }
            wfs << "    ]\n    actions = {\n      update-props = {\n        node.disabled = true\n      }\n    }\n  }\n]\n";
        };
        write_section("monitor.alsa.rules", alsa_nodes);
        write_section("monitor.bluez.rules", bluez_nodes);
    }

    std::string out;
    int rc = 1;
    return run_argv_capture({"systemctl", "--user", "restart", "wireplumber"}, out, rc) && rc == 0;
}

std::vector<AudioDevEntry> collect_audio_devices() {
    std::vector<AudioDevEntry> res;
    std::set<std::string> active_nodes;

    std::string def_sink;
    std::string def_source;
    if (has_executable("pactl")) {
        int sink_rc = 1;
        run_argv_capture({"pactl", "get-default-sink"}, def_sink, sink_rc);
        def_sink = trim_copy(def_sink);
        int src_rc = 1;
        run_argv_capture({"pactl", "get-default-source"}, def_source, src_rc);
        def_source = trim_copy(def_source);
    }

    auto collect_pactl_type = [&](const char *ptype, const char *kind, const std::string &def_name) {
        std::string out;
        int rc = 1;
        if (!run_argv_capture({"pactl", "list", ptype}, out, rc) || rc != 0) return;

        std::string cur_name, cur_desc;
        auto flush_audio = [&]() {
            if (cur_name.empty()) return;
            // Skip virtual loopback monitor sources
            if (cur_name.size() > 8 && cur_name.compare(cur_name.size() - 8, 8, ".monitor") == 0) return;
            if (!is_valid_audio_node_name(cur_name)) return;
            active_nodes.insert(cur_name);
            AudioDevEntry e;
            e.node = cur_name;
            e.label = cur_desc.empty() ? cur_name : cur_desc;
            e.kind = kind;
            e.enabled = true;
            e.is_default = (cur_name == def_name);
            res.push_back(std::move(e));
        };

        for (const auto &raw : split_lines(out)) {
            std::string t = trim_copy(raw);
            if (t.rfind("Sink #", 0) == 0 || t.rfind("Source #", 0) == 0) {
                flush_audio();
                cur_name.clear();
                cur_desc.clear();
            } else if (t.rfind("Name:", 0) == 0) {
                cur_name = trim_copy(t.substr(5));
            } else if (t.rfind("Description:", 0) == 0) {
                cur_desc = trim_copy(t.substr(12));
            }
        }
        flush_audio();
    };

    if (has_executable("pactl")) {
        collect_pactl_type("sinks", "sink", def_sink);
        collect_pactl_type("sources", "source", def_source);
    }

    for (auto &d : load_disabled_audio_entries()) {
        if (active_nodes.count(d.node)) continue;
        res.push_back(std::move(d));
    }

    std::stable_sort(res.begin(), res.end(), [](const AudioDevEntry &a, const AudioDevEntry &b) {
        if (a.kind != b.kind) return a.kind < b.kind;  // sinks first, then sources
        if (a.enabled != b.enabled) return a.enabled > b.enabled;
        return a.label < b.label;
    });
    return res;
}

void emit_devices_feedback(AppState *state, bool ok, const std::string &msg) {
    append_app_log(std::string("[Devices] ") + (ok ? "" : "[ERR] ") + msg);
    post_js_to_main(state,
        "window.__spinelDevicesAction && window.__spinelDevicesAction(" +
            std::string(ok ? "true" : "false") + ",\"" + json_escape(msg) + "\");",
        false);
}

bool check_devices_sudo_nopass() {
    std::string out;
    int rc = 1;
    return run_argv_capture({"sudo", "-n", "true"}, out, rc) && rc == 0;
}

bool is_valid_uuid_token(const std::string &uuid) {
    if (uuid.empty() || uuid.size() > 64) return false;
    for (char ch : uuid) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && c != '-') return false;
    }
    return true;
}

std::map<std::string, std::string> parse_kv_payload(const std::string &payload) {
    std::map<std::string, std::string> kv;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

bool run_devices_sudo_script(const std::string &script,
                             const std::vector<std::string> &script_args,
                             const std::string &password,
                             std::string &out,
                             int &rc) {
    bool nopass = check_devices_sudo_nopass();
    if (!nopass && password.empty()) {
        out = "需要当前用户管理员密码（sudo）才能修改 /etc/fstab。";
        rc = 2;
        return false;
    }
    std::vector<std::string> cmd = nopass
        ? std::vector<std::string>{"sudo", "-n", "sh", "-c", script, "_"}
        : std::vector<std::string>{"sudo", "-S", "-p", "", "sh", "-c", script, "_"};
    cmd.insert(cmd.end(), script_args.begin(), script_args.end());
    return run_argv_stdin_capture(cmd, nopass ? "" : password, out, rc) && rc == 0;
}

}  // namespace

std::string unescape_lsblk_value(const std::string &raw) {
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            char next = raw[i + 1];
            if (next == 'x' && i + 3 < raw.size()) {
                int h1 = hex_val(raw[i + 2]);
                int h2 = hex_val(raw[i + 3]);
                if (h1 >= 0 && h2 >= 0) {
                    out.push_back(static_cast<char>((h1 << 4) | h2));
                    i += 3;
                    continue;
                }
            }
            switch (next) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                default: out.push_back(next); break;
            }
            ++i;
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

bool is_valid_block_dev_path(const std::string &path) {
    if (path.rfind("/dev/", 0) != 0 || path.size() <= 5 || path.size() > 40) return false;
    std::string leaf = path.substr(5);
    if (leaf.find("..") != std::string::npos) return false;
    for (char ch : leaf) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

bool is_valid_mnt_leaf(const std::string &leaf) {
    if (leaf.empty() || leaf.size() > 48 || leaf == "." || leaf == "..") return false;
    for (char ch : leaf) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

bool is_valid_audio_node_name(const std::string &node) {
    if (node.empty() || node.size() > 160) return false;
    for (char ch : node) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && c != '.' && c != '_' && c != '-' && c != ':') return false;
    }
    return true;
}

void push_devices_state(AppState *state) {
    if (!state || state->shutting_down.load()) return;
    std::thread([state]() {
        auto disks = collect_block_devices(false);
        auto usb = collect_usb_devices();
        auto inputs = collect_input_devices();
        auto audio = collect_audio_devices();
        bool sudo_nopass = check_devices_sudo_nopass();

        std::ostringstream js;
        js << "window.__spinelDevicesData && window.__spinelDevicesData({"
           << "\"sudo_nopass\":" << (sudo_nopass ? "true" : "false") << ","
           << "\"disks\":[";
        for (size_t i = 0; i < disks.size(); ++i) {
            if (i) js << ",";
            const auto &d = disks[i];
            js << "{"
               << "\"name\":\"" << json_escape(d.name) << "\","
               << "\"path\":\"" << json_escape(d.path) << "\","
               << "\"parent_disk\":\"" << json_escape(d.parent_disk) << "\","
               << "\"size\":\"" << json_escape(d.size_str) << "\","
               << "\"tran\":\"" << json_escape(d.tran) << "\","
               << "\"removable\":" << (d.removable ? "true" : "false") << ","
               << "\"fstype\":\"" << json_escape(d.fstype) << "\","
               << "\"label\":\"" << json_escape(d.label) << "\","
               << "\"uuid\":\"" << json_escape(d.uuid) << "\","
               << "\"mountpoint\":\"" << json_escape(d.mountpoint) << "\","
               << "\"mounted_ro\":" << (d.mounted_ro ? "true" : "false") << ","
               << "\"fstab_mount\":\"" << json_escape(d.fstab_mount) << "\","
               << "\"fstab_ro\":" << (d.fstab_ro ? "true" : "false")
               << "}";
        }
        js << "],\"usb\":[";
        for (size_t i = 0; i < usb.size(); ++i) {
            if (i) js << ",";
            const auto &u = usb[i];
            js << "{"
               << "\"bus_id\":\"" << json_escape(u.bus_id) << "\","
               << "\"vid_pid\":\"" << json_escape(u.vid_pid) << "\","
               << "\"name\":\"" << json_escape(u.name) << "\","
               << "\"speed\":\"" << json_escape(u.speed_mbps) << "\","
               << "\"driver\":\"" << json_escape(u.driver) << "\""
               << "}";
        }
        js << "],\"audio_devs\":[";
        for (size_t i = 0; i < audio.size(); ++i) {
            if (i) js << ",";
            const auto &a = audio[i];
            js << "{"
               << "\"node\":\"" << json_escape(a.node) << "\","
               << "\"label\":\"" << json_escape(a.label) << "\","
               << "\"kind\":\"" << json_escape(a.kind) << "\","
               << "\"enabled\":" << (a.enabled ? "true" : "false") << ","
               << "\"is_default\":" << (a.is_default ? "true" : "false")
               << "}";
        }
        js << "],\"inputs\":[";
        for (size_t i = 0; i < inputs.size(); ++i) {
            if (i) js << ",";
            const auto &in = inputs[i];
            js << "{"
               << "\"name\":\"" << json_escape(in.name) << "\","
               << "\"bus\":\"" << json_escape(in.bus) << "\","
               << "\"kind\":\"" << json_escape(in.kind) << "\","
               << "\"handlers\":\"" << json_escape(in.handlers) << "\""
               << "}";
        }
        js << "]});";
        post_js_to_main(state, js.str(), false);
    }).detach();
}

void mount_block_device(AppState *state, const std::string &dev_path, bool read_only) {
    if (!state) return;
    if (!is_valid_block_dev_path(dev_path)) {
        emit_devices_feedback(state, false, "无效的设备路径: " + dev_path);
        return;
    }
    append_app_log(std::string("[Devices] 请求") + (read_only ? "只读挂载: " : "挂载: ") + dev_path);
    std::thread([state, dev_path, read_only]() {
        std::string out;
        int rc = 1;
        auto run_udisks_mount = [&](bool ro, std::string &m_out, int &m_rc) {
            std::vector<std::string> args = {"udisksctl", "mount", "--no-user-interaction"};
            if (ro) args.insert(args.end(), {"-o", "ro"});
            args.insert(args.end(), {"-b", dev_path});
            return run_argv_capture(args, m_out, m_rc) && m_rc == 0;
        };

        bool ok = run_udisks_mount(read_only, out, rc);
        bool used_ro_fallback = false;

        if (!ok && !read_only) {
            std::string low = out;
            for (char &ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const bool maybe_ntfs_dirty =
                low.find("ntfs") != std::string::npos ||
                low.find("dirty") != std::string::npos ||
                low.find("hibernat") != std::string::npos ||
                low.find("windows") != std::string::npos ||
                low.find("unsafe") != std::string::npos ||
                low.find("read-only") != std::string::npos ||
                low.find("wrong fs type") != std::string::npos ||
                low.find("superblock") != std::string::npos ||
                low.find("mount exited") != std::string::npos ||
                low.find("cannot mount") != std::string::npos ||
                low.find("metadata") != std::string::npos;
            if (maybe_ntfs_dirty) {
                std::string ro_out;
                int ro_rc = 1;
                if (run_udisks_mount(true, ro_out, ro_rc)) {
                    ok = true;
                    used_ro_fallback = true;
                    out = ro_out;
                }
            }
        }

        std::string msg = trim_copy(out);
        if (ok) {
            if (used_ro_fallback) {
                emit_devices_feedback(state, true,
                    (msg.empty() ? ("已挂载 " + dev_path) : msg) +
                    "（检测到 Windows 休眠或脏标志，已自动切换为【只读安全模式】以保护数据）");
            } else if (read_only) {
                emit_devices_feedback(state, true,
                    (msg.empty() ? ("已只读挂载 " + dev_path) : msg) + "（只读模式）");
            } else {
                emit_devices_feedback(state, true, msg.empty() ? ("已挂载 " + dev_path) : msg);
            }
        } else {
            std::string low = msg;
            for (char &ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (low.find("not authorized") != std::string::npos ||
                low.find("authentication agent") != std::string::npos) {
                emit_devices_feedback(state, false,
                    "挂载未获系统权限放行（" + msg + "）：请在「系统设置 → 权限与安全」启用认证代理，或更新系统规则 (--full)；也可在终端运行 sudo mount -o ro " + dev_path + " /mnt/win_data 抢救数据。");
            } else if (low.find("bitlocker") != std::string::npos) {
                emit_devices_feedback(state, false,
                    "挂载失败：该分区可能已启用 Windows BitLocker 加密，需先在 Windows 或使用恢复密钥解锁。");
            } else {
                emit_devices_feedback(state, false,
                    "挂载失败: " + (msg.empty() ? dev_path : msg) + "（如为重要 Windows 盘，可尝试「只读挂载」或在终端执行 sudo mount -o ro " + dev_path + " /mnt/win_data）");
            }
        }
        push_devices_state(state);
    }).detach();
}

void unmount_block_device(AppState *state, const std::string &dev_path) {
    if (!state) return;
    if (!is_valid_block_dev_path(dev_path)) {
        emit_devices_feedback(state, false, "无效的设备路径: " + dev_path);
        return;
    }
    append_app_log("[Devices] 请求卸载分区: " + dev_path);
    std::thread([state, dev_path]() {
        std::string out;
        int rc = 1;
        bool ok = run_argv_capture({"udisksctl", "unmount", "--no-user-interaction", "-b", dev_path}, out, rc) && rc == 0;
        if (!ok && check_devices_sudo_nopass()) {
            std::string s_out;
            int s_rc = 1;
            if (run_argv_capture({"sudo", "-n", "umount", dev_path}, s_out, s_rc) && s_rc == 0) {
                ok = true;
                out.clear();
            }
        }
        std::string msg = trim_copy(out);
        emit_devices_feedback(state, ok, ok ? ("已安全卸载 " + dev_path) : ("卸载失败: " + (msg.empty() ? dev_path : msg)));
        push_devices_state(state);
    }).detach();
}

void eject_block_device(AppState *state, const std::string &disk_path) {
    if (!state) return;
    if (!is_valid_block_dev_path(disk_path)) {
        emit_devices_feedback(state, false, "无效的磁盘路径: " + disk_path);
        return;
    }
    append_app_log("[Devices] 请求安全断电弹出磁盘: " + disk_path);
    std::thread([state, disk_path]() {
        auto devs = collect_block_devices(true);
        std::string out;
        int rc = 1;
        for (const auto &d : devs) {
            if (d.parent_disk == disk_path && !d.mountpoint.empty()) {
                if (d.is_system) {
                    emit_devices_feedback(state, false, "系统关键分区无法弹出: " + d.path);
                    return;
                }
                if (!run_argv_capture({"udisksctl", "unmount", "--no-user-interaction", "-b", d.path}, out, rc) || rc != 0) {
                    emit_devices_feedback(state, false, "卸载分区失败 (" + d.path + "): " + trim_copy(out));
                    push_devices_state(state);
                    return;
                }
            }
        }
        bool ok = run_argv_capture({"udisksctl", "power-off", "--no-user-interaction", "-b", disk_path}, out, rc) && rc == 0;
        std::string msg = trim_copy(out);
        emit_devices_feedback(state, ok, ok ? ("设备已安全断电弹出，现在可以拔出: " + disk_path) : ("安全弹出失败: " + (msg.empty() ? disk_path : msg)));
        push_devices_state(state);
    }).detach();
}

void open_mount_folder(AppState *state, const std::string &mount_path) {
    if (!state) return;
    std::error_code ec;
    if (mount_path.empty() || !fs::is_directory(mount_path, ec)) {
        emit_devices_feedback(state, false, "挂载目录不可访问: " + mount_path);
        return;
    }
    append_app_log("[Devices] 打开挂载目录: " + mount_path);
    spawn_argv_detached({"spinel-files", mount_path});
}

void set_block_fstab_mount(AppState *state, const std::string &payload) {
    if (!state) return;
    auto kv = parse_kv_payload(payload);
    std::string dev_path = trim_copy(kv["dev"]);
    std::string mnt_leaf = trim_copy(kv["name"]);
    bool want_ro = (trim_copy(kv["ro"]) != "0");
    std::string password = kv["password"];

    if (!is_valid_block_dev_path(dev_path)) {
        emit_devices_feedback(state, false, "无效的设备路径: " + dev_path);
        return;
    }
    if (!is_valid_mnt_leaf(mnt_leaf)) {
        emit_devices_feedback(state, false, "无效的 /mnt 目录名: " + mnt_leaf);
        return;
    }

    append_app_log("[Devices] 请求设置 /etc/fstab 开机自动挂载: " + dev_path + " -> /mnt/" + mnt_leaf + (want_ro ? " (ro)" : " (rw)"));
    std::thread([state, dev_path, mnt_leaf, want_ro, password]() {
        auto devs = collect_block_devices(true);
        const BlockDevEntry *found = nullptr;
        for (const auto &d : devs) {
            if (d.path == dev_path) { found = &d; break; }
        }
        if (!found) {
            emit_devices_feedback(state, false, "未找到设备: " + dev_path);
            return;
        }
        if (found->is_system) {
            emit_devices_feedback(state, false, "系统关键分区不允许修改 fstab: " + dev_path);
            return;
        }
        if (!is_valid_uuid_token(found->uuid)) {
            emit_devices_feedback(state, false, "无法读取分区 UUID，请先格式化后再配置固定挂载: " + dev_path);
            return;
        }

        std::string raw_fs = found->fstype;
        std::string fstab_fs;
        std::string opts_rw;
        std::string opts_ro;
        std::string uid_s = std::to_string(static_cast<unsigned long>(::getuid()));
        std::string gid_s = std::to_string(static_cast<unsigned long>(::getgid()));

        if (raw_fs == "ntfs") {
            fstab_fs = "ntfs3";
            opts_rw = "rw,uid=" + uid_s + ",gid=" + gid_s + ",dmask=022,fmask=133,nofail,x-systemd.device-timeout=5";
            opts_ro = "ro,uid=" + uid_s + ",gid=" + gid_s + ",dmask=022,fmask=133,nofail,x-systemd.device-timeout=5";
        } else if (raw_fs == "exfat" || raw_fs == "vfat") {
            fstab_fs = raw_fs;
            opts_rw = "rw,uid=" + uid_s + ",gid=" + gid_s + ",dmask=022,fmask=133,nofail,x-systemd.device-timeout=5";
            opts_ro = "ro,uid=" + uid_s + ",gid=" + gid_s + ",dmask=022,fmask=133,nofail,x-systemd.device-timeout=5";
        } else if (raw_fs == "ext4" || raw_fs == "ext3" || raw_fs == "ext2" ||
                   raw_fs == "btrfs" || raw_fs == "xfs" || raw_fs == "f2fs") {
            fstab_fs = raw_fs;
            opts_rw = "defaults,nofail,x-systemd.device-timeout=5";
            opts_ro = "defaults,ro,nofail,x-systemd.device-timeout=5";
        } else {
            emit_devices_feedback(state, false, "暂不支持将该文件系统写入 /etc/fstab: " + raw_fs);
            return;
        }

        const std::string target = "/mnt/" + mnt_leaf;
        const std::string script =
            "set -eu; "
            "DEV=\"$1\"; UUID=\"$2\"; TGT=\"$3\"; FST=\"$4\"; OPTS_RW=\"$5\"; OPTS_RO=\"$6\"; WANT_RO=\"$7\"; "
            "[ -f /etc/fstab.spinelos.bak ] || cp -a /etc/fstab /etc/fstab.spinelos.bak; "
            "write_fstab() { "
            "  _opts=\"$1\"; "
            "  _tmp=$(mktemp /tmp/spinel-fstab-XXXXXX); "
            "  awk -v u=\"UUID=$UUID\" -v d=\"$DEV\" -v t=\"$TGT\" -v c=\"# SpinelOS auto-mount: $DEV\" '"
            "    $0 == c { next } "
            "    $0 ~ /^[[:space:]]*(#|$)/ { print; next } "
            "    $1 == u || $1 == d || $2 == t { next } "
            "    { print } "
            "  ' /etc/fstab > \"$_tmp\"; "
            "  printf '\\n# SpinelOS auto-mount: %s\\nUUID=%s\\t%s\\t%s\\t%s\\t0\\t0\\n' \"$DEV\" \"$UUID\" \"$TGT\" \"$FST\" \"$_opts\" >> \"$_tmp\"; "
            "  chmod 644 \"$_tmp\"; "
            "  mv \"$_tmp\" /etc/fstab; "
            "}; "
            "CUR=$(findmnt -n -o TARGET --source \"$DEV\" 2>/dev/null | head -n1 || true); "
            "if [ -n \"$CUR\" ]; then umount \"$CUR\" || umount \"$DEV\"; fi; "
            "mkdir -p \"$TGT\"; "
            "if [ \"$WANT_RO\" = \"1\" ]; then "
            "  write_fstab \"$OPTS_RO\"; "
            "  systemctl daemon-reload >/dev/null 2>&1 || true; "
            "  mount \"$TGT\"; "
            "else "
            "  write_fstab \"$OPTS_RW\"; "
            "  systemctl daemon-reload >/dev/null 2>&1 || true; "
            "  if ! mount \"$TGT\" 2>/tmp/spinel-fstab-mount.err; then "
            "    if [ \"$FST\" = \"ntfs3\" ] && mount -t \"$FST\" -o \"$OPTS_RO\" \"UUID=$UUID\" \"$TGT\" >/dev/null 2>&1; then "
            "      umount \"$TGT\" 2>/dev/null || true; "
            "      write_fstab \"$OPTS_RO\"; "
            "      systemctl daemon-reload >/dev/null 2>&1 || true; "
            "      mount \"$TGT\"; "
            "      echo '__SPINEL_RO_FALLBACK__'; "
            "    else "
            "      cat /tmp/spinel-fstab-mount.err >&2 || true; "
            "      exit 1; "
            "    fi; "
            "  fi; "
            "fi";

        std::string out;
        int rc = 1;
        bool ok = run_devices_sudo_script(
            script,
            {dev_path, found->uuid, target, fstab_fs, opts_rw, opts_ro, want_ro ? "1" : "0"},
            password, out, rc);

        if (ok) {
            bool ro_fb = (out.find("__SPINEL_RO_FALLBACK__") != std::string::npos);
            if (ro_fb) {
                emit_devices_feedback(state, true,
                    "已固定并开机自动挂载到 " + target + "（检测到 Windows 休眠/脏标志，已自动设为只读保护模式）");
            } else {
                emit_devices_feedback(state, true,
                    "已固定挂载并写入 /etc/fstab: " + target + (want_ro ? "（只读模式，含 nofail 保护）" : "（读写模式，含 nofail 保护）"));
            }
        } else {
            emit_devices_feedback(state, false, "配置固定挂载失败: " + trim_copy(out));
        }
        push_devices_state(state);
    }).detach();
}

void remove_block_fstab_mount(AppState *state, const std::string &payload) {
    if (!state) return;
    auto kv = parse_kv_payload(payload);
    std::string dev_path = trim_copy(kv["dev"]);
    std::string password = kv["password"];

    if (!is_valid_block_dev_path(dev_path)) {
        emit_devices_feedback(state, false, "无效的设备路径: " + dev_path);
        return;
    }

    append_app_log("[Devices] 请求移除 /etc/fstab 开机自动挂载: " + dev_path);
    std::thread([state, dev_path, password]() {
        auto devs = collect_block_devices(true);
        std::string uuid;
        std::string old_mp;
        for (const auto &d : devs) {
            if (d.path == dev_path) {
                uuid = d.uuid;
                old_mp = d.fstab_mount;
                break;
            }
        }
        const std::string script =
            "set -eu; "
            "DEV=\"$1\"; UUID=\"$2\"; OLD_MP=\"$3\"; "
            "TMP=$(mktemp /tmp/spinel-fstab-XXXXXX); "
            "awk -v u=\"UUID=$UUID\" -v d=\"$DEV\" -v has_u=\"${UUID:+1}\" -v c=\"# SpinelOS auto-mount: $DEV\" '"
            "  $0 == c { next } "
            "  $0 ~ /^[[:space:]]*(#|$)/ { print; next } "
            "  (has_u == \"1\" && $1 == u) || $1 == d { next } "
            "  { print } "
            "' /etc/fstab > \"$TMP\"; "
            "chmod 644 \"$TMP\"; "
            "mv \"$TMP\" /etc/fstab; "
            "if [ -n \"$OLD_MP\" ] && mountpoint -q \"$OLD_MP\" 2>/dev/null; then umount \"$OLD_MP\" || true; fi; "
            "if [ -n \"$OLD_MP\" ] && [ -d \"$OLD_MP\" ]; then rmdir \"$OLD_MP\" 2>/dev/null || true; fi; "
            "systemctl daemon-reload >/dev/null 2>&1 || true";

        std::string out;
        int rc = 1;
        bool ok = run_devices_sudo_script(script, {dev_path, uuid, old_mp}, password, out, rc);
        emit_devices_feedback(state, ok,
            ok ? ("已取消 " + dev_path + " 的开机自动挂载配置") : ("移除开机挂载失败: " + trim_copy(out)));
        push_devices_state(state);
    }).detach();
}

void disable_audio_device(AppState *state, const std::string &payload) {
    if (!state) return;
    std::string node, kind = "sink", label;
    for (const auto &line : split_lines(payload)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "node") node = trim_copy(v);
        else if (k == "kind") kind = trim_copy(v);
        else if (k == "label") label = trim_copy(v);
    }
    if (!is_valid_audio_node_name(node)) {
        emit_devices_feedback(state, false, "无效的音频节点名");
        return;
    }
    for (char &ch : label) {
        if (ch == '\t' || ch == '\r' || ch == '\n') ch = ' ';
    }

    std::thread([state, node, kind, label]() {
        auto entries = load_disabled_audio_entries();
        bool exists = false;
        for (auto &e : entries) {
            if (e.node == node) {
                exists = true;
                if (!label.empty()) e.label = label;
            }
        }
        if (!exists) {
            entries.push_back({node, kind == "source" ? "source" : "sink", label.empty() ? node : label, false});
        }
        bool ok = save_disabled_audio_and_apply(entries);
        emit_devices_feedback(state, ok, ok ? ("已禁用音频设备: " + (label.empty() ? node : label)) : "重启 WirePlumber 应用禁用规则失败");
        push_devices_state(state);
    }).detach();
}

void enable_audio_device(AppState *state, const std::string &node_name) {
    if (!state) return;
    std::string node = trim_copy(node_name);
    if (!is_valid_audio_node_name(node)) {
        emit_devices_feedback(state, false, "无效的音频节点名: " + node);
        return;
    }
    std::thread([state, node]() {
        auto entries = load_disabled_audio_entries();
        std::string label = node;
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const AudioDevEntry &e) {
            if (e.node == node) {
                if (!e.label.empty()) label = e.label;
                return true;
            }
            return false;
        }), entries.end());

        bool ok = save_disabled_audio_and_apply(entries);
        emit_devices_feedback(state, ok, ok ? ("已重新启用音频设备: " + label) : "重启 WirePlumber 恢复设备失败");
        push_devices_state(state);
    }).detach();
}
