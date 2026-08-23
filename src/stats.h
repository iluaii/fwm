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

#ifndef FWM_STATS_H
#define FWM_STATS_H

#include <stdbool.h>
#include "config.h"

/*
 * The readouts in the tray's stats pill: what the machine is doing, sampled on
 * a timer and rendered as one short line.
 *
 * Five sensors are built in — cpu, ram, gpu, bat, net — and everything else is
 * a shell command the user names in [stats]. That split is the whole design: a
 * compositor has no business knowing how to ask PulseAudio for the volume, and
 * a user who wants it in the tray should not have to write a plugin to get it.
 *
 * NOTHING HERE MAY BLOCK. The built-in sensors are small reads out of /proc and
 * /sys, which are memory, not disk. A custom sensor is a child
 * process, and a child process can take as long as it likes — so it is started
 * on one tick and collected on a later one, through a pipe that is never read
 * except when it has something to say. A command that never exits is killed
 * rather than waited for (STATS_CMD_TIMEOUT_S).
 */

typedef struct FwmStats FwmStats;

/* How a value comes to be. Built-ins are read in-process; a custom item is
 * whatever its command printed on its first line. */
enum {
    STATS_SRC_CPU = 0,
    STATS_SRC_RAM,
    STATS_SRC_GPU,
    STATS_SRC_BAT,
    STATS_SRC_NET,
    STATS_SRC_CUSTOM,
};

typedef struct {
    char name[STATS_NAME_MAX];   /* the key in [stats]; also the menu's label */
    char value[32];              /* rendered text ("12%", "7.4G"), "" if unknown */
    int  source;                 /* STATS_SRC_* */
    int  enabled;                /* drawn in the pill; toggled from the menu */
    int  available;              /* the machine can answer this at all */
} StatsItem;

/* Build the item list from the config. Safe to call again on reload: items that
 * survive keep their enabled flag and their last value, so a reload does not
 * blank the pill for an interval. */
FwmStats *stats_create(const StatsConfig *cfg);
void stats_reconfigure(FwmStats *s, const StatsConfig *cfg);
void stats_destroy(FwmStats *s);

/* Sample whatever is due. Cheap on the ticks where nothing is (a clock compare
 * per item), so it belongs in the frame loop rather than on a timer of its own.
 * Returns true when a value changed, i.e. when the tray is worth redrawing. */
bool stats_tick(FwmStats *s, double dt);

int stats_count(const FwmStats *s);
const StatsItem *stats_item(const FwmStats *s, int i);
void stats_set_enabled(FwmStats *s, int i, int on);

/* The pill's whole text: enabled items with values, joined by a bullet. Writes
 * "" when nothing is enabled or nothing has answered yet — the caller draws a
 * placeholder rather than an empty island, since a pill nobody can see is also
 * a pill nobody can click to get their readouts back. */
void stats_format(const FwmStats *s, char *out, size_t out_size);

#endif /* FWM_STATS_H */
