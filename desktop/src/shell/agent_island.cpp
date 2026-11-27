#include "shell/agent_island.hpp"
#include "shell/sandbox_pip.hpp"
#include "shell_internal.hpp"

#include "ai/agent_bus.hpp"
#include "ai/agent_perms.hpp"
#include "ai/agent_tracker.hpp"
#include "ai/os_atomics.hpp"
#include "ai/task_inbox.hpp"
#include "common.hpp"
#include "sandbox_manager.hpp"
#include "system_control.hpp"
#include "window_tracker.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace shell_internal {

namespace {

std::atomic<NativeShellState *> g_island_ns{nullptr};

void show_island_toast(NativeShellState *ns, const std::string &msg, guint duration_ms = 3800) {
    if (!ns || msg.empty()) return;
    ns->ai_toast_text = msg;
    if (ns->ai_toast_timer) {
        g_source_remove(ns->ai_toast_timer);
        ns->ai_toast_timer = 0;
    }
    ns->ai_toast_timer = g_timeout_add(duration_ms, +[](gpointer d) -> gboolean {
        auto *n = static_cast<NativeShellState *>(d);
        if (!n) return G_SOURCE_REMOVE;
        n->ai_toast_timer = 0;
        n->ai_toast_text.clear();
        agent_island_refresh(n);
        return G_SOURCE_REMOVE;
    }, ns);
    agent_island_refresh(ns);
}

void schedule_island_refresh_from_any_thread(const std::string &toast_msg = "") {
    NativeShellState *ns = g_island_ns.load();
    if (!ns) return;
    struct WakeMsg {
        NativeShellState *ns;
        std::string toast;
    };
    auto *pkt = new WakeMsg{ns, toast_msg};
    g_idle_add(+[](gpointer data) -> gboolean {
        auto *p = static_cast<WakeMsg *>(data);
        if (p->ns && g_island_ns.load() == p->ns) {
            if (!p->toast.empty()) {
                show_island_toast(p->ns, p->toast);
            } else {
                agent_island_refresh(p->ns);
            }
        }
        delete p;
        return G_SOURCE_REMOVE;
    }, pkt);
}

void rebuild_ai_popover_contents(NativeShellState *ns,
                                 const std::vector<spinel::ai::AgentSessionInfo> &sessions,
                                 const std::vector<spinel::ai::ApprovalRequestInfo> &approvals) {
    if (!ns || !ns->bar_ai_list_box) return;
    while (GtkWidget *c = gtk_widget_get_first_child(ns->bar_ai_list_box)) {
        gtk_box_remove(GTK_BOX(ns->bar_ai_list_box), c);
    }

    // Popover Header Bar
    {
        GtkWidget *top_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(top_hdr, "spinel-ai-pop-hdr");
        GtkWidget *hdr_lbl = gtk_label_new("Spinel AI 状态中心");
        gtk_widget_add_css_class(hdr_lbl, "spinel-ai-pop-title");
        gtk_widget_set_hexpand(hdr_lbl, TRUE);
        gtk_widget_set_halign(hdr_lbl, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(top_hdr), hdr_lbl);
        gtk_box_append(GTK_BOX(ns->bar_ai_list_box), top_hdr);
    }

    // Section 1: Pending Tier 3 Interactive Approvals
    if (!approvals.empty()) {
        GtkWidget *ap_head = gtk_label_new("待人工审批 (Tier 3 权限门禁)");
        gtk_widget_add_css_class(ap_head, "spinel-ai-sec-title");
        gtk_widget_add_css_class(ap_head, "is-alert");
        gtk_widget_set_halign(ap_head, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(ns->bar_ai_list_box), ap_head);

        for (const auto &apr : approvals) {
            GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
            gtk_widget_add_css_class(card, "spinel-ai-apr-card");

            std::string title_txt = apr.client_id + " 请求 " + apr.capability;
            GtkWidget *t_lbl = gtk_label_new(title_txt.c_str());
            gtk_widget_add_css_class(t_lbl, "spinel-ai-item-title");
            gtk_widget_set_halign(t_lbl, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(t_lbl), PANGO_ELLIPSIZE_END);
            gtk_box_append(GTK_BOX(card), t_lbl);

            if (!apr.summary.empty()) {
                GtkWidget *s_lbl = gtk_label_new(apr.summary.c_str());
                gtk_widget_add_css_class(s_lbl, "spinel-ai-item-sub");
                gtk_widget_set_halign(s_lbl, GTK_ALIGN_START);
                gtk_label_set_wrap(GTK_LABEL(s_lbl), TRUE);
                gtk_box_append(GTK_BOX(card), s_lbl);
            }
            if (!apr.detail.empty() && apr.detail != "{}") {
                GtkWidget *d_lbl = gtk_label_new(apr.detail.c_str());
                gtk_widget_add_css_class(d_lbl, "spinel-ai-mono");
                gtk_widget_set_halign(d_lbl, GTK_ALIGN_START);
                gtk_label_set_ellipsize(GTK_LABEL(d_lbl), PANGO_ELLIPSIZE_END);
                gtk_label_set_max_width_chars(GTK_LABEL(d_lbl), 44);
                gtk_box_append(GTK_BOX(card), d_lbl);
            }

            GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
            auto add_dec_btn = [&](const char *lbl, const char *cls, const char *dec) {
                GtkWidget *b = gtk_button_new_with_label(lbl);
                gtk_widget_add_css_class(b, "spinel-ai-mini-btn");
                if (cls) gtk_widget_add_css_class(b, cls);
                g_object_set_data_full(G_OBJECT(b), "apr-id", g_strdup(apr.id.c_str()), g_free);
                g_object_set_data_full(G_OBJECT(b), "apr-dec", g_strdup(dec), g_free);
                g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton *btn, gpointer d) {
                    auto *n = static_cast<NativeShellState *>(d);
                    const char *id = static_cast<const char *>(g_object_get_data(G_OBJECT(btn), "apr-id"));
                    const char *decision = static_cast<const char *>(g_object_get_data(G_OBJECT(btn), "apr-dec"));
                    if (!id || !decision) return;
                    if (std::string(decision) == "snapshot_allow") {
                        std::string aid = id;
                        std::thread([aid]() {
                            std::string out_json, err;
                            if (spinel::ai::create_guarded_snapshot("AI 审批前保护点", out_json, err)) {
                                spinel::ai::resolve_pending_approval(aid, "allow");
                                schedule_island_refresh_from_any_thread("✓ 已创建还原点并放行");
                            } else {
                                schedule_island_refresh_from_any_thread("⚠ 快照创建失败，请改选允许或拒绝");
                            }
                        }).detach();
                    } else {
                        spinel::ai::resolve_pending_approval(id, decision);
                        agent_island_refresh(n);
                    }
                }), ns);
                gtk_box_append(GTK_BOX(btn_row), b);
            };
            add_dec_btn("允许", "is-primary", "allow");
            add_dec_btn("快照后允许", nullptr, "snapshot_allow");
            add_dec_btn("沙箱", nullptr, "sandbox");
            add_dec_btn("拒绝", "is-danger", "deny");
            gtk_box_append(GTK_BOX(card), btn_row);
            gtk_box_append(GTK_BOX(ns->bar_ai_list_box), card);
        }
        gtk_box_append(GTK_BOX(ns->bar_ai_list_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    }

    // Section 2: Active & Recent Agent Sessions
    {
        GtkWidget *sec_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *sec_lbl = gtk_label_new("活动 AI 会话");
        gtk_widget_add_css_class(sec_lbl, "spinel-ai-sec-title");
        gtk_widget_set_hexpand(sec_lbl, TRUE);
        gtk_widget_set_halign(sec_lbl, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(sec_head), sec_lbl);

        GtkWidget *clr_btn = gtk_button_new_with_label("清理已结束");
        gtk_widget_add_css_class(clr_btn, "spinel-clip-clr");
        g_signal_connect(clr_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
            auto *n = static_cast<NativeShellState *>(d);
            spinel::ai::clear_agent_session("");
            agent_island_refresh(n);
        }), ns);
        gtk_box_append(GTK_BOX(sec_head), clr_btn);
        gtk_box_append(GTK_BOX(ns->bar_ai_list_box), sec_head);
    }

    if (sessions.empty()) {
        GtkWidget *empty = gtk_label_new("暂无活跃 Agent 会话\n（支持 Codex / Antigravity / Claude / DeepSeekHarness 自动感知）");
        gtk_widget_add_css_class(empty, "spinel-clip-empty");
        gtk_label_set_justify(GTK_LABEL(empty), GTK_JUSTIFY_CENTER);
        gtk_box_append(GTK_BOX(ns->bar_ai_list_box), empty);
    } else {
        for (const auto &s : sessions) {
            GtkWidget *btn = gtk_button_new();
            gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
            gtk_widget_add_css_class(btn, "spinel-clip-row");
            gtk_widget_add_css_class(btn, "spinel-ai-row");

            GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

            const char *st_glyph = "●";
            const char *st_cls = "st-run";
            if (s.status == spinel::ai::AgentSessionStatus::WaitingUser) {
                st_glyph = "⚠";
                st_cls = "st-wait";
            } else if (s.status == spinel::ai::AgentSessionStatus::Done) {
                st_glyph = "✓";
                st_cls = "st-done";
            } else if (s.status == spinel::ai::AgentSessionStatus::Error) {
                st_glyph = "✕";
                st_cls = "st-err";
            }
            GtkWidget *dot = gtk_label_new(st_glyph);
            gtk_widget_add_css_class(dot, "spinel-ai-dot");
            gtk_widget_add_css_class(dot, st_cls);
            gtk_box_append(GTK_BOX(top), dot);

            std::string title = s.agent;
            if (!s.title.empty() && s.title != s.agent) title += " · " + s.title;
            else if (!s.repo_path.empty()) title += " · " + fs::path(s.repo_path).filename().string();
            GtkWidget *t_lbl = gtk_label_new(title.c_str());
            gtk_widget_add_css_class(t_lbl, "spinel-ai-item-title");
            gtk_widget_set_hexpand(t_lbl, TRUE);
            gtk_widget_set_halign(t_lbl, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(t_lbl), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(t_lbl), 30);
            gtk_box_append(GTK_BOX(top), t_lbl);

            if (s.in_sandbox) {
                GtkWidget *sb_tag = gtk_label_new("沙箱 PiP");
                gtk_widget_add_css_class(sb_tag, "spinel-ai-badge");
                gtk_box_append(GTK_BOX(top), sb_tag);
            } else if (s.win_id > 0) {
                GtkWidget *jmp = gtk_label_new("切窗 ›");
                gtk_widget_add_css_class(jmp, "spinel-clip-meta");
                gtk_box_append(GTK_BOX(top), jmp);
            }
            gtk_box_append(GTK_BOX(v), top);

            if (!s.summary.empty()) {
                GtkWidget *sum = gtk_label_new(s.summary.c_str());
                gtk_widget_add_css_class(sum, "spinel-ai-item-sub");
                gtk_widget_set_halign(sum, GTK_ALIGN_START);
                gtk_label_set_ellipsize(GTK_LABEL(sum), PANGO_ELLIPSIZE_END);
                gtk_label_set_max_width_chars(GTK_LABEL(sum), 40);
                gtk_box_append(GTK_BOX(v), sum);
            }

            gtk_button_set_child(GTK_BUTTON(btn), v);
            g_object_set_data_full(G_OBJECT(btn), "win-id", g_strdup(std::to_string(s.win_id).c_str()), g_free);
            g_object_set_data(G_OBJECT(btn), "in-sb", GINT_TO_POINTER(s.in_sandbox ? 1 : 0));
            g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer d) {
                auto *n = static_cast<NativeShellState *>(d);
                if (n->bar_ai_popover) gtk_popover_popdown(GTK_POPOVER(n->bar_ai_popover));
                bool is_sb = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "in-sb")) != 0;
                const char *wid_s = static_cast<const char *>(g_object_get_data(G_OBJECT(b), "win-id"));
                uint64_t wid = wid_s ? static_cast<uint64_t>(std::strtoull(wid_s, nullptr, 10)) : 0;
                if (is_sb) {
                    sandbox_pip_sync_action(n, "show");
                } else if (wid > 0) {
                    activate_tracked_window(wid);
                }
            }), ns);
            gtk_box_append(GTK_BOX(ns->bar_ai_list_box), btn);
        }
    }

    gtk_box_append(GTK_BOX(ns->bar_ai_list_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    // Compact bottom action bar
    {
        GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_top(foot, 2);

        GtkWidget *feed_btn = gtk_button_new_with_label("复制当前现场");
        gtk_widget_add_css_class(feed_btn, "spinel-ai-mini-btn");
        gtk_widget_set_tooltip_text(feed_btn, "打包当前窗口/工作区与近期剪贴板到剪贴板，直接喂给任意 AI 工具");
        g_signal_connect(feed_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
            auto *n = static_cast<NativeShellState *>(d);
            std::string bundle = spinel::ai::build_work_context_bundle("");
            copy_text_to_clipboard(bundle);
            show_island_toast(n, "✓ 已打包工作现场到剪贴板");
        }), ns);
        gtk_box_append(GTK_BOX(foot), feed_btn);

        GtkWidget *perm_btn = gtk_button_new_with_label("权限与审计");
        gtk_widget_add_css_class(perm_btn, "spinel-ai-mini-btn");
        g_signal_connect(perm_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
            auto *n = static_cast<NativeShellState *>(d);
            if (n->bar_ai_popover) gtk_popover_popdown(GTK_POPOVER(n->bar_ai_popover));
            trigger_cmd(n->app_state, "settings:open:perms");
        }), ns);
        gtk_box_append(GTK_BOX(foot), perm_btn);
        gtk_box_append(GTK_BOX(ns->bar_ai_list_box), foot);
    }
}

void draw_island_dot_cb(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer user_data) {
    auto *ns = static_cast<NativeShellState *>(user_data);
    std::string cls = ns ? ns->ai_state_cls : "idle";
    double cx = w * 0.5;
    double cy = h * 0.5;

    if (cls == "toast") {
        // Luminous mint core on emerald toast
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.25);
        cairo_arc(cr, cx, cy, 5.5, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.96);
        cairo_arc(cr, cx, cy, 3.0, 0, 2 * G_PI);
        cairo_fill(cr);
        return;
    }
    if (cls == "alert") {
        cairo_set_source_rgba(cr, 0.96, 0.62, 0.04, 0.28);
        cairo_arc(cr, cx, cy, 6.0, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.98, 0.75, 0.14, 0.98);
        cairo_arc(cr, cx, cy, 3.2, 0, 2 * G_PI);
        cairo_fill(cr);
        return;
    }
    if (cls == "running") {
        cairo_set_source_rgba(cr, 0.22, 0.74, 0.97, 0.26);
        cairo_arc(cr, cx, cy, 6.0, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.22, 0.74, 0.97, 0.98);
        cairo_arc(cr, cx, cy, 3.1, 0, 2 * G_PI);
        cairo_fill(cr);
        return;
    }
    // Idle: crisp 4-point Spinel AI spark
    cairo_set_source_rgba(cr, 0.56, 0.78, 1.0, 0.92);
    cairo_move_to(cr, cx, cy - 5.2);
    cairo_curve_to(cr, cx + 0.9, cy - 1.4, cx + 1.4, cy - 0.9, cx + 5.2, cy);
    cairo_curve_to(cr, cx + 1.4, cy + 0.9, cx + 0.9, cy + 1.4, cx, cy + 5.2);
    cairo_curve_to(cr, cx - 0.9, cy + 1.4, cx - 1.4, cy + 0.9, cx - 5.2, cy);
    cairo_curve_to(cr, cx - 1.4, cy - 0.9, cx - 0.9, cy - 1.4, cx, cy - 5.2);
    cairo_close_path(cr);
    cairo_fill(cr);
}

}  // namespace

std::string format_island_pill_summary(const std::vector<spinel::ai::AgentSessionInfo> &sessions,
                                       const std::vector<spinel::ai::ApprovalRequestInfo> &approvals,
                                       const std::string &toast_text,
                                       std::string &out_state_cls) {
    return spinel::ai::format_agent_island_pill_summary(sessions, approvals, toast_text, out_state_cls);
}

GtkWidget *build_agent_island_widget(NativeShellState *ns) {
    GtkWidget *btn = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(btn), FALSE);
    gtk_widget_add_css_class(btn, "spinel-ai-pill");
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(btn, "Spinel AI 灵动岛：查看活动 Agent 会话与 Tier 3 权限审批");

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(row, GTK_ALIGN_CENTER);

    GtkWidget *dot = gtk_drawing_area_new();
    gtk_widget_set_size_request(dot, 13, 13);
    gtk_widget_set_halign(dot, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(dot), draw_island_dot_cb, ns, nullptr);

    GtkWidget *lbl = gtk_label_new("AI");
    gtk_widget_add_css_class(lbl, "spinel-ai-pill-lbl");
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 38);

    gtk_box_append(GTK_BOX(row), dot);
    gtk_box_append(GTK_BOX(row), lbl);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(btn), row);

    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_widget_add_css_class(pop, "spinel-ctx-popover");
    gtk_widget_add_css_class(pop, "spinel-ai-popover");

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 400);
    gtk_widget_set_size_request(scroll, 348, -1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_popover_set_child(GTK_POPOVER(pop), scroll);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(btn), pop);

    g_signal_connect(pop, "show", G_CALLBACK(+[](GtkWidget *, gpointer d) {
        auto *n = static_cast<NativeShellState *>(d);
        native_shell_set_cc_visible(n->app_state, false);
        agent_island_refresh(n);
    }), ns);

    ns->bar_ai_btn = btn;
    ns->bar_ai_dot_da = dot;
    ns->bar_ai_lbl = lbl;
    ns->bar_ai_popover = pop;
    ns->bar_ai_list_box = box;
    return btn;
}

void agent_island_refresh(NativeShellState *ns) {
    if (!ns || !ns->bar_ai_btn || !ns->bar_ai_lbl) return;

    std::vector<spinel::ai::WindowBindingRef> wins;
    for (const auto &w : list_tracked_windows()) {
        wins.push_back({w.id, w.app_id, w.title, w.activated});
    }
    auto sessions = spinel::ai::reconcile_agent_sessions(0, 300, true, wins);
    auto approvals = spinel::ai::list_pending_approvals();

    std::string cls;
    std::string text = format_island_pill_summary(sessions, approvals, ns->ai_toast_text, cls);
    ns->ai_state_cls = cls;
    gtk_label_set_text(GTK_LABEL(ns->bar_ai_lbl), text.c_str());

    for (const char *c : {"is-running", "is-alert", "is-toast"}) {
        gtk_widget_remove_css_class(ns->bar_ai_btn, c);
    }
    if (cls == "running") gtk_widget_add_css_class(ns->bar_ai_btn, "is-running");
    else if (cls == "alert") gtk_widget_add_css_class(ns->bar_ai_btn, "is-alert");
    else if (cls == "toast") gtk_widget_add_css_class(ns->bar_ai_btn, "is-toast");

    if (ns->bar_ai_dot_da) gtk_widget_queue_draw(ns->bar_ai_dot_da);

    if (ns->bar_ai_popover && gtk_widget_get_visible(ns->bar_ai_popover)) {
        rebuild_ai_popover_contents(ns, sessions, approvals);
    }
}

void init_agent_island(NativeShellState *ns) {
    if (!ns || !ns->bar_ai_btn) return;
    g_island_ns.store(ns);

    std::error_code ec;
    std::string rt_dir = spinel::ai::agent_sessions_runtime_dir();
    fs::create_directories(rt_dir, ec);

    GFile *dir_gf = g_file_new_for_path(rt_dir.c_str());
    ns->ai_dir_monitor = g_file_monitor_directory(dir_gf, G_FILE_MONITOR_NONE, nullptr, nullptr);
    g_object_unref(dir_gf);
    if (ns->ai_dir_monitor) {
        g_signal_connect(ns->ai_dir_monitor, "changed",
                         G_CALLBACK(+[](GFileMonitor *, GFile *, GFile *, GFileMonitorEvent, gpointer) {
                             schedule_island_refresh_from_any_thread();
                         }),
                         nullptr);
    }

    spinel::ai::set_agent_transition_callback([](const spinel::ai::AgentTransitionEvent &ev) {
        spinel::ai::sync_task_with_agent_transition(ev);
        std::string toast;
        if (ev.new_status == spinel::ai::AgentSessionStatus::Done) {
            toast = "✓ " + ev.session.agent + " · " +
                    (ev.session.summary.empty() ? "任务已完成" : ev.session.summary);
        } else if (ev.new_status == spinel::ai::AgentSessionStatus::Error) {
            toast = "✕ " + ev.session.agent + " · " +
                    (ev.session.summary.empty() ? "执行异常" : ev.session.summary);
        } else if (ev.new_status == spinel::ai::AgentSessionStatus::WaitingUser) {
            toast = "⚠ " + ev.session.agent + " 等待确认";
        }
        schedule_island_refresh_from_any_thread(toast);
    });

    spinel::ai::set_approval_changed_callback([]() {
        auto pend = spinel::ai::list_pending_approvals();
        std::string toast;
        if (!pend.empty()) {
            toast = "⚠ 审批请求: " + pend.front().client_id + " · " + pend.front().summary;
        }
        schedule_island_refresh_from_any_thread(toast);
    });

    ns->ai_refresh_timer = g_timeout_add(2500, +[](gpointer d) -> gboolean {
        auto *n = static_cast<NativeShellState *>(d);
        if (!n) return G_SOURCE_REMOVE;
        agent_island_refresh(n);
        return G_SOURCE_CONTINUE;
    }, ns);

    agent_island_refresh(ns);
}

void shutdown_agent_island(NativeShellState *ns) {
    g_island_ns.store(nullptr);
    spinel::ai::set_agent_transition_callback(nullptr);
    spinel::ai::set_approval_changed_callback(nullptr);
    if (!ns) return;
    if (ns->ai_refresh_timer) {
        g_source_remove(ns->ai_refresh_timer);
        ns->ai_refresh_timer = 0;
    }
    if (ns->ai_toast_timer) {
        g_source_remove(ns->ai_toast_timer);
        ns->ai_toast_timer = 0;
    }
    if (ns->ai_dir_monitor) {
        g_file_monitor_cancel(ns->ai_dir_monitor);
        g_object_unref(ns->ai_dir_monitor);
        ns->ai_dir_monitor = nullptr;
    }
}

std::string agent_island_css(const NativeShellState *ns) {
    ThemeSpec t = ns ? ns->theme : get_active_theme();
    bool is_light = t.is_light;
    const char *pill_bg = is_light ? "rgba(15, 23, 42, 0.08)" : "rgba(255, 255, 255, 0.11)";
    const char *card_bg = is_light ? "rgba(15, 23, 42, 0.04)" : "rgba(255, 255, 255, 0.06)";
    const char *sub_fg = is_light ? "rgba(15, 23, 42, 0.62)" : "rgba(255, 255, 255, 0.66)";
    const char *apr_bg = is_light ? "rgba(245, 158, 11, 0.14)" : "rgba(245, 158, 11, 0.18)";
    const char *apr_bd = is_light ? "rgba(217, 119, 6, 0.45)" : "rgba(251, 191, 36, 0.45)";

    return
        ".spinel-ai-pill > button {\n"
        "  background-color: rgba(15, 20, 32, 0.90);\n"
        "  background-image: none;\n"
        "  border: 1px solid rgba(255, 255, 255, 0.16);\n"
        "  border-radius: 999px;\n"
        "  box-shadow: 0 2px 10px rgba(0, 0, 0, 0.22);\n"
        "  padding: 1px 12px 1px 9px;\n"
        "  min-height: 22px;\n"
        "  min-width: 56px;\n"
        "  margin: 0;\n"
        "  transition: all 220ms cubic-bezier(0.22, 1, 0.36, 1);\n"
        "}\n"
        ".spinel-ai-pill > button:hover {\n"
        "  background-color: rgba(24, 32, 50, 0.96);\n"
        "  border-color: rgba(255, 255, 255, 0.28);\n"
        "}\n"
        ".spinel-ai-pill.is-running > button {\n"
        "  background-color: rgba(12, 24, 42, 0.94);\n"
        "  border-color: rgba(56, 189, 248, 0.58);\n"
        "  box-shadow: 0 2px 12px rgba(56, 189, 248, 0.24);\n"
        "  padding: 1px 14px 1px 10px;\n"
        "}\n"
        ".spinel-ai-pill.is-alert > button {\n"
        "  background-color: rgba(34, 22, 8, 0.95);\n"
        "  border-color: rgba(245, 158, 11, 0.78);\n"
        "  box-shadow: 0 2px 12px rgba(245, 158, 11, 0.28);\n"
        "  padding: 1px 14px 1px 10px;\n"
        "}\n"
        ".spinel-ai-pill.is-toast > button {\n"
        "  background-color: " + t.cc_accent + ";\n"
        "  border-color: rgba(255, 255, 255, 0.34);\n"
        "  box-shadow: 0 3px 14px rgba(37, 99, 235, 0.38);\n"
        "  padding: 1px 16px 1px 11px;\n"
        "}\n"
        ".spinel-ai-pill-lbl { font-size: 11.5px; font-weight: 600; color: #f8fafc; letter-spacing: 0.01em; }\n"
        ".spinel-ai-pop-hdr { padding: 2px 4px 4px 4px; }\n"
        ".spinel-ai-pop-title { font-size: 13px; font-weight: 700; color: " + t.bar_fg + "; }\n"
        ".spinel-ai-sec-title { font-size: 11.5px; font-weight: 700; color: " + t.bar_fg + "; padding: 4px 4px 2px 4px; }\n"
        ".spinel-ai-sec-title.is-alert { color: #f59e0b; }\n"
        ".spinel-ai-item-title { font-size: 12px; font-weight: 600; color: " + t.bar_fg + "; }\n"
        ".spinel-ai-item-sub { font-size: 11px; color: " + std::string(sub_fg) + "; }\n"
        ".spinel-ai-mono { font-size: 10.5px; font-family: monospace; color: " + std::string(sub_fg) + "; }\n"
        ".spinel-ai-dot { font-size: 11px; min-width: 14px; }\n"
        ".spinel-ai-dot.st-run { color: #38bdf8; }\n"
        ".spinel-ai-dot.st-wait { color: #f59e0b; }\n"
        ".spinel-ai-dot.st-done { color: #22c55e; }\n"
        ".spinel-ai-dot.st-err { color: #ef4444; }\n"
        ".spinel-ai-badge {\n"
        "  font-size: 10px; font-weight: 600; padding: 1px 7px; border-radius: 999px;\n"
        "  background-color: " + std::string(pill_bg) + "; color: " + t.bar_fg + ";\n"
        "}\n"
        ".spinel-ai-apr-card {\n"
        "  background-color: " + std::string(apr_bg) + ";\n"
        "  border: 1px solid " + std::string(apr_bd) + ";\n"
        "  border-radius: 11px;\n"
        "  padding: 9px 11px;\n"
        "  margin: 2px 0;\n"
        "}\n"
        ".spinel-ai-mini-btn {\n"
        "  background-color: " + std::string(pill_bg) + ";\n"
        "  background-image: none;\n"
        "  border: 1px solid " + t.bar_border + ";\n"
        "  box-shadow: none;\n"
        "  border-radius: 8px;\n"
        "  padding: 3px 9px;\n"
        "  min-height: 24px;\n"
        "  font-size: 11px;\n"
        "  font-weight: 600;\n"
        "  color: " + t.bar_fg + ";\n"
        "}\n"
        ".spinel-ai-mini-btn:hover { background-color: " + std::string(card_bg) + "; }\n"
        ".spinel-ai-mini-btn.is-primary { background-color: " + t.cc_accent + "; border-color: " + t.cc_accent + "; color: #ffffff; }\n"
        ".spinel-ai-mini-btn.is-danger { color: #ef4444; }\n";
}

}  // namespace shell_internal
