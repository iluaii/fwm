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

#include "icons.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Premultiplied ARGB32 cairo surface from a pixbuf scaled to fit. */
cairo_surface_t *icon_surface_from_pixbuf(GdkPixbuf *pb) {
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int nch = gdk_pixbuf_get_n_channels(pb);
    int sstride = gdk_pixbuf_get_rowstride(pb);
    int has_alpha = gdk_pixbuf_get_has_alpha(pb);
    const guchar *src = gdk_pixbuf_get_pixels(pb);

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    unsigned char *dst = cairo_image_surface_get_data(surf);
    int dstride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < h; y++) {
        const guchar *s = src + (size_t)y * sstride;
        uint32_t *d = (uint32_t *)(dst + (size_t)y * dstride);
        for (int x = 0; x < w; x++) {
            uint32_t r = s[0], g = s[1], b = s[2];
            uint32_t a = has_alpha ? s[3] : 255;
            if (a != 255) { r = r * a / 255; g = g * a / 255; b = b * a / 255; }
            d[x] = (a << 24) | (r << 16) | (g << 8) | b;
            s += nch;
        }
    }
    cairo_surface_mark_dirty(surf);
    return surf;
}

/* Pragmatic subset of the XDG icon lookup: <base>/<theme>/<size>/apps/<name>.<ext>
 * across the usual base dirs and sizes, then /usr/share/pixmaps. No index.theme
 * parsing or inheritance — hicolor is always tried as the last theme. */
static int icon_locate(const char *name, const char *cfg_theme, char *out, size_t out_sz) {
    if (name[0] == '/') {
        if (access(name, R_OK) == 0) { snprintf(out, out_sz, "%s", name); return 1; }
        return 0;
    }

    /* themes to try, most specific first */
    const char *themes[3];
    int theme_count = 0;
    if (cfg_theme && cfg_theme[0]) themes[theme_count++] = cfg_theme;
    static char gtk_theme[64];
    if (!gtk_theme[0]) {
        const char *home = getenv("HOME");
        char path[512];
        if (home) {
            snprintf(path, sizeof(path), "%s/.config/gtk-3.0/settings.ini", home);
            FILE *f = fopen(path, "r");
            if (f) {
                char line[256];
                while (fgets(line, sizeof(line), f)) {
                    if (strncmp(line, "gtk-icon-theme-name=", 20) == 0) {
                        line[strcspn(line, "\r\n")] = '\0';
                        snprintf(gtk_theme, sizeof(gtk_theme), "%s", line + 20);
                        break;
                    }
                }
                fclose(f);
            }
        }
        if (!gtk_theme[0]) snprintf(gtk_theme, sizeof(gtk_theme), "-");
    }
    if (gtk_theme[0] != '-') themes[theme_count++] = gtk_theme;
    themes[theme_count++] = "hicolor";

    const char *home = getenv("HOME");
    char user_icons[512] = "", user_local[512] = "";
    if (home) {
        snprintf(user_icons, sizeof(user_icons), "%s/.icons", home);
        snprintf(user_local, sizeof(user_local), "%s/.local/share/icons", home);
    }
    const char *bases[] = { user_icons, user_local, "/usr/share/icons" };
    static const char *sizes[] = { "48x48", "64x64", "32x32", "128x128", "256x256", "scalable" };
    static const char *exts[] = { "png", "svg" };

    for (int t = 0; t < theme_count; t++) {
        for (size_t b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
            if (!bases[b][0]) continue;
            for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
                for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
                    snprintf(out, out_sz, "%s/%s/%s/apps/%s.%s",
                             bases[b], themes[t], sizes[s], name, exts[e]);
                    if (access(out, R_OK) == 0) return 1;
                }
            }
        }
    }
    for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
        snprintf(out, out_sz, "/usr/share/pixmaps/%s.%s", name, exts[e]);
        if (access(out, R_OK) == 0) return 1;
    }
    return 0;
}

cairo_surface_t *icon_load(const char *name, const char *cfg_theme, int size) {
    if (!name || !name[0]) return NULL;

    char path[1024];
    if (!icon_locate(name, cfg_theme, path, sizeof(path))) return NULL;

    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size(path, size, size, NULL);
    if (!pb) return NULL;
    cairo_surface_t *surf = icon_surface_from_pixbuf(pb);
    g_object_unref(pb);
    return surf;
}
