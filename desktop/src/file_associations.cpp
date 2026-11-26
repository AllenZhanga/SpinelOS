#include "file_associations.hpp"
#include "wine_launcher.hpp"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

bool FileOpenType::can_set_default() const {
    return !g_content_type_is_unknown(content_type.c_str()) || !unknown_extension.empty();
}

bool FileOpenType::set_default(GAppInfo *app, GError **error) const {
    if (!unknown_extension.empty()) {
        return g_app_info_set_as_default_for_extension(app, unknown_extension.c_str(), error);
    }
    if (g_content_type_is_unknown(content_type.c_str())) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "此文件没有可保存的类型或扩展名。");
        return false;
    }
    return g_app_info_set_as_default_for_type(app, content_type.c_str(), error);
}

std::optional<FileOpenType> query_file_open_type(GFile *file, GError **error) {
    g_autoptr(GFileInfo) info = g_file_query_info(file,
        G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
        G_FILE_QUERY_INFO_NONE, nullptr, error);
    if (!info) return std::nullopt;
    if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "请选择一个普通文件。");
        return std::nullopt;
    }

    const char *detected_type = g_file_info_get_content_type(info);
    FileOpenType type{detected_type ? detected_type : "application/octet-stream", {}, false};
    g_autofree char *path = g_file_get_path(file);
    if (path && is_windows_executable(path)) {
        // Use one type for .exe files regardless of case or the detected PE subtype.
        type.content_type = "application/vnd.microsoft.portable-executable";
        type.windows_exe = true;
    }
    if (path && g_content_type_is_unknown(type.content_type.c_str())) {
        const auto extension = fs::path(path).extension().string();
        // GIO also uses the extension to construct the name of a shared-mime-info XML file.
        if (extension.size() > 1 && extension.size() <= 65 &&
            std::all_of(extension.begin() + 1, extension.end(), [](char ch) {
                return g_ascii_isalnum(ch) || ch == '-' || ch == '_';
            })) {
            g_autofree char *lower = g_ascii_strdown(extension.c_str() + 1, -1);
            type.unknown_extension = lower;
        }
    }
    return type;
}
