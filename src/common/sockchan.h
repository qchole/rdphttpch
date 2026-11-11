/* Event-driven socket carrier shared by the mock transports
 * (server listener + client connector).  Provides the same semantics as
 * the WTS/mstsc channel transports: HELLO handshake, frame parsing,
 * non-blocking queued sends.
 *
 * Lifetime: attach() returns one reference; stop() halts the IO thread
 * (idempotent, safe while sends are in flight); release() drops a
 * reference and frees when the count reaches zero.  Wrappers publish
 * the sockchan pointer and protect senders with addref/release. */
#ifndef RDPCH_SOCKCHAN_H
#define RDPCH_SOCKCHAN_H

#include <winsock2.h>

#include "transport.h"

typedef struct sockchan sockchan;

/* Takes ownership of the connected socket s. */
sockchan *sockchan_attach(SOCKET s, const rdpch_xport_cbs *cbs);
sockchan *sockchan_addref(sockchan *sc);
void      sockchan_stop(sockchan *sc);
void      sockchan_release(sockchan *sc);

/* Thread-safe, non-blocking; returns -1 when stopped or queue full. */
int sockchan_send(sockchan *sc, const void *frame, size_t len);

/* Frame bytes queued but not yet written to the socket. */
size_t sockchan_backlog(sockchan *sc);

#endif /* RDPCH_SOCKCHAN_H */
