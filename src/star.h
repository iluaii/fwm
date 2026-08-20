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

#ifndef FWM_STAR_H
#define FWM_STAR_H

#include <stdbool.h>

#include "config.h"
#include "sun.h"

/* A star standing in the world, and what becomes of it.
 *
 * Everything here is arithmetic over [star] and a clock — no scene, no
 * wlroots, no compositor — so a whole stellar lifetime can be walked in a loop
 * (tests/test_star.c) instead of waited for. Drawing it belongs elsewhere; the
 * only thing this hands the compositor is light, in exactly the shape sun.c
 * hands it, so shadow.c never learns that the source changed.
 *
 * Where it differs from the sun is the one thing that matters to a shadow: the
 * sun is infinitely far away, so one dx/dy describes every window on screen.
 * A star is HERE, a few hundred pixels up and to the left, so every window
 * gets its own answer — the shadows fan out from it. That is why star_light
 * takes a point and sun_light does not.
 *
 * The star is not a picture of a star. It burns fuel, it takes mass from the
 * windows thrown past it, and when the fuel runs out what is left of it is
 * decided the way it is decided in the sky: by the mass. Under the
 * Chandrasekhar limit an electron gas holds and it ends a dwarf; up to the
 * Tolman–Oppenheimer–Volkoff limit neutrons hold and it ends a pulsar; past
 * that nothing holds. */

typedef enum {
    STAR_BURNING = 0, /* on the main sequence, spending fuel */
    STAR_COLLAPSE,    /* the squeeze: brighter, bluer, shadows sharpening */
    STAR_DWARF,       /* under the Chandrasekhar limit: an ember */
    STAR_NEUTRON,     /* a pulsar, sweeping its beam across the desktop */
    STAR_HOLE,        /* no light at all, and something to bend the wallpaper */
} FwmStarPhase;

typedef struct FwmStar {
    double wx, wy;        /* where it stands, in world px */
    double mass;          /* solar masses, as the two limits are written */
    double fuel;          /* 1 at ignition, 0 when it collapses */
    FwmStarPhase phase;
    double phase_s;       /* seconds spent in the phase so far */
    double age_s;         /* seconds since it was lit, for the ignition */
    double beam;          /* pulsar beam bearing, deg clockwise from up */
    /* The size it had when THIS collapse started, as a fraction of the burning
     * radius. A second collapse begins from the remnant it is now, not from
     * the star it once was — without this, pushing a dwarf over its limit made
     * it snap back to full size before falling again. */
    double from_frac;
    /* And how brightly it was shining when this collapse started, for the same
     * reason: a pulsar that begins to fall must not jump to the brightness a
     * whole star would have had. */
    double from_lum;
    /* What the hole is drawn at, chasing what its mass says it should be. A
     * swallowed window adds its mass in one frame; without this the hole
     * jumped a step wider each time instead of swelling. */
    double shown_mass;

    /* It moves. A star sits where it was put until something heavy enough
     * shoves it, and then it carries on across the desktop like anything else
     * — which is the whole reason a compositor with physics in it should have
     * one. */
    double vx, vy;   /* px/s */
    int held;        /* being dragged: gravity and drift are suspended */
    /* And it turns on its own axis, the way a thrown window does. Visible
     * because the surface has a texture to carry round — a featureless disc
     * could spin all day and look still. */
    double angle;    /* radians, y-down like every other angle here */
    double angvel;   /* rad/s */
} FwmStar;

/* A star at ignition: full fuel, the configured mass, burning. */
void star_init(FwmStar *star, const StarConfig *cfg);

/* Advance by `dt` seconds: spends fuel, runs the collapse, turns the beam.
 * Fuel running out starts the collapse on its own; the collapse ending picks
 * the remnant from the mass it had at that moment. */
void star_tick(FwmStar *star, const StarConfig *cfg, double dt);

/* A window went past at `speed` px/s. Whatever it gave up becomes mass — which
 * is the only thing that decides how this ends. Ignored once the star is no
 * longer burning: a remnant has nothing left to burn. */
void star_feed(FwmStar *star, const StarConfig *cfg, double speed);

/* Light the fuse now, whatever the fuel says.
 *
 * A remnant that still has something holding it up can be pushed over the edge
 * as well: a dwarf takes the mass to pass Chandrasekhar and falls to a pulsar,
 * a pulsar takes the mass to pass the TOV limit and falls to a hole. That is
 * the mass rule rather than a way around it — a neutron star that gains enough
 * really does have nothing left to hold it. Returns false only for a hole,
 * which has nowhere further to fall. */
bool star_collapse_now(FwmStar *star);

/* How wide it is drawn, in px. Falls through the collapse the way a real one
 * does — free fall, so most of the shrinking happens at the very end — and
 * settles at whatever the remnant is. */
double star_radius(const FwmStar *star, const StarConfig *cfg);

/* How hard it shines, 1 being an ordinary day on the main sequence. A star
 * being squeezed gets BRIGHTER: the surface loses area as the square of the
 * radius but gains temperature faster, which is also why it goes blue. Zero
 * for a hole. */
double star_luminosity(const FwmStar *star, const StarConfig *cfg);

/* Its colour right now, straight RGB 0..1: yellow while it burns, white then
 * blue through the collapse, dull red for an ember, hard blue-white for a
 * pulsar. Black for a hole, which is drawn by not drawing it. */
void star_color(const FwmStar *star, const StarConfig *cfg, float out[3]);

/* The light this star throws on a window whose centre is at (wx, wy) in world
 * coordinates, in the shape shadow.c already takes.
 *
 * The star hangs `height` px above the plane the windows lie on, so a window
 * far from it is lit from a shallow angle and throws a long shadow, and one
 * directly underneath throws almost none — the fan that makes a near source
 * read as near. A remnant with no light in it comes back with alpha 0, which
 * is all that a dead star costs. */
void star_light(const FwmStar *star, const StarConfig *cfg,
                double wx, double wy, FwmSunLight *out);

/* What this star weighs in the physics world's own units, so it can be
 * compared against a window's mass. `weight` in [star] says what one solar
 * mass is worth; the star's own mass does the rest, which means a hole that
 * has eaten well really is harder to shift. */
double star_weight(const FwmStar *star, const StarConfig *cfg);

/* Shove it. `mass` is what the shover weighs, in the same units star_weight
 * returns: LIGHTER THAN THE STAR AND NOTHING HAPPENS — a window bouncing off a
 * star does not move the star, any more than a football moves a building.
 * Returns true if it actually shifted.
 *
 * `speed` is the approach speed in px/s and (dx, dy) the direction it was
 * travelling; momentum is shared in proportion to the masses, so a heavy
 * window barely nudges a heavy star and sends a light one flying. */
bool star_push(FwmStar *star, const StarConfig *cfg, double mass,
               double dx, double dy, double speed);

/* Set it spinning, or add to the spin it has. Same idea as spin_window: a
 * shove off-centre turns as well as pushes. */
void star_spin(FwmStar *star, double angvel);

/* Carry it by hand: the pointer has it, and it goes exactly where the pointer
 * goes. star_release hands it back to the physics with whatever speed the hand
 * was moving at, which is what makes it a throw rather than a placement. */
void star_grab(FwmStar *star);
void star_release(FwmStar *star, double vx, double vy);

/* Advance its motion and keep it inside its desktop, bouncing off the edges.
 * Called with the box the desktop occupies in world coordinates. */
void star_move(FwmStar *star, const StarConfig *cfg, double dt,
               double min_x, double max_x, double min_y, double max_y);

/* ── two of them ──────────────────────────────────────────────────────
 *
 * Everything below is about a PAIR, and all of it is ordinary two-body
 * mechanics: nothing here knows what a desktop is.
 *
 * Left alone, two stars near each other do what two stars do — fall towards
 * each other, miss, and end up circling. That is not scripted anywhere; it
 * falls out of star_attract plus the velocity they already carry, which is why
 * throwing one past another is worth doing. */

/* Pull each towards the other for `dt` seconds. Inverse square, softened at
 * short range so a near-miss cannot divide by nothing and fling them to the
 * far side of the world. Neither is moved if either is held by hand. */
void star_attract(FwmStar *a, FwmStar *b, const StarConfig *cfg, double dt);

/* Mass transfer: the vampirism of a close binary.
 *
 * A compact object beside an ordinary star strips gas off it — the companion
 * swells until it fills its Roche lobe and the surface nearest the hole is no
 * longer bound to it, so it streams across. The star loses mass and dims; the
 * thief gains it, and the stream is what feeds an accretion disc. Real, common,
 * and the reason we can see most stellar-mass black holes at all.
 *
 * Returns the fraction of the way the stream is "on", 0..1, so the drawing can
 * show it: 0 when nothing is happening, 1 when it is pouring. */
double star_siphon(FwmStar *thief, FwmStar *victim, const StarConfig *cfg, double dt);

/* Are these two touching? Compact objects merge when they meet, and what comes
 * out is decided by the mass the way it always is. */
bool star_touching(const FwmStar *a, const FwmStar *b, const StarConfig *cfg);

/* Merge `b` into `a`. `a` keeps their combined mass and their combined
 * momentum; `b` is left dead (STAR_HOLE with no mass) for the caller to drop.
 *
 * A few percent of the mass does not arrive: in a real merger it leaves as
 * gravitational waves, which is what LIGO hears. The remnant is then whatever
 * that mass implies — two neutron stars can add up to a black hole, and in the
 * sky that is a kilonova, the event that makes most of the gold. */
void star_merge(FwmStar *a, FwmStar *b, const StarConfig *cfg);

/* Where the pulsar's beam appears to point, and how strongly it is aimed at
 * you. Both come out of one fact: the magnetic axis is not the rotation axis.
 *
 * A pulsar spins with a regularity that embarrasses atomic clocks, but its
 * beam is tied to the MAGNETIC pole, which sits at an angle to the spin axis
 * and therefore sweeps out a cone. Projected on the sky that cone is an
 * ellipse, so the beam appears to hurry through the near side of its circuit
 * and dawdle through the far side — uneven motion from perfectly even
 * rotation. It also points more nearly at you at one part of the turn than
 * another, which is what makes a pulse a pulse rather than a spotlight.
 *
 * `bearing_deg` is where it looks like it points, `aim` is 0..1 for how close
 * to head-on it is. */
void star_pulse(const FwmStar *star, const StarConfig *cfg,
                double *bearing_deg, double *aim);

/* Ignition: 0 while the star is still gathering, 1 once it is a star.
 *
 * A star does not arrive, it CONDENSES: a cloud of gas falls together under
 * its own weight, spins up, heats, and lights when the core finally reaches
 * fusion. That is a few million years in the sky and a couple of seconds here,
 * but it is the same shape of event — and it is why a star appearing out of
 * nothing looks wrong even to somebody who has never thought about it.
 *
 * Only ever runs once, at the beginning of a star's life. */
double star_ignition(const FwmStar *star, const StarConfig *cfg);

/* The supernova: how far along the blast is, 0 before it and 1 when the shell
 * has faded. Non-zero only around the end of a collapse and for a few seconds
 * after it.
 *
 * A core collapse does not simply end — the infall rebounds off the new
 * remnant and blows the star's outer layers off at a good fraction of the
 * speed of light. That flash is briefly brighter than the galaxy holding it,
 * and it is the part everybody has actually seen pictures of. Without it a
 * collapse is a star quietly shrinking until it is not there, which is what
 * this looked like. */
double star_blast(const FwmStar *star, const StarConfig *cfg);

/* Name of the phase, for the tray and the log. Never NULL. */
const char *star_phase_name(FwmStarPhase phase);

#endif /* FWM_STAR_H */
