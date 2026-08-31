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

#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "video.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Sampling width for palette extraction. gdk-pixbuf scales on load, so a 4K
 * wallpaper costs a decode and a few thousand pixels, not megapixels. */
#define SAMPLE_W  200
#define HUE_BINS  36

/* A video wallpaper has no single frame to sample: a clip that opens on a dark
 * sky and ends in daylight would derive a different theme depending on which
 * still we grabbed. Take a few frames spread through the clip and pour them all
 * into one histogram, so the theme follows the piece rather than a moment. */
#define VIDEO_SAMPLE_FRAMES 3

/* The built-in dark scheme — also the base the wallpaper tint moves away
 * from, so "wallpaper" mode stays recognisably fwm. */
static const FwmTheme theme_base = {
    .pill   = {0.075, 0.082, 0.098},
    .sel    = {0.145, 0.155, 0.195},
    .text   = {0.91, 0.92, 0.94},
    .muted  = {0.54, 0.57, 0.63},
    .dim    = {0.32, 0.34, 0.40},
    .accent = {0.478, 0.635, 0.969}, /* #7aa2f7, the default focus blue */
};

static FwmTheme theme_current = {
    .pill   = {0.075, 0.082, 0.098},
    .sel    = {0.145, 0.155, 0.195},
    .text   = {0.91, 0.92, 0.94},
    .muted  = {0.54, 0.57, 0.63},
    .dim    = {0.32, 0.34, 0.40},
    .accent = {0.478, 0.635, 0.969},
    .border_active   = {0.478, 0.635, 0.969, 1.0},
    .border_inactive = {0.231, 0.259, 0.380, 1.0},
};

static unsigned theme_gen = 1;

const FwmTheme *theme_get(void) {
    return &theme_current;
}

unsigned theme_generation(void) {
    return theme_gen;
}

/* ── colour helpers ──────────────────────────────────────────────────── */

static void rgb_to_hsv(const double rgb[3], double *h, double *s, double *v) {
    double mx = fmax(rgb[0], fmax(rgb[1], rgb[2]));
    double mn = fmin(rgb[0], fmin(rgb[1], rgb[2]));
    double d = mx - mn;
    *v = mx;
    *s = mx <= 0.0 ? 0.0 : d / mx;
    if (d <= 0.0) { *h = 0.0; return; }
    if (mx == rgb[0])      *h = 60.0 * fmod((rgb[1] - rgb[2]) / d, 6.0);
    else if (mx == rgb[1]) *h = 60.0 * ((rgb[2] - rgb[0]) / d + 2.0);
    else                   *h = 60.0 * ((rgb[0] - rgb[1]) / d + 4.0);
    if (*h < 0.0) *h += 360.0;
}

static void hsv_to_rgb(double h, double s, double v, double out[3]) {
    double c = v * s;
    double x = c * (1.0 - fabs(fmod(h / 60.0, 2.0) - 1.0));
    double m = v - c;
    double r = 0, g = 0, b = 0;
    if      (h <  60) { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else              { r = c; b = x; }
    out[0] = r + m; out[1] = g + m; out[2] = b + m;
}

static void lerp3(const double a[3], const double b[3], double t, double out[3]) {
    for (int i = 0; i < 3; i++) out[i] = a[i] + (b[i] - a[i]) * t;
}

/* wlr_scene_rect wants premultiplied alpha, same as parse_hex_color emits. */
static void to_premul_rgba(const double rgb[3], float out[4]) {
    out[3] = 1.0f;
    for (int i = 0; i < 3; i++) out[i] = (float)rgb[i];
}

/* ── wallpaper sampling ──────────────────────────────────────────────── */

struct Sampled {
    double mean_h, mean_s;  /* overall cast, for the tint */
    double accent[3];
    int    have_accent;
};

/* Weighted circular mean of hues, accumulated as unit vectors: averaging hue
 * degrees directly turns red (350°) and red (10°) into cyan. */
struct HueBin {
    double weight;
    double sx, sy;   /* sum of weight * (cos h, sin h) */
    double sum_s, sum_v;
};

/* Histogram in progress: one image, or several frames of a video, poured in. */
struct Accum {
    struct HueBin bins[HUE_BINS];
    double cast_x, cast_y, cast_s, cast_w;
    long   cast_n;
};

/* One image's worth of pixels, given as byte offsets within a pixel so both
 * gdk-pixbuf's RGB(A) and cairo's native-endian BGRA feed the same code.
 * `ao` is the alpha offset, or -1 when the buffer is opaque. */
static void accum_pixels(struct Accum *a, const unsigned char *pix,
                         int w, int h, int nch, int stride,
                         int ro, int go, int bo, int ao) {
    for (int y = 0; y < h; y++) {
        const unsigned char *s = pix + (size_t)y * stride;
        for (int x = 0; x < w; x++, s += nch) {
            if (ao >= 0 && s[ao] < 128) continue;
            double rgb[3] = { s[ro] / 255.0, s[go] / 255.0, s[bo] / 255.0 };
            double hh, ss, vv;
            rgb_to_hsv(rgb, &hh, &ss, &vv);

            /* Overall cast, which drives the tint: weighted by saturation only,
             * NOT by brightness. The tint should follow whatever fills the
             * image — a large dark sky — and weighting by value let a thin
             * bright band outvote it, tinting the islands the accent's colour
             * and leaving the accent to the background. */
            if (ss > 0.08 && vv > 0.06) {
                double rad = hh * M_PI / 180.0;
                double wgt = ss;
                a->cast_x += cos(rad) * wgt;
                a->cast_y += sin(rad) * wgt;
                a->cast_s += ss * wgt;
                a->cast_w += wgt;
                a->cast_n++;
            }

            /* Accent candidates: skip greys, near-blacks and blown highlights —
             * none of them survive as a usable accent on a dark island. */
            if (ss < 0.25 || vv < 0.15 || (vv > 0.97 && ss < 0.20)) continue;
            int bin = (int)(hh / (360.0 / HUE_BINS));
            if (bin < 0) bin = 0;
            if (bin >= HUE_BINS) bin = HUE_BINS - 1;
            double wgt = ss * ss * vv; /* saturation dominates: vivid beats merely present */
            double rad = hh * M_PI / 180.0;
            a->bins[bin].weight += wgt;
            a->bins[bin].sx += cos(rad) * wgt;
            a->bins[bin].sy += sin(rad) * wgt;
            a->bins[bin].sum_s += ss * wgt;
            a->bins[bin].sum_v += vv * wgt;
        }
    }
}

/* Kept in step with is_video_layer() in src/wallpaper.c. */
static int path_is_video(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    static const char *ext[] = { "mp4", "mkv", "webm", "mov", "m4v", "avi", NULL };
    for (int i = 0; ext[i]; i++)
        if (g_ascii_strcasecmp(dot + 1, ext[i]) == 0) return 1;
    return 0;
}

/* Frames spread through a video wallpaper. Returns the number accumulated. */
static int accum_video(struct Accum *a, const char *path) {
    int got = 0;
    for (int i = 0; i < VIDEO_SAMPLE_FRAMES; i++) {
        double frac = (i + 1.0) / (VIDEO_SAMPLE_FRAMES + 1.0);
        cairo_surface_t *surf = video_still_at(path, SAMPLE_W, SAMPLE_W, frac);
        if (!surf) continue;
        cairo_surface_flush(surf);
        const unsigned char *pix = cairo_image_surface_get_data(surf);
        if (pix) {
            /* CAIRO_FORMAT_ARGB32 is a native-endian 32-bit word, so on
             * little-endian the bytes land B, G, R, A. Video frames are opaque,
             * and premultiplication by alpha 255 is the identity. */
            accum_pixels(a, pix, cairo_image_surface_get_width(surf),
                         cairo_image_surface_get_height(surf), 4,
                         cairo_image_surface_get_stride(surf), 2, 1, 0, -1);
            got++;
        }
        cairo_surface_destroy(surf);
    }
    return got;
}

static int sample_wallpaper(const char *path, struct Sampled *out) {
    struct Accum acc;
    memset(&acc, 0, sizeof(acc));

    if (path_is_video(path)) {
        if (accum_video(&acc, path) == 0) return 0;
    } else {
        GError *err = NULL;
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, SAMPLE_W, -1, TRUE, &err);
        if (!pb) {
            if (err) g_error_free(err);
            /* A video with an unfamiliar extension still decodes; better to try
             * than to tell the user their wallpaper has no colour in it. */
            if (accum_video(&acc, path) == 0) return 0;
        } else {
            int has_alpha = gdk_pixbuf_get_has_alpha(pb);
            accum_pixels(&acc, gdk_pixbuf_get_pixels(pb),
                         gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb),
                         gdk_pixbuf_get_n_channels(pb),
                         gdk_pixbuf_get_rowstride(pb),
                         0, 1, 2, has_alpha ? 3 : -1);
            g_object_unref(pb);
        }
    }

    struct HueBin *bins = acc.bins;
    double cast_x = acc.cast_x, cast_y = acc.cast_y;
    double cast_s = acc.cast_s, cast_w = acc.cast_w;
    long cast_n = acc.cast_n;

    if (cast_n == 0) return 0; /* fully greyscale image: nothing to derive */

    double cast_h = atan2(cast_y, cast_x) * 180.0 / M_PI;
    if (cast_h < 0) cast_h += 360.0;
    out->mean_h = cast_h;
    out->mean_s = cast_w > 0.0 ? cast_s / cast_w : 0.0;

    if (getenv("FWM_THEME_DEBUG")) {
        double tot = 0;
        for (int i = 0; i < HUE_BINS; i++) tot += bins[i].weight;
        fprintf(stderr, "cast hue %.0f sat %.2f; bins (hue: %% of weight):\n",
                out->mean_h, out->mean_s);
        for (int i = 0; i < HUE_BINS; i++)
            if (tot > 0 && bins[i].weight / tot > 0.005)
                fprintf(stderr, "   %3d deg: %5.2f%%  s=%.2f v=%.2f\n", i * 10,
                        100.0 * bins[i].weight / tot,
                        bins[i].sum_s / bins[i].weight, bins[i].sum_v / bins[i].weight);
    }

    double total = 0;
    for (int i = 0; i < HUE_BINS; i++) total += bins[i].weight;

    /* Prefer a hue that actually contrasts with the tint: the dominant hue is
     * already the island tint, so an accent taken from it reads as monochrome.
     * The alternative must carry real weight (>=10%) — chasing a 0.3% splash
     * of colour picks up compression artefacts and swings wildly between
     * similar wallpapers. Otherwise the dominant hue is the honest answer. */
    int best = -1;
    for (int i = 0; i < HUE_BINS; i++) {
        if (bins[i].weight < total * 0.10) continue;
        double hue = i * (360.0 / HUE_BINS) + (360.0 / HUE_BINS) / 2.0;
        double dist = fabs(hue - out->mean_h);
        if (dist > 180.0) dist = 360.0 - dist;
        if (dist <= 40.0) continue;
        if (best < 0 || bins[i].weight > bins[best].weight) best = i;
    }
    if (best < 0) {
        for (int i = 0; i < HUE_BINS; i++)
            if (best < 0 || bins[i].weight > bins[best].weight) best = i;
    }

    if (best < 0 || bins[best].weight <= 0.0) {
        out->have_accent = 0;
        return 1;
    }

    double bh = atan2(bins[best].sy, bins[best].sx) * 180.0 / M_PI;
    if (bh < 0) bh += 360.0;
    double bs = bins[best].sum_s / bins[best].weight;
    double bv = bins[best].sum_v / bins[best].weight;

    /* Normalise into a band that always reads clearly against a dark island:
     * a washed-out or near-black source colour would vanish otherwise. */
    if (bs < 0.55) bs = 0.55;
    if (bs > 0.92) bs = 0.92;
    if (bv < 0.78) bv = 0.78;
    if (bv > 1.00) bv = 1.00;

    hsv_to_rgb(bh, bs, bv, out->accent);
    out->have_accent = 1;
    return 1;
}

/* ── build ───────────────────────────────────────────────────────────── */

void theme_build(FwmConfig *cfg) {
    FwmTheme t = theme_base;

    /* Configured border colours are the baseline in both modes. */
    memcpy(t.border_active, cfg->decor.col_active, sizeof(t.border_active));
    memcpy(t.border_inactive, cfg->decor.col_inactive, sizeof(t.border_inactive));

    if (cfg->decor.color_source != COLOR_SOURCE_WALLPAPER) {
        theme_current = t;
        theme_gen++;
        return;
    }

    if (cfg->wallpaper_count <= 0) {
        config_report_error(cfg, "[decor] color_source = \"wallpaper\" but no "
                                 "[[wallpaper]] is configured — keeping config colours");
        theme_current = t;
        theme_gen++;
        return;
    }

    /* With two monitors showing two images the palette can only come from one
     * of them: the screen the user is on (see FwmConfig.palette_output), which
     * is the one the picker was last used on. */
    const WallpaperLayer *pal = config_wallpaper_first(cfg, cfg->palette_output);
    if (!pal) {
        theme_current = t;
        theme_gen++;
        return;
    }

    struct Sampled s = {0};
    if (!sample_wallpaper(pal->path, &s)) {
        config_report_error(cfg, "[decor] color_source = \"wallpaper\": \"%s\" has no "
                                 "colour to derive from (unreadable or greyscale) — "
                                 "keeping config colours",
                            pal->path);
        theme_current = t;
        theme_gen++;
        return;
    }

    /* Tint: pull the near-black islands toward the wallpaper's cast. Value is
     * pinned low and saturation capped, so the fill stays dark whatever the
     * image does — this is what keeps the text readable without recomputing it. */
    double strength = cfg->decor.tint_strength;
    double tint_s = fmin(s.mean_s * 1.2, 0.45);

    double tint_pill[3], tint_sel[3];
    hsv_to_rgb(s.mean_h, tint_s, 0.13, tint_pill);
    hsv_to_rgb(s.mean_h, tint_s, 0.22, tint_sel);
    lerp3(theme_base.pill, tint_pill, strength, t.pill);
    lerp3(theme_base.sel,  tint_sel,  strength, t.sel);

    /* Secondary text picks up a little of the hue; primary text stays neutral
     * so titles keep maximum contrast. */
    double tint_muted[3];
    hsv_to_rgb(s.mean_h, tint_s * 0.5, 0.60, tint_muted);
    lerp3(theme_base.muted, tint_muted, strength * 0.5, t.muted);

    if (s.have_accent) {
        memcpy(t.accent, s.accent, sizeof(t.accent));
        to_premul_rgba(t.accent, t.border_active);

        /* Inactive border: same hue, drained and darkened — related to the
         * accent without competing with it. */
        double ah, as, av, inactive[3];
        rgb_to_hsv(t.accent, &ah, &as, &av);
        hsv_to_rgb(ah, as * 0.45, av * 0.38, inactive);
        to_premul_rgba(inactive, t.border_inactive);
    }

    theme_current = t;
    theme_gen++;
}
