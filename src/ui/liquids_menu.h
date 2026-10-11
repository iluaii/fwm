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

#ifndef FWM_LIQUIDS_MENU_H
#define FWM_LIQUIDS_MENU_H

#include <stdbool.h>
#include <wlr/util/box.h>
#include <wlr/types/wlr_scene.h>

/*
 * The menu the modes menu's Liquids row opens: every liquid there is, one row
 * each, its name over a line saying what it is like. Clicking a row puts a
 * bucket of it in your hand (server_liquid.c). The row under the pointer
 * shows a gear, which opens it in the liquid form (ui/liquid_form.h), and a
 * bin, which removes it — water has no bin. The + on the caption line makes a
 * new one, and the dry sponge beside it soaks up every liquid on the desktops.
 *
 * Same chrome as the modes and stats menus — ui/modes.h exports the panel —
 * and the same open and close, because it opens in the place the modes menu
 * just closed and has to read as the next page of the same thing.
 */

#define LIQUIDS_MENU_ANIM_MS 170.0
#define LIQUIDS_MENU_RISE_PX  14.0
#define LIQUIDS_MENU_MAX 32

typedef struct {
    char  name[32];
    float color[3];     /* straight RGB */
    char  desc[96];     /* what it is like: the line under the name */
    int   desktop;      /* where it has been poured, -1 nowhere */
    bool  removable;    /* water is not */
} LiquidsMenuItem;

/* What a point in the menu is on. */
enum {
    LIQUIDS_PART_NONE = 0,
    LIQUIDS_PART_ROW,   /* the liquid itself: take a bucket of it */
    LIQUIDS_PART_GEAR,  /* change it */
    LIQUIDS_PART_BIN,   /* remove it */
    LIQUIDS_PART_PLUS,  /* make a new one */
    LIQUIDS_PART_DRAIN, /* the sponge: drain every liquid poured */
};

typedef struct {
    LiquidsMenuItem item[LIQUIDS_MENU_MAX];
    int count;
    int hover;          /* the row under the pointer, -1 for none */
    int hover_part;     /* LIQUIDS_PART_*, what of it */
    int in_hand;        /* the row whose bucket is in the hand, -1 for none */
    bool full;          /* no room for another: the + is greyed */
    bool wet;           /* something is poured: else the sponge is greyed */
    double opacity;
} LiquidsMenuState;

/* Open, right-aligned at `right_x` under the tray of `screen` (layout
 * coordinates) — where the modes menu stood. */
struct wlr_scene_buffer *liquids_menu_show(struct wlr_scene_tree *parent,
                                           const struct wlr_box *screen,
                                           double right_x,
                                           const LiquidsMenuState *st);
void liquids_menu_redraw(struct wlr_scene_buffer *buf, const LiquidsMenuState *st);

/* Redraw, at a new height if the number of liquids changed while it was open:
 * the old panel is replaced by one of the right size in the same place, with
 * no animation, and the new one is returned. A buffer cannot grow, and rows
 * drawn past its bottom ran into the line under them. */
struct wlr_scene_buffer *liquids_menu_refit(struct wlr_scene_buffer *buf,
                                            const LiquidsMenuState *st);

/* Advance the row stagger; true while it is still running. */
bool liquids_menu_tick(struct wlr_scene_buffer *buf, const LiquidsMenuState *st, double dt);
bool liquids_menu_animating(void);

void liquids_menu_size(int count, int *w, int *h);

/* Hit-test in MENU-BUFFER-LOCAL coordinates: the row under the point (-1 for
 * none, including the + and the sponge on the caption line), and in `part` what of it. */
int liquids_menu_hit(int count, double x, double y, int *part);

#endif /* FWM_LIQUIDS_MENU_H */
