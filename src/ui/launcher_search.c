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

#include "launcher_search.h"

#include <glib.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── locale ──────────────────────────────────────────────────────────── */

static void copy_bounded(char *out, size_t cap, const char *in) {
    size_t n = strlen(in);
    if (n >= cap) n = cap - 1;
    memcpy(out, in, n);
    out[n] = '\0';
}

void ls_locale_from(LsLocale *loc, const char *value) {
    loc->lang[0] = '\0';
    loc->full[0] = '\0';
    if (!value || !value[0]) return;
    if (strcmp(value, "C") == 0 || strcmp(value, "POSIX") == 0) return;

    /* "ru_RU.UTF-8@euro" -> full "ru_RU", lang "ru". The encoding and the
     * modifier are dropped: a .desktop file that spells one of them still
     * names the same language, and matching on the language is what we do
     * with the result anyway. */
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", value);
    char *cut = strpbrk(buf, ".@");
    if (cut) *cut = '\0';
    if (!buf[0]) return;

    /* Copied by length rather than printed: a locale name longer than these
     * buffers is not a locale, and truncating one silently is better than a
     * compiler warning on every build about a case that cannot happen. */
    copy_bounded(loc->full, sizeof(loc->full), buf);
    char *us = strchr(buf, '_');
    if (us) *us = '\0';
    else loc->full[0] = '\0';   /* no country: "full" would only repeat lang */
    copy_bounded(loc->lang, sizeof(loc->lang), buf);
}

void ls_locale_init(LsLocale *loc) {
    /* LC_ALL overrides everything, LC_MESSAGES is the category that governs
     * displayed text, LANG is the fallback for both. */
    const char *v = getenv("LC_ALL");
    if (!v || !v[0]) v = getenv("LC_MESSAGES");
    if (!v || !v[0]) v = getenv("LANG");
    ls_locale_from(loc, v);
}

int ls_entry_value(const LsLocale *loc, const char *line, const char *key,
                   const char **value) {
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0) return -1;

    const char *p = line + klen;
    if (*p == '=') { *value = p + 1; return 0; }
    if (*p != '[') return -1;

    const char *end = strchr(p + 1, ']');
    if (!end || end[1] != '=') return -1;

    char want[64];
    size_t n = (size_t)(end - p - 1);
    if (n == 0 || n >= sizeof(want)) return -1;
    memcpy(want, p + 1, n);
    want[n] = '\0';
    char *cut = strpbrk(want, ".@");
    if (cut) *cut = '\0';

    if (loc->full[0] && strcmp(want, loc->full) == 0) { *value = end + 2; return 2; }
    if (loc->lang[0] && strcmp(want, loc->lang) == 0) { *value = end + 2; return 1; }
    return -1;
}

/* ── folding ─────────────────────────────────────────────────────────── */

/* Compatibility-decompose, drop the combining marks, then case fold. The
 * decomposition is what makes "Télégram" reachable by typing "tele" and
 * "Ёлка" by typing "елка" — the accent becomes a separate codepoint and then
 * goes. Both the row and the query take this path, so the two always agree. */
static gchar *fold_alloc(const char *in) {
    gchar *norm = g_utf8_normalize(in, -1, G_NORMALIZE_ALL);
    if (!norm) return NULL;

    /* Dropping codepoints never grows the string, so the normalized length is
     * a safe bound for the copy. */
    gchar *bare = g_malloc(strlen(norm) + 1);
    gchar *w = bare;
    for (const gchar *p = norm; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (g_unichar_type(c) == G_UNICODE_NON_SPACING_MARK) continue;
        w += g_unichar_to_utf8(c, w);
    }
    *w = '\0';
    g_free(norm);

    gchar *folded = g_utf8_casefold(bare, -1);
    g_free(bare);
    if (!folded) return NULL;

    /* Squeeze the whitespace out while we are here: a Keywords= list ends in
     * its separator, so the alias would otherwise carry a double space in the
     * middle of it, and a query with a stray space would match nothing that
     * did not have the same stray space. Both sides come through here, so
     * both end up spelling a gap the same way. */
    gchar *squeeze = folded;
    int gap = 0, any = 0;
    for (const gchar *r = folded; *r; r++) {
        /* Byte-wise is safe: no byte of a multibyte character is ever ASCII. */
        if (*r == ' ' || *r == '\t' || *r == '\n') { gap = any; continue; }
        if (gap) { *squeeze++ = ' '; gap = 0; }
        *squeeze++ = *r;
        any = 1;
    }
    *squeeze = '\0';
    return folded;
}

void ls_fold(char *out, size_t cap, const char *in) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!in || !in[0] || cap < 2) return;

    if (!g_utf8_validate(in, -1, NULL)) {
        /* A .desktop file in some legacy encoding is not worth failing over,
         * and GLib's UTF-8 calls would refuse it. Lower the ASCII half and
         * leave the rest; it still matches what an ASCII query asks for. */
        size_t i = 0;
        for (; in[i] && i + 1 < cap; i++) out[i] = (char)tolower((unsigned char)in[i]);
        out[i] = '\0';
        return;
    }

    gchar *folded = fold_alloc(in);
    if (!folded) return;

    size_t n = strlen(folded);
    if (n >= cap) {
        /* Cut before the codepoint that does not fit, not through it: half a
         * character would never match anything and could not be printed. */
        n = cap - 1;
        while (n > 0 && ((unsigned char)folded[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, folded, n);
    out[n] = '\0';
    g_free(folded);
}

void ls_fold_append(char *out, size_t cap, const char *in) {
    if (!out || cap == 0 || !in || !in[0]) return;
    size_t n = strlen(out);
    if (n > 0) {
        if (n + 2 >= cap) return;
        out[n++] = ' ';
        out[n] = '\0';
    }
    if (n + 1 >= cap) return;
    ls_fold(out + n, cap - n, in);
    if (out[n] == '\0' && n > 0) out[n - 1] = '\0';  /* nothing appended: drop the space */
}

void ls_exec_binary(const char *exec, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!exec) return;

    for (const char *tok = exec; ; ) {
        while (*tok == ' ' || *tok == '\t') tok++;
        if (!*tok) return;
        const char *end = tok;
        while (*end && *end != ' ' && *end != '\t') end++;

        /* An assignment is a variable being set, not the program being run. */
        int assign = 0;
        for (const char *c = tok; c < end && !assign; c++) assign = (*c == '=');
        if (!assign) {
            /* Path taken off first, so the wrapper is recognised however the
             * entry spells it — "env" and "/usr/bin/env" are the same word. */
            size_t len = (size_t)(end - tok);
            for (size_t i = len; i > 0; i--) {
                if (tok[i - 1] != '/') continue;
                tok += i;
                len -= i;
                break;
            }
            if (!(len == 3 && strncmp(tok, "env", 3) == 0)) {
                if (len >= cap) len = cap - 1;
                memcpy(out, tok, len);
                out[len] = '\0';
                return;
            }
        }
        tok = end;
    }
}

/* ── ranking ─────────────────────────────────────────────────────────── */

/* Every byte here is folded UTF-8, so the separators we look for are ASCII
 * and a continuation byte can never be mistaken for one. */
static int at_word_start(const char *hay, size_t off) {
    if (off == 0) return 1;
    char prev = hay[off - 1];
    return prev == ' ' || prev == '-' || prev == '_' || prev == '.' ||
           prev == '/' || prev == ':' || prev == ';';
}

/* The query's codepoints in order, gaps allowed. Returns how many codepoints
 * the match spans — never fewer than the query is long — or -1 when the
 * letters are not all there in order. */
static int fuzzy_span(const char *hay, const char *q) {
    const char *h = hay;
    int hi = 0, first = -1, last = -1;
    for (const char *qp = q; *qp; qp = g_utf8_next_char(qp)) {
        gunichar qc = g_utf8_get_char(qp);
        int found = 0;
        while (*h) {
            gunichar hc = g_utf8_get_char(h);
            h = g_utf8_next_char(h);
            hi++;
            if (hc == qc) {
                if (first < 0) first = hi - 1;
                last = hi - 1;
                found = 1;
                break;
            }
        }
        if (!found) return -1;
    }
    return first < 0 ? -1 : last - first;
}

int ls_rank(const char *name, const char *alias, const char *query, int *score) {
    int dummy;
    if (!score) score = &dummy;
    *score = 0;
    if (!name) name = "";
    if (!query || !query[0]) return LS_TIER_EXACT;

    if (strcmp(name, query) == 0) return LS_TIER_EXACT;

    const char *p = strstr(name, query);
    if (p) {
        size_t off = (size_t)(p - name);
        *score = (int)off;
        if (off == 0) return LS_TIER_PREFIX;
        return at_word_start(name, off) ? LS_TIER_WORD : LS_TIER_SUB;
    }

    if (alias && alias[0]) {
        p = strstr(alias, query);
        if (p) {
            *score = (int)(p - alias);
            return LS_TIER_ALIAS;
        }
    }

    /* Fuzzy is the last resort and the loosest, so it is fenced: the letters
     * have to sit reasonably close together. Without that, a three-letter
     * query would "match" every 200-character booru filename in the wallpaper
     * folder and bury the rows that actually contain it. */
    int qlen = (int)g_utf8_strlen(query, -1);
    if (qlen < 2) return LS_TIER_NONE;
    int span = fuzzy_span(name, query);
    if (span < 0 || span > qlen * 4 + 8) return LS_TIER_NONE;
    /* Score the gaps, not the span: "ffx" against "firefox" is a tighter
     * match than against "far from fixed", whatever the queries' lengths. */
    *score = span - (qlen - 1);
    return LS_TIER_FUZZY;
}

/* ── frecency ────────────────────────────────────────────────────────── */

/* Room for a few hundred applications and wallpapers, which is more than a
 * machine tends to have installed; the weakest entry is evicted past that. */
#define LS_FREC_MAX       512
#define LS_KEY_MAX        256
#define LS_HALFLIFE_DAYS  10.0
/* Below this a row has not been touched in months and is only taking up a
 * line in the file. */
#define LS_FREC_FLOOR     0.02

typedef struct {
    char      key[LS_KEY_MAX];
    double    score;
    long long last;
} LsFrecEnt;

struct LsFrec {
    LsFrecEnt e[LS_FREC_MAX];
    int       n;
};

static double aged(double score, long long last, time_t now) {
    if (score <= 0.0) return 0.0;
    double days = ((double)now - (double)last) / 86400.0;
    if (days <= 0.0) return score;   /* clock went backwards; do not punish it */
    return score * pow(0.5, days / LS_HALFLIFE_DAYS);
}

static int ent_cmp(const void *va, const void *vb) {
    return strcmp(((const LsFrecEnt *)va)->key, ((const LsFrecEnt *)vb)->key);
}

void ls_frec_path(char *buf, size_t cap) {
    const char *state = getenv("XDG_STATE_HOME");
    const char *home  = getenv("HOME");
    if (state && state[0]) snprintf(buf, cap, "%s/fwm/launcher", state);
    else if (home)         snprintf(buf, cap, "%s/.local/state/fwm/launcher", home);
    else                   snprintf(buf, cap, ".fwm-launcher");
}

static void mkdir_parents(const char *file) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", file);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(dir, 0755);
        *p = '/';
    }
    mkdir(dir, 0755);
}

LsFrec *ls_frec_load(const char *path) {
    LsFrec *f = calloc(1, sizeof(*f));
    if (!f) return NULL;

    FILE *fp = path ? fopen(path, "r") : NULL;
    if (!fp) return f;   /* no history yet is a valid history */

    char line[LS_KEY_MAX + 64];
    while (f->n < LS_FREC_MAX && fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        double score = 0.0;
        long long last = 0;
        int off = 0;
        /* %n does not count toward the return value, so two is the whole of
         * what sscanf can report here. */
        if (sscanf(line, "%lf %lld %n", &score, &last, &off) < 2) continue;
        if (off <= 0 || !line[off]) continue;
        if (score <= 0.0) continue;

        LsFrecEnt *e = &f->e[f->n];
        snprintf(e->key, sizeof(e->key), "%s", line + off);
        e->score = score;
        e->last = last;
        f->n++;
    }
    fclose(fp);

    qsort(f->e, (size_t)f->n, sizeof(f->e[0]), ent_cmp);
    return f;
}

void ls_frec_free(LsFrec *f) {
    free(f);
}

static LsFrecEnt *frec_find(const LsFrec *f, const char *key) {
    if (!f || f->n == 0 || !key || !key[0]) return NULL;
    LsFrecEnt probe;
    snprintf(probe.key, sizeof(probe.key), "%s", key);
    return bsearch(&probe, f->e, (size_t)f->n, sizeof(f->e[0]), ent_cmp);
}

double ls_frec_score(const LsFrec *f, const char *key, time_t now) {
    const LsFrecEnt *e = frec_find(f, key);
    return e ? aged(e->score, e->last, now) : 0.0;
}

void ls_frec_bump(LsFrec *f, const char *key, time_t now) {
    if (!f || !key || !key[0]) return;

    LsFrecEnt *e = frec_find(f, key);
    if (e) {
        /* Age first, then add one: the increment is always worth the same,
         * and what it is added to is what the row is worth today. */
        e->score = aged(e->score, e->last, now) + 1.0;
        e->last = (long long)now;
        return;
    }

    if (f->n >= LS_FREC_MAX) {
        int worst = 0;
        double worst_score = aged(f->e[0].score, f->e[0].last, now);
        for (int i = 1; i < f->n; i++) {
            double s = aged(f->e[i].score, f->e[i].last, now);
            if (s < worst_score) { worst_score = s; worst = i; }
        }
        f->e[worst] = f->e[f->n - 1];
        f->n--;
    }

    e = &f->e[f->n++];
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->score = 1.0;
    e->last = (long long)now;
    qsort(f->e, (size_t)f->n, sizeof(f->e[0]), ent_cmp);
}

int ls_frec_save(const LsFrec *f, const char *path, time_t now) {
    if (!f || !path || !path[0]) return -1;
    mkdir_parents(path);

    char tmp[600];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;

    FILE *fp = fopen(tmp, "w");
    if (!fp) return -1;
    for (int i = 0; i < f->n; i++) {
        if (aged(f->e[i].score, f->e[i].last, now) < LS_FREC_FLOOR) continue;
        /* Stored undecayed with its timestamp, so ageing happens on read and
         * a file that sits untouched for a month does not lose anything the
         * next open would not also have taken off it. */
        fprintf(fp, "%.4f %lld %s\n", f->e[i].score, f->e[i].last, f->e[i].key);
    }
    int err = ferror(fp);
    if (fclose(fp) != 0 || err) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}
