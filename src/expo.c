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
#include "star_draw.h"
#include "server.h"
#include "view.h"
#include "snapshot.h"
#include "rotate.h"
#include "wallpaper.h"
#include "physics.h"
#include "theme.h"
#include "server_internal.h"
#include "lock.h"
#include "ui/launcher.h"
#include "ui/expo_menu.h"
#include "ui/cairo_overlay.h"
#include "ui/expo_hints.h"
#include "defines.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

void expo_menu_close(FwmExpo *e) {
    if (!e->menu) return;
    cairo_overlay_destroy(e->menu);
    e->menu = NULL;
    e->menu_item = NULL;
}

/* FWM_TEST_ORBIT=<radians> opens the strip already zoomed out and tilted, so a
 * nested run can be photographed from an angle no keypress can be injected to
 * reach; FWM_TEST_ORBIT_DIST pulls the camera back as well.
 *
 * It also round-trips the projection against the ray that inverts it, over a
 * grid of world points on every desktop. That check earns its keep: the ray is
 * the one half of this that a screenshot cannot show, and it caught the two
 * real bugs here — a facet hit from inside the drum, and expo_to_screen
 * placing a point on the arc where the facet is a chord. Points that come back
 * "missed" are the ones the eye cannot see either: occluded, or on the far
 * wall. */
static void expo_selftest(FwmExpo *e) {
    FwmServer *server = e->server;
    if (!getenv("FWM_TEST_ORBIT")) return;

    double worst = 0.0;
    int misses = 0, tried = 0;
    for (int d = 0; d < FWM_DESKTOPS; d++) {
        for (int i = 0; i <= 4; i++) for (int j = 0; j <= 4; j++) {
            double wx = (double)d * server->screen_width
                      + server->screen_width * i / 4.0;
            double wy = server->screen_height * j / 4.0;
            double sx, sy;
            expo_to_screen(e, wx, wy, d, &sx, &sy);
            if (sx < 0 || sx > server->screen_width) continue;
            if (sy < 0 || sy > server->screen_height) continue;
            tried++;
            int gd; double gx, gy;
            if (!expo_point(e, sx, sy, &gd, &gx, &gy) || gd != d) { misses++; continue; }
            double err = fabs(gx - wx) + fabs(gy - wy);
            if (err > worst) worst = err;
        }
    }
    wlr_log(WLR_INFO, "expo: ray round-trip over %d points: worst %.3f px, %d hidden",
            tried, worst, misses);
}

/* ── open and close ───────────────────────────────────────────────────── */

static void expo_teardown(FwmServer *server) {
    /* The orrery's star goes with the strip: it belongs to the view, not to
     * the desktops, and there is nothing to keep it alive for. */
    if (server->expo) {
        star_draw_destroy(server->expo->orrery_draw);
        server->expo->orrery_draw = NULL;
    }

    FwmExpo *e = server->expo;
    if (!e) return;
    server->expo = NULL;    /* before anything else: nothing may re-enter here */

    expo_menu_close(e);
    if (e->hints) cairo_overlay_destroy(e->hints);
    for (int i = 0; i < e->n_items; i++) {
        if (e->items[i].tex) wlr_texture_destroy(e->items[i].tex);
        if (e->items[i].buf) wlr_buffer_unlock(e->items[i].buf);
    }
    for (int i = 0; i < e->wp_n; i++)
        if (e->wp_tex[i]) wlr_texture_destroy(e->wp_tex[i]);
    for (int i = 0; i < 2; i++)
        if (e->canvas_buf[i]) wlr_buffer_unlock(e->canvas_buf[i]);
    if (e->tree) wlr_scene_node_destroy(&e->tree->node);

    /* Never hand the desktop back while the session is locked: lock.c hid
     * these trees for its own reasons and it, not the strip, decides when they
     * come back. */
    if (!lock_is_active(server)) expo_set_world_visible(server, e->out, true);
    free(e);
}

bool expo_active(FwmServer *server) {
    return server->expo != NULL;
}

bool expo_live_active(FwmServer *server) {
    FwmExpo *e = server->expo;
    if (!e || e->leaving || server->config.effects.live <= 0.0) return false;
    int d = expo_view_desktop(server);
    for (int i = 0; i < e->n_items; i++)
        if (e->items[i].desktop == d) return true;
    return false;
}

bool expo_animating(FwmServer *server) {
    FwmExpo *e = server->expo;
    /* The orrery always counts as animating: the ring turns and the hole's
     * disc turns with it, and neither shows up in the deltas below. */
    if (e && e->orrery) return true;
    return e && (fabs(e->zoom - e->zoom_target) > 0.001
              || fabs(e->pan - e->pan_target) > 0.5
              || fabs(e->seam - e->seam_target) > 0.001
              || fabs(e->hints_reveal - e->hints_reveal_target) > 0.002
              || fabs(e->spin) > EXPO_SPIN_MIN
              || fabs(e->tilt - e->tilt_target) > 0.0005
              || fabs(e->dist - e->dist_target) > 0.0005);
}

static void expo_open(FwmServer *server) {
    if (server->expo || server->screen_width <= 0) return;
    /* Not over a lock, and not in the middle of a drag or a resize: the strip
     * takes the pointer away, and the interactive gesture would be left holding
     * a window nobody can reach. The launcher is the same argument the other
     * way round — it owns the keyboard, and two things cannot. */
    if (lock_is_active(server)) return;
    if (server->interactive.action != FWM_ACTION_NONE) return;
    if (launcher_is_open(server->launcher)) return;

    FwmExpo *e = calloc(1, sizeof(*e));
    if (!e) return;
    e->server = server;
    e->zoom = 1.0;
    e->zoom_target = EXPO_ZOOM_NEAR;
    expo_canvas_dirty(e);   /* nothing drawn yet; the first layout must run */
    e->seam = e->seam_target = server->config.camera.wrap ? 0.0 : 1.0;
    e->dist = e->dist_target = 1.0;
    { const char *t = getenv("FWM_TEST_ORBIT");
      if (t) { e->zoom = e->zoom_target = EXPO_ZOOM_FAR; e->tilt = e->tilt_target = atof(t);
               const char *dd = getenv("FWM_TEST_ORBIT_DIST");
               if (dd) e->dist = e->dist_target = atof(dd); } }
    e->out = server_active_output(server);
    if (!e->out) { free(e); return; }
    e->home = e->out->desktop;
    if (e->home < 0) e->home = 0;
    if (e->home >= FWM_DESKTOPS) e->home = FWM_DESKTOPS - 1;

    e->tree = wlr_scene_tree_create(&server->scene->tree);
    if (!e->tree) { free(e); return; }
    /* The whole strip lives on its monitor. */
    wlr_scene_node_set_position(&e->tree->node, e->out->box.x, e->out->box.y);
    /* Above the windows it is replacing, below the chrome it is not — and an
     * external bar is chrome exactly as our own strip is. ls_top is the lowest
     * of the layers a panel actually sits on, so going under it leaves the bar,
     * the tray and a layer-shell overlay all above the strip, and puts only the
     * windows and the wallpaper underneath. */
    wlr_scene_node_place_below(&e->tree->node, &server->ls_top->node);

    e->backdrop = wlr_scene_rect_create(e->tree, server->screen_width,
                                        server->screen_height,
                                        (float[4]){0.0f, 0.0f, 0.0f, 0.0f});

    /* Turned cards need a renderer that can draw a trapezoid. Where there is
     * one, the strip is one hand-drawn buffer over the backdrop; where there is
     * not, it stays the flat set of scene nodes and nothing else changes. */
    e->gl = rotate_supported(server->wlr_renderer);
    if (e->gl) {
        for (int i = 0; i < 2; i++) {
            struct wlr_buffer *b = snapshot_alloc(server, server->screen_width,
                                                  server->screen_height);
            if (!b) { e->gl = false; break; }
            e->canvas_buf[i] = wlr_buffer_lock(b);
            wlr_buffer_drop(b);
        }
        if (e->gl) {
            e->canvas = wlr_scene_buffer_create(e->tree, NULL);
            if (!e->canvas) e->gl = false;
            else wlr_scene_node_set_position(&e->canvas->node, 0, 0);
        }
        if (!e->gl) wlr_log(WLR_INFO, "expo: no offscreen buffer, strip stays flat");
    }

    if (!e->gl) {
        e->hilight = wlr_scene_rect_create(e->tree, 1, 1,
                                           (float[4]){0.35f, 0.62f, 0.95f, 0.9f});
        if (e->hilight) wlr_scene_node_set_enabled(&e->hilight->node, false);
    }

    server->expo = e;
    expo_build_cards(e);
    /* The highlight is a frame BEHIND the window it marks, so the window's own
     * picture covers all but the border. Windows are created after it and
     * therefore already sit above; the cards must not. */
    if (e->hilight) wlr_scene_node_raise_to_top(&e->hilight->node);
    expo_snap_windows(e);

    /* Nothing may be sliding under the strip: the slide is a render-only offset
     * on the very trees about to be hidden, and it would still be there when
     * they came back. */
    server_wrap_slide_stop(server, e->out);
    expo_set_world_visible(server, e->out, false);
    expo_layout(e);

    expo_selftest(e);

    /* Nothing under the cursor belongs to a client any more, and a client that
     * keeps pointer focus would go on receiving motion through the strip. */
    wlr_seat_pointer_notify_clear_focus(server->seat);
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");

    /* The tray stays on screen, but its menu hangs off a pill that clicks can
     * no longer reach while the strip owns the pointer. */
    server_close_modes_menu(server);

    /* The strip's keys are not binds — they belong to the mode and are in
     * nobody's config — so nothing else could tell anyone what they are. */
    e->hints = expo_hints_show(server->layer_overlay, e->out->box.x, e->out->box.y,
                               server->screen_width, server->screen_height,
                               expo_can_orbit(e));
    e->hints_reveal = e->hints_reveal_target = 1.0;
    e->hints_timer = EXPO_HINT_SHOW_S;

    /* FWM_TEST_ORRERY=1 opens straight into the orrery. Its own key is `o` and
     * a nested run cannot be sent one — the headless backend has no keyboard —
     * so without this the ring, the star in the middle of it and everything
     * that happens when the strip closes over the top of them can only be
     * looked at by hand. Same bargain as FWM_TEST_ORBIT above. */
    if (getenv("FWM_TEST_ORRERY")) expo_orrery_toggle(server);
}

void expo_close(FwmServer *server, int desktop) {
    FwmExpo *e = server->expo;
    if (!e || e->leaving) return;
    if (desktop < 0) desktop = 0;
    if (desktop >= FWM_DESKTOPS) desktop = FWM_DESKTOPS - 1;

    e->leaving = 1;
    e->zoom_target = 1.0;
    /* Down with the strip, not after it: the collapse waits for the camera as
     * well as the zoom, and a bar still sitting over the live desktop while
     * that finishes is the part that looked stuck. */
    e->hints_reveal_target = 0.0;
    e->hints_timer = 0.0;
    expo_camera_home(e);
    e->hover = NULL;
    e->drag = NULL;
    expo_menu_close(e);

    /* Put the live camera on the destination NOW, while it is still hidden:
     * the closing animation is written against camera_x (expo_center), so the
     * strip flies to exactly the view that is revealed underneath it, and the
     * two cannot disagree by a frame at the moment of the swap.
     *
     * Moving the camera moves the strip's own centre with it, so `pan` is
     * rebased to cancel that out exactly: the picture must not jump on the
     * frame the destination is chosen, only travel there. */
    double was_looking_at = expo_center(e);
    e->home = desktop;
    server_output_show_desktop(server, e->out, desktop, 0);
    e->out->camera_x = e->out->target_camera_x;
    e->pan = was_looking_at - (e->out->camera_x + server->screen_width / 2.0
                               + e->home * expo_gap(e));
    if (server->config.camera.wrap) {
        /* On a ring the rebased offset can be most of a circle even though the
         * picture did not move; fly the short way home. */
        double lap = expo_lap(e);
        while (e->pan >  lap / 2.0) e->pan -= lap;
        while (e->pan < -lap / 2.0) e->pan += lap;
    }
    e->pan_target = 0.0;
    e->out->cam_anim = 0;
    e->out->cam_free = 0;
    server_camera_settled(server);
    wallpaper_update(e->out->wallpaper, e->out->camera_x);
    server_request_tray_redraw(server);
}

void expo_toggle(FwmServer *server) {
    FwmExpo *e = server->expo;
    if (!e) {
        expo_open(server);
    } else if (e->leaving) {
        /* Caught on the way out: turn round rather than stacking a second
         * strip on top of the one still animating. */
        e->leaving = 0;
        e->zoom_target = EXPO_ZOOM_NEAR;
    } else {
        expo_close(server, expo_view_desktop(server));
    }
}

/* The far zoom step is the looking-at-it step, and the only one the camera may
 * leave the canonical view in. Read from the TARGET, so the controls come
 * alive as soon as `z` is pressed rather than when the zoom finishes. */
/* Turn the orrery on or off.
 *
 * On: the ring is closed (planets go round, they do not stop at a wall), the
 * camera lifts above it and pulls back so the whole circle is in frame, and it
 * starts turning. Off: the turning stops and the camera comes back down. The
 * ring is left closed — that is a setting of the user's, not ours to undo.
 */
void expo_orrery_toggle(FwmServer *server) {
    FwmExpo *e = server->expo;
    if (!e) return;

    e->orrery = !e->orrery;
    if (e->orrery) {
        /* The thing in the middle: a STAR, at the start of its life.
         *
         * Not a black hole from the outset — "star-black hole" is the whole
         * road, not the destination: it burns, it collapses to a dwarf, the
         * dwarf can be pushed to a pulsar and the pulsar to a hole, and each
         * step is worth watching. star_collapse takes it one step further
         * whenever you press it, here as anywhere else.
         *
         * Sized for the middle of a ring of ten desktops rather than for one
         * of them, so it has its own config: the same numbers scaled up. */
        e->orrery_cfg = server->config.star;
        e->orrery_cfg.enabled = 1;
        double size = server->config.star.orrery_size;
        if (size <= 0.0) size = 0.16;
        e->orrery_cfg.radius  = server->screen_height * size;
        /* Light enough that the first collapse leaves an ember: the road down
         * is then three presses long instead of one. */
        e->orrery_cfg.mass    = 1.10;
        /* It is on display, not on a clock: it does not burn out while you
         * are looking at it. */
        e->orrery_cfg.lifetime_s = 1e9;
        if (e->orrery_scale <= 0.0) e->orrery_scale = 1.0;
        /* Tipped out of the ring's plane, and left there.
         *
         * Lying flat in it, the disc is axially symmetric about the very axis
         * you walk around — so every angle shows the identical picture, and no
         * amount of steering changes anything. Tipped, it has a side and a
         * face, and alt+drag is then enough on its own: sideways carries you
         * round it, up and down lifts you over it. That is why the two keys
         * that used to set this angle are gone — they did by hand, badly, what
         * moving the camera does properly. */
        e->orrery_fade = 1.0;
        e->orrery_tilt = 0.62;
        e->orrery_cfg.radius *= e->orrery_scale;
        star_init(&e->orrery_star, &e->orrery_cfg);
        /* Turning, so the disc has a direction and the ring around it agrees
         * with the desktops going the same way. */
        star_spin(&e->orrery_star, 0.35);
        /* No scene node: it is drawn inside expo's own 3D pass, sorted among
         * the desktops by depth like anything else in the ring. */
        if (!e->orrery_draw)
            e->orrery_draw = star_draw_create(server, e->out, NULL, NULL,
                                              &e->orrery_cfg);
        /* Against stars, not against the desktop: the strip has hidden the
         * world it is drawing cards of, so there is nothing behind the hole to
         * photograph — and painting that nothing over the ring is exactly why
         * the desktops disappeared the first time this ran. */
        star_draw_set_lensing(e->orrery_draw, false);
        /* Behind the cards. The desktops orbit AROUND it, so they have to pass
         * in front of it — drawn on top it simply covered the ring, which is
         * the whole of "I cannot see the desktops". A scene node is flat and
         * cannot be half in front, so behind is the answer that is right for
         * the near half of the ring and unnoticeable for the far half. */
        /* Planets need a circle to go round. */
        if (!server->config.camera.wrap)
            server_dispatch_action(server, "toggle_wrap");
        e->orbits = 1;
        e->orbit_blend = 0.0;          /* they set off from the ring */
        e->orbit_blend_target = 1.0;
        expo_orbits_layout(e);
        e->orrery_speed = EXPO_ORRERY_SPEED;
        e->tilt_target = EXPO_ORRERY_TILT;
        e->dist_target = EXPO_ORRERY_DIST;
    } else {
        e->spin = 0.0;
        e->tilt_target = 0.0;
        e->dist_target = 1.0;
        star_draw_destroy(e->orrery_draw);
        e->orrery_draw = NULL;
    }
    /* The panel at the bottom shows the verbs that apply, and inside the
     * orrery none of the usual ones do. */
    if (e->hints) {
        expo_hints_set_orrery(e->hints, e->orrery);
        e->hints_reveal_target = 1.0;
        e->hints_timer = EXPO_HINT_SHOW_S;
    }
    wlr_log(WLR_INFO, "expo: orrery %s", e->orrery ? "running" : "stopped");
    expo_canvas_dirty(e);
}

void expo_orrery_resize(FwmServer *server, double factor) {
    FwmExpo *e = server->expo;
    if (!e || !e->orrery || factor <= 0.0) return;

    double scale = (e->orrery_scale > 0.0 ? e->orrery_scale : 1.0) * factor;
    /* Room to make it a speck or to fill the ring, and no further: past that
     * it is either invisible or it is the only thing on screen. */
    if (scale < 0.15) scale = 0.15;
    if (scale > 6.0)  scale = 6.0;
    e->orrery_scale = scale;

    /* The buffer is sized at creation, so a new size means a new one. The star
     * itself — its phase, its mass, how far through a collapse it is — is
     * untouched: only the picture is rebuilt. */
    double base = server->config.star.orrery_size;
    if (base <= 0.0) base = 0.16;
    star_draw_destroy(e->orrery_draw);
    e->orrery_cfg.radius = server->screen_height * base * scale;
    e->orrery_draw = star_draw_create(server, e->out, NULL, NULL, &e->orrery_cfg);
    star_draw_set_lensing(e->orrery_draw, false);
    expo_canvas_dirty(e);
}

/* Where each desktop's orbit is, and therefore how fast it goes round.
 *
 * A desktop's distance from the axis comes from how many windows are on it: a
 * busy desktop is a heavy planet and sits further out, an empty one has no
 * business claiming an orbit at all and gets a random one. Kepler then does
 * the rest — the rate falls as r^-1.5 — so the crowded desktops sail slowly
 * round the outside while the quiet ones race past inside them.
 *
 * Re-run whenever the mode starts or the windows change, which is why the
 * phases are only seeded for orbits that did not have one. */
void expo_orbits_layout(FwmExpo *e) {
    double lap = expo_lap(e);
    unsigned seed = 0x2545f491u;

    for (int d = 0; d < FWM_DESKTOPS; d++) {
        int n = 0;
        for (int i = 0; i < e->n_items; i++)
            if (e->items[i].desktop == d) n++;

        double r;
        if (n == 0) {
            /* Nothing on it: give it somewhere of its own rather than piling
             * every empty desktop onto one track. */
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            r = 0.40 + 0.85 * ((seed & 0xffff) / 65535.0);
        } else {
            /* Grows quickly at first and then levels off: two windows should
             * look different from none, twenty need not look different from
             * twelve. */
            r = 0.45 + 0.75 * (1.0 - exp(-n / 3.5));
        }
        e->orbit_r[d] = r;
        if (e->orbit_phase[d] == 0.0)
            e->orbit_phase[d] = fmod((d + 1) * lap * 0.382, lap);

        /* The windows on it become its moons, spread round it and turning at
         * their own rates — inner ones faster, as everything else here. */
        int k = 0;
        for (int i = 0; i < e->n_items; i++) {
            ExpoItem *it = &e->items[i];
            if (it->desktop != d) continue;
            double frac = n > 1 ? (double)k / n : 0.0;
            it->moon_a = frac * 2.0 * M_PI;
            it->moon_r = 0.34 + 0.30 * frac;
            it->moon_rate = 0.55 * pow(it->moon_r, -1.5);
            k++;
        }
    }
}

void expo_orbits_toggle(FwmServer *server) {
    FwmExpo *e = server->expo;
    if (!e || !e->orrery) return;
    e->orbits = !e->orbits;
    if (e->orbits) expo_orbits_layout(e);
    e->orbit_blend_target = e->orbits ? 1.0 : 0.0;
    wlr_log(WLR_INFO, "orrery: orbits %s", e->orbits ? "on" : "off");
    expo_canvas_dirty(e);
}

void expo_orrery_roll(FwmServer *server, double delta) {
    FwmExpo *e = server->expo;
    if (!e || !e->orrery) return;
    e->orrery_roll += delta;
    if (e->orrery_roll > 2.0 * M_PI)  e->orrery_roll -= 2.0 * M_PI;
    if (e->orrery_roll < -2.0 * M_PI) e->orrery_roll += 2.0 * M_PI;
    wlr_log(WLR_INFO, "orrery: disc rolled to %.2f rad", e->orrery_roll);
    expo_canvas_dirty(e);
}


void expo_orrery_collapse(FwmServer *server) {
    FwmExpo *e = server->expo;
    if (!e || !e->orrery) {
        wlr_log(WLR_INFO, "star_collapse: the orrery is not running (press o)");
        return;
    }
    if (!star_collapse_now(&e->orrery_star))
        wlr_log(WLR_INFO, "star_collapse: the orrery's star is already a hole");
    else
        wlr_log(WLR_INFO, "orrery: collapsing from %s",
                star_phase_name(e->orrery_star.phase));
}

bool expo_can_orbit(FwmExpo *e) {
    /* Not on the fallback path: a scene node is an axis-aligned rectangle, and
     * a strip that cannot even be curved certainly cannot be flown around. */
    if (!e->gl || e->leaving) return false;
    /* The orrery is nothing BUT looking: the whole mode is a thing to walk
     * around. Gating it on the zoom step, which exists to stop you tilting a
     * desktop you are working in, would keep resetting the camera to level and
     * take the mode's one verb away from it. */
    if (e->orrery) return true;
    return e->zoom_target > (EXPO_ZOOM_NEAR + EXPO_ZOOM_FAR) / 2.0;
}

/* Back to level, from anywhere. Everything that means "act on a desktop"
 * rather than "look at the ring" goes through here. */
void expo_camera_home(FwmExpo *e) {
    e->tilt_target = 0.0;
    e->dist_target = 1.0;
    e->orbiting = 0;
    e->spin = 0.0;
}

void expo_zoom_step(FwmServer *server) {
    FwmExpo *e = server->expo;
    if (!e || e->leaving) return;
    e->zoom_target = e->zoom_target > (EXPO_ZOOM_NEAR + EXPO_ZOOM_FAR) / 2.0
                   ? EXPO_ZOOM_NEAR : EXPO_ZOOM_FAR;
    /* The far step is where the camera may leave its seat, so it is where the
     * hints have something else to say — and a set that has just changed is
     * worth showing again, whether or not the bar had taken itself away. */
    expo_hints_set_flight(e->hints, server->screen_width, server->screen_height,
                          expo_can_orbit(e));
    e->hints_reveal_target = 1.0;
    e->hints_timer = EXPO_HINT_SHOW_S;
}

void expo_destroy(FwmServer *server) {
    expo_teardown(server);
}

/* The monitor the strip is drawn on is going away, and `e->out` points into the
 * FwmOutput that is about to be freed — the camera, the box and the wallpaper
 * are read out of it on every tick, every motion and every draw. There is no
 * strip without a screen to put it on, so it closes with the screen.
 *
 * Closing rather than moving it to another monitor: the strip is a photograph
 * of one screen's desktops, taken at that screen's size, and its home desktop
 * has just been handed to whichever monitor takes it next. */
void expo_output_gone(FwmServer *server, FwmOutput *out) {
    if (server->expo && server->expo->out == out) expo_teardown(server);
}

/* Re-photograph a desktop after something rearranged it under the strip.
 *
 * Two things have to happen and neither is optional. The layout's glide is
 * driven by the physics tick, which the strip freezes, so a tiled window would
 * otherwise sit at its old coordinates until the strip closed: it is settled
 * here by hand. And the pictures themselves are stale, so they are retaken —
 * rebuilt outright when the window changed size, since the buffer and the
 * texture were cut to the old one. */
void expo_refresh_desktop(FwmServer *server, int d) {
    FwmExpo *e = server->expo;
    if (!e || d < 0 || d >= FWM_DESKTOPS) return;

    for (int i = 0; i < e->n_items; i++) {
        ExpoItem *it = &e->items[i];
        if (it->desktop != d) continue;
        FwmView *view = it->view;

        if (view->tile_anim) {
            view->x = (int)lround(view->tile_tx);
            view->y = (int)lround(view->tile_ty);
            view->tile_anim = 0;
            PhysicsBody *b = physics_find_body(&server->physics, view->id);
            if (b) { b->x = view->x; b->y = view->y; b->vx = b->vy = 0; }
            view_sync_position(view);
        }

        if (view->width != it->w || view->height != it->h) {
            expo_forget_view(server, view);   /* takes the slot with it */
            expo_add_item(e, view);
            i = -1;                            /* the array moved under us */
            continue;
        }

        expo_resnap_item(e, it);
    }
    expo_canvas_dirty(e);
}

void expo_forget_view(FwmServer *server, FwmView *view) {
    FwmExpo *e = server->expo;
    if (!e) return;
    for (int i = 0; i < e->n_items; i++) {
        if (e->items[i].view != view) continue;
        ExpoItem *it = &e->items[i];
        if (e->hover == it) e->hover = NULL;
        if (e->drag == it) e->drag = NULL;
        if (e->menu_item == it) expo_menu_close(e);
        if (it->node) wlr_scene_node_destroy(&it->node->node);
        if (it->tex) wlr_texture_destroy(it->tex);
        if (it->buf) wlr_buffer_unlock(it->buf);
        /* Compact the slots: the hover/drag pointers are into this array, so
         * the last item moving into the hole has to be followed. */
        ExpoItem *last = &e->items[--e->n_items];
        if (last != it) {
            if (e->hover == last) e->hover = it;
            if (e->drag == last) e->drag = it;
            if (e->menu_item == last) e->menu_item = it;
            *it = *last;
        }
        memset(last, 0, sizeof(*last));
        return;
    }
}

/* ── animation ────────────────────────────────────────────────────────── */



void expo_tick(FwmServer *server, double dt) {
    FwmExpo *e = server->expo;
    if (!e) return;

    /* One 60Hz frame at most. The tick drops to a 200ms heartbeat whenever
     * nothing is moving, and the frame that opens the strip is usually the
     * first after such a beat: fed that dt, an exponential chase covers 94% of
     * the distance in a single step and the strip appears rather than opens. */
    if (dt > 1.0 / 60.0) dt = 1.0 / 60.0;

    /* Exponential chase, framerate-independent: a step retargeted mid-flight is
     * simply a new target, with no ease to restart. */
    double gap = e->zoom_target - e->zoom;
    if (fabs(gap) > 0.0005) e->zoom += gap * (1.0 - exp(-EXPO_ZOOM_SPEED * dt));
    else                    e->zoom = e->zoom_target;

    double pgap = e->pan_target - e->pan;
    if (fabs(pgap) > 0.5) e->pan += pgap * (1.0 - exp(-EXPO_PAN_SPEED * dt));
    else                  e->pan = e->pan_target;

    /* Let go of the ring mid-turn and it keeps turning, slowing down — the
     * same throw the windows themselves get. Stopped by grabbing it again, by
     * anything that asks for a desktop, and by running into the ends of a strip
     * that is not a ring (expo_clamp_pan). */
    /* The orrery drives the ring instead of the hand. Fed through `spin`
     * rather than moved directly, so everything that already stops a coasting
     * ring — grabbing it, asking for a desktop, the ends of an open strip —
     * stops this too without knowing it exists. */
    /* The centre of the system. It lives at the middle of the screen because
     * that is where the middle of the ring projects to — the camera always
     * looks at the centre of the circle, so no projection is needed to place
     * it, only to size it. */
    if (e->orrery && e->orrery_draw && e->out) {
        expo_canvas_dirty(e);
        star_tick(&e->orrery_star, &e->orrery_cfg, dt);
        e->orrery_star.wx = e->out->box.width / 2.0;
        e->orrery_star.wy = e->out->box.height / 2.0;
        /* The strip's own clock, which is what everything else here animates
         * against. */
        /* How open the disc looks: the angle between the disc's own normal
         * and the direction you are looking from. Both vectors, not two
         * numbers added together.
         *
         * This is what makes it an object in the world instead of a picture.
         * A disc lying flat in the ring's plane is axially symmetric, so
         * walking round the ring genuinely does not change it — that part was
         * never wrong. But a TIPPED disc must change: from one side you see
         * its face, from a quarter turn away you see its edge. The azimuth of
         * the camera has to be in the arithmetic for that, and it was not,
         * which is why tipping it and then turning the ring did nothing at
         * all.
         *
         *   n = the disc's normal, tipped by `orrery_tilt` towards azimuth 0
         *   d = the line of sight, from camera azimuth `az` and height `tilt`
         *   open = |n . d|
         *
         * At orrery_tilt = 0 this reduces to |sin(tilt)| — the old formula,
         * which is the check that the general case is right. */
        double r_ring = expo_radius(e);
        double az = r_ring > 0.0 ? e->pan / r_ring : 0.0;
        double b = e->orrery_tilt;              /* tip of the disc */
        double th = e->tilt;                    /* how high the camera is */
        double open = fabs(sin(b) * cos(th) * cos(az) + cos(b) * sin(th));
        star_draw_set_disc_tilt(e->orrery_draw, open);
        star_draw_set_disc_roll(e->orrery_draw, e->orrery_roll);
        star_draw_update(e->orrery_draw, &e->orrery_star, &e->orrery_cfg,
                         e->clock, 0, 0, 0);
    }

    /* The orbits themselves. Kepler: the further out, the slower — that is
     * the one rule that makes a set of circles read as a system rather than as
     * a record player. Period goes as r^(3/2), so the angular rate goes as
     * r^(-3/2).
     *
     * `pan` still turns the whole thing, so flying round with alt+drag works
     * exactly as it does elsewhere; this is on top of it. */
    if (e->orrery) {
        /* Easing between the ring and the orbits. Slow enough to read as the
         * desktops moving out and settling, quick enough not to be a wait. */
        double bgap = e->orbit_blend_target - e->orbit_blend;
        if (fabs(bgap) > 0.001) e->orbit_blend += bgap * (1.0 - exp(-2.2 * dt));
        else                    e->orbit_blend = e->orbit_blend_target;
    }

    if (e->orrery && e->orbits) {
        double lap = expo_lap(e);
        for (int i = 0; i < e->n_items; i++) {
            ExpoItem *it = &e->items[i];
            it->moon_a += it->moon_rate * dt;
            if (it->moon_a > 2.0 * M_PI) it->moon_a -= 2.0 * M_PI;
        }
        for (int d = 0; d < FWM_DESKTOPS; d++) {
            double k = e->orbit_r[d] > 0.05 ? e->orbit_r[d] : 0.05;
            e->orbit_phase[d] += e->orrery_speed * pow(k, -1.5) * dt;
            if (lap > 0.0) e->orbit_phase[d] = fmod(e->orbit_phase[d], lap);
        }
        expo_canvas_dirty(e);
    }

    /* The ring itself no longer turns in the orrery: the desktops carry their
     * own way round now, each at its own rate, so driving the whole strip on
     * top of that would spin everything twice. `pan` is left to the hand,
     * which is what alt+drag steers.
     */

    if (!e->orbiting && fabs(e->spin) > EXPO_SPIN_MIN) {
        double before = e->pan_target;
        e->pan_target += e->spin * dt;
        expo_clamp_pan(e);
        if (fabs(e->pan_target - before) < fabs(e->spin * dt) * 0.5) e->spin = 0.0;
        e->pan = e->pan_target;
        e->spin *= exp(-EXPO_SPIN_DECAY * dt);
    } else if (!e->orbiting) {
        e->spin = 0.0;
    }

    /* Carrying a window to a desktop that is not on screen. The hand is at the
     * edge and cannot go further, so the strip turns under it instead — and on
     * a closed ring that means a window really can be walked all the way round,
     * which is most of what closing the ring was for. */
    if (e->drag && !e->leaving) {
        double lx = server->cursor->x;
        double band = EXPO_DRAG_EDGE_PX;
        int dir = lx >= server->screen_width - band ? 1 : (lx <= band ? -1 : 0);
        if (dir) {
            /* Faster the closer to the edge: a window nudged into the band
             * drifts, one pressed against the screen edge travels. */
            double into = dir > 0 ? (lx - (server->screen_width - band)) / band
                                  : (band - lx) / band;
            if (into < 0.0) into = 0.0;
            if (into > 1.0) into = 1.0;
            e->pan_target += dir * into * EXPO_DRAG_PAN_SPEED * expo_pitch(e) * dt;
            expo_clamp_pan(e);
            /* No easing while a hand is steering: the window would lag the
             * strip it is being carried over. */
            e->pan = e->pan_target;
            expo_drag_to(e, lx, server->cursor->y);
        }
    }

    /* Windows that opened since the last frame, and the one desktop that is
     * allowed to still be moving. */
    if (!e->leaving) {
        expo_sync_items(e);
        e->clock += dt;
        expo_live_pass(e, e->clock);
    }

    /* The orbit is offered at the far zoom step only, and taken away the
     * instant the strip comes back in — a tilted desktop is a thing to look
     * at, not a thing to work in. */
    if (!expo_can_orbit(e)) {
        e->tilt_target = 0.0;
        e->dist_target = 1.0;
        e->orbiting = 0;
    }
    /* Coming home is quicker than being sent: the collapse below may only
     * happen from the canonical view, and a camera still easing down while the
     * zoom lands is what made a thumbnail slide off its own desktop. */
    bool homing = e->tilt_target == 0.0 && e->dist_target == 1.0;
    double crate = homing ? EXPO_ORBIT_HOME_SPEED : EXPO_ORBIT_SPEED;
    double tgap = e->tilt_target - e->tilt;
    if (fabs(tgap) > 0.0005) e->tilt += tgap * (1.0 - exp(-crate * dt));
    else                     e->tilt = e->tilt_target;
    double dgap = e->dist_target - e->dist;
    if (fabs(dgap) > 0.0005) e->dist += dgap * (1.0 - exp(-crate * dt));
    else                     e->dist = e->dist_target;

    /* And the star in the middle goes out as the strip closes. Everything else
     * on screen is a picture of a desktop and simply arrives at the desktop it
     * is a picture of; the star is an object standing at the centre of the
     * ring, and the end of the closing animation is the camera moving to where
     * that centre is. A billboard at the viewer's own depth is the size of the
     * screen, which is why it lunged out of the middle of the picture in the
     * last frames before the strip handed over. */
    double ofade = e->leaving ? 0.0 : 1.0;
    double ogap = ofade - e->orrery_fade;
    if (fabs(ogap) > 0.002) e->orrery_fade += ogap * (1.0 - exp(-EXPO_ORRERY_FADE * dt));
    else                    e->orrery_fade = ofade;

    /* The hints show themselves when the strip opens, take themselves away
     * once there has been time to read them, and come back when the cursor
     * goes looking for them along the bottom. */
    if (e->hints) {
        if (!e->leaving) {
            /* The cursor in THIS monitor's frame: the bar is along the bottom
             * of the strip's screen, not of the layout. */
            if (expo_hints_hit(server->screen_height,
                               server->cursor->y - e->out->box.y)) {
                e->hints_reveal_target = 1.0;
                e->hints_timer = EXPO_HINT_LINGER_S;
            } else if (e->hints_timer > 0.0) {
                e->hints_timer -= dt;
                if (e->hints_timer <= 0.0) e->hints_reveal_target = 0.0;
            }
        }
        double hgap = e->hints_reveal_target - e->hints_reveal;
        if (fabs(hgap) > 0.002)
            e->hints_reveal += hgap * (1.0 - exp(-EXPO_HINT_SLIDE * dt));
        else
            e->hints_reveal = e->hints_reveal_target;
        expo_hints_place(e->hints, server->screen_width, server->screen_height,
                         e->hints_reveal);
    }

    /* Read from the config every frame rather than tracked: `x`, a `fwmctl set`
     * and a config reload all mean the same thing here, and none of them has to
     * know the strip is open. */
    e->seam_target = server->config.camera.wrap ? 0.0 : 1.0;
    double sgap = e->seam_target - e->seam;
    if (fabs(sgap) > 0.001) e->seam += sgap * (1.0 - exp(-EXPO_RING_SPEED * dt));
    else                    e->seam = e->seam_target;

    /* Only from level, and only from the right distance: at zoom 1.0 the strip
     * is pixel-for-pixel the live screen ONLY through the canonical camera, and
     * swapping to the real desktop from any other one is a visible jump. */
    if (e->leaving && e->zoom <= 1.0005
        && fabs(e->tilt) < 0.002 && fabs(e->dist - 1.0) < 0.002) {
        expo_teardown(server);
        return;
    }
    expo_layout(e);
}

