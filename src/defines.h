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

#ifndef FWM_DEFINES_H
#define FWM_DEFINES_H

/* Concurrent windows with a physics body. Slots are recycled when a window
 * closes, so this is a ceiling on what is on screen at once, not on what has
 * been opened over the session. Costs ~256 * (sizeof(PhysicsBody) +
 * sizeof(BodySlot)) of static memory, which is tens of kilobytes. */
#define MAX_WINDOWS             256
#define DRAG_MARGIN             5
#define PHYSICS_MARGIN          3
#define MASS_DENSITY            0.0005
#define FRICTION                0.985
#define THROW_SPEED_MULTIPLIER  0.65
#define MAX_THROW_SPEED         1800.0
#define STOP_SPEED_THRESHOLD    1.0
#define RESTITUTION             0.3
#define PHYSICS_TICK_RATE       60.0
/* Approach speed (px/s) a collision must reach before it counts as an impact
 * worth reacting to. Above resting jitter, below a short drop. */
#define PHYSICS_HIT_MIN_SPEED   120.0
#define GRAVITY  981.0   // px/s² (earth ~9.8 m/s² at 100 px/m)

#endif /* FWM_DEFINES_H */