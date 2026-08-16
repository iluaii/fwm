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

#include "radial.h"
#include "../theme.h"
#include "cairo_overlay.h"
#include "icons.h"
#include "../server.h"

#include <box2d/box2d.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* A ring of actions around a hub, built for a knob: turning it walks the ring,
 * pressing it fires the petal the ring stopped on.
 *
 * A petal may hold a ring of its own instead of an action. Pressing it swaps
 * the ring for the deeper one — same panel, same hub, the petals simply bloom
 * out of the middle again — and the hub becomes the way back: it wears the
 * face of the petal that was pressed, and pressing it (or Escape, or
 * Backspace) returns one ring. The path back is a stack, so the nesting is as
 * deep as the config wrote it.
 *
 * The petals are Box2D bodies that bloom out of the hub, each pulled to its
 * angle's slot by the same kind of under-damped spring the launcher's tiles
 * ride — this is a physics compositor, and a menu that simply appeared would
 * be the one panel in it that does not belong to the world.
 *
 * Configured in [radial] and [[radial.item]]; see src/config.h. */

#define PETAL_R     44.0   /* petal circle radius */
#define HUB_R       50.0
#define RING_PAD    14.0   /* margin outside the petals, for the focus swell */
#define ICON_SZ     30
#define HUB_ART     58     /* the hub picture, decoded to fit this box */
#define GROW_RATE   14.0   /* how fast the focused petal swells, per second */
#define GROW_PX     7.0    /* how much bigger it gets */
#define SPRING_W    15.0   /* spring frequency toward the slot, rad/s */
#define SPRING_Z    0.55   /* damping ratio: < 1 = a little overshoot */
#define PANEL_ANIM_MS 150.0
#define PANEL_RISE_PX 10.0

typedef struct {
    b2BodyId        body;
    double          grow;        /* 0..1, eased toward focus */
    cairo_surface_t *art;        /* the item's icon, or NULL */
    int             art_tried;
} Petal;

struct Radial {
    struct FwmServer *server;
    bool open;
    bool dirty;

    /* The menu is a SNAPSHOT of [radial], taken when it opens, not a window
     * onto the live config: `reload_config` is itself a bindable action, so a
     * petal can replace the very tables the menu is drawing from. Copying it
     * once per open buys immunity from that — and the whole menu is copied,
     * not just the ring on screen, because a sub-ring is entered later, long
     * after a reload could have moved the ground. */
    RadialConfig cfg;

    int        menu;       /* the ring on screen: an index into cfg.menus */
    int        count;      /* its items, mirrored from cfg for the loops below */
    int        sel;
    Petal      petals[CONFIG_MAX_RADIAL];

    /* The way back: which ring was left, and which petal was focused in it.
     * One entry per level descended — the config's ring budget is the depth
     * budget too, since every level costs a ring. */
    int   back_menu[CONFIG_MAX_RADIAL_MENUS];
    int   back_sel[CONFIG_MAX_RADIAL_MENUS];
    int   depth;

    char  center[512];
    char  center_text[32];
    cairo_surface_t *hub_art;
    int   hub_tried;

    struct wlr_scene_buffer *overlay;
    /* The panel on its way out. The animation owns and destroys it; the
     * pointer is kept only so a reopen can cut it short instead of leaving a
     * ghost under the new one. */
    struct wlr_scene_buffer *closing;
    int   side;      /* the panel is square */
    int   px, py;
    double cx, cy;   /* hub centre, panel-local */
    double radius;

    bool      world_ok;
    b2WorldId world;
};

static inline float px2m(double px) { return (float)(px / 100.0); }
static inline double m2px(float m)  { return (double)m * 100.0; }

static const RadialConfig *conf(Radial *r) { return &r->server->config.radial; }

/* The item at a place in the ring on screen. Everything below reads the menu
 * through this, never through cfg.menus[0], so descending a level is a change
 * of one integer. */
static const RadialItem *item_at(Radial *r, int i) {
    return &r->cfg.menus[r->menu].items[i];
}

/* Clockwise from the top: the first item written is the one straight up, which
 * is where a hand expects to find it. */
static double slot_angle(Radial *r, int i) {
    return -M_PI / 2.0 + (2.0 * M_PI * i) / (double)r->count;
}

static void slot_pos(Radial *r, int i, double *x, double *y) {
    double a = slot_angle(r, i);
    *x = r->cx + r->radius * cos(a);
    *y = r->cy + r->radius * sin(a);
}

/* ── petal physics ───────────────────────────────────────────────────── */

static b2BodyId petal_body_create(Radial *r) {
    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_dynamicBody;
    bd.fixedRotation = true;
    /* Every petal starts inside the hub and blooms out of it. */
    bd.position = (b2Vec2){ px2m(r->cx), px2m(r->cy) };
    b2BodyId body = b2CreateBody(r->world, &bd);

    b2ShapeDef sd = b2DefaultShapeDef();
    sd.density = 1.0f;
    /* Driven entirely by their own springs, and they pass through each other
     * on the way out — no petal-petal collisions. */
    sd.filter.categoryBits = 0;
    sd.filter.maskBits = 0;
    b2Circle circle = { .center = {0.0f, 0.0f}, .radius = px2m(PETAL_R) };
    b2CreateCircleShape(body, &sd, &circle);
    return body;
}

static void petals_apply_springs(Radial *r) {
    for (int i = 0; i < r->count; i++) {
        b2BodyId body = r->petals[i].body;
        float mass = b2Body_GetMass(body);
        b2Vec2 pos = b2Body_GetPosition(body);
        b2Vec2 vel = b2Body_GetLinearVelocity(body);

        double sx, sy;
        slot_pos(r, i, &sx, &sy);
        float ax = (float)(SPRING_W * SPRING_W) * (px2m(sx) - pos.x)
                 - 2.0f * (float)(SPRING_Z * SPRING_W) * vel.x;
        float ay = (float)(SPRING_W * SPRING_W) * (px2m(sy) - pos.y)
                 - 2.0f * (float)(SPRING_Z * SPRING_W) * vel.y;
        b2Body_ApplyForceToCenter(body, (b2Vec2){ mass * ax, mass * ay }, true);
    }
}

static bool petals_moving(Radial *r) {
    for (int i = 0; i < r->count; i++) {
        b2Vec2 p = b2Body_GetPosition(r->petals[i].body);
        b2Vec2 v = b2Body_GetLinearVelocity(r->petals[i].body);
        double sx, sy;
        slot_pos(r, i, &sx, &sy);
        if (fabs(m2px(p.x) - sx) > 0.5 || fabs(m2px(p.y) - sy) > 0.5) return true;
        if (fabs(m2px(v.x)) > 2.0 || fabs(m2px(v.y)) > 2.0) return true;
        double want = (i == r->sel) ? 1.0 : 0.0;
        if (fabs(r->petals[i].grow - want) > 0.005) return true;
    }
    return false;
}

/* ── drawing ─────────────────────────────────────────────────────────── */

static void ensure_art(Radial *r, int i) {
    Petal *p = &r->petals[i];
    if (p->art_tried) return;
    p->art_tried = 1;
    p->art = icon_load(item_at(r, i)->icon,
                       r->server->config.decor.icon_theme, ICON_SZ);
}

static void ensure_hub_art(Radial *r) {
    if (r->hub_tried) return;
    r->hub_tried = 1;
    r->hub_art = icon_load(r->center,
                           r->server->config.decor.icon_theme, HUB_ART);
}

/* A surface centred on (x, y), clipped to nothing but its own bounds. */
static void paint_centered(cairo_t *cr, cairo_surface_t *surf, double x, double y) {
    int w = cairo_image_surface_get_width(surf);
    int h = cairo_image_surface_get_height(surf);
    cairo_save(cr);
    cairo_set_source_surface(cr, surf, x - w / 2.0, y - h / 2.0);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void show_centered(cairo_t *cr, PangoLayout *layout, const char *text,
                          double x, double y) {
    int tw, th;
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_move_to(cr, x - tw / 2.0, y - th / 2.0);
    pango_cairo_show_layout(cr, layout);
}

static void draw_radial(cairo_t *cr, int w, int h, void *data) {
    Radial *r = data;
    (void)w; (void)h;

    const FwmTheme *thm = theme_get();
    double alpha = r->server->config.decor.launcher_opacity;

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    /* ── hub ── */
    cairo_new_path(cr);
    cairo_arc(cr, r->cx, r->cy, HUB_R, 0, 2.0 * M_PI);
    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], alpha);
    cairo_fill(cr);

    /* Inside a sub-ring the hub is the way back, and says so with a ring in
     * the accent colour: the picture in the middle is the petal that was
     * pressed to get here, and pressing it undoes that. At the root there is
     * nothing to go back to and the hub stays plain. */
    if (r->depth > 0) {
        cairo_new_path(cr);
        cairo_arc(cr, r->cx, r->cy, HUB_R - 1.0, 0, 2.0 * M_PI);
        cairo_set_source_rgba(cr, thm->accent[0], thm->accent[1], thm->accent[2], 0.8);
        cairo_set_line_width(cr, 2.0);
        cairo_stroke(cr);
    }

    ensure_hub_art(r);
    if (r->hub_art) {
        /* Clipped to the hub so an oversized or square picture still reads as
         * the middle of a ring rather than a stamp laid over it. */
        cairo_save(cr);
        cairo_arc(cr, r->cx, r->cy, HUB_R - 4.0, 0, 2.0 * M_PI);
        cairo_clip(cr);
        paint_centered(cr, r->hub_art, r->cx, r->cy);
        cairo_restore(cr);
    } else if (r->center_text[0]) {
        cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
        show_centered(cr, layout, r->center_text, r->cx, r->cy);
    }

    /* ── petals ── */
    /* Everything a petal says is drawn INSIDE it, on its own fill. A name
     * floating on the wallpaper under the circle was the first thing tried and
     * it is unreadable over a busy picture — half the labels vanished into a
     * bright patch, which is a poor way to run a menu you pick from by sight. */
    for (int i = 0; i < r->count; i++) {
        b2Vec2 c = b2Body_GetPosition(r->petals[i].body);
        double x = m2px(c.x), y = m2px(c.y);
        bool focused = (i == r->sel);
        double rad = PETAL_R + GROW_PX * r->petals[i].grow;

        cairo_new_path(cr);
        cairo_arc(cr, x, y, rad, 0, 2.0 * M_PI);
        if (focused) {
            double sa = alpha + 0.04 > 1.0 ? 1.0 : alpha + 0.04;
            cairo_set_source_rgba(cr, thm->sel[0], thm->sel[1], thm->sel[2], sa);
        } else {
            cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], alpha);
        }
        cairo_fill_preserve(cr);
        /* The focus is a ring as well as a fill: on a theme whose sel and pill
         * sit close together the fill alone is easy to miss. */
        if (focused) {
            cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
            cairo_set_line_width(cr, 2.0);
            cairo_stroke(cr);
        } else {
            cairo_new_path(cr);
        }

        const RadialItem *it = item_at(r, i);
        ensure_art(r, i);

        /* A petal that opens a ring of its own wears three dots on its outer
         * edge, pointing the way the ring grows. Without a mark the only way
         * to learn which petals go deeper is to press them, and pressing is
         * how everything else fires. */
        if (it->submenu) {
            double a = slot_angle(r, i);
            double ux = cos(a), uy = sin(a);      /* outward, hub to petal */
            double px = -uy, py = ux;             /* along the edge */
            cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
            for (int d = -1; d <= 1; d++) {
                double dx = x + ux * (rad - 8.0) + px * d * 6.0;
                double dy = y + uy * (rad - 8.0) + py * d * 6.0;
                cairo_new_path(cr);
                cairo_arc(cr, dx, dy, 1.8, 0, 2.0 * M_PI);
                cairo_fill(cr);
            }
        }

        /* With a picture or a glyph the name sits under it; with neither the
         * name has the whole petal to itself and is centred. */
        bool has_art = r->petals[i].art || it->text[0];
        double art_y   = has_art && it->label[0] ? y - 9.0 : y;
        double label_y = has_art ? y + 19.0 : y;

        if (r->petals[i].art) {
            paint_centered(cr, r->petals[i].art, x, art_y);
        } else if (it->text[0]) {
            PangoFontDescription *big = pango_font_description_from_string("sans bold 15");
            pango_layout_set_font_description(layout, big);
            pango_font_description_free(big);
            cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
            show_centered(cr, layout, it->text, x, art_y);
            PangoFontDescription *small = pango_font_description_from_string("sans 10");
            pango_layout_set_font_description(layout, small);
            pango_font_description_free(small);
        }

        if (it->label[0]) {
            /* Ellipsized to the chord of the circle at the line's height, not
             * to its full width: a long name laid across the widest part of a
             * disc still runs out over the edge. */
            pango_layout_set_width(layout, (int)((1.5 * rad) * PANGO_SCALE));
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
            pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
            if (focused) cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
            else         cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
            show_centered(cr, layout, it->label, x, label_y);
            pango_layout_set_width(layout, -1);
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
            pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
        }
    }

    g_object_unref(layout);
}

/* ── open / close ────────────────────────────────────────────────────── */

static void closing_done(void *data) {
    Radial *r = data;
    r->closing = NULL;
}

static void closing_cancel(Radial *r) {
    if (!r->closing) return;
    struct wlr_scene_buffer *buf = r->closing;
    r->closing = NULL;
    cairo_overlay_destroy(buf);
}

static void art_free(Radial *r) {
    for (int i = 0; i < CONFIG_MAX_RADIAL; i++) {
        if (r->petals[i].art) cairo_surface_destroy(r->petals[i].art);
        r->petals[i].art = NULL;
        r->petals[i].art_tried = 0;
    }
    if (r->hub_art) cairo_surface_destroy(r->hub_art);
    r->hub_art = NULL;
    r->hub_tried = 0;
}

/* Put a ring on screen — the first one on open, and every one entered after.
 * The petals of the ring that is leaving are thrown away and the new ones are
 * born in the hub, so descending a level looks like the menu blooming again
 * rather than a set of labels being swapped under the cursor.
 *
 * The world goes with them: a fresh one is cheaper to make than it is to
 * reason about a body count that changes underneath a running simulation. */
static void show_menu(Radial *r, int idx) {
    const RadialMenu *m = &r->cfg.menus[idx];

    r->menu  = idx;
    r->count = m->item_count;
    r->sel   = 0;
    snprintf(r->center, sizeof(r->center), "%s", m->center);
    snprintf(r->center_text, sizeof(r->center_text), "%s", m->center_text);

    /* Icons belong to the ring that drew them, hub picture included. */
    art_free(r);

    if (r->world_ok) b2DestroyWorld(r->world);
    b2WorldDef wd = b2DefaultWorldDef();
    wd.gravity = (b2Vec2){0.0f, 0.0f};   /* the motion is all in the springs */
    r->world = b2CreateWorld(&wd);
    r->world_ok = true;
    for (int i = 0; i < r->count; i++) {
        r->petals[i].body = petal_body_create(r);
        r->petals[i].grow = 0.0;
    }
    r->dirty = true;
}

/* Down into a petal's own ring. */
static void enter_submenu(Radial *r, int sub) {
    if (sub <= 0 || sub >= r->cfg.menu_count) return;   /* loader says impossible */
    if (r->depth >= CONFIG_MAX_RADIAL_MENUS) return;
    r->back_menu[r->depth] = r->menu;
    r->back_sel[r->depth]  = r->sel;
    r->depth++;
    show_menu(r, sub);
}

/* Back up one ring, landing on the petal that was pressed to leave it. False
 * at the root, where there is nothing above and the caller decides what a way
 * out means — closing, for every caller there is. */
static bool leave_submenu(Radial *r) {
    if (r->depth <= 0) return false;
    r->depth--;
    int sel = r->back_sel[r->depth];
    show_menu(r, r->back_menu[r->depth]);
    if (sel >= 0 && sel < r->count) r->sel = sel;
    return true;
}

/* `animate` false tears the panel down synchronously — required from
 * radial_destroy, where a pending callback would reference freed memory. */
static void radial_close_ex(Radial *r, bool animate) {
    if (!r->open) return;
    /* Input goes back to the client NOW; only the pixels linger, so the
     * keyboard grab never waits on the animation. */
    r->open = false;
    if (r->overlay) {
        closing_cancel(r);
        if (animate) {
            r->closing = r->overlay;
            cairo_overlay_animate_out(r->overlay, PANEL_ANIM_MS, PANEL_RISE_PX,
                                      closing_done, r);
        } else {
            cairo_overlay_destroy(r->overlay);
        }
        r->overlay = NULL;
    }
    if (r->world_ok) {
        b2DestroyWorld(r->world);   /* frees every petal body */
        r->world_ok = false;
    }
    r->count = 0;
    /* The next open starts at the root, whichever ring this one ended on. */
    r->menu  = 0;
    r->depth = 0;
    /* Icons are dropped with the menu. Re-decoding ten of them costs a few ms
     * on the next open, and holding them costs memory for as long as the
     * session lasts — the menu is open for seconds at a time. */
    art_free(r);
}

static void radial_close(Radial *r) { radial_close_ex(r, true); }

static void radial_open(Radial *r) {
    if (r->open) return;
    FwmServer *server = r->server;
    const RadialConfig *rc = conf(r);

    /* Nothing configured: say so once, rather than flashing an empty ring. The
     * config error is already recorded by the loader. */
    if (rc->menu_count <= 0 || rc->menus[0].item_count <= 0) return;

    closing_cancel(r);

    r->cfg    = *rc;      /* the snapshot; see the struct */
    r->radius = rc->radius;
    r->depth  = 0;

    /* The ring must fit the monitor it is drawn on, labels and all. A radius
     * asked for in the config that does not fit is shrunk rather than clipped
     * — half a menu off the bottom of the screen is worse than a smaller one. */
    struct wlr_box screen;
    server_active_output_box(server, &screen);

    double need = 2.0 * (r->radius + PETAL_R + RING_PAD);
    double room = screen.width < screen.height ? screen.width : screen.height;
    if (need > room) {
        r->radius = room / 2.0 - PETAL_R - RING_PAD;
        if (r->radius < PETAL_R) r->radius = PETAL_R;
        need = 2.0 * (r->radius + PETAL_R + RING_PAD);
    }
    r->side = (int)need;
    r->cx = r->cy = r->side / 2.0;

    /* Centred on the monitor the user is at, not at the layout origin — and in
     * that monitor's box, so the ring sits under the pointer's screen rather
     * than in the middle of a column that screen only shows part of. */
    r->px = screen.x + (screen.width  - r->side) / 2;
    r->py = screen.y + (screen.height - r->side) / 2;

    /* The panel before the petals: with no surface to draw on there is nothing
     * to build a world for, and this way the failure path frees nothing. */
    r->overlay = cairo_overlay_create(server->layer_overlay, r->side, r->side);
    if (!r->overlay) return;

    show_menu(r, 0);

    wlr_scene_node_set_position(&r->overlay->node, r->px, r->py);
    cairo_overlay_animate_in(r->overlay, PANEL_ANIM_MS, PANEL_RISE_PX);
    r->open = true;
    r->dirty = true;
}

/* ── public api ──────────────────────────────────────────────────────── */

Radial *radial_create(struct FwmServer *server) {
    Radial *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->server = server;
    return r;
}

void radial_destroy(Radial *r) {
    if (!r) return;
    radial_close_ex(r, false);
    closing_cancel(r);
    art_free(r);
    free(r);
}

void radial_toggle(Radial *r) {
    if (!r) return;
    if (r->open) radial_close(r);
    else         radial_open(r);
}

bool radial_is_open(Radial *r) { return r && r->open; }

/* Press the focused petal: down into its ring if it has one, otherwise the
 * action, and the menu is done.
 *
 * Close BEFORE dispatching, never after: the action may be "mode:physics" or
 * "launcher", and tearing the menu down on top of what it just opened would
 * undo it. */
static void fire_selected(Radial *r) {
    if (r->sel < 0 || r->sel >= r->count) return;
    const RadialItem *it = item_at(r, r->sel);
    if (it->submenu) {
        enter_submenu(r, it->submenu);
        return;
    }
    char action[sizeof(it->action)];
    snprintf(action, sizeof(action), "%s", it->action);
    radial_close(r);
    server_dispatch_action(r->server, action);
}

static void step_focus(Radial *r, int delta) {
    if (r->count <= 0) return;
    /* Wraps, because a ring does. */
    r->sel = (r->sel + delta % r->count + r->count) % r->count;
}

bool radial_handle_key(Radial *r, xkb_keysym_t sym) {
    if (!r || !r->open) return false;
    r->dirty = true;

    switch (sym) {
    /* One ring at a time on the way out: Escape climbs back to the ring a
     * petal was pressed in, and only closes the menu from the root. Anything
     * else would make a mis-turn three levels down cost the whole menu. */
    case XKB_KEY_Escape:
    case XKB_KEY_BackSpace:
        if (!leave_submenu(r)) radial_close(r);
        return true;

    /* The knob. Turning it is a volume key on every keyboard that has one, and
     * pressing it is mute — which is exactly why the menu has to be asked
     * before the binds are: [binds] answers these three by changing the
     * volume, and it must not do that with the ring up. */
    case XKB_KEY_XF86AudioRaiseVolume:
        step_focus(r, +server_knob_step(r->server, +1));
        return true;
    case XKB_KEY_XF86AudioLowerVolume:
        step_focus(r, -server_knob_step(r->server, -1));
        return true;
    case XKB_KEY_XF86AudioMute:
        fire_selected(r);
        return true;

    /* The same ring from a keyboard with no knob at all. Without these the
     * feature would work on one model of one brand, and could not be tried by
     * anyone else — or on a laptop. */
    case XKB_KEY_Right:
    case XKB_KEY_Down:
    case XKB_KEY_Tab:
        step_focus(r, +1);
        return true;
    case XKB_KEY_Left:
    case XKB_KEY_Up:
    case XKB_KEY_ISO_Left_Tab:
        step_focus(r, -1);
        return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
        fire_selected(r);
        return true;
    default:
        break;
    }

    /* Straight to a petal by its place in the ring, 1 at the top. */
    if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
        int idx = (int)(sym - XKB_KEY_1);
        if (idx < r->count) { r->sel = idx; fire_selected(r); }
        return true;
    }
    if (sym == XKB_KEY_0 && r->count >= 10) {
        r->sel = 9;
        fire_selected(r);
        return true;
    }

    return true;   /* swallow everything else while open */
}

/* Petal under a panel-local point, or -1. Uses the live body positions so
 * hit-testing matches what is drawn, even mid-bloom. */
static int petal_at(Radial *r, double x, double y) {
    for (int i = 0; i < r->count; i++) {
        b2Vec2 c = b2Body_GetPosition(r->petals[i].body);
        double dx = x - m2px(c.x), dy = y - m2px(c.y);
        if (dx * dx + dy * dy <= PETAL_R * PETAL_R) return i;
    }
    return -1;
}

void radial_handle_motion(Radial *r, double lx, double ly) {
    if (!r || !r->open) return;
    int hit = petal_at(r, lx - r->px, ly - r->py);
    if (hit >= 0 && hit != r->sel) {
        r->sel = hit;
        r->dirty = true;
    }
}

bool radial_handle_button(Radial *r, double lx, double ly, bool pressed) {
    if (!r || !r->open) return false;
    if (!pressed) return true;   /* swallow releases too */

    double x = lx - r->px, y = ly - r->py;
    int hit = petal_at(r, x, y);
    if (hit >= 0) {
        r->sel = hit;
        fire_selected(r);
        return true;
    }
    /* The hub never dismisses — it is the middle of the ring, and closing from
     * there would make the picture a trap. Inside a sub-ring it is the way
     * back up; at the root it is simply the picture, and does nothing. */
    double dx = x - r->cx, dy = y - r->cy;
    if (dx * dx + dy * dy <= HUB_R * HUB_R) {
        leave_submenu(r);
        return true;
    }
    radial_close(r);
    return true;
}

void radial_tick(Radial *r, double dt) {
    if (!r || !r->open) return;

    bool moving = petals_moving(r);
    if (!moving && !r->dirty) return;
    r->dirty = false;

    /* The focus swell is a wall-clock ease, not part of the simulation: it
     * follows the knob, and the knob does not push anything. */
    double k = 1.0 - exp(-GROW_RATE * dt);
    for (int i = 0; i < r->count; i++) {
        double want = (i == r->sel) ? 1.0 : 0.0;
        r->petals[i].grow += (want - r->petals[i].grow) * k;
    }

    petals_apply_springs(r);
    b2World_Step(r->world, (float)dt, 4);
    cairo_overlay_update(r->overlay, draw_radial, r);
}
