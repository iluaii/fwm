#ifndef FWM_IPC_H
#define FWM_IPC_H

struct FwmServer;
typedef struct FwmIpc FwmIpc;

/* Control socket: lets anything outside the compositor read its state and run
 * the same actions the keybinds run, so customisation no longer requires
 * editing C and rebuilding. Path is $XDG_RUNTIME_DIR/fwm-<wl_socket>.sock and
 * is exported as FWM_SOCKET for children (fwmctl reads either).
 *
 * Returns NULL if the socket could not be created; that is NOT fatal, the
 * compositor runs fine without it. */
FwmIpc *ipc_create(struct FwmServer *server, const char *wl_socket);
void ipc_destroy(FwmIpc *ipc);

#endif /* FWM_IPC_H */
