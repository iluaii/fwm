# Changelog

## [0.1.0] - 2026-06-24

### Added
- Lightweight X11 window manager written in C with physics-based window handling
- Window drag and collision physics with momentum preservation
- Multi-desktop support with switchable layouts
- Tiling layout mode with configurable master/stack ratio
- Physics-based movement mode with window collisions
- Window fullscreen support (EWMH _NET_WM_STATE_FULLSCREEN compliance)
- Focus follow mouse functionality
- Configurable keybind system with key/button event handling
- Window decorations with octagon shape via XShape extension
- Custom tray with hexagonal shape for window indicators
- Window close functionality via XKillClient
- Lock mask handling (Mod2/Lock/Mod5) for proper key grabs
- Double-buffered tray rendering
- Installation script (install.sh)

### Features
- Per-desktop mode configuration (physics/tiling/normal)
- Focused window tracking with visual highlighting
- Drag window through collisions with Super+Shift+LMB binding
- Real window name display in tray via XFetchName
- Proper window raise order management
- Motion event coalescing for smooth dragging
- Resize clamping to screen boundaries

### Technical
- Clean C codebase with modular design
- Configuration via config.h and defines.h headers
- X11 Xlib-based implementation
- Support for Linux/Unix X11 environments

### Project Setup
- MIT License
- Topics: X11 WM, window manager, physics engine, Linux
