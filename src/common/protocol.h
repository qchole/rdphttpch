/* rdpch - RDP virtual channel HTTP/HTTPS forwarder
 *
 * Wire protocol shared by server (rdpchsrv.exe) and client (rdpchcli.dll).
 *
 * Frame layout (all integers little-endian):
 *
 *   +---------+--------+--------+---------------------------+
 *   | conn_id |  cmd   | length | payload (0..1400 bytes)   |
 *   |  u32    |   u8   |  u32   |                           |
 *   +---------+--------+--------+---------------------------+
 *   <--------- 9 byte header ------><---------- length ------>
 *
 * The underlying virtual channel (or mock TCP channel) is an unordered
 * chunked byte stream with no message semantics, so both sides feed raw
 * bytes into rdpch_parser and get complete frames back.
 */
#ifndef RDPCH_PROTOCOL_H
#define RDPCH_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define RDPCH_FRAME_HEADER_SIZE 9
#define RDPCH_MAX_PAYLOAD       1400u
#define RDPCH_MAX_FRAME         (RDPCH_FRAME_HEADER_SIZE + RDPCH_MAX_PAYLOAD)

#define RDPCH_PROTO_VERSION 1
#define RDPCH_HOST_MAX      253

/* commands */
enum {
    RDPCH_CMD_HELLO     = 1,
    RDPCH_CMD_OPEN      = 2,
    RDPCH_CMD_OPEN_OK   = 3,
    RDPCH_CMD_OPEN_FAIL = 4,
    RDPCH_CMD_DATA      = 5,
    RDPCH_CMD_CLOSE     = 6,
    RDPCH_CMD_PING      = 7,
    RDPCH_CMD_PONG      = 8,
    RDPCH_CMD_BYE       = 9,
};

/* OPEN_FAIL reasons */
enum {
    RDPCH_FAIL_DNS     = 1,
    RDPCH_FAIL_REFUSED = 2,
    RDPCH_FAIL_TIMEOUT = 3,
    RDPCH_FAIL_LIMIT   = 4,
    RDPCH_FAIL_PROTO   = 5,
};

/* CLOSE reasons */
enum {
    RDPCH_CLOSE_EOF      = 0,
    RDPCH_CLOSE_ERROR    = 1,
    RDPCH_CLOSE_OVERFLOW = 2,
};

/* conn_id 0 is reserved for control frames (HELLO/BYE/PING/PONG) */
#define RDPCH_CONN_ID_CTRL 0u

typedef struct {
    uint32_t        conn_id;
    uint8_t         cmd;
    uint32_t        length;
    const uint8_t  *payload; /* valid only during the callback */
} rdpch_frame;

typedef void (*rdpch_on_frame)(void *user, const rdpch_frame *f);

typedef struct {
    rdpch_on_frame  cb;
    void           *user;
    uint8_t         hdr[RDPCH_FRAME_HEADER_SIZE];
    size_t          hdr_have;
    uint8_t         payload[RDPCH_MAX_PAYLOAD];
    uint32_t        pay_need;
    uint32_t        pay_have;
} rdpch_parser;

void rdpch_parser_init(rdpch_parser *p, rdpch_on_frame cb, void *user);
/* Feed raw channel bytes; returns 0 on success, -1 on unrecoverable
 * protocol error (caller must kill the transport). */
int rdpch_parser_feed(rdpch_parser *p, const void *data, size_t len);
void rdpch_parser_reset(rdpch_parser *p);

/* Build a complete frame into out; returns total size or 0 if cap too small. */
size_t rdpch_frame_build(uint8_t *out, size_t cap, uint32_t conn_id, uint8_t cmd,
                         const void *payload, uint32_t length);

int rdpch_cmd_is_valid(uint8_t cmd);

/* HELLO payload: ver u16, max_payload u16, reserved u16 */
int  rdpch_hello_encode(uint8_t *payload, size_t cap, uint32_t *out_len);
/* returns 0 if payload is a valid matching HELLO */
int  rdpch_hello_check(const uint8_t *payload, uint32_t len);

/* OPEN payload: port u16, hostlen u16, host bytes (ASCII) */
int  rdpch_open_encode(uint8_t *payload, size_t cap, uint32_t *out_len,
                       uint16_t port, const char *host, size_t hostlen);
int  rdpch_open_decode(const uint8_t *payload, uint32_t len,
                       uint16_t *port, char *host, size_t hostcap);

#endif /* RDPCH_PROTOCOL_H */
