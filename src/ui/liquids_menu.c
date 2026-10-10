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

/* The liquids menu. See liquids_menu.h. */

#include "liquids_menu.h"
#include "cairo_overlay.h"
#include "modes.h"
#include "tray.h"
#include "../theme.h"
#include "../glass.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MENU_W        300
#define MENU_PAD      12.0
#define MENU_ROW_H    36.0
/* The caption over the rows and the line under them that says what the
 * buttons do: the bucket takes the pointer over, and a mode that does not say
 * how to leave it is a trap. */
#define CAPTION_H     24.0
#define FOOTER_H      28.0
#define BIN_W         28.0

#define MENU_H_FOR(n) ((int)(MENU_PAD * 2 + CAPTION_H + FOOTER_H + MENU_ROW_H * ((n) > 0 ? (n) : 1)))

#define ANIM_EPS 0.002

/* The bin under the pointer goes the red the toast uses when something did not
 * happen: draining is the one thing here that cannot be taken back. */
static const double COL_DRAIN[3] = { 0.93, 0.42, 0.38 };

static struct {
    double open;    /* 0..1, drives the row stagger */
    int    live;
} g_anim;

void liquids_menu_size(int count, int *w, int *h) {
    if (w) *w = MENU_W;
    if (h) *h = MENU_H_FOR(count);
}

static double row_y(int row) { return MENU_PAD + CAPTION_H + MENU_ROW_H * row; }

int liquids_menu_hit(int count, double x, double y, bool *bin) {
    if (bin) *bin = false;
    if (x < 0 || x > MENU_W || y < 0 || y > MENU_H_FOR(count)) return -1;
    for (int r = 0; r < count; r++) {
        double ry = row_y(r);
        if (y < ry || y >= ry + MENU_ROW_H) continue;
        if (bin) *bin = x >= MENU_W - MENU_PAD - BIN_W;
        return r;
    }
    return -1;
}

bool liquids_menu_animating(void) {
    return g_anim.live && g_anim.open < 1.0 - ANIM_EPS;
}

/* Rows come in from the top, each a little after the one above it — the
 * modes menu's stagger, so the page that replaces it arrives the same way. */
static double row_reveal(int row) {
    double p = (g_anim.open - row * 0.10) / 0.55;
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;
    double inv = 1.0 - p;
    return 1.0 - inv * inv * inv;
}

/* A bin: lid, handle, and a body narrowing to its foot, with two ribs. */
static void draw_bin(cairo_t *cr, double x, double y, double s) {
    double lw = fmax(1.5, s * 0.11);
    cairo_set_line_width(cr, lw);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, x + s * 0.06, y + s * 0.22);
    cairo_line_to(cr, x + s * 0.94, y + s * 0.22);
    cairo_stroke(cr);
    cairo_move_to(cr, x + s * 0.36, y + s * 0.22);
    cairo_line_to(cr, x + s * 0.40, y + s * 0.06);
    cairo_line_to(cr, x + s * 0.60, y + s * 0.06);
    cairo_line_to(cr, x + s * 0.64, y + s * 0.22);
    cairo_stroke(cr);
    cairo_move_to(cr, x + s * 0.16, y + s * 0.30);
    cairo_line_to(cr, x + s * 0.24, y + s * 0.96);
    cairo_line_to(cr, x + s * 0.76, y + s * 0.96);
    cairo_line_to(cr, x + s * 0.84, y + s * 0.30);
    cairo_stroke(cr);
    cairo_move_to(cr, x + s * 0.42, y + s * 0.42);
    cairo_line_to(cr, x + s * 0.43, y + s * 0.82);
    cairo_move_to(cr, x + s * 0.58, y + s * 0.42);
    cairo_line_to(cr, x + s * 0.57, y + s * 0.82);
    cairo_stroke(cr);
}

static void text_at(cairo_t *cr, PangoLayout *layout, const char *s,
                    double x, double y, double h, int *w_out) {
    int tw, th;
    pango_layout_set_text(layout, s, -1);
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_move_to(cr, x, y + (h - th) / 2.0);
    pango_cairo_show_layout(cr, layout);
    if (w_out) *w_out = tw;
}

static void set_font(PangoLayout *layout, const char *font) {
    PangoFontDescription *desc = pango_font_description_from_string(font);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
}

static void draw_menu(cairo_t *cr, int w, int h, void *user) {
    const LiquidsMenuState *st = user;
    const FwmTheme *thm = theme_get();

    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], st->opacity);
    modes_panel_path(cr, 0, 0, w, h, MODES_MENU_CHAMFER);
    cairo_fill(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);

    set_font(layout, "sans 8");
    cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
    text_at(cr, layout, "LIQUIDS", MENU_PAD, MENU_PAD, CAPTION_H, NULL);

    set_font(layout, "sans 10");
    for (int r = 0; r < st->count; r++) {
        const LiquidsMenuItem *it = &st->item[r];
        double ry = row_y(r);
        double rev = g_anim.live ? row_reveal(r) : 1.0;
        if (rev <= 0.001) continue;
        cairo_push_group(cr);
        cairo_save(cr);
        cairo_translate(cr, (1.0 - rev) * 14.0, 0.0);

        bool hover = r == st->hover;
        if (hover) {
            cairo_set_source_rgba(cr, thm->accent[0], thm->accent[1], thm->accent[2], 0.14);
            cairo_rectangle(cr, MENU_PAD * 0.5, ry + 2.0, MENU_W - MENU_PAD, MENU_ROW_H - 4.0);
            cairo_fill(cr);
        }

        /* The drop in the liquid's own colour: that IS the swatch. */
        float a = it->color[3];
        float k = a > 0.0f ? 1.0f / a : 0.0f;
        double icon_s = 16.0;
        cairo_set_source_rgb(cr, it->color[0] * k, it->color[1] * k, it->color[2] * k);
        modes_icon(cr, MODE_ICON_LIQUID, MENU_PAD, ry + (MENU_ROW_H - icon_s) / 2.0, icon_s);

        cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
        int nw = 0;
        text_at(cr, layout, it->name, MENU_PAD + icon_s + 12.0, ry, MENU_ROW_H, &nw);

        if (r == st->in_hand) {
            set_font(layout, "sans 8");
            cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
            text_at(cr, layout, "in hand", MENU_PAD + icon_s + 12.0 + nw + 10.0, ry, MENU_ROW_H, NULL);
            set_font(layout, "sans 10");
        }

        double right = MENU_W - MENU_PAD;
        if (hover) {
            /* The bin, only on the row under the pointer, so a list of liquids
             * is not a column of bins waiting for a slip of the hand. */
            const double *c = st->hover_bin ? COL_DRAIN : thm->muted;
            cairo_set_source_rgb(cr, c[0], c[1], c[2]);
            double bs = 15.0;
            draw_bin(cr, right - BIN_W + (BIN_W - bs) / 2.0, ry + (MENU_ROW_H - bs) / 2.0, bs);
        } else if (it->desktop >= 0) {
            char where[32];
            snprintf(where, sizeof where, "desktop %d", it->desktop + 1);
            set_font(layout, "sans 9");
            int tw, th;
            pango_layout_set_text(layout, where, -1);
            pango_layout_get_pixel_size(layout, &tw, &th);
            cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
            cairo_move_to(cr, right - tw, ry + (MENU_ROW_H - th) / 2.0);
            pango_cairo_show_layout(cr, layout);
            set_font(layout, "sans 10");
        }

        cairo_restore(cr);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, rev);
    }

    set_font(layout, "sans 8");
    cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
    text_at(cr, layout, "left: pour   ·   right: drain   ·   Esc: put it down",
            MENU_PAD, h - MENU_PAD - FOOTER_H, FOOTER_H, NULL);

    g_object_unref(layout);
}

struct wlr_scene_buffer *liquids_menu_show(struct wlr_scene_tree *parent,
                                           const struct wlr_box *screen,
                                           double right_x,
                                           const LiquidsMenuState *st) {
    int wx = (int)lround(right_x - MENU_W);
    if (wx + MENU_W > screen->x + screen->width - 8)
        wx = screen->x + screen->width - 8 - MENU_W;
    if (wx < screen->x + 8) wx = screen->x + 8;
    int wy = screen->y + TRAY_BOTTOM + 6;

    g_anim.open = 0.0;
    g_anim.live = 1;

    struct wlr_scene_buffer *buf = cairo_overlay_create(parent, MENU_W, MENU_H_FOR(st->count));
    if (buf) {
        wlr_scene_node_set_position(&buf->node, wx, wy);
        cairo_overlay_update(buf, draw_menu, (void *)st);
        glass_attach(buf);
        cairo_overlay_animate_in(buf, LIQUIDS_MENU_ANIM_MS, -LIQUIDS_MENU_RISE_PX);
    }
    return buf;
}

void liquids_menu_redraw(struct wlr_scene_buffer *buf, const LiquidsMenuState *st) {
    if (buf) cairo_overlay_update(buf, draw_menu, (void *)st);
}

bool liquids_menu_tick(struct wlr_scene_buffer *buf, const LiquidsMenuState *st, double dt) {
    if (!buf || !g_anim.live || g_anim.open >= 1.0 - ANIM_EPS) return false;
    /* Capped at one 60Hz frame, for the reason ui/modes.c gives: the first
     * tick after the idle heartbeat would otherwise carry 200ms. */
    if (dt > 1.0 / 60.0) dt = 1.0 / 60.0;
    g_anim.open += dt * 3.6;
    if (g_anim.open > 1.0) g_anim.open = 1.0;
    liquids_menu_redraw(buf, st);
    return true;
}
