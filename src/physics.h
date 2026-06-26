#ifndef FWM_PHYSICS_H
#define FWM_PHYSICS_H

#include <X11/Xlib.h>
#include "defines.h"

typedef struct {
    Window win;
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
    double tile_sav_x, tile_sav_y;
    int tile_sav_w, tile_sav_h;
    int tiling_saved;
} PhysicsBody;



typedef struct {
    PhysicsBody bodies[MAX_WINDOWS];
    int body_count;
} PhysicsWorld;

void physics_init(PhysicsWorld *world);
PhysicsBody *physics_sync_body(PhysicsWorld *world, Window win, int x, int y, int width, int height, int screen_width);
void physics_stop_body(PhysicsWorld *world, Window win);
void physics_throw_body(PhysicsWorld *world, Window win, double vx, double vy);
void physics_step(PhysicsWorld *world, Display *dpy, int screen_width, int screen_height,
                  int camera_x,
                  Window skip_a, Window skip_b, Window dragged_win, double dt);
void physics_set_velocity(PhysicsWorld *world, Window win, double vx, double vy);
PhysicsBody *physics_find_body(PhysicsWorld *world, Window win);
void physics_remove_body(PhysicsWorld *world, Window win);
void physics_push_away(PhysicsWorld *world, Window pushed, Window pusher, double speed);
void physics_push_overlapping(PhysicsWorld *world, Window pusher, double speed);

#endif /* FWM_PHYSICS_H */
