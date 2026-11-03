#ifndef SHELL_SINK_H
#define SHELL_SINK_H

#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

typedef enum {
    SINK_FRAMED, /* In-process framed to GUI over pipe */
    SINK_RAW_FD  /* Raw file descriptor (for redirection or child pipelines) */
} SinkType;

typedef struct {
    SinkType type;
    int fd;
} OutputSink;

OutputSink sink_framed(int to_gui_fd);
OutputSink sink_raw(int fd);

int sink_write(OutputSink *sink, const void *data, size_t len);
int sink_printf(OutputSink *sink, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif /* SHELL_SINK_H */
