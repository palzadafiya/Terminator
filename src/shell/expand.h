#ifndef SHELL_EXPAND_H
#define SHELL_EXPAND_H

#include "lexer.h"
#include "shell.h"

#define MAX_SUBST_DEPTH 16

/* Expand a single Word token into zero or more argv strings through the 6-stage pipeline */
int expand_word_token(Shell *sh, const Token *tok, char ***out_argv, int *out_argc);

/* Command substitution runner: evaluates inner_cmd in a forked subshell,
 * captures stdout, waitpids, and strips trailing newlines */
char *expand_command_subst(Shell *sh, const char *inner_cmd, int depth);

/* Expand a string directly (used for command substitution / variables) */
char *expand_string(Shell *sh, const char *input, int depth);

#endif /* SHELL_EXPAND_H */
