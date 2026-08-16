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

#include "seam.h"

#include <stddef.h>

static int box_holds(const SeamBox *b, double x, double y) {
    return x >= b->x && x < b->x + b->width
        && y >= b->y && y < b->y + b->height;
}

static const SeamBox *screen_at(const SeamBox *screens, int count, double x, double y) {
    for (int i = 0; i < count; i++) {
        if (screens[i].width <= 0 || screens[i].height <= 0) continue;
        if (box_holds(&screens[i], x, y)) return &screens[i];
    }
    return NULL;
}

/* How far a value is from a screen's span on one axis; 0 while it is inside.
 * This is what picks between two screens stacked on the same side — the one
 * whose edge the pointer is nearest to is the one it is heading for. */
static double span_gap(double v, int lo, int len) {
    if (v < lo) return lo - v;
    if (v > lo + len - 1) return v - (lo + len - 1);
    return 0.0;
}

int seam_cross(const SeamBox *screens, int count,
               double x, double y, double dx, double dy,
               double *out_x, double *out_y) {
    if (!screens || count <= 0) return 0;

    double nx = x + dx, ny = y + dy;
    /* The move lands on a screen by itself. That is the ordinary crossing, and
     * it already comes out at the height it went in at — there is nothing here
     * to improve on and nothing this may touch. */
    if (screen_at(screens, count, nx, ny)) return 0;

    const SeamBox *from = screen_at(screens, count, x, y);
    if (!from || from->width <= 0 || from->height <= 0) return 0;

    int out_h = nx < from->x || nx >= from->x + from->width;
    int out_v = ny < from->y || ny >= from->y + from->height;

    /* Which screen the motion is heading INTO: one that covers the point of the
     * edge being crossed AND lies wholly on the side being left towards. That
     * second half is what keeps a push off the BOTTOM of the small screen from
     * finding the tall screen beside it — which does cover that height, and is
     * emphatically not below anything.
     *
     * Nearest wins, measured on the OTHER axis: with two screens stacked to the
     * right, the pointer is heading for the one whose edge it is closest to. */
    const SeamBox *to = NULL;
    double best = 0.0;

    if (out_h) {
        int rightwards = nx >= from->x + from->width;
        for (int i = 0; i < count; i++) {
            const SeamBox *o = &screens[i];
            if (o == from || o->width <= 0 || o->height <= 0) continue;
            if (nx < o->x || nx >= o->x + o->width) continue;
            if (rightwards ? o->x < from->x + from->width
                           : o->x + o->width > from->x) continue;
            double gap = span_gap(ny, o->y, o->height);
            if (!to || gap < best) { to = o; best = gap; }
        }
    }
    /* A diagonal at a corner leaves on both axes at once. Sideways is tried
     * first because monitors are normally side by side; if there is nothing
     * that way, the same question is asked up and down. */
    if (!to && out_v) {
        int downwards = ny >= from->y + from->height;
        for (int i = 0; i < count; i++) {
            const SeamBox *o = &screens[i];
            if (o == from || o->width <= 0 || o->height <= 0) continue;
            if (ny < o->y || ny >= o->y + o->height) continue;
            if (downwards ? o->y < from->y + from->height
                          : o->y + o->height > from->y) continue;
            double gap = span_gap(nx, o->x, o->width);
            if (!to || gap < best) { to = o; best = gap; }
        }
    }
    if (!to) return 0;

    /* Straight through at the height it left at, and only as far as that edge
     * reaches: the clamp IS the rule — the nearest point of the seam. Never the
     * destination's far edge, which belongs to no monitor (the layout's right
     * and bottom bounds are exclusive). */
    double mx = nx, my = ny;
    if (mx < to->x) mx = to->x;
    if (mx > to->x + to->width  - 1) mx = to->x + to->width  - 1;
    if (my < to->y) my = to->y;
    if (my > to->y + to->height - 1) my = to->y + to->height - 1;

    if (out_x) *out_x = mx;
    if (out_y) *out_y = my;
    return 1;
}
