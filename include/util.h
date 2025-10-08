#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/* time helpers */
double util_now_seconds(void);
void util_iso8601(char *buf, size_t len);

/* filesystem helpers */
void util_ensure_dir_tree(const char *path);
void util_append_path(char *dest, size_t dest_len, const char *dir, const char *leaf);

/* string helpers */
char *util_string_dup(const char *src);
void util_rstrip_whitespace(char *s);
char *util_read_trimmed_file(const char *path);
char *util_json_escape(const char *src);
void util_trim_newline(char *s);

#endif /* UTIL_H */
