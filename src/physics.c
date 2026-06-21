#include "physics.h"

#include <math.h>

static double calc_mass(int width, int height) {
    return (double)(width * height) * MASS_DENSITY;
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

static void update_body_geometry(PhysicsBody *body, int x, int y, int width, int height) {
    body->x = x;
    body->y = y;
    body->width = width;
    body->height = height;
    body->mass = calc_mass(width, height);
}

static int rects_overlap(int ax, int ay, int aw, int ah,
                         int bx, int by, int bw, int bh) {
    if (ax + aw <= bx) return 0;
    if (bx + bw <= ax) return 0;
    if (ay + ah <= by) return 0;
    if (by + bh <= ay) return 0;
    return 1;
}

static void separate_bodies(PhysicsBody *a, PhysicsBody *b) {
    double ax2 = a->x + a->width;
    double ay2 = a->y + a->height;
    double bx2 = b->x + b->width;
    double by2 = b->y + b->height;

    double overlap_left = ax2 - b->x;
    double overlap_right = bx2 - a->x;
    double overlap_top = ay2 - b->y;
    double overlap_bottom = by2 - a->y;

    double overlap_x = overlap_left < overlap_right ? overlap_left : overlap_right;
    double overlap_y = overlap_top < overlap_bottom ? overlap_top : overlap_bottom;

    if (overlap_x < overlap_y) {
        double shift = overlap_x / 2.0;
        if (a->x < b->x) {
            a->x -= shift;
            b->x += shift;
        } else {
            a->x += shift;
            b->x -= shift;
        }
    } else {
        double shift = overlap_y / 2.0;
        if (a->y < b->y) {
            a->y -= shift;
            b->y += shift;
        } else {
            a->y += shift;
            b->y -= shift;
        }
    }
}

static void resolve_collision(PhysicsBody *a, PhysicsBody *b) {
    double m1 = a->mass;
    double m2 = b->mass;

    double new_vx_a = ((m1 - m2) * a->vx + 2 * m2 * b->vx) / (m1 + m2);
    double new_vx_b = ((m2 - m1) * b->vx + 2 * m1 * a->vx) / (m1 + m2);
    double new_vy_a = ((m1 - m2) * a->vy + 2 * m2 * b->vy) / (m1 + m2);
    double new_vy_b = ((m2 - m1) * b->vy + 2 * m1 * a->vy) / (m1 + m2);

    a->vx = new_vx_a * RESTITUTION;
    b->vx = new_vx_b * RESTITUTION;
    a->vy = new_vy_a * RESTITUTION;
    b->vy = new_vy_b * RESTITUTION;

    a->flying = 1;
    b->flying = 1;
}

static int should_skip_collision(Window skip_a, Window skip_b, Window win) {
    return win != None && (win == skip_a || win == skip_b);
}

void physics_init(PhysicsWorld *world) {
    world->body_count = 0;
}

PhysicsBody *physics_sync_body(PhysicsWorld *world, Window win, int x, int y, int width, int height) {
    for (int i = 0; i < world->body_count; i++) {
        if (world->bodies[i].active && world->bodies[i].win == win) {
            update_body_geometry(&world->bodies[i], x, y, width, height);
            return &world->bodies[i];
        }
    }

    if (world->body_count >= MAX_WINDOWS) {
        return NULL;
    }

    PhysicsBody *body = &world->bodies[world->body_count++];
    body->win = win;
    body->active = 1;
    body->flying = 0;
    body->vx = 0;
    body->vy = 0;
    update_body_geometry(body, x, y, width, height);
    return body;
}

void physics_stop_body(PhysicsWorld *world, Window win) {
    for (int i = 0; i < world->body_count; i++) {
        PhysicsBody *body = &world->bodies[i];
        if (body->active && body->win == win) {
            body->flying = 0;
            body->vx = 0;
            body->vy = 0;
            return;
        }
    }
}

void physics_throw_body(PhysicsWorld *world, Window win, double vx, double vy) {
    for (int i = 0; i < world->body_count; i++) {
        PhysicsBody *body = &world->bodies[i];
        if (body->active && body->win == win) {
            body->flying = 1;
            body->vx = vx * THROW_SPEED_MULTIPLIER;
            body->vy = vy * THROW_SPEED_MULTIPLIER;
            clamp_velocity(&body->vx, &body->vy, MAX_THROW_SPEED);
            return;
        }
    }
}

void physics_step(PhysicsWorld *world, Display *dpy, int screen_width, int screen_height,
                  Window skip_a, Window skip_b, double dt) {
    for (int i = 0; i < world->body_count; i++) {
        PhysicsBody *body = &world->bodies[i];
        if (!body->active || !body->flying) continue;

        double new_x = body->x + body->vx * dt;
        double new_y = body->y + body->vy * dt;

        double friction_factor = pow(FRICTION, dt * PHYSICS_TICK_RATE);
        body->vx *= friction_factor;
        body->vy *= friction_factor;

        double speed = body->vx * body->vx + body->vy * body->vy;
        if (speed < STOP_SPEED_THRESHOLD) {
            body->flying = 0;
        }

        double min_x = -(body->width - PHYSICS_MARGIN);
        double max_x = screen_width - PHYSICS_MARGIN - body->width;
        double min_y = -(body->height - PHYSICS_MARGIN);
        double max_y = screen_height - PHYSICS_MARGIN - body->height;

        if (new_x < min_x) { new_x = min_x; body->vx = -body->vx * RESTITUTION; }
        if (new_x > max_x) { new_x = max_x; body->vx = -body->vx * RESTITUTION; }
        if (new_y < min_y) { new_y = min_y; body->vy = -body->vy * RESTITUTION; }
        if (new_y > max_y) { new_y = max_y; body->vy = -body->vy * RESTITUTION; }

        body->x = new_x;
        body->y = new_y;

        XMoveWindow(dpy, body->win, (int)lround(new_x), (int)lround(new_y));
    }

    for (int i = 0; i < world->body_count; i++) {
        PhysicsBody *a = &world->bodies[i];
        if (!a->active) continue;

        for (int j = i + 1; j < world->body_count; j++) {
            PhysicsBody *b = &world->bodies[j];
            if (!b->active) continue;
            if (!a->flying && !b->flying) continue;
            if (should_skip_collision(skip_a, skip_b, a->win) ||
                should_skip_collision(skip_a, skip_b, b->win)) {
                continue;
            }

            if (!rects_overlap((int)lround(a->x), (int)lround(a->y), a->width, a->height,
                               (int)lround(b->x), (int)lround(b->y), b->width, b->height)) {
                continue;
            }

            separate_bodies(a, b);
            resolve_collision(a, b);

            XMoveWindow(dpy, a->win, (int)lround(a->x), (int)lround(a->y));
            XMoveWindow(dpy, b->win, (int)lround(b->x), (int)lround(b->y));
        }
    }
}
