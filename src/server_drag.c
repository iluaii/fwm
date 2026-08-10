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

/* The interactive gesture: what a held mouse button MEANS while it is held.
 *
 * Split out of server_pointer.c, which is about where the cursor is and what
 * is under it. Everything here runs between a press and its release — moving a
 * window, resizing one, dragging a tiling border, turning one by its corner,
 * swapping two tiles — and all of it ends by handing momentum to the
 * simulation, which is why it belongs together and away from the plumbing. */
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
#include "ui/modes.h"
#include "ui/hints.h"
#include "ui/errors.h"
#include "ui/welcome.h"
#include "ui/launcher.h"
#include "ui/cairo_overlay.h"
#include "wallpaper.h"
#include "group.h"
#include "expo.h"
#include <linux/input-event-codes.h>

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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Ceiling (rad/s) on the spin a twist can let go of: about two turns a second,
 * fast enough to look flung and slow enough that the window is still a window.
 * A flick at the corner of a wide window measures far more than the hand meant. */
#define TWIST_MAX_SPIN 12.0

/* How close to the window's centre the cursor may get before its angle stops
 * meaning anything. See the dead-zone note in the motion handler. */
#define TWIST_DEAD_ZONE 28.0

/* How far the hand must travel before a tiled window is taken out of the
 * layout. Without it, a click that happens to carry the move chord would tear
 * the window out and put it straight back — possibly on the other side of its
 * neighbour, since where it lands is read from the cursor. The layout is not
 * something to rearrange by accident. */
#define TILE_TEAR_PX 12.0

/* Winding a spinning window up by stirring the mouse in circles (see the drag
 * handler).
 *
 * The rate is measured over a WINDOW of hand movement, never from a single
 * pointer event. Dividing one event's change of direction by its own interval
 * was the first attempt and it was unusable: events arrive every 2-4ms, so a
 * few degrees of ordinary hand wobble read as hundreds of rad/s and the
 * smallest movement sent the window off like a buzzsaw. Summed over ~0.2s
 * instead, wobble cancels itself out (it turns both ways) while real circling
 * accumulates — which is exactly the difference we want to be sensitive to. */
#define SWIRL_MIN_SPEED 150.0   /* px/s below which the direction is noise */
#define SWIRL_MIN_STEP    0.02  /* s between samples; events are far denser */
#define SWIRL_TAU         0.20  /* s of hand movement the rate is read from */
#define SWIRL_GAIN        0.7   /* of the hand's rate; <1 keeps it controllable */
#define SWIRL_MAX         6.0   /* rad/s ceiling, about one turn a second */
#define SWIRL_DEADBAND    0.4   /* rad/s under which stirring is ignored */


/* Put the dragged window where the anchor and the cursor say it goes, and tell
 * the simulation. Split out of the motion handler because the CAMERA can move a
 * drag as well as the hand can (server_drag_follow_camera), and that path has no
 * pointer event to feed the velocity history and the swirl below.
 *
 * `lx`/`ly` are the cursor in layout coordinates. */
static void drag_place(FwmServer *server, double lx, double ly) {
    FwmView *view = server->interactive.view;
    if (!view) return;
    double dx = lx - server->interactive.start_x;
    double dy = ly - server->interactive.start_y;
    PhysicsBody *db = physics_find_body(&server->physics, view->id);
    /* A spinning window is placed by server_drag_swing on the physics tick
     * instead — it hangs from the grab point, so where it belongs depends
     * on an angle that is still being integrated. Writing a second,
     * unswung position here would fight it into a jitter. */
    bool swinging = db && db->spin;

    // Keep the window fully inside the play area while dragging. Because the
    // dragged body is kinematic it would otherwise pass straight through the
    // (static) boundary walls and, on release, either get stuck outside them
    // or be shot out at 90 degrees as Box2D resolves the wall penetration.
    int min_world_x = 0;
    int max_world_x = 10 * server->screen_width - server->interactive.view_start_width;
    int min_y = 0;
    int max_y = server->screen_height - server->interactive.view_start_height;
    if (max_world_x < min_world_x) max_world_x = min_world_x;
    if (max_y < min_y) max_y = min_y;

    /* view_start_x is already a world coordinate, and dx is a distance:
      * a hand moving n px across a monitor moves the window n px through
      * the world, whichever monitor that is. */
    int target_world_x = server->interactive.view_start_x + dx;
    int target_world_y = server->interactive.view_start_y + dy;
    int want_x = target_world_x, want_y = target_world_y;

    if (target_world_x < min_world_x) target_world_x = min_world_x;
    if (target_world_x > max_world_x) target_world_x = max_world_x;
    if (target_world_y < min_y) target_world_y = min_y;
    if (target_world_y > max_y) target_world_y = max_y;

    // When the clamp engages, re-base the grab anchor onto the clamped
    // position: while the window is pinned against a wall the cursor keeps
    // travelling, and without this the whole overshoot has to be dragged
    // back before the window moves again — magnet-stuck to the edge.
    // Only on an actual clamp: doing it unconditionally accumulates
    // int-truncation error every motion event and the window drifts away
    // from the cursor.
    if (target_world_x != want_x) {
        server->interactive.view_start_x += target_world_x - want_x;
    }
    if (target_world_y != want_y) {
        server->interactive.view_start_y += target_world_y - want_y;
    }

    if (!swinging) {
        view->x = target_world_x;
        view->y = target_world_y;

        if (view->scene_tree)
            server_place_node(server, &view->scene_tree->node, view->x, view->y);
    }

    physics_sync_body(&server->physics, view->id, view->x, view->y,
                      view->width, view->height, server->screen_width);
}

/* The camera has moved under a drag: bring the window along.
 *
 * A drag anchors its window with a SCREEN delta — where the window started plus
 * how far the cursor has travelled — and that is right for exactly as long as
 * the world does not move underneath it. Edge auto-scroll moves it: the camera
 * slides a whole screen while the cursor sits still against the edge, so the
 * world position the cursor is pointing at changes by a screen and the window's
 * does not. You arrive on the next desktop and the window you were holding is
 * still on the one you left.
 *
 * So carry the anchor with the camera. Called every tick the camera is
 * travelling, not once when the slide is ordered: the slide is an eased
 * animation and the window has to stay in the hand for every frame of it, not
 * jump a screen ahead and wait to be caught up with.
 *
 * A spinning window needs none of this — server_drag_swing_place already places
 * it from the cursor's WORLD position every frame, so the camera is in the sum
 * already. */
void server_drag_follow_camera(FwmServer *server) {
    if (server->interactive.action != FWM_ACTION_MOVE || !server->interactive.view) return;
    if (!server->cursor) return;

    FwmOutput *o = server_output_at(server, server->cursor->x, server->cursor->y);
    if (!o) return;

    /* Re-seed rather than shift when the hand crosses to another monitor: the
     * two cameras are independent, and the difference between them is not
     * travel the window did. */
    if (!server->interactive.cam_have || server->interactive.cam_output != o) {
        server->interactive.cam_output = o;
        server->interactive.cam_ref = o->camera_x;
        server->interactive.cam_have = 1;
        return;
    }

    int delta = o->camera_x - server->interactive.cam_ref;
    if (delta == 0) return;
    server->interactive.cam_ref = o->camera_x;
    server->interactive.view_start_x += delta;
    drag_place(server, server->cursor->x, server->cursor->y);
}

/* Take a tiled window out of the layout so it can be carried.
 *
 * The slot closes over the hole immediately — that is what makes this feel like
 * picking a window up rather than dragging a placeholder around — and the
 * window becomes an ordinary free one, which is all the drag needs it to be:
 * every other part of carrying a window (the camera following the hand to the
 * screen edge, the desktop it lands on, the throw at the end) already works on
 * free windows and is untouched by this.
 *
 * It goes back to whatever size it had before the desktop was tiled, held so
 * the hand keeps the same grip on it: shrinking a full-height tile under a
 * cursor that stays where it was would otherwise leave the window hanging off
 * a corner it was not picked up by. */
static void tile_tear_out(FwmServer *server, FwmView *view, PhysicsBody *pb,
                          double wx, double wy) {
    int d = pb->desktop_id;

    bsp_remove(&server->bsp_roots[d], view->id);
    pb->tiled = 0;
    pb->vx = 0; pb->vy = 0; pb->flying = 0;
    view->tile_anim = 0;

    double fx = view->width  > 0 ? (wx - view->x) / view->width  : 0.5;
    double fy = view->height > 0 ? (wy - view->y) / view->height : 0.5;

    if (pb->tiling_saved && pb->tile_sav_w > 0 && pb->tile_sav_h > 0) {
        pb->width  = view->width  = pb->tile_sav_w;
        pb->height = view->height = pb->tile_sav_h;
        view_set_size(view, view->width, view->height);
    }

    pb->x = wx - fx * view->width;
    pb->y = wy - fy * view->height;
    view->x = (int)lround(pb->x);
    view->y = (int)lround(pb->y);
    if (view->scene_tree)
        wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
    view_sync_position(view);

    /* The desktop it LEFT, laid out again without it. */
    server_apply_tiling(server, d);
}

/* Put a carried window down on a tiling desktop: beside the window it was
 * dropped on, on the side of that window the cursor is nearest to. Dropped on
 * the gaps, or on an empty desktop, it simply joins the layout wherever
 * bsp_insert would have put it. */
static void tile_drop(FwmServer *server, FwmView *view, int d, double wx, double wy) {
    BspNode *leaf = bsp_leaf_at(server->bsp_roots[d], wx, wy);
    if (leaf && leaf->aw > 0 && leaf->ah > 0) {
        /* Which edge of the target the cursor is nearest, measured as a
         * fraction of the slot so a tall thin window is judged by its own
         * shape rather than by pixels. */
        double fx = (wx - leaf->ax) / leaf->aw - 0.5;
        double fy = (wy - leaf->ay) / leaf->ah - 0.5;
        BspSide side = fabs(fx) >= fabs(fy)
            ? (fx < 0 ? BSP_SIDE_LEFT : BSP_SIDE_RIGHT)
            : (fy < 0 ? BSP_SIDE_UP   : BSP_SIDE_DOWN);
        bsp_insert_at(&server->bsp_roots[d], leaf->id, view->id, side);
    } else {
        bsp_insert(&server->bsp_roots[d], 0, view->id);
    }
    server_apply_tiling(server, d);
}

/* Motion while a gesture is held. False when there is none, and the caller
 * then does the ordinary hover-and-focus work. */
bool server_drag_motion(FwmServer *server, double lx, double ly,
                        const struct timespec *nowp) {
    struct timespec now = *nowp;
    (void)now;
    if (server->interactive.action == FWM_ACTION_MOVE) {
        FwmView *view = server->interactive.view;
        if (!view) return true;   /* client exited mid-drag; see the resize arm */
        PhysicsBody *db = physics_find_body(&server->physics, view->id);

        /* Still deciding whether this is a drag at all. Under the threshold the
         * window stays where the layout put it — and the drag is re-seeded from
         * HERE when it does leave, because the window has just changed size and
         * moved under the hand, and the start it was given at the press
         * describes a window that no longer exists. */
        if (server->interactive.tile_grab) {
            double dx = lx - server->interactive.start_x;
            double dy = ly - server->interactive.start_y;
            if (dx * dx + dy * dy < TILE_TEAR_PX * TILE_TEAR_PX) return true;

            double wx, wy;
            if (!db || !server_screen_to_world(server, lx, ly, &wx, &wy)) return true;
            tile_tear_out(server, view, db, wx, wy);
            server->interactive.tile_grab = 0;
            server->interactive.start_x = lx;
            server->interactive.start_y = ly;
            server->interactive.view_start_x = view->x;
            server->interactive.view_start_y = view->y;
            server->interactive.view_start_width = view->width;
            server->interactive.view_start_height = view->height;
            server->interactive.cam_have = 0;
            server->interactive.cam_output = NULL;
            /* Where the hand is on the window it is now holding, for the swing.
             * A tile cannot have been spinning, so its frame is unrotated. */
            server->interactive.grab_lx = wx - (view->x + view->width  / 2.0);
            server->interactive.grab_ly = wy - (view->y + view->height / 2.0);
            server->interactive.pivot_have = 0;
            view_jelly_begin(view, server->config.effects.jelly,
                             wx - view->x, wy - view->y);
        }

        drag_place(server, lx, ly);

        // Shift velocity history
        for (int i = 0; i < 3; i++) {
            server->interactive.hist_x[i] = server->interactive.hist_x[i+1];
            server->interactive.hist_y[i] = server->interactive.hist_y[i+1];
            server->interactive.hist_time[i] = server->interactive.hist_time[i+1];
        }
        server->interactive.hist_x[3] = lx;
        server->interactive.hist_y[3] = ly;
        server->interactive.hist_time[3] = now;
        if (server->interactive.hist_count < 4) server->interactive.hist_count++;
        
        if (server->interactive.hist_count >= 2) {
            int oldest = 4 - server->interactive.hist_count;
            double dt = (double)(now.tv_sec - server->interactive.hist_time[oldest].tv_sec) +
                        (double)(now.tv_nsec - server->interactive.hist_time[oldest].tv_nsec) / 1e9;
            if (dt > 0.001) {
                server->interactive.vx = (lx - server->interactive.hist_x[oldest]) / dt;
                server->interactive.vy = (ly - server->interactive.hist_y[oldest]) / dt;
            }
        }

        /* Swirl the dragged window up (only one that is already spinning: see
         * spin_window). What is measured is not where the cursor is but how
         * fast its DIRECTION of travel is turning — stir the mouse in circles
         * and that rate is the rate of the circles, so the window turns with
         * your hand; drag it in a straight line and it is exactly zero.
         *
         * Measuring the cursor's angle around the window center instead does
         * not work at all: the window follows the cursor, so the grab point
         * stays put relative to it and circling produces a sine that averages
         * to nothing. */
        {
            PhysicsBody *sb = db;
            double sp = hypot(server->interactive.vx, server->interactive.vy);
            if (sb && sb->spin && sp > SWIRL_MIN_SPEED) {
                double dir = atan2(server->interactive.vy, server->interactive.vx);
                double dt_s = server->interactive.swirl_have
                    ? (double)(now.tv_sec - server->interactive.swirl_time.tv_sec)
                      + (double)(now.tv_nsec - server->interactive.swirl_time.tv_nsec) / 1e9
                    : 0.0;
                /* Pointer events arrive every few ms. Sampling each one reads
                 * the hand's tremor rather than its path, so take one every
                 * SWIRL_MIN_STEP and let the ones in between accumulate into
                 * the same interval. */
                if (server->interactive.swirl_have && dt_s >= SWIRL_MIN_STEP) {
                    double d = dir - server->interactive.swirl_dir;
                    while (d >  M_PI) d -= 2.0 * M_PI;   /* shortest way round */
                    while (d < -M_PI) d += 2.0 * M_PI;
                    /* Half a turn between two samples is not a swirl, it is the
                     * hand reversing; taking it as one would fling the window
                     * the wrong way on every direction change. */
                    if (dt_s < 0.2 && fabs(d) < M_PI / 2.0) {
                        /* Leaky integrals: all three fade with the same time
                         * constant, so their ratios describe the last
                         * SWIRL_TAU seconds of hand movement without anything
                         * being kept in a ring buffer. */
                        double decay = exp(-dt_s / SWIRL_TAU);
                        server->interactive.swirl_acc =
                            server->interactive.swirl_acc * decay + d;
                        server->interactive.swirl_abs =
                            server->interactive.swirl_abs * decay + fabs(d);
                        server->interactive.swirl_span =
                            server->interactive.swirl_span * decay + dt_s;

                        if (server->interactive.swirl_span > 0.05 &&
                            server->interactive.swirl_abs > 1e-6) {
                            /* How much of the turning went the SAME way: 1 for
                             * a clean circle, near 0 for a hand that wobbles
                             * both ways while dragging in a straight-ish line.
                             * Squared, because without it an unsteady hand
                             * still won — a random walk of directions sums to a
                             * large number often enough to fling the window
                             * across the screen, which is exactly how this
                             * first shipped and exactly how it felt. */
                            double coh = fabs(server->interactive.swirl_acc)
                                       / server->interactive.swirl_abs;
                            double omega = server->interactive.swirl_acc
                                         / server->interactive.swirl_span;
                            omega *= SWIRL_GAIN * coh * coh;
                            if (omega >  SWIRL_MAX) omega =  SWIRL_MAX;
                            if (omega < -SWIRL_MAX) omega = -SWIRL_MAX;
                            /* Approach rather than assign, so the window winds
                             * up over a stroke or two instead of snapping to
                             * whatever the last fifth of a second looked like.
                             *
                             * Only ever ADDS speed (or reverses it): a window
                             * held by its corner is already being whirled by
                             * the swing, usually faster than the hand itself
                             * turns, and braking it back down to the hand's
                             * rate would undo the physical part with the
                             * gesture part. */
                            bool helps = (omega > 0) != (sb->angvel > 0)
                                       || fabs(omega) > fabs(sb->angvel);
                            if (fabs(omega) >= SWIRL_DEADBAND && helps)
                                sb->angvel += (omega - sb->angvel) * 0.15;
                        }
                    }
                }
                /* Only a processed sample becomes the new reference: skipping
                 * one must leave the interval to grow, not restart it. */
                if (!server->interactive.swirl_have || dt_s >= SWIRL_MIN_STEP) {
                    server->interactive.swirl_dir = dir;
                    server->interactive.swirl_time = now;
                    server->interactive.swirl_have = 1;
                }
            }
        }
        
        /* Auto camera scroll at edges.
         *
         * Throttled by the camera still travelling — one step per slide, or the
         * desktops would flick past as fast as motion events arrive. A step
         * across the ring's join has no camera travel to throttle it: the
         * camera is on the far side the same frame. Its slide is what stands in
         * for that, and without this line dragging a window into the edge on
         * the last desktop spun the world round the ring until the hand moved
         * away, which is not "the window would not cross" but looks a great
         * deal like it. */
        FwmOutput *eo = server_output_at(server, lx, ly);
        if (eo && eo->camera_x == eo->target_camera_x
            && eo->wrap_slide <= 0.0) {
            int current_d = eo->desktop;
            /* The edge of THIS monitor, not of the layout. */
            double ex = lx - eo->box.x;
            int step = ex >= eo->box.width - 10 ? 1 : (ex <= 10 ? -1 : 0);
            if (step) {
                /* Dragging a window off the end of a ring puts it on the other
                 * end, which is the whole point of the ring — the window is
                 * already following the cursor and moves with the camera. */
                int d = current_d + step;
                int seam = 0;
                if (d < 0 || d >= FWM_DESKTOPS) {
                    if (!server->config.camera.wrap) d = current_d;
                    else { d = (d + FWM_DESKTOPS) % FWM_DESKTOPS; seam = 1; }
                }
                if (d != current_d) server_goto_desktop(server, d, seam);
            }
        }
    } else if (server->interactive.action == FWM_ACTION_RESIZE) {
        FwmView *view = server->interactive.view;
        /* The window can go while the hand is still on it — a client that
         * exits mid-drag. Destroy clears the view but not the action. */
        if (!view) return true;
        double dx = lx - server->interactive.start_x;
        double dy = ly - server->interactive.start_y;

        int new_w = server->interactive.view_start_width + dx;
        int new_h = server->interactive.view_start_height + dy;
        /* Room to grow reaches the right edge of the window's OWN desktop, not
         * of the world: view_start_x is a world coordinate, so on desktop N it
         * is already N screens along and a plain screen_width - x is negative
         * there. That negative then beat the 50px floor below (the ceiling was
         * applied second) and a negative width tripped an assertion inside
         * wlroots — resizing any window off desktop 0 killed the compositor. */
        int desk = server->interactive.view_start_x / server->screen_width;
        if (desk < 0) desk = 0;
        int max_w = (desk + 1) * server->screen_width - server->interactive.view_start_x;
        int max_h = server->screen_height - server->interactive.view_start_y;

        if (new_w > max_w) new_w = max_w;
        if (new_h > max_h) new_h = max_h;
        /* The floor is applied last, so a window standing right at an edge —
         * where the room left is under 50px — still keeps a usable size and
         * simply overhangs. */
        if (new_w < 50) new_w = 50;
        if (new_h < 50) new_h = 50;

        view->width = new_w;
        view->height = new_h;
        
        view_set_size(view, view->width, view->height);
        physics_sync_body(&server->physics, view->id, view->x, view->y, view->width, view->height, server->screen_width);
    } else if (server->interactive.action == FWM_ACTION_BSP_RESIZE) {
        int d = server->interactive.bsp_desktop;
        BspNode *root = (d >= 0 && d < FWM_DESKTOPS) ? server->bsp_roots[d] : NULL;
        /* The dividers are pointers into a tree that belongs to the layout, not
         * to the drag: a window closing frees the node under the hand, and
         * toggling the desktop out of tiling frees the whole tree. Both are a
         * keypress or a client exit away while one is held, and the drag used to
         * write the new ratio into that freed memory. Ask the tree whether each
         * node is still in it, every event, and let that axis go if not. */
        if (!bsp_contains(root, server->interactive.bsp_node))
            server->interactive.bsp_node = NULL;
        if (!bsp_contains(root, server->interactive.bsp_node_v))
            server->interactive.bsp_node_v = NULL;
        if (!server->interactive.bsp_node && !server->interactive.bsp_node_v) {
            server->interactive.action = FWM_ACTION_NONE;
            return true;
        }

        /* Measured from where the drag STARTED, not from the last event: a
         * ratio that stops at its limit must not lose the travel that took it
         * there, or pulling back would leave the divider behind the hand by
         * however far it was pushed past the end. */
        int gap = server->config.tiling.gaps_in;
        BspNode *hn = server->interactive.bsp_node;
        if (hn && hn->aw > gap + 1) {
            float lo, hi;
            server_tile_ratio_limits(server, d, hn, &lo, &hi);
            float r = server->interactive.bsp_start_ratio
                    + (float)(lx - server->interactive.start_x) / (float)(hn->aw - gap);
            hn->ratio = r < lo ? lo : (r > hi ? hi : r);
        }
        BspNode *vn = server->interactive.bsp_node_v;
        if (vn && vn->ah > gap + 1) {
            float lo, hi;
            server_tile_ratio_limits(server, d, vn, &lo, &hi);
            float r = server->interactive.bsp_start_ratio_v
                    + (float)(ly - server->interactive.start_y) / (float)(vn->ah - gap);
            vn->ratio = r < lo ? lo : (r > hi ? hi : r);
        }

        /* The desktop the dividers belong to, which on a second monitor is not
         * the one the hand happens to be over. */
        server_apply_tiling(server, d);
    } else if (server->interactive.action == FWM_ACTION_TWIST) {
        FwmView *view = server->interactive.view;
        PhysicsBody *pb = view ? physics_find_body(&server->physics, view->id) : NULL;
        if (pb && pb->spin) {
            double cx, cy;
            /* No screen position for the centre means the window's desktop is
             * not on any monitor any more — a screen unplugged, or a lid closed,
             * with the hand still down. There is no angle to measure against, so
             * sit the frame out rather than turn the window by whatever the
             * uninitialised stack happened to hold. The unwrap below keeps the
             * jump to half a turn if the monitor comes back mid-twist. */
            if (!server_world_to_screen(server, view->x + view->width / 2.0,
                                        view->y + view->height / 2.0, &cx, &cy))
                return true;
            double a = atan2(ly - cy, lx - cx);

            /* atan2 jumps by 2π at the back of the circle; unwrap against the
             * last sample so a hand going round and round keeps winding the
             * window up instead of flipping it. */
            double d = a - server->interactive.twist_last;
            while (d >  M_PI) d -= 2.0 * M_PI;
            while (d < -M_PI) d += 2.0 * M_PI;
            server->interactive.twist_last = a;

            /* Near the centre the angle is meaningless: at 4px out, one pixel
             * of ordinary hand tremor is fifteen degrees, and the window snaps
             * about while the cursor is barely moving. Inside the dead zone the
             * angle is still TRACKED (twist_last above) but not applied, so
             * leaving the zone continues from wherever the hand now is instead
             * of jumping. Radius is a fixed number of pixels rather than a
             * fraction of the window: the trouble is cursor noise, which does
             * not care how big the window is. */
            double radius = hypot(lx - cx, ly - cy);
            if (radius >= TWIST_DEAD_ZONE) {
                server->interactive.twist_base += d;
                pb->angle = server->interactive.twist_base;
            } else {
                d = 0.0;   /* nothing turned, so nothing to hand the release */
            }

            /* How fast the hand is turning, smoothed. The release hands this to
             * the simulation, and an unsmoothed sample would let one 2 ms frame
             * decide whether the window drifts or is flung. */
            double dt_t = (now.tv_sec - server->interactive.twist_time.tv_sec)
                        + (now.tv_nsec - server->interactive.twist_time.tv_nsec) / 1e9;
            if (dt_t > 0.001) {
                double rate = d / dt_t;
                double k = dt_t / (dt_t + 0.08);   /* ~80 ms of hand movement */
                server->interactive.twist_vel += (rate - server->interactive.twist_vel) * k;
                server->interactive.twist_time = now;
            }
        }
    } else if (server->interactive.action == FWM_ACTION_SWAP) {
        server->interactive.cur_x = lx;
        server->interactive.cur_y = ly;
    } else {
        return false;
    }
    return true;
}

/* A press landed on a window. True when it became a gesture (or fired a mouse
 * bind) and the caller must not treat it as a click. */
bool server_drag_press(FwmServer *server, uint32_t button, double lx, double ly,
                       const struct timespec *nowp) {
    /* The press in world coordinates: everything below that talks to a body or
     * a layout works in the world, and the monitor under the cursor is what
     * says which desktop's world that is. */
    /* No monitor under the cursor — unplugged with the hand still moving, or a
     * gap in the layout — leaves nothing to convert against. The reads below
     * would then be of whatever the stack held, and a border grab or a jelly
     * hold measured against that lands the window anywhere at all. */
    double wx, wy;
    if (!server_screen_to_world(server, lx, ly, &wx, &wy)) return false;

    struct timespec now = *nowp;
    (void)now;
    (void)button;
        struct wlr_surface *surface = NULL;
        double sx, sy;
        FwmView *view = view_at(server, lx, ly, &surface, &sx, &sy);
        
        if (view) {
            server->last_touched_view = view;
            /* Touching a window ends its impact squash: while one runs the
             * live surface is hidden behind a snapshot, so without this the
             * grabbed window would keep wobbling and stay unresponsive inside
             * for the rest of the effect. The user taking hold of it outranks
             * the animation. */
            view_stop_squash(view);
            view_jelly_stop(view);
            server_focus_view(server, view);
            physics_stop_body(&server->physics, view->id);
            
            uint32_t active_mods = get_active_modifiers(server);

            /* What this chord does is [mouse]'s to say. The verb it names is
             * then read against the desktop's mode, exactly as the hard-coded
             * behaviour this replaced was: a tiling desktop owns its geometry,
             * so there a drag can only swap tiles or move a border. */
            const MouseBind *mb = config_match_mouse(&server->config,
                                                     button_to_fwm(button), active_mods);
            if (mb) {
                PhysicsBody *pb = physics_find_body(&server->physics, view->id);
                int tiling = pb ? (server->desktop_mode[pb->desktop_id] == DESKTOP_MODE_TILING) : 0;
                const char *verb = mb->action;

                /* Not a drag verb: an ordinary action, fired once on the press.
                 * Lets "super+middle" close a window, or a side button open the
                 * launcher, without inventing a second action vocabulary. */
                if (!config_action_is_drag(verb)) {
                    server_dispatch_action(server, verb);
                    return true;
                }

                int is_move = strcmp(verb, FWM_MOUSE_MOVE) == 0;
                int is_move_nc = strcmp(verb, FWM_MOUSE_MOVE_NOCOLLIDE) == 0;
                /* On a tiling desktop a move is a swap: the layout decides where
                 * tiles are, so dragging one can only mean trading it with the
                 * tile it is dropped on. Plain `move` stays inert there, as it
                 * always has — a bare super+drag over tiles used to do nothing,
                 * and quietly turning that into a swap would surprise a hand
                 * that has learnt it. */
                int is_swap = strcmp(verb, FWM_MOUSE_SWAP) == 0 || (tiling && is_move_nc);

                if (is_swap) {
                    if (tiling) {
                        server->interactive.action = FWM_ACTION_SWAP;
                        server->interactive.view = view;
                        server->interactive.start_x = lx;
                        server->interactive.start_y = ly;
                        server->interactive.cur_x = lx;
                        server->interactive.cur_y = ly;
                    }
                } else if (strcmp(verb, FWM_MOUSE_TWIST) == 0) {
                    /* Turning a window by hand. Same gate as the spin_window
                     * bind: a tiled, pinned or fullscreen window has nowhere to
                     * turn to. */
                    double cx, cy;
                    if (pb && server_can_spin(pb) && server->config.effects.spin > 0.0
                        && server_world_to_screen(server, view->x + view->width / 2.0,
                                                  view->y + view->height / 2.0, &cx, &cy)) {
                        /* Spinning it with no kick: the hand supplies the
                         * rotation from here, and physics only takes over at
                         * the release. */
                        physics_spin_body(&server->physics, view->id, 0.0);
                        server->interactive.action = FWM_ACTION_TWIST;
                        server->interactive.view = view;
                        server->interactive.twist_base = pb->angle;
                        server->interactive.twist_last = atan2(ly - cy, lx - cx);
                        server->interactive.twist_vel = 0.0;
                        server->interactive.twist_time = now;
                    }
                } else if (strcmp(verb, FWM_MOUSE_RESIZE) == 0) {
                    if (tiling) {
                        /* Resize by the CORNER the hand is nearest, from
                         * anywhere inside the window — the same grab a floating
                         * window gets, and the same one Hyprland gives a tile.
                         *
                         * It replaces hunting for the seam between two windows.
                         * That was hard to hit and, worse, ambiguous: the search
                         * ran from the root down and took the first divider
                         * within reach, so an outer split passing anywhere near
                         * the window's edge won over the one actually under the
                         * cursor, and the wrong pair of windows moved. */
                        int d = pb ? pb->desktop_id : 0;
                        BspNode *leaf = bsp_leaf_at(server->bsp_roots[d], wx, wy);
                        if (leaf) {
                            int right = wx >= leaf->ax + leaf->aw / 2.0;
                            int below = wy >= leaf->ay + leaf->ah / 2.0;
                            /* The divider along the chosen edge, or the one on
                             * the far side when this edge is the screen's: a
                             * window in the corner of the layout still has to be
                             * resizable, and it has only one divider to do it
                             * with. Moving a divider always means "this way is
                             * bigger", whichever side of it the window is on, so
                             * neither case needs a sign of its own. */
                            BspNode *hn = bsp_edge_node(leaf, 0, right);
                            if (!hn) hn = bsp_edge_node(leaf, 0, !right);
                            BspNode *vn = bsp_edge_node(leaf, 1, below);
                            if (!vn) vn = bsp_edge_node(leaf, 1, !below);
                            if (hn || vn) {
                                server->interactive.action = FWM_ACTION_BSP_RESIZE;
                                server->interactive.bsp_node = hn;
                                server->interactive.bsp_node_v = vn;
                                server->interactive.bsp_desktop = d;
                                server->interactive.bsp_start_ratio = hn ? hn->ratio : 0.5f;
                                server->interactive.bsp_start_ratio_v = vn ? vn->ratio : 0.5f;
                                server->interactive.start_x = lx;
                                server->interactive.start_y = ly;
                            }
                        }
                    } else {
                        server->interactive.action = FWM_ACTION_RESIZE;
                        server->interactive.view = view;
                        server->interactive.start_x = lx;
                        server->interactive.start_y = ly;
                        server->interactive.view_start_x = view->x;
                        server->interactive.view_start_y = view->y;
                        server->interactive.view_start_width = view->width;
                        server->interactive.view_start_height = view->height;
                    }
                } else if (is_move || is_move_nc) {
                    /* A tiled window is picked UP: it leaves the layout and
                     * becomes a free window in the hand, carried by the ordinary
                     * move below. Not one that owns the whole screen or is
                     * nailed down — those two are not the layout's to give away.
                     *
                     * The window does not actually leave until the hand has
                     * moved (server_drag_motion); all that is set up here is the
                     * same drag it would get anywhere else. */
                    int held_tile = tiling && pb && !pb->fullscreen && !pb->pinned;
                    if (held_tile) tiling = 0;
                    if (!tiling) {
                        server->interactive.tile_grab = held_tile;
                        server->interactive.action = FWM_ACTION_MOVE;
                        server->interactive.view = view;
                        server->interactive.start_x = lx;
                        server->interactive.start_y = ly;
                        server->interactive.view_start_x = view->x;
                        server->interactive.view_start_y = view->y;
                        server->interactive.view_start_width = view->width;
                        server->interactive.view_start_height = view->height;
                        server->interactive.last_x = lx;
                        server->interactive.last_y = ly;
                        server->interactive.last_time = now;
                        server->interactive.vx = 0;
                        server->interactive.vy = 0;
                        server->interactive.hist_count = 0;
                        server->interactive.swirl_have = 0;
                        server->interactive.swirl_acc = 0.0;
                        server->interactive.swirl_abs = 0.0;
                        server->interactive.swirl_span = 0.0;
                        /* Unseeded, exactly as server_start_interactive_move
                         * leaves it — server_drag_follow_camera measures the
                         * camera against a reference taken when the drag
                         * began, and a reference left over from the PREVIOUS
                         * drag is a distance the window never travelled.
                         *
                         * Missing here, this was: a drag on desktop 1 seeded
                         * the reference at camera 0, and the next drag one
                         * desktop over opened with a delta of a whole screen
                         * and teleported the window forward by it. Each drag
                         * then re-seeded one desktop behind the next, so it
                         * was +1 desktop from the second onward and clean on
                         * the first — and it vanished whenever two drags
                         * happened on the same desktop, which is what made it
                         * look intermittent. */
                        server->interactive.cam_have = 0;
                        server->interactive.cam_output = NULL;

                        /* Remember WHERE the window was taken hold of, in its
                         * own frame, so a spinning one can hang from that
                         * point (server_drag_swing). Recorded for every drag,
                         * spinning or not: the window may be set spinning
                         * while it is already being held. */
                        {
                            double cx = view->x + view->width  / 2.0;
                            double cy = view->y + view->height / 2.0;
                            double ox = wx - cx;
                            double oy = wy - cy;
                            double ang = pb ? pb->angle : 0.0;
                            double c = cos(-ang), s = sin(-ang);
                            server->interactive.grab_lx = c * ox - s * oy;
                            server->interactive.grab_ly = s * ox + c * oy;
                            server->interactive.pivot_have = 0;
                            server->interactive.pivot_ax = 0.0;
                            server->interactive.pivot_ay = 0.0;
                        }
                        server->interactive.collision_disabled = is_move_nc;

                        /* The window goes soft for as long as it is held, and
                         * the sheet is held at the point the cursor took hold
                         * of — window-local, from its top-left, which is the
                         * frame the lattice works in. (Not interactive.grab_l*
                         * above: that one is measured from the CENTRE and
                         * un-rotated, for the pendulum a spinning window hangs
                         * from.) A tile that has not been torn out yet does not
                         * move under the hand, so there is nothing for the sheet
                         * to lag behind until it does. */
                        if (!held_tile)
                            view_jelly_begin(view, server->config.effects.jelly,
                                             wx - view->x, wy - view->y);
                    }
                }
            }
        }
    return false;
}

/* The release: hand whatever the hand was doing to the simulation. */
void server_drag_release(FwmServer *server, double lx, double ly) {
        // Button release
        if (server->interactive.action == FWM_ACTION_MOVE) {
            FwmView *view = server->interactive.view;
            /* Let the wobble ring itself out — whatever the drop turns out to
             * be, the hand is off the window. */
            if (view) view_jelly_release(view);
            // Dropping an ungrouped window onto a tab bar adds it to that stack.
            int tab;
            FwmGroup *bg = view ? group_bar_at(server, lx, ly, &tab) : NULL;
            if (view && bg && !view->group && group_add(server, bg, view)) {
                // adopted by the group — no throw
            } else if (view) {
                PhysicsBody *pb = physics_find_body(&server->physics, view->id);
                if (pb) {
                    if (server->desktop_mode[pb->desktop_id] == DESKTOP_MODE_TILING) {
                        /* Landed on a tiling desktop: join the layout where it
                         * was put down. Already in the tree means the drag never
                         * took it out (fullscreen, pinned), and then there is
                         * nothing to place — only the layout to restore. */
                        int d = pb->desktop_id;
                        double wx, wy;
                        if (!bsp_find(server->bsp_roots[d], view->id)
                            && server_screen_to_world(server, lx, ly, &wx, &wy)) {
                            tile_drop(server, view, d, wx, wy);
                        } else {
                            server_apply_tiling(server, d);
                        }
                    } else {
                        physics_push_overlapping(&server->physics, view->id, 280.0);
                        physics_throw_body(&server->physics, view->id, server->interactive.vx, server->interactive.vy);
                    }
                }
            }
        } else if (server->interactive.action == FWM_ACTION_TWIST) {
            /* Let go and the window keeps turning at the rate the hand was
             * turning it — the whole point of doing this by hand rather than
             * with a bind that guesses a number. Capped, because a flick at the
             * very edge of a large window can measure a rate that would leave
             * the window a blur nobody asked for. */
            FwmView *view = server->interactive.view;
            PhysicsBody *pb = view ? physics_find_body(&server->physics, view->id) : NULL;
            if (pb && pb->spin) {
                double w = server->interactive.twist_vel;
                if (w >  TWIST_MAX_SPIN) w =  TWIST_MAX_SPIN;
                if (w < -TWIST_MAX_SPIN) w = -TWIST_MAX_SPIN;
                pb->angvel = w;
            }
        } else if (server->interactive.action == FWM_ACTION_SWAP) {
            FwmView *src_view = server->interactive.view;
            if (src_view) {
                PhysicsBody *src_pb = physics_find_body(&server->physics, src_view->id);
                if (src_pb) {
                    /* The window under the cursor, in the world the layout is
                     * measured in. Tested against where the windows actually
                     * are: the slot grid this used to compare against is not
                     * where a client that committed a smaller size ended up. */
                    int d = src_pb->desktop_id;
                    double wx, wy;
                    BspNode *target = server_screen_to_world(server,
                            server->interactive.cur_x, server->interactive.cur_y, &wx, &wy)
                        ? bsp_leaf_at(server->bsp_roots[d], wx, wy) : NULL;

                    if (target && target->id != src_view->id) {
                        bsp_swap(server->bsp_roots[d], src_view->id, target->id);
                        server_apply_tiling(server, d);
                    }
                }
            }
        }
        
        // X11 windows: push the final position after a drag/resize so the
        // client's idea of its root coordinates matches reality again.
        if (server->interactive.view) view_sync_position(server->interactive.view);

        /* A divider that has just been let go: the desktop is laid out one more
         * time below, after the action is cleared, so it settles against the
         * sizes the clients really took. While the hand was on it the layout was
         * following the slots instead — see server_align_tiles. */
        int settle_d = server->interactive.action == FWM_ACTION_BSP_RESIZE
                     ? server->interactive.bsp_desktop : -1;

        server->interactive.action = FWM_ACTION_NONE;
        server->interactive.view = NULL;
        server->interactive.tile_grab = 0;
        /* Nothing may keep a pointer into the layout's tree past the gesture
         * that borrowed it: the next thing to touch the tree is free to free
         * these nodes, and the check that guards them is in the drag. */
        server->interactive.bsp_node = NULL;
        server->interactive.bsp_node_v = NULL;

        if (settle_d >= 0) server_apply_tiling(server, settle_d);
}
