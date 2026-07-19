cmake_minimum_required(VERSION 3.10)
project(fwm_wayland C)

set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_SERVER REQUIRED wayland-server)
pkg_check_modules(WLROOTS REQUIRED wlroots-0.20)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(PANGO REQUIRED pango)
pkg_check_modules(PANGOCAIRO REQUIRED pangocairo)
pkg_check_modules(GDK_PIXBUF REQUIRED gdk-pixbuf-2.0)
find_package(box2d REQUIRED)

# List of source files
set(SOURCES
    main.c
    src/physics.c
    src/bsp.c
    src/toml.c
    src/config.c
    src/wallpaper.c
    src/view.c
    src/server.c
    src/ui/cairo_overlay.c
    src/ui/tray.c
    src/ui/hints.c
    src/ui/welcome.c
)

add_executable(fwm-wayland ${SOURCES})

target_include_directories(fwm-wayland PRIVATE
        ${WAYLAND_SERVER_INCLUDE_DIRS}
        ${WLROOTS_INCLUDE_DIRS}
        ${XKBCOMMON_INCLUDE_DIRS}
        ${CAIRO_INCLUDE_DIRS}
        ${PANGO_INCLUDE_DIRS}
        ${PANGOCAIRO_INCLUDE_DIRS}
        ${GDK_PIXBUF_INCLUDE_DIRS}
        src
        src/ui
)

target_link_libraries(fwm-wayland PRIVATE
        ${WAYLAND_SERVER_LIBRARIES}
        ${WLROOTS_LIBRARIES}
        ${XKBCOMMON_LIBRARIES}
        ${CAIRO_LIBRARIES}
        ${PANGO_LIBRARIES}
        ${PANGOCAIRO_LIBRARIES}
        ${GDK_PIXBUF_LIBRARIES}
        box2d::box2d
        m
)

target_compile_definitions(fwm-wayland PRIVATE WLR_USE_UNSTABLE)