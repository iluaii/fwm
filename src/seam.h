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

#ifndef FWM_SEAM_H
#define FWM_SEAM_H

/* ── the seam between two screens ─────────────────────────────────────────
 *
 * Where the pointer comes out when it leaves one monitor for the next.
 *
 * A crossing keeps its height. That is not this file's doing — a pointer moved
 * sideways onto a neighbouring screen simply keeps its y, and the layout is
 * where the two screens' real positions are written down: monitors that stand
 * at different heights on the desk are told so with [[output]] position, and
 * then the height that is kept is the physically right one.
 *
 * What this file is for is the part of an edge with nothing on the other side.
 * Two monitors of different sizes share an edge that only one of them covers
 * all of — a 1920x1080 beside a 1366x768 has 312 pixels of right edge with no
 * screen beyond it — and a pointer kept inside the layout, at the closest point
 * to wherever it was asked to go, stops dead against that overhang. To get
 * across you first have to remember to go up.
 *
 * So a crossing that has no screen at its own height comes out at the NEAREST
 * POINT of the neighbour's edge instead: the corner of the seam, which is as
 * close to where the hand was pushing as a screen exists. No proportional
 * rescaling — leaving the middle of the tall screen must not arrive somewhere
 * that is not the same place, and where the two screens do overlap this code
 * does nothing at all and says so by returning 0.
 *
 * Kept free of wlroots so the arithmetic can be asserted on its own; the
 * compositor's half is cursor_cross_seam in server_pointer.c. */

/* Screens, in layout coordinates — a copy of the monitors' boxes. */
typedef struct {
    int x, y, width, height;
} SeamBox;

/* More monitors than anyone plugs in; the caller stops filling at this. */
#define SEAM_MAX_SCREENS 16

/* A pointer at (x,y) asked to move by (dx,dy).
 *
 * Returns 1 when the move leaves the screen it is on for a place no screen
 * covers, and a neighbour on that side can take it — with the landing point in
 * out_x and out_y, already inside that neighbour and never on its far edge (the
 * layout's right and bottom bounds are exclusive, and a pointer exactly on one
 * is a pointer on no monitor at all).
 *
 * Returns 0 when there is nothing to do: the move stays on the same screen,
 * lands on another by itself, starts on no screen, or heads off the layout
 * with nothing on that side. The caller then moves the pointer the ordinary
 * way. */
int seam_cross(const SeamBox *screens, int count,
               double x, double y, double dx, double dy,
               double *out_x, double *out_y);

#endif /* FWM_SEAM_H */
