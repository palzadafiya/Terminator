#ifndef SHELL_CMD_H
#define SHELL_CMD_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    REDIR_NONE = 0,
    REDIR_IN,      /* < */
    REDIR_OUT,     /* > */
    REDIR_APPEND,  /* >> */
    REDIR_HEREDOC, /* << */
    REDIR_DUP      /* >&N or 2>&1 */
} RedirType;

typedef struct {
    RedirType type;
    char *filename;
    int fd;
    int dup_target;
} Redir;

typedef struct {
    char **argv;
    int argc;
    Redir *redirs;
    int nredirs;
} Cmd;

typedef struct {
    Cmd *cmds;
    int n;
    bool background;
    char *text;
} Pipeline;

typedef struct Shell Shell;

int parse_line(Shell *sh, const char *line, Pipeline *out_pl);
void pipeline_free(Pipeline *pl);

#endif /* SHELL_CMD_H */
