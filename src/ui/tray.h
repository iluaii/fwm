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

#ifndef FWM_TRAY_H
#define FWM_TRAY_H

#include <wlr/types/wlr_scene.h>
#include <wlr/render/wlr_renderer.h>
#include "../defines.h"

#define TRAY_HEIGHT 28
/* The tray floats: it is inset from the top edge, so the strip it actually
 * occupies runs down to TRAY_BOTTOM, not to TRAY_HEIGHT. Anything reserving
 * space for the tray must use TRAY_BOTTOM, or the gap it leaves comes out
 * TRAY_MARGIN px short of the gap on every other side. */
#define TRAY_MARGIN 8
#define TRAY_BOTTOM (TRAY_MARGIN + TRAY_HEIGHT)

typedef struct {
    const char *win_name;
    double speed;
    double angle;
    double mass;
    int flying;
    int desktop_window_counts[FWM_DESKTOPS];
    int active_desktop;
    double active_pos; /* fractional desktop position (camera_x / screen_w):
                        * the underline marker glides with the camera slide */
    double opacity; /* island fill alpha 0..1 (decor.tray_opacity) */
    char kbd_layout[8]; /* short active-layout tag ("EN", "RU"); "" hides it */
    int error_count;    /* config problems; >0 draws the warning pill */
    int error_expanded; /* detail panel open — pill renders as active */
    /* Name of the active [mode.<name>] submap, or NULL in the root map. A mode
     * swallows every key it does not bind, so it MUST be visible: without this
     * the only symptom of standing in one is a keyboard that has gone dead. */
    const char *mode_name;
    /* Modes pill (see ui/modes.h). Read straight from the live compositor every
     * frame, so a mode changed by a keybind lights up here with nothing having
     * to notify the tray. */
    int modes_tiling;
    int modes_floating;
    int modes_gravity;
    int modes_cava;      /* CAVA_MODE_* */
    int modes_ring;      /* camera.wrap */
    int modes_open;      /* menu is showing — the pill renders as pressed */
} TrayData;

/* Everything the strip drew, in tray-buffer-local coordinates: where the
 * clickable islands landed, plus a signature of the contents so an unchanged
 * strip is not repainted.
 *
 * Owned by the caller — one per monitor. It used to be file-static, which was
 * true only while there was one strip in the world: two monitors draw two
 * strips of different widths showing different desktops, and the statics held
 * whichever was painted last, so every click and the modes menu's anchor came
 * out of the WRONG screen's geometry. */
typedef struct { double x, y, w, h; int valid; } TrayRect;

typedef struct {
    /* Redraw signature. Opaque outside tray.c; here so each strip owns one. */
    char sig_name[128];
    int  sig_speed, sig_angle, sig_mass10, sig_flying;
    int  sig_counts[FWM_DESKTOPS];
    int  sig_active_desktop, sig_pos_mil, sig_opacity1000, sig_minute;
    char sig_kbd[8];
    int  sig_errors, sig_err_expanded;
    int  sig_m_tiling, sig_m_floating, sig_m_gravity, sig_m_cava, sig_m_open;
    unsigned sig_theme_gen;
    int  have_sig;

    TrayRect err;    /* config-error pill */
    TrayRect modes;  /* modes pill */
    TrayRect desk;   /* desktop-indicator island */
    double desk_first_cx;  /* centre of indicator 0 */
    double desk_spacing;
} TrayStrip;

struct wlr_scene_buffer *tray_init(struct wlr_scene_tree *parent, int screen_width);
void tray_redraw(struct wlr_scene_buffer *tray_buf, const TrayData *data,
                 TrayStrip *strip);

/* Hit-test for the config-error pill, in tray-buffer-local coordinates.
 * Valid once the pill has been drawn; returns 0 when no pill is on screen. */
int tray_error_pill_hit(const TrayStrip *strip, double x, double y);

/* Desktop indicators. Both take TRAY-BUFFER-LOCAL coordinates: subtract
 * tray_buffer->node.x/y before calling, like the error pill.
 * tray_desktop_hit returns the desktop under the point (snapped to the nearest
 * indicator anywhere inside the island) or -1 when the point is elsewhere.
 * tray_desktop_island_hit is the same test without picking an index, for
 * scroll-over-the-island. */
int tray_desktop_hit(const TrayStrip *strip, double x, double y);
int tray_desktop_island_hit(const TrayStrip *strip, double x, double y);

/* Modes pill, same tray-buffer-local coordinates. Returns 0 when the pill is
 * not on screen — on a narrow screen it is dropped rather than drawn over the
 * neighbouring islands, and then it cannot be clicked either. */
int tray_modes_pill_hit(const TrayStrip *strip, double x, double y);

/* Where the pill starts, in tray-buffer-local x, so the menu can line up with
 * it. Valid only while tray_modes_pill_hit can succeed. */
double tray_modes_pill_x(const TrayStrip *strip);

#endif /* FWM_TRAY_H */
