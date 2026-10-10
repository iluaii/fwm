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

#ifndef FWM_EXPO_MENU_H
#define FWM_EXPO_MENU_H

#include <wlr/types/wlr_scene.h>

/* The menu a right click opens on a window in the desktop strip.
 *
 * Deliberately not the modes menu with different rows: that one is anchored
 * under a fixed pill, animates switches and reads live compositor state every
 * frame. This one appears at the cursor, says what it will do to one window and
 * goes away again. Sharing the code would have meant making both configurable
 * in ways neither needs. */

enum {
    EXPO_MENU_ROW_NONE = -1,
    EXPO_MENU_ROW_GOTO = 0,
    EXPO_MENU_ROW_CLOSE,
    /* The desktop this window is on, and what it does with its windows.
     * A cycle rather than a submenu: there are three modes, and a menu that
     * opens another menu to choose between three things is worse than a row
     * that says which one it is on now. */
    EXPO_MENU_ROW_MODE,
    EXPO_MENU_ROW_COUNT,
};

/* Open at (x, y) in layout coordinates, naming `title` (the window's own, shown
 * dimmed above the rows). The menu is nudged back onto the monitor whose
 * top-left is (origin_x, origin_y) if it would hang off its right or bottom
 * edge. Returns NULL if it could not be created. */
struct wlr_scene_buffer *expo_menu_show(struct wlr_scene_tree *parent,
                                        int origin_x, int origin_y,
                                        int screen_w, int screen_h,
                                        double x, double y, const char *title,
                                        const char *mode);

/* Rewrite the mode row after cycling it, without closing the menu: the row is
 * a cycle, so it has to be able to say where the cycle has got to. */
void expo_menu_set_mode(struct wlr_scene_buffer *buf, const char *mode);

/* Redraw with a different row highlighted. `row` is EXPO_MENU_ROW_NONE when the
 * cursor is off the menu. No-op when nothing changed, so it is safe to call on
 * every motion event. */
void expo_menu_hover(struct wlr_scene_buffer *buf, int row);

/* Hit-test in MENU-LOCAL coordinates; EXPO_MENU_ROW_NONE outside any row. */
int expo_menu_hit(double x, double y);

/* Size of the panel, so a caller can tell whether a point is on it at all. */
void expo_menu_size(int *w, int *h);

#endif /* FWM_EXPO_MENU_H */
