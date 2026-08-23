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

#ifndef FWM_TOAST_H
#define FWM_TOAST_H

#include <stdbool.h>

/* Its footprint, which is public because the region screenshot's flight aims at
 * it: the frozen copy shrinks into the line that says it was copied, and it can
 * only do that if it knows where the line will be. */
#define TOAST_W  520
#define TOAST_H  44

struct FwmServer;

/* One line at the bottom of the active monitor, held for about two seconds and
 * then lifted away. `bad` gives it the red accent — something did not happen,
 * or is about to stop happening.
 *
 * Only one is ever up: a second call replaces the first outright rather than
 * stacking, because two messages about the same thing help nobody. It is not a
 * notification system and has no queue, no history and nothing to click; the
 * compositor uses it for what only the compositor knows — where a screenshot
 * went, how much charge is left. */
void toast_show(struct FwmServer *server, bool bad, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Drop whatever is on screen, on the way out of the session. */
void toast_cleanup(void);

#endif /* FWM_TOAST_H */
