/*
 * builtins.c: Shell built-in command implementations.
 *
 * Implements core built-in commands writing to OutputSink abstractions:
 * cd, pwd, echo (-n, -e), history, export, unset, bind, jobs, fg, bg, multiWatch, exit.
 */

#define _GNU_SOURCE
#include "builtins.h"
#include "shell.h"
#include "exec.h"
#include "multiwatch.h"
#include "../keymap.h"
#include "../proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <errno.h>

static void print_echo_escaped(OutputSink *out, const char *s) {
    while (*s) {
        if (*s == '\\' && *(s + 1)) {
            s++;
            switch (*s) {
            case 'a': sink_write(out, "\a", 1); break;
            case 'b': sink_write(out, "\b", 1); break;
            case 'e': sink_write(out, "\033", 1); break;
            case 'f': sink_write(out, "\f", 1); break;
            case 'n': sink_write(out, "\n", 1); break;
            case 'r': sink_write(out, "\r", 1); break;
            case 't': sink_write(out, "\t", 1); break;
            case 'v': sink_write(out, "\v", 1); break;
            case '\\': sink_write(out, "\\", 1); break;
            case 'c': return; /* suppress remaining output */
            default:
                sink_write(out, "\\", 1);
                sink_write(out, s, 1);
                break;
            }
        } else {
            sink_write(out, s, 1);
        }
        s++;
    }
}

int builtin_echo(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err) {
    (void)sh; (void)err;
    bool newline = true;
    bool interpret_escapes = false;

    int idx = 1;
    while (idx < argc && argv[idx][0] == '-' && argv[idx][1] != '\0') {
        bool valid = true;
        for (int j = 1; argv[idx][j]; j++) {
            if (argv[idx][j] != 'n' && argv[idx][j] != 'e' && argv[idx][j] != 'E') {
                valid = false;
                break;
            }
        }
        if (!valid) break;

        for (int j = 1; argv[idx][j]; j++) {
            if (argv[idx][j] == 'n') newline = false;
            else if (argv[idx][j] == 'e') interpret_escapes = true;
            else if (argv[idx][j] == 'E') interpret_escapes = false;
        }
        idx++;
    }

    for (int i = idx; i < argc; i++) {
        if (i > idx) {
            sink_write(out, " ", 1);
        }
        if (interpret_escapes) {
            print_echo_escaped(out, argv[i]);
        } else {
            sink_write(out, argv[i], strlen(argv[i]));
        }
    }

    if (newline) {
        sink_write(out, "\n", 1);
    }
    return 0;
}

int builtin_pwd(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err) {
    (void)argc; (void)argv; (void)err;
    sink_printf(out, "%s\n", sh->cwd);
    return 0;
}

int builtin_cd(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err) {
    const char *target = NULL;
    bool is_cd_dash = false;

    if (argc == 1) {
        target = vars_get(&sh->vars, "HOME");
        if (!target || !target[0]) {
            target = getenv("HOME");
        }
        if (!target || !target[0]) {
            target = "/";
        }
    } else {
        if (strcmp(argv[1], "-") == 0) {
            target = vars_get(&sh->vars, "OLDPWD");
            if (!target || !target[0]) {
                sink_printf(err, "terminator: cd: OLDPWD not set\n");
                return 1;
            }
            is_cd_dash = true;
        } else {
            target = argv[1];
        }
    }

    /* Handle ~ expansion */
    char expanded[PATH_MAX];
    if (target[0] == '~') {
        const char *home = vars_get(&sh->vars, "HOME");
        if (!home || !home[0]) home = getenv("HOME");
        if (!home || !home[0]) home = "/";

        if (target[1] == '\0') {
            snprintf(expanded, sizeof(expanded), "%s", home);
            target = expanded;
        } else if (target[1] == '/') {
            snprintf(expanded, sizeof(expanded), "%s%s", home, target + 1);
            target = expanded;
        }
    }

    /* Build full absolute path */
    char full_path[PATH_MAX];
    if (target[0] == '/') {
        strncpy(full_path, target, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", sh->cwd, target);
    }

    char resolved[PATH_MAX];
    if (!realpath(full_path, resolved)) {
        int e = errno;
        if (e == ENOENT) {
            sink_printf(err, "terminator: cd: %s: No such file or directory\n", argc > 1 ? argv[1] : target);
        } else if (e == ENOTDIR) {
            sink_printf(err, "terminator: cd: %s: Not a directory\n", argc > 1 ? argv[1] : target);
        } else if (e == EACCES) {
            sink_printf(err, "terminator: cd: %s: Permission denied\n", argc > 1 ? argv[1] : target);
        } else {
            sink_printf(err, "terminator: cd: %s: No such file or directory\n", argc > 1 ? argv[1] : target);
        }
        return 1;
    }

    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
        sink_printf(err, "terminator: cd: %s: Not a directory\n", argc > 1 ? argv[1] : target);
        return 1;
    }

    if (access(resolved, X_OK) != 0) {
        sink_printf(err, "terminator: cd: %s: Permission denied\n", argc > 1 ? argv[1] : target);
        return 1;
    }

    /* Update per-tab state WITHOUT calling process-wide chdir() */
    vars_set(&sh->vars, "OLDPWD", sh->cwd, true);
    strncpy(sh->cwd, resolved, sizeof(sh->cwd) - 1);
    sh->cwd[sizeof(sh->cwd) - 1] = '\0';
    vars_set(&sh->vars, "PWD", sh->cwd, true);
    shell_sync_envp(sh);

    if (is_cd_dash) {
        sink_printf(out, "%s\n", sh->cwd);
    }

    return 0;
}

int builtin_exit(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err) {
    (void)out;
    int stopped_count = jobs_count_stopped(&sh->jobs);
    if (stopped_count > 0 && !sh->exit_warned) {
        sink_printf(err, "There are stopped jobs.\n");
        sh->exit_warned = true;
        return 1;
    }

    int code = sh->last_status;
    if (argc >= 2) {
        code = (int)strtol(argv[1], NULL, 10);
    }
    sh->should_exit = true;
    sh->exit_code = code;
    return code;
}

int builtin_history(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err) {
    (void)argc; (void)argv; (void)err;
    if (!sh->shared) return 0;

    char (*lines)[HISTORY_MAX_LINE] = malloc(1000 * sizeof(*lines));
    uint64_t *seqs = malloc(1000 * sizeof(uint64_t));
    if (!lines || !seqs) {
        free(lines);
        free(seqs);
        return 1;
    }

    size_t count = history_snapshot(sh->shared, lines, seqs, 1000);
    for (size_t i = 0; i < count; i++) {
        sink_printf(out, "%5llu  %s\n", (unsigned long long)seqs[i], lines[i]);
    }

    free(lines);
    free(seqs);
    return 0;
}

int builtin_export(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err) {
    (void)err;
    if (argc == 1) {
        for (VarEntry *cur = sh->vars.head; cur; cur = cur->next) {
            if (cur->exported && cur->name && cur->value) {
                sink_printf(out, "export %s=\"%s\"\n", cur->name, cur->value);
            }
        }
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            size_t nlen = (size_t)(eq - argv[i]);
            char *name = malloc(nlen + 1);
            if (name) {
                memcpy(name, argv[i], nlen);
                name[nlen] = '\0';
                vars_set(&sh->vars, name, eq + 1, true);
                free(name);
            }
        } else {
            vars_export(&sh->vars, argv[i]);
        }
    }
    shell_sync_envp(sh);
    return 0;
}

int builtin_unset(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err) {
    (void)out; (void)err;
    for (int i = 1; i < argc; i++) {
        vars_unset(&sh->vars, argv[i]);
    }
    shell_sync_envp(sh);
    return 0;
}

int builtin_bind(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err) {
    char config_path[1024];
    keymap_get_config_path(config_path, sizeof(config_path));

    KeyMap km;
    char load_warns[1024] = {0};
    keymap_load_file(&km, config_path, load_warns, sizeof(load_warns));

    if (argc == 1) {
        /* Show all current bindings */
        for (int i = 1; i < ACTION_COUNT; i++) {
            char chord_buf[64];
            keymap_format_chord(&km.bindings[i].chord, chord_buf, sizeof(chord_buf));
            sink_printf(out, "bind %s = %s\n", keyaction_to_name(km.bindings[i].action), chord_buf);
        }
        return 0;
    }

    if (argc == 3 && strcmp(argv[1], "reset") == 0) {
        /* bind reset <action> */
        KeyAction act = keyaction_from_name(argv[2]);
        if (act == ACTION_NONE) {
            sink_printf(err, "terminator: bind: unknown action '%s'\n", argv[2]);
            return 1;
        }

        if (keymap_reset_binding(&km, act) != 0) {
            sink_printf(err, "terminator: bind: failed to reset action '%s'\n", argv[2]);
            return 1;
        }

        char err_buf[256];
        if (keymap_save_file(&km, config_path, err_buf, sizeof(err_buf)) != 0) {
            sink_printf(err, "terminator: bind: %s\n", err_buf);
            return 1;
        }

        if (sh && sh->from_tab_fd >= 0) {
            proto_write_frame_blocking(sh->from_tab_fd, FRAME_RELOAD_KEYS, NULL, 0);
        }

        return 0;
    }

    if (argc == 3) {
        /* bind <action> <chord> */
        KeyAction act = keyaction_from_name(argv[1]);
        if (act == ACTION_NONE) {
            sink_printf(err, "terminator: bind: unknown action '%s'\n", argv[1]);
            return 1;
        }

        KeyChord chord;
        if (!keymap_parse_chord(argv[2], &chord)) {
            sink_printf(err, "terminator: bind: invalid chord '%s'\n", argv[2]);
            return 1;
        }

        char err_buf[256];
        if (keymap_set_binding(&km, act, &chord, err_buf, sizeof(err_buf)) != 0) {
            sink_printf(err, "terminator: bind: %s\n", err_buf);
            return 1;
        }

        if (keymap_save_file(&km, config_path, err_buf, sizeof(err_buf)) != 0) {
            sink_printf(err, "terminator: bind: %s\n", err_buf);
            return 1;
        }

        if (sh && sh->from_tab_fd >= 0) {
            proto_write_frame_blocking(sh->from_tab_fd, FRAME_RELOAD_KEYS, NULL, 0);
        }

        return 0;
    }

    sink_printf(err, "usage: bind [action chord | reset action]\n");
    return 1;
}

int builtin_multiwatch(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err) {
    (void)sh; (void)argc; (void)argv; (void)out;
    sink_printf(err, "%s", MULTIWATCH_USAGE);
    return 1;
}

int builtin_jobs(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err) {
    (void)argc; (void)argv; (void)err;
    jobs_update_status(&sh->jobs);
    JobTable *table = &sh->jobs;
    Job *curr = jobs_get_current(table);
    Job *prev = jobs_get_previous(table);

    for (int i = 0; i < table->njobs; i++) {
        Job *j = &table->jobs[i];
        if (j->foreground) continue;

        char marker = ' ';
        if (j == curr) marker = '+';
        else if (j == prev) marker = '-';

        const char *state_str = "Running";
        if (j->state == JOB_STOPPED) state_str = "Stopped";
        else if (j->state == JOB_DONE) state_str = "Done";

        if (j->state == JOB_RUNNING) {
            sink_printf(out, "[%d]%c %-10s %s &\n", j->id, marker, state_str, j->cmd_text);
        } else {
            sink_printf(out, "[%d]%c %-10s %s\n", j->id, marker, state_str, j->cmd_text);
        }
    }
    return 0;
}

int builtin_fg(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err) {
    JobTable *table = &sh->jobs;
    const char *spec = (argc > 1) ? argv[1] : NULL;
    Job *j = jobs_resolve_spec(table, spec, "fg", err);
    if (!j) return 1;

    sink_printf(out, "%s\n", j->cmd_text);

    if (j->state == JOB_STOPPED && j->pgid > 1) {
        kill(-j->pgid, SIGCONT);
    }
    j->state = JOB_RUNNING;
    j->foreground = true;
    jobs_update_order(table, j);

    int status = job_wait_foreground(sh, j, sh->from_tab_fd, sh->to_tab_fd);
    return status;
}

int builtin_bg(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err) {
    JobTable *table = &sh->jobs;
    const char *spec = (argc > 1) ? argv[1] : NULL;
    Job *j = jobs_resolve_spec(table, spec, "bg", err);
    if (!j) return 1;

    j->state = JOB_RUNNING;
    j->foreground = false;
    jobs_update_order(table, j);

    if (j->pgid > 1) {
        kill(-j->pgid, SIGCONT);
    }

    Job *curr = jobs_get_current(table);
    char marker = (j == curr) ? '+' : '-';
    sink_printf(out, "[%d]%c %s &\n", j->id, marker, j->cmd_text);
    return 0;
}

int builtin_clear(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err) {
    (void)sh; (void)argc; (void)argv; (void)err;
    if (out->type == SINK_FRAMED) {
        if (proto_write_frame_blocking(out->fd, FRAME_CLEAR, NULL, 0) != 0) {
            return 1;
        }
    } else {
        if (sink_write(out, "\033[H\033[2J", 7) < 0) {
            return 1;
        }
    }
    return 0;
}

int builtin_stub(Shell *sh, int argc, char **argv, OutputSink *out, OutputSink *err) {
    (void)sh; (void)argc; (void)out;
    sink_printf(err, "terminator: %s: not implemented yet\n", argv[0]);
    return 1;
}

static const BuiltinDef builtins_table[] = {
    { "cd", builtin_cd, true },
    { "exit", builtin_exit, true },
    { "export", builtin_export, true },
    { "unset", builtin_unset, true },
    { "bind", builtin_bind, true },
    { "pwd", builtin_pwd, false },
    { "echo", builtin_echo, false },
    { "history", builtin_history, false },
    { "multiWatch", builtin_multiwatch, false },
    { "jobs", builtin_jobs, true },
    { "fg", builtin_fg, true },
    { "bg", builtin_bg, true },
    { "clear", builtin_clear, true },
    { "source", builtin_stub, true },
    { NULL, NULL, false }
};

const BuiltinDef *builtin_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; builtins_table[i].name; i++) {
        if (strcmp(builtins_table[i].name, name) == 0) {
            return &builtins_table[i];
        }
    }
    return NULL;
}
