/*
 * exec.c: Command and pipeline execution engine.
 *
 * Implements pipeline execution of arbitrary length:
 * - Creates N-1 pipes up front via pipe2(O_CLOEXEC) and forks N children.
 * - Establishes process group isolation (setpgid in both parent and child).
 * - Applies redirections exclusively in child processes after pipe dup2s.
 * - Parent closes all pipe write ends immediately to guarantee clean EOF / SIGPIPE.
 * - Event-driven foreground wait loop multiplexing control pipes and SIGCHLD wake-pipes.
 */

#define _GNU_SOURCE
#include "exec.h"
#include "builtins.h"
#include "sink.h"
#include "../proto.h"
#include "../syscompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

extern char **environ;

#define MAX_WAKE_PIPES 64
static volatile sig_atomic_t g_wake_fds[MAX_WAKE_PIPES];
static pthread_mutex_t g_wake_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_sigchld_installed = false;

static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    char b = 1;
    for (int i = 0; i < MAX_WAKE_PIPES; i++) {
        int fd = g_wake_fds[i];
        if (fd > 0) {
            ssize_t ignored = write(fd, &b, 1);
            (void)ignored;
        }
    }
    errno = saved_errno;
}

void sigchld_init(void) {
    pthread_mutex_lock(&g_wake_lock);
    if (!g_sigchld_installed) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sigchld_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGCHLD, &sa, NULL);
        g_sigchld_installed = true;
    }
    pthread_mutex_unlock(&g_wake_lock);
}

void wake_pipe_register(int write_fd) {
    sigchld_init();
    pthread_mutex_lock(&g_wake_lock);
    for (int i = 0; i < MAX_WAKE_PIPES; i++) {
        if (g_wake_fds[i] == 0) {
            g_wake_fds[i] = write_fd;
            break;
        }
    }
    pthread_mutex_unlock(&g_wake_lock);
}

void wake_pipe_unregister(int write_fd) {
    pthread_mutex_lock(&g_wake_lock);
    for (int i = 0; i < MAX_WAKE_PIPES; i++) {
        if (g_wake_fds[i] == write_fd) {
            g_wake_fds[i] = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_wake_lock);
}

static void drain_and_forward(int fd, int from_tab_fd) {
    char buf[4096];
    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            proto_write_frame_blocking(from_tab_fd, FRAME_OUTPUT, buf, (uint32_t)n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

/* Apply redirections for a child process. Returns false on open/dup failure. */
bool apply_cmd_redirections(Cmd *cmd, char *err_msg, size_t err_size) {
    if (!cmd || cmd->nredirs == 0) return true;

    for (int j = 0; j < cmd->nredirs; j++) {
        Redir *r = &cmd->redirs[j];
        if (r->type == REDIR_IN) {
            int fd = open(r->filename, O_RDONLY);
            if (fd < 0) {
                snprintf(err_msg, err_size, "terminator: %s: %s\n", r->filename, strerror(errno));
                return false;
            }
            int target_fd = (r->fd >= 0) ? r->fd : STDIN_FILENO;
            if (dup2(fd, target_fd) < 0) {
                snprintf(err_msg, err_size, "terminator: dup2 failed: %s\n", strerror(errno));
                close(fd);
                return false;
            }
            close(fd);
        } else if (r->type == REDIR_OUT) {
            int fd = open(r->filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd < 0) {
                snprintf(err_msg, err_size, "terminator: %s: %s\n", r->filename, strerror(errno));
                return false;
            }
            int target_fd = (r->fd >= 0) ? r->fd : STDOUT_FILENO;
            if (dup2(fd, target_fd) < 0) {
                snprintf(err_msg, err_size, "terminator: dup2 failed: %s\n", strerror(errno));
                close(fd);
                return false;
            }
            close(fd);
        } else if (r->type == REDIR_APPEND) {
            int fd = open(r->filename, O_WRONLY | O_CREAT | O_APPEND, 0666);
            if (fd < 0) {
                snprintf(err_msg, err_size, "terminator: %s: %s\n", r->filename, strerror(errno));
                return false;
            }
            int target_fd = (r->fd >= 0) ? r->fd : STDOUT_FILENO;
            if (dup2(fd, target_fd) < 0) {
                snprintf(err_msg, err_size, "terminator: dup2 failed: %s\n", strerror(errno));
                close(fd);
                return false;
            }
            close(fd);
        } else if (r->type == REDIR_DUP) {
            if (dup2(r->dup_target, r->fd) < 0) {
                snprintf(err_msg, err_size, "terminator: dup2 failed: %s\n", strerror(errno));
                return false;
            }
        }
    }
    return true;
}

/* Execute a built-in in-process without modifying stdio 0/1/2 in tab context */
static int execute_builtin_in_process(Shell *sh, Cmd *cmd, int from_tab_fd) {
    const BuiltinDef *b = builtin_find(cmd->argv[0]);
    if (!b) return 1;

    if (cmd->nredirs == 0) {
        OutputSink out = sink_framed(from_tab_fd);
        OutputSink err = sink_framed(from_tab_fd);
        return b->fn(sh, cmd->argc, cmd->argv, &out, &err);
    }

    int out_fd = -1;
    int err_fd = -1;

    for (int j = 0; j < cmd->nredirs; j++) {
        Redir *r = &cmd->redirs[j];
        char resolved_path[SHELL_MAX_PATH * 2];

        if (r->filename) {
            if (r->filename[0] == '/') {
                strncpy(resolved_path, r->filename, sizeof(resolved_path) - 1);
                resolved_path[sizeof(resolved_path) - 1] = '\0';
            } else {
                snprintf(resolved_path, sizeof(resolved_path), "%s/%s", sh->cwd, r->filename);
            }
        }

        if (r->type == REDIR_IN) {
            int fd = open(resolved_path, O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                char err_msg[512];
                int len = snprintf(err_msg, sizeof(err_msg), "terminator: %s: %s\n", r->filename, strerror(errno));
                proto_write_frame_blocking(from_tab_fd, FRAME_OUTPUT, err_msg, (uint32_t)len);
                if (out_fd >= 0) close(out_fd);
                if (err_fd >= 0) close(err_fd);
                return 1;
            }
            close(fd); /* Built-in does not read from stdin */
        } else if (r->type == REDIR_OUT) {
            int fd = open(resolved_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
            if (fd < 0) {
                char err_msg[512];
                int len = snprintf(err_msg, sizeof(err_msg), "terminator: %s: %s\n", r->filename, strerror(errno));
                proto_write_frame_blocking(from_tab_fd, FRAME_OUTPUT, err_msg, (uint32_t)len);
                if (out_fd >= 0) close(out_fd);
                if (err_fd >= 0) close(err_fd);
                return 1;
            }
            int target_fd = (r->fd >= 0) ? r->fd : 1;
            if (target_fd == 2) {
                if (err_fd >= 0) close(err_fd);
                err_fd = fd;
            } else {
                if (out_fd >= 0) close(out_fd);
                out_fd = fd;
            }
        } else if (r->type == REDIR_APPEND) {
            int fd = open(resolved_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
            if (fd < 0) {
                char err_msg[512];
                int len = snprintf(err_msg, sizeof(err_msg), "terminator: %s: %s\n", r->filename, strerror(errno));
                proto_write_frame_blocking(from_tab_fd, FRAME_OUTPUT, err_msg, (uint32_t)len);
                if (out_fd >= 0) close(out_fd);
                if (err_fd >= 0) close(err_fd);
                return 1;
            }
            int target_fd = (r->fd >= 0) ? r->fd : 1;
            if (target_fd == 2) {
                if (err_fd >= 0) close(err_fd);
                err_fd = fd;
            } else {
                if (out_fd >= 0) close(out_fd);
                out_fd = fd;
            }
        } else if (r->type == REDIR_DUP) {
            if (r->fd == 2 && r->dup_target == 1) {
                if (out_fd >= 0) {
                    if (err_fd >= 0) close(err_fd);
                    err_fd = dup(out_fd);
                }
            }
        }
    }

    OutputSink out = (out_fd >= 0) ? sink_raw(out_fd) : sink_framed(from_tab_fd);
    OutputSink err = (err_fd >= 0) ? sink_raw(err_fd) : sink_framed(from_tab_fd);

    int status = b->fn(sh, cmd->argc, cmd->argv, &out, &err);

    if (out_fd >= 0) close(out_fd);
    if (err_fd >= 0) close(err_fd);

    return status;
}

typedef struct {
    Job *job;
    Shell *sh;
} FgFrameContext;

static void fg_handle_to_tab_frame(const Frame *f, void *user_data) {
    FgFrameContext *ctx = (FgFrameContext *)user_data;
    Job *j = ctx->job;
    Shell *sh = ctx->sh;

    if (f->type == FRAME_LINE) {
        if (j->stdin_write_fd >= 0) {
            ssize_t w = write(j->stdin_write_fd, f->payload, f->len);
            (void)w;
            w = write(j->stdin_write_fd, "\n", 1);
            (void)w;
        }
    } else if (f->type == FRAME_EOF) {
        if (j->stdin_write_fd >= 0) {
            close(j->stdin_write_fd);
            j->stdin_write_fd = -1;
        }
    } else if (f->type == FRAME_INTR) {
        if (j->pgid > 1) {
            kill(-j->pgid, SIGINT);
        }
        if (j->stdin_write_fd >= 0) {
            close(j->stdin_write_fd);
            j->stdin_write_fd = -1;
        }
    } else if (f->type == FRAME_TSTP) {
        if (j->pgid > 1) {
            kill(-j->pgid, SIGTSTP);
        }
    } else if (f->type == FRAME_CLOSE) {
        if (j->pgid > 1) {
            kill(-j->pgid, SIGHUP);
            kill(-j->pgid, SIGCONT);
        }
        if (j->stdin_write_fd >= 0) {
            close(j->stdin_write_fd);
            j->stdin_write_fd = -1;
        }
        sh->should_exit = true;
    }
}

int job_wait_foreground(Shell *sh, Job *j, int from_tab_fd, int to_tab_fd) {
    if (!sh || !j) return 0;

    sh->foreground_pgid = j->pgid;
    j->foreground = true;

    FgFrameContext ctx;
    ctx.job = j;
    ctx.sh = sh;

    InBuffer to_tab_in;
    memset(&to_tab_in, 0, sizeof(to_tab_in));

    int exit_status = 0;
    bool done_or_stopped = false;

    while (!done_or_stopped && !sh->should_exit) {
        struct pollfd pfds[3 + MAX_JOBS];
        int nfds = 0;
        int to_idx = -1;
        int wake_idx = -1;
        int fg_cap_idx = -1;
        int bg_cap_indices[MAX_JOBS];
        Job *bg_jobs[MAX_JOBS];
        int num_bg_caps = 0;

        if (to_tab_fd >= 0) {
            pfds[nfds].fd = to_tab_fd;
            pfds[nfds].events = POLLIN | POLLHUP | POLLERR;
            to_idx = nfds++;
        }

        if (sh->wake_pipe[0] >= 0) {
            pfds[nfds].fd = sh->wake_pipe[0];
            pfds[nfds].events = POLLIN;
            wake_idx = nfds++;
        }

        if (j->capture_fd >= 0) {
            pfds[nfds].fd = j->capture_fd;
            pfds[nfds].events = POLLIN | POLLHUP;
            fg_cap_idx = nfds++;
        }

        for (int k = 0; k < sh->jobs.njobs; k++) {
            Job *bg = &sh->jobs.jobs[k];
            if (bg != j && bg->capture_fd >= 0) {
                pfds[nfds].fd = bg->capture_fd;
                pfds[nfds].events = POLLIN | POLLHUP;
                bg_jobs[num_bg_caps] = bg;
                bg_cap_indices[num_bg_caps] = nfds++;
                num_bg_caps++;
            }
        }

        int ret = poll(pfds, (nfds_t)nfds, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* 1. Forward background jobs output */
        for (int b = 0; b < num_bg_caps; b++) {
            int bidx = bg_cap_indices[b];
            if (pfds[bidx].revents & (POLLIN | POLLHUP | POLLERR)) {
                Job *bg = bg_jobs[b];
                char buf[4096];
                while (1) {
                    ssize_t n = read(bg->capture_fd, buf, sizeof(buf));
                    if (n > 0) {
                        proto_write_frame_blocking(from_tab_fd, FRAME_OUTPUT, buf, (uint32_t)n);
                    } else if (n < 0 && errno == EINTR) {
                        continue;
                    } else {
                        if (n == 0) {
                            close(bg->capture_fd);
                            bg->capture_fd = -1;
                        }
                        break;
                    }
                }
            }
        }

        /* 2. Forward foreground job output */
        if (fg_cap_idx >= 0 && (pfds[fg_cap_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[4096];
            while (1) {
                ssize_t n = read(j->capture_fd, buf, sizeof(buf));
                if (n > 0) {
                    proto_write_frame_blocking(from_tab_fd, FRAME_OUTPUT, buf, (uint32_t)n);
                } else if (n < 0 && errno == EINTR) {
                    continue;
                } else {
                    break;
                }
            }
        }

        /* 3. Handle wake pipe */
        if (wake_idx >= 0 && (pfds[wake_idx].revents & POLLIN)) {
            char dummy[64];
            while (read(sh->wake_pipe[0], dummy, sizeof(dummy)) > 0) {}
        }

        /* 4. Handle incoming frames from GUI */
        if (to_idx >= 0 && (pfds[to_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            bool eof_reached = false;
            proto_read_frames(to_tab_fd, &to_tab_in, fg_handle_to_tab_frame, &ctx, &eof_reached);
            if (eof_reached) {
                sh->should_exit = true;
                break;
            }
        }

        /* 5. Update statuses of all jobs */
        jobs_update_status(&sh->jobs);

        if (j->state == JOB_STOPPED) {
            done_or_stopped = true;
            exit_status = 128 + SIGTSTP;
            j->foreground = false;
            sh->foreground_pgid = 0;
            jobs_update_order(&sh->jobs, j);

            if (j->capture_fd >= 0) {
                drain_and_forward(j->capture_fd, from_tab_fd);
            }

            char notice[512];
            int len = snprintf(notice, sizeof(notice), "\n[%d]+ Stopped %s\n", j->id, j->cmd_text);
            proto_write_frame_blocking(from_tab_fd, FRAME_OUTPUT, notice, (uint32_t)len);
        } else if (j->state == JOB_DONE) {
            done_or_stopped = true;
            exit_status = j->exit_status;
            j->foreground = false;
            sh->foreground_pgid = 0;

            if (j->capture_fd >= 0) {
                drain_and_forward(j->capture_fd, from_tab_fd);
                close(j->capture_fd);
                j->capture_fd = -1;
            }
            if (j->stdin_write_fd >= 0) {
                close(j->stdin_write_fd);
                j->stdin_write_fd = -1;
            }
            jobs_remove_by_id(&sh->jobs, j->id);
        }
    }

    sh->last_status = exit_status;
    vars_set_status(&sh->vars, exit_status);
    sh->foreground_pgid = 0;
    return exit_status;
}

int shell_execute_cmd(Shell *sh, Cmd *cmd, int from_tab_fd, int to_tab_fd) {
    Pipeline pl;
    memset(&pl, 0, sizeof(pl));
    pl.cmds = cmd;
    pl.n = 1;
    pl.background = false;
    return shell_execute_pipeline(sh, &pl, from_tab_fd, to_tab_fd);
}

int shell_execute_pipeline(Shell *sh, Pipeline *pl, int from_tab_fd, int to_tab_fd) {
    if (!sh || !pl || pl->n == 0) {
        return 0;
    }

    /* Single built-in runs in-process */
    if (!pl->background && pl->n == 1 && pl->cmds[0].argc > 0) {
        const BuiltinDef *b = builtin_find(pl->cmds[0].argv[0]);
        if (b) {
            int status = execute_builtin_in_process(sh, &pl->cmds[0], from_tab_fd);
            sh->last_status = status;
            vars_set_status(&sh->vars, status);
            return status;
        }
    }

    int n = pl->n;

    /* Create capture pipe for capturing stdout/stderr */
    int capture_pipe[2];
    if (pipe2(capture_pipe, O_CLOEXEC) < 0) {
        const char *err = "terminator: failed to create capture pipe\n";
        proto_write_frame_blocking(from_tab_fd, FRAME_OUTPUT, err, (uint32_t)strlen(err));
        vars_set_status(&sh->vars, 1);
        sh->last_status = 1;
        return 1;
    }

    int fl = fcntl(capture_pipe[0], F_GETFL, 0);
    fcntl(capture_pipe[0], F_SETFL, fl | O_NONBLOCK);

    /* Allocate dynamic arrays for pipelines of arbitrary length */
    int (*pipes)[2] = NULL;
    if (n > 1) {
        pipes = malloc((size_t)(n - 1) * sizeof(*pipes));
        if (!pipes) {
            close(capture_pipe[0]);
            close(capture_pipe[1]);
            vars_set_status(&sh->vars, 1);
            sh->last_status = 1;
            return 1;
        }
    }

    pid_t *pids = malloc((size_t)n * sizeof(pid_t));
    bool *child_done = calloc((size_t)n, sizeof(bool));
    if (!pids || !child_done) {
        free(pipes);
        free(pids);
        free(child_done);
        close(capture_pipe[0]);
        close(capture_pipe[1]);
        vars_set_status(&sh->vars, 1);
        sh->last_status = 1;
        return 1;
    }

    /* Create inter-stage pipeline pipes up front with O_CLOEXEC */
    for (int j = 0; j < n - 1; j++) {
        if (pipe2(pipes[j], O_CLOEXEC) < 0) {
            close(capture_pipe[0]);
            close(capture_pipe[1]);
            for (int k = 0; k < j; k++) {
                close(pipes[k][0]);
                close(pipes[k][1]);
            }
            free(pipes);
            free(pids);
            free(child_done);
            const char *err = "terminator: failed to create pipe\n";
            proto_write_frame_blocking(from_tab_fd, FRAME_OUTPUT, err, (uint32_t)strlen(err));
            vars_set_status(&sh->vars, 1);
            sh->last_status = 1;
            return 1;
        }
    }

    /* Check if stage 0 has input redirection */
    bool stage0_has_in = false;
    for (int r = 0; r < pl->cmds[0].nredirs; r++) {
        if (pl->cmds[0].redirs[r].type == REDIR_IN) {
            stage0_has_in = true;
            break;
        }
    }

    /* Create stdin_pipe for interactive foreground command with no < */
    int stdin_pipe[2] = {-1, -1};
    if (!pl->background && !stage0_has_in) {
        if (pipe2(stdin_pipe, O_CLOEXEC) < 0) {
            stdin_pipe[0] = -1;
            stdin_pipe[1] = -1;
        }
    }

    pid_t pgid = 0;
    int forked_children = 0;

    for (int i = 0; i < n; i++) {
        Cmd *cmd = &pl->cmds[i];
        pids[i] = fork();
        if (pids[i] < 0) {
            int err = errno;
            /* Close all inter-stage pipes in parent */
            for (int j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            close(capture_pipe[1]);
            if (stdin_pipe[0] >= 0) { close(stdin_pipe[0]); stdin_pipe[0] = -1; }
            if (stdin_pipe[1] >= 0) { close(stdin_pipe[1]); stdin_pipe[1] = -1; }

            /* Terminate and reap already-forked children to prevent zombies */
            if (forked_children > 0 && pgid > 0) {
                kill(-pgid, SIGTERM);
                for (int k = 0; k < forked_children; k++) {
                    waitpid(pids[k], NULL, 0);
                }
            }
            drain_and_forward(capture_pipe[0], from_tab_fd);
            close(capture_pipe[0]);
            free(pipes);
            free(pids);
            free(child_done);

            char msg[256];
            snprintf(msg, sizeof(msg), "terminator: fork failed: %s\n", strerror(err));
            proto_write_frame_blocking(from_tab_fd, FRAME_OUTPUT, msg, (uint32_t)strlen(msg));
            vars_set_status(&sh->vars, 1);
            sh->last_status = 1;
            return 1;
        }

        if (pids[i] == 0) {
#if defined(__linux__) && defined(PR_SET_PDEATHSIG)
            prctl(PR_SET_PDEATHSIG, SIGHUP);
#endif
            /* Child process:
             * 1. setpgid(0, pgid_of_first_child)
             * 2. Reset all signal dispositions to SIG_DFL
             * 3. chdir(sh->cwd)
             * 4. environ = sh->envp
             * 5. Pipeline pipe dup2s onto 0 and 1
             * 6. Close all pipeline pipes and capture pipes
             * 7. Stdin pipe / background /dev/null
             * 8. Redirections (overrides pipes)
             * 9. Close all file descriptors > 2
             * 10. Check empty argv
             * 11. Builtin or execvp
             */
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
            if (i < n - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            } else {
                dup2(capture_pipe[1], STDOUT_FILENO);
            }
            dup2(capture_pipe[1], STDERR_FILENO);

            for (int j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            close(capture_pipe[0]);
            close(capture_pipe[1]);

            if (i == 0 && !stage0_has_in) {
                if (pl->background) {
                    int null_fd = open("/dev/null", O_RDONLY);
                    if (null_fd >= 0) {
                        dup2(null_fd, STDIN_FILENO);
                        close(null_fd);
                    }
                } else if (stdin_pipe[0] >= 0) {
                    dup2(stdin_pipe[0], STDIN_FILENO);
                }
            }

            if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
            if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);

            /* Apply redirections in child (overrides pipeline ends) */
            char err_msg[512];
            if (!apply_cmd_redirections(cmd, err_msg, sizeof(err_msg))) {
                ssize_t w = write(STDERR_FILENO, err_msg, strlen(err_msg));
                (void)w;
                _exit(1);
            }

            /* Explicitly close all unused descriptors > 2 */
            int max_fd = getdtablesize();
            if (max_fd < 256) max_fd = 256;
            if (max_fd > 1024) max_fd = 1024;
            for (int fd = 3; fd < max_fd; fd++) {
                close(fd);
            }

            /* Empty command with only redirections */
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
                int err = errno;
                char msg[512];
                if (err == ENOENT) {
                    int nwritten = snprintf(msg, sizeof(msg), "terminator: %s: command not found\n", cmd->argv[0]);
                    if (nwritten > 0) { ssize_t w = write(STDERR_FILENO, msg, (size_t)nwritten); (void)w; }
                    _exit(127);
                } else if (err == EACCES || err == EISDIR) {
                    int nwritten = snprintf(msg, sizeof(msg), "terminator: %s: Permission denied\n", cmd->argv[0]);
                    if (nwritten > 0) { ssize_t w = write(STDERR_FILENO, msg, (size_t)nwritten); (void)w; }
                    _exit(126);
                } else {
                    int nwritten = snprintf(msg, sizeof(msg), "terminator: %s: %s\n", cmd->argv[0], strerror(err));
                    if (nwritten > 0) { ssize_t w = write(STDERR_FILENO, msg, (size_t)nwritten); (void)w; }
                    _exit(126);
                }
            }
        }

        forked_children++;
        if (i == 0) pgid = pids[0];
        setpgid(pids[i], pgid);
    }

    /* Immediately close all inter-stage pipes in the parent */
    for (int j = 0; j < n - 1; j++) {
        close(pipes[j][0]);
        close(pipes[j][1]);
    }
    close(capture_pipe[1]);

    if (stdin_pipe[0] >= 0) {
        close(stdin_pipe[0]);
        stdin_pipe[0] = -1;
    }

    if (pl->background) {
        Job *j = jobs_add(&sh->jobs, pgid, pids, n, pl->text ? pl->text : "", false, capture_pipe[0], -1);
        char bg_msg[64];
        int blen = snprintf(bg_msg, sizeof(bg_msg), "[%d] %d\n", j ? j->id : 1, (int)pids[n - 1]);
        proto_write_frame_blocking(from_tab_fd, FRAME_OUTPUT, bg_msg, (uint32_t)blen);

        free(pipes);
        free(pids);
        free(child_done);

        sh->last_status = 0;
        vars_set_status(&sh->vars, 0);
        return 0;
    }

    /* Foreground job */
    Job *j = jobs_add(&sh->jobs, pgid, pids, n, pl->text ? pl->text : "", true, capture_pipe[0], stdin_pipe[1]);
    int status = job_wait_foreground(sh, j, from_tab_fd, to_tab_fd);

    free(pipes);
    free(pids);
    free(child_done);

    return status;
}
