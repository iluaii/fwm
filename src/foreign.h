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

#ifndef FWM_FOREIGN_H
#define FWM_FOREIGN_H

#include <stdbool.h>

struct FwmServer;
struct FwmView;

/* wlr-foreign-toplevel-management-v1: publishes the window list so external
 * panels can show a taskbar (waybar's wlr/taskbar) and window switchers can
 * offer more than the compositor's own binds.
 *
 * fwm's own tray does not use this — it reads the view list directly. This
 * exists purely for outside clients. */
void foreign_init(struct FwmServer *server);

/* Mirror one view's lifetime into a handle. map creates it, unmap destroys it,
 * and the rest push state the panel displays. All are no-ops when the manager
 * failed to create. */
void foreign_view_map(struct FwmView *view);
void foreign_view_unmap(struct FwmView *view);
void foreign_view_title_changed(struct FwmView *view);
void foreign_view_set_activated(struct FwmView *view, bool activated);
void foreign_view_set_fullscreen(struct FwmView *view, bool fullscreen);

#endif /* FWM_FOREIGN_H */
