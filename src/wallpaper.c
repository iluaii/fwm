#include "wallpaper.h"
#include "ui/cairo_overlay.h"

#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Runtime layer. `slack` (= buffer_width - screen_width) is how far the layer can
 * scroll left before its right edge would enter view; `wallpaper_update` clamps
 * the camera-driven shift to it so an edge is never exposed. Only "pan" layers
 * have non-zero factor/slack; "cover"/"contain" are static (screen-sized). */
struct WallpaperRT {
    struct wlr_scene_buffer *buffer;
    int slack;
};

struct FwmWallpaper {
    struct WallpaperRT *layers;
    int count;
    int pan_range; /* camera_x span mapped to a full-slack traversal (all desktops) */
};

struct DrawCtx {
    cairo_surface_t *img;
    int contain; /* 1: fit whole image inside; 0: cover (fill+crop) */
};

/* Load any gdk-pixbuf-supported format (PNG/JPEG/WebP/…) into a premultiplied
 * ARGB32 cairo surface. Returns NULL on failure. */
static cairo_surface_t *load_image(const char *path) {
    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &err);
    if (!pb) {
        fprintf(stderr, "fwm wallpaper: cannot load '%s': %s\n",
                path, err ? err->message : "unknown error");
        if (err) g_error_free(err);
        return NULL;
    }

    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int nch = gdk_pixbuf_get_n_channels(pb);
    int sstride = gdk_pixbuf_get_rowstride(pb);
    int has_alpha = gdk_pixbuf_get_has_alpha(pb);
    const guchar *src = gdk_pixbuf_get_pixels(pb);

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        g_object_unref(pb);
        return NULL;
    }

    unsigned char *dst = cairo_image_surface_get_data(surf);
    int dstride = cairo_image_surface_get_stride(surf);

    // gdk-pixbuf is non-premultiplied R,G,B[,A]; cairo ARGB32 is premultiplied
    // native-endian 0xAARRGGBB. Convert per pixel.
    for (int y = 0; y < h; y++) {
        const guchar *s = src + (size_t)y * sstride;
        uint32_t *d = (uint32_t *)(dst + (size_t)y * dstride);
        for (int x = 0; x < w; x++) {
            uint32_t r = s[0], g = s[1], b = s[2];
            uint32_t a = has_alpha ? s[3] : 255;
            if (a != 255) {
                r = r * a / 255;
                g = g * a / 255;
                b = b * a / 255;
            }
            d[x] = (a << 24) | (r << 16) | (g << 8) | b;
            s += nch;
        }
    }

    cairo_surface_mark_dirty(surf);
    g_object_unref(pb);
    return surf;
}

static void draw_layer(cairo_t *cr, int w, int h, void *user) {
    struct DrawCtx *ctx = user;
    int iw = cairo_image_surface_get_width(ctx->img);
    int ih = cairo_image_surface_get_height(ctx->img);
    if (iw <= 0 || ih <= 0) return;

    double sx = (double)w / iw;
    double sy = (double)h / ih;
    // cover -> max (fill, crop); contain -> min (whole image fits, letterboxed)
    double s = ctx->contain ? (sx < sy ? sx : sy) : (sx > sy ? sx : sy);
    double tx = (w - iw * s) / 2.0;
    double ty = (h - ih * s) / 2.0;

    cairo_save(cr);
    cairo_translate(cr, tx, ty);
    cairo_scale(cr, s, s);
    cairo_set_source_surface(cr, ctx->img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
}

FwmWallpaper *wallpaper_create(struct wlr_scene_tree *parent, const FwmConfig *cfg,
                               int screen_w, int screen_h) {
    if (!parent || cfg->wallpaper_count <= 0 || screen_w <= 0 || screen_h <= 0) {
        return NULL;
    }

    FwmWallpaper *wp = calloc(1, sizeof(*wp));
    if (!wp) return NULL;
    wp->layers = calloc(cfg->wallpaper_count, sizeof(struct WallpaperRT));
    if (!wp->layers) { free(wp); return NULL; }
    // The compositor has 10 desktops (camera 0 .. 9*screen_w); map that whole
    // span to a full-slack pan so a pan layer scrolls from its left edge on
    // desktop 0 to its right edge on the last desktop.
    wp->pan_range = 9 * screen_w;

    for (int i = 0; i < cfg->wallpaper_count; i++) {
        const WallpaperLayer *layer = &cfg->wallpapers[i];

        cairo_surface_t *img = load_image(layer->path);
        if (!img) continue;

        int iw = cairo_image_surface_get_width(img);
        int ih = cairo_image_surface_get_height(img);

        int buf_w = screen_w;   // buffer width (>= screen_w); slack = buf_w - screen_w
        int contain = 0;        // draw mode passed to draw_layer

        switch (layer->fit) {
        case WALLPAPER_FIT_CONTAIN:
            contain = 1;                 // whole image, letterboxed, no pan
            break;
        case WALLPAPER_FIT_PAN:
            // Walkable background. Default (zoom <= 0) renders at the image's
            // native width — no upscaling, so it stays sharp — and pans across
            // whatever width exceeds the screen. A positive zoom widens the
            // render to `screen_w * zoom` for more travel at the cost of scaling
            // the image up. draw_layer then covers this buffer.
            if (layer->zoom > 0.0) {
                buf_w = (int)lround(screen_w * layer->zoom);
            } else {
                buf_w = iw; // native width = sharpest
            }
            if (buf_w < screen_w) buf_w = screen_w;
            break;
        case WALLPAPER_FIT_COVER:
        default:
            break;                       // fill screen, crop overflow, static
        }

        struct wlr_scene_buffer *buf = cairo_overlay_create(parent, buf_w, screen_h);
        if (buf) {
            struct DrawCtx ctx = { .img = img, .contain = contain };
            cairo_overlay_update(buf, draw_layer, &ctx);

            int idx = wp->count++;
            wp->layers[idx].buffer = buf;
            wp->layers[idx].slack  = buf_w - screen_w;
            wlr_scene_node_set_position(&buf->node, 0, 0);
        }
        cairo_surface_destroy(img);
    }

    if (wp->count == 0) {
        free(wp->layers);
        free(wp);
        return NULL;
    }
    return wp;
}

void wallpaper_update(FwmWallpaper *wp, int camera_x) {
    if (!wp || wp->pan_range <= 0) return;
    // Map the camera position across the whole desktop range to [0,1], so each
    // pan layer traverses its full slack evenly and reaches its right edge only
    // on the last desktop (no early, abrupt stop).
    double t = (double)camera_x / wp->pan_range;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    for (int i = 0; i < wp->count; i++) {
        struct WallpaperRT *l = &wp->layers[i];
        int shift = (int)lround(l->slack * t);
        wlr_scene_node_set_position(&l->buffer->node, -shift, 0);
    }
}

void wallpaper_destroy(FwmWallpaper *wp) {
    if (!wp) return;
    for (int i = 0; i < wp->count; i++) {
        if (wp->layers[i].buffer) {
            cairo_overlay_destroy(wp->layers[i].buffer);
        }
    }
    free(wp->layers);
    free(wp);
}
