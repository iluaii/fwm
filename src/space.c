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

#include "space.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TAU (2.0 * M_PI)
#define D2R (M_PI / 180.0)

/* Vertical field of view, degrees. The shader is told tan of half of it. */
#define SPACE_FOV 50.0

/* How long a planet takes to swap kind (the wave from pole to pole), and a
 * star (old one shrinks into a flash at SWAP, the new one grows out of it). */
#define CHANGE_S   3.2
#define STAR_S     2.4
#define STAR_SWAP  0.4

/* A new window's moon is left to finish being born before the camera flies
 * to it: the birth is the event, and flying at once would miss it. */
#define BIRTH_WATCH_S 1.1
/* The focused window closed: the camera backs off, the moon goes up this much
 * later, and the camera stays back for HOLD before following the focus on. */
#define DIE_DELAY_S 0.5
#define HOLD_S      3.2
#define BOOM_LIFE_S 9.0

/* Kinds, in config_space_kinds order. R is the planet's radius; dist where the
 * camera stands off it, in its radii; tilt its axis (and rings'). */
static const struct { double R, dist, tilt; bool rings; } KIND[SPACE_KIND_COUNT] = {
    { 1.00, 2.75, 0.41, false },   /* terra */
    { 0.75, 2.75, 0.44, false },   /* ochre */
    { 2.60, 5.60, 0.47, true  },   /* saturn */
    { 0.85, 2.75, 0.10, false },   /* ice */
    { 0.80, 2.75, 0.05, false },   /* lava */
    { 1.10, 2.75, 0.30, false },   /* ocean */
    { 2.00, 5.00, 0.50, true  },   /* neptune */
    { 0.90, 2.90, 0.30, false },   /* titan */
    { 1.00, 2.75, 0.35, false },   /* selva */
    { 3.00, 3.30, 0.05, false },   /* jupiter */
};

/* The places on the orbits, one per desktop. The default kinds sit on them
 * hot to cold — lava at 18, ice out at 175 — and a kind swapped onto another
 * slot simply lives there. Periods are seconds of the system's own time. */
static const struct { double orbit, phase, orbitT, spinT, cloudT; } SLOT[] = {
    {  40, 0.4,  7200, 600, 540 },
    {  28, 2.3,  4300, 700, 700 },
    { 125, 4.1, 19000, 400, 400 },
    { 175, 5.6, 34000, 900, 900 },
    {  18, 1.1,  2600, 800, 800 },
    {  64, 3.0,  9800, 650, 560 },
    { 150, 0.2, 26000, 450, 450 },
    {  80, 5.0, 12500, 900, 900 },
    {  52, 2.0,  8500, 620, 520 },
    { 100, 3.7, 15500, 380, 380 },
};
_Static_assert(SPACE_PLANETS == sizeof(SLOT) / sizeof(SLOT[0]),
               "space.c has one orbit per desktop: add slots when FWM_DESKTOPS changes");

/* Stars, in config_space_stars order. col tints the disc and the glare; sun is
 * the colour of the light on the planets and lum its strength. */
static const struct { double R; double col[3], sun[3]; double lum; } STAR[SPACE_STAR_COUNT] = {
    { 1.20, { 1.00, 0.92, 0.80 }, { 1.00, 0.95, 0.88 }, 4.5 },   /* sun */
    { 5.00, { 1.00, 0.52, 0.28 }, { 1.00, 0.66, 0.46 }, 4.0 },   /* red giant */
    { 2.20, { 0.70, 0.82, 1.00 }, { 0.80, 0.88, 1.00 }, 5.0 },   /* blue giant */
    { 0.35, { 0.86, 0.92, 1.00 }, { 0.92, 0.95, 1.00 }, 3.2 },   /* white dwarf */
    { 0.25, { 0.70, 0.85, 1.00 }, { 0.80, 0.88, 1.00 }, 2.6 },   /* pulsar */
    { 0.80, { 1.00, 0.80, 0.60 }, { 1.00, 0.80, 0.60 }, 2.2 },   /* black hole */
};

/* ---------------------------------------------------------------- vectors */

typedef struct { double x, y, z; } V;

static V v3(double x, double y, double z) { return (V){ x, y, z }; }
static V vadd(V a, V b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V vsub(V a, V b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V vmul(V a, double s) { return v3(a.x * s, a.y * s, a.z * s); }
static double vdot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static double vlen(V a) { return sqrt(vdot(a, a)); }
static V vnorm(V a) { double l = vlen(a); return l > 0.0 ? vmul(a, 1.0 / l) : v3(0, 0, 1); }
static V vcross(V a, V b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static V vlerp(V a, V b, double t) { return vadd(a, vmul(vsub(b, a), t)); }
static V vrotY(V a, double ang) {
    double c = cos(ang), s = sin(ang);
    return v3(c * a.x + s * a.z, a.y, -s * a.x + c * a.z);
}
/* The same turn the shader makes for an axial tilt. */
static V vrotX(V a, double ang) {
    double c = cos(ang), s = sin(ang);
    return v3(a.x, c * a.y - s * a.z, s * a.y + c * a.z);
}
static V varr(const double a[3]) { return v3(a[0], a[1], a[2]); }
static void vout(V a, double o[3]) { o[0] = a.x; o[1] = a.y; o[2] = a.z; }

static double clampd(double x, double a, double b) { return x < a ? a : (x > b ? b : x); }
static double smooth(double a, double b, double x) {
    double t = clampd((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
static double ease(double u) { return u * u * u * (u * (u * 6.0 - 15.0) + 10.0); }

static double rnd(Space *sp) {
    uint64_t x = sp->rng;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    sp->rng = x;
    return (double)((x * 0x2545F4914F6CDD1DULL) >> 11) / 9007199254740992.0;
}
static double rrange(Space *sp, double a, double b) { return a + (b - a) * rnd(sp); }

/* ---------------------------------------------------------------- places */

static V planet_pos(const Space *sp, int i, double t) {
    (void)sp;
    double a = SLOT[i].phase + TAU * t / SLOT[i].orbitT;
    return v3(cos(a) * SLOT[i].orbit, sin(a + i) * SLOT[i].orbit * 0.025, sin(a) * SLOT[i].orbit);
}

void space_planet_pos(const Space *sp, int i, double out[3]) { vout(planet_pos(sp, i, sp->t), out); }

/* A moon's orbit is laid against its planet's equator — a ringed planet's
 * moons go round in the plane of the rings, as real ones do. */
static V moon_pos(const Space *sp, const SpaceMoon *m, double t) {
    const SpacePlanet *P = &sp->planet[m->planet];
    double a = m->phase + m->dir * TAU * t / m->period;
    double x = cos(a) * m->orbit * P->R, z = sin(a) * m->orbit * P->R;
    V p = vrotY(v3(x, -z * sin(m->inc), z * cos(m->inc)), m->node);
    return vadd(planet_pos(sp, m->planet, t), vrotX(p, P->tilt));
}

void space_moon_pos(const Space *sp, const SpaceMoon *m, double out[3]) { vout(moon_pos(sp, m, sp->t), out); }

static V moon_normal(const Space *sp, const SpaceMoon *m) {
    return vrotX(vrotY(v3(0, cos(m->inc), sin(m->inc)), m->node), sp->planet[m->planet].tilt);
}

static double moon_radius(const Space *sp, const SpaceMoon *m) { return m->size * sp->planet[m->planet].R; }

const SpaceMoon *space_find_moon(const Space *sp, uint32_t id) {
    if (!id) return NULL;
    for (int i = 0; i < sp->moon_count; i++)
        if (sp->moon[i].id == id) return &sp->moon[i];
    return NULL;
}

/* The camera never goes inside a planet, its rings, or a moon. */
static V keep_out(const Space *sp, V eye, double t) {
    for (int j = 0; j < SPACE_PLANETS; j++) {
        const SpacePlanet *P = &sp->planet[j];
        double lim = P->R * (KIND[P->kind].rings ? 2.7 : 1.6);
        V c = planet_pos(sp, j, t), d = vsub(eye, c);
        if (vlen(d) < lim) eye = vadd(c, vmul(vnorm(d), lim));
    }
    for (int i = 0; i < sp->moon_count; i++) {
        double lim = moon_radius(sp, &sp->moon[i]) * 3.0;
        V c = moon_pos(sp, &sp->moon[i], t), d = vsub(eye, c);
        if (vlen(d) < lim) eye = vadd(c, vmul(vnorm(d), lim));
    }
    return eye;
}

/* ---------------------------------------------------------------- views */

static SpaceView random_view(Space *sp) {
    SpaceView v;
    /* More often the lit side; now and then a crescent. */
    v.az   = (45.0 + 100.0 * pow(rnd(sp), 1.3)) * (rnd(sp) < 0.5 ? -1.0 : 1.0);
    v.el   = rrange(sp, -18, 30);
    v.dist = rrange(sp, 0.85, 1.4);
    v.bias = rrange(sp, 0.05, 0.32);
    v.roll = rrange(sp, -14, 14);
    return v;
}

static SpacePose planet_pose(const Space *sp, int i, double t, const SpaceView *view) {
    const SpacePlanet *P = &sp->planet[i];
    V p = planet_pos(sp, i, t);
    V to_star = vnorm(vmul(p, -1.0));
    double az = view->az * D2R + sin(t * 0.004 + i) * 0.06;
    V dir = vrotY(to_star, az);
    dir = vnorm(vadd(dir, v3(0, tan(view->el * D2R), 0)));
    if (KIND[P->kind].rings) {
        /* Rings are looked at from their lit face and never edge-on. */
        V n = v3(0, cos(P->tilt), sin(P->tilt));
        double side = vdot(to_star, n) < 0.0 ? -1.0 : 1.0;
        double dn = vdot(dir, n);
        if (fabs(dn) < 0.3 || (dn < 0.0 ? -1.0 : 1.0) != side)
            dir = vnorm(vadd(dir, vmul(n, 0.42 * side - dn)));
    }
    SpacePose pose;
    V eye = vadd(p, vmul(dir, P->R * P->dist * view->dist));
    V to_p = vnorm(vsub(p, eye)), to_s = vnorm(vmul(eye, -1.0));
    vout(eye, pose.eye);
    vout(vnorm(vlerp(to_p, to_s, view->bias)), pose.fwd);
    pose.roll = view->roll * D2R;
    return pose;
}

static V moon_dir(const Space *sp, const SpaceMoon *m, double t, double a, double e) {
    V c = planet_pos(sp, m->planet, t), p = moon_pos(sp, m, t);
    V r = vnorm(vsub(p, c)), n = moon_normal(sp, m), tg = vcross(n, r);
    return vadd(vadd(vmul(r, cos(e) * cos(a)), vmul(tg, cos(e) * sin(a))), vmul(n, sin(e)));
}

/* Side light: a moon or a planet lit from straight behind the camera is a
 * flat disc. The direction is picked for an angle camera-moon-star of 70..125
 * degrees, and never from between the moon and its planet. */
static SpaceMoonView random_moon_view(Space *sp, const SpaceMoon *m, double t) {
    V to_star = vnorm(vmul(moon_pos(sp, m, t), -1.0));
    double phase = rrange(sp, 70, 125) * D2R;
    SpaceMoonView best = { 0, 0, 0, 0, 0 };
    double best_score = -1e9;
    for (int k = 0; k < 60; k++) {
        double a = rrange(sp, -115, 115), e = rrange(sp, -35, 35);
        if (cos(e * D2R) * cos(a * D2R) < -0.1) continue;
        V d = moon_dir(sp, m, t, a * D2R, e * D2R);
        double score = -fabs(acos(clampd(vdot(d, to_star), -1.0, 1.0)) - phase);
        if (score > best_score) { best_score = score; best.a = a; best.e = e; }
    }
    best.dist = rrange(sp, 7, 11);
    best.bias = rrange(sp, 0.2, 0.4);
    best.roll = rrange(sp, -12, 12);
    return best;
}

static SpacePose moon_pose(const Space *sp, const SpaceMoon *m, double t, const SpaceMoonView *view) {
    V c = planet_pos(sp, m->planet, t), p = moon_pos(sp, m, t);
    V dir = moon_dir(sp, m, t, view->a * D2R + sin(t * 0.01 + m->seed * 10.0) * 0.05, view->e * D2R);
    V eye = keep_out(sp, vadd(p, vmul(dir, moon_radius(sp, m) * view->dist)), t);
    V to_m = vnorm(vsub(p, eye)), to_p = vnorm(vsub(c, eye));
    SpacePose pose;
    vout(eye, pose.eye);
    vout(vnorm(vlerp(to_m, to_p, view->bias)), pose.fwd);
    pose.roll = view->roll * D2R;
    return pose;
}

/* A target whose moon has gone falls back on its planet. */
static SpacePose target_pose(const Space *sp, const SpaceTarget *tg, double t) {
    const SpaceMoon *m = space_find_moon(sp, tg->moon);
    return m ? moon_pose(sp, m, t, &tg->mview) : planet_pose(sp, tg->planet, t, &tg->pview);
}
static V target_center(const Space *sp, const SpaceTarget *tg, double t) {
    const SpaceMoon *m = space_find_moon(sp, tg->moon);
    return m ? moon_pos(sp, m, t) : planet_pos(sp, tg->planet, t);
}
static double target_radius(const Space *sp, const SpaceTarget *tg) {
    const SpaceMoon *m = space_find_moon(sp, tg->moon);
    return m ? moon_radius(sp, m) : sp->planet[tg->planet].R;
}

/* ---------------------------------------------------------------- flights */

static double bez1(double a, double b, double c, double d, double s) {
    double m = 1.0 - s;
    return a * m * m * m + 3.0 * b * m * m * s + 3.0 * c * m * s * s + d * s * s * s;
}
static V bez(V a, V b, V c, V d, double s) {
    return v3(bez1(a.x, b.x, c.x, d.x, s), bez1(a.y, b.y, c.y, d.y, s), bez1(a.z, b.z, c.z, d.z, s));
}

/* Between desktops a long flight over the plane of the orbits; between the
 * windows of one desktop a short one. */
static double flight_duration(const Space *sp, const SpacePose *from, const SpaceTarget *to, double t) {
    SpacePose B = target_pose(sp, to, t);
    double d = vlen(vsub(varr(B.eye), varr(from->eye)));
    return 1.6 + fmin(d / 6.0, 1.0) + fmin(d / 50.0, 2.4);
}

/* The camera backs away from what it was looking at, rises, and comes down on
 * the target; it looks back at where it came from first, then ahead, then
 * settles into the target's own framing. */
static SpacePose flight_pose(const Space *sp, const SpaceCam *cam, double u, double t) {
    SpacePose B = target_pose(sp, &cam->to, t);
    double s = ease(clampd(u, 0.0, 1.0));
    V P0 = varr(cam->from.eye), P3 = varr(B.eye);
    V ffwd = varr(cam->from.fwd), bfwd = varr(B.fwd);
    double d = vlen(vsub(P3, P0));
    double pull = fmax(d * 0.28, target_radius(sp, &cam->to) * 1.5);
    V lift = v3(0, d * 0.18, 0);
    V P1 = vadd(vsub(P0, vmul(ffwd, pull)), lift);
    V P2 = vadd(vsub(P3, vmul(bfwd, pull)), lift);
    V eye = keep_out(sp, bez(P0, P1, P2, P3, s), t);
    V dest = target_center(sp, &cam->to, t);
    V look = ffwd;
    if (cam->from_has_target)
        look = vnorm(vlerp(look, vnorm(vsub(target_center(sp, &cam->from_target, t), eye)), smooth(0, 0.2, s)));
    V to_dest = vnorm(vsub(dest, eye));
    look = vlerp(look, to_dest, smooth(0.2, 0.6, s));
    if (vlen(look) < 1e-3) look = to_dest;
    look = vnorm(vlerp(vnorm(look), bfwd, smooth(0.72, 1.0, s)));
    SpacePose pose;
    vout(eye, pose.eye);
    vout(look, pose.fwd);
    pose.roll = cam->from.roll + (B.roll - cam->from.roll) * smooth(0.1, 1.0, s);
    return pose;
}

static void camera_basis(const double fwd3[3], double roll, V *right, V *up) {
    V fwd = varr(fwd3);
    V r0 = vnorm(vcross(fwd, v3(0, 1, 0)));
    V u0 = vcross(r0, fwd);
    double c = cos(roll), s = sin(roll);
    *right = vadd(vmul(r0, c), vmul(u0, s));
    *up    = vsub(vmul(u0, c), vmul(r0, s));
}

/* Where p lands on screen, -1..1 each way; false behind the camera. */
static bool project(const SpacePose *pose, double aspect, V p, double q[3]) {
    V right, up;
    camera_basis(pose->fwd, pose->roll, &right, &up);
    V d = vsub(p, varr(pose->eye));
    double z = vdot(d, varr(pose->fwd));
    if (z <= 0.0) return false;
    double th = tan(SPACE_FOV * 0.5 * D2R);
    q[0] = vdot(d, right) / (z * th * aspect);
    q[1] = vdot(d, up) / (z * th);
    q[2] = z;
    return true;
}

static bool hidden_by_planet(const Space *sp, V eye, V p, double t) {
    V d = vsub(p, eye);
    double L = vlen(d);
    V rd = vmul(d, 1.0 / L);
    for (int j = 0; j < SPACE_PLANETS; j++) {
        V oc = vsub(eye, planet_pos(sp, j, t));
        double R = sp->planet[j].R;
        double b = vdot(oc, rd), c = vdot(oc, oc) - R * R, h = b * b - c;
        if (h > 0.0) {
            double tt = -b - sqrt(h);
            if (tt > 0.0 && tt < L) return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- moons */

static void random_moon(Space *sp, SpaceMoon *m, int planet) {
    bool ringed = KIND[sp->planet[planet].kind].rings;
    m->planet = planet;
    m->orbit  = ringed ? rrange(sp, 2.7, 3.4) : rrange(sp, 1.4, 1.95);
    m->inc    = ringed ? rrange(sp, -0.12, 0.12) : rrange(sp, -0.5, 0.5);
    m->node   = rrange(sp, 0, TAU);
    m->phase  = rrange(sp, 0, TAU);
    m->dir    = rnd(sp) < 0.5 ? -1.0 : 1.0;
    m->period = rrange(sp, 140, 260);
    m->size   = ringed ? rrange(sp, 0.03, 0.05) : rrange(sp, 0.04, 0.075);
    m->seed   = rnd(sp);
}

/* A new moon goes where its birth can be seen: in frame, not behind a planet,
 * not up against the camera and not on top of another moon. */
static void place_moon(Space *sp, SpaceMoon *m, int planet, const SpacePose *pose, double aspect) {
    SpaceMoon best;
    double best_score = -1e9;
    for (int k = 0; k < 80; k++) {
        SpaceMoon c = *m;
        random_moon(sp, &c, planet);
        if (!pose) { *m = c; return; }
        V p = moon_pos(sp, &c, sp->t);
        double q[3];
        double score = 0.0;
        if (!project(pose, aspect, p, q)) score -= 100.0;
        else {
            score -= fmax(fabs(q[0]) - 0.7, 0.0) * 20.0 + fmax(fabs(q[1]) - 0.7, 0.0) * 20.0;
            if (hidden_by_planet(sp, varr(pose->eye), p, sp->t)) score -= 50.0;
            if (q[2] < sp->planet[planet].R * 0.8) score -= 10.0;
            for (int i = 0; i < sp->moon_count; i++) {
                if (sp->moon[i].planet != planet) continue;
                double qo[3];
                if (project(pose, aspect, moon_pos(sp, &sp->moon[i], sp->t), qo))
                    score -= fmax(0.25 - hypot(qo[0] - q[0], qo[1] - q[1]), 0.0) * 40.0;
            }
        }
        if (score > best_score) { best_score = score; best = c; }
        if (score >= 0.0) break;
    }
    *m = best;
}

static void boom_now(Space *sp, int i, double real) {
    if (sp->boom_count == SPACE_MAX_BOOMS) {
        memmove(&sp->boom[0], &sp->boom[1], sizeof(sp->boom[0]) * (SPACE_MAX_BOOMS - 1));
        sp->boom_count--;
    }
    sp->boom[sp->boom_count].moon = sp->moon[i];
    sp->boom[sp->boom_count].at = real;
    sp->boom_count++;
    sp->moon[i] = sp->moon[--sp->moon_count];
}

static void drop_moon(Space *sp, int i) { sp->moon[i] = sp->moon[--sp->moon_count]; }

static bool cam_targets(const SpaceCam *cam, uint32_t id) {
    return cam && cam->ready && (cam->at.moon == id || (cam->flying && cam->to.moon == id));
}

void space_sync_windows(Space *sp, const SpaceWindow *wins, int n,
                        SpaceCam *const cams[SPACE_PLANETS], double aspect,
                        double real, bool quiet) {
    /* Gone, or moved to another desktop. */
    for (int i = 0; i < sp->moon_count; ) {
        SpaceMoon *m = &sp->moon[i];
        if (m->dying) { i++; continue; }
        int to = -1;
        for (int k = 0; k < n; k++)
            if (wins[k].id == m->id) { to = wins[k].desktop; break; }
        if (to == m->planet) { i++; continue; }
        if (to >= 0 && to < SPACE_PLANETS) {
            /* Moved: it turns up round its new planet, born again there. */
            uint32_t id = m->id;
            place_moon(sp, m, to, cams[to] && cams[to]->ready ? &cams[to]->pose : NULL, aspect);
            m->id = id;
            m->born = quiet ? -1e9 : real;
            i++;
            continue;
        }
        if (quiet) { drop_moon(sp, i); continue; }
        bool watched = false;
        for (int d = 0; d < SPACE_PLANETS; d++)
            if (cam_targets(cams[d], m->id)) watched = true;
        if (watched) {
            /* The camera is up against it: it backs off first, and the moon
             * goes up once there is room to see it go. */
            m->dying = true;
            m->die_at = real + DIE_DELAY_S;
            sp->hold_until[m->planet] = real + HOLD_S;
            i++;
        } else {
            boom_now(sp, i, real);
        }
    }

    /* New. */
    for (int k = 0; k < n; k++) {
        int d = wins[k].desktop;
        if (d < 0 || d >= SPACE_PLANETS || !wins[k].id) continue;
        if (space_find_moon(sp, wins[k].id)) continue;
        if (sp->moon_count == SPACE_MAX_MOONS) break;
        SpaceMoon *m = &sp->moon[sp->moon_count];
        memset(m, 0, sizeof(*m));
        const SpaceCam *c = cams[d];
        SpacePose look;
        const SpacePose *pose = NULL;
        if (c && c->ready && !quiet) {
            /* Mid-flight, where the camera will be looking once it lands. */
            look = c->flying ? target_pose(sp, &c->to, sp->t) : c->pose;
            pose = &look;
        }
        place_moon(sp, m, d, pose, aspect);
        m->id = wins[k].id;
        m->born = quiet ? -1e9 : real;
        sp->moon_count++;
    }
}

void space_sync_focus(Space *sp, uint32_t focused) {
    const SpaceMoon *m = space_find_moon(sp, focused);
    if (m && !m->dying) sp->focus[m->planet] = focused;
}

/* ---------------------------------------------------------------- swaps */

static void planet_settle(SpacePlanet *P) {
    P->R = KIND[P->kind].R;
    P->dist = KIND[P->kind].dist;
    P->tilt = KIND[P->kind].tilt;
    P->prog = 1.0;
    P->prev = P->kind;
    P->changing = false;
}

static void planets_update(Space *sp, double real) {
    for (int d = 0; d < SPACE_PLANETS; d++) {
        SpacePlanet *P = &sp->planet[d];
        if (!P->changing) { planet_settle(P); continue; }
        double u = clampd((real - P->t0) / CHANGE_S, 0.0, 1.0), e = ease(u);
        P->R    = P->fromR    + (KIND[P->kind].R    - P->fromR)    * e;
        P->dist = P->fromDist + (KIND[P->kind].dist - P->fromDist) * e;
        P->tilt = P->fromTilt + (KIND[P->kind].tilt - P->fromTilt) * e;
        P->prog = u;
        if (u >= 1.0) planet_settle(P);
    }
}

typedef struct {
    int shown;
    double R, flash;
    double col[3], sun[3];
} StarNow;

static StarNow star_now(const Space *sp, double real) {
    double u = clampd((real - sp->star.t0) / STAR_S, 0.0, 1.0);
    StarNow s;
    int a = sp->star.prev, b = sp->star.kind;
    s.shown = u < STAR_SWAP ? a : b;
    double scale = u < STAR_SWAP ? 1.0 - smooth(0.0, STAR_SWAP, u) : smooth(STAR_SWAP, 1.0, u);
    double k = smooth(0.15, 0.85, u);
    double lum = STAR[a].lum + (STAR[b].lum - STAR[a].lum) * k;
    s.R = fmax(STAR[s.shown].R * scale, 0.02);
    for (int j = 0; j < 3; j++) {
        s.col[j] = STAR[s.shown].col[j];
        s.sun[j] = (STAR[a].sun[j] + (STAR[b].sun[j] - STAR[a].sun[j]) * k) * lum;
    }
    s.flash = u < 1.0 ? exp(-pow((u - STAR_SWAP) / 0.07, 2.0)) : 0.0;
    return s;
}

void space_apply_config(Space *sp, const SpaceConfig *cfg, double real, bool quiet) {
    planets_update(sp, real);
    for (int d = 0; d < SPACE_PLANETS; d++) {
        SpacePlanet *P = &sp->planet[d];
        int kind = cfg->planets[d];
        if (kind < 0 || kind >= SPACE_KIND_COUNT || kind == P->kind) continue;
        if (quiet) { P->kind = kind; planet_settle(P); continue; }
        P->fromR = P->R; P->fromDist = P->dist; P->fromTilt = P->tilt;
        P->prev = P->kind;
        P->kind = kind;
        P->t0 = real;
        P->changing = true;
    }
    planets_update(sp, real);

    int star = cfg->star;
    if (star >= 0 && star < SPACE_STAR_COUNT && star != sp->star.kind) {
        if (quiet) {
            sp->star.kind = sp->star.prev = star;
            sp->star.t0 = -1e9;
        } else {
            /* From whatever is on screen now, even mid-swap. */
            sp->star.prev = star_now(sp, real).shown;
            sp->star.kind = star;
            sp->star.t0 = real;
        }
    }
}

/* ---------------------------------------------------------------- life */

void space_init(Space *sp, const SpaceConfig *cfg, double wall_s, uint64_t seed) {
    memset(sp, 0, sizeof(*sp));
    sp->rng = seed ? seed : 0x9E3779B97F4A7C15ULL;
    /* Carried on from the wall clock, so the planets are where they were
     * left; reduced first, so the time that reaches the shader keeps its
     * precision. */
    sp->t = fmod(wall_s * (cfg->speed > 0.0 ? cfg->speed : 1.0), 1.0e6);
    for (int d = 0; d < SPACE_PLANETS; d++) {
        int k = cfg->planets[d];
        sp->planet[d].kind = (k >= 0 && k < SPACE_KIND_COUNT) ? k : d % SPACE_KIND_COUNT;
        planet_settle(&sp->planet[d]);
    }
    int s = cfg->star >= 0 && cfg->star < SPACE_STAR_COUNT ? cfg->star : 0;
    sp->star.kind = sp->star.prev = s;
    sp->star.t0 = -1e9;
}

void space_step(Space *sp, const SpaceConfig *cfg, double dt, double real) {
    if (dt > 0.0) sp->t += dt * cfg->speed;
    planets_update(sp, real);
    for (int i = 0; i < sp->moon_count; ) {
        if (sp->moon[i].dying && real >= sp->moon[i].die_at) boom_now(sp, i, real);
        else i++;
    }
    int keep = 0;
    for (int j = 0; j < sp->boom_count; j++)
        if (real - sp->boom[j].at < BOOM_LIFE_S) sp->boom[keep++] = sp->boom[j];
    sp->boom_count = keep;
}

static bool same_target(const SpaceTarget *a, const SpaceTarget *b) {
    return a->planet == b->planet && a->moon == b->moon;
}

static void fly_to(Space *sp, SpaceCam *cam, const SpaceTarget *to, double real) {
    cam->from = cam->pose;
    cam->from_has_target = !cam->flying;
    cam->from_target = cam->at;
    cam->to = *to;
    cam->t0 = real;
    cam->dur = flight_duration(sp, &cam->from, to, sp->t);
    cam->flying = true;
}

void space_cam_update(Space *sp, SpaceCam *cam, int desktop, double real, bool quiet) {
    if (desktop < 0 || desktop >= SPACE_PLANETS) desktop = 0;

    /* What it should be looking at. */
    SpaceTarget want = { .planet = desktop, .moon = 0 };
    const SpaceMoon *m = real >= sp->hold_until[desktop] ? space_find_moon(sp, sp->focus[desktop]) : NULL;
    if (m && (m->dying || m->planet != desktop)) m = NULL;
    const SpaceTarget *cur = cam->flying ? &cam->to : &cam->at;
    if (m) {
        want.moon = m->id;
        /* A moon still being born is watched from where the camera is. */
        if (!quiet && cam->ready && real - m->born < BIRTH_WATCH_S && cur->planet == desktop)
            want = *cur;
    }

    if (!cam->ready) {
        want.pview = random_view(sp);
        if (m) want.mview = random_moon_view(sp, m, sp->t);
        cam->at = want;
        cam->flying = false;
        cam->ready = true;
    } else if (!same_target(&want, cur)) {
        want.pview = random_view(sp);
        if (want.moon) want.mview = random_moon_view(sp, space_find_moon(sp, want.moon), sp->t);
        if (quiet) {
            cam->at = want;
            cam->flying = false;
        } else {
            fly_to(sp, cam, &want, real);
        }
    }

    /* A target whose moon has gone is its planet now. */
    if (cam->at.moon && !space_find_moon(sp, cam->at.moon)) cam->at.moon = 0;
    if (cam->flying && cam->to.moon && !space_find_moon(sp, cam->to.moon)) cam->to.moon = 0;

    if (cam->flying) {
        double u = (real - cam->t0) / cam->dur;
        if (u >= 1.0) { cam->at = cam->to; cam->flying = false; }
        else { cam->pose = flight_pose(sp, cam, u, sp->t); return; }
    }
    cam->pose = target_pose(sp, &cam->at, sp->t);
}

bool space_busy(const Space *sp, const SpaceCam *cam, double real) {
    if (cam && cam->flying) return true;
    if (sp->boom_count > 0) return true;
    if (real - sp->star.t0 < STAR_S) return true;
    for (int d = 0; d < SPACE_PLANETS; d++)
        if (sp->planet[d].changing) return true;
    for (int i = 0; i < sp->moon_count; i++)
        if (sp->moon[i].dying || real - sp->moon[i].born < 3.5) return true;
    return false;
}

/* ---------------------------------------------------------------- uniforms */

static void fset3(float *o, V a) { o[0] = (float)a.x; o[1] = (float)a.y; o[2] = (float)a.z; }

void space_uniforms(const Space *sp, const SpaceCam *cam, const SpaceConfig *cfg,
                    double real, double aspect, SpaceUniforms *u) {
    (void)aspect;
    memset(u, 0, sizeof(*u));
    double t = sp->t;
    u->time = (float)fmod(t, 20000.0);
    u->real = (float)fmod(real, 10000.0);
    V right, up;
    camera_basis(cam->pose.fwd, cam->pose.roll, &right, &up);
    fset3(u->eye, varr(cam->pose.eye));
    fset3(u->fwd, varr(cam->pose.fwd));
    fset3(u->right, right);
    fset3(u->up, up);
    u->tan_half = (float)tan(SPACE_FOV * 0.5 * D2R);
    u->exposure = (float)cfg->exposure;

    for (int i = 0; i < SPACE_PLANETS; i++) {
        const SpacePlanet *P = &sp->planet[i];
        fset3(&u->pp[i * 3], planet_pos(sp, i, t));
        u->pr[i * 4 + 0] = (float)P->R;
        u->pr[i * 4 + 1] = (float)P->kind;
        u->pr[i * 4 + 2] = (float)fmod(TAU * t / SLOT[i].spinT, TAU);
        u->pr[i * 4 + 3] = (float)fmod(TAU * t / SLOT[i].cloudT + 1.7, TAU);
        u->px[i * 4 + 0] = (float)P->prev;
        u->px[i * 4 + 1] = (float)P->prog;
        u->px[i * 4 + 2] = (float)P->tilt;
    }

    StarNow s = star_now(sp, real);
    u->star_r = (float)s.R;
    u->star_type = (float)s.shown;
    u->star_flash = (float)s.flash;
    for (int j = 0; j < 3; j++) { u->star_col[j] = (float)s.col[j]; u->sun[j] = (float)s.sun[j]; }

    for (int i = 0; i < sp->moon_count && i < SPACE_MAX_MOONS; i++) {
        const SpaceMoon *m = &sp->moon[i];
        fset3(&u->sat[i * 4], moon_pos(sp, m, t));
        u->sat[i * 4 + 3] = (float)moon_radius(sp, m);
        u->satb[i * 4 + 0] = (float)m->seed;
        u->satb[i * 4 + 1] = (float)fmin(real - m->born, 1e6);
    }
    for (int j = 0; j < SPACE_MAX_BOOMS; j++) {
        if (j >= sp->boom_count) { u->boom[j * 4 + 3] = -1.0f; continue; }
        const SpaceMoon *m = &sp->boom[j].moon;
        fset3(&u->boom[j * 4], moon_pos(sp, m, t));
        u->boom[j * 4 + 3] = (float)(real - sp->boom[j].at);
        fset3(&u->boomn[j * 4], moon_normal(sp, m));
        u->boomn[j * 4 + 3] = (float)moon_radius(sp, m);
    }
}
