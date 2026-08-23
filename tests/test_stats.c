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
#include "battery.h"

#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

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

/* The charge watcher: when it speaks, and — the part that matters — when it
 * shuts up. A warning that repeats every ten seconds is one people turn off,
 * which leaves them with no warning at all. Driven with a battery-free machine
 * in mind: where there is nothing to read, battery_watch must answer NONE
 * forever and cost nothing, and that is what runs in CI. */
static void test_battery_watch(void) {
    CASE("a machine with no battery is never warned about one");
    BatteryConfig cfg = { .low = 15, .critical = 5, .command = "" };
    BatteryWatch w = {0};

    BatteryReading r;
    int has_battery = battery_read(&r) && r.present;

    if (!has_battery) {
        for (int i = 0; i < 20; i++)
            CHECK_INT(battery_watch(&w, &cfg, 11.0, NULL), BATTERY_ALERT_NONE);
    }

    CASE("thresholds off mean silence whatever the charge is");
    BatteryConfig off = { .low = 0, .critical = 0, .command = "" };
    w = (BatteryWatch){0};
    for (int i = 0; i < 5; i++)
        CHECK_INT(battery_watch(&w, &off, 11.0, NULL), BATTERY_ALERT_NONE);

    CASE("a read only happens when it is due");
    w = (BatteryWatch){0};
    /* Ten seconds apart, so nine one-second ticks must not sample at all — the
     * `due` countdown is the whole of the cost control here. */
    battery_watch(&w, &cfg, 0.0, NULL);        /* first call: due, and sampled */
    double before = w.due;
    battery_watch(&w, &cfg, 1.0, NULL);
    CHECK(w.due < before);                     /* counted down ... */
    CHECK(w.due > 0.0);                        /* ... and not re-armed */
}

/* The rules themselves, over a battery of our own: FWM_BATTERY_DIR points the
 * reader at two files we write, so a discharge that would take a laptop four
 * hours takes four lines here. */
static void write_file(const char *dir, const char *name, const char *body) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(body, f);
    fclose(f);
}

static void set_charge(const char *dir, int pct, int charging) {
    char n[16];
    snprintf(n, sizeof(n), "%d\n", pct);
    write_file(dir, "capacity", n);
    write_file(dir, "status", charging ? "Charging\n" : "Discharging\n");
}

static void test_battery_thresholds(void) {
    CASE("a discharge warns twice and repeats neither");
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/fwm-test-battery-%d", (int)getpid());
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return;
    setenv("FWM_BATTERY_DIR", dir, 1);

    BatteryConfig cfg = { .low = 15, .critical = 5, .command = "" };
    BatteryWatch w = {0};
    int pct = 0;

    /* Comfortable: nothing to say, however many times it is asked. */
    set_charge(dir, 80, 0);
    CHECK_INT(battery_watch(&w, &cfg, 11.0, &pct), BATTERY_ALERT_NONE);
    CHECK_INT(pct, 80);

    /* Down through the first threshold: said once, then not again while it
     * sits there — the failure mode this whole test exists for. */
    set_charge(dir, 14, 0);
    CHECK_INT(battery_watch(&w, &cfg, 11.0, &pct), BATTERY_ALERT_LOW);
    CHECK_INT(battery_watch(&w, &cfg, 11.0, &pct), BATTERY_ALERT_NONE);
    set_charge(dir, 13, 0);
    CHECK_INT(battery_watch(&w, &cfg, 11.0, &pct), BATTERY_ALERT_NONE);

    /* And through the second. */
    set_charge(dir, 4, 0);
    CHECK_INT(battery_watch(&w, &cfg, 11.0, &pct), BATTERY_ALERT_CRITICAL);
    CHECK_INT(battery_watch(&w, &cfg, 11.0, &pct), BATTERY_ALERT_NONE);

    CASE("the plug going in arms both warnings again");
    set_charge(dir, 30, 1);
    CHECK_INT(battery_watch(&w, &cfg, 11.0, &pct), BATTERY_ALERT_NONE);
    set_charge(dir, 12, 0);   /* unplugged again, already low */
    CHECK_INT(battery_watch(&w, &cfg, 11.0, &pct), BATTERY_ALERT_LOW);

    CASE("a fall past both thresholds says the worse one, once");
    w = (BatteryWatch){0};
    set_charge(dir, 60, 0);
    CHECK_INT(battery_watch(&w, &cfg, 11.0, &pct), BATTERY_ALERT_NONE);
    set_charge(dir, 3, 0);    /* asleep through the middle of the range */
    CHECK_INT(battery_watch(&w, &cfg, 11.0, &pct), BATTERY_ALERT_CRITICAL);
    CHECK_INT(battery_watch(&w, &cfg, 11.0, &pct), BATTERY_ALERT_NONE);

    CASE("climbing clear of a threshold arms it again without a charger");
    w = (BatteryWatch){0};
    set_charge(dir, 15, 0);
    CHECK_INT(battery_watch(&w, &cfg, 11.0, &pct), BATTERY_ALERT_LOW);
    set_charge(dir, 16, 0);   /* inside the margin: still the same warning */
    CHECK_INT(battery_watch(&w, &cfg, 11.0, &pct), BATTERY_ALERT_NONE);
    set_charge(dir, 25, 0);   /* well clear: armed */
    CHECK_INT(battery_watch(&w, &cfg, 11.0, &pct), BATTERY_ALERT_NONE);
    set_charge(dir, 15, 0);
    CHECK_INT(battery_watch(&w, &cfg, 11.0, &pct), BATTERY_ALERT_LOW);

    unsetenv("FWM_BATTERY_DIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/capacity", dir); unlink(path);
    snprintf(path, sizeof(path), "%s/status", dir);   unlink(path);
    rmdir(dir);
}

int main(void) {
    test_builtins();
    test_custom_command();
    test_format();
    test_slow_command();
    test_reconfigure_keeps_values();
    test_battery_and_network();
    test_battery_watch();
    test_battery_thresholds();
    return t_report("stats");
}
