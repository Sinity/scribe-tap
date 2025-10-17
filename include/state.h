#ifndef STATE_H
#define STATE_H

#include <linux/input.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <limits.h>
#ifdef __linux__
#include <linux/limits.h>
#endif

#if __has_include(<xkbcommon/xkbcommon.h>)
#include <xkbcommon/xkbcommon.h>
#define STATE_HAVE_XKBCOMMON 1
#else
#define STATE_HAVE_XKBCOMMON 0
struct xkb_context;
struct xkb_keymap;
struct xkb_state;
#endif

#include "buffer.h"
#include "exec.h"

enum ClipboardMode {
    CLIPBOARD_AUTO,
    CLIPBOARD_OFF,
};

enum TranslateMode {
    TRANSLATE_XKB,
    TRANSLATE_RAW,
};

enum LogMode {
    LOG_MODE_EVENTS,
    LOG_MODE_SNAPSHOTS,
    LOG_MODE_BOTH,
};

typedef struct StateConfig {
    const char *log_dir;
    const char *snapshot_dir;
    const char *hyprctl_cmd;
    double snapshot_interval;
    double context_refresh;
    enum ClipboardMode clipboard_mode;
    enum TranslateMode translate_mode;
    enum LogMode log_mode;
    bool context_enabled;
    const char *xkb_layout;
    const char *xkb_variant;
    const char *hypr_signature_path;
    const char *hypr_user;
} StateConfig;

enum { STATE_MOD_COUNT = 4 };

typedef struct State {
    char session_id[64];
    char log_dir[PATH_MAX];
    char snapshot_dir[PATH_MAX];
    char hyprctl_cmd[PATH_MAX];
    double snapshot_interval;
    double context_refresh;
    enum ClipboardMode clipboard_mode;
    enum TranslateMode translate_mode;
    enum LogMode log_mode;
    bool context_enabled;
    const char *xkb_layout;
    const char *xkb_variant;

    FILE *log_file;
    int log_year;
    int log_month;
    int log_day;
    BufferList buffers;
    char current_context[512];
    double last_context_poll;

    bool capslock;
    bool modifiers[STATE_MOD_COUNT];
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    char *hypr_signature;
    CommandExecutor *executor;
} State;

void state_init(State *state, const StateConfig *config, CommandExecutor *executor);
void state_cleanup(State *state);
void state_flush_idle(State *state, bool force_all);
void state_process_input(State *state, const struct input_event *event);
int state_poll_timeout_ms(const State *state);

#endif /* STATE_H */
