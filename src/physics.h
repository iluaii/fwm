#ifndef FWM_PHYSICS_H
#define FWM_PHYSICS_H

#include <stdint.h>
#include "defines.h"

#define CORNER_SHARP   0
#define CORNER_CHAMFER 1
#define CORNER_ROUND   2

typedef struct {
    uint32_t id;         /* Unique ID of the view/window */
    int active;
    int flying;
    double vx, vy;
    double mass;
    double x, y;
    int width, height;
    int desktop_id;
    int fullscreen;
    int shaped;
    double sav_x, sav_y;
    int sav_w, sav_h;
    int pinned;
    int no_collide;
    int tiled;     /* window is managed by the tiling layout: physics never moves it */
    double tile_sav_x, tile_sav_y;
    int tile_sav_w, tile_sav_h;
    int tiling_saved;
    int corner_mode;
} PhysicsBody;

/* One collision hard enough to be worth reacting to, reported through the
 * mirror so the compositor never has to talk to Box2D. Valid only until the
 * next physics_step, which refills the list from scratch. */
#define PHYSICS_MAX_IMPACTS 32

typedef struct {
    uint32_t id_a, id_b;   /* window ids; 0 means a play-area wall */
    double x, y;           /* impact point, world px */
    double nx, ny;         /* contact normal, pointing from A to B */
    double speed;          /* approach speed, px/s — always positive */
} PhysicsImpact;

typedef struct {
    PhysicsBody bodies[MAX_WINDOWS];
    int body_count;

    PhysicsImpact impacts[PHYSICS_MAX_IMPACTS];
    int impact_count;
    double gravity_scale;
    
    /* Configurable physics parameters */
    double friction;
    double mass_density;
    double throw_speed_multiplier;
    double max_throw_speed;
    double stop_speed_threshold;
    double restitution;
    double gravity;

    /* Opaque Box2D engine state (see physics.c). The struct above is the
     * authoritative "mirror" that the rest of the compositor reads and writes;
     * physics_step pushes it into Box2D, steps, and pulls results back. */
    void *engine;
} PhysicsWorld;

void physics_init(PhysicsWorld *world);
void physics_destroy(PhysicsWorld *world);
PhysicsBody *physics_sync_body(PhysicsWorld *world, uint32_t id, int x, int y, int width, int height, int screen_width);
void physics_stop_body(PhysicsWorld *world, uint32_t id);
void physics_throw_body(PhysicsWorld *world, uint32_t id, double vx, double vy);
void physics_step(PhysicsWorld *world, int screen_width, int screen_height,
                  uint32_t skip_a, uint32_t skip_b, uint32_t dragged_id, double dt);
void physics_set_velocity(PhysicsWorld *world, uint32_t id, double vx, double vy);
PhysicsBody *physics_find_body(PhysicsWorld *world, uint32_t id);
void physics_remove_body(PhysicsWorld *world, uint32_t id);
void physics_push_away(PhysicsWorld *world, uint32_t pushed, uint32_t pusher, double speed);
void physics_push_overlapping(PhysicsWorld *world, uint32_t pusher, double speed);

#endif /* FWM_PHYSICS_H */
