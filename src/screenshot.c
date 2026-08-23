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

#include "screenshot.h"
#include "clipboard.h"
#include "server.h"
#include "rotate.h"
#include "snapshot.h"
#include "theme.h"
#include "ui/cairo_overlay.h"

#include <cairo.h>
#include <errno.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

#define TOAST_W        520
#define TOAST_H        44
#define TOAST_HOLD_MS  1800
#define TOAST_ANIM_MS  170.0
#define TOAST_RISE_PX  14.0
#define TOAST_CUT      12.0   /* corner chevron, same silhouette as the panels */

#define LABEL_W        260
#define LABEL_H        30

/* ── the toast ────────────────────────────────────────────────────────
 *
 * One line at the bottom of the screen saying where the file went, because a
 * screenshot that saves silently is indistinguishable from one that failed.
 * Module state rather than a server field: like the overlay animations it
 * rides on, only one is ever up, and nothing outside this file asks about it. */

static struct {
    struct wlr_scene_buffer *buffer;
    struct wl_event_source  *timer;
    char text[512];
    bool bad;                       /* red accent: the shot did not happen */
} toast;

static void toast_drop(void *data) {
    (void)data;
    toast.buffer = NULL;
}

static void toast_draw(cairo_t *cr, int w, int h, void *user_data) {
    (void)user_data;
    const FwmTheme *thm = theme_get();

    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], 0.94);
    cairo_move_to(cr, TOAST_CUT, 0);
    cairo_line_to(cr, w - TOAST_CUT, 0);
    cairo_line_to(cr, w, TOAST_CUT);
    cairo_line_to(cr, w, h - TOAST_CUT);
    cairo_line_to(cr, w - TOAST_CUT, h);
    cairo_line_to(cr, TOAST_CUT, h);
    cairo_line_to(cr, 0, h - TOAST_CUT);
    cairo_line_to(cr, 0, TOAST_CUT);
    cairo_close_path(cr);
    cairo_fill(cr);

    /* A dot in the accent colour, red when the shot failed — the same "is this
     * good news" cue the tray's pills use. */
    if (toast.bad) cairo_set_source_rgb(cr, 0.93, 0.42, 0.38);
    else cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
    cairo_arc(cr, 18, h / 2.0, 4.0, 0, 2 * 3.14159265358979);
    cairo_fill(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    /* Middle-ellipsized: what matters in a long path is the directory and the
     * file name, not the bit in between. */
    pango_layout_set_width(layout, (w - 46) * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_MIDDLE);
    pango_layout_set_text(layout, toast.text, -1);

    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
    cairo_move_to(cr, 34, (h - th) / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

static int toast_expire(void *data) {
    (void)data;
    if (toast.buffer) {
        /* Ownership passes to the animation, so the pointer goes now and the
         * callback only clears what is left. */
        cairo_overlay_animate_out(toast.buffer, TOAST_ANIM_MS, -TOAST_RISE_PX,
                                  toast_drop, NULL);
        toast.buffer = NULL;
    }
    if (toast.timer) {
        wl_event_source_remove(toast.timer);
        toast.timer = NULL;
    }
    return 0;
}

static void toast_show(FwmServer *server, bool bad, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toast.text, sizeof(toast.text), fmt, ap);
    va_end(ap);
    toast.bad = bad;

    /* A second shot while the first one's toast is still up replaces it
     * outright: two stacked messages about the same thing help nobody. */
    if (toast.buffer) {
        cairo_overlay_destroy(toast.buffer);
        toast.buffer = NULL;
    }
    if (toast.timer) {
        wl_event_source_remove(toast.timer);
        toast.timer = NULL;
    }

    FwmOutput *out = server_active_output(server);
    if (!out) return;

    toast.buffer = cairo_overlay_create(server->layer_overlay, TOAST_W, TOAST_H);
    if (!toast.buffer) return;
    wlr_scene_node_set_position(&toast.buffer->node,
                                out->box.x + (out->box.width - TOAST_W) / 2,
                                out->box.y + out->box.height - TOAST_H - 64);
    cairo_overlay_update(toast.buffer, toast_draw, NULL);
    cairo_overlay_make_static(toast.buffer);
    cairo_overlay_animate_in(toast.buffer, TOAST_ANIM_MS, TOAST_RISE_PX);

    toast.timer = wl_event_loop_add_timer(wl_display_get_event_loop(server->wl_display),
                                          toast_expire, NULL);
    if (toast.timer) wl_event_source_timer_update(toast.timer, TOAST_HOLD_MS);
}

/* ── the clipboard ───────────────────────────────────────────────────
 *
 * fwm IS the clipboard: a compositor owns the seat's selection, so the PNG is
 * offered from here directly and there is no wl-copy to install and no helper
 * process to keep alive holding the bytes.
 *
 * The handing-over lives in src/clipboard.c, which does the same job for text
 * a dead client left behind — one place that knows how to feed a pipe without
 * stalling the compositor, rather than two copies of the same event-loop
 * writer. */
static bool clipboard_put(FwmServer *server, unsigned char *png, size_t len) {
    static const char *const mimes[] = { "image/png" };
    return clipboard_offer(server->clipboard, mimes, 1, png, len);
}

/* ── reading the frame back ──────────────────────────────────────────── */

/* Both orders the renderers hand out, and both with and without a meaningful
 * alpha channel. Cairo's RGB24 is 0xXXRRGGBB in native words, which IS
 * DRM_FORMAT_[XA]RGB8888 on little-endian — so half of these need no work at
 * all, and the other half needs the red and blue bytes swapped. Writing RGB24
 * rather than ARGB32 is deliberate: an X-format buffer's fourth byte is
 * undefined, and cairo would read it as (premultiplied) alpha and produce a
 * PNG with holes in it. A screenshot is opaque. */
static bool read_format_ok(uint32_t fmt, bool *swap_rb) {
    switch (fmt) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888: *swap_rb = false; return true;
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888: *swap_rb = true;  return true;
    default: return false;
    }
}

/* Growing byte buffer for cairo's PNG writer. */
struct png_sink {
    unsigned char *data;
    size_t len, cap;
};

static cairo_status_t png_write(void *closure, const unsigned char *chunk,
                                unsigned int len) {
    struct png_sink *sink = closure;
    if (sink->len + len > sink->cap) {
        size_t cap = sink->cap ? sink->cap * 2 : 256 * 1024;
        while (cap < sink->len + len) cap *= 2;
        unsigned char *bigger = realloc(sink->data, cap);
        if (!bigger) return CAIRO_STATUS_NO_MEMORY;
        sink->data = bigger;
        sink->cap = cap;
    }
    memcpy(sink->data + sink->len, chunk, len);
    sink->len += len;
    return CAIRO_STATUS_SUCCESS;
}

/* The frame, cropped to `box`, encoded as a PNG in memory. The caller owns
 * *out and hands it straight to the clipboard. */
static bool encode_png(FwmServer *server, struct wlr_buffer *src,
                       const struct wlr_box *box, unsigned char **out,
                       size_t *out_len, int *out_w, int *out_h) {
    struct wlr_texture *tex = wlr_texture_from_buffer(server->wlr_renderer, src);
    if (!tex) {
        wlr_log(WLR_ERROR, "screenshot: the frame is not readable as a texture");
        return false;
    }

    bool swap_rb = false;
    uint32_t fmt = wlr_texture_preferred_read_format(tex);
    if (!read_format_ok(fmt, &swap_rb)) {
        /* Ask for the one format every renderer here can produce rather than
         * giving up on an exotic preference. */
        fmt = DRM_FORMAT_XRGB8888;
        swap_rb = false;
    }

    struct wlr_box b = *box;
    struct wlr_box full = { 0, 0, (int)tex->width, (int)tex->height };
    if (!wlr_box_intersection(&b, &b, &full) || b.width <= 0 || b.height <= 0) {
        wlr_texture_destroy(tex);
        wlr_log(WLR_ERROR, "screenshot: nothing of the region is on the screen");
        return false;
    }

    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, b.width);
    unsigned char *data = malloc((size_t)stride * b.height);
    if (!data) {
        wlr_texture_destroy(tex);
        return false;
    }

    bool ok = wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
        .data = data,
        .format = fmt,
        .stride = (uint32_t)stride,
        .src_box = b,
    });
    wlr_texture_destroy(tex);
    if (!ok) {
        free(data);
        wlr_log(WLR_ERROR, "screenshot: the renderer would not read the frame back");
        return false;
    }

    if (swap_rb) {
        for (int y = 0; y < b.height; y++) {
            unsigned char *row = data + (size_t)y * stride;
            for (int x = 0; x < b.width; x++) {
                unsigned char t = row[x * 4];
                row[x * 4] = row[x * 4 + 2];
                row[x * 4 + 2] = t;
            }
        }
    }

    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        data, CAIRO_FORMAT_RGB24, b.width, b.height, stride);
    struct png_sink sink = {0};
    cairo_status_t st = cairo_surface_status(surf);
    if (st == CAIRO_STATUS_SUCCESS)
        st = cairo_surface_write_to_png_stream(surf, png_write, &sink);
    cairo_surface_destroy(surf);
    free(data);

    if (st != CAIRO_STATUS_SUCCESS) {
        free(sink.data);
        wlr_log(WLR_ERROR, "screenshot: cannot encode the picture: %s",
                cairo_status_to_string(st));
        return false;
    }
    *out = sink.data;
    *out_len = sink.len;
    *out_w = b.width;
    *out_h = b.height;
    return true;
}

/* ── the peel-off ─────────────────────────────────────────────────────
 *
 * The region does not simply stop being selected: a frozen copy of it lifts
 * off the screen, shrinks, and flies down to the line that says it was
 * copied, while the live screen carries on underneath. It is the one moment
 * where a screenshot is visible as an act rather than as a file appearing
 * somewhere, and it is also honest feedback — you can see WHICH pixels were
 * taken, after the selector's dimming is already gone.
 *
 * The copy is a buffer of our own rather than the monitor's frame with a
 * source box on it: that frame belongs to the swapchain, and holding one of
 * its buffers for a third of a second to animate it is a loan the compositor
 * should not be taking out. One GPU blit and the swapchain is left alone. */

#define FLY_MS       340.0   /* at effects.shot_fly = 1 */
#define FLY_SCALE    0.55    /* how small it gets by the end */
#define FLY_SOLID    0.30    /* fraction of the flight before it starts fading */
#define FLY_TILT     0.16    /* radians it has turned by the end (~9 degrees) */
#define FLY_TICK_MS  16      /* ~60 Hz; the node moving is what damages the scene */

static struct {
    struct wlr_scene_buffer *node;
    struct wl_event_source  *timer;
    double t;                     /* 0 → 1 */
    double dur_ms;
    double cx0, cy0, cx1, cy1;    /* centre, start and destination (layout) */
    int w, h;                     /* full size, layout pixels */

    /* The tilt. Only present where arbitrary rotation works at all; without it
     * everything below stays NULL and the picture flies upright, which is a
     * smaller loss than losing the effect. */
    struct wlr_buffer  *src;      /* the frozen copy, in buffer pixels */
    struct wlr_texture *tex;      /* imported once — the tilt redraws from it
                                   * every tick, and re-importing a dmabuf 60
                                   * times a second is pure waste */
    struct wlr_buffer  *dst[2];   /* two, used alternately: the scene may still
                                   * be reading last tick's while this one is
                                   * drawn, and overwriting in place tears */
    int flip;
    int size_px;                  /* the square that holds the picture at every
                                   * angle: its own diagonal */
    double out_scale;             /* buffer pixels per layout pixel */
    FwmServer *server;
} fly;

static void fly_stop(void) {
    if (fly.timer) {
        wl_event_source_remove(fly.timer);
        fly.timer = NULL;
    }
    if (fly.node) {
        wlr_scene_node_destroy(&fly.node->node);
        fly.node = NULL;
    }
    if (fly.tex) {
        wlr_texture_destroy(fly.tex);
        fly.tex = NULL;
    }
    if (fly.src) {
        wlr_buffer_unlock(fly.src);
        fly.src = NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (fly.dst[i]) wlr_buffer_unlock(fly.dst[i]);
        fly.dst[i] = NULL;
    }
}

/* Fast at first, then coasting: the picture is thrown, not driven. */
static double ease_out(double t) {
    double u = 1.0 - t;
    return 1.0 - u * u * u;
}

static int fly_tick(void *data) {
    (void)data;
    if (!fly.node) return 0;

    fly.t += FLY_TICK_MS / fly.dur_ms;
    if (fly.t >= 1.0) { fly_stop(); return 0; }

    double e = ease_out(fly.t);
    double scale = 1.0 - (1.0 - FLY_SCALE) * e;
    int w = (int)(fly.w * scale);
    int h = (int)(fly.h * scale);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    double cx = fly.cx0 + (fly.cx1 - fly.cx0) * e;
    double cy = fly.cy0 + (fly.cy1 - fly.cy0) * e;

    if (fly.tex) {
        /* Tilted: the picture is drawn into a square target of its own
         * diagonal, turned about that square's centre, and the whole square is
         * placed centred on where the flight has got to. Scaling happens here
         * too — rotate_blit takes the size the source is drawn at — so the node
         * is always the full square and only its contents shrink. */
        struct wlr_buffer *dst = fly.dst[fly.flip];
        int src_w = (int)(w * fly.out_scale);
        int src_h = (int)(h * fly.out_scale);
        if (src_w < 1) src_w = 1;
        if (src_h < 1) src_h = 1;
        if (rotate_blit(fly.server->wlr_renderer, dst, fly.tex,
                        src_w, src_h, FLY_TILT * e)) {
            fly.flip ^= 1;
            double side = fly.size_px / fly.out_scale;
            wlr_scene_buffer_set_buffer(fly.node, dst);
            wlr_scene_buffer_set_dest_size(fly.node, (int)side, (int)side);
            wlr_scene_node_set_position(&fly.node->node,
                                        (int)(cx - side / 2.0), (int)(cy - side / 2.0));
        }
    } else {
        wlr_scene_buffer_set_dest_size(fly.node, w, h);
        wlr_scene_node_set_position(&fly.node->node,
                                    (int)(cx - w / 2.0), (int)(cy - h / 2.0));
    }

    /* Solid while it lifts off, then out: fading from the first frame would
     * read as the picture never having been there. */
    double o = fly.t < FLY_SOLID ? 1.0 : 1.0 - (fly.t - FLY_SOLID) / (1.0 - FLY_SOLID);
    wlr_scene_buffer_set_opacity(fly.node, (float)(o * o));

    wl_event_source_timer_update(fly.timer, FLY_TICK_MS);
    return 0;
}

/* A copy of `box` (buffer pixels of `src`) in a buffer of our own. */
static struct wlr_buffer *freeze_region(FwmServer *server, struct wlr_buffer *src,
                                        const struct wlr_box *box) {
    struct wlr_buffer *dst = snapshot_alloc(server, box->width, box->height);
    if (!dst) return NULL;

    struct wlr_texture *tex = wlr_texture_from_buffer(server->wlr_renderer, src);
    struct wlr_render_pass *pass = tex
        ? wlr_renderer_begin_buffer_pass(server->wlr_renderer, dst, NULL) : NULL;
    if (!pass) {
        if (tex) wlr_texture_destroy(tex);
        wlr_buffer_drop(dst);
        return NULL;
    }

    wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
        .texture = tex,
        .src_box = { box->x, box->y, box->width, box->height },
        .dst_box = { 0, 0, box->width, box->height },
        .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
    });
    bool ok = wlr_render_pass_submit(pass);
    wlr_texture_destroy(tex);
    if (!ok) { wlr_buffer_drop(dst); return NULL; }
    return dst;
}

/* Launch the effect for a shot of `box` (buffer pixels) on `out`. */
static void fly_start(FwmServer *server, struct wlr_output *out,
                      struct wlr_buffer *frame, const struct wlr_box *box) {
    double strength = server->config.effects.shot_fly;
    if (strength <= 0.0) return;

    FwmOutput *o = NULL, *it;
    wl_list_for_each(it, &server->outputs, link)
        if (it->wlr_output == out) { o = it; break; }
    if (!o) return;

    struct wlr_buffer *copy = freeze_region(server, frame, box);
    if (!copy) return;   /* the effect is never worth failing the screenshot for */

    fly_stop();   /* one at a time: a second shot replaces the first mid-flight */

    double scale = out->scale > 0.0f ? out->scale : 1.0;

    /* Rotation is the GLES2 escape hatch and not every renderer has it, so the
     * tilt is set up first and the upright path is what is left if any of it
     * fails. Nothing here is worth losing the flight over. */
    bool tilt = rotate_supported(server->wlr_renderer);
    if (tilt) {
        fly.src = wlr_buffer_lock(copy);
        fly.tex = wlr_texture_from_buffer(server->wlr_renderer, fly.src);
        fly.size_px = (int)ceil(hypot(box->width, box->height)) + 2;
        for (int i = 0; i < 2 && fly.tex; i++) {
            struct wlr_buffer *d = snapshot_alloc(server, fly.size_px, fly.size_px);
            if (!d) break;
            fly.dst[i] = wlr_buffer_lock(d);
            wlr_buffer_drop(d);
        }
        if (!fly.tex || !fly.dst[0] || !fly.dst[1]) {
            /* Half a tilt is no tilt: give the pieces back and fly upright. */
            if (fly.tex) wlr_texture_destroy(fly.tex);
            fly.tex = NULL;
            if (fly.src) wlr_buffer_unlock(fly.src);
            fly.src = NULL;
            for (int i = 0; i < 2; i++) {
                if (fly.dst[i]) wlr_buffer_unlock(fly.dst[i]);
                fly.dst[i] = NULL;
            }
        }
    }

    /* Upright starts from the copy itself; tilted starts empty and the first
     * tick draws the turned picture into it. */
    fly.node = wlr_scene_buffer_create(server->layer_overlay,
                                       fly.tex ? NULL : copy);
    wlr_buffer_drop(copy);   /* the scene (or fly.src) holds a reference now */
    if (!fly.node) { fly_stop(); return; }
    fly.server = server;
    fly.out_scale = scale;
    fly.flip = 0;
    fly.w = (int)(box->width / scale);
    fly.h = (int)(box->height / scale);
    if (fly.w < 1) fly.w = 1;
    if (fly.h < 1) fly.h = 1;
    fly.cx0 = o->box.x + box->x / scale + fly.w / 2.0;
    fly.cy0 = o->box.y + box->y / scale + fly.h / 2.0;
    /* Down to where the "copied" line is about to appear, so the two read as
     * one gesture: the picture goes into the message that says it went. */
    fly.cx1 = o->box.x + o->box.width / 2.0;
    fly.cy1 = o->box.y + o->box.height - 64 - TOAST_H / 2.0;
    fly.t = 0.0;
    fly.dur_ms = FLY_MS * strength;

    if (fly.tex) {
        /* Draw frame zero now: an empty node for one tick would flash. */
        if (rotate_blit(server->wlr_renderer, fly.dst[0], fly.tex,
                        box->width, box->height, 0.0)) {
            double side = fly.size_px / scale;
            fly.flip = 1;
            wlr_scene_buffer_set_buffer(fly.node, fly.dst[0]);
            wlr_scene_buffer_set_dest_size(fly.node, (int)side, (int)side);
            wlr_scene_node_set_position(&fly.node->node,
                                        (int)(fly.cx0 - side / 2.0),
                                        (int)(fly.cy0 - side / 2.0));
        }
    } else {
        wlr_scene_buffer_set_dest_size(fly.node, fly.w, fly.h);
        wlr_scene_node_set_position(&fly.node->node,
                                    (int)(fly.cx0 - fly.w / 2.0),
                                    (int)(fly.cy0 - fly.h / 2.0));
    }

    fly.timer = wl_event_loop_add_timer(wl_display_get_event_loop(server->wl_display),
                                        fly_tick, NULL);
    if (fly.timer) wl_event_source_timer_update(fly.timer, FLY_TICK_MS);
    else fly_stop();
}

/* ── the pending capture ─────────────────────────────────────────────── */

/* A request waiting for the monitor to hand over a frame. It owns itself: the
 * commit that satisfies it is also what frees it, and the monitor going away
 * first does the same. */
typedef struct {
    FwmServer *server;
    struct wlr_output *wlr_output;
    struct wlr_box region;              /* output-local pixels */
    bool fly;                           /* peel the picture off on the way out */
    struct wl_listener commit;
    struct wl_listener destroy;
} ShotPending;

static void pending_free(ShotPending *p) {
    wl_list_remove(&p->commit.link);
    wl_list_remove(&p->destroy.link);
    free(p);
}

static void pending_commit(struct wl_listener *listener, void *data) {
    ShotPending *p = wl_container_of(listener, p, commit);
    struct wlr_output_event_commit *event = data;

    /* A commit that changed something other than the picture (a mode, the
     * gamma ramp) carries no buffer; keep waiting for one that does. */
    if (!(event->state->committed & WLR_OUTPUT_STATE_BUFFER) || !event->state->buffer)
        return;

    FwmServer *server = p->server;
    struct wlr_box region = p->region;
    struct wlr_output *out = p->wlr_output;
    bool want_fly = p->fly;
    pending_free(p);   /* before the work below: encoding a picture takes a
                        * while, and a second frame must not find this request
                        * still armed */

    unsigned char *png = NULL;
    size_t len = 0;
    int w = 0, h = 0;
    if (!encode_png(server, event->state->buffer, &region, &png, &len, &w, &h) ||
        !clipboard_put(server, png, len)) {
        toast_show(server, true, "screenshot failed (see the log)");
        return;
    }
    if (want_fly) fly_start(server, out, event->state->buffer, &region);
    wlr_log(WLR_INFO, "screenshot: %dx%d on the clipboard (%zu KiB)", w, h, len / 1024);
    toast_show(server, false, "screenshot copied \xE2\x80\x94 %d \xC3\x97 %d, paste it anywhere", w, h);
}

static void pending_output_destroy(struct wl_listener *listener, void *data) {
    ShotPending *p = wl_container_of(listener, p, destroy);
    pending_free(p);
}

/* Ask `out` for a frame and photograph it when it arrives. `region` is
 * output-local and in LAYOUT (logical) pixels, the coordinates the pointer
 * works in; a zero-sized one means the whole monitor. The frame that comes
 * back is in buffer pixels, which on a scaled monitor is a different number —
 * hence the multiply. */
static void capture(FwmServer *server, FwmOutput *out, const struct wlr_box *region,
                    bool fly_effect) {
    if (!out || !out->wlr_output || !out->enabled) {
        toast_show(server, true, "screenshot failed: no screen to photograph");
        return;
    }

    ShotPending *p = calloc(1, sizeof(*p));
    if (!p) return;
    p->server = server;
    p->wlr_output = out->wlr_output;
    p->fly = fly_effect;
    if (region && region->width > 0 && region->height > 0) {
        double s = out->wlr_output->scale > 0.0f ? out->wlr_output->scale : 1.0;
        p->region = (struct wlr_box){
            (int)(region->x * s), (int)(region->y * s),
            (int)(region->width * s + 0.5), (int)(region->height * s + 0.5) };
    } else {
        p->region = (struct wlr_box){ 0, 0,
            out->wlr_output->width, out->wlr_output->height };
    }

    p->commit.notify = pending_commit;
    wl_signal_add(&out->wlr_output->events.commit, &p->commit);
    p->destroy.notify = pending_output_destroy;
    wl_signal_add(&out->wlr_output->events.destroy, &p->destroy);

    /* Nothing may have changed on screen, and a frame nobody asks for never
     * comes — so ask. */
    wlr_output_schedule_frame(out->wlr_output);
}

void screenshot_full(FwmServer *server) {
    /* No peel-off here: a whole screen taking off is a lot of motion for
     * something people press every few minutes. The region shot earns it. */
    capture(server, server_active_output(server), NULL, false);
}

/* ── the region selector ─────────────────────────────────────────────── */

/* Dimming is four rectangles around the selection rather than one with a hole
 * in it: the scene graph has no hole, and four cheap rects beat repainting a
 * screen-sized cairo surface on every motion event. */
struct FwmShotPicker {
    FwmServer *server;
    FwmOutput *out;                 /* the monitor the drag started on */
    struct wlr_scene_tree *tree;
    struct wlr_scene_rect *dim[4];  /* above, below, left, right of the box */
    struct wlr_scene_rect *edge[4]; /* the box's own outline */
    struct wlr_scene_buffer *label;
    bool dragging;
    double x0, y0, x1, y1;          /* layout coordinates */
    char label_text[64];
};

static void picker_label_draw(cairo_t *cr, int w, int h, void *user_data) {
    const char *text = user_data;
    const FwmTheme *thm = theme_get();

    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], 0.92);
    cairo_move_to(cr, 8, 0);
    cairo_line_to(cr, w - 8, 0);
    cairo_line_to(cr, w, 8);
    cairo_line_to(cr, w, h - 8);
    cairo_line_to(cr, w - 8, h);
    cairo_line_to(cr, 8, h);
    cairo_line_to(cr, 0, h - 8);
    cairo_line_to(cr, 0, 8);
    cairo_close_path(cr);
    cairo_fill(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("sans 9");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(layout, text, -1);
    pango_layout_set_width(layout, (w - 16) * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
    cairo_move_to(cr, (w - tw) / 2.0, (h - th) / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

/* The selection in layout coordinates, normalised so a drag in any direction
 * gives the same box. */
static struct wlr_box picker_box(const struct FwmShotPicker *p) {
    int x = (int)(p->x0 < p->x1 ? p->x0 : p->x1);
    int y = (int)(p->y0 < p->y1 ? p->y0 : p->y1);
    int w = (int)(p->x0 < p->x1 ? p->x1 - p->x0 : p->x0 - p->x1);
    int h = (int)(p->y0 < p->y1 ? p->y1 - p->y0 : p->y0 - p->y1);
    return (struct wlr_box){ x, y, w, h };
}

static void rect_place(struct wlr_scene_rect *r, int x, int y, int w, int h) {
    if (!r) return;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    wlr_scene_rect_set_size(r, w, h);
    wlr_scene_node_set_position(&r->node, x, y);
}

static void picker_redraw(struct FwmShotPicker *p) {
    struct wlr_box o = p->out->box;
    struct wlr_box s = p->dragging ? picker_box(p)
                                   : (struct wlr_box){ o.x, o.y, 0, 0 };

    /* Clamp to the monitor: the pointer may wander onto another screen
     * mid-drag, and the picture can only come from this one. */
    struct wlr_box c = s;
    if (!wlr_box_intersection(&c, &s, &o)) c = (struct wlr_box){ o.x, o.y, 0, 0 };

    rect_place(p->dim[0], o.x, o.y, o.width, c.y - o.y);
    rect_place(p->dim[1], o.x, c.y + c.height, o.width, o.y + o.height - (c.y + c.height));
    rect_place(p->dim[2], o.x, c.y, c.x - o.x, c.height);
    rect_place(p->dim[3], c.x + c.width, c.y, o.x + o.width - (c.x + c.width), c.height);

    rect_place(p->edge[0], c.x, c.y, c.width, 1);
    rect_place(p->edge[1], c.x, c.y + c.height - 1, c.width, c.height ? 1 : 0);
    rect_place(p->edge[2], c.x, c.y, 1, c.height);
    rect_place(p->edge[3], c.x + c.width - 1, c.y, c.width ? 1 : 0, c.height);

    if (!p->label) return;
    char text[64];
    if (p->dragging)
        snprintf(text, sizeof(text), "%d \xC3\x97 %d", c.width, c.height);
    else
        snprintf(text, sizeof(text), "drag a region \xC2\xB7 Esc cancels");
    if (strcmp(text, p->label_text) != 0) {
        snprintf(p->label_text, sizeof(p->label_text), "%s", text);
        cairo_overlay_update(p->label, picker_label_draw, p->label_text);
    }
    /* Under the box while there is room, inside its top-left when the drag has
     * reached the bottom of the screen. */
    int lx = c.x;
    int ly = c.y + c.height + 8;
    if (ly + LABEL_H > o.y + o.height) ly = c.y + c.height - LABEL_H - 8;
    if (lx + LABEL_W > o.x + o.width) lx = o.x + o.width - LABEL_W;
    if (lx < o.x) lx = o.x;
    if (ly < o.y) ly = o.y;
    wlr_scene_node_set_position(&p->label->node, lx, ly);
}

static void picker_close(FwmServer *server) {
    struct FwmShotPicker *p = server->shot_picker;
    if (!p) return;
    server->shot_picker = NULL;   /* first: the tree going away damages the
                                   * scene, and nothing may find a half-freed
                                   * picker while that is handled */
    if (p->label) cairo_overlay_destroy(p->label);
    if (p->tree) wlr_scene_node_destroy(&p->tree->node);
    free(p);
    /* Not at teardown, where the cursor is already gone. */
    if (server->cursor && server->cursor_mgr)
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
}

void screenshot_region(FwmServer *server) {
    if (server->shot_picker) { picker_close(server); return; }

    FwmOutput *out = server_active_output(server);
    if (!out) {
        toast_show(server, true, "screenshot failed: no screen to photograph");
        return;
    }

    struct FwmShotPicker *p = calloc(1, sizeof(*p));
    if (!p) return;
    p->server = server;
    p->out = out;
    p->tree = wlr_scene_tree_create(server->layer_overlay);
    if (!p->tree) { free(p); return; }

    const FwmTheme *thm = theme_get();
    const float dim[4] = { 0.0f, 0.0f, 0.0f, 0.38f };  /* premultiplied */
    const float edge[4] = { (float)thm->accent[0], (float)thm->accent[1],
                            (float)thm->accent[2], 1.0f };
    for (int i = 0; i < 4; i++) {
        p->dim[i]  = wlr_scene_rect_create(p->tree, 1, 1, dim);
        p->edge[i] = wlr_scene_rect_create(p->tree, 1, 1, edge);
    }
    p->label = cairo_overlay_create(p->tree, LABEL_W, LABEL_H);

    server->shot_picker = p;
    picker_redraw(p);
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "crosshair");
}

bool screenshot_selecting(FwmServer *server) {
    return server->shot_picker != NULL;
}

bool screenshot_handle_motion(FwmServer *server, double lx, double ly) {
    struct FwmShotPicker *p = server->shot_picker;
    if (!p) return false;
    if (p->dragging) {
        p->x1 = lx;
        p->y1 = ly;
        picker_redraw(p);
    }
    return true;
}

bool screenshot_handle_button(FwmServer *server, bool pressed) {
    struct FwmShotPicker *p = server->shot_picker;
    if (!p) return false;

    if (pressed) {
        p->x0 = p->x1 = server->cursor->x;
        p->y0 = p->y1 = server->cursor->y;
        p->dragging = true;
        picker_redraw(p);
        return true;
    }
    if (!p->dragging) return true;   /* the release of a click that began
                                      * elsewhere; swallow it and wait */

    struct wlr_box sel = picker_box(p);
    struct wlr_box out_box = p->out->box;
    FwmOutput *out = p->out;
    struct wlr_box hit;
    bool on_screen = wlr_box_intersection(&hit, &sel, &out_box);

    picker_close(server);   /* the overlay must be gone BEFORE the request: the
                             * picture is the next frame, dimming and all */

    /* A click with no drag in it is how people cancel out of a selector, and
     * a one-pixel sliver is never what was meant. */
    if (!on_screen || hit.width < 2 || hit.height < 2) return true;

    capture(server, out, &(struct wlr_box){
        hit.x - out_box.x, hit.y - out_box.y, hit.width, hit.height }, true);
    return true;
}

bool screenshot_handle_key(FwmServer *server, xkb_keysym_t sym) {
    if (!server->shot_picker) return false;
    if (sym == XKB_KEY_Escape) picker_close(server);
    return true;   /* everything else is swallowed: the windows a key would
                    * reach are under a selector the user is aiming with */
}

void screenshot_cleanup(FwmServer *server) {
    picker_close(server);
    fly_stop();
    if (toast.timer) {
        wl_event_source_remove(toast.timer);
        toast.timer = NULL;
    }
    if (toast.buffer) {
        cairo_overlay_destroy(toast.buffer);
        toast.buffer = NULL;
    }
}
