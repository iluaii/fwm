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

/* The liquids made in the menu outlive the session in a file fwm writes and
 * reads back, and a liquid that came back renamed, recoloured or under
 * someone else's id — the id is what a poured liquid remembers it is — would
 * be a silent loss nobody notices until they reach for it. */

#include "test.h"
#include "liquid_kinds.h"

#include <stdlib.h>
#include <string.h>

static const float BLUE[4] = { 0.2f, 0.5f, 0.9f, 0.8f };

static LiquidKind make(const char *name, double visc, double dens) {
    LiquidKind k = { .viscosity = visc, .density = dens, .color = { 0.9f, 0.6f, 0.1f, 0.8f } };
    snprintf(k.name, sizeof(k.name), "%s", name);
    return k;
}

static void test_round_trip(void) {
    CASE("liquids made in the menu come back exactly as they were");
    LiquidKinds a;
    liquid_kinds_init(&a, BLUE, 60.0);
    LiquidKind honey = make("Honey", 3000, 1.4);
    LiquidKind odd = make("Say \"hi\" \\ there", 10, 0.8);
    int h = liquid_kinds_add(&a, &honey);
    int o = liquid_kinds_add(&a, &odd);
    CHECK(h > 0 && o > 0 && h != o);
    CHECK(liquid_kinds_remove(&a, h));
    LiquidKind tar = make("Tar", 90000, 1.2);
    int t = liquid_kinds_add(&a, &tar);
    CHECK(t != h);   /* an id is never handed out twice */

    char *text = liquid_kinds_serialize(&a);
    CHECK_NOT_NULL(text);
    LiquidKinds b;
    liquid_kinds_init(&b, BLUE, 60.0);
    CHECK_INT(liquid_kinds_parse(&b, text), 2);
    free(text);
    CHECK_INT(b.count, 3);
    CHECK(!b.water_edited);
    const LiquidKind *bo = liquid_kinds_find(&b, o);
    CHECK_NOT_NULL(bo);
    if (bo) {
        CHECK_STR(bo->name, "Say \"hi\" \\ there");
        CHECK_DBL(bo->viscosity, 10, 1e-9);
        CHECK_DBL(bo->density, 0.8, 1e-9);
        CHECK_DBL(bo->color[1], 0.6, 0.004);
    }
    CHECK_NOT_NULL(liquid_kinds_find(&b, t));
    /* The next one made after the restart does not reuse an old id either. */
    LiquidKind more = make("More", 1, 1);
    CHECK(liquid_kinds_add(&b, &more) > t);
}

static void test_water(void) {
    CASE("water cannot be removed, and its edit is kept over the config");
    LiquidKinds a;
    liquid_kinds_init(&a, BLUE, 60.0);
    CHECK(!liquid_kinds_remove(&a, 0));
    char *text = liquid_kinds_serialize(&a);
    LiquidKinds b;
    liquid_kinds_init(&b, BLUE, 77.0);
    liquid_kinds_parse(&b, text);
    free(text);
    /* Unedited, the file says nothing about water: the config's 77 stands. */
    CHECK_DBL(b.kind[0].viscosity, 77.0, 1e-9);

    LiquidKind w = make("Sea", 20, 1.03);
    CHECK(liquid_kinds_update(&a, 0, &w));
    CHECK(a.water_edited);
    text = liquid_kinds_serialize(&a);
    liquid_kinds_init(&b, BLUE, 77.0);
    liquid_kinds_parse(&b, text);
    free(text);
    CHECK(b.water_edited);
    CHECK_STR(b.kind[0].name, "Sea");
    CHECK_DBL(b.kind[0].viscosity, 20.0, 1e-9);
    CHECK_INT(b.kind[0].id, 0);
}

static void test_bad_file(void) {
    CASE("a garbled file costs what it garbled, not everything");
    LiquidKinds k;
    liquid_kinds_init(&k, BLUE, 60.0);
    const char *text =
        "[[liquid]]\nid = 4\nname = \"Oil\"\nviscosity = 400\ndensity = 0.9\n"
        "[[liquid]]\nid = 4\nname = \"Twin\"\n"
        "[[liquid]]\nname = \"\"\n"
        "[[liquid]]\nid = 9\nname = \"Wild\"\nviscosity = -5\ndensity = 1e9\ncolor = \"red\"\n";
    CHECK_INT(liquid_kinds_parse(&k, text), 3);
    CHECK_INT(k.count, 4);
    const LiquidKind *twin = &k.kind[2];
    CHECK_STR(twin->name, "Twin");
    CHECK(twin->id != 4);
    const LiquidKind *wild = liquid_kinds_find(&k, 9);
    CHECK_NOT_NULL(wild);
    if (wild) {
        CHECK_DBL(wild->viscosity, 0.0, 1e-9);
        CHECK_DBL(wild->density, LIQUID_DENSITY_MAX, 1e-9);
    }
    CHECK_INT(liquid_kinds_parse(&k, "this is [not toml"), 0);
    CHECK_INT(k.count, 4);

    CASE("the list has a ceiling");
    liquid_kinds_init(&k, BLUE, 60.0);
    LiquidKind x = make("x", 1, 1);
    for (int i = 1; i < LIQUID_KINDS_MAX; i++) CHECK(liquid_kinds_add(&k, &x) > 0);
    CHECK_INT(liquid_kinds_add(&k, &x), -1);
    LiquidKind nameless = make("", 1, 1);
    liquid_kinds_init(&k, BLUE, 60.0);
    CHECK_INT(liquid_kinds_add(&k, &nameless), -1);
}

static void test_describe(void) {
    CASE("the line under a liquid's name is its two numbers and nothing else");
    char d[96];
    LiquidKind h = make("Honey", 3000, 1.4);
    liquid_kind_describe(&h, d, sizeof d);
    CHECK_STR(d, "thickness 3000 · density 1.4");
    LiquidKind w = make("Water", 2.5, 1);
    liquid_kind_describe(&w, d, sizeof d);
    CHECK_STR(d, "thickness 2.5 · density 1");
}

static void test_ready_made(void) {
    CASE("a ready-made liquid is in the list, not in the file, and cannot be removed");
    LiquidKinds k;
    liquid_kinds_init(&k, BLUE, 60.0);
    LiquidKind oil = make("Oil", 300.0, 0.9);
    CHECK(liquid_kinds_add_preset(&k, &oil, LIQUID_PRESET_ID));
    CHECK_INT(k.count, 2);
    CHECK(liquid_kinds_find(&k, LIQUID_PRESET_ID)->preset);
    CHECK(!liquid_kinds_remove(&k, LIQUID_PRESET_ID));
    char *text = liquid_kinds_serialize(&k);
    CHECK(text && !strstr(text, "Oil"));
    free(text);

    CASE("one the user made under the same name is theirs, and stays");
    liquid_kinds_init(&k, BLUE, 60.0);
    LiquidKind mine = make("oil", 50.0, 0.8);
    int id = liquid_kinds_add(&k, &mine);
    CHECK(!liquid_kinds_add_preset(&k, &oil, LIQUID_PRESET_ID));
    CHECK_INT(k.count, 2);
    CHECK_DBL(liquid_kinds_find(&k, id)->viscosity, 50.0, 1e-9);

    CASE("edited, a ready-made liquid is the user's, keeps its id, and is written down");
    liquid_kinds_init(&k, BLUE, 60.0);
    LiquidKind honey = make("Honey", 3000.0, 1.4);
    liquid_kinds_add_preset(&k, &oil, LIQUID_PRESET_ID);
    liquid_kinds_add_preset(&k, &honey, LIQUID_PRESET_ID + 1);
    LiquidKind thin = make("Oil", 120.0, 0.9);
    CHECK(liquid_kinds_update(&k, LIQUID_PRESET_ID, &thin));
    CHECK(!liquid_kinds_find(&k, LIQUID_PRESET_ID)->preset);
    text = liquid_kinds_serialize(&k);
    CHECK(text && strstr(text, "Oil") && !strstr(text, "Honey"));
    /* As the next start reads it: the edit under its own id, the rest of the
     * ready-made ones after it, and a new liquid numbered below them all. */
    LiquidKinds back;
    liquid_kinds_init(&back, BLUE, 60.0);
    CHECK_INT(liquid_kinds_parse(&back, text), 1);
    free(text);
    CHECK(liquid_kinds_find(&back, LIQUID_PRESET_ID) != NULL);
    CHECK_DBL(liquid_kinds_find(&back, LIQUID_PRESET_ID)->viscosity, 120.0, 1e-9);
    CHECK(!liquid_kinds_add_preset(&back, &oil, LIQUID_PRESET_ID));
    CHECK(liquid_kinds_add_preset(&back, &honey, LIQUID_PRESET_ID + 1));
    LiquidKind fresh = make("Brine", 40.0, 1.2);
    int fid = liquid_kinds_add(&back, &fresh);
    CHECK(fid > 0 && fid < LIQUID_PRESET_ID);
}

int main(void) {
    test_ready_made();
    test_round_trip();
    test_water();
    test_bad_file();
    test_describe();
    return t_report("test_liquid_kinds");
}
