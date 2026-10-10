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

#ifndef FWM_LIQUID_FORM_H
#define FWM_LIQUID_FORM_H

#include <stdbool.h>
#include <stdint.h>
#include <wlr/types/wlr_scene.h>

#include "../liquid_kinds.h"

/*
 * The panel a liquid is made or changed in, opened from the liquids menu's +
 * and its gears: a name, a colour — typed as hex or picked off a wheel, shown
 * on the drop at the top — and how thick and how heavy it is, each typed as a
 * number or dragged on a slider beside it.
 *
 * Everything here is the form's own: what is typed, where the caret is, what a
 * drag has hold of. It decides nothing about liquids; the server reads the
 * result back with liquid_form_result when the form says it was submitted.
 */

enum {
    LIQUID_FORM_NAME = 0,
    LIQUID_FORM_HEX,
    LIQUID_FORM_VISC,
    LIQUID_FORM_DENS,
    LIQUID_FORM_FIELDS,
};

typedef enum {
    LIQUID_FORM_IGNORED = 0,   /* not the form's, or nothing changed */
    LIQUID_FORM_CHANGED,       /* redraw it */
    LIQUID_FORM_SUBMIT,        /* Create / Save, and the fields are good */
    LIQUID_FORM_CANCEL,
} LiquidFormResult;

typedef struct {
    bool   editing;            /* Save rather than Create */
    char   text[LIQUID_FORM_FIELDS][LIQUID_NAME_MAX];
    int    focus;              /* the field with the caret, -1 for none */
    double hue, sat, val;      /* the colour, 0..1 each */
    float  alpha;              /* carried over; the form does not change it */
    double viscosity, density;
    int    drag;               /* what the held button is moving */
    int    hover_button;       /* 0 none, 1 cancel, 2 submit */
    char   error[64];
    double opacity;
} LiquidForm;

/* Fill from `from`; `editing` says whether it is an existing liquid. */
void liquid_form_init(LiquidForm *f, const LiquidKind *from, bool editing);
/* What the form now describes. Only meaningful after a SUBMIT. */
void liquid_form_result(const LiquidForm *f, LiquidKind *out);
/* The colour as it stands, straight RGB. */
void liquid_form_rgb(const LiquidForm *f, float rgb[3]);

void liquid_form_size(int *w, int *h);
struct wlr_scene_buffer *liquid_form_show(struct wlr_scene_tree *parent, int x, int y,
                                          const LiquidForm *f);
void liquid_form_redraw(struct wlr_scene_buffer *buf, const LiquidForm *f);

/* The pointer, in FORM-BUFFER-LOCAL coordinates. */
LiquidFormResult liquid_form_press(LiquidForm *f, double x, double y);
LiquidFormResult liquid_form_motion(LiquidForm *f, double x, double y);
LiquidFormResult liquid_form_release(LiquidForm *f);
/* A key press: its keysym and the text it types (may be ""). */
LiquidFormResult liquid_form_key(LiquidForm *f, uint32_t sym, const char *utf8);

#endif /* FWM_LIQUID_FORM_H */
