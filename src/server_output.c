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
#include "workspace.h"
#include "ipc.h"
#include "session.h"
#include <signal.h>
#include "ui/tray.h"
#include "ui/stats_menu.h"
#include "ui/hints.h"
#include "ui/errors.h"
#include "ui/welcome.h"
#include "ui/launcher.h"
#include "ui/cairo_overlay.h"
#include "wallpaper.h"
#include "expo.h"
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

    /* ── effect frame diagnostics (FWM_DEBUG_EFFECTS) ──────────────────────
     * Counted here because this is the one place that knows how much real time
     * passed between the frames a viewer actually saw. Nothing below runs
     * unless the variable was set at startup. */
    if (server->fx_debug) {
        bool busy = false;
        FwmView *fv;
        wl_list_for_each(fv, &server->views, link) {
            if (view_is_spinning(fv) || fv->jelly) { busy = true; break; }
        }
        if (busy) {
            if (!server->fx_frames) {
                server->fx_since = now;
                server->fx_dt_min = 1e9;
                server->fx_dt_max = 0.0;
                server->fx_omega_jump = 0.0;
                server->fx_omega_have = 0;
            }
            server->fx_frames++;
            if (dt < server->fx_dt_min) server->fx_dt_min = dt;
            if (dt > server->fx_dt_max) server->fx_dt_max = dt;

            double span = (double)(now.tv_sec - server->fx_since.tv_sec)
                        + (double)(now.tv_nsec - server->fx_since.tv_nsec) / 1e9;
            if (span >= 1.0) {
                wlr_log(WLR_INFO,
                        "effects: %d frames in %.2fs (%.1f fps), dt %.1f..%.1f ms, "
                        "redraws %d, worst speed jump %.0f%%, "
                        "composited snaps %d (%.2f ms each), live=%.0f",
                        server->fx_frames, span, server->fx_frames / span,
                        server->fx_dt_min * 1000.0, server->fx_dt_max * 1000.0,
                        server->fx_moved, server->fx_omega_jump * 100.0,
                        server->fx_snaps,
                        server->fx_snaps ? server->fx_snap_us / server->fx_snaps / 1000.0 : 0.0,
                        server->config.effects.live);
                server->fx_frames = server->fx_snaps = server->fx_moved = 0;
                server->fx_snap_us = 0.0;
            }
        } else if (server->fx_frames) {
            server->fx_frames = server->fx_snaps = server->fx_moved = 0;
        }
    }

    /* Rate-limits itself to one write every few seconds, and only when the set
     * of running applications or their desktops actually changed. */
    session_maybe_save(server);

    /* A window being dragged by a corner while it spins is placed HERE as well
     * as on the physics tick: the hand moves it, and the hand is not on the
     * timer's clock. See server_drag_swing_place. */
    server_drag_swing_place(server);

    server_shake_tick(server, dt);

    /* The desktop strip rides frame time like every other visual ramp; while
     * it is up it is the only thing on screen that moves. */
    expo_tick(server, dt);

    /* Squash rides frame time with the other visual ramps. */
    if (server->config.effects.squash > 0.0) {
        FwmView *sv;
        wl_list_for_each(sv, &server->views, link) view_squash_tick(sv, dt);
    }

    /* The drag wobble ticks unconditionally, not under an `if (jelly > 0)` like
     * the squash above: turning the effect off in a config reload has to reach
     * a window that is wobbling right now, and view_jelly_tick is what puts its
     * live content back. Views without one return immediately. */
    {
        FwmView *jv;
        wl_list_for_each(jv, &server->views, link) {
            view_jelly_tick(jv, server->config.effects.jelly, dt);
        }
    }

    /* Free rotation. Also a frame-time ramp, and for a stronger reason than
     * the others: this one repaints a snapshot every frame, so running it on
     * the physics timer would render angles nobody ever sees. The body is the
     * authority — the view only shows the angle Box2D integrated. */
    {
        FwmView *rv;
        wl_list_for_each(rv, &server->views, link) {
            PhysicsBody *rb = physics_find_body(&server->physics, rv->id);
            if (rb && rb->spin && server_can_spin(rb) && server->config.effects.spin > 0.0) {
                view_spin_tick(rv, server_render_angle(server, rb), dt);
            } else {
                /* Also the path that lands a window turned off mid-spin by a
                 * config reload: the body stops rotating and the picture goes
                 * back to being the live window — as does one that was tiled
                 * or made fullscreen while it turned. */
                if (rb && rb->spin) physics_unspin_body(&server->physics, rv->id);
                if (view_is_spinning(rv)) view_stop_spin(rv);
            }
        }
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
            server_place_node(server, &fv->scene_tree->node, fv->x,
                              fv->y + OPEN_RISE_PX * (1.0 - e));

            if (done) {
                wlr_scene_node_destroy(&fv->open_cover->node);
                fv->open_cover = NULL;
                fv->open_anim = 0;
                server_place_node(server, &fv->scene_tree->node, fv->x, fv->y);
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
            double gx = g->x, gy = g->y;

            if (g->kind == GHOST_IMPLODE && g->w > 0 && g->h > 0) {
                // Cubic rather than the smoothstep above: a collapse should
                // leave quickly and finish slowly, so the eye catches which
                // window went. Smoothstep eases INTO the motion, which on a
                // shrink reads as a pause before anything happens.
                double k = 1.0 - gt;
                double s = k * k * k;
                int w = (int)(g->w * s);
                int h = (int)(g->h * s);
                // A zero-sized dest_size is not a scene buffer wlroots will
                // draw sensibly, and the last frame of the collapse is a point
                // nobody can see anyway.
                if (w < 1 || h < 1) {
                    wlr_scene_node_set_enabled(&g->scene_buffer->node, false);
                } else {
                    wlr_scene_buffer_set_dest_size(g->scene_buffer, w, h);
                    // Shrink about the centre: dest_size grows from the
                    // top-left, so the origin has to walk inward by half of
                    // what was lost or the window slides up-left as it dies.
                    g->draw_dx = (g->w - w) / 2.0;
                    g->draw_dy = (g->h - h) / 2.0;
                    gx += g->draw_dx;
                    gy += g->draw_dy;
                }
                // Held opaque until the very end. Fading a collapse as well
                // makes it vanish before it has visibly shrunk, and then the
                // two deaths look the same again.
                o = 1.0 - gt * gt * gt * gt;
            }

            wlr_scene_buffer_set_opacity(g->scene_buffer, (float)o);
            server_place_node(server, &g->scene_buffer->node, gx, gy);
        }
    }

    // Panel appear animations (hints, errors, welcome).
    cairo_overlay_tick(dt);

    // Video wallpaper: upload the next decoded frame if the fps cap allows. The
    // outgoing set is presented too so a video->video swap keeps moving under
    // the fade instead of freezing on its last frame.
    FwmOutput *wo;
    wl_list_for_each(wo, &server->outputs, link) {
        wallpaper_present(wo->wallpaper);
        wallpaper_present(wo->wallpaper_prev);

        // Wallpaper cross-fade: drop the outgoing set once the new one is opaque.
        if (wo->wallpaper_prev && wallpaper_fade_tick(wo->wallpaper, dt)) {
            wallpaper_destroy(wo->wallpaper_prev);
            wo->wallpaper_prev = NULL;
            /* The fade is over and nothing is animating on the next frame
             * anyway, so this is the cheapest moment in the whole swap to give
             * the heap back — the outgoing set was only just released. */
            server_reclaim_memory();
        }
    }
}

static void handle_output_frame(struct wl_listener *listener, void *data) {
    FwmOutput *output = wl_container_of(listener, output, frame);
    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(output->server->scene, output->wlr_output);
    /* A monitor turned off loses its scene output with its layout output, and a
     * frame event can still be in flight when it does. */
    if (!scene_output) return;
    server_animate(output->server);
    wlr_scene_output_commit(scene_output, NULL);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
    FwmOutput *output = wl_container_of(listener, output, destroy);
    /* Bars on this monitor go with it; they hold a pointer to it. */
    layer_output_gone(output->server, output->wlr_output);
    /* And the panels are told the windows left this screen while it still
     * exists to name — after this the pointer they cache is freed. */
    foreign_output_gone(output->server, output->wlr_output);
    /* This monitor's ext-workspace group is this monitor: it goes too, and the
     * desktop it was holding becomes one nobody is showing. */
    workspace_output_gone(output->server, output);
    /* So does the desktop strip, for the same reason: it is drawn on one
     * monitor and reads that monitor's camera and box every tick. Only a
     * change of SIZE tore it down before, so unplugging a second screen of
     * the same size as the first left it reading freed memory. */
    expo_output_gone(output->server, output);
    /* So do the things that were built for its size. The desktop it was showing
     * is simply free again: its windows park off-layout until some monitor
     * takes that desktop. */
    server_wrap_slide_stop(output->server, output);
    /* A drag in progress remembers which monitor's camera it is following, and
     * compares that pointer every tick. Forget it here rather than leave it
     * dangling onto memory the next monitor may be handed. */
    if (output->server->interactive.cam_output == output) {
        output->server->interactive.cam_output = NULL;
        output->server->interactive.cam_have = 0;
    }
    if (output->wallpaper_prev) wallpaper_destroy(output->wallpaper_prev);
    if (output->wallpaper) wallpaper_destroy(output->wallpaper);
    if (output->tray_buffer) cairo_overlay_destroy(output->tray_buffer);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
    /* The layout fires its own change event for the rest (desktops, cameras). */
}

/* ── one world, one window onto it per monitor ────────────────────────────
 *
 * The world is a horizontal strip of FWM_DESKTOPS columns, each the size of the
 * primary monitor (screen_width/height). Each monitor shows ONE column, the one
 * its `desktop` names, through its own camera. Two monitors are two independent
 * windows onto the same strip: they show different desktops, switch desktops
 * separately, and carry their own wallpaper and status strip.
 *
 * A desktop is never on two screens at once — asking for one that is already
 * up elsewhere trades the two (server_output_show_desktop).
 *
 * Windows keep WORLD coordinates. What turns those into pixels is
 * server_place_node, and it is the only place that knows about monitors: a
 * window whose desktop nobody is showing is parked far off the layout, where
 * no monitor covers it and nothing draws it. */

/* Far enough outside any sane layout that no monitor can ever cover it. Windows
 * on unshown desktops are parked here instead of being disabled, so this path
 * cannot fight with the enabled flag that the open animation, the desktop strip
 * and the lock screen each use for their own reasons. */
#define PARKED_X (-1000000)

static void server_output_first_layout(FwmServer *server);

FwmOutput *server_primary_output(FwmServer *server) {
    FwmOutput *o, *any = NULL;
    wl_list_for_each(o, &server->outputs, link) {
        if (!o->enabled || o->box.width <= 0) continue;
        if (o->box.x == 0 && o->box.y == 0) return o;
        if (!any) any = o;
    }
    return any;
}

FwmOutput *server_output_at(FwmServer *server, double lx, double ly) {
    FwmOutput *o;
    wl_list_for_each(o, &server->outputs, link) {
        if (!o->enabled || o->box.width <= 0 || o->box.height <= 0) continue;
        if (lx >= o->box.x && lx < o->box.x + o->box.width
         && ly >= o->box.y && ly < o->box.y + o->box.height) return o;
    }
    return NULL;
}

FwmOutput *server_active_output(FwmServer *server) {
    /* Debug: FWM_TEST_OUTPUT=<name> pins "the monitor the user is at". A nested
     * run cannot move the pointer onto the second screen, so without this there
     * is no way to exercise anything that opens on the active monitor. */
    const char *pin = getenv("FWM_TEST_OUTPUT");
    if (pin) {
        FwmOutput *p;
        wl_list_for_each(p, &server->outputs, link) {
            if (strcmp(p->wlr_output->name, pin) == 0) return p;
        }
    }
    FwmOutput *o = server->cursor
                 ? server_output_at(server, server->cursor->x, server->cursor->y)
                 : NULL;
    return o ? o : server_primary_output(server);
}

FwmOutput *server_output_showing(FwmServer *server, int d) {
    FwmOutput *o;
    wl_list_for_each(o, &server->outputs, link) {
        if (o->enabled && o->desktop == d) return o;
    }
    return NULL;
}

int server_active_desktop(FwmServer *server) {
    FwmOutput *o = server_active_output(server);
    return o ? o->desktop : 0;
}

int server_desktop_at_x(FwmServer *server, double wx) {
    if (server->screen_width <= 0) return 0;
    int d = (int)(wx / server->screen_width);
    if (d < 0) d = 0;
    if (d >= FWM_DESKTOPS) d = FWM_DESKTOPS - 1;
    return d;
}

/* Is there room around this monitor for a window to hang off its edge without
 * landing on another one? Nothing clips the scene at a monitor's border, so a
 * window drawn half outside a screen would be drawn on whatever screen the
 * layout has next to it. */
static bool output_has_elbow_room(FwmServer *server, FwmOutput *o) {
    int reach = server->screen_width;
    FwmOutput *p;
    wl_list_for_each(p, &server->outputs, link) {
        if (p == o || !p->enabled || p->box.width <= 0) continue;
        if (p->box.x + p->box.width > o->box.x - reach
         && p->box.x < o->box.x + o->box.width + reach) return false;
    }
    return true;
}

/* Which monitor draws a window whose desktop no monitor owns.
 *
 * `desktop` names where a monitor is HEADED — it is set the moment the switch
 * is asked for, while camera_x takes the whole slide to get there. So for those
 * few hundred milliseconds the camera is still standing over the desktop being
 * left, whose windows now belong to nobody: parking them there made them blink
 * out at the start of every switch instead of sliding away, and the same for a
 * held pan, which crosses into the next desktop at its halfway point.
 *
 * Only the columns the camera actually overlaps count — one screen to the left
 * is the furthest a window can start and still have any part of it on screen.
 * Anything beyond that is genuinely somewhere else and stays parked. */
static FwmOutput *output_passing_over(FwmServer *server, double wx) {
    FwmOutput *o;
    wl_list_for_each(o, &server->outputs, link) {
        if (!o->enabled || o->hide_world || o->box.width <= 0) continue;
        if (wx <= o->camera_x - server->screen_width) continue;
        if (wx >= o->camera_x + o->box.width) continue;
        if (!output_has_elbow_room(server, o)) continue;
        return o;
    }
    return NULL;
}

bool server_world_to_screen(FwmServer *server, double wx, double wy,
                            double *sx, double *sy) {
    FwmOutput *o = server_output_showing(server, server_desktop_at_x(server, wx));
    if (o && o->hide_world) return false;
    if (!o) o = output_passing_over(server, wx);
    if (!o) return false;
    if (sx) *sx = wx - o->camera_x + o->box.x + o->render_dx;
    if (sy) *sy = wy + o->box.y + o->render_dy;
    return true;
}

bool server_screen_to_world(FwmServer *server, double lx, double ly,
                            double *wx, double *wy) {
    FwmOutput *o = server_output_at(server, lx, ly);
    if (!o) return false;
    if (wx) *wx = lx - o->box.x - o->render_dx + o->camera_x;
    if (wy) *wy = ly - o->box.y - o->render_dy;
    return true;
}

void server_cursor_world(FwmServer *server, double *wx, double *wy) {
    double cx = server->cursor ? server->cursor->x : 0;
    double cy = server->cursor ? server->cursor->y : 0;
    if (server_screen_to_world(server, cx, cy, wx, wy)) return;
    /* Off every monitor (a cursor warped mid-unplug): answer as if it were on
     * the active one, which is where the next click would land anyway. */
    FwmOutput *o = server_active_output(server);
    if (wx) *wx = cx - (o ? o->box.x : 0) + (o ? o->camera_x : 0);
    if (wy) *wy = cy - (o ? o->box.y : 0);
}

/* A centred panel is built as if the screen started at 0,0, so this moves one
 * onto the monitor the user is actually at. Panels do not belong to a desktop
 * — they are not placed by server_place_node — so they are shifted once, when
 * they open. */
void server_panel_to_active_output(FwmServer *server, struct wlr_scene_buffer *panel) {
    if (!panel) return;
    FwmOutput *o = server_active_output(server);
    if (!o) return;
    wlr_scene_node_set_position(&panel->node,
                                panel->node.x + o->box.x, panel->node.y + o->box.y);
}

void server_place_node(FwmServer *server, struct wlr_scene_node *node,
                       double wx, double wy) {
    if (!node) return;
    double sx, sy;
    if (!server_world_to_screen(server, wx, wy, &sx, &sy)) {
        wlr_scene_node_set_position(node, PARKED_X, (int)lround(wy));
        return;
    }
    wlr_scene_node_set_position(node, (int)lround(sx), (int)lround(sy));
}

void server_output_show_desktop(FwmServer *server, FwmOutput *out, int d, int seam) {
    if (!out || d < 0 || d >= FWM_DESKTOPS || server->screen_width <= 0) return;
    if (out->desktop == d) return;

    /* Already up on the other screen: the two trade, which is the only answer
     * that keeps "one desktop, one monitor" true and never leaves a monitor
     * showing nothing. */
    FwmOutput *other = server_output_showing(server, d);
    if (other) {
        int back = out->desktop;
        other->desktop = back;
        /* Only the target moves: the monitor being traded with slides to its
         * new desktop exactly like the one that asked. Writing camera_x here
         * as well teleported it, so half of a swap cut and half of it slid. */
        other->target_camera_x = back * server->screen_width;
        other->cam_free = 0;
    }

    out->desktop = d;
    out->target_camera_x = d * server->screen_width;
    if (seam) {
        /* The join is never drawn: jump it, and let the slide animation tell
         * the story instead. */
        out->camera_x = out->target_camera_x;
        out->cam_anim = 0;
    }
    server_views_place(server);
    server_request_tray_redraw(server);
}

/* Every window (and every ghost) put back where it belongs on screen. Called
 * whenever a monitor changes which desktop it shows, or the layout changes
 * under them. */
void server_views_place(FwmServer *server) {
    FwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->scene_tree) server_place_node(server, &v->scene_tree->node, v->x, v->y);
    }
    FwmGhost *g;
    wl_list_for_each(g, &server->ghosts, link) {
        if (g->scene_buffer)
            server_place_node(server, &g->scene_buffer->node,
                              g->x + g->draw_dx, g->y + g->draw_dy);
    }
}

/* A world whose columns just changed size keeps every window where it was ON
 * ITS DESKTOP: desktops are one screen apart, so without this a hotplug that
 * changes the primary monitor's size would deal the windows out to other
 * desktops. */
static void world_reflow(FwmServer *server, int old_w, int old_h) {
    if (old_w <= 0) return;
    int new_w = server->screen_width, new_h = server->screen_height;
    if (new_w == old_w && new_h == old_h) return;

    FwmView *v;
    wl_list_for_each(v, &server->views, link) {
        PhysicsBody *b = physics_find_body(&server->physics, v->id);
        int d = b ? b->desktop_id : (int)(v->x / old_w);
        if (d < 0) d = 0;
        if (d >= FWM_DESKTOPS) d = FWM_DESKTOPS - 1;

        double lx = (b ? b->x : (double)v->x) - (double)d * old_w;
        double ly = b ? b->y : (double)v->y;
        /* Order matters: a window wider than the new screen clamps to the left
         * edge rather than off the right one. */
        if (lx > new_w - v->width) lx = new_w - v->width;
        if (lx < 0) lx = 0;
        if (ly > new_h - v->height) ly = new_h - v->height;
        if (ly < 0) ly = 0;

        if (b) {
            b->x = (double)d * new_w + lx;
            b->y = ly;
            v->x = (int)lround(b->x);
            v->y = (int)lround(b->y);
        } else {
            v->x = d * new_w + (int)lround(lx);
            v->y = (int)lround(ly);
        }
        view_sync_position(v);
    }
}

/* This monitor's wallpaper, built for its size and panned by its camera. */
static void output_build_wallpaper(FwmOutput *out) {
    FwmServer *server = out->server;
    if (out->wallpaper_prev) {
        wallpaper_destroy(out->wallpaper_prev);
        out->wallpaper_prev = NULL;
    }
    if (out->wallpaper) {
        wallpaper_destroy(out->wallpaper);
        out->wallpaper = NULL;
    }
    if (server->config.wallpaper_count <= 0) return;
    out->wallpaper = wallpaper_create(server->layer_background, &server->config,
                                      out->box.width, out->box.height);
    if (!out->wallpaper) return;
    /* Its own strip of the scene, moved onto this monitor. The layers are laid
     * out from 0,0 like every other screen-sized thing. */
    wallpaper_set_origin(out->wallpaper, out->box.x, out->box.y);
    wallpaper_update(out->wallpaper, out->camera_x);
}

/* This monitor's status strip, at the top of it. */
static void output_build_tray(FwmOutput *out) {
    FwmServer *server = out->server;
    if (out->tray_buffer) {
        cairo_overlay_destroy(out->tray_buffer);
        out->tray_buffer = NULL;
    }
    /* A fresh buffer has nothing in it, so the record of what was last drawn
     * has to go with the old one. Kept, it says "this strip already shows
     * exactly that" and tray_redraw skips the paint — the new buffer then stays
     * blank until something in it happens to change, which for an idle strip
     * means until the clock rolls over to the next minute. Every rebuild hits
     * this: a second monitor arriving, a mode change, a screen coming back. */
    out->tray_strip = (TrayStrip){0};
    out->tray_buffer = tray_init(server->layer_overlay, out->box.width);
    if (!out->tray_buffer) return;
    wlr_scene_node_set_position(&out->tray_buffer->node,
                                out->tray_buffer->node.x + out->box.x,
                                out->tray_buffer->node.y + out->box.y);
    if (server->tray_hidden)
        wlr_scene_node_set_enabled(&out->tray_buffer->node, false);
}

/* Everything sized to a monitor, rebuilt. Called for the first monitor and on
 * every later layout change (hotplug, unplug, mode change) — one path, so a
 * monitor arriving an hour in lands in exactly the state one present at startup
 * would have. */
static void server_output_layout_update(FwmServer *server) {
    /* Fresh boxes first: everything below reads them. */
    FwmOutput *o;
    wl_list_for_each(o, &server->outputs, link) {
        wlr_output_layout_get_box(server->output_layout, o->wlr_output, &o->box);
    }

    FwmOutput *primary = server_primary_output(server);
    /* The last monitor going away leaves nothing to size a desktop against.
     * Keep the world as it was: its windows are still alive, and a size of zero
     * would divide by zero in half the compositor. */
    if (!primary || primary->box.width <= 0 || primary->box.height <= 0) return;

    int first = server->screen_width == 0;
    int old_w = server->screen_width, old_h = server->screen_height;
    int resized = primary->box.width != old_w || primary->box.height != old_h;

    server->screen_width = primary->box.width;
    server->screen_height = primary->box.height;

    if (first) {
        const ConfigOutput *pc = config_find_output(&server->config,
                                                    primary->wlr_output->name);
        primary->desktop = (pc && pc->desktop >= 0) ? pc->desktop : 0;
        wlr_log(WLR_INFO, "desktop is %dx%d (from %s)", server->screen_width,
                server->screen_height, primary->wlr_output->name);
    }

    if (resized && !first) {
        wlr_log(WLR_INFO, "desktop is now %dx%d", server->screen_width, server->screen_height);
        /* The strip is a frozen photograph of a world that no longer has that
         * shape, and re-photographing every desktop costs more than dropping
         * it. */
        expo_destroy(server);
        world_reflow(server, old_w, old_h);
    }

    /* A monitor that just arrived takes the lowest desktop nobody else has, so
     * plugging one in gives you a fresh screen rather than a copy of one you
     * are already looking at. One that is showing a desktop it lost to another
     * monitor is moved along the same way. */
    wl_list_for_each(o, &server->outputs, link) {
        if (!o->enabled) { o->desktop = -1; continue; }
        FwmOutput *clash = server_output_showing(server, o->desktop);
        if (o->desktop >= 0 && o->desktop < FWM_DESKTOPS && clash == o) continue;

        /* What [[output]] asked for, if nobody else has it. */
        const ConfigOutput *oc = config_find_output(&server->config, o->wlr_output->name);
        if (oc && oc->desktop >= 0 && !server_output_showing(server, oc->desktop)) {
            o->desktop = oc->desktop;
            continue;
        }
        for (int d = 0; d < FWM_DESKTOPS; d++) {
            if (!server_output_showing(server, d)) { o->desktop = d; break; }
        }
    }

    wl_list_for_each(o, &server->outputs, link) {
        if (!o->enabled) continue;
        o->camera_x = o->target_camera_x = o->desktop * server->screen_width;
        o->cam_anim = 0;
        output_build_wallpaper(o);
        output_build_tray(o);
    }

    server_views_place(server);

    /* The bars are built for a screen's width. -1 is "no mode applied", which
     * forces the rebuild even though the configured mode did not change. */
    server->cava_applied = -1;
    server_cava_sync(server);

    server_request_tray_redraw(server);

    /* Bars and docks get their screen; tiled desktops get the new splits. */
    layer_arrange(server);
    /* And the lock screen, if one is up: it is sized to a monitor exactly as a
     * bar is, and a monitor that changed shape under it must not be left
     * showing anything but the locker. */
    lock_arrange(server);
    if (resized) {
        for (int d = 0; d < FWM_DESKTOPS; d++) {
            if (server->desktop_mode[d] == DESKTOP_MODE_TILING) server_apply_tiling(server, d);
        }
    }

    /* The panels and the debug hooks that exist once, now that there is a
     * screen to size them against. */
    if (first) server_output_first_layout(server);

    server_reclaim_memory();
}

static void handle_output_layout_change(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, output_layout_change);
    server_output_layout_update(server);
}

/* Defined with the rest of the mode handling below; a screen coming back on
 * needs it, and that path is above it. */
static void output_apply_mode_config(FwmServer *server, FwmOutput *out);

/* ── turning a monitor off and on ─────────────────────────────────────────
 *
 * Three things have to move together, and every path below goes through these
 * two helpers so they cannot drift apart:
 *
 *   the OUTPUT itself   — the panel's backlight, via a commit;
 *   the LAYOUT          — a dark monitor must not be in it, or it still holds a
 *                         desktop and windows can be placed on a screen nobody
 *                         can see;
 *   the SCENE OUTPUT    — destroyed with the layout output, so re-lighting a
 *                         monitor has to build a new one or it stays black
 *                         while claiming to be on.
 *
 * Everything sized to the monitor (wallpaper, status strip) goes too: it is
 * rebuilt from the layout-change event when the screen comes back, and left in
 * place it would sit at coordinates the remaining monitors have since moved
 * into. */
static void output_leave_layout(FwmServer *server, FwmOutput *out) {
    /* Bars and docks anchored to this screen go with it — the same thing that
     * happens when a monitor is unplugged, and for the same reason: they hold a
     * pointer to it and are laid out against a box that is about to be zero. */
    layer_output_gone(server, out->wlr_output);
    server_wrap_slide_stop(server, out);
    if (out->wallpaper_prev) {
        wallpaper_destroy(out->wallpaper_prev);
        out->wallpaper_prev = NULL;
    }
    if (out->wallpaper) {
        wallpaper_destroy(out->wallpaper);
        out->wallpaper = NULL;
    }
    if (out->tray_buffer) {
        cairo_overlay_destroy(out->tray_buffer);
        out->tray_buffer = NULL;
        out->tray_strip = (TrayStrip){0};
    }
    wlr_output_layout_remove(server->output_layout, out->wlr_output);
    out->box = (struct wlr_box){0};
    out->desktop = -1;
}

static void output_join_layout(FwmServer *server, FwmOutput *out) {
    const ConfigOutput *cfg = config_find_output(&server->config, out->wlr_output->name);
    if (cfg && cfg->have_pos)
        wlr_output_layout_add(server->output_layout, out->wlr_output, cfg->x, cfg->y);
    else if (out->manual_pos)
        wlr_output_layout_add(server->output_layout, out->wlr_output,
                              out->manual_x, out->manual_y);
    else
        wlr_output_layout_add_auto(server->output_layout, out->wlr_output);

    /* A monitor that has never been in the layout, or that left it while it was
     * dark, has no scene output — nothing would ever be composited onto it. */
    if (!wlr_scene_get_scene_output(server->scene, out->wlr_output)) {
        struct wlr_output_layout_output *l_output =
            wlr_output_layout_get(server->output_layout, out->wlr_output);
        struct wlr_scene_output *scene_output =
            wlr_scene_output_create(server->scene, out->wlr_output);
        if (l_output && scene_output)
            wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
    }
}

/* The cursor is standing on a screen that just went dark, so it is nowhere.
 * Put it in the middle of a monitor that still exists — a pointer off every
 * output means the next click and every "which monitor am I at" question fall
 * back to guesses. */
static void cursor_to_output(FwmServer *server, FwmOutput *out) {
    if (!server->cursor || !out || out->box.width <= 0) return;
    wlr_cursor_warp(server->cursor, NULL,
                    out->box.x + out->box.width / 2.0,
                    out->box.y + out->box.height / 2.0);
}

/* Light one monitor or put it out, with nothing standing in the way. Returns 1
 * if anything changed. */
static int output_set_enabled(FwmServer *server, FwmOutput *out, int on) {
    if (!out || out->enabled == !!on) return 0;

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, on);
    if (on) {
        struct wlr_output_mode *mode = wlr_output_preferred_mode(out->wlr_output);
        if (mode) wlr_output_state_set_mode(&state, mode);
    }
    wlr_output_commit_state(out->wlr_output, &state);
    wlr_output_state_finish(&state);
    out->enabled = on;

    if (on) {
        /* Lit with the preferred mode above, which is only a starting point:
         * a screen coming back has to come back the shape the file asked for,
         * or the lid closing and opening would silently undo it. */
        output_apply_mode_config(server, out);
        output_join_layout(server, out);
    } else {
        /* Anchored to the strip on a screen that is going dark. */
        if (server->modes_buffer) server_kill_modes_menu(server);
        if (server->stats_buffer) server_kill_stats_menu(server);
        output_leave_layout(server, out);
    }
    wlr_log(WLR_INFO, "output %s: %s", out->wlr_output->name, on ? "on" : "off");

    /* Both branches above fired the layout's change event, which rebuilds the
     * world; all that is left is the pointer, which may be standing on a screen
     * that no longer exists. */
    if (!on) cursor_to_output(server, server_primary_output(server));
    return 1;
}

/* The same thing asked for by a person: a keybind, `fwmctl dispatch`, or the
 * lid coming down. Refuses to put out the last lit screen — a session with no
 * output has nowhere left to show the bind that would bring one back, and the
 * config's own `enabled = false` is not routed through here precisely so a file
 * that turns a monitor off before its neighbour has arrived still works. */
int server_output_set_enabled(FwmServer *server, FwmOutput *out, int on) {
    if (!out || out->enabled == !!on) return 0;
    if (!on) {
        int others = 0;
        FwmOutput *o;
        wl_list_for_each(o, &server->outputs, link)
            if (o != out && o->enabled) others++;
        if (!others) {
            wlr_log(WLR_INFO, "output %s: refusing to turn off the last screen",
                    out->wlr_output->name);
            return 0;
        }
    }
    if (!output_set_enabled(server, out, on)) return 0;
    /* Remembered so the next config reload does not undo it. Cleared by
     * turning the screen back on, whoever asks. */
    out->forced_off = !on;
    return 1;
}

/* ── resolution, scale and rotation ───────────────────────────────────────
 *
 * One entry point, used by both `[[output]]` and `fwmctl output`, so a
 * resolution set from the socket goes through exactly the code a reload does.
 * Everything it changes lands in ONE commit that is tested first: a monitor
 * that cannot do what was asked is left running what it was running, which
 * matters more here than anywhere else in fwm — the failure mode of getting
 * this wrong is a black screen with no way left to type the undo. */

FwmOutput *server_output_for(FwmServer *server, struct wlr_output *wlr_output) {
    if (!wlr_output) return NULL;
    FwmOutput *o;
    wl_list_for_each(o, &server->outputs, link) {
        if (o->wlr_output == wlr_output) return o;
    }
    return NULL;
}

FwmOutput *server_output_find(FwmServer *server, const char *name) {
    if (!name) return NULL;
    FwmOutput *o;
    wl_list_for_each(o, &server->outputs, link) {
        if (o->wlr_output->name && strcmp(o->wlr_output->name, name) == 0) return o;
    }
    return NULL;
}

/* The advertised mode that best answers width×height[@refresh], or NULL if the
 * monitor advertises none that fits.
 *
 * With no refresh asked for, the highest one at that size wins: a screen that
 * can do 144Hz should not be driven at 60 because 60 happened to be listed
 * first. With one asked for, it has to be within a hertz — near enough to
 * cover 59.94 written as 60, far enough from handing someone 144 when they
 * asked for 60. */
static struct wlr_output_mode *output_pick_mode(struct wlr_output *wlr_output,
                                                int w, int h, int refresh) {
    struct wlr_output_mode *mode, *best = NULL;
    wl_list_for_each(mode, &wlr_output->modes, link) {
        if (mode->width != w || mode->height != h) continue;
        if (refresh > 0) {
            if (abs(mode->refresh - refresh) > 1000) continue;
            if (!best || abs(mode->refresh - refresh) < abs(best->refresh - refresh))
                best = mode;
        } else if (!best || mode->refresh > best->refresh) {
            best = mode;
        }
    }
    return best;
}

/* Tacked onto a failure message: what the monitor would have accepted. A
 * rejected mode is nearly always a typo or a guess, and the list is the answer
 * to the question the error otherwise leaves the user holding. */
static void output_append_modes(struct wlr_output *wlr_output, char *err, size_t err_len) {
    size_t len = strlen(err);
    if (wl_list_empty(&wlr_output->modes) || len + 16 >= err_len) return;

    len += (size_t)snprintf(err + len, err_len - len, " — available:");
    struct wlr_output_mode *mode;
    wl_list_for_each(mode, &wlr_output->modes, link) {
        int n = snprintf(err + len, err_len - len, " %dx%d@%.2f",
                         mode->width, mode->height, mode->refresh / 1000.0);
        if (n < 0 || len + (size_t)n >= err_len) {
            /* Out of room: say so rather than end on a half-written mode. */
            snprintf(err + len, err_len - len, " ...");
            return;
        }
        len += (size_t)n;
    }
}

FwmOutputSetup server_output_setup_from_config(const ConfigOutput *cfg) {
    FwmOutputSetup s = {0};
    if (!cfg) return s;
    if (cfg->have_mode) {
        s.have_mode = 1;
        s.mode_w = cfg->mode_w;
        s.mode_h = cfg->mode_h;
        s.mode_refresh = cfg->mode_refresh;
    }
    if (cfg->scale > 0.0)    { s.have_scale = 1;     s.scale = cfg->scale; }
    if (cfg->transform >= 0) { s.have_transform = 1; s.transform = cfg->transform; }
    if (cfg->have_pos)       { s.have_pos = 1;       s.x = cfg->x; s.y = cfg->y; }
    return s;
}

bool server_output_apply_setup(FwmServer *server, FwmOutput *out,
                               const FwmOutputSetup *s, char *err, size_t err_len) {
    #define FAIL(...) do { if (err && err_len) snprintf(err, err_len, __VA_ARGS__); \
                           return false; } while (0)

    if (err && err_len) err[0] = '\0';
    if (!out || !s) FAIL("no such monitor");

    struct wlr_output *wlr_output = out->wlr_output;
    bool commit = s->have_mode || s->have_scale || s->have_transform;

    /* A dark monitor is out of the layout and running no mode at all; the
     * commit below would either fail or quietly light it back up, and neither
     * is what the caller asked for. */
    if (commit && !out->enabled)
        FAIL("%s is off — turn it on before setting a mode", wlr_output->name);

    if (commit) {
        struct wlr_output_state state;
        wlr_output_state_init(&state);

        bool custom = false;
        if (s->have_mode) {
            struct wlr_output_mode *mode =
                output_pick_mode(wlr_output, s->mode_w, s->mode_h, s->mode_refresh);
            if (mode) {
                wlr_output_state_set_mode(&state, mode);
            } else {
                /* Either the monitor lists no modes at all (the nested and
                 * headless backends are simply resizable) or it lists none
                 * that fit and the user means it. The test below is what
                 * decides whether the hardware agrees. */
                wlr_output_state_set_custom_mode(&state, s->mode_w, s->mode_h,
                                                 s->mode_refresh);
                custom = true;
            }
        }
        if (s->have_scale)     wlr_output_state_set_scale(&state, (float)s->scale);
        if (s->have_transform) wlr_output_state_set_transform(&state,
                                   (enum wl_output_transform)s->transform);

        bool ok = wlr_output_test_state(wlr_output, &state) &&
                  wlr_output_commit_state(wlr_output, &state);
        wlr_output_state_finish(&state);

        if (!ok) {
            if (err && err_len) {
                snprintf(err, err_len, "%s rejected the change", wlr_output->name);
                if (s->have_mode) output_append_modes(wlr_output, err, err_len);
            }
            return false;
        }

        wlr_log(WLR_INFO, "output %s: now %dx%d@%.2f%s, scale %.2f, transform %s",
                wlr_output->name, wlr_output->width, wlr_output->height,
                wlr_output->refresh / 1000.0, custom ? " (custom mode)" : "",
                wlr_output->scale, config_transform_name(wlr_output->transform));
    }

    /* Moving a screen is layout work, not a commit: the monitor draws exactly
     * what it drew, it is the world around it that shifts. */
    if (s->have_pos) {
        out->manual_pos = 1;
        out->manual_x = s->x;
        out->manual_y = s->y;
        if (out->enabled)
            wlr_output_layout_add(server->output_layout, wlr_output, s->x, s->y);
    }

    return true;
    #undef FAIL
}

/* What `[[output]]` asks for, applied to a monitor that is on. Called from the
 * config paths only; a mode the hardware refuses is logged and the screen goes
 * on running what it was running. */
static void output_apply_mode_config(FwmServer *server, FwmOutput *out) {
    const ConfigOutput *cfg = config_find_output(&server->config, out->wlr_output->name);
    if (!cfg) return;
    FwmOutputSetup setup = server_output_setup_from_config(cfg);
    if (!setup.have_mode && !setup.have_scale && !setup.have_transform) return;
    /* Position is output_join_layout's job on this path — it runs for monitors
     * with no mode to set too, so letting it own x/y keeps one answer. */
    setup.have_pos = 0;

    char err[512];
    if (!server_output_apply_setup(server, out, &setup, err, sizeof err))
        wlr_log(WLR_ERROR, "[[output]] %s: %s", cfg->name, err);
}

/* ── the laptop lid ───────────────────────────────────────────────────────
 *
 * Which monitor is the built-in panel is not something the kernel tells us in
 * any usable way, so it is read off the connector name — eDP, LVDS and DSI are
 * the three that mean "soldered to the machine". Every other compositor draws
 * the line in the same place. */
FwmOutput *server_internal_output(FwmServer *server) {
    FwmOutput *o;
    wl_list_for_each(o, &server->outputs, link) {
        const char *n = o->wlr_output->name;
        if (!n) continue;
        if (strncmp(n, "eDP", 3) == 0 || strncmp(n, "LVDS", 4) == 0 ||
            strncmp(n, "DSI", 3) == 0) return o;
    }
    return NULL;
}

/* Closing the lid puts the panel out, opening it brings it back.
 *
 * server_output_set_enabled refuses to turn off the last screen, which is
 * exactly the right answer for a laptop with nothing plugged in: shutting the
 * lid there is a suspend for logind to handle, not a request to leave the
 * session running on no display at all. */
void server_lid_changed(FwmServer *server, int closed) {
    FwmOutput *panel = server_internal_output(server);
    if (!panel) return;
    if (closed) server_output_set_enabled(server, panel, 0);
    else        server_output_set_enabled(server, panel, 1);
}

/* Put one monitor where [[output]] says, in the mode it says, or leave it as
 * the backend brought it up. Called when a monitor arrives and again on every
 * config reload, so moving or resizing a screen in config.toml takes effect
 * without a restart — and so a `fwmctl output` experiment is undone by the
 * same reload that undoes a `fwmctl set`.
 *
 * `enabled = false` in the file is honoured; a monitor turned off at RUNTIME
 * (the lid, the keybind) is left alone — the file did not ask for it, and a
 * reload must not light up the panel of a closed laptop. */
static void output_apply_config(FwmServer *server, FwmOutput *out) {
    const ConfigOutput *cfg = config_find_output(&server->config, out->wlr_output->name);
    int want_on = !cfg || cfg->enabled;

    if (!want_on) {
        output_set_enabled(server, out, 0);
        return;
    }
    /* The file says on. A monitor the FILE turned off comes back — that is the
     * reload doing its job — but one a person turned off stays off. */
    if (!out->enabled) {
        if (out->forced_off) return;
        output_set_enabled(server, out, 1);   /* which applies the mode too */
        return;
    }

    output_apply_mode_config(server, out);
    output_join_layout(server, out);
}

/* Every monitor re-placed from the current config. */
void server_outputs_apply_config(FwmServer *server) {
    FwmOutput *o;
    wl_list_for_each(o, &server->outputs, link) output_apply_config(server, o);
    /* Each add/remove above already fired the layout's change event, which is
     * what resizes the world and rebuilds what hangs off it. */

    /* A reload is the file having the last word, so a `desktop =` that changed
     * moves the monitor now — unlike at startup, where it is only a preference
     * among the free ones. Anything already showing that desktop trades, the
     * same as a desktop bind. */
    wl_list_for_each(o, &server->outputs, link) {
        if (!o->enabled) continue;
        const ConfigOutput *oc = config_find_output(&server->config, o->wlr_output->name);
        if (oc && oc->desktop >= 0) server_output_show_desktop(server, o, oc->desktop, 0);
    }
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

    FwmOutput *output = calloc(1, sizeof(FwmOutput));
    if (!output) return;
    output->server = server;
    output->wlr_output = wlr_output;
    output->enabled = 1;
    /* -1 is "no desktop yet": the layout update below hands it the one
     * [[output]] asks for, or the lowest no other monitor is showing. */
    output->desktop = -1;

    const ConfigOutput *cfg = config_find_output(&server->config, wlr_output->name);
    wlr_log(WLR_INFO, "output %s: %dx%d, scale %.2f%s", wlr_output->name,
            wlr_output->width, wlr_output->height, wlr_output->scale,
            cfg ? " ([[output]] entry found)" : "");

    output->frame.notify = handle_output_frame;
    output->destroy.notify = handle_output_destroy;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);
    wl_list_insert(&server->outputs, &output->link);

    /* Its ext-workspace group, before the layout hands it a desktop: the next
     * sync has to have somewhere to put that desktop. */
    workspace_output_added(server, output);

    /* In the list BEFORE it joins the layout: adding fires the layout's change
     * event, and the handler walks server->outputs to find the primary. The
     * scene output is built by output_join_layout, the one path that knows a
     * monitor is only really on when it has all three of output, layout entry
     * and scene output. */
    output_apply_config(server, output);
}

/* The first output in the layout: everything below exists once and needs a
 * world to be sized against, so it cannot run before one arrives. */
static void server_output_first_layout(FwmServer *server) {
    server_video_sync(server); // begin driving frames if the wallpaper is a video

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
        FwmOutput *po = server_primary_output(server);
        if (po) {
            po->desktop = d;
            po->camera_x = po->target_camera_x = d * server->screen_width;
            wallpaper_update(po->wallpaper, po->camera_x);
        }
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

    /* Every monitor arriving, leaving or changing mode ends here — including
     * the destroy path, which the layout handles for us. */
    server->output_layout_change.notify = handle_output_layout_change;
    wl_signal_add(&server->output_layout->events.change, &server->output_layout_change);

    if (server->output_power) {
        server->output_power_set_mode.notify = handle_output_power_set_mode;
        wl_signal_add(&server->output_power->events.set_mode, &server->output_power_set_mode);
    }
}
