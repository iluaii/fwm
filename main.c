#include "src/server.h"
#include <stdlib.h>
#include <stdio.h>
#include <wlr/util/log.h>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

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