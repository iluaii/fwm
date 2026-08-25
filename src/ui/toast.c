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

/* One line at the bottom of the screen, held for a moment and gone.
 *
 * It began in screenshot.c, where a shot that saved silently was
 * indistinguishable from one that failed, and it is the same sentence the
 * battery wants when it is nearly empty: something happened, here is what, and
 * nothing to click. That is the whole of it — a compositor with a notification
 * daemon in it would be a different program, and this is not that.
 */

#include "toast.h"

#include "../server.h"
#include "../view.h"
#include "../server_internal.h"
#include "../theme.h"
#include "cairo_overlay.h"
#include "../glass.h"

/* How long it stays, and how it arrives and leaves. Private: nothing outside
 * this file has an opinion about the timing, only about where the line sits. */
#define TOAST_HOLD_MS  1800
#define TOAST_ANIM_MS  170.0
#define TOAST_RISE_PX  14.0
#define TOAST_CUT      12.0   /* corner chevron, same silhouette as the panels */

#include <pango/pangocairo.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

/* Module state rather than a server field: like the overlay animations it rides
 * on, only one is ever up, and nothing outside this file asks about it. */

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

    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2],
                          glass_fill(NULL, 0.94));
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

void toast_show(FwmServer *server, bool bad, const char *fmt, ...) {
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
    glass_attach(toast.buffer);   /* before make_static; see ui/hints.c */
    cairo_overlay_make_static(toast.buffer);
    cairo_overlay_animate_in(toast.buffer, TOAST_ANIM_MS, TOAST_RISE_PX);

    toast.timer = wl_event_loop_add_timer(wl_display_get_event_loop(server->wl_display),
                                          toast_expire, NULL);
    if (toast.timer) wl_event_source_timer_update(toast.timer, TOAST_HOLD_MS);
}

void toast_cleanup(void) {
    if (toast.timer) {
        wl_event_source_remove(toast.timer);
        toast.timer = NULL;
    }
    if (toast.buffer) {
        cairo_overlay_destroy(toast.buffer);
        toast.buffer = NULL;
    }
}
