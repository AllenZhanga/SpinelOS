#include "test_harness.hpp"

namespace spinel_test {

namespace {

void test_parallel_sandbox() {
    std::string norm, err;
    EXPECT(!is_valid_sandbox_mount_dir("/", norm, err));
    EXPECT(!is_valid_sandbox_mount_dir(home_dir(), norm, err));
    EXPECT(!is_valid_sandbox_mount_dir("relative/path", norm, err));

    const fs::path ws = fs::path(home_dir()) / "ai-workspace";
    fs::create_directories(ws);
    EXPECT(is_valid_sandbox_mount_dir(ws.string() + "/", norm, err));
    EXPECT(norm == fs::weakly_canonical(ws).string());

    EXPECT(add_sandbox_mount_dir(ws.string(), true, err));
    SandboxConfig loaded = load_sandbox_config();
    EXPECT(loaded.allow_network);
    EXPECT(loaded.sync_clipboard);
    EXPECT(loaded.perf_mode == "host-priority");
    EXPECT(loaded.rw_dirs.size() == 1);
    EXPECT(loaded.rw_dirs[0] == norm);

    const std::string fake_home = sandbox_home_dir();
    const std::string rt_dir = sandbox_runtime_dir();
    auto argv_shared = build_sandbox_bwrap_argv(loaded, rt_dir, fake_home, home_dir(), 1000, {"spinel-terminal"}, norm);
    auto has_arg = [&](const std::vector<std::string> &v, const std::string &a) {
        return std::find(v.begin(), v.end(), a) != v.end();
    };
    EXPECT(!has_arg(argv_shared, "--unshare-net"));
    EXPECT(has_arg(argv_shared, "/run/user/1000"));
    EXPECT(has_arg(argv_shared, fake_home));
    EXPECT(has_arg(argv_shared, norm));

    loaded.allow_network = false;
    auto argv_offline = build_sandbox_bwrap_argv(loaded, rt_dir, fake_home, home_dir(), 1000, {"chromium"});
    EXPECT(has_arg(argv_offline, "--unshare-net"));

    std::string rc = build_sandbox_rc_xml(loaded);
    EXPECT(rc.find("<labwc_config>") != std::string::npos);
    EXPECT(rc.find("spinel-shell --sandbox-launcher") != std::string::npos);
    EXPECT(has_arg(argv_shared, "SPINEL_SANDBOX_SHELL"));

    EXPECT(remove_sandbox_mount_dir(norm));
    EXPECT(load_sandbox_config().rw_dirs.empty());

    {
        // Default apps are seeded from host-installed executables; start from an explicit empty list for determinism.
        SandboxConfig base = load_sandbox_config();
        base.apps.clear();
        base.apps_configured = true;
        EXPECT(save_sandbox_config(base));
    }
    EXPECT(add_sandbox_app("My Terminal", "launch:terminal", true, err));
    EXPECT(add_sandbox_app("Browser", "launch:browser", false, err));
    auto apps = load_sandbox_config().apps;
    bool found_term = false, found_browser = false;
    for (const auto &a : apps) {
        if (a.cmd == "launch:terminal" && a.auto_start) found_term = true;
        if (a.cmd == "launch:browser" && !a.auto_start) found_browser = true;
    }
    EXPECT(found_term && found_browser);
    EXPECT(set_sandbox_app_autostart("launch:browser", true));
    EXPECT(remove_sandbox_app("launch:terminal"));
    EXPECT(load_sandbox_config().apps.size() == 1);
    EXPECT(load_sandbox_config().apps[0].cmd == "launch:browser" && load_sandbox_config().apps[0].auto_start);

    AppState dummy;
    std::string ipc_rep;
    EXPECT(dispatch_ipc_command(&dummy, "{\"id\":99,\"method\":\"sandbox:state-get\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("\"ok\":true") != std::string::npos);

    AppState sb_state;
    sb_state.sandbox_shell_mode = true;
    ipc_rep.clear();
    EXPECT(!dispatch_ipc_command(&sb_state, "{\"id\":100,\"method\":\"settings:open:display\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("sandbox_mode_restricted") != std::string::npos);
    ipc_rep.clear();
    EXPECT(!dispatch_ipc_command(&sb_state, "{\"id\":101,\"method\":\"power:shutdown\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("sandbox_mode_restricted") != std::string::npos);
    ipc_rep.clear();
    EXPECT(!dispatch_ipc_command(&sb_state, "{\"id\":103,\"method\":\"ota:check\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("sandbox_mode_restricted") != std::string::npos);
    ipc_rep.clear();
    EXPECT(!dispatch_ipc_command(&sb_state, "{\"id\":104,\"method\":\"store:spinel-ota-full\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("sandbox_mode_restricted") != std::string::npos);
    ipc_rep.clear();
    EXPECT(!dispatch_ipc_command(&sb_state, "{\"id\":105,\"method\":\"ota:pick-offline\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("sandbox_mode_restricted") != std::string::npos);
    ipc_rep.clear();
    EXPECT(!dispatch_ipc_command(&sb_state, "{\"id\":106,\"method\":\"store:spinel-ota-offline\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("sandbox_mode_restricted") != std::string::npos);
    ipc_rep.clear();
    EXPECT(dispatch_ipc_command(&sb_state, "{\"id\":102,\"method\":\"launcher:toggle\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("\"ok\":true") != std::string::npos);

    // Regression guard: querying sandbox status must never kill sandbox shell.pid
    {
        std::error_code ec;
        std::string sh_pid_path = sandbox_runtime_dir() + "/shell.pid";
        fs::create_directories(sandbox_runtime_dir(), ec);
        pid_t child = ::fork();
        if (child == 0) {
            for (;;) ::pause();
            _exit(0);
        }
        EXPECT(child > 1);
        std::ofstream pf(sh_pid_path);
        pf << child << "\n";
        pf.close();
        (void)query_sandbox_status();
        int wst = 0;
        EXPECT(::waitpid(child, &wst, WNOHANG) == 0);
        EXPECT(::kill(child, 0) == 0);
        ::kill(child, SIGKILL);
        ::waitpid(child, &wst, 0);
        fs::remove(sh_pid_path, ec);
    }
}

void test_relay_crypto_and_stream() {
    // 1. Ed25519 sign/verify roundtrip & tamper rejection
    std::string pub_hex, err;
    EXPECT(ensure_device_ed25519_key(pub_hex, err));
    EXPECT(pub_hex.size() == 64);
    std::string sig;
    EXPECT(sign_ed25519_hex("sess123|pubabc", sig));
    EXPECT(verify_ed25519_hex(pub_hex, "sess123|pubabc", sig));
    EXPECT(!verify_ed25519_hex(pub_hex, "sess123|tampered", sig));

    // 2. X25519 ECDH agreement & ChaCha20-Poly1305 AEAD
    std::vector<uint8_t> a_priv, b_priv, a_key, b_key;
    std::string a_pub, b_pub;
    EXPECT(generate_x25519_keypair(a_priv, a_pub));
    EXPECT(generate_x25519_keypair(b_priv, b_pub));
    EXPECT(derive_x25519_session_key(a_priv, b_pub, "sid001", a_key));
    EXPECT(derive_x25519_session_key(b_priv, a_pub, "sid001", b_key));
    EXPECT(a_key == b_key && a_key.size() == 32);

    std::vector<uint8_t> plain = {1, 2, 3, 4, 5, 6, 7}, ct, dec;
    EXPECT(aead_encrypt(a_key, 42, 1, plain, ct));
    EXPECT(aead_decrypt(b_key, 42, 1, ct, dec));
    EXPECT(dec == plain);
    ct[0] ^= 0xFF;
    EXPECT(!aead_decrypt(b_key, 42, 1, ct, dec));

    // 3. Tile-Delta zlib video codec roundtrip
    const int w = 128, h = 128;
    std::vector<uint8_t> fb(w * h * 4, 0x20);
    sandbox_stream::TileEncoderState enc;
    sandbox_stream::TileDecoderState dec_st;
    std::vector<uint8_t> pkt;
    EXPECT(sandbox_stream::encode_tile_frame(enc, fb.data(), w, h, w * 4, false, 0, pkt));
    EXPECT(sandbox_stream::decode_tile_frame(dec_st, pkt.data(), pkt.size()));
    EXPECT(dec_st.width == w && dec_st.height == h && dec_st.bgra == fb);

    // Unchanged frame produces 0 bytes
    EXPECT(!sandbox_stream::encode_tile_frame(enc, fb.data(), w, h, w * 4, false, 0, pkt));

    // Modify single pixel -> delta decodes identically
    fb[(70 * w + 70) * 4 + 1] = 0xEE;
    EXPECT(sandbox_stream::encode_tile_frame(enc, fb.data(), w, h, w * 4, false, 0, pkt));
    EXPECT(sandbox_stream::decode_tile_frame(dec_st, pkt.data(), pkt.size()));
    EXPECT(dec_st.bgra == fb);

    // 4. Relay Config, unique device naming & IPC routing
    RelayConfig rc = load_relay_config();
    EXPECT(!rc.device_name.empty());
    std::string fallback_nm = default_device_name_for_id("a1b2c3d4e5f60000");
    EXPECT(!fallback_nm.empty() && fallback_nm != "spinelos");

    rc.server_addr = "127.0.0.1:7443";
    rc.allow_remote = true;
    EXPECT(save_relay_config(rc));
    EXPECT(load_relay_config().server_addr == "127.0.0.1:7443");

    rc.account.clear();
    rc.token.clear();
    save_relay_config(rc);
    std::string rename_err;
    EXPECT(relay_set_device_name("客厅测试机", rename_err));
    RelayConfig renamed = load_relay_config();
    EXPECT(renamed.device_name == "客厅测试机" && renamed.device_name_custom);

    AppState dummy;
    std::string ipc_rep;
    EXPECT(dispatch_ipc_command(&dummy, "{\"id\":101,\"method\":\"relay:state-get\"}", {}, &ipc_rep));
    EXPECT(ipc_rep.find("\"ok\":true") != std::string::npos);
}

void test_ai_os_stage1_atomics() {
    using namespace spinel::ai;
    reset_agent_tracker_for_tests();
    reset_os_atomics_for_tests();

    fs::path ai_tmp = fs::path(home_dir()) / "ai-test-run";
    std::error_code ec;
    fs::create_directories(ai_tmp, ec);
    setenv("SPINEL_AGENT_RUNTIME_DIR", (ai_tmp / "agents").c_str(), 1);
    setenv("SPINEL_TASK_INBOX_FILE", (ai_tmp / "tasks.json").c_str(), 1);
    setenv("SPINEL_AGENT_PERMS_FILE", (ai_tmp / "agent_perms.conf").c_str(), 1);
    setenv("SPINEL_AGENT_BUS_SOCK", (ai_tmp / "agent.sock").c_str(), 1);

    // 1. Permission gate & audit log
    std::string reason;
    EXPECT(check_agent_capability("agent.report", reason) == PermDecision::Allow);
    EXPECT(check_agent_capability("context.clipboard", reason) == PermDecision::Deny);
    EXPECT(check_agent_capability("action.snapshot", reason) == PermDecision::Allow);
    EXPECT(check_agent_capability("action.workspace_focus", reason) == PermDecision::RequireApproval);
    EXPECT(check_agent_capability("unknown.evil", reason) == PermDecision::Deny);

    AgentPermConfig updated_perms;
    EXPECT(update_agent_perms_kv("tier1_clipboard=true\ntier3_policy=deny\n", updated_perms));
    EXPECT(updated_perms.tier1_clipboard == true);
    EXPECT(updated_perms.tier3_policy == Tier3Policy::Deny);
    EXPECT(check_agent_capability("context.clipboard", reason) == PermDecision::Allow);
    EXPECT(check_agent_capability("action.workspace_focus", reason) == PermDecision::Deny);
    EXPECT(update_agent_perms_kv("tier3_policy=ask\n", updated_perms));

    // 2. Agent command line matching & drop-in lifecycle + window binding
    std::string matched;
    EXPECT(match_known_agent_cmdline("/usr/bin/codex --repo /tmp/demo", matched) && matched == "Codex");
    EXPECT(match_known_agent_cmdline("node /opt/antigravity/cli.js", matched) && matched == "Antigravity");
    EXPECT(match_known_agent_cmdline("python3 /opt/DeepSeekHarness/run.py", matched) && matched == "DeepSeekHarness");
    EXPECT(!match_known_agent_cmdline("spinel-agent-ctl report --agent codex", matched));
    EXPECT(!match_known_agent_cmdline("grep codex /tmp/foo", matched));
    EXPECT(!match_known_agent_cmdline("/home/gamer/Downloads/Antigravity-x64/antigravity --type=renderer --app-path=/opt/app.asar", matched));
    EXPECT(!match_known_agent_cmdline("/home/gamer/Downloads/Antigravity-x64/antigravity --type=zygote", matched));
    EXPECT(!match_known_agent_cmdline("/home/gamer/Downloads/Antigravity-x64/antigravity --import=file:///home/gamer/.gemini/antigravity/lib/node_modules/sidecar_sdk/register_loader.js main.mjs", matched));

    // 3. Task Inbox state machine + bound session auto-transition
    AgentTaskItem created_task;
    EXPECT(create_task("修复登录超时重试", "/tmp/SpinelOS", "DeepSeekHarness", "high", "加上指数退避", created_task));
    EXPECT(created_task.status == TaskStatus::Backlog);

    AgentTaskItem claimed;
    EXPECT(claim_task(created_task.id, "ds-sess-1", "DeepSeekHarness", claimed));
    EXPECT(claimed.status == TaskStatus::InProgress);
    EXPECT(claimed.assigned_session_id == "ds-sess-1");

    // 4. Bus dispatch: report Running -> Done automatically promotes bound Task to Review
    std::string r1 = dispatch_agent_bus_json(
        R"({"id":1,"client":"deepseek","method":"agent.report","params":{"id":"ds-sess-1","agent":"DeepSeekHarness","repo":"/tmp/SpinelOS","status":"running","task_id":")" +
        created_task.id + R"("}})");
    EXPECT(r1.find("\"ok\":true") != std::string::npos);

    std::string r2 = dispatch_agent_bus_json(
        R"({"id":2,"client":"deepseek","method":"agent.report","params":{"id":"ds-sess-1","agent":"DeepSeekHarness","status":"done","summary":"单测全部通过","task_id":")" +
        created_task.id + R"("}})");
    EXPECT(r2.find("\"ok\":true") != std::string::npos);

    auto review_list = list_tasks("review");
    EXPECT(review_list.size() == 1 && review_list[0].id == created_task.id);
    EXPECT(review_list[0].result_summary == "单测全部通过");

    AgentTaskItem accepted;
    EXPECT(accept_task(created_task.id, accepted));
    EXPECT(accepted.status == TaskStatus::Done);
    AgentTaskItem reopened;
    EXPECT(reopen_task(created_task.id, reopened));
    EXPECT(reopened.status == TaskStatus::Backlog && reopened.assigned_session_id.empty());
    EXPECT(delete_task(created_task.id));

    // Stale finished session eviction + window binding
    auto list_now = reconcile_agent_sessions(
        static_cast<long long>(std::time(nullptr)), 300, false,
        {{88, "spinel-terminal", "SpinelOS - DeepSeekHarness", true}});
    EXPECT(list_now.size() == 1 && list_now[0].win_id == 88);
    auto list_expired = reconcile_agent_sessions(static_cast<long long>(std::time(nullptr)) + 600, 300, false, {});
    EXPECT(list_expired.empty());

    // Live Antigravity ConversationService discovery + Running->Done transition
    {
        fs::path hub_dir = fs::path(home_dir()) / "agy-daemon-test";
        std::error_code ec;
        fs::create_directories(hub_dir, ec);
        int srv_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT(srv_fd >= 0);
        if (srv_fd >= 0) {
            int opt = 1;
            ::setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            sockaddr_in sa{};
            sa.sin_family = AF_INET;
            sa.sin_port = 0;
            sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            EXPECT(::bind(srv_fd, reinterpret_cast<const sockaddr *>(&sa), sizeof(sa)) == 0);
            socklen_t slen = sizeof(sa);
            EXPECT(::getsockname(srv_fd, reinterpret_cast<sockaddr *>(&sa), &slen) == 0);
            int port = ntohs(sa.sin_port);
            EXPECT(::listen(srv_fd, 8) == 0);

            std::atomic<bool> mock_running{true};
            std::thread srv_thr([&]() {
                for (int req_i = 0; req_i < 2; ++req_i) {
                    int cfd = ::accept(srv_fd, nullptr, nullptr);
                    if (cfd < 0) break;
                    char rbuf[2048];
                    ::recv(cfd, rbuf, sizeof(rbuf), 0);
                    std::string payload = mock_running.load()
                        ? R"({"conversations":[{"id":"conv-run-1","annotations":{"title":"Debug Island"},"state":{"runState":"RUN_STATE_RUNNING","summary":"Inspecting hub"}},{"id":"conv-idle-2","annotations":{"title":"Old Chat"},"state":{"runState":"RUN_STATE_IDLE","summary":"Done"}}]})"
                        : R"({"conversations":[{"id":"conv-run-1","annotations":{"title":"Debug Island"},"state":{"runState":"RUN_STATE_IDLE","summary":"Inspecting hub"}}]})";
                    std::ostringstream resp;
                    resp << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                         << payload.size() << "\r\nConnection: close\r\n\r\n" << payload;
                    std::string wire = resp.str();
                    ::send(cfd, wire.data(), wire.size(), 0);
                    ::close(cfd);
                }
            });

            std::string hub_json = "{\"pid\":" + std::to_string(static_cast<long long>(::getpid())) +
                                   ",\"httpPort\":" + std::to_string(port) +
                                   ",\"csrfToken\":\"tok-123\"}\n";
            EXPECT(write_text_file((hub_dir / "hub_mock.json").string(), hub_json));
            setenv("SPINEL_ANTIGRAVITY_DAEMON_DIR", hub_dir.c_str(), 1);
            reset_agent_tracker_for_tests();

            long long t0 = static_cast<long long>(std::time(nullptr));
            auto active_list = reconcile_agent_sessions(t0, 300, true, {{501, "antigravity", "SpinelOS - Antigravity", true}});
            bool found_running = false;
            bool found_idle = false;
            for (const auto &s : active_list) {
                if (s.id == "agy-conv-run-1" && s.status == AgentSessionStatus::Running && s.title == "Debug Island" && s.win_id == 501) {
                    found_running = true;
                }
                if (s.id == "agy-conv-idle-2") found_idle = true;
            }
            EXPECT(found_running && !found_idle);

            mock_running.store(false);
            auto after_list = reconcile_agent_sessions(t0 + 2, 300, true, {{501, "antigravity", "SpinelOS - Antigravity", true}});
            bool transitioned_done = false;
            for (const auto &s : after_list) {
                if (s.id == "agy-conv-run-1" && s.status == AgentSessionStatus::Done) {
                    transitioned_done = true;
                }
            }
            EXPECT(transitioned_done);

            ::close(srv_fd);
            if (srv_thr.joinable()) srv_thr.join();
            unsetenv("SPINEL_ANTIGRAVITY_DAEMON_DIR");
            reset_agent_tracker_for_tests();
        }
    }

    // 5. Mocked desktop context + AF_UNIX agent_bus socket + interactive approval gate
    DesktopContextHooks mock_ctx;
    mock_ctx.list_windows = []() -> std::vector<WindowBindingRef> {
        return {{77, "org.spinelos.terminal", "Spinel 终端", true}};
    };
    mock_ctx.list_workspaces = []() -> std::vector<WorkspaceBindingRef> {
        return {{0, "工作区 1", true, false}, {1, "工作区 2", false, false}};
    };
    static int focused_ws = 0;
    static uint64_t focused_wid = 0;
    mock_ctx.activate_workspace = [](int ws1) { focused_ws = ws1; return true; };
    mock_ctx.activate_window = [](uint64_t wid) { focused_wid = wid; return true; };
    set_desktop_context_hooks(std::move(mock_ctx));

    std::string bound_sock;
    EXPECT(start_agent_bus_server(&bound_sock));
    EXPECT(is_agent_bus_running());

    std::string ping_resp;
    EXPECT(call_agent_bus_socket(bound_sock, R"({"id":"p1","method":"ping","params":{}})", ping_resp, 2000));
    EXPECT(ping_resp.find("\"pong\":true") != std::string::npos);

    std::string ctx_resp;
    EXPECT(call_agent_bus_socket(bound_sock, R"({"id":"c1","method":"context.desktop","params":{}})", ctx_resp, 2000));
    EXPECT(ctx_resp.find("\"active_workspace\":1") != std::string::npos);
    EXPECT(ctx_resp.find("Spinel 终端") != std::string::npos);

    // Trigger concurrent Tier 3 interactive approval over socket and resolve from a second connection
    std::string focus_resp;
    std::thread requester([&]() {
        call_agent_bus_socket(
            bound_sock,
            R"({"id":"f1","client":"codex","interactive":true,"method":"action.workspace_focus","params":{"workspace":2,"win_id":77,"summary":"切换验证","timeout_ms":5000}})",
            focus_resp, 6000);
    });
    std::string pending_id;
    for (int i = 0; i < 40; ++i) {
        auto pend = list_pending_approvals();
        if (!pend.empty()) {
            pending_id = pend[0].id;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT(!pending_id.empty());
    std::string resolve_resp;
    EXPECT(call_agent_bus_socket(
        bound_sock,
        "{\"id\":\"r1\",\"client\":\"ui\",\"method\":\"approval.resolve\",\"params\":{\"id\":\"" +
            pending_id + "\",\"decision\":\"allow\"}}",
        resolve_resp, 2000));
    EXPECT(resolve_resp.find("\"resolved\":true") != std::string::npos);
    requester.join();
    EXPECT(focus_resp.find("\"focused\":true") != std::string::npos);
    EXPECT(focused_ws == 2 && focused_wid == 77);

    auto audits = tail_agent_audit(20);
    EXPECT(!audits.empty());
    // Audit denoising: internal polling & ping must NOT be written, while external actions & approvals MUST be kept.
    bool saw_ping = false;
    bool saw_settings_poll = false;
    bool saw_external_ctx = false;
    bool saw_approval = false;
    append_agent_audit("settings", "todo.list", PermDecision::Allow, "0");
    append_agent_audit("settings", "perms.get", PermDecision::Allow, "");
    append_agent_audit("settings", "agent.list", PermDecision::Allow, "1");
    audits = tail_agent_audit(30);
    for (const auto &a : audits) {
        if (a.capability == "ping") saw_ping = true;
        if (a.client_id == "settings" && (a.capability == "todo.list" || a.capability == "perms.get" || a.capability == "agent.list")) {
            saw_settings_poll = true;
        }
        if (a.capability == "context.desktop") saw_external_ctx = true;
        if (a.capability == "approval.resolve") saw_approval = true;
    }
    EXPECT(!saw_ping && !saw_settings_poll && saw_external_ctx && saw_approval);

    // MCP auto-install safely merges existing JSON & TOML configs without clobbering sibling entries
    {
        fs::path agy_cfg = fs::path(home_dir()) / ".gemini/config/mcp_config.json";
        EXPECT(write_text_file(agy_cfg.string(),
            "{\n  \"theme\": \"dark\",\n  \"mcpServers\": {\n    \"other-srv\": {\"command\": \"/bin/true\"}\n  }\n}\n"));
        std::string out_p, err;
        EXPECT(install_mcp_client_config("antigravity", out_p, err));
        EXPECT(out_p == agy_cfg.string() && is_mcp_client_installed("antigravity"));
        std::ifstream ifs(agy_cfg);
        std::string merged((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        EXPECT(merged.find("\"theme\": \"dark\"") != std::string::npos);
        EXPECT(merged.find("\"other-srv\"") != std::string::npos);
        EXPECT(merged.find("\"spinel-os\"") != std::string::npos);
        EXPECT(merged.find("\"SPINEL_AGENT_CLIENT\":\"Antigravity\"") != std::string::npos);

        fs::path codex_cfg = fs::path(home_dir()) / ".codex/config.toml";
        EXPECT(write_text_file(codex_cfg.string(), "model = \"gpt-5\"\n\n[mcp_servers.other]\ncommand = \"echo\"\n"));
        EXPECT(install_mcp_client_config("codex", out_p, err));
        EXPECT(out_p == codex_cfg.string() && is_mcp_client_installed("codex"));
        std::ifstream tfs(codex_cfg);
        std::string toml_merged((std::istreambuf_iterator<char>(tfs)), std::istreambuf_iterator<char>());
        EXPECT(toml_merged.find("model = \"gpt-5\"") != std::string::npos);
        EXPECT(toml_merged.find("[mcp_servers.other]") != std::string::npos);
        EXPECT(toml_merged.find("[mcp_servers.spinel_os]") != std::string::npos);
    }

    stop_agent_bus_server();
    EXPECT(!is_agent_bus_running());

    unsetenv("SPINEL_AGENT_RUNTIME_DIR");
    unsetenv("SPINEL_TASK_INBOX_FILE");
    unsetenv("SPINEL_AGENT_PERMS_FILE");
    unsetenv("SPINEL_AGENT_BUS_SOCK");
    reset_agent_tracker_for_tests();
    reset_os_atomics_for_tests();
}

void test_ai_os_stage2_mcp() {
    using namespace spinel::ai;
    reset_agent_tracker_for_tests();
    reset_os_atomics_for_tests();

    fs::path base = fs::path(home_dir()) / ".spinel_ai_stage2_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    setenv("SPINEL_AGENT_RUNTIME_DIR", (base / "rt/agents").c_str(), 1);
    setenv("SPINEL_TASK_INBOX_FILE", (base / "tasks.json").c_str(), 1);
    setenv("SPINEL_AGENT_PERMS_FILE", (base / "agent_perms.conf").c_str(), 1);
    setenv("SPINEL_AGENT_BUS_SOCK", (base / "rt/agent.sock").c_str(), 1);

    AgentPermConfig perms;
    EXPECT(update_agent_perms_kv("tier1_clipboard=false\ntier3_policy=deny\n", perms));

    // Mock desktop compositor context + start live AF_UNIX bus
    DesktopContextHooks mock_ctx;
    mock_ctx.list_windows = []() -> std::vector<WindowBindingRef> {
        return {{99, "codex", "Codex CLI - SpinelOS", true}};
    };
    mock_ctx.list_workspaces = []() -> std::vector<WorkspaceBindingRef> {
        return {{0, "工作区 1", true, false}};
    };
    set_desktop_context_hooks(std::move(mock_ctx));

    std::string bound_sock;
    EXPECT(start_agent_bus_server(&bound_sock));

    // 1. Initialize + notification + ping + tools/list
    std::string init_r = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}})");
    EXPECT(init_r.find("\"spinel-os-mcp\"") != std::string::npos);
    EXPECT(init_r.find("\"protocolVersion\":\"2024-11-05\"") != std::string::npos);

    std::string notif_r = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})");
    EXPECT(notif_r.empty());

    std::string ping_r = dispatch_mcp_jsonrpc(R"({"jsonrpc":"2.0","id":"p","method":"ping"})");
    EXPECT(ping_r.find("\"result\":{}") != std::string::npos);

    std::string list_r = dispatch_mcp_jsonrpc(R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})");
    for (const char *tool : {
             "agent_report", "agent_list", "todo_list", "todo_create", "todo_claim", "todo_complete",
             "desktop_get_context", "clipboard_recent", "screenshot_capture", "doctor_diagnose",
             "logs_tail", "snapshot_checkpoint", "sandbox_launch", "sandbox_status",
             "workspace_focus", "service_control", "proxy_status", "request_user_approval"}) {
        EXPECT(list_r.find(std::string("\"") + tool + "\"") != std::string::npos);
    }

    // 2. End-to-end live MCP -> AF_UNIX bus -> Task Inbox + Agent Tracker flow
    std::string create_r = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"todo_create","arguments":{"title":"通过 MCP 联调 Stage 2","repo":"/tmp/SpinelOS","preferred_agent":"Codex"}}})");
    EXPECT(create_r.find("\"isError\":false") != std::string::npos);
    EXPECT(create_r.find("通过 MCP 联调 Stage 2") != std::string::npos);

    auto backlog = list_tasks("backlog");
    EXPECT(backlog.size() == 1);
    std::string tid = backlog[0].id;

    std::string claim_r = dispatch_mcp_jsonrpc(
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"todo_claim\",\"arguments\":{\"id\":\"" +
        tid + "\",\"session_id\":\"codex-mcp-1\",\"agent\":\"Codex\"}}}");
    EXPECT(claim_r.find("\"isError\":false") != std::string::npos);

    std::string rep_r = dispatch_mcp_jsonrpc(
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"agent_report\",\"arguments\":{\"id\":\"codex-mcp-1\",\"agent\":\"Codex\",\"status\":\"done\",\"summary\":\"MCP 链路验收成功\",\"task_id\":\"" +
        tid + "\"}}}");
    EXPECT(rep_r.find("\"isError\":false") != std::string::npos);

    std::string todo_rev = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"todo_list","arguments":{"status":"review"}}})");
    EXPECT(todo_rev.find("\"isError\":false") != std::string::npos);
    EXPECT(todo_rev.find("MCP 链路验收成功") != std::string::npos);

    // 3. Context / Sandbox / Proxy / Permission gate enforcement via MCP
    std::string ctx_r = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"desktop_get_context","arguments":{}}})");
    EXPECT(ctx_r.find("\"isError\":false") != std::string::npos);
    EXPECT(ctx_r.find("Codex CLI - SpinelOS") != std::string::npos);

    std::string sb_r = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"sandbox_status","arguments":{}}})");
    EXPECT(sb_r.find("\"isError\":false") != std::string::npos);

    std::string clip_denied = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"clipboard_recent","arguments":{"limit":3}}})");
    EXPECT(clip_denied.find("\"isError\":true") != std::string::npos);

    std::string svc_denied = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"service_control","arguments":{"service":"ollama","action":"stop"}}})");
    EXPECT(svc_denied.find("\"isError\":true") != std::string::npos);

    // 4. Multimodal screenshot PNG base64 image return via mocked capture response
    fs::path fake_png = base / "cap-test.png";
    const unsigned char tiny_png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 1};
    {
        std::ofstream pfs(fake_png, std::ios::binary);
        pfs.write(reinterpret_cast<const char *>(tiny_png), sizeof(tiny_png));
    }
    std::string shot_r = dispatch_mcp_jsonrpc(
        R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"screenshot_capture","arguments":{"mode":"full"}}})",
        [&](const std::string &, std::string &resp_out, int) {
            resp_out = "{\"id\":\"mcp\",\"ok\":true,\"decision\":\"allow\",\"result\":{\"path\":\"" +
                       json_escape(fake_png.string()) + "\"}}";
            return true;
        });
    EXPECT(shot_r.find("\"type\":\"image\"") != std::string::npos);
    EXPECT(shot_r.find("\"mimeType\":\"image/png\"") != std::string::npos);
    EXPECT(shot_r.find("iVBORw0K") != std::string::npos);

    // 5. Stdio loop framing: both NDJSON and Content-Length framing
    std::string frame_msg = R"({"jsonrpc":"2.0","id":99,"method":"ping"})";
    std::ostringstream in_stream;
    in_stream << frame_msg << "\n";
    in_stream << "Content-Length: " << frame_msg.size() << "\r\n\r\n" << frame_msg;
    std::istringstream iss(in_stream.str());
    std::ostringstream oss;
    EXPECT(run_mcp_stdio_loop(iss, oss) == 0);
    std::string stdio_out = oss.str();
    EXPECT(stdio_out.find("\"id\":99,\"result\":{}}\nContent-Length: ") != std::string::npos);
    EXPECT(stdio_out.rfind("\"id\":99,\"result\":{}}") > stdio_out.find("Content-Length: "));

    stop_agent_bus_server();
    unsetenv("SPINEL_AGENT_RUNTIME_DIR");
    unsetenv("SPINEL_TASK_INBOX_FILE");
    unsetenv("SPINEL_AGENT_PERMS_FILE");
    unsetenv("SPINEL_AGENT_BUS_SOCK");
    reset_agent_tracker_for_tests();
    reset_os_atomics_for_tests();
}

void test_ai_os_stage3_integration() {
    using namespace spinel::ai;
    reset_agent_tracker_for_tests();
    reset_os_atomics_for_tests();

    fs::path base = fs::path(home_dir()) / ".spinel_ai_stage3_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    setenv("SPINEL_AGENT_RUNTIME_DIR", (base / "rt/agents").c_str(), 1);
    setenv("SPINEL_TASK_INBOX_FILE", (base / "tasks.json").c_str(), 1);
    setenv("SPINEL_AGENT_PERMS_FILE", (base / "agent_perms.conf").c_str(), 1);
    setenv("SPINEL_AGENT_BUS_SOCK", (base / "rt/agent.sock").c_str(), 1);

    // 1. Dynamic Island pill summary formatting across states
    std::string cls;
    EXPECT(format_agent_island_pill_summary({}, {}, "", cls) == "AI" && cls == "idle");
    AgentSessionInfo s1;
    s1.id = "s1";
    s1.agent = "Codex";
    s1.status = AgentSessionStatus::Running;
    EXPECT(format_agent_island_pill_summary({s1}, {}, "", cls) == "Codex · 运行中" && cls == "running");

    AgentSessionInfo s2 = s1;
    s2.id = "s2";
    s2.agent = "DeepSeekHarness";
    EXPECT(format_agent_island_pill_summary({s1, s2}, {}, "", cls) == "2 运行中" && cls == "running");

    ApprovalRequestInfo ap1;
    ap1.id = "apr_1";
    EXPECT(format_agent_island_pill_summary({s1}, {ap1}, "", cls).find("1 待确认") != std::string::npos && cls == "alert");
    EXPECT(format_agent_island_pill_summary({s1}, {ap1}, "✓ Codex · 完成", cls) == "✓ Codex · 完成" && cls == "toast");

    // 2. GameMode <-> vLLM VRAM yield arbitration & persistence
    std::string vram_act;
    EXPECT(evaluate_vram_yield_for_gamemode(true, true, true, vram_act) && vram_act == "stop_vllm");
    EXPECT(!evaluate_vram_yield_for_gamemode(true, false, true, vram_act) && vram_act == "none");
    EXPECT(!evaluate_vram_yield_for_gamemode(false, true, true, vram_act) && vram_act == "none");

    // 3. Work context bundle & consolidated live snapshot JSON
    AgentTaskItem task;
    EXPECT(create_task("Stage 3 联调", "/tmp/SpinelOS", "Antigravity", "high", "检查热路径", task));
    std::string bundle = build_work_context_bundle(task.id);
    EXPECT(bundle.find("Stage 3 联调") != std::string::npos);
    EXPECT(bundle.find("\"windows\":") != std::string::npos);

    std::string snap = build_ai_os_live_snapshot_json();
    EXPECT(snap.find("\"vram_yield_on_gamemode\":true") != std::string::npos);
    EXPECT(snap.find("Stage 3 联调") != std::string::npos);

    // 4. WebUI IPC router routes for AI Settings (ai:perms-set, ai:state-get, ai:copy-snippet:)
    AppState dummy_state;
    IpcHostActions acts;
    std::string reply;
    EXPECT(dispatch_ipc_command(
        &dummy_state,
        R"({"id":701,"method":"ai:perms-set","params":{"tier1_clipboard":"true","vram_yield_on_gamemode":"false"}})",
        acts, &reply));
    EXPECT(reply.find("\"ok\":true") != std::string::npos);
    AgentPermConfig loaded = load_agent_perms();
    EXPECT(loaded.tier1_clipboard == true);
    EXPECT(loaded.vram_yield_on_gamemode == false);

    reply.clear();
    EXPECT(dispatch_ipc_command(&dummy_state, R"({"id":702,"method":"ai:state-get","params":{}})", acts, &reply));
    EXPECT(reply.find("\"ok\":true") != std::string::npos);

    // 5. Spinel Doctor includes AI OS Bus & MCP Gateway health item & Proxy env propagation
    auto doc_checks = run_system_health_checks();
    bool has_ai_doc = std::any_of(doc_checks.begin(), doc_checks.end(), [](const DoctorCheckItem &c) {
        return c.id == "ai_os_mcp" && !c.name.empty();
    });
    EXPECT(has_ai_doc);
    fs::create_directories(fs::path(home_dir()) / ".config/spinelos", ec);
    write_text_file(fs::path(home_dir()) / ".config/spinelos/proxy.conf",
                    "enabled=true\nhttp_proxy=http://127.0.0.1:10801\nno_proxy=localhost,127.0.0.1\n");
    auto proxy_envs = collect_active_proxy_envs();
    bool has_https_p = std::any_of(proxy_envs.begin(), proxy_envs.end(), [](const auto &kv) {
        return kv.first == "HTTPS_PROXY" && kv.second == "http://127.0.0.1:10801";
    });
    EXPECT(has_https_p);
    write_text_file(fs::path(home_dir()) / ".config/spinelos/proxy.conf", "enabled=false\n");

    unsetenv("SPINEL_AGENT_RUNTIME_DIR");
    unsetenv("SPINEL_TASK_INBOX_FILE");
    unsetenv("SPINEL_AGENT_PERMS_FILE");
    unsetenv("SPINEL_AGENT_BUS_SOCK");
    reset_agent_tracker_for_tests();
    reset_os_atomics_for_tests();
}


}  // namespace

void run_sandbox_ai_tests() {
    test_parallel_sandbox();
    test_relay_crypto_and_stream();
    test_ai_os_stage1_atomics();
    test_ai_os_stage2_mcp();
    test_ai_os_stage3_integration();
}

}  // namespace spinel_test
