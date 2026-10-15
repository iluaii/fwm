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

#include <stdbool.h>
#include <stddef.h>
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
 * Expires as a PLACE, that is. The command a launch ran is kept for as long as
 * the process it started is alive, because that is what the session writes
 * down for a window to come back by (launched_command), and a window opened
 * half an hour later still came from that command.
 *
 * Nothing here is asked of a client, and nothing needs the client's help:
 * xdg-activation is the protocol for a launcher to say where a window belongs,
 * and neither Steam nor most of what one starts from a keybind speaks it.
 */

struct FwmServer;
struct FwmView;

#define LAUNCHED_TTL 120.0   /* seconds a launch places the windows it opens */
#define LAUNCHED_CMD_MAX 512 /* longest command remembered with a launch */

/* Remember that `pid` was started while `desktop` was the one in front of you,
 * by running `cmd` through sh -c (NULL when there is none worth keeping).
 * pid <= 0 is ignored, so a caller that failed to fork needs no branch. */
void launched_note(struct FwmServer *server, pid_t pid, int desktop, const char *cmd);

/* The desktop this window's application was launched from, or -1 when it was
 * not launched by us, when the launch has expired, or when the window's process
 * cannot be read. Does not consume the entry. */
int launched_desktop(struct FwmServer *server, struct FwmView *view);

/* The command this window's application was started with, for the session to
 * relaunch instead of the window's own process.
 *
 * What owns a window is very often not something that can be started: Steam's
 * windows belong to `./steamwebhelper` inside its container and to the client
 * binary that only runs under steam.sh, and relaunching either after a crash
 * does nothing. The command a person typed is what brings the application
 * back. Only when the launched process does not own a window of its own,
 * though, unless it is this one: a terminal is a launch too, and firefox
 * started from inside it must not be written down as the terminal.
 *
 * Copies the command into `out` and the launched pid into `*pid`, and returns
 * true; false when there is no such launch, or its process has ended. */
bool launched_command(struct FwmServer *server, struct FwmView *view,
                      pid_t *pid, char *out, size_t cap);

/* The session wrote the launch `pid` down on `desktop`, by one of its windows.
 * From then on it stays written down while its process runs, window or not:
 * see launched_running. `desktop` -1: by a window that is not on screen, which
 * keeps the desktop it was last written on, or else the one it was launched
 * from. */
void launched_written(struct FwmServer *server, pid_t pid, int desktop);

struct LaunchedApp {
    pid_t pid;
    int desktop;
    const char *cmd;   /* valid until the next call into this file */
};

/* Every launch the session has written down whose process is still running,
 * up to `cap` of them; returns how many.
 *
 * An application closed to the tray has no window, and a session made of
 * windows had nothing to write for it: Steam and Discord both sit there most
 * of the time, so a crash then left them out, and they did not come back. Its
 * process running is what says it is still open. Only a launch that has had a
 * window written down, though — what fwm started to run in the background and
 * never had a window is not an application in the session. */
int launched_running(struct FwmServer *server, struct LaunchedApp *out, int cap);

/* Drop the table. */
void launched_finish(struct FwmServer *server);

#endif /* FWM_LAUNCHED_H */
