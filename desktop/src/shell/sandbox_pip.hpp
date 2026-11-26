#pragma once

#include "shell_internal.hpp"

#include <string>

struct SandboxPipState;

void init_sandbox_pip(NativeShellState *ns);
void shutdown_sandbox_pip(NativeShellState *ns);
void sandbox_pip_sync_action(NativeShellState *ns, const std::string &action = "sync");
void sandbox_pip_on_theme_changed(NativeShellState *ns);
std::string sandbox_pip_css(const NativeShellState *ns);
bool sandbox_pip_is_running(const NativeShellState *ns);
bool sandbox_pip_is_visible(const NativeShellState *ns);
