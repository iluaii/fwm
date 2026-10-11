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

#ifndef FWM_LIQUID_H
#define FWM_LIQUID_H

#include <stdbool.h>

/* A liquid standing on a desktop.
 *
 * Particles, not a height map. A height map is a surface over a floor and can
 * only ever make waves: it has no way to hold a splash in the air, a ball
 * floating with gravity off, or a pour landing on itself. Particles carry the
 * liquid, and a grid laid over them every step turns them back into one
 * continuous body: particles to the grid, the laws applied there, the grid
 * back to the particles (APIC, Jiang et al. 2015 — the transfer MLS-MPM is
 * built on). The particles are how it is computed, never what is drawn.
 *
 * Everything it does comes out of the laws it is given and nothing else:
 *
 *   inertia     what moves keeps moving
 *   gravity     whatever vector the caller hands in, zero included
 *   pressure    it cannot be squeezed: wherever it would be, it pushes back
 *               exactly as hard as it takes not to be
 *   viscosity   neighbours moving at different speeds drag on each other
 *   tension     its surface pulls on itself, so a blob left alone rounds into
 *               a ball and a thin sheet gathers into beads
 *
 * There is no other rule anywhere in here. Nothing knows what a splash or a
 * ball IS: gravity off and a pool lifts and gathers itself because tension is
 * all that is left acting on it; gravity back and the ball falls, hits the
 * floor and spreads because now the pressure has a weight to hold up.
 *
 * Incompressible, not "slightly compressible". The pressure is solved for
 * (a projection) rather than read off how squeezed the liquid is, and that
 * was measured rather than assumed: water that falls a screen height hits the
 * floor at ~1500 px/s and its splash runs at twice that, and a liquid that
 * gets its pressure from being squeezed only behaves like water when sound in
 * it is ten times faster than that. On a CPU that is hundreds of steps a
 * frame. Short of it, it is rubber — a ball dropped on the floor squashed
 * into a dome and its fast sheets fell apart into combs. Solving for the
 * pressure costs one sparse solve per step and takes a step as long as the
 * liquid's own motion allows: two or three a frame.
 *
 * Like physics.c it knows nothing of the compositor — no wlroots, no windows,
 * no desktops. Where and when a step runs is the caller's; what it does with
 * threads of its own is split its loops across them (liquid_set_threads), and
 * a step still starts and ends inside the one call.
 *
 * The world is one box, `w` x `h` px, y-down with the origin at its top-left;
 * its floor is exactly the box's bottom edge, and the other walls stand at
 * most a grid cell outside it. Inside it stand whatever solids the caller
 * places (liquid_set_solids) — windows, to the caller; boxes, to it. The
 * caller decides what the box is (a desktop), what stands in it, what the
 * gravity is (that desktop's), and when time passes. That is what lets the
 * whole thing be stepped in a loop by tests/test_liquid.c, and moved to the
 * GPU later without anything outside noticing.
 *
 * Units are px and seconds throughout, and the density is 1. */

typedef struct {
    /* px²/s, kinematic: how thick it is. Tens is water at this scale; the
     * hundreds, oil; the thousands, honey. */
    double viscosity;
    /* px: how strongly the surface pulls, as the capillary length under earth
     * gravity — the size below which a drop on the floor holds itself round
     * rather than spreading flat. Real water's is 0.27 px at 100 px/m, which
     * is no tension anyone could see; a few tens is a liquid that beads, and
     * the only kind that gathers into a ball with gravity off. 0 is none. */
    double tension;
} LiquidMaterial;

typedef struct FwmLiquid FwmLiquid;

/* An empty liquid whose particles sit `spacing` px apart at rest, which is
 * also the finest detail it can have — a drop is never smaller than one
 * particle. At most `budget` particles may be poured into it. NULL if the
 * memory cannot be had. Nothing can be poured until liquid_set_box says how
 * big the world is. */
FwmLiquid *liquid_create(double spacing, int budget);
void liquid_destroy(FwmLiquid *l);

/* The walls: a `w` x `h` px box. A box that shrinks under liquid standing in
 * it pushes that liquid in with it, and what no longer fits in it at all runs
 * over the top and is gone. */
void liquid_set_box(FwmLiquid *l, double w, double h);
void liquid_set_material(FwmLiquid *l, const LiquidMaterial *m);

/* Something solid standing in the liquid: a box `w` x `h` whose unturned
 * top-left is (x, y), turned `angle` radians about its centre (y-down, the
 * way a window's is). `id` says it is the same thing from one call to the
 * next.
 *
 * Nothing goes through a solid; what it moves into is pushed ahead of it and
 * what lies on it stays there; it pulls on the liquid against it only where
 * no air could get in behind — a window lifted out of a pool comes out
 * dripping, not with the pool hanging under it. And it takes up room, so a
 * desktop holds that much less liquid while it is there. A solid is one of
 * two kinds:
 *
 * HELD, with no density: something else moves it, and nothing the liquid
 * does moves it back — a window in the hand, or pinned. The liquid is told
 * where it is, never how fast it goes: the speed is how far it got over a
 * step, and that is the same for a window carried on the pointer or put
 * somewhere by a key.
 *
 * FREE, with a density: a body, as heavy as its density times its area, the
 * liquid's being 1. The pressure on it is solved for together with the
 * pressure in the liquid — not read off the liquid afterwards and handed
 * over, which a body lighter than the liquid it pushes aside cannot survive
 * (it is shoved back harder than it pushed, and the next step harder still).
 * So a free solid is buoyed up by exactly the weight of what it displaces,
 * floats when that is more than its own, sinks when it is less, is slowed by
 * what it has to push out of its way and carried along by what runs into it.
 * It starts each step where it stands, going (vx, vy) px/s and turning
 * `spin` rad/s, under the acceleration (ax, ay) everything but the liquid
 * puts on it — its weight; it turns only if `turns`. What the liquid did to
 * it over the step is read back with liquid_push, for whatever moves it to
 * act on: the liquid moves its own copy of the body over a step, and nothing
 * else. */
typedef struct {
    unsigned id;
    double x, y, w, h;
    double angle;
    double density;      /* against the liquid's; 0 is held */
    double vx, vy, spin; /* free: how fast it is going now */
    double ax, ay;       /* free: px/s², what else pulls on it */
    bool turns;          /* free: it may turn */
} LiquidSolid;

/* The solids for the next step; `n` of them, which may be none. A held one
 * moves over the step from where it stood at the end of the last one to
 * here; one it did not know before simply is there, and so is one that went
 * further in a step than anything is thrown — a window sent across the
 * desktop by a key, not by a hand. A free one starts the step here. A change
 * near the liquid wakes it; far from it, it does not. */
void liquid_set_solids(FwmLiquid *l, const LiquidSolid *s, int n);

/* What the liquid did to free solid `id` over the last step, as the
 * acceleration that does it over the step's time: px/s², and rad/s² about its
 * centre. Applied over the next stretch of the caller's own time, it carries
 * the body where the liquid carried its copy. False, and zeros, for a solid
 * that was not free in it.
 *
 * A liquid that is not stepped — asleep, or not to be seen — still holds up
 * what floats in it, with the pressure it was left in: liquid_hold works that
 * out for the free solids standing where they stood when it was solved, and
 * gives none to one that has moved since. A step that finds the liquid
 * asleep does the same. */
bool liquid_push(const FwmLiquid *l, unsigned id, double *ax, double *ay, double *alpha);
void liquid_hold(FwmLiquid *l);
/* Change the most the liquid may hold. Lowering it below what is already
 * poured keeps what is there and refuses more. */
void liquid_set_budget(FwmLiquid *l, int budget);

/* Split each step across `n` threads, the caller's among them, so n - 1 are
 * started; 1, the default, starts none. Clamped to 1 .. LIQUID_MAX_THREADS.
 * Not while a step is running. The liquid moves the same whatever n is, to
 * the rounding of the sums — n parts of a sum are added in another order. */
#define LIQUID_MAX_THREADS 16
void liquid_set_threads(FwmLiquid *l, int n);
int liquid_threads(const FwmLiquid *l);

/* Pour up to `count` particles as one round blob centred on (cx, cy), moving
 * at (vx, vy) px/s. The blob is pulled inside the walls if it would cross
 * them; liquid already there is poured into, and swells to hold it. Returns
 * how many particles went in — fewer than asked when there is no more room
 * (liquid_room), or by a few when a disc of lattice sites does not come out
 * exactly. */
int liquid_pour(FwmLiquid *l, double cx, double cy, int count,
                double vx, double vy);
void liquid_clear(FwmLiquid *l);

/* Take out up to `max` particles standing within `r` px of (cx, cy) — a drain
 * opened there, which the liquid around it then runs into. Returns how many
 * went. */
int liquid_drain(FwmLiquid *l, double cx, double cy, double r, int max);

/* Advance by `dt` seconds under gravity (gx, gy) px/s², y-down.
 *
 * As many internal steps as the fastest particle and the surface tension
 * need, up to a ceiling — past it the liquid runs slow for that frame rather
 * than takes a step it cannot survive, the same choice the compositor's tick
 * makes. Nothing at all once the liquid has settled (liquid_asleep) and the
 * gravity has not changed. */
void liquid_step(FwmLiquid *l, double dt, double gx, double gy);

int liquid_count(const FwmLiquid *l);
int liquid_budget(const FwmLiquid *l);
/* How many more particles it takes: up to the budget, and up to what the box
 * holds — it cannot be squeezed, so a box is full when its area is, less a
 * little for air. */
int liquid_room(const FwmLiquid *l);
double liquid_spacing(const FwmLiquid *l);

/* Particle positions, px inside the box, `stride` floats from one particle's
 * x to the next one's (x then y). Valid until the next call that changes the
 * liquid. */
const float *liquid_positions(const FwmLiquid *l, int *stride);

/* Settled: the liquid has crawled for long enough that the step has stopped
 * running. A pour, a clear, a new box, a new material or a change of gravity
 * wakes it. An empty liquid is asleep. */
bool liquid_asleep(const FwmLiquid *l);

/* For a debug line and for the tests: how many internal steps the last
 * liquid_step took, the fastest particle and the root-mean-square speed after
 * it (px/s), and the pressure solve's iterations in its last step. */
int liquid_last_substeps(const FwmLiquid *l);
double liquid_max_speed(const FwmLiquid *l);
double liquid_rms_speed(const FwmLiquid *l);
int liquid_last_iterations(const FwmLiquid *l);

#endif /* FWM_LIQUID_H */
