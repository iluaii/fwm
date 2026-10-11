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

#ifndef FWM_LIQUID_DRAW_H
#define FWM_LIQUID_DRAW_H

#include <stdbool.h>
#include <wlr/util/box.h>

struct FwmServer;
struct wlr_renderer;
struct wlr_buffer;

/* The picture of a liquid: its particles turned back into one body.
 *
 * The particles are how liquid.c computes it and never what anyone should
 * see. So they are drawn twice, both times on the GPU through the door
 * rotate.c opened: first each one as a soft round splat, added together into
 * a picture of how much liquid is where (at half resolution — it is a smooth
 * field, and half the pixels are a quarter of the cost); then that field is
 * cut at a level, and everything above it is liquid. Particles close together
 * melt into one surface, a lone one is a drop, and the edge is wherever the
 * field crosses the level — which is smooth, because the field is.
 *
 * A splat is wider than the gap between particles on purpose: as wide as a
 * grid cell of the simulation and half again. The simulation cannot see
 * anything smaller than a cell, so neither may the picture — narrower, and a
 * fast sheet whose particles the flow had strung out into rows read as a
 * comb, gaps the liquid itself did not know it had.
 *
 * Unavailable on the Vulkan and pixman renderers, like everything else that
 * draws through rotate.c's door; the liquid then still runs, unseen. */
typedef struct FwmLiquidPaint FwmLiquidPaint;

bool liquid_paint_supported(struct wlr_renderer *renderer);

/* Buffers for a `w` x `h` px box. NULL if they cannot be had. */
FwmLiquidPaint *liquid_paint_create(struct FwmServer *server, int w, int h);
void liquid_paint_destroy(FwmLiquidPaint *paint);
int liquid_paint_width(const FwmLiquidPaint *paint);
int liquid_paint_height(const FwmLiquidPaint *paint);

/* Paint `count` particles — (x, y) pairs, px inside the box — `spacing` px
 * apart at rest, each of liquid kinds[i] (0 .. LIQUID_PAINT_KINDS - 1), in
 * that liquid's colour out of `palette` (premultiplied, as parse_hex_color
 * leaves it). Where liquids meet, the colour is as much of each as there is
 * of it.
 *
 * Returns the buffer now holding the picture, owned by the paint and valid
 * until the next call, or NULL if nothing could be drawn. `damage` is the part
 * of the box that may differ from the previous picture: what the liquid
 * covered then and what it covers now. */
#define LIQUID_PAINT_KINDS 8
struct wlr_buffer *liquid_paint_draw(FwmLiquidPaint *paint, const float *xy,
                                     const float *kinds, int count, double spacing,
                                     const float palette[LIQUID_PAINT_KINDS][4],
                                     struct wlr_box *damage);

/* The last picture painted, or NULL. */
struct wlr_buffer *liquid_paint_current(const FwmLiquidPaint *paint);

/* Forget the shader programs. Called when the renderer is replaced or goes
 * away; nothing is deleted, because by then the context they lived in is gone
 * (star_gl_finish explains). */
void liquid_paint_finish(void);

#endif /* FWM_LIQUID_DRAW_H */
