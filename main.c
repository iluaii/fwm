#include "src/server.h"
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <wlr/util/log.h>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

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