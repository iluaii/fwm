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

#ifndef FWM_OSD_H
#define FWM_OSD_H

#include <stdbool.h>

struct FwmServer;
typedef struct Osd Osd;

Osd *osd_create(struct FwmServer *server);
void osd_destroy(Osd *osd);

/* Say what a dial is worth now: `label` names it, `value` is the reading as
 * text, and `frac` is where it sits between the ends of its range (0..1, or
 * below 0 for a value with no range worth drawing). `at_end` marks the turn
 * the range answered instead of the hand.
 *
 * Showing it again while it is up simply retimes it — a hand on a knob sends
 * one of these per detent, and each must not stack a panel on the last. */
void osd_show(Osd *osd, const char *label, const char *value, double frac, bool at_end);

/* Count down the hold and fade it out when it runs out; once per tick. */
void osd_tick(Osd *osd, double dt);

/* True while the panel is up, so the tick keeps running fast enough to time
 * the hold — on the idle heartbeat it would overstay by a fifth of a second. */
bool osd_busy(Osd *osd);

#endif /* FWM_OSD_H */
