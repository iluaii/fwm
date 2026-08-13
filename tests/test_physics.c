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

/* The ends of the world, and what happens at them.
 *
 * A window thrown off the edge is the one interaction nobody can check by
 * hand in a nested session — throwing needs a mouse, and a nested run cannot
 * move the pointer. physics.c reaches Box2D but not wlroots, so the throw can
 * be asserted here instead, which is also the only reason this file exists:
 * the ring shipped once without it and "windows cannot move between 1 and 10"
 * was the report that came back. */

#include "test.h"
#include "physics.h"

#define SW  1920
#define SH  1080
#define SPAN (10 * SW)
#define DT  (1.0 / 60.0)

static PhysicsBody *spawn(PhysicsWorld *w, int x, int y) {
    return physics_sync_body(w, 1, x, y, 400, 300, SW);
}

static void run(PhysicsWorld *w, int frames) {
    for (int i = 0; i < frames; i++)
        physics_step(w, SW, SH, 0, 0, 0, DT);
}

/* Thrown at the left end of a STRIP: the wall is there, so it stays inside. */
static void test_wall_holds_a_line(void) {
    CASE("line");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.wrap = 0;

    PhysicsBody *b = spawn(&w, 60, 400);
    CHECK_NOT_NULL(b);
    physics_throw_body(&w, 1, -4000.0, 0.0);
    run(&w, 60);

    b = physics_find_body(&w, 1);
    CHECK_NOT_NULL(b);
    CHECK(b->x >= -1.0);                 /* inside the world */
    CHECK_INT(b->desktop_id, 0);
    physics_destroy(&w);
}

/* The same throw on a RING arrives at the far end, still moving. */
static void test_ring_carries_a_throw_round(void) {
    CASE("ring, leftward");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.wrap = 1;

    PhysicsBody *b = spawn(&w, 60, 400);
    CHECK_NOT_NULL(b);
    physics_throw_body(&w, 1, -4000.0, 0.0);
    run(&w, 60);

    b = physics_find_body(&w, 1);
    CHECK_NOT_NULL(b);
    CHECK(b->x > SPAN - 2 * SW);          /* came out at the far end */
    CHECK_INT(b->desktop_id, 9);
    CHECK(b->vx < -100.0);                /* and is still flying */
    physics_destroy(&w);
}

static void test_ring_carries_a_throw_the_other_way(void) {
    CASE("ring, rightward");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.wrap = 1;

    PhysicsBody *b = spawn(&w, SPAN - 460, 400);
    CHECK_NOT_NULL(b);
    physics_throw_body(&w, 1, 4000.0, 0.0);
    run(&w, 60);

    b = physics_find_body(&w, 1);
    CHECK_NOT_NULL(b);
    CHECK(b->x < 2 * SW);
    CHECK_INT(b->desktop_id, 0);
    CHECK(b->vx > 100.0);
    physics_destroy(&w);
}

/* The crossing must not be visible as a jump WITHIN the world: a window is
 * only carried once it has left entirely, so it is never half at each end. */
static void test_ring_crosses_only_when_clear(void) {
    CASE("ring, straddling");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.wrap = 1;

    PhysicsBody *b = spawn(&w, -100, 400);   /* half off the left end */
    CHECK_NOT_NULL(b);
    physics_set_velocity(&w, 1, 0.0, 0.0);
    run(&w, 2);

    b = physics_find_body(&w, 1);
    CHECK_NOT_NULL(b);
    CHECK(b->x < SW);                        /* still at this end, not teleported */
    physics_destroy(&w);
}

/* mass_scale is how [physics] mass = "ram" reaches the simulation: the
 * compositor writes a multiplier per body and nothing else about the world
 * changes. What has to be true is that the multiplier is felt in a collision —
 * a two-gigabyte browser must shove a terminal harder than a terminal shoves
 * back — and that is not visible anywhere except here. */
static double shove_speed(double scale) {
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.wrap = 0;

    PhysicsBody *heavy = physics_sync_body(&w, 1, 400, 400, 400, 300, SW);
    PhysicsBody *light = physics_sync_body(&w, 2, 900, 400, 400, 300, SW);
    heavy->mass_scale = scale;
    light->mass_scale = 1.0;

    /* Same size, same everything else: the only asymmetry is the multiplier. */
    physics_throw_body(&w, 1, 3000.0, 0.0);
    run(&w, 30);

    light = physics_find_body(&w, 2);
    double vx = light ? light->vx : 0.0;
    physics_destroy(&w);
    return vx;
}

static void test_mass_scale_is_felt(void) {
    CASE("mass_scale");
    double even  = shove_speed(1.0);
    double heavy = shove_speed(10.0);

    CHECK(even > 0.0);            /* it was hit, so it is moving away */
    CHECK(heavy > even * 1.2);    /* ... and much faster when hit by a hog */

    /* A body nobody has an opinion about weighs what it always did, so a scale
     * of 1 must be indistinguishable from the field not being there. */
    CHECK_DBL(shove_speed(1.0), even, 1e-9);
}

/* ── hit points ──────────────────────────────────────────────────────── */

/* Toughness scales the mass, and a rule that says nothing must leave it alone —
 * the same tri-state every other material property uses, and the one that is
 * easy to get wrong because NAN compares false against everything. */
static void test_hp_follows_mass_and_toughness(void) {
    CASE("hp_toughness");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;

    PhysicsBody *b = spawn(&w, 400, 400);
    run(&w, 1);                       /* physics_step is what writes ->mass */
    double plain = physics_body_hp(b);
    CHECK(plain > 0.0);
    CHECK_DBL(plain, b->mass, 1e-9);  /* no rule: hit points ARE the mass */

    b->rule_toughness = 3.0;
    CHECK_DBL(physics_body_hp(b), b->mass * 3.0, 1e-9);

    /* Glass: destroyed by anything at all, and 0 has to survive the round trip
     * because it is a value a rule may legitimately set. */
    b->rule_toughness = 0.0;
    CHECK_DBL(physics_body_hp(b), 0.0, 1e-9);

    b->rule_toughness = NAN;
    CHECK_DBL(physics_body_hp(b), plain, 1e-9);
    CHECK_DBL(physics_body_hardness(b), 1.0, 1e-9);

    physics_destroy(&w);
}

/* The freeze exists so that mass = "ram" cannot quietly move a window's hit
 * points while it is resampled. Pinning has to survive a change of mass, and
 * releasing has to hand them back. */
static void test_hp_freeze_pins_and_releases(void) {
    CASE("hp_freeze");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;

    PhysicsBody *b = spawn(&w, 400, 400);
    run(&w, 1);
    double before = b->mass;

    physics_freeze_hp(&w, 1);
    b->mass_scale = 8.0;              /* the window "grew" to eight times */
    run(&w, 1);
    CHECK(b->mass > before * 4.0);    /* ... its mass really did move */
    CHECK_DBL(physics_body_hp(b), before, 1e-9);   /* ... and its hp did not */

    /* Freezing again must not re-pin something already pinned: that is what
     * lets the tick call it on every sample so late windows get a value. */
    physics_freeze_hp(&w, 1);
    CHECK_DBL(physics_body_hp(b), before, 1e-9);

    physics_freeze_hp(&w, 0);
    CHECK_DBL(physics_body_hp(b), b->mass, 1e-9);

    physics_destroy(&w);
}

/* A bar that reserved space is a floor: a window dropped onto it comes to rest
 * ON TOP, not in the strip the bar occupies. The height is a plain 300, so the
 * bottom of a settled window is its y plus that. */
static void test_solid_bars_raise_the_floor(void) {
    CASE("solid bars");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 1.0;
    w.inset_bottom = 64;              /* a dock along the bottom of the screen */

    PhysicsBody *b = spawn(&w, 400, 100);
    CHECK_NOT_NULL(b);
    run(&w, 240);                     /* four seconds: long enough to settle */

    b = physics_find_body(&w, 1);
    CHECK_NOT_NULL(b);
    /* Resting on the dock, not standing in it and not through the screen. */
    CHECK(b->y + 300 <= SH - 64 + 2.0);
    CHECK(b->y + 300 >= SH - 64 - 8.0);
    physics_destroy(&w);
}

/* A desktop shown on a monitor shorter than the strip gets a floor at the
 * bottom of THAT monitor, and its neighbours keep theirs. This is #20: with a
 * 1080p screen beside a 1440p one the strip is 1440 tall, and a window on the
 * small screen used to settle 360px below the bottom of the glass. */
static void test_short_monitor_raises_the_floor(void) {
    CASE("per-desktop floor");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 1.0;
    w.desktop_h[0] = 800;             /* a short monitor on desktop 0 */
                                      /* and nothing said about desktop 1 */

    PhysicsBody *a = physics_sync_body(&w, 1, 400, 100, 400, 300, SW);
    PhysicsBody *b = physics_sync_body(&w, 2, SW + 400, 100, 400, 300, SW);
    CHECK_NOT_NULL(a);
    CHECK_NOT_NULL(b);
    run(&w, 240);                     /* four seconds: long enough to settle */

    a = physics_find_body(&w, 1);
    b = physics_find_body(&w, 2);
    CHECK_NOT_NULL(a);
    CHECK_NOT_NULL(b);
    /* On its own monitor's bottom, not the strip's. */
    CHECK_DBL(a->y + 300, 800.0, 8.0);
    /* The desktop nobody described still gets the screen. */
    CHECK_DBL(b->y + 300, (double)SH, 8.0);
    physics_destroy(&w);
}

/* The floor a window is standing over is also the one the escape net uses:
 * shoved below a short monitor, it is caught at that monitor's bottom rather
 * than at the strip's. */
static void test_escape_net_follows_the_short_floor(void) {
    CASE("per-desktop escape net");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.desktop_h[0] = 800;

    PhysicsBody *b = physics_sync_body(&w, 1, 400, 100, 400, 300, SW);
    CHECK_NOT_NULL(b);
    physics_throw_body(&w, 1, 0.0, 6000.0);   /* straight down, hard */
    run(&w, 120);

    b = physics_find_body(&w, 1);
    CHECK_NOT_NULL(b);
    CHECK(b->y + 300 <= 800.0 + 8.0);         /* never got below the glass */
    physics_destroy(&w);
}

/* And a bar along the TOP is a ceiling: a window thrown up bounces off the
 * underside of it rather than passing behind it. */
static void test_solid_bars_lower_the_ceiling(void) {
    CASE("solid bars, ceiling");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.inset_top = 48;

    PhysicsBody *b = spawn(&w, 400, 600);
    CHECK_NOT_NULL(b);
    physics_throw_body(&w, 1, 0.0, -4000.0);
    run(&w, 60);

    b = physics_find_body(&w, 1);
    CHECK_NOT_NULL(b);
    CHECK(b->y >= 48 - 2.0);          /* never got above the bar */
    physics_destroy(&w);
}

int main(void) {
    test_wall_holds_a_line();
    test_solid_bars_raise_the_floor();
    test_solid_bars_lower_the_ceiling();
    test_short_monitor_raises_the_floor();
    test_escape_net_follows_the_short_floor();
    test_ring_carries_a_throw_round();
    test_ring_carries_a_throw_the_other_way();
    test_ring_crosses_only_when_clear();
    test_mass_scale_is_felt();
    test_hp_follows_mass_and_toughness();
    test_hp_freeze_pins_and_releases();
    return t_report("physics");
}
