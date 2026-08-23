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

#ifndef FWM_LAUNCHER_SEARCH_H
#define FWM_LAUNCHER_SEARCH_H

/* How the launcher decides what a query means and which row wins.
 *
 * Split out of launcher.c because none of it needs a compositor: the ranking
 * rules and the frecency store are pure text and arithmetic, so tests/ can
 * link this file on its own and pin the answers (tests/test_launcher.c). The
 * panel, its physics and its drawing stay next door.
 */

#include <stddef.h>
#include <time.h>

/* Folded forms a row carries. The name buffer is sized for the 128-byte
 * display name plus the room case folding can add (ß -> ss and friends); the
 * alias holds GenericName, Keywords and the binary's basename, which is what
 * makes "browser" find Firefox. */
#define LS_FOLD_MAX   160
#define LS_ALIAS_MAX  192

/* ── locale ──────────────────────────────────────────────────────────── */

/* The user's language, split the way desktop-entry keys spell it. Resolved
 * once per scan and passed down, rather than cached in a static, so a test can
 * ask for a locale that is not the one the machine is running under. */
typedef struct {
    char lang[16];  /* "ru", "" when the locale is C/POSIX or unset */
    char full[40];  /* "ru_RU", "" when the locale names no country */
} LsLocale;

void ls_locale_init(LsLocale *loc);
/* Same, from an explicit locale string ("ru_RU.UTF-8@x") instead of the
 * environment. */
void ls_locale_from(LsLocale *loc, const char *value);

/* If `line` is `<key>=value` or `<key>[locale]=value`, point `*value` at the
 * value and return how well the suffix matches: 0 unlocalised, 1 language,
 * 2 language and country. -1 for a different key, or a locale that is not
 * ours. Higher wins, so a caller keeps the best `Name` it has seen. */
int ls_entry_value(const LsLocale *loc, const char *line, const char *key,
                   const char **value);

/* ── folding ─────────────────────────────────────────────────────────── */

/* Case-folded, accent-stripped copy of `in`, truncated on a codepoint
 * boundary to fit `cap`. Both the row and the query go through this, so
 * matching downstream is plain byte work: "Ёлка" and "елка" fold alike, and
 * so do "Ü" and "u". */
void ls_fold(char *out, size_t cap, const char *in);
/* ls_fold, appended to what `out` already holds with a space between. */
void ls_fold_append(char *out, size_t cap, const char *in);

/* The program an Exec= line actually runs, bare: "firefox" out of
 * "/usr/bin/firefox --new-window". Folded into a row's alias, because the
 * name on an entry and the name a person types are routinely different —
 * nobody looks up "Neovim" by anything but "nvim". A leading env(1) and its
 * assignments are stepped over, which is how a fair number of entries spell
 * "run this with a variable set". */
void ls_exec_binary(const char *exec, char *out, size_t cap);

/* ── ranking ─────────────────────────────────────────────────────────── */

/* Match quality, best first. The tier decides the order before anything else
 * does — a row you launch daily still loses to a better match, or typing
 * would stop steering the list. */
enum {
    LS_TIER_EXACT  = 0, /* the whole name */
    LS_TIER_PREFIX = 1, /* name starts with the query */
    LS_TIER_WORD   = 2, /* query starts a word inside the name */
    LS_TIER_SUB    = 3, /* anywhere in the name */
    LS_TIER_ALIAS  = 4, /* in the keywords, generic name or binary */
    LS_TIER_FUZZY  = 5, /* the letters in order, gaps allowed: "ffx" */
    LS_TIER_NONE   = -1,
};

/* Rank folded `query` against a row's folded name and alias. `*score` gets a
 * within-tier tiebreak where smaller is better (how far into the string the
 * match sits, or how spread out a fuzzy one is). An empty query matches
 * everything at LS_TIER_EXACT, which is what leaves the order to frecency. */
int ls_rank(const char *name, const char *alias, const char *query, int *score);

/* ── frecency ────────────────────────────────────────────────────────── */

/* What the launcher remembers about what you run. One number per entry, aged
 * by a half-life rather than counted forever: an application launched twice a
 * day this week outranks one launched thirty times last spring, and nothing
 * has to be reset by hand when habits change. */
typedef struct LsFrec LsFrec;

/* ~/.local/state/fwm/launcher, honouring $XDG_STATE_HOME. */
void ls_frec_path(char *buf, size_t cap);

/* A missing or unreadable file is an empty store, not a failure: the first
 * run of a new install has no history and must still open. NULL only on
 * allocation failure. */
LsFrec *ls_frec_load(const char *path);
void    ls_frec_free(LsFrec *f);

/* Score of `key` aged to `now`; 0 when unknown. */
double  ls_frec_score(const LsFrec *f, const char *key, time_t now);
/* Record a launch: age what is stored, then add one. */
void    ls_frec_bump(LsFrec *f, const char *key, time_t now);
/* Write the store back, dropping entries that have aged to nothing. Written
 * beside the target and renamed, so an interrupted save cannot leave a
 * half-written history. Returns 0 on success. */
int     ls_frec_save(const LsFrec *f, const char *path, time_t now);

#endif /* FWM_LAUNCHER_SEARCH_H */
