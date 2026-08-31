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

#ifndef FWM_WALLPAPER_H
#define FWM_WALLPAPER_H

#include <stdbool.h>
#include <wlr/util/box.h>
#include <wlr/types/wlr_scene.h>

struct wlr_renderer;
struct wlr_texture;
#include "config.h"

typedef struct FwmWallpaper FwmWallpaper;

/* Build parallax wallpaper layers as children of `parent`, which must be a scene
 * tree positioned below the window layer. Layers are drawn back-to-front in the
 * order they appear in the config. Returns NULL if no layer could be loaded.
 *
 * `output` is the monitor this set is for, as fwm names it ("DP-1"): only the
 * layers that belong to it are built, so two screens can carry two different
 * wallpapers out of one array. NULL takes the un-named layers. */
FwmWallpaper *wallpaper_create(struct wlr_scene_tree *parent, const FwmConfig *cfg,
                               const char *output, int screen_w, int screen_h);

/* Reposition every layer for the current horizontal camera offset. */
/* Move the whole set onto its monitor, in layout coordinates. The layers are
 * built as if the screen started at 0,0, so this is what puts a second
 * monitor's wallpaper on the second monitor. */
void wallpaper_set_origin(FwmWallpaper *wp, int x, int y);

void wallpaper_update(FwmWallpaper *wp, int camera_x);

/* Advance any video layers: call once per rendered frame. Cheap and a no-op
 * when there is no video layer or it is paused. */
void wallpaper_present(FwmWallpaper *wp);

/* Pause/resume video layers, e.g. while the wallpaper is fully covered by a
 * real-fullscreen window. A paused video stops decoding once its small queue
 * fills, dropping its CPU use to nothing. */
void wallpaper_set_paused(FwmWallpaper *wp, bool paused);

/* True while a video layer is actively playing (not paused). */
bool wallpaper_playing(FwmWallpaper *wp);

/* Smallest present interval (ms) across video layers, for driving a frame timer
 * at the video's own rate instead of 60 Hz. 0 when no video layer is present. */
int wallpaper_video_interval_ms(FwmWallpaper *wp);

/* Start the layers transparent and ramp them to opaque over `duration_ms`.
 * The caller keeps the previous wallpaper alive underneath until
 * wallpaper_fade_tick reports the fade finished. */
void wallpaper_fade_in(FwmWallpaper *wp, double duration_ms);

/* Advance a running cross-fade. Returns true on the frame it completes. */
bool wallpaper_fade_tick(FwmWallpaper *wp, double dt);

/* What the expo strip needs to draw the same wallpaper into a card of its own.
 *
 * The live layers can be neither photographed nor re-used: the scene drops its
 * reference to a cairo overlay's buffer as soon as it has uploaded a texture,
 * and the wallpaper then frees the CPU-side pixels itself. Both attempts ended
 * with ten grey cards. What is handed out instead is the small off-screen copy
 * each layer keeps for exactly this (see WallpaperRT.card), plus the crop the
 * given camera position corresponds to — so a card shows precisely what
 * standing on that desktop looks like. */
int wallpaper_layer_count(FwmWallpaper *wp);
struct wlr_buffer *wallpaper_layer_buffer(FwmWallpaper *wp, int i);

/* That same copy as a texture, ready to draw, without paying for it twice.
 *
 * The copy lives in ordinary memory, so handing it to wlr_texture_from_buffer
 * is an UPLOAD of the whole picture — a few megabytes for a pan layer — and
 * throwing the texture away afterwards means paying for it again next time.
 * Fine for the strip, which opens once; ruinous for the black hole's lens,
 * which photographs the desktop behind it on every frame it moves.
 *
 * So a layer that never redraws keeps its texture: `*borrowed` comes back true
 * and the caller must NOT destroy it. A video layer has no such luck — its
 * card is the live buffer, recycled from a small pool, so the same pointer
 * coming back is no promise that the pixels did — and it is imported fresh
 * with `*borrowed` false, for the caller to destroy as before. */
struct wlr_texture *wallpaper_layer_texture(FwmWallpaper *wp, int i,
                                            struct wlr_renderer *renderer,
                                            bool *borrowed);
/* The part of that buffer a screen at `camera_x` is looking at, in the
 * buffer's own pixels — the caller never has to know it is a downscaled copy. */
void wallpaper_layer_crop(FwmWallpaper *wp, int i, int camera_x,
                          int screen_w, int screen_h, struct wlr_fbox *out);


/* Drop the cached card textures ahead of a renderer swap after a GPU reset.
 * The cards are CPU-side and stay; the textures are re-uploaded on demand. */
void wallpaper_gpu_release(FwmWallpaper *wp);
void wallpaper_destroy(FwmWallpaper *wp);

#endif /* FWM_WALLPAPER_H */
