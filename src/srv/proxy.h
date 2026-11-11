/* Server-side HTTP proxy (127.0.0.1:listen_port).
 *
 * Accepts browser connections, parses the first request head:
 *   CONNECT host:port  -> OPEN over the channel, on OPEN_OK reply
 *                         "200 Connection established" and tunnel raw bytes
 *   http:// absolute   -> OPEN, then stream through the http_rewriter
 *                         (absolute-form -> origin-form, proxy headers
 *                         stripped) -- supports keep-alive pipelining
 *   origin-form        -> same, target taken from the Host header
 *   https:// absolute  -> 502 (cannot de-tunnel TLS)
 * Channel-bound frames (OPEN_OK/OPEN_FAIL/DATA/CLOSE) are routed back to
 * the owning connection through the table below. */
#ifndef RDPCH_SRV_PROXY_H
#define RDPCH_SRV_PROXY_H

#include <stddef.h>
#include <stdint.h>

#include "transport.h"

typedef struct {
    uint16_t listen_port;
    int      max_conns;           /* cap on concurrent browser connections */
    size_t   sock_buf_max;        /* per-conn channel->browser buffer cap */
    uint32_t open_timeout_ms;     /* wait for OPEN_OK/OPEN_FAIL */
    uint32_t head_timeout_ms;     /* wait for a complete request head */
} proxy_cfg;

void proxy_cfg_default(proxy_cfg *cfg, uint16_t listen_port);

/* Idempotent start; owns WSAStartup for the server process. */
int  proxy_start(const proxy_cfg *cfg);
void proxy_set_xport(rdpch_xport *xp);
/* Must be called BEFORE the xport is started so routing is in place. */
void proxy_stop(void);

rdpch_xport_cbs proxy_cbs(void);

#endif /* RDPCH_SRV_PROXY_H */
