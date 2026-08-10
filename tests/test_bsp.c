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

/* bsp.c is a plain tree over ints — no compositor, no wlroots, no display.
 * That makes the tiling geometry the one part of fwm that can be pinned down
 * exactly, so this suite asserts numbers rather than "looks about right". */

#include "test.h"
#include "bsp.h"
#include <stdlib.h>

/* One window's committed size, with no floor under it — which is every window
 * that has never refused a size, and so every window in these cases but the
 * ones that are about floors. */
#define ACT(id, w, h) { (id), (w), (h), 0, 0 }
/* ... and one that will not go below (mw, mh), whatever it is offered. */
#define ACT_MIN(id, w, h, mw, mh) { (id), (w), (h), (mw), (mh) }

/* Build a tree of `n` leaves with ids 1..n, each split off the previous one. */
static BspNode *tree_of(int n) {
    BspNode *root = NULL;
    for (int i = 1; i <= n; i++) bsp_insert(&root, i > 1 ? (uint32_t)(i - 1) : 0, (uint32_t)i);
    return root;
}

static void test_leaf(void) {
    CASE("new_leaf");
    BspNode *n = bsp_new_leaf(7);
    CHECK_NOT_NULL(n);
    CHECK_INT(n->id, 7);
    CHECK_DBL(n->ratio, 0.5, 1e-6);
    CHECK_NULL(n->left);
    CHECK_NULL(n->right);
    CHECK_NULL(n->parent);
    bsp_free(n);
}

static void test_find(void) {
    CASE("find");
    CHECK_NULL(bsp_find(NULL, 1));

    BspNode *root = tree_of(3);
    CHECK_NOT_NULL(bsp_find(root, 1));
    CHECK_NOT_NULL(bsp_find(root, 3));
    CHECK_NULL(bsp_find(root, 99));

    /* 0 is the marker for an internal node, so it must never match one —
     * otherwise removing a window could pick a subtree instead of a leaf. */
    CHECK_NULL(bsp_find(root, 0));
    CHECK_INT(root->id, 0);          /* the root really is internal here */
    bsp_free(root);
}

static void test_insert(void) {
    CASE("insert into empty");
    BspNode *root = NULL;
    bsp_insert(&root, 0, 1);
    CHECK_NOT_NULL(root);
    CHECK_INT(root->id, 1);
    CHECK_NULL(root->left);

    CASE("insert splits the focused leaf");
    bsp_insert(&root, 1, 2);
    CHECK_INT(root->id, 0);                    /* became internal */
    CHECK_NOT_NULL(root->left);
    CHECK_NOT_NULL(root->right);
    CHECK_INT(root->left->id, 1);              /* the old window stays left */
    CHECK_INT(root->right->id, 2);             /* the new one arrives right */
    CHECK(root->left->parent == root);
    CHECK(root->right->parent == root);
    bsp_free(root);

    CASE("unknown focus falls back to the leftmost leaf");
    root = tree_of(3);
    BspNode *leftmost = root;
    while (leftmost->left) leftmost = leftmost->left;
    uint32_t victim = leftmost->id;
    bsp_insert(&root, 12345, 9);               /* no such window */
    BspNode *split = bsp_find(root, 9);
    CHECK_NOT_NULL(split);
    CHECK_NOT_NULL(split->parent);
    if (split->parent) CHECK_INT(split->parent->left->id, victim);
    bsp_free(root);
}

static void test_split_direction(void) {
    /* "Dwindle" means splitting along the longer side. The decision reads the
     * node's current w/h, which only exist once bsp_recalc has run. */
    CASE("wide leaf splits vertically");
    BspNode *root = NULL;
    bsp_insert(&root, 0, 1);
    bsp_recalc(root, 0, 0, 200, 100, 0);
    bsp_insert(&root, 1, 2);
    CHECK_INT(root->split_h, 0);
    bsp_free(root);

    CASE("tall leaf splits horizontally");
    root = NULL;
    bsp_insert(&root, 0, 1);
    bsp_recalc(root, 0, 0, 100, 200, 0);
    bsp_insert(&root, 1, 2);
    CHECK_INT(root->split_h, 1);
    bsp_free(root);

    /* Pinning down a real quirk rather than an intention: a leaf that has
     * never been through bsp_recalc has w == h == 0, and 0 >= 0 picks the
     * vertical split. Insert order relative to recalc is therefore visible in
     * the layout. */
    CASE("leaf with no geometry yet splits vertically");
    root = NULL;
    bsp_insert(&root, 0, 1);
    bsp_insert(&root, 1, 2);
    CHECK_INT(root->split_h, 0);
    bsp_free(root);
}

static void test_remove(void) {
    CASE("remove the only leaf");
    BspNode *root = NULL;
    bsp_insert(&root, 0, 1);
    bsp_remove(&root, 1);
    CHECK_NULL(root);

    CASE("removing a leaf promotes its sibling");
    root = tree_of(2);
    bsp_remove(&root, 1);
    CHECK_NOT_NULL(root);
    if (root) {
        CHECK_INT(root->id, 2);
        CHECK_NULL(root->parent);   /* promoted to root, so it has no parent */
        CHECK_NULL(root->left);
    }
    bsp_remove(&root, 2);
    CHECK_NULL(root);

    CASE("removing deeper keeps the rest of the tree intact");
    root = tree_of(3);
    bsp_remove(&root, 2);
    CHECK_NULL(bsp_find(root, 2));
    CHECK_NOT_NULL(bsp_find(root, 1));
    CHECK_NOT_NULL(bsp_find(root, 3));
    bsp_free(root);

    CASE("removing an unknown id changes nothing");
    root = tree_of(2);
    bsp_remove(&root, 999);
    CHECK_NOT_NULL(bsp_find(root, 1));
    CHECK_NOT_NULL(bsp_find(root, 2));
    bsp_free(root);
}

static void test_recalc(void) {
    /* One vertical split across 100px with a 10px gap: 45 | gap | 45. */
    CASE("vertical split geometry");
    BspNode *root = tree_of(2);
    root->split_h = 0;
    bsp_recalc(root, 0, 0, 100, 50, 10);
    CHECK_INT(root->left->x, 0);
    CHECK_INT(root->left->w, 45);
    CHECK_INT(root->right->x, 55);
    CHECK_INT(root->right->w, 45);
    CHECK_INT(root->left->h, 50);
    CHECK_INT(root->right->h, 50);
    /* The gap comes out of the content, it is not added on top. */
    CHECK_INT(root->left->w + 10 + root->right->w, 100);
    bsp_free(root);

    CASE("horizontal split geometry");
    root = tree_of(2);
    root->split_h = 1;
    bsp_recalc(root, 0, 0, 50, 100, 10);
    CHECK_INT(root->left->y, 0);
    CHECK_INT(root->left->h, 45);
    CHECK_INT(root->right->y, 55);
    CHECK_INT(root->right->h, 45);
    bsp_free(root);

    CASE("ratio is honoured");
    root = tree_of(2);
    root->split_h = 0;
    root->ratio = 0.25f;
    bsp_recalc(root, 0, 0, 100, 50, 0);
    CHECK_INT(root->left->w, 25);
    CHECK_INT(root->right->w, 75);
    bsp_free(root);

    CASE("degenerate sizes never go below 1px");
    root = tree_of(2);
    root->split_h = 0;
    bsp_recalc(root, 0, 0, 4, 4, 10);   /* gap wider than the area */
    CHECK(root->left->w >= 1);
    CHECK(root->right->w >= 1);
    CHECK(root->left->h >= 1);
    bsp_free(root);

    CASE("offset origin propagates");
    root = tree_of(2);
    root->split_h = 0;
    bsp_recalc(root, 1000, 20, 100, 50, 0);
    CHECK_INT(root->left->x, 1000);
    CHECK_INT(root->right->x, 1050);
    CHECK_INT(root->left->y, 20);
    bsp_free(root);
}

static void test_collect_leaves(void) {
    CASE("collect gathers every leaf");
    BspNode *root = tree_of(4);
    BspNode **out = malloc(8 * sizeof *out);
    int n = 0;
    bsp_collect_leaves(root, out, &n, 8);
    CHECK_INT(n, 4);
    free(out);

    /* The reason `max` exists: windows past MAX_WINDOWS still enter the tree,
     * so a desktop can hold more leaves than the caller's array. The array
     * here is sized exactly to `max`, so a sanitizer build turns any overrun
     * into a failure rather than silent corruption. */
    CASE("collect stops at max instead of overrunning");
    out = malloc(2 * sizeof *out);
    n = 0;
    bsp_collect_leaves(root, out, &n, 2);
    CHECK_INT(n, 2);
    free(out);

    CASE("collect on an empty tree");
    out = malloc(2 * sizeof *out);
    n = 0;
    bsp_collect_leaves(NULL, out, &n, 2);
    CHECK_INT(n, 0);
    free(out);
    bsp_free(root);
}

static void test_swap(void) {
    CASE("swap exchanges two windows");
    BspNode *root = tree_of(2);
    BspNode *a = bsp_find(root, 1);
    bsp_swap(root, 1, 2);
    CHECK_INT(a->id, 2);                  /* the node stays, the id moves */
    CHECK_NOT_NULL(bsp_find(root, 1));
    CHECK_NOT_NULL(bsp_find(root, 2));

    CASE("swap with an unknown id is a no-op");
    bsp_swap(root, 1, 999);
    CHECK_NOT_NULL(bsp_find(root, 1));
    CHECK_NOT_NULL(bsp_find(root, 2));
    bsp_free(root);
}

/* Where the windows actually are, which is what a cursor is tested against.
 * bsp_recalc alone fills the slot grid and leaves ax/ay/aw/ah at zero. */
static void place(BspNode *root, int x, int y, int w, int h, int gap) {
    bsp_recalc(root, x, y, w, h, gap);
    bsp_place_actual(root, x, y, w, h, gap, NULL, 0);
}

static void test_leaf_at(void) {
    CASE("the window under a point, and nothing in the gap between two");
    BspNode *root = tree_of(2);
    root->split_h = 0;
    place(root, 0, 0, 100, 50, 10);       /* 45 | gap | 45 */

    CHECK(bsp_leaf_at(root, 10, 25) == root->left);
    CHECK(bsp_leaf_at(root, 90, 25) == root->right);
    CHECK_NULL(bsp_leaf_at(root, 50, 25));    /* in the gap */
    CHECK_NULL(bsp_leaf_at(root, 10, 80));    /* below the layout */
    CHECK_NULL(bsp_leaf_at(NULL, 10, 25));
    bsp_free(root);
}

static void test_edge_node(void) {
    CASE("each edge names the split that moves it");
    /* Three in a row: 1 | 2 | 3, the second split nested in the first's right. */
    BspNode *root = tree_of(3);
    root->split_h = 0;
    root->right->split_h = 0;
    BspNode *one = bsp_find(root, 1), *two = bsp_find(root, 2);

    /* The leftmost window has a divider on its right and none on its left. */
    CHECK(bsp_edge_node(one, 0, 1) == root);
    CHECK_NULL(bsp_edge_node(one, 0, 0));
    /* The middle one has a divider either side, and they are different. */
    CHECK(bsp_edge_node(two, 0, 1) == root->right);
    CHECK(bsp_edge_node(two, 0, 0) == root);

    CASE("a split along the other axis is not an answer");
    CHECK_NULL(bsp_edge_node(two, 1, 1));
    CHECK_NULL(bsp_edge_node(two, 1, 0));
    bsp_free(root);
}

static void test_insert_at(void) {
    CASE("dropped on the right of a window, it follows it");
    BspNode *root = NULL;
    bsp_insert(&root, 0, 1);
    bsp_insert_at(&root, 1, 2, BSP_SIDE_RIGHT);
    CHECK_INT(root->split_h, 0);
    CHECK_INT(root->left->id, 1);
    CHECK_INT(root->right->id, 2);

    CASE("dropped on the left, it goes first");
    bsp_insert_at(&root, 1, 3, BSP_SIDE_LEFT);
    BspNode *split = bsp_find(root, 3)->parent;
    CHECK_INT(split->split_h, 0);
    CHECK_INT(split->left->id, 3);
    CHECK_INT(split->right->id, 1);

    CASE("dropped above and below, the split turns");
    bsp_insert_at(&root, 2, 4, BSP_SIDE_UP);
    BspNode *up = bsp_find(root, 4)->parent;
    CHECK_INT(up->split_h, 1);
    CHECK_INT(up->left->id, 4);
    CHECK_INT(up->right->id, 2);

    CASE("dropped on a window that is not there, it still lands");
    bsp_insert_at(&root, 999, 5, BSP_SIDE_RIGHT);
    CHECK_NOT_NULL(bsp_find(root, 5));

    CASE("the first window in an empty tree is the root");
    bsp_free(root);
    root = NULL;
    bsp_insert_at(&root, 7, 1, BSP_SIDE_DOWN);
    CHECK_INT(root->id, 1);
    bsp_free(root);
}

/* A window with a minimum size — Discord will not go below about 940px wide —
 * used to be laid out as though it had taken the smaller size it was offered,
 * and its neighbour was then placed over the top of it. */
static void test_min_size(void) {
    CASE("a window that refuses to shrink keeps its room");
    BspNode *root = tree_of(2);
    root->split_h = 0;
    root->ratio = 0.2f;                    /* asks the left one to be 190px */
    BspActual act[] = { ACT_MIN(1, 400, 100, 400, 0), ACT(2, 590, 100) };
    bsp_place_actual(root, 0, 0, 1000, 100, 10, act, 2);

    CHECK_INT(root->left->aw, 400);        /* its floor, not the 190 asked for */
    /* And the neighbour starts after it rather than under it. */
    CHECK(root->right->ax >= root->left->ax + root->left->aw + 10);

    CASE("the ratio may not be dragged past that floor");
    float lo, hi;
    bsp_ratio_limits(root, 10, act, 2, &lo, &hi);
    CHECK(lo >= 400.0f / 990.0f - 0.001f);
    CHECK(hi <= 0.95f);

    CASE("two floors that cannot both fit are ignored rather than crossed");
    BspActual big[] = { ACT_MIN(1, 800, 100, 800, 0), ACT_MIN(2, 800, 100, 800, 0) };
    bsp_ratio_limits(root, 10, big, 2, &lo, &hi);
    CHECK(lo < hi);
    bsp_free(root);
}

/* A held border is a bare pointer into a tree the layout is free to rebuild.
 * Dragging one while a window closes used to write the new ratio into freed
 * memory, so the drag now asks the tree whether it still holds the node. */
static void test_contains(void) {
    CASE("a node of the tree is in it, a stranger is not");
    BspNode *root = tree_of(3);
    BspNode *leaf = bsp_find(root, 2);
    CHECK(bsp_contains(root, root));
    CHECK(bsp_contains(root, leaf));
    CHECK_NOT_NULL(leaf);

    BspNode *stray = bsp_new_leaf(99);
    CHECK(!bsp_contains(root, stray));
    free(stray);

    CASE("nothing is in an empty tree, and nothing contains nothing");
    CHECK(!bsp_contains(NULL, root));
    CHECK(!bsp_contains(root, NULL));

    CASE("removing a window takes its border out of the tree");
    /* bsp_remove frees the leaf AND its parent — which is exactly the node a
     * hand may be holding. The pointer is stale afterwards, so it must be
     * compared, never dereferenced: only the address is used here. */
    BspNode *parent = leaf->parent;
    CHECK(bsp_contains(root, parent));
    bsp_remove(&root, 2);
    CHECK(!bsp_contains(root, parent));
    bsp_free(root);
}

/* The bug this was written for: a terminal rounds its height down to whole
 * character cells, so it commits less than its slot. Anchored at the slot's
 * top-left, that leftover sits between windows and the gap reads far wider
 * than gaps_in — invisible with two side-by-side tiles, glaring from three. */
static void test_place_actual(void) {
    CASE("slot-sized clients place exactly like the slot grid");
    BspNode *root = tree_of(2);
    root->split_h = 1;
    bsp_recalc(root, 0, 0, 100, 600, 10);
    BspActual same[] = { ACT(1, root->left->w, root->left->h),
                         ACT(2, root->right->w, root->right->h) };
    bsp_place_actual(root, 0, 0, 100, 600, 10, same, 2);
    CHECK_INT(root->left->ay, root->left->y);
    CHECK_INT(root->right->ay, root->right->y);
    bsp_free(root);

    CASE("a short client does not widen the gap below it");
    root = tree_of(2);
    root->split_h = 1;
    bsp_recalc(root, 0, 0, 100, 600, 10);           /* slots 295 | gap | 295 */
    CHECK_INT(root->left->h, 295);
    /* The upper window took 289 of its 295. */
    BspActual act[] = { ACT(1, 100, 289), ACT(2, 100, 295) };
    bsp_place_actual(root, 0, 0, 100, 600, 10, act, 2);
    /* 594 of the 600 is taken, and the leftover 6 is split between the two
     * edges rather than dumped below the last window: everything shifts by 3. */
    CHECK_INT(root->left->ay, 3);
    CHECK_INT(root->right->ay, 302);                /* 3 + 289 + 10, not 305 */
    /* Which is the whole point: the visible gap is the configured one. */
    CHECK_INT(root->right->ay - (root->left->ay + 289), 10);
    bsp_free(root);

    CASE("every gap in a column stays exact");
    root = tree_of(3);
    /* Force a single column: three tiles stacked. */
    root->split_h = 1;
    BspNode *sub = root->right->id == 0 ? root->right : root->left;
    sub->split_h = 1;
    bsp_recalc(root, 0, 0, 100, 900, 10);
    BspActual a3[] = { ACT(1, 100, 440), ACT(2, 100, 210), ACT(3, 100, 200) };
    bsp_place_actual(root, 0, 0, 100, 900, 10, a3, 3);

    BspNode *lv[8]; int n = 0;
    bsp_collect_leaves(root, lv, &n, 8);
    CHECK_INT(n, 3);
    /* Walk the column top to bottom and check each seam. */
    for (int i = 0; i + 1 < n; i++) {
        int hi = 0;
        for (int k = 0; k < 3; k++) if (a3[k].id == lv[i]->id) hi = a3[k].h;
        CHECK_INT(lv[i + 1]->ay - (lv[i]->ay + hi), 10);
    }
    bsp_free(root);

    CASE("horizontal neighbours line up the same way");
    root = tree_of(2);
    root->split_h = 0;
    bsp_recalc(root, 0, 0, 1000, 100, 10);
    BspActual h2[] = { ACT(1, 480, 100), ACT(2, 495, 100) };   /* left is 15 short */
    bsp_place_actual(root, 0, 0, 1000, 100, 10, h2, 2);
    CHECK_INT(root->left->ax, 7);                        /* 15 leftover, halved */
    CHECK_INT(root->right->ax, 497);                     /* 7 + 480 + 10 */
    CHECK_INT(root->right->ax - (root->left->ax + 480), 10);
    bsp_free(root);

    /* A subtree reports the space it occupies to its parent, gaps included.
     * Only a nested split placed FIRST exposes that: its accumulated extent is
     * what the sibling after it is measured from. */
    CASE("a nested subtree reports its own gaps to the parent");
    root = NULL;
    bsp_insert(&root, 0, 1);
    bsp_insert(&root, 1, 2);            /* root: leaf1 | leaf2 */
    bsp_insert(&root, 1, 3);            /* leaf1 becomes a split of 1 and 3 */
    CHECK_INT(root->left->id, 0);       /* the subtree really is first */
    CHECK_INT(root->right->id, 2);
    root->split_h = 1;                  /* subtree on top, leaf2 below */
    root->left->split_h = 1;            /* and the subtree is a column too */
    bsp_recalc(root, 0, 0, 200, 900, 10);

    BspActual nest[] = { ACT(1, 200, 100), ACT(3, 200, 150), ACT(2, 200, 300) };
    bsp_place_actual(root, 0, 0, 200, 900, 10, nest, 3);
    BspNode *l1 = bsp_find(root, 1), *l3 = bsp_find(root, 3), *l2 = bsp_find(root, 2);
    /* The column occupies 570 of 900, so the whole thing sits 165 lower. */
    CHECK_INT(l1->ay, 165);
    CHECK_INT(l3->ay, 275);             /* 100 + gap */
    /* leaf2 sits below the whole subtree: 100 + gap + 150, then one more gap.
     * Drop the gap from the subtree's reported height and this lands at 260. */
    CHECK_INT(l2->ay, 435);
    bsp_free(root);

    CASE("the same for widths");
    root = NULL;
    bsp_insert(&root, 0, 1);
    bsp_insert(&root, 1, 2);
    bsp_insert(&root, 1, 3);
    root->split_h = 0;                  /* subtree left, leaf2 right */
    root->left->split_h = 0;            /* subtree splits side by side */
    bsp_recalc(root, 0, 0, 900, 200, 10);
    BspActual wide[] = { ACT(1, 100, 200), ACT(3, 150, 200), ACT(2, 300, 200) };
    bsp_place_actual(root, 0, 0, 900, 200, 10, wide, 3);
    l1 = bsp_find(root, 1); l3 = bsp_find(root, 3); l2 = bsp_find(root, 2);
    CHECK_INT(l1->ax, 165);
    CHECK_INT(l3->ax, 275);
    CHECK_INT(l2->ax, 435);
    bsp_free(root);

    /* The other half of the fix. Moving a window up to close an interior gap
     * only relocates the leftover unless something takes it: without this the
     * slack piled up at the bottom of the column, so a column of three
     * terminals ended far short of the area and its bottom gap dwarfed the
     * neighbouring column's. */
    CASE("the last child is offered what the earlier ones did not take");
    root = tree_of(2);
    root->split_h = 1;
    bsp_recalc(root, 0, 0, 100, 600, 10);
    CHECK_INT(root->left->h, 295);                  /* even slots to start */
    CHECK_INT(root->right->h, 295);
    BspActual shy[] = { ACT(1, 100, 289), ACT(2, 100, 600) };   /* upper is 6 short */
    bsp_place_actual(root, 0, 0, 100, 600, 10, shy, 2);
    l1 = bsp_find(root, 1); l2 = bsp_find(root, 2);
    CHECK_INT(l1->ah, 295);                         /* offered its share */
    CHECK_INT(l2->ah, 301);                         /* offered the remainder */
    CHECK_INT(l2->ay, 299);                         /* 289 + gap */
    /* And so the column reaches the bottom of the area instead of stopping
     * short of it. */
    CHECK_INT(l2->ay + l2->ah, 600);
    bsp_free(root);

    CASE("the same remainder rule across a row");
    root = tree_of(2);
    root->split_h = 0;
    bsp_recalc(root, 0, 0, 1000, 100, 10);
    CHECK_INT(root->left->w, 495);
    BspActual narrow[] = { ACT(1, 480, 100), ACT(2, 1000, 100) };   /* left is 15 short */
    bsp_place_actual(root, 0, 0, 1000, 100, 10, narrow, 2);
    l1 = bsp_find(root, 1); l2 = bsp_find(root, 2);
    CHECK_INT(l1->aw, 495);                         /* offered its share */
    CHECK_INT(l2->aw, 510);                         /* offered the remainder */
    CHECK_INT(l2->ax, 490);                         /* 480 + gap */
    CHECK_INT(l2->ax + l2->aw, 1000);               /* the row reaches the edge */
    bsp_free(root);

    CASE("slack does not accumulate down a column of three");
    root = tree_of(3);
    root->split_h = 1;
    (root->right->id == 0 ? root->right : root->left)->split_h = 1;
    bsp_recalc(root, 0, 0, 100, 900, 10);
    /* Every window comes up 6 short of whatever it is offered. */
    BspActual s3[] = { ACT(1, 100, 439), ACT(2, 100, 209), ACT(3, 100, 900) };
    bsp_place_actual(root, 0, 0, 100, 900, 10, s3, 3);
    BspNode *w3[8]; int n3 = 0;
    bsp_collect_leaves(root, w3, &n3, 8);
    CHECK_INT(n3, 3);
    /* The last one still ends at the bottom: it was offered the slack. */
    CHECK_INT(w3[2]->ay + w3[2]->ah, 900);
    bsp_free(root);

    CASE("a client asking for more than it was offered is clamped");
    root = tree_of(2);
    root->split_h = 1;
    bsp_recalc(root, 0, 0, 100, 600, 10);
    BspActual greedy[] = { ACT(1, 100, 5000), ACT(2, 100, 100) };
    bsp_place_actual(root, 0, 0, 100, 600, 10, greedy, 2);
    l2 = bsp_find(root, 2);
    CHECK(l2->ay >= 0);
    CHECK(l2->ay <= 600);                           /* not shoved off the area */
    bsp_free(root);

    CASE("a leaf with no committed size falls back to its slot");
    root = tree_of(2);
    root->split_h = 1;
    bsp_recalc(root, 0, 0, 100, 600, 10);
    bsp_place_actual(root, 0, 0, 100, 600, 10, NULL, 0);
    CHECK_INT(root->left->ay, root->left->y);
    CHECK_INT(root->right->ay, root->right->y);
    bsp_free(root);

    /* The last window on each axis has no neighbour to absorb ITS leftover, so
     * before this it all landed against the bottom/right edge: with a terminal
     * short by most of a character cell the bottom gap read as roughly twice
     * the top one. Splitting it centres the layout in its area instead. */
    CASE("the final leftover is split between the two edges, not dumped at the end");
    root = tree_of(1);
    bsp_recalc(root, 0, 0, 100, 600, 10);
    BspActual lone[] = { ACT(1, 100, 560) };   /* client 40 short of the area */
    bsp_place_actual(root, 0, 0, 100, 600, 10, lone, 1);
    CHECK_INT(root->ay, 20);                /* 40 / 2 above, 40 / 2 below */
    CHECK_INT(600 - (root->ay + 560), 20);  /* the two edges match */
    bsp_free(root);

    CASE("an odd leftover never pushes the layout out of its area");
    root = tree_of(1);
    bsp_recalc(root, 0, 0, 100, 600, 10);
    BspActual odd[] = { ACT(1, 100, 599) };
    bsp_place_actual(root, 0, 0, 100, 600, 10, odd, 1);
    CHECK_INT(root->ay, 0);                 /* half of 1 rounds down to 0 */
    bsp_free(root);

    CASE("a client that fills its area is not moved");
    root = tree_of(1);
    bsp_recalc(root, 0, 0, 100, 600, 10);
    BspActual full[] = { ACT(1, 100, 600) };
    bsp_place_actual(root, 0, 0, 100, 600, 10, full, 1);
    CHECK_INT(root->ay, 0);
    bsp_free(root);

    CASE("origin is honoured");
    root = tree_of(2);
    root->split_h = 1;
    bsp_recalc(root, 40, 70, 100, 600, 10);
    BspActual off[] = { ACT(1, 100, 289), ACT(2, 100, 295) };
    bsp_place_actual(root, 40, 70, 100, 600, 10, off, 2);
    CHECK_INT(root->left->ax, 40);
    CHECK_INT(root->left->ay, 73);              /* origin + half the leftover */
    CHECK_INT(root->right->ay, 73 + 289 + 10);
    bsp_free(root);
}

int main(void) {
    test_leaf();
    test_find();
    test_insert();
    test_split_direction();
    test_remove();
    test_recalc();
    test_collect_leaves();
    test_swap();
    test_leaf_at();
    test_edge_node();
    test_insert_at();
    test_contains();
    test_place_actual();
    test_min_size();
    return t_report("bsp");
}
