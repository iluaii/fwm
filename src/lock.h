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

#ifndef FWM_LOCK_H
#define FWM_LOCK_H

#include <stdbool.h>

struct FwmServer;

/* ext-session-lock-v1 (swaylock and friends).
 *
 * While a lock is engaged the compositor must show ONLY the lock client's
 * surfaces and must not let anything else reach the user. Two consequences the
 * rest of the code has to respect:
 *
 *  - `lock_is_active()` gates keybind matching in handle_keyboard_key and all
 *    pointer routing. A bind that still fired while locked (spawn:kitty, EXIT,
 *    killclient) would defeat the lock outright.
 *  - If the lock client dies without unlocking, the session STAYS locked
 *    showing a blank screen. Unlocking on client death would turn a crashing
 *    screen locker into an unlock. This is required by the protocol. */
void lock_init(struct FwmServer *server);

/* True while the session is locked, including after the lock client died. */
bool lock_is_active(struct FwmServer *server);

#endif /* FWM_LOCK_H */
