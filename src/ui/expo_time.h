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

#ifndef FWM_UI_EXPO_TIME_H
#define FWM_UI_EXPO_TIME_H

#include <stdbool.h>
#include <wlr/types/wlr_scene.h>

/* The desktop strip's "time" view, as pictures: the button in the bottom-left
 * corner that opens it, the page it opens, and the pill along the bottom that
 * picks what the page shows.
 *
 * The strip is space — every desktop at once. This is the same session along
 * the other axis: what happened to the windows, how much of it, and the whole
 * of it played back. Its own page over the strip rather than a mode of the
 * strip, because none of the strip's verbs apply to it and the strip is still
 * worth seeing behind the glass.
 *
 * Drawing and hit-testing only. Which section is up, what the keys do and when
 * any of it appears is expo_time.c's business. One strip means one of each, so
 * what they show lives in here rather than being threaded through the caller —
 * the same bargain expo_menu.c strikes. */

enum {
    EXPO_TIME_LOG = 0,      /* windows opened and closed */
    EXPO_TIME_STATS,        /* the counts */
    EXPO_TIME_LAPSE,        /* the session played back */
    EXPO_TIME_SECTIONS,
};

/* No section under the cursor. */
#define EXPO_TIME_NONE (-1)

/* All three are made on the strip's monitor, whose top-left is (ox, oy) in
 * layout coordinates and whose size is (sw, sh): they live in the shared
 * overlay tree to stay above the strip, so they do not inherit its position. */

/* The corner button. `reveal` slides it, 0 below the bottom edge, 1 in place. */
struct wlr_scene_buffer *expo_time_button_show(struct wlr_scene_tree *parent,
                                               int ox, int oy, int sw, int sh);
void expo_time_button_place(struct wlr_scene_buffer *buf, int ox, int oy,
                            int sw, int sh, double reveal);
/* Redraw for the cursor over it, or the view it opens being up. No-op when
 * neither changed, so it is safe on every motion event. */
void expo_time_button_set(struct wlr_scene_buffer *buf, bool hover, bool open);
bool expo_time_button_hit(struct wlr_scene_buffer *buf, double lx, double ly);

/* The page: frosted, filling the screen between the tray and the pill. It is
 * the glass and the keys only; what a section says is a layer of its own over
 * it, made the page's size and put where the page is, so that changing section
 * moves words and never redraws the panel. */
struct wlr_scene_buffer *expo_time_page_show(struct wlr_scene_tree *parent,
                                             int ox, int oy, int sw, int sh);
struct wlr_scene_buffer *expo_time_content_show(struct wlr_scene_tree *parent,
                                                struct wlr_scene_buffer *page,
                                                int section);

/* The section pill, centred along the bottom where the strip's key hints are
 * while the strip is up. */
struct wlr_scene_buffer *expo_time_tabs_show(struct wlr_scene_tree *parent,
                                             int ox, int oy, int sw, int sh,
                                             int section);
/* `pos` is where the lit section is: a whole number at rest, fractional while
 * it travels from one section to the next. No-op when nothing changed. */
void expo_time_tabs_set(struct wlr_scene_buffer *buf, double pos, int hover);
/* The section under a LAYOUT point, or EXPO_TIME_NONE. */
int expo_time_tabs_hit(struct wlr_scene_buffer *buf, double lx, double ly);

#endif /* FWM_UI_EXPO_TIME_H */
