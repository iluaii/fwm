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

#ifndef FWM_STAR_DRAW_H
#define FWM_STAR_DRAW_H

#include <stdbool.h>
#include <wlr/types/wlr_scene.h>

#include "config.h"
#include "star.h"

/* The picture of the star: what star.c's numbers look like.
 *
 * Same split the sun already makes — sun.c works out the light and shadow.c
 * draws what it does — so everything here is cairo and scene nodes and nothing
 * here decides anything.
 *
 * One of these per monitor, like the grass, because a star stands in the WORLD
 * and a monitor shows a moving window onto it: the same star is drawn by every
 * screen looking at its desktop, each at its own camera offset, and by none of
 * the others. The buffer is sized for the star at its largest and never
 * resized — the collapse shrinks what is drawn inside it, not the canvas,
 * which is what keeps a squeeze from reallocating sixty times a second. */
typedef struct FwmStarDraw FwmStarDraw;

/* A node in `behind`, sized from the burning radius in `cfg`. NULL if there is
 * nothing to draw or the buffer cannot be had; every caller carries on.
 *
 * `behind` may be NULL, which asks for no scene node at all: the star is drawn
 * into a buffer and handed out as a texture (star_draw_texture) for a caller
 * that is doing its own 3D. That is what the orrery needs — inside a ring of
 * desktops, a star has to be sorted among them by depth like everything else,
 * and a flat node in the scene graph can only ever be wholly in front of them
 * or wholly behind. */
struct FwmServer;
/* `behind` is where a star lives — in the world, under the windows. `front` is
 * where a HOLE has to live: it bends what is behind it, and it cannot bend
 * what is drawn over it. The node moves between the two when the star
 * collapses. */
struct FwmOutput;
FwmStarDraw *star_draw_create(struct FwmServer *server, struct FwmOutput *out,
                              struct wlr_scene_tree *behind,
                              struct wlr_scene_tree *front, const StarConfig *cfg);
void star_draw_destroy(FwmStarDraw *draw);

/* GPU reset recovery, in two halves either side of the renderer swap: release
 * lets go of everything that lived in the dying context, rebuild allocates
 * again from the new one. Rebuild returning false means the shader path could
 * not be restored and the star has to be dropped. */
void star_draw_gpu_release(FwmStarDraw *draw);
bool star_draw_gpu_rebuild(FwmStarDraw *draw);

/* Redraw for the state `star` is in now, and put it where a monitor whose
 * camera is at `camera_x` and whose top-left is at (origin_x, origin_y) would
 * see it. Cheap to call every frame: the picture is only actually repainted
 * when it has changed enough to see, and never more than 30 times a second. */
void star_draw_update(FwmStarDraw *draw, const FwmStar *star,
                      const StarConfig *cfg, double now_s,
                      int camera_x, int origin_x, int origin_y);

/* Show or hide it — a monitor looking at another desktop draws no star. */
void star_draw_set_visible(FwmStarDraw *draw, bool visible);

/* Whether a hole bends the DESKTOP behind it, or its own starfield.
 *
 * On the desktop it must bend the desktop — that is the whole effect. In the
 * orrery it must not: the strip hides the world it is showing a picture of, so
 * photographing the scene there returns nothing and the hole helpfully paints
 * that nothing over the ring. A hole against stars is also simply the better
 * picture when it is the thing being looked at. */
void star_draw_set_lensing(FwmStarDraw *draw, bool on);

/* What the star is standing in FRONT of, when that is not a desktop.
 *
 * The orrery's star has no desktop behind it — the strip has hidden the world
 * it is drawing cards of — and it therefore bent nothing at all: the
 * procedural sky it falls back on is a scatter of points, and moving points
 * does not read as space bending. What is really behind it is the far half of
 * the ring, and that exists only inside expo's own 3D pass.
 *
 * So expo captures the pass mid-flight (scene3d_capture) and hands the texture
 * here, with `u0,v0,du,dv` saying where this star's canvas lands on it. The
 * capture is taken BEFORE the star is drawn, so it can never contain the star:
 * no feedback, at the cost of the picture being one frame old, which on a ring
 * that takes seconds to turn is not visible.
 *
 * The rectangle is in ordinary screen orientation, v measured from the TOP,
 * and that is worth stating because it looks like it should not be. Both ends
 * of the trip live in the same convention: scene3d maps screen y=0 to the
 * bottom of clip space (see scene3d_fill), so the capture's first row is the
 * top of the screen, and the star's canvas is written from its own fragment
 * coordinates into a buffer whose first row expo then draws at the top of the
 * billboard. Two identical conventions, nothing to cancel. Flipping v "to be
 * safe" mirrors the ring inside the lens. Pass tex 0 to stop using one. */
void star_draw_set_ring(FwmStarDraw *draw, unsigned tex,
                        float u0, float v0, float du, float dv);

/* How open the accretion disc is drawn, 0 edge-on .. 1 face-on. The orrery
 * feeds the camera's own tilt in here so that flying around the ring takes the
 * hole with it. */
void star_draw_set_disc_tilt(FwmStarDraw *draw, double tilt);

/* Which way round the disc's plane lies, radians. Tilt says how squashed it
 * looks; this says which way it is turned — without it the long axis of the
 * disc is pinned horizontal and the thing cannot be rotated at all. */
void star_draw_set_disc_roll(FwmStarDraw *draw, double roll);

/* Keep it above the wallpaper when the background layer is rebuilt. */
void star_draw_raise(FwmStarDraw *draw);

/* Put it behind everything else in its parent — for the orrery, where the
 * desktops orbit AROUND it and must pass in front. */
void star_draw_lower(FwmStarDraw *draw);

/* The last frame drawn, as a texture. NULL until one has been. Only useful for
 * a node-less star; owned by the FwmStarDraw and valid until the next update. */
struct wlr_texture *star_draw_texture(FwmStarDraw *draw);

/* How wide the buffer is, in px — what a caller placing the texture itself
 * needs in order to size its quad. */
int star_draw_side(const FwmStarDraw *draw);

/* Half the width the buffer stands for, in WORLD units. Not the same as half
 * its resolution: past a cap the picture is painted smaller and stretched, so
 * a caller placing the quad must ask for this rather than measure the pixels. */
double star_draw_extent(const FwmStarDraw *draw);

#endif /* FWM_STAR_DRAW_H */
