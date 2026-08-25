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

#ifndef FWM_BLUR_H
#define FWM_BLUR_H

#include <stdbool.h>

struct wlr_renderer;
struct wlr_buffer;
struct wlr_texture;

/* The frosted pane under fwm's own panels, drawn on the GPU.
 *
 * The third raw-GLES2 file, through the same door rotate.c and star_gl.c use
 * and for the same reason: wlr_render_pass_add_texture can place a picture and
 * fade it, and there is no way in the public renderer API to say "and blur it"
 * — or to cut the result to a shape. So this file takes a photograph of the
 * desktop and the panel's own pixels, and hands back the one picture that goes
 * under the panel: the desktop blurred, cut to exactly the shape the panel
 * paints, with the panel's shadow lying under it.
 *
 * It decides nothing. Where the light is, how much blur, what colour the frost
 * is tinted — all of that is settled in glass.c and arrives here as numbers,
 * the same division of labour star_gl.c keeps with star.c.
 *
 * Unavailable on the Vulkan and pixman renderers, exactly like the other two;
 * the caller drops the frost and lets the panel stand on the desktop as it
 * always did. */

/* True when blur_glass can work at all with this renderer. Cheap. */
bool blur_supported(struct wlr_renderer *renderer);

/* The mask is the panel's own alpha, and it arrives as COVERAGE times the
 * island's fill opacity, because that is how cairo left it: a pill painted at
 * 0.55 covers its middle at 0.55 and its antialiased rim at 0.55 * whatever
 * fraction of the pixel it touched. Dividing by that fill recovers the
 * coverage exactly — full inside the pill, a clean ramp along the rim, nothing
 * outside it — which is why `fill` has to be told rather than guessed. */
/* Every length in here is a REACH: how far from the shape the blur is still
 * doing anything, not the sigma of the gaussian that gets there. That is what
 * shadow.c means by its penumbra, what the manual promises for [glass] radius,
 * and — the part that matters — exactly how much room glass.c leaves around a
 * panel for the blur to spread into. A gaussian handed a reach as its sigma
 * runs three times past the end of that room and is cut off there with a
 * fiftieth of its darkness still on, which on screen is a soft rectangle
 * standing around the panel. blur.c divides; nobody else needs to know. */
typedef struct {
    double radius;        /* how far the blur reaches on the desktop, px */
    double fill;          /* the alpha the panel's islands are painted at */
    double tint;          /* 0..1, how far the frost is pulled toward tint_color */
    float  tint_color[3]; /* straight RGB */
    double brightness;    /* multiplier on the blurred desktop; 1 leaves it alone */

    /* The shadow. Direction and darkness come from the same sun the windows
     * cast from — a panel is one more object lying on the desktop, and a panel
     * lit from somewhere else would be the one thing on screen disagreeing
     * about where the light is. */
    double shadow_radius; /* px of penumbra; 0 draws no shadow at all */
    double shadow_dx;     /* px the shadow is offset by */
    double shadow_dy;
    double shadow_alpha;  /* 0..1 */
    float  shadow_color[3];

    /* The part of `backdrop` that holds real pixels, as (x0, y0, x1, y1) in
     * its own 0..1 coordinates. The photograph is the size of `dst` and `dst`
     * is bigger than the panel, so a panel near the edge of a screen is
     * photographed with a band of nothing along it; the blur is kept out of
     * that band rather than allowed to average it in. {0,0,1,1} for a panel
     * with room on every side. */
    float keep[4];

    /* Where the panel sits inside `dst`, in px. The destination is bigger than
     * the panel by whatever room the shadow needs on each side, so this says
     * which part of it the mask covers. */
    int panel_x, panel_y, panel_w, panel_h;
} BlurParams;

/* A scratch buffer, from both sides.
 *
 * Each of the three is rendered into and then sampled, so it needs an FBO and
 * a texture of the same memory. The texture is the CALLER's and is imported
 * once for the life of the buffer: a texture of a buffer we own is a view of
 * it rather than a copy, and building one per frame is an EGL image made and
 * thrown away sixty times a second for a picture that has not moved — the
 * lesson star_draw.c learned about its own photograph. */
typedef struct {
    struct wlr_buffer *buf;
    struct wlr_texture *tex;
} BlurScratch;

/* Draw the pane into `dst`, which is cleared first.
 *
 * `backdrop` is a photograph of the desktop the size of `dst`, `mask` the
 * panel's own pixels (panel_w x panel_h). The three scratch buffers are the
 * caller's, must all be the same size, and want to be a fraction of `dst` —
 * blur is done small and stretched back up, which is most of why this is cheap
 * enough to run every frame. glass.c sizes them; see blur_scratch_scale.
 *
 * False if anything could not be drawn, in which case `dst` is not to be
 * trusted and the caller should hide the pane rather than show it. */
bool blur_glass(struct wlr_renderer *renderer, struct wlr_buffer *dst,
                struct wlr_texture *backdrop, struct wlr_texture *mask,
                const BlurScratch *a, const BlurScratch *b,
                const BlurScratch *c, const BlurParams *params);

/* How far down the scratch buffers should be scaled for a blur of `radius` px
 * (and a shadow of `shadow_radius`) over a `w` x `h` destination, as a
 * fraction of it.
 *
 * Shrinking the picture is the cheap way to a wide blur — a blur reaching 48px
 * is a sigma-3 blur of a picture at a fifth — but only down to a point. Below it there
 * is not enough of the shape left to blur, and what comes back up is a grid of
 * soft squares; the tray, 88px tall, was landing in nine rows. So this also
 * refuses to shrink past what keeps the shape, and blur_glass makes up the
 * missing reach by running its pass more than once instead. */
double blur_scratch_scale(double radius, double shadow_radius, int w, int h);

/* Release the shader programs. Call once at teardown, before the renderer is
 * destroyed. */
void blur_shutdown(struct wlr_renderer *renderer);

#endif /* FWM_BLUR_H */
