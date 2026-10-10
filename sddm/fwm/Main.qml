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

// The fwm login screen.
//
// The same wallpaper fwm was showing, the tray's pills along the top, and
// each user as a card of frosted glass that is a body: it can be picked up
// and thrown, bounces off the edges and off the other cards, drops in when
// the screen comes up, jolts when the password is wrong and is flung away
// when it is right. A spring always brings it home (physics.js), so none of
// it ever stands between you and typing your password.
//
// SDDM makes one of these per monitor. Only the primary one gets the cards
// and the pills; the others show their own wallpaper and nothing else.
//
// Colours and wallpapers come from theme.conf, overridden by theme.conf.user,
// which fwm-sddm-sync writes from fwm's wallpaper and its matugen palette.

import QtQuick
import SddmComponents 2.0 as Sddm
import "physics.js" as Physics

Rectangle {
    id: root
    width: 1920
    height: 1080
    color: surface

    readonly property bool primary: typeof primaryScreen === "undefined" ? true : primaryScreen

    function cfg(key, fallback) {
        var v = config[key];
        return (v === undefined || v === null || v === "") ? fallback : v;
    }

    property color surface:    cfg("surface", "#191206")
    property color foreground: cfg("foreground", "#efe0cf")
    property color accent:     cfg("accent", "#ffba28")
    property color alarm:      cfg("alarm", "#ffb4ab")
    property real  fill:       Number(cfg("fill", 0.5))
    // A wallpaper per monitor size: SDDM's screens and fwm's outputs do not
    // share names (X11 calls DP-2 "DisplayPort-1"), but the two agree on how
    // big each one is.
    property string wallpaper: cfg("wall_" + width + "x" + height, cfg("wall", ""))

    Sddm.TextConstants { id: words }

    // ── the wallpaper ────────────────────────────────────────────────────
    Item {
        id: wallLayer
        anchors.fill: parent

        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.lighter(root.surface, 1.6) }
                GradientStop { position: 1.0; color: root.surface }
            }
        }
        Image {
            anchors.fill: parent
            source: root.wallpaper ? "file://" + root.wallpaper : ""
            fillMode: Image.PreserveAspectCrop
            asynchronous: false
            cache: false
        }
    }

    // Everything below here is the primary screen's.
    Item {
        id: stage
        anchors.fill: parent
        visible: root.primary
        enabled: root.primary

        // ── the top strip ────────────────────────────────────────────────
        property var sessionNames: []
        property int sessionIndex: sessionModel.lastIndex >= 0 ? sessionModel.lastIndex : 0
        property int sessionsSeen: 0
        Repeater {
            model: sessionModel
            delegate: Item {
                Component.onCompleted: {
                    stage.sessionNames[index] = model.name;
                    stage.sessionsSeen++;
                }
            }
        }

        property string now: ""
        Timer {
            interval: 1000; running: root.primary; repeat: true; triggeredOnStart: true
            onTriggered: stage.now = Qt.formatDateTime(new Date(), "HH:mm  •  ddd, dd.MM")
        }

        // A power pill wants a second press within three seconds: a stray
        // click on the login screen must not switch the machine off.
        property string armed: ""
        Timer { id: disarm; interval: 3000; onTriggered: stage.armed = "" }
        function power(what) {
            if (stage.armed !== what) { stage.armed = what; disarm.restart(); return; }
            stage.armed = "";
            if (what === "suspend") sddm.suspend();
            else if (what === "reboot") sddm.reboot();
            else if (what === "poweroff") sddm.powerOff();
        }

        Row {
            anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 14
            spacing: 6
            Pill {
                visible: text !== ""
                text: sddm.hostName
                surface: root.surface; foreground: root.foreground; fill: root.fill
            }
        }
        Pill {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top; anchors.topMargin: 14
            text: stage.now
            surface: root.surface; foreground: root.foreground; fill: root.fill
        }
        Row {
            anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 14
            spacing: 6
            Pill {
                visible: keyboard.layouts.length > 1
                text: visible ? keyboard.layouts[keyboard.currentLayout].shortName : ""
                clickable: true
                surface: root.surface; foreground: root.foreground; fill: root.fill
                onClicked: keyboard.currentLayout = (keyboard.currentLayout + 1) % keyboard.layouts.length
            }
            Pill {
                visible: sessionModel.rowCount() > 1
                text: stage.sessionsSeen >= 0 ? (stage.sessionNames[stage.sessionIndex] || "") : ""
                clickable: true
                surface: root.surface; foreground: root.foreground; fill: root.fill
                onClicked: stage.sessionIndex = (stage.sessionIndex + 1) % sessionModel.rowCount()
            }
            Pill {
                visible: sddm.canSuspend
                text: stage.armed === "suspend" ? "suspend?" : "☾"
                clickable: true; armed: stage.armed === "suspend"
                surface: root.surface; foreground: root.foreground; alarm: root.alarm; fill: root.fill
                onClicked: stage.power("suspend")
            }
            Pill {
                visible: sddm.canReboot
                text: stage.armed === "reboot" ? "reboot?" : "↻"
                clickable: true; armed: stage.armed === "reboot"
                surface: root.surface; foreground: root.foreground; alarm: root.alarm; fill: root.fill
                onClicked: stage.power("reboot")
            }
            Pill {
                visible: sddm.canPowerOff
                text: stage.armed === "poweroff" ? "power off?" : "⏻"
                clickable: true; armed: stage.armed === "poweroff"
                surface: root.surface; foreground: root.foreground; alarm: root.alarm; fill: root.fill
                onClicked: stage.power("poweroff")
            }
        }

        // ── the cards ────────────────────────────────────────────────────
        property int selected: userModel.lastIndex >= 0 ? userModel.lastIndex : 0
        property var bodies: []
        property bool leaving: false
        readonly property real gap: 40

        function homeX(i, w) {
            var n = cards.count, total = n * w + (n - 1) * gap;
            return (width - total) / 2 + i * (w + gap);
        }
        function homeY(h) { return height * 0.48 - h / 2; }

        function wake() { if (!world.running) world.running = true; }

        // After a login every card has left the screen for good; nothing is
        // left to move.
        function allGone() {
            for (var i = 0; i < bodies.length; i++) {
                var b = bodies[i];
                if (b.y + b.h > 0 && b.y < height) return false;
            }
            return true;
        }

        function select(i) {
            if (i < 0 || i >= cards.count) return;
            selected = i;
            var c = cards.itemAt(i);
            if (c) c.focusPassword();
        }

        Repeater {
            id: cards
            model: userModel
            delegate: Card {
                backdrop: wallLayer
                userName: model.name
                realName: model.realName
                icon: model.icon
                selected: index === stage.selected
                capsLock: keyboard.capsLock
                surface: root.surface; foreground: root.foreground
                accent: root.accent; alarm: root.alarm; fill: root.fill

                property real grabX: 0
                property real grabY: 0
                property real lastT: 0

                onChosen: stage.select(index)
                onLogin: (pw) => {
                    if (stage.leaving) return;
                    message = "";
                    stage.select(index);
                    sddm.login(userName, pw, stage.sessionIndex);
                }
                onGrabbed: (gx, gy) => {
                    var b = stage.bodies[index];
                    if (!b || stage.leaving) return;
                    b.held = true; b.vx = 0; b.vy = 0;
                    grabX = gx - b.x; grabY = gy - b.y;
                    lastT = Date.now();
                    stage.wake();
                }
                onDragged: (gx, gy) => {
                    var b = stage.bodies[index];
                    if (!b || !b.held) return;
                    var t = Date.now(), dt = Math.max(1, t - lastT) / 1000;
                    var nx = gx - grabX, ny = gy - grabY;
                    // Smoothed, so the throw is the hand's speed and not the
                    // last stray event's.
                    b.vx = b.vx * 0.5 + (nx - b.x) / dt * 0.5;
                    b.vy = b.vy * 0.5 + (ny - b.y) / dt * 0.5;
                    b.x = nx; b.y = ny;
                    lastT = t;
                    x = b.x; y = b.y;
                }
                onDropped: {
                    var b = stage.bodies[index];
                    if (!b) return;
                    // A hand that stopped before letting go threw nothing.
                    if (Date.now() - lastT > 80) { b.vx = 0; b.vy = 0; }
                    b.held = false;
                    stage.wake();
                }
            }
            onItemAdded: (i, item) => Qt.callLater(stage.build)
        }

        // Every card starts above the screen and drops into place, one a
        // moment after another — the only time the screen moves on its own.
        function build() {
            var list = [];
            for (var i = 0; i < cards.count; i++) {
                var c = cards.itemAt(i);
                if (!c) return;
                var hx = homeX(i, c.width), hy = homeY(c.height);
                list.push({ x: hx, y: -c.height - 60 - i * 90, w: c.width, h: c.height,
                            vx: 0, vy: 0, hx: hx, hy: hy,
                            held: false, spring: true, gravity: false, bounded: false });
                c.x = hx; c.y = list[i].y;
            }
            bodies = list;
            // Bounded only once inside: a card coming in from above the top
            // edge would otherwise be stopped by it.
            arrive.restart();
            select(selected);
            wake();
        }
        Timer {
            id: arrive; interval: 450
            onTriggered: {
                for (var i = 0; i < stage.bodies.length; i++) stage.bodies[i].bounded = true;
            }
        }

        FrameAnimation {
            id: world
            running: false
            onTriggered: {
                Physics.step(stage.bodies, frameTime, stage.width, stage.height);
                for (var i = 0; i < stage.bodies.length; i++) {
                    var c = cards.itemAt(i), b = stage.bodies[i];
                    if (!c || b.held) continue;
                    c.x = b.x; c.y = b.y;
                }
                if (Physics.resting(stage.bodies) || (stage.leaving && stage.allGone()))
                    running = false;
            }
        }

        onWidthChanged: relayout()
        onHeightChanged: relayout()
        function relayout() {
            for (var i = 0; i < bodies.length; i++) {
                bodies[i].hx = homeX(i, bodies[i].w);
                bodies[i].hy = homeY(bodies[i].h);
            }
            wake();
        }

        Connections {
            target: sddm

            // Wrong: a jolt down and to one side. The spring brings it straight
            // back, and the field is empty and still has the keys.
            function onLoginFailed() {
                var c = cards.itemAt(stage.selected), b = stage.bodies[stage.selected];
                if (c) {
                    c.message = words.loginFailed ? words.loginFailed : "wrong password";
                    c.clearPassword();
                    c.focusPassword();
                }
                if (b) {
                    b.held = false;
                    b.vy += 1300;
                    b.vx += (Math.random() < 0.5 ? -1 : 1) * 900;
                }
                stage.wake();
            }

            // Right: the card is flung up and away, and the others drop out of
            // the way, while the session starts.
            function onLoginSucceeded() {
                stage.leaving = true;
                for (var i = 0; i < stage.bodies.length; i++) {
                    var b = stage.bodies[i];
                    b.held = false; b.spring = false; b.bounded = false;
                    if (i === stage.selected) { b.vy = -4200; b.vx *= 0.3; }
                    else                      { b.gravity = true; b.vy = -400; }
                }
                stage.wake();
                fade.start();
            }
        }

        NumberAnimation {
            id: fade
            target: stage; property: "opacity"; to: 0; duration: 400
            easing.type: Easing.InQuad
        }
    }

    Component.onCompleted: if (primary) Qt.callLater(stage.build)
}
