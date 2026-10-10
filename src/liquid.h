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
 * no desktops, no threads. The world is one box, `w` x `h` px, y-down with the
 * origin at its top-left; its floor is exactly the box's bottom edge, and the
 * other walls stand at most a grid cell outside it. The caller decides what
 * the box is (a desktop), what the gravity is (that desktop's), and when time
 * passes. That is what lets the whole thing be stepped in a loop by
 * tests/test_liquid.c, and moved to the GPU later without anything outside
 * noticing.
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
/* Change the most the liquid may hold. Lowering it below what is already
 * poured keeps what is there and refuses more. */
void liquid_set_budget(FwmLiquid *l, int budget);

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
