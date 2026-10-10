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

// A rectangle with its corners cut off — the shape every island in fwm has.
// `cut` is how deep each corner goes; `ends` turns it into the tray's pill:
// the left and right sides come to a point half way down instead.

import QtQuick
import QtQuick.Shapes

Shape {
    id: shape
    property color color: "#000000"
    property color edge: "transparent"
    property real edgeWidth: 0
    property real cut: 12
    property bool ends: false

    preferredRendererType: Shape.CurveRenderer
    antialiasing: true

    readonly property real c: ends ? height / 2 : Math.min(cut, width / 2, height / 2)

    ShapePath {
        fillColor: shape.color
        strokeColor: shape.edge
        strokeWidth: shape.edgeWidth
        joinStyle: ShapePath.MiterJoin

        startX: shape.c; startY: 0
        PathLine { x: shape.width - shape.c; y: 0 }
        PathLine { x: shape.width; y: shape.ends ? shape.height / 2 : shape.c }
        PathLine { x: shape.width; y: shape.ends ? shape.height / 2 : shape.height - shape.c }
        PathLine { x: shape.width - shape.c; y: shape.height }
        PathLine { x: shape.c; y: shape.height }
        PathLine { x: 0; y: shape.ends ? shape.height / 2 : shape.height - shape.c }
        PathLine { x: 0; y: shape.ends ? shape.height / 2 : shape.c }
        PathLine { x: shape.c; y: 0 }
    }
}
