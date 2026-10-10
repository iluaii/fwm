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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MENU_W        320
#define MENU_PAD      12.0
#define MENU_ROW_H    46.0
/* The caption over the rows — with the + that makes a new liquid at its right
 * — and the line under them that says what the buttons do: the bucket takes
 * the pointer over, and a mode that does not say how to leave it is a trap. */
#define CAPTION_H     26.0
#define FOOTER_H      28.0
#define ICON_W        28.0     /* the gear's and the bin's columns, and the + */

#define MENU_H_FOR(n) ((int)(MENU_PAD * 2 + CAPTION_H + FOOTER_H + MENU_ROW_H * ((n) > 0 ? (n) : 1)))

#define ANIM_EPS 0.002

/* The bin under the pointer goes the red the toast uses when something did not
 * happen: removing a liquid is the one thing here that cannot be taken back. */
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
static double bin_x(void)  { return MENU_W - MENU_PAD - ICON_W; }
static double gear_x(void) { return MENU_W - MENU_PAD - 2.0 * ICON_W; }

int liquids_menu_hit(int count, double x, double y, int *part) {
    if (part) *part = LIQUIDS_PART_NONE;
    if (x < 0 || x > MENU_W || y < 0 || y > MENU_H_FOR(count)) return -1;
    if (y >= MENU_PAD && y < MENU_PAD + CAPTION_H && x >= bin_x()) {
        if (part) *part = LIQUIDS_PART_PLUS;
        return -1;
    }
    for (int r = 0; r < count; r++) {
        double ry = row_y(r);
        if (y < ry || y >= ry + MENU_ROW_H) continue;
        if (part) *part = x >= bin_x()  ? LIQUIDS_PART_BIN
                        : x >= gear_x() ? LIQUIDS_PART_GEAR : LIQUIDS_PART_ROW;
        return r;
    }
    return -1;
}

bool liquids_menu_animating(void) {
    return g_anim.live && g_anim.open < 1.0 - ANIM_EPS;
}

/* Rows come in from the top, each a little after the one above it — the
 * modes menu's stagger, so the page that replaces it arrives the same way,
 * and no more after it than lets the last of `rows` land as the clock
 * reaches 1: with more than five liquids the last ones never came in. */
static double row_reveal(int row, int rows) {
    double stagger = rows > 1 ? fmin(0.10, (1.0 - 0.55) / (rows - 1)) : 0.0;
    double p = (g_anim.open - row * stagger) / 0.55;
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

/* A gear: eight teeth round a ring, with the hole left open. */
static void draw_gear(cairo_t *cr, double x, double y, double s) {
    double cx = x + s / 2.0, cy = y + s / 2.0;
    double ro = s * 0.50, ri = s * 0.36, rh = s * 0.16;
    cairo_new_path(cr);
    for (int i = 0; i < 8; i++) {
        double a0 = (i - 0.22) * M_PI / 4.0, a1 = (i + 0.22) * M_PI / 4.0;
        double b0 = (i + 0.32) * M_PI / 4.0, b1 = (i + 0.68) * M_PI / 4.0;
        if (i == 0) cairo_move_to(cr, cx + ro * cos(a0), cy + ro * sin(a0));
        else        cairo_line_to(cr, cx + ro * cos(a0), cy + ro * sin(a0));
        cairo_line_to(cr, cx + ro * cos(a1), cy + ro * sin(a1));
        cairo_line_to(cr, cx + ri * cos(b0), cy + ri * sin(b0));
        cairo_line_to(cr, cx + ri * cos(b1), cy + ri * sin(b1));
    }
    cairo_close_path(cr);
    cairo_new_sub_path(cr);
    cairo_arc_negative(cr, cx, cy, rh, 2.0 * M_PI, 0.0);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_fill(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
}

static void draw_plus(cairo_t *cr, double x, double y, double s) {
    double lw = fmax(2.0, s * 0.14);
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, x + s / 2.0, y + lw);
    cairo_line_to(cr, x + s / 2.0, y + s - lw);
    cairo_move_to(cr, x + lw, y + s / 2.0);
    cairo_line_to(cr, x + s - lw, y + s / 2.0);
    cairo_stroke(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

static void text_at(cairo_t *cr, PangoLayout *layout, const char *s,
                    double x, double y, double h, int *w_out) {
    int tw, th;
    pango_layout_set_text(layout, s, -1);
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_move_to(cr, x, y + (h - th) / 2.0);
    pango_cairo_show_layout(cr, layout);
    /* show_layout leaves the move_to's point as the current point, and the
     * next arc anyone draws would be joined to it by a straight line. */
    cairo_new_path(cr);
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

    /* The +, over the bins' column. */
    const double *pc = st->full ? thm->dim
                     : st->hover_part == LIQUIDS_PART_PLUS ? thm->accent : thm->muted;
    cairo_set_source_rgb(cr, pc[0], pc[1], pc[2]);
    double ps = 13.0;
    draw_plus(cr, bin_x() + (ICON_W - ps) / 2.0, MENU_PAD + (CAPTION_H - ps) / 2.0, ps);

    for (int r = 0; r < st->count; r++) {
        const LiquidsMenuItem *it = &st->item[r];
        double ry = row_y(r);
        double rev = g_anim.live ? row_reveal(r, st->count) : 1.0;
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
        double icon_s = 18.0;
        cairo_set_source_rgb(cr, it->color[0], it->color[1], it->color[2]);
        modes_icon(cr, MODE_ICON_LIQUID, MENU_PAD, ry + (MENU_ROW_H - icon_s) / 2.0, icon_s);

        double tx = MENU_PAD + icon_s + 12.0;
        double text_right = gear_x() - 6.0;
        cairo_save(cr);
        cairo_rectangle(cr, tx, ry, text_right - tx, MENU_ROW_H);
        cairo_clip(cr);
        set_font(layout, "sans 10");
        cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
        int nw = 0;
        text_at(cr, layout, it->name, tx, ry + 4.0, 20.0, &nw);
        set_font(layout, "sans 8");
        if (r == st->in_hand) {
            cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
            text_at(cr, layout, "in hand", tx + nw + 10.0, ry + 4.0, 20.0, NULL);
        } else if (it->desktop >= 0) {
            char where[32];
            snprintf(where, sizeof where, "on desktop %d", it->desktop + 1);
            cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
            text_at(cr, layout, where, tx + nw + 10.0, ry + 4.0, 20.0, NULL);
        }
        cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
        text_at(cr, layout, it->desc, tx, ry + 22.0, 18.0, NULL);
        cairo_restore(cr);

        if (hover) {
            /* The gear and the bin only on the row under the pointer, so a
             * list of liquids is not a column of bins waiting for a slip of
             * the hand. */
            double is = 15.0, iy = ry + (MENU_ROW_H - is) / 2.0;
            const double *gc = st->hover_part == LIQUIDS_PART_GEAR ? thm->accent : thm->muted;
            cairo_set_source_rgb(cr, gc[0], gc[1], gc[2]);
            draw_gear(cr, gear_x() + (ICON_W - is) / 2.0, iy, is);
            if (it->removable) {
                const double *bc = st->hover_part == LIQUIDS_PART_BIN ? COL_DRAIN : thm->muted;
                cairo_set_source_rgb(cr, bc[0], bc[1], bc[2]);
                draw_bin(cr, bin_x() + (ICON_W - is) / 2.0, iy, is);
            }
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

struct wlr_scene_buffer *liquids_menu_refit(struct wlr_scene_buffer *buf,
                                            const LiquidsMenuState *st) {
    if (!buf) return NULL;
    struct wlr_buffer *pixels = cairo_overlay_buffer(buf);
    if (pixels && pixels->height == MENU_H_FOR(st->count)) {
        liquids_menu_redraw(buf, st);
        return buf;
    }
    struct wlr_scene_tree *parent = buf->node.parent;
    int x = buf->node.x, y = buf->node.y;
    cairo_overlay_destroy(buf);
    struct wlr_scene_buffer *fresh = cairo_overlay_create(parent, MENU_W, MENU_H_FOR(st->count));
    if (fresh) {
        wlr_scene_node_set_position(&fresh->node, x, y);
        cairo_overlay_update(fresh, draw_menu, (void *)st);
        glass_attach(fresh);
    }
    return fresh;
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
