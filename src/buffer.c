#include "buffer.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

typedef struct BufferIndexEntry {
    const char *key;
    size_t index;
    uint32_t hash;
    unsigned char state;
} BufferIndexEntry;

enum {
    BUFFER_INDEX_EMPTY = 0,
    BUFFER_INDEX_OCCUPIED = 1,
    BUFFER_INDEX_TOMBSTONE = 2,
};

static size_t next_pow2(size_t value) {
    if (value < 8) return 8;
    value--;
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
#if SIZE_MAX > UINT32_MAX
    value |= value >> 32;
#endif
    return value + 1;
}

static void buffer_index_reset(BufferList *list) {
    list->index = NULL;
    list->index_cap = 0;
    list->index_len = 0;
}

void buffer_list_init(BufferList *list) {
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
    buffer_index_reset(list);
}

static uint32_t fnv1a32(const char *src) {
    const unsigned char *ptr = (const unsigned char *)(src ? src : "");
    uint32_t hash = 2166136261u;
    while (*ptr) {
        hash ^= *ptr++;
        hash *= 16777619u;
    }
    return hash;
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
    const char *input = src ? src : "";
    size_t cap = 128;
    char *copy = calloc(cap, 1);
    if (!copy) {
        perror("calloc");
        exit(1);
    }
    strncpy(copy, input, cap - 1);
    copy[cap - 1] = '\0';
    sanitize_slug(copy);
    if (!copy[0]) {
        strcpy(copy, "window");
    }

    uint32_t hash = fnv1a32(input);
    char suffix[10];
    snprintf(suffix, sizeof(suffix), "-%06x", hash & 0xFFFFFFu);
    size_t suffix_len = strlen(suffix);
    size_t max_total = 80;
    size_t base_len = strlen(copy);
    if (base_len + suffix_len > max_total) {
        if (max_total > suffix_len) {
            copy[max_total - suffix_len] = '\0';
        } else {
            copy[0] = '\0';
        }
        base_len = strlen(copy);
    }
    strncat(copy, suffix, cap - base_len - 1);
    return copy;
}

static BufferIndexEntry *buffer_index_lookup(BufferList *list, const char *context, uint32_t hash, BufferIndexEntry **insert_slot) {
    if (list->index_cap == 0) {
        if (insert_slot) *insert_slot = NULL;
        return NULL;
    }
    size_t mask = list->index_cap - 1;
    size_t pos = (size_t)hash & mask;
    BufferIndexEntry *tombstone = NULL;
    for (;;) {
        BufferIndexEntry *entry = &list->index[pos];
        if (entry->state == BUFFER_INDEX_EMPTY) {
            if (insert_slot) *insert_slot = tombstone ? tombstone : entry;
            return NULL;
        }
        if (entry->state == BUFFER_INDEX_TOMBSTONE) {
            if (!tombstone) tombstone = entry;
        } else if (entry->hash == hash && strcmp(entry->key, context) == 0) {
            if (insert_slot) *insert_slot = entry;
            return entry;
        }
        pos = (pos + 1) & mask;
    }
}

static void buffer_index_grow(BufferList *list) {
    size_t new_cap = list->index_cap ? list->index_cap * 2 : 16;
    new_cap = next_pow2(new_cap);

    BufferIndexEntry *old_entries = list->index;
    size_t old_cap = list->index_cap;

    list->index = calloc(new_cap, sizeof(BufferIndexEntry));
    if (!list->index) {
        perror("calloc");
        exit(1);
    }
    list->index_cap = new_cap;
    list->index_len = 0;

    if (!old_entries) {
        return;
    }

    for (size_t i = 0; i < old_cap; ++i) {
        BufferIndexEntry *entry = &old_entries[i];
        if (entry->state != BUFFER_INDEX_OCCUPIED) continue;

        BufferIndexEntry *insert_slot = NULL;
        buffer_index_lookup(list, entry->key, entry->hash, &insert_slot);
        if (!insert_slot) {
            fprintf(stderr, "buffer index insert failed during rehash\n");
            exit(1);
        }
        *insert_slot = *entry;
        insert_slot->state = BUFFER_INDEX_OCCUPIED;
        list->index_len++;
    }

    free(old_entries);
}

static void buffer_index_insert(BufferList *list, const char *context, uint32_t hash, size_t index) {
    if ((list->index_len + 1) * 4 >= list->index_cap * 3) {
        buffer_index_grow(list);
    } else if (list->index_cap == 0) {
        buffer_index_grow(list);
    }

    BufferIndexEntry *slot = NULL;
    buffer_index_lookup(list, context, hash, &slot);
    if (!slot) {
        fprintf(stderr, "buffer index insert failed\n");
        exit(1);
    }
    if (slot->state == BUFFER_INDEX_OCCUPIED) {
        slot->index = index;
        return;
    }
    slot->key = context;
    slot->hash = hash;
    slot->index = index;
    slot->state = BUFFER_INDEX_OCCUPIED;
    list->index_len++;
}

static void buffer_index_remove(BufferList *list, const char *context, uint32_t hash) {
    BufferIndexEntry *slot = buffer_index_lookup(list, context, hash, NULL);
    if (!slot) {
        return;
    }
    slot->key = NULL;
    slot->hash = 0;
    slot->state = BUFFER_INDEX_TOMBSTONE;
    if (list->index_len > 0) {
        list->index_len--;
    }
}

static void buffer_index_update(BufferList *list, const char *context, uint32_t hash, size_t new_index) {
    BufferIndexEntry *slot = buffer_index_lookup(list, context, hash, NULL);
    if (!slot) {
        buffer_index_insert(list, context, hash, new_index);
        return;
    }
    slot->index = new_index;
}

static void buffer_list_reserve(BufferList *list, size_t needed) {
    if (needed <= list->cap) return;
    size_t new_cap = list->cap ? list->cap : 8;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    Buffer *tmp = realloc(list->items, new_cap * sizeof(Buffer));
    if (!tmp) {
        perror("realloc");
        exit(1);
    }
    list->items = tmp;
    list->cap = new_cap;
}

static Buffer *buffer_list_emplace(BufferList *list, const char *context, uint32_t hash) {
    buffer_list_reserve(list, list->len + 1);
    Buffer *buf = &list->items[list->len++];
    memset(buf, 0, sizeof(*buf));
    buf->context = util_string_dup(context);
    buf->slug = make_slug(context);
    buf->cap = 1024;
    buf->text = calloc(buf->cap, 1);
    if (!buf->text) {
        perror("calloc");
        exit(1);
    }
    buf->hash = hash;
    buffer_index_insert(list, buf->context, hash, list->len - 1);
    return buf;
}

Buffer *buffer_lookup(BufferList *list, const char *context, bool create) {
    if (!context) {
        context = "";
    }
    uint32_t hash = fnv1a32(context);
    BufferIndexEntry *slot = buffer_index_lookup(list, context, hash, NULL);
    if (slot) {
        Buffer *buf = &list->items[slot->index];
        buf->last_used = util_now_seconds();
        return buf;
    }
    if (!create) {
        return NULL;
    }
    Buffer *buf = buffer_list_emplace(list, context, hash);
    buf->last_used = util_now_seconds();
    return buf;
}

void buffer_append(Buffer *buf, const char *data, size_t len) {
    if (!len) return;
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

static size_t utf8_prev_char_len(const char *s, size_t len) {
    if (!s || len == 0) {
        return 0;
    }
    size_t i = len;
    while (i > 0) {
        --i;
        unsigned char c = (unsigned char)s[i];
        if ((c & 0x80u) == 0) {
            return len - i;
        }
        if ((c & 0xC0u) == 0x80u) {
            continue;
        }
        if ((c & 0xE0u) == 0xC0u || (c & 0xF0u) == 0xE0u || (c & 0xF8u) == 0xF0u) {
            return len - i;
        }
        return len - i;
    }
    return len;
}

void buffer_backspace(Buffer *buf) {
    if (buf->len == 0) return;
    size_t char_len = utf8_prev_char_len(buf->text, buf->len);
    if (char_len == 0 || char_len > buf->len) {
        char_len = 1;
    }
    buf->len -= char_len;
    buf->text[buf->len] = '\0';
}

void buffer_list_free(BufferList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->len; ++i) {
        free(list->items[i].context);
        free(list->items[i].slug);
        free(list->items[i].text);
    }
    free(list->items);
    free(list->index);
    list->items = NULL;
    list->index = NULL;
    list->len = 0;
    list->cap = 0;
    list->index_cap = 0;
    list->index_len = 0;
}

static void buffer_list_remove_at(BufferList *list, size_t idx) {
    if (!list || idx >= list->len) return;
    Buffer *buf = &list->items[idx];
    buffer_index_remove(list, buf->context, buf->hash);

    free(buf->context);
    free(buf->slug);
    free(buf->text);

    size_t last = list->len - 1;
    if (idx != last) {
        list->items[idx] = list->items[last];
        buffer_index_update(list,
                            list->items[idx].context,
                            list->items[idx].hash,
                            idx);
    }
    list->len--;
}

void buffer_list_evict_idle(BufferList *list, double now, double max_idle_seconds, size_t max_buffers, bool allow_dirty_removal) {
    if (!list || list->len == 0) return;

    if (max_idle_seconds > 0.0) {
        size_t i = 0;
        while (i < list->len) {
            Buffer *buf = &list->items[i];
            double idle = now - buf->last_used;
            if (idle > max_idle_seconds &&
                (allow_dirty_removal || buf->last_snapshot >= buf->last_update)) {
                buffer_list_remove_at(list, i);
                continue;
            }
            ++i;
        }
    }

    if (max_buffers == 0 || list->len <= max_buffers) {
        return;
    }

    while (list->len > max_buffers) {
        size_t candidate = SIZE_MAX;
        double oldest_used = now;
        for (size_t i = 0; i < list->len; ++i) {
            Buffer *buf = &list->items[i];
            if (!allow_dirty_removal && buf->last_snapshot < buf->last_update) {
                continue;
            }
            if (candidate == SIZE_MAX || buf->last_used <= oldest_used) {
                oldest_used = buf->last_used;
                candidate = i;
            }
        }
        if (candidate == SIZE_MAX) {
            break;
        }
        buffer_list_remove_at(list, candidate);
    }
}
