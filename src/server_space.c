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

/* The solar-system wallpaper, wired into the compositor.
 *
 * space.c is the world and space_draw.c the picture; this is what tells the
 * world what the compositor is doing. It does that by LOOKING, once a frame,
 * not by being told: the windows are the active physics bodies (the same
 * count the tray's desktop pill shows), the focus is server->focused_view,
 * each monitor's desktop is its own. Nothing in the window code had to learn
 * that a wallpaper cares — and a change made while nobody could see it (a
 * fullscreen game over the wallpaper) is simply found on the next look, and
 * applied without the explosions and flights it would have had.
 *
 * It runs on a timer of its own at [space] fps, the way a video wallpaper
 * does, not on the physics tick: holding the tick at 60Hz for a picture that
 * changes thirty times a second would cost the CPU twice for nothing. */

#include "server.h"
#include "view.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "grass.h"
#include "physics.h"
#include "server_internal.h"
#include "space_draw.h"
#include "space_gl.h"
#include "star_draw.h"

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double wall_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ── which monitors ──────────────────────────────────────────────────── */

static int pick_index(FwmServer *server, const char *name) {
    for (int i = 0; i < server->space_output_count; i++)
        if (strcmp(server->space_outputs[i], name) == 0) return i;
    return -1;
}

/* A monitor with a [[wallpaper]] layer of its own — configured, or made by
 * picking an image for it — shows that, not the un-named set. */
static bool has_own_layer(FwmServer *server, const char *name) {
    for (int i = 0; i < server->config.wallpaper_count; i++)
        if (strcmp(server->config.wallpapers[i].output, name) == 0) return true;
    return false;
}

bool server_space_on_output(FwmServer *server, FwmOutput *out) {
    if (!server || !out || !out->wlr_output) return false;
    const char *name = out->wlr_output->name;
    if (pick_index(server, name) >= 0) return true;
    return pick_index(server, "") >= 0 && !has_own_layer(server, name);
}

void server_space_pick(FwmServer *server, const char *output, bool on) {
    if (!output) output = "";
    int i = pick_index(server, output);
    if (on && i < 0) {
        if (server->space_output_count == FWM_SPACE_OUTPUTS) {
            wlr_log(WLR_ERROR, "space: picked for more than %d monitors — \"%s\" left out",
                    FWM_SPACE_OUTPUTS, output);
            return;
        }
        snprintf(server->space_outputs[server->space_output_count++],
                 sizeof(server->space_outputs[0]), "%s", output);
    } else if (!on && i >= 0) {
        server->space_outputs[i][0] = '\0';
        memmove(&server->space_outputs[i], &server->space_outputs[i + 1],
                sizeof(server->space_outputs[0]) * (size_t)(server->space_output_count - i - 1));
        server->space_output_count--;
    }
}

/* What it is showing: lit, awake, picked, and with somewhere to draw. */
static bool shows(FwmServer *server, FwmOutput *out) {
    return out->enabled && !out->idle_blanked && out->box.width > 0 && out->box.height > 0
        && server_space_on_output(server, out);
}

/* A fullscreen window on this monitor's desktop hides the wallpaper whole —
 * real or fake, the same test the video wallpaper sleeps on. */
static bool covered(FwmServer *server, FwmOutput *out) {
    FwmView *v;
    wl_list_for_each(v, &server->views, link) {
        PhysicsBody *b = physics_find_body(&server->physics, v->id);
        if (b && b->fullscreen && b->desktop_id == out->desktop) return true;
    }
    return false;
}

static void drop_draw(FwmOutput *out) {
    space_draw_destroy(out->space_draw);
    out->space_draw = NULL;
}

/* ── one frame ───────────────────────────────────────────────────────── */

/* The windows as the tray counts them: active bodies, on the desktop their
 * middle is over. */
static int gather_windows(FwmServer *server, SpaceWindow **buf, int *cap) {
    int need = server->physics.body_count;
    if (need > *cap) {
        SpaceWindow *nb = realloc(*buf, sizeof(SpaceWindow) * (size_t)need);
        if (!nb) return 0;
        *buf = nb;
        *cap = need;
    }
    int n = 0;
    for (int i = 0; i < server->physics.body_count; i++) {
        const PhysicsBody *b = &server->physics.bodies[i];
        if (!b->active || server->screen_width <= 0) continue;
        int d = (int)floor((b->x + b->width / 2.0) / server->screen_width);
        if (d < 0 || d >= FWM_DESKTOPS) continue;
        (*buf)[n].id = b->id;
        (*buf)[n].desktop = d;
        n++;
    }
    return n;
}

static void space_frame(FwmServer *server) {
    static SpaceWindow *wins = NULL;
    static int wins_cap = 0;
    static bool gl_failed_logged = false;

    if (server->renderer_recovering) return;
    const SpaceConfig *cfg = &server->config.space;
    double real = now_s();
    bool fresh = false;
    if (!server->space) {
        server->space = calloc(1, sizeof(Space));
        if (!server->space) return;
        space_init(server->space, cfg, wall_s(), (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32));
        server->space_last = real;
        fresh = true;
    }
    Space *sp = server->space;
    double dt = real - server->space_last;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.25) dt = 0.25;
    server->space_last = real;

    /* Who is looking at which desktop, and is anyone looking at all. */
    SpaceCam *cams[SPACE_PLANETS] = { 0 };
    bool any_awake = false;
    double aspect = 16.0 / 9.0;
    FwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (!shows(server, out)) { if (out->space_draw) drop_draw(out); continue; }
        if (covered(server, out)) continue;
        any_awake = true;
        aspect = (double)out->box.width / out->box.height;
        if (!out->space_asleep && out->desktop >= 0 && out->desktop < SPACE_PLANETS)
            cams[out->desktop] = &out->space_cam;
    }

    space_step(sp, cfg, dt, real);
    int n = gather_windows(server, &wins, &wins_cap);
    space_sync_windows(sp, wins, n, cams, aspect, real, fresh || !any_awake);
    space_sync_focus(sp, server->focused_view ? server->focused_view->id : 0);

    wl_list_for_each(out, &server->outputs, link) {
        if (!shows(server, out)) continue;
        if (covered(server, out)) { out->space_asleep = true; continue; }
        bool quiet = fresh || out->space_asleep;
        out->space_asleep = false;

        int w = out->box.width, h = out->box.height;
        if (out->space_draw && !space_draw_fits(out->space_draw, w, h, cfg->scale)) drop_draw(out);
        if (!out->space_draw) {
            out->space_draw = space_draw_create(server, server->layer_background, w, h, cfg->scale);
            if (!out->space_draw) continue;
            /* Over the wallpaper; the grass and the star stay over it. */
            space_draw_raise(out->space_draw);
            if (out->grass) grass_raise(out->grass);
            if (out->star_draw) star_draw_raise(out->star_draw);
            quiet = true;
        }
        space_draw_place(out->space_draw, out->box.x, out->box.y);

        space_cam_update(sp, &out->space_cam, out->desktop, real, quiet);
        SpaceUniforms u;
        space_uniforms(sp, &out->space_cam, cfg, real, (double)w / h, &u);
        if (space_draw_render(out->space_draw, &u) == SPACE_GL_FAILED) {
            if (!gl_failed_logged)
                wlr_log(WLR_ERROR, "space: could not draw — the wallpaper under it stays");
            gl_failed_logged = true;
            drop_draw(out);
        }
    }
}

/* ── the timer ───────────────────────────────────────────────────────── */

static bool wanted(FwmServer *server) {
    if (server->space_output_count == 0) return false;
    FwmOutput *out;
    wl_list_for_each(out, &server->outputs, link)
        if (shows(server, out) && !covered(server, out)) return true;
    return false;
}

/* The full rate while anything is happening on any screen showing it, the
 * calm one otherwise. */
static int interval_ms(FwmServer *server) {
    const SpaceConfig *cfg = &server->config.space;
    bool busy = !server->space;
    FwmOutput *out;
    wl_list_for_each(out, &server->outputs, link)
        if (!busy && shows(server, out) && server->space
            && space_busy(server->space, &out->space_cam, now_s()))
            busy = true;
    int ms = (int)lround(1000.0 / (busy ? cfg->fps : cfg->calm_fps));
    return ms < 1 ? 1 : ms;
}

static int space_timer_cb(void *data) {
    FwmServer *server = data;
    space_frame(server);
    if (wanted(server)) {
        wl_event_source_timer_update(server->space_timer, interval_ms(server));
    } else {
        server->space_timer_on = false;
        /* Nobody is looking: leave the last picture up, but let go of the
         * pictures of monitors it is no longer picked for. */
        FwmOutput *out;
        wl_list_for_each(out, &server->outputs, link)
            if (out->space_draw && !shows(server, out)) drop_draw(out);
    }
    return 0;
}

void server_space_sync(FwmServer *server) {
    if (!server->space_timer) {
        if (server->space_output_count == 0) {
            /* Never picked: nothing to arm, and no reason to keep pictures. */
            FwmOutput *out;
            wl_list_for_each(out, &server->outputs, link)
                if (out->space_draw) drop_draw(out);
            return;
        }
        struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
        server->space_timer = wl_event_loop_add_timer(loop, space_timer_cb, server);
        if (!server->space_timer) return;
    }
    bool want = wanted(server);
    if (want && !server->space_timer_on) {
        server->space_timer_on = true;
        wl_event_source_timer_update(server->space_timer, 1);
    } else if (!want && server->space_timer_on) {
        server->space_timer_on = false;
        wl_event_source_timer_update(server->space_timer, 0);
        FwmOutput *out;
        wl_list_for_each(out, &server->outputs, link)
            if (out->space_draw && !shows(server, out)) drop_draw(out);
    }
}

void server_space_config(FwmServer *server) {
    if (!server->space) return;
    server_space_sync(server);
    space_apply_config(server->space, &server->config.space, now_s(), !server->space_timer_on);
}

void server_space_gpu_release(FwmServer *server) {
    FwmOutput *out;
    wl_list_for_each(out, &server->outputs, link)
        if (out->space_draw) drop_draw(out);
    space_gl_finish();
}

void server_space_output_gone(FwmServer *server, FwmOutput *out) {
    (void)server;
    if (out->space_draw) drop_draw(out);
}

void server_space_finish(FwmServer *server) {
    FwmOutput *out;
    wl_list_for_each(out, &server->outputs, link)
        if (out->space_draw) drop_draw(out);
    if (server->space_timer) {
        wl_event_source_remove(server->space_timer);
        server->space_timer = NULL;
    }
    server->space_timer_on = false;
    free(server->space);
    server->space = NULL;
    space_gl_finish();
}
