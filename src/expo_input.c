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

#include "expo_internal.h"
#include "snapshot.h"
#include "physics.h"
#include "theme.h"
#include "server_internal.h"
#include "ui/expo_menu.h"
#include <math.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

/* ── input ────────────────────────────────────────────────────────────── */

static ExpoItem *expo_item_at(FwmExpo *e, double lx, double ly) {
    /* Through the inverse projection, not by testing projected boxes: a turned
     * card's window is a trapezoid on screen, and there is exactly one place
     * that knows how the strip is bent. Back in world coordinates the test is
     * the same rectangle it always was.
     *
     * Later items are drawn on top, so the last match is the one the cursor is
     * actually pointing at. */
    int d;
    double wx, wy;
    expo_point(e, lx, ly, &d, &wx, &wy);

    ExpoItem *hit = NULL;
    for (int i = 0; i < e->n_items; i++) {
        ExpoItem *it = &e->items[i];
        if (it->desktop != d) continue;
        if (wx >= it->wx && wx < it->wx + it->w &&
            wy >= it->wy && wy < it->wy + it->h) hit = it;
    }
    return hit;
}

/* Keep the strip within reach: half a screen of overscroll past either end,
 * which is enough to see that there is nothing further and not enough to get
 * lost in. */
void expo_clamp_pan(FwmExpo *e) {
    double pitch = expo_pitch(e);

    if (e->server->config.camera.wrap) {
        /* A ring has no ends to stop at: keep going and the tenth desktop is
         * followed by the first. Only the accumulated number is bounded, and
         * folding it by a whole circumference is invisible — every card is
         * placed modulo that same circumference. */
        double lap = expo_lap(e);
        while (e->pan_target >  lap / 2.0) { e->pan_target -= lap; e->pan -= lap; }
        while (e->pan_target < -lap / 2.0) { e->pan_target += lap; e->pan += lap; }
        return;
    }

    double half = e->server->screen_width / (2.0 * expo_scale(e));
    double lo = -half - e->home * pitch;
    double hi = (FWM_DESKTOPS - 1 - e->home) * pitch + half;
    if (e->pan_target < lo) e->pan_target = lo;
    if (e->pan_target > hi) e->pan_target = hi;
}

static const char *expo_mode_name(FwmServer *server, int d) {
    if (d < 0 || d >= FWM_DESKTOPS) return "physics";
    switch (server->desktop_mode[d]) {
    case DESKTOP_MODE_TILING:   return "tiling";
    case DESKTOP_MODE_FLOATING: return "floating";
    default:                    return "physics";
    }
}

/* Put the dragged window where the cursor is. Through expo_point, so it works
 * at any curvature and on either side of the seam. */
void expo_drag_to(FwmExpo *e, double lx, double ly) {
    if (!e->drag) return;
    int d;
    double wx, wy;
    expo_point(e, lx, ly, &d, &wx, &wy);
    e->drag->wx = wx - e->drag_off_x;
    e->drag->wy = wy - e->drag_off_y;
    e->drag->desktop = d;
}

/* Sends the strip; expo_tick brings it. */
static void expo_pan_by(FwmExpo *e, double strip_px) {
    e->pan_target += strip_px;
    expo_clamp_pan(e);
}

bool expo_goto_desktop(FwmServer *server, int d) {
    FwmExpo *e = server->expo;
    if (!e || e->leaving) return false;
    if (d < 0 || d >= FWM_DESKTOPS) return true;   /* consumed; nowhere to go */
    /* Asking for a desktop is asking to work, so the camera comes level. */
    expo_camera_home(e);

    /* Centre desktop d, by panning rather than by moving camera_x.
     *
     * Moving the camera is what everything ELSE that jumps desktops does, and
     * it would half work here — the strip rides camera_x, so it would slide the
     * right way — but the gaps are anchored on the desktop the strip was
     * entered from, so the landing would be off by (d - home) gaps: most of a
     * card by the far end of the strip. Pan is exact, and it is already eased.
     *
     * strip x of desktop d's centre is d * pitch + screen_width / 2, and
     * expo_center is camera_x + screen_width/2 + home * gap + pan, with
     * camera_x == home * screen_width — which leaves this. */
    e->pan_target = (d - e->home) * expo_pitch(e);
    if (server->config.camera.wrap) {
        /* Round the short way. Stepping from desktop 9 to 0 must travel one
         * card, not nine backwards — the same rule server_goto_desktop follows
         * outside the strip, except that here it is a movement and not a jump,
         * because on a ring there is a picture of the way round. */
        double lap = expo_lap(e);
        while (e->pan_target - e->pan >  lap / 2.0) e->pan_target -= lap;
        while (e->pan_target - e->pan < -lap / 2.0) e->pan_target += lap;
    }
    expo_clamp_pan(e);
    return true;
}

bool expo_handle_key(FwmServer *server, xkb_keysym_t sym) {
    FwmExpo *e = server->expo;
    if (!e) return false;

    switch (sym) {
    case XKB_KEY_Escape:
        expo_toggle(server);
        return true;
    case XKB_KEY_z:
    case XKB_KEY_Z:
        expo_zoom_step(server);
        return true;
    case XKB_KEY_minus:
    case XKB_KEY_equal:
    case XKB_KEY_plus:
        /* The size of the thing in the middle. What looks right depends on the
         * screen and on how far back the camera is, so it is a knob here
         * rather than a number in a file. */
        if (!e->orrery) return true;
        expo_orrery_resize(server, sym == XKB_KEY_minus ? 1.0 / 1.15 : 1.15);
        return true;
    case XKB_KEY_comma:
    case XKB_KEY_period:
        /* Turn the disc's plane — the one that reads as rotating the object. */
        if (!e->orrery) return true;
        expo_orrery_roll(server, sym == XKB_KEY_comma ? -0.13 : 0.13);
        return true;
    case XKB_KEY_p:
    case XKB_KEY_P:
        /* Orbits on or off. With them off the orrery is a plain turning ring,
         * which is a perfectly good thing to look at and much easier to aim a
         * click at — hence a switch rather than a decision made for you. */
        if (!e->orrery) return true;
        expo_orbits_toggle(server);
        return true;
    case XKB_KEY_o:
    case XKB_KEY_O:
        /* The orrery. Closes the ring if it is open, because planets that stop
         * at a wall are not planets. */
        expo_orrery_toggle(server);
        return true;
    case XKB_KEY_x:
    case XKB_KEY_X:
        /* Close the strip into a ring, or open it back into a line. The same
         * action the `toggle_wrap` bind runs outside the strip — this is a
         * shortcut to it, not a second meaning for it. */
        server_dispatch_action(server, "toggle_wrap");
        return true;
    /* Turning a keyboard knob walks the strip and pressing it steps into the
     * desktop under the eye — the same two verbs the ring uses, so the hand
     * learns one thing and it holds in both places. The knob's keys are the
     * volume keys, which is why server_input.c has to take them before the
     * binds do; see the gate there. */
    case XKB_KEY_Left:
    case XKB_KEY_XF86AudioLowerVolume:
        expo_pan_by(e, -expo_pitch(e) * (sym == XKB_KEY_Left
                                         ? 1 : server_knob_step(server, -1)));
        return true;
    case XKB_KEY_Right:
    case XKB_KEY_XF86AudioRaiseVolume:
        expo_pan_by(e, expo_pitch(e) * (sym == XKB_KEY_Right
                                        ? 1 : server_knob_step(server, +1)));
        return true;
    case XKB_KEY_Up:
    case XKB_KEY_Down: {
        /* Lift the camera over the ring. Only at the far step, where the strip
         * is something to look at — nearer, the arrows have nothing to do and
         * saying so by ignoring them is better than tilting the desktop the
         * user is about to click on. */
        if (!expo_can_orbit(e)) return true;
        double step = sym == XKB_KEY_Up ? EXPO_TILT_STEP : -EXPO_TILT_STEP;
        e->tilt_target += step;
        if (e->tilt_target >  EXPO_TILT_MAX) e->tilt_target =  EXPO_TILT_MAX;
        if (e->tilt_target < -EXPO_TILT_MAX) e->tilt_target = -EXPO_TILT_MAX;
        return true;
    }
    case XKB_KEY_Page_Up:
    case XKB_KEY_Page_Down: {
        if (!expo_can_orbit(e)) return true;
        e->dist_target *= sym == XKB_KEY_Page_Up ? 1.0 / EXPO_DIST_STEP
                                                 : EXPO_DIST_STEP;
        if (e->dist_target < EXPO_DIST_MIN) e->dist_target = EXPO_DIST_MIN;
        if (e->dist_target > EXPO_DIST_MAX) e->dist_target = EXPO_DIST_MAX;
        return true;
    }
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_XF86AudioMute:
        expo_close(server, expo_view_desktop(server));
        return true;
    default:
        break;
    }
    /* Not one of ours. Says so rather than claiming the key: the strip swallows
     * everything either way (server_input.c holds the keyboard while it is up,
     * exactly as the launcher does, so no stray key reaches a client that
     * cannot be seen) — but the caller needs the answer to know whether to try
     * the same key again in another layout. */
    return false;
}

bool expo_handle_motion(FwmServer *server, double lx, double ly) {
    FwmExpo *e = server->expo;
    if (!e) return false;
    if (e->leaving) return true;

    if (e->orbiting) {
        double dx = lx - e->orbit_x, dy = ly - e->orbit_y;
        e->orbit_x = lx;
        e->orbit_y = ly;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        e->orbit_dt = (double)(now.tv_sec - e->orbit_time.tv_sec)
                    + (double)(now.tv_nsec - e->orbit_time.tv_nsec) / 1e9;
        e->orbit_time = now;

        /* Sideways is the pan the strip always had — turning the ring under a
         * fixed camera is the same thing as flying round it, and reusing it
         * keeps one notion of where the strip is looking. */
        double s = expo_scale(e);
        if (s > 1e-6) {
            double step = -dx / s;
            e->pan_target += step;
            expo_clamp_pan(e);
            e->pan = e->pan_target;      /* a hand is steering; no easing */
            /* Remember how fast it was going, for the throw. Smoothed, because
             * the last motion event before a release is often a stray pixel and
             * would otherwise decide the whole spin. */
            double dt = e->orbit_dt > 0.001 ? e->orbit_dt : 0.016;
            e->spin = e->spin * 0.6 + (step / dt) * 0.4;
        }
        e->tilt_target += dy * EXPO_TILT_STEP / 40.0;
        if (e->tilt_target >  EXPO_TILT_MAX) e->tilt_target =  EXPO_TILT_MAX;
        if (e->tilt_target < -EXPO_TILT_MAX) e->tilt_target = -EXPO_TILT_MAX;
        e->tilt = e->tilt_target;
        return true;
    }

    if (e->menu) {
        /* The menu is modal over the cards: while it is open the cursor is
         * choosing a row, not pointing at windows. */
        int mw, mh;
        expo_menu_size(&mw, &mh);
        double mx = lx - e->menu->node.x, my = ly - e->menu->node.y;
        expo_menu_hover(e->menu, (mx >= 0 && mx < mw && my >= 0 && my < mh)
                                     ? expo_menu_hit(mx, my) : EXPO_MENU_ROW_NONE);
        return true;
    }

    if (e->drag) {
        expo_drag_to(e, lx, ly);
    } else {
        e->hover = expo_item_at(e, lx, ly);
    }
    expo_layout(e);
    return true;
}

/* Put a dragged window where its card was dropped. */
static void expo_drop(FwmServer *server, ExpoItem *it) {
    FwmView *view = it->view;
    int d = it->desktop;

    PhysicsBody *b = physics_find_body(&server->physics, view->id);
    int src = b ? b->desktop_id : view->x / server->screen_width;
    if (d != src) server_move_view_to_desktop(server, view, d, 0);

    /* A tiled desktop owns its geometry — server_move_view_to_desktop has
     * already inserted the window into the layout, and placing it by hand here
     * would only fight the tiler on the next tick. */
    if (server->desktop_mode[d] == DESKTOP_MODE_TILING) {
        /* Snap the card back onto whatever the layout decided. */
        it->wx = view->x;
        it->wy = view->y;
        return;
    }

    double max_y = server->screen_height - it->h;
    if (it->wy < 0) it->wy = 0;
    if (it->wy > max_y) it->wy = max_y > 0 ? max_y : 0;
    double min_x = (double)d * server->screen_width;
    double max_x = min_x + server->screen_width - it->w;
    if (it->wx < min_x) it->wx = min_x;
    if (it->wx > max_x) it->wx = max_x > min_x ? max_x : min_x;

    view->x = (int)lround(it->wx);
    view->y = (int)lround(it->wy);
    view->tile_anim = 0;
    if (b) {
        b->x = it->wx;
        b->y = it->wy;
        b->vx = 0; b->vy = 0; b->flying = 0;
    }
    view_sync_position(view);
}

bool expo_handle_button(FwmServer *server, uint32_t button, bool pressed,
                        double lx, double ly) {
    FwmExpo *e = server->expo;
    if (!e) return false;
    if (e->leaving) return true;

    /* A menu on screen owns the next click wherever it lands: on a row it acts,
     * anywhere else it dismisses — and dismissing does NOT also do whatever the
     * click would have meant, which is the rule every other menu here follows. */
    if (e->menu) {
        if (!pressed) return true;
        int mw, mh;
        expo_menu_size(&mw, &mh);
        double mx = lx - e->menu->node.x, my = ly - e->menu->node.y;
        int row = (mx >= 0 && mx < mw && my >= 0 && my < mh)
                      ? expo_menu_hit(mx, my) : EXPO_MENU_ROW_NONE;
        ExpoItem *target = e->menu_item;

        if (row == EXPO_MENU_ROW_MODE && target) {
            /* A cycle stays open: clicking three times should walk the three
             * modes, not open the menu three times. */
            int d = target->desktop;
            int next = server->desktop_mode[d] == DESKTOP_MODE_PHYSICS
                           ? DESKTOP_MODE_TILING
                     : server->desktop_mode[d] == DESKTOP_MODE_TILING
                           ? DESKTOP_MODE_FLOATING : DESKTOP_MODE_PHYSICS;
            server_set_desktop_mode(server, d, next);
            expo_refresh_desktop(server, d);
            expo_menu_set_mode(e->menu, expo_mode_name(server, d));
            server_request_tray_redraw(server);
            return true;
        }

        expo_menu_close(e);
        if (row == EXPO_MENU_ROW_GOTO && target) {
            server_focus_view(server, target->view);
            server->focus_desktop = target->desktop;
            expo_close(server, target->desktop);
        } else if (row == EXPO_MENU_ROW_CLOSE && target) {
            /* Ask, do not force: the card stays until the client actually
             * unmaps, and expo_forget_view is what takes it off the strip. */
            view_send_close(target->view);
        }
        return true;
    }

    struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
    uint32_t mods = kb ? wlr_keyboard_get_modifiers(kb) : 0;

    /* Fly round the ring: sideways turns it, up and down lifts the camera over
     * it. Two ways in, because one of them assumes hardware: the middle button
     * for a mouse that has one, and alt+left for a touchpad, which is the
     * gesture every 3D viewer has used for thirty years. Super+left is taken —
     * that carries windows — and the right button opens their menu. */
    if (button == BTN_MIDDLE
        || (button == BTN_LEFT && (mods & WLR_MODIFIER_ALT) && expo_can_orbit(e))) {
        if (!pressed) { e->orbiting = 0; return true; }
        if (!expo_can_orbit(e)) return true;
        e->orbiting = 1;
        e->orbit_x = lx;
        e->orbit_y = ly;
        e->orbit_dt = 0.0;
        clock_gettime(CLOCK_MONOTONIC, &e->orbit_time);
        e->spin = 0.0;               /* catching it stops it dead */
        return true;
    }

    /* A release always ends the flight, whichever button began it. */
    if (!pressed && e->orbiting) { e->orbiting = 0; return true; }

    if (button == BTN_RIGHT) {
        if (pressed) {
            ExpoItem *it = expo_item_at(e, lx, ly);
            if (it) {
                e->menu = expo_menu_show(server->layer_overlay,
                                         server->screen_width, server->screen_height,
                                         lx, ly, view_title(it->view),
                                         expo_mode_name(server, it->desktop));
                e->menu_item = e->menu ? it : NULL;
                e->hover = it;
                expo_layout(e);
            }
        }
        return true;
    }

    if (button != BTN_LEFT) return true;

    if (!pressed) {
        if (e->drag) {
            expo_drop(server, e->drag);
            e->drag = NULL;
            expo_layout(e);
        }
        return true;
    }

    ExpoItem *it = expo_item_at(e, lx, ly);
    if (it && (mods & WLR_MODIFIER_LOGO)) {
        /* Super+drag moves the window, including across desktops — the reason
         * the strip is a window manager and not a screenshot. */
        int d;
        double wx, wy;
        expo_point(e, lx, ly, &d, &wx, &wy);
        e->drag = it;
        e->drag_off_x = wx - it->wx;
        e->drag_off_y = wy - it->wy;
        e->hover = it;
        /* Only the flat path has a node to raise. On the curved one there is
         * no scene node for a window at all — the strip is drawn by hand, and
         * expo_draw_gl already paints the dragged card last. Raising the NULL
         * was a segfault the moment a window was picked up in 3D, and one that
         * left nothing in the log to find it by. */
        if (it->node) wlr_scene_node_raise_to_top(&it->node->node);
        return true;
    }

    if (it) {
        server_focus_view(server, it->view);
        server->focus_desktop = it->desktop;
        expo_close(server, it->desktop);
        return true;
    }

    /* Empty space: go to the desktop that was clicked. */
    int d;
    double wx, wy;
    expo_point(e, lx, ly, &d, &wx, &wy);
    expo_close(server, d);
    return true;
}

bool expo_handle_axis(FwmServer *server, double delta) {
    FwmExpo *e = server->expo;
    if (!e) return false;
    if (e->orbiting) {
        /* Mid-flight the wheel is the one thing left to ask for: closer or
         * further. Panning as well would fight the hand already turning it. */
        e->dist_target *= delta > 0.0 ? EXPO_DIST_STEP : 1.0 / EXPO_DIST_STEP;
        if (e->dist_target < EXPO_DIST_MIN) e->dist_target = EXPO_DIST_MIN;
        if (e->dist_target > EXPO_DIST_MAX) e->dist_target = EXPO_DIST_MAX;
        return true;
    }
    /* A wheel notch is ~15 units, so a notch is a third of a desktop —
     * enough to feel like scrolling a strip, small enough to aim with. */
    if (!e->leaving) expo_pan_by(e, delta * expo_pitch(e) / 45.0);
    return true;
}
