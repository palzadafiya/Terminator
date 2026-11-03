#ifndef SHELL_H
#define SHELL_H

#include <stdbool.h>
#include <limits.h>
#include "vars.h"
#include "cmd.h"
#include "jobs.h"
#include "../shared.h"
#include "../proto.h"

#define SHELL_MAX_PATH 1024

typedef struct Shell {
    char cwd[SHELL_MAX_PATH];
    VarTable vars;
    char **envp;
    int last_status;
    bool should_exit;
    int exit_code;

    int wake_pipe[2]; /* [0]=read in tab poll, [1]=registered for SIGCHLD write */
    Shared *shared;   /* Shared memory & history handle */
    JobTable jobs;    /* Pipeline process group & job table */
    int from_tab_fd;  /* Current active GUI communication fd */
    int to_tab_fd;    /* Control fd from GUI */
    pid_t foreground_pgid; /* PGID of current foreground job (0 if none) */
    bool exit_warned; /* True if user was warned about stopped jobs */

    /* Multiline line accumulation for continuation prompt */
    char *accum_line;
    size_t accum_len;
    size_t accum_cap;
    bool in_continuation;

    char last_error[512];
} Shell;

Shell *shell_create(const char *initial_cwd, char **initial_envp, Shared *shared);
void shell_destroy(Shell *sh);

/* Rebuild shell->envp from shell->vars */
void shell_sync_envp(Shell *sh);

/* Execute command line within this tab shell context */
int shell_execute_line(Shell *sh, const char *line, int from_tab_fd, int to_tab_fd);

/* Execute subshell command line with output directed to stdout_fd */
int shell_execute_subshell(Shell *sh, const char *line, int stdout_fd);

#endif /* SHELL_H */
