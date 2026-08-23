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

/* The charge, and when to say something about it. See battery.h. */

#include "battery.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define POWER_SUPPLY_DIR "/sys/class/power_supply"

/* How often the charge is read. It moves in minutes, not seconds, and every
 * read that finds the same number is a read nobody needed. */
#define BATTERY_PERIOD_S 10.0

/* How far the charge has to climb back over a threshold before that warning is
 * armed again. A resting battery wanders a percent either way, and without this
 * a laptop sitting at exactly 15% would say so every ten seconds all evening. */
#define BATTERY_REARM_MARGIN 3

/* Read a whole small file. /sys files are generated on read, so they report a
 * size of 0 and cannot be sized first — hence the fixed buffer. */
static int read_small(const char *path, char *out, size_t out_size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, out, out_size - 1);
    close(fd);
    if (n <= 0) return 0;
    out[n] = '\0';
    return 1;
}

/* Where the battery is, found once and remembered. "" once we have looked and
 * found nothing, so a desktop does not walk /sys every ten seconds forever. */
static char bat_path[288];
static int  bat_looked;

/* `scope = Device` is what keeps a wireless mouse out of this. Peripherals
 * report themselves as batteries here too — hidpp_battery_0, a headset, a
 * stylus — and they are batteries, just not the one the machine runs on. A
 * supply with no `capacity` is skipped for the same reason: nothing to read. */
static void find_battery(void) {
    bat_looked = 1;
    bat_path[0] = '\0';

    DIR *d = opendir(POWER_SUPPLY_DIR);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;

        char path[384], buf[64];
        snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/type", e->d_name);
        if (!read_small(path, buf, sizeof(buf))) continue;
        if (strncmp(buf, "Battery", 7) != 0) continue;

        snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/scope", e->d_name);
        if (read_small(path, buf, sizeof(buf)) && strncmp(buf, "Device", 6) == 0)
            continue;   /* a mouse, not the machine */

        snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/capacity", e->d_name);
        if (access(path, R_OK) != 0) continue;

        snprintf(bat_path, sizeof(bat_path), POWER_SUPPLY_DIR "/%s", e->d_name);
        break;
    }
    closedir(d);
}

int battery_read(BatteryReading *out) {
    if (!out) return 0;
    *out = (BatteryReading){0};

    /* FWM_BATTERY_DIR points the reader at a directory of your own, holding a
     * `capacity` and a `status` file. It is how the warnings can be exercised
     * at all: the thresholds only ever fire on a laptop at 15%, and waiting for
     * one is not a test. Consulted on every read rather than cached with the
     * search below, so a test can move the charge between two calls — and so
     * that setting it after the first read still takes effect. */
    const char *fake = getenv("FWM_BATTERY_DIR");
    if (fake && *fake) {
        snprintf(bat_path, sizeof(bat_path), "%s", fake);
    } else {
        if (!bat_looked) find_battery();
        if (!bat_path[0]) return 0;
    }

    char path[sizeof(bat_path) + 16], buf[64];
    snprintf(path, sizeof(path), "%s/capacity", bat_path);
    if (!read_small(path, buf, sizeof(buf))) {
        /* The battery was there and is not any more — a removable pack, or a
         * kernel that renamed the supply. Look again next time rather than
         * reading a path that no longer exists forever. */
        bat_looked = 0;
        return 0;
    }

    int pct = atoi(buf);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    /* "Full" is deliberately NOT charging: a battery that has finished charging
     * is not filling, and a plug left in overnight reads the same as one that
     * was never in. */
    snprintf(path, sizeof(path), "%s/status", bat_path);
    int charging = read_small(path, buf, sizeof(buf)) && strncmp(buf, "Charging", 8) == 0;

    out->present  = 1;
    out->percent  = pct;
    out->charging = charging;
    return 1;
}

BatteryAlert battery_watch(BatteryWatch *w, const BatteryConfig *cfg,
                           double dt, int *pct_out) {
    if (!w || !cfg) return BATTERY_ALERT_NONE;

    w->due -= dt;
    if (w->due > 0.0) return BATTERY_ALERT_NONE;
    w->due = BATTERY_PERIOD_S;

    BatteryReading r;
    if (!battery_read(&r) || !r.present) return BATTERY_ALERT_NONE;

    w->last_pct = r.percent;
    if (pct_out) *pct_out = r.percent;

    /* On the charger there is nothing to warn about, and everything said during
     * the last discharge is forgotten: unplugging is the start of a new one. */
    if (r.charging) {
        w->said_low = w->said_crit = 0;
        return BATTERY_ALERT_NONE;
    }

    /* Climbing back well clear of a threshold arms it again. This is not the
     * charging case above — a battery can gain a few percent on its own after a
     * heavy load lets up, and the reading it gave while the CPU was flat out
     * should not silence the warning for the rest of the evening. */
    if (cfg->low > 0 && r.percent > cfg->low + BATTERY_REARM_MARGIN)
        w->said_low = 0;
    if (cfg->critical > 0 && r.percent > cfg->critical + BATTERY_REARM_MARGIN)
        w->said_crit = 0;

    /* Critical first, and it counts as the low warning too: a machine that fell
     * from 20% to 4% between two reads should say the worse of the two things
     * once, not both of them in a row. */
    if (cfg->critical > 0 && r.percent <= cfg->critical && !w->said_crit) {
        w->said_crit = 1;
        w->said_low  = 1;
        return BATTERY_ALERT_CRITICAL;
    }
    if (cfg->low > 0 && r.percent <= cfg->low && !w->said_low) {
        w->said_low = 1;
        return BATTERY_ALERT_LOW;
    }
    return BATTERY_ALERT_NONE;
}
