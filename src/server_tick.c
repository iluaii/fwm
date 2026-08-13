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

/* The heartbeat: the fixed-step simulation tick, everything it drives (impact
 * squash and camera shake, the drag pendulum, the free-rotation angle the
 * renderer draws), and the two timers that pace the compositor when nothing
 * else is damaging the scene. Split out of server.c; see server_internal.h. */
#include "server.h"
#include "view.h"
#include "physics.h"
#include "layer.h"
#include "lock.h"
#include "foreign.h"
#include "workspace.h"
#include "ipc.h"
#include <signal.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include "ui/tray.h"
#include "ui/modes.h"
#include "ui/stats_menu.h"
#include "ui/welcome.h"
#include "ui/launcher.h"
#include "ui/cairo_overlay.h"
#include "wallpaper.h"
#include "cava.h"
#include "sound.h"
#include "ram.h"
#include "group.h"
#include "expo.h"
#include "shadow.h"
#include "snapshot.h"
#include "server_internal.h"

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
#include <wlr/types/wlr_pointer_gestures_v1.h>
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
#include <linux/input-event-codes.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
/* Impacts only matter if the user can see them: a window landing on desktop 7
 * must not shake the view of someone working on desktop 0. */
#define SHAKE_MAX_PX      14.0
#define SHAKE_FULL_SPEED  2000.0  /* px/s that produces a full-strength shake */
#define SHAKE_DECAY       9.0     /* 1/s; ~0.3s until it dies out */

/* Peak deformation for a window taking `speed`. The shake wants gentle impacts
 * damped because it is intrusive; the squash wants them SEEN, so the curve
 * bends the other way — squaring it (measured) turned an ordinary landing at
 * ~600 px/s into a 3.7% dent.
 *
 * Linear was still too flat at the low end: window-on-window contacts in real
 * use land around 200-300 px/s (measured in a nested run: 0.053 and 0.067,
 * i.e. 5-7%, which reads as "there is no animation between windows"), while
 * only a long fall onto the floor reaches four figures. sqrt lifts the gentle
 * half of the range without touching the cap: 200 px/s -> 14%, 500 -> 22%,
 * 900+ -> the full 30%. */
#define SQUASH_FULL_SPEED 900.0
#define SQUASH_MAX_AMOUNT 0.24

static void server_squash_from_impact(FwmServer *server, uint32_t id,
                                      double nx, double ny, double speed) {
    double strength = server->config.effects.squash;
    if (strength <= 0.0 || id == 0) return;  /* id 0 is a wall, nothing to squash */
    FwmView *v = server_find_view(server, id);
    if (!v) return;

    double f = speed / SQUASH_FULL_SPEED;
    if (f > 1.0) f = 1.0;
    view_start_squash(v, nx, ny, strength * SQUASH_MAX_AMOUNT * sqrt(f));
}

/* How heavy a window has to be for the knock to sound its natural pitch — the
 * same middling window MASS_REF_AREA describes, at the default density. */
#define SOUND_REF_MASS 46.0

/* Play one impact. The two numbers are the whole of the sound design: how hard
 * it was hit, and how big the thing that was hit is. */
static void server_sound_from_impact(FwmServer *server, const PhysicsImpact *im) {
    const SoundConfig *sc = &server->config.sound;

    double span = sc->max_speed - sc->min_speed;
    if (span <= 0.0) return;
    double gain = (im->speed - sc->min_speed) / span;
    if (gain <= 0.0) return;      /* a nudge, not a knock */
    if (gain > 1.0) gain = 1.0;

    /* Heavier windows knock deeper: the same sample played slower IS a bigger
     * object, which is why this is a pitch and not a second file. Taken from the
     * heavier of the two bodies, since that is the one whose voice you would
     * hear; a wall (id 0) has no body and simply does not vote. */
    double mass = 0.0;
    PhysicsBody *a = im->id_a ? physics_find_body(&server->physics, im->id_a) : NULL;
    PhysicsBody *b = im->id_b ? physics_find_body(&server->physics, im->id_b) : NULL;
    if (a && a->mass > mass) mass = a->mass;
    if (b && b->mass > mass) mass = b->mass;

    double pitch = 1.0;
    if (mass > 1.0) pitch = pow(SOUND_REF_MASS / mass, 0.15);

    /* A few percent of scatter, taken from where the hit happened rather than
     * from a random number: two windows resting against each other can bump
     * repeatedly, and identical clicks in a row sound like a stuck machine
     * rather than like objects. */
    int jig = ((int)(fabs(im->x) + fabs(im->y) * 7.0)) % 21 - 10;
    pitch *= 1.0 + 0.04 * (jig / 10.0);

    sound_play(server->sound, gain, pitch);
}

/* ── hit points ──────────────────────────────────────────────────────── */

static double server_now_s(void);

/* One side of a collision, worked out against the other. See [physics] hp in
 * config.h for the formula and why it is shaped that way; in short, damage is
 * the attacker's mass scaled by the square of how far past the break speed the
 * blow landed, and it is compared against the defender's mass ONCE. Nothing is
 * accumulated, so surviving a hit leaves a window in perfect health.
 *
 * `attacker` NULL means a wall hit: walls are scenery and deal no damage at
 * all, or a window dropped from the top of the screen would shatter on the
 * floor every time it was thrown. */
static void server_damage_one(FwmServer *server, const PhysicsBody *attacker,
                              const PhysicsBody *victim, double speed) {
    if (!attacker || !victim) return;

    const PhysicsConfig *pc = &server->config.physics;
    double v_break = pc->hp_break_speed > 0.0 ? pc->hp_break_speed : pc->max_throw_speed;
    if (v_break <= 0.0) return;

    double r = speed / v_break;
    double damage = attacker->mass * r * r * physics_body_hardness(attacker);
    double hp = physics_body_hp(victim);
    if (damage <= hp) return;

    FwmView *v = server_find_view(server, victim->id);
    if (!v) return;
    /* Once is enough: a window caught between two others takes both blows in
     * the same frame, and the second must not queue a second close. Past the
     * grace period the client has visibly declined, and the next hard enough
     * hit asks again — see FwmView.dying_at. */
    double now = server_now_s();
    if (v->dying && now - v->dying_at < HP_CLOSE_GRACE_S) return;
    v->dying = 1;
    v->dying_at = now;

    wlr_log(WLR_DEBUG, "window %u destroyed: %.1f damage vs %.1f hp at %.0f px/s",
            victim->id, damage, hp, speed);
    view_send_close(v);
}

static void server_damage_from_impact(FwmServer *server, const PhysicsImpact *im) {
    PhysicsBody *a = im->id_a ? physics_find_body(&server->physics, im->id_a) : NULL;
    PhysicsBody *b = im->id_b ? physics_find_body(&server->physics, im->id_b) : NULL;
    /* Both directions from the one approach speed: two equal windows meeting
     * fast enough destroy each other, which is the honest reading of a
     * collision and the one the asymmetry of mass already softens. */
    server_damage_one(server, a, b, im->speed);
    server_damage_one(server, b, a, im->speed);
}

static void server_consume_impacts(FwmServer *server) {
    double shake = server->config.effects.camera_shake;
    double squash = server->config.effects.squash;
    int hp = server->config.physics.hp;
    if (shake <= 0.0 && squash <= 0.0 && !server->sound && !hp) {
        server->physics.impact_count = 0;
        return;
    }

    /* An impact shakes the monitor SHOWING the desktop it happened on — and
     * only that one. A window landing on desktop 3 must not jolt the screen
     * next to it that is showing desktop 7. */
    for (int i = 0; i < server->physics.impact_count; i++) {
        const PhysicsImpact *im = &server->physics.impacts[i];
        /* A contact point sits ON the surface it hit, so a wall impact lands
         * just OUTSIDE the play area: the right wall's inner face is at
         * 10*screen_width, which divides to desktop 10 — a desktop that does
         * not exist — and every hit against it was silently dropped. (The left
         * wall only escaped because C truncates -2/1920 toward zero.) Clamp
         * into the real range instead of trusting the division. */
        int impact_d = (int)(im->x / server->screen_width);
        if (impact_d < 0) impact_d = 0;
        if (impact_d > 9) impact_d = 9;

        /* Before the "is anyone watching" test below, and deliberately: the
         * shake and the knock are presentation and there is no point spending
         * them on a desktop nobody can see, but a window crushed on desktop 7
         * is crushed whether or not you were there to watch it happen. */
        if (hp) server_damage_from_impact(server, im);

        FwmOutput *out = server_output_showing(server, impact_d);
        if (!out) continue;   /* nobody is watching that desktop */

        /* The normal points from A to B, so it faces the contact for A and
         * away from it for B — flip it for B. */
        server_squash_from_impact(server, im->id_a,  im->nx,  im->ny, im->speed);
        server_squash_from_impact(server, im->id_b, -im->nx, -im->ny, im->speed);

        /* Only for a desktop somebody is looking at, like the shake above: a
         * window landing on desktop 7 while you work on desktop 0 is not an
         * event you asked to hear. */
        if (server->sound) server_sound_from_impact(server, im);

        if (shake <= 0.0) continue;
        double f = im->speed / SHAKE_FULL_SPEED;
        if (f > 1.0) f = 1.0;
        /* Squared so gentle bumps stay subtle and only real slams shake hard. */
        double mag = shake * SHAKE_MAX_PX * f * f;
        /* Take the strongest impact of the frame rather than summing: three
         * windows landing together should not triple the shake. */
        if (mag > out->shake_mag) {
            out->shake_mag = mag;
            out->shake_t = 0.0;
        }
    }
}

/* Advanced at FRAME time, like the other purely visual ramps (see
 * server_animate) — on the physics timer it would beat against vsync. */
/* Both the shake and the seam slide move what one monitor draws without moving
 * its camera, so they have to agree on where things end up rather than each
 * writing its own answer over the other's. The sum lands in render_dx/dy, which
 * server_place_node adds to every window on that monitor's desktop.
 *
 * It used to be an offset on the shared layer trees; with independent screens
 * that shook both of them at once. */
static void output_render_offset(FwmServer *server, FwmOutput *out, int ox, int oy) {
    if (out->wrap_slide > 0.0)
        ox += (int)lround(out->wrap_dir * out->wrap_slide);
    out->render_dx = ox;
    out->render_dy = oy;

    /* The wallpaper travels with the world it belongs to. */
    if (out->wallpaper) {
        wallpaper_set_origin(out->wallpaper, out->box.x + ox, out->box.y + oy);
        wallpaper_update(out->wallpaper, out->camera_x);
    }
    if (out->wrap_ghost) {
        /* A screen behind the world, in the direction it came from. */
        wlr_scene_node_set_position(&out->wrap_ghost->node,
                                    out->box.x + ox - out->wrap_dir * server->screen_width,
                                    out->box.y + oy);
    }
}

/* Start the slide. The caller has already put this monitor's camera on the far
 * side of the join; this is what stops that being a cut. */
void server_wrap_slide_start(FwmServer *server, FwmOutput *out, int dir) {
    if (!out || server->screen_width <= 0 || dir == 0) return;
    server_wrap_slide_stop(server, out);

    /* Photograph what this screen showed a moment ago — before the camera
     * jumped, this was called; see server_goto_desktop. */
    struct wlr_buffer *buf = snapshot_alloc(server, server->screen_width,
                                            server->screen_height);
    if (!buf) return;
    if (!snapshot_world(server, out, buf)) { wlr_buffer_drop(buf); return; }

    /* In the scene root rather than inside the window tree: the ghost travels
     * the opposite way to the world, and it sits one place above the windows,
     * being in front of the desktop arriving until it has left. */
    out->wrap_ghost = wlr_scene_buffer_create(&server->scene->tree, buf);
    if (!out->wrap_ghost) { wlr_buffer_drop(buf); return; }
    out->wrap_ghost_buf = wlr_buffer_lock(buf);
    wlr_buffer_drop(buf);
    wlr_scene_node_place_above(&out->wrap_ghost->node,
                               &server->layer_windows->node);

    out->wrap_dir = dir;
    out->wrap_slide = server->screen_width;
}

void server_wrap_slide_stop(FwmServer *server, FwmOutput *out) {
    (void)server;
    if (!out) return;
    if (out->wrap_ghost) {
        wlr_scene_node_destroy(&out->wrap_ghost->node);
        out->wrap_ghost = NULL;
    }
    if (out->wrap_ghost_buf) {
        wlr_buffer_unlock(out->wrap_ghost_buf);
        out->wrap_ghost_buf = NULL;
    }
    out->wrap_slide = 0.0;
}

void server_shake_tick(FwmServer *server, double dt) {
    FwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (out->wrap_slide > 0.0) {
            double per_ms = server->screen_width / WRAP_SLIDE_MS;
            out->wrap_slide -= per_ms * dt * 1000.0;
            if (out->wrap_slide <= 1.0) server_wrap_slide_stop(server, out);
        }

        if (out->shake_mag <= 0.01) {
            if (out->shake_mag != 0.0) out->shake_mag = 0.0;
            if (out->render_dx || out->render_dy || out->wrap_slide > 0.0)
                output_render_offset(server, out, 0, 0);
            continue;
        }
        out->shake_t += dt;
        out->shake_mag *= exp(-SHAKE_DECAY * dt);

        /* Two different frequencies, or the offset would travel a straight
         * diagonal instead of reading as a shake. */
        int ox = (int)lround(out->shake_mag * sin(out->shake_t * 38.0));
        int oy = (int)lround(out->shake_mag * sin(out->shake_t * 47.0 + 1.3));

        /* Only the world shakes. The tray and panels stay put: UI jittering
         * under the cursor reads as a glitch, not as impact. */
        output_render_offset(server, out, ox, oy);
    }
    /* The windows follow the offsets that just changed. */
    server_views_place(server);
}

/* How long the compositor may sit without driving a frame itself. Not a
 * refresh rate: client redraws and scene node moves damage the scene and
 * wlr_scene schedules a frame off that damage, so this is only a heartbeat. */
#define TICK_IDLE_MS 200

/* Is anything actually moving? Everything listed here is driven by our timers
 * rather than by client damage, so the frame loop must keep running for it.
 * Err on the side of "yes": a false busy costs some idle wakeups, a false idle
 * freezes an animation halfway. */
static int server_is_busy(FwmServer *server) {
    if (server->interactive.action != FWM_ACTION_NONE) return 1;
    if (expo_animating(server)) return 1;             /* the strip zooming */
    if (expo_live_active(server)) return 1;           /* front desktop kept live */
    FwmOutput *bo;
    wl_list_for_each(bo, &server->outputs, link) {
        if (bo->camera_x != bo->target_camera_x || bo->cam_anim) return 1;
        if (bo->wallpaper_prev) return 1;              /* wallpaper cross-fade */
        if (bo->shake_mag > 0.0) return 1;
        if (bo->wrap_slide > 0.0) return 1;            /* sliding across the join */
    }

    if (!wl_list_empty(&server->ghosts)) return 1;     /* close animations */
    if (launcher_is_open(server->launcher)) return 1;  /* spring tiles */
    if (cairo_overlay_animating()) return 1;
    if (server->modes_buffer && modes_menu_animating()) return 1;
    if (server->stats_buffer && stats_menu_animating()) return 1;
    /* Bars that are up must keep the tick at full rate: on the heartbeat they
     * would fall four times a second, and the windows standing on them would
     * be shoved by a bar that teleported rather than rose. Silence lets it go
     * idle again, so a configured visualiser costs nothing while nothing plays. */
    if (cava_busy(server->cava)) return 1;

    for (int i = 0; i < server->physics.body_count; i++) {
        const PhysicsBody *b = &server->physics.bodies[i];
        if (b->active && b->flying) return 1;
        /* A window can spin in place with no linear motion at all: `flying`
         * would say idle and the frame loop would drop to the heartbeat,
         * leaving the rotation to advance four times a second. Once the spin
         * has bled off, the window keeps the angle it came to rest at — that
         * is the point of the effect — but nothing is moving any more, so it
         * stops counting as busy and the snapshot refresh rides the heartbeat
         * from there. */
        if (b->active && b->spin && fabs(b->angvel) > 0.01) return 1;
    }
    FwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->open_anim || v->tile_anim || v->squash_buf) return 1;
        if (v->dim != v->dim_target) return 1;         /* focus fading over */
    }
    return 0;
}

void server_schedule_frames(FwmServer *server) {
    struct FwmOutput *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_schedule_frame(output->wlr_output);
    }
}

/* Wake-up period for the frame timer: HALF the video's frame interval.
 *
 * On an otherwise idle desktop this timer is the only thing scheduling output
 * frames, so its ticks are the only moments at which a video frame can reach
 * the screen. Waking exactly once per frame interval makes that a knife-edge:
 * the timer has millisecond granularity, re-arms only after the blit, and its
 * phase drifts freely against the refresh clock — so a tick that lands just
 * after a vblank pushes its frame a whole refresh late and the wallpaper
 * hitches, no matter how exact the pacing in video.c is.
 *
 * Waking twice per interval gives every deadline a second chance. The extra
 * tick is nearly free: presenting nothing leaves the scene undamaged, and
 * wlr_scene_output_commit returns immediately when nothing needs a frame. */
static int video_wake_ms(FwmServer *server) {
    /* The fastest of the monitors' wallpapers: one timer serves them all, so
     * it has to keep up with whichever is playing at the highest rate. */
    int ms = 0;
    FwmOutput *o;
    wl_list_for_each(o, &server->outputs, link) {
        int m = wallpaper_video_interval_ms(o->wallpaper);
        if (m > 0 && (ms == 0 || m < ms)) ms = m;
    }
    if (ms <= 0) return 0;
    return ms > 1 ? ms / 2 : 1;
}

/* Any monitor's wallpaper still playing video. */
static bool any_wallpaper_playing(FwmServer *server) {
    FwmOutput *o;
    wl_list_for_each(o, &server->outputs, link) {
        if (wallpaper_playing(o->wallpaper)) return true;
    }
    return false;
}

/* Fires at twice the playing video wallpaper's own fps. Uploading the next
 * frame here (when it is due) and scheduling one output frame is what lets the
 * compositor render at, say, 24 Hz for a 24 fps clip instead of the 60 Hz the
 * physics heartbeat would otherwise impose — the biggest single CPU saving for
 * video wallpaper. */
static int video_timer_cb(void *data) {
    FwmServer *server = data;
    FwmOutput *o;
    wl_list_for_each(o, &server->outputs, link) {
        wallpaper_present(o->wallpaper);
        wallpaper_present(o->wallpaper_prev);
    }
    server_schedule_frames(server);

    int ms = video_wake_ms(server);
    if (any_wallpaper_playing(server) && ms > 0) {
        wl_event_source_timer_update(server->video_timer, ms);
    } else {
        server->video_timer_on = 0; /* paused/gone: stop until re-armed */
    }
    return 0;
}

/* Hand freed heap back to the kernel after a wallpaper set is torn down.
 *
 * Tearing one down releases tens to hundreds of MB in a handful of very large
 * chunks. main() pins the mmap threshold so most of that is munmapped on the
 * spot, but the decode threads also leave whole arenas dirty, and free() alone
 * only trims what happens to sit at the top of a heap. malloc_trim walks every
 * arena (glibc >= 2.26), which is exactly what is wanted here: it costs a few
 * ms and runs only on a wallpaper change, never in the frame loop. */
void server_reclaim_memory(void) {
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}

/* Arm or disarm the video-frame timer to match the current state. Cheap and
 * idempotent, so it is safe to call every physics tick and on every wallpaper
 * change. */
void server_video_sync(FwmServer *server) {
    if (!server->video_timer) return;
    int ms = video_wake_ms(server);
    int want = any_wallpaper_playing(server) && ms > 0;
    if (want && !server->video_timer_on) {
        server->video_timer_on = 1;
        wl_event_source_timer_update(server->video_timer, ms);
    } else if (!want && server->video_timer_on) {
        server->video_timer_on = 0;
        wl_event_source_timer_update(server->video_timer, 0); /* disarm */
    }
}

/* How long to wait between looks for a sound server that is not there yet.
 * Two stats, so the cost is nothing; the delay is only so the log line and the
 * work stay out of the way of a machine that will never have one. */
#define CAVA_RETRY_S 3.0

static double server_now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ── mass from memory use ────────────────────────────────────────────── */

/* How often /proc is walked while mass = "ram". A window's memory footprint is
 * not a thing that moves at 60Hz, and the walk costs a few ms — but a browser
 * opening a heavy page should be visibly heavier before the user has finished
 * looking at it, so this is seconds and not tens of seconds. */
#define MASS_SAMPLE_S 1.5

/* The window a memory footprint is measured against: mass = "ram" means the
 * SIZE of a window says nothing about its weight, so the area term has to be
 * replaced by a constant rather than kept alongside. A middling window, so a
 * desktop switched into this mode does not suddenly weigh ten times what it
 * did — only the hogs do. */
/* ── the sun ──────────────────────────────────────────────────────────────
 *
 * How often the wall clock is worth re-reading in clock mode. The sun crosses
 * the sky in a working day; a quarter of a second is already four times finer
 * than anything the eye can follow it at, and it keeps the cost of the whole
 * feature at four trigonometric functions a second. */
#define SUN_CLOCK_INTERVAL_S 0.25

void server_sun_apply(FwmServer *server) {
    const SunConfig *cfg = &server->config.sun;
    sun_light(cfg, sun_hour_local(time(NULL)), &server->sun_light);

    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        /* A window mapped while [sun] was off has no nodes to put a shadow in;
         * one mapped while it was on keeps them, hidden, for nothing. Both are
         * settled here, so turning the sun on and off at runtime works without
         * anyone having to remap a window. */
        if (cfg->enabled && !view->shadow && view->scene_tree)
            view->shadow = shadow_create(view->scene_tree);
        else if (!cfg->enabled && view->shadow) {
            shadow_destroy(view->shadow);
            view->shadow = NULL;
        }
        view_shadow_update(view);
    }
}

void server_sun_sync(FwmServer *server) {
    const SunConfig *cfg = &server->config.sun;
    double now = server_now_s();

    /* Manual mode has no clock to watch: the light only moves when something
     * moved it, and those paths call server_sun_apply themselves. All that is
     * left here is noticing `fwmctl set sun.azimuth`, which writes the config
     * behind everyone's back — cheap enough to check against the light we are
     * already drawing. */
    if (cfg->mode != SUN_MODE_CLOCK) {
        FwmSunLight want;
        sun_light(cfg, 0.0, &want);
        if (sun_light_differs(&want, &server->sun_light)) server_sun_apply(server);
        return;
    }

    if (now - server->sun_checked_at < SUN_CLOCK_INTERVAL_S) return;
    server->sun_checked_at = now;

    FwmSunLight want;
    sun_light(cfg, sun_hour_local(time(NULL)), &want);
    if (sun_light_differs(&want, &server->sun_light)) server_sun_apply(server);
}

#define MASS_REF_AREA (1280.0 * 720.0)

/* Reset every body to the weight its area gives it. */
static void server_mass_clear(FwmServer *server) {
    for (int i = 0; i < server->physics.body_count; i++)
        server->physics.bodies[i].mass_scale = 1.0;
}

void server_mass_sync(FwmServer *server) {
    const PhysicsConfig *pc = &server->config.physics;
    int mode = pc->mass_mode;

    if (mode != PHYSICS_MASS_RAM) {
        /* Only on the way out of the mode, not every tick: bodies carry a
         * scale of 1.0 from birth, so a compositor that never turns this on has
         * nothing to undo. */
        if (server->mass_applied != mode) {
            server_mass_clear(server);
            server->mass_applied = mode;
            /* Back on the area rule, where mass only changes when the window
             * is resized: hit points can follow it again. Releasing needs no
             * deferral, unlike the freeze below. */
            physics_freeze_hp(&server->physics, 0);
            server->hp_freeze_pending = 0;
        }
        return;
    }

    double now = server_now_s();
    /* A mode that has only just been switched on samples immediately; after
     * that it is on the timer. */
    if (server->mass_applied == mode && now < server->mass_sample_at) return;
    /* First sample of this mode: the weights below are the ones hit points are
     * pinned to, once physics_step has turned them into masses. */
    /* Every sample, not just the first: physics_freeze_hp only fills in bodies
     * that have none, so this is what gives a window opened halfway through the
     * mode its hit points. */
    server->hp_freeze_pending = 1;
    server->mass_applied = mode;
    server->mass_sample_at = now + MASS_SAMPLE_S;

    ram_snapshot();

    double hi = pc->mass_ram_max > 1.0 ? pc->mass_ram_max : 1.0;
    double ref = pc->mass_ram_ref > 1.0 ? pc->mass_ram_ref : 1.0;

    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        PhysicsBody *b = physics_find_body(&server->physics, view->id);
        if (!b) continue;

        double mb = ram_tree_mb(view_pid(view));
        /* Nothing to read — an X11 surface with no pid, a client on another
         * machine, a process that vanished mid-walk. Weighing it nothing would
         * fling it off the screen on the next collision; leave it as its area
         * says instead. */
        if (mb <= 0.0) { b->mass_scale = 1.0; continue; }

        double area = (double)b->width * (double)b->height;
        if (area < 1.0) area = 1.0;
        /* Area out, memory in: what a "size" window of MASS_REF_AREA using
         * `ref` MB would weigh, scaled by how far past that this one is. */
        double scale = (MASS_REF_AREA / area) * (mb / ref);
        if (scale > hi)       scale = hi;
        if (scale < 1.0 / hi) scale = 1.0 / hi;
        b->mass_scale = scale;
    }
}

/* ── collision sound ─────────────────────────────────────────────────── */

/* Bring the mixer in line with [sound] collisions: start it, stop it, or leave
 * it be. Cheap and idempotent, so the tick calls it unconditionally and the
 * menu, `fwmctl set` and a reload all land here.
 *
 * A changed sample path is a rebuild rather than a live update — the sample is
 * loaded once and read by the mixer without a lock — so server_reload_config
 * drops the mixer and the next call here starts it again with the new file. */
void server_sound_sync(FwmServer *server) {
    int want = server->config.sound.collisions ? 1 : 0;

    if (want == server->sound_applied) {
        /* Volume is the one knob that can change under a running mixer. */
        if (server->sound) sound_set_config(server->sound, &server->config.sound);
        return;
    }
    server->sound_applied = want;

    if (!want) {
        if (server->sound) {
            sound_destroy(server->sound);
            server->sound = NULL;
        }
        return;
    }

    if (!sound_supported()) {
        /* Once, not once per toggle: the build cannot grow a backend while it
         * runs, and the switch still reads as on so the user can see what they
         * asked for. */
        static int told = 0;
        if (!told) {
            wlr_log(WLR_INFO, "sound: built without libpulse-simple — collisions stay silent");
            told = 1;
        }
        return;
    }

    server->sound = sound_create(&server->config.sound);
    if (!server->sound)
        wlr_log(WLR_ERROR, "sound: could not start the mixer thread");
}

void server_cava_sync(FwmServer *server) {
    int want = server->config.cava.mode;

    /* Debug: FWM_TEST_CAVA_MODE forces the mode on for a nested run, whose
     * config.toml is the user's real one and almost certainly has no [cava].
     * Cached because this runs every tick and getenv walks the environment.
     * It overrides a reload too — that is the point of a test hook. */
    static int test_mode = -2;
    if (test_mode == -2) {
        const char *tm = getenv("FWM_TEST_CAVA_MODE");
        if      (!tm)                        test_mode = -1;
        else if (strcmp(tm, "visual")   == 0) test_mode = CAVA_MODE_VISUAL;
        else if (strcmp(tm, "physical") == 0) test_mode = CAVA_MODE_PHYSICAL;
        else if (strcmp(tm, "both")     == 0) test_mode = CAVA_MODE_BOTH;
        else if (strcmp(tm, "off")      == 0) test_mode = CAVA_MODE_OFF;
        else                                  test_mode = -1;
    }
    if (test_mode >= 0) want = test_mode;

    /* The capture thread reports failure asynchronously — it cannot be waited
     * on (see audio.h) — so a row built optimistically at startup is retired
     * here, the moment the answer comes back. Retired, not abandoned: the sound
     * server it could not find may well turn up later, and the retry below is
     * what brings the bars back when it does. */
    if (server->cava && cava_dead(server->cava)) {
        if (!server->cava_reported) {
            wlr_log(WLR_INFO, "cava: no audio capture available — waiting for a sound server");
            server->cava_reported = 1;
        }
        physics_set_bars(&server->physics, NULL, 0, 0.0, 0.0, 0.0, 0, 0.0, 0.0);
        cava_destroy(server->cava);
        server->cava = NULL;
        server->cava_retry_at = server_now_s() + CAVA_RETRY_S;
        return;
    }

    /* Compared against the attempt, not against the live instance: cava is NULL
     * both when the mode is off and when the build failed, and only this tells
     * those apart. */
    if (server->cava_applied != want) {
        /* A mode change is a rebuild, not a toggle: the visual half owns a scene
         * subtree that only exists when it is on, and the physical half owns
         * bodies in the world. Tear the row out of the world first —
         * cava_destroy takes the levels with it, and physics_set_bars would
         * otherwise keep stepping bodies against numbers that no longer
         * exist. */
        if (server->cava) {
            physics_set_bars(&server->physics, NULL, 0, 0.0, 0.0, 0.0, 0, 0.0, 0.0);
            cava_destroy(server->cava);
            server->cava = NULL;
        }
        /* Only once the screen exists: cava_create needs its size, and the
         * output handler calls straight back here as soon as it does. */
        if (server->screen_width == 0) return;
        server->cava_applied = want;
        server->cava_retry_at = 0.0;   /* a fresh mode tries at once */
        server->cava_reported = 0;
    }

    if (want == CAVA_MODE_OFF || server->cava) return;
    if (server->screen_width == 0) return;

    /* The mode is on and nothing is capturing. That is the ordinary state of a
     * machine whose sound daemon is autospawned by the first client that wants
     * sound: fwm starts before any of them, so at login there is no server and
     * ten minutes later there is one. Looking exactly once — which is what this
     * used to do — left the bars off for the whole session, and no reload
     * brought them back, because the configured mode had not changed. So keep
     * asking, slowly. cava_create answers with a pair of stats when there is
     * still nothing there, which is cheap enough to do every few seconds. */
    double now = server_now_s();
    if (now < server->cava_retry_at) return;
    server->cava_retry_at = now + CAVA_RETRY_S;

    /* The test hook forces the mode without touching the file, so hand
     * cava_create the mode we actually resolved rather than the configured one
     * it would otherwise read straight back out and refuse. */
    CavaConfig cfg = server->config.cava;
    cfg.mode = want;

    server->cava = cava_create(server->layer_background, &cfg,
                               server->screen_width, server->screen_height);
    if (server->cava) {
        server->cava_reported = 0;
    } else if (!server->cava_reported) {
        wlr_log(WLR_INFO, "cava: no sound server yet — bars will start when one appears");
        server->cava_reported = 1;
    }
}

/* How fast the swing bleeds off, 1/s. Some is wanted: a real window dragged by
 * its corner settles behind the hand rather than swinging forever. */
#define SWING_DAMP 1.2
/* Ceilings on what one tick may produce, because the pivot's acceleration is
 * differentiated twice from a 60Hz cursor sample: one dropped frame during a
 * fast flick is otherwise an impulse the size of a car crash. */
#define SWING_MAX_ACCEL 20000.0   /* px/s^2 */
#define SWING_MAX_SPEED    12.0   /* rad/s */

/* Time constants for the two filters between the cursor and that acceleration.
 *
 * Differentiating a hand-driven point twice is the noisiest thing in the whole
 * compositor: the pointer arrives in bursts, so at 60Hz some ticks see two
 * events and some none, and the raw second difference alternates hard enough
 * to saturate the clamp above on ordinary movement. Saturating in alternating
 * directions is not a big swing, it is a shudder — worse the further from the
 * centre the window is held, because the torque scales with the lever arm, and
 * worse again with gravity on, because then there is a real swing underneath
 * for the noise to ride on.
 *
 * Expressed as seconds rather than as per-tick blend factors so the filtering
 * means the same thing whatever the tick rate is.
 *
 * Chosen against both things that matter at once, for a hand moving at a steady
 * 300 px/s with pointer events arriving unevenly, and for a genuine flick:
 *
 *   vel/acc tau      noise      peak on a flick
 *   none             12000        20000  (clamped)
 *   0.030 / 0.060      373        15464
 *   0.040 / 0.080      327        12128   <- here
 *   0.060 / 0.150      417         7162
 *
 * Past this the noise stops improving and only the flick gets weaker, so this
 * is where the two curves cross. */
#define SWING_VEL_TAU 0.040   /* s — smooths the pivot's velocity */
#define SWING_ACC_TAU 0.080   /* s — and then its acceleration */

/* The angle to DRAW a body at, as opposed to the one it was last simulated at.
 *
 * The simulation runs on a timer and the screen presents on vsync, and the two
 * are not the same clock — the timer is armed in whole milliseconds, so a "60Hz"
 * tick is really 16ms and 62.5Hz. Nothing keeps them in phase: over about half
 * a second they drift a whole tick apart, so a run of frames each show the
 * angle one step further on, then one frame shows the same angle twice or skips
 * a step. That is a judder you can see whenever the rotation is slow enough to
 * follow — and a window held by its edge shows it most, because there the same
 * step of angle is the largest movement in pixels.
 *
 * So draw where the body IS at this instant, not where it was when the last
 * step ended: advance it by its own angular velocity over the time since. The
 * lag is capped at a tick and a half, past which the compositor has stalled and
 * extrapolating further would only invent a spin nobody performed. */
double server_render_angle(FwmServer *server, const PhysicsBody *b) {
    if (!b->spin || b->angvel == 0.0) return b->angle;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double lag = server->sim_accum
               + (double)(now.tv_sec - server->last_tick.tv_sec)
               + (double)(now.tv_nsec - server->last_tick.tv_nsec) / 1e9;
    if (lag < 0.0) lag = 0.0;
    double cap = 1.5 / PHYSICS_TICK_RATE;
    if (lag > cap) lag = cap;
    return b->angle + b->angvel * lag;
}

/* A window being dragged hangs from the point it was grabbed by.
 *
 * Physically this is a compound pendulum whose pivot is the cursor: the window
 * turns about its own center of mass, the hand holds it somewhere else, and
 * everything that would swing a real object hanging from that point swings
 * this one — gravity when it is on, and the pseudo-force of the pivot being
 * yanked around when it is not (which is what makes a flick of the wrist
 * whirl a window held by its corner even in zero-g).
 *
 *   alpha = (r x (g - a_pivot)) / (I/m)      with  I/m = (w^2+h^2)/12 + |r|^2
 *
 * The mass cancels, which is why none of it appears below. `r` runs from the
 * grab point to the window's center, so grabbing a window dead center gives
 * r = 0 and no swing at all — exactly as it should, and exactly what happens
 * with a real sheet of paper.
 *
 * The position is then placed so the grab point stays under the cursor: as the
 * window turns, its center orbits the pivot. */
static void server_drag_swing(FwmServer *server, double dt) {
    if (server->interactive.action != FWM_ACTION_MOVE || !server->interactive.view) return;
    FwmView *view = server->interactive.view;
    PhysicsBody *b = physics_find_body(&server->physics, view->id);
    if (!b || !b->spin || dt <= 0.0) return;

    /* The cursor, in the same world coordinates the bodies live in. */
    double px, py;
    server_cursor_world(server, &px, &py);

    if (!server->interactive.pivot_have) {
        server->interactive.pivot_x = px;
        server->interactive.pivot_y = py;
        server->interactive.pivot_vx = 0.0;
        server->interactive.pivot_vy = 0.0;
        server->interactive.pivot_have = 1;
        return;   /* no history yet: nothing to differentiate */
    }

    /* Velocity of the pivot, smoothed, then its change, smoothed again. Both
     * stages are needed: one filter on the velocity still leaves an
     * acceleration that alternates sign every tick (see SWING_*_TAU). */
    double nvx = (px - server->interactive.pivot_x) / dt;
    double nvy = (py - server->interactive.pivot_y) / dt;
    double kv = dt / (dt + SWING_VEL_TAU);
    double svx = server->interactive.pivot_vx + (nvx - server->interactive.pivot_vx) * kv;
    double svy = server->interactive.pivot_vy + (nvy - server->interactive.pivot_vy) * kv;

    double rax = (svx - server->interactive.pivot_vx) / dt;
    double ray = (svy - server->interactive.pivot_vy) / dt;
    double ka = dt / (dt + SWING_ACC_TAU);
    double ax = server->interactive.pivot_ax + (rax - server->interactive.pivot_ax) * ka;
    double ay = server->interactive.pivot_ay + (ray - server->interactive.pivot_ay) * ka;
    if (ax >  SWING_MAX_ACCEL) ax =  SWING_MAX_ACCEL;
    if (ax < -SWING_MAX_ACCEL) ax = -SWING_MAX_ACCEL;
    if (ay >  SWING_MAX_ACCEL) ay =  SWING_MAX_ACCEL;
    if (ay < -SWING_MAX_ACCEL) ay = -SWING_MAX_ACCEL;
    server->interactive.pivot_x = px;
    server->interactive.pivot_y = py;
    server->interactive.pivot_vx = svx;
    server->interactive.pivot_vy = svy;
    server->interactive.pivot_ax = ax;
    server->interactive.pivot_ay = ay;

    /* Grab point -> center, with the window's current rotation applied. */
    double c = cos(b->angle), s = sin(b->angle);
    double rx = -(c * server->interactive.grab_lx - s * server->interactive.grab_ly);
    double ry = -(s * server->interactive.grab_lx + c * server->interactive.grab_ly);

    double gy = server->physics.gravity * server->physics.gravity_scale;
    double ex = 0.0 - ax;      /* effective field in the pivot's frame */
    double ey = gy  - ay;

    double inertia = ((double)b->width * b->width + (double)b->height * b->height) / 12.0
                   + rx * rx + ry * ry;
    if (inertia > 1.0) {
        double alpha = (rx * ey - ry * ex) / inertia;
        b->angvel += alpha * dt;
    }
    b->angvel *= exp(-SWING_DAMP * dt);
    if (b->angvel >  SWING_MAX_SPEED) b->angvel =  SWING_MAX_SPEED;
    if (b->angvel < -SWING_MAX_SPEED) b->angvel = -SWING_MAX_SPEED;

    server_drag_swing_place(server);
}

/* Hang the window off the pivot: as it turns, its centre orbits the cursor.
 *
 * Split out of the integration above and called once per FRAME as well,
 * because where the window is drawn must follow the hand, not the physics
 * timer. The timer is free-running 60Hz and the output presents on its own
 * vsync; a position written only on the tick arrives on screen twice in one
 * frame and not at all in the next, which is a judder no amount of smoothing
 * in the pendulum can fix. Everything here is a function of the cursor and the
 * current angle, so running it more often is free and always correct — it is
 * the integration that needs a fixed step, not the placement.
 *
 * The clamp is the same one the drag itself uses: a kinematic body passes
 * straight through the play-area walls, so nothing else keeps a swinging
 * window on screen. */
void server_drag_swing_place(FwmServer *server) {
    if (server->interactive.action != FWM_ACTION_MOVE || !server->interactive.view) return;
    FwmView *view = server->interactive.view;
    PhysicsBody *b = physics_find_body(&server->physics, view->id);
    if (!b || !b->spin || !server->interactive.pivot_have) return;

    double px, py;
    server_cursor_world(server, &px, &py);

    /* The angle as it is being DRAWN this frame, not as of the last step: the
     * window hangs off the pivot at whatever angle the viewer can see, or the
     * picture and the place it is drawn disagree by a tick. */
    double a = server_render_angle(server, b);
    double c = cos(a), s = sin(a);
    double rx = -(c * server->interactive.grab_lx - s * server->interactive.grab_ly);
    double ry = -(s * server->interactive.grab_lx + c * server->interactive.grab_ly);

    double cx = px + rx, cy = py + ry;
    double nx = cx - b->width / 2.0, ny = cy - b->height / 2.0;
    double max_x = 10.0 * server->screen_width - b->width;
    double max_y = server->screen_height - b->height;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx > max_x) nx = max_x > 0 ? max_x : 0;
    if (ny > max_y) ny = max_y > 0 ? max_y : 0;

    b->x = nx;
    b->y = ny;
    view->x = (int)lround(nx);
    view->y = (int)lround(ny);
    if (view->scene_tree) server_place_node(server, &view->scene_tree->node, nx, ny);
}

/* The camera has come to rest. Called from the tick when a slide or a pan
 * finishes, and by anything that parks the camera without moving it (a gesture
 * released on a desktop it was already standing on). */
void server_camera_settled(FwmServer *server) {
    FwmView *xv;
    wl_list_for_each(xv, &server->views, link) view_sync_position(xv);

    /* Arriving on a desktop should hand the keyboard to something there.
     * Otherwise focus stays on the window you left behind and typing goes to a
     * desktop you can no longer see. Covers every way of getting here — the
     * view: binds, the tray, edge auto-scroll, a three-finger swipe. */
    int arrived = server_active_desktop(server);

    /* Asked of the focused window itself, not of focus_desktop, because that
     * number is a cache and the cache can lie. Anything that places the
     * keyboard deliberately writes it (move_to_view:, a click in the strip) so
     * that the arrival below leaves that choice alone — which also means it can
     * be left naming a desktop the monitor is no longer on, and the arrival
     * that then lands on that very number would skip the re-home entirely and
     * strand the keyboard on a window nobody can see. No window in fwm is
     * sticky, so a focused window on another desktop is always wrong, whatever
     * the cache says. */
    FwmView *focused = server->focused_view;
    PhysicsBody *fb = focused
        ? physics_find_body(&server->physics, focused->id) : NULL;
    int stranded = fb && fb->desktop_id != arrived;

    /* The `desktop` event rides the same test on purpose. It is the only place
     * one is emitted, so a cache that lied dropped the event as well as the
     * re-home — and for a control protocol a missed switch is worse than the
     * duplicate this can cost when a thrown window drifts over the line. */
    if (arrived != server->focus_desktop || stranded) {
        server->focus_desktop = arrived;
        server_refocus(server, arrived, NULL);
        ipc_emit_desktop(server->ipc, arrived);
    }
}

static int physics_tick_cb(void *data) {
    FwmServer *server = data;

    // Tick intervals
    double dt = 1.0 / PHYSICS_TICK_RATE;

    /* How much real time the simulation still owes.
     *
     * The step itself stays fixed at 1/60 — Box2D wants that, and every
     * animation below is written against it — but the TIMER cannot deliver it.
     * wl_event_source_timer_update takes whole milliseconds, so a 60Hz tick is
     * armed at 16ms and actually fires at 62.5Hz, and a busy moment delays it
     * further. Simulated time then runs ahead of the clock, in a pattern that
     * has nothing to do with the display's, and a slow rotation drawn from it
     * beats against the refresh: smooth for half a second, then a visible skip.
     *
     * So take real time in, and spend it in whole steps: usually one, sometimes
     * none, occasionally two. What is left over is `sim_accum`, which is
     * exactly how far behind the clock the simulation is — and therefore
     * exactly what the renderer must extrapolate over (server_render_angle). */
    struct timespec tick_now;
    clock_gettime(CLOCK_MONOTONIC, &tick_now);
    double elapsed = dt;
    if (server->tick_real_prev.tv_sec || server->tick_real_prev.tv_nsec) {
        elapsed = (double)(tick_now.tv_sec - server->tick_real_prev.tv_sec)
                + (double)(tick_now.tv_nsec - server->tick_real_prev.tv_nsec) / 1e9;
        /* After a real stall (VT switch, a big decode) do not try to catch up
         * on the whole gap; the steps cap below would clamp it anyway, and
         * pretending the world moved that far is worse than losing the time. */
        if (elapsed < 0.0)  elapsed = 0.0;
        if (elapsed > 0.25) elapsed = dt;
    }
    server->tick_real_prev = tick_now;
    server->sim_accum += elapsed;

    /* Never more than a couple of steps in one go: a machine that cannot keep
     * up must fall behind in real time rather than spiral, spending ever longer
     * in the tick trying to catch up with itself. */
    int steps = (int)(server->sim_accum / dt);
    if (steps > 2) {
        /* More owed than we are willing to pay in one go — a stall, or the idle
         * heartbeat, where the timer deliberately fires seconds apart. Write the
         * debt off rather than carrying it: an accumulator that only ever grows
         * would leave the renderer extrapolating over a lag that never shrinks. */
        steps = 2;
        server->sim_accum = 0.0;
    } else {
        server->sim_accum -= steps * dt;
        if (server->sim_accum < 0.0) server->sim_accum = 0.0;
    }

    /* interactive.view goes NULL when the dragged client exits mid-drag, while
     * the action stays — every read of it has to allow for that. */
    uint32_t drag_win = (server->interactive.action == FWM_ACTION_MOVE && server->interactive.collision_disabled
                         && server->interactive.view) ? server->interactive.view->id : 0;
    uint32_t dragged_win = (server->interactive.action == FWM_ACTION_MOVE && server->interactive.view)
                         ? server->interactive.view->id : 0;
    // Freeze the window being resized into a static anchor (skip_b): it must not
    // sink under gravity, get shoved by neighbors, or jitter while its collision
    // box is rebuilt every motion event — the mouse owns it entirely.
    // A window being turned by hand is frozen the same way and for the same
    // reason: the hand owns its angle, and a window that slid or fell away
    // from under the cursor mid-twist would be turning about a centre that is
    // no longer where the hand is reaching.
    uint32_t resize_win = ((server->interactive.action == FWM_ACTION_RESIZE ||
                            server->interactive.action == FWM_ACTION_TWIST) &&
                           server->interactive.view) ? server->interactive.view->id : 0;

    if (resize_win) {
        PhysicsBody *rb = physics_find_body(&server->physics, resize_win);
        if (rb) { rb->flying = 0; rb->vx = 0; rb->vy = 0; }
    }
    
    if (server->interactive.action == FWM_ACTION_MOVE && server->interactive.view) {
        physics_set_velocity(&server->physics, server->interactive.view->id, server->interactive.vx, server->interactive.vy);
    }
    
    // Desktop-switch camera slide: fixed-duration ease-in-out instead of the
    // old exponential chase (fast jump + 1px/tick crawl tail). If the target
    // changes mid-flight, restart from the current position so it stays smooth.
    //
    // Per monitor: each screen slides its own camera, so switching desktops on
    // one of them leaves the other where it was.
    int any_settled = 0;
    FwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (out->camera_x == out->target_camera_x && !out->cam_anim) continue;

        // X11 clients place popups from their last-configured root coords;
        // tell them where they are once the camera comes to rest.
        int cam_settled = 0;

        if (out->cam_free) {
            // Continuous pan under a held bind: framerate-independent
            // exponential chase, same form as the tile glide. Unlike the slide
            // below it has no notion of "start over", so a target that moves
            // every 40ms costs nothing — the camera tracks it immediately and
            // coasts the last few px once the key is released.
            out->cam_anim = 0;
            int gap = out->target_camera_x - out->camera_x;
            if (gap != 0) {
                double speed = server->config.camera.free_speed;
                double k = speed > 0.0 ? 1.0 - exp(-speed * dt) : 1.0;
                int step = (int)lround(gap * k);
                if (step == 0) step = gap > 0 ? 1 : -1; // never stall sub-pixel
                out->camera_x += step;
                // Snap the last pixel: edge auto-scroll only ever fires while
                // camera_x == target_camera_x exactly.
                if (abs(out->target_camera_x - out->camera_x) <= 1) {
                    out->camera_x = out->target_camera_x;
                }
                cam_settled = out->camera_x == out->target_camera_x;
            }
        } else {
            if (!out->cam_anim || out->cam_anim_to != out->target_camera_x) {
                out->cam_anim = 1;
                out->cam_anim_from = out->camera_x;
                out->cam_anim_to = out->target_camera_x;
                out->cam_anim_t = 0.0;
            }
            double cam_ms = server->config.camera.anim_ms;
            out->cam_anim_t += cam_ms > 0.0 ? dt * 1000.0 / cam_ms : 1.0;
            double t = out->cam_anim_t;
            if (t >= 1.0) {
                out->camera_x = out->cam_anim_to;
                out->cam_anim = 0;
                cam_settled = 1;
            } else {
                // Cubic ease-in-out.
                double e = t < 0.5 ? 4.0 * t * t * t
                                   : 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0;
                out->camera_x = out->cam_anim_from
                    + (int)lround((out->cam_anim_to - out->cam_anim_from) * e);
            }
        }

        wallpaper_update(out->wallpaper, out->camera_x);
        any_settled |= cam_settled;

        // Every window this monitor shows moves with it. Cheap enough to sweep
        // the whole list: server_place_node sends the ones it is not showing
        // off the layout, which is where they already are.
        FwmView *view;
        wl_list_for_each(view, &server->views, link) {
            if (view->id == dragged_win || !view->scene_tree) continue;
            PhysicsBody *body = physics_find_body(&server->physics, view->id);
            if (body) server_place_node(server, &view->scene_tree->node, body->x, body->y);
        }
    }
    /* A window in your hand goes where you go. The loop above moved every window
     * the camera shows EXCEPT the dragged one, which is placed by the hand and
     * so has to be carried across by its anchor instead — otherwise dragging a
     * window into the screen edge scrolls you to the next desktop and leaves the
     * window behind on the old one. After the slide, so it follows the camera
     * within the same frame, and before physics_step, so the shove it hands out
     * on the way is part of this tick's simulation. */
    server_drag_follow_camera(server);
    if (any_settled) server_camera_settled(server);

    // Tile-glide animations: ease windows toward their tile slots (Hyprland-
    // style) instead of teleporting. Exponential approach is frame-rate
    // independent; the physics bridge sees these as external writes and keeps
    // the Box2D body glued to the glide.
    {
        double k = 1.0 - exp(-server->config.tiling.anim_speed * dt);
        FwmView *av;
        wl_list_for_each(av, &server->views, link) {
            if (!av->tile_anim) continue;
            PhysicsBody *pb = physics_find_body(&server->physics, av->id);
            if (!pb) { av->tile_anim = 0; continue; }
            int settled = 0;
            double dx = av->tile_tx - pb->x;
            double dy = av->tile_ty - pb->y;
            if (fabs(dx) < 1.0 && fabs(dy) < 1.0) {
                pb->x = av->tile_tx;
                pb->y = av->tile_ty;
                av->tile_anim = 0;
                settled = 1;
            } else {
                pb->x += dx * k;
                pb->y += dy * k;
            }
            pb->vx = 0; pb->vy = 0; pb->flying = 0;
            av->x = pb->x;
            av->y = pb->y;
            /* An X11 client places its menus from the root coords it was last
             * configured with, so a window that glided to a new slot without
             * being told would open them back at the old slot until something
             * else resynced it (a desktop switch did). Once per glide, at the
             * end: mid-flight configures would be a configure storm for a
             * position the window is about to leave anyway. */
            if (settled) view_sync_position(av);
        }
    }

    /* A spinning window being dragged hangs from the point it was grabbed by.
     * Runs on the tick rather than on pointer motion because a pendulum keeps
     * swinging after the hand stops, and because differentiating the cursor
     * twice needs a fixed dt — which is also why it advances once per STEP,
     * not once per tick: a tick that paid for no step must not swing it. */
    for (int i = 0; i < steps; i++) server_drag_swing(server, dt);

    // Tab bars follow their window's width (resize/tiling glides).
    group_tick(server);

    // Launcher: tile physics + overlay redraw while open.
    launcher_tick(server->launcher, dt);

    // Modes menu: knobs sliding, the cava highlight travelling, rows staggering
    // in. Uses `elapsed`, not the fixed step — these are wall-clock animations
    // with nothing in the simulation depending on them.
    if (server->modes_buffer) {
        ModesState ms;
        server_modes_state(server, &ms);
        modes_menu_tick(server->modes_buffer, &ms, elapsed);
    }

    /* And the stats menu's, on the same terms. Not optional and not merely
     * decorative: the rows are staggered in by this tick from a reveal of zero,
     * so a menu nobody advances is a panel with nothing drawn in it. */
    if (server->stats_buffer)
        stats_menu_tick(server->stats_buffer, server->stats,
                        server->config.decor.tray_opacity, elapsed);

    /* The readouts. Sampling is cheap on the ticks where nothing is due, and
     * only a value that CHANGED asks for a repaint — a tray redrawn at the
     * frame rate to show the same number is the whole thing the strip's
     * signature exists to prevent. */
    if (stats_tick(server->stats, elapsed)) {
        server_request_tray_redraw(server);
        /* The open menu shows each sensor's live value beside its switch, so it
         * is as stale as the pill would be. */
        if (server->stats_buffer)
            stats_menu_redraw(server->stats_buffer, server->stats,
                              server->config.decor.tray_opacity);
    }

    /* Audio spectrum. Analysed once per TICK rather than once per step: it is
     * driven by the sound card's clock, not the simulation's, and running the
     * FFT twice over a tick that paid for two steps would just decay the bars
     * twice as fast on a slow frame. */
    server_cava_sync(server);
    if (server->cava) cava_tick(server->cava, &server->config.cava, elapsed);

    /* The collision mixer: started, stopped, or left alone. No device is open
     * unless something is actually being played. */
    server_sound_sync(server);

    /* What every window weighs, when that is decided by something outside the
     * simulation. On its own timer inside, so calling it every tick is cheap. */
    server_mass_sync(server);

    /* The desktop strip shows still pictures of every desktop. Letting the
     * simulation run behind them would mean the windows you are looking at have
     * quietly moved by the time you drop one — and a window dropped onto a card
     * would be shoved by a world it was never in. Freeze it instead; nothing
     * else in the tick needs to stop, the tray and the wallpaper still live. */
    /* The ring reaches the simulation the same way gravity does: read every
     * tick, so toggling it catches a throw already in the air. */
    server->physics.wrap = server->config.camera.wrap;

    /* And so does the space bars have reserved, when [physics] solid_bars asks
     * for it. The primary monitor's, because the world is a strip of columns
     * the size of that monitor and the floor is one line across all ten — a
     * bar on the second screen has no column of its own to stand in. */
    server->physics.inset_top = server->physics.inset_bottom = 0;
    if (server->config.physics.solid_bars) {
        struct wlr_box u = server->usable_area;
        if (u.width > 0 && u.height > 0) {
            int top = u.y, bottom = server->screen_height - (u.y + u.height);
            /* A bar taller than the screen would leave the floor above the
             * ceiling and Box2D solving an impossible world. */
            if (top >= 0 && bottom >= 0 && top + bottom < server->screen_height) {
                server->physics.inset_top = top;
                server->physics.inset_bottom = bottom;
            }
        }
    }

    /* And so does the height of the glass each desktop is being shown on. The
     * floor belongs to the monitor, not to the strip: on a 1080p screen beside
     * a 1440p one, a floor at the strip's height is 360px below the bottom of
     * the smaller screen and windows settle out of sight. Read every tick, so a
     * hotplug or a mode change moves the floor under whatever is resting on it.
     *
     * Only the height. A desktop's WIDTH is the strip's stride and cannot vary
     * per monitor: a window has to be able to be carried from desktop d to
     * d+1, so there is nothing solid to put at the edge of a column. A monitor
     * narrower than the stride still has dead space along its right — that half
     * of #20 needs the strip itself to change shape, not another wall. */
    for (int d = 0; d < FWM_DESKTOPS; d++) {
        FwmOutput *mon = server_output_showing(server, d);
        server->physics.desktop_h[d] = (mon && mon->box.height > 0) ? mon->box.height : 0;
    }

    if (expo_active(server)) steps = 0;

    /* Physics steps: as many whole 1/60 steps as the real time since the last
     * tick paid for (see the accumulator at the top). Usually one. */
    for (int i = 0; i < steps; i++) {
        /* The bar row stands on the floor of the desktop the camera is parked
         * on. Anchoring it to the ACTIVE DESKTOP rather than to camera_x is
         * deliberate: the row is kinematic and infinitely heavy, so sliding it
         * sideways during a desktop switch would sweep every window it passed
         * into the far wall. It jumps a whole screen once instead. */
        int bar_n = 0;
        const float *bar_lvl = cava_levels(server->cava, &bar_n);
        if (bar_lvl && bar_n > 0) {
            int bar_desk = server_active_desktop(server);
            /* A monitor that is off carries desktop -1, and the active output
             * is only ever an enabled one — but this now indexes an array, so
             * "in practice" is no longer a good enough reason not to check. */
            if (bar_desk < 0 || bar_desk >= FWM_DESKTOPS) bar_desk = 0;
            int bar_h = server->physics.desktop_h[bar_desk] > 0
                      ? server->physics.desktop_h[bar_desk] : server->screen_height;
            physics_set_bars(&server->physics, bar_lvl, bar_n,
                             (double)bar_desk * server->screen_width,
                             (double)server->screen_width,
                             server->config.cava.height * server->config.cava.push,
                             /* The row rises out of the FLOOR, wherever that
                              * currently is: with solid_bars on and a dock at
                              * the bottom, the floor is the top of the dock and
                              * a visualiser sunk below it would never show. And
                              * the floor is now that desktop's monitor, so a
                              * row on a short screen rises out of the short
                              * screen rather than from under it. */
                             bar_h - server->physics.inset_bottom,
                             BAR_MAX_RISE_SPEED * server->config.cava.push, dt);
        }

        physics_step(&server->physics, server->screen_width, server->screen_height,
                     drag_win, resize_win, dragged_win, dt);
        /* The step that just ran is the first to carry mass = "ram" weights, so
         * this is the earliest the freeze can read a real mass. */
        if (server->hp_freeze_pending) {
            physics_freeze_hp(&server->physics, 1);
            server->hp_freeze_pending = 0;
        }
        /* Impacts are only valid until the NEXT step, so they have to be drained
         * inside the loop — collecting them after two steps would lose the
         * first step's landings entirely. */
        server_consume_impacts(server);
    }

    /* The instant the simulation's state became current. Rendering measures its
     * lag from here, plus whatever the accumulator still owes
     * (server_render_angle). */
    clock_gettime(CLOCK_MONOTONIC, &server->last_tick);

    // Synchronize scene tree nodes to physics coordinates
    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->scene_tree) {
            PhysicsBody *body = physics_find_body(&server->physics, view->id);
            if (body && !body->pinned && view->id != dragged_win) {
                view->x = body->x;
                view->y = body->y;
                server_place_node(server, &view->scene_tree->node, body->x, body->y);
            }
        }
        /* Which screen a panel sees this window on. Here because a window
         * changes desktop by crossing the seam under its own momentum — there
         * is no event to hang it off, only the position it ended up at. The
         * call returns immediately unless the answer changed. */
        foreign_view_sync_output(view);

        /* The unfocused dim. Read off the focus every tick rather than hung
         * off server_focus_view: a window can stop being the focused one
         * without that function being called about it — mapped onto another
         * desktop, hidden inside a tab stack, left behind when the keyboard
         * went to an override-redirect surface — and every one of those has to
         * end up at the same place as an ordinary click elsewhere. */
        view_dim_set(view, view == server->focused_view
                           ? 1.0 : server->config.decor.inactive_opacity, false);
        view_dim_tick(view, dt);
    }

    /* Where the light is. After the windows have been placed and before the
     * frame is scheduled, so a shadow never lags the window it belongs to by a
     * frame. Costs nothing on the ticks where the sun has not moved. */
    server_sun_sync(server);

    /* And which monitor is showing which desktop, for the same reason and by
     * the same rule: only what changed goes out. */
    workspace_sync(server);

    // Hide the tray while a real-fullscreen window occupies the active desktop
    // (overlays outrank windows in the scene, so the surface can't cover it).
    // Fake fullscreen keeps the tray — that's its point. Checking every tick
    // also covers desktop switches and the fullscreen window closing.
    /* Per monitor: a fullscreen window on one screen must not blank the other
     * screen's tray or freeze its wallpaper. */
    bool any_real_fs = false;
    FwmOutput *fo;
    wl_list_for_each(fo, &server->outputs, link) {
        bool real_fs = false;    /* hides this monitor's tray */
        bool wp_covered = false; /* hides its wallpaper: real OR fake fullscreen */
        FwmView *fsv;
        wl_list_for_each(fsv, &server->views, link) {
            PhysicsBody *fb = physics_find_body(&server->physics, fsv->id);
            if (!fb || !fb->fullscreen || fb->desktop_id != fo->desktop) continue;
            wp_covered = true;
            if (fsv->fs_real) { real_fs = true; break; } /* also hides the tray */
        }
        /* Real fullscreen hides the tray; fake fullscreen deliberately keeps
         * it. A user-hidden tray stays hidden through both, and so does one
         * that stood down for an external bar on this screen. */
        bool yielded = server->config.decor.tray_yield && fo->top_reserved;
        if (fo->tray_buffer)
            wlr_scene_node_set_enabled(&fo->tray_buffer->node,
                                       !real_fs && !server->tray_hidden && !yielded);
        /* Either kind fully hides the wallpaper (fake fills the work area and
         * the tray covers the strip above it), so pause a video behind it: the
         * decode thread then blocks on its full queue and stops burning CPU. */
        wallpaper_set_paused(fo->wallpaper, wp_covered);
        any_real_fs |= real_fs;
    }
    bool real_fs = any_real_fs;

    /* The modes menu hangs off a pill that is no longer on screen — and unlike
     * the tray it is not merely disabled, it is a panel floating over a
     * fullscreen window with nothing left to explain it. */
    if (server->modes_buffer && (real_fs || server->tray_hidden))
        server_close_modes_menu(server);
    if (server->stats_buffer && (real_fs || server->tray_hidden))
        server_close_stats_menu(server);

    server_video_sync(server); /* (dis)arm the video-frame timer for the new state */

    // Redraw tray if data changed
    server_request_tray_redraw(server);

    idle_inhibit_refresh(server);

    // Parallax: shift each wallpaper layer by a fraction of its own monitor's
    // camera offset.
    wl_list_for_each(fo, &server->outputs, link)
        wallpaper_update(fo->wallpaper, fo->camera_x);

    /* While anything is actually moving we drive the frame loop ourselves at
     * the full tick rate. Once everything settles we drop to a slow heartbeat
     * instead of spinning at 60Hz forever: client redraws and scene node moves
     * damage the scene, and wlr_scene schedules a frame off that damage on its
     * own, so nothing depends on us for those.
     *
     * The heartbeat is not just for the tray clock. It is the safety net that
     * the old unconditional schedule used to provide: should any path ever
     * change what is on screen without damaging the scene, the screen is stale
     * for at most one beat instead of forever. */
    int busy = server_is_busy(server);
    server_schedule_frames(server);

    /* physics_step always advances a FIXED 1/60 regardless of when we are
     * called, so slowing the timer cannot hand Box2D an enormous dt — idle
     * simply means nothing is moving for it to integrate. */
    server->tick_idle = !busy;
    wl_event_source_timer_update(server->physics_timer,
                                 busy ? (int)(1000.0 / PHYSICS_TICK_RATE) : TICK_IDLE_MS);
    return 0;
}

/* Create the timers this file owns. The physics timer starts armed; the video
 * one stays disarmed until a video wallpaper actually plays (server_video_sync).
 * Here rather than in the lifecycle file so both callbacks can stay static. */
void server_tick_register(FwmServer *server, struct wl_event_loop *event_loop) {
    server->physics_timer = wl_event_loop_add_timer(event_loop, physics_tick_cb, server);
    wl_event_source_timer_update(server->physics_timer, (int)(1000.0 / PHYSICS_TICK_RATE));
    server->video_timer = wl_event_loop_add_timer(event_loop, video_timer_cb, server);
}
