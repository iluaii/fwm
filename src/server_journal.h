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

#ifndef FWM_SERVER_JOURNAL_H
#define FWM_SERVER_JOURNAL_H

#include <stdint.h>

struct FwmServer;
struct FwmView;

/* The compositor's half of the session's history: what it tells journal.c,
 * and when.
 *
 * Opening, closing, a new title, the focus and a throw are told as they
 * happen, from the places that already tell `fwmctl subscribe` about them.
 * Where a window is, is not: a window is moved by a hand, a throw, the tiler
 * making room, a fullscreen request, a script — too many places to say it in
 * each. So the windows are looked at instead, every JOURNAL_LOOK_MS, and
 * whatever has moved is written down whoever moved it: points along its way
 * while it travels, and where it ends up once it has stopped. */

/* Read the history this boot has so far and carry it on. `socket` is the
 * Wayland display's name: a nested compositor keeps a history of its own
 * rather than writing into the session's. */
void server_journal_start(struct FwmServer *server, const char *socket);
void server_journal_finish(struct FwmServer *server);

void server_journal_map(struct FwmServer *server, struct FwmView *view);
void server_journal_unmap(struct FwmServer *server, struct FwmView *view);
void server_journal_title(struct FwmServer *server, struct FwmView *view);
void server_journal_focus(struct FwmServer *server, struct FwmView *view);
void server_journal_throw(struct FwmServer *server, struct FwmView *view);

/* Look at the windows. Called from the physics tick, as often as it runs; it
 * keeps its own pace. */
void server_journal_look(struct FwmServer *server);

/* The wall clock in ms, which is what the history is written in. */
int64_t server_journal_now(void);

#endif /* FWM_SERVER_JOURNAL_H */
