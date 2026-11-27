#include "common.hpp"
#include "file_associations.hpp"
#include "files_ops.hpp"
#include "files_theme.hpp"
#include "sandbox_manager.hpp"
#include "wine_launcher.hpp"
#include "windows_apps_window.hpp"

#include <gtk/gtk.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

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

// Lightweight GObject wrapper so GListStore + GtkListView/GtkGridView virtualize 100k+ items effortlessly
struct _SpinelFileObj {
    GObject parent_instance;
    DirItem *item;
};

struct _SpinelFileObjClass {
    GObjectClass parent_class;
};

static GType spinel_file_obj_get_type(void);
G_DEFINE_TYPE(_SpinelFileObj, spinel_file_obj, G_TYPE_OBJECT)

static void spinel_file_obj_finalize(GObject *obj) {
    auto *self = reinterpret_cast<_SpinelFileObj *>(obj);
    delete self->item;
    self->item = nullptr;
    G_OBJECT_CLASS(spinel_file_obj_parent_class)->finalize(obj);
}

static void spinel_file_obj_class_init(_SpinelFileObjClass *klass) {
    G_OBJECT_CLASS(klass)->finalize = spinel_file_obj_finalize;
}

static void spinel_file_obj_init(_SpinelFileObj *self) {
    self->item = nullptr;
}

static _SpinelFileObj *spinel_file_obj_new(const DirItem &src) {
    auto *obj = static_cast<_SpinelFileObj *>(g_object_new(spinel_file_obj_get_type(), nullptr));
    obj->item = new DirItem(src);
    return obj;
}

struct FilesWin {
    GtkApplication *app = nullptr;
    GtkWindow *win = nullptr;
    guint search_timer = 0;

    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<uint64_t>> scan_gen = std::make_shared<std::atomic<uint64_t>>(0);

    GtkWidget *root = nullptr;
    GtkWidget *btn_back = nullptr;
    GtkWidget *btn_fwd = nullptr;
    GtkWidget *btn_up = nullptr;
    GtkWidget *btn_view_mode = nullptr;
    GtkWidget *btn_hidden = nullptr;
    GtkWidget *btn_path_edit = nullptr;
    GtkWidget *btn_empty_trash = nullptr;

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

    bool pick_dir_mode = false;
    std::string *pick_dir_out = nullptr;
    GtkWidget *pick_label = nullptr;

    GListStore *store = nullptr;
    GtkMultiSelection *selection = nullptr;

    fs::path current_dir;
    std::vector<fs::path> history;
    int hist_idx = -1;
    std::vector<DirItem> items;
    size_t visible_count = 0;
    fs::path selected_path;
    std::vector<fs::path> selected_paths;
    std::uintmax_t selected_bytes = 0;
    fs::path pending_select;
    bool show_hidden = false;
    bool grid_mode = false;
    bool path_edit_mode = false;
    bool scanning = false;

    ClipOp clip_op = ClipOp::None;
    std::vector<fs::path> clip_paths;
};

using namespace spinel::files;

struct FileBadge {
    const char *icon_name;
    const char *css_class;
};

FileBadge classify_item(const DirItem &it) {
    if (it.is_dir) return {"folder-symbolic", "sf-ico-dir"};
    std::string ext = to_lower_copy(it.path.extension().string());
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".gif" || ext == ".svg") {
        return {"image-x-generic-symbolic", "sf-ico-img"};
    }
    if (ext == ".mp4" || ext == ".mkv" || ext == ".webm" || ext == ".mov" || ext == ".avi") {
        return {"video-x-generic-symbolic", "sf-ico-vid"};
    }
    if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg") {
        return {"audio-x-generic-symbolic", "sf-ico-aud"};
    }
    if (ext == ".zip" || ext == ".tar" || ext == ".gz" || ext == ".xz" || ext == ".zst" || ext == ".7z") {
        return {"package-x-generic-symbolic", "sf-ico-arc"};
    }
    if (ext == ".cpp" || ext == ".hpp" || ext == ".c" || ext == ".h" || ext == ".py" ||
        ext == ".js" || ext == ".ts" || ext == ".sh" || ext == ".json" || ext == ".md" ||
        ext == ".txt" || ext == ".conf" || ext == ".css" || ext == ".html") {
        return {"text-x-generic-symbolic", "sf-ico-code"};
    }
    if (it.is_exec || ext == ".appimage") {
        return {"application-x-executable-symbolic", "sf-ico-exec"};
    }
    return {"text-x-generic-symbolic", "sf-ico-file"};
}

// Fast POSIX directory scan (1 fstatat syscall per entry, zero chrono clock conversions)
std::vector<DirItem> scan_dir_posix(const fs::path &dir) {
    std::vector<DirItem> out;
    DIR *dp = opendir(dir.c_str());
    if (!dp) return out;
    int dfd = dirfd(dp);
    struct dirent *de = nullptr;
    while ((de = readdir(dp)) != nullptr) {
        const char *name = de->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }
        DirItem it;
        it.name = name;
        it.path = dir / name;

        struct stat lst{};
        if (fstatat(dfd, name, &lst, AT_SYMLINK_NOFOLLOW) == 0) {
            it.is_symlink = S_ISLNK(lst.st_mode);
            struct stat st = lst;
            if (it.is_symlink) {
                struct stat target_st{};
                if (fstatat(dfd, name, &target_st, 0) == 0) st = target_st;
            }
            it.is_dir = S_ISDIR(st.st_mode);
            if (!it.is_dir && S_ISREG(st.st_mode)) {
                it.size = static_cast<std::uintmax_t>(st.st_size);
                it.is_exec = (st.st_mode & S_IXUSR) != 0;
            }
            it.mtime = st.st_mtime;
        } else {
            it.is_dir = (de->d_type == DT_DIR);
        }
        out.push_back(std::move(it));
    }
    closedir(dp);

    std::sort(out.begin(), out.end(), [](const DirItem &a, const DirItem &b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return to_lower_copy(a.name) < to_lower_copy(b.name);
    });
    return out;
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

void clear_box_children(GtkWidget *box) {
    if (!box) return;
    while (GtkWidget *c = gtk_widget_get_first_child(box)) {
        gtk_box_remove(GTK_BOX(box), c);
    }
}

void navigate_to(FilesWin *fw, const fs::path &dir, bool record_history = true);
void refresh_current_dir(FilesWin *fw);

std::vector<fs::path> get_selected_paths(FilesWin *fw) {
    std::vector<fs::path> res;
    if (!fw || !fw->selection) return res;
    GtkBitset *bs = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(fw->selection));
    if (!bs) return res;
    GtkBitsetIter iter{};
    guint pos = 0;
    if (gtk_bitset_iter_init_first(&iter, bs, &pos)) {
        do {
            auto *fobj = reinterpret_cast<_SpinelFileObj *>(g_list_model_get_item(G_LIST_MODEL(fw->selection), pos));
            if (fobj) {
                if (fobj->item) res.push_back(fobj->item->path);
                g_object_unref(fobj);
            }
        } while (gtk_bitset_iter_next(&iter, &pos));
    }
    gtk_bitset_unref(bs);
    return res;
}

void sync_selection_cache(FilesWin *fw) {
    if (!fw) return;
    fw->selected_paths.clear();
    fw->selected_bytes = 0;
    if (fw->selection) {
        GtkBitset *bs = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(fw->selection));
        if (bs) {
            GtkBitsetIter iter{};
            guint pos = 0;
            if (gtk_bitset_iter_init_first(&iter, bs, &pos)) {
                do {
                    auto *fobj = reinterpret_cast<_SpinelFileObj *>(g_list_model_get_item(G_LIST_MODEL(fw->selection), pos));
                    if (fobj) {
                        if (fobj->item) {
                            fw->selected_paths.push_back(fobj->item->path);
                            if (!fobj->item->is_dir) fw->selected_bytes += fobj->item->size;
                        }
                        g_object_unref(fobj);
                    }
                } while (gtk_bitset_iter_next(&iter, &pos));
            }
            gtk_bitset_unref(bs);
        }
    }
    fw->selected_path = fw->selected_paths.empty() ? fs::path{} : fw->selected_paths.front();
}

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

fs::path trash_root_dir() { return fs::path(home_dir()) / ".local/share/Trash"; }
fs::path trash_files_dir() { return trash_root_dir() / "files"; }
fs::path trash_info_dir() { return trash_root_dir() / "info"; }

bool is_in_trash_dir(const fs::path &dir) {
    fs::path tf = trash_files_dir();
    std::string ds = dir.string();
    std::string tfs = tf.string();
    return ds == tfs || ds.rfind(tfs + "/", 0) == 0;
}

bool trash_has_items() {
    std::error_code ec;
    fs::path tf = trash_files_dir();
    if (!fs::is_directory(tf, ec)) return false;
    for (const auto &e : fs::directory_iterator(tf, fs::directory_options::skip_permission_denied, ec)) {
        (void)e;
        return true;
    }
    return false;
}

void do_restore_from_trash(FilesWin *fw, const std::vector<fs::path> &trashed_items) {
    if (!fw || trashed_items.empty()) return;
    fs::path last_target;
    for (const auto &trashed_item : trashed_items) {
        if (trashed_item.empty()) continue;
        std::error_code ec;
        fs::path info_file = trash_info_dir() / (trashed_item.filename().string() + ".trashinfo");
        fs::path target;
        if (fs::is_regular_file(info_file, ec)) {
            std::ifstream ifs(info_file);
            std::string line;
            while (std::getline(ifs, line)) {
                line = trim_copy(line);
                if (line.rfind("Path=", 0) == 0) {
                    std::string raw = line.substr(5);
                    g_autofree char *unescaped = g_uri_unescape_string(raw.c_str(), nullptr);
                    std::string decoded = (unescaped && *unescaped) ? std::string(unescaped) : raw;
                    fs::path parsed(decoded);
                    target = parsed.is_absolute() ? parsed : (trash_root_dir() / parsed);
                    break;
                }
            }
        }
        if (target.empty()) {
            target = fs::path(home_dir()) / "Desktop" / trashed_item.filename();
        }
        std::string error;
        if (!restore_trashed_path(trashed_item, target, error)) {
            show_error_alert(fw, "还原失败", error);
            refresh_current_dir(fw);
            return;
        }
        std::error_code rm_ec;
        fs::remove(info_file, rm_ec);
        last_target = target;
    }
    fw->selected_path.clear();
    fw->selected_paths.clear();
    refresh_current_dir(fw);
    if (fw->status_label && !last_target.empty()) {
        std::string msg = (trashed_items.size() > 1)
            ? ("已批量还原 " + std::to_string(trashed_items.size()) + " 项")
            : ("已还原到: " + last_target.string());
        gtk_label_set_text(GTK_LABEL(fw->status_label), msg.c_str());
    }
}

void do_empty_trash(FilesWin *fw) {
    if (!fw) return;
    if (!trash_has_items()) {
        if (fw->status_label) gtk_label_set_text(GTK_LABEL(fw->status_label), "回收站已经是空的");
        return;
    }
    GtkAlertDialog *dlg = gtk_alert_dialog_new("确认清空回收站吗？");
    gtk_alert_dialog_set_detail(dlg, "清空后将永久删除回收站内的全部文件（含已挂载分区回收站），无法恢复。");
    const char *btns[] = {"取消", "清空回收站", nullptr};
    gtk_alert_dialog_set_buttons(dlg, btns);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 0);

    gtk_alert_dialog_choose(dlg, fw->win, nullptr, +[](GObject *src, GAsyncResult *res, gpointer d) {
        auto *w = static_cast<FilesWin *>(d);
        int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
        if (choice != 1) return;

        std::string out;
        int rc = 1;
        if (has_executable("gio")) {
            run_argv_capture({"gio", "trash", "--empty"}, out, rc);
        }
        std::error_code ec;
        for (const fs::path &dir : {trash_files_dir(), trash_info_dir(), trash_root_dir() / "expunged"}) {
            if (!fs::is_directory(dir, ec)) continue;
            for (const auto &entry : fs::directory_iterator(dir, ec)) {
                fs::remove_all(entry.path(), ec);
            }
        }
        w->selected_path.clear();
        refresh_current_dir(w);
    }, fw);
    g_object_unref(dlg);
}

void do_delete_selected(FilesWin *fw, bool permanent) {
    std::vector<fs::path> targets = get_selected_paths(fw);
    if (targets.empty() && !fw->selected_path.empty()) {
        targets.push_back(fw->selected_path);
    }
    if (targets.empty()) return;

    permanent = permanent || is_in_trash_dir(fw->current_dir);
    if (!permanent && has_executable("gio")) {
        std::vector<std::string> argv = {"gio", "trash"};
        for (const auto &t : targets) argv.push_back(t.string());
        std::string out;
        int rc = 1;
        if (run_argv_capture(argv, out, rc) && rc == 0) {
            fw->selected_path.clear();
            fw->selected_paths.clear();
            refresh_current_dir(fw);
            return;
        }
    }

    std::string prompt = (targets.size() > 1)
        ? ("确认永久删除选中的 " + std::to_string(targets.size()) + " 个项目吗？")
        : ("确认永久删除 \"" + targets.front().filename().string() + "\" 吗？");
    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", prompt.c_str());
    gtk_alert_dialog_set_detail(dlg, "永久删除后无法从回收站恢复。");
    const char *btns[] = {"取消", "永久删除", nullptr};
    gtk_alert_dialog_set_buttons(dlg, btns);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 0);

    struct DelCtx {
        FilesWin *fw;
        std::vector<fs::path> targets;
    };
    gtk_alert_dialog_choose(dlg, fw->win, nullptr, +[](GObject *src, GAsyncResult *res, gpointer d) {
        std::unique_ptr<DelCtx> ctx(static_cast<DelCtx *>(d));
        int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
        if (choice == 1) {
            std::string first_err;
            for (const auto &target : ctx->targets) {
                std::error_code ec;
                fs::remove_all(target, ec);
                if (!ec && target.parent_path() == trash_files_dir()) {
                    std::error_code iec;
                    fs::remove(trash_info_dir() / (target.filename().string() + ".trashinfo"), iec);
                }
                if (ec && first_err.empty()) first_err = ec.message();
            }
            if (!first_err.empty()) {
                show_error_alert(ctx->fw, "部分或全部删除失败", first_err);
            }
            ctx->fw->selected_path.clear();
            ctx->fw->selected_paths.clear();
            refresh_current_dir(ctx->fw);
        }
    }, new DelCtx{fw, std::move(targets)});
    g_object_unref(dlg);
}

void open_terminal_at(const fs::path &dir) {
    std::string term = has_executable("spinel-terminal") ? "spinel-terminal" : (has_executable("ptyxis") ? "ptyxis" : "foot");
    spawn_argv_detached({term, "--working-directory", dir.string()});
}

bool open_store_for_missing_wine(GtkWindow *parent, const GError *launch_error) {
    if (!g_error_matches(launch_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) || wine_is_available()) return false;
    g_autoptr(GError) store_error = nullptr;
    if (open_wine_store(parent, &store_error)) return true;
    append_app_log("[Wine] 无法打开应用商店：" + std::string(store_error->message));
    return false;
}

bool run_with_wine(FilesWin *fw, const fs::path &path) {
    g_autoptr(GError) error = nullptr;
    g_autoptr(GSubprocess) process = launch_windows_executable(path, &error);
    if (!process) {
        if (open_store_for_missing_wine(fw->win, error)) {
            gtk_label_set_text(GTK_LABEL(fw->status_label), "请在应用商店安装 Wine，安装完成后重新打开此文件。");
            return true;
        }
        show_error_alert(fw, "无法用 Wine 启动", error->message);
        return false;
    }

    g_autofree char *name = g_filename_display_basename(path.c_str());
    const std::string status = "已交给 Wine 启动 " + std::string(name) + "。首次运行会初始化虚拟 C 盘。";
    gtk_label_set_text(GTK_LABEL(fw->status_label), status.c_str());

    struct WineExit {
        FilesWin *window;
        std::shared_ptr<std::atomic<bool>> alive;
    };
    g_subprocess_wait_check_async(process, nullptr, +[](GObject *object, GAsyncResult *result, gpointer data) {
        std::unique_ptr<WineExit> ctx(static_cast<WineExit *>(data));
        g_autoptr(GError) wait_error = nullptr;
        if (g_subprocess_wait_check_finish(G_SUBPROCESS(object), result, &wait_error) ||
            !ctx->alive->load()) return;
        if (wait_error->domain == G_SPAWN_EXIT_ERROR) {
            // A Windows program can return a nonzero exit code after a normal cancellation.
            const std::string status = "Windows 程序已退出（代码 " + std::to_string(wait_error->code) + "）。";
            gtk_label_set_text(GTK_LABEL(ctx->window->status_label), status.c_str());
            return;
        }
        show_error_alert(ctx->window, "Wine 运行失败",
                         std::string(wait_error->message) + "\n日志位于 " + spinel_state_dir() + "/apps.log");
    }, new WineExit{fw, fw->alive});
    return true;
}

int run_external_wine(const fs::path &path) {
    g_autoptr(GError) error = nullptr;
    g_autoptr(GSubprocess) process = launch_windows_executable(path, &error);
    if (process && g_subprocess_wait_check(process, nullptr, &error)) return 0;
    if (!process && open_store_for_missing_wine(nullptr, error)) return 1;

    const bool program_exited = process && g_subprocess_get_if_exited(process);
    const int status = program_exited ? g_subprocess_get_exit_status(process) : 1;
    g_autofree char *name = g_filename_display_basename(path.c_str());
    const std::string title = program_exited ? "Windows 程序已退出" : "无法用 Wine 启动";
    const std::string detail = std::string(name) + "\n" +
        (program_exited ? "退出代码 " + std::to_string(status) : std::string(error->message)) +
        "\n日志位于 " + spinel_state_dir() + "/apps.log";
    append_app_log("[Wine] " + std::string(error->message));
    g_printerr("%s\n%s\n", title.c_str(), detail.c_str());
    g_autofree char *body = g_markup_escape_text(detail.c_str(), -1);
    std::string output;
    int notification_status = 0;
    run_argv_capture({"notify-send", "--app-name=SpinelOS", "--icon=org.spinelos.WindowsApps",
                      "--", title, body}, output, notification_status);
    return status;
}

bool is_wine_open_app(GAppInfo *app) {
    return app && g_strcmp0(g_app_info_get_id(app), wine_open_desktop_id) == 0;
}

bool launch_file_app(FilesWin *fw, GAppInfo *app, GFile *file) {
    if (is_wine_open_app(app)) {
        g_autofree char *path = g_file_get_path(file);
        if (!path) {
            show_error_alert(fw, "无法用 Wine 启动", "请先将此文件复制到本地，再用 Wine 打开。");
            return false;
        }
        return run_with_wine(fw, path);
    }
    g_autoptr(GdkAppLaunchContext) context = gdk_display_get_app_launch_context(
        gtk_widget_get_display(GTK_WIDGET(fw->win)));
    if (context) {
        g_app_launch_context_setenv(G_APP_LAUNCH_CONTEXT(context), "XDG_CURRENT_DESKTOP", "SpinelOS:GNOME");
        for (const auto &kv : collect_active_proxy_envs()) {
            g_app_launch_context_setenv(G_APP_LAUNCH_CONTEXT(context), kv.first.c_str(), kv.second.c_str());
        }
    }
    GList files{file, nullptr, nullptr};
    g_autoptr(GError) error = nullptr;
    if (g_app_info_launch(app, &files, G_APP_LAUNCH_CONTEXT(context), &error)) return true;
    show_error_alert(fw, "无法打开文件", error->message);
    return false;
}

struct OpenWithDialog {
    FilesWin *files;
    GFile *file;
    FileOpenType type;
    GtkWindow *window;
    GtkDropDown *apps_dropdown = nullptr;
    GtkCheckButton *set_default = nullptr;
    std::vector<GAppInfo *> apps;

    ~OpenWithDialog() {
        g_object_unref(file);
        for (auto *app : apps) g_object_unref(app);
    }
};

void choose_file_app(FilesWin *fw, GFile *file, const FileOpenType &type, GAppInfo *current_default) {
    auto dialog = std::unique_ptr<OpenWithDialog>(
        new OpenWithDialog{fw, G_FILE(g_object_ref(file)), type, nullptr, nullptr, nullptr, {}});
    auto add_app = [&](GAppInfo *app) {
        if (!app || (is_wine_open_app(app) && !type.windows_exe) ||
            (!g_app_info_supports_files(app) && !g_app_info_supports_uris(app))) return;
        for (auto *existing : dialog->apps) {
            if (g_app_info_equal(existing, app)) return;
        }
        dialog->apps.push_back(G_APP_INFO(g_object_ref(app)));
    };
    add_app(current_default);
    GList *recommended = g_app_info_get_recommended_for_type(type.content_type.c_str());
    for (auto *entry = recommended; entry; entry = entry->next) {
        auto *app = G_APP_INFO(entry->data);
        if (g_app_info_should_show(app) || is_wine_open_app(app)) add_app(app);
    }
    g_list_free_full(recommended, g_object_unref);

    GList *all = g_app_info_get_all();
    for (auto *entry = all; entry; entry = entry->next) {
        auto *app = G_APP_INFO(entry->data);
        if (g_app_info_should_show(app)) add_app(app);
    }
    g_list_free_full(all, g_object_unref);
    if (dialog->apps.empty()) {
        show_error_alert(fw, "没有可用的打开方式", "系统里找不到可接收此文件的应用。");
        return;
    }

    auto *window = GTK_WINDOW(gtk_window_new());
    dialog->window = window;
    gtk_window_set_title(window, "打开方式");
    gtk_window_set_transient_for(window, fw->win);
    gtk_window_set_destroy_with_parent(window, TRUE);
    gtk_window_set_modal(window, TRUE);
    gtk_window_set_default_size(window, 420, -1);
    gtk_widget_add_css_class(GTK_WIDGET(window), "sf-win");

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);
    gtk_widget_set_margin_top(content, 16);
    gtk_widget_set_margin_bottom(content, 16);
    gtk_window_set_child(window, content);
    g_autofree char *basename = g_file_get_basename(file);
    g_autofree char *file_name = g_filename_display_name(basename);
    GtkWidget *label = gtk_label_new(file_name);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(content), label);

    GtkStringList *names = gtk_string_list_new(nullptr);
    for (auto *app : dialog->apps) {
        std::string name = g_app_info_get_display_name(app);
        if (current_default && g_app_info_equal(app, current_default)) name += "（当前默认）";
        gtk_string_list_append(names, name.c_str());
    }
    dialog->apps_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(names), nullptr));
    gtk_drop_down_set_enable_search(dialog->apps_dropdown, TRUE);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(dialog->apps_dropdown));

    dialog->set_default = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("始终使用所选应用打开这类文件"));
    gtk_widget_set_sensitive(GTK_WIDGET(dialog->set_default), type.can_set_default());
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(dialog->set_default));

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("取消");
    GtkWidget *open = gtk_button_new_with_label("打开");
    gtk_widget_add_css_class(cancel, "sf-tool-btn");
    gtk_widget_add_css_class(open, "sf-tool-btn");
    gtk_widget_add_css_class(open, "is-on");
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), open);
    gtk_box_append(GTK_BOX(content), buttons);

    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), window);
    g_signal_connect(open, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
        auto *ctx = static_cast<OpenWithDialog *>(data);
        const auto selected = gtk_drop_down_get_selected(ctx->apps_dropdown);
        if (selected >= ctx->apps.size()) return;
        auto *app = ctx->apps[selected];
        if (gtk_check_button_get_active(ctx->set_default)) {
            g_autoptr(GError) error = nullptr;
            if (!ctx->type.set_default(app, &error)) {
                show_error_alert(ctx->files, "无法保存默认应用", error->message);
                return;
            }
        }
        if (launch_file_app(ctx->files, app, ctx->file)) gtk_window_destroy(ctx->window);
    }), dialog.get());
    g_object_set_data_full(G_OBJECT(window), "open-with-dialog", dialog.release(),
                          +[](gpointer data) { delete static_cast<OpenWithDialog *>(data); });
    gtk_window_set_default_widget(window, open);
    gtk_window_present(window);
}

void open_item_action(FilesWin *fw, const fs::path &path, bool choose_app = false) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        navigate_to(fw, path, true);
        return;
    }

    g_autoptr(GFile) file = g_file_new_for_path(path.c_str());
    g_autoptr(GError) error = nullptr;
    const auto type = query_file_open_type(file, &error);
    if (!type) {
        show_error_alert(fw, "无法打开文件", error->message);
        return;
    }
    g_autoptr(GAppInfo) app = g_app_info_get_default_for_type(type->content_type.c_str(), false);
    if (is_wine_open_app(app) && !type->windows_exe) g_clear_object(&app);
    if (choose_app || !app) {
        choose_file_app(fw, file, *type, app);
    } else {
        launch_file_app(fw, app, file);
    }
}

void execute_file_direct(FilesWin *fw, const fs::path &p) {
    std::string dir = p.has_parent_path() ? p.parent_path().string() : home_dir();
    auto envs = collect_active_proxy_envs();
    envs.emplace_back("XDG_CURRENT_DESKTOP", "SpinelOS:GNOME");
    spawn_argv_detached({
        "sh", "-c", "cd \"$1\" && exec \"$2\"", "_", dir, p.string()
    }, envs);
    if (fw && fw->status_label) {
        gtk_label_set_text(GTK_LABEL(fw->status_label), ("已启动执行: " + p.filename().string()).c_str());
    }
}

void execute_file_in_terminal(const fs::path &p, bool force_bash) {
    std::string dir = p.has_parent_path() ? p.parent_path().string() : home_dir();
    std::string term = has_executable("spinel-terminal") ? "spinel-terminal" : (has_executable("ptyxis") ? "ptyxis" : "foot");
    if (force_bash) {
        spawn_argv_detached({
            term, "--working-directory", dir, "-e",
            "sh", "-c", "bash \"$1\"; echo \"\"; echo \"[执行结束，按回车关闭]\"; read _", "_", p.string()
        });
    } else {
        spawn_argv_detached({
            term, "--working-directory", dir, "-e",
            "sh", "-c", "\"$1\"; echo \"\"; echo \"[执行结束，按回车关闭]\"; read _", "_", p.string()
        });
    }
}

void toggle_exec_permission(FilesWin *fw, const fs::path &p, bool enable_exec) {
    std::error_code ec;
    auto st = fs::symlink_status(p, ec);
    if (ec) {
        show_error_alert(fw, "修改权限失败", ec.message());
        return;
    }
    auto perms = st.permissions();
    auto next = enable_exec
        ? (perms | fs::perms::owner_exec | fs::perms::group_exec)
        : (perms & ~(fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec));
    fs::permissions(p, next, ec);
    if (ec) {
        show_error_alert(fw, "修改权限失败", ec.message());
        return;
    }
    fw->pending_select = p;
    refresh_current_dir(fw);
}

std::string extract_asar_png_icon(const fs::path &asar_path, const std::string &slug) {
    std::ifstream ifs(asar_path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) return "";
    std::streamsize total_sz = ifs.tellg();
    if (total_sz < 32) return "";
    ifs.seekg(0, std::ios::beg);

    uint8_t hdr[16]{};
    if (!ifs.read(reinterpret_cast<char *>(hdr), 16)) return "";
    auto u32le = [](const uint8_t *p) -> uint32_t {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    };
    uint32_t pickle_sz = u32le(hdr + 4);
    uint32_t jlen = u32le(hdr + 12);
    if (jlen == 0 || jlen > 4u * 1024u * 1024u || 8ULL + pickle_sz > static_cast<uint64_t>(total_sz)) {
        return "";
    }

    std::string jstr(jlen, '\0');
    if (!ifs.read(&jstr[0], jlen)) return "";
    uint64_t base_offset = 8ULL + pickle_sz;

    for (const char *key : {"\"icon.png\":{", "\"code.png\":{", "\"logo.png\":{", "\"app.png\":{"}) {
        size_t pos = jstr.find(key);
        if (pos == std::string::npos) continue;
        size_t obj_start = jstr.find('{', pos);
        size_t obj_end = jstr.find('}', obj_start);
        if (obj_start == std::string::npos || obj_end == std::string::npos) continue;
        std::string sub = jstr.substr(obj_start, obj_end - obj_start + 1);

        auto find_uint = [&](const char *field, uint64_t &out_val) -> bool {
            size_t fp = sub.find(field);
            if (fp == std::string::npos) return false;
            fp = sub.find(':', fp);
            if (fp == std::string::npos) return false;
            ++fp;
            while (fp < sub.size() && (sub[fp] == ' ' || sub[fp] == '"')) ++fp;
            if (fp >= sub.size() || !std::isdigit(static_cast<unsigned char>(sub[fp]))) return false;
            out_val = std::strtoull(sub.c_str() + fp, nullptr, 10);
            return true;
        };

        uint64_t sz = 0, off = 0;
        if (!find_uint("\"size\"", sz) || !find_uint("\"offset\"", off)) continue;
        if (sz < 16 || sz > 4ULL * 1024 * 1024 || base_offset + off + sz > static_cast<uint64_t>(total_sz)) continue;

        std::string bytes(sz, '\0');
        ifs.seekg(static_cast<std::streamoff>(base_offset + off), std::ios::beg);
        if (!ifs.read(&bytes[0], static_cast<std::streamsize>(sz))) continue;
        static const uint8_t kPngMagic[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        if (std::memcmp(bytes.data(), kPngMagic, 8) != 0) continue;

        std::error_code ec;
        fs::path icon_dir = fs::path(home_dir()) / ".local/share/icons/hicolor/256x256/apps";
        fs::create_directories(icon_dir, ec);
        fs::path out_png = icon_dir / ("spinelos-user-" + slug + ".png");
        std::ofstream ofs(out_png, std::ios::binary);
        if (ofs.is_open()) {
            ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            return out_png.string();
        }
    }
    return "";
}

std::string find_nearby_app_icon(const fs::path &exe_path, const std::string &slug) {
    std::error_code ec;
    fs::path dir = exe_path.parent_path();
    std::string stem_low = to_lower_copy(exe_path.stem().string());
    const std::vector<fs::path> probe_dirs = {dir, dir / "resources", dir / "icons"};
    std::string fallback;
    for (const auto &d : probe_dirs) {
        if (!fs::is_directory(d, ec)) continue;
        for (const auto &ent : fs::directory_iterator(d, ec)) {
            if (!ent.is_regular_file(ec)) continue;
            std::string fn = ent.path().filename().string();
            std::string fn_low = to_lower_copy(fn);
            std::string ext = to_lower_copy(ent.path().extension().string());
            if (fn == ".DirIcon" || ext == ".png" || ext == ".svg") {
                if (!stem_low.empty() && fn_low.find(stem_low) != std::string::npos) {
                    return ent.path().string();
                }
                if (fn == ".DirIcon" || fn_low == "icon.png" || fn_low == "icon.svg") {
                    return ent.path().string();
                }
                if (fallback.empty()) fallback = ent.path().string();
            }
        }
    }
    if (!fallback.empty()) return fallback;
    fs::path asar = dir / "resources/app.asar";
    if (fs::is_regular_file(asar, ec)) {
        std::string extracted = extract_asar_png_icon(asar, slug);
        if (!extracted.empty()) return extracted;
    }
    return "application-x-executable";
}

fs::path user_desktop_entry_path(const fs::path &exe_path) {
    return fs::path(home_dir()) / ".local/share/applications" /
           ("spinelos-user-" + sanitize_app_slug(exe_path.filename().string()) + ".desktop");
}

void notify_shell_apps_changed() {
    GError *err = nullptr;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (err) g_error_free(err);
    if (bus) {
        g_dbus_connection_call(
            bus, "org.spinelos.shell", "/org/spinelos/shell", "org.gtk.Actions",
            "Activate",
            g_variant_new("(sava{sv})", "sync-desktop", nullptr, nullptr),
            nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr, nullptr);
        g_object_unref(bus);
    }
}

void do_remove_from_app_center(FilesWin *fw, const fs::path &target) {
    std::error_code ec;
    fs::path desk = user_desktop_entry_path(target);
    fs::remove(desk, ec);
    fs::path apps_dir = fs::path(home_dir()) / ".local/share/applications";
    if (has_executable("update-desktop-database")) {
        std::string out;
        int rc = 1;
        run_argv_capture({"update-desktop-database", apps_dir.string()}, out, rc);
    }
    notify_shell_apps_changed();
    if (fw->status_label) {
        gtk_label_set_text(GTK_LABEL(fw->status_label),
                           ("已从应用中心移除: " + target.filename().string()).c_str());
    }
}

void do_add_to_app_center(FilesWin *fw, const fs::path &target) {
    GtkWindow *dlg = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(dlg, "添加到应用中心与启动器");
    gtk_window_set_transient_for(dlg, fw->win);
    gtk_window_set_modal(dlg, TRUE);
    gtk_window_set_default_size(dlg, 430, -1);
    gtk_widget_add_css_class(GTK_WIDGET(dlg), "sf-win");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);

    GtkWidget *name_lbl = gtk_label_new("应用名称");
    gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);
    GtkWidget *name_entry = gtk_entry_new();
    gtk_widget_add_css_class(name_entry, "sf-entry");
    gtk_editable_set_text(GTK_EDITABLE(name_entry), guess_friendly_app_name(target).c_str());

    GtkWidget *scheme_lbl = gtk_label_new("URL 协议（选填，例如 antigravity，用于浏览器登录回调）");
    gtk_widget_set_halign(scheme_lbl, GTK_ALIGN_START);
    gtk_widget_add_css_class(scheme_lbl, "sf-muted");
    GtkWidget *scheme_entry = gtk_entry_new();
    gtk_widget_add_css_class(scheme_entry, "sf-entry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(scheme_entry), "留空则只创建普通应用快捷方式");

    GtkWidget *term_chk = gtk_check_button_new_with_label("在终端中运行（CLI 脚本或命令行程序勾选）");
    std::string ext_low = to_lower_copy(target.extension().string());
    if (ext_low == ".sh") gtk_check_button_set_active(GTK_CHECK_BUTTON(term_chk), TRUE);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btns, GTK_ALIGN_END);
    GtkWidget *cancel_btn = gtk_button_new_with_label("取消");
    gtk_widget_add_css_class(cancel_btn, "sf-tool-btn");
    GtkWidget *ok_btn = gtk_button_new_with_label("添加到应用中心");
    gtk_widget_add_css_class(ok_btn, "sf-tool-btn");
    gtk_widget_add_css_class(ok_btn, "is-on");
    gtk_box_append(GTK_BOX(btns), cancel_btn);
    gtk_box_append(GTK_BOX(btns), ok_btn);

    gtk_box_append(GTK_BOX(box), name_lbl);
    gtk_box_append(GTK_BOX(box), name_entry);
    gtk_box_append(GTK_BOX(box), scheme_lbl);
    gtk_box_append(GTK_BOX(box), scheme_entry);
    gtk_box_append(GTK_BOX(box), term_chk);
    gtk_box_append(GTK_BOX(box), btns);
    gtk_window_set_child(dlg, box);

    struct AddAppCtx {
        FilesWin *fw;
        GtkWindow *dlg;
        GtkWidget *name_entry;
        GtkWidget *scheme_entry;
        GtkWidget *term_chk;
        fs::path target;
    };
    auto *ctx = new AddAppCtx{fw, dlg, name_entry, scheme_entry, term_chk, target};

    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        gtk_window_close(static_cast<AddAppCtx *>(d)->dlg);
    }), ctx);

    auto submit_cb = +[](GtkWidget *, gpointer d) {
        auto *c = static_cast<AddAppCtx *>(d);
        std::string app_name = trim_copy(gtk_editable_get_text(GTK_EDITABLE(c->name_entry)));
        if (app_name.empty()) app_name = guess_friendly_app_name(c->target);
        for (char &ch : app_name) {
            if (ch == '\n' || ch == '\r') ch = ' ';
        }
        std::string scheme = sanitize_url_scheme(gtk_editable_get_text(GTK_EDITABLE(c->scheme_entry)));
        bool in_term = gtk_check_button_get_active(GTK_CHECK_BUTTON(c->term_chk)) == TRUE;

        std::error_code ec;
        auto st = fs::symlink_status(c->target, ec);
        if (!ec) {
            fs::permissions(c->target, st.permissions() | fs::perms::owner_exec | fs::perms::group_exec, ec);
        }

        fs::path apps_dir = fs::path(home_dir()) / ".local/share/applications";
        fs::create_directories(apps_dir, ec);
        fs::path desk_path = user_desktop_entry_path(c->target);
        std::string desk_id = desk_path.filename().string();

        std::string exe_str = c->target.string();
        std::string quoted_exe;
        quoted_exe.reserve(exe_str.size() + 4);
        quoted_exe += '"';
        for (char ch : exe_str) {
            if (ch == '"' || ch == '\\') quoted_exe += '\\';
            quoted_exe += ch;
        }
        quoted_exe += '"';

        std::string slug = sanitize_app_slug(c->target.filename().string());
        std::string icon = find_nearby_app_icon(c->target, slug);
        std::ostringstream oss;
        oss << "[Desktop Entry]\n"
            << "Name=" << app_name << "\n"
            << "Exec=" << quoted_exe << " %U\n"
            << "Path=" << c->target.parent_path().string() << "\n"
            << "Icon=" << icon << "\n"
            << "Terminal=" << (in_term ? "true" : "false") << "\n"
            << "Type=Application\n"
            << "Categories=Utility;Application;\n"
            << "StartupWMClass=" << app_name << "\n";
        if (!scheme.empty()) {
            oss << "MimeType=x-scheme-handler/" << scheme << ";\n";
        }

        std::ofstream ofs(desk_path);
        bool ok = ofs.is_open();
        if (ok) {
            ofs << oss.str();
            ofs.close();
        }
        FilesWin *win = c->fw;
        gtk_window_close(c->dlg);

        if (!ok) {
            show_error_alert(win, "注册快捷方式失败", "无法写入 " + desk_path.string());
            return;
        }

        if (has_executable("update-desktop-database")) {
            std::string out;
            int rc = 1;
            run_argv_capture({"update-desktop-database", apps_dir.string()}, out, rc);
        }
        if (!scheme.empty() && has_executable("xdg-mime")) {
            std::string out;
            int rc = 1;
            run_argv_capture({"xdg-mime", "default", desk_id, "x-scheme-handler/" + scheme}, out, rc);
        }
        notify_shell_apps_changed();
        if (win && win->status_label) {
            std::string msg = "已添加到应用中心与 Alt+Space 启动器: " + app_name;
            if (!scheme.empty()) msg += "（已绑定 " + scheme + "://）";
            gtk_label_set_text(GTK_LABEL(win->status_label), msg.c_str());
        }
    };

    g_signal_connect(ok_btn, "clicked", G_CALLBACK(submit_cb), ctx);
    g_signal_connect(name_entry, "activate", G_CALLBACK(submit_cb), ctx);
    g_signal_connect(scheme_entry, "activate", G_CALLBACK(submit_cb), ctx);
    g_signal_connect(dlg, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer d) {
        delete static_cast<AddAppCtx *>(d);
    }), ctx);

    gtk_window_present(dlg);
    gtk_widget_grab_focus(name_entry);
}

void refresh_current_dir(FilesWin *fw);

void run_archive_task(FilesWin *fw,
                      const std::string &status_msg,
                      const std::vector<std::string> &argv,
                      const fs::path &select_after) {
    if (fw->status_label) gtk_label_set_text(GTK_LABEL(fw->status_label), status_msg.c_str());
    auto alive = fw->alive;
    struct ArchiveDoneIdle {
        FilesWin *fw;
        std::shared_ptr<std::atomic<bool>> alive;
        int rc;
        std::string out;
        fs::path select_after;
    };
    std::thread([fw, alive, argv, select_after]() {
        std::string out;
        int rc = 1;
        run_argv_capture(argv, out, rc);
        g_idle_add(+[](gpointer d) -> gboolean {
            std::unique_ptr<ArchiveDoneIdle> msg(static_cast<ArchiveDoneIdle *>(d));
            if (!msg->alive->load()) return G_SOURCE_REMOVE;
            if (msg->rc == 0) {
                msg->fw->pending_select = msg->select_after;
                refresh_current_dir(msg->fw);
            } else {
                if (msg->fw->status_label) gtk_label_set_text(GTK_LABEL(msg->fw->status_label), "归档操作失败");
                show_error_alert(msg->fw, "压缩 / 解压失败",
                                 msg->out.empty() ? "请检查文件权限或格式是否完整。" : msg->out);
            }
            return G_SOURCE_REMOVE;
        }, new ArchiveDoneIdle{fw, alive, rc, std::move(out), select_after});
    }).detach();
}

void do_extract_archive(FilesWin *fw, const fs::path &archive, bool into_subdir) {
    if (!has_executable("bsdtar")) {
        show_error_alert(fw, "无法解压", "未找到 bsdtar 命令。");
        return;
    }
    fs::path dest = archive.parent_path();
    fs::path select_target = archive;
    if (into_subdir) {
        dest = archive.parent_path() / strip_archive_ext(archive.filename().string());
        std::error_code ec;
        fs::create_directories(dest, ec);
        if (ec) {
            show_error_alert(fw, "创建解压目录失败", ec.message());
            return;
        }
        select_target = dest;
    }
    run_archive_task(
        fw,
        "正在解压: " + archive.filename().string() + " ...",
        {"sh", "-c",
         "ERR=$(mktemp); "
         "bsdtar -xf \"$1\" -C \"$2\" 2>\"$ERR\" || "
         "(command -v 7z >/dev/null 2>&1 && 7z x -y -o\"$2\" \"$1\") || "
         "{ cat \"$ERR\"; rm -f \"$ERR\"; exit 1; }; "
         "rm -f \"$ERR\"",
         "_", archive.string(), dest.string()},
        select_target
    );
}

void do_compress_zip(FilesWin *fw, const std::vector<fs::path> &targets) {
    if (targets.empty()) return;
    if (!has_executable("bsdtar")) {
        show_error_alert(fw, "无法压缩", "未找到 bsdtar 命令。");
        return;
    }
    std::error_code ec;
    fs::path parent = targets.front().parent_path();
    std::string base;
    if (targets.size() == 1) {
        base = fs::is_directory(targets.front(), ec)
            ? targets.front().filename().string()
            : targets.front().stem().string();
    } else {
        std::string p_name = parent.filename().string();
        base = (!p_name.empty() ? p_name : "archive") + " (" + std::to_string(targets.size()) + "项)";
    }
    if (base.empty()) base = "archive";
    fs::path out_zip = parent / (base + ".zip");
    int idx = 2;
    while (fs::exists(out_zip, ec) && idx < 100) {
        out_zip = parent / (base + " (" + std::to_string(idx++) + ").zip");
    }
    std::vector<std::string> argv = {"bsdtar", "-a", "-cf", out_zip.string(), "-C", parent.string()};
    for (const auto &t : targets) {
        argv.push_back(t.filename().string());
    }
    run_archive_task(fw, "正在压缩: " + out_zip.filename().string() + " ...", argv, out_zip);
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

    std::vector<fs::path> sel = get_selected_paths(fw);
    if (!item_path.empty() && std::find(sel.begin(), sel.end(), item_path) == sel.end()) {
        sel = {item_path};
    }

    if (is_in_trash_dir(fw->current_dir)) {
        if (sel.size() > 1) {
            std::string n_str = std::to_string(sel.size());
            add_btn(("还原选中的 " + n_str + " 项到原位置").c_str(), false, [fw, sel]() { do_restore_from_trash(fw, sel); });
            add_btn(("永久删除 " + n_str + " 项 (Delete)").c_str(), true, [fw]() { do_delete_selected(fw, true); });
        } else if (!item_path.empty()) {
            fw->selected_path = item_path;
            add_btn("还原到原位置", false, [fw, item_path]() { do_restore_from_trash(fw, {item_path}); });
            add_btn("属性", false, [fw, item_path]() { show_properties(fw, item_path); });
            add_btn("永久删除 (Delete)", true, [fw]() { do_delete_selected(fw, true); });
        } else {
            add_btn("清空回收站", true, [fw]() { do_empty_trash(fw); });
            add_btn("刷新", false, [fw]() { refresh_current_dir(fw); });
        }
    } else if (sel.size() > 1) {
        std::string n_str = std::to_string(sel.size());
        add_btn(("压缩这 " + n_str + " 项为 .zip").c_str(), false, [fw, sel]() { do_compress_zip(fw, sel); });
        add_btn(("复制选中的 " + n_str + " 项 (Ctrl+C)").c_str(), false, [fw, sel]() {
            fw->clip_op = ClipOp::Copy;
            fw->clip_paths = sel;
        });
        add_btn(("剪切选中的 " + n_str + " 项 (Ctrl+X)").c_str(), false, [fw, sel]() {
            fw->clip_op = ClipOp::Cut;
            fw->clip_paths = sel;
        });
        add_btn(("移至回收站 " + n_str + " 项 (Delete)").c_str(), true, [fw]() { do_delete_selected(fw, false); });
    } else if (!item_path.empty()) {
        fw->selected_path = item_path;
        std::error_code ec;
        bool is_dir = fs::is_directory(item_path, ec);
        bool is_windows_exe = is_windows_executable(item_path);
        bool is_exec_file = !is_dir && !is_windows_exe && (::access(item_path.c_str(), X_OK) == 0);
        if (is_windows_exe) {
            add_btn("用 Wine 运行", false, [fw, item_path]() { run_with_wine(fw, item_path); });
        } else if (is_exec_file) {
            add_btn("执行", false, [fw, item_path]() { execute_file_direct(fw, item_path); });
            add_btn("在终端中运行", false, [item_path]() { execute_file_in_terminal(item_path, false); });
        } else if (!is_dir && is_script_or_appimage(item_path)) {
            if (to_lower_copy(item_path.extension().string()) == ".sh") {
                add_btn("在终端中运行 (bash)", false, [item_path]() { execute_file_in_terminal(item_path, true); });
            }
            add_btn("设为可执行 (+x)", false, [fw, item_path]() { toggle_exec_permission(fw, item_path, true); });
        }
        if (is_exec_file || (!is_dir && is_script_or_appimage(item_path))) {
            bool registered = fs::exists(user_desktop_entry_path(item_path), ec);
            add_btn(registered ? "更新应用中心快捷方式..." : "添加到应用中心 / 启动器...", false,
                    [fw, item_path]() { do_add_to_app_center(fw, item_path); });
            if (registered) {
                add_btn("从应用中心移除", true, [fw, item_path]() { do_remove_from_app_center(fw, item_path); });
            }
        }
        add_btn("打开", false, [fw, item_path]() { open_item_action(fw, item_path); });
        if (!is_dir) add_btn("打开方式…", false, [fw, item_path]() { open_item_action(fw, item_path, true); });
        if (is_dir) {
            add_btn("在此目录打开终端", false, [item_path]() { open_terminal_at(item_path); });
            add_btn("在平行沙箱中打开工作区", false, [fw, item_path]() {
                if (fw && fw->status_label) {
                    gtk_label_set_text(GTK_LABEL(fw->status_label), "正在拉起平行沙箱桌面...");
                }
                std::thread([item_path]() {
                    std::string err;
                    if (launch_in_sandbox({"spinel-terminal"}, item_path.string(), err)) {
                        if (has_executable("gdbus")) {
                            std::string out;
                            int rc = 1;
                            run_argv_capture({
                                "gdbus", "call", "--session",
                                "--dest", "org.spinelos.shell",
                                "--object-path", "/org/spinelos/shell",
                                "--method", "org.gtk.Actions.Activate",
                                "sync-sandbox", "['show']", "{}"
                            }, out, rc);
                        }
                    } else {
                        append_app_log("[Files Sandbox] failed: " + err);
                    }
                }).detach();
            });
        } else if (is_archive_file(item_path)) {
            add_btn("解压到当前目录", false, [fw, item_path]() { do_extract_archive(fw, item_path, false); });
            add_btn("解压到同名文件夹", false, [fw, item_path]() { do_extract_archive(fw, item_path, true); });
            if (has_executable("file-roller")) {
                add_btn("用归档管理器查看", false, [item_path]() {
                    spawn_argv_detached({"file-roller", item_path.string()});
                });
            }
        }
        if (is_exec_file) {
            add_btn("取消执行权限 (-x)", false, [fw, item_path]() { toggle_exec_permission(fw, item_path, false); });
        }
        add_btn("压缩为 .zip", false, [fw, item_path]() { do_compress_zip(fw, {item_path}); });
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
    std::error_code init_ec;
    fs::create_directories(trash_files_dir(), init_ec);
    fs::create_directories(trash_info_dir(), init_ec);
    struct PlaceDef { const char *name; fs::path path; const char *icon; };
    std::vector<PlaceDef> places = {
        {"主目录", home, "user-home-symbolic"},
        {"桌面", home / "Desktop", "user-desktop-symbolic"},
        {"下载", home / "Downloads", "folder-download-symbolic"},
        {"文档", home / "Documents", "folder-documents-symbolic"},
        {"图片", home / "Pictures", "folder-pictures-symbolic"},
        {"回收站", trash_files_dir(), trash_has_items() ? "user-trash-full-symbolic" : "user-trash-symbolic"},
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
            gtk_label_set_max_width_chars(GTK_LABEL(lbl), 14);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
            gtk_widget_set_hexpand(lbl, TRUE);
            gtk_widget_set_halign(lbl, GTK_ALIGN_FILL);
            gtk_widget_set_tooltip_text(btn, m.mount_path.c_str());
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

void update_status_bar(FilesWin *fw) {
    std::ostringstream status;
    if (fw->scanning) {
        status << "正在读取目录...";
    } else {
        status << "共 " << fw->visible_count << " 项";
        if (fw->selected_paths.size() > 1) {
            status << " · 已选中 " << fw->selected_paths.size() << " 项 (" << format_size(fw->selected_bytes) << ")";
        } else if (!fw->selected_path.empty()) {
            status << " · 已选: " << fw->selected_path.filename().string();
        }
        status << " · Ctrl+A 全选 · Ctrl/Shift 多选 · Ctrl+C/X/V · Delete 回收站";
    }
    gtk_label_set_text(GTK_LABEL(fw->status_label), status.str().c_str());
    if (fw->pick_dir_mode && fw->pick_label) {
        std::error_code ec;
        fs::path cand = fw->current_dir;
        if (!fw->selected_path.empty() && fs::is_directory(fw->selected_path, ec)) {
            cand = fw->selected_path;
        }
        std::string txt = "将选择文件夹: " + cand.string();
        gtk_label_set_text(GTK_LABEL(fw->pick_label), txt.c_str());
    }
}

void apply_visible_filter(FilesWin *fw) {
    std::string q = to_lower_copy(trim_copy(gtk_editable_get_text(GTK_EDITABLE(fw->search_entry))));
    std::vector<gpointer> objs;
    objs.reserve(fw->items.size());
    for (const auto &it : fw->items) {
        if (!fw->show_hidden && !it.name.empty() && it.name.front() == '.') continue;
        if (!q.empty() && to_lower_copy(it.name).find(q) == std::string::npos) continue;
        objs.push_back(spinel_file_obj_new(it));
    }

    guint old_n = g_list_model_get_n_items(G_LIST_MODEL(fw->store));
    g_list_store_splice(fw->store, 0, old_n, objs.data(), static_cast<guint>(objs.size()));
    for (gpointer p : objs) g_object_unref(p);  // GListStore owns a ref

    fw->visible_count = objs.size();
    gtk_stack_set_visible_child_name(GTK_STACK(fw->view_stack), fw->grid_mode ? "grid" : "list");

    if (!fw->pending_select.empty()) {
        std::string target_name = fw->pending_select.filename().string();
        for (guint i = 0; i < static_cast<guint>(objs.size()); ++i) {
            auto *fobj = reinterpret_cast<_SpinelFileObj *>(objs[i]);
            if (fobj && fobj->item &&
                (fobj->item->path == fw->pending_select ||
                 (!target_name.empty() && fobj->item->name == target_name))) {
                gtk_selection_model_select_item(GTK_SELECTION_MODEL(fw->selection), i, TRUE);
                sync_selection_cache(fw);
#if GTK_CHECK_VERSION(4, 12, 0)
                if (fw->grid_mode) {
                    gtk_grid_view_scroll_to(GTK_GRID_VIEW(fw->grid_view), i, GTK_LIST_SCROLL_SELECT, nullptr);
                } else {
                    gtk_list_view_scroll_to(GTK_LIST_VIEW(fw->list_view), i, GTK_LIST_SCROLL_SELECT, nullptr);
                }
#endif
                break;
            }
        }
        fw->pending_select.clear();
    }

    update_status_bar(fw);
}

void refresh_current_dir(FilesWin *fw) {
    gtk_widget_set_sensitive(fw->btn_back, fw->hist_idx > 0);
    gtk_widget_set_sensitive(fw->btn_fwd, fw->hist_idx + 1 < static_cast<int>(fw->history.size()));
    gtk_widget_set_sensitive(fw->btn_up, fw->current_dir.has_parent_path() && fw->current_dir.parent_path() != fw->current_dir);

    std::string title = fw->current_dir.filename().string();
    if (is_in_trash_dir(fw->current_dir)) title = "回收站";
    else if (title.empty()) title = fw->current_dir.string();
    gtk_window_set_title(fw->win, ("Spinel Files · " + title).c_str());

    if (fw->btn_empty_trash) {
        bool in_trash = is_in_trash_dir(fw->current_dir);
        gtk_widget_set_visible(fw->btn_empty_trash, in_trash);
        gtk_widget_set_sensitive(fw->btn_empty_trash, in_trash && trash_has_items());
    }

    rebuild_sidebar(fw);
    rebuild_breadcrumbs(fw);

    fw->scanning = true;
    update_status_bar(fw);

    uint64_t gen = ++(*fw->scan_gen);
    fs::path dir = fw->current_dir;
    auto alive = fw->alive;
    auto gen_ptr = fw->scan_gen;

    struct ScanResultIdle {
        FilesWin *fw;
        std::shared_ptr<std::atomic<bool>> alive;
        std::shared_ptr<std::atomic<uint64_t>> gen_ptr;
        uint64_t gen;
        std::vector<DirItem> items;
    };

    std::thread([fw, alive, gen_ptr, gen, dir]() {
        auto scanned = scan_dir_posix(dir);
        if (!alive->load() || gen_ptr->load() != gen) return;
        auto *res = new ScanResultIdle{fw, alive, gen_ptr, gen, std::move(scanned)};
        g_idle_add(+[](gpointer d) -> gboolean {
            std::unique_ptr<ScanResultIdle> msg(static_cast<ScanResultIdle *>(d));
            if (!msg->alive->load() || msg->gen_ptr->load() != msg->gen) return G_SOURCE_REMOVE;
            msg->fw->scanning = false;
            msg->fw->items = std::move(msg->items);
            apply_visible_filter(msg->fw);
            return G_SOURCE_REMOVE;
        }, res);
    }).detach();
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

void clear_badge_classes(GtkWidget *ico) {
    for (const char *c : {"sf-ico-dir", "sf-ico-img", "sf-ico-vid", "sf-ico-aud",
                          "sf-ico-arc", "sf-ico-code", "sf-ico-exec", "sf-ico-file"}) {
        gtk_widget_remove_css_class(ico, c);
    }
}

GtkListItemFactory *create_item_factory(FilesWin *fw, bool is_grid) {
    GtkListItemFactory *fac = gtk_signal_list_item_factory_new();

    if (!is_grid) {
        g_signal_connect(fac, "setup", G_CALLBACK(+[](GtkListItemFactory *, GtkListItem *li, gpointer d) {
            auto *win = static_cast<FilesWin *>(d);
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_widget_add_css_class(row, "sf-row");
            GtkWidget *ico = gtk_image_new();
            GtkWidget *name = gtk_label_new("");
            gtk_widget_set_hexpand(name, TRUE);
            gtk_widget_set_halign(name, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);

            GtkWidget *sz = gtk_label_new("");
            gtk_widget_add_css_class(sz, "sf-muted");
            gtk_widget_set_size_request(sz, 86, -1);
            gtk_label_set_xalign(GTK_LABEL(sz), 1.0f);

            GtkWidget *mt = gtk_label_new("");
            gtk_widget_add_css_class(mt, "sf-muted");
            gtk_widget_set_size_request(mt, 130, -1);
            gtk_label_set_xalign(GTK_LABEL(mt), 1.0f);

            gtk_box_append(GTK_BOX(row), ico);
            gtk_box_append(GTK_BOX(row), name);
            gtk_box_append(GTK_BOX(row), sz);
            gtk_box_append(GTK_BOX(row), mt);

            g_object_set_data(G_OBJECT(row), "sf-ico", ico);
            g_object_set_data(G_OBJECT(row), "sf-name", name);
            g_object_set_data(G_OBJECT(row), "sf-sz", sz);
            g_object_set_data(G_OBJECT(row), "sf-mt", mt);
            g_object_set_data(G_OBJECT(row), "sf-li", li);

            GtkGesture *click3 = gtk_gesture_click_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click3), 3);
            g_signal_connect(click3, "pressed", G_CALLBACK(+[](GtkGestureClick *g, gint, gdouble x, gdouble y, gpointer wd) {
                auto *fwin = static_cast<FilesWin *>(wd);
                GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
                auto *item = static_cast<GtkListItem *>(g_object_get_data(G_OBJECT(w), "sf-li"));
                auto *fobj = item ? reinterpret_cast<_SpinelFileObj *>(gtk_list_item_get_item(item)) : nullptr;
                if (fobj && fobj->item) {
                    guint pos = gtk_list_item_get_position(item);
                    if (!gtk_selection_model_is_selected(GTK_SELECTION_MODEL(fwin->selection), pos)) {
                        gtk_selection_model_select_item(GTK_SELECTION_MODEL(fwin->selection), pos, TRUE);
                        sync_selection_cache(fwin);
                    }
                    show_context_menu(fwin, w, x, y, fobj->item->path);
                }
            }), win);
            gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click3));
            gtk_list_item_set_child(li, row);
        }), fw);

        g_signal_connect(fac, "bind", G_CALLBACK(+[](GtkListItemFactory *, GtkListItem *li, gpointer) {
            GtkWidget *row = gtk_list_item_get_child(li);
            auto *fobj = reinterpret_cast<_SpinelFileObj *>(gtk_list_item_get_item(li));
            if (!row || !fobj || !fobj->item) return;
            const DirItem &it = *fobj->item;
            FileBadge badge = classify_item(it);

            GtkWidget *ico = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(row), "sf-ico"));
            GtkWidget *name = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(row), "sf-name"));
            GtkWidget *sz = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(row), "sf-sz"));
            GtkWidget *mt = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(row), "sf-mt"));

            clear_badge_classes(ico);
            gtk_image_set_from_icon_name(GTK_IMAGE(ico), badge.icon_name);
            gtk_widget_add_css_class(ico, badge.css_class);
            gtk_label_set_text(GTK_LABEL(name), it.name.c_str());
            gtk_label_set_text(GTK_LABEL(sz), it.is_dir ? "文件夹" : format_size(it.size).c_str());
            gtk_label_set_text(GTK_LABEL(mt), format_time(it.mtime).c_str());
        }), fw);
    } else {
        g_signal_connect(fac, "setup", G_CALLBACK(+[](GtkListItemFactory *, GtkListItem *li, gpointer d) {
            auto *win = static_cast<FilesWin *>(d);
            GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
            gtk_widget_add_css_class(card, "sf-grid-card");
            GtkWidget *ico = gtk_image_new();
            gtk_image_set_pixel_size(GTK_IMAGE(ico), 36);
            GtkWidget *name = gtk_label_new("");
            gtk_label_set_max_width_chars(GTK_LABEL(name), 14);
            gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
            GtkWidget *sub = gtk_label_new("");
            gtk_widget_add_css_class(sub, "sf-muted");

            gtk_box_append(GTK_BOX(card), ico);
            gtk_box_append(GTK_BOX(card), name);
            gtk_box_append(GTK_BOX(card), sub);

            g_object_set_data(G_OBJECT(card), "sf-ico", ico);
            g_object_set_data(G_OBJECT(card), "sf-name", name);
            g_object_set_data(G_OBJECT(card), "sf-sub", sub);
            g_object_set_data(G_OBJECT(card), "sf-li", li);

            GtkGesture *click3 = gtk_gesture_click_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click3), 3);
            g_signal_connect(click3, "pressed", G_CALLBACK(+[](GtkGestureClick *g, gint, gdouble x, gdouble y, gpointer wd) {
                auto *fwin = static_cast<FilesWin *>(wd);
                GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
                auto *item = static_cast<GtkListItem *>(g_object_get_data(G_OBJECT(w), "sf-li"));
                auto *fobj = item ? reinterpret_cast<_SpinelFileObj *>(gtk_list_item_get_item(item)) : nullptr;
                if (fobj && fobj->item) {
                    guint pos = gtk_list_item_get_position(item);
                    if (!gtk_selection_model_is_selected(GTK_SELECTION_MODEL(fwin->selection), pos)) {
                        gtk_selection_model_select_item(GTK_SELECTION_MODEL(fwin->selection), pos, TRUE);
                        sync_selection_cache(fwin);
                    }
                    show_context_menu(fwin, w, x, y, fobj->item->path);
                }
            }), win);
            gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(click3));
            gtk_list_item_set_child(li, card);
        }), fw);

        g_signal_connect(fac, "bind", G_CALLBACK(+[](GtkListItemFactory *, GtkListItem *li, gpointer) {
            GtkWidget *card = gtk_list_item_get_child(li);
            auto *fobj = reinterpret_cast<_SpinelFileObj *>(gtk_list_item_get_item(li));
            if (!card || !fobj || !fobj->item) return;
            const DirItem &it = *fobj->item;
            FileBadge badge = classify_item(it);

            GtkWidget *ico = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(card), "sf-ico"));
            GtkWidget *name = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(card), "sf-name"));
            GtkWidget *sub = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(card), "sf-sub"));

            clear_badge_classes(ico);
            gtk_image_set_from_icon_name(GTK_IMAGE(ico), badge.icon_name);
            gtk_widget_add_css_class(ico, badge.css_class);
            gtk_label_set_text(GTK_LABEL(name), it.name.c_str());
            gtk_label_set_text(GTK_LABEL(sub), it.is_dir ? "文件夹" : format_size(it.size).c_str());
        }), fw);
    }
    return fac;
}

gboolean on_key_pressed(GtkEventControllerKey *, guint keyval, guint, GdkModifierType state, gpointer d) {
    auto *fw = static_cast<FilesWin *>(d);
    bool ctrl = (state & GDK_CONTROL_MASK) != 0;
    bool shift = (state & GDK_SHIFT_MASK) != 0;

    if (ctrl && (keyval == GDK_KEY_h || keyval == GDK_KEY_H)) {
        fw->show_hidden = !fw->show_hidden;
        if (fw->show_hidden) gtk_widget_add_css_class(fw->btn_hidden, "is-on");
        else gtk_widget_remove_css_class(fw->btn_hidden, "is-on");
        apply_visible_filter(fw);
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
    if (ctrl && (keyval == GDK_KEY_a || keyval == GDK_KEY_A) && !gtk_widget_has_focus(fw->search_entry) && !gtk_widget_has_focus(fw->path_entry)) {
        if (fw->selection) gtk_selection_model_select_all(GTK_SELECTION_MODEL(fw->selection));
        return TRUE;
    }
    if (ctrl && (keyval == GDK_KEY_c || keyval == GDK_KEY_C) && !gtk_widget_has_focus(fw->search_entry) && !gtk_widget_has_focus(fw->path_entry)) {
        auto sel = get_selected_paths(fw);
        if (!sel.empty()) {
            fw->clip_op = ClipOp::Copy;
            fw->clip_paths = std::move(sel);
            if (fw->status_label) {
                gtk_label_set_text(GTK_LABEL(fw->status_label), ("已复制 " + std::to_string(fw->clip_paths.size()) + " 项到剪贴板").c_str());
            }
        }
        return TRUE;
    }
    if (ctrl && (keyval == GDK_KEY_x || keyval == GDK_KEY_X) && !gtk_widget_has_focus(fw->search_entry) && !gtk_widget_has_focus(fw->path_entry)) {
        auto sel = get_selected_paths(fw);
        if (!sel.empty()) {
            fw->clip_op = ClipOp::Cut;
            fw->clip_paths = std::move(sel);
            if (fw->status_label) {
                gtk_label_set_text(GTK_LABEL(fw->status_label), ("已剪切 " + std::to_string(fw->clip_paths.size()) + " 项").c_str());
            }
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
    if (keyval == GDK_KEY_Escape) {
        if (fw->path_edit_mode) {
            set_path_edit_mode(fw, false);
            return TRUE;
        }
        if (!fw->selected_paths.empty() && fw->selection) {
            gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(fw->selection));
            return TRUE;
        }
    }
    return FALSE;
}

void build_files_window(GtkApplication *app,
                        const fs::path &start_dir,
                        const fs::path &select_path,
                        bool pick_dir_mode = false,
                        std::string *pick_dir_out = nullptr) {
    auto *fw = new FilesWin();
    fw->app = app;
    fw->pick_dir_mode = pick_dir_mode;
    fw->pick_dir_out = pick_dir_out;
    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    fw->win = win;
    gtk_window_set_default_size(win, pick_dir_mode ? 900 : 1020, pick_dir_mode ? 580 : 660);
    if (pick_dir_mode) gtk_window_set_title(win, "选择文件夹 — Spinel Files");
    gtk_window_set_icon_name(win, "spinel-files");
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
        auto *w = static_cast<FilesWin *>(d);
        if (w->search_timer) g_source_remove(w->search_timer);
        w->search_timer = g_timeout_add(50, +[](gpointer wd) -> gboolean {
            auto *win = static_cast<FilesWin *>(wd);
            win->search_timer = 0;
            apply_visible_filter(win);
            return G_SOURCE_REMOVE;
        }, w);
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
        apply_visible_filter(w);
    }), fw);

    fw->btn_view_mode = gtk_button_new_with_label("网格视图");
    gtk_widget_add_css_class(fw->btn_view_mode, "sf-tool-btn");
    g_signal_connect(fw->btn_view_mode, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer d) {
        auto *w = static_cast<FilesWin *>(d);
        w->grid_mode = !w->grid_mode;
        gtk_button_set_label(b, w->grid_mode ? "列表视图" : "网格视图");
        gtk_stack_set_visible_child_name(GTK_STACK(w->view_stack), w->grid_mode ? "grid" : "list");
    }), fw);

    fw->btn_empty_trash = gtk_button_new_with_label("清空回收站");
    gtk_widget_add_css_class(fw->btn_empty_trash, "sf-tool-danger");
    gtk_widget_set_visible(fw->btn_empty_trash, FALSE);
    g_signal_connect(fw->btn_empty_trash, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        do_empty_trash(static_cast<FilesWin *>(d));
    }), fw);

    GtkWidget *term_btn = gtk_button_new_with_label("打开终端");
    gtk_widget_add_css_class(term_btn, "sf-tool-btn");
    g_signal_connect(term_btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        open_terminal_at(static_cast<FilesWin *>(d)->current_dir);
    }), fw);

    for (GtkWidget *w : {fw->btn_back, fw->btn_fwd, fw->btn_up, fw->crumb_scroll, fw->path_entry,
                         fw->btn_path_edit, fw->search_entry, fw->btn_empty_trash, new_dir_btn,
                         fw->btn_hidden, fw->btn_view_mode, term_btn}) {
        gtk_box_append(GTK_BOX(bar), w);
    }

    // Body split: sidebar + virtualized views
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(body, TRUE);

    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(sidebar, "sf-sidebar");
    gtk_widget_set_hexpand(sidebar, FALSE);
    gtk_widget_set_size_request(sidebar, 204, -1);

    GtkWidget *places_lbl = gtk_label_new("常用位置");
    gtk_widget_add_css_class(places_lbl, "sf-side-title");
    gtk_widget_set_halign(places_lbl, GTK_ALIGN_START);
    fw->places_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    GtkWidget *drives_lbl = gtk_label_new("外置盘与挂载点");
    gtk_widget_add_css_class(drives_lbl, "sf-side-title");
    gtk_widget_set_halign(drives_lbl, GTK_ALIGN_START);
    fw->drives_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    // Block child hexpand propagation (external drive rows) so sidebar stays ~212px wide.
    GtkWidget *side_scroll = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(side_scroll, FALSE);
    gtk_widget_set_size_request(side_scroll, 212, -1);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(side_scroll), FALSE);
    gtk_scrolled_window_set_max_content_width(GTK_SCROLLED_WINDOW(side_scroll), 216);
    gtk_box_append(GTK_BOX(sidebar), places_lbl);
    gtk_box_append(GTK_BOX(sidebar), fw->places_box);
    gtk_box_append(GTK_BOX(sidebar), drives_lbl);
    gtk_box_append(GTK_BOX(sidebar), fw->drives_box);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(side_scroll), sidebar);

    fw->store = g_list_store_new(spinel_file_obj_get_type());
    fw->selection = gtk_multi_selection_new(G_LIST_MODEL(fw->store));  // takes ref to store

    g_signal_connect(fw->selection, "selection-changed", G_CALLBACK(+[](GtkSelectionModel *, guint, guint, gpointer d) {
        auto *w = static_cast<FilesWin *>(d);
        sync_selection_cache(w);
        update_status_bar(w);
    }), fw);

    GtkListItemFactory *list_fac = create_item_factory(fw, false);
    fw->list_view = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(fw->selection)), list_fac);
    gtk_widget_add_css_class(fw->list_view, "sf-listview");
    gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(fw->list_view), FALSE);
    gtk_list_view_set_enable_rubberband(GTK_LIST_VIEW(fw->list_view), TRUE);
    g_signal_connect(fw->list_view, "activate", G_CALLBACK(+[](GtkListView *, guint pos, gpointer d) {
        auto *w = static_cast<FilesWin *>(d);
        auto *fobj = reinterpret_cast<_SpinelFileObj *>(g_list_model_get_item(G_LIST_MODEL(w->selection), pos));
        if (fobj) {
            if (fobj->item) open_item_action(w, fobj->item->path);
            g_object_unref(fobj);
        }
    }), fw);

    GtkListItemFactory *grid_fac = create_item_factory(fw, true);
    fw->grid_view = gtk_grid_view_new(GTK_SELECTION_MODEL(fw->selection), grid_fac);  // takes final ref to selection
    gtk_widget_add_css_class(fw->grid_view, "sf-gridview");
    gtk_grid_view_set_single_click_activate(GTK_GRID_VIEW(fw->grid_view), FALSE);
    gtk_grid_view_set_enable_rubberband(GTK_GRID_VIEW(fw->grid_view), TRUE);
    gtk_grid_view_set_max_columns(GTK_GRID_VIEW(fw->grid_view), 10);
    g_signal_connect(fw->grid_view, "activate", G_CALLBACK(+[](GtkGridView *, guint pos, gpointer d) {
        auto *w = static_cast<FilesWin *>(d);
        auto *fobj = reinterpret_cast<_SpinelFileObj *>(g_list_model_get_item(G_LIST_MODEL(w->selection), pos));
        if (fobj) {
            if (fobj->item) open_item_action(w, fobj->item->path);
            g_object_unref(fobj);
        }
    }), fw);

    fw->view_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(fw->view_stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_add_named(GTK_STACK(fw->view_stack), fw->list_view, "list");
    gtk_stack_add_named(GTK_STACK(fw->view_stack), fw->grid_view, "grid");

    fw->content_scroll = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(fw->content_scroll, TRUE);
    gtk_widget_set_vexpand(fw->content_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(fw->content_scroll), fw->view_stack);

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
    if (pick_dir_mode) {
        GtkWidget *pick_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_add_css_class(pick_bar, "sf-toolbar");
        gtk_widget_set_margin_top(pick_bar, 2);
        gtk_widget_set_margin_bottom(pick_bar, 4);
        gtk_widget_set_margin_start(pick_bar, 6);
        gtk_widget_set_margin_end(pick_bar, 6);

        fw->pick_label = gtk_label_new(("将选择文件夹: " + start_dir.string()).c_str());
        gtk_widget_set_hexpand(fw->pick_label, TRUE);
        gtk_widget_set_halign(fw->pick_label, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(fw->pick_label), PANGO_ELLIPSIZE_MIDDLE);

        GtkWidget *btn_cancel = gtk_button_new_with_label("取消");
        gtk_widget_add_css_class(btn_cancel, "sf-tool-btn");
        g_signal_connect(btn_cancel, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
            auto *w = static_cast<FilesWin *>(d);
            if (w && w->win) gtk_window_close(w->win);
        }), fw);

        GtkWidget *btn_pick = gtk_button_new_with_label("选择此文件夹");
        gtk_widget_add_css_class(btn_pick, "sf-tool-btn");
        gtk_widget_add_css_class(btn_pick, "suggested-action");
        g_signal_connect(btn_pick, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
            auto *w = static_cast<FilesWin *>(d);
            if (!w) return;
            std::error_code ec;
            fs::path chosen = w->current_dir;
            if (!w->selected_path.empty() && fs::is_directory(w->selected_path, ec)) {
                chosen = w->selected_path;
            }
            if (w->pick_dir_out) *w->pick_dir_out = chosen.string();
            if (w->win) gtk_window_close(w->win);
        }), fw);

        gtk_box_append(GTK_BOX(pick_bar), fw->pick_label);
        gtk_box_append(GTK_BOX(pick_bar), btn_cancel);
        gtk_box_append(GTK_BOX(pick_bar), btn_pick);
        gtk_box_append(GTK_BOX(fw->root), pick_bar);
    }
    gtk_window_set_child(win, fw->root);

    bind_files_window_theme(win);

    GtkEventController *key_ctl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctl, "key-pressed", G_CALLBACK(on_key_pressed), fw);
    gtk_widget_add_controller(GTK_WIDGET(win), key_ctl);

    g_signal_connect(win, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer d) {
        auto *w = static_cast<FilesWin *>(d);
        w->alive->store(false);
        ++(*w->scan_gen);
        if (w->search_timer) g_source_remove(w->search_timer);
        delete w;
    }), fw);

    fw->pending_select = select_path;
    navigate_to(fw, start_dir, true);
    gtk_window_present(win);
}

struct StartLaunchTarget {
    fs::path dir;
    fs::path select_item;
};

StartLaunchTarget resolve_start_target(int argc, char **argv) {
    StartLaunchTarget res{fs::path(home_dir()), {}};
    if (argc >= 2 && argv[1] && *argv[1]) {
        std::string arg = argv[1];
        if (arg.rfind("file://", 0) == 0) {
            g_autofree char *decoded = g_filename_from_uri(arg.c_str(), nullptr, nullptr);
            if (decoded && *decoded) arg = decoded;
            else arg = arg.substr(7);
        }
        fs::path p(arg);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            res.dir = p;
        } else if (fs::exists(p, ec) && p.has_parent_path()) {
            res.dir = p.parent_path();
            res.select_item = p;
        }
    }
    return res;
}

std::string resolve_self_files_bin() {
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!exe.empty() && fs::exists(exe, ec)) return exe.string();
    return has_executable("spinel-files") ? "spinel-files" : "/usr/local/bin/spinel-files";
}

struct Fm1DbusState {
    GMainLoop *loop = nullptr;
    guint reg_id = 0;
    guint name_id = 0;
    guint idle_quit_timer = 0;
    GDBusNodeInfo *introspection = nullptr;
};

void reset_fm1_idle_timer(Fm1DbusState *st) {
    if (!st) return;
    if (st->idle_quit_timer) g_source_remove(st->idle_quit_timer);
    st->idle_quit_timer = g_timeout_add_seconds(60, +[](gpointer d) -> gboolean {
        auto *s = static_cast<Fm1DbusState *>(d);
        s->idle_quit_timer = 0;
        if (s->loop) g_main_loop_quit(s->loop);
        return G_SOURCE_REMOVE;
    }, st);
}

int run_file_manager1_service() {
    static const char kFm1Xml[] =
        "<node>"
        "  <interface name='org.freedesktop.FileManager1'>"
        "    <method name='ShowFolders'>"
        "      <arg type='as' name='URIs' direction='in'/>"
        "      <arg type='s' name='StartupId' direction='in'/>"
        "    </method>"
        "    <method name='ShowItems'>"
        "      <arg type='as' name='URIs' direction='in'/>"
        "      <arg type='s' name='StartupId' direction='in'/>"
        "    </method>"
        "    <method name='ShowItemProperties'>"
        "      <arg type='as' name='URIs' direction='in'/>"
        "      <arg type='s' name='StartupId' direction='in'/>"
        "    </method>"
        "  </interface>"
        "</node>";

    Fm1DbusState st;
    st.loop = g_main_loop_new(nullptr, FALSE);
    st.introspection = g_dbus_node_info_new_for_xml(kFm1Xml, nullptr);

    static const GDBusInterfaceVTable kVTable = {
        +[](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *method_name,
            GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data) {
            auto *s = static_cast<Fm1DbusState *>(user_data);
            reset_fm1_idle_timer(s);

            std::string method = method_name ? method_name : "";
            GVariantIter *iter = nullptr;
            const gchar *startup_id = nullptr;
            g_variant_get(parameters, "(as&s)", &iter, &startup_id);

            std::vector<std::string> targets;
            if (iter) {
                gchar *uri = nullptr;
                while (g_variant_iter_next(iter, "s", &uri)) {
                    if (uri && *uri) {
                        g_autofree gchar *path = g_filename_from_uri(uri, nullptr, nullptr);
                        targets.emplace_back(path ? path : uri);
                    }
                    g_free(uri);
                }
                g_variant_iter_free(iter);
            }

            std::string bin = resolve_self_files_bin();
            if (targets.empty()) {
                spawn_argv_detached({bin});
            } else {
                const std::string &first = targets[0];
                if (method == "ShowFolders") {
                    std::error_code ec;
                    fs::path p(first);
                    fs::path open_dir = fs::is_directory(p, ec) ? p : (p.has_parent_path() ? p.parent_path() : fs::path(home_dir()));
                    spawn_argv_detached({bin, open_dir.string()});
                } else {
                    // ShowItems / ShowItemProperties: spinels-files opens parent dir and highlights exact file
                    spawn_argv_detached({bin, first});
                }
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
        },
        nullptr, nullptr, {nullptr}
    };

    st.name_id = g_bus_own_name(
        G_BUS_TYPE_SESSION,
        "org.freedesktop.FileManager1",
        G_BUS_NAME_OWNER_FLAGS_REPLACE,
        +[](GDBusConnection *conn, const gchar *, gpointer d) {
            auto *s = static_cast<Fm1DbusState *>(d);
            if (s->introspection && s->introspection->interfaces && s->introspection->interfaces[0]) {
                s->reg_id = g_dbus_connection_register_object(
                    conn, "/org/freedesktop/FileManager1", s->introspection->interfaces[0],
                    &kVTable, s, nullptr, nullptr);
            }
            reset_fm1_idle_timer(s);
        },
        nullptr,
        +[](GDBusConnection *, const gchar *, gpointer d) {
            auto *s = static_cast<Fm1DbusState *>(d);
            if (s->loop) g_main_loop_quit(s->loop);
        },
        &st, nullptr);

    g_main_loop_run(st.loop);
    if (st.idle_quit_timer) g_source_remove(st.idle_quit_timer);
    if (st.name_id) g_bus_unown_name(st.name_id);
    if (st.introspection) g_dbus_node_info_unref(st.introspection);
    g_main_loop_unref(st.loop);
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && argv[1] && std::string(argv[1]) == "--file-manager1") {
        return run_file_manager1_service();
    }

    ensure_display_env();
    if (argc == 3 && std::string_view(argv[1]) == "--run-wine") {
        return run_external_wine(argv[2]);
    }
    const char *wlr = std::getenv("WLR_RENDERER");
    if (wlr && std::string(wlr) == "pixman") {
        setenv("GSK_RENDERER", "cairo", 0);
    }

    if (argc >= 2 && std::string_view(argv[1]) == "--windows-apps") {
        g_autoptr(GtkApplication) app = gtk_application_new("org.spinelos.WindowsApps", G_APPLICATION_DEFAULT_FLAGS);
        g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication *gapp, gpointer) {
            show_windows_apps_window(gapp);
        }), nullptr);
        return g_application_run(G_APPLICATION(app), 0, nullptr);
    }

    if (argc >= 2 && std::string_view(argv[1]) == "--pick-dir") {
        fs::path start(argc >= 3 && argv[2] && *argv[2] ? argv[2] : home_dir());
        std::error_code ec;
        if (!fs::is_directory(start, ec)) start = fs::path(home_dir());
        struct PickCtx {
            fs::path start;
            std::string chosen;
        } ctx{start, ""};
        GtkApplication *app = gtk_application_new("org.spinelos.files.picker", G_APPLICATION_NON_UNIQUE);
        g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication *gapp, gpointer d) {
            auto *c = static_cast<PickCtx *>(d);
            build_files_window(gapp, c->start, {}, true, &c->chosen);
        }), &ctx);
        g_application_run(G_APPLICATION(app), 0, nullptr);
        g_object_unref(app);
        if (ctx.chosen.empty()) return 1;
        std::fprintf(stdout, "%s\n", ctx.chosen.c_str());
        std::fflush(stdout);
        return 0;
    }

    StartLaunchTarget target = resolve_start_target(argc, argv);
    GtkApplication *app = gtk_application_new("org.spinelos.files", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication *gapp, gpointer d) {
        auto *t = static_cast<StartLaunchTarget *>(d);
        build_files_window(gapp, t->dir, t->select_item);
    }), &target);
    int rc = g_application_run(G_APPLICATION(app), 0, nullptr);
    g_object_unref(app);
    return rc;
}
