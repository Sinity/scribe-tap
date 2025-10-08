#ifndef BUFFER_H
#define BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Buffer {
    char *context;
    char *slug;
    char *text;
    size_t len;
    size_t cap;
    double last_update;
    double last_snapshot;
    double last_used;
    uint32_t hash;
} Buffer;

struct BufferIndexEntry;

typedef struct BufferList {
    Buffer *items;
    size_t len;
    size_t cap;
    struct BufferIndexEntry *index;
    size_t index_cap;
    size_t index_len;
} BufferList;

void buffer_list_init(BufferList *list);
void buffer_list_free(BufferList *list);
Buffer *buffer_lookup(BufferList *list, const char *context, bool create);
void buffer_append(Buffer *buf, const char *data, size_t len);
void buffer_backspace(Buffer *buf);
void buffer_list_evict_idle(BufferList *list, double now, double max_idle_seconds, size_t max_buffers, bool allow_dirty);

#endif /* BUFFER_H */
