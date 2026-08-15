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

#ifndef FWM_RADIAL_H
#define FWM_RADIAL_H

#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>

struct FwmServer;
typedef struct Radial Radial;

Radial *radial_create(struct FwmServer *server);
void radial_destroy(Radial *radial);

void radial_toggle(Radial *radial);
bool radial_is_open(Radial *radial);

/* Feed a pressed key while the menu is open. Turning the knob moves the focus,
 * pressing it fires; the arrows and the digits do the same for a keyboard that
 * has no knob. Everything else is swallowed — the menu owns the keyboard for
 * the same reason the launcher does. Returns true if the key was consumed,
 * which while open is always. */
bool radial_handle_key(Radial *radial, xkb_keysym_t sym);

/* Advance the petal springs and redraw; call once per physics tick. */
void radial_tick(Radial *radial, double dt);

/* Pointer support while open: motion moves the focus, a press fires the petal
 * under the cursor or closes on a click outside the ring. Both are no-ops when
 * closed; handle_button returns true if consumed. */
void radial_handle_motion(Radial *radial, double lx, double ly);
bool radial_handle_button(Radial *radial, double lx, double ly, bool pressed);

#endif /* FWM_RADIAL_H */
