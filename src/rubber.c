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

#include "rubber.h"
#include <string.h>

void rubber_parts(int pic_w, int pic_h, int buf_w, int buf_h,
                  int box_w, int box_h, RubberPart out[RUBBER_PARTS]) {
    memset(out, 0, sizeof(RubberPart) * RUBBER_PARTS);
    if (pic_w <= 0 || pic_h <= 0) return;
    if (buf_w <= 0 || buf_h <= 0) return;
    if (box_w <= 0 || box_h <= 0) return;

    /* Buffer pixels per layout pixel. The picture is the whole buffer and it
     * stands for a pic_w x pic_h window, so this ratio carries whatever scale
     * the surface was drawn at and nothing here has to know about outputs. */
    double bx = (double)buf_w / (double)pic_w;
    double by = (double)buf_h / (double)pic_h;

    /* How much of the picture is on screen: all of it, or as much as the box
     * has room for. This is the crop, and it is also where the fill starts. */
    int cw = box_w < pic_w ? box_w : pic_w;
    int ch = box_h < pic_h ? box_h : pic_h;

    out[RUBBER_PICTURE] = (RubberPart){
        .sx = 0.0, .sy = 0.0, .sw = cw * bx, .sh = ch * by,
        .x = 0, .y = 0, .w = cw, .h = ch, .on = 1,
    };

    int gw = box_w - cw;   /* the strip to the right of the picture */
    int gh = box_h - ch;   /* ...and the one below it */

    /* The picture's last column, drawn gw wide and beside it. Its height is the
     * picture's, not the box's — the square below is the corner's. */
    if (gw > 0) {
        out[RUBBER_RIGHT] = (RubberPart){
            .sx = (pic_w - 1) * bx, .sy = 0.0, .sw = bx, .sh = ch * by,
            .x = cw, .y = 0, .w = gw, .h = ch, .on = 1,
        };
    }
    if (gh > 0) {
        out[RUBBER_BOTTOM] = (RubberPart){
            .sx = 0.0, .sy = (pic_h - 1) * by, .sw = cw * bx, .sh = by,
            .x = 0, .y = ch, .w = cw, .h = gh, .on = 1,
        };
    }
    /* The square neither strip reaches, from the picture's own corner pixel. */
    if (gw > 0 && gh > 0) {
        out[RUBBER_CORNER] = (RubberPart){
            .sx = (pic_w - 1) * bx, .sy = (pic_h - 1) * by, .sw = bx, .sh = by,
            .x = cw, .y = ch, .w = gw, .h = gh, .on = 1,
        };
    }
}
