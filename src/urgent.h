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

#ifndef FWM_URGENT_H
#define FWM_URGENT_H

#include <stdbool.h>

struct FwmServer;

/* A desktop asking to be looked at: its number goes red in the tray.
 *
 * The one thing a compositor knows that no notification daemon does — WHERE
 * the thing that wants you is. So this is the whole of fwm's part in
 * notifications, and it deliberately stops here: the popup, the history and
 * the D-Bus service belong to dunst or mako, which already do them, and
 * `fwmctl urgent <n>` is how they say which desktop to light up.
 *
 * Three things raise it, and all three mean the same sentence — "something
 * happened somewhere you are not looking":
 *
 *   - an xdg-activation request the focus policy would not honour, because the
 *     window is on a desktop nobody is showing (server_seat.c). Under
 *     `on_activate = "always"` the camera goes there instead and nothing is
 *     raised: you were taken to it, so there is nothing left to point at.
 *   - the X11 urgency hint, which is what Telegram, Thunderbird and every
 *     other XWayland client with an inbox still sets (view.c).
 *   - `fwmctl urgent <n>`, for everything else.
 *
 * The flag is per DESKTOP, not per window. A window that closes, or is thrown
 * to another desktop, leaves the flag where it was: something did happen
 * there, and the digit says so until you go and look. That is also what makes
 * `fwmctl urgent` a first-class source rather than a simulation of one — a
 * script pointing at desktop 3 has no window to hang the flag on.
 *
 * A desktop that is ON A SCREEN is never urgent. Not "cleared when focused":
 * with two monitors, half the strip is in front of you at all times, and a red
 * digit for something you are already looking at is noise. urgent_sweep() is
 * what enforces this, from the tick, so every way of arriving somewhere — the
 * binds, the tray, a swipe, expo, a monitor being plugged in — clears the
 * digit without knowing this file exists. */

/* Raise the flag. Ignored when the desktop is on a screen, or `d` is not one
 * of the ten. Returns what the flag ended up as, which is what lets the IPC
 * tell a script "asked, but it was already in front of you". */
bool urgent_raise(struct FwmServer *server, int d);

/* Drop it by hand — `fwmctl urgent <n> off`. */
void urgent_drop(struct FwmServer *server, int d);

bool urgent_get(const struct FwmServer *server, int d);

/* Clear every desktop a monitor is showing. Called each tick; does nothing,
 * and emits nothing, unless a flag actually goes out. */
void urgent_sweep(struct FwmServer *server);

#endif /* FWM_URGENT_H */
