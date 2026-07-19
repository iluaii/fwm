#ifndef FWM_GROUP_H
#define FWM_GROUP_H

/* Tab-stacks (Hyprland-style groups): several windows share one geometry and
 * one physics body; only the active member is visible. A chevron tab bar
 * (tray-island style) sits above the window and lists the members. */

#include <wayland-server-core.h>
#include <stdbool.h>
#include <stdint.h>

struct FwmServer;
struct FwmView;

#define GROUP_MAX_MEMBERS 16
#define GROUP_TAB_H 26

typedef struct FwmGroup {
    struct FwmView *members[GROUP_MAX_MEMBERS];
    int count;
    int active;
    struct wlr_scene_buffer *tabbar; /* child of the active member's scene tree */
    int drawn_w;                     /* width the bar was last drawn for */
    struct wl_list link;
} FwmGroup;

/* Create a group holding just `view` (tab bar appears). NULL if impossible. */
FwmGroup *group_create(struct FwmServer *server, struct FwmView *view);
/* Dissolve: all members become normal windows again (slightly cascaded). */
void group_dissolve(struct FwmServer *server, FwmGroup *group);
/* Add a window to an existing group (it becomes the active tab). */
bool group_add(struct FwmServer *server, FwmGroup *group, struct FwmView *view);
/* Remove one member (window closed / popped out). Dissolves at count==1. */
void group_remove(struct FwmServer *server, struct FwmView *view);
/* Switch the visible tab. */
void group_set_active(struct FwmServer *server, FwmGroup *group, int index);
void group_cycle(struct FwmServer *server, FwmGroup *group, int dir);

/* Hit-test the tab bars of all groups at screen coords; returns the group and
 * tab index under the cursor, or NULL. Used for click-to-switch and for
 * dropping a dragged window onto a bar. */
FwmGroup *group_bar_at(struct FwmServer *server, double lx, double ly, int *tab_index);

/* Per-tick upkeep: redraw a bar whose window width changed (resize/tiling). */
void group_tick(struct FwmServer *server);
/* Redraw the bar (titles changed etc.). */
void group_redraw(struct FwmServer *server, FwmGroup *group);

#endif /* FWM_GROUP_H */
