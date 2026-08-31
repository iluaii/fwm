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

#ifndef FWM_THEME_H
#define FWM_THEME_H

#include "config.h"

/* Every colour the UI overlays draw with. The overlays read a theme instead of
 * holding their own constants, so a palette change repaints the whole system
 * consistently. One theme per monitor: with a wallpaper each, a screen is drawn
 * in the colours of the picture it is showing and never in the other one's.
 *
 * With [decor] color_source = "wallpaper" the island fill is tinted toward the
 * wallpaper's dominant hue and the accent is lifted from its most vivid
 * colour. Text stays put: the fill is always kept dark, so contrast never
 * depends on the image. */
typedef struct {
    double pill[3];   /* island / panel fill */
    double sel[3];    /* launcher selection row */
    double text[3];   /* primary text */
    double muted[3];  /* secondary text */
    double dim[3];    /* empty desktop dot */
    double accent[3]; /* active desktop marker, tab underline, focus border */

    /* Border colours for wlr_scene_rect — PREMULTIPLIED, matching
     * parse_hex_color's output. */
    float border_active[4];
    float border_inactive[4];
} FwmTheme;

/* The palette the overlays draw with. Never NULL: falls back to the built-in
 * dark scheme before theme_build() has run.
 *
 * With one wallpaper this is the whole story. With one per monitor it is the
 * palette of the screen the user is on — which is the right answer for
 * everything that opens where the hand is (the launcher, the ring, the OSD)
 * and the wrong one for anything that LIVES on a screen; those ask
 * theme_get_output() instead, or draw inside theme_use_output(). */
const FwmTheme *theme_get(void);

/* The palette of one monitor, derived from the wallpaper that monitor shows.
 * `output` is a connector name ("DP-2"); a screen with no [[wallpaper]] of its
 * own gets the un-named set's palette, and NULL or "" asks for that directly.
 * Never NULL. */
const FwmTheme *theme_get_output(const char *output);

/* Draw the next thing in one monitor's colours: theme_get() answers for
 * `output` until this is called with NULL. For overlays that are painted per
 * screen deep inside cairo helpers — the tray — where threading a palette down
 * through every draw_pill() would be the whole file. Not nestable, and it must
 * be cleared on the way out. */
void theme_use_output(const char *output);

/* Point theme_get() at the monitor the user is now on. Returns non-zero when
 * the palette actually changed, so the caller can repaint just then; two
 * screens whose images derive the same colours are not a change. */
int theme_set_active_output(const char *output);

/* Bumped by every theme_build(). Overlays that skip redraws by comparing a
 * content signature (the tray) must fold this in, or a reloaded palette would
 * not repaint until something else happened to change. */
unsigned theme_generation(void);

/* Build the themes from `cfg`: one per monitor that has a wallpaper of its own,
 * plus the un-named set's. With color_source = "wallpaper" each is read
 * downscaled (~10ms an image) and derives tint + accent;
 * anything that goes wrong falls back to the configured colours, reporting
 * through the caller's config diagnostics where it is the user's mistake.
 * Safe to call again on config reload. */
void theme_build(FwmConfig *cfg);

#endif /* FWM_THEME_H */
