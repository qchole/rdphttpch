#ifndef RDPCH_SRV_TRANSPORTS_H
#define RDPCH_SRV_TRANSPORTS_H

#include "transport.h"

/* Real carrier: RDP static virtual channel "RDPCH" in the current
 * session.  IO thread re-opens the channel with backoff so the proxy
 * survives RDP disconnect/reconnect. */
rdpch_xport *xport_wts_create(const rdpch_xport_cbs *cbs);

/* Mock carrier: TCP listener - accepts exactly one connection at a
 * time and treats it as the channel (loopback testing without RDP). */
rdpch_xport *xport_mock_create(uint16_t listen_port, const rdpch_xport_cbs *cbs);

#endif /* RDPCH_SRV_TRANSPORTS_H */
