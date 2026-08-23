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

/* The sensor engine behind the tray's stats pill.
 *
 * The custom sensors are child processes, so this is also where the process
 * handling is checked: that a command's output arrives, that a slow one does
 * not block the caller, and that nothing is left behind — a compositor that
 * leaks a zombie per interval is a compositor that runs out of pids overnight.
 */

#include "test.h"
#include "stats.h"

#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

/* Drive the engine as the frame loop would, for at most `budget` seconds of
 * simulated time, until `name` has a value. Real time passes too (the child has
 * to run), so this sleeps a frame between ticks rather than spinning. */
static const StatsItem *pump_until_value(FwmStats *s, const char *name, double budget) {
    for (double t = 0.0; t < budget; t += 0.05) {
        stats_tick(s, 0.05);
        for (int i = 0; i < stats_count(s); i++) {
            const StatsItem *it = stats_item(s, i);
            if (strcmp(it->name, name) == 0 && it->value[0]) return it;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static StatsConfig cfg_with(const char *name, const char *cmd) {
    StatsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.interval = 0.5;
    snprintf(cfg.items[0], STATS_NAME_MAX, "%s", name);
    cfg.item_count = 1;
    if (cmd) {
        snprintf(cfg.custom[0].name, STATS_NAME_MAX, "%s", name);
        snprintf(cfg.custom[0].cmd, STATS_CMD_MAX, "%s", cmd);
        cfg.custom_count = 1;
    }
    return cfg;
}

static void test_builtins(void) {
    CASE("cpu and ram answer on this machine");
    StatsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.interval = 0.5;
    snprintf(cfg.items[0], STATS_NAME_MAX, "cpu");
    snprintf(cfg.items[1], STATS_NAME_MAX, "ram");
    cfg.item_count = 2;

    FwmStats *s = stats_create(&cfg);
    CHECK(s != NULL);
    CHECK_INT(stats_count(s), 2);

    /* RAM is a single read, so it lands on the first tick. CPU is a difference
     * between two reads and cannot say anything until the second — that is the
     * behaviour being pinned here, not an implementation detail: a load figure
     * from one sample would be "since boot", which is not what the pill claims
     * to show. */
    CHECK(pump_until_value(s, "ram", 1.0) != NULL);
    CHECK(pump_until_value(s, "cpu", 4.0) != NULL);
    stats_destroy(s);
}

static void test_custom_command(void) {
    CASE("a custom sensor shows what its command printed");
    StatsConfig cfg = cfg_with("vol", "echo 60%");
    FwmStats *s = stats_create(&cfg);
    const StatsItem *it = pump_until_value(s, "vol", 4.0);
    CHECK(it != NULL);
    if (it) CHECK_STR(it->value, "60%");
    stats_destroy(s);

    /* Only the first line, trimmed: commands answer with a trailing newline,
     * and a tray is one line high. */
    CASE("only the first line is taken");
    cfg = cfg_with("multi", "printf '  first \\nsecond\\n'");
    s = stats_create(&cfg);
    it = pump_until_value(s, "multi", 4.0);
    CHECK(it != NULL);
    if (it) CHECK_STR(it->value, "first");
    stats_destroy(s);
}

static void test_format(void) {
    CASE("the pill line joins the enabled sensors");
    StatsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.interval = 0.5;
    snprintf(cfg.items[0], STATS_NAME_MAX, "a");
    snprintf(cfg.items[1], STATS_NAME_MAX, "b");
    cfg.item_count = 2;
    snprintf(cfg.custom[0].name, STATS_NAME_MAX, "a");
    snprintf(cfg.custom[0].cmd, STATS_CMD_MAX, "echo 1");
    snprintf(cfg.custom[1].name, STATS_NAME_MAX, "b");
    snprintf(cfg.custom[1].cmd, STATS_CMD_MAX, "echo 2");
    cfg.custom_count = 2;

    FwmStats *s = stats_create(&cfg);
    CHECK(pump_until_value(s, "a", 4.0) != NULL);
    CHECK(pump_until_value(s, "b", 4.0) != NULL);

    char line[160];
    stats_format(s, line, sizeof(line));
    CHECK_STR(line, "A 1 \xE2\x80\xA2 B 2");

    /* Switched off in the menu: out of the line, but still known — which is
     * what lets switching it back on show a value at once. */
    stats_set_enabled(s, 0, 0);
    stats_format(s, line, sizeof(line));
    CHECK_STR(line, "B 2");

    stats_set_enabled(s, 1, 0);
    stats_format(s, line, sizeof(line));
    CHECK_STR(line, "");   /* the pill draws its placeholder on this */
    stats_destroy(s);
}

static void test_slow_command(void) {
    /* The frame loop calls this. A sensor that takes seconds must cost the
     * caller nothing but the ticks it takes to notice. */
    CASE("a slow command does not block the tick");
    StatsConfig cfg = cfg_with("slow", "sleep 30; echo never");
    FwmStats *s = stats_create(&cfg);

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 20; i++) stats_tick(s, 0.05);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double spent = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
    CHECK(spent < 0.5);

    const StatsItem *it = stats_item(s, 0);
    CHECK(it != NULL);
    if (it) CHECK_STR(it->value, "");   /* still nothing to say */

    /* And the child goes with the handle rather than outliving the session. */
    stats_destroy(s);
    CHECK_INT(waitpid(-1, NULL, WNOHANG), -1);   /* nothing left unreaped */
    CHECK_INT(errno, ECHILD);
}

static void test_reconfigure_keeps_values(void) {
    CASE("a reload keeps what a surviving sensor already had");
    StatsConfig cfg = cfg_with("vol", "echo 60%");
    FwmStats *s = stats_create(&cfg);
    CHECK(pump_until_value(s, "vol", 4.0) != NULL);
    stats_set_enabled(s, 0, 0);

    /* The same sensor plus a built-in in front of it. */
    StatsConfig next;
    memset(&next, 0, sizeof(next));
    next.interval = 0.5;
    snprintf(next.items[0], STATS_NAME_MAX, "ram");
    snprintf(next.items[1], STATS_NAME_MAX, "vol");
    next.item_count = 2;
    snprintf(next.custom[0].name, STATS_NAME_MAX, "vol");
    snprintf(next.custom[0].cmd, STATS_CMD_MAX, "echo 60%%");
    next.custom_count = 1;

    stats_reconfigure(s, &next);
    CHECK_INT(stats_count(s), 2);
    const StatsItem *vol = stats_item(s, 1);
    CHECK(vol != NULL);
    if (vol) {
        CHECK_STR(vol->name, "vol");
        CHECK_STR(vol->value, "60%");   /* not blanked for an interval ... */
        CHECK_INT(vol->enabled, 0);     /* ... and the menu's choice survived */
    }
    stats_destroy(s);
}

/* The two sensors added after the first three. Both are machine-dependent in a
 * way cpu and ram are not — a desktop has no battery, a container has no
 * interface with a device behind it — so what is pinned here is the CONTRACT
 * rather than a number: an unavailable sensor says so instead of sitting there
 * empty, and an available one answers in a shape the tray can draw. */
static void test_battery_and_network(void) {
    CASE("the network rate needs two reads, like the cpu load");
    StatsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.interval = 0.5;
    snprintf(cfg.items[0], STATS_NAME_MAX, "net");
    cfg.item_count = 1;

    FwmStats *s = stats_create(&cfg);
    CHECK(s != NULL);
    CHECK_INT(stats_count(s), 1);

    const StatsItem *net = stats_item(s, 0);
    CHECK(net != NULL);
    if (net && net->available) {
        /* One tick is one read: a rate cannot exist yet. */
        stats_tick(s, 0.05);
        CHECK_STR(net->value, "");

        const StatsItem *got = pump_until_value(s, "net", 4.0);
        CHECK(got != NULL);
        /* Both directions, down first, each with its arrow: "↓12K ↑0". */
        if (got) CHECK(strstr(got->value, "\xE2\x86\x93") == got->value);
        if (got) CHECK(strstr(got->value, "\xE2\x86\x91") != NULL);
    }
    stats_destroy(s);

    CASE("the battery is either answered or declared unavailable");
    memset(&cfg, 0, sizeof(cfg));
    cfg.interval = 0.5;
    snprintf(cfg.items[0], STATS_NAME_MAX, "bat");
    cfg.item_count = 1;

    s = stats_create(&cfg);
    CHECK(s != NULL);
    const StatsItem *bat = stats_item(s, 0);
    CHECK(bat != NULL);
    if (bat && bat->available) {
        const StatsItem *got = pump_until_value(s, "bat", 2.0);
        CHECK(got != NULL);
        /* "87%" or "87%+", never a bare number: the pill's own label says BAT,
         * and a percentage without its sign reads as minutes to some people. */
        if (got) {
            size_t n = strlen(got->value);
            CHECK(n >= 2);
            if (n >= 2) CHECK(got->value[n - 1] == '%' || got->value[n - 1] == '+');
        }
    } else {
        /* A machine with no battery must not draw an empty BAT in the pill —
         * unavailable is what keeps it out of stats_format entirely. */
        char line[256];
        stats_format(s, line, sizeof(line));
        CHECK_STR(line, "");
    }
    stats_destroy(s);
}

int main(void) {
    test_builtins();
    test_custom_command();
    test_format();
    test_slow_command();
    test_reconfigure_keeps_values();
    test_battery_and_network();
    return t_report("stats");
}
