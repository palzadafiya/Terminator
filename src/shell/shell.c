/*
 * shell.c: Core tab shell runtime state and command dispatcher.
 *
 * Coordinates command dispatch, continuation handling, multiWatch interception,
 * subshell execution, environment array synchronization, and graceful teardown.
 */

#define _GNU_SOURCE
#include "shell.h"
#include "multiwatch.h"
#include "exec.h"
#include "builtins.h"
#include "sink.h"
#include "../syscompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

extern char **environ;

static bool is_empty_line(const char *s) {
    if (!s) return true;
    while (*s) {
        if (!isspace((unsigned char)*s)) return false;
        s++;
    }
    return true;
}

Shell *shell_create(const char *initial_cwd, char **initial_envp, Shared *shared) {
    Shell *sh = calloc(1, sizeof(Shell));
    if (!sh) return NULL;

    if (initial_cwd && initial_cwd[0]) {
        strncpy(sh->cwd, initial_cwd, sizeof(sh->cwd) - 1);
        sh->cwd[sizeof(sh->cwd) - 1] = '\0';
    } else {
        if (!getcwd(sh->cwd, sizeof(sh->cwd))) {
            const char *home = getenv("HOME");
            if (home) {
                strncpy(sh->cwd, home, sizeof(sh->cwd) - 1);
            } else {
                strcpy(sh->cwd, "/");
            }
            sh->cwd[sizeof(sh->cwd) - 1] = '\0';
        }
    }

    vars_init(&sh->vars, initial_envp ? initial_envp : environ);
    vars_set(&sh->vars, "PWD", sh->cwd, true);

    const char *home = getenv("HOME");
    if (home) {
        vars_set(&sh->vars, "HOME", home, true);
    }

    if (!vars_get(&sh->vars, "TERM")) {
        vars_set(&sh->vars, "TERM", "xterm-256color", true);
    }

    shell_sync_envp(sh);

    sh->shared = shared;
    sh->last_status = 0;
    sh->should_exit = false;
    sh->exit_code = 0;
    sh->from_tab_fd = -1;

    sh->accum_line = NULL;
    sh->accum_len = 0;
    sh->accum_cap = 0;
    sh->in_continuation = false;

    jobs_init(&sh->jobs);

    /* Create nonblocking SIGCHLD wake-pipe for this tab */
    if (pipe2(sh->wake_pipe, O_CLOEXEC | O_NONBLOCK) == 0) {
        wake_pipe_register(sh->wake_pipe[1]);
    } else {
        sh->wake_pipe[0] = -1;
        sh->wake_pipe[1] = -1;
    }

    return sh;
}

void shell_destroy(Shell *sh) {
    if (!sh) return;

    if (sh->accum_line) {
        free(sh->accum_line);
        sh->accum_line = NULL;
    }

    if (sh->wake_pipe[1] >= 0) {
        wake_pipe_unregister(sh->wake_pipe[1]);
        close(sh->wake_pipe[1]);
    }
    if (sh->wake_pipe[0] >= 0) {
        close(sh->wake_pipe[0]);
    }

    jobs_free(&sh->jobs);
    vars_free(&sh->vars);
    vars_free_envp(sh->envp);
    free(sh);
}

void shell_sync_envp(Shell *sh) {
    if (!sh) return;
    if (sh->envp) {
        vars_free_envp(sh->envp);
    }
    sh->envp = vars_to_envp(&sh->vars);
}

int shell_execute_line(Shell *sh, const char *line, int from_tab_fd, int to_tab_fd) {
    if (!sh) return 0;
    sh->from_tab_fd = from_tab_fd;
    sh->to_tab_fd = to_tab_fd;

    if (!line || is_empty_line(line)) {
        return sh->last_status;
    }

    const char *check_exit = line;
    while (*check_exit && isspace((unsigned char)*check_exit)) check_exit++;
    if (strncmp(check_exit, "exit", 4) != 0 || (check_exit[4] != '\0' && !isspace((unsigned char)check_exit[4]))) {
        sh->exit_warned = false;
    }

    /* Requirement 7: Append non-empty command line to shared history */
    if (sh->shared) {
        /* Trim trailing newline for history storage */
        char clean_line[PROTO_MAX_PAYLOAD];
        strncpy(clean_line, line, sizeof(clean_line) - 1);
        clean_line[sizeof(clean_line) - 1] = '\0';
        size_t len = strlen(clean_line);
        while (len > 0 && (clean_line[len - 1] == '\n' || clean_line[len - 1] == '\r')) {
            clean_line[--len] = '\0';
        }
        if (len > 0) {
            history_append(sh->shared, clean_line);
        }
    }

    /* Requirement 1: Handle multiWatch at the top of command dispatch with a dedicated parser over the RAW remainder of the line */
    const char *mw_p = line;
    while (*mw_p && (*mw_p == ' ' || *mw_p == '\t')) mw_p++;
    if (strncmp(mw_p, "multiWatch", 10) == 0 &&
        (mw_p[10] == '\0' || mw_p[10] == ' ' || mw_p[10] == '\t' || mw_p[10] == '[' || mw_p[10] == '-')) {
        return multiwatch_run(sh, mw_p + 10, from_tab_fd, to_tab_fd);
    }

    sh->last_error[0] = '\0';
    Pipeline pl;
    int parse_rc = parse_line(sh, line, &pl);
    if (parse_rc != 0 || pl.n == 0) {
        if (parse_rc != 0 && sh->last_error[0] != '\0' && from_tab_fd >= 0) {
            proto_write_frame_blocking(from_tab_fd, FRAME_OUTPUT, sh->last_error, (uint32_t)strlen(sh->last_error));
        }
        pipeline_free(&pl);
        return sh->last_status;
    }

    int status = shell_execute_pipeline(sh, &pl, from_tab_fd, to_tab_fd);
    pipeline_free(&pl);

    return status;
}

int shell_execute_subshell(Shell *sh, const char *line, int stdout_fd) {
    if (!sh || !line || is_empty_line(line)) {
        return 0;
    }

    const char *mw_p = line;
    while (*mw_p && (*mw_p == ' ' || *mw_p == '\t')) mw_p++;
    if (strncmp(mw_p, "multiWatch", 10) == 0 &&
        (mw_p[10] == '\0' || mw_p[10] == ' ' || mw_p[10] == '\t' || mw_p[10] == '[' || mw_p[10] == '-')) {
        return multiwatch_run(sh, mw_p + 10, -1, -1);
    }

    sh->last_error[0] = '\0';
    Pipeline pl;
    int parse_rc = parse_line(sh, line, &pl);
    if (parse_rc != 0 || pl.n == 0) {
        if (parse_rc != 0 && sh->last_error[0] != '\0') {
            ssize_t w = write(STDERR_FILENO, sh->last_error, strlen(sh->last_error));
            (void)w;
        }
        pipeline_free(&pl);
        return sh->last_status;
    }

    int status = 0;
    if (pl.n == 1) {
        Cmd *cmd = &pl.cmds[0];

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGQUIT, &sa, NULL);
        sigaction(SIGTSTP, &sa, NULL);
        sigaction(SIGTTIN, &sa, NULL);
        sigaction(SIGTTOU, &sa, NULL);
        sigaction(SIGPIPE, &sa, NULL);
        sigaction(SIGCHLD, &sa, NULL);

        if (sh->cwd[0]) {
            if (chdir(sh->cwd) != 0) {}
        }
        if (sh->envp) {
            environ = sh->envp;
        }

        if (cmd->nredirs > 0) {
            char err_buf[512];
            if (!apply_cmd_redirections(cmd, err_buf, sizeof(err_buf))) {
                ssize_t w = write(STDERR_FILENO, err_buf, strlen(err_buf));
                (void)w;
                pipeline_free(&pl);
                _exit(1);
            }
        }
        if (cmd->argc == 0) {
            pipeline_free(&pl);
            _exit(0);
        }
        const BuiltinDef *b = builtin_find(cmd->argv[0]);
        if (b) {
            OutputSink out = sink_raw(stdout_fd);
            OutputSink err = sink_raw(STDERR_FILENO);
            status = b->fn(sh, cmd->argc, cmd->argv, &out, &err);
            _exit(status);
        } else {
            if (stdout_fd != STDOUT_FILENO) {
                dup2(stdout_fd, STDOUT_FILENO);
                close(stdout_fd);
            }
            execvp(cmd->argv[0], cmd->argv);
            int err = errno;
            char msg[512];
            if (err == ENOENT) {
                int n = snprintf(msg, sizeof(msg), "terminator: %s: command not found\n", cmd->argv[0]);
                if (n > 0) { ssize_t w = write(STDERR_FILENO, msg, (size_t)n); (void)w; }
                _exit(127);
            } else if (err == EACCES || err == EISDIR) {
                int n = snprintf(msg, sizeof(msg), "terminator: %s: Permission denied\n", cmd->argv[0]);
                if (n > 0) { ssize_t w = write(STDERR_FILENO, msg, (size_t)n); (void)w; }
                _exit(126);
            } else {
                int n = snprintf(msg, sizeof(msg), "terminator: %s: %s\n", cmd->argv[0], strerror(err));
                if (n > 0) { ssize_t w = write(STDERR_FILENO, msg, (size_t)n); (void)w; }
                _exit(126);
            }
        }
    } else {
        int num_pipes = pl.n - 1;
        int (*pipes)[2] = malloc((size_t)num_pipes * sizeof(*pipes));
        pid_t *pids = malloc((size_t)pl.n * sizeof(pid_t));
        if (!pipes || !pids) {
            free(pipes);
            free(pids);
            pipeline_free(&pl);
            return 1;
        }

        for (int j = 0; j < num_pipes; j++) {
            if (pipe2(pipes[j], O_CLOEXEC) < 0) {
                for (int k = 0; k < j; k++) {
                    close(pipes[k][0]);
                    close(pipes[k][1]);
                }
                free(pipes);
                free(pids);
                pipeline_free(&pl);
                return 1;
            }
        }

        pid_t pgid = 0;
        int forked_children = 0;

        for (int i = 0; i < pl.n; i++) {
            Cmd *cmd = &pl.cmds[i];
            pids[i] = fork();
            if (pids[i] < 0) {
                for (int j = 0; j < num_pipes; j++) {
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
                if (forked_children > 0 && pgid > 0) {
                    kill(-pgid, SIGTERM);
                    for (int k = 0; k < forked_children; k++) {
                        waitpid(pids[k], NULL, 0);
                    }
                }
                free(pipes);
                free(pids);
                pipeline_free(&pl);
                return 1;
            }
            if (pids[i] == 0) {
                if (i == 0) setpgid(0, 0);
                else setpgid(0, pgid);

                struct sigaction sa;
                memset(&sa, 0, sizeof(sa));
                sa.sa_handler = SIG_DFL;
                sigaction(SIGINT, &sa, NULL);
                sigaction(SIGQUIT, &sa, NULL);
                sigaction(SIGTSTP, &sa, NULL);
                sigaction(SIGTTIN, &sa, NULL);
                sigaction(SIGTTOU, &sa, NULL);
                sigaction(SIGPIPE, &sa, NULL);
                sigaction(SIGCHLD, &sa, NULL);

                if (sh->cwd[0]) {
                    if (chdir(sh->cwd) != 0) {}
                }
                if (sh->envp) {
                    environ = sh->envp;
                }

                if (i > 0) {
                    dup2(pipes[i - 1][0], STDIN_FILENO);
                }
                if (i < pl.n - 1) {
                    dup2(pipes[i][1], STDOUT_FILENO);
                } else {
                    if (stdout_fd != STDOUT_FILENO) {
                        dup2(stdout_fd, STDOUT_FILENO);
                    }
                }

                for (int j = 0; j < num_pipes; j++) {
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
                if (stdout_fd != STDOUT_FILENO) {
                    close(stdout_fd);
                }

                if (cmd->nredirs > 0) {
                    char err_buf[512];
                    if (!apply_cmd_redirections(cmd, err_buf, sizeof(err_buf))) {
                        ssize_t w = write(STDERR_FILENO, err_buf, strlen(err_buf));
                        (void)w;
                        _exit(1);
                    }
                }

                int max_fd = getdtablesize();
                if (max_fd < 256) max_fd = 256;
                if (max_fd > 1024) max_fd = 1024;
                for (int fd = 3; fd < max_fd; fd++) {
                    close(fd);
                }

                if (cmd->argc == 0) {
                    _exit(0);
                }

                const BuiltinDef *b = builtin_find(cmd->argv[0]);
                if (b) {
                    OutputSink out = sink_raw(STDOUT_FILENO);
                    OutputSink err = sink_raw(STDERR_FILENO);
                    int s = b->fn(sh, cmd->argc, cmd->argv, &out, &err);
                    _exit(s);
                } else {
                    execvp(cmd->argv[0], cmd->argv);
                    _exit(127);
                }
            }

            forked_children++;
            if (i == 0) pgid = pids[0];
            setpgid(pids[i], pgid);
        }

        for (int j = 0; j < num_pipes; j++) {
            close(pipes[j][0]);
            close(pipes[j][1]);
        }

        for (int i = 0; i < pl.n; i++) {
            int st = 0;
            waitpid(pids[i], &st, 0);
            if (i == pl.n - 1) {
                if (WIFEXITED(st)) status = WEXITSTATUS(st);
                else if (WIFSIGNALED(st)) status = 128 + WTERMSIG(st);
            }
        }

        free(pipes);
        free(pids);
    }

    pipeline_free(&pl);
    return status;
}
