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

/* Input, as far as it can be tested without a compositor.
 *
 * Real key and button events cannot be injected: the wlroots 0.20 headless
 * backend adds outputs but no input devices, and fwm implements neither
 * virtual-keyboard-v1 nor virtual-pointer-v1, so nothing can feed it events
 * from outside. What is testable is the decision each path makes once an
 * event has arrived — which bind a key resolves to, and what a drag turns
 * into when it is released. Both were moved behind plain functions for
 * exactly that reason.
 *
 * The wlroots plumbing between the two — listener registration, seat state,
 * focus follow — remains uncovered here and is what the dynamic harness
 * exercises instead. */

#include "test.h"
#include "config.h"
#include "physics.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

/* ── keyboard: which bind does a key resolve to ───────────────────────── */

static char tmp_path[256];

static const char *write_config(const char *body) {
    snprintf(tmp_path, sizeof tmp_path, "/tmp/fwm-test-input-%d.toml", (int)getpid());
    FILE *f = fopen(tmp_path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", tmp_path); exit(2); }
    fputs(body, f);
    fclose(f);
    return tmp_path;
}

/* Binds are matched against what the parser produced, not a hand-built table,
 * so a change in how "super+q" is read shows up here too. */
static void load_binds(FwmConfig *cfg) {
    config_load(cfg, write_config(
        "[binds]\n"
        "\"super+q\"       = \"killclient\"\n"
        "\"super+shift+q\" = \"toggle_tiling\"\n"
        "\"super+h\"       = \"move_camera:-50\"\n"
        "\"alt+Return\"    = \"launcher\"\n"
        "\"F1\"            = \"show_hints\"\n"));
}

static const char *action_of(const KeyBind *b) { return b ? b->action : "(none)"; }

static void test_bind_matching(void) {
    FwmConfig cfg;
    load_binds(&cfg);

    CASE("exact key and modifier match");
    CHECK_INT(cfg.error_count, 0);
    CHECK_STR(action_of(config_match_bind(&cfg, XKB_KEY_q, FWM_MOD_LOGO)), "killclient");
    CHECK_STR(action_of(config_match_bind(&cfg, XKB_KEY_h, FWM_MOD_LOGO)), "move_camera:-50");
    CHECK_STR(action_of(config_match_bind(&cfg, XKB_KEY_Return, FWM_MOD_ALT)), "launcher");

    CASE("a bind with no modifier needs no modifier");
    CHECK_STR(action_of(config_match_bind(&cfg, XKB_KEY_F1, 0)), "show_hints");
    CHECK_NULL(config_match_bind(&cfg, XKB_KEY_F1, FWM_MOD_LOGO));

    /* This is a fixed bug, not a nicety. xkb reports the shifted keysym while
     * Caps Lock is on, so 'q' arrives as 'Q'; binds come from lowercase config
     * text. Before normalising, every letter bind died the moment Caps Lock
     * was pressed while digits and Return kept working. */
    CASE("Caps Lock does not break letter binds");
    CHECK_STR(action_of(config_match_bind(&cfg, XKB_KEY_Q, FWM_MOD_LOGO)), "killclient");
    CHECK_STR(action_of(config_match_bind(&cfg, XKB_KEY_H, FWM_MOD_LOGO)), "move_camera:-50");

    /* Modifiers are compared for equality, not containment: holding an extra
     * modifier selects a different bind rather than falling back to the
     * looser one. That is what makes super+q and super+shift+q distinct. */
    CASE("modifiers must match exactly");
    CHECK_STR(action_of(config_match_bind(&cfg, XKB_KEY_q, FWM_MOD_LOGO | FWM_MOD_SHIFT)),
              "toggle_tiling");
    CHECK_NULL(config_match_bind(&cfg, XKB_KEY_q, FWM_MOD_LOGO | FWM_MOD_CTRL));
    CHECK_NULL(config_match_bind(&cfg, XKB_KEY_q, 0));
    CHECK_NULL(config_match_bind(&cfg, XKB_KEY_q, FWM_MOD_ALT));

    CASE("keys that are not bound");
    CHECK_NULL(config_match_bind(&cfg, XKB_KEY_z, FWM_MOD_LOGO));
    CHECK_NULL(config_match_bind(&cfg, XKB_KEY_F2, 0));

    CASE("no config at all");
    CHECK_NULL(config_match_bind(NULL, XKB_KEY_q, FWM_MOD_LOGO));

    config_free(&cfg);
    unlink(tmp_path);
}

static void test_builtin_binds_match(void) {
    /* The fallback table has to be searchable by the same lookup, or a broken
     * config would leave a keyboard that reports binds but answers to none. */
    CASE("built-in binds are reachable through the same lookup");
    FwmConfig cfg;
    config_load(&cfg, "/nonexistent/fwm/none.toml");
    CHECK_INT(cfg.fallback_binds, 1);
    CHECK(cfg.key_count > 0);

    int reachable = 0;
    for (int i = 0; i < cfg.key_count; i++)
        if (config_match_bind(&cfg, cfg.keys[i].key, cfg.keys[i].mod)) reachable++;
    CHECK_INT(reachable, cfg.key_count);
    config_free(&cfg);
}

static void test_repeatable(void) {
    /* Only camera movement repeats while held. Anything else repeating would
     * fire an action per repeat tick — closing a stack of windows, say. */
    CASE("which actions repeat while held");
    CHECK_INT(config_action_is_repeatable("move_camera:-50"), 1);
    CHECK_INT(config_action_is_repeatable("move_camera:1"), 1);
    CHECK(!config_action_is_repeatable("killclient"));
    CHECK(!config_action_is_repeatable("toggle_tiling"));
    CHECK(!config_action_is_repeatable("move_to:3"));
    CHECK(!config_action_is_repeatable("move_camera"));   /* prefix only, no arg */
    CHECK(!config_action_is_repeatable(""));
    CHECK(!config_action_is_repeatable(NULL));
}

/* ── pointer: what a drag turns into when released ────────────────────── */

static PhysicsWorld *world_with_body(uint32_t id) {
    PhysicsWorld *w = calloc(1, sizeof *w);
    physics_init(w);
    w->throw_speed_multiplier = 0.5;
    w->max_throw_speed        = 1000.0;
    physics_sync_body(w, id, 100, 100, 200, 150, 1920);
    return w;
}

static void world_free(PhysicsWorld *w) { physics_destroy(w); free(w); }

static void test_throw(void) {
    CASE("release velocity is scaled, not passed through");
    PhysicsWorld *w = world_with_body(1);
    physics_throw_body(w, 1, 100.0, 40.0);
    PhysicsBody *b = physics_find_body(w, 1);
    CHECK_NOT_NULL(b);
    if (b) {
        CHECK_DBL(b->vx, 50.0, 1e-9);      /* 100 * 0.5 */
        CHECK_DBL(b->vy, 20.0, 1e-9);
        CHECK_INT(b->flying, 1);
    }

    /* A fast flick must not become an arbitrarily fast window — continuous
     * collision only helps if the speed stays inside what the solver was
     * tuned for. Clamping has to keep the direction, or a hard throw would
     * also veer off the line the user dragged along. */
    CASE("a fast flick is clamped but keeps its direction");
    physics_throw_body(w, 1, 100000.0, 0.0);
    b = physics_find_body(w, 1);
    if (b) {
        CHECK_DBL(b->vx, 1000.0, 1e-6);
        CHECK_DBL(b->vy, 0.0, 1e-9);
    }

    physics_throw_body(w, 1, 30000.0, 40000.0);   /* 3:4:5 triangle */
    b = physics_find_body(w, 1);
    if (b) {
        double speed = sqrt(b->vx * b->vx + b->vy * b->vy);
        CHECK_DBL(speed, 1000.0, 1e-6);
        CHECK_DBL(b->vy / b->vx, 4.0 / 3.0, 1e-6);   /* direction preserved */
    }

    CASE("throwing a window that is not there");
    physics_throw_body(w, 999, 10.0, 10.0);        /* must not crash */
    CHECK_NULL(physics_find_body(w, 999));

    CASE("stop clears the throw");
    physics_stop_body(w, 1);
    b = physics_find_body(w, 1);
    if (b) {
        CHECK_DBL(b->vx, 0.0, 1e-9);
        CHECK_DBL(b->vy, 0.0, 1e-9);
    }
    world_free(w);
}

static void test_body_slots(void) {
    CASE("a body is found by id and forgotten when removed");
    PhysicsWorld *w = calloc(1, sizeof *w);
    physics_init(w);
    physics_sync_body(w, 1, 0, 0, 100, 100, 1920);
    physics_sync_body(w, 2, 0, 0, 100, 100, 1920);
    CHECK_NOT_NULL(physics_find_body(w, 1));
    CHECK_NOT_NULL(physics_find_body(w, 2));
    CHECK_NULL(physics_find_body(w, 3));
    physics_remove_body(w, 1);
    CHECK_NULL(physics_find_body(w, 1));
    CHECK_NOT_NULL(physics_find_body(w, 2));

    CASE("syncing the same id twice reuses one slot");
    int before = w->body_count;
    physics_sync_body(w, 2, 10, 10, 100, 100, 1920);
    CHECK_INT(w->body_count, before);

    /* bodies[] is MAX_WINDOWS long and windows past it get no body at all.
     * Asking for more must degrade to "no body" rather than write past the
     * end — the array is fixed and the ids come from however many windows the
     * user opened. */
    CASE("asking for more bodies than the array holds");
    for (uint32_t id = 100; id < 100 + MAX_WINDOWS + 40; id++)
        physics_sync_body(w, id, 0, 0, 50, 50, 1920);
    CHECK(w->body_count <= MAX_WINDOWS);
    world_free(w);
}

int main(void) {
    test_bind_matching();
    test_builtin_binds_match();
    test_repeatable();
    test_throw();
    test_body_slots();
    return t_report("input");
}
