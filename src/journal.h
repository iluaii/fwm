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

#ifndef FWM_JOURNAL_H
#define FWM_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The session's history: every window that opened and closed, where each one
 * went and when — what the strip's time view reads.
 *
 * Two tables: the events in the order they happened, and the windows they
 * happened to, so an event is a few numbers and a window's name is kept once.
 * Both are rings: past their capacity the oldest goes first.
 *
 * And a file. The history lives as long as the boot does — in $XDG_RUNTIME_DIR,
 * which is a tmpfs of the user's own, gone at the next reboot and readable by
 * nobody else — so a compositor that crashes and is started again picks its
 * history back up where it left off. Text, one event a line, appended as it
 * happens; rewritten from memory when it has grown past what memory holds.
 *
 * Nothing in here knows about wlroots, windows or screens: it is told numbers
 * and names, which is what lets the tests drive it. server_journal.c is the
 * half that watches the compositor. */

typedef enum {
    JOURNAL_START = 0,   /* the compositor started: the windows before it are gone */
    JOURNAL_OPEN,        /* a window appeared */
    JOURNAL_CLOSE,       /* ...and went */
    JOURNAL_TITLE,       /* it changed its title */
    JOURNAL_FOCUS,       /* it took the focus */
    JOURNAL_MOVE,        /* a point on the way somewhere: dragged, flying, laid out */
    JOURNAL_REST,        /* where it came to rest */
    JOURNAL_THROW,       /* it was thrown */
    JOURNAL_KINDS,
} JournalKind;

/* Where a window is: on which desktop, and where on it — x is from that
 * desktop's left edge, not from the start of the strip. */
#define JOURNAL_F_TILED      1u
#define JOURNAL_F_FULLSCREEN 2u

typedef struct {
    int desktop;
    int x, y, w, h;
    unsigned flags;
} JournalGeom;

typedef struct {
    int64_t ms;          /* wall clock, ms since the epoch */
    uint32_t wid;        /* the window, by the journal's own number; 0 for START */
    uint8_t kind;        /* JournalKind */
    uint8_t flags;
    int8_t desktop;
    int32_t x, y, w, h;
} JournalEvent;

#define JOURNAL_APP_MAX   64
#define JOURNAL_TITLE_MAX 128

typedef struct {
    uint32_t wid;
    char app_id[JOURNAL_APP_MAX];
    char title[JOURNAL_TITLE_MAX];   /* the latest it had */
    int64_t opened_ms;
    int64_t closed_ms;               /* 0 while it is open */
} JournalWindow;

typedef struct FwmJournal FwmJournal;

/* Open the history kept at `path`, reading back whatever an earlier run of the
 * compositor left there, and mark that this one has started at `now_ms`.
 * Windows the earlier run had open went with it; they are closed at the last
 * moment it recorded anything. NULL path keeps the history in memory only.
 * Never NULL: a file that cannot be had costs the file, not the history. */
FwmJournal *journal_open(const char *path, int64_t now_ms);
/* The same with the limits named: how many events and windows the rings hold
 * and how far the file may grow before it is rewritten. For the tests, which
 * need to reach the limits without writing a day's worth of history. */
FwmJournal *journal_open_sized(const char *path, int64_t now_ms,
                               int events, int windows, size_t file_max);
void journal_close(FwmJournal *j);

/* A window appeared. Returns its number for every later call about it — never
 * 0, never one an earlier run of this boot used. */
uint32_t journal_window_open(FwmJournal *j, int64_t ms, const char *app_id,
                             const char *title, const JournalGeom *g);
void journal_window_close(FwmJournal *j, int64_t ms, uint32_t wid);
void journal_window_title(FwmJournal *j, int64_t ms, uint32_t wid, const char *title);
/* FOCUS, MOVE, REST or THROW. `g` may be NULL for the ones that do not move it. */
void journal_record(FwmJournal *j, int64_t ms, uint32_t wid, JournalKind kind,
                    const JournalGeom *g);

/* The events, oldest first: 0 .. journal_count() - 1. */
int journal_count(const FwmJournal *j);
const JournalEvent *journal_at(const FwmJournal *j, int i);
/* A window by its number; NULL once the ring has let it go. */
const JournalWindow *journal_window(const FwmJournal *j, uint32_t wid);
/* The windows, in the order they opened: 0 .. journal_window_count() - 1.
 * Their openings and closings outlast the events — a window dragged about all
 * afternoon pushes points of its way out of the event ring, never the fact
 * that it, and every other window, opened and closed. */
int journal_window_count(const FwmJournal *j);
const JournalWindow *journal_window_at(const FwmJournal *j, int i);

/* Goes up by one with every change, so a reader can tell whether what it drew
 * is still what is there without comparing the contents. The second only with
 * the changes a list of windows shows — one opening, closing or being renamed
 * — and not with every point of a window's way across the screen. */
uint64_t journal_version(const FwmJournal *j);
uint64_t journal_list_version(const FwmJournal *j);

/* How big the file has grown, for the tests of the rewrite. */
size_t journal_file_bytes(const FwmJournal *j);

#endif /* FWM_JOURNAL_H */
