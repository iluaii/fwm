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
 * each. Clicking a row puts a bucket of it in your hand (server_liquid.c);
 * the bin that appears at the right of the row under the pointer drains that
 * liquid off the desktop.
 *
 * Same chrome as the modes and stats menus — ui/modes.h exports the panel —
 * and the same open and close, because it opens in the place the modes menu
 * just closed and has to read as the next page of the same thing.
 */

#define LIQUIDS_MENU_ANIM_MS 170.0
#define LIQUIDS_MENU_RISE_PX  14.0
#define LIQUIDS_MENU_MAX 16

typedef struct {
    const char *name;
    float color[4];     /* premultiplied, as parse_hex_color leaves it */
    int desktop;        /* where it has been poured, -1 nowhere */
} LiquidsMenuItem;

typedef struct {
    LiquidsMenuItem item[LIQUIDS_MENU_MAX];
    int count;
    int hover;          /* the row under the pointer, -1 for none */
    bool hover_bin;     /* ...and the pointer is on its bin */
    int in_hand;        /* the row whose bucket is in the hand, -1 for none */
    double opacity;
} LiquidsMenuState;

/* Open, right-aligned at `right_x` under the tray of `screen` (layout
 * coordinates) — where the modes menu stood. */
struct wlr_scene_buffer *liquids_menu_show(struct wlr_scene_tree *parent,
                                           const struct wlr_box *screen,
                                           double right_x,
                                           const LiquidsMenuState *st);
void liquids_menu_redraw(struct wlr_scene_buffer *buf, const LiquidsMenuState *st);

/* Advance the row stagger; true while it is still running. */
bool liquids_menu_tick(struct wlr_scene_buffer *buf, const LiquidsMenuState *st, double dt);
bool liquids_menu_animating(void);

void liquids_menu_size(int count, int *w, int *h);

/* Hit-test in MENU-BUFFER-LOCAL coordinates: the row under the point, or -1,
 * and whether the point is on that row's bin. */
int liquids_menu_hit(int count, double x, double y, bool *bin);

#endif /* FWM_LIQUIDS_MENU_H */
