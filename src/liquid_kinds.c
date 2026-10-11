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

#include "liquid_kinds.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "toml.h"

void liquid_kinds_init(LiquidKinds *k, const float color[4], double viscosity) {
    memset(k, 0, sizeof(*k));
    LiquidKind *w = &k->kind[0];
    w->id = 0;
    snprintf(w->name, sizeof(w->name), "Water");
    memcpy(w->color, color, sizeof(w->color));
    w->viscosity = viscosity;
    w->density = 1.0;
    k->count = 1;
    k->next_id = 1;
}

/* What a liquid may hold. The menu holds the user to the same ranges, so this
 * only ever bites on a file someone edited by hand. */
static void clamp_kind(LiquidKind *kind) {
    if (!(kind->viscosity >= 0.0)) kind->viscosity = 0.0;
    if (kind->viscosity > LIQUID_VISCOSITY_MAX) kind->viscosity = LIQUID_VISCOSITY_MAX;
    if (!(kind->density >= LIQUID_DENSITY_MIN)) kind->density = LIQUID_DENSITY_MIN;
    if (kind->density > LIQUID_DENSITY_MAX) kind->density = LIQUID_DENSITY_MAX;
    for (int i = 0; i < 4; i++) {
        if (!(kind->color[i] >= 0.0f)) kind->color[i] = 0.0f;
        if (kind->color[i] > 1.0f) kind->color[i] = 1.0f;
    }
}

/* #RRGGBB or #RRGGBBAA, straight. */
static bool parse_hex(const char *s, float out[4]) {
    if (!s || s[0] != '#') return false;
    size_t len = strlen(s + 1);
    if (len != 6 && len != 8) return false;
    unsigned v[4] = { 0, 0, 0, 255 };
    for (size_t i = 0; i < len / 2; i++) {
        char buf[3] = { s[1 + 2 * i], s[2 + 2 * i], 0 };
        char *end;
        v[i] = (unsigned)strtoul(buf, &end, 16);
        if (*end) return false;
    }
    for (int i = 0; i < 4; i++) out[i] = (float)v[i] / 255.0f;
    return true;
}

/* One liquid's fields from a table; false if it has no usable name. */
static bool read_kind(const toml_table_t *t, LiquidKind *kind) {
    toml_datum_t s = toml_string_in(t, "name");
    if (!s.ok) return false;
    snprintf(kind->name, sizeof(kind->name), "%s", s.u.s);
    free(s.u.s);
    if (!kind->name[0]) return false;
    toml_datum_t c = toml_string_in(t, "color");
    if (c.ok) {
        float rgba[4];
        if (parse_hex(c.u.s, rgba)) memcpy(kind->color, rgba, sizeof(rgba));
        free(c.u.s);
    }
    toml_datum_t v = toml_double_in(t, "viscosity");
    if (v.ok) kind->viscosity = v.u.d;
    toml_datum_t d = toml_double_in(t, "density");
    if (d.ok) kind->density = d.u.d;
    clamp_kind(kind);
    return true;
}

int liquid_kinds_parse(LiquidKinds *k, const char *text) {
    if (!k || !text) return 0;
    char *copy = strdup(text);
    if (!copy) return 0;
    char err[128];
    toml_table_t *root = toml_parse(copy, err, sizeof(err));
    free(copy);
    if (!root) return 0;

    toml_table_t *water = toml_table_in(root, "water");
    if (water) {
        LiquidKind w = k->kind[0];
        if (read_kind(water, &w)) {
            w.id = 0;
            k->kind[0] = w;
            k->water_edited = true;
        }
    }

    int made = 0;
    toml_array_t *list = toml_array_in(root, "liquid");
    int n = list ? toml_array_nelem(list) : 0;
    for (int i = 0; i < n && k->count < LIQUID_KINDS_MAX; i++) {
        toml_table_t *t = toml_table_at(list, i);
        if (!t) continue;
        LiquidKind kind = {
            .color = { 0.23f, 0.55f, 0.88f, 0.8f },
            .viscosity = 60.0,
            .density = 1.0,
        };
        if (!read_kind(t, &kind)) continue;
        toml_datum_t id = toml_int_in(t, "id");
        /* A ready-made liquid edited in the menu keeps its id, so that what
         * was poured of it is still it; new ones are numbered below those. */
        kind.id = (id.ok && id.u.i > 0 && id.u.i < LIQUID_PRESET_ID + 1000) ? (int)id.u.i
                                                                           : k->next_id;
        /* An id the file repeats would make two liquids one; the second gets
         * a new one. */
        if (liquid_kinds_find(k, kind.id)) kind.id = k->next_id;
        k->kind[k->count++] = kind;
        if (kind.id >= k->next_id && kind.id < LIQUID_PRESET_ID) k->next_id = kind.id + 1;
        made++;
    }
    toml_free(root);
    return made;
}

/* A name as a TOML basic string: quotes and backslashes escaped, control
 * characters dropped — a name is one line of text. */
static void put_string(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p >= 0x20 && *p != 0x7f) fputc(*p, f);
    }
    fputc('"', f);
}

static void put_kind(FILE *f, const LiquidKind *kind, bool with_id) {
    if (with_id) fprintf(f, "id = %d\n", kind->id);
    fputs("name = ", f);
    put_string(f, kind->name);
    fprintf(f, "\ncolor = \"#%02X%02X%02X%02X\"\n",
            (unsigned)lroundf(kind->color[0] * 255.0f), (unsigned)lroundf(kind->color[1] * 255.0f),
            (unsigned)lroundf(kind->color[2] * 255.0f), (unsigned)lroundf(kind->color[3] * 255.0f));
    fprintf(f, "viscosity = %.6g\ndensity = %.6g\n", kind->viscosity, kind->density);
}

char *liquid_kinds_serialize(const LiquidKinds *k) {
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) return NULL;
    fputs("# The liquids made in fwm's liquids menu, and water as it was edited\n"
          "# there. Written by fwm each time the menu changes them.\n", f);
    if (k->water_edited) {
        fputs("\n[water]\n", f);
        put_kind(f, &k->kind[0], false);
    }
    for (int i = 1; i < k->count; i++) {
        if (k->kind[i].preset) continue;
        fputs("\n[[liquid]]\n", f);
        put_kind(f, &k->kind[i], true);
    }
    if (fclose(f) != 0) { free(buf); return NULL; }
    return buf;
}

int liquid_kinds_index(const LiquidKinds *k, int id) {
    for (int i = 0; i < k->count; i++)
        if (k->kind[i].id == id) return i;
    return -1;
}

const LiquidKind *liquid_kinds_find(const LiquidKinds *k, int id) {
    int i = liquid_kinds_index(k, id);
    return i >= 0 ? &k->kind[i] : NULL;
}

int liquid_kinds_add(LiquidKinds *k, const LiquidKind *kind) {
    if (!k || !kind || !kind->name[0] || k->count >= LIQUID_KINDS_MAX) return -1;
    LiquidKind copy = *kind;
    copy.id = k->next_id++;
    clamp_kind(&copy);
    k->kind[k->count++] = copy;
    return copy.id;
}

bool liquid_kinds_add_preset(LiquidKinds *k, const LiquidKind *kind, int id) {
    if (!k || !kind || !kind->name[0] || k->count >= LIQUID_KINDS_MAX) return false;
    for (int i = 0; i < k->count; i++)
        if (strcasecmp(k->kind[i].name, kind->name) == 0) return false;
    if (liquid_kinds_find(k, id)) return false;
    LiquidKind copy = *kind;
    copy.id = id;
    copy.preset = true;
    clamp_kind(&copy);
    k->kind[k->count++] = copy;
    return true;
}

bool liquid_kinds_update(LiquidKinds *k, int id, const LiquidKind *kind) {
    int i = liquid_kinds_index(k, id);
    if (i < 0 || !kind->name[0]) return false;
    LiquidKind copy = *kind;
    copy.id = id;
    copy.preset = false;   /* changed, it is the user's now */
    clamp_kind(&copy);
    k->kind[i] = copy;
    if (id == 0) k->water_edited = true;
    return true;
}

bool liquid_kinds_remove(LiquidKinds *k, int id) {
    if (id == 0) return false;
    int i = liquid_kinds_index(k, id);
    if (i < 0 || k->kind[i].preset) return false;
    memmove(&k->kind[i], &k->kind[i + 1], sizeof(k->kind[0]) * (size_t)(k->count - i - 1));
    k->count--;
    return true;
}

/* Only the two numbers, named as the form names them: the list is for
 * telling liquids apart at a glance, and anything more was a sentence to read
 * through to find them. */
void liquid_kind_describe(const LiquidKind *kind, char *out, int cap) {
    char visc[24];
    if (kind->viscosity >= 10.0) snprintf(visc, sizeof visc, "%.0f", kind->viscosity);
    else                         snprintf(visc, sizeof visc, "%.1f", kind->viscosity);
    snprintf(out, (size_t)cap, "thickness %s · density %.3g", visc, kind->density);
}
