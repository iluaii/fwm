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

/* Outputs and the per-frame path: the animation step that runs immediately
 * before the scene is committed, plus output creation and teardown. Split out
 * of server.c; see server_internal.h. */
#include "server.h"
#include "view.h"
#include "physics.h"
#include "bsp.h"
#include "theme.h"
#include "layer.h"
#include "lock.h"
#include "foreign.h"
#include "ipc.h"
#include "session.h"
#include <signal.h>
#include "ui/tray.h"
#include "ui/hints.h"
#include "ui/errors.h"
#include "ui/welcome.h"
#include "ui/launcher.h"
#include "ui/cairo_overlay.h"
#include "wallpaper.h"
#include "group.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
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
#include <wlr/render/color.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
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
#include "server_internal.h"

static int test_action_cb(void *data) {
    FwmServer *server = data;
    if (server->test_action) {
        wlr_log(WLR_INFO, "FWM_TEST_ACTION: %s", server->test_action);
        server_dispatch_action(server, server->test_action);
        free(server->test_action);
        server->test_action = NULL;
    }
    return 0;
}

/* Purely visual animations advance HERE, immediately before the scene is
 * committed — NOT on the physics timer.
 *
 * The timer runs free at 60Hz while the output presents on its own vsync. Two
 * unsynchronised 60Hz clocks beat against each other: some presented frames
 * repeat the previous opacity step and others skip one, so a fade that is
 * perfectly even in the log arrives on screen visibly uneven ("the brightness
 * jumps"). Advancing from measured time at frame time gives every presented
 * frame a correctly timed value.
 *
 * Called once per output frame; a second output in the same instant sees
 * dt ~= 0 and changes nothing. */
/* How far a window rises into place as it opens. */
#define OPEN_RISE_PX 16.0

static void server_animate(FwmServer *server) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!server->last_anim.tv_sec && !server->last_anim.tv_nsec) {
        server->last_anim = now;
        return;
    }
    double dt = (double)(now.tv_sec - server->last_anim.tv_sec)
              + (double)(now.tv_nsec - server->last_anim.tv_nsec) / 1e9;
    if (dt <= 0.0) return;
    /* After a stall (VT switch, a big image decode) do not teleport every
     * animation to its end. */
    if (dt > 0.25) dt = 0.25;
    server->last_anim = now;

    /* Rate-limits itself to one write every few seconds, and only when the set
     * of running applications or their desktops actually changed. */
    session_maybe_save(server);

    server_shake_tick(server, dt);

    /* Squash rides frame time with the other visual ramps. */
    if (server->config.effects.squash > 0.0) {
        FwmView *sv;
        wl_list_for_each(sv, &server->views, link) view_squash_tick(sv, dt);
    }

    // Window open animation. The client's surface is never blended: it is
    // hidden until it has content, then shown fully opaque while a cover rect
    // we draw ourselves fades out over it and the window rises into place.
    if (server->config.decor.fade_in_ms > 0.0) {
        double step = dt * 1000.0 / server->config.decor.fade_in_ms;
        FwmView *fv;
        wl_list_for_each(fv, &server->views, link) {
            if (!fv->open_anim || !fv->scene_tree) continue;

            /* Waiting for the client's first real frame. Capped, so a client
             * that draws once and never commits again still appears. */
            if (fv->open_hold > 0) {
                fv->open_hold_ms += dt * 1000.0;
                if (fv->open_hold_ms < 150.0) continue;
                fv->open_hold = 0;
            }

            /* First frame with content: reveal the window at full opacity and
             * put the cover on top of it. */
            if (!fv->open_cover) {
                const FwmTheme *thm = theme_get();
                float cover[4] = { (float)thm->pill[0], (float)thm->pill[1],
                                   (float)thm->pill[2], 1.0f };
                int cw = fv->width > 0 ? fv->width : 1;
                int ch = fv->height > 0 ? fv->height : 1;
                fv->open_cover = wlr_scene_rect_create(fv->scene_tree, cw, ch, cover);
                if (!fv->open_cover) {   /* no cover: just show the window */
                    fv->open_anim = 0;
                    wlr_scene_node_set_enabled(&fv->scene_tree->node, true);
                    continue;
                }
                wlr_scene_node_raise_to_top(&fv->open_cover->node);
                wlr_scene_node_set_enabled(&fv->scene_tree->node, true);
            }

            fv->open_t += step;
            int done = fv->open_t >= 1.0;
            if (done) fv->open_t = 1.0;

            double t = fv->open_t;
            double e = t * t * (3.0 - 2.0 * t);   /* smoothstep */

            /* Cover fades away; the window rises the last few px into place. */
            const FwmTheme *thm = theme_get();
            float a = (float)(1.0 - e);
            float cover[4] = { (float)thm->pill[0] * a, (float)thm->pill[1] * a,
                               (float)thm->pill[2] * a, a };
            wlr_scene_rect_set_size(fv->open_cover,
                                    fv->width > 0 ? fv->width : 1,
                                    fv->height > 0 ? fv->height : 1);
            wlr_scene_rect_set_color(fv->open_cover, cover);
            wlr_scene_node_set_position(&fv->scene_tree->node,
                                        fv->x - server->camera_x,
                                        fv->y + (int)lround(OPEN_RISE_PX * (1.0 - e)));

            if (done) {
                wlr_scene_node_destroy(&fv->open_cover->node);
                fv->open_cover = NULL;
                fv->open_anim = 0;
                wlr_scene_node_set_position(&fv->scene_tree->node,
                                            fv->x - server->camera_x, fv->y);
            }
        }

        // Window fade-out: ghost snapshots of closed windows ramp 1 -> 0
        // over the same duration, then die (node destroyed, buffer released).
        FwmGhost *g, *g_tmp;
        wl_list_for_each_safe(g, g_tmp, &server->ghosts, link) {
            g->t += step;
            if (g->t >= 1.0) {
                wlr_scene_node_destroy(&g->scene_buffer->node);
                wlr_buffer_unlock(g->buffer);
                wl_list_remove(&g->link);
                free(g);
                continue;
            }
            // Mirror of the fade-in, so closing feels like the reverse of
            // opening rather than a different effect.
            double gt = g->t;
            double o = 1.0 - gt * gt * (3.0 - 2.0 * gt);
            wlr_scene_buffer_set_opacity(g->scene_buffer, (float)o);
            wlr_scene_node_set_position(&g->scene_buffer->node,
                                        (int)lround(g->x) - server->camera_x, (int)lround(g->y));
        }
    }

    // Panel appear animations (hints, errors, welcome).
    cairo_overlay_tick(dt);

    // Wallpaper cross-fade: drop the outgoing set once the new one is opaque.
    if (server->wallpaper_prev && wallpaper_fade_tick(server->wallpaper, dt)) {
        wallpaper_destroy(server->wallpaper_prev);
        server->wallpaper_prev = NULL;
    }
}

static void handle_output_frame(struct wl_listener *listener, void *data) {
    struct FwmOutput *output = wl_container_of(listener, output, frame);
    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(output->server->scene, output->wlr_output);
    server_animate(output->server);
    wlr_scene_output_commit(scene_output, NULL);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
    struct FwmOutput *output = wl_container_of(listener, output, destroy);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

static void handle_new_output(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;
    
    wlr_output_init_render(wlr_output, server->wlr_allocator, server->wlr_renderer);
    
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode) {
        wlr_output_state_set_mode(&state, mode);
    }
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);
    
    struct FwmOutput *output = calloc(1, sizeof(struct FwmOutput));
    output->server = server;
    output->wlr_output = wlr_output;

    /* fwm is single-output: the first one to arrive sizes the whole world (see
     * below), and any further one renders that same world rather than getting
     * a desktop strip of its own. Say so, or a second monitor looks like a
     * rendering bug instead of a missing feature. */
    wlr_log(WLR_INFO, "output %s: %dx%d%s", wlr_output->name,
            wlr_output->width, wlr_output->height,
            server->screen_width == 0 ? " (sizes the world)"
                                      : " — EXTRA OUTPUT, fwm is single-output");
    output->frame.notify = handle_output_frame;
    output->destroy.notify = handle_output_destroy;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);
    wl_list_insert(&server->outputs, &output->link);
    
    struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server->output_layout, wlr_output);
    struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
    
    if (server->screen_width == 0) {
        server->screen_width = wlr_output->width;
        server->screen_height = wlr_output->height;

        // Parallax wallpaper (below the window layer)
        server->wallpaper = wallpaper_create(server->layer_background, &server->config,
                                             server->screen_width, server->screen_height);

        // Initialize UI panels
        server->tray_buffer = tray_init(server->layer_overlay, server->screen_width);
        server_request_tray_redraw(server);

        server->welcome_buffer = welcome_show(server->layer_overlay, server->screen_width, server->screen_height, &server->config);

        // A config so broken that the built-in binds had to stand in is worth
        // explaining up front — the user's own keys will not work. Lesser
        // problems only light up the tray pill.
        if (server->config.fallback_binds && server->config.error_count > 0) {
            server->errors_buffer = errors_show(server->layer_overlay, server->screen_width,
                                                server->screen_height, &server->config);
            server_request_tray_redraw(server);
        }

        // Debug: FWM_TEST_CAMERA=N parks the camera on desktop N at startup.
        // Wallpaper panning and the desktop slide are otherwise only reachable
        // through keybinds, which a nested test run cannot press.
        const char *tc = getenv("FWM_TEST_CAMERA");
        if (tc) {
            int d = atoi(tc);
            if (d < 0) d = 0;
            if (d > 9) d = 9;
            server->camera_x = d * server->screen_width;
            server->target_camera_x = server->camera_x;
            if (server->wallpaper) wallpaper_update(server->wallpaper, server->camera_x);
        }

        // Debug: FWM_TEST_ACTION=<action> dispatches one action a second after
        // startup. Key injection into a nested run is impossible, so this is
        // the only way to exercise a bind's code path in a test.
        const char *ta = getenv("FWM_TEST_ACTION");
        if (ta) {
            struct wl_event_loop *el = wl_display_get_event_loop(server->wl_display);
            server->test_action = strdup(ta);
            server->test_action_timer =
                wl_event_loop_add_timer(el, test_action_cb, server);
            if (server->test_action_timer) {
                wl_event_source_timer_update(server->test_action_timer, 1500);
            }
        }

        // Debug: FWM_TEST_GRAVITY=<scale> starts with gravity on. fwm boots in
        // zero-g and gravity is only reachable through cycle_gravity, so a
        // nested run can otherwise never make a window fall — which means the
        // impact effects (shake, squash) cannot be exercised at all.
        const char *tg = getenv("FWM_TEST_GRAVITY");
        if (tg) {
            double g = atof(tg);
            if (g < 0.0) g = 0.0;
            if (g > 2.0) g = 2.0;
            server->physics.gravity_scale = g;
        }

        // Debug: FWM_OPEN_PICKER=1 opens the wallpaper picker at startup —
        // same purpose as FWM_SHOW_HINTS, since a nested run has no way to
        // press the bind.
        if (getenv("FWM_OPEN_PICKER")) {
            launcher_toggle_wallpapers(server->launcher);
        }

        // Debug: FWM_SHOW_HINTS=1 opens the bind help at startup, so the
        // overlay can be checked in a nested run without a keyboard.
        if (getenv("FWM_SHOW_HINTS") && !server->hints_buffer) {
            server->hints_buffer = hints_show(server->layer_overlay, server->screen_width, server->screen_height, &server->config);
        }
    }
}

/* swayidle and friends turning the display off. Without this nothing can blank
 * the screen: fwm has no idle blanking of its own and the physics tick keeps
 * scheduling frames forever, so the monitor would stay lit indefinitely. */
static void handle_output_power_set_mode(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, output_power_set_mode);
    (void)server;
    const struct wlr_output_power_v1_set_mode_event *event = data;
    if (!event->output || !event->output->allocator) return;

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
    wlr_output_commit_state(event->output, &state);
    wlr_output_state_finish(&state);
}


/* Called once from server_init(). */
void server_output_register(FwmServer *server) {
    server->new_output.notify = handle_new_output;
    wl_signal_add(&server->wlr_backend->events.new_output, &server->new_output);

    if (server->output_power) {
        server->output_power_set_mode.notify = handle_output_power_set_mode;
        wl_signal_add(&server->output_power->events.set_mode, &server->output_power_set_mode);
    }
}
