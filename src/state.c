#define _GNU_SOURCE
#include "state.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if STATE_HAVE_XKBCOMMON
#include <xkbcommon/xkbcommon.h>
#endif

#include "util.h"

enum ModifierIndex {
    MOD_SHIFT = 0,
    MOD_CTRL,
    MOD_ALT,
    MOD_SUPER,
    MOD_COUNT = STATE_MOD_COUNT
};

static void log_event(State *state, const char *event, bool flush,
                      const char *keycode, bool changed, const char *buffer_text,
                      const char *clipboard_text, const struct input_event *input);
static void log_pointer_event(State *state, const char *event, const char *code_name,
                              int value, bool flush);
static void write_snapshot(State *state, Buffer *buf, bool force);
static void update_modifiers(State *state, int code, int value);
static bool open_log_file_for_tm(State *state, const struct tm *tm);
static void rotate_log_if_needed(State *state);

static void copy_path_checked(char *dest, size_t dest_len, const char *src, const char *label) {
    if (!dest || dest_len == 0) {
        fprintf(stderr, "invalid destination for %s\n", label);
        exit(1);
    }
    if (!src) {
        src = "";
    }
    int written = snprintf(dest, dest_len, "%s", src);
    if (written < 0 || (size_t)written >= dest_len) {
        fprintf(stderr, "%s path too long\n", label);
        exit(1);
    }
}

static void init_xkb(State *state) {
#if !STATE_HAVE_XKBCOMMON
    state->translate_mode = TRANSLATE_RAW;
    return;
#else
    if (state->translate_mode != TRANSLATE_XKB) {
        return;
    }

    state->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!state->xkb_ctx) {
        state->translate_mode = TRANSLATE_RAW;
        return;
    }

    struct xkb_rule_names names = {
        .layout = state->xkb_layout,
        .variant = state->xkb_variant,
    };
    state->xkb_keymap = xkb_keymap_new_from_names(state->xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!state->xkb_keymap) {
        xkb_context_unref(state->xkb_ctx);
        state->xkb_ctx = NULL;
        state->translate_mode = TRANSLATE_RAW;
        return;
    }

    state->xkb_state = xkb_state_new(state->xkb_keymap);
    if (!state->xkb_state) {
        xkb_keymap_unref(state->xkb_keymap);
        xkb_context_unref(state->xkb_ctx);
        state->xkb_keymap = NULL;
        state->xkb_ctx = NULL;
        state->translate_mode = TRANSLATE_RAW;
    }
#endif
}

void state_init(State *state, const StateConfig *config, CommandExecutor *executor) {
    memset(state, 0, sizeof(*state));
    buffer_list_init(&state->buffers);

    copy_path_checked(state->log_dir, sizeof(state->log_dir), config->log_dir, "log directory");
    copy_path_checked(state->snapshot_dir, sizeof(state->snapshot_dir), config->snapshot_dir, "snapshot directory");
    state->snapshot_interval = config->snapshot_interval;
    state->clipboard_mode = config->clipboard_mode;
    state->translate_mode = config->translate_mode;
    state->log_mode = config->log_mode;
    state->xkb_layout = config->xkb_layout;
    state->xkb_variant = config->xkb_variant;
    state->executor = executor;

    struct timespec ts;
    util_get_realtime(&ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    snprintf(state->session_id, sizeof(state->session_id),
             "%04d%02d%02dT%02d%02d%02d-%06ld",
             tm.tm_year + 1900,
             tm.tm_mon + 1,
             tm.tm_mday,
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec,
             ts.tv_nsec / 1000);

    if (!open_log_file_for_tm(state, &tm)) {
        exit(1);
    }

    init_xkb(state);

    log_event(state, "start", true, NULL, false, NULL, NULL, NULL);
}

void state_cleanup(State *state) {
    state_flush_idle(state, true);
    log_event(state, "stop", true, NULL, false, NULL, NULL, NULL);
    if (state->log_file) fclose(state->log_file);
    buffer_list_free(&state->buffers);
#if STATE_HAVE_XKBCOMMON
    if (state->xkb_state) xkb_state_unref(state->xkb_state);
    if (state->xkb_keymap) xkb_keymap_unref(state->xkb_keymap);
    if (state->xkb_ctx) xkb_context_unref(state->xkb_ctx);
#endif
}

int state_poll_timeout_ms(const State *state) {
    if (state->log_mode == LOG_MODE_EVENTS) {
        return -1;
    }
    double interval_ms = state->snapshot_interval * 1000.0;
    if (interval_ms < 50.0) {
        interval_ms = 50.0;
    }
    double max_ms = 3600000.0; /* clamp to one hour to avoid overflow */
    if (interval_ms > max_ms) {
        interval_ms = max_ms;
    }
    return (int)interval_ms;
}

static char lowercase_char_for_key(int code);  /* forward decl for keycode_name */

static const char *keycode_name(int code) {
    static char buf[32];
    switch (code) {
        case KEY_ESC: return "KEY_ESC";
        case KEY_ENTER: return "KEY_ENTER";
        case KEY_BACKSPACE: return "KEY_BACKSPACE";
        case KEY_TAB: return "KEY_TAB";
        case KEY_SPACE: return "KEY_SPACE";
        case KEY_CAPSLOCK: return "KEY_CAPSLOCK";
        case KEY_INSERT: return "KEY_INSERT";
        default:
            break;
    }
    /* Use lowercase_char_for_key() to get the correct letter — evdev keycodes
       are NOT contiguous A-Z (they follow keyboard rows: Q=16..P=25, A=30..L=38, Z=44..M=50).
       The old code assumed contiguous codes and mislabeled most letters. */
    {
        char letter = lowercase_char_for_key(code);
        if (letter >= 'a' && letter <= 'z') {
            snprintf(buf, sizeof(buf), "KEY_%c", letter - 32);  /* uppercase */
            return buf;
        }
    }
    if (code >= KEY_0 && code <= KEY_9) {
        snprintf(buf, sizeof(buf), "KEY_%c", '0' + (code - KEY_0));
        return buf;
    }
    snprintf(buf, sizeof(buf), "KEY_%d", code);
    return buf;
}

/* True for the evdev BTN_MOUSE-group codes (mouse/trackball buttons) --
   deliberately narrow (not the whole BTN_* namespace, which also covers
   gamepad/joystick/tablet/touch buttons interleaved in the same numeric
   space) so a plain mouse click is never misrouted through the keyboard
   text-buffer path in process_key(). */
static bool is_pointer_button(int code) {
    return code >= BTN_LEFT && code <= BTN_TASK;
}

static const char *button_name(int code) {
    static char buf[32];
    switch (code) {
        case BTN_LEFT: return "BTN_LEFT";
        case BTN_RIGHT: return "BTN_RIGHT";
        case BTN_MIDDLE: return "BTN_MIDDLE";
        case BTN_SIDE: return "BTN_SIDE";
        case BTN_EXTRA: return "BTN_EXTRA";
        case BTN_FORWARD: return "BTN_FORWARD";
        case BTN_BACK: return "BTN_BACK";
        case BTN_TASK: return "BTN_TASK";
        default:
            snprintf(buf, sizeof(buf), "BTN_%d", code);
            return buf;
    }
}

static const char *rel_axis_name(int code) {
    static char buf[32];
    switch (code) {
        case REL_X: return "REL_X";
        case REL_Y: return "REL_Y";
        case REL_Z: return "REL_Z";
        case REL_WHEEL: return "REL_WHEEL";
        case REL_HWHEEL: return "REL_HWHEEL";
#ifdef REL_WHEEL_HI_RES
        case REL_WHEEL_HI_RES: return "REL_WHEEL_HI_RES";
#endif
#ifdef REL_HWHEEL_HI_RES
        case REL_HWHEEL_HI_RES: return "REL_HWHEEL_HI_RES";
#endif
        default:
            snprintf(buf, sizeof(buf), "REL_%d", code);
            return buf;
    }
}

static const char *abs_axis_name(int code) {
    static char buf[32];
    switch (code) {
        case ABS_X: return "ABS_X";
        case ABS_Y: return "ABS_Y";
        case ABS_PRESSURE: return "ABS_PRESSURE";
        default:
            snprintf(buf, sizeof(buf), "ABS_%d", code);
            return buf;
    }
}

static char lowercase_char_for_key(int code) {
    switch (code) {
        case KEY_A: return 'a';
        case KEY_B: return 'b';
        case KEY_C: return 'c';
        case KEY_D: return 'd';
        case KEY_E: return 'e';
        case KEY_F: return 'f';
        case KEY_G: return 'g';
        case KEY_H: return 'h';
        case KEY_I: return 'i';
        case KEY_J: return 'j';
        case KEY_K: return 'k';
        case KEY_L: return 'l';
        case KEY_M: return 'm';
        case KEY_N: return 'n';
        case KEY_O: return 'o';
        case KEY_P: return 'p';
        case KEY_Q: return 'q';
        case KEY_R: return 'r';
        case KEY_S: return 's';
        case KEY_T: return 't';
        case KEY_U: return 'u';
        case KEY_V: return 'v';
        case KEY_W: return 'w';
        case KEY_X: return 'x';
        case KEY_Y: return 'y';
        case KEY_Z: return 'z';
        case KEY_1: return '1';
        case KEY_2: return '2';
        case KEY_3: return '3';
        case KEY_4: return '4';
        case KEY_5: return '5';
        case KEY_6: return '6';
        case KEY_7: return '7';
        case KEY_8: return '8';
        case KEY_9: return '9';
        case KEY_0: return '0';
        case KEY_MINUS: return '-';
        case KEY_EQUAL: return '=';
        case KEY_LEFTBRACE: return '[';
        case KEY_RIGHTBRACE: return ']';
        case KEY_BACKSLASH: return '\\';
        case KEY_SEMICOLON: return ';';
        case KEY_APOSTROPHE: return '\'';
        case KEY_COMMA: return ',';
        case KEY_DOT: return '.';
        case KEY_SLASH: return '/';
        case KEY_GRAVE: return '`';
        default: return '\0';
    }
}

static bool is_letter_key(int code) {
    switch (code) {
        case KEY_A: case KEY_B: case KEY_C: case KEY_D: case KEY_E:
        case KEY_F: case KEY_G: case KEY_H: case KEY_I: case KEY_J:
        case KEY_K: case KEY_L: case KEY_M: case KEY_N: case KEY_O:
        case KEY_P: case KEY_Q: case KEY_R: case KEY_S: case KEY_T:
        case KEY_U: case KEY_V: case KEY_W: case KEY_X: case KEY_Y:
        case KEY_Z:
            return true;
        default:
            return false;
    }
}

static char shifted_symbol_for_key(int code) {
    switch (code) {
        case KEY_1: return '!';
        case KEY_2: return '@';
        case KEY_3: return '#';
        case KEY_4: return '$';
        case KEY_5: return '%';
        case KEY_6: return '^';
        case KEY_7: return '&';
        case KEY_8: return '*';
        case KEY_9: return '(';
        case KEY_0: return ')';
        case KEY_MINUS: return '_';
        case KEY_EQUAL: return '+';
        case KEY_LEFTBRACE: return '{';
        case KEY_RIGHTBRACE: return '}';
        case KEY_BACKSLASH: return '|';
        case KEY_SEMICOLON: return ':';
        case KEY_APOSTROPHE: return '"';
        case KEY_COMMA: return '<';
        case KEY_DOT: return '>';
        case KEY_SLASH: return '?';
        case KEY_GRAVE: return '~';
        default: return '\0';
    }
}

static char translate_char(State *state, int code) {
    char base = lowercase_char_for_key(code);
    if (base) {
        bool shift = state->modifiers[MOD_SHIFT];
        if (is_letter_key(code)) {
            if (state->capslock ^ shift) {
                return (char)(base - 32);
            }
            return base;
        }
        if (shift) {
            char sym = shifted_symbol_for_key(code);
            if (sym) return sym;
        }
        return base;
    }
    switch (code) {
        case KEY_SPACE: return ' ';
        case KEY_KP0: return '0';
        case KEY_KP1: return '1';
        case KEY_KP2: return '2';
        case KEY_KP3: return '3';
        case KEY_KP4: return '4';
        case KEY_KP5: return '5';
        case KEY_KP6: return '6';
        case KEY_KP7: return '7';
        case KEY_KP8: return '8';
        case KEY_KP9: return '9';
        case KEY_KPPLUS: return '+';
        case KEY_KPMINUS: return '-';
        case KEY_KPDOT: return '.';
        case KEY_KPASTERISK: return '*';
        default: return '\0';
    }
}

static void update_modifiers(State *state, int code, int value) {
    switch (code) {
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT:
            state->modifiers[MOD_SHIFT] = (value != 0);
            break;
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:
            state->modifiers[MOD_CTRL] = (value != 0);
            break;
        case KEY_LEFTALT:
        case KEY_RIGHTALT:
            state->modifiers[MOD_ALT] = (value != 0);
            break;
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA:
            state->modifiers[MOD_SUPER] = (value != 0);
            break;
        case KEY_CAPSLOCK:
            if (value == 1) state->capslock = !state->capslock;
            break;
        default:
            break;
    }
}

static bool open_log_file_for_tm(State *state, const struct tm *tm) {
    if (!state || !tm) return false;

    char log_name[64];
    int name_written = snprintf(log_name, sizeof(log_name), "%04d-%02d-%02d.jsonl",
                                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    if (name_written < 0 || (size_t)name_written >= sizeof(log_name)) {
        fprintf(stderr, "log filename too long\n");
        return false;
    }

    char log_path[PATH_MAX];
    util_append_path(log_path, sizeof(log_path), state->log_dir, log_name);

    FILE *file = fopen(log_path, "a");
    if (!file) {
        perror("fopen log");
        return false;
    }

    if (state->log_file) {
        fflush(state->log_file);
        fclose(state->log_file);
    }
    state->log_file = file;
    state->log_year = tm->tm_year + 1900;
    state->log_month = tm->tm_mon + 1;
    state->log_day = tm->tm_mday;
    return true;
}

static void rotate_log_if_needed(State *state) {
    if (!state) return;

    struct timespec ts;
    util_get_realtime(&ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);

    int year = tm.tm_year + 1900;
    int month = tm.tm_mon + 1;
    int day = tm.tm_mday;

    if (state->log_file &&
        state->log_year == year &&
        state->log_month == month &&
        state->log_day == day) {
        return;
    }

    if (!open_log_file_for_tm(state, &tm)) {
        /* Leave the previous file in place so we continue logging somewhere. */
    }
}

/* sinnix-7dwp: two independent scribe-tap processes (keyboard pipeline,
   mouse pipeline) fopen(path, "a") the SAME daily JSONL file and were each
   composing one record via multiple buffered stdio calls (fprintf x N +
   fputs), then fflush()ing at their own cadence. Buffered stdio gives no
   atomicity guarantee across those calls, and log_pointer_event's
   deliberate flush=false for motion events (see its comment) widened the
   window further -- the two processes' partial in-flight records could
   (and, per the bead's byte-level evidence, did) land underneath one
   another's write(2)s and torn/interleave.
   Fix: compose the FULL record in memory first, then hand it to the
   kernel as exactly one write(2) syscall on the O_APPEND fd. A single
   write() to a regular local (non-NFS) file is atomic against other
   writers' write()s at the VFS layer regardless of size -- this is the
   same guarantee syslog-style multi-writer append logging has relied on
   for decades on Linux -- so this needs no flock() and no single-writer
   funnel process, and preserves the pointer path's no-per-event-fsync
   throughput characteristic (write() is inherently unbuffered; there is
   no "sitting in a stdio buffer" window left to widen). */
static bool log_write_line(State *state, const char *line, size_t len) {
    if (!state->log_file) return false;
    int fd = fileno(state->log_file);
    ssize_t n = write(fd, line, len);
    if (n < 0) {
        perror("write log line");
        return false;
    }
    if ((size_t)n != len) {
        /* A genuine short write on a local regular file is abnormal (disk
           full, quota, etc.) -- report it rather than retrying with a
           second write() call, since a second call here would itself be
           a second syscall and could interleave with a concurrent
           writer's record, reintroducing exactly the bug this fixes. */
        fprintf(stderr, "scribe-tap: short write to log (%zd of %zu bytes), record dropped\n",
                n, len);
        return false;
    }
    return true;
}

static void log_event(State *state, const char *event, bool flush,
                      const char *keycode, bool changed, const char *buffer_text,
                      const char *clipboard_text, const struct input_event *input) {
    rotate_log_if_needed(state);
    if (!state->log_file) return;
    bool is_snapshot = (event && strcmp(event, "snapshot") == 0);
    if (input && state->log_mode == LOG_MODE_SNAPSHOTS) return;
    if (is_snapshot && state->log_mode == LOG_MODE_EVENTS) return;
    char ts[64];
    util_iso8601(ts, sizeof(ts));

    char *buffer_json = (is_snapshot && buffer_text) ? util_json_escape(buffer_text) : NULL;
    char *clip_json = clipboard_text ? util_json_escape(clipboard_text) : NULL;

    /* Composed via open_memstream so unbounded buffer/clipboard content
       (buffer.c grows Buffer.text with no fixed cap) can't overflow a
       fixed-size stack buffer -- correctness matters more than avoiding
       one malloc here, these are not the high-rate pointer events. */
    char *line = NULL;
    size_t line_len = 0;
    FILE *mem = open_memstream(&line, &line_len);
    if (!mem) {
        perror("open_memstream");
        free(buffer_json);
        free(clip_json);
        return;
    }

    fprintf(mem, "{\"ts\":\"%s\",\"event\":\"%s\",\"session\":\"%s\"",
            ts, event, state->session_id);
    if (keycode) {
        fprintf(mem, ",\"keycode\":\"%s\"", keycode);
    }
    if (input) {
        fprintf(mem, ",\"code\":%u,\"value\":%d,\"input_sec\":%lld,\"input_usec\":%lld",
                (unsigned int)input->code, input->value,
                (long long)input->input_event_sec, (long long)input->input_event_usec);
    }
    fprintf(mem, ",\"changed\":%s", changed ? "true" : "false");
    if (buffer_json) {
        fprintf(mem, ",\"buffer\":%s", buffer_json);
    }
    if (clip_json) {
        fprintf(mem, ",\"clipboard\":%s", clip_json);
    }
    fputs("}\n", mem);
    fclose(mem); /* flushes into line/line_len */

    free(buffer_json);
    free(clip_json);

    log_write_line(state, line, line_len);
    free(line);
    (void)flush; /* write() above is already unbuffered; nothing left to flush */
}

/* Pointer/other-device events (motion, buttons, generic axes) have a
   different field shape than keystroke events, but the SAME durability
   story now: every record here is one write(2) syscall regardless of
   event type (see sinnix-7dwp comment below and on log_event), so there
   is no separate stdio-buffer-vs-fsync tradeoff to make anymore -- the
   `flush` parameter is kept only for call-site symmetry with log_event
   and does nothing. */
static void log_pointer_event(State *state, const char *event, const char *code_name,
                              int value, bool flush) {
    rotate_log_if_needed(state);
    if (!state->log_file) return;
    if (state->log_mode == LOG_MODE_SNAPSHOTS) return;
    char ts[64];
    util_iso8601(ts, sizeof(ts));
    /* sinnix-7dwp: every field here is fixed/bounded (ts, short event/code
       string literals, State.session_id[64], an int) -- a stack buffer is
       safe and, unlike open_memstream's malloc, keeps this hot 1kHz path
       allocation-free. See log_event's comment for why a single write(2)
       of the fully-composed line (not per-field fprintf) is what actually
       fixes the torn-write bug. */
    char line[512];
    int n = snprintf(line, sizeof(line),
            "{\"ts\":\"%s\",\"event\":\"%s\",\"session\":\"%s\",\"code\":\"%s\",\"value\":%d}\n",
            ts, event, state->session_id, code_name, value);
    if (n < 0) {
        perror("snprintf pointer log line");
        return;
    }
    if ((size_t)n >= sizeof(line)) {
        fprintf(stderr, "scribe-tap: pointer log line truncated (%d bytes needed), record dropped\n", n);
        return;
    }
    log_write_line(state, line, (size_t)n);
    (void)flush; /* write() above is already unbuffered; nothing left to flush */
}

static void write_snapshot(State *state, Buffer *buf, bool force) {
    if (state->log_mode == LOG_MODE_EVENTS) {
        return;
    }
    double now = util_now_seconds();
    if (!force && now - buf->last_snapshot < state->snapshot_interval) {
        return;
    }
    char path[PATH_MAX];
    util_append_path(path, sizeof(path), state->snapshot_dir, buf->slug);
    strncat(path, ".txt", sizeof(path) - strlen(path) - 1);

    FILE *f = fopen(path, "w");
    if (!f) {
        perror("fopen snapshot");
        return;
    }
    fwrite(buf->text, 1, buf->len, f);
    fclose(f);
    buf->last_snapshot = now;
    log_event(state, "snapshot", true, NULL, false, buf->text, NULL, NULL);
}

void state_flush_idle(State *state, bool force_all) {
    double now = util_now_seconds();
    if (state->log_mode != LOG_MODE_EVENTS && state->buffers.len > 0) {
        for (size_t i = 0; i < state->buffers.len; ++i) {
            Buffer *buf = &state->buffers.items[i];
            if (buf->last_update <= buf->last_snapshot) {
                continue;
            }
            if (!force_all) {
                if (now - buf->last_update < state->snapshot_interval) {
                    continue;
                }
            }
            write_snapshot(state, buf, true);
        }
    }

    double eviction_interval = state->snapshot_interval > 0.0 ? state->snapshot_interval * 6.0 : 300.0;
    if (eviction_interval < 30.0) {
        eviction_interval = 30.0;
    } else if (eviction_interval > 3600.0) {
        eviction_interval = 3600.0;
    }
    bool allow_dirty = (state->log_mode == LOG_MODE_EVENTS);
    buffer_list_evict_idle(&state->buffers, now, eviction_interval, 256, allow_dirty);
}

static char *read_clipboard(State *state) {
    if (state->clipboard_mode != CLIPBOARD_AUTO) return NULL;
    const char *wl_paste_cmd[] = {"wl-paste", "-n", NULL};
    char *clip = command_executor_capture(state->executor, wl_paste_cmd);
    if (clip) {
        util_trim_newline(clip);
        return clip;
    }
    const char *xclip_cmd[] = {"xclip", "-selection", "clipboard", "-o", NULL};
    clip = command_executor_capture(state->executor, xclip_cmd);
    if (clip) {
        util_trim_newline(clip);
    }
    return clip;
}

/* Single session-scoped buffer: scribe-tap no longer partitions keystroke
   buffering by compositor window (that was a Hyprland-polling feature that
   never worked correctly against this host's actual runtime layout -- see
   git history). Window/app attribution, if wanted later, is a downstream
   join against ActivityWatch's focus events by timestamp, not something
   this process should re-derive by talking to the compositor itself. */
#define SCRIBE_TAP_BUFFER_KEY "session"

static void process_key(State *state, const struct input_event *input, const char *key_name, const char *utf8_text, char *dynamic_text) {
    int code = input->code;
    Buffer *buf = buffer_lookup(&state->buffers, SCRIBE_TAP_BUFFER_KEY, true);

    char appended[2] = {0};
    bool changed = false;
    bool force_snapshot = false;
    char *clipboard = NULL;

    switch (code) {
        case KEY_BACKSPACE:
            if (buf->len) {
                buffer_backspace(buf);
                changed = true;
            }
            break;
        case KEY_DELETE:
            changed = false;
            break;
        case KEY_ENTER:
        case KEY_KPENTER:
            appended[0] = '\n';
            buffer_append(buf, appended, 1);
            changed = true;
            force_snapshot = true;
            break;
        case KEY_TAB:
            appended[0] = '\t';
            buffer_append(buf, appended, 1);
            changed = true;
            break;
        default: {
            bool is_paste = false;
            if (code == KEY_V && state->modifiers[MOD_CTRL]) {
                is_paste = true;
            } else if (code == KEY_INSERT && state->modifiers[MOD_SHIFT] && !state->modifiers[MOD_CTRL]) {
                is_paste = true;
            }
            if (is_paste) {
                clipboard = read_clipboard(state);
                if (clipboard) {
                    buffer_append(buf, clipboard, strlen(clipboard));
                    changed = true;
                }
            } else if (state->modifiers[MOD_CTRL] || state->modifiers[MOD_ALT]) {
                /* Skip buffer append when Ctrl or Alt is held — xkbcommon returns
                   control codes (0x01-0x1f) that corrupt the buffer. The press event
                   still logs the keycode for reconstruction from raw events. */
                break;
            } else {
                if (utf8_text && *utf8_text) {
                    buffer_append(buf, utf8_text, strlen(utf8_text));
                    changed = true;
                } else if (dynamic_text && *dynamic_text) {
                    buffer_append(buf, dynamic_text, strlen(dynamic_text));
                    changed = true;
                } else if (state->translate_mode == TRANSLATE_RAW) {
                    char c = translate_char(state, code);
                    if (c) {
                        appended[0] = c;
                        buffer_append(buf, appended, 1);
                        changed = true;
                    }
                }
            }
            break;
        }
    }

    if (changed) {
        buf->last_update = util_now_seconds();
        buf->last_used = buf->last_update;
        write_snapshot(state, buf, force_snapshot);
    }

    if (state->log_mode != LOG_MODE_SNAPSHOTS) {
        log_event(state, "press", true, key_name, changed, NULL, clipboard, input);
    }

    free(clipboard);
}

static void process_keyboard_key(State *state, const struct input_event *event) {
    if (event->value < 0 || event->value > 2) return;
    const char *name = keycode_name(event->code);

    if (event->value == 1 || event->value == 2) {
        update_modifiers(state, event->code, event->value);
    } else if (event->value == 0) {
        update_modifiers(state, event->code, 0);
    }

#if STATE_HAVE_XKBCOMMON
    if (state->translate_mode == TRANSLATE_XKB && state->xkb_state && event->value != 2) {
        enum xkb_key_direction dir = (event->value == 0) ? XKB_KEY_UP : XKB_KEY_DOWN;
        xkb_state_update_key(state->xkb_state, event->code + 8, dir);
    }
#endif

    if (event->value == 1 || event->value == 2) {
#if STATE_HAVE_XKBCOMMON
        char static_buf[64];
#endif
        char *dynamic_buf = NULL;
        const char *text_ptr = NULL;
#if STATE_HAVE_XKBCOMMON
        if (state->translate_mode == TRANSLATE_XKB && state->xkb_state) {
            int needed = xkb_state_key_get_utf8(state->xkb_state, event->code + 8, NULL, 0);
            if (needed > 0) {
                if ((size_t)needed < sizeof(static_buf)) {
                    int written = xkb_state_key_get_utf8(state->xkb_state, event->code + 8, static_buf, sizeof(static_buf));
                    if (written > 0) {
                        if ((size_t)written >= sizeof(static_buf)) {
                            static_buf[sizeof(static_buf) - 1] = '\0';
                        } else {
                            static_buf[written] = '\0';
                        }
                        text_ptr = static_buf;
                    }
                } else {
                    dynamic_buf = calloc((size_t)needed + 1, 1);
                    if (dynamic_buf) {
                        int written = xkb_state_key_get_utf8(state->xkb_state, event->code + 8, dynamic_buf, (size_t)needed + 1);
                        if (written > 0) {
                            dynamic_buf[written] = '\0';
                            text_ptr = dynamic_buf;
                        } else {
                            free(dynamic_buf);
                            dynamic_buf = NULL;
                        }
                    }
                }
            }
        }
#endif
        process_key(state, event, name, text_ptr, dynamic_buf);
        free(dynamic_buf);
    } else {
        log_event(state, "release", true, name, false, NULL, NULL, event);
    }
}

/* Mouse button press/release. Logged like keystrokes (fsync'd every line --
   button events are low-rate and each one is a discrete fact worth not
   losing) but never touch the text buffer/snapshot machinery, which is
   keystroke-content-specific. */
static void process_pointer_button(State *state, const struct input_event *event) {
    if (event->value != 0 && event->value != 1) {
        return; /* ignore autorepeat (value==2), which evdev never emits for buttons anyway */
    }
    const char *ev = event->value ? "pointer_button_press" : "pointer_button_release";
    log_pointer_event(state, ev, button_name(event->code), event->value, true);
}

/* Relative motion/scroll. Deliberately generic over the whole REL_* axis
   space (not just X/Y/WHEEL) so an unrecognized axis is still captured
   with its raw code number rather than silently dropped -- "if we can
   grab it, capture it" per the design brief, applied consistently. Motion
   (REL_X/REL_Y) does not force an fsync per event (see log_pointer_event);
   wheel/scroll events do, since they are semantically closer to discrete
   button-press facts than to a continuous position stream. */
static void process_pointer_rel(State *state, const struct input_event *event) {
    bool is_wheel = (event->code == REL_WHEEL || event->code == REL_HWHEEL);
#ifdef REL_WHEEL_HI_RES
    is_wheel = is_wheel || (event->code == REL_WHEEL_HI_RES);
#endif
#ifdef REL_HWHEEL_HI_RES
    is_wheel = is_wheel || (event->code == REL_HWHEEL_HI_RES);
#endif
    log_pointer_event(state, "pointer_rel", rel_axis_name(event->code), event->value, is_wheel);
}

/* Absolute-position axes (touchpads/tablets/touchscreens riding the same
   interception pipeline as a plain mouse). Same low-overhead flush policy
   as relative motion. */
static void process_pointer_abs(State *state, const struct input_event *event) {
    log_pointer_event(state, "pointer_abs", abs_axis_name(event->code), event->value, false);
}

void state_process_input(State *state, const struct input_event *event) {
    if (!event) {
        return;
    }

    switch (event->type) {
        case EV_KEY:
            if (is_pointer_button(event->code)) {
                process_pointer_button(state, event);
            } else {
                process_keyboard_key(state, event);
            }
            break;
        case EV_REL:
            process_pointer_rel(state, event);
            break;
        case EV_ABS:
            process_pointer_abs(state, event);
            break;
        default:
            break; /* EV_SYN, EV_MSC, etc. carry no capturable content of their own */
    }
}
