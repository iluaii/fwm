# Changelog

All notable changes to this project will be documented in this file.

## [0.2.0] - 2026-07-07

### Added
- **Welcome overlay** — improved onboarding for new users with display of key controls and features
- **Keybind hints system** — press `Super+?` to display available keybindings at any time
- **Enhanced physics debugging** — improved window velocity and angle display in status tray
- **Gravity system refinements** — better support for space mode and Earth gravity modes
- **Window property improvements** — better handling of window names and states

### Changed
- **Status tray redesign** — hexagon-shaped overlay now displays more information (speed, angle, mass, flying state)
- **Physics tick rate optimization** — improved frame timing for smoother animations
- **Collision detection refinement** — better handling of edge cases and window boundaries
- **Code organization** — split UI components into dedicated modules (tray, decorations, hints, welcome)

### Fixed
- **Window visibility on desktop switching** — fixed issues where windows sometimes didn't appear on correct desktop
- **Collision edge cases** — fixed floating-point precision issues in collision detection
- **Memory management** — improved cleanup of window resources when windows are closed
- **Xephyr testing improvements** — better compatibility with testing setup

### Technical
- Refactored window manager core (`wm.c/h`) for better maintainability
- Improved physics engine accuracy with better time step handling
- Enhanced BSP tiling implementation for more predictable behavior
- Better X11 event handling and window property queries

## [0.1.0] - 2026-06-24

### Initial Release
- Core physics-based window manager for X11
- Physics simulation with mass, velocity, and friction
- Window collision system with elastic bounces
- Drag-to-throw mechanics with configurable velocity
- 10 virtual desktops with absolute positioning
- BSP tiling mode toggle
- EWMH fullscreen support
- Focus-follows-mouse
- Keybindings system with Super modifier
- Window decorations with chamfered corners
- Basic status tray
- Installation script with package manager detection

---

### Legend
- **Added** — new features
- **Changed** — changes in existing functionality
- **Fixed** — bug fixes
- **Technical** — internal improvements without user-facing changes
