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

/* The session's history: that it is kept, that it outlives a compositor that
 * crashes, and that the windows that compositor had open are taken as closed
 * when it is read back — closed when it was last heard from, not when the next
 * one started. And that it stays bounded, in memory and on disk. */

#include "test.h"
#include "journal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char dir[] = "/tmp/fwm-test-journal-XXXXXX";
static char path[256];

static JournalGeom at(int desktop, int x, int y) {
    return (JournalGeom){ .desktop = desktop, .x = x, .y = y, .w = 800, .h = 600 };
}

/* The kinds of the events, oldest first, as one string ("SOMRC"), which is
 * what the order checks below read easiest as. */
static void kinds(const FwmJournal *j, char *out, size_t cap) {
    static const char *const k = "SOCTFMRH";   /* by JournalKind */
    size_t n = 0;
    for (int i = 0; i < journal_count(j) && n + 1 < cap; i++)
        out[n++] = k[journal_at(j, i)->kind];
    out[n] = '\0';
}

static void test_memory(void) {
    CASE("memory");
    FwmJournal *j = journal_open(NULL, 1000);
    CHECK_INT(journal_count(j), 1);
    CHECK_INT(journal_at(j, 0)->kind, JOURNAL_START);

    JournalGeom g = at(2, 100, 50);
    uint32_t a = journal_window_open(j, 1100, "kitty", "~", &g);
    uint32_t b = journal_window_open(j, 1200, "firefox", "Start", &g);
    CHECK(a != 0 && b != 0 && a != b);

    uint64_t v = journal_version(j);
    journal_window_title(j, 1300, b, "Start");          /* not a change */
    CHECK_INT(journal_version(j), v);
    journal_window_title(j, 1300, b, "News\tof\nthe day");
    CHECK_STR(journal_window(j, b)->title, "News of the day");

    g = at(3, 0, 0);
    uint64_t lv = journal_list_version(j);
    journal_record(j, 1400, a, JOURNAL_FOCUS, NULL);
    CHECK_INT(journal_list_version(j), lv);             /* not a thing a list shows */
    journal_record(j, 1400, a, JOURNAL_MOVE, &g);
    journal_record(j, 1500, a, JOURNAL_REST, &g);
    journal_record(j, 1500, a, JOURNAL_OPEN, &g);       /* not this call's kind */
    journal_window_close(j, 1600, a);

    char k[32];
    kinds(j, k, sizeof(k));
    CHECK_STR(k, "SOOTFMRC");
    CHECK(journal_list_version(j) > lv);                /* the close is */
    const JournalEvent *rest = journal_at(j, 6);
    CHECK_INT(rest->desktop, 3);
    CHECK_INT(rest->w, 800);
    CHECK_INT(journal_window(j, a)->closed_ms, 1600);
    CHECK_INT(journal_window(j, b)->closed_ms, 0);
    CHECK_STR(journal_window(j, a)->app_id, "kitty");
    CHECK_INT(journal_window_count(j), 2);
    CHECK_INT(journal_window_at(j, 0)->wid, a);
    CHECK_INT(journal_window_at(j, 1)->wid, b);
    CHECK(journal_window_at(j, 2) == NULL);
    journal_close(j);
}

static void test_crash_and_back(void) {
    CASE("crash and back");
    unlink(path);
    FwmJournal *j = journal_open(path, 1000);
    JournalGeom g = at(0, 10, 20);
    uint32_t a = journal_window_open(j, 1100, "kitty", "one", &g);
    uint32_t b = journal_window_open(j, 1200, "foot", "two", &g);
    journal_window_close(j, 1300, a);
    g = at(1, 30, 40);
    journal_record(j, 1700, b, JOURNAL_REST, &g);       /* last heard of at 1700 */
    /* No close for `b`: the compositor dies here, and its windows with it. */
    journal_close(j);

    j = journal_open(path, 5000);
    const JournalWindow *wb = journal_window(j, b);
    CHECK(wb != NULL);
    if (wb) {
        CHECK_STR(wb->title, "two");
        CHECK_INT(wb->closed_ms, 1700);                 /* not 5000 */
    }
    CHECK_INT(journal_window(j, a)->closed_ms, 1300);

    /* Read back in order, with the close the crash never wrote put in where it
     * belongs: before this run's START. */
    char k[32];
    kinds(j, k, sizeof(k));
    CHECK_STR(k, "SOOCRCS");
    CHECK_INT(journal_at(j, 5)->wid, b);
    CHECK_INT(journal_at(j, 5)->ms, 1700);

    /* Numbers go on from where the last run left them. */
    uint32_t c = journal_window_open(j, 5100, "kitty", "three", &g);
    CHECK(c > b);
    journal_close(j);

    /* Twice is the same as once: the close worked out for `b` is not doubled
     * by a second run, and the one still open from the second is now closed
     * at the moment that run was last heard from. */
    j = journal_open(path, 9000);
    kinds(j, k, sizeof(k));
    CHECK_STR(k, "SOOCRCSOCS");
    CHECK_INT(journal_window(j, c)->closed_ms, 5100);
    journal_close(j);
}

static void test_bounded(void) {
    CASE("bounded");
    unlink(path);
    /* Eight events, four windows, and a file let grow to 600 bytes. */
    FwmJournal *j = journal_open_sized(path, 1000, 8, 4, 600);
    JournalGeom g = at(0, 0, 0);
    uint32_t first = 0, last = 0;
    for (int i = 0; i < 40; i++) {
        char title[32];
        snprintf(title, sizeof(title), "window %d", i);
        last = journal_window_open(j, 2000 + i * 10, "kitty", title, &g);
        if (!first) first = last;
        journal_record(j, 2005 + i * 10, last, JOURNAL_MOVE, &g);
    }
    CHECK_INT(journal_count(j), 8);
    CHECK(journal_window(j, first) == NULL);            /* long gone */
    CHECK(journal_window(j, last) != NULL);
    CHECK(journal_file_bytes(j) <= 600 + 128);
    /* The newest events are the ones kept. */
    CHECK_INT(journal_at(j, 7)->kind, JOURNAL_MOVE);
    CHECK_INT(journal_at(j, 7)->wid, last);
    journal_close(j);

    /* The rewritten file reads back to the same windows. */
    j = journal_open_sized(path, 9000, 8, 4, 600);
    const JournalWindow *w = journal_window(j, last);
    CHECK(w != NULL);
    if (w) {
        CHECK_STR(w->title, "window 39");
        CHECK_INT(w->closed_ms, 2395);                  /* its last MOVE */
    }
    uint32_t next = journal_window_open(j, 9100, "foot", "after", &g);
    CHECK(next > last);
    journal_close(j);
}

static void test_names(void) {
    CASE("names");
    FwmJournal *j = journal_open(NULL, 0);
    JournalGeom g = at(0, 0, 0);
    /* A title longer than is kept, ending in two-byte letters: it is cut where
     * a letter ends, never inside one. */
    char longt[400];
    size_t n = 0;
    while (n + 2 < sizeof(longt) - 1) { longt[n++] = '\xD0'; longt[n++] = '\xB6'; }
    longt[n] = '\0';
    uint32_t a = journal_window_open(j, 1, "app", longt, &g);
    const char *t = journal_window(j, a)->title;
    size_t len = strlen(t);
    CHECK(len < JOURNAL_TITLE_MAX);
    CHECK_INT(len % 2, 0);
    CHECK_INT((unsigned char)t[len - 2], 0xD0);
    journal_close(j);
}

int main(void) {
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(path, sizeof(path), "%s/journal", dir);

    test_memory();
    test_crash_and_back();
    test_bounded();
    test_names();

    unlink(path);
    rmdir(dir);
    return t_report("journal");
}
