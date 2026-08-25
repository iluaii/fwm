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

#ifndef FWM_GLASS_H
#define FWM_GLASS_H

#include <stdbool.h>

#include "config.h"

struct FwmServer;
struct FwmOutput;
struct wlr_scene_buffer;

/* What fwm's own panels are made of.
 *
 * Every panel fwm draws — the tray, the launcher, the modes menu, the sound
 * panel, a toast — is a cairo overlay painting flat islands on a transparent
 * sheet. This gives those islands a material: the desktop behind them, blurred,
 * and a shadow under them from the same sun the windows cast from.
 *
 * The whole thing rests on one observation about how those panels are drawn.
 * An island is filled at a known alpha, so the sheet's own alpha channel is
 * that alpha times the coverage of whatever shape was painted — and dividing
 * it back out recovers the shape EXACTLY, rounded corners, gaps between pills,
 * antialiased rim and all. So a panel needs to say nothing about its geometry
 * to get frosted: it already said it, in the pixels. Nothing in ui/ knows this
 * file exists beyond asking glass_fill what alpha to paint at.
 *
 * What one attached panel costs per frame is a photograph of the strip of
 * desktop it covers and four small passes over it (src/blur.c). Panels are few
 * and thin, and a panel that is switched off costs nothing at all.
 *
 * The frost cannot be drawn without the GLES2 renderer, and on the others the
 * panels simply stand on the desktop the way they always did. */

/* The one server, remembered once so that attaching a pane is a single line
 * anywhere a panel is built. Panels are made in a dozen files, several of
 * which have no FwmServer to hand and no reason to grow one — the expo menu
 * takes a scene tree and a title, and that is the right shape for it. Called
 * once, when the scene exists. */
void glass_init(struct FwmServer *server);

/* Is the frost both asked for and possible? Answers from the config alone
 * before glass_init, which is the form config validation and the tests
 * want. */
bool glass_available(const FwmConfig *cfg);

/* The alpha a panel should fill its islands at.
 *
 * `cfg` may be NULL for a panel that has none to hand — a toast, the expo
 * menu — and the one compositor's own is used; see glass_init.
 *
 * With the glass off this is `fallback`, whatever the panel was going to use.
 * With it on, every panel paints at the same [glass] fill: the frost under an
 * island only shows through what the island does not cover, so how opaque the
 * islands are IS how much glass there is, and it has to be one number for the
 * whole desktop or the panels stop looking like the same material. */
double glass_fill(const FwmConfig *cfg, double fallback);

/* Hang a pane under `panel`, a cairo overlay, and keep it there.
 *
 * The pane is a sibling node placed directly below the panel's own, so it
 * inherits nothing and is positioned to match on every frame. Ownership needs
 * no thought from the caller: the pane listens for the panel's node being
 * destroyed and goes with it, which is what lets a panel that is created and
 * thrown away on every open — the launcher, a toast — attach in one line and
 * never mention it again.
 *
 * Attaching is unconditional and costs one disabled scene node: the glass can
 * be switched on with `fwmctl set glass.enabled 1` while the tray is already
 * standing there, and a pane that had to be asked for at build time would
 * leave every panel already on screen bare until it was next rebuilt. Nothing
 * is allocated until the first frame that actually draws one. */
void glass_attach(struct wlr_scene_buffer *panel);

/* Redraw every pane on `out`. Once per frame, after everything has moved and
 * before the scene is committed. */
void glass_tick(struct FwmOutput *out);

/* Give up everything that lives on the GPU — buffers, textures, the shader
 * programs — and keep the panes themselves. For a renderer that was lost: the
 * next tick builds all of it again against the new one. */
void glass_gpu_release(void);

/* Drop every pane and the shader programs behind them. Compositor shutdown. */
void glass_finish(void);

#endif /* FWM_GLASS_H */
