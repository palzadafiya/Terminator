#ifndef SHELL_VARS_H
#define SHELL_VARS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct VarEntry {
    char *name;
    char *value;
    bool exported;
    struct VarEntry *next;
} VarEntry;

typedef struct {
    VarEntry *head;
    int last_status;
    char status_str[16];
} VarTable;

void vars_init(VarTable *vt, char **initial_envp);
void vars_free(VarTable *vt);

const char *vars_get(VarTable *vt, const char *name);
int vars_set(VarTable *vt, const char *name, const char *value, bool exported);
int vars_unset(VarTable *vt, const char *name);
int vars_export(VarTable *vt, const char *name);

void vars_set_status(VarTable *vt, int status);
int vars_get_status(const VarTable *vt);

/* Allocate a new NULL-terminated envp array from exported variables */
char **vars_to_envp(const VarTable *vt);
void vars_free_envp(char **envp);

#endif /* SHELL_VARS_H */
