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

#ifndef FWM_SANDBOX_H
#define FWM_SANDBOX_H

struct FwmServer;

/* security-context-v1, and the one thing it is for.
 *
 * A Flatpak app does not connect to fwm directly: the sandbox holds one end of
 * a socket it made itself, tells fwm through this protocol what the thing on
 * the other end is (engine "org.flatpak", the app id, an instance id), and only
 * then lets the app speak. Everything that arrives on that socket is a
 * sandboxed client, and stays one for its whole life — the marking is done
 * before the first byte and can never be taken back.
 *
 * Creating the global is only half of it. The protocol carries no rule about
 * what a sandboxed client may do; it just says which clients are sandboxed, and
 * the compositor decides. fwm's decision is a filter on the globals themselves:
 * a client with a security context never sees the privileged ones, so it cannot
 * bind them, and it cannot bind them without help — a global that is filtered
 * out is simply not in the registry that client is sent.
 *
 * The privileged set is the protocols that read or drive the session as a
 * whole rather than the client's own window: the clipboard behind everyone's
 * back, screen capture, the panel shell, the window list, the workspaces,
 * global keys, the backlight and gamma ramps, the lock screen — and this
 * protocol itself, so nothing sandboxed can claim to be a sandbox. A screen
 * recorder or a panel is meant to have all of that, which is exactly why it
 * must not be handed to a chat client that happens to ship as a Flatpak. Such
 * an app asks the portal instead, and the portal asks the user.
 *
 * Ordinary clients — everything with no security context, which is everything
 * started from the session itself — are unaffected: the filter answers true
 * before it has looked at anything. */
void sandbox_init(struct FwmServer *server);

#endif /* FWM_SANDBOX_H */
