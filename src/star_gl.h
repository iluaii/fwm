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

#ifndef FWM_STAR_GL_H
#define FWM_STAR_GL_H

#include <stdbool.h>

struct wlr_renderer;
struct wlr_buffer;

/* Drawing the star on the GPU, through the same door rotate.c uses.
 *
 * cairo drew a competent illustration of a star and could never have drawn
 * more than that: it is a vector rasteriser, with gradients and fills and no
 * noise, no values above 1.0 and no cheap blur. Everything that makes a star
 * look photographed is per-pixel and per-frame — turbulence with structure at
 * every scale, light that blooms because it is brighter than the display can
 * show, a corona combed into filaments. A fragment shader does all of it for
 * about the price of one gradient.
 *
 * It is also, incidentally, the cheap path: nothing crosses the bus, no buffer
 * is allocated per frame, and the compositor's own renderer does the work.
 *
 * Unavailable on the Vulkan and pixman renderers, exactly like rotate.c —
 * callers fall back to the cairo star, which is why that one is still here. */

/* True when star_gl_render can work with this renderer. Cheap. */
bool star_gl_supported(struct wlr_renderer *renderer);

/* What the shader needs to know. All of it comes from star.c: this file
 * decides nothing about the star, it only draws what it is told. */
typedef struct {
    double time_s;    /* seconds; drives the boil, the loops and the corona */
    double radius_px; /* the star's radius inside the buffer */
    float  color[3];  /* its surface colour */
    double lum;       /* 1 = an ordinary main-sequence day */
    int    phase;     /* 0 burning, 1 collapsing, 2 pulsar */
    double beam_deg;  /* pulsar beam bearing, deg clockwise from up */
    double angle;     /* how far round its own axis it has turned, radians */
    /* How open the accretion disc is, 0 edge-on .. 1 face-on. Follows the
     * camera when there is one to follow. */
    double disc_tilt;
    /* Which way round the disc's plane lies, radians. Separate from the tilt:
     * one is how squashed it looks, the other is which way it is turned. */
    double disc_roll;
    double blast;     /* supernova shell, 0..1; see star_blast */
    double birth;     /* ignition, 0..1; see star_ignition */
    double beam_aim;  /* 0..1, how head-on the pulsar's beam is */
    /* The desktop behind it, photographed this frame, for a hole to bend.
     * NULL means there is none and the shader falls back to its own sky. */
    struct wlr_texture *background;
} StarGlParams;

/* Draw into `dst`, which is cleared first. The result is premultiplied, ready
 * for a wlr_scene_buffer. False if the renderer cannot do it or the shader
 * would not build — the caller must have a fallback. */
bool star_gl_render(struct wlr_renderer *renderer, struct wlr_buffer *dst,
                    const StarGlParams *p);

/* Drop the compiled program. Called when the renderer goes away. */
void star_gl_finish(void);

#endif /* FWM_STAR_GL_H */
