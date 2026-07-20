cmake_minimum_required(VERSION 3.10)
project(fwm_wayland C)

set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_SERVER REQUIRED wayland-server)
# wlroots is hard-pinned: the unstable API breaks between minor versions, so
# fwm builds only against the 0.20 series (the .pc name itself is versioned;
# a system upgrade to 0.21 fails configure loudly instead of breaking the
# build halfway with cryptic API errors).
pkg_check_modules(WLROOTS REQUIRED "wlroots-0.20>=0.20.0" "wlroots-0.20<0.21.0")
pkg_get_variable(WLROOTS_HAVE_XWAYLAND wlroots-0.20 have_xwayland)
if(NOT WLROOTS_HAVE_XWAYLAND STREQUAL "true")
    message(FATAL_ERROR "wlroots-0.20 was built without Xwayland support (needed for X11 apps)")
endif()
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
    src/theme.c
    src/wallpaper.c
    src/view.c
    src/layer.c
    src/group.c
    src/server.c
    src/ui/cairo_overlay.c
    src/ui/tray.c
    src/ui/hints.c
    src/ui/errors.c
    src/ui/welcome.c
    src/ui/launcher.c
    src/ui/logo.c
)

# wlroots ships the layer-shell C header but not the generated protocol header
# it includes, so produce it here. The XML is vendored in protocols/ to keep
# the build independent of any local wlroots checkout.
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
set(LAYER_SHELL_HEADER ${CMAKE_BINARY_DIR}/wlr-layer-shell-unstable-v1-protocol.h)
add_custom_command(
    OUTPUT ${LAYER_SHELL_HEADER}
    COMMAND ${WAYLAND_SCANNER} server-header
            ${CMAKE_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml
            ${LAYER_SHELL_HEADER}
    DEPENDS ${CMAKE_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml
    COMMENT "Generating wlr-layer-shell-unstable-v1 protocol header")

add_executable(fwm-wayland ${SOURCES} ${LAYER_SHELL_HEADER})

target_include_directories(fwm-wayland PRIVATE
        ${WAYLAND_SERVER_INCLUDE_DIRS}
        ${WLROOTS_INCLUDE_DIRS}
        ${XKBCOMMON_INCLUDE_DIRS}
        ${CAIRO_INCLUDE_DIRS}
        ${PANGO_INCLUDE_DIRS}
        ${PANGOCAIRO_INCLUDE_DIRS}
        ${GDK_PIXBUF_INCLUDE_DIRS}
        src
        ${CMAKE_BINARY_DIR}
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