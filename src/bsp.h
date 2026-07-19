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
} BspNode;

typedef struct {
    BspNode *node;
    int x, y, w, h;
} BspBorder;

BspNode *bsp_new_leaf(uint32_t id);
BspNode *bsp_find(BspNode *root, uint32_t id);
void bsp_insert(BspNode **root, uint32_t focused, uint32_t new_id);
void bsp_remove(BspNode **root, uint32_t id);
void bsp_recalc(BspNode *node, int x, int y, int w, int h, int gap);
void bsp_collect_leaves(BspNode *node, BspNode **out, int *count);
void bsp_swap(BspNode *root, uint32_t a, uint32_t b);
BspNode *bsp_find_border(BspNode *root, int x, int y, int threshold);
void bsp_free(BspNode *node);

#endif
