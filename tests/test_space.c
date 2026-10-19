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

/* The solar-system wallpaper's world and camera, without a GPU.
 *
 * What the picture LOOKS like is checked by eye, offscreen (tools/space-
 * render.c). What it DOES is here: a window becomes a moon you can see being
 * born, closing it blows the moon up, focus flies the camera to it, a closed
 * focused window backs the camera off before anything else happens, and the
 * camera never ends up inside anything on the way. space.c reaches no further
 * than config.h's types, so all of it runs in a loop. */

#include "test.h"
#include "space.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ASPECT (16.0 / 9.0)

static SpaceConfig base(void) {
    SpaceConfig c;
    memset(&c, 0, sizeof(c));
    c.star = 0;
    for (int d = 0; d < FWM_DESKTOPS; d++) c.planets[d] = d % SPACE_KIND_COUNT;
    c.fps = 30.0;
    c.scale = 0.5;
    c.exposure = 1.0;
    c.speed = 1.0;
    return c;
}

typedef struct {
    Space sp;
    SpaceConfig cfg;
    SpaceCam cam;          /* the one screen, on `desk` */
    int desk;
    SpaceCam *cams[SPACE_PLANETS];
    SpaceWindow win[32];
    int nwin;
    uint32_t focused;
    double real;
} World;

static void world_init(World *w) {
    memset(w, 0, sizeof(*w));
    w->cfg = base();
    space_init(&w->sp, &w->cfg, 123456.0, 42);
    w->real = 100.0;
}

/* One frame, the way space_draw.c drives it. */
static void frame(World *w, double dt, bool quiet) {
    w->real += dt;
    for (int d = 0; d < SPACE_PLANETS; d++) w->cams[d] = NULL;
    w->cams[w->desk] = &w->cam;
    space_step(&w->sp, &w->cfg, dt, w->real);
    space_sync_windows(&w->sp, w->win, w->nwin, w->cams, ASPECT, w->real, quiet);
    space_sync_focus(&w->sp, w->focused);
    space_cam_update(&w->sp, &w->cam, w->desk, w->real, quiet);
}

static void run(World *w, double secs) {
    for (double s = 0.0; s < secs; s += 1.0 / 30.0) frame(w, 1.0 / 30.0, false);
}

static void open_win(World *w, uint32_t id, int desk) {
    w->win[w->nwin].id = id;
    w->win[w->nwin].desktop = desk;
    w->nwin++;
}

static void close_win(World *w, uint32_t id) {
    for (int i = 0; i < w->nwin; i++)
        if (w->win[i].id == id) { w->win[i] = w->win[--w->nwin]; return; }
}

static double dist3(const double a[3], const double b[3]) {
    return sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

/* Is p on screen for this pose? The shader's projection: a 50 degree
 * vertical field, a 16:9 screen, the horizon tipped by `roll`. */
static bool on_screen(const SpacePose *pose, const double p[3]) {
    const double *f = pose->fwd;
    double r0[3] = { -f[2], 0.0, f[0] };              /* fwd x up, up = +y */
    double rl = sqrt(r0[0] * r0[0] + r0[2] * r0[2]);
    r0[0] /= rl; r0[2] /= rl;
    double u0[3] = { r0[1] * f[2] - r0[2] * f[1], r0[2] * f[0] - r0[0] * f[2], r0[0] * f[1] - r0[1] * f[0] };
    double c = cos(pose->roll), s = sin(pose->roll);
    double right[3], up[3];
    for (int i = 0; i < 3; i++) { right[i] = r0[i] * c + u0[i] * s; up[i] = u0[i] * c - r0[i] * s; }
    double d[3] = { p[0] - pose->eye[0], p[1] - pose->eye[1], p[2] - pose->eye[2] };
    double z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    if (z <= 0.0) return false;
    double th = tan(25.0 * M_PI / 180.0);
    double x = (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / (z * th * ASPECT);
    double y = (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / (z * th);
    return fabs(x) < 1.0 && fabs(y) < 1.0;
}

static void test_windows_already_open_are_just_there(void) {
    CASE("windows already open are just there");
    World w;
    world_init(&w);
    open_win(&w, 1, 0);
    open_win(&w, 2, 0);
    open_win(&w, 3, 4);
    frame(&w, 1.0 / 30.0, true);
    CHECK_INT(w.sp.moon_count, 3);
    CHECK_INT(w.sp.boom_count, 0);
    const SpaceMoon *m = space_find_moon(&w.sp, 3);
    CHECK_NOT_NULL(m);
    if (m) {
        CHECK_INT(m->planet, 4);
        CHECK(w.real - m->born > 1000.0);   /* no birth to show */
    }
}

static void test_a_new_window_is_born_in_view(void) {
    CASE("a new window is born where it can be seen");
    for (int seed = 1; seed <= 20; seed++) {
        World w;
        world_init(&w);
        w.sp.rng = (uint64_t)seed * 7919u;
        run(&w, 0.5);
        open_win(&w, 10, 0);
        frame(&w, 1.0 / 30.0, false);
        const SpaceMoon *m = space_find_moon(&w.sp, 10);
        CHECK_NOT_NULL(m);
        if (!m) continue;
        CHECK_DBL(m->born, w.real, 1e-9);
        double p[3];
        space_moon_pos(&w.sp, m, p);
        if (!on_screen(&w.cam.pose, p))
            T_FAIL("seed %d: the new moon is off screen", seed);
        t_checks++;
    }
}

static void test_closing_blows_the_moon_up(void) {
    CASE("closing an unfocused window blows its moon up");
    World w;
    world_init(&w);
    open_win(&w, 1, 0);
    open_win(&w, 2, 0);
    frame(&w, 1.0 / 30.0, true);
    close_win(&w, 2);
    frame(&w, 1.0 / 30.0, false);
    CHECK_INT(w.sp.moon_count, 1);
    CHECK_INT(w.sp.boom_count, 1);
    CHECK_NULL(space_find_moon(&w.sp, 2));
    run(&w, 10.0);
    CHECK_INT(w.sp.boom_count, 0);   /* and the smoke clears */
}

static void test_asleep_nothing_explodes(void) {
    CASE("a change made while asleep is just there on waking");
    World w;
    world_init(&w);
    open_win(&w, 1, 0);
    open_win(&w, 2, 0);
    frame(&w, 1.0 / 30.0, true);
    close_win(&w, 1);
    open_win(&w, 3, 0);
    frame(&w, 1.0 / 30.0, true);
    CHECK_INT(w.sp.boom_count, 0);
    CHECK_NULL(space_find_moon(&w.sp, 1));
    const SpaceMoon *m = space_find_moon(&w.sp, 3);
    CHECK_NOT_NULL(m);
    if (m) CHECK(w.real - m->born > 1000.0);
}

static void test_focus_flies_to_the_moon(void) {
    CASE("focusing a window flies the camera to its moon");
    World w;
    world_init(&w);
    open_win(&w, 1, 0);
    open_win(&w, 2, 0);
    frame(&w, 1.0 / 30.0, true);
    run(&w, 1.0);
    CHECK(!w.cam.flying);
    CHECK_INT((int)w.cam.at.moon, 0);
    w.focused = 2;
    frame(&w, 1.0 / 30.0, false);
    CHECK(w.cam.flying);
    run(&w, 8.0);
    CHECK(!w.cam.flying);
    CHECK_INT((int)w.cam.at.moon, 2);
    const SpaceMoon *m = space_find_moon(&w.sp, 2);
    if (m) {
        double p[3];
        space_moon_pos(&w.sp, m, p);
        double r = m->size * w.sp.planet[0].R;
        double d = dist3(w.cam.pose.eye, p) / r;
        CHECK(d > 6.0 && d < 12.0);       /* close up, 7..11 of its radii */
        CHECK(on_screen(&w.cam.pose, p));
    }
}

static void test_newborn_is_watched_before_flying(void) {
    CASE("a new focused window is watched being born first");
    World w;
    world_init(&w);
    frame(&w, 1.0 / 30.0, true);
    run(&w, 1.0);
    open_win(&w, 5, 0);
    w.focused = 5;
    frame(&w, 1.0 / 30.0, false);
    CHECK(!w.cam.flying);
    run(&w, 0.5);
    CHECK(!w.cam.flying);
    run(&w, 1.0);
    CHECK(w.cam.flying || w.cam.at.moon == 5);
}

static void test_closing_focus_backs_off_first(void) {
    CASE("closing the focused window backs the camera off, then follows focus");
    World w;
    world_init(&w);
    open_win(&w, 1, 0);
    open_win(&w, 2, 0);
    frame(&w, 1.0 / 30.0, true);
    w.focused = 2;
    run(&w, 8.0);
    CHECK_INT((int)w.cam.at.moon, 2);

    close_win(&w, 2);
    w.focused = 1;          /* fwm hands the focus on at once */
    frame(&w, 1.0 / 30.0, false);
    const SpaceMoon *m = space_find_moon(&w.sp, 2);
    CHECK_NOT_NULL(m);      /* not yet: the camera is up against it */
    if (m) CHECK(m->dying);
    CHECK(w.cam.flying);
    CHECK_INT((int)w.cam.to.moon, 0);   /* to the planet, not to window 1 */
    CHECK_INT(w.sp.boom_count, 0);

    run(&w, 0.6);
    CHECK_NULL(space_find_moon(&w.sp, 2));
    CHECK_INT(w.sp.boom_count, 1);
    CHECK_INT((int)(w.cam.flying ? w.cam.to.moon : w.cam.at.moon), 0);

    run(&w, 3.0);           /* the hold is over: on to the new focus */
    CHECK_INT((int)(w.cam.flying ? w.cam.to.moon : w.cam.at.moon), 1);
}

static void test_desktop_switch_flies_to_the_planet(void) {
    CASE("switching desktop flies to that planet");
    World w;
    world_init(&w);
    frame(&w, 1.0 / 30.0, true);
    run(&w, 1.0);
    w.desk = 6;
    frame(&w, 1.0 / 30.0, false);
    CHECK(w.cam.flying);
    run(&w, 8.0);
    CHECK(!w.cam.flying);
    CHECK_INT(w.cam.at.planet, 6);
    double p[3];
    space_planet_pos(&w.sp, 6, p);
    double d = dist3(w.cam.pose.eye, p) / w.sp.planet[6].R;
    CHECK(d > 4.0 && d < 8.5);   /* neptune is stood off further, rings and all */
}

static void test_never_inside_anything(void) {
    CASE("the camera never ends up inside a planet or a moon");
    World w;
    world_init(&w);
    for (int i = 0; i < 12; i++) open_win(&w, (uint32_t)(i + 1), i % FWM_DESKTOPS);
    frame(&w, 1.0 / 30.0, true);
    double worst_p = 1e9, worst_m = 1e9;
    for (int step = 0; step < 400; step++) {
        int r = (int)(w.sp.rng % 7u);
        if (step % 25 == 0) {
            if (r < 3) w.desk = (int)(w.sp.rng % FWM_DESKTOPS);
            else if (r < 6) w.focused = (uint32_t)(1 + w.sp.rng % 12u);
            else w.focused = 0;
        }
        frame(&w, 1.0 / 30.0, false);
        for (int j = 0; j < SPACE_PLANETS; j++) {
            double p[3];
            space_planet_pos(&w.sp, j, p);
            double d = dist3(w.cam.pose.eye, p) / w.sp.planet[j].R;
            if (d < worst_p) worst_p = d;
        }
        for (int i = 0; i < w.sp.moon_count; i++) {
            double p[3];
            space_moon_pos(&w.sp, &w.sp.moon[i], p);
            double d = dist3(w.cam.pose.eye, p) / (w.sp.moon[i].size * w.sp.planet[w.sp.moon[i].planet].R);
            if (d < worst_m) worst_m = d;
        }
    }
    CHECK(worst_p >= 1.59);
    CHECK(worst_m >= 2.9);
}

static void test_moved_window_moves_its_moon(void) {
    CASE("a window sent to another desktop takes its moon along");
    World w;
    world_init(&w);
    open_win(&w, 1, 0);
    frame(&w, 1.0 / 30.0, true);
    w.win[0].desktop = 3;
    frame(&w, 1.0 / 30.0, false);
    const SpaceMoon *m = space_find_moon(&w.sp, 1);
    CHECK_NOT_NULL(m);
    if (m) CHECK_INT(m->planet, 3);
    CHECK_INT(w.sp.boom_count, 0);
}

static void test_planet_swap_runs_its_wave(void) {
    CASE("a planet changing kind runs a wave and resizes along with it");
    World w;
    world_init(&w);
    frame(&w, 1.0 / 30.0, true);
    w.cfg.planets[0] = 9;   /* terra to jupiter */
    space_apply_config(&w.sp, &w.cfg, w.real, false);
    CHECK_INT(w.sp.planet[0].kind, 9);
    CHECK_INT(w.sp.planet[0].prev, 0);
    run(&w, 1.6);
    CHECK(w.sp.planet[0].prog > 0.3 && w.sp.planet[0].prog < 0.7);
    CHECK(w.sp.planet[0].R > 1.0 && w.sp.planet[0].R < 3.0);
    run(&w, 2.0);
    CHECK_DBL(w.sp.planet[0].prog, 1.0, 1e-9);
    CHECK_DBL(w.sp.planet[0].R, 3.0, 1e-9);
    CHECK_INT(w.sp.planet[0].prev, 9);

    w.cfg.planets[1] = 4;
    space_apply_config(&w.sp, &w.cfg, w.real, true);   /* nobody looking: at once */
    CHECK_DBL(w.sp.planet[1].prog, 1.0, 1e-9);
    CHECK_DBL(w.sp.planet[1].R, 0.8, 1e-9);
}

static void test_star_swap_flashes(void) {
    CASE("a star swap shrinks, flashes, grows");
    World w;
    world_init(&w);
    frame(&w, 1.0 / 30.0, true);
    SpaceUniforms u;
    space_uniforms(&w.sp, &w.cam, &w.cfg, w.real, ASPECT, &u);
    CHECK_DBL(u.star_flash, 0.0, 1e-6);
    CHECK_DBL(u.star_type, 0.0, 1e-6);
    w.cfg.star = 5;
    space_apply_config(&w.sp, &w.cfg, w.real, false);
    double t0 = w.real;
    space_uniforms(&w.sp, &w.cam, &w.cfg, t0 + 0.5, ASPECT, &u);
    CHECK_DBL(u.star_type, 0.0, 1e-6);   /* still the old one, shrinking */
    CHECK(u.star_r < 1.2f);
    space_uniforms(&w.sp, &w.cam, &w.cfg, t0 + 0.96, ASPECT, &u);
    CHECK(u.star_flash > 0.9f);
    space_uniforms(&w.sp, &w.cam, &w.cfg, t0 + 3.0, ASPECT, &u);
    CHECK_DBL(u.star_type, 5.0, 1e-6);
    CHECK_DBL(u.star_flash, 0.0, 1e-6);
    CHECK_DBL(u.star_r, 0.8, 1e-6);
}

static void test_busy_only_when_something_happens(void) {
    CASE("the full frame rate only while something is happening");
    World w;
    world_init(&w);
    open_win(&w, 1, 0);
    frame(&w, 1.0 / 30.0, true);
    run(&w, 1.0);
    CHECK(!space_busy(&w.sp, &w.cam, w.real));    /* just turning */
    open_win(&w, 2, 0);
    frame(&w, 1.0 / 30.0, false);
    CHECK(space_busy(&w.sp, &w.cam, w.real));     /* a birth */
    run(&w, 4.0);
    CHECK(!space_busy(&w.sp, &w.cam, w.real));
    w.desk = 3;
    frame(&w, 1.0 / 30.0, false);
    CHECK(space_busy(&w.sp, &w.cam, w.real));     /* a flight */
    run(&w, 8.0);
    CHECK(!space_busy(&w.sp, &w.cam, w.real));
}

static void test_uniforms_are_finite(void) {
    CASE("everything handed to the shader is a number");
    World w;
    world_init(&w);
    for (int i = 0; i < 30; i++) open_win(&w, (uint32_t)(i + 1), i % FWM_DESKTOPS);
    frame(&w, 1.0 / 30.0, true);
    CHECK_INT(w.sp.moon_count, SPACE_MAX_MOONS);   /* the rest wait for room */
    w.focused = 3;
    run(&w, 2.0);
    SpaceUniforms u;
    space_uniforms(&w.sp, &w.cam, &w.cfg, w.real, ASPECT, &u);
    const float *f = (const float *)&u;
    int bad = 0;
    for (size_t i = 0; i < sizeof(u) / sizeof(float); i++)
        if (!isfinite(f[i])) bad++;
    CHECK_INT(bad, 0);
}

int main(void) {
    test_windows_already_open_are_just_there();
    test_a_new_window_is_born_in_view();
    test_closing_blows_the_moon_up();
    test_asleep_nothing_explodes();
    test_focus_flies_to_the_moon();
    test_newborn_is_watched_before_flying();
    test_closing_focus_backs_off_first();
    test_desktop_switch_flies_to_the_planet();
    test_never_inside_anything();
    test_moved_window_moves_its_moon();
    test_planet_swap_runs_its_wave();
    test_star_swap_flashes();
    test_busy_only_when_something_happens();
    test_uniforms_are_finite();
    return t_report("space");
}
