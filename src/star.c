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

#include "star.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG (M_PI / 180.0)

/* The two masses that decide how this ends, in solar masses, as they are
 * written in the sky. Not config: a limit you can dial is not a limit, and
 * these are the whole reason the ending is worth watching. */
#define STAR_CHANDRASEKHAR 1.44 /* electrons hold below this: an ember */
#define STAR_TOV           2.50 /* neutrons hold below this: a pulsar */

/* Fraction of the burning radius each remnant settles at. A real neutron star
 * is twenty kilometres against a million, which on screen is one pixel and no
 * effect at all — these are the smallest sizes that still read as an object. */
/* Strictly decreasing, and it must STAY strictly decreasing: a remnant that is
 * larger than the thing it fell from turns a collapse into an expansion, and
 * since an expansion is forbidden the size simply holds still — a collapse
 * with no animation in it at all. That is what raising the hole to 0.30 did to
 * the pulsar, twice. */
#define STAR_R_DWARF   0.55
#define STAR_R_NEUTRON 0.30
/* A hole is drawn at 2.6 times this — the shadow is larger than the horizon —
 * and its disc reaches several times further again, so it is by far the
 * biggest thing here on screen while being the smallest by radius. 0.07 was
 * too small to aim a window at; this keeps the remnants in their proper order
 * (a hole IS smaller than the neutron star it fell from) while leaving a
 * shadow worth hitting. It grows from here with every window. */
#define STAR_R_HOLE    0.18

/* How much brighter the squeeze is allowed to get. Surface brightness runs
 * away as the radius falls (the area goes as r^2 while the temperature climbs
 * faster), so without a ceiling the last moment before the flash is a white
 * screen rather than a star. */
#define STAR_L_MAX 8.0

/* What the remnants shine at. Named because a collapse STARTS from one of
 * them: a pulsar falling into a hole has to begin at the brightness it
 * already had, or the first frame of the fall is a flash of a whole star. */
#define STAR_L_DWARF   0.14
#define STAR_L_NEUTRON 0.75

/* The last of the collapse, over which whatever light is left goes out. The
 * flash is the peak of the runaway, not a separate event. */
#define STAR_FLASH_AT 0.93

static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

static double lerp(double a, double b, double t) { return a + (b - a) * t; }

/* Where the collapse is, 0..1. */
static double collapse_t(const FwmStar *star, const StarConfig *cfg) {
    if (star->phase != STAR_COLLAPSE) return 0.0;
    double dur = cfg->collapse_s > 0.0 ? cfg->collapse_s : 1.0;
    return clamp01(star->phase_s / dur);
}

/* What this mass leaves behind. */
static FwmStarPhase remnant_of(double mass) {
    if (mass < STAR_CHANDRASEKHAR) return STAR_DWARF;
    if (mass < STAR_TOV)           return STAR_NEUTRON;
    return STAR_HOLE;
}

static double remnant_fraction(FwmStarPhase phase) {
    switch (phase) {
    case STAR_DWARF:   return STAR_R_DWARF;
    case STAR_NEUTRON: return STAR_R_NEUTRON;
    case STAR_HOLE:    return STAR_R_HOLE;
    default:           return 1.0;
    }
}

/* The radius a remnant of this mass settles at, in px.
 *
 * A hole is the one whose size depends on its mass, so it cannot be a plain
 * fraction like the others — and getting that wrong meant the collapse
 * converged on one number while the hole was drawn at another, so the last
 * frame of the fall jumped outward. */
static double remnant_radius_px(FwmStarPhase phase, double mass, const StarConfig *cfg) {
    if (phase == STAR_HOLE) {
        double m = mass > STAR_TOV ? mass : STAR_TOV;
        return cfg->radius * STAR_R_HOLE * (m / STAR_TOV);
    }
    return cfg->radius * remnant_fraction(phase);
}


void star_init(FwmStar *star, const StarConfig *cfg) {
    memset(star, 0, sizeof(*star));
    if (!cfg) return;
    star->wx    = cfg->x;
    star->wy    = cfg->y;
    star->mass  = cfg->mass;
    star->fuel  = 1.0;
    star->phase = STAR_BURNING;
    star->from_frac = 1.0;
    star->from_lum  = cfg->mass;
}

void star_feed(FwmStar *star, const StarConfig *cfg, double speed) {
    /* Only a burning star has anything to add mass to. A remnant has already
     * been decided, and letting a late throw promote an ember into a hole
     * would make the ending a matter of what happened after it. */
    /* A hole is the other thing that can be fed, and unlike a star it never
     * stops: what falls in stays in, and it grows. */
    if (!star || !cfg) return;
    if (star->phase != STAR_BURNING && star->phase != STAR_HOLE) return;
    double ref = cfg->throw_speed > 0.0 ? cfg->throw_speed : 1.0;
    double share = fabs(speed) / ref;
    if (share > 4.0) share = 4.0; /* one heroic throw is not four windows */
    star->mass += cfg->mass_per_throw * share;
    if (cfg->mass_max > 0.0 && star->mass > cfg->mass_max)
        star->mass = cfg->mass_max;
}

bool star_collapse_now(FwmStar *star) {
    if (!star) return false;

    /* A remnant that is still holding itself up can be pushed over the edge.
     * That is not a shortcut around the mass rule — it is the mass rule: a
     * neutron star that gains enough to pass the TOV limit has nothing left to
     * hold it, which in the sky is how a good share of black holes are made.
     * So a pulsar takes the mass it needs and falls the rest of the way.
     *
     * A dwarf does the same one step at a time: pushed over Chandrasekhar it
     * becomes a pulsar, pushed again it becomes a hole. Each press is one
     * collapse, and each one is a different thing to watch. */
    if (star->phase == STAR_DWARF) {
        star->mass = STAR_CHANDRASEKHAR + 0.01;
    } else if (star->phase == STAR_NEUTRON) {
        star->mass = STAR_TOV + 0.01;
    } else if (star->phase != STAR_BURNING) {
        return false; /* a hole has nowhere further to fall */
    }

    star->from_frac = remnant_fraction(star->phase); /* 1.0 while burning */
    star->from_lum  = star->phase == STAR_DWARF   ? STAR_L_DWARF
                    : star->phase == STAR_NEUTRON ? STAR_L_NEUTRON
                    : (star->mass > 0.0 ? star->mass : 1.0);
    star->fuel      = 0.0;
    star->phase     = STAR_COLLAPSE;
    star->phase_s   = 0.0;
    return true;
}

void star_tick(FwmStar *star, const StarConfig *cfg, double dt) {
    if (!star || !cfg || dt <= 0.0) return;

    switch (star->phase) {
    case STAR_BURNING: {
        star->age_s += dt;
        double life = cfg->lifetime_s > 0.0 ? cfg->lifetime_s : 1.0;
        star->fuel -= dt / life;
        if (star->fuel <= 0.0) {
            star->fuel = 0.0;
            star_collapse_now(star);
        }
        break;
    }
    case STAR_COLLAPSE:
        star->phase_s += dt;
        if (star->phase_s >= (cfg->collapse_s > 0.0 ? cfg->collapse_s : 1.0)) {
            /* The mass it holds at THIS moment is the whole verdict — which is
             * why star_feed stops here and not a tick later. */
            star->phase   = remnant_of(star->mass);
            star->phase_s = 0.0;
            /* A new hole starts at the size its mass says, and grows from
             * there; it must not ease up from nothing. */
            star->shown_mass = star->mass;
        }
        break;
    case STAR_HOLE: {
        /* Swelling, not stepping: the drawn size chases the real mass with a
         * time constant of about a second, so a window going in is a growth
         * spurt rather than a jump cut. */
        star->phase_s += dt;
        double k = 1.0 - exp(-dt / 0.9);
        star->shown_mass += (star->mass - star->shown_mass) * k;
        break;
    }
    case STAR_NEUTRON:
        star->phase_s += dt;
        star->beam = fmod(star->beam + 360.0 * cfg->pulsar_hz * dt, 360.0);
        if (star->beam < 0.0) star->beam += 360.0;
        break;
    default:
        star->phase_s += dt;
        break;
    }
}

/* Where this collapse began, in px: the size it had when the fuse was lit,
 * never smaller than what it is falling to. */
static double collapse_start(const FwmStar *star, const StarConfig *cfg) {
    double end = remnant_radius_px(remnant_of(star->mass), star->mass, cfg);
    double start = cfg->radius * (star->from_frac > 0.0 ? star->from_frac : 1.0);
    return start < end ? end : start;
}

/* Radii in kilometres, which is the only place in this file that leaves the
 * screen's units — and it has to, because compactness is a ratio of two real
 * lengths and drawing a neutron star at thirty pixels does not make it thirty
 * pixels wide. The relations are the usual ones: a neutron star is much the
 * same size whatever it weighs, a white dwarf gets SMALLER as it gets heavier
 * (that is the electron gas, and it is why there is a Chandrasekhar limit at
 * all), and a main-sequence star runs a little under linear in mass. */
#define KM_PER_SOLAR_MASS 2.953   /* the Schwarzschild radius of one sun */
#define KM_NEUTRON        11.5
#define KM_DWARF_AT_ONE   8770.0  /* scaled by m^(-1/3) from here */
#define KM_SUN            696000.0

double star_compactness(const FwmStar *star, const StarConfig *cfg) {
    if (!star) return 0.0;
    /* At a horizon the two lengths are the same length. */
    if (star->phase == STAR_HOLE) return 1.0;

    double m = star->mass > 0.05 ? star->mass : 0.05;
    double rs = KM_PER_SOLAR_MASS * m;
    double R;
    if (star->phase == STAR_NEUTRON)   R = KM_NEUTRON;
    else if (star->phase == STAR_DWARF) R = KM_DWARF_AT_ONE / cbrt(m);
    else                                R = KM_SUN * pow(m, 0.8);

    /* Mid-fall it is neither the star it was nor the remnant it will be, and
     * the drawing already knows how far down it has got. Reusing that keeps
     * the lens exactly in step with the picture, which is the only way the
     * moment it switches on does not read as a switch being thrown. */
    if (star->phase == STAR_COLLAPSE && cfg && cfg->radius > 0.0) {
        R *= clamp01(star_radius(star, cfg) / cfg->radius);
        if (R < KM_NEUTRON) R = KM_NEUTRON;
    }

    double c = R > 0.0 ? rs / R : 0.0;
    return c > 1.0 ? 1.0 : c;
}

/* Below this there is nothing to see: a pulsar is 0.36 and the next thing
 * down, a white dwarf, is 0.0003. */
#define STAR_LENS_MIN 0.02

bool star_lenses(const FwmStar *star, const StarConfig *cfg) {
    return star_compactness(star, cfg) >= STAR_LENS_MIN;
}

double star_radius(const FwmStar *star, const StarConfig *cfg) {
    if (!star || !cfg) return 0.0;
    double r0 = cfg->radius;
    if (star->phase == STAR_BURNING) return r0;
    /* A hole's size IS its mass — the Schwarzschild radius is linear in it —
     * so anything it swallows makes it visibly bigger. The only remnant that
     * grows, and the only one with any business growing. */
    if (star->phase == STAR_HOLE)
        return remnant_radius_px(STAR_HOLE, star->shown_mass, cfg);
    if (star->phase != STAR_COLLAPSE) return r0 * remnant_fraction(star->phase);

    /* Free fall: the pull grows as the star shrinks, so almost all of the
     * shrinking happens in the last moment. r goes as (t_end - t)^(2/3), which
     * is what makes the squeeze read as a fall rather than a fade. */
    double end   = remnant_radius_px(remnant_of(star->mass), star->mass, cfg);
    double start = collapse_start(star, cfg);
    double t = collapse_t(star, cfg);
    return end + (start - end) * pow(1.0 - t, 2.0 / 3.0);
}

double star_hole_half(const StarConfig *cfg) {
    if (!cfg) return 0.0;
    /* The ceiling, or a generous stand-in when there is none: an uncapped hole
     * can be fed for ever, and a buffer cannot follow it there. Four times the
     * TOV limit is a hole that has swallowed a screenful of windows. */
    double ceiling = cfg->mass_max > 0.0 ? cfg->mass_max : STAR_TOV * 4.0;
    if (ceiling < cfg->mass) ceiling = cfg->mass;
    return remnant_radius_px(STAR_HOLE, ceiling, cfg) * STAR_HOLE_BOX;
}

double star_luminosity(const FwmStar *star, const StarConfig *cfg) {
    if (!star || !cfg) return 0.0;
    switch (star->phase) {
    case STAR_BURNING:
        /* Heavier stars burn brighter, and this is the only place the mass is
         * visible before the ending it decides. */
        return star->mass > 0.0 ? star->mass : 0.0;
    case STAR_COLLAPSE: {
        double r = star_radius(star, cfg);
        if (r <= 0.0) return 0.0;
        /* The same start star_radius uses, floor and all: read separately,
         * the two disagreed whenever a collapse ended larger than it began,
         * and the luminosity stepped down at the very moment it should have
         * been continuous. */
        double start = collapse_start(star, cfg);
        double ratio = start / r;
        /* Continuous with whatever it was a moment ago, then the runaway. */
        double base = star->from_lum > 0.0 ? star->from_lum : star->mass;
        double l = base * ratio * ratio;
        if (l > STAR_L_MAX) l = STAR_L_MAX;
        /* And then it is over: the flash is this runaway at its peak, cut off
         * as the surface stops being a surface. */
        double t = collapse_t(star, cfg);
        if (t > STAR_FLASH_AT) {
            double f = (t - STAR_FLASH_AT) / (1.0 - STAR_FLASH_AT);
            l *= clamp01(1.0 - f);
        }
        return l;
    }
    case STAR_DWARF:   return STAR_L_DWARF;
    case STAR_NEUTRON: return STAR_L_NEUTRON;
    case STAR_HOLE:    return 0.0;
    }
    return 0.0;
}

void star_color(const FwmStar *star, const StarConfig *cfg, float out[3]) {
    static const float burning[3] = { 1.00f, 0.87f, 0.60f }; /* ordinary daylight */
    static const float squeezed[3] = { 0.76f, 0.85f, 1.00f }; /* hotter, so bluer */
    static const float ember[3]   = { 1.00f, 0.42f, 0.24f };
    static const float pulsar[3]  = { 0.80f, 0.90f, 1.00f };

    out[0] = out[1] = out[2] = 0.0f;
    if (!star || !cfg) return;
    switch (star->phase) {
    case STAR_BURNING:
        memcpy(out, burning, sizeof(burning));
        break;
    case STAR_COLLAPSE: {
        double t = collapse_t(star, cfg);
        for (int i = 0; i < 3; i++)
            out[i] = (float)lerp(burning[i], squeezed[i], clamp01(t * 1.4));
        break;
    }
    case STAR_DWARF:   memcpy(out, ember,  sizeof(ember));  break;
    case STAR_NEUTRON: memcpy(out, pulsar, sizeof(pulsar)); break;
    case STAR_HOLE:    break; /* drawn by not drawing it */
    }
}

/* How much of the pulsar's beam is pointed at a bearing. A lighthouse, not a
 * lamp: the exponent is what makes it a sweep across the desktop rather than a
 * general flicker, and the floor is the glow that never quite leaves. */
static double beam_at(const FwmStar *star, double bearing_deg) {
    double d = fabs(fmod(bearing_deg - star->beam + 540.0, 360.0) - 180.0);
    double c = cos(d * DEG);
    if (c <= 0.0) return 0.10;
    return 0.10 + 0.90 * pow(c, 24.0);
}

void star_light(const FwmStar *star, const StarConfig *cfg,
                double wx, double wy, FwmSunLight *out) {
    memset(out, 0, sizeof(*out));
    if (!star || !cfg || !cfg->enabled) return;

    double lum = star_luminosity(star, cfg);
    if (lum <= 0.0) return; /* a hole, and the whole of what a hole costs */

    /* Out from the star, along the ground. A window standing exactly under it
     * has no direction to throw a shadow in, and throws none. */
    double dx = wx - star->wx, dy = wy - star->wy;
    double r  = sqrt(dx * dx + dy * dy);
    double h  = cfg->height > 0.0 ? cfg->height : 1.0;

    out->elevation = atan2(h, r) / DEG;
    /* Written back in the sun's convention (azimuth names where the light IS,
     * and dx/dy point away from it) so anything reading a FwmSunLight cannot
     * tell the two sources apart. */
    out->azimuth = (r > 0.0) ? atan2(-dx, dy) / DEG : 0.0;

    if (r > 0.0) {
        /* Same reference length the sun uses: px at 45 degrees, stretched by
         * how shallow the angle is. Here that is r/h — the shadows lengthen
         * with distance from the star, which is the fan. */
        double len = cfg->length * (r / h);
        if (len > cfg->length_max) len = cfg->length_max;
        out->dx = dx / r * len;
        out->dy = dy / r * len;
    }

    /* Inverse square from the star itself, not from the point under it: h sets
     * the scale, so a window one height away is lit half as hard. */
    double att = (h * h) / (r * r + h * h);
    double a   = cfg->opacity * lum * att;
    if (star->phase == STAR_NEUTRON) a *= beam_at(star, out->azimuth + 180.0);
    out->alpha = clamp01(a);
}

double star_weight(const FwmStar *star, const StarConfig *cfg) {
    if (!star || !cfg) return 0.0;
    double w = cfg->weight > 0.0 ? cfg->weight : 1.0;
    double m = star->mass > 0.0 ? star->mass : 1.0;
    return w * m;
}

bool star_push(FwmStar *star, const StarConfig *cfg, double mass,
               double dx, double dy, double speed) {
    if (!star || !cfg || speed <= 0.0) return false;
    if (star->held) return false;   /* the hand wins over anything thrown */

    double self = star_weight(star, cfg);
    /* The whole rule, and the reason it is one line: only something heavier
     * than the star moves the star. A window that is lighter bounces, and the
     * star does not notice it happened. */
    if (mass <= self) return false;

    double len = sqrt(dx * dx + dy * dy);
    if (len < 1e-9) return false;

    /* Momentum shared the way two bodies share it: the ratio of the masses is
     * what decides how much of the blow the star takes, so a barely-heavier
     * window nudges it and a giant one launches it. */
    double share = mass / (mass + self);
    star->vx += dx / len * speed * share;
    star->vy += dy / len * speed * share;
    return true;
}

void star_spin(FwmStar *star, double angvel) {
    if (!star) return;
    star->angvel += angvel;
    /* A star that turns faster than the eye can follow is a flicker, not a
     * spin, and a real one that fast would tear itself apart. */
    double cap = 9.0;
    if (star->angvel >  cap) star->angvel =  cap;
    if (star->angvel < -cap) star->angvel = -cap;
}

void star_grab(FwmStar *star) {
    if (!star) return;
    star->held = 1;
    star->vx = star->vy = 0.0;
}

void star_release(FwmStar *star, double vx, double vy) {
    if (!star) return;
    star->held = 0;
    star->vx = vx;
    star->vy = vy;
}

void star_move(FwmStar *star, const StarConfig *cfg, double dt,
               double min_x, double max_x, double min_y, double max_y) {
    if (!star || !cfg || dt <= 0.0 || star->held) return;

    star->wx += star->vx * dt;
    star->wy += star->vy * dt;
    star->angle += star->angvel * dt;
    if (star->angle > 2.0 * M_PI)  star->angle -= 2.0 * M_PI;
    if (star->angle < -2.0 * M_PI) star->angle += 2.0 * M_PI;

    /* Drag. Space has none, but a desktop is not space: a star shoved once
     * and coasting forever would end up parked against an edge, and getting
     * it back would be a chore rather than a throw. */
    double keep = exp(-dt / 2.5);
    star->vx *= keep;
    star->vy *= keep;
    /* The spin bleeds off far more slowly than the drift: there is nothing for
     * a ball of plasma to rub against, and a star that stops turning the
     * moment you let go looks like a prop. */
    star->angvel *= exp(-dt / 40.0);
    if (fabs(star->vx) < 0.5) star->vx = 0.0;
    if (fabs(star->vy) < 0.5) star->vy = 0.0;

    /* The edges of its own desktop. It bounces rather than stopping — it is a
     * ball of plasma, not a window that snaps to a wall — and loses a little
     * each time, so it settles instead of rattling forever. */
    if (star->wx < min_x) { star->wx = min_x; if (star->vx < 0.0) star->vx = -star->vx * 0.55; }
    if (star->wx > max_x) { star->wx = max_x; if (star->vx > 0.0) star->vx = -star->vx * 0.55; }
    if (star->wy < min_y) { star->wy = min_y; if (star->vy < 0.0) star->vy = -star->vy * 0.55; }
    if (star->wy > max_y) { star->wy = max_y; if (star->vy > 0.0) star->vy = -star->vy * 0.55; }
}

/* ── two of them ──────────────────────────────────────────────────────── */

/* How hard a star pulls, relative to one solar mass at one `height`. The same
 * `pull` that moves windows, so a star and a window feel the same gravity. */
static double pair_accel(const StarConfig *cfg, double mass, double r) {
    double h = cfg->height > 0.0 ? cfg->height : 1.0;
    /* Softened at short range: two objects that pass very close would
     * otherwise take an impulse of nearly infinite size in one tick and be
     * gone from the desktop entirely. */
    double soft = r > h * 0.12 ? r : h * 0.12;
    return cfg->pull * mass * (h * h) / (soft * soft);
}

void star_attract(FwmStar *a, FwmStar *b, const StarConfig *cfg, double dt) {
    if (!a || !b || !cfg || dt <= 0.0) return;
    if (a->held || b->held) return;

    double dx = b->wx - a->wx, dy = b->wy - a->wy;
    double r = sqrt(dx * dx + dy * dy);
    if (r < 1e-6) return;
    double ux = dx / r, uy = dy / r;

    /* Each falls towards the other, and the lighter one falls faster — which
     * is what makes the pair swing about a point between them rather than one
     * of them simply arriving. */
    double aa = pair_accel(cfg, b->mass, r);
    double ab = pair_accel(cfg, a->mass, r);
    a->vx += ux * aa * dt;  a->vy += uy * aa * dt;
    b->vx -= ux * ab * dt;  b->vy -= uy * ab * dt;
}

/* How compact each phase is, for deciding who robs whom. A hole takes from
 * anything; a neutron star takes from a burning one; nothing takes from a
 * hole. */
static int compactness(FwmStarPhase p) {
    switch (p) {
    case STAR_HOLE:    return 4;
    case STAR_NEUTRON: return 3;
    case STAR_DWARF:   return 2;
    case STAR_BURNING: return 1;
    default:           return 0;   /* mid-collapse: not taking part */
    }
}

double star_siphon(FwmStar *thief, FwmStar *victim, const StarConfig *cfg, double dt) {
    if (!thief || !victim || !cfg || dt <= 0.0) return 0.0;
    if (compactness(thief->phase) <= compactness(victim->phase)) return 0.0;

    /* The Roche lobe: how close the two must be before the victim's own
     * gravity stops winning at its surface. Scaled off the victim's size,
     * because that is what has to reach across. */
    double vr = star_radius(victim, cfg);
    double reach = vr * 7.0;
    double dx = thief->wx - victim->wx, dy = thief->wy - victim->wy;
    double r = sqrt(dx * dx + dy * dy);
    if (r >= reach || r < 1e-6) return 0.0;

    /* Steeply nonlinear, as overflow is: nothing at all until the lobe is
     * nearly filled, then a torrent. */
    double over = (reach - r) / reach;
    double rate = over * over * over;
    if (rate <= 0.0) return 0.0;

    /* Solar masses a second at full flow. Fast enough to watch, slow enough
     * that a star is not gone before you have looked at it. */
    double moved = rate * 0.20 * dt;
    if (moved > victim->mass) moved = victim->mass;
    victim->mass -= moved;
    thief->mass  += moved;
    if (cfg->mass_max > 0.0 && thief->mass > cfg->mass_max)
        thief->mass = cfg->mass_max;
    return rate;
}

bool star_touching(const FwmStar *a, const FwmStar *b, const StarConfig *cfg) {
    if (!a || !b || !cfg) return false;
    double ra = star_radius(a, cfg), rb = star_radius(b, cfg);
    /* A hole reaches as far as its shadow: that is what is drawn, and what
     * anything arriving there has visibly fallen into. */
    if (a->phase == STAR_HOLE) ra *= 2.6;
    if (b->phase == STAR_HOLE) rb *= 2.6;
    double dx = b->wx - a->wx, dy = b->wy - a->wy;
    return dx * dx + dy * dy <= (ra + rb) * (ra + rb);
}

void star_merge(FwmStar *a, FwmStar *b, const StarConfig *cfg) {
    if (!a || !b || !cfg) return;

    double ma = a->mass > 0.0 ? a->mass : 0.0;
    double mb = b->mass > 0.0 ? b->mass : 0.0;
    double total = ma + mb;
    if (total <= 0.0) return;

    /* Where they meet and how fast the pair was going: the centre of mass,
     * and its momentum. Both are conserved, so the merged object carries on
     * exactly as the pair was carrying on. */
    a->wx = (a->wx * ma + b->wx * mb) / total;
    a->wy = (a->wy * ma + b->wy * mb) / total;
    a->vx = (a->vx * ma + b->vx * mb) / total;
    a->vy = (a->vy * ma + b->vy * mb) / total;

    /* Not all of it arrives. Several percent leaves as gravitational waves —
     * the loudest thing in the universe and completely invisible — which is
     * the one part of this a compositor can only take on trust. */
    a->mass = total * 0.96;

    /* And then it settles into whatever that mass allows, through a collapse,
     * so the merger is watched rather than switched. */
    a->from_frac = remnant_fraction(a->phase);
    a->from_lum  = star_luminosity(a, cfg);
    a->phase     = STAR_COLLAPSE;
    a->phase_s   = 0.0;
    a->shown_mass = a->mass;

    b->mass = 0.0;
    b->phase = STAR_HOLE;
    b->shown_mass = 0.0;
}

/* Seconds the shell stays visible after the star lets go. */
#define STAR_BLAST_S 2.6

/* Seconds a star spends gathering itself before it lights. */
#define STAR_IGNITE_S 2.2

/* How far the magnetic axis leans from the spin axis. Real pulsars run the
 * whole range; well off-axis is what makes one a lighthouse rather than a lamp
 * that happens to rotate. */
#define STAR_MAG_TILT_DEG 58.0

void star_pulse(const FwmStar *star, const StarConfig *cfg,
                double *bearing_deg, double *aim) {
    if (bearing_deg) *bearing_deg = 0.0;
    if (aim) *aim = 0.0;
    if (!star || !cfg) return;

    /* The spin itself is dead even — that is the one thing a pulsar is famous
     * for — so `beam` advances linearly and everything uneven below comes out
     * of geometry, not out of a wobble in the rotation. */
    double phase = star->beam * DEG;
    double lean = STAR_MAG_TILT_DEG * DEG;

    /* The magnetic pole, swung round the spin axis on its cone. Taking the
     * viewer to be looking along +z, the pole's direction is: */
    double px = sin(lean) * cos(phase);
    double py = cos(lean);
    double pz = sin(lean) * sin(phase);

    /* On the sky that is an ellipse, and the bearing round it is not the phase
     * — which is the whole point. atan2 of the projected components gives the
     * angle actually seen, and it runs fast where the cone is edge-on and slow
     * where it is face-on. */
    if (bearing_deg) *bearing_deg = atan2(px, -py) / DEG;

    /* And how nearly it is pointed at you: the component along the line of
     * sight. Sharpened, because a pulse is narrow — you are inside the beam
     * for a small part of each turn and outside it for the rest. */
    if (aim) {
        double toward = 0.5 + 0.5 * pz;
        *aim = pow(toward, 3.0);
    }
}

double star_ignition(const FwmStar *star, const StarConfig *cfg) {
    (void)cfg;
    if (!star || star->phase != STAR_BURNING) return 1.0;
    if (star->age_s >= STAR_IGNITE_S) return 1.0;
    return star->age_s / STAR_IGNITE_S;
}

double star_blast(const FwmStar *star, const StarConfig *cfg) {
    if (!star || !cfg) return 0.0;

    /* The last sliver of the fall, when the core has already bounced. */
    if (star->phase == STAR_COLLAPSE) {
        double dur = cfg->collapse_s > 0.0 ? cfg->collapse_s : 1.0;
        double t = star->phase_s / dur;
        if (t < STAR_FLASH_AT) return 0.0;
        /* Maps the tail of the collapse onto the first sliver of the blast, so
         * the shell is already on its way out as the star finishes falling. */
        return (t - STAR_FLASH_AT) / (1.0 - STAR_FLASH_AT) * 0.15;
    }

    /* And on into the remnant, where the shell does its expanding. A hole is
     * included: what is blown off is the star's own envelope, and whether the
     * core ended as a hole makes no difference to it.
     *
     * A star that is still BURNING has not blown anything off and must fall
     * through here — the phases are listed rather than assumed, because
     * "anything that is not collapsing" also covers the one case that has not
     * collapsed yet. */
    if (star->phase != STAR_DWARF && star->phase != STAR_NEUTRON &&
        star->phase != STAR_HOLE)
        return 0.0;
    if (star->phase_s >= STAR_BLAST_S) return 0.0;
    return 0.15 + 0.85 * (star->phase_s / STAR_BLAST_S);
}

const char *star_phase_name(FwmStarPhase phase) {
    switch (phase) {
    case STAR_BURNING:  return "burning";
    case STAR_COLLAPSE: return "collapsing";
    case STAR_DWARF:    return "white dwarf";
    case STAR_NEUTRON:  return "pulsar";
    case STAR_HOLE:     return "black hole";
    }
    return "?";
}
