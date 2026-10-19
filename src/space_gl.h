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

#ifndef FWM_SPACE_GL_H
#define FWM_SPACE_GL_H

#include <stdbool.h>

#include "space.h"

struct wlr_renderer;
struct wlr_buffer;

/* Drawing the solar system on the GPU, through the door star_gl.c uses: the
 * compositor's own GLES2 context, an FBO on a buffer we allocated, one quad.
 *
 * There is no fallback. The star has a cairo one because a star is a small
 * picture cairo can approximate; a planet with an atmosphere, a lensing hole
 * and a sky of stars is not, and a bad imitation of a wallpaper someone chose
 * is worse than the wallpaper under it, which is what stays on screen when
 * this cannot run (Vulkan and pixman renderers). */

bool space_gl_supported(struct wlr_renderer *renderer);

/* What a draw came to. PENDING is the shader still being built in the
 * background — nothing drawn, nothing wrong; ask again next frame. */
typedef enum {
    SPACE_GL_FAILED  = -1,   /* cannot here, or the shader would not build (logged) */
    SPACE_GL_PENDING =  0,
    SPACE_GL_DRAWN   =  1,
} SpaceGlResult;

/* Draw one frame into `dst`. */
SpaceGlResult space_gl_render(struct wlr_renderer *renderer, struct wlr_buffer *dst,
                              const SpaceUniforms *u);

/* Forget the program: its context is gone (renderer swap after a GPU reset,
 * or shutdown). */
void space_gl_finish(void);

#endif /* FWM_SPACE_GL_H */
