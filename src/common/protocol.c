#include "protocol.h"

#include <string.h>

#include "log.h"

static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void wr_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void rdpch_parser_init(rdpch_parser *p, rdpch_on_frame cb, void *user)
{
    memset(p, 0, sizeof(*p));
    p->cb   = cb;
    p->user = user;
}

void rdpch_parser_reset(rdpch_parser *p)
{
    p->hdr_have = 0;
    p->pay_need = 0;
    p->pay_have = 0;
}

int rdpch_cmd_is_valid(uint8_t cmd)
{
    return cmd >= RDPCH_CMD_HELLO && cmd <= RDPCH_CMD_BYE;
}

int rdpch_parser_feed(rdpch_parser *p, const void *data, size_t len)
{
    const uint8_t *src = (const uint8_t *)data;

    while (len > 0) {
        if (p->hdr_have < RDPCH_FRAME_HEADER_SIZE) {
            size_t n = RDPCH_FRAME_HEADER_SIZE - p->hdr_have;
            if (n > len)
                n = len;
            memcpy(p->hdr + p->hdr_have, src, n);
            p->hdr_have += n;
            src += n;
            len -= n;
            if (p->hdr_have < RDPCH_FRAME_HEADER_SIZE)
                return 0;

            p->pay_need = rd_u32le(p->hdr + 5);
            p->pay_have = 0;
            if (!rdpch_cmd_is_valid(p->hdr[4]))
                return -1;
            if (p->pay_need > RDPCH_MAX_PAYLOAD)
                return -1;
            if (p->hdr[4] == RDPCH_CMD_DATA && p->pay_need == 0)
                return -1;
        }

        if (p->pay_have < p->pay_need) {
            uint32_t n = p->pay_need - p->pay_have;
            if (n > len)
                n = (uint32_t)len;
            memcpy(p->payload + p->pay_have, src, n);
            p->pay_have += n;
            src += n;
            len -= n;
        }

        if (p->pay_have == p->pay_need) {
            rdpch_frame f;
            f.conn_id = rd_u32le(p->hdr);
            f.cmd     = p->hdr[4];
            f.length  = p->pay_need;
            f.payload = p->payload;
            p->hdr_have = 0;
            p->pay_need = 0;
            p->pay_have = 0;
            RLOGD("proto: frame conn=%u cmd=%u len=%u",
                  f.conn_id, f.cmd, f.length);
            if (p->cb)
                p->cb(p->user, &f);
        }
    }
    return 0;
}

size_t rdpch_frame_build(uint8_t *out, size_t cap, uint32_t conn_id, uint8_t cmd,
                         const void *payload, uint32_t length)
{
    if (cap < RDPCH_FRAME_HEADER_SIZE + (size_t)length)
        return 0;
    wr_u32le(out, conn_id);
    out[4] = cmd;
    wr_u32le(out + 5, length);
    if (length && payload)
        memcpy(out + RDPCH_FRAME_HEADER_SIZE, payload, length);
    return RDPCH_FRAME_HEADER_SIZE + (size_t)length;
}

int rdpch_hello_encode(uint8_t *payload, size_t cap, uint32_t *out_len)
{
    if (cap < 6)
        return -1;
    wr_u16le(payload, RDPCH_PROTO_VERSION);
    wr_u16le(payload + 2, RDPCH_MAX_PAYLOAD);
    wr_u16le(payload + 4, 0);
    *out_len = 6;
    return 0;
}

int rdpch_hello_check(const uint8_t *payload, uint32_t len)
{
    if (len != 6)
        return -1;
    if (rd_u16le(payload) != RDPCH_PROTO_VERSION)
        return -1;
    return 0;
}

int rdpch_open_encode(uint8_t *payload, size_t cap, uint32_t *out_len,
                      uint16_t port, const char *host, size_t hostlen)
{
    if (hostlen == 0 || hostlen > RDPCH_HOST_MAX)
        return -1;
    if (cap < 4 + hostlen)
        return -1;
    wr_u16le(payload, port);
    wr_u16le(payload + 2, (uint16_t)hostlen);
    memcpy(payload + 4, host, hostlen);
    *out_len = (uint32_t)(4 + hostlen);
    return 0;
}

int rdpch_open_decode(const uint8_t *payload, uint32_t len,
                      uint16_t *port, char *host, size_t hostcap)
{
    uint16_t hostlen;
    if (len < 4)
        return -1;
    *port = rd_u16le(payload);
    hostlen = rd_u16le(payload + 2);
    if ((uint32_t)hostlen + 4 != len || hostlen == 0)
        return -1;
    if (hostcap < (size_t)hostlen + 1)
        return -1;
    memcpy(host, payload + 4, hostlen);
    host[hostlen] = '\0';
    return 0;
}
