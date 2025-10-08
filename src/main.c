#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/input.h>
#include <limits.h>
#include <time.h>

#include "exec.h"
#include "state.h"
#include "util.h"

static volatile sig_atomic_t g_should_stop = 0;

typedef struct {
    struct input_event *items;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    bool shutdown;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    clockid_t clock_id;
} EventQueue;

typedef enum {
    QUEUE_WAIT_EVENT,
    QUEUE_WAIT_TIMEOUT,
    QUEUE_WAIT_SHUTDOWN,
} QueueWaitResult;

static int read_full(int fd, void *buffer, size_t len, ssize_t *out_total) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, (char *)buffer + total, len - total);
        if (n == 0) {
            if (out_total) *out_total = (ssize_t)total;
            return 1; /* EOF before requested length */
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)n;
    }
    if (out_total) *out_total = (ssize_t)total;
    return 0;
}

static int write_full(int fd, const void *buffer, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, (const char *)buffer + total, len - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static void event_queue_init(EventQueue *queue) {
    queue->capacity = 64;
    queue->items = calloc(queue->capacity, sizeof(*queue->items));
    if (!queue->items) {
        perror("calloc");
        exit(1);
    }
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->shutdown = false;
    queue->clock_id = CLOCK_REALTIME;
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        perror("pthread_mutex_init");
        exit(1);
    }
    pthread_condattr_t attr;
    pthread_condattr_t *attr_ptr = NULL;
    bool attr_inited = (pthread_condattr_init(&attr) == 0);
    if (attr_inited) {
        if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0) {
            queue->clock_id = CLOCK_MONOTONIC;
            attr_ptr = &attr;
        }
    }
    if (pthread_cond_init(&queue->cond, attr_ptr) != 0) {
        perror("pthread_cond_init");
        exit(1);
    }
    if (attr_inited) {
        pthread_condattr_destroy(&attr);
    }
}

static void event_queue_destroy(EventQueue *queue) {
    free(queue->items);
    pthread_cond_destroy(&queue->cond);
    pthread_mutex_destroy(&queue->mutex);
}

static void event_queue_grow(EventQueue *queue) {
    size_t new_cap = queue->capacity ? queue->capacity * 2 : 64;
    struct input_event *tmp = calloc(new_cap, sizeof(*tmp));
    if (!tmp) {
        perror("calloc");
        exit(1);
    }
    for (size_t i = 0; i < queue->count; ++i) {
        size_t idx = (queue->head + i) % queue->capacity;
        tmp[i] = queue->items[idx];
    }
    free(queue->items);
    queue->items = tmp;
    queue->capacity = new_cap;
    queue->head = 0;
    queue->tail = queue->count;
}

static void event_queue_push(EventQueue *queue, const struct input_event *event) {
    if (!event) return;
    pthread_mutex_lock(&queue->mutex);
    if (queue->shutdown) {
        pthread_mutex_unlock(&queue->mutex);
        return;
    }
    if (queue->count == queue->capacity) {
        event_queue_grow(queue);
    }
    queue->items[queue->tail] = *event;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
}

static void event_queue_shutdown(EventQueue *queue) {
    pthread_mutex_lock(&queue->mutex);
    queue->shutdown = true;
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
}

static void compute_future_timespec(clockid_t clock_id, struct timespec *ts, int timeout_ms) {
    clock_gettime(clock_id, ts);
    ts->tv_sec += timeout_ms / 1000;
    ts->tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static QueueWaitResult event_queue_wait_pop(EventQueue *queue, struct input_event *out, int timeout_ms) {
    pthread_mutex_lock(&queue->mutex);
    bool use_timeout = timeout_ms >= 0;
    struct timespec deadline;
    if (use_timeout) {
        compute_future_timespec(queue->clock_id, &deadline, timeout_ms);
    }
    while (queue->count == 0 && !queue->shutdown) {
        if (!use_timeout) {
            pthread_cond_wait(&queue->cond, &queue->mutex);
            continue;
        }
        int rc = pthread_cond_timedwait(&queue->cond, &queue->mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&queue->mutex);
            return QUEUE_WAIT_TIMEOUT;
        }
        if (rc != 0) {
            continue;
        }
    }

    if (queue->count > 0) {
        *out = queue->items[queue->head];
        queue->head = (queue->head + 1) % queue->capacity;
        queue->count--;
        pthread_mutex_unlock(&queue->mutex);
        return QUEUE_WAIT_EVENT;
    }

    bool shutting_down = queue->shutdown;
    pthread_mutex_unlock(&queue->mutex);
    return shutting_down ? QUEUE_WAIT_SHUTDOWN : QUEUE_WAIT_TIMEOUT;
}

typedef struct {
    State *state;
    EventQueue *queue;
} WorkerArgs;

static void *state_worker_thread(void *userdata) {
    WorkerArgs *args = userdata;
    State *state = args->state;
    EventQueue *queue = args->queue;
    free(args);

    for (;;) {
        int timeout_ms = state_poll_timeout_ms(state);
        struct input_event ev;
        QueueWaitResult result = event_queue_wait_pop(queue, &ev, timeout_ms);

        if (result == QUEUE_WAIT_EVENT) {
            state_process_input(state, &ev);
            state_flush_idle(state, false);
            continue;
        }
        if (result == QUEUE_WAIT_TIMEOUT) {
            state_flush_idle(state, false);
            continue;
        }
        if (result == QUEUE_WAIT_SHUTDOWN) {
            break;
        }
    }

    state_flush_idle(state, true);
    return NULL;
}

static void handle_signal(int sig) {
    (void)sig;
    g_should_stop = 1;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [--data-dir DIR] [--log-dir DIR] [--snapshot-dir DIR] [--snapshot-interval SEC]\n"
            "           [--clipboard auto|off] [--context-refresh SEC] [--context hyprland|none]\n"
            "           [--log-mode events|snapshots|both] [--translate xkb|raw]\n"
            "           [--xkb-layout LAYOUT] [--xkb-variant VARIANT]\n"
            "           [--hyprctl CMD] [--hypr-signature PATH] [--hypr-user USER]\n",
            prog);
}

int main(int argc, char **argv) {
    const char *data_dir = "/realm/data/keylog";
    const char *log_dir = NULL;
    const char *snapshot_dir = NULL;
    const char *hyprctl_cmd = "hyprctl";
    double snapshot_interval = 5.0;
    double context_refresh = 0.4;
    enum ClipboardMode clipboard_mode = CLIPBOARD_AUTO;
    bool context_enabled = true;
    enum TranslateMode translate_mode = TRANSLATE_XKB;
    enum LogMode log_mode = LOG_MODE_BOTH;
    const char *xkb_layout = NULL;
    const char *xkb_variant = NULL;
    const char *hypr_signature_path = NULL;
    const char *hypr_user = NULL;

    char log_dir_buf[PATH_MAX] = {0};
    char snapshot_dir_buf[PATH_MAX] = {0};

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--log-dir") == 0 && i + 1 < argc) {
            log_dir = argv[++i];
        } else if (strcmp(argv[i], "--snapshot-dir") == 0 && i + 1 < argc) {
            snapshot_dir = argv[++i];
        } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (strcmp(argv[i], "--snapshot-interval") == 0 && i + 1 < argc) {
            snapshot_interval = atof(argv[++i]);
        } else if (strcmp(argv[i], "--context-refresh") == 0 && i + 1 < argc) {
            context_refresh = atof(argv[++i]);
        } else if (strcmp(argv[i], "--clipboard") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "auto") == 0) {
                clipboard_mode = CLIPBOARD_AUTO;
            } else if (strcmp(mode, "off") == 0) {
                clipboard_mode = CLIPBOARD_OFF;
            } else {
                fprintf(stderr, "Invalid clipboard mode: %s\n", mode);
                return 1;
            }
        } else if (strcmp(argv[i], "--hyprctl") == 0 && i + 1 < argc) {
            hyprctl_cmd = argv[++i];
        } else if (strcmp(argv[i], "--context") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "hyprland") == 0) {
                context_enabled = true;
            } else if (strcmp(mode, "none") == 0) {
                context_enabled = false;
            } else {
                fprintf(stderr, "Invalid context mode: %s\n", mode);
                return 1;
            }
        } else if (strcmp(argv[i], "--log-mode") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "events") == 0) {
                log_mode = LOG_MODE_EVENTS;
            } else if (strcmp(mode, "snapshots") == 0) {
                log_mode = LOG_MODE_SNAPSHOTS;
            } else if (strcmp(mode, "both") == 0) {
                log_mode = LOG_MODE_BOTH;
            } else {
                fprintf(stderr, "Invalid log mode: %s\n", mode);
                return 1;
            }
        } else if (strcmp(argv[i], "--translate") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "xkb") == 0) {
                translate_mode = TRANSLATE_XKB;
            } else if (strcmp(mode, "raw") == 0) {
                translate_mode = TRANSLATE_RAW;
            } else {
                fprintf(stderr, "Invalid translate mode: %s\n", mode);
                return 1;
            }
        } else if (strcmp(argv[i], "--xkb-layout") == 0 && i + 1 < argc) {
            xkb_layout = argv[++i];
        } else if (strcmp(argv[i], "--xkb-variant") == 0 && i + 1 < argc) {
            xkb_variant = argv[++i];
        } else if (strcmp(argv[i], "--hypr-signature") == 0 && i + 1 < argc) {
            hypr_signature_path = argv[++i];
        } else if (strcmp(argv[i], "--hypr-user") == 0 && i + 1 < argc) {
            hypr_user = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!log_dir) {
        int written = snprintf(log_dir_buf, sizeof(log_dir_buf), "%s/logs", data_dir);
        if (written < 0 || written >= (int)sizeof(log_dir_buf)) {
            fprintf(stderr, "log dir path too long\n");
            return 1;
        }
        log_dir = log_dir_buf;
    }

    if (!snapshot_dir) {
        int written = snprintf(snapshot_dir_buf, sizeof(snapshot_dir_buf), "%s/snapshots", data_dir);
        if (written < 0 || written >= (int)sizeof(snapshot_dir_buf)) {
            fprintf(stderr, "snapshot dir path too long\n");
            return 1;
        }
        snapshot_dir = snapshot_dir_buf;
    }

    util_ensure_dir_tree(data_dir);
    util_ensure_dir_tree(log_dir);
    util_ensure_dir_tree(snapshot_dir);

    StateConfig config = {
        .log_dir = log_dir,
        .snapshot_dir = snapshot_dir,
        .hyprctl_cmd = hyprctl_cmd,
        .snapshot_interval = snapshot_interval,
        .context_refresh = context_refresh,
        .clipboard_mode = clipboard_mode,
        .translate_mode = translate_mode,
        .log_mode = log_mode,
        .context_enabled = context_enabled,
        .xkb_layout = xkb_layout,
        .xkb_variant = xkb_variant,
        .hypr_signature_path = hypr_signature_path,
        .hypr_user = hypr_user,
    };

    CommandExecutor executor;
    command_executor_init_default(&executor);

    State state;
    state_init(&state, &config, &executor);

    EventQueue queue;
    event_queue_init(&queue);

    WorkerArgs *args = malloc(sizeof(*args));
    if (!args) {
        perror("malloc");
        state_cleanup(&state);
        event_queue_destroy(&queue);
        return 1;
    }
    args->state = &state;
    args->queue = &queue;

    pthread_t worker_thread;
    if (pthread_create(&worker_thread, NULL, state_worker_thread, args) != 0) {
        perror("pthread_create");
        free(args);
        event_queue_destroy(&queue);
        state_cleanup(&state);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int poll_timeout = -1;
    struct pollfd pfd = {
        .fd = STDIN_FILENO,
        .events = POLLIN,
    };

    while (!g_should_stop) {
        int rc = poll(&pfd, 1, poll_timeout);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }
        if (rc == 0) {
            continue;
        }

        bool saw_hup = (pfd.revents & POLLHUP) != 0;
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            break;
        }

        if (pfd.revents & POLLIN) {
            struct input_event ev;
            ssize_t total = 0;
            int read_rc = read_full(STDIN_FILENO, &ev, sizeof(ev), &total);
            if (read_rc == 0) {
                /* full event read */
            } else if (read_rc == 1) {
                if (total == 0) {
                    break;
                }
                fprintf(stderr, "short read from stdin\n");
                break;
            } else {
                perror("read");
                break;
            }

            event_queue_push(&queue, &ev);

            if (write_full(STDOUT_FILENO, &ev, sizeof(ev)) != 0) {
                perror("write");
                break;
            }
        }

        if (saw_hup && !(pfd.revents & POLLIN)) {
            break;
        }
    }

    event_queue_shutdown(&queue);
    pthread_join(worker_thread, NULL);
    event_queue_destroy(&queue);
    state_cleanup(&state);
    return 0;
}
