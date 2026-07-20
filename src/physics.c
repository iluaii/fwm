#include "physics.h"
#include "defines.h"

#include <box2d/box2d.h>

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
#define POS_EPS 0.05  /* px: threshold to treat a mirror write as "external" */
#define VEL_EPS 0.05  /* px/s */

static inline float px2m(double px) { return (float)(px / PX_PER_METER); }
static inline double m2px(float m)  { return (double)m * PX_PER_METER; }

/* Per-body Box2D state, kept in an array parallel to PhysicsWorld.bodies. Body
 * slots are stable (physics_sync_body only appends, physics_remove_body only
 * marks inactive), so index i here always maps to world->bodies[i]. The shadow
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
    double sx, sy, svx, svy; /* shadow of last-written mirror position/velocity */
};

struct Engine {
    b2WorldId world;
    struct BodySlot slots[MAX_WINDOWS];
    b2BodyId walls[4];
    bool walls_built;
    int wall_w, wall_h;      /* screen dims the walls were built for */
    float linear_damping;    /* derived from world->friction */
    double last_gravity;     /* px/s^2 applied last step; wake bodies on change */
};

static struct Engine *engine_of(PhysicsWorld *world) {
    return (struct Engine *)world->engine;
}

static double calc_mass(int width, int height, double mass_density) {
    return (double)(width * height) * mass_density;
}

static void clamp_velocity(double *vx, double *vy, double max_speed) {
    double speed = hypot(*vx, *vy);
    if (speed <= max_speed || speed <= 0.0) {
        return;
    }
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
    eng->world = b2CreateWorld(&wd);
    eng->walls_built = false;

    // Per-frame friction factor f (applied at PHYSICS_TICK_RATE) maps to Box2D
    // linear damping d via f^(dt*RATE) ~= 1/(1+d*dt)  =>  d ~= -ln(f)*RATE.
    double f = world->friction;
    if (f <= 0.0) f = 0.0001;
    if (f > 1.0) f = 1.0;
    eng->linear_damping = (float)(-log(f) * PHYSICS_TICK_RATE);
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

    if (world->body_count >= MAX_WINDOWS) {
        return NULL;
    }

    PhysicsBody *body = &world->bodies[world->body_count++];
    memset(body, 0, sizeof(PhysicsBody));
    body->id = id;
    body->active = 1;
    body->flying = 0;
    body->vx = 0;
    body->vy = 0;
    body->pinned = 0;
    body->no_collide = 0;
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
            return;
        }
    }
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

static void rebuild_walls(struct Engine *eng, PhysicsWorld *world, int screen_w, int screen_h) {
    if (eng->walls_built && eng->wall_w == screen_w && eng->wall_h == screen_h) {
        return;
    }
    if (eng->walls_built) {
        for (int i = 0; i < 4; i++) {
            if (B2_IS_NON_NULL(eng->walls[i])) b2DestroyBody(eng->walls[i]);
        }
    }

    double W = 10.0 * screen_w; // full virtual-desktop span
    double H = screen_h;
    double t = WALL_THICK_PX;

    // {center_x, center_y, half_w, half_h} in px; inner faces flush with [0,W]x[0,H]
    double specs[4][4] = {
        {-t / 2.0,     H / 2.0,     t / 2.0, H / 2.0 + t}, // left
        {W + t / 2.0,  H / 2.0,     t / 2.0, H / 2.0 + t}, // right
        {W / 2.0,     -t / 2.0,     W / 2.0 + t, t / 2.0}, // top
        {W / 2.0,      H + t / 2.0, W / 2.0 + t, t / 2.0}, // bottom
    };

    for (int i = 0; i < 4; i++) {
        b2BodyDef bd = b2DefaultBodyDef();
        bd.type = b2_staticBody;
        bd.position = (b2Vec2){px2m(specs[i][0]), px2m(specs[i][1])};
        eng->walls[i] = b2CreateBody(eng->world, &bd);

        b2ShapeDef sd = b2DefaultShapeDef();
        sd.material.restitution = (float)world->restitution;
        // Real-world feel: some Coulomb friction so a window sliding along the
        // floor/wall actually grinds to a stop instead of gliding like on ice.
        // (The old "sticking" bug was the pull-back clamp, not this friction.)
        sd.material.friction = 0.35f;
        sd.enableHitEvents = true;       // hitting the floor counts as an impact
        sd.filter = filter_for_wall();
        b2Polygon box = b2MakeBox(px2m(specs[i][2]), px2m(specs[i][3]));
        b2CreatePolygonShape(eng->walls[i], &sd, &box);
    }

    eng->walls_built = true;
    eng->wall_w = screen_w;
    eng->wall_h = screen_h;
}

static void slot_create(struct Engine *eng, PhysicsWorld *world, int i, PhysicsBody *m,
                        b2BodyType type, int no_collide) {
    struct BodySlot *s = &eng->slots[i];

    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = type;
    bd.position = body_center_m(m);
    bd.linearVelocity = (b2Vec2){px2m(m->vx), px2m(m->vy)};
    bd.fixedRotation = true;             // windows never rotate
    bd.linearDamping = eng->linear_damping;
    bd.isBullet = true;                  // continuous collision: never tunnel a wall
    bd.isAwake = true;
    /* Carries the window id, so a hit event's shape resolves straight back to
     * the mirror without searching the slot array. */
    bd.userData = (void *)(uintptr_t)m->id;
    s->body = b2CreateBody(eng->world, &bd);

    b2ShapeDef sd = b2DefaultShapeDef();
    sd.density = 1.0f;
    sd.material.restitution = (float)world->restitution;
    sd.material.friction = 0.4f;
    sd.enableHitEvents = true;           // impact effects (squash, shake, dust)
    sd.filter = filter_for(no_collide);
    b2Polygon box = b2MakeBox(px2m(m->width / 2.0), px2m(m->height / 2.0));
    s->shape = b2CreatePolygonShape(s->body, &sd, &box);

    s->has = true;
    s->sw = m->width;
    s->sh = m->height;
    s->type = type;
    s->no_collide = no_collide;
    s->sx = m->x; s->sy = m->y;
    s->svx = m->vx; s->svy = m->vy;
}

void physics_step(PhysicsWorld *world, int screen_width, int screen_height,
                  uint32_t skip_a, uint32_t skip_b, uint32_t dragged_id, double dt) {
    struct Engine *eng = engine_of(world);
    if (!eng || B2_IS_NULL(eng->world)) return;

    rebuild_walls(eng, world, screen_width, screen_height);

    // Gravity: config value is px/s^2 (y-down); convert to m/s^2.
    double g = world->gravity * world->gravity_scale;
    b2World_SetGravity(eng->world, (b2Vec2){0.0f, px2m(g)});
    // Box2D does NOT wake sleeping bodies when world gravity changes — a
    // window that sat still long enough to sleep would just hang in the air
    // after cycle_gravity (until a drag changed its body type and woke it).
    bool gravity_changed = (g != eng->last_gravity);
    eng->last_gravity = g;

    // Air drag depends on the mode. Zero-g (floating windows) needs the config
    // friction as the only brake; with gravity on, real objects barely slow down
    // mid-air — they stop via floor/wall contact friction instead.
    float damping = (world->gravity_scale > 0.0) ? 0.05f : eng->linear_damping;

    // --- Push mirror -> Box2D ------------------------------------------------
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
        if (m->pinned || m->fullscreen || m->tiled || frozen) type = b2_staticBody;
        else if (dragged) type = b2_kinematicBody;

        int no_collide = m->no_collide || (skip_a != 0 && m->id == skip_a);

        if (!s->has) {
            slot_create(eng, world, i, m, type, no_collide);
            continue;
        }

        // Size change -> rebuild the collision box.
        if (s->sw != m->width || s->sh != m->height) {
            b2DestroyShape(s->shape, false);
            b2ShapeDef sd = b2DefaultShapeDef();
            sd.density = 1.0f;
            sd.material.restitution = (float)world->restitution;
            sd.material.friction = 0.4f;
            sd.filter = filter_for(no_collide);
            b2Polygon box = b2MakeBox(px2m(m->width / 2.0), px2m(m->height / 2.0));
            s->shape = b2CreatePolygonShape(s->body, &sd, &box);
            s->sw = m->width; s->sh = m->height;
            s->no_collide = no_collide;
            // The box was rebuilt around the body's OLD center, but the mirror
            // anchors the top-left corner — the center moves by half the size
            // delta. Re-drive the transform from the mirror or the window
            // jitters/creeps during interactive resize.
            b2Body_SetTransform(s->body, body_center_m(m), b2Rot_identity);
        }

        if (s->type != type) {
            b2Body_SetType(s->body, type);
            s->type = type;
        }
        b2Body_SetLinearDamping(s->body, damping);
        if (s->no_collide != no_collide) {
            b2Shape_SetFilter(s->shape, filter_for(no_collide));
            s->no_collide = no_collide;
        }

        if (type == b2_dynamicBody) {
            if (gravity_changed) b2Body_SetAwake(s->body, true);
            // Only override Box2D's own evolution when the mirror was changed
            // from outside (teleport, throw, stop) since the last write-back.
            if (fabs(m->x - s->sx) > POS_EPS || fabs(m->y - s->sy) > POS_EPS) {
                b2Body_SetTransform(s->body, body_center_m(m), b2Rot_identity);
            }
            if (fabs(m->vx - s->svx) > VEL_EPS || fabs(m->vy - s->svy) > VEL_EPS) {
                b2Body_SetLinearVelocity(s->body, (b2Vec2){px2m(m->vx), px2m(m->vy)});
                b2Body_SetAwake(s->body, true);
            }
        } else {
            // Static / kinematic: the mirror is authoritative, drive it in.
            b2Body_SetTransform(s->body, body_center_m(m), b2Rot_identity);
            b2Vec2 v = (type == b2_kinematicBody)
                           ? (b2Vec2){px2m(m->vx), px2m(m->vy)}
                           : (b2Vec2){0.0f, 0.0f};
            b2Body_SetLinearVelocity(s->body, v);
        }
    }

    // --- Step ---------------------------------------------------------------
    b2World_Step(eng->world, (float)dt, 4);

    // --- Collect impacts ----------------------------------------------------
    // Refilled from scratch every step; consumers read world->impacts before
    // the next one. A body's userData carries its window id, and walls have
    // none, so a wall reads back as id 0.
    world->impact_count = 0;
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

            // Safety net for a TRUE escape only: the body has left the play area
            // entirely (no overlap at all) — e.g. spawned out of bounds. Normal
            // fast wall contacts penetrate a few px transiently and MUST be left
            // to Box2D so restitution can bounce them; clamping/zeroing here would
            // kill the bounce and make windows slide along the wall. On a real
            // escape, reflect (don't zero) so the window springs back into view.
            double W = 10.0 * screen_width, H = (double)screen_height;
            double max_x = W - m->width;  if (max_x < 0) max_x = 0;
            double max_y = H - m->height; if (max_y < 0) max_y = 0;
            double r = world->restitution;
            int clamped = 0;
            if (m->x + m->width <= 0.0) { m->x = 0.0;   m->vx = fabs(m->vx) * r; clamped = 1; }
            else if (m->x >= W)         { m->x = max_x; m->vx = -fabs(m->vx) * r; clamped = 1; }
            if (m->y + m->height <= 0.0){ m->y = 0.0;   m->vy = fabs(m->vy) * r; clamped = 1; }
            else if (m->y >= H)         { m->y = max_y; m->vy = -fabs(m->vy) * r; clamped = 1; }
            if (clamped) {
                b2Body_SetTransform(s->body, body_center_m(m), b2Rot_identity);
                b2Body_SetLinearVelocity(s->body, (b2Vec2){px2m(m->vx), px2m(m->vy)});
            }

            double speed = hypot(m->vx, m->vy);
            m->flying = (speed > world->stop_speed_threshold) ? 1 : 0;
            if (!m->flying) { m->vx = 0; m->vy = 0; }
        } else {
            // Anchor / dragged bodies keep the mirror the outside world set.
            if (s->type == b2_staticBody) { m->vx = 0; m->vy = 0; m->flying = 0; }
        }

        // Refresh the shadow so the next step can detect external writes.
        s->sx = m->x; s->sy = m->y;
        s->svx = m->vx; s->svy = m->vy;

        int d = (int)((m->x + m->width / 2.0) / screen_width);
        if (d < 0) d = 0;
        if (d >= 10) d = 9;
        m->desktop_id = d;
    }
}
