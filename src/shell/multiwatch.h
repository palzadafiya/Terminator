#ifndef TERMINATOR_MULTIWATCH_H
#define TERMINATOR_MULTIWATCH_H

#include "shell.h"
#include <stdbool.h>

#define MULTIWATCH_DASH_LINE "----------------------------------------------------\n"
#define MULTIWATCH_USAGE     "usage: multiWatch [\"cmd1\", \"cmd2\", ...]\n"
#define MULTIWATCH_MAX_OUTPUT (1024 * 1024) /* 1 MiB output cap per run */

/*
 * Parses the raw argument string following 'multiWatch'.
 * Supports optional '-n <seconds>' and a bracketed list of quoted strings.
 * Returns 0 on success, -1 on syntax error.
 */
int multiwatch_parse_args(const char *raw_args, double *out_interval, char ***out_cmds, int *out_ncmds);

/*
 * Runs the multiWatch loop over the specified raw arguments.
 * Manages parallel children, polling, output printing, rescheduling, and Ctrl+C.
 */
int multiwatch_run(Shell *sh, const char *raw_args, int from_tab_fd, int to_tab_fd);

#endif /* TERMINATOR_MULTIWATCH_H */
