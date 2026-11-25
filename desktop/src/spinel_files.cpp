#include "common.hpp"

#include <gtk/gtk.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct FilesTheme {
    std::string id;
    bool is_light = false;
    std::string fg = "#f1f5f9";
    std::string muted = "#94a3b8";
    std::string accent = "#38bdf8";
};

FilesTheme load_active_files_theme() {
    std::string id = trim_copy(read_first_line(
        (fs::path(home_dir()) / ".config/spinelos/theme.conf").string(), ""
    ));
    if (id.empty()) {
        std::string legacy = trim_copy(read_first_line(
            (fs::path(home_dir()) / ".config/spinelos/wallpaper.conf").string(), ""
        ));
        if (legacy == "obsidian-purple") id = "obsidian-neon";
        else if (legacy == "emerald-aurora") id = "emerald-nordic";
        else id = "tahoe-light";
    }

    FilesTheme th;
    th.id = id;
    if (id == "tahoe-light") {
        th.is_light = true;
        th.fg = "#0f172a";
        th.muted = "#64748b";
        th.accent = "#0a84ff";
    } else if (id == "emerald-nordic") {
        th.fg = "#ecfdf5";
        th.muted = "#6ee7b7";
        th.accent = "#10b981";
    } else if (id == "obsidian-neon") {
        th.fg = "#f5f3ff";
        th.muted = "#c4b5fd";
        th.accent = "#a855f7";
    } else if (id != "crystal-aurora") {
        fs::path custom = fs::path(home_dir()) / ".config/spinelos/themes" / (id + ".theme");
        std::ifstream ifs(custom);
        std::string line;
        while (std::getline(ifs, line)) {
            line = trim_copy(line);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim_copy(line.substr(0, eq));
            std::string v = trim_copy(line.substr(eq + 1));
            if (k == "is_light") th.is_light = (v == "true" || v == "1");
            else if (k == "ui_text") th.fg = v;
            else if (k == "ui_muted") th.muted = v;
            else if (k == "ui_accent") th.accent = v;
        }
    }
    return th;
}

namespace {

struct DirItem {
    std::string name;
    fs::path path;
    bool is_dir = false;
    bool is_symlink = false;
    bool is_exec = false;
    std::uintmax_t size = 0;
    std::time_t mtime = 0;
};

struct MountItem {
    std::string label;
    fs::path mount_path;
    std::string device;
    bool removable = false;
};

enum class ClipOp { None, Copy, Cut };

struct FilesWin {
    GtkApplication *app = nullptr;
    GtkWindow *win = nullptr;
    GtkCssProvider *css = nullptr;
    guint theme_timer = 0;
    std::string last_theme_id;

    GtkWidget *root = nullptr;
    GtkWidget *btn_back = nullptr;
    GtkWidget *btn_fwd = nullptr;
    GtkWidget *btn_up = nullptr;
    GtkWidget *btn_view_mode = nullptr;
    GtkWidget *btn_hidden = nullptr;
    GtkWidget *btn_path_edit = nullptr;

    GtkWidget *crumb_scroll = nullptr;
    GtkWidget *crumb_box = nullptr;
    GtkWidget *path_entry = nullptr;
    GtkWidget *search_entry = nullptr;

    GtkWidget *places_box = nullptr;
    GtkWidget *drives_box = nullptr;
    GtkWidget *content_scroll = nullptr;
    GtkWidget *list_view = nullptr;
    GtkWidget *grid_view = nullptr;
    GtkWidget *view_stack = nullptr;
    GtkWidget *status_label = nullptr;

    fs::path current_dir;
    std::vector<fs::path> history;
    int hist_idx = -1;
    std::vector<DirItem> items;
    std::vector<DirItem> visible_items;
    fs::path selected_path;
    bool show_hidden = false;
    bool grid_mode = false;
    bool path_edit_mode = false;

    ClipOp clip_op = ClipOp::None;
    std::vector<fs::path> clip_paths;
};

std::string to_lower_copy(std::string s) {
    for (char &ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string format_size(std::uintmax_t bytes) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int idx = 0;
    while (v >= 1024.0 && idx < 4) {
        v /= 1024.0;
        ++idx;
    }
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(idx == 0 ? 0 : 1);
    oss << v << " " << units[idx];
    return oss.str();
}

std::string format_time(std::time_t t) {
    if (t <= 0) return "-";
    std::tm tm_val{};
    localtime_r(&t, &tm_val);
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_val);
    return buf;
}

std::string format_perms(fs::perms p) {
    auto bit = [&](fs::perms f, char c) { return (p & f) != fs::perms::none ? c : '-'; };
    std::string s;
    s += bit(fs::perms::owner_read, 'r');
    s += bit(fs::perms::owner_write, 'w');
    s += bit(fs::perms::owner_exec, 'x');
    s += bit(fs::perms::group_read, 'r');
    s += bit(fs::perms::group_write, 'w');
    s += bit(fs::perms::group_exec, 'x');
    s += bit(fs::perms::others_read, 'r');
    s += bit(fs::perms::others_write, 'w');
    s += bit(fs::perms::others_exec, 'x');
    return s;
}

struct FileBadge {
    const char *icon_name;
    const char *css_class;
    const char *kind_label;
};

FileBadge classify_item(const DirItem &it) {
    if (it.is_dir) return {"folder-symbolic", "sf-ico-dir", "文件夹"};
    std::string ext = to_lower_copy(it.path.extension().string());
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".gif" || ext == ".svg") {
        return {"image-x-generic-symbolic", "sf-ico-img", "图片"};
    }
    if (ext == ".mp4" || ext == ".mkv" || ext == ".webm" || ext == ".mov" || ext == ".avi") {
        return {"video-x-generic-symbolic", "sf-ico-vid", "视频"};
    }
    if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg") {
        return {"audio-x-generic-symbolic", "sf-ico-aud", "音频"};
    }
    if (ext == ".zip" || ext == ".tar" || ext == ".gz" || ext == ".xz" || ext == ".zst" || ext == ".7z") {
        return {"package-x-generic-symbolic", "sf-ico-arc", "压缩包"};
    }
    if (ext == ".cpp" || ext == ".hpp" || ext == ".c" || ext == ".h" || ext == ".py" ||
        ext == ".js" || ext == ".ts" || ext == ".sh" || ext == ".json" || ext == ".md" ||
        ext == ".txt" || ext == ".conf" || ext == ".css" || ext == ".html") {
        return {"text-x-generic-symbolic", "sf-ico-code", "文本/代码"};
    }
    if (it.is_exec || ext == ".appimage") {
        return {"application-x-executable-symbolic", "sf-ico-exec", "可执行"};
    }
    return {"text-x-generic-symbolic", "sf-ico-file", "文件"};
}

std::vector<MountItem> scan_external_mounts() {
    std::vector<MountItem> res;
    std::ifstream ifs("/proc/mounts");
    if (!ifs.is_open()) return res;
    std::set<std::string> seen;
    std::string line;
    while (std::getline(ifs, line)) {
        std::istringstream iss(line);
        std::string dev, mp, fstype;
        if (!(iss >> dev >> mp >> fstype)) continue;
        // Unescape \040 spaces from /proc/mounts
        std::string clean_mp;
        for (size_t i = 0; i < mp.size(); ++i) {
            if (mp[i] == '\\' && i + 3 < mp.size() && mp.substr(i + 1, 3) == "040") {
                clean_mp.push_back(' ');
                i += 3;
            } else {
                clean_mp.push_back(mp[i]);
            }
        }
        bool is_media = (clean_mp.rfind("/run/media/", 0) == 0 || clean_mp.rfind("/media/", 0) == 0);
        bool is_mnt = (clean_mp.rfind("/mnt/", 0) == 0);
        if (!is_media && !is_mnt) continue;
        if (!seen.insert(clean_mp).second) continue;

        fs::path p(clean_mp);
        std::string lbl = p.filename().string();
        if (lbl.empty()) lbl = clean_mp;
        res.push_back({lbl, p, dev, is_media || dev.rfind("/dev/sd", 0) == 0 || dev.rfind("/dev/nvme", 0) == 0});
    }
    return res;
}

void clear_box_children(GtkWidget *box);

void apply_theme_css(FilesWin *fw) {
    FilesTheme th = load_active_files_theme();
    if (fw->last_theme_id == th.id && fw->css) return;
    fw->last_theme_id = th.id;

    std::string win_bg = th.is_light ? "#f5f7fb" : "#0b111e";
    std::string bar_bg = th.is_light ? "#eef2f8" : "#11192b";
    std::string side_bg = th.is_light ? "#eaf0f8" : "#0e1627";
    std::string card_bg = th.is_light ? "#ffffff" : "#152038";
    std::string hover_bg = th.is_light ? "rgba(10, 132, 255, 0.08)" : "rgba(56, 189, 248, 0.12)";
    std::string sel_bg = th.is_light ? "rgba(10, 132, 255, 0.16)" : "rgba(56, 189, 248, 0.22)";
    std::string border = th.is_light ? "rgba(15, 23, 42, 0.10)" : "rgba(255, 255, 255, 0.10)";
    std::string fg = th.fg;
    std::string muted = th.muted;
    std::string accent = th.accent;

    std::ostringstream css;
    css << "window.sf-win { background: " << win_bg << "; color: " << fg << "; }\n"
        << ".sf-toolbar { background: " << bar_bg << "; border-bottom: 1px solid " << border << "; padding: 8px 12px; }\n"
        << ".sf-sidebar { background: " << side_bg << "; border-right: 1px solid " << border << "; padding: 10px 8px; }\n"
        << ".sf-side-title { color: " << muted << "; font-size: 11px; font-weight: 700; padding: 6px 8px 4px 8px; }\n"
        << ".sf-nav-btn { background: transparent; color: " << fg << "; border: none; border-radius: 8px; padding: 6px 10px; font-size: 13px; }\n"
        << ".sf-nav-btn:hover { background: " << hover_bg << "; }\n"
        << ".sf-nav-btn.is-active { background: " << sel_bg << "; color: " << accent << "; font-weight: 600; }\n"
        << ".sf-tool-btn { background: " << card_bg << "; color: " << fg << "; border: 1px solid " << border << "; border-radius: 8px; padding: 4px 10px; min-height: 28px; }\n"
        << ".sf-tool-btn:hover { border-color: " << accent << "; }\n"
        << ".sf-tool-btn.is-on { background: " << sel_bg << "; border-color: " << accent << "; color: " << accent << "; }\n"
        << ".sf-crumb-pill { background: " << card_bg << "; color: " << fg << "; border: 1px solid " << border << "; border-radius: 7px; padding: 3px 9px; font-size: 12px; }\n"
        << ".sf-crumb-pill:hover { border-color: " << accent << "; }\n"
        << ".sf-entry { background: " << card_bg << "; color: " << fg << "; border: 1px solid " << border << "; border-radius: 8px; padding: 4px 10px; min-height: 28px; }\n"
        << ".sf-listbox, .sf-flowbox { background: transparent; padding: 8px; }\n"
        << ".sf-row { border-radius: 8px; padding: 6px 10px; margin: 1px 0; }\n"
        << ".sf-row:hover { background: " << hover_bg << "; }\n"
        << ".sf-row:selected { background: " << sel_bg << "; }\n"
        << ".sf-grid-card { background: " << card_bg << "; border: 1px solid " << border << "; border-radius: 10px; padding: 10px 8px; min-width: 112px; }\n"
        << ".sf-grid-card:hover { border-color: " << accent << "; }\n"
        << ".sf-muted { color: " << muted << "; font-size: 12px; }\n"
        << ".sf-status { background: " << bar_bg << "; border-top: 1px solid " << border << "; padding: 5px 14px; font-size: 12px; color: " << muted << "; }\n"
        << ".sf-ico-dir { color: #38bdf8; }\n"
        << ".sf-ico-img { color: #f472b6; }\n"
        << ".sf-ico-vid { color: #a78bfa; }\n"
        << ".sf-ico-aud { color: #34d399; }\n"
        << ".sf-ico-arc { color: #fbbf24; }\n"
        << ".sf-ico-code { color: #60a5fa; }\n"
        << ".sf-ico-exec { color: #fb7185; }\n"
        << ".sf-ico-file { color: " << muted << "; }\n"
        << ".sf-menu-item { background: transparent; color: " << fg << "; border: none; border-radius: 6px; padding: 6px 12px; font-size: 13px; }\n"
        << ".sf-menu-item:hover { background: " << hover_bg << "; }\n"
        << ".sf-menu-danger { color: #f43f5e; }\n";

    if (!fw->css) {
        fw->css = gtk_css_provider_new();
        if (GdkDisplay *d = gdk_display_get_default()) {
            gtk_style_context_add_provider_for_display(d, GTK_STYLE_PROVIDER(fw->css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
    }
    gtk_css_provider_load_from_string(fw->css, css.str().c_str());
}

void clear_box_children(GtkWidget *box) {
    if (!box) return;
    while (GtkWidget *c = gtk_widget_get_first_child(box)) {
        if (GTK_IS_LIST_BOX(box)) gtk_list_box_remove(GTK_LIST_BOX(box), c);
        else if (GTK_IS_FLOW_BOX(box)) gtk_flow_box_remove(GTK_FLOW_BOX(box), c);
        else gtk_box_remove(GTK_BOX(box), c);
    }
}

void navigate_to(FilesWin *fw, const fs::path &dir, bool record_history = true);
void refresh_current_dir(FilesWin *fw);

void show_error_alert(FilesWin *fw, const std::string &title, const std::string &detail) {
    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", title.c_str());
    gtk_alert_dialog_set_detail(dlg, detail.c_str());
    const char *btns[] = {"确定", nullptr};
    gtk_alert_dialog_set_buttons(dlg, btns);
    gtk_alert_dialog_choose(dlg, fw->win, nullptr, nullptr, nullptr);
    g_object_unref(dlg);
}

template <typename Fn>
void prompt_text_dialog(FilesWin *fw,
                        const std::string &title,
                        const std::string &initial_val,
                        const std::string &ok_label,
                        Fn on_submit) {
    GtkWindow *dlg = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(dlg, title.c_str());
    gtk_window_set_transient_for(dlg, fw->win);
    gtk_window_set_modal(dlg, TRUE);
    gtk_window_set_default_size(dlg, 380, 120);
    gtk_widget_add_css_class(GTK_WIDGET(dlg), "sf-win");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);

    GtkWidget *entry = gtk_entry_new();
    gtk_widget_add_css_class(entry, "sf-entry");
    gtk_editable_set_text(GTK_EDITABLE(entry), initial_val.c_str());
    gtk_box_append(GTK_BOX(box), entry);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btns, GTK_ALIGN_END);
    GtkWidget *cancel_btn = gtk_button_new_with_label("取消");
    gtk_widget_add_css_class(cancel_btn, "sf-tool-btn");
    GtkWidget *ok_btn = gtk_button_new_with_label(ok_label.c_str());
    gtk_widget_add_css_class(ok_btn, "sf-tool-btn");
    gtk_widget_add_css_class(ok_btn, "is-on");
    gtk_box_append(GTK_BOX(btns), cancel_btn);
    gtk_box_append(GTK_BOX(btns), ok_btn);
    gtk_box_append(GTK_BOX(box), btns);

    gtk_window_set_child(dlg, box);

    struct PromptCtx {
        FilesWin *fw;
        GtkWindow *dlg;
        GtkWidget *entry;
        Fn cb;
    };
    auto *ctx = new PromptCtx{fw, dlg, entry, on_submit};

    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        gtk_window_close(static_cast<PromptCtx *>(d)->dlg);
    }), ctx);
    auto accept_cb = +[](GtkWidget *, gpointer d) {
        auto *c = static_cast<PromptCtx *>(d);
        std::string text = trim_copy(gtk_editable_get_text(GTK_EDITABLE(c->entry)));
        gtk_window_close(c->dlg);
        if (!text.empty()) c->cb(text);
    };
    g_signal_connect(ok_btn, "clicked", G_CALLBACK(accept_cb), ctx);
    g_signal_connect(entry, "activate", G_CALLBACK(accept_cb), ctx);
    g_signal_connect(dlg, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer d) {
        delete static_cast<PromptCtx *>(d);
    }), ctx);

    gtk_window_present(dlg);
    gtk_widget_grab_focus(entry);
}

void do_create_folder(FilesWin *fw) {
    prompt_text_dialog(fw, "新建文件夹", "新建文件夹", "创建", [fw](const std::string &name) {
        if (name.find('/') != std::string::npos || name == "." || name == "..") {
            show_error_alert(fw, "名称无效", "文件名不能包含 / 字符。");
            return;
        }
        std::error_code ec;
        fs::path target = fw->current_dir / name;
        if (!fs::create_directory(target, ec)) {
            show_error_alert(fw, "创建文件夹失败", ec ? ec.message() : "目录已存在或权限不足。");
            return;
        }
        fw->selected_path = target;
        refresh_current_dir(fw);
    });
}

void do_create_file(FilesWin *fw) {
    prompt_text_dialog(fw, "新建空白文件", "untitled.txt", "创建", [fw](const std::string &name) {
        if (name.find('/') != std::string::npos || name == "." || name == "..") {
            show_error_alert(fw, "名称无效", "文件名不能包含 / 字符。");
            return;
        }
        fs::path target = fw->current_dir / name;
        std::error_code ec;
        if (fs::exists(target, ec)) {
            show_error_alert(fw, "创建失败", "同名文件已存在。");
            return;
        }
        std::ofstream ofs(target);
        if (!ofs.is_open()) {
            show_error_alert(fw, "创建失败", "无法写入当前目录。");
            return;
        }
        fw->selected_path = target;
        refresh_current_dir(fw);
    });
}

void do_rename_selected(FilesWin *fw) {
    if (fw->selected_path.empty()) return;
    fs::path old_p = fw->selected_path;
    prompt_text_dialog(fw, "重命名", old_p.filename().string(), "确定", [fw, old_p](const std::string &next_name) {
        if (next_name.find('/') != std::string::npos || next_name == "." || next_name == "..") {
            show_error_alert(fw, "名称无效", "文件名不能包含 / 字符。");
            return;
        }
        fs::path next_p = old_p.parent_path() / next_name;
        if (next_p == old_p) return;
        std::error_code ec;
        if (fs::exists(next_p, ec)) {
            show_error_alert(fw, "重命名失败", "目标名称已存在。");
            return;
        }
        fs::rename(old_p, next_p, ec);
        if (ec) {
            show_error_alert(fw, "重命名失败", ec.message());
            return;
        }
        fw->selected_path = next_p;
        refresh_current_dir(fw);
    });
}

void do_paste(FilesWin *fw) {
    if (fw->clip_op == ClipOp::None || fw->clip_paths.empty()) return;
    for (const auto &src : fw->clip_paths) {
        std::error_code ec;
        if (!fs::exists(src, ec)) continue;
        std::string stem = src.stem().string();
        std::string ext = src.extension().string();
        if (fs::is_directory(src, ec)) {
            stem = src.filename().string();
            ext.clear();
        }
        fs::path dst = fw->current_dir / src.filename();
        int idx = 1;
        while (fs::exists(dst, ec) && idx < 200) {
            std::string suffix = (idx == 1) ? " (副本)" : (" (副本 " + std::to_string(idx) + ")");
            dst = fw->current_dir / (stem + suffix + ext);
            ++idx;
        }
        if (fw->clip_op == ClipOp::Copy) {
            fs::copy(src, dst, fs::copy_options::recursive, ec);
        } else {
            fs::rename(src, dst, ec);
            if (ec) {
                // Cross-filesystem move fallback: copy then remove source
                ec.clear();
                fs::copy(src, dst, fs::copy_options::recursive, ec);
                if (!ec) fs::remove_all(src, ec);
            }
        }
        if (ec) {
            show_error_alert(fw, fw->clip_op == ClipOp::Copy ? "复制失败" : "移动失败", ec.message());
            return;
        }
        fw->selected_path = dst;
    }
    if (fw->clip_op == ClipOp::Cut) {
        fw->clip_op = ClipOp::None;
        fw->clip_paths.clear();
    }
    refresh_current_dir(fw);
}

void do_delete_selected(FilesWin *fw, bool permanent) {
    if (fw->selected_path.empty()) return;
    fs::path target = fw->selected_path;
    if (!permanent && has_executable("gio")) {
        std::string out;
        int rc = 1;
        if (run_argv_capture({"gio", "trash", target.string()}, out, rc) && rc == 0) {
            fw->selected_path.clear();
            refresh_current_dir(fw);
            return;
        }
    }

    GtkAlertDialog *dlg = gtk_alert_dialog_new("确认永久删除 \"%s\" 吗？", target.filename().string().c_str());
    gtk_alert_dialog_set_detail(dlg, "永久删除后无法从回收站恢复。");
    const char *btns[] = {"取消", "永久删除", nullptr};
    gtk_alert_dialog_set_buttons(dlg, btns);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 0);

    struct DelCtx {
        FilesWin *fw;
        fs::path target;
    };
    gtk_alert_dialog_choose(dlg, fw->win, nullptr, +[](GObject *src, GAsyncResult *res, gpointer d) {
        std::unique_ptr<DelCtx> ctx(static_cast<DelCtx *>(d));
        int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
        if (choice == 1) {
            std::error_code ec;
            fs::remove_all(ctx->target, ec);
            if (ec) {
                show_error_alert(ctx->fw, "删除失败", ec.message());
            } else {
                ctx->fw->selected_path.clear();
                refresh_current_dir(ctx->fw);
            }
        }
    }, new DelCtx{fw, target});
    g_object_unref(dlg);
}

void open_item_action(FilesWin *fw, const fs::path &p) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        navigate_to(fw, p, true);
        return;
    }
    spawn_argv_detached({"xdg-open", p.string()});
}

void open_terminal_at(const fs::path &dir) {
    std::string term = has_executable("spinel-terminal") ? "spinel-terminal" : (has_executable("ptyxis") ? "ptyxis" : "foot");
    spawn_argv_detached({term, "--working-directory", dir.string()});
}

void show_properties(FilesWin *fw, const fs::path &p) {
    std::error_code ec;
    auto st = fs::symlink_status(p, ec);
    std::ostringstream oss;
    oss << "完整路径: " << p.string() << "\n"
        << "类型: " << (fs::is_directory(p, ec) ? "文件夹" : "文件") << "\n";
    if (!fs::is_directory(p, ec)) {
        auto sz = fs::file_size(p, ec);
        oss << "大小: " << format_size(ec ? 0 : sz) << " (" << (ec ? 0 : sz) << " 字节)\n";
    }
    oss << "权限: " << format_perms(st.permissions());
    show_error_alert(fw, "属性 · " + p.filename().string(), oss.str());
}

void show_context_menu(FilesWin *fw, GtkWidget *anchor, double x, double y, const fs::path &item_path) {
    GtkWidget *pop = gtk_popover_new();
    gtk_widget_set_parent(pop, anchor);
    GdkRectangle rect{static_cast<int>(x), static_cast<int>(y), 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);

    auto add_btn = [&](const char *label, bool danger, std::function<void()> fn) {
        GtkWidget *b = gtk_button_new_with_label(label);
        gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
        gtk_widget_set_halign(b, GTK_ALIGN_FILL);
        gtk_widget_add_css_class(b, "sf-menu-item");
        if (danger) gtk_widget_add_css_class(b, "sf-menu-danger");
        if (GtkWidget *lbl = gtk_button_get_child(GTK_BUTTON(b))) {
            gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        }
        auto *heap_fn = new std::function<void()>(std::move(fn));
        g_object_set_data_full(G_OBJECT(b), "sf-cb", heap_fn, +[](gpointer p) {
            delete static_cast<std::function<void()> *>(p);
        });
        g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton *btn, gpointer p_pop) {
            gtk_popover_popdown(GTK_POPOVER(p_pop));
            auto *cb = static_cast<std::function<void()> *>(g_object_get_data(G_OBJECT(btn), "sf-cb"));
            if (cb && *cb) (*cb)();
        }), pop);
        gtk_box_append(GTK_BOX(box), b);
    };

    if (!item_path.empty()) {
        fw->selected_path = item_path;
        add_btn("打开", false, [fw, item_path]() { open_item_action(fw, item_path); });
        if (fs::is_directory(item_path)) {
            add_btn("在此目录打开终端", false, [item_path]() { open_terminal_at(item_path); });
        }
        add_btn("复制 (Ctrl+C)", false, [fw, item_path]() {
            fw->clip_op = ClipOp::Copy;
            fw->clip_paths = {item_path};
        });
        add_btn("剪切 (Ctrl+X)", false, [fw, item_path]() {
            fw->clip_op = ClipOp::Cut;
            fw->clip_paths = {item_path};
        });
        add_btn("重命名 (F2)", false, [fw]() { do_rename_selected(fw); });
        add_btn("复制完整路径", false, [fw, item_path]() {
            if (GdkDisplay *d = gdk_display_get_default()) {
                gdk_clipboard_set_text(gdk_display_get_clipboard(d), item_path.string().c_str());
            }
            if (fw->status_label) {
                gtk_label_set_text(GTK_LABEL(fw->status_label), ("已复制路径: " + item_path.string()).c_str());
            }
        });
        add_btn("属性", false, [fw, item_path]() { show_properties(fw, item_path); });
        add_btn("移至回收站 (Delete)", true, [fw]() { do_delete_selected(fw, false); });
    } else {
        add_btn("新建文件夹", false, [fw]() { do_create_folder(fw); });
        add_btn("新建空白文件", false, [fw]() { do_create_file(fw); });
        if (fw->clip_op != ClipOp::None && !fw->clip_paths.empty()) {
            add_btn("粘贴到此处 (Ctrl+V)", false, [fw]() { do_paste(fw); });
        }
        add_btn("在这里打开终端", false, [fw]() { open_terminal_at(fw->current_dir); });
        add_btn("刷新", false, [fw]() { refresh_current_dir(fw); });
    }

    gtk_popover_set_child(GTK_POPOVER(pop), box);
    g_signal_connect(pop, "closed", G_CALLBACK(+[](GtkPopover *p, gpointer) {
        gtk_widget_unparent(GTK_WIDGET(p));
    }), nullptr);
    gtk_popover_popup(GTK_POPOVER(pop));
}

void rebuild_sidebar(FilesWin *fw) {
    clear_box_children(fw->places_box);
    clear_box_children(fw->drives_box);

    fs::path home(home_dir());
    struct PlaceDef { const char *name; fs::path path; const char *icon; };
    std::vector<PlaceDef> places = {
        {"主目录", home, "user-home-symbolic"},
        {"桌面", home / "Desktop", "user-desktop-symbolic"},
        {"下载", home / "Downloads", "folder-download-symbolic"},
        {"文档", home / "Documents", "folder-documents-symbolic"},
        {"图片", home / "Pictures", "folder-pictures-symbolic"},
        {"文件系统根目录", fs::path("/"), "drive-harddisk-symbolic"}
    };

    for (const auto &pl : places) {
        std::error_code ec;
        if (!fs::exists(pl.path, ec)) continue;
        GtkWidget *btn = gtk_button_new();
        gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
        gtk_widget_add_css_class(btn, "sf-nav-btn");
        if (fw->current_dir == pl.path) gtk_widget_add_css_class(btn, "is-active");
        GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *ico = gtk_image_new_from_icon_name(pl.icon);
        GtkWidget *lbl = gtk_label_new(pl.name);
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(hb), ico);
        gtk_box_append(GTK_BOX(hb), lbl);
        gtk_button_set_child(GTK_BUTTON(btn), hb);

        auto *p = new fs::path(pl.path);
        g_object_set_data_full(G_OBJECT(btn), "sf-path", p, +[](gpointer x) { delete static_cast<fs::path *>(x); });
        g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer d) {
            auto *target = static_cast<fs::path *>(g_object_get_data(G_OBJECT(b), "sf-path"));
            if (target) navigate_to(static_cast<FilesWin *>(d), *target, true);
        }), fw);
        gtk_box_append(GTK_BOX(fw->places_box), btn);
    }

    auto mounts = scan_external_mounts();
    if (mounts.empty()) {
        GtkWidget *empty = gtk_label_new("暂无外置挂载盘");
        gtk_widget_add_css_class(empty, "sf-muted");
        gtk_widget_set_halign(empty, GTK_ALIGN_START);
        gtk_widget_set_margin_start(empty, 8);
        gtk_box_append(GTK_BOX(fw->drives_box), empty);
    } else {
        for (const auto &m : mounts) {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
            GtkWidget *btn = gtk_button_new();
            gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
            gtk_widget_set_hexpand(btn, TRUE);
            gtk_widget_add_css_class(btn, "sf-nav-btn");
            if (fw->current_dir == m.mount_path) gtk_widget_add_css_class(btn, "is-active");
            GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_box_append(GTK_BOX(hb), gtk_image_new_from_icon_name("drive-removable-media-symbolic"));
            GtkWidget *lbl = gtk_label_new(m.label.c_str());
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
            gtk_widget_set_halign(lbl, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(hb), lbl);
            gtk_button_set_child(GTK_BUTTON(btn), hb);

            auto *p = new fs::path(m.mount_path);
            g_object_set_data_full(G_OBJECT(btn), "sf-path", p, +[](gpointer x) { delete static_cast<fs::path *>(x); });
            g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer d) {
                auto *target = static_cast<fs::path *>(g_object_get_data(G_OBJECT(b), "sf-path"));
                if (target) navigate_to(static_cast<FilesWin *>(d), *target, true);
            }), fw);
            gtk_box_append(GTK_BOX(row), btn);

            if (m.removable) {
                GtkWidget *ej = gtk_button_new_from_icon_name("media-eject-symbolic");
                gtk_button_set_has_frame(GTK_BUTTON(ej), FALSE);
                gtk_widget_set_tooltip_text(ej, "卸载此分区");
                auto *devp = new std::string(m.device);
                g_object_set_data_full(G_OBJECT(ej), "sf-dev", devp, +[](gpointer x) { delete static_cast<std::string *>(x); });
                g_signal_connect(ej, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer d) {
                    auto *win = static_cast<FilesWin *>(d);
                    auto *dev = static_cast<std::string *>(g_object_get_data(G_OBJECT(b), "sf-dev"));
                    if (!dev) return;
                    std::string out;
                    int rc = 1;
                    if (run_argv_capture({"udisksctl", "unmount", "--no-user-interaction", "-b", *dev}, out, rc) && rc == 0) {
                        navigate_to(win, fs::path(home_dir()), true);
                    } else {
                        show_error_alert(win, "卸载失败", out.empty() ? *dev : out);
                    }
                }), fw);
                gtk_box_append(GTK_BOX(row), ej);
            }
            gtk_box_append(GTK_BOX(fw->drives_box), row);
        }
    }
}

void rebuild_breadcrumbs(FilesWin *fw) {
    clear_box_children(fw->crumb_box);
    std::vector<fs::path> parts;
    fs::path cur = fw->current_dir;
    while (!cur.empty()) {
        parts.push_back(cur);
        fs::path par = cur.parent_path();
        if (par == cur) break;
        cur = par;
    }
    std::reverse(parts.begin(), parts.end());
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            GtkWidget *sep = gtk_label_new("›");
            gtk_widget_add_css_class(sep, "sf-muted");
            gtk_box_append(GTK_BOX(fw->crumb_box), sep);
        }
        std::string txt = parts[i].filename().string();
        if (txt.empty()) txt = "/";
        GtkWidget *b = gtk_button_new_with_label(txt.c_str());
        gtk_widget_add_css_class(b, "sf-crumb-pill");
        auto *p = new fs::path(parts[i]);
        g_object_set_data_full(G_OBJECT(b), "sf-path", p, +[](gpointer x) { delete static_cast<fs::path *>(x); });
        g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton *btn, gpointer d) {
            auto *target = static_cast<fs::path *>(g_object_get_data(G_OBJECT(btn), "sf-path"));
            if (target) navigate_to(static_cast<FilesWin *>(d), *target, true);
        }), fw);
        gtk_box_append(GTK_BOX(fw->crumb_box), b);
    }
    gtk_editable_set_text(GTK_EDITABLE(fw->path_entry), fw->current_dir.string().c_str());
}

void attach_item_gestures(FilesWin *fw, GtkWidget *widget, const fs::path &item_path) {
    auto *p = new fs::path(item_path);
    g_object_set_data_full(G_OBJECT(widget), "sf-item-path", p, +[](gpointer x) { delete static_cast<fs::path *>(x); });

    GtkGesture *click1 = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click1), 1);
    g_signal_connect(click1, "pressed", G_CALLBACK(+[](GtkGestureClick *g, gint n_press, gdouble, gdouble, gpointer d) {
        auto *win = static_cast<FilesWin *>(d);
        GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
        auto *ip = static_cast<fs::path *>(g_object_get_data(G_OBJECT(w), "sf-item-path"));
        if (!ip) return;
        win->selected_path = *ip;
        if (n_press == 2) {
            open_item_action(win, *ip);
        }
    }), fw);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click1));

    GtkGesture *click3 = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click3), 3);
    g_signal_connect(click3, "pressed", G_CALLBACK(+[](GtkGestureClick *g, gint, gdouble x, gdouble y, gpointer d) {
        auto *win = static_cast<FilesWin *>(d);
        GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
        auto *ip = static_cast<fs::path *>(g_object_get_data(G_OBJECT(w), "sf-item-path"));
        show_context_menu(win, w, x, y, ip ? *ip : fs::path{});
    }), fw);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click3));
}

void render_visible_items(FilesWin *fw) {
    clear_box_children(fw->list_view);
    clear_box_children(fw->grid_view);

    std::string q = to_lower_copy(trim_copy(gtk_editable_get_text(GTK_EDITABLE(fw->search_entry))));
    fw->visible_items.clear();
    for (const auto &it : fw->items) {
        if (!fw->show_hidden && !it.name.empty() && it.name.front() == '.') continue;
        if (!q.empty() && to_lower_copy(it.name).find(q) == std::string::npos) continue;
        fw->visible_items.push_back(it);
    }

    for (const auto &it : fw->visible_items) {
        FileBadge badge = classify_item(it);
        if (!fw->grid_mode) {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_widget_add_css_class(row, "sf-row");
            GtkWidget *ico = gtk_image_new_from_icon_name(badge.icon_name);
            gtk_widget_add_css_class(ico, badge.css_class);
            GtkWidget *name = gtk_label_new(it.name.c_str());
            gtk_widget_set_hexpand(name, TRUE);
            gtk_widget_set_halign(name, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);

            GtkWidget *sz = gtk_label_new(it.is_dir ? "文件夹" : format_size(it.size).c_str());
            gtk_widget_add_css_class(sz, "sf-muted");
            gtk_widget_set_size_request(sz, 86, -1);
            gtk_label_set_xalign(GTK_LABEL(sz), 1.0f);

            GtkWidget *mt = gtk_label_new(format_time(it.mtime).c_str());
            gtk_widget_add_css_class(mt, "sf-muted");
            gtk_widget_set_size_request(mt, 130, -1);
            gtk_label_set_xalign(GTK_LABEL(mt), 1.0f);

            gtk_box_append(GTK_BOX(row), ico);
            gtk_box_append(GTK_BOX(row), name);
            gtk_box_append(GTK_BOX(row), sz);
            gtk_box_append(GTK_BOX(row), mt);
            attach_item_gestures(fw, row, it.path);
            gtk_list_box_append(GTK_LIST_BOX(fw->list_view), row);
        } else {
            GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
            gtk_widget_add_css_class(card, "sf-grid-card");
            GtkWidget *ico = gtk_image_new_from_icon_name(badge.icon_name);
            gtk_image_set_pixel_size(GTK_IMAGE(ico), 36);
            gtk_widget_add_css_class(ico, badge.css_class);
            GtkWidget *name = gtk_label_new(it.name.c_str());
            gtk_label_set_max_width_chars(GTK_LABEL(name), 14);
            gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
            GtkWidget *sub = gtk_label_new(it.is_dir ? "文件夹" : format_size(it.size).c_str());
            gtk_widget_add_css_class(sub, "sf-muted");
            gtk_box_append(GTK_BOX(card), ico);
            gtk_box_append(GTK_BOX(card), name);
            gtk_box_append(GTK_BOX(card), sub);
            attach_item_gestures(fw, card, it.path);
            gtk_flow_box_append(GTK_FLOW_BOX(fw->grid_view), card);
        }
    }

    gtk_stack_set_visible_child_name(GTK_STACK(fw->view_stack), fw->grid_mode ? "grid" : "list");

    std::ostringstream status;
    status << "共 " << fw->visible_items.size() << " 项 · Ctrl+H 隐藏文件 · Ctrl+L 输路径 · F2 重命名 · Ctrl+C/X/V · Delete 回收站";
    gtk_label_set_text(GTK_LABEL(fw->status_label), status.str().c_str());
}

void refresh_current_dir(FilesWin *fw) {
    fw->items.clear();
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(fw->current_dir, fs::directory_options::skip_permission_denied, ec)) {
        DirItem it;
        it.name = entry.path().filename().string();
        it.path = entry.path();
        std::error_code sec;
        auto sym = entry.symlink_status(sec);
        it.is_symlink = fs::is_symlink(sym);
        auto st = entry.status(sec);
        it.is_dir = fs::is_directory(st);
        if (!it.is_dir && fs::is_regular_file(st)) {
            it.size = entry.file_size(sec);
            it.is_exec = (st.permissions() & fs::perms::owner_exec) != fs::perms::none;
        }
        auto ft = entry.last_write_time(sec);
        if (!sec) {
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            it.mtime = std::chrono::system_clock::to_time_t(sctp);
        }
        fw->items.push_back(std::move(it));
    }

    std::sort(fw->items.begin(), fw->items.end(), [](const DirItem &a, const DirItem &b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return to_lower_copy(a.name) < to_lower_copy(b.name);
    });

    gtk_widget_set_sensitive(fw->btn_back, fw->hist_idx > 0);
    gtk_widget_set_sensitive(fw->btn_fwd, fw->hist_idx + 1 < static_cast<int>(fw->history.size()));
    gtk_widget_set_sensitive(fw->btn_up, fw->current_dir.has_parent_path() && fw->current_dir.parent_path() != fw->current_dir);

    std::string title = fw->current_dir.filename().string();
    if (title.empty()) title = fw->current_dir.string();
    gtk_window_set_title(fw->win, ("Spinel Files · " + title).c_str());

    rebuild_sidebar(fw);
    rebuild_breadcrumbs(fw);
    render_visible_items(fw);
}

void navigate_to(FilesWin *fw, const fs::path &dir, bool record_history) {
    std::error_code ec;
    fs::path clean = fs::weakly_canonical(dir, ec);
    if (ec || !fs::is_directory(clean, ec)) {
        clean = dir;
        if (!fs::is_directory(clean, ec)) {
            show_error_alert(fw, "无法打开目录", dir.string());
            return;
        }
    }
    if (record_history) {
        if (fw->hist_idx >= 0 && fw->hist_idx < static_cast<int>(fw->history.size()) && fw->history[fw->hist_idx] == clean) {
            refresh_current_dir(fw);
            return;
        }
        if (fw->hist_idx + 1 < static_cast<int>(fw->history.size())) {
            fw->history.resize(static_cast<size_t>(fw->hist_idx + 1));
        }
        fw->history.push_back(clean);
        fw->hist_idx = static_cast<int>(fw->history.size()) - 1;
    }
    fw->current_dir = clean;
    fw->selected_path.clear();
    refresh_current_dir(fw);
}

void set_path_edit_mode(FilesWin *fw, bool on) {
    fw->path_edit_mode = on;
    gtk_widget_set_visible(fw->crumb_scroll, !on);
    gtk_widget_set_visible(fw->path_entry, on);
    if (on) {
        gtk_editable_set_text(GTK_EDITABLE(fw->path_entry), fw->current_dir.string().c_str());
        gtk_widget_grab_focus(fw->path_entry);
    }
}

gboolean on_key_pressed(GtkEventControllerKey *, guint keyval, guint, GdkModifierType state, gpointer d) {
    auto *fw = static_cast<FilesWin *>(d);
    bool ctrl = (state & GDK_CONTROL_MASK) != 0;
    bool shift = (state & GDK_SHIFT_MASK) != 0;

    if (ctrl && (keyval == GDK_KEY_h || keyval == GDK_KEY_H)) {
        fw->show_hidden = !fw->show_hidden;
        if (fw->show_hidden) gtk_widget_add_css_class(fw->btn_hidden, "is-on");
        else gtk_widget_remove_css_class(fw->btn_hidden, "is-on");
        render_visible_items(fw);
        return TRUE;
    }
    if (ctrl && (keyval == GDK_KEY_l || keyval == GDK_KEY_L)) {
        set_path_edit_mode(fw, !fw->path_edit_mode);
        return TRUE;
    }
    if (ctrl && (keyval == GDK_KEY_f || keyval == GDK_KEY_F)) {
        gtk_widget_grab_focus(fw->search_entry);
        return TRUE;
    }
    if (ctrl && shift && (keyval == GDK_KEY_N || keyval == GDK_KEY_n)) {
        do_create_folder(fw);
        return TRUE;
    }
    if (keyval == GDK_KEY_F2) {
        do_rename_selected(fw);
        return TRUE;
    }
    if (ctrl && (keyval == GDK_KEY_c || keyval == GDK_KEY_C) && !gtk_widget_has_focus(fw->search_entry) && !gtk_widget_has_focus(fw->path_entry)) {
        if (!fw->selected_path.empty()) {
            fw->clip_op = ClipOp::Copy;
            fw->clip_paths = {fw->selected_path};
        }
        return TRUE;
    }
    if (ctrl && (keyval == GDK_KEY_x || keyval == GDK_KEY_X) && !gtk_widget_has_focus(fw->search_entry) && !gtk_widget_has_focus(fw->path_entry)) {
        if (!fw->selected_path.empty()) {
            fw->clip_op = ClipOp::Cut;
            fw->clip_paths = {fw->selected_path};
        }
        return TRUE;
    }
    if (ctrl && (keyval == GDK_KEY_v || keyval == GDK_KEY_V) && !gtk_widget_has_focus(fw->search_entry) && !gtk_widget_has_focus(fw->path_entry)) {
        do_paste(fw);
        return TRUE;
    }
    if (keyval == GDK_KEY_Delete && !gtk_widget_has_focus(fw->search_entry) && !gtk_widget_has_focus(fw->path_entry)) {
        do_delete_selected(fw, shift);
        return TRUE;
    }
    if (keyval == GDK_KEY_Escape && fw->path_edit_mode) {
        set_path_edit_mode(fw, false);
        return TRUE;
    }
    return FALSE;
}

void build_files_window(GtkApplication *app, const fs::path &start_dir) {
    auto *fw = new FilesWin();
    fw->app = app;
    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    fw->win = win;
    gtk_window_set_default_size(win, 1020, 660);
    gtk_widget_add_css_class(GTK_WIDGET(win), "sf-win");

    fw->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // Top toolbar
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(bar, "sf-toolbar");

    fw->btn_back = gtk_button_new_from_icon_name("go-previous-symbolic");
    fw->btn_fwd = gtk_button_new_from_icon_name("go-next-symbolic");
    fw->btn_up = gtk_button_new_from_icon_name("go-up-symbolic");
    for (GtkWidget *b : {fw->btn_back, fw->btn_fwd, fw->btn_up}) {
        gtk_widget_add_css_class(b, "sf-tool-btn");
    }
    g_signal_connect(fw->btn_back, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        auto *w = static_cast<FilesWin *>(d);
        if (w->hist_idx > 0) navigate_to(w, w->history[--w->hist_idx], false);
    }), fw);
    g_signal_connect(fw->btn_fwd, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        auto *w = static_cast<FilesWin *>(d);
        if (w->hist_idx + 1 < static_cast<int>(w->history.size())) navigate_to(w, w->history[++w->hist_idx], false);
    }), fw);
    g_signal_connect(fw->btn_up, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        auto *w = static_cast<FilesWin *>(d);
        if (w->current_dir.has_parent_path()) navigate_to(w, w->current_dir.parent_path(), true);
    }), fw);

    fw->crumb_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    fw->crumb_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fw->crumb_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(fw->crumb_scroll), fw->crumb_box);
    gtk_widget_set_hexpand(fw->crumb_scroll, TRUE);

    fw->path_entry = gtk_entry_new();
    gtk_widget_add_css_class(fw->path_entry, "sf-entry");
    gtk_widget_set_hexpand(fw->path_entry, TRUE);
    gtk_widget_set_visible(fw->path_entry, FALSE);
    g_signal_connect(fw->path_entry, "activate", G_CALLBACK(+[](GtkEntry *e, gpointer d) {
        auto *w = static_cast<FilesWin *>(d);
        std::string txt = trim_copy(gtk_editable_get_text(GTK_EDITABLE(e)));
        set_path_edit_mode(w, false);
        if (!txt.empty()) navigate_to(w, fs::path(txt), true);
    }), fw);

    fw->btn_path_edit = gtk_button_new_from_icon_name("document-edit-symbolic");
    gtk_widget_add_css_class(fw->btn_path_edit, "sf-tool-btn");
    gtk_widget_set_tooltip_text(fw->btn_path_edit, "编辑路径 (Ctrl+L)");
    g_signal_connect(fw->btn_path_edit, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        auto *w = static_cast<FilesWin *>(d);
        set_path_edit_mode(w, !w->path_edit_mode);
    }), fw);

    fw->search_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(fw->search_entry), "筛选当前目录...");
    gtk_widget_add_css_class(fw->search_entry, "sf-entry");
    gtk_widget_set_size_request(fw->search_entry, 170, -1);
    g_signal_connect(fw->search_entry, "changed", G_CALLBACK(+[](GtkEditable *, gpointer d) {
        render_visible_items(static_cast<FilesWin *>(d));
    }), fw);

    GtkWidget *new_dir_btn = gtk_button_new_with_label("+ 文件夹");
    gtk_widget_add_css_class(new_dir_btn, "sf-tool-btn");
    g_signal_connect(new_dir_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        do_create_folder(static_cast<FilesWin *>(d));
    }), fw);

    fw->btn_hidden = gtk_button_new_with_label("隐藏文件");
    gtk_widget_add_css_class(fw->btn_hidden, "sf-tool-btn");
    g_signal_connect(fw->btn_hidden, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        auto *w = static_cast<FilesWin *>(d);
        w->show_hidden = !w->show_hidden;
        if (w->show_hidden) gtk_widget_add_css_class(w->btn_hidden, "is-on");
        else gtk_widget_remove_css_class(w->btn_hidden, "is-on");
        render_visible_items(w);
    }), fw);

    fw->btn_view_mode = gtk_button_new_with_label("网格视图");
    gtk_widget_add_css_class(fw->btn_view_mode, "sf-tool-btn");
    g_signal_connect(fw->btn_view_mode, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer d) {
        auto *w = static_cast<FilesWin *>(d);
        w->grid_mode = !w->grid_mode;
        gtk_button_set_label(b, w->grid_mode ? "列表视图" : "网格视图");
        render_visible_items(w);
    }), fw);

    GtkWidget *term_btn = gtk_button_new_with_label("打开终端");
    gtk_widget_add_css_class(term_btn, "sf-tool-btn");
    g_signal_connect(term_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        open_terminal_at(static_cast<FilesWin *>(d)->current_dir);
    }), fw);

    for (GtkWidget *w : {fw->btn_back, fw->btn_fwd, fw->btn_up, fw->crumb_scroll, fw->path_entry,
                         fw->btn_path_edit, fw->search_entry, new_dir_btn, fw->btn_hidden, fw->btn_view_mode, term_btn}) {
        gtk_box_append(GTK_BOX(bar), w);
    }

    // Body split: sidebar + main content
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(body, TRUE);

    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(sidebar, "sf-sidebar");
    gtk_widget_set_size_request(sidebar, 204, -1);

    GtkWidget *places_lbl = gtk_label_new("常用位置");
    gtk_widget_add_css_class(places_lbl, "sf-side-title");
    gtk_widget_set_halign(places_lbl, GTK_ALIGN_START);
    fw->places_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    GtkWidget *drives_lbl = gtk_label_new("外置盘与挂载点");
    gtk_widget_add_css_class(drives_lbl, "sf-side-title");
    gtk_widget_set_halign(drives_lbl, GTK_ALIGN_START);
    fw->drives_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    GtkWidget *side_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(sidebar), places_lbl);
    gtk_box_append(GTK_BOX(sidebar), fw->places_box);
    gtk_box_append(GTK_BOX(sidebar), drives_lbl);
    gtk_box_append(GTK_BOX(sidebar), fw->drives_box);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(side_scroll), sidebar);

    fw->list_view = gtk_list_box_new();
    gtk_widget_add_css_class(fw->list_view, "sf-listbox");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(fw->list_view), GTK_SELECTION_SINGLE);

    fw->grid_view = gtk_flow_box_new();
    gtk_widget_add_css_class(fw->grid_view, "sf-flowbox");
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(fw->grid_view), GTK_SELECTION_SINGLE);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(fw->grid_view), 10);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(fw->grid_view), 10);

    fw->view_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(fw->view_stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_add_named(GTK_STACK(fw->view_stack), fw->list_view, "list");
    gtk_stack_add_named(GTK_STACK(fw->view_stack), fw->grid_view, "grid");

    fw->content_scroll = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(fw->content_scroll, TRUE);
    gtk_widget_set_vexpand(fw->content_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(fw->content_scroll), fw->view_stack);

    // Right-click on empty background inside content scroll
    GtkGesture *bg_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(bg_click), 3);
    g_signal_connect(bg_click, "pressed", G_CALLBACK(+[](GtkGestureClick *g, gint, gdouble x, gdouble y, gpointer d) {
        auto *w = static_cast<FilesWin *>(d);
        show_context_menu(w, gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)), x, y, fs::path{});
    }), fw);
    gtk_widget_add_controller(fw->content_scroll, GTK_EVENT_CONTROLLER(bg_click));

    gtk_box_append(GTK_BOX(body), side_scroll);
    gtk_box_append(GTK_BOX(body), fw->content_scroll);

    fw->status_label = gtk_label_new("");
    gtk_widget_add_css_class(fw->status_label, "sf-status");
    gtk_widget_set_halign(fw->status_label, GTK_ALIGN_FILL);
    gtk_label_set_xalign(GTK_LABEL(fw->status_label), 0.0f);

    gtk_box_append(GTK_BOX(fw->root), bar);
    gtk_box_append(GTK_BOX(fw->root), body);
    gtk_box_append(GTK_BOX(fw->root), fw->status_label);
    gtk_window_set_child(win, fw->root);

    apply_theme_css(fw);

    GtkEventController *key_ctl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctl, "key-pressed", G_CALLBACK(on_key_pressed), fw);
    gtk_widget_add_controller(GTK_WIDGET(win), key_ctl);

    fw->theme_timer = g_timeout_add(1200, +[](gpointer d) -> gboolean {
        apply_theme_css(static_cast<FilesWin *>(d));
        return G_SOURCE_CONTINUE;
    }, fw);

    g_signal_connect(win, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer d) {
        auto *w = static_cast<FilesWin *>(d);
        if (w->theme_timer) g_source_remove(w->theme_timer);
        if (w->css) {
            if (GdkDisplay *disp = gdk_display_get_default()) {
                gtk_style_context_remove_provider_for_display(disp, GTK_STYLE_PROVIDER(w->css));
            }
            g_object_unref(w->css);
        }
        delete w;
    }), fw);

    navigate_to(fw, start_dir, true);
    gtk_window_present(win);
}

fs::path resolve_start_dir(int argc, char **argv) {
    if (argc >= 2 && argv[1] && *argv[1]) {
        std::string arg = argv[1];
        if (arg.rfind("file://", 0) == 0) arg = arg.substr(7);
        fs::path p(arg);
        std::error_code ec;
        if (fs::is_directory(p, ec)) return p;
        if (fs::is_regular_file(p, ec) && p.has_parent_path()) return p.parent_path();
    }
    return fs::path(home_dir());
}

}  // namespace

int main(int argc, char **argv) {
    ensure_display_env();
    const char *wlr = std::getenv("WLR_RENDERER");
    if (wlr && std::string(wlr) == "pixman") {
        setenv("GSK_RENDERER", "cairo", 0);
    }

    fs::path start_dir = resolve_start_dir(argc, argv);
    GtkApplication *app = gtk_application_new("org.spinelos.files", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication *gapp, gpointer d) {
        build_files_window(gapp, *static_cast<fs::path *>(d));
    }), &start_dir);
    int rc = g_application_run(G_APPLICATION(app), 0, nullptr);
    g_object_unref(app);
    return rc;
}
