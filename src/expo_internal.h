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

#ifndef FWM_EXPO_INTERNAL_H
#define FWM_EXPO_INTERNAL_H

#include "expo.h"
#include "server.h"
#include "view.h"
#include "rotate.h"
#include "star.h"
#include "defines.h"
#include <time.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

/* The desktop strip, shared between the four files it is split across:
 *
 *   expo.c        opening, closing, the frame tick, and the camera policy
 *                 that says when the view may leave its canonical seat
 *   expo_geom.c   the ring in space: where a desktop is, where a point on it
 *                 lands on screen, and the ray that takes a click back
 *   expo_cards.c  the pictures — taking them, keeping the front one alive,
 *                 and following windows that come and go
 *   expo_draw.c   turning all that into a frame, on either rendering path
 *
 * Nothing here is public: expo.h is what the rest of the compositor sees. */

/* One window on the strip. The card is a still picture: the live subtree it was
 * taken from is hidden for as long as the strip is up. */
typedef struct {
    FwmView *view;
    /* The client buffer the picture was taken from, and when. A window that has
     * not drawn since is not worth photographing again. */
    struct wlr_buffer *seen;
    double snapped;
    struct wlr_scene_buffer *node;   /* flat path only */
    struct wlr_texture *tex;         /* curved path only */
    struct wlr_buffer *buf;          /* our lock on the snapshot */
    double wx, wy;                   /* world position, in real (1:1) px */
    /* Moon duty, in the orrery: where this window is round its desktop and how
     * far out. Kept apart from wx/wy because those are where the window IS —
     * what a drag reads and where a drop lands — while this is only where it
     * is drawn. */
    double moon_a, moon_r, moon_rate;
    int w, h;                        /* world size */
    int desktop;
} ExpoItem;

struct FwmExpo {
    FwmServer *server;
    /* The monitor the strip belongs to: it opens on the screen the pointer is
     * on, shows that screen's desktops and lands its choice there. */
    FwmOutput *out;

    struct wlr_scene_tree *tree;
    struct wlr_scene_rect *backdrop;
    struct wlr_scene_rect *hilight;             /* frame under the hovered card */
    /* One card per desktop: a tree holding the card's edge, the slot's own dark
     * base and, on top of both, one node per wallpaper layer cropped to that
     * desktop. The edge is simply a rect the card sits on top of, so only its
     * margin is ever seen. */
    struct wlr_scene_tree *cards[FWM_DESKTOPS];
    struct wlr_scene_rect *card_edge[FWM_DESKTOPS];
    struct wlr_scene_rect *card_base[FWM_DESKTOPS];
    struct wlr_scene_buffer *card_layers[FWM_DESKTOPS][EXPO_MAX_WP_LAYERS];
    int card_layer_n[FWM_DESKTOPS];

    /* Fixed slots for the same reason the rest of fwm uses them: no allocation
     * on a path that runs while the compositor is drawing. */
    ExpoItem items[MAX_WINDOWS];
    int n_items;

    /* The strip's own camera. `zoom` is how many desktops fit across the
     * screen — 1.0 is exactly what you were looking at a moment ago, which is
     * what makes entering and leaving a single continuous movement. */
    double zoom, zoom_target;
    /* How far open the seam is: 1 while the strip is a line, 0 once the ring is
     * closed, eased between the two so `x` brings the two ends together
     * instead of teleporting one of them. */
    double seam, seam_target;
    /* Free horizontal offset from the home desktop, strip px. Eased like the
     * zoom: an arrow key that teleported the strip sideways left you with no
     * idea which way it had gone. */
    double pan, pan_target;
    /* The orbit: how far the camera is lifted above the ring and how far it is
     * pulled back. Both are DEVIATIONS — they ease back to level and 1 the
     * moment the strip is asked to do anything else, because the property
     * worth protecting is that the strip and the live screen coincide exactly
     * at zoom 1.0. Only offered at the far zoom step: near the strip is
     * something you work in, and a tilted desktop is not something to work in.
     */
    double tilt, tilt_target;
    double dist, dist_target;
    int orbiting;                    /* middle button, or alt+left, held */
    double orbit_x, orbit_y;         /* cursor at the last orbit sample */
    double spin;                     /* strip px/s still to coast, after a throw */
    /* Orrery mode: the ring turns by itself, seen from above, with a star in
     * the middle of it.
     *
     * Everything it needs already existed — the desktops have been a ring
     * since `toggle_wrap`, the camera has been able to lift off it since the
     * tilt went in, and the ring has been able to coast since it could be
     * thrown. This adds the one thing missing: something driving it, so the
     * desktops go round instead of waiting to be pushed. */
    int orrery;
    double orrery_speed;             /* strip px/s the ring is driven at */
    /* What sits in the middle of the ring. Its own object, not the one on the
     * desktops: the orrery is a thing to look AT, and it should have a star in
     * it whether or not [star] put one on a desktop. Config of its own too,
     * because the size that suits a desktop is not the size that suits the
     * centre of a circle of ten of them. */
    FwmStar orrery_star;
    StarConfig orrery_cfg;
    struct FwmStarDraw *orrery_draw;
    /* Its size, as a multiplier on [star] orrery_size, and how far its disc is
     * tilted out of the ring's plane. Both are the user's to set from inside
     * the strip: what looks right depends on the monitor and on how far back
     * the camera is, and neither is knowable from a config file. */
    double orrery_scale;
    double orrery_tilt;
    double orrery_roll;   /* which way the disc's plane is turned, radians */
    /* Orbits: where each desktop is round its own one, in strip px, and how
     * far out that orbit is as a fraction of the ring's radius. Planets do not
     * share a track — that is the difference between an orrery and a
     * carousel. */
    double orbit_phase[FWM_DESKTOPS];
    double orbit_r[FWM_DESKTOPS];
    int orbits;           /* desktops ride their own orbits, rather than a ring */
    /* How far along that change is, 0..1. Desktops leaving one shared ring for
     * ten separate orbits is a big rearrangement, and done in one frame it
     * reads as a glitch rather than as a manoeuvre. */
    double orbit_blend, orbit_blend_target;
    double clock;                    /* seconds the strip has been open */
    double orbit_dt;                 /* seconds since the last orbit sample */
    struct timespec orbit_time;
    int home;            /* desktop the strip was entered from */
    int leaving;         /* animating out; torn down when the zoom lands */

    /* The curved path. A scene node is an axis-aligned rectangle and a turned
     * card is not, so when the renderer can do it (GLES2 — see rotate.h) the
     * whole strip is drawn by hand into one buffer instead, and `canvas` is the
     * single node that shows it. Two buffers, alternately: the scene may still
     * be reading last frame's while this one is drawn.
     *
     * When it cannot (Vulkan, pixman), `gl` stays false, the scene nodes above
     * are used, and expo_openness is forced to leave the strip flat. Everything
     * else — the geometry, the input, the animation — is shared. */
    bool gl;
    struct wlr_scene_buffer *canvas;
    struct wlr_buffer *canvas_buf[2];
    int canvas_i;
    /* What the canvas currently shows. Handing the scene a buffer damages the
     * whole screen, which schedules another frame, which would redraw and
     * damage again: a strip standing perfectly still would pin the machine at
     * 60fps forever. The flat path has no such problem — setting a node to the
     * position it already has is a no-op in wlr_scene — so this is the price of
     * drawing by hand, and it is one comparison. */
    double drawn_zoom, drawn_seam, drawn_pan, drawn_drag_x, drawn_drag_y;
    double drawn_tilt, drawn_dist;
    int drawn_cam, drawn_items, drawn_wrap;
    void *drawn_hover, *drawn_drag;
    struct wlr_texture *wp_tex[EXPO_MAX_WP_LAYERS];
    struct wlr_fbox wp_crop[FWM_DESKTOPS][EXPO_MAX_WP_LAYERS];
    int wp_n;

    ExpoItem *hover;
    ExpoItem *drag;
    double drag_off_x, drag_off_y;   /* cursor to card top-left, in world px */

    /* Right-click menu. It is parented to layer_overlay, not to the strip's own
     * tree, so it stays above the tray and does not scale with the cards. */
    struct wlr_scene_buffer *menu;
    /* The key hints along the bottom. Parented to layer_overlay with the menu,
     * so they neither scale with the cards nor hide under them. */
    struct wlr_scene_buffer *hints;
    /* Where the bar is (0 hidden, 1 in place), where it is heading, and how
     * long it has left before it takes itself away. */
    double hints_reveal, hints_reveal_target, hints_timer;
    ExpoItem *menu_item;
};



/* A point in ring space: x right, y down, z toward the viewer, with the front
 * of the strip at the origin. */
typedef struct { double x, y, z; } ExpoPt;

/* ── expo.c ───────────────────────────────────────────────────────────── */
void expo_menu_close(FwmExpo *e);
/* Start leaving, landing on `desktop`. The strip goes on animating until the
 * camera and the zoom are both home; see the collapse test in expo_tick. */
void expo_close(FwmServer *server, int desktop);
void expo_camera_home(FwmExpo *e);
bool expo_can_orbit(FwmExpo *e);

/* ── expo_geom.c ──────────────────────────────────────────────────────── */
double expo_openness(FwmExpo *e);
double expo_gap(FwmExpo *e);
double expo_pitch(FwmExpo *e);
double expo_scale(FwmExpo *e);
double expo_center(FwmExpo *e);
double expo_lap(FwmExpo *e);
double expo_radius(FwmExpo *e);
double expo_dist(FwmExpo *e);
double expo_focal(FwmExpo *e);
ExpoPt expo_ring_point(FwmExpo *e, double u, double wy);
ExpoPt expo_facet_point(FwmExpo *e, ExpoPt a, ExpoPt b, double s, double wy);
struct scene3d_vert expo_project(FwmExpo *e, ExpoPt p, double u, double v);
double expo_desktop_strip_x(FwmExpo *e, int desktop);
double expo_offset(FwmExpo *e, double wx, int desktop);
void expo_facet_ends(FwmExpo *e, int desktop, ExpoPt *a, ExpoPt *b);
void expo_to_screen(FwmExpo *e, double wx, double wy, int desktop,
                    double *sx, double *sy);
bool expo_point(FwmExpo *e, double sx, double sy,
                int *desktop, double *wx, double *wy);

/* ── expo_cards.c ─────────────────────────────────────────────────────── */
bool expo_resnap_item(FwmExpo *e, ExpoItem *it);
void expo_build_cards(FwmExpo *e);
bool expo_add_item(FwmExpo *e, FwmView *view);
void expo_snap_windows(FwmExpo *e);
void expo_sync_items(FwmExpo *e);
void expo_live_pass(FwmExpo *e, double now);

/* ── expo_draw.c ──────────────────────────────────────────────────────── */
/* The orrery's hole: its view depth (0 when there is none), and drawing it as
 * a billboard at the centre of the ring. Both live in expo_draw.c, and the
 * depth is separate so the card loop can sort it in without drawing it. */
double expo_orrery_depth(FwmExpo *e);
void expo_draw_orrery(FwmExpo *e);

/* Work out which orbit each desktop rides and where its windows sit round it.
 * Driven by how many windows a desktop has. */
void expo_orbits_layout(FwmExpo *e);

void expo_layout(FwmExpo *e);
void expo_canvas_dirty(FwmExpo *e);
void expo_set_world_visible(FwmServer *server, FwmOutput *out, bool visible);

/* ── expo_input.c ─────────────────────────────────────────────────────── */
void expo_clamp_pan(FwmExpo *e);
void expo_drag_to(FwmExpo *e, double lx, double ly);

#endif /* FWM_EXPO_INTERNAL_H */
