#pragma once

#include <gtk/gtk.h>

bool open_wine_store(GtkWindow *parent, GError **error);
void show_windows_apps_window(GtkApplication *app);
