cmake_minimum_required(VERSION 3.20)
project(spinel-shell VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(WEBKIT6 REQUIRED IMPORTED_TARGET webkitgtk-6.0)
pkg_check_modules(LAYER_SHELL REQUIRED IMPORTED_TARGET gtk4-layer-shell-0)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

pkg_get_variable(WLR_PROTOCOLS_DATADIR wlr-protocols pkgdatadir)
if(NOT WLR_PROTOCOLS_DATADIR)
    set(WLR_PROTOCOLS_DATADIR "/usr/share/wlr-protocols")
endif()

set(TOPLEVEL_XML "${WLR_PROTOCOLS_DATADIR}/unstable/wlr-foreign-toplevel-management-unstable-v1.xml")
set(TOPLEVEL_HDR "${CMAKE_CURRENT_BINARY_DIR}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h")
set(TOPLEVEL_SRC "${CMAKE_CURRENT_BINARY_DIR}/wlr-foreign-toplevel-management-unstable-v1-protocol.c")

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

add_custom_target(spinel_wayland_protos
    DEPENDS "${TOPLEVEL_HDR}" "${TOPLEVEL_SRC}"
)

set_source_files_properties(src/window_tracker.cpp PROPERTIES
    OBJECT_DEPENDS "${TOPLEVEL_HDR}"
)

add_executable(spinel-shell
    src/main.cpp
    src/common.cpp
    src/steam_library.cpp
    src/system_control.cpp
    src/installer.cpp
    src/desktop_apps.cpp
    src/app_store.cpp
    src/native_shell.cpp
    src/window_tracker.cpp
    "${TOPLEVEL_HDR}"
    "${TOPLEVEL_SRC}"
)

add_dependencies(spinel-shell spinel_wayland_protos)

target_include_directories(spinel-shell PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
)

target_link_libraries(spinel-shell PRIVATE
    PkgConfig::GTK4
    PkgConfig::WEBKIT6
    PkgConfig::LAYER_SHELL
    PkgConfig::WAYLAND_CLIENT
)

target_compile_options(spinel-shell PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -O2>
)

install(TARGETS spinel-shell DESTINATION bin)
install(DIRECTORY ui/ DESTINATION share/spinel-shell/ui)
