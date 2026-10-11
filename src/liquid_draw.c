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

#include "liquid_draw.h"

#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <math.h>
#include <stdlib.h>

#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/gles2.h>
#include <wlr/render/egl.h>
#include <wlr/util/log.h>

#include "server.h"
#include "snapshot.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* How wide a splat reaches, in particle spacings: a simulation cell and a
 * half (see the header for why no narrower). */
#define SPLAT_REACH 3.0

/* The field is drawn at this fraction of the box. It is a sum of soft bumps
 * with nothing sharp in it, so half the resolution loses nothing the
 * threshold below can show and costs a quarter of the pixels. */
#define FIELD_SCALE 0.5

/* What the inside of the liquid adds up to, as a fraction of what a pixel can
 * hold. Half: the splats are summed into an 8-bit channel, and a body squeezed
 * past rest, or a pour landing on liquid, must not saturate it into a flat
 * plateau whose edge the threshold can no longer place. */
#define FIELD_INSIDE 0.5f

/* Where the surface is, as a fraction of the inside. A flat surface sits at
 * about half (a splat at the edge has liquid on one side only); a quarter
 * puts the drawn edge a little outside the outermost particles — which is
 * where the liquid they stand for ends — and still shows a lone particle as a
 * drop rather than dropping it. */
#define SURFACE_LEVEL 0.22f

/* How far in from the edge the light rim reaches, in the same units. */
#define RIM_DEPTH 0.18f

/* u_mirror reflects the particle across a wall: x' = x * m.x + m.y, and the
 * same for y with m.z, m.w. Identity is (1, 0, 1, 0). */
/* a_kind picks the particle's liquid out of u_palette (straight rgb, alpha). */
static const char splat_vert_src[] =
    "attribute vec2 a_pos;\n"
    "attribute float a_kind;\n"
    "uniform vec2 u_scale;\n"
    "uniform float u_size;\n"
    "uniform vec4 u_mirror;\n"
    "uniform vec4 u_palette[8];\n"
    "varying vec4 v_col;\n"
    "void main() {\n"
    "    vec2 p = a_pos * u_mirror.xz + u_mirror.yw;\n"
    "    gl_Position = vec4(p * u_scale - 1.0, 0.0, 1.0);\n"
    "    gl_PointSize = u_size;\n"
    "    v_col = u_palette[int(a_kind + 0.5)];\n"
    "}\n";

/* A smooth bump that is exactly zero at its rim, so adjacent splats join
 * without a seam: (1 - r²)³. */
static const char splat_frag_src[] =
    "precision mediump float;\n"
    "uniform float u_gain;\n"
    "uniform float u_alpha;\n"   /* 0: colour and amount; 1: how opaque */
    "varying vec4 v_col;\n"
    "void main() {\n"
    "    vec2 d = gl_PointCoord * 2.0 - 1.0;\n"
    "    float k = 1.0 - dot(d, d);\n"
    "    if (k <= 0.0) discard;\n"
    "    float w = k * k * k * u_gain;\n"
    "    gl_FragColor = u_alpha > 0.5 ? vec4(v_col.a * w) : vec4(v_col.rgb * w, w);\n"
    "}\n";

static const char shade_vert_src[] =
    "attribute vec2 a_pos;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_pos * 0.5 + 0.5;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

/* Cut the field at the surface. The edge is softened over a band as wide as
 * the field changes across a pixel, which is what keeps it from stair-
 * stepping; the body is the colour of the liquids there, as much of each as
 * there is of it, and a band just inside the edge is lifted toward white —
 * the bright line where a surface turns away from you, which is most of what
 * makes a flat fill read as a liquid. How opaque is the same mix, read off a
 * field of its own when the liquids are not all equally so, and otherwise
 * u_opaque. */
static const char shade_frag_src[] =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_field;\n"
    "uniform sampler2D u_opacity;\n"
    "uniform float u_opaque;\n"   /* < 0: read u_opacity */
    "uniform float u_level;\n"
    "uniform float u_soft;\n"
    "uniform float u_rim;\n"
    "void main() {\n"
    "    vec4 fc = texture2D(u_field, v_uv);\n"
    "    float f = fc.a;\n"
    "    float a = smoothstep(u_level - u_soft, u_level + u_soft, f);\n"
    "    if (a <= 0.0) { gl_FragColor = vec4(0.0); return; }\n"
    "    vec3 body = fc.rgb / max(f, 1e-4);\n"
    "    float op = u_opaque >= 0.0 ? u_opaque\n"
    "             : clamp(texture2D(u_opacity, v_uv).r / max(f, 1e-4), 0.0, 1.0);\n"
    "    float inner = smoothstep(u_level, u_level + u_rim, f);\n"
    "    vec3 c = mix(mix(body, vec3(1.0), 0.45), body, inner);\n"
    "    float alpha = a * mix(min(op + 0.25, 1.0), op, inner);\n"
    "    gl_FragColor = vec4(c * alpha, alpha);\n"
    "}\n";

#define UNIFORMS 8

struct program {
    GLuint id;
    GLint u[UNIFORMS];
};

/* Rebuilt if the renderer is ever replaced — a GL object belongs to the
 * context it was made in, the same bookkeeping star_gl.c does. */
static struct {
    struct program splat, shade;
    bool tried, ok;
    struct wlr_renderer *owner;
    float max_point;
} gl;

struct FwmLiquidPaint {
    struct FwmServer *server;
    int w, h;              /* the box, px */
    int fw, fh;            /* the field */
    struct wlr_buffer *field;
    struct wlr_texture *field_tex;
    /* How opaque, beside it: only made once liquids that are not equally so
     * share the box. */
    struct wlr_buffer *opacity;
    struct wlr_texture *opacity_tex;
    struct wlr_buffer *out[2];
    int next;              /* which of `out` the next picture goes into */
    struct wlr_buffer *current;
    struct wlr_box drawn;  /* what the last picture covered, empty at first */
};

bool liquid_paint_supported(struct wlr_renderer *renderer) {
    return renderer && wlr_renderer_is_gles2(renderer);
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    if (!s) return 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        wlr_log(WLR_ERROR, "liquid: shader would not compile: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static bool link(struct program *p, const char *vsrc, const char *fsrc,
                 const char *const names[UNIFORMS]) {
    GLuint vs = compile(GL_VERTEX_SHADER, vsrc);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fsrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    GLuint id = glCreateProgram();
    glAttachShader(id, vs);
    glAttachShader(id, fs);
    glBindAttribLocation(id, 0, "a_pos");
    glBindAttribLocation(id, 1, "a_kind");
    glLinkProgram(id);
    glDetachShader(id, vs);
    glDetachShader(id, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(id, sizeof log, NULL, log);
        wlr_log(WLR_ERROR, "liquid: shader would not link: %s", log);
        glDeleteProgram(id);
        return false;
    }
    p->id = id;
    for (int i = 0; i < UNIFORMS; i++)
        p->u[i] = names[i] ? glGetUniformLocation(id, names[i]) : -1;
    return true;
}

static bool programs_ready(struct wlr_renderer *renderer) {
    if (gl.owner != renderer) {
        /* The old programs belonged to a context that is gone; there is
         * nothing to delete safely, only to forget. */
        gl.splat.id = gl.shade.id = 0;
        gl.tried = gl.ok = false;
        gl.owner = renderer;
    }
    if (gl.tried) return gl.ok;
    gl.tried = true;

    static const char *const splat_u[UNIFORMS] = {
        "u_scale", "u_size", "u_gain", "u_mirror", "u_palette", "u_alpha", NULL, NULL };
    static const char *const shade_u[UNIFORMS] = {
        "u_field", "u_opacity", "u_opaque", "u_level", "u_soft", "u_rim", NULL, NULL };
    if (!link(&gl.splat, splat_vert_src, splat_frag_src, splat_u)) return false;
    if (!link(&gl.shade, shade_vert_src, shade_frag_src, shade_u)) return false;

    /* GLES2 promises points of one pixel and nothing more. Every driver fwm
     * will meet does hundreds, but a splat silently clipped to the limit would
     * be a liquid of dots, so the limit is read and honoured. */
    GLfloat range[2] = { 1.0f, 1.0f };
    glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, range);
    gl.max_point = range[1];
    gl.ok = true;
    wlr_log(WLR_INFO, "liquid: shaders ready (points up to %.0f px)", (double)gl.max_point);
    return true;
}

struct egl_save {
    EGLDisplay display;
    EGLContext context;
    EGLSurface draw, read;
};

static bool egl_enter(struct wlr_renderer *renderer, struct egl_save *save) {
    struct wlr_egl *egl = wlr_gles2_renderer_get_egl(renderer);
    if (!egl) return false;
    EGLDisplay dpy = wlr_egl_get_display(egl);
    EGLContext ctx = wlr_egl_get_context(egl);
    if (dpy == EGL_NO_DISPLAY || ctx == EGL_NO_CONTEXT) return false;

    save->display = eglGetCurrentDisplay();
    save->context = eglGetCurrentContext();
    save->draw    = eglGetCurrentSurface(EGL_DRAW);
    save->read    = eglGetCurrentSurface(EGL_READ);
    return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) == EGL_TRUE;
}

static void egl_leave(struct wlr_renderer *renderer, const struct egl_save *save) {
    if (save->display == EGL_NO_DISPLAY) {
        struct wlr_egl *egl = wlr_gles2_renderer_get_egl(renderer);
        eglMakeCurrent(wlr_egl_get_display(egl), EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        return;
    }
    eglMakeCurrent(save->display, save->draw, save->read, save->context);
}

/* ── the paint ───────────────────────────────────────────────────────── */

FwmLiquidPaint *liquid_paint_create(struct FwmServer *server, int w, int h) {
    if (!server || w <= 0 || h <= 0) return NULL;
    FwmLiquidPaint *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->server = server;
    p->w = w;
    p->h = h;
    p->fw = (int)ceil(w * FIELD_SCALE);
    p->fh = (int)ceil(h * FIELD_SCALE);

    /* Two pictures, taking turns: the scene may still be showing the last one
     * while the next is drawn — the same pair star_draw.c keeps. The field is
     * drawn into and then read back, so it needs a texture of its own memory,
     * made once for its life rather than once a frame (blur.h explains). */
    p->field = snapshot_alloc(server, p->fw, p->fh);
    p->out[0] = snapshot_alloc(server, w, h);
    p->out[1] = snapshot_alloc(server, w, h);
    if (!p->field || !p->out[0] || !p->out[1]) {
        liquid_paint_destroy(p);
        return NULL;
    }
    return p;
}

void liquid_paint_destroy(FwmLiquidPaint *p) {
    if (!p) return;
    if (p->field_tex) wlr_texture_destroy(p->field_tex);
    if (p->field) wlr_buffer_drop(p->field);
    if (p->opacity_tex) wlr_texture_destroy(p->opacity_tex);
    if (p->opacity) wlr_buffer_drop(p->opacity);
    for (int i = 0; i < 2; i++)
        if (p->out[i]) wlr_buffer_drop(p->out[i]);
    free(p);
}

int liquid_paint_width(const FwmLiquidPaint *p) { return p ? p->w : 0; }
int liquid_paint_height(const FwmLiquidPaint *p) { return p ? p->h : 0; }
struct wlr_buffer *liquid_paint_current(const FwmLiquidPaint *p) { return p ? p->current : NULL; }

void liquid_paint_finish(void) {
    gl.splat.id = gl.shade.id = 0;
    gl.tried = gl.ok = false;
    gl.owner = NULL;
}

static void box_union(struct wlr_box *a, const struct wlr_box *b) {
    if (wlr_box_empty(b)) return;
    if (wlr_box_empty(a)) { *a = *b; return; }
    int x0 = a->x < b->x ? a->x : b->x;
    int y0 = a->y < b->y ? a->y : b->y;
    int x1 = a->x + a->width  > b->x + b->width  ? a->x + a->width  : b->x + b->width;
    int y1 = a->y + a->height > b->y + b->height ? a->y + a->height : b->y + b->height;
    *a = (struct wlr_box){ x0, y0, x1 - x0, y1 - y0 };
}

/* The splats of every particle, into the framebuffer bound: nine times, the
 * last eight reflected in the walls (see below). */
static void splat_all(FwmLiquidPaint *p, const float *xy, const float *kinds, int count,
                      double reach, double spacing, float alpha_pass) {
    double r_field = reach * FIELD_SCALE;
    float size = (float)(2.0 * r_field);
    if (size > gl.max_point) size = gl.max_point;
    /* The bumps of a liquid at rest — one every spacing² of area — add up to
     * pi*R²/4 per spacing² of (1 - r²)³ each; scaled so that comes to
     * FIELD_INSIDE. */
    double sum = M_PI * reach * reach / 4.0 / (spacing * spacing);
    glUniform2f(gl.splat.u[0], 2.0f / (float)p->w, 2.0f / (float)p->h);
    glUniform1f(gl.splat.u[1], size);
    glUniform1f(gl.splat.u[2], (float)(FIELD_INSIDE / sum));
    glUniform1f(gl.splat.u[5], alpha_pass);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, xy);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 0, kinds);
    glEnableVertexAttribArray(1);
    /* Nine times: where it is, and reflected in each wall and in each
     * corner. Liquid against a wall is liquid right up to it, but a splat
     * there has half its reach in the wall where no particle can be, so the
     * field along a wall read like a free surface: at the level, dipping
     * under it wherever the particles happened to lie a little high — a row
     * of bubbles along the floor, with the bright rim of a surface drawn
     * around each. With every particle mirrored behind the wall the field
     * there is whole, and the rim is left where it belongs, on the surface. A
     * mirror image far from its wall lands outside the picture and costs
     * nothing. */
    const float W = (float)p->w, H = (float)p->h;
    const float xs[3][2] = { { 1.0f, 0.0f }, { -1.0f, 0.0f }, { -1.0f, 2.0f * W } };
    const float ys[3][2] = { { 1.0f, 0.0f }, { -1.0f, 0.0f }, { -1.0f, 2.0f * H } };
    for (int m = 0; m < 9; m++) {
        const float mirror[4] = { xs[m % 3][0], xs[m % 3][1], ys[m / 3][0], ys[m / 3][1] };
        glUniform4fv(gl.splat.u[3], 1, mirror);
        glDrawArrays(GL_POINTS, 0, count);
    }
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(0);
}

struct wlr_buffer *liquid_paint_draw(FwmLiquidPaint *p, const float *xy, const float *kinds,
                                     int count, double spacing,
                                     const float palette[LIQUID_PAINT_KINDS][4],
                                     struct wlr_box *damage) {
    if (!p || !palette || spacing <= 0.0) return NULL;
    struct wlr_renderer *renderer = p->server ? p->server->wlr_renderer : NULL;
    if (!liquid_paint_supported(renderer)) return NULL;

    if (!p->field_tex) {
        p->field_tex = wlr_texture_from_buffer(renderer, p->field);
        if (!p->field_tex) return NULL;
    }
    struct wlr_gles2_texture_attribs at = {0};
    wlr_gles2_texture_get_attribs(p->field_tex, &at);
    if (at.target != GL_TEXTURE_2D) return NULL;

    /* What the liquid covers now: its particles, and a splat's reach around
     * them. */
    double reach = SPLAT_REACH * spacing;
    struct wlr_box now = {0};
    if (count > 0) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int i = 0; i < count; i++) {
            float x = xy[2 * i], y = xy[2 * i + 1];
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
        int bx0 = (int)floor(x0 - reach) - 1, by0 = (int)floor(y0 - reach) - 1;
        int bx1 = (int)ceil(x1 + reach) + 1,  by1 = (int)ceil(y1 + reach) + 1;
        if (bx0 < 0) bx0 = 0;
        if (by0 < 0) by0 = 0;
        if (bx1 > p->w) bx1 = p->w;
        if (by1 > p->h) by1 = p->h;
        if (bx1 > bx0 && by1 > by0) now = (struct wlr_box){ bx0, by0, bx1 - bx0, by1 - by0 };
    }

    struct egl_save save;
    if (!egl_enter(renderer, &save)) return NULL;
    struct wlr_buffer *target = p->out[p->next];
    bool ok = false;
    if (!programs_ready(renderer)) goto out;

    /* The palette straight, as the shaders mix it; and whether the liquids
     * in it are all as opaque as each other, in which case one number says
     * how opaque and the second field is not drawn. */
    float straight[LIQUID_PAINT_KINDS][4];
    bool one_opacity = true;
    for (int k = 0; k < LIQUID_PAINT_KINDS; k++) {
        float a = palette[k][3], inv = a > 0.0f ? 1.0f / a : 0.0f;
        for (int c = 0; c < 3; c++) straight[k][c] = palette[k][c] * inv;
        straight[k][3] = a;
    }
    float opaque = -1.0f;
    if (count > 0) {
        unsigned seen = 0;
        for (int i = 0; i < count; i++) seen |= 1u << ((int)kinds[i] & 7);
        for (int k = 0; k < LIQUID_PAINT_KINDS; k++) {
            if (!(seen & (1u << k))) continue;
            if (opaque < 0.0f) opaque = straight[k][3];
            else if (fabsf(straight[k][3] - opaque) > 1e-3f) one_opacity = false;
        }
    }
    if (!one_opacity && !p->opacity) {
        p->opacity = snapshot_alloc(p->server, p->fw, p->fh);
        if (p->opacity) p->opacity_tex = wlr_texture_from_buffer(renderer, p->opacity);
        if (!p->opacity_tex) one_opacity = true;
    }

    GLuint field_fbo = wlr_gles2_renderer_get_buffer_fbo(renderer, p->field);
    GLuint out_fbo = wlr_gles2_renderer_get_buffer_fbo(renderer, target);
    GLuint opacity_fbo = !one_opacity ? wlr_gles2_renderer_get_buffer_fbo(renderer, p->opacity) : 0;
    if (!field_fbo || !out_fbo || (!one_opacity && !opacity_fbo)) {
        wlr_log(WLR_ERROR, "liquid: no FBO to draw into");
        goto out;
    }
    glDisable(GL_SCISSOR_TEST);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    /* 1. The field: every particle a soft bump in its liquid's colour, added
     * up — the colours times the amount, and the amount. Box px to clip
     * space, with y=0 at -1: the same way up as wlroots' own buffers, so the
     * field and the picture agree without a flip. */
    glBindFramebuffer(GL_FRAMEBUFFER, field_fbo);
    glViewport(0, 0, p->fw, p->fh);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (count > 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glUseProgram(gl.splat.id);
        glUniform4fv(gl.splat.u[4], LIQUID_PAINT_KINDS, &straight[0][0]);
        splat_all(p, xy, kinds, count, reach, spacing, 0.0f);
        if (!one_opacity) {
            glBindFramebuffer(GL_FRAMEBUFFER, opacity_fbo);
            glClear(GL_COLOR_BUFFER_BIT);
            splat_all(p, xy, kinds, count, reach, spacing, 1.0f);
        }
        glDisable(GL_BLEND);
    }

    /* 2. The picture: the field cut at the surface. */
    glBindFramebuffer(GL_FRAMEBUFFER, out_fbo);
    glViewport(0, 0, p->w, p->h);
    glClear(GL_COLOR_BUFFER_BIT);
    if (count > 0) {
        glUseProgram(gl.shade.id);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, at.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glUniform1i(gl.shade.u[0], 0);
        if (!one_opacity) {
            struct wlr_gles2_texture_attribs ot = {0};
            wlr_gles2_texture_get_attribs(p->opacity_tex, &ot);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, ot.tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glActiveTexture(GL_TEXTURE0);
        }
        glUniform1i(gl.shade.u[1], 1);
        glUniform1f(gl.shade.u[2], one_opacity ? (opaque < 0.0f ? 1.0f : opaque) : -1.0f);
        float level = SURFACE_LEVEL * FIELD_INSIDE;
        glUniform1f(gl.shade.u[3], level);
        /* At a flat surface the field falls from about half the inside to
         * nothing across one reach; a band that wide per pixel, either side
         * of the level, is one pixel of antialiasing. */
        glUniform1f(gl.shade.u[4], (float)(0.5 * FIELD_INSIDE / reach));
        glUniform1f(gl.shade.u[5], RIM_DEPTH * FIELD_INSIDE);
        static const GLfloat quad[8] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,  1.0f, 1.0f };
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (!one_opacity) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE0);
        }
    }
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ok = true;

out:
    egl_leave(renderer, &save);
    if (!ok) return NULL;

    if (damage) {
        *damage = p->drawn;
        box_union(damage, &now);
    }
    p->drawn = now;
    p->current = target;
    p->next ^= 1;
    return target;
}
