#include "ai/agent_bus.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace spinel::ai {

std::string agent_bus_socket_path() {
    if (const char *ov = std::getenv("SPINEL_AGENT_BUS_SOCK"); ov && *ov) {
        return ov;
    }
    const char *xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) {
        return (fs::path(xdg) / "spinelos/agent.sock").string();
    }
    std::error_code ec;
    fs::path run_user = fs::path("/run/user") / std::to_string(::getuid());
    if (fs::is_directory(run_user, ec)) {
        return (run_user / "spinelos/agent.sock").string();
    }
    return "/tmp/spinelos-" + std::to_string(::getuid()) + "/agent.sock";
}

bool call_agent_bus_socket(const std::string &sock_path,
                           const std::string &req_json,
                           std::string &resp_json_out,
                           int timeout_ms) {
    resp_json_out.clear();
    if (sock_path.empty() || sock_path.size() >= sizeof(sockaddr_un::sun_path)) {
        return false;
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path.c_str());

    if (::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }

    std::string payload = req_json;
    if (payload.empty() || payload.back() != '\n') payload.push_back('\n');

    size_t sent = 0;
    while (sent < payload.size()) {
        ssize_t w = ::send(fd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (w <= 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        sent += static_cast<size_t>(w);
    }

    std::string buf;
    char chunk[4096];
    int clamped = std::clamp(timeout_ms, 50, 305000);
    while (true) {
        pollfd pfd{fd, POLLIN, 0};
        int pr = ::poll(&pfd, 1, clamped);
        if (pr <= 0) {
            if (pr < 0 && errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        ssize_t r = ::recv(fd, chunk, sizeof(chunk), 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (r == 0) break;
        buf.append(chunk, static_cast<size_t>(r));
        auto nl = buf.find('\n');
        if (nl != std::string::npos) {
            buf.resize(nl);
            break;
        }
    }
    ::close(fd);
    resp_json_out = std::move(buf);
    return !resp_json_out.empty();
}

}  // namespace spinel::ai
