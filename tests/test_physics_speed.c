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

/* What the physics has to keep doing when things move fast.
 *
 * All of it came out of one outside report — "the physics breaks at high
 * speeds" — and every case here is a measurement that used to fail. None of it
 * is checkable by hand: a drag needs a mouse, a nested run cannot inject one,
 * and the failure is three frames long. See docs/physics.md.
 */

#include "test.h"
#include "physics.h"
#include "defines.h"

#include <math.h>

#define SW 1920
#define SH 1080
#define WORLD PHYSICS_WORLD_W(SW)
#define DT   (1.0 / 60.0)

/* Depth of the intersection of two windows, in px; 0 when they are apart. */
static double overlap_px(const PhysicsBody *a, const PhysicsBody *b) {
    double ox = fmin(a->x + a->width,  b->x + b->width)  - fmax(a->x, b->x);
    double oy = fmin(a->y + a->height, b->y + b->height) - fmax(a->y, b->y);
    if (ox <= 0.0 || oy <= 0.0) return 0.0;
    return fmin(ox, oy);
}

/* Drag window 1 rightward at `speed` into window 2 and report the worst overlap
 * reached, the way server_drag does it: write a position every frame and let
 * physics_step derive the push from it. */
static double drag_into_neighbour(double speed, double *victim_speed) {
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;

    physics_sync_body(&w, 1, 400, 400, 400, 300, SW);
    physics_sync_body(&w, 2, 1400, 400, 400, 300, SW);

    double x = 400.0, worst = 0.0, fastest = 0.0;
    for (int i = 0; i < 400; i++) {
        x += speed * DT;
        physics_sync_body(&w, 1, (int)x, 400, 400, 300, SW);
        physics_step(&w, SW, SH, 0, 0, 1 /* dragged */, DT);
        PhysicsBody *v = physics_find_body(&w, 2);
        /* Stop once the victim runs out of room. Past that it is a window held
         * against the far wall by the hand, and the overlap that produces is
         * physics working, not physics breaking. */
        if (v->x + v->width > WORLD - 1500.0) break;
        double ov = overlap_px(physics_find_body(&w, 1), v);
        if (ov > worst) worst = ov;
        double vs = hypot(v->vx, v->vy);
        if (vs > fastest) fastest = vs;
    }
    if (victim_speed) *victim_speed = fastest;
    physics_destroy(&w);
    return worst;
}

static void test_drag_does_not_pass_through(void) {
    /* The bug this file exists for. A dragged body is kinematic: its transform is
     * teleported to the cursor while its velocity used to be clamped to 600 px/s,
     * so above that the solver was resolving a contact far deeper than it
     * believed. Measured before the fix: the overlap grew to 295px — three
     * quarters of the window — and the dragged window came out the far side.
     *
     * 3000 px/s is a brisk but ordinary mouse movement; the old ceiling failed at
     * a fifth of it. */
    CASE("a drag shoves, it does not pass through");
    const double speeds[] = { 600.0, 1200.0, 2000.0, 3000.0 };
    for (unsigned i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
        double victim = 0.0;
        double worst = drag_into_neighbour(speeds[i], &victim);
        /* A few px of speculative contact are normal and invisible; a quarter of
         * a window is the failure. */
        CHECK(worst < 20.0);
        /* And the shove has to actually move the other window — the old clamp
         * made the push WEAKER the faster you dragged (591 px/s at a 600 px/s
         * drag, 98 px/s at 12000). */
        CHECK(victim > speeds[i] * 0.4);
    }
}

static void test_drag_survives_a_flick(void) {
    /* Issue #7: the honest velocity above bought collision back only as far as
     * about 3000 px/s. Beyond that a drag is still a teleport, and one that lands
     * 100px inside the neighbour gives the solver a contact already too deep to
     * undo — the overlap ran away to 269px through a 300px window and the hand
     * walked out the far side. 6000 px/s is a flick across one screen in a third
     * of a second, which is nothing unusual.
     *
     * Two things were wrong and both had to be fixed:
     *   - the tick was one solve however far the hand moved (PHYSICS_MAX_STEP_ADVANCE)
     *   - Box2D's own speed ceiling held the shoved window to 2 * max_throw_speed,
     *     so above 3600 px/s the victim was forbidden to get out of the way
     *     however finely the tick was cut (physics_step sets it from the drag).
     *
     * Measured before: 249 / 269 / 269 / 269 / 76 px of overlap, every one of them
     * with the dragged window ending up on the far side of its neighbour. */
    CASE("a flick shoves too, it does not pass through");
    const double speeds[] = { 6000.0, 9000.0, 12000.0, 20000.0 };
    for (unsigned i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
        double victim = 0.0;
        double worst = drag_into_neighbour(speeds[i], &victim);
        /* Scales with how far the hand moves in one substep rather than in one
         * tick, so it stays a sliver instead of swallowing the window. */
        CHECK(worst < 30.0);
        /* And the victim keeps up with the hand: it is what stops the overlap
         * growing in the first place. */
        CHECK(victim > speeds[i] * 0.8);
    }

    /* Past the substep ceiling the pieces grow again and the overlap with them,
     * so this is a promise about degrading, not about being exact: 40000 px/s is
     * a hand no display can even show. It must still be a shove and not a
     * pass-through, and the victim must still be carried along at the fastest
     * speed one tick of substepping can actually resolve — which is also the
     * ceiling the exemption is capped at, so a mid-drag teleport cannot make
     * itself the speed limit for everything it sweeps past. */
    CASE("and past the substep ceiling it degrades rather than breaks");
    double budget = PHYSICS_MAX_SUBSTEPS * PHYSICS_MAX_STEP_ADVANCE * PHYSICS_TICK_RATE;
    double victim = 0.0;
    double worst = drag_into_neighbour(40000.0, &victim);
    CHECK(worst < 150.0);              /* half a window, not the whole of it */
    CHECK(victim > budget * 0.95);
    CHECK(victim <= budget + 1.0);
}

static void test_ordinary_drag_lands_on_the_cursor(void) {
    /* The dragged body no longer teleports to the cursor: it starts where it was
     * and is carried the rest of the way by its own velocity, which is what lets
     * a substep move it part of the way. Over a whole tick that has to come out
     * in exactly the same place, or every drag in the compositor now lags the
     * mouse by a frame. */
    CASE("a drag still ends the tick exactly where the mouse put it");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 1.0;
    physics_sync_body(&w, 1, 400, 400, 400, 300, SW);

    double x = 400.0, y = 300.0;
    for (int i = 0; i < 60; i++) {
        x += 900.0 * DT;
        y += 300.0 * DT;
        physics_sync_body(&w, 1, (int)x, (int)y, 400, 300, SW);
        physics_step(&w, SW, SH, 0, 0, 1 /* dragged */, DT);
        PhysicsBody *b = physics_find_body(&w, 1);
        CHECK(fabs(b->x - (int)x) < 0.001);
        CHECK(fabs(b->y - (int)y) < 0.001);
    }
    physics_destroy(&w);
}

static void test_a_teleport_is_not_a_drag(void) {
    /* The dragged body is swept rather than teleported now, and that is right for
     * a hand and wrong for everything else that can move a held window a long way
     * in one tick. Crossing the ring's join mid-drag puts the camera — and the
     * drag's anchor with it — nine screens over in a single tick; a swept body
     * would plough through every window on all ten desktops on the way.
     *
     * A row of windows standing between here and there must be exactly where it
     * was, and still asleep. */
    CASE("a window teleported mid-drag ploughs no furrow on the way out");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;

    physics_sync_body(&w, 1, 200, 400, 400, 300, SW);
    /* A dense row all the way along, so a sweep cannot slip between them. */
    int n = 0;
    for (int d = 0; d <= 9; d++)
        for (int k = 0; k < 5; k++)
            physics_sync_body(&w, 100 + n++, d * SW + 700 + k * 380, 400, 360, 300, SW);

    /* Settle, so "did not move" means something. */
    for (int i = 0; i < 60; i++) physics_step(&w, SW, SH, 0, 0, 1, DT);

    double was[50];
    for (int i = 0; i < n; i++) was[i] = physics_find_body(&w, 100 + i)->x;

    /* The join: nine screens in one tick, exactly as server_goto_desktop's seam
     * branch moves the camera and server_drag_follow_camera moves the anchor. */
    physics_sync_body(&w, 1, 9 * SW + 200, 400, 400, 300, SW);
    physics_step(&w, SW, SH, 0, 0, 1 /* dragged */, DT);

    for (int i = 0; i < n; i++) {
        PhysicsBody *b = physics_find_body(&w, 100 + i);
        /* Nudged apart where it LANDED is fine and is what should happen; swept
         * is not. Measured when the teleport was treated as travel: one window
         * thrown 837px in the single tick, at 30720 px/s, and 32 impacts —
         * a furrow through the desktop being left, and a sound for every window
         * in it. */
        CHECK(fabs(b->x - was[i]) < 60.0);
        /* Above all: a teleport hands out no momentum. Nothing may come out of
         * this tick faster than the world's ordinary limit, because nothing in
         * it was actually shoved by a hand. */
        CHECK(hypot(b->vx, b->vy) <= w.max_throw_speed + 1.0);
    }
    /* And it arrived. */
    CHECK(fabs(physics_find_body(&w, 1)->x - (9.0 * SW + 200)) < 1.0);
    physics_destroy(&w);
}

static void test_world_speed_ceiling(void) {
    /* One limit for everything: no dynamic body outruns the hardest throw the
     * config allows. It is what lets the drag hand Box2D an honest velocity
     * without a fast shove launching its neighbour across the strip. */
    CASE("nothing dynamic exceeds max_throw_speed");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.throw_speed_multiplier = 1.0;
    w.max_throw_speed = 1200.0;

    physics_sync_body(&w, 1, 900, 400, 400, 300, SW);
    /* physics_throw_body clamps on the way in, so ask through the back door the
     * cava row and the drag use. */
    physics_set_velocity(&w, 1, 30000.0, 12000.0);
    for (int i = 0; i < 10; i++) {
        physics_step(&w, SW, SH, 0, 0, 0, DT);
        PhysicsBody *b = physics_find_body(&w, 1);
        CHECK(hypot(b->vx, b->vy) <= 1200.0 + 1.0);
    }
    physics_destroy(&w);

    /* Zero means "no limit" — the escape hatch for anyone who wants chaos. */
    CASE("max_throw_speed = 0 lifts the ceiling");
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.max_throw_speed = 0.0;
    physics_sync_body(&w, 1, 900, 400, 400, 300, SW);
    physics_set_velocity(&w, 1, 30000.0, 0.0);
    physics_step(&w, SW, SH, 0, 0, 0, DT);
    CHECK(hypot(physics_find_body(&w, 1)->vx, physics_find_body(&w, 1)->vy) > 20000.0);
    physics_destroy(&w);

    /* And it has to mean that on the way in as well. The case above goes through
     * physics_set_velocity; a thrown window goes through physics_throw_body,
     * which clamps before the body ever reaches the solver — and read zero as a
     * ceiling of zero, so chaos mode was the one setting that made a throw land
     * dead while a shove still flew. */
    CASE("max_throw_speed = 0 does not kill a throw");
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.throw_speed_multiplier = 1.0;
    w.max_throw_speed = 0.0;
    physics_sync_body(&w, 1, 900, 400, 400, 300, SW);
    physics_throw_body(&w, 1, 3000.0, 0.0);
    CHECK(hypot(physics_find_body(&w, 1)->vx, physics_find_body(&w, 1)->vy) > 2999.0);
    physics_destroy(&w);
}

static void test_engine_ceiling_follows_config(void) {
    /* Box2D enforces a speed limit of its own — 400 m/s, which is 40000 px/s at
     * this scale — silently. A config asking for a faster throw than that used to
     * get 39550 px/s and no explanation. */
    CASE("a raised max_throw_speed is not quietly clipped");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.throw_speed_multiplier = 1.0;
    w.max_throw_speed = 100000.0;

    physics_sync_body(&w, 1, 900, 400, 400, 300, SW);
    physics_throw_body(&w, 1, 60000.0, 0.0);
    physics_step(&w, SW, SH, 0, 0, 0, DT);
    PhysicsBody *b = physics_find_body(&w, 1);
    /* Damping takes a per-step bite; the old failure was a third of the speed
     * disappearing, not two percent. */
    CHECK(hypot(b->vx, b->vy) > 55000.0);
    physics_destroy(&w);
}

static void test_walls_hold_absurd_speeds(void) {
    CASE("the play area contains a throw nothing should survive");
    const double speeds[] = { 12000.0, 48000.0, 96000.0 };
    for (unsigned i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
        PhysicsWorld w;
        physics_init(&w);
        w.gravity_scale = 1.0;
        w.throw_speed_multiplier = 1.0;
        w.max_throw_speed = 1.0e9;      /* no ceiling: test the WALLS */

        physics_sync_body(&w, 1, 900, 400, 400, 300, SW);
        physics_throw_body(&w, 1, speeds[i], speeds[i] / 2.0);

        for (int s = 0; s < 600; s++) {
            physics_step(&w, SW, SH, 0, 0, 0, DT);
            PhysicsBody *b = physics_find_body(&w, 1);
            CHECK(!isnan(b->x) && !isnan(b->y));
            /* Inside, or at worst a wall's thickness into one — never out. */
            CHECK(b->x + b->width > -80.0);
            CHECK(b->x < WORLD + 80.0);
            CHECK(b->y + b->height > -80.0);
            CHECK(b->y < SH + 80.0);
        }
        physics_destroy(&w);
    }
}

static void test_init_gives_a_closed_world(void) {
    /* physics_init used to leave `wrap` alone, and `wrap` decides whether the
     * strip HAS end walls. A caller with a stack-allocated world got whatever was
     * on the stack, and one value of that quietly removes the ends — windows sail
     * off the left of desktop 1 and reappear at desktop 10. The compositor never
     * saw it because its server struct is zeroed. */
    CASE("a freshly initialised world has ends");
    PhysicsWorld w;
    physics_init(&w);
    CHECK_INT(w.wrap, 0);
    CHECK_INT(w.impact_count, 0);
    w.gravity_scale = 0.0;
    w.throw_speed_multiplier = 1.0;
    w.max_throw_speed = 1.0e9;

    PhysicsBody *b = physics_sync_body(&w, 1, 200, 400, 400, 300, SW);
    physics_throw_body(&w, 1, -20000.0, 0.0);   /* straight at the left end */

    /* What goes wrong here is a TELEPORT, not a long slide: a body that left
     * the left end of a world with no ring came back at the right in a single
     * step. So the assertion is continuity — no step carries the window
     * further than its own speed could — and that says the same thing at any
     * world width. "It stayed in the left half" only said it while the world
     * was ten screens wide and the throw happened to run out before the
     * middle; in a four-screen world the same bounce crosses the middle
     * honestly and the test called it a wrap. */
    double prev_x = b->x, prev_vx = b->vx;
    for (int i = 0; i < 240; i++) {
        physics_step(&w, SW, SH, 0, 0, 0, DT);
        b = physics_find_body(&w, 1);
        double reach = fmax(fabs(prev_vx), fabs(b->vx)) * DT + 80.0;
        CHECK(fabs(b->x - prev_x) < reach);
        prev_x = b->x;
        prev_vx = b->vx;
    }

    CHECK(b->x > -80.0);                 /* bounced, not gone */
    CHECK(b->x < WORLD);                 /* and still in the world it bounced in */
    physics_destroy(&w);
}

int main(void) {
    test_drag_does_not_pass_through();
    test_drag_survives_a_flick();
    test_ordinary_drag_lands_on_the_cursor();
    test_a_teleport_is_not_a_drag();
    test_world_speed_ceiling();
    test_engine_ceiling_follows_config();
    test_walls_hold_absurd_speeds();
    test_init_gives_a_closed_world();
    return t_report("physics_speed");
}
