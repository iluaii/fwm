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

/* config.c parses attacker-adjacent input — a hand-edited file that the user
 * can get arbitrarily wrong — and it carries a hard promise: a broken config
 * must never cost the session. These tests hold it to that, and pin down the
 * parsed values so a refactor cannot quietly change what a config means. */

#include "test.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static char tmp_path[256];

/* Write `body` to a fresh file and return its path. */
static const char *write_config(const char *body) {
    snprintf(tmp_path, sizeof tmp_path, "/tmp/fwm-test-config-%d.toml", (int)getpid());
    FILE *f = fopen(tmp_path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", tmp_path); exit(2); }
    fputs(body, f);
    fclose(f);
    return tmp_path;
}

static void drop_config(void) { unlink(tmp_path); }

static void test_missing_file(void) {
    /* The path the compositor hands in need not exist — a fresh install has no
     * config at all, and that has to boot to a usable desktop. */
    CASE("missing file still yields a usable config");
    FwmConfig cfg;
    config_load(&cfg, "/nonexistent/fwm/does-not-exist.toml");

    CHECK(cfg.error_count > 0);          /* the problem is reported ... */
    CHECK_INT(cfg.fallback_binds, 1);    /* ... and built-in binds took over */
    CHECK(cfg.key_count > 0);            /* so the keyboard still works */
    CHECK_DBL(cfg.physics.friction, 0.97, 1e-9);
    CHECK_DBL(cfg.physics.gravity, 200.0, 1e-9);
    CHECK_DBL(cfg.physics.restitution, 0.75, 1e-9);
    CHECK_DBL(cfg.physics.tick_rate, 60.0, 1e-9);
    config_free(&cfg);
}

static void test_values_parse(void) {
    CASE("values are read from the file");
    const char *p = write_config(
        "[physics]\n"
        "friction = 0.5\n"
        "gravity  = 123.5\n"
        "[tiling]\n"
        "gaps_in  = 7\n"
        "gaps_out = 21\n"
        "[decor]\n"
        "border_width = 3\n"
        "[input]\n"
        "kbd_layout   = \"us,ru\"\n"
        "repeat_rate  = 25\n"
        "repeat_delay = 600\n"
        "[binds]\n"
        "\"super+q\" = \"killclient\"\n");
    FwmConfig cfg;
    config_load(&cfg, p);

    CHECK_INT(cfg.error_count, 0);
    CHECK_DBL(cfg.physics.friction, 0.5, 1e-9);
    CHECK_DBL(cfg.physics.gravity, 123.5, 1e-9);
    CHECK_INT(cfg.tiling.gaps_in, 7);
    CHECK_INT(cfg.tiling.gaps_out, 21);
    CHECK_INT(cfg.decor.border_width, 3);
    CHECK_STR(cfg.input.kbd_layout, "us,ru");
    CHECK_INT(cfg.input.repeat_rate, 25);
    CHECK_INT(cfg.input.repeat_delay, 600);

    /* A file that supplied a usable bind must not be treated as bind-less. */
    CHECK_INT(cfg.fallback_binds, 0);
    CHECK(cfg.key_count > 0);
    CHECK_STR(cfg.source, p);

    /* Keys the file never mentioned keep their defaults. */
    CHECK_DBL(cfg.physics.tick_rate, 60.0, 1e-9);
    config_free(&cfg);
    drop_config();
}

static void test_colors(void) {
    CASE("#RRGGBB");
    const char *p = write_config(
        "[decor]\n"
        "col_active = \"#ff8000\"\n"
        "[binds]\n\"super+q\" = \"killclient\"\n");
    FwmConfig cfg;
    config_load(&cfg, p);
    CHECK_DBL(cfg.decor.col_active[0], 1.0, 1e-3);
    CHECK_DBL(cfg.decor.col_active[1], 128 / 255.0, 1e-3);
    CHECK_DBL(cfg.decor.col_active[2], 0.0, 1e-3);
    CHECK_DBL(cfg.decor.col_active[3], 1.0, 1e-3);
    config_free(&cfg);

    /* Eight digits carry alpha, and the result is premultiplied because that
     * is what wlr_scene_rect expects — half-alpha red is (0.5, 0, 0, 0.5),
     * not (1, 0, 0, 0.5). */
    CASE("#RRGGBBAA is premultiplied");
    p = write_config(
        "[decor]\n"
        "col_active = \"#ff000080\"\n"
        "[binds]\n\"super+q\" = \"killclient\"\n");
    config_load(&cfg, p);
    CHECK_DBL(cfg.decor.col_active[3], 128 / 255.0, 1e-3);
    CHECK_DBL(cfg.decor.col_active[0], 128 / 255.0, 1e-3);
    CHECK_DBL(cfg.decor.col_active[1], 0.0, 1e-3);
    config_free(&cfg);
    drop_config();
}

static void test_bad_input(void) {
    /* Each of these is something a user can plausibly type. None may crash,
     * and every one has to leave a working config behind. */
    CASE("malformed toml");
    const char *p = write_config("[physics\nfriction = = 0.5\n}{\n");
    FwmConfig cfg;
    config_load(&cfg, p);
    CHECK(cfg.error_count > 0);
    CHECK(cfg.key_count > 0);
    CHECK_INT(cfg.fallback_binds, 1);
    CHECK_DBL(cfg.physics.friction, 0.97, 1e-9);   /* default survived */
    config_free(&cfg);

    CASE("empty file");
    p = write_config("");
    config_load(&cfg, p);
    CHECK(cfg.key_count > 0);
    CHECK_INT(cfg.fallback_binds, 1);
    config_free(&cfg);

    CASE("wrong types where numbers belong");
    p = write_config(
        "[physics]\nfriction = \"lots\"\n"
        "[tiling]\ngaps_in = \"wide\"\n"
        "[binds]\n\"super+q\" = \"killclient\"\n");
    config_load(&cfg, p);
    CHECK_DBL(cfg.physics.friction, 0.97, 1e-9);
    CHECK_INT(cfg.tiling.gaps_in, 6);              /* documented default */
    config_free(&cfg);

    CASE("unparseable colour keeps the default");
    p = write_config(
        "[decor]\ncol_active = \"not-a-colour\"\n"
        "[binds]\n\"super+q\" = \"killclient\"\n");
    config_load(&cfg, p);
    CHECK(cfg.error_count > 0);
    CHECK(cfg.decor.col_active[3] > 0.0);          /* still a visible colour */
    config_free(&cfg);

    CASE("bind to an action that does not exist");
    p = write_config("[binds]\n\"super+q\" = \"no_such_action\"\n");
    config_load(&cfg, p);
    CHECK(cfg.key_count > 0);                      /* usable either way */
    config_free(&cfg);

    CASE("bind with an unparseable key");
    p = write_config("[binds]\n\"super+\" = \"killclient\"\n\"\" = \"launcher\"\n");
    config_load(&cfg, p);
    CHECK(cfg.key_count > 0);
    config_free(&cfg);
    drop_config();
}

static void test_error_cap(void) {
    /* errors[] is a fixed array; error_total counts everything seen while
     * error_count stops at the cap. A config that is wrong on every line must
     * not walk off the end of it. */
    CASE("more errors than the array holds");
    /* Distinct keys on purpose: repeating one key is a TOML duplicate, which
     * aborts the parse after a single error and never reaches the cap. Each
     * line here has to be independently wrong. */
    char body[16384];
    int n = snprintf(body, sizeof body, "[binds]\n");
    for (int i = 0; i < CONFIG_MAX_ERRORS * 3; i++)
        n += snprintf(body + n, sizeof body - (size_t)n,
                      "\"super+nosuchkey%d\" = \"killclient\"\n", i);
    const char *p = write_config(body);

    FwmConfig cfg;
    config_load(&cfg, p);
    CHECK(cfg.error_total > CONFIG_MAX_ERRORS);        /* the cap was passed */
    CHECK_INT(cfg.error_count, CONFIG_MAX_ERRORS);     /* storage stopped there */
    CHECK(cfg.key_count > 0);
    config_free(&cfg);
    drop_config();
}

static void test_tilde_expansion(void) {
    /* Config paths are hand-written, so "~" has to be expanded here — no
     * shell is involved by the time the compositor reads them. */
    CASE("~ in a path becomes an absolute path");
    const char *p = write_config(
        "[wallpaper_picker]\ndir = \"~/Pictures\"\n"
        "[binds]\n\"super+q\" = \"killclient\"\n");
    FwmConfig cfg;
    config_load(&cfg, p);
    if (getenv("HOME")) {
        CHECK(cfg.wallpaper_dir[0] == '/');
        CHECK(strstr(cfg.wallpaper_dir, "Pictures") != NULL);
        CHECK(strchr(cfg.wallpaper_dir, '~') == NULL);
    }
    config_free(&cfg);
    drop_config();
}

static void test_option_table(void) {
    CASE("the runtime option table");
    int count = 0;
    const ConfigOption *opts = config_options(&count);
    CHECK_NOT_NULL(opts);
    CHECK(count > 0);

    /* Every advertised option must be findable by its own name, or `fwmctl
     * set` would reject a name the table itself lists. */
    int missing = 0;
    for (int i = 0; i < count; i++)
        if (!config_option_find(opts[i].name)) missing++;
    CHECK_INT(missing, 0);

    CHECK_NULL(config_option_find("definitely_not_an_option"));
    CHECK_NULL(config_option_find(""));
}

int main(void) {
    test_missing_file();
    test_values_parse();
    test_colors();
    test_bad_input();
    test_error_cap();
    test_tilde_expansion();
    test_option_table();
    return t_report("config");
}
