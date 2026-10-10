// fwm — a Wayland compositor
// Copyright (C) 2026 Ilu
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 as
// published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// The login screen's physics: a handful of boxes, nothing more.
//
// fwm's own engine is Box2D; this is not, on purpose. A greeter has one to a
// few cards and has to come up in a frame, so what it gets is the part of
// fwm's feel that matters here — a body can be thrown, bounces off the edges
// of the screen and off the other cards — and one thing fwm's windows do not
// have: a spring that always brings a card home. That spring is what keeps it
// from being annoying. Nothing can be lost off screen or left lying on the
// floor between you and your password; whatever happens to the card, it is
// back where you type within a second.
//
// A body is a plain object: { x, y, w, h, vx, vy, hx, hy, held, spring,
// gravity, bounded }. hx/hy is home. The caller owns the items and copies
// x/y onto them after each step.

.pragma library

var K = 90.0          // spring stiffness, 1/s²
var C = 11.0          // spring damping, 1/s — a little under critical: one soft overshoot
var G = 2600.0        // gravity while falling, px/s²
var WALL = 0.55       // restitution against the screen's edges
var HIT = 0.5         // restitution between two cards
var MAX_V = 5000.0    // a throw faster than this is a glitch of the pointer, not a throw

function clampV(b) {
    var s = Math.sqrt(b.vx * b.vx + b.vy * b.vy);
    if (s > MAX_V) { b.vx *= MAX_V / s; b.vy *= MAX_V / s; }
}

function integrate(b, dt, W, H) {
    if (b.held) return;
    var ax = 0, ay = 0;
    if (b.spring) {
        ax += K * (b.hx - b.x) - C * b.vx;
        ay += K * (b.hy - b.y) - C * b.vy;
    }
    if (b.gravity) ay += G;
    b.vx += ax * dt;
    b.vy += ay * dt;
    clampV(b);
    b.x += b.vx * dt;
    b.y += b.vy * dt;

    if (!b.bounded) return;
    if (b.x < 0)           { b.x = 0;           if (b.vx < 0) b.vx = -b.vx * WALL; }
    if (b.x + b.w > W)     { b.x = W - b.w;     if (b.vx > 0) b.vx = -b.vx * WALL; }
    if (b.y < 0)           { b.y = 0;           if (b.vy < 0) b.vy = -b.vy * WALL; }
    if (b.y + b.h > H)     { b.y = H - b.h;     if (b.vy > 0) b.vy = -b.vy * WALL; }
}

// Two boxes overlapping are pushed apart along the shallower axis, and the
// part of their velocity that was bringing them together is traded the way
// two equal masses trade it. A card in the hand does not give way: it is
// the hand that is pushing.
function collide(a, b) {
    if (!a.bounded || !b.bounded) return;
    var ox = Math.min(a.x + a.w, b.x + b.w) - Math.max(a.x, b.x);
    var oy = Math.min(a.y + a.h, b.y + b.h) - Math.max(a.y, b.y);
    if (ox <= 0 || oy <= 0) return;

    var nx = 0, ny = 0, depth;
    if (ox < oy) { nx = (a.x + a.w / 2 < b.x + b.w / 2) ? -1 : 1; depth = ox; }
    else         { ny = (a.y + a.h / 2 < b.y + b.h / 2) ? -1 : 1; depth = oy; }

    // n points from b to a.
    var sa = a.held ? 0 : (b.held ? 1 : 0.5);
    var sb = b.held ? 0 : (a.held ? 1 : 0.5);
    a.x += nx * depth * sa; a.y += ny * depth * sa;
    b.x -= nx * depth * sb; b.y -= ny * depth * sb;

    var vrel = (a.vx - b.vx) * nx + (a.vy - b.vy) * ny;
    if (vrel >= 0) return;                         // already parting
    var j = -(1 + HIT) * vrel;
    if (a.held)      { b.vx -= j * nx; b.vy -= j * ny; }
    else if (b.held) { a.vx += j * nx; a.vy += j * ny; }
    else {
        a.vx += j * nx / 2; a.vy += j * ny / 2;
        b.vx -= j * nx / 2; b.vy -= j * ny / 2;
    }
}

// Advance the world by dt seconds. Substepped so a hard throw cannot pass
// through a wall or another card in one frame.
function step(bodies, dt, W, H) {
    if (dt > 1 / 20) dt = 1 / 20;
    var n = 4, h = dt / n;
    for (var s = 0; s < n; s++) {
        for (var i = 0; i < bodies.length; i++) integrate(bodies[i], h, W, H);
        for (var i2 = 0; i2 < bodies.length; i2++)
            for (var k = i2 + 1; k < bodies.length; k++) collide(bodies[i2], bodies[k]);
    }
}

// Still enough to stop asking for frames.
function resting(bodies) {
    for (var i = 0; i < bodies.length; i++) {
        var b = bodies[i];
        if (b.held) return false;
        if (Math.abs(b.vx) > 2 || Math.abs(b.vy) > 2) return false;
        if (b.spring && (Math.abs(b.hx - b.x) > 0.5 || Math.abs(b.hy - b.y) > 0.5)) return false;
        if (b.gravity) return false;
    }
    return true;
}
