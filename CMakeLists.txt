cmake_minimum_required(VERSION 3.10)
project(fwm C)

set(CMAKE_C_STANDARD 11)

# A compositor cannot report its own memory corruption: it IS the display
# server, so a smashed stack takes the whole session down with no way to see
# what happened. These are cheap enough to keep on in release builds.
#
# -fstack-protector-strong earns its keep: a 256-entry array overrun in the
# tiling code ran silently for lack of it, and only showed up under ASan.
add_compile_options(
    -Wall
    -Wextra
    -Wno-unused-parameter   # wlroots listeners take args most handlers ignore
    -fstack-protector-strong
)
# Frame pointers make the backtrace from a crash or a sanitizer usable; the
# cost only lands on debug builds, where nobody is counting registers.
add_compile_options($<$<CONFIG:Debug>:-fno-omit-frame-pointer>)

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
# Used directly for pointer-constraint regions (pixman_region32_contains_point);
# wlroots exposes pixman types in its API but does not link it for us.
pkg_check_modules(PIXMAN REQUIRED pixman-1)
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
    src/lock.c
    src/foreign.c
    src/group.c
    src/ipc.c
    src/session.c
    src/server.c
    src/server_shell.c
    src/server_output.c
    src/server_input.c
    src/server_pointer.c
    src/server_config.c
    src/server_desktop.c
    src/server_actions.c
    src/ui/cairo_overlay.c
    src/ui/tray.c
    src/ui/hints.c
    src/ui/errors.c
    src/ui/welcome.c
    src/ui/launcher.c
    src/ui/logo.c
)

# wlroots ships the C headers for the wlr-* protocols but not the generated
# protocol headers they include, so produce those here. The XMLs are vendored
# in protocols/ to keep the build independent of any local wlroots checkout.
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
set(WLR_PROTOCOLS
    wlr-layer-shell-unstable-v1
    wlr-output-power-management-unstable-v1
    wlr-gamma-control-unstable-v1)

set(PROTOCOL_HEADERS "")
foreach(proto ${WLR_PROTOCOLS})
    set(header ${CMAKE_BINARY_DIR}/${proto}-protocol.h)
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${WAYLAND_SCANNER} server-header
                ${CMAKE_SOURCE_DIR}/protocols/${proto}.xml
                ${header}
        DEPENDS ${CMAKE_SOURCE_DIR}/protocols/${proto}.xml
        COMMENT "Generating ${proto} protocol header")
    list(APPEND PROTOCOL_HEADERS ${header})
endforeach()

add_executable(fwm ${SOURCES} ${PROTOCOL_HEADERS})

target_include_directories(fwm PRIVATE
        ${WAYLAND_SERVER_INCLUDE_DIRS}
        ${WLROOTS_INCLUDE_DIRS}
        ${XKBCOMMON_INCLUDE_DIRS}
        ${CAIRO_INCLUDE_DIRS}
        ${PANGO_INCLUDE_DIRS}
        ${PANGOCAIRO_INCLUDE_DIRS}
        ${GDK_PIXBUF_INCLUDE_DIRS}
        ${PIXMAN_INCLUDE_DIRS}
        src
        ${CMAKE_BINARY_DIR}
        src/ui
)

target_link_libraries(fwm PRIVATE
        ${WAYLAND_SERVER_LIBRARIES}
        ${WLROOTS_LIBRARIES}
        ${XKBCOMMON_LIBRARIES}
        ${CAIRO_LIBRARIES}
        ${PANGO_LIBRARIES}
        ${PANGOCAIRO_LIBRARIES}
        ${PIXMAN_LIBRARIES}
        ${GDK_PIXBUF_LIBRARIES}
        box2d::box2d
        m
)

target_compile_definitions(fwm PRIVATE WLR_USE_UNSTABLE)

# Control-socket CLI. Deliberately links nothing: it speaks plain text over a
# unix socket, so it stays usable even if the compositor's deps change.
add_executable(fwmctl tools/fwmctl.c)