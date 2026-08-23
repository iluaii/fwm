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

/* What the launcher does with what you type.
 *
 * The three things worth pinning are the ones a person would notice and could
 * not easily debug from the panel: which locale's Name a row shows, which of
 * two matching rows comes first, and what a launch recorded last month is
 * still worth today. None of it needs a compositor — that is why it lives in
 * its own file — so all of it is asserted here rather than by opening the
 * launcher and squinting at the order.
 */

#include "test.h"
#include "launcher_search.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DAY 86400

/* Rank folded strings the way refilter() does, so the tests exercise the same
 * path the launcher takes rather than raw byte comparisons it never makes. */
static int rank_of(const char *name, const char *alias, const char *query, int *score) {
    char fn[LS_FOLD_MAX], fa[LS_ALIAS_MAX], fq[256];
    ls_fold(fn, sizeof(fn), name);
    ls_fold(fa, sizeof(fa), alias);
    ls_fold(fq, sizeof(fq), query);
    return ls_rank(fn, fa, fq, score);
}

static void test_locale(void) {
    CASE("locale");

    LsLocale loc;
    ls_locale_from(&loc, "ru_RU.UTF-8");
    CHECK_STR(loc.lang, "ru");
    CHECK_STR(loc.full, "ru_RU");

    /* A modifier is part of neither half. */
    ls_locale_from(&loc, "sr_RS@latin");
    CHECK_STR(loc.lang, "sr");
    CHECK_STR(loc.full, "sr_RS");

    /* No country: "full" would only repeat the language, so it stays empty
     * and Name[de_DE] does not get treated as a match for Name[de]. */
    ls_locale_from(&loc, "de");
    CHECK_STR(loc.lang, "de");
    CHECK_STR(loc.full, "");

    /* The C locale asks for the unlocalised keys and nothing else. */
    ls_locale_from(&loc, "C");
    CHECK_STR(loc.lang, "");
    ls_locale_from(&loc, "POSIX");
    CHECK_STR(loc.lang, "");
    ls_locale_from(&loc, NULL);
    CHECK_STR(loc.lang, "");
}

static void test_entry_keys(void) {
    CASE("entry keys");

    LsLocale ru;
    ls_locale_from(&ru, "ru_RU.UTF-8");
    const char *v = NULL;

    CHECK_INT(ls_entry_value(&ru, "Name=Settings", "Name", &v), 0);
    CHECK_STR(v, "Settings");

    /* Language beats no language, country beats language — which is what lets
     * the caller keep the best it has seen with a single > test. */
    CHECK_INT(ls_entry_value(&ru, "Name[ru]=Параметры", "Name", &v), 1);
    CHECK_STR(v, "Параметры");
    CHECK_INT(ls_entry_value(&ru, "Name[ru_RU]=Настройки", "Name", &v), 2);
    CHECK_STR(v, "Настройки");

    /* The encoding a key spells is noise; the language it names is not. */
    CHECK_INT(ls_entry_value(&ru, "Name[ru_RU.UTF-8]=Настройки", "Name", &v), 2);

    /* Someone else's language is not a candidate at all. */
    CHECK_INT(ls_entry_value(&ru, "Name[de]=Einstellungen", "Name", &v), -1);

    /* Keys that merely start alike must not collide in either direction. */
    CHECK_INT(ls_entry_value(&ru, "GenericName=Text Editor", "Name", &v), -1);
    CHECK_INT(ls_entry_value(&ru, "Name=Kate", "GenericName", &v), -1);
    CHECK_INT(ls_entry_value(&ru, "NameServer=x", "Name", &v), -1);

    CHECK_INT(ls_entry_value(&ru, "Keywords[ru]=браузер;интернет;", "Keywords", &v), 1);
    CHECK_STR(v, "браузер;интернет;");

    /* Malformed suffixes are skipped, not half-parsed. */
    CHECK_INT(ls_entry_value(&ru, "Name[ru=X", "Name", &v), -1);
    CHECK_INT(ls_entry_value(&ru, "Name[]=X", "Name", &v), -1);

    /* An empty value is still a value; the caller decides what to do with it. */
    CHECK_INT(ls_entry_value(&ru, "Name=", "Name", &v), 0);
    CHECK_STR(v, "");
}

static void test_fold(void) {
    CASE("fold");

    char out[LS_FOLD_MAX];

    ls_fold(out, sizeof(out), "FireFox");
    CHECK_STR(out, "firefox");

    /* The whole point of not using strcasecmp: it lowers ASCII and leaves
     * every Russian capital exactly where it was. */
    ls_fold(out, sizeof(out), "НАСТРОЙКИ");
    char lower[LS_FOLD_MAX];
    ls_fold(lower, sizeof(lower), "настройки");
    CHECK_STR(out, lower);

    /* Accents come off, so a name can be reached from a keyboard that cannot
     * type them — and so can Ё, which is the same mechanism. */
    char plain[LS_FOLD_MAX];
    ls_fold(out, sizeof(out), "Télégram");
    ls_fold(plain, sizeof(plain), "telegram");
    CHECK_STR(out, plain);
    ls_fold(out, sizeof(out), "Ёлка");
    ls_fold(plain, sizeof(plain), "елка");
    CHECK_STR(out, plain);

    /* Truncation cuts between characters. A half-written codepoint would
     * match nothing and could not be drawn. */
    char tiny[6];
    ls_fold(tiny, sizeof(tiny), "ёлка");
    CHECK(strlen(tiny) < sizeof(tiny));
    CHECK(g_utf8_validate(tiny, -1, NULL));

    ls_fold(out, sizeof(out), "");
    CHECK_STR(out, "");

    /* Gaps are spelled one way on both sides, so a Keywords= list ending in
     * its separator and a query with a stray space still meet. */
    ls_fold(out, sizeof(out), "  Web   Browser \t");
    CHECK_STR(out, "web browser");
    ls_fold(out, sizeof(out), "   ");
    CHECK_STR(out, "");

    CASE("fold append");
    char alias[LS_ALIAS_MAX] = "";
    ls_fold_append(alias, sizeof(alias), "Web Browser");
    CHECK_STR(alias, "web browser");
    ls_fold_append(alias, sizeof(alias), "FIREFOX");
    CHECK_STR(alias, "web browser firefox");
    /* What a Keywords= line actually looks like once its semicolons are
     * spaces: a trailing separator must not become a double space. */
    ls_fold_append(alias, sizeof(alias), "internet www ");
    CHECK_STR(alias, "web browser firefox internet www");
    /* Nothing to add leaves no dangling separator behind. */
    ls_fold_append(alias, sizeof(alias), "");
    CHECK_STR(alias, "web browser firefox internet www");
    ls_fold_append(alias, sizeof(alias), "   ");
    CHECK_STR(alias, "web browser firefox internet www");
}

static void test_exec_binary(void) {
    CASE("exec binary");

    char out[128];
    ls_exec_binary("firefox", out, sizeof(out));
    CHECK_STR(out, "firefox");
    ls_exec_binary("/usr/bin/firefox --new-window", out, sizeof(out));
    CHECK_STR(out, "firefox");

    /* Field codes are stripped before this runs, so what arrives can still
     * end in the spaces they left behind. */
    ls_exec_binary("/usr/bin/gimp-2.10  ", out, sizeof(out));
    CHECK_STR(out, "gimp-2.10");

    /* A wrapper and the assignments it introduces are not the program —
     * however the entry spells the wrapper. */
    ls_exec_binary("env GDK_BACKEND=x11 inkscape", out, sizeof(out));
    CHECK_STR(out, "inkscape");
    ls_exec_binary("/usr/bin/env LC_ALL=C /opt/thing/bin/thing --gui", out, sizeof(out));
    CHECK_STR(out, "thing");
    ls_exec_binary("QT_SCALE_FACTOR=2 telegram-desktop", out, sizeof(out));
    CHECK_STR(out, "telegram-desktop");

    /* Nothing but a wrapper is nothing to search on, not a crash. */
    ls_exec_binary("env FOO=1", out, sizeof(out));
    CHECK_STR(out, "");
    ls_exec_binary("", out, sizeof(out));
    CHECK_STR(out, "");
}

static void test_rank(void) {
    CASE("rank tiers");

    int score = 0;

    /* An empty query matches everything at the top tier, which is what hands
     * the opening order to frecency instead of to the alphabet. */
    CHECK_INT(rank_of("Firefox", "", "", &score), LS_TIER_EXACT);
    CHECK_INT(rank_of("Firefox", "", "firefox", &score), LS_TIER_EXACT);
    CHECK_INT(rank_of("Firefox", "", "fire", &score), LS_TIER_PREFIX);

    /* A query landing on a word boundary beats one landing mid-word. */
    CHECK_INT(rank_of("GNU Image Manipulation Program", "", "image", &score),
              LS_TIER_WORD);
    CHECK_INT(rank_of("Firefox", "", "fox", &score), LS_TIER_SUB);

    /* Names alone would never find this; the keywords do. */
    CHECK_INT(rank_of("Firefox", "Web Browser;internet;firefox", "browser", &score),
              LS_TIER_ALIAS);
    /* And the binary is part of the alias, so what you type in a shell works. */
    CHECK_INT(rank_of("Neovim", "Text Editor nvim", "nvim", &score), LS_TIER_ALIAS);

    /* The letters in order, gaps allowed — the thing the README always
     * promised and the substring match never did. */
    CHECK_INT(rank_of("Firefox", "", "ffx", &score), LS_TIER_FUZZY);
    CHECK_INT(rank_of("Firefox", "", "zzz", &score), LS_TIER_NONE);

    /* One letter is never fuzzy: if it were there it would have matched as a
     * substring, and if it is not, "every row containing an e" is not an
     * answer to anything. */
    CHECK_INT(rank_of("Firefox", "", "z", &score), LS_TIER_NONE);

    CASE("rank fence");
    /* Fuzzy is fenced by how far the letters are spread. Without this, three
     * letters would "match" every 200-character booru filename in the
     * wallpaper folder and bury the rows that actually contain them. */
    CHECK_INT(rank_of("abcdefghijklmnopqrstuvwxyz0123456789", "", "az9", &score),
              LS_TIER_NONE);
    CHECK_INT(rank_of("abcdefgh", "", "ah", &score), LS_TIER_FUZZY);

    CASE("rank scores");
    /* Within a tier, smaller is better: how far in the match sits, and for a
     * fuzzy one how many characters it had to skip. */
    rank_of("Firefox", "", "fox", &score);
    CHECK_INT(score, 4);
    rank_of("Firefox", "", "ffx", &score);
    CHECK_INT(score, 4);      /* f..f..x across seven letters: four skipped */
    rank_of("Firefox", "", "fire", &score);
    CHECK_INT(score, 0);

    CASE("rank cyrillic");
    /* The case that used to fail outright: a capital query against a
     * lowercase name, in an alphabet strcasecmp has never heard of. */
    CHECK_INT(rank_of("Настройки", "", "НАСТР", &score), LS_TIER_PREFIX);
    CHECK_INT(rank_of("Параметры системы", "", "системы", &score), LS_TIER_WORD);
}

static void test_frecency(void) {
    CASE("frecency");

    char path[256];
    snprintf(path, sizeof(path), "test_launcher_frec.%d", (int)getpid());
    unlink(path);

    /* No history yet is a history, not a failure: a fresh install has to
     * open. */
    LsFrec *f = ls_frec_load(path);
    CHECK_NOT_NULL(f);
    time_t t0 = 1700000000;
    CHECK_DBL(ls_frec_score(f, "firefox", t0), 0.0, 1e-9);

    ls_frec_bump(f, "firefox", t0);
    CHECK_DBL(ls_frec_score(f, "firefox", t0), 1.0, 1e-9);
    ls_frec_bump(f, "firefox", t0);
    CHECK_DBL(ls_frec_score(f, "firefox", t0), 2.0, 1e-9);

    /* Aged by a half-life rather than counted forever, so habits that change
     * are followed without anything having to be reset by hand. */
    CHECK_DBL(ls_frec_score(f, "firefox", t0 + 10 * DAY), 1.0, 1e-6);
    CHECK_DBL(ls_frec_score(f, "firefox", t0 + 20 * DAY), 0.5, 1e-6);

    /* A launch today is worth its full one, whatever is under it. */
    ls_frec_bump(f, "firefox", t0 + 10 * DAY);
    CHECK_DBL(ls_frec_score(f, "firefox", t0 + 10 * DAY), 2.0, 1e-6);

    /* The key is a command line, spaces and all. */
    ls_frec_bump(f, "/usr/bin/kitty -e ranger", t0);
    CHECK_DBL(ls_frec_score(f, "/usr/bin/kitty -e ranger", t0), 1.0, 1e-9);

    CASE("frecency round trip");
    CHECK_INT(ls_frec_save(f, path, t0 + 10 * DAY), 0);
    LsFrec *g = ls_frec_load(path);
    CHECK_NOT_NULL(g);
    CHECK_DBL(ls_frec_score(g, "firefox", t0 + 10 * DAY), 2.0, 1e-3);
    CHECK_DBL(ls_frec_score(g, "/usr/bin/kitty -e ranger", t0), 1.0, 1e-3);
    CHECK_DBL(ls_frec_score(g, "never-run", t0), 0.0, 1e-9);
    ls_frec_free(g);

    CASE("frecency pruning");
    /* Written a year on, everything in it has aged past the floor and the
     * file comes back empty rather than growing forever. */
    CHECK_INT(ls_frec_save(f, path, t0 + 365 * DAY), 0);
    g = ls_frec_load(path);
    CHECK_DBL(ls_frec_score(g, "firefox", t0), 0.0, 1e-9);
    ls_frec_free(g);
    ls_frec_free(f);

    CASE("frecency junk");
    /* A truncated or hand-edited file must not take the launcher down with
     * it; unparseable lines are simply not history. */
    FILE *fp = fopen(path, "w");
    CHECK_NOT_NULL(fp);
    fputs("garbage\n"
          "3.0\n"
          "2.5 1700000000 kept\n"
          "-1.0 1700000000 negative\n"
          "1.0 1700000000\n",
          fp);
    fclose(fp);
    f = ls_frec_load(path);
    CHECK_NOT_NULL(f);
    CHECK_DBL(ls_frec_score(f, "kept", t0), 2.5, 1e-9);
    CHECK_DBL(ls_frec_score(f, "negative", t0), 0.0, 1e-9);
    ls_frec_free(f);

    unlink(path);
}

static void test_overflow(void) {
    CASE("frecency overflow");

    char path[256];
    snprintf(path, sizeof(path), "test_launcher_frec_big.%d", (int)getpid());
    unlink(path);

    LsFrec *f = ls_frec_load(path);
    CHECK_NOT_NULL(f);
    time_t t0 = 1700000000;

    /* Past capacity the weakest entry goes, not the newest: an application
     * launched once a year ago is what a full store can afford to forget. */
    char key[32];
    for (int i = 0; i < 600; i++) {
        snprintf(key, sizeof(key), "app%03d", i);
        ls_frec_bump(f, key, t0 - 300 * DAY);
    }
    ls_frec_bump(f, "daily", t0);
    ls_frec_bump(f, "daily", t0);
    CHECK_DBL(ls_frec_score(f, "daily", t0), 2.0, 1e-9);

    ls_frec_free(f);
    unlink(path);
}

int main(void) {
    test_locale();
    test_entry_keys();
    test_fold();
    test_exec_binary();
    test_rank();
    test_frecency();
    test_overflow();
    return t_report("launcher");
}
