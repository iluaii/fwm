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


#ifndef FWM_CONFIG_INTERNAL_H
#define FWM_CONFIG_INTERNAL_H

#include "config.h"
#include "toml.h"
#include "defines.h"
#include <xkbcommon/xkbcommon.h>

/* Shared between the three files config.c is split across:
 *
 *   config.c        reading the file, and the sections that describe the
 *                   world — physics, decor, wallpaper, window rules — plus
 *                   the public API
 *   config_binds.c  everything that turns a gesture into an action: keys,
 *                   submaps, mouse buttons and touchpad swipes, and the list
 *                   of action names that keeps a typo from being silent
 *   config_set.c    the runtime-settable option table, which is a different
 *                   kind of thing again: it exists for `fwmctl set`, not for
 *                   the file, and deliberately covers less
 *
 * Nothing here is public; config.h is what the compositor sees. */

/* Accumulate a diagnostic for the tray's config pill. */
void config_report_error(FwmConfig *cfg, const char *fmt, ...);

/* "super" -> its modifier bit, and "super+shift+q" -> mask plus keysym. Shared
 * because a bind, a mouse button and a gesture all start the same way. */
unsigned int parse_mod_token(const char *tok);
int parse_bind_key(const char *str, unsigned int *mod_out, xkb_keysym_t *key_out);

/* Is this an action the compositor will actually run? Checked at load time, so
 * a typo is reported then rather than doing nothing when pressed. */
int action_is_known(const char *a);

/* "#RRGGBB" or "#RRGGBBAA" into premultiplied RGBA floats; 0 on bad input.
 * Both the file and `fwmctl set` accept colours, and they must accept exactly
 * the same ones. */
int parse_hex_color(const char *s, float out[4]);

/* Expand a leading "~/" — config paths are hand-written, and the shell that
 * would normally do this is not involved. */
void expand_tilde(const char *in, char *out, size_t cap);

/* The bindings sections, loaded by config_load beside all the others. */
void load_binds(toml_table_t *root, FwmConfig *cfg);
void load_modes(toml_table_t *root, FwmConfig *cfg);
void load_radial(toml_table_t *root, FwmConfig *cfg);
void load_mouse(toml_table_t *root, FwmConfig *cfg);
void load_gestures(toml_table_t *root, FwmConfig *cfg);

/* The built-in keymap, which a config too broken to parse falls back to. That
 * fallback is the reason a bad config never costs the session. */
void apply_default_binds(FwmConfig *cfg);

#endif /* FWM_CONFIG_INTERNAL_H */
