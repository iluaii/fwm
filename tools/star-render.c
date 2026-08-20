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

/* The star, drawn to a PNG without a compositor.
 *
 * src/star_shader.h says the shader can be looked at offscreen. This is the
 * thing that does it: the same source string, compiled against a surfaceless
 * EGL context, one quad, one glReadPixels, one PNG. Nothing here is linked
 * into fwm and nothing in fwm links to it.
 *
 * It exists because the alternative is a fifteen-second round trip through a
 * headless compositor for every number that gets nudged — and because the
 * shader's framing depends on numbers the compositor computes (how many
 * Schwarzschild radii of canvas a hole is given), which are easy to get wrong
 * and impossible to see from the source.
 *
 * Build and run, from the repository root:
 *   cc tools/star-render.c -lEGL -lGLESv2 -lz -lm -o build/star-render
 *   build/star-render --phase 3 --radius 39 --side 522 -o hole.png
 *
 * --help lists the rest. Defaults describe the black hole a 6-solar-mass
 * remnant produces with the shipped [star] radius, which is the tightest
 * framing fwm ever asks the shader for.
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "../src/star_shader.h"

struct opts {
    int side;
    double radius, time, lum, phase, beam, angle, incl, roll, lens;
    double blast, birth, aim, form;
    float color[3];
    const char *out;
    const char *bg;      /* PNG-free: a raw BGRA dump to stand in for the desktop */
    const char *raw;     /* where to dump the premultiplied RGBA, for checking alpha */
};

static void usage(void) {
    printf("star-render — draw fwm's star shader to a PNG\n\n"
           "  --side N      canvas, px (default 522: [star] radius 90 * 2.9 * 2)\n"
           "  --radius R    the star's radius inside it, px (default 39: a\n"
           "                6-solar-mass hole at the shipped [star] radius)\n"
           "  --phase P     0 burning, 1 collapsing, 2 pulsar, 3 hole (default 3)\n"
           "  --time T      seconds (default 3)\n"
           "  --lum L, --incl I, --roll R, --lens K, --angle A, --beam B,\n"
           "  --blast X, --birth X, --aim X, --form X, --color r,g,b\n"
           "  --bg FILE     raw BGRA, side x side, as the desktop behind a hole\n"
           "  --raw FILE    also dump the premultiplied RGBA result, alpha and all\n"
           "  -o FILE       output PNG (default star.png)\n");
}

/* --- PNG, by hand: one IDAT, zlib for the deflate. ---------------------- */
static bool write_png(const char *path, const unsigned char *rgba, int w, int h) {
    unsigned long raw_len = (unsigned long)(w * 3 + 1) * h;
    unsigned char *raw = malloc(raw_len);
    if (!raw) return false;
    unsigned char *p = raw;
    for (int y = 0; y < h; y++) {
        *p++ = 0;  /* filter: none */
        for (int x = 0; x < w; x++) {
            /* glReadPixels gives bottom-up; PNG wants top-down. And the
             * shader writes for an ARGB8888 buffer — B, G, R, A in memory —
             * so what GL hands back as red is the blue the scene would show.
             * Undone here, so this tool sees exactly what a session sees. */
            const unsigned char *s = rgba + ((size_t)(h - 1 - y) * w + x) * 4;
            *p++ = s[2]; *p++ = s[1]; *p++ = s[0];
        }
    }
    unsigned long zlen = compressBound(raw_len);
    unsigned char *z = malloc(zlen);
    if (!z || compress2(z, &zlen, raw, raw_len, 6) != Z_OK) { free(raw); free(z); return false; }
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return false; }
    const unsigned char sig[8] = {0x89,'P','N','G','\r','\n',0x1a,'\n'};
    fwrite(sig, 1, 8, f);
    unsigned char ihdr[13];
    ihdr[0] = (unsigned char)(w >> 24); ihdr[1] = (unsigned char)(w >> 16);
    ihdr[2] = (unsigned char)(w >> 8);  ihdr[3] = (unsigned char)w;
    ihdr[4] = (unsigned char)(h >> 24); ihdr[5] = (unsigned char)(h >> 16);
    ihdr[6] = (unsigned char)(h >> 8);  ihdr[7] = (unsigned char)h;
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    struct { const char *tag; const unsigned char *data; unsigned long len; } ch[3] = {
        { "IHDR", ihdr, sizeof ihdr }, { "IDAT", z, zlen }, { "IEND", NULL, 0 },
    };
    for (int i = 0; i < 3; i++) {
        unsigned char len[4] = { (unsigned char)(ch[i].len >> 24), (unsigned char)(ch[i].len >> 16),
                                 (unsigned char)(ch[i].len >> 8), (unsigned char)ch[i].len };
        fwrite(len, 1, 4, f);
        unsigned long crc = crc32(0, (const unsigned char *)ch[i].tag, 4);
        fwrite(ch[i].tag, 1, 4, f);
        if (ch[i].len) {
            crc = crc32(crc, ch[i].data, (unsigned)ch[i].len);
            fwrite(ch[i].data, 1, ch[i].len, f);
        }
        unsigned char c[4] = { (unsigned char)(crc >> 24), (unsigned char)(crc >> 16),
                               (unsigned char)(crc >> 8), (unsigned char)crc };
        fwrite(c, 1, 4, f);
    }
    fclose(f);
    free(z);
    return true;
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        return 0;
    }
    return s;
}

int main(int argc, char **argv) {
    struct opts o = { .side = 522, .radius = 39.0, .time = 3.0, .lum = 1.0,
                      .phase = 3.0, .incl = 0.17, .lens = 1.0, .birth = 1.0, .form = 1.0,
                      .color = { 1.0f, 0.85f, 0.6f }, .out = "star.png" };
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
#define NUM(name, field) if (!strcmp(a, name) && v) { o.field = atof(v); i++; continue; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        NUM("--side", side) NUM("--radius", radius) NUM("--time", time)
        NUM("--lum", lum) NUM("--phase", phase) NUM("--beam", beam)
        NUM("--angle", angle) NUM("--incl", incl) NUM("--roll", roll)
        NUM("--lens", lens) NUM("--blast", blast) NUM("--birth", birth)
        NUM("--aim", aim) NUM("--form", form)
#undef NUM
        if (!strcmp(a, "--color") && v) {
            sscanf(v, "%f,%f,%f", &o.color[0], &o.color[1], &o.color[2]); i++; continue;
        }
        if (!strcmp(a, "--bg") && v) { o.bg = v; i++; continue; }
        if (!strcmp(a, "--raw") && v) { o.raw = v; i++; continue; }
        if (!strcmp(a, "-o") && v) { o.out = v; i++; continue; }
        o.out = a;
    }
    if (o.side < 8) o.side = 8;

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) { fprintf(stderr, "no EGL display\n"); return 1; }
    if (!eglInitialize(dpy, NULL, NULL)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                          EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                          EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                          EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "no EGL config\n"); return 1;
    }
    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    EGLint surf_attr[] = { EGL_WIDTH, o.side, EGL_HEIGHT, o.side, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, surf_attr);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE) {
        fprintf(stderr, "no EGL context/surface\n"); return 1;
    }
    eglMakeCurrent(dpy, surf, surf, ctx);

    GLuint vs = compile(GL_VERTEX_SHADER, star_vert_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, star_frag_src);
    if (!vs || !fs) return 1;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glLinkProgram(prog);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096]; glGetProgramInfoLog(prog, sizeof log, NULL, log);
        fprintf(stderr, "link: %s\n", log); return 1;
    }
    glUseProgram(prog);

    GLuint tex = 0;
    int has_bg = 0;
    if (o.bg) {
        FILE *f = fopen(o.bg, "rb");
        if (f) {
            size_t len = (size_t)o.side * o.side * 4;
            unsigned char *px = malloc(len);
            if (px && fread(px, 1, len, f) == len) {
                /* The file is BGRA, which is what a wlroots capture gives
                 * and exactly the order the shader's own background is in, so
                 * the bytes go up as they lie; only the alpha is forced, since
                 * an X-format capture leaves it undefined. */
                for (size_t i = 0; i < len; i += 4) px[i + 3] = 255;
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, o.side, o.side, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, px);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                has_bg = 1;
            }
            free(px);
            fclose(f);
        }
        if (!has_bg) fprintf(stderr, "--bg: could not read %s as %dx%d BGRA\n", o.bg, o.side, o.side);
    }

#define U1(name, val) glUniform1f(glGetUniformLocation(prog, name), (GLfloat)(val))
    glUniform2f(glGetUniformLocation(prog, "u_res"), (GLfloat)o.side, (GLfloat)o.side);
    U1("u_time", o.time); U1("u_radius", o.radius); U1("u_lum", o.lum);
    U1("u_phase", o.phase); U1("u_beam", o.beam); U1("u_angle", o.angle);
    U1("u_incl", o.incl); U1("u_roll", o.roll); U1("u_lens", o.lens);
    U1("u_blast", o.blast); U1("u_birth", o.birth); U1("u_aim", o.aim);
    U1("u_form", o.form);
    U1("u_has_bg", has_bg ? 1.0 : 0.0);
    glUniform3f(glGetUniformLocation(prog, "u_color"), o.color[0], o.color[1], o.color[2]);
    glUniform4f(glGetUniformLocation(prog, "u_bg_rect"), 0.0f, 0.0f, 1.0f, 1.0f);
    glUniform1i(glGetUniformLocation(prog, "u_bg"), 0);
#undef U1

    glViewport(0, 0, o.side, o.side);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_BLEND);
    static const GLfloat quad[] = { -1, -1, 3, -1, -1, 3 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();

    unsigned char *px = malloc((size_t)o.side * o.side * 4);
    glReadPixels(0, 0, o.side, o.side, GL_RGBA, GL_UNSIGNED_BYTE, px);
    /* Premultiplied, and a PNG of it wants un-premultiplying to look right
     * over the black the viewer will put behind it. Composited over black is
     * exactly what the premultiplied value already is, so this writes it as
     * it stands. */
    /* The alpha matters as much as the colour: a hole that leaves a pixel
     * transparent is a hole you can see the unbent desktop through. */
    if (o.raw) {
        FILE *f = fopen(o.raw, "wb");
        if (f) { fwrite(px, 1, (size_t)o.side * o.side * 4, f); fclose(f); }
    }
    if (!write_png(o.out, px, o.side, o.side)) {
        fprintf(stderr, "could not write %s\n", o.out); return 1;
    }
    printf("%s (%dx%d, radius %.1f, phase %.0f)\n", o.out, o.side, o.side, o.radius, o.phase);
    free(px);
    if (tex) glDeleteTextures(1, &tex);
    eglDestroySurface(dpy, surf);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    return 0;
}
