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

#ifndef FWM_CLIPBOARD_H
#define FWM_CLIPBOARD_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The seat's selection, and the one thing about Wayland's that surprises
 * everybody: the clipboard is not a place where the text is kept. It is a
 * PROMISE by the window you copied from to hand the bytes over when somebody
 * asks — so closing that window takes the clipboard with it. Copy a command out
 * of a terminal, close the terminal, paste: nothing. Every desktop answers this
 * with a daemon (wl-clip-persist, cliphist, clipman); fwm answers it here,
 * because a compositor is already the one holding the promise and reading it
 * once costs less than a second process that does nothing else.
 *
 * Two jobs, one module:
 *
 *  - OFFERING bytes fwm itself owns. A screenshot is a selection with no client
 *    behind it at all (src/screenshot.c), and the handover cannot be one
 *    blocking write — a pipe holds 64K, a screenshot is most of a megabyte, and
 *    a write that insisted on finishing would park the compositor until the
 *    pasting application got round to reading. Each transfer gets its own
 *    event-loop writer and its own copy of the bytes.
 *
 *  - KEEPING what a client copied. When a client takes the selection its text
 *    is read once, into memory here — once the selection has been still for a
 *    moment, never in the same breath as the copy, because a client asked for
 *    one flavour while it is still arranging the others can be wedged by the
 *    question. That wait narrows the window rather than closing it: nothing in
 *    the protocol says when a client has finished offering (clipboard.c, "the
 *    wait before asking"). When that client dies and the selection would go
 *    empty, fwm offers the copy in its place. TEXT ONLY, deliberately:
 *    the point is the command you copied out of a terminal you have since
 *    closed, and quietly holding on to megabytes of image data that a paste may
 *    never come for is a memory leak with a feature's name on it.
 *
 * What survives is the LAST thing copied, not a history. A history is a
 * different feature with a UI attached (cliphist and friends), and this one has
 * no window and no keybinding.
 */

typedef struct FwmClipboard FwmClipboard;

struct wl_display;
struct wlr_seat;

FwmClipboard *clipboard_create(struct wl_display *display, struct wlr_seat *seat);
void clipboard_destroy(FwmClipboard *cb);

/* Real user input arrived. fwm reads a client's selection into memory only
 * once the user has taken their hands off for a moment — a paste is a keystroke
 * or a middle click, and a read that overlaps one is a read racing the user for
 * the client's single clipboard slot. See clipboard.c, "the wait before
 * asking". Called from server_notify_activity, so every path that counts as
 * activity anywhere counts here too. */
void clipboard_note_activity(FwmClipboard *cb);

/* [clipboard] persist and max_kb, live. Switching persistence off drops what is
 * being held: a setting that says "do not keep my clipboard" and then keeps the
 * last one anyway is not the setting it says it is. */
void clipboard_configure(FwmClipboard *cb, bool persist, size_t max_bytes);

/* Offer `data` as the selection under each of `mimes`, taking ownership of it
 * (freed here whether the call succeeds or not). The bytes stay live for as
 * long as the selection does, and every transfer already under way keeps its
 * own copy — so a paste in progress survives the next thing being copied. */
bool clipboard_offer(FwmClipboard *cb, const char *const *mimes, int mime_count,
                     unsigned char *data, size_t len);

#endif /* FWM_CLIPBOARD_H */
