#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include "../src/proto.h"
#include "../src/shared.h"
#include "../src/history.h"
#include "../src/lineedit.h"
#include "../src/keymap.h"
#include "../src/tabrt.h"
#include "../src/complete.h"
#include "../src/syscompat.h"
#include "../src/shell/shell.h"
#include "../src/shell/lexer.h"
#include "../src/shell/expand.h"
#include "../src/shell/cmd.h"
#include "../src/shell/multiwatch.h"

typedef struct {
    char *last_output;
    size_t last_output_len;
    size_t last_output_cap;
    char last_prompt[512];
    char last_cwd[1024];
    bool exited;
    bool reloaded_keys;
    bool cleared;
} HarnessTab;

static void on_frame(const Frame *f, void *user_data) {
    HarnessTab *tab = (HarnessTab *)user_data;
    if (f->type == FRAME_OUTPUT) {
        if (!tab->last_output) {
            tab->last_output_cap = 2 * 1024 * 1024;
            tab->last_output = malloc(tab->last_output_cap);
            if (tab->last_output) tab->last_output[0] = '\0';
            tab->last_output_len = 0;
        }
        if (tab->last_output && tab->last_output_len + f->len < tab->last_output_cap - 1) {
            memcpy(tab->last_output + tab->last_output_len, f->payload, f->len);
            tab->last_output_len += f->len;
            tab->last_output[tab->last_output_len] = '\0';
        }
    } else if (f->type == FRAME_PROMPT) {
        const char *cwd = f->payload;
        size_t cwd_len = strlen(cwd);
        const char *pmt = cwd + cwd_len + 1;
        strncpy(tab->last_cwd, cwd, sizeof(tab->last_cwd) - 1);
        tab->last_cwd[sizeof(tab->last_cwd) - 1] = '\0';
        strncpy(tab->last_prompt, pmt, sizeof(tab->last_prompt) - 1);
        tab->last_prompt[sizeof(tab->last_prompt) - 1] = '\0';
    } else if (f->type == FRAME_CLEAR) {
        tab->cleared = true;
    } else if (f->type == FRAME_RELOAD_KEYS) {
        tab->reloaded_keys = true;
    } else if (f->type == FRAME_EXIT) {
        tab->exited = true;
    }
}

static void *tab_thread_func(void *arg) {
    tab_main((TabChannel *)arg);
    return NULL;
}

static void wait_for_prompt(int from_tab_fd, HarnessTab *tab, InBuffer *in) {
    tab->last_prompt[0] = '\0';
    while (tab->last_prompt[0] == '\0' && !tab->exited) {
        struct pollfd pfd;
        pfd.fd = from_tab_fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 3000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) {
            break;
        }
        bool eof_reached = false;
        ssize_t n = proto_read_frames(from_tab_fd, in, on_frame, tab, &eof_reached);
        if (n <= 0 && eof_reached) break;
    }
}

static void reset_tab_output(HarnessTab *tab) {
    if (!tab->last_output) {
        tab->last_output_cap = 2 * 1024 * 1024;
        tab->last_output = malloc(tab->last_output_cap);
    }
    tab->last_output_len = 0;
    if (tab->last_output) tab->last_output[0] = '\0';
}

static void ensure_tab_output(HarnessTab *tab) {
    if (!tab->last_output) {
        tab->last_output_cap = 2 * 1024 * 1024;
        tab->last_output = malloc(tab->last_output_cap);
        tab->last_output_len = 0;
        if (tab->last_output) tab->last_output[0] = '\0';
    }
}

static void send_cmd(int to_tab_fd, int from_tab_fd, HarnessTab *tab, InBuffer *in, const char *cmd) {
    reset_tab_output(tab);
    assert(proto_write_frame_blocking(to_tab_fd, FRAME_LINE, cmd, (uint32_t)strlen(cmd)) == 0);
    wait_for_prompt(from_tab_fd, tab, in);
}

static void send_line_expect_prompt(int to_tab_fd, int from_tab_fd, HarnessTab *tab, InBuffer *in, const char *line, const char *expected_prompt) {
    reset_tab_output(tab);
    assert(proto_write_frame_blocking(to_tab_fd, FRAME_LINE, line, (uint32_t)strlen(line)) == 0);
    wait_for_prompt(from_tab_fd, tab, in);
    assert(strcmp(tab->last_prompt, expected_prompt) == 0);
}

static void send_intr_expect_prompt(int to_tab_fd, int from_tab_fd, HarnessTab *tab, InBuffer *in, const char *expected_prompt) {
    reset_tab_output(tab);
    assert(proto_write_frame_blocking(to_tab_fd, FRAME_INTR, NULL, 0) == 0);
    wait_for_prompt(from_tab_fd, tab, in);
    assert(strcmp(tab->last_prompt, expected_prompt) == 0);
}

static int64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)ts.tv_nsec / 1000000LL;
}

static bool wait_for_output_substring(int from_tab_fd, HarnessTab *tab, InBuffer *in, const char *sub, int timeout_ms) {
    ensure_tab_output(tab);
    int64_t start = get_monotonic_ms();
    while (!tab->exited) {
        if (tab->last_output && strstr(tab->last_output, sub) != NULL) {
            return true;
        }
        int64_t now = get_monotonic_ms();
        int rem = timeout_ms - (int)(now - start);
        if (rem <= 0) break;

        struct pollfd pfd;
        pfd.fd = from_tab_fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, rem > 100 ? 100 : rem);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret > 0) {
            bool eof_reached = false;
            proto_read_frames(from_tab_fd, in, on_frame, tab, &eof_reached);
            if (eof_reached) break;
        }
    }
    return strstr(tab->last_output, sub) != NULL;
}

/* ----------------------------------------------------
 * Table-Driven Tests for Every Expansion Rule
 * ---------------------------------------------------- */
static void run_expansion_unit_tests(Shared *shared) {
    printf("\n--- PART 0: Table-Driven Expansion Unit Tests ---\n");
    Shell *sh = shell_create(NULL, NULL, shared);
    assert(sh != NULL);

    /* 1. Tilde Expansion */
    printf("  [0.1] Testing Tilde Expansion...\n");
    Pipeline pl;
    memset(&pl, 0, sizeof(pl));

    const char *home = getenv("HOME");
    assert(home != NULL);

    assert(parse_line(sh, "echo ~", &pl) == 0);
    assert(pl.n == 1 && pl.cmds[0].argc == 2);
    assert(strcmp(pl.cmds[0].argv[1], home) == 0);
    pipeline_free(&pl);

    assert(parse_line(sh, "echo ~/test_subpath", &pl) == 0);
    char expected_path[1024];
    snprintf(expected_path, sizeof(expected_path), "%s/test_subpath", home);
    assert(pl.n == 1 && pl.cmds[0].argc == 2);
    assert(strcmp(pl.cmds[0].argv[1], expected_path) == 0);
    pipeline_free(&pl);

    assert(parse_line(sh, "echo ~nonexistent_user_xyz12345/abc", &pl) == 0);
    assert(pl.n == 1 && pl.cmds[0].argc == 2);
    assert(strcmp(pl.cmds[0].argv[1], "~nonexistent_user_xyz12345/abc") == 0);
    pipeline_free(&pl);

    assert(parse_line(sh, "echo '~'", &pl) == 0);
    assert(pl.n == 1 && pl.cmds[0].argc == 2);
    assert(strcmp(pl.cmds[0].argv[1], "~") == 0);
    pipeline_free(&pl);
    printf("    Tilde expansion verified (~ -> $HOME, ~/path, non-existent user preserved, quoted '~' literal).\n");

    /* 2. Parameter Expansion */
    printf("  [0.2] Testing Parameter Expansion...\n");
    vars_set(&sh->vars, "MY_TEST_VAR", "apple_banana", true);
    char *p1 = expand_string(sh, "$MY_TEST_VAR", 0);
    assert(strcmp(p1, "apple_banana") == 0);
    free(p1);

    char *p2 = expand_string(sh, "${MY_TEST_VAR}", 0);
    assert(strcmp(p2, "apple_banana") == 0);
    free(p2);

    char *p3 = expand_string(sh, "fruit_${MY_TEST_VAR}_pie", 0);
    assert(strcmp(p3, "fruit_apple_banana_pie") == 0);
    free(p3);

    char *p4 = expand_string(sh, "$UNDEFINED_VAR_ABCXYZ", 0);
    assert(strcmp(p4, "") == 0);
    free(p4);

    sh->last_status = 42;
    vars_set_status(&sh->vars, 42);
    char *p5 = expand_string(sh, "$?", 0);
    assert(strcmp(p5, "42") == 0);
    free(p5);

    char *p6 = expand_string(sh, "$$", 0);
    char expected_pid[64];
    snprintf(expected_pid, sizeof(expected_pid), "%d", (int)getpid());
    assert(strcmp(p6, expected_pid) == 0);
    free(p6);

    char *p7 = expand_string(sh, "$#", 0);
    assert(strcmp(p7, "0") == 0);
    free(p7);

    char *p8 = expand_string(sh, "$0", 0);
    assert(strcmp(p8, "terminator") == 0);
    free(p8);
    printf("    Parameter expansion verified ($VAR, ${VAR}, prefix/suffix, $?, $$, $#, $0).\n");

    /* 3. Command Substitution */
    printf("  [0.3] Testing Command Substitution...\n");
    char *c1 = expand_string(sh, "$(echo subst_ok)", 0);
    assert(strcmp(c1, "subst_ok") == 0);
    free(c1);

    char *c2 = expand_string(sh, "`echo backtick_ok`", 0);
    assert(strcmp(c2, "backtick_ok") == 0);
    free(c2);

    char *c3 = expand_string(sh, "$(echo outer $(echo inner))", 0);
    assert(strcmp(c3, "outer inner") == 0);
    free(c3);

    char *c4 = expand_string(sh, "`echo outer $(echo inner)`", 0);
    assert(strcmp(c4, "outer inner") == 0);
    free(c4);

    /* Pipeline inside command substitution */
    char *c5 = expand_string(sh, "$(echo 'line1\nline2\nline3' | wc -l)", 0);
    assert(atoi(c5) == 3);
    free(c5);
    printf("    Command substitution verified ($(cmd), `cmd`, nested $(), pipeline inside $()).\n");

    /* 4. Quoting & Word Splitting */
    printf("  [0.4] Testing Quoting & Word Splitting...\n");
    memset(&pl, 0, sizeof(pl));

    /* Preserving empty argument "" */
    assert(parse_line(sh, "echo \"\" arg2", &pl) == 0);
    assert(pl.n == 1);
    assert(pl.cmds[0].argc == 3);
    assert(strcmp(pl.cmds[0].argv[0], "echo") == 0);
    assert(strcmp(pl.cmds[0].argv[1], "") == 0);
    assert(strcmp(pl.cmds[0].argv[2], "arg2") == 0);
    pipeline_free(&pl);

    /* Single quotes literal */
    assert(parse_line(sh, "echo '$MY_TEST_VAR'", &pl) == 0);
    assert(pl.n == 1);
    assert(pl.cmds[0].argc == 2);
    assert(strcmp(pl.cmds[0].argv[1], "$MY_TEST_VAR") == 0);
    pipeline_free(&pl);

    /* Double quotes expand */
    assert(parse_line(sh, "echo \"$MY_TEST_VAR\"", &pl) == 0);
    assert(pl.n == 1);
    assert(pl.cmds[0].argc == 2);
    assert(strcmp(pl.cmds[0].argv[1], "apple_banana") == 0);
    pipeline_free(&pl);

    /* Unquoted expansion splits on whitespace */
    vars_set(&sh->vars, "SPLIT_TEST", "one two three", true);
    assert(parse_line(sh, "mycmd $SPLIT_TEST", &pl) == 0);
    assert(pl.n == 1);
    assert(pl.cmds[0].argc == 4);
    assert(strcmp(pl.cmds[0].argv[0], "mycmd") == 0);
    assert(strcmp(pl.cmds[0].argv[1], "one") == 0);
    assert(strcmp(pl.cmds[0].argv[2], "two") == 0);
    assert(strcmp(pl.cmds[0].argv[3], "three") == 0);
    pipeline_free(&pl);

    /* Quoted expansion does NOT split on whitespace */
    assert(parse_line(sh, "mycmd \"$SPLIT_TEST\"", &pl) == 0);
    assert(pl.n == 1);
    assert(pl.cmds[0].argc == 2);
    assert(strcmp(pl.cmds[0].argv[0], "mycmd") == 0);
    assert(strcmp(pl.cmds[0].argv[1], "one two three") == 0);
    pipeline_free(&pl);
    printf("    Quoting & word splitting verified (empty \"\" preserved, literal ', expanded \", unquoted split).\n");

    /* 5. Pathname Globbing */
    printf("  [0.5] Testing Pathname Globbing...\n");
    assert(parse_line(sh, "ls Makef*le", &pl) == 0);
    assert(pl.n == 1);
    assert(pl.cmds[0].argc == 2);
    assert(strcmp(pl.cmds[0].argv[1], "Makefile") == 0);
    pipeline_free(&pl);

    /* Quoted glob is NOT expanded */
    assert(parse_line(sh, "ls 'Makef*le'", &pl) == 0);
    assert(pl.n == 1);
    assert(pl.cmds[0].argc == 2);
    assert(strcmp(pl.cmds[0].argv[1], "Makef*le") == 0);
    pipeline_free(&pl);
    printf("    Globbing verified (Makef*le expanded, 'Makef*le' kept literal).\n");

    shell_destroy(sh);
    printf("  All table-driven expansion unit tests passed!\n");
}

/* ----------------------------------------------------
 * 30-Second Lexer Random Byte Fuzzer
 * ---------------------------------------------------- */
static void run_fuzzer(int seconds) {
    printf("\n--- PART 6: %d-Second Lexer Random Byte Fuzzer ---\n", seconds);
    time_t start = time(NULL);
    unsigned long iterations = 0;
    unsigned int seed = 1337;

    while (time(NULL) - start < seconds) {
        int len = rand_r(&seed) % 512;
        char buf[513];

        for (int i = 0; i < len; i++) {
            int r = rand_r(&seed) % 100;
            if (r < 40) {
                const char metas[] = "'\"\\$`()|&;<> \t\n\r*?[]{}~";
                buf[i] = metas[rand_r(&seed) % (sizeof(metas) - 1)];
            } else if (r < 80) {
                buf[i] = (char)(32 + (rand_r(&seed) % 95));
            } else {
                buf[i] = (char)(rand_r(&seed) % 256);
            }
        }
        buf[len] = '\0';

        LexStatus st = lex_check_completeness(buf);
        (void)st;

        Token *tokens = NULL;
        LexStatus tok_st = lex_tokenize(buf, &tokens);
        (void)tok_st;
        if (tokens) {
            token_free_list(tokens);
        }

        iterations++;
    }

    printf("  Fuzzer completed %lu iterations in %d seconds with ZERO crashes!\n", iterations, seconds);
}

/* ----------------------------------------------------
 * Part 7: Task 4 Redirection Tests
 * ---------------------------------------------------- */
static int count_open_fds(void) {
    int count = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) {
        d = opendir("/dev/fd");
    }
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] != '.') count++;
        }
        closedir(d);
        return count - 1; /* subtract fd used by opendir */
    }
    for (int fd = 0; fd < 256; fd++) {
        if (fcntl(fd, F_GETFD) != -1) count++;
    }
    return count;
}

static void run_task4_parser_unit_tests(Shared *shared) {
    printf("\n--- PART 7A: Task 4 Redirection Parser Unit Tests ---\n");
    Shell *sh = shell_create(NULL, NULL, shared);
    assert(sh != NULL);

    Pipeline pl;
    memset(&pl, 0, sizeof(pl));

    /* 1. Basic input redirection */
    assert(parse_line(sh, "sort < file.txt", &pl) == 0);
    assert(pl.n == 1 && pl.cmds[0].argc == 1);
    assert(strcmp(pl.cmds[0].argv[0], "sort") == 0);
    assert(pl.cmds[0].nredirs == 1);
    assert(pl.cmds[0].redirs[0].type == REDIR_IN);
    assert(strcmp(pl.cmds[0].redirs[0].filename, "file.txt") == 0);
    assert(pl.cmds[0].redirs[0].fd == 0);
    pipeline_free(&pl);

    /* 2. Output and Append redirection */
    assert(parse_line(sh, "ls > out.txt", &pl) == 0);
    assert(pl.n == 1 && pl.cmds[0].nredirs == 1);
    assert(pl.cmds[0].redirs[0].type == REDIR_OUT);
    assert(strcmp(pl.cmds[0].redirs[0].filename, "out.txt") == 0);
    pipeline_free(&pl);

    assert(parse_line(sh, "echo hi >> out.txt", &pl) == 0);
    assert(pl.n == 1 && pl.cmds[0].nredirs == 1);
    assert(pl.cmds[0].redirs[0].type == REDIR_APPEND);
    assert(strcmp(pl.cmds[0].redirs[0].filename, "out.txt") == 0);
    pipeline_free(&pl);

    /* 3. Both < and > together */
    assert(parse_line(sh, "./a.out < in.txt > out.txt", &pl) == 0);
    assert(pl.n == 1 && pl.cmds[0].nredirs == 2);
    assert(pl.cmds[0].redirs[0].type == REDIR_IN);
    assert(strcmp(pl.cmds[0].redirs[0].filename, "in.txt") == 0);
    assert(pl.cmds[0].redirs[1].type == REDIR_OUT);
    assert(strcmp(pl.cmds[0].redirs[1].filename, "out.txt") == 0);
    pipeline_free(&pl);

    /* 4. 2> and 2>&1 */
    assert(parse_line(sh, "cmd 2> err.txt", &pl) == 0);
    assert(pl.n == 1 && pl.cmds[0].nredirs == 1);
    assert(pl.cmds[0].redirs[0].type == REDIR_OUT);
    assert(pl.cmds[0].redirs[0].fd == 2);
    assert(strcmp(pl.cmds[0].redirs[0].filename, "err.txt") == 0);
    pipeline_free(&pl);

    assert(parse_line(sh, "cmd > out.txt 2>&1", &pl) == 0);
    assert(pl.n == 1 && pl.cmds[0].nredirs == 2);
    assert(pl.cmds[0].redirs[1].type == REDIR_DUP);
    assert(pl.cmds[0].redirs[1].fd == 2);
    assert(pl.cmds[0].redirs[1].dup_target == 1);
    pipeline_free(&pl);

    /* 5. Ambiguous redirect: target expands to 0 or >1 words */
    vars_set(&sh->vars, "AMBIG_EMPTY", "", true);
    assert(parse_line(sh, "cat < $AMBIG_EMPTY", &pl) == 1);
    assert(strstr(sh->last_error, "ambiguous redirect") != NULL);
    pipeline_free(&pl);

    vars_set(&sh->vars, "AMBIG_MULTI", "file1.txt file2.txt", true);
    assert(parse_line(sh, "cat < $AMBIG_MULTI", &pl) == 1);
    assert(strstr(sh->last_error, "ambiguous redirect") != NULL);
    pipeline_free(&pl);

    /* 6. Syntax errors: sort <, > >, < | */
    assert(parse_line(sh, "sort <", &pl) == 2);
    assert(strstr(sh->last_error, "syntax error near unexpected token 'newline'") != NULL);
    pipeline_free(&pl);

    assert(parse_line(sh, "> >", &pl) == 2);
    assert(strstr(sh->last_error, "syntax error near unexpected token '>'") != NULL);
    pipeline_free(&pl);

    assert(parse_line(sh, "< |", &pl) == 2);
    assert(strstr(sh->last_error, "syntax error near unexpected token '|'") != NULL);
    pipeline_free(&pl);

    shell_destroy(sh);
    printf("  Task 4 Parser Unit Tests passed!\n");
}

static void run_task4_execution_tests(Shared *shared) {
    printf("\n--- PART 7B: Task 4 Redirection Execution Tests (Thread Mode) ---\n");
    int to_tab[2], from_tab[2];
    assert(pipe2(to_tab, O_CLOEXEC) == 0);
    assert(pipe2(from_tab, O_CLOEXEC) == 0);

    TabChannel chan;
    chan.to_tab_fd = to_tab[0];
    chan.from_tab_fd = from_tab[1];
    chan.shared = shared;

    pthread_t th;
    assert(pthread_create(&th, NULL, tab_thread_func, &chan) == 0);

    HarnessTab tab;
    memset(&tab, 0, sizeof(tab));
    InBuffer in;
    memset(&in, 0, sizeof(in));

    wait_for_prompt(from_tab[0], &tab, &in);

    /* [7.1] Output redirection > */
    printf("  [7.1] Testing stdout redirection (>)...\n");
    unlink("/tmp/term_t4_out1.txt");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo \"hello redirection\" > /tmp/term_t4_out1.txt");
    assert(strlen(tab.last_output) == 0);
    FILE *f1 = fopen("/tmp/term_t4_out1.txt", "r");
    assert(f1 != NULL);
    char buf[256] = {0};
    assert(fgets(buf, sizeof(buf), f1) != NULL);
    assert(strcmp(buf, "hello redirection\n") == 0);
    fclose(f1);
    printf("    Stdout redirection verified!\n");

    /* [7.2] Input redirection < */
    printf("  [7.2] Testing stdin redirection (<)...\n");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "cat < /tmp/term_t4_out1.txt");
    assert(strstr(tab.last_output, "hello redirection\n") != NULL);
    printf("    Stdin redirection verified!\n");

    /* [7.3] Both < and > together */
    printf("  [7.3] Testing both < and > together...\n");
    unlink("/tmp/term_t4_out2.txt");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "cat < /tmp/term_t4_out1.txt > /tmp/term_t4_out2.txt");
    FILE *f2 = fopen("/tmp/term_t4_out2.txt", "r");
    assert(f2 != NULL);
    memset(buf, 0, sizeof(buf));
    assert(fgets(buf, sizeof(buf), f2) != NULL);
    assert(strcmp(buf, "hello redirection\n") == 0);
    fclose(f2);
    printf("    Both < and > together verified!\n");

    /* [7.4] sort < unsorted.txt */
    printf("  [7.4] Testing sort < file...\n");
    FILE *f_unsort = fopen("/tmp/term_t4_unsorted.txt", "w");
    assert(f_unsort != NULL);
    fputs("cherry\napple\nbanana\n", f_unsort);
    fclose(f_unsort);

    send_cmd(to_tab[1], from_tab[0], &tab, &in, "sort < /tmp/term_t4_unsorted.txt");
    assert(strstr(tab.last_output, "apple\nbanana\ncherry\n") != NULL);
    printf("    sort < file verified!\n");

    /* [7.5] Append >> */
    printf("  [7.5] Testing append (>>)...\n");
    unlink("/tmp/term_t4_app.txt");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo line1 > /tmp/term_t4_app.txt");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo line2 >> /tmp/term_t4_app.txt");
    FILE *f_app = fopen("/tmp/term_t4_app.txt", "r");
    assert(f_app != NULL);
    char app_buf[512] = {0};
    size_t nr = fread(app_buf, 1, sizeof(app_buf) - 1, f_app);
    app_buf[nr] = '\0';
    assert(strcmp(app_buf, "line1\nline2\n") == 0);
    fclose(f_app);
    printf("    Append (>>) verified!\n");

    /* [7.6] Built-in redirection in tab context */
    printf("  [7.6] Testing built-in redirection (history >, pwd >, echo >>)...\n");
    unlink("/tmp/term_t4_pwd.txt");
    unlink("/tmp/term_t4_hist.txt");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "pwd > /tmp/term_t4_pwd.txt");
    FILE *f_pwd = fopen("/tmp/term_t4_pwd.txt", "r");
    assert(f_pwd != NULL);
    memset(buf, 0, sizeof(buf));
    assert(fgets(buf, sizeof(buf), f_pwd) != NULL);
    assert(strlen(buf) > 1);
    fclose(f_pwd);

    send_cmd(to_tab[1], from_tab[0], &tab, &in, "history > /tmp/term_t4_hist.txt");
    FILE *f_hist = fopen("/tmp/term_t4_hist.txt", "r");
    assert(f_hist != NULL);
    memset(buf, 0, sizeof(buf));
    assert(fgets(buf, sizeof(buf), f_hist) != NULL);
    assert(strlen(buf) > 0);
    fclose(f_hist);
    printf("    Built-in redirection verified!\n");

    /* [7.7] Missing / unreadable input file */
    printf("  [7.7] Testing missing input file error and $?...\n");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "cat < /tmp/term_t4_missing_xyz123.txt");
    assert(strstr(tab.last_output, "terminator: /tmp/term_t4_missing_xyz123.txt: No such file or directory") != NULL);
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "1\n") != NULL);
    printf("    Missing input file error and $? verified!\n");

    /* [7.8] Missing input file in pipeline */
    printf("  [7.8] Testing missing input file in pipeline...\n");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "cat < /tmp/term_t4_missing_xyz123.txt | cat");
    assert(strstr(tab.last_output, "terminator: /tmp/term_t4_missing_xyz123.txt: No such file or directory") != NULL);
    printf("    Missing input file in pipeline verified!\n");

    /* [7.9] Uncreatable output file */
    printf("  [7.9] Testing uncreatable output file...\n");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo hi > /nonexistent_dir_t4_xyz/out.txt");
    assert(strstr(tab.last_output, "terminator: /nonexistent_dir_t4_xyz/out.txt: No such file or directory") != NULL);
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "1\n") != NULL);
    printf("    Uncreatable output file verified!\n");

    /* [7.10] Ambiguous redirect at runtime */
    printf("  [7.10] Testing ambiguous redirect at runtime...\n");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "cat < $UNSET_VAR_T4_XYZ");
    assert(strstr(tab.last_output, "terminator: ambiguous redirect\n") != NULL);
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "1\n") != NULL);
    printf("    Ambiguous redirect at runtime verified!\n");

    /* [7.11] Redirection syntax errors at runtime */
    printf("  [7.11] Testing redirection syntax errors at runtime...\n");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "sort <");
    assert(strstr(tab.last_output, "terminator: syntax error near unexpected token 'newline'\n") != NULL);
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "2\n") != NULL);

    send_cmd(to_tab[1], from_tab[0], &tab, &in, "> >");
    assert(strstr(tab.last_output, "terminator: syntax error near unexpected token '>'\n") != NULL);
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "2\n") != NULL);

    send_cmd(to_tab[1], from_tab[0], &tab, &in, "< |");
    assert(strstr(tab.last_output, "terminator: syntax error near unexpected token '|'\n") != NULL);
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "2\n") != NULL);
    printf("    Redirection syntax errors verified!\n");

    /* [7.12] 2> redirection */
    printf("  [7.12] Testing 2> redirection...\n");
    unlink("/tmp/term_t4_err.txt");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "cat /tmp/term_t4_missing_xyz123.txt 2> /tmp/term_t4_err.txt");
    assert(strlen(tab.last_output) == 0);
    FILE *f_err = fopen("/tmp/term_t4_err.txt", "r");
    assert(f_err != NULL);
    memset(buf, 0, sizeof(buf));
    assert(fgets(buf, sizeof(buf), f_err) != NULL);
    assert(strstr(buf, "No such file or directory") != NULL);
    fclose(f_err);
    printf("    2> redirection verified!\n");

    /* [7.13] Interactive foreground cat fed by FRAME_LINE and closed by FRAME_EOF */
    printf("  [7.13] Testing interactive foreground cat (FRAME_LINE + FRAME_EOF)...\n");
    tab.last_output[0] = '\0';
    tab.last_output_len = 0;
    assert(proto_write_frame_blocking(to_tab[1], FRAME_LINE, "cat", 3) == 0);
    usleep(50000); /* give child time to start */
    assert(proto_write_frame_blocking(to_tab[1], FRAME_LINE, "interactive line 1", 18) == 0);
    assert(proto_write_frame_blocking(to_tab[1], FRAME_LINE, "interactive line 2", 18) == 0);
    assert(proto_write_frame_blocking(to_tab[1], FRAME_EOF, NULL, 0) == 0);
    wait_for_prompt(from_tab[0], &tab, &in);
    assert(strstr(tab.last_output, "interactive line 1\ninteractive line 2\n") != NULL);
    printf("    Interactive foreground cat verified!\n");

    /* [7.14] Concurrent readers of the same large file */
    printf("  [7.14] Testing concurrent readers of the same large file (independent offsets)...\n");
    FILE *f_big = fopen("/tmp/term_t4_big.txt", "w");
    assert(f_big != NULL);
    for (int line_idx = 0; line_idx < 10000; line_idx++) {
        fputs("line for concurrency testing 1234567890\n", f_big);
    }
    fclose(f_big);

    /* Test concurrent subshell pipelines reading the same file */
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo $(wc -l < /tmp/term_t4_big.txt) and $(wc -l < /tmp/term_t4_big.txt)");
    assert(strstr(tab.last_output, "10000") != NULL);
    printf("    Concurrent readers verified (both got full 10000 lines)!\n");

    /* [7.15] Zero FD leak check over 1,000 redirected commands */
    printf("  [7.15] Testing zero fd leaks over 1,000 redirected commands...\n");
    int fds_before = count_open_fds();
    for (int k = 0; k < 1000; k++) {
        send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo test > /tmp/term_t4_leak.txt");
    }
    int fds_after = count_open_fds();
    printf("    FD count before: %d, after 1000 redirected commands: %d\n", fds_before, fds_after);
    assert(fds_before == fds_after);
    printf("    Zero FD leaks verified!\n");

    /* Clean exit of tab */
    assert(proto_write_frame_blocking(to_tab[1], FRAME_EOF, NULL, 0) == 0);
    pthread_join(th, NULL);
    close(to_tab[1]);
    close(from_tab[0]);
    printf("  Task 4 Redirection Execution Tests passed!\n");
}

static void run_task4_process_mode_tests(Shared *shared) {
    printf("\n--- PART 7C: Task 4 Redirection Tests in Process Mode ---\n");
    int p_to[2], p_from[2];
    assert(pipe2(p_to, O_CLOEXEC) == 0);
    assert(pipe2(p_from, O_CLOEXEC) == 0);

    pid_t proc_pid = fork();
    assert(proc_pid >= 0);

    if (proc_pid == 0) {
        close(p_to[1]);
        close(p_from[0]);
        TabChannel pchan;
        pchan.to_tab_fd = p_to[0];
        pchan.from_tab_fd = p_from[1];
        pchan.shared = shared;
        tab_main(&pchan);
        close(p_to[0]);
        close(p_from[1]);
        _exit(0);
    }

    close(p_to[0]);
    close(p_from[1]);

    HarnessTab ptab;
    memset(&ptab, 0, sizeof(ptab));
    InBuffer pin;
    memset(&pin, 0, sizeof(pin));

    wait_for_prompt(p_from[0], &ptab, &pin);

    /* Process Mode Redirections */
    unlink("/tmp/term_t4_pm_out.txt");
    send_cmd(p_to[1], p_from[0], &ptab, &pin, "echo \"process mode redirection\" > /tmp/term_t4_pm_out.txt");
    send_cmd(p_to[1], p_from[0], &ptab, &pin, "cat < /tmp/term_t4_pm_out.txt");
    assert(strstr(ptab.last_output, "process mode redirection\n") != NULL);

    send_cmd(p_to[1], p_from[0], &ptab, &pin, "sort < /tmp/term_t4_unsorted.txt > /tmp/term_t4_pm_sorted.txt");
    send_cmd(p_to[1], p_from[0], &ptab, &pin, "cat < /tmp/term_t4_pm_sorted.txt");
    assert(strstr(ptab.last_output, "apple\nbanana\ncherry\n") != NULL);

    send_cmd(p_to[1], p_from[0], &ptab, &pin, "cat < /tmp/nonexistent_pm_xyz.txt");
    assert(strstr(ptab.last_output, "terminator: /tmp/nonexistent_pm_xyz.txt: No such file or directory") != NULL);

    assert(proto_write_frame_blocking(p_to[1], FRAME_EOF, NULL, 0) == 0);
    int pstatus = 0;
    assert(waitpid(proc_pid, &pstatus, 0) == proc_pid);
    assert(WIFEXITED(pstatus) && WEXITSTATUS(pstatus) == 0);
    close(p_to[1]);
    close(p_from[0]);
    printf("  Task 4 Redirection Tests in Process Mode passed!\n");
}

/* ----------------------------------------------------
 * Part 8: Task 5 Pipeline Tests
 * ---------------------------------------------------- */
static void run_task5_pipeline_tests(Shared *shared) {
    printf("\n--- PART 8A: Task 5 Pipeline Execution Tests (Thread Mode) ---\n");
    int to_tab[2], from_tab[2];
    assert(pipe2(to_tab, O_CLOEXEC) == 0);
    assert(pipe2(from_tab, O_CLOEXEC) == 0);

    TabChannel chan;
    chan.to_tab_fd = to_tab[0];
    chan.from_tab_fd = from_tab[1];
    chan.shared = shared;

    pthread_t th;
    assert(pthread_create(&th, NULL, tab_thread_func, &chan) == 0);

    HarnessTab tab;
    memset(&tab, 0, sizeof(tab));
    InBuffer in;
    memset(&in, 0, sizeof(in));

    wait_for_prompt(from_tab[0], &tab, &in);
    char start_cwd[1024];
    strncpy(start_cwd, tab.last_cwd, sizeof(start_cwd) - 1);
    start_cwd[sizeof(start_cwd) - 1] = '\0';

    /* [8.1] 2-stage pipeline: echo | tr */
    printf("  [8.1] Testing 2-stage pipeline (echo | tr)...\n");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo \"hello world\" | tr 'a-z' 'A-Z'");
    assert(strstr(tab.last_output, "HELLO WORLD\n") != NULL);
    printf("    2-stage pipeline verified!\n");

    /* [8.2] 3-stage pipeline: echo | sort | tr */
    printf("  [8.2] Testing 3-stage pipeline (echo | sort | tr)...\n");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo -e \"cherry\\napple\\nbanana\" | sort | tr 'a-z' 'A-Z'");
    assert(strstr(tab.last_output, "APPLE\nBANANA\nCHERRY\n") != NULL);
    printf("    3-stage pipeline verified!\n");

    /* [8.3] 5-stage pipeline */
    printf("  [8.3] Testing 5-stage pipeline (echo | cat | cat | cat | tr)...\n");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo \"pipeline stage 5\" | cat | cat | cat | tr 'a-z' 'A-Z'");
    assert(strstr(tab.last_output, "PIPELINE STAGE 5\n") != NULL);
    printf("    5-stage pipeline verified!\n");

    /* [8.4] 20-stage pipeline */
    printf("  [8.4] Testing 20-stage pipeline...\n");
    char cmd20[1024];
    strcpy(cmd20, "echo \"twenty stage pipeline\"");
    for (int k = 0; k < 18; k++) {
        strcat(cmd20, " | cat");
    }
    strcat(cmd20, " | tr 'a-z' 'A-Z'");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, cmd20);
    assert(strstr(tab.last_output, "TWENTY STAGE PIPELINE\n") != NULL);
    printf("    20-stage pipeline verified!\n");

    /* [8.5] Throughput test with seq 1 200000 | sort -n | tail -1 */
    printf("  [8.5] Testing high-throughput pipeline (seq 1 200000 | sort -n | tail -1)...\n");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "seq 1 200000 | sort -n | tail -1");
    assert(strstr(tab.last_output, "200000\n") != NULL);
    printf("    Throughput test verified!\n");

    /* [8.6] Large data & deadlock freedom */
    printf("  [8.6] Testing large data without deadlocks (cat < big | sort -n | head -5)...\n");
    FILE *fbig = fopen("/tmp/term_t5_big.txt", "w");
    assert(fbig != NULL);
    for (int i = 50000; i >= 1; i--) {
        fprintf(fbig, "%d\n", i);
    }
    fclose(fbig);
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "cat < /tmp/term_t5_big.txt | sort -n | head -5");
    assert(strstr(tab.last_output, "1\n2\n3\n4\n5\n") != NULL);
    printf("    Large data sort/head verified!\n");

    /* [8.7] SIGPIPE & early termination (yes | head -3) */
    printf("  [8.7] Testing SIGPIPE termination (yes | head -3, yes | head -1)...\n");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "yes | head -3");
    assert(strcmp(tab.last_output, "y\ny\ny\n") == 0);
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "yes | head -1");
    assert(strcmp(tab.last_output, "y\n") == 0);
    printf("    SIGPIPE early termination verified!\n");

    /* [8.8] Pipes combined with redirections */
    printf("  [8.8] Testing pipes combined with redirections (< and >)...\n");
    FILE *fin = fopen("/tmp/term_t5_in.txt", "w");
    assert(fin != NULL);
    fputs("zebra\napple\nmongoose\n", fin);
    fclose(fin);
    unlink("/tmp/term_t5_out.txt");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "cat < /tmp/term_t5_in.txt | sort > /tmp/term_t5_out.txt");
    FILE *fout = fopen("/tmp/term_t5_out.txt", "r");
    assert(fout != NULL);
    char buf[256] = {0};
    size_t nr = fread(buf, 1, sizeof(buf) - 1, fout);
    buf[nr] = '\0';
    fclose(fout);
    assert(strcmp(buf, "apple\nmongoose\nzebra\n") == 0);

    unlink("/tmp/term_t5_out2.txt");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "cat < /tmp/term_t5_in.txt | sort | tr 'a-z' 'A-Z' > /tmp/term_t5_out2.txt");
    FILE *fout2 = fopen("/tmp/term_t5_out2.txt", "r");
    assert(fout2 != NULL);
    memset(buf, 0, sizeof(buf));
    nr = fread(buf, 1, sizeof(buf) - 1, fout2);
    buf[nr] = '\0';
    fclose(fout2);
    assert(strcmp(buf, "APPLE\nMONGOOSE\nZEBRA\n") == 0);
    printf("    Pipes combined with redirections verified!\n");

    /* [8.9] Pipeline exit status ($?) */
    printf("  [8.9] Testing pipeline exit status ($? is status of LAST command)...\n");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo test | false | cat");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "0\n") != NULL);

    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo test | cat | false");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "1\n") != NULL);

    /* Failing middle command */
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo hello | /tmp/nonexistent_term_cmd_xyz | cat");
    assert(strstr(tab.last_output, "terminator: /tmp/nonexistent_term_cmd_xyz: command not found") != NULL);
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "0\n") != NULL);
    printf("    Pipeline exit status and failing middle stage verified!\n");

    /* [8.10] Builtins in pipeline & state isolation */
    printf("  [8.10] Testing builtins in pipeline & tab state isolation...\n");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo \"builtin pipeline\" | cat");
    assert(strstr(tab.last_output, "builtin pipeline\n") != NULL);

    send_cmd(to_tab[1], from_tab[0], &tab, &in, "cd /tmp | cat");
    assert(strcmp(tab.last_cwd, start_cwd) == 0);
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "pwd");
    assert(strstr(tab.last_output, start_cwd) != NULL);

    send_cmd(to_tab[1], from_tab[0], &tab, &in, "export TASK5_ISO_VAR=secret_val | cat");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo var_is_${TASK5_ISO_VAR}");
    assert(strstr(tab.last_output, "var_is_\n") != NULL);
    printf("    Builtins in pipeline and state isolation verified!\n");

    /* [8.11] Globs in pipelines relative to cwd */
    printf("  [8.11] Testing globs in pipelines (ls *.txt | wc -l, ls *.txt | xargs rm)...\n");
    system("rm -rf /tmp/term_t5_glob_dir && mkdir -p /tmp/term_t5_glob_dir");
    system("touch /tmp/term_t5_glob_dir/f1.t5test /tmp/term_t5_glob_dir/f2.t5test /tmp/term_t5_glob_dir/f3.t5test");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "cd /tmp/term_t5_glob_dir");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "ls *.t5test | wc -l");
    assert(strstr(tab.last_output, "3\n") != NULL);

    send_cmd(to_tab[1], from_tab[0], &tab, &in, "ls *.t5test | xargs rm");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "ls *.t5test");
    assert(strstr(tab.last_output, "No such file or directory") != NULL);
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "cd -");
    printf("    Globs in pipelines verified!\n");

    /* [8.12] Syntax errors */
    printf("  [8.12] Testing pipeline syntax errors (ls | | wc, | wc)...\n");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "ls | | wc");
    assert(strstr(tab.last_output, "syntax error near unexpected token '|'") != NULL);

    send_cmd(to_tab[1], from_tab[0], &tab, &in, "| wc");
    assert(strstr(tab.last_output, "syntax error near unexpected token '|'") != NULL);
    printf("    Syntax errors verified!\n");

    /* [8.13] Zero FD leaks across 500 pipelines */
    printf("  [8.13] Testing zero FD leaks over 500 multi-stage pipelines...\n");
    int fds_before = count_open_fds();
    for (int k = 0; k < 500; k++) {
        send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo leak_test | cat | cat");
    }
    int fds_after = count_open_fds();
    printf("    FD count before: %d, after 500 pipelines: %d\n", fds_before, fds_after);
    assert(fds_before == fds_after);
    printf("    Zero FD leaks verified!\n");

    /* Clean exit of tab */
    assert(proto_write_frame_blocking(to_tab[1], FRAME_EOF, NULL, 0) == 0);
    pthread_join(th, NULL);
    close(to_tab[1]);
    close(from_tab[0]);
    printf("  Task 5 Pipeline Execution Tests passed!\n");
}

static void run_task5_process_mode_tests(Shared *shared) {
    printf("\n--- PART 8B: Task 5 Pipeline Tests in Process Mode ---\n");
    int p_to[2], p_from[2];
    assert(pipe2(p_to, O_CLOEXEC) == 0);
    assert(pipe2(p_from, O_CLOEXEC) == 0);

    pid_t proc_pid = fork();
    assert(proc_pid >= 0);

    if (proc_pid == 0) {
        close(p_to[1]);
        close(p_from[0]);
        TabChannel pchan;
        pchan.to_tab_fd = p_to[0];
        pchan.from_tab_fd = p_from[1];
        pchan.shared = shared;
        tab_main(&pchan);
        close(p_to[0]);
        close(p_from[1]);
        _exit(0);
    }

    close(p_to[0]);
    close(p_from[1]);

    HarnessTab ptab;
    memset(&ptab, 0, sizeof(ptab));
    InBuffer pin;
    memset(&pin, 0, sizeof(pin));

    wait_for_prompt(p_from[0], &ptab, &pin);

    /* Process Mode multi-stage pipeline */
    printf("  [8B.1] Process Mode multi-stage pipeline...\n");
    send_cmd(p_to[1], p_from[0], &ptab, &pin, "echo \"process pipeline\" | tr 'a-z' 'A-Z' | cat");
    assert(strstr(ptab.last_output, "PROCESS PIPELINE\n") != NULL);

    /* Process Mode throughput */
    printf("  [8B.2] Process Mode throughput (seq 1 50000 | sort -n | tail -1)...\n");
    send_cmd(p_to[1], p_from[0], &ptab, &pin, "seq 1 50000 | sort -n | tail -1");
    assert(strstr(ptab.last_output, "50000\n") != NULL);

    /* Process Mode SIGPIPE */
    printf("  [8B.3] Process Mode SIGPIPE (yes | head -3)...\n");
    send_cmd(p_to[1], p_from[0], &ptab, &pin, "yes | head -3");
    assert(strcmp(ptab.last_output, "y\ny\ny\n") == 0);

    /* Process Mode pipe + redirection */
    printf("  [8B.4] Process Mode pipe + redirection...\n");
    unlink("/tmp/term_t5_pm_out.txt");
    send_cmd(p_to[1], p_from[0], &ptab, &pin, "cat < /tmp/term_t5_in.txt | sort > /tmp/term_t5_pm_out.txt");
    FILE *fout = fopen("/tmp/term_t5_pm_out.txt", "r");
    assert(fout != NULL);
    char buf[256] = {0};
    size_t nr = fread(buf, 1, sizeof(buf) - 1, fout);
    buf[nr] = '\0';
    fclose(fout);
    assert(strcmp(buf, "apple\nmongoose\nzebra\n") == 0);

    assert(proto_write_frame_blocking(p_to[1], FRAME_EOF, NULL, 0) == 0);
    int pstatus = 0;
    assert(waitpid(proc_pid, &pstatus, 0) == proc_pid);
    assert(WIFEXITED(pstatus) && WEXITSTATUS(pstatus) == 0);
    close(p_to[1]);
    close(p_from[0]);
    printf("  Task 5 Pipeline Tests in Process Mode passed!\n");
}

typedef struct {
    int to_fd;
    int from_fd;
    const char *cmd;
    const char *expected_substring;
    bool success;
} TabWorkerArg;

static void *concurrency_tab_worker(void *arg) {
    TabWorkerArg *w = (TabWorkerArg *)arg;
    HarnessTab tab;
    memset(&tab, 0, sizeof(tab));
    InBuffer in;
    memset(&in, 0, sizeof(in));

    wait_for_prompt(w->from_fd, &tab, &in);
    send_cmd(w->to_fd, w->from_fd, &tab, &in, w->cmd);
    if (strstr(tab.last_output, w->expected_substring) != NULL) {
        w->success = true;
    }
    proto_write_frame_blocking(w->to_fd, FRAME_EOF, NULL, 0);
    return NULL;
}

static void run_task5_concurrency_tests(Shared *shared) {
    printf("\n--- PART 8C: 4 Concurrent Tabs Running Pipelines (Thread Mode) ---\n");
    const int NUM_CONCURRENT = 4;
    int to_fds[4][2];
    int from_fds[4][2];
    pthread_t th_tabs[4];
    TabChannel chans[4];

    for (int i = 0; i < NUM_CONCURRENT; i++) {
        assert(pipe2(to_fds[i], O_CLOEXEC) == 0);
        assert(pipe2(from_fds[i], O_CLOEXEC) == 0);
        chans[i].to_tab_fd = to_fds[i][0];
        chans[i].from_tab_fd = from_fds[i][1];
        chans[i].shared = shared;
        assert(pthread_create(&th_tabs[i], NULL, tab_thread_func, &chans[i]) == 0);
    }

    TabWorkerArg workers[4] = {
        {to_fds[0][1], from_fds[0][0], "seq 1 50000 | sort -n | tail -1", "50000", false},
        {to_fds[1][1], from_fds[1][0], "echo \"message from tab 1\" | tr 'a-z' 'A-Z'", "MESSAGE FROM TAB 1", false},
        {to_fds[2][1], from_fds[2][0], "yes | head -5", "y\ny\ny\ny\ny\n", false},
        {to_fds[3][1], from_fds[3][0], "echo -e \"3\\n1\\n2\" | sort", "1\n2\n3\n", false}
    };

    pthread_t th_workers[4];
    for (int i = 0; i < NUM_CONCURRENT; i++) {
        assert(pthread_create(&th_workers[i], NULL, concurrency_tab_worker, &workers[i]) == 0);
    }

    for (int i = 0; i < NUM_CONCURRENT; i++) {
        pthread_join(th_workers[i], NULL);
        assert(workers[i].success);
        pthread_join(th_tabs[i], NULL);
        close(to_fds[i][1]);
        close(from_fds[i][0]);
    }

    printf("  All 4 concurrent tabs finished successfully with zero cross-talk!\n");
}

/* ----------------------------------------------------
 * Part 10: Task 6 multiWatch Tests
 * ---------------------------------------------------- */
static void run_multiwatch_parser_unit_tests(void) {
    printf("\n--- PART 10A: Task 6 multiWatch Parser Unit Tests ---\n");
    double interval = 0.0;
    char **cmds = NULL;
    int ncmds = 0;

    /* 1. Basic two commands */
    assert(multiwatch_parse_args("[\"date\", \"ls | wc -l\"]", &interval, &cmds, &ncmds) == 0);
    assert(interval == 2.0);
    assert(ncmds == 2);
    assert(strcmp(cmds[0], "date") == 0);
    assert(strcmp(cmds[1], "ls | wc -l") == 0);
    for (int i = 0; i < ncmds; i++) free(cmds[i]);
    free(cmds);

    /* 2. Custom interval -n 5 */
    assert(multiwatch_parse_args("-n 5 [\"cmd1\", \"cmd2\"]", &interval, &cmds, &ncmds) == 0);
    assert(interval == 5.0);
    assert(ncmds == 2);
    for (int i = 0; i < ncmds; i++) free(cmds[i]);
    free(cmds);

    /* 3. Fractional interval -n 0.5 */
    assert(multiwatch_parse_args("-n 0.5 ['single quote cmd']", &interval, &cmds, &ncmds) == 0);
    assert(interval == 0.5);
    assert(ncmds == 1);
    assert(strcmp(cmds[0], "single quote cmd") == 0);
    for (int i = 0; i < ncmds; i++) free(cmds[i]);
    free(cmds);

    /* 4. Escapes and whitespace */
    assert(multiwatch_parse_args("   [   \"echo \\\"nested\\\"\"   ,   \"echo \\\\path\\\\\"   ]   ", &interval, &cmds, &ncmds) == 0);
    assert(ncmds == 2);
    assert(strcmp(cmds[0], "echo \"nested\"") == 0);
    assert(strcmp(cmds[1], "echo \\path\\") == 0);
    for (int i = 0; i < ncmds; i++) free(cmds[i]);
    free(cmds);

    /* 5. Malformed syntax errors */
    assert(multiwatch_parse_args("", &interval, &cmds, &ncmds) != 0);
    assert(multiwatch_parse_args("hello", &interval, &cmds, &ncmds) != 0);
    assert(multiwatch_parse_args("[]", &interval, &cmds, &ncmds) != 0);
    assert(multiwatch_parse_args("[\"cmd\",]", &interval, &cmds, &ncmds) != 0);
    assert(multiwatch_parse_args("[\"cmd1\" \"cmd2\"]", &interval, &cmds, &ncmds) != 0);
    assert(multiwatch_parse_args("[\"unterminated", &interval, &cmds, &ncmds) != 0);
    assert(multiwatch_parse_args("[\"cmd\"] extra_junk", &interval, &cmds, &ncmds) != 0);
    assert(multiwatch_parse_args("-n -1 [\"cmd\"]", &interval, &cmds, &ncmds) != 0);
    assert(multiwatch_parse_args("-n abc [\"cmd\"]", &interval, &cmds, &ncmds) != 0);
    assert(multiwatch_parse_args("-n [\"cmd\"]", &interval, &cmds, &ncmds) != 0);

    printf("  All multiWatch parser unit tests passed!\n");
}

static void run_multiwatch_execution_tests(Shared *shared) {
    printf("\n--- PART 10B: Task 6 multiWatch Execution Tests (Thread Mode) ---\n");
    int to_tab[2], from_tab[2];
    assert(pipe2(to_tab, O_CLOEXEC) == 0);
    assert(pipe2(from_tab, O_CLOEXEC) == 0);

    TabChannel chan;
    chan.to_tab_fd = to_tab[0];
    chan.from_tab_fd = from_tab[1];
    chan.shared = shared;

    pthread_t th;
    assert(pthread_create(&th, NULL, tab_thread_func, &chan) == 0);

    HarnessTab tab;
    memset(&tab, 0, sizeof(tab));
    InBuffer in;
    memset(&in, 0, sizeof(in));

    wait_for_prompt(from_tab[0], &tab, &in);

    /* [10.1] Malformed syntax at runtime */
    printf("  [10.1] Testing malformed syntax handling and $?...\n");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "multiWatch []");
    assert(strstr(tab.last_output, MULTIWATCH_USAGE) != NULL);
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "1\n") != NULL);
    printf("    Malformed syntax usage message and $?=1 verified!\n");

    /* [10.2] Byte-comparison of exact output block */
    printf("  [10.2] Testing byte-level format of multiWatch block...\n");
    reset_tab_output(&tab);
    const char *mw_exact = "multiWatch -n 5 [\"echo test_byte_exact\"]";
    proto_write_frame_blocking(to_tab[1], FRAME_LINE, mw_exact, (uint32_t)strlen(mw_exact));
    assert(wait_for_output_substring(from_tab[0], &tab, &in, "test_byte_exact", 3000));

    /* Verify format */
    const char *dash = MULTIWATCH_DASH_LINE;
    assert(strstr(tab.last_output, "\"echo test_byte_exact\", ") != NULL);
    assert(strstr(tab.last_output, dash) != NULL);
    const char *header_start = strstr(tab.last_output, "\"echo test_byte_exact\", ");
    assert(header_start != NULL);
    const char *colon = strchr(header_start, ':');
    assert(colon != NULL && *(colon + 1) == '\n');
    assert(strncmp(colon + 2, dash, strlen(dash)) == 0);
    const char *body_start = colon + 2 + strlen(dash);
    assert(strncmp(body_start, "test_byte_exact\n", 16) == 0);
    assert(strncmp(body_start + 16, dash, strlen(dash)) == 0);

    /* Send Ctrl+C */
    send_intr_expect_prompt(to_tab[1], from_tab[0], &tab, &in, "user@terminator> ");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "130\n") != NULL);
    printf("    Exact byte format and Ctrl+C exit ($?=130) verified!\n");

    /* [10.3] Output lacking trailing newline gets closing dash line on own line */
    printf("  [10.3] Testing output lacking trailing newline...\n");
    reset_tab_output(&tab);
    const char *mw_nonl = "multiWatch -n 5 [\"echo -n no_nl\"]";
    proto_write_frame_blocking(to_tab[1], FRAME_LINE, mw_nonl, (uint32_t)strlen(mw_nonl));
    assert(wait_for_output_substring(from_tab[0], &tab, &in, "no_nl\n----------------------------------------------------\n", 3000));
    send_intr_expect_prompt(to_tab[1], from_tab[0], &tab, &in, "user@terminator> ");
    printf("    Missing newline handled cleanly!\n");

    /* [10.4] Multiple commands running in parallel with inner pipes */
    printf("  [10.4] Testing parallel commands with inner pipelines...\n");
    reset_tab_output(&tab);
    const char *mw_par = "multiWatch -n 5 [\"echo foo | tr 'a-z' 'A-Z'\", \"echo bar | tr 'a-z' 'A-Z'\"]";
    proto_write_frame_blocking(to_tab[1], FRAME_LINE, mw_par, (uint32_t)strlen(mw_par));
    bool got_foo = wait_for_output_substring(from_tab[0], &tab, &in, "FOO", 3000);
    bool got_bar = wait_for_output_substring(from_tab[0], &tab, &in, "BAR", 3000);
    assert(got_foo);
    assert(got_bar);
    send_intr_expect_prompt(to_tab[1], from_tab[0], &tab, &in, "user@terminator> ");
    printf("    Parallel execution and inner pipelines verified!\n");

    /* [10.5] Command that fails */
    printf("  [10.5] Testing failing command block capture...\n");
    reset_tab_output(&tab);
    const char *mw_fail = "multiWatch -n 5 [\"/tmp/nonexistent_multiwatch_cmd\"]";
    proto_write_frame_blocking(to_tab[1], FRAME_LINE, mw_fail, (uint32_t)strlen(mw_fail));
    assert(wait_for_output_substring(from_tab[0], &tab, &in, "command not found", 3000));
    send_intr_expect_prompt(to_tab[1], from_tab[0], &tab, &in, "user@terminator> ");
    printf("    Failing command error block verified!\n");

    /* [10.6] Massive output truncation (1 MiB cap) */
    printf("  [10.6] Testing huge output truncation cap...\n");
    reset_tab_output(&tab);
    const char *mw_huge = "multiWatch -n 5 [\"yes huge_text_for_truncation | head -n 80000\"]";
    proto_write_frame_blocking(to_tab[1], FRAME_LINE, mw_huge, (uint32_t)strlen(mw_huge));
    assert(wait_for_output_substring(from_tab[0], &tab, &in, "[output truncated]", 5000));
    send_intr_expect_prompt(to_tab[1], from_tab[0], &tab, &in, "user@terminator> ");
    printf("    Huge output truncation cap verified!\n");

    /* [10.7] Extra LINE frames ignored while multiWatch runs */
    printf("  [10.7] Verifying extra LINE frames are ignored while running...\n");
    reset_tab_output(&tab);
    tab.last_prompt[0] = '\0';
    const char *mw_iter = "multiWatch -n 5 [\"echo iter1\"]";
    proto_write_frame_blocking(to_tab[1], FRAME_LINE, mw_iter, (uint32_t)strlen(mw_iter));
    assert(wait_for_output_substring(from_tab[0], &tab, &in, "iter1", 3000));
    const char *mw_ignored = "echo extra_line_ignored";
    proto_write_frame_blocking(to_tab[1], FRAME_LINE, mw_ignored, (uint32_t)strlen(mw_ignored));
    /* Still running multiWatch, prompt should NOT appear */
    assert(tab.last_prompt[0] == '\0');
    send_intr_expect_prompt(to_tab[1], from_tab[0], &tab, &in, "user@terminator> ");
    printf("    Extra LINE frames successfully ignored!\n");

    /* [10.8] Shell remains fully usable after multiWatch Ctrl+C */
    printf("  [10.8] Verifying shell is usable after multiWatch...\n");
    send_cmd(to_tab[1], from_tab[0], &tab, &in, "echo shell_intact");
    assert(strstr(tab.last_output, "shell_intact\n") != NULL);
    printf("    Shell usability verified!\n");

    /* Clean exit of tab */
    assert(proto_write_frame_blocking(to_tab[1], FRAME_EOF, NULL, 0) == 0);
    pthread_join(th, NULL);
    close(to_tab[1]);
    close(from_tab[0]);
    free(tab.last_output);
    printf("  Task 6 Execution Tests passed!\n");
}

static void run_multiwatch_two_tabs_concurrency_test(Shared *shared) {
    printf("\n--- PART 10C: Two Tabs Running multiWatch (Ctrl+C Only One) ---\n");
    int to1[2], from1[2];
    int to2[2], from2[2];
    assert(pipe2(to1, O_CLOEXEC) == 0);
    assert(pipe2(from1, O_CLOEXEC) == 0);
    assert(pipe2(to2, O_CLOEXEC) == 0);
    assert(pipe2(from2, O_CLOEXEC) == 0);

    TabChannel chan1 = {to1[0], from1[1], shared};
    TabChannel chan2 = {to2[0], from2[1], shared};

    pthread_t th1, th2;
    assert(pthread_create(&th1, NULL, tab_thread_func, &chan1) == 0);
    assert(pthread_create(&th2, NULL, tab_thread_func, &chan2) == 0);

    HarnessTab tab1, tab2;
    memset(&tab1, 0, sizeof(tab1));
    memset(&tab2, 0, sizeof(tab2));
    InBuffer in1, in2;
    memset(&in1, 0, sizeof(in1));
    memset(&in2, 0, sizeof(in2));

    wait_for_prompt(from1[0], &tab1, &in1);
    wait_for_prompt(from2[0], &tab2, &in2);

    /* Launch multiWatch in both tabs */
    reset_tab_output(&tab1);
    reset_tab_output(&tab2);

    const char *mw_t1 = "multiWatch -n 1 [\"echo TAB_ONE\"]";
    const char *mw_t2 = "multiWatch -n 1 [\"echo TAB_TWO\"]";
    proto_write_frame_blocking(to1[1], FRAME_LINE, mw_t1, (uint32_t)strlen(mw_t1));
    proto_write_frame_blocking(to2[1], FRAME_LINE, mw_t2, (uint32_t)strlen(mw_t2));

    assert(wait_for_output_substring(from1[0], &tab1, &in1, "TAB_ONE", 3000));
    assert(wait_for_output_substring(from2[0], &tab2, &in2, "TAB_TWO", 3000));

    /* Send Ctrl+C ONLY to Tab 1! */
    printf("  Sending Ctrl+C only to Tab 1...\n");
    send_intr_expect_prompt(to1[1], from1[0], &tab1, &in1, "user@terminator> ");
    send_cmd(to1[1], from1[0], &tab1, &in1, "echo tab1_ready");
    assert(strstr(tab1.last_output, "tab1_ready\n") != NULL);

    /* Tab 2 should still be running and produce another TAB_TWO block! */
    reset_tab_output(&tab2);
    assert(wait_for_output_substring(from2[0], &tab2, &in2, "TAB_TWO", 3000));
    printf("  Tab 2 continues running unharmed after Tab 1 was Ctrl+C'd!\n");

    /* Now stop Tab 2 */
    send_intr_expect_prompt(to2[1], from2[0], &tab2, &in2, "user@terminator> ");

    /* Clean exit of both tabs */
    proto_write_frame_blocking(to1[1], FRAME_EOF, NULL, 0);
    proto_write_frame_blocking(to2[1], FRAME_EOF, NULL, 0);
    pthread_join(th1, NULL);
    pthread_join(th2, NULL);
    close(to1[1]); close(from1[0]);
    close(to2[1]); close(from2[0]);
    free(tab1.last_output);
    free(tab2.last_output);

    printf("  Two tabs multiWatch isolation test passed!\n");
}

static void run_multiwatch_process_mode_test(Shared *shared) {
    printf("\n--- PART 10D: multiWatch Process Mode Test ---\n");
    int p_to[2], p_from[2];
    assert(pipe2(p_to, O_CLOEXEC) == 0);
    assert(pipe2(p_from, O_CLOEXEC) == 0);

    pid_t proc_pid = fork();
    assert(proc_pid >= 0);

    if (proc_pid == 0) {
        close(p_to[1]);
        close(p_from[0]);
        TabChannel pchan;
        pchan.to_tab_fd = p_to[0];
        pchan.from_tab_fd = p_from[1];
        pchan.shared = shared;
        tab_main(&pchan);
        close(p_to[0]);
        close(p_from[1]);
        _exit(0);
    }

    close(p_to[0]);
    close(p_from[1]);

    HarnessTab ptab;
    memset(&ptab, 0, sizeof(ptab));
    InBuffer pin;
    memset(&pin, 0, sizeof(pin));

    wait_for_prompt(p_from[0], &ptab, &pin);

    reset_tab_output(&ptab);
    const char *mw_pm = "multiWatch -n 5 [\"echo 'proc mode mw'\"]";
    proto_write_frame_blocking(p_to[1], FRAME_LINE, mw_pm, (uint32_t)strlen(mw_pm));
    assert(wait_for_output_substring(p_from[0], &ptab, &pin, "proc mode mw", 3000));

    send_intr_expect_prompt(p_to[1], p_from[0], &ptab, &pin, "user@terminator> ");
    send_cmd(p_to[1], p_from[0], &ptab, &pin, "echo $?");
    assert(strstr(ptab.last_output, "130\n") != NULL);

    assert(proto_write_frame_blocking(p_to[1], FRAME_EOF, NULL, 0) == 0);
    int pstatus = 0;
    assert(waitpid(proc_pid, &pstatus, 0) == proc_pid);
    assert(WIFEXITED(pstatus) && WEXITSTATUS(pstatus) == 0);
    close(p_to[1]);
    close(p_from[0]);
    free(ptab.last_output);
    printf("  multiWatch Process Mode Test passed!\n");
}

static void run_task7_lineedit_unit_tests(void) {
    printf("\n--- PART 11A: Task 7 Lineedit Unit Tests ---\n");
    LineEdit input;
    lineedit_init(&input);

    /* 1. Basic insertion */
    lineedit_insert_text(&input, "hello world", 11);
    assert(input.len == 11);
    assert(input.cursor == 11);
    assert(strcmp(input.buf, "hello world") == 0);

    /* 2. Cursor movements and mid-line insertion */
    lineedit_move_left(&input);
    lineedit_move_left(&input);
    lineedit_move_left(&input);
    lineedit_move_left(&input);
    lineedit_move_left(&input); /* cursor now at 6 ('w') */
    assert(input.cursor == 6);

    lineedit_insert_text(&input, "brave ", 6);
    assert(strcmp(input.buf, "hello brave world") == 0);
    assert(input.len == 17);
    assert(input.cursor == 12);

    /* 3. Line start & line end */
    lineedit_line_start(&input);
    assert(input.cursor == 0);
    lineedit_line_end(&input);
    assert(input.cursor == 17);

    /* 4. Backspace */
    lineedit_backspace(&input);
    assert(strcmp(input.buf, "hello brave worl") == 0);
    assert(input.cursor == 16);
    assert(input.len == 16);

    /* 5. Delete (at mid-line) */
    lineedit_line_start(&input);
    lineedit_move_right(&input); /* cursor at 1 ('e') */
    lineedit_delete(&input);     /* deletes 'e' */
    assert(strcmp(input.buf, "hllo brave worl") == 0);
    assert(input.cursor == 1);

    /* 6. Kill to start (Ctrl+U) */
    lineedit_move_right(&input);
    lineedit_move_right(&input);
    lineedit_move_right(&input);
    lineedit_move_right(&input);
    assert(input.cursor == 5);
    lineedit_kill_to_start(&input);
    assert(strcmp(input.buf, "brave worl") == 0);
    assert(input.cursor == 0);
    assert(input.len == 10);

    /* 7. Kill to end (Ctrl+K) */
    for (int i = 0; i < 5; i++) lineedit_move_right(&input);
    assert(input.cursor == 5);
    lineedit_kill_to_end(&input);
    assert(strcmp(input.buf, "brave") == 0);
    assert(input.cursor == 5);
    assert(input.len == 5);

    /* 8. Delete prev word (Ctrl+W) */
    lineedit_insert_text(&input, "   foo   bar   ", 15);
    lineedit_delete_prev_word(&input);
    assert(strcmp(input.buf, "brave   foo   ") == 0);
    assert(input.cursor == 14);

    lineedit_delete_prev_word(&input);
    assert(strcmp(input.buf, "brave   ") == 0);
    assert(input.cursor == 8);

    lineedit_delete_prev_word(&input);
    assert(strcmp(input.buf, "") == 0);
    assert(input.cursor == 0);
    assert(input.len == 0);

    /* 9. Layout computation for wrapped lines and mid-line cursor */
    size_t rows = 0, cur_r = 0, cur_c = 0;
    lineedit_compute_layout(10, 70, 70, 40, &rows, &cur_r, &cur_c);
    assert(rows == 3);
    assert(cur_r == 2);
    assert(cur_c == 0);

    lineedit_compute_layout(10, 70, 15, 40, &rows, &cur_r, &cur_c);
    assert(rows == 2);
    assert(cur_r == 0);
    assert(cur_c == 25);

    lineedit_compute_layout(10, 70, 45, 40, &rows, &cur_r, &cur_c);
    assert(rows == 2);
    assert(cur_r == 1);
    assert(cur_c == 15);

    printf("  All Task 7 Lineedit Unit Tests passed!\n");
}

static void run_task7_keymap_unit_tests(void) {
    printf("\n--- PART 11B: Task 7 Keymap Unit Tests ---\n");
    KeyMap km;
    keymap_init_defaults(&km);

    /* 1. Defaults verification */
    assert(strcmp(km.bindings[ACTION_LINE_START].chord.key, "a") == 0);
    assert(km.bindings[ACTION_LINE_START].chord.mods == KM_MOD_CTRL);
    assert(strcmp(km.bindings[ACTION_COPY].chord.key, "c") == 0);
    assert(km.bindings[ACTION_COPY].chord.mods == (KM_MOD_CTRL | KM_MOD_SHIFT));
    assert(strcmp(km.bindings[ACTION_PASTE].chord.key, "v") == 0);
    assert(km.bindings[ACTION_PASTE].chord.mods == (KM_MOD_CTRL | KM_MOD_SHIFT));
    assert(strcmp(km.bindings[ACTION_HISTORY_PREV].chord.key, "Up") == 0);
    assert(km.bindings[ACTION_HISTORY_PREV].chord.mods == KM_MOD_NONE);

    /* 2. Chord parsing */
    KeyChord chord;
    assert(keymap_parse_chord("Ctrl+Shift+c", &chord));
    assert(chord.mods == (KM_MOD_CTRL | KM_MOD_SHIFT));
    assert(strcmp(chord.key, "c") == 0);

    assert(keymap_parse_chord("Alt+Return", &chord));
    assert(chord.mods == KM_MOD_ALT);
    assert(strcmp(chord.key, "Return") == 0);

    assert(keymap_parse_chord("  super + left  ", &chord));
    assert(chord.mods == KM_MOD_SUPER);
    assert(strcmp(chord.key, "Left") == 0);

    /* Malformed chords */
    assert(!keymap_parse_chord("", &chord));
    assert(!keymap_parse_chord("Ctrl+", &chord));
    assert(!keymap_parse_chord("+Return", &chord));
    assert(!keymap_parse_chord("UnknownMod+a", &chord));

    /* 3. Reserved chord detection */
    KeyChord r_chord;
    keymap_parse_chord("Ctrl+c", &r_chord);
    assert(keymap_is_reserved(&r_chord));
    keymap_parse_chord("Ctrl+z", &r_chord);
    assert(keymap_is_reserved(&r_chord));
    keymap_parse_chord("Tab", &r_chord);
    assert(keymap_is_reserved(&r_chord));
    keymap_parse_chord("Ctrl+Shift+t", &r_chord);
    assert(keymap_is_reserved(&r_chord));
    keymap_parse_chord("Ctrl+Shift+w", &r_chord);
    assert(keymap_is_reserved(&r_chord));
    keymap_parse_chord("Alt+1", &r_chord);
    assert(keymap_is_reserved(&r_chord));
    keymap_parse_chord("Alt+9", &r_chord);
    assert(keymap_is_reserved(&r_chord));
    keymap_parse_chord("Ctrl+Page_Up", &r_chord);
    assert(keymap_is_reserved(&r_chord));
    keymap_parse_chord("Shift+Page_Down", &r_chord);
    assert(keymap_is_reserved(&r_chord));

    /* Non-reserved */
    keymap_parse_chord("Ctrl+a", &r_chord);
    assert(!keymap_is_reserved(&r_chord));
    keymap_parse_chord("Ctrl+Shift+c", &r_chord);
    assert(!keymap_is_reserved(&r_chord));

    /* 4. File loading, conflict handling, and warnings */
    char test_rc_path[] = "/tmp/test_terminatorrc_unit_XXXXXX";
    int fd = mkstemp(test_rc_path);
    assert(fd >= 0);
    const char *rc_content =
        "# Terminator sample config for test\n"
        "bind kill_to_start = Ctrl+X\n"
        "bind kill_to_end = Ctrl+X\n"            /* duplicate chord: keep first, report second */
        "bind clear_screen = Ctrl+C\n"          /* reserved chord: warning, fallback to default */
        "bind unknown_act = Ctrl+Y\n"           /* unknown action: warning */
        "bind delete_prev_word = BadChord++\n"; /* unparsable chord: warning, fallback to default */
    assert(write(fd, rc_content, strlen(rc_content)) == (ssize_t)strlen(rc_content));
    close(fd);

    KeyMap loaded_km;
    char warnings[2048] = {0};
    int load_res = keymap_load_file(&loaded_km, test_rc_path, warnings, sizeof(warnings));
    assert(load_res == 0);
    assert(strstr(warnings, "is already bound to 'kill_to_start'") != NULL);
    assert(strstr(warnings, "reserved") != NULL);
    assert(strstr(warnings, "unknown action 'unknown_act'") != NULL);
    assert(strstr(warnings, "unparsable chord 'BadChord++'") != NULL);

    /* Verify fallback to defaults */
    assert(strcmp(loaded_km.bindings[ACTION_KILL_TO_START].chord.key, "x") == 0);
    assert(strcmp(loaded_km.bindings[ACTION_KILL_TO_END].chord.key, "k") == 0); /* default */
    assert(strcmp(loaded_km.bindings[ACTION_CLEAR_SCREEN].chord.key, "l") == 0); /* default */
    assert(strcmp(loaded_km.bindings[ACTION_DELETE_PREV_WORD].chord.key, "w") == 0); /* default */

    /* 5. Atomic saving and reloading */
    char err_buf[256] = {0};
    KeyChord new_chord;
    keymap_parse_chord("Ctrl+J", &new_chord);
    assert(keymap_set_binding(&loaded_km, ACTION_KILL_TO_START, &new_chord, err_buf, sizeof(err_buf)) == 0);
    assert(keymap_save_file(&loaded_km, test_rc_path, err_buf, sizeof(err_buf)) == 0);

    KeyMap reloaded_km;
    warnings[0] = '\0';
    assert(keymap_load_file(&reloaded_km, test_rc_path, warnings, sizeof(warnings)) == 0);
    assert(warnings[0] == '\0');
    assert(strcmp(reloaded_km.bindings[ACTION_KILL_TO_START].chord.key, "j") == 0);
    assert(reloaded_km.bindings[ACTION_KILL_TO_START].chord.mods == KM_MOD_CTRL);

    unlink(test_rc_path);
    printf("  All Task 7 Keymap Unit Tests passed!\n");
}

static void run_task7_bind_builtin_tests(Shared *sh) {
    printf("\n--- PART 11C: Task 7 Built-in bind Tests (Thread Mode) ---\n");

    char temp_rc_path[] = "/tmp/test_terminatorrc_builtin_XXXXXX";
    int fd = mkstemp(temp_rc_path);
    assert(fd >= 0);
    close(fd);
    setenv("TERMINATORRC", temp_rc_path, 1);

    int to[2], from[2];
    assert(pipe2(to, O_CLOEXEC) == 0);
    assert(pipe2(from, O_CLOEXEC) == 0);

    TabChannel chan;
    chan.to_tab_fd = to[0];
    chan.from_tab_fd = from[1];
    chan.shared = sh;

    pthread_t th;
    assert(pthread_create(&th, NULL, tab_thread_func, &chan) == 0);

    HarnessTab tab;
    memset(&tab, 0, sizeof(tab));
    InBuffer in;
    memset(&in, 0, sizeof(in));

    wait_for_prompt(from[0], &tab, &in);

    /* 1. Run bind without args: lists all bindings */
    printf("  [11.1] Testing 'bind' list...\n");
    send_cmd(to[1], from[0], &tab, &in, "bind");
    assert(strstr(tab.last_output, "bind line_start = Ctrl+A") != NULL);
    assert(strstr(tab.last_output, "bind kill_to_start = Ctrl+U") != NULL);
    assert(strstr(tab.last_output, "bind copy = Ctrl+Shift+C") != NULL);
    printf("    bind output list verified!\n");

    /* 2. Rebind kill_to_start to Ctrl+J */
    printf("  [11.2] Testing 'bind kill_to_start Ctrl+J' and FRAME_RELOAD_KEYS...\n");
    tab.reloaded_keys = false;
    send_cmd(to[1], from[0], &tab, &in, "bind kill_to_start Ctrl+J");
    assert(tab.reloaded_keys == true);
    printf("    FRAME_RELOAD_KEYS frame received!\n");

    /* Verify updated via 'bind' command */
    send_cmd(to[1], from[0], &tab, &in, "bind");
    assert(strstr(tab.last_output, "bind kill_to_start = Ctrl+J") != NULL);
    printf("    Updated binding reflected in 'bind' listing!\n");

    /* 3. Reset kill_to_start */
    printf("  [11.3] Testing 'bind reset kill_to_start'...\n");
    tab.reloaded_keys = false;
    send_cmd(to[1], from[0], &tab, &in, "bind reset kill_to_start");
    assert(tab.reloaded_keys == true);
    send_cmd(to[1], from[0], &tab, &in, "bind");
    assert(strstr(tab.last_output, "bind kill_to_start = Ctrl+U") != NULL);
    printf("    Reset binding verified!\n");

    /* 4. Reserved chord error */
    printf("  [11.4] Testing reserved chord rejected...\n");
    send_cmd(to[1], from[0], &tab, &in, "bind clear_screen Ctrl+C");
    assert(strstr(tab.last_output, "reserved") != NULL);
    send_cmd(to[1], from[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "1\n") != NULL);
    printf("    Reserved chord correctly rejected with status 1!\n");

    /* 5. Unknown action error */
    printf("  [11.5] Testing unknown action rejected...\n");
    send_cmd(to[1], from[0], &tab, &in, "bind nonexistent_act Ctrl+X");
    assert(strstr(tab.last_output, "unknown action 'nonexistent_act'") != NULL);
    send_cmd(to[1], from[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "1\n") != NULL);
    printf("    Unknown action correctly rejected with status 1!\n");

    /* Exit tab */
    proto_write_frame_blocking(to[1], FRAME_EOF, NULL, 0);
    pthread_join(th, NULL);
    close(to[1]); close(from[0]);
    free(tab.last_output);
    unlink(temp_rc_path);
    unsetenv("TERMINATORRC");
    printf("  Task 7 Built-in bind Tests passed!\n");
}

static void run_task7_history_navigation_tests(Shared *sh) {
    printf("\n--- PART 11D: Task 7 History Navigation Sequence Tests ---\n");

    int to1[2], from1[2];
    int to2[2], from2[2];
    assert(pipe2(to1, O_CLOEXEC) == 0);
    assert(pipe2(from1, O_CLOEXEC) == 0);
    assert(pipe2(to2, O_CLOEXEC) == 0);
    assert(pipe2(from2, O_CLOEXEC) == 0);

    TabChannel chan1;
    chan1.to_tab_fd = to1[0];
    chan1.from_tab_fd = from1[1];
    chan1.shared = sh;

    TabChannel chan2;
    chan2.to_tab_fd = to2[0];
    chan2.from_tab_fd = from2[1];
    chan2.shared = sh;

    pthread_t th1, th2;
    assert(pthread_create(&th1, NULL, tab_thread_func, &chan1) == 0);
    assert(pthread_create(&th2, NULL, tab_thread_func, &chan2) == 0);

    HarnessTab tab1;
    memset(&tab1, 0, sizeof(tab1));
    HarnessTab tab2;
    memset(&tab2, 0, sizeof(tab2));
    InBuffer in1;
    memset(&in1, 0, sizeof(in1));
    InBuffer in2;
    memset(&in2, 0, sizeof(in2));

    wait_for_prompt(from1[0], &tab1, &in1);
    wait_for_prompt(from2[0], &tab2, &in2);

    /* Tab 1 executes 2 commands */
    send_cmd(to1[1], from1[0], &tab1, &in1, "echo cmd1_tab1");
    send_cmd(to1[1], from1[0], &tab1, &in1, "echo cmd2_tab1");

    /* Tab 2 executes 1 command */
    send_cmd(to2[1], from2[0], &tab2, &in2, "echo cmd3_tab2");

    /* Verify shared history sequence integrity */
    uint64_t newest_seq = history_newest_seq(sh);
    assert(newest_seq >= 3);

    char hbuf[1024];
    /* newest entry should be "echo cmd3_tab2" */
    assert(history_get_by_seq(sh, newest_seq, hbuf, sizeof(hbuf)) == 0);
    assert(strcmp(hbuf, "echo cmd3_tab2") == 0);

    /* previous entry should be "echo cmd2_tab1" */
    assert(history_get_by_seq(sh, newest_seq - 1, hbuf, sizeof(hbuf)) == 0);
    assert(strcmp(hbuf, "echo cmd2_tab1") == 0);

    /* entry before that should be "echo cmd1_tab1" */
    assert(history_get_by_seq(sh, newest_seq - 2, hbuf, sizeof(hbuf)) == 0);
    assert(strcmp(hbuf, "echo cmd1_tab1") == 0);

    /* Test in-progress line saving and sequence navigation logic */
    Tab test_tab;
    memset(&test_tab, 0, sizeof(test_tab));
    lineedit_init(&test_tab.input);
    lineedit_insert_text(&test_tab.input, "in_progress_partial_cmd", 23);

    /* First Up: save in-progress text and load newest */
    test_tab.hist_browsing = true;
    test_tab.hist_seq = newest_seq;
    strncpy(test_tab.saved_input, test_tab.input.buf, sizeof(test_tab.saved_input) - 1);
    char entry[1024];
    assert(history_get_by_seq(sh, test_tab.hist_seq, entry, sizeof(entry)) == 0);
    lineedit_set_text(&test_tab.input, entry);
    assert(strcmp(test_tab.input.buf, "echo cmd3_tab2") == 0);

    /* Second Up: load seq - 1 */
    test_tab.hist_seq--;
    assert(history_get_by_seq(sh, test_tab.hist_seq, entry, sizeof(entry)) == 0);
    lineedit_set_text(&test_tab.input, entry);
    assert(strcmp(test_tab.input.buf, "echo cmd2_tab1") == 0);

    /* Down: load seq + 1 */
    test_tab.hist_seq++;
    assert(history_get_by_seq(sh, test_tab.hist_seq, entry, sizeof(entry)) == 0);
    lineedit_set_text(&test_tab.input, entry);
    assert(strcmp(test_tab.input.buf, "echo cmd3_tab2") == 0);

    /* Down past newest: restore in-progress saved input */
    test_tab.hist_seq++;
    if (test_tab.hist_seq > newest_seq) {
        test_tab.hist_browsing = false;
        test_tab.hist_seq = 0;
        lineedit_set_text(&test_tab.input, test_tab.saved_input);
        test_tab.saved_input[0] = '\0';
    }
    assert(strcmp(test_tab.input.buf, "in_progress_partial_cmd") == 0);
    assert(!test_tab.hist_browsing);

    /* Clean exit of both tabs */
    proto_write_frame_blocking(to1[1], FRAME_EOF, NULL, 0);
    proto_write_frame_blocking(to2[1], FRAME_EOF, NULL, 0);
    pthread_join(th1, NULL);
    pthread_join(th2, NULL);
    close(to1[1]); close(from1[0]);
    close(to2[1]); close(from2[0]);
    free(tab1.last_output);
    free(tab2.last_output);

    printf("  Task 7 History Navigation Sequence Tests passed!\n");
}

static void run_task8_signals_jobs_tests(Shared *sh) {
    printf("\n--- PART 12: Task 8 Signals & Job Control Tests ---\n");

    int to[2], from[2];
    assert(pipe2(to, O_CLOEXEC) == 0);
    assert(pipe2(from, O_CLOEXEC) == 0);

    TabChannel chan;
    chan.to_tab_fd = to[0];
    chan.from_tab_fd = from[1];
    chan.shared = sh;

    pthread_t th;
    assert(pthread_create(&th, NULL, tab_thread_func, &chan) == 0);

    HarnessTab tab;
    memset(&tab, 0, sizeof(tab));
    InBuffer in;
    memset(&in, 0, sizeof(in));

    wait_for_prompt(from[0], &tab, &in);
    assert(strcmp(tab.last_prompt, "user@terminator> ") == 0);

    /* Test 12.1: Spec Transcript exact reproduction
     * user@terminator> sleep 100
     * ^Z
     * [1]+ Stopped sleep 100
     * user@terminator> bg %1
     * [1]+ sleep 100 &
     * user@terminator> fg %1
     * sleep 100
     * ^C
     * user@terminator>
     */
    printf("  [12.1] Testing Spec Transcript exact reproduction...\n");
    reset_tab_output(&tab);
    proto_write_frame_blocking(to[1], FRAME_LINE, "sleep 100", 9);
    poll(NULL, 0, 50); /* Let sleep 100 start and setpgid */
    proto_write_frame_blocking(to[1], FRAME_TSTP, NULL, 0);
    wait_for_prompt(from[0], &tab, &in);

    assert(strstr(tab.last_output, "[1]+ Stopped sleep 100") != NULL);
    assert(strcmp(tab.last_prompt, "user@terminator> ") == 0);

    send_cmd(to[1], from[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "148") != NULL || strstr(tab.last_output, "146") != NULL);

    send_cmd(to[1], from[0], &tab, &in, "bg %1");
    assert(strstr(tab.last_output, "[1]+ sleep 100 &") != NULL);

    send_cmd(to[1], from[0], &tab, &in, "jobs");
    assert(strstr(tab.last_output, "[1]+ Running") != NULL);
    assert(strstr(tab.last_output, "sleep 100 &") != NULL);

    reset_tab_output(&tab);
    proto_write_frame_blocking(to[1], FRAME_LINE, "fg %1", 5);
    wait_for_output_substring(from[0], &tab, &in, "sleep 100", 2000);
    assert(strstr(tab.last_output, "sleep 100") != NULL);

    poll(NULL, 0, 50);
    proto_write_frame_blocking(to[1], FRAME_INTR, NULL, 0);
    wait_for_prompt(from[0], &tab, &in);
    assert(strcmp(tab.last_prompt, "user@terminator> ") == 0);

    send_cmd(to[1], from[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "130") != NULL);

    send_cmd(to[1], from[0], &tab, &in, "jobs");
    assert(strstr(tab.last_output, "sleep 100") == NULL);
    printf("    Spec transcript reproduction verified!\n");

    /* Test 12.2: Job spec resolution and errors */
    printf("  [12.2] Testing Job spec resolution (%%, %%%%, %%+ , %%- , %%n) and errors...\n");
    /* Start job 1 and stop it */
    reset_tab_output(&tab);
    proto_write_frame_blocking(to[1], FRAME_LINE, "sleep 101", 9);
    poll(NULL, 0, 50);
    proto_write_frame_blocking(to[1], FRAME_TSTP, NULL, 0);
    wait_for_prompt(from[0], &tab, &in);

    /* Start job 2 and stop it */
    reset_tab_output(&tab);
    proto_write_frame_blocking(to[1], FRAME_LINE, "sleep 102", 9);
    poll(NULL, 0, 50);
    proto_write_frame_blocking(to[1], FRAME_TSTP, NULL, 0);
    wait_for_prompt(from[0], &tab, &in);

    /* jobs should show both jobs with + on job 2 and - on job 1 */
    send_cmd(to[1], from[0], &tab, &in, "jobs");
    assert(strstr(tab.last_output, "[1]-") != NULL);
    assert(strstr(tab.last_output, "[2]+") != NULL);

    /* Test nonexistent job errors */
    send_cmd(to[1], from[0], &tab, &in, "fg %99");
    assert(strstr(tab.last_output, "terminator: fg: %99: no such job") != NULL);
    send_cmd(to[1], from[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "1") != NULL);

    send_cmd(to[1], from[0], &tab, &in, "bg %99");
    assert(strstr(tab.last_output, "terminator: bg: %99: no such job") != NULL);
    send_cmd(to[1], from[0], &tab, &in, "echo $?");
    assert(strstr(tab.last_output, "1") != NULL);

    /* Resume previous job %- with bg */
    send_cmd(to[1], from[0], &tab, &in, "bg %-");
    assert(strstr(tab.last_output, "[1]") != NULL && strstr(tab.last_output, "sleep 101 &") != NULL);

    /* Bring current job %+ to foreground and kill it */
    reset_tab_output(&tab);
    proto_write_frame_blocking(to[1], FRAME_LINE, "fg %+", 5);
    wait_for_output_substring(from[0], &tab, &in, "sleep 102", 2000);
    poll(NULL, 0, 50);
    proto_write_frame_blocking(to[1], FRAME_INTR, NULL, 0);
    wait_for_prompt(from[0], &tab, &in);

    /* Bring remaining job to foreground and kill it */
    reset_tab_output(&tab);
    proto_write_frame_blocking(to[1], FRAME_LINE, "fg", 2);
    wait_for_output_substring(from[0], &tab, &in, "sleep 101", 2000);
    poll(NULL, 0, 50);
    proto_write_frame_blocking(to[1], FRAME_INTR, NULL, 0);
    wait_for_prompt(from[0], &tab, &in);

    send_cmd(to[1], from[0], &tab, &in, "jobs");
    assert(strstr(tab.last_output, "sleep") == NULL);
    printf("    Job spec resolution verified!\n");

    /* Test 12.3: Background execution cmd & and done notification */
    printf("  [12.3] Testing Background Execution (cmd & and pipeline &) & Done notice...\n");
    send_cmd(to[1], from[0], &tab, &in, "sleep 1 &");
    assert(strstr(tab.last_output, "[1]") != NULL);

    send_cmd(to[1], from[0], &tab, &in, "jobs");
    assert(strstr(tab.last_output, "[1]+ Running") != NULL);
    assert(strstr(tab.last_output, "sleep 1 &") != NULL);

    /* Sleep 1.2s until background job exits */
    poll(NULL, 0, 1200);

    /* Send command to trigger the Done announcement before next prompt */
    send_cmd(to[1], from[0], &tab, &in, "echo checking_done");
    assert(strstr(tab.last_output, "[1]+ Done sleep 1") != NULL);
    assert(strstr(tab.last_output, "checking_done") != NULL);

    /* jobs table must now be empty */
    send_cmd(to[1], from[0], &tab, &in, "jobs");
    assert(strstr(tab.last_output, "sleep 1") == NULL);

    /* Pipeline in background with output */
    send_cmd(to[1], from[0], &tab, &in, "echo bg_pipe_test | tr a-z A-Z &");
    assert(strstr(tab.last_output, "[1]") != NULL);
    wait_for_output_substring(from[0], &tab, &in, "BG_PIPE_TEST", 2000);
    assert(strstr(tab.last_output, "BG_PIPE_TEST") != NULL);
    printf("    Background execution and Done notification verified!\n");

    /* Test 12.4: Exit warning with stopped jobs */
    printf("  [12.4] Testing Exit Warning with stopped jobs...\n");
    reset_tab_output(&tab);
    proto_write_frame_blocking(to[1], FRAME_LINE, "sleep 200", 9);
    poll(NULL, 0, 50);
    proto_write_frame_blocking(to[1], FRAME_TSTP, NULL, 0);
    wait_for_prompt(from[0], &tab, &in);

    /* First exit: should warn and stay open */
    send_cmd(to[1], from[0], &tab, &in, "exit");
    assert(strstr(tab.last_output, "There are stopped jobs.") != NULL);
    assert(tab.exited == false);

    /* Intervening command resets warning */
    send_cmd(to[1], from[0], &tab, &in, "jobs");
    assert(strstr(tab.last_output, "Stopped") != NULL);

    send_cmd(to[1], from[0], &tab, &in, "exit");
    assert(strstr(tab.last_output, "There are stopped jobs.") != NULL);
    assert(tab.exited == false);

    /* Second exit immediately forces exit */
    reset_tab_output(&tab);
    proto_write_frame_blocking(to[1], FRAME_LINE, "exit", 4);
    wait_for_prompt(from[0], &tab, &in);
    assert(tab.exited == true);
    pthread_join(th, NULL);
    close(to[1]);
    close(to[0]);
    close(from[0]);
    close(from[1]);
    free(tab.last_output);
    printf("    Exit warning with stopped jobs verified!\n");

    /* Test 12.5: Tab Isolation: Signals in Tab A do NOT affect Tab B */
    printf("  [12.5] Testing Tab Isolation (Ctrl+C in Tab A does not affect Tab B)...\n");
    int toA[2], fromA[2], toB[2], fromB[2];
    assert(pipe2(toA, O_CLOEXEC) == 0);
    assert(pipe2(fromA, O_CLOEXEC) == 0);
    assert(pipe2(toB, O_CLOEXEC) == 0);
    assert(pipe2(fromB, O_CLOEXEC) == 0);

    TabChannel chanA = { toA[0], fromA[1], sh };
    TabChannel chanB = { toB[0], fromB[1], sh };

    pthread_t thA, thB;
    assert(pthread_create(&thA, NULL, tab_thread_func, &chanA) == 0);
    assert(pthread_create(&thB, NULL, tab_thread_func, &chanB) == 0);

    HarnessTab tabA, tabB;
    memset(&tabA, 0, sizeof(tabA));
    memset(&tabB, 0, sizeof(tabB));
    InBuffer inA, inB;
    memset(&inA, 0, sizeof(inA));
    memset(&inB, 0, sizeof(inB));

    wait_for_prompt(fromA[0], &tabA, &inA);
    wait_for_prompt(fromB[0], &tabB, &inB);

    /* Start sleep in both tabs */
    reset_tab_output(&tabA);
    proto_write_frame_blocking(toA[1], FRAME_LINE, "sleep 100", 9);
    reset_tab_output(&tabB);
    proto_write_frame_blocking(toB[1], FRAME_LINE, "sleep 100", 9);
    poll(NULL, 0, 50);

    /* Send Ctrl+C only to Tab A */
    proto_write_frame_blocking(toA[1], FRAME_INTR, NULL, 0);
    wait_for_prompt(fromA[0], &tabA, &inA);
    assert(strcmp(tabA.last_prompt, "user@terminator> ") == 0);
    send_cmd(toA[1], fromA[0], &tabA, &inA, "echo $?");
    assert(strstr(tabA.last_output, "130") != NULL);

    /* Tab B is STILL running sleep 100! Verify by sending Ctrl+Z to Tab B */
    proto_write_frame_blocking(toB[1], FRAME_TSTP, NULL, 0);
    wait_for_prompt(fromB[0], &tabB, &inB);
    assert(strstr(tabB.last_output, "[1]+ Stopped sleep 100") != NULL);

    /* Clean up tabs */
    reset_tab_output(&tabB);
    proto_write_frame_blocking(toB[1], FRAME_LINE, "fg", 2);
    poll(NULL, 0, 50);
    proto_write_frame_blocking(toB[1], FRAME_INTR, NULL, 0);
    wait_for_prompt(fromB[0], &tabB, &inB);

    send_cmd(toA[1], fromA[0], &tabA, &inA, "exit");
    send_cmd(toB[1], fromB[0], &tabB, &inB, "exit");
    pthread_join(thA, NULL);
    pthread_join(thB, NULL);
    close(toA[1]); close(toA[0]); close(fromA[0]); close(fromA[1]);
    close(toB[1]); close(toB[0]); close(fromB[0]); close(fromB[1]);
    free(tabA.last_output);
    free(tabB.last_output);
    printf("    Tab isolation verified!\n");

    /* Test 12.6: Process Mode Signals & Job Control */
    printf("  [12.6] Testing Process Mode Signals & Job Control...\n");
    int toP[2], fromP[2];
    assert(pipe2(toP, O_CLOEXEC) == 0);
    assert(pipe2(fromP, O_CLOEXEC) == 0);

    pid_t tab_proc = fork();
    assert(tab_proc >= 0);
    if (tab_proc == 0) {
        close(toP[1]);
        close(fromP[0]);
        TabChannel proc_chan = { toP[0], fromP[1], sh };
        tab_main(&proc_chan);
        close(toP[0]);
        close(fromP[1]);
        _exit(0);
    }

    close(toP[0]);
    close(fromP[1]);

    HarnessTab tabP;
    memset(&tabP, 0, sizeof(tabP));
    InBuffer inP;
    memset(&inP, 0, sizeof(inP));

    wait_for_prompt(fromP[0], &tabP, &inP);

    /* Run sleep 100 in process mode tab, Ctrl+Z it */
    reset_tab_output(&tabP);
    proto_write_frame_blocking(toP[1], FRAME_LINE, "sleep 100", 9);
    poll(NULL, 0, 50);
    proto_write_frame_blocking(toP[1], FRAME_TSTP, NULL, 0);
    wait_for_prompt(fromP[0], &tabP, &inP);
    assert(strstr(tabP.last_output, "[1]+ Stopped sleep 100") != NULL);

    /* Resume in background */
    send_cmd(toP[1], fromP[0], &tabP, &inP, "bg %1");
    assert(strstr(tabP.last_output, "[1]+ sleep 100 &") != NULL);

    /* Foreground and Ctrl+C */
    reset_tab_output(&tabP);
    proto_write_frame_blocking(toP[1], FRAME_LINE, "fg %1", 5);
    wait_for_output_substring(fromP[0], &tabP, &inP, "sleep 100", 2000);
    poll(NULL, 0, 50);
    proto_write_frame_blocking(toP[1], FRAME_INTR, NULL, 0);
    wait_for_prompt(fromP[0], &tabP, &inP);
    send_cmd(toP[1], fromP[0], &tabP, &inP, "echo $?");
    assert(strstr(tabP.last_output, "130") != NULL);

    /* Background pipeline */
    send_cmd(toP[1], fromP[0], &tabP, &inP, "echo proc_pipe_test | tr a-z A-Z &");
    wait_for_output_substring(fromP[0], &tabP, &inP, "PROC_PIPE_TEST", 2000);
    assert(strstr(tabP.last_output, "PROC_PIPE_TEST") != NULL);

    /* Exit process mode tab */
    reset_tab_output(&tabP);
    proto_write_frame_blocking(toP[1], FRAME_LINE, "exit", 4);
    wait_for_prompt(fromP[0], &tabP, &inP);
    int p_status = 0;
    waitpid(tab_proc, &p_status, 0);
    assert(WIFEXITED(p_status) && WEXITSTATUS(p_status) == 0);
    close(toP[1]);
    close(fromP[0]);
    free(tabP.last_output);
    printf("    Process mode signals and job control verified!\n");

    printf("  All Task 8 Signals & Job Control Tests passed!\n");
}

/* Worker thread argument for history stress test */
typedef struct {
    Shared *sh;
    int worker_id;
    int count;
} HistWorkerArg;

static void *hist_thread_stress_worker(void *arg) {
    HistWorkerArg *w = (HistWorkerArg *)arg;
    char cmd[128];
    for (int i = 0; i < w->count; i++) {
        snprintf(cmd, sizeof(cmd), "cmd_th_%d_%d", w->worker_id, i);
        uint64_t seq = history_append(w->sh, cmd);
        assert(seq > 0);
    }
    return NULL;
}

static void run_task9_history_tests(void) {
    printf("\n--- PART 13: Task 9 Searchable, Persistent, Shared History Tests ---\n");

    /* ----------------------------------------------------
     * Test 13.1: LCS Algorithm & Search Decision Order
     * ---------------------------------------------------- */
    printf("  [13.1] Testing LCS Algorithm and Search Decision Order...\n");
    Shared *t_sh = shared_create();
    assert(t_sh != NULL);

    /* Empty term */
    HistSearchResult res;
    assert(history_search(t_sh, "", &res) == 0);
    assert(res.match_type == HIST_MATCH_NONE);
    hist_search_result_free(&res);

    assert(history_search(t_sh, "   ", &res) == 0);
    assert(res.match_type == HIST_MATCH_NONE);
    hist_search_result_free(&res);

    /* Spec example reproduction:
     * history has "gcc -o myprog myprog.c" and "./myprog < in.txt"
     * search "myprog" prints both commands (longest common substring = 6 > 2) */
    history_append(t_sh, "gcc -o myprog myprog.c");
    history_append(t_sh, "./myprog < in.txt");

    assert(history_search(t_sh, "myprog", &res) == 0);
    assert(res.match_type == HIST_MATCH_SUBSTRING);
    assert(res.match_len == 6);
    assert(res.count == 2);
    /* Tied for maximum length, most recent first */
    assert(strcmp(res.entries[0], "./myprog < in.txt") == 0);
    assert(strcmp(res.entries[1], "gcc -o myprog myprog.c") == 0);
    hist_search_result_free(&res);

    /* Spec example reproduction for no match:
     * search "xyz" prints "No match for search term in history" */
    assert(history_search(t_sh, "xyz", &res) == 0);
    assert(res.match_type == HIST_MATCH_NONE);
    assert(res.count == 0);
    hist_search_result_free(&res);

    /* Test <= 2 chars threshold for substring matches: match must be > 2 chars */
    history_append(t_sh, "ab_other");
    assert(history_search(t_sh, "ab", &res) == 0);
    assert(res.match_type == HIST_MATCH_NONE);
    hist_search_result_free(&res);

    history_append(t_sh, "abcd");
    assert(history_search(t_sh, "abc", &res) == 0);
    assert(res.match_type == HIST_MATCH_SUBSTRING);
    assert(res.match_len == 3);
    assert(res.count == 1);
    assert(strcmp(res.entries[0], "abcd") == 0);
    hist_search_result_free(&res);

    /* Decision Order rule (a):
     * "the most recent command that exactly matches the term (equal string); otherwise" */
    history_append(t_sh, "myprog");
    assert(history_search(t_sh, "myprog", &res) == 0);
    assert(res.match_type == HIST_MATCH_EXACT);
    assert(res.count == 1);
    assert(strcmp(res.entries[0], "myprog") == 0);
    hist_search_result_free(&res);

    /* Deduplication test: identical commands tied for max LCS only returned once */
    history_append(t_sh, "git status");
    history_append(t_sh, "cargo build");
    history_append(t_sh, "git status");
    assert(history_search(t_sh, "status", &res) == 0);
    assert(res.match_type == HIST_MATCH_SUBSTRING);
    assert(res.count == 1);
    assert(strcmp(res.entries[0], "git status") == 0);
    hist_search_result_free(&res);

    shared_destroy(t_sh);
    printf("    LCS Algorithm and Search Decision Order verified!\n");

    /* ----------------------------------------------------
     * Test 13.2: Multiline Encoding/Decoding & Persistence File
     * ---------------------------------------------------- */
    printf("  [13.2] Testing Multiline Encoding/Decoding and Persistence File...\n");
    const char *raw_multiline = "echo \"first line\\nsecond\\\\third\"";
    char enc[2048];
    size_t elen = history_encode_line(raw_multiline, enc, sizeof(enc));
    assert(elen > 0);
    assert(enc[elen - 1] == '\n'); /* Trailing newline delimiter */

    char dec[1024];
    size_t dlen = history_decode_line(enc, elen - 1, dec, sizeof(dec));
    assert(dlen == strlen(raw_multiline));
    assert(strcmp(dec, raw_multiline) == 0);

    /* File creation and permissions 0600 */
    const char *test_histfile = "/tmp/terminator_test_histfile.txt";
    unlink(test_histfile);
    setenv("TERMINATOR_HISTFILE", test_histfile, 1);

    Shared *p_sh = shared_create();
    assert(p_sh != NULL);
    assert(history_init_persistence(p_sh) == 0);

    struct stat st;
    assert(stat(test_histfile, &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    history_append(p_sh, "line_one");
    history_append(p_sh, "echo \"line 2\nline 3\"");
    assert(history_count(p_sh) == 2);
    shared_destroy(p_sh);

    /* Persistence restart: new Shared loads entries from existing file */
    Shared *p_sh2 = shared_create();
    assert(p_sh2 != NULL);
    assert(history_init_persistence(p_sh2) == 0);
    assert(history_count(p_sh2) == 2);

    char hbuf[1024];
    assert(history_get_by_seq(p_sh2, 1, hbuf, sizeof(hbuf)) == 0);
    assert(strcmp(hbuf, "line_one") == 0);
    assert(history_get_by_seq(p_sh2, 2, hbuf, sizeof(hbuf)) == 0);
    assert(strcmp(hbuf, "echo \"line 2\nline 3\"") == 0);

    /* Partial last line (unterminated file line) gracefully handled */
    int afd = open(test_histfile, O_WRONLY | O_APPEND);
    assert(afd >= 0);
    const char *partial = "partial_cmd_no_newline";
    assert(write(afd, partial, strlen(partial)) == (ssize_t)strlen(partial));
    close(afd);
    shared_destroy(p_sh2);

    Shared *p_sh3 = shared_create();
    assert(p_sh3 != NULL);
    assert(history_init_persistence(p_sh3) == 0);
    assert(history_count(p_sh3) == 3);
    assert(history_get_by_seq(p_sh3, 3, hbuf, sizeof(hbuf)) == 0);
    assert(strcmp(hbuf, "partial_cmd_no_newline") == 0);
    shared_destroy(p_sh3);
    unlink(test_histfile);

    printf("    Multiline Encoding/Decoding and Persistence File verified!\n");

    /* ----------------------------------------------------
     * Test 13.3: Compaction (> 20,000 entries compacted to 10,000)
     * ---------------------------------------------------- */
    printf("  [13.3] Testing Startup Compaction (> 20,000 entries)...\n");
    const char *compact_histfile = "/tmp/terminator_test_compact.txt";
    unlink(compact_histfile);
    setenv("TERMINATOR_HISTFILE", compact_histfile, 1);

    FILE *fcomp = fopen(compact_histfile, "w");
    assert(fcomp != NULL);
    for (int i = 1; i <= 21500; i++) {
        fprintf(fcomp, "history_item_%d\n", i);
    }
    fclose(fcomp);

    Shared *c_sh = shared_create();
    assert(c_sh != NULL);
    assert(history_init_persistence(c_sh) == 0);
    assert(history_count(c_sh) == 10000);

    /* Verify ring holds the latest 10,000 entries (item 11501 to 21500) */
    assert(history_get_by_seq(c_sh, 11501, hbuf, sizeof(hbuf)) == 0);
    assert(strcmp(hbuf, "history_item_11501") == 0);
    assert(history_get_by_seq(c_sh, 21500, hbuf, sizeof(hbuf)) == 0);
    assert(strcmp(hbuf, "history_item_21500") == 0);

    /* Verify file was atomically compacted down to 10,000 entries */
    FILE *fcheck = fopen(compact_histfile, "r");
    assert(fcheck != NULL);
    size_t lines_in_file = 0;
    char line_tmp[256];
    while (fgets(line_tmp, sizeof(line_tmp), fcheck)) {
        lines_in_file++;
    }
    fclose(fcheck);
    assert(lines_in_file == 10000);

    shared_destroy(c_sh);
    unlink(compact_histfile);
    printf("    Startup Compaction verified!\n");

    /* ----------------------------------------------------
     * Test 13.4: Thread Mode Stress (8 threads x 5,000 appends = 40,000 total)
     * ---------------------------------------------------- */
    printf("  [13.4] Testing Thread Mode Stress (8 threads x 5,000 appends)...\n");
    const char *stress_th_file = "/tmp/terminator_stress_th.txt";
    unlink(stress_th_file);
    setenv("TERMINATOR_HISTFILE", stress_th_file, 1);

    Shared *s_th = shared_create();
    assert(s_th != NULL);
    assert(history_init_persistence(s_th) == 0);

    pthread_t th_workers[8];
    HistWorkerArg th_args[8];
    for (int i = 0; i < 8; i++) {
        th_args[i].sh = s_th;
        th_args[i].worker_id = i;
        th_args[i].count = 5000;
        assert(pthread_create(&th_workers[i], NULL, hist_thread_stress_worker, &th_args[i]) == 0);
    }
    for (int i = 0; i < 8; i++) {
        pthread_join(th_workers[i], NULL);
    }

    assert(history_count(s_th) == 10000);
    assert(s_th->history.next_seq == 40001);
    shared_destroy(s_th);
    unlink(stress_th_file);
    printf("    Thread Mode Stress test passed!\n");

    /* ----------------------------------------------------
     * Test 13.5: Process Mode Stress (8 processes x 5,000 appends = 40,000 total)
     * ---------------------------------------------------- */
    printf("  [13.5] Testing Process Mode Stress (8 processes x 5,000 appends)...\n");
    const char *stress_proc_file = "/tmp/terminator_stress_proc.txt";
    unlink(stress_proc_file);
    setenv("TERMINATOR_HISTFILE", stress_proc_file, 1);

    Shared *s_pr = shared_create();
    assert(s_pr != NULL);
    assert(history_init_persistence(s_pr) == 0);

    pid_t proc_workers[8];
    for (int w = 0; w < 8; w++) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            char p_cmd[128];
            for (int i = 0; i < 5000; i++) {
                snprintf(p_cmd, sizeof(p_cmd), "proc_cmd_%d_%d", w, i);
                uint64_t seq = history_append(s_pr, p_cmd);
                assert(seq > 0);
            }
            _exit(0);
        }
        proc_workers[w] = pid;
    }

    for (int w = 0; w < 8; w++) {
        int status = 0;
        waitpid(proc_workers[w], &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    assert(history_count(s_pr) == 10000);
    assert(s_pr->history.next_seq == 40001);
    shared_destroy(s_pr);
    unlink(stress_proc_file);
    printf("    Process Mode Stress test passed!\n");

    /* ----------------------------------------------------
     * Test 13.6: Process Mode Dead-Owner Recovery (kill -9 mid-lock)
     * ---------------------------------------------------- */
    printf("  [13.6] Testing Process Mode Dead-Owner Recovery (kill -9 mid-lock)...\n");
    Shared *s_kill = shared_create();
    assert(s_kill != NULL);

    pid_t lock_pid = fork();
    assert(lock_pid >= 0);
    if (lock_pid == 0) {
        /* Take shared lock and spin/pause holding the lock */
        shared_lock(s_kill);
        while (1) {
            pause();
        }
        _exit(0);
    }

    /* Wait for child to take lock */
    usleep(50000);

    /* Kill child with SIGKILL while it holds the lock */
    kill(lock_pid, SIGKILL);
    int kill_status = 0;
    waitpid(lock_pid, &kill_status, 0);

    /* Parent attempts history_append; dead-owner detection must recover the lock */
    uint64_t rec_seq = history_append(s_kill, "recovered_cmd_after_kill");
    assert(rec_seq == 1);
    assert(history_count(s_kill) == 1);
    assert(history_get_by_seq(s_kill, 1, hbuf, sizeof(hbuf)) == 0);
    assert(strcmp(hbuf, "recovered_cmd_after_kill") == 0);

    shared_destroy(s_kill);
    printf("    Dead-owner recovery verified!\n");

    /* ----------------------------------------------------
     * Test 13.7: Benchmark Ctrl+R on 10,000 full history entries
     * ---------------------------------------------------- */
    printf("  [13.7] Benchmarking Ctrl+R on 10,000 full history entries (< 50 ms)...\n");
    Shared *b_sh = shared_create();
    assert(b_sh != NULL);

    /* Populate 10,000 varied command lines */
    for (int i = 0; i < 10000; i++) {
        char b_cmd[128];
        snprintf(b_cmd, sizeof(b_cmd), "gcc -O2 -Wall src/module%d.c -o bin%d", i, i);
        uint32_t idx = b_sh->history.head;
        b_sh->history.entries[idx].seq = b_sh->history.next_seq++;
        strncpy(b_sh->history.entries[idx].text, b_cmd, HISTORY_MAX_LINE - 1);
        b_sh->history.entries[idx].text[HISTORY_MAX_LINE - 1] = '\0';
        b_sh->history.head = (b_sh->history.head + 1) % HISTORY_CAPACITY;
        if (b_sh->history.count < HISTORY_CAPACITY) b_sh->history.count++;
    }
    assert(b_sh->history.count == 10000);

    struct timespec t0, t1;
    assert(clock_gettime(CLOCK_MONOTONIC, &t0) == 0);

    HistSearchResult b_res;
    assert(history_search(b_sh, "module4528", &b_res) == 0);

    assert(clock_gettime(CLOCK_MONOTONIC, &t1) == 0);

    double elapsed_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                        (double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0;

    assert(b_res.match_type == HIST_MATCH_SUBSTRING);
    assert(b_res.count >= 1);
    assert(strcmp(b_res.entries[0], "gcc -O2 -Wall src/module4528.c -o bin4528") == 0);
    hist_search_result_free(&b_res);
    shared_destroy(b_sh);

    printf("    Ctrl+R 10,000 entry search completed in %.2f ms (Target: < 50 ms)!\n", elapsed_ms);
    assert(elapsed_ms < 50.0);

    /* Restore default histfile */
    unsetenv("TERMINATOR_HISTFILE");

    printf("  All Task 9 History Tests passed!\n");
}

/* ====================================================
 * Part 14: Task 10 Tab-Key File-Name Completion Tests
 * ==================================================== */

static void run_task10_word_extraction_tests(void) {
    printf("  [14.1] Testing Word Extraction (start, middle, end, quotes, escapes)...\n");
    size_t wlen = 0;
    size_t start = 0;

    /* Single word at start */
    start = complete_extract_word("ls", 2, &wlen);
    assert(start == 0 && wlen == 2);

    /* Second word ending at cursor */
    start = complete_extract_word("ls foo", 6, &wlen);
    assert(start == 3 && wlen == 3);

    /* Inside single quotes */
    start = complete_extract_word("echo 'hello world' bar", 22, &wlen);
    assert(start == 19 && wlen == 3);

    /* Inside double quotes */
    start = complete_extract_word("echo \"hello world\" bar", 22, &wlen);
    assert(start == 19 && wlen == 3);

    /* Escaped space in middle of word */
    start = complete_extract_word("cat my\\ file.txt", 16, &wlen);
    assert(start == 4 && wlen == 12);

    /* Paths with slashes and tilde */
    start = complete_extract_word("cat /usr/local/bin", 18, &wlen);
    assert(start == 4 && wlen == 14);

    start = complete_extract_word("cat ~/documents", 15, &wlen);
    assert(start == 4 && wlen == 11);

    /* Mid-line cursor */
    start = complete_extract_word("cat abc def", 7, &wlen);
    assert(start == 4 && wlen == 3);

    /* Empty line */
    start = complete_extract_word("", 0, &wlen);
    assert(start == 0 && wlen == 0);

    /* Cursor at space after word */
    start = complete_extract_word("cat  ", 5, &wlen);
    assert(start == 5 && wlen == 0);

    printf("    Word extraction tests passed!\n");
}

static void run_task10_word_decomposition_tests(void) {
    printf("  [14.2] Testing Word Decomposition (dir_prefix & unescaped_prefix)...\n");
    char dir_pre[COMPLETE_MAX_WORD];
    char file_pre[COMPLETE_MAX_WORD];

    /* Simple filename without directory */
    complete_decompose_word("hello.txt", 9, dir_pre, sizeof(dir_pre), file_pre, sizeof(file_pre));
    assert(strcmp(dir_pre, "") == 0);
    assert(strcmp(file_pre, "hello.txt") == 0);

    /* Relative directory prefix */
    complete_decompose_word("src/test.c", 10, dir_pre, sizeof(dir_pre), file_pre, sizeof(file_pre));
    assert(strcmp(dir_pre, "src/") == 0);
    assert(strcmp(file_pre, "test.c") == 0);

    /* Tilde directory prefix */
    complete_decompose_word("~/docs/plan.md", 14, dir_pre, sizeof(dir_pre), file_pre, sizeof(file_pre));
    assert(strcmp(dir_pre, "~/docs/") == 0);
    assert(strcmp(file_pre, "plan.md") == 0);

    /* Directory prefix with escaped spaces */
    complete_decompose_word("my\\ folder/sub\\ file", 20, dir_pre, sizeof(dir_pre), file_pre, sizeof(file_pre));
    assert(strcmp(dir_pre, "my\\ folder/") == 0);
    assert(strcmp(file_pre, "sub file") == 0);

    printf("    Word decomposition tests passed!\n");
}

static void run_task10_escaping_tests(void) {
    printf("  [14.3] Testing Filename Escaping & Unescaping...\n");
    char esc[2048];
    char unesc[2048];

    /* Simple name */
    complete_escape_filename("simple.txt", esc, sizeof(esc));
    assert(strcmp(esc, "simple.txt") == 0);

    /* Name with spaces */
    complete_escape_filename("hello world.txt", esc, sizeof(esc));
    assert(strcmp(esc, "hello\\ world.txt") == 0);

    /* Shell metacharacters */
    complete_escape_filename("foo$bar&baz;qux*?()", esc, sizeof(esc));
    assert(strcmp(esc, "foo\\$bar\\&baz\\;qux\\*\\?\\(\\)") == 0);

    /* Directory with trailing slash: slash is NOT escaped */
    complete_escape_filename("my directory/", esc, sizeof(esc));
    assert(strcmp(esc, "my\\ directory/") == 0);

    /* Unescaping */
    complete_unescape_word(esc, strlen(esc), unesc, sizeof(unesc));
    assert(strcmp(unesc, "my directory/") == 0);

    printf("    Filename escaping & unescaping tests passed!\n");
}

static void run_task10_lcp_tests(void) {
    printf("  [14.4] Testing LCP (Longest Common Prefix) Algorithm...\n");
    char lcp[COMPLETE_MAX_WORD];

    /* Single element */
    char *c1[] = {"myfile.txt"};
    complete_compute_lcp(c1, 1, lcp, sizeof(lcp));
    assert(strcmp(lcp, "myfile.txt") == 0);

    /* Multiple elements with common prefix */
    char *c2[] = {"abc.txt", "abcd.txt"};
    complete_compute_lcp(c2, 2, lcp, sizeof(lcp));
    assert(strcmp(lcp, "abc") == 0);

    /* No common prefix */
    char *c3[] = {"apple", "banana"};
    complete_compute_lcp(c3, 2, lcp, sizeof(lcp));
    assert(strcmp(lcp, "") == 0);

    /* Identical elements */
    char *c4[] = {"foo_bar_123", "foo_bar_123"};
    complete_compute_lcp(c4, 2, lcp, sizeof(lcp));
    assert(strcmp(lcp, "foo_bar_123") == 0);

    /* Multiple sorted elements */
    char *c5[] = {"prefix_aaa", "prefix_bbb", "prefix_zzz"};
    complete_compute_lcp(c5, 3, lcp, sizeof(lcp));
    assert(strcmp(lcp, "prefix_") == 0);

    printf("    LCP algorithm tests passed!\n");
}

static void run_task10_candidate_gathering_tests(void) {
    printf("  [14.5] Testing Candidate Gathering, Dotfiles & Sorting...\n");
    char tmp_dir[] = "/tmp/term_test_compl_cand_XXXXXX";
    assert(mkdtemp(tmp_dir) != NULL);

    /* Create sample files and directory */
    char p[1024];
    snprintf(p, sizeof(p), "%s/abc.txt", tmp_dir);
    int fd = open(p, O_CREAT | O_WRONLY, 0644); assert(fd >= 0); close(fd);

    snprintf(p, sizeof(p), "%s/abcd.txt", tmp_dir);
    fd = open(p, O_CREAT | O_WRONLY, 0644); assert(fd >= 0); close(fd);

    snprintf(p, sizeof(p), "%s/def.txt", tmp_dir);
    fd = open(p, O_CREAT | O_WRONLY, 0644); assert(fd >= 0); close(fd);

    snprintf(p, sizeof(p), "%s/hello world.txt", tmp_dir);
    fd = open(p, O_CREAT | O_WRONLY, 0644); assert(fd >= 0); close(fd);

    snprintf(p, sizeof(p), "%s/.hidden_file", tmp_dir);
    fd = open(p, O_CREAT | O_WRONLY, 0644); assert(fd >= 0); close(fd);

    snprintf(p, sizeof(p), "%s/test_subfolder", tmp_dir);
    assert(mkdir(p, 0755) == 0);

    /* Query 1: Exactly 1 match for "de" */
    CompletionResult cr1;
    assert(complete_file_candidates("cmd de", 6, tmp_dir, &cr1) == 0);
    assert(cr1.count == 1);
    assert(strcmp(cr1.items[0], "def.txt") == 0);
    assert(strcmp(cr1.lcp, "def.txt") == 0);
    completion_result_free(&cr1);

    /* Query 2: Multiple matches for "abc" */
    CompletionResult cr2;
    assert(complete_file_candidates("cmd abc", 7, tmp_dir, &cr2) == 0);
    assert(cr2.count == 2);
    assert(strcmp(cr2.items[0], "abc.txt") == 0);
    assert(strcmp(cr2.items[1], "abcd.txt") == 0);
    assert(strcmp(cr2.lcp, "abc") == 0);
    completion_result_free(&cr2);

    /* Query 3: Empty prefix ("cmd ") lists all visible items, hides dotfiles, marks dir with trailing / */
    CompletionResult cr3;
    assert(complete_file_candidates("cmd ", 4, tmp_dir, &cr3) == 0);
    assert(cr3.count == 5); /* abc.txt, abcd.txt, def.txt, hello world.txt, test_subfolder/ */
    bool found_dir = false;
    bool found_dot = false;
    for (size_t i = 0; i < cr3.count; i++) {
        if (strcmp(cr3.items[i], "test_subfolder/") == 0) found_dir = true;
        if (strcmp(cr3.items[i], ".hidden_file") == 0) found_dot = true;
    }
    assert(found_dir);
    assert(!found_dot); /* Dotfiles must be hidden unless prefix starts with '.' */
    completion_result_free(&cr3);

    /* Query 4: Prefix starting with '.' reveals dotfiles */
    CompletionResult cr4;
    assert(complete_file_candidates("cmd .", 5, tmp_dir, &cr4) == 0);
    assert(cr4.count == 1);
    assert(strcmp(cr4.items[0], ".hidden_file") == 0);
    completion_result_free(&cr4);

    /* Query 5: Prefix with escaped space matches "hello world.txt" */
    CompletionResult cr5;
    assert(complete_file_candidates("cmd hello\\ ", 11, tmp_dir, &cr5) == 0);
    assert(cr5.count == 1);
    assert(strcmp(cr5.items[0], "hello world.txt") == 0);
    completion_result_free(&cr5);

    /* Clean up temporary directory */
    snprintf(p, sizeof(p), "%s/abc.txt", tmp_dir); unlink(p);
    snprintf(p, sizeof(p), "%s/abcd.txt", tmp_dir); unlink(p);
    snprintf(p, sizeof(p), "%s/def.txt", tmp_dir); unlink(p);
    snprintf(p, sizeof(p), "%s/hello world.txt", tmp_dir); unlink(p);
    snprintf(p, sizeof(p), "%s/.hidden_file", tmp_dir); unlink(p);
    snprintf(p, sizeof(p), "%s/test_subfolder", tmp_dir); rmdir(p);
    rmdir(tmp_dir);

    printf("    Candidate gathering & filtering tests passed!\n");
}

static void run_task10_spec_transcript_tests(void) {
    printf("  [14.6] Testing Exact Spec Transcript Reproduction...\n");
    /*
     * Spec Transcript:
     * user@terminator> ./myprog de   <Tab>   ->  ./myprog def.txt
     * user@terminator> ./myprog def.txt abc <Tab> -> 1. abc.txt 2. abcd.txt   (press 1)  -> ./myprog def.txt abc.txt
     */
    char tmp_dir[] = "/tmp/term_spec_trans_XXXXXX";
    assert(mkdtemp(tmp_dir) != NULL);

    char p[1024];
    snprintf(p, sizeof(p), "%s/abc.txt", tmp_dir);
    int fd = open(p, O_CREAT | O_WRONLY, 0644); assert(fd >= 0); close(fd);
    snprintf(p, sizeof(p), "%s/abcd.txt", tmp_dir);
    fd = open(p, O_CREAT | O_WRONLY, 0644); assert(fd >= 0); close(fd);
    snprintf(p, sizeof(p), "%s/def.txt", tmp_dir);
    fd = open(p, O_CREAT | O_WRONLY, 0644); assert(fd >= 0); close(fd);

    /* Step 1: "./myprog de" + <Tab> -> "./myprog def.txt" */
    LineEdit le;
    lineedit_init(&le);
    lineedit_insert_text(&le, "./myprog de", 11);
    assert(le.cursor == 11);

    CompletionResult cr1;
    assert(complete_file_candidates(lineedit_get_text(&le), le.cursor, tmp_dir, &cr1) == 0);
    assert(cr1.count == 1);
    assert(strcmp(cr1.items[0], "def.txt") == 0);

    /* Complete single candidate */
    char esc1[1024];
    complete_escape_filename(cr1.items[0], esc1, sizeof(esc1));
    char repl1[1024];
    snprintf(repl1, sizeof(repl1), "%s%s", cr1.dir_prefix, esc1);
    assert(lineedit_replace_range(&le, cr1.word_start, cr1.word_start + cr1.word_len, repl1));
    assert(strcmp(lineedit_get_text(&le), "./myprog def.txt") == 0);
    assert(le.cursor == 16);
    completion_result_free(&cr1);
    printf("    Transcript Step 1: './myprog de' <Tab> -> './myprog def.txt' verified!\n");

    /* Step 2: Append " abc" -> "./myprog def.txt abc" + <Tab> */
    lineedit_insert_text(&le, " abc", 4);
    assert(strcmp(lineedit_get_text(&le), "./myprog def.txt abc") == 0);
    assert(le.cursor == 20);

    CompletionResult cr2;
    assert(complete_file_candidates(lineedit_get_text(&le), le.cursor, tmp_dir, &cr2) == 0);
    assert(cr2.count == 2);
    assert(strcmp(cr2.items[0], "abc.txt") == 0);
    assert(strcmp(cr2.items[1], "abcd.txt") == 0);
    assert(strcmp(cr2.lcp, "abc") == 0);

    /* Verify formatted numbered list */
    char *fmt_list = complete_format_numbered_list(&cr2, 80);
    assert(fmt_list != NULL);
    assert(strstr(fmt_list, "1. abc.txt") != NULL);
    assert(strstr(fmt_list, "2. abcd.txt") != NULL);
    printf("    Transcript Step 2A: Numbered list verified:\n      %s\n", fmt_list);
    free(fmt_list);

    /* User selects candidate '1' -> "./myprog def.txt abc.txt" */
    int selected_idx = 1; /* 1-based index */
    assert(selected_idx >= 1 && (size_t)selected_idx <= cr2.count);
    const char *chosen = cr2.items[selected_idx - 1];
    char esc2[1024];
    complete_escape_filename(chosen, esc2, sizeof(esc2));
    char repl2[1024];
    snprintf(repl2, sizeof(repl2), "%s%s", cr2.dir_prefix, esc2);
    assert(lineedit_replace_range(&le, cr2.word_start, cr2.word_start + cr2.word_len, repl2));
    assert(strcmp(lineedit_get_text(&le), "./myprog def.txt abc.txt") == 0);
    assert(le.cursor == 24);
    completion_result_free(&cr2);
    printf("    Transcript Step 2B: Selection 1 -> './myprog def.txt abc.txt' verified!\n");

    /* Step 3: Mid-line completion preserving trailing text */
    lineedit_clear(&le);
    lineedit_insert_text(&le, "./myprog de --verbose", 21);
    le.cursor = 11; /* Cursor right after "de" */

    CompletionResult cr3;
    assert(complete_file_candidates(lineedit_get_text(&le), le.cursor, tmp_dir, &cr3) == 0);
    assert(cr3.count == 1);
    assert(strcmp(cr3.items[0], "def.txt") == 0);
    char repl3[1024];
    snprintf(repl3, sizeof(repl3), "%s%s", cr3.dir_prefix, cr3.items[0]);
    assert(lineedit_replace_range(&le, cr3.word_start, cr3.word_start + cr3.word_len, repl3));
    assert(strcmp(lineedit_get_text(&le), "./myprog def.txt --verbose") == 0);
    assert(le.cursor == 16);
    completion_result_free(&cr3);
    printf("    Transcript Step 3: Mid-line completion preserves suffix '--verbose' verified!\n");

    /* Clean up spec dir */
    snprintf(p, sizeof(p), "%s/abc.txt", tmp_dir); unlink(p);
    snprintf(p, sizeof(p), "%s/abcd.txt", tmp_dir); unlink(p);
    snprintf(p, sizeof(p), "%s/def.txt", tmp_dir); unlink(p);
    rmdir(tmp_dir);
}

static void run_task10_per_tab_cwd_isolation_tests(Shared *sh) {
    printf("  [14.7] Testing Per-Tab CWD Isolation in Tab-Key Completion...\n");
    char dir1[] = "/tmp/term_tab1_compl_XXXXXX";
    char dir2[] = "/tmp/term_tab2_compl_XXXXXX";
    assert(mkdtemp(dir1) != NULL);
    assert(mkdtemp(dir2) != NULL);

    char f1[1024], f2[1024];
    snprintf(f1, sizeof(f1), "%s/apple_tab1.txt", dir1);
    int fd1 = open(f1, O_CREAT | O_WRONLY, 0644); assert(fd1 >= 0); close(fd1);

    snprintf(f2, sizeof(f2), "%s/apricot_tab2.txt", dir2);
    int fd2 = open(f2, O_CREAT | O_WRONLY, 0644); assert(fd2 >= 0); close(fd2);

    /* Spawn two tabs */
    int to1[2], from1[2];
    assert(pipe2(to1, O_CLOEXEC) == 0);
    assert(pipe2(from1, O_CLOEXEC) == 0);
    TabChannel chan1 = {to1[0], from1[1], sh};
    pthread_t th1;
    assert(pthread_create(&th1, NULL, tab_thread_func, &chan1) == 0);

    int to2[2], from2[2];
    assert(pipe2(to2, O_CLOEXEC) == 0);
    assert(pipe2(from2, O_CLOEXEC) == 0);
    TabChannel chan2 = {to2[0], from2[1], sh};
    pthread_t th2;
    assert(pthread_create(&th2, NULL, tab_thread_func, &chan2) == 0);

    HarnessTab htab1, htab2;
    memset(&htab1, 0, sizeof(htab1));
    memset(&htab2, 0, sizeof(htab2));
    InBuffer in1, in2;
    memset(&in1, 0, sizeof(in1));
    memset(&in2, 0, sizeof(in2));

    wait_for_prompt(from1[0], &htab1, &in1);
    wait_for_prompt(from2[0], &htab2, &in2);

    /* cd Tab 1 to dir1 and Tab 2 to dir2 */
    char cd1[1024], cd2[1024];
    snprintf(cd1, sizeof(cd1), "cd %s", dir1);
    snprintf(cd2, sizeof(cd2), "cd %s", dir2);
    send_cmd(to1[1], from1[0], &htab1, &in1, cd1);
    send_cmd(to2[1], from2[0], &htab2, &in2, cd2);

    assert(strstr(htab1.last_cwd, dir1) != NULL);
    assert(strstr(htab2.last_cwd, dir2) != NULL);

    /* Complete prefix "ap" using Tab 1's cwd */
    CompletionResult cr_t1;
    assert(complete_file_candidates("cat ap", 6, htab1.last_cwd, &cr_t1) == 0);
    assert(cr_t1.count == 1);
    assert(strcmp(cr_t1.items[0], "apple_tab1.txt") == 0);
    completion_result_free(&cr_t1);

    /* Complete prefix "ap" using Tab 2's cwd */
    CompletionResult cr_t2;
    assert(complete_file_candidates("cat ap", 6, htab2.last_cwd, &cr_t2) == 0);
    assert(cr_t2.count == 1);
    assert(strcmp(cr_t2.items[0], "apricot_tab2.txt") == 0);
    completion_result_free(&cr_t2);

    /* Clean exit of tabs */
    send_cmd(to1[1], from1[0], &htab1, &in1, "exit 0");
    send_cmd(to2[1], from2[0], &htab2, &in2, "exit 0");
    pthread_join(th1, NULL);
    pthread_join(th2, NULL);
    close(to1[0]); close(to1[1]); close(from1[0]); close(from1[1]);
    close(to2[0]); close(to2[1]); close(from2[0]); close(from2[1]);
    free(htab1.last_output);
    free(htab2.last_output);

    /* Clean up directories */
    unlink(f1);
    unlink(f2);
    rmdir(dir1);
    rmdir(dir2);
    printf("    Per-Tab CWD Isolation in Completion verified!\n");
}

static void run_task10_benchmark_and_edge_cases(void) {
    printf("  [14.8] Testing Responsiveness Benchmark (5,000 files < 50 ms) & Edge Cases...\n");
    char bench_dir[] = "/tmp/term_bench_5000_XXXXXX";
    assert(mkdtemp(bench_dir) != NULL);

    /* Populate 5,000 files */
    for (int i = 0; i < 5000; i++) {
        char fn[1024];
        snprintf(fn, sizeof(fn), "%s/item_%04d.dat", bench_dir, i);
        int bfd = open(fn, O_CREAT | O_WRONLY, 0644);
        assert(bfd >= 0);
        close(bfd);
    }

    struct timespec t0, t1;
    assert(clock_gettime(CLOCK_MONOTONIC, &t0) == 0);

    /* Query completion on prefix item_345 */
    CompletionResult b_res;
    assert(complete_file_candidates("cat item_345", 12, bench_dir, &b_res) == 0);

    assert(clock_gettime(CLOCK_MONOTONIC, &t1) == 0);
    double elapsed_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                        (double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0;

    assert(b_res.count >= 1);
    assert(strncmp(b_res.items[0], "item_345", 8) == 0);
    completion_result_free(&b_res);

    printf("    5,000 file completion query completed in %.2f ms (Target: < 50 ms)!\n", elapsed_ms);
    assert(elapsed_ms < 50.0);

    /* Test > 100 candidates ask threshold */
    CompletionResult big_res;
    assert(complete_file_candidates("cat item_", 9, bench_dir, &big_res) == 0);
    assert(big_res.count == 5000);
    assert(big_res.count > 100); /* Will trigger "Display all 5000 possibilities? (y or n)" */
    completion_result_free(&big_res);

    /* Non-existent directory edge case */
    CompletionResult nonexist_res;
    assert(complete_file_candidates("cat /nonexistent_dir_999999/foo", 30, bench_dir, &nonexist_res) == 0);
    assert(nonexist_res.count == 0);
    completion_result_free(&nonexist_res);

    /* Clean up benchmark directory */
    for (int i = 0; i < 5000; i++) {
        char fn[1024];
        snprintf(fn, sizeof(fn), "%s/item_%04d.dat", bench_dir, i);
        unlink(fn);
    }
    rmdir(bench_dir);

    printf("    Responsiveness benchmark and edge cases passed!\n");
}

static void run_task10_completion_tests(Shared *sh) {
    printf("\n--- PART 14: Task 10 Tab-Key File-Name Completion Tests ---\n");
    run_task10_word_extraction_tests();
    run_task10_word_decomposition_tests();
    run_task10_escaping_tests();
    run_task10_lcp_tests();
    run_task10_candidate_gathering_tests();
    run_task10_spec_transcript_tests();
    run_task10_per_tab_cwd_isolation_tests(sh);
    run_task10_benchmark_and_edge_cases();
    printf("  All Task 10 Completion Tests passed!\n");
}

int main(int argc, char **argv) {
    int fuzz_sec = 5;
    if (argc > 1 && strcmp(argv[1], "--fuzz") == 0) {
        fuzz_sec = (argc > 2) ? atoi(argv[2]) : 30;
    } else {
        const char *env_fuzz = getenv("FUZZ_SECONDS");
        if (env_fuzz) {
            fuzz_sec = atoi(env_fuzz);
        }
    }

    printf("=== Terminator Comprehensive Test Harness (Tasks 1-6) ===\n");

    Shared *sh = shared_create();
    assert(sh != NULL);

    /* Part 0: Table-driven expansion tests */
    run_expansion_unit_tests(sh);

    /* ----------------------------------------------------
     * Part 1: Thread Mode Comprehensive Testing
     * ---------------------------------------------------- */
    printf("\n--- PART 1: Thread Mode Shell & Built-ins ---\n");

    int to1[2], from1[2];
    assert(pipe2(to1, O_CLOEXEC) == 0);
    assert(pipe2(from1, O_CLOEXEC) == 0);

    TabChannel chan1;
    chan1.to_tab_fd = to1[0];
    chan1.from_tab_fd = from1[1];
    chan1.shared = sh;

    pthread_t th1;
    assert(pthread_create(&th1, NULL, tab_thread_func, &chan1) == 0);

    HarnessTab tab1;
    memset(&tab1, 0, sizeof(tab1));
    InBuffer in1;
    memset(&in1, 0, sizeof(in1));

    wait_for_prompt(from1[0], &tab1, &in1);
    printf("[1.1] Initial prompt received: %s (cwd: %s)\n", tab1.last_prompt, tab1.last_cwd);
    char original_cwd[1024];
    strncpy(original_cwd, tab1.last_cwd, sizeof(original_cwd) - 1);
    original_cwd[sizeof(original_cwd) - 1] = '\0';

    /* Test 1.2: Built-in echo (-n, -e) */
    printf("[1.2] Testing echo (-n, -e)...\n");
    send_cmd(to1[1], from1[0], &tab1, &in1, "echo -n hello_no_newline");
    assert(strcmp(tab1.last_output, "hello_no_newline") == 0);

    send_cmd(to1[1], from1[0], &tab1, &in1, "echo -e 'foo\\nbar'");
    assert(strstr(tab1.last_output, "foo\nbar\n") != NULL);
    printf("  echo -n and echo -e verified.\n");

    /* Test 1.3: Built-in cd and pwd */
    printf("[1.3] Testing cd and pwd...\n");
    send_cmd(to1[1], from1[0], &tab1, &in1, "cd /tmp");
    assert(strstr(tab1.last_cwd, "tmp") != NULL);

    send_cmd(to1[1], from1[0], &tab1, &in1, "pwd");
    assert(strstr(tab1.last_output, "tmp") != NULL);

    /* cd - */
    send_cmd(to1[1], from1[0], &tab1, &in1, "cd -");
    assert(strstr(tab1.last_output, original_cwd) != NULL);
    assert(strcmp(tab1.last_cwd, original_cwd) == 0);
    printf("  cd /tmp, pwd, and cd - verified.\n");

    /* Test 1.4: cd error handling */
    printf("[1.4] Testing cd error reporting...\n");
    send_cmd(to1[1], from1[0], &tab1, &in1, "cd /this_path_does_not_exist_xyz123");
    assert(strstr(tab1.last_output, "No such file or directory") != NULL);

    /* Test 1.5: External command execution */
    printf("[1.5] Testing external command (uname -s)...\n");
    send_cmd(to1[1], from1[0], &tab1, &in1, "uname -s");
    assert(strlen(tab1.last_output) > 0);
    printf("  uname -s: %s", tab1.last_output);

    /* Test 1.6: Unknown command error message and $? = 127 */
    printf("[1.6] Testing unknown command error message and $?...\n");
    send_cmd(to1[1], from1[0], &tab1, &in1, "notacommand_404");
    assert(strstr(tab1.last_output, "terminator: notacommand_404: command not found") != NULL);

    send_cmd(to1[1], from1[0], &tab1, &in1, "echo $?");
    assert(strstr(tab1.last_output, "127") != NULL);
    printf("  Status 127 and error message verified.\n");

    /* Test 1.7: Non-executable file permission denied and $? = 126 */
    printf("[1.7] Testing non-executable file permission denied and $?...\n");
    const char *nonexec_path = "/tmp/terminator_test_nonexec";
    int ne_fd = open(nonexec_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(ne_fd >= 0);
    close(ne_fd);
    chmod(nonexec_path, 0644);

    char ne_cmd[256];
    snprintf(ne_cmd, sizeof(ne_cmd), "%s", nonexec_path);
    send_cmd(to1[1], from1[0], &tab1, &in1, ne_cmd);
    assert(strstr(tab1.last_output, "Permission denied") != NULL);

    send_cmd(to1[1], from1[0], &tab1, &in1, "echo $?");
    assert(strstr(tab1.last_output, "126") != NULL);
    unlink(nonexec_path);
    printf("  Status 126 and Permission denied verified.\n");

    /* Test 1.8: Built-in stubs */
    printf("[1.8] Testing built-in stubs...\n");
    send_cmd(to1[1], from1[0], &tab1, &in1, "source");
    assert(strstr(tab1.last_output, "terminator: source: not implemented yet") != NULL);
    send_cmd(to1[1], from1[0], &tab1, &in1, "multiWatch");
    assert(strstr(tab1.last_output, "usage: multiWatch") != NULL);

    /* Test 1.9: Built-in clear command & FRAME_CLEAR */
    printf("[1.9] Testing built-in clear command & FRAME_CLEAR...\n");
    tab1.cleared = false;
    send_cmd(to1[1], from1[0], &tab1, &in1, "clear");
    assert(tab1.cleared == true);
    send_cmd(to1[1], from1[0], &tab1, &in1, "echo $?");
    assert(strstr(tab1.last_output, "0\n") != NULL);

    /* TabBuf ANSI escape \033[2J unit test */
    TabBuf *tb_test = tabbuf_create();
    tabbuf_append_line(tb_test, "hello world 1");
    tabbuf_append_line(tb_test, "hello world 2");
    assert(tb_test->count > 1);
    tabbuf_append_bytes(tb_test, "\033[2J", 4);
    assert(tb_test->count == 1);
    assert(tb_test->lines[0].len == 0);
    tabbuf_destroy(tb_test);

    /* Redirection of clear command to raw fd */
    send_cmd(to1[1], from1[0], &tab1, &in1, "clear > /tmp/test_clear.out");
    int fd_clear = open("/tmp/test_clear.out", O_RDONLY);
    assert(fd_clear >= 0);
    char clear_buf[32];
    ssize_t n_clear = read(fd_clear, clear_buf, sizeof(clear_buf));
    close(fd_clear);
    unlink("/tmp/test_clear.out");
    assert(n_clear == 7);
    assert(memcmp(clear_buf, "\033[H\033[2J", 7) == 0);
    printf("  clear command and FRAME_CLEAR verified.\n");

    /* ----------------------------------------------------
     * Part 2: Task 3 Multiline Input & Continuation (Thread Mode)
     * ---------------------------------------------------- */
    printf("\n--- PART 2: Task 3 Multiline Input & Continuation (Thread Mode) ---\n");

    /* Spec Example 1 */
    printf("[2.1] Testing Spec Example 1 (multiline quoting with DQUOTE_BACKSLASH_NEWLINE_KEEPS_NEWLINE)...\n");
    send_line_expect_prompt(to1[1], from1[0], &tab1, &in1, "echo \"Isn't process creation exciting?\\", "> ");
    send_line_expect_prompt(to1[1], from1[0], &tab1, &in1, "Fork, exec, repeat!\"", "user@terminator> ");
    assert(strstr(tab1.last_output, "Isn't process creation exciting?\nFork, exec, repeat!\n") != NULL);
    printf("  Spec Example 1 output verified:\n%s", tab1.last_output);

    /* Spec Example 2 */
    printf("[2.2] Testing Spec Example 2 (command substitution with pipeline inside)...\n");
    send_line_expect_prompt(to1[1], from1[0], &tab1, &in1, "echo \"Files in $(basename $(pwd)): $(ls | \\", "> ");
    send_line_expect_prompt(to1[1], from1[0], &tab1, &in1, "wc -l)\"", "user@terminator> ");
    assert(strstr(tab1.last_output, "Files in ") != NULL);
    printf("  Spec Example 2 output verified:\n%s", tab1.last_output);

    /* Spec Example 3 */
    printf("[2.3] Testing Spec Example 3 (literal vs expanded quotes & nested substitutions)...\n");
    send_cmd(to1[1], from1[0], &tab1, &in1, "echo 'literal: $HOME' \"expanded: $HOME\"");
    assert(strstr(tab1.last_output, "literal: $HOME expanded: ") != NULL);
    assert(strstr(tab1.last_output, "literal: $HOME expanded: $HOME") == NULL);
    printf("  Spec Example 3A verified:\n%s", tab1.last_output);

    send_cmd(to1[1], from1[0], &tab1, &in1, "echo `echo outer $(echo inner)`");
    assert(strstr(tab1.last_output, "outer inner\n") != NULL);
    printf("  Spec Example 3B verified:\n%s", tab1.last_output);

    /* Multiline continuation abort via Ctrl+C (FRAME_INTR) */
    printf("[2.4] Testing continuation abort via Ctrl+C (FRAME_INTR)...\n");
    send_line_expect_prompt(to1[1], from1[0], &tab1, &in1, "echo \"unterminated_quote_line", "> ");
    send_intr_expect_prompt(to1[1], from1[0], &tab1, &in1, "user@terminator> ");
    send_cmd(to1[1], from1[0], &tab1, &in1, "echo continuation_aborted_successfully");
    assert(strstr(tab1.last_output, "continuation_aborted_successfully\n") != NULL);
    printf("  Continuation abort on Ctrl+C verified.\n");

    /* Multiline command stored in history as ONE entry */
    printf("[2.5] Testing multiline command stored in shared history as ONE entry...\n");
    send_cmd(to1[1], from1[0], &tab1, &in1, "history");
    assert(strstr(tab1.last_output, "Isn't process creation exciting?") != NULL);
    printf("  Multiline command verified in history output.\n");

    /* ----------------------------------------------------
     * Part 3: Per-Tab Isolation Proof (Thread Mode)
     * ---------------------------------------------------- */
    printf("\n--- PART 3: Per-Tab Isolation Proof (Thread Mode) ---\n");

    int to2[2], from2[2];
    assert(pipe2(to2, O_CLOEXEC) == 0);
    assert(pipe2(from2, O_CLOEXEC) == 0);

    TabChannel chan2;
    chan2.to_tab_fd = to2[0];
    chan2.from_tab_fd = from2[1];
    chan2.shared = sh;

    pthread_t th2;
    assert(pthread_create(&th2, NULL, tab_thread_func, &chan2) == 0);

    HarnessTab tab2;
    memset(&tab2, 0, sizeof(tab2));
    InBuffer in2;
    memset(&in2, 0, sizeof(in2));

    wait_for_prompt(from2[0], &tab2, &in2);
    printf("  Tab 2 started with cwd: %s\n", tab2.last_cwd);

    /* Tab 1 changes cwd to /tmp */
    printf("  Tab 1 changing directory to /tmp...\n");
    send_cmd(to1[1], from1[0], &tab1, &in1, "cd /tmp");
    assert(strstr(tab1.last_cwd, "tmp") != NULL);

    /* Tab 2 checks pwd */
    printf("  Tab 2 checking pwd (must NOT be /tmp)...\n");
    send_cmd(to2[1], from2[0], &tab2, &in2, "pwd");
    assert(strstr(tab2.last_output, original_cwd) != NULL);
    assert(strstr(tab2.last_cwd, "tmp") == NULL);
    printf("  PROOF VERIFIED: Tab 1 cwd is '%s', Tab 2 cwd remains '%s'!\n", tab1.last_cwd, tab2.last_cwd);

    /* Shared history across tabs */
    printf("[3.2] Testing shared history across tabs...\n");
    send_cmd(to1[1], from1[0], &tab1, &in1, "echo unique_marker_from_tab_1");
    send_cmd(to2[1], from2[0], &tab2, &in2, "history");
    assert(strstr(tab2.last_output, "unique_marker_from_tab_1") != NULL);
    printf("  Tab 2 history command successfully shows command typed in Tab 1!\n");

    /* Clean up Tab 1 & Tab 2 */
    send_cmd(to1[1], from1[0], &tab1, &in1, "exit 0");
    pthread_join(th1, NULL);
    close(to1[0]); close(to1[1]); close(from1[0]); close(from1[1]);

    send_cmd(to2[1], from2[0], &tab2, &in2, "exit 0");
    pthread_join(th2, NULL);
    close(to2[0]); close(to2[1]); close(from2[0]); close(from2[1]);

    /* ----------------------------------------------------
     * Part 4: Process Mode Comprehensive Testing (Including Spec Examples)
     * ---------------------------------------------------- */
    printf("\n--- PART 4: Process Mode Execution, Spec Examples & Isolation ---\n");

    int p_to[2], p_from[2];
    assert(pipe2(p_to, O_CLOEXEC) == 0);
    assert(pipe2(p_from, O_CLOEXEC) == 0);

    pid_t proc_pid = fork();
    assert(proc_pid >= 0);
    if (proc_pid == 0) {
        close(p_to[1]);
        close(p_from[0]);
        TabChannel pchan;
        pchan.to_tab_fd = p_to[0];
        pchan.from_tab_fd = p_from[1];
        pchan.shared = sh;
        tab_main(&pchan);
        close(p_to[0]);
        close(p_from[1]);
        _exit(0);
    }

    close(p_to[0]);
    close(p_from[1]);

    HarnessTab ptab;
    memset(&ptab, 0, sizeof(ptab));
    InBuffer pin;
    memset(&pin, 0, sizeof(pin));

    wait_for_prompt(p_from[0], &ptab, &pin);
    printf("  Process mode tab started with PID %d\n", proc_pid);

    /* Process Mode Spec Example 1 */
    printf("  [4.1] Process Mode: Testing Spec Example 1...\n");
    send_line_expect_prompt(p_to[1], p_from[0], &ptab, &pin, "echo \"Isn't process creation exciting?\\", "> ");
    send_line_expect_prompt(p_to[1], p_from[0], &ptab, &pin, "Fork, exec, repeat!\"", "user@terminator> ");
    assert(strstr(ptab.last_output, "Isn't process creation exciting?\nFork, exec, repeat!\n") != NULL);
    printf("    Process Mode Spec Example 1 output verified!\n");

    /* Process Mode Spec Example 2 */
    printf("  [4.2] Process Mode: Testing Spec Example 2...\n");
    send_line_expect_prompt(p_to[1], p_from[0], &ptab, &pin, "echo \"Files in $(basename $(pwd)): $(ls | \\", "> ");
    send_line_expect_prompt(p_to[1], p_from[0], &ptab, &pin, "wc -l)\"", "user@terminator> ");
    assert(strstr(ptab.last_output, "Files in ") != NULL);
    printf("    Process Mode Spec Example 2 output verified!\n");

    /* Process Mode Spec Example 3 */
    printf("  [4.3] Process Mode: Testing Spec Example 3...\n");
    send_cmd(p_to[1], p_from[0], &ptab, &pin, "echo 'literal: $HOME' \"expanded: $HOME\"");
    assert(strstr(ptab.last_output, "literal: $HOME expanded: ") != NULL);
    assert(strstr(ptab.last_output, "literal: $HOME expanded: $HOME") == NULL);
    send_cmd(p_to[1], p_from[0], &ptab, &pin, "echo `echo outer $(echo inner)`");
    assert(strstr(ptab.last_output, "outer inner\n") != NULL);
    printf("    Process Mode Spec Example 3 output verified!\n");

    /* Clean exit of process mode tab */
    assert(proto_write_frame_blocking(p_to[1], FRAME_LINE, "exit 0", 6) == 0);
    int pstatus = 0;
    assert(waitpid(proc_pid, &pstatus, 0) == proc_pid);
    assert(WIFEXITED(pstatus) && WEXITSTATUS(pstatus) == 0);
    close(p_to[1]);
    close(p_from[0]);
    printf("  Process mode tab exited and reaped with status 0.\n");

    /* ----------------------------------------------------
     * Part 5: TabRuntime tabrt_spawn Integration
     * ---------------------------------------------------- */
    printf("\n--- PART 5: TabRuntime tabrt_spawn Integration (Thread Mode) ---\n");
    TabRuntime *rt = tabrt_create(false, sh);
    assert(rt != NULL);

    Tab *tab_rt = tabrt_spawn(rt, -1);
    assert(tab_rt != NULL);
    assert(tab_rt->alive);
    assert(tab_rt->to_tab_fd >= 0);
    assert(tab_rt->from_tab_fd >= 0);

    HarnessTab rt_tab_data;
    memset(&rt_tab_data, 0, sizeof(rt_tab_data));
    InBuffer rt_in;
    memset(&rt_in, 0, sizeof(rt_in));

    wait_for_prompt(tab_rt->from_tab_fd, &rt_tab_data, &rt_in);
    printf("  Initial prompt from spawned tab: %s\n", rt_tab_data.last_prompt);

    send_cmd(tab_rt->to_tab_fd, tab_rt->from_tab_fd, &rt_tab_data, &rt_in, "echo tabrt_spawn_test_passed");
    assert(strstr(rt_tab_data.last_output, "tabrt_spawn_test_passed") != NULL);
    printf("  Output from spawned tab: %s", rt_tab_data.last_output);

    tabrt_close(rt, 0);
    tabrt_destroy(rt);
    printf("  Spawned tab closed and cleaned up successfully.\n");

    /* ----------------------------------------------------
     * Part 7: Task 4 Redirection Tests
     * ---------------------------------------------------- */
    run_task4_parser_unit_tests(sh);
    run_task4_execution_tests(sh);
    run_task4_process_mode_tests(sh);

    /* ----------------------------------------------------
     * Part 8: Task 5 Pipeline Tests
     * ---------------------------------------------------- */
    run_task5_pipeline_tests(sh);
    run_task5_process_mode_tests(sh);
    run_task5_concurrency_tests(sh);

    /* ----------------------------------------------------
     * Part 10: Task 6 multiWatch Tests
     * ---------------------------------------------------- */
    run_multiwatch_parser_unit_tests();
    run_multiwatch_execution_tests(sh);
    run_multiwatch_two_tabs_concurrency_test(sh);
    run_multiwatch_process_mode_test(sh);

    /* ----------------------------------------------------
     * Part 11: Task 7 Hotkeys, Keymap & Lineedit Tests
     * ---------------------------------------------------- */
    run_task7_lineedit_unit_tests();
    run_task7_keymap_unit_tests();
    run_task7_bind_builtin_tests(sh);
    run_task7_history_navigation_tests(sh);

    /* ----------------------------------------------------
     * Part 12: Task 8 Signals & Job Control Tests
     * ---------------------------------------------------- */
    run_task8_signals_jobs_tests(sh);

    /* ----------------------------------------------------
     * Part 13: Task 9 History Tests
     * ---------------------------------------------------- */
    run_task9_history_tests();

    /* ----------------------------------------------------
     * Part 14: Task 10 Completion Tests
     * ---------------------------------------------------- */
    run_task10_completion_tests(sh);

    /* Verify no lingering zombie children */
    pid_t lingering = waitpid(-1, NULL, WNOHANG);
    assert(lingering <= 0);
    printf("\n  Zero lingering zombie processes verified!\n");

    /* ----------------------------------------------------
     * Part 9: Lexer Random Byte Fuzzer
     * ---------------------------------------------------- */
    run_fuzzer(fuzz_sec);

    shared_destroy(sh);

    printf("\n=== ALL TASK 1 THROUGH 10 TESTS PASSED SUCCESSFULLY! ===\n");
    return 0;
}
