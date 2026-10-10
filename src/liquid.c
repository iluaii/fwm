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

#include "liquid.h"
#include "crew.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── the grid ─────────────────────────────────────────────────────────────
 *
 * A MAC grid: pressure in the middle of each cell, the horizontal velocity on
 * the cell's left and right edges, the vertical on its top and bottom. That
 * staggering is what lets a pressure difference between two neighbouring cells
 * act on exactly the edge between them; on a grid that keeps everything in one
 * place the pressure can alternate cell by cell and nothing sees it.
 *
 * Every array carries PAD cells of margin past each wall. A particle reads a
 * 3x3 block of nodes around it, so one standing right at a wall reaches a node
 * and a half past it; the margin is where those reads land, and it is what
 * keeps every loop in here free of bounds checks. */
#define PAD 2

/* The gravity a `tension` is measured against. Fixed, not the desktop's: the
 * surface of a liquid does not get weaker because gravity was switched off,
 * and a knob that meant something different on every desktop would mean
 * nothing on any of them. */
#define EARTH_G 981.0

/* How many cells the fastest particle may cross in one internal step. A
 * pressure solve is stable at any step; what this limits is the transfer —
 * a particle that jumps past the nodes it was read from misses the walls and
 * the liquid it ran into. */
#define CFL 1.0

/* The most internal steps one liquid_step may take. A hard splash wants six
 * or seven; a frame owing more than this has been handed too long a dt, and
 * runs slow rather than unsafe. */
#define MAX_SUBSTEPS 16

/* Settling. A pool is never perfectly still — its surface is particles, and
 * they never quite stop jostling — so "asleep" is the liquid as a whole
 * crawling, with nothing in it faster than a trickle, for long enough that it
 * is not just turning round at the top of a slosh. */
#define SLEEP_RMS   3.0   /* px/s */
#define SLEEP_MAX  30.0   /* px/s */
#define SLEEP_AFTER 0.5   /* s */

/* A cell no particle happens to be in, inside the liquid, is still liquid:
 * with two particles to a cell side, the flow opens such gaps all the time.
 * Taken for air, every one of them is a bubble with no pressure in it that the
 * liquid around it falls into — which reads as a boiling, pitted body. So a
 * cell the particles around it fill this much counts as full. */
#define FILL 0.6f

/* Where the surface is, for the tension: a cell this full is wholly inside.
 * The bulk is never exactly at rest density, and measured against 1 its
 * jitter gave the inside a "surface" everywhere. */
#define SURFACE_FULL 0.7f

/* Particles drift: the grid keeps the FLOW from squeezing, but where the
 * particles land within a cell is theirs. Left alone they bunch, and a pool
 * settled a quarter shallower than the liquid poured into it. So a cell
 * fuller than it should be is told to spread out, and one emptier draws in,
 * this fraction of the difference per step.
 *
 * Drawing in only where the cell is surrounded by liquid on every side: a
 * cell at the surface is part air, and pulled to full it would suck the
 * surface down into the pool. And both ways or neither — spreading alone,
 * the particles' jostle was a ratchet that only ever pushed them apart, and
 * the same pool settled a sixth DEEPER, and kept rising. Gentle, because
 * against the surface tension, which packs the edge of a drop, a hard shove
 * blows particles off it.
 *
 * A rate per second, not a share per step. As a share per step it was a
 * velocity of share/h, and h is chosen from the fastest particle — so the
 * spreading sped the particles up, which shortened the step, which made the
 * spreading faster still. A big pour landing in a pool near the floor ran
 * that loop up to tens of thousands of px/s and the ceiling of steps, and the
 * liquid went into slow motion; and the same pool swelled at a different
 * speed on every refresh rate. Per second, a shorter step spreads less per
 * step and exactly as much per second. The old share came to 3 to 9 a
 * second depending on the tension and the monitor; 4.5 is low in that range,
 * because the swelling is momentum too — at 7.5 it shoved a strip of
 * tensionless liquid poured in overlapping blobs halfway round. */
#define DRIFT_RATE 4.5f   /* per second */

/* The pressure solve stops when nothing is left over bigger than this
 * fraction of what it started with. Tighter is invisible; looser lets a deep
 * pool sag. */
#define SOLVE_TOL 1e-4f
#define SOLVE_MAX_ITERS 200

/* How full a box may be poured. The liquid cannot be squeezed, so liquid past
 * the box's area has nowhere to be: the pressure has no answer, and the drift
 * correction shoves for good at cells it can never empty — a desktop poured
 * 5% over its area thrashed at the most steps a frame may take and never
 * settled. Full to the brim settles; a twentieth is left for air anyway, as
 * the margin between the two. */
#define BRIM 0.95

enum { AIR = 0, FLUID = 1, SOLID = 2 };

/* One particle, laid out the way the loops read it: everything a step touches
 * for a particle in one 32-byte run. Float, not double: the error that matters
 * here is the grid's, a cell wide, and float halves the memory traffic of the
 * passes that are most of the cost. It is also what a GPU wants. */
typedef struct {
    float x, y;            /* px inside the box */
    float vx, vy;          /* px/s */
    /* The velocity gradient around the particle (APIC's affine part):
     * c00 = du/dx, c01 = du/dy, c10 = dv/dx, c11 = dv/dy. Carrying it is what
     * keeps a swirl a swirl rather than letting each trip to the grid and back
     * smear it out, and it doubles as the strain rate the viscosity is made
     * of. */
    float c00, c01, c10, c11;
} Particle;

struct FwmLiquid {
    double spacing;
    float  dx, inv_dx;     /* a cell: two particles to a side */
    float  pmass;          /* = its share of area, density being 1 */
    int    budget;

    Particle *p;
    int n, cap;

    double w, h;           /* the box the caller asked for */
    float  ox, oy;         /* where the grid's first cell starts */
    int    nx, ny;         /* cells inside the walls */
    int    uw, vw, cw;     /* row strides, margin included */
    size_t un, vn, cn;     /* array lengths */

    float *u, *um;         /* horizontal velocity on vertical edges, and mass */
    float *v, *vm;         /* vertical velocity on horizontal edges, and mass */
    unsigned char *uok, *vok, *okb;  /* edges holding a real value */
    float *ftmp;           /* extrapolation scratch, edge-sized */

    float *mass;           /* per cell */
    float *colour;         /* 0 outside .. 1 inside, for the tension */
    float *txx, *tyy, *txy;
    unsigned char *type;

    /* The pressure solve, over the fluid cells only, strip by strip (see
     * "the pressure solve"): strip k is fluid[strip_at[k] .. strip_at[k + 1])
     * and the columns strip_col[k] .. strip_col[k + 1], in grid order inside
     * it, which is the order the preconditioner's sweeps need. Beside each
     * cell, which of its neighbours count and how many are not wall. */
    int   *fluid;
    int    nfluid;
    unsigned char *nbits, *ndiag;
    int    strip_at[LIQUID_MAX_THREADS + 1];
    int    strip_col[LIQUID_MAX_THREADS + 1];
    unsigned char *ring;   /* per wall cell, for release_walls */
    float *pres, *rhs, *res, *aux, *dir, *prec, *q;
    double last_h;         /* the step `pres` was solved for */

    LiquidMaterial mat;
    double gx, gy;
    bool have_g;

    /* The solids (liquid_set_solids): where they were left at the end of the
     * last step and where they are to be at the end of the next; during a
     * step, each one's start, motion and where it is now; which cells it
     * stands on, and the edges where it meets the rest. */
    LiquidSolid *sol_prev, *sol_next;
    int nsol_prev, nsol_next, sol_cap;
    struct body *bodies;
    int nbodies;
    unsigned char *sid;    /* per cell: 1 + the body standing on it, or 0 */
    int nsolid;            /* cells under a body, as of the last step */
    struct sface *sfaces;
    int nsfaces, sfaces_cap;
    int *flood;            /* release_solids' stack */
    double smax;           /* the fastest solid surface near the liquid */
    /* The particles' bounding box after the last step: "near the liquid". */
    float lo_x, lo_y, hi_x, hi_y;

    bool asleep;
    double calm_s;
    int substeps, iters;
    double vmax, vrms;

    unsigned rng;

    /* The step split across a crew (liquid_set_threads). Each member but the
     * first gathers its particles into a grid of its own — u, um, v, vm and
     * mass, back to back, `pstride` floats — and every member marks the
     * cells its particles stand in, `cn` bytes each; then they are added up.
     * One sum per member per slot, a cache line each, for the reductions. */
    FwmCrew *crew;
    int parts;
    float *pgrid;
    size_t pstride;
    unsigned char *pocc;
    struct { _Alignas(64) double v[8]; } psum[LIQUID_MAX_THREADS];
};

#define UIX(l, i, j) ((size_t)((j) + PAD) * (l)->uw + (size_t)((i) + PAD))
#define VIX(l, i, j) ((size_t)((j) + PAD) * (l)->vw + (size_t)((i) + PAD))
#define CIX(l, i, j) ((size_t)((j) + PAD) * (l)->cw + (size_t)((i) + PAD))

/* ── small helpers ───────────────────────────────────────────────────── */

/* Deterministic and private, like grass.c's: the jitter in a pour must not
 * reach into rand() and tug at whatever else in the compositor uses it. */
static unsigned xr_next(unsigned *s) {
    unsigned x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}
static double xr_unit(unsigned *s) {
    return (double)(xr_next(s) & 0xffffff) / (double)0x1000000;
}

/* The three quadratic B-spline weights around a point `g` grid units along
 * one axis of a lattice whose nodes sit at the integers. Returns the first
 * node; `f` is how far past it the point is, in [0.5, 1.5). */
static inline int stencil(float g, float w[3], float *f) {
    int b = (int)floorf(g - 0.5f);
    float t = g - (float)b;
    float a = 1.5f - t, m = t - 1.0f, c = t - 0.5f;
    w[0] = 0.5f * a * a;
    w[1] = 0.75f - m * m;
    w[2] = 0.5f * c * c;
    *f = t;
    return b;
}

/* What every job of a step is handed: the liquid and the step. */
struct step_job {
    FwmLiquid *l;
    float h;
    bool lists;      /* the pressure job: the strips are new */
};

static void wake(FwmLiquid *l) {
    l->asleep = false;
    l->calm_s = 0.0;
}

/* ── lifetime ────────────────────────────────────────────────────────── */

FwmLiquid *liquid_create(double spacing, int budget) {
    if (!(spacing > 0.0) || budget < 0) return NULL;
    FwmLiquid *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->spacing = spacing;
    /* Two particles to a cell side, four to a cell: fewer and cells inside
     * the liquid go empty; more is paying for particles the grid cannot tell
     * apart. */
    l->dx     = (float)(spacing * 2.0);
    l->inv_dx = 1.0f / l->dx;
    l->pmass  = (float)(spacing * spacing);
    l->budget = budget;
    l->mat = (LiquidMaterial){ .viscosity = 0.0, .tension = 0.0 };
    l->rng = 0x2545f491u;
    l->parts = 1;
    return l;
}

static void grid_free(FwmLiquid *l) {
    free(l->u);    free(l->um);   free(l->v);    free(l->vm);
    free(l->uok);  free(l->vok);  free(l->okb);  free(l->ftmp);
    free(l->mass); free(l->colour);
    free(l->txx);  free(l->tyy);  free(l->txy);  free(l->type);
    free(l->fluid); free(l->nbits); free(l->ndiag);
    free(l->sid);   free(l->flood);
    free(l->ring);
    free(l->pgrid); free(l->pocc);
    free(l->pres); free(l->rhs);  free(l->res);  free(l->aux);
    free(l->dir);  free(l->prec); free(l->q);
    l->u = l->um = l->v = l->vm = l->ftmp = NULL;
    l->uok = l->vok = l->okb = NULL;
    l->mass = l->colour = l->txx = l->tyy = l->txy = NULL;
    l->type = NULL;
    l->fluid = NULL;
    l->nbits = l->ndiag = NULL;
    l->sid = NULL;
    l->flood = NULL;
    l->ring = NULL;
    l->pgrid = NULL;
    l->pocc = NULL;
    l->pres = l->rhs = l->res = l->aux = l->dir = l->prec = l->q = NULL;
}

void liquid_destroy(FwmLiquid *l) {
    if (!l) return;
    crew_destroy(l->crew);
    free(l->p);
    free(l->sol_prev); free(l->sol_next);
    free(l->bodies);   free(l->sfaces);
    grid_free(l);
    free(l);
}

/* What each member of the crew gathers into, for the grid as it now is. */
static bool parts_alloc(FwmLiquid *l) {
    free(l->pgrid);
    free(l->pocc);
    l->pgrid = NULL;
    l->pocc = NULL;
    l->pstride = 2 * l->un + 2 * l->vn + l->cn;
    if (l->parts > 1) {
        l->pgrid = malloc((size_t)(l->parts - 1) * l->pstride * sizeof(float));
        if (!l->pgrid) return false;
    }
    l->pocc = malloc((size_t)l->parts * l->cn);
    return l->pocc != NULL;
}

void liquid_set_threads(FwmLiquid *l, int n) {
    if (!l) return;
    if (n < 1) n = 1;
    if (n > LIQUID_MAX_THREADS) n = LIQUID_MAX_THREADS;
    if (n == l->parts) return;
    crew_destroy(l->crew);
    l->crew = crew_create(n);
    /* No threads to be had is one thread, not no liquid. */
    l->parts = crew_size(l->crew);
    if (l->u && !parts_alloc(l)) {
        crew_destroy(l->crew);
        l->crew = NULL;
        l->parts = 1;
        if (!parts_alloc(l)) {
            grid_free(l);
            l->w = l->h = 0.0;
            l->nx = l->ny = 0;
        }
    }
}

int liquid_threads(const FwmLiquid *l) {
    return l ? l->parts : 1;
}

static int capacity(const FwmLiquid *l) {
    double room = l->w * l->h - (double)l->nsolid * l->dx * l->dx;
    return room > 0.0 ? (int)(BRIM * room / (l->spacing * l->spacing)) : 0;
}

static bool in_any_solid(const LiquidSolid *s, int n, double x, double y) {
    for (int k = 0; k < n; k++) {
        double cx = s[k].x + s[k].w / 2, cy = s[k].y + s[k].h / 2;
        double c = cos(s[k].angle), sn = sin(s[k].angle);
        double dx = x - cx, dy = y - cy;
        double lx = c * dx + sn * dy, ly = -sn * dx + c * dy;
        if (fabs(lx) < s[k].w / 2 && fabs(ly) < s[k].h / 2) return true;
    }
    return false;
}

/* Lowest first: y grows downwards. */
static int lower_first(const void *a, const void *b) {
    float ya = ((const Particle *)a)->y, yb = ((const Particle *)b)->y;
    return (ya < yb) - (ya > yb);
}

void liquid_set_box(FwmLiquid *l, double w, double h) {
    if (!l || !(w > 0.0) || !(h > 0.0)) return;
    if (w == l->w && h == l->h) return;
    grid_free(l);

    /* The floor is exactly the bottom of the box: liquid at rest is the most
     * looked-at thing here, and a pool standing a few px above the edge of
     * the screen, or sunk below it, is the first thing anyone would notice.
     * The box is not a whole number of cells, so the slack goes elsewhere —
     * half to each side wall and all of it above the ceiling, where liquid
     * hardly ever is and nobody can see it. */
    l->nx = (int)ceil(w * l->inv_dx);
    l->ny = (int)ceil(h * l->inv_dx);
    l->ox = (float)((w - l->nx * (double)l->dx) / 2.0);
    l->oy = (float)(h - l->ny * (double)l->dx);
    l->uw = l->nx + 1 + 2 * PAD;
    l->vw = l->nx + 2 * PAD;
    l->cw = l->nx + 2 * PAD;
    l->un = (size_t)l->uw * (size_t)(l->ny + 2 * PAD);
    l->vn = (size_t)l->vw * (size_t)(l->ny + 1 + 2 * PAD);
    l->cn = (size_t)l->cw * (size_t)(l->ny + 2 * PAD);
    size_t fmax = l->un > l->vn ? l->un : l->vn;

    bool ok = true;
#define GRAB(ptr, count, size) do { (ptr) = calloc((count), (size)); ok = ok && (ptr); } while (0)
    GRAB(l->u, l->un, sizeof(float));   GRAB(l->um, l->un, sizeof(float));
    GRAB(l->v, l->vn, sizeof(float));   GRAB(l->vm, l->vn, sizeof(float));
    GRAB(l->uok, l->un, 1);             GRAB(l->vok, l->vn, 1);
    GRAB(l->okb, fmax, 1);              GRAB(l->ftmp, fmax, sizeof(float));
    GRAB(l->mass, l->cn, sizeof(float)); GRAB(l->colour, l->cn, sizeof(float));
    GRAB(l->txx, l->cn, sizeof(float)); GRAB(l->tyy, l->cn, sizeof(float));
    GRAB(l->txy, l->cn, sizeof(float)); GRAB(l->type, l->cn, 1);
    GRAB(l->fluid, l->cn, sizeof(int));
    GRAB(l->nbits, l->cn, 1);           GRAB(l->ndiag, l->cn, 1);
    GRAB(l->sid, l->cn, 1);             GRAB(l->flood, l->cn, sizeof(int));
    GRAB(l->ring, (size_t)(2 * (l->nx + l->ny)), 1);
    GRAB(l->pres, l->cn, sizeof(float)); GRAB(l->rhs, l->cn, sizeof(float));
    GRAB(l->res, l->cn, sizeof(float));  GRAB(l->aux, l->cn, sizeof(float));
    GRAB(l->dir, l->cn, sizeof(float));  GRAB(l->prec, l->cn, sizeof(float));
    GRAB(l->q, l->cn, sizeof(float));
#undef GRAB
    ok = ok && parts_alloc(l);
    if (!ok) {
        /* No grid is a liquid that cannot step, which every call handles;
         * half a grid is not. */
        grid_free(l);
        l->w = l->h = 0.0;
        l->nx = l->ny = 0;
        return;
    }
    l->w = w;
    l->h = h;
    l->last_h = 0.0;

    /* Whatever now stands outside is pushed in to the wall it crossed. */
    float x0 = l->ox + 0.1f * l->dx, x1 = l->ox + l->nx * l->dx - 0.1f * l->dx;
    float y0 = l->oy + 0.1f * l->dx, y1 = l->oy + l->ny * l->dx - 0.1f * l->dx;
    for (int k = 0; k < l->n; k++) {
        Particle *p = &l->p[k];
        p->x = fminf(fmaxf(p->x, x0), x1);
        p->y = fminf(fmaxf(p->y, y0), y1);
    }
    /* And what the smaller box cannot hold runs over its top. */
    int most = capacity(l);
    if (l->n > most) {
        qsort(l->p, (size_t)l->n, sizeof(*l->p), lower_first);
        l->n = most;
    }
    wake(l);
}

void liquid_set_material(FwmLiquid *l, const LiquidMaterial *m) {
    if (!l || !m) return;
    LiquidMaterial v = *m;
    /* The step is chosen from these, so nothing may be negative or so large
     * that the step it implies is a rounding error. The config clamps the
     * same ranges; this is for a caller that is not the config. */
    if (!(v.viscosity >= 0.0)) v.viscosity = 0.0;
    if (v.viscosity > 100000.0) v.viscosity = 100000.0;
    if (!(v.tension >= 0.0)) v.tension = 0.0;
    if (v.tension > 500.0) v.tension = 500.0;
    if (v.viscosity == l->mat.viscosity && v.tension == l->mat.tension) return;
    l->mat = v;
    /* A thinner liquid flows where a thicker one had stopped, and a stronger
     * surface pulls on a puddle that had settled under a weaker one. */
    wake(l);
}

void liquid_set_budget(FwmLiquid *l, int budget) {
    if (l && budget >= 0) l->budget = budget;
}

static bool reserve(FwmLiquid *l, int want) {
    if (want <= l->cap) return true;
    int cap = l->cap ? l->cap : 1024;
    while (cap < want) cap *= 2;
    Particle *p = realloc(l->p, (size_t)cap * sizeof(*p));
    if (!p) return false;
    l->p = p;
    l->cap = cap;
    return true;
}

/* ── pouring ─────────────────────────────────────────────────────────── */

int liquid_pour(FwmLiquid *l, double cx, double cy, int count,
                double vx, double vy) {
    if (!l || count <= 0 || !l->u) return 0;
    int room = liquid_room(l);
    if (room <= 0) return 0;
    if (count > room) count = room;
    if (!reserve(l, l->n + count)) return 0;

    const double s = l->spacing;
    /* A disc holding `count` particles at rest spacing. */
    double r = s * sqrt((double)count / M_PI);

    /* Inside the walls, if it fits at all; centred if it does not, and the
     * walls then cut it to what does. */
    double m = r + s;
    cx = (2.0 * m < l->w) ? fmin(fmax(cx, m), l->w - m) : l->w / 2.0;
    cy = (2.0 * m < l->h) ? fmin(fmax(cy, m), l->h - m) : l->h / 2.0;

    /* Liquid already there is not stepped around: the new blob goes in on
     * top of it, the cells it lands in are twice as full as liquid can be,
     * and the drift correction spreads them out over the next few tenths of a
     * second — the pool swells to hold what was poured into it. Leaving the
     * occupied space out was tried first, and "pour 2000" into a ball put in
     * five hundred. An incompressible solve can take that; a squeezable one
     * could not, it would have gone off like a spring. */
    int added = 0;
    int k = (int)ceil(r / s) + 1;
    for (int j = -k; j <= k && added < count; j++) {
        for (int i = -k; i <= k && added < count; i++) {
            /* A little jitter: a perfect lattice is a crystal, and its rows
             * fall and splash in lockstep. */
            double px = cx + i * s + (xr_unit(&l->rng) - 0.5) * 0.2 * s;
            double py = cy + j * s + (xr_unit(&l->rng) - 0.5) * 0.2 * s;
            double ddx = px - cx, ddy = py - cy;
            if (ddx * ddx + ddy * ddy > r * r) continue;
            if (px < 0.5 * s || px > l->w - 0.5 * s) continue;
            if (py < 0.5 * s || py > l->h - 0.5 * s) continue;
            if (in_any_solid(l->sol_next, l->nsol_next, px, py)) continue;

            l->p[l->n++] = (Particle){
                .x = (float)px, .y = (float)py,
                .vx = (float)vx, .vy = (float)vy,
            };
            added++;
        }
    }
    if (added) {
        /* Near the liquid now, for liquid_set_solids, before a step says so. */
        if (l->n == added) {
            l->lo_x = l->lo_y = INFINITY;
            l->hi_x = l->hi_y = -INFINITY;
        }
        l->lo_x = fminf(l->lo_x, (float)(cx - r));
        l->lo_y = fminf(l->lo_y, (float)(cy - r));
        l->hi_x = fmaxf(l->hi_x, (float)(cx + r));
        l->hi_y = fmaxf(l->hi_y, (float)(cy + r));
        wake(l);
    }
    return added;
}

void liquid_clear(FwmLiquid *l) {
    if (!l) return;
    l->n = 0;
    l->vmax = l->vrms = 0.0;
    wake(l);
}

int liquid_drain(FwmLiquid *l, double cx, double cy, double r, int max) {
    if (!l || max <= 0 || !(r > 0.0)) return 0;
    float fx = (float)cx, fy = (float)cy, r2 = (float)(r * r);
    int gone = 0;
    /* Swapped out from the end, so the order the rest are in does not matter
     * and nothing is moved twice. */
    for (int k = 0; k < l->n && gone < max; ) {
        float dx = l->p[k].x - fx, dy = l->p[k].y - fy;
        if (dx * dx + dy * dy <= r2) {
            l->p[k] = l->p[--l->n];
            gone++;
        } else {
            k++;
        }
    }
    if (gone) wake(l);
    return gone;
}

/* ── solids ──────────────────────────────────────────────────────────────
 *
 * A solid is a box the caller moves. On the grid it is the cells whose
 * centres it covers, marked solid like the walls past the edge of the box,
 * and the edges where those cells meet the rest carry the solid's own
 * velocity across them — so a solid moving into liquid pushes it, one moving
 * away from it is followed, and one standing still is a wall. Along it the
 * liquid slides: the edges inside a solid have no value of their own and are
 * extrapolated from the liquid beside it, as past a wall.
 *
 * A step moves each solid at one speed from where it was left to where it is
 * to be, and the particles are kept out of it twice a substep: before they go
 * to the grid, for whatever the solid moved over, and after they move, for
 * whatever ran into it. Out means onto its nearest face, and no longer moving
 * into it. */

/* Faster than this over a step, a solid did not move there — it was put
 * there: a window sent across the desktop by a key arrives without a wave
 * the size of the desktop in front of it. Px/s at its fastest point. */
#define SOLID_JUMP 8000.0

/* How far outside a face a particle is put, in cells: off the face, so that
 * the next test does not find it on it. */
#define SKIN 0.01f

struct body {
    float hw, hh, reach;       /* half its size, and half its diagonal */
    float cx0, cy0, a0;        /* its centre and turn at the start of the step */
    float vx, vy, spin;        /* px/s and rad/s, over the step */
    float px, py, c, s;        /* now: its centre, and its turn as cos, sin */
};

/* An edge where a solid meets a cell that is not solid. */
struct sface {
    int f;                     /* the edge, in u (axis 0) or v (axis 1) */
    int cell;                  /* the cell on the liquid's side */
    unsigned char axis;
    unsigned char had;         /* the liquid had a velocity there */
    float out;                 /* +1 or -1: which way along the axis is away from the solid */
    float solid, own;          /* the solid's velocity across it, and the liquid's */
};

static const LiquidSolid *solid_with(const LiquidSolid *s, int n, unsigned id) {
    for (int k = 0; k < n; k++) if (s[k].id == id) return &s[k];
    return NULL;
}

/* Whether a box at this centre and reach, anywhere between two places, comes
 * within a couple of cells of the liquid. */
static bool near_liquid(const FwmLiquid *l, double x0, double y0, double x1, double y1,
                        double reach) {
    if (l->n == 0) return false;
    double m = reach + 2.0 * l->dx;
    return fmin(x0, x1) - m <= l->hi_x && fmax(x0, x1) + m >= l->lo_x
        && fmin(y0, y1) - m <= l->hi_y && fmax(y0, y1) + m >= l->lo_y;
}

static bool solid_moved(const LiquidSolid *a, const LiquidSolid *b) {
    return fabs(a->x - b->x) > 0.05 || fabs(a->y - b->y) > 0.05
        || a->w != b->w || a->h != b->h || fabs(a->angle - b->angle) > 1e-4;
}

static double reach_of(const LiquidSolid *s) {
    return 0.5 * sqrt(s->w * s->w + s->h * s->h);
}

void liquid_set_solids(FwmLiquid *l, const LiquidSolid *s, int n) {
    if (!l) return;
    if (n < 0 || !s) n = 0;
    if (n > 254) n = 254;   /* a cell names its solid in a byte */
    if (n > l->sol_cap) {
        LiquidSolid *a = realloc(l->sol_next, (size_t)n * sizeof(*a));
        if (!a) return;
        l->sol_next = a;
        LiquidSolid *b = realloc(l->sol_prev, (size_t)n * sizeof(*b));
        if (!b) return;
        l->sol_prev = b;
        struct body *c = realloc(l->bodies, (size_t)n * sizeof(*c));
        if (!c) return;
        l->bodies = c;
        l->sol_cap = n;
    }

    /* Wake only for what moved near the liquid: a window dragged about
     * across the top of a settled pool is no reason to step it. */
    bool wakes = false;
    for (int k = 0; k < n && !wakes; k++) {
        const LiquidSolid *now = &s[k];
        const LiquidSolid *was = solid_with(l->sol_prev, l->nsol_prev, now->id);
        if (was && !solid_moved(was, now)) continue;
        const LiquidSolid *from = was ? was : now;
        wakes = near_liquid(l, from->x + from->w / 2, from->y + from->h / 2,
                            now->x + now->w / 2, now->y + now->h / 2,
                            fmax(reach_of(from), reach_of(now)));
    }
    for (int k = 0; k < l->nsol_prev && !wakes; k++) {
        const LiquidSolid *was = &l->sol_prev[k];
        if (solid_with(s, n, was->id)) continue;
        wakes = near_liquid(l, was->x + was->w / 2, was->y + was->h / 2,
                            was->x + was->w / 2, was->y + was->h / 2, reach_of(was));
    }
    if (n) memcpy(l->sol_next, s, (size_t)n * sizeof(*s));
    l->nsol_next = n;
    if (wakes) wake(l);
}

/* The step's solids: where each starts, and how fast it goes to where it is
 * to be. And the fastest of them near the liquid, which the step has to keep
 * up with as it does with the liquid's own fastest. */
static void bodies_start(FwmLiquid *l, double dt) {
    l->nbodies = 0;
    l->smax = 0.0;
    for (int k = 0; k < l->nsol_next; k++) {
        const LiquidSolid *now = &l->sol_next[k];
        const LiquidSolid *was = solid_with(l->sol_prev, l->nsol_prev, now->id);
        struct body *b = &l->bodies[l->nbodies++];
        double cx = now->x + now->w / 2, cy = now->y + now->h / 2;
        b->hw = (float)(now->w / 2);
        b->hh = (float)(now->h / 2);
        b->reach = (float)reach_of(now);
        b->cx0 = (float)cx;
        b->cy0 = (float)cy;
        b->a0 = (float)now->angle;
        b->vx = b->vy = b->spin = 0.0f;
        if (was && was->w == now->w && was->h == now->h && dt > 0.0) {
            double wx = was->x + was->w / 2, wy = was->y + was->h / 2;
            double da = remainder(now->angle - was->angle, 2.0 * M_PI);
            double vx = (cx - wx) / dt, vy = (cy - wy) / dt, spin = da / dt;
            double fastest = sqrt(vx * vx + vy * vy) + fabs(spin) * b->reach;
            if (fastest <= SOLID_JUMP) {
                b->cx0 = (float)wx;
                b->cy0 = (float)wy;
                b->a0 = (float)(now->angle - da);
                b->vx = (float)vx;
                b->vy = (float)vy;
                b->spin = (float)spin;
                if (fastest > l->smax && near_liquid(l, wx, wy, cx, cy, b->reach))
                    l->smax = fastest;
            }
        }
    }
}

/* Every solid where it is `t` seconds into the step. */
static void bodies_at(FwmLiquid *l, double t) {
    for (int k = 0; k < l->nbodies; k++) {
        struct body *b = &l->bodies[k];
        b->px = b->cx0 + (float)(b->vx * t);
        b->py = b->cy0 + (float)(b->vy * t);
        float a = b->a0 + (float)(b->spin * t);
        b->c = cosf(a);
        b->s = sinf(a);
    }
}

/* The end of a step, or of one that did not run: what the solids were to be
 * is now what they are. */
static void bodies_settle(FwmLiquid *l) {
    if (l->nsol_next) memcpy(l->sol_prev, l->sol_next, (size_t)l->nsol_next * sizeof(*l->sol_prev));
    l->nsol_prev = l->nsol_next;
}

static inline void body_velocity(const struct body *b, float x, float y, float *vx, float *vy) {
    *vx = b->vx - b->spin * (y - b->py);
    *vy = b->vy + b->spin * (x - b->px);
}

/* Out of every solid it stands in: onto that solid's nearest face, and no
 * longer moving into it faster than the face is moving away. */
static void push_out(const FwmLiquid *l, Particle *p) {
    for (int k = 0; k < l->nbodies; k++) {
        const struct body *b = &l->bodies[k];
        float dx = p->x - b->px, dy = p->y - b->py;
        if (fabsf(dx) >= b->reach || fabsf(dy) >= b->reach) continue;
        float lx = b->c * dx + b->s * dy, ly = -b->s * dx + b->c * dy;
        float ex = b->hw - fabsf(lx), ey = b->hh - fabsf(ly);
        if (ex <= 0.0f || ey <= 0.0f) continue;
        float nx = 0.0f, ny = 0.0f, skin = SKIN * l->dx;
        if (ex < ey) {
            nx = lx < 0.0f ? -1.0f : 1.0f;
            lx = nx * (b->hw + skin);
        } else {
            ny = ly < 0.0f ? -1.0f : 1.0f;
            ly = ny * (b->hh + skin);
        }
        p->x = b->px + b->c * lx - b->s * ly;
        p->y = b->py + b->s * lx + b->c * ly;
        float wnx = b->c * nx - b->s * ny, wny = b->s * nx + b->c * ny;
        float svx, svy;
        body_velocity(b, p->x, p->y, &svx, &svy);
        float into = (p->vx - svx) * wnx + (p->vy - svy) * wny;
        if (into < 0.0f) {
            p->vx -= into * wnx;
            p->vy -= into * wny;
        }
    }
}

static bool add_sface(FwmLiquid *l, const struct sface *e) {
    if (l->nsfaces == l->sfaces_cap) {
        int cap = l->sfaces_cap ? 2 * l->sfaces_cap : 256;
        struct sface *a = realloc(l->sfaces, (size_t)cap * sizeof(*a));
        if (!a) return false;
        l->sfaces = a;
        l->sfaces_cap = cap;
    }
    l->sfaces[l->nsfaces++] = *e;
    return true;
}

/* One edge between cells a (before it along the axis) and b. */
static void solid_edge(FwmLiquid *l, int axis, size_t f, size_t a, size_t b, float x, float y) {
    float *vel = axis ? l->v : l->u;
    unsigned char *ok = axis ? l->vok : l->uok;
    int sa = l->sid[a], sb = l->sid[b];
    if (!sa && !sb) return;
    if (sa && sb) {
        /* Inside a solid, or between two: the liquid's, extrapolated. */
        ok[f] = 0;
        vel[f] = 0.0f;
        return;
    }
    const struct body *bd = &l->bodies[(sa ? sa : sb) - 1];
    float svx, svy;
    body_velocity(bd, x, y, &svx, &svy);
    struct sface e = {
        .f = (int)f, .cell = (int)(sa ? b : a), .axis = (unsigned char)axis,
        .had = ok[f], .out = sa ? 1.0f : -1.0f,
        .solid = axis ? svy : svx, .own = vel[f],
    };
    add_sface(l, &e);
    vel[f] = e.solid;
    ok[f] = 1;
}

/* The solids onto the grid: their cells solid, and their edges carrying
 * their velocity. */
static void close_solids(FwmLiquid *l) {
    const int nx = l->nx, ny = l->ny;
    const float dx = l->dx, idx = l->inv_dx;
    memset(l->sid, 0, l->cn);
    l->nsolid = 0;
    l->nsfaces = 0;
    if (l->nbodies == 0) return;

    for (int k = 0; k < l->nbodies; k++) {
        const struct body *b = &l->bodies[k];
        float ex = fabsf(b->c) * b->hw + fabsf(b->s) * b->hh;
        float ey = fabsf(b->s) * b->hw + fabsf(b->c) * b->hh;
        int i0 = (int)floorf((b->px - ex - l->ox) * idx), i1 = (int)floorf((b->px + ex - l->ox) * idx);
        int j0 = (int)floorf((b->py - ey - l->oy) * idx), j1 = (int)floorf((b->py + ey - l->oy) * idx);
        if (i0 < 0) i0 = 0;
        if (j0 < 0) j0 = 0;
        if (i1 > nx - 1) i1 = nx - 1;
        if (j1 > ny - 1) j1 = ny - 1;
        for (int j = j0; j <= j1; j++)
            for (int i = i0; i <= i1; i++) {
                size_t c = CIX(l, i, j);
                if (l->sid[c]) continue;
                float dxc = l->ox + (i + 0.5f) * dx - b->px, dyc = l->oy + (j + 0.5f) * dx - b->py;
                float lx = b->c * dxc + b->s * dyc, ly = -b->s * dxc + b->c * dyc;
                if (fabsf(lx) >= b->hw || fabsf(ly) >= b->hh) continue;
                l->sid[c] = (unsigned char)(k + 1);
                l->type[c] = SOLID;
                l->nsolid++;
            }
    }

    for (int j = 0; j < ny; j++)
        for (int i = 1; i < nx; i++)
            solid_edge(l, 0, UIX(l, i, j), CIX(l, i - 1, j), CIX(l, i, j),
                       l->ox + i * dx, l->oy + (j + 0.5f) * dx);
    for (int j = 1; j < ny; j++)
        for (int i = 0; i < nx; i++)
            solid_edge(l, 1, VIX(l, i, j), CIX(l, i, j - 1), CIX(l, i, j),
                       l->ox + (i + 0.5f) * dx, l->oy + j * dx);
}

/* Let go where a solid is pulling, as the walls do (release_walls): solved
 * with every solid edge closed, the liquid against a solid is under tension
 * wherever the solid is drawing it along — a window lifted out of a pool,
 * with the pool under it. That holds only where no air can get in behind: a
 * stretch of the solid's side under tension, from end to end, lets go if air
 * touches any of it, and the edges along it take the velocity the liquid had
 * of its own, if that was away from the solid. A window rising in the middle
 * of a pool is ringed by liquid pushed ahead of it, and is followed. True if
 * anything let go. */
static bool release_solids(FwmLiquid *l) {
    if (l->nsfaces == 0) return false;
    unsigned char *mark = l->okb;   /* free until the extrapolation */
    const int cw = l->cw;
    int pulling = 0;
    memset(mark, 0, l->cn);
    for (int k = 0; k < l->nsfaces; k++) {
        int c = l->sfaces[k].cell;
        if (l->type[c] == FLUID && l->pres[c] < 0.0f) {
            if (!mark[c]) pulling++;
            mark[c] = 1;
        }
    }
    if (pulling == 0) return false;

    /* From every pulling cell that touches air, along the pulling cells —
     * corners included, since a solid's side turns them. */
    int *stack = l->flood, top = 0;
    for (int k = 0; k < l->nsfaces; k++) {
        int c = l->sfaces[k].cell;
        if (mark[c] != 1) continue;
        if (l->type[c - 1] == AIR || l->type[c + 1] == AIR
            || l->type[c - cw] == AIR || l->type[c + cw] == AIR) {
            mark[c] = 2;
            stack[top++] = c;
        }
    }
    static const int dj[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
    static const int di[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    while (top > 0) {
        int c = stack[--top];
        for (int d = 0; d < 8; d++) {
            int n = c + dj[d] * cw + di[d];
            if (mark[n] == 1) {
                mark[n] = 2;
                stack[top++] = n;
            }
        }
    }

    bool any = false;
    for (int k = 0; k < l->nsfaces; k++) {
        const struct sface *e = &l->sfaces[k];
        if (mark[e->cell] == 2 && e->had && (e->own - e->solid) * e->out > 0.0f) {
            (e->axis ? l->v : l->u)[e->f] = e->own;
            any = true;
        }
    }
    return any;
}

/* More liquid than the room the solids leave runs over the top, as it does
 * from a box made smaller. */
static void spill(FwmLiquid *l) {
    int most = capacity(l);
    if (l->n > most) {
        qsort(l->p, (size_t)l->n, sizeof(*l->p), lower_first);
        l->n = most;
    }
}

/* ── the pressure solve ──────────────────────────────────────────────────
 *
 * One equation per fluid cell: the pressure it takes for the edges around it
 * to carry no more out than in. Air around a fluid cell holds no pressure at
 * all, which is what a free surface IS; a wall holds whatever it takes for
 * nothing to go through it, which is why a wall's edge is simply left out of
 * the equation. Conjugate gradients with an incomplete-Cholesky
 * preconditioner (MIC(0), as in Bridson's book), because the matrix is
 * symmetric and positive definite and this converges in tens of iterations
 * where plain relaxation takes thousands.
 *
 * Split across the crew in strips of whole columns, each holding about as
 * many fluid cells as the next — columns, because liquid gathers on the floor
 * and strips of rows would leave most of the crew with the sky. CG splits
 * the way any sum does: each member multiplies its own cells, and every dot
 * product is added up from one partial sum per member. The preconditioner
 * does not split that way — a Cholesky sweep walks the cells in order, each
 * waiting on the ones before it — so each strip gets its own, as if nothing
 * coupled it to the strips beside it (block Jacobi, MIC(0) in each block).
 * That is still symmetric and positive definite, which is all CG asks; what
 * it costs is a few iterations, as a pressure difference across a strip's
 * side is now seen an iteration later. With one member there is one strip,
 * and this is the plain MIC(0) it always was. */

/* Which neighbours of a fluid cell count, worked out once per step and kept
 * beside it in the list: the matrix wants the four that are liquid, the
 * preconditioner only those in its own strip, and two diagonal ones for the
 * modified part of MIC. */
enum {
    NB_L  = 1,     /* the cell to the left is liquid */
    NB_R  = 2,     /* ...to the right */
    NB_U  = 4,     /* ...above */
    NB_D  = 8,     /* ...below */
    NB_LS = 16,    /* left, and in the same strip */
    NB_RS = 32,    /* right, and in the same strip */
    NB_LD = 64,    /* left and one down is liquid */
    NB_UR = 128,   /* up and one right is liquid, and in the same strip */
};

/* The strips: the columns cut where the running count of fluid cells passes
 * each member's share. */
static void cut_strips(FwmLiquid *l) {
    const int nx = l->nx, ny = l->ny, parts = l->parts;
    /* The list is rebuilt from scratch right after this, so the first nx
     * places in it hold the column counts until then. */
    int *count = l->fluid;
    memset(count, 0, (size_t)nx * sizeof(int));
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++)
            count[i] += l->type[CIX(l, i, j)] == FLUID;
    int total = 0;
    for (int i = 0; i < nx; i++) total += count[i];

    int i = 0, sum = 0;
    l->strip_col[0] = 0;
    l->strip_at[0] = 0;
    for (int k = 1; k < parts; k++) {
        long long want = (long long)total * k / parts;
        while (i < nx && sum + count[i] <= want) sum += count[i++];
        l->strip_col[k] = i;
        l->strip_at[k] = sum;
    }
    l->strip_col[parts] = nx;
    l->strip_at[parts] = total;
    l->nfluid = total;
}

/* Strip k's cells into the list, in grid order, with their neighbours. */
static void strip_list(FwmLiquid *l, int k) {
    const int i0 = l->strip_col[k], i1 = l->strip_col[k + 1], cw = l->cw;
    const unsigned char *t = l->type;
    int n = l->strip_at[k];
    for (int j = 0; j < l->ny; j++)
        for (int i = i0; i < i1; i++) {
            size_t c = CIX(l, i, j);
            if (t[c] != FLUID) continue;
            unsigned b = 0;
            if (t[c - 1] == FLUID)      b |= NB_L | (i - 1 >= i0 ? NB_LS : 0);
            if (t[c + 1] == FLUID)      b |= NB_R | (i + 1 < i1 ? NB_RS : 0);
            if (t[c - cw] == FLUID)     b |= NB_U;
            if (t[c + cw] == FLUID)     b |= NB_D;
            if (t[c - 1 + cw] == FLUID) b |= NB_LD;
            if (t[c + 1 - cw] == FLUID && i + 1 < i1) b |= NB_UR;
            l->fluid[n] = (int)c;
            l->nbits[n] = (unsigned char)b;
            l->ndiag[n] = (unsigned char)((t[c - 1] != SOLID) + (t[c + 1] != SOLID)
                                        + (t[c - cw] != SOLID) + (t[c + cw] != SOLID));
            n++;
        }
}

static void strip_preconditioner(FwmLiquid *l, int k0, int k1) {
    const float tau = 0.97f, sigma = 0.25f;
    const int cw = l->cw;
    float *pc = l->prec;
    for (int k = k0; k < k1; k++) {
        int c = l->fluid[k];
        unsigned b = l->nbits[k];
        float diag = (float)l->ndiag[k];
        float e = diag;
        if (b & NB_LS) {
            float a = pc[c - 1] * pc[c - 1];
            e -= a;
            if (b & NB_LD) e -= tau * a;
        }
        if (b & NB_U) {
            float a = pc[c - cw] * pc[c - cw];
            e -= a;
            if (b & NB_UR) e -= tau * a;
        }
        if (e < sigma * diag) e = diag;
        pc[c] = 1.0f / sqrtf(e);
    }
}

/* What the pressure has to cancel in each fluid cell: the flow into or out of
 * it — plus a little, where the particles have drifted, so the cell evens
 * them out again (DRIFT_RATE). The unknown is pressure times h (density is 1),
 * which makes the update after the solve a plain difference. Returns the
 * largest of them. */
static float strip_rhs(FwmLiquid *l, int k0, int k1, float h) {
    const float idx = l->inv_dx, dx = l->dx, full = 4.0f * l->pmass;
    const int cw = l->cw;
    float most = 0.0f;
    for (int k = k0; k < k1; k++) {
        int c = l->fluid[k];
        /* A cell's left edge in u is its own index plus its row, u's rows
         * being one longer; in v it is its own index. */
        int row = c / cw;
        float div = (l->u[c + row + 1] - l->u[c + row] + l->v[c + cw] - l->v[c]) * idx;
        float over = l->mass[c] / full - 1.0f;
        bool inside = (l->nbits[k] & (NB_L | NB_R | NB_U | NB_D)) == (NB_L | NB_R | NB_U | NB_D);
        float spread = 0.0f;
        if (over > 0.0f || inside)
            spread = fmaxf(fminf(DRIFT_RATE * h * over, 0.5f), -0.5f) / h;
        float b = -(div - spread) * dx * dx;
        l->rhs[c] = b;
        if (fabsf(b) > most) most = fabsf(b);
    }
    return most;
}

/* y = A x over the strip, and the dot of y with `x` for it. */
static double strip_multiply(const FwmLiquid *l, int k0, int k1, const float *x, float *y) {
    const int cw = l->cw;
    double dot = 0.0;
    for (int k = k0; k < k1; k++) {
        int c = l->fluid[k];
        unsigned b = l->nbits[k];
        float s = 0.0f;
        if (b & NB_L) s += x[c - 1];
        if (b & NB_R) s += x[c + 1];
        if (b & NB_U) s += x[c - cw];
        if (b & NB_D) s += x[c + cw];
        float v = (float)l->ndiag[k] * x[c] - s;
        y[c] = v;
        dot += (double)v * (double)x[c];
    }
    return dot;
}

/* z = M^-1 r over the strip, and the dot of z with r for it. */
static double strip_precondition(FwmLiquid *l, int k0, int k1, const float *r, float *z) {
    const int cw = l->cw;
    const float *pc = l->prec;
    float *q = l->q;
    for (int k = k0; k < k1; k++) {
        int c = l->fluid[k];
        unsigned b = l->nbits[k];
        float s = r[c];
        if (b & NB_LS) s += pc[c - 1] * q[c - 1];
        if (b & NB_U)  s += pc[c - cw] * q[c - cw];
        q[c] = s * pc[c];
    }
    double dot = 0.0;
    for (int k = k1 - 1; k >= k0; k--) {
        int c = l->fluid[k];
        unsigned b = l->nbits[k];
        float s = q[c];
        if (b & NB_RS) s += pc[c] * z[c + 1];
        if (b & NB_D)  s += pc[c] * z[c + cw];
        z[c] = s * pc[c];
        dot += (double)z[c] * (double)r[c];
    }
    return dot;
}

/* The member sums of one slot, added up — in member order, so that every
 * member gets the same number and takes the same turn. */
static double summed(const FwmLiquid *l, int slot) {
    double s = 0.0;
    for (int q = 0; q < l->parts; q++) s += l->psum[q].v[slot];
    return s;
}
static double largest(const FwmLiquid *l, int slot) {
    double m = 0.0;
    for (int q = 0; q < l->parts; q++) if (l->psum[q].v[slot] > m) m = l->psum[q].v[slot];
    return m;
}

/* Solve for `pres` (which holds the last step's answer, scaled to this one,
 * as the first guess): each member its own strip, in step with the rest. */
static void job_pressure(void *ctx, int part, int parts) {
    const struct step_job *job = ctx;
    FwmLiquid *l = job->l;
    (void)parts;
    const int k0 = l->strip_at[part], k1 = l->strip_at[part + 1];
    double *mine = l->psum[part].v;
    float *x = l->pres, *r = l->res, *z = l->aux, *s = l->dir;

    if (job->lists) {
        strip_list(l, part);
        strip_preconditioner(l, k0, k1);
    }
    mine[0] = strip_rhs(l, k0, k1, job->h);
    crew_sync(l->crew);

    float scale = (float)largest(l, 0);
    if (scale < 1e-9f) {
        for (int k = k0; k < k1; k++) x[l->fluid[k]] = 0.0f;
        if (part == 0) l->iters = 0;
        return;
    }
    const float tol = SOLVE_TOL * scale;

    strip_multiply(l, k0, k1, x, z);
    float worst = 0.0f;
    for (int k = k0; k < k1; k++) {
        int c = l->fluid[k];
        r[c] = l->rhs[c] - z[c];
        if (fabsf(r[c]) > worst) worst = fabsf(r[c]);
    }
    mine[1] = worst;
    crew_sync(l->crew);
    if (largest(l, 1) < tol) {
        if (part == 0) l->iters = 0;
        return;
    }

    mine[2] = strip_precondition(l, k0, k1, r, z);
    for (int k = k0; k < k1; k++) s[l->fluid[k]] = z[l->fluid[k]];
    crew_sync(l->crew);
    double sigma = summed(l, 2);

    int it;
    for (it = 1; it <= SOLVE_MAX_ITERS; it++) {
        mine[3] = strip_multiply(l, k0, k1, s, z);
        crew_sync(l->crew);
        double ds = summed(l, 3);
        if (!(ds > 0.0)) break;
        float alpha = (float)(sigma / ds);
        worst = 0.0f;
        for (int k = k0; k < k1; k++) {
            int c = l->fluid[k];
            x[c] += alpha * s[c];
            r[c] -= alpha * z[c];
            if (fabsf(r[c]) > worst) worst = fabsf(r[c]);
        }
        mine[4] = worst;
        mine[5] = strip_precondition(l, k0, k1, r, z);
        crew_sync(l->crew);
        if (largest(l, 4) < tol) break;
        double sigma_new = summed(l, 5);
        float beta = (float)(sigma_new / sigma);
        for (int k = k0; k < k1; k++) {
            int c = l->fluid[k];
            s[c] = z[c] + beta * s[c];
        }
        sigma = sigma_new;
        /* Every strip's s in place before anyone multiplies by it. */
        crew_sync(l->crew);
    }
    if (part == 0) l->iters = it > SOLVE_MAX_ITERS ? SOLVE_MAX_ITERS : it;
}

/* The pressure for this step: the strips cut afresh when the cells did
 * change, and the solve. */
static void solve_pressure(FwmLiquid *l, float h, bool lists) {
    if (lists) cut_strips(l);
    struct step_job job = { .l = l, .h = h, .lists = lists };
    crew_run(l->crew, job_pressure, &job);
}

/* ── the step ────────────────────────────────────────────────────────── */

/* The longest internal step this liquid survives right now. */
static double stable_dt(const FwmLiquid *l) {
    double dx = l->dx;
    /* A solid moving through the liquid is as fast as anything in it. */
    double h = CFL * dx / (fmax(l->vmax, l->smax) + 1e-3);
    /* Viscosity is explicit diffusion, and explicit diffusion has its own
     * limit: the momentum it moves in one step must not overshoot the
     * neighbour it is evening out with. */
    if (l->mat.viscosity > 0.0) {
        double hv = 0.2 * dx * dx / l->mat.viscosity;
        if (hv < h) h = hv;
    }
    /* Capillary waves are fastest at the shortest wavelength the grid holds,
     * and an explicit surface tension is stable only while the step resolves
     * them (Brackbill, Kothe & Zemach 1992). */
    double gamma = l->mat.tension * l->mat.tension * EARTH_G;
    if (gamma > 0.0) {
        double hc = 0.5 * sqrt(dx * dx * dx / (2.0 * M_PI * gamma));
        if (hc < h) h = hc;
    }
    /* And nothing starting from rest may fall more than a cell in one step,
     * for the same reason as the CFL above. */
    double g = sqrt(l->gx * l->gx + l->gy * l->gy);
    if (g > 0.0) {
        double hg = sqrt(2.0 * dx / g);
        if (hg < h) h = hg;
    }
    return h;
}

/* Particles to the grid: momentum and mass on the edges, mass in the cells,
 * and which cells a particle stands in — for particles lo .. hi, into the
 * grid given, which is the liquid's own for the first member of the crew and
 * a private one for the rest.
 *
 * The momentum includes the affine part, which is the A in APIC, and the
 * viscosity. Viscosity is the stress each particle carries: twice the strain
 * rate, and C is the velocity gradient. It reaches the grid the way a stress
 * always does in MLS-MPM — the force a stress exerts on a node is its
 * divergence there, which for this kernel works out to the stress times the
 * node's offset — and the offset is the one the affine part is already
 * multiplied by, so the two go in as one term and one pass over the
 * particles. */
struct grid_view {
    float *u, *um, *v, *vm, *mass;
    unsigned char *occ;
};

static void gather(const FwmLiquid *l, int lo, int hi, float h, const struct grid_view *g) {
    const float x0 = l->ox + 0.1f * l->dx, x1 = l->ox + l->nx * l->dx - 0.1f * l->dx;
    const float y0 = l->oy + 0.1f * l->dx, y1 = l->oy + l->ny * l->dx - 0.1f * l->dx;
    const float dx = l->dx, idx = l->inv_dx, m = l->pmass;
    const float nu = (float)l->mat.viscosity;
    /* -dt * volume * D^-1, with D^-1 = 4/dx^2 for the quadratic kernel. */
    const float ks = -h * m * 4.0f * idx * idx * nu;
    const int nx = l->nx, ny = l->ny;
    for (int k = lo; k < hi; k++) {
        Particle *p = &l->p[k];
        /* Whatever a solid moved over this substep, it is off it now. */
        if (l->nbodies) {
            push_out(l, p);
            p->x = fminf(fmaxf(p->x, x0), x1);
            p->y = fminf(fmaxf(p->y, y0), y1);
        }
        float gx = (p->x - l->ox) * idx, gy = (p->y - l->oy) * idx;
        float wx[3], wy[3], fx, fy;
        float s00 = ks * 2.0f * p->c00, s11 = ks * 2.0f * p->c11;
        float s01 = ks * (p->c01 + p->c10);

        /* Horizontal velocity lives on vertical edges: integer x, half y. */
        float a = m * p->vx, bx = m * p->c00 + s00, by = m * p->c01 + s01;
        int ix = stencil(gx, wx, &fx), iy = stencil(gy - 0.5f, wy, &fy);
        for (int j = 0; j < 3; j++) {
            float dpy = ((float)j - fy) * dx;
            size_t row = UIX(l, ix, iy + j);
            for (int i = 0; i < 3; i++) {
                float w = wx[i] * wy[j];
                float dpx = ((float)i - fx) * dx;
                g->um[row + i] += w * m;
                g->u[row + i]  += w * (a + bx * dpx + by * dpy);
            }
        }
        /* Vertical velocity on horizontal edges: half x, integer y. */
        a = m * p->vy; bx = m * p->c10 + s01; by = m * p->c11 + s11;
        ix = stencil(gx - 0.5f, wx, &fx);
        iy = stencil(gy, wy, &fy);
        for (int j = 0; j < 3; j++) {
            float dpy = ((float)j - fy) * dx;
            size_t row = VIX(l, ix, iy + j);
            for (int i = 0; i < 3; i++) {
                float w = wx[i] * wy[j];
                float dpx = ((float)i - fx) * dx;
                g->vm[row + i] += w * m;
                g->v[row + i]  += w * (a + bx * dpx + by * dpy);
            }
        }
        /* Mass at cell centres: half x, half y. */
        ix = stencil(gx - 0.5f, wx, &fx);
        iy = stencil(gy - 0.5f, wy, &fy);
        for (int j = 0; j < 3; j++) {
            size_t row = CIX(l, ix, iy + j);
            for (int i = 0; i < 3; i++)
                g->mass[row + i] += wx[i] * wy[j] * m;
        }

        int ci = (int)gx, cj = (int)gy;
        if (ci < 0) ci = 0;
        if (ci >= nx) ci = nx - 1;
        if (cj < 0) cj = 0;
        if (cj >= ny) cj = ny - 1;
        g->occ[CIX(l, ci, cj)] = 1;
    }
}

/* Every member gathers its share of the particles; then, a share of the grid
 * each, the members' grids are added up, momentum becomes velocity, gravity
 * goes in, and each cell is told whether it is liquid. An edge no particle
 * reached has no velocity yet; the extrapolation after the pressure gives it
 * one. A cell counts as liquid when the particles around it fill it enough
 * (FILL), or when one stands in it. */
static void job_to_grid(void *ctx, int part, int parts) {
    const struct step_job *job = ctx;
    FwmLiquid *l = job->l;
    const size_t un = l->un, vn = l->vn, cn = l->cn;

    struct grid_view g;
    if (part == 0) {
        g = (struct grid_view){ l->u, l->um, l->v, l->vm, l->mass, NULL };
    } else {
        float *base = l->pgrid + (size_t)(part - 1) * l->pstride;
        g = (struct grid_view){ base, base + un, base + 2 * un, base + 2 * un + vn,
                                base + 2 * un + 2 * vn, NULL };
    }
    g.occ = l->pocc + (size_t)part * cn;
    memset(g.u, 0, un * sizeof(float));
    memset(g.um, 0, un * sizeof(float));
    memset(g.v, 0, vn * sizeof(float));
    memset(g.vm, 0, vn * sizeof(float));
    memset(g.mass, 0, cn * sizeof(float));
    memset(g.occ, 0, cn);

    int lo, hi;
    crew_share(l->n, part, parts, &lo, &hi);
    gather(l, lo, hi, job->h, &g);
    crew_sync(l->crew);

    const float hgx = (float)(job->h * l->gx), hgy = (float)(job->h * l->gy);
    const float *other = l->pgrid;
    const size_t ps = l->pstride;

    crew_share((int)un, part, parts, &lo, &hi);
    for (size_t f = (size_t)lo; f < (size_t)hi; f++) {
        float u = l->u[f], um = l->um[f];
        for (int q = 1; q < parts; q++) {
            u  += other[(size_t)(q - 1) * ps + f];
            um += other[(size_t)(q - 1) * ps + un + f];
        }
        l->um[f] = um;
        l->uok[f] = um > 0.0f;
        l->u[f] = um > 0.0f ? u / um + hgx : 0.0f;
    }
    crew_share((int)vn, part, parts, &lo, &hi);
    for (size_t f = (size_t)lo; f < (size_t)hi; f++) {
        float v = l->v[f], vm = l->vm[f];
        for (int q = 1; q < parts; q++) {
            v  += other[(size_t)(q - 1) * ps + 2 * un + f];
            vm += other[(size_t)(q - 1) * ps + 2 * un + vn + f];
        }
        l->vm[f] = vm;
        l->vok[f] = vm > 0.0f;
        l->v[f] = vm > 0.0f ? v / vm + hgy : 0.0f;
    }
    const float full = FILL * 4.0f * l->pmass;
    crew_share((int)cn, part, parts, &lo, &hi);
    for (size_t c = (size_t)lo; c < (size_t)hi; c++) {
        float mass = l->mass[c];
        unsigned char occ = l->pocc[c];
        for (int q = 1; q < parts; q++) {
            mass += other[(size_t)(q - 1) * ps + 2 * un + 2 * vn + c];
            occ |= l->pocc[(size_t)q * cn + c];
        }
        l->mass[c] = mass;
        int i = (int)(c % (size_t)l->cw) - PAD, j = (int)(c / (size_t)l->cw) - PAD;
        if (i < 0 || i >= l->nx || j < 0 || j >= l->ny) l->type[c] = SOLID;
        else l->type[c] = (occ || mass >= full) ? FLUID : AIR;
    }
}

/* Surface tension, as a stress: gamma * |g| * (I - n n^T), with g the
 * gradient of how full the cells are and n its direction (Lafaurie et al.
 * 1994). A tension along the surface and none across it — a stretched
 * membrane — whose divergence is exactly the pull toward the centre of
 * curvature that rounds a drop, while a flat surface, pulled equally from
 * both sides, feels nothing.
 *
 * On the grid and not on the particles. Carried by each particle, it was the
 * particles at the very edge of a drop that felt it most and lopsidedly, and
 * it threw them off the surface in a steady haze. Here it is the divergence
 * of one smooth field, taken across each edge: whatever one edge gains its
 * neighbour loses, so it cannot push the liquid anywhere as a whole, and it
 * does not care how the particles happen to lie. Only the first derivative of
 * the fullness is needed, where a curvature force would need the second and
 * be mostly noise at this resolution. */
static void job_tension(void *ctx, int part, int parts) {
    const struct step_job *job = ctx;
    FwmLiquid *l = job->l;
    const float gamma = (float)(l->mat.tension * l->mat.tension * EARTH_G);
    const int nx = l->nx, ny = l->ny, cw = l->cw;
    const float idx = l->inv_dx, h = job->h;
    const float fill = 4.0f * l->pmass * SURFACE_FULL;
    float *c = l->colour;
    int lo, hi;

    /* How full each cell is, a ring past the walls included. A wall reads as
     * neither liquid nor air — the fullness past it is a copy of the
     * fullness against it — so a drop on the floor meets it at a right
     * angle, neither beading up off it as if it were air nor creeping along
     * it. */
    crew_share(ny + 2, part, parts, &lo, &hi);
    for (int j = lo - 1; j < hi - 1; j++) {
        int sj = j < 0 ? 0 : j >= ny ? ny - 1 : j;
        for (int i = -1; i <= nx; i++) {
            int si = i < 0 ? 0 : i >= nx ? nx - 1 : i;
            float r = l->mass[CIX(l, si, sj)] / fill;
            c[CIX(l, i, j)] = r > 1.0f ? 1.0f : r;
        }
    }
    crew_sync(l->crew);

    /* Normal stresses at the cell centres... */
    crew_share(ny, part, parts, &lo, &hi);
    for (int j = lo; j < hi; j++)
        for (int i = 0; i < nx; i++) {
            size_t q = CIX(l, i, j);
            float gx = (c[q + 1] - c[q - 1]) * 0.5f * idx;
            float gy = (c[q + cw] - c[q - cw]) * 0.5f * idx;
            float g = sqrtf(gx * gx + gy * gy);
            l->txx[q] = g > 1e-6f ? gamma * gy * gy / g : 0.0f;
            l->tyy[q] = g > 1e-6f ? gamma * gx * gx / g : 0.0f;
        }
    /* ...and the shear one at the corners, where the staggered grid needs it:
     * corner (i, j) is the top-left of cell (i, j). */
    crew_share(ny + 1, part, parts, &lo, &hi);
    for (int j = lo; j < hi; j++)
        for (int i = 0; i <= nx; i++) {
            size_t q = CIX(l, i, j);
            float a = c[q], b = c[q - 1], d = c[q - cw], e = c[q - 1 - cw];
            float gx = (a + d - b - e) * 0.5f * idx;
            float gy = (a + b - d - e) * 0.5f * idx;
            float g = sqrtf(gx * gx + gy * gy);
            l->txy[q] = g > 1e-6f ? -gamma * gx * gy / g : 0.0f;
        }
    crew_sync(l->crew);

    crew_share(ny, part, parts, &lo, &hi);
    for (int j = lo; j < hi; j++)
        for (int i = 1; i < nx; i++) {
            size_t f = UIX(l, i, j);
            if (!l->uok[f]) continue;
            float fx = (l->txx[CIX(l, i, j)] - l->txx[CIX(l, i - 1, j)]
                      + l->txy[CIX(l, i, j + 1)] - l->txy[CIX(l, i, j)]) * idx;
            l->u[f] += h * fx;
        }
    for (int j = lo > 1 ? lo : 1; j < hi; j++)
        for (int i = 0; i < nx; i++) {
            size_t f = VIX(l, i, j);
            if (!l->vok[f]) continue;
            float fy = (l->txy[CIX(l, i + 1, j)] - l->txy[CIX(l, i, j)]
                      + l->tyy[CIX(l, i, j)] - l->tyy[CIX(l, i, j - 1)]) * idx;
            l->v[f] += h * fy;
        }
}

/* The walls: nothing goes through them, and they are slippery along them.
 *
 * Across a wall, its edge carries nothing — to begin with. A wall can push,
 * and it can pull only where no air can get in behind the liquid. Held at
 * zero regardless, it held liquid to itself by suction everywhere: a pool with
 * gravity turned round stayed on the floor, and a splash that reached the
 * ceiling hung there in sheets. Let go wherever it was pulling, it opened
 * pockets of nothing under a wave that was lifting faster than it was falling,
 * in the middle of a pool where no air could have been.
 *
 * So the pressure is solved with every wall closed, and then a wall lets go
 * along each stretch where it was PULLING — the pressure against it below
 * zero, the liquid there on its way out — that reaches the liquid's surface,
 * where air is (release_walls). A sheet on the ceiling peels from its edges;
 * a pool under reversed gravity is under tension right up to its surface, so
 * the air comes in down the side walls and it falls; the suction under a
 * lifting wave is ringed by ordinary pressure, so the floor holds. Then the
 * pressure is solved again. Letting go on the velocity alone, without the
 * pressure, was tried first: a pool at rest jostles against its floor, and a
 * floor that let go of every jostle kept the pool from ever settling.
 *
 * Written on every edge at or beyond the wall, which is the band a particle
 * against it reads. The edges past a wall that run ALONG it are left to be
 * extrapolated from the inside, which is what makes it slippery rather than
 * sticky. */
static void close_walls(FwmLiquid *l) {
    const int nx = l->nx, ny = l->ny;
    for (int j = -PAD; j < ny + PAD; j++)
        for (int i = -PAD; i <= nx + PAD; i++) {
            if (i > 0 && i < nx && j >= 0 && j < ny) continue;
            size_t f = UIX(l, i, j);
            if (i <= 0 || i >= nx) { l->u[f] = 0.0f; l->uok[f] = 1; }
            else l->uok[f] = 0;
        }
    for (int j = -PAD; j <= ny + PAD; j++)
        for (int i = -PAD; i < nx + PAD; i++) {
            if (j > 0 && j < ny && i >= 0 && i < nx) continue;
            size_t f = VIX(l, i, j);
            if (j <= 0 || j >= ny) { l->v[f] = 0.0f; l->vok[f] = 1; }
            else l->vok[f] = 0;
        }
}

/* The cells along the walls, as one ring: the floor left to right, the right
 * wall bottom to top, the ceiling right to left, the left wall top to bottom.
 * A corner cell is in it twice, once for each of its walls, and the two are
 * neighbours — which is what lets the air come round a corner. */
enum { FLOOR, RIGHT, CEILING, LEFT };

static void ring_slot(const FwmLiquid *l, int k, int *wall, int *i, int *j) {
    const int nx = l->nx, ny = l->ny;
    if (k < nx)               { *wall = FLOOR;   *i = k;                        *j = ny - 1; }
    else if (k < nx + ny)     { *wall = RIGHT;   *i = nx - 1;                   *j = ny - 1 - (k - nx); }
    else if (k < 2 * nx + ny) { *wall = CEILING; *i = nx - 1 - (k - nx - ny);   *j = 0; }
    else                      { *wall = LEFT;    *i = 0;                        *j = k - (2 * nx + ny); }
}

/* The edge just inside a wall slot, and whether the velocity in it is
 * leaving the wall. The edges next to a wall have not been touched by the
 * pressure yet when this runs, so what is in them is the velocity the liquid
 * had before the wall held it back. */
static float *leaving(FwmLiquid *l, int wall, int i, int j) {
    float *f;
    bool ok, away;
    switch (wall) {
    case FLOOR:   f = &l->v[VIX(l, i, l->ny - 1)]; ok = l->vok[VIX(l, i, l->ny - 1)]; away = *f < 0.0f; break;
    case RIGHT:   f = &l->u[UIX(l, l->nx - 1, j)]; ok = l->uok[UIX(l, l->nx - 1, j)]; away = *f < 0.0f; break;
    case CEILING: f = &l->v[VIX(l, i, 1)];         ok = l->vok[VIX(l, i, 1)];         away = *f > 0.0f; break;
    default:      f = &l->u[UIX(l, 1, j)];         ok = l->uok[UIX(l, 1, j)];         away = *f > 0.0f; break;
    }
    return ok && away ? f : NULL;
}

/* Open the wall along every stretch of the ring that is under tension and
 * reaches air, wherever the liquid on it is leaving, to the velocity it was
 * leaving with. True if any wall let go. */
static bool release_walls(FwmLiquid *l) {
    const int n = 2 * (l->nx + l->ny);
    unsigned char *st = l->ring;
    /* 0: the wall pushes here, or there is no liquid; 1: the liquid against
     * it is under tension, so air that got in anywhere along it would run
     * along it; 2: the same, and air is right there — this cell of liquid is
     * at the surface. Whether the liquid is also LEAVING the wall decides only
     * where the wall lets go, not where the air can travel: under reversed
     * gravity the side walls are not being pulled away from, but they are
     * under tension, and that is the way the air comes down to the floor. */
    int pulling = 0, start = -1;
    for (int k = 0; k < n; k++) {
        int wall, i, j;
        ring_slot(l, k, &wall, &i, &j);
        size_t c = CIX(l, i, j);
        st[k] = 0;
        if (l->type[c] != FLUID || l->pres[c] >= 0.0f) {
            start = k;
            continue;
        }
        bool air = l->type[c - 1] == AIR || l->type[c + 1] == AIR
                || l->type[c - l->cw] == AIR || l->type[c + l->cw] == AIR;
        st[k] = air ? 2 : 1;
        pulling++;
    }
    if (pulling == 0) return false;

    /* Each run of pulling slots lets go whole if any of it touches air. The
     * ring is a loop, so the walk starts just after a slot that pushes; if
     * there is none, the whole ring is one run. */
    if (start < 0) {
        bool air = false;
        for (int k = 0; k < n; k++) air |= st[k] == 2;
        for (int k = 0; k < n; k++) st[k] = air ? 3 : 0;
    } else {
        int run = -1;
        bool air = false;
        for (int s = 1; s <= n; s++) {
            int k = (start + s) % n;
            if (st[k]) {
                if (run < 0) { run = k; air = false; }
                air |= st[k] == 2;
                continue;
            }
            if (run >= 0)
                for (int r = run; r != k; r = (r + 1) % n) st[r] = air ? 3 : 0;
            run = -1;
        }
    }

    bool any = false;
    for (int k = 0; k < n; k++) {
        if (st[k] != 3) continue;
        int wall, i, j;
        ring_slot(l, k, &wall, &i, &j);
        float *in = leaving(l, wall, i, j);
        if (!in) continue;
        switch (wall) {
        case FLOOR:   for (int b = l->ny; b <= l->ny + PAD; b++) l->v[VIX(l, i, b)] = *in; break;
        case RIGHT:   for (int a = l->nx; a <= l->nx + PAD; a++) l->u[UIX(l, a, j)] = *in; break;
        case CEILING: for (int b = -PAD; b <= 0; b++)            l->v[VIX(l, i, b)] = *in; break;
        default:      for (int a = -PAD; a <= 0; a++)            l->u[UIX(l, a, j)] = *in; break;
        }
        any = true;
    }
    return any;
}

/* Give every edge that has no value of its own the average of the neighbours
 * that do, a few rings out. A particle at the surface reads edges that lie
 * partly in the air, and without this it would read zero there and be held
 * back by nothing at all. */
static void extrapolate(FwmLiquid *l, float *f, unsigned char *ok, size_t len,
                        int stride, int rings, int part, int parts, int slot) {
    unsigned char *next = l->okb;
    float *val = l->ftmp;
    const size_t first = (size_t)stride + 1, last = len - (size_t)stride - 1;
    int lo, hi;
    crew_share((int)len, part, parts, &lo, &hi);
    for (int r = 0; r < rings; r++) {
        bool any = false;
        for (size_t c = (size_t)lo; c < (size_t)hi; c++) {
            next[c] = ok[c];
            if (ok[c] || c < first || c >= last) continue;
            float s = 0.0f;
            int n = 0;
            if (ok[c - 1])      { s += f[c - 1];      n++; }
            if (ok[c + 1])      { s += f[c + 1];      n++; }
            if (ok[c - stride]) { s += f[c - stride]; n++; }
            if (ok[c + stride]) { s += f[c + stride]; n++; }
            if (n) { val[c] = s / (float)n; next[c] = 1; any = true; }
        }
        /* Its own slot: u is done with one when v starts on the other,
         * without a sync between, and a member still reading u's would see
         * v's. */
        l->psum[part].v[slot] = any;
        crew_sync(l->crew);
        if (summed(l, slot) == 0.0) break;
        for (size_t c = (size_t)lo; c < (size_t)hi; c++) {
            if (next[c] && !ok[c]) f[c] = val[c];
            ok[c] = next[c];
        }
        crew_sync(l->crew);
    }
}

/* The pressure onto the edges between liquid cells and the ones beside
 * them, and every edge left without a value given one. */
static void job_finish(void *ctx, int part, int parts) {
    const struct step_job *job = ctx;
    FwmLiquid *l = job->l;
    const int nx = l->nx, ny = l->ny;
    const float idx = l->inv_dx;
    int lo, hi;
    crew_share(ny, part, parts, &lo, &hi);
    for (int j = lo; j < hi; j++)
        for (int i = 1; i < nx; i++) {
            size_t a = CIX(l, i - 1, j), b = CIX(l, i, j);
            if (l->type[a] != FLUID && l->type[b] != FLUID) continue;
            if (l->sid[a] || l->sid[b]) continue;   /* the solid's, or let go */
            size_t f = UIX(l, i, j);
            l->u[f] -= (l->pres[b] - l->pres[a]) * idx;
            l->uok[f] = 1;
        }
    for (int j = lo > 1 ? lo : 1; j < hi; j++)
        for (int i = 0; i < nx; i++) {
            size_t a = CIX(l, i, j - 1), b = CIX(l, i, j);
            if (l->type[a] != FLUID && l->type[b] != FLUID) continue;
            if (l->sid[a] || l->sid[b]) continue;
            size_t f = VIX(l, i, j);
            l->v[f] -= (l->pres[b] - l->pres[a]) * idx;
            l->vok[f] = 1;
        }
    crew_sync(l->crew);
    extrapolate(l, l->u, l->uok, l->un, l->uw, 3, part, parts, 6);
    extrapolate(l, l->v, l->vok, l->vn, l->vw, 3, part, parts, 7);
}

/* Back to the particles: a velocity, the gradient around it, a move — each
 * member its share, and how fast its fastest went. */
static void job_to_particles(void *ctx, int part, int parts) {
    const struct step_job *job = ctx;
    FwmLiquid *l = job->l;
    const float h = job->h, idx = l->inv_dx, dx = l->dx;
    const int nx = l->nx, ny = l->ny;
    const float x0 = l->ox + 0.1f * dx, x1 = l->ox + nx * dx - 0.1f * dx;
    const float y0 = l->oy + 0.1f * dx, y1 = l->oy + ny * dx - 0.1f * dx;
    const float dinv = 4.0f * idx;   /* D^-1 times the dx in each offset */
    float vmax2 = 0.0f;
    double vsum2 = 0.0;
    float lox = INFINITY, loy = INFINITY, hix = -INFINITY, hiy = -INFINITY;
    int bad = 0, lo, hi;
    crew_share(l->n, part, parts, &lo, &hi);
    for (int k = lo; k < hi; k++) {
        Particle *p = &l->p[k];
        float gx = (p->x - l->ox) * idx, gy = (p->y - l->oy) * idx;
        float wx[3], wy[3], fx, fy;
        float vx = 0.0f, vy = 0.0f, c00 = 0.0f, c01 = 0.0f, c10 = 0.0f, c11 = 0.0f;

        int bx = stencil(gx, wx, &fx), by = stencil(gy - 0.5f, wy, &fy);
        for (int j = 0; j < 3; j++) {
            const float *row = l->u + UIX(l, bx, by + j);
            for (int i = 0; i < 3; i++) {
                float w = wx[i] * wy[j] * row[i];
                vx += w;
                c00 += w * ((float)i - fx);
                c01 += w * ((float)j - fy);
            }
        }
        bx = stencil(gx - 0.5f, wx, &fx);
        by = stencil(gy, wy, &fy);
        for (int j = 0; j < 3; j++) {
            const float *row = l->v + VIX(l, bx, by + j);
            for (int i = 0; i < 3; i++) {
                float w = wx[i] * wy[j] * row[i];
                vy += w;
                c10 += w * ((float)i - fx);
                c11 += w * ((float)j - fy);
            }
        }
        p->vx = vx;  p->vy = vy;
        p->c00 = c00 * dinv;  p->c01 = c01 * dinv;
        p->c10 = c10 * dinv;  p->c11 = c11 * dinv;

        p->x += h * vx;
        p->y += h * vy;
        /* The grid stops liquid at a solid's face the way it does at a wall,
         * and a substep's move can still carry a particle a hair into it. */
        if (l->nbodies) push_out(l, p);
        /* The grid stops liquid at the walls; this catches the last fraction
         * of a step that carries a particle a hair past one, which would
         * otherwise read nodes outside the grid next time. */
        if (p->x < x0) { p->x = x0; if (p->vx < 0.0f) p->vx = 0.0f; }
        if (p->x > x1) { p->x = x1; if (p->vx > 0.0f) p->vx = 0.0f; }
        if (p->y < y0) { p->y = y0; if (p->vy < 0.0f) p->vy = 0.0f; }
        if (p->y > y1) { p->y = y1; if (p->vy > 0.0f) p->vy = 0.0f; }

        float v2 = p->vx * p->vx + p->vy * p->vy;
        if (!isfinite(v2) || !isfinite(p->x) || !isfinite(p->y)) { bad++; continue; }
        if (v2 > vmax2) vmax2 = v2;
        vsum2 += v2;
        if (p->x < lox) lox = p->x;
        if (p->x > hix) hix = p->x;
        if (p->y < loy) loy = p->y;
        if (p->y > hiy) hiy = p->y;
    }

    double *mine = l->psum[part].v;
    mine[0] = vmax2;
    mine[1] = vsum2;
    mine[2] = bad;
    mine[3] = lox;
    mine[4] = loy;
    mine[5] = hix;
    mine[6] = hiy;
}

static void substep(FwmLiquid *l, float h) {
    struct step_job job = { .l = l, .h = h };

    /* 1. Particles to the grid — their momentum, the viscosity among it —
     * gravity, and which cells are liquid. */
    crew_run(l->crew, job_to_grid, &job);

    /* 2. Surface tension. */
    if (l->mat.tension > 0.0) crew_run(l->crew, job_tension, &job);

    close_walls(l);
    close_solids(l);

    /* 3. The pressure, starting from last step's: a pool at rest needs the
     * same pressure every step, and from there the solve has almost nothing
     * left to do. It is pressure times h, hence the rescale. A wall that lets
     * go changes what flows, not which cells are liquid, so the second solve
     * keeps the strips and their preconditioners. */
    float rescale = l->last_h > 0.0 ? (float)(h / l->last_h) : 0.0f;
    for (size_t c = 0; c < l->cn; c++)
        l->pres[c] = l->type[c] == FLUID ? l->pres[c] * rescale : 0.0f;
    solve_pressure(l, h, true);
    bool let_go = release_walls(l);
    let_go |= release_solids(l);
    if (let_go) solve_pressure(l, h, false);
    l->last_h = h;
    crew_run(l->crew, job_finish, &job);

    /* 4. Back to the particles. */
    crew_run(l->crew, job_to_particles, &job);
    float vmax2 = (float)largest(l, 0);
    double vsum2 = summed(l, 1);
    int bad = (int)summed(l, 2);
    l->lo_x = l->lo_y = INFINITY;
    l->hi_x = l->hi_y = -INFINITY;
    for (int q = 0; q < l->parts; q++) {
        const double *v = l->psum[q].v;
        l->lo_x = fminf(l->lo_x, (float)v[3]);
        l->lo_y = fminf(l->lo_y, (float)v[4]);
        l->hi_x = fmaxf(l->hi_x, (float)v[5]);
        l->hi_y = fmaxf(l->hi_y, (float)v[6]);
    }

    /* A particle gone to NaN would poison the grid for every neighbour on the
     * next step and the whole body after that. It cannot happen inside the
     * step limits; if it does anyway, that one particle is lost rather than
     * the liquid. */
    if (bad) {
        int out = 0;
        for (int k = 0; k < l->n; k++) {
            Particle *p = &l->p[k];
            if (isfinite(p->x) && isfinite(p->y) && isfinite(p->vx) && isfinite(p->vy))
                l->p[out++] = *p;
        }
        l->n = out;
    }
    l->vmax = sqrt((double)vmax2);
    l->vrms = l->n ? sqrt(vsum2 / l->n) : 0.0;
}

void liquid_step(FwmLiquid *l, double dt, double gx, double gy) {
    if (!l || !(dt > 0.0)) return;

    /* Gravity is the one thing that changes under a settled liquid without
     * anything having been done TO it: switch it off and a resting pool has
     * to lift and gather itself. */
    if (!l->have_g || gx != l->gx || gy != l->gy) {
        l->gx = gx;
        l->gy = gy;
        l->have_g = true;
        wake(l);
    }

    l->substeps = 0;
    if (l->n == 0 || !l->u || l->asleep) {
        bodies_settle(l);
        return;
    }

    bodies_start(l, dt);
    spill(l);
    double t = 0.0;
    while (t < dt && l->substeps < MAX_SUBSTEPS) {
        /* Equal steps over what is left, each within the limit: a long step
         * and a sliver to finish the frame would waste the sliver. */
        double left = dt - t;
        int n = (int)ceil(left / stable_dt(l) - 1e-9);
        if (n < 1) n = 1;
        double h = left / n;
        bodies_at(l, t + h);
        substep(l, (float)h);
        t += h;
        l->substeps++;
    }
    bodies_settle(l);

    /* Calm, and nothing near it moving: a window pushed slowly through a
     * pool keeps it awake however gently the pool moves. */
    if (l->vrms < SLEEP_RMS && l->vmax < SLEEP_MAX && l->smax < SLEEP_MAX) {
        l->calm_s += dt;
        if (l->calm_s >= SLEEP_AFTER) {
            /* Put down exactly at rest, so that waking it — a pour beside it,
             * a change of gravity — starts from still liquid and not from the
             * last of a jostle. */
            for (int k = 0; k < l->n; k++) {
                Particle *p = &l->p[k];
                p->vx = p->vy = 0.0f;
                p->c00 = p->c01 = p->c10 = p->c11 = 0.0f;
            }
            l->vmax = l->vrms = 0.0;
            l->asleep = true;
        }
    } else {
        l->calm_s = 0.0;
    }
}

/* ── reading it ──────────────────────────────────────────────────────── */

int liquid_count(const FwmLiquid *l) { return l ? l->n : 0; }
int liquid_budget(const FwmLiquid *l) { return l ? l->budget : 0; }
int liquid_room(const FwmLiquid *l) {
    if (!l || !l->u) return 0;
    int most = capacity(l) < l->budget ? capacity(l) : l->budget;
    return most > l->n ? most - l->n : 0;
}
double liquid_spacing(const FwmLiquid *l) { return l ? l->spacing : 0.0; }
bool liquid_asleep(const FwmLiquid *l) { return !l || l->n == 0 || l->asleep; }
int liquid_last_substeps(const FwmLiquid *l) { return l ? l->substeps : 0; }
double liquid_max_speed(const FwmLiquid *l) { return l ? l->vmax : 0.0; }
double liquid_rms_speed(const FwmLiquid *l) { return l ? l->vrms : 0.0; }
int liquid_last_iterations(const FwmLiquid *l) { return l ? l->iters : 0; }

const float *liquid_positions(const FwmLiquid *l, int *stride) {
    if (stride) *stride = (int)(sizeof(Particle) / sizeof(float));
    return (l && l->p) ? &l->p[0].x : NULL;
}
