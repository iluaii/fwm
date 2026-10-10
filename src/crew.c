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

#include "crew.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* How long a member spins for the next job before it sleeps, and how long
 * anyone spins at a crew_sync before it starts giving the core away, in
 * turns of the loop. A turn is a pause instruction, tens of cycles: the first
 * is a few tenths of a millisecond, longer than any gap between the jobs of
 * one step; the second is far longer than members of an evenly split loop
 * ever wait for each other, so it is reached only when the system has taken
 * a member's core. */
#define IDLE_SPINS 20000
#define SYNC_SPINS 4096

static inline void relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

struct member {
    FwmCrew *crew;
    int part;
    pthread_t thread;
};

struct FwmCrew {
    int size;
    struct member *members;   /* size - 1 of them; the caller is part 0 */

    CrewJob fn;
    void *ctx;
    /* Bumped once per job: what a member waits on. Writing fn and ctx before
     * it is what publishes them. */
    atomic_uint job;
    atomic_int left;          /* members still on the job, the caller not counted */
    atomic_bool quit;

    pthread_mutex_t lock;
    pthread_cond_t wake;
    atomic_int sleepers;

    /* crew_sync: how many have arrived, and which round it is. */
    atomic_int arrived;
    atomic_uint round;
};

static void *member_main(void *data) {
    struct member *m = data;
    FwmCrew *c = m->crew;
    /* Not read from the crew: the first job can be handed out before this
     * thread has even started, and taken for one already done it would be
     * waited for by everyone else forever. Nothing is run before the crew is
     * made, so the count starts at nothing. */
    unsigned seen = 0;
    for (;;) {
        unsigned now = atomic_load_explicit(&c->job, memory_order_acquire);
        for (int s = 0; now == seen && s < IDLE_SPINS; s++) {
            relax();
            now = atomic_load_explicit(&c->job, memory_order_acquire);
        }
        if (now == seen) {
            /* Counted as asleep BEFORE looking again, and the caller looks at
             * the count AFTER bumping the job: one of the two always sees the
             * other, so a job is never left unannounced to a sleeper. */
            pthread_mutex_lock(&c->lock);
            atomic_fetch_add(&c->sleepers, 1);
            while ((now = atomic_load(&c->job)) == seen && !atomic_load(&c->quit))
                pthread_cond_wait(&c->wake, &c->lock);
            atomic_fetch_sub(&c->sleepers, 1);
            pthread_mutex_unlock(&c->lock);
        }
        if (atomic_load(&c->quit)) break;
        seen = now;
        c->fn(c->ctx, m->part, c->size);
        atomic_fetch_sub_explicit(&c->left, 1, memory_order_release);
    }
    return NULL;
}

FwmCrew *crew_create(int size) {
    if (size < 2) return NULL;
    FwmCrew *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->members = calloc((size_t)(size - 1), sizeof(*c->members));
    if (!c->members) { free(c); return NULL; }
    atomic_init(&c->job, 0);
    atomic_init(&c->left, 0);
    atomic_init(&c->quit, false);
    atomic_init(&c->sleepers, 0);
    atomic_init(&c->arrived, 0);
    atomic_init(&c->round, 0);
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->wake, NULL);

    int started = 0;
    for (int k = 0; k < size - 1; k++) {
        c->members[k].crew = c;
        c->members[k].part = k + 1;
        if (pthread_create(&c->members[k].thread, NULL, member_main, &c->members[k]) != 0)
            break;
        started++;
    }
    c->size = size;
    if (started < size - 1) {
        /* A crew short of members would wait forever at its first sync. */
        c->size = started + 1;
        crew_destroy(c);
        return NULL;
    }
    return c;
}

void crew_destroy(FwmCrew *c) {
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    atomic_store(&c->quit, true);
    pthread_cond_broadcast(&c->wake);
    pthread_mutex_unlock(&c->lock);
    for (int k = 0; k < c->size - 1; k++) pthread_join(c->members[k].thread, NULL);
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->wake);
    free(c->members);
    free(c);
}

int crew_size(const FwmCrew *c) {
    return c ? c->size : 1;
}

void crew_run(FwmCrew *c, CrewJob fn, void *ctx) {
    if (!c) {
        fn(ctx, 0, 1);
        return;
    }
    c->fn = fn;
    c->ctx = ctx;
    atomic_store_explicit(&c->left, c->size - 1, memory_order_relaxed);
    atomic_fetch_add(&c->job, 1);
    if (atomic_load(&c->sleepers) > 0) {
        pthread_mutex_lock(&c->lock);
        pthread_cond_broadcast(&c->wake);
        pthread_mutex_unlock(&c->lock);
    }

    fn(ctx, 0, c->size);

    for (int s = 0; atomic_load_explicit(&c->left, memory_order_acquire) > 0; s++) {
        if (s < SYNC_SPINS) relax();
        else sched_yield();
    }
}

void crew_sync(FwmCrew *c) {
    if (!c) return;
    unsigned r = atomic_load_explicit(&c->round, memory_order_acquire);
    if (atomic_fetch_add_explicit(&c->arrived, 1, memory_order_acq_rel) == c->size - 1) {
        /* The last one in opens the gate. Nobody touches `arrived` again
         * until the round moves, so it can be reset before it does. */
        atomic_store_explicit(&c->arrived, 0, memory_order_relaxed);
        atomic_fetch_add_explicit(&c->round, 1, memory_order_release);
        return;
    }
    for (int s = 0; atomic_load_explicit(&c->round, memory_order_acquire) == r; s++) {
        if (s < SYNC_SPINS) relax();
        else sched_yield();
    }
}

int crew_default_size(int most) {
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online < 1) online = 1;
    /* With SMT on, the kernel's CPUs are two to a core; where it does not
     * say, every one is taken for a core. */
    long per_core = 1;
    FILE *f = fopen("/sys/devices/system/cpu/smt/active", "r");
    if (f) {
        int active = 0;
        if (fscanf(f, "%d", &active) == 1 && active == 1) per_core = 2;
        fclose(f);
    }
    long cores = online / per_core;
    if (cores < 1) cores = 1;
    if (cores > most) cores = most;
    return (int)cores;
}
