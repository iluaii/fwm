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

#ifndef FWM_BATTERY_H
#define FWM_BATTERY_H

#include "config.h"

/*
 * The machine's own battery: the number the tray shows, and the moment it is
 * worth interrupting somebody over.
 *
 * The reading lives here rather than in stats.c because two things want it and
 * only one of them is a readout. A charge in the tray is a number you have to
 * be looking at to see; a laptop that dies mid-sentence was never going to be
 * saved by a number in a corner. Same file, so there is one answer to "how
 * much is left" and one place that knows a wireless mouse is not the machine.
 */

typedef struct {
    int present;   /* 0 on a desktop, and on a laptop whose battery is gone */
    int percent;   /* 0..100 */
    int charging;  /* filling right now — "Full" is not charging */
} BatteryReading;

/* Read it now. Returns 0 when there is nothing to read, leaving `out` zeroed.
 * Two small reads out of /sys, which is memory: safe on the frame loop. */
int battery_read(BatteryReading *out);

/* What the watcher below decided the user should be told. */
typedef enum {
    BATTERY_ALERT_NONE = 0,
    BATTERY_ALERT_LOW,
    BATTERY_ALERT_CRITICAL,
} BatteryAlert;

/* The watcher's memory. Held by the caller and zero-initialised: there is one
 * battery, and a module-wide static would be a second place for the session to
 * keep state that the session already keeps. */
typedef struct {
    double due;         /* seconds until the next read */
    int    last_pct;
    int    said_low;    /* already spoken for this discharge */
    int    said_crit;
} BatteryWatch;

/* Sample when due and answer once per crossing. Charging clears everything, so
 * a plug in and out arms the warnings again; climbing back above a threshold
 * (with a margin, since a charge wobbles by a percent while it sits there)
 * does the same. `pct_out` is filled whenever a reading was taken. */
BatteryAlert battery_watch(BatteryWatch *w, const BatteryConfig *cfg,
                           double dt, int *pct_out);

#endif /* FWM_BATTERY_H */
