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

#include "star_draw.h"

#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <wlr/util/log.h>

#include "ui/cairo_overlay.h"
#include "server.h"
#include "snapshot.h"
#include "star_gl.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG (M_PI / 180.0)

/* How much wider than the star the canvas is, to hold the glow around it. The
 * corona is the part that makes a disc read as something burning rather than a
 * circle, so it is given room — and the room is paid for once, since the
 * buffer is sized for the star at its largest and never resized. */
#define STAR_GLOW 2.9

/* How often the picture is handed to the scene graph.
 *
 * This is not the cost of drawing — cairo paints this in well under a
 * millisecond. It is the cost of DELIVERING it: cairo_overlay_update allocates
 * a fresh buffer every call and damages the whole of it, so every repaint is
 * an allocation, a texture upload and a region of the screen the compositor
 * must composite again. At 30Hz on a 540px canvas that was 33MB/s of both, and
 * it is what made the star stutter while the drawing itself was free.
 *
 * 12Hz is chosen against what actually moves: the convection cells turn over
 * in four to eleven seconds, so twelve frames a second is many times finer
 * than the thing being animated. */
#define STAR_REPAINT_HZ 12.0

/* The largest canvas the star is ever painted into.
 *
 * Its size on screen and the RESOLUTION it is painted at are different things,
 * and tying them together is how a big star came to cost a 6000x6000 buffer —
 * thirty-six megapixels a frame, redrawn continuously, which is a GPU pinned
 * at 100% for one decoration.
 *
 * Past this the picture stops growing: a star configured wider than this is
 * drawn at this size and no larger, which is a size nobody has ever asked for
 * on purpose. It is not stretched to fit — a black hole's photon ring is one
 * pixel wide, and magnifying that turns the sharpest thing in the picture into
 * the softest. */
#define STAR_MAX_SIDE 1024

/* The surface, as a fixed set of convection cells. Real granulation is a
 * million cells that boil over in about eight minutes each; what reads as that
 * on screen is a few dozen soft patches that brighten and fade out of step, so
 * the disc is never flat and never obviously repeating. Positions are drawn
 * once and kept — a surface whose features jump every frame looks like static,
 * not like a star. */
#define STAR_GRANULES 96

/* Loops standing off the limb. Few, because the eye reads them individually:
 * a dozen is a corona, three or four is a star with something happening on it. */
#define STAR_PROMINENCES 7

typedef struct {
    double u, v;      /* position on the disc, -1..1 in units of the radius */
    double r;         /* size, in the same units */
    double phase;     /* where in its own boil it started */
    double rate;      /* how fast it boils */
} StarGranule;

typedef struct {
    double bearing;   /* deg clockwise from up, where it stands on the limb */
    double span;      /* deg of limb the foot of the loop covers */
    double height;    /* how far out it reaches, in units of the radius */
    double phase;     /* where in its own rise and fall it started */
    double rate;
    double lean;      /* which way the loop leans over, -1..1 */
} StarProminence;

struct FwmStarDraw {
    struct wlr_scene_buffer *buf;
    int side;          /* canvas is side x side */
    double glow_r;     /* px from the centre to the edge of the canvas */

    StarGranule granule[STAR_GRANULES];
    StarProminence prom[STAR_PROMINENCES];

    /* The slow half of the picture — the corona and the limb-darkened disc —
     * kept as pixels between frames. Both are expensive (large areas, many
     * gradient stops) and both change on the timescale of a minute, so
     * repainting them at the frame rate was most of the cost of the star for
     * none of the motion. Copying them back is a blit. */
    cairo_surface_t *slow;
    double slow_radius;
    float  slow_color[3];
    FwmStarPhase slow_phase;
    double slow_at_s;

    /* One convection cell, drawn once and stamped wherever a cell goes. A
     * radial gradient per cell per frame is the same picture at eighty times
     * the price. */
    cairo_surface_t *cell;
    int cell_px;

    /* The radius the BUFFER was painted for, as against the radius the star
     * is. A collapse changes only the size of the picture, so the node is
     * scaled to follow it and the pixels are left alone — see the note in
     * star_draw_update. */
    double buf_radius;
    int dest_side;
    double world_half;  /* half-width the buffer stands for, in world px */
    double shown_half;  /* and what THIS frame's picture stands for: see below */
    double last_now_s;  /* the clock the caller last handed us */

    /* The GPU path, when the renderer allows it. Two buffers, used in turn:
     * handing the scene the same buffer it is already showing is not a change
     * it can see, and allocating a new one per frame is the cost the cairo
     * path was paying. Two is enough because a frame is done with the old one
     * by the time the next is drawn. */
    struct FwmServer *server;
    struct FwmOutput *out;   /* whose wallpaper and camera this copy follows */
    struct wlr_scene_tree *behind, *front;
    bool in_front;
    /* The photograph of the desktop a hole is bending, and the texture made
     * from it. Allocated only once a hole exists — a star bends nothing. */
    struct wlr_buffer *bg_buf;
    struct wlr_texture *bg_tex;
    struct wlr_buffer *gpu[2];
    int gpu_next;
    bool use_gl;
    bool lens_scene;   /* bend the desktop behind it, rather than a starfield */
    unsigned ring_tex;    /* what expo captured for it to bend; see star_draw_set_ring */
    float ring_rect[4];
    double disc_tilt;  /* 0 edge-on .. 1 face-on; negative = no opinion */
    double disc_roll;  /* which way the disc's plane is turned, radians */
    bool headless;     /* no scene node: the caller draws the texture itself */
    struct wlr_texture *frame_tex;  /* the last frame, for a headless star */

    /* FWM_DEBUG_STAR=1 accounting. */
    int dbg;
    double dbg_at_s;
    int dbg_repaints;
    double dbg_ms, dbg_worst;

    /* What the last repaint was of, so an unchanged star costs nothing. */
    FwmStarPhase drawn_phase;
    double drawn_radius;
    double drawn_lum;
    double drawn_beam;
    double drawn_at_s;
    bool   drawn;
};

/* The state the picture is painted from, gathered once so the draw callback
 * and the staleness test cannot disagree about it. */
struct star_paint {
    FwmStarPhase phase;
    double radius;     /* px */
    double lum;
    double beam;       /* deg */
    float  color[3];
    double glow_r;
    double t;          /* seconds, for everything that boils or breathes */
    const StarGranule *granule;
    const StarProminence *prom;
};

static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

/* A star has to be seeded from something, and it has to be the SAME something
 * every time or the surface would reshuffle on every repaint. */
static uint32_t rnd(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (*state = x);
}
static double rnd01(uint32_t *state) { return (rnd(state) & 0xffffff) / (double)0xffffff; }

static void seed_surface(FwmStarDraw *d) {
    uint32_t st = 0x51ed270bu;
    for (int i = 0; i < STAR_GRANULES; i++) {
        StarGranule *g = &d->granule[i];
        /* Uniform over the disc, which means sqrt on the radius — without it
         * every cell crowds into the middle and the limb goes bare. */
        double a = rnd01(&st) * 2.0 * M_PI, rr = sqrt(rnd01(&st));
        g->u = cos(a) * rr;
        g->v = sin(a) * rr;
        g->r = 0.045 + 0.065 * rnd01(&st);
        g->phase = rnd01(&st) * 2.0 * M_PI;
        g->rate  = 0.55 + 0.9 * rnd01(&st);
    }
    for (int i = 0; i < STAR_PROMINENCES; i++) {
        StarProminence *p = &d->prom[i];
        p->bearing = rnd01(&st) * 360.0;
        p->span    = 22.0 + 26.0 * rnd01(&st);  /* deg of limb the feet cover */
        p->height  = 0.18 + 0.26 * rnd01(&st);  /* about as wide as it is tall */
        p->phase   = rnd01(&st) * 2.0 * M_PI;
        p->rate    = 0.12 + 0.22 * rnd01(&st);
        p->lean    = rnd01(&st) * 2.0 - 1.0;
    }
}

/* Limb darkening: the one thing that separates a star from a disc.
 *
 * Looking at the centre you see straight down into hot gas; looking at the
 * edge your line of sight skims through the cooler upper layers, so the rim is
 * dimmer and redder. I(mu) = 1 - u(1 - mu), with mu the cosine of the viewing
 * angle — the classical linear law, and enough of it to read. */
static double limb(double frac) {
    double mu = sqrt(fmax(0.0, 1.0 - frac * frac));
    return 1.0 - 0.62 * (1.0 - mu);
}

/* The stamp every convection cell is made from: white in the middle, falling
 * away to nothing. Drawn white so it can be tinted on the way down. */
#define STAR_CELL_PX 64
static cairo_surface_t *cell_sprite(void) {
    cairo_surface_t *sf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, STAR_CELL_PX, STAR_CELL_PX);
    if (cairo_surface_status(sf) != CAIRO_STATUS_SUCCESS) return NULL;
    cairo_t *cr = cairo_create(sf);
    double c = STAR_CELL_PX / 2.0;
    cairo_pattern_t *g = cairo_pattern_create_radial(c, c, 0.0, c, c, c);
    cairo_pattern_add_color_stop_rgba(g, 0.0, 1.0, 1.0, 1.0, 1.00);
    cairo_pattern_add_color_stop_rgba(g, 0.55, 1.0, 1.0, 1.0, 0.40);
    cairo_pattern_add_color_stop_rgba(g, 1.0, 1.0, 1.0, 1.0, 0.0);
    cairo_set_source(cr, g);
    cairo_arc(cr, c, c, c, 0.0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    cairo_destroy(cr);
    return sf;
}

/* The cells, stamped over whatever disc is already there. No clip: a cell is
 * placed by its own distance from the centre and shrunk as it nears the limb,
 * so nothing needs to be cut off — and cairo_clip on every frame cost more
 * than the cells did. */
static void paint_cells(cairo_t *cr, double cx, double cy,
                        const struct star_paint *p, cairo_surface_t *sprite,
                        double a) {
    if (!sprite) return;
    double r = p->radius;
    const float *c = p->color;

    for (int i = 0; i < STAR_GRANULES; i++) {
        const StarGranule *gr = &p->granule[i];
        double frac = sqrt(gr->u * gr->u + gr->v * gr->v);
        double vis = limb(fmin(frac, 1.0));
        double boil = 0.5 + 0.5 * sin(p->t * gr->rate + gr->phase);
        double ga = a * 0.46 * vis * (0.30 + 0.70 * boil);
        if (ga < 0.01) continue;

        /* Kept inside the limb by its own size, which is also the foreshorten:
         * a cell seen edge-on is a sliver. */
        double size = gr->r * r * (0.55 + 0.45 * vis);
        double room = (1.0 - frac) * r;
        if (size > room) size = room;
        if (size < 1.0) continue;

        double gx = cx + gr->u * r, gy = cy + gr->v * r;
        double k = (2.0 * size) / STAR_CELL_PX;

        cairo_save(cr);
        cairo_translate(cr, gx - size, gy - size);
        cairo_scale(cr, k, k);
        cairo_set_source_surface(cr, sprite, 0, 0);
        cairo_paint_with_alpha(cr, ga);
        cairo_restore(cr);

        /* A breath of the star's own colour around it, so the cells are not
         * simply white dots on a coloured disc. */
        cairo_save(cr);
        cairo_set_source_rgba(cr, c[0], c[1], c[2], ga * 0.30);
        cairo_arc(cr, gx, gy, size * 0.75, 0.0, 2.0 * M_PI);
        cairo_fill(cr);
        cairo_restore(cr);
    }
}

/* The disc: limb-darkened body, then the convection cells over the top of it,
 * clipped to the disc so nothing boils off the edge. */
static void paint_disc(cairo_t *cr, double cx, double cy,
                       const struct star_paint *p, double a) {
    double r = p->radius;
    const float *c = p->color;

    /* The limb is COOLER, not greyer. Scaling the colour towards black is what
     * a dimmer switch does; a star's edge keeps its hue and loses its
     * temperature, so it runs towards deep orange while a blue-white one runs
     * towards steel. Mixing towards a warm dark instead of towards nothing is
     * the whole difference between a star and a billiard ball. */
    float rim[3] = {
        (float)fmin(1.0, c[0] * 0.92),
        (float)(c[1] * 0.42),
        (float)(c[2] * 0.30),
    };

    cairo_pattern_t *g = cairo_pattern_create_radial(cx, cy, 0.0, cx, cy, r);
    for (int i = 0; i <= 10; i++) {
        double f = i / 10.0;
        double k = limb(f);
        /* k runs 1 at the centre to about 0.38 at the edge; use it to cross
         * from the hot core to the rim colour rather than to multiply. */
        double t = (1.0 - k) / 0.62;              /* 0 centre .. 1 limb */
        double core_w = 0.42 * (1.0 - t) * (1.0 - t); /* white only deep in */
        cairo_pattern_add_color_stop_rgba(g, f,
            fmin(1.0, c[0] * (1.0 - t) + rim[0] * t + core_w),
            fmin(1.0, c[1] * (1.0 - t) + rim[1] * t + core_w),
            fmin(1.0, c[2] * (1.0 - t) + rim[2] * t + core_w),
            a);
    }
    cairo_set_source(cr, g);
    cairo_arc(cr, cx, cy, r, 0.0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
}

/* Prominences: arcs of gas standing off the limb.
 *
 * Drawn as a thin curved band — an arc with an inside and an outside — rather
 * than a filled wedge, because that is what one is: plasma strung along a
 * magnetic loop, which has a THICKNESS and not a taper. A filled triangle off
 * the edge of a circle reads as a cartoon star, which is exactly what the
 * first attempt at this looked like.
 *
 * They rise and fall on their own slow clocks and most are down at any moment,
 * so the limb has one or two things happening on it rather than a ring of
 * decoration. */
static void paint_prominences(cairo_t *cr, double cx, double cy,
                              const struct star_paint *p, double a) {
    double r = p->radius;
    for (int i = 0; i < STAR_PROMINENCES; i++) {
        const StarProminence *pr = &p->prom[i];
        /* Sharpened so most of the cycle is spent down: plain sine would keep
         * every loop half-up all the time and ring the limb with them. */
        double raw = 0.5 + 0.5 * sin(p->t * pr->rate + pr->phase);
        double life = raw * raw * raw * raw;
        if (life < 0.10) continue;   /* one or two up at a time, not a ring */
        double h = pr->height * r * life;
        if (h < 2.5) continue;

        /* Both feet on the surface and a gap under the arch. That gap is the
         * whole difference between a prominence and a spike: plasma is strung
         * along a magnetic loop, so it goes up on one side and comes down on
         * the other, and you can see the star between the legs. */
        double foot = r * sin(pr->span * 0.5 * DEG) * 2.0; /* span of the feet */
        if (foot < 2.0) foot = 2.0;
        /* A prominence is a THREAD of plasma, not a handle: the first version
         * of this was thick enough to read as an ear stuck on the limb. */
        double thick = fmax(1.5, fmin(foot * 0.17, h * 0.38));
        double bearing = pr->bearing * DEG;

        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_rotate(cr, bearing);       /* +y is now outward along the bearing */
        cairo_translate(cr, 0.0, r * 0.98);
        cairo_scale(cr, 1.0, h / (foot * 0.5)); /* a tall loop, not a semicircle */
        /* Leaning is a shear, so the legs stay planted while the crown drifts. */
        cairo_matrix_t lean = { 1.0, 0.0, pr->lean * 0.45, 1.0, 0.0, 0.0 };
        cairo_transform(cr, &lean);

        double outer = foot * 0.5, inner = outer - thick;
        if (inner < 0.5) inner = outer * 0.55;
        cairo_new_path(cr);
        cairo_arc(cr, 0.0, 0.0, outer, 0.0, M_PI);
        cairo_line_to(cr, -inner, 0.0);
        cairo_arc_negative(cr, 0.0, 0.0, inner, M_PI, 0.0);
        cairo_close_path(cr);

        /* Hydrogen-alpha: dense and pink at the feet, thinning at the crown. */
        cairo_pattern_t *g = cairo_pattern_create_linear(0.0, 0.0, 0.0, outer);
        cairo_pattern_add_color_stop_rgba(g, 0.0, 1.00, 0.46, 0.34, a * 1.00 * life);
        cairo_pattern_add_color_stop_rgba(g, 0.6, 1.00, 0.30, 0.24, a * 0.62 * life);
        cairo_pattern_add_color_stop_rgba(g, 1.0, 0.98, 0.24, 0.22, a * 0.20 * life);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
        cairo_restore(cr);
    }
}

/* The corona: streamers, and always sized off the STAR.
 *
 * The first version filled the canvas whatever the star's size was, so a
 * pulsar nine pixels across came with a corona two hundred and seventy pixels
 * wide — a milky rectangle with a dot in it. The extent belongs to the object,
 * not to the buffer it is drawn in.
 *
 * Shape is a handful of tapering rays at uneven bearings over a faint even
 * halo. A real corona is structured by the magnetic field and looks combed;
 * one smooth blob around the disc reads as blur, which is the other thing the
 * first version looked like. */
static void paint_corona(cairo_t *cr, double cx, double cy,
                         const struct star_paint *p, double a) {
    double r = p->radius;
    double outer = fmin(p->glow_r, r * STAR_GLOW);
    const float *c = p->color;
    if (outer <= r * 1.05) return;

    cairo_pattern_t *halo = cairo_pattern_create_radial(cx, cy, r * 0.95, cx, cy, outer);
    cairo_pattern_add_color_stop_rgba(halo, 0.0,  c[0], c[1] * 0.9, c[2] * 0.75, a * 0.34);
    cairo_pattern_add_color_stop_rgba(halo, 0.28, c[0], c[1] * 0.8, c[2] * 0.6, a * 0.11);
    cairo_pattern_add_color_stop_rgba(halo, 1.0,  c[0], c[1], c[2], 0.0);
    cairo_set_source(cr, halo);
    cairo_arc(cr, cx, cy, outer, 0.0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(halo);

    /* During the collapse the plumes are skipped: the star is falling, the
     * cached layer has to be rebuilt on nearly every frame because the radius
     * is moving, and seven soft ellipses were most of that rebuild — for
     * structure nobody can resolve on something shrinking this fast. The halo
     * carries the corona for those six seconds. */
    if (p->phase == STAR_COLLAPSE) return;

    /* Plumes, not rays. Drawn as soft ellipses leaning outward: a triangle
     * with a point on it reads as the sun a child draws, and the giveaway is
     * the straight edge, not the number of them. Wide, short, and faint enough
     * that what you see is structure in the glow rather than spokes. */
    const int plumes = 7;
    for (int i = 0; i < plumes; i++) {
        double jitter = sin(i * 12.9898) * 0.7;
        double th = (i + jitter) * 2.0 * M_PI / plumes;
        double breathe = 0.55 + 0.45 * (0.5 + 0.5 * sin(p->t * 0.11 + i * 2.4));
        double len = (outer - r) * 0.72 * breathe;
        if (len < 2.0) continue;
        double wide = r * (0.55 + 0.35 * (0.5 + 0.5 * sin(i * 7.3)));

        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_rotate(cr, th);
        /* Unit circle stretched along the bearing, so the plume is an oval
         * standing on the limb with no corner anywhere on it. */
        cairo_translate(cr, r * 0.5 + len * 0.5, 0.0);
        cairo_scale(cr, (r * 0.5 + len) * 0.5, wide * 0.5);

        cairo_pattern_t *g = cairo_pattern_create_radial(0, 0, 0.0, 0, 0, 1.0);
        cairo_pattern_add_color_stop_rgba(g, 0.0, c[0], c[1], c[2], a * 0.11);
        cairo_pattern_add_color_stop_rgba(g, 0.55, c[0], c[1], c[2], a * 0.05);
        cairo_pattern_add_color_stop_rgba(g, 1.0, c[0], c[1], c[2], 0.0);
        cairo_set_source(cr, g);
        cairo_arc(cr, 0.0, 0.0, 1.0, 0.0, 2.0 * M_PI);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
        cairo_restore(cr);
    }
}

/* The limb bloom: light scattering forward past the edge. Without it the disc
 * has a hard cut where it meets the corona, and a hard-edged sun is the one
 * thing nobody has ever seen. Part of the slow layer — it is a function of the
 * radius and nothing else. */
static void paint_bloom(cairo_t *cr, double cx, double cy,
                        const struct star_paint *p, double a) {
    double r = p->radius;
    const float *c = p->color;
    /* Starts AT the limb and goes outward, in the star's own colour.
     *
     * The first version began inside the disc and was white, which painted a
     * grey hoop right around the edge — white over an orange rim, against a
     * dark wallpaper, is exactly the colour of a steel ring. Scattered light
     * belongs outside the body it scattered off, and it is the colour of the
     * light that made it. */
    cairo_pattern_t *g =
        cairo_pattern_create_radial(cx, cy, r * 0.99, cx, cy, r * 1.45);
    cairo_pattern_add_color_stop_rgba(g, 0.0,  c[0], c[1], c[2], a * 0.45);
    cairo_pattern_add_color_stop_rgba(g, 0.35, c[0], c[1] * 0.85, c[2] * 0.7, a * 0.20);
    cairo_pattern_add_color_stop_rgba(g, 1.0,  c[0], c[1] * 0.8, c[2] * 0.6, 0.0);
    cairo_set_source(cr, g);
    cairo_arc(cr, cx, cy, r * 1.45, 0.0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
}

/* How bright the star is drawn. Rides the luminosity but saturates, or the
 * flash at the end of a collapse is a white rectangle instead of a star. */
static double body_alpha(const struct star_paint *p) {
    return clamp01(0.42 + 0.58 * (p->lum / (p->lum + 1.0)) * 2.0);
}

/* The pulsar's two beams, opposite each other and swept round by star.c. Drawn
 * as soft wedges rather than lines: what is visible from the side of a
 * lighthouse is the lit air, not the lamp. */
static void paint_beams(cairo_t *cr, double cx, double cy,
                        const struct star_paint *p) {
    const double half = 7.0 * DEG;
    for (int i = 0; i < 2; i++) {
        /* Bearings are clockwise from up, cairo's angles anticlockwise from
         * the +x axis: up is -pi/2 and the sweep runs the same way round. */
        double bearing = (p->beam + i * 180.0) * DEG - M_PI / 2.0;
        /* A pulsar is tiny and its beam is not: this one reaches across the
         * canvas on purpose, unlike the corona, which belongs to the body. */
        cairo_pattern_t *g =
            cairo_pattern_create_radial(cx, cy, p->radius, cx, cy, p->glow_r);
        cairo_pattern_add_color_stop_rgba(g, 0.0, 1.0, 1.0, 1.0, 0.85);
        cairo_pattern_add_color_stop_rgba(g, 0.18,
                                          p->color[0], p->color[1], p->color[2], 0.45);
        cairo_pattern_add_color_stop_rgba(g, 0.5,
                                          p->color[0], p->color[1], p->color[2], 0.18);
        cairo_pattern_add_color_stop_rgba(g, 1.0,
                                          p->color[0], p->color[1], p->color[2], 0.0);
        cairo_set_source(cr, g);
        cairo_move_to(cr, cx, cy);
        cairo_arc(cr, cx, cy, p->glow_r, bearing - half, bearing + half);
        cairo_close_path(cr);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }
}

/* A hole: the shadow, and the photon ring around it.
 *
 * The bright rim is not decoration — it is light that came round the back and
 * arrived anyway, and it is the only part of a black hole there is to draw.
 * The shadow is drawn larger than the object for the same reason: what you see
 * is the lensed size, about two and a half times the horizon. */
static void paint_hole(cairo_t *cr, double cx, double cy,
                       const struct star_paint *p) {
    double shadow = p->radius * 2.6;

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
    cairo_arc(cr, cx, cy, shadow, 0.0, 2.0 * M_PI);
    cairo_fill(cr);

    cairo_pattern_t *g =
        cairo_pattern_create_radial(cx, cy, shadow * 0.94, cx, cy, shadow * 1.5);
    cairo_pattern_add_color_stop_rgba(g, 0.0,  1.00, 0.96, 0.85, 0.00);
    cairo_pattern_add_color_stop_rgba(g, 0.10, 1.00, 0.94, 0.78, 0.85);
    cairo_pattern_add_color_stop_rgba(g, 0.24, 1.00, 0.78, 0.45, 0.22);
    cairo_pattern_add_color_stop_rgba(g, 1.0,  1.00, 0.70, 0.35, 0.00);
    cairo_set_source(cr, g);
    cairo_arc(cr, cx, cy, shadow * 1.5, 0.0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
}

/* The slow layer, into its own surface: corona, disc, bloom. Everything here
 * is a function of the radius, the colour and a clock that turns over in about
 * a minute, so it is repainted on ITS timescale rather than the frame's. */
static void redraw_slow(FwmStarDraw *d, const struct star_paint *p) {
    if (!d->slow) {
        d->slow = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, d->side, d->side);
        if (cairo_surface_status(d->slow) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(d->slow);
            d->slow = NULL;
            return;
        }
    }
    cairo_t *cr = cairo_create(d->slow);
    double cx = d->side / 2.0, cy = d->side / 2.0;
    double a = body_alpha(p);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    paint_corona(cr, cx, cy, p, a);
    /* Behind the disc, so only the part standing off the limb is seen. Drawn
     * over it, a loop on the near side of the star read as a ring around it —
     * the sight the first attempt produced, and one nothing in the sky has.
     * (Seen against the disc a real prominence is a dark filament, not a
     * bright arch, so hiding it there is right twice over.)
     *
     * This puts them in the cached layer, which they can afford: a loop rises
     * and falls over half a minute, so half a second of staleness is nothing. */
    paint_prominences(cr, cx, cy, p, a);
    paint_disc(cr, cx, cy, p, a);
    paint_bloom(cr, cx, cy, p, a);
    cairo_destroy(cr);

    d->slow_radius = p->radius;
    d->slow_phase  = p->phase;
    memcpy(d->slow_color, p->color, sizeof(d->slow_color));
    d->slow_at_s = p->t;
}

/* Is the cached half still the picture? The corona wanders slowly enough that
 * half a second of it is invisible; the radius and colour are what actually
 * change during a collapse, and they change fast. */
static bool slow_is_stale(const FwmStarDraw *d, const struct star_paint *p) {
    if (!d->slow || d->slow_phase != p->phase) return true;
    /* Proportional, not absolute: during a collapse the radius runs from 90px
     * to 6px, and a fixed threshold would redraw the expensive layer on every
     * frame at the top of the fall — where it is also at its largest — while
     * being far too coarse at the bottom. */
    double slack = fmax(0.75, p->radius * 0.03);
    if (fabs(d->slow_radius - p->radius) > slack) return true;
    for (int i = 0; i < 3; i++)
        if (fabsf(d->slow_color[i] - p->color[i]) > 0.02f) return true;
    return p->t - d->slow_at_s > 0.5;
}

struct paint_ctx {
    FwmStarDraw *d;
    const struct star_paint *p;
};

static void star_paint_cb(cairo_t *cr, int w, int h, void *data) {
    struct paint_ctx *ctx = data;
    FwmStarDraw *d = ctx->d;
    const struct star_paint *p = ctx->p;
    double cx = w / 2.0, cy = h / 2.0;

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (p->phase == STAR_HOLE) {
        paint_hole(cr, cx, cy, p);
        return;
    }
    if (p->phase == STAR_NEUTRON) paint_beams(cr, cx, cy, p);

    if (slow_is_stale(d, p)) redraw_slow(d, p);
    if (d->slow) {
        cairo_set_source_surface(cr, d->slow, 0, 0);
        cairo_paint(cr);
    }

    /* And the half that is actually alive at frame rate: the surface boiling.
     * The cells turn over in seconds; everything else on this star takes
     * minutes, which is why everything else is a blit. */
    paint_cells(cr, cx, cy, p, d->cell, body_alpha(p));
}

static void gather(struct star_paint *p, const FwmStarDraw *d,
                   const FwmStar *star, const StarConfig *cfg, double now_s) {
    p->phase   = star->phase;
    p->radius  = star_radius(star, cfg);
    p->lum     = star_luminosity(star, cfg);
    p->beam    = star->beam;
    p->glow_r  = d->glow_r;
    p->t       = now_s;
    p->granule = d->granule;
    p->prom    = d->prom;
    star_color(star, cfg, p->color);
}

/* Wall clock in ms, for FWM_DEBUG_STAR only. */
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* A hole is drawn OVER the windows so it can bend them, which put a
 * 378-pixel-wide node between the pointer and everything under it: windows
 * could not be dragged, clicked or focused through it. Scenery has no business
 * taking input, so it declines all of it and the hit test carries on to
 * whatever is behind — which is the window you were actually pointing at. */
static bool star_declines_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
    (void)buffer; (void)sx; (void)sy;
    return false;
}

FwmStarDraw *star_draw_create(struct FwmServer *server, struct FwmOutput *out,
                              struct wlr_scene_tree *behind,
                              struct wlr_scene_tree *front, const StarConfig *cfg) {
    if (!cfg || !cfg->enabled) return NULL;
    struct wlr_scene_tree *parent = behind;

    FwmStarDraw *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->server = server;
    d->out    = out;
    d->behind = behind;
    d->front  = front ? front : behind;

    /* Sized for the star at its largest — which is the burning radius, since
     * everything after that is smaller. The hole is the one thing drawn bigger
     * than its own radius, and by then the radius is a fraction of this. */
    /* What the buffer REPRESENTS, in world units — the caller needs this to
     * size the quad it draws, since the buffer's own resolution may be capped
     * well below it. */
    /* Big enough for both things this buffer ever holds: the burning star
     * inside its corona, and the hole it may end as after being fed to its
     * ceiling. The second is usually the smaller of the two and costs nothing;
     * where it is not — a light star fed a lot of windows — the difference is
     * the whole of why a fed hole used to have its disc cut off. */
    d->world_half = cfg->radius * STAR_GLOW;
    double hole_half = star_hole_half(cfg);
    if (hole_half > d->world_half) d->world_half = hole_half;
    d->glow_r = d->world_half;
    d->side   = (int)lround(d->world_half * 2.0);
    if (d->side < 8) d->side = 8;
    if (d->side > STAR_MAX_SIDE) {
        d->side = STAR_MAX_SIDE;
        /* Everything inside the shader is in buffer pixels, so the radius it
         * is told about has to shrink with the canvas — the picture is the
         * same, drawn smaller and stretched back up. */
        d->glow_r = d->side / 2.0;
    }

    /* The shader, if this renderer will take it: it draws a better star and
     * costs less, since nothing crosses the bus and no buffer is allocated per
     * frame. The cairo star stays as the fallback for the Vulkan and pixman
     * renderers, exactly as rotate.c's callers keep theirs. */
    d->use_gl = server && star_gl_supported(server->wlr_renderer);
    if (d->use_gl) {
        for (int i = 0; i < 2; i++) {
            d->gpu[i] = snapshot_alloc(server, d->side, d->side);
            if (!d->gpu[i]) { d->use_gl = false; break; }
        }
        if (!d->use_gl)
            for (int i = 0; i < 2; i++) {
                if (d->gpu[i]) wlr_buffer_drop(d->gpu[i]);
                d->gpu[i] = NULL;
            }
    }

    /* Which star you are actually looking at. Said out loud because the two
     * paths do not look remotely alike — the shader has a surface, a disc and
     * lensing; the cairo fallback has none of those and cannot — and there is
     * no way to tell them apart from a screenshot without knowing which one
     * ran. */
    wlr_log(WLR_INFO, "star: drawing with %s%s",
            d->use_gl ? "the GPU shader" : "the cairo fallback",
            d->use_gl ? "" :
              (star_gl_supported(server ? server->wlr_renderer : NULL)
                 ? " (no buffer could be allocated)"
                 : " (this renderer is not GLES2 — see rotate.h)"));

    d->headless = (parent == NULL);
    if (d->headless) {
        /* Nothing in the scene graph: the caller will ask for the texture and
         * place it in its own pass. Only the shader can do this — the cairo
         * path draws through an overlay node by construction. */
        if (!d->use_gl) { free(d); return NULL; }
    } else if (d->use_gl) {
        d->buf = wlr_scene_buffer_create(parent, d->gpu[0]);
        if (d->buf) d->buf->point_accepts_input = star_declines_input;
        if (!d->buf) {
            for (int i = 0; i < 2; i++) wlr_buffer_drop(d->gpu[i]);
            free(d);
            return NULL;
        }
    } else {
        d->buf = cairo_overlay_create(parent, d->side, d->side);
        if (!d->buf) { free(d); return NULL; }
        d->buf->point_accepts_input = star_declines_input;
    }
    seed_surface(d);
    d->cell = cell_sprite();
    d->lens_scene = true;
    d->disc_tilt = -1.0;   /* no opinion until somebody has one */
    d->buf_radius = cfg->radius;
    d->dest_side = d->side;
    d->shown_half = d->world_half;
    /* FWM_DEBUG_STAR=1 reports what this actually costs, once a second. */
    const char *dbg = getenv("FWM_DEBUG_STAR");
    d->dbg = dbg && *dbg && *dbg != '0';
    return d;
}

void star_draw_destroy(FwmStarDraw *d) {
    if (!d) return;
    if (d->frame_tex) wlr_texture_destroy(d->frame_tex);
    if (d->bg_tex) wlr_texture_destroy(d->bg_tex);
    if (d->bg_buf) wlr_buffer_drop(d->bg_buf);
    if (d->use_gl) {
        if (d->buf) wlr_scene_node_destroy(&d->buf->node);
        for (int i = 0; i < 2; i++)
            if (d->gpu[i]) wlr_buffer_drop(d->gpu[i]);
    } else if (d->buf) {
        cairo_overlay_destroy(d->buf);
    }
    if (d->slow) cairo_surface_destroy(d->slow);
    if (d->cell) cairo_surface_destroy(d->cell);
    free(d);
}

/* Re-cut the canvas to `side` pixels.
 *
 * The buffer a star is given is sized once and never touched again — it is the
 * same picture at every size, so stretching it costs nothing anybody can see.
 * A hole is the exception the rule was not written for: it GROWS, by swallowing
 * windows, and its picture is a hairline ring that goes to mush the moment it
 * is stretched. So when a hole outgrows the pixels it inherited, it is given
 * more.
 *
 * Both frames and the photograph go together, since all three have to agree on
 * what one pixel is. The scene keeps its own reference to whatever buffer it
 * was last handed, so dropping ours here cannot pull the picture out from
 * under it; the next frame hands over one of the new ones. Failure leaves
 * everything as it was, which is a hole drawn a little soft rather than no
 * hole at all. */
static void star_recanvas(FwmStarDraw *d, int side) {
    if (!d->use_gl || side < 8 || side == d->side) return;
    struct wlr_buffer *next[2] = { NULL, NULL };
    for (int i = 0; i < 2; i++) {
        next[i] = snapshot_alloc(d->server, side, side);
        if (!next[i]) {
            for (int j = 0; j < i; j++) wlr_buffer_drop(next[j]);
            return;
        }
    }
    for (int i = 0; i < 2; i++) {
        if (d->gpu[i]) wlr_buffer_drop(d->gpu[i]);
        d->gpu[i] = next[i];
    }
    d->gpu_next = 0;
    /* The photograph is taken at the canvas's own size; it is rebuilt on
     * demand, so dropping it is the whole of resizing it. */
    if (d->bg_tex) { wlr_texture_destroy(d->bg_tex); d->bg_tex = NULL; }
    if (d->bg_buf) { wlr_buffer_drop(d->bg_buf); d->bg_buf = NULL; }
    d->side   = side;
    d->glow_r = side / 2.0;
    d->drawn  = false;
}

void star_draw_set_visible(FwmStarDraw *d, bool visible) {
    if (d && d->buf) wlr_scene_node_set_enabled(&d->buf->node, visible);
}

struct wlr_texture *star_draw_texture(FwmStarDraw *d) {
    return d ? d->frame_tex : NULL;
}

int star_draw_side(const FwmStarDraw *d) {
    return d ? d->side : 0;
}

double star_draw_extent(const FwmStarDraw *d) {
    if (!d) return 0.0;
    /* What the last frame drawn actually stands for, which is the buffer's own
     * box for everything except a hole. The orrery sizes its billboard from
     * this, so a hole in the middle of the ring is the size its mass says. */
    return d->shown_half > 0.0 ? d->shown_half : d->world_half;
}

void star_draw_set_lensing(FwmStarDraw *d, bool on) {
    if (d) d->lens_scene = on;
}

void star_draw_set_ring(FwmStarDraw *d, unsigned tex,
                        float u0, float v0, float du, float dv) {
    if (!d) return;
    d->ring_tex = tex;
    d->ring_rect[0] = u0;
    d->ring_rect[1] = v0;
    d->ring_rect[2] = du;
    d->ring_rect[3] = dv;
}

void star_draw_set_disc_roll(FwmStarDraw *d, double roll) {
    if (d) d->disc_roll = roll;
}

void star_draw_set_disc_tilt(FwmStarDraw *d, double tilt) {
    if (!d) return;
    if (tilt < 0.0) tilt = 0.0;
    if (tilt > 1.0) tilt = 1.0;
    d->disc_tilt = tilt;
}

void star_draw_raise(FwmStarDraw *d) {
    if (d && d->buf) wlr_scene_node_raise_to_top(&d->buf->node);
}

void star_draw_lower(FwmStarDraw *d) {
    if (d && d->buf) wlr_scene_node_lower_to_bottom(&d->buf->node);
}

/* Enough of a change to be worth a repaint.
 *
 * A star is never actually still — it boils, its loops rise and fall, its
 * corona wanders — so unlike the sun there is no quiet tick to skip. What the
 * cap buys instead is the difference between drawing this at the monitor's
 * refresh rate and drawing it at a rate the eye cannot tell from it. The one
 * thing that always redraws is a change of phase: that is a different star. */
static bool worth_repainting(const FwmStarDraw *d, const struct star_paint *p,
                             double now_s) {
    if (!d->drawn || d->drawn_phase != p->phase) return true;
    /* On the GPU there is nothing to ration: the shader draws into a buffer
     * that never leaves the card, and the two buffers are allocated once. All
     * the rate limiting below exists because the cairo path allocates and
     * uploads a megabyte per repaint — none of which applies here, so the star
     * simply animates at the frame rate, which is what it should always have
     * done. */
    if (d->use_gl) return true;
    /* A hole has nothing that moves: no surface, no corona, no beam. It is
     * drawn once and left alone. */
    /* A hole has no light of its own, but everything around it moves: the
     * disc orbits, the far image shears, the ring flickers. On the cairo path
     * it really is a still picture; on the GPU it is the busiest thing here,
     * and that case is already taken by the check above. */
    if (p->phase == STAR_HOLE)
        return fabs(d->drawn_radius - p->radius) > 0.5;
    /* Mid-collapse the size is carried by the node's scale, so the pixels are
     * only here for the colour shift — which is slow. */
    if (p->phase == STAR_COLLAPSE)
        return now_s - d->drawn_at_s >= 1.0 / 3.0;
    /* A pulsar's beam turns, and a beam stepping at 12Hz reads as a stutter
     * rather than a sweep — it is the one thing on this star that moves fast. */
    if (p->phase == STAR_NEUTRON)
        return now_s - d->drawn_at_s >= 1.0 / 24.0;
    return now_s - d->drawn_at_s >= 1.0 / STAR_REPAINT_HZ;
}

/* Photograph the desktop the hole is standing in front of.
 *
 * The whole scene, minus the hole itself — which has to be switched off for
 * the duration or it would photograph its own last frame and feed it back in,
 * frame after frame, until the screen is nothing but hole. The same trick
 * view_snapshot_into plays with a window's shadow.
 *
 * Returns NULL if the photograph could not be taken, and the shader then falls
 * back to its own starfield; nothing here is allowed to fail loudly. */
static struct wlr_texture *star_draw_photograph(FwmStarDraw *d, const FwmStar *star,
                                                int camera_x, int origin_x, int origin_y) {
    if (!d->server || !d->buf) return NULL;
    if (!d->bg_buf) {
        d->bg_buf = snapshot_alloc(d->server, d->side, d->side);
        if (!d->bg_buf) return NULL;
    }

    /* One photograph per frame, and no rationing.
     *
     * It used to be rationed to 15Hz whenever the hole sat still, because the
     * photograph was the expensive half — a render pass that imported the
     * whole wallpaper afresh and then walked EVERY buffer in the scene, most
     * of them on other desktops entirely. With both of those gone (see
     * wallpaper_layer_texture and the cull in snapshot_add_buffer) it is a
     * pass over the handful of buffers that actually stand under the lens, and
     * the rationing costs more than it saves.
     *
     * What it cost is the whole reason this changed: the wallpaper behind a
     * hole is a still picture, so nobody could see it being served four frames
     * out of date — but a WINDOW behind one moves, and its bent image lagged a
     * sixth of a second behind the unbent part of the same window standing
     * beside it. Dragging a window past a hole showed it plainly. The lens has
     * to see the desktop the frame it is drawn in. */
    /* Layout coordinates of the buffer's top-left: the same trip from world to
     * screen every window makes. */
    int lx = (int)lround(star->wx - camera_x + origin_x - d->side / 2.0);
    int ly = (int)lround(star->wy + origin_y - d->side / 2.0);

    /* Wallpaper included: photographed through the scene graph alone it came
     * back as windows over nothing, so the lens bent a film of windows and
     * left the wallpaper behind them flat. */
    bool ok = snapshot_lens(d->server, d->out, d->bg_buf, lx, ly, &d->buf->node);
    if (!ok) {
        if (d->dbg) wlr_log(WLR_INFO, "star: could not photograph the desktop");
        return NULL;
    }

    /* One texture for all of them. The buffer is the renderer's own, made once
     * and drawn into again and again, and a texture of it is a VIEW of that
     * memory rather than a copy taken at the moment it was made — which is
     * what render-to-texture is. Importing it afresh each photograph built and
     * threw away an EGL image every frame for a picture that had not moved. */
    if (!d->bg_tex)
        d->bg_tex = wlr_texture_from_buffer(d->server->wlr_renderer, d->bg_buf);
    if (d->dbg && !d->bg_tex)
        wlr_log(WLR_INFO, "star: photographed the desktop but could not use it");
    return d->bg_tex;
}

void star_draw_update(FwmStarDraw *d, const FwmStar *star, const StarConfig *cfg,
                      double now_s, int camera_x, int origin_x, int origin_y) {
    if (!d || !star || !cfg) return;
    if (!d->buf && !d->headless) return;

    d->last_now_s = now_s;
    struct star_paint p;
    gather(&p, d, star, cfg, now_s);

    /* A collapse is a change of SIZE, and a change of size does not need new
     * pixels.
     *
     * Repainting for it meant rebuilding the cached layer on nearly every
     * frame — the one case where the cache never hits — and handing the scene
     * a freshly allocated buffer each time, which is an upload and a full
     * damage region. So the buffer is painted at one radius and the node is
     * scaled to whatever radius the star is now. What scaling cannot carry is
     * the colour going blue, so the pixels are still refreshed during a
     * collapse — just slowly, at a rate chosen for the colour rather than for
     * the motion. The motion is exact regardless, because it is geometry. */
    /* The scaling trick below is a workaround for the price of cairo repaints;
     * the shader has no such price and draws whatever radius the star actually
     * is, every frame. */
    bool collapsing = (p.phase == STAR_COLLAPSE) && !d->use_gl;
    double paint_radius = collapsing ? (d->buf_radius > 0.0 ? d->buf_radius : cfg->radius)
                                     : p.radius;

    /* Does this thing bend what is behind it hard enough to be worth the
     * photograph? A hole always; a pulsar too, which is the whole of what a
     * neutron star's compactness buys it. */
    double compact = star_compactness(star, cfg);
    bool lensing = d->lens_scene && star_lenses(star, cfg);

    struct star_paint paint = p;
    paint.radius = paint_radius;
    /* When the canvas is capped, everything is drawn at the same fraction. */
    if (d->world_half > 0.0)
        paint.radius = paint_radius * (d->glow_r / d->world_half);

    /* How much world the picture stands for. A star is its glow box, which is
     * what the buffer was sized for; a hole is nine of its own radii (see
     * STAR_HOLE_BOX) and nothing more, so it is drawn filling the whole canvas
     * whatever it weighs and the node is scaled to whatever that is worth on
     * screen. Every pixel of the buffer goes on the hole that way, and the
     * framing the shader computes is the framing it is shown at — the two
     * things that a fed hole, drawn small in a corner of a star's canvas and
     * stretched to a star's node, had neither of. */
    double shown_half = d->world_half;
    if (p.phase == STAR_HOLE && p.radius > 0.0) {
        /* Enough pixels for the size it is actually shown at, up to the hole's
         * own ceiling — and only in real steps, so a hole being fed does not
         * reallocate three buffers a frame while the picture creeps outward. */
        int want = (int)lround(p.radius * STAR_HOLE_BOX * 2.0);
        if (want > STAR_MAX_SIDE) want = STAR_MAX_SIDE;
        if (d->use_gl && (want > d->side * 5 / 4 || want * 5 / 4 < d->side))
            star_recanvas(d, want);
        paint.radius = d->glow_r / STAR_HOLE_BOX;
        shown_half   = p.radius * STAR_HOLE_BOX;
        /* Never magnified past the pixels it was drawn with: a hairline photon
         * ring stretched over four screen pixels is a smear, and a hole that
         * stops growing is the better of the two failures. This is the same
         * ceiling a star meets — see STAR_MAX_SIDE — reached by the same
         * route. */
        if (shown_half > d->side / 2.0) shown_half = d->side / 2.0;
    }
    d->shown_half = shown_half;

    if (worth_repainting(d, &paint, now_s)) {
        double t0 = d->dbg ? now_ms() : 0.0;
        if (d->use_gl) {
            struct wlr_texture *bg = NULL;
            if (lensing)
                bg = star_draw_photograph(d, star, camera_x, origin_x, origin_y);
            StarGlParams gp = {
                .time_s    = now_s,
                .radius_px = paint.radius,
                .lum       = paint.lum,
                .phase     = paint.phase == STAR_COLLAPSE ? 1
                           : paint.phase == STAR_NEUTRON  ? 2
                           : paint.phase == STAR_HOLE     ? 3 : 0,
                .beam_deg  = paint.beam,
                .angle     = star->angle,
                .disc_tilt = d->disc_tilt,
                .disc_roll = d->disc_roll,
                .lens      = compact,
                /* Nothing to photograph, but expo may have handed us the ring
                 * this star is standing in the middle of. */
                .background_gl = (!lensing && star_lenses(star, cfg))
                               ? d->ring_tex : 0,
                .blast     = star_blast(star, cfg),
                .birth     = star_ignition(star, cfg),
                .form      = star_disc_form(star, cfg),
            };
            memcpy(gp.color, paint.color, sizeof(gp.color));
            if (gp.background_gl) memcpy(gp.bg_rect, d->ring_rect, sizeof(gp.bg_rect));
            /* A pulsar's beam is on a cone, so where it LOOKS like it points
             * is not its rotation phase — see star_pulse. */
            if (paint.phase == STAR_NEUTRON)
                star_pulse(star, cfg, &gp.beam_deg, &gp.beam_aim);
            gp.background = bg;
            struct wlr_buffer *target = d->gpu[d->gpu_next];
            if (star_gl_render(d->server->wlr_renderer, target, &gp)) {
                if (d->headless) {
                    /* Published as a texture for whoever is drawing the scene
                     * this belongs in. */
                    if (d->frame_tex) wlr_texture_destroy(d->frame_tex);
                    d->frame_tex = wlr_texture_from_buffer(d->server->wlr_renderer, target);
                } else {
                    wlr_scene_buffer_set_buffer_with_damage(d->buf, target, NULL);
                }
                d->gpu_next ^= 1;
            }
        } else {
            struct paint_ctx ctx = { .d = d, .p = &paint };
            cairo_overlay_update(d->buf, star_paint_cb, &ctx);
        }
        d->drawn_phase  = paint.phase;
        d->drawn_radius = paint.radius;
        d->drawn_lum    = paint.lum;
        d->drawn_beam   = paint.beam;
        d->drawn_at_s   = now_s;
        d->drawn        = true;
        d->buf_radius   = paint_radius;
        if (d->dbg) {
            double ms = now_ms() - t0;
            d->dbg_repaints++;
            d->dbg_ms += ms;
            if (ms > d->dbg_worst) d->dbg_worst = ms;
        }
    }

    /* Anything that bends what is behind it must be drawn OVER the windows,
     * for the plain reason that you cannot bend a picture that is painted on
     * top of you. Everything else is an object in the world and belongs under
     * them, which is where a burning star stays. The node moves across when
     * the answer changes, and only then. */
    bool want_front = lensing;
    if (!d->headless && want_front != d->in_front && d->front != d->behind) {
        wlr_scene_node_reparent(&d->buf->node, want_front ? d->front : d->behind);
        d->in_front = want_front;
    }

    /* Scale: the node is drawn at whatever fraction of the painted radius the
     * star has fallen to. Never up — a buffer stretched past its own size is
     * just blur — so the buffer is painted at the largest radius in play. */
    double k = (d->use_gl || d->buf_radius <= 0.0) ? 1.0 : (p.radius / d->buf_radius);
    if (k > 1.0) k = 1.0;
    if (k < 0.02) k = 0.02;
    int side = (int)lround(d->side * k);
    /* A hole stands for its own box rather than the buffer's, so its node is
     * that box in world pixels — bigger than the buffer for a heavy one, which
     * is a picture stretched, and smaller for a light one, which is a picture
     * drawn at more resolution than it is shown at. */
    if (p.phase == STAR_HOLE && d->shown_half > 0.0)
        side = (int)lround(d->shown_half * 2.0);
    if (side < 2) side = 2;
    if (!d->headless && side != d->dest_side) {
        wlr_scene_buffer_set_dest_size(d->buf, side, side);
        d->dest_side = side;
    }

    /* World to screen, the way every window makes the same trip: the star has
     * a place in the world and the monitor is a window onto it. Centred, so
     * the scaling above shrinks it about its own middle rather than its
     * corner. */
    if (!d->headless)
        wlr_scene_node_set_position(&d->buf->node,
                                    (int)lround(star->wx - camera_x + origin_x - side / 2.0),
                                    (int)lround(star->wy + origin_y - side / 2.0));

    if (d->dbg && now_s - d->dbg_at_s >= 1.0) {
        wlr_log(WLR_INFO,
                "star: %s r=%.0f  %d repaints/s, %.2f ms avg, %.2f ms worst, "
                "canvas %dpx, node %dpx",
                star_phase_name(star->phase), p.radius, d->dbg_repaints,
                d->dbg_repaints ? d->dbg_ms / d->dbg_repaints : 0.0, d->dbg_worst,
                d->side, side);
        d->dbg_at_s = now_s;
        d->dbg_repaints = 0;
        d->dbg_ms = 0.0;
        d->dbg_worst = 0.0;
    }
}
