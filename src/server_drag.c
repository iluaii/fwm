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
    int max_world_x = (int)PHYSICS_WORLD_W(server->screen_width)
                    - server->interactive.view_start_width;
    int min_y = 0;
    if (max_world_x < min_world_x) max_world_x = min_world_x;

    /* view_start_x is already a world coordinate, and dx is a distance:
      * a hand moving n px across a monitor moves the window n px through
      * the world, whichever monitor that is. */
    int target_world_x = server->interactive.view_start_x + dx;
    int target_world_y = server->interactive.view_start_y + dy;
    int want_x = target_world_x, want_y = target_world_y;

    if (target_world_x < min_world_x) target_world_x = min_world_x;
    if (target_world_x > max_world_x) target_world_x = max_world_x;

    /* THE JOIN, CLOSED IN ONE PLACEMENT.
     *
     * A window is drawn by a single screen and cut to it, and which screen that
     * is flips when its LEFT EDGE crosses into the next column — not when the
     * hand crosses the bezel. The two moments are a whole grab-offset apart.
     *
     * That matters because the flip changes the frame the window's position is
     * read in: server_world_to_screen adds the drawing monitor's box.y, and two
     * monitors of different height are stood on a shared centre or a shared
     * bottom, never a shared top. So the flip alone moves the window down the
     * glass by the gap between their top edges, and a shift of the anchor alone
     * moves it up by the same amount. Do one without the other and you see it.
     *
     * So they are done together, here, against the screen that is about to draw
     * the window rather than the one under the hand: the anchor is carried by
     * exactly what the change of frame is about to undo, and the window does not
     * move on the glass at all. The join stops being a place where anything
     * happens. */
    if (server->interactive.cam_have) {
        FwmOutput *frame = server_view_frame(server, view, target_world_x,
                                             server->interactive.view_start_width,
                                             NULL, NULL);
        if (frame) {
            int off  = frame->camera_x - frame->box.x;
            int offy = -frame->box.y;
            int cx = off  - server->interactive.cam_offset;
            int cy = offy - server->interactive.cam_offset_y;
            if (cx || cy) {
                server->interactive.cam_offset   = off;
                server->interactive.cam_offset_y = offy;
                server->interactive.view_start_x += cx;
                server->interactive.view_start_y += cy;
                target_world_x += cx;  want_x += cx;
                target_world_y += cy;  want_y += cy;
                /* The world moved, the window did not; the wobble must not be
                 * told it travelled. */
                view_jelly_carry(view, cx, cy);
            }
        }
    }

    /* The floor is the MONITOR's, not the column's — the same correction the
     * resize path makes a few hundred lines down, for the same reason. A column
     * is the size of the PRIMARY monitor, so on a shorter screen its bottom
     * stretch is glass nobody has: clamping a drag to the column let a window
     * be carried down past the last row of pixels that screen owns, where
     * server_views_clip cuts it away and the hand is holding something it can
     * no longer see.
     *
     * Read off where the window is GOING, not off the anchor it started from:
     * the anchor is a whole screen behind during a crossing, and a window on
     * its way onto the short monitor would be measured against the tall one it
     * is leaving. A column no monitor is showing has only the column to go by. */
    int drag_col = target_world_x / server->screen_width;
    if (drag_col < 0) drag_col = 0;
    if (drag_col >= FWM_DESKTOPS) drag_col = FWM_DESKTOPS - 1;
    FwmOutput *dmon = server_output_showing(server, drag_col);
    int lim_h = dmon && dmon->box.height > 0 ? dmon->box.height : server->screen_height;

    int max_y = lim_h - server->interactive.view_start_height;
    if (max_y < min_y) max_y = min_y;

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
            server_place_view(server, view, view->x, view->y);
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
/* THE HAND CROSSED TO ANOTHER SCREEN. Carry the window over with it.
     *
     * The drag moves a window through the WORLD by the distance the hand moved
     * across the desk (drag_place), which is right for as long as both are on
     * one screen and wrong the moment they are not: two monitors are two
     * windows onto the same strip, and the strip does not have to run past
     * them in order. The screen on the right can be showing desktop 5 while
     * the one on the left shows desktop 0 — and then a hand that travelled
     * 1300px right has moved the window 1300px into desktop 1, which nothing
     * is showing. The window vanishes out from under the cursor that is
     * carrying it.
     *
     * So re-home it: shift the anchor by the difference between what the two
     * screens have under them, which leaves the window exactly where it looks
     * like it is — still in the hand, on the same pixel of glass — and puts it
     * in the column the new screen is showing. Two screens that happen to show
     * neighbouring desktops in order have no difference between them, the shift
     * is zero, and a drag across that join behaves as it always did.
     *
     * The camera reference is re-seeded rather than carried for the reason it
     * always was: the two cameras are independent, and the gap between them is
     * not travel the window did.
     *
     * TAKES THE POSITION rather than reading the cursor, and does NOT place the
     * window itself. Both so that the POINTER path can settle a crossing with
     * the very event that caused it: the tick runs at 60Hz and motion events
     * arrive several hundred times a second, so a crossing left for the next
     * tick is up to a frame of the window drawn against the old screen's
     * offset and then snapped back — which on two monitors of different height
     * is a visible jerk, the width of the gap between their top edges. The
     * caller places the window once, afterwards, with the corrected anchor —
     * every time, whether this moved the anchor or not, which is why it has
     * nothing to report back. */
static void drag_cross_screens(FwmServer *server, double lx, double ly) {
    FwmOutput *o = server_output_at(server, lx, ly);
    if (!o) return;

    int offset   = o->camera_x - o->box.x;
    int offset_y = -o->box.y;
    if (!server->interactive.cam_have) {
        server->interactive.cam_output = o;
        /* The anchor is expressed in the frame of the screen DRAWING the
         * window, which is not always the one under the hand — a window can be
         * picked up while it straddles a join. drag_place owns these two from
         * here on; this only gives them their first value. */
        FwmOutput *frame = server->interactive.view ? server->interactive.view->drawn_on : NULL;
        if (!frame) frame = o;
        server->interactive.cam_offset   = frame->camera_x - frame->box.x;
        server->interactive.cam_offset_y = -frame->box.y;
        server->interactive.cam_have = 1;
        return;
    }
    if (server->interactive.cam_output == o) return;
    server->interactive.cam_output = o;

    /* ONLY ACROSS A BREAK IN THE WORLD.
     *
     * Where the two screens show columns that run on from one another, the
     * window needs no help from the hand: it walks across the join under the
     * drag's own arithmetic, and drag_place re-frames it in the one placement
     * where the picture changes screens. Carrying it here as well would be the
     * same move made twice, a grab-offset apart — the hand's crossing first,
     * the picture's later — and that pair of half-corrections IS the jerk.
     *
     * Where they do not run on — the screen on the right showing desktop 5
     * while the left shows 0 — walking across is not available: the world
     * between them is a column nothing displays, and a window walked into it
     * disappears out from under the cursor carrying it. That one is handed
     * over the moment the hand is, because there is no other moment. */
    int carry = offset - server->interactive.cam_offset;
    if (!carry) return;

    int carry_y = offset_y - server->interactive.cam_offset_y;
    server->interactive.cam_offset   = offset;
    server->interactive.cam_offset_y = offset_y;
    server->interactive.view_start_x += carry;
    server->interactive.view_start_y += carry_y;
    /* The world moved, the window did not, and telling the wobble otherwise
     * hands it a whole screen of travel in one tick. */
    if (server->interactive.view)
        view_jelly_carry(server->interactive.view, carry, carry_y);
}

/* The camera moved under a drag (edge auto-scroll, above all): bring the window
 * along. The crossing above is settled here too, for a hand that leaves one
 * monitor without a motion event to say so — a screen unplugged, a camera
 * sliding out from under a cursor standing still.
 *
 * THE CARRYING ITSELF IS drag_place'S, ALL OF IT, AND ONLY ITS.
 *
 * This used to add a correction of its own as well: the camera's travel since
 * the last tick, measured against a reference of its own, added to the anchor.
 * That was right when drag_place knew only about the hand. It stopped being
 * right when drag_place learned to re-frame the anchor against the screen
 * DRAWING the window, because a camera sliding under a stationary hand moves
 * that frame by exactly the same amount — so the two were one correction
 * applied twice, and the window ran away from the hand at precisely the
 * camera's speed. Push a window into the screen edge and by the end of the
 * slide it was a full screen width past the cursor, off the edge you were
 * pushing towards, sitting in the desktop AFTER the one you had arrived on —
 * which is where it landed if you let go.
 *
 * One movement of the world, one correction, in the placement that reads it. */
void server_drag_follow_camera(FwmServer *server) {
    if (server->interactive.action != FWM_ACTION_MOVE || !server->interactive.view) return;
    if (!server->cursor) return;

    drag_cross_screens(server, server->cursor->x, server->cursor->y);

    /* Placed every tick, not only when the camera moved. The frame the anchor
     * is read in (drag_place) can change with the hand perfectly still — the
     * OTHER monitor switching desktops is enough to hand this window's column
     * to a different screen — and nothing else would notice until the hand
     * moved again. The call is idempotent: the same cursor and the same anchor
     * give the same position. */
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
 * It comes off at one size whatever it was ([tiling] pickup), held so the hand
 * keeps the same grip on it: shrinking a full-height tile under a cursor that
 * stays where it was would otherwise leave the window hanging off a corner it
 * was not picked up by.
 *
 * One size and not the geometry it had before the desktop was tiled, which is
 * what this used to restore. That answer was only ever available to a window
 * that had lived on the desktop BEFORE it went tiling: one that opened onto a
 * tiled desktop never had a pre-tiling size to go back to, so it came off the
 * tree still wearing its slot — a full-height sliver, or most of the screen —
 * which is the shape you are dragging it out of in the first place. */
static void tile_tear_out(FwmServer *server, FwmView *view, PhysicsBody *pb,
                          double wx, double wy) {
    int d = pb->desktop_id;

    bsp_remove(&server->bsp_roots[d], view->id);
    pb->tiled = 0;
    pb->vx = 0; pb->vy = 0; pb->flying = 0;
    view->tile_anim = 0;

    double fx = view->width  > 0 ? (wx - view->x) / view->width  : 0.5;
    double fy = view->height > 0 ? (wy - view->y) / view->height : 0.5;

    double pickup = server->config.tiling.pickup;
    if (pickup > 0.0) {
        int w = (int)lround(pickup * server->screen_width);
        int h = (int)lround(pickup * server->screen_height);
        int mw, mh;
        view_min_size(view, &mw, &mh);
        if (w < mw) w = mw;
        if (h < mh) h = mh;
        pb->width  = view->width  = w;
        pb->height = view->height = h;
        view_set_size(view, view->width, view->height);
    } else if (pb->tiling_saved && pb->tile_sav_w > 0 && pb->tile_sav_h > 0) {
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

    /* Whatever is in the hand now is a drop, and rounds off into one over the
     * next few frames. Told where it was taken hold of for the same reason the
     * wobble is: that is the part of the sheet the hand has. */
    view_droplet_begin(view, wx - pb->x, wy - pb->y);

    /* The desktop it LEFT, laid out again without it. */
    server_apply_tiling(server, d);
}

/* Put a carried window down on a tiling desktop: beside the window it was
 * dropped on, on the side of that window the cursor is nearest to. Dropped on
 * the gaps, or on an empty desktop, it simply joins the layout wherever
 * bsp_insert would have put it.
 *
 * `spread` says the thing being put down is a drop, and so should arrive by
 * spreading into its slot rather than gliding to it. Only a window that came
 * OUT of a layout is one: carry a window in from a physics desktop and it was
 * never round, so rounding it off for a fifth of a second on the way in would
 * be an effect about nothing. It just takes its slot. */
static void tile_drop(FwmServer *server, FwmView *view, int d, double wx, double wy,
                      bool spread) {
    /* The shape in the hand, before the layout resizes it into a slot. */
    int drop_w = view->width, drop_h = view->height;

    BspNode *leaf = bsp_leaf_at(server->bsp_roots[d], wx, wy);
    if (leaf && leaf->aw > 0 && leaf->ah > 0) {
        bsp_insert_at(&server->bsp_roots[d], leaf->id, view->id,
                      server_tile_side_at(leaf, wx, wy));
    } else {
        bsp_insert(&server->bsp_roots[d], 0, view->id);
    }
    server_apply_tiling(server, d);

    /* The slot is where the window goes; the fill is how it gets there. So the
     * tile glide that would otherwise carry it across is finished here and now
     * — the window is already home, and everything still moving is a picture of
     * it spreading out from where your hand let go. */
    PhysicsBody *pb = physics_find_body(&server->physics, view->id);
    if (spread && pb && server->config.effects.droplet > 0.0) {
        if (view->tile_anim) {
            pb->x = view->tile_tx;
            pb->y = view->tile_ty;
            view->x = (int)lround(pb->x);
            view->y = (int)lround(pb->y);
            view->tile_anim = 0;
            if (view->scene_tree)
                wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
            view_sync_position(view);
        }
        view_droplet_fill(view, wx - pb->x, wy - pb->y, drop_w, drop_h);
    }
}

/* Motion while a gesture is held. False when there is none, and the caller
 * then does the ordinary hover-and-focus work. */
bool server_drag_motion(FwmServer *server, double lx, double ly,
                        const struct timespec *nowp) {
    struct timespec now = *nowp;
    (void)now;

    /* Carrying the star. It goes exactly where the hand goes, and the hand's
     * speed is remembered so that letting go is a throw. */
    if (server->star_drag) {
        double wx, wy;
        if (!server_screen_to_world(server, lx, ly, &wx, &wy)) return true;
        double dt = 1.0 / PHYSICS_TICK_RATE;
        double nvx = (wx - server->star_drag_x) / dt;
        double nvy = (wy - server->star_drag_y) / dt;
        /* Smoothed, or the throw takes its speed from the last single frame —
         * which is often the one where the hand had already stopped. */
        server->star_drag_vx = server->star_drag_vx * 0.7 + nvx * 0.3;
        server->star_drag_vy = server->star_drag_vy * 0.7 + nvy * 0.3;
        server->star.wx = wx;
        server->star.wy = wy;
        server->star_drag_x = wx;
        server->star_drag_y = wy;
        return true;
    }

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

        /* Before the placement, not after: this is what keeps the seam of a
         * crossing invisible — see drag_cross_screens. */
        drag_cross_screens(server, lx, ly);
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
            /* But NOT an edge with another monitor against it. That strip of
             * ten pixels is the last thing the hand crosses on its way to the
             * screen next door, and turning it into a desktop switch means the
             * window can never get there: you reach for the other monitor and
             * the one you are on flicks to the next desktop instead, over and
             * over. An edge facing open air is still a place to push a window
             * off the end of the strip — that is what this is for — but an
             * edge facing glass belongs to the crossing. */
            if (step) {
                double probe = step > 0 ? eo->box.x + eo->box.width + 1
                                        : eo->box.x - 1;
                if (server_output_at(server, probe, ly)) step = 0;
            }
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

        /* The edges the hand has hold of move; the opposite ones stay put. A
         * left or top grab therefore both resizes AND moves the window, which
         * is the whole difference between an edge you can drag and a corner
         * that drags the far one instead. */
        int start_w = server->interactive.view_start_width;
        int start_h = server->interactive.view_start_height;
        int new_w = server->interactive.resize_left ? start_w - dx : start_w + dx;
        int new_h = server->interactive.resize_top  ? start_h - dy : start_h + dy;

        /* Room to grow reaches the edge of the MONITOR showing the window's own
         * desktop, not of the world: view_start_x is a world coordinate, so on
         * desktop N it is already N screens along and a plain screen_width - x
         * is negative there. That negative then beat the 50px floor below (the
         * ceiling was applied second) and a negative width tripped an assertion
         * inside wlroots — resizing any window off desktop 0 killed the
         * compositor. And a column is the size of the PRIMARY monitor, so on a
         * smaller screen the last stretch of it is glass nobody has: growing
         * into it put the corner the hand was holding past the edge, where
         * server_views_clip cuts it away. */
        int desk = server->interactive.view_start_x / server->screen_width;
        if (desk < 0) desk = 0;
        if (desk >= FWM_DESKTOPS) desk = FWM_DESKTOPS - 1;
        FwmOutput *rmon = server_output_showing(server, desk);
        int lim_w = rmon && rmon->box.width  > 0 ? rmon->box.width  : server->screen_width;
        int lim_h = rmon && rmon->box.height > 0 ? rmon->box.height : server->screen_height;
        int col_x = desk * server->screen_width;

        int max_w = server->interactive.resize_left
                  ? server->interactive.view_start_x + start_w - col_x
                  : col_x + lim_w - server->interactive.view_start_x;
        int max_h = server->interactive.resize_top
                  ? server->interactive.view_start_y + start_h
                  : lim_h - server->interactive.view_start_y;

        if (new_w > max_w) new_w = max_w;
        if (new_h > max_h) new_h = max_h;
        /* The floor is applied last, so a window standing right at an edge —
         * where the room left is under 50px — still keeps a usable size and
         * simply overhangs. */
        if (new_w < 50) new_w = 50;
        if (new_h < 50) new_h = 50;

        /* An edge that does not move is an edge that does not move.
         *
         * Which means the position has to be measured back from the far edge
         * using the size the window is actually DRAWN at — not the size we are
         * asking for. With the rubber up the two are the same, because the
         * picture is stretched to exactly what was asked. Without it the window
         * is whatever the client last committed, and anchoring the far edge on
         * the request instead put it out by the difference: the side nobody was
         * touching crept toward the corner in the hand and away from it again,
         * every time the client answered. */
        int draw_w = new_w, draw_h = new_h;
        if (!view->rub_buf) {
            int cw, ch;
            view_committed_size(view, &cw, &ch);
            if (cw > 0) draw_w = cw;
            if (ch > 0) draw_h = ch;
            /* The clamps above bound what we ASK for, and a client is free to
             * answer with something bigger — a minimum it never declared. The
             * position is measured back from the far edge using this size, so
             * an unbounded answer walks the near edge out of the desktop
             * altogether: drag the left edge fast on a window near the join
             * and it crossed the column boundary, where the desktop is decided
             * from the body's centre, and the window changed desktop under the
             * hand and came back when the client caught up. */
            if (draw_w > max_w) draw_w = max_w;
            if (draw_h > max_h) draw_h = max_h;
        }
        int new_x = server->interactive.resize_left
                  ? server->interactive.view_start_x + start_w - draw_w
                  : server->interactive.view_start_x;
        int new_y = server->interactive.resize_top
                  ? server->interactive.view_start_y + start_h - draw_h
                  : server->interactive.view_start_y;

        view->x = new_x;
        view->y = new_y;
        view->width = draw_w;
        view->height = draw_h;

        /* Only when it actually changed. Motion events arrive every couple of
         * milliseconds and a client redraws at its own pace; re-sending the
         * size it is already working on adds a configure to the queue it is
         * already behind, and the window falls further behind the hand. */
        if (new_w != server->interactive.sent_w || new_h != server->interactive.sent_h) {
            view_set_size(view, new_w, new_h);
            server->interactive.sent_w = new_w;
            server->interactive.sent_h = new_h;
        }
        /* And on screen it is that size NOW, whatever the client has managed
         * to draw. Nothing else here waits for the client, so nothing else
         * should look like it does. */
        view_rubber_to(view, new_w, new_h);
        physics_sync_body(&server->physics, view->id, view->x, view->y,
                          view->width, view->height, server->screen_width);
        if (view->scene_tree) server_place_view(server, view, view->x, view->y);
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
            /* The gesture is over without a button ever coming up — the tree
             * under it was freed by a window closing or the desktop leaving
             * tiling. The pictures still have to be handed back, or the
             * windows they stand in front of stay frozen for good. */
            server_tile_rubber_settle(server);
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
                                        view->y + view->height / 2.0, 0, &cx, &cy))
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

    /* The star, before anything else on the desktop.
     *
     * Its own node refuses input — it has to, or it would swallow every click
     * meant for the windows under it — so being able to pick it up has to be
     * asked for here, by where the press landed rather than by what the scene
     * says is there. Grabbing it takes precedence over the window behind it:
     * if you are pointing at a star, you meant the star. */
    if (server->star_running && server->config.star.enabled &&
        server->star.phase != STAR_COLLAPSE) {
        double sr = star_radius(&server->star, &server->config.star);
        /* Aim at what is DRAWN: a hole's shadow is far bigger than its
         * horizon, and nobody can be asked to hit the horizon. */
        double reach = server->star.phase == STAR_HOLE ? sr * 2.6 : sr;
        double dx = wx - server->star.wx, dy = wy - server->star.wy;
        if (dx * dx + dy * dy <= reach * reach) {
            star_grab(&server->star);
            server->star_drag = 1;
            server->star_drag_x = wx;
            server->star_drag_y = wy;
            server->star_drag_vx = 0.0;
            server->star_drag_vy = 0.0;
            return true;
        }
    }
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
                                                  view->y + view->height / 2.0, 0,
                                                  &cx, &cy)) {
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
                                /* Every window this divider resizes is drawn
                                 * at the size the layout asks for from here
                                 * until the release, the same way a floating
                                 * window is; the clients catch up behind. */
                                server_tile_rubber_begin(server, hn, vn);
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
                        /* The nearest corner, from anywhere inside the window —
                         * the same grab the tiling branch above takes, and the
                         * one it has always claimed a floating window got. */
                        server->interactive.resize_left =
                            wx < view->x + view->width  / 2.0;
                        server->interactive.resize_top =
                            wy < view->y + view->height / 2.0;
                        /* Pinned to the screen it is being resized ON, so
                         * that growing it across a join does not hand it to
                         * another monitor mid-drag (server_view_frame). */
                        server->interactive.cam_output = view->drawn_on;
                        server->interactive.sent_w = view->width;
                        server->interactive.sent_h = view->height;
                        /* The window is drawn at the size the hand asks for
                         * from here until the release; the client catches up
                         * underneath. */
                        view_rubber_begin(view);
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
    if (server->star_drag) {
        /* Thrown, not placed: it leaves with whatever the hand was doing. */
        star_release(&server->star, server->star_drag_vx, server->star_drag_vy);
        server->star_drag = 0;
        return;
    }

        // Button release
        if (server->interactive.action == FWM_ACTION_MOVE) {
            FwmView *view = server->interactive.view;
            /* Was what is being put down a drop? Only a window taken out of a
             * layout ever is, and the answer has to be taken before the clear
             * below wipes it — it is what tells a window going back into a tree
             * from one arriving from a physics desktop, which was never round
             * and must not become round on the way in. */
            bool was_drop = view && view_is_droplet(view);
            /* Let the wobble ring itself out — whatever the drop turns out to
             * be, the hand is off the window. */
            if (view) view_jelly_release(view);
            /* And it is not a drop any more either — unless it lands in a
             * layout, and tile_drop below says so by arming the fill. */
            if (view) view_droplet_clear(view);
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
                            tile_drop(server, view, d, wx, wy, was_drop);
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
        
        /* The hand is off a resize. The client is still answering the last size
         * it was asked for, so the window is not settled yet: the far edges are
         * held where the grab left them, and the stretched picture stays up
         * until the client's next frame (view_resize_settle). */
        if (server->interactive.action == FWM_ACTION_RESIZE && server->interactive.view) {
            view_resize_settle(server->interactive.view,
                               server->interactive.resize_left,
                               server->interactive.resize_top,
                               server->interactive.view_start_x
                                   + server->interactive.view_start_width,
                               server->interactive.view_start_y
                                   + server->interactive.view_start_height);
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

        if (settle_d >= 0) {
            server_apply_tiling(server, settle_d);
            /* And only now the pictures the tiles were drawn from are let go —
             * after that last layout, so what each window waits for is the
             * answer to the size it has just been asked for and not to one
             * from the middle of the drag. They come down one at a time, as
             * the answers arrive; dropping them here would be the jump this
             * whole mechanism exists to remove. */
            server_tile_rubber_settle(server);
        }
}
