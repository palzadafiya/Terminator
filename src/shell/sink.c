/*
 * sink.c: Output sink abstraction layer.
 *
 * Decouples built-in command output formatting from the underlying transport:
 * - SINK_FRAMED: Encapsulates output into framed binary protocol packets (FRAME_OUTPUT) for the GUI.
 * - SINK_RAW_FD: Writes directly to raw file descriptors when builtins execute inside pipelines or subshells.
 */

#define _GNU_SOURCE
#include "sink.h"
#include "../proto.h"
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

OutputSink sink_framed(int to_gui_fd) {
    OutputSink s;
    s.type = SINK_FRAMED;
    s.fd = to_gui_fd;
    return s;
}

OutputSink sink_raw(int fd) {
    OutputSink s;
    s.type = SINK_RAW_FD;
    s.fd = fd;
    return s;
}

int sink_write(OutputSink *sink, const void *data, size_t len) {
    if (!sink || !data || len == 0 || sink->fd < 0) return 0;

    if (sink->type == SINK_FRAMED) {
        const char *p = (const char *)data;
        size_t sent = 0;
        /* Chunk frames if larger than 32 KiB to prevent overflow */
        while (sent < len) {
            size_t chunk = len - sent;
            if (chunk > 32768) chunk = 32768;
            if (proto_write_frame_blocking(sink->fd, FRAME_OUTPUT, p + sent, (uint32_t)chunk) != 0) {
                return -1;
            }
            sent += chunk;
        }
        return (int)len;
    } else {
        const char *p = (const char *)data;
        size_t total = 0;
        while (total < len) {
            ssize_t n = write(sink->fd, p + total, len - total);
            if (n < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            total += (size_t)n;
        }
        return (int)total;
    }
}

int sink_printf(OutputSink *sink, const char *fmt, ...) {
    if (!sink) return -1;

    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0) return -1;

    if ((size_t)n < sizeof(buf)) {
        return sink_write(sink, buf, (size_t)n);
    }

    /* Larger buffer needed */
    char *dyn = malloc((size_t)n + 1);
    if (!dyn) return -1;

    va_start(ap, fmt);
    vsnprintf(dyn, (size_t)n + 1, fmt, ap);
    va_end(ap);

    int ret = sink_write(sink, dyn, (size_t)n);
    free(dyn);
    return ret;
}
