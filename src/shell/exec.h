#ifndef SHELL_EXEC_H
#define SHELL_EXEC_H

#include "shell.h"
#include "cmd.h"

/* Initialize the process-wide SIGCHLD handler */
void sigchld_init(void);

/* Register / unregister a tab's wake-pipe write end */
void wake_pipe_register(int write_fd);
void wake_pipe_unregister(int write_fd);

/* Apply redirections for a child process. Returns false on open/dup failure. */
bool apply_cmd_redirections(Cmd *cmd, char *err_msg, size_t err_size);

/* Execute a single command or pipeline within the tab shell context */
int shell_execute_cmd(Shell *sh, Cmd *cmd, int from_tab_fd, int to_tab_fd);
int shell_execute_pipeline(Shell *sh, Pipeline *pl, int from_tab_fd, int to_tab_fd);
int job_wait_foreground(Shell *sh, Job *j, int from_tab_fd, int to_tab_fd);

#endif /* SHELL_EXEC_H */
