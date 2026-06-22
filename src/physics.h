#ifndef FWM_PHYSICS_H
#define FWM_PHYSICS_H

#include <X11/Xlib.h>
#include "config.h"

typedef struct {
    Window win;
    int active;
    int flying;
    double vx, vy;
    double mass;
    double x, y;
    int width, height;
    int desktop_id;
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

#endif /* FWM_PHYSICS_H */
