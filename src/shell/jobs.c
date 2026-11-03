/*
 * jobs.c: Asynchronous job control and process group tracking.
 *
 * Manages the shell's active background and stopped jobs table:
 * - Tracks job states: RUNNING, STOPPED, DONE.
 * - Current (+) and previous (-) job sequence tracking.
 * - Resolution of job specifications: %n, %+, %-, %%.
 * - Non-blocking status updates driven by SIGCHLD wake-pipe notifications.
 * - Single notification of finished background jobs before returning to the prompt.
 */

#define _GNU_SOURCE
#include "jobs.h"
#include "../proto.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <errno.h>
#include <ctype.h>

void jobs_init(JobTable *table) {
    if (!table) return;
    memset(table, 0, sizeof(JobTable));
    table->order_counter = 0;
}

void jobs_free(JobTable *table) {
    if (!table) return;
    for (int i = 0; i < table->njobs; i++) {
        if (table->jobs[i].cmd_text) {
            free(table->jobs[i].cmd_text);
            table->jobs[i].cmd_text = NULL;
        }
        if (table->jobs[i].capture_fd >= 0) {
            close(table->jobs[i].capture_fd);
            table->jobs[i].capture_fd = -1;
        }
        if (table->jobs[i].stdin_write_fd >= 0) {
            close(table->jobs[i].stdin_write_fd);
            table->jobs[i].stdin_write_fd = -1;
        }
    }
    table->njobs = 0;
}

int jobs_alloc_id(const JobTable *table) {
    if (!table) return 1;
    for (int candidate = 1; candidate <= MAX_JOBS + 1; candidate++) {
        bool used = false;
        for (int i = 0; i < table->njobs; i++) {
            if (table->jobs[i].id == candidate) {
                used = true;
                break;
            }
        }
        if (!used) return candidate;
    }
    return 1;
}

Job *jobs_add(JobTable *table, pid_t pgid, const pid_t *pids, int npids, const char *cmd_text, bool foreground, int capture_fd, int stdin_write_fd) {
    if (!table || table->njobs >= MAX_JOBS) return NULL;

    Job *j = &table->jobs[table->njobs++];
    memset(j, 0, sizeof(Job));
    j->id = jobs_alloc_id(table);
    j->pgid = pgid;
    j->npids = (npids < MAX_JOB_PIDS) ? npids : MAX_JOB_PIDS;
    if (pids && j->npids > 0) {
        memcpy(j->pids, pids, (size_t)j->npids * sizeof(pid_t));
    }
    for (int k = 0; k < j->npids; k++) {
        j->pid_done[k] = false;
    }
    j->cmd_text = cmd_text ? strdup(cmd_text) : strdup("");
    j->state = JOB_RUNNING;
    j->foreground = foreground;
    j->notified = false;
    j->capture_fd = capture_fd;
    j->stdin_write_fd = stdin_write_fd;
    j->exit_status = 0;
    j->order = ++table->order_counter;
    return j;
}

Job *jobs_find_by_pgid(JobTable *table, pid_t pgid) {
    if (!table || pgid <= 0) return NULL;
    for (int i = 0; i < table->njobs; i++) {
        if (table->jobs[i].pgid == pgid) {
            return &table->jobs[i];
        }
    }
    return NULL;
}

Job *jobs_find_by_id(JobTable *table, int id) {
    if (!table || id <= 0) return NULL;
    for (int i = 0; i < table->njobs; i++) {
        if (table->jobs[i].id == id) {
            return &table->jobs[i];
        }
    }
    return NULL;
}

Job *jobs_find_by_pid(JobTable *table, pid_t pid, int *out_pid_idx) {
    if (!table || pid <= 0) return NULL;
    for (int i = 0; i < table->njobs; i++) {
        Job *j = &table->jobs[i];
        for (int k = 0; k < j->npids; k++) {
            if (j->pids[k] == pid) {
                if (out_pid_idx) *out_pid_idx = k;
                return j;
            }
        }
    }
    return NULL;
}

Job *jobs_get_current(JobTable *table) {
    if (!table || table->njobs == 0) return NULL;
    Job *best = NULL;
    uint64_t max_order = 0;

    /* Prioritize active (non-done) jobs */
    for (int i = 0; i < table->njobs; i++) {
        Job *j = &table->jobs[i];
        if (j->state != JOB_DONE) {
            if (!best || j->order > max_order) {
                best = j;
                max_order = j->order;
            }
        }
    }
    if (best) return best;

    for (int i = 0; i < table->njobs; i++) {
        Job *j = &table->jobs[i];
        if (!best || j->order > max_order) {
            best = j;
            max_order = j->order;
        }
    }
    return best;
}

Job *jobs_get_previous(JobTable *table) {
    if (!table || table->njobs < 2) return NULL;
    Job *curr = jobs_get_current(table);
    Job *best = NULL;
    uint64_t max_order = 0;

    for (int i = 0; i < table->njobs; i++) {
        Job *j = &table->jobs[i];
        if (j != curr && j->state != JOB_DONE) {
            if (!best || j->order > max_order) {
                best = j;
                max_order = j->order;
            }
        }
    }
    if (best) return best;

    for (int i = 0; i < table->njobs; i++) {
        Job *j = &table->jobs[i];
        if (j != curr) {
            if (!best || j->order > max_order) {
                best = j;
                max_order = j->order;
            }
        }
    }
    return best;
}

void jobs_update_order(JobTable *table, Job *j) {
    if (!table || !j) return;
    j->order = ++table->order_counter;
}

void jobs_remove(JobTable *table, pid_t pgid) {
    if (!table || pgid <= 0) return;
    for (int i = 0; i < table->njobs; i++) {
        if (table->jobs[i].pgid == pgid) {
            if (table->jobs[i].cmd_text) {
                free(table->jobs[i].cmd_text);
            }
            if (table->jobs[i].capture_fd >= 0) {
                close(table->jobs[i].capture_fd);
            }
            if (table->jobs[i].stdin_write_fd >= 0) {
                close(table->jobs[i].stdin_write_fd);
            }
            for (int k = i; k < table->njobs - 1; k++) {
                table->jobs[k] = table->jobs[k + 1];
            }
            table->njobs--;
            break;
        }
    }
}

void jobs_remove_by_id(JobTable *table, int id) {
    if (!table || id <= 0) return;
    for (int i = 0; i < table->njobs; i++) {
        if (table->jobs[i].id == id) {
            if (table->jobs[i].cmd_text) {
                free(table->jobs[i].cmd_text);
            }
            if (table->jobs[i].capture_fd >= 0) {
                close(table->jobs[i].capture_fd);
            }
            if (table->jobs[i].stdin_write_fd >= 0) {
                close(table->jobs[i].stdin_write_fd);
            }
            for (int k = i; k < table->njobs - 1; k++) {
                table->jobs[k] = table->jobs[k + 1];
            }
            table->njobs--;
            break;
        }
    }
}

void jobs_update_status(JobTable *table) {
    if (!table) return;

    for (int i = 0; i < table->njobs; i++) {
        Job *j = &table->jobs[i];
        if (j->state == JOB_DONE) continue;

        bool any_stopped = false;
        bool any_continued = false;
        bool all_done = true;

        for (int k = 0; k < j->npids; k++) {
            if (!j->pid_done[k]) {
                int st = 0;
                pid_t w = waitpid(j->pids[k], &st, WNOHANG | WUNTRACED | WCONTINUED);
                if (w == j->pids[k]) {
                    if (WIFSTOPPED(st)) {
                        any_stopped = true;
                    } else if (WIFCONTINUED(st)) {
                        any_continued = true;
                    } else if (WIFEXITED(st) || WIFSIGNALED(st)) {
                        j->pid_done[k] = true;
                        if (k == j->npids - 1) {
                            j->exit_status = WIFEXITED(st) ? WEXITSTATUS(st) : (128 + WTERMSIG(st));
                        }
                    }
                } else if (w < 0 && errno == ECHILD) {
                    j->pid_done[k] = true;
                }
            }

            if (!j->pid_done[k]) {
                all_done = false;
            }
        }

        if (any_stopped) {
            j->state = JOB_STOPPED;
        } else if (any_continued && j->state == JOB_STOPPED) {
            j->state = JOB_RUNNING;
        } else if (all_done) {
            j->state = JOB_DONE;
        }
    }
}

Job *jobs_resolve_spec(JobTable *table, const char *spec, const char *builtin_name, OutputSink *err_sink) {
    if (!table) return NULL;

    if (!spec || !spec[0] || strcmp(spec, "%%") == 0 || strcmp(spec, "%+") == 0 || strcmp(spec, "+") == 0 || strcmp(spec, "%") == 0) {
        Job *j = jobs_get_current(table);
        if (!j) {
            sink_printf(err_sink, "terminator: %s: current: no such job\n", builtin_name);
            return NULL;
        }
        return j;
    }

    if (strcmp(spec, "%-") == 0 || strcmp(spec, "-") == 0) {
        Job *j = jobs_get_previous(table);
        if (!j) {
            sink_printf(err_sink, "terminator: %s: previous: no such job\n", builtin_name);
            return NULL;
        }
        return j;
    }

    const char *num_str = spec;
    if (spec[0] == '%') num_str = spec + 1;

    char *endptr = NULL;
    long id = strtol(num_str, &endptr, 10);
    if (!endptr || *endptr != '\0' || id <= 0) {
        sink_printf(err_sink, "terminator: %s: %s: no such job\n", builtin_name, spec);
        return NULL;
    }

    Job *j = jobs_find_by_id(table, (int)id);
    if (!j) {
        sink_printf(err_sink, "terminator: %s: %s: no such job\n", builtin_name, spec);
        return NULL;
    }

    return j;
}

int jobs_notify_done(JobTable *table, int from_tab_fd) {
    if (!table || from_tab_fd < 0) return 0;
    jobs_update_status(table);
    int count = 0;
    Job *curr = jobs_get_current(table);
    Job *prev = jobs_get_previous(table);

    for (int i = 0; i < table->njobs; ) {
        Job *j = &table->jobs[i];
        if (j->state == JOB_DONE && !j->foreground) {
            if (j->capture_fd >= 0) {
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
                close(j->capture_fd);
                j->capture_fd = -1;
            }
            if (j->stdin_write_fd >= 0) {
                close(j->stdin_write_fd);
                j->stdin_write_fd = -1;
            }

            char marker = (j == curr) ? '+' : (j == prev ? '-' : ' ');
            char notice[512];
            int len = snprintf(notice, sizeof(notice), "[%d]%c Done %s\n", j->id, marker, j->cmd_text);
            proto_write_frame_blocking(from_tab_fd, FRAME_OUTPUT, notice, (uint32_t)len);

            jobs_remove_by_id(table, j->id);
            count++;
            curr = jobs_get_current(table);
            prev = jobs_get_previous(table);
        } else {
            i++;
        }
    }
    return count;
}

int jobs_count_stopped(const JobTable *table) {
    if (!table) return 0;
    int count = 0;
    for (int i = 0; i < table->njobs; i++) {
        if (table->jobs[i].state == JOB_STOPPED) {
            count++;
        }
    }
    return count;
}

int jobs_count_active(const JobTable *table) {
    if (!table) return 0;
    int count = 0;
    for (int i = 0; i < table->njobs; i++) {
        if (table->jobs[i].state != JOB_DONE) {
            count++;
        }
    }
    return count;
}

void jobs_terminate_all(JobTable *table) {
    if (!table) return;
    for (int i = 0; i < table->njobs; i++) {
        Job *j = &table->jobs[i];
        if (j->pgid > 1) {
            kill(-j->pgid, SIGHUP);
            kill(-j->pgid, SIGCONT);
        }
    }
    poll(NULL, 0, 20); /* 20 ms wait without busy-waiting or usleep */
    for (int i = 0; i < table->njobs; i++) {
        Job *j = &table->jobs[i];
        if (j->pgid > 1) {
            kill(-j->pgid, SIGKILL);
            for (int k = 0; k < j->npids; k++) {
                if (j->pids[k] > 0) {
                    waitpid(j->pids[k], NULL, 0);
                }
            }
        }
    }
    jobs_free(table);
}
