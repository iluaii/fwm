#include "group.h"
#include "server.h"
#include "view.h"
#include "physics.h"
#include "bsp.h"
#include "ui/cairo_overlay.h"
#include <pango/pangocairo.h>
#include "theme.h"

#include <stdlib.h>
#include <string.h>

/* Tray-island look: flat near-black chevron pills, no gradients/outlines.
 * Colours come from the live theme so tab-stacks follow the system palette. */
#define TAB_GAP 4.0

/* Grouping changes how much room the window needs: the tab bar lives above the
 * client area, and server_apply_tiling reserves that strip only for views that
 * are already grouped. Without this the bar hangs into the slot above — for the
 * top row, straight over our tray. */
static void group_retile(FwmServer *server, struct FwmView *view) {
    if (!view) return;
    PhysicsBody *pb = physics_find_body(&server->physics, view->id);
    if (!pb) return;
    if (server->desktop_mode[pb->desktop_id] == DESKTOP_MODE_TILING) {
        server_apply_tiling(server, pb->desktop_id);
    }
}

static void pill_path(cairo_t *cr, double x, double y, double w, double h) {
    double cut = h / 2.0;
    if (cut * 2.0 > w) cut = w / 2.0;
    cairo_move_to(cr, x + cut, y);
    cairo_line_to(cr, x + w - cut, y);
    cairo_line_to(cr, x + w, y + h / 2.0);
    cairo_line_to(cr, x + w - cut, y + h);
    cairo_line_to(cr, x + cut, y + h);
    cairo_line_to(cr, x, y + h / 2.0);
    cairo_close_path(cr);
}

struct BarCtx {
    FwmGroup *group;
    double opacity;
};

static void draw_bar(cairo_t *cr, int w, int h, void *user) {
    struct BarCtx *ctx = user;
    FwmGroup *g = ctx->group;
    if (g->count <= 0) return;

    const FwmTheme *thm = theme_get();

    double tab_w = ((double)w - TAB_GAP * (g->count - 1)) / g->count;

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("sans 9");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

    for (int i = 0; i < g->count; i++) {
        double x = i * (tab_w + TAB_GAP);

        cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], ctx->opacity);
        pill_path(cr, x, 0, tab_w, h - 3);
        cairo_fill(cr);

        const char *title = view_title(g->members[i]);
        if (!title || !title[0]) title = "window";
        pango_layout_set_text(layout, title, -1);
        pango_layout_set_width(layout, (int)((tab_w - h) * PANGO_SCALE));

        int tw, th;
        pango_layout_get_pixel_size(layout, &tw, &th);
        if (i == g->active) {
            cairo_set_source_rgba(cr, thm->text[0], thm->text[1], thm->text[2], 1.0);
        } else {
            cairo_set_source_rgba(cr, thm->muted[0], thm->muted[1], thm->muted[2], 1.0);
        }
        cairo_move_to(cr, x + (tab_w - tw) / 2.0, (h - 3 - th) / 2.0);
        pango_cairo_show_layout(cr, layout);

        // Active tab: gold underline bar, same accent as the tray marker.
        if (i == g->active) {
            cairo_set_source_rgba(cr, thm->accent[0], thm->accent[1], thm->accent[2], 1.0);
            cairo_rectangle(cr, x + tab_w / 2.0 - 10, h - 2, 20, 2);
            cairo_fill(cr);
        }
    }
    g_object_unref(layout);
}

static struct FwmView *group_active_view(FwmGroup *g) {
    if (g->count <= 0) return NULL;
    return g->members[g->active];
}

void group_redraw(FwmServer *server, FwmGroup *group) {
    struct FwmView *view = group_active_view(group);
    if (!view || !view->scene_tree) return;

    // The bar is recreated on every redraw: it is parented to the active
    // member's scene tree (moves with the window for free), and reparenting
    // plus buffer resize is cheaper to reason about than in-place updates.
    if (group->tabbar) {
        cairo_overlay_destroy(group->tabbar);
        group->tabbar = NULL;
    }
    int w = view->width > 40 ? view->width : 40;
    group->tabbar = cairo_overlay_create(view->scene_tree, w, GROUP_TAB_H);
    if (!group->tabbar) return;
    wlr_scene_node_set_position(&group->tabbar->node, 0, -GROUP_TAB_H);

    struct BarCtx ctx = { .group = group, .opacity = server->config.decor.tray_opacity };
    cairo_overlay_update(group->tabbar, draw_bar, &ctx);
    cairo_overlay_make_static(group->tabbar);
    group->drawn_w = w;
}

FwmGroup *group_create(FwmServer *server, struct FwmView *view) {
    if (!view || view->group || !view->scene_tree) return NULL;
    FwmGroup *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->members[0] = view;
    g->count = 1;
    g->active = 0;
    view->group = g;
    wl_list_insert(&server->groups, &g->link);
    group_retile(server, view);
    group_redraw(server, g);
    return g;
}

/* Make `view` the only visible member: enable its scene node, give it the
 * group geometry and the physics body. `from` is the previously active view
 * (NULL on first add). */
static void activate_member(FwmServer *server, FwmGroup *g, struct FwmView *view,
                            struct FwmView *from) {
    if (from && from != view) {
        view->x = from->x;
        view->y = from->y;
        view->width = from->width;
        view->height = from->height;

        // Hand the physics body over: keep position, velocity and flags.
        PhysicsBody *ob = physics_find_body(&server->physics, from->id);
        double vx = 0, vy = 0;
        int pinned = 0, no_collide = 0, tiled = 0, flying = 0;
        if (ob) {
            vx = ob->vx; vy = ob->vy;
            pinned = ob->pinned; no_collide = ob->no_collide;
            tiled = ob->tiled; flying = ob->flying;
        }
        physics_remove_body(&server->physics, from->id);
        PhysicsBody *nb = physics_sync_body(&server->physics, view->id, view->x, view->y,
                                            view->width, view->height, server->screen_width);
        if (nb) {
            nb->vx = vx; nb->vy = vy;
            nb->pinned = pinned; nb->no_collide = no_collide;
            nb->tiled = tiled; nb->flying = flying;
        }

        // In a tiling desktop the group occupies one BSP leaf under the id of
        // its active member — rewrite the leaf to the new front window.
        for (int d = 0; d < 10; d++) {
            BspNode *leaf = bsp_find(server->bsp_roots[d], from->id);
            if (leaf) leaf->id = view->id;
        }

        if (from->scene_tree) {
            wlr_scene_node_set_enabled(&from->scene_tree->node, false);
        }
    }

    if (view->scene_tree) {
        wlr_scene_node_set_enabled(&view->scene_tree->node, true);
        wlr_scene_node_set_position(&view->scene_tree->node,
                                    view->x - server->camera_x, view->y);
    }
    view_set_size(view, view->width, view->height);
    server_focus_view(server, view);
}

bool group_add(FwmServer *server, FwmGroup *g, struct FwmView *view) {
    if (!view || view->group || g->count >= GROUP_MAX_MEMBERS || !view->scene_tree) {
        return false;
    }
    struct FwmView *front = group_active_view(g);
    g->members[g->count] = view;
    g->active = g->count;
    g->count++;
    view->group = g;
    activate_member(server, g, view, front);
    group_retile(server, view);
    group_redraw(server, g);
    return true;
}

void group_set_active(FwmServer *server, FwmGroup *g, int index) {
    if (index < 0 || index >= g->count || index == g->active) return;
    struct FwmView *from = group_active_view(g);
    g->active = index;
    activate_member(server, g, g->members[index], from);
    group_redraw(server, g);
}

void group_cycle(FwmServer *server, FwmGroup *g, int dir) {
    group_set_active(server, g, (g->active + dir + g->count) % g->count);
}

/* Un-hide a member as a standalone window at the given position. */
static void release_member(FwmServer *server, struct FwmView *view, int x, int y) {
    view->group = NULL;
    view->x = x;
    view->y = y;
    if (view->scene_tree) {
        wlr_scene_node_set_enabled(&view->scene_tree->node, true);
        wlr_scene_node_set_position(&view->scene_tree->node, x - server->camera_x, y);
    }
    physics_sync_body(&server->physics, view->id, view->x, view->y,
                      view->width, view->height, server->screen_width);
    view_set_size(view, view->width, view->height);
}

void group_dissolve(FwmServer *server, FwmGroup *g) {
    struct FwmView *front = group_active_view(g);
    int fx = front ? front->x : 0, fy = front ? front->y : 0;

    if (g->tabbar) cairo_overlay_destroy(g->tabbar);
    for (int i = 0; i < g->count; i++) {
        struct FwmView *v = g->members[i];
        if (v == front) {
            v->group = NULL;
            continue; /* already visible with a live body */
        }
        // Cascade the hidden members out so they don't stack pixel-perfectly.
        release_member(server, v, fx + 40 * (i + 1), fy + 40 * (i + 1));
    }
    wl_list_remove(&g->link);
    free(g);
    // Members are ungrouped now, so the reserved tab strip goes back to them.
    group_retile(server, front);
    server_request_tray_redraw(server);
}

void group_remove(FwmServer *server, struct FwmView *view) {
    FwmGroup *g = view->group;
    if (!g) return;

    int idx = -1;
    for (int i = 0; i < g->count; i++) {
        if (g->members[i] == view) { idx = i; break; }
    }
    if (idx < 0) { view->group = NULL; return; }

    bool was_active = (idx == g->active);
    struct FwmView *old_front = group_active_view(g);
    memmove(&g->members[idx], &g->members[idx + 1],
            (g->count - idx - 1) * sizeof(g->members[0]));
    g->count--;
    view->group = NULL;
    if (g->active > idx) g->active--;
    else if (g->active >= g->count) g->active = g->count - 1;

    if (g->count <= 1) {
        // A one-window group is pointless — dissolve quietly.
        group_dissolve(server, g);
        return;
    }
    if (was_active) {
        // The visible window left (closed or popped out): bring up the next
        // tab. The departed view keeps its geometry; the new front inherits it.
        activate_member(server, g, g->members[g->active], old_front);
    }
    group_retile(server, view);
    group_redraw(server, g);
}

FwmGroup *group_bar_at(FwmServer *server, double lx, double ly, int *tab_index) {
    FwmGroup *g;
    wl_list_for_each(g, &server->groups, link) {
        struct FwmView *v = group_active_view(g);
        if (!v || !g->tabbar) continue;
        double x = v->x - server->camera_x;
        double y = v->y - GROUP_TAB_H;
        double w = g->drawn_w;
        if (lx < x || lx >= x + w || ly < y || ly >= y + GROUP_TAB_H) continue;
        if (tab_index) {
            double tab_w = (w - TAB_GAP * (g->count - 1)) / g->count;
            int idx = (int)((lx - x) / (tab_w + TAB_GAP));
            if (idx < 0) idx = 0;
            if (idx >= g->count) idx = g->count - 1;
            *tab_index = idx;
        }
        return g;
    }
    return NULL;
}

void group_tick(FwmServer *server) {
    FwmGroup *g, *tmp;
    wl_list_for_each_safe(g, tmp, &server->groups, link) {
        struct FwmView *v = group_active_view(g);
        if (v && v->width != g->drawn_w) {
            group_redraw(server, g);
        }
    }
}
