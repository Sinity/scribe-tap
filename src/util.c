#define _GNU_SOURCE
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>

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

double util_now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void util_iso8601(char *buf, size_t len) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    int millis = (int)(ts.tv_nsec / 1000000);
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm.tm_year + 1900,
             tm.tm_mon + 1,
             tm.tm_mday,
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec,
             millis);
}

void util_ensure_dir_tree(const char *path) {
    if (!path || !*path) return;
    char tmp[PATH_MAX];
    size_t path_len = strlen(path);
    if (path_len >= sizeof(tmp)) {
        fprintf(stderr, "path too long: %s\n", path);
        exit(1);
    }
    strncpy(tmp, path, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';
    size_t len = strlen(tmp);
    if (!len) return;

    for (size_t i = 1; i < len; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (*tmp) {
                ensure_dir(tmp);
            }
            tmp[i] = '/';
        }
    }
    ensure_dir(tmp);
}

void util_append_path(char *dest, size_t dest_len, const char *dir, const char *leaf) {
    int written = snprintf(dest, dest_len, "%s/%s", dir, leaf);
    if (written < 0 || (size_t)written >= dest_len) {
        fprintf(stderr, "path too long: %s/%s\n", dir ? dir : "", leaf ? leaf : "");
        exit(1);
    }
}

char *util_string_dup(const char *src) {
    char *dup = strdup(src ? src : "");
    if (!dup) {
        perror("strdup");
        exit(1);
    }
    return dup;
}

void util_rstrip_whitespace(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

char *util_read_trimmed_file(const char *path) {
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
    util_rstrip_whitespace(line);
    return line;
}

static void append_json_escape(char **out, size_t *cap, size_t *len, const char *buf, size_t buf_len) {
    if (*len + buf_len + 1 >= *cap) {
        while (*len + buf_len + 1 >= *cap) {
            *cap *= 2;
        }
        char *tmp = realloc(*out, *cap);
        if (!tmp) {
            perror("realloc");
            exit(1);
        }
        *out = tmp;
    }
    memcpy(*out + *len, buf, buf_len);
    *len += buf_len;
}

char *util_json_escape(const char *src) {
    const char *input = src ? src : "";
    size_t len = strlen(input);
    size_t cap = len * 2 + 3;
    if (cap < 16) cap = 16;
    char *out = malloc(cap);
    if (!out) {
        perror("malloc");
        exit(1);
    }
    size_t j = 0;
    out[j++] = '"';
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)input[i];
        switch (c) {
            case '\\':
            case '"': {
                char buf[2] = {'\\', (char)c};
                append_json_escape(&out, &cap, &j, buf, sizeof buf);
                break;
            }
            case '\n':
                append_json_escape(&out, &cap, &j, "\\n", 2);
                break;
            case '\t':
                append_json_escape(&out, &cap, &j, "\\t", 2);
                break;
            case '\r':
                append_json_escape(&out, &cap, &j, "\\r", 2);
                break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    append_json_escape(&out, &cap, &j, buf, strlen(buf));
                } else {
                    append_json_escape(&out, &cap, &j, (const char *)&c, 1);
                }
        }
    }
    append_json_escape(&out, &cap, &j, "\"", 1);
    out[j] = '\0';
    return out;
}

void util_trim_newline(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}
