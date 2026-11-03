/*
 * syscompat.h: Cross-platform POSIX system compatibility shims.
 *
 * Provides safe pipe2 emulation with O_CLOEXEC and O_NONBLOCK on platforms
 * lacking native Linux pipe2 (e.g. macOS during local development).
 */

#ifndef SYSCOMPAT_H
#define SYSCOMPAT_H

#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>

#if defined(__APPLE__)
static inline int safe_pipe2(int fds[2], int flags) {
    if (pipe(fds) < 0) return -1;
    if (flags & O_CLOEXEC) {
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    }
    if (flags & O_NONBLOCK) {
        fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
        fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
    }
    return 0;
}
#define pipe2 safe_pipe2
#endif

#endif /* SYSCOMPAT_H */
