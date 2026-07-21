/* fwmctl — talk to a running fwm compositor.
 *
 *   fwmctl state
 *   fwmctl windows
 *   fwmctl dispatch view:3
 *   fwmctl reload
 *
 * Deliberately dependency-free (no JSON library, no wlroots): it joins its
 * arguments into one request line, writes it to the compositor's control
 * socket and copies the reply to stdout. Pipe it through jq for anything
 * fancier — that is the whole point of replying in JSON.
 *
 * Socket: $FWM_SOCKET, else $XDG_RUNTIME_DIR/fwm-$WAYLAND_DISPLAY.sock. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static int socket_path(char *out, size_t cap) {
    const char *explicit_path = getenv("FWM_SOCKET");
    if (explicit_path && *explicit_path) {
        if (strlen(explicit_path) >= cap) return -1;
        strcpy(out, explicit_path);
        return 0;
    }

    const char *dir = getenv("XDG_RUNTIME_DIR");
    const char *wl = getenv("WAYLAND_DISPLAY");
    if (!dir || !*dir) dir = "/tmp";
    if (!wl || !*wl) {
        fprintf(stderr, "fwmctl: no FWM_SOCKET and no WAYLAND_DISPLAY — "
                        "is fwm running, and are you inside its session?\n");
        return -1;
    }
    int n = snprintf(out, cap, "%s/fwm-%s.sock", dir, wl);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        fprintf(stderr,
            "usage: fwmctl <command> [args...]\n"
            "\n"
            "  state              compositor state as JSON\n"
            "  windows            open windows as JSON\n"
            "  config             every settable option, with values and ranges\n"
            "  get <name>         read one option, e.g. physics.gravity\n"
            "  set <name> <val>   change one option for this session only\n"
            "  dispatch <action>  run a keybind action (same names as config.toml)\n"
            "  reload             reload the config, discarding every `set`\n"
            "  version            IPC protocol version\n"
            "  subscribe [events] stream events as JSON lines until killed\n"
            "\n"
            "`set` never writes config.toml: the file stays the source of truth,\n"
            "so `reload` (or super+shift+r) puts everything back.\n"
            "\n"
            "subscribe takes all events by default, or a comma-separated subset:\n"
            "  window_open window_close window_focus window_title\n"
            "  desktop mode gravity config_reload\n"
            "\n"
            "Pairing it with `dispatch` is the whole plugin story — no shared\n"
            "address space, so a script that crashes takes nothing with it:\n"
            "\n"
            "  fwmctl subscribe window_open | while read -r ev; do\n"
            "      echo \"$ev\" | grep -q mpv && fwmctl dispatch toggle_float\n"
            "  done\n");
        return argc < 2 ? 1 : 0;
    }

    char path[108];
    if (socket_path(path, sizeof(path)) < 0) return 1;

    /* Join argv into one line. The compositor splits off the first word and
     * treats the rest as the argument, so quoting is never needed. */
    char req[4096];
    size_t len = 0;
    for (int i = 1; i < argc; i++) {
        int n = snprintf(req + len, sizeof(req) - len, "%s%s",
                         i > 1 ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(req) - len) {
            fprintf(stderr, "fwmctl: command too long\n");
            return 1;
        }
        len += (size_t)n;
    }
    if (len + 2 > sizeof(req)) { fprintf(stderr, "fwmctl: command too long\n"); return 1; }
    req[len++] = '\n';

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("fwmctl: socket"); return 1; }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    memcpy(addr.sun_path, path, strlen(path) + 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "fwmctl: cannot reach fwm at %s: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }

    for (size_t off = 0; off < len; ) {
        ssize_t n = write(fd, req + off, len - off);
        if (n <= 0) { perror("fwmctl: write"); close(fd); return 1; }
        off += (size_t)n;
    }

    /* The compositor closes after one reply, so read to EOF. `subscribe` is
     * the exception: it never closes, and the flush is what makes this usable
     * in a pipeline — stdout is block-buffered when it is not a terminal, so
     * without it a `| while read` loop would see nothing for 4 KiB at a time. */
    char buf[8192];
    ssize_t n;
    int had_output = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)n, stdout);
        fflush(stdout);
        had_output = 1;
    }
    close(fd);

    if (!had_output) {
        fprintf(stderr, "fwmctl: no reply\n");
        return 1;
    }
    return 0;
}
