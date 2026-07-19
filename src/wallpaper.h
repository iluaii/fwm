#ifndef FWM_WALLPAPER_H
#define FWM_WALLPAPER_H

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

void wallpaper_destroy(FwmWallpaper *wp);

#endif /* FWM_WALLPAPER_H */
