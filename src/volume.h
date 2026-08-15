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

#ifndef FWM_VOLUME_H
#define FWM_VOLUME_H

#include <stdbool.h>

struct FwmServer;
typedef struct Volume Volume;

Volume *volume_create(struct FwmServer *server);
void volume_destroy(Volume *v);

/* The `volume:` action's argument: "+5" / "-5" step, "50" goes straight there,
 * "mute" toggles. Moves the system volume through the commands in [volume] and
 * puts the reading on screen. */
void volume_action(Volume *v, const char *arg);

/* Collect whatever the reader has said; once per tick. */
void volume_tick(Volume *v, double dt);

/* True while a read is in flight, so the tick keeps running until the answer
 * is in and the panel can show it. */
bool volume_busy(Volume *v);

#endif /* FWM_VOLUME_H */
