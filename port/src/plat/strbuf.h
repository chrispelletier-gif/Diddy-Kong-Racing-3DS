/* A growing string for the bug report and the flight recorder's text (header only, so it needs no build rule). */
#ifndef PORT_STRBUF_H
#define PORT_STRBUF_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct StrBuf {
    char *data;
    size_t length;
    size_t capacity;
    int failed; /* An allocation failed: later additions do nothing; data is whatever was written before. */
} StrBuf;

/* Makes room for extra more characters and the NUL. */
static inline void sb_reserve(StrBuf *sb, size_t extra) {
    size_t need = sb->length + extra + 1;
    char *data;
    if (sb->failed || need <= sb->capacity) {
        return;
    }
    if (need < sb->capacity * 2) {
        need = sb->capacity * 2;
    }
    data = realloc(sb->data, need);
    if (data == NULL) {
        sb->failed = 1;
        return;
    }
    sb->data = data;
    sb->capacity = need;
}

static inline void sb_add_n(StrBuf *sb, const char *text, size_t n) {
    sb_reserve(sb, n);
    if (sb->failed) {
        return;
    }
    memcpy(sb->data + sb->length, text, n);
    sb->length += n;
    sb->data[sb->length] = '\0';
}

static inline void sb_add(StrBuf *sb, const char *text) {
    sb_add_n(sb, text, strlen(text));
}

static inline void sb_addf(StrBuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static inline void sb_addf(StrBuf *sb, const char *fmt, ...) {
    va_list args;
    int n;
    sb_reserve(sb, 0);
    while (!sb->failed) {
        va_start(args, fmt);
        n = vsnprintf(sb->data + sb->length, sb->capacity - sb->length, fmt, args);
        va_end(args);
        if (n < 0) {
            return;
        }
        if (sb->length + (size_t) n < sb->capacity) {
            sb->length += (size_t) n;
            return;
        }
        sb_reserve(sb, (size_t) n);
    }
}

static inline void sb_free(StrBuf *sb) {
    free(sb->data);
    memset(sb, 0, sizeof(*sb));
}

#endif
