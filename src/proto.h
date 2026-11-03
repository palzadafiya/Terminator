#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

#define PROTO_MAX_PAYLOAD 65536
#define PROTO_HEADER_SIZE 5
#define OUT_QUEUE_CAPACITY (128 * 1024)

/* Frame Types */
/* GUI -> Tab */
#define FRAME_LINE        0x01
#define FRAME_INTR        0x02
#define FRAME_TSTP        0x03
#define FRAME_EOF         0x04
#define FRAME_CLOSE       0x05
#define FRAME_RESIZE      0x06

/* Tab -> GUI */
#define FRAME_OUTPUT      0x10
#define FRAME_PROMPT      0x11
#define FRAME_RELOAD_KEYS 0x12
#define FRAME_EXIT        0x13
#define FRAME_CLEAR       0x14

typedef struct {
    uint8_t type;
    uint32_t len;
    char payload[PROTO_MAX_PAYLOAD + 1];
} Frame;

typedef struct {
    char buf[PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD];
    size_t len;
} InBuffer;

typedef struct {
    char buf[OUT_QUEUE_CAPACITY];
    size_t head; /* write offset */
    size_t tail; /* read offset */
} OutQueue;

/* Callback signature when a full frame is decoded */
typedef void (*FrameCallback)(const Frame *frame, void *user_data);

/* Read from fd (nonblocking or blocking) and invoke on_frame for each complete frame.
 * Returns > 0 if bytes read, 0 on EOF, -1 on EAGAIN/EWOULDBLOCK or fatal error.
 * *eof_reached is set to true on genuine EOF. */
ssize_t proto_read_frames(int fd, InBuffer *in, FrameCallback on_frame, void *user_data, bool *eof_reached);

/* Write an entire frame to fd in blocking fashion (retries EINTR, handles partial writes).
 * Returns 0 on success, -1 on failure. */
int proto_write_frame_blocking(int fd, uint8_t type, const void *payload, uint32_t len);

/* Enqueue a frame into an OutQueue for nonblocking write.
 * Returns 0 on success, -1 if queue would overflow. */
int proto_queue_frame(OutQueue *q, uint8_t type, const void *payload, uint32_t len);

/* Flush pending bytes in OutQueue to nonblocking fd.
 * Returns 0 if all or partial written without error, -1 on write error (excluding EAGAIN). */
int proto_flush_queue(int fd, OutQueue *q);

/* Check if OutQueue has pending data to write */
bool proto_queue_has_data(const OutQueue *q);

#endif /* PROTO_H */
