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

#ifndef FWM_CAIRO_OVERLAY_H
#define FWM_CAIRO_OVERLAY_H

#include <stdbool.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <wlr/types/wlr_scene.h>

struct wlr_scene_buffer *cairo_overlay_create(struct wlr_scene_tree *parent, int width, int height);
void cairo_overlay_update(struct wlr_scene_buffer *scene_buffer,
                          void (*draw_func)(cairo_t *cr, int w, int h, void *data), void *user_data);
void cairo_overlay_destroy(struct wlr_scene_buffer *scene_buffer);

/* Fade the overlay in while it rises `rise_px` into place, instead of simply
 * appearing. Call AFTER positioning it: the current node position is taken as
 * the resting one. Safe on a make_static'd overlay — this animates the scene
 * node, it never redraws. */
void cairo_overlay_animate_in(struct wlr_scene_buffer *scene_buffer,
                              double duration_ms, double rise_px);

/* Exact mirror of animate_in: fade out while sinking `sink_px`, then DESTROY
 * the overlay and call on_done (may be NULL). Ownership of `scene_buffer`
 * passes to the animation — the caller must drop its pointer immediately and
 * never draw into it again. on_done exists so the caller can clear whatever
 * pointer it kept. Falls back to an immediate destroy if duration_ms <= 0. */
void cairo_overlay_animate_out(struct wlr_scene_buffer *scene_buffer,
                               double duration_ms, double sink_px,
                               void (*on_done)(void *), void *user_data);

/* Advance every running overlay animation. Call once per frame. */
void cairo_overlay_tick(double dt);

/* True while any overlay animation is running, so the compositor knows it must
 * keep driving the frame loop. */
bool cairo_overlay_animating(void);

/* Replace the overlay's pixels with a centre-crop of a raw BGRA image, without
 * going through cairo. Used by the video wallpaper to push a decoded frame
 * every tick: it copies the overlay's own width*height starting at (src_x,
 * src_y) out of `src` (whose row stride is `src_stride`), so the source may be
 * larger than the overlay (a scaled-to-cover frame). BGRA byte order matches
 * the overlay's ARGB8888 backing on little-endian. */
void cairo_overlay_blit_bgra(struct wlr_scene_buffer *scene_buffer,
                             const unsigned char *src, int src_stride,
                             int src_x, int src_y);

/* For overlays that are drawn once and never updated again (wallpaper layers,
 * welcome, hints): free the CPU-side pixel copy and keep only the GPU texture.
 * Do not call cairo_overlay_update() on the overlay afterwards. */
void cairo_overlay_make_static(struct wlr_scene_buffer *scene_buffer);

#endif /* FWM_CAIRO_OVERLAY_H */
