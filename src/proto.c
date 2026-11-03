/*
 * proto.c: Framed Inter-Process Protocol (IPC) implementation.
 *
 * Implements framed communication between the GUI and tab shells over unidirectional pipes:
 * 5-byte header: [1 byte opcode] [4 bytes little-endian length] followed by payload (max 64 KiB).
 * Handles partial reads, non-blocking outbound queues, and bounded frame boundary reassembly.
 */

#define _GNU_SOURCE
#include "proto.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

static void encode_header(uint8_t type, uint32_t len, uint8_t *out) {
    out[0] = type;
    out[1] = (uint8_t)(len & 0xFF);
    out[2] = (uint8_t)((len >> 8) & 0xFF);
    out[3] = (uint8_t)((len >> 16) & 0xFF);
    out[4] = (uint8_t)((len >> 24) & 0xFF);
}

static uint32_t decode_len(const uint8_t *in) {
    return (uint32_t)in[1] |
           ((uint32_t)in[2] << 8) |
           ((uint32_t)in[3] << 16) |
           ((uint32_t)in[4] << 24);
}

ssize_t proto_read_frames(int fd, InBuffer *in, FrameCallback on_frame, void *user_data, bool *eof_reached) {
    if (eof_reached) *eof_reached = false;

    size_t space = sizeof(in->buf) - in->len;
    if (space == 0) {
        /* Buffer full without complete frame or frame too big */
        return -1;
    }

    ssize_t n = 0;
    while (1) {
        n = read(fd, in->buf + in->len, space);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* No more data right now */
                break;
            }
            return -1;
        }
        if (n == 0) {
            if (eof_reached) *eof_reached = true;
            break;
        }
        in->len += (size_t)n;
        break;
    }

    /* Process all complete frames in buffer */
    size_t consumed = 0;
    while (in->len - consumed >= PROTO_HEADER_SIZE) {
        const uint8_t *hdr = (const uint8_t *)(in->buf + consumed);
        uint8_t type = hdr[0];
        uint32_t payload_len = decode_len(hdr);

        if (payload_len > PROTO_MAX_PAYLOAD) {
            /* Protocol violation / corrupted stream */
            in->len = 0;
            return -1;
        }

        size_t total_frame_len = PROTO_HEADER_SIZE + payload_len;
        if (in->len - consumed < total_frame_len) {
            /* Partial payload, wait for more data */
            break;
        }

        Frame f;
        f.type = type;
        f.len = payload_len;
        if (payload_len > 0) {
            memcpy(f.payload, in->buf + consumed + PROTO_HEADER_SIZE, payload_len);
        }
        f.payload[payload_len] = '\0';

        if (on_frame) {
            on_frame(&f, user_data);
        }

        consumed += total_frame_len;
    }

    if (consumed > 0) {
        if (consumed < in->len) {
            memmove(in->buf, in->buf + consumed, in->len - consumed);
            in->len -= consumed;
        } else {
            in->len = 0;
        }
    }

    return n;
}

int proto_write_frame_blocking(int fd, uint8_t type, const void *payload, uint32_t len) {
    if (len > PROTO_MAX_PAYLOAD) {
        return -1;
    }

    uint8_t hdr[PROTO_HEADER_SIZE];
    encode_header(type, len, hdr);

    /* Write header */
    size_t written = 0;
    while (written < PROTO_HEADER_SIZE) {
        ssize_t n = write(fd, hdr + written, PROTO_HEADER_SIZE - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += (size_t)n;
    }

    /* Write payload */
    written = 0;
    const char *p = (const char *)payload;
    while (written < len) {
        ssize_t n = write(fd, p + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += (size_t)n;
    }

    return 0;
}

int proto_queue_frame(OutQueue *q, uint8_t type, const void *payload, uint32_t len) {
    if (len > PROTO_MAX_PAYLOAD) {
        return -1;
    }

    size_t needed = PROTO_HEADER_SIZE + len;

    /* Compact queue if possible */
    if (q->head == q->tail) {
        q->head = 0;
        q->tail = 0;
    } else if (q->tail > 0 && sizeof(q->buf) - q->head < needed) {
        size_t existing = q->head - q->tail;
        memmove(q->buf, q->buf + q->tail, existing);
        q->tail = 0;
        q->head = existing;
    }

    if (sizeof(q->buf) - q->head < needed) {
        /* Queue full */
        return -1;
    }

    uint8_t hdr[PROTO_HEADER_SIZE];
    encode_header(type, len, hdr);
    memcpy(q->buf + q->head, hdr, PROTO_HEADER_SIZE);
    q->head += PROTO_HEADER_SIZE;

    if (len > 0 && payload != NULL) {
        memcpy(q->buf + q->head, payload, len);
        q->head += len;
    }

    return 0;
}

int proto_flush_queue(int fd, OutQueue *q) {
    while (q->tail < q->head) {
        size_t to_write = q->head - q->tail;
        ssize_t n = write(fd, q->buf + q->tail, to_write);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0; /* Would block, resume later */
            }
            return -1;
        }
        q->tail += (size_t)n;
    }

    q->head = 0;
    q->tail = 0;
    return 0;
}

bool proto_queue_has_data(const OutQueue *q) {
    return q->tail < q->head;
}
