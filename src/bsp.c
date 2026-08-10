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

#include "bsp.h"
#include <stdlib.h>

BspNode *bsp_new_leaf(uint32_t id) {
    BspNode *n = calloc(1, sizeof(BspNode));
    n->id = id;
    n->ratio = 0.5f;
    return n;
}

BspNode *bsp_find(BspNode *root, uint32_t id) {
    if (!root || id == 0) return NULL;
    if (root->id == id && root->left == NULL) return root;
    BspNode *l = bsp_find(root->left, id);
    if (l) return l;
    return bsp_find(root->right, id);
}

void bsp_insert(BspNode **root, uint32_t focused, uint32_t new_id) {
    if (!*root) {
        *root = bsp_new_leaf(new_id);
        return;
    }

    BspNode *target = focused ? bsp_find(*root, focused) : NULL;
    if (!target) {
        target = *root;
        while (target->left) target = target->left;
    }

    BspNode *old_leaf = bsp_new_leaf(target->id);
    BspNode *new_leaf = bsp_new_leaf(new_id);

    old_leaf->parent = target;
    new_leaf->parent = target;

    target->split_h = (target->w >= target->h) ? 0 : 1;
    target->id = 0;
    target->left  = old_leaf;
    target->right = new_leaf;
}

void bsp_remove(BspNode **root, uint32_t id) {
    BspNode *leaf = bsp_find(*root, id);
    if (!leaf) return;

    BspNode *parent = leaf->parent;

    if (!parent) {
        free(leaf);
        *root = NULL;
        return;
    }

    BspNode *sibling = (parent->left == leaf) ? parent->right : parent->left;

    BspNode *grandparent = parent->parent;
    if (!grandparent) {
        sibling->parent = NULL;
        *root = sibling;
    } else {
        if (grandparent->left == parent)
            grandparent->left = sibling;
        else
            grandparent->right = sibling;
        sibling->parent = grandparent;
    }

    free(leaf);
    free(parent);
    if (!grandparent && sibling->id == 0 && !sibling->left && !sibling->right) {
        *root = NULL;
        free(sibling);
    }
}

void bsp_recalc(BspNode *node, int x, int y, int w, int h, int gap) {
    if (!node) return;
    node->x = x; node->y = y;
    node->w = w; node->h = h;

    if (node->id != 0) {
        // Recalculating coordinates of the leaf node. The window manager
        // will update the actual window dimensions later.
        return;
    }

    if (!node->left && !node->right) return;

    int left_w = (int)((w - gap) * node->ratio);
    if (left_w < 1) left_w = 1;
    int right_w = w - left_w - gap;
    if (right_w < 1) right_w = 1;

    if (!node->split_h) {
        bsp_recalc(node->left,  x,              y, left_w,  h, gap);
        bsp_recalc(node->right, x + left_w + gap, y, right_w, h, gap);
    } else {
        int top_h = (int)((h - gap) * node->ratio);
        if (top_h < 1) top_h = 1;
        int bot_h = h - top_h - gap;
        if (bot_h < 1) bot_h = 1;
        bsp_recalc(node->left,  x, y,            w, top_h, gap);
        bsp_recalc(node->right, x, y + top_h + gap, w, bot_h, gap);
    }
}

void bsp_swap(BspNode *root, uint32_t a, uint32_t b) {
    BspNode *na = bsp_find(root, a);
    BspNode *nb = bsp_find(root, b);
    if (!na || !nb) return;
    na->id = b;
    nb->id = a;
}

/* Is this exact node still part of this tree?
 *
 * For holders of a node pointer across events. The tree is rebuilt from under
 * them by anything that opens, closes or moves a window, and a freed node
 * cannot be told apart from a live one by looking at it — only by asking the
 * tree whether it is still there. */
bool bsp_contains(const BspNode *root, const BspNode *node) {
    if (!root || !node) return false;
    if (root == node) return true;
    return bsp_contains(root->left, node) || bsp_contains(root->right, node);
}

void bsp_insert_at(BspNode **root, uint32_t target, uint32_t new_id, BspSide side) {
    if (!*root) { *root = bsp_new_leaf(new_id); return; }

    BspNode *leaf = bsp_find(*root, target);
    if (!leaf) { bsp_insert(root, 0, new_id); return; }

    /* The same surgery bsp_insert does — the target leaf becomes a split and
     * gains two children — but the axis comes from the side the window was put
     * down on rather than from which way round the slot happens to be, and the
     * new window may be the FIRST child, which is what "dropped on the left"
     * means and what bsp_insert has no way to say. */
    BspNode *old_leaf = bsp_new_leaf(leaf->id);
    BspNode *new_leaf = bsp_new_leaf(new_id);
    old_leaf->parent = leaf;
    new_leaf->parent = leaf;

    int before = (side == BSP_SIDE_LEFT || side == BSP_SIDE_UP);
    leaf->id = 0;
    leaf->ratio = 0.5f;
    leaf->split_h = (side == BSP_SIDE_UP || side == BSP_SIDE_DOWN);
    leaf->left  = before ? new_leaf : old_leaf;
    leaf->right = before ? old_leaf : new_leaf;
}

BspNode *bsp_leaf_at(BspNode *root, int x, int y) {
    if (!root) return NULL;
    if (root->id != 0) {
        return (x >= root->ax && x < root->ax + root->aw &&
                y >= root->ay && y < root->ay + root->ah) ? root : NULL;
    }
    BspNode *l = bsp_leaf_at(root->left, x, y);
    return l ? l : bsp_leaf_at(root->right, x, y);
}

BspNode *bsp_edge_node(BspNode *leaf, int split_h, int want_left) {
    for (BspNode *n = leaf; n && n->parent; n = n->parent) {
        BspNode *p = n->parent;
        if (!p->split_h != !split_h) continue;
        if ((p->left == n) == (want_left != 0)) return p;
    }
    return NULL;
}

/* The smallest a subtree can be made: a split needs both its children plus the
 * gap between them along the axis it divides, and the larger of the two across
 * it. Windows that have never refused a size contribute nothing. */
static void subtree_min(const BspNode *n, int gap, const BspActual *act,
                        int n_act, int *mw, int *mh) {
    *mw = 0; *mh = 0;
    if (!n) return;

    if (n->id != 0 || !n->left || !n->right) {
        for (int i = 0; i < n_act; i++) {
            if (act[i].id != n->id) continue;
            *mw = act[i].min_w;
            *mh = act[i].min_h;
            return;
        }
        return;
    }

    int aw, ah, bw, bh;
    subtree_min(n->left,  gap, act, n_act, &aw, &ah);
    subtree_min(n->right, gap, act, n_act, &bw, &bh);
    if (!n->split_h) { *mw = aw + gap + bw;     *mh = ah > bh ? ah : bh; }
    else             { *mw = aw > bw ? aw : bw; *mh = ah + gap + bh; }
}

/* Where the divider stands inside `span`, held clear of both floors.
 *
 * When the two floors together do not fit there is nothing to hold to, and the
 * proportional cut is used unchanged: a layout smaller than the windows in it
 * is going to overlap wherever the split goes, and choosing which pair overlaps
 * helps nobody. */
static int clamp_split(int cut, int span, int gap, int lo_min, int hi_min) {
    int room = span - gap;
    if (room < 2) return 1;
    if (lo_min + hi_min <= room) {
        if (cut < lo_min) cut = lo_min;
        if (cut > room - hi_min) cut = room - hi_min;
    }
    if (cut < 1) cut = 1;
    if (cut > room - 1) cut = room - 1;
    return cut;
}

void bsp_ratio_limits(const BspNode *n, int gap, const BspActual *actual,
                      int n_actual, float *lo, float *hi) {
    *lo = 0.05f;
    *hi = 0.95f;
    if (!n || n->id != 0 || !n->left || !n->right) return;

    int span = n->split_h ? n->ah : n->aw;
    int room = span - gap;
    if (room < 2) return;

    int lmw, lmh, rmw, rmh;
    subtree_min(n->left,  gap, actual, n_actual, &lmw, &lmh);
    subtree_min(n->right, gap, actual, n_actual, &rmw, &rmh);
    int lmin = n->split_h ? lmh : lmw;
    int rmin = n->split_h ? rmh : rmw;
    if (lmin + rmin > room) return;

    float l = (float)lmin / (float)room;
    float h = (float)(room - rmin) / (float)room;
    if (l > *lo) *lo = l;
    if (h < *hi) *hi = h;
    if (*hi < *lo) *hi = *lo;
}

/* `max` is not paranoia: windows past MAX_WINDOWS get no physics body, but
 * they are still inserted into the tree, so a desktop can hold more leaves
 * than every caller's fixed-size array has room for. Dropping the excess
 * leaves those windows untiled -- the same degradation they already get from
 * having no body -- instead of writing past the end of a stack buffer. */
void bsp_collect_leaves(BspNode *node, BspNode **out, int *count, int max) {
    if (!node || *count >= max) return;
    if (node->id != 0) {
        out[(*count)++] = node;
        return;
    }
    bsp_collect_leaves(node->left, out, count, max);
    bsp_collect_leaves(node->right, out, count, max);
}

static void place_rec(BspNode *n, int x, int y, int w, int h, int gap,
                      const BspActual *act, int n_act, int *out_w, int *out_h) {
    if (!n) { *out_w = 0; *out_h = 0; return; }
    n->ax = x;
    n->ay = y;
    n->aw = w;
    n->ah = h;

    if (n->id != 0 || !n->left || !n->right) {
        /* What the client took of what it was offered. Never more: a client
         * that asks for more than the layout has must not push its neighbours
         * off the screen. */
        int cw = w, ch = h;
        for (int i = 0; i < n_act; i++) {
            if (act[i].id == n->id) { cw = act[i].w; ch = act[i].h; break; }
        }
        if (cw > w) cw = w;
        if (ch > h) ch = h;
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
        *out_w = cw;
        *out_h = ch;
        return;
    }

    /* What neither side may be squeezed below. Without this a window that
     * refuses to shrink — Discord stops at about 940px wide — is still only
     * counted as the size it was OFFERED, so the layout puts its neighbour
     * where the refused pixels are and the two overlap: the neighbour appears
     * to slide underneath the window that hit its limit. */
    int lmw, lmh, rmw, rmh;
    subtree_min(n->left,  gap, act, n_act, &lmw, &lmh);
    subtree_min(n->right, gap, act, n_act, &rmw, &rmh);

    int aw, ah, bw, bh;
    if (!n->split_h) {
        /* The first child is offered its share of the split, the second is
         * offered the rest — which is more than its share whenever the first
         * came up short. */
        int lw = clamp_split((int)((w - gap) * n->ratio), w, gap, lmw, rmw);
        place_rec(n->left, x, y, lw, h, gap, act, n_act, &aw, &ah);
        int rw = w - aw - gap;
        if (rw < 1) rw = 1;
        place_rec(n->right, x + aw + gap, y, rw, h, gap, act, n_act, &bw, &bh);
        *out_w = aw + gap + bw;
        *out_h = ah > bh ? ah : bh;
    } else {
        int th = clamp_split((int)((h - gap) * n->ratio), h, gap, lmh, rmh);
        place_rec(n->left, x, y, w, th, gap, act, n_act, &aw, &ah);
        int rh = h - ah - gap;
        if (rh < 1) rh = 1;
        place_rec(n->right, x, y + ah + gap, w, rh, gap, act, n_act, &bw, &bh);
        *out_w = aw > bw ? aw : bw;
        *out_h = ah + gap + bh;
    }
}

static void shift_rec(BspNode *n, int dx, int dy) {
    if (!n) return;
    n->ax += dx;
    n->ay += dy;
    shift_rec(n->left, dx, dy);
    shift_rec(n->right, dx, dy);
}

void bsp_place_actual(BspNode *root, int x, int y, int w, int h, int gap,
                      const BspActual *actual, int n_actual) {
    int ow, oh;
    place_rec(root, x, y, w, h, gap, actual, n_actual, &ow, &oh);

    /* Interior leftovers are handed to the next window along, but the LAST one
     * on each axis has no neighbour to hand its own to — so everything the
     * layout did not take piles up against the bottom and right edges. A
     * terminal short by most of a character cell makes the bottom gap read as
     * roughly two gaps_out against a top gap of exactly one. Splitting the
     * remainder between the two edges makes the layout sit centred in its area
     * instead of anchored top-left. Most visible with the tray hidden, where
     * the top gap is a plain gaps_out standing right next to it. */
    int dx = (w - ow) / 2;
    int dy = (h - oh) / 2;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    if (dx || dy) shift_rec(root, dx, dy);
}

void bsp_free(BspNode *node) {
    if (!node) return;
    bsp_free(node->left);
    bsp_free(node->right);
    free(node);
}
