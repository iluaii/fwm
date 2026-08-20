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

#include "star_gl.h"

#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <math.h>
#include <stdlib.h>

#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/gles2.h>
#include <wlr/render/egl.h>
#include <wlr/util/log.h>

#include "star_shader.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* One program, rebuilt if the renderer is ever replaced — the same bookkeeping
 * rotate.c does, and for the same reason: a GL object belongs to the context
 * it was made in. */
static struct {
    GLuint id;
    GLint attr_pos;
    GLint u_res, u_time, u_radius, u_color, u_lum, u_phase, u_beam;
    GLint u_bg, u_has_bg, u_angle, u_incl, u_roll, u_blast, u_birth, u_aim;
    GLint u_lens, u_bg_rect, u_form;
    bool tried;
    bool ok;
    struct wlr_renderer *owner;
} prog;

bool star_gl_supported(struct wlr_renderer *renderer) {
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
        wlr_log(WLR_ERROR, "star: shader would not compile: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static bool program_ready(struct wlr_renderer *renderer) {
    if (prog.owner != renderer) {
        /* The old program belonged to a context that is gone; there is nothing
         * to delete safely, only to forget. */
        prog.id = 0;
        prog.tried = false;
        prog.ok = false;
        prog.owner = renderer;
    }
    if (prog.tried) return prog.ok;
    prog.tried = true;

    GLuint vs = compile(GL_VERTEX_SHADER, star_vert_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, star_frag_src);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    GLuint id = glCreateProgram();
    glAttachShader(id, vs);
    glAttachShader(id, fs);
    glBindAttribLocation(id, 0, "a_pos");
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
        wlr_log(WLR_ERROR, "star: shader would not link: %s", log);
        glDeleteProgram(id);
        return false;
    }

    prog.id       = id;
    prog.attr_pos = 0;
    prog.u_res    = glGetUniformLocation(id, "u_res");
    prog.u_time   = glGetUniformLocation(id, "u_time");
    prog.u_radius = glGetUniformLocation(id, "u_radius");
    prog.u_color  = glGetUniformLocation(id, "u_color");
    prog.u_lum    = glGetUniformLocation(id, "u_lum");
    prog.u_phase  = glGetUniformLocation(id, "u_phase");
    prog.u_beam   = glGetUniformLocation(id, "u_beam");
    prog.u_bg     = glGetUniformLocation(id, "u_bg");
    prog.u_has_bg = glGetUniformLocation(id, "u_has_bg");
    prog.u_angle  = glGetUniformLocation(id, "u_angle");
    prog.u_incl   = glGetUniformLocation(id, "u_incl");
    prog.u_roll   = glGetUniformLocation(id, "u_roll");
    prog.u_blast  = glGetUniformLocation(id, "u_blast");
    prog.u_birth  = glGetUniformLocation(id, "u_birth");
    prog.u_aim    = glGetUniformLocation(id, "u_aim");
    prog.u_lens   = glGetUniformLocation(id, "u_lens");
    prog.u_form   = glGetUniformLocation(id, "u_form");
    prog.u_bg_rect = glGetUniformLocation(id, "u_bg_rect");
    prog.ok = true;
    wlr_log(WLR_INFO, "star: shader ready");
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

bool star_gl_render(struct wlr_renderer *renderer, struct wlr_buffer *dst,
                    const StarGlParams *p) {
    if (!star_gl_supported(renderer) || !dst || !p) return false;

    struct egl_save save;
    if (!egl_enter(renderer, &save)) return false;

    bool ok = false;
    if (!program_ready(renderer)) goto out;

    GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(renderer, dst);
    if (!fbo) {
        wlr_log(WLR_ERROR, "star: no FBO for the destination buffer");
        goto out;
    }

    int w = dst->width, h = dst->height;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    /* The shader writes premultiplied pixels and the target was just cleared,
     * so this is a copy, not a composite — the same bargain rotate.c makes. */
    glDisable(GL_BLEND);

    glUseProgram(prog.id);
    glUniform2f(prog.u_res, (GLfloat)w, (GLfloat)h);
    glUniform1f(prog.u_time, (GLfloat)p->time_s);
    glUniform1f(prog.u_radius, (GLfloat)p->radius_px);
    glUniform3f(prog.u_color, p->color[0], p->color[1], p->color[2]);
    glUniform1f(prog.u_lum, (GLfloat)p->lum);
    glUniform1f(prog.u_phase, (GLfloat)p->phase);
    /* Bearings are clockwise from up in fwm and anticlockwise from +x in the
     * shader's atan, which is the same turn the pulsar's beam already makes in
     * star.c — converted here so the shader can stay in radians. */
    glUniform1f(prog.u_beam, (GLfloat)((p->beam_deg - 90.0) * M_PI / 180.0));
    glUniform1f(prog.u_angle, (GLfloat)p->angle);
    /* Negative means "no opinion"; ZERO is a real value and means edge-on.
     * Treating zero as unset — which is what this did — made the one position
     * the tilt control exists to reach the one position it could never reach,
     * silently substituting a wide-open disc for it. */
    /* 0.30 rather than dead edge-on. A disc with no thickness seen exactly in
     * its own plane is a mathematical line, and that is what it drew: a
     * hairline across the desktop, sharper than anything else in the picture
     * and unmistakably a line rather than an object. Tipped a little, the same
     * disc has a face as well as an edge, and the edge becomes a band. */
    glUniform1f(prog.u_incl, (GLfloat)(p->disc_tilt >= 0.0 ? p->disc_tilt : 0.30));
    glUniform1f(prog.u_roll, (GLfloat)p->disc_roll);
    glUniform1f(prog.u_blast, (GLfloat)p->blast);
    glUniform1f(prog.u_birth, (GLfloat)(p->birth > 0.0 ? p->birth : 1.0));
    glUniform1f(prog.u_aim, (GLfloat)p->beam_aim);
    glUniform1f(prog.u_lens, (GLfloat)p->lens);
    /* Zero is a real value here: it is the instant a hole appears, with
     * nothing around it yet. Negative is the way to say nothing about it, the
     * same convention disc_tilt uses above. */
    glUniform1f(prog.u_form, (GLfloat)(p->form >= 0.0 ? p->form : 1.0));

    /* The desktop, for a hole to bend. Only 2D textures: a client buffer that
     * arrived as an external image would need its own sampler type, and the
     * snapshot we take is never one — it is a buffer we allocated. */
    bool have_bg = false;
    /* A texture handed to us directly, already in this context: the ring. */
    if (p->background_gl) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)p->background_gl);
        glUniform1i(prog.u_bg, 0);
        have_bg = true;
    }
    if (!have_bg && p->background && wlr_texture_is_gles2(p->background)) {
        struct wlr_gles2_texture_attribs at = {0};
        wlr_gles2_texture_get_attribs(p->background, &at);
        if (at.target == GL_TEXTURE_2D) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, at.tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glUniform1i(prog.u_bg, 0);
            have_bg = true;
        }
    }
    glUniform1f(prog.u_has_bg, have_bg ? 1.0f : 0.0f);
    /* Where the canvas lands inside the background. A photograph was taken of
     * exactly this canvas, so it is the identity; the ring is a whole screen
     * with the star somewhere on it, so it is not. */
    bool rect = p->bg_rect[2] != 0.0f && p->bg_rect[3] != 0.0f;
    glUniform4f(prog.u_bg_rect,
                rect ? p->bg_rect[0] : 0.0f, rect ? p->bg_rect[1] : 0.0f,
                rect ? p->bg_rect[2] : 1.0f, rect ? p->bg_rect[3] : 1.0f);

    static const GLfloat quad[8] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,  1.0f, 1.0f };
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(prog.attr_pos, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(prog.attr_pos);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(prog.attr_pos);

    if (have_bg) glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ok = true;

out:
    egl_leave(renderer, &save);
    return ok;
}

void star_gl_finish(void) {
    /* Nothing is deleted: by the time this is called the context the program
     * lived in is already gone, and deleting a name in a foreign context is
     * worse than leaking one at shutdown. Forgetting is the whole job. */
    prog.id = 0;
    prog.tried = false;
    prog.ok = false;
    prog.owner = NULL;
}
