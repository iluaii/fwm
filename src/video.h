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

#ifndef FWM_VIDEO_H
#define FWM_VIDEO_H

#include <stdbool.h>
#include <cairo.h>
#include <wlr/types/wlr_scene.h>

/* A looping video wallpaper layer. Decoding (libavcodec, software) runs on a
 * dedicated thread that touches no wlroots object; it hands screen-cover-sized
 * BGRA frames to the compositor thread over a small bounded queue. The queue is
 * what keeps this cheap: stop consuming (wallpaper covered, screen blanked) and
 * the queue fills, the decode thread blocks on it, and CPU use drops to zero
 * until we present again. */
typedef struct FwmVideo FwmVideo;

/* Open `path`, scale-to-cover `screen_w`x`screen_h`, and start decoding. The
 * returned layer owns a scene buffer under `parent`. NULL on any failure (bad
 * file, no video stream, out of memory) — the caller falls back to a still. */
FwmVideo *video_create(struct wlr_scene_tree *parent, const char *path,
                       int screen_w, int screen_h, double fps_cap);

/* The scene buffer backing the layer, so the wallpaper module can fade it in
 * and out exactly like a still layer (opacity lives on the node, independent of
 * the per-frame buffer swaps this does underneath). */
struct wlr_scene_buffer *video_scene_buffer(FwmVideo *v);

/* Compositor thread, once per frame: upload the next decoded frame if enough
 * wall-clock time has passed for the fps cap. A no-op while paused. */
void video_present(FwmVideo *v);

/* Pause presentation while the wallpaper is fully covered (a real-fullscreen
 * window on the active desktop). The decode thread is not signalled directly —
 * it simply blocks once the queue fills, which is the whole savings. */
void video_set_paused(FwmVideo *v, bool paused);

/* True while the layer is playing (exists and not paused). */
bool video_playing(FwmVideo *v);

/* Presentation interval in milliseconds (1000 / capped fps), for driving a
 * frame-scheduling timer at the video's own rate rather than 60 Hz. 0 if none. */
int video_interval_ms(FwmVideo *v);

/* Stop the decode thread, then free everything including the scene buffer. */
void video_destroy(FwmVideo *v);

/* Decode one frame from a random point in `path` into a premultiplied ARGB32
 * cairo surface that fits within max_w x max_h (aspect preserved), for the
 * wallpaper picker's thumbnail. Synchronous and self-contained — opens and
 * closes its own decode context. NULL on any failure. Caller owns the surface
 * (cairo_surface_destroy). */
cairo_surface_t *video_thumbnail(const char *path, int max_w, int max_h);

#endif /* FWM_VIDEO_H */
