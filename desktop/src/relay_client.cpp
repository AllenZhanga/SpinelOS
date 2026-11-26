#include "relay_client.hpp"

#include "common.hpp"
#include "sandbox_manager.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace {

std::string relay_config_dir() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base = (xdg && *xdg) ? std::string(xdg) : (home_dir() + "/.config");
    return base + "/spinelos";
}

std::string runtime_base_dir() {
    const char *rt = std::getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) return rt;
    return "/tmp";
}

std::string to_hex(const uint8_t *p, size_t n) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(kHex[(p[i] >> 4) & 0xF]);
        s.push_back(kHex[p[i] & 0xF]);
    }
    return s;
}

bool from_hex(const std::string &hex, std::vector<uint8_t> &out) {
    if (hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        int a = nib(hex[i]);
        int b = nib(hex[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(static_cast<uint8_t>((a << 4) | b));
    }
    return true;
}

std::string default_hostname() {
    char buf[128]{};
    if (::gethostname(buf, sizeof(buf) - 1) == 0 && buf[0]) return buf;
    return "SpinelOS";
}

bool split_host_port(const std::string &addr, std::string &host, int &port, int def_port) {
    std::string s = trim_copy(addr);
    if (s.empty()) return false;
    port = def_port;
    if (s.front() == '[') {
        auto rb = s.find(']');
        if (rb == std::string::npos) return false;
        host = s.substr(1, rb - 1);
        if (rb + 1 < s.size() && s[rb + 1] == ':') {
            try { port = std::stoi(s.substr(rb + 2)); } catch (...) { return false; }
        }
        return !host.empty() && port > 0 && port <= 65535;
    }
    auto col = s.rfind(':');
    auto first_col = s.find(':');
    if (col != std::string::npos && col == first_col) {
        host = s.substr(0, col);
        try { port = std::stoi(s.substr(col + 1)); } catch (...) { return false; }
    } else {
        host = s;
    }
    return !host.empty() && port > 0 && port <= 65535;
}

/* ---------------- Compact JSON Value Parser (for Go relay responses) ---------------- */

struct JsonVal {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool bval = false;
    double nval = 0.0;
    std::string sval;
    std::vector<JsonVal> arr;
    std::map<std::string, JsonVal> obj;

    std::string str(const std::string &k, const std::string &def = "") const {
        if (kind != Obj) return def;
        auto it = obj.find(k);
        return (it != obj.end() && it->second.kind == Str) ? it->second.sval : def;
    }
    bool boolean(const std::string &k, bool def = false) const {
        if (kind != Obj) return def;
        auto it = obj.find(k);
        return (it != obj.end() && it->second.kind == Bool) ? it->second.bval : def;
    }
    long long num(const std::string &k, long long def = 0) const {
        if (kind != Obj) return def;
        auto it = obj.find(k);
        return (it != obj.end() && it->second.kind == Num) ? static_cast<long long>(it->second.nval) : def;
    }
    const JsonVal *get(const std::string &k) const {
        if (kind != Obj) return nullptr;
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
};

void j_ws(const std::string &s, size_t &p) {
    while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
}

bool j_parse_str(const std::string &s, size_t &p, std::string &out) {
    if (p >= s.size() || s[p] != '"') return false;
    ++p;
    out.clear();
    while (p < s.size()) {
        char c = s[p++];
        if (c == '"') return true;
        if (c == '\\') {
            if (p >= s.size()) return false;
            char e = s[p++];
            switch (e) {
                case '"': case '\\': case '/': out += e; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (p + 4 > s.size()) return false;
                    unsigned int cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = s[p + i];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned int>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned int>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned int>(h - 'A' + 10);
                        else return false;
                    }
                    p += 4;
                    if (cp < 0x80) out += static_cast<char>(cp);
                    else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out += e; break;
            }
        } else {
            out += c;
        }
    }
    return false;
}

bool j_parse_val(const std::string &s, size_t &p, JsonVal &out, int depth = 0) {
    if (depth > 32) return false;
    j_ws(s, p);
    if (p >= s.size()) return false;
    char c = s[p];
    if (c == '"') {
        out = JsonVal{};
        out.kind = JsonVal::Str;
        return j_parse_str(s, p, out.sval);
    }
    if (c == '{') {
        out = JsonVal{};
        out.kind = JsonVal::Obj;
        ++p;
        j_ws(s, p);
        if (p < s.size() && s[p] == '}') { ++p; return true; }
        while (p < s.size()) {
            j_ws(s, p);
            std::string k;
            if (!j_parse_str(s, p, k)) return false;
            j_ws(s, p);
            if (p >= s.size() || s[p] != ':') return false;
            ++p;
            JsonVal v;
            if (!j_parse_val(s, p, v, depth + 1)) return false;
            out.obj.emplace(std::move(k), std::move(v));
            j_ws(s, p);
            if (p < s.size() && s[p] == ',') { ++p; continue; }
            if (p < s.size() && s[p] == '}') { ++p; return true; }
            return false;
        }
        return false;
    }
    if (c == '[') {
        out = JsonVal{};
        out.kind = JsonVal::Arr;
        ++p;
        j_ws(s, p);
        if (p < s.size() && s[p] == ']') { ++p; return true; }
        while (p < s.size()) {
            JsonVal v;
            if (!j_parse_val(s, p, v, depth + 1)) return false;
            out.arr.push_back(std::move(v));
            j_ws(s, p);
            if (p < s.size() && s[p] == ',') { ++p; continue; }
            if (p < s.size() && s[p] == ']') { ++p; return true; }
            return false;
        }
        return false;
    }
    if (s.compare(p, 4, "true") == 0) { out = JsonVal{}; out.kind = JsonVal::Bool; out.bval = true; p += 4; return true; }
    if (s.compare(p, 5, "false") == 0) { out = JsonVal{}; out.kind = JsonVal::Bool; out.bval = false; p += 5; return true; }
    if (s.compare(p, 4, "null") == 0) { out = JsonVal{}; out.kind = JsonVal::Null; p += 4; return true; }
    size_t st = p;
    if (s[p] == '-') ++p;
    while (p < s.size() && (std::isdigit(static_cast<unsigned char>(s[p])) || s[p] == '.' || s[p] == 'e' || s[p] == 'E' || s[p] == '+' || s[p] == '-')) ++p;
    if (p == st) return false;
    out = JsonVal{};
    out.kind = JsonVal::Num;
    try { out.nval = std::stod(s.substr(st, p - st)); } catch (...) { return false; }
    return true;
}

bool parse_json(const std::string &s, JsonVal &out) {
    size_t p = 0;
    return j_parse_val(s, p, out);
}

/* ---------------- TLS Relay Connection Helper ---------------- */

class TlsRelayConn {
public:
    ~TlsRelayConn() { close(); }

    bool connect_server(RelayConfig &cfg, std::string &err) {
        close();
        std::string host;
        int port = 7443;
        if (!split_host_port(cfg.server_addr, host, port, 7443)) {
            err = "云服务器地址无效，应为 IP:端口（如 1.2.3.4:7443）";
            return false;
        }

        struct addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        struct addrinfo *res = nullptr;
        std::string port_str = std::to_string(port);
        if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
            err = "无法解析服务器地址: " + host;
            return false;
        }

        int sock = -1;
        for (auto *ai = res; ai; ai = ai->ai_next) {
            sock = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
            if (sock < 0) continue;
            struct timeval tv{8, 0};
            ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (::connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) break;
            ::close(sock);
            sock = -1;
        }
        ::freeaddrinfo(res);

        if (sock < 0) {
            err = "无法连接云服务器 " + host + ":" + port_str + "（请检查公网端口放行）";
            return false;
        }

        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) {
            ::close(sock);
            err = "初始化 OpenSSL TLS 上下文失败";
            return false;
        }
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, sock);
        if (SSL_connect(ssl) <= 0) {
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            ::close(sock);
            err = "TLS 握手失败，请确认对方端口为 spinel-relay 服务";
            return false;
        }

        X509 *cert = SSL_get1_peer_certificate(ssl);
        if (!cert) {
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            ::close(sock);
            err = "服务器未提供 TLS 证书";
            return false;
        }
        unsigned char md[EVP_MAX_MD_SIZE]{};
        unsigned int md_len = 0;
        X509_digest(cert, EVP_sha256(), md, &md_len);
        X509_free(cert);
        std::string fp = "sha256-" + to_hex(md, md_len);

        if (cfg.server_fp.empty()) {
            cfg.server_fp = fp;
            save_relay_config(cfg);
        } else if (cfg.server_fp != fp) {
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            ::close(sock);
            err = "服务器 TLS 指纹不匹配，连接被拒绝（可在设置中重置指纹后重试）";
            return false;
        }

        // Switch socket timeout to normal blocking or custom poll
        struct timeval tv{30, 0};
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sock_ = sock;
        ctx_ = ctx;
        ssl_ = ssl;
        return true;
    }

    int fd() const { return sock_; }
    bool is_open() const { return ssl_ != nullptr && sock_ >= 0; }

    bool send_frame(uint8_t ftype, const void *data, size_t len) {
        if (!ssl_ || len > 8u * 1024u * 1024u) return false;
        std::lock_guard<std::mutex> lk(write_mu_);
        uint8_t hdr[5];
        hdr[0] = ftype;
        uint32_t n = static_cast<uint32_t>(len);
        hdr[1] = static_cast<uint8_t>((n >> 24) & 0xFF);
        hdr[2] = static_cast<uint8_t>((n >> 16) & 0xFF);
        hdr[3] = static_cast<uint8_t>((n >> 8) & 0xFF);
        hdr[4] = static_cast<uint8_t>(n & 0xFF);
        if (!ssl_write_all(hdr, 5)) return false;
        if (len > 0 && !ssl_write_all(data, len)) return false;
        return true;
    }

    bool send_control(const std::string &json_str) {
        return send_frame(0x01, json_str.data(), json_str.size());
    }

    bool recv_frame(uint8_t &out_type, std::vector<uint8_t> &out_payload, int timeout_ms = 8000) {
        if (!ssl_ || sock_ < 0) return false;
        if (timeout_ms >= 0 && SSL_pending(ssl_) == 0) {
            struct pollfd pfd{sock_, POLLIN, 0};
            int pr = ::poll(&pfd, 1, timeout_ms);
            if (pr <= 0) return false;
        }
        uint8_t hdr[5]{};
        if (!ssl_read_all(hdr, 5)) return false;
        uint32_t len = (static_cast<uint32_t>(hdr[1]) << 24) |
                       (static_cast<uint32_t>(hdr[2]) << 16) |
                       (static_cast<uint32_t>(hdr[3]) << 8) |
                       static_cast<uint32_t>(hdr[4]);
        if (len > 8u * 1024u * 1024u) return false;
        out_payload.resize(len);
        if (len > 0 && !ssl_read_all(out_payload.data(), len)) return false;
        out_type = hdr[0];
        return true;
    }

    bool rpc(const std::string &json_req, JsonVal &out_resp, std::string &out_err, int timeout_ms = 8000) {
        if (!send_control(json_req)) {
            out_err = "发送控制请求失败";
            return false;
        }
        uint8_t ft = 0;
        std::vector<uint8_t> pl;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!recv_frame(ft, pl, timeout_ms)) {
                out_err = "等待服务器响应超时";
                return false;
            }
            if (ft != 0x01) continue;
            std::string s(pl.begin(), pl.end());
            if (!parse_json(s, out_resp)) {
                out_err = "服务器返回无效 JSON";
                return false;
            }
            if (out_resp.str("type") == "resp") {
                if (!out_resp.boolean("ok", false)) {
                    out_err = out_resp.str("error", "操作失败");
                    return false;
                }
                return true;
            }
        }
        out_err = "服务器响应超时";
        return false;
    }

    void close() {
        std::lock_guard<std::mutex> lk(write_mu_);
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (ctx_) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
        if (sock_ >= 0) {
            ::close(sock_);
            sock_ = -1;
        }
    }

private:
    bool ssl_write_all(const void *buf, size_t n) {
        const uint8_t *p = static_cast<const uint8_t *>(buf);
        size_t off = 0;
        while (off < n) {
            int w = SSL_write(ssl_, p + off, static_cast<int>(n - off));
            if (w <= 0) return false;
            off += static_cast<size_t>(w);
        }
        return true;
    }

    bool ssl_read_all(uint8_t *buf, size_t n) {
        size_t off = 0;
        while (off < n) {
            int r = SSL_read(ssl_, buf + off, static_cast<int>(n - off));
            if (r > 0) {
                off += static_cast<size_t>(r);
                continue;
            }
            int ssl_err = SSL_get_error(ssl_, r);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                struct pollfd pfd{sock_, POLLIN, 0};
                if (::poll(&pfd, 1, 1000) > 0) continue;
            }
            return false;
        }
        return true;
    }

    std::mutex write_mu_;
    int sock_ = -1;
    SSL_CTX *ctx_ = nullptr;
    SSL *ssl_ = nullptr;
};

/* ---------------- Network Probes (STUN + Local IPs) ---------------- */

std::string json_str_array(const std::vector<std::string> &v) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) oss << ",";
        oss << "\"" << json_escape(v[i]) << "\"";
    }
    oss << "]";
    return oss.str();
}

std::string stun_probe_with_socket(int udp_sock, const std::string &server_addr, int udp_port) {
    std::string host;
    int unused = 0;
    if (!split_host_port(server_addr, host, unused, 7443) || udp_sock < 0 || udp_port <= 0) return "";

    struct addrinfo hints{};
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *res = nullptr;
    std::string port_s = std::to_string(udp_port);
    if (::getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0 || !res) return "";

    uint8_t pkt[12];
    std::memcpy(pkt, "SPINELST", 8);
    RAND_bytes(pkt + 8, 4);

    for (auto *ai = res; ai; ai = ai->ai_next) {
        ::sendto(udp_sock, pkt, sizeof(pkt), 0, ai->ai_addr, ai->ai_addrlen);
    }
    ::freeaddrinfo(res);

    struct pollfd pfd{udp_sock, POLLIN, 0};
    if (::poll(&pfd, 1, 700) <= 0) return "";
    char buf[256]{};
    ssize_t n = ::recvfrom(udp_sock, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
    if (n <= 12 || std::memcmp(buf, "SPINELOK", 8) != 0 || std::memcmp(buf + 8, pkt + 8, 4) != 0) return "";
    return std::string(buf + 12, static_cast<size_t>(n - 12));
}

int create_dualstack_udp_socket(uint16_t &out_port) {
    int fd = ::socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        int off = 0;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        struct sockaddr_in6 sa{};
        sa.sin6_family = AF_INET6;
        sa.sin6_addr = in6addr_any;
        sa.sin6_port = 0;
        if (::bind(fd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)) == 0) {
            socklen_t len = sizeof(sa);
            if (::getsockname(fd, reinterpret_cast<struct sockaddr *>(&sa), &len) == 0) {
                out_port = ntohs(sa.sin6_port);
                return fd;
            }
        }
        ::close(fd);
    }
    fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        struct sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = 0;
        if (::bind(fd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)) == 0) {
            socklen_t len = sizeof(sa);
            if (::getsockname(fd, reinterpret_cast<struct sockaddr *>(&sa), &len) == 0) {
                out_port = ntohs(sa.sin_port);
                return fd;
            }
        }
        ::close(fd);
    }
    return -1;
}

bool sockaddr_from_hostport(const std::string &hp, struct sockaddr_storage &out_ss, socklen_t &out_len) {
    std::string host;
    int port = 0;
    if (!split_host_port(hp, host, port, 0) || port <= 0) return false;
    struct in_addr in4{};
    if (::inet_pton(AF_INET, host.c_str(), &in4) == 1) {
        // Use v4-mapped IPv6 if socket is dual-stack IPv6
        std::memset(&out_ss, 0, sizeof(out_ss));
        auto *sa6 = reinterpret_cast<struct sockaddr_in6 *>(&out_ss);
        sa6->sin6_family = AF_INET6;
        sa6->sin6_port = htons(static_cast<uint16_t>(port));
        std::memset(sa6->sin6_addr.s6_addr, 0, 10);
        sa6->sin6_addr.s6_addr[10] = 0xFF;
        sa6->sin6_addr.s6_addr[11] = 0xFF;
        std::memcpy(sa6->sin6_addr.s6_addr + 12, &in4, 4);
        out_len = sizeof(struct sockaddr_in6);
        return true;
    }
    struct in6_addr in6{};
    if (::inet_pton(AF_INET6, host.c_str(), &in6) == 1) {
        std::memset(&out_ss, 0, sizeof(out_ss));
        auto *sa6 = reinterpret_cast<struct sockaddr_in6 *>(&out_ss);
        sa6->sin6_family = AF_INET6;
        sa6->sin6_port = htons(static_cast<uint16_t>(port));
        sa6->sin6_addr = in6;
        out_len = sizeof(struct sockaddr_in6);
        return true;
    }
    return false;
}

std::vector<RelayPeerDevice> parse_devices_array(const JsonVal &resp) {
    std::vector<RelayPeerDevice> out;
    const JsonVal *arr = resp.get("devices");
    if (!arr || arr->kind != JsonVal::Arr) return out;
    for (const auto &el : arr->arr) {
        if (el.kind != JsonVal::Obj) continue;
        RelayPeerDevice d;
        d.device_id = el.str("device_id");
        d.name = el.str("name");
        d.pub_key_hex = el.str("pub_key_hex");
        d.online = el.boolean("online");
        d.allow_remote = el.boolean("allow_remote");
        d.sandbox_running = el.boolean("sandbox_running");
        d.udp_mapped = el.str("udp_mapped");
        d.last_seen = el.num("last_seen");
        if (const JsonVal *la = el.get("lan_addrs"); la && la->kind == JsonVal::Arr) {
            for (const auto &s : la->arr) if (s.kind == JsonVal::Str) d.lan_addrs.push_back(s.sval);
        }
        if (const JsonVal *v6 = el.get("ipv6_addrs"); v6 && v6->kind == JsonVal::Arr) {
            for (const auto &s : v6->arr) if (s.kind == JsonVal::Str) d.ipv6_addrs.push_back(s.sval);
        }
        out.push_back(std::move(d));
    }
    return out;
}

std::string build_device_meta_json(const std::string &type,
                                   const std::string &req_id,
                                   const RelayConfig &cfg,
                                   const std::string &pub_hex,
                                   const std::string &extra_fields = "") {
    std::vector<std::string> lan, v6;
    collect_local_candidates(lan, v6);
    bool sb_running = query_sandbox_status().running;
    std::ostringstream oss;
    oss << "{\"type\":\"" << json_escape(type) << "\""
        << ",\"req_id\":\"" << json_escape(req_id) << "\""
        << ",\"device_id\":\"" << json_escape(cfg.device_id) << "\""
        << ",\"device_name\":\"" << json_escape(cfg.device_name) << "\""
        << ",\"pub_key_hex\":\"" << json_escape(pub_hex) << "\""
        << ",\"allow_remote\":" << (cfg.allow_remote ? "true" : "false")
        << ",\"sandbox_running\":" << (sb_running ? "true" : "false")
        << ",\"lan_addrs\":" << json_str_array(lan)
        << ",\"ipv6_addrs\":" << json_str_array(v6);
    if (!extra_fields.empty()) oss << "," << extra_fields;
    oss << "}";
    return oss.str();
}

}  // namespace

/* ==================== Config & Ed25519 / X25519 / ChaCha20 Crypto ==================== */

std::string relay_config_path() { return relay_config_dir() + "/relay.conf"; }
std::string relay_identity_key_path() { return relay_config_dir() + "/device_id.pem"; }
std::string relay_hostd_pid_file() { return runtime_base_dir() + "/spinel-sandbox-hostd.pid"; }

RelayConfig load_relay_config() {
    RelayConfig cfg;
    cfg.device_name = default_hostname();
    std::ifstream in(relay_config_path());
    if (in) {
        std::string line;
        while (std::getline(in, line)) {
            line = trim_copy(line);
            if (line.empty() || line.front() == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim_copy(line.substr(0, eq));
            std::string v = trim_copy(line.substr(eq + 1));
            if (k == "server_addr") cfg.server_addr = v;
            else if (k == "udp_port") { try { cfg.udp_port = std::stoi(v); } catch (...) {} }
            else if (k == "server_fp") cfg.server_fp = v;
            else if (k == "account") cfg.account = v;
            else if (k == "token") cfg.token = v;
            else if (k == "device_id") cfg.device_id = v;
            else if (k == "device_name" && !v.empty()) cfg.device_name = v;
            else if (k == "allow_remote") cfg.allow_remote = (v == "true" || v == "1");
        }
    }
    if (cfg.device_id.empty()) {
        uint8_t rnd[16]{};
        RAND_bytes(rnd, sizeof(rnd));
        cfg.device_id = to_hex(rnd, sizeof(rnd));
        save_relay_config(cfg);
    }
    if (cfg.udp_port <= 0 || cfg.udp_port > 65535) cfg.udp_port = 7444;
    return cfg;
}

bool save_relay_config(const RelayConfig &cfg) {
    std::error_code ec;
    fs::create_directories(relay_config_dir(), ec);
    ::chmod(relay_config_dir().c_str(), 0700);
    std::ostringstream oss;
    oss << "# SpinelOS Cloud Relay & Continuity Configuration\n";
    oss << "server_addr=" << cfg.server_addr << "\n";
    oss << "udp_port=" << cfg.udp_port << "\n";
    oss << "server_fp=" << cfg.server_fp << "\n";
    oss << "account=" << cfg.account << "\n";
    oss << "token=" << cfg.token << "\n";
    oss << "device_id=" << cfg.device_id << "\n";
    oss << "device_name=" << cfg.device_name << "\n";
    oss << "allow_remote=" << (cfg.allow_remote ? "true" : "false") << "\n";
    bool ok = write_text_file(relay_config_path(), oss.str());
    ::chmod(relay_config_path().c_str(), 0600);
    return ok;
}

bool ensure_device_ed25519_key(std::string &out_pub_hex, std::string &error_out) {
    std::error_code ec;
    fs::create_directories(relay_config_dir(), ec);
    std::string key_path = relay_identity_key_path();
    EVP_PKEY *pkey = nullptr;

    if (fs::exists(key_path, ec)) {
        FILE *fp = std::fopen(key_path.c_str(), "rb");
        if (fp) {
            pkey = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
            std::fclose(fp);
        }
    }
    if (!pkey) {
        EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        if (!pctx || EVP_PKEY_keygen_init(pctx) <= 0 || EVP_PKEY_keygen(pctx, &pkey) <= 0) {
            if (pctx) EVP_PKEY_CTX_free(pctx);
            error_out = "生成 Ed25519 设备密钥失败";
            return false;
        }
        EVP_PKEY_CTX_free(pctx);
        FILE *fp = std::fopen(key_path.c_str(), "wb");
        if (!fp) {
            EVP_PKEY_free(pkey);
            error_out = "无法保存设备密钥文件";
            return false;
        }
        PEM_write_PrivateKey(fp, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        std::fclose(fp);
        ::chmod(key_path.c_str(), 0600);
    }

    uint8_t pub[32]{};
    size_t pub_len = sizeof(pub);
    bool ok = (EVP_PKEY_get_raw_public_key(pkey, pub, &pub_len) == 1 && pub_len == 32);
    EVP_PKEY_free(pkey);
    if (!ok) {
        error_out = "导出 Ed25519 公钥失败";
        return false;
    }
    out_pub_hex = to_hex(pub, 32);
    return true;
}

bool sign_ed25519_hex(const std::string &message, std::string &out_sig_hex) {
    std::string pub, err;
    if (!ensure_device_ed25519_key(pub, err)) return false;
    FILE *fp = std::fopen(relay_identity_key_path().c_str(), "rb");
    if (!fp) return false;
    EVP_PKEY *pkey = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
    std::fclose(fp);
    if (!pkey) return false;

    EVP_MD_CTX *mctx = EVP_MD_CTX_new();
    size_t sig_len = 64;
    uint8_t sig[64]{};
    bool ok = mctx &&
              EVP_DigestSignInit(mctx, nullptr, nullptr, nullptr, pkey) == 1 &&
              EVP_DigestSign(mctx, sig, &sig_len,
                             reinterpret_cast<const uint8_t *>(message.data()), message.size()) == 1;
    if (mctx) EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(pkey);
    if (!ok) return false;
    out_sig_hex = to_hex(sig, sig_len);
    return true;
}

bool verify_ed25519_hex(const std::string &pub_hex, const std::string &message, const std::string &sig_hex) {
    std::vector<uint8_t> pub, sig;
    if (!from_hex(pub_hex, pub) || pub.size() != 32) return false;
    if (!from_hex(sig_hex, sig) || sig.size() != 64) return false;

    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.data(), pub.size());
    if (!pkey) return false;
    EVP_MD_CTX *mctx = EVP_MD_CTX_new();
    bool ok = mctx &&
              EVP_DigestVerifyInit(mctx, nullptr, nullptr, nullptr, pkey) == 1 &&
              EVP_DigestVerify(mctx, sig.data(), sig.size(),
                               reinterpret_cast<const uint8_t *>(message.data()), message.size()) == 1;
    if (mctx) EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(pkey);
    return ok;
}

bool generate_x25519_keypair(std::vector<uint8_t> &priv_32, std::string &pub_hex) {
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    EVP_PKEY *pkey = nullptr;
    if (!pctx || EVP_PKEY_keygen_init(pctx) <= 0 || EVP_PKEY_keygen(pctx, &pkey) <= 0) {
        if (pctx) EVP_PKEY_CTX_free(pctx);
        return false;
    }
    EVP_PKEY_CTX_free(pctx);

    priv_32.resize(32);
    size_t pr_len = 32;
    uint8_t pub[32]{};
    size_t pu_len = 32;
    bool ok = (EVP_PKEY_get_raw_private_key(pkey, priv_32.data(), &pr_len) == 1 && pr_len == 32 &&
               EVP_PKEY_get_raw_public_key(pkey, pub, &pu_len) == 1 && pu_len == 32);
    EVP_PKEY_free(pkey);
    if (!ok) return false;
    pub_hex = to_hex(pub, 32);
    return true;
}

bool derive_x25519_session_key(const std::vector<uint8_t> &my_priv_32,
                               const std::string &peer_pub_hex,
                               const std::string &session_id,
                               std::vector<uint8_t> &out_key_32) {
    std::vector<uint8_t> peer_pub;
    if (my_priv_32.size() != 32 || !from_hex(peer_pub_hex, peer_pub) || peer_pub.size() != 32) return false;

    EVP_PKEY *priv = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, my_priv_32.data(), 32);
    EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer_pub.data(), 32);
    if (!priv || !peer) {
        if (priv) EVP_PKEY_free(priv);
        if (peer) EVP_PKEY_free(peer);
        return false;
    }
    EVP_PKEY_CTX *dctx = EVP_PKEY_CTX_new(priv, nullptr);
    uint8_t shared[32]{};
    size_t slen = 32;
    bool ok = dctx &&
              EVP_PKEY_derive_init(dctx) > 0 &&
              EVP_PKEY_derive_set_peer(dctx, peer) > 0 &&
              EVP_PKEY_derive(dctx, shared, &slen) > 0 &&
              slen == 32;
    if (dctx) EVP_PKEY_CTX_free(dctx);
    EVP_PKEY_free(priv);
    EVP_PKEY_free(peer);
    if (!ok) return false;

    out_key_32.resize(32);
    unsigned int klen = 32;
    EVP_MD_CTX *mctx = EVP_MD_CTX_new();
    const char *label = "spinel-e2ee-v1|";
    bool digest_ok = mctx &&
                     EVP_DigestInit_ex(mctx, EVP_sha256(), nullptr) == 1 &&
                     EVP_DigestUpdate(mctx, label, std::strlen(label)) == 1 &&
                     EVP_DigestUpdate(mctx, shared, 32) == 1 &&
                     EVP_DigestUpdate(mctx, session_id.data(), session_id.size()) == 1 &&
                     EVP_DigestFinal_ex(mctx, out_key_32.data(), &klen) == 1;
    if (mctx) EVP_MD_CTX_free(mctx);
    return digest_ok;
}

namespace {

void build_nonce12(uint64_t seq, uint8_t dir_bit, uint8_t nonce[12]) {
    std::memset(nonce, 0, 12);
    nonce[0] = dir_bit;
    for (int i = 0; i < 8; ++i) {
        nonce[4 + i] = static_cast<uint8_t>((seq >> ((7 - i) * 8)) & 0xFF);
    }
}

}  // namespace

bool aead_encrypt(const std::vector<uint8_t> &key_32,
                  uint64_t seq,
                  uint8_t dir_bit,
                  const std::vector<uint8_t> &plain,
                  std::vector<uint8_t> &out_cipher) {
    if (key_32.size() != 32) return false;
    uint8_t nonce[12];
    build_nonce12(seq, dir_bit, nonce);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    out_cipher.resize(plain.size() + 16);
    int len = 0, total = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1 &&
              EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_32.data(), nonce) == 1 &&
              (plain.empty() || EVP_EncryptUpdate(ctx, out_cipher.data(), &len, plain.data(), static_cast<int>(plain.size())) == 1);
    if (ok) {
        total = len;
        ok = EVP_EncryptFinal_ex(ctx, out_cipher.data() + total, &len) == 1;
        if (ok) {
            total += len;
            ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, out_cipher.data() + total) == 1;
            if (ok) out_cipher.resize(static_cast<size_t>(total) + 16);
        }
    }
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

bool aead_decrypt(const std::vector<uint8_t> &key_32,
                  uint64_t seq,
                  uint8_t dir_bit,
                  const std::vector<uint8_t> &cipher,
                  std::vector<uint8_t> &out_plain) {
    if (key_32.size() != 32 || cipher.size() < 16) return false;
    uint8_t nonce[12];
    build_nonce12(seq, dir_bit, nonce);

    size_t ct_len = cipher.size() - 16;
    const uint8_t *tag = cipher.data() + ct_len;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    out_plain.resize(ct_len);
    int len = 0, total = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1 &&
              EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_32.data(), nonce) == 1 &&
              (ct_len == 0 || EVP_DecryptUpdate(ctx, out_plain.data(), &len, cipher.data(), static_cast<int>(ct_len)) == 1);
    if (ok) {
        total = len;
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, const_cast<uint8_t *>(tag)) == 1 &&
             EVP_DecryptFinal_ex(ctx, out_plain.data() + total, &len) == 1;
        if (ok) out_plain.resize(static_cast<size_t>(total + len));
    }
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

/* ==================== Candidates & Daemon Lifecycle ==================== */

void collect_local_candidates(std::vector<std::string> &lan_v4, std::vector<std::string> &global_v6) {
    lan_v4.clear();
    global_v6.clear();
    struct ifaddrs *ifap = nullptr;
    if (::getifaddrs(&ifap) != 0 || !ifap) return;
    for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            char buf[INET_ADDRSTRLEN]{};
            auto *sa = reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr);
            if (::inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
                uint32_t ip = ntohl(sa->sin_addr.s_addr);
                bool priv = ((ip >> 24) == 10) ||
                            ((ip >> 20) == (172 << 4 | 1)) ||
                            ((ip >> 16) == (192 << 8 | 168));
                if (priv && std::find(lan_v4.begin(), lan_v4.end(), buf) == lan_v4.end()) {
                    lan_v4.emplace_back(buf);
                }
            }
        } else if (ifa->ifa_addr->sa_family == AF_INET6) {
            auto *sa6 = reinterpret_cast<struct sockaddr_in6 *>(ifa->ifa_addr);
            if (!IN6_IS_ADDR_LOOPBACK(&sa6->sin6_addr) &&
                !IN6_IS_ADDR_LINKLOCAL(&sa6->sin6_addr) &&
                (sa6->sin6_addr.s6_addr[0] & 0xE0) == 0x20) {
                char buf[INET6_ADDRSTRLEN]{};
                if (::inet_ntop(AF_INET6, &sa6->sin6_addr, buf, sizeof(buf))) {
                    if (std::find(global_v6.begin(), global_v6.end(), buf) == global_v6.end()) {
                        global_v6.emplace_back(buf);
                    }
                }
            }
        }
    }
    ::freeifaddrs(ifap);
}

bool is_relay_hostd_running() {
    std::string line = trim_copy(read_first_line(relay_hostd_pid_file()));
    if (line.empty()) return false;
    try {
        int pid = std::stoi(line);
        return pid > 1 && (::kill(pid, 0) == 0 || errno == EPERM);
    } catch (...) {
        return false;
    }
}

void ensure_relay_hostd_running(bool enabled) {
    if (!enabled) {
        std::string line = trim_copy(read_first_line(relay_hostd_pid_file()));
        if (!line.empty()) {
            try {
                int pid = std::stoi(line);
                if (pid > 1) ::kill(pid, SIGTERM);
            } catch (...) {}
        }
        std::error_code ec;
        fs::remove(relay_hostd_pid_file(), ec);
        return;
    }
    RelayConfig cfg = load_relay_config();
    if (cfg.server_addr.empty() || cfg.account.empty() || cfg.token.empty()) return;
    if (is_relay_hostd_running()) return;

    std::string bin = has_executable("spinel-sandbox") ? "spinel-sandbox" : "/usr/local/bin/spinel-sandbox";
    spawn_argv_detached({bin, "hostd"});
}

/* ==================== Control RPCs ==================== */

bool relay_login_or_register(bool is_register,
                             const std::string &server_addr,
                             int udp_port,
                             const std::string &server_fp,
                             const std::string &account,
                             const std::string &password,
                             std::string &out_error) {
    RelayConfig cfg = load_relay_config();
    if (!server_addr.empty()) cfg.server_addr = trim_copy(server_addr);
    if (udp_port > 0) cfg.udp_port = udp_port;
    cfg.server_fp = trim_copy(server_fp);

    std::string pub_hex;
    if (!ensure_device_ed25519_key(pub_hex, out_error)) return false;

    TlsRelayConn conn;
    if (!conn.connect_server(cfg, out_error)) return false;

    std::string extra = "\"account\":\"" + json_escape(trim_copy(account)) +
                        "\",\"password\":\"" + json_escape(password) + "\",\"ephemeral\":true";
    std::string req = build_device_meta_json(is_register ? "register" : "login", "auth1", cfg, pub_hex, extra);
    JsonVal resp;
    if (!conn.rpc(req, resp, out_error)) return false;

    cfg.account = resp.str("account", trim_copy(account));
    cfg.token = resp.str("token");
    save_relay_config(cfg);
    return !cfg.token.empty();
}

bool relay_create_pair_code(std::string &out_code, long long &out_expires, std::string &out_error) {
    RelayConfig cfg = load_relay_config();
    if (cfg.server_addr.empty() || cfg.account.empty() || cfg.token.empty()) {
        out_error = "请先登录 Spinel ID";
        return false;
    }
    std::string pub_hex;
    if (!ensure_device_ed25519_key(pub_hex, out_error)) return false;

    TlsRelayConn conn;
    if (!conn.connect_server(cfg, out_error)) return false;

    std::string login_extra = "\"account\":\"" + json_escape(cfg.account) + "\",\"token\":\"" + json_escape(cfg.token) + "\",\"ephemeral\":true";
    JsonVal r;
    if (!conn.rpc(build_device_meta_json("login", "p1", cfg, pub_hex, login_extra), r, out_error)) return false;
    if (!conn.rpc("{\"type\":\"pair_create\",\"req_id\":\"p2\"}", r, out_error)) return false;

    out_code = r.str("pair_code");
    out_expires = r.num("expires_at");
    return !out_code.empty();
}

bool relay_claim_pair_code(const std::string &server_addr,
                           int udp_port,
                           const std::string &server_fp,
                           const std::string &pair_code,
                           std::string &out_error) {
    RelayConfig cfg = load_relay_config();
    if (!server_addr.empty()) cfg.server_addr = trim_copy(server_addr);
    if (udp_port > 0) cfg.udp_port = udp_port;
    cfg.server_fp = trim_copy(server_fp);

    std::string pub_hex;
    if (!ensure_device_ed25519_key(pub_hex, out_error)) return false;

    TlsRelayConn conn;
    if (!conn.connect_server(cfg, out_error)) return false;

    std::string extra = "\"pair_code\":\"" + json_escape(trim_copy(pair_code)) + "\",\"ephemeral\":true";
    JsonVal r;
    if (!conn.rpc(build_device_meta_json("pair_claim", "pc1", cfg, pub_hex, extra), r, out_error)) return false;

    cfg.account = r.str("account");
    cfg.token = r.str("token");
    save_relay_config(cfg);
    return !cfg.token.empty();
}

bool relay_fetch_devices(std::vector<RelayPeerDevice> &out_devices, std::string &out_error) {
    RelayConfig cfg = load_relay_config();
    if (cfg.server_addr.empty() || cfg.account.empty() || cfg.token.empty()) {
        out_error = "尚未登录 Spinel 账号";
        return false;
    }
    std::string pub_hex;
    if (!ensure_device_ed25519_key(pub_hex, out_error)) return false;

    TlsRelayConn conn;
    if (!conn.connect_server(cfg, out_error)) return false;

    std::string extra = "\"account\":\"" + json_escape(cfg.account) + "\",\"token\":\"" + json_escape(cfg.token) + "\",\"ephemeral\":true";
    JsonVal r;
    if (!conn.rpc(build_device_meta_json("login", "dl1", cfg, pub_hex, extra), r, out_error)) return false;
    out_devices = parse_devices_array(r);
    return true;
}

bool relay_remove_device(const std::string &target_device_id, std::string &out_error) {
    RelayConfig cfg = load_relay_config();
    if (cfg.server_addr.empty() || cfg.account.empty() || cfg.token.empty()) {
        out_error = "尚未登录 Spinel 账号";
        return false;
    }
    std::string pub_hex;
    if (!ensure_device_ed25519_key(pub_hex, out_error)) return false;

    TlsRelayConn conn;
    if (!conn.connect_server(cfg, out_error)) return false;
    std::string extra = "\"account\":\"" + json_escape(cfg.account) + "\",\"token\":\"" + json_escape(cfg.token) + "\",\"ephemeral\":true";
    JsonVal r;
    if (!conn.rpc(build_device_meta_json("login", "dr1", cfg, pub_hex, extra), r, out_error)) return false;
    if (!conn.rpc("{\"type\":\"device_remove\",\"req_id\":\"dr2\",\"target_device_id\":\"" +
                      json_escape(target_device_id) + "\"}", r, out_error)) {
        return false;
    }
    return true;
}

void relay_logout() {
    ensure_relay_hostd_running(false);
    RelayConfig cfg = load_relay_config();
    cfg.account.clear();
    cfg.token.clear();
    cfg.allow_remote = false;
    save_relay_config(cfg);
}

std::string relay_state_json() {
    RelayConfig cfg = load_relay_config();
    std::string pub_hex, err;
    ensure_device_ed25519_key(pub_hex, err);
    bool logged = (!cfg.server_addr.empty() && !cfg.account.empty() && !cfg.token.empty());
    bool hostd = is_relay_hostd_running();
    std::ostringstream oss;
    oss << "{"
        << "\"configured\":" << (!cfg.server_addr.empty() ? "true" : "false") << ","
        << "\"logged_in\":" << (logged ? "true" : "false") << ","
        << "\"hostd_running\":" << (hostd ? "true" : "false") << ","
        << "\"server_addr\":\"" << json_escape(cfg.server_addr) << "\","
        << "\"udp_port\":" << cfg.udp_port << ","
        << "\"server_fp\":\"" << json_escape(cfg.server_fp) << "\","
        << "\"account\":\"" << json_escape(cfg.account) << "\","
        << "\"device_id\":\"" << json_escape(cfg.device_id) << "\","
        << "\"device_name\":\"" << json_escape(cfg.device_name) << "\","
        << "\"pub_key_hex\":\"" << json_escape(pub_hex) << "\","
        << "\"allow_remote\":" << (cfg.allow_remote ? "true" : "false")
        << "}";
    return oss.str();
}

/* ==================== E2EE P2P / Relay Tunnel ==================== */

namespace {

constexpr uint8_t PKT_PUNCH_PING = 0xFE;
constexpr uint8_t PKT_PUNCH_PONG = 0xFD;
constexpr size_t UDP_MTU_CHUNK = 1200;

struct FragAccum {
    uint16_t total = 0;
    uint16_t received = 0;
    std::vector<std::vector<uint8_t>> parts;
};

void u64_to_be(uint64_t v, uint8_t *p) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> ((7 - i) * 8)) & 0xFF);
}

uint64_t be_to_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

void u16_to_be(uint16_t v, uint8_t *p) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

uint16_t be_to_u16(const uint8_t *p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

std::vector<std::string> build_punch_candidates(int udp_fd, uint16_t local_port, const RelayConfig &cfg) {
    std::vector<std::string> out;
    std::vector<std::string> lan, v6;
    collect_local_candidates(lan, v6);
    for (const auto &ip : lan) out.push_back(ip + ":" + std::to_string(local_port));
    for (const auto &ip6 : v6) out.push_back("[" + ip6 + "]:" + std::to_string(local_port));
    std::string mapped = stun_probe_with_socket(udp_fd, cfg.server_addr, cfg.udp_port);
    if (!mapped.empty() && std::find(out.begin(), out.end(), mapped) == out.end()) {
        out.push_back(mapped);
    }
    return out;
}

std::string classify_addr_label(const struct sockaddr_storage &ss) {
    if (ss.ss_family == AF_INET6) {
        auto *sa6 = reinterpret_cast<const struct sockaddr_in6 *>(&ss);
        if (IN6_IS_ADDR_V4MAPPED(&sa6->sin6_addr)) {
            uint32_t ip = ntohl(*reinterpret_cast<const uint32_t *>(sa6->sin6_addr.s6_addr + 12));
            bool priv = ((ip >> 24) == 10) || ((ip >> 20) == (172 << 4 | 1)) || ((ip >> 16) == (192 << 8 | 168));
            return priv ? "局域网直连" : "UDP穿透";
        }
        return "IPv6直连";
    }
    return "UDP穿透";
}

}  // namespace

struct E2eeTunnel::Impl {
    bool is_host = false;
    std::vector<uint8_t> sid_16;
    std::vector<uint8_t> key_32;
    uint64_t send_seq = 1;
    std::mutex send_mu;

    bool use_udp = false;
    int udp_fd = -1;
    struct sockaddr_storage peer_addr{};
    socklen_t peer_len = 0;

    std::unique_ptr<TlsRelayConn> relay_conn;
    std::string mode_label = "未连接";
    std::atomic<bool> closed{false};
    std::map<uint64_t, FragAccum> frags;

    bool send_udp_blob(const std::vector<uint8_t> &ct, uint64_t seq) {
        if (udp_fd < 0 || peer_len == 0) return false;
        uint16_t cnt = static_cast<uint16_t>(std::max<size_t>(1, (ct.size() + UDP_MTU_CHUNK - 1) / UDP_MTU_CHUNK));
        uint8_t dir = is_host ? 1 : 0;
        for (uint16_t i = 0; i < cnt; ++i) {
            size_t off = static_cast<size_t>(i) * UDP_MTU_CHUNK;
            size_t chunk = std::min(UDP_MTU_CHUNK, ct.size() - off);
            std::vector<uint8_t> dgram(21 + chunk);
            std::memcpy(dgram.data(), "SPRM", 4);
            std::memcpy(dgram.data() + 4, sid_16.data(), 4);
            u64_to_be(seq, dgram.data() + 8);
            dgram[16] = dir;
            u16_to_be(i, dgram.data() + 17);
            u16_to_be(cnt, dgram.data() + 19);
            if (chunk > 0) std::memcpy(dgram.data() + 21, ct.data() + off, chunk);
            ::sendto(udp_fd, dgram.data(), dgram.size(), 0,
                     reinterpret_cast<struct sockaddr *>(&peer_addr), peer_len);
        }
        return true;
    }

    bool negotiate_udp_punch(const std::vector<std::string> &peer_cands, int budget_ms) {
        if (udp_fd < 0 || peer_cands.empty()) return false;
        std::vector<std::pair<sockaddr_storage, socklen_t>> addrs;
        for (const auto &c : peer_cands) {
            sockaddr_storage ss{};
            socklen_t sl = 0;
            if (sockaddr_from_hostport(c, ss, sl)) addrs.emplace_back(ss, sl);
        }
        if (addrs.empty()) return false;

        std::vector<uint8_t> ping_plain = {PKT_PUNCH_PING};
        std::vector<uint8_t> pong_plain = {PKT_PUNCH_PONG};
        std::vector<uint8_t> ping_ct, pong_ct;
        if (!aead_encrypt(key_32, 0, is_host ? 1 : 0, ping_plain, ping_ct)) return false;
        if (!aead_encrypt(key_32, 0x80000000ULL, is_host ? 1 : 0, pong_plain, pong_ct)) return false;

        auto make_ctrl_dgram = [&](uint64_t s, const std::vector<uint8_t> &ct) {
            std::vector<uint8_t> d(21 + ct.size());
            std::memcpy(d.data(), "SPRM", 4);
            std::memcpy(d.data() + 4, sid_16.data(), 4);
            u64_to_be(s, d.data() + 8);
            d[16] = is_host ? 1 : 0;
            u16_to_be(0, d.data() + 17);
            u16_to_be(1, d.data() + 19);
            std::memcpy(d.data() + 21, ct.data(), ct.size());
            return d;
        };

        auto ping_dg = make_ctrl_dgram(0, ping_ct);
        auto pong_dg = make_ctrl_dgram(0x80000000ULL, pong_ct);
        uint8_t expect_dir = is_host ? 0 : 1;

        auto end_t = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
        bool verified_peer = false;
        while (std::chrono::steady_clock::now() < end_t) {
            for (const auto &[ss, sl] : addrs) {
                ::sendto(udp_fd, ping_dg.data(), ping_dg.size(), 0,
                         reinterpret_cast<const struct sockaddr *>(&ss), sl);
            }
            struct pollfd pfd{udp_fd, POLLIN, 0};
            if (::poll(&pfd, 1, 90) > 0) {
                uint8_t buf[2048];
                sockaddr_storage src{};
                socklen_t srclen = sizeof(src);
                ssize_t n = ::recvfrom(udp_fd, buf, sizeof(buf), 0,
                                       reinterpret_cast<struct sockaddr *>(&src), &srclen);
                if (n > 21 && std::memcmp(buf, "SPRM", 4) == 0 && std::memcmp(buf + 4, sid_16.data(), 4) == 0 &&
                    buf[16] == expect_dir) {
                    uint64_t rseq = be_to_u64(buf + 8);
                    std::vector<uint8_t> ct(buf + 21, buf + n), pt;
                    if (aead_decrypt(key_32, rseq, expect_dir, ct, pt) && !pt.empty()) {
                        if (pt[0] == PKT_PUNCH_PING || pt[0] == PKT_PUNCH_PONG) {
                            peer_addr = src;
                            peer_len = srclen;
                            ::sendto(udp_fd, pong_dg.data(), pong_dg.size(), 0,
                                     reinterpret_cast<struct sockaddr *>(&peer_addr), peer_len);
                            if (pt[0] == PKT_PUNCH_PONG) {
                                verified_peer = true;
                                break;
                            }
                            // Also accept ping once, keep trying briefly for pong confirmation
                            verified_peer = true;
                        }
                    }
                }
            }
        }
        if (verified_peer) {
            use_udp = true;
            mode_label = classify_addr_label(peer_addr);
            return true;
        }
        return false;
    }
};

E2eeTunnel::~E2eeTunnel() { close(); }

bool E2eeTunnel::is_open() const { return impl_ && !impl_->closed.load(); }

std::string E2eeTunnel::transport_label() const { return impl_ ? impl_->mode_label : "未连接"; }

void E2eeTunnel::close() {
    if (!impl_) return;
    impl_->closed.store(true);
    if (impl_->udp_fd >= 0) {
        ::close(impl_->udp_fd);
        impl_->udp_fd = -1;
    }
    if (impl_->relay_conn) {
        impl_->relay_conn->close();
    }
}

bool E2eeTunnel::send_msg(uint8_t msg_type, const void *data, size_t len) {
    if (!is_open()) return false;
    std::vector<uint8_t> plain(1 + len);
    plain[0] = msg_type;
    if (len > 0 && data) std::memcpy(plain.data() + 1, data, len);

    std::lock_guard<std::mutex> lk(impl_->send_mu);
    uint64_t seq = impl_->send_seq++;
    uint8_t dir = impl_->is_host ? 1 : 0;
    std::vector<uint8_t> ct;
    if (!aead_encrypt(impl_->key_32, seq, dir, plain, ct)) return false;

    if (impl_->use_udp) {
        return impl_->send_udp_blob(ct, seq);
    }
    if (impl_->relay_conn) {
        std::vector<uint8_t> frame(16 + 9 + ct.size());
        std::memcpy(frame.data(), impl_->sid_16.data(), 16);
        u64_to_be(seq, frame.data() + 16);
        frame[24] = dir;
        std::memcpy(frame.data() + 25, ct.data(), ct.size());
        return impl_->relay_conn->send_frame(0x02, frame.data(), frame.size());
    }
    return false;
}

bool E2eeTunnel::recv_msg(uint8_t &out_type, std::vector<uint8_t> &out_data, int timeout_ms) {
    if (!is_open()) return false;
    uint8_t expect_dir = impl_->is_host ? 0 : 1;

    if (impl_->use_udp) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, timeout_ms));
        while (!impl_->closed.load()) {
            int rem = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
            if (rem <= 0) return false;
            struct pollfd pfd{impl_->udp_fd, POLLIN, 0};
            if (::poll(&pfd, 1, rem) <= 0) return false;

            uint8_t buf[2048];
            sockaddr_storage src{};
            socklen_t sl = sizeof(src);
            ssize_t n = ::recvfrom(impl_->udp_fd, buf, sizeof(buf), 0,
                                   reinterpret_cast<struct sockaddr *>(&src), &sl);
            if (n <= 21 || std::memcmp(buf, "SPRM", 4) != 0 || std::memcmp(buf + 4, impl_->sid_16.data(), 4) != 0) {
                continue;
            }
            uint64_t seq = be_to_u64(buf + 8);
            uint8_t dir = buf[16];
            uint16_t fidx = be_to_u16(buf + 17);
            uint16_t fcnt = be_to_u16(buf + 19);
            if (dir != expect_dir || fcnt == 0 || fidx >= fcnt || fcnt > 4096) continue;
            if (seq == 0 || seq == 0x80000000ULL) continue;  // late punch packet

            std::vector<uint8_t> ct_full;
            if (fcnt == 1) {
                ct_full.assign(buf + 21, buf + n);
            } else {
                auto &acc = impl_->frags[seq];
                if (acc.total != fcnt) {
                    acc.total = fcnt;
                    acc.received = 0;
                    acc.parts.assign(fcnt, {});
                }
                if (acc.parts[fidx].empty()) {
                    acc.parts[fidx].assign(buf + 21, buf + n);
                    ++acc.received;
                }
                if (acc.received < acc.total) {
                    if (impl_->frags.size() > 16) impl_->frags.erase(impl_->frags.begin());
                    continue;
                }
                for (const auto &part : acc.parts) ct_full.insert(ct_full.end(), part.begin(), part.end());
                impl_->frags.erase(seq);
            }

            std::vector<uint8_t> pt;
            if (!aead_decrypt(impl_->key_32, seq, expect_dir, ct_full, pt) || pt.empty()) continue;
            out_type = pt[0];
            out_data.assign(pt.begin() + 1, pt.end());
            return true;
        }
        return false;
    }

    if (impl_->relay_conn) {
        uint8_t ft = 0;
        std::vector<uint8_t> pl;
        if (!impl_->relay_conn->recv_frame(ft, pl, timeout_ms)) return false;
        if (ft != 0x02 || pl.size() < 16 + 9 + 16) return false;
        if (std::memcmp(pl.data(), impl_->sid_16.data(), 16) != 0) return false;
        uint64_t seq = be_to_u64(pl.data() + 16);
        uint8_t dir = pl[24];
        if (dir != expect_dir) return false;
        std::vector<uint8_t> ct(pl.begin() + 25, pl.end()), pt;
        if (!aead_decrypt(impl_->key_32, seq, expect_dir, ct, pt) || pt.empty()) return false;
        out_type = pt[0];
        out_data.assign(pt.begin() + 1, pt.end());
        return true;
    }
    return false;
}

std::unique_ptr<E2eeTunnel> E2eeTunnel::dial_remote_host(const std::string &target_device_id, std::string &out_error) {
    RelayConfig cfg = load_relay_config();
    if (cfg.server_addr.empty() || cfg.account.empty() || cfg.token.empty()) {
        out_error = "请先在「系统设置 -> 云接力与远程沙箱」登录 Spinel ID";
        return nullptr;
    }
    std::string my_pub_hex;
    if (!ensure_device_ed25519_key(my_pub_hex, out_error)) return nullptr;

    TlsRelayConn ctrl;
    if (!ctrl.connect_server(cfg, out_error)) return nullptr;

    std::string login_ex = "\"account\":\"" + json_escape(cfg.account) + "\",\"token\":\"" + json_escape(cfg.token) + "\",\"ephemeral\":true";
    JsonVal login_resp;
    if (!ctrl.rpc(build_device_meta_json("login", "d1", cfg, my_pub_hex, login_ex), login_resp, out_error)) {
        return nullptr;
    }

    std::string peer_pub_hex;
    bool peer_online = false;
    bool peer_allow = false;
    for (const auto &d : parse_devices_array(login_resp)) {
        if (d.device_id == target_device_id) {
            peer_pub_hex = d.pub_key_hex;
            peer_online = d.online;
            peer_allow = d.allow_remote;
            break;
        }
    }
    if (!peer_online) {
        out_error = "目标设备当前不在线";
        return nullptr;
    }
    if (!peer_allow) {
        out_error = "目标设备未开启「允许远程接管平行沙箱」";
        return nullptr;
    }

    uint16_t uport = 0;
    int ufd = create_dualstack_udp_socket(uport);
    auto my_cands = build_punch_candidates(ufd, uport, cfg);

    uint8_t sid_raw[16]{};
    RAND_bytes(sid_raw, sizeof(sid_raw));
    std::string sid_hex = to_hex(sid_raw, sizeof(sid_raw));

    std::vector<uint8_t> eph_priv;
    std::string eph_pub;
    if (!generate_x25519_keypair(eph_priv, eph_pub)) {
        if (ufd >= 0) ::close(ufd);
        out_error = "生成会话密钥失败";
        return nullptr;
    }
    std::string sig;
    if (!sign_ed25519_hex(sid_hex + "|" + eph_pub, sig)) {
        if (ufd >= 0) ::close(ufd);
        out_error = "签名会话请求失败";
        return nullptr;
    }

    std::ostringstream offer;
    offer << "{\"type\":\"punch_offer\",\"req_id\":\"o1\""
          << ",\"target_device_id\":\"" << json_escape(target_device_id) << "\""
          << ",\"session_id\":\"" << sid_hex << "\""
          << ",\"candidates\":" << json_str_array(my_cands)
          << ",\"ephemeral_pub\":\"" << eph_pub << "\""
          << ",\"signature\":\"" << sig << "\"}";

    if (!ctrl.send_control(offer.str())) {
        if (ufd >= 0) ::close(ufd);
        out_error = "发送连接请求失败";
        return nullptr;
    }

    // Wait for punch_answer forwarded by relay
    JsonVal ans;
    bool got_answer = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        uint8_t ft = 0;
        std::vector<uint8_t> pl;
        if (!ctrl.recv_frame(ft, pl, 1500)) continue;
        if (ft != 0x01) continue;
        JsonVal v;
        if (!parse_json(std::string(pl.begin(), pl.end()), v)) continue;
        std::string tp = v.str("type");
        if (tp == "resp" && v.str("req_id") == "o1" && !v.boolean("ok", false)) {
            if (ufd >= 0) ::close(ufd);
            out_error = v.str("error", "对方拒绝连接或离线");
            return nullptr;
        }
        if (tp == "punch_answer" && v.str("session_id") == sid_hex) {
            ans = std::move(v);
            got_answer = true;
            break;
        }
    }
    ctrl.close();

    if (!got_answer) {
        if (ufd >= 0) ::close(ufd);
        out_error = "等待远程设备响应超时";
        return nullptr;
    }

    std::string peer_eph = ans.str("ephemeral_pub");
    std::string peer_sig = ans.str("signature");
    if (!peer_pub_hex.empty() && !verify_ed25519_hex(peer_pub_hex, sid_hex + "|" + peer_eph, peer_sig)) {
        if (ufd >= 0) ::close(ufd);
        out_error = "远程设备身份签名校验失败（疑似密钥不匹配）";
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->is_host = false;
    impl->sid_16.assign(sid_raw, sid_raw + 16);
    impl->udp_fd = ufd;
    if (!derive_x25519_session_key(eph_priv, peer_eph, sid_hex, impl->key_32)) {
        out_error = "协商端到端会话密钥失败";
        return nullptr;
    }

    std::vector<std::string> peer_cands;
    if (const JsonVal *ca = ans.get("candidates"); ca && ca->kind == JsonVal::Arr) {
        for (const auto &c : ca->arr) if (c.kind == JsonVal::Str) peer_cands.push_back(c.sval);
    }

    if (impl->negotiate_udp_punch(peer_cands, 1200)) {
        auto tun = std::unique_ptr<E2eeTunnel>(new E2eeTunnel());
        tun->impl_ = std::move(impl);
        return tun;
    }

    // Fallback to TLS Relay over server:7443
    if (impl->udp_fd >= 0) {
        ::close(impl->udp_fd);
        impl->udp_fd = -1;
    }
    auto rconn = std::make_unique<TlsRelayConn>();
    if (!rconn->connect_server(cfg, out_error)) return nullptr;
    std::ostringstream join;
    join << "{\"type\":\"relay_join\",\"req_id\":\"rj\",\"session_id\":\"" << sid_hex
         << "\",\"device_id\":\"" << json_escape(cfg.device_id)
         << "\",\"token\":\"" << json_escape(cfg.token)
         << "\",\"target_device_id\":\"" << json_escape(target_device_id) << "\"}";
    JsonVal rresp;
    if (!rconn->rpc(join.str(), rresp, out_error)) return nullptr;

    impl->use_udp = false;
    impl->mode_label = "云中继";
    impl->relay_conn = std::move(rconn);
    auto tun = std::unique_ptr<E2eeTunnel>(new E2eeTunnel());
    tun->impl_ = std::move(impl);
    return tun;
}

std::unique_ptr<E2eeTunnel> E2eeTunnel::accept_remote_client(const RelayConfig &cfg_in,
                                                             void *ctrl_tls_conn,
                                                             const IncomingPunchOffer &offer,
                                                             std::string &out_error) {
    RelayConfig cfg = cfg_in;
    auto *ctrl = static_cast<TlsRelayConn *>(ctrl_tls_conn);
    if (!ctrl || !offer.from_pub_hex.empty()) {
        if (!offer.from_pub_hex.empty() &&
            !verify_ed25519_hex(offer.from_pub_hex, offer.session_id + "|" + offer.ephemeral_pub, offer.signature)) {
            out_error = "主控设备 Ed25519 签名验证失败";
            return nullptr;
        }
    }
    std::vector<uint8_t> sid_raw;
    if (!from_hex(offer.session_id, sid_raw) || sid_raw.size() != 16) {
        out_error = "无效会话 ID";
        return nullptr;
    }

    uint16_t uport = 0;
    int ufd = create_dualstack_udp_socket(uport);
    auto my_cands = build_punch_candidates(ufd, uport, cfg);

    std::vector<uint8_t> eph_priv;
    std::string eph_pub;
    if (!generate_x25519_keypair(eph_priv, eph_pub)) {
        if (ufd >= 0) ::close(ufd);
        return nullptr;
    }
    std::string sig;
    if (!sign_ed25519_hex(offer.session_id + "|" + eph_pub, sig)) {
        if (ufd >= 0) ::close(ufd);
        return nullptr;
    }

    std::ostringstream ans;
    ans << "{\"type\":\"punch_answer\",\"req_id\":\"a1\""
        << ",\"target_device_id\":\"" << json_escape(offer.from_device_id) << "\""
        << ",\"session_id\":\"" << json_escape(offer.session_id) << "\""
        << ",\"candidates\":" << json_str_array(my_cands)
        << ",\"ephemeral_pub\":\"" << eph_pub << "\""
        << ",\"signature\":\"" << sig << "\"}";
    if (ctrl && !ctrl->send_control(ans.str())) {
        if (ufd >= 0) ::close(ufd);
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->is_host = true;
    impl->sid_16 = std::move(sid_raw);
    impl->udp_fd = ufd;
    if (!derive_x25519_session_key(eph_priv, offer.ephemeral_pub, offer.session_id, impl->key_32)) {
        out_error = "派生会话密钥失败";
        return nullptr;
    }

    if (impl->negotiate_udp_punch(offer.candidates, 1200)) {
        auto tun = std::unique_ptr<E2eeTunnel>(new E2eeTunnel());
        tun->impl_ = std::move(impl);
        return tun;
    }

    if (impl->udp_fd >= 0) {
        ::close(impl->udp_fd);
        impl->udp_fd = -1;
    }
    auto rconn = std::make_unique<TlsRelayConn>();
    if (!rconn->connect_server(cfg, out_error)) return nullptr;
    std::ostringstream join;
    join << "{\"type\":\"relay_join\",\"req_id\":\"rj\",\"session_id\":\"" << json_escape(offer.session_id)
         << "\",\"device_id\":\"" << json_escape(cfg.device_id)
         << "\",\"token\":\"" << json_escape(cfg.token)
         << "\",\"target_device_id\":\"" << json_escape(offer.from_device_id) << "\"}";
    JsonVal rresp;
    if (!rconn->rpc(join.str(), rresp, out_error)) return nullptr;

    impl->use_udp = false;
    impl->mode_label = "云中继";
    impl->relay_conn = std::move(rconn);
    auto tun = std::unique_ptr<E2eeTunnel>(new E2eeTunnel());
    tun->impl_ = std::move(impl);
    return tun;
}

void run_relay_host_signaling(const std::atomic<bool> &stop_flag, const RelaySessionHandler &on_session) {
    while (!stop_flag.load()) {
        RelayConfig cfg = load_relay_config();
        std::string pub_hex, err;
        if (!cfg.allow_remote || cfg.server_addr.empty() || cfg.account.empty() || cfg.token.empty() ||
            !ensure_device_ed25519_key(pub_hex, err)) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        TlsRelayConn ctrl;
        if (!ctrl.connect_server(cfg, err)) {
            append_app_log("[RelayHostd] connect failed: " + err);
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        std::string extra = "\"account\":\"" + json_escape(cfg.account) + "\",\"token\":\"" + json_escape(cfg.token) + "\",\"role\":\"hostd\"";
        JsonVal resp;
        if (!ctrl.rpc(build_device_meta_json("login", "hl1", cfg, pub_hex, extra), resp, err)) {
            append_app_log("[RelayHostd] login failed: " + err);
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        (void)ctrl.send_control(build_device_meta_json("heartbeat", "hb0", cfg, pub_hex));

        append_app_log("[RelayHostd] connected to " + cfg.server_addr + " as " + cfg.device_name);
        auto last_hb = std::chrono::steady_clock::now();

        while (!stop_flag.load() && ctrl.is_open()) {
            uint8_t ft = 0;
            std::vector<uint8_t> pl;
            if (ctrl.recv_frame(ft, pl, 1500)) {
                if (ft == 0x01) {
                    JsonVal msg;
                    if (parse_json(std::string(pl.begin(), pl.end()), msg)) {
                        if (msg.str("type") == "punch_offer") {
                            RelayConfig fresh = load_relay_config();
                            if (fresh.allow_remote) {
                                IncomingPunchOffer offer;
                                offer.from_device_id = msg.str("from_device_id");
                                offer.from_pub_hex = msg.str("from_pub_hex");
                                offer.session_id = msg.str("session_id");
                                offer.ephemeral_pub = msg.str("ephemeral_pub");
                                offer.signature = msg.str("signature");
                                if (const JsonVal *ca = msg.get("candidates"); ca && ca->kind == JsonVal::Arr) {
                                    for (const auto &c : ca->arr) if (c.kind == JsonVal::Str) offer.candidates.push_back(c.sval);
                                }
                                std::string aerr;
                                auto tun = E2eeTunnel::accept_remote_client(fresh, &ctrl, offer, aerr);
                                if (tun && on_session) {
                                    std::thread([on_session, t = std::move(tun)]() mutable {
                                        on_session(std::move(t));
                                    }).detach();
                                } else if (!aerr.empty()) {
                                    append_app_log("[RelayHostd] accept session failed: " + aerr);
                                }
                            }
                        }
                    }
                }
            }
            auto now = std::chrono::steady_clock::now();
            if (now - last_hb >= std::chrono::seconds(15)) {
                last_hb = now;
                RelayConfig fresh = load_relay_config();
                if (!fresh.allow_remote) break;
                if (!ctrl.send_control(build_device_meta_json("heartbeat", "hb", fresh, pub_hex))) break;
            }
        }
        ctrl.close();
        if (!stop_flag.load()) std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}
