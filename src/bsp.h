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
    /* Position from bsp_place_actual(), which may differ from x/y: slots are
     * what a window is asked for, this is where it goes once the client has
     * said what it actually took. */
    int ax, ay;
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
/* Position every leaf from its neighbours' real extents rather than from the
 * slot grid, writing the result to each node's ax/ay. `actual` gives the
 * committed size per window id; a leaf missing from it keeps its slot size.
 * Interior gaps come out exactly `gap`; the slack a short client leaves is
 * pushed to the right and bottom of the layout. */
void bsp_place_actual(BspNode *root, int x, int y, int gap,
                      const BspActual *actual, int n_actual);

void bsp_free(BspNode *node);

#endif
