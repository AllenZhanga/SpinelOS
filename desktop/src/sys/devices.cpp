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
    std::string type;
    std::string tran;
    bool removable = false;
    std::string fstype;
    std::string label;
    std::string mountpoint;
    std::string model;
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

struct PciDevEntry {
    std::string slot;
    std::string desc;
    std::string driver;
};

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
                if (line[i] == '\\' && i + 1 < n) {
                    ++i;
                }
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

std::vector<BlockDevEntry> collect_block_devices() {
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
        if (type == "disk" && fstype.empty()) continue;  // Unformatted/partitioned disk container
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
        e.type = type;
        e.tran = (dit != disk_meta.end() && !dit->second.tran.empty()) ? dit->second.tran : kv["TRAN"];
        e.removable = (kv["RM"] == "1" || kv["HOTPLUG"] == "1" || e.tran == "usb" ||
                       (dit != disk_meta.end() && dit->second.removable));
        e.fstype = fstype.empty() ? "未格式化" : fstype;
        e.label = trim_copy(kv["LABEL"]);
        if (e.label.empty() && dit != disk_meta.end()) e.label = dit->second.model;
        if (e.label.empty()) e.label = e.name;
        e.mountpoint = kv["MOUNTPOINT"];
        e.model = (dit != disk_meta.end()) ? dit->second.model : "";
        e.is_system = is_protected_mount(e.mountpoint) || (fstype == "vfat" && !e.removable && bytes <= 1073741824ULL);
        res.push_back(std::move(e));
    }

    // Sort removable USB drives first, then secondary partitions, then system mounts
    std::stable_sort(res.begin(), res.end(), [](const BlockDevEntry &a, const BlockDevEntry &b) {
        if (a.removable != b.removable) return a.removable > b.removable;
        if (a.is_system != b.is_system) return a.is_system < b.is_system;
        return a.path < b.path;
    });
    return res;
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

        // Skip internal root/external hub noise only if unnamed, keep labeled hubs too
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

        UsbDevEntry u;
        u.bus_id = fname;
        u.vid_pid = vid + ":" + pid;
        if (!mfg.empty() && !prod.empty()) {
            u.name = (prod.rfind(mfg, 0) == 0) ? prod : (mfg + " " + prod);
        } else {
            u.name = !prod.empty() ? prod : (!mfg.empty() ? mfg : (dev_class == "09" ? "USB Hub 集线器" : "USB 设备"));
        }
        u.speed_mbps = speed.empty() ? "" : (speed + " Mbps");
        std::ostringstream doss;
        bool first = true;
        for (const auto &d : drivers) {
            if (!first) doss << ", ";
            first = false;
            doss << d;
        }
        u.driver = doss.str();
        res.push_back(std::move(u));
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

    std::string line, name, bus_code, handlers;
    auto flush_dev = [&]() {
        if (name.empty()) return;
        std::string bus = "其他";
        if (bus_code == "0003") bus = "USB";
        else if (bus_code == "0005") bus = "蓝牙";
        else if (bus_code == "0018") bus = "I2C";
        else if (bus_code == "0011" || bus_code == "0019") bus = "主板/内置";

        std::string kind = "输入设备";
        if (handlers.find("js") != std::string::npos) kind = "游戏手柄";
        else if (handlers.find("mouse") != std::string::npos) kind = "鼠标/触控";
        else if (handlers.find("kbd") != std::string::npos) kind = "键盘";
        else return;  // Filter out pure power-button / virtual video bus events

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

std::vector<PciDevEntry> collect_pci_devices() {
    std::vector<PciDevEntry> res;
    std::string out;
    int rc = 1;
    if (!run_argv_capture({"lspci", "-nnk"}, out, rc) || rc != 0) return res;

    PciDevEntry cur;
    bool active = false;
    for (const auto &line : split_lines(out)) {
        if (!line.empty() && !std::isspace(static_cast<unsigned char>(line[0]))) {
            if (active) res.push_back(std::move(cur));
            cur = PciDevEntry{};
            auto sp = line.find(' ');
            if (sp != std::string::npos) {
                cur.slot = line.substr(0, sp);
                cur.desc = trim_copy(line.substr(sp + 1));
            } else {
                cur.desc = line;
            }
            active = true;
        } else if (active) {
            std::string t = trim_copy(line);
            const std::string prefix = "Kernel driver in use:";
            if (t.rfind(prefix, 0) == 0) {
                cur.driver = trim_copy(t.substr(prefix.size()));
            }
        }
    }
    if (active) res.push_back(std::move(cur));
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

void push_devices_state(AppState *state) {
    if (!state || state->shutting_down.load()) return;
    std::thread([state]() {
        auto disks = collect_block_devices();
        auto usb = collect_usb_devices();
        auto inputs = collect_input_devices();
        auto pci = collect_pci_devices();

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
               << "\"mountpoint\":\"" << json_escape(d.mountpoint) << "\","
               << "\"model\":\"" << json_escape(d.model) << "\","
               << "\"is_system\":" << (d.is_system ? "true" : "false")
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
        js << "],\"pci\":[";
        for (size_t i = 0; i < pci.size(); ++i) {
            if (i) js << ",";
            const auto &p = pci[i];
            js << "{"
               << "\"slot\":\"" << json_escape(p.slot) << "\","
               << "\"desc\":\"" << json_escape(p.desc) << "\","
               << "\"driver\":\"" << json_escape(p.driver) << "\""
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
        if (ok) {
            emit_devices_feedback(state, true, msg.empty() ? ("已挂载 " + dev_path) : msg);
        } else {
            emit_devices_feedback(state, false, "挂载失败: " + (msg.empty() ? dev_path : msg));
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
        auto devs = collect_block_devices();
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
