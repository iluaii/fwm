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

#ifndef FWM_CREW_H
#define FWM_CREW_H

/* A crew: a fixed number of threads that do one job at a time, all of them on
 * it at once, the thread that asked included.
 *
 * Made for loops that are split into as many parts as there are members, and
 * for jobs that go through several such loops one after another with a
 * crew_sync between them — a conjugate gradient solve is dozens of them a
 * step. So a member does not go to sleep the moment a job is done: it waits
 * for the next one spinning, for a fraction of a millisecond, and only then
 * on a condition variable. Inside a step that is no sleeps at all; between
 * steps, and while there is nothing to step, it is no CPU at all.
 *
 * Knows nothing of what it runs. NULL is a crew of one: every call works on
 * it, and runs the job on the caller. */

typedef struct FwmCrew FwmCrew;

/* `fn` is run once per member, `part` 0 .. `parts` - 1, part 0 on the caller. */
typedef void (*CrewJob)(void *ctx, int part, int parts);

/* `size` members, the caller one of them, so size - 1 threads are started.
 * NULL for a size under 2, or when the threads cannot be had — a crew of
 * one, which is still a crew. */
FwmCrew *crew_create(int size);
void crew_destroy(FwmCrew *c);
int crew_size(const FwmCrew *c);

/* Run `fn` on every member and return once all of them have finished. Not
 * re-entrant: a job does not run another job on its own crew. */
void crew_run(FwmCrew *c, CrewJob fn, void *ctx);

/* Inside a job: wait here until every member has got here. Everything a
 * member wrote before it is seen by every member after it. */
void crew_sync(FwmCrew *c);

/* The share of `n` items that falls to `part` of `parts`: [*lo, *hi), the
 * shares differing by one item at most and covering 0 .. n in order. */
static inline void crew_share(int n, int part, int parts, int *lo, int *hi) {
    *lo = (int)((long long)n * part / parts);
    *hi = (int)((long long)n * (part + 1) / parts);
}

/* How many members to give a crew when nobody said: one per core, counting
 * the two threads of a core with SMT as one — the loops this is for are bound
 * by memory more than arithmetic, and a second thread on a core adds little
 * to them and takes it from everything else — and no more than `most`. */
int crew_default_size(int most);

#endif
