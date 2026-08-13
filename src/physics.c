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

#include "physics.h"
#include "defines.h"

#include <box2d/box2d.h>

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --- Unit bridge -----------------------------------------------------------
 * The rest of the compositor speaks pixels with a y-down, top-left-origin
 * coordinate system. Box2D wants meters and positions bodies by their center.
 * We keep the y axis pointing down (gravity is simply a +y vector) and convert
 * lengths with a fixed pixels-per-meter scale so window-sized boxes land in the
 * ~1..20 m range the solver is tuned for. */
#define PX_PER_METER 100.0
#define WALL_THICK_PX 60.0
/* The floor is one box per desktop (they sit at different heights), and two
 * boxes butted exactly edge to edge give Box2D a vertex to catch on: a window
 * sliding across the join between two desktops snags on the seam. Overlapping
 * them welds the seam shut. Small on purpose — where the two floors are at
 * DIFFERENT heights the overlap is a ledge sticking out over the lower one,
 * and a ledge nobody can feel is the whole point. */
#define FLOOR_SEAM_PX 2.0
/* A bar can reserve more space than a short monitor has, which would leave the
 * floor above the ceiling and Box2D solving a world with negative room in it.
 * The compositor guards this against the primary monitor's height (see
 * physics_tick_cb) but cannot for a monitor shorter than that one, so the last
 * word is here: whatever else happens, a desktop keeps this much play. */
#define MIN_PLAY_H_PX 64.0
#define POS_EPS 0.05  /* px: threshold to treat a mirror write as "external" */
#define VEL_EPS 0.05  /* px/s */

static inline float px2m(double px) { return (float)(px / PX_PER_METER); }
static inline double m2px(float m)  { return (double)m * PX_PER_METER; }

/* Per-body Box2D state, kept in an array parallel to PhysicsWorld.bodies. Index
 * i here always maps to world->bodies[i]: physics_remove_body marks the mirror
 * slot inactive and releases the body here, and physics_sync_body may then hand
 * that same index to a new window — the pairing is what keeps that safe. The shadow
 * fields hold the last values we wrote back into the mirror; comparing the
 * mirror against them tells us whether the mirror was changed by the outside
 * world (drag/throw/teleport) between steps and must be forced into Box2D. */
struct BodySlot {
    b2BodyId body;
    b2ShapeId shape;
    bool has;
    int sw, sh;              /* shape size the box was built with (px) */
    b2BodyType type;         /* last applied body type */
    int no_collide;          /* last applied collision-filter state */
    int spin;                /* last applied free-rotation state */
    /* Last applied material, so the per-step resolve below only talks to Box2D
     * when something actually changed (a reload, a drag onto another desktop). */
    float density, restitution, friction, gscale, damping;
    double sx, sy, svx, svy; /* shadow of last-written mirror position/velocity */
    double sang, sangvel;    /* ... and of its rotation */
};

struct Engine {
    b2WorldId world;
    struct BodySlot slots[MAX_WINDOWS];
    /* 0 left, 1 right, 2 ceiling, then one floor per desktop: the ceiling is
     * the same line on every screen (an inset from the top is an inset from
     * the top), but the floor is wherever that desktop's monitor ends. */
    b2BodyId walls[3 + FWM_DESKTOPS];
    bool walls_built;
    int wall_w, wall_h;      /* screen dims the walls were built for */
    int wall_wrap;           /* and whether the world was a ring at the time */
    int wall_top, wall_bottom; /* and the insets bars had reserved */
    int wall_desk_h[FWM_DESKTOPS]; /* and how tall each desktop's monitor was */

    /* Visualiser bars (physics_set_bars). Shapes are built once at a fixed
     * size and then only ever slid vertically, so a bar changing height costs
     * one velocity write and no shape rebuild — which matters when it happens
     * 48 times per bar per second. */
    b2BodyId bars[PHYSICS_MAX_BARS];
    int    bar_count;
    double bar_origin;       /* world x of the row's left edge */
    double bar_span;         /* px the row covers */
    double bar_max_h;        /* px the fully-lit bar rises */
    int    bar_screen_h;
    float linear_damping;    /* derived from world->friction */
    double last_gravity;     /* px/s^2 applied last step; wake bodies on change */
    float engine_max_speed;  /* last value pushed into Box2D's own speed limit */
    double drag_speed;       /* px/s the dragged window is moving, this step */
    double kin_advance;      /* px the fastest mouse-driven body covers, this step */
    float contact_push;      /* last overlap-recovery speed pushed into Box2D */
};

static struct Engine *engine_of(PhysicsWorld *world) {
    return (struct Engine *)world->engine;
}

/* Drop the Box2D body backing mirror slot i. Called as soon as a window goes
 * away rather than at the next step, so the slot is immediately safe to hand
 * to a new window: slot_create rewrites every shadow field, but only if it
 * runs, and it only runs when has == false. */
static void slot_release(struct Engine *eng, int i) {
    if (!eng) return;
    struct BodySlot *s = &eng->slots[i];
    if (!s->has) return;
    b2DestroyBody(s->body);
    s->has = false;
}

static double calc_mass(int width, int height, double mass_density) {
    return (double)(width * height) * mass_density;
}

/* Per-frame friction factor f (applied at PHYSICS_TICK_RATE) maps to Box2D
 * linear damping d via f^(dt*RATE) ~= 1/(1+d*dt)  =>  d ~= -ln(f)*RATE. */
static float damping_from_friction(double f) {
    if (f <= 0.0) f = 0.0001;
    if (f > 1.0) f = 1.0;
    return (float)(-log(f) * PHYSICS_TICK_RATE);
}

/* The physics one window is actually simulated with: its desktop's profile,
 * with whatever its [[rule]] overrode written over the top. Everything the
 * engine needs comes out of here, so there is one place that decides what a
 * window weighs and how hard it bounces. */
struct Material {
    float density;      /* Box2D shape density; 1.0 = the world's mass_density */
    float restitution;
    float friction;     /* Box2D surface friction, NOT the per-tick retention */
    float gravity_scale;/* per-body multiplier on the world's gravity vector */
    float damping;      /* linear damping, derived from per-tick retention */
};

static struct Material material_for(const PhysicsWorld *world, const PhysicsBody *m) {
    int d = m->desktop_id;
    if (d < 0 || d >= FWM_DESKTOPS) d = 0;
    const PhysicsProfile *p = &world->desktops[d];

    double density_ref = world->mass_density;
    if (density_ref <= 0.0) density_ref = MASS_DENSITY;

    double mass = p->mass_density / density_ref;
    if (!isnan(m->rule_mass)) mass *= m->rule_mass;
    /* Whatever the compositor decided this window weighs for reasons the
     * simulation knows nothing about (memory use, today). Guarded rather than
     * trusted: a body that predates the field, or one whose sampler has not run
     * yet, must weigh what it always did rather than nothing at all. */
    if (m->mass_scale > 0.0) mass *= m->mass_scale;

    double bounce = isnan(m->rule_bounce) ? p->restitution : m->rule_bounce;

    /* Retention is a per-tick factor; Box2D wants a damping rate. The rule's
     * value replaces the profile's outright rather than multiplying — 0.9 means
     * "keeps 90% of its speed each tick" wherever it is written. */
    double retention = isnan(m->rule_friction) ? p->friction : m->rule_friction;

    /* A desktop's gravity is expressed in px/s^2 so it reads like the global
     * one, but Box2D has a single gravity vector for the world: the difference
     * is carried per body. The world vector is world->gravity (scaled by the
     * cycle_gravity master switch), so the ratio is what is left over. */
    double gscale = (world->gravity != 0.0) ? p->gravity / world->gravity : 0.0;
    if (!isnan(m->rule_gravity)) gscale *= m->rule_gravity;

    struct Material mat;
    mat.density = (float)(mass > 0.0 ? mass : 0.0001);
    mat.restitution = (float)(bounce < 0.0 ? 0.0 : bounce);
    /* Real-world feel: some Coulomb friction so a window sliding along the
     * floor actually grinds to a stop instead of gliding like on ice. A window
     * told to keep less of its speed each tick is a rougher object, so the two
     * move together — but never to zero, or a stack of windows would slide
     * apart under its own weight. */
    mat.friction = (float)(0.4 * (retention < 1.0 ? (1.0 - retention) / 0.015 : 1.0));
    if (mat.friction < 0.05f) mat.friction = 0.05f;
    if (mat.friction > 1.0f)  mat.friction = 1.0f;
    mat.gravity_scale = (float)gscale;
    /* With gravity on, real objects barely slow down mid-air — they stop via
     * floor and wall contact friction instead. In zero-g the per-tick retention
     * is the only brake there is. */
    mat.damping = (fabs(gscale * world->gravity_scale) > 1e-6)
                      ? 0.05f : damping_from_friction(retention);
    return mat;
}

/* The one speed limit in this world: nothing dynamic moves faster than the
 * hardest throw the config allows.
 *
 * Applied to every dynamic body after every solve — including each piece of a
 * tick that was cut into substeps — which is what lets everything
 * that can hand out momentum — a fast drag, a visualiser bar, a stack collapsing
 * on itself — be bounded in ONE place instead of each guessing its own ceiling.
 * That is also the only reason the drag can now tell Box2D the truth about how
 * fast it is moving (see the drag branch below and defines.h).
 *
 * It is deliberately not felt in ordinary use: a window falling the full height
 * of a 1080px screen under earth gravity arrives at ~1456 px/s, below the 1800
 * default, so gravity, bounces and throws behave exactly as they did. What it
 * bounds is the pathological end — and with a ceiling of 1800 px/s a body covers
 * 30px per step, an order of magnitude less than the smallest window, so
 * window-through-window tunnelling stops being reachable at all. */
static void clamp_world_speed(PhysicsWorld *world, struct Engine *eng) {
    double cap = world->max_throw_speed;

    /* While a window is being dragged, the ceiling rises to whatever the hand is
     * doing. Nothing else is capable of that speed in the same step, so this is
     * in practice a licence for one thing only: the window being shoved may keep
     * up with the window shoving it. Hold it to the lower ceiling instead and a
     * hand moving faster than the cap simply walks through, because the victim
     * physically cannot get out of the way in time. The moment the drag ends the
     * ordinary ceiling applies again, so free flight is still bounded by what a
     * throw can do.
     *
     * Zero (chaos / no limit mode) must survive the exemption so free bodies
     * remain unclamped during a drag. */
    if (cap > 0.0 && eng->drag_speed > cap) cap = eng->drag_speed;

    for (int i = 0; i < world->body_count; i++) {
        struct BodySlot *s = &eng->slots[i];
        if (!world->bodies[i].active || !s->has) continue;
        if (s->type != b2_dynamicBody) continue;

        b2Vec2 v = b2Body_GetLinearVelocity(s->body);
        double vx = m2px(v.x), vy = m2px(v.y);
        assert(isfinite(vx) && isfinite(vy));
        if (cap <= 0.0) continue;   /* 0 means "no limit", for anyone who wants chaos */
        double speed = hypot(vx, vy);
        if (speed <= cap || speed <= 0.0) continue;
        double k = cap / speed;
        b2Body_SetLinearVelocity(s->body, (b2Vec2){ v.x * (float)k, v.y * (float)k });
    }
}

static void clamp_velocity(double *vx, double *vy, double max_speed) {
    assert(isfinite(*vx) && isfinite(*vy));
    /* Same reading of zero as clamp_world_speed: no limit, not a limit of zero.
     * Without this a throw in chaos mode scaled by 0/speed and landed dead,
     * while a window shoved by a drag — bounded post-solve, where the check
     * above does exist — kept moving. The one setting documented as "removes
     * the limit" was the one setting that stopped a throw entirely. */
    if (max_speed <= 0.0) return;

    double speed = hypot(*vx, *vy);
    if (speed <= max_speed || speed <= 0.0) return;
    double scale = max_speed / speed;
    *vx *= scale;
    *vy *= scale;
}

static void update_body_geometry(PhysicsBody *body, int x, int y, int width, int height, double mass_density) {
    body->x = x;
    body->y = y;
    body->width = width;
    body->height = height;
    body->mass = calc_mass(width, height, mass_density);
}

static int rects_overlap(int ax, int ay, int aw, int ah,
                         int bx, int by, int bw, int bh) {
    if (ax + aw <= bx) return 0;
    if (bx + bw <= ax) return 0;
    if (ay + ah <= by) return 0;
    if (by + bh <= ay) return 0;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void physics_init(PhysicsWorld *world) {
    world->body_count = 0;
    world->gravity_scale = 0.0;
    /* The strip has ends until somebody says otherwise. Spelled out because the
     * compositor happens to hand over zeroed memory and so never noticed this
     * was missing: a caller with a stack-allocated world got a garbage `wrap`,
     * which decides whether the world HAS end walls at all — and a world with no
     * end walls quietly lets windows sail off it. */
    world->wrap = 0;
    /* And the screen is the whole of it until a bar reserves part of it. */
    world->inset_top = world->inset_bottom = 0;
    /* And every desktop is as tall as the screen until a monitor says how tall
     * IT is. Spelled out for the same reason `wrap` is: a caller with a
     * stack-allocated world would otherwise get garbage heights and floors at
     * arbitrary depths. */
    for (int i = 0; i < FWM_DESKTOPS; i++) world->desktop_h[i] = 0;
    world->impact_count = 0;

    // Set system defaults just in case config doesn't overwrite them.
    // Tuned for a "real object" feel: earth gravity at 100 px/m, a dull bounce
    // (heavy furniture, not a rubber ball) and long weighty glides in zero-g.
    world->friction               = 0.985;
    world->mass_density           = 0.0005;
    world->throw_speed_multiplier = 0.65;
    world->max_throw_speed        = 1800.0;
    world->stop_speed_threshold   = 1.0;
    world->restitution            = 0.3;
    world->gravity                = 981.0;
    physics_reset_profiles(world);

    struct Engine *eng = calloc(1, sizeof(*eng));
    world->engine = eng;
    if (!eng) return;

    b2WorldDef wd = b2DefaultWorldDef();
    wd.gravity = (b2Vec2){0.0f, 0.0f}; // set per-step from gravity_scale
    wd.enableSleep = true;
    // Box2D suppresses restitution below this normal impact speed. Too high
    // (default ~100 px/s) and shallow wall hits slide instead of bounce; too low
    // and bodies micro-bounce forever instead of settling with a dead "thud".
    wd.restitutionThreshold = px2m(40.0);
    // Below this approach speed a contact produces no hit event. Set well above
    // resting jitter so stacked windows do not emit a stream of tiny impacts,
    // but low enough that a short drop still registers (a 200px fall under
    // gravity 981 lands at ~630 px/s).
    wd.hitEventThreshold = px2m(PHYSICS_HIT_MIN_SPEED);
    /* How fast overlap is undone. Box2D's default is 3 m/s, which is sensible
     * for a scene whose objects move at walking pace and far too slow for this
     * one: a window travels at up to 18 m/s here, so a shove that produced 100px
     * of overlap in one step was being unwound at 5px per step and the window
     * doing the shoving simply arrived first. Scaled to the world's own top speed
     * instead — overlap is resolved as fast as anything in this world moves.
     * Kept in step with the live knob below. */
    wd.maxContactPushSpeed = px2m(world->max_throw_speed);
    eng->world = b2CreateWorld(&wd);
    eng->contact_push = wd.maxContactPushSpeed;
    eng->walls_built = false;

    eng->linear_damping = damping_from_friction(world->friction);
}

void physics_reset_profiles(PhysicsWorld *world) {
    for (int i = 0; i < FWM_DESKTOPS; i++) {
        world->desktops[i].gravity      = world->gravity;
        world->desktops[i].friction     = world->friction;
        world->desktops[i].restitution  = world->restitution;
        world->desktops[i].mass_density = world->mass_density;
    }
}

void physics_destroy(PhysicsWorld *world) {
    if (!world || !world->engine) return;
    struct Engine *eng = engine_of(world);
    if (B2_IS_NON_NULL(eng->world)) {
        b2DestroyWorld(eng->world); // frees all bodies/shapes it owns
    }
    free(eng);
    world->engine = NULL;
}

/* ---------------------------------------------------------------------------
 * Mirror-only mutators (the bridge reads these each step)
 * ------------------------------------------------------------------------- */

PhysicsBody *physics_sync_body(PhysicsWorld *world, uint32_t id, int x, int y, int width, int height, int screen_width) {
    for (int i = 0; i < world->body_count; i++) {
        if (world->bodies[i].active && world->bodies[i].id == id) {
            update_body_geometry(&world->bodies[i], x, y, width, height, world->mass_density);
            int d = (int)((world->bodies[i].x + world->bodies[i].width / 2.0) / screen_width);
            if (d < 0) d = 0;
            if (d >= 10) d = 9;
            world->bodies[i].desktop_id = d;
            return &world->bodies[i];
        }
    }

    /* Reuse a slot left behind by a closed window before growing the array.
     * Without this the cap counts every window opened since the compositor
     * started, not the ones currently on screen — open and close a terminal
     * enough times in a day's work and the next window silently gets no body
     * at all. Slot indices stay aligned with the engine's, as the invariant
     * at the top of this file requires. */
    PhysicsBody *body = NULL;
    for (int i = 0; i < world->body_count; i++) {
        if (!world->bodies[i].active) {
            slot_release(engine_of(world), i);  /* no-op if already released */
            body = &world->bodies[i];
            break;
        }
    }

    if (!body) {
        if (world->body_count >= MAX_WINDOWS) return NULL;
        body = &world->bodies[world->body_count++];
    }
    memset(body, 0, sizeof(PhysicsBody));
    body->id = id;
    body->active = 1;
    body->flying = 0;
    body->vx = 0;
    body->vy = 0;
    body->pinned = 0;
    body->no_collide = 0;
    body->floating = 0;
    /* No rule has spoken for this window yet (view.c does that right after, if
     * one matched), so every material property defers to the desktop. */
    body->rule_mass = body->rule_gravity = NAN;
    body->rule_bounce = body->rule_friction = NAN;
    body->rule_toughness = body->rule_hardness = NAN;
    /* Weighs exactly what its area says until a sampler says otherwise. */
    body->mass_scale = 1.0;
    /* Nothing frozen: hit points follow the live mass until mass = "ram". */
    body->hp_frozen = 0.0;
    update_body_geometry(body, x, y, width, height, world->mass_density);

    int d = (int)((body->x + body->width / 2.0) / screen_width);
    if (d < 0) d = 0;
    if (d >= 10) d = 9;
    body->desktop_id = d;

    return body;
}

void physics_stop_body(PhysicsWorld *world, uint32_t id) {
    for (int i = 0; i < world->body_count; i++) {
        PhysicsBody *body = &world->bodies[i];
        if (body->active && body->id == id) {
            body->flying = 0;
            body->vx = 0;
            body->vy = 0;
            return;
        }
    }
}

void physics_throw_body(PhysicsWorld *world, uint32_t id, double vx, double vy) {
    for (int i = 0; i < world->body_count; i++) {
        PhysicsBody *body = &world->bodies[i];
        if (body->active && body->id == id) {
            body->flying = 1;
            body->vx = vx * world->throw_speed_multiplier;
            body->vy = vy * world->throw_speed_multiplier;
            clamp_velocity(&body->vx, &body->vy, world->max_throw_speed);
            /* The spin a thrown window carries is NOT invented here. It is
             * whatever it already had when it was let go, which the drag has
             * been building all along out of where it was grabbed and how the
             * hand moved (server_drag_swing). Adding a term proportional to
             * the throw was the first version and it fought that: a window
             * flung straight got a spin nothing in the gesture called for. */
            return;
        }
    }
}

void physics_spin_body(PhysicsWorld *world, uint32_t id, double angvel) {
    PhysicsBody *body = physics_find_body(world, id);
    if (!body) return;
    body->spin = 1;
    body->angvel = angvel;
}

void physics_unspin_body(PhysicsWorld *world, uint32_t id) {
    PhysicsBody *body = physics_find_body(world, id);
    if (!body) return;
    body->spin = 0;
    /* Settle upright rather than freezing at whatever angle the window happened
     * to be at: a window left tilted has an axis-aligned scene node again the
     * moment the snapshot goes away, so the tilt would simply vanish in one
     * frame — snapping the mirror here makes that the intended end state. */
    body->angle = 0.0;
    body->angvel = 0.0;
}

void physics_set_velocity(PhysicsWorld *world, uint32_t id, double vx, double vy) {
    for (int i = 0; i < world->body_count; i++) {
        if (world->bodies[i].active && world->bodies[i].id == id) {
            world->bodies[i].vx = vx;
            world->bodies[i].vy = vy;
            return;
        }
    }
}

void physics_remove_body(PhysicsWorld *world, uint32_t id) {
    for (int i = 0; i < world->body_count; i++) {
        if (world->bodies[i].id == id) {
            world->bodies[i].active = 0;
            world->bodies[i].id = 0;
            /* Free the Box2D side now. physics_sync_body may hand this slot to
             * the next window before the next step ever runs, and reusing it
             * with a live body would inherit the dead window's velocity. */
            slot_release(engine_of(world), i);
            return;
        }
    }
}

void physics_push_away(PhysicsWorld *world, uint32_t pushed, uint32_t pusher, double speed) {
    PhysicsBody *a = physics_find_body(world, pusher);
    PhysicsBody *b = physics_find_body(world, pushed);
    if (!a || !b) return;

    double ax = a->x + a->width  / 2.0;
    double ay = a->y + a->height / 2.0;
    double bx = b->x + b->width  / 2.0;
    double by = b->y + b->height / 2.0;

    double dx = bx - ax;
    double dy = by - ay;
    double len = hypot(dx, dy);

    if (len < 1.0) { dx = 1.0; dy = 0.0; len = 1.0; }

    b->vx = (dx / len) * speed;
    b->vy = (dy / len) * speed;
    b->flying = 1;
}

void physics_push_overlapping(PhysicsWorld *world, uint32_t pusher, double speed) {
    PhysicsBody *a = physics_find_body(world, pusher);
    if (!a) return;

    for (int i = 0; i < world->body_count; i++) {
        PhysicsBody *b = &world->bodies[i];
        if (!b->active || b->id == pusher || b->no_collide || b->fullscreen || b->shaped) continue;
        if (b->desktop_id != a->desktop_id) continue;

        if (!rects_overlap((int)a->x, (int)a->y, a->width, a->height,
                           (int)b->x, (int)b->y, b->width, b->height)) continue;

        double dx = (b->x + b->width/2.0) - (a->x + a->width/2.0);
        double dy = (b->y + b->height/2.0) - (a->y + a->height/2.0);
        double len = hypot(dx, dy);
        if (len < 1.0) { dx = 1.0; dy = 0.0; len = 1.0; }

        b->vx = (dx / len) * speed;
        b->vy = (dy / len) * speed;
        b->flying = 1;
    }
}

/* ── hit points ──────────────────────────────────────────────────────── */

double physics_body_hp(const PhysicsBody *b) {
    if (!b) return 0.0;
    double base = (b->hp_frozen > 0.0) ? b->hp_frozen : b->mass;
    double tough = isnan(b->rule_toughness) ? 1.0 : b->rule_toughness;
    double hp = base * tough;
    return hp > 0.0 ? hp : 0.0;
}

double physics_body_hardness(const PhysicsBody *b) {
    if (!b) return 0.0;
    return isnan(b->rule_hardness) ? 1.0 : b->rule_hardness;
}

void physics_freeze_hp(PhysicsWorld *world, int freeze) {
    for (int i = 0; i < world->body_count; i++) {
        PhysicsBody *b = &world->bodies[i];
        if (!b->active) continue;
        if (!freeze) { b->hp_frozen = 0.0; continue; }
        /* Only bodies that have not been pinned yet, so this is safe to call on
         * every sample: windows opened after the mode was switched on get their
         * hit points at the next one, and windows already holding a value keep
         * it — which is the entire point of freezing.
         *
         * Mass is written by physics_step from the resolved material, so a body
         * that has not been stepped yet has none to freeze. Leaving it at 0
         * means it keeps following the live mass and is retried next time,
         * which is the safe way round: a window with no recorded weight is not
         * accidentally immortal. */
        if (b->hp_frozen <= 0.0 && b->mass > 0.0) b->hp_frozen = b->mass;
    }
}

PhysicsBody *physics_find_body(PhysicsWorld *world, uint32_t id) {
    for (int i = 0; i < world->body_count; i++) {
        if (world->bodies[i].active && world->bodies[i].id == id) {
            return &world->bodies[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Box2D bridge
 * ------------------------------------------------------------------------- */

/* Mirror top-left (px) + size -> Box2D center (meters). */
static b2Vec2 body_center_m(const PhysicsBody *m) {
    return (b2Vec2){
        px2m(m->x + m->width  / 2.0),
        px2m(m->y + m->height / 2.0),
    };
}

/* Every b2Body_SetTransform below must go through this: passing b2Rot_identity
 * would silently un-rotate a spinning window on the next resize, drag or
 * teleport. */
static b2Rot body_rot(const PhysicsBody *m) {
    return m->spin ? b2MakeRot((float)m->angle) : b2Rot_identity;
}

/* Collision categories. "No collide" means "pass through other WINDOWS", never
 * "leave the play area": a body whose category or mask is zero fails Box2D's
 * two-way filter test against everything, walls included, so such windows used
 * to sail straight out past the top and the desktop-1 / desktop-10 edges.
 * Walls therefore get their own bit that even a no-collide window keeps. */
#define CAT_WINDOW 0x0001u
#define CAT_WALL   0x0002u

static b2Filter filter_for(int no_collide) {
    b2Filter f = b2DefaultFilter();
    f.categoryBits = CAT_WINDOW;
    // Walls always; other windows only when collision is enabled.
    f.maskBits = no_collide ? CAT_WALL : (CAT_WALL | CAT_WINDOW);
    return f;
}

static b2Filter filter_for_wall(void) {
    b2Filter f = b2DefaultFilter();
    f.categoryBits = CAT_WALL;
    f.maskBits = CAT_WINDOW;
    return f;
}

/* One static box, centre and half-extents in px. Every wall in the world is
 * this shape with different numbers. */
static b2BodyId make_wall(struct Engine *eng, PhysicsWorld *world,
                          double cx, double cy, double hw, double hh) {
    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_staticBody;
    bd.position = (b2Vec2){px2m(cx), px2m(cy)};
    b2BodyId id = b2CreateBody(eng->world, &bd);

    b2ShapeDef sd = b2DefaultShapeDef();
    sd.material.restitution = (float)world->restitution;
    // Real-world feel: some Coulomb friction so a window sliding along the
    // floor/wall actually grinds to a stop instead of gliding like on ice.
    // (The old "sticking" bug was the pull-back clamp, not this friction.)
    sd.material.friction = 0.35f;
    sd.enableHitEvents = true;           // hitting the floor counts as an impact
    sd.filter = filter_for_wall();
    b2Polygon box = b2MakeBox(px2m(hw), px2m(hh));
    b2CreatePolygonShape(id, &sd, &box);
    return id;
}

/* Where the floor of desktop `d` sits: the bottom of the monitor showing it,
 * less whatever a bar along that bottom reserved. */
static double floor_y_for(const PhysicsWorld *world, int d, int screen_h) {
    double h = world->desktop_h[d] > 0 ? world->desktop_h[d] : screen_h;
    double y = h - world->inset_bottom;
    if (y < world->inset_top + MIN_PLAY_H_PX) y = world->inset_top + MIN_PLAY_H_PX;
    return y;
}

static void rebuild_walls(struct Engine *eng, PhysicsWorld *world, int screen_w, int screen_h) {
    /* The floor and ceiling stand at the edges of the space bars left free,
     * which is the whole screen unless [physics] solid_bars says otherwise. */
    int top = world->inset_top, bottom = world->inset_bottom;
    bool same = eng->walls_built && eng->wall_w == screen_w && eng->wall_h == screen_h
        && eng->wall_wrap == world->wrap
        && eng->wall_top == top && eng->wall_bottom == bottom;
    for (int i = 0; same && i < FWM_DESKTOPS; i++)
        same = eng->wall_desk_h[i] == world->desktop_h[i];
    if (same) return;

    if (eng->walls_built) {
        for (int i = 0; i < 3 + FWM_DESKTOPS; i++) {
            if (B2_IS_NON_NULL(eng->walls[i])) b2DestroyBody(eng->walls[i]);
            eng->walls[i] = b2_nullBodyId;
        }
    }

    double W = 10.0 * screen_w; // full virtual-desktop span
    double T = top;             // ceiling y: 0, or below a bar anchored up there
    double t = WALL_THICK_PX;

    /* The end walls have to reach past the DEEPEST floor in the world, not
     * past this desktop's — they are one pair for the whole strip, and a
     * window sitting on the low floor of a tall monitor at the far end must
     * still find something there. */
    double deepest = T;
    for (int i = 0; i < FWM_DESKTOPS; i++) {
        double y = floor_y_for(world, i, screen_h);
        if (y > deepest) deepest = y;
    }

    /* A ring has no ends, so it has no end walls: a window thrown off the left
     * of the first desktop must fly on and arrive at the right of the last, not
     * bounce. The floor and ceiling stay whatever happens. */
    if (world->wrap) {
        eng->walls[0] = eng->walls[1] = b2_nullBodyId;
    } else {
        double cy = (T + deepest) / 2.0, hh = (deepest - T) / 2.0 + t;
        eng->walls[0] = make_wall(eng, world, -t / 2.0,    cy, t / 2.0, hh);
        eng->walls[1] = make_wall(eng, world, W + t / 2.0, cy, t / 2.0, hh);
    }

    eng->walls[2] = make_wall(eng, world, W / 2.0, T - t / 2.0, W / 2.0 + t, t / 2.0);

    /* One floor per desktop, each at its own monitor's bottom, each overlapping
     * its neighbours so the strip reads as a single surface (FLOOR_SEAM_PX). */
    for (int d = 0; d < FWM_DESKTOPS; d++) {
        double y = floor_y_for(world, d, screen_h);
        eng->walls[3 + d] = make_wall(eng, world,
                                      (d + 0.5) * screen_w, y + t / 2.0,
                                      screen_w / 2.0 + FLOOR_SEAM_PX, t / 2.0);
        eng->wall_desk_h[d] = world->desktop_h[d];
    }

    eng->walls_built = true;
    eng->wall_w = screen_w;
    eng->wall_h = screen_h;
    eng->wall_wrap = world->wrap;
    eng->wall_top = top;
    eng->wall_bottom = bottom;
}

/* ── visualiser bars ─────────────────────────────────────────────────── */

static void bars_destroy(struct Engine *eng) {
    for (int i = 0; i < eng->bar_count; i++) {
        if (B2_IS_NON_NULL(eng->bars[i])) b2DestroyBody(eng->bars[i]);
        eng->bars[i] = b2_nullBodyId;
    }
    eng->bar_count = 0;
}

/* Where the CENTRE of bar i sits when it is lit to `level`. The box is a fixed
 * max_h tall and its top face is the surface windows land on, so level 0 parks
 * the whole box below the floor line rather than shrinking it — a shape that
 * changed size every frame would rebuild its contact manifold every frame, and
 * a window standing on it would judder. */
static double bar_center_y(const struct Engine *eng, double level) {
    double top = (double)eng->bar_screen_h - level * eng->bar_max_h;
    return top + eng->bar_max_h / 2.0;
}

static void bars_build(struct Engine *eng, PhysicsWorld *world, int count,
                       double origin_x, double span_w, double max_h, int screen_h) {
    bars_destroy(eng);
    if (count <= 0) return;
    if (count > PHYSICS_MAX_BARS) count = PHYSICS_MAX_BARS;

    eng->bar_count    = count;
    eng->bar_origin   = origin_x;
    eng->bar_span     = span_w;
    eng->bar_max_h    = max_h;
    eng->bar_screen_h = screen_h;

    double bw = span_w / (double)count;

    for (int i = 0; i < count; i++) {
        b2BodyDef bd = b2DefaultBodyDef();
        bd.type = b2_kinematicBody;
        bd.position = (b2Vec2){ px2m(origin_x + (i + 0.5) * bw),
                                px2m(bar_center_y(eng, 0.0)) };
        eng->bars[i] = b2CreateBody(eng->world, &bd);

        b2ShapeDef sd = b2DefaultShapeDef();
        /* Barely bouncy on purpose. The toss comes from the bar's own upward
         * speed, and stacking restitution on top of that turns a window that
         * merely sat down into one that never settles. */
        sd.material.restitution = 0.05f;
        /* Low friction so a window rides up and off a bar instead of being
         * dragged sideways every time a neighbouring band peaks. */
        sd.material.friction = 0.1f;
        /* Hit events off: a bar row touches a resting window sixty times a
         * second, and every one of those would fire a squash. */
        sd.enableHitEvents = false;
        sd.filter = filter_for_wall();
        b2Polygon box = b2MakeBox(px2m(bw / 2.0), px2m(max_h / 2.0));
        b2CreatePolygonShape(eng->bars[i], &sd, &box);
    }
    (void)world;
}

void physics_set_bars(PhysicsWorld *world, const float *levels, int count,
                      double origin_x, double span_w, double max_h,
                      int screen_h, double max_rise, double dt) {
    struct Engine *eng = engine_of(world);
    if (!eng) return;

    if (count <= 0 || !levels || span_w <= 0.0 || max_h <= 0.0 || dt <= 0.0) {
        bars_destroy(eng);
        return;
    }
    if (count > PHYSICS_MAX_BARS) count = PHYSICS_MAX_BARS;

    /* Anything that changes the row's geometry rather than its levels — a
     * different bar count, a resized screen, a config reload — rebuilds it. */
    if (eng->bar_count != count || eng->bar_span != span_w ||
        eng->bar_max_h != max_h || eng->bar_screen_h != screen_h) {
        bars_build(eng, world, count, origin_x, span_w, max_h, screen_h);
    }

    double bw = span_w / (double)count;

    /* The row follows the active desktop, so switching desktops moves it a
     * whole screen sideways. Sliding there would sweep every window on the way
     * into the far wall, so drop the row flat and teleport it instead. */
    bool moved = eng->bar_origin != origin_x;
    if (moved) eng->bar_origin = origin_x;

    for (int i = 0; i < count; i++) {
        b2BodyId body = eng->bars[i];
        if (B2_IS_NULL(body)) continue;

        double level = levels[i];
        if (level < 0.0) level = 0.0;
        if (level > 1.0) level = 1.0;
        double want_y = bar_center_y(eng, level);
        double want_x = origin_x + (i + 0.5) * bw;

        if (moved) {
            b2Body_SetTransform(body, (b2Vec2){ px2m(want_x), px2m(bar_center_y(eng, 0.0)) },
                                b2MakeRot(0.0f));
            b2Body_SetLinearVelocity(body, (b2Vec2){0.0f, 0.0f});
            continue;
        }

        /* Velocity from the body's ACTUAL position, not from the level we asked
         * for last time: integration error and any solver pushback then correct
         * themselves on the next step instead of accumulating into a row that
         * has drifted off the floor. */
        double cur_y = m2px(b2Body_GetPosition(body).y);
        double vy = (want_y - cur_y) / dt;
        /* Rising is negative y here. Capped — see BAR_MAX_RISE_SPEED. */
        if (vy < -max_rise) vy = -max_rise;
        b2Body_SetLinearVelocity(body, (b2Vec2){ 0.0f, px2m(vy) });
    }
}

static void slot_create(struct Engine *eng, PhysicsWorld *world, int i, PhysicsBody *m,
                        b2BodyType type, int no_collide) {
    struct BodySlot *s = &eng->slots[i];
    struct Material mat = material_for(world, m);

    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = type;
    bd.position = body_center_m(m);
    bd.linearVelocity = (b2Vec2){px2m(m->vx), px2m(m->vy)};
    bd.rotation = body_rot(m);
    bd.angularVelocity = (float)m->angvel;
    bd.fixedRotation = !m->spin;         // windows never rotate unless spun
    /* Left free a spin coasts for the better part of a minute, which reads as
     * the window being stuck rather than thrown. This bleeds it off over a few
     * seconds while still letting a hard kick get several turns in. */
    bd.angularDamping = 0.35f;
    bd.linearDamping = mat.damping;
    bd.gravityScale = mat.gravity_scale;
    bd.isBullet = true;                  // continuous collision: never tunnel a wall
    bd.isAwake = true;
    /* Carries the window id, so a hit event's shape resolves straight back to
     * the mirror without searching the slot array. */
    bd.userData = (void *)(uintptr_t)m->id;
    s->body = b2CreateBody(eng->world, &bd);

    b2ShapeDef sd = b2DefaultShapeDef();
    sd.density = mat.density;
    sd.material.restitution = mat.restitution;
    sd.material.friction = mat.friction;
    sd.enableHitEvents = true;           // impact effects (squash, shake, dust)
    sd.filter = filter_for(no_collide);
    b2Polygon box = b2MakeBox(px2m(m->width / 2.0), px2m(m->height / 2.0));
    s->shape = b2CreatePolygonShape(s->body, &sd, &box);

    s->density = mat.density;
    s->restitution = mat.restitution;
    s->friction = mat.friction;
    s->gscale = mat.gravity_scale;
    s->damping = mat.damping;
    s->has = true;
    s->sw = m->width;
    s->sh = m->height;
    s->type = type;
    s->no_collide = no_collide;
    s->spin = m->spin;
    s->sx = m->x; s->sy = m->y;
    s->svx = m->vx; s->svy = m->vy;
    s->sang = m->angle; s->sangvel = m->angvel;
}

void physics_step(PhysicsWorld *world, int screen_width, int screen_height,
                  uint32_t skip_a, uint32_t skip_b, uint32_t dragged_id, double dt) {
    struct Engine *eng = engine_of(world);
    if (!eng || B2_IS_NULL(eng->world)) return;

    rebuild_walls(eng, world, screen_width, screen_height);

    /* Gravity: config value is px/s^2 (y-down); convert to m/s^2. This is the
     * BASE vector — the one [physics] gravity names. A desktop with its own
     * gravity, and a window whose rule overrode it, ride on top of it through
     * their body's gravityScale (material_for), because Box2D has exactly one
     * gravity vector for the whole world. */
    double g = world->gravity * world->gravity_scale;
    b2World_SetGravity(eng->world, (b2Vec2){0.0f, px2m(g)});

    // Box2D does NOT wake sleeping bodies when world gravity changes — a
    // window that sat still long enough to sleep would just hang in the air
    // after cycle_gravity (until a drag changed its body type and woke it).
    bool gravity_changed = (g != eng->last_gravity);
    eng->last_gravity = g;

    // --- Push mirror -> Box2D ------------------------------------------------
    /* Recomputed from this step's drag, never carried over from the last one. */
    eng->drag_speed = 0.0;
    eng->kin_advance = 0.0;
    for (int i = 0; i < world->body_count; i++) {
        PhysicsBody *m = &world->bodies[i];
        struct BodySlot *s = &eng->slots[i];

        if (!m->active) {
            if (s->has) { b2DestroyBody(s->body); s->has = false; }
            continue;
        }

        // Desired body type. Pinned / fullscreen / tiled / an explicitly-frozen
        // body (skip_b) are immovable anchors (tiles are laid out by the BSP
        // layout and glide animation — physics must never shove them); the
        // dragged window is kinematic so it shoves others while the mouse, not
        // physics, owns its position.
        bool frozen = (skip_b != 0 && m->id == skip_b);
        bool dragged = (dragged_id != 0 && m->id == dragged_id);
        b2BodyType type = b2_dynamicBody;
        if (m->pinned || m->fullscreen || m->tiled || m->floating || frozen) type = b2_staticBody;
        else if (dragged) type = b2_kinematicBody;

        int no_collide = m->no_collide || m->floating || (skip_a != 0 && m->id == skip_a);

        if (!s->has) {
            slot_create(eng, world, i, m, type, no_collide);
            continue;
        }

        /* What this window is made of, this step: its desktop's profile with
         * its own rule over the top. Cheap to recompute, and it has to be — a
         * window dragged across a desktop edge changes material mid-flight. */
        struct Material mat = material_for(world, m);
        /* Keep the mirror's mass honest for whoever reads it (tray, IPC):
         * mat.density is a multiple of the world's mass_density by
         * construction. */
        m->mass = calc_mass(m->width, m->height, world->mass_density * mat.density);

        // Size change -> rebuild the collision box.
        if (s->sw != m->width || s->sh != m->height) {
            b2DestroyShape(s->shape, false);
            b2ShapeDef sd = b2DefaultShapeDef();
            sd.density = mat.density;
            sd.material.restitution = mat.restitution;
            sd.material.friction = mat.friction;
            sd.enableHitEvents = true;
            sd.filter = filter_for(no_collide);
            b2Polygon box = b2MakeBox(px2m(m->width / 2.0), px2m(m->height / 2.0));
            s->shape = b2CreatePolygonShape(s->body, &sd, &box);
            s->sw = m->width; s->sh = m->height;
            s->no_collide = no_collide;
            // The box was rebuilt around the body's OLD center, but the mirror
            // anchors the top-left corner — the center moves by half the size
            // delta. Re-drive the transform from the mirror or the window
            // jitters/creeps during interactive resize.
            b2Body_SetTransform(s->body, body_center_m(m), body_rot(m));
            s->density = mat.density;
            s->restitution = mat.restitution;
            s->friction = mat.friction;
        }

        /* Material changes: a config reload, a rule applied, or the window
         * having crossed onto a desktop with different physics. Guarded on the
         * shadow so the common case is four float compares and no Box2D call. */
        if (s->density != mat.density) {
            b2Shape_SetDensity(s->shape, mat.density, true);
            s->density = mat.density;
        }
        if (s->restitution != mat.restitution) {
            b2Shape_SetRestitution(s->shape, mat.restitution);
            s->restitution = mat.restitution;
        }
        if (s->friction != mat.friction) {
            b2Shape_SetFriction(s->shape, mat.friction);
            s->friction = mat.friction;
        }
        if (s->gscale != mat.gravity_scale) {
            b2Body_SetGravityScale(s->body, mat.gravity_scale);
            /* A window that was weightless and now is not has very likely been
             * asleep for a while; gravity alone does not wake it (see above). */
            b2Body_SetAwake(s->body, true);
            s->gscale = mat.gravity_scale;
        }
        if (s->damping != mat.damping) {
            b2Body_SetLinearDamping(s->body, mat.damping);
            s->damping = mat.damping;
        }

        if (s->type != type) {
            b2Body_SetType(s->body, type);
            s->type = type;
            /* Changing type moves the body between solver sets; re-assert the
             * spin the mirror is carrying so letting go of a window you were
             * stirring hands the spin over to free flight instead of dropping
             * it at the moment of release. */
            if (m->spin) {
                b2Body_SetAwake(s->body, true);
                b2Body_SetAngularVelocity(s->body, (float)m->angvel);
            }
        }
        if (s->no_collide != no_collide) {
            b2Shape_SetFilter(s->shape, filter_for(no_collide));
            s->no_collide = no_collide;
        }

        /* Spin turned on or off since the last step. Clearing fixedRotation
         * makes Box2D recompute the body's rotational inertia from its shape,
         * so the order here matters: free the rotation first, then hand it the
         * angle and the kick the mirror is carrying. */
        if (s->spin != m->spin) {
            /* Waking comes FIRST and is not optional: a sleeping Box2D body has
             * no velocity state at all, so b2Body_SetAngularVelocity on one is
             * silently dropped and the window that was just kicked into a spin
             * simply sits there. (A window resting in zero-g is asleep within a
             * second, which is to say: always, by the time the bind is pressed.) */
            b2Body_SetAwake(s->body, true);
            b2Body_SetFixedRotation(s->body, !m->spin);
            b2Body_SetTransform(s->body, body_center_m(m), body_rot(m));
            b2Body_SetAngularVelocity(s->body, (float)m->angvel);
            s->spin = m->spin;
            s->sang = m->angle;
            s->sangvel = m->angvel;
        }

        if (type == b2_dynamicBody) {
            if (gravity_changed) b2Body_SetAwake(s->body, true);
            // Only override Box2D's own evolution when the mirror was changed
            // from outside (teleport, throw, stop) since the last write-back.
            if (fabs(m->x - s->sx) > POS_EPS || fabs(m->y - s->sy) > POS_EPS) {
                b2Body_SetTransform(s->body, body_center_m(m), body_rot(m));
            }
            if (fabs(m->vx - s->svx) > VEL_EPS || fabs(m->vy - s->svy) > VEL_EPS) {
                b2Body_SetLinearVelocity(s->body, (b2Vec2){px2m(m->vx), px2m(m->vy)});
                b2Body_SetAwake(s->body, true);
            }
            /* The same rule one axis up, for the angle and the spin. */
            if (m->spin && fabs(m->angle - s->sang) > 1e-4) {
                b2Body_SetTransform(s->body, body_center_m(m), body_rot(m));
            }
            if (m->spin && fabs(m->angvel - s->sangvel) > 1e-4) {
                b2Body_SetAwake(s->body, true);   /* before the write; see above */
                b2Body_SetAngularVelocity(s->body, (float)m->angvel);
            }
        } else {
            /* Static / kinematic: the mirror is authoritative, drive it in.
             *
             * With one exception. A window being DRAGGED is kinematic — the
             * mouse owns where it is — but a spinning one must keep turning
             * while you hold it, or the whole effect freezes the moment you
             * pick the window up. So the position still comes from the mirror
             * and the ROTATION is left to Box2D, which integrates it from the
             * angular velocity the drag keeps writing (server_pointer.c's
             * swirl). Rewriting the rotation from the mirror here would pin
             * the window at the angle it had when the grab started. */
            bool spin_drag = (type == b2_kinematicBody && m->spin);
            b2Rot rot = spin_drag ? b2Body_GetRotation(s->body) : body_rot(m);
            /* A dragged window's velocity has to be DERIVED from how far the
             * mouse moved it since the last step. The mirror's vx/vy are not
             * it: the drag writes only a position (physics_sync_body), and the
             * grab zeroed the velocity, so handing Box2D m->vx/vy told it the
             * window was standing still while the transform teleported it
             * across the screen. A contact with a body that has no approach
             * speed produces no hit event — which is why shoving one window
             * into another never squashed either of them (they only got pushed
             * apart by penetration resolution) while a thrown window bouncing
             * off a wall, dynamic and honestly moving, always did.
             *
             * The velocity is handed over UNCLAMPED, and that is the fix for
             * dragging through windows: it has to agree with how far the
             * transform actually moved, or the solver resolves a contact that is
             * already deeper than it believes and the dragged window walks
             * straight through its neighbour (see defines.h, where the old
             * ceiling used to be). What a shove is allowed to hand over is
             * bounded after the step instead, by clamp_world_speed. */
            b2Vec2 v = {0.0f, 0.0f};
            b2Vec2 c = body_center_m(m);
            /* A whole monitor in one tick is not a hand — no arm covers 1920px
             * in 16ms. It is the world being moved under a window somebody
             * happens to be holding: the ring's join teleports the camera nine
             * screens and the drag's anchor with it, and send-to-desktop moves a
             * window a screen sideways on its own.
             *
             * Those must stay TELEPORTS. A body that appears somewhere did not
             * travel there: it sweeps nothing on the way (or crossing the join
             * mid-drag would batter every window on all ten desktops), it hands
             * over no momentum when it lands, and it is not a speed anything else
             * in the world is allowed to be measured against. Below the line,
             * everything is a hand and is swept. */
            bool teleport = (hypot(m->x - s->sx, m->y - s->sy) >= (double)screen_width);
            if (type == b2_kinematicBody && dt > 0.0 && !teleport) {
                double dvx = (m->x - s->sx) / dt;
                double dvy = (m->y - s->sy) / dt;
                v = (b2Vec2){px2m(dvx), px2m(dvy)};
                /* Start it where it WAS and let the velocity above carry it the
                 * rest of the way. Over a whole step that lands it in exactly the
                 * place the old teleport put it, so nothing changes for an
                 * ordinary drag — but a step cut into pieces now moves it a piece
                 * at a time instead of all at once, which is the entire point of
                 * the substepping below. Box2D's own kinematic integration is
                 * the "interpolated data"; there is nothing to interpolate by
                 * hand. */
                c = (b2Vec2){ px2m(s->sx + m->width  / 2.0),
                              px2m(s->sy + m->height / 2.0) };
                double adv = hypot(m->x - s->sx, m->y - s->sy);
                if (adv > eng->kin_advance) eng->kin_advance = adv;
                if (dragged) {
                    double sp = hypot(dvx, dvy);
                    if (sp > eng->drag_speed) eng->drag_speed = sp;
                }
            }
            b2Body_SetTransform(s->body, c, rot);
            b2Body_SetLinearVelocity(s->body, v);
            if (spin_drag) b2Body_SetAngularVelocity(s->body, (float)m->angvel);
        }
    }

    /* How much the hand is allowed to be worth to the REST of the world. The
     * dragged window itself is kinematic and goes wherever the mirror says
     * regardless; this bounds only the ceiling its victims are lifted to.
     *
     * One tick of substepping can resolve 16 * 32px of travel and no more, so
     * past that speed the exemption buys nothing — it just makes what is already
     * an unresolvable shove more violent. (Anything faster still than that is a
     * teleport and was never counted here at all; see the branch above.) */
    if (dt > 0.0) {
        double budget = PHYSICS_MAX_SUBSTEPS * PHYSICS_MAX_STEP_ADVANCE / dt;
        if (eng->drag_speed > budget) eng->drag_speed = budget;
    }

    /* Box2D has a speed limit of its own — 400 m/s, which is 40000 px/s at this
     * scale — and it enforces it silently. A config asking for a faster throw
     * than that used to get 39550 px/s and no explanation, so the engine's
     * ceiling is kept above ours rather than left at whatever the default was.
     *
     * "Ours" includes the drag exemption, which is why this sits AFTER the push
     * loop that measures the hand. clamp_world_speed lets a shoved window keep up
     * with the window shoving it; Box2D's own ceiling did not, and quietly held
     * every shove to 2 * max_throw_speed — 3600 px/s at the default. A hand moving
     * faster than that outran the window it was pushing no matter how finely the
     * tick was cut, because the victim was forbidden to get out of the way. That
     * is the other half of what breaks at speed, and the half no amount of
     * substepping fixes.
     *
     * Pushed every step because max_throw_speed is a live knob (`fwmctl set`) and
     * the drag speed changes constantly; it is one float compare inside Box2D
     * when nothing changed. */
    {
        double ceiling = world->max_throw_speed;
        if (ceiling > 0.0 && eng->drag_speed > ceiling) ceiling = eng->drag_speed;

        float want = (float)px2m(ceiling > 0.0 ? ceiling * 2.0 : 1.0e6);
        if (want != eng->engine_max_speed) {
            b2World_SetMaximumLinearSpeed(eng->world, want);
            eng->engine_max_speed = want;
        }

        /* And overlap recovery with it, for the reason given where the world was
         * created: overlap has to be undone at least as fast as anything in this
         * world moves, and during a drag the hand is that thing. The other two
         * numbers are Box2D's own defaults, repeated here because the call takes
         * all three. */
        float push = (float)px2m(ceiling > 0.0 ? ceiling : 1800.0);
        if (push != eng->contact_push) {
            b2World_SetContactTuning(eng->world, 30.0f, 10.0f, push);
            eng->contact_push = push;
        }
    }

    // --- Step ---------------------------------------------------------------
    /* One tick, but not necessarily one solve. Everything the simulation owns is
     * already bounded to max_throw_speed and so covers at most 30px in a step at
     * the default — the hand is the one thing in this world with no speed limit,
     * so it is the hand that decides how finely the tick has to be cut. See
     * PHYSICS_MAX_STEP_ADVANCE. A drag at ordinary speed asks for one substep and
     * this is the code that was always here. */
    int subs = 1;
    if (eng->kin_advance > PHYSICS_MAX_STEP_ADVANCE) {
        subs = (int)ceil(eng->kin_advance / PHYSICS_MAX_STEP_ADVANCE);
        if (subs > PHYSICS_MAX_SUBSTEPS) subs = PHYSICS_MAX_SUBSTEPS;
    }
    float sdt = (float)(dt / subs);

    // Refilled from scratch every tick; consumers read world->impacts before the
    // next one. Accumulated ACROSS the substeps rather than read after the last:
    // a landing in the first piece of a cut tick is a landing, and reading only
    // the final piece would silently drop it.
    world->impact_count = 0;

    for (int k = 0; k < subs; k++) {
        b2World_Step(eng->world, sdt, 4);

        /* Between the substeps as much as after them: a shove that hands a
         * neighbour more speed than this world allows must not be allowed to
         * spend it on the next piece of the same tick. */
        clamp_world_speed(world, eng);

        // A body's userData carries its window id, and walls have none, so a
        // wall reads back as id 0.
        b2ContactEvents ev = b2World_GetContactEvents(eng->world);
        for (int i = 0; i < ev.hitCount && world->impact_count < PHYSICS_MAX_IMPACTS; i++) {
            const b2ContactHitEvent *h = &ev.hitEvents[i];
            PhysicsImpact *im = &world->impacts[world->impact_count++];
            im->id_a = (uint32_t)(uintptr_t)b2Body_GetUserData(b2Shape_GetBody(h->shapeIdA));
            im->id_b = (uint32_t)(uintptr_t)b2Body_GetUserData(b2Shape_GetBody(h->shapeIdB));
            im->x = m2px(h->point.x);
            im->y = m2px(h->point.y);
            im->nx = h->normal.x;
            im->ny = h->normal.y;
            im->speed = m2px(h->approachSpeed);
        }
    }

    // --- Pull Box2D -> mirror -----------------------------------------------
    for (int i = 0; i < world->body_count; i++) {
        PhysicsBody *m = &world->bodies[i];
        struct BodySlot *s = &eng->slots[i];
        if (!m->active || !s->has) continue;

        if (s->type == b2_dynamicBody) {
            b2Vec2 p = b2Body_GetPosition(s->body);
            b2Vec2 v = b2Body_GetLinearVelocity(s->body);
            m->x = m2px(p.x) - m->width  / 2.0;
            m->y = m2px(p.y) - m->height / 2.0;
            m->vx = m2px(v.x);
            m->vy = m2px(v.y);
            if (m->spin) {
                m->angle  = b2Rot_GetAngle(b2Body_GetRotation(s->body));
                m->angvel = b2Body_GetAngularVelocity(s->body);
            }

            // Safety net for a TRUE escape only: the body has left the play area
            // entirely (no overlap at all) — e.g. spawned out of bounds. Normal
            // fast wall contacts penetrate a few px transiently and MUST be left
            // to Box2D so restitution can bounce them; clamping/zeroing here would
            // kill the bounce and make windows slide along the wall. On a real
            // escape, reflect (don't zero) so the window springs back into view.
            /* The net has to sit at the same depth as the floor the window is
             * standing over, or on a monitor shorter than the screen it would
             * catch windows below the glass instead of before it. Guarded like
             * material_for() does: desktop_id is set from a position, and a
             * position can be anywhere. */
            int d = m->desktop_id;
            if (d < 0 || d >= FWM_DESKTOPS) d = 0;
            double W = 10.0 * screen_width;
            double H = world->desktop_h[d] > 0 ? world->desktop_h[d] : (double)screen_height;
            double max_x = W - m->width;  if (max_x < 0) max_x = 0;
            double max_y = H - m->height; if (max_y < 0) max_y = 0;
            double r = world->restitution;
            int clamped = 0;
            /* On a ring the two horizontal cases are not escapes at all: they
             * are the crossing. The window is carried to the other end with
             * its velocity intact, because a throw that reaches the join is
             * still a throw. Only once it is ENTIRELY past, so it leaves one
             * end before appearing at the other and is never seen in both.
             *
             * This has to happen HERE rather than after the escape test: they
             * test the same condition, and the escape net got there first and
             * bounced the window back — which is exactly what "windows cannot
             * move between 1 and 10" turned out to be. */
            if (world->wrap && m->x >= W)                 { m->x -= W; clamped = 1; }
            else if (world->wrap && m->x + m->width <= 0.0) { m->x += W; clamped = 1; }
            else if (m->x + m->width <= 0.0) { m->x = 0.0;   m->vx = fabs(m->vx) * r; clamped = 1; }
            else if (m->x >= W)              { m->x = max_x; m->vx = -fabs(m->vx) * r; clamped = 1; }
            if (m->y + m->height <= 0.0){ m->y = 0.0;   m->vy = fabs(m->vy) * r; clamped = 1; }
            else if (m->y >= H)         { m->y = max_y; m->vy = -fabs(m->vy) * r; clamped = 1; }
            if (clamped) {
                b2Body_SetTransform(s->body, body_center_m(m), body_rot(m));
                b2Body_SetLinearVelocity(s->body, (b2Vec2){px2m(m->vx), px2m(m->vy)});
            }

            double speed = hypot(m->vx, m->vy);
            m->flying = (speed > world->stop_speed_threshold) ? 1 : 0;
            if (!m->flying) { m->vx = 0; m->vy = 0; }
        } else {
            // Anchor / dragged bodies keep the mirror the outside world set.
            if (s->type == b2_staticBody) { m->vx = 0; m->vy = 0; m->flying = 0; }
            /* Except the angle of a window spinning in your hand, which Box2D
             * integrated above and the mirror has to learn about — it is what
             * the renderer draws, and it is the angle the window carries into
             * free flight when you let go. */
            if (s->type == b2_kinematicBody && m->spin) {
                m->angle = b2Rot_GetAngle(b2Body_GetRotation(s->body));
            }
        }

        // Refresh the shadow so the next step can detect external writes.
        s->sx = m->x; s->sy = m->y;
        s->svx = m->vx; s->svy = m->vy;
        s->sang = m->angle; s->sangvel = m->angvel;

        int d = (int)((m->x + m->width / 2.0) / screen_width);
        if (d < 0) d = 0;
        if (d >= 10) d = 9;
        m->desktop_id = d;
    }
}
