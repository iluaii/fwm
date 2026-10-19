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

#include "space_gl.h"

#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/gles2.h>
#include <wlr/render/egl.h>
#include <wlr/util/log.h>

#include "space_shader.h"

/* The program, per renderer, as star_gl.c and rotate.c keep theirs: a GL
 * object belongs to the context (here, the share group) it was made in. */
static struct {
    GLuint id;
    GLint u_res, u_time, u_real, u_flip, u_eye, u_right, u_up, u_fwd, u_tan_half, u_exposure;
    GLint u_pp, u_pr, u_px;
    GLint u_star_r, u_star_type, u_star_col, u_sun, u_star_flash;
    GLint u_sat, u_satb, u_boom, u_boomn;
    bool tried, ok;
    struct wlr_renderer *owner;
} prog;

/* The build, off the compositor's thread.
 *
 * Cold — first run, or after Mesa or fwm changed and the shader cache no
 * longer matches — this shader takes Mesa about three seconds: most of it in
 * the link, the rest in the first draw. Done on the compositor's own thread
 * that is three seconds of nothing moving, starting the instant the wallpaper
 * is picked. GL_KHR_parallel_shader_compile would be the usual way out, and
 * does work — but not on wlroots' context: wlroots turns its debug output on,
 * and once a Mesa context has had debug output it builds everything in the
 * calling thread, whatever is switched back off afterwards.
 *
 * So the build gets a context of its own, sharing objects with wlroots', on a
 * thread of its own. It may block there as long as Mesa likes; the frame only
 * asks whether it is done. It also draws one throwaway frame, because Mesa
 * finishes compiling a program at its first draw, and that part would land on
 * the compositor's thread too. Warm from the cache, all of it is tens of ms. */
static struct {
    bool started, joined;
    pthread_t thread;
    atomic_int state;          /* 0 building, 1 built, -1 failed */
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint id;
    char log[2048];
} build;

bool space_gl_supported(struct wlr_renderer *renderer) {
    return renderer && wlr_renderer_is_gles2(renderer);
}

static bool has_egl_extension(EGLDisplay dpy, const char *name) {
    const char *all = eglQueryString(dpy, EGL_EXTENSIONS);
    if (!all) return false;
    size_t n = strlen(name);
    for (const char *p = all; (p = strstr(p, name)); p += n)
        if ((p == all || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
    return false;
}

/* Compile, link, and say why not in `log`. Runs on whichever context is
 * current: the build thread's, or the compositor's when there is no thread. */
static GLuint build_program(char *log, size_t cap) {
    const char *srcs[2] = { space_vert_src, space_frag_src };
    GLenum types[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
    GLuint sh[2] = { 0, 0 };
    for (int i = 0; i < 2; i++) {
        sh[i] = glCreateShader(types[i]);
        glShaderSource(sh[i], 1, &srcs[i], NULL);
        glCompileShader(sh[i]);
        GLint ok = GL_FALSE;
        glGetShaderiv(sh[i], GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char l[1800];
            glGetShaderInfoLog(sh[i], sizeof l, NULL, l);
            snprintf(log, cap, "%s shader would not compile: %s", i ? "fragment" : "vertex", l);
            for (int j = 0; j <= i; j++) glDeleteShader(sh[j]);
            return 0;
        }
    }
    GLuint id = glCreateProgram();
    glAttachShader(id, sh[0]);
    glAttachShader(id, sh[1]);
    glBindAttribLocation(id, 0, "a_pos");
    glLinkProgram(id);
    glDetachShader(id, sh[0]);
    glDetachShader(id, sh[1]);
    glDeleteShader(sh[0]);
    glDeleteShader(sh[1]);
    GLint ok = GL_FALSE;
    glGetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char l[1800];
        glGetProgramInfoLog(id, sizeof l, NULL, l);
        snprintf(log, cap, "shader would not link: %s", l);
        glDeleteProgram(id);
        return 0;
    }
    return id;
}

/* One frame into a scratch target, so that what Mesa leaves for the first draw
 * is done here. Same state the real draw has: no blend, an RGBA target. */
static void warm_up(GLuint id) {
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glViewport(0, 0, 8, 8);
        glDisable(GL_BLEND);
        glUseProgram(id);
        glUniform2f(glGetUniformLocation(id, "uRes"), 8.0f, 8.0f);
        static const GLfloat quad[8] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,  1.0f, 1.0f };
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(0);
        glUseProgram(0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
}

static void *build_thread(void *arg) {
    (void)arg;
    if (eglMakeCurrent(build.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, build.ctx) != EGL_TRUE) {
        snprintf(build.log, sizeof build.log, "the build context would not become current");
        atomic_store(&build.state, -1);
        return NULL;
    }
    build.id = build_program(build.log, sizeof build.log);
    if (build.id) warm_up(build.id);
    /* Everything done before the compositor's context is told it may use it. */
    glFinish();
    eglMakeCurrent(build.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    atomic_store(&build.state, build.id ? 1 : -1);
    return NULL;
}

/* Start the build thread on a context that shares wlroots' objects. It has to
 * be created the way wlroots made its own, or EGL refuses the share: the same
 * reset notification strategy, and no config. False means there is no thread
 * and the caller builds in place. */
static bool build_start(struct wlr_renderer *renderer) {
    struct wlr_egl *egl = wlr_gles2_renderer_get_egl(renderer);
    if (!egl) return false;
    EGLDisplay dpy = wlr_egl_get_display(egl);
    EGLContext share = wlr_egl_get_context(egl);
    if (!has_egl_extension(dpy, "EGL_KHR_no_config_context") ||
        !has_egl_extension(dpy, "EGL_KHR_surfaceless_context"))
        return false;
    EGLint attribs[5];
    int n = 0;
    attribs[n++] = EGL_CONTEXT_CLIENT_VERSION;
    attribs[n++] = 2;
    if (has_egl_extension(dpy, "EGL_EXT_create_context_robustness")) {
        attribs[n++] = EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT;
        attribs[n++] = EGL_LOSE_CONTEXT_ON_RESET_EXT;
    }
    attribs[n] = EGL_NONE;
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, share, attribs);
    if (ctx == EGL_NO_CONTEXT) return false;

    memset(&build, 0, sizeof(build));
    build.dpy = dpy;
    build.ctx = ctx;
    atomic_store(&build.state, 0);
    if (pthread_create(&build.thread, NULL, build_thread, NULL) != 0) {
        eglDestroyContext(dpy, ctx);
        return false;
    }
    build.started = true;
    return true;
}

/* The thread is finished (or is waited for): collect it. */
static void build_join(void) {
    if (!build.started || build.joined) return;
    pthread_join(build.thread, NULL);
    eglDestroyContext(build.dpy, build.ctx);
    build.joined = true;
}

/* Where the uniforms are, once there is a program to ask. */
static void program_locate(void) {
    GLuint id = prog.id;
    prog.u_res        = glGetUniformLocation(id, "uRes");
    prog.u_time       = glGetUniformLocation(id, "uTime");
    prog.u_real       = glGetUniformLocation(id, "uReal");
    prog.u_flip       = glGetUniformLocation(id, "uFlip");
    prog.u_eye        = glGetUniformLocation(id, "uEye");
    prog.u_right      = glGetUniformLocation(id, "uRight");
    prog.u_up         = glGetUniformLocation(id, "uUp");
    prog.u_fwd        = glGetUniformLocation(id, "uFwd");
    prog.u_tan_half   = glGetUniformLocation(id, "uTanHalf");
    prog.u_exposure   = glGetUniformLocation(id, "uExposure");
    prog.u_pp         = glGetUniformLocation(id, "uPP[0]");
    prog.u_pr         = glGetUniformLocation(id, "uPR[0]");
    prog.u_px         = glGetUniformLocation(id, "uPX[0]");
    prog.u_star_r     = glGetUniformLocation(id, "uStarR");
    prog.u_star_type  = glGetUniformLocation(id, "uStarType");
    prog.u_star_col   = glGetUniformLocation(id, "uStarCol");
    prog.u_sun        = glGetUniformLocation(id, "uSun");
    prog.u_star_flash = glGetUniformLocation(id, "uStarFlash");
    prog.u_sat        = glGetUniformLocation(id, "uSat[0]");
    prog.u_satb       = glGetUniformLocation(id, "uSatB[0]");
    prog.u_boom       = glGetUniformLocation(id, "uBoom[0]");
    prog.u_boomn      = glGetUniformLocation(id, "uBoomN[0]");
}

static SpaceGlResult program_ready(struct wlr_renderer *renderer) {
    if (prog.owner != renderer) {
        /* The old program belonged to a context that is gone. */
        build_join();
        prog.id = 0;
        prog.tried = prog.ok = false;
        prog.owner = renderer;
    }
    if (prog.ok) return SPACE_GL_DRAWN;
    if (!prog.tried) {
        prog.tried = true;
        if (build_start(renderer)) {
            wlr_log(WLR_INFO, "space: building the shader in the background");
        } else {
            /* No second context to be had: built here, the slow way. */
            char log[2048] = "";
            prog.id = build_program(log, sizeof log);
            if (!prog.id) {
                wlr_log(WLR_ERROR, "space: %s", log);
                return SPACE_GL_FAILED;
            }
            program_locate();
            prog.ok = true;
            wlr_log(WLR_INFO, "space: shader ready");
            return SPACE_GL_DRAWN;
        }
    }
    if (!build.started || build.joined) return SPACE_GL_FAILED;
    int st = atomic_load(&build.state);
    if (st == 0) return SPACE_GL_PENDING;
    build_join();
    if (st < 0) {
        wlr_log(WLR_ERROR, "space: %s", build.log);
        return SPACE_GL_FAILED;
    }
    prog.id = build.id;
    program_locate();
    prog.ok = true;
    wlr_log(WLR_INFO, "space: shader ready");
    return SPACE_GL_DRAWN;
}

/* Into the renderer's context and back out, exactly as star_gl.c does it: the
 * compositor may be anywhere in its own frame when this runs. */
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
        eglMakeCurrent(wlr_egl_get_display(egl), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return;
    }
    eglMakeCurrent(save->display, save->draw, save->read, save->context);
}

SpaceGlResult space_gl_render(struct wlr_renderer *renderer, struct wlr_buffer *dst,
                              const SpaceUniforms *u) {
    if (!space_gl_supported(renderer) || !dst || !u) return SPACE_GL_FAILED;

    struct egl_save save;
    if (!egl_enter(renderer, &save)) return SPACE_GL_FAILED;

    SpaceGlResult res = program_ready(renderer);
    if (res != SPACE_GL_DRAWN) goto out;
    res = SPACE_GL_FAILED;

    GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(renderer, dst);
    if (!fbo) {
        wlr_log(WLR_ERROR, "space: no FBO for the destination buffer");
        goto out;
    }

    int w = dst->width, h = dst->height;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    /* Every pixel is written, opaque: no clear, no blend. */
    glDisable(GL_BLEND);

    glUseProgram(prog.id);
    glUniform2f(prog.u_res, (GLfloat)w, (GLfloat)h);
    glUniform1f(prog.u_time, u->time);
    glUniform1f(prog.u_real, u->real);
    /* Row 0 of an FBO on a wlroots buffer is the top of the picture. */
    glUniform1f(prog.u_flip, 1.0f);
    glUniform3fv(prog.u_eye, 1, u->eye);
    glUniform3fv(prog.u_right, 1, u->right);
    glUniform3fv(prog.u_up, 1, u->up);
    glUniform3fv(prog.u_fwd, 1, u->fwd);
    glUniform1f(prog.u_tan_half, u->tan_half);
    glUniform1f(prog.u_exposure, u->exposure);
    glUniform3fv(prog.u_pp, SPACE_PLANETS, u->pp);
    glUniform4fv(prog.u_pr, SPACE_PLANETS, u->pr);
    glUniform4fv(prog.u_px, SPACE_PLANETS, u->px);
    glUniform1f(prog.u_star_r, u->star_r);
    glUniform1f(prog.u_star_type, u->star_type);
    glUniform3fv(prog.u_star_col, 1, u->star_col);
    glUniform3fv(prog.u_sun, 1, u->sun);
    glUniform1f(prog.u_star_flash, u->star_flash);
    glUniform4fv(prog.u_sat, SPACE_MAX_MOONS, u->sat);
    glUniform4fv(prog.u_satb, SPACE_MAX_MOONS, u->satb);
    glUniform4fv(prog.u_boom, SPACE_MAX_BOOMS, u->boom);
    glUniform4fv(prog.u_boomn, SPACE_MAX_BOOMS, u->boomn);

    static const GLfloat quad[8] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,  1.0f, 1.0f };
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);

    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    res = SPACE_GL_DRAWN;

out:
    egl_leave(renderer, &save);
    return res;
}

void space_gl_finish(void) {
    /* A build still running holds a context in the old renderer's share
     * group: it is waited for here, while that group still exists. The
     * program itself is forgotten, not deleted — its context is going or gone,
     * as with the star. */
    build_join();
    prog.id = 0;
    prog.tried = prog.ok = false;
    prog.owner = NULL;
}
