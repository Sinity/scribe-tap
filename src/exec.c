#include "exec.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static char *default_runner(const char *const *argv, void *userdata) {
    (void)userdata;
    if (!argv || !argv[0]) {
        return NULL;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    if (pid == 0) {
        if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
            _exit(127);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(pipefd[1]);

    size_t cap = 1024;
    size_t len = 0;
    char *data = malloc(cap);
    if (!data) {
        close(pipefd[0]);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        }
        return NULL;
    }

    bool error = false;
    for (;;) {
        char buf[512];
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n > 0) {
            if (len + (size_t)n + 1 > cap) {
                size_t new_cap = cap * 2;
                while (len + (size_t)n + 1 > new_cap) {
                    new_cap *= 2;
                }
                char *tmp = realloc(data, new_cap);
                if (!tmp) {
                    error = true;
                    break;
                }
                data = tmp;
                cap = new_cap;
            }
            memcpy(data + len, buf, (size_t)n);
            len += (size_t)n;
        } else if (n == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            error = true;
            break;
        }
    }

    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        error = true;
        break;
    }

    if (!error && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
        error = true;
    }

    if (error) {
        free(data);
        return NULL;
    }

    if (len + 1 > cap) {
        char *tmp = realloc(data, len + 1);
        if (!tmp) {
            free(data);
            return NULL;
        }
        data = tmp;
    }

    data[len] = '\0';
    return data;
}

void command_executor_init_default(CommandExecutor *exec) {
    exec->run = default_runner;
    exec->userdata = NULL;
}

char *command_executor_capture(CommandExecutor *exec, const char *const *argv) {
    if (!exec || !exec->run) {
        return NULL;
    }
    return exec->run(argv, exec->userdata);
}
