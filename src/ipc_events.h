#ifndef FWM_IPC_EVENTS_H
#define FWM_IPC_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The event stream's vocabulary, kept apart from ipc.c on purpose: ipc.c
 * reaches wlroots and cannot be linked into a test binary, while deciding
 * what `subscribe window_open,desktop` means is plain string work that
 * should not need a running compositor to check. */

enum {
    FWM_EV_WINDOW_OPEN   = 1u << 0,
    FWM_EV_WINDOW_CLOSE  = 1u << 1,
    FWM_EV_WINDOW_FOCUS  = 1u << 2,
    FWM_EV_WINDOW_TITLE  = 1u << 3,
    FWM_EV_DESKTOP       = 1u << 4,
    FWM_EV_MODE          = 1u << 5,
    FWM_EV_GRAVITY       = 1u << 6,
    FWM_EV_CONFIG_RELOAD = 1u << 7,

    FWM_EV_COUNT = 8,
    FWM_EV_ALL   = (1u << FWM_EV_COUNT) - 1
};

/* The bit for one event name, or 0 when it is not one of ours. `len` bounds
 * the name so a token can be matched in place, without copying it out. */
uint32_t fwm_ipc_event_bit(const char *name, size_t len);

/* Name of a single event bit, NULL if `bit` is not exactly one of them. */
const char *fwm_ipc_event_name(uint32_t bit);

/* Parse a `subscribe` argument. NULL, empty, or "all" selects everything;
 * otherwise a list of event names separated by commas and/or spaces.
 *
 * All-or-nothing: one unknown name fails the whole request with a message in
 * `err` and leaves `*mask` untouched, because a silently ignored typo would
 * present as "the compositor never sends that event". */
bool fwm_ipc_events_parse(const char *list, uint32_t *mask, char *err, size_t errcap);

/* Every event name, space-separated, for help text and error messages.
 * Writes at most `cap` bytes including the terminator. */
void fwm_ipc_event_list(char *out, size_t cap);

#endif /* FWM_IPC_EVENTS_H */
