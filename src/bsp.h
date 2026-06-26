#ifndef FWM_BSP_H
#define FWM_BSP_H

#include <X11/Xlib.h>

typedef struct BspNode {
    struct BspNode *parent;
    struct BspNode *left;
    struct BspNode *right;
    Window win;
    int x, y, w, h;
    int split_h;
    float ratio;
} BspNode;

typedef struct {
    BspNode *node;
    int x, y, w, h;
} BspBorder;

BspNode *bsp_new_leaf(Window win);

void bsp_insert(BspNode **root, Window focused, Window new_win);

void bsp_remove(BspNode **root, Window win);

void bsp_recalc(BspNode *node, Display *dpy, int camera_x,
                int x, int y, int w, int h);
void bsp_collect_leaves(BspNode *node, BspNode **out, int *count);
void bsp_swap(BspNode *root, Window a, Window b);
BspNode *bsp_find_border(BspNode *root, int x, int y, int threshold);

BspNode *bsp_find(BspNode *root, Window win);

void bsp_free(BspNode *node);

#endif
