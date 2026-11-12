/* Unit tests for common/protocol.c + bufchain.c + util.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"
#include "bufchain.h"
#include "util.h"

static int g_fail;
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

/* ---- captured frames ---- */
#define MAX_CAP 16
static rdpch_frame cap[MAX_CAP];
static uint8_t cap_pl[MAX_CAP][RDPCH_MAX_PAYLOAD];
static int cap_n;

static void capture(void *user, const rdpch_frame *f)
{
    (void)user;
    if (cap_n < MAX_CAP) {
        /* payload is only valid during the callback: deep-copy it */
        cap[cap_n] = *f;
        memcpy(cap_pl[cap_n], f->payload, f->length);
        cap[cap_n].payload = cap_pl[cap_n];
        cap_n++;
    }
}

static void reset_cap(void)
{
    cap_n = 0;
}

static size_t build(uint8_t *out, uint32_t id, uint8_t cmd, const void *pl,
                    uint32_t n)
{
    return rdpch_frame_build(out, RDPCH_MAX_FRAME, id, cmd, pl, n);
}

/* ---- protocol tests ---- */

static void test_build_roundtrip(void)
{
    uint8_t buf[RDPCH_MAX_FRAME];
    uint8_t payload[300];
    rdpch_parser p;
    size_t n;
    uint32_t i;

    for (i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i * 7);

    n = build(buf, 0x11223344, RDPCH_CMD_DATA, payload, sizeof(payload));
    CHECK(n == RDPCH_FRAME_HEADER_SIZE + sizeof(payload));
    CHECK(buf[0] == 0x44 && buf[1] == 0x33 && buf[2] == 0x22 && buf[3] == 0x11);
    CHECK(buf[4] == RDPCH_CMD_DATA);
    CHECK(buf[5] == 0x2C && buf[6] == 0x01 && buf[7] == 0 && buf[8] == 0);

    rdpch_parser_init(&p, capture, NULL);
    reset_cap();
    CHECK(rdpch_parser_feed(&p, buf, n) == 0);
    CHECK(cap_n == 1);
    CHECK(cap[0].conn_id == 0x11223344);
    CHECK(cap[0].cmd == RDPCH_CMD_DATA);
    CHECK(cap[0].length == sizeof(payload));
    CHECK(memcmp(cap[0].payload, payload, sizeof(payload)) == 0);
}

static void test_byte_at_a_time(void)
{
    uint8_t buf[RDPCH_MAX_FRAME];
    rdpch_parser p;
    size_t n, i;

    n = build(buf, 7, RDPCH_CMD_CLOSE, "\x02", 1);
    n += build(buf + n, 42, RDPCH_CMD_DATA, "hello", 5);

    rdpch_parser_init(&p, capture, NULL);
    reset_cap();
    for (i = 0; i < n; i++)
        CHECK(rdpch_parser_feed(&p, buf + i, 1) == 0);
    CHECK(cap_n == 2);
    CHECK(cap[0].cmd == RDPCH_CMD_CLOSE && cap[0].conn_id == 7);
    CHECK(cap[0].length == 1 && cap[0].payload[0] == RDPCH_CLOSE_OVERFLOW);
    CHECK(cap[1].cmd == RDPCH_CMD_DATA && cap[1].conn_id == 42);
    CHECK(cap[1].length == 5 && memcmp(cap[1].payload, "hello", 5) == 0);
}

static void test_random_fragmentation(void)
{
    uint8_t stream[8192];
    size_t off = 0, i;
    rdpch_parser p;
    int total = 0;
    uint32_t ids[] = { 1, 2, 3, 0xdeadbeef, 300 };

    srand(12345);
    for (i = 0; i < 5; i++) {
        uint8_t payload[700];
        uint32_t j, plen = (uint32_t)(rand() % sizeof(payload)) + 1;
        for (j = 0; j < plen; j++)
            payload[j] = (uint8_t)(rand());
        off += build(stream + off, ids[i], RDPCH_CMD_DATA, payload, plen);
        total++;
    }

    rdpch_parser_init(&p, capture, NULL);
    reset_cap();
    off = 0;
    while (off < sizeof(stream) && cap_n < total) {
        size_t chunk = (size_t)(rand() % 97) + 1;
        if (off + chunk > sizeof(stream))
            chunk = sizeof(stream) - off;
        CHECK(rdpch_parser_feed(&p, stream + off, chunk) == 0);
        off += chunk;
    }
    CHECK(cap_n == total);
    for (i = 0; i < 5; i++) {
        CHECK(cap[i].conn_id == ids[i]);
        CHECK(cap[i].cmd == RDPCH_CMD_DATA);
    }
}

static void test_protocol_errors(void)
{
    uint8_t buf[RDPCH_MAX_FRAME];
    rdpch_parser p;
    size_t n;

    /* bad cmd */
    n = build(buf, 1, 99, "x", 1);
    rdpch_parser_init(&p, capture, NULL);
    CHECK(rdpch_parser_feed(&p, buf, n) == -1);

    /* oversized length: 1401 = 0x0579 */
    buf[0] = buf[1] = buf[2] = buf[3] = 0;
    buf[4] = RDPCH_CMD_DATA;
    buf[5] = 0x79; buf[6] = 0x05; buf[7] = 0; buf[8] = 0;
    rdpch_parser_init(&p, capture, NULL);
    CHECK(rdpch_parser_feed(&p, buf, 9) == -1);

    /* zero-length DATA */
    n = build(buf, 1, RDPCH_CMD_DATA, NULL, 0);
    rdpch_parser_init(&p, capture, NULL);
    CHECK(rdpch_parser_feed(&p, buf, n) == -1);

    /* truncated header is fine, must not error */
    rdpch_parser_init(&p, capture, NULL);
    CHECK(rdpch_parser_feed(&p, "12345", 5) == 0);
}

static void test_open_hello_codec(void)
{
    uint8_t pl[300];
    uint32_t n;
    uint16_t port;
    char host[256];

    CHECK(rdpch_hello_encode(pl, sizeof(pl), &n) == 0 && n == 6);
    CHECK(rdpch_hello_check(pl, 6) == 0);
    CHECK(rdpch_hello_check(pl, 5) == -1);
    pl[0] = RDPCH_PROTO_VERSION + 1; /* wrong version */
    CHECK(rdpch_hello_check(pl, 6) == -1);

    CHECK(rdpch_open_encode(pl, sizeof(pl), &n, 8080, "example.com", 11) == 0);
    CHECK(n == 15);
    CHECK(rdpch_open_decode(pl, n, &port, host, sizeof(host)) == 0);
    CHECK(port == 8080 && strcmp(host, "example.com") == 0);

    CHECK(rdpch_open_encode(pl, sizeof(pl), &n, 443, "", 0) == -1);
    CHECK(rdpch_open_decode(pl, 3, &port, host, sizeof(host)) == -1);
    CHECK(rdpch_open_decode(pl, 4 + 300, &port, host, sizeof(host)) == -1);
}

/* ---- bufchain tests ---- */

static void test_bufchain(void)
{
    bufchain b;
    uint8_t big[10000];
    uint8_t tmp[100];
    const uint8_t *seg;
    size_t i, pos;
    char host[64];
    uint16_t port;

    for (i = 0; i < sizeof(big); i++)
        big[i] = (uint8_t)(i ^ 0x5a);

    bc_init(&b);
    CHECK(bc_len(&b) == 0);
    CHECK(bc_append(&b, big, sizeof(big)) == 0);
    CHECK(bc_len(&b) == sizeof(big));
    CHECK(bc_append_str(&b, " tail") == 0);
    CHECK(bc_len(&b) == sizeof(big) + 5);

    /* copy across nodes */
    CHECK(bc_copy_head(&b, tmp, sizeof(tmp)) == sizeof(tmp));
    CHECK(memcmp(tmp, big, sizeof(tmp)) == 0);

    /* consume across nodes */
    bc_consume(&b, 4000);
    CHECK(bc_len(&b) == sizeof(big) + 5 - 4000);
    CHECK(bc_copy_head(&b, tmp, 8) == 8);
    CHECK(memcmp(tmp, big + 4000, 8) == 0);

    /* find (offset past big[] so the first 't' is the one in " tail";
     * 4000 bytes were consumed, so big[] now ends at 6000) */
    CHECK(bc_find(&b, sizeof(big) - 4000, (uint8_t)'t', &pos) == 1);
    CHECK(pos == sizeof(big) - 4000 + 1);
    CHECK(bc_find(&b, sizeof(big) + 5 - 4000, 'x', &pos) == 0);

    /* peek */
    bc_free(&b);
    bc_init(&b);
    CHECK(bc_peek(&b, &seg) == 0);
    CHECK(bc_append(&b, "hello", 5) == 0);
    CHECK(bc_peek(&b, &seg) == 5 && memcmp(seg, "hello", 5) == 0);
    bc_consume(&b, 2);
    CHECK(bc_peek(&b, &seg) == 3 && memcmp(seg, "llo", 3) == 0);
    bc_consume(&b, 3);
    CHECK(bc_len(&b) == 0 && b.head == NULL);
    bc_free(&b);

    /* authority parsing */
    CHECK(rdpch_parse_authority("example.com", 11, host, sizeof(host), &port, 443) == 0);
    CHECK(strcmp(host, "example.com") == 0 && port == 443);
    CHECK(rdpch_parse_authority("example.com:8443", 16, host, sizeof(host), &port, 80) == 0);
    CHECK(strcmp(host, "example.com") == 0 && port == 8443);
    CHECK(rdpch_parse_authority("[::1]:8443", 10, host, sizeof(host), &port, 443) == 0);
    CHECK(strcmp(host, "::1") == 0 && port == 8443);
    CHECK(rdpch_parse_authority("[2001:db8::1]", 13, host, sizeof(host), &port, 443) == 0);
    CHECK(strcmp(host, "2001:db8::1") == 0 && port == 443);
    CHECK(rdpch_parse_authority("2001:db8::1", 11, host, sizeof(host), &port, 80) == 0);
    CHECK(strcmp(host, "2001:db8::1") == 0 && port == 80);
    CHECK(rdpch_parse_authority("", 0, host, sizeof(host), &port, 80) == -1);
    CHECK(rdpch_parse_authority("h:0", 3, host, sizeof(host), &port, 80) == -1);
    CHECK(rdpch_parse_authority("h:99999", 7, host, sizeof(host), &port, 80) == -1);
}

int main(void)
{
    test_build_roundtrip();
    test_byte_at_a_time();
    test_random_fragmentation();
    test_protocol_errors();
    test_open_hello_codec();
    test_bufchain();

    if (g_fail) {
        printf("test_protocol: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_protocol: all passed\n");
    return 0;
}
