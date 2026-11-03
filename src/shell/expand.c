/*
 * expand.c: Six-stage shell word expansion pipeline.
 *
 * Implements bash-compatible word expansion:
 * 1. Tilde expansion (~, ~/path).
 * 2. Parameter expansion ($VAR, ${VAR}, $?, $$, $#, $0).
 * 3. Command substitution ($(cmd) and `cmd`, evaluated in forked subshells up to depth 16).
 * 4. Pathname globbing (*, ?, [...]).
 * 5. Word splitting across whitespace.
 * 6. Quote removal.
 */

#define _GNU_SOURCE
#include "expand.h"
#include "vars.h"
#include "../syscompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <glob.h>
#include <pwd.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

extern char **environ;

/* Subshell command substitution execution */
char *expand_command_subst(Shell *sh, const char *inner_cmd, int depth) {
    if (!inner_cmd || depth > MAX_SUBST_DEPTH) {
        return strdup("");
    }

    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) < 0) {
        return strdup("");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return strdup("");
    }

    if (pid == 0) {
        /* Child process for command substitution subshell */
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);

        /* Reset signals to defaults */
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGQUIT, &sa, NULL);
        sigaction(SIGTSTP, &sa, NULL);
        sigaction(SIGTTIN, &sa, NULL);
        sigaction(SIGTTOU, &sa, NULL);
        sigaction(SIGPIPE, &sa, NULL);
        sigaction(SIGCHLD, &sa, NULL);

        if (sh->cwd[0]) {
            chdir(sh->cwd);
        }
        if (sh->envp) {
            environ = sh->envp;
        }

        /* Child must not touch shared locks or history */
        sh->shared = NULL;

        /* Close extra file descriptors */
        for (int fd = 3; fd < 256; fd++) {
            close(fd);
        }

        int status = shell_execute_subshell(sh, inner_cmd, STDOUT_FILENO);
        _exit(status);
    }

    /* Parent */
    close(pfd[1]);

    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(pfd[0]);
        waitpid(pid, NULL, 0);
        return strdup("");
    }

    while (1) {
        if (len + 1024 >= cap) {
            cap *= 2;
            char *new_buf = realloc(buf, cap);
            if (!new_buf) break;
            buf = new_buf;
        }
        ssize_t n = read(pfd[0], buf + len, cap - len - 1);
        if (n > 0) {
            len += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    buf[len] = '\0';
    close(pfd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    /* Strip trailing newlines per POSIX specification */
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }

    return buf;
}

/* Helper to scan matching closing delimiter for $( ... ) */
static const char *find_matching_paren(const char *str) {
    if (!str) return NULL;
    int depth = 1;
    const char *p = str;

    while (*p && depth > 0) {
        if (*p == '\\') {
            p++;
            if (*p) p++;
            continue;
        }
        if (*p == '\'') {
            p++;
            while (*p && *p != '\'') p++;
            if (*p == '\'') p++;
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
                    const char *sub = find_matching_paren(p + 2);
                    if (!sub) return NULL;
                    p = sub + 1;
                    continue;
                }
                p++;
            }
            if (*p == '"') p++;
            continue;
        }
        if (*p == '$' && *(p + 1) == '(') {
            depth++;
            p += 2;
            continue;
        }
        if (*p == ')') {
            depth--;
            if (depth == 0) return p;
            p++;
            continue;
        }
        p++;
    }

    return NULL;
}

/* Helper to scan matching closing backtick ` */
static const char *find_matching_backtick(const char *str) {
    if (!str) return NULL;
    const char *p = str;
    while (*p) {
        if (*p == '\\') {
            p++;
            if (*p) p++;
            continue;
        }
        if (*p == '`') {
            return p;
        }
        p++;
    }
    return NULL;
}

/* Expand parameters ($VAR, $?, $$, etc.) and command substitutions ($(cmd), `cmd`) in a string */
char *expand_string(Shell *sh, const char *input, int depth) {
    if (!input) return strdup("");
    if (depth > MAX_SUBST_DEPTH) return strdup(input);

    size_t cap = strlen(input) + 256;
    size_t len = 0;
    char *out = malloc(cap);
    if (!out) return strdup("");
    out[0] = '\0';

    const char *p = input;

    while (*p) {
        /* Ensure capacity */
        if (len + 512 >= cap) {
            cap = (cap * 2) + 512;
            char *new_out = realloc(out, cap);
            if (!new_out) {
                free(out);
                return strdup("");
            }
            out = new_out;
        }

        /* 1. Command substitution: $( ... ) */
        if (*p == '$' && *(p + 1) == '(') {
            const char *start = p + 2;
            const char *end = find_matching_paren(start);
            if (end) {
                size_t cmd_len = (size_t)(end - start);
                char *inner = malloc(cmd_len + 1);
                if (inner) {
                    memcpy(inner, start, cmd_len);
                    inner[cmd_len] = '\0';

                    char *subst = expand_command_subst(sh, inner, depth + 1);
                    free(inner);

                    if (subst) {
                        size_t slen = strlen(subst);
                        while (len + slen + 64 >= cap) {
                            cap = (cap * 2) + slen + 64;
                            char *new_out = realloc(out, cap);
                            if (!new_out) break;
                            out = new_out;
                        }
                        memcpy(out + len, subst, slen);
                        len += slen;
                        out[len] = '\0';
                        free(subst);
                    }
                }
                p = end + 1;
                continue;
            }
        }

        /* 2. Command substitution: `...` */
        if (*p == '`') {
            const char *start = p + 1;
            const char *end = find_matching_backtick(start);
            if (end) {
                size_t cmd_len = (size_t)(end - start);
                char *inner = malloc(cmd_len + 1);
                if (inner) {
                    /* Unescape \` inside backticks */
                    size_t j = 0;
                    for (size_t i = 0; i < cmd_len; i++) {
                        if (start[i] == '\\' && (start[i + 1] == '`' || start[i + 1] == '\\' || start[i + 1] == '$')) {
                            i++;
                        }
                        inner[j++] = start[i];
                    }
                    inner[j] = '\0';

                    char *subst = expand_command_subst(sh, inner, depth + 1);
                    free(inner);

                    if (subst) {
                        size_t slen = strlen(subst);
                        while (len + slen + 64 >= cap) {
                            cap = (cap * 2) + slen + 64;
                            char *new_out = realloc(out, cap);
                            if (!new_out) break;
                            out = new_out;
                        }
                        memcpy(out + len, subst, slen);
                        len += slen;
                        out[len] = '\0';
                        free(subst);
                    }
                }
                p = end + 1;
                continue;
            }
        }

        /* 3. Parameter expansion: $VAR, ${VAR}, $?, $$, $#, $0..$9 */
        if (*p == '$') {
            p++;
            if (*p == '?') {
                p++;
                const char *val = sh ? vars_get(&sh->vars, "?") : "0";
                if (!val) val = "0";
                size_t vlen = strlen(val);
                memcpy(out + len, val, vlen);
                len += vlen;
                out[len] = '\0';
                continue;
            } else if (*p == '$') {
                p++;
                char pid_str[32];
                snprintf(pid_str, sizeof(pid_str), "%d", (int)getpid());
                size_t vlen = strlen(pid_str);
                memcpy(out + len, pid_str, vlen);
                len += vlen;
                out[len] = '\0';
                continue;
            } else if (*p == '#') {
                p++;
                out[len++] = '0';
                out[len] = '\0';
                continue;
            } else if (*p == '0') {
                p++;
                const char *val = "terminator";
                size_t vlen = strlen(val);
                memcpy(out + len, val, vlen);
                len += vlen;
                out[len] = '\0';
                continue;
            } else if (isdigit((unsigned char)*p)) {
                /* Positional parameter $1..$9 */
                char vname[2] = { *p, '\0' };
                p++;
                const char *val = sh ? vars_get(&sh->vars, vname) : "";
                if (!val) val = "";
                size_t vlen = strlen(val);
                memcpy(out + len, val, vlen);
                len += vlen;
                out[len] = '\0';
                continue;
            } else if (*p == '{') {
                p++;
                char varname[256];
                size_t vlen = 0;
                while (*p && *p != '}' && vlen < sizeof(varname) - 1) {
                    varname[vlen++] = *p++;
                }
                varname[vlen] = '\0';
                if (*p == '}') p++;
                const char *val = sh ? vars_get(&sh->vars, varname) : NULL;
                if (!val) val = getenv(varname);
                if (val) {
                    size_t vl = strlen(val);
                    while (len + vl + 64 >= cap) {
                        cap = (cap * 2) + vl + 64;
                        char *new_out = realloc(out, cap);
                        if (!new_out) break;
                        out = new_out;
                    }
                    memcpy(out + len, val, vl);
                    len += vl;
                    out[len] = '\0';
                }
                continue;
            } else if (isalpha((unsigned char)*p) || *p == '_') {
                char varname[256];
                size_t vlen = 0;
                while ((isalnum((unsigned char)*p) || *p == '_') && vlen < sizeof(varname) - 1) {
                    varname[vlen++] = *p++;
                }
                varname[vlen] = '\0';
                const char *val = sh ? vars_get(&sh->vars, varname) : NULL;
                if (!val) val = getenv(varname);
                if (val) {
                    size_t vl = strlen(val);
                    while (len + vl + 64 >= cap) {
                        cap = (cap * 2) + vl + 64;
                        char *new_out = realloc(out, cap);
                        if (!new_out) break;
                        out = new_out;
                    }
                    memcpy(out + len, val, vl);
                    len += vl;
                    out[len] = '\0';
                }
                continue;
            } else {
                /* Standalone $ */
                out[len++] = '$';
                out[len] = '\0';
                continue;
            }
        }

        /* Regular character */
        out[len++] = *p++;
        out[len] = '\0';
    }

    out[len] = '\0';
    return out;
}

/* Helper to perform tilde expansion on an unquoted segment */
static char *expand_tilde(Shell *sh, const char *text) {
    if (!text || text[0] != '~') {
        return text ? strdup(text) : strdup("");
    }

    const char *slash = strchr(text, '/');
    if (text[1] == '\0' || text[1] == '/') {
        /* ~ or ~/path */
        const char *home = sh ? vars_get(&sh->vars, "HOME") : NULL;
        if (!home || !home[0]) home = getenv("HOME");
        if (!home || !home[0]) home = "/";

        size_t hlen = strlen(home);
        size_t rest_len = slash ? strlen(slash) : 0;
        char *res = malloc(hlen + rest_len + 1);
        if (!res) return strdup(text);

        strcpy(res, home);
        if (slash) {
            strcat(res, slash);
        }
        return res;
    } else {
        /* ~user or ~user/path */
        size_t ulen = slash ? (size_t)(slash - (text + 1)) : strlen(text + 1);
        char user[128];
        if (ulen < sizeof(user)) {
            memcpy(user, text + 1, ulen);
            user[ulen] = '\0';

            struct passwd *pw = getpwnam(user);
            if (pw && pw->pw_dir) {
                size_t dlen = strlen(pw->pw_dir);
                size_t rest_len = slash ? strlen(slash) : 0;
                char *res = malloc(dlen + rest_len + 1);
                if (res) {
                    strcpy(res, pw->pw_dir);
                    if (slash) {
                        strcat(res, slash);
                    }
                    return res;
                }
            }
        }
    }

    return strdup(text);
}

typedef struct {
    char *text;
    bool has_unquoted_glob;
} WordCandidate;

int expand_word_token(Shell *sh, const Token *tok, char ***out_argv, int *out_argc) {
    if (!tok || tok->kind != TOK_WORD || !out_argv || !out_argc) {
        return -1;
    }

    *out_argv = NULL;
    *out_argc = 0;

    /* 1. Step 1: Tilde expansion (only on leading unquoted segment) */
    WordSegment *seg = tok->segments;
    if (!seg) {
        return 0;
    }

    /* Temporary array of expanded segment texts and quote kinds */
    typedef struct {
        char *text;
        QuoteKind quote;
    } ExpandedSeg;

    int seg_count = 0;
    for (WordSegment *s = seg; s; s = s->next) seg_count++;

    ExpandedSeg *exp_segs = calloc(seg_count, sizeof(ExpandedSeg));
    if (!exp_segs) return -1;

    int idx = 0;
    for (WordSegment *s = seg; s; s = s->next, idx++) {
        exp_segs[idx].quote = s->quote;

        if (idx == 0 && s->quote == QUOTE_NONE && s->text && s->text[0] == '~') {
            char *tilde_exp = expand_tilde(sh, s->text);
            /* Then run parameter & command substitution on the result */
            exp_segs[idx].text = expand_string(sh, tilde_exp, 0);
            free(tilde_exp);
        } else if (s->quote == QUOTE_SINGLE) {
            /* Single quotes: 100% literal */
            exp_segs[idx].text = strdup(s->text ? s->text : "");
        } else {
            /* QUOTE_NONE or QUOTE_DOUBLE: parameter and command substitution */
            exp_segs[idx].text = expand_string(sh, s->text ? s->text : "", 0);
        }
    }

    /* 2. Step 4: Word splitting of UNQUOTED results on space/tab/newline.
     * Quoted segments are never split.
     * Preserves "" if quoted; drops empty words if unquoted. */
    WordCandidate *candidates = NULL;
    int cand_count = 0;
    int cand_cap = 0;

    char cur_buf[8192];
    size_t cur_len = 0;
    bool cur_has_content = false;
    bool cur_was_quoted = false;
    bool cur_has_glob = false;

    for (int i = 0; i < seg_count; i++) {
        const char *txt = exp_segs[i].text ? exp_segs[i].text : "";
        QuoteKind q = exp_segs[i].quote;

        if (q == QUOTE_SINGLE || q == QUOTE_DOUBLE) {
            cur_was_quoted = true;
            size_t tl = strlen(txt);
            for (size_t k = 0; k < tl && cur_len < sizeof(cur_buf) - 1; k++) {
                cur_buf[cur_len++] = txt[k];
            }
            cur_has_content = true;
        } else {
            /* QUOTE_NONE: subject to word splitting on whitespace */
            const char *p = txt;
            while (*p) {
                if (*p == ' ' || *p == '\t' || *p == '\n') {
                    if (cur_has_content || cur_was_quoted) {
                        cur_buf[cur_len] = '\0';
                        if (cand_count >= cand_cap) {
                            cand_cap = cand_cap == 0 ? 8 : cand_cap * 2;
                            candidates = realloc(candidates, cand_cap * sizeof(WordCandidate));
                        }
                        candidates[cand_count].text = strdup(cur_buf);
                        candidates[cand_count].has_unquoted_glob = cur_has_glob;
                        cand_count++;

                        cur_len = 0;
                        cur_has_content = false;
                        cur_was_quoted = false;
                        cur_has_glob = false;
                    }
                    p++;
                    continue;
                }

                if (*p == '*' || *p == '?' || *p == '[') {
                    cur_has_glob = true;
                }
                if (cur_len < sizeof(cur_buf) - 1) {
                    cur_buf[cur_len++] = *p;
                }
                cur_has_content = true;
                p++;
            }
        }
    }

    if (cur_has_content || cur_was_quoted) {
        cur_buf[cur_len] = '\0';
        if (cand_count >= cand_cap) {
            cand_cap = cand_cap == 0 ? 8 : cand_cap * 2;
            candidates = realloc(candidates, cand_cap * sizeof(WordCandidate));
        }
        candidates[cand_count].text = strdup(cur_buf);
        candidates[cand_count].has_unquoted_glob = cur_has_glob;
        cand_count++;
    }

    /* Free expanded segment texts */
    for (int i = 0; i < seg_count; i++) {
        free(exp_segs[i].text);
    }
    free(exp_segs);

    /* 3. Step 5: Pathname Globbing of unquoted *, ?, [..] relative to sh->cwd */
    char **final_argv = NULL;
    int final_argc = 0;
    int final_cap = 0;

    for (int i = 0; i < cand_count; i++) {
        WordCandidate *c = &candidates[i];

        if (c->has_unquoted_glob && (strchr(c->text, '*') || strchr(c->text, '?') || strchr(c->text, '['))) {
            glob_t gb;
            memset(&gb, 0, sizeof(gb));

            char pattern[PATH_MAX];
            bool is_absolute = (c->text[0] == '/');

            if (is_absolute) {
                snprintf(pattern, sizeof(pattern), "%s", c->text);
            } else {
                snprintf(pattern, sizeof(pattern), "%s/%s", sh ? sh->cwd : ".", c->text);
            }

            int gret = glob(pattern, 0, NULL, &gb);
            if (gret == 0 && gb.gl_pathc > 0) {
                size_t prefix_len = is_absolute ? 0 : (strlen(sh ? sh->cwd : ".") + 1);

                for (size_t k = 0; k < gb.gl_pathc; k++) {
                    const char *match = gb.gl_pathv[k];
                    if (!is_absolute && strlen(match) >= prefix_len) {
                        match += prefix_len;
                    }
                    if (final_argc + 1 >= final_cap) {
                        final_cap = final_cap == 0 ? 8 : final_cap * 2;
                        final_argv = realloc(final_argv, final_cap * sizeof(char *));
                    }
                    final_argv[final_argc++] = strdup(match);
                }
                globfree(&gb);
                free(c->text);
                continue;
            }
            globfree(&gb);
        }

        /* No match or no globbing needed: keep pattern literal */
        if (final_argc + 1 >= final_cap) {
            final_cap = final_cap == 0 ? 8 : final_cap * 2;
            final_argv = realloc(final_argv, final_cap * sizeof(char *));
        }
        final_argv[final_argc++] = c->text;
    }

    free(candidates);

    if (final_argc > 0) {
        final_argv = realloc(final_argv, (final_argc + 1) * sizeof(char *));
        final_argv[final_argc] = NULL;
    }

    *out_argv = final_argv;
    *out_argc = final_argc;
    return 0;
}
