cmake_minimum_required(VERSION 3.10)

# The version lives in one file at the root, not in this one, so that everything
# which needs it reads the same characters: the build, `fwm --version`, the IPC,
# and packaging that has no business parsing CMake (a Nix derivation asked for
# exactly this — see PR #19). Bumping a release means editing VERSION and
# tagging v<that>.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FWM_VERSION)
string(STRIP "${FWM_VERSION}" FWM_VERSION)

project(fwm VERSION ${FWM_VERSION} LANGUAGES C)

set(CMAKE_C_STANDARD 11)
add_compile_definitions(FWM_VERSION="${FWM_VERSION}")

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
# Touchpad settings (tap-to-click, scrolling, acceleration) are libinput's, not
# wlroots': the backend hands out the libinput_device and fwm configures it.
pkg_check_modules(LIBINPUT REQUIRED libinput)
# Arbitrary window rotation is the one effect wlroots' render pass API cannot
# express, so src/rotate.c draws its quad with GLES2 on the renderer's own EGL
# context (see the header for why). Nothing else in fwm touches GL.
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLESV2 REQUIRED glesv2)
pkg_check_modules(DRM REQUIRED libdrm)
# Video wallpaper decode. ffmpeg 6 on Void is the split libav* set + headers in
# ffmpeg6-devel; the runtime is libav*6. Software decode only, so no libva pin.
pkg_check_modules(LIBAV REQUIRED libavformat libavcodec libavutil libswscale)
# Audio: loopback capture for the [cava] visualiser, and playback for the
# [sound] collision knock. OPTIONAL on purpose: a compositor must not refuse to
# build because the machine has no sound stack, and src/audio.c and src/sound.c
# compile to working no-ops without it (the bars never come up, the collisions
# stay silent). On Void the headers are in pipewire-devel.
#
# Playback needs libpulse-simple specifically — PipeWire boxes serve that API
# through pipewire-pulse, so one blocking write on one thread reaches both
# stacks (src/sound.c explains why capture cannot do the same).
#
# BOTH are optional and BOTH are worth having: the visualiser picks at runtime
# by probing for a socket, because which sound server a machine actually runs is
# not knowable at build time — and a PulseAudio box gets no bars from a
# PipeWire-only build.
pkg_check_modules(PIPEWIRE libpipewire-0.3)
pkg_check_modules(PULSE libpulse-simple)
if(NOT PIPEWIRE_FOUND AND NOT PULSE_FOUND)
    message(STATUS "no libpipewire-0.3 and no libpulse-simple "
                   "— [cava] audio visualiser disabled")
else()
    set(CAVA_BACKENDS "")
    if(PIPEWIRE_FOUND)
        list(APPEND CAVA_BACKENDS pipewire)
    endif()
    if(PULSE_FOUND)
        list(APPEND CAVA_BACKENDS pulse)
    endif()
    message(STATUS "[cava] capture backends: ${CAVA_BACKENDS}")
endif()
if(NOT PULSE_FOUND)
    message(STATUS "no libpulse-simple — [sound] collision audio disabled")
endif()
find_package(box2d REQUIRED)
# The video decode runs on its own pthread; the compositor stays single-threaded.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# List of source files
set(SOURCES
    main.c
    src/physics.c
    src/bsp.c
    src/toml.c
    src/config.c
    src/config_binds.c
    src/config_set.c
    src/theme.c
    src/wallpaper.c
    src/video.c
    src/audio.c
    src/cava.c
    src/sound.c
    src/wav.c
    src/ram.c
    src/stats.c
    src/view.c
    src/view_effects.c
    src/rotate.c
    src/snapshot.c
    src/screenshot.c
    src/expo.c
    src/expo_geom.c
    src/expo_cards.c
    src/expo_draw.c
    src/expo_input.c
    src/wobble.c
    src/layer.c
    src/lock.c
    src/foreign.c
    src/workspace.c
    src/shortcuts.c
    src/group.c
    src/ipc.c
    src/ipc_events.c
    src/session.c
    src/server.c
    src/server_lifecycle.c
    src/server_tick.c
    src/server_tiling.c
    src/server_shell.c
    src/server_output.c
    src/server_input.c
    src/server_pointer.c
    src/server_seat.c
    src/server_drag.c
    src/gestures.c
    src/server_gestures.c
    src/server_config.c
    src/server_desktop.c
    src/server_actions.c
    src/ui/cairo_overlay.c
    src/ui/tray.c
    src/ui/modes.c
    src/ui/stats_menu.c
    src/ui/hints.c
    src/ui/errors.c
    src/ui/welcome.c
    src/ui/launcher.c
    src/ui/logo.c
    src/ui/expo_menu.c
    src/ui/expo_hints.c
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

# Protocols wlroots does NOT implement, so fwm implements the interfaces itself
# (see src/shortcuts.c). These need the glue code as well as the header — the
# loop above only produces headers because wlroots supplies the code for its
# own protocols.
set(OWN_PROTOCOLS
    hyprland-global-shortcuts-v1)

foreach(proto ${OWN_PROTOCOLS})
    set(header ${CMAKE_BINARY_DIR}/${proto}-protocol.h)
    set(code ${CMAKE_BINARY_DIR}/${proto}-protocol.c)
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${WAYLAND_SCANNER} server-header
                ${CMAKE_SOURCE_DIR}/protocols/${proto}.xml ${header}
        DEPENDS ${CMAKE_SOURCE_DIR}/protocols/${proto}.xml
        COMMENT "Generating ${proto} protocol header")
    add_custom_command(
        OUTPUT ${code}
        COMMAND ${WAYLAND_SCANNER} private-code
                ${CMAKE_SOURCE_DIR}/protocols/${proto}.xml ${code}
        DEPENDS ${CMAKE_SOURCE_DIR}/protocols/${proto}.xml
        COMMENT "Generating ${proto} protocol code")
    list(APPEND PROTOCOL_HEADERS ${header} ${code})
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
        ${LIBINPUT_INCLUDE_DIRS}
        ${EGL_INCLUDE_DIRS}
        ${GLESV2_INCLUDE_DIRS}
        ${LIBAV_INCLUDE_DIRS}
        ${DRM_INCLUDE_DIRS}
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
        ${LIBINPUT_LIBRARIES}
        ${EGL_LIBRARIES}
        ${GLESV2_LIBRARIES}
        ${GDK_PIXBUF_LIBRARIES}
        ${LIBAV_LIBRARIES}
        box2d::box2d
        Threads::Threads
        m
)

target_compile_definitions(fwm PRIVATE WLR_USE_UNSTABLE)

if(PIPEWIRE_FOUND)
    target_compile_definitions(fwm PRIVATE HAVE_PIPEWIRE)
    target_include_directories(fwm PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
    target_link_libraries(fwm PRIVATE ${PIPEWIRE_LIBRARIES})
endif()

if(PULSE_FOUND)
    target_compile_definitions(fwm PRIVATE HAVE_PULSE)
    target_include_directories(fwm PRIVATE ${PULSE_INCLUDE_DIRS})
    target_link_libraries(fwm PRIVATE ${PULSE_LIBRARIES})
endif()

# Control-socket CLI. Deliberately links nothing: it speaks plain text over a
# unix socket, so it stays usable even if the compositor's deps change.
add_executable(fwmctl tools/fwmctl.c)
# ── unit tests ─────────────────────────────────────────────────────────
# Off by default: a normal build should not pay for targets nobody asked for.
#
#   cmake -S . -B build-test -DFWM_TESTS=ON && cmake --build build-test
#   ctest --test-dir build-test --output-on-failure
#
# Only the modules that need no compositor are covered here. bsp.c is a plain
# tree over ints, and config.c reaches no further than toml and xkbcommon, so
# both link without wlroots, a display, or a running fwm.
option(FWM_TESTS "Build the unit tests" OFF)
if(FWM_TESTS)
    enable_testing()

    add_executable(test_bsp tests/test_bsp.c src/bsp.c)
    target_include_directories(test_bsp PRIVATE src tests)
    target_link_libraries(test_bsp PRIVATE m)

    add_executable(test_config tests/test_config.c
        src/config.c src/config_binds.c src/config_set.c src/toml.c)
    target_include_directories(test_config PRIVATE src tests ${XKBCOMMON_INCLUDE_DIRS})
    target_link_libraries(test_config PRIVATE ${XKBCOMMON_LIBRARIES} m)

    # Input needs physics too: the pointer half of it is what a drag becomes
    # on release, which lives in physics.c. That file reaches Box2D but not
    # wlroots, so it links here without a display.
    add_executable(test_input tests/test_input.c
        src/config.c src/config_binds.c src/config_set.c src/toml.c src/physics.c)
    target_include_directories(test_input PRIVATE src tests ${XKBCOMMON_INCLUDE_DIRS})
    target_link_libraries(test_input PRIVATE ${XKBCOMMON_LIBRARIES} box2d::box2d m)

    # ipc.c itself reaches wlroots, but the event vocabulary it serves does
    # not — that split is what lets `subscribe` argument parsing be checked
    # without a compositor.
    # The ends of the world: a window thrown off one needs a mouse to throw it,
    # and a nested run cannot move the pointer — so the ring's physics is
    # asserted here rather than by hand. physics.c reaches Box2D, not wlroots.
    add_executable(test_physics tests/test_physics.c src/physics.c)
    target_include_directories(test_physics PRIVATE src tests)
    target_link_libraries(test_physics PRIVATE box2d::box2d m)

    # Everything the physics has to keep doing at speed. Its own suite because it
    # came out of one bug report and every case is a measurement that used to
    # fail — a drag needs a mouse, and a nested run cannot inject one.
    add_executable(test_physics_speed tests/test_physics_speed.c src/physics.c)
    target_include_directories(test_physics_speed PRIVATE src tests)
    target_link_libraries(test_physics_speed PRIVATE box2d::box2d m)

    add_executable(test_ipc_events tests/test_ipc_events.c src/ipc_events.c)
    target_include_directories(test_ipc_events PRIVATE src tests)

    # The drag wobble's lattice is springs over doubles. It only ever runs with
    # a mouse button held down, which is exactly when nobody can inspect it —
    # so it is asserted here instead.
    add_executable(test_wobble tests/test_wobble.c src/wobble.c)
    target_include_directories(test_wobble PRIVATE src tests)
    target_link_libraries(test_wobble PRIVATE m)

    # Touchpad gestures are decided in gestures.c, which reaches no further
    # than config.h's types — the wlroots half is server_gestures.c and stays
    # out of it, so what a swipe means can be asserted without a touchpad.
    add_executable(test_gestures tests/test_gestures.c src/gestures.c)
    target_include_directories(test_gestures PRIVATE src tests ${XKBCOMMON_INCLUDE_DIRS})
    target_link_libraries(test_gestures PRIVATE m)

    # Application memory footprints come out of /proc, which cannot be faked
    # and is easy to misparse. The suite forks its own child to check the tree
    # walk, so it needs nothing of the machine but a working /proc.
    add_executable(test_ram tests/test_ram.c src/ram.c)
    target_include_directories(test_ram PRIVATE src tests)
    target_link_libraries(test_ram PRIVATE m)

    # [sound] path points wav.c at a file fwm did not write, so the parser is
    # fed truncated and lying headers here. No wlroots and no audio backend:
    # that is the whole reason the parse lives in its own module.
    add_executable(test_wav tests/test_wav.c src/wav.c)
    target_include_directories(test_wav PRIVATE src tests)
    target_link_libraries(test_wav PRIVATE m)

    # The stats engine forks children and reads /proc, so the suite is the only
    # place the process handling can be checked at all: a leaked child or a
    # blocking read shows up here and nowhere else. It reaches wlroots for its
    # log line and for nothing else.
    add_executable(test_stats tests/test_stats.c src/stats.c)
    target_include_directories(test_stats PRIVATE src tests ${WLROOTS_INCLUDE_DIRS}
        ${WAYLAND_SERVER_INCLUDE_DIRS} ${PIXMAN_INCLUDE_DIRS} ${XKBCOMMON_INCLUDE_DIRS})
    target_compile_definitions(test_stats PRIVATE WLR_USE_UNSTABLE)
    target_link_libraries(test_stats PRIVATE ${WLROOTS_LIBRARIES} m)
    target_link_directories(test_stats PRIVATE ${WLROOTS_LIBRARY_DIRS})

    add_test(NAME bsp        COMMAND test_bsp)
    add_test(NAME config     COMMAND test_config)
    add_test(NAME input      COMMAND test_input)
    add_test(NAME physics    COMMAND test_physics)
    add_test(NAME physics_speed COMMAND test_physics_speed)
    add_test(NAME ipc_events COMMAND test_ipc_events)
    add_test(NAME wobble     COMMAND test_wobble)
    add_test(NAME gestures   COMMAND test_gestures)
    add_test(NAME ram        COMMAND test_ram)
    add_test(NAME wav        COMMAND test_wav)
    add_test(NAME stats      COMMAND test_stats)
endif()
