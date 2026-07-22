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

/* The size a window really committed, which need not be the size of its slot. */
typedef struct {
    uint32_t id;
    int w, h;
} BspActual;

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
BspNode *bsp_find_border(BspNode *root, int x, int y, int threshold);
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
 * accumulating into the edge of the layout. */
void bsp_place_actual(BspNode *root, int x, int y, int w, int h, int gap,
                      const BspActual *actual, int n_actual);

void bsp_free(BspNode *node);

#endif
