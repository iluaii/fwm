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

#ifndef FWM_MIXER_H
#define FWM_MIXER_H

#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>

struct FwmServer;
typedef struct Mixer Mixer;

Mixer *mixer_create(struct FwmServer *server);
void mixer_destroy(Mixer *mixer);

void mixer_toggle(Mixer *mixer);
bool mixer_is_open(Mixer *mixer);

/* Feed a pressed key while the panel is up. Turning the knob walks the list,
 * pressing it takes hold of the row it stopped on — and while a row is held,
 * turning moves ITS volume instead of the selection. Pressing again lets go.
 * Everything else is swallowed, as the launcher and the ring do.
 *
 * Returns true when the panel ACTED on the key, not whether it was consumed —
 * consumption is not in doubt, the panel owns the keyboard while it is up and
 * its caller swallows the key either way. The answer is for the second layout:
 * `m` is the one letter here, and with a Cyrillic layout active it arrives as
 * Cyrillic_softsign, so the caller retries the key as the first layout spells
 * it, exactly as it already does for the binds and the desktop strip. */
bool mixer_handle_key(Mixer *mixer, xkb_keysym_t sym);

/* Advance the row animations, the eased bars and the poll clock, and redraw
 * when anything moved; once per tick. */
void mixer_tick(Mixer *mixer, double dt);

/* Pointer support while open: motion selects the row under the cursor, a press
 * takes hold of it (or, on the bar, drops the volume where it was clicked),
 * and a press outside the panel closes it. The wheel is the knob. All are
 * no-ops when closed; the two that return a bool return true if consumed. */
void mixer_handle_motion(Mixer *mixer, double lx, double ly);
bool mixer_handle_button(Mixer *mixer, double lx, double ly, bool pressed);
bool mixer_handle_axis(Mixer *mixer, double delta);

/* True while the panel is up or a reader is still out asking the mixer what is
 * playing, so the compositor keeps ticking fast enough to answer it. */
bool mixer_busy(Mixer *mixer);

#endif /* FWM_MIXER_H */
