/* bsp.c is a plain tree over ints — no compositor, no wlroots, no display.
 * That makes the tiling geometry the one part of fwm that can be pinned down
 * exactly, so this suite asserts numbers rather than "looks about right". */

#include "test.h"
#include "bsp.h"
#include <stdlib.h>

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

static void test_find_border(void) {
    CASE("border is found within the threshold");
    BspNode *root = tree_of(2);
    root->split_h = 0;
    bsp_recalc(root, 0, 0, 100, 50, 10);   /* border sits at x = 45 */

    CHECK(bsp_find_border(root, 45, 25, 3) == root);
    CHECK(bsp_find_border(root, 47, 25, 3) == root);   /* inside threshold */
    CHECK_NULL(bsp_find_border(root, 60, 25, 3));      /* outside it */
    CHECK_NULL(bsp_find_border(root, 45, 99, 3));      /* past the node's height */

    CASE("a leaf has no border");
    CHECK_NULL(bsp_find_border(root->left, 45, 25, 3));
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
    BspActual same[] = { {1, root->left->w, root->left->h},
                         {2, root->right->w, root->right->h} };
    bsp_place_actual(root, 0, 0, 10, same, 2);
    CHECK_INT(root->left->ay, root->left->y);
    CHECK_INT(root->right->ay, root->right->y);
    bsp_free(root);

    CASE("a short client does not widen the gap below it");
    root = tree_of(2);
    root->split_h = 1;
    bsp_recalc(root, 0, 0, 100, 600, 10);           /* slots 295 | gap | 295 */
    CHECK_INT(root->left->h, 295);
    /* The upper window took 289 of its 295. */
    BspActual act[] = { {1, 100, 289}, {2, 100, 295} };
    bsp_place_actual(root, 0, 0, 10, act, 2);
    CHECK_INT(root->left->ay, 0);
    CHECK_INT(root->right->ay, 299);                /* 289 + 10, not 305 */
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
    BspActual a3[] = { {1, 100, 440}, {2, 100, 210}, {3, 100, 200} };
    bsp_place_actual(root, 0, 0, 10, a3, 3);

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
    BspActual h2[] = { {1, 480, 100}, {2, 495, 100} };   /* left is 15 short */
    bsp_place_actual(root, 0, 0, 10, h2, 2);
    CHECK_INT(root->left->ax, 0);
    CHECK_INT(root->right->ax, 490);                     /* 480 + 10 */
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

    BspActual nest[] = { {1, 200, 100}, {3, 200, 150}, {2, 200, 300} };
    bsp_place_actual(root, 0, 0, 10, nest, 3);
    BspNode *l1 = bsp_find(root, 1), *l3 = bsp_find(root, 3), *l2 = bsp_find(root, 2);
    CHECK_INT(l1->ay, 0);
    CHECK_INT(l3->ay, 110);             /* 100 + gap */
    /* leaf2 sits below the whole subtree: 100 + gap + 150, then one more gap.
     * Drop the gap from the subtree's reported height and this lands at 260. */
    CHECK_INT(l2->ay, 270);
    bsp_free(root);

    CASE("the same for widths");
    root = NULL;
    bsp_insert(&root, 0, 1);
    bsp_insert(&root, 1, 2);
    bsp_insert(&root, 1, 3);
    root->split_h = 0;                  /* subtree left, leaf2 right */
    root->left->split_h = 0;            /* subtree splits side by side */
    bsp_recalc(root, 0, 0, 900, 200, 10);
    BspActual wide[] = { {1, 100, 200}, {3, 150, 200}, {2, 300, 200} };
    bsp_place_actual(root, 0, 0, 10, wide, 3);
    l1 = bsp_find(root, 1); l3 = bsp_find(root, 3); l2 = bsp_find(root, 2);
    CHECK_INT(l1->ax, 0);
    CHECK_INT(l3->ax, 110);
    CHECK_INT(l2->ax, 270);
    bsp_free(root);

    CASE("a leaf with no committed size falls back to its slot");
    root = tree_of(2);
    root->split_h = 1;
    bsp_recalc(root, 0, 0, 100, 600, 10);
    bsp_place_actual(root, 0, 0, 10, NULL, 0);
    CHECK_INT(root->left->ay, root->left->y);
    CHECK_INT(root->right->ay, root->right->y);
    bsp_free(root);

    CASE("origin is honoured");
    root = tree_of(2);
    root->split_h = 1;
    bsp_recalc(root, 40, 70, 100, 600, 10);
    BspActual off[] = { {1, 100, 289}, {2, 100, 295} };
    bsp_place_actual(root, 40, 70, 10, off, 2);
    CHECK_INT(root->left->ax, 40);
    CHECK_INT(root->left->ay, 70);
    CHECK_INT(root->right->ay, 70 + 289 + 10);
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
    test_find_border();
    test_place_actual();
    return t_report("bsp");
}
