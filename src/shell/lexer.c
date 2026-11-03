/*
 * lexer.c: Hand-written POSIX shell lexer and completeness checker.
 *
 * Implements a single-pass state machine converting input command strings into tokens:
 * - TOK_WORD: Composed of WordSegments tagged with quote kinds (NONE, SINGLE, DOUBLE).
 * - Control operators: |, &, ;, <, >, >>, \n.
 * - Completeness detection: Evaluates unclosed quotes, trailing backslashes, open
 *   command substitutions, and trailing pipes to trigger the continuation prompt (> ).
 */

#define _GNU_SOURCE
#include "lexer.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

WordSegment *word_segment_create(const char *text, size_t len, QuoteKind quote) {
    WordSegment *seg = malloc(sizeof(WordSegment));
    if (!seg) return NULL;
    seg->text = malloc(len + 1);
    if (!seg->text) {
        free(seg);
        return NULL;
    }
    if (len > 0 && text) {
        memcpy(seg->text, text, len);
    }
    seg->text[len] = '\0';
    seg->len = len;
    seg->quote = quote;
    seg->next = NULL;
    return seg;
}

void word_segments_free(WordSegment *seg) {
    while (seg) {
        WordSegment *next = seg->next;
        free(seg->text);
        free(seg);
        seg = next;
    }
}

void word_segment_append(WordSegment **head, WordSegment *seg) {
    if (!head || !seg) return;
    if (!*head) {
        *head = seg;
        return;
    }
    WordSegment *cur = *head;
    while (cur->next) {
        cur = cur->next;
    }
    cur->next = seg;
}

static Token *token_create(TokenKind kind) {
    Token *tok = calloc(1, sizeof(Token));
    if (!tok) return NULL;
    tok->kind = kind;
    tok->redir_fd = -1;
    tok->dup_target = -1;
    tok->segments = NULL;
    tok->next = NULL;
    return tok;
}

void token_free_list(Token *tok) {
    while (tok) {
        Token *next = tok->next;
        if (tok->segments) {
            word_segments_free(tok->segments);
        }
        free(tok);
        tok = next;
    }
}

static void token_append(Token **head, Token *tok) {
    if (!head || !tok) return;
    if (!*head) {
        *head = tok;
        return;
    }
    Token *cur = *head;
    while (cur->next) {
        cur = cur->next;
    }
    cur->next = tok;
}

/* Recursive scanner for matching $( ... ) respecting quotes and nested $( ) */
static LexStatus scan_subshell(const char *str, size_t *consumed_out) {
    if (!str || str[0] != '$' || str[1] != '(') {
        return LEX_ERROR;
    }

    const char *p = str + 2;
    int depth = 1;

    while (*p && depth > 0) {
        if (*p == '\\') {
            p++;
            if (*p) p++;
            continue;
        }

        if (*p == '\'') {
            p++;
            while (*p && *p != '\'') {
                p++;
            }
            if (!*p) return LEX_INCOMPLETE_QUOTE_SINGLE;
            p++;
            continue;
        }

        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\') {
                    p++;
                    if (*p) p++;
                    continue;
                }
                if (*p == '$' && *(p + 1) == '(') {
                    size_t nested_len = 0;
                    LexStatus st = scan_subshell(p, &nested_len);
                    if (st != LEX_COMPLETE) return st;
                    p += nested_len;
                    continue;
                }
                if (*p == '`') {
                    p++;
                    while (*p && *p != '`') {
                        if (*p == '\\') {
                            p++;
                            if (*p) p++;
                            continue;
                        }
                        p++;
                    }
                    if (!*p) return LEX_INCOMPLETE_BACKTICK;
                    p++;
                    continue;
                }
                p++;
            }
            if (!*p) return LEX_INCOMPLETE_QUOTE_DOUBLE;
            p++;
            continue;
        }

        if (*p == '`') {
            p++;
            while (*p && *p != '`') {
                if (*p == '\\') {
                    p++;
                    if (*p) p++;
                    continue;
                }
                p++;
            }
            if (!*p) return LEX_INCOMPLETE_BACKTICK;
            p++;
            continue;
        }

        if (*p == '$' && *(p + 1) == '(') {
            depth++;
            p += 2;
            continue;
        }

        if (*p == ')') {
            depth--;
            p++;
            if (depth == 0) {
                if (consumed_out) *consumed_out = (size_t)(p - str);
                return LEX_COMPLETE;
            }
            continue;
        }

        p++;
    }

    if (depth > 0) {
        return LEX_INCOMPLETE_SUBSHELL;
    }

    if (consumed_out) *consumed_out = (size_t)(p - str);
    return LEX_COMPLETE;
}

/* Scanner for matching `...` respecting backslash escaping */
static LexStatus scan_backtick(const char *str, size_t *consumed_out) {
    if (!str || str[0] != '`') {
        return LEX_ERROR;
    }

    const char *p = str + 1;
    while (*p && *p != '`') {
        if (*p == '\\') {
            p++;
            if (*p) p++;
            continue;
        }
        p++;
    }

    if (!*p) {
        return LEX_INCOMPLETE_BACKTICK;
    }

    p++; /* Skip closing ` */
    if (consumed_out) *consumed_out = (size_t)(p - str);
    return LEX_COMPLETE;
}

LexStatus lex_check_completeness(const char *input) {
    if (!input) return LEX_COMPLETE;

    const char *p = input;
    TokenKind last_op = TOK_EOF;
    bool saw_word = false;
    bool expect_redir_word = false;

    while (*p) {
        /* Skip leading whitespace (except newline) */
        while (*p && *p != '\n' && isspace((unsigned char)*p)) {
            p++;
        }
        if (!*p) break;

        /* Backslash outside quotes */
        if (*p == '\\') {
            p++;
            if (!*p) {
                return LEX_INCOMPLETE_BACKSLASH;
            }
            if (*p == '\n') {
                p++;
                continue;
            }
            saw_word = true;
            expect_redir_word = false;
            last_op = TOK_WORD;
            p++;
            continue;
        }

        /* Operators */
        if (*p == '|') {
            if (*(p + 1) == '|') {
                last_op = (saw_word && !expect_redir_word) ? TOK_OR : TOK_EOF;
                p += 2;
            } else {
                last_op = (saw_word && !expect_redir_word) ? TOK_PIPE : TOK_EOF;
                p++;
            }
            saw_word = false;
            expect_redir_word = false;
            continue;
        }

        if (*p == '&') {
            if (*(p + 1) == '&') {
                last_op = (saw_word && !expect_redir_word) ? TOK_AND : TOK_EOF;
                p += 2;
                saw_word = false;
                expect_redir_word = false;
            } else {
                last_op = TOK_AMP;
                p++;
            }
            continue;
        }

        if (*p == ';') {
            last_op = TOK_SEMI;
            expect_redir_word = false;
            p++;
            continue;
        }

        if (*p == '\n') {
            last_op = TOK_NEWLINE;
            expect_redir_word = false;
            p++;
            continue;
        }

        if ((*p == '1' || *p == '2') && *(p + 1) == '>') {
            if (*(p + 2) == '>') {
                expect_redir_word = true;
                p += 3;
            } else if (*(p + 2) == '&' && isdigit((unsigned char)*(p + 3))) {
                expect_redir_word = false;
                p += 4;
            } else {
                expect_redir_word = true;
                p += 2;
            }
            continue;
        }

        if (*p == '>' && *(p + 1) == '&' && isdigit((unsigned char)*(p + 2))) {
            expect_redir_word = false;
            p += 3;
            continue;
        }

        if (*p == '<' || *p == '>') {
            expect_redir_word = true;
            if (*p == '>' && *(p + 1) == '>') {
                p += 2;
            } else {
                p++;
            }
            continue;
        }

        /* Word parsing */
        saw_word = true;
        expect_redir_word = false;
        last_op = TOK_WORD;
        const char *word_start = p;

        while (*p && !isspace((unsigned char)*p) &&
               *p != '|' && *p != '&' && *p != ';' && *p != '<' && *p != '>') {

            if (*p == '\\') {
                p++;
                if (!*p) {
                    return LEX_INCOMPLETE_BACKSLASH;
                }
                if (*p == '\n') {
                    p++;
                    continue;
                }
                p++;
                continue;
            }

            if (*p == '\'') {
                p++;
                while (*p && *p != '\'') {
                    p++;
                }
                if (!*p) {
                    return LEX_INCOMPLETE_QUOTE_SINGLE;
                }
                p++;
                continue;
            }

            if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\') {
                        p++;
                        if (!*p) {
                            return LEX_INCOMPLETE_QUOTE_DOUBLE;
                        }
                        if (*p == '\n') {
                            p++;
                            continue;
                        }
                        p++;
                        continue;
                    }
                    if (*p == '$' && *(p + 1) == '(') {
                        size_t consumed = 0;
                        LexStatus st = scan_subshell(p, &consumed);
                        if (st != LEX_COMPLETE) {
                            return st;
                        }
                        p += consumed;
                        continue;
                    }
                    if (*p == '`') {
                        size_t consumed = 0;
                        LexStatus st = scan_backtick(p, &consumed);
                        if (st != LEX_COMPLETE) {
                            return st;
                        }
                        p += consumed;
                        continue;
                    }
                    p++;
                }
                if (!*p) {
                    return LEX_INCOMPLETE_QUOTE_DOUBLE;
                }
                p++;
                continue;
            }

            if (*p == '$' && *(p + 1) == '(') {
                size_t consumed = 0;
                LexStatus st = scan_subshell(p, &consumed);
                if (st != LEX_COMPLETE) {
                    return st;
                }
                p += consumed;
                continue;
            }

            if (*p == '`') {
                size_t consumed = 0;
                LexStatus st = scan_backtick(p, &consumed);
                if (st != LEX_COMPLETE) {
                    return st;
                }
                p += consumed;
                continue;
            }

            p++;
        }

        if (p == word_start && *p) {
            p++;
        }
    }

    if (last_op == TOK_PIPE) {
        return LEX_INCOMPLETE_PIPE;
    }
    if (last_op == TOK_AND) {
        return LEX_INCOMPLETE_AND;
    }
    if (last_op == TOK_OR) {
        return LEX_INCOMPLETE_OR;
    }

    (void)saw_word;
    return LEX_COMPLETE;
}

LexStatus lex_tokenize(const char *input, Token **out_tokens) {
    if (!out_tokens) return LEX_ERROR;
    *out_tokens = NULL;
    if (!input) return LEX_COMPLETE;

    LexStatus completeness = lex_check_completeness(input);
    if (completeness != LEX_COMPLETE) {
        return completeness;
    }

    const char *p = input;
    Token *head = NULL;

    while (*p) {
        /* Skip horizontal whitespace (except newline) */
        while (*p && *p != '\n' && isspace((unsigned char)*p)) {
            p++;
        }
        if (!*p) break;

        /* Check for operators */
        if (*p == '\n') {
            Token *tok = token_create(TOK_NEWLINE);
            token_append(&head, tok);
            p++;
            continue;
        }

        if (*p == '|' && *(p + 1) == '|') {
            Token *tok = token_create(TOK_OR);
            token_append(&head, tok);
            p += 2;
            continue;
        }

        if (*p == '|') {
            Token *tok = token_create(TOK_PIPE);
            token_append(&head, tok);
            p++;
            continue;
        }

        if (*p == '&' && *(p + 1) == '&') {
            Token *tok = token_create(TOK_AND);
            token_append(&head, tok);
            p += 2;
            continue;
        }

        if (*p == '&') {
            Token *tok = token_create(TOK_AMP);
            token_append(&head, tok);
            p++;
            continue;
        }

        if (*p == ';') {
            Token *tok = token_create(TOK_SEMI);
            token_append(&head, tok);
            p++;
            continue;
        }

        if ((*p == '1' || *p == '2') && *(p + 1) == '>') {
            int rfd = *p - '0';
            if (*(p + 2) == '>') {
                Token *tok = token_create(TOK_DGREAT);
                tok->redir_fd = rfd;
                token_append(&head, tok);
                p += 3;
                continue;
            } else if (*(p + 2) == '&' && isdigit((unsigned char)*(p + 3))) {
                Token *tok = token_create(TOK_GREAT);
                tok->redir_fd = rfd;
                tok->dup_target = *(p + 3) - '0';
                token_append(&head, tok);
                p += 4;
                continue;
            } else {
                Token *tok = token_create(TOK_GREAT);
                tok->redir_fd = rfd;
                token_append(&head, tok);
                p += 2;
                continue;
            }
        }

        if (*p == '>' && *(p + 1) == '&' && isdigit((unsigned char)*(p + 2))) {
            Token *tok = token_create(TOK_GREAT);
            tok->redir_fd = 1;
            tok->dup_target = *(p + 2) - '0';
            token_append(&head, tok);
            p += 3;
            continue;
        }

        if (*p == '<') {
            Token *tok = token_create(TOK_LESS);
            tok->redir_fd = 0;
            token_append(&head, tok);
            p++;
            continue;
        }

        if (*p == '>' && *(p + 1) == '>') {
            Token *tok = token_create(TOK_DGREAT);
            tok->redir_fd = 1;
            token_append(&head, tok);
            p += 2;
            continue;
        }

        if (*p == '>') {
            Token *tok = token_create(TOK_GREAT);
            tok->redir_fd = 1;
            token_append(&head, tok);
            p++;
            continue;
        }

        /* Check for backslash-newline line continuation outside quotes */
        if (*p == '\\' && *(p + 1) == '\n') {
            p += 2;
            continue;
        }

        /* Must be a WORD token composed of one or more WordSegments */
        Token *word_tok = token_create(TOK_WORD);

        char seg_buf[8192];
        size_t seg_len = 0;
        QuoteKind cur_quote = QUOTE_NONE;
        const char *word_start = p;

        while (*p) {
            /* Check word delimiter outside quotes */
            if (cur_quote == QUOTE_NONE) {
                if (isspace((unsigned char)*p) ||
                    *p == '|' || *p == '&' || *p == ';' || *p == '<' || *p == '>') {
                    break;
                }

                if (*p == '\\') {
                    if (*(p + 1) == '\n') {
                        p += 2;
                        continue;
                    }
                    p++;
                    if (*p) {
                        if (seg_len < sizeof(seg_buf) - 1) {
                            seg_buf[seg_len++] = *p;
                        }
                        p++;
                    }
                    continue;
                }

                if (*p == '\'') {
                    if (seg_len > 0) {
                        WordSegment *seg = word_segment_create(seg_buf, seg_len, QUOTE_NONE);
                        word_segment_append(&word_tok->segments, seg);
                        seg_len = 0;
                    }
                    cur_quote = QUOTE_SINGLE;
                    p++;
                    continue;
                }

                if (*p == '"') {
                    if (seg_len > 0) {
                        WordSegment *seg = word_segment_create(seg_buf, seg_len, QUOTE_NONE);
                        word_segment_append(&word_tok->segments, seg);
                        seg_len = 0;
                    }
                    cur_quote = QUOTE_DOUBLE;
                    p++;
                    continue;
                }

                /* Command substitution $(...) in unquoted text */
                if (*p == '$' && *(p + 1) == '(') {
                    size_t consumed = 0;
                    scan_subshell(p, &consumed);
                    for (size_t k = 0; k < consumed && seg_len < sizeof(seg_buf) - 1; k++) {
                        seg_buf[seg_len++] = p[k];
                    }
                    p += consumed;
                    continue;
                }

                /* Backtick substitution in unquoted text */
                if (*p == '`') {
                    size_t consumed = 0;
                    scan_backtick(p, &consumed);
                    for (size_t k = 0; k < consumed && seg_len < sizeof(seg_buf) - 1; k++) {
                        seg_buf[seg_len++] = p[k];
                    }
                    p += consumed;
                    continue;
                }

                /* Regular unquoted character */
                if (seg_len < sizeof(seg_buf) - 1) {
                    seg_buf[seg_len++] = *p;
                }
                p++;
                continue;
            }

            if (cur_quote == QUOTE_SINGLE) {
                if (*p == '\'') {
                    /* Single quote closed; always flush segment (even if empty, preserving empty string) */
                    WordSegment *seg = word_segment_create(seg_buf, seg_len, QUOTE_SINGLE);
                    word_segment_append(&word_tok->segments, seg);
                    seg_len = 0;
                    cur_quote = QUOTE_NONE;
                    p++;
                    continue;
                }
                /* Single quotes are completely literal */
                if (seg_len < sizeof(seg_buf) - 1) {
                    seg_buf[seg_len++] = *p;
                }
                p++;
                continue;
            }

            if (cur_quote == QUOTE_DOUBLE) {
                if (*p == '"') {
                    /* Double quote closed; always flush segment */
                    WordSegment *seg = word_segment_create(seg_buf, seg_len, QUOTE_DOUBLE);
                    word_segment_append(&word_tok->segments, seg);
                    seg_len = 0;
                    cur_quote = QUOTE_NONE;
                    p++;
                    continue;
                }

                if (*p == '\\') {
                    if (*(p + 1) == '\n') {
#if DQUOTE_BACKSLASH_NEWLINE_KEEPS_NEWLINE
                        /* Spec rule: keep newline, drop backslash */
                        if (seg_len < sizeof(seg_buf) - 1) {
                            seg_buf[seg_len++] = '\n';
                        }
#endif
                        p += 2;
                        continue;
                    }

                    /* In double quotes, backslash only escapes $, `, ", \, and \n */
                    char next_c = *(p + 1);
                    if (next_c == '$' || next_c == '`' || next_c == '"' || next_c == '\\') {
                        p++;
                        if (seg_len < sizeof(seg_buf) - 1) {
                            seg_buf[seg_len++] = *p;
                        }
                        p++;
                        continue;
                    } else {
                        /* Otherwise keep literal backslash */
                        if (seg_len < sizeof(seg_buf) - 1) {
                            seg_buf[seg_len++] = '\\';
                        }
                        p++;
                        continue;
                    }
                }

                /* Command substitution $(...) inside double quotes */
                if (*p == '$' && *(p + 1) == '(') {
                    size_t consumed = 0;
                    scan_subshell(p, &consumed);
                    for (size_t k = 0; k < consumed && seg_len < sizeof(seg_buf) - 1; k++) {
                        seg_buf[seg_len++] = p[k];
                    }
                    p += consumed;
                    continue;
                }

                /* Backtick substitution inside double quotes */
                if (*p == '`') {
                    size_t consumed = 0;
                    scan_backtick(p, &consumed);
                    for (size_t k = 0; k < consumed && seg_len < sizeof(seg_buf) - 1; k++) {
                        seg_buf[seg_len++] = p[k];
                    }
                    p += consumed;
                    continue;
                }

                if (seg_len < sizeof(seg_buf) - 1) {
                    seg_buf[seg_len++] = *p;
                }
                p++;
                continue;
            }
        }

        if (p == word_start && *p) {
            p++;
        }

        /* Flush any remaining segment */
        if (seg_len > 0 || (cur_quote == QUOTE_NONE && word_tok->segments == NULL)) {
            WordSegment *seg = word_segment_create(seg_buf, seg_len, cur_quote);
            word_segment_append(&word_tok->segments, seg);
        }

        token_append(&head, word_tok);
    }

    *out_tokens = head;
    return LEX_COMPLETE;
}
