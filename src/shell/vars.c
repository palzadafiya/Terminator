/*
 * vars.c: Shell and environment variable management.
 *
 * Implements per-tab variable tables supporting exported and local variables:
 * - Dynamic variable lookup and setting ($VAR, ${VAR}).
 * - Special parameters: $?, $$, $#, $0.
 * - Synchronization to environ arrays passed to execvp.
 */

#define _GNU_SOURCE
#include "vars.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void vars_init(VarTable *vt, char **initial_envp) {
    if (!vt) return;
    vt->head = NULL;
    vt->last_status = 0;
    snprintf(vt->status_str, sizeof(vt->status_str), "0");

    if (initial_envp) {
        for (char **p = initial_envp; *p; p++) {
            char *eq = strchr(*p, '=');
            if (eq) {
                size_t name_len = (size_t)(eq - *p);
                char *name = malloc(name_len + 1);
                if (name) {
                    memcpy(name, *p, name_len);
                    name[name_len] = '\0';
                    vars_set(vt, name, eq + 1, true);
                    free(name);
                }
            }
        }
    }
}

void vars_free(VarTable *vt) {
    if (!vt) return;
    VarEntry *cur = vt->head;
    while (cur) {
        VarEntry *next = cur->next;
        free(cur->name);
        free(cur->value);
        free(cur);
        cur = next;
    }
    vt->head = NULL;
}

const char *vars_get(VarTable *vt, const char *name) {
    if (!vt || !name) return NULL;

    if (strcmp(name, "?") == 0) {
        return vt->status_str;
    }

    for (VarEntry *cur = vt->head; cur; cur = cur->next) {
        if (strcmp(cur->name, name) == 0) {
            return cur->value ? cur->value : "";
        }
    }
    return NULL;
}

int vars_set(VarTable *vt, const char *name, const char *value, bool exported) {
    if (!vt || !name) return -1;

    for (VarEntry *cur = vt->head; cur; cur = cur->next) {
        if (strcmp(cur->name, name) == 0) {
            char *new_val = value ? strdup(value) : strdup("");
            if (!new_val) return -1;
            free(cur->value);
            cur->value = new_val;
            if (exported) cur->exported = true;
            return 0;
        }
    }

    VarEntry *entry = malloc(sizeof(VarEntry));
    if (!entry) return -1;

    entry->name = strdup(name);
    entry->value = value ? strdup(value) : strdup("");
    entry->exported = exported;
    entry->next = vt->head;
    vt->head = entry;

    return 0;
}

int vars_unset(VarTable *vt, const char *name) {
    if (!vt || !name) return -1;

    VarEntry **indirect = &vt->head;
    while (*indirect) {
        VarEntry *cur = *indirect;
        if (strcmp(cur->name, name) == 0) {
            *indirect = cur->next;
            free(cur->name);
            free(cur->value);
            free(cur);
            return 0;
        }
        indirect = &(*indirect)->next;
    }
    return 0;
}

int vars_export(VarTable *vt, const char *name) {
    if (!vt || !name) return -1;

    for (VarEntry *cur = vt->head; cur; cur = cur->next) {
        if (strcmp(cur->name, name) == 0) {
            cur->exported = true;
            return 0;
        }
    }

    return vars_set(vt, name, "", true);
}

void vars_set_status(VarTable *vt, int status) {
    if (!vt) return;
    vt->last_status = status;
    snprintf(vt->status_str, sizeof(vt->status_str), "%d", status);
}

int vars_get_status(const VarTable *vt) {
    return vt ? vt->last_status : 0;
}

char **vars_to_envp(const VarTable *vt) {
    if (!vt) return NULL;

    size_t count = 0;
    for (VarEntry *cur = vt->head; cur; cur = cur->next) {
        if (cur->exported && cur->name && cur->value) {
            count++;
        }
    }

    char **envp = malloc((count + 1) * sizeof(char *));
    if (!envp) return NULL;

    size_t i = 0;
    for (VarEntry *cur = vt->head; cur; cur = cur->next) {
        if (cur->exported && cur->name && cur->value) {
            size_t len = strlen(cur->name) + 1 + strlen(cur->value) + 1;
            char *buf = malloc(len);
            if (buf) {
                snprintf(buf, len, "%s=%s", cur->name, cur->value);
                envp[i++] = buf;
            }
        }
    }
    envp[i] = NULL;
    return envp;
}

void vars_free_envp(char **envp) {
    if (!envp) return;
    for (char **p = envp; *p; p++) {
        free(*p);
    }
    free(envp);
}
