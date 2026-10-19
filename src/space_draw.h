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

#ifndef FWM_SPACE_DRAW_H
#define FWM_SPACE_DRAW_H

#include <stdbool.h>

#include "space.h"
#include "space_gl.h"

struct FwmServer;
struct wlr_scene_tree;

/* One monitor's picture of the solar system: a scene buffer over the
 * wallpaper, drawn by space_gl.c into one of two buffers in turn.
 *
 * The buffers are a fraction ([space] scale) of the screen and the node is
 * stretched over the whole of it. That is the whole of the cost control: the
 * shader is the most expensive thing fwm draws, at full resolution a third of
 * a mid-range GPU at 30 frames a second, and the wallpaper is mostly seen
 * through a translucent terminal, where half the pixels are not missed. */
typedef struct FwmSpaceDraw FwmSpaceDraw;

/* NULL if the renderer is not GLES2 or no buffer could be had; the wallpaper
 * under it then simply stays. */
FwmSpaceDraw *space_draw_create(struct FwmServer *server, struct wlr_scene_tree *parent,
                                int w, int h, double scale);

/* Whether this draw was made for a w x h screen at this scale. */
bool space_draw_fits(const FwmSpaceDraw *d, int w, int h, double scale);

/* Where the screen is in the layout. */
void space_draw_place(FwmSpaceDraw *d, int x, int y);

/* Draw one frame and hand it to the scene. PENDING while the shader is still
 * being built: nothing is shown yet, and the wallpaper under it stays. */
SpaceGlResult space_draw_render(FwmSpaceDraw *d, const SpaceUniforms *u);

/* Over everything else in the background layer (the wallpaper is rebuilt on
 * top of its tree every time one is picked). */
void space_draw_raise(FwmSpaceDraw *d);

void space_draw_destroy(FwmSpaceDraw *d);

#endif /* FWM_SPACE_DRAW_H */
