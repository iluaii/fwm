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

#include "rotate.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

/* The quad is drawn with the corner positions computed on the CPU, so the
 * shaders carry no matrix at all: the rotation IS the four vertices. */
static const char vert_src[] =
    "attribute vec2 pos;\n"
    "attribute vec2 texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "  v_texcoord = texcoord;\n"
    "  gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char frag_2d_src[] =
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D tex;\n"
    "void main() { gl_FragColor = texture2D(tex, v_texcoord); }\n";

/* A dmabuf imported by the GLES2 renderer usually lands on the external target
 * instead of GL_TEXTURE_2D, and sampling it needs a different sampler type —
 * hence two programs rather than one. */
static const char frag_ext_src[] =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform samplerExternalOES tex;\n"
    "void main() { gl_FragColor = texture2D(tex, v_texcoord); }\n";

/* The background key: a straight copy, except that pixels of the window's
 * background colour come out at `alpha` instead of whole — the translucent
 * background a terminal draws for itself, for a client that does not.
 *
 * The colour is either handed in (key.a = 1) or found here, per fragment, by
 * a vote among twelve points along the left, right and bottom edges, where a
 * page's background shows if it shows anywhere: the colour at least four of
 * them agree on is the background. Points that are not opaque — the shadow a
 * client draws around itself — do not vote, and no agreement means no key:
 * the window is copied as it is rather than guessed at. Twelve fetches and a
 * hundred-odd comparisons a fragment, on a commit and not on a frame.
 *
 * Every pixel is then un-mixed, the way an image editor's colour-to-alpha
 * does it: read as something laid over the background, with the least alpha
 * that explains it, and only that much of it kept. Antialiased text, the soft
 * edge of an icon, a rounded corner, a shadow fading out over a dozen pixels —
 * all of them ARE something laid over the background, and come out exactly
 * right. Anything cut by distance from the background instead left a step
 * where the cut fell: first the opaque rim round every letter, then a ring
 * across every shadow wider than the cut. The price is honest: a card a shade
 * off the page, or the darkest parts of a picture on a dark page, are mostly
 * background too, and come out as a tint over what is behind the window. */
#define KEY_BODY \
    "varying vec2 v_texcoord;\n" \
    "uniform vec4 color;\n" \
    "uniform float alpha;\n" \
    "uniform float tol;\n" \
    "vec3 straight(vec4 c) { return c.a > 0.004 ? c.rgb / c.a : vec3(0.0); }\n" \
    "float dist(vec3 a, vec3 b) { vec3 d = abs(a - b); return max(d.r, max(d.g, d.b)); }\n" \
    "void main() {\n" \
    "  vec4 c = texture2D(tex, v_texcoord);\n" \
    "  vec3 k = color.rgb;\n" \
    "  bool have = color.a > 0.5;\n" \
    "  if (!have) {\n" \
    "    vec4 s[12];\n" \
    "    s[0] = texture2D(tex, vec2(0.01, 0.35)); s[1] = texture2D(tex, vec2(0.01, 0.50));\n" \
    "    s[2] = texture2D(tex, vec2(0.01, 0.65)); s[3] = texture2D(tex, vec2(0.01, 0.80));\n" \
    "    s[4] = texture2D(tex, vec2(0.99, 0.35)); s[5] = texture2D(tex, vec2(0.99, 0.50));\n" \
    "    s[6] = texture2D(tex, vec2(0.99, 0.65)); s[7] = texture2D(tex, vec2(0.99, 0.80));\n" \
    "    s[8] = texture2D(tex, vec2(0.20, 0.99)); s[9] = texture2D(tex, vec2(0.40, 0.99));\n" \
    "    s[10] = texture2D(tex, vec2(0.60, 0.99)); s[11] = texture2D(tex, vec2(0.80, 0.99));\n" \
    "    int best = 0;\n" \
    "    for (int i = 0; i < 12; i++) {\n" \
    "      if (s[i].a < 0.98) continue;\n" \
    "      int n = 0;\n" \
    "      for (int j = 0; j < 12; j++)\n" \
    "        if (s[j].a >= 0.98 && dist(s[i].rgb, s[j].rgb) < tol) n++;\n" \
    "      if (n > best) { best = n; k = s[i].rgb; }\n" \
    "    }\n" \
    "    have = best >= 4;\n" \
    "  }\n" \
    "  if (!have) { gl_FragColor = c; return; }\n" \
    "  vec3 cs = straight(c);\n" \
    "  vec3 up = (cs - k) / max(vec3(1.0) - k, vec3(0.004));\n" \
    "  vec3 dn = (k - cs) / max(k, vec3(0.004));\n" \
    "  vec3 ch = max(up, dn);\n" \
    "  float a = clamp(max(ch.r, max(ch.g, ch.b)), 0.0, 1.0);\n" \
    "  if (dist(cs, k) < tol) a = 0.0;\n" \
    "  vec3 rgb = cs - (1.0 - a) * k * (1.0 - alpha);\n" \
    "  float oa = 1.0 - (1.0 - a) * (1.0 - alpha);\n" \
    "  gl_FragColor = vec4(rgb, oa) * c.a;\n" \
    "}\n"

static const char frag_key_2d_src[] =
    "precision mediump float;\n"
    "uniform sampler2D tex;\n"
    KEY_BODY;

static const char frag_key_ext_src[] =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES tex;\n"
    KEY_BODY;

struct program {
    GLuint id;
    GLint attr_pos, attr_texcoord, uni_tex;
    GLint uni_alpha, uni_color;   /* the strip's programs; -1 in the others */
    GLint uni_tol;                /* the key's; -1 in the others */
    bool tried;       /* compiled once already, successfully or not */
};

/* fwm has exactly one renderer and one thread, so the programs live here
 * rather than being threaded through every caller. `owner` guards the only way
 * that could go wrong: a config reload that replaces the renderer would leave
 * these belonging to a destroyed context. */
static struct wlr_renderer *owner;
static struct program prog_2d, prog_ext;
static struct program prog3d_2d, prog3d_ext, prog3d_solid;
static struct program prog_key_2d, prog_key_ext;

/* A different renderer than the one the programs were built on: forget them
 * without touching GL. The old context is the only thing that could free them
 * and it is already gone — calling into it would be a use-after-free, and the
 * objects died with it anyway. */
static void programs_forget(struct wlr_renderer *renderer) {
    memset(&prog_2d, 0, sizeof(prog_2d));
    memset(&prog_ext, 0, sizeof(prog_ext));
    memset(&prog3d_2d, 0, sizeof(prog3d_2d));
    memset(&prog3d_ext, 0, sizeof(prog3d_ext));
    memset(&prog3d_solid, 0, sizeof(prog3d_solid));
    memset(&prog_key_2d, 0, sizeof(prog_key_2d));
    memset(&prog_key_ext, 0, sizeof(prog_key_ext));
    owner = renderer;
}

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(shader, sizeof(log) - 1, NULL, log);
        wlr_log(WLR_ERROR, "rotate: shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/* The strip's vertex shader. `pos` is (x_ndc, y_ndc, w): pre-multiplying the
 * position by w and handing w through as the clip coordinate makes the
 * rasterizer's own divide reproduce the projection exactly — and, the point of
 * the exercise, interpolate the texture coordinate perspective-correctly. */
static const char vert3d_src[] =
    "attribute vec3 pos;\n"
    "attribute vec2 texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "  v_texcoord = texcoord;\n"
    "  gl_Position = vec4(pos.xy * pos.z, 0.0, pos.z);\n"
    "}\n";

static const char frag3d_2d_src[] =
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D tex;\n"
    "uniform float alpha;\n"
    "void main() { gl_FragColor = texture2D(tex, v_texcoord) * alpha; }\n";

static const char frag3d_ext_src[] =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform samplerExternalOES tex;\n"
    "uniform float alpha;\n"
    "void main() { gl_FragColor = texture2D(tex, v_texcoord) * alpha; }\n";

static const char frag3d_solid_src[] =
    "precision mediump float;\n"
    "uniform vec4 color;\n"
    "void main() { gl_FragColor = color; }\n";

static bool program_build_from(struct program *p, const char *vsrc, const char *frag_src);

static bool program_build(struct program *p, const char *frag_src) {
    return program_build_from(p, vert_src, frag_src);
}

static bool program_build_from(struct program *p, const char *vsrc, const char *frag_src) {
    if (p->tried) return p->id != 0;
    p->tried = true;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vsrc);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }

    GLuint id = glCreateProgram();
    glAttachShader(id, vs);
    glAttachShader(id, fs);
    glLinkProgram(id);
    /* The program keeps its own reference until it is linked; the shaders are
     * of no further use to us either way. */
    glDetachShader(id, vs);
    glDetachShader(id, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = GL_FALSE;
    glGetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetProgramInfoLog(id, sizeof(log) - 1, NULL, log);
        wlr_log(WLR_ERROR, "rotate: shader link failed: %s", log);
        glDeleteProgram(id);
        return false;
    }

    p->id = id;
    p->attr_pos = glGetAttribLocation(id, "pos");
    p->attr_texcoord = glGetAttribLocation(id, "texcoord");
    p->uni_tex = glGetUniformLocation(id, "tex");
    p->uni_alpha = glGetUniformLocation(id, "alpha");
    p->uni_color = glGetUniformLocation(id, "color");
    p->uni_tol = glGetUniformLocation(id, "tol");
    return true;
}

bool rotate_supported(struct wlr_renderer *renderer) {
    return renderer && wlr_renderer_is_gles2(renderer);
}

/* Making the renderer's context current is the whole reason this file can
 * exist: wlroots hands out the EGL display and context precisely so a
 * compositor can render something the render pass API cannot express. The
 * previous context is restored afterwards — wlroots' own passes do the same,
 * and the current EGL context is global state shared with them. */
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
    save->draw = eglGetCurrentSurface(EGL_DRAW);
    save->read = eglGetCurrentSurface(EGL_READ);

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

/* Draw `count` textured vertices into `dst`. Positions are in NDC and texture
 * coordinates in [0,1], both already laid out by the caller — which is the
 * whole trick this file turns: the transform lives in the vertex array, never
 * in a matrix, so a rotation and a bend are the same draw call with different
 * numbers in it. */
struct blit_key {
    float color[4];   /* rgb straight; a = 1 for a given colour, 0 for auto */
    float alpha;
    float tol;
};

static bool blit_verts_keyed(struct wlr_renderer *renderer, struct wlr_buffer *dst,
                             struct wlr_texture *src, const GLfloat *verts,
                             const GLfloat *texcoords, int count, GLenum mode,
                             const struct blit_key *key);

static bool blit_verts(struct wlr_renderer *renderer, struct wlr_buffer *dst,
                       struct wlr_texture *src, const GLfloat *verts,
                       const GLfloat *texcoords, int count, GLenum mode) {
    return blit_verts_keyed(renderer, dst, src, verts, texcoords, count, mode, NULL);
}

static bool blit_verts_keyed(struct wlr_renderer *renderer, struct wlr_buffer *dst,
                             struct wlr_texture *src, const GLfloat *verts,
                             const GLfloat *texcoords, int count, GLenum mode,
                             const struct blit_key *key) {
    if (!rotate_supported(renderer) || !dst || !src) return false;
    if (owner != renderer) programs_forget(renderer);

    struct wlr_gles2_texture_attribs attribs = {0};
    if (!wlr_texture_is_gles2(src)) return false;
    wlr_gles2_texture_get_attribs(src, &attribs);

    struct egl_save save;
    if (!egl_enter(renderer, &save)) return false;

    bool ok = false;

    bool ext = attribs.target == GL_TEXTURE_EXTERNAL_OES;
    struct program *p;
    const char *frag;
    if (key) {
        p = ext ? &prog_key_ext : &prog_key_2d;
        frag = ext ? frag_key_ext_src : frag_key_2d_src;
    } else {
        p = ext ? &prog_ext : &prog_2d;
        frag = ext ? frag_ext_src : frag_2d_src;
    }
    if (!program_build(p, frag)) goto out;

    GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(renderer, dst);
    if (!fbo) {
        wlr_log(WLR_ERROR, "rotate: no FBO for the destination buffer");
        goto out;
    }

    int dw = dst->width, dh = dst->height;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, dw, dh);
    glDisable(GL_SCISSOR_TEST);
    /* The destination is bigger than the window (it has to hold the rotated or
     * bent shape), so everything the mesh does not cover must end up fully
     * transparent. */
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* No blending: the snapshot is already premultiplied and the target was
     * just cleared, so a straight copy preserves its alpha exactly. Blending
     * it over transparent black would multiply the colour by alpha twice and
     * leave a translucent window with dark fringes. */
    glDisable(GL_BLEND);

    glUseProgram(p->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(attribs.target, attribs.tex);
    /* Linear sampling is what makes a rotation look rotated rather than
     * shredded, and what keeps a stretched cell of the mesh smooth; clamping
     * keeps the edge pixels from wrapping around. */
    glTexParameteri(attribs.target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(attribs.target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(attribs.target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(p->uni_tex, 0);
    if (key) {
        glUniform4fv(p->uni_color, 1, key->color);
        glUniform1f(p->uni_alpha, key->alpha);
        glUniform1f(p->uni_tol, key->tol);
        /* One to one: nearest reads exactly the pixel there, and the votes
         * compare real pixels rather than blends of two. */
        glTexParameteri(attribs.target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    /* Client-side arrays, so a buffer left bound by whoever ran last would be
     * read instead of ours. */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(p->attr_pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glVertexAttribPointer(p->attr_texcoord, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
    glEnableVertexAttribArray(p->attr_pos);
    glEnableVertexAttribArray(p->attr_texcoord);

    glDrawArrays(mode, 0, count);

    glDisableVertexAttribArray(p->attr_pos);
    glDisableVertexAttribArray(p->attr_texcoord);
    glBindTexture(attribs.target, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ok = true;

out:
    egl_leave(renderer, &save);
    return ok;
}

bool rotate_blit(struct wlr_renderer *renderer, struct wlr_buffer *dst,
                 struct wlr_texture *src, int src_w, int src_h, double angle) {
    if (!dst || src_w <= 0 || src_h <= 0) return false;

    int dw = dst->width, dh = dst->height;

    /* The four corners of the source rectangle, rotated about the center of
     * the destination. Same y-down matrix Box2D's polygon uses in this world,
     * so what is drawn is exactly the box that collides. */
    double c = cos(angle), s = sin(angle);
    double hw = src_w / 2.0, hh = src_h / 2.0;
    const double local[4][2] = {
        { -hw, -hh }, {  hw, -hh }, {  hw,  hh }, { -hw,  hh },
    };
    /* Texture coordinates follow the same corner order. v=0 is the top row of
     * the source, and NDC y=-1 is the top row of the destination FBO (wlroots
     * renders into buffers y-down), so top maps to top with no flip. */
    static const GLfloat texcoords[8] = {
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
    };
    GLfloat verts[8];
    for (int i = 0; i < 4; i++) {
        double x = dw / 2.0 + (c * local[i][0] - s * local[i][1]);
        double y = dh / 2.0 + (s * local[i][0] + c * local[i][1]);
        verts[i * 2 + 0] = (GLfloat)(2.0 * x / dw - 1.0);
        verts[i * 2 + 1] = (GLfloat)(2.0 * y / dh - 1.0);
    }
    /* Triangle fan over the corners in order: 0-1-2, 0-2-3. */
    return blit_verts(renderer, dst, src, verts, texcoords, 4, GL_TRIANGLE_FAN);
}

bool key_blit(struct wlr_renderer *renderer, struct wlr_buffer *dst,
              struct wlr_texture *src, const float *color, float alpha) {
    if (!dst) return false;
    /* The whole destination, one to one. Same corner order and orientation as
     * rotate_blit at angle 0. */
    static const GLfloat verts[8] = {
        -1.0f, -1.0f,  1.0f, -1.0f,  1.0f, 1.0f,  -1.0f, 1.0f,
    };
    static const GLfloat texcoords[8] = {
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
    };
    struct blit_key key = {
        .color = { 0.0f, 0.0f, 0.0f, 0.0f },
        .alpha = alpha,
        /* A step of four in eight bits: tight enough that text in a colour
         * near the background keeps its strokes, loose enough for a page that
         * dithers its gradient by a shade. */
        .tol = 4.0f / 255.0f,
    };
    if (color) {
        key.color[0] = color[0];
        key.color[1] = color[1];
        key.color[2] = color[2];
        key.color[3] = 1.0f;
    }
    return blit_verts_keyed(renderer, dst, src, verts, texcoords, 4,
                            GL_TRIANGLE_FAN, &key);
}

/* Two triangles per cell of the lattice. A strip would need degenerate
 * vertices to jump rows and the mesh is small enough that the saving is not
 * worth the trap; a list is what it looks like. */
#define WARP_MAX_VERTS ((WARP_MAX_GRID - 1) * (WARP_MAX_GRID - 1) * 6)
static GLfloat warp_verts[WARP_MAX_VERTS * 2];
static GLfloat warp_tex[WARP_MAX_VERTS * 2];

bool warp_blit(struct wlr_renderer *renderer, struct wlr_buffer *dst,
               struct wlr_texture *src, int grid, const float *pts) {
    if (!dst || !pts) return false;
    if (grid < 2 || grid > WARP_MAX_GRID) return false;

    double dw = dst->width, dh = dst->height;
    if (dw <= 0.0 || dh <= 0.0) return false;

    int n = 0;
    for (int j = 0; j < grid - 1; j++) {
        for (int i = 0; i < grid - 1; i++) {
            /* The cell's four corners, and the same four in texture space. */
            const int ci[4] = { j * grid + i,       j * grid + i + 1,
                                (j + 1) * grid + i + 1, (j + 1) * grid + i };
            const int cu[4] = { i, i + 1, i + 1, i };
            const int cv[4] = { j, j,     j + 1, j + 1 };
            static const int tri[6] = { 0, 1, 2, 0, 2, 3 };

            for (int k = 0; k < 6; k++) {
                int c = tri[k];
                warp_verts[n * 2 + 0] = (GLfloat)(2.0 * pts[ci[c] * 2 + 0] / dw - 1.0);
                warp_verts[n * 2 + 1] = (GLfloat)(2.0 * pts[ci[c] * 2 + 1] / dh - 1.0);
                warp_tex[n * 2 + 0] = (GLfloat)cu[c] / (GLfloat)(grid - 1);
                warp_tex[n * 2 + 1] = (GLfloat)cv[c] / (GLfloat)(grid - 1);
                n++;
            }
        }
    }

    return blit_verts(renderer, dst, src, warp_verts, warp_tex, n, GL_TRIANGLES);
}

/* ── the strip's pass ─────────────────────────────────────────────────── */

/* One pass at a time, held between begin and end: the EGL context is global
 * state and nesting two of these would restore the wrong one. */
static struct {
    struct wlr_renderer *renderer;
    struct egl_save save;
    int dw, dh;
    bool open;
} pass;

bool scene3d_begin(struct wlr_renderer *renderer, struct wlr_buffer *dst) {
    if (pass.open || !rotate_supported(renderer) || !dst) return false;
    if (owner != renderer) programs_forget(renderer);

    /* Context first, THEN the FBO: asking the renderer for a framebuffer is a GL
     * call, and blit_verts above has always had that order. Getting it the
     * other way round happened to work here and would have stopped working the
     * first time nothing else had left a context current. */
    if (!egl_enter(renderer, &pass.save)) return false;

    GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(renderer, dst);
    if (!fbo) {
        /* Not an error worth logging every frame — the caller falls back. */
        egl_leave(renderer, &pass.save);
        return false;
    }

    pass.renderer = renderer;
    pass.dw = dst->width;
    pass.dh = dst->height;
    pass.open = true;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, pass.dw, pass.dh);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Everything in the pass is premultiplied, and cards do overlap at the
     * edges once the strip is turned, so this one composes rather than copies
     * — unlike rotate_blit, which owns its whole destination. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    return true;
}

/* Lay out vertices for one primitive. Pre-multiplying the position by w and
 * handing w through as the clip coordinate makes the rasterizer's own divide
 * reproduce the projection exactly — and, the point of the exercise,
 * interpolate the texture coordinate perspective-correctly. */
static int scene3d_fill(const struct scene3d_vert *v, int n,
                        GLfloat *pos, GLfloat *uv) {
    for (int i = 0; i < n; i++) {
        pos[i * 3 + 0] = 2.0f * v[i].x / pass.dw - 1.0f;
        pos[i * 3 + 1] = 2.0f * v[i].y / pass.dh - 1.0f;
        pos[i * 3 + 2] = v[i].w > 0.001f ? v[i].w : 0.001f;
        uv[i * 2 + 0] = v[i].u;
        uv[i * 2 + 1] = v[i].v;
    }
    return n;
}

static GLfloat scene3d_pos[SCENE3D_MAX_VERTS * 3];
static GLfloat scene3d_uv[SCENE3D_MAX_VERTS * 2];

static void scene3d_draw(struct program *p, int count, GLenum mode) {
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(p->attr_pos, 3, GL_FLOAT, GL_FALSE, 0, scene3d_pos);
    glEnableVertexAttribArray(p->attr_pos);
    if (p->attr_texcoord >= 0) {
        glVertexAttribPointer(p->attr_texcoord, 2, GL_FLOAT, GL_FALSE, 0, scene3d_uv);
        glEnableVertexAttribArray(p->attr_texcoord);
    }

    glDrawArrays(mode, 0, count);

    glDisableVertexAttribArray(p->attr_pos);
    if (p->attr_texcoord >= 0) glDisableVertexAttribArray(p->attr_texcoord);
}

bool scene3d_quad(struct wlr_texture *tex, const struct scene3d_vert v[4],
                  float alpha) {
    if (!pass.open || !tex || !v) return false;
    if (!wlr_texture_is_gles2(tex)) return false;

    struct wlr_gles2_texture_attribs attribs = {0};
    wlr_gles2_texture_get_attribs(tex, &attribs);

    struct program *p = attribs.target == GL_TEXTURE_EXTERNAL_OES
                            ? &prog3d_ext : &prog3d_2d;
    if (!program_build_from(p, vert3d_src,
                            p == &prog3d_ext ? frag3d_ext_src : frag3d_2d_src))
        return false;

    int count = scene3d_fill(v, 4, scene3d_pos, scene3d_uv);

    glUseProgram(p->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(attribs.target, attribs.tex);
    glTexParameteri(attribs.target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* Clamped, or a card turned far enough would sample the opposite edge of
     * its own wallpaper along the seam. */
    glTexParameteri(attribs.target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(attribs.target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(p->uni_tex, 0);
    if (p->uni_alpha >= 0) glUniform1f(p->uni_alpha, alpha);

    scene3d_draw(p, count, GL_TRIANGLE_STRIP);

    glBindTexture(attribs.target, 0);
    glUseProgram(0);
    return true;
}

bool scene3d_quad_solid(const float rgba[4], const struct scene3d_vert v[4]) {
    if (!pass.open || !rgba || !v) return false;
    if (!program_build_from(&prog3d_solid, vert3d_src, frag3d_solid_src)) return false;

    int count = scene3d_fill(v, 4, scene3d_pos, scene3d_uv);

    glUseProgram(prog3d_solid.id);
    if (prog3d_solid.uni_color >= 0)
        glUniform4f(prog3d_solid.uni_color, rgba[0], rgba[1], rgba[2], rgba[3]);

    scene3d_draw(&prog3d_solid, count, GL_TRIANGLE_STRIP);

    glUseProgram(0);
    return true;
}

static struct {
    GLuint tex;
    int w, h;
} capture;

unsigned scene3d_capture(void) {
    if (!pass.open || pass.dw <= 0 || pass.dh <= 0) return 0;
    if (!capture.tex) {
        glGenTextures(1, &capture.tex);
        if (!capture.tex) return 0;
        capture.w = capture.h = 0;
    }
    glBindTexture(GL_TEXTURE_2D, capture.tex);
    if (capture.w != pass.dw || capture.h != pass.dh) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pass.dw, pass.dh, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        capture.w = pass.dw;
        capture.h = pass.dh;
    }
    /* Straight off the framebuffer that is bound right now — no readback, no
     * allocation after the first frame. */
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, pass.dw, pass.dh);
    glBindTexture(GL_TEXTURE_2D, 0);
    return capture.tex;
}

void scene3d_end(void) {
    if (!pass.open) return;
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    egl_leave(pass.renderer, &pass.save);
    pass.open = false;
}

void rotate_shutdown(struct wlr_renderer *renderer) {
    if (!renderer || !rotate_supported(renderer)) return;
    capture.tex = 0;   /* the context it lived in is going; forgetting is the job */
    capture.w = capture.h = 0;

    struct program *all[] = { &prog_2d, &prog_ext,
                              &prog3d_2d, &prog3d_ext, &prog3d_solid,
                              &prog_key_2d, &prog_key_ext };
    bool any = false;
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        if (all[i]->id) any = true;
    if (!any) {
        owner = NULL;
        return;
    }

    struct egl_save save;
    if (egl_enter(renderer, &save)) {
        for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
            if (all[i]->id) glDeleteProgram(all[i]->id);
        egl_leave(renderer, &save);
    }
    programs_forget(NULL);
}
