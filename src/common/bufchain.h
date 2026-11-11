/* Contiguous-segment linked-list byte buffer.
 * Used for socket send queues and header accumulation on both sides. */
#ifndef RDPCH_BUFCHAIN_H
#define RDPCH_BUFCHAIN_H

#include <stddef.h>
#include <stdint.h>

typedef struct bc_node {
    struct bc_node *next;
    size_t          off; /* start of valid data in data[] */
    size_t          len; /* valid bytes */
    uint8_t        *data;
} bc_node;

typedef struct {
    bc_node *head;
    bc_node *tail;
    size_t   len; /* total buffered bytes */
} bufchain;

void   bc_init(bufchain *b);
void   bc_free(bufchain *b);
/* Append a copy of data; returns 0 on success, -1 on OOM. */
int    bc_append(bufchain *b, const void *data, size_t n);
int    bc_append_str(bufchain *b, const char *s);
size_t bc_len(const bufchain *b);
/* View of the first contiguous segment (0 if empty). */
size_t bc_peek(const bufchain *b, const uint8_t **seg);
/* Copy up to n bytes from the front into dst; returns bytes copied. */
size_t bc_copy_head(const bufchain *b, uint8_t *dst, size_t n);
/* Drop n bytes from the front. */
void   bc_consume(bufchain *b, size_t n);
/* Find the first occurrence of byte c starting at offset (byte index);
 * returns 1 and sets *pos to the absolute index, or 0 if not found. */
int    bc_find(const bufchain *b, size_t offset, uint8_t c, size_t *pos);

#endif /* RDPCH_BUFCHAIN_H */
