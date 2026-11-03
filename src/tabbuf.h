#ifndef TABBUF_H
#define TABBUF_H

#include <stddef.h>
#include <stdbool.h>

#define TABBUF_MAX_LINES 10000

typedef struct {
    char *text;
    size_t len;
    size_t cap;
} TabLine;

typedef enum {
    ANSI_STATE_NORMAL,
    ANSI_STATE_ESC,
    ANSI_STATE_CSI,
    ANSI_STATE_OSC
} AnsiState;

typedef struct {
    TabLine lines[TABBUF_MAX_LINES];
    size_t head;          /* Index of current active line being edited/written */
    size_t count;         /* Total valid logical lines (1 .. TABBUF_MAX_LINES) */
    size_t col;           /* Current column in the active line */
    size_t scroll_offset; /* Lines scrolled up from bottom (0 = follow bottom) */
    AnsiState ansi_state;
    char csi_buf[32];
    size_t csi_len;
} TabBuf;

TabBuf *tabbuf_create(void);
void tabbuf_destroy(TabBuf *tb);
void tabbuf_clear(TabBuf *tb);

/* Append bytes received from tab output (handles \n, \r, \t, \b, drops ANSI escapes) */
void tabbuf_append_bytes(TabBuf *tb, const char *data, size_t len);

/* Append a completed line (e.g. committed prompt + command) */
void tabbuf_append_line(TabBuf *tb, const char *line);

/* Scrolling */
void tabbuf_scroll_up(TabBuf *tb, size_t n, size_t total_visual, size_t visible_rows);
void tabbuf_scroll_down(TabBuf *tb, size_t n);
void tabbuf_scroll_to_bottom(TabBuf *tb);

/* Visual wrapping helpers for rendering */
size_t tabbuf_total_visual_lines(const TabBuf *tb, size_t cols);

/* Fill a visual line chunk (from visual_row index 0 .. total_visual_lines - 1) */
void tabbuf_get_visual_line(const TabBuf *tb, size_t cols, size_t visual_idx,
                            const char **out_str, size_t *out_len);

#endif /* TABBUF_H */
