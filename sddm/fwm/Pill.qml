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

// One island of the top strip, drawn like fwm's tray: a pill with pointed
// ends, the theme's surface at the tray's opacity, its text in the theme's
// foreground. Clickable when `onClicked` is wired; `armed` paints it in the
// error colour, for the power pills that want a second press.

import QtQuick

Item {
    id: pill
    property string text: ""
    property color surface: "#191206"
    property color foreground: "#efe0cf"
    property color accent: "#ffba28"
    property color alarm: "#ffb4ab"
    property real fill: 0.5
    property bool clickable: false
    property bool armed: false
    property int fontSize: 13
    signal clicked()

    implicitHeight: 30
    implicitWidth: label.implicitWidth + height + 14
    width: implicitWidth
    height: implicitHeight

    Chamfer {
        anchors.fill: parent
        ends: true
        color: Qt.rgba(pill.surface.r, pill.surface.g, pill.surface.b,
                       mouse.containsMouse && pill.clickable ? Math.min(1, pill.fill + 0.2) : pill.fill)
        edge: pill.armed ? pill.alarm : "transparent"
        edgeWidth: pill.armed ? 1.5 : 0
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    Text {
        id: label
        anchors.centerIn: parent
        text: pill.text
        color: pill.armed ? pill.alarm : pill.foreground
        font.pixelSize: pill.fontSize
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        enabled: pill.clickable
        hoverEnabled: true
        cursorShape: pill.clickable ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: pill.clicked()
    }
}
