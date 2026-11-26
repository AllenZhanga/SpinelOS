#pragma once

#include <gio/gio.h>

#include <optional>
#include <string>

inline constexpr char wine_open_desktop_id[] = "org.spinelos.WineOpen.desktop";

struct FileOpenType {
    std::string content_type;
    std::string unknown_extension;
    bool windows_exe = false;

    bool can_set_default() const;
    bool set_default(GAppInfo *app, GError **error) const;
};

std::optional<FileOpenType> query_file_open_type(GFile *file, GError **error);
