#ifndef FWM_CAIRO_OVERLAY_H
#define FWM_CAIRO_OVERLAY_H

#include <cairo.h>
#include <pango/pangocairo.h>
#include <wlr/types/wlr_scene.h>

struct wlr_scene_buffer *cairo_overlay_create(struct wlr_scene_tree *parent, int width, int height);
void cairo_overlay_update(struct wlr_scene_buffer *scene_buffer,
                          void (*draw_func)(cairo_t *cr, int w, int h, void *data), void *user_data);
void cairo_overlay_destroy(struct wlr_scene_buffer *scene_buffer);

/* For overlays that are drawn once and never updated again (wallpaper layers,
 * welcome, hints): free the CPU-side pixel copy and keep only the GPU texture.
 * Do not call cairo_overlay_update() on the overlay afterwards. */
void cairo_overlay_make_static(struct wlr_scene_buffer *scene_buffer);

#endif /* FWM_CAIRO_OVERLAY_H */
