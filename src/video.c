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

#include "video.h"
#include "ui/cairo_overlay.h"

#include <cairo.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <wlr/util/log.h>

/* Frames buffered ahead of the compositor. Three is enough to ride jitter
 * without hoarding RAM (each is a full cover-sized BGRA image, ~8 MB at 1080p),
 * and it is the throttle: when we stop presenting, the decode thread fills
 * these and then blocks on `not_full` instead of spinning. */
#define VIDEO_QUEUE_CAP 3
/* Never present faster than this, whatever the source fps. A wallpaper does not
 * need 60 fps, and the cap halves upload/redraw work on a high-fps clip. */
#define VIDEO_FPS_CAP   30.0

/* One decoded frame handed thread -> compositor: plain malloc'd BGRA, sized to
 * the cover buffer. The decode thread produces only this — never a wlr_buffer,
 * so all of wlroots stays single-threaded on the main loop. */
struct VideoFrame {
    uint8_t *data;
};

struct FwmVideo {
    struct wlr_scene_buffer *scene_buffer; /* screen-sized; owned here */
    int screen_w, screen_h;

    /* The video is scaled to *cover* the screen (fill + crop) rather than
     * letterboxed. Working the cover size out once means the decode thread
     * always emits the same dimensions and the compositor only centre-crops. */
    int cover_w, cover_h;
    int crop_x, crop_y;

    /* Decode state — touched only by the decode thread after create() returns. */
    AVFormatContext   *fmt;
    AVCodecContext    *codec;
    struct SwsContext *sws;
    int                stream_index;
    AVRational         time_base;
    double             frame_interval; /* seconds between presented frames */

    /* Decode thread + bounded queue. */
    pthread_t         thread;
    int               have_thread;
    pthread_mutex_t   lock;
    pthread_cond_t    not_full;
    pthread_cond_t    not_empty;
    struct VideoFrame queue[VIDEO_QUEUE_CAP];
    int               head, count;
    int               stop; /* set by destroy(); guarded by lock */

    /* Compositor-thread presentation pacing (single thread, no lock needed). */
    struct timespec last_present;
    int             primed;
    bool            paused;
};

/* Scale one decoded frame into a fresh cover-sized BGRA buffer. BGRA in memory
 * (B,G,R,A) is byte-identical to DRM_FORMAT_ARGB8888 on little-endian, which is
 * what the scene buffer wants, so no further conversion happens on upload.
 * Runs on the decode thread. */
static uint8_t *scale_to_cover(FwmVideo *v, AVFrame *frame) {
    v->sws = sws_getCachedContext(v->sws,
                                  frame->width, frame->height,
                                  (enum AVPixelFormat)frame->format,
                                  v->cover_w, v->cover_h, AV_PIX_FMT_BGRA,
                                  SWS_BILINEAR, NULL, NULL, NULL);
    if (!v->sws) return NULL;

    uint8_t *buf = malloc((size_t)v->cover_w * v->cover_h * 4);
    if (!buf) return NULL;

    uint8_t *dst[4]     = { buf, NULL, NULL, NULL };
    int      dstride[4] = { v->cover_w * 4, 0, 0, 0 };
    sws_scale(v->sws, (const uint8_t *const *)frame->data, frame->linesize,
              0, frame->height, dst, dstride);
    return buf;
}

static void *decode_thread(void *arg) {
    FwmVideo *v = arg;
    AVPacket *pkt   = av_packet_alloc();
    AVFrame  *frame = av_frame_alloc();
    if (!pkt || !frame) goto out;

    /* Video-clock time (seconds) of the last frame we queued, used to drop
     * frames on a source faster than the cap. Negative sentinel = "none yet",
     * also reset after each loop-around so the first frame is never dropped. */
    double last_pushed = -1e9;

    for (;;) {
        pthread_mutex_lock(&v->lock);
        int stop = v->stop;
        pthread_mutex_unlock(&v->lock);
        if (stop) break;

        int ret = av_read_frame(v->fmt, pkt);
        if (ret < 0) {
            /* EOF (or a read error): loop the clip. A file we cannot seek is
             * rare enough that giving up on it is better than busy-looping. */
            av_packet_unref(pkt);
            if (av_seek_frame(v->fmt, v->stream_index, 0, AVSEEK_FLAG_BACKWARD) < 0)
                break;
            avcodec_flush_buffers(v->codec);
            last_pushed = -1e9;
            continue;
        }
        if (pkt->stream_index != v->stream_index) {
            av_packet_unref(pkt);
            continue;
        }

        ret = avcodec_send_packet(v->codec, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (avcodec_receive_frame(v->codec, frame) == 0) {
            int64_t ts = frame->best_effort_timestamp;
            double t;
            if (ts == AV_NOPTS_VALUE) {
                /* No timestamps: assume the source is already at/under the cap
                 * and never drop, spacing frames by the interval. */
                t = (last_pushed < -1e8) ? 0.0 : last_pushed + v->frame_interval;
            } else {
                t = ts * av_q2d(v->time_base);
                if (last_pushed > -1e8 &&
                    t - last_pushed < v->frame_interval * 0.999) {
                    av_frame_unref(frame); /* too soon — cap drop */
                    continue;
                }
            }

            uint8_t *buf = scale_to_cover(v, frame);
            av_frame_unref(frame);
            if (!buf) continue;

            pthread_mutex_lock(&v->lock);
            while (v->count == VIDEO_QUEUE_CAP && !v->stop)
                pthread_cond_wait(&v->not_full, &v->lock);
            if (v->stop) {
                pthread_mutex_unlock(&v->lock);
                free(buf);
                goto out;
            }
            v->queue[(v->head + v->count) % VIDEO_QUEUE_CAP].data = buf;
            v->count++;
            pthread_cond_signal(&v->not_empty);
            pthread_mutex_unlock(&v->lock);

            last_pushed = t;
        }
    }

out:
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return NULL;
}

static bool pop_frame(FwmVideo *v, uint8_t **out) {
    pthread_mutex_lock(&v->lock);
    if (v->count == 0) {
        pthread_mutex_unlock(&v->lock);
        return false;
    }
    *out = v->queue[v->head].data;
    v->head = (v->head + 1) % VIDEO_QUEUE_CAP;
    v->count--;
    pthread_cond_signal(&v->not_full);
    pthread_mutex_unlock(&v->lock);
    return true;
}

static void present(FwmVideo *v, bool force) {
    if (!v || v->paused) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!force && v->primed) {
        double dt = (double)(now.tv_sec - v->last_present.tv_sec)
                  + (double)(now.tv_nsec - v->last_present.tv_nsec) / 1e9;
        if (dt < v->frame_interval * 0.999) return; /* not time for a new frame */
    }

    uint8_t *buf;
    if (!pop_frame(v, &buf)) return; /* decoder has not caught up: keep last frame */

    cairo_overlay_blit_bgra(v->scene_buffer, buf, v->cover_w * 4, v->crop_x, v->crop_y);
    free(buf);
    v->last_present = now;
    v->primed = 1;
}

void video_present(FwmVideo *v) { present(v, false); }

void video_set_paused(FwmVideo *v, bool paused) {
    if (v) v->paused = paused;
}

bool video_playing(FwmVideo *v) {
    return v && !v->paused;
}

int video_interval_ms(FwmVideo *v) {
    if (!v || v->frame_interval <= 0.0) return 0;
    int ms = (int)lround(v->frame_interval * 1000.0);
    return ms < 1 ? 1 : ms;
}

struct wlr_scene_buffer *video_scene_buffer(FwmVideo *v) {
    return v ? v->scene_buffer : NULL;
}

FwmVideo *video_create(struct wlr_scene_tree *parent, const char *path,
                       int screen_w, int screen_h, double fps_cap) {
    if (!parent || !path || !path[0] || screen_w <= 0 || screen_h <= 0)
        return NULL;

    FwmVideo *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->screen_w = screen_w;
    v->screen_h = screen_h;
    v->stream_index = -1;

    if (avformat_open_input(&v->fmt, path, NULL, NULL) < 0) {
        wlr_log(WLR_ERROR, "video wallpaper: cannot open '%s'", path);
        free(v);
        return NULL;
    }
    if (avformat_find_stream_info(v->fmt, NULL) < 0) goto fail;

    const AVCodec *codec = NULL;
    int si = av_find_best_stream(v->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (si < 0 || !codec) {
        wlr_log(WLR_ERROR, "video wallpaper: no video stream in '%s'", path);
        goto fail;
    }
    v->stream_index = si;
    AVStream *st = v->fmt->streams[si];
    v->time_base = st->time_base;

    v->codec = avcodec_alloc_context3(codec);
    if (!v->codec) goto fail;
    if (avcodec_parameters_to_context(v->codec, st->codecpar) < 0) goto fail;
    /* Speed over strict fidelity — this is an out-of-focus background, and the
     * machine running a compositor + video decode may be weak. thread_count 0
     * lets libav use every core (a 4K clip needs them just to reach 30 fps);
     * FLAG2_FAST allows non-compliant shortcuts; and dropping the in-loop
     * deblocking filter (the single most expensive stage of H.264 decode) buys
     * ~20% on 1080p and pushes 4K past real time, with no visible loss at
     * wallpaper scale. */
    v->codec->thread_count = 0;
    v->codec->flags2 |= AV_CODEC_FLAG2_FAST;
    v->codec->skip_loop_filter = AVDISCARD_ALL;
    if (avcodec_open2(v->codec, codec, NULL) < 0) goto fail;

    int vw = v->codec->width, vh = v->codec->height;
    if (vw <= 0 || vh <= 0) goto fail;

    double scale = fmax((double)screen_w / vw, (double)screen_h / vh);
    v->cover_w = (int)lround(vw * scale);
    v->cover_h = (int)lround(vh * scale);
    if (v->cover_w < screen_w) v->cover_w = screen_w;
    if (v->cover_h < screen_h) v->cover_h = screen_h;
    v->crop_x = (v->cover_w - screen_w) / 2;
    v->crop_y = (v->cover_h - screen_h) / 2;

    /* Cap presentation fps: the source rate unless the config asks for less.
     * Lowering it trims scale/upload/recomposite — but NOT the decode, which
     * still runs on every frame (inter-prediction needs them all). */
    double cap = fps_cap > 0.0 ? fps_cap : VIDEO_FPS_CAP;
    if (cap < 1.0) cap = 1.0;
    if (cap > 60.0) cap = 60.0;
    double src_fps = av_q2d(av_guess_frame_rate(v->fmt, st, NULL));
    if (!(src_fps > 0.0)) src_fps = cap;
    double fps = src_fps > cap ? cap : src_fps;
    v->frame_interval = 1.0 / fps;

    /* Asking for roughly half the clip's rate (or less) is a clear signal that
     * smoothness is being traded for CPU, so let the decoder drop non-reference
     * frames — the ONLY way to cut the decode itself. It roughly halves decode
     * work (and output framerate); on 4K that is the difference between ~45%
     * and ~90% of a core. Motion gets choppier, which is why it is tied to an
     * explicitly low fps rather than always on. */
    int skip_nonref = fps <= src_fps * 0.55;
    if (skip_nonref) v->codec->skip_frame = AVDISCARD_NONREF;

    v->scene_buffer = cairo_overlay_create(parent, screen_w, screen_h);
    if (!v->scene_buffer) goto fail;

    pthread_mutex_init(&v->lock, NULL);
    pthread_cond_init(&v->not_full, NULL);
    pthread_cond_init(&v->not_empty, NULL);
    if (pthread_create(&v->thread, NULL, decode_thread, v) != 0) {
        wlr_log(WLR_ERROR, "video wallpaper: cannot start decode thread");
        pthread_mutex_destroy(&v->lock);
        pthread_cond_destroy(&v->not_full);
        pthread_cond_destroy(&v->not_empty);
        cairo_overlay_destroy(v->scene_buffer);
        v->scene_buffer = NULL;
        goto fail;
    }
    v->have_thread = 1;

    /* Show the first frame before returning, so the wallpaper never flashes
     * empty in the window between create() and the first present tick. Bounded
     * wait: a file that yields no frame in a second is treated as playable but
     * simply starts blank rather than stalling startup. */
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 1;
    pthread_mutex_lock(&v->lock);
    while (v->count == 0 && !v->stop) {
        if (pthread_cond_timedwait(&v->not_empty, &v->lock, &until) != 0) break;
    }
    pthread_mutex_unlock(&v->lock);
    present(v, true);

    wlr_log(WLR_INFO, "video wallpaper: %s %dx%d@%.0ffps%s -> cover %dx%d (crop %d,%d)",
            path, vw, vh, fps, skip_nonref ? " skip-nonref" : "",
            v->cover_w, v->cover_h, v->crop_x, v->crop_y);
    return v;

fail:
    if (v->sws) sws_freeContext(v->sws);
    if (v->codec) avcodec_free_context(&v->codec);
    if (v->fmt) avformat_close_input(&v->fmt);
    free(v);
    return NULL;
}

cairo_surface_t *video_thumbnail(const char *path, int max_w, int max_h) {
    if (!path || !path[0] || max_w <= 0 || max_h <= 0) return NULL;

    AVFormatContext   *fmt = NULL;
    AVCodecContext    *cc  = NULL;
    struct SwsContext *sws = NULL;
    AVPacket          *pkt = NULL;
    AVFrame           *frame = NULL;
    cairo_surface_t   *surf = NULL;

    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return NULL;
    if (avformat_find_stream_info(fmt, NULL) < 0) goto done;

    const AVCodec *codec = NULL;
    int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (si < 0 || !codec) goto done;
    AVStream *st = fmt->streams[si];

    cc = avcodec_alloc_context3(codec);
    if (!cc) goto done;
    if (avcodec_parameters_to_context(cc, st->codecpar) < 0) goto done;
    cc->thread_count = 1; /* a single still: no point spinning up decode threads */
    cc->flags2 |= AV_CODEC_FLAG2_FAST;
    cc->skip_loop_filter = AVDISCARD_ALL; /* one thumbnail frame: quality is moot */
    if (avcodec_open2(cc, codec, NULL) < 0) goto done;
    if (cc->width <= 0 || cc->height <= 0) goto done;

    /* Seek to a random point so the icon is a real frame from the clip, not the
     * often-black or logo first frame. Landing on the nearest earlier keyframe
     * is fine — any representative frame will do. */
    if (fmt->duration > 0) {
        static int seeded = 0;
        if (!seeded) { srand((unsigned)time(NULL)); seeded = 1; }
        double frac = 0.1 + (rand() / ((double)RAND_MAX + 1.0)) * 0.8;
        int64_t target = (int64_t)(frac * fmt->duration); /* AV_TIME_BASE units */
        int64_t seek_ts = av_rescale_q(target, AV_TIME_BASE_Q, st->time_base);
        if (av_seek_frame(fmt, si, seek_ts, AVSEEK_FLAG_BACKWARD) >= 0)
            avcodec_flush_buffers(cc);
    }

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) goto done;

    /* Pull packets until the first frame emerges. Bounded so a pathological
     * file cannot spin here forever. */
    int got = 0, draining = 0;
    for (int guard = 0; !got && guard < 4096; guard++) {
        if (!draining) {
            int r = av_read_frame(fmt, pkt);
            if (r < 0) { draining = 1; avcodec_send_packet(cc, NULL); continue; }
            if (pkt->stream_index != si) { av_packet_unref(pkt); continue; }
            avcodec_send_packet(cc, pkt);
            av_packet_unref(pkt);
        }
        int r = avcodec_receive_frame(cc, frame);
        if (r == 0) got = 1;
        else if (r == AVERROR(EAGAIN)) continue;
        else break;
    }
    if (!got) goto done;

    int fw = frame->width, fh = frame->height;
    if (fw <= 0 || fh <= 0) goto done;
    double scale = fmin((double)max_w / fw, (double)max_h / fh);
    int out_w = (int)lround(fw * scale);
    int out_h = (int)lround(fh * scale);
    if (out_w < 1) out_w = 1;
    if (out_h < 1) out_h = 1;

    surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, out_w, out_h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        surf = NULL;
        goto done;
    }
    sws = sws_getContext(fw, fh, (enum AVPixelFormat)frame->format,
                         out_w, out_h, AV_PIX_FMT_BGRA, SWS_BILINEAR,
                         NULL, NULL, NULL);
    if (!sws) {
        cairo_surface_destroy(surf);
        surf = NULL;
        goto done;
    }

    cairo_surface_flush(surf);
    uint8_t *dst[4]     = { cairo_image_surface_get_data(surf), NULL, NULL, NULL };
    int      dstride[4] = { cairo_image_surface_get_stride(surf), 0, 0, 0 };
    sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize,
              0, fh, dst, dstride);
    cairo_surface_mark_dirty(surf);

done:
    if (sws) sws_freeContext(sws);
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (cc) avcodec_free_context(&cc);
    if (fmt) avformat_close_input(&fmt);
    return surf;
}

void video_destroy(FwmVideo *v) {
    if (!v) return;

    if (v->have_thread) {
        pthread_mutex_lock(&v->lock);
        v->stop = 1;
        pthread_cond_broadcast(&v->not_full);
        pthread_cond_broadcast(&v->not_empty);
        pthread_mutex_unlock(&v->lock);
        pthread_join(v->thread, NULL);
        pthread_mutex_destroy(&v->lock);
        pthread_cond_destroy(&v->not_full);
        pthread_cond_destroy(&v->not_empty);
    }

    for (int i = 0; i < v->count; i++)
        free(v->queue[(v->head + i) % VIDEO_QUEUE_CAP].data);

    if (v->scene_buffer) cairo_overlay_destroy(v->scene_buffer);
    if (v->sws) sws_freeContext(v->sws);
    if (v->codec) avcodec_free_context(&v->codec);
    if (v->fmt) avformat_close_input(&v->fmt);
    free(v);
}
