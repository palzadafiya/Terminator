#ifndef LINEEDIT_H
#define LINEEDIT_H

#include <stddef.h>
#include <stdbool.h>

#define LINEEDIT_MAX 4096

typedef struct {
    char buf[LINEEDIT_MAX];
    size_t len;
    size_t cursor; /* byte offset of cursor within buf (0 <= cursor <= len) */
} LineEdit;

void lineedit_init(LineEdit *le);
void lineedit_clear(LineEdit *le);

bool lineedit_insert_char(LineEdit *le, char c);
bool lineedit_insert_text(LineEdit *le, const char *s, size_t n);

bool lineedit_backspace(LineEdit *le);
bool lineedit_delete(LineEdit *le);

void lineedit_move_left(LineEdit *le);
void lineedit_move_right(LineEdit *le);
void lineedit_move_home(LineEdit *le);
void lineedit_move_end(LineEdit *le);

void lineedit_delete_to_start(LineEdit *le);
void lineedit_delete_to_end(LineEdit *le);
void lineedit_delete_word_prev(LineEdit *le);

/* Spec aliases */
void lineedit_kill_to_start(LineEdit *le);
void lineedit_kill_to_end(LineEdit *le);
void lineedit_delete_prev_word(LineEdit *le);
void lineedit_line_start(LineEdit *le);
void lineedit_line_end(LineEdit *le);

void lineedit_set_text(LineEdit *le, const char *text);
const char *lineedit_get_text(const LineEdit *le);
bool lineedit_replace_range(LineEdit *le, size_t start, size_t end, const char *repl);

/* Visual row/col layout computation for wrapped rendering */
void lineedit_compute_layout(size_t prompt_len, size_t input_len, size_t cursor, size_t cols,
                             size_t *out_vlines, size_t *out_cur_row, size_t *out_cur_col);

#endif /* LINEEDIT_H */
