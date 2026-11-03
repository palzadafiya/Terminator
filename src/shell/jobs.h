#ifndef SHELL_JOBS_H
#define SHELL_JOBS_H

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include "sink.h"

#define MAX_JOB_PIDS 128
#define MAX_JOBS 64

typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} JobState;

typedef struct {
    int id;
    pid_t pgid;
    pid_t pids[MAX_JOB_PIDS];
    bool pid_done[MAX_JOB_PIDS];
    int npids;
    char *cmd_text;
    JobState state;
    bool foreground;
    bool notified;
    int capture_fd;     /* Pipe read-end for stdout/stderr capture (-1 if closed) */
    int stdin_write_fd; /* Stdin write-end for interactive pipeline input (-1 if none) */
    int exit_status;
    uint64_t order;     /* Monotonic sequence for current (+) and previous (-) tracking */
} Job;

typedef struct {
    Job jobs[MAX_JOBS];
    int njobs;
    uint64_t order_counter;
} JobTable;

void jobs_init(JobTable *table);
void jobs_free(JobTable *table);

int jobs_alloc_id(const JobTable *table);
Job *jobs_add(JobTable *table, pid_t pgid, const pid_t *pids, int npids, const char *cmd_text, bool foreground, int capture_fd, int stdin_write_fd);
Job *jobs_find_by_pgid(JobTable *table, pid_t pgid);
Job *jobs_find_by_id(JobTable *table, int id);
Job *jobs_find_by_pid(JobTable *table, pid_t pid, int *out_pid_idx);

Job *jobs_get_current(JobTable *table);
Job *jobs_get_previous(JobTable *table);
void jobs_update_order(JobTable *table, Job *j);

void jobs_remove(JobTable *table, pid_t pgid);
void jobs_remove_by_id(JobTable *table, int id);

void jobs_update_status(JobTable *table);
Job *jobs_resolve_spec(JobTable *table, const char *spec, const char *builtin_name, OutputSink *err_sink);

/* Announces completed background jobs once as "[1]+ Done <cmd>" before prompt and removes them */
int jobs_notify_done(JobTable *table, int from_tab_fd);

int jobs_count_stopped(const JobTable *table);
int jobs_count_active(const JobTable *table);

/* SIGHUP + SIGCONT to all jobs on exit */
void jobs_terminate_all(JobTable *table);

#endif /* SHELL_JOBS_H */
