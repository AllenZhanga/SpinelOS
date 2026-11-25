#include "system_control.hpp"
#include "sys_internal.hpp"
#include "common.hpp"
#include "native_shell.hpp"

#include <fcntl.h>
#include <linux/joystick.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct AudioDeviceItem {
    int id = 0;
    std::string name;
    bool is_default = false;
};

void parse_wpctl_audio(std::vector<AudioDeviceItem> &sinks, std::vector<AudioDeviceItem> &sources) {
    std::string out;
    int rc = 1;
    if (!run_argv_capture({"wpctl", "status"}, out, rc) || rc != 0) return;

    enum Sec { None, Sinks, Sources } sec = None;
    bool in_audio = false;
    for (const auto &raw : split_lines(out)) {
        if (raw.find("Audio") != std::string::npos && raw.rfind("Audio", 0) == 0) {
            in_audio = true;
            sec = None;
            continue;
        }
        if (in_audio && !raw.empty() && std::isalpha(static_cast<unsigned char>(raw[0])) && raw.back() == ':') {
            break;
        }
        if (!in_audio) continue;

        if (raw.find("Sinks:") != std::string::npos) { sec = Sinks; continue; }
        if (raw.find("Sources:") != std::string::npos) { sec = Sources; continue; }
        if (raw.find("Filters:") != std::string::npos || raw.find("Streams:") != std::string::npos ||
            raw.find("Sink endpoints:") != std::string::npos || raw.find("Source endpoints:") != std::string::npos) {
            sec = None;
            continue;
        }
        if (sec == None) continue;

        bool is_def = (raw.find('*') != std::string::npos);
        size_t i = 0;
        while (i < raw.size() && !std::isdigit(static_cast<unsigned char>(raw[i]))) ++i;
        if (i >= raw.size()) continue;
        size_t j = i;
        while (j < raw.size() && std::isdigit(static_cast<unsigned char>(raw[j]))) ++j;
        if (j >= raw.size() || raw[j] != '.') continue;
        int id = std::atoi(raw.substr(i, j - i).c_str());
        if (id <= 0) continue;
        std::string nm = trim_copy(raw.substr(j + 1));
        auto bpos = nm.rfind('[');
        if (bpos != std::string::npos) {
            nm = trim_copy(nm.substr(0, bpos));
        }
        if (nm.empty()) nm = "Audio Device " + std::to_string(id);
        if (sec == Sinks) sinks.push_back({id, nm, is_def});
        else if (sec == Sources) sources.push_back({id, nm, is_def});
    }
}

void write_audio_items_json(std::ostringstream &js, const std::vector<AudioDeviceItem> &items) {
    js << "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) js << ",";
        js << "{\"id\":" << items[i].id
           << ",\"name\":\"" << json_escape(items[i].name) << "\""
           << ",\"is_default\":" << (items[i].is_default ? "true" : "false") << "}";
    }
    js << "]";
}

std::atomic<bool> g_gamepad_stop{false};
std::thread g_gamepad_thread;

struct GamepadCmdIdle {
    AppState *state;
    std::string cmd;
};

void dispatch_gamepad_cmd(AppState *state, const std::string &cmd) {
    if (!state || state->shutting_down.load()) return;
    g_idle_add([](gpointer d) -> gboolean {
        std::unique_ptr<GamepadCmdIdle> msg(static_cast<GamepadCmdIdle *>(d));
        if (msg->state && !msg->state->shutting_down.load()) {
            handle_shell_command(msg->state, msg->cmd);
        }
        return G_SOURCE_REMOVE;
    }, new GamepadCmdIdle{state, cmd});
}

}  // namespace

std::vector<std::string> list_supported_power_profiles() {
    std::vector<std::string> out;
    if (!has_executable("powerprofilesctl")) return out;
    std::string raw;
    int rc = 1;
    if (!run_argv_capture({"powerprofilesctl", "list"}, raw, rc) || rc != 0) return out;
    for (const auto &line : split_lines(raw)) {
        std::string s = trim_copy(line);
        if (!s.empty() && s.front() == '*') s = trim_copy(s.substr(1));
        if (s.rfind("power-saver:", 0) == 0) out.emplace_back("power-saver");
        else if (s.rfind("balanced:", 0) == 0) out.emplace_back("balanced");
        else if (s.rfind("performance:", 0) == 0) out.emplace_back("performance");
    }
    return out;
}

void push_audio_power_state(AppState *state) {
    if (!state || !state->settings_view) return;
    MediaSnapshot media = collect_media_state();
    std::vector<std::string> supported_profiles = list_supported_power_profiles();

    std::vector<AudioDeviceItem> sinks, sources;
    if (has_executable("wpctl")) {
        parse_wpctl_audio(sinks, sources);
    }

    int mic_vol = 100;
    bool mic_muted = false;
    std::string mic_out = run_cmd_capture("wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>/dev/null");
    auto pos = mic_out.find("Volume:");
    if (pos != std::string::npos) {
        double v = std::atof(mic_out.c_str() + pos + 7);
        mic_vol = std::clamp(static_cast<int>(v * 100.0 + 0.5), 0, 100);
    }
    mic_muted = (mic_out.find("MUTED") != std::string::npos);

    bool zram_active = !trim_copy(run_cmd_capture("swapon --noheadings --show=NAME 2>/dev/null")).empty();
    bool ppd_available = !media.power_profile.empty();
    bool idle_lock = get_idle_lock_enabled();

    std::ostringstream js;
    js << "window.__spinelAudioPowerData && window.__spinelAudioPowerData({"
       << "\"volume\":" << media.volume << ","
       << "\"volume_muted\":" << (media.volume_muted ? "true" : "false") << ","
       << "\"brightness\":" << media.brightness << ","
       << "\"mic_volume\":" << mic_vol << ","
       << "\"mic_muted\":" << (mic_muted ? "true" : "false") << ","
       << "\"battery_pct\":" << media.battery_pct << ","
       << "\"battery_charging\":" << (media.battery_charging ? "true" : "false") << ","
       << "\"power_profile\":\"" << json_escape(media.power_profile) << "\","
       << "\"ppd_available\":" << (ppd_available ? "true" : "false") << ","
       << "\"idle_lock\":" << (idle_lock ? "true" : "false") << ","
       << "\"supported_profiles\":[";
    for (size_t i = 0; i < supported_profiles.size(); ++i) {
        if (i > 0) js << ",";
        js << "\"" << json_escape(supported_profiles[i]) << "\"";
    }
    js << "],"
       << "\"zram_active\":" << (zram_active ? "true" : "false") << ","
       << "\"sinks\":";
    write_audio_items_json(js, sinks);
    js << ",\"sources\":";
    write_audio_items_json(js, sources);
    js << "});";
    eval_js(state->settings_view, js.str());
}

void set_audio_default_node(AppState *state, int id, bool is_sink) {
    if (id <= 0 || !has_executable("wpctl")) return;
    std::string out;
    int rc = 1;
    run_argv_capture({"wpctl", "set-default", std::to_string(id)}, out, rc);
    append_app_log(std::string("[Audio] 切换默认") + (is_sink ? "输出设备" : "麦克风") + " ID=" + std::to_string(id));
    push_audio_power_state(state);
}

void set_mic_volume(AppState *state, int pct) {
    if (!has_executable("wpctl")) return;
    int clamped = std::clamp(pct, 0, 100);
    spawn_argv_detached({"wpctl", "set-volume", "@DEFAULT_AUDIO_SOURCE@", std::to_string(clamped) + "%"});
    (void)state;
}

void toggle_mic_mute(AppState *state) {
    if (!has_executable("wpctl")) return;
    std::string out;
    int rc = 1;
    run_argv_capture({"wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", "toggle"}, out, rc);
    push_audio_power_state(state);
}

void set_power_profile(AppState *state, const std::string &profile) {
    if (profile != "performance" && profile != "balanced" && profile != "power-saver") return;
    if (!has_executable("powerprofilesctl")) return;
    auto supported = list_supported_power_profiles();
    if (!supported.empty() && std::find(supported.begin(), supported.end(), profile) == supported.end()) {
        append_app_log("[Power] 当前硬件/虚拟机电源驱动不支持 '" + profile + "' 档位，维持现有设置");
        push_audio_power_state(state);
        return;
    }
    std::string out;
    int rc = 1;
    bool ok = run_argv_capture({"powerprofilesctl", "set", profile}, out, rc) && rc == 0;
    append_app_log("[Power] 切换性能电源模式 -> " + profile + (ok ? " (成功)" : " (失败: " + out + ")"));
    push_audio_power_state(state);
    native_shell_sync_telemetry(state);
}

bool get_idle_lock_enabled() {
    std::ifstream ifs(sys_internal::config_dir() / "power.conf");
    if (!ifs.is_open()) return false;
    std::string line;
    while (std::getline(ifs, line)) {
        if (trim_copy(line) == "idle_lock=true") return true;
    }
    return false;
}

void set_idle_lock_enabled(AppState *state, bool enabled) {
    fs::path p = sys_internal::config_dir() / "power.conf";
    write_text_file(p.string(), enabled ? "idle_lock=true\n" : "idle_lock=false\n");
    if (has_executable("swayidle")) {
        std::string tmp_out;
        int rc = 0;
        run_argv_capture({"pkill", "-x", "swayidle"}, tmp_out, rc);
        if (enabled) {
            spawn_argv_detached({"swayidle", "-w", "timeout", "900", "spinel-lock", "before-sleep", "spinel-lock"});
        } else {
            spawn_argv_detached({"swayidle", "-w", "before-sleep", "spinel-lock"});
        }
    }
    append_app_log(std::string("[Power] 闲置 15 分钟自动锁屏已") + (enabled ? "开启" : "关闭"));
    push_audio_power_state(state);
}

bool set_system_brightness_pct(int pct) {
    int clamped = std::clamp(pct, 5, 100);
    if (has_executable("brightnessctl")) {
        std::string out;
        int rc = 1;
        return run_argv_capture({"brightnessctl", "-q", "set", std::to_string(clamped) + "%"}, out, rc) && rc == 0;
    }
    std::string cmd_str =
        "for d in /sys/class/backlight/*; do "
        "[ -w \"$d/brightness\" ] && [ -r \"$d/max_brightness\" ] || continue; "
        "max=$(cat \"$d/max_brightness\"); "
        "[ \"$max\" -gt 0 ] || continue; "
        "echo $(( max * " + std::to_string(clamped) + " / 100 )) > \"$d/brightness\" && echo ok; break; done 2>/dev/null";
    std::string out = run_cmd_capture(cmd_str.c_str());
    return trim_copy(out) == "ok";
}

void trigger_media_osd(AppState *state, const std::string &op) {
    if (op == "volume-up" || op == "volume-down" || op == "mute") {
        if (has_executable("wpctl")) {
            std::string out;
            int rc = 1;
            if (op == "volume-up") {
                run_argv_capture({"wpctl", "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", "5%+"}, out, rc);
            } else if (op == "volume-down") {
                run_argv_capture({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-"}, out, rc);
            } else {
                run_argv_capture({"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle"}, out, rc);
            }
        }
        MediaSnapshot media = collect_media_state();
        native_shell_sync_telemetry(state);
        native_shell_show_osd(state, "volume", media.volume, media.volume_muted);
        return;
    }

    if (op == "bright-up" || op == "bright-down") {
        MediaSnapshot before = collect_media_state();
        int cur = before.brightness >= 0 ? before.brightness : 75;
        int next = std::clamp(cur + (op == "bright-up" ? 5 : -5), 5, 100);
        set_system_brightness_pct(next);
        MediaSnapshot after = collect_media_state();
        int shown = after.brightness >= 0 ? after.brightness : next;
        native_shell_sync_telemetry(state);
        native_shell_show_osd(state, "brightness", shown, false);
    }
}

void start_gamepad_hotkey_listener(AppState *state) {
    if (!state || g_gamepad_thread.joinable()) return;
    g_gamepad_stop.store(false);
    g_gamepad_thread = std::thread([state]() {
        int fd = -1;
        bool btn_state[32]{};
        auto last_trigger = std::chrono::steady_clock::now() - std::chrono::seconds(5);

        while (!g_gamepad_stop.load()) {
            if (fd < 0) {
                std::error_code ec;
                for (int idx = 0; idx < 4; ++idx) {
                    std::string dev = "/dev/input/js" + std::to_string(idx);
                    if (!fs::exists(dev, ec)) continue;
                    fd = ::open(dev.c_str(), O_RDONLY | O_NONBLOCK);
                    if (fd >= 0) {
                        std::fill(std::begin(btn_state), std::end(btn_state), false);
                        break;
                    }
                }
                if (fd < 0) {
                    for (int s = 0; s < 12 && !g_gamepad_stop.load(); ++s) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    }
                    continue;
                }
            }

            struct pollfd pfd{fd, POLLIN, 0};
            int pr = ::poll(&pfd, 1, 250);
            if (pr < 0) {
                ::close(fd);
                fd = -1;
                continue;
            }
            if (pr == 0) continue;

            struct js_event ev{};
            ssize_t n = 0;
            bool closed = false;
            while ((n = ::read(fd, &ev, sizeof(ev))) == sizeof(ev)) {
                unsigned char type = static_cast<unsigned char>(ev.type & ~JS_EVENT_INIT);
                if (type == JS_EVENT_BUTTON && ev.number < 32) {
                    bool down = (ev.value != 0);
                    bool rising = down && !btn_state[ev.number];
                    btn_state[ev.number] = down;
                    if (!rising) continue;

                    auto now = std::chrono::steady_clock::now();
                    if (now - last_trigger < std::chrono::milliseconds(700)) continue;

                    // Button 8 is Guide/Xbox/PS home button on standard Linux joydev drivers.
                    // Start(7)+Select(6) also works as fallback chord for generic controllers.
                    if (ev.number == 8 || (btn_state[6] && btn_state[7])) {
                        last_trigger = now;
                        append_app_log("[Gamepad] 检测到手柄 Guide/Home 组合键，呼出主机大屏");
                        dispatch_gamepad_cmd(state, "launch:steam-gamepadui");
                    } else if (btn_state[6] && ev.number == 0) {
                        last_trigger = now;
                        dispatch_gamepad_cmd(state, "games:open");
                    }
                }
            }
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                closed = true;
            }
            if (closed || (pfd.revents & (POLLERR | POLLHUP))) {
                ::close(fd);
                fd = -1;
            }
        }
        if (fd >= 0) ::close(fd);
    });
}

void stop_gamepad_hotkey_listener() {
    g_gamepad_stop.store(true);
    if (g_gamepad_thread.joinable()) {
        g_gamepad_thread.join();
    }
}
