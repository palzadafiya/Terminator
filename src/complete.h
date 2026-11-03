#ifndef COMPLETE_H
#define COMPLETE_H

#include <stddef.h>
#include <stdbool.h>

#define COMPLETE_MAX_CANDIDATES 10000
#define COMPLETE_MAX_WORD 1024
#define COMPLETE_MAX_PATH 4096

typedef enum {
    COMPL_STATE_NONE = 0,
    COMPL_STATE_ASK_CONFIRM,  /* "Display all N possibilities? (y or n)" */
    COMPL_STATE_SELECT        /* Numbered selection mode: digits accumulate, Enter confirms */
} CompletionState;

typedef struct {
    char **items;                       /* Array of candidate strings (e.g. "abc.txt", "dir/") */
    size_t count;                       /* Total matching candidates */
    char lcp[COMPLETE_MAX_WORD];        /* Longest Common Prefix of all candidates */
    char dir_prefix[COMPLETE_MAX_WORD]; /* Dir prefix as typed (e.g. "src/", "~/", "") */
    char unescaped_prefix[COMPLETE_MAX_WORD]; /* Typed file prefix unescaped */
    size_t word_start;                  /* Start index of word in input buffer */
    size_t word_len;                    /* Length of word up to cursor */
} CompletionResult;

/* Free any dynamically allocated memory inside CompletionResult */
void completion_result_free(CompletionResult *cr);

/* 1. Extract the word ending at cursor from input buffer.
 * Splits on unescaped whitespace.
 * Returns word_start offset, and sets out_word_len. */
size_t complete_extract_word(const char *buf, size_t cursor, size_t *out_word_len);

/* 2. Decompose a typed word into typed dir_prefix and unescaped file_prefix */
void complete_decompose_word(const char *word, size_t word_len,
                             char *out_dir_prefix, size_t dir_prefix_cap,
                             char *out_file_prefix, size_t file_prefix_cap);

/* 3. Escape filename for safe insertion into shell command line.
 * Escapes whitespace and shell metacharacters with backslashes.
 * Trailing slash on directories is NOT escaped. */
size_t complete_escape_filename(const char *raw, char *out, size_t out_cap);

/* 4. Unescape a word (removes backslashes that escape characters) */
size_t complete_unescape_word(const char *in, size_t in_len, char *out, size_t out_cap);

/* 5. Compute the Longest Common Prefix (LCP) of an array of strings */
size_t complete_compute_lcp(char * const *items, size_t count, char *out_lcp, size_t out_cap);

/* 6. Perform file-name completion for line buffer at cursor with given tab cwd.
 * Allocates and populates CompletionResult. */
int complete_file_candidates(const char *line, size_t cursor, const char *cwd,
                             CompletionResult *result);

/* 7. Format a numbered list of candidates into rows/columns for display.
 * e.g. "1. abc.txt  2. abcd.txt\n" */
char *complete_format_numbered_list(const CompletionResult *cr, int term_cols);

#endif /* COMPLETE_H */
