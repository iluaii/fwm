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

#ifndef FWM_SPACE_H
#define FWM_SPACE_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

/* The wallpaper as a living solar system.
 *
 * Each desktop is a planet on its own orbit round a star; each window is a
 * moon of the planet its desktop is. A window opening is a moon condensing
 * out of a contracting shell of light; a window closing is that moon blowing
 * up. Switching desktop flies the camera off one planet and over the plane
 * of the orbits to the next; focusing a window flies it to that moon, with
 * its planet filling half the sky behind.
 *
 * This file is the WORLD and the CAMERA and nothing else: orbits, moons,
 * where the camera is, what it is flying to, which kind of planet or star is
 * swapping for which. It has no scene, no GL and no wlroots, so all of that
 * can be walked through in tests/test_space.c instead of watched. What it
 * hands out is SpaceUniforms — exactly what space_shader.h reads — and
 * space_draw.c does the drawing.
 *
 * Two clocks, on purpose. `t` is the system's own time: orbits, spin, the
 * weather on the planets; [space] speed scales it and it carries on from the
 * wall clock across restarts, so a planet is where it was left. `real` is
 * seconds from a monotonic clock: births, explosions, flights, the swap of a
 * star — things that happen BECAUSE of something, and must take the time they
 * take whatever the system's speed. */

#define SPACE_PLANETS   FWM_DESKTOPS
#define SPACE_MAX_MOONS 24
#define SPACE_MAX_BOOMS 3

/* How the camera stands off a planet. az is the angle between the star and
 * the camera seen from the planet (0 = the camera has the star at its back
 * and sees a full disc; the sign says which side); el is the height over the
 * plane of the orbits; dist is a fraction of the kind's own standing-off
 * distance, so a planet that changes kind takes the camera with it; bias is
 * how far the gaze is turned from the planet toward the star; roll tips the
 * horizon. Degrees. */
typedef struct {
    double az, el, dist, bias, roll;
} SpaceView;

/* The same for a moon, in the moon's own frame: a is round its orbit, e out
 * of it, dist in the moon's radii. */
typedef struct {
    double a, e, dist, bias, roll;
} SpaceMoonView;

typedef struct {
    double eye[3], fwd[3], roll;
} SpacePose;

typedef struct {
    uint32_t id;       /* the window's */
    int planet;        /* which desktop's planet it circles */
    double orbit;      /* radius, in planet radii */
    double inc, node;  /* the orbit's tilt to the equator, and where it crosses */
    double phase, dir, period;
    double size;       /* radius, in planet radii */
    double seed;       /* its looks: colour, craters */
    double born;       /* real; far in the past for a moon that was simply there */
    bool dying;        /* its window is gone; it blows up at die_at */
    double die_at;
    SpaceMoonView view;
} SpaceMoon;

/* What the camera is looking at: a planet, or one moon of it (moon != 0). */
typedef struct {
    int planet;
    uint32_t moon;
    SpaceView pview;
    SpaceMoonView mview;
} SpaceTarget;

/* One monitor's camera. The world is shared; each screen looks at its own
 * desktop from its own place. */
typedef struct {
    bool ready;            /* has a pose at all */
    SpaceTarget at;        /* where it rests, or is coming from */
    bool flying;
    SpaceTarget to;
    SpacePose from;
    bool from_has_target;
    SpaceTarget from_target;
    double t0, dur;        /* real */
    SpacePose pose;        /* this frame's */
} SpaceCam;

typedef struct {
    int kind, prev;        /* kind now, and the one the wave is replacing */
    double R, dist, tilt;  /* this frame's, mid-swap included */
    double prog;           /* 0..1 how far the wave has gone; 1 = no swap */
    bool changing;
    double t0;
    double fromR, fromDist, fromTilt;
} SpacePlanet;

typedef struct {
    SpacePlanet planet[SPACE_PLANETS];
    struct { int kind, prev; double t0; } star;

    SpaceMoon moon[SPACE_MAX_MOONS];
    int moon_count;
    struct { SpaceMoon moon; double at; } boom[SPACE_MAX_BOOMS];
    int boom_count;

    /* The window each desktop last had focused. A camera looking at that
     * desktop looks at its moon. */
    uint32_t focus[SPACE_PLANETS];
    /* After the focused window closes, its desktop's camera backs off to see
     * the explosion and holds there this long, whatever takes the focus. */
    double hold_until[SPACE_PLANETS];

    double t;              /* the system's own time */
    uint64_t rng;
} Space;

/* A window as space_sync_windows wants it. */
typedef struct {
    uint32_t id;
    int desktop;
} SpaceWindow;

/* Everything the shader reads, already in its units. */
typedef struct {
    float time, real;
    float eye[3], right[3], up[3], fwd[3];
    float tan_half, exposure;
    float pp[SPACE_PLANETS * 3];   /* planet centres */
    float pr[SPACE_PLANETS * 4];   /* radius, kind, spin angle, cloud angle */
    float px[SPACE_PLANETS * 4];   /* kind being replaced, wave 0..1, axial tilt, - */
    float star_r, star_type, star_flash;
    float star_col[3], sun[3];
    float sat[SPACE_MAX_MOONS * 4];   /* centre, radius (0 = no moon) */
    float satb[SPACE_MAX_MOONS * 4];  /* seed, age */
    float boom[SPACE_MAX_BOOMS * 4];  /* centre, age (<0 = none) */
    float boomn[SPACE_MAX_BOOMS * 4]; /* orbit normal, the moon's radius */
} SpaceUniforms;

/* A fresh system: kinds and star from cfg, orbits from `wall_s` (seconds of
 * the wall clock, so it picks up where a previous session left it). */
void space_init(Space *sp, const SpaceConfig *cfg, double wall_s, uint64_t seed);

/* A config reload: a planet whose kind changed is swapped by a wave running
 * pole to pole; a changed star collapses into a flash and the new one grows
 * out of it. `quiet` swaps on the spot (nobody is looking). */
void space_apply_config(Space *sp, const SpaceConfig *cfg, double real, bool quiet);

/* Advance by dt seconds of real time. */
void space_step(Space *sp, const SpaceConfig *cfg, double dt, double real);

/* The windows as they are now. New ones condense into moons, gone ones blow
 * up, moved ones jump to their new planet. cams[d] is the camera looking at
 * desktop d, or NULL: a moon is born where that camera can see it being born.
 * `quiet` makes every change silently — no birth, no explosion — for a wall-
 * paper that was asleep under a fullscreen window when it happened. */
void space_sync_windows(Space *sp, const SpaceWindow *wins, int n,
                        SpaceCam *const cams[SPACE_PLANETS], double aspect,
                        double real, bool quiet);

/* The focused window, 0 for none. Remembered per desktop. */
void space_sync_focus(Space *sp, uint32_t focused);

/* Where `cam` looks for a screen showing `desktop`: the focused moon there,
 * else the planet — flown to when that changes, jumped to when `quiet`.
 * Fills cam->pose. */
void space_cam_update(Space *sp, SpaceCam *cam, int desktop, double real, bool quiet);

/* Whether anything is happening that wants the full frame rate: this camera
 * flying, a moon being born, an explosion, a planet or the star swapping. The
 * rest of the time it is only the system turning, and [space] calm_fps does. */
bool space_busy(const Space *sp, const SpaceCam *cam, double real);

void space_uniforms(const Space *sp, const SpaceCam *cam, const SpaceConfig *cfg,
                    double real, double aspect, SpaceUniforms *u);

/* For the tests: where things are. */
void space_planet_pos(const Space *sp, int i, double out[3]);
void space_moon_pos(const Space *sp, const SpaceMoon *m, double out[3]);
const SpaceMoon *space_find_moon(const Space *sp, uint32_t id);

#endif /* FWM_SPACE_H */
