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

#ifndef FWM_MODES_H
#define FWM_MODES_H

#include <stdbool.h>
#include <cairo.h>
#include <wlr/util/box.h>
#include <wlr/types/wlr_scene.h>

/*
 * The modes pill in the tray, and the menu it opens.
 *
 * The pill is drawn by tray.c (it is part of the tray's one buffer); the menu
 * is its own overlay, anchored under the pill the way the error panel is
 * anchored under the warning pill. What lives here is everything both need to
 * agree on: the icons, the state struct, and the hit-testing.
 *
 * Icons are drawn as cairo paths rather than set as text. A glyph font is not
 * something a compositor can assume — the tray already gambles on ⚠ and ⌨ —
 * and a handful of shapes is less code than the fallback logic would be.
 */

enum {
    MODE_ICON_TILING = 0,
    MODE_ICON_FLOATING,
    MODE_ICON_GRAVITY,
    MODE_ICON_MASS,
    MODE_ICON_SOUND,
    MODE_ICON_CAVA,
    MODE_ICON_GRASS,
    MODE_ICON_WIND,
    MODE_ICON_RING,
    MODE_ICON_HP,
    MODE_ICON_COUNT,
};

/* Draw `icon` into a size x size box at (x, y) using the CURRENT cairo source,
 * so the caller owns the colour and therefore the on/off/dimmed distinction. */
void modes_icon(cairo_t *cr, int icon, double x, double y, double size);

/* ── shared menu chrome ───────────────────────────────────────────────────
 * The panel outline and the switch, exported because the stats menu (ui/stats_
 * menu.c) is the same object with different rows. Two menus that hang off two
 * neighbouring pills must be the same menu to look at; copying the geometry
 * into a second file is how that stops being true after the first change to
 * either. */
#define MODES_MENU_CHAMFER  10.0   /* 45-degree corner cut; the tray is pointed */
#define MODES_SWITCH_W      36.0
#define MODES_SWITCH_H      18.0

/* Chamfered panel path. Left as a path, not filled: callers pick the source. */
void modes_panel_path(cairo_t *cr, double x, double y, double w, double h, double c);

/* `pos` is an ANIMATED 0..1 knob position rather than a boolean — the caller
 * has already eased it, and the colours crossfade along the same number. */
void modes_switch(cairo_t *cr, double x, double y, double pos, double alpha);

/* Everything the pill and the menu render. Filled by the server each frame from
 * the live compositor state, never cached here — a mode changed by a keybind
 * must show up in the pill without anyone telling it. */
typedef struct ModesState {
    int tiling;    /* active desktop is DESKTOP_MODE_TILING */
    int floating;  /* active desktop is DESKTOP_MODE_FLOATING */
    int gravity;   /* physics.gravity_scale > 0 */
    int mass;      /* PHYSICS_MASS_*: what decides how heavy a window is */
    int sound;     /* sound.collisions: windows knock when they hit something */
    int cava;      /* CAVA_MODE_* */
    int grass;     /* grass.enabled: a strip of grass along the bottom */
    int wind;      /* grass.wind > 0: the gusts that bend it */
    int ring;      /* camera.wrap: the desktops are a ring */
    int hp;        /* physics.hp: a hard enough hit destroys a window */
    double opacity;
} ModesState;

/* Fixed pill width. Fixed on purpose: the pill sits between the desktop island
 * and the clock, both of which move with the screen width and the date string,
 * and a pill that grew with the number of active modes would start overlapping
 * one of them at exactly the moment it had the most to say. */
#define MODES_PILL_W 126

/* Open and close are the same animation in opposite directions, so they read
 * the same two numbers. Split them and they drift. */
#define MODES_MENU_ANIM_MS 170.0
#define MODES_MENU_RISE_PX  14.0

/* Rows of the menu, in draw order. Mass and Sound sit under Gravity because all
 * three are the same subject — what the simulation does to a window, what the
 * window weighs while it does it, and what that sounds like. */
enum {
    MODES_ROW_NONE = -1,
    MODES_ROW_TILING = 0,
    MODES_ROW_FLOATING,
    MODES_ROW_GRAVITY,
    MODES_ROW_MASS,
    MODES_ROW_SOUND,
    MODES_ROW_CAVA,
    /* Next to cava because it is the same kind of thing: something drawn along
     * the bottom of the screen that the compositor keeps animating. */
    MODES_ROW_GRASS,
    /* Under grass because it is the grass's own knob: switching it on switches
     * the grass on with it, since wind through a lawn nobody is drawing is a
     * thing the user cannot see. */
    MODES_ROW_WIND,
    MODES_ROW_RING,
    /* Last on purpose: it is the only row that can destroy someone's unsaved
     * work, so it is not the one the hand lands on by accident. */
    MODES_ROW_HP,
    MODES_ROW_COUNT,
};

/* Cava's three positions. The config has a fourth (`"physical"`, bars that push
 * without being drawn), deliberately left out of the menu: it is a thing to
 * discover in the file, not to land on by clicking past it. */
enum {
    MODES_CAVA_OFF = 0,
    MODES_CAVA_VISUAL,
    MODES_CAVA_PHYSICAL,
    MODES_CAVA_SEGS,
};

/* Mass's two positions, in the same order as PHYSICS_MASS_* — the row is a
 * choice between two ways of being on, never an off, which is why it is a
 * segmented control and not a switch. */
enum {
    MODES_MASS_SIZE = 0,
    MODES_MASS_RAM,
    MODES_MASS_SEGS,
};

/* Segments in row `row`, or 0 for the rows that carry a switch instead. */
int modes_row_segs(int row);

/* Open the menu under the pill.
 *
 * `screen` is the MONITOR the pill is on, in layout coordinates, and `pill_x`
 * is where the pill starts in those same coordinates — both, because the menu
 * hangs off one particular strip and must be clamped to the screen that strip
 * is drawn on. Clamping to the primary monitor's size instead is what used to
 * drag a menu opened on the second screen back onto the first. */
struct wlr_scene_buffer *modes_menu_show(struct wlr_scene_tree *parent,
                                         const struct wlr_box *screen,
                                         double pill_x, const ModesState *st);

/* Redraw an open menu after a toggle, without the flicker of a destroy+show.
 * The switches do not jump to the new state — they ease toward it on the tick
 * below, and this only records where they are heading. */
void modes_menu_redraw(struct wlr_scene_buffer *buf, const ModesState *st);

/* Advance the menu's own animations (switch knobs sliding, the cava highlight
 * travelling, the rows staggering in) and redraw if anything moved. Call once
 * per frame while the menu is open. Separate from cairo_overlay_tick, which
 * animates the NODE — position and opacity — and cannot reach inside the
 * buffer to move a knob.
 *
 * Returns true while anything is still in motion, so the compositor keeps
 * driving frames instead of dropping to the idle heartbeat mid-slide. */
bool modes_menu_tick(struct wlr_scene_buffer *buf, const ModesState *st, double dt);

/* True while the menu has animation left to run, for server_is_busy. */
bool modes_menu_animating(void);

/* Hit-test the menu, in MENU-BUFFER-LOCAL coordinates. Returns a MODES_ROW_*,
 * or MODES_ROW_NONE outside any row. For a row with a segmented control (see
 * modes_row_segs) *seg receives the segment under the point, or -1 if the point
 * is on the row but not on the control. *seg is untouched for switch rows. */
int modes_menu_hit(double x, double y, int *seg);

/* Size of the menu, so the caller can tell whether a click landed inside it at
 * all before converting coordinates. */
void modes_menu_size(int *w, int *h);

#endif /* FWM_MODES_H */
