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

#include "expo_internal.h"
#include <math.h>

/* ── geometry ─────────────────────────────────────────────────────────────
 *
 * Three coordinate systems, and only one function may convert between them:
 *
 *   world  — what a window stores in view->x: absolute px across all ten
 *            desktops, exactly as the flat compositor uses it.
 *   strip  — world plus the gaps opened between the cards. Identical to world
 *            while the gap is closed, which is why zoom 1.0 is the live view.
 *   screen — output-local px, what the cursor is in.
 *
 * The inverse (expo_point) is not a convenience: input has to travel back down
 * this path, and when the strip eventually bends into a curve there must be one
 * place that learns about the curve, not one per event handler. */

/* How far the strip has opened, 0 at the live view and 1 at the near zoom step
 * (and beyond). Drives everything that must not exist at zoom 1.0: the gaps
 * between the cards, their edges, the dimming of the wallpaper behind them. */
double expo_openness(FwmExpo *e) {
    double t = (e->zoom - 1.0) / (EXPO_ZOOM_NEAR - 1.0);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return t;
}

double expo_gap(FwmExpo *e) {
    /* Zero at zoom 1.0, so entering opens the seams rather than jumping them
     * open. Full by the near step — the near step is only a little way out, and
     * a gap that kept growing with the zoom would never be visible there. */
    return EXPO_GAP_FRAC * e->server->screen_width * expo_openness(e);
}

double expo_pitch(FwmExpo *e) {
    return e->server->screen_width + expo_gap(e);
}

/* Screen px per world px. */
double expo_scale(FwmExpo *e) {
    return e->server->screen_width / (e->zoom * expo_pitch(e));
}

/* Strip coordinate the middle of the screen is looking at. */
double expo_center(FwmExpo *e) {
    return e->out->camera_x + e->server->screen_width / 2.0
         + e->home * expo_gap(e) + e->pan;
}

/* Defined further down with the input and the camera they belong to; the tick
 * and the close path need them earlier. */
void expo_canvas_dirty(FwmExpo *e);
void expo_drag_to(FwmExpo *e, double lx, double ly);
void expo_clamp_pan(FwmExpo *e);
bool expo_can_orbit(FwmExpo *e);
void expo_camera_home(FwmExpo *e);

/* ── the ring in space ────────────────────────────────────────────────────
 *
 * The strip lies on a circle whose circumference is the ten desktops plus the
 * seam, and each desktop is a FLAT facet of that circle — a chord, not a piece
 * of the arc. Flat is not an approximation here, it is what a desktop is; and
 * it means a card is four corners rather than a tessellated bend, because a
 * plane in perspective is reproduced exactly by two triangles with a real w.
 *
 * The camera orbits the front of the strip: `pan` turns the ring under it
 * (that degree of freedom always existed), `tilt` lifts it above the ring, and
 * `dist` pulls it back. All three are held as a DEVIATION that eases to zero,
 * because the one property worth protecting is that at zoom 1.0 the strip is
 * pixel-for-pixel the live screen — which is what makes entering and leaving
 * one movement instead of a substitution.
 *
 * Ring space: x right, y down, z toward the viewer, origin at the front of the
 * strip. At zero curvature, zero tilt and distance 1 every formula below
 * reduces to `u * scale + half a screen`, exactly as the flat strip had it. */


/* One lap of the ring: the ten cards, plus the seam standing between the last
 * and the first. Closing the ring shrinks the seam to nothing.
 *
 * Everything that goes round — where a card is placed, where a screen point
 * lands, how far a pan is folded — measures by THIS, not by the ten cards
 * alone. They are the same number only when the ring is shut, and the
 * difference is exactly what used to make the end card jump. */
double expo_lap(FwmExpo *e) {
    return FWM_DESKTOPS * expo_pitch(e)
         + EXPO_SEAM_PITCHES * expo_pitch(e) * e->seam;
}

/* Radius the strip is bent to, in strip px. 0 means flat — the live view, the
 * first frames of opening, and the whole fallback path. */
double expo_radius(FwmExpo *e) {
    /* A scene node cannot be a trapezoid, so the fallback path is flat and
     * says so here rather than in six places downstream. */
    if (!e->gl) return 0.0;
    double k = expo_openness(e);
    if (k < 0.02) return 0.0;
    /* A longer lap is a bigger circle: an open strip is bent more gently than
     * a closed one, which falls out of the seam rather than being a second
     * knob to tune. */
    return expo_lap(e) / (2.0 * M_PI) / k;
}

/* Base viewer distance, and the focal length taken from it. The focal length
 * deliberately does NOT follow `dist`: if it did, pulling the camera back
 * would narrow the lens by the same amount and nothing would appear to move. */
double expo_dist(FwmExpo *e) {
    /* Rising over the ring also backs away from it, or the near wall walks off
     * the bottom of the screen and the view becomes floor. The wheel still
     * has the final say; this only keeps the default flight framed. */
    double lift = 1.0 + EXPO_TILT_PULLBACK * fabs(e->tilt);
    return EXPO_CAM_DIST * expo_pitch(e) * e->dist * lift;
}

double expo_focal(FwmExpo *e) {
    return expo_scale(e) * EXPO_CAM_DIST * expo_pitch(e);
}

/* A point on the strip: `u` strip px from the middle of the screen, `wy` in
 * world pixels down the desktop. */
ExpoPt expo_ring_point(FwmExpo *e, double u, double wy) {
    double r = expo_radius(e);
    ExpoPt p = { .y = wy - e->server->screen_height / 2.0 };
    if (r <= 0.0) {
        p.x = u;
        p.z = 0.0;
    } else {
        double phi = u / r;
        p.x = r * sin(phi);
        p.z = r * cos(phi) - r;
    }
    return p;
}

/* A point on a facet, given its two ends: `s` runs 0..1 across the desktop and
 * may run past either end, which keeps a window that straddles a boundary in
 * the plane of its own desktop instead of bending it onto the arc. */
ExpoPt expo_facet_point(FwmExpo *e, ExpoPt a, ExpoPt b, double s, double wy) {
    ExpoPt p = { .x = a.x + (b.x - a.x) * s,
                 .y = wy - e->server->screen_height / 2.0,
                 .z = a.z + (b.z - a.z) * s };
    return p;
}

/* Ring space → a projected vertex.
 *
 * The camera swings about the CENTRE of the ring, not about the desktop in
 * front of it: tilting then lifts you over the whole drum, which stays in the
 * middle of the view, instead of pivoting on one card and swinging the rest of
 * the ring out of frame. The centre is a radius behind the front of the strip,
 * so the eye stands D + R from it — and at R = 0, or no tilt, every term below
 * cancels back to the flat `D - z` the strip started with. */
struct scene3d_vert expo_project(FwmExpo *e, ExpoPt p, double u, double v) {
    double ct = cos(e->tilt), st = sin(e->tilt);
    double r = expo_radius(e);
    double pz = p.z + r;                 /* about the ring's centre */
    double y =  p.y * ct + pz * st;
    double z = -p.y * st + pz * ct;

    double w = expo_dist(e) + r - z;
    double f = expo_focal(e);
    struct scene3d_vert out = {
        .x = (float)(e->server->screen_width / 2.0 + f * p.x / (w > 1.0 ? w : 1.0)),
        .y = (float)(e->server->screen_height / 2.0 + f * y / (w > 1.0 ? w : 1.0)),
        .w = (float)w,
        .u = (float)u, .v = (float)v,
    };
    return out;
}

/* Where a desktop's card stands along the strip. Always the nearest way round,
 * whether or not the ring is CLOSED: the strip lies on a circle either way, and
 * the seam is what says the two ends have not met. Skipping this while the
 * strip was a "line" is what made the end card appear out of nowhere as the
 * seam shrank. */
double expo_desktop_strip_x(FwmExpo *e, int desktop) {
    double x = (double)desktop * expo_pitch(e);
    double lap = expo_lap(e);
    double centre = expo_center(e) - e->server->screen_width / 2.0;
    while (x - centre >  lap / 2.0) x -= lap;
    while (x - centre < -lap / 2.0) x += lap;
    return x;
}

/* World point on a desktop → strip offset from the middle of the screen. */
double expo_offset(FwmExpo *e, double wx, int desktop) {
    return expo_desktop_strip_x(e, desktop)
         + (wx - (double)desktop * e->server->screen_width)
         - expo_center(e);
}

/* The two ends of a desktop's facet, in ring space. */
/* A ring point moved onto its desktop's own orbit.
 *
 * The ring is a circle of radius r whose centre sits at (0, y, -r) — see
 * expo_ring_point. So moving a point to a different orbit is a scale about
 * that centre and nothing more: the direction from the axis is what says where
 * round the orbit you are, and the length is which orbit. */
static ExpoPt orbit_scale(FwmExpo *e, ExpoPt p, int desktop) {
    double r = expo_radius(e);
    /* Eased in: 1.0 is the shared ring, orbit_r[d] is where this desktop ends
     * up, and the blend walks between them. */
    double k = 1.0 + (e->orbit_r[desktop] - 1.0) * e->orbit_blend;
    if (r <= 0.0 || k <= 0.0 || k == 1.0) return p;
    ExpoPt c = { 0.0, p.y, -r };
    return (ExpoPt){ c.x + (p.x - c.x) * k, p.y, c.z + (p.z - c.z) * k };
}

void expo_facet_ends(FwmExpo *e, int desktop, ExpoPt *a, ExpoPt *b) {
    double sw = e->server->screen_width;
    /* In the orrery each desktop carries its own way round — planets do not
     * keep formation — and rides its own orbit. Everything downstream (the
     * depth sort, the hit test, where a dragged window lands) reads the facet
     * through this one function, so this is the only place that has to know. */
    double phase = e->orrery ? e->orbit_phase[desktop] * e->orbit_blend : 0.0;
    *a = expo_ring_point(e, expo_offset(e, (double)desktop * sw, desktop) + phase, 0);
    *b = expo_ring_point(e, expo_offset(e, ((double)desktop + 1.0) * sw, desktop) + phase, 0);
    if (e->orrery && e->orbit_blend > 0.0) {
        *a = orbit_scale(e, *a, desktop);
        *b = orbit_scale(e, *b, desktop);
    }
}

/* A world point on a desktop → the screen. Through the FACET, not the arc: a
 * desktop is the chord between its two ends, and a point half way along it
 * stands a sagitta inside the circle. Placing it on the arc instead put it a
 * tenth of a desktop out — invisible while only the flat path used this, and
 * hundreds of pixels of disagreement with the ray the moment anything checked
 * one against the other. */
void expo_to_screen(FwmExpo *e, double wx, double wy, int desktop,
                           double *sx, double *sy) {
    ExpoPt a, b;
    expo_facet_ends(e, desktop, &a, &b);
    double s = (wx - (double)desktop * e->server->screen_width)
             / e->server->screen_width;
    struct scene3d_vert v = expo_project(e, expo_facet_point(e, a, b, s, wy), 0, 0);
    if (sx) *sx = v.x;
    if (sy) *sy = v.y;
}

/* ── input back down the same path ────────────────────────────────────────
 *
 * A closed-form inverse died with the free camera, and good riddance: a ray
 * against the facets is both more general and shorter. It is still the ONE
 * place that knows how the strip is shaped — the cursor and the cards cannot
 * disagree, because they are the same function read in opposite directions. */

/* The line of sight through a screen point, in ring space. */
static void expo_ray(FwmExpo *e, double sx, double sy, ExpoPt *origin, ExpoPt *dir) {
    double f = expo_focal(e), d = expo_dist(e);
    double X = (sx - e->server->screen_width / 2.0) / f;
    double Y = (sy - e->server->screen_height / 2.0) / f;
    double ct = cos(e->tilt), st = sin(e->tilt);

    /* Camera space runs (X w, Y w, D - w) as w grows; rotating that back into
     * ring space is linear in w, so the eye is the w = 0 end and the direction
     * is what one unit of w adds. */
    double r = expo_radius(e);
    origin->x = 0.0;
    origin->y = -(d + r) * st;
    origin->z =  (d + r) * ct - r;       /* back out of the ring's centre */
    dir->x = X;
    dir->y = Y * ct + st;
    dir->z = Y * st - ct;
}

/* Screen point → desktop and the world point on it. False when the line of
 * sight misses the strip entirely (the backdrop, or straight through the
 * seam), which callers must treat as "nothing there" rather than as desktop 0. */
bool expo_point(FwmExpo *e, double sx, double sy,
                       int *desktop, double *wx, double *wy) {
    ExpoPt o, dir;
    expo_ray(e, sx, sy, &o, &dir);

    double sw = e->server->screen_width;
    double best_t = 0.0;
    bool found = false;

    for (int d = 0; d < FWM_DESKTOPS; d++) {
        ExpoPt a, b;
        expo_facet_ends(e, d, &a, &b);
        double ex = b.x - a.x, ez = b.z - a.z;
        /* The facet's plane: its horizontal edge crossed with straight down,
         * which points OUT of the drum. A ray running with that normal rather
         * than against it is looking at the card's back, from inside the ring:
         * it is a surface the eye cannot see, and a click must never land on
         * it — the nearer card in front of it owns that pixel. */
        double nx = -ez, nz = ex;
        double denom = nx * dir.x + nz * dir.z;
        if (denom >= -1e-9) continue;   /* edge-on, or seen from behind */
        double t = (nx * (a.x - o.x) + nz * (a.z - o.z)) / denom;
        if (t <= 0.0) continue;                       /* behind the viewer */
        if (found && t >= best_t) continue;           /* something nearer already */

        double hx = o.x + dir.x * t, hz = o.z + dir.z * t;
        double len2 = ex * ex + ez * ez;
        if (len2 < 1e-9) continue;
        double s = ((hx - a.x) * ex + (hz - a.z) * ez) / len2;
        if (s < 0.0 || s > 1.0) continue;             /* past the card's ends */

        best_t = t;
        found = true;
        *desktop = d;
        *wx = (double)d * sw + s * sw;
        *wy = o.y + dir.y * t + e->server->screen_height / 2.0;
    }
    return found;
}

/* Where the strip is looking, as a fractional desktop index — the same number
 * camera_x/screen_width is when the strip is closed. The tray reads it so its
 * marker keeps saying where you are while the strip pans; without that it went
 * on pointing at the desktop you entered from. */
static double expo_position_for(FwmExpo *e, double pan) {
    FwmServer *server = e->server;
    double centre = e->out->camera_x + server->screen_width / 2.0
                  + e->home * expo_gap(e) + pan;
    double p = (centre - server->screen_width / 2.0) / expo_pitch(e);
    if (server->config.camera.wrap) {
        p = fmod(p, (double)FWM_DESKTOPS);
        if (p < 0.0) p += FWM_DESKTOPS;
    }
    if (p < 0.0) p = 0.0;
    if (p > FWM_DESKTOPS - 1) p = FWM_DESKTOPS - 1;
    return p;
}

bool expo_view_position(FwmServer *server, double *pos) {
    FwmExpo *e = server->expo;
    if (!e) return false;
    double p = expo_position_for(e, e->pan);
    *pos = p;
    return true;
}

static int expo_desktop_at(FwmExpo *e, double pan) {
    int d = (int)lround(expo_position_for(e, pan));
    if (d < 0) d = 0;
    if (d >= FWM_DESKTOPS) d = FWM_DESKTOPS - 1;
    return d;
}

int expo_view_desktop(FwmServer *server) {
    FwmExpo *e = server->expo;
    return e ? expo_desktop_at(e, e->pan) : -1;
}

int expo_target_desktop(FwmServer *server) {
    /* Where the strip is HEADED, which is what a step must be measured from:
     * firing next twice in quick succession has to advance two desktops rather
     * than fight a pan still in flight. Same rule the tray's wheel follows. */
    FwmExpo *e = server->expo;
    return e ? expo_desktop_at(e, e->pan_target) : -1;
}

