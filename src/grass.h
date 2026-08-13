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

#ifndef FWM_GRASS_H
#define FWM_GRASS_H

#include <stdbool.h>
#include <wlr/types/wlr_scene.h>
#include "config.h"

/*
 * The grass along the bottom of a monitor.
 *
 * One instance per output, sized and positioned like the wallpaper is: a
 * monitor's grass stands on the bottom edge of THAT monitor, which is also
 * where the floor of the desktop it shows is (physics.c, floor_y_for). A single
 * screen-wide row like cava's would sit at the wrong height on the shorter of
 * two mixed-resolution screens.
 *
 * Unlike cava this is a cairo overlay rather than a row of scene rects: a blade
 * that bends is a filled curve, and wlr_scene_rect cannot lean. The cost is a
 * strip-sized ARGB upload per redraw, so the strip is redrawn only when
 * something changes — for now, only the config.
 *
 * The blades themselves are generated once, at create, and kept: their
 * positions, lengths and shades must not shuffle when the strip is repainted
 * for a colour change, and a blade needs somewhere to keep its bend once the
 * wind arrives.
 */
typedef struct FwmGrass FwmGrass;

/* Grow a strip `width` px wide against a monitor `screen_h` px tall. `parent`
 * should be the background layer: the grass sits above the wallpaper and below
 * the windows. Returns NULL when [grass] is off or nothing can be allocated.
 * The caller positions the result with grass_set_origin. */
FwmGrass *grass_create(struct wlr_scene_tree *parent, const GrassConfig *cfg,
                       int width, int screen_h);

/* Put the strip on its monitor, in layout coordinates: `x` is the monitor's
 * left edge and `bottom_y` its BOTTOM one (box.y + box.height), because that
 * is the line the blades are rooted on and the strip's own height is this
 * module's business, not the caller's. */
void grass_set_origin(FwmGrass *g, int x, int bottom_y);

/* One window, as the grass sees it: a span of the strip with a bottom edge at
 * some height above the ground, moving sideways at some speed.
 *
 * Strip-local pixels, so the caller does the camera and the monitor offset once
 * and this module never learns what a desktop is. `bottom` is measured UP from
 * the line the blades are rooted on: a window resting on the floor is at 0, one
 * hovering above the grass is at the height of its lower edge, and anything at
 * or above the tallest blade touches nothing. */
typedef struct {
    float x0, x1;   /* left and right edge, px from the left of the strip */
    float bottom;   /* px the window's lower edge is above the ground line */
    float vx;       /* px/s, signed: which way it is travelling */
} GrassWindow;

/* Hand over the windows standing in this strip, for this tick. Copied, and
 * whatever was here before is replaced — a window that has flown away simply
 * stops being in the list and the blades it was holding down spring back.
 * `count` is clamped internally; a screen with more windows than that in the
 * grass has bigger problems than which ones bend it. */
void grass_set_windows(FwmGrass *g, const GrassWindow *win, int count);

/* Blow the wind on for `dt` seconds and repaint if a repaint is due (the strip
 * is one buffer, so it is redrawn at [grass] fps rather than at the frame
 * rate). Returns true while anything is still moving, so the caller keeps the
 * frame loop at the tick rate instead of dropping to the idle heartbeat — a
 * still lawn (wind = 0, everything settled) says false and costs nothing.
 *
 * `cfg` is re-read every call, so `fwmctl set grass.wind` takes effect live;
 * the knobs that change the blades themselves cannot be applied this way and
 * are what grass_stale reports instead. */
bool grass_tick(FwmGrass *g, const GrassConfig *cfg, double dt);

/* Stop the wind while nothing can see the strip — a fullscreen window covering
 * this monitor, the same condition that pauses a video wallpaper. A paused
 * patch stops ticking and stops counting as moving, so the frame loop can go
 * idle behind the window; the blades keep whatever bend they had and carry on
 * from it when the window goes away. */
void grass_set_paused(FwmGrass *g, bool paused);

/* What the last grass_tick returned, for a caller that has to ask again later
 * (the frame loop's busy test). Safe on NULL — a monitor with no grass is not
 * moving. */
bool grass_moving(const FwmGrass *g);

/* Put the strip back on top of the background layer. A new wallpaper set is a
 * NEW node in that same tree, and the scene draws it above everything already
 * there — so grass grown before it is buried by a reload or by picking a
 * wallpaper unless the caller says otherwise. */
void grass_raise(FwmGrass *g);

/* The config no longer describes the BLADES — their number, length or width —
 * so the caller should tear this instance down and grow a new one. Everything
 * else (colour, opacity, wind) grass_tick picks up on its own, without
 * reshuffling a patch somebody is looking at. */
bool grass_stale(const FwmGrass *g, const GrassConfig *cfg);

void grass_destroy(FwmGrass *g);

#endif /* FWM_GRASS_H */
