#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <ctype.h>

static volatile sig_atomic_t g_should_stop = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_should_stop = 1;
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void iso8601(char *buf, size_t len) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    int millis = ts.tv_nsec / 1000000;
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm.tm_year + 1900,
             tm.tm_mon + 1,
             tm.tm_mday,
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec,
             millis);
}

static void ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "%s exists and is not a directory\n", path);
            exit(1);
        }
        return;
    }
    if (errno != ENOENT) {
        perror("stat");
        exit(1);
    }
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        perror("mkdir");
        exit(1);
    }
}

static void append_path(char *dest, size_t dest_len, const char *dir, const char *leaf) {
    int written = snprintf(dest, dest_len, "%s/%s", dir, leaf);
    if (written < 0 || (size_t)written >= dest_len) {
        if (dest_len > 0) {
            dest[dest_len - 1] = '\0';
        }
    }
}

static char *string_dup(const char *src) {
    char *dup = strdup(src ? src : "");
    if (!dup) {
        perror("strdup");
        exit(1);
    }
    return dup;
}

static void rstrip_whitespace(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static char *read_trimmed_file(const char *path) {
    if (!path) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len = getline(&line, &cap, f);
    fclose(f);
    if (len <= 0) {
        free(line);
        return NULL;
    }
    rstrip_whitespace(line);
    return line;
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
    for (size_t i = 0; i < sizeof(home_candidates)/sizeof(home_candidates[0]); ++i) {
        if (!home) break;
        snprintf(path, sizeof(path), home_candidates[i], home);
        char *value = read_trimmed_file(path);
        if (value && *value) return value;
        free(value);
    }
    const char *runtime_candidates[] = {
        "/run/user/%u/hypr/instance",
        "/run/user/%u/hypr/hyprland_instance",
    };
    for (size_t i = 0; i < sizeof(runtime_candidates)/sizeof(runtime_candidates[0]); ++i) {
        snprintf(path, sizeof(path), runtime_candidates[i], uid);
        char *value = read_trimmed_file(path);
        if (value && *value) return value;
        free(value);
    }
    return NULL;
}

static void sanitize_slug(char *s) {
    size_t len = strlen(s);
    size_t j = 0;
    char prev = '\0';
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            s[j++] = (char)c;
            prev = (char)c;
        } else if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c + 32);
            s[j++] = (char)c;
            prev = (char)c;
        } else if (prev != '_') {
            s[j++] = '_';
            prev = '_';
        }
        if (j >= 80) break;
    }
    if (j == 0) {
        strcpy(s, "window");
        return;
    }
    s[j] = '\0';
}

static char *make_slug(const char *src) {
    char *copy = string_dup(src);
    sanitize_slug(copy);
    return copy;
}

static char *json_escape(const char *src) {
    size_t len = strlen(src);
    size_t cap = len * 2 + 3;
    char *out = malloc(cap);
    if (!out) {
        perror("malloc");
        exit(1);
    }
    size_t j = 0;
    out[j++] = '"';
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
            case '\\':
            case '"':
                out[j++] = '\\';
                out[j++] = (char)c;
                break;
            case '\n':
                out[j++] = '\\';
                out[j++] = 'n';
                break;
            case '\t':
                out[j++] = '\\';
                out[j++] = 't';
                break;
            case '\r':
                out[j++] = '\\';
                out[j++] = 'r';
                break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    size_t b = strlen(buf);
                    memcpy(&out[j], buf, b);
                    j += b;
                } else {
                    out[j++] = (char)c;
                }
        }
        if (j + 7 >= cap) {
            cap *= 2;
            char *tmp = realloc(out, cap);
            if (!tmp) {
                perror("realloc");
                exit(1);
            }
            out = tmp;
        }
    }
    out[j++] = '"';
    out[j] = '\0';
    return out;
}

struct Buffer {
    char *context;
    char *slug;
    char *text;
    size_t len;
    size_t cap;
    double last_update;
    double last_snapshot;
};

struct BufferList {
    struct Buffer *items;
    size_t len;
    size_t cap;
};

static struct Buffer *buffer_lookup(struct BufferList *list, const char *context, bool create) {
    for (size_t i = 0; i < list->len; ++i) {
        if (strcmp(list->items[i].context, context) == 0) {
            return &list->items[i];
        }
    }
    if (!create) return NULL;
    if (list->len == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 8;
        struct Buffer *tmp = realloc(list->items, new_cap * sizeof(struct Buffer));
        if (!tmp) {
            perror("realloc");
            exit(1);
        }
        list->items = tmp;
        list->cap = new_cap;
    }
    struct Buffer *buf = &list->items[list->len++];
    memset(buf, 0, sizeof(*buf));
    buf->context = string_dup(context);
    buf->slug = make_slug(context);
    buf->cap = 1024;
    buf->text = calloc(buf->cap, 1);
    if (!buf->text) {
        perror("calloc");
        exit(1);
    }
    return buf;
}

static void buffer_append(struct Buffer *buf, const char *data, size_t len) {
    if (buf->len + len + 1 > buf->cap) {
        size_t new_cap = buf->cap;
        while (buf->len + len + 1 > new_cap) new_cap *= 2;
        char *tmp = realloc(buf->text, new_cap);
        if (!tmp) {
            perror("realloc");
            exit(1);
        }
        buf->text = tmp;
        buf->cap = new_cap;
    }
    memcpy(buf->text + buf->len, data, len);
    buf->len += len;
    buf->text[buf->len] = '\0';
}

static void buffer_backspace(struct Buffer *buf) {
    if (buf->len == 0) return;
    buf->len--;
    buf->text[buf->len] = '\0';
}

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

enum ModifierIndex {
    MOD_SHIFT = 0,
    MOD_CTRL,
    MOD_ALT,
    MOD_SUPER,
    MOD_COUNT
};

struct State {
    char session_id[64];
    char log_dir[PATH_MAX];
    char snapshot_dir[PATH_MAX];
    char hyprctl_cmd[64];
    double snapshot_interval;
    double context_refresh;
    enum ClipboardMode clipboard_mode;
    enum TranslateMode translate_mode;
    enum LogMode log_mode;
    bool context_enabled;
    const char *xkb_layout;
    const char *xkb_variant;

    FILE *log_file;
    struct BufferList buffers;
    char current_context[512];
    double last_context_poll;

    bool capslock;
    bool modifiers[MOD_COUNT];
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    char *hypr_signature;
};

static void log_event(struct State *state, const char *event, const char *window,
                      const char *keycode, bool changed, const char *buffer_text,
                      const char *clipboard_text) {
    if (!state->log_file) return;
    bool is_press = (event && strcmp(event, "press") == 0);
    bool is_snapshot = (event && strcmp(event, "snapshot") == 0);
    if (is_press && state->log_mode == LOG_MODE_SNAPSHOTS) return;
    if (is_snapshot && state->log_mode == LOG_MODE_EVENTS) return;
    char ts[64];
    iso8601(ts, sizeof(ts));

    fprintf(state->log_file,
            "{\"ts\":\"%s\",\"event\":\"%s\",\"session\":\"%s\"",
            ts, event, state->session_id);

    if (window) {
        char *window_json = json_escape(window);
        fprintf(state->log_file, ",\"window\":%s", window_json);
        free(window_json);
    }
    if (keycode) {
        fprintf(state->log_file, ",\"keycode\":\"%s\"", keycode);
    }
    fprintf(state->log_file, ",\"changed\":%s", changed ? "true" : "false");
    if (is_snapshot && buffer_text) {
        char *buffer_json = json_escape(buffer_text);
        fprintf(state->log_file, ",\"buffer\":%s", buffer_json);
        free(buffer_json);
    }
    if (clipboard_text) {
        char *clip_json = json_escape(clipboard_text);
        fprintf(state->log_file, ",\"clipboard\":%s", clip_json);
        free(clip_json);
    }
    fputs("}\n", state->log_file);
    fflush(state->log_file);
}

static void write_snapshot(struct State *state, struct Buffer *buf, bool force) {
    if (state->log_mode == LOG_MODE_EVENTS) {
        return;
    }
    double now = now_seconds();
    if (!force && now - buf->last_snapshot < state->snapshot_interval) {
        return;
    }
    char path[PATH_MAX];
    append_path(path, sizeof(path), state->snapshot_dir, buf->slug);
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

static void flush_idle_buffers(struct State *state, bool force_all) {
    if (state->log_mode == LOG_MODE_EVENTS || state->buffers.len == 0) {
        return;
    }

    double now = now_seconds();
    for (size_t i = 0; i < state->buffers.len; ++i) {
        struct Buffer *buf = &state->buffers.items[i];
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

static void trim_newline(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

static char *read_command_output(const char *cmd) {
    FILE *pipe = popen(cmd, "r");
    if (!pipe) return NULL;
    size_t cap = 1024;
    size_t len = 0;
    char *data = malloc(cap);
    if (!data) {
        pclose(pipe);
        return NULL;
    }
    int c;
    while ((c = fgetc(pipe)) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(data, cap);
            if (!tmp) {
                free(data);
                pclose(pipe);
                return NULL;
            }
            data = tmp;
        }
        data[len++] = (char)c;
    }
    data[len] = '\0';
    pclose(pipe);
    if (len == 0) {
        free(data);
        return NULL;
    }
    return data;
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

static void update_context(struct State *state) {
    double now = now_seconds();
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

    char cmd[256];
    if (state->hypr_signature && *state->hypr_signature) {
        snprintf(cmd, sizeof(cmd), "%s --instance %s activewindow -j",
                 state->hyprctl_cmd, state->hypr_signature);
    } else {
        snprintf(cmd, sizeof(cmd), "%s activewindow -j", state->hyprctl_cmd);
    }
    char *json = read_command_output(cmd);
    if (!json) return;

    char title[256] = "untitled";
    char clazz[128] = "unknown";
    char address[64] = "0x0";

    extract_json_field(json, "title", title, sizeof(title));
    extract_json_field(json, "class", clazz, sizeof(clazz));
    extract_json_field(json, "address", address, sizeof(address));

    char combined[512];
    snprintf(combined, sizeof(combined), "%s (%s) [%s]", title, clazz, address);
    trim_newline(combined);

    if (strcmp(combined, state->current_context) != 0) {
        char previous[512];
        strncpy(previous, state->current_context, sizeof(previous));
        previous[sizeof(previous) - 1] = '\0';

        strncpy(state->current_context, combined, sizeof(state->current_context));
        state->current_context[sizeof(state->current_context) - 1] = '\0';

        if (previous[0]) {
            struct Buffer *prev = buffer_lookup(&state->buffers, previous, false);
            if (prev) {
                write_snapshot(state, prev, true);
            }
        }
        log_event(state, "focus", state->current_context, NULL, false, NULL, NULL);
    }

    free(json);
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

static char translate_char(struct State *state, int code) {
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

static char *read_clipboard(enum ClipboardMode mode) {
    if (mode != CLIPBOARD_AUTO) return NULL;
    char *clip = read_command_output("wl-paste -n");
    if (clip) {
        trim_newline(clip);
        return clip;
    }
    clip = read_command_output("xclip -selection clipboard -o");
    if (clip) {
        trim_newline(clip);
    }
    return clip;
}

static void process_key(struct State *state, int code, const char *key_name, const char *utf8_text) {
    update_context(state);

    struct Buffer *buf = buffer_lookup(&state->buffers, state->current_context[0] ? state->current_context : "unknown", true);

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
        default:
            if (state->modifiers[MOD_CTRL] && (code == KEY_V || code == KEY_INSERT)) {
                clipboard = read_clipboard(state->clipboard_mode);
                if (clipboard) {
                    buffer_append(buf, clipboard, strlen(clipboard));
                    changed = true;
                }
            } else {
                if (utf8_text && *utf8_text) {
                    buffer_append(buf, utf8_text, strlen(utf8_text));
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

    if (changed) {
        buf->last_update = now_seconds();
        write_snapshot(state, buf, force_snapshot);
    }

    if (state->log_mode != LOG_MODE_SNAPSHOTS) {
        log_event(state, "press", buf->context, key_name, changed, NULL, clipboard);
    }

    free(clipboard);
}

static void update_modifiers(struct State *state, int code, bool pressed) {
    switch (code) {
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT:
            state->modifiers[MOD_SHIFT] = pressed;
            break;
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:
            state->modifiers[MOD_CTRL] = pressed;
            break;
        case KEY_LEFTALT:
        case KEY_RIGHTALT:
            state->modifiers[MOD_ALT] = pressed;
            break;
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA:
            state->modifiers[MOD_SUPER] = pressed;
            break;
        case KEY_CAPSLOCK:
            if (pressed) state->capslock = !state->capslock;
            break;
        default:
            break;
    }
}

static void init_state(struct State *state,
                       const char *log_dir,
                       const char *snapshot_dir,
                       const char *hyprctl_cmd,
                       double snapshot_interval,
                       double context_refresh,
                       enum ClipboardMode clipboard_mode,
                       enum TranslateMode translate_mode,
                       enum LogMode log_mode,
                       bool context_enabled,
                       const char *xkb_layout,
                       const char *xkb_variant,
                       const char *hypr_signature_path,
                       const char *hypr_user) {
    memset(state, 0, sizeof(*state));
    strncpy(state->log_dir, log_dir, sizeof(state->log_dir));
    strncpy(state->snapshot_dir, snapshot_dir, sizeof(state->snapshot_dir));
    strncpy(state->hyprctl_cmd, hyprctl_cmd, sizeof(state->hyprctl_cmd));
    state->snapshot_interval = snapshot_interval;
    state->context_refresh = context_refresh;
    state->clipboard_mode = clipboard_mode;
    state->translate_mode = translate_mode;
    state->log_mode = log_mode;
    state->context_enabled = context_enabled;
    state->xkb_layout = xkb_layout;
    state->xkb_variant = xkb_variant;
    state->hypr_signature = NULL;

    if (hypr_signature_path) {
        state->hypr_signature = read_trimmed_file(hypr_signature_path);
    } else if (hypr_user) {
        state->hypr_signature = load_hypr_signature_for_user(hypr_user);
    } else {
        const char *env_sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
        if (env_sig && *env_sig) {
            state->hypr_signature = string_dup(env_sig);
        }
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
    append_path(log_path, sizeof(log_path), state->log_dir, log_name);

    state->log_file = fopen(log_path, "a");
    if (!state->log_file) {
        perror("fopen log");
        exit(1);
    }

    if (state->translate_mode == TRANSLATE_XKB) {
        state->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (state->xkb_ctx) {
            struct xkb_rule_names names = {
                .layout = state->xkb_layout,
                .variant = state->xkb_variant,
            };
            state->xkb_keymap = xkb_keymap_new_from_names(state->xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
            if (state->xkb_keymap) {
                state->xkb_state = xkb_state_new(state->xkb_keymap);
            }
        }
        if (!state->xkb_state) {
            state->translate_mode = TRANSLATE_RAW;
        }
    }

    log_event(state, "start", NULL, NULL, false, NULL, NULL);
}

static void cleanup_state(struct State *state) {
    flush_idle_buffers(state, true);
    log_event(state, "stop", NULL, NULL, false, NULL, NULL);
    if (state->log_file) fclose(state->log_file);
    for (size_t i = 0; i < state->buffers.len; ++i) {
        free(state->buffers.items[i].context);
        free(state->buffers.items[i].slug);
        free(state->buffers.items[i].text);
    }
    free(state->buffers.items);
    if (state->xkb_state) xkb_state_unref(state->xkb_state);
    if (state->xkb_keymap) xkb_keymap_unref(state->xkb_keymap);
    if (state->xkb_ctx) xkb_context_unref(state->xkb_ctx);
    free(state->hypr_signature);
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [--log-dir DIR] [--snapshot-dir DIR] [--snapshot-interval SEC]\n"
            "           [--clipboard auto|off] [--context-refresh SEC] [--context hyprland|none]\n"
            "           [--log-mode events|snapshots|both] [--translate xkb|raw]\n"
            "           [--xkb-layout LAYOUT] [--xkb-variant VARIANT]\n"
            "           [--hyprctl CMD] [--hypr-signature PATH] [--hypr-user USER]\n",
            prog);
}

int main(int argc, char **argv) {
    const char *log_dir = "/realm/data/keylog/logs";
    const char *snapshot_dir = "/realm/data/keylog/snapshots";
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

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--log-dir") == 0 && i + 1 < argc) {
            log_dir = argv[++i];
        } else if (strcmp(argv[i], "--snapshot-dir") == 0 && i + 1 < argc) {
            snapshot_dir = argv[++i];
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

    ensure_dir("/realm");
    ensure_dir("/realm/data");
    ensure_dir("/realm/data/keylog");
    ensure_dir(log_dir);
    ensure_dir(snapshot_dir);

    struct State state;
    init_state(&state, log_dir, snapshot_dir, hyprctl_cmd, snapshot_interval, context_refresh,
               clipboard_mode, translate_mode, log_mode, context_enabled, xkb_layout, xkb_variant,
               hypr_signature_path, hypr_user);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int poll_timeout = -1;
    if (state.log_mode != LOG_MODE_EVENTS) {
        double interval_ms = state.snapshot_interval * 1000.0;
        if (interval_ms < 50.0) {
            interval_ms = 50.0;
        } else if (interval_ms > 500.0) {
            interval_ms = 500.0;
        }
        poll_timeout = (int)interval_ms;
    }
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
            flush_idle_buffers(&state, false);
            continue;
        }

        bool saw_hup = (pfd.revents & POLLHUP) != 0;
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            break;
        }

        if (pfd.revents & POLLIN) {
            struct input_event ev;
            ssize_t n = read(STDIN_FILENO, &ev, sizeof(ev));
            if (n == 0) {
                break;
            } else if (n < 0) {
                if (errno == EINTR) continue;
                perror("read");
                break;
            } else if (n != sizeof(ev)) {
                fprintf(stderr, "short read from stdin\n");
                break;
            }

            if (ev.type == EV_KEY) {
                const char *name = keycode_name(ev.code);
                if (state.translate_mode == TRANSLATE_XKB && state.xkb_state) {
                    enum xkb_key_direction dir = (ev.value == 0) ? XKB_KEY_UP : XKB_KEY_DOWN;
                    xkb_state_update_key(state.xkb_state, ev.code + 8, dir);
                }
                if (ev.value == 1 || ev.value == 2) {
                    update_modifiers(&state, ev.code, true);
                    const char *text_ptr = NULL;
                    char utf8_buf[64];
                    if (state.translate_mode == TRANSLATE_XKB && state.xkb_state) {
                        int len = xkb_state_key_get_utf8(state.xkb_state, ev.code + 8, utf8_buf, sizeof utf8_buf);
                        if (len > 0) {
                            utf8_buf[len] = '\0';
                            text_ptr = utf8_buf;
                        }
                    }
                    process_key(&state, ev.code, name, text_ptr);
                } else if (ev.value == 0) {
                    update_modifiers(&state, ev.code, false);
                }
            }

            if (write(STDOUT_FILENO, &ev, sizeof(ev)) != sizeof(ev)) {
                perror("write");
                break;
            }
        }

        if (state.log_mode != LOG_MODE_EVENTS) {
            flush_idle_buffers(&state, false);
        }

        if (saw_hup && !(pfd.revents & POLLIN)) {
            break;
        }
    }

    cleanup_state(&state);
    return 0;
}
