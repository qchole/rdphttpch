#include "transport.h"

#include <string.h>

#include "log.h"
#include "util.h"

#define LINK_ECHO_MIN_GAP_MS 3000

static void link_frame(void *user, const rdpch_frame *f);

void rdpch_link_init(rdpch_link *l, const rdpch_xport_cbs *cbs)
{
    memset(l, 0, sizeof(*l));
    rdpch_parser_init(&l->parser, link_frame, l);
    l->inner      = cbs->on_frame;
    l->inner_user = cbs->user;
    l->on_down    = cbs->on_down;
    l->down_user  = cbs->user;
}

static void link_frame(void *user, const rdpch_frame *f)
{
    rdpch_link *l = (rdpch_link *)user;

    if (f->cmd == RDPCH_CMD_HELLO &&
        rdpch_hello_check(f->payload, f->length) == 0) {
        if (l->hello_done && l->resend_hello) {
            /* Peer re-handshake: its process or carrier restarted and
             * this HELLO came from a fresh link.  Answer so its gate
             * opens.  A peer that is not yet established never answers,
             * and the rate limit below caps echo ping-pongs between
             * two established peers that each see a double bounce. */
            uint64_t now = rdpch_now_ms();
            if (now - l->last_echo_ms >= LINK_ECHO_MIN_GAP_MS) {
                l->last_echo_ms = now;
                l->resend_hello(l->resend_user);
            } else {
                RLOGD("link: extra HELLO ignored (rate limit)");
            }
        }
        l->hello_done = 1;
        return;
    }
    if (!l->hello_done) {
        /* Frame ahead of the handshake: the peer restarted while we
         * stayed up.  Frames from its previous life describe conns
         * that no longer exist anywhere - drop them silently instead
         * of killing the link (the old behavior caused a flap loop:
         * every client frame after a server bounce murdered the
         * channel again). */
        RLOGD("link: pre-hello frame conn=%u cmd=%u dropped (peer restart)",
              f->conn_id, f->cmd);
        return;
    }
    if (l->inner)
        l->inner(l->inner_user, f);
}

int rdpch_link_feed(rdpch_link *l, const void *data, size_t len)
{
    if (l->dead)
        return -1;
    if (rdpch_parser_feed(&l->parser, data, len) != 0) {
        l->dead = 1;
        return -1;
    }
    return l->dead ? -1 : 0;
}

void rdpch_link_reset(rdpch_link *l)
{
    rdpch_parser_reset(&l->parser);
    l->hello_done = 0;
    l->dead = 0;
    l->last_echo_ms = 0; /* fresh incarnation may answer immediately */
}

size_t rdpch_link_hello_frame(uint8_t *out, size_t cap)
{
    uint8_t payload[6];
    uint32_t n = 0;

    if (rdpch_hello_encode(payload, sizeof(payload), &n) != 0)
        return 0;
    return rdpch_frame_build(out, cap, RDPCH_CONN_ID_CTRL, RDPCH_CMD_HELLO,
                             payload, n);
}

/* Chop a byte range into DATA frames and push them through a transport.
 * Bulk senders must pace their reads with backlog_bytes(): a full queue
 * fails send() and the caller treats that as a dead channel.  (The
 * original design did exactly that on a slow RDP uplink and killed
 * bulk uploads - e.g. SFTP - mid-transfer.) */
int rdpch_send_data_frames(rdpch_xport *x, uint32_t conn_id,
                           const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;

    while (n > 0) {
        uint8_t frame[RDPCH_MAX_FRAME];
        size_t chunk = n > RDPCH_MAX_PAYLOAD ? RDPCH_MAX_PAYLOAD : n;
        size_t built = rdpch_frame_build(frame, sizeof(frame), conn_id,
                                         RDPCH_CMD_DATA, p, (uint32_t)chunk);
        if (built == 0)
            return -1;
        if (x->ops->send(x, frame, built) != 0)
            return -1;
        p += chunk;
        n -= chunk;
    }
    return 0;
}
