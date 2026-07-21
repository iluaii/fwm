#include "wallpaper.h"
#include "ui/cairo_overlay.h"

#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/util/log.h>

/* `fit = "pan"` with no explicit zoom pans ONLY images wide enough to stick
 * out past the screen on their own, at native scale with nothing cropped.
 *
 * Anything narrower can only be made to travel by zooming in, which pushes the
 * height past the screen and eats the composition — for an image near the
 * screen's own aspect that costs 25-40% of the height for a few hundred px of
 * movement, which reads as "the picture is barely there". That trade is the
 * user's to make per wallpaper, via [[wallpaper]] pan_crop (or zoom for exact
 * control), so the default budget is zero. */
/* Decode margin over the drawn size: FILTER_BEST does the final, high-quality
 * step, so the cheap in-decode scale never has to be the last word. */
#define WALLPAPER_OVERSAMPLE 2.0
#define PAN_MIN_TRAVEL  24    /* below this, movement is not worth any crop */

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

    /* Cross-fade in, used when the picker swaps wallpapers at runtime: the
     * outgoing set stays on screen underneath until this reaches 1. */
    int    fading;
    double fade_t, fade_ms;
};

struct DrawCtx {
    cairo_surface_t *img;
    int contain;  /* 1: fit whole image inside; 0: cover (fill+crop) */
    double scale; /* > 0: uniform aspect-true scale (pan layers); 0: cover/contain */
};

/* Load any gdk-pixbuf-supported format (PNG/JPEG/WebP/…) into a premultiplied
 * ARGB32 cairo surface. Returns NULL on failure. */
/* Decode `path`, scaled down to fit max_w x max_h (aspect preserved). Pass 0
 * for either to decode at native size. Scaling during the decode is what keeps
 * a big wallpaper cheap: gdk-pixbuf hands JPEG a DCT-scale hint instead of
 * expanding every pixel just so cairo can throw most of them away. */
static cairo_surface_t *load_image(const char *path, int max_w, int max_h) {
    GError *err = NULL;
    GdkPixbuf *pb = (max_w > 0 && max_h > 0)
        ? gdk_pixbuf_new_from_file_at_scale(path, max_w, max_h, TRUE, &err)
        : gdk_pixbuf_new_from_file(path, &err);
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

    double s, tx, ty;
    if (ctx->scale > 0.0) {
        // Pan layer: one aspect-true scale chosen so the image height exactly
        // matches the buffer height — never the cover math, which either
        // stretched short images or drew tall ones unscaled (1:1 crop band).
        s = ctx->scale;
        tx = 0.0;
        ty = 0.0;
    } else {
        double sx = (double)w / iw;
        double sy = (double)h / ih;
        // cover -> max (fill, crop); contain -> min (whole image fits, letterboxed)
        s = ctx->contain ? (sx < sy ? sx : sy) : (sx > sy ? sx : sy);
        tx = (w - iw * s) / 2.0;
        ty = (h - ih * s) / 2.0;
    }

    cairo_save(cr);
    cairo_translate(cr, tx, ty);
    cairo_scale(cr, s, s);
    cairo_set_source_surface(cr, ctx->img, 0, 0);
    // One-time render into a static buffer: spend on quality. GOOD's bilinear
    // smears badly on strong downscales.
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
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

        // Header only. The fit maths below needs the dimensions, but decoding
        // at native size just to discard most of the pixels is what made
        // applying a large wallpaper take seconds on a slow machine.
        int iw = 0, ih = 0;
        if (!gdk_pixbuf_get_file_info(layer->path, &iw, &ih) || iw <= 0 || ih <= 0) {
            fprintf(stderr, "fwm wallpaper: cannot read '%s'\n", layer->path);
            continue;
        }

        int buf_w = screen_w;   // buffer width (>= screen_w); slack = buf_w - screen_w
        int contain = 0;        // draw mode passed to draw_layer
        double scale = 0.0;     // > 0: uniform pan scale for draw_layer

        switch (layer->fit) {
        case WALLPAPER_FIT_CONTAIN:
            contain = 1;                 // whole image, letterboxed, no pan
            break;
        case WALLPAPER_FIT_PAN:
            // Walkable background. The buffer is wider than the screen and the
            // overhang is the pan travel.
            if (layer->zoom > 0.0) {
                // Explicit zoom always wins: the user asked for exactly this
                // much travel and accepted the upscale that comes with it.
                buf_w = (int)lround(screen_w * layer->zoom);
                if (buf_w < screen_w) buf_w = screen_w;
            } else if (ih > 0) {
                // Auto. Fitting to screen height keeps the image at native
                // scale, which is sharpest — but only wide images stick out
                // past the screen that way. A 4:3-ish image fitted to a 16:9
                // screen ends up NARROWER than the screen, and panning it was
                // silently dropped, which just reads as "parallax is broken".
                // So: pan at native scale when the image is wide enough,
                // otherwise zoom in just enough to buy some travel, capped by
                // an upscale budget so it never turns to mush.
                double fit_scale = (double)screen_h / ih;
                int fitted_w = (int)lround(iw * fit_scale);

                if (fitted_w >= screen_w + PAN_MIN_TRAVEL) {
                    buf_w = fitted_w;
                    scale = fit_scale;   // native size, nothing cropped
                } else if (layer->pan_crop > 0.0) {
                    // Opted in: spend exactly the configured slice of height
                    // and take whatever travel it buys.
                    double max_scale = fit_scale / (1.0 - layer->pan_crop);
                    buf_w = (int)lround(iw * max_scale);
                    scale = 0.0;         // cover semantics fill the wider buffer
                    if (buf_w < screen_w + PAN_MIN_TRAVEL) {
                        buf_w = screen_w;
                        wlr_log(WLR_INFO, "wallpaper layer %d: pan_crop %.2f still "
                                "buys no travel for %dx%d on %dx%d — static fill",
                                i + 1, layer->pan_crop, iw, ih, screen_w, screen_h);
                    }
                } else {
                    // Not wide enough to pan and no crop allowed: show it whole
                    // and still. Cropping it uninvited is what made the last
                    // version unusable.
                    buf_w = screen_w;
                    scale = 0.0;
                    wlr_log(WLR_INFO, "wallpaper layer %d: %dx%d is not wide enough "
                            "to pan on %dx%d — static fill (set pan_crop or zoom "
                            "to trade height for travel)", i + 1, iw, ih,
                            screen_w, screen_h);
                }
            }
            break;
        case WALLPAPER_FIT_COVER:
        default:
            break;                       // fill screen, crop overflow, static
        }

        // Decode only what the buffer will actually show, with an oversample
        // margin so draw_layer's FILTER_BEST still has detail to resample from.
        // Clamped to native: upscaling here would just burn memory.
        double sx = (double)buf_w / iw, sy = (double)screen_h / ih;
        double draw_s = scale > 0.0 ? scale
                      : contain    ? (sx < sy ? sx : sy)
                                   : (sx > sy ? sx : sy);
        int dec_w = (int)lround(iw * draw_s * WALLPAPER_OVERSAMPLE);
        int dec_h = (int)lround(ih * draw_s * WALLPAPER_OVERSAMPLE);
        if (dec_w >= iw || dec_h >= ih) { dec_w = 0; dec_h = 0; }  /* native */

        cairo_surface_t *img = load_image(layer->path, dec_w, dec_h);
        if (!img) continue;

        // The pan scale was derived from the file's native height; rebase it on
        // what we actually decoded, or the layer draws at a fraction of size.
        int ah = cairo_image_surface_get_height(img);
        if (scale > 0.0 && ah > 0) scale = (double)screen_h / ah;

        struct wlr_scene_buffer *buf = cairo_overlay_create(parent, buf_w, screen_h);
        if (buf) {
            struct DrawCtx ctx = { .img = img, .contain = contain, .scale = scale };
            cairo_overlay_update(buf, draw_layer, &ctx);
            // Layers never redraw (panning just moves the node): drop the
            // CPU-side pixels — a native-width pan buffer is tens of MB.
            cairo_overlay_make_static(buf);

            int idx = wp->count++;
            wp->layers[idx].buffer = buf;
            wp->layers[idx].slack  = buf_w - screen_w;
            wlr_log(WLR_INFO, "wallpaper layer %d: buffer %dpx, pan travel %dpx, "
                    "height cropped %.0f%%", i + 1, buf_w, buf_w - screen_w,
                    100.0 * (scale > 0.0 ? 0.0
                             : 1.0 - (double)screen_h / ((double)ih * buf_w / iw)));
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

void wallpaper_fade_in(FwmWallpaper *wp, double duration_ms) {
    if (!wp || duration_ms <= 0.0) return;
    wp->fading = 1;
    wp->fade_t = 0.0;
    wp->fade_ms = duration_ms;
    for (int i = 0; i < wp->count; i++) {
        wlr_scene_buffer_set_opacity(wp->layers[i].buffer, 0.0f);
    }
}

bool wallpaper_fade_tick(FwmWallpaper *wp, double dt) {
    if (!wp || !wp->fading) return false;
    wp->fade_t += dt * 1000.0 / wp->fade_ms;
    int done = wp->fade_t >= 1.0;
    if (done) wp->fade_t = 1.0;

    /* Smoothstep: no hard start or stop, which is what made the instant swap
     * feel like a cut. */
    double t = wp->fade_t;
    double e = t * t * (3.0 - 2.0 * t);
    for (int i = 0; i < wp->count; i++) {
        wlr_scene_buffer_set_opacity(wp->layers[i].buffer, (float)e);
    }
    if (done) wp->fading = 0;
    return done;
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
