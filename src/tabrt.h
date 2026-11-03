#ifndef TABRT_H
#define TABRT_H

#include <stdbool.h>
#include <pthread.h>
#include <sys/types.h>
#include "proto.h"
#include "shared.h"
#include "tabbuf.h"
#include "lineedit.h"
#include "complete.h"

#define MAX_TABS 8

typedef struct {
    int to_tab_fd;   /* Tab reads GUI commands from this fd */
    int from_tab_fd; /* Tab writes frames to GUI on this fd */
    Shared *shared;
} TabChannel;

typedef struct Tab {
    int id;               /* Tab ID (1, 2, ...) */
    bool alive;           /* True if tab process/thread is still running */
    bool should_close;    /* True if tab shell exited voluntarily */
    bool process_exited_printed; /* True if [process exited] was printed */
    pid_t pid;            /* Child PID in process mode (0 in thread mode) */
    pthread_t thread;     /* Pthread in thread mode */
    int to_tab_fd;        /* GUI write fd (nonblocking) */
    int from_tab_fd;      /* GUI read fd (nonblocking) */
    OutQueue pending_out; /* Pending outbound frames to tab */
    InBuffer in_buf;      /* Inbound frame buffer from tab */
    TabBuf *buf;          /* Scrollback text buffer */
    LineEdit input;       /* Live input buffer */
    char cwd[1024];       /* Tab's current working directory */
    char prompt[256];     /* Tab's prompt string */
    char label[128];      /* Formatted tab title for the tab strip */

    /* History browsing state */
    bool hist_browsing;
    uint64_t hist_seq;
    char saved_input[LINEEDIT_MAX];

    /* Search mode state (Task 9 Ctrl+R) */
    bool search_mode;
    char saved_prompt[256];
    char saved_input_before_search[LINEEDIT_MAX];

    /* Tab completion state (Task 10) */
    CompletionState compl_state;
    CompletionResult compl_result;
    char compl_digits[32];
    size_t compl_digits_len;
} Tab;

typedef struct {
    bool use_process;
    Shared *shared;
    Tab *tabs[MAX_TABS];
    int tab_count;
    int active_idx;
    int next_tab_id;
} TabRuntime;

TabRuntime *tabrt_create(bool use_process, Shared *shared);
void tabrt_destroy(TabRuntime *rt);

Tab *tabrt_spawn(TabRuntime *rt, int x_conn_fd);
bool tabrt_close(TabRuntime *rt, int tab_idx);

void tab_main(TabChannel *chan);
void tab_update_label(Tab *tab);

#endif /* TABRT_H */
