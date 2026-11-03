#ifndef SHARED_H
#define SHARED_H

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "history.h"

#if defined(__APPLE__)
#include <pthread.h>
#else
#include <semaphore.h>
#endif

typedef struct Shared {
#if defined(__APPLE__)
    pthread_mutex_t mtx;
#else
    sem_t sem;
#endif
    pid_t owner_pid;
    char histfile[1024];
    HistoryRing history;
} Shared;

/* Initialize mmap'd MAP_SHARED | MAP_ANONYMOUS region and process-shared semaphore */
Shared *shared_create(void);

/* Clean up semaphore and unmap region */
void shared_destroy(Shared *sh);

/* Lock with dead-owner detection and recovery */
int shared_lock(Shared *sh);

/* Unlock shared state */
void shared_unlock(Shared *sh);

/* History API */
uint64_t history_append(Shared *sh, const char *line);
uint32_t history_count(Shared *sh);
uint64_t history_oldest_seq(Shared *sh);
uint64_t history_newest_seq(Shared *sh);
int history_get_by_seq(Shared *sh, uint64_t seq, char *out, size_t out_cap);
size_t history_snapshot(Shared *sh, char lines[][HISTORY_MAX_LINE], uint64_t *seqs, size_t max_items);

#endif /* SHARED_H */
