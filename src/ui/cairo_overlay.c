#include "cairo_overlay.h"
#include <stdlib.h>
#include <wlr/interfaces/wlr_buffer.h>

#ifndef DRM_FORMAT_ARGB8888
#define DRM_FORMAT_ARGB8888 875713089
#endif

/* wlroots-0.20 scene buffers are backed by a struct wlr_buffer rather than a
 * texture, so overlays wrap a cairo image surface in a wlr_buffer. */
struct CairoOverlayBuffer {
    struct wlr_buffer base;
    cairo_surface_t *surface;
};

/* Persistent per-overlay state kept on the scene node (node.data).
 *
 * Two wlroots behaviours make this necessary:
 *  - After a buffer is presented, the scene clears scene_buffer->buffer back to
 *    NULL, so it cannot be used to recover the overlay's dimensions on the next
 *    redraw.
 *  - Re-setting the *same* wlr_buffer is a no-op, so every redraw must supply a
 *    fresh buffer or the first frame stays on screen forever.
 * We therefore remember the size here and keep the current backing buffer alive
 * with our own lock until the next redraw replaces it. */
struct CairoOverlayInfo {
    int width, height;
    struct wlr_buffer *current; /* held alive by one lock we own */
};

static void overlay_buffer_destroy(struct wlr_buffer *wlr_buffer) {
    struct CairoOverlayBuffer *buffer = wl_container_of(wlr_buffer, buffer, base);
    wlr_buffer_finish(wlr_buffer);
    cairo_surface_destroy(buffer->surface);
    free(buffer);
}

static bool overlay_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
                                                  uint32_t flags, void **data,
                                                  uint32_t *format, size_t *stride) {
    struct CairoOverlayBuffer *buffer = wl_container_of(wlr_buffer, buffer, base);
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
        return false;
    }
    *format = DRM_FORMAT_ARGB8888;
    *data = cairo_image_surface_get_data(buffer->surface);
    *stride = cairo_image_surface_get_stride(buffer->surface);
    return true;
}

static void overlay_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
    (void)wlr_buffer;
}

static const struct wlr_buffer_impl overlay_buffer_impl = {
    .destroy = overlay_buffer_destroy,
    .begin_data_ptr_access = overlay_buffer_begin_data_ptr_access,
    .end_data_ptr_access = overlay_buffer_end_data_ptr_access,
};

/* Allocate a fresh cairo-backed wlr_buffer (n_locks == 0). */
static struct CairoOverlayBuffer *overlay_buffer_alloc(int width, int height) {
    struct CairoOverlayBuffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) return NULL;

    wlr_buffer_init(&buffer->base, &overlay_buffer_impl, width, height);

    buffer->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(buffer->surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(buffer->surface);
        free(buffer);
        return NULL;
    }
    return buffer;
}

/* Take an owning lock on a freshly-allocated buffer. lock() then drop() leaves
 * the buffer alive with exactly one lock (ours); a matching unlock frees it. */
static struct wlr_buffer *overlay_buffer_own(struct CairoOverlayBuffer *buffer) {
    wlr_buffer_lock(&buffer->base);
    wlr_buffer_drop(&buffer->base);
    return &buffer->base;
}

struct wlr_scene_buffer *cairo_overlay_create(struct wlr_scene_tree *parent, int width, int height) {
    struct CairoOverlayInfo *info = calloc(1, sizeof(*info));
    if (!info) return NULL;
    info->width = width;
    info->height = height;

    struct CairoOverlayBuffer *buffer = overlay_buffer_alloc(width, height);
    if (!buffer) {
        free(info);
        return NULL;
    }
    struct wlr_buffer *buf = overlay_buffer_own(buffer);

    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_create(parent, buf);
    if (!scene_buffer) {
        wlr_buffer_unlock(buf);
        free(info);
        return NULL;
    }
    info->current = buf;
    scene_buffer->node.data = info;
    return scene_buffer;
}

void cairo_overlay_update(struct wlr_scene_buffer *scene_buffer,
                          void (*draw_func)(cairo_t *cr, int w, int h, void *user_data), void *user_data) {
    if (!scene_buffer) return;
    struct CairoOverlayInfo *info = scene_buffer->node.data;
    if (!info || info->width <= 0 || info->height <= 0) return;

    struct CairoOverlayBuffer *buffer = overlay_buffer_alloc(info->width, info->height);
    if (!buffer) return;

    cairo_t *cr = cairo_create(buffer->surface);

    // Clear surface with transparency
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    draw_func(cr, info->width, info->height, user_data);

    cairo_destroy(cr);
    cairo_surface_flush(buffer->surface);

    struct wlr_buffer *buf = overlay_buffer_own(buffer);
    wlr_scene_buffer_set_buffer_with_damage(scene_buffer, buf, NULL);

    // Release the previous frame's buffer now that the scene shows the new one.
    if (info->current) {
        wlr_buffer_unlock(info->current);
    }
    info->current = buf;
}

void cairo_overlay_destroy(struct wlr_scene_buffer *scene_buffer) {
    if (!scene_buffer) return;
    struct CairoOverlayInfo *info = scene_buffer->node.data;
    wlr_scene_node_destroy(&scene_buffer->node);
    if (info) {
        if (info->current) {
            wlr_buffer_unlock(info->current);
        }
        free(info);
    }
}
