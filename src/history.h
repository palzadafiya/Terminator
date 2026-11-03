#ifndef HISTORY_H
#define HISTORY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define HISTORY_CAPACITY 10000
#define HISTORY_MAX_LINE 1024
#define HIST_TERM_MAX    256

typedef struct {
    uint64_t seq;
    char text[HISTORY_MAX_LINE];
} HistoryEntry;

typedef struct {
    uint64_t next_seq; /* monotonic counter, starts at 1 */
    uint32_t count;    /* total valid entries (0 .. HISTORY_CAPACITY) */
    uint32_t head;     /* next write index in ring (0 .. HISTORY_CAPACITY-1) */
    HistoryEntry entries[HISTORY_CAPACITY];
} HistoryRing;

/* Search Match Types */
#define HIST_MATCH_NONE      0
#define HIST_MATCH_EXACT     1
#define HIST_MATCH_SUBSTRING 2

typedef struct {
    int match_type;       /* HIST_MATCH_NONE, HIST_MATCH_EXACT, HIST_MATCH_SUBSTRING */
    size_t match_len;     /* Length of matching substring (term len for exact, LCS len for substring) */
    size_t count;         /* Number of unique matching command entries */
    char **entries;       /* Dynamically allocated array of matching strings */
} HistSearchResult;

void hist_search_result_free(HistSearchResult *res);

/* Line encoding/decoding helpers for persistence */
size_t history_encode_line(const char *raw, char *out, size_t out_cap);
size_t history_decode_line(const char *in, size_t in_len, char *out, size_t out_cap);

/* Forward declaration of Shared */
struct Shared;

/* Core History Operations on Shared */
int history_init_persistence(struct Shared *sh);
uint64_t history_append(struct Shared *sh, const char *line);
uint32_t history_count(struct Shared *sh);
uint64_t history_oldest_seq(struct Shared *sh);
uint64_t history_newest_seq(struct Shared *sh);
int history_get_by_seq(struct Shared *sh, uint64_t seq, char *out, size_t out_cap);
size_t history_snapshot(struct Shared *sh, char lines[][HISTORY_MAX_LINE], uint64_t *seqs, size_t max_items);
size_t history_snapshot_last(struct Shared *sh, size_t n, HistoryEntry *out);
int history_search(struct Shared *sh, const char *term, HistSearchResult *result);

/* Default instance / convenience API (hist_*) */
void hist_init(struct Shared *sh);
uint64_t hist_append_default(const char *text);
uint32_t hist_count_default(void);
int hist_get_by_seq_default(uint64_t seq, char *out, size_t out_cap);
size_t hist_snapshot_last_default(size_t n, HistoryEntry *out);
int hist_search_default(const char *term, HistSearchResult *result);

/* Macro bindings supporting both hist_*(...) and hist_*(sh, ...) */
#define _HIST_APP_1(t)           hist_append_default(t)
#define _HIST_APP_2(sh, t)       history_append((sh), (t))
#define _HIST_APP_SEL(_1, _2, NAME, ...) NAME
#define hist_append(...)         _HIST_APP_SEL(__VA_ARGS__, _HIST_APP_2, _HIST_APP_1)(__VA_ARGS__)

#define _HIST_CNT_0()            hist_count_default()
#define _HIST_CNT_1(sh)          history_count(sh)
#define _HIST_CNT_SEL(_0, _1, NAME, ...) NAME
#define hist_count(...)          _HIST_CNT_SEL(dummy, ##__VA_ARGS__, _HIST_CNT_1, _HIST_CNT_0)(__VA_ARGS__)

#define _HIST_GBS_1(s)           hist_get_by_seq_default(s, NULL, 0)
#define _HIST_GBS_3(s, o, c)     hist_get_by_seq_default(s, o, c)
#define _HIST_GBS_4(sh, s, o, c) history_get_by_seq(sh, s, o, c)
#define _HIST_GBS_SEL(_1, _2, _3, _4, NAME, ...) NAME
#define hist_get_by_seq(...)     _HIST_GBS_SEL(__VA_ARGS__, _HIST_GBS_4, _HIST_GBS_3, dummy, _HIST_GBS_1)(__VA_ARGS__)

#define _HIST_SNAP_2(n, out)     hist_snapshot_last_default(n, out)
#define _HIST_SNAP_3(sh, n, out) history_snapshot_last(sh, n, out)
#define _HIST_SNAP_SEL(_1, _2, _3, NAME, ...) NAME
#define hist_snapshot_last(...)  _HIST_SNAP_SEL(__VA_ARGS__, _HIST_SNAP_3, _HIST_SNAP_2)(__VA_ARGS__)

#define _HIST_SRCH_2(t, r)       hist_search_default(t, r)
#define _HIST_SRCH_3(sh, t, r)   history_search(sh, t, r)
#define _HIST_SRCH_SEL(_1, _2, _3, NAME, ...) NAME
#define hist_search(...)         _HIST_SRCH_SEL(__VA_ARGS__, _HIST_SRCH_3, _HIST_SRCH_2)(__VA_ARGS__)

#endif /* HISTORY_H */
