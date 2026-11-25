#include "system_control.hpp"
#include "sys_internal.hpp"
#include "common.hpp"

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
    std::string mountpoint;
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
        std::string val;
        if (i < n && line[i] == '"') {
            ++i;
            while (i < n && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < n) ++i;
                val.push_back(line[i++]);
            }
            if (i < n && line[i] == '"') ++i;
        } else {
            while (i < n && !std::isspace(static_cast<unsigned char>(line[i]))) {
                val.push_back(line[i++]);
            }
        }
        out[std::move(key)] = std::move(val);
    }
    return out;
}

bool is_protected_mount(const std::string &mp) {
    return mp == "/" || mp == "/boot" || mp == "/boot/efi" || mp == "/efi" || mp == "[SWAP]";
}

std::vector<BlockDevEntry> collect_block_devices(bool include_system = false) {
    std::string out;
    int rc = 1;
    if (!run_argv_capture({
            "lsblk", "-P", "-b", "-o",
            "NAME,PATH,PKNAME,SIZE,TYPE,TRAN,RM,HOTPLUG,FSTYPE,LABEL,MOUNTPOINT,MODEL"
        }, out, rc) || rc != 0) {
        return {};
    }

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
        e.mountpoint = kv["MOUNTPOINT"];
        e.is_system = is_protected_mount(e.mountpoint) ||
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

    std::string def_sink = trim_copy(run_cmd_capture("pactl get-default-sink 2>/dev/null"));
    std::string def_source = trim_copy(run_cmd_capture("pactl get-default-source 2>/dev/null"));

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
    post_js_to_main(state,
        "window.__spinelDevicesAction && window.__spinelDevicesAction(" +
            std::string(ok ? "true" : "false") + ",\"" + json_escape(msg) + "\");",
        false);
}

}  // namespace

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

        std::ostringstream js;
        js << "window.__spinelDevicesData && window.__spinelDevicesData({\"disks\":[";
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
               << "\"mountpoint\":\"" << json_escape(d.mountpoint) << "\""
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

void mount_block_device(AppState *state, const std::string &dev_path) {
    if (!state) return;
    if (!is_valid_block_dev_path(dev_path)) {
        emit_devices_feedback(state, false, "无效的设备路径: " + dev_path);
        return;
    }
    std::thread([state, dev_path]() {
        std::string out;
        int rc = 1;
        bool ok = run_argv_capture({"udisksctl", "mount", "--no-user-interaction", "-b", dev_path}, out, rc) && rc == 0;
        std::string msg = trim_copy(out);
        emit_devices_feedback(state, ok, ok ? (msg.empty() ? ("已挂载 " + dev_path) : msg) : ("挂载失败: " + (msg.empty() ? dev_path : msg)));
        push_devices_state(state);
    }).detach();
}

void unmount_block_device(AppState *state, const std::string &dev_path) {
    if (!state) return;
    if (!is_valid_block_dev_path(dev_path)) {
        emit_devices_feedback(state, false, "无效的设备路径: " + dev_path);
        return;
    }
    std::thread([state, dev_path]() {
        std::string out;
        int rc = 1;
        bool ok = run_argv_capture({"udisksctl", "unmount", "--no-user-interaction", "-b", dev_path}, out, rc) && rc == 0;
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
    spawn_argv_detached({"thunar", mount_path});
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
