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

/* The stats pill's menu. See stats_menu.h for what it is and why it is not a
 * few more rows in ui/modes.c. */

#include "stats_menu.h"
#include "cairo_overlay.h"
#include "modes.h"
#include "tray.h"
#include "../theme.h"
#include "../glass.h"

#include <math.h>
#include <string.h>

#define MENU_W        260
#define MENU_PAD      12.0
#define MENU_ROW_H    34.0

/* An empty [stats] is a menu with nothing in it, and a panel of pure padding
 * reads as a bug. One row's worth of height carries the "no sensors" line. */
#define MENU_H_FOR(rows) ((int)(MENU_PAD * 2 + MENU_ROW_H * ((rows) > 0 ? (rows) : 1)))

#define ANIM_SPEED   16.0
#define ANIM_EPS      0.002

/* File-static for the same reason the modes menu's is: there is exactly one
 * stats menu, and it is destroyed on close. */
static struct {
    double sw[STATS_MAX_ITEMS];  /* 0 = off, 1 = on */
    double open;                 /* 0..1, drives the row stagger */
    int    moving;
    int    live;
} g_anim;

void stats_menu_size(const FwmStats *stats, int *w, int *h) {
    if (w) *w = MENU_W;
    if (h) *h = MENU_H_FOR(stats_count(stats));
}

static double row_y(int row) { return MENU_PAD + MENU_ROW_H * row; }

int stats_menu_hit(const FwmStats *stats, double x, double y) {
    int n = stats_count(stats);
    if (x < 0 || x > MENU_W || y < 0 || y > MENU_H_FOR(n)) return -1;
    for (int r = 0; r < n; r++) {
        double ry = row_y(r);
        if (y >= ry && y < ry + MENU_ROW_H) return r;
    }
    return -1;
}

bool stats_menu_animating(void) {
    return g_anim.live && (g_anim.moving || g_anim.open < 1.0 - ANIM_EPS);
}

/* Rows come in from the top, each a little after the one above it. */
static double row_reveal(int row) {
    double p = (g_anim.open - row * 0.10) / 0.55;
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;
    double inv = 1.0 - p;
    return 1.0 - inv * inv * inv;
}

/* Snap the switches to the current state with the menu closed: opening
 * animates the rows in but NOT the switches, or every open would look like the
 * user had just flipped everything. */
static void anim_reset(const FwmStats *stats) {
    int n = stats_count(stats);
    for (int i = 0; i < STATS_MAX_ITEMS; i++) {
        const StatsItem *it = i < n ? stats_item(stats, i) : NULL;
        g_anim.sw[i] = (it && it->enabled) ? 1.0 : 0.0;
    }
    g_anim.open   = 0.0;
    g_anim.moving = 1;
    g_anim.live   = 1;
}

struct MenuCtx { const FwmStats *stats; double opacity; };

static void draw_menu(cairo_t *cr, int w, int h, void *user) {
    struct MenuCtx *ctx = user;
    const FwmTheme *thm = theme_get();
    int n = stats_count(ctx->stats);

    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], ctx->opacity);
    modes_panel_path(cr, 0, 0, w, h, MODES_MENU_CHAMFER);
    cairo_fill(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    if (n == 0) {
        int tw, th;
        pango_layout_set_text(layout, "no sensors in [stats]", -1);
        pango_layout_get_pixel_size(layout, &tw, &th);
        cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
        cairo_move_to(cr, MENU_PAD, row_y(0) + (MENU_ROW_H - th) / 2.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
        return;
    }

    for (int r = 0; r < n; r++) {
        const StatsItem *it = stats_item(ctx->stats, r);
        if (!it) continue;
        double ry = row_y(r);

        double rev = g_anim.live ? row_reveal(r) : 1.0;
        if (rev <= 0.001) continue;
        /* One group per row, so the row's icon, name, value and switch share a
         * single alpha instead of each fading on its own schedule. */
        cairo_push_group(cr);
        cairo_save(cr);
        cairo_translate(cr, (1.0 - rev) * 14.0, 0.0);

        int tw, th;
        pango_layout_set_text(layout, it->name, -1);
        pango_layout_get_pixel_size(layout, &tw, &th);
        /* A sensor this machine cannot answer — a gpu with no busy percentage —
         * is shown greyed rather than hidden: "your card does not report this"
         * is information, and a row that silently is not there looks like a
         * config that did not load. */
        if (it->available) cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
        else               cairo_set_source_rgb(cr, thm->dim[0], thm->dim[1], thm->dim[2]);
        cairo_move_to(cr, MENU_PAD, ry + (MENU_ROW_H - th) / 2.0);
        pango_cairo_show_layout(cr, layout);

        /* The live value, right after the name: the row is a choice about what
         * to put in the tray, and the answer to "what does this one show" is
         * the reading itself. */
        const char *value = it->available ? (it->value[0] ? it->value : "…")
                                          : "unavailable";
        int vw, vh;
        pango_layout_set_text(layout, value, -1);
        pango_layout_get_pixel_size(layout, &vw, &vh);
        cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
        cairo_move_to(cr, MENU_W - MENU_PAD - MODES_SWITCH_W - 12.0 - vw,
                      ry + (MENU_ROW_H - vh) / 2.0);
        pango_cairo_show_layout(cr, layout);

        modes_switch(cr, MENU_W - MENU_PAD - MODES_SWITCH_W,
                     ry + (MENU_ROW_H - MODES_SWITCH_H) / 2.0,
                     g_anim.live ? g_anim.sw[r] : (it->enabled ? 1.0 : 0.0),
                     it->available ? ctx->opacity : ctx->opacity * 0.4);

        /* CTM back to what push_group saw BEFORE popping: the group pattern is
         * matrixed against that, and popping under the row's own slide would
         * apply the offset twice. */
        cairo_restore(cr);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, rev);
    }

    g_object_unref(layout);
}

struct wlr_scene_buffer *stats_menu_show(struct wlr_scene_tree *parent,
                                         const struct wlr_box *screen,
                                         double pill_x, double pill_w,
                                         const FwmStats *stats, double opacity) {
    struct MenuCtx ctx = { .stats = stats, .opacity = opacity };
    int menu_h = MENU_H_FOR(stats_count(stats));

    /* LEFT-aligned with the pill, where the modes menu is right-aligned with
     * its own: this pill is anchored to the desktop island on its left and
     * grows rightwards, so its left edge is the one that stays put. Clamped to
     * its own monitor, in layout coordinates, so the clamp can never drag the
     * menu onto a neighbouring screen. */
    int wx = (int)lround(pill_x);
    if (wx + MENU_W > screen->x + screen->width - 8)
        wx = screen->x + screen->width - 8 - MENU_W;
    if (wx < screen->x + 8) wx = screen->x + 8;
    int wy = screen->y + TRAY_BOTTOM + 6;
    (void)pill_w;

    anim_reset(stats);

    struct wlr_scene_buffer *buf = cairo_overlay_create(parent, MENU_W, menu_h);
    if (buf) {
        wlr_scene_node_set_position(&buf->node, wx, wy);
        cairo_overlay_update(buf, draw_menu, &ctx);
        glass_attach(buf);
        cairo_overlay_animate_in(buf, STATS_MENU_ANIM_MS, -STATS_MENU_RISE_PX);
    }
    return buf;
}

void stats_menu_redraw(struct wlr_scene_buffer *buf, const FwmStats *stats,
                       double opacity) {
    if (!buf) return;
    g_anim.moving = 1;
    struct MenuCtx ctx = { .stats = stats, .opacity = opacity };
    cairo_overlay_update(buf, draw_menu, &ctx);
}

bool stats_menu_tick(struct wlr_scene_buffer *buf, const FwmStats *stats,
                     double opacity, double dt) {
    if (!buf || !g_anim.live) return false;

    /* Capped at one 60Hz frame, for the reason ui/modes.c sets out at length:
     * the loop idles at 200ms when nothing moves, which is exactly the state
     * the menu is in when a switch is clicked, and an uncapped first step would
     * cover most of the travel in that one frame. */
    if (dt > 1.0 / 60.0) dt = 1.0 / 60.0;

    double k = 1.0 - exp(-ANIM_SPEED * dt);
    int moving = 0;
    int n = stats_count(stats);

    for (int r = 0; r < n && r < STATS_MAX_ITEMS; r++) {
        const StatsItem *it = stats_item(stats, r);
        double target = (it && it->enabled) ? 1.0 : 0.0;
        double d = target - g_anim.sw[r];
        if (fabs(d) < ANIM_EPS) { g_anim.sw[r] = target; continue; }
        g_anim.sw[r] += d * k;
        moving = 1;
    }

    if (g_anim.open < 1.0 - ANIM_EPS) {
        /* Linear: row_reveal puts the easing on each row, and easing this as
         * well would stack two curves and stall the stagger. */
        g_anim.open += dt * 3.6;
        if (g_anim.open > 1.0) g_anim.open = 1.0;
        moving = 1;
    }

    g_anim.moving = moving;
    if (!moving) return false;
    stats_menu_redraw(buf, stats, opacity);
    return true;
}
