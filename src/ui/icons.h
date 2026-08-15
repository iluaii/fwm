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

#ifndef FWM_ICONS_H
#define FWM_ICONS_H

#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/* Icon lookup and decoding, shared by every panel that draws one — the
 * launcher's result rows and the radial menu's petals. It lived in the
 * launcher until there were two of them. */

/* Premultiplied ARGB32 cairo surface from a pixbuf. The caller owns the
 * surface and destroys it; the pixbuf is left alone. */
cairo_surface_t *icon_surface_from_pixbuf(GdkPixbuf *pb);

/* An icon by Icon= name (resolved against the icon theme) or by path, decoded
 * to fit `size` px. NULL when there is no such icon or it will not decode.
 * `cfg_theme` may be "" or NULL to auto-detect, as [decor] icon_theme does. */
cairo_surface_t *icon_load(const char *name, const char *cfg_theme, int size);

#endif /* FWM_ICONS_H */
