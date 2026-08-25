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

#include "expo_menu.h"
#include "../theme.h"
#include "cairo_overlay.h"
#include "../glass.h"
#include <stdio.h>

#define MENU_W        212
#define MENU_PAD_X     14
#define MENU_PAD_Y     10
#define MENU_TITLE_H   24
#define MENU_ROW_H     30
#define MENU_CUT       10.0   /* corner chevron cut, matching the other panels */
#define MENU_H  (MENU_PAD_Y * 2 + MENU_TITLE_H + EXPO_MENU_ROW_COUNT * MENU_ROW_H)

/* There is only ever one of these open — it belongs to the desktop strip, which
 * is itself a single mode — so its contents live here rather than being threaded
 * through the caller. A redraw for a hover change must not lose the title, and
 * this is the cheapest way to guarantee it. */
static struct MenuCtx {
    char title[128];
    char mode[32];
    int hover;
} menu;

static const char *row_label(int row, char *buf, size_t cap) {
    switch (row) {
    case EXPO_MENU_ROW_GOTO:  return "Go to window";
    case EXPO_MENU_ROW_CLOSE: return "Close window";
    case EXPO_MENU_ROW_MODE:
        snprintf(buf, cap, "Desktop: %s", menu.mode[0] ? menu.mode : "physics");
        return buf;
    default:                  return "";
    }
}

static void panel_path(cairo_t *cr, double x, double y, double w, double h, double cut) {
    cairo_move_to(cr, x + cut, y);
    cairo_line_to(cr, x + w - cut, y);
    cairo_line_to(cr, x + w, y + cut);
    cairo_line_to(cr, x + w, y + h - cut);
    cairo_line_to(cr, x + w - cut, y + h);
    cairo_line_to(cr, x + cut, y + h);
    cairo_line_to(cr, x, y + h - cut);
    cairo_line_to(cr, x, y + cut);
    cairo_close_path(cr);
}

static void draw_menu(cairo_t *cr, int w, int h, void *user_data) {
    struct MenuCtx *ctx = user_data;
    const FwmTheme *thm = theme_get();

    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2],
                          glass_fill(NULL, 0.97));
    panel_path(cr, 0, 0, w, h, MENU_CUT);
    cairo_fill(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_width(layout, (w - 2 * MENU_PAD_X) * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

    /* The window's own title, dimmed: the menu acts on one window and has to
     * say which, because the card it was opened over may be a thumbnail the
     * size of a postage stamp. */
    cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
    pango_layout_set_text(layout, ctx->title, -1);
    cairo_move_to(cr, MENU_PAD_X, MENU_PAD_Y);
    pango_cairo_show_layout(cr, layout);

    for (int r = 0; r < EXPO_MENU_ROW_COUNT; r++) {
        double y = MENU_PAD_Y + MENU_TITLE_H + r * MENU_ROW_H;
        if (r == ctx->hover) {
            cairo_set_source_rgb(cr, thm->sel[0], thm->sel[1], thm->sel[2]);
            cairo_rectangle(cr, MENU_PAD_X - 6, y, w - 2 * (MENU_PAD_X - 6), MENU_ROW_H);
            cairo_fill(cr);
        }
        cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
        char rbuf[48];
        pango_layout_set_text(layout, row_label(r, rbuf, sizeof(rbuf)), -1);
        cairo_move_to(cr, MENU_PAD_X, y + (MENU_ROW_H - 20) / 2.0);
        pango_cairo_show_layout(cr, layout);
    }

    g_object_unref(layout);
}

void expo_menu_size(int *w, int *h) {
    if (w) *w = MENU_W;
    if (h) *h = MENU_H;
}

int expo_menu_hit(double x, double y) {
    if (x < 0 || x >= MENU_W || y < 0 || y >= MENU_H) return EXPO_MENU_ROW_NONE;
    double ry = y - (MENU_PAD_Y + MENU_TITLE_H);
    if (ry < 0) return EXPO_MENU_ROW_NONE;
    int row = (int)(ry / MENU_ROW_H);
    if (row < 0 || row >= EXPO_MENU_ROW_COUNT) return EXPO_MENU_ROW_NONE;
    return row;
}

struct wlr_scene_buffer *expo_menu_show(struct wlr_scene_tree *parent,
                                        int screen_w, int screen_h,
                                        double x, double y, const char *title,
                                        const char *mode) {
    struct wlr_scene_buffer *buf = cairo_overlay_create(parent, MENU_W, MENU_H);
    if (!buf) return NULL;
    glass_attach(buf);

    menu.hover = EXPO_MENU_ROW_NONE;
    snprintf(menu.title, sizeof(menu.title), "%s",
             title && title[0] ? title : "Window");
    snprintf(menu.mode, sizeof(menu.mode), "%s", mode ? mode : "");
    cairo_overlay_update(buf, draw_menu, &menu);

    /* Opened at the cursor, so it is the one panel in fwm that can be asked to
     * appear off-screen. Push it back rather than letting a row be unclickable. */
    if (x + MENU_W > screen_w) x = screen_w - MENU_W;
    if (y + MENU_H > screen_h) y = screen_h - MENU_H;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    wlr_scene_node_set_position(&buf->node, (int)x, (int)y);
    return buf;
}

void expo_menu_set_mode(struct wlr_scene_buffer *buf, const char *mode) {
    if (!buf) return;
    snprintf(menu.mode, sizeof(menu.mode), "%s", mode ? mode : "");
    cairo_overlay_update(buf, draw_menu, &menu);
}

void expo_menu_hover(struct wlr_scene_buffer *buf, int row) {
    if (!buf || menu.hover == row) return;
    menu.hover = row;
    cairo_overlay_update(buf, draw_menu, &menu);
}
