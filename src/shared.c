/*
 * shared.c: Shared memory mapping and synchronization.
 *
 * Allocates the process-shared state in an anonymous mmap region shared across
 * all tabs in both Thread Mode and Process Mode. Implements semaphore locking with
 * automatic dead-owner detection and recovery to prevent hangs if a tab crashes.
 */

#define _GNU_SOURCE
#include "shared.h"
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <stdio.h>

Shared *shared_create(void) {
    Shared *sh = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) {
        perror("mmap shared");
        return NULL;
    }

    memset(sh, 0, sizeof(Shared));

#if defined(__APPLE__)
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        munmap(sh, sizeof(Shared));
        return NULL;
    }
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    if (pthread_mutex_init(&sh->mtx, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        munmap(sh, sizeof(Shared));
        return NULL;
    }
    pthread_mutexattr_destroy(&attr);
#else
    if (sem_init(&sh->sem, 1, 1) != 0) {
        perror("sem_init");
        munmap(sh, sizeof(Shared));
        return NULL;
    }
#endif

    sh->owner_pid = 0;
    sh->history.next_seq = 1;
    sh->history.count = 0;
    sh->history.head = 0;

    return sh;
}

void shared_destroy(Shared *sh) {
    if (!sh || sh == MAP_FAILED) return;

#if defined(__APPLE__)
    pthread_mutex_destroy(&sh->mtx);
#else
    sem_destroy(&sh->sem);
#endif

    munmap(sh, sizeof(Shared));
}

int shared_lock(Shared *sh) {
    if (!sh) return -1;

#if defined(__APPLE__)
    struct timespec req = {0, 10000000}; /* 10ms */
    int attempts = 0;
    while (1) {
        int ret = pthread_mutex_trylock(&sh->mtx);
        if (ret == 0) {
            sh->owner_pid = getpid();
            return 0;
        }
        attempts++;
        if (attempts > 50) { /* ~500ms passed */
            pid_t owner = sh->owner_pid;
            if (owner > 0 && kill(owner, 0) == -1 && errno == ESRCH) {
                /* Dead owner recovery */
                pthread_mutex_destroy(&sh->mtx);
                pthread_mutexattr_t attr;
                pthread_mutexattr_init(&attr);
                pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
                pthread_mutex_init(&sh->mtx, &attr);
                pthread_mutexattr_destroy(&attr);
                pthread_mutex_lock(&sh->mtx);
                sh->owner_pid = getpid();
                return 0;
            }
        }
        nanosleep(&req, NULL);
    }
#else
    struct timespec ts;
    while (1) {
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
            return -1;
        }
        ts.tv_sec += 2; /* 2 second timeout */

        int ret = sem_timedwait(&sh->sem, &ts);
        if (ret == 0) {
            sh->owner_pid = getpid();
            return 0;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == ETIMEDOUT) {
            pid_t owner = sh->owner_pid;
            if (owner > 0 && kill(owner, 0) == -1 && errno == ESRCH) {
                /* Dead owner detected! Recover semaphore */
                sem_init(&sh->sem, 1, 0);
                sh->owner_pid = getpid();
                return 0;
            }
            /* Owner is still alive, retry */
            continue;
        }

        return -1;
    }
#endif
}

void shared_unlock(Shared *sh) {
    if (!sh) return;
    sh->owner_pid = 0;
#if defined(__APPLE__)
    pthread_mutex_unlock(&sh->mtx);
#else
    sem_post(&sh->sem);
#endif
}
