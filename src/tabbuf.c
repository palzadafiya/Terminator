/*
 * tabbuf.c: Scrollback text buffer and visual line reflow.
 *
 * Implements a 10,000-line circular text buffer per tab:
 * - Processes terminal escape codes, newlines, carriage returns, tabs, and backspaces.
 * - Dynamic visual row calculation for line reflow based on window column width.
 * - Viewport scroll tracking and bottom follow upon new output.
 */

#include "tabbuf.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_LINE_CAP 128

static void ensure_line_cap(TabLine *line, size_t needed) {
    if (line->cap >= needed) return;
    size_t new_cap = line->cap == 0 ? INITIAL_LINE_CAP : line->cap * 2;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    char *new_text = realloc(line->text, new_cap);
    if (!new_text) return;
    line->text = new_text;
    line->cap = new_cap;
}

TabBuf *tabbuf_create(void) {
    TabBuf *tb = calloc(1, sizeof(TabBuf));
    if (!tb) return NULL;

    tb->count = 1;
    tb->head = 0;
    tb->col = 0;
    tb->scroll_offset = 0;
    tb->ansi_state = ANSI_STATE_NORMAL;

    ensure_line_cap(&tb->lines[0], INITIAL_LINE_CAP);
    if (tb->lines[0].text) {
        tb->lines[0].text[0] = '\0';
        tb->lines[0].len = 0;
    }

    return tb;
}

void tabbuf_destroy(TabBuf *tb) {
    if (!tb) return;
    for (size_t i = 0; i < TABBUF_MAX_LINES; i++) {
        free(tb->lines[i].text);
    }
    free(tb);
}

void tabbuf_clear(TabBuf *tb) {
    if (!tb) return;
    for (size_t i = 0; i < TABBUF_MAX_LINES; i++) {
        if (tb->lines[i].text) {
            tb->lines[i].text[0] = '\0';
            tb->lines[i].len = 0;
        }
    }
    tb->count = 1;
    tb->head = 0;
    tb->col = 0;
    tb->scroll_offset = 0;
    tb->ansi_state = ANSI_STATE_NORMAL;
    tb->csi_len = 0;
    tb->csi_buf[0] = '\0';
}

static void advance_to_newline(TabBuf *tb) {
    tb->head = (tb->head + 1) % TABBUF_MAX_LINES;
    if (tb->count < TABBUF_MAX_LINES) {
        tb->count++;
    }

    TabLine *line = &tb->lines[tb->head];
    ensure_line_cap(line, INITIAL_LINE_CAP);
    line->len = 0;
    if (line->text) {
        line->text[0] = '\0';
    }
    tb->col = 0;
}

void tabbuf_append_bytes(TabBuf *tb, const char *data, size_t len) {
    if (!tb || !data || len == 0) return;

    tb->scroll_offset = 0; /* Auto-follow to bottom on new output */

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];

        if (tb->ansi_state == ANSI_STATE_NORMAL) {
            if (c == 0x1B) {
                tb->ansi_state = ANSI_STATE_ESC;
                continue;
            }
            if (c == '\n') {
                advance_to_newline(tb);
                continue;
            }
            if (c == '\r') {
                tb->col = 0;
                continue;
            }
            if (c == '\t') {
                size_t target_col = (tb->col + 8) & ~7;
                TabLine *line = &tb->lines[tb->head];
                ensure_line_cap(line, target_col + 1);
                if (line->text) {
                    while (tb->col < target_col) {
                        if (tb->col >= line->len) {
                            line->text[tb->col] = ' ';
                            line->len = tb->col + 1;
                            line->text[line->len] = '\0';
                        }
                        tb->col++;
                    }
                }
                continue;
            }
            if (c == '\b') {
                if (tb->col > 0) {
                    tb->col--;
                }
                continue;
            }
            if (c < 32 && c != '\t') {
                /* Discard other non-printable control characters */
                continue;
            }

            /* Regular printable or Latin-1 character */
            TabLine *line = &tb->lines[tb->head];
            ensure_line_cap(line, tb->col + 2);
            if (!line->text) continue;

            /* Pad with spaces if col jumped ahead */
            while (line->len < tb->col) {
                line->text[line->len++] = ' ';
            }

            line->text[tb->col] = (char)c;
            if (tb->col >= line->len) {
                line->len = tb->col + 1;
                line->text[line->len] = '\0';
            }
            tb->col++;
        } else if (tb->ansi_state == ANSI_STATE_ESC) {
            if (c == '[') {
                tb->ansi_state = ANSI_STATE_CSI;
                tb->csi_len = 0;
                tb->csi_buf[0] = '\0';
            } else if (c == ']') {
                tb->ansi_state = ANSI_STATE_OSC;
            } else {
                /* Unknown ESC sequence, return to normal */
                tb->ansi_state = ANSI_STATE_NORMAL;
            }
        } else if (tb->ansi_state == ANSI_STATE_CSI) {
            /* CSI parameter bytes: 0x20 - 0x3F; final byte: 0x40 - 0x7E */
            if (c >= 0x40 && c <= 0x7E) {
                if (c == 'J') {
                    int param = (tb->csi_len > 0) ? atoi(tb->csi_buf) : 0;
                    if (param == 2 || param == 3) {
                        tabbuf_clear(tb);
                    }
                } else if (c == 'H' || c == 'f') {
                    tb->col = 0;
                }
                tb->ansi_state = ANSI_STATE_NORMAL;
            } else {
                if (tb->csi_len + 1 < sizeof(tb->csi_buf)) {
                    tb->csi_buf[tb->csi_len++] = (char)c;
                    tb->csi_buf[tb->csi_len] = '\0';
                }
            }
        } else if (tb->ansi_state == ANSI_STATE_OSC) {
            /* Terminated by BEL (0x07) or ST (ESC \) */
            if (c == 0x07) {
                tb->ansi_state = ANSI_STATE_NORMAL;
            } else if (c == 0x1B) {
                /* Potential ST */
                tb->ansi_state = ANSI_STATE_ESC;
            }
        }
    }
}

void tabbuf_append_line(TabBuf *tb, const char *line) {
    if (!tb) return;
    if (line) {
        tabbuf_append_bytes(tb, line, strlen(line));
    }
    advance_to_newline(tb);
}

void tabbuf_scroll_up(TabBuf *tb, size_t n, size_t total_visual, size_t visible_rows) {
    if (!tb) return;
    if (total_visual <= visible_rows) {
        tb->scroll_offset = 0;
        return;
    }
    size_t max_scroll = total_visual - visible_rows;
    tb->scroll_offset += n;
    if (tb->scroll_offset > max_scroll) {
        tb->scroll_offset = max_scroll;
    }
}

void tabbuf_scroll_down(TabBuf *tb, size_t n) {
    if (!tb) return;
    if (tb->scroll_offset <= n) {
        tb->scroll_offset = 0;
    } else {
        tb->scroll_offset -= n;
    }
}

void tabbuf_scroll_to_bottom(TabBuf *tb) {
    if (tb) tb->scroll_offset = 0;
}

size_t tabbuf_total_visual_lines(const TabBuf *tb, size_t cols) {
    if (!tb || tb->count == 0) return 0;
    if (cols == 0) cols = 80;

    size_t total = 0;
    size_t oldest = (tb->head + TABBUF_MAX_LINES + 1 - tb->count) % TABBUF_MAX_LINES;

    for (size_t i = 0; i < tb->count; i++) {
        size_t idx = (oldest + i) % TABBUF_MAX_LINES;
        /* Skip trailing empty line at head */
        if (idx == tb->head && tb->lines[idx].len == 0) {
            continue;
        }
        size_t len = tb->lines[idx].len;
        size_t vlines = (len == 0) ? 1 : ((len + cols - 1) / cols);
        total += vlines;
    }

    return total;
}

void tabbuf_get_visual_line(const TabBuf *tb, size_t cols, size_t visual_idx,
                            const char **out_str, size_t *out_len) {
    if (!tb || !out_str || !out_len || tb->count == 0) {
        if (out_str) *out_str = "";
        if (out_len) *out_len = 0;
        return;
    }
    if (cols == 0) cols = 80;

    size_t cur_visual = 0;
    size_t oldest = (tb->head + TABBUF_MAX_LINES + 1 - tb->count) % TABBUF_MAX_LINES;

    for (size_t i = 0; i < tb->count; i++) {
        size_t idx = (oldest + i) % TABBUF_MAX_LINES;
        /* Skip trailing empty line at head */
        if (idx == tb->head && tb->lines[idx].len == 0) {
            continue;
        }
        const TabLine *line = &tb->lines[idx];
        size_t len = line->len;
        size_t vlines = (len == 0) ? 1 : ((len + cols - 1) / cols);

        if (visual_idx < cur_visual + vlines) {
            size_t sub_idx = visual_idx - cur_visual;
            size_t char_offset = sub_idx * cols;
            if (len == 0 || char_offset >= len) {
                *out_str = "";
                *out_len = 0;
            } else {
                *out_str = line->text ? line->text + char_offset : "";
                size_t remaining = len - char_offset;
                *out_len = remaining < cols ? remaining : cols;
            }
            return;
        }

        cur_visual += vlines;
    }

    *out_str = "";
    *out_len = 0;
}
