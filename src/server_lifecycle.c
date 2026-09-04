/*
 * fwm — a Wayland compositor
 * Copyright (C) 2026 Ilu
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

/* Lifecycle: bringing the compositor up, running the display loop, and taking
 * it all down again in the reverse order. Split out of server.c; see
 * server_internal.h. */
#include "server.h"
#include "bsp.h"
#include "clipboard.h"
#include "view.h"
#include "rotate.h"
#include "shadow.h"
#include "glass.h"
#include "physics.h"
#include "theme.h"
#include "layer.h"
#include "lock.h"
#include "foreign.h"
#include "workspace.h"
#include "shortcuts.h"
#include "ipc.h"
#include "session.h"
#include "launched.h"
#include <signal.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include "ui/tray.h"
#include "ui/stats_menu.h"
#include "stats.h"
#include "ui/hints.h"
#include "ui/errors.h"
#include "screenshot.h"
#include "ui/welcome.h"
#include "ui/launcher.h"
#include "ui/osd.h"
#include "volume.h"
#include "ui/radial.h"
#include "ui/mixer.h"
#include "expo.h"
#include "ui/cairo_overlay.h"
#include "wallpaper.h"
#include "star_draw.h"
#include "cava.h"
#include "sound.h"
#include "sandbox.h"
#include "server_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <wayland-server.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/render/color.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* SIGTERM/SIGINT arrive when the session ends or a dev run is killed. Without
 * these the process dies where it stands, server_destroy never runs, and the
 * control socket is left behind as a dead file. Terminating the display loop
 * instead makes the normal teardown path the only exit path. */
static int handle_signal(int signal, void *data) {
    FwmServer *server = data;
    wlr_log(WLR_INFO, "caught signal %d, shutting down", signal);
    server->running = 0;
    wl_display_terminate(server->wl_display);
    return 0;
}

/* Hand the session environment to the D-Bus session bus.
 *
 * Portals are D-Bus-activated, so a backend is spawned by the bus and inherits
 * the bus daemon's environment — not ours. The bus is started before fwm, so
 * without this it has no WAYLAND_DISPLAY and no XDG_CURRENT_DESKTOP: the
 * frontend cannot tell which backend to pick, and xdg-desktop-portal-wlr
 * cannot connect back to us to capture anything. That is what makes screen
 * sharing fail in Discord, OBS and every other portal client.
 *
 * Entirely best-effort. dbus-update-activation-environment need not exist, and
 * a session without a bus is still a working session — just one without
 * portals, so we say so once and carry on.
 *
 * Nothing here may block for long, and nothing here may talk to a bus we would
 * have to start ourselves. This runs inside server_init, before the event loop:
 * by now libseat has taken the VT, which means graphics mode and the console
 * keyboard switched off. A compositor stuck here does not look stuck, it looks
 * like a dead machine — no picture, no keys, not even a VT switch, only the
 * power button. See the DISPLAY note below for how that used to happen. */

/* Is there a session bus to talk to at all?
 *
 * This is the whole reason the exec below is safe. With no address libdbus
 * falls back to X11 autolaunch: it asks the X server named by DISPLAY for the
 * bus address — and DISPLAY is our own Xwayland, created lazily, which cannot
 * answer until the event loop starts it. The event loop is what we are on our
 * way to. The child blocks on the X handshake, we block on the child, and the
 * session is gone before it drew a frame. Both checks mirror what libdbus
 * itself tries before it resorts to autolaunch. */
static bool session_bus_exists(void) {
    const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (addr && *addr) return true;

    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) return false;
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/bus", runtime) >= (int)sizeof(path)) return false;
    struct stat st;
    return stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

static void export_session_environment(void) {
    /* Identifies the desktop to the portal frontend. "wlroots" is the name
     * xdg-desktop-portal-wlr answers to; "fwm" lets a portals.conf single us
     * out. Never clobber a value the session script chose. */
    setenv("XDG_CURRENT_DESKTOP", "fwm:wlroots", 0);
    /* Display managers that launch us from a VT leave this as "tty". */
    setenv("XDG_SESSION_TYPE", "wayland", 1);

    if (!session_bus_exists()) {
        wlr_log(WLR_INFO, "no D-Bus session bus: portals such as screen sharing "
                          "will not work (start fwm under dbus-run-session)");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        wlr_log(WLR_ERROR, "cannot fork to export the session environment: "
                           "portals (screen sharing) will not work");
        return;
    }
    if (pid == 0) {
        /* Its own session: whatever the bus tools leave running behind them
         * is not in our process group and does not share our terminal. */
        setsid();
        execlp("dbus-update-activation-environment",
               "dbus-update-activation-environment",
               "WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE",
               "XDG_RUNTIME_DIR", "DISPLAY",
               "XCURSOR_THEME", "XCURSOR_SIZE", (char *)NULL);
        _exit(127);
    }

    /* Wait for it, but never indefinitely: the portal must not be activated
     * with a half-updated environment, and the process is short-lived by
     * construction — while a bus that accepts a connection and then answers
     * nothing is a hang with no timeout of its own. A second is far longer
     * than the round trip needs and short enough to sit through; past that we
     * take the session over the portals and kill it. */
    static const int wait_ms = 1000, step_ms = 5;
    int status = 0, waited = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) return;    /* nothing left to reap */
        if (r == 0 && waited >= wait_ms) {
            wlr_log(WLR_ERROR, "dbus-update-activation-environment did not "
                               "answer in %dms: giving up on it so the session "
                               "can start; portals may not work", wait_ms);
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                // reap the corpse; it cannot escape SIGKILL
            }
            return;
        }
        struct timespec ts = { 0, step_ms * 1000000L };
        nanosleep(&ts, NULL);
        waited += step_ms;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        wlr_log(WLR_INFO, "dbus-update-activation-environment did not run "
                          "(is dbus installed?): portals such as screen "
                          "sharing may not work");
    }
}

/* The cursor theme, which is worth choosing rather than leaving to chance.
 *
 * Asked for no theme by name, wlroots looks for one called "default" and, when
 * there is none installed (there usually is not — the convention is a
 * ~/.icons/default symlink that nothing creates on its own), quietly falls back
 * to a built-in set of four: default, left_ptr, text and pointer. Everything a
 * client asks for beyond those — the resize arrows over a splitter or a panel
 * edge, the grab hand, the crosshair — resolves to nothing, and
 * wlr_cursor_set_xcursor leaves the cursor as it was. The pointer stops
 * changing shape at all, which looks like the compositor ignoring the client
 * when in fact it is asking for a picture nobody has.
 *
 * XCURSOR_THEME/XCURSOR_SIZE are what every toolkit reads and what a user sets
 * to choose a cursor, so a theme named there is taken as named — even a bad
 * one, because overriding a choice someone made explicitly is worse than
 * honouring it badly. Only with nothing asked for do we go looking, and then
 * a candidate has to prove itself by having a cursor the built-in set has not. */
static struct wlr_xcursor_manager *cursor_theme_load(void) {
    int size = 24;
    const char *env_size = getenv("XCURSOR_SIZE");
    if (env_size) {
        int v = atoi(env_size);
        if (v >= 8 && v <= 256) size = v;
    }

    const char *env_theme = getenv("XCURSOR_THEME");
    if (env_theme && *env_theme) {
        struct wlr_xcursor_manager *mgr = wlr_xcursor_manager_create(env_theme, size);
        if (mgr) {
            wlr_xcursor_manager_load(mgr, 1);
            /* Said and done — but say so when the theme predates the CSS names
             * clients ask by (an old X theme has sb_h_double_arrow and no
             * ew-resize), because the symptom is a cursor that never changes
             * shape and nothing else would explain it. */
            if (!wlr_xcursor_manager_get_xcursor(mgr, "ew-resize", 1)) {
                wlr_log(WLR_INFO, "cursor theme %s has no ew-resize: clients "
                                  "asking for modern cursor names will get no "
                                  "cursor change at all", env_theme);
            }
            return mgr;
        }
    }

    /* Adwaita ships with the GTK icon theme, so it is on practically every
     * desktop; NULL is "whatever wlroots would have done", the last resort. */
    const char *tries[] = { "Adwaita", NULL };
    for (size_t i = 0; i < sizeof(tries) / sizeof(*tries); i++) {
        bool last = (tries[i] == NULL);
        struct wlr_xcursor_manager *mgr = wlr_xcursor_manager_create(tries[i], size);
        if (!mgr) continue;
        wlr_xcursor_manager_load(mgr, 1);
        if (last || wlr_xcursor_manager_get_xcursor(mgr, "ew-resize", 1)) {
            return mgr;
        }
        wlr_xcursor_manager_destroy(mgr);
    }
    return NULL;
}

/* ---- GPU reset recovery ------------------------------------------------
 *
 * A GPU reset — the driver giving up on a hung command stream and restarting
 * the card — takes the EGL context down with it. Every GL object made on that
 * context is gone, and every call into it silently does nothing. The renderer
 * serves ALL monitors, so a compositor that does not notice keeps compositing
 * through the corpse and paints garbage (on radeonsi, a flat acid green) on
 * every screen at once, for good: the picture never comes back, because
 * nothing ever rebuilds the context.
 *
 * wlroots does notice, and says so through renderer->events.lost. The whole of
 * the fix is to take it up: let go of everything that lived in the dead
 * context, build a new renderer and allocator, and point the compositor and
 * every output at them. Clients need no help — wlroots drops their textures
 * itself (wlr_client_buffer keeps a renderer_destroy listener for exactly
 * this) and they come back on their next commit.
 *
 * The reset itself is not ours to prevent; it is whatever hung the card. This
 * only decides whether the desktop survives it. */

static void handle_renderer_lost(struct wl_listener *listener, void *data);

/* FWM_DEBUG_GPU_LOST=<seconds>: raise the lost signal by hand that long after
 * startup, as the driver would after a reset.
 *
 * Recovery code that has never run is a guess. A real reset needs a hung card
 * and cannot be asked for, so this asks wlroots' signal for it instead —
 * everything downstream of the signal is then the genuine path, unchanged. */
static int gpu_lost_debug_cb(void *data) {
    FwmServer *server = data;
    wlr_log(WLR_INFO, "FWM_DEBUG_GPU_LOST: pretending the GPU was reset");
    wl_signal_emit_mutable(&server->wlr_renderer->events.lost, server->wlr_renderer);
    return 0;
}

static void gpu_lost_debug_arm(FwmServer *server, struct wl_event_loop *loop) {
    const char *s = getenv("FWM_DEBUG_GPU_LOST");
    if (!s || !*s) return;
    int secs = atoi(s);
    if (secs < 1) secs = 1;
    struct wl_event_source *t = wl_event_loop_add_timer(loop, gpu_lost_debug_cb, server);
    if (!t) return;
    wl_event_source_timer_update(t, secs * 1000);
    wlr_log(WLR_INFO, "FWM_DEBUG_GPU_LOST: will fake a GPU reset in %ds", secs);
}

static void renderer_watch_lost(FwmServer *server) {
    server->renderer_lost.notify = handle_renderer_lost;
    wl_signal_add(&server->wlr_renderer->events.lost, &server->renderer_lost);
}

/* Let go of every GPU object fwm itself caches across frames.
 *
 * Called while the dying renderer is still a live C object, because destroying
 * a texture calls through it — doing this after wlr_renderer_destroy would be
 * a use-after-free. Calls into the lost CONTEXT are harmless no-ops; calls
 * into freed MEMORY are not, and the ordering is the whole difference.
 *
 * Animations in flight are stopped rather than carried across. They are all
 * transient by nature, and a window that finishes its wobble instantly beats
 * one drawn from a texture that no longer exists. */
static void gpu_release_caches(FwmServer *server) {
    /* The strip holds a photograph of every window on every desktop, and the
     * orrery's star on top of that. */
    expo_destroy(server);
    /* A shot in flight holds the still it is flying across the screen. */
    screenshot_cleanup(server);

    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        view_jelly_stop(view);
        view_stop_spin(view);
        view_stop_squash(view);
    }

    FwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        server_wrap_slide_stop(server, out);   /* holds the outgoing desktop */
        wallpaper_gpu_release(out->wallpaper);
        wallpaper_gpu_release(out->wallpaper_prev);
        star_draw_gpu_release(out->star_draw);
    }

    /* rotate.c's and star_gl.c's shader programs are static and keyed to the
     * renderer that built them; both notice the new one on their own. This
     * still has to run, because rotate.c also holds a raw GL texture name from
     * the last capture, which nothing else clears. */
    rotate_shutdown(server->wlr_renderer);
    /* And everything the frost under the panels keeps on the GPU. The panes
     * themselves stay; the next frame builds their buffers again. */
    glass_gpu_release();
}

/* The rebuild proper, on an idle callback rather than straight off the signal.
 *
 * The renderer cannot be destroyed from inside its own lost handler: the
 * emission is still in progress, and wl_signal_emit_mutable keeps two marker
 * listeners on the signal for the length of it, so wlr_renderer_destroy's
 * "no listeners left" assertion trips. Waiting for idle also gets us off
 * whatever stack raised the loss — plausibly a render pass inside the very
 * renderer being replaced. */
static void renderer_rebuild(void *data) {
    FwmServer *server = data;

    struct wlr_renderer *old_renderer = server->wlr_renderer;
    struct wlr_allocator *old_allocator = server->wlr_allocator;

    gpu_release_caches(server);

    struct wlr_renderer *renderer = wlr_renderer_autocreate(server->wlr_backend);
    if (!renderer) {
        wlr_log(WLR_ERROR, "GPU reset: no renderer could be created; shutting down");
        wl_display_terminate(server->wl_display);
        return;
    }
    struct wlr_allocator *allocator =
        wlr_allocator_autocreate(server->wlr_backend, renderer);
    if (!allocator) {
        wlr_log(WLR_ERROR, "GPU reset: no allocator could be created; shutting down");
        wlr_renderer_destroy(renderer);
        wl_display_terminate(server->wl_display);
        return;
    }

    server->wlr_renderer = renderer;
    server->wlr_allocator = allocator;

    /* NOT wlr_renderer_init_wl_display again: it creates the wl_shm and
     * linux-dmabuf globals, and those keep a COPY of the format set rather
     * than a pointer to the renderer, so they neither dangle nor go stale
     * across a reset of the same card. Calling it twice would hang a second
     * set of globals off the display. */

    /* What imports client buffers on commit from here on. */
    wlr_compositor_set_renderer(server->compositor, renderer);

    /* Every output's swapchain came out of the old allocator. Re-init before
     * dropping it, so the old buffers are released in the swap. */
    FwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        wlr_output_init_render(out->wlr_output, allocator, renderer);
    }

    wlr_allocator_destroy(old_allocator);
    wlr_renderer_destroy(old_renderer);

    /* Now that the new allocator exists, give back what needed it. A star that
     * cannot be rebuilt is dropped rather than left as a node with no buffer
     * behind it; the desktop keeps working without it. */
    wl_list_for_each(out, &server->outputs, link) {
        if (out->star_draw && !star_draw_gpu_rebuild(out->star_draw)) {
            wlr_log(WLR_ERROR, "GPU reset: the star could not be rebuilt, dropping it");
            star_draw_destroy(out->star_draw);
            out->star_draw = NULL;
        }
    }

    renderer_watch_lost(server);
    server->renderer_recovering = false;

    server_schedule_frames(server);
    wlr_log(WLR_INFO, "GPU reset: renderer rebuilt, the desktop is back");
}

static void handle_renderer_lost(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, renderer_lost);
    (void)data;

    /* A second loss reported before the swap finishes would re-enter this with
     * half a renderer in place. */
    if (server->renderer_recovering) return;
    server->renderer_recovering = true;

    wlr_log(WLR_ERROR, "GPU reset: the renderer was lost, rebuilding it");

    /* Off the signal now, while merely unhooking is safe, so nothing arrives
     * twice between here and the rebuild. */
    wl_list_remove(&server->renderer_lost.link);
    wl_list_init(&server->renderer_lost.link);

    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    if (!wl_event_loop_add_idle(loop, renderer_rebuild, server)) {
        wlr_log(WLR_ERROR, "GPU reset: cannot schedule the rebuild; shutting down");
        wl_display_terminate(server->wl_display);
    }
}

bool server_init(FwmServer *server, bool debug) {
    memset(server, 0, sizeof(*server));
    server->debug = debug;
    server->key_mode = -1;   /* the root keymap; 0 would be the first submap */
    /* Read once: the diagnostic must cost nothing at all when it is off. */
    server->fx_debug = getenv("FWM_DEBUG_EFFECTS") != NULL;
    server->wl_display = wl_display_create();
    if (!server->wl_display) {
        wlr_log(WLR_ERROR, "failed to create display");
        return false;
    }
    
    struct wl_event_loop *event_loop = wl_display_get_event_loop(server->wl_display);

    /* MUST happen before the backend/renderer, and thus before the GPU driver
     * spawns its worker threads. wl_event_loop_add_signal blocks the signal
     * with sigprocmask, and a thread inherits the mask AS IT IS AT CREATION —
     * threads that already exist keep accepting the signal. The kernel hands a
     * process-directed signal to any thread that does not block it, so with
     * this registered later the driver's threads took SIGTERM and the default
     * disposition killed us (exit 143) with server_destroy never running.
     * Verified by reading SigBlk from each thread's status under
     * /proc/<pid>/task: it was 0 on those threads. */
    wl_event_loop_add_signal(event_loop, SIGTERM, handle_signal, server);
    wl_event_loop_add_signal(event_loop, SIGINT, handle_signal, server);

    server->wlr_backend = wlr_backend_autocreate(event_loop, &server->session);
    if (!server->wlr_backend) {
        wlr_log(WLR_ERROR, "failed to create backend");
        return false;
    }
    
    server->wlr_renderer = wlr_renderer_autocreate(server->wlr_backend);
    if (!server->wlr_renderer) {
        wlr_log(WLR_ERROR, "failed to create renderer");
        return false;
    }
    wlr_renderer_init_wl_display(server->wlr_renderer, server->wl_display);
    /* From here on a GPU reset is survivable: see the recovery block above. */
    renderer_watch_lost(server);
    gpu_lost_debug_arm(server, event_loop);
    
    server->wlr_allocator = wlr_allocator_autocreate(server->wlr_backend, server->wlr_renderer);
    if (!server->wlr_allocator) {
        wlr_log(WLR_ERROR, "failed to create allocator");
        return false;
    }

    server->compositor = wlr_compositor_create(server->wl_display, 5, server->wlr_renderer);
    wlr_subcompositor_create(server->wl_display);
    wlr_data_device_manager_create(server->wl_display);
    /* Screen capture, in both generations.
     *
     * wlr-screencopy is what the tools on disk today still speak: grim,
     * wf-recorder, and the xdg-desktop-portal-wlr behind every "share your
     * screen" button. It is also deprecated upstream, and the portals and
     * recorders are moving off it.
     *
     * ext-image-copy-capture is where they are moving. It splits the job in
     * two: a *source* names what is being captured, and a session copies
     * frames out of it. fwm serves the output sources — one per monitor —
     * which is the whole of what a screen share needs, and wlroots does the
     * copying itself. Per-window capture is a second source manager that wants
     * ext-foreign-toplevel-list first; fwm publishes its window list over the
     * wlr protocol only, so that one is not offered yet.
     *
     * Both run at once on purpose: dropping the old one would break every
     * tool that has not moved, and adding the new one costs two globals. */
    wlr_screencopy_manager_v1_create(server->wl_display);
    wlr_ext_output_image_capture_source_manager_v1_create(server->wl_display, 1);
    wlr_ext_image_copy_capture_manager_v1_create(server->wl_display, 1);
    // Standard client protocols, all self-contained in wlroots:
    // primary selection = middle-click paste; viewporter + fractional-scale +
    // single-pixel-buffer are expected by games/video players/toolkits;
    // presentation-time gives clients accurate frame timing (mpv, games).
    wlr_primary_selection_v1_device_manager_create(server->wl_display);
    /* Clipboard managers (cliphist and the widgets that wrap it) read the
     * selection without holding keyboard focus, which is the one thing the
     * ordinary data-device cannot give them. Both spellings: ext- is the
     * standardised one, wlr- is what most of the existing tools still bind. */
    wlr_data_control_manager_v1_create(server->wl_display);
    wlr_ext_data_control_manager_v1_create(server->wl_display, 1);
    wlr_viewporter_create(server->wl_display);
    wlr_fractional_scale_manager_v1_create(server->wl_display, 1);
    wlr_single_pixel_buffer_manager_v1_create(server->wl_display);
    wlr_presentation_create(server->wl_display, server->wlr_backend, 2);

    /* Which clients came out of a sandbox, and what they are not shown for it.
     * Installed here, well before the socket exists: the filter has to be in
     * place before any client can be sent a registry. See sandbox.h. */
    sandbox_init(server);

    server->output_layout = wlr_output_layout_create(server->wl_display);
    // xdg-output: exposes output geometry to clients (grim/wf-recorder need it).
    wlr_xdg_output_manager_v1_create(server->wl_display, server->output_layout);

    server->scene = wlr_scene_create();
    server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);

    // Scene layers, created bottom-to-top: parallax wallpaper sits below the
    // windows, and the tray/hints/welcome overlays sit above them so a raised
    // window can never cover them.
    server->layer_background = wlr_scene_tree_create(&server->scene->tree);
    server->layer_windows = wlr_scene_tree_create(&server->scene->tree);
    server->layer_overlay = wlr_scene_tree_create(&server->scene->tree);
    /* The frost under fwm's own panels hangs in this layer; it needs the
     * server once, and then panels attach to it in one line each. */
    glass_init(server);

    // Layer-shell trees are woven between ours. Creation order alone cannot
    // express this (new trees always land on top), so place them explicitly:
    // wallpaper < ls_background < ls_bottom < windows < ls_top < our overlays
    // < ls_overlay. A layer-shell overlay therefore outranks even the tray,
    // which is what clients like a screen locker or a menu expect.
    server->ls_background = wlr_scene_tree_create(&server->scene->tree);
    server->ls_bottom = wlr_scene_tree_create(&server->scene->tree);
    server->ls_top = wlr_scene_tree_create(&server->scene->tree);
    server->ls_overlay = wlr_scene_tree_create(&server->scene->tree);
    wlr_scene_node_place_above(&server->ls_background->node, &server->layer_background->node);
    wlr_scene_node_place_above(&server->ls_bottom->node, &server->ls_background->node);
    wlr_scene_node_place_above(&server->layer_windows->node, &server->ls_bottom->node);
    wlr_scene_node_place_above(&server->ls_top->node, &server->layer_windows->node);
    wlr_scene_node_place_above(&server->layer_overlay->node, &server->ls_top->node);
    wlr_scene_node_place_above(&server->ls_overlay->node, &server->layer_overlay->node);
    /* The lock screen outranks everything, including an external bar's overlay
     * layer — nothing may be drawn over a locked session. Disabled until a
     * lock actually engages. */
    server->layer_lock = wlr_scene_tree_create(&server->scene->tree);
    wlr_scene_node_place_above(&server->layer_lock->node, &server->ls_overlay->node);
    wlr_scene_node_set_enabled(&server->layer_lock->node, false);

    server->launcher = launcher_create(server);
    server->radial = radial_create(server);
    server->mixer  = mixer_create(server);
    server->osd = osd_create(server);
    server->volume = volume_create(server);

    wl_list_init(&server->views);
    wl_list_init(&server->xwl_unmanaged);
    wl_list_init(&server->groups);
    wl_list_init(&server->ghosts);
    wl_list_init(&server->outputs);
    wl_list_init(&server->keyboards);
    wl_list_init(&server->pointers);
    wl_list_init(&server->switches);
    
    server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 3); // xdg-shell v3/v6 depending on wlroots version (v3 is standard in 0.17+)
    layer_shell_init(server);
    lock_init(server);
    foreign_init(server);
    workspace_init(server);
    shortcuts_init(server);

    server->cursor = wlr_cursor_create();
    server->cursor_mgr = cursor_theme_load();
    if (!server->cursor_mgr) {
        wlr_log(WLR_ERROR, "cannot create a cursor theme");
        return false;
    }
    /* Name the theme to the clients as well. Plenty of them never ask us for a
     * cursor picture at all — GTK loads the theme itself and hands us a
     * finished surface, Xwayland's Xcursor does the same for X clients — and
     * they find it by reading XCURSOR_THEME/XCURSOR_SIZE out of their
     * environment. Left unset, each one picks whatever it likes and the
     * pointer changes not just shape but style as it crosses between windows.
     *
     * Overwriting rather than deferring to what is already there: the value we
     * settled on may be a REPLACEMENT for the environment's, and a client
     * pointed at the theme we rejected is exactly the mismatch this avoids.
     * The whole environment is exported to D-Bus below, so activated services
     * get it too. Nothing is set when wlroots chose for us (no name to give). */
    wlr_log(WLR_INFO, "cursor theme: %s, size %u",
            server->cursor_mgr->name ? server->cursor_mgr->name : "(wlroots' own)",
            server->cursor_mgr->size);
    if (server->cursor_mgr->name) {
        setenv("XCURSOR_THEME", server->cursor_mgr->name, 1);
    }
    char cursor_size[16];
    snprintf(cursor_size, sizeof(cursor_size), "%u", server->cursor_mgr->size);
    setenv("XCURSOR_SIZE", cursor_size, 1);
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
    // Show a default cursor image immediately. Without this the pointer has no
    // image until a client sets one, so it looks "gone" for the first few
    // seconds after startup (and over the empty background). (The theme itself
    // is already loaded, by cursor_theme_load, which has to read it to know
    // whether it is the one to use.)
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");

    server->seat = wlr_seat_create(server->wl_display, "seat0");

    /* Straight after the seat, because it listens to the seat's selection and
     * a client could take it as soon as one exists. Its [clipboard] settings
     * arrive further down — the config file has not been read yet at this
     * point, and the module's own defaults hold until it has. */
    server->clipboard = clipboard_create(server->wl_display, server->seat);

    // Xwayland (lazy: the X server starts on the first X11 client). Managed
    // windows become FwmViews, override-redirect ones bare scene surfaces.
    server->xwayland = wlr_xwayland_create(server->wl_display, server->compositor, true);
    if (server->xwayland) {
        setenv("DISPLAY", server->xwayland->display_name, true);
        wlr_log(WLR_INFO, "Xwayland on DISPLAY=%s", server->xwayland->display_name);
    } else {
        wlr_log(WLR_ERROR, "Failed to start Xwayland; X11 apps won't work");
    }

    /* No listeners needed: the notifier is driven by server_notify_activity
     * from the input paths, and the inhibit manager keeps its own list, which
     * idle_inhibit_refresh polls each tick. */
    server->relative_pointer = wlr_relative_pointer_manager_v1_create(server->wl_display);
    server->pointer_constraints = wlr_pointer_constraints_v1_create(server->wl_display);

    /* Touchpad gestures. The global is what lets a gesture fwm has no bind for
     * reach the client instead (see server_gestures.c). */
    server->pointer_gestures = wlr_pointer_gestures_v1_create(server->wl_display);

    server->output_power = wlr_output_power_manager_v1_create(server->wl_display);
    server->gamma_control = wlr_gamma_control_manager_v1_create(server->wl_display);
    if (server->gamma_control) {
        /* The scene applies client ramps itself, including re-applying them
         * when an output comes back — far less to get wrong than committing
         * them by hand. We only watch the event to stand our own night light
         * down when a client takes the ramp over. */
        wlr_scene_set_gamma_control_manager_v1(server->scene, server->gamma_control);
    }
    server->cursor_shape = wlr_cursor_shape_manager_v1_create(server->wl_display, 1);

    /* Lets a client — in practice a game, through Mesa's Vulkan WSI when it is
     * running without vsync — ask for its frames to be shown the instant they
     * are ready rather than at the next refresh. Advertising the global costs
     * nothing and promises nothing: the hint is only ever acted on for a
     * real-fullscreen window on a monitor whose allow_tearing is set. */
    server->tearing_control = wlr_tearing_control_manager_v1_create(server->wl_display, 1);

    server->idle_notifier = wlr_idle_notifier_v1_create(server->wl_display);
    server->idle_inhibit  = wlr_idle_inhibit_v1_create(server->wl_display);
    server->idle_inhibited = 0;

    server->xdg_activation = wlr_xdg_activation_v1_create(server->wl_display);

    /* Each module wires its own listeners. Nothing can fire while init runs —
     * the display socket is created below and the backend does not start until
     * server_run() — so doing this in one place, after every object exists, is
     * equivalent to interleaving it with construction, and it lets the handlers
     * stay private to the file that implements them. */
    server_shell_register(server);
    server_pointer_register(server);
    server_gestures_register(server);
    server_output_register(server);
    server_input_register(server);
    
    // Load config
    char path[512];
    server_config_path(path, sizeof(path));
    config_load(&server->config, path);
    /* Between the file and everything that goes over it: what save --all
     * later means by "changed". */
    server_settings_baseline(server);
    server_state_apply_settings(server);
    server_state_apply_wallpaper(server);
    server_state_apply_modes(server);
    /* `[tiling] default`, or the modes the last run ended in when `remember`
     * is on — server_state_apply_modes above has already put one over the
     * other. Written straight into the array instead of going through
     * server_set_desktop_mode because there is nothing yet to move — no
     * window, no tray, no ipc — and a desktop that is tiled BEFORE the session
     * restores is one the restored windows join as tiles, instead of landing
     * as loose bodies the layout swallows a frame later. Only here, never on
     * reload: after that the mode is the user's to toggle. */
    for (int d = 0; d < FWM_DESKTOPS; d++)
        server->desktop_mode[d] = server->config.tiling.default_mode[d];
    /* And the shape of every split made from here on. */
    bsp_configure((float)server->config.tiling.split_ratio,
                  server->config.tiling.force_split);
    /* Prime the `setting` event's memory with what the session is starting
     * from. Without it the first change of the session is the call that fills
     * the snapshot in, and so is the one change nobody is told about. */
    server_settings_notify(server);
    // Palette for every overlay and window border; may sample the wallpaper.
    theme_build(&server->config);

    /* And the clipboard's settings, now that there is a config to read them
     * from — the module has been listening since the seat existed. */
    clipboard_configure(server->clipboard, server->config.clipboard.persist != 0,
                        (size_t)(server->config.clipboard.max_kb * 1024.0));

    /* The tray's readouts. Created after the config and before the first frame:
     * the pill is drawn on that frame, and a handle that appeared later would
     * mean a strip whose islands move once, a second in. */
    server->stats = stats_create(&server->config.stats);

    // Init physics
    physics_init(&server->physics);
    server_apply_physics_config(server);
    
    
    /* The simulation heartbeat and the video-wallpaper pacer, both owned by
     * server_tick.c along with their callbacks. */
    server_tick_register(server, event_loop);

    // Held-key auto-repeat timer for repeatable binds (armed on demand).
    server->repeat_action = NULL;
    server->repeat_keycode = 0;

    // Must be created after the backend (which may itself connect to an
    // upstream WAYLAND_DISPLAY when nested) so we don't clobber that env var
    // with our own socket before the backend has a chance to read it.
    const char *socket = wl_display_add_socket_auto(server->wl_display);
    if (!socket) {
        wlr_log(WLR_ERROR, "failed to create socket");
        return false;
    }
    wlr_log(WLR_INFO, "Wayland socket: %s", socket);
    setenv("WAYLAND_DISPLAY", socket, 1);

    /* Now that the socket exists and is in our environment, publish it so
     * D-Bus-activated services (portals above all) can reach us. */
    export_session_environment();

    /* Control socket. Named after the Wayland display so several fwm
     * instances (a nested dev run inside a real session) never collide.
     * Failure is non-fatal: fwm works without it, just not scriptably. */
    server->ipc = ipc_create(server, socket);

    server->running = 1;
    return true;
}

void server_run(FwmServer *server) {
    if (!wlr_backend_start(server->wlr_backend)) {
        wlr_log(WLR_ERROR, "failed to start backend");
        return;
    }

    /* After the backend is up (so WAYLAND_DISPLAY is exported and the socket
     * accepts connections) but before the event loop blocks: the clients
     * started here connect into the loop we are about to enter. */
    if (server->debug) {
        /* Neither of the two below, and for the same reason: a -debug run is a
         * second fwm over the first one's HOME, and both of them reach out of
         * this process into the session you are still using. */
        session_debug_desktop(server);
    } else {
        session_restore(server);
        /* After restore, so a helper that reads the window list at startup sees
         * the session it is joining rather than an empty one. */
        session_autostart(server);
    }

    wl_display_run(server->wl_display);
}

/* Detach a listener that may never have been attached: server_init memsets the
 * whole struct, so an unused one still has a zeroed link and wl_list_remove
 * would walk a NULL pointer. */
/* Re-initialised, not merely unlinked: a listener may also be taken off by the
 * global it belongs to, on its way out, and which of the two happens first is
 * not ours to decide (see workspace.c). Leaving the removed links pointing at
 * their old neighbours makes the second removal corrupt the list; an empty list
 * can be removed from as often as anyone likes. */
static void server_remove_listener(struct wl_listener *l) {
    if (!l->link.prev) return;
    wl_list_remove(&l->link);
    wl_list_init(&l->link);
}

void server_destroy(FwmServer *server) {
    /* Before anything else: stop accepting commands that would touch state we
     * are about to free, and take the socket file with us. */
    ipc_destroy(server->ipc);
    server->ipc = NULL;

    /* Order matters: the policy lives in the config, which is freed below. */
    session_clear_on_clean_exit(server);
    session_finish(server);
    launched_finish(server);
    config_free(&server->config);
    
    /* Clean overlays. Clearing each pointer is not tidiness: teardown below
     * takes the clients down, and an unmapping view still calls
     * server_request_tray_redraw(). That guards on a NULL tray_buffer — but a
     * freed pointer is not NULL, so it sailed through the guard and read the
     * released scene buffer. Same reasoning as the IPC handle above. */
    /* The trays and wallpapers belong to the monitors and go with them. */
    if (server->hints_buffer) cairo_overlay_destroy(server->hints_buffer);
    server->hints_buffer = NULL;
    if (server->welcome_buffer) cairo_overlay_destroy(server->welcome_buffer);
    server->welcome_buffer = NULL;
    if (server->errors_buffer) cairo_overlay_destroy(server->errors_buffer);
    server->errors_buffer = NULL;
    server_kill_modes_menu(server);
    server_kill_stats_menu(server);
    /* Before the wallpaper, whose tree the bars hang under — and while the
     * physics world is still alive, so the kinematic row is removed rather than
     * left pointing at freed levels. */
    if (server->cava) cava_destroy(server->cava);
    server->cava = NULL;
    /* Told to stop and let go of, never waited for — the mixer may be parked in
     * a blocking write, and this is the teardown path. */
    if (server->sound) sound_destroy(server->sound);
    server->sound = NULL;
    /* Kills any sensor command still running; nothing would ever read its
     * output again. */
    stats_destroy(server->stats);
    server->stats = NULL;
    screenshot_cleanup(server);
    clipboard_destroy(server->clipboard);
    server->clipboard = NULL;
    launcher_destroy(server->launcher);
    server->launcher = NULL;
    radial_destroy(server->radial);
    server->radial = NULL;
    mixer_destroy(server->mixer);
    server->mixer = NULL;
    osd_destroy(server->osd);
    server->osd = NULL;
    volume_destroy(server->volume);
    server->volume = NULL;
    expo_destroy(server);
    {   /* each monitor's join-slide holds a buffer and a scene node */
        FwmOutput *o;
        wl_list_for_each(o, &server->outputs, link) server_wrap_slide_stop(server, o);
    }

    /* Nothing left to rebuild a renderer for: stop listening before the
     * display tears it down under us. */
    wl_list_remove(&server->renderer_lost.link);

    /* The rotation shaders belong to the renderer's GL context; they have to go
     * while that context still exists. */
    rotate_shutdown(server->wlr_renderer);

    /* The panes under fwm's panels, while the renderer they hold buffers and
     * textures from is still standing. */
    glass_finish();

    /* The one image every window's shadow was cut from. The windows are gone
     * by now, so nothing is left holding a lock on it. */
    shadow_atlas_finish();
    server_settings_finish();

    if (server->video_timer) {
        wl_event_source_remove(server->video_timer);
        server->video_timer = NULL;
    }
    if (server->physics_timer) {
        wl_event_source_remove(server->physics_timer);
    }
    if (server->key_repeat_timer) {
        wl_event_source_remove(server->key_repeat_timer);
    }
    physics_destroy(&server->physics);

    FwmGhost *g, *g_tmp;
    wl_list_for_each_safe(g, g_tmp, &server->ghosts, link) {
        wlr_scene_node_destroy(&g->scene_buffer->node);
        wlr_buffer_unlock(g->buffer);
        wl_list_remove(&g->link);
        free(g);
    }

    // Xwayland must go down before the clients/display it hangs off.
    if (server->xwayland) {
        wl_list_remove(&server->xwl_ready.link);
        wl_list_remove(&server->xwl_new_surface.link);
        wlr_xwayland_destroy(server->xwayland);
        server->xwayland = NULL;
    }

    /* Every wlroots global we listen to asserts on destroy that its signals
     * have no listeners left (xdg_shell was the one that caught us), so all of
     * ours have to come off before wl_display_destroy runs them down.
     *
     * None of this ever ran before: without a signal handler the process was
     * killed outright and server_destroy was dead code on the normal exit
     * path. server_init memsets the struct, so a listener that was never added
     * still has a NULL link and is skipped. */
    server_remove_listener(&server->new_xdg_toplevel);
    server_remove_listener(&server->new_xdg_popup);
    server_remove_listener(&server->new_toplevel_decoration);
    server_remove_listener(&server->new_input);
    server_remove_listener(&server->new_output);
    server_remove_listener(&server->output_layout_change);
    server_remove_listener(&server->cursor_motion);
    server_remove_listener(&server->cursor_motion_absolute);
    server_remove_listener(&server->cursor_button);
    server_remove_listener(&server->cursor_axis);
    server_remove_listener(&server->cursor_frame);
    server_remove_listener(&server->cursor_swipe_begin);
    server_remove_listener(&server->cursor_swipe_update);
    server_remove_listener(&server->cursor_swipe_end);
    server_remove_listener(&server->cursor_pinch_begin);
    server_remove_listener(&server->cursor_pinch_update);
    server_remove_listener(&server->cursor_pinch_end);
    server_remove_listener(&server->cursor_hold_begin);
    server_remove_listener(&server->cursor_hold_end);
    server_remove_listener(&server->request_cursor);
    server_remove_listener(&server->seat_request_set_selection);
    server_remove_listener(&server->seat_request_set_primary_selection);
    server_remove_listener(&server->seat_request_start_drag);
    server_remove_listener(&server->seat_start_drag);
    server_remove_listener(&server->new_pointer_constraint);
    server_remove_listener(&server->constraint_destroy);
    server_remove_listener(&server->output_power_set_mode);
    server_remove_listener(&server->cursor_shape_request);
    server_remove_listener(&server->xdg_activation_request_activate);
    /* Owned by other modules but attached to globals just the same. */
    server_remove_listener(&server->new_layer_surface);
    server_remove_listener(&server->new_lock);
    /* The commit listener has to come off here: the manager asserts on destroy
     * that nothing is still on that signal. The DESTROY listener deliberately
     * stays — it is what tells us the manager and its groups have been freed,
     * and it can only do that if it is still attached when they are. It takes
     * itself off from inside (workspace.c). */
    server_remove_listener(&server->workspace_commit);

    wl_display_destroy_clients(server->wl_display);
    wl_display_destroy(server->wl_display);
}
