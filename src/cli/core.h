/* Client-side relay engine shared by rdpchcli.dll and rdpchclihost.exe.
 *
 * Consumes frames from a transport (OPEN/DATA/CLOSE), maintains a table
 * of proxied TCP connections, resolves and connects to targets, and
 * pumps bytes between sockets and DATA frames. */
#ifndef RDPCH_CLI_CORE_H
#define RDPCH_CLI_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "transport.h"

typedef struct {
    int      max_conns;            /* cap on concurrent connections */
    size_t   sock_buf_max;         /* per-conn channel->socket buffer cap */
    uint32_t connect_timeout_ms;
} core_cfg;

void core_cfg_default(core_cfg *cfg);

/* Idempotent: (re)arms the core after a stop (mstsc reconnects reuse it). */
int  core_start(const core_cfg *cfg);
void core_set_xport(rdpch_xport *xp);
void core_stop(void);

/* Callback bundle to hand to a transport. */
rdpch_xport_cbs core_cbs(void);

#endif /* RDPCH_CLI_CORE_H */
