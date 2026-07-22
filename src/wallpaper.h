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

#ifndef FWM_WALLPAPER_H
#define FWM_WALLPAPER_H

#include <stdbool.h>
#include <wlr/types/wlr_scene.h>
#include "config.h"

typedef struct FwmWallpaper FwmWallpaper;

/* Build parallax wallpaper layers as children of `parent`, which must be a scene
 * tree positioned below the window layer. Layers are drawn back-to-front in the
 * order they appear in the config. Returns NULL if no layer could be loaded. */
FwmWallpaper *wallpaper_create(struct wlr_scene_tree *parent, const FwmConfig *cfg,
                               int screen_w, int screen_h);

/* Reposition every layer for the current horizontal camera offset. */
void wallpaper_update(FwmWallpaper *wp, int camera_x);

/* Start the layers transparent and ramp them to opaque over `duration_ms`.
 * The caller keeps the previous wallpaper alive underneath until
 * wallpaper_fade_tick reports the fade finished. */
void wallpaper_fade_in(FwmWallpaper *wp, double duration_ms);

/* Advance a running cross-fade. Returns true on the frame it completes. */
bool wallpaper_fade_tick(FwmWallpaper *wp, double dt);

void wallpaper_destroy(FwmWallpaper *wp);

#endif /* FWM_WALLPAPER_H */
