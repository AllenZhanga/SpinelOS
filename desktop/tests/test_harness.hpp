#pragma once

#include "ai/agent_bus.hpp"
#include "ai/agent_perms.hpp"
#include "ai/agent_tracker.hpp"
#include "ai/json_util.hpp"
#include "ai/mcp_server.hpp"
#include "ai/os_atomics.hpp"
#include "ai/task_inbox.hpp"
#include "app_store.hpp"
#include "common.hpp"
#include "desktop_apps.hpp"
#include "file_associations.hpp"
#include "ipc_router.hpp"
#include "native_shell.hpp"
#include "relay_client.hpp"
#include "sandbox_manager.hpp"
#include "sandbox_stream_codec.hpp"
#include "shell/sni_tray.hpp"
#include "steam_library.hpp"
#include "system_control.hpp"
#include "temp_memory.hpp"
#include "theme_manager.hpp"
#include "wine_launcher.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace spinel_test {

extern int g_failures;

inline void expect(bool cond, const char *expr, int line, const char *file) {
    if (!cond) {
        std::cerr << "[FAIL] " << file << ":" << line << ": " << expr << "\n";
        ++g_failures;
    }
}

void run_core_tests();
void run_sys_tests();
void run_sandbox_ai_tests();

}  // namespace spinel_test

#define EXPECT(cond) spinel_test::expect((cond), #cond, __LINE__, __FILE__)
