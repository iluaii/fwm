#include "launcher.h"
#include "../theme.h"
#include "cairo_overlay.h"
#include "../server.h"

#include <box2d/box2d.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include <strings.h>
#include <sys/stat.h>

/* Search-bar launcher in the tray's "sharp islands" style: a chevron-ended
 * bar in the upper third; result tiles are Box2D bodies, each pulled to its
 * rank's slot by an under-damped spring. New results fly in from below with
 * an overshoot; results that survive a query change glide to their new rank
 * instead of re-flying, so typing doesn't restart the whole animation. */

#define BAR_H       44.0
#define BAR_GAP     8.0   /* bar bottom -> first tile */
#define TILE_H      36.0
#define TILE_GAP    4.0   /* visual gap: body is TILE_H+TILE_GAP tall, drawn TILE_H */
#define MAX_SHOW    8
#define SPAWN_DROP  220.0 /* how far below its slot a new tile spawns */
#define SPAWN_STEP  30.0  /* extra per rank: staggers arrivals */
#define SPRING_W    14.0  /* spring frequency toward the slot, rad/s */
#define SPRING_Z    0.5   /* damping ratio: < 1 = springy overshoot */
#define MAX_APPS    2048

#define QUERY_MAX   120

/* Palette follows the live theme (src/theme.h). */

#define ICON_SZ 24
#define THUMB_W 50.0   /* wallpaper preview width inside a tile (16:9-ish) */
#define THUMB_H 28.0

/* One row in the list. The launcher has two modes and both use this struct:
 * in LMODE_APPS it is a .desktop entry; in LMODE_WALLPAPERS `exec` holds the
 * image path, `icon`/`terminal` are unused and `icon_surf` is the thumbnail. */
typedef struct {
    char name[128];
    char exec[512];
    char icon[128];             /* Icon= value: theme name or absolute path */
    int  terminal;
    cairo_surface_t *icon_surf; /* lazily resolved+loaded; NULL if none */
    int  icon_tried;
} LApp;

enum { LMODE_APPS = 0, LMODE_WALLPAPERS = 1 };

struct Launcher {
    struct FwmServer *server;
    bool open;
    bool scanned;
    bool dirty; /* state changed since the last drawn frame */

    int   mode;      /* LMODE_* — which list is showing */

    LApp *apps;
    int   app_count;
    LApp *walls;      /* wallpaper files, scanned from config wallpaper_dir */
    int   wall_count;
    bool  wall_scanned;

    char query[QUERY_MAX + 8];
    int *match;      /* indices into apps, ranked */
    int  match_count;
    int  sel;        /* selection among the first MAX_SHOW matches */

    struct wlr_scene_buffer *overlay;
    /* Panel that is fading out. It is no longer ours to draw into — the
     * animation owns and destroys it — we only keep the pointer so a reopen
     * can cut it short instead of leaving a ghost under the new panel. */
    struct wlr_scene_buffer *closing;
    int panel_w, panel_h;
    int bar_w, tile_w;
    int px, py;

    bool      world_ok;
    b2WorldId world;
    b2BodyId  tiles[MAX_SHOW];
    int       tile_app[MAX_SHOW]; /* app index each tile body represents */
    int       tile_count;
};

static inline float px2m(double px) { return (float)(px / 100.0); }
static inline double m2px(float m)  { return (double)m * 100.0; }

/* The list currently on screen. Everything below (filtering, ranking, tiles,
 * drawing) works through these two so both modes share one code path. */
static LApp *items(Launcher *l)     { return l->mode == LMODE_WALLPAPERS ? l->walls : l->apps; }
static int   item_count(Launcher *l) { return l->mode == LMODE_WALLPAPERS ? l->wall_count : l->app_count; }

/* ── desktop entry scan ──────────────────────────────────────────────── */

static void strip_field_codes(char *exec) {
    char *src = exec, *dst = exec;
    while (*src) {
        if (src[0] == '%' && src[1]) { src += 2; continue; }
        *dst++ = *src++;
    }
    *dst = '\0';
    /* trim trailing spaces */
    while (dst > exec && dst[-1] == ' ') *--dst = '\0';
}

static void scan_desktop_file(Launcher *l, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    LApp app = {0};
    int in_entry = 0, hidden = 0, is_app = 1;
    char line[600];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '[') {
            if (in_entry) break; /* keys of later groups don't apply */
            in_entry = strncmp(line, "[Desktop Entry]", 15) == 0;
            continue;
        }
        if (!in_entry) continue;
        line[strcspn(line, "\r\n")] = '\0';
        if (!app.name[0] && strncmp(line, "Name=", 5) == 0) {
            snprintf(app.name, sizeof(app.name), "%s", line + 5);
        } else if (!app.exec[0] && strncmp(line, "Exec=", 5) == 0) {
            snprintf(app.exec, sizeof(app.exec), "%s", line + 5);
        } else if (!app.icon[0] && strncmp(line, "Icon=", 5) == 0) {
            snprintf(app.icon, sizeof(app.icon), "%s", line + 5);
        } else if (strncmp(line, "Terminal=", 9) == 0) {
            app.terminal = strcmp(line + 9, "true") == 0;
        } else if (strncmp(line, "NoDisplay=", 10) == 0) {
            hidden |= strcmp(line + 10, "true") == 0;
        } else if (strncmp(line, "Hidden=", 7) == 0) {
            hidden |= strcmp(line + 7, "true") == 0;
        } else if (strncmp(line, "Type=", 5) == 0) {
            is_app = strcmp(line + 5, "Application") == 0;
        }
    }
    fclose(f);

    if (hidden || !is_app || !app.name[0] || !app.exec[0]) return;
    if (l->app_count >= MAX_APPS) return;
    strip_field_codes(app.exec);
    l->apps[l->app_count++] = app;
}

static void scan_dir(Launcher *l, const char *dir,
                     char ***seen, int *seen_count) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 9 || strcmp(e->d_name + n - 8, ".desktop") != 0) continue;
        /* XDG: the first data dir (user first) that provides an id wins */
        int dup = 0;
        for (int i = 0; i < *seen_count; i++) {
            if (strcmp((*seen)[i], e->d_name) == 0) { dup = 1; break; }
        }
        if (dup) continue;
        *seen = realloc(*seen, sizeof(char *) * (*seen_count + 1));
        (*seen)[(*seen_count)++] = strdup(e->d_name);

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        scan_desktop_file(l, path);
    }
    closedir(d);
}

static void scan_apps(Launcher *l) {
    l->apps = calloc(MAX_APPS, sizeof(LApp));
    l->app_count = 0;

    char **seen = NULL;
    int seen_count = 0;
    char dir[1024];

    const char *home = getenv("HOME");
    const char *data_home = getenv("XDG_DATA_HOME");
    if (data_home && data_home[0]) {
        snprintf(dir, sizeof(dir), "%s/applications", data_home);
        scan_dir(l, dir, &seen, &seen_count);
    } else if (home) {
        snprintf(dir, sizeof(dir), "%s/.local/share/applications", home);
        scan_dir(l, dir, &seen, &seen_count);
    }

    const char *data_dirs = getenv("XDG_DATA_DIRS");
    if (!data_dirs || !data_dirs[0]) data_dirs = "/usr/local/share:/usr/share";
    char dirs[2048];
    snprintf(dirs, sizeof(dirs), "%s", data_dirs);
    for (char *save = NULL, *tok = strtok_r(dirs, ":", &save); tok;
         tok = strtok_r(NULL, ":", &save)) {
        snprintf(dir, sizeof(dir), "%s/applications", tok);
        scan_dir(l, dir, &seen, &seen_count);
    }

    for (int i = 0; i < seen_count; i++) free(seen[i]);
    free(seen);
    l->scanned = true;
}

/* ── icons ───────────────────────────────────────────────────────────── */

/* Premultiplied ARGB32 cairo surface from a pixbuf scaled to fit ICON_SZ. */
static cairo_surface_t *icon_surface_from_pixbuf(GdkPixbuf *pb) {
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

static void ensure_icon(Launcher *l, LApp *app) {
    if (app->icon_tried) return;
    app->icon_tried = 1;
    if (!app->icon[0]) return;

    char path[1024];
    if (!icon_locate(app->icon, l->server->config.decor.icon_theme, path, sizeof(path))) {
        return;
    }
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size(path, ICON_SZ, ICON_SZ, NULL);
    if (!pb) return;
    app->icon_surf = icon_surface_from_pixbuf(pb);
    g_object_unref(pb);
}

/* Thumbnail for a wallpaper row. gdk-pixbuf scales while decoding, so a 4K
 * image costs a decode and a 50x28 surface, not a full-size copy. */
static void ensure_thumb(Launcher *l, LApp *item) {
    (void)l;
    if (item->icon_tried) return;
    item->icon_tried = 1;

    /* Aspect preserved: the preview is meant to tell wallpapers apart, and a
     * squashed portrait image defeats that. It is centred in the THUMB box by
     * the draw code, so rows stay aligned whatever the shape. */
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(item->exec, (int)THUMB_W, (int)THUMB_H,
                                                      TRUE, NULL);
    if (!pb) return;
    item->icon_surf = icon_surface_from_pixbuf(pb);
    g_object_unref(pb);
}

static int has_image_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    static const char *ext[] = { ".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif", NULL };
    for (int i = 0; ext[i]; i++)
        if (strcasecmp(dot, ext[i]) == 0) return 1;
    return 0;
}

static int wall_name_cmp(const void *va, const void *vb) {
    return strcasecmp(((const LApp *)va)->name, ((const LApp *)vb)->name);
}

/* One flat scan of the configured directory — no recursion: a wallpaper folder
 * is a wallpaper folder, and descending into it risks walking a whole Pictures
 * tree on every open. */
static void scan_wallpapers(Launcher *l) {
    /* Thumbnails are expensive (a full image decode each), so carry the ones
     * already decoded over to the new listing instead of freeing and redoing
     * them — reopening the picker is then instant. */
    LApp *old = l->walls;
    int old_count = l->wall_count;

    l->wall_scanned = true;
    l->wall_count = 0;
    l->walls = calloc(MAX_APPS, sizeof(LApp));
    if (!l->walls) { l->walls = old; l->wall_count = old_count; return; }

    const char *dir = l->server->config.wallpaper_dir;
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) && l->wall_count < MAX_APPS) {
        if (e->d_name[0] == '.') continue;
        if (!has_image_ext(e->d_name)) continue;

        LApp *it = &l->walls[l->wall_count];
        memset(it, 0, sizeof(*it));
        snprintf(it->exec, sizeof(it->exec), "%s/%s", dir, e->d_name);

        struct stat st;
        if (stat(it->exec, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        /* Show the bare filename: extensions add nothing when every row is an
         * image, and they cost width in a narrow tile. Strip the extension off
         * the ORIGINAL name — copying first and cutting at the last dot
         * afterwards loses the extension to truncation on long names, and then
         * cuts at whatever dot is left ("Konachan.com - 12345 ..." collapsed to
         * "Konachan" for every file). */
        const char *base = e->d_name;
        const char *ext = strrchr(base, '.');
        size_t base_len = ext ? (size_t)(ext - base) : strlen(base);
        if (base_len >= sizeof(it->name)) base_len = sizeof(it->name) - 1;
        memcpy(it->name, base, base_len);
        it->name[base_len] = '\0';

        /* Reuse a previously decoded thumbnail for the same file. */
        for (int k = 0; k < old_count; k++) {
            if (strcmp(old[k].exec, it->exec) != 0) continue;
            it->icon_surf = old[k].icon_surf;
            it->icon_tried = old[k].icon_tried;
            old[k].icon_surf = NULL;   /* ownership moved */
            break;
        }
        l->wall_count++;
    }
    closedir(d);

    /* Whatever was not carried over belonged to files that are gone. */
    for (int k = 0; k < old_count; k++) {
        if (old[k].icon_surf) cairo_surface_destroy(old[k].icon_surf);
    }
    free(old);

    qsort(l->walls, l->wall_count, sizeof(LApp), wall_name_cmp);
}

/* ── filtering ───────────────────────────────────────────────────────── */

/* Case-insensitive substring; returns match offset or -1. */
static int ci_find(const char *hay, const char *needle) {
    if (!needle[0]) return 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, nl) == 0) return (int)(p - hay);
    }
    return -1;
}

static LApp *g_sort_apps;
static const char *g_sort_query;

static int match_rank(const LApp *a, const char *q, int *pos) {
    int p = ci_find(a->name, q);
    *pos = p;
    if (p < 0) return -1;
    if (p == 0) return 0;                                    /* prefix */
    char prev = a->name[p - 1];
    if (prev == ' ' || prev == '-' || prev == '_') return 1; /* word start */
    return 2;                                                /* substring */
}

static int match_cmp(const void *va, const void *vb) {
    int ia = *(const int *)va, ib = *(const int *)vb;
    int pa, pb;
    int ra = match_rank(&g_sort_apps[ia], g_sort_query, &pa);
    int rb = match_rank(&g_sort_apps[ib], g_sort_query, &pb);
    if (ra != rb) return ra - rb;
    if (pa != pb) return pa - pb;
    return strcasecmp(g_sort_apps[ia].name, g_sort_apps[ib].name);
}

static void refilter(Launcher *l) {
    l->match_count = 0;
    for (int i = 0; i < item_count(l); i++) {
        int pos;
        if (match_rank(&items(l)[i], l->query, &pos) >= 0) {
            l->match[l->match_count++] = i;
        }
    }
    g_sort_apps = items(l);
    g_sort_query = l->query;
    qsort(l->match, l->match_count, sizeof(int), match_cmp);
    l->sel = 0;
}

/* ── tile physics ────────────────────────────────────────────────────── */

static double tile_cx(Launcher *l) { return l->bar_w / 2.0; }

static double slot_cy(int i) {
    /* rest position of tile i's body center (body is TILE_H+TILE_GAP tall) */
    return BAR_H + BAR_GAP + (i + 0.5) * (TILE_H + TILE_GAP);
}

static b2BodyId tile_body_create(Launcher *l, int rank) {
    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_dynamicBody;
    bd.fixedRotation = true;
    bd.position = (b2Vec2){px2m(tile_cx(l)),
                           px2m(slot_cy(rank) + SPAWN_DROP + rank * SPAWN_STEP)};
    b2BodyId body = b2CreateBody(l->world, &bd);

    b2ShapeDef sd = b2DefaultShapeDef();
    sd.density = 1.0f;
    /* Tiles are driven by per-tile springs and may pass each other while
     * reordering — no tile-tile collisions. */
    sd.filter.categoryBits = 0;
    sd.filter.maskBits = 0;
    b2Polygon box = b2MakeBox(px2m(l->tile_w / 2.0),
                              px2m((TILE_H + TILE_GAP) / 2.0));
    b2CreatePolygonShape(body, &sd, &box);
    return body;
}

/* Diff the shown matches against the current tiles: surviving apps keep their
 * body (the slot spring glides it to its new rank), departed bodies die, and
 * only genuinely new results fly in from below. */
static void tiles_rebuild(Launcher *l) {
    int shown = l->match_count < MAX_SHOW ? l->match_count : MAX_SHOW;
    b2BodyId new_tiles[MAX_SHOW];
    int new_app[MAX_SHOW];
    bool reused[MAX_SHOW] = {0};

    for (int i = 0; i < shown; i++) {
        int app = l->match[i];
        new_app[i] = app;
        new_tiles[i] = b2_nullBodyId;
        for (int j = 0; j < l->tile_count; j++) {
            if (!reused[j] && l->tile_app[j] == app) {
                new_tiles[i] = l->tiles[j];
                reused[j] = true;
                break;
            }
        }
        if (B2_IS_NULL(new_tiles[i])) {
            new_tiles[i] = tile_body_create(l, i);
        }
    }
    for (int j = 0; j < l->tile_count; j++) {
        if (!reused[j] && B2_IS_NON_NULL(l->tiles[j])) b2DestroyBody(l->tiles[j]);
    }
    memcpy(l->tiles, new_tiles, sizeof(b2BodyId) * shown);
    memcpy(l->tile_app, new_app, sizeof(int) * shown);
    l->tile_count = shown;
}

static void world_create(Launcher *l) {
    b2WorldDef wd = b2DefaultWorldDef();
    wd.gravity = (b2Vec2){0.0f, 0.0f}; /* motion comes from slot springs */
    l->world = b2CreateWorld(&wd);
    l->world_ok = true;
}

/* Damped spring pulling each tile to its rank's slot; SPRING_Z < 1 gives the
 * springy overshoot when a tile arrives or changes rank. */
static void tiles_apply_springs(Launcher *l) {
    for (int i = 0; i < l->tile_count; i++) {
        b2BodyId body = l->tiles[i];
        float mass = b2Body_GetMass(body);
        b2Vec2 pos = b2Body_GetPosition(body);
        b2Vec2 vel = b2Body_GetLinearVelocity(body);
        float dy = px2m(slot_cy(i)) - pos.y;
        float ay = (float)(SPRING_W * SPRING_W) * dy
                 - 2.0f * (float)(SPRING_Z * SPRING_W) * vel.y;
        b2Body_ApplyForceToCenter(body, (b2Vec2){0.0f, mass * ay}, true);
    }
}

/* ── drawing ─────────────────────────────────────────────────────────── */

/* Chevron-ended island, same silhouette as the tray pills. */
static void pill_path(cairo_t *cr, double x, double y, double w, double h) {
    double cut = h / 2.0;
    cairo_new_path(cr);
    cairo_move_to(cr, x + cut, y);
    cairo_line_to(cr, x + w - cut, y);
    cairo_line_to(cr, x + w, y + h / 2.0);
    cairo_line_to(cr, x + w - cut, y + h);
    cairo_line_to(cr, x + cut, y + h);
    cairo_line_to(cr, x, y + h / 2.0);
    cairo_close_path(cr);
}

static void draw_launcher(cairo_t *cr, int w, int h, void *data) {
    Launcher *l = data;
    (void)w; (void)h;

    const FwmTheme *thm = theme_get();

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("sans 11");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    double alpha = l->server->config.decor.launcher_opacity;

    /* ── bar ── */
    pill_path(cr, 0, 0, l->bar_w, BAR_H);
    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], alpha);
    cairo_fill(cr);

    /* magnifier glyph */
    double gx = BAR_H / 2.0 + 4.0, gy = BAR_H / 2.0 - 2.0;
    cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
    cairo_set_line_width(cr, 1.8);
    cairo_new_path(cr);
    cairo_arc(cr, gx, gy, 6.0, 0, 6.2832);
    cairo_stroke(cr);
    cairo_move_to(cr, gx + 4.4, gy + 4.4);
    cairo_line_to(cr, gx + 9.0, gy + 9.0);
    cairo_stroke(cr);

    /* query / placeholder + caret */
    double tx = BAR_H / 2.0 + 22.0;
    int tw, th;
    if (l->query[0]) {
        pango_layout_set_text(layout, l->query, -1);
        cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
    } else {
        pango_layout_set_text(layout,
            l->mode == LMODE_WALLPAPERS ? "Choose a wallpaper…" : "Search or run…", -1);
        cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
    }
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_move_to(cr, tx, (BAR_H - th) / 2.0);
    pango_cairo_show_layout(cr, layout);

    double caret_x = l->query[0] ? tx + tw + 3.0 : tx;
    cairo_set_source_rgba(cr, thm->text[0], thm->text[1], thm->text[2], 0.8);
    cairo_rectangle(cr, caret_x, (BAR_H - th) / 2.0, 1.5, th);
    cairo_fill(cr);

    /* match count, right-aligned before the chevron point */
    if (l->query[0]) {
        char cnt[16];
        snprintf(cnt, sizeof(cnt), "%d", l->match_count);
        pango_layout_set_text(layout, cnt, -1);
        pango_layout_get_pixel_size(layout, &tw, &th);
        cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
        cairo_move_to(cr, l->bar_w - BAR_H / 2.0 - tw - 4.0, (BAR_H - th) / 2.0);
        pango_cairo_show_layout(cr, layout);
    }

    /* ── tiles ── */
    for (int i = 0; i < l->tile_count; i++) {
        b2Vec2 c = b2Body_GetPosition(l->tiles[i]);
        double cx = m2px(c.x), cy = m2px(c.y);
        double x = cx - l->tile_w / 2.0;
        double y = cy - TILE_H / 2.0;
        if (y > l->panel_h) continue; /* still flying in from below */

        int selected = (i == l->sel);
        pill_path(cr, x, y, l->tile_w, TILE_H);
        if (selected) {
            double sa = alpha + 0.04 > 1.0 ? 1.0 : alpha + 0.04;
            cairo_set_source_rgba(cr, thm->sel[0], thm->sel[1], thm->sel[2], sa);
        } else {
            cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], alpha);
        }
        cairo_fill(cr);

        LApp *app = &items(l)[l->match[i]];
        double text_x = x + TILE_H / 2.0 + 4.0;
        double art_w = ICON_SZ;
        if (l->mode == LMODE_WALLPAPERS) {
            /* No decoding here — a wallpaper thumbnail costs 15-200ms and the
             * whole panel would freeze on its first frame. launcher_tick loads
             * them one per frame; until then the slot shows a placeholder. */
            art_w = THUMB_W;
            if (!app->icon_surf) {
                /* dim, not sel: sel is the selected row's own fill and the
                 * placeholder would vanish on exactly that row. */
                cairo_set_source_rgba(cr, thm->dim[0], thm->dim[1], thm->dim[2],
                                      alpha * 0.5);
                cairo_rectangle(cr, text_x, y + (TILE_H - THUMB_H) / 2.0,
                                THUMB_W, THUMB_H);
                cairo_fill(cr);
            }
        } else {
            ensure_icon(l, app);
        }
        if (app->icon_surf) {
            int iw = cairo_image_surface_get_width(app->icon_surf);
            int ih = cairo_image_surface_get_height(app->icon_surf);
            cairo_save(cr);
            cairo_set_source_surface(cr, app->icon_surf,
                                     text_x + (art_w - iw) / 2.0,
                                     y + (TILE_H - ih) / 2.0);
            cairo_paint(cr);
            cairo_restore(cr);
        }
        text_x += art_w + 10.0; /* fixed text column, art or not */

        /* Names can be far wider than a tile (booru filenames carry their
         * whole tag list), so ellipsize to the space left after the art. */
        pango_layout_set_width(layout, (int)((x + l->tile_w - 12.0 - text_x) * PANGO_SCALE));
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_text(layout, app->name, -1);
        pango_layout_get_pixel_size(layout, &tw, &th);
        if (selected) {
            cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
        } else {
            cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
        }
        cairo_move_to(cr, text_x, y + (TILE_H - th) / 2.0);
        pango_cairo_show_layout(cr, layout);
    }

    /* Undo the per-tile ellipsis before anything else uses this layout. */
    pango_layout_set_width(layout, -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);

    if (l->query[0] && l->match_count == 0) {
        pango_layout_set_text(layout, "nothing found", -1);
        pango_layout_get_pixel_size(layout, &tw, &th);
        cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
        cairo_move_to(cr, (l->bar_w - tw) / 2.0, BAR_H + BAR_GAP + 10.0);
        pango_cairo_show_layout(cr, layout);
    }

    g_object_unref(layout);
}

/* ── open / close ────────────────────────────────────────────────────── */

/* Appear/disappear of the panel itself. The tiles have always had their spring
 * fly-in, but the panel used to pop in and vanish instantly. */
#define PANEL_ANIM_MS 170.0
#define PANEL_RISE_PX 12.0

static void closing_done(void *data) {
    Launcher *l = data;
    l->closing = NULL;
}

/* Drop the outgoing panel this instant (reopen, or teardown: the callback must
 * not outlive the Launcher). */
static void closing_cancel(Launcher *l) {
    if (!l->closing) return;
    struct wlr_scene_buffer *buf = l->closing;
    l->closing = NULL;
    cairo_overlay_destroy(buf);
}

/* `animate` false tears the panel down synchronously — required from
 * launcher_destroy, where a pending callback would reference freed memory. */
static void launcher_close_ex(Launcher *l, bool animate) {
    if (!l->open) return;
    /* Input state goes back to the client NOW; only the pixels linger, so the
     * keyboard grab and pointer routing never wait on the animation. */
    l->open = false;
    if (l->overlay) {
        closing_cancel(l); /* at most one outgoing panel at a time */
        if (animate) {
            l->closing = l->overlay;
            cairo_overlay_animate_out(l->overlay, PANEL_ANIM_MS, PANEL_RISE_PX,
                                      closing_done, l);
        } else {
            cairo_overlay_destroy(l->overlay);
        }
        l->overlay = NULL;
    }
    if (l->world_ok) {
        /* Safe to drop while the panel is still fading: the last frame the
         * tiles drew is already in the buffer, and the fade only touches the
         * scene node. */
        b2DestroyWorld(l->world); /* frees tiles and walls */
        l->world_ok = false;
        l->tile_count = 0;
    }
}

static void launcher_close(Launcher *l) {
    launcher_close_ex(l, true);
}

static void launcher_open(Launcher *l) {
    if (l->open) return;
    FwmServer *server = l->server;
    /* A mode switch closes then immediately reopens; without this the old
     * panel would fade out underneath the new one. */
    closing_cancel(l);
    if (l->mode == LMODE_WALLPAPERS) {
        /* Rescanned on every open: the folder's contents are expected to
         * change between openings, unlike the installed .desktop set. */
        scan_wallpapers(l);
    } else if (!l->scanned) {
        scan_apps(l);
    }

    l->bar_w = server->screen_width / 3;
    if (l->bar_w < 420) l->bar_w = 420;
    if (l->bar_w > 640) l->bar_w = 640;
    l->tile_w = l->bar_w - 24;
    l->panel_w = l->bar_w;
    /* Height covers the bar + full stack + a short fly-in strip; new tiles
     * emerge from the bottom edge instead of the buffer holding the whole
     * spawn runway (which cost ~1.5 MB extra per frame buffer). */
    l->panel_h = (int)(BAR_H + BAR_GAP + MAX_SHOW * (TILE_H + TILE_GAP) + 120.0);
    l->px = (server->screen_width - l->panel_w) / 2;
    l->py = server->screen_height / 5;

    l->query[0] = '\0';
    refilter(l);

    world_create(l);
    tiles_rebuild(l);

    l->overlay = cairo_overlay_create(server->layer_overlay, l->panel_w, l->panel_h);
    if (!l->overlay) {
        launcher_close(l);
        return;
    }
    wlr_scene_node_set_position(&l->overlay->node, l->px, l->py);
    cairo_overlay_animate_in(l->overlay, PANEL_ANIM_MS, PANEL_RISE_PX);
    l->open = true;
    l->dirty = true;
}

/* ── public api ──────────────────────────────────────────────────────── */

Launcher *launcher_create(struct FwmServer *server) {
    Launcher *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->server = server;
    /* Shared by both modes, so it cannot live in scan_apps: the wallpaper
     * picker never runs that and refilter() would write through NULL. */
    l->match = calloc(MAX_APPS, sizeof(int));
    if (!l->match) { free(l); return NULL; }
    return l;
}

void launcher_destroy(Launcher *l) {
    if (!l) return;
    launcher_close_ex(l, false);
    closing_cancel(l);
    for (int i = 0; i < l->app_count; i++) {
        if (l->apps[i].icon_surf) cairo_surface_destroy(l->apps[i].icon_surf);
    }
    free(l->apps);
    for (int i = 0; i < l->wall_count; i++) {
        if (l->walls[i].icon_surf) cairo_surface_destroy(l->walls[i].icon_surf);
    }
    free(l->walls);
    free(l->match);
    free(l);
}

void launcher_toggle(Launcher *l) {
    launcher_toggle_mode(l, LMODE_APPS);
}

void launcher_toggle_wallpapers(Launcher *l) {
    launcher_toggle_mode(l, LMODE_WALLPAPERS);
}

/* Toggling while the other mode is open switches to it rather than closing:
 * hitting the wallpaper bind with the app launcher up should show wallpapers,
 * not nothing. */
void launcher_toggle_mode(Launcher *l, int mode) {
    if (!l) return;
    if (l->open && l->mode == mode) {
        launcher_close(l);
        return;
    }
    if (l->open) launcher_close(l);
    l->mode = mode;
    launcher_open(l);
}

bool launcher_is_open(Launcher *l) {
    return l && l->open;
}

static void launch_selected(Launcher *l) {
    if (l->match_count == 0) return;
    int shown = l->match_count < MAX_SHOW ? l->match_count : MAX_SHOW;
    int sel = l->sel < shown ? l->sel : 0;
    const LApp *app = &items(l)[l->match[sel]];

    if (l->mode == LMODE_WALLPAPERS) {
        server_set_wallpaper(l->server, app->exec);
        return;
    }

    char cmd[600];
    if (app->terminal) {
        snprintf(cmd, sizeof(cmd), "kitty -e %s", app->exec);
    } else {
        snprintf(cmd, sizeof(cmd), "%s", app->exec);
    }
    if (fork() == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        exit(1);
    }
}

bool launcher_handle_key(Launcher *l, xkb_keysym_t sym, const char *utf8) {
    if (!l || !l->open) return false;
    int shown = l->match_count < MAX_SHOW ? l->match_count : MAX_SHOW;
    l->dirty = true;

    switch (sym) {
    case XKB_KEY_Escape:
        launcher_close(l);
        return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        launch_selected(l);
        launcher_close(l);
        return true;
    case XKB_KEY_Up:
        if (l->sel > 0) l->sel--;
        return true;
    case XKB_KEY_Down:
    case XKB_KEY_Tab:
        if (shown > 0) l->sel = (l->sel + 1) % shown;
        return true;
    case XKB_KEY_BackSpace: {
        size_t len = strlen(l->query);
        /* strip one UTF-8 codepoint: continuation bytes, then the lead */
        while (len > 0 && (l->query[len - 1] & 0xC0) == 0x80) len--;
        if (len > 0) len--;
        l->query[len] = '\0';
        refilter(l);
        tiles_rebuild(l);
        return true;
    }
    default:
        break;
    }

    if (utf8 && utf8[0] && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f
        && strlen(l->query) + strlen(utf8) <= QUERY_MAX) {
        strcat(l->query, utf8);
        refilter(l);
        tiles_rebuild(l);
        return true;
    }
    return true; /* swallow everything else while open */
}

/* Tile index under panel-local point, or -1. Uses the live body positions so
 * hit-testing matches what is drawn, even mid-flight. */
static int tile_at(Launcher *l, double x, double y) {
    for (int i = 0; i < l->tile_count; i++) {
        b2Vec2 c = b2Body_GetPosition(l->tiles[i]);
        if (fabs(x - m2px(c.x)) <= l->tile_w / 2.0 &&
            fabs(y - m2px(c.y)) <= TILE_H / 2.0) {
            return i;
        }
    }
    return -1;
}

void launcher_handle_motion(Launcher *l, double lx, double ly) {
    if (!l || !l->open) return;
    int hit = tile_at(l, lx - l->px, ly - l->py);
    if (hit >= 0 && hit != l->sel) {
        l->sel = hit;
        l->dirty = true;
    }
}

bool launcher_handle_button(Launcher *l, double lx, double ly, bool pressed) {
    if (!l || !l->open) return false;
    if (!pressed) return true; /* swallow releases too */

    double x = lx - l->px, y = ly - l->py;
    int hit = tile_at(l, x, y);
    if (hit >= 0) {
        l->sel = hit;
        launch_selected(l);
        launcher_close(l);
    } else if (!(x >= 0 && x <= l->bar_w && y >= 0 && y <= BAR_H)) {
        launcher_close(l); /* click-away dismiss; clicks on the bar keep it */
    }
    return true;
}

void launcher_tick(Launcher *l, double dt) {
    if (!l || !l->open) return;

    bool moving = false;
    for (int i = 0; i < l->tile_count && !moving; i++) {
        b2Vec2 p = b2Body_GetPosition(l->tiles[i]);
        b2Vec2 v = b2Body_GetLinearVelocity(l->tiles[i]);
        if (fabs(m2px(p.y) - slot_cy(i)) > 0.5 || fabs(m2px(v.y)) > 2.0) {
            moving = true;
        }
    }

    /* Wallpaper previews are decoded here rather than while drawing, one per
     * frame, and only once the tiles have stopped moving: a single decode runs
     * 15-200ms and would hitch the fly-in badly (the whole picker used to
     * freeze ~600ms on its first frame). So the panel animates smoothly first,
     * then previews fill in. */
    if (!moving && l->mode == LMODE_WALLPAPERS) {
        int shown = l->match_count < MAX_SHOW ? l->match_count : MAX_SHOW;
        for (int i = 0; i < shown; i++) {
            LApp *it = &items(l)[l->match[i]];
            if (it->icon_tried) continue;
            ensure_thumb(l, it);
            l->dirty = true;   /* redraw to show it */
            break;
        }
    }

    /* Fully settled and nothing changed -> skip the step and, above all, the
     * per-tick reallocation + upload of the panel buffer. */
    if (!moving && !l->dirty) return;
    l->dirty = false;

    tiles_apply_springs(l);
    b2World_Step(l->world, (float)dt, 4);
    cairo_overlay_update(l->overlay, draw_launcher, l);
}
