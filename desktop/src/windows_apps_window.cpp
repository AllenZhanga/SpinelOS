#include "windows_apps_window.hpp"
#include "files_theme.hpp"
#include "wine_launcher.hpp"

#include <algorithm>
#include <memory>

namespace fs = std::filesystem;

namespace {

struct WindowState {
    GtkWindow *window;
    GtkListBox *list;
    GtkLabel *status;
    GtkWidget *refresh;
    GtkWidget *install;
    bool busy = false;
    bool refresh_requested = false;
    guint refresh_timer = 0;
    GFileMonitor *registry = nullptr;

    ~WindowState() {
        if (refresh_timer) g_source_remove(refresh_timer);
        if (registry) {
            g_file_monitor_cancel(registry);
            g_object_unref(registry);
        }
    }
};

struct PendingAction {
    GWeakRef window;
    WineInstalledApp app;

    explicit PendingAction(GtkWindow *owner, WineInstalledApp target = {}) : app(std::move(target)) {
        g_weak_ref_init(&window, G_OBJECT(owner));
    }
    ~PendingAction() { g_weak_ref_clear(&window); }
};

struct AppRow {
    WindowState *state;
    WineInstalledApp app;
    WineShortcut shortcut;
};

constexpr auto state_key = "spinel-windows-apps";

void set_busy(WindowState &state, bool busy, const char *message) {
    state.busy = busy;
    gtk_widget_set_sensitive(GTK_WIDGET(state.list), !busy);
    gtk_widget_set_sensitive(state.refresh, !busy);
    gtk_label_set_text(state.status, message);
}

void show_error(GtkWindow *window, const char *title, const GError *error) {
    g_autoptr(GtkAlertDialog) dialog = gtk_alert_dialog_new("%s", title);
    if (error) gtk_alert_dialog_set_detail(dialog, error->message);
    gtk_alert_dialog_show(dialog, window);
}

void watch_launch(GtkWindow *window, GSubprocess *process, const GError *error) {
    if (!process) {
        show_error(window, "无法启动 Windows 应用", error);
        return;
    }
    g_subprocess_wait_check_async(process, nullptr, +[](GObject *source, GAsyncResult *result, gpointer data) {
        std::unique_ptr<PendingAction> action(static_cast<PendingAction *>(data));
        g_autoptr(GError) error = nullptr;
        if (g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error)) return;
        g_autoptr(GtkWindow) window = GTK_WINDOW(g_weak_ref_get(&action->window));
        if (window) show_error(window, "Wine 启动程序失败", error);
    }, new PendingAction(window));
}

void choose_executable(GtkWindow *window) {
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "选择要启动的 Windows 程序");
    const fs::path drive = spinel_wine_prefix() / "drive_c";
    g_autoptr(GFile) folder = g_file_new_for_path(drive.c_str());
    gtk_file_dialog_set_initial_folder(dialog, folder);
    g_autoptr(GtkFileFilter) filter = gtk_file_filter_new();
    gtk_file_filter_add_suffix(filter, "exe");
    gtk_file_dialog_set_default_filter(dialog, filter);
    gtk_file_dialog_open(dialog, window, nullptr, +[](GObject *source, GAsyncResult *result, gpointer data) {
        std::unique_ptr<PendingAction> action(static_cast<PendingAction *>(data));
        g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, nullptr);
        g_autoptr(GtkWindow) window = GTK_WINDOW(g_weak_ref_get(&action->window));
        if (!file || !window) return;
        g_autofree char *path = g_file_get_path(file);
        if (!path) {
            g_autoptr(GError) error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                                        "请先将此文件复制到本地，再用 Wine 打开。");
            show_error(window, "无法用 Wine 启动", error);
            return;
        }
        g_autoptr(GError) error = nullptr;
        g_autoptr(GSubprocess) process = launch_windows_executable(path, &error);
        watch_launch(window, process, error);
    }, new PendingAction(window));
}

void refresh_apps(WindowState &state);

void schedule_refresh(WindowState &state) {
    state.refresh_requested = true;
    if (state.busy || state.refresh_timer) return;
    state.refresh_timer = g_timeout_add(300, +[](gpointer data) -> gboolean {
        auto &state = *static_cast<WindowState *>(data);
        state.refresh_timer = 0;
        if (!state.busy && state.refresh_requested) refresh_apps(state);
        return G_SOURCE_REMOVE;
    }, &state);
}

void watch_registry(WindowState &state) {
    if (state.registry) return;
    g_autoptr(GFile) directory = g_file_new_for_path(spinel_wine_prefix().c_str());
    state.registry = g_file_monitor_directory(directory, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    if (!state.registry) return;
    // Wine writes registry files by replacing them, so watch the containing directory.
    g_signal_connect(state.registry, "changed",
        G_CALLBACK(+[](GFileMonitor *, GFile *file, GFile *other, GFileMonitorEvent, gpointer data) {
            auto is_registry = [](GFile *candidate) {
                if (!candidate) return false;
                g_autofree char *name = g_file_get_basename(candidate);
                return g_strcmp0(name, "system.reg") == 0 || g_strcmp0(name, "user.reg") == 0;
            };
            if (is_registry(file) || is_registry(other)) schedule_refresh(*static_cast<WindowState *>(data));
        }), &state);
}

void confirm_uninstall(const AppRow &row) {
    g_autoptr(GtkAlertDialog) dialog = gtk_alert_dialog_new("卸载 %s？", row.app.name.c_str());
    gtk_alert_dialog_set_detail(dialog, "Wine 将运行此应用自己的卸载程序。");
    const char *buttons[] = {"取消", "卸载", nullptr};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);
    gtk_alert_dialog_choose(dialog, row.state->window, nullptr,
        +[](GObject *source, GAsyncResult *result, gpointer data) {
            std::unique_ptr<PendingAction> action(static_cast<PendingAction *>(data));
            if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, nullptr) != 1) return;
            g_autoptr(GtkWindow) window = GTK_WINDOW(g_weak_ref_get(&action->window));
            if (!window) return;
            auto &state = *static_cast<WindowState *>(g_object_get_data(G_OBJECT(window), state_key));
            if (state.busy) return;
            g_autoptr(GError) error = nullptr;
            g_autoptr(GSubprocess) process = uninstall_wine_app(action->app.key, &error);
            if (!process) {
                show_error(window, "无法开始卸载", error);
                return;
            }
            set_busy(state, true, "正在打开卸载程序…");
            g_subprocess_wait_check_async(process, nullptr,
                +[](GObject *source, GAsyncResult *result, gpointer data) {
                    std::unique_ptr<PendingAction> action(static_cast<PendingAction *>(data));
                    g_autoptr(GError) error = nullptr;
                    const bool success = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error);
                    g_autoptr(GtkWindow) window = GTK_WINDOW(g_weak_ref_get(&action->window));
                    if (!window) return;
                    auto &state = *static_cast<WindowState *>(g_object_get_data(G_OBJECT(window), state_key));
                    if (!success) show_error(window, "Wine 卸载程序执行失败", error);
                    set_busy(state, false, success ? "请按 Windows 卸载程序的提示操作。" : "卸载程序执行失败。");
                    if (state.refresh_requested) schedule_refresh(state);
                }, action.release());
        }, new PendingAction(row.state->window, row.app));
}

void populate_rows(WindowState &state, const std::vector<WineInstalledApp> &apps) {
    gtk_list_box_remove_all(state.list);
    gtk_widget_set_visible(GTK_WIDGET(state.list), !apps.empty());
    const auto shortcuts = list_wine_shortcuts();
    for (const auto &app : apps) {
        const auto *shortcut = find_wine_app_shortcut(app, shortcuts);
        const bool unique_key = g_utf8_validate(app.key.c_str(), app.key.size(), nullptr) &&
            std::ranges::count_if(apps, [&](const WineInstalledApp &other) {
                return g_ascii_strcasecmp(other.key.c_str(), app.key.c_str()) == 0;
            }) == 1;
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_add_css_class(row, "windows-app-row");
        auto *data = new AppRow{&state, app, shortcut ? *shortcut : WineShortcut{}};
        g_object_set_data_full(G_OBJECT(row), "app", data, +[](gpointer p) { delete static_cast<AppRow *>(p); });

        const char *icon = "application-x-executable";
        if (shortcut && !shortcut->icon.empty()) {
            auto *theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(GTK_WIDGET(state.window)));
            if (gtk_icon_theme_has_icon(theme, shortcut->icon.c_str())) icon = shortcut->icon.c_str();
        }
        GtkWidget *image = gtk_image_new_from_icon_name(icon);
        gtk_image_set_pixel_size(GTK_IMAGE(image), 28);
        gtk_box_append(GTK_BOX(row), image);

        GtkWidget *name = gtk_label_new(app.name.c_str());
        gtk_widget_add_css_class(name, "windows-app-name");
        gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_widget_set_tooltip_text(name, app.name.c_str());
        gtk_widget_set_hexpand(name, TRUE);
        gtk_box_append(GTK_BOX(row), name);

        GtkWidget *launch = gtk_button_new_with_label(shortcut ? "启动" : "选择 .exe…");
        gtk_widget_add_css_class(launch, "sf-tool-btn");
        if (!shortcut) gtk_widget_set_tooltip_text(launch, "此应用没有匹配的 Wine 快捷方式");
        g_signal_connect(launch, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) {
            const auto &row = *static_cast<AppRow *>(p);
            GtkWindow *window = row.state->window;
            if (row.shortcut.link.empty()) {
                choose_executable(window);
                return;
            }
            g_autoptr(GError) error = nullptr;
            g_autoptr(GSubprocess) process = launch_wine_shortcut(row.shortcut, &error);
            watch_launch(window, process, error);
        }), data);
        gtk_box_append(GTK_BOX(row), launch);

        GtkWidget *remove = gtk_button_new_with_label("卸载");
        gtk_widget_add_css_class(remove, "sf-tool-danger");
        gtk_widget_set_sensitive(remove, unique_key);
        if (!unique_key) gtk_widget_set_tooltip_text(remove, "请在 Wine 卸载面板里选择这条卸载记录");
        g_signal_connect(remove, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) {
            confirm_uninstall(*static_cast<AppRow *>(p));
        }), data);
        gtk_box_append(GTK_BOX(row), remove);
        gtk_widget_set_valign(launch, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
        gtk_list_box_append(state.list, row);
    }
}

void refresh_apps(WindowState &state) {
    if (state.busy) return;
    state.refresh_requested = false;
    const bool wine_available = wine_is_available();
    gtk_widget_set_visible(state.install, !wine_available);
    if (!wine_available) {
        gtk_list_box_remove_all(state.list);
        gtk_widget_set_visible(GTK_WIDGET(state.list), FALSE);
        set_busy(state, false, "安装 Wine 后即可运行和管理 Windows 应用。");
        return;
    }
    std::error_code ec;
    if (!fs::is_regular_file(spinel_wine_prefix() / "system.reg", ec)) {
        gtk_list_box_remove_all(state.list);
        gtk_widget_set_visible(GTK_WIDGET(state.list), FALSE);
        set_busy(state, false, "还没有安装 Windows 应用。可以在文件管理器里右键安装包，选择「用 Wine 运行」。");
        return;
    }
    watch_registry(state);
    set_busy(state, true, "正在读取已安装应用…");
    g_autoptr(GTask) query = g_task_new(nullptr, nullptr,
        +[](GObject *, GAsyncResult *result, gpointer data) {
            std::unique_ptr<PendingAction> action(static_cast<PendingAction *>(data));
            g_autoptr(GError) error = nullptr;
            std::unique_ptr<std::vector<WineInstalledApp>> apps(
                static_cast<std::vector<WineInstalledApp> *>(g_task_propagate_pointer(G_TASK(result), &error)));
            g_autoptr(GtkWindow) window = GTK_WINDOW(g_weak_ref_get(&action->window));
            if (!window) return;
            auto &state = *static_cast<WindowState *>(g_object_get_data(G_OBJECT(window), state_key));
            if (!apps) {
                set_busy(state, false, error ? error->message : "Wine 无法读取已安装应用。");
                if (state.refresh_requested) schedule_refresh(state);
                return;
            }
            populate_rows(state, *apps);
            const std::string message = apps->empty() ?
                "还没有安装 Windows 应用。可以在文件管理器里右键安装包，选择「用 Wine 运行」。" :
                std::to_string(apps->size()) + " 个已安装应用";
            set_busy(state, false, message.c_str());
            if (state.refresh_requested) schedule_refresh(state);
        }, new PendingAction(state.window));
    g_task_run_in_thread(query, +[](GTask *task, gpointer, gpointer, GCancellable *) {
        auto apps = std::make_unique<std::vector<WineInstalledApp>>();
        GError *error = nullptr;
        if (!list_wine_installed_apps(*apps, &error)) {
            g_task_return_error(task, error);
            return;
        }
        g_task_return_pointer(task, apps.release(), +[](gpointer p) {
            delete static_cast<std::vector<WineInstalledApp> *>(p);
        });
    });
}

} // namespace

bool open_wine_store(GtkWindow *parent, GError **error) {
    g_autoptr(GdkAppLaunchContext) context = nullptr;
    g_autofree char *token = nullptr;
    if (parent) {
        context = gdk_display_get_app_launch_context(gtk_widget_get_display(GTK_WIDGET(parent)));
        token = g_app_launch_context_get_startup_notify_id(G_APP_LAUNCH_CONTEXT(context), nullptr, nullptr);
    } else if (gtk_init_check()) {
        // Preserve the activation token from the external MIME launch.
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        token = g_strdup(gdk_display_get_startup_notification_id(gdk_display_get_default()));
        G_GNUC_END_IGNORE_DEPRECATIONS
    }

    const auto flags = static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                                     G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(flags);
    if (token) g_subprocess_launcher_setenv(launcher, "DESKTOP_STARTUP_ID", token, TRUE);
    g_autoptr(GSubprocess) store = g_subprocess_launcher_spawn(launcher, error, "spinel-webui", "store:open:wine", nullptr);
    if (!store && context && token) g_app_launch_context_launch_failed(G_APP_LAUNCH_CONTEXT(context), token);
    return store != nullptr;
}

void show_windows_apps_window(GtkApplication *app) {
    if (GtkWindow *existing = gtk_application_get_active_window(app)) {
        gtk_window_present(existing);
        return;
    }
    auto *window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(window, "Windows 应用");
    gtk_window_set_icon_name(window, "org.spinelos.WindowsApps");
    gtk_window_set_default_size(window, 600, 370);
    gtk_widget_add_css_class(GTK_WIDGET(window), "sf-win");

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(window, content);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(header, "sf-toolbar");
    auto *status = GTK_LABEL(gtk_label_new(nullptr));
    gtk_label_set_xalign(status, 0.0f);
    gtk_label_set_wrap(status, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(status), "sf-muted");
    gtk_widget_set_hexpand(GTK_WIDGET(status), TRUE);
    gtk_box_append(GTK_BOX(header), GTK_WIDGET(status));
    GtkWidget *install = gtk_button_new_with_label("安装 Wine");
    gtk_widget_add_css_class(install, "sf-tool-btn");
    gtk_widget_set_valign(install, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(header), install);
    GtkWidget *refresh = gtk_button_new_with_label("刷新");
    gtk_widget_add_css_class(refresh, "sf-tool-btn");
    gtk_widget_set_valign(refresh, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(header), refresh);
    gtk_box_append(GTK_BOX(content), header);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_margin_start(scroll, 26);
    gtk_widget_set_margin_end(scroll, 26);
    gtk_widget_set_margin_top(scroll, 22);
    gtk_widget_set_margin_bottom(scroll, 22);
    gtk_box_append(GTK_BOX(content), scroll);
    auto *list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(list), "windows-app-list");
    gtk_widget_set_valign(GTK_WIDGET(list), GTK_ALIGN_START);
    gtk_list_box_set_selection_mode(list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(list));

    auto *state = new WindowState{window, list, status, refresh, install};
    g_object_set_data_full(G_OBJECT(window), state_key, state, +[](gpointer p) { delete static_cast<WindowState *>(p); });
    g_signal_connect(install, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
        auto &state = *static_cast<WindowState *>(data);
        g_autoptr(GError) error = nullptr;
        if (!open_wine_store(state.window, &error)) show_error(state.window, "无法打开应用商店", error);
    }), state);
    g_signal_connect(refresh, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
        refresh_apps(*static_cast<WindowState *>(data));
    }), state);
    g_signal_connect(window, "notify::is-active", G_CALLBACK(+[](GtkWindow *window, GParamSpec *, gpointer data) {
        if (gtk_window_is_active(window)) schedule_refresh(*static_cast<WindowState *>(data));
    }), state);
    bind_files_window_theme(window);
    refresh_apps(*state);
    gtk_window_present(window);
}
