/*
 * cmd.c: Shell command, pipeline, and redirection parser.
 *
 * Converts token streams produced by lexer.c into structured Pipeline and Cmd trees:
 * - Groups commands separated by pipes (|) into multi-stage pipelines.
 * - Extracts I/O redirections (<, >, >>, 2>, 2>&1) anywhere in the command line.
 * - Performs word expansion on arguments and redirection targets via expand.c.
 */

#define _GNU_SOURCE
#include "cmd.h"
#include "lexer.h"
#include "expand.h"
#include "shell.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_CMDS 64

static const char *token_to_desc(TokenKind kind) {
    switch (kind) {
        case TOK_PIPE: return "|";
        case TOK_AMP: return "&";
        case TOK_SEMI: return ";";
        case TOK_LESS: return "<";
        case TOK_GREAT: return ">";
        case TOK_DGREAT: return ">>";
        case TOK_NEWLINE: return "newline";
        case TOK_AND: return "&&";
        case TOK_OR: return "||";
        default: return "unknown";
    }
}

static void free_temp_cmds(Cmd *cmds, int ncmds, char **cur_argv, int cur_argc, Redir *cur_redirs, int cur_nredirs) {
    if (cur_argv) {
        for (int i = 0; i < cur_argc; i++) free(cur_argv[i]);
        free(cur_argv);
    }
    if (cur_redirs) {
        for (int i = 0; i < cur_nredirs; i++) free(cur_redirs[i].filename);
        free(cur_redirs);
    }
    for (int i = 0; i < ncmds; i++) {
        if (cmds[i].argv) {
            for (int j = 0; j < cmds[i].argc; j++) free(cmds[i].argv[j]);
            free(cmds[i].argv);
        }
        if (cmds[i].redirs) {
            for (int j = 0; j < cmds[i].nredirs; j++) free(cmds[i].redirs[j].filename);
            free(cmds[i].redirs);
        }
    }
}

int parse_line(Shell *sh, const char *line, Pipeline *out_pl) {
    if (!out_pl) return -1;
    memset(out_pl, 0, sizeof(Pipeline));

    if (!line) return 0;
    out_pl->text = strdup(line);

    Token *tokens = NULL;
    LexStatus st = lex_tokenize(line, &tokens);
    if (st != LEX_COMPLETE) {
        token_free_list(tokens);
        return (int)st;
    }

    if (!tokens) {
        return 0;
    }

    /* Check leading pipe syntax error */
    if (tokens->kind == TOK_PIPE) {
        if (sh) {
            snprintf(sh->last_error, sizeof(sh->last_error),
                     "terminator: syntax error near unexpected token '|'\n");
            sh->last_status = 2;
            vars_set_status(&sh->vars, 2);
        }
        token_free_list(tokens);
        return 2;
    }

    int cmd_capacity = 16;
    Cmd *temp_cmds = calloc(cmd_capacity, sizeof(Cmd));
    if (!temp_cmds) {
        token_free_list(tokens);
        return -1;
    }
    int ncmds = 1;
    Cmd *cur_cmd = &temp_cmds[0];

    char **cmd_argv = NULL;
    int cmd_argc = 0;
    int cmd_cap = 0;

    Redir *cmd_redirs = NULL;
    int cmd_nredirs = 0;
    int redir_cap = 0;

    Token *tok = tokens;
    while (tok) {
        if (tok->kind == TOK_NEWLINE) {
            tok = tok->next;
            continue;
        }

        if (tok->kind == TOK_PIPE) {
            /* Check if previous command had nothing */
            if (cmd_argc == 0 && cmd_nredirs == 0) {
                if (sh) {
                    snprintf(sh->last_error, sizeof(sh->last_error),
                             "terminator: syntax error near unexpected token '|'\n");
                    sh->last_status = 2;
                    vars_set_status(&sh->vars, 2);
                }
                free_temp_cmds(temp_cmds, ncmds, cmd_argv, cmd_argc, cmd_redirs, cmd_nredirs);
                free(temp_cmds);
                token_free_list(tokens);
                return 2;
            }

            /* Finalize current command */
            cur_cmd->argv = cmd_argv;
            cur_cmd->argc = cmd_argc;
            cur_cmd->redirs = cmd_redirs;
            cur_cmd->nredirs = cmd_nredirs;

            cmd_argv = NULL;
            cmd_argc = 0;
            cmd_cap = 0;
            cmd_redirs = NULL;
            cmd_nredirs = 0;
            redir_cap = 0;

            if (tok->next == NULL) {
                if (sh) {
                    snprintf(sh->last_error, sizeof(sh->last_error),
                             "terminator: syntax error near unexpected token '|'\n");
                    sh->last_status = 2;
                    vars_set_status(&sh->vars, 2);
                }
                free_temp_cmds(temp_cmds, ncmds, cmd_argv, cmd_argc, cmd_redirs, cmd_nredirs);
                free(temp_cmds);
                token_free_list(tokens);
                return 2;
            }

            if (ncmds >= cmd_capacity) {
                int new_cap = cmd_capacity * 2;
                Cmd *new_cmds = realloc(temp_cmds, new_cap * sizeof(Cmd));
                if (!new_cmds) {
                    free_temp_cmds(temp_cmds, ncmds, cmd_argv, cmd_argc, cmd_redirs, cmd_nredirs);
                    free(temp_cmds);
                    token_free_list(tokens);
                    return -1;
                }
                temp_cmds = new_cmds;
                cmd_capacity = new_cap;
            }
            cur_cmd = &temp_cmds[ncmds++];
            memset(cur_cmd, 0, sizeof(Cmd));
            tok = tok->next;
            continue;
        }

        if (tok->kind == TOK_AMP) {
            if (tok->next == NULL || (tok->next->kind == TOK_NEWLINE && tok->next->next == NULL)) {
                out_pl->background = true;
                tok = tok->next;
                continue;
            } else {
                if (sh) {
                    snprintf(sh->last_error, sizeof(sh->last_error),
                             "terminator: syntax error near unexpected token '&'\n");
                    sh->last_status = 2;
                    vars_set_status(&sh->vars, 2);
                }
                free_temp_cmds(temp_cmds, ncmds, cmd_argv, cmd_argc, cmd_redirs, cmd_nredirs);
                free(temp_cmds);
                token_free_list(tokens);
                return 2;
            }
        }

        if (tok->kind == TOK_LESS || tok->kind == TOK_GREAT || tok->kind == TOK_DGREAT) {
            /* Case A: Duplicate redirection (e.g. 2>&1) */
            if (tok->dup_target >= 0) {
                if (cmd_nredirs >= redir_cap) {
                    redir_cap = redir_cap == 0 ? 4 : redir_cap * 2;
                    cmd_redirs = realloc(cmd_redirs, redir_cap * sizeof(Redir));
                }
                cmd_redirs[cmd_nredirs].type = REDIR_DUP;
                cmd_redirs[cmd_nredirs].filename = NULL;
                cmd_redirs[cmd_nredirs].fd = (tok->redir_fd >= 0) ? tok->redir_fd : 1;
                cmd_redirs[cmd_nredirs].dup_target = tok->dup_target;
                cmd_nredirs++;
                tok = tok->next;
                continue;
            }

            /* Case B: File redirection (<, >, >>) */
            Token *next = tok->next;
            if (!next || next->kind == TOK_NEWLINE) {
                if (sh) {
                    snprintf(sh->last_error, sizeof(sh->last_error),
                             "terminator: syntax error near unexpected token 'newline'\n");
                    sh->last_status = 2;
                    vars_set_status(&sh->vars, 2);
                }
                free_temp_cmds(temp_cmds, ncmds, cmd_argv, cmd_argc, cmd_redirs, cmd_nredirs);
                free(temp_cmds);
                token_free_list(tokens);
                return 2;
            }

            if (next->kind != TOK_WORD) {
                if (sh) {
                    snprintf(sh->last_error, sizeof(sh->last_error),
                             "terminator: syntax error near unexpected token '%s'\n",
                             token_to_desc(next->kind));
                    sh->last_status = 2;
                    vars_set_status(&sh->vars, 2);
                }
                free_temp_cmds(temp_cmds, ncmds, cmd_argv, cmd_argc, cmd_redirs, cmd_nredirs);
                free(temp_cmds);
                token_free_list(tokens);
                return 2;
            }

            /* Expand target word */
            char **r_argv = NULL;
            int r_argc = 0;
            expand_word_token(sh, next, &r_argv, &r_argc);

            if (r_argc != 1 || !r_argv || !r_argv[0]) {
                if (r_argv) {
                    for (int k = 0; k < r_argc; k++) free(r_argv[k]);
                    free(r_argv);
                }
                if (sh) {
                    snprintf(sh->last_error, sizeof(sh->last_error),
                             "terminator: ambiguous redirect\n");
                    sh->last_status = 1;
                    vars_set_status(&sh->vars, 1);
                }
                free_temp_cmds(temp_cmds, ncmds, cmd_argv, cmd_argc, cmd_redirs, cmd_nredirs);
                free(temp_cmds);
                token_free_list(tokens);
                return 1;
            }

            RedirType rtype = (tok->kind == TOK_LESS) ? REDIR_IN :
                              ((tok->kind == TOK_DGREAT) ? REDIR_APPEND : REDIR_OUT);
            int target_fd = (tok->redir_fd >= 0) ? tok->redir_fd : ((rtype == REDIR_IN) ? 0 : 1);

            if (cmd_nredirs >= redir_cap) {
                redir_cap = redir_cap == 0 ? 4 : redir_cap * 2;
                cmd_redirs = realloc(cmd_redirs, redir_cap * sizeof(Redir));
            }
            cmd_redirs[cmd_nredirs].type = rtype;
            cmd_redirs[cmd_nredirs].filename = strdup(r_argv[0]);
            cmd_redirs[cmd_nredirs].fd = target_fd;
            cmd_redirs[cmd_nredirs].dup_target = -1;
            cmd_nredirs++;

            free(r_argv[0]);
            free(r_argv);

            tok = next->next;
            continue;
        }

        if (tok->kind == TOK_WORD) {
            char **words = NULL;
            int nwords = 0;
            expand_word_token(sh, tok, &words, &nwords);

            if (nwords > 0 && words) {
                while (cmd_argc + nwords + 1 >= cmd_cap) {
                    cmd_cap = cmd_cap == 0 ? 8 : cmd_cap * 2;
                    cmd_argv = realloc(cmd_argv, cmd_cap * sizeof(char *));
                }
                for (int w = 0; w < nwords; w++) {
                    cmd_argv[cmd_argc++] = words[w];
                }
                cmd_argv[cmd_argc] = NULL;
                free(words);
            }
            tok = tok->next;
            continue;
        }

        /* Any other unexpected token (e.g. unexpected ';') */
        if (sh) {
            snprintf(sh->last_error, sizeof(sh->last_error),
                     "terminator: syntax error near unexpected token '%s'\n",
                     token_to_desc(tok->kind));
            sh->last_status = 2;
            vars_set_status(&sh->vars, 2);
        }
        free_temp_cmds(temp_cmds, ncmds, cmd_argv, cmd_argc, cmd_redirs, cmd_nredirs);
        free(temp_cmds);
        token_free_list(tokens);
        return 2;
    }

    /* Finalize last command */
    cur_cmd->argv = cmd_argv;
    cur_cmd->argc = cmd_argc;
    cur_cmd->redirs = cmd_redirs;
    cur_cmd->nredirs = cmd_nredirs;

    /* Filter out any empty commands if no argv and no redirs */
    int valid_cmds = 0;
    for (int i = 0; i < ncmds; i++) {
        if (temp_cmds[i].argc > 0 || temp_cmds[i].nredirs > 0) {
            valid_cmds++;
        }
    }

    if (valid_cmds > 0) {
        out_pl->cmds = malloc(valid_cmds * sizeof(Cmd));
        int dst = 0;
        for (int i = 0; i < ncmds; i++) {
            if (temp_cmds[i].argc > 0 || temp_cmds[i].nredirs > 0) {
                out_pl->cmds[dst++] = temp_cmds[i];
            }
        }
        out_pl->n = valid_cmds;
    }

    free(temp_cmds);
    token_free_list(tokens);
    return 0;
}

void pipeline_free(Pipeline *pl) {
    if (!pl) return;

    if (pl->cmds) {
        for (int i = 0; i < pl->n; i++) {
            Cmd *c = &pl->cmds[i];
            if (c->argv) {
                for (int j = 0; j < c->argc; j++) {
                    free(c->argv[j]);
                }
                free(c->argv);
            }
            if (c->redirs) {
                for (int j = 0; j < c->nredirs; j++) {
                    free(c->redirs[j].filename);
                }
                free(c->redirs);
            }
        }
        free(pl->cmds);
    }

    free(pl->text);
    memset(pl, 0, sizeof(Pipeline));
}
