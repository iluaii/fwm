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

#ifndef FWM_RUBBER_H
#define FWM_RUBBER_H

/* Where the resize rubber's picture goes, and what fills the box around it.
 *
 * The whole of the effect's geometry, as arithmetic: given the picture, the
 * buffer holding it and the box the hand is asking for, this says which piece
 * of the picture to sample and where to put it. No wlroots, no scene graph,
 * no view — so it can be, and is, checked on its own (tests/test_rubber.c),
 * which is the only way any of this can be checked at all: the headless
 * backend has no pointer to drag an edge with.
 *
 * The rule it encodes, and the reason the effect exists in this shape: THE
 * PICTURE IS NEVER SCALED. A box smaller than the picture crops it; a box
 * larger leaves it at its own size and fills the strip left over with the
 * pixels running along its edge. See the rub_* comment in view.h for why —
 * briefly, a window dragged from its minimum to full screen would otherwise be
 * one frame magnified tenfold, which is the mush this replaced.
 *
 * The edge fill samples ONE row or column, never two. Two stretched apart are
 * a gradient, which is the blur being avoided; one is a flat band of exactly
 * the colour the window ends on, and stretching it cannot introduce detail
 * that is not there. */

typedef struct {
    /* Source rectangle in BUFFER pixels — what to sample out of the picture. */
    double sx, sy, sw, sh;
    /* Destination in LAYOUT pixels, relative to the window's top-left. */
    int x, y, w, h;
    int on;   /* 0 when this piece has nothing to draw */
} RubberPart;

enum {
    RUBBER_PICTURE = 0,
    RUBBER_RIGHT,
    RUBBER_BOTTOM,
    RUBBER_CORNER,
    RUBBER_PARTS
};

/* `pic_w`/`pic_h` is what the picture spans in layout pixels, `buf_w`/`buf_h`
 * the same picture measured in its own — the two differ on a scaled output,
 * and their ratio is the only conversion in here. `box_w`/`box_h` is what the
 * hand is asking for.
 *
 * Every part comes back with `on` set or clear; a caller switching a piece off
 * may leave the rest of its fields alone. Nonsense in (a picture of no size, a
 * box of none) leaves every part off rather than dividing by zero. */
void rubber_parts(int pic_w, int pic_h, int buf_w, int buf_h,
                  int box_w, int box_h, RubberPart out[RUBBER_PARTS]);

#endif /* FWM_RUBBER_H */
