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

#ifndef FWM_EXPO_HINTS_H
#define FWM_EXPO_HINTS_H

#include <stdbool.h>
#include <wlr/types/wlr_scene.h>

/* The strip's own keys, along the bottom of the screen while it is open.
 *
 * Its own panel rather than a line in the `show_hints` cheat-sheet, because
 * these are not binds: they belong to the mode and are not in anyone's config,
 * so nothing else could have found them to list. And on screen rather than in
 * the documentation, because a mode you enter once a day is a mode whose keys
 * you have forgotten by the next time.
 *
 * Two sets: what the strip always does, and — only at the far zoom step, where
 * the camera is allowed off its seat — what flying around it costs. */

/* Create the panel, centred along the bottom. `flight` picks the second set. */
/* `origin_x`/`origin_y` place the bar on the strip's own monitor: it lives in
 * the shared overlay tree, so unlike the rest of the strip it does not inherit
 * that monitor's position. */
struct wlr_scene_buffer *expo_hints_show(struct wlr_scene_tree *parent,
                                         int origin_x, int origin_y,
                                         int screen_w, int screen_h, bool flight);

/* Redraw for a change of zoom step. Cheap and idempotent: it only redraws when
 * the set actually changed. */
/* Swap the panel for the orrery's verbs, and back. */
void expo_hints_set_orrery(struct wlr_scene_buffer *buf, bool orrery);

void expo_hints_set_flight(struct wlr_scene_buffer *buf, int screen_w,
                           int screen_h, bool flight);

/* Where the bar sits, from 0 (fully below the bottom edge, out of the way) to
 * 1 (in place). The caller eases it: the bar shows itself when the strip opens,
 * takes itself away once it has been read, and comes back when the cursor asks
 * for it. */
void expo_hints_place(struct wlr_scene_buffer *buf, int screen_w, int screen_h,
                      double reveal);

/* Is the cursor in the band along the bottom that brings the bar back? Wider
 * than the bar itself is tall, because it is asked for by aiming roughly at
 * where it was, not by hitting it. */
bool expo_hints_hit(int screen_h, double ly);

#endif /* FWM_EXPO_HINTS_H */
