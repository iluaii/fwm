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

/* The wallpaper-derived palette, with more than one wallpaper.
 *
 * Two monitors showing two images used to share one palette, and it was picked
 * before any monitor existed — so a red accent lifted from a bright picture
 * stood on top of a dark screen that had nothing red in it. These tests pin the
 * rule that replaced it: a palette per monitor, from the image that monitor is
 * actually showing, and the un-tied one following the screen the user is on.
 *
 * The images are written here rather than kept as fixtures: a flat colour has
 * exactly one hue, so what the sampler should answer is not a matter of taste. */

#include "test.h"
#include "theme.h"
#include "config.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static char tmp_cfg[256], tmp_red[256], tmp_blue[256], tmp_grey[256];

/* A 64x64 image of one colour. */
static void write_flat_png(const char *path, guint8 r, guint8 g, guint8 b) {
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 64, 64);
    if (!pb) { fprintf(stderr, "cannot make a pixbuf\n"); exit(2); }
    gdk_pixbuf_fill(pb, ((guint32)r << 24) | ((guint32)g << 16) | ((guint32)b << 8) | 0xff);
    GError *err = NULL;
    if (!gdk_pixbuf_save(pb, path, "png", &err, NULL)) {
        fprintf(stderr, "cannot write %s: %s\n", path, err ? err->message : "?");
        exit(2);
    }
    g_object_unref(pb);
}

static void write_images(void) {
    int pid = (int)getpid();
    snprintf(tmp_red,  sizeof tmp_red,  "/tmp/fwm-test-theme-red-%d.png", pid);
    snprintf(tmp_blue, sizeof tmp_blue, "/tmp/fwm-test-theme-blue-%d.png", pid);
    snprintf(tmp_grey, sizeof tmp_grey, "/tmp/fwm-test-theme-grey-%d.png", pid);
    write_flat_png(tmp_red,  0xd9, 0x4f, 0x3d);
    write_flat_png(tmp_blue, 0x1a, 0x2a, 0x55);
    write_flat_png(tmp_grey, 0x80, 0x80, 0x80);
}

static void drop_files(void) {
    unlink(tmp_red); unlink(tmp_blue); unlink(tmp_grey); unlink(tmp_cfg);
}

static void load_config(FwmConfig *cfg, const char *body) {
    snprintf(tmp_cfg, sizeof tmp_cfg, "/tmp/fwm-test-theme-%d.toml", (int)getpid());
    FILE *f = fopen(tmp_cfg, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", tmp_cfg); exit(2); }
    fputs(body, f);
    fclose(f);
    config_load(cfg, tmp_cfg);
}

/* Is this colour more red than blue, or the other way round? Precise enough to
 * tell two wallpapers apart and loose enough to survive a change to how the
 * accent is normalised. */
static int is_reddish(const double rgb[3])  { return rgb[0] > rgb[2] + 0.15; }
static int is_bluish(const double rgb[3])   { return rgb[2] > rgb[0] + 0.15; }

static void test_palette_per_monitor(void) {
    CASE("each monitor's palette comes from its own wallpaper");

    char body[1024];
    snprintf(body, sizeof body,
             "[decor]\ncolor_source = \"wallpaper\"\n"
             "[[wallpaper]]\npath = \"%s\"\noutput = \"DP-1\"\n"
             "[[wallpaper]]\npath = \"%s\"\noutput = \"DP-2\"\n",
             tmp_blue, tmp_red);
    FwmConfig cfg;
    load_config(&cfg, body);
    theme_build(&cfg);

    CHECK(is_bluish(theme_get_output("DP-1")->accent));
    CHECK(is_reddish(theme_get_output("DP-2")->accent));
    /* And the island fill each screen's panels stand on, which is the tint
     * rather than the accent: the same split, on the other half of the theme. */
    CHECK(theme_get_output("DP-1")->pill[2] > theme_get_output("DP-2")->pill[2]);
    CHECK(theme_get_output("DP-2")->pill[0] > theme_get_output("DP-1")->pill[0]);

    /* The un-tied palette follows the monitor the user is on. This is the
     * whole bug: it used to be whichever block came first in the file, for the
     * life of the session. */
    CHECK(theme_set_active_output("DP-2") != 0);
    CHECK(is_reddish(theme_get()->accent));
    CHECK(theme_set_active_output("DP-1") != 0);
    CHECK(is_bluish(theme_get()->accent));
    /* Asking for the screen it is already on changes nothing and repaints
     * nothing. */
    CHECK_INT(theme_set_active_output("DP-1"), 0);

    /* A panel painted for one screen reads that screen's colours whatever the
     * pointer is doing, and the scope is only as wide as the draw. */
    theme_use_output("DP-2");
    CHECK(is_reddish(theme_get()->accent));
    theme_use_output(NULL);
    CHECK(is_bluish(theme_get()->accent));

    config_free(&cfg);
}

static void test_unnamed_set_answers_for_the_rest(void) {
    CASE("a monitor with no wallpaper of its own takes the un-named one's");

    char body[1024];
    snprintf(body, sizeof body,
             "[decor]\ncolor_source = \"wallpaper\"\n"
             "[[wallpaper]]\npath = \"%s\"\n"
             "[[wallpaper]]\npath = \"%s\"\noutput = \"DP-2\"\n",
             tmp_blue, tmp_red);
    FwmConfig cfg;
    load_config(&cfg, body);
    theme_build(&cfg);

    CHECK(is_reddish(theme_get_output("DP-2")->accent));
    /* Every other screen, named or not, is drawn from the general layer —
     * the same rule config_wallpaper_on_output applies to the images. */
    CHECK(is_bluish(theme_get_output("HDMI-A-1")->accent));
    CHECK(is_bluish(theme_get_output("")->accent));
    CHECK(is_bluish(theme_get_output(NULL)->accent));

    config_free(&cfg);
}

static void test_config_colours_and_bad_images(void) {
    CASE("nothing to derive from: the configured colours, for every monitor");

    /* color_source = "config": one palette, and asking per monitor answers
     * with it rather than with nothing. */
    char body[1024];
    snprintf(body, sizeof body,
             "[decor]\ncolor_source = \"config\"\ncol_active = \"#7aa2f7\"\n"
             "[[wallpaper]]\npath = \"%s\"\noutput = \"DP-1\"\n", tmp_red);
    FwmConfig cfg;
    load_config(&cfg, body);
    theme_build(&cfg);
    CHECK(!is_reddish(theme_get_output("DP-1")->accent));
    CHECK(theme_get_output("DP-1") == theme_get());
    config_free(&cfg);

    /* A greyscale image has no hue to lift: that monitor keeps the configured
     * colours and the user is told, while the other screen is unaffected. */
    CASE("a greyscale wallpaper costs only its own monitor its colours");
    snprintf(body, sizeof body,
             "[decor]\ncolor_source = \"wallpaper\"\n"
             "[[wallpaper]]\npath = \"%s\"\noutput = \"DP-1\"\n"
             "[[wallpaper]]\npath = \"%s\"\noutput = \"DP-2\"\n",
             tmp_grey, tmp_red);
    load_config(&cfg, body);
    theme_build(&cfg);
    CHECK(cfg.error_count > 0);
    CHECK(!is_reddish(theme_get_output("DP-1")->accent));
    CHECK(is_reddish(theme_get_output("DP-2")->accent));
    config_free(&cfg);

    /* And with no [[wallpaper]] at all, "wallpaper" is a mistake that must
     * cost nothing but a report. */
    CASE("color_source = wallpaper with no wallpaper keeps the config colours");
    load_config(&cfg, "[decor]\ncolor_source = \"wallpaper\"\n");
    theme_build(&cfg);
    CHECK(cfg.error_count > 0);
    CHECK(theme_get_output("DP-1") == theme_get());
    config_free(&cfg);
}

int main(void) {
    write_images();
    test_palette_per_monitor();
    test_unnamed_set_answers_for_the_rest();
    test_config_colours_and_bad_images();
    drop_files();
    return t_report("theme");
}
