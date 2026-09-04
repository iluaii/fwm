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

#include "wpgen.h"
#include "sun.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── the seed ────────────────────────────────────────────────────────── */

static uint64_t seed_now;
static int      seed_drawn;

static uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* splitmix64 over a running state: one call, one independent number. */
static uint64_t next(uint64_t *s) {
    return mix64(*s += 0x9E3779B97F4A7C15ULL);
}

static double rnd01(uint64_t *s) {
    return (double)(next(s) >> 11) * 0x1.0p-53;
}

static double rndr(uint64_t *s, double lo, double hi) {
    return lo + rnd01(s) * (hi - lo);
}

/* A jitter of +/- `d` around `v`. */
static double jit(uint64_t *s, double v, double d) {
    return v + rndr(s, -d, d);
}

static void draw_seed(void) {
    /* The clock and the pid, mixed. Not cryptography — it decides what a
     * mountain looks like — and going through getrandom() would buy nothing
     * except a header that is not the same one on every libc. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t s = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    seed_now = mix64(s ^ (mix64((uint64_t)getpid()) << 1));
    seed_drawn = 1;
}

uint64_t wpgen_seed(void) {
    if (!seed_drawn) draw_seed();
    return seed_now;
}

void wpgen_reseed(void) {
    draw_seed();
}

/* ── noise ───────────────────────────────────────────────────────────── */

static double hash1(int64_t x, uint64_t seed) {
    return (double)(mix64((uint64_t)x * 0x2545F4914F6CDD1DULL ^ seed) >> 11) * 0x1.0p-53;
}

static double hash2(int64_t x, int64_t y, uint64_t seed) {
    uint64_t h = (uint64_t)x * 0x2545F4914F6CDD1DULL
               ^ (uint64_t)y * 0x9E3779B97F4A7C15ULL ^ seed;
    return (double)(mix64(h) >> 11) * 0x1.0p-53;
}

static double smooth(double t) { return t * t * (3.0 - 2.0 * t); }

static double vnoise1(double x, uint64_t seed) {
    double fx = floor(x);
    double t  = smooth(x - fx);
    double a  = hash1((int64_t)fx,     seed);
    double b  = hash1((int64_t)fx + 1, seed);
    return a + (b - a) * t;
}

static double vnoise2(double x, double y, uint64_t seed) {
    double fx = floor(x), fy = floor(y);
    double tx = smooth(x - fx), ty = smooth(y - fy);
    int64_t ix = (int64_t)fx, iy = (int64_t)fy;
    double a = hash2(ix,     iy,     seed);
    double b = hash2(ix + 1, iy,     seed);
    double c = hash2(ix,     iy + 1, seed);
    double d = hash2(ix + 1, iy + 1, seed);
    double ab = a + (b - a) * tx;
    double cd = c + (d - c) * tx;
    return ab + (cd - ab) * ty;
}

/* Octaves halving in weight and doubling in rate, normalised back to 0..1 so
 * an amplitude in the world struct means the same thing at any octave count. */
static double fbm1(double x, uint64_t seed, int oct) {
    double v = 0.0, amp = 0.5, f = 1.0, norm = 0.0;
    for (int i = 0; i < oct; i++) {
        v    += amp * vnoise1(x * f, seed + (uint64_t)i * 0x9E37ULL);
        norm += amp;
        amp  *= 0.5;
        f    *= 2.0;
    }
    return norm > 0.0 ? v / norm : 0.0;
}

static double fbm2(double x, double y, uint64_t seed, int oct) {
    double v = 0.0, amp = 0.5, f = 1.0, norm = 0.0;
    for (int i = 0; i < oct; i++) {
        v    += amp * vnoise2(x * f, y * f, seed + (uint64_t)i * 0x9E37ULL);
        norm += amp;
        amp  *= 0.5;
        f    *= 2.0;
    }
    return norm > 0.0 ? v / norm : 0.0;
}

/* ── colour ──────────────────────────────────────────────────────────── */

static void hsv2rgb(const double hsv[3], double rgb[3]) {
    double h = fmod(fmod(hsv[0], 360.0) + 360.0, 360.0) / 60.0;
    double s = hsv[1] < 0.0 ? 0.0 : (hsv[1] > 1.0 ? 1.0 : hsv[1]);
    double v = hsv[2] < 0.0 ? 0.0 : (hsv[2] > 1.0 ? 1.0 : hsv[2]);
    double c = v * s;
    double x = c * (1.0 - fabs(fmod(h, 2.0) - 1.0));
    double m = v - c;
    double r = 0, g = 0, b = 0;
    switch ((int)h) {
    case 0:  r = c; g = x; break;
    case 1:  r = x; g = c; break;
    case 2:  g = c; b = x; break;
    case 3:  g = x; b = c; break;
    case 4:  r = x; b = c; break;
    default: r = c; b = x; break;
    }
    rgb[0] = r + m; rgb[1] = g + m; rgb[2] = b + m;
}

static void mix3(const double a[3], const double b[3], double t, double out[3]) {
    for (int i = 0; i < 3; i++) out[i] = a[i] + (b[i] - a[i]) * t;
}

static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

/* ── the ramps ───────────────────────────────────────────────────────────
 *
 * The seed picks one of these and then moves it a little. It does NOT invent a
 * palette from scratch: nothing here can be pinned or saved, so the only
 * acceptable failure is a dull wallpaper and never an ugly one, and a random
 * walk through HSV produces both. Eight moods, each authored dark — this is
 * what windows are going to sit on, and a bright desktop makes every border
 * and panel fight for contrast.
 *
 * `glow` is the light in the sky, and the one colour here allowed to be
 * vivid: it is what theme.c lifts the accent from. */
struct Ramp {
    const char *name;
    double sky_top[3], sky_hor[3], ridge[3], haze[3], glow[3];
};

static const struct Ramp ramps[] = {
    /*                     sky top            sky horizon        ridge              haze               glow              */
    { "midnight",  { 226, 0.62, 0.13 }, { 220, 0.54, 0.36 }, { 228, 0.56, 0.13 }, { 216, 0.42, 0.38 }, { 214, 0.58, 0.98 } },
    { "dusk",      { 338, 0.52, 0.14 }, { 348, 0.46, 0.45 }, { 330, 0.48, 0.14 }, { 352, 0.34, 0.40 }, {  20, 0.68, 1.00 } },
    { "teal",      { 190, 0.68, 0.11 }, { 184, 0.62, 0.32 }, { 192, 0.60, 0.13 }, { 180, 0.46, 0.34 }, { 166, 0.62, 0.94 } },
    { "ember",     {  12, 0.44, 0.11 }, {   8, 0.60, 0.40 }, {  16, 0.44, 0.12 }, {   4, 0.44, 0.34 }, {  32, 0.76, 1.00 } },
    { "violet",    { 272, 0.62, 0.13 }, { 282, 0.54, 0.40 }, { 266, 0.54, 0.14 }, { 288, 0.44, 0.38 }, { 292, 0.56, 1.00 } },
    { "steel",     { 212, 0.36, 0.12 }, { 206, 0.26, 0.46 }, { 214, 0.30, 0.14 }, { 202, 0.22, 0.40 }, { 200, 0.32, 1.00 } },
    { "moss",      { 146, 0.44, 0.10 }, { 138, 0.40, 0.32 }, { 150, 0.44, 0.12 }, { 132, 0.32, 0.33 }, {  94, 0.56, 0.90 } },
    { "amber",     {  44, 0.40, 0.10 }, {  40, 0.54, 0.36 }, {  38, 0.34, 0.12 }, {  46, 0.42, 0.32 }, {  42, 0.74, 1.00 } },
};

#define RAMP_COUNT ((int)(sizeof(ramps) / sizeof(ramps[0])))

/* Move a ramp entry off its authored value by a little. Hue wanders furthest,
 * because that is the axis a whole picture reads as "a different evening";
 * value barely moves at all, because that is the axis that would make one
 * wallpaper too bright to put a window on. */
static void jitter(uint64_t *s, const double in[3], double h, double sat,
                   double val, double out[3]) {
    out[0] = jit(s, in[0], h);
    out[1] = clamp01(jit(s, in[1], sat));
    out[2] = clamp01(jit(s, in[2], val));
}

/* ── the world ───────────────────────────────────────────────────────── */

/* Pick from a table by seed. */
static int pick(uint64_t *s, int n) {
    return (int)(next(s) % (uint64_t)n);
}

/* True with probability `p`. */
static int chance(uint64_t *s, double p) {
    return rnd01(s) < p;
}

void wpgen_world(const FwmConfig *cfg, WpgenWorld *out) {
    memset(out, 0, sizeof(*out));
    out->seed  = wpgen_seed();
    out->water = -1;

    uint64_t s = out->seed;

    /* ── colour ── */
    const struct Ramp *r = &ramps[pick(&s, RAMP_COUNT)];
    jitter(&s, r->sky_top, 9.0, 0.06, 0.02, out->sky_top);
    jitter(&s, r->sky_hor, 9.0, 0.07, 0.04, out->sky_hor);
    jitter(&s, r->ridge,   9.0, 0.06, 0.02, out->ridge);
    jitter(&s, r->haze,    9.0, 0.06, 0.04, out->haze);
    jitter(&s, r->glow,   14.0, 0.07, 0.02, out->glow);

    /* ── the shape of the place ──
     *
     * Three world-level numbers the per-layer ones are then derived from, so
     * that a seed produces a landscape with a CHARACTER rather than four
     * ranges that each rolled their own dice and disagree. */

    /* Two ranges is a stark cut-out; four is depth and mist. Three most often,
     * because it is the one that reads as a landscape without either the
     * emptiness of two or the business of four. */
    static const int counts[] = { 2, 3, 3, 3, 4, 4 };
    out->count = counts[pick(&s, (int)(sizeof(counts) / sizeof(counts[0])))];

    /* How much of the frame is sky. Low horizon: a big sky with a strip of
     * land, which is where the light and the cloud get to do the work. High:
     * the ranges fill the screen and the sky is a band above them. */
    double h0 = rndr(&s, 0.28, 0.60);
    /* Where the NEAREST range sits. Tied to the count, because it is the
     * count that decides how much room each range gets: two ranges have to
     * share the frame, so the near one sits high and takes half of it, while
     * four are a stack and the last of them is a band along the bottom. Fixed
     * high, the two-layer worlds came out as one enormous flat wall with a
     * sliver of foreground under it. */
    double hN = rndr(&s, 0.55 + 0.11 * (out->count - 2),
                         0.76 + 0.09 * (out->count - 2));
    if (hN < h0 + 0.16) hN = h0 + 0.16;

    /* 0 dunes, 1 alps. One number for the whole world: a seed is a PLACE, and
     * a place does not have jagged peaks behind rolling dunes behind jagged
     * peaks. */
    double relief = rndr(&s, 0.05, 1.0);
    /* How tall the land stands, over and above where the ranges sit. */
    double amp_scale = rndr(&s, 0.55, 1.45);
    /* Detail per screen. High is a long serrated range read from far off, low
     * is a few big landforms. Floored well clear of 1.5 further down, because
     * below that one screen sits inside a single cell of the base octave and
     * the range draws a straight line. */
    double detail = rndr(&s, 2.8, 5.2);
    /* Whether the far distance is buried in mist or cut out sharply. */
    out->haze_depth = rndr(&s, 0.04, 0.52);
    /* How far the nearest layer travels. Everything else is spaced under it. */
    double slack_max = rndr(&s, 0.55, 1.15);

    /* The nearest range is sometimes water instead: flat, with the sky broken
     * up on it. Only ever the nearest — it is the one with a whole edge along
     * the bottom of the frame for a shore to be.
     *
     * Water needs an expanse or it is a puddle at the bottom of the picture,
     * so its shoreline comes up the frame whatever the count wanted. */
    if (chance(&s, 0.30)) {
        out->water = out->count - 1;
        if (hN > 0.74) hN = rndr(&s, 0.58, 0.74);
        if (hN < h0 + 0.14) hN = h0 + 0.14;
    }

    /* Spacing: bunched up toward the horizon, spread evenly down the frame, or
     * crowded into the foreground. One draw for the world, not one per layer —
     * the ranges of a place are spaced the way that place is. */
    static const double bias_pick[] = { 0.7, 1.0, 1.0, 1.4 };
    double bias = bias_pick[pick(&s, 4)];

    for (int i = 0; i < out->count; i++) {
        /* 0 at the far range, 1 at the near one. */
        double d = out->count > 1 ? (double)i / (out->count - 1) : 0.0;

        out->horizon[i] = h0 + (hN - h0) * pow(d, bias);
        out->horizon[i] = jit(&s, out->horizon[i], 0.02);

        /* Travel grows toward the viewer — that IS the parallax. Geometric, so
         * the far range barely moves however many there are. */
        double t = out->count > 1 ? d : 1.0;
        out->slack[i] = 0.07 * pow(slack_max / 0.07, t) * jit(&s, 1.0, 0.12);

        /* Height falls off toward the front: a near hill that stood as tall as
         * a distant mountain would read as the mountain being small. */
        out->amp[i] = amp_scale * (0.23 - 0.11 * d) * jit(&s, 1.0, 0.22);

        /* Far ranges carry more detail per screen, because they are farther:
         * the same rock subtends less. Never below the floor: a layer under it
         * has one lattice cell to a screen and comes out ruler-straight, which
         * is the failure that looks least like a landscape and most like a
         * bug. */
        out->freq[i] = detail * (1.0 - 0.34 * d) * jit(&s, 1.0, 0.2);
        if (out->freq[i] < 1.8) out->freq[i] = 1.8;

        /* The world's character, softening toward the front where the land is
         * closer to the eye and reads as ground rather than skyline. */
        out->rough[i] = clamp01(relief * (1.0 - 0.45 * d) + rndr(&s, -0.1, 0.1));

        /* Somewhere along an endless range, so two seeds that drew the same
         * shape parameters still do not draw the same mountain. */
        out->phase[i] = rndr(&s, 0.0, 512.0);

        /* A near range may rise in FRONT of a far one — that is what being
         * nearer looks like — but its crest must not stand above the far
         * skyline, or the depth reads backwards. So its top is allowed up to
         * halfway into the range behind it and no further. */
        if (i > 0) {
            double room = out->horizon[i] - out->horizon[i - 1]
                        + out->amp[i - 1] * 0.5;
            if (out->amp[i] > room) out->amp[i] = room;
        }
        if (out->amp[i] < 0.03) out->amp[i] = 0.03;
    }

    /* Water is flat by definition, and travels least of anything: a plane has
     * no parallax to give. */
    if (out->water >= 0) {
        out->amp[out->water]   = 0.0;
        out->rough[out->water] = 0.0;
        out->slack[out->water] *= 0.45;
    }

    /* ── the sky ── */
    out->cloud   = rndr(&s, 0.0, 1.0);
    /* Squared, so a clear sky is a real outcome and not just a faint one. */
    out->cloud  *= out->cloud;
    out->cloud_w = rndr(&s, 1.4, 3.6);
    out->cloud_h = rndr(&s, 4.0, 11.0);

    /* ── the light ──
     *
     * One sun over the desktop: the glow belongs on the side the windows are
     * lit from, or the wallpaper is the one thing on screen disagreeing about
     * where the light is. So WHERE it is and whether it is night are the
     * compositor's; how wide, how strong and whether the disc itself is up are
     * the seed's. */
    if (cfg && cfg->sun.enabled) {
        /* Asked of sun.c rather than read off the config, because in clock
         * mode the configured angles are only where the sun starts: the live
         * ones are worked out from the hour, and at the moment a wallpaper is
         * built nothing has computed them yet. This is what makes starting fwm
         * in the evening get an evening. */
        FwmSunLight light;
        sun_light(&cfg->sun, sun_hour_local(time(NULL)), &light);

        /* [sun] azimuth is degrees clockwise from the top of the screen, so
         * its sine is how far across the screen the light stands — pulled off
         * the very edge, where a glow reads as a gradient someone forgot to
         * finish rather than as a sun. */
        out->sun_x = 0.5 + 0.42 * sin(light.azimuth * M_PI / 180.0);
        /* Below the horizon is night: the sky drains and the glow goes with
         * it, but never entirely — a landscape with no light in it anywhere is
         * a grey rectangle. */
        double el = light.elevation;
        out->night = el >= 25.0 ? 0.0 : (el <= -5.0 ? 1.0 : (25.0 - el) / 30.0);
    } else {
        out->sun_x = rndr(&s, 0.12, 0.88);
        out->night = rndr(&s, 0.0, 0.45);
    }

    out->sun_power = rndr(&s, 0.34, 0.72) * (1.0 - 0.68 * out->night);
    out->glow_w    = rndr(&s, 0.16, 0.62);
    out->glow_h    = rndr(&s, 0.10, 0.34);
    /* A disc in the sky. Not always: an overcast sky with the light only
     * implied is half the moods here.
     *
     * Where it sits has to be measured from the CRESTS and not from the
     * horizon line, which is where the first version put it and why it was
     * never once visible: the horizon is the height the range's VALLEYS sit
     * at, and the range itself is entirely above it, so a disc centred there
     * is a disc behind a mountain. Measured from the crest it can be cut by
     * the skyline (which is the good one) or stand clear above it. */
    if (chance(&s, 0.38)) {
        /* Half the badge's width, in screen heights. Bigger than the plain
         * disc this replaced had to be: the mark has a monogram inside it, and
         * below about a tenth of the screen its strokes go to thread. */
        out->disc = rndr(&s, 0.055, 0.125);
        out->disc_y = out->horizon[0] - out->amp[0] * rndr(&s, 0.45, 1.05)
                    - out->disc * rndr(&s, -0.2, 1.1);
        if (out->disc_y < out->disc * 0.9) out->disc_y = out->disc * 0.9;
    }
    /* Stars, and only when the sun says it is actually dark enough for them —
     * a starfield at noon would be the wallpaper contradicting the shadows. */
    out->stars = out->night > 0.45 ? rndr(&s, 0.3, 1.0) * out->night : 0.0;
}

/* About a 1920x1440 screen's worth. Chosen as an area and not a height because
 * an ultrawide is as expensive as a 4K and half as tall: capping the height
 * alone left the one shape of monitor the cap was for paying full price. */
#define WPGEN_PIXEL_BUDGET (1920.0 * 1440.0)

double wpgen_render_scale(int screen_w, int screen_h) {
    if (screen_w <= 0 || screen_h <= 0) return 1.0;
    double px = (double)screen_w * screen_h;
    if (px <= WPGEN_PIXEL_BUDGET) return 1.0;
    double k = sqrt(WPGEN_PIXEL_BUDGET / px);
    return k < 0.5 ? 0.5 : k;
}

int wpgen_layer_width(const WpgenWorld *w, int i, int screen_w) {
    if (!w || i < 0 || i >= w->count || screen_w <= 0) return screen_w;
    int width = (int)lround(screen_w * (1.0 + w->slack[i]));
    return width < screen_w ? screen_w : width;
}

void wpgen_palette(const WpgenWorld *w, double cast[2], double accent[3]) {
    if (!w) return;
    /* The cast is the horizon's: the largest coloured area in the picture, and
     * the one the eye calls the wallpaper's colour. The sky above it is the
     * same hue and the ridges are nearly black, so nothing is gained by
     * averaging them in. */
    cast[0] = w->sky_hor[0];
    cast[1] = w->sky_hor[1];
    accent[0] = w->glow[0];
    accent[1] = w->glow[1];
    accent[2] = w->glow[2];
}

/* ── drawing ─────────────────────────────────────────────────────────── */

/* Five octaves of anything cluster around their own mean — a sum of uniform
 * draws is a bell, not a range — so a silhouette taken straight off an fbm
 * uses about a third of the amplitude it was given and comes out as a gentle
 * rumple across the whole screen, which is what the first version of this drew.
 * Stretched about the mean by roughly its own spread, so `amp` in the world
 * struct means the height it says. */
static double spread(double f) {
    /* tanh and not a clamp: clipping the tail flattens every valley the noise
     * pushed past the end onto one exact height, and a range with a ruler-
     * straight kilometre in it is the one thing that says "generated" out
     * loud. This saturates without ever arriving. */
    return 0.5 + 0.5 * tanh((f - 0.5) * 3.4);
}

/* The silhouette of range `i` at world position `u`, 0 (on the horizon line)
 * to 1 (a full amplitude above it). */
static double ridge_shape(const WpgenWorld *w, int i, double u) {
    double x = u * w->freq[i] + w->phase[i];
    double f = spread(fbm1(x, w->seed + (uint64_t)(i + 1) * 0x1F123BB5ULL, 5));
    /* Folded, it has peaks and valleys instead of dunes: `rough` is how much
     * of a mountain this range is rather than a hill. */
    double ridged = 1.0 - fabs(2.0 * f - 1.0);
    return (1.0 - w->rough[i]) * f + w->rough[i] * ridged;
}

/* A star field that costs the same as not having one.
 *
 * The obvious way — a list of stars, checked per pixel — is pixels times stars
 * and unaffordable at this size. So the sky is a lattice of cells with at most
 * one star in each: a pixel only ever asks about the cell it is in and its
 * eight neighbours, and the cell's hash decides where its star sits, how
 * bright it is, and whether it is there at all. Fixed work, and the field is
 * as reproducible as everything else here. */
#define STAR_CELLS_X 46.0

static double star_at(const WpgenWorld *w, double u, double v, double aspect) {
    double sx = u * STAR_CELLS_X;
    double sy = v * STAR_CELLS_X / (aspect > 0.0 ? aspect : 1.0);
    int cx = (int)floor(sx), cy = (int)floor(sy);
    double acc = 0.0;

    for (int oy = -1; oy <= 1; oy++) {
        for (int ox = -1; ox <= 1; ox++) {
            int gx = cx + ox, gy = cy + oy;
            double h = hash2(gx, gy, w->seed ^ 0x57A45ULL);
            /* Most cells are empty sky. */
            if (h > 0.16) continue;
            double px = gx + hash2(gx, gy, w->seed ^ 0xA1ULL);
            double py = gy + hash2(gx, gy, w->seed ^ 0xB2ULL);
            double mag = 0.35 + 0.65 * hash2(gx, gy, w->seed ^ 0xC3ULL);
            double dx = sx - px, dy = sy - py;
            double d2 = dx * dx + dy * dy;
            /* A tight core with a little bloom, which is what stops a star
             * being one hard pixel that crawls when the layer pans. */
            acc += mag * exp(-d2 * 90.0) + 0.22 * mag * exp(-d2 * 12.0);
        }
    }
    return acc;
}

/* ── the badge ────────────────────────────────────────────────────────
 *
 * fwm's own mark, stroked into an alpha mask once per layer that wants one and
 * then sampled per pixel like a sprite.
 *
 * A mask and not a draw, because of WHERE it has to land: in the sky, behind
 * the ranges, in the middle of a pass that is writing raw pixels. Handing the
 * surface to cairo afterwards would put the badge on top of the mountains it
 * is supposed to be standing behind. A mask costs one bounds test and one byte
 * read in the loop and sorts itself into the picture correctly.
 *
 * The path is assets/logo-brackets.svg — the one the README opens with —
 * transcribed by hand: it is six polylines, and parsing SVG to find that out
 * would mean a dependency on librsvg for a shape that fits in this comment's
 * worth of code. Keep the two in step if the logo ever changes.
 *
 * Nothing is filled, because nothing in it is closed: this mark is a monogram
 * between two open chevrons, and it is the halo behind it that gives it a body
 * to be a light rather than an outline. */
static void badge_path(cairo_t *cr) {
    cairo_move_to(cr, -14.0, 16.0); cairo_line_to(cr, -40.0, 53.0);
    cairo_line_to(cr, -14.0, 90.0);
    cairo_stroke(cr);

    cairo_move_to(cr, 6.0, 96.0);  cairo_line_to(cr, 6.0, 10.0);
    cairo_line_to(cr, 76.0, 10.0);
    cairo_stroke(cr);

    cairo_move_to(cr, 6.0, 46.0);  cairo_line_to(cr, 34.0, 46.0);
    cairo_stroke(cr);

    cairo_move_to(cr, 34.0, 46.0); cairo_line_to(cr, 46.0, 96.0);
    cairo_line_to(cr, 54.0, 62.0); cairo_line_to(cr, 64.0, 96.0);
    cairo_line_to(cr, 76.0, 10.0);
    cairo_stroke(cr);

    cairo_move_to(cr, 76.0, 10.0); cairo_line_to(cr, 90.0, 54.0);
    cairo_line_to(cr, 104.0, 14.0); cairo_line_to(cr, 104.0, 96.0);
    cairo_stroke(cr);

    cairo_move_to(cr, 124.0, 16.0); cairo_line_to(cr, 150.0, 53.0);
    cairo_line_to(cr, 124.0, 90.0);
    cairo_stroke(cr);
}

/* How far the halo reaches out from the badge, in badge half-widths. Its
 * falloff lands on exactly zero here, so this is both the size of the glow and
 * the box it needs to be tested in — one number, and no edge to see. */
#define WPGEN_HALO 2.6

/* The logo's viewBox, and the width of what is actually drawn inside it. The
 * ink is what a caller means by "this big"; the box is what has to be
 * allocated, padding and all. Their centres coincide, which is what lets the
 * badge be placed by its middle without any of this leaking out. */
#define BADGE_BOX_W 208.0
#define BADGE_BOX_H 102.0
#define BADGE_INK_W 190.0

static cairo_surface_t *badge_mask(int mw, int mh) {
    if (mw < 12 || mh < 6) return NULL;
    cairo_surface_t *m = cairo_image_surface_create(CAIRO_FORMAT_A8, mw, mh);
    if (cairo_surface_status(m) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(m);
        return NULL;
    }
    cairo_t *cr = cairo_create(m);
    cairo_scale(cr, mw / BADGE_BOX_W, mh / BADGE_BOX_H);
    cairo_translate(cr, 48.0, -2.0);   /* the viewBox's own origin */
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_set_line_width(cr, 7.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    badge_path(cr);

    cairo_destroy(cr);
    cairo_surface_flush(m);
    return m;
}

/* How dark the ground is `down` of the way from its crest to the bottom edge.
 *
 * A range with nothing in front of it fills most of the screen, and at the
 * flat 12% this used to be that is a painted wall rather than ground going
 * away from you. Most of the fall happens in the first third, the way real
 * ground does: the light it catches drops off fast just under the skyline and
 * then hardly at all. */
static double ground_k(double down) {
    return 1.0 - 0.30 * (1.0 - exp(-down * 2.6)) / (1.0 - exp(-2.6));
}

/* Enough of the sky's texture to break a flat gradient, computed on a coarse
 * grid and read back bilinearly. Cloud is low-frequency by nature, so the
 * upsample costs nothing visible and turns a per-pixel fbm — five million
 * noise lookups on a 4K backdrop — into thirty thousand. */
#define CLOUD_STEP 8

struct Cloud {
    float *v;
    int w, h;
};

static void cloud_build(struct Cloud *c, const WpgenWorld *w,
                        int buf_w, int buf_h, int screen_w) {
    c->w = buf_w / CLOUD_STEP + 2;
    c->h = buf_h / CLOUD_STEP + 2;
    c->v = calloc((size_t)c->w * c->h, sizeof(float));
    if (!c->v) { c->w = c->h = 0; return; }
    for (int gy = 0; gy < c->h; gy++) {
        double v = (double)(gy * CLOUD_STEP) / buf_h;
        for (int gx = 0; gx < c->w; gx++) {
            double u = (double)(gx * CLOUD_STEP) / screen_w;
            /* Stretched flat: cloud on a real sky is far wider than it is
             * tall, and an unstretched fbm reads as gravel. */
            c->v[(size_t)gy * c->w + gx] =
                (float)fbm2(u * w->cloud_w, v * w->cloud_h, w->seed ^ 0xC10DULL, 4);
        }
    }
}

static double cloud_at(const struct Cloud *c, int x, int y) {
    if (!c->v) return 0.5;
    double gx = (double)x / CLOUD_STEP, gy = (double)y / CLOUD_STEP;
    int ix = (int)gx, iy = (int)gy;
    if (ix < 0) ix = 0;
    if (ix > c->w - 2) ix = c->w - 2;
    if (iy < 0) iy = 0;
    if (iy > c->h - 2) iy = c->h - 2;
    double tx = gx - ix, ty = gy - iy;
    const float *row0 = c->v + (size_t)iy * c->w + ix;
    const float *row1 = row0 + c->w;
    double a = row0[0] + (row0[1] - row0[0]) * tx;
    double b = row1[0] + (row1[1] - row1[0]) * tx;
    return a + (b - a) * ty;
}

cairo_surface_t *wpgen_layer(const WpgenWorld *w, int i,
                             int buf_w, int buf_h, int screen_w) {
    if (!w || i < 0 || i >= w->count || buf_w <= 0 || buf_h <= 0 || screen_w <= 0)
        return NULL;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, buf_w, buf_h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    unsigned char *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);

    double *ridge_y = calloc((size_t)buf_w, sizeof(double));
    if (!ridge_y) { cairo_surface_destroy(surf); return NULL; }

    /* One evaluation per COLUMN, not per pixel: a range is a height field, so
     * everything below the line is the same answer repeated. */
    int water = (i == w->water);
    for (int x = 0; x < buf_w; x++) {
        double u = (double)x / screen_w;
        /* Water has no relief — that is what makes it read as water — so its
         * line is the horizon and nothing else. */
        double h = water ? 0.0 : ridge_shape(w, i, u);
        ridge_y[x] = (w->horizon[i] - w->amp[i] * h) * buf_h;
    }

    double sky_top[3], sky_hor[3], ridge_rgb[3], haze_rgb[3], glow_rgb[3];
    hsv2rgb(w->sky_top, sky_top);
    hsv2rgb(w->sky_hor, sky_hor);
    hsv2rgb(w->ridge,   ridge_rgb);
    hsv2rgb(w->haze,    haze_rgb);
    hsv2rgb(w->glow,    glow_rgb);

    /* How far back this range stands: the first is nearly all haze, the last
     * nearly all rock. Aerial perspective, and the only thing that makes three
     * silhouettes in one colour read as distance. */
    double depth = w->count > 1 ? (double)i / (w->count - 1) : 1.0;
    double fill[3];
    /* haze_depth is how much rock the FARTHEST range keeps: low buries it in
     * mist, high cuts it out sharply against the sky. */
    double hz = w->haze_depth;
    mix3(haze_rgb, ridge_rgb, hz + (1.0 - hz) * depth, fill);

    int sky = (i == 0);
    struct Cloud cloud = { 0 };
    if (sky) cloud_build(&cloud, w, buf_w, buf_h, screen_w);

    double flat_horizon = w->horizon[0] * buf_h;
    if (flat_horizon < 1.0) flat_horizon = 1.0;

    /* The glow, separated. exp(-(dx*dx + dy*dy)) is exp(-dx*dx) * exp(-dy*dy),
     * and dx depends only on the column — so the whole horizontal half of a
     * two-million-pixel gaussian is this one array, and the inner loop is left
     * with a multiply instead of a call into libm. */
    double *glow_x = NULL, *wobble = NULL;
    if (sky || water) {
        glow_x = calloc((size_t)buf_w, sizeof(double));
        if (glow_x) {
            /* Water gets a narrower column than the sky gets a glow: a
             * reflection is the light's own path across the surface toward
             * the viewer, not the whole scattered sky. */
            double gw = w->glow_w > 0.02 ? w->glow_w : 0.02;
            if (water) gw *= 0.55;
            for (int x = 0; x < buf_w; x++) {
                double dx = ((double)x / screen_w - w->sun_x) / gw;
                glow_x[x] = exp(-dx * dx);
            }
        }
    }
    if (water) {
        /* One phase per column, so the ripple bands are not straight rules
         * across the whole screen. Per column and not per pixel: an fbm in the
         * inner loop would cost more than the rest of the layer. */
        wobble = calloc((size_t)buf_w, sizeof(double));
        if (wobble)
            for (int x = 0; x < buf_w; x++)
                wobble[x] = fbm1((double)x / screen_w * 4.0,
                                 w->seed ^ 0x2A7E2ULL, 3) * 6.0;
    }

    /* Square units for anything that has to come out round on a wide screen. */
    double aspect = (double)screen_w / buf_h;
    double disc_rgb[3];
    mix3(glow_rgb, (double[3]){ 1.0, 1.0, 1.0 }, 0.45, disc_rgb);

    /* The badge. In the sky where the light is when this seed put a body
     * there; otherwise small in the top corner, because fwm's mark belongs on
     * fwm's wallpaper either way.
     *
     * The corner one is placed in the PICTURE and not on the screen, so it
     * travels with the sky as the desktops go by. That is deliberate: the sky
     * layer has the least slack of any of them, so the drift is slight, and a
     * mark pinned to the glass while the landscape slid underneath it would
     * read as a panel fwm had left switched on. */
    cairo_surface_t *badge = NULL;
    const unsigned char *badge_px = NULL;
    int badge_stride = 0, badge_x = 0, badge_y = 0, badge_w = 0, badge_h = 0;
    double badge_alpha = 1.0;
    double badge_rgb[3];
    memcpy(badge_rgb, disc_rgb, sizeof(badge_rgb));
    int badge_over_land = 0;   /* the corner mark sits on top of everything */

    if (sky) {
        if (w->disc > 0.0) {
            badge_w = (int)lround(w->disc * 2.0 * buf_h
                                  * (BADGE_BOX_W / BADGE_INK_W));
            badge_h = (int)lround(badge_w * (BADGE_BOX_H / BADGE_BOX_W));
            badge_x = (int)lround(w->sun_x * screen_w - badge_w / 2.0);
            badge_y = (int)lround(w->disc_y * buf_h - badge_h / 2.0);
        } else {
            /* A watermark: small, and only just brighter than the sky it is
             * on, so it is fwm signing the picture rather than fwm putting a
             * logo on your desktop. */
            badge_w = (int)lround(0.20 * buf_h
                                  * (BADGE_BOX_W / BADGE_INK_W));
            badge_h = (int)lround(badge_w * (BADGE_BOX_H / BADGE_BOX_W));
            int margin = (int)lround(0.055 * buf_h);
            badge_x = margin;
            badge_y = margin;
            badge_alpha = 0.17;
            badge_over_land = 1;
            /* Drawn in the light's own colour, lifted toward white just enough
             * to stay legible on a dark sky. */
            mix3(glow_rgb, (double[3]){ 1.0, 1.0, 1.0 }, 0.30, badge_rgb);
        }
        badge = badge_mask(badge_w, badge_h);
        if (badge) {
            badge_px = cairo_image_surface_get_data(badge);
            badge_stride = cairo_image_surface_get_stride(badge);
        }
        if (!badge_px) badge_w = badge_h = 0;
    }

    for (int y = 0; y < buf_h; y++) {
        uint32_t *row = (uint32_t *)(data + (size_t)y * stride);
        double v = ((double)y + 0.5) / buf_h;

        /* Everything the sky decides once per row: the gradient it has reached,
         * how much cloud belongs at this height, and the vertical half of the
         * glow. */
        double sky_c[3] = { 0, 0, 0 };
        double cloud_k = 0.0, glow_y = 0.0, star_k = 0.0;
        if (sky) {
            double t = clamp01((double)y / flat_horizon);
            /* Squared, so the horizon's warmth stays near the horizon instead
             * of washing halfway up the picture. */
            mix3(sky_top, sky_hor, t * t, sky_c);
            /* Cloud is strongest across the middle of the sky and gone by both
             * ends — banded against the zenith it looks like a gradient
             * artefact, and against the ridge like dirt. */
            cloud_k = 0.30 * w->cloud * sin(M_PI * t) * (1.0 - 0.5 * w->night);
            double gh = w->glow_h > 0.02 ? w->glow_h : 0.02;
            double dy = (v - w->horizon[0]) / gh;
            glow_y = exp(-dy * dy);
            /* Thinned toward the horizon, where the air is thickest and where
             * the glow would drown them anyway. */
            star_k = w->stars * (1.0 - 0.85 * clamp01(t));
        }
        /* The disc is a circle around one point: whole rows of the sky cannot
         * touch it, and this is how they find out for the price of one
         * comparison instead of a square root per pixel. */
        double disc_dy = v - w->disc_y;
        int disc_row = sky && w->disc > 0.0 && fabs(disc_dy) < w->disc * WPGEN_HALO;

        for (int x = 0; x < buf_w; x++) {
            double c[3];
            double a;

            /* 0 above the ridge line, 1 below it, over about a pixel — a
             * height field never needs more than a vertical ramp to look cut
             * rather than stepped. */
            double below = ((double)y + 0.5) - ridge_y[x];
            double cov = clamp01(below / 1.2 + 0.5);

            if (sky && cov >= 1.0) {
                /* Under the range: the sky here is about to be painted over
                 * completely, and on the backdrop that is half the screen.
                 * Working out a gradient, a cloud and a glow in order to throw
                 * all three away was most of what this layer cost. */
                double denom = buf_h - ridge_y[x];
                double down = denom > 1.0 ? clamp01(below / denom) : 0.0;
                for (int ch = 0; ch < 3; ch++) c[ch] = fill[ch] * ground_k(down);
                a = 1.0;
            } else if (sky) {
                double u = (double)x / screen_w;
                memcpy(c, sky_c, sizeof(c));

                /* Under the cloud, so a bank of it puts them out. */
                if (star_k > 0.0) {
                    double sv = star_at(w, u, v, aspect) * star_k;
                    for (int ch = 0; ch < 3; ch++) c[ch] = clamp01(c[ch] + sv);
                }

                if (cloud.v && cloud_k > 0.0) {
                    double n = cloud_at(&cloud, x, y) - 0.5;
                    for (int ch = 0; ch < 3; ch++) c[ch] = clamp01(c[ch] + n * cloud_k);
                }

                /* The sun's own light, sitting on the horizon where it stands.
                 * Wide and shallow: this is scattered sky, not a disc. */
                if (glow_x) {
                    double g = glow_x[x] * glow_y * w->sun_power;
                    for (int ch = 0; ch < 3; ch++)
                        c[ch] = clamp01(c[ch] + glow_rgb[ch] * g);
                }

                /* The sun itself, standing on the horizon line — which means
                 * the far range is drawn over its lower half a few lines down,
                 * and it rises out of the land instead of floating on it. */
                /* The air around the badge. A body with nothing glowing
                 * about it reads as a shape pasted on the sky rather than a
                 * light standing in it.
                 *
                 * The falloff has to REACH zero, and a gaussian never does: at
                 * the edge of the box it was being tested in it was still
                 * worth a level or two of blue, and cutting it there drew a
                 * faint square around the sun. This one lands exactly on zero
                 * at the radius the box is drawn from, so there is no edge to
                 * see. */
                if (disc_row) {
                    double dxh = (u - w->sun_x) * aspect;
                    if (fabs(dxh) < w->disc * WPGEN_HALO) {
                        double rr = sqrt(dxh * dxh + disc_dy * disc_dy);
                        double reach = w->disc * (WPGEN_HALO - 1.0);
                        double t2 = (rr - w->disc) / reach;
                        if (t2 < 1.0) {
                            if (t2 < 0.0) t2 = 0.0;
                            double fall = 1.0 - t2;
                            double halo = fall * fall * fall;
                            double hk = halo * 0.34 * (0.4 + 0.6 * w->sun_power);
                            for (int ch = 0; ch < 3; ch++)
                                c[ch] = clamp01(c[ch] + glow_rgb[ch] * hk);
                        }
                    }
                }

                /* And the badge itself, over the halo and under the ranges. */
                if (badge_px && !badge_over_land) {
                    int bx = x - badge_x, by = y - badge_y;
                    if (bx >= 0 && bx < badge_w && by >= 0 && by < badge_h) {
                        double ba = badge_px[(size_t)by * badge_stride + bx] / 255.0;
                        if (ba > 0.0) mix3(c, badge_rgb, ba * badge_alpha, c);
                    }
                }

                /* The sky is drawn all the way down and the first range over
                 * it: an opaque backdrop is what stops the desktop showing
                 * through the one layer with nothing behind it. */
                if (cov > 0.0) {
                    double rock[3];
                    /* Ground darkens toward the bottom of the frame — the part
                     * of a range nearest the viewer is the part in its own
                     * shadow. */
                    double denom = buf_h - ridge_y[x];
                    double down = denom > 1.0 ? clamp01(below / denom) : 0.0;
                    for (int ch = 0; ch < 3; ch++) rock[ch] = fill[ch] * ground_k(down);
                    mix3(c, rock, cov, c);
                }
                a = 1.0;
            } else if (water) {
                if (cov <= 0.0) {
                    row[x] = 0;
                    continue;
                }
                /* A plane the sky is broken up on. Near the line it is almost
                 * all sky and the bands are crowded; further down it is its
                 * own dark colour and the bands open out. That crowding is the
                 * whole cue that the surface is flat and receding, so it is
                 * sqrt of the distance and not the distance. */
                double d = below / buf_h;
                double fade = exp(-d * 7.0);
                mix3(fill, sky_hor, 0.28 * fade + 0.10, c);

                double band = 0.5 + 0.5 * sin(sqrt(d) * 44.0
                                              + (wobble ? wobble[x] : 0.0));
                if (glow_x) {
                    /* Held back from what the sky is doing. A reflection is
                     * the brighter thing physically, but this is a desktop:
                     * the bottom of the frame is where windows sit, and the
                     * brightest band on screen does not belong under them. */
                    double col = glow_x[x] * fade * w->sun_power * 0.62
                               * (0.5 + 0.5 * band);
                    for (int ch = 0; ch < 3; ch++)
                        c[ch] = clamp01(c[ch] + glow_rgb[ch] * col);
                }
                /* And a little of the same banding in the water itself, so the
                 * surface still reads as a surface away from the light. */
                double lift = 0.05 * fade * (band - 0.5);
                double denom = buf_h - ridge_y[x];
                double down = denom > 1.0 ? clamp01(below / denom) : 0.0;
                double gk = ground_k(down);
                for (int ch = 0; ch < 3; ch++)
                    c[ch] = clamp01((c[ch] + lift) * gk);
                a = cov;
            } else {
                if (cov <= 0.0) {
                    row[x] = 0;
                    continue;
                }
                double denom = buf_h - ridge_y[x];
                double down = denom > 1.0 ? clamp01(below / denom) : 0.0;
                for (int ch = 0; ch < 3; ch++) c[ch] = fill[ch] * ground_k(down);
                a = cov;
            }

            /* The corner mark, over the land as well as the sky: it is fwm
             * signing the picture, not an object standing in it. */
            if (badge_px && badge_over_land) {
                int bx = x - badge_x, by = y - badge_y;
                if (bx >= 0 && bx < badge_w && by >= 0 && by < badge_h) {
                    double ba = badge_px[(size_t)by * badge_stride + bx] / 255.0;
                    if (ba > 0.0) mix3(c, badge_rgb, ba * badge_alpha, c);
                }
            }

            /* A thread of light along the crest, on the slopes that face the
             * sun. Without it three flat silhouettes stack into one shape; with
             * it they separate exactly where they overlap. */
            if (!water && below >= 0.0 && below < 4.0) {
                double slope = 0.0;
                if (x > 0 && x < buf_w - 1)
                    slope = (ridge_y[x + 1] - ridge_y[x - 1]) * 0.5;
                double side = (w->sun_x > 0.5) ? 1.0 : -1.0;
                double face = clamp01(0.5 + 0.7 * slope * side);
                double rim = (1.0 - below / 4.0) * face * 0.45 * w->sun_power;
                for (int ch = 0; ch < 3; ch++)
                    c[ch] = clamp01(c[ch] + glow_rgb[ch] * rim);
            }

            /* Dither, and the reason this can be cairo at all.
             *
             * A sky is a gradient two hundred steps wide over a thousand
             * pixels, which in 8 bits is a stack of visible bands — the one
             * thing a shader would have given for free. Half a level of noise,
             * hashed off the pixel so it never crawls, breaks the step edge and
             * costs a multiply. */
            double d = (hash2(x, y, w->seed ^ 0xD17ULL) - 0.5) / 255.0;

            uint32_t A = (uint32_t)lround(clamp01(a) * 255.0);
            uint32_t R = (uint32_t)lround(clamp01(c[0] * a + d) * 255.0);
            uint32_t G = (uint32_t)lround(clamp01(c[1] * a + d) * 255.0);
            uint32_t B = (uint32_t)lround(clamp01(c[2] * a + d) * 255.0);
            /* Premultiplied, and it has to stay that way: a rounded channel
             * above its own alpha is what wlroots asserts on. */
            if (R > A) R = A;
            if (G > A) G = A;
            if (B > A) B = A;
            row[x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }

    if (badge) cairo_surface_destroy(badge);
    free(glow_x);
    free(wobble);
    free(cloud.v);
    free(ridge_y);
    cairo_surface_mark_dirty(surf);
    return surf;
}
