#include "src/server.h"
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wlr/util/log.h>

int main(int argc, char *argv[]) {
    /* The only arguments fwm takes. Everything else it is told comes from the
     * config file or the control socket, so there is no option parser here and
     * an unknown flag is not worth inventing one for — it is ignored, and the
     * compositor starts, which is what somebody who typed it wanted. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("fwm %s\n", FWM_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("fwm %s — a Wayland compositor where windows are physical objects\n"
                   "\n"
                   "  fwm                start the compositor\n"
                   "  fwm --version      print the version\n"
                   "\n"
                   "Everything else is configured in ~/.config/fwm/config.toml or\n"
                   "changed at runtime with fwmctl.\n", FWM_VERSION);
            return 0;
        }
    }

    /* Pin the mmap threshold before anything allocates.
     *
     * A video wallpaper mallocs one cover-sized frame buffer per decoded frame
     * (8 MB at 1080p, 33 MB at 4K) plus a screen-sized cairo surface per
     * presented frame. glibc starts by serving those through mmap, but its
     * threshold is DYNAMIC: every freed mmap chunk raises it to that chunk's
     * size, up to 32 MB. Within a few frames the threshold swallows the frame
     * size, and from then on every buffer is carved out of a per-thread arena
     * instead — where free() can only return memory sitting at the top of the
     * heap. The result was 1 GB of resident, entirely free arena that survived
     * switching back to a still image.
     *
     * Setting the option explicitly disables that dynamic adjustment (glibc
     * documents this), so frame buffers keep going through mmap and munmap
     * hands them back to the kernel the moment they are freed. */
    mallopt(M_MMAP_THRESHOLD, 256 * 1024);

    wlr_log_init(getenv("FWM_DEBUG") ? WLR_DEBUG : WLR_INFO, NULL);

    FwmServer server;
    if (!server_init(&server)) {
        fprintf(stderr, "Failed to initialize fwm-Wayland server\n");
        return 1;
    }
    
    server_run(&server);
    server_destroy(&server);
    return 0;
}