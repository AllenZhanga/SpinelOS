#pragma once

#include "ai/agent_tracker.hpp"
#include "ai/os_atomics.hpp"

#include <gtk/gtk.h>

#include <string>
#include <vector>

struct NativeShellState;

namespace shell_internal {

/// Compute compact Dynamic Island pill text and visual state class ("idle"|"running"|"alert"|"toast").
std::string format_island_pill_summary(const std::vector<spinel::ai::AgentSessionInfo> &sessions,
                                       const std::vector<spinel::ai::ApprovalRequestInfo> &approvals,
                                       const std::string &toast_text,
                                       std::string &out_state_cls);

/// Build the Top Bar Dynamic Island GtkMenuButton + Popover (placed inside top_bar right box).
GtkWidget *build_agent_island_widget(NativeShellState *ns);

/// Attach GFileMonitor, agent/approval callbacks, and reconcile timer for live updates.
void init_agent_island(NativeShellState *ns);

/// Detach timers, monitors, and callbacks cleanly before tearing down NativeShellState.
void shutdown_agent_island(NativeShellState *ns);

/// Trigger an immediate main-thread refresh of the Dynamic Island pill & open popover.
void agent_island_refresh(NativeShellState *ns);

/// Theme-aware CSS fragment for Dynamic Island pill, toast animation, cards, and Tier 3 approval box.
std::string agent_island_css(const NativeShellState *ns);

}  // namespace shell_internal
