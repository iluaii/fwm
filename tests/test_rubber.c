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

/* The resize rubber's geometry.
 *
 * Worth its own file for a plain reason: this effect cannot be tried any other
 * way here. It only ever runs under a hand on a window edge, the headless
 * backend has no pointer, and fwm serves no virtual-pointer protocol — so
 * without these the arithmetic would ship on nothing but a careful reading.
 *
 * What every case below is really asserting is the one promise the effect
 * makes: the picture is drawn at 1:1 and never scaled, whatever the box does.
 * A destination that does not match its source, in the picture's own units, is
 * a stretch — and a stretch is the mush this replaced. */

#include "test.h"
#include "rubber.h"

/* Destination equals source, measured in layout pixels: what "1:1" means when
 * the buffer may be at some other scale. Only the PICTURE is held to this on
 * both axes — an edge strip is one pixel across on purpose and is stretched
 * along that axis alone, which is what fills the box without inventing
 * detail. */
static void check_unscaled(const RubberPart *p, double bx, double by) {
    if (!p->on) return;
    CHECK_DBL(p->sw / bx, (double)p->w, 0.001);
    CHECK_DBL(p->sh / by, (double)p->h, 0.001);
}

static void test_box_matches_the_picture(void) {
    CASE("box the size of the picture");
    RubberPart p[RUBBER_PARTS];
    rubber_parts(800, 600, 800, 600, 800, 600, p);

    /* The whole picture, and nothing else on. */
    CHECK_INT(p[RUBBER_PICTURE].on, 1);
    CHECK_INT(p[RUBBER_PICTURE].w, 800);
    CHECK_INT(p[RUBBER_PICTURE].h, 600);
    CHECK_DBL(p[RUBBER_PICTURE].sw, 800, 0.001);
    CHECK_DBL(p[RUBBER_PICTURE].sh, 600, 0.001);
    CHECK_INT(p[RUBBER_RIGHT].on, 0);
    CHECK_INT(p[RUBBER_BOTTOM].on, 0);
    CHECK_INT(p[RUBBER_CORNER].on, 0);
}

static void test_smaller_box_crops(void) {
    CASE("a smaller box crops rather than squashes");
    RubberPart p[RUBBER_PARTS];
    rubber_parts(800, 600, 800, 600, 300, 200, p);

    /* Sampling only the corner of the picture that fits, at 1:1. */
    CHECK_INT(p[RUBBER_PICTURE].w, 300);
    CHECK_INT(p[RUBBER_PICTURE].h, 200);
    CHECK_DBL(p[RUBBER_PICTURE].sw, 300, 0.001);
    CHECK_DBL(p[RUBBER_PICTURE].sh, 200, 0.001);
    check_unscaled(&p[RUBBER_PICTURE], 1.0, 1.0);
    /* Nothing to fill: the picture already covers the box. */
    CHECK_INT(p[RUBBER_RIGHT].on, 0);
    CHECK_INT(p[RUBBER_BOTTOM].on, 0);
    CHECK_INT(p[RUBBER_CORNER].on, 0);
}

static void test_wider_box_fills_one_edge(void) {
    CASE("wider only: the right strip, and nothing below");
    RubberPart p[RUBBER_PARTS];
    rubber_parts(800, 600, 800, 600, 1000, 600, p);

    CHECK_INT(p[RUBBER_PICTURE].w, 800);
    CHECK_INT(p[RUBBER_PICTURE].h, 600);
    CHECK_INT(p[RUBBER_RIGHT].on, 1);
    CHECK_INT(p[RUBBER_RIGHT].x, 800);      /* butted against the picture */
    CHECK_INT(p[RUBBER_RIGHT].y, 0);
    CHECK_INT(p[RUBBER_RIGHT].w, 200);      /* exactly the shortfall */
    CHECK_INT(p[RUBBER_RIGHT].h, 600);
    /* ONE column, the last one. Two would be a gradient. */
    CHECK_DBL(p[RUBBER_RIGHT].sx, 799, 0.001);
    CHECK_DBL(p[RUBBER_RIGHT].sw, 1, 0.001);
    /* Along its length it is 1:1 — only across is it stretched, and across it
     * there is a single pixel, so there is no detail to smear. */
    CHECK_DBL(p[RUBBER_RIGHT].sh, 600, 0.001);
    CHECK_INT(p[RUBBER_BOTTOM].on, 0);
    CHECK_INT(p[RUBBER_CORNER].on, 0);
}

static void test_bigger_box_fills_and_covers(void) {
    CASE("bigger both ways: three pieces, no gap, no overlap");
    RubberPart p[RUBBER_PARTS];
    rubber_parts(800, 600, 800, 600, 1000, 700, p);

    CHECK_INT(p[RUBBER_RIGHT].on, 1);
    CHECK_INT(p[RUBBER_BOTTOM].on, 1);
    CHECK_INT(p[RUBBER_CORNER].on, 1);

    /* The four pieces tile the box exactly: picture 800x600 at (0,0), the
     * right strip 200x600 at (800,0), the bottom 800x100 at (0,600), and the
     * corner 200x100 at (800,600). Anything else leaves a hole showing the
     * desktop through the middle of a window. */
    CHECK_INT(p[RUBBER_RIGHT].x + p[RUBBER_RIGHT].w, 1000);
    CHECK_INT(p[RUBBER_RIGHT].y, 0);
    CHECK_INT(p[RUBBER_RIGHT].h, 600);
    CHECK_INT(p[RUBBER_BOTTOM].x, 0);
    CHECK_INT(p[RUBBER_BOTTOM].w, 800);
    CHECK_INT(p[RUBBER_BOTTOM].y + p[RUBBER_BOTTOM].h, 700);
    CHECK_INT(p[RUBBER_CORNER].x, 800);
    CHECK_INT(p[RUBBER_CORNER].y, 600);
    CHECK_INT(p[RUBBER_CORNER].w, 200);
    CHECK_INT(p[RUBBER_CORNER].h, 100);
    /* The corner comes from the picture's own corner pixel. */
    CHECK_DBL(p[RUBBER_CORNER].sx, 799, 0.001);
    CHECK_DBL(p[RUBBER_CORNER].sy, 599, 0.001);
    CHECK_DBL(p[RUBBER_CORNER].sw, 1, 0.001);
    CHECK_DBL(p[RUBBER_CORNER].sh, 1, 0.001);
}

static void test_scaled_buffer(void) {
    CASE("a HiDPI picture is measured in its own pixels");
    RubberPart p[RUBBER_PARTS];
    /* 800x600 of window held in a 1600x1200 buffer: scale 2. */
    rubber_parts(800, 600, 1600, 1200, 1000, 700, p);

    /* Still 1:1 on screen — the source is twice the destination because the
     * buffer's pixels are half the size, which is not a stretch. */
    check_unscaled(&p[RUBBER_PICTURE], 2.0, 2.0);
    /* The strips, on the axis where they are not a single pixel. */
    CHECK_DBL(p[RUBBER_RIGHT].sh / 2.0, (double)p[RUBBER_RIGHT].h, 0.001);
    CHECK_DBL(p[RUBBER_BOTTOM].sw / 2.0, (double)p[RUBBER_BOTTOM].w, 0.001);
    CHECK_INT(p[RUBBER_PICTURE].w, 800);
    CHECK_DBL(p[RUBBER_PICTURE].sw, 1600, 0.001);
    /* The last column in BUFFER pixels is two wide and starts at 1598. */
    CHECK_DBL(p[RUBBER_RIGHT].sx, 1598, 0.001);
    CHECK_DBL(p[RUBBER_RIGHT].sw, 2, 0.001);
    /* And it is never sampled past the end of the buffer. */
    CHECK_INT(p[RUBBER_RIGHT].sx + p[RUBBER_RIGHT].sw <= 1600, 1);
}

static void test_the_case_that_started_this(void) {
    CASE("minimum pulled out to full screen");
    RubberPart p[RUBBER_PARTS];
    /* The screenshot: a window squeezed to nothing and dragged to 2560x1440.
     * The old effect magnified one frame twelve times over. */
    rubber_parts(160, 120, 160, 120, 2560, 1440, p);

    /* The picture is still its own size. That is the whole fix. */
    CHECK_INT(p[RUBBER_PICTURE].w, 160);
    CHECK_INT(p[RUBBER_PICTURE].h, 120);
    check_unscaled(&p[RUBBER_PICTURE], 1.0, 1.0);
    /* And the rest of the box is covered by the three strips. */
    CHECK_INT(p[RUBBER_RIGHT].w, 2400);
    CHECK_INT(p[RUBBER_BOTTOM].h, 1320);
    CHECK_INT(p[RUBBER_CORNER].w, 2400);
    CHECK_INT(p[RUBBER_CORNER].h, 1320);
}

static void test_nonsense_draws_nothing(void) {
    CASE("nothing to draw is said, not divided by");
    RubberPart p[RUBBER_PARTS];

    rubber_parts(0, 600, 800, 600, 800, 600, p);
    CHECK_INT(p[RUBBER_PICTURE].on, 0);
    rubber_parts(800, 600, 0, 0, 800, 600, p);
    CHECK_INT(p[RUBBER_PICTURE].on, 0);
    rubber_parts(800, 600, 800, 600, 0, 0, p);
    CHECK_INT(p[RUBBER_PICTURE].on, 0);
    rubber_parts(800, 600, 800, 600, -5, -5, p);
    CHECK_INT(p[RUBBER_PICTURE].on, 0);

    CASE("a one-pixel picture is a legal picture");
    rubber_parts(1, 1, 1, 1, 500, 400, p);
    CHECK_INT(p[RUBBER_PICTURE].w, 1);
    CHECK_DBL(p[RUBBER_PICTURE].sx, 0, 0.001);
    CHECK_DBL(p[RUBBER_CORNER].sx, 0, 0.001);   /* not -1 into the buffer */
    CHECK_INT(p[RUBBER_CORNER].w, 499);
    CHECK_INT(p[RUBBER_CORNER].h, 399);
}

int main(void) {
    test_box_matches_the_picture();
    test_smaller_box_crops();
    test_wider_box_fills_one_edge();
    test_bigger_box_fills_and_covers();
    test_scaled_buffer();
    test_the_case_that_started_this();
    test_nonsense_draws_nothing();
    return t_report("rubber");
}
