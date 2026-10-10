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

// One user, as a body: a pane of frosted glass over the wallpaper with the
// face, the name and the password on it. Main.qml moves it — the card only
// says when a hand takes hold of it, where the hand is, and when it lets go.

import QtQuick
import QtQuick.Effects

Item {
    id: card
    width: 340
    height: 268

    property Item backdrop
    property string userName: ""
    property string realName: ""
    property string icon: ""
    property bool selected: false
    property string message: ""
    property bool capsLock: false

    property color surface: "#191206"
    property color foreground: "#efe0cf"
    property color accent: "#ffba28"
    property color alarm: "#ffb4ab"
    property real fill: 0.5
    property real cut: 14

    signal login(string password)
    signal chosen()
    signal grabbed(real gx, real gy)
    signal dragged(real gx, real gy)
    signal dropped()

    function focusPassword() { password.forceActiveFocus(); }
    function clearPassword() { password.text = ""; }

    // ── the glass ────────────────────────────────────────────────────────
    // What is behind the card, blurred, cut to the card's shape. Taken from
    // the wallpaper item rather than from the screen, so the other cards and
    // the pills never end up inside it.
    ShaderEffectSource {
        id: behind
        anchors.fill: parent
        visible: false
        sourceItem: card.backdrop
        sourceRect: Qt.rect(card.x, card.y, card.width, card.height)
        live: true
    }
    Chamfer {
        id: shapeMask
        anchors.fill: parent
        cut: card.cut
        color: "white"
        layer.enabled: true
        visible: false
    }
    MultiEffect {
        anchors.fill: parent
        source: behind
        blurEnabled: true
        blur: 1.0
        blurMax: 48
        maskEnabled: true
        maskSource: shapeMask
    }
    Chamfer {
        anchors.fill: parent
        cut: card.cut
        color: Qt.rgba(card.surface.r, card.surface.g, card.surface.b, card.fill)
        edge: card.selected ? card.accent : Qt.rgba(card.foreground.r, card.foreground.g,
                                                    card.foreground.b, 0.18)
        edgeWidth: card.selected ? 1.5 : 1
    }

    // ── a hand on the card ───────────────────────────────────────────────
    // Under everything that takes input, so typing and clicking the field are
    // never a drag. A press that never moves is a click: it picks this user.
    MouseArea {
        id: hand
        anchors.fill: parent
        property bool moved: false
        property real px: 0
        property real py: 0
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        onPressed: (m) => {
            moved = false;
            var g = mapToItem(null, m.x, m.y);
            px = g.x; py = g.y;
            card.grabbed(g.x, g.y);
        }
        onPositionChanged: (m) => {
            var g = mapToItem(null, m.x, m.y);
            if (Math.abs(g.x - px) + Math.abs(g.y - py) > 4) moved = true;
            card.dragged(g.x, g.y);
        }
        onReleased: {
            card.dropped();
            if (!moved) card.chosen();
            card.focusPassword();
        }
    }

    // ── the face ─────────────────────────────────────────────────────────
    Column {
        anchors.centerIn: parent
        spacing: 12

        Item {
            id: face
            width: 76; height: 76
            anchors.horizontalCenter: parent.horizontalCenter

            Chamfer {
                anchors.fill: parent
                cut: 12
                color: Qt.rgba(card.accent.r, card.accent.g, card.accent.b, 0.85)
            }
            Text {
                anchors.centerIn: parent
                visible: avatar.status !== Image.Ready
                text: (card.realName || card.userName).charAt(0).toUpperCase()
                color: card.surface
                font.pixelSize: 34
                font.bold: true
            }
            Image {
                id: avatar
                anchors.fill: parent
                source: card.icon ? card.icon : ""
                fillMode: Image.PreserveAspectCrop
                visible: false
            }
            Chamfer {
                id: faceMask
                anchors.fill: parent
                cut: 12
                color: "white"
                layer.enabled: true
                visible: false
            }
            MultiEffect {
                anchors.fill: parent
                visible: avatar.status === Image.Ready
                source: avatar
                maskEnabled: true
                maskSource: faceMask
            }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: card.realName || card.userName
            color: card.foreground
            font.pixelSize: 18
        }

        // The password, always where the keys go: a card that has just been
        // thrown, shaken or picked still takes what is typed.
        Item {
            width: 248; height: 38
            anchors.horizontalCenter: parent.horizontalCenter

            Chamfer {
                anchors.fill: parent
                cut: 8
                color: Qt.rgba(card.surface.r, card.surface.g, card.surface.b,
                               Math.min(1, card.fill + 0.25))
                edge: password.activeFocus ? card.accent
                                           : Qt.rgba(card.foreground.r, card.foreground.g,
                                                     card.foreground.b, 0.25)
                edgeWidth: 1
            }
            TextInput {
                id: password
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                verticalAlignment: TextInput.AlignVCenter
                echoMode: TextInput.Password
                passwordCharacter: "•"
                color: card.foreground
                selectionColor: card.accent
                font.pixelSize: 16
                clip: true
                onAccepted: card.login(text)
                onActiveFocusChanged: if (activeFocus) card.chosen()
                Keys.onEscapePressed: text = ""
            }
            Text {
                anchors.fill: password
                anchors.leftMargin: 14
                verticalAlignment: Text.AlignVCenter
                visible: password.text.length === 0
                text: "password"
                color: Qt.rgba(card.foreground.r, card.foreground.g, card.foreground.b, 0.45)
                font.pixelSize: 14
            }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            height: 16
            text: card.message ? card.message : (card.capsLock ? "caps lock is on" : "")
            color: card.message ? card.alarm : Qt.rgba(card.foreground.r, card.foreground.g,
                                                       card.foreground.b, 0.6)
            font.pixelSize: 12
        }
    }
}
