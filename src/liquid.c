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
/* ...and nothing floating in it going faster than this, px/s: where the
 * compositor's physics lets a window it moves fall asleep too. */
#define SLEEP_BODY  5.0

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

/* Liquids that share a cell move with it: the grid sees the cell, and has
 * no way to tell one liquid in it from another. So a drop of oil smaller
 * than a cell, inside water, would never come up — which real ones do, at
 * the speed at which its buoyancy is balanced by the drag on it (Stokes):
 * (rho - rho_around) g d^2 / (18 mu), d the drop's size, and rho_around
 * and mu what it moves through: the cell it is in, less its own liquid — a
 * drop of honey in water falls through water, and is held back by water's
 * thickness, not its own. The drop is a cell: what is smaller than one the
 * grid does not see, and so moves as one. Each particle moves that much
 * against the cell it is in, which is nothing in a cell of its own liquid;
 * between two, it sorts them, and the edge between layers stays sharp. A
 * liquid with no thickness at all would give no drag and an endless speed,
 * so the drag is at least this thick's, px²/s, and no drift crosses half a
 * cell a step. */
#define DRIFT_NU_LEAST 1.0f

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
    unsigned kind;         /* which liquid it is */
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

    float *mass;           /* per cell: how much liquid, as area */
    float *heavy;          /* ...and as weight, area times density */
    /* With more than one liquid in the box, for the drift between them: area
     * times density times viscosity, and the area of each liquid on its own,
     * LIQUID_MAX_KINDS arrays of cn back to back. */
    float *thick, *kvol;
    bool mixed;            /* more than one liquid in it, as of the step */
    float *rho;            /* how dense the liquid in it is */
    /* The pressure solve's weights: across the edge to the right of a cell
     * and the one below it, one over the density there — 0 where the cell on
     * the other side is not liquid — and the cell's own diagonal. All 1s and
     * a count of neighbours with one liquid in the box. */
    float *wr, *wd, *pdiag;
    float *colour;         /* 0 outside .. 1 inside, for the tension */
    float *cover;          /* how much of each cell the solids stand on, 0 .. 1 */
    float *txx, *tyy, *txy;
    unsigned char *type;

    /* The pressure solve, over the fluid cells only, strip by strip (see
     * "the pressure solve"): strip k is fluid[strip_at[k] .. strip_at[k + 1])
     * and the columns strip_col[k] .. strip_col[k + 1], in grid order inside
     * it, which is the order the preconditioner's sweeps need. Beside each
     * cell, which of its neighbours count and how many are not wall. */
    int   *fluid;
    int    nfluid;
    unsigned char *nbits;
    int    strip_at[LIQUID_MAX_THREADS + 1];
    int    strip_col[LIQUID_MAX_THREADS + 1];
    unsigned char *ring;   /* per wall cell, for release_walls */
    float *pres, *rhs, *res, *aux, *dir, *prec, *q;
    double last_h;         /* the step `pres` was solved for */

    LiquidMaterial mat;    /* the tension; and liquid 0, as kind[0] is */
    LiquidMaterial kind[LIQUID_MAX_KINDS];
    unsigned present;      /* which kinds have particles, as of the step */
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
    /* The faces free solids share with liquid cells, for the pressure solve
     * (see "free solids"), and what the liquid did to each free solid over
     * the last step, by id. */
    struct couple *cpl;
    int ncpl, cpl_cap;
    struct push *push;
    int npush, push_cap;
    int *flood;            /* release_solids' and find_pockets' stack */
    /* Liquid shut in with no air — between a solid and a wall, say — as
     * runs of cells in `pocket`, pocket k from pocket_at[k]. */
    int *pocket, *pocket_at;
    int npockets, pocket_cap;
    double smax;           /* the fastest solid surface near the liquid */
    double hmax;           /* ...of the held ones, and whether there are any */
    bool held;
    double fmax;           /* the fastest free solid in it, after the last step */
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
    l->mat = (LiquidMaterial){ .viscosity = 0.0, .tension = 0.0, .density = 1.0 };
    for (int k = 0; k < LIQUID_MAX_KINDS; k++) l->kind[k] = l->mat;
    l->rng = 0x2545f491u;
    l->parts = 1;
    return l;
}

static void grid_free(FwmLiquid *l) {
    free(l->u);    free(l->um);   free(l->v);    free(l->vm);
    free(l->uok);  free(l->vok);  free(l->okb);  free(l->ftmp);
    free(l->mass); free(l->colour); free(l->cover);
    free(l->heavy); free(l->thick); free(l->kvol);
    free(l->rho); free(l->wr); free(l->wd); free(l->pdiag);
    free(l->txx);  free(l->tyy);  free(l->txy);  free(l->type);
    free(l->fluid); free(l->nbits);
    free(l->sid);   free(l->flood); free(l->pocket);
    free(l->ring);
    free(l->pgrid); free(l->pocc);
    free(l->pres); free(l->rhs);  free(l->res);  free(l->aux);
    free(l->dir);  free(l->prec); free(l->q);
    l->u = l->um = l->v = l->vm = l->ftmp = NULL;
    l->uok = l->vok = l->okb = NULL;
    l->mass = l->colour = l->cover = l->txx = l->tyy = l->txy = NULL;
    l->heavy = l->thick = l->kvol = l->rho = l->wr = l->wd = l->pdiag = NULL;
    l->type = NULL;
    l->fluid = NULL;
    l->nbits = NULL;
    l->sid = NULL;
    l->flood = NULL;
    l->pocket = NULL;
    l->ring = NULL;
    l->pgrid = NULL;
    l->pocc = NULL;
    l->pres = l->rhs = l->res = l->aux = l->dir = l->prec = l->q = NULL;
    /* Its cells are gone with the grid, and so is the pressure it held. */
    l->ncpl = 0;
    l->npush = 0;
}

void liquid_destroy(FwmLiquid *l) {
    if (!l) return;
    crew_destroy(l->crew);
    free(l->p);
    free(l->sol_prev); free(l->sol_next);
    free(l->bodies);   free(l->sfaces); free(l->pocket_at);
    free(l->cpl);      free(l->push);
    grid_free(l);
    free(l);
}

/* What each member of the crew gathers into, for the grid as it now is. */
static bool parts_alloc(FwmLiquid *l) {
    free(l->pgrid);
    free(l->pocc);
    l->pgrid = NULL;
    l->pocc = NULL;
    l->pstride = 2 * l->un + 2 * l->vn + (3 + LIQUID_MAX_KINDS) * l->cn;
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
    GRAB(l->cover, l->cn, sizeof(float));
    GRAB(l->heavy, l->cn, sizeof(float)); GRAB(l->rho, l->cn, sizeof(float));
    GRAB(l->thick, l->cn, sizeof(float));
    GRAB(l->kvol, (size_t)LIQUID_MAX_KINDS * l->cn, sizeof(float));
    GRAB(l->wr, l->cn, sizeof(float));  GRAB(l->wd, l->cn, sizeof(float));
    GRAB(l->pdiag, l->cn, sizeof(float));
    GRAB(l->txx, l->cn, sizeof(float)); GRAB(l->tyy, l->cn, sizeof(float));
    GRAB(l->txy, l->cn, sizeof(float)); GRAB(l->type, l->cn, 1);
    GRAB(l->fluid, l->cn, sizeof(int));
    GRAB(l->nbits, l->cn, 1);
    GRAB(l->sid, l->cn, 1);             GRAB(l->flood, l->cn, sizeof(int));
    GRAB(l->pocket, l->cn, sizeof(int));
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

/* The ranges a liquid is held to. The step is chosen from these, so nothing
 * may be negative or so large that the step it implies is a rounding error.
 * The config and the menu clamp the same; this is for a caller that is not
 * them. */
static LiquidMaterial clamped(const LiquidMaterial *m) {
    LiquidMaterial v = *m;
    if (!(v.viscosity >= 0.0)) v.viscosity = 0.0;
    if (v.viscosity > 100000.0) v.viscosity = 100000.0;
    if (!(v.tension >= 0.0)) v.tension = 0.0;
    if (v.tension > 500.0) v.tension = 500.0;
    if (!(v.density > 0.0)) v.density = 1.0;
    if (v.density < 0.05) v.density = 0.05;
    if (v.density > 50.0) v.density = 50.0;
    return v;
}

void liquid_set_material(FwmLiquid *l, const LiquidMaterial *m) {
    if (!l || !m) return;
    LiquidMaterial v = clamped(m);
    if (v.viscosity == l->mat.viscosity && v.tension == l->mat.tension
        && v.density == l->mat.density) return;
    l->mat = v;
    l->kind[0].viscosity = v.viscosity;
    l->kind[0].density = v.density;
    /* A thinner liquid flows where a thicker one had stopped, and a stronger
     * surface pulls on a puddle that had settled under a weaker one. */
    wake(l);
}

void liquid_set_kind(FwmLiquid *l, int kind, const LiquidMaterial *m) {
    if (!l || !m || kind < 0 || kind >= LIQUID_MAX_KINDS) return;
    LiquidMaterial v = clamped(m);
    LiquidMaterial *k = &l->kind[kind];
    if (v.viscosity == k->viscosity && v.density == k->density) return;
    k->viscosity = v.viscosity;
    k->density = v.density;
    if (kind == 0) {
        l->mat.viscosity = v.viscosity;
        l->mat.density = v.density;
    }
    /* A liquid made heavier sinks through what it lay on. */
    if (l->present & (1u << kind)) wake(l);
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
    return liquid_pour_kind(l, 0, cx, cy, count, vx, vy);
}

int liquid_pour_kind(FwmLiquid *l, int kind, double cx, double cy, int count,
                     double vx, double vy) {
    if (!l || count <= 0 || !l->u || kind < 0 || kind >= LIQUID_MAX_KINDS) return 0;
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
                .kind = (unsigned)kind,
            };
            added++;
        }
    }
    if (added) {
        l->present |= 1u << kind;
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

int liquid_count_kind(const FwmLiquid *l, int kind) {
    if (!l) return 0;
    int n = 0;
    for (int k = 0; k < l->n; k++) n += l->p[k].kind == (unsigned)kind;
    return n;
}

int liquid_remove_kind(FwmLiquid *l, int kind) {
    if (!l) return 0;
    int out = 0;
    for (int k = 0; k < l->n; k++)
        if (l->p[k].kind != (unsigned)kind) l->p[out++] = l->p[k];
    int gone = l->n - out;
    l->n = out;
    if (gone) wake(l);
    return gone;
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

/* The fastest a held solid may drive the liquid: this many times its own
 * fastest point, and never less than SQUEEZE_LEAST px/s.
 *
 * Not a law, and the one place here that is not: a limit on what a law
 * cannot do with a solid nothing pushes back on. Liquid between a solid and
 * a wall it is closing on has to leave through the gap, and as fast as the
 * gap is narrow — a window's corner sweeping the floor of a pool drove it at
 * 50000 px/s, the step ran at its ceiling for frames on end and the pool
 * sprayed like a burst pipe. A free solid is held off by the liquid it
 * squeezes, as a real one would be, and needs nothing of the kind; a held
 * one — carried by the hand, which no water stops — still does, and while
 * one stands in the liquid it gives, here, at a speed nothing else on a
 * desktop reaches, and is a little fuller there for a moment. A free splash
 * never comes near it: water that fell the height of the screen splashes at
 * a few thousand px/s. */
#define SQUEEZE_TIMES 3.0f
#define SQUEEZE_LEAST 6000.0f

struct body {
    unsigned id;
    bool free;
    float hw, hh, reach;       /* half its size, and half its diagonal */
    float cx0, cy0, a0;        /* held: its centre and turn at the start of the step */
    float vx, vy, spin;        /* px/s and rad/s: held, over the step; free, now */
    float px, py, a, c, s;     /* now: its centre, and its turn, also as cos, sin */
    float minv, iinv;          /* free: one over its mass, and its moment (0: does not turn) */
    float ax, ay;              /* free: what else pulls on it, px/s² */
    double gvx, gvy, gspin;    /* free: what the liquid has given it this step */
    double wx, wy, ww;         /* free: during a solve, M^-1 J x */
};

/* An edge where a solid meets a cell that is not solid. */
struct sface {
    int f;                     /* the edge, in u (axis 0) or v (axis 1) */
    int cell;                  /* the cell on the liquid's side */
    int body;                  /* the solid's index in bodies */
    unsigned char axis;
    unsigned char had;         /* the liquid had a velocity there */
    unsigned char let_go;      /* release_solids opened it */
    float out;                 /* +1 or -1: which way along the axis is away from the solid */
    float x, y;                /* its middle */
    float solid, own;          /* the solid's velocity across it, and the liquid's */
};

/* A face a free solid shares with a liquid cell, and what a unit of pressure
 * in that cell does to the solid through it: pushes it (gx, gy) and turns it
 * gw about its centre, per unit of the face's length. */
struct couple {
    int cell, body;
    float gx, gy, gw;
};

struct push {
    unsigned id;
    double ax, ay, alpha;
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
        struct push *d = realloc(l->push, (size_t)n * sizeof(*d));
        if (!d) return;
        l->push = d;
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

/* The step's solids: where each starts, and how fast it goes — a held one
 * to where it is to be, a free one as fast as it is going. And the fastest of
 * them near the liquid, which the step has to keep up with as it does with
 * the liquid's own fastest. */
static void bodies_start(FwmLiquid *l, double dt) {
    l->nbodies = 0;
    l->smax = l->hmax = 0.0;
    l->held = false;
    for (int k = 0; k < l->nsol_next; k++) {
        const LiquidSolid *now = &l->sol_next[k];
        const LiquidSolid *was = solid_with(l->sol_prev, l->nsol_prev, now->id);
        struct body *b = &l->bodies[l->nbodies++];
        double cx = now->x + now->w / 2, cy = now->y + now->h / 2;
        memset(b, 0, sizeof(*b));
        b->id = now->id;
        b->hw = (float)(now->w / 2);
        b->hh = (float)(now->h / 2);
        b->reach = (float)reach_of(now);
        b->cx0 = b->px = (float)cx;
        b->cy0 = b->py = (float)cy;
        b->a0 = b->a = (float)now->angle;
        b->c = cosf(b->a);
        b->s = sinf(b->a);
        double mass = now->density * now->w * now->h;
        if (mass > 0.0 && isfinite(mass)) {
            b->free = true;
            b->minv = (float)(1.0 / mass);
            b->iinv = now->turns ? (float)(12.0 / (mass * (now->w * now->w + now->h * now->h))) : 0.0f;
            b->vx = (float)now->vx;
            b->vy = (float)now->vy;
            b->spin = now->turns ? (float)now->spin : 0.0f;
            b->ax = (float)now->ax;
            b->ay = (float)now->ay;
            double fastest = hypot(now->vx, now->vy) + fabs(b->spin) * b->reach;
            if (fastest > l->smax && near_liquid(l, cx, cy, cx, cy, b->reach))
                l->smax = fastest;
            continue;
        }
        l->held = true;
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
                if (fastest > l->hmax && near_liquid(l, wx, wy, cx, cy, b->reach))
                    l->hmax = fastest;
            }
        }
    }
}

/* A free solid that has reached a wall stops going into it: the walls are
 * solid to it as they are to the liquid. */
static void keep_in_box(const FwmLiquid *l, struct body *b) {
    float ex = fabsf(b->c) * b->hw + fabsf(b->s) * b->hh;
    float ey = fabsf(b->s) * b->hw + fabsf(b->c) * b->hh;
    float w = (float)l->w, h = (float)l->h;
    if (2.0f * ex < w) {
        if (b->px - ex < 0.0f) { b->px = ex;     if (b->vx < 0.0f) b->vx = 0.0f; }
        if (b->px + ex > w)    { b->px = w - ex; if (b->vx > 0.0f) b->vx = 0.0f; }
    }
    if (2.0f * ey < h) {
        if (b->py - ey < 0.0f) { b->py = ey;     if (b->vy < 0.0f) b->vy = 0.0f; }
        if (b->py + ey > h)    { b->py = h - ey; if (b->vy > 0.0f) b->vy = 0.0f; }
    }
}

/* Every solid for the next `h` seconds, `t` seconds into the step: a held
 * one where its path puts it at the end of them, a free one where it stands,
 * going as fast as what else pulls on it makes it go by then. The liquid's
 * part of a free one's speed comes when the pressure is solved for
 * (couple_finish), and it moves on that after (bodies_move) — moved first,
 * on a speed the liquid had not had its say in, a window at rest on a pool
 * sank into it a little every step and was put back the next, and the pool
 * never stopped trembling. */
static void bodies_at(FwmLiquid *l, double t, double h) {
    for (int k = 0; k < l->nbodies; k++) {
        struct body *b = &l->bodies[k];
        if (b->free) {
            b->vx += b->ax * (float)h;
            b->vy += b->ay * (float)h;
            continue;
        }
        b->px = b->cx0 + (float)(b->vx * t);
        b->py = b->cy0 + (float)(b->vy * t);
        b->a = b->a0 + (float)(b->spin * t);
        b->c = cosf(b->a);
        b->s = sinf(b->a);
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

/* Out of every solid it stands in: onto that solid's nearest face that is
 * inside the walls, and no longer moving into it faster than the face is
 * moving away. False when no face of a solid it is in has room on the near
 * side of the walls — a window's corner has met the wall with this particle
 * between them, and there is nowhere left for it to be. */
static bool push_out(const FwmLiquid *l, Particle *p) {
    const float dx0 = l->dx;
    const float x0 = l->ox + 0.1f * dx0, x1 = l->ox + l->nx * dx0 - 0.1f * dx0;
    const float y0 = l->oy + 0.1f * dx0, y1 = l->oy + l->ny * dx0 - 0.1f * dx0;
    for (int k = 0; k < l->nbodies; k++) {
        const struct body *b = &l->bodies[k];
        float dx = p->x - b->px, dy = p->y - b->py;
        if (fabsf(dx) >= b->reach || fabsf(dy) >= b->reach) continue;
        float lx = b->c * dx + b->s * dy, ly = -b->s * dx + b->c * dy;
        if (b->hw - fabsf(lx) <= 0.0f || b->hh - fabsf(ly) <= 0.0f) continue;

        /* The four faces, nearest first. */
        const float skin = SKIN * dx0;
        struct { float d, nx, ny; } face[4] = {
            { b->hw - lx,  1.0f,  0.0f }, { b->hw + lx, -1.0f,  0.0f },
            { b->hh - ly,  0.0f,  1.0f }, { b->hh + ly,  0.0f, -1.0f },
        };
        for (int i = 1; i < 4; i++)
            for (int j = i; j > 0 && face[j].d < face[j - 1].d; j--) {
                __typeof__(face[0]) t = face[j];
                face[j] = face[j - 1];
                face[j - 1] = t;
            }
        bool placed = false;
        for (int i = 0; i < 4 && !placed; i++) {
            float nlx = face[i].nx ? face[i].nx * (b->hw + skin) : lx;
            float nly = face[i].ny ? face[i].ny * (b->hh + skin) : ly;
            float wx = b->px + b->c * nlx - b->s * nly;
            float wy = b->py + b->s * nlx + b->c * nly;
            if (wx < x0 || wx > x1 || wy < y0 || wy > y1) continue;
            p->x = wx;
            p->y = wy;
            float wnx = b->c * face[i].nx - b->s * face[i].ny;
            float wny = b->s * face[i].nx + b->c * face[i].ny;
            float svx, svy;
            body_velocity(b, wx, wy, &svx, &svy);
            float into = (p->vx - svx) * wnx + (p->vy - svy) * wny;
            if (into < 0.0f) {
                p->vx -= into * wnx;
                p->vy -= into * wny;
            }
            placed = true;
        }
        if (!placed) return false;
    }
    return true;
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
    int body = (sa ? sa : sb) - 1;
    float svx, svy;
    body_velocity(&l->bodies[body], x, y, &svx, &svy);
    struct sface e = {
        .f = (int)f, .cell = (int)(sa ? b : a), .body = body, .axis = (unsigned char)axis,
        .had = ok[f], .out = sa ? 1.0f : -1.0f, .x = x, .y = y,
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
            l->sfaces[k].let_go = 1;
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

/* The free solids on, `h` seconds, at the speed the liquid left them. */
static void bodies_move(FwmLiquid *l, float h) {
    for (int k = 0; k < l->nbodies; k++) {
        struct body *b = &l->bodies[k];
        if (!b->free) continue;
        b->px += b->vx * h;
        b->py += b->vy * h;
        b->a += b->spin * h;
        b->c = cosf(b->a);
        b->s = sinf(b->a);
        keep_in_box(l, b);
    }
}

/* ── free solids ─────────────────────────────────────────────────────────
 *
 * A free solid is pushed by the pressure on its faces and pushes back on the
 * liquid with its own motion, and both at once: the solid's velocity is an
 * unknown in the pressure solve like the pressure itself (Batty, Bertails &
 * Bridson 2007). Through a face it shares with a liquid cell, a pressure p in
 * the cell pushes the solid with p times the face's length along G =
 * (d, r x d) — d the way from the cell into the solid along the face's axis,
 * r the face's place about the solid's centre — so over a step the solid's
 * velocity becomes V* + M^-1 J p h, J the sum of those over its faces and V*
 * where it was going anyway. And what flows out of the cell through that
 * face is the solid's velocity there, G . V. Put the one into the other and
 * each cell's equation gains dx² G^T M^-1 J p: the matrix is A + dx² J^T
 * M^-1 J, still symmetric and positive definite, and the conjugate gradients
 * solve it as they did A — with the MIC(0) of A alone as the preconditioner,
 * which the solid's part only slows down a little.
 *
 * Solved together, a body lighter than the liquid it moves through is fine;
 * solved apart — the pressure read off afterwards and the push handed to the
 * body for the next step — it is not, because pushing liquid aside makes it
 * heavier than it is (the liquid it shoves has to be shoved too), and a body
 * pushed by that from the last step is pushed back past where it started,
 * further each step. */

static bool add_couple(FwmLiquid *l, const struct couple *c) {
    if (l->ncpl == l->cpl_cap) {
        int cap = l->cpl_cap ? 2 * l->cpl_cap : 256;
        struct couple *a = realloc(l->cpl, (size_t)cap * sizeof(*a));
        if (!a) return false;
        l->cpl = a;
        l->cpl_cap = cap;
    }
    l->cpl[l->ncpl++] = *c;
    return true;
}

/* The faces free solids share with liquid cells, as they stand: not the ones
 * let go of, which have air behind them. */
static void couple_build(FwmLiquid *l) {
    l->ncpl = 0;
    for (int k = 0; k < l->nsfaces; k++) {
        const struct sface *e = &l->sfaces[k];
        const struct body *b = &l->bodies[e->body];
        if (!b->free || e->let_go || l->type[e->cell] != FLUID) continue;
        float d = -e->out;
        float rx = e->x - b->px, ry = e->y - b->py;
        struct couple c = { .cell = e->cell, .body = e->body };
        if (e->axis == 0) { c.gx = d; c.gy = 0.0f; c.gw = -d * ry; }
        else              { c.gx = 0.0f; c.gy = d; c.gw = d * rx; }
        if (!add_couple(l, &c)) return;
    }
}

/* M^-1 J x for every free solid, into its (wx, wy, ww). */
static void couple_reduce(FwmLiquid *l, const float *x) {
    for (int k = 0; k < l->nbodies; k++) {
        struct body *b = &l->bodies[k];
        b->wx = b->wy = b->ww = 0.0;
    }
    for (int k = 0; k < l->ncpl; k++) {
        const struct couple *c = &l->cpl[k];
        struct body *b = &l->bodies[c->body];
        double v = x[c->cell];
        b->wx += c->gx * v;
        b->wy += c->gy * v;
        b->ww += c->gw * v;
    }
    for (int k = 0; k < l->nbodies; k++) {
        struct body *b = &l->bodies[k];
        b->wx *= b->minv;
        b->wy *= b->minv;
        b->ww *= b->iinv;
    }
}

/* The pressure solved: each free solid in the liquid takes what it gave it,
 * and its faces carry its new velocity, which is the one the liquid beside
 * them was solved to flow with. */
static void couple_finish(FwmLiquid *l) {
    if (l->ncpl == 0) return;
    couple_reduce(l, l->pres);
    const double dx = l->dx;
    for (int k = 0; k < l->nbodies; k++) {
        struct body *b = &l->bodies[k];
        if (!b->free) continue;
        double dvx = dx * b->wx, dvy = dx * b->wy, dw = dx * b->ww;
        b->vx += (float)dvx;
        b->vy += (float)dvy;
        b->spin += (float)dw;
        b->gvx += dvx;
        b->gvy += dvy;
        b->gspin += dw;
        double fastest = hypot(b->vx, b->vy) + fabs(b->spin) * b->reach;
        if (fastest > l->smax && near_liquid(l, b->px, b->py, b->px, b->py, b->reach))
            l->smax = fastest;
    }
    for (int k = 0; k < l->nsfaces; k++) {
        const struct sface *e = &l->sfaces[k];
        const struct body *b = &l->bodies[e->body];
        if (!b->free || e->let_go) continue;
        float svx, svy;
        body_velocity(b, e->x, e->y, &svx, &svy);
        (e->axis ? l->v : l->u)[e->f] = e->axis ? svy : svx;
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
            /* Across an edge, one over its density: the two cells' between
             * liquid and liquid, the liquid's own against air, and nothing
             * against a wall. */
            const float *rho = l->rho;
            float rc = rho[c];
            l->wr[c] = t[c + 1] == FLUID ? 2.0f / (rc + rho[c + 1]) : 0.0f;
            l->wd[c] = t[c + cw] == FLUID ? 2.0f / (rc + rho[c + cw]) : 0.0f;
            float d = 0.0f;
            const size_t around[4] = { c - 1, c + 1, c - cw, c + cw };
            for (int a = 0; a < 4; a++) {
                size_t q = around[a];
                if (t[q] == FLUID)    d += 2.0f / (rc + rho[q]);
                else if (t[q] == AIR) d += 1.0f / rc;
            }
            l->pdiag[c] = d;
            n++;
        }
}

static void strip_preconditioner(FwmLiquid *l, int k0, int k1) {
    const float tau = 0.97f, sigma = 0.25f;
    const int cw = l->cw;
    float *pc = l->prec;
    const float *wr = l->wr, *wd = l->wd;
    for (int k = k0; k < k1; k++) {
        int c = l->fluid[k];
        unsigned b = l->nbits[k];
        float diag = l->pdiag[c];
        float e = diag;
        if (b & NB_LS) {
            float a = wr[c - 1] * pc[c - 1];
            e -= a * a;
            if (b & NB_LD) e -= tau * wr[c - 1] * wd[c - 1] * pc[c - 1] * pc[c - 1];
        }
        if (b & NB_U) {
            float a = wd[c - cw] * pc[c - cw];
            e -= a * a;
            if (b & NB_UR) e -= tau * wd[c - cw] * wr[c - cw] * pc[c - cw] * pc[c - cw];
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
    const float *wr = l->wr, *wd = l->wd;
    double dot = 0.0;
    for (int k = k0; k < k1; k++) {
        int c = l->fluid[k];
        unsigned b = l->nbits[k];
        float s = 0.0f;
        if (b & NB_L) s += wr[c - 1] * x[c - 1];
        if (b & NB_R) s += wr[c] * x[c + 1];
        if (b & NB_U) s += wd[c - cw] * x[c - cw];
        if (b & NB_D) s += wd[c] * x[c + cw];
        float v = l->pdiag[c] * x[c] - s;
        y[c] = v;
        dot += (double)v * (double)x[c];
    }
    return dot;
}

/* y = (A + dx² J^T M^-1 J) x over the strip, and the dot of y with x for it:
 * the liquid's part, and the free solids' (see "free solids"). Every member
 * calls it at once, with x final everywhere. */
static double strip_apply(FwmLiquid *l, int part, int k0, int k1, const float *x, float *y) {
    double dot = strip_multiply(l, k0, k1, x, y);
    if (l->ncpl == 0) return dot;
    if (part == 0) couple_reduce(l, x);
    crew_sync(l->crew);
    const int i0 = l->strip_col[part], i1 = l->strip_col[part + 1], cw = l->cw;
    const float dx2 = l->dx * l->dx;
    for (int k = 0; k < l->ncpl; k++) {
        const struct couple *c = &l->cpl[k];
        int i = c->cell % cw - PAD;
        if (i < i0 || i >= i1) continue;
        const struct body *b = &l->bodies[c->body];
        float t = dx2 * (float)(c->gx * b->wx + c->gy * b->wy + c->gw * b->ww);
        y[c->cell] += t;
        dot += (double)t * (double)x[c->cell];
    }
    return dot;
}

/* z = M^-1 r over the strip, and the dot of z with r for it. */
static double strip_precondition(FwmLiquid *l, int k0, int k1, const float *r, float *z) {
    const int cw = l->cw;
    const float *pc = l->prec, *wr = l->wr, *wd = l->wd;
    float *q = l->q;
    for (int k = k0; k < k1; k++) {
        int c = l->fluid[k];
        unsigned b = l->nbits[k];
        float s = r[c];
        if (b & NB_LS) s += wr[c - 1] * pc[c - 1] * q[c - 1];
        if (b & NB_U)  s += wd[c - cw] * pc[c - cw] * q[c - cw];
        q[c] = s * pc[c];
    }
    double dot = 0.0;
    for (int k = k1 - 1; k >= k0; k--) {
        int c = l->fluid[k];
        unsigned b = l->nbits[k];
        float s = q[c];
        if (b & NB_RS) s += wr[c] * pc[c] * z[c + 1];
        if (b & NB_D)  s += wd[c] * pc[c] * z[c + cw];
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

/* Liquid with no air anywhere around it: every cell reachable from one
 * beside air marked, and what is left gathered into pockets. Only a solid
 * can shut liquid in — the box alone never fills to the brim — so with none
 * there is nothing to look for. */
static void find_pockets(FwmLiquid *l) {
    l->npockets = 0;
    if (l->nbodies == 0) return;
    const int cw = l->cw;
    const unsigned char *t = l->type;
    unsigned char *seen = l->okb;   /* free until release_solids */
    int *stack = l->flood, top = 0;
    memset(seen, 0, l->cn);
    for (int j = 0; j < l->ny; j++)
        for (int i = 0; i < l->nx; i++) {
            size_t c = CIX(l, i, j);
            if (t[c] != FLUID) continue;
            if (t[c - 1] == AIR || t[c + 1] == AIR || t[c - cw] == AIR || t[c + cw] == AIR) {
                seen[c] = 1;
                stack[top++] = (int)c;
            }
        }
    while (top > 0) {
        int c = stack[--top];
        const int around[4] = { c - 1, c + 1, c - cw, c + cw };
        for (int k = 0; k < 4; k++)
            if (!seen[around[k]] && t[around[k]] == FLUID) {
                seen[around[k]] = 1;
                stack[top++] = around[k];
            }
    }

    int n = 0;
    for (int j = 0; j < l->ny; j++)
        for (int i = 0; i < l->nx; i++) {
            size_t c0 = CIX(l, i, j);
            if (seen[c0] || t[c0] != FLUID) continue;
            if (l->npockets + 1 >= l->pocket_cap) {
                int cap = l->pocket_cap ? 2 * l->pocket_cap : 16;
                int *a = realloc(l->pocket_at, (size_t)cap * sizeof(*a));
                if (!a) return;
                l->pocket_at = a;
                l->pocket_cap = cap;
            }
            l->pocket_at[l->npockets++] = n;
            seen[c0] = 1;
            stack[top++] = (int)c0;
            while (top > 0) {
                int c = stack[--top];
                l->pocket[n++] = c;
                const int around[4] = { c - 1, c + 1, c - cw, c + cw };
                for (int k = 0; k < 4; k++)
                    if (!seen[around[k]] && t[around[k]] == FLUID) {
                        seen[around[k]] = 1;
                        stack[top++] = around[k];
                    }
            }
        }
    if (l->npockets) l->pocket_at[l->npockets] = n;
}

/* A pocket shut in by a solid that is moving can have more coming into it
 * than going out, and nothing can be squeezed: for that there is no
 * pressure at all, and the solve, asked for one, answered in millions of
 * px/s. So what a pocket cannot get rid of it keeps — the part of its
 * inflow every cell of it shares is taken out of the equations, and the
 * pocket is a little fuller until the solid lets it out again, which the
 * drift correction then evens out.
 *
 * Unless a free solid shuts it in: then the pocket has a pressure, the one
 * that pushes that solid back, and is left as it is. */
static void open_pockets(FwmLiquid *l) {
    int *tag = l->flood;   /* free between find_pockets and release_solids */
    for (int k = 0; k < l->ncpl; k++) tag[l->cpl[k].cell] = 0;
    for (int k = 0; k < l->npockets; k++)
        for (int q = l->pocket_at[k]; q < l->pocket_at[k + 1]; q++) tag[l->pocket[q]] = k + 1;
    for (int k = 0; k < l->npockets; k++) {
        int a = l->pocket_at[k], b = l->pocket_at[k + 1];
        double gx = 0.0, gy = 0.0;
        for (int q = 0; q < l->ncpl; q++)
            if (tag[l->cpl[q].cell] == k + 1) {
                gx += l->cpl[q].gx;
                gy += l->cpl[q].gy;
            }
        if (fabs(gx) + fabs(gy) >= 1.0) continue;
        double sum = 0.0;
        for (int q = a; q < b; q++) sum += l->rhs[l->pocket[q]];
        float mean = (float)(sum / (b - a));
        for (int q = a; q < b; q++) l->rhs[l->pocket[q]] -= mean;
    }
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
    if (l->npockets) {
        if (part == 0) open_pockets(l);
        crew_sync(l->crew);
    }

    float scale = (float)largest(l, 0);
    if (scale < 1e-9f) {
        for (int k = k0; k < k1; k++) x[l->fluid[k]] = 0.0f;
        if (part == 0) l->iters = 0;
        return;
    }
    const float tol = SOLVE_TOL * scale;

    strip_apply(l, part, k0, k1, x, z);
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
        mine[3] = strip_apply(l, part, k0, k1, s, z);
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
    if (lists) {
        cut_strips(l);
        find_pockets(l);
    }
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
     * neighbour it is evening out with. With several liquids, where a thick
     * heavy one drags on a thin light one across the edge between them it
     * moves the light one as if it were as thick as the heavy one's weight
     * makes it: the worst of those. */
    double mu = 0.0, lightest = INFINITY;
    for (int k = 0; k < LIQUID_MAX_KINDS; k++) {
        if (!(l->present & (1u << k))) continue;
        mu = fmax(mu, l->kind[k].viscosity * l->kind[k].density);
        lightest = fmin(lightest, l->kind[k].density);
    }
    if (mu > 0.0) {
        double hv = 0.2 * dx * dx * lightest / mu;
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
    float *u, *um, *v, *vm, *mass, *heavy, *thick, *kvol;
    unsigned char *occ;
};

static void gather(const FwmLiquid *l, int lo, int hi, float h, const struct grid_view *g) {
    const float x0 = l->ox + 0.1f * l->dx, x1 = l->ox + l->nx * l->dx - 0.1f * l->dx;
    const float y0 = l->oy + 0.1f * l->dx, y1 = l->oy + l->ny * l->dx - 0.1f * l->dx;
    const float dx = l->dx, idx = l->inv_dx, area = l->pmass;
    /* Per liquid: its density, and -dt * volume * D^-1 * its viscosity, with
     * D^-1 = 4/dx^2 for the quadratic kernel. */
    float rho_of[LIQUID_MAX_KINDS], ks_of[LIQUID_MAX_KINDS], nu_of[LIQUID_MAX_KINDS];
    for (int q = 0; q < LIQUID_MAX_KINDS; q++) {
        rho_of[q] = (float)l->kind[q].density;
        nu_of[q] = (float)l->kind[q].viscosity;
        ks_of[q] = -h * area * rho_of[q] * 4.0f * idx * idx * (float)l->kind[q].viscosity;
    }
    const int nx = l->nx, ny = l->ny;
    for (int k = lo; k < hi; k++) {
        Particle *p = &l->p[k];
        /* Momentum goes to the grid by weight, so that a velocity there is
         * what the liquids crossing it carry between them, the heavier one
         * the more; the stress is the viscosity's times the density. */
        unsigned kd = p->kind < LIQUID_MAX_KINDS ? p->kind : 0;
        const float m = area * rho_of[kd], ks = ks_of[kd];
        /* Whatever a solid moved over this substep, it is off it now. */
        if (l->nbodies) {
            /* One with nowhere left to be is let go of after the move. */
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
            for (int i = 0; i < 3; i++) {
                g->mass[row + i] += wx[i] * wy[j] * area;
                g->heavy[row + i] += wx[i] * wy[j] * m;
            }
        }
        if (l->mixed) {
            float *mine = g->kvol + (size_t)kd * l->cn;
            const float mnu = m * nu_of[kd];
            for (int j = 0; j < 3; j++) {
                size_t row = CIX(l, ix, iy + j);
                for (int i = 0; i < 3; i++) {
                    float w = wx[i] * wy[j];
                    g->thick[row + i] += w * mnu;
                    mine[row + i] += w * area;
                }
            }
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
        g = (struct grid_view){ l->u, l->um, l->v, l->vm, l->mass, l->heavy, l->thick,
                                l->kvol, NULL };
    } else {
        float *base = l->pgrid + (size_t)(part - 1) * l->pstride;
        g = (struct grid_view){ base, base + un, base + 2 * un, base + 2 * un + vn,
                                base + 2 * un + 2 * vn, base + 2 * un + 2 * vn + cn,
                                base + 2 * un + 2 * vn + 2 * cn,
                                base + 2 * un + 2 * vn + 3 * cn, NULL };
    }
    g.occ = l->pocc + (size_t)part * cn;
    memset(g.u, 0, un * sizeof(float));
    memset(g.um, 0, un * sizeof(float));
    memset(g.v, 0, vn * sizeof(float));
    memset(g.vm, 0, vn * sizeof(float));
    memset(g.mass, 0, cn * sizeof(float));
    memset(g.heavy, 0, cn * sizeof(float));
    if (l->mixed) {
        memset(g.thick, 0, cn * sizeof(float));
        for (int k = 0; k < LIQUID_MAX_KINDS; k++)
            if (l->present & (1u << k)) memset(g.kvol + (size_t)k * cn, 0, cn * sizeof(float));
    }
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
        float mass = l->mass[c], heavy = l->heavy[c];
        unsigned char occ = l->pocc[c];
        for (int q = 1; q < parts; q++) {
            const float *o = other + (size_t)(q - 1) * ps + 2 * un + 2 * vn;
            mass += o[c];
            heavy += o[cn + c];
            occ |= l->pocc[(size_t)q * cn + c];
        }
        if (l->mixed) {
            float thick = l->thick[c];
            for (int q = 1; q < parts; q++)
                thick += other[(size_t)(q - 1) * ps + 2 * un + 2 * vn + 2 * cn + c];
            l->thick[c] = thick;
            for (int k = 0; k < LIQUID_MAX_KINDS; k++) {
                if (!(l->present & (1u << k))) continue;
                float vol = l->kvol[(size_t)k * cn + c];
                for (int q = 1; q < parts; q++)
                    vol += other[(size_t)(q - 1) * ps + 2 * un + 2 * vn + (3 + (size_t)k) * cn + c];
                l->kvol[(size_t)k * cn + c] = vol;
            }
        }
        l->mass[c] = mass;
        l->heavy[c] = heavy;
        l->rho[c] = mass > 0.0f ? heavy / mass : 1.0f;
        int i = (int)(c % (size_t)l->cw) - PAD, j = (int)(c / (size_t)l->cw) - PAD;
        if (i < 0 || i >= l->nx || j < 0 || j >= l->ny) l->type[c] = SOLID;
        else l->type[c] = (occ || mass >= full) ? FLUID : AIR;
    }
}

/* How much of each cell the solids stand on, sampled 4 x 4 in the cells
 * along their edges, for the tension's fullness: a cell a window covers half
 * of holds half as many particles and is full. */
static void solid_cover(FwmLiquid *l) {
    memset(l->cover, 0, l->cn * sizeof(float));
    if (l->nbodies == 0) return;
    const int nx = l->nx, ny = l->ny;
    const float dx = l->dx, idx = l->inv_dx;
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
                int in = 0;
                for (int sj = 0; sj < 4; sj++)
                    for (int si = 0; si < 4; si++) {
                        float x = l->ox + (i + (si + 0.5f) * 0.25f) * dx - b->px;
                        float y = l->oy + (j + (sj + 0.5f) * 0.25f) * dx - b->py;
                        float lx = b->c * x + b->s * y, ly = -b->s * x + b->c * y;
                        in += fabsf(lx) < b->hw && fabsf(ly) < b->hh;
                    }
                float *c = &l->cover[CIX(l, i, j)];
                *c = fminf(*c + in / 16.0f, 1.0f);
            }
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
     * it. A solid the same: full is what the solid leaves of a cell, and a
     * cell it all but covers reads as the cells around it. Counted against
     * the whole cell, the sliver a window's edge leaves between itself and
     * the next cell was a surface in the middle of the pool, pulled on for
     * ever, and a pool with a window in it never settled. */
    const float *cover = l->cover;
    crew_share(ny, part, parts, &lo, &hi);
    for (int j = lo; j < hi; j++)
        for (int i = 0; i < nx; i++) {
            size_t q = CIX(l, i, j);
            float room = 1.0f - cover[q];
            /* -1: covered, to be read off the cells around it. */
            float r = room > 0.25f ? l->mass[q] / (fill * room) : -1.0f;
            c[q] = r > 1.0f ? 1.0f : r;
        }
    crew_sync(l->crew);
    if (l->nbodies) {
        for (int j = lo; j < hi; j++)
            for (int i = 0; i < nx; i++) {
                size_t q = CIX(l, i, j);
                if (c[q] >= 0.0f) continue;
                float sum = 0.0f;
                int n = 0;
                if (i > 0      && c[q - 1] >= 0.0f)  { sum += c[q - 1];  n++; }
                if (i < nx - 1 && c[q + 1] >= 0.0f)  { sum += c[q + 1];  n++; }
                if (j > 0      && c[q - cw] >= 0.0f) { sum += c[q - cw]; n++; }
                if (j < ny - 1 && c[q + cw] >= 0.0f) { sum += c[q + cw]; n++; }
                /* Into scratch, and in only once every member has read
                 * its neighbours. */
                l->txy[q] = n ? sum / (float)n : 0.0f;
            }
        crew_sync(l->crew);
        for (int j = lo; j < hi; j++)
            for (int i = 0; i < nx; i++) {
                size_t q = CIX(l, i, j);
                if (c[q] < 0.0f) c[q] = l->txy[q];
            }
        crew_sync(l->crew);
    }
    crew_share(ny + 2, part, parts, &lo, &hi);
    for (int j = lo - 1; j < hi - 1; j++) {
        int sj = j < 0 ? 0 : j >= ny ? ny - 1 : j;
        for (int i = -1; i <= nx; i++) {
            if (i >= 0 && i < nx && j >= 0 && j < ny) continue;
            int si = i < 0 ? 0 : i >= nx ? nx - 1 : i;
            c[CIX(l, i, j)] = c[CIX(l, si, sj)];
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

/* One over the density across the edge between cells a and b, at least one
 * of them liquid — the solve's own weight there. */
static inline float edge_weight(const FwmLiquid *l, size_t a, size_t b) {
    bool fa = l->type[a] == FLUID, fb = l->type[b] == FLUID;
    if (fa && fb) return 2.0f / (l->rho[a] + l->rho[b]);
    return 1.0f / l->rho[fa ? a : b];
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
            l->u[f] -= (l->pres[b] - l->pres[a]) * idx * edge_weight(l, a, b);
            l->uok[f] = 1;
        }
    for (int j = lo > 1 ? lo : 1; j < hi; j++)
        for (int i = 0; i < nx; i++) {
            size_t a = CIX(l, i, j - 1), b = CIX(l, i, j);
            if (l->type[a] != FLUID && l->type[b] != FLUID) continue;
            if (l->sid[a] || l->sid[b]) continue;
            size_t f = VIX(l, i, j);
            l->v[f] -= (l->pres[b] - l->pres[a]) * idx * edge_weight(l, a, b);
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
    const float squeeze = l->held ? fmaxf(SQUEEZE_LEAST, SQUEEZE_TIMES * (float)l->hmax) : 0.0f;
    /* Drift (see DRIFT_STOKES): only with more than one liquid in the box
     * and something pulling. */
    const bool mixed = l->mixed && (l->gx != 0.0 || l->gy != 0.0);
    const float d2 = dx * dx, area = l->pmass;
    const float slip_most = 0.5f * dx / h;
    const size_t cn = l->cn;
    float rho_of[LIQUID_MAX_KINDS], nu_of[LIQUID_MAX_KINDS];
    for (int q = 0; q < LIQUID_MAX_KINDS; q++) {
        rho_of[q] = (float)l->kind[q].density;
        nu_of[q] = (float)l->kind[q].viscosity;
    }
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
        float drive = vx * vx + vy * vy;
        if (squeeze > 0.0f && drive > squeeze * squeeze) {
            float k = squeeze / sqrtf(drive);
            vx *= k;
            vy *= k;
        }
        p->vx = vx;  p->vy = vy;
        p->c00 = c00 * dinv;  p->c01 = c01 * dinv;
        p->c10 = c10 * dinv;  p->c11 = c11 * dinv;

        p->x += h * vx;
        p->y += h * vy;
        if (mixed) {
            int ci = (int)gx, cj = (int)gy;
            if (ci >= 0 && ci < nx && cj >= 0 && cj < ny) {
                size_t c = CIX(l, ci, cj);
                unsigned kd = p->kind < LIQUID_MAX_KINDS ? p->kind : 0;
                /* What is around it: the cell, less its own liquid. */
                float own = l->kvol[(size_t)kd * cn + c];
                float around = l->mass[c] - own;
                float rk = rho_of[kd];
                float ro = around > 0.02f * area ? (l->heavy[c] - rk * own) / around : rk;
                float dr = rk - ro;
                if (l->type[c] == FLUID && fabsf(dr) > 1e-4f) {
                    float mu = (l->thick[c] - rk * nu_of[kd] * own) / around;
                    mu = fmaxf(mu, DRIFT_NU_LEAST * ro);
                    float k = dr * d2 / (18.0f * mu);
                    float sx = k * (float)l->gx, sy = k * (float)l->gy;
                    float s = sqrtf(sx * sx + sy * sy);
                    if (s > slip_most) { sx *= slip_most / s; sy *= slip_most / s; }
                    p->x += h * sx;
                    p->y += h * sy;
                }
            }
        }
        /* The grid stops liquid at a solid's face the way it does at a wall,
         * and a substep's move can still carry a particle a hair into it.
         * Caught between a solid and a wall with no room left, it is
         * squeezed out of the liquid altogether — what no longer fits runs
         * over the top, here as anywhere. */
        if (l->nbodies && !push_out(l, p)) {
            p->x = NAN;
            bad++;
            continue;
        }
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
    if (l->mat.tension > 0.0) {
        solid_cover(l);
        crew_run(l->crew, job_tension, &job);
    }

    close_walls(l);
    close_solids(l);
    couple_build(l);

    /* 3. The pressure, starting from last step's: a pool at rest needs the
     * same pressure every step, and from there the solve has almost nothing
     * left to do. It is pressure times h, hence the rescale. A wall that lets
     * go changes what flows, not which cells are liquid, so the second solve
     * keeps the strips and their preconditioners. */
    float rescale = l->last_h > 0.0 ? (float)(h / l->last_h) : 0.0f;
    for (size_t c = 0; c < l->cn; c++)
        l->pres[c] = l->type[c] == FLUID ? l->pres[c] * rescale : 0.0f;
    solve_pressure(l, h, true);
    bool walls = release_walls(l);
    bool solids = release_solids(l);
    if (solids) couple_build(l);
    if (walls || solids) solve_pressure(l, h, false);
    l->last_h = h;
    couple_finish(l);
    crew_run(l->crew, job_finish, &job);

    /* 4. Back to the particles, and the free solids on with them. */
    crew_run(l->crew, job_to_particles, &job);
    bodies_move(l, h);
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
     * the liquid. The ones squeezed out between a solid and a wall leave the
     * same way. */
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

/* The pressure it sleeps under: one solve with nothing moving but gravity,
 * and the free solids held where they stand. What holds up a window floating
 * on a sleeping pool (liquid_hold) is then its buoyancy where it went to
 * sleep — not the pressure of the last step, which still had the pool and
 * the window moving in it. */
static void rest_pressure(FwmLiquid *l) {
    if (l->nbodies == 0 || !(l->last_h > 0.0)) return;
    float h = (float)l->last_h;
    struct step_job job = { .l = l, .h = h };
    for (int k = 0; k < l->nbodies; k++) {
        struct body *b = &l->bodies[k];
        b->vx = b->vy = b->spin = 0.0f;
        b->wx = b->free;   /* kept through the solve */
        b->free = false;
    }
    crew_run(l->crew, job_to_grid, &job);
    if (l->mat.tension > 0.0) {
        solid_cover(l);
        crew_run(l->crew, job_tension, &job);
    }
    close_walls(l);
    close_solids(l);
    l->ncpl = 0;
    solve_pressure(l, h, true);
    for (int k = 0; k < l->nbodies; k++) {
        struct body *b = &l->bodies[k];
        b->free = b->wx != 0.0;
    }
    couple_build(l);
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
        liquid_hold(l);
        bodies_settle(l);
        return;
    }

    l->present = 0;
    for (int k = 0; k < l->n; k++) l->present |= 1u << (l->p[k].kind & 31u);
    l->mixed = (l->present & (l->present - 1)) != 0;
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
        bodies_at(l, t + h, h);
        substep(l, (float)h);
        t += h;
        l->substeps++;
    }
    /* What the liquid did to each free solid, spread over the time it took. */
    l->npush = 0;
    l->fmax = 0.0;
    for (int k = 0; k < l->nbodies; k++) {
        const struct body *b = &l->bodies[k];
        if (!b->free) continue;
        if (b->gvx != 0.0 || b->gvy != 0.0 || b->gspin != 0.0)
            l->fmax = fmax(l->fmax, hypot(b->vx, b->vy) + fabs(b->spin) * b->reach);
        l->push[l->npush++] = (struct push){
            .id = b->id, .ax = b->gvx / t, .ay = b->gvy / t, .alpha = b->gspin / t,
        };
    }
    bodies_settle(l);

    /* Calm, and nothing near it moving: a window pushed slowly through a
     * pool keeps it awake however gently the pool moves. And nothing in it
     * still bobbing: what holds a floating window up while the liquid sleeps
     * is the pressure it went to sleep with, which is only its weight if it
     * was at rest. */
    if (l->vrms < SLEEP_RMS && l->vmax < SLEEP_MAX && l->smax < SLEEP_MAX
        && l->fmax < SLEEP_BODY) {
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
            rest_pressure(l);
        }
    } else {
        l->calm_s = 0.0;
    }
}

/* ── what it does to the solids ──────────────────────────────────────── */

bool liquid_push(const FwmLiquid *l, unsigned id, double *ax, double *ay, double *alpha) {
    *ax = *ay = *alpha = 0.0;
    if (!l) return false;
    for (int k = 0; k < l->npush; k++)
        if (l->push[k].id == id) {
            *ax = l->push[k].ax;
            *ay = l->push[k].ay;
            *alpha = l->push[k].alpha;
            return true;
        }
    return false;
}

/* The pressure the last step left, on the faces it was left on, is what
 * holds a free solid up while nothing moves: at rest it is the weight of the
 * liquid above each face and nothing else. Only for a solid standing where it
 * stood — one that has moved is no longer on those faces, and gets nothing
 * until a step finds where it is. */
void liquid_hold(FwmLiquid *l) {
    if (!l) return;
    l->npush = 0;
    bool any = l->n > 0 && l->u && l->last_h > 0.0 && l->ncpl > 0;
    if (any) couple_reduce(l, l->pres);
    const double k = any ? l->dx / l->last_h : 0.0;
    for (int s = 0; s < l->nsol_next; s++) {
        const LiquidSolid *now = &l->sol_next[s];
        if (!(now->density > 0.0)) continue;
        const LiquidSolid *was = solid_with(l->sol_prev, l->nsol_prev, now->id);
        struct push p = { .id = now->id };
        for (int b = 0; any && b < l->nbodies; b++) {
            const struct body *bd = &l->bodies[b];
            if (bd->id != now->id || !bd->free) continue;
            if (was && !solid_moved(was, now))
                p = (struct push){ .id = now->id, .ax = k * bd->wx, .ay = k * bd->wy, .alpha = k * bd->ww };
            break;
        }
        l->push[l->npush++] = p;
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

const unsigned *liquid_kinds_of(const FwmLiquid *l, int *stride) {
    if (stride) *stride = (int)(sizeof(Particle) / sizeof(unsigned));
    return (l && l->p) ? &l->p[0].kind : NULL;
}
