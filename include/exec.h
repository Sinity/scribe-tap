#ifndef EXEC_H
#define EXEC_H

typedef char *(*command_runner_fn)(const char *const *argv, void *userdata);

typedef struct CommandExecutor {
    command_runner_fn run;
    void *userdata;
} CommandExecutor;

void command_executor_init_default(CommandExecutor *exec);
char *command_executor_capture(CommandExecutor *exec, const char *const *argv);

#endif /* EXEC_H */
