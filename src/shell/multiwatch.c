/*
 * multiwatch.c: Concurrent periodic command runner.
 *
 * Implements multiWatch [-n <seconds>] ["cmd1", "cmd2", ...]:
 * - Intercepts and parses bracketed command lists with interval support.
 * - Spawns all commands concurrently in isolated subshells with dedicated pipes.
 * - Single non-blocking poll() loop with monotonic deadlines (zero busy-waiting).
 * - Emits atomic output blocks with timestamps and 52-character dash separators.
 * - Graceful two-stage signal teardown (SIGTERM -> SIGKILL) on Ctrl+C.
 */

#define _GNU_SOURCE
#include "multiwatch.h"
#include "../proto.h"
#include "../syscompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <ctype.h>

extern char **environ;

typedef enum {
    MW_IDLE,
    MW_RUNNING
} MWState;

typedef struct {
    char *cmd_text;
    MWState state;
    pid_t pid;
    pid_t pgid;
    int pipe_read_fd;
    int pipe_write_fd;
    time_t run_start_wall;
    struct timespec run_start_mono;
    struct timespec next_start_mono;
    char *output_buf;
    size_t output_len;
    size_t output_cap;
    bool truncated;
} MWCmd;

typedef struct {
    bool got_intr;
    bool got_close;
} MWFrameContext;

/* Helper to convert timespec to milliseconds */
static inline int64_t timespec_to_ms(const struct timespec *ts) {
    return (int64_t)ts->tv_sec * 1000LL + (int64_t)ts->tv_nsec / 1000000LL;
}

/* Helper to convert milliseconds to timespec */
static inline struct timespec ms_to_timespec(int64_t ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000LL);
    ts.tv_nsec = (long)((ms % 1000LL) * 1000000LL);
    return ts;
}

static inline void get_monotonic_time(struct timespec *ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

int multiwatch_parse_args(const char *raw_args, double *out_interval, char ***out_cmds, int *out_ncmds) {
    if (!raw_args || !out_interval || !out_cmds || !out_ncmds) {
        return -1;
    }

    *out_interval = 2.0; /* Default 2 seconds */
    *out_cmds = NULL;
    *out_ncmds = 0;

    const char *p = raw_args;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;

    /* Check optional -n <seconds> or -n<seconds> */
    if (*p == '-' && *(p + 1) == 'n' && (isspace((unsigned char)*(p + 2)) || isdigit((unsigned char)*(p + 2)) || *(p + 2) == '.' || *(p + 2) == '\0')) {
        p += 2;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (*p == '\0') return -1;
        char *endptr = NULL;
        double val = strtod(p, &endptr);
        if (endptr == p || val <= 0.0) {
            return -1;
        }
        *out_interval = val;
        p = endptr;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    }

    /* Next character must be '[' */
    if (*p != '[') {
        return -1;
    }
    p++; /* Skip '[' */

    char **cmds = NULL;
    int ncmds = 0;
    int cap = 0;

    while (*p) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (*p == ']') {
            p++; /* End of list */
            break;
        }
        if (*p != '"' && *p != '\'') {
            goto fail;
        }

        char quote = *p++;
        char buf[PROTO_MAX_PAYLOAD];
        size_t bi = 0;

        while (*p && *p != quote) {
            if (*p == '\\') {
                p++;
                if (!*p) goto fail; /* Unterminated escape */
                if (*p == quote || *p == '\\') {
                    if (bi + 1 < sizeof(buf)) buf[bi++] = *p;
                } else {
                    if (bi + 2 < sizeof(buf)) {
                        buf[bi++] = '\\';
                        buf[bi++] = *p;
                    }
                }
                p++;
            } else {
                if (bi + 1 < sizeof(buf)) buf[bi++] = *p;
                p++;
            }
        }

        if (*p != quote) {
            goto fail; /* Unterminated quote */
        }
        p++; /* Skip closing quote */
        buf[bi] = '\0';

        if (ncmds >= cap) {
            cap = cap == 0 ? 4 : cap * 2;
            char **new_cmds = realloc(cmds, (size_t)cap * sizeof(char *));
            if (!new_cmds) goto fail;
            cmds = new_cmds;
        }
        cmds[ncmds] = strdup(buf);
        if (!cmds[ncmds]) goto fail;
        ncmds++;

        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (*p == ',') {
            p++; /* Skip comma */
            const char *peek = p;
            while (*peek && (*peek == ' ' || *peek == '\t' || *peek == '\r' || *peek == '\n')) peek++;
            if (*peek == ']') {
                goto fail; /* Trailing comma before ']' is malformed */
            }
        } else if (*p == ']') {
            p++; /* Closing bracket */
            break;
        } else {
            goto fail; /* Expected ',' or ']' */
        }
    }

    if (ncmds == 0) {
        goto fail; /* At least one command required */
    }

    /* Check trailing characters after ']' */
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (*p != '\0') {
        goto fail; /* Unexpected trailing characters */
    }

    *out_cmds = cmds;
    *out_ncmds = ncmds;
    return 0;

fail:
    if (cmds) {
        for (int i = 0; i < ncmds; i++) {
            free(cmds[i]);
        }
        free(cmds);
    }
    return -1;
}

static void mw_print_block(MWCmd *cmd, int from_tab_fd) {
    char header[1024];
    int hlen = snprintf(header, sizeof(header), "\"%s\", %ld:\n", cmd->cmd_text, (long)cmd->run_start_wall);
    if (hlen < 0) return;

    const char *dash = MULTIWATCH_DASH_LINE;
    size_t dash_len = strlen(dash);

    bool needs_nl = false;
    if (cmd->output_len > 0 && cmd->output_buf[cmd->output_len - 1] != '\n') {
        needs_nl = true;
    }

    const char *trunc_msg = "[output truncated]\n";
    size_t trunc_len = cmd->truncated ? strlen(trunc_msg) : 0;

    size_t total_len = (size_t)hlen + dash_len + cmd->output_len + (needs_nl ? 1 : 0) + trunc_len + dash_len;

    char *block = malloc(total_len + 1);
    if (!block) return;

    char *p = block;
    memcpy(p, header, (size_t)hlen);
    p += hlen;

    memcpy(p, dash, dash_len);
    p += dash_len;

    if (cmd->output_len > 0) {
        memcpy(p, cmd->output_buf, cmd->output_len);
        p += cmd->output_len;
    }

    if (needs_nl) {
        *p++ = '\n';
    }

    if (cmd->truncated) {
        memcpy(p, trunc_msg, trunc_len);
        p += trunc_len;
    }

    memcpy(p, dash, dash_len);
    p += dash_len;
    *p = '\0';

    if (from_tab_fd >= 0) {
        size_t sent = 0;
        while (sent < total_len) {
            size_t chunk = total_len - sent;
            if (chunk > PROTO_MAX_PAYLOAD) chunk = PROTO_MAX_PAYLOAD;
            proto_write_frame_blocking(from_tab_fd, FRAME_OUTPUT, block + sent, (uint32_t)chunk);
            sent += chunk;
        }
    } else {
        ssize_t w = write(STDOUT_FILENO, block, total_len);
        (void)w;
    }

    free(block);
}

static bool mw_spawn_cmd(Shell *sh, MWCmd *cmd, MWCmd *all_cmds, int ncmds) {
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) < 0) {
        return false;
    }

    cmd->pipe_read_fd = pfd[0];
    cmd->pipe_write_fd = pfd[1];

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        cmd->pipe_read_fd = -1;
        cmd->pipe_write_fd = -1;
        return false;
    }

    if (pid == 0) {
        /* Child process:
         * 1. setpgid(0, 0)
         * 2. Reset signals to SIG_DFL
         * 3. chdir(sh->cwd)
         * 4. environ = sh->envp
         * 5. Redirect stdout + stderr to pipefd[1]
         * 6. Close all other pipe fds and read end
         * 7. Close fds > 2
         * 8. Execute subshell
         */
        setpgid(0, 0);

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

        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);

        close(pfd[0]);
        close(pfd[1]);

        for (int j = 0; j < ncmds; j++) {
            if (all_cmds[j].pipe_read_fd >= 0) close(all_cmds[j].pipe_read_fd);
            if (all_cmds[j].pipe_write_fd >= 0) close(all_cmds[j].pipe_write_fd);
        }

        int max_fd = getdtablesize();
        if (max_fd < 256) max_fd = 256;
        if (max_fd > 1024) max_fd = 1024;
        for (int fd = 3; fd < max_fd; fd++) {
            close(fd);
        }

        int st = shell_execute_subshell(sh, cmd->cmd_text, STDOUT_FILENO);
        _exit(st);
    }

    /* Parent */
    cmd->pid = pid;
    cmd->pgid = pid;
    setpgid(pid, pid);

    close(pfd[1]);
    cmd->pipe_write_fd = -1;

    int fl = fcntl(cmd->pipe_read_fd, F_GETFL, 0);
    fcntl(cmd->pipe_read_fd, F_SETFL, fl | O_NONBLOCK);

    get_monotonic_time(&cmd->run_start_mono);
    cmd->run_start_wall = time(NULL);
    cmd->state = MW_RUNNING;
    cmd->output_len = 0;
    cmd->truncated = false;

    return true;
}

static void mw_handle_to_tab_frame(const Frame *f, void *user_data) {
    MWFrameContext *ctx = (MWFrameContext *)user_data;
    if (f->type == FRAME_INTR) {
        ctx->got_intr = true;
    } else if (f->type == FRAME_CLOSE) {
        ctx->got_close = true;
    } else if (f->type == FRAME_LINE) {
        /* Extra LINE frames are ignored while multiWatch runs (Requirement 7) */
    }
}

static bool mw_has_running_children(MWCmd *cmds, int ncmds) {
    for (int i = 0; i < ncmds; i++) {
        if (cmds[i].state == MW_RUNNING && cmds[i].pid > 0) {
            return true;
        }
    }
    return false;
}

int multiwatch_run(Shell *sh, const char *raw_args, int from_tab_fd, int to_tab_fd) {
    double interval_sec = 2.0;
    char **parsed_cmds = NULL;
    int ncmds = 0;

    if (multiwatch_parse_args(raw_args, &interval_sec, &parsed_cmds, &ncmds) != 0) {
        const char *usage = MULTIWATCH_USAGE;
        if (from_tab_fd >= 0) {
            proto_write_frame_blocking(from_tab_fd, FRAME_OUTPUT, usage, (uint32_t)strlen(usage));
        } else {
            ssize_t w = write(STDERR_FILENO, usage, strlen(usage));
            (void)w;
        }
        sh->last_status = 1;
        vars_set_status(&sh->vars, 1);
        return 1;
    }

    int64_t interval_ms = (int64_t)(interval_sec * 1000.0);
    if (interval_ms <= 0) interval_ms = 1;

    MWCmd *cmds = calloc((size_t)ncmds, sizeof(MWCmd));
    if (!cmds) {
        for (int i = 0; i < ncmds; i++) free(parsed_cmds[i]);
        free(parsed_cmds);
        sh->last_status = 1;
        vars_set_status(&sh->vars, 1);
        return 1;
    }

    for (int i = 0; i < ncmds; i++) {
        cmds[i].cmd_text = parsed_cmds[i];
        cmds[i].state = MW_IDLE;
        cmds[i].pid = -1;
        cmds[i].pgid = 0;
        cmds[i].pipe_read_fd = -1;
        cmds[i].pipe_write_fd = -1;
        cmds[i].output_cap = 4096;
        cmds[i].output_buf = malloc(cmds[i].output_cap);
        cmds[i].output_len = 0;
        cmds[i].truncated = false;
        cmds[i].next_start_mono.tv_sec = 0;
        cmds[i].next_start_mono.tv_nsec = 0;
    }
    free(parsed_cmds); /* Transfer ownership of strings to cmds[i].cmd_text */

    /* Initial spawn of all commands in parallel */
    for (int i = 0; i < ncmds; i++) {
        mw_spawn_cmd(sh, &cmds[i], cmds, ncmds);
    }

    MWFrameContext ctx;
    ctx.got_intr = false;
    ctx.got_close = false;

    InBuffer to_tab_in;
    memset(&to_tab_in, 0, sizeof(to_tab_in));

    int exit_status = 0;

    /* Main multiWatch event loop */
    while (!ctx.got_intr && !ctx.got_close) {
        struct timespec now;
        get_monotonic_time(&now);
        int64_t now_ms = timespec_to_ms(&now);

        /* 1. Check idle commands whose start deadline has arrived */
        for (int i = 0; i < ncmds; i++) {
            if (cmds[i].state == MW_IDLE) {
                int64_t next_ms = timespec_to_ms(&cmds[i].next_start_mono);
                if (now_ms >= next_ms) {
                    mw_spawn_cmd(sh, &cmds[i], cmds, ncmds);
                }
            }
        }

        /* 2. Compute earliest timeout for poll() */
        int timeout_ms = -1;
        int64_t min_diff_ms = -1;

        get_monotonic_time(&now);
        now_ms = timespec_to_ms(&now);

        for (int i = 0; i < ncmds; i++) {
            if (cmds[i].state == MW_IDLE) {
                int64_t next_ms = timespec_to_ms(&cmds[i].next_start_mono);
                int64_t diff = next_ms - now_ms;
                if (diff <= 0) {
                    min_diff_ms = 0;
                    break;
                }
                if (min_diff_ms < 0 || diff < min_diff_ms) {
                    min_diff_ms = diff;
                }
            }
        }

        if (min_diff_ms >= 0) {
            if (min_diff_ms > INT_MAX) timeout_ms = INT_MAX;
            else timeout_ms = (int)min_diff_ms;
        } else {
            timeout_ms = -1; /* All commands running, block until fd activity */
        }

        /* 3. Build pollfd array */
        struct pollfd pfds[2 + ncmds];
        int nfds = 0;
        int to_idx = -1;
        int wake_idx = -1;
        int cmd_pfd_idx[ncmds];
        for (int i = 0; i < ncmds; i++) cmd_pfd_idx[i] = -1;

        if (to_tab_fd >= 0) {
            pfds[nfds].fd = to_tab_fd;
            pfds[nfds].events = POLLIN;
            to_idx = nfds++;
        }

        if (sh->wake_pipe[0] >= 0) {
            pfds[nfds].fd = sh->wake_pipe[0];
            pfds[nfds].events = POLLIN;
            wake_idx = nfds++;
        }

        for (int i = 0; i < ncmds; i++) {
            if (cmds[i].state == MW_RUNNING && cmds[i].pipe_read_fd >= 0) {
                pfds[nfds].fd = cmds[i].pipe_read_fd;
                pfds[nfds].events = POLLIN | POLLHUP | POLLERR;
                cmd_pfd_idx[i] = nfds++;
            }
        }

        int ret = poll(pfds, (nfds_t)nfds, timeout_ms);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Check GUI input frames (Ctrl+C, Close, etc.) */
        if (to_idx >= 0 && (pfds[to_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            bool eof_reached = false;
            proto_read_frames(to_tab_fd, &to_tab_in, mw_handle_to_tab_frame, &ctx, &eof_reached);
            if (ctx.got_intr || ctx.got_close) {
                break;
            }
        }

        /* Check wake pipe */
        if (wake_idx >= 0 && (pfds[wake_idx].revents & POLLIN)) {
            char dummy[64];
            while (read(sh->wake_pipe[0], dummy, sizeof(dummy)) > 0) {}
        }

        /* Check running command pipes */
        for (int i = 0; i < ncmds; i++) {
            int pidx = cmd_pfd_idx[i];
            if (pidx < 0) continue;

            short revents = pfds[pidx].revents;
            if (revents & (POLLIN | POLLHUP | POLLERR)) {
                char buf[4096];
                bool eof = false;

                while (1) {
                    ssize_t n = read(cmds[i].pipe_read_fd, buf, sizeof(buf));
                    if (n > 0) {
                        if (cmds[i].output_len + (size_t)n <= MULTIWATCH_MAX_OUTPUT) {
                            if (cmds[i].output_len + (size_t)n >= cmds[i].output_cap) {
                                size_t new_cap = cmds[i].output_cap * 2;
                                while (new_cap <= cmds[i].output_len + (size_t)n) new_cap *= 2;
                                char *new_buf = realloc(cmds[i].output_buf, new_cap);
                                if (new_buf) {
                                    cmds[i].output_buf = new_buf;
                                    cmds[i].output_cap = new_cap;
                                }
                            }
                            if (cmds[i].output_buf && cmds[i].output_len + (size_t)n < cmds[i].output_cap) {
                                memcpy(cmds[i].output_buf + cmds[i].output_len, buf, (size_t)n);
                                cmds[i].output_len += (size_t)n;
                            }
                        } else {
                            cmds[i].truncated = true;
                            if (cmds[i].output_len < MULTIWATCH_MAX_OUTPUT && cmds[i].output_buf) {
                                size_t fit = MULTIWATCH_MAX_OUTPUT - cmds[i].output_len;
                                memcpy(cmds[i].output_buf + cmds[i].output_len, buf, fit);
                                cmds[i].output_len += fit;
                            }
                        }
                    } else if (n == 0) {
                        eof = true;
                        break;
                    } else {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            if (revents & POLLHUP) {
                                eof = true;
                            }
                            break;
                        }
                        eof = true;
                        break;
                    }
                }

                if (eof) {
                    /* EOF reached: child finished writing */
                    close(cmds[i].pipe_read_fd);
                    cmds[i].pipe_read_fd = -1;

                    /* Reap child */
                    int st = 0;
                    waitpid(cmds[i].pid, &st, 0);
                    cmds[i].pid = -1;
                    cmds[i].pgid = 0;

                    /* Print atomic output block */
                    mw_print_block(&cmds[i], from_tab_fd);

                    /* Schedule next start at run_start + interval (Requirement 4) */
                    int64_t start_ms = timespec_to_ms(&cmds[i].run_start_mono);
                    int64_t next_ms = start_ms + interval_ms;

                    struct timespec cur_now;
                    get_monotonic_time(&cur_now);
                    int64_t cur_now_ms = timespec_to_ms(&cur_now);

                    if (cur_now_ms >= next_ms) {
                        /* Run took longer than interval: restart immediately */
                        mw_spawn_cmd(sh, &cmds[i], cmds, ncmds);
                    } else {
                        cmds[i].next_start_mono = ms_to_timespec(next_ms);
                        cmds[i].state = MW_IDLE;
                    }
                }
            }
        }
    }

    /* Handle Ctrl+C or Close */
    if (ctx.got_intr) {
        exit_status = 130;

        /* Kill all active children with SIGTERM to their process groups */
        for (int i = 0; i < ncmds; i++) {
            if (cmds[i].state == MW_RUNNING && cmds[i].pgid > 0) {
                kill(-cmds[i].pgid, SIGTERM);
            }
        }

        /* Wait up to 300 ms for children to exit before escalating to SIGKILL */
        struct timespec term_start;
        get_monotonic_time(&term_start);
        int64_t term_deadline_ms = timespec_to_ms(&term_start) + 300;

        while (mw_has_running_children(cmds, ncmds)) {
            for (int i = 0; i < ncmds; i++) {
                if (cmds[i].state == MW_RUNNING && cmds[i].pid > 0) {
                    int st = 0;
                    pid_t w = waitpid(cmds[i].pid, &st, WNOHANG);
                    if (w == cmds[i].pid || (w < 0 && errno == ECHILD)) {
                        cmds[i].pid = -1;
                        cmds[i].state = MW_IDLE;
                    }
                }
            }
            if (!mw_has_running_children(cmds, ncmds)) break;

            struct timespec now;
            get_monotonic_time(&now);
            int64_t now_ms = timespec_to_ms(&now);
            int remaining_ms = (int)(term_deadline_ms - now_ms);

            if (remaining_ms <= 0) {
                /* Escalate to SIGKILL */
                for (int i = 0; i < ncmds; i++) {
                    if (cmds[i].state == MW_RUNNING && cmds[i].pgid > 0) {
                        kill(-cmds[i].pgid, SIGKILL);
                    }
                }
                for (int i = 0; i < ncmds; i++) {
                    if (cmds[i].state == MW_RUNNING && cmds[i].pid > 0) {
                        waitpid(cmds[i].pid, NULL, 0);
                        cmds[i].pid = -1;
                        cmds[i].state = MW_IDLE;
                    }
                }
                break;
            }

            struct pollfd pfd;
            pfd.fd = (sh->wake_pipe[0] >= 0) ? sh->wake_pipe[0] : -1;
            pfd.events = POLLIN;
            poll(&pfd, pfd.fd >= 0 ? 1 : 0, remaining_ms > 30 ? 30 : remaining_ms);
            if (pfd.fd >= 0 && (pfd.revents & POLLIN)) {
                char dummy[64];
                while (read(sh->wake_pipe[0], dummy, sizeof(dummy)) > 0) {}
            }
        }

        /* Close all remaining pipe fds */
        for (int i = 0; i < ncmds; i++) {
            if (cmds[i].pipe_read_fd >= 0) {
                close(cmds[i].pipe_read_fd);
                cmds[i].pipe_read_fd = -1;
            }
        }

        /* Normal GUI echo of ^C */
        const char *intr_echo = "^C\n";
        if (from_tab_fd >= 0) {
            proto_write_frame_blocking(from_tab_fd, FRAME_OUTPUT, intr_echo, (uint32_t)strlen(intr_echo));
        } else {
            ssize_t w = write(STDOUT_FILENO, intr_echo, strlen(intr_echo));
            (void)w;
        }
    } else if (ctx.got_close) {
        exit_status = 0;
        sh->should_exit = true;

        for (int i = 0; i < ncmds; i++) {
            if (cmds[i].state == MW_RUNNING && cmds[i].pgid > 0) {
                kill(-cmds[i].pgid, SIGKILL);
            }
            if (cmds[i].pid > 0) {
                waitpid(cmds[i].pid, NULL, 0);
                cmds[i].pid = -1;
            }
            if (cmds[i].pipe_read_fd >= 0) {
                close(cmds[i].pipe_read_fd);
                cmds[i].pipe_read_fd = -1;
            }
        }
    }

    /* Free all resources */
    for (int i = 0; i < ncmds; i++) {
        free(cmds[i].cmd_text);
        free(cmds[i].output_buf);
    }
    free(cmds);

    sh->last_status = exit_status;
    vars_set_status(&sh->vars, exit_status);
    return exit_status;
}
