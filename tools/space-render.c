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

/* The solar-system wallpaper, drawn to a PNG without a compositor.
 *
 * The same shader string fwm compiles (src/space_shader.h) and the same world
 * (src/space.c), driven the way space_draw.c drives them, on a surfaceless EGL
 * context: one frame, one glReadPixels, one PNG. Nothing here is linked into
 * fwm. It is how the picture is looked at while it is being changed — a
 * session round trip per nudge is too slow to tune a colour by.
 *
 * Build and run, from the repository root:
 *   cc -Isrc $(pkg-config --cflags xkbcommon) tools/space-render.c src/space.c \
 *      -lEGL -lGLESv2 -lz -lm -o build/space-render
 *   build/space-render --desktop 2 --star black_hole -o space.png
 *
 * --help lists the rest. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "space.h"
#include "../src/space_shader.h"

/* config.c is not linked in: the two name lists, as [space] spells them. */
const char *const config_space_kinds[SPACE_KIND_COUNT] = {
    "terra", "ochre", "saturn", "ice", "lava", "ocean", "neptune", "titan", "selva", "jupiter",
};
const char *const config_space_stars[SPACE_STAR_COUNT] = {
    "sun", "red_giant", "blue_giant", "white_dwarf", "pulsar", "black_hole",
};

static void usage(void) {
    printf("space-render — draw fwm's solar-system wallpaper to a PNG\n\n"
           "  --size WxH      picture, px (default 1280x720)\n"
           "  --desktop N     which desktop's planet the camera is at (default 0)\n"
           "  --star NAME     sun red_giant blue_giant white_dwarf pulsar black_hole\n"
           "  --planet D=K    put kind K (terra ... jupiter) on desktop D; repeatable\n"
           "  --moons N       N windows on that desktop, open for a long time\n"
           "  --focus         the camera at the last of them, not at the planet\n"
           "  --birth S       the last moon born S seconds ago\n"
           "  --boom S        the last moon blown up S seconds ago\n"
           "  --swap K --wave U   the planet mid-swap to kind K, wave at U (0..1)\n"
           "  --seed N        another camera angle and other moons\n"
           "  --bench N       draw N frames and report ms each\n"
           "  -o FILE         output PNG (default space.png)\n");
}

/* --- PNG, by hand, as star-render does it. ------------------------------ */
static bool write_png(const char *path, const unsigned char *rgba, int w, int h) {
    unsigned long raw_len = (unsigned long)(w * 3 + 1) * h;
    unsigned char *raw = malloc(raw_len);
    if (!raw) return false;
    unsigned char *p = raw;
    for (int y = 0; y < h; y++) {
        *p++ = 0;
        for (int x = 0; x < w; x++) {
            /* Bottom-up from GL, so the rows are turned here (and uFlip is
             * left off in the shader). */
            const unsigned char *s = rgba + ((size_t)(h - 1 - y) * w + x) * 4;
            *p++ = s[0]; *p++ = s[1]; *p++ = s[2];
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
    unsigned char ihdr[13] = {
        (unsigned char)(w >> 24), (unsigned char)(w >> 16), (unsigned char)(w >> 8), (unsigned char)w,
        (unsigned char)(h >> 24), (unsigned char)(h >> 16), (unsigned char)(h >> 8), (unsigned char)h,
        8, 2, 0, 0, 0 };
    struct { const char *tag; const unsigned char *data; unsigned long len; } ch[3] = {
        { "IHDR", ihdr, sizeof ihdr }, { "IDAT", z, zlen }, { "IEND", NULL, 0 },
    };
    for (int i = 0; i < 3; i++) {
        unsigned char len[4] = { (unsigned char)(ch[i].len >> 24), (unsigned char)(ch[i].len >> 16),
                                 (unsigned char)(ch[i].len >> 8), (unsigned char)ch[i].len };
        fwrite(len, 1, 4, f);
        unsigned long crc = crc32(0, (const unsigned char *)ch[i].tag, 4);
        fwrite(ch[i].tag, 1, 4, f);
        if (ch[i].len) { crc = crc32(crc, ch[i].data, (unsigned)ch[i].len); fwrite(ch[i].data, 1, ch[i].len, f); }
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

static int name_index(const char *const *names, int n, const char *s) {
    for (int i = 0; i < n; i++) if (!strcmp(names[i], s)) return i;
    return -1;
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    int W = 1280, H = 720, desk = 0, moons = 0, bench = 0, swap = -1;
    bool focus = false;
    double birth = -1.0, boom = -1.0, wave = 1.0;
    uint64_t seed = 1;
    const char *out = "space.png";
    SpaceConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    for (int d = 0; d < FWM_DESKTOPS; d++) cfg.planets[d] = d % SPACE_KIND_COUNT;
    cfg.fps = 30.0; cfg.scale = 1.0; cfg.exposure = 1.0; cfg.speed = 1.0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        if (!strcmp(a, "--size") && v) { sscanf(v, "%dx%d", &W, &H); i++; continue; }
        if (!strcmp(a, "--desktop") && v) { desk = atoi(v); i++; continue; }
        if (!strcmp(a, "--moons") && v) { moons = atoi(v); i++; continue; }
        if (!strcmp(a, "--birth") && v) { birth = atof(v); i++; continue; }
        if (!strcmp(a, "--boom") && v) { boom = atof(v); i++; continue; }
        if (!strcmp(a, "--wave") && v) { wave = atof(v); i++; continue; }
        if (!strcmp(a, "--seed") && v) { seed = strtoull(v, NULL, 10); i++; continue; }
        if (!strcmp(a, "--bench") && v) { bench = atoi(v); i++; continue; }
        if (!strcmp(a, "--focus")) { focus = true; continue; }
        if (!strcmp(a, "--star") && v) {
            int k = name_index(config_space_stars, SPACE_STAR_COUNT, v);
            if (k < 0) { fprintf(stderr, "no star %s\n", v); return 1; }
            cfg.star = k; i++; continue;
        }
        if (!strcmp(a, "--swap") && v) {
            swap = name_index(config_space_kinds, SPACE_KIND_COUNT, v);
            if (swap < 0) { fprintf(stderr, "no kind %s\n", v); return 1; }
            i++; continue;
        }
        if (!strcmp(a, "--planet") && v) {
            int d = -1; char k[32] = "";
            if (sscanf(v, "%d=%31s", &d, k) == 2 && d >= 0 && d < FWM_DESKTOPS) {
                int ki = name_index(config_space_kinds, SPACE_KIND_COUNT, k);
                if (ki >= 0) cfg.planets[d] = ki;
            }
            i++; continue;
        }
        if (!strcmp(a, "-o") && v) { out = v; i++; continue; }
        out = a;
    }
    if (desk < 0 || desk >= FWM_DESKTOPS) desk = 0;
    if (moons > SPACE_MAX_MOONS) moons = SPACE_MAX_MOONS;

    /* The world, as space_draw.c would have it after a while. */
    static Space sp;
    space_init(&sp, &cfg, 1000.0, seed);
    SpaceCam cam;
    memset(&cam, 0, sizeof(cam));
    SpaceCam *cams[SPACE_PLANETS] = { 0 };
    cams[desk] = &cam;
    double aspect = (double)W / H, real = 100.0;
    SpaceWindow win[SPACE_MAX_MOONS];
    for (int k = 0; k < moons; k++) { win[k].id = (uint32_t)(k + 1); win[k].desktop = desk; }
    space_cam_update(&sp, &cam, desk, real, true);
    space_sync_windows(&sp, win, moons > 0 ? moons - 1 : 0, cams, aspect, real, true);
    if (moons > 0) {
        /* The last one through the ordinary door, so it is placed in view. */
        space_sync_windows(&sp, win, moons, cams, aspect, real, false);
        SpaceMoon *last = (SpaceMoon *)space_find_moon(&sp, (uint32_t)moons);
        if (last) last->born = birth >= 0.0 ? real - birth : -1e9;
        if (focus) {
            space_sync_focus(&sp, (uint32_t)moons);
            memset(&cam, 0, sizeof(cam));
            space_cam_update(&sp, &cam, desk, real, true);
        }
        if (boom >= 0.0) {
            /* Closed `boom` seconds ago, by nobody watching it up close. */
            SpaceCam *none[SPACE_PLANETS] = { 0 };
            space_sync_windows(&sp, win, moons - 1, none, aspect, real - boom, false);
        }
    }
    if (swap >= 0) {
        cfg.planets[desk] = swap;
        space_apply_config(&sp, &cfg, real - wave * 3.2, false);
    }
    space_step(&sp, &cfg, 0.0, real);
    space_cam_update(&sp, &cam, desk, real, true);
    SpaceUniforms u;
    space_uniforms(&sp, &cam, &cfg, real, aspect, &u);

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) { fprintf(stderr, "no EGL\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig ec; EGLint n = 0;
    if (!eglChooseConfig(dpy, ca, &ec, 1, &n) || n < 1) { fprintf(stderr, "no EGL config\n"); return 1; }
    EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, ec, EGL_NO_CONTEXT, xa);
    EGLint sa[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, ec, sa);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE) { fprintf(stderr, "no EGL context\n"); return 1; }
    eglMakeCurrent(dpy, surf, surf, ctx);

    GLuint vs = compile(GL_VERTEX_SHADER, space_vert_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, space_frag_src);
    if (!vs || !fs) return 1;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glLinkProgram(prog);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "link failed\n"); return 1; }
    glUseProgram(prog);

#define L(name) glGetUniformLocation(prog, name)
    glUniform2f(L("uRes"), (GLfloat)W, (GLfloat)H);
    glUniform1f(L("uTime"), u.time);
    glUniform1f(L("uReal"), u.real);
    glUniform1f(L("uFlip"), 0.0f);
    glUniform3fv(L("uEye"), 1, u.eye);
    glUniform3fv(L("uRight"), 1, u.right);
    glUniform3fv(L("uUp"), 1, u.up);
    glUniform3fv(L("uFwd"), 1, u.fwd);
    glUniform1f(L("uTanHalf"), u.tan_half);
    glUniform1f(L("uExposure"), u.exposure);
    glUniform3fv(L("uPP[0]"), SPACE_PLANETS, u.pp);
    glUniform4fv(L("uPR[0]"), SPACE_PLANETS, u.pr);
    glUniform4fv(L("uPX[0]"), SPACE_PLANETS, u.px);
    glUniform1f(L("uStarR"), u.star_r);
    glUniform1f(L("uStarType"), u.star_type);
    glUniform3fv(L("uStarCol"), 1, u.star_col);
    glUniform3fv(L("uSun"), 1, u.sun);
    glUniform1f(L("uStarFlash"), u.star_flash);
    glUniform4fv(L("uSat[0]"), SPACE_MAX_MOONS, u.sat);
    glUniform4fv(L("uSatB[0]"), SPACE_MAX_MOONS, u.satb);
    glUniform4fv(L("uBoom[0]"), SPACE_MAX_BOOMS, u.boom);
    glUniform4fv(L("uBoomN[0]"), SPACE_MAX_BOOMS, u.boomn);
#undef L

    static const GLfloat quad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, W, H);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    if (bench > 0) {
        double t0 = now_s();
        for (int i = 0; i < bench; i++) { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glFinish(); }
        printf("%.2f ms/frame at %dx%d\n", (now_s() - t0) * 1000.0 / bench, W, H);
    }
    unsigned char *px = malloc((size_t)W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (!write_png(out, px, W, H)) { fprintf(stderr, "cannot write %s\n", out); return 1; }
    free(px);
    return 0;
}
