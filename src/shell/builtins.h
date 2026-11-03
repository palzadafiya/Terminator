#ifndef SHELL_BUILTINS_H
#define SHELL_BUILTINS_H

#include <stdbool.h>
#include "sink.h"

typedef struct Shell Shell;

typedef int (*BuiltinFn)(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err);

typedef struct {
    const char *name;
    BuiltinFn fn;
    bool runs_in_parent;
} BuiltinDef;

const BuiltinDef *builtin_find(const char *name);

/* Individual built-in functions */
int builtin_cd(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err);
int builtin_exit(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err);
int builtin_pwd(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err);
int builtin_echo(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err);
int builtin_history(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err);
int builtin_export(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err);
int builtin_unset(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err);
int builtin_bind(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err);
int builtin_jobs(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err);
int builtin_fg(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err);
int builtin_bg(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err);
int builtin_clear(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err);

/* Stubs for later tasks */
int builtin_stub(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err);

#endif /* SHELL_BUILTINS_H */
