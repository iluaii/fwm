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
#include <math.h>
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

    CASE("the built-in binds are all understood");
    /* apply_default_binds runs whenever the file is unusable, so a typo in that
     * table would be found only on a machine with a broken config. */
    p = write_config("");
    config_load(&cfg, p);
    CHECK_INT(cfg.fallback_binds, 1);
    const KeyBind *kb = config_match_bind(&cfg, XKB_KEY_Return, FWM_MOD_LOGO);
    CHECK_NOT_NULL(kb);
    CHECK_STR(kb->action, "terminal");   /* not a hard-coded emulator name */
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

static void test_input_touchpad(void) {
    CASE("touchpad knobs default to libinput, except tap");
    const char *p = write_config("[binds]\n\"super+q\" = \"killclient\"\n");
    FwmConfig cfg;
    config_load(&cfg, p);
    /* -1 means "say nothing to libinput"; tap is the one fwm insists on, or a
     * laptop with no mouse cannot click at all. */
    CHECK_INT(cfg.input.tap, 1);
    CHECK_INT(cfg.input.tap_drag, -1);
    CHECK_INT(cfg.input.natural_scroll, -1);
    CHECK_INT(cfg.input.dwt, -1);
    CHECK_INT(cfg.input.left_handed, -1);
    CHECK_DBL(cfg.input.accel_speed, INPUT_ACCEL_UNSET, 1e-9);
    CHECK_STR(cfg.input.accel_profile, "");
    CHECK_STR(cfg.input.scroll_method, "");
    config_free(&cfg);
    drop_config();

    CASE("touchpad knobs are read, including turning tap off");
    p = write_config(
        "[input]\n"
        "tap = false\n"
        "natural_scroll = true\n"
        "accel_speed = -0.3\n"
        "accel_profile = \"flat\"\n"
        "scroll_method = \"edge\"\n"
        "click_method = \"clickfinger\"\n"
        "[binds]\n\"super+q\" = \"killclient\"\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.input.tap, 0);
    CHECK_INT(cfg.input.natural_scroll, 1);
    CHECK_DBL(cfg.input.accel_speed, -0.3, 1e-9);
    CHECK_STR(cfg.input.accel_profile, "flat");
    CHECK_STR(cfg.input.scroll_method, "edge");
    CHECK_STR(cfg.input.click_method, "clickfinger");
    CHECK_INT(cfg.error_count, 0);
    config_free(&cfg);
    drop_config();

    CASE("bad touchpad values are reported and left at the default");
    p = write_config(
        "[input]\n"
        "accel_speed = 4.0\n"            /* libinput's range is -1..1 */
        "accel_profile = \"turbo\"\n"
        "scroll_method = \"three_finger\"\n"
        "[binds]\n\"super+q\" = \"killclient\"\n");
    config_load(&cfg, p);
    CHECK_DBL(cfg.input.accel_speed, INPUT_ACCEL_UNSET, 1e-9);
    CHECK_STR(cfg.input.accel_profile, "");
    CHECK_STR(cfg.input.scroll_method, "");
    CHECK_INT(cfg.error_count, 3);
    CHECK_INT(cfg.fallback_binds, 0);   /* still a working config */
    config_free(&cfg);
    drop_config();
}

/* Find a gesture bind the way gestures.c does, without linking it in. */
static const char *gesture_action(const FwmConfig *cfg, int fingers, int dir) {
    for (int i = 0; i < cfg->gestures.bind_count; i++)
        if (cfg->gestures.binds[i].fingers == fingers &&
            cfg->gestures.binds[i].dir == dir)
            return cfg->gestures.binds[i].action;
    return NULL;
}

static void test_gestures(void) {
    CASE("no [gestures] section means no gestures at all");
    /* Unlike [binds], nothing stands in: a gesture nobody asked for takes a
     * swipe away from the application under the cursor. */
    const char *p = write_config("[binds]\n\"super+q\" = \"killclient\"\n");
    FwmConfig cfg;
    config_load(&cfg, p);
    CHECK_INT(cfg.gestures.bind_count, 0);
    CHECK_NULL(gesture_action(&cfg, 3, GESTURE_SWIPE_LEFT));
    /* The scalars still have their defaults, for whenever binds do arrive. */
    CHECK_DBL(cfg.gestures.sensitivity, 1.0, 1e-9);
    CHECK_INT(cfg.gestures.natural, 1);
    config_free(&cfg);
    drop_config();

    CASE("a [gestures] section binds exactly what it lists");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[gestures]\n"
        "sensitivity = 1.5\n"
        "natural = false\n"
        "\"swipe4+up\" = \"launcher\"\n"
        "\"pinch3+in\" = \"calm_all\"\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.gestures.bind_count, 2);
    CHECK_STR(gesture_action(&cfg, 4, GESTURE_SWIPE_UP), "launcher");
    CHECK_STR(gesture_action(&cfg, 3, GESTURE_PINCH_IN), "calm_all");
    CHECK_NULL(gesture_action(&cfg, 3, GESTURE_SWIPE_LEFT)); /* never bound */
    CHECK_DBL(cfg.gestures.sensitivity, 1.5, 1e-9);
    CHECK_INT(cfg.gestures.natural, 0);
    config_free(&cfg);
    drop_config();

    CASE("broken gesture lines are reported, not obeyed");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[gestures]\n"
        "\"swipe9+left\" = \"launcher\"\n"    /* no such finger count */
        "\"swipe3+sideways\" = \"launcher\"\n"/* no such direction */
        "\"wave3+left\" = \"launcher\"\n"     /* no such gesture */
        "\"swipe3+up\" = \"not_an_action\"\n" /* no such action */
        "\"swipe3+down\" = \"pan_desktop\"\n" /* the strip is horizontal */
        "\"swipe3+right\" = \"pan_desktop\"\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.gestures.bind_count, 1);
    CHECK_STR(gesture_action(&cfg, 3, GESTURE_SWIPE_RIGHT), GESTURE_ACTION_PAN);
    CHECK(cfg.error_count >= 5);
    /* A file this broken is still a working config: the keyboard is untouched. */
    CHECK_INT(cfg.fallback_binds, 0);
    CHECK(cfg.key_count > 0);
    config_free(&cfg);
    drop_config();
}

/* Per-window material and per-desktop profiles. Both use the same trick — a
 * value the file may simply not mention — and both get it wrong in the same
 * way if the "unset" state is ever confused with a real number, so the tests
 * lean on the cases where 0 and -1 are meaningful values. */
static void test_rule_material(void) {
    CASE("[[rule]] material properties");
    const char *p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[[rule]]\n"
        "app_id   = \"^mpv$\"\n"
        "mass     = 8\n"          /* an int where a double is expected */
        "bounce   = 0.0\n"        /* 0 is a value, not silence */
        "[[rule]]\n"
        "app_id   = \"^balloon$\"\n"
        "gravity  = -0.2\n"
        "friction = 0.9\n");
    FwmConfig cfg;
    config_load(&cfg, p);
    CHECK_INT(cfg.error_count, 0);
    CHECK_INT(cfg.rule_count, 2);

    ConfigRule out;
    CHECK_INT(config_match_rules(&cfg, "mpv", "whatever", &out), 1);
    CHECK_DBL(out.mass, 8.0, 1e-9);
    CHECK_DBL(out.bounce, 0.0, 1e-9);
    CHECK(isnan(out.gravity));    /* this rule said nothing about it */
    CHECK(isnan(out.friction));

    CHECK_INT(config_match_rules(&cfg, "balloon", NULL, &out), 1);
    CHECK_DBL(out.gravity, -0.2, 1e-9);
    CHECK_DBL(out.friction, 0.9, 1e-9);
    CHECK(isnan(out.mass));

    /* A window no rule matches carries no material at all. */
    CHECK_INT(config_match_rules(&cfg, "kitty", NULL, &out), 0);
    CHECK(isnan(out.mass) && isnan(out.gravity) && isnan(out.bounce) && isnan(out.friction));
    config_free(&cfg);
    drop_config();

    CASE("out-of-range material is reported, not obeyed");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[[rule]]\n"
        "app_id = \"^mpv$\"\n"
        "mass   = 0\n"      /* a massless body breaks the solver */
        "bounce = 3.0\n"    /* would gain energy on every bounce */
        "pin    = true\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.rule_count, 1);
    CHECK(cfg.error_count >= 2);
    config_match_rules(&cfg, "mpv", NULL, &out);
    CHECK(isnan(out.mass));
    CHECK(isnan(out.bounce));
    CHECK_INT(out.pin, 1);   /* the rest of the rule still stands */
    config_free(&cfg);
    drop_config();
}

static void test_physics_profiles(void) {
    CASE("[physics.<name>] profiles claim desktops");
    const char *p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[physics]\n"
        "gravity  = 981.0\n"
        "friction = 0.985\n"
        "gravity_steps = [0.0, 1.0]\n"
        "[physics.moon]\n"
        "gravity  = 160.0\n"
        "desktops = [3, 4]\n"
        "[physics.water]\n"
        "friction = 0.9\n"
        "desktops = [7]\n");
    FwmConfig cfg;
    config_load(&cfg, p);
    CHECK_INT(cfg.error_count, 0);
    CHECK_INT(cfg.physics.profile_count, 2);
    CHECK_INT(cfg.physics.gravity_step_count, 2);
    CHECK_DBL(cfg.physics.gravity_steps[1], 1.0, 1e-9);

    /* Unclaimed desktops stay on the world's values. */
    CHECK_INT(cfg.physics.desktop_profile[0], -1);
    CHECK_INT(cfg.physics.desktop_profile[3], cfg.physics.desktop_profile[4]);
    CHECK(cfg.physics.desktop_profile[3] >= 0);

    const PhysicsProfileConfig *moon = &cfg.physics.profiles[cfg.physics.desktop_profile[3]];
    CHECK_STR(moon->name, "moon");
    CHECK_DBL(moon->gravity, 160.0, 1e-9);
    /* A profile is a diff: what it does not write, it inherits. */
    CHECK_DBL(moon->friction, 0.985, 1e-9);

    const PhysicsProfileConfig *water = &cfg.physics.profiles[cfg.physics.desktop_profile[7]];
    CHECK_DBL(water->friction, 0.9, 1e-9);
    CHECK_DBL(water->gravity, 981.0, 1e-9);
    config_free(&cfg);
    drop_config();

    CASE("a profile pointing nowhere is reported");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[physics.moon]\n"
        "gravity  = 160.0\n"
        "desktops = [99]\n");     /* no such desktop, so nothing is claimed */
    config_load(&cfg, p);
    CHECK(cfg.error_count >= 2);  /* the bad index, and the unused profile */
    for (int i = 0; i < FWM_DESKTOPS; i++)
        CHECK_INT(cfg.physics.desktop_profile[i], -1);
    config_free(&cfg);
    drop_config();
}

static void test_mass_mode(void) {
    CASE("[physics] mass picks what a window weighs");
    const char *p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[physics]\n"
        "mass = \"ram\"\n"
        "mass_ram_ref = 512.0\n"
        "mass_ram_max = 8.0\n");
    FwmConfig cfg;
    config_load(&cfg, p);
    CHECK_INT(cfg.error_count, 0);
    CHECK_INT(cfg.physics.mass_mode, PHYSICS_MASS_RAM);
    CHECK_DBL(cfg.physics.mass_ram_ref, 512.0, 1e-9);
    CHECK_DBL(cfg.physics.mass_ram_max, 8.0, 1e-9);
    config_free(&cfg);
    drop_config();

    CASE("mass defaults to the window's size");
    p = write_config("[binds]\n\"super+q\" = \"killclient\"\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.physics.mass_mode, PHYSICS_MASS_SIZE);
    CHECK_DBL(cfg.physics.mass_ram_ref, 300.0, 1e-9);
    config_free(&cfg);
    drop_config();

    /* A typo here is the difference between a heavy browser and a silently
     * ordinary one, so it is reported rather than guessed at. */
    CASE("an unknown mass mode is reported and the default kept");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[physics]\nmass = \"memry\"\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.error_count, 1);
    CHECK_INT(cfg.physics.mass_mode, PHYSICS_MASS_SIZE);
    config_free(&cfg);
    drop_config();

    /* Nonsense limits would divide a weight by nothing, or make the hog the
     * lightest thing on screen. */
    CASE("mass limits are clamped to something usable");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[physics]\nmass_ram_ref = 0.0\nmass_ram_max = 0.2\n");
    config_load(&cfg, p);
    CHECK_DBL(cfg.physics.mass_ram_ref, 1.0, 1e-9);
    CHECK_DBL(cfg.physics.mass_ram_max, 1.0, 1e-9);
    config_free(&cfg);
    drop_config();
}

static void test_every_dispatchable_action_binds(void) {
    /* action_is_known is the gate a bind has to pass at load time, and it is a
     * hand-kept list next to a hand-kept switch in server_actions.c. When the two
     * drift, the symptom is a bind that is REPORTED as unknown and dropped for an
     * action the compositor performs perfectly well — which is how `modes_menu`
     * spent its life documented but unbindable. This is the cheap half of the
     * check: every name the docs promise, bound for real. */
    CASE("documented actions are accepted in [binds]");
    static const char *const actions[] = {
        "killclient", "toggle_tiling", "toggle_split", "toggle_floating",
        "toggle_tiling_all", "toggle_floating_all", "toggle_nocollide",
        "toggle_nocollide_all", "pin_window", "calm_all", "cycle_gravity",
        "spin_window", "spin_all", "fake_fullscreen", "real_fullscreen",
        "group_toggle", "group_next", "group_prev", "group_add",
        "terminal", "launcher", "expo", "toggle_tray", "toggle_wrap",
        "modes_menu", "radial_menu", "show_hints", "show_errors", "reload_config",
        "wallpaper_picker", "screenshot", "screenshot_region",
        "output_off", "outputs_on", "toggle_sun", "sun_mode",
        "toggle_internal_output", "EXIT",
        "view:3", "move_to:7", "move_to_view:next", "move_camera:-50",
        "tile_focus:l", "tile_move:d", "spawn:true", "mode:default",
        "sun_azimuth:+15", "sun_elevation:-5",
        NULL,
    };

    /* One distinct, definitely-existing key per action, across two modifier sets
     * so there are enough of them. Not F1..Fn: xkb stops at F35, and a bind
     * failing on the KEY would look exactly like an action being rejected. */
    static const char keys[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    static const char *const mods[] = { "super+ctrl+shift", "super+alt+shift" };
    const int per_mod = (int)(sizeof(keys) - 1);

    char body[8192];
    int n = snprintf(body, sizeof body, "[binds]\n");
    int count = 0;
    while (actions[count]) count++;
    CHECK(count <= per_mod * (int)(sizeof(mods) / sizeof(mods[0])));
    for (int i = 0; i < count; i++)
        n += snprintf(body + n, sizeof body - (size_t)n, "\"%s+%c\" = \"%s\"\n",
                      mods[i / per_mod], keys[i % per_mod], actions[i]);

    const char *p = write_config(body);
    FwmConfig cfg;
    config_load(&cfg, p);
    /* Not one of them may be reported, and every one must have produced a
     * bind — the count is what catches a silently dropped line. */
    CHECK_INT(cfg.error_count, 0);
    CHECK_INT(cfg.fallback_binds, 0);
    CHECK_INT(cfg.key_count, count);
    config_free(&cfg);
    drop_config();
}

static void test_sound(void) {
    CASE("[sound] is off with sane defaults until asked for");
    const char *p = write_config("[binds]\n\"super+q\" = \"killclient\"\n");
    FwmConfig cfg;
    config_load(&cfg, p);
    CHECK_INT(cfg.sound.collisions, 0);
    CHECK_STR(cfg.sound.path, "");
    CHECK_DBL(cfg.sound.volume, 0.6, 1e-9);
    CHECK(cfg.sound.max_speed > cfg.sound.min_speed);
    config_free(&cfg);
    drop_config();

    CASE("[sound] values are read");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[sound]\n"
        "collisions = true\n"
        "volume     = 0.25\n"
        "min_speed  = 150.0\n"
        "max_speed  = 900.0\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.error_count, 0);
    CHECK_INT(cfg.sound.collisions, 1);
    CHECK_DBL(cfg.sound.volume, 0.25, 1e-9);
    CHECK_DBL(cfg.sound.min_speed, 150.0, 1e-9);
    CHECK_DBL(cfg.sound.max_speed, 900.0, 1e-9);
    config_free(&cfg);
    drop_config();

    /* A path that is not there is the likeliest thing to get wrong, and it must
     * not leave the feature silently playing the click while the user believes
     * their own file is being used. */
    CASE("an unreadable sample is reported and dropped");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[sound]\ncollisions = true\npath = \"/nonexistent/fwm/knock.wav\"\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.error_count, 1);
    CHECK_STR(cfg.sound.path, "");     /* fall back to the built-in click */
    CHECK_INT(cfg.sound.collisions, 1);  /* ... but the feature stays on */
    config_free(&cfg);
    drop_config();

    CASE("an inverted speed range is reported and repaired");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[sound]\nmin_speed = 800.0\nmax_speed = 100.0\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.error_count, 1);
    CHECK(cfg.sound.max_speed > cfg.sound.min_speed);
    config_free(&cfg);
    drop_config();

    CASE("volume is clamped");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[sound]\nvolume = 4.0\n");
    config_load(&cfg, p);
    CHECK_DBL(cfg.sound.volume, 1.0, 1e-9);
    config_free(&cfg);
    drop_config();
}

static void test_mouse(void) {
    CASE("[mouse] defaults reproduce the old hard-coded drags");
    const char *p = write_config("[binds]\n\"super+q\" = \"killclient\"\n");
    FwmConfig cfg;
    config_load(&cfg, p);
    const MouseBind *mb = config_match_mouse(&cfg, FWM_BTN_LEFT, FWM_MOD_LOGO);
    CHECK_NOT_NULL(mb);
    CHECK_STR(mb->action, FWM_MOUSE_MOVE);
    mb = config_match_mouse(&cfg, FWM_BTN_LEFT, FWM_MOD_LOGO | FWM_MOD_SHIFT);
    CHECK_NOT_NULL(mb);
    CHECK_STR(mb->action, FWM_MOUSE_MOVE_NOCOLLIDE);
    mb = config_match_mouse(&cfg, FWM_BTN_RIGHT, FWM_MOD_LOGO);
    CHECK_NOT_NULL(mb);
    CHECK_STR(mb->action, FWM_MOUSE_RESIZE);
    /* Exact modifier match, as for keys: a chord nobody bound stays the
     * client's. */
    CHECK_NULL(config_match_mouse(&cfg, FWM_BTN_LEFT, FWM_MOD_LOGO | FWM_MOD_CTRL));
    CHECK_NULL(config_match_mouse(&cfg, FWM_BTN_LEFT, 0));
    config_free(&cfg);
    drop_config();

    CASE("a [mouse] section replaces them wholesale");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[mouse]\n"
        "\"super+ctrl+left\" = \"twist\"\n"
        "\"super+middle\"    = \"killclient\"\n");   /* an ordinary action */
    config_load(&cfg, p);
    CHECK_INT(cfg.error_count, 0);
    CHECK_INT(cfg.mouse.bind_count, 2);
    mb = config_match_mouse(&cfg, FWM_BTN_LEFT, FWM_MOD_LOGO | FWM_MOD_CTRL);
    CHECK_NOT_NULL(mb);
    CHECK_STR(mb->action, FWM_MOUSE_TWIST);
    CHECK_INT(config_action_is_drag(mb->action), 1);
    mb = config_match_mouse(&cfg, FWM_BTN_MIDDLE, FWM_MOD_LOGO);
    CHECK_NOT_NULL(mb);
    CHECK_INT(config_action_is_drag(mb->action), 0);
    /* The defaults are gone: the table is the whole truth about the mouse. */
    CHECK_NULL(config_match_mouse(&cfg, FWM_BTN_LEFT, FWM_MOD_LOGO));
    config_free(&cfg);
    drop_config();

    CASE("broken [mouse] lines are reported, not obeyed");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[mouse]\n"
        "\"super+wheel\"   = \"move\"\n"          /* no such button */
        "\"hyper+left\"    = \"move\"\n"          /* no such modifier */
        "\"super+right\"   = \"not_an_action\"\n" /* no such action */
        "\"super+side\"    = \"launcher\"\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.mouse.bind_count, 1);
    CHECK(cfg.error_count >= 3);
    mb = config_match_mouse(&cfg, FWM_BTN_SIDE, FWM_MOD_LOGO);
    CHECK_NOT_NULL(mb);
    CHECK_STR(mb->action, "launcher");
    CHECK(cfg.key_count > 0);   /* still a working config */
    config_free(&cfg);
    drop_config();
}

static void test_modes(void) {
    CASE("[mode.<name>] submaps");
    const char *p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[mode.physics]\n"
        "enter = \"super+o\"\n"
        "\"g\" = \"cycle_gravity\"\n"
        "\"c\" = \"calm_all\"\n"
        "[mode.resize]\n"
        "enter  = \"super+r\"\n"
        "sticky = true\n"
        "\"shift+Left\" = \"tile_move:l\"\n");
    FwmConfig cfg;
    config_load(&cfg, p);
    CHECK_INT(cfg.error_count, 0);
    CHECK_INT(cfg.mode_count, 2);

    int m = config_mode_find(&cfg, "physics");
    CHECK(m >= 0);
    /* The prefixed form is what a dispatched action carries. */
    CHECK_INT(config_mode_find(&cfg, "mode:physics"), m);
    CHECK_INT(config_mode_find(&cfg, "nope"), -1);
    CHECK_INT(cfg.modes[m].sticky, 0);      /* one-shot unless asked otherwise */
    CHECK_INT(cfg.modes[m].key_count, 2);   /* `enter` is not one of its binds */

    const KeyBind *kb = config_match_mode_bind(&cfg, m, XKB_KEY_g, 0);
    CHECK_NOT_NULL(kb);
    CHECK_STR(kb->action, "cycle_gravity");
    /* Exact modifiers here too: the whole point of a mode is that bare keys
     * mean something, so super+g inside it is a different bind. */
    CHECK_NULL(config_match_mode_bind(&cfg, m, XKB_KEY_g, FWM_MOD_LOGO));
    CHECK_NULL(config_match_mode_bind(&cfg, m, XKB_KEY_x, 0));

    int r = config_mode_find(&cfg, "resize");
    CHECK_INT(cfg.modes[r].sticky, 1);
    CHECK_NOT_NULL(config_match_mode_bind(&cfg, r, XKB_KEY_Left, FWM_MOD_SHIFT));

    /* `enter` lands in the ROOT map as a mode: action, so the key that opens a
     * submap is an ordinary bind like any other. */
    kb = config_match_bind(&cfg, XKB_KEY_o, FWM_MOD_LOGO);
    CHECK_NOT_NULL(kb);
    CHECK_STR(kb->action, "mode:physics");
    config_free(&cfg);
    drop_config();

    CASE("an empty mode is dropped, and its enter key with it");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[mode.empty]\n"
        "enter = \"super+o\"\n"
        "[mode.broken]\n"
        "enter = \"super+i\"\n"
        "\"nosuchkey\" = \"calm_all\"\n"
        "\"g\" = \"not_an_action\"\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.mode_count, 0);
    CHECK(cfg.error_count >= 3);
    /* No way in, so no way to get stuck in a mode that does nothing. */
    CHECK_NULL(config_match_bind(&cfg, XKB_KEY_o, FWM_MOD_LOGO));
    CHECK_NULL(config_match_bind(&cfg, XKB_KEY_i, FWM_MOD_LOGO));
    CHECK(cfg.key_count > 0);
    config_free(&cfg);
    drop_config();
}

static void test_radial(void) {
    CASE("[radial] and its items");
    const char *p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[radial]\n"
        "enter  = \"super+shift+XF86AudioMute\"\n"
        "radius = 240\n"
        "center_text = \"fwm\"\n"
        "[[radial.item]]\n"
        "label  = \"Terminal\"\n"
        "icon   = \"foot\"\n"
        "action = \"spawn:foot\"\n"
        "[[radial.item]]\n"
        "label  = \"Physics\"\n"
        "text   = \"G\"\n"
        "action = \"cycle_gravity\"\n");
    FwmConfig cfg;
    config_load(&cfg, p);
    CHECK_INT(cfg.error_count, 0);
    CHECK_INT(cfg.radial.item_count, 2);
    CHECK_DBL(cfg.radial.radius, 240.0, 1e-9);
    CHECK_STR(cfg.radial.center_text, "fwm");
    /* File order is ring order, clockwise from the top. */
    CHECK_STR(cfg.radial.items[0].label, "Terminal");
    CHECK_STR(cfg.radial.items[0].action, "spawn:foot");
    CHECK_STR(cfg.radial.items[1].text, "G");

    /* `enter` lands in the ROOT map, exactly as a mode's does — the knob's own
     * press with the modifiers held, which is the whole reason this exists. */
    const KeyBind *kb = config_match_bind(&cfg, XKB_KEY_XF86AudioMute,
                                          FWM_MOD_LOGO | FWM_MOD_SHIFT);
    CHECK_NOT_NULL(kb);
    CHECK_STR(kb->action, "radial_menu");
    /* And it is a chord: the bare mute key is still the volume key it was. */
    CHECK_NULL(config_match_bind(&cfg, XKB_KEY_XF86AudioMute, 0));
    config_free(&cfg);
    drop_config();

    CASE("no [radial] leaves an empty ring and no bind");
    p = write_config("[binds]\n\"super+q\" = \"killclient\"\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.error_count, 0);
    CHECK_INT(cfg.radial.item_count, 0);
    CHECK_DBL(cfg.radial.radius, 190.0, 1e-9);
    config_free(&cfg);
    drop_config();

    CASE("broken items are dropped, the rest of the ring survives");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[radial]\n"
        "enter  = \"super+shift+XF86AudioMute\"\n"
        "radius = 5000\n"
        "[[radial.item]]\n"
        "label = \"no action at all\"\n"
        "[[radial.item]]\n"
        "action = \"not_an_action\"\n"
        "[[radial.item]]\n"
        "action = \"calm_all\"\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.radial.item_count, 1);
    CHECK(cfg.error_count >= 3);          /* two items and the radius */
    CHECK_DBL(cfg.radial.radius, 190.0, 1e-9);
    /* An item with nothing to show falls back to naming its own action, so a
     * half-written petal is visibly half-written rather than a blank circle. */
    CHECK_STR(cfg.radial.items[0].label, "calm_all");
    CHECK_NOT_NULL(config_match_bind(&cfg, XKB_KEY_XF86AudioMute,
                                     FWM_MOD_LOGO | FWM_MOD_SHIFT));
    config_free(&cfg);
    drop_config();

    CASE("the ring has a cap");
    char body[4096];
    int n = snprintf(body, sizeof body,
                     "[binds]\n\"super+q\" = \"killclient\"\n[radial]\n");
    for (int i = 0; i < CONFIG_MAX_RADIAL + 3; i++)
        n += snprintf(body + n, sizeof body - (size_t)n,
                      "[[radial.item]]\naction = \"calm_all\"\n");
    p = write_config(body);
    config_load(&cfg, p);
    CHECK_INT(cfg.radial.item_count, CONFIG_MAX_RADIAL);
    CHECK(cfg.error_count >= 1);
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

/* config_option_check answers the same question config_option_set does and
 * writes nothing. It is what lets `fwmctl set a=1 b=2 c=nonsense` be refused
 * whole instead of leaving the first two applied, so the property that matters
 * is not "it validates" but "it validates IDENTICALLY and touches nothing". */
static void test_option_check(void) {
    CASE("checking a value without storing it");
    FwmConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    config_load(&cfg, "/nonexistent/fwm-test.toml");

    const ConfigOption *grav = config_option_find("physics.gravity");
    const ConfigOption *blur = config_option_find("sun.blur");
    const ConfigOption *col  = config_option_find("decor.col_active");
    CHECK_NOT_NULL(grav);
    CHECK_NOT_NULL(blur);
    CHECK_NOT_NULL(col);

    char err[192];
    double before = cfg.physics.gravity;
    CHECK(config_option_check(grav, "123.5", err, sizeof(err)));
    CHECK_DBL(cfg.physics.gravity, before, 1e-12);   /* checked, not stored */

    /* The same three refusals `set` makes, in the same words. */
    CHECK(!config_option_check(blur, "banana", err, sizeof(err)));
    CHECK(!config_option_check(blur, "-1", err, sizeof(err)));       /* below min */
    CHECK(!config_option_check(blur, "999", err, sizeof(err)));      /* above max */
    CHECK(!config_option_check(blur, "", err, sizeof(err)));
    CHECK(!config_option_check(col, "not-a-colour", err, sizeof(err)));
    CHECK(config_option_check(col, "#ff8800", err, sizeof(err)));

    /* And a value that passes the check is one set then accepts — the two must
     * never be able to disagree about the same string. */
    CHECK(config_option_set(&cfg, blur, "12", err, sizeof(err)));
    CHECK_DBL(cfg.sun.blur, 12.0, 1e-12);

    config_free(&cfg);
}

/* Mode and transform strings. The file and `fwmctl output` share these two
 * parsers precisely so both take the same spelling, which makes them the one
 * place a typo in either has to be caught. */
static void test_output_spellings(void) {
    CASE("mode strings");
    int w = 0, h = 0, r = 0;

    CHECK(config_parse_mode("1920x1080", &w, &h, &r));
    CHECK_INT(w, 1920); CHECK_INT(h, 1080);
    CHECK_INT(r, 0);                       /* no refresh asked for */

    CHECK(config_parse_mode("2560x1440@144", &w, &h, &r));
    CHECK_INT(w, 2560); CHECK_INT(h, 1440); CHECK_INT(r, 144000);

    /* The fractional rates real monitors advertise must round to the mHz the
     * mode list holds, or an exact match would never be found. */
    CHECK(config_parse_mode("1920x1080@59.94", &w, &h, &r));
    CHECK_INT(r, 59940);

    CHECK(!config_parse_mode("1920", &w, &h, &r));
    CHECK(!config_parse_mode("1920x", &w, &h, &r));
    CHECK(!config_parse_mode("1920x1080@", &w, &h, &r));
    CHECK(!config_parse_mode("1920x1080@60junk", &w, &h, &r));
    CHECK(!config_parse_mode("1920x1080@0", &w, &h, &r));
    CHECK(!config_parse_mode("32x32", &w, &h, &r));        /* absurdly small */
    CHECK(!config_parse_mode("99999x1080", &w, &h, &r));   /* absurdly wide */
    CHECK(!config_parse_mode("", &w, &h, &r));
    CHECK(!config_parse_mode(NULL, &w, &h, &r));
    /* A rejected string leaves the caller's values alone: apply-nothing is
     * what makes a bad key in [[output]] cost only that key. */
    CHECK_INT(w, 1920); CHECK_INT(h, 1080); CHECK_INT(r, 59940);

    CASE("transform names");
    CHECK_INT(config_parse_transform("normal"), 0);
    CHECK_INT(config_parse_transform("0"), 0);
    CHECK_INT(config_parse_transform("90"), 1);
    CHECK_INT(config_parse_transform("270"), 3);
    CHECK_INT(config_parse_transform("flipped-180"), 6);
    CHECK_INT(config_parse_transform("sideways"), -1);
    CHECK_INT(config_parse_transform(""), -1);
    CHECK_INT(config_parse_transform(NULL), -1);
    CHECK_STR(config_transform_name(1), "90");
    CHECK_STR(config_transform_name(99), "?");
    CHECK_STR(config_transform_name(-1), "?");
}

static void test_outputs(void) {
    CASE("[[output]] entries");
    const char *p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[[output]]\n"
        "name = \"eDP-1\"\n"
        "x = 0\ny = 0\n"
        "desktop = 2\n"
        "mode = \"1366x768@60\"\n"
        "scale = 1.5\n"
        "transform = \"90\"\n"
        "[[output]]\n"
        "name = \"HDMI-A-1\"\n"
        "scale = 2\n"                      /* an integer, which TOML types apart */
        "enabled = false\n");
    FwmConfig cfg;
    config_load(&cfg, p);

    CHECK_INT(cfg.output_count, 2);
    const ConfigOutput *o = config_find_output(&cfg, "eDP-1");
    CHECK_NOT_NULL(o);
    CHECK_INT(o->have_pos, 1);
    CHECK_INT(o->desktop, 2);
    CHECK_INT(o->enabled, 1);
    CHECK_INT(o->have_mode, 1);
    CHECK_INT(o->mode_w, 1366);
    CHECK_INT(o->mode_h, 768);
    CHECK_INT(o->mode_refresh, 60000);
    CHECK_DBL(o->scale, 1.5, 1e-9);
    CHECK_INT(o->transform, 1);

    o = config_find_output(&cfg, "HDMI-A-1");
    CHECK_NOT_NULL(o);
    CHECK_DBL(o->scale, 2.0, 1e-9);
    CHECK_INT(o->enabled, 0);
    /* Everything it did not say stays unset, so this entry only turns a screen
     * off — it does not also decide its resolution. */
    CHECK_INT(o->have_mode, 0);
    CHECK_INT(o->have_pos, 0);
    CHECK_INT(o->transform, -1);
    CHECK_INT(o->desktop, -1);

    CHECK_NULL(config_find_output(&cfg, "DP-9"));
    CHECK_INT(cfg.error_count, 0);
    config_free(&cfg);
    drop_config();

    CASE("a monitor setting fwm cannot honour is dropped, not obeyed");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[[output]]\n"
        "name = \"eDP-1\"\n"
        "mode = \"huge\"\n"
        "scale = 0\n"
        "transform = \"upside-down\"\n");
    config_load(&cfg, p);
    o = config_find_output(&cfg, "eDP-1");
    CHECK_NOT_NULL(o);
    CHECK_INT(o->have_mode, 0);
    CHECK_DBL(o->scale, 0.0, 1e-9);
    CHECK_INT(o->transform, -1);
    CHECK_INT(cfg.error_count, 3);
    /* The entry survives its bad keys: the monitor is still named, so a screen
     * with one typo'd line is not a screen fwm has never heard of. */
    CHECK_INT(cfg.output_count, 1);
    config_free(&cfg);
    drop_config();
}

static void test_stats(void) {
    CASE("[stats] defaults to the three built-in sensors");
    const char *p = write_config("[binds]\n\"super+q\" = \"killclient\"\n");
    FwmConfig cfg;
    config_load(&cfg, p);
    CHECK_INT(cfg.stats.item_count, 3);
    CHECK_STR(cfg.stats.items[0], "cpu");
    CHECK_STR(cfg.stats.items[1], "ram");
    CHECK_STR(cfg.stats.items[2], "gpu");
    CHECK_INT(cfg.stats.custom_count, 0);
    config_free(&cfg);
    drop_config();

    /* The whole point of the one-line shape: a key that is not one of the
     * table's own settings IS a sensor, and naming it in `items` shows it. */
    CASE("a bare key defines a custom sensor");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[stats]\n"
        "items = [\"vol\", \"cpu\"]\n"
        "vol = \"echo 60%\"\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.error_count, 0);
    CHECK_INT(cfg.stats.custom_count, 1);
    CHECK_STR(cfg.stats.custom[0].name, "vol");
    CHECK_STR(cfg.stats.custom[0].cmd, "echo 60%");
    /* Order is the user's, not the built-ins-first order of the default. */
    CHECK_INT(cfg.stats.item_count, 2);
    CHECK_STR(cfg.stats.items[0], "vol");
    CHECK_STR(cfg.stats.items[1], "cpu");
    config_free(&cfg);
    drop_config();

    CASE("the command may be written after the items that use it");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[stats]\nitems = [\"mic\"]\nmic = \"echo on\"\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.error_count, 0);
    CHECK_INT(cfg.stats.item_count, 1);
    config_free(&cfg);
    drop_config();

    /* A misspelt sensor and a sensor with nothing to say look identical in the
     * tray, so the typo has to be said out loud. */
    CASE("an unknown name is reported and dropped");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[stats]\nitems = [\"cpu\", \"bogus\"]\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.error_count, 1);
    CHECK_INT(cfg.stats.item_count, 1);
    CHECK_STR(cfg.stats.items[0], "cpu");
    config_free(&cfg);
    drop_config();

    CASE("a built-in name cannot be given a command");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[stats]\ncpu = \"echo 1\"\n");
    config_load(&cfg, p);
    CHECK_INT(cfg.error_count, 1);
    CHECK_INT(cfg.stats.custom_count, 0);
    config_free(&cfg);
    drop_config();

    /* A command every tenth of a second is a fork bomb with a nice UI. */
    CASE("the interval has a floor");
    p = write_config(
        "[binds]\n\"super+q\" = \"killclient\"\n"
        "[stats]\ninterval = 0.01\n");
    config_load(&cfg, p);
    CHECK_DBL(cfg.stats.interval, 0.5, 1e-9);
    config_free(&cfg);
    drop_config();
}

int main(void) {
    test_missing_file();
    test_values_parse();
    test_colors();
    test_bad_input();
    test_error_cap();
    test_tilde_expansion();
    test_input_touchpad();
    test_gestures();
    test_rule_material();
    test_physics_profiles();
    test_mass_mode();
    test_every_dispatchable_action_binds();
    test_sound();
    test_mouse();
    test_modes();
    test_radial();
    test_option_table();
    test_option_check();
    test_output_spellings();
    test_outputs();
    test_stats();
    return t_report("config");
}
