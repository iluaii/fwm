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

BspNode *bsp_find_border(BspNode *root, int x, int y, int threshold) {
    if (!root || root->id != 0) return NULL;

    if (!root->split_h) {
        int border_x = root->left->x + root->left->w;
        if (abs(x - border_x) <= threshold &&
            y >= root->y && y <= root->y + root->h)
            return root;
    } else {
        int border_y = root->left->y + root->left->h;
        if (abs(y - border_y) <= threshold &&
            x >= root->x && x <= root->x + root->w)
            return root;
    }

    BspNode *l = bsp_find_border(root->left, x, y, threshold);
    if (l) return l;
    return bsp_find_border(root->right, x, y, threshold);
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

    int aw, ah, bw, bh;
    if (!n->split_h) {
        /* The first child is offered its share of the split, the second is
         * offered the rest — which is more than its share whenever the first
         * came up short. */
        int lw = (int)((w - gap) * n->ratio);
        if (lw < 1) lw = 1;
        place_rec(n->left, x, y, lw, h, gap, act, n_act, &aw, &ah);
        int rw = w - aw - gap;
        if (rw < 1) rw = 1;
        place_rec(n->right, x + aw + gap, y, rw, h, gap, act, n_act, &bw, &bh);
        *out_w = aw + gap + bw;
        *out_h = ah > bh ? ah : bh;
    } else {
        int th = (int)((h - gap) * n->ratio);
        if (th < 1) th = 1;
        place_rec(n->left, x, y, w, th, gap, act, n_act, &aw, &ah);
        int rh = h - ah - gap;
        if (rh < 1) rh = 1;
        place_rec(n->right, x, y + ah + gap, w, rh, gap, act, n_act, &bw, &bh);
        *out_w = aw > bw ? aw : bw;
        *out_h = ah + gap + bh;
    }
}

void bsp_place_actual(BspNode *root, int x, int y, int w, int h, int gap,
                      const BspActual *actual, int n_actual) {
    int ow, oh;
    place_rec(root, x, y, w, h, gap, actual, n_actual, &ow, &oh);
}

void bsp_free(BspNode *node) {
    if (!node) return;
    bsp_free(node->left);
    bsp_free(node->right);
    free(node);
}
