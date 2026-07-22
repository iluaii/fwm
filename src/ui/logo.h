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

#ifndef FWM_LOGO_H
#define FWM_LOGO_H

#include <cairo.h>

/* Logo variants (match assets/logo-*.svg). */
#define FWM_LOGO_MONO     0 /* bare F/W/M monogram        (logo-mono.svg)     */
#define FWM_LOGO_FRAMED   1 /* monogram in hexagon badge  (logo.svg)          */
#define FWM_LOGO_BRACKETS 2 /* monogram between < >       (logo-brackets.svg) */

/* Aspect ratios: width = height * FWM_LOGO_AR_<variant>. */
#define FWM_LOGO_AR_MONO     (110.0 / 100.0)
#define FWM_LOGO_AR_FRAMED   (240.0 / 132.0)
#define FWM_LOGO_AR_BRACKETS (198.0 / 100.0)

/* Draw the fwm logo with its top-left corner at (x, y), scaled to `height`. */
void fwm_logo_draw(cairo_t *cr, double x, double y, double height, int variant,
                   double r, double g, double b, double a);

#endif /* FWM_LOGO_H */
