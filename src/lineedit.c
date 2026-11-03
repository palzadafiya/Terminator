/*
 * lineedit.c: In-memory live command line editor.
 *
 * Implements pure C, X-free live input line buffer manipulations:
 * - Character/text insertion, backspace, forward delete, start/end navigation.
 * - Word boundary navigation and deletion (kill to start, kill to end, delete previous word).
 * - Arbitrary range replacement for auto-completion (lineedit_replace_range).
 * - Row and column layout calculation for multi-line visual wrapped rendering.
 */

#include "lineedit.h"
#include <string.h>
#include <ctype.h>

void lineedit_init(LineEdit *le) {
    if (!le) return;
    le->len = 0;
    le->cursor = 0;
    le->buf[0] = '\0';
}

void lineedit_clear(LineEdit *le) {
    if (!le) return;
    le->len = 0;
    le->cursor = 0;
    le->buf[0] = '\0';
}

bool lineedit_insert_char(LineEdit *le, char c) {
    return lineedit_insert_text(le, &c, 1);
}

bool lineedit_insert_text(LineEdit *le, const char *s, size_t n) {
    if (!le || !s || n == 0) return false;
    if (le->len + n >= LINEEDIT_MAX) return false;

    if (le->cursor < le->len) {
        memmove(le->buf + le->cursor + n, le->buf + le->cursor, le->len - le->cursor);
    }
    memcpy(le->buf + le->cursor, s, n);
    le->len += n;
    le->cursor += n;
    le->buf[le->len] = '\0';
    return true;
}

bool lineedit_backspace(LineEdit *le) {
    if (!le || le->cursor == 0) return false;

    memmove(le->buf + le->cursor - 1, le->buf + le->cursor, le->len - le->cursor);
    le->cursor--;
    le->len--;
    le->buf[le->len] = '\0';
    return true;
}

bool lineedit_delete(LineEdit *le) {
    if (!le || le->cursor >= le->len) return false;

    memmove(le->buf + le->cursor, le->buf + le->cursor + 1, le->len - le->cursor - 1);
    le->len--;
    le->buf[le->len] = '\0';
    return true;
}

void lineedit_move_left(LineEdit *le) {
    if (le && le->cursor > 0) le->cursor--;
}

void lineedit_move_right(LineEdit *le) {
    if (le && le->cursor < le->len) le->cursor++;
}

void lineedit_move_home(LineEdit *le) {
    if (le) le->cursor = 0;
}

void lineedit_move_end(LineEdit *le) {
    if (le) le->cursor = le->len;
}

void lineedit_delete_to_start(LineEdit *le) {
    if (!le || le->cursor == 0) return;
    memmove(le->buf, le->buf + le->cursor, le->len - le->cursor);
    le->len -= le->cursor;
    le->cursor = 0;
    le->buf[le->len] = '\0';
}

void lineedit_delete_to_end(LineEdit *le) {
    if (!le) return;
    le->len = le->cursor;
    le->buf[le->len] = '\0';
}

void lineedit_delete_word_prev(LineEdit *le) {
    if (!le || le->cursor == 0) return;

    size_t i = le->cursor;
    /* Skip whitespace backwards */
    while (i > 0 && isspace((unsigned char)le->buf[i - 1])) {
        i--;
    }
    /* Skip non-whitespace backwards */
    while (i > 0 && !isspace((unsigned char)le->buf[i - 1])) {
        i--;
    }

    size_t removed = le->cursor - i;
    memmove(le->buf + i, le->buf + le->cursor, le->len - le->cursor);
    le->len -= removed;
    le->cursor = i;
    le->buf[le->len] = '\0';
}

void lineedit_kill_to_start(LineEdit *le) {
    lineedit_delete_to_start(le);
}

void lineedit_kill_to_end(LineEdit *le) {
    lineedit_delete_to_end(le);
}

void lineedit_delete_prev_word(LineEdit *le) {
    lineedit_delete_word_prev(le);
}

void lineedit_line_start(LineEdit *le) {
    lineedit_move_home(le);
}

void lineedit_line_end(LineEdit *le) {
    lineedit_move_end(le);
}

void lineedit_compute_layout(size_t prompt_len, size_t input_len, size_t cursor, size_t cols,
                             size_t *out_vlines, size_t *out_cur_row, size_t *out_cur_col) {
    if (cols == 0) cols = 80;
    size_t total_len = prompt_len + input_len;
    size_t vlines = (total_len == 0) ? 1 : ((total_len + cols - 1) / cols);

    size_t cur_pos = prompt_len + cursor;
    size_t cur_row = cur_pos / cols;
    size_t cur_col = cur_pos % cols;

    if (vlines <= cur_row) {
        vlines = cur_row + 1;
    }

    if (out_vlines) *out_vlines = vlines;
    if (out_cur_row) *out_cur_row = cur_row;
    if (out_cur_col) *out_cur_col = cur_col;
}

void lineedit_set_text(LineEdit *le, const char *text) {
    if (!le) return;
    lineedit_clear(le);
    if (text) {
        size_t n = strlen(text);
        lineedit_insert_text(le, text, n);
    }
}

const char *lineedit_get_text(const LineEdit *le) {
    return le ? le->buf : "";
}

bool lineedit_replace_range(LineEdit *le, size_t start, size_t end, const char *repl) {
    if (!le || !repl) return false;
    if (start > end || end > le->len) return false;

    size_t repl_len = strlen(repl);
    size_t suffix_len = le->len - end;
    size_t new_len = start + repl_len + suffix_len;
    if (new_len >= LINEEDIT_MAX) return false;

    memmove(le->buf + start + repl_len, le->buf + end, suffix_len);
    memcpy(le->buf + start, repl, repl_len);
    le->len = new_len;
    le->buf[new_len] = '\0';
    le->cursor = start + repl_len;
    return true;
}
