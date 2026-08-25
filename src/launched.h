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

#ifndef FWM_LAUNCHED_H
#define FWM_LAUNCHED_H

#include <sys/types.h>

/*
 * Which desktop an application was STARTED from.
 *
 * A window is put on the desktop that is on screen when it maps. For anything
 * that appears at once that is the same thing as the desktop you asked from,
 * which is why it went unnoticed. For anything slow it is not: start Steam on
 * desktop 2, go and read something on desktop 1 while it loads, and its window
 * arrives on 1 — the desktop you happened to drift to, not the one you asked
 * on. The longer an application takes to start, the more likely you are to have
 * gone somewhere else, so the rule is worst exactly where it matters most.
 *
 * So the desktop is written down at the moment of the launch, against the
 * process that was started, and a window whose process descends from that one
 * opens where it was asked for. Descends, not equals: `steam` is a shell script
 * that execs a launcher that starts the client, and every interesting
 * application is at least one fork away from the command that named it.
 *
 * An entry is kept for LAUNCHED_TTL seconds and may be claimed more than once
 * inside that window — an application that shows a splash screen and then its
 * real window would otherwise send the first to the right desktop and the
 * second to whatever the camera had drifted to, which is the bug again with an
 * extra step. After that it expires, so a window opened by hand from an
 * application half an hour later follows the camera like anything else.
 *
 * Nothing here is asked of a client, and nothing needs the client's help:
 * xdg-activation is the protocol for a launcher to say where a window belongs,
 * and neither Steam nor most of what one starts from a keybind speaks it.
 */

struct FwmServer;
struct FwmView;

#define LAUNCHED_TTL 120.0   /* seconds an unclaimed launch stays interesting */

/* Remember that `pid` was started while `desktop` was the one in front of you.
 * pid <= 0 is ignored, so a caller that failed to fork needs no branch. */
void launched_note(struct FwmServer *server, pid_t pid, int desktop);

/* The desktop this window's application was launched from, or -1 when it was
 * not launched by us, when the launch has expired, or when the window's process
 * cannot be read. Does not consume the entry. */
int launched_desktop(struct FwmServer *server, struct FwmView *view);

/* Drop the table. */
void launched_finish(struct FwmServer *server);

#endif /* FWM_LAUNCHED_H */
