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

#include "blur.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

/* Every pass here is one quad over the whole destination, so the vertex shader
 * is the same three lines each time. v_uv runs 0..1 across the destination
 * with 0 at the TOP: NDC y=-1 is the top row of a wlroots buffer FBO, and
 * texture v=0 is the top row of a texture, so the two agree with no flip —
 * the same convention rotate.c writes its corners in. */
static const char vert_src[] =
    "attribute vec2 pos;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = pos * 0.5 + 0.5;\n"
    "  gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

/* Shrink a picture into a smaller buffer, four taps at the corners of the
 * destination texel so nothing is skipped over on the way down — a single
 * bilinear tap averages two source pixels and would alias the other fourteen.
 *
 * `u_src_rect` says which part of the DESTINATION the source covers, which is
 * what lets the same program pull a panel-sized mask into its place inside a
 * bigger frame: everything outside comes out transparent, which is exactly
 * what a shadow needs around its edges.
 *
 * `u_clamp` is the other half of the same idea, and it is what keeps a panel
 * at the edge of the screen from going grey. The photograph is bigger than the
 * panel — it has to be, the shadow needs somewhere to fall — so a tray eight
 * pixels below the top of the screen is photographed with a band of nothing
 * above it, and a blur wide enough to be worth having pulls that nothing into
 * the frost. Clamping every tap into the part of the picture that is actually
 * ON the screen extends the edge instead, which is what a blur at a boundary
 * is supposed to do and what the measurement asked for: 9% of the brightness
 * was missing from the tray's frost before this. */
static const char frag_down_src[] =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D tex;\n"
    "uniform vec4 u_src_rect;\n"
    "uniform vec4 u_clamp;\n"
    "uniform vec2 u_halfpixel;\n"
    "void main() {\n"
    "  vec2 uv = (v_uv - u_src_rect.xy) / u_src_rect.zw;\n"
    "  vec2 inb = step(vec2(0.0), uv) * step(uv, vec2(1.0));\n"
    "  vec2 c = clamp(uv, u_clamp.xy, u_clamp.zw);\n"
    "  vec4 s = texture2D(tex, c + vec2(-u_halfpixel.x, -u_halfpixel.y));\n"
    "  s += texture2D(tex, c + vec2( u_halfpixel.x, -u_halfpixel.y));\n"
    "  s += texture2D(tex, c + vec2(-u_halfpixel.x,  u_halfpixel.y));\n"
    "  s += texture2D(tex, c + vec2( u_halfpixel.x,  u_halfpixel.y));\n"
    "  gl_FragColor = s * 0.25 * inb.x * inb.y;\n"
    "}\n";

/* Nine taps along one axis. Two of these are a separable gaussian, and the
 * radius they can reach is bought by how far down the picture was shrunk
 * first, never by more taps. */
static const char frag_blur_src[] =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D tex;\n"
    "uniform vec2 u_texel;\n"
    "uniform float u_w[5];\n"
    "uniform float u_off[4];\n"
    "void main() {\n"
    "  vec4 s = texture2D(tex, v_uv) * u_w[0];\n"
    "  vec2 d0 = u_texel * u_off[0];\n"
    "  vec2 d1 = u_texel * u_off[1];\n"
    "  vec2 d2 = u_texel * u_off[2];\n"
    "  vec2 d3 = u_texel * u_off[3];\n"
    "  s += (texture2D(tex, v_uv + d0) + texture2D(tex, v_uv - d0)) * u_w[1];\n"
    "  s += (texture2D(tex, v_uv + d1) + texture2D(tex, v_uv - d1)) * u_w[2];\n"
    "  s += (texture2D(tex, v_uv + d2) + texture2D(tex, v_uv - d2)) * u_w[3];\n"
    "  s += (texture2D(tex, v_uv + d3) + texture2D(tex, v_uv - d3)) * u_w[4];\n"
    "  gl_FragColor = s;\n"
    "}\n";

/* The one pass that decides what the pane looks like: the blurred desktop cut
 * to the panel's own coverage, over the panel's shadow.
 *
 * The coverage comes out of the panel's alpha divided by the fill it was
 * painted at — see BlurParams.fill. The shadow is the same coverage, blurred
 * and moved, and it is drawn only where the glass is NOT: a shadow under an
 * opaque pane is invisible, and one under a translucent pane would darken the
 * frost from underneath instead of the desktop. */
static const char frag_glass_src[] =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_backdrop;\n"
    "uniform sampler2D u_mask;\n"
    "uniform sampler2D u_shadow;\n"
    "uniform vec4 u_panel;\n"
    "uniform vec2 u_shift;\n"
    "uniform float u_fill;\n"
    "uniform float u_tint;\n"
    "uniform vec3 u_tint_color;\n"
    "uniform float u_bright;\n"
    "uniform float u_shadow_alpha;\n"
    "uniform vec3 u_shadow_color;\n"
    "uniform vec2 u_small;\n"
    "void main() {\n"
    "  vec2 muv = (v_uv - u_panel.xy) / u_panel.zw;\n"
    "  vec2 inb = step(vec2(0.0), muv) * step(muv, vec2(1.0));\n"
    "  float cov = clamp(texture2D(u_mask, clamp(muv, 0.0, 1.0)).a / u_fill, 0.0, 1.0);\n"
    "  cov *= inb.x * inb.y;\n"
    "  vec2 h = u_small * 0.5;\n"
    "  vec2 suv = clamp(v_uv - u_shift, 0.0, 1.0);\n"
    "  vec4 sm = texture2D(u_shadow, suv + vec2(-h.x, -h.y))\n"
    "          + texture2D(u_shadow, suv + vec2( h.x, -h.y))\n"
    "          + texture2D(u_shadow, suv + vec2(-h.x,  h.y))\n"
    "          + texture2D(u_shadow, suv + vec2( h.x,  h.y));\n"
    "  float sh = clamp(sm.a * 0.25 / u_fill, 0.0, 1.0) * u_shadow_alpha;\n"
    "  vec4 bd = texture2D(u_backdrop, v_uv + vec2(-h.x, -h.y))\n"
    "          + texture2D(u_backdrop, v_uv + vec2( h.x, -h.y))\n"
    "          + texture2D(u_backdrop, v_uv + vec2(-h.x,  h.y))\n"
    "          + texture2D(u_backdrop, v_uv + vec2( h.x,  h.y));\n"
    "  vec3 rgb = bd.rgb * 0.25;\n"
    "  rgb = mix(rgb * u_bright, u_tint_color, u_tint);\n"
    "  gl_FragColor = vec4(rgb, 1.0) * cov\n"
    "               + vec4(u_shadow_color, 1.0) * (sh * (1.0 - cov));\n"
    "}\n";

struct program {
    GLuint id;
    GLint attr_pos;
    /* down */
    GLint u_tex, u_src_rect, u_clamp, u_halfpixel;
    /* blur */
    GLint u_texel, u_w, u_off;
    /* glass */
    GLint u_backdrop, u_mask, u_shadow, u_panel, u_shift, u_fill;
    GLint u_tint, u_tint_color, u_bright, u_shadow_alpha, u_shadow_color, u_small;
    bool tried;
};

/* One renderer, one thread — the same reasoning rotate.c writes down. `owner`
 * catches the config reload that replaces the renderer: the programs belonged
 * to a context that no longer exists, and the only safe thing to do with them
 * is forget them where they lie. */
static struct wlr_renderer *owner;
static struct program prog_down, prog_blur, prog_glass;

static void programs_forget(struct wlr_renderer *renderer) {
    memset(&prog_down, 0, sizeof(prog_down));
    memset(&prog_blur, 0, sizeof(prog_blur));
    memset(&prog_glass, 0, sizeof(prog_glass));
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
        wlr_log(WLR_ERROR, "blur: shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool program_build(struct program *p, const char *frag_src) {
    if (p->tried) return p->id != 0;
    p->tried = true;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src);
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
    glDetachShader(id, vs);
    glDetachShader(id, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = GL_FALSE;
    glGetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetProgramInfoLog(id, sizeof(log) - 1, NULL, log);
        wlr_log(WLR_ERROR, "blur: shader link failed: %s", log);
        glDeleteProgram(id);
        return false;
    }

    p->id = id;
    p->attr_pos       = glGetAttribLocation(id, "pos");
    p->u_tex          = glGetUniformLocation(id, "tex");
    p->u_src_rect     = glGetUniformLocation(id, "u_src_rect");
    p->u_clamp        = glGetUniformLocation(id, "u_clamp");
    p->u_halfpixel    = glGetUniformLocation(id, "u_halfpixel");
    p->u_texel        = glGetUniformLocation(id, "u_texel");
    p->u_off          = glGetUniformLocation(id, "u_off[0]");
    p->u_w            = glGetUniformLocation(id, "u_w[0]");
    p->u_backdrop     = glGetUniformLocation(id, "u_backdrop");
    p->u_mask         = glGetUniformLocation(id, "u_mask");
    p->u_shadow       = glGetUniformLocation(id, "u_shadow");
    p->u_panel        = glGetUniformLocation(id, "u_panel");
    p->u_shift        = glGetUniformLocation(id, "u_shift");
    p->u_fill         = glGetUniformLocation(id, "u_fill");
    p->u_tint         = glGetUniformLocation(id, "u_tint");
    p->u_tint_color   = glGetUniformLocation(id, "u_tint_color");
    p->u_bright       = glGetUniformLocation(id, "u_bright");
    p->u_shadow_alpha = glGetUniformLocation(id, "u_shadow_alpha");
    p->u_shadow_color = glGetUniformLocation(id, "u_shadow_color");
    p->u_small        = glGetUniformLocation(id, "u_small");
    return true;
}

bool blur_supported(struct wlr_renderer *renderer) {
    return renderer && wlr_renderer_is_gles2(renderer);
}

/* Making the renderer's own context current, and giving it back exactly as it
 * was — the current EGL context is global state shared with wlroots' passes.
 * Third copy of this in the tree, and deliberately: each GLES2 file stands on
 * its own so that one of them failing to compile takes nothing else with it. */
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

/* Every texture this file samples is one fwm allocated and drew into itself,
 * or a cairo surface it uploaded — never a client's dmabuf — so they are all
 * plain 2D textures and there is no external-sampler variant to build. A
 * texture that is not is refused rather than drawn wrong. */
static bool bind_tex(struct wlr_texture *tex, GLenum unit) {
    if (!tex || !wlr_texture_is_gles2(tex)) return false;
    struct wlr_gles2_texture_attribs at = {0};
    wlr_gles2_texture_get_attribs(tex, &at);
    if (at.target != GL_TEXTURE_2D) return false;
    glActiveTexture(unit);
    glBindTexture(GL_TEXTURE_2D, at.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

/* A scratch buffer with its FBO resolved for this pass. The texture side comes
 * from the caller and is only borrowed. */
struct scratch {
    struct wlr_texture *tex;
    GLuint fbo;
    int w, h;
};

static bool scratch_open(struct wlr_renderer *r, const BlurScratch *in,
                         struct scratch *s) {
    if (!in || !in->buf || !in->tex) return false;
    s->tex = in->tex;
    s->w = in->buf->width;
    s->h = in->buf->height;
    s->fbo = wlr_gles2_renderer_get_buffer_fbo(r, in->buf);
    return s->fbo != 0;
}

static void quad_draw(struct program *p) {
    static const GLfloat quad[8] = {
        -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,  1.0f, 1.0f,
    };
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(p->attr_pos, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(p->attr_pos);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(p->attr_pos);
}

/* Bind an FBO as the target of the next quad. Nothing is blended anywhere in
 * this file: every pass writes a whole buffer of premultiplied pixels over a
 * target it has just cleared, which is a copy and not a composite. */
static void target(GLuint fbo, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

/* A nine-fetch gaussian on the texel grid.
 *
 * Nine taps spread half a sigma apart was the wrong shape for this: past a
 * sigma of two the taps sit MORE than a texel apart and the kernel starts
 * stepping over whole texels, which is sampling error dressed up as blur —
 * and it is what the tray's shadow came back as, a staircase.
 *
 * These sit on the grid instead. A discrete gaussian is evaluated at integer
 * offsets 0..8, and each adjacent pair is folded into ONE bilinear fetch placed
 * between them at their centre of mass — so nine fetches carry a seventeen-tap
 * kernel and every texel under it is actually read. Good to about sigma 4,
 * which is where 2*sigma reaches the end of the kernel. */
static void gauss(double sigma, GLfloat off[4], GLfloat w[5]) {
    if (sigma < 0.35) sigma = 0.35;

    double g[9];
    for (int i = 0; i < 9; i++) g[i] = exp(-(double)(i * i) / (2.0 * sigma * sigma));

    double total = g[0];
    double pw[4];
    for (int k = 0; k < 4; k++) {
        int i1 = 1 + 2 * k, i2 = 2 + 2 * k;
        pw[k] = g[i1] + g[i2];
        /* Where between the two texels the fetch has to land for bilinear to
         * hand back exactly their weighted sum. */
        off[k] = (GLfloat)((i1 * g[i1] + i2 * g[i2]) / pw[k]);
        total += 2.0 * pw[k];
    }

    w[0] = (GLfloat)(g[0] / total);
    for (int k = 0; k < 4; k++) w[k + 1] = (GLfloat)(pw[k] / total);
}

#define BLUR_MIN_SIDE 32

/* The sigma the gaussian actually runs at, once the picture has been shrunk.
 * Everything above chooses the scale that lands on it. */
#define BLUR_SCRATCH_SIGMA 3.0

/* A blur's number is how far it reaches; the gaussian that reaches that far has
 * a third of it for a sigma. Three sigma is where shadow.c stops drawing its
 * penumbra, where the manual says [glass] radius stops, and where the room
 * glass.c leaves around a panel ends — so a gaussian run at the reach itself
 * spills three times as far as it is allowed to and gets cut off by the edge of
 * the pane, which is the rectangle that was standing around every panel. */
#define BLUR_REACH_SIGMAS 3.0

static double blur_sigma(double reach) {
    return reach / BLUR_REACH_SIGMAS;
}

double blur_scratch_scale(double radius, double shadow_radius, int w, int h) {
    double sigma = blur_sigma(radius > shadow_radius ? radius : shadow_radius);
    if (sigma <= BLUR_SCRATCH_SIGMA || w <= 0 || h <= 0)
        return 1.0;  /* blur it where it stands */

    double s = BLUR_SCRATCH_SIGMA / sigma;
    int side = w < h ? w : h;
    double least = (double)BLUR_MIN_SIDE / side;
    if (s < least) s = least;
    if (s > 1.0) s = 1.0;
    return s;
}

/* How many times the nine-tap pair has to run to add up to `sigma`, and at what
 * sigma each run goes.
 *
 * Nine taps reach 2 sigma with the taps half a sigma apart, and past about five
 * texels of sigma that spacing starts stepping over whole texels — the kernel
 * gets wider without getting any better. Gaussians compose by the square,
 * though: n runs of sigma s are one run of s*sqrt(n). So a blur too wide for
 * one pass is several narrower ones, on buffers small enough that the extra
 * draws cost nothing worth counting. */
#define BLUR_SIGMA_PER_ROUND 4.0
#define BLUR_MAX_ROUNDS      16

static int blur_rounds(double sigma) {
    if (sigma <= BLUR_SIGMA_PER_ROUND) return 1;
    double k = sigma / BLUR_SIGMA_PER_ROUND;
    int n = (int)ceil(k * k);
    if (n > BLUR_MAX_ROUNDS) n = BLUR_MAX_ROUNDS;
    return n;
}

/* Shrink `src` (which covers `rect` of the destination frame) into `dst`,
 * taking no tap outside `keep` — the part of the source, in its own
 * coordinates, that holds real pixels. */
static bool pass_down(struct scratch *dst, struct wlr_texture *src,
                      const float rect[4], const float keep[4]) {
    struct program *p = &prog_down;
    target(dst->fbo, dst->w, dst->h);
    glUseProgram(p->id);
    if (!bind_tex(src, GL_TEXTURE0)) return false;
    glUniform1i(p->u_tex, 0);
    glUniform4f(p->u_src_rect, rect[0], rect[1], rect[2], rect[3]);
    glUniform4f(p->u_clamp, keep[0], keep[1], keep[2], keep[3]);
    /* Half a DESTINATION texel, expressed in the source's own coordinates:
     * the source may cover only part of the frame, and then its texels are
     * that much bigger than the frame's. */
    glUniform2f(p->u_halfpixel,
                (GLfloat)(0.5 / (dst->w * rect[2])),
                (GLfloat)(0.5 / (dst->h * rect[3])));
    quad_draw(p);
    return true;
}

/* One axis of the gaussian, `from` into `to`. */
static bool pass_blur(struct scratch *to, struct scratch *from,
                      double sigma, bool vertical) {
    struct program *p = &prog_blur;
    GLfloat w[5], off[4];
    gauss(sigma, off, w);

    target(to->fbo, to->w, to->h);
    glUseProgram(p->id);
    if (!bind_tex(from->tex, GL_TEXTURE0)) return false;
    glUniform1i(p->u_tex, 0);
    glUniform2f(p->u_texel,
                vertical ? 0.0f : (GLfloat)(1.0 / from->w),
                vertical ? (GLfloat)(1.0 / from->h) : 0.0f);
    glUniform1fv(p->u_w, 5, w);
    glUniform1fv(p->u_off, 4, off);
    quad_draw(p);
    return true;
}

/* One separable gaussian of (sigma_x, sigma_y), however many rounds that takes.
 * `hot` holds the picture going in and holds it again coming out; `tmp` is
 * scribbled on. Both are left in a defined state either way. */
static bool blur_chain(struct scratch *hot, struct scratch *tmp,
                       double sigma_x, double sigma_y) {
    int n = blur_rounds(sigma_x > sigma_y ? sigma_x : sigma_y);
    double px = sigma_x / sqrt((double)n);
    double py = sigma_y / sqrt((double)n);
    for (int i = 0; i < n; i++) {
        if (!pass_blur(tmp, hot, px, false)) return false;
        if (!pass_blur(hot, tmp, py, true)) return false;
    }
    return true;
}

bool blur_glass(struct wlr_renderer *renderer, struct wlr_buffer *dst,
                struct wlr_texture *backdrop, struct wlr_texture *mask,
                const BlurScratch *sa, const BlurScratch *sb,
                const BlurScratch *sc, const BlurParams *pr) {
    if (!blur_supported(renderer) || !dst || !backdrop || !mask || !pr) return false;
    if (pr->fill <= 0.0 || pr->panel_w <= 0 || pr->panel_h <= 0) return false;
    if (owner != renderer) programs_forget(renderer);

    struct egl_save save;
    if (!egl_enter(renderer, &save)) return false;

    bool ok = false;
    struct scratch a = {0}, b = {0}, c = {0};
    GLuint dst_fbo = 0;
    int dw = 0, dh = 0;
    double sx = 0.0, sy = 0.0;
    double sigma = 0.0, shadow_sigma = 0.0;
    float panel[4] = {0};
    bool shadow = false;
    struct program *g = NULL;

    if (!program_build(&prog_down, frag_down_src)) goto out;
    if (!program_build(&prog_blur, frag_blur_src)) goto out;
    if (!program_build(&prog_glass, frag_glass_src)) goto out;

    dst_fbo = wlr_gles2_renderer_get_buffer_fbo(renderer, dst);
    if (!dst_fbo) {
        wlr_log(WLR_ERROR, "blur: no FBO for the destination buffer");
        goto out;
    }
    if (!scratch_open(renderer, sa, &a)) goto out;
    if (!scratch_open(renderer, sb, &b)) goto out;
    if (!scratch_open(renderer, sc, &c)) goto out;

    dw = dst->width; dh = dst->height;
    /* How far down the scratch buffers actually are, taken from the buffers
     * rather than recomputed: the caller rounded them to whole pixels and a
     * blur that assumed the exact fraction would be off by that rounding. */
    sx = (double)a.w / dw; sy = (double)a.h / dh;

    static const float whole[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    static const float keep_all[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    /* An empty or inverted keep rect would clamp every tap onto one corner and
     * fill the pane with a single colour, so a caller that has not filled it in
     * gets the whole picture rather than a flat one. */
    const float *keep = (pr->keep[2] > pr->keep[0] && pr->keep[3] > pr->keep[1])
                            ? pr->keep : keep_all;
    sigma = blur_sigma(pr->radius);
    if (!pass_down(&a, backdrop, whole, keep)) goto out;
    if (!blur_chain(&a, &b, sigma * sx, sigma * sy)) goto out;

    /* Where the panel sits in the frame, as a fraction of it. The frame is
     * bigger by the room the shadow needs, and the mask covers only the panel:
     * everything else is the margin the shadow falls into. */
    panel[0] = (float)(pr->panel_x / (double)dw);
    panel[1] = (float)(pr->panel_y / (double)dh);
    panel[2] = (float)(pr->panel_w / (double)dw);
    panel[3] = (float)(pr->panel_h / (double)dh);

    shadow = pr->shadow_alpha > 0.0 && pr->shadow_radius > 0.0;
    if (shadow) {
        shadow_sigma = blur_sigma(pr->shadow_radius);
        if (!pass_down(&b, mask, panel, keep_all)) goto out;
        if (!blur_chain(&b, &c, shadow_sigma * sx, shadow_sigma * sy)) goto out;
    }

    g = &prog_glass;
    target(dst_fbo, dw, dh);
    glUseProgram(g->id);
    if (!bind_tex(a.tex, GL_TEXTURE0)) goto out;
    glUniform1i(g->u_backdrop, 0);
    if (!bind_tex(mask, GL_TEXTURE1)) goto out;
    glUniform1i(g->u_mask, 1);
    if (!bind_tex(shadow ? b.tex : a.tex, GL_TEXTURE2)) goto out;
    glUniform1i(g->u_shadow, 2);

    glUniform4f(g->u_panel, panel[0], panel[1], panel[2], panel[3]);
    glUniform2f(g->u_shift, (GLfloat)(pr->shadow_dx / dw), (GLfloat)(pr->shadow_dy / dh));
    /* One texel of the blurred buffers, for the four-tap read that stretches
     * them back up. Straight bilinear is only continuous, not smooth: its
     * kink at every texel boundary is exactly the grid of soft squares the
     * shadow was arriving as. */
    glUniform2f(g->u_small, (GLfloat)(1.0 / a.w), (GLfloat)(1.0 / a.h));
    glUniform1f(g->u_fill, (GLfloat)pr->fill);
    glUniform1f(g->u_tint, (GLfloat)pr->tint);
    glUniform3f(g->u_tint_color, pr->tint_color[0], pr->tint_color[1], pr->tint_color[2]);
    glUniform1f(g->u_bright, (GLfloat)pr->brightness);
    glUniform1f(g->u_shadow_alpha, shadow ? (GLfloat)pr->shadow_alpha : 0.0f);
    glUniform3f(g->u_shadow_color, pr->shadow_color[0], pr->shadow_color[1],
                pr->shadow_color[2]);
    quad_draw(g);

    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ok = true;

out:
    egl_leave(renderer, &save);
    return ok;
}

void blur_shutdown(struct wlr_renderer *renderer) {
    /* Deleting a program is only safe in the context that made it, and by the
     * time anything calls this that context is on its way out. Forgetting is
     * the whole job — the same bargain star_gl_finish strikes. */
    programs_forget(renderer);
    owner = NULL;
}
