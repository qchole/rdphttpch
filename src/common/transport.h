/* Transport abstraction shared by server and client.
 *
 * A transport moves raw frame bytes over some carrier:
 *   - server: WTS virtual channel (channel_wts.c) or mock TCP listener
 *   - client: mstsc virtual channel (transport_vc.c) or mock TCP connector
 *
 * Contract:
 *   start()  spawns internal threads; calls on_up once the carrier is ready,
 *            then on_frame for every complete frame, then on_down when the
 *            carrier dies.  A transport may go up/down repeatedly (WTS
 *            reconnect, mock re-accept) without being restarted.
 *   send()  is thread-safe and non-blocking (copies the frame bytes into an
 *           internal queue).  Returns 0 on success, -1 if the transport is
 *           dead or its queue limit is exceeded (caller should kill the
 *           connection the frame belonged to).
 *   backlog_bytes() reports how many frame bytes are queued but not yet
 *           written to the carrier (0 when unavailable).  Bulk senders
 *           MUST pace against RDPCH_SEND_BACKLOG_MAX: a full queue fails
 *           send() and the caller treats that as a dead channel, so a
 *           slow carrier has to throttle the socket reader LONG before
 *           the queue fills - that is end-to-end TCP backpressure and it
 *           is what keeps bulk uploads (SFTP etc.) alive on slow RDP
 *           uplinks.
 *   stop()   tears everything down and joins internal threads (idempotent).
 *
 * HELLO handshake: every transport sends a HELLO frame when its carrier
 * comes up and only forwards frames to on_frame after receiving a valid
 * HELLO from the peer.  A frame that arrives ahead of the handshake is
 * dropped (the peer restarted mid-stream; its old conns are gone) - it
 * does NOT kill the transport.  A HELLO received while already
 * established means the peer re-handshook; the link then calls the
 * optional resend_hello hook once so the peer's fresh gate opens.
 * Malformed frames (bad cmd/length) still kill the transport: the byte
 * stream cannot be resynchronized.
 */
#ifndef RDPCH_TRANSPORT_H
#define RDPCH_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

/* While the HELLO gate is shut the carrier re-announces HELLO at this
 * rate.  The peer's answering HELLO can be lost, or swallowed by its own
 * 3 s echo rate limit (two carrier bounces seconds apart), and an
 * established peer never re-handshakes on its own - without a
 * re-announce the gate would stay shut forever: the peer keeps answering
 * everything and we silently drop each reply as "pre-hello".  At 1 Hz it
 * is free (all inbound traffic is dropped anyway) and self-heals once
 * the peer's echo rate-limit window passes. */
#define RDPCH_HELLO_REANNOUNCE_MS 1000

typedef struct rdpch_xport rdpch_xport;

/* Bulk senders pause their socket reads while the transport backlog
 * exceeds this (see backlog_bytes); the queue-full kill below it must
 * stay unreachable in normal operation. */
#define RDPCH_SEND_BACKLOG_MAX (1024u * 1024u)

/* Receiver-side flow control (v2).  A DATA receiver credits its peer
 * with the bytes its local app actually consumed from the to_sock
 * buffer (RDPCH_CMD_ACK).  A sender stops reading its app socket while
 * per-conn unacked data exceeds RDPCH_FLOW_WINDOW, so a slow consumer
 * throttles the far end through the tunnel instead of overflowing the
 * 4 MB sink buffer (which kills the conn - the old large-transfer
 * disconnect bug).  Sizing: window + ack chunk stays well under
 * sock_buf_max (4 MB), so the overflow kill valve is a backstop only. */
#define RDPCH_FLOW_WINDOW     (2u * 1024u * 1024u)
#define RDPCH_FLOW_ACK_CHUNK  (512u * 1024u)

typedef struct rdpch_xport_ops {
    int    (*start)(rdpch_xport *x);
    int    (*send)(rdpch_xport *x, const void *frame, size_t len);
    /* may be NULL: engines then skip pacing and rely on the queue-full
     * failure alone (tests, degenerate carriers) */
    size_t (*backlog_bytes)(rdpch_xport *x);
    void   (*stop)(rdpch_xport *x);
} rdpch_xport_ops;

struct rdpch_xport {
    const rdpch_xport_ops *ops;
};

typedef struct {
    void (*on_up)(void *user);
    void (*on_frame)(void *user, const rdpch_frame *f);
    void (*on_down)(void *user);
    void *user;
} rdpch_xport_cbs;

/* ---- shared helpers used by transport implementations ---- */

/* HELLO handshake state wrapped around an rdpch_parser. */
typedef struct {
    rdpch_parser    parser;
    int             hello_done;
    int             dead;       /* set on protocol error; transport dies */
    rdpch_on_frame  inner;      /* real frame handler */
    void           *inner_user;
    void          (*on_down)(void *user);
    void           *down_user;
    /* called once when a valid HELLO arrives while hello_done is already
     * set (peer re-handshake); transports set it to re-send their HELLO.
     * May run on the carrier's read path, so it must only enqueue.
     * Rate-limited: in a bounce storm one side can receive two peer
     * HELLOs and would otherwise answer each, starting an echo
     * ping-pong between established peers. */
    void          (*resend_hello)(void *user);
    void           *resend_user;
    uint64_t        last_echo_ms; /* last resend, for the rate limit */
} rdpch_link;

void rdpch_link_init(rdpch_link *l, const rdpch_xport_cbs *cbs);
/* Feed carrier bytes; enforces HELLO-first.  Returns 0 ok, -1 protocol
 * error (caller must treat the link as dead). */
int  rdpch_link_feed(rdpch_link *l, const void *data, size_t len);
void rdpch_link_reset(rdpch_link *l);
/* Build the HELLO frame to send when a link comes up.  Returns length. */
size_t rdpch_link_hello_frame(uint8_t *out, size_t cap);

/* Chop a byte range into DATA frames and push them through a transport.
 * Stops and returns -1 on the first send failure. */
int rdpch_send_data_frames(rdpch_xport *x, uint32_t conn_id,
                           const void *buf, size_t n);

/* Emit an ACK credit frame for conn_id (receiver->sender flow
 * control).  0 ok, -1 transport refused (harmless: the conn dies with
 * the channel anyway). */
int rdpch_send_ack(rdpch_xport *x, uint32_t conn_id, uint32_t credit);

#endif /* RDPCH_TRANSPORT_H */
