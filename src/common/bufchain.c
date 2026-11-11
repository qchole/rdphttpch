#include "bufchain.h"

#include <stdlib.h>
#include <string.h>

#define BC_NODE_CAP 4096

void bc_init(bufchain *b)
{
    b->head = b->tail = NULL;
    b->len = 0;
}

void bc_free(bufchain *b)
{
    bc_node *n = b->head;
    while (n) {
        bc_node *next = n->next;
        free(n->data);
        free(n);
        n = next;
    }
    bc_init(b);
}

int bc_append(bufchain *b, const void *data, size_t n)
{
    const uint8_t *src = (const uint8_t *)data;

    if (n == 0)
        return 0;

    if (b->tail && b->tail->off + b->tail->len + n <= BC_NODE_CAP) {
        memcpy(b->tail->data + b->tail->off + b->tail->len, src, n);
        b->tail->len += n;
        b->len += n;
        return 0;
    }

    while (n > 0) {
        size_t chunk = n > BC_NODE_CAP ? BC_NODE_CAP : n;
        bc_node *node = (bc_node *)malloc(sizeof(bc_node));
        if (!node)
            return -1;
        node->data = (uint8_t *)malloc(BC_NODE_CAP);
        if (!node->data) {
            free(node);
            return -1;
        }
        node->next = NULL;
        node->off = 0;
        node->len = chunk;
        memcpy(node->data, src, chunk);
        if (b->tail)
            b->tail->next = node;
        else
            b->head = node;
        b->tail = node;
        src += chunk;
        n -= chunk;
        b->len += chunk;
    }
    return 0;
}

int bc_append_str(bufchain *b, const char *s)
{
    return bc_append(b, s, strlen(s));
}

size_t bc_len(const bufchain *b)
{
    return b->len;
}

size_t bc_peek(const bufchain *b, const uint8_t **seg)
{
    if (!b->head || b->head->len == 0) {
        *seg = NULL;
        return 0;
    }
    *seg = b->head->data + b->head->off;
    return b->head->len;
}

size_t bc_copy_head(const bufchain *b, uint8_t *dst, size_t n)
{
    size_t done = 0;
    bc_node *node = b->head;

    while (node && done < n) {
        size_t take = node->len < n - done ? node->len : n - done;
        memcpy(dst + done, node->data + node->off, take);
        done += take;
        node = node->next;
    }
    return done;
}

void bc_consume(bufchain *b, size_t n)
{
    while (n > 0 && b->head) {
        size_t take = b->head->len < n ? b->head->len : n;
        b->head->off += take;
        b->head->len -= take;
        b->len -= take;
        n -= take;
        if (b->head->len == 0) {
            bc_node *dead = b->head;
            b->head = dead->next;
            if (!b->head)
                b->tail = NULL;
            free(dead->data);
            free(dead);
        }
    }
}

int bc_find(const bufchain *b, size_t offset, uint8_t c, size_t *pos)
{
    size_t base = 0;
    bc_node *node = b->head;

    while (node) {
        size_t node_end = base + node->len;
        if (offset < node_end) {
            size_t start = offset > base ? offset : base;
            const uint8_t *p = node->data + node->off + (start - base);
            const uint8_t *q = memchr(p, c, node_end - start);
            if (q) {
                *pos = start + (size_t)(q - p);
                return 1;
            }
        }
        base = node_end;
        node = node->next;
    }
    return 0;
}
