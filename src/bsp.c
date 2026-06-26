#include "bsp.h"

#include <stdlib.h>
#include <X11/Xlib.h>

BspNode *bsp_new_leaf(Window win) {
    BspNode *n = calloc(1, sizeof(BspNode));
    n->win = win;
    n->ratio = 0.5f;
    return n;
}

BspNode *bsp_find(BspNode *root, Window win) {
    if (!root || win == None) return NULL;
    if (root->win == win && root->left == NULL) return root;
    BspNode *l = bsp_find(root->left, win);
    if (l) return l;
    return bsp_find(root->right, win);
}

void bsp_insert(BspNode **root, Window focused, Window new_win) {
    if (!*root) {
        *root = bsp_new_leaf(new_win);
        return;
    }

    BspNode *target = focused ? bsp_find(*root, focused) : NULL;
    if (!target) {
        target = *root;
        while (target->left) target = target->left;
    }

    BspNode *old_leaf = bsp_new_leaf(target->win);
    BspNode *new_leaf = bsp_new_leaf(new_win);

    old_leaf->parent = target;
    new_leaf->parent = target;

    target->split_h = (target->w >= target->h) ? 0 : 1;
    target->win = None;
    target->left  = old_leaf;
    target->right = new_leaf;
}

void bsp_remove(BspNode **root, Window win) {
    BspNode *leaf = bsp_find(*root, win);
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
    if (!grandparent && sibling->win == None && !sibling->left && !sibling->right) {
        *root = NULL;
        free(sibling);
    }
}

void bsp_recalc(BspNode *node, Display *dpy, int camera_x,
                int x, int y, int w, int h) {
    if (!node) return;
    node->x = x; node->y = y;
    node->w = w; node->h = h;

    if (node->win != None) {
        XMoveResizeWindow(dpy, node->win, x - camera_x, y, w, h);
        return;
    }

    if (!node->left && !node->right) return;

    int gap = 6;

    if (!node->split_h) {
        int left_w = (int)((w - gap) * node->ratio);
        int right_w = w - left_w - gap;
        bsp_recalc(node->left,  dpy, camera_x, x,              y, left_w,  h);
        bsp_recalc(node->right, dpy, camera_x, x + left_w + gap, y, right_w, h);
    } else {
        int top_h = (int)((h - gap) * node->ratio);
        int bot_h = h - top_h - gap;
        bsp_recalc(node->left,  dpy, camera_x, x, y,            w, top_h);
        bsp_recalc(node->right, dpy, camera_x, x, y + top_h + gap, w, bot_h);
    }
}
void bsp_swap(BspNode *root, Window a, Window b) {
    BspNode *na = bsp_find(root, a);
    BspNode *nb = bsp_find(root, b);
    if (!na || !nb) return;
    na->win = b;
    nb->win = a;
}

BspNode *bsp_find_border(BspNode *root, int x, int y, int threshold) {
    if (!root || root->win != None) return NULL;

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

void bsp_collect_leaves(BspNode *node, BspNode **out, int *count) {
    if (!node) return;
    if (node->win != None) {
        out[(*count)++] = node;
        return;
    }
    bsp_collect_leaves(node->left, out, count);
    bsp_collect_leaves(node->right, out, count);
}

void bsp_free(BspNode *node) {
    if (!node) return;
    bsp_free(node->left);
    bsp_free(node->right);
    free(node);
}