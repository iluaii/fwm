# fwm (Physics Window Manager)

`fwm` is a lightweight X11 window manager written in C where windows are treated as physical objects. Instead of strict, pixel-perfect tiling, windows here possess **mass, momentum, inertia, and velocity**. You can throw them, push them, and let them slide.

> ⚠️ **Status:** Early development. Basic window physics and screen edge bouncing are implemented. Window-to-window collisions are coming soon.

---

## 🚀 Features

* **Physics-Driven Elements:** Windows have inertia and speed. Throw a window and watch it slide and bounce.
* **Screen Edge Bouncing:** Windows bounce off screen boundaries (with configurable paddings/margins).
* **Granular Control:** Toggle physics completely or pin specific windows using keybindings when you need to get real work done.
* **Extensible Architecture:** Designed to eventually treat separate UI elements as independent "windows" with their own physics via custom configuration syntax.

---

## 🛠 Requirements

To build `fwm`, you need the X11 development libraries.