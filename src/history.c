/*
 * history.c: Persistent, shared command history and reverse search.
 *
 * Implements a 10,000-entry shared memory circular ring buffer, persistent disk storage
 * at ~/.terminator_history with atomic append writes and startup compaction, and interactive
 * reverse search (Ctrl+R) using dynamic programming Longest Common Substring (LCS) with rolling rows.
 */

#define _GNU_SOURCE
#include "shared.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <pwd.h>

static Shared *s_default_shared = NULL;

static bool is_all_whitespace(const char *s) {
    if (!s) return true;
    while (*s) {
        if (!isspace((unsigned char)*s)) return false;
        s++;
    }
    return true;
}

void hist_init(Shared *sh) {
    s_default_shared = sh;
}

void hist_search_result_free(HistSearchResult *res) {
    if (!res) return;
    if (res->entries) {
        for (size_t i = 0; i < res->count; i++) {
            free(res->entries[i]);
        }
        free(res->entries);
        res->entries = NULL;
    }
    res->count = 0;
    res->match_len = 0;
    res->match_type = HIST_MATCH_NONE;
}

size_t history_encode_line(const char *raw, char *out, size_t out_cap) {
    if (!raw || !out || out_cap < 2) return 0;

    size_t out_len = 0;
    for (size_t i = 0; raw[i] != '\0' && out_len + 3 < out_cap; i++) {
        char c = raw[i];
        if (c == '\\') {
            out[out_len++] = '\\';
            out[out_len++] = '\\';
        } else if (c == '\n') {
            out[out_len++] = '\\';
            out[out_len++] = 'n';
        } else if (c == '\r') {
            out[out_len++] = '\\';
            out[out_len++] = 'r';
        } else {
            out[out_len++] = c;
        }
    }

    /* Trailing newline delimiter */
    if (out_len + 1 < out_cap) {
        out[out_len++] = '\n';
    }
    out[out_len] = '\0';
    return out_len;
}

size_t history_decode_line(const char *in, size_t in_len, char *out, size_t out_cap) {
    if (!in || !out || out_cap == 0) return 0;

    size_t out_len = 0;
    for (size_t i = 0; i < in_len && out_len + 1 < out_cap; i++) {
        char c = in[i];
        if (c == '\\' && i + 1 < in_len) {
            char next = in[i + 1];
            if (next == 'n') {
                out[out_len++] = '\n';
                i++;
            } else if (next == 'r') {
                out[out_len++] = '\r';
                i++;
            } else if (next == '\\') {
                out[out_len++] = '\\';
                i++;
            } else {
                out[out_len++] = '\\';
            }
        } else {
            out[out_len++] = c;
        }
    }
    out[out_len] = '\0';
    return out_len;
}

static void resolve_histfile_path(Shared *sh) {
    if (!sh) return;
    if (sh->histfile[0] != '\0') return;

    const char *env_hist = getenv("TERMINATOR_HISTFILE");
    if (env_hist && env_hist[0]) {
        strncpy(sh->histfile, env_hist, sizeof(sh->histfile) - 1);
        sh->histfile[sizeof(sh->histfile) - 1] = '\0';
        return;
    }

    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) {
            home = pw->pw_dir;
        }
    }

    if (home && home[0]) {
        snprintf(sh->histfile, sizeof(sh->histfile), "%s/.terminator_history", home);
    } else {
        snprintf(sh->histfile, sizeof(sh->histfile), "./.terminator_history");
    }
}

int history_init_persistence(Shared *sh) {
    if (!sh) return -1;
    resolve_histfile_path(sh);

    int fd = open(sh->histfile, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        /* If file does not exist, initialize empty file with mode 0600 */
        int create_fd = open(sh->histfile, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
        if (create_fd >= 0) {
            close(create_fd);
        }
        return 0;
    }

    char chunk[65536];
    char line_buf[HISTORY_MAX_LINE * 2 + 2];
    size_t line_len = 0;
    size_t total_lines_read = 0;

    ssize_t nread = 0;
    while ((nread = read(fd, chunk, sizeof(chunk))) > 0) {
        for (ssize_t i = 0; i < nread; i++) {
            char c = chunk[i];
            if (c == '\n') {
                line_buf[line_len] = '\0';
                char decoded[HISTORY_MAX_LINE];
                history_decode_line(line_buf, line_len, decoded, sizeof(decoded));
                if (!is_all_whitespace(decoded)) {
                    uint32_t idx = sh->history.head;
                    sh->history.entries[idx].seq = sh->history.next_seq++;
                    strncpy(sh->history.entries[idx].text, decoded, HISTORY_MAX_LINE - 1);
                    sh->history.entries[idx].text[HISTORY_MAX_LINE - 1] = '\0';

                    sh->history.head = (sh->history.head + 1) % HISTORY_CAPACITY;
                    if (sh->history.count < HISTORY_CAPACITY) {
                        sh->history.count++;
                    }
                    total_lines_read++;
                }
                line_len = 0;
            } else {
                if (line_len + 1 < sizeof(line_buf)) {
                    line_buf[line_len++] = c;
                }
            }
        }
    }
    close(fd);

    /* Handle partial last line if no trailing newline */
    if (line_len > 0) {
        line_buf[line_len] = '\0';
        char decoded[HISTORY_MAX_LINE];
        history_decode_line(line_buf, line_len, decoded, sizeof(decoded));
        if (!is_all_whitespace(decoded)) {
            uint32_t idx = sh->history.head;
            sh->history.entries[idx].seq = sh->history.next_seq++;
            strncpy(sh->history.entries[idx].text, decoded, HISTORY_MAX_LINE - 1);
            sh->history.entries[idx].text[HISTORY_MAX_LINE - 1] = '\0';

            sh->history.head = (sh->history.head + 1) % HISTORY_CAPACITY;
            if (sh->history.count < HISTORY_CAPACITY) {
                sh->history.count++;
            }
            total_lines_read++;
        }
    }

    /* Requirement 1: If file grows past ~2x the cap (20,000 entries),
     * rewrite it atomically at startup under no contention. */
    if (total_lines_read >= 20000 && sh->history.count > 0) {
        char tmp_path[1050];
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", sh->histfile, (int)getpid());
        int wfd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (wfd >= 0) {
            uint32_t cnt = sh->history.count;
            uint64_t oldest = sh->history.next_seq - cnt;
            for (uint32_t i = 0; i < cnt; i++) {
                uint64_t s = oldest + i;
                uint64_t offset = (sh->history.next_seq - 1) - s;
                uint32_t head = sh->history.head;
                uint32_t idx = (head + HISTORY_CAPACITY - 1 - (offset % HISTORY_CAPACITY)) % HISTORY_CAPACITY;

                char enc[HISTORY_MAX_LINE * 2 + 2];
                size_t enc_len = history_encode_line(sh->history.entries[idx].text, enc, sizeof(enc));
                if (enc_len > 0) {
                    size_t written = 0;
                    while (written < enc_len) {
                        ssize_t w = write(wfd, enc + written, enc_len - written);
                        if (w <= 0) {
                            if (w < 0 && errno == EINTR) continue;
                            break;
                        }
                        written += (size_t)w;
                    }
                }
            }
            fsync(wfd);
            close(wfd);
            rename(tmp_path, sh->histfile);
        }
    }

    return 0;
}

uint64_t history_append(Shared *sh, const char *line) {
    if (!sh || !line || is_all_whitespace(line)) {
        return 0;
    }

    if (shared_lock(sh) != 0) {
        return 0;
    }

    uint64_t seq = sh->history.next_seq++;
    uint32_t idx = sh->history.head;

    sh->history.entries[idx].seq = seq;
    strncpy(sh->history.entries[idx].text, line, HISTORY_MAX_LINE - 1);
    sh->history.entries[idx].text[HISTORY_MAX_LINE - 1] = '\0';

    sh->history.head = (sh->history.head + 1) % HISTORY_CAPACITY;
    if (sh->history.count < HISTORY_CAPACITY) {
        sh->history.count++;
    }

    /* Requirement 1: Single O_APPEND write while holding the lock */
    resolve_histfile_path(sh);
    if (sh->histfile[0] != '\0') {
        char enc[HISTORY_MAX_LINE * 2 + 2];
        size_t enc_len = history_encode_line(sh->history.entries[idx].text, enc, sizeof(enc));
        if (enc_len > 0) {
            int fd = open(sh->histfile, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
            if (fd >= 0) {
                size_t written = 0;
                while (written < enc_len) {
                    ssize_t w = write(fd, enc + written, enc_len - written);
                    if (w <= 0) {
                        if (w < 0 && errno == EINTR) continue;
                        break;
                    }
                    written += (size_t)w;
                }
                close(fd);
            }
        }
    }

    shared_unlock(sh);
    return seq;
}

uint32_t history_count(Shared *sh) {
    if (!sh) return 0;
    if (shared_lock(sh) != 0) return 0;
    uint32_t count = sh->history.count;
    shared_unlock(sh);
    return count;
}

uint64_t history_oldest_seq(Shared *sh) {
    if (!sh) return 0;
    if (shared_lock(sh) != 0) return 0;
    uint64_t seq = 0;
    if (sh->history.count > 0) {
        seq = sh->history.next_seq - sh->history.count;
    }
    shared_unlock(sh);
    return seq;
}

uint64_t history_newest_seq(Shared *sh) {
    if (!sh) return 0;
    if (shared_lock(sh) != 0) return 0;
    uint64_t seq = 0;
    if (sh->history.count > 0) {
        seq = sh->history.next_seq - 1;
    }
    shared_unlock(sh);
    return seq;
}

int history_get_by_seq(Shared *sh, uint64_t seq, char *out, size_t out_cap) {
    if (!sh || !out || out_cap == 0 || seq == 0) return -1;
    if (shared_lock(sh) != 0) return -1;

    if (sh->history.count == 0) {
        shared_unlock(sh);
        return -1;
    }

    uint64_t oldest = sh->history.next_seq - sh->history.count;
    uint64_t newest = sh->history.next_seq - 1;

    if (seq < oldest || seq > newest) {
        shared_unlock(sh);
        return -1;
    }

    uint64_t offset_from_newest = newest - seq;
    uint32_t head = sh->history.head;
    uint32_t idx = (head + HISTORY_CAPACITY - 1 - (offset_from_newest % HISTORY_CAPACITY)) % HISTORY_CAPACITY;

    if (sh->history.entries[idx].seq != seq) {
        shared_unlock(sh);
        return -1;
    }

    strncpy(out, sh->history.entries[idx].text, out_cap - 1);
    out[out_cap - 1] = '\0';

    shared_unlock(sh);
    return 0;
}

size_t history_snapshot(Shared *sh, char lines[][HISTORY_MAX_LINE], uint64_t *seqs, size_t max_items) {
    if (!sh || !lines || max_items == 0) return 0;
    if (shared_lock(sh) != 0) return 0;

    uint32_t count = sh->history.count;
    if (count == 0) {
        shared_unlock(sh);
        return 0;
    }

    size_t to_copy = count < max_items ? count : max_items;
    uint64_t oldest = sh->history.next_seq - to_copy;

    for (size_t i = 0; i < to_copy; i++) {
        uint64_t s = oldest + i;
        uint64_t offset_from_newest = (sh->history.next_seq - 1) - s;
        uint32_t head = sh->history.head;
        uint32_t idx = (head + HISTORY_CAPACITY - 1 - (offset_from_newest % HISTORY_CAPACITY)) % HISTORY_CAPACITY;

        if (seqs) seqs[i] = s;
        strncpy(lines[i], sh->history.entries[idx].text, HISTORY_MAX_LINE - 1);
        lines[i][HISTORY_MAX_LINE - 1] = '\0';
    }

    shared_unlock(sh);
    return to_copy;
}

size_t history_snapshot_last(Shared *sh, size_t n, HistoryEntry *out) {
    if (!sh || !out || n == 0) return 0;
    if (shared_lock(sh) != 0) return 0;

    uint32_t count = sh->history.count;
    if (count == 0) {
        shared_unlock(sh);
        return 0;
    }

    size_t to_copy = count < n ? count : n;
    uint64_t oldest = sh->history.next_seq - to_copy;

    for (size_t i = 0; i < to_copy; i++) {
        uint64_t s = oldest + i;
        uint64_t offset = (sh->history.next_seq - 1) - s;
        uint32_t head = sh->history.head;
        uint32_t idx = (head + HISTORY_CAPACITY - 1 - (offset % HISTORY_CAPACITY)) % HISTORY_CAPACITY;

        out[i].seq = s;
        strncpy(out[i].text, sh->history.entries[idx].text, HISTORY_MAX_LINE - 1);
        out[i].text[HISTORY_MAX_LINE - 1] = '\0';
    }

    shared_unlock(sh);
    return to_copy;
}

/* Dynamic programming Longest Common Substring with two rolling rows
 * Time complexity: O(len(term) * len(cmd))
 * Space complexity: O(len(term))
 */
static size_t lcs_length(const char *term, size_t m, const char *cmd, size_t n) {
    if (m == 0 || n == 0) return 0;
    if (m > HIST_TERM_MAX) m = HIST_TERM_MAX;

    uint16_t row0[HIST_TERM_MAX + 1];
    uint16_t row1[HIST_TERM_MAX + 1];
    memset(row0, 0, sizeof(row0));
    memset(row1, 0, sizeof(row1));

    uint16_t *prev = row0;
    uint16_t *curr = row1;
    size_t max_len = 0;

    for (size_t j = 0; j < n; j++) {
        char c_cmd = cmd[j];
        for (size_t i = 0; i < m; i++) {
            if (term[i] == c_cmd) {
                curr[i + 1] = (uint16_t)(prev[i] + 1);
                if (curr[i + 1] > max_len) {
                    max_len = curr[i + 1];
                }
            } else {
                curr[i + 1] = 0;
            }
        }
        uint16_t *tmp = prev;
        prev = curr;
        curr = tmp;
    }

    return max_len;
}

int history_search(Shared *sh, const char *term, HistSearchResult *result) {
    if (!result) return -1;
    memset(result, 0, sizeof(*result));
    if (!sh || !term || is_all_whitespace(term)) {
        result->match_type = HIST_MATCH_NONE;
        return 0;
    }

    /* Snapshot the entire history ring under the lock, then release lock immediately */
    HistoryEntry *snapshot = malloc(HISTORY_CAPACITY * sizeof(HistoryEntry));
    if (!snapshot) return -1;

    size_t count = history_snapshot_last(sh, HISTORY_CAPACITY, snapshot);
    if (count == 0) {
        free(snapshot);
        result->match_type = HIST_MATCH_NONE;
        return 0;
    }

    size_t term_len = strlen(term);
    if (term_len > HIST_TERM_MAX) {
        term_len = HIST_TERM_MAX;
    }

    /* Decision order rule (a):
     * "the most recent command that exactly matches the term (equal string)"
     * Search newest to oldest. */
    for (size_t i = count; i > 0; i--) {
        const HistoryEntry *entry = &snapshot[i - 1];
        if (strcmp(entry->text, term) == 0) {
            result->match_type = HIST_MATCH_EXACT;
            result->match_len = term_len;
            result->count = 1;
            result->entries = malloc(sizeof(char *));
            if (result->entries) {
                result->entries[0] = strdup(entry->text);
            }
            free(snapshot);
            return 0;
        }
    }

    /* Decision order rule (b):
     * "the command(s) sharing the longest common substring with the term,
     *  provided that match is longer than 2 characters; print all commands tied
     *  for that maximum length, most recent first, without duplicate lines" */
    size_t *lcs_scores = malloc(count * sizeof(size_t));
    if (!lcs_scores) {
        free(snapshot);
        return -1;
    }

    size_t max_lcs = 0;
    for (size_t i = 0; i < count; i++) {
        size_t l = lcs_length(term, term_len, snapshot[i].text, strlen(snapshot[i].text));
        lcs_scores[i] = l;
        if (l > max_lcs) {
            max_lcs = l;
        }
    }

    /* Requirement: match must be longer than 2 characters */
    if (max_lcs <= 2) {
        /* Rule (c): No match */
        free(lcs_scores);
        free(snapshot);
        result->match_type = HIST_MATCH_NONE;
        return 0;
    }

    /* Collect all entries tied for max_lcs, most recent first, without duplicate lines */
    size_t cap = 16;
    result->entries = malloc(cap * sizeof(char *));
    if (!result->entries) {
        free(lcs_scores);
        free(snapshot);
        return -1;
    }
    result->count = 0;
    result->match_type = HIST_MATCH_SUBSTRING;
    result->match_len = max_lcs;

    for (size_t i = count; i > 0; i--) {
        size_t idx = i - 1;
        if (lcs_scores[idx] == max_lcs) {
            const char *cand = snapshot[idx].text;
            /* Deduplicate */
            bool dup = false;
            for (size_t d = 0; d < result->count; d++) {
                if (strcmp(result->entries[d], cand) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                if (result->count >= cap) {
                    size_t new_cap = cap * 2;
                    char **tmp = realloc(result->entries, new_cap * sizeof(char *));
                    if (!tmp) break;
                    result->entries = tmp;
                    cap = new_cap;
                }
                result->entries[result->count++] = strdup(cand);
            }
        }
    }

    free(lcs_scores);
    free(snapshot);
    return 0;
}

/* Default instance wrappers */
uint64_t hist_append_default(const char *text) {
    return history_append(s_default_shared, text);
}

uint32_t hist_count_default(void) {
    return history_count(s_default_shared);
}

int hist_get_by_seq_default(uint64_t seq, char *out, size_t out_cap) {
    return history_get_by_seq(s_default_shared, seq, out, out_cap);
}

size_t hist_snapshot_last_default(size_t n, HistoryEntry *out) {
    return history_snapshot_last(s_default_shared, n, out);
}

int hist_search_default(const char *term, HistSearchResult *result) {
    return history_search(s_default_shared, term, result);
}
