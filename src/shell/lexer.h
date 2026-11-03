#ifndef SHELL_LEXER_H
#define SHELL_LEXER_H

#include <stddef.h>
#include <stdbool.h>

#ifndef DQUOTE_BACKSLASH_NEWLINE_KEEPS_NEWLINE
#define DQUOTE_BACKSLASH_NEWLINE_KEEPS_NEWLINE 1
#endif

typedef enum {
    QUOTE_NONE = 0,
    QUOTE_SINGLE,
    QUOTE_DOUBLE
} QuoteKind;

typedef struct WordSegment {
    char *text;
    size_t len;
    QuoteKind quote;
    struct WordSegment *next;
} WordSegment;

typedef enum {
    TOK_EOF = 0,
    TOK_WORD,
    TOK_PIPE,     /* | */
    TOK_AMP,      /* & */
    TOK_SEMI,     /* ; */
    TOK_LESS,     /* < */
    TOK_GREAT,    /* > */
    TOK_DGREAT,   /* >> */
    TOK_NEWLINE,  /* \n */
    TOK_AND,      /* && */
    TOK_OR        /* || */
} TokenKind;

typedef struct Token {
    TokenKind kind;
    int redir_fd;          /* 0 for <, 1 for >, 2 for 2>, or -1 */
    int dup_target;        /* target fd for >&N (e.g. 1 for 2>&1), or -1 */
    WordSegment *segments; /* Linked list of segments for TOK_WORD */
    struct Token *next;
} Token;

typedef enum {
    LEX_COMPLETE = 0,
    LEX_INCOMPLETE_QUOTE_SINGLE,
    LEX_INCOMPLETE_QUOTE_DOUBLE,
    LEX_INCOMPLETE_BACKSLASH,
    LEX_INCOMPLETE_PIPE,
    LEX_INCOMPLETE_AND,
    LEX_INCOMPLETE_OR,
    LEX_INCOMPLETE_SUBSHELL,
    LEX_INCOMPLETE_BACKTICK,
    LEX_ERROR
} LexStatus;

/* Check completeness of input string */
LexStatus lex_check_completeness(const char *input);

/* Tokenize input string into linked list of Tokens */
LexStatus lex_tokenize(const char *input, Token **out_tokens);

/* Memory management */
void token_free_list(Token *tok);
void word_segments_free(WordSegment *seg);
WordSegment *word_segment_create(const char *text, size_t len, QuoteKind quote);
void word_segment_append(WordSegment **head, WordSegment *seg);

#endif /* SHELL_LEXER_H */
