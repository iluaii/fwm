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

#ifndef FWM_WELCOME_H
#define FWM_WELCOME_H

#include <wlr/types/wlr_scene.h>
#include "../config.h"

struct wlr_scene_buffer *welcome_show(struct wlr_scene_tree *parent, int screen_w, int screen_h,
                                      const FwmConfig *cfg);
void welcome_set_welcomed(void);

#endif /* FWM_WELCOME_H */
