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

#ifndef FWM_WPGEN_H
#define FWM_WPGEN_H

#include <cairo.h>
#include <stdint.h>

#include "config.h"

/* The wallpaper fwm draws for itself when the config names none.
 *
 * A compositor whose first frame is black looks broken rather than empty, and
 * shipping a photograph to avoid that would put the only megabyte in the
 * repository somewhere install.sh does not currently reach. So this draws one:
 * a layered horizon — sky, then ridges receding into haze — sized to whatever
 * screen asks and handed to wallpaper.c as an ordinary image.
 *
 * Everything about it comes from one 64-bit seed, drawn once per fwm process
 * and never written down: a landscape lasts for a session and the next login
 * is somewhere else. That is also why the seed lives here as module state
 * rather than in FwmConfig or FwmServer — "one per process" is exactly what a
 * lazily-drawn static is, and both the layer builder and theme.c need to reach
 * it without either of them owning it.
 *
 * Drawn on the CPU. The star's shader path was the obvious place to start —
 * star_shader.h already has the fbm this wants — but a wallpaper is rendered
 * once and then never again, which is the half of the bargain the GPU is not
 * needed for. Staying on the CPU means no second GL program, no render target
 * to lose to a GPU reset, and no readback: the generated layer is a
 * cairo_surface_t, which is what wallpaper.c already knows how to mount, card
 * and make static. The only thing cairo could not do for free is a gradient
 * without banding, and that is a dither loop (see wpgen_layer).
 */

/* Up to four ranges; a seed uses two, three or four of them.
 *
 * The count is part of what a seed decides, and it is the loudest part: two is
 * a stark graphic horizon, four is depth and mist. Four is the ceiling because
 * every layer is another screen-and-a-bit of pixels to draw and to keep. */
#define WPGEN_MAX_LAYERS 4

/* One seed's landscape, in units no screen has been mentioned in yet:
 * horizons and amplitudes are fractions of the screen HEIGHT, frequencies and
 * slack are per screen WIDTH. That is what makes the same seed the same
 * picture on a monitor of a different size — see wpgen_layer, which maps a
 * pixel to world u by dividing by the screen width and nothing else. Hotplug
 * rebuilds the wallpaper (server_output.c), and this is why it rebuilds the
 * same one.
 *
 * Almost all of it varies. The first version of this varied only the palette
 * and it showed: eight colourways of one picture, with the ranges always in
 * the same places at the same heights. So the count, where the horizon sits,
 * how mountainous the land is, how fast it washes out, what the sky is doing
 * and whether there is a sun or water in it are all the seed's to choose. */
typedef struct {
    uint64_t seed;
    int      count;

    /* Per layer, back to front. */
    double slack[WPGEN_MAX_LAYERS];    /* buffer overhang, in screen widths */
    double horizon[WPGEN_MAX_LAYERS];  /* ridge line, 0 = top of screen, 1 = bottom */
    double amp[WPGEN_MAX_LAYERS];      /* how far peaks rise above it, in screen heights */
    double freq[WPGEN_MAX_LAYERS];     /* ridges per screen width */
    double phase[WPGEN_MAX_LAYERS];    /* where along the range this seed starts */
    double rough[WPGEN_MAX_LAYERS];    /* 0 rolling hills .. 1 jagged peaks */

    /* Which layer is water rather than land, or -1. A flat plane with the sky
     * broken up on it reads as a different place entirely, which is most of
     * why it is here. */
    int    water;

    /* The palette, in HSV because that is the shape theme.c wants it back in
     * and the shape hue jitter is natural in. */
    double sky_top[3], sky_hor[3], ridge[3], haze[3], glow[3];
    /* How much haze the FARTHEST range is buried in: low is a valley full of
     * mist, high is a crisp cut-out silhouette. */
    double haze_depth;

    /* The light. Where it stands and whether it is night come from [sun], so
     * the wallpaper agrees with the shadows the windows cast; how big and how
     * soft it is, and whether the disc itself is above the horizon, are the
     * seed's. */
    double sun_x;      /* 0..1 across the screen */
    double sun_power;  /* 0 = no glow at all */
    double glow_w;     /* how far the glow reaches across, in screen widths */
    double glow_h;     /* and up, in screen heights */
    /* fwm's badge, standing in the sky where the sun would be: half its width
     * in screen heights, and where its centre sits down the frame. Zero means
     * this seed has no body in its sky — and then the badge is drawn small and
     * faint in the top corner instead, because the mark is always somewhere. */
    double disc;
    double disc_y;
    double night;      /* 0 day .. 1 night; drains the sky */
    double stars;      /* 0..1; only ever above zero when it is actually night */
    double cloud;      /* 0..1 how much texture is in the sky */
    double cloud_w;    /* how far cloud is stretched flat, and how tall it piles */
    double cloud_h;
} WpgenWorld;

/* This process's seed, drawn on first use. */
uint64_t wpgen_seed(void);

/* Draw a new one — the `wallpaper_reroll` action. Everything built from the
 * old seed is stale afterwards and the caller has to rebuild it. */
void wpgen_reseed(void);

/* The landscape this process's seed describes, for `cfg`'s sun. Cheap: no
 * pixels are touched, so theme.c may ask for it without drawing anything. */
void wpgen_world(const FwmConfig *cfg, WpgenWorld *out);

/* The fraction of a screen's own size the picture is actually drawn at, 0 < k
 * <= 1, and 1 on anything up to about 1080p.
 *
 * Cost here is per pixel and a wallpaper is three screens-and-a-bit of them,
 * so a 4K monitor was paying half a second at start and on every reroll — the
 * one place a generated wallpaper could be worse to live with than a decoded
 * one. Above a budget the picture is drawn smaller and scaled up on the way
 * into the layer, which costs a soft edge on a silhouette and nothing else:
 * there is no detail in a gradient to lose, and the ridge lines are the only
 * hard edges in the whole image. Never below half size, so the softening has
 * a floor too.
 *
 * The caller must draw the surface at 1/k to fill the layer — see
 * build_generated in wallpaper.c. */
double wpgen_render_scale(int screen_w, int screen_h);

/* The buffer width layer `i` wants on a screen `screen_w` wide. Always at
 * least screen_w; the excess is the pan travel wallpaper_update spends. */
int wpgen_layer_width(const WpgenWorld *w, int i, int screen_w);

/* Draw layer `i` at `buf_w` x `buf_h`, for a screen `screen_w` wide.
 * Premultiplied ARGB32, ready for cairo_overlay_update to paint 1:1. Layers
 * above the first are transparent over their sky. NULL if it cannot be had. */
cairo_surface_t *wpgen_layer(const WpgenWorld *w, int i,
                             int buf_w, int buf_h, int screen_w);

/* What theme.c would have sampled, had there been a file to sample.
 *
 * The generator chose these colours; recovering them from its own pixels with
 * gdk-pixbuf would be a slower way to learn a number it already knows. `cast`
 * is the hue and saturation the islands are tinted toward, `accent` the light
 * — returned raw, in HSV, because normalising it into a band that reads
 * against a dark island is theme.c's policy and not this file's. */
void wpgen_palette(const WpgenWorld *w, double cast[2], double accent[3]);

#endif /* FWM_WPGEN_H */
