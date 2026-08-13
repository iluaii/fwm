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

#include "grass.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ui/cairo_overlay.h"

/* A blade per 3px at density 32 across 4K is ~2000 of them, and each one is a
 * filled bezier. Past that the strip stops reading as more grass and starts
 * reading as a solid green bar that costs more to paint. */
#define GRASS_MAX_BLADES 4096

/* Where a blade sits between "stands up again the moment the gust passes" and
 * "keeps swinging after it". Springy rather than critically damped: grass that
 * eases back to upright reads as rubber, and the overshoot is most of what makes
 * a gust look like moving air. */
#define BEND_STIFFNESS 24.0
#define BEND_DAMPING   3.4

/* Windows in the strip at once. Past this the extra ones are ignored rather
 * than the whole list refused: a blade held down by nine windows instead of ten
 * is not a bug anyone can see. */
#define GRASS_MAX_WINDOWS 32

/* How hard a moving window drags the blades it is standing in. Divides the
 * window's speed, so a window shoved across the floor at 900 px/s lays them
 * over about as far as a strong gust does. */
#define DRAG_REFERENCE_SPEED 900.0

/* How fast a window may push a blade down, in bend units per second. A blade
 * goes from upright to flat in about a fifth of a second, which is quick enough
 * to look like the window is doing it and slow enough to be a movement rather
 * than a jump. */
#define BEND_PRESS_RATE 7.0

typedef struct {
    float x;      /* base centre, px from the left of the strip */
    float len;    /* how far up the strip the tip reaches, px */
    float halfw;  /* half the base width, px */
    float lean;   /* resting sideways drift of the tip, as a fraction of len */
    float depth;  /* 0 at the back of the strip, 1 at the front */
    float shade;  /* per-blade brightness jitter around the depth shading */
    /* Wind state. `bend` is added to `lean`, in the same units: how far the tip
     * has been pushed sideways as a fraction of the blade's own length. */
    float bend, vel;
    float drawn_bend; /* the bend the pixels on screen were painted with */
    float stiff;  /* per-blade multiplier on the spring; nothing moves in step */
    float phase;  /* per-blade offset into the wind field, for the same reason */
} Blade;

struct FwmGrass {
    struct wlr_scene_buffer *buf;
    Blade *blade;
    int    count;
    int    width, height;
    double t;          /* seconds of wind blown so far */
    double since_draw; /* seconds since the last repaint; see [grass] fps */
    bool   moving;     /* anything still swaying as of the last tick */
    bool   paused;     /* nothing can see the strip; see grass_set_paused */
    /* What the pixels on screen were painted with, so a colour changed on a
     * STILL lawn still reaches the screen — nothing else would ask for the
     * repaint. */
    double drawn_opacity;
    float  drawn_color[4];
    GrassWindow win[GRASS_MAX_WINDOWS];
    int    win_count;
    GrassConfig cfg;   /* the live config, re-read every tick; see grass_stale */
};

/* Deterministic and self-contained: the strip is repainted from the same blades
 * every time, and seeding rand() from here would tug at whatever else in the
 * compositor happens to use it. */
static unsigned xr_next(unsigned *s) {
    unsigned x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}
static float xr_unit(unsigned *s) { return (float)(xr_next(s) & 0xffffff) / (float)0x1000000; }
static float xr_range(unsigned *s, float lo, float hi) { return lo + xr_unit(s) * (hi - lo); }

/* Back to front, so a blade nearer the viewer covers the ones behind it. */
static int by_depth(const void *a, const void *b) {
    float d = ((const Blade *)a)->depth - ((const Blade *)b)->depth;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

static void grow_blades(FwmGrass *g, const GrassConfig *cfg) {
    int n = (int)lround(cfg->density * g->width / 100.0);
    if (n < 1) n = 1;
    if (n > GRASS_MAX_BLADES) n = GRASS_MAX_BLADES;

    g->blade = calloc((size_t)n, sizeof(*g->blade));
    if (!g->blade) { g->count = 0; return; }
    g->count = n;

    /* Seeded from the width so a second monitor of a different size grows a
     * different patch, while the same screen looks the same across a restart. */
    unsigned seed = 0x9e3779b9u ^ (unsigned)g->width;
    if (!seed) seed = 1;

    /* Evenly spaced slots with jitter inside each, rather than n independent
     * uniform draws: uniform x leaves bald patches and clumps that read as a
     * mistake at this density. */
    double slot = (double)g->width / n;
    for (int i = 0; i < n; i++) {
        Blade *b = &g->blade[i];
        b->x     = (float)((i + 0.5) * slot + xr_range(&seed, -0.5f, 0.5f) * slot * 1.6f);
        b->depth = xr_unit(&seed);
        /* Depth is the whole sense of a third dimension here: blades at the
         * back are shorter, thinner and darker, and the eye reads that as
         * distance rather than as an untidy row. */
        float near = 0.55f + 0.45f * b->depth;
        b->len   = (float)cfg->height * near * xr_range(&seed, 0.72f, 1.0f);
        b->halfw = (float)cfg->width * near * xr_range(&seed, 0.75f, 1.15f) * 0.5f;
        if (b->halfw < 0.4f) b->halfw = 0.4f;
        /* Signed, so a patch leans both ways and looks grown rather than combed. */
        b->lean  = xr_range(&seed, -0.42f, 0.42f);
        b->shade = xr_range(&seed, 0.88f, 1.12f);
        /* A short stiff blade and a long limber one in the same gust is what
         * keeps the patch from moving like one painted sheet. */
        b->stiff = xr_range(&seed, 0.75f, 1.45f);
        b->phase = xr_range(&seed, 0.0f, 6.2832f);
    }

    qsort(g->blade, (size_t)n, sizeof(*g->blade), by_depth);
}

/* One blade: up the left edge to the tip, back down the right edge. Two cubics
 * rather than a stroked line, because a blade tapers — a stroke of even width
 * looks like wire, and the taper is most of what says "grass".
 *
 * `drift` is how far the tip is off to one side, as a fraction of the blade's
 * length: its resting lean plus whatever the wind is doing to it. */
static void blade_path(cairo_t *cr, const Blade *b, double base_y, double drift) {
    double x = b->x, w = b->halfw;
    /* A blade bent over does not get longer, it gets LOWER: without this the
     * tips ride up as the gust passes and the whole strip looks like it is
     * being stretched sideways rather than blown. */
    double len = b->len / sqrt(1.0 + drift * drift);
    double dx = drift * len;
    double tx = x + dx, ty = base_y - len;

    cairo_move_to(cr, x - w, base_y);
    cairo_curve_to(cr,
                   x - w * 0.85 + dx * 0.10, base_y - len * 0.42,
                   tx - w * 0.30 - dx * 0.05, base_y - len * 0.80,
                   tx, ty);
    cairo_curve_to(cr,
                   tx + w * 0.35 - dx * 0.05, base_y - len * 0.78,
                   x + w * 0.90 + dx * 0.10, base_y - len * 0.40,
                   x + w, base_y);
    cairo_close_path(cr);
}

static void grass_draw(cairo_t *cr, int w, int h, void *data) {
    (void)w;
    FwmGrass *g = data;
    const GrassConfig *cfg = &g->cfg;

    /* parse_hex_color leaves the colour premultiplied (wlr_scene_rect wants it
     * that way); cairo wants it straight. */
    double a = cfg->color[3];
    double cr0 = a > 0.0 ? cfg->color[0] / a : 0.0;
    double cg0 = a > 0.0 ? cfg->color[1] / a : 0.0;
    double cb0 = a > 0.0 ? cfg->color[2] / a : 0.0;
    double alpha = a * cfg->opacity;

    /* Blades are thin, dark and in constant motion, which is exactly the case
     * cairo's cheap antialiasing was meant for — the difference is invisible at
     * this size and it is worth about a fifth of the repaint. */
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_FAST);
    /* And a coarse flattening tolerance. The default 0.1px splits every blade's
     * two cubics into far more line segments than a shape 5px wide and in
     * constant motion can show. */
    cairo_set_tolerance(cr, 0.8);

    double base_y = h;   /* every blade is rooted on the bottom edge of the strip */

    /* A fill per blade, and not a single path of all of them: batching the
     * blades that share a shade into one fill was measured at nearly twice the
     * cost, because cairo tessellates a path against ITSELF and a strip's worth
     * of overlapping subpaths is a lot of intersections to find. Five hundred
     * small independent fills are cheaper than one large self-overlapping one.
     *
     * The blades are drawn back to front, so a near one covers a far one. */
    for (int i = 0; i < g->count; i++) {
        const Blade *b = &g->blade[i];

        /* Distance dims: the back of the patch is darker and slightly bluer,
         * the front keeps the configured colour. */
        double k = (0.52 + 0.48 * b->depth) * b->shade;
        double rr = cr0 * k, gg = cg0 * k, bb = cb0 * (k * 0.92 + 0.08);
        if (rr > 1.0) rr = 1.0;
        if (gg > 1.0) gg = 1.0;
        if (bb > 1.0) bb = 1.0;

        cairo_set_source_rgba(cr, rr, gg, bb, alpha);
        blade_path(cr, b, base_y, b->lean + b->bend);
        cairo_fill(cr);
        g->blade[i].drawn_bend = b->bend;
    }

    g->drawn_opacity = cfg->opacity;
    memcpy(g->drawn_color, cfg->color, sizeof(g->drawn_color));
}

FwmGrass *grass_create(struct wlr_scene_tree *parent, const GrassConfig *cfg,
                       int width, int screen_h) {
    if (!parent || !cfg || !cfg->enabled) return NULL;
    if (width <= 0 || screen_h <= 0) return NULL;

    int height = (int)lround(cfg->height);
    if (height < 4) height = 4;
    /* A strip taller than the screen it stands on is a config typo, not a
     * lawn: cap it rather than allocate a buffer nobody can see past. */
    if (height > screen_h) height = screen_h;

    FwmGrass *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->width  = width;
    g->height = height;
    g->cfg    = *cfg;

    grow_blades(g, cfg);
    if (!g->count) { free(g); return NULL; }

    g->buf = cairo_overlay_create(parent, width, height);
    if (!g->buf) { free(g->blade); free(g); return NULL; }

    cairo_overlay_update(g->buf, grass_draw, g);
    return g;
}

void grass_set_origin(FwmGrass *g, int x, int bottom_y) {
    if (!g || !g->buf) return;
    wlr_scene_node_set_position(&g->buf->node, x, bottom_y - g->height);
}

/* The wind at one point of the strip, in the same units a blade bends in.
 *
 * Two travelling waves and no noise field: a long slow one carries the gust
 * across the screen, a short quick one breaks it up so neighbouring blades are
 * never quite together, and the whole thing is scaled by a swell that takes
 * about half a minute to come round — which is what makes a lull read as a lull
 * rather than as the animation having stopped. */
static double wind_at(const FwmGrass *g, double x, double phase, double t) {
    double slow = sin((x - phase) / 340.0);
    double fast = sin((x - phase * 1.7) / 90.0 + t * 1.3);
    double gust = 0.55 + 0.45 * sin(t * 0.21) * sin(t * 0.083 + 1.7);
    (void)g;
    return (0.72 * slow + 0.28 * fast) * gust;
}

void grass_set_windows(FwmGrass *g, const GrassWindow *win, int count) {
    if (!g) return;
    if (count > GRASS_MAX_WINDOWS) count = GRASS_MAX_WINDOWS;
    if (count < 0) count = 0;
    g->win_count = count;
    if (count > 0 && win) memcpy(g->win, win, (size_t)count * sizeof(*win));
}

/* What the windows do to one blade, on top of what the wind does.
 *
 * `*target` gains the sideways push; the return value is the SMALLEST bend the
 * blade is allowed to have, because a blade standing under a window's lower
 * edge has to be bent far enough for its tip to fit under it. That constraint,
 * rather than any force, is what makes a window look like it is really standing
 * in the grass: the blades at its edges lie further and further over as it
 * comes down, and stand straight back up the moment it lifts. */
static double windows_on_blade(const FwmGrass *g, const Blade *b, double *target) {
    double floor_bend = 0.0;

    for (int i = 0; i < g->win_count; i++) {
        const GrassWindow *w = &g->win[i];
        if (b->x < w->x0 || b->x > w->x1) continue;
        if (w->bottom >= b->len) continue;   /* clear of this blade's tip */

        /* 0 where the edge is level with the tip, 1 where it is on the ground.
         * Clamped, because a window sunk BELOW the ground line (a throw still
         * being resolved, a tiled slot below the floor) is not more than fully
         * in contact. */
        double contact = (b->len - w->bottom) / b->len;
        if (contact > 1.0) contact = 1.0;

        /* Dragged the way the window is going. A window shoved sideways combs
         * the blades it passes through; one sitting still does not. */
        *target += (w->vx / DRAG_REFERENCE_SPEED) * contact * 1.3;

        /* And pushed out from under it: a blade squarely beneath the middle of a
         * window has nowhere to go but the nearer edge, and without this the
         * whole footprint bends whichever way the wind happened to be blowing. */
        double mid = (w->x0 + w->x1) / 2.0;
        *target += (b->x < mid ? -1.0 : 1.0) * contact * 0.55;

        /* The geometric constraint. blade_path shortens a bent blade by
         * 1/sqrt(1+drift²), so the tip is at len/sqrt(1+drift²) — solving that
         * for "at most w->bottom" gives the drift below. Clamped: an edge on the
         * ground would ask for infinity. */
        double h = w->bottom < 1.0 ? 1.0 : w->bottom;
        double ratio = b->len / h;
        double need = sqrt(ratio * ratio - 1.0);
        if (need > 2.5) need = 2.5;
        if (need > floor_bend) floor_bend = need;
    }
    return floor_bend;
}

bool grass_tick(FwmGrass *g, const GrassConfig *cfg, double dt) {
    if (!g || !cfg) return false;

    /* The live config, so opacity, colour and the wind knobs all take effect on
     * the next repaint. What cannot be applied here is the shape of the blades
     * themselves — grass_stale is how the caller hears about that. */
    g->cfg = *cfg;

    /* A tick that fell behind (a frozen session, a slow first frame) is bounded
     * rather than integrated whole — but only bounded, not TRUNCATED. This used
     * to clamp dt to 50ms, which was invisible while the compositor ticked at
     * 60Hz and became a bug the moment the tick started running at [grass] fps:
     * at fps = 15 a 66ms tick had 16ms of its wind quietly thrown away every
     * time, and the whole strip swayed at three quarters speed. */
    if (dt > 0.25) dt = 0.25;
    if (dt <= 0.0 || g->paused) return g->moving;

    /* The spring is integrated in steps of at most 20ms whatever the tick rate,
     * because a stiff spring taken in one 66ms step overshoots and rings. The
     * wind field is smooth over a tick and is sampled once. */
    int steps = (int)ceil(dt / 0.02);
    if (steps < 1)  steps = 1;
    if (steps > 16) steps = 16;
    double sdt = dt / steps;

    g->t += dt;
    double phase = g->t * cfg->wind_speed;

    bool moving = false;
    float worst_shift = 0.0f;
    for (int i = 0; i < g->count; i++) {
        Blade *b = &g->blade[i];

        /* Taller blades are bent further by the same air, and the ones at the
         * back of the patch are sheltered by the ones in front. */
        double exposure = (0.55 + 0.45 * b->depth) * (0.6 + 0.4 * b->len / (float)g->height);
        double target = cfg->wind * exposure *
                        wind_at(g, b->x, phase, g->t + b->phase);

        double floor_bend = g->win_count ? windows_on_blade(g, b, &target) : 0.0;

        double k = BEND_STIFFNESS * b->stiff;
        double c = BEND_DAMPING * sqrt(b->stiff);
        for (int s = 0; s < steps; s++) {
            b->vel += (float)((k * (target - b->bend) - c * b->vel) * sdt);
            b->bend += (float)(b->vel * sdt);
        }

        /* A blade laid past flat would fold through its own base. */
        if (b->bend >  2.5f) { b->bend =  2.5f; if (b->vel > 0) b->vel = 0; }
        if (b->bend < -2.5f) { b->bend = -2.5f; if (b->vel < 0) b->vel = 0; }

        /* Held down by a window: the spring may bend it further, never less.
         * Applied to whichever side it is already going, so a blade pinned by a
         * descending window keeps the lean the wind gave it instead of snapping
         * to one side as the window arrives.
         *
         * Rate-limited, and that is the whole point of BEND_PRESS_RATE: the
         * constraint is a position, and writing a position straight into the
         * blade TELEPORTS it. A window dragged sideways brings blade after blade
         * into its shadow, each of which was standing up in the previous frame,
         * so the whole leading edge of the sweep popped flat in one frame and
         * only the spring-back looked animated. Now the constraint pushes at a
         * speed instead — fast enough that a window still visibly presses the
         * grass down, slow enough that the press is a movement and not a cut. */
        if (floor_bend > 0.0 && fabsf(b->bend) < floor_bend) {
            float side = b->bend < 0.0f ? -1.0f : 1.0f;
            float want = side * (float)floor_bend;
            float step = (float)(BEND_PRESS_RATE * dt);
            if (fabsf(want - b->bend) <= step) b->bend = want;
            else                               b->bend += want > b->bend ? step : -step;
            /* The window is what is holding it, not the spring — leaving the
             * velocity alone would have it fighting the constraint and buzzing
             * against the underside of the window. */
            if (b->vel * side < 0.0f) b->vel = 0.0f;
            moving = true;
        }

        if (fabsf(b->vel) > 0.002f || fabsf(b->bend) > 0.002f) moving = true;

        /* How far this blade's tip has travelled since the strip was last
         * painted, in px. The repaint below is skipped while the largest of
         * these is under half a pixel — in the trough of a lull that is most
         * frames, and a repaint nobody can distinguish from the last one is the
         * most expensive thing this module does. */
        float shift = fabsf(b->bend - b->drawn_bend) * b->len;
        if (shift > worst_shift) worst_shift = shift;
    }
    /* Wind that is on never stops, and the test above can read false for a
     * frame at the bottom of a lull — which would drop the frame loop to the
     * heartbeat and stutter the gust coming back. The per-blade test is for the
     * other case: wind turned OFF, and the patch still standing back up. */
    if (cfg->wind > 0.0) moving = true;
    g->moving = moving;

    /* One buffer for the whole strip, so a repaint is a strip-sized upload:
     * paced at [grass] fps rather than run at the frame rate. Grass is slow
     * enough that nobody can tell, and the tick above still runs every frame so
     * the motion itself keeps its timing. */
    bool recolor = g->drawn_opacity != cfg->opacity ||
                   memcmp(g->drawn_color, cfg->color, sizeof(g->drawn_color)) != 0;

    g->since_draw += dt;
    double period = 1.0 / cfg->fps;
    if (recolor || (worst_shift >= 0.5f && g->since_draw >= period)) {
        g->since_draw = 0.0;
        cairo_overlay_update(g->buf, grass_draw, g);
    }
    return moving;
}

bool grass_moving(const FwmGrass *g) { return g && g->moving; }

void grass_set_paused(FwmGrass *g, bool paused) {
    if (!g || g->paused == paused) return;
    g->paused = paused;
    if (paused) g->moving = false;
}

void grass_raise(FwmGrass *g) {
    if (!g || !g->buf) return;
    wlr_scene_node_raise_to_top(&g->buf->node);
}

bool grass_stale(const FwmGrass *g, const GrassConfig *cfg) {
    if (!g || !cfg) return false;
    /* Only what the blades were CUT from. Colour, opacity and the wind knobs
     * are read afresh on every tick, and regrowing the patch for one of those
     * would reshuffle every blade — a wind knob nudged while watching would
     * shuffle the lawn under the very thing being judged. */
    return g->cfg.enabled != cfg->enabled ||
           g->cfg.height  != cfg->height  ||
           g->cfg.density != cfg->density ||
           g->cfg.width   != cfg->width;
}

void grass_destroy(FwmGrass *g) {
    if (!g) return;
    if (g->buf) cairo_overlay_destroy(g->buf);
    free(g->blade);
    free(g);
}
