cmake_minimum_required(VERSION 3.20)
project(spinel-shell VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(WEBKIT6 REQUIRED IMPORTED_TARGET webkitgtk-6.0)
pkg_check_modules(LAYER_SHELL REQUIRED IMPORTED_TARGET gtk4-layer-shell-0)

add_executable(spinel-shell
    src/main.cpp
)

target_link_libraries(spinel-shell PRIVATE
    PkgConfig::GTK4
    PkgConfig::WEBKIT6
    PkgConfig::LAYER_SHELL
)

target_compile_options(spinel-shell PRIVATE -Wall -Wextra -O2)

install(TARGETS spinel-shell DESTINATION bin)
install(DIRECTORY ui/ DESTINATION share/spinel-shell/ui)
