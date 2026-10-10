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

static FwmLiquid *make(double w, double h, double viscosity, double tension) {
    FwmLiquid *l = liquid_create(SPACING, 20000);
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

int main(void) {
    test_pour();
    test_free_fall();
    test_inertia();
    test_pool();
    test_tension();
    test_viscosity();
    test_walls();
    return t_report("test_liquid");
}
