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

#ifndef FWM_BOX2D_COMPAT_H
#define FWM_BOX2D_COMPAT_H

/* Box2D 3.2 renamed a few calls; the two spellings mean the same thing.
 * FWM_BOX2D_3_2 comes from CMakeLists.txt. */

#include <box2d/box2d.h>
#include <stdbool.h>

#ifdef FWM_BOX2D_3_2
#define B2_CONTACT_SPEED contactSpeed
#define b2_set_max_speed b2World_SetMaxLinearSpeed
static inline void b2_lock_rotation(b2BodyDef *bd, bool lock) {
    bd->motionLocks.angularZ = lock;
}
static inline void b2_set_rotation_locked(b2BodyId body, bool lock) {
    b2MotionLocks locks = b2Body_GetMotionLocks(body);
    locks.angularZ = lock;
    b2Body_SetMotionLocks(body, locks);
}
#else
#define B2_CONTACT_SPEED maxContactPushSpeed
#define b2_set_max_speed b2World_SetMaximumLinearSpeed
static inline void b2_lock_rotation(b2BodyDef *bd, bool lock) {
    bd->fixedRotation = lock;
}
static inline void b2_set_rotation_locked(b2BodyId body, bool lock) {
    b2Body_SetFixedRotation(body, lock);
}
#endif

#endif
