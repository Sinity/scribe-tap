#define _GNU_SOURCE
#include "state.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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

static void log_event(State *state, const char *event, const char *window,
                      const char *keycode, bool changed, const char *buffer_text,
                      const char *clipboard_text);
static void write_snapshot(State *state, Buffer *buf, bool force);
static void update_context(State *state);
static void update_modifiers(State *state, int code, int value);

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

static char *load_hypr_signature_for_user(const char *user) {
    if (!user) return NULL;
    struct passwd *pw = getpwnam(user);
    if (!pw) return NULL;
    const uid_t uid = pw->pw_uid;
    char path[PATH_MAX];
    const char *home = pw->pw_dir;
    const char *home_candidates[] = {
        "%s/.cache/hyprland/instance",
        "%s/.cache/hyprland/hyprland_instance",
        "%s/.cache/hyprland/hyprland.conf-instance",
    };
    for (size_t i = 0; i < sizeof(home_candidates) / sizeof(home_candidates[0]); ++i) {
        if (!home) break;
        snprintf(path, sizeof(path), home_candidates[i], home);
        char *value = util_read_trimmed_file(path);
        if (value && *value) return value;
        free(value);
    }
    const char *runtime_candidates[] = {
        "/run/user/%u/hypr/instance",
        "/run/user/%u/hypr/hyprland_instance",
    };
    for (size_t i = 0; i < sizeof(runtime_candidates) / sizeof(runtime_candidates[0]); ++i) {
        snprintf(path, sizeof(path), runtime_candidates[i], uid);
        char *value = util_read_trimmed_file(path);
        if (value && *value) return value;
        free(value);
    }
    return NULL;
}

static char *auto_detect_hypr_signature(void) {
    DIR *dir = opendir("/run/user");
    if (!dir) return NULL;

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!entry->d_name[0] || entry->d_name[0] == '.') {
            continue;
        }
        char *end = NULL;
        errno = 0;
        unsigned long uid = strtoul(entry->d_name, &end, 10);
        if (errno != 0 || !end || *end != '\0') {
            continue;
        }
        struct passwd *pw = getpwuid((uid_t)uid);
        if (!pw || !pw->pw_name) {
            continue;
        }
        char *value = load_hypr_signature_for_user(pw->pw_name);
        if (value && *value) {
            closedir(dir);
            return value;
        }
        free(value);
    }

    closedir(dir);
    return NULL;
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
    copy_path_checked(state->hyprctl_cmd, sizeof(state->hyprctl_cmd), config->hyprctl_cmd, "hyprctl command");
    state->snapshot_interval = config->snapshot_interval;
    state->context_refresh = config->context_refresh;
    state->clipboard_mode = config->clipboard_mode;
    state->translate_mode = config->translate_mode;
    state->log_mode = config->log_mode;
    state->context_enabled = config->context_enabled;
    state->xkb_layout = config->xkb_layout;
    state->xkb_variant = config->xkb_variant;
    state->executor = executor;

    if (config->hypr_signature_path) {
        state->hypr_signature = util_read_trimmed_file(config->hypr_signature_path);
    } else if (config->hypr_user) {
        state->hypr_signature = load_hypr_signature_for_user(config->hypr_user);
    } else {
        const char *env_sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
        if (env_sig && *env_sig) {
            state->hypr_signature = util_string_dup(env_sig);
        }
    }

    if (!state->hypr_signature) {
        state->hypr_signature = auto_detect_hypr_signature();
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
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

    char log_name[64];
    snprintf(log_name, sizeof(log_name), "%04d-%02d-%02d.jsonl",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    char log_path[PATH_MAX];
    util_append_path(log_path, sizeof(log_path), state->log_dir, log_name);

    state->log_file = fopen(log_path, "a");
    if (!state->log_file) {
        perror("fopen log");
        exit(1);
    }

    init_xkb(state);

    log_event(state, "start", NULL, NULL, false, NULL, NULL);
}

void state_cleanup(State *state) {
    state_flush_idle(state, true);
    log_event(state, "stop", NULL, NULL, false, NULL, NULL);
    if (state->log_file) fclose(state->log_file);
    buffer_list_free(&state->buffers);
#if STATE_HAVE_XKBCOMMON
    if (state->xkb_state) xkb_state_unref(state->xkb_state);
    if (state->xkb_keymap) xkb_keymap_unref(state->xkb_keymap);
    if (state->xkb_ctx) xkb_context_unref(state->xkb_ctx);
#endif
    free(state->hypr_signature);
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
    if (code >= KEY_A && code <= KEY_Z) {
        snprintf(buf, sizeof(buf), "KEY_%c", 'A' + (code - KEY_A));
        return buf;
    }
    if (code >= KEY_0 && code <= KEY_9) {
        snprintf(buf, sizeof(buf), "KEY_%c", '0' + (code - KEY_0));
        return buf;
    }
    snprintf(buf, sizeof(buf), "KEY_%d", code);
    return buf;
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

static void trim_newline(char *s) {
    util_trim_newline(s);
}

static void extract_json_field(const char *json, const char *field, char *out, size_t out_len) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", field);
    const char *pos = strstr(json, needle);
    if (!pos) {
        out[0] = '\0';
        return;
    }
    pos = strchr(pos, ':');
    if (!pos) {
        out[0] = '\0';
        return;
    }
    pos = strchr(pos, '"');
    if (!pos) {
        out[0] = '\0';
        return;
    }
    pos++;
    size_t j = 0;
    while (*pos && *pos != '"' && j + 1 < out_len) {
        if (*pos == '\\' && pos[1]) {
            pos++;
        }
        out[j++] = *pos++;
    }
    out[j] = '\0';
    trim_newline(out);
}

static void reset_context_on_failure(State *state) {
    const char *fallback = "unknown";
    if (strcmp(state->current_context, fallback) == 0) {
        return;
    }

    char previous[sizeof(state->current_context)];
    strncpy(previous, state->current_context, sizeof(previous));
    previous[sizeof(previous) - 1] = '\0';

    strncpy(state->current_context, fallback, sizeof(state->current_context));
    state->current_context[sizeof(state->current_context) - 1] = '\0';

    if (previous[0]) {
        Buffer *prev = buffer_lookup(&state->buffers, previous, false);
        if (prev) {
            write_snapshot(state, prev, true);
        }
    }

    log_event(state, "focus", state->current_context, NULL, false, NULL, NULL);
}

static void update_context(State *state) {
    double now = util_now_seconds();
    if (!state->context_enabled) {
        if (state->current_context[0] == '\0') {
            strncpy(state->current_context, "global", sizeof(state->current_context));
            state->current_context[sizeof(state->current_context) - 1] = '\0';
        }
        return;
    }
    if (now - state->last_context_poll < state->context_refresh) {
        return;
    }
    state->last_context_poll = now;

    const char *argv[6];
    size_t argc = 0;
    argv[argc++] = state->hyprctl_cmd;
    if (state->hypr_signature && *state->hypr_signature) {
        argv[argc++] = "--instance";
        argv[argc++] = state->hypr_signature;
    }
    argv[argc++] = "activewindow";
    argv[argc++] = "-j";
    argv[argc] = NULL;

    char *json = command_executor_capture(state->executor, argv);
    if (!json) {
        reset_context_on_failure(state);
        return;
    }

    char title[256] = "untitled";
    char clazz[128] = "unknown";
    char address[64] = "0x0";

    extract_json_field(json, "title", title, sizeof(title));
    extract_json_field(json, "class", clazz, sizeof(clazz));
    extract_json_field(json, "address", address, sizeof(address));

    char combined[sizeof(state->current_context)];
    snprintf(combined, sizeof(combined), "%s (%s) [%s]", title, clazz, address);
    trim_newline(combined);

    if (strcmp(combined, state->current_context) != 0) {
        char previous[sizeof(state->current_context)];
        strncpy(previous, state->current_context, sizeof(previous));
        previous[sizeof(previous) - 1] = '\0';

        strncpy(state->current_context, combined, sizeof(state->current_context));
        state->current_context[sizeof(state->current_context) - 1] = '\0';

        if (previous[0]) {
            Buffer *prev = buffer_lookup(&state->buffers, previous, false);
            if (prev) {
                write_snapshot(state, prev, true);
            }
        }
        log_event(state, "focus", state->current_context, NULL, false, NULL, NULL);
    }

    free(json);
}

static void log_event(State *state, const char *event, const char *window,
                      const char *keycode, bool changed, const char *buffer_text,
                      const char *clipboard_text) {
    if (!state->log_file) return;
    bool is_press = (event && strcmp(event, "press") == 0);
    bool is_snapshot = (event && strcmp(event, "snapshot") == 0);
    if (is_press && state->log_mode == LOG_MODE_SNAPSHOTS) return;
    if (is_snapshot && state->log_mode == LOG_MODE_EVENTS) return;
    char ts[64];
    util_iso8601(ts, sizeof(ts));

    fprintf(state->log_file,
            "{\"ts\":\"%s\",\"event\":\"%s\",\"session\":\"%s\"",
            ts, event, state->session_id);

    if (window) {
        char *window_json = util_json_escape(window);
        fprintf(state->log_file, ",\"window\":%s", window_json);
        free(window_json);
    }
    if (keycode) {
        fprintf(state->log_file, ",\"keycode\":\"%s\"", keycode);
    }
    fprintf(state->log_file, ",\"changed\":%s", changed ? "true" : "false");
    if (is_snapshot && buffer_text) {
        char *buffer_json = util_json_escape(buffer_text);
        fprintf(state->log_file, ",\"buffer\":%s", buffer_json);
        free(buffer_json);
    }
    if (clipboard_text) {
        char *clip_json = util_json_escape(clipboard_text);
        fprintf(state->log_file, ",\"clipboard\":%s", clip_json);
        free(clip_json);
    }
    fputs("}\n", state->log_file);
    fflush(state->log_file);
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
    log_event(state, "snapshot", buf->context, NULL, false, buf->text, NULL);
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

static void process_key(State *state, int code, const char *key_name, const char *utf8_text, char *dynamic_text) {
    update_context(state);

    const char *context = state->current_context[0] ? state->current_context : "unknown";
    Buffer *buf = buffer_lookup(&state->buffers, context, true);

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
        log_event(state, "press", buf->context, key_name, changed, NULL, clipboard);
    }

    free(clipboard);
}

void state_process_input(State *state, const struct input_event *event) {
    if (!event || event->type != EV_KEY) {
        return;
    }

    const char *name = keycode_name(event->code);
#if STATE_HAVE_XKBCOMMON
    if (state->translate_mode == TRANSLATE_XKB && state->xkb_state) {
        enum xkb_key_direction dir = (event->value == 0) ? XKB_KEY_UP : XKB_KEY_DOWN;
        xkb_state_update_key(state->xkb_state, event->code + 8, dir);
    }
#endif

    if (event->value == 1 || event->value == 2) {
        update_modifiers(state, event->code, event->value);
#if STATE_HAVE_XKBCOMMON
        char static_buf[64];
#endif
        char *dynamic_buf = NULL;
        const char *text_ptr = NULL;
#if STATE_HAVE_XKBCOMMON
        if (state->translate_mode == TRANSLATE_XKB) {
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
        process_key(state, event->code, name, text_ptr, dynamic_buf);
        free(dynamic_buf);
    } else if (event->value == 0) {
        update_modifiers(state, event->code, 0);
    }
}
