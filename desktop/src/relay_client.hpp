#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct RelayConfig {
    std::string server_addr;          // e.g. "1.2.3.4:7443"
    int udp_port = 7444;              // STUN UDP port on relay server
    std::string server_fp;            // pinned sha256-<hex> TLS cert fingerprint
    std::string account;
    std::string token;
    std::string device_id;
    std::string device_name;
    bool allow_remote = false;
};

struct RelayPeerDevice {
    std::string device_id;
    std::string name;
    std::string pub_key_hex;
    bool online = false;
    bool allow_remote = false;
    bool sandbox_running = false;
    std::vector<std::string> lan_addrs;
    std::vector<std::string> ipv6_addrs;
    std::string udp_mapped;
    long long last_seen = 0;
};

struct RelayStatus {
    bool configured = false;
    bool logged_in = false;
    bool hostd_running = false;
    std::string account;
    std::string device_id;
    std::string device_name;
    std::string pub_key_hex;
    std::string server_addr;
    std::string server_fp;
    bool allow_remote = false;
};

// Identity & Config
std::string relay_config_path();
std::string relay_identity_key_path();
std::string relay_hostd_pid_file();
RelayConfig load_relay_config();
bool save_relay_config(const RelayConfig &cfg);
bool ensure_device_ed25519_key(std::string &out_pub_hex, std::string &error_out);
bool sign_ed25519_hex(const std::string &message, std::string &out_sig_hex);
bool verify_ed25519_hex(const std::string &pub_hex, const std::string &message, const std::string &sig_hex);

// Ephemeral ECDH & ChaCha20-Poly1305 AEAD helpers (also unit-tested directly)
bool generate_x25519_keypair(std::vector<uint8_t> &priv_32, std::string &pub_hex);
bool derive_x25519_session_key(const std::vector<uint8_t> &my_priv_32,
                               const std::string &peer_pub_hex,
                               const std::string &session_id,
                               std::vector<uint8_t> &out_key_32);
bool aead_encrypt(const std::vector<uint8_t> &key_32,
                  uint64_t seq,
                  uint8_t dir_bit,
                  const std::vector<uint8_t> &plain,
                  std::vector<uint8_t> &out_cipher);
bool aead_decrypt(const std::vector<uint8_t> &key_32,
                  uint64_t seq,
                  uint8_t dir_bit,
                  const std::vector<uint8_t> &cipher,
                  std::vector<uint8_t> &out_plain);

// Local network candidate discovery & hostd lifecycle
void collect_local_candidates(std::vector<std::string> &lan_v4, std::vector<std::string> &global_v6);
bool is_relay_hostd_running();
void ensure_relay_hostd_running(bool enabled);

// High-level control RPCs against spinel-relay
bool relay_login_or_register(bool is_register,
                             const std::string &server_addr,
                             int udp_port,
                             const std::string &server_fp,
                             const std::string &account,
                             const std::string &password,
                             std::string &out_error);
bool relay_create_pair_code(std::string &out_code, long long &out_expires, std::string &out_error);
bool relay_claim_pair_code(const std::string &server_addr,
                           int udp_port,
                           const std::string &server_fp,
                           const std::string &pair_code,
                           std::string &out_error);
bool relay_fetch_devices(std::vector<RelayPeerDevice> &out_devices, std::string &out_error);
bool relay_remove_device(const std::string &target_device_id, std::string &out_error);
void relay_logout();
std::string relay_state_json();

// Persistent hostd signaling loop handler types
struct IncomingPunchOffer {
    std::string from_device_id;
    std::string from_pub_hex;
    std::string session_id;
    std::vector<std::string> candidates;
    std::string ephemeral_pub;
    std::string signature;
};

// Unified encrypted P2P/Relay tunnel for media & input packets
class E2eeTunnel {
public:
    ~E2eeTunnel();

    static std::unique_ptr<E2eeTunnel> dial_remote_host(const std::string &target_device_id, std::string &out_error);
    static std::unique_ptr<E2eeTunnel> accept_remote_client(const RelayConfig &cfg,
                                                            void *ctrl_tls_conn,
                                                            const IncomingPunchOffer &offer,
                                                            std::string &out_error);

    bool send_msg(uint8_t msg_type, const void *data, size_t len);
    bool recv_msg(uint8_t &out_type, std::vector<uint8_t> &out_data, int timeout_ms = 500);
    void close();
    bool is_open() const;
    std::string transport_label() const;  // "局域网直连" | "IPv6直连" | "UDP穿透" | "云中继"

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Persistent signaling loop used by `spinel-sandbox hostd`
using RelaySessionHandler = std::function<void(std::unique_ptr<E2eeTunnel>)>;
void run_relay_host_signaling(const std::atomic<bool> &stop_flag, const RelaySessionHandler &on_session);

// Runs the blocking host daemon (`spinel-sandbox hostd`)
int run_sandbox_host_daemon();
