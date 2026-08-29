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

#ifndef FWM_SNAPSHOT_H
#define FWM_SNAPSHOT_H

#include <stdbool.h>

struct FwmServer;
struct FwmOutput;
struct wlr_buffer;
struct wlr_texture;
struct wlr_scene_node;

/* Photographing a piece of the scene graph into a buffer of our own.
 *
 * Deforming a client's own buffer is wrong for anything that paints through
 * subsurfaces: their content lives in a different buffer entirely and is simply
 * absent, while the toplevel's ARGB alpha gets blended over the hole (Firefox
 * turned see-through during an impact; kitty, which has no subsurfaces, never
 * did). So the picture is composited the way the compositor would draw it.
 *
 * All public wlroots API — no raw GLES, no scene-graph internals.
 * (wlr_scene_node_snapshot does not exist in 0.20; if a future wlroots grows
 * one, it replaces this wholesale.) */

/* An empty ARGB8888 buffer the renderer can draw into. NULL if the allocator
 * cannot serve it. The caller owns the reference. */
struct wlr_buffer *snapshot_alloc(struct FwmServer *server, int w, int h);

/* Composite `node`'s subtree into `dst`, which is cleared to transparent first.
 * Layout point (origin_x, origin_y) lands on the buffer's top-left and
 * everything is scaled by `scale` about it, so a whole desktop fits into a
 * buffer a fraction of its size. Content outside `dst` is clipped by the pass.
 *
 * Scaling here rather than at draw time is what keeps the expo strip's memory
 * bounded: ten screen-sized cards at full resolution is most of a hundred
 * megabytes, and the strip never shows them larger than a third of a screen. */
bool snapshot_subtree(struct FwmServer *server, struct wlr_buffer *dst,
                      struct wlr_scene_node *node,
                      int origin_x, int origin_y, double scale);

/* The whole desktop as it looks right now — wallpaper, windows, layer-shell
 * background and bottom — and nothing that is screen furniture: no tray, no
 * panels, no lock. Used for the slide across the ring's join, where a
 * photograph of the desktop being left has to travel off one side while the
 * one arriving comes in the other.
 *
 * The wallpaper is drawn from the copy it keeps rather than from the layer on
 * screen: the live one is a cairo overlay whose pixels are gone by then, which
 * is why the first version of this came out black behind the windows. */
bool snapshot_world(struct FwmServer *server, struct FwmOutput *out,
                    struct wlr_buffer *dst);

/* One rectangle of `tex`, copied into a buffer of its own at 1:1 and with no
 * filtering. The caller owns the reference that comes back; NULL if the
 * allocator or the pass would not serve it.
 *
 * For the resize rubber's edge fill, and the reason it exists rather than the
 * scene's own source box: a scene buffer told to sample a one-texel strip and
 * draw it two hundred pixels tall did not stay inside that strip — the band
 * came out solid for the first stretch and then bled into whatever lies past
 * the row. Cutting the strip out into a texture that IS the strip leaves the
 * sampler nowhere else to go, whatever it does with the coordinates. */
struct wlr_buffer *snapshot_rect(struct FwmServer *server, struct wlr_texture *tex,
                                 int sx, int sy, int w, int h);

/* One WINDOW onto the desktop, at layout position (lx, ly), the size of `dst`:
 * wallpaper first, then everything the scene graph draws over it.
 *
 * This is snapshot_world's trick — the wallpaper comes from the copy it keeps,
 * because the layer on screen is a cairo overlay whose pixels the scene has
 * already taken — applied to a piece rather than the whole screen. Without it
 * a photograph of the desktop comes back with the windows in it and nothing
 * behind them, which is exactly what a lens then bends: a film of windows over
 * an unbent wallpaper.
 *
 * `hide` (may be NULL) is switched off for the duration, for the caller that
 * must not photograph itself. */
bool snapshot_lens(struct FwmServer *server, struct FwmOutput *out,
                   struct wlr_buffer *dst, int lx, int ly,
                   struct wlr_scene_node *hide);

#endif /* FWM_SNAPSHOT_H */
