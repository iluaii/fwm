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

#ifndef FWM_BGKEY_H
#define FWM_BGKEY_H

struct FwmView;

/*
 * A see-through background for a window that does not draw one.
 *
 * foot draws its background translucent and its text whole, because it knows
 * which pixel is which. A browser does not offer to, and from outside a window
 * is only a picture — so [[rule]] bg_alpha keys it instead: the background's
 * colour (given, or found in the picture by key_blit) comes out at bg_alpha,
 * and the rest as it was.
 *
 * Done in place, on the scene's own nodes. Each surface of the window keeps its
 * scene buffer, its position, its input region and everything the effects do
 * with it; only the picture in it is swapped for a keyed copy — once per
 * picture the client sends, so a window that is not changing costs nothing.
 * The scene puts the client's buffer back every time the surface commits, and
 * the next frame keys that one in turn.
 *
 * Only on the GLES2 renderer, like every other shader in fwm; elsewhere the
 * rule is a no-op and the window is drawn as it is.
 */

/* Key whatever the window's surfaces have committed since the last frame.
 * Once a frame, before the frame is drawn. A no-op for a window without the
 * rule. */
void bgkey_view_frame(struct FwmView *view);

/* Drop the keyed copies; the window is going. */
void bgkey_view_free(struct FwmView *view);

#endif /* FWM_BGKEY_H */
