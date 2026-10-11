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

/* The liquid is nothing but laws, so the laws are what is asserted: a blob
 * falls at g, a thrown one keeps going, a pool holds the depth it was poured
 * at and then goes still, a surface pulls a strip into a ball and no surface
 * leaves it a strip. None of it can be checked by eye — a liquid that has lost
 * a tenth of its volume, or drifts sideways with nothing pushing it, looks
 * like a liquid — and every one of them is a way an earlier version of
 * liquid.c got it wrong. liquid.c reaches nothing but libm. */

#include "test.h"
#include "liquid.h"

#include <math.h>
#include <stdlib.h>

#define DT (1.0 / 60.0)
#define G  981.0
#define SPACING 6.0

typedef struct {
    double cx, cy;            /* centre of mass */
    double x0, x1, y0, y1;    /* bounding box */
    double aspect;            /* long axis / short axis of the second moments */
    int n;
    int bad;                  /* not finite, or outside the box */
} Shape;

static Shape shape_of(const FwmLiquid *l, double w, double h) {
    Shape s = { .x0 = 1e9, .x1 = -1e9, .y0 = 1e9, .y1 = -1e9 };
    int st;
    const float *p = liquid_positions(l, &st);
    s.n = liquid_count(l);
    for (int i = 0; i < s.n; i++) {
        double x = p[i * st], y = p[i * st + 1];
        /* The side walls and the ceiling may stand up to a cell outside the
         * box (liquid.h); the floor is exact. */
        if (!isfinite(x) || !isfinite(y) || x < -2.0 * SPACING || x > w + 2.0 * SPACING
            || y < -4.0 * SPACING || y > h + 0.01) {
            s.bad++;
            continue;
        }
        s.cx += x; s.cy += y;
        if (x < s.x0) s.x0 = x;
        if (x > s.x1) s.x1 = x;
        if (y < s.y0) s.y0 = y;
        if (y > s.y1) s.y1 = y;
    }
    if (s.n) { s.cx /= s.n; s.cy /= s.n; }
    double ixx = 0, iyy = 0, ixy = 0;
    for (int i = 0; i < s.n; i++) {
        double x = p[i * st] - s.cx, y = p[i * st + 1] - s.cy;
        ixx += x * x; iyy += y * y; ixy += x * y;
    }
    double tr = ixx + iyy, det = ixx * iyy - ixy * ixy;
    double disc = sqrt(fmax(tr * tr / 4.0 - det, 0.0));
    double big = tr / 2.0 + disc, small = tr / 2.0 - disc;
    s.aspect = small > 0.0 ? sqrt(big / small) : 1e9;
    return s;
}

static int by_y(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* The y that `frac` of the particles stand above. */
static double height_below(const FwmLiquid *l, double frac) {
    int st, n = liquid_count(l);
    const float *p = liquid_positions(l, &st);
    float *ys = malloc(sizeof(float) * (size_t)n);
    for (int i = 0; i < n; i++) ys[i] = p[i * st + 1];
    qsort(ys, (size_t)n, sizeof(float), by_y);
    double y = ys[(int)(frac * n)];
    free(ys);
    return y;
}

/* How many threads every liquid here steps on: see main. */
static int threads = 1;

static FwmLiquid *make(double w, double h, double viscosity, double tension) {
    FwmLiquid *l = liquid_create(SPACING, 20000);
    liquid_set_threads(l, threads);
    liquid_set_box(l, w, h);
    LiquidMaterial m = { .viscosity = viscosity, .tension = tension };
    liquid_set_material(l, &m);
    return l;
}

static void run(FwmLiquid *l, double seconds, double gx, double gy) {
    int frames = (int)lround(seconds / DT);
    for (int f = 0; f < frames; f++) liquid_step(l, DT, gx, gy);
}

/* ── pouring ─────────────────────────────────────────────────────────── */

static void test_pour(void) {
    CASE("a pour puts in what was asked, inside the walls");
    FwmLiquid *l = make(1000, 600, 0, 0);
    int got = liquid_pour(l, 500, 300, 1500, 0, 0);
    CHECK(got >= 1450 && got <= 1500);
    CHECK_INT(liquid_count(l), got);
    Shape s = shape_of(l, 1000, 600);
    CHECK_INT(s.bad, 0);
    CHECK_DBL(s.cx, 500, 3);
    CHECK_DBL(s.cy, 300, 3);
    /* Round: a disc of that many particles at rest spacing. */
    double r = SPACING * sqrt(1500 / M_PI);
    CHECK_DBL(s.x1 - s.x0, 2 * r, 2 * SPACING);
    CHECK_DBL(s.aspect, 1.0, 0.05);

    CASE("pouring onto liquid pours into it, and it swells to hold it");
    /* With a surface to hold it together: with none, the push of swelling
     * carries an inviscid blob on outward forever, which is right and is not
     * what is being measured. */
    liquid_set_material(l, &(LiquidMaterial){ .viscosity = 60, .tension = 30 });
    got = liquid_pour(l, 500, 300, 1500, 0, 0);
    CHECK(got >= 1450);
    run(l, 3.0, 0, 0);
    s = shape_of(l, 1000, 600);
    CHECK_INT(s.bad, 0);
    /* Twice the particles, twice the area: sqrt 2 the width. */
    CHECK_DBL(s.x1 - s.x0, 2 * r * sqrt(2.0), 0.15 * 2 * r * sqrt(2.0));

    CASE("a big pour into a pool by the floor swells it, and nothing runs away");
    /* The second blob is pulled up off the floor onto the first, and the
     * cells they share start twice as full as liquid can be. Spreading them
     * once sped the particles up, which shortened the step, which sped the
     * spreading up — 43000 px/s and every step a frame may take. */
    FwmLiquid *big = make(1920, 1080, 60, 30);
    liquid_pour(big, 960, 700, 10000, 0, 0);
    liquid_pour(big, 960, 900, 10000, 0, 0);
    double fastest = 0;
    int most_steps = 0;
    for (int f = 0; f < 60; f++) {
        liquid_step(big, DT, 0, G);
        if (liquid_max_speed(big) > fastest) fastest = liquid_max_speed(big);
        if (liquid_last_substeps(big) > most_steps) most_steps = liquid_last_substeps(big);
    }
    CHECK(fastest < 8000);
    CHECK(most_steps < 12);
    CHECK_INT(shape_of(big, 1920, 1080).bad, 0);
    liquid_destroy(big);

    CASE("a blob poured against a wall is pulled inside it");
    liquid_clear(l);
    CHECK_INT(liquid_count(l), 0);
    CHECK(liquid_asleep(l));
    got = liquid_pour(l, -200, 900, 800, 0, 0);
    CHECK(got >= 780);
    s = shape_of(l, 1000, 600);
    CHECK_INT(s.bad, 0);
    CHECK(s.x0 >= 0 && s.y1 <= 600);

    CASE("a drain takes what stands over it and nothing else");
    liquid_clear(l);
    liquid_set_material(l, &(LiquidMaterial){ .viscosity = 0, .tension = 0 });
    liquid_pour(l, 500, 300, 1500, 0, 0);
    int before = liquid_count(l);
    CHECK_INT(liquid_drain(l, 500, 300, 30, 10), 10);
    CHECK_INT(liquid_count(l), before - 10);
    int gone = liquid_drain(l, 500, 300, 30, 100000);
    /* A disc of radius 30 holds about pi*30²/36 of them. */
    CHECK(gone > 60 && gone < 90);
    CHECK_INT(liquid_drain(l, 900, 100, 30, 100), 0);
    Shape d = shape_of(l, 1000, 600);
    CHECK_INT(d.bad, 0);
    int st;
    const float *dp = liquid_positions(l, &st);
    int inside = 0;
    for (int i = 0; i < liquid_count(l); i++) {
        double ddx = dp[i * st] - 500, ddy = dp[i * st + 1] - 300;
        if (ddx * ddx + ddy * ddy <= 30 * 30) inside++;
    }
    CHECK_INT(inside, 0);

    CASE("the budget is a ceiling");
    liquid_clear(l);
    liquid_set_budget(l, 300);
    got = liquid_pour(l, 500, 300, 1000, 0, 0);
    CHECK(got > 280 && got <= 300);
    liquid_pour(l, 200, 300, 50, 0, 0);
    liquid_pour(l, 800, 300, 50, 0, 0);
    CHECK_INT(liquid_count(l), 300);
    CHECK_INT(liquid_pour(l, 100, 100, 50, 0, 0), 0);
    liquid_destroy(l);

    CASE("a box holds no more than its area, and full it still settles");
    /* Liquid past the box's area has nowhere to be: poured 5% over, a full
     * desktop once thrashed at the ceiling of steps for good. */
    l = liquid_create(SPACING, 100000);
    liquid_set_threads(l, threads);
    liquid_set_box(l, 600, 400);
    liquid_set_material(l, &(LiquidMaterial){ .viscosity = 60, .tension = 30 });
    int brim = (int)(600 * 400 / (SPACING * SPACING));
    for (int k = 0; k < 40; k++) {
        liquid_pour(l, 100 + (k % 5) * 100, 100, 1500, 0, 0);
        run(l, 0.1, 0, G);
    }
    CHECK(liquid_count(l) <= brim);
    CHECK(liquid_count(l) >= 0.9 * brim);
    CHECK_INT(liquid_room(l), 0);
    CHECK_INT(liquid_pour(l, 300, 100, 100, 0, 0), 0);
    run(l, 3.0, 0, G);
    CHECK(liquid_max_speed(l) < 1000);
    CHECK(liquid_last_substeps(l) < 6);

    CASE("a box made smaller than the liquid in it spills the top of it");
    int before_shrink = liquid_count(l);
    liquid_set_box(l, 600, 200);
    CHECK(liquid_count(l) < before_shrink);
    CHECK(liquid_count(l) <= (int)(600 * 200 / (SPACING * SPACING)));
    CHECK_INT(shape_of(l, 600, 200).bad, 0);
    run(l, 2.0, 0, G);
    CHECK(liquid_max_speed(l) < 1000);
    liquid_destroy(l);

    CASE("nothing can be poured into a world with no size");
    l = liquid_create(SPACING, 100);
    CHECK_INT(liquid_pour(l, 10, 10, 50, 0, 0), 0);
    liquid_destroy(l);
}

/* ── the laws ────────────────────────────────────────────────────────── */

static void test_free_fall(void) {
    CASE("a blob falls at g and keeps its shape on the way");
    FwmLiquid *l = make(1000, 3000, 30, 20);
    liquid_pour(l, 500, 400, 1200, 0, 0);
    Shape a = shape_of(l, 1000, 3000);
    run(l, 0.5, 0, G);
    Shape b = shape_of(l, 1000, 3000);
    /* Half g t², as any falling thing; nothing in the liquid holds it up. */
    CHECK_DBL(b.cy - a.cy, 0.5 * G * 0.25, 0.04 * 0.5 * G * 0.25);
    CHECK_DBL(b.cx, a.cx, 1.0);
    CHECK_DBL(b.x1 - b.x0, a.x1 - a.x0, 2 * SPACING);
    liquid_destroy(l);
}

static void test_inertia(void) {
    CASE("with gravity off, a thrown blob keeps going");
    FwmLiquid *l = make(3000, 2000, 30, 20);
    liquid_pour(l, 800, 1000, 1200, 300, -100);
    Shape a = shape_of(l, 3000, 2000);
    run(l, 2.0, 0, 0);
    Shape b = shape_of(l, 3000, 2000);
    CHECK_DBL(b.cx - a.cx, 600, 30);
    CHECK_DBL(b.cy - a.cy, -200, 30);
    CHECK(!liquid_asleep(l));
    liquid_destroy(l);

    CASE("with gravity off, a blob at rest stays where it is");
    l = make(2000, 2000, 30, 30);
    liquid_pour(l, 1000, 1000, 1500, 0, 0);
    a = shape_of(l, 2000, 2000);
    run(l, 5.0, 0, 0);
    b = shape_of(l, 2000, 2000);
    CHECK_DBL(b.cx, a.cx, 25);
    CHECK_DBL(b.cy, a.cy, 25);
    liquid_destroy(l);
}

static void test_pool(void) {
    CASE("a pour comes to rest as a pool as deep as its volume says");
    const double W = 1200, H = 700;
    FwmLiquid *l = make(W, H, 60, 30);
    int n = 0;
    for (int k = 0; k < 4; k++) n += liquid_pour(l, 150 + k * 300, 250, 1200, 0, 0);
    double t = 0;
    while (!liquid_asleep(l) && t < 40) { liquid_step(l, DT, 0, G); t += DT; }
    CHECK(liquid_asleep(l));
    if (!liquid_asleep(l)) fprintf(stderr, "    still moving after %.0fs: rms %.1f max %.1f\n",
                                   t, liquid_rms_speed(l), liquid_max_speed(l));
    Shape s = shape_of(l, W, H);
    CHECK_INT(s.bad, 0);
    CHECK_INT(s.n, n);
    /* Its depth: every particle is spacing² of area, spread over the floor.
     * Measured under the highest 2% of the particles rather than at the very
     * top, because with a surface tension a drop can hang off the ceiling the
     * way one does off a real one; the surface particles stand half a spacing
     * below the top of the liquid they stand for, which is the allowance. */
    double depth = n * SPACING * SPACING / W;
    CHECK_DBL(H - height_below(l, 0.02), depth, 0.1 * depth + SPACING);
    /* And flat: at rest nothing holds one side up. */
    CHECK(s.x0 < 2 * SPACING && s.x1 > W - 2 * SPACING);

    CASE("a settled pool stays exactly where it settled");
    int st;
    const float *p = liquid_positions(l, &st);
    float x = p[0], y = p[1];
    run(l, 1.0, 0, G);
    p = liquid_positions(l, &st);
    CHECK(p[0] == x && p[1] == y);
    CHECK_INT(liquid_last_substeps(l), 0);

    CASE("gravity turned round wakes it, and it falls to the ceiling");
    run(l, 4.0, 0, -G);
    s = shape_of(l, W, H);
    CHECK_INT(s.bad, 0);
    CHECK(s.cy < 0.25 * H);
    liquid_destroy(l);
}

static void test_tension(void) {
    /* A strip in zero gravity: five blobs touching in a row. */
    for (int with = 0; with <= 1; with++) {
        FwmLiquid *l = make(2400, 1400, 60, with ? 40 : 0);
        for (int k = 0; k < 5; k++) liquid_pour(l, 800 + k * 120, 700, 700, 0, 0);
        Shape a = shape_of(l, 2400, 1400);
        run(l, 6.0, 0, 0);
        Shape b = shape_of(l, 2400, 1400);
        if (with) {
            CASE("a surface pulls a strip of liquid into one round body");
            CHECK(a.aspect > 3.0);
            CHECK(b.aspect < 1.8);
            CHECK_INT(b.bad, 0);
        } else {
            CASE("with no surface tension nothing gathers it");
            CHECK(b.aspect > 3.0);
        }
        liquid_destroy(l);
    }
}

static void test_viscosity(void) {
    CASE("a thick liquid spreads slower than a thin one");
    double spread[2];
    for (int k = 0; k < 2; k++) {
        FwmLiquid *l = make(2000, 800, k ? 4000 : 20, 0);
        liquid_pour(l, 1000, 650, 2000, 0, 0);
        run(l, 1.0, 0, G);
        Shape s = shape_of(l, 2000, 800);
        CHECK_INT(s.bad, 0);
        spread[k] = s.x1 - s.x0;
        liquid_destroy(l);
    }
    CHECK(spread[1] < 0.8 * spread[0]);
}

static void test_walls(void) {
    CASE("nothing gets through a wall, however hard it is thrown");
    FwmLiquid *l = make(800, 600, 0, 30);
    liquid_pour(l, 400, 300, 1500, 6000, 3000);
    int n = liquid_count(l);
    for (int f = 0; f < 120; f++) {
        liquid_step(l, DT, 0, G);
        Shape s = shape_of(l, 800, 600);
        if (s.bad) { CHECK_INT(s.bad, 0); break; }
    }
    CHECK_INT(liquid_count(l), n);

    CASE("a box that shrinks under the liquid takes it in with it");
    liquid_set_box(l, 400, 300);
    Shape s = shape_of(l, 400, 300);
    CHECK_INT(s.bad, 0);
    CHECK(!liquid_asleep(l));
    run(l, 1.0, 0, G);
    s = shape_of(l, 400, 300);
    CHECK_INT(s.bad, 0);
    CHECK_INT(liquid_count(l), n);
    liquid_destroy(l);

    CASE("the far ends of every knob step without blowing up");
    l = make(1000, 700, 100000, 500);
    liquid_pour(l, 500, 200, 2000, 0, 0);
    run(l, 2.0, 0, G);
    s = shape_of(l, 1000, 700);
    CHECK_INT(s.bad, 0);
    CHECK_INT(liquid_count(l), 2000);
    liquid_destroy(l);
}

/* ── solids ──────────────────────────────────────────────────────────── */

/* Particles standing inside a box turned `angle` about its centre — deeper
 * than `skin` px, so that one lying on the face does not count. */
static int inside(const FwmLiquid *l, LiquidSolid b, double skin) {
    int st, in = 0;
    const float *p = liquid_positions(l, &st);
    double cx = b.x + b.w / 2, cy = b.y + b.h / 2, c = cos(b.angle), sn = sin(b.angle);
    for (int i = 0; i < liquid_count(l); i++) {
        double dx = p[i * st] - cx, dy = p[i * st + 1] - cy;
        double lx = c * dx + sn * dy, ly = -sn * dx + c * dy;
        if (fabs(lx) < b.w / 2 - skin && fabs(ly) < b.h / 2 - skin) in++;
    }
    return in;
}

/* The surface of a pool between x0 and x1, from how much of it is there: at
 * rest every particle holds its own square. */
static double surface_between(const FwmLiquid *l, double H, double x0, double x1) {
    int st, n = 0;
    const float *p = liquid_positions(l, &st);
    for (int i = 0; i < liquid_count(l); i++) n += p[i * st] > x0 && p[i * st] < x1;
    return H - n * SPACING * SPACING / (x1 - x0);
}

static void settle(FwmLiquid *l, double most) {
    for (double t = 0; !liquid_asleep(l) && t < most; t += DT) liquid_step(l, DT, 0, G);
}

static void test_solids(void) {
    const double W = 1200, H = 700;

    CASE("a window standing in a pool raises it by what it displaces");
    FwmLiquid *l = make(W, H, 60, 30);
    LiquidSolid post = { .id = 1, .x = 500, .y = H - 450, .w = 200, .h = 450 };
    liquid_set_solids(l, &post, 1);
    int n = 0;
    for (int k = 0; k < 4; k++) n += liquid_pour(l, 150 + k * 300, 200, 2000, 0, 0);
    /* Two deep, narrow basins slosh for longer than a test should wait for
     * them to sleep — a box that narrow does without any window in it. */
    run(l, 25.0, 0, G);
    CHECK_INT(inside(l, post, 1.0), 0);
    CHECK_INT(liquid_count(l), n);
    /* The window covers the cells whose centres it covers: 16 of 12 px. Read
     * off the pool on either side of it — some of what was poured onto it
     * stays on top, a puddle on a flat roof. */
    double depth = n * SPACING * SPACING / (W - 16 * 12.0);
    double level = 0.5 * (surface_between(l, H, 30, 480) + surface_between(l, H, 720, W - 30));
    CHECK_DBL(H - level, depth, 0.1 * depth + SPACING);

    liquid_destroy(l);

    CASE("a window lifted out of a pool comes out dripping, not with the pool");
    /* Slowly, its bottom under the surface: held to it everywhere, the
     * liquid under it would be drawn up after it in a column. Let go where
     * air can get in, what still hangs there when it is well clear is what
     * the surface tension holds up — no more than a film as thick as the
     * tension's own length, 30 px under its 300: 250 particles. */
    l = make(W, H, 60, 30);
    n = 0;
    for (int k = 0; k < 4; k++) n += liquid_pour(l, 150 + k * 300, 250, 1800, 0, 0);
    run(l, 10.0, 0, G);
    double surface = H - n * SPACING * SPACING / W;
    LiquidSolid lid = { .id = 2, .x = 450, .y = surface - 100, .w = 300, .h = 180 };
    liquid_set_solids(l, &lid, 1);
    run(l, 2.0, 0, G);
    int hanging = -1;
    for (int f = 0; f < 300 && hanging < 0; f++) {
        lid.y -= 150 * DT;
        liquid_set_solids(l, &lid, 1);
        liquid_step(l, DT, 0, G);
        double bottom = lid.y + lid.h;
        if (bottom > surface - 100) continue;
        int st;
        const float *q = liquid_positions(l, &st);
        hanging = 0;
        for (int i = 0; i < liquid_count(l); i++) {
            double x = q[i * st], y = q[i * st + 1];
            hanging += x > 450 && x < 750 && y > bottom - 2 && y < surface - 30;
        }
    }
    CHECK(hanging >= 0 && hanging < 250);
    liquid_destroy(l);

    CASE("liquid poured into a cup of three solids stays in it");
    l = make(W, H, 60, 30);
    LiquidSolid cup[3] = {
        { .id = 1, .x = 400, .y = 300, .w = 24, .h = 220 },
        { .id = 2, .x = 776, .y = 300, .w = 24, .h = 220 },
        { .id = 3, .x = 400, .y = 496, .w = 400, .h = 24 },
    };
    liquid_set_solids(l, cup, 3);
    n = liquid_pour(l, 600, 380, 1500, 0, 0);
    run(l, 4.0, 0, G);
    CHECK_INT(shape_of(l, W, H).bad, 0);
    int st, in_cup = 0, under = 0;
    const float *p = liquid_positions(l, &st);
    for (int i = 0; i < liquid_count(l); i++) {
        double x = p[i * st], y = p[i * st + 1];
        in_cup += x > 420 && x < 780 && y < 500;
        under += y > 520;
    }
    CHECK(in_cup >= 0.98 * n);
    CHECK_INT(under, 0);
    for (int k = 0; k < 3; k++) CHECK_INT(inside(l, cup[k], 1.0), 0);

    CASE("taking the bottom out of the cup lets it all fall");
    liquid_set_solids(l, cup, 2);
    CHECK(!liquid_asleep(l));
    run(l, 2.0, 0, G);
    /* Not every drop: what lands throws spray as high as the ceiling, cup or
     * no cup. The body of it. */
    CHECK(shape_of(l, W, H).cy > 560);
    liquid_destroy(l);

    CASE("a window dropped into a pool throws it up, and displaces it");
    l = make(W, H, 60, 30);
    for (int k = 0; k < 4; k++) liquid_pour(l, 150 + k * 300, 300, 1800, 0, 0);
    settle(l, 40);
    n = liquid_count(l);
    surface = H - n * SPACING * SPACING / W;
    LiquidSolid slab = { .id = 7, .x = 450, .y = surface - 260, .w = 300, .h = 120 };
    liquid_set_solids(l, &slab, 1);
    liquid_step(l, DT, 0, G);
    double highest = H;
    for (int f = 0; f < 90; f++) {
        if (slab.y + slab.h < H - 30) slab.y += 900 * DT;
        liquid_set_solids(l, &slab, 1);
        liquid_step(l, DT, 0, G);
        Shape sh = shape_of(l, W, H);
        if (sh.y0 < highest) highest = sh.y0;
    }
    CHECK(highest < surface - 80);
    CHECK_INT(inside(l, slab, 1.0), 0);
    CHECK_INT(shape_of(l, W, H).bad, 0);
    /* Sunk to 30 px off the floor, all 120 px of it under: 10 rows of 25
     * cells, over 1200 px. */
    run(l, 8.0, 0, G);
    CHECK_DBL(H - height_below(l, 0.02), n * SPACING * SPACING / W + 250 * 144.0 / W, 20);
    liquid_destroy(l);

    CASE("a window moving far above a settled pool does not wake it");
    l = make(W, H, 60, 30);
    for (int k = 0; k < 4; k++) liquid_pour(l, 150 + k * 300, 250, 1200, 0, 0);
    settle(l, 40);
    CHECK(liquid_asleep(l));
    LiquidSolid far = { .id = 9, .x = 100, .y = 20, .w = 200, .h = 100 };
    for (int f = 0; f < 30; f++) {
        far.x += 5;
        liquid_set_solids(l, &far, 1);
        liquid_step(l, DT, 0, G);
    }
    CHECK(liquid_asleep(l));

    CASE("a window put somewhere by a key is there, without a wave the size of the desktop");
    far.y = H - 200;   /* 480 px in one step, into the pool */
    liquid_set_solids(l, &far, 1);
    CHECK(!liquid_asleep(l));
    double fastest = 0;
    for (int f = 0; f < 30; f++) {
        liquid_step(l, DT, 0, G);
        if (liquid_max_speed(l) > fastest) fastest = liquid_max_speed(l);
    }
    CHECK(fastest < 2500);
    CHECK_INT(inside(l, far, 1.0), 0);
    CHECK_INT(shape_of(l, W, H).bad, 0);
    liquid_destroy(l);

    CASE("a window tumbling along the floor of a pool does not send it wild");
    /* What spin mode does: a turned window rolling through a pool, its
     * corners sweeping the floor and meeting the walls. Liquid shut in
     * between a corner and a wall once had no pressure that could answer it
     * and went to millions of px/s; liquid squeezed out of a closing gap, to
     * tens of thousands, with every frame at the ceiling of steps. */
    l = make(W, H, 60, 30);
    for (int k = 0; k < 4; k++) liquid_pour(l, 150 + k * 300, 250, 1800, 0, 0);
    run(l, 3.0, 0, G);
    n = liquid_count(l);
    LiquidSolid tumbler = { .id = 4, .w = 400, .h = 250 };
    double turn = 0.3, cx = W / 2, dir = 1, wildest = 0;
    int ceiling = 0;
    for (int f = 0; f < 300; f++) {
        turn += 2.5 * DT;
        double c = fabs(cos(turn)), sn = fabs(sin(turn));
        double ex = c * 200 + sn * 125, ey = sn * 200 + c * 125;
        cx += dir * 400 * DT;
        if (cx + ex > W - 1) { cx = W - 1 - ex; dir = -1; }
        if (cx - ex < 1) { cx = 1 + ex; dir = 1; }
        tumbler.x = cx - 200;
        tumbler.y = H - 1 - ey - 125;
        tumbler.angle = turn;
        liquid_set_solids(l, &tumbler, 1);
        liquid_step(l, DT, 0, G);
        if (liquid_max_speed(l) > wildest) wildest = liquid_max_speed(l);
        ceiling += liquid_last_substeps(l) >= 16;
    }
    CHECK(wildest < 10000);
    CHECK_INT(ceiling, 0);
    CHECK_INT(shape_of(l, W, H).bad, 0);
    CHECK(liquid_count(l) > 0.99 * n);
    liquid_destroy(l);

    CASE("a turning paddle stirs a pool and stays dry inside");
    l = make(W, H, 60, 30);
    for (int k = 0; k < 4; k++) liquid_pour(l, 150 + k * 300, 300, 1800, 0, 0);
    settle(l, 40);
    LiquidSolid paddle = { .id = 3, .x = 450, .y = H - 300, .w = 300, .h = 40 };
    double rms = 0;
    for (int f = 0; f < 90; f++) {
        paddle.angle += 2.0 * DT;
        liquid_set_solids(l, &paddle, 1);
        liquid_step(l, DT, 0, G);
        if (liquid_rms_speed(l) > rms) rms = liquid_rms_speed(l);
    }
    CHECK(rms > 30);
    CHECK(inside(l, paddle, 2.0) < 5);
    CHECK_INT(shape_of(l, W, H).bad, 0);
    liquid_destroy(l);
}

/* ── free solids ─────────────────────────────────────────────────────── */

/* What the compositor's physics does with a window between two steps of the
 * liquid, cut down to one body: the liquid steps from where the window is,
 * then the window goes on under its weight and what the liquid did to it,
 * and stops at the walls. */
static void body_frame(FwmLiquid *l, LiquidSolid *b, double W, double H, double g) {
    b->ax = 0.0;
    b->ay = g;
    liquid_set_solids(l, b, 1);
    liquid_step(l, DT, 0, g);
    double ax, ay, alpha;
    liquid_push(l, b->id, &ax, &ay, &alpha);
    b->vx += ax * DT;
    b->vy += (g + ay) * DT;
    b->spin += alpha * DT;
    b->x += b->vx * DT;
    b->y += b->vy * DT;
    b->angle += b->spin * DT;
    if (b->y + b->h > H) { b->y = H - b->h; if (b->vy > 0) b->vy = 0; }
    if (b->x < 0)        { b->x = 0;        if (b->vx < 0) b->vx = 0; }
    if (b->x + b->w > W) { b->x = W - b->w; if (b->vx > 0) b->vx = 0; }
}

/* A pool, and a window of `density` let go with its bottom on its surface;
 * how deep the window floats. Averaged over its last seconds: in a pool this
 * narrow it bobs for a long while — the waves it makes come back off the
 * walls, as they would in a bathtub. */
static double draft_of(double density, double w, double h, double *speed) {
    const double W = 1200, H = 700;
    FwmLiquid *l = make(W, H, 60, 30);
    for (int k = 0; k < 4; k++) liquid_pour(l, 150 + k * 300, 300, 1800, 0, 0);
    settle(l, 40);
    double surface = surface_between(l, H, 0, W);
    LiquidSolid b = { .id = 5, .x = W / 2 - w / 2, .y = surface - h, .w = w, .h = h,
                      .density = density };
    double fastest = 0, sum = 0;
    int n = 0;
    for (int f = 0; f < 1200; f++) {
        body_frame(l, &b, W, H, G);
        if (fabs(b.vy) > fastest) fastest = fabs(b.vy);
        if (f < 720) continue;
        double level = 0.5 * (surface_between(l, H, 30, 300) + surface_between(l, H, W - 300, W - 30));
        sum += b.y + b.h - level;
        n++;
    }
    if (speed) *speed = fastest;
    CHECK_INT(inside(l, b, 1.0), 0);
    CHECK_INT(shape_of(l, W, H).bad, 0);
    liquid_destroy(l);
    return sum / n;
}

static void test_bodies(void) {
    CASE("a window lighter than water floats as deep in it as it is heavy");
    /* Archimedes: the weight of what it displaces holds up its own, so it
     * sits its density's share of its height under. To a cell and a half:
     * the grid takes the pressure on a face from the middle of the cell
     * beside it, and finds the surface between two cells. */
    double d50 = draft_of(0.5, 300, 160, NULL);
    CHECK_DBL(d50, 80, 18);
    double d25 = draft_of(0.25, 300, 160, NULL);
    CHECK_DBL(d25, 40, 18);
    double d75 = draft_of(0.75, 300, 160, NULL);
    CHECK_DBL(d75, 120, 18);
    CHECK(d25 < d50 && d50 < d75);

    CASE("a window heavier than water sinks to the floor");
    const double W = 1200, H = 700;
    FwmLiquid *l = make(W, H, 60, 30);
    for (int k = 0; k < 4; k++) liquid_pour(l, 150 + k * 300, 300, 1800, 0, 0);
    settle(l, 40);
    double surface = surface_between(l, H, 0, W);
    LiquidSolid lead = { .id = 6, .x = 450, .y = surface - 120, .w = 300, .h = 120, .density = 2.0 };
    double slowest_sink = 0;
    for (int f = 0; f < 300; f++) {
        double y0 = lead.y;
        body_frame(l, &lead, W, H, G);
        /* Slowed by the water it has to push aside: never as fast as it
         * would fall in air over the same time. */
        if (f == 60) slowest_sink = (lead.y - y0) / DT;
    }
    CHECK_DBL(lead.y + lead.h, H, 1.0);
    CHECK(slowest_sink < G * 1.0);
    CHECK_INT(inside(l, lead, 1.0), 0);
    liquid_destroy(l);

    CASE("a window far lighter than the water it lands in floats, and does not fly off");
    /* Lighter than what it pushes aside, and wide and thin, so the water it
     * shoves outweighs it many times — the case that pushing it with the
     * last step's pressure cannot survive. */
    double fastest;
    double d5 = draft_of(0.05, 600, 60, &fastest);
    CHECK_DBL(d5, 3, 18);
    CHECK(fastest < 2000);

    CASE("a plank floats flat, and a tilted one rights itself");
    l = make(W, H, 60, 30);
    for (int k = 0; k < 4; k++) liquid_pour(l, 150 + k * 300, 300, 1800, 0, 0);
    settle(l, 40);
    surface = surface_between(l, H, 0, W);
    LiquidSolid plank = { .id = 8, .x = 400, .y = surface - 70, .w = 400, .h = 80,
                          .density = 0.5, .turns = true, .angle = 0.5 };
    for (int f = 0; f < 600; f++) body_frame(l, &plank, W, H, G);
    double tilt = fabs(remainder(plank.angle, M_PI));
    CHECK(tilt < 0.15);
    liquid_destroy(l);

    CASE("asleep, the pool still holds a window up");
    l = make(W, H, 60, 30);
    for (int k = 0; k < 4; k++) liquid_pour(l, 150 + k * 300, 300, 1800, 0, 0);
    settle(l, 40);
    surface = surface_between(l, H, 0, W);
    LiquidSolid raft = { .id = 10, .x = 450, .y = surface - 160 + 80, .w = 300, .h = 160,
                         .density = 0.5 };
    /* The pool is asleep already; the window in it wakes it. */
    int f = 0;
    do body_frame(l, &raft, W, H, G);
    while (++f < 3600 && !liquid_asleep(l));
    CHECK(liquid_asleep(l));
    /* Held where it went to sleep by its buoyancy there: all but the little
     * it was still bobbing by. The compositor's physics puts a window that
     * still sleeps; this one never stops being moved. */
    double rest = raft.y;
    for (f = 0; f < 120; f++) body_frame(l, &raft, W, H, G);
    CHECK_DBL(raft.y, rest, 1.0);
    liquid_destroy(l);

    CASE("with gravity off, a window thrown into a ball of liquid is slowed by it");
    l = make(W, H, 60, 30);
    liquid_pour(l, 700, 350, 6000, 0, 0);
    run(l, 1.0, 0, 0);
    LiquidSolid dart = { .id = 11, .x = 100, .y = 320, .w = 120, .h = 60,
                         .density = 0.5, .vx = 600 };
    for (f = 0; f < 90; f++) body_frame(l, &dart, W, H, 0);
    CHECK(dart.vx < 400);
    CHECK(dart.vx > -100);
    CHECK_INT(shape_of(l, W, H).bad, 0);
    liquid_destroy(l);
}

/* ── several liquids ─────────────────────────────────────────────────── */

/* Of liquid `kind`: how many particles, their mean height, and the share of
 * them above `y`. */
static void kind_stats(const FwmLiquid *l, unsigned kind, double y, int *n, double *mean,
                       double *above) {
    int st, sk, c = 0, up = 0;
    double sum = 0.0;
    const float *p = liquid_positions(l, &st);
    const unsigned *k = liquid_kinds_of(l, &sk);
    for (int i = 0; i < liquid_count(l); i++) {
        if (k[i * sk] != kind) continue;
        c++;
        sum += p[i * st + 1];
        up += p[i * st + 1] < y;
    }
    *n = c;
    *mean = c ? sum / c : 0.0;
    *above = c ? (double)up / c : 0.0;
}

static void test_layers(void) {
    const double W = 1200, H = 700;
    const LiquidMaterial oil = { .viscosity = 60, .density = 0.6 };
    const LiquidMaterial mercury = { .viscosity = 30, .density = 13.0 };

    CASE("a lighter liquid poured under a heavier one comes up through it");
    FwmLiquid *l = make(W, H, 60, 30);
    liquid_set_kind(l, 1, &oil);
    int no = 0, nw = 0;
    for (int k = 0; k < 4; k++) no += liquid_pour_kind(l, 1, 150 + k * 300, 560, 1200, 0, 0);
    run(l, 2.0, 0, G);
    for (int k = 0; k < 4; k++) nw += liquid_pour(l, 150 + k * 300, 200, 1500, 0, 0);
    run(l, 25.0, 0, G);
    /* The water's own depth, from the bottom: the oil above that. */
    double interface = H - nw * SPACING * SPACING / W;
    int n;
    double mo, mw, up;
    kind_stats(l, 1, interface + SPACING, &n, &mo, &up);
    CHECK_INT(n, no);
    CHECK(up > 0.9);
    kind_stats(l, 0, interface + SPACING, &n, &mw, &up);
    CHECK_INT(n, nw);
    CHECK(up < 0.1);
    CHECK(mo < mw);
    CHECK_INT(shape_of(l, W, H).bad, 0);
    liquid_destroy(l);

    CASE("a heavy liquid poured onto water sinks to the bottom of it");
    l = make(W, H, 60, 30);
    liquid_set_kind(l, 2, &mercury);
    nw = 0;
    for (int k = 0; k < 4; k++) nw += liquid_pour(l, 150 + k * 300, 400, 1500, 0, 0);
    run(l, 2.0, 0, G);
    int nm = liquid_pour_kind(l, 2, 600, 150, 1500, 0, 0);
    run(l, 15.0, 0, G);
    double floor_layer = H - nm * SPACING * SPACING / W;
    kind_stats(l, 2, floor_layer - 2 * SPACING, &n, &mo, &up);
    CHECK_INT(n, nm);
    CHECK(up < 0.1);
    kind_stats(l, 0, floor_layer - 2 * SPACING, &n, &mw, &up);
    CHECK(up > 0.9);
    CHECK_INT(shape_of(l, W, H).bad, 0);
    liquid_destroy(l);

    CASE("a window between the two densities floats on the edge between them");
    /* 0.8, between oil's 0.6 and water's 1: wholly in the liquids, it hangs
     * across the edge, half in each — to the grid's cell, which the small
     * difference between the two makes worth some tens of px here: what the
     * window floats on is only 0.4 heavier than what it sinks through, and a
     * cell's worth of push on its faces moves it 2.5 times as far as in water
     * alone. It comes out sitting in the water by a few px rather than half
     * its height; that it stops at the edge is the law, and what is held. */
    l = make(W, H, 60, 30);
    liquid_set_kind(l, 1, &oil);
    nw = no = 0;
    for (int k = 0; k < 4; k++) nw += liquid_pour(l, 150 + k * 300, 450, 1500, 0, 0);
    run(l, 2.0, 0, G);
    for (int k = 0; k < 4; k++) no += liquid_pour_kind(l, 1, 150 + k * 300, 150, 1500, 0, 0);
    settle(l, 30);
    interface = H - nw * SPACING * SPACING / W;
    LiquidSolid raft = { .id = 12, .x = 450, .y = interface - 140, .w = 300, .h = 100,
                         .density = 0.8 };
    double sum = 0;
    int f, frames = 0;
    for (f = 0; f < 1200; f++) {
        body_frame(l, &raft, W, H, G);
        if (f < 720) continue;
        sum += raft.y + raft.h - interface;
        frames++;
    }
    /* Through the oil and onto the water: its top under the oil's surface,
     * its bottom no further than three cells either side of where the two
     * meet. */
    double under = sum / frames;
    double oil_top = interface - no * SPACING * SPACING / W;
    CHECK(raft.y > oil_top);
    CHECK(fabs(under) < 36.0);
    CHECK_INT(inside(l, raft, 1.0), 0);
    liquid_destroy(l);

    CASE("taking a liquid out leaves the others");
    l = make(W, H, 60, 30);
    liquid_set_kind(l, 1, &oil);
    nw = liquid_pour(l, 400, 300, 1000, 0, 0);
    no = liquid_pour_kind(l, 1, 800, 300, 1000, 0, 0);
    CHECK_INT(liquid_count_kind(l, 1), no);
    CHECK_INT(liquid_remove_kind(l, 1), no);
    CHECK_INT(liquid_count_kind(l, 1), 0);
    CHECK_INT(liquid_count(l), nw);
    liquid_destroy(l);
}

/* ── foam ────────────────────────────────────────────────────────────── */

static int foamy(const FwmLiquid *l) {
    int st, n = 0;
    const float *f = liquid_foam_of(l, &st);
    for (int i = 0; i < liquid_count(l); i++) n += f[i * st] > 0.3f;
    return n;
}

static void test_foam(void) {
    CASE("a still pool has no foam, a splash whips it up, and it fades");
    FwmLiquid *l = make(1200, 700, 60, 30);
    for (int k = 0; k < 4; k++) liquid_pour(l, 150 + k * 300, 500, 1500, 0, 0);
    settle(l, 40);
    CHECK_INT(foamy(l), 0);
    liquid_pour(l, 600, 120, 2500, 0, 0);
    int most = 0;
    for (int f = 0; f < 90; f++) {
        liquid_step(l, DT, 0, G);
        int n = foamy(l);
        if (n > most) most = n;
    }
    CHECK(most > 50);
    run(l, 10.0, 0, G);
    CHECK(foamy(l) < most / 5);
    liquid_destroy(l);
}

/* ── threads ─────────────────────────────────────────────────────────── */

static void test_threads(void) {
    CASE("a crew of threads is what it was asked to be, within bounds");
    FwmLiquid *l = liquid_create(SPACING, 100);
    CHECK_INT(liquid_threads(l), 1);
    liquid_set_threads(l, 3);
    CHECK_INT(liquid_threads(l), 3);
    liquid_set_threads(l, 0);
    CHECK_INT(liquid_threads(l), 1);
    liquid_set_threads(l, 1000);
    CHECK_INT(liquid_threads(l), LIQUID_MAX_THREADS);
    liquid_destroy(l);

    CASE("split across threads, a splash comes down where it does on one");
    /* Not to the bit: the strips change the preconditioner, and sums of n
     * parts are added in another order. To well under a particle. */
    Shape s[2];
    for (int k = 0; k < 2; k++) {
        FwmLiquid *b = liquid_create(SPACING, 20000);
        liquid_set_box(b, 1200, 700);
        liquid_set_material(b, &(LiquidMaterial){ .viscosity = 60, .tension = 30 });
        liquid_set_threads(b, k ? 4 : 1);
        liquid_pour(b, 400, 250, 4000, 300, 0);
        run(b, 1.5, 0, G);
        s[k] = shape_of(b, 1200, 700);
        liquid_destroy(b);
    }
    CHECK_INT(s[0].bad, 0);
    CHECK_INT(s[1].bad, 0);
    CHECK_INT(s[1].n, s[0].n);
    CHECK_DBL(s[1].cx, s[0].cx, 0.5 * SPACING);
    CHECK_DBL(s[1].cy, s[0].cy, 0.5 * SPACING);

    CASE("a liquid can be moved onto more threads and back between steps");
    l = make(1000, 600, 60, 30);
    liquid_pour(l, 500, 200, 2000, 0, 0);
    run(l, 0.3, 0, G);
    liquid_set_threads(l, 5);
    run(l, 0.3, 0, G);
    liquid_set_threads(l, 2);
    run(l, 0.3, 0, G);
    Shape m = shape_of(l, 1000, 600);
    CHECK_INT(m.bad, 0);
    CHECK_INT(m.n, liquid_count(l));
    liquid_destroy(l);
}

int main(void) {
    test_threads();
    /* Everything twice: on the caller alone, and split across a crew. The
     * laws must not notice the difference. */
    for (threads = 1; threads <= 4; threads += 3) {
        printf("test_liquid: on %d thread%s\n", threads, threads > 1 ? "s" : "");
        test_pour();
        test_free_fall();
        test_inertia();
        test_pool();
        test_tension();
        test_viscosity();
        test_walls();
        test_solids();
        test_bodies();
        test_layers();
        test_foam();
    }
    return t_report("test_liquid");
}
