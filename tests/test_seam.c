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

/* Crossing between monitors is the one thing a single-screen machine cannot
 * try, and the mistakes it hides are not subtle to the person who owns two
 * screens: a pointer that arrives somewhere other than where it left, or a
 * shove off the bottom edge teleporting to the screen beside it. Both are
 * asserted here, along with the promise this file exists for — that the
 * overhang of the bigger screen is a way through and not a wall. */

#include "test.h"
#include "seam.h"

/* Tops level, which is where a layout with nothing configured puts them. */
static const SeamBox pair[] = {
    { 0,    0, 1920, 1080 },
    { 1920, 0, 1366,  768 },
};
#define PAIR_N ((int)(sizeof(pair) / sizeof(pair[0])))

/* The same two screens as they actually stand on a desk: the small one hung
 * lower, said with [[output]] position = "1920,150". */
static const SeamBox offset[] = {
    { 0,    0,   1920, 1080 },
    { 1920, 150, 1366,  768 },   /* covers y 150..917 */
};
#define OFFSET_N ((int)(sizeof(offset) / sizeof(offset[0])))

/* One pixel of motion is enough to leave a screen from its last column. */
#define STEP 2.0

static void test_ordinary_crossing_is_left_alone(void) {
    CASE("ordinary crossing");
    double mx = 0, my = 0;
    /* Sideways off the middle of the tall screen, at a height the small screen
     * covers. The plain move already lands there at the same height, so this
     * must keep its hands off it entirely. */
    CHECK_INT(seam_cross(pair, PAIR_N, 1919, 540, STEP, 0, &mx, &my), 0);
    /* Same on the offset layout, at a height inside the small screen's span. */
    CHECK_INT(seam_cross(offset, OFFSET_N, 1919, 500, STEP, 0, &mx, &my), 0);
    /* And well inside one screen, which is not a crossing at all. */
    CHECK_INT(seam_cross(pair, PAIR_N, 400, 400, 10, 10, &mx, &my), 0);
}

static void test_overhang_is_a_way_through(void) {
    CASE("overhang");
    double mx = 0, my = 0;
    /* Below the small screen's bottom: the 312-pixel stretch of edge that was
     * a wall. Out at the nearest point of the seam — its bottom corner — and
     * on the far side of the boundary, not on it. */
    CHECK_INT(seam_cross(pair, PAIR_N, 1919, 980, STEP, 0, &mx, &my), 1);
    CHECK_DBL(mx, 1921, 0.5);
    CHECK_DBL(my, 767, 0.5);

    /* The offset layout has an overhang at BOTH ends: above y=150 as well as
     * below y=917. Each comes out at its own corner. */
    CHECK_INT(seam_cross(offset, OFFSET_N, 1919, 980, STEP, 0, &mx, &my), 1);
    CHECK_DBL(my, 917, 0.5);
    CHECK_INT(seam_cross(offset, OFFSET_N, 1919, 40, STEP, 0, &mx, &my), 1);
    CHECK_DBL(my, 150, 0.5);
}

static void test_height_is_never_rescaled(void) {
    CASE("no rescaling");
    double mx = 0, my = 0;
    /* Nothing about a crossing depends on the ratio of the two screens. A
     * pointer three quarters of the way down the tall screen is at y=810, and
     * on the offset layout that is a height the small screen really covers —
     * so it stays 810, not 3/4 of the small screen's height. */
    CHECK_INT(seam_cross(offset, OFFSET_N, 1919, 810, STEP, 0, &mx, &my), 0);
    /* One pixel further down than the small screen reaches, and the answer
     * moves by one pixel — not by a hundred. */
    CHECK_INT(seam_cross(offset, OFFSET_N, 1919, 918, STEP, 0, &mx, &my), 1);
    CHECK_DBL(my, 917, 0.5);
}

static void test_round_trip(void) {
    CASE("round trip");
    double mx = 0, my = 0;
    /* Out through the overhang and straight back: the way back is an ordinary
     * crossing (the tall screen covers every height the small one does), so
     * the pointer returns at the height it came out at and stays put. */
    CHECK_INT(seam_cross(offset, OFFSET_N, 1919, 980, STEP, 0, &mx, &my), 1);
    CHECK_DBL(my, 917, 0.5);
    double bx = 0, by = 0;
    CHECK_INT(seam_cross(offset, OFFSET_N, mx, my, -STEP, 0, &bx, &by), 0);
}

static void test_nothing_to_land_on(void) {
    CASE("nothing to land on");
    double mx = 0, my = 0;
    /* Off the far right of the layout. */
    CHECK_INT(seam_cross(pair, PAIR_N, 3285, 300, STEP, 0, &mx, &my), 0);
    /* Off the left of the first screen. */
    CHECK_INT(seam_cross(pair, PAIR_N, 0, 300, -STEP, 0, &mx, &my), 0);
    /* A pointer that is on no screen at all (mid-unplug) is left alone. */
    CHECK_INT(seam_cross(pair, PAIR_N, 2000, 900, STEP, 0, &mx, &my), 0);
    /* A single screen has no seam. */
    CHECK_INT(seam_cross(pair, 1, 1919, 540, STEP, 0, &mx, &my), 0);
}

static void test_down_is_not_sideways(void) {
    CASE("down is not sideways");
    double mx = 0, my = 0;
    /* Pushed off the BOTTOM of the small screen. The tall screen covers that
     * height and is the only other one there is — but it is beside, not below,
     * and a pointer must not be teleported onto it. */
    CHECK_INT(seam_cross(pair, PAIR_N, 2500, 767, 0, STEP, &mx, &my), 0);
}

static void test_stacked(void) {
    CASE("stacked screens");
    /* One above the other and narrower, offset to the right: the same rule on
     * the other axis. Straight down where the narrow one is there is an
     * ordinary crossing; further left than it reaches, out at its corner. */
    const SeamBox stack[] = { { 0, 0, 1920, 1080 }, { 300, 1080, 1280, 720 } };
    double mx = 0, my = 0;
    CHECK_INT(seam_cross(stack, 2, 960, 1079, 0, STEP, &mx, &my), 0);
    CHECK_INT(seam_cross(stack, 2, 100, 1079, 0, STEP, &mx, &my), 1);
    CHECK_DBL(mx, 300, 0.5);
    CHECK_DBL(my, 1081, 0.5);
}

static void test_nearest_of_two_neighbours(void) {
    CASE("nearest neighbour");
    /* Two small screens stacked to the right of the tall one, with a gap
     * between them that no screen covers. A pointer pushed into that gap goes
     * to whichever of the two its edge is nearer. */
    const SeamBox three[] = {
        { 0,    0,   1920, 1080 },
        { 1920, 0,   1280,  300 },   /* upper: y 0..299   */
        { 1920, 700, 1280,  300 },   /* lower: y 700..999 */
    };
    double mx = 0, my = 0;
    CHECK_INT(seam_cross(three, 3, 1919, 380, STEP, 0, &mx, &my), 1);
    CHECK_DBL(my, 299, 0.5);         /* nearer the upper one's bottom */
    CHECK_INT(seam_cross(three, 3, 1919, 620, STEP, 0, &mx, &my), 1);
    CHECK_DBL(my, 700, 0.5);         /* nearer the lower one's top */
}

int main(void) {
    test_ordinary_crossing_is_left_alone();
    test_overhang_is_a_way_through();
    test_height_is_never_rescaled();
    test_round_trip();
    test_nothing_to_land_on();
    test_down_is_not_sideways();
    test_stacked();
    test_nearest_of_two_neighbours();
    return t_report("seam");
}
