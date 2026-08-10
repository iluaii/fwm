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

#ifndef FWM_BSP_H
#define FWM_BSP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct BspNode {
    struct BspNode *parent;
    struct BspNode *left;
    struct BspNode *right;
    uint32_t        id; /* ID of the view/window. 0 if it's an internal node */
    int x, y, w, h;
    int split_h;
    float ratio;
    /* Result of bsp_place_actual(): where this node goes and what size to ask
     * it for, once the clients before it have said what they actually took.
     * Both differ from the x/y/w/h slot grid, which stays as bsp_recalc left
     * it because border dragging hit-tests against it. */
    int ax, ay, aw, ah;
} BspNode;

/* The size a window really committed, which need not be the size of its slot,
 * and the smallest it has been observed to accept. A client with a minimum size
 * — Discord will not go under about 940px wide — answers a smaller configure by
 * committing its minimum anyway, and the layout has to give it that room rather
 * than lay the next window out over the top of it. Zero means "no floor known",
 * which is every window until one refuses. */
typedef struct {
    uint32_t id;
    int w, h;
    int min_w, min_h;
} BspActual;

/* Which side of an existing window a new one is put down on. */
typedef enum {
    BSP_SIDE_LEFT,
    BSP_SIDE_RIGHT,
    BSP_SIDE_UP,
    BSP_SIDE_DOWN,
} BspSide;

typedef struct {
    BspNode *node;
    int x, y, w, h;
} BspBorder;

BspNode *bsp_new_leaf(uint32_t id);
BspNode *bsp_find(BspNode *root, uint32_t id);
void bsp_insert(BspNode **root, uint32_t focused, uint32_t new_id);
void bsp_remove(BspNode **root, uint32_t id);
void bsp_recalc(BspNode *node, int x, int y, int w, int h, int gap);
/* Collects at most `max` leaves into `out`; excess leaves are dropped. */
void bsp_collect_leaves(BspNode *node, BspNode **out, int *count, int max);
void bsp_swap(BspNode *root, uint32_t a, uint32_t b);
/* Put `new_id` down beside `target`, on the named side, splitting that window's
 * slot in two. Unlike bsp_insert — which splits the focused window along
 * whichever axis is longer — the caller chooses both the axis and the order,
 * which is what lets a window be dropped onto a particular edge of another.
 * Falls back to bsp_insert when `target` is not in the tree. */
void bsp_insert_at(BspNode **root, uint32_t target, uint32_t new_id, BspSide side);
/* The window at a point, hit-tested against where the windows actually ARE
 * (ax/ay/aw/ah) rather than against the slot grid. NULL in the gaps between
 * them and outside the layout. */
BspNode *bsp_leaf_at(BspNode *root, int x, int y);
/* The split that owns one edge of a leaf's slot: the nearest ancestor dividing
 * along `split_h` (0 vertical line, 1 horizontal line) that has this subtree on
 * its `left` side when `want_left` is set, on its right when it is not. Which
 * means: the divider along the leaf's right edge is (0, 1) and the one along its
 * left edge is (0, 0), bottom is (1, 1) and top is (1, 0). NULL when that edge
 * of the window is the edge of the screen. */
BspNode *bsp_edge_node(BspNode *leaf, int split_h, int want_left);
/* How far a split's ratio may travel before one side is squeezed under the
 * minimum size of the windows in it. Always returns a usable range: when the
 * two sides together cannot fit, the floors are ignored rather than crossed. */
void bsp_ratio_limits(const BspNode *n, int gap, const BspActual *actual,
                      int n_actual, float *lo, float *hi);
/* Whether `node` is still part of `root`'s tree — for anything holding a node
 * pointer across events, which the tree does not promise to keep alive. */
bool bsp_contains(const BspNode *root, const BspNode *node);
/* Lay out the tree inside (x, y, w, h) against the sizes clients really
 * committed, writing each node's position to ax/ay and the size to ask it for
 * to aw/ah. `actual` gives the committed size per window id; an id missing
 * from it is assumed to fill what it was offered.
 *
 * Two things fall out of this that the slot grid cannot do. Interior gaps come
 * out exactly `gap`, because each subtree starts one gap past where its
 * neighbour really ended rather than past where its slot ended. And the last
 * child of every split is offered whatever its earlier siblings did not take,
 * so a short client's leftover is absorbed by the next window along instead of
 * accumulating into the edge of the layout. What the LAST child of each axis
 * leaves over has no such neighbour, so the finished layout is centred in the
 * area: that remainder is split between the two edges rather than piling up
 * against the bottom and right ones. */
void bsp_place_actual(BspNode *root, int x, int y, int w, int h, int gap,
                      const BspActual *actual, int n_actual);

void bsp_free(BspNode *node);

#endif
