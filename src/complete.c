/*
 * complete.c: Tab-key file-name auto-completion subsystem.
 *
 * Provides X-free, portable filename auto-completion:
 * - Word boundary extraction ending at cursor with quote and escape support.
 * - Path decomposition into typed dir_prefix and unescaped filename prefix.
 * - Candidate gathering relative to the tab's cwd (zero GUI chdir()).
 * - Alphabetical sorting, dotfile filtering, directory trailing slashes.
 * - Longest Common Prefix (LCP) calculation and numbered candidate list formatting.
 * - Backslash escaping of shell metacharacters for safe command insertion.
 */

#define _GNU_SOURCE
#include "complete.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

void completion_result_free(CompletionResult *cr) {
    if (!cr) return;
    if (cr->items) {
        for (size_t i = 0; i < cr->count; i++) {
            free(cr->items[i]);
        }
        free(cr->items);
        cr->items = NULL;
    }
    cr->count = 0;
    cr->lcp[0] = '\0';
    cr->dir_prefix[0] = '\0';
    cr->unescaped_prefix[0] = '\0';
    cr->word_start = 0;
    cr->word_len = 0;
}

static bool is_shell_meta(char c) {
    switch (c) {
        case ' ':  case '\t': case '\n':
        case '|':  case '&':  case ';':
        case '<':  case '>':  case '(':
        case ')':  case '$':  case '`':
        case '"':  case '\'': case '\\':
        case '*':  case '?':  case '[':
        case ']':  case '!':  case '#':
        case '~':  case '=':
            return true;
        default:
            return false;
    }
}

size_t complete_extract_word(const char *buf, size_t cursor, size_t *out_word_len) {
    if (!buf || cursor == 0) {
        if (out_word_len) *out_word_len = 0;
        return cursor;
    }

    size_t word_start = 0;
    bool in_squote = false;
    bool in_dquote = false;

    for (size_t i = 0; i < cursor; i++) {
        if (buf[i] == '\\' && !in_squote) {
            i++; /* Skip escaped character */
            continue;
        }
        if (buf[i] == '\'' && !in_dquote) {
            in_squote = !in_squote;
            continue;
        }
        if (buf[i] == '"' && !in_squote) {
            in_dquote = !in_dquote;
            continue;
        }
        if (!in_squote && !in_dquote && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n')) {
            word_start = i + 1;
        }
    }

    if (out_word_len) {
        *out_word_len = cursor - word_start;
    }
    return word_start;
}

size_t complete_unescape_word(const char *in, size_t in_len, char *out, size_t out_cap) {
    if (!out || out_cap == 0) return 0;
    if (!in || in_len == 0) {
        out[0] = '\0';
        return 0;
    }

    size_t out_len = 0;
    for (size_t i = 0; i < in_len && out_len + 1 < out_cap; i++) {
        if (in[i] == '\\' && i + 1 < in_len) {
            out[out_len++] = in[i + 1];
            i++;
        } else if (in[i] != '\'' && in[i] != '"') {
            out[out_len++] = in[i];
        }
    }
    out[out_len] = '\0';
    return out_len;
}

void complete_decompose_word(const char *word, size_t word_len,
                             char *out_dir_prefix, size_t dir_prefix_cap,
                             char *out_file_prefix, size_t file_prefix_cap) {
    if (out_dir_prefix && dir_prefix_cap > 0) out_dir_prefix[0] = '\0';
    if (out_file_prefix && file_prefix_cap > 0) out_file_prefix[0] = '\0';
    if (!word || word_len == 0) return;

    /* Scan backwards to find the last unescaped directory separator '/' */
    ssize_t last_slash = -1;
    for (ssize_t i = (ssize_t)word_len - 1; i >= 0; i--) {
        if (word[i] == '/') {
            /* Check if escaped */
            size_t backslashes = 0;
            for (ssize_t b = i - 1; b >= 0 && word[b] == '\\'; b--) {
                backslashes++;
            }
            if (backslashes % 2 == 0) {
                last_slash = i;
                break;
            }
        }
    }

    if (last_slash >= 0) {
        size_t dlen = (size_t)(last_slash + 1);
        if (out_dir_prefix && dir_prefix_cap > 0) {
            size_t to_copy = dlen < dir_prefix_cap - 1 ? dlen : dir_prefix_cap - 1;
            memcpy(out_dir_prefix, word, to_copy);
            out_dir_prefix[to_copy] = '\0';
        }

        const char *fpart = word + last_slash + 1;
        size_t flen = word_len - (size_t)(last_slash + 1);
        complete_unescape_word(fpart, flen, out_file_prefix, file_prefix_cap);
    } else {
        if (out_dir_prefix && dir_prefix_cap > 0) {
            out_dir_prefix[0] = '\0';
        }
        complete_unescape_word(word, word_len, out_file_prefix, file_prefix_cap);
    }
}

size_t complete_escape_filename(const char *raw, char *out, size_t out_cap) {
    if (!out || out_cap == 0) return 0;
    if (!raw) {
        out[0] = '\0';
        return 0;
    }

    size_t out_len = 0;
    for (size_t i = 0; raw[i] != '\0' && out_len + 2 < out_cap; i++) {
        char c = raw[i];
        if (c == '/') {
            out[out_len++] = '/';
        } else if (is_shell_meta(c)) {
            out[out_len++] = '\\';
            out[out_len++] = c;
        } else {
            out[out_len++] = c;
        }
    }
    out[out_len] = '\0';
    return out_len;
}

size_t complete_compute_lcp(char * const *items, size_t count, char *out_lcp, size_t out_cap) {
    if (!out_lcp || out_cap == 0) return 0;
    out_lcp[0] = '\0';
    if (!items || count == 0) return 0;

    if (count == 1) {
        strncpy(out_lcp, items[0], out_cap - 1);
        out_lcp[out_cap - 1] = '\0';
        return strlen(out_lcp);
    }

    /* Longest Common Prefix between first and last string in sorted array */
    const char *first = items[0];
    const char *last = items[count - 1];
    size_t l = 0;

    while (l + 1 < out_cap && first[l] && first[l] == last[l]) {
        out_lcp[l] = first[l];
        l++;
    }
    out_lcp[l] = '\0';
    return l;
}

static int compare_candidates(const void *a, const void *b) {
    const char *sa = *(const char * const *)a;
    const char *sb = *(const char * const *)b;
    return strcmp(sa, sb);
}

int complete_file_candidates(const char *line, size_t cursor, const char *cwd,
                             CompletionResult *result) {
    if (!result) return -1;
    memset(result, 0, sizeof(*result));

    result->word_start = complete_extract_word(line, cursor, &result->word_len);
    complete_decompose_word(line ? line + result->word_start : NULL,
                            result->word_len,
                            result->dir_prefix, sizeof(result->dir_prefix),
                            result->unescaped_prefix, sizeof(result->unescaped_prefix));

    /* Resolve filesystem lookup directory */
    char dir_to_open[COMPLETE_MAX_PATH];
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }

    if (result->dir_prefix[0] == '\0') {
        strncpy(dir_to_open, (cwd && cwd[0]) ? cwd : ".", sizeof(dir_to_open) - 1);
        dir_to_open[sizeof(dir_to_open) - 1] = '\0';
    } else if (strncmp(result->dir_prefix, "~/", 2) == 0) {
        if (home && home[0]) {
            snprintf(dir_to_open, sizeof(dir_to_open), "%s/%s", home, result->dir_prefix + 2);
        } else {
            snprintf(dir_to_open, sizeof(dir_to_open), "./%s", result->dir_prefix + 2);
        }
    } else if (strcmp(result->dir_prefix, "~") == 0) {
        strncpy(dir_to_open, (home && home[0]) ? home : ".", sizeof(dir_to_open) - 1);
        dir_to_open[sizeof(dir_to_open) - 1] = '\0';
    } else if (result->dir_prefix[0] == '/') {
        strncpy(dir_to_open, result->dir_prefix, sizeof(dir_to_open) - 1);
        dir_to_open[sizeof(dir_to_open) - 1] = '\0';
    } else {
        snprintf(dir_to_open, sizeof(dir_to_open), "%s/%s", (cwd && cwd[0]) ? cwd : ".", result->dir_prefix);
    }

    /* Unescape directory path if needed */
    char unesc_dir[COMPLETE_MAX_PATH];
    complete_unescape_word(dir_to_open, strlen(dir_to_open), unesc_dir, sizeof(unesc_dir));

    DIR *d = opendir(unesc_dir);
    if (!d) {
        /* Silently return 0 candidates on permission denied or missing directory */
        return 0;
    }

    size_t cap = 32;
    result->items = malloc(cap * sizeof(char *));
    if (!result->items) {
        closedir(d);
        return -1;
    }
    result->count = 0;

    size_t pre_len = strlen(result->unescaped_prefix);
    bool want_dotfiles = (result->unescaped_prefix[0] == '.');

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (de->d_name[0] == '.' && !want_dotfiles) {
            continue;
        }
        if (strncmp(de->d_name, result->unescaped_prefix, pre_len) != 0) {
            continue;
        }

        /* Check if candidate is a directory using stat */
        char full_entry[COMPLETE_MAX_PATH];
        size_t ulen = strlen(unesc_dir);
        if (ulen > 0 && unesc_dir[ulen - 1] == '/') {
            snprintf(full_entry, sizeof(full_entry), "%s%s", unesc_dir, de->d_name);
        } else {
            snprintf(full_entry, sizeof(full_entry), "%s/%s", unesc_dir, de->d_name);
        }

        struct stat st;
        bool is_dir = false;
        if (stat(full_entry, &st) == 0 && S_ISDIR(st.st_mode)) {
            is_dir = true;
        }

        size_t name_len = strlen(de->d_name);
        char *cand = malloc(name_len + (is_dir ? 2 : 1));
        if (!cand) continue;
        memcpy(cand, de->d_name, name_len);
        if (is_dir) {
            cand[name_len] = '/';
            cand[name_len + 1] = '\0';
        } else {
            cand[name_len] = '\0';
        }

        if (result->count >= cap) {
            size_t new_cap = cap * 2;
            if (new_cap > COMPLETE_MAX_CANDIDATES) new_cap = COMPLETE_MAX_CANDIDATES;
            if (result->count >= new_cap) {
                free(cand);
                break; /* Cap exceeded to prevent memory blowup */
            }
            char **tmp = realloc(result->items, new_cap * sizeof(char *));
            if (!tmp) {
                free(cand);
                break;
            }
            result->items = tmp;
            cap = new_cap;
        }

        result->items[result->count++] = cand;
        if (result->count >= COMPLETE_MAX_CANDIDATES) {
            break;
        }
    }
    closedir(d);

    if (result->count > 0) {
        qsort(result->items, result->count, sizeof(char *), compare_candidates);
        complete_compute_lcp(result->items, result->count, result->lcp, sizeof(result->lcp));
    }

    return 0;
}

char *complete_format_numbered_list(const CompletionResult *cr, int term_cols) {
    if (!cr || cr->count == 0) return NULL;
    if (term_cols < 20) term_cols = 80;

    size_t est_size = cr->count * 64 + 1024;
    char *buf = malloc(est_size);
    if (!buf) return NULL;
    buf[0] = '\0';
    size_t len = 0;

    int cur_line_len = 0;
    for (size_t i = 0; i < cr->count; i++) {
        char item[512];
        int ilen = snprintf(item, sizeof(item), "%zu. %s", i + 1, cr->items[i]);
        if (ilen <= 0) continue;

        if (cur_line_len == 0) {
            /* First item on line */
            if (len + (size_t)ilen + 4 >= est_size) {
                est_size = est_size * 2 + (size_t)ilen;
                char *new_buf = realloc(buf, est_size);
                if (!new_buf) { free(buf); return NULL; }
                buf = new_buf;
            }
            memcpy(buf + len, item, ilen);
            len += ilen;
            cur_line_len = ilen;
        } else if (cur_line_len + 2 + ilen <= term_cols) {
            /* Fits on current line */
            if (len + (size_t)ilen + 6 >= est_size) {
                est_size = est_size * 2 + (size_t)ilen;
                char *new_buf = realloc(buf, est_size);
                if (!new_buf) { free(buf); return NULL; }
                buf = new_buf;
            }
            buf[len++] = ' ';
            buf[len++] = ' ';
            memcpy(buf + len, item, ilen);
            len += ilen;
            cur_line_len += 2 + ilen;
        } else {
            /* Wrap to next line */
            if (len + (size_t)ilen + 6 >= est_size) {
                est_size = est_size * 2 + (size_t)ilen;
                char *new_buf = realloc(buf, est_size);
                if (!new_buf) { free(buf); return NULL; }
                buf = new_buf;
            }
            buf[len++] = '\n';
            memcpy(buf + len, item, ilen);
            len += ilen;
            cur_line_len = ilen;
        }
    }

    buf[len] = '\0';
    return buf;
}
