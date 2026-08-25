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

#include "osd.h"
#include "../theme.h"
#include "cairo_overlay.h"
#include "modes.h"
#include "../server.h"
#include "../glass.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The readout for a value being turned: a name, what it is worth now, and a
 * bar saying how far through its range that is.
 *
 * It exists because `set:` made the knob a dial for every option in the table,
 * and a dial with no face is a dial you turn while watching the wallpaper for
 * a hint that anything happened. Nothing else in fwm reports a number that
 * changes in the hand — the tray shows state, not a value mid-turn — so this
 * panel is small, low, and gone a second after the turning stops.
 *
 * It is drawn on the tray's terms (the same chamfered island, the same
 * colours) so that the compositor keeps one visual vocabulary rather than
 * growing a second one for its second panel-that-is-not-a-menu. */

#define OSD_W        340
#define OSD_H         76
#define OSD_MARGIN    90.0   /* above the bottom edge of the screen */
#define OSD_HOLD_S     1.2   /* how long it stays after the last turn */
#define OSD_ANIM_MS  140.0
#define OSD_RISE_PX   10.0
#define BAR_H          6.0
#define PAD           20.0

struct Osd {
    struct FwmServer *server;

    struct wlr_scene_buffer *overlay;
    /* The panel on its way out, owned by the animation; kept only so a new
     * turn can cut it short instead of fading behind its own replacement. */
    struct wlr_scene_buffer *closing;

    char   label[64];
    char   value[64];
    double frac;      /* < 0 = no bar */
    bool   at_end;
    double hold;      /* seconds left before it goes */
};

static void closing_done(void *data) {
    Osd *o = data;
    o->closing = NULL;
}

static void closing_cancel(Osd *o) {
    if (!o->closing) return;
    struct wlr_scene_buffer *buf = o->closing;
    o->closing = NULL;
    cairo_overlay_destroy(buf);
}

static void show_at(cairo_t *cr, PangoLayout *layout, const char *font,
                    const char *text, double x, double y, int right_align) {
    PangoFontDescription *desc = pango_font_description_from_string(font);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(layout, text, -1);
    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_move_to(cr, right_align ? x - tw : x, y - th / 2.0);
    pango_cairo_show_layout(cr, layout);
}

static void draw_osd(cairo_t *cr, int w, int h, void *data) {
    Osd *o = data;
    const FwmTheme *thm = theme_get();
    /* The launcher's opacity, not the tray's: this is a panel that appears over
     * whatever is on screen and has to be read in the second it is up, while
     * the tray is a permanent strip people turn down to see their wallpaper
     * through. A readout at tray_opacity = 0.5 over a bright picture is a
     * readout you cannot read — that is a real setting, and it was the first
     * one this was tried against. */
    double alpha = glass_fill(&o->server->config,
                              o->server->config.decor.launcher_opacity);

    modes_panel_path(cr, 0.0, 0.0, (double)w, (double)h, MODES_MENU_CHAMFER);
    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], alpha);
    cairo_fill(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);

    /* The name is the quiet half: you know which dial you are turning, and it
     * is the number you are looking at. */
    cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
    show_at(cr, layout, "sans 10", o->label, PAD, 24.0, 0);
    cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
    show_at(cr, layout, "sans bold 13", o->value, w - PAD, 23.0, 1);

    if (o->frac >= 0.0) {
        double x0 = PAD, x1 = w - PAD, y = h - PAD - BAR_H / 2.0;
        double f = o->frac < 0.0 ? 0.0 : (o->frac > 1.0 ? 1.0 : o->frac);

        cairo_set_line_width(cr, BAR_H);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_new_path(cr);
        cairo_move_to(cr, x0, y);
        cairo_line_to(cr, x1, y);
        cairo_set_source_rgb(cr, thm->dim[0], thm->dim[1], thm->dim[2]);
        cairo_stroke(cr);

        /* The fill starts at the left end even when the value is at the
         * minimum, because a bar of zero length reads as a bar that failed to
         * draw. One cap's worth is the least it can say. */
        cairo_new_path(cr);
        cairo_move_to(cr, x0, y);
        cairo_line_to(cr, x0 + (x1 - x0) * f, y);
        cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
        cairo_stroke(cr);

        /* At the end of the range the far cap lights up: the knob kept turning
         * and the number did not, and that is worth seeing rather than
         * guessing at from a bar that stopped moving. */
        if (o->at_end) {
            cairo_new_path(cr);
            cairo_arc(cr, f > 0.5 ? x1 : x0, y, BAR_H, 0.0, 2.0 * M_PI);
            cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
            cairo_fill(cr);
        }
    }

    g_object_unref(layout);
}

Osd *osd_create(struct FwmServer *server) {
    Osd *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->server = server;
    o->frac = -1.0;
    return o;
}

static void osd_hide_now(Osd *o) {
    if (o->overlay) {
        cairo_overlay_destroy(o->overlay);
        o->overlay = NULL;
    }
    o->hold = 0.0;
}

void osd_destroy(Osd *o) {
    if (!o) return;
    osd_hide_now(o);
    closing_cancel(o);
    free(o);
}

void osd_show(Osd *o, const char *label, const char *value, double frac, bool at_end) {
    if (!o) return;
    FwmServer *server = o->server;

    snprintf(o->label, sizeof(o->label), "%s", label ? label : "");
    snprintf(o->value, sizeof(o->value), "%s", value ? value : "");
    o->frac   = frac;
    o->at_end = at_end;
    o->hold   = OSD_HOLD_S;

    if (!o->overlay) {
        closing_cancel(o);
        o->overlay = cairo_overlay_create(server->layer_overlay, OSD_W, OSD_H);
        if (!o->overlay) return;
        glass_attach(o->overlay);
        /* The monitor's box, not the column's: on a second screen of another
         * size the pill would sit off-centre and, on a shorter one, below the
         * bottom edge entirely. */
        struct wlr_box screen;
        server_active_output_box(server, &screen);
        int px = screen.x + (screen.width - OSD_W) / 2;
        int py = screen.y + screen.height - OSD_H - (int)OSD_MARGIN;
        wlr_scene_node_set_position(&o->overlay->node, px, py);
        cairo_overlay_animate_in(o->overlay, OSD_ANIM_MS, OSD_RISE_PX);
    }
    /* Redrawn here, not on the tick: the number must be on screen for the
     * detent that produced it, and the tick is only what takes it away. */
    cairo_overlay_update(o->overlay, draw_osd, o);
}

void osd_tick(Osd *o, double dt) {
    if (!o || !o->overlay) return;
    o->hold -= dt;
    if (o->hold > 0.0) return;

    closing_cancel(o);
    o->closing = o->overlay;
    o->overlay = NULL;
    cairo_overlay_animate_out(o->closing, OSD_ANIM_MS, OSD_RISE_PX, closing_done, o);
}

bool osd_busy(Osd *o) { return o && o->overlay; }
