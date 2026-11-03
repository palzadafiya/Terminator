/*
 * tabrt.c: Tab runtime management and execution loop.
 *
 * Implements tab lifecycle management across Thread Mode and Process Mode:
 * - Spawning tabs as POSIX threads (Thread Mode) or isolated child processes (Process Mode).
 * - tab_main: The unified event loop running the tab shell over bidirectional pipes.
 * - Non-blocking poll loop multiplexing GUI control frames, wake-pipes, and background job pipes.
 * - Clean tab termination and resource reclamation.
 */

#define _GNU_SOURCE
#include "tabrt.h"
#include "shell/shell.h"
#include "shell/exec.h"
#include "shell/lexer.h"
#include "syscompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <errno.h>

static void format_display_cwd(const char *cwd, char *out, size_t out_cap) {
    const char *home = getenv("HOME");
    if (home && home[0] && strncmp(cwd, home, strlen(home)) == 0) {
        size_t home_len = strlen(home);
        if (cwd[home_len] == '\0') {
            snprintf(out, out_cap, "~");
        } else if (cwd[home_len] == '/') {
            snprintf(out, out_cap, "~%s", cwd + home_len);
        } else {
            snprintf(out, out_cap, "%s", cwd);
        }
    } else {
        snprintf(out, out_cap, "%s", cwd);
    }
}

void tab_update_label(Tab *tab) {
    if (!tab) return;
    char disp[256];
    format_display_cwd(tab->cwd, disp, sizeof(disp));
    snprintf(tab->label, sizeof(tab->label), "%d: %s", tab->id, disp);
}

static void send_prompt_frame(TabChannel *chan, const char *cwd, const char *prompt) {
    char payload[1024 + 256];
    size_t cwd_len = strlen(cwd);
    size_t pmt_len = strlen(prompt);

    memcpy(payload, cwd, cwd_len);
    payload[cwd_len] = '\0';
    memcpy(payload + cwd_len + 1, prompt, pmt_len + 1);

    uint32_t total = (uint32_t)(cwd_len + 1 + pmt_len + 1);
    proto_write_frame_blocking(chan->from_tab_fd, FRAME_PROMPT, payload, total);
}

typedef struct {
    TabChannel *chan;
    Shell *sh;
} TabMainContext;

static void handle_backend_frame(const Frame *f, void *user_data) {
    TabMainContext *ctx = (TabMainContext *)user_data;
    TabChannel *chan = ctx->chan;
    Shell *sh = ctx->sh;

    if (f->type == FRAME_INTR) {
        if (sh->in_continuation) {
            if (sh->accum_line) {
                free(sh->accum_line);
                sh->accum_line = NULL;
            }
            sh->accum_len = 0;
            sh->accum_cap = 0;
            sh->in_continuation = false;
        }
        jobs_notify_done(&sh->jobs, chan->from_tab_fd);
        send_prompt_frame(chan, sh->cwd, "user@terminator> ");
    } else if (f->type == FRAME_TSTP) {
        if (sh->in_continuation) {
            if (sh->accum_line) {
                free(sh->accum_line);
                sh->accum_line = NULL;
            }
            sh->accum_len = 0;
            sh->accum_cap = 0;
            sh->in_continuation = false;
        }
        jobs_notify_done(&sh->jobs, chan->from_tab_fd);
        send_prompt_frame(chan, sh->cwd, "user@terminator> ");
    } else if (f->type == FRAME_LINE) {
        char line[PROTO_MAX_PAYLOAD];
        size_t len = f->len < sizeof(line) - 1 ? f->len : sizeof(line) - 1;
        memcpy(line, f->payload, len);
        line[len] = '\0';

        if (!sh->in_continuation) {
            LexStatus st = lex_check_completeness(line);
            if (st == LEX_COMPLETE || st == LEX_ERROR) {
                shell_execute_line(sh, line, chan->from_tab_fd, chan->to_tab_fd);
                if (sh->should_exit) return;
                jobs_notify_done(&sh->jobs, chan->from_tab_fd);
                send_prompt_frame(chan, sh->cwd, "user@terminator> ");
            } else {
                sh->in_continuation = true;
                sh->accum_len = len;
                sh->accum_cap = len + 512;
                sh->accum_line = malloc(sh->accum_cap);
                if (sh->accum_line) {
                    memcpy(sh->accum_line, line, len + 1);
                }
                send_prompt_frame(chan, sh->cwd, "> ");
            }
        } else {
            LexStatus prev_st = lex_check_completeness(sh->accum_line);
            if (prev_st == LEX_INCOMPLETE_BACKSLASH && sh->accum_len > 0 &&
                sh->accum_line[sh->accum_len - 1] == '\\') {
                sh->accum_len--;
                sh->accum_line[sh->accum_len] = '\0';

                size_t need = sh->accum_len + len + 1;
                if (need > sh->accum_cap) {
                    sh->accum_cap = need * 2;
                    sh->accum_line = realloc(sh->accum_line, sh->accum_cap);
                }
                if (sh->accum_line) {
                    memcpy(sh->accum_line + sh->accum_len, line, len + 1);
                    sh->accum_len += len;
                }
            } else {
                size_t need = sh->accum_len + 1 + len + 1;
                if (need > sh->accum_cap) {
                    sh->accum_cap = need * 2;
                    sh->accum_line = realloc(sh->accum_line, sh->accum_cap);
                }
                if (sh->accum_line) {
                    sh->accum_line[sh->accum_len++] = '\n';
                    memcpy(sh->accum_line + sh->accum_len, line, len + 1);
                    sh->accum_len += len;
                }
            }

            LexStatus st = lex_check_completeness(sh->accum_line);
            if (st == LEX_COMPLETE || st == LEX_ERROR) {
                sh->in_continuation = false;
                shell_execute_line(sh, sh->accum_line, chan->from_tab_fd, chan->to_tab_fd);

                free(sh->accum_line);
                sh->accum_line = NULL;
                sh->accum_len = 0;
                sh->accum_cap = 0;

                if (sh->should_exit) return;
                jobs_notify_done(&sh->jobs, chan->from_tab_fd);
                send_prompt_frame(chan, sh->cwd, "user@terminator> ");
            } else {
                send_prompt_frame(chan, sh->cwd, "> ");
            }
        }
    } else if (f->type == FRAME_CLOSE) {
        sh->should_exit = true;
    } else if (f->type == FRAME_EOF) {
        int stopped_count = jobs_count_stopped(&sh->jobs);
        if (stopped_count > 0 && !sh->exit_warned) {
            proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, "There are stopped jobs.\n", 24);
            sh->exit_warned = true;
            send_prompt_frame(chan, sh->cwd, "user@terminator> ");
        } else {
            sh->should_exit = true;
        }
    }
}

void tab_main(TabChannel *chan) {
    if (!chan) return;

    Shell *sh = shell_create(NULL, NULL, chan->shared);
    if (!sh) return;

    sh->from_tab_fd = chan->from_tab_fd;
    sh->to_tab_fd = chan->to_tab_fd;

    TabMainContext ctx;
    ctx.chan = chan;
    ctx.sh = sh;

    /* Send initial prompt */
    send_prompt_frame(chan, sh->cwd, "user@terminator> ");

    InBuffer in;
    memset(&in, 0, sizeof(in));

    while (!sh->should_exit) {
        struct pollfd pfds[2 + MAX_JOBS];
        int nfds = 0;

        int to_idx = -1;
        int wake_idx = -1;
        int bg_cap_indices[MAX_JOBS];
        Job *bg_jobs[MAX_JOBS];
        int num_bg_caps = 0;

        pfds[nfds].fd = chan->to_tab_fd;
        pfds[nfds].events = POLLIN | POLLHUP | POLLERR;
        to_idx = nfds++;

        if (sh->wake_pipe[0] >= 0) {
            pfds[nfds].fd = sh->wake_pipe[0];
            pfds[nfds].events = POLLIN;
            wake_idx = nfds++;
        }

        for (int k = 0; k < sh->jobs.njobs; k++) {
            Job *bg = &sh->jobs.jobs[k];
            if (bg->capture_fd >= 0) {
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

        /* Forward background jobs output */
        for (int b = 0; b < num_bg_caps; b++) {
            int bidx = bg_cap_indices[b];
            if (pfds[bidx].revents & (POLLIN | POLLHUP | POLLERR)) {
                Job *bg = bg_jobs[b];
                char buf[4096];
                while (1) {
                    ssize_t n = read(bg->capture_fd, buf, sizeof(buf));
                    if (n > 0) {
                        proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, buf, (uint32_t)n);
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

        /* Drain wake pipe and update statuses */
        if (wake_idx >= 0 && (pfds[wake_idx].revents & POLLIN)) {
            char dummy[64];
            while (read(sh->wake_pipe[0], dummy, sizeof(dummy)) > 0) {}
            jobs_update_status(&sh->jobs);
        }

        if (to_idx >= 0 && (pfds[to_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            bool eof_reached = false;
            ssize_t n = proto_read_frames(chan->to_tab_fd, &in, handle_backend_frame, &ctx, &eof_reached);
            if (n <= 0 && eof_reached) {
                break;
            }
        }
    }

    if (sh->should_exit) {
        uint32_t status = (uint32_t)sh->exit_code;
        proto_write_frame_blocking(chan->from_tab_fd, FRAME_EXIT, &status, sizeof(status));
    }

    jobs_terminate_all(&sh->jobs);
    shell_destroy(sh);
}

static void *tab_thread_entry(void *arg) {
    TabChannel *chan = (TabChannel *)arg;
    tab_main(chan);
    close(chan->to_tab_fd);
    close(chan->from_tab_fd);
    free(chan);
    return NULL;
}

TabRuntime *tabrt_create(bool use_process, Shared *shared) {
    TabRuntime *rt = calloc(1, sizeof(TabRuntime));
    if (!rt) return NULL;

    rt->use_process = use_process;
    rt->shared = shared;
    rt->tab_count = 0;
    rt->active_idx = 0;
    rt->next_tab_id = 1;

    return rt;
}

void tabrt_destroy(TabRuntime *rt) {
    if (!rt) return;

    /* Close remaining tabs */
    while (rt->tab_count > 0) {
        tabrt_close(rt, rt->tab_count - 1);
    }

    free(rt);
}

Tab *tabrt_spawn(TabRuntime *rt, int x_conn_fd) {
    if (!rt || rt->tab_count >= MAX_TABS) {
        return NULL;
    }

    int to_tab[2];
    int from_tab[2];

    if (pipe2(to_tab, O_CLOEXEC) < 0) {
        perror("pipe2 to_tab");
        return NULL;
    }
    if (pipe2(from_tab, O_CLOEXEC) < 0) {
        perror("pipe2 from_tab");
        close(to_tab[0]);
        close(to_tab[1]);
        return NULL;
    }

    /* Set GUI ends to nonblocking */
    int flags = fcntl(to_tab[1], F_GETFL, 0);
    fcntl(to_tab[1], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(from_tab[0], F_GETFL, 0);
    fcntl(from_tab[0], F_SETFL, flags | O_NONBLOCK);

    Tab *tab = calloc(1, sizeof(Tab));
    if (!tab) {
        close(to_tab[0]);
        close(to_tab[1]);
        close(from_tab[0]);
        close(from_tab[1]);
        return NULL;
    }

    tab->id = rt->next_tab_id++;
    tab->alive = true;
    tab->to_tab_fd = to_tab[1];
    tab->from_tab_fd = from_tab[0];
    tab->buf = tabbuf_create();
    lineedit_init(&tab->input);
    strcpy(tab->prompt, "user@terminator> ");

    if (!getcwd(tab->cwd, sizeof(tab->cwd))) {
        strcpy(tab->cwd, "/");
    }
    tab_update_label(tab);

    if (rt->use_process) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork tab");
            tabbuf_destroy(tab->buf);
            free(tab);
            close(to_tab[0]);
            close(to_tab[1]);
            close(from_tab[0]);
            close(from_tab[1]);
            return NULL;
        }

        if (pid == 0) {
            /* Child process: Process mode tab */
            if (x_conn_fd >= 0) {
                close(x_conn_fd);
            }

            /* Close all existing tabs' GUI pipe fds */
            for (int i = 0; i < rt->tab_count; i++) {
                if (rt->tabs[i]->to_tab_fd >= 0) close(rt->tabs[i]->to_tab_fd);
                if (rt->tabs[i]->from_tab_fd >= 0) close(rt->tabs[i]->from_tab_fd);
            }

            /* Close GUI ends of own pipes */
            close(to_tab[1]);
            close(from_tab[0]);

            TabChannel chan;
            chan.to_tab_fd = to_tab[0];
            chan.from_tab_fd = from_tab[1];
            chan.shared = rt->shared;

            tab_main(&chan);

            close(to_tab[0]);
            close(from_tab[1]);
            _exit(0);
        }

        /* Parent process */
        tab->pid = pid;
        close(to_tab[0]);
        close(from_tab[1]);
    } else {
        /* Thread mode tab */
        TabChannel *chan = malloc(sizeof(TabChannel));
        if (!chan) {
            tabbuf_destroy(tab->buf);
            free(tab);
            close(to_tab[0]);
            close(to_tab[1]);
            close(from_tab[0]);
            close(from_tab[1]);
            return NULL;
        }
        chan->to_tab_fd = to_tab[0];
        chan->from_tab_fd = from_tab[1];
        chan->shared = rt->shared;

        if (pthread_create(&tab->thread, NULL, tab_thread_entry, chan) != 0) {
            perror("pthread_create tab");
            free(chan);
            tabbuf_destroy(tab->buf);
            free(tab);
            close(to_tab[0]);
            close(to_tab[1]);
            close(from_tab[0]);
            close(from_tab[1]);
            return NULL;
        }

        tab->pid = 0;
        /* Do NOT close to_tab[0] or from_tab[1] in thread mode!
         * In thread mode, all threads share the process fd table.
         * to_tab[0] and from_tab[1] are owned by tab_thread_entry and will be
         * closed by tab_thread_entry when tab_main finishes. */
    }

    rt->tabs[rt->tab_count] = tab;
    rt->active_idx = rt->tab_count;
    rt->tab_count++;

    return tab;
}

bool tabrt_close(TabRuntime *rt, int tab_idx) {
    if (!rt || tab_idx < 0 || tab_idx >= rt->tab_count) {
        return false;
    }

    Tab *tab = rt->tabs[tab_idx];

    /* If tab is still running, request it to close */
    if (tab->alive) {
        if (tab->to_tab_fd >= 0) {
            proto_write_frame_blocking(tab->to_tab_fd, FRAME_CLOSE, NULL, 0);
        }
    }

    /* Wait for child process or thread to finish regardless of prior alive flag */
    if (rt->use_process) {
        if (tab->pid > 0) {
            int status = 0;
            while (waitpid(tab->pid, &status, 0) < 0) {
                if (errno == EINTR) continue;
                break;
            }
            tab->pid = 0;
        }
    } else {
        if (tab->thread) {
            pthread_join(tab->thread, NULL);
            tab->thread = 0;
        }
    }
    tab->alive = false;

    if (tab->to_tab_fd >= 0) {
        close(tab->to_tab_fd);
        tab->to_tab_fd = -1;
    }
    if (tab->from_tab_fd >= 0) {
        close(tab->from_tab_fd);
        tab->from_tab_fd = -1;
    }

    if (tab->buf) {
        tabbuf_destroy(tab->buf);
    }
    completion_result_free(&tab->compl_result);
    free(tab);

    /* Shift remaining tabs */
    for (int i = tab_idx; i < rt->tab_count - 1; i++) {
        rt->tabs[i] = rt->tabs[i + 1];
    }
    rt->tabs[rt->tab_count - 1] = NULL;
    rt->tab_count--;

    if (rt->tab_count == 0) {
        rt->active_idx = 0;
        return true; /* All tabs closed */
    }

    if (rt->active_idx >= rt->tab_count) {
        rt->active_idx = rt->tab_count - 1;
    }

    return false;
}
