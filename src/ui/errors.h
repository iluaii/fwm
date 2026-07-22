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

#ifndef FWM_ERRORS_H
#define FWM_ERRORS_H

#include <wlr/types/wlr_scene.h>
#include "../config.h"

/* Detail panel for config problems, opened from the tray's warning pill.
 * Anchored under the tray rather than centred: it annotates the pill. */
struct wlr_scene_buffer *errors_show(struct wlr_scene_tree *parent, int screen_w, int screen_h,
                                     const FwmConfig *cfg);

#endif /* FWM_ERRORS_H */
