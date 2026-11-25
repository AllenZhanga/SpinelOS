#include "system_control.hpp"
#include "sys_internal.hpp"
#include "common.hpp"
#include "native_shell.hpp"

#include <fcntl.h>
#include <linux/joystick.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
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

std::mutex g_audio_test_mu;
pid_t g_speaker_pid = -1;
std::thread g_speaker_thread;
std::string g_speaker_channel = "idle";
uint64_t g_speaker_gen = 0;

pid_t g_mic_loopback_pid = -1;
pid_t g_mic_rec_pid = -1;
std::thread g_mic_thread;
std::atomic<bool> g_mic_testing{false};
uint64_t g_mic_gen = 0;
std::string g_audio_test_hint;

void kill_and_reap_pid(pid_t pid) {
    if (pid <= 0) return;
    ::kill(pid, SIGTERM);
    int st = 0;
    for (int i = 0; i < 25; ++i) {
        pid_t r = ::waitpid(pid, &st, WNOHANG);
        if (r == pid || r < 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &st, 0);
}

pid_t spawn_argv_pid(const std::vector<std::string> &argv, int *stdout_pipe_fd = nullptr) {
    if (argv.empty()) return -1;
    int fds[2]{-1, -1};
    if (stdout_pipe_fd) {
        if (::pipe2(fds, O_CLOEXEC) != 0) return -1;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        if (fds[0] >= 0) ::close(fds[0]);
        if (fds[1] >= 0) ::close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            if (!stdout_pipe_fd) ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > 2) ::close(devnull);
        }
        if (stdout_pipe_fd) {
            ::close(fds[0]);
            ::dup2(fds[1], STDOUT_FILENO);
            if (fds[1] > 2) ::close(fds[1]);
        }
        std::vector<char *> c_args;
        c_args.reserve(argv.size() + 1);
        for (const auto &s : argv) c_args.push_back(const_cast<char *>(s.c_str()));
        c_args.push_back(nullptr);
        ::execvp(c_args[0], c_args.data());
        ::_exit(127);
    }

    if (stdout_pipe_fd) {
        ::close(fds[1]);
        *stdout_pipe_fd = fds[0];
    }
    return pid;
}

void push_audio_state_async(AppState *state) {
    if (!state || state->shutting_down.load()) return;
    g_idle_add([](gpointer d) -> gboolean {
        auto *st = static_cast<AppState *>(d);
        if (st && !st->shutting_down.load() && st->settings_view) {
            push_audio_power_state(st);
        }
        return G_SOURCE_REMOVE;
    }, state);
}

void stop_speaker_test_locked(std::unique_lock<std::mutex> &lk) {
    ++g_speaker_gen;
    pid_t pid = g_speaker_pid;
    g_speaker_pid = -1;
    g_speaker_channel = "idle";
    std::thread th = std::move(g_speaker_thread);
    lk.unlock();
    if (pid > 0) kill_and_reap_pid(pid);
    if (th.joinable()) th.join();
    lk.lock();
}

void stop_mic_test_locked(std::unique_lock<std::mutex> &lk) {
    ++g_mic_gen;
    g_mic_testing.store(false);
    pid_t loop_pid = g_mic_loopback_pid;
    pid_t rec_pid = g_mic_rec_pid;
    g_mic_loopback_pid = -1;
    g_mic_rec_pid = -1;
    std::thread th = std::move(g_mic_thread);
    lk.unlock();
    if (loop_pid > 0) kill_and_reap_pid(loop_pid);
    if (rec_pid > 0) kill_and_reap_pid(rec_pid);
    if (th.joinable()) th.join();
    lk.lock();
}

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

    std::string spk_ch;
    bool mic_testing = false;
    std::string test_hint;
    {
        std::lock_guard<std::mutex> lk(g_audio_test_mu);
        spk_ch = g_speaker_channel;
        mic_testing = g_mic_testing.load();
        test_hint = g_audio_test_hint;
    }

    std::ostringstream js;
    js << "window.__spinelAudioPowerData && window.__spinelAudioPowerData({"
       << "\"volume\":" << media.volume << ","
       << "\"volume_muted\":" << (media.volume_muted ? "true" : "false") << ","
       << "\"brightness\":" << media.brightness << ","
       << "\"mic_volume\":" << mic_vol << ","
       << "\"mic_muted\":" << (mic_muted ? "true" : "false") << ","
       << "\"speaker_testing\":\"" << json_escape(spk_ch) << "\","
       << "\"mic_testing\":" << (mic_testing ? "true" : "false") << ","
       << "\"audio_test_hint\":\"" << json_escape(test_hint) << "\","
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
    if (g_mic_testing.load()) {
        toggle_mic_test(state, "start");
        return;
    }
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

bool is_valid_audio_test_channel(const std::string &ch) {
    return ch == "left" || ch == "right" || ch == "stereo" || ch == "stop";
}

std::vector<uint8_t> build_speaker_test_wav(const std::string &ch, int sample_rate) {
    if (ch != "left" && ch != "right" && ch != "stereo") return {};
    if (sample_rate < 8000 || sample_rate > 192000) sample_rate = 48000;

    constexpr double kPi = 3.14159265358979323846;
    const double dur_sec = (ch == "stereo") ? 1.05 : 0.90;
    const uint32_t frames = static_cast<uint32_t>(std::round(sample_rate * dur_sec));
    const uint32_t data_bytes = frames * 4;
    const uint32_t riff_bytes = 36 + data_bytes;

    std::vector<uint8_t> wav;
    wav.reserve(44 + data_bytes);
    auto push_u16 = [&](uint16_t v) {
        wav.push_back(static_cast<uint8_t>(v & 0xFF));
        wav.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    auto push_u32 = [&](uint32_t v) {
        wav.push_back(static_cast<uint8_t>(v & 0xFF));
        wav.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        wav.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        wav.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };
    auto push_tag = [&](const char *tag) {
        for (int i = 0; i < 4; ++i) wav.push_back(static_cast<uint8_t>(tag[i]));
    };

    push_tag("RIFF");
    push_u32(riff_bytes);
    push_tag("WAVE");
    push_tag("fmt ");
    push_u32(16);
    push_u16(1);  // PCM
    push_u16(2);  // Stereo
    push_u32(static_cast<uint32_t>(sample_rate));
    push_u32(static_cast<uint32_t>(sample_rate) * 4);
    push_u16(4);
    push_u16(16);
    push_tag("data");
    push_u32(data_bytes);

    auto tone_env = [&](double t, double t0, double len, double freq) -> double {
        if (t < t0 || t >= t0 + len) return 0.0;
        double local = t - t0;
        double atk = std::min(1.0, local / 0.018);
        double dec = std::exp(-local * 4.2);
        double rel = (local > len - 0.05) ? std::max(0.0, (t0 + len - t) / 0.05) : 1.0;
        double osc = 0.82 * std::sin(2.0 * kPi * freq * local) +
                     0.18 * std::sin(2.0 * kPi * (freq * 2.0) * local);
        return atk * dec * rel * osc;
    };

    for (uint32_t i = 0; i < frames; ++i) {
        double t = static_cast<double>(i) / static_cast<double>(sample_rate);
        double sig = 0.0;
        if (ch == "left") {
            sig = tone_env(t, 0.02, 0.42, 523.25) + tone_env(t, 0.36, 0.50, 659.25);
        } else if (ch == "right") {
            sig = tone_env(t, 0.02, 0.42, 659.25) + tone_env(t, 0.36, 0.50, 783.99);
        } else {
            sig = tone_env(t, 0.02, 0.50, 523.25) +
                  tone_env(t, 0.22, 0.55, 659.25) +
                  tone_env(t, 0.44, 0.58, 783.99);
        }
        int16_t pcm = static_cast<int16_t>(std::clamp(sig * 0.42, -0.95, 0.95) * 32767.0);
        int16_t l_pcm = (ch == "right") ? 0 : pcm;
        int16_t r_pcm = (ch == "left") ? 0 : pcm;
        push_u16(static_cast<uint16_t>(l_pcm));
        push_u16(static_cast<uint16_t>(r_pcm));
    }
    return wav;
}

void test_audio_speaker(AppState *state, const std::string &ch_raw) {
    std::string ch = trim_copy(ch_raw);
    if (!is_valid_audio_test_channel(ch)) return;

    std::unique_lock<std::mutex> lk(g_audio_test_mu);
    bool toggle_off = (ch == "stop" || g_speaker_channel == ch);
    stop_speaker_test_locked(lk);
    if (toggle_off) {
        g_audio_test_hint.clear();
        lk.unlock();
        push_audio_power_state(state);
        return;
    }

    auto wav = build_speaker_test_wav(ch, 48000);
    std::string wav_path = "/tmp/spinel-speaker-test-" + std::to_string(::getuid()) + "-" + ch + ".wav";
    {
        std::ofstream ofs(wav_path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            g_audio_test_hint = "无法写入测试音频临时文件";
            lk.unlock();
            push_audio_power_state(state);
            return;
        }
        ofs.write(reinterpret_cast<const char *>(wav.data()), static_cast<std::streamsize>(wav.size()));
    }

    std::vector<std::string> argv;
    if (has_executable("pw-play")) {
        argv = {"pw-play", "--media-role=Test", wav_path};
    } else if (has_executable("paplay")) {
        argv = {"paplay", wav_path};
    } else if (has_executable("aplay")) {
        argv = {"aplay", "-q", wav_path};
    } else {
        g_audio_test_hint = "未找到 pw-play / paplay / aplay 播放工具";
        lk.unlock();
        push_audio_power_state(state);
        return;
    }

    pid_t pid = spawn_argv_pid(argv);
    if (pid <= 0) {
        g_audio_test_hint = "无法启动扬声器测试进程";
        lk.unlock();
        push_audio_power_state(state);
        return;
    }

    uint64_t my_gen = g_speaker_gen;
    g_speaker_pid = pid;
    g_speaker_channel = ch;
    g_audio_test_hint.clear();
    g_speaker_thread = std::thread([state, pid, my_gen]() {
        int st = 0;
        while (::waitpid(pid, &st, WNOHANG) == 0) {
            {
                std::lock_guard<std::mutex> g(g_audio_test_mu);
                if (g_speaker_gen != my_gen || (state && state->shutting_down.load())) return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        {
            std::lock_guard<std::mutex> g(g_audio_test_mu);
            if (g_speaker_gen != my_gen) return;
            if (g_speaker_pid == pid) g_speaker_pid = -1;
            g_speaker_channel = "idle";
        }
        push_audio_state_async(state);
    });
    lk.unlock();
    push_audio_power_state(state);
}

void toggle_mic_test(AppState *state, const std::string &mode_raw) {
    std::string mode = trim_copy(mode_raw);
    std::unique_lock<std::mutex> lk(g_audio_test_mu);
    bool currently_on = g_mic_testing.load();
    bool want_stop = (mode == "stop") || (mode != "start" && currently_on);

    stop_mic_test_locked(lk);
    if (want_stop) {
        g_audio_test_hint.clear();
        lk.unlock();
        sys_internal::post_js_to_main(state, "window.__spinelMicLevel && window.__spinelMicLevel(0);", false);
        push_audio_power_state(state);
        return;
    }

    bool has_loopback = has_executable("pw-loopback");
    bool has_record = has_executable("pw-record");
    if (!has_loopback && !has_record) {
        g_audio_test_hint = "未找到 pw-loopback / pw-record 工具";
        lk.unlock();
        push_audio_power_state(state);
        return;
    }

    pid_t loop_pid = -1;
    if (has_loopback) {
        loop_pid = spawn_argv_pid({
            "pw-loopback",
            "-n", "spinel-mic-test",
            "-l", "20",
            "--capture-props=node.description=SpinelMicCapture",
            "--playback-props=media.role=Communication"
        });
    }

    int meter_fd = -1;
    pid_t rec_pid = -1;
    if (has_record) {
        rec_pid = spawn_argv_pid({
            "pw-record",
            "--format=s16",
            "--rate=16000",
            "--channels=1",
            "-"
        }, &meter_fd);
    }

    if (loop_pid <= 0 && rec_pid <= 0) {
        if (meter_fd >= 0) ::close(meter_fd);
        g_audio_test_hint = "无法启动麦克风回听或采集进程";
        lk.unlock();
        push_audio_power_state(state);
        return;
    }

    uint64_t my_gen = g_mic_gen;
    g_mic_loopback_pid = loop_pid;
    g_mic_rec_pid = rec_pid;
    g_mic_testing.store(true);
    g_audio_test_hint = (loop_pid > 0) ? "" : "已启用电平监测（缺少 pw-loopback，暂无法回放）";
    g_mic_thread = std::thread([state, meter_fd, loop_pid, rec_pid, my_gen]() {
        int16_t buf[960];
        auto last_emit = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);
        int held_pct = 0;

        while (true) {
            {
                std::lock_guard<std::mutex> g(g_audio_test_mu);
                if (g_mic_gen != my_gen || (state && state->shutting_down.load())) break;
                if (loop_pid > 0) {
                    int st = 0;
                    if (::waitpid(loop_pid, &st, WNOHANG) != 0) break;
                }
            }
            if (meter_fd >= 0) {
                struct pollfd pfd{meter_fd, POLLIN, 0};
                int pr = ::poll(&pfd, 1, 60);
                if (pr > 0 && (pfd.revents & POLLIN)) {
                    ssize_t n = ::read(meter_fd, buf, sizeof(buf));
                    if (n <= 0) break;
                    size_t samples = static_cast<size_t>(n) / sizeof(int16_t);
                    double sum_sq = 0.0;
                    int32_t peak = 0;
                    for (size_t i = 0; i < samples; ++i) {
                        int32_t v = buf[i];
                        int32_t av = v < 0 ? -v : v;
                        if (av > peak) peak = av;
                        double f = static_cast<double>(v) / 32768.0;
                        sum_sq += f * f;
                    }
                    double rms = samples > 0 ? std::sqrt(sum_sq / static_cast<double>(samples)) : 0.0;
                    double pk = static_cast<double>(peak) / 32768.0;
                    int inst = static_cast<int>(std::clamp(std::sqrt(std::max(rms * 1.45, pk * 0.55)) * 135.0, 0.0, 100.0));
                    held_pct = (inst >= held_pct) ? inst : static_cast<int>(held_pct * 0.65 + inst * 0.35);

                    auto now = std::chrono::steady_clock::now();
                    if (now - last_emit >= std::chrono::milliseconds(60)) {
                        last_emit = now;
                        sys_internal::post_js_to_main(
                            state,
                            "window.__spinelMicLevel && window.__spinelMicLevel(" + std::to_string(held_pct) + ");",
                            false
                        );
                    }
                } else if (pr < 0 && errno != EINTR) {
                    break;
                } else if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    break;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        if (meter_fd >= 0) ::close(meter_fd);

        bool unexpected_exit = false;
        pid_t rem_loop = -1;
        pid_t rem_rec = -1;
        {
            std::lock_guard<std::mutex> g(g_audio_test_mu);
            if (g_mic_gen == my_gen) {
                unexpected_exit = true;
                g_mic_testing.store(false);
                rem_loop = g_mic_loopback_pid;
                rem_rec = g_mic_rec_pid;
                g_mic_loopback_pid = -1;
                g_mic_rec_pid = -1;
            }
        }
        if (rem_loop > 0) kill_and_reap_pid(rem_loop);
        if (rem_rec > 0) kill_and_reap_pid(rem_rec);
        if (unexpected_exit) {
            sys_internal::post_js_to_main(state, "window.__spinelMicLevel && window.__spinelMicLevel(0);", false);
            push_audio_state_async(state);
        }
        (void)rec_pid;
    });

    lk.unlock();
    append_app_log("[Audio] 启动麦克风实时监听回放测试");
    push_audio_power_state(state);
}

void stop_audio_tests(AppState *state) {
    bool was_mic = false;
    {
        std::unique_lock<std::mutex> lk(g_audio_test_mu);
        was_mic = g_mic_testing.load();
        stop_speaker_test_locked(lk);
        stop_mic_test_locked(lk);
        g_audio_test_hint.clear();
    }
    if (state && !state->shutting_down.load() && state->settings_view && was_mic) {
        sys_internal::post_js_to_main(state, "window.__spinelMicLevel && window.__spinelMicLevel(0);", false);
    }
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
