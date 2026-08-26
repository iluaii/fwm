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

#include "shadow.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>

#ifndef DRM_FORMAT_ARGB8888
#define DRM_FORMAT_ARGB8888 875713089
#endif

/* Widest penumbra we will build an image for. The image is (4r+1) square, so
 * this is a 257x257 buffer — a quarter of a megabyte for the entire desktop,
 * however many windows are on it. */
#define SHADOW_MAX_BLUR 64

/* ── the shared image ─────────────────────────────────────────────────────
 *
 * A blurred rectangle whose flat middle is exactly one pixel wide, so the
 * nine-patch has something to stretch. The rectangle occupies [r, 3r+1] of a
 * (4r+1) wide image: r of margin for the penumbra to fall into on each side,
 * and r of rectangle on each side of the middle pixel, which is enough for the
 * blur to have reached full strength by the time the stretched part begins. */

struct ShadowBuffer {
    struct wlr_buffer base;
    uint32_t *pixels;
};

static void shadow_buffer_destroy(struct wlr_buffer *wlr_buffer) {
    struct ShadowBuffer *buf = wl_container_of(wlr_buffer, buf, base);
    wlr_buffer_finish(wlr_buffer);
    free(buf->pixels);
    free(buf);
}

static bool shadow_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
                                                uint32_t flags, void **data,
                                                uint32_t *format, size_t *stride) {
    struct ShadowBuffer *buf = wl_container_of(wlr_buffer, buf, base);
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;
    *format = DRM_FORMAT_ARGB8888;
    *data   = buf->pixels;
    *stride = (size_t)wlr_buffer->width * 4;
    return true;
}

static void shadow_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
    (void)wlr_buffer;
}

static const struct wlr_buffer_impl shadow_buffer_impl = {
    .destroy = shadow_buffer_destroy,
    .begin_data_ptr_access = shadow_buffer_begin_data_ptr_access,
    .end_data_ptr_access = shadow_buffer_end_data_ptr_access,
};

/* The image every window's shadow is cut out of, and the parameters it was
 * built for. `retired` is the previous one: a rebuild happens in the pass that
 * then re-points every window at the new image, so the old one has to outlive
 * the call by exactly one generation rather than be freed under a node that
 * has not been updated yet. */
static struct wlr_buffer *g_atlas;
static struct wlr_buffer *g_atlas_retired;
static int   g_atlas_r = -1;
static float g_atlas_color[3];

/* Coverage of the rectangle [lo, hi] at `x`, blurred by a gaussian of sigma.
 * The exact answer rather than a sampled kernel — it is one erf pair per pixel
 * of one row, and the row is then multiplied out into the square. */
static double blur_profile(double x, double lo, double hi, double sigma) {
    if (sigma <= 0.0) return (x >= lo && x <= hi) ? 1.0 : 0.0;
    double s = sigma * sqrt(2.0);
    return 0.5 * (erf((x - lo) / s) - erf((x - hi) / s));
}

static struct wlr_buffer *atlas_build(int r, const float color[3]) {
    int size = 4 * r + 1;
    struct ShadowBuffer *buf = calloc(1, sizeof(*buf));
    if (!buf) return NULL;
    buf->pixels = calloc((size_t)size * size, 4);
    if (!buf->pixels) { free(buf); return NULL; }
    wlr_buffer_init(&buf->base, &shadow_buffer_impl, size, size);

    double *row = calloc((size_t)size, sizeof(double));
    if (!row) { shadow_buffer_destroy(&buf->base); return NULL; }

    /* 3 sigma reaches the edge of the margin, which is where the eye stops
     * being able to tell the penumbra from nothing. */
    double sigma = r / 3.0;
    for (int i = 0; i < size; i++)
        row[i] = blur_profile(i + 0.5, r, 3.0 * r + 1.0, sigma);

    /* ARGB8888 little-endian, premultiplied — what the scene expects and what
     * lets the whole thing be blended with one opacity per node. */
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double a = row[x] * row[y];
            if (a < 0.0) a = 0.0;
            if (a > 1.0) a = 1.0;
            uint32_t A = (uint32_t)lround(a * 255.0);
            uint32_t R = (uint32_t)lround(color[0] * a * 255.0);
            uint32_t G = (uint32_t)lround(color[1] * a * 255.0);
            uint32_t B = (uint32_t)lround(color[2] * a * 255.0);
            buf->pixels[(size_t)y * size + x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }
    free(row);
    return &buf->base;
}

/* The image for this blur and colour, building it if the settings moved. */
static struct wlr_buffer *atlas_get(int r, const float color[3]) {
    if (g_atlas && g_atlas_r == r &&
        g_atlas_color[0] == color[0] &&
        g_atlas_color[1] == color[1] &&
        g_atlas_color[2] == color[2]) return g_atlas;

    struct wlr_buffer *next = atlas_build(r, color);
    if (!next) return g_atlas; /* carry on with the old one rather than lose the shadows */

    if (g_atlas_retired) wlr_buffer_drop(g_atlas_retired);
    g_atlas_retired = g_atlas;
    g_atlas = next;
    g_atlas_r = r;
    memcpy(g_atlas_color, color, sizeof(g_atlas_color));
    return g_atlas;
}

void shadow_atlas_finish(void) {
    if (g_atlas_retired) wlr_buffer_drop(g_atlas_retired);
    if (g_atlas)         wlr_buffer_drop(g_atlas);
    g_atlas = g_atlas_retired = NULL;
    g_atlas_r = -1;
}

/* ── the nine nodes ──────────────────────────────────────────────────── */

/* Three slices per axis, cut again at both edges of the window: five bands at
 * most, so twenty-five cells. The extra cuts are what lets the part of the
 * shadow the window is standing on be left out — a cell is then either wholly
 * under the window or wholly outside it, never half of each. */
#define SHADOW_BANDS 5
#define SHADOW_CELLS (SHADOW_BANDS * SHADOW_BANDS)

struct FwmShadow {
    struct wlr_scene_buffer *cell[SHADOW_CELLS];
    bool live[SHADOW_CELLS];  /* has geometry: what set_enabled(true) may turn on */
};

/* A shadow reaches outside the window that casts it, and its cells are scene
 * buffers, which take the hit test over their whole rectangle whatever their
 * alpha. The cells sit at the bottom of their OWN window's tree, so they never
 * came between that window and the hand — but the tree as a whole stands above
 * the windows underneath, so the overhang took input from THEM: a band around
 * every raised window where the neighbour below could not be clicked, and one
 * that walked across the desktop over the day as the sun moved.
 *
 * Landing on a cell was not simply lost, which is what made it hard to name:
 * view_at walks up to the owning tree, so the click reached the CASTING window
 * instead — it focused and could be grabbed, from a place its neighbour was
 * drawn. Scenery declines input; the same answer glass.c and star_draw.c give,
 * and the window itself goes on answering for its own area. */
static bool shadow_declines_input(struct wlr_scene_buffer *buffer,
                                  double *sx, double *sy) {
    (void)buffer; (void)sx; (void)sy;
    return false;
}

FwmShadow *shadow_create(struct wlr_scene_tree *parent) {
    FwmShadow *sh = calloc(1, sizeof(*sh));
    if (!sh) return NULL;
    for (int i = 0; i < SHADOW_CELLS; i++) {
        sh->cell[i] = wlr_scene_buffer_create(parent, NULL);
        if (!sh->cell[i]) { shadow_destroy(sh); return NULL; }
        sh->cell[i]->point_accepts_input = shadow_declines_input;
        wlr_scene_node_set_enabled(&sh->cell[i]->node, false);
        /* Under the window and under its borders, wherever they were added. */
        wlr_scene_node_lower_to_bottom(&sh->cell[i]->node);
    }
    return sh;
}

void shadow_destroy(FwmShadow *sh) {
    if (!sh) return;
    for (int i = 0; i < SHADOW_CELLS; i++)
        if (sh->cell[i]) wlr_scene_node_destroy(&sh->cell[i]->node);
    free(sh);
}

void shadow_set_enabled(FwmShadow *sh, bool enabled) {
    if (!sh) return;
    for (int i = 0; i < SHADOW_CELLS; i++)
        wlr_scene_node_set_enabled(&sh->cell[i]->node, enabled && sh->live[i]);
}

bool shadow_owns_buffer(const FwmShadow *sh, struct wlr_scene_buffer *buf) {
    if (!sh || !buf) return false;
    for (int i = 0; i < SHADOW_CELLS; i++)
        if (sh->cell[i] == buf) return true;
    return false;
}

bool shadow_owns_node(const FwmShadow *sh, struct wlr_scene_node *node) {
    if (!sh || !node) return false;
    for (int i = 0; i < SHADOW_CELLS; i++)
        if (&sh->cell[i]->node == node) return true;
    return false;
}

/* One band of the shadow along one axis: where it lands and where in the image
 * it is sampled from. */
typedef struct {
    int    dst, len;    /* px, in window-local coordinates */
    double src, src_len;/* px in the image */
    bool   over_window; /* the window is standing on this band */
} ShadowBand;

/* Cut one axis into bands.
 *
 * The three nine-patch slices first: two corners sampled 1:1, and between them
 * the single flat pixel of the image stretched over whatever is left. `e` is
 * how far into the window a corner may reach — normally the full penumbra r,
 * less on a window narrower than 2r, where the corners meet and the stretched
 * middle disappears. Sampling stops at the same place either way, so a small
 * window's shadow is a truncated version of a large one's, not a squashed one.
 *
 * Then each slice is cut again wherever an edge of the window falls inside it,
 * because a band may not be half-covered: the caller decides per band whether
 * the window is standing on it. Returns how many bands were produced. */
static int axis_bands(int s, int r, int offset, ShadowBand out[SHADOW_BANDS]) {
    int e = r < s / 2 ? r : s / 2;
    struct { int dst, len; double src, src_len; } slice[3] = {
        { offset - r, r + e,     0.0,             r + e },
        { offset + e, s - 2 * e, 2.0 * r,         1.0   },
        { offset + s - e, r + e, 3.0 * r + 1 - e, r + e },
    };

    int n = 0;
    for (int i = 0; i < 3; i++) {
        if (slice[i].len <= 0) continue;
        /* The window's own edges, as cuts inside this slice. */
        int cut[2] = {0, s};
        int lo = slice[i].dst, hi = slice[i].dst + slice[i].len;
        int at[4]; int k = 0;
        at[k++] = lo;
        for (int c = 0; c < 2; c++)
            if (cut[c] > lo && cut[c] < hi) at[k++] = cut[c];
        at[k++] = hi;
        /* The two cuts arrive in order (0 < s), so `at` is already sorted. */
        for (int j = 0; j + 1 < k; j++) {
            if (n >= SHADOW_BANDS) return n; /* cannot happen: 3 slices, 2 cuts */
            double scale = slice[i].src_len / slice[i].len;
            out[n].dst     = at[j];
            out[n].len     = at[j + 1] - at[j];
            out[n].src     = slice[i].src + (at[j] - lo) * scale;
            out[n].src_len = out[n].len * scale;
            int mid = at[j] + out[n].len / 2;
            out[n].over_window = (mid >= 0 && mid < s);
            n++;
        }
    }
    return n;
}

void shadow_update(FwmShadow *sh, int w, int h,
                   const SunConfig *cfg, const FwmSunLight *light) {
    if (!sh) return;
    if (!cfg || !cfg->enabled || light->alpha <= 0.0 || w <= 0 || h <= 0) {
        memset(sh->live, 0, sizeof(sh->live));
        shadow_set_enabled(sh, false);
        return;
    }

    int r = (int)lround(cfg->blur);
    if (r < 0) r = 0;
    if (r > SHADOW_MAX_BLUR) r = SHADOW_MAX_BLUR;

    /* Back to straight RGB: the config stores colours premultiplied (what
     * wlr_scene_rect wants), and the alpha there is not part of how dark the
     * shadow is — [sun] opacity is. */
    float rgb[3] = { 0.0f, 0.0f, 0.0f };
    if (cfg->color[3] > 0.0f)
        for (int c = 0; c < 3; c++) rgb[c] = cfg->color[c] / cfg->color[3];

    struct wlr_buffer *atlas = atlas_get(r, rgb);
    if (!atlas) {
        memset(sh->live, 0, sizeof(sh->live));
        shadow_set_enabled(sh, false);
        return;
    }

    ShadowBand cols[SHADOW_BANDS], rows[SHADOW_BANDS];
    int nc = axis_bands(w, r, (int)lround(light->dx), cols);
    int nr = axis_bands(h, r, (int)lround(light->dy), rows);

    int i = 0;
    for (int y = 0; y < nr; y++) {
        for (int x = 0; x < nc; x++) {
            if (!cfg->under_window && cols[x].over_window && rows[y].over_window)
                continue;
            struct wlr_scene_buffer *node = sh->cell[i];
            /* Unconditionally: which cell index a band lands on moves as the
             * sun does, so a cell that was skipped last time may still be
             * holding no buffer at all. Re-setting the same one is a no-op in
             * the scene, so this costs nothing when nothing changed. */
            wlr_scene_buffer_set_buffer(node, atlas);
            struct wlr_fbox src = { cols[x].src, rows[y].src,
                                    cols[x].src_len, rows[y].src_len };
            wlr_scene_buffer_set_source_box(node, &src);
            wlr_scene_buffer_set_dest_size(node, cols[x].len, rows[y].len);
            wlr_scene_buffer_set_opacity(node, (float)light->alpha);
            wlr_scene_node_set_position(&node->node, cols[x].dst, rows[y].dst);
            wlr_scene_node_set_enabled(&node->node, true);
            sh->live[i] = true;
            i++;
        }
    }
    for (; i < SHADOW_CELLS; i++) {
        sh->live[i] = false;
        wlr_scene_node_set_enabled(&sh->cell[i]->node, false);
    }
}
