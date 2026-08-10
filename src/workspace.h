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

#ifndef FWM_WORKSPACE_H
#define FWM_WORKSPACE_H

struct FwmServer;
struct FwmOutput;

/* ext-workspace-v1: publishes the ten desktops so an external bar can draw a
 * desktop strip and switch with a click. fwm's own strip does not use this — it
 * reads the outputs directly; this exists purely for outside clients.
 *
 * The mapping to fwm's world, which is one strip of ten columns rather than a
 * per-monitor set:
 *
 *   workspace  — one of the ten desktops. All ten exist for as long as the
 *                compositor does: they cannot be created or removed, so
 *                neither capability is offered.
 *   group      — one MONITOR. A desktop is shown by at most one monitor at a
 *                time, so a desktop joins the group of whichever monitor is
 *                showing it and leaves it again when that monitor moves on.
 *                A desktop nobody is showing belongs to no group, which is
 *                exactly what the protocol's optional group means.
 *
 * So a bar filtered to its own screen sees the one desktop that screen is
 * showing, and a bar that wants the whole strip walks all ten. */
void workspace_init(struct FwmServer *server);

/* A monitor arriving and leaving. The group is the monitor, so its lifetime is
 * the monitor's. */
void workspace_output_added(struct FwmServer *server, struct FwmOutput *out);
void workspace_output_gone(struct FwmServer *server, struct FwmOutput *out);

/* Republish which monitor is showing which desktop. Cheap and called every
 * tick: what it reports changes when a window is thrown across the seam and
 * drags the camera with it, and there is no one event for that. Nothing goes
 * on the wire unless an answer actually changed. */
void workspace_sync(struct FwmServer *server);

#endif /* FWM_WORKSPACE_H */
