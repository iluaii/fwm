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

#ifndef FWM_LIQUID_KINDS_H
#define FWM_LIQUID_KINDS_H

#include <stdbool.h>

/* The liquids there are to pick from: water, and whatever has been made in
 * the liquids menu.
 *
 * Water is always first and cannot be removed. What it IS comes from
 * [liquids] in the config — its colour and its viscosity are those keys, live
 * under `fwmctl set` — unless it has been edited in the menu, in which case
 * the edit is laid over the config the way every other choice made in fwm's
 * own UI is.
 *
 * Everything made in the menu lives in ~/.local/state/fwm/liquids, never in
 * config.toml: fwm does not rewrite the user's file. This module only turns
 * that file into a list and back; reading and writing it is the caller's. No
 * wlroots, so the round trip is tested on its own (tests/test_liquid_kinds.c). */

#define LIQUID_KINDS_MAX 16
#define LIQUID_NAME_MAX  32

typedef struct {
    int    id;                      /* stable across edits and restarts; 0 is water */
    char   name[LIQUID_NAME_MAX];
    float  color[4];                /* straight RGB and alpha, 0..1 */
    double viscosity;               /* px²/s */
    double density;                 /* relative to water */
} LiquidKind;

typedef struct {
    LiquidKind kind[LIQUID_KINDS_MAX];
    int  count;
    int  next_id;
    /* Water was edited in the menu, so the file speaks for it over the
     * config. Until then the config does. */
    bool water_edited;
} LiquidKinds;

/* Water alone, with the given colour (straight RGBA) and viscosity. */
void liquid_kinds_init(LiquidKinds *k, const float color[4], double viscosity);

/* Read the state file's text over `k` (which should hold water alone):
 * water's edit, if it has one, and every liquid that was made. Anything it
 * cannot make sense of is skipped rather than refused — the file is fwm's, and
 * a half-readable one should cost the liquids it garbled, not all of them.
 * Returns how many liquids were made from it. */
int liquid_kinds_parse(LiquidKinds *k, const char *text);

/* The state file's text, malloc'd; NULL on no memory. */
char *liquid_kinds_serialize(const LiquidKinds *k);

/* Add one; the id is assigned here. Returns it, or -1 when the list is full
 * or the name is empty. */
int liquid_kinds_add(LiquidKinds *k, const LiquidKind *kind);
/* Replace the fields of liquid `id` (keeping its id). False if there is none. */
bool liquid_kinds_update(LiquidKinds *k, int id, const LiquidKind *kind);
/* Remove one. Water cannot be removed. */
bool liquid_kinds_remove(LiquidKinds *k, int id);
const LiquidKind *liquid_kinds_find(const LiquidKinds *k, int id);
int liquid_kinds_index(const LiquidKinds *k, int id);

/* The line the menu shows under a liquid's name: its thickness and density. */
void liquid_kind_describe(const LiquidKind *kind, char *out, int cap);

/* The ranges the menu and the file both hold a liquid to. */
#define LIQUID_VISCOSITY_MAX 100000.0
#define LIQUID_DENSITY_MIN   0.1
#define LIQUID_DENSITY_MAX   20.0

#endif /* FWM_LIQUID_KINDS_H */
