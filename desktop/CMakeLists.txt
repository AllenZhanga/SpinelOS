cmake_minimum_required(VERSION 3.20)
project(spinel-shell VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(WEBKIT6 REQUIRED IMPORTED_TARGET webkitgtk-6.0)
pkg_check_modules(LAYER_SHELL REQUIRED IMPORTED_TARGET gtk4-layer-shell-0)
pkg_check_modules(VTE4 REQUIRED IMPORTED_TARGET vte-2.91-gtk4)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

pkg_get_variable(WLR_PROTOCOLS_DATADIR wlr-protocols pkgdatadir)
if(NOT WLR_PROTOCOLS_DATADIR)
    set(WLR_PROTOCOLS_DATADIR "/usr/share/wlr-protocols")
endif()

set(TOPLEVEL_XML "${WLR_PROTOCOLS_DATADIR}/unstable/wlr-foreign-toplevel-management-unstable-v1.xml")
set(TOPLEVEL_HDR "${CMAKE_CURRENT_BINARY_DIR}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h")
set(TOPLEVEL_SRC "${CMAKE_CURRENT_BINARY_DIR}/wlr-foreign-toplevel-management-unstable-v1-protocol.c")
set(SCREENCOPY_XML "${WLR_PROTOCOLS_DATADIR}/unstable/wlr-screencopy-unstable-v1.xml")
set(SCREENCOPY_HDR "${CMAKE_CURRENT_BINARY_DIR}/wlr-screencopy-unstable-v1-client-protocol.h")
set(SCREENCOPY_SRC "${CMAKE_CURRENT_BINARY_DIR}/wlr-screencopy-unstable-v1-protocol.c")
set(VPOINTER_XML "${WLR_PROTOCOLS_DATADIR}/unstable/wlr-virtual-pointer-unstable-v1.xml")
set(VPOINTER_HDR "${CMAKE_CURRENT_BINARY_DIR}/wlr-virtual-pointer-unstable-v1-client-protocol.h")
set(VPOINTER_SRC "${CMAKE_CURRENT_BINARY_DIR}/wlr-virtual-pointer-unstable-v1-protocol.c")
set(VKEYBOARD_XML "${CMAKE_CURRENT_SOURCE_DIR}/protocols/virtual-keyboard-unstable-v1.xml")
set(VKEYBOARD_HDR "${CMAKE_CURRENT_BINARY_DIR}/virtual-keyboard-unstable-v1-client-protocol.h")
set(VKEYBOARD_SRC "${CMAKE_CURRENT_BINARY_DIR}/virtual-keyboard-unstable-v1-protocol.c")

pkg_get_variable(WAYLAND_PROTOCOLS_DATADIR wayland-protocols pkgdatadir)
if(WAYLAND_PROTOCOLS_DATADIR AND EXISTS "${WAYLAND_PROTOCOLS_DATADIR}/staging/ext-workspace/ext-workspace-v1.xml")
    set(EXT_WORKSPACE_XML "${WAYLAND_PROTOCOLS_DATADIR}/staging/ext-workspace/ext-workspace-v1.xml")
else()
    set(EXT_WORKSPACE_XML "${CMAKE_CURRENT_SOURCE_DIR}/protocols/ext-workspace-v1.xml")
endif()
set(EXT_WORKSPACE_HDR "${CMAKE_CURRENT_BINARY_DIR}/ext-workspace-v1-client-protocol.h")
set(EXT_WORKSPACE_SRC "${CMAKE_CURRENT_BINARY_DIR}/ext-workspace-v1-protocol.c")

add_custom_command(
    OUTPUT "${TOPLEVEL_HDR}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${TOPLEVEL_XML}" "${TOPLEVEL_HDR}"
    DEPENDS "${TOPLEVEL_XML}"
    COMMENT "Generating wlr-foreign-toplevel-management client header"
)

add_custom_command(
    OUTPUT "${TOPLEVEL_SRC}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${TOPLEVEL_XML}" "${TOPLEVEL_SRC}"
    DEPENDS "${TOPLEVEL_XML}"
    COMMENT "Generating wlr-foreign-toplevel-management protocol code"
)

add_custom_command(
    OUTPUT "${EXT_WORKSPACE_HDR}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${EXT_WORKSPACE_XML}" "${EXT_WORKSPACE_HDR}"
    DEPENDS "${EXT_WORKSPACE_XML}"
    COMMENT "Generating ext-workspace-v1 client header"
)

add_custom_command(
    OUTPUT "${EXT_WORKSPACE_SRC}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${EXT_WORKSPACE_XML}" "${EXT_WORKSPACE_SRC}"
    DEPENDS "${EXT_WORKSPACE_XML}"
    COMMENT "Generating ext-workspace-v1 protocol code"
)

add_custom_command(
    OUTPUT "${SCREENCOPY_HDR}" "${SCREENCOPY_SRC}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${SCREENCOPY_XML}" "${SCREENCOPY_HDR}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${SCREENCOPY_XML}" "${SCREENCOPY_SRC}"
    DEPENDS "${SCREENCOPY_XML}"
    COMMENT "Generating Wayland wlr-screencopy protocol client bindings"
)

add_custom_command(
    OUTPUT "${VPOINTER_HDR}" "${VPOINTER_SRC}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${VPOINTER_XML}" "${VPOINTER_HDR}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${VPOINTER_XML}" "${VPOINTER_SRC}"
    DEPENDS "${VPOINTER_XML}"
    COMMENT "Generating Wayland wlr-virtual-pointer protocol client bindings"
)

add_custom_command(
    OUTPUT "${VKEYBOARD_HDR}" "${VKEYBOARD_SRC}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${VKEYBOARD_XML}" "${VKEYBOARD_HDR}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${VKEYBOARD_XML}" "${VKEYBOARD_SRC}"
    DEPENDS "${VKEYBOARD_XML}"
    COMMENT "Generating Wayland zwp-virtual-keyboard protocol client bindings"
)

add_custom_target(spinel_wayland_protos
    DEPENDS "${TOPLEVEL_HDR}" "${TOPLEVEL_SRC}"
            "${EXT_WORKSPACE_HDR}" "${EXT_WORKSPACE_SRC}"
            "${SCREENCOPY_HDR}" "${SCREENCOPY_SRC}"
            "${VPOINTER_HDR}" "${VPOINTER_SRC}"
            "${VKEYBOARD_HDR}" "${VKEYBOARD_SRC}"
)

set_source_files_properties(src/window_tracker.cpp PROPERTIES
    OBJECT_DEPENDS "${TOPLEVEL_HDR};${EXT_WORKSPACE_HDR}"
)
set_source_files_properties(src/shell/sandbox_pip.cpp PROPERTIES
    OBJECT_DEPENDS "${SCREENCOPY_HDR};${VPOINTER_HDR};${VKEYBOARD_HDR}"
)

add_executable(spinel-shell
    src/main.cpp
    src/ipc_router.cpp
    src/common.cpp
    src/sandbox_manager.cpp
    src/steam_library.cpp
    src/sys/network.cpp
    src/sys/display_input.cpp
    src/sys/audio_power.cpp
    src/sys/devices.cpp
    src/sys/doctor_logs.cpp
    src/desktop_apps.cpp
    src/wine_launcher.cpp
    src/temp_memory.cpp
    src/theme_manager.cpp
    src/shell/shell_core.cpp
    src/shell/top_bar.cpp
    src/shell/sni_tray.cpp
    src/shell/desktop_monitors.cpp
    src/shell/dock.cpp
    src/shell/overlays.cpp
    src/shell/sandbox_pip.cpp
    src/window_tracker.cpp
    "${TOPLEVEL_HDR}"
    "${TOPLEVEL_SRC}"
    "${EXT_WORKSPACE_HDR}"
    "${EXT_WORKSPACE_SRC}"
    "${SCREENCOPY_HDR}"
    "${SCREENCOPY_SRC}"
    "${VPOINTER_HDR}"
    "${VPOINTER_SRC}"
    "${VKEYBOARD_HDR}"
    "${VKEYBOARD_SRC}"
)

add_dependencies(spinel-shell spinel_wayland_protos)

target_compile_definitions(spinel-shell PRIVATE
    SPINEL_HAS_WEBKIT=0
)

target_include_directories(spinel-shell PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

target_link_libraries(spinel-shell PRIVATE
    PkgConfig::GTK4
    PkgConfig::LAYER_SHELL
    PkgConfig::WAYLAND_CLIENT
    PkgConfig::XKBCOMMON
)

target_compile_options(spinel-shell PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -O2>
)

add_executable(spinel-webui
    src/main.cpp
    src/ipc_router.cpp
    src/common.cpp
    src/sandbox_manager.cpp
    src/steam_library.cpp
    src/sys/network.cpp
    src/sys/display_input.cpp
    src/sys/audio_power.cpp
    src/sys/devices.cpp
    src/sys/doctor_logs.cpp
    src/installer.cpp
    src/desktop_apps.cpp
    src/wine_launcher.cpp
    src/temp_memory.cpp
    src/app_store.cpp
    src/theme_manager.cpp
)

target_compile_definitions(spinel-webui PRIVATE
    SPINEL_HAS_WEBKIT=1
)

target_include_directories(spinel-webui PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(spinel-webui PRIVATE
    PkgConfig::GTK4
    PkgConfig::WEBKIT6
)

target_compile_options(spinel-webui PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -O2>
)

add_executable(spinel-terminal
    src/spinel_terminal.cpp
    src/common.cpp
)

target_link_libraries(spinel-terminal PRIVATE
    PkgConfig::GTK4
    PkgConfig::VTE4
)

target_compile_options(spinel-terminal PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -O2>
)

add_executable(spinel-files
    src/spinel_files.cpp
    src/file_associations.cpp
    src/files_theme.cpp
    src/windows_apps_window.cpp
    src/common.cpp
    src/sandbox_manager.cpp
    src/wine_launcher.cpp
    src/temp_memory.cpp
)

target_compile_definitions(spinel-files PRIVATE
    SPINEL_HAS_WEBKIT=0
)

target_include_directories(spinel-files PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(spinel-files PRIVATE
    PkgConfig::GTK4
)

target_compile_options(spinel-files PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -O2>
)

add_executable(spinel-sandbox
    src/spinel_sandbox_main.cpp
    src/sandbox_manager.cpp
    src/common.cpp
)

target_include_directories(spinel-sandbox PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_options(spinel-sandbox PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -O2>
)

enable_testing()
configure_file(../archiso/airootfs/usr/local/bin/spinel-wine spinel-wine COPYONLY)

add_executable(spinel-tests
    tests/spinel_tests.cpp
    src/file_associations.cpp
    src/ipc_router.cpp
    src/common.cpp
    src/sandbox_manager.cpp
    src/wine_launcher.cpp
    src/temp_memory.cpp
    src/theme_manager.cpp
    src/desktop_apps.cpp
    src/steam_library.cpp
    src/shell/sni_tray.cpp
    src/sys/network.cpp
    src/sys/display_input.cpp
    src/sys/audio_power.cpp
    src/sys/devices.cpp
    src/sys/doctor_logs.cpp
)

target_compile_definitions(spinel-tests PRIVATE
    SPINEL_HAS_WEBKIT=0
    SPINEL_TEST_WINE_HELPER_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

target_include_directories(spinel-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(spinel-tests PRIVATE
    PkgConfig::GTK4
)

target_compile_options(spinel-tests PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -O2>
)

add_test(NAME spinel_unit_tests COMMAND spinel-tests)

install(TARGETS spinel-shell spinel-webui spinel-terminal spinel-files spinel-sandbox DESTINATION bin)
install(PROGRAMS ../archiso/airootfs/usr/local/bin/spinel-wine DESTINATION bin)
install(DIRECTORY ui/ DESTINATION share/spinel-shell/ui)
install(FILES
    ../archiso/airootfs/usr/share/applications/org.spinelos.WindowsApps.desktop
    ../archiso/airootfs/usr/share/applications/org.spinelos.WineOpen.desktop
    DESTINATION share/applications)
install(FILES ../archiso/airootfs/usr/share/icons/hicolor/scalable/apps/org.spinelos.WindowsApps.svg
    DESTINATION share/icons/hicolor/scalable/apps)
