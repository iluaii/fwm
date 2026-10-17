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

#ifndef FWM_SIDEBAR_H
#define FWM_SIDEBAR_H

#include <stdbool.h>
#include <wlr/types/wlr_scene.h>
#include "config.h"

/*
 * The column of text down the right edge of each monitor ([sidebar]): the
 * machine's readouts, a command's output, or plain words, drawn straight onto
 * the wallpaper with no panel behind them and the windows above.
 *
 * Two halves, because the text is one and the screens are many. The FEED is
 * the server's single source: it runs the commands and samples the built-in
 * sensors — through the same engine the tray's stats pill uses (stats.c), so
 * there is one way fwm forks a command and reads it, not two — and composes
 * the lines. A PANEL is one monitor's picture of that text, in that monitor's
 * palette, repainted only when the text, the palette or the look changed.
 */

/* ── the feed ────────────────────────────────────────────────────────── */

typedef struct FwmSidebarFeed FwmSidebarFeed;

FwmSidebarFeed *sidebar_feed_create(const SidebarConfig *cfg);
/* Reload: readouts that survive keep their last value, so the column does not
 * blank for an interval. */
void sidebar_feed_reconfigure(FwmSidebarFeed *f, const SidebarConfig *cfg);
void sidebar_feed_destroy(FwmSidebarFeed *f);

/* Sample whatever is due. Returns true when the text changed. */
bool sidebar_feed_tick(FwmSidebarFeed *f, double dt);

/* The whole column as Pango markup, lines joined by '\n': each configured
 * line as written with its readouts escaped, or escaped whole when its own
 * markup does not parse. Always valid markup; never NULL. */
const char *sidebar_feed_markup(const FwmSidebarFeed *f);

/* ── a monitor's panel ───────────────────────────────────────────────── */

typedef struct FwmSidebar FwmSidebar;

/* A column for a monitor `screen_w` x `screen_h`. `parent` sits above the
 * wallpaper and below the windows. NULL if it cannot be allocated. */
FwmSidebar *sidebar_create(struct wlr_scene_tree *parent, const SidebarConfig *cfg,
                           int screen_w, int screen_h);

/* Put the column on its monitor: `x`,`y` are the monitor's top-left corner in
 * layout coordinates; the column finds its own right edge. */
void sidebar_set_origin(FwmSidebar *sb, int x, int y);

/* The monitor or the width changed: the caller tears this one down and builds
 * a new one. */
bool sidebar_stale(const FwmSidebar *sb, const SidebarConfig *cfg,
                   int screen_w, int screen_h);

/* Paint `feed`'s text in `output`'s palette, unless what is on screen already
 * is exactly that. Cheap to call every tick. */
void sidebar_draw(FwmSidebar *sb, const SidebarConfig *cfg,
                  const FwmSidebarFeed *feed, const char *output);

void sidebar_destroy(FwmSidebar *sb);

#endif /* FWM_SIDEBAR_H */
