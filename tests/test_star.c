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

/* A star's whole life, walked in a loop.
 *
 * The alternative is sitting in front of the compositor for the four hours of
 * fuel it ships with, three times over, to see each of the three endings — and
 * the endings are the entire point of it. star.c reaches no further than
 * config.h's types, so the hours are spent here instead. */

#include <math.h>

#include "test.h"
#include "star.h"

static StarConfig base(void) {
    StarConfig c = {0};
    c.enabled        = 1;
    c.desktop        = 0;
    c.x              = 400.0;
    c.y              = 200.0;
    c.height         = 600.0;
    c.radius         = 90.0;
    c.mass           = 1.10;
    c.mass_max       = 6.0;
    c.mass_per_throw = 0.06;
    c.throw_speed    = 1400.0;
    c.lifetime_s     = 100.0;
    c.collapse_s     = 6.0;
    c.pulsar_hz      = 0.7;
    c.pull           = 260.0;
    c.weight         = 480000.0;
    c.length         = 18.0;
    c.length_max     = 90.0;
    c.opacity        = 0.5;
    return c;
}

/* Run until the star stops being on the main sequence and its collapse is
 * over, so the phase left behind is the remnant. */
static FwmStarPhase live_out(const StarConfig *cfg, double mass) {
    StarConfig c = *cfg;
    c.mass = mass;
    FwmStar s;
    star_init(&s, &c);
    for (int i = 0; i < 200000 && s.phase <= STAR_COLLAPSE; i++)
        star_tick(&s, &c, 1.0 / 60.0);
    return s.phase;
}

static void test_mass_decides_the_ending(void) {
    /* The two limits, and that they are limits: a hair either side of each
     * number is a different star for the rest of the session. */
    CASE("the mass at collapse decides the remnant");
    StarConfig c = base();

    CHECK_INT(live_out(&c, 0.90), STAR_DWARF);
    CHECK_INT(live_out(&c, 1.43), STAR_DWARF);
    CHECK_INT(live_out(&c, 1.45), STAR_NEUTRON);
    CHECK_INT(live_out(&c, 2.49), STAR_NEUTRON);
    CHECK_INT(live_out(&c, 2.51), STAR_HOLE);
    CHECK_INT(live_out(&c, 9.00), STAR_HOLE);
}

static void test_fuel_and_the_fuse(void) {
    CASE("fuel runs out on its own, and the bind does not wait for it");
    StarConfig c = base();
    FwmStar s;

    /* Burning is the whole of the lifetime and not a second more. */
    star_init(&s, &c);
    star_tick(&s, &c, c.lifetime_s * 0.5);
    CHECK_INT(s.phase, STAR_BURNING);
    CHECK(s.fuel > 0.49 && s.fuel < 0.51);
    star_tick(&s, &c, c.lifetime_s * 0.6);
    CHECK_INT(s.phase, STAR_COLLAPSE);

    /* And the fuse skips straight to it with the tank full. */
    star_init(&s, &c);
    star_collapse_now(&s);
    CHECK_INT(s.phase, STAR_COLLAPSE);
    CHECK(s.fuel == 0.0);

    /* Firing it twice must not restart a collapse already under way. */
    star_tick(&s, &c, 3.0);
    star_collapse_now(&s);
    CHECK(s.phase_s > 2.9);
}

static void test_pushing_a_remnant_over(void) {
    /* Each press is one collapse, and the remnant it lands on is one step
     * further down: ember, pulsar, hole. A hole has nowhere left to go. */
    CASE("a remnant can be pushed over its own limit, one step per press");
    StarConfig c = base();
    FwmStar s;
    star_init(&s, &c);           /* 1.10 solar masses: an ember on its own */

    CHECK(star_collapse_now(&s));
    for (int i = 0; i < 2000 && s.phase == STAR_COLLAPSE; i++) star_tick(&s, &c, 1.0/60.0);
    CHECK_INT(s.phase, STAR_DWARF);

    /* The second collapse starts from the size it IS, not the size it was:
     * a dwarf pushed over its limit must not snap back to a full-sized star
     * before falling again. */
    double was = star_radius(&s, &c);
    CHECK(star_collapse_now(&s));
    CHECK(s.mass >= 1.44);
    CHECK(star_radius(&s, &c) <= was + 0.001);
    for (int i = 0; i < 2000 && s.phase == STAR_COLLAPSE; i++) {
        CHECK(star_radius(&s, &c) <= was + 0.001);
        star_tick(&s, &c, 1.0/60.0);
    }
    CHECK_INT(s.phase, STAR_NEUTRON);

    CHECK(star_collapse_now(&s));
    CHECK(s.mass >= 2.50);
    for (int i = 0; i < 2000 && s.phase == STAR_COLLAPSE; i++) star_tick(&s, &c, 1.0/60.0);
    CHECK_INT(s.phase, STAR_HOLE);

    /* Brightness has to be continuous across the moment it starts to fall.
     * Left to derive itself from the mass, a pulsar's collapse opened at the
     * brightness of a whole star — a flash out of nowhere. */
    star_init(&s, &c);
    star_collapse_now(&s);
    for (int i = 0; i < 2000 && s.phase == STAR_COLLAPSE; i++) star_tick(&s, &c, 1.0/60.0);
    CHECK_INT(s.phase, STAR_DWARF);
    double before = star_luminosity(&s, &c);
    star_collapse_now(&s);
    double after = star_luminosity(&s, &c);
    CHECK(after >= before * 0.95 && after <= before * 1.05);

    /* And it still runs away from there — it just starts where it was. */
    for (int i = 0; i < 30; i++) star_tick(&s, &c, 1.0/60.0);
    CHECK(star_luminosity(&s, &c) > before);

    for (int i = 0; i < 2000 && s.phase == STAR_COLLAPSE; i++) star_tick(&s, &c, 1.0/60.0);
    CHECK_INT(s.phase, STAR_NEUTRON);
    before = star_luminosity(&s, &c);
    star_collapse_now(&s);
    after = star_luminosity(&s, &c);
    CHECK(after >= before * 0.95 && after <= before * 1.05);
    for (int i = 0; i < 2000 && s.phase == STAR_COLLAPSE; i++) star_tick(&s, &c, 1.0/60.0);
    CHECK_INT(s.phase, STAR_HOLE);

    /* And that is the end of it. */
    CHECK(!star_collapse_now(&s));
    CHECK_INT(s.phase, STAR_HOLE);
}

static void test_feeding(void) {
    CASE("thrown windows are the mass, and only while it burns");
    StarConfig c = base();
    FwmStar s;
    star_init(&s, &c);

    double m0 = s.mass;
    star_feed(&s, &c, c.throw_speed);
    CHECK(fabs(s.mass - (m0 + c.mass_per_throw)) < 1e-9);

    /* Twice the speed hands over twice as much ... */
    m0 = s.mass;
    star_feed(&s, &c, c.throw_speed * 2.0);
    CHECK(fabs(s.mass - (m0 + 2.0 * c.mass_per_throw)) < 1e-9);

    /* ... but one heroic throw is not a session's worth of them. */
    m0 = s.mass;
    star_feed(&s, &c, c.throw_speed * 400.0);
    CHECK(s.mass - m0 <= 4.0 * c.mass_per_throw + 1e-9);

    /* Enough throws promote the ending. */
    star_init(&s, &c);
    for (int i = 0; i < 40; i++) star_feed(&s, &c, c.throw_speed);
    CHECK(s.mass >= 2.5);
    CHECK(s.mass <= c.mass_max);

    /* Once it has collapsed, a late throw cannot rewrite what it became. */
    star_init(&s, &c);
    star_collapse_now(&s);
    double frozen = s.mass;
    star_feed(&s, &c, c.throw_speed * 3.0);
    CHECK(s.mass == frozen);
}

static void test_collapse_shape(void) {
    /* The squeeze is a fall, not a fade: it must still be most of its size
     * halfway through and then go all at once. And it must get BRIGHTER on the
     * way down, which is the part that looks wrong until you know it is right. */
    CASE("the collapse falls late and brightens on the way");
    StarConfig c = base();
    c.mass = 4.0; /* heavy enough to fall all the way, so the shape is visible */
    FwmStar s;
    star_init(&s, &c);
    star_collapse_now(&s);

    double r_start = star_radius(&s, &c);
    CHECK(fabs(r_start - c.radius) < 1e-9);
    double l_start = star_luminosity(&s, &c);

    /* Measured as the FRACTION of the fall completed, not as an absolute
     * radius: what it ends at depends on the remnant, and the shape of the
     * fall is the thing being asserted here. */
    star_tick(&s, &c, c.collapse_s * 0.5);
    double r_half = star_radius(&s, &c);
    double r_end = star_radius(&s, &c);
    for (int i = 0; i < 6000 && s.phase == STAR_COLLAPSE; i++) {
        FwmStar probe = s;
        star_tick(&probe, &c, c.collapse_s);
        r_end = star_radius(&probe, &c);
        break;
    }
    double span = r_start - r_end;
    CHECK(span > 0.0);
    double done_half = (r_start - r_half) / span;
    CHECK(done_half < 0.45);                  /* still most of itself */
    CHECK(star_luminosity(&s, &c) > l_start); /* and brighter for it */

    star_tick(&s, &c, c.collapse_s * 0.4);
    double done_late = (r_start - star_radius(&s, &c)) / span;
    CHECK(done_late > 0.70);
    /* The last tenth of the time covers more ground per second than the first
     * half does — which is what "free fall" means and what a fade would not
     * do. */
    CHECK((done_late - done_half) / 0.4 > done_half / 0.5);

    /* The light is gone before the star is: the flash is the last of it. */
    star_tick(&s, &c, c.collapse_s * 0.11);
    CHECK_INT(s.phase, STAR_HOLE);
    CHECK(star_luminosity(&s, &c) == 0.0);
}

static void test_shadows_fan_out(void) {
    /* The one thing that makes this a star and not a sun: two windows on
     * opposite sides throw their shadows in OPPOSITE directions, both away
     * from it. */
    CASE("shadows point away from the star, and lengthen with distance");
    StarConfig c = base();
    FwmStar s;
    star_init(&s, &c);
    FwmSunLight l, r, far;

    star_light(&s, &c, c.x - 300.0, c.y, &l);
    star_light(&s, &c, c.x + 300.0, c.y, &r);
    CHECK(l.dx < 0.0 && r.dx > 0.0);
    CHECK(fabs(l.dy) < 1e-9 && fabs(r.dy) < 1e-9);

    star_light(&s, &c, c.x + 900.0, c.y, &far);
    CHECK(far.dx > r.dx);              /* longer, further out */
    CHECK(far.alpha < r.alpha);        /* and dimmer: inverse square */
    CHECK(far.elevation < r.elevation);/* from a shallower angle */

    /* A window standing directly under it has no direction to throw in. */
    FwmSunLight under;
    star_light(&s, &c, c.x, c.y, &under);
    CHECK(fabs(under.dx) < 1e-9 && fabs(under.dy) < 1e-9);
    CHECK(under.alpha > 0.0);

    /* Never longer than the ceiling, however far out the window is. */
    FwmSunLight edge;
    star_light(&s, &c, c.x + 40000.0, c.y, &edge);
    CHECK(sqrt(edge.dx * edge.dx + edge.dy * edge.dy) <= c.length_max + 1e-9);
}

static void test_dead_star_costs_nothing(void) {
    CASE("a hole throws no light, and neither does a star switched off");
    StarConfig c = base();
    FwmStar s;
    FwmSunLight out;

    CHECK_INT(live_out(&c, 4.0), STAR_HOLE);
    star_init(&s, &c);
    s.phase = STAR_HOLE;
    star_light(&s, &c, c.x + 200.0, c.y, &out);
    CHECK(out.alpha == 0.0);
    CHECK(star_luminosity(&s, &c) == 0.0);

    c.enabled = 0;
    star_init(&s, &c);
    star_light(&s, &c, c.x + 200.0, c.y, &out);
    CHECK(out.alpha == 0.0);

    /* An ember still lights the room, faintly. */
    c.enabled = 1;
    star_init(&s, &c);
    s.phase = STAR_DWARF;
    star_light(&s, &c, c.x + 200.0, c.y, &out);
    CHECK(out.alpha > 0.0);
}

static void test_pulsar_sweeps(void) {
    /* A lighthouse, not a lamp: one bearing has to be bright while the one
     * behind it is not, and the bright one has to travel. */
    CASE("the pulsar's beam turns, and lights one bearing at a time");
    StarConfig c = base();
    FwmStar s;
    star_init(&s, &c);
    s.phase = STAR_NEUTRON;
    s.beam  = 0.0;

    FwmSunLight a, b;
    star_light(&s, &c, c.x, c.y - 300.0, &a); /* the way the beam points */
    star_light(&s, &c, c.x, c.y + 300.0, &b); /* and away from it */
    CHECK(a.alpha > b.alpha * 3.0);

    /* Half a turn later the two have traded places. */
    star_tick(&s, &c, 0.5 / c.pulsar_hz);
    FwmSunLight a2, b2;
    star_light(&s, &c, c.x, c.y - 300.0, &a2);
    star_light(&s, &c, c.x, c.y + 300.0, &b2);
    CHECK(b2.alpha > a2.alpha * 3.0);

    /* A whole turn is a whole turn: the beam comes back to where it started. */
    star_tick(&s, &c, 0.5 / c.pulsar_hz);
    CHECK(s.beam < 1.0 || s.beam > 359.0);
}

static void test_every_collapse_actually_shrinks(void) {
    /* Each remnant must be strictly smaller than the one it fell from.
     *
     * This has been got wrong twice, both times by enlarging the black hole to
     * make it easier to see: once the hole is bigger than the pulsar, the last
     * collapse becomes an expansion, expansion is forbidden, so the radius
     * simply holds still — a six-second collapse in which nothing whatsoever
     * moves. It looks like a missing animation and it is really an ordering
     * bug, which is exactly the kind of thing worth nailing down. */
    CASE("every collapse ends smaller than it started");
    StarConfig c = base();
    FwmStar s;
    star_init(&s, &c);

    double before = star_radius(&s, &c);
    for (int step = 0; step < 3; step++) {
        CHECK(star_collapse_now(&s));
        double at_start = star_radius(&s, &c);
        /* It may not begin bigger than the thing that is falling. */
        CHECK(at_start <= before + 0.001);

        double prev = at_start;
        int moved = 0;
        for (int i = 0; i < 6000 && s.phase == STAR_COLLAPSE; i++) {
            star_tick(&s, &c, 1.0/60.0);
            double now = star_radius(&s, &c);
            CHECK(now <= prev + 0.001);      /* never grows mid-fall */
            if (now < prev - 0.001) moved++;
            prev = now;
        }
        /* And it genuinely travelled: a collapse where the size never changes
         * is the bug this test exists for. */
        CHECK(moved > 10);
        CHECK(star_radius(&s, &c) < at_start * 0.95);
        before = star_radius(&s, &c);
    }
    CHECK_INT(s.phase, STAR_HOLE);
}

static void test_a_hole_grows(void) {
    /* The one remnant that is not the end of the story: a hole keeps taking
     * mass, and because its radius is its mass, taking mass makes it bigger. */
    CASE("a black hole eats, and gets bigger for it");
    StarConfig c = base();
    FwmStar s;
    star_init(&s, &c);
    s.mass = 4.0;
    star_collapse_now(&s);
    for (int i = 0; i < 2000 && s.phase == STAR_COLLAPSE; i++) star_tick(&s, &c, 1.0/60.0);
    CHECK_INT(s.phase, STAR_HOLE);

    double r0 = star_radius(&s, &c), m0 = s.mass;
    star_feed(&s, &c, c.throw_speed * 3.0);
    CHECK(s.mass > m0);

    /* The mass lands in one frame; the SIZE eases up to it, so a window going
     * in is a swelling rather than a jump cut. One tick must therefore have
     * moved it a little and nowhere near all the way. */
    star_tick(&s, &c, 1.0/60.0);
    double r_step = star_radius(&s, &c);
    CHECK(r_step > r0);
    double r_target = r0 * (s.mass / m0);
    CHECK(r_step < r0 + (r_target - r0) * 0.5);

    /* Given a few seconds it arrives, and it is linear in the mass — which is
     * what a Schwarzschild radius is. */
    for (int i = 0; i < 600; i++) star_tick(&s, &c, 1.0/60.0);
    double r1 = star_radius(&s, &c);
    CHECK(fabs((r1 / r0) - (s.mass / m0)) < 1e-3);

    /* It still throws no light, however fat it gets. */
    CHECK(star_luminosity(&s, &c) == 0.0);
    FwmSunLight out;
    star_light(&s, &c, c.x + 200.0, c.y, &out);
    CHECK(out.alpha == 0.0);
}

static void test_only_something_heavier_moves_it(void) {
    /* The rule in one line: a star is furniture to anything lighter than
     * itself. Throwing a terminal at one is throwing a football at a
     * building. */
    CASE("only something heavier than the star can shift it");
    StarConfig c = base();
    c.weight = 100000.0;               /* one solar mass, in window units */
    FwmStar s;
    star_init(&s, &c);                 /* 1.10 masses -> 110000 */

    double self = star_weight(&s, &c);
    CHECK(fabs(self - 110000.0) < 1.0);

    /* Lighter: nothing at all happens, not even a little. */
    CHECK(!star_push(&s, &c, self * 0.99, 1.0, 0.0, 900.0));
    CHECK(s.vx == 0.0 && s.vy == 0.0);

    /* Heavier: it moves, in the direction it was hit. */
    CHECK(star_push(&s, &c, self * 3.0, 1.0, 0.0, 900.0));
    CHECK(s.vx > 0.0);
    CHECK(fabs(s.vy) < 1e-9);

    /* And the heavier the shover, the more of the blow it hands over. */
    FwmStar a, b;
    star_init(&a, &c); star_init(&b, &c);
    star_push(&a, &c, self * 1.2, 0.0, 1.0, 900.0);
    star_push(&b, &c, self * 20.0, 0.0, 1.0, 900.0);
    CHECK(b.vy > a.vy * 1.5);

    /* A fatter star is harder to shift by the same blow — which is what makes
     * feeding a black hole tell. */
    FwmStar heavy;
    star_init(&heavy, &c);
    heavy.mass = 5.0;
    star_push(&heavy, &c, star_weight(&heavy, &c) * 1.2, 0.0, 1.0, 900.0);
    FwmStar light;
    star_init(&light, &c);
    light.mass = 1.0;
    star_push(&light, &c, star_weight(&heavy, &c) * 1.2, 0.0, 1.0, 900.0);
    CHECK(light.vy > heavy.vy);
}

static void test_it_can_be_carried_and_thrown(void) {
    CASE("a held star ignores the world; a released one keeps the throw");
    StarConfig c = base();
    FwmStar s;
    star_init(&s, &c);
    s.wx = 500.0; s.wy = 300.0;

    star_grab(&s);
    CHECK(!star_push(&s, &c, star_weight(&s, &c) * 10.0, 1.0, 0.0, 2000.0));
    star_move(&s, &c, 1.0, 0.0, 1920.0, 0.0, 1080.0);
    CHECK(s.wx == 500.0 && s.wy == 300.0);   /* the hand decides where it is */

    star_release(&s, 400.0, -200.0);
    star_move(&s, &c, 0.5, 0.0, 1920.0, 0.0, 1080.0);
    CHECK(s.wx > 500.0);
    CHECK(s.wy < 300.0);

    /* It slows down, and it stops: a desktop is not space. */
    for (int i = 0; i < 4000; i++) star_move(&s, &c, 1.0/60.0, 0.0, 1920.0, 0.0, 1080.0);
    CHECK(s.vx == 0.0 && s.vy == 0.0);

    /* And it stays on its own desktop, bouncing off the ends. */
    star_init(&s, &c);
    s.wx = 1900.0; s.wy = 500.0;
    star_release(&s, 3000.0, 0.0);
    for (int i = 0; i < 600; i++) star_move(&s, &c, 1.0/60.0, 0.0, 1920.0, 0.0, 1080.0);
    CHECK(s.wx >= 0.0 && s.wx <= 1920.0);
}

static void test_two_of_them(void) {
    /* Orbits are not scripted: attraction plus the speed they already carry
     * is the whole of it. Thrown past one another, they should end up going
     * round rather than either meeting or leaving. */
    CASE("two stars pull on each other, and can end up circling");
    StarConfig c = base();
    FwmStar a, b;
    star_init(&a, &c); star_init(&b, &c);
    a.wx = 800.0; a.wy = 500.0;
    b.wx = 1100.0; b.wy = 500.0;
    b.vy = 260.0;                       /* thrown across, not at */

    double r0 = 300.0, closest = r0, farthest = 0.0;
    int laps = 0;
    double prev_side = 1.0;
    for (int i = 0; i < 4000; i++) {
        star_attract(&a, &b, &c, 1.0/60.0);
        star_move(&a, &c, 1.0/60.0, 0.0, 4000.0, 0.0, 2000.0);
        star_move(&b, &c, 1.0/60.0, 0.0, 4000.0, 0.0, 2000.0);
        double dx = b.wx - a.wx, dy = b.wy - a.wy;
        double r = sqrt(dx * dx + dy * dy);
        if (r < closest) closest = r;
        if (r > farthest) farthest = r;
        double side = dy >= 0.0 ? 1.0 : -1.0;   /* crossings of the axis */
        if (side != prev_side) { laps++; prev_side = side; }
    }
    CHECK(closest < r0);                /* it fell towards it ... */
    CHECK(laps >= 2);                   /* ... and went round rather than in */

    /* Straight at each other, they meet. */
    star_init(&a, &c); star_init(&b, &c);
    a.wx = 800.0; a.wy = 500.0;
    b.wx = 1000.0; b.wy = 500.0;
    int met = 0;
    for (int i = 0; i < 4000 && !met; i++) {
        star_attract(&a, &b, &c, 1.0/60.0);
        star_move(&a, &c, 1.0/60.0, 0.0, 4000.0, 0.0, 2000.0);
        star_move(&b, &c, 1.0/60.0, 0.0, 4000.0, 0.0, 2000.0);
        met = star_touching(&a, &b, &c);
    }
    CHECK(met);
}

static void test_vampirism(void) {
    /* A compact object beside an ordinary star strips gas off it. One gets
     * heavier, the other lighter, and only ever in that direction. */
    CASE("a black hole drinks from the star beside it");
    StarConfig c = base();
    FwmStar hole, victim;
    star_init(&hole, &c); star_init(&victim, &c);
    hole.phase = STAR_HOLE; hole.mass = 3.0; hole.shown_mass = 3.0;
    victim.mass = 2.0;
    victim.wx = 500.0; victim.wy = 500.0;

    /* Far apart, nothing happens at all. */
    hole.wx = 500.0 + star_radius(&victim, &c) * 30.0; hole.wy = 500.0;
    CHECK(star_siphon(&hole, &victim, &c, 1.0) == 0.0);
    CHECK(victim.mass == 2.0);

    /* Close in, it pours, and the books balance. */
    hole.wx = 500.0 + star_radius(&victim, &c) * 2.0;
    double before = hole.mass + victim.mass;
    double rate = star_siphon(&hole, &victim, &c, 1.0);
    CHECK(rate > 0.0);
    CHECK(victim.mass < 2.0);
    CHECK(hole.mass > 3.0);
    CHECK(fabs((hole.mass + victim.mass) - before) < 1e-9);

    /* Never the other way: an ordinary star does not rob a black hole. */
    CHECK(star_siphon(&victim, &hole, &c, 1.0) == 0.0);
}

static void test_merging(void) {
    /* What comes out is decided by the mass, as everything here is — two
     * neutron stars can add up to a hole, which in the sky is a kilonova. */
    CASE("two merge into whatever their combined mass allows");
    StarConfig c = base();
    FwmStar a, b;
    star_init(&a, &c); star_init(&b, &c);
    a.phase = STAR_NEUTRON; a.mass = 1.6; a.wx = 400.0; a.wy = 400.0; a.vx = 100.0;
    b.phase = STAR_NEUTRON; b.mass = 1.6; b.wx = 440.0; b.wy = 400.0; b.vx = -20.0;

    star_merge(&a, &b, &c);
    /* Some of it left as gravitational waves. */
    CHECK(a.mass < 3.2);
    CHECK(a.mass > 3.0);
    /* Momentum carried through the merger. */
    CHECK(a.vx > 30.0 && a.vx < 50.0);
    /* And it settles through a collapse, which is the part worth watching. */
    CHECK_INT(a.phase, STAR_COLLAPSE);
    for (int i = 0; i < 4000 && a.phase == STAR_COLLAPSE; i++) star_tick(&a, &c, 1.0/60.0);
    CHECK_INT(a.phase, STAR_HOLE);     /* 3.07 solar masses is past TOV */
    CHECK(b.mass == 0.0);
}

static void test_it_spins(void) {
    CASE("it turns on its axis, and keeps turning");
    StarConfig c = base();
    FwmStar s;
    star_init(&s, &c);
    star_spin(&s, 2.0);
    CHECK(s.angvel > 0.0);
    double a0 = s.angle;
    star_move(&s, &c, 0.25, 0.0, 4000.0, 0.0, 2000.0);
    CHECK(s.angle != a0);

    /* The spin outlasts the drift by a long way: nothing rubs on a star. */
    double spin_then = s.angvel;
    for (int i = 0; i < 300; i++) star_move(&s, &c, 1.0/60.0, 0.0, 4000.0, 0.0, 2000.0);
    CHECK(s.angvel > spin_then * 0.5);

    /* And it cannot be spun into a flicker. */
    for (int i = 0; i < 50; i++) star_spin(&s, 5.0);
    CHECK(s.angvel <= 9.0);
}

static void test_the_supernova(void) {
    /* A collapse that simply ends is a star quietly going missing. The blast
     * is the part everybody has seen pictures of, and it has to happen at the
     * end of the fall and be over shortly after. */
    CASE("a collapse throws off a shell, and it fades");
    StarConfig c = base();
    FwmStar s;
    star_init(&s, &c);
    CHECK(star_blast(&s, &c) == 0.0);         /* nothing while it burns */

    star_collapse_now(&s);
    CHECK(star_blast(&s, &c) == 0.0);         /* nor at the start of the fall */

    /* It lights up at the very end of the collapse ... */
    star_tick(&s, &c, c.collapse_s * 0.95);
    double during = star_blast(&s, &c);
    CHECK(during > 0.0 && during < 0.2);

    /* ... goes on expanding through the remnant ... */
    for (int i = 0; i < 60 && s.phase == STAR_COLLAPSE; i++) star_tick(&s, &c, 1.0/60.0);
    CHECK(s.phase != STAR_COLLAPSE);
    double after = star_blast(&s, &c);
    CHECK(after >= during);

    double prev = after;
    for (int i = 0; i < 60; i++) {
        star_tick(&s, &c, 1.0/60.0);
        double now = star_blast(&s, &c);
        CHECK(now >= prev - 1e-9);            /* only ever outward */
        prev = now;
    }

    /* ... and is gone a few seconds later, so it does not cost anything for
     * the rest of the session. */
    for (int i = 0; i < 600; i++) star_tick(&s, &c, 1.0/60.0);
    CHECK(star_blast(&s, &c) == 0.0);
}

static void test_ignition(void) {
    /* A star condenses rather than arriving. It has to start unlit, come up
     * over a couple of seconds, and then never do it again. */
    CASE("a star lights rather than appearing");
    StarConfig c = base();
    FwmStar s;
    star_init(&s, &c);

    CHECK(star_ignition(&s, &c) == 0.0);
    star_tick(&s, &c, 0.5);
    double half = star_ignition(&s, &c);
    CHECK(half > 0.0 && half < 1.0);
    star_tick(&s, &c, 0.5);
    CHECK(star_ignition(&s, &c) > half);

    /* Lit, and it stays lit. */
    star_tick(&s, &c, 5.0);
    CHECK(star_ignition(&s, &c) == 1.0);
    star_tick(&s, &c, 60.0);
    CHECK(star_ignition(&s, &c) == 1.0);

    /* A remnant is not being born, whatever its own clock says. */
    star_collapse_now(&s);
    for (int i = 0; i < 2000 && s.phase == STAR_COLLAPSE; i++) star_tick(&s, &c, 1.0/60.0);
    CHECK(star_ignition(&s, &c) == 1.0);
}

static void test_the_beam_is_on_a_cone(void) {
    /* Even rotation, uneven apparent motion — because the beam is tied to the
     * magnetic axis and that is not the spin axis. If the bearing simply
     * tracked the phase there would be no cone and no pulse, only a spotlight
     * going round. */
    CASE("the pulsar's beam sweeps a cone, not a circle");
    StarConfig c = base();
    FwmStar s;
    star_init(&s, &c);
    s.phase = STAR_NEUTRON;

    double prev_b = 0.0, prev_step = -1.0;
    double min_step = 1e9, max_step = -1e9;
    double min_aim = 1e9, max_aim = -1e9;

    for (int i = 0; i <= 72; i++) {
        s.beam = i * 5.0;
        double b, aim;
        star_pulse(&s, &c, &b, &aim);
        if (aim < min_aim) min_aim = aim;
        if (aim > max_aim) max_aim = aim;
        if (i > 0) {
            double step = fabs(fmod(b - prev_b + 540.0, 360.0) - 180.0);
            if (step < min_step) min_step = step;
            if (step > max_step) max_step = step;
            (void)prev_step;
        }
        prev_b = b;
    }

    /* The apparent bearing moves at visibly different rates round the turn. */
    CHECK(max_step > min_step * 1.5);
    /* And the beam is aimed at you for only part of it: that is the pulse. */
    CHECK(min_aim < 0.05);
    /* Not 1.0: the beam only points dead at you if the magnetic axis lies in
     * the plane of the sky, and at 58 degrees it never quite does. Nearly
     * every real pulsar is like this — you catch the edge of the beam, which
     * is why the pulse has a shape rather than being a square wave. */
    CHECK(max_aim > 0.7);

    /* The spin itself stays perfectly even — a pulsar's one famous quality. */
    star_tick(&s, &c, 1.0 / c.pulsar_hz);
    double after = s.beam;
    star_tick(&s, &c, 1.0 / c.pulsar_hz);
    double later = s.beam;
    double turn1 = fmod(after + 360.0, 360.0);
    double turn2 = fmod(later + 360.0, 360.0);
    CHECK(fabs(turn1 - turn2) < 1.0);
}

static void test_what_bends_light(void) {
    /* Lensing is not a black hole's private trick — it is a matter of how much
     * mass sits inside how small a radius, and a neutron star is compact
     * enough that the effect is plain. Everything with a surface you could
     * land on is not, by several orders of magnitude, and that gap is what
     * lets one number decide the whole thing with no phase named anywhere. */
    CASE("compactness sorts what lenses from what does not");
    StarConfig c = base();
    FwmStar s;
    star_init(&s, &c);

    /* A burning star: the Sun deflects starlight by under two arcseconds, and
     * this says the same thing. */
    double burning = star_compactness(&s, &c);
    CHECK(burning < 1e-5);

    /* An ember. Denser than a star by a factor of a million and still nothing
     * to look at: a white dwarf is the size of a planet, not of a city. */
    s.mass = 1.0;
    star_collapse_now(&s);
    for (int i = 0; i < 2000 && s.phase == STAR_COLLAPSE; i++) star_tick(&s, &c, 1.0/60.0);
    CHECK_INT(s.phase, STAR_DWARF);
    double dwarf = star_compactness(&s, &c);
    CHECK(dwarf > burning);
    CHECK(dwarf < 0.01);

    /* A pulsar. A sun and a half inside twenty kilometres, and now it is a
     * third of the way to a horizon — which is why you can see rather more
     * than half of one, and why this had to stop being a hole-only effect. */
    star_init(&s, &c);
    s.mass = 2.0;
    star_collapse_now(&s);
    for (int i = 0; i < 2000 && s.phase == STAR_COLLAPSE; i++) star_tick(&s, &c, 1.0/60.0);
    CHECK_INT(s.phase, STAR_NEUTRON);
    double pulsar = star_compactness(&s, &c);
    CHECK(pulsar > 0.2);
    CHECK(pulsar < 1.0);
    /* Three orders between it and the ember it did not become. */
    CHECK(pulsar > dwarf * 100.0);

    /* And a horizon, where the two lengths are one length. */
    star_init(&s, &c);
    s.mass = 6.0;
    star_collapse_now(&s);
    for (int i = 0; i < 2000 && s.phase == STAR_COLLAPSE; i++) star_tick(&s, &c, 1.0/60.0);
    CHECK_INT(s.phase, STAR_HOLE);
    CHECK(star_compactness(&s, &c) == 1.0);
    /* Eating does not make it bend harder — a bigger horizon reaches further,
     * but at the horizon itself it is always the same lens. */
    star_feed(&s, &c, c.throw_speed * 3.0);
    CHECK(star_compactness(&s, &c) == 1.0);
}

static void test_the_disc_forms(void) {
    /* A hole arrives out of the collapse with nothing around it. What becomes
     * the disc is the part of the envelope that failed to escape, and it takes
     * seconds to come down — so the picture has something to animate rather
     * than a disc that is simply there from the first frame. */
    CASE("a hole's disc falls in rather than arriving");
    StarConfig c = base();
    FwmStar s;
    star_init(&s, &c);

    /* Nothing that is not a hole is ever mid-formation. */
    CHECK(star_disc_form(&s, &c) == 1.0);

    s.mass = 6.0;
    star_collapse_now(&s);
    for (int i = 0; i < 2000 && s.phase == STAR_COLLAPSE; i++) star_tick(&s, &c, 1.0 / 60.0);
    CHECK_INT(s.phase, STAR_HOLE);

    /* Born with none of it. */
    CHECK(star_disc_form(&s, &c) < 0.05);
    star_tick(&s, &c, 0.5);
    double early = star_disc_form(&s, &c);
    CHECK(early > 0.0 && early < 1.0);
    star_tick(&s, &c, 0.5);
    CHECK(star_disc_form(&s, &c) > early);

    /* Settled, and it stays settled — including after a meal, which makes the
     * hole bigger without making it new. */
    star_tick(&s, &c, 6.0);
    CHECK(star_disc_form(&s, &c) == 1.0);
    star_feed(&s, &c, c.throw_speed * 3.0);
    star_tick(&s, &c, 1.0 / 60.0);
    CHECK(star_disc_form(&s, &c) == 1.0);
}

int main(void) {
    test_mass_decides_the_ending();
    test_fuel_and_the_fuse();
    test_pushing_a_remnant_over();
    test_feeding();
    test_collapse_shape();
    test_shadows_fan_out();
    test_dead_star_costs_nothing();
    test_every_collapse_actually_shrinks();
    test_a_hole_grows();
    test_only_something_heavier_moves_it();
    test_it_can_be_carried_and_thrown();
    test_two_of_them();
    test_vampirism();
    test_merging();
    test_it_spins();
    test_the_supernova();
    test_ignition();
    test_the_disc_forms();
    test_the_beam_is_on_a_cone();
    test_pulsar_sweeps();
    test_what_bends_light();
    return t_report("star");
}
