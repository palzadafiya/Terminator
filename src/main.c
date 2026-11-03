/*
 * Terminator - First-Principles Linux Terminal Emulator & Shell in C (gnu11)
 *
 * main.c: Bootstrap, command-line argument parsing (Thread Mode vs Process Mode),
 * signal isolation setup, shared memory creation, and event loop initialization.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "shared.h"
#include "tabrt.h"
#include "gui.h"

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);

    /* GUI ignores terminal signals and SIGPIPE */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGTTIN, &sa, NULL);
    sigaction(SIGTTOU, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
}

int main(int argc, char **argv) {
    bool use_process = false;

    if (argc == 1) {
        use_process = false;
    } else if (argc == 2 && strcmp(argv[1], "useProcess=True") == 0) {
        use_process = true;
    } else {
        fprintf(stderr, "Usage: %s [useProcess=True]\n", argv[0]);
        return 2;
    }

    setup_signals();

    /* Create shared memory and history */
    Shared *sh = shared_create();
    if (!sh) {
        fprintf(stderr, "terminator: failed to initialize shared memory\n");
        return 1;
    }

    /* Initialize history persistence (~/.terminator_history) before any tab exists */
    history_init_persistence(sh);
    hist_init(sh);

    /* Create tab runtime */
    TabRuntime *rt = tabrt_create(use_process, sh);
    if (!rt) {
        fprintf(stderr, "terminator: failed to initialize tab runtime\n");
        shared_destroy(sh);
        return 1;
    }

    /* Initialize GUI */
    Gui *gui = gui_create(rt);
    if (!gui) {
        fprintf(stderr, "terminator: failed to initialize GUI\n");
        tabrt_destroy(rt);
        shared_destroy(sh);
        return 1;
    }

    /* Spawn initial tab */
    if (!tabrt_spawn(rt, ConnectionNumber(gui->dpy))) {
        fprintf(stderr, "terminator: failed to spawn initial tab\n");
        gui_destroy(gui);
        tabrt_destroy(rt);
        shared_destroy(sh);
        return 1;
    }

    /* Run event loop */
    int ret = gui_run(gui);

    /* Clean exit */
    gui_destroy(gui);
    tabrt_destroy(rt);
    shared_destroy(sh);

    return ret;
}
