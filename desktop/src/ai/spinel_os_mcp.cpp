#include "ai/mcp_server.hpp"

#include <signal.h>

#include <iostream>

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    return spinel::ai::run_mcp_stdio_loop(std::cin, std::cout);
}
