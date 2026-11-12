#include "proxy.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bufchain.h"
#include "http_rewrite.h"
#include "log.h"
#include "util.h"

#define PROXY_HEAD_MAX   65536
#define PROXY_RECV_BUF   16384
#define PROXY_LINGER_MS  5000

enum {
    MODE_HEAD   = 0,   /* still reading the first request head */
    MODE_HTTP   = 1,   /* rewriting stream (absolute/origin-form) */
    MODE_TUNNEL = 2,   /* CONNECT: raw relay */
};

typedef struct pconn {
    struct pconn     *next;
    uint32_t          id;
    volatile LONG     refs;      /* table(1) + worker thread(1) + routers */
    CRITICAL_SECTION  lock;      /* to_sock + killed + open_result */
    bufchain          to_sock;   /* channel -> browser */
    SOCKET            sock;
    HANDLE            ev_sock;   /* manual-reset WSA network event */
    HANDLE            ev_buf;    /* auto-reset: to_sock non-empty */
    HANDLE            ev_kill;   /* manual: channel closed / abort */
    HANDLE            ev_open;   /* auto-reset: OPEN_OK / OPEN_FAIL */
    volatile LONG     killed;
    int               open_result;  /* 0 pending, 1 ok, 2 fail */
    uint8_t           open_fail_reason;
    int               mode;
    int               close_sent;   /* we already sent CLOSE to channel */
    int               browser_eof;
    uint64_t          inflight;     /* DATA sent, unacked by the peer (v2) */
    uint32_t          credit;       /* consumed bytes pending ACK to peer */
    uint8_t          *hbuf;         /* request head accumulation */
    size_t            hlen;
    size_t            hcap;
    http_rewriter     rw;
    HANDLE            thread;
} pconn;

static struct {
    int               started;
    volatile LONG     stopping;
    proxy_cfg         cfg;
    rdpch_xport      *xp;
    CRITICAL_SECTION  tlock;
    pconn            *head;
    int               count;
    volatile LONG     next_id;
    HANDLE            ev_stop;    /* manual */
    SOCKET            ls;
    HANDLE            lsev;       /* WSA event: FD_ACCEPT */
    HANDLE            thread;     /* accept thread */
    int               wsa;
} P;

void proxy_cfg_default(proxy_cfg *cfg, uint16_t listen_port)
{
    cfg->listen_port = listen_port;
    cfg->max_conns = 128;
    cfg->sock_buf_max = 4 * 1024 * 1024;
    cfg->open_timeout_ms = 20000;
    cfg->head_timeout_ms = 30000;
}

/* ---- small helpers ---- */

/* The last reference out closes the socket and handles: routers on the
 * channel thread may still hold a temporary ref and SetEvent() after
 * the worker is gone, so nothing may be closed before refs hits 0. */
static void pconn_unref(pconn *c)
{
    if (InterlockedDecrement(&c->refs) == 0) {
        if (c->sock != INVALID_SOCKET)
            closesocket(c->sock);
        if (c->ev_sock)
            CloseHandle(c->ev_sock);
        if (c->ev_buf)
            CloseHandle(c->ev_buf);
        if (c->ev_kill)
            CloseHandle(c->ev_kill);
        if (c->ev_open)
            CloseHandle(c->ev_open);
        free(c->hbuf);
        bc_free(&c->to_sock);
        DeleteCriticalSection(&c->lock);
        free(c);
    }
}

static pconn *table_ref(uint32_t id)
{
    pconn *c;

    EnterCriticalSection(&P.tlock);
    for (c = P.head; c; c = c->next) {
        if (c->id == id) {
            InterlockedIncrement(&c->refs);
            break;
        }
    }
    LeaveCriticalSection(&P.tlock);
    return c;
}

static int send_ctrl(uint32_t id, uint8_t cmd, uint8_t reason)
{
    uint8_t frame[RDPCH_MAX_FRAME];
    size_t n = rdpch_frame_build(frame, sizeof(frame), id, cmd, &reason, 1);

    if (n == 0)
        return -1;
    return P.xp->ops->send(P.xp, frame, n);
}

static int send_close(uint32_t id, uint8_t reason)
{
    return send_ctrl(id, RDPCH_CMD_CLOSE, reason);
}

/* best-effort plain-text error response; tolerates a briefly
 * non-writable socket */
static void send_simple(SOCKET s, int code, const char *reason)
{
    char buf[256];
    int n, sent = 0, tries;

    n = snprintf(buf, sizeof(buf),
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Length: 0\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 code, reason);
    if (n <= 0)
        return;
    for (tries = 0; tries < 200 && sent < n; tries++) {
        int k = send(s, buf + sent, n - sent, 0);
        if (k > 0) {
            sent += k;
            continue;
        }
        if (k == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            Sleep(1);
            continue;
        }
        break;
    }
}

/* v2 flow control forward decls (helpers live below is_killed) */
static void pconn_add_inflight(pconn *c, size_t n);

/* emit sink for the http rewriter */
static int conn_emit(void *ctx, const void *data, size_t n)
{
    pconn *c = (pconn *)ctx;
    if (rdpch_send_data_frames(P.xp, c->id, data, n) != 0)
        return -1;
    pconn_add_inflight(c, n); /* v2 flow window */
    return 0;
}

/* ---- connection worker ---- */

static void pconn_cleanup(pconn *c)
{
    int was_linked = 0;

    EnterCriticalSection(&P.tlock);
    {
        pconn **pp = &P.head;
        while (*pp && *pp != c)
            pp = &(*pp)->next;
        if (*pp) {
            *pp = c->next;
            P.count--;
            was_linked = 1;
        }
    }
    LeaveCriticalSection(&P.tlock);

    /* the table reference may already have been dropped by proxy_stop */
    if (was_linked)
        pconn_unref(c);
}

/* returns 0 when a complete head is in c->hbuf with its end at *head_end */
static int read_head(pconn *c, size_t *head_end)
{
    uint64_t deadline = rdpch_now_ms() + P.cfg.head_timeout_ms;
    size_t scanned = 0;
    int line_ended = 0;

    WSAEventSelect(c->sock, c->ev_sock, FD_READ | FD_CLOSE);

    for (;;) {
        char buf[PROXY_RECV_BUF];
        uint64_t remain = deadline - rdpch_now_ms();
        HANDLE hs[3];
        DWORD w;
        int n;
        size_t i;

        if (remain > (uint64_t)INFINITE)
            remain = INFINITE - 1;
        hs[0] = c->ev_sock;
        hs[1] = c->ev_kill;
        hs[2] = P.ev_stop;
        w = WaitForMultipleObjects(3, hs, FALSE, (DWORD)remain);
        if (w == WAIT_OBJECT_0 + 1 || w == WAIT_OBJECT_0 + 2)
            return -1; /* killed / stopping: no response needed */
        if (w == WAIT_TIMEOUT) {
            send_simple(c->sock, 408, "Request Timeout");
            return -1;
        }

        {
            WSANETWORKEVENTS ne;
            if (WSAEnumNetworkEvents(c->sock, c->ev_sock, &ne) != 0)
                continue;
            if (!(ne.lNetworkEvents & (FD_READ | FD_CLOSE)))
                continue;
        }

        n = recv(c->sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n == 0 && c->hlen > 0)
                send_simple(c->sock, 400, "Bad Request");
            else if (n == 0)
                RLOGD("conn %u: browser closed before request", c->id);
            else
                RLOGD("conn %u: recv error %d", c->id, WSAGetLastError());
            return -1;
        }

        if (c->hlen + (size_t)n > PROXY_HEAD_MAX) {
            send_simple(c->sock, 400, "Bad Request");
            RLOGW("conn %u: request head too large", c->id);
            return -1;
        }
        if (c->hlen + (size_t)n > c->hcap) {
            size_t ncap = c->hcap ? c->hcap * 2 : 8192;
            uint8_t *nb;
            while (ncap < c->hlen + (size_t)n)
                ncap *= 2;
            nb = (uint8_t *)realloc(c->hbuf, ncap);
            if (!nb) {
                send_simple(c->sock, 500, "Out of Memory");
                return -1;
            }
            c->hbuf = nb;
            c->hcap = ncap;
        }
        memcpy(c->hbuf + c->hlen, buf, (size_t)n);
        c->hlen += (size_t)n;

        if (!line_ended) {
            for (i = 0; i < c->hlen; i++) {
                if (c->hbuf[i] == '\n') {
                    line_ended = 1;
                    break;
                }
            }
            if (!line_ended && c->hlen > (size_t)HTRW_MAX_LINE) {
                send_simple(c->sock, 400, "Bad Request");
                RLOGW("conn %u: request line too long", c->id);
                return -1;
            }
        }

        /* find end of head: CRLFCRLF ("\n\r\n") or LFLF ("\n\n") */
        for (i = scanned; i + 1 < c->hlen; i++) {
            if (c->hbuf[i] != '\n')
                continue;
            if (c->hbuf[i + 1] == '\n') {
                *head_end = i + 2;
                return 0;
            }
            if (c->hbuf[i + 1] == '\r') {
                if (i + 2 >= c->hlen)
                    break; /* need one more byte: rescan from here */
                if (c->hbuf[i + 2] == '\n') {
                    *head_end = i + 3;
                    return 0;
                }
            }
        }
        scanned = c->hlen >= 3 ? c->hlen - 3 : 0;
    }
}

/* send OPEN and wait for the verdict.  0 = OPEN_OK. */
static int wait_open(pconn *c, uint16_t port, const char *host)
{
    uint8_t payload[4 + RDPCH_HOST_MAX];
    uint8_t frame[RDPCH_MAX_FRAME];
    uint32_t plen = 0;
    size_t flen;
    uint64_t deadline;

    if (rdpch_open_encode(payload, sizeof(payload), &plen, port, host,
                          strlen(host)) != 0)
        return -1;
    flen = rdpch_frame_build(frame, sizeof(frame), c->id, RDPCH_CMD_OPEN,
                             payload, plen);
    if (flen == 0)
        return -1;
    if (P.xp->ops->send(P.xp, frame, flen) != 0) {
        send_simple(c->sock, 502, "Bad Gateway");
        return -1;
    }

    deadline = rdpch_now_ms() + P.cfg.open_timeout_ms;
    for (;;) {
        uint64_t remain = deadline - rdpch_now_ms();
        HANDLE hs[3];
        DWORD w;
        int res;

        if (remain > (uint64_t)INFINITE)
            remain = INFINITE - 1;
        hs[0] = c->ev_open;
        hs[1] = c->ev_kill;
        hs[2] = P.ev_stop;
        w = WaitForMultipleObjects(3, hs, FALSE, (DWORD)remain);
        if (w == WAIT_OBJECT_0 + 1 || w == WAIT_OBJECT_0 + 2)
            return -1; /* channel gone / stopping */
        if (w == WAIT_TIMEOUT) {
            send_simple(c->sock, 504, "Gateway Timeout");
            return -1;
        }

        EnterCriticalSection(&c->lock);
        res = c->open_result;
        LeaveCriticalSection(&c->lock);
        if (res == 1)
            return 0;
        if (res == 2) {
            RLOGI("conn %u: open failed (reason %u)", c->id,
                  (unsigned)c->open_fail_reason);
            send_simple(c->sock,
                        c->open_fail_reason == RDPCH_FAIL_TIMEOUT
                            ? 504 : 502,
                        c->open_fail_reason == RDPCH_FAIL_TIMEOUT
                            ? "Gateway Timeout" : "Bad Gateway");
            return -1;
        }
        /* res == 0: spurious wake (e.g. channel-down kick); the manual
         * ev_kill stays set, so the next wait returns immediately */
    }
}

static int is_killed(pconn *c)
{
    int k;
    EnterCriticalSection(&c->lock);
    k = c->killed;
    LeaveCriticalSection(&c->lock);
    return k;
}

/* ---- v2 receiver-side flow control ----
 *
 * The sender side of a conn pauses its socket reads while the peer has
 * not acknowledged more than RDPCH_FLOW_WINDOW of our DATA; the
 * receiver credits the peer as its local app consumes bytes from
 * to_sock.  A slow consumer therefore throttles the far end via its
 * own TCP instead of overflowing to_sock (which kills the conn - the
 * large-transfer disconnect bug).  inflight is accounted at every site
 * that emits DATA frames (tunnel recv, rewriter emit, early tunnel
 * bytes), so MODE_HTTP rewriting cannot skew the window. */

static int pconn_inflight_over(pconn *c)
{
    int over;

    EnterCriticalSection(&c->lock);
    over = c->inflight > RDPCH_FLOW_WINDOW;
    LeaveCriticalSection(&c->lock);
    return over;
}

static void pconn_add_inflight(pconn *c, size_t n)
{
    EnterCriticalSection(&c->lock);
    c->inflight += (uint64_t)n;
    LeaveCriticalSection(&c->lock);
}

/* peer consumed app data: reopen its window */
static void pconn_apply_credit(pconn *c, uint32_t credit)
{
    EnterCriticalSection(&c->lock);
    c->inflight = c->inflight > (uint64_t)credit
                      ? c->inflight - (uint64_t)credit
                      : 0;
    LeaveCriticalSection(&c->lock);
    SetEvent(c->ev_buf); /* rekick a sender paused on the window */
}

/* credit the peer for bytes our app consumed (batched: one small ACK
 * frame per RDPCH_FLOW_ACK_CHUNK consumed) */
static void pconn_maybe_ack(pconn *c)
{
    uint32_t credit = 0;

    EnterCriticalSection(&c->lock);
    if (c->credit >= RDPCH_FLOW_ACK_CHUNK) {
        credit = c->credit;
        c->credit = 0;
    }
    LeaveCriticalSection(&c->lock);
    if (credit)
        rdpch_send_ack(P.xp, c->id, credit);
}

/* classify + OPEN + reply/init the rewriter.  0 = relaying. */
static int setup_relay(pconn *c, size_t head_end)
{
    char linebuf[HTRW_MAX_LINE + 1];
    char method[16], authority[HTRW_AUTH_MAX], path[HTRW_MAX_LINE];
    char hostbuf[RDPCH_HOST_MAX + 8];
    char tgt[RDPCH_HOST_MAX + 8];
    const char *target;
    uint16_t port;
    int kind, ver10;
    size_t linelen = 0;

    while (linelen < c->hlen && c->hbuf[linelen] != '\n')
        linelen++;
    if (linelen >= sizeof(linebuf))
        linelen = sizeof(linebuf) - 1;
    memcpy(linebuf, c->hbuf, linelen);
    linebuf[linelen] = 0;
    if (linelen && linebuf[linelen - 1] == '\r')
        linebuf[--linelen] = 0;

    kind = htrw_parse_request_line(linebuf, linelen, method, sizeof(method),
                                   authority, sizeof(authority),
                                   path, sizeof(path), &ver10);

    switch (kind) {
    case HTRW_LINE_CONNECT:
        if (rdpch_parse_authority(authority, strlen(authority), tgt,
                                  sizeof(tgt), &port, 443) != 0) {
            send_simple(c->sock, 400, "Bad Request");
            return -1;
        }
        RLOGI("conn %u: CONNECT %s:%u", c->id, tgt, (unsigned)port);
        if (wait_open(c, port, tgt) != 0)
            return -1;
        if (is_killed(c))
            return -1;
        {
            static const char ok[] =
                "HTTP/1.1 200 Connection established\r\n\r\n";
            int off = 0;
            while (off < (int)sizeof(ok) - 1) {
                int k = send(c->sock, ok + off, (int)sizeof(ok) - 1 - off, 0);
                if (k > 0) {
                    off += k;
                } else if (k == SOCKET_ERROR &&
                           WSAGetLastError() == WSAEWOULDBLOCK) {
                    Sleep(1);
                } else {
                    send_close(c->id, RDPCH_CLOSE_ERROR);
                    c->close_sent = 1;
                    return -1;
                }
            }
        }
        c->mode = MODE_TUNNEL;
        break;

    case HTRW_LINE_HTTP_ABS:
    case HTRW_LINE_ORIGIN:
        if (kind == HTRW_LINE_ORIGIN) {
            /* target comes from the Host header (may carry :port) */
            if (htrw_find_host(c->hbuf, head_end, hostbuf,
                               sizeof(hostbuf) - 4) != 0) {
                send_simple(c->sock, 400, "Bad Request");
                return -1;
            }
            target = hostbuf;
        } else {
            /* authority is host[:port], userinfo already stripped */
            target = authority;
        }
        if (rdpch_parse_authority(target, strlen(target), tgt, sizeof(tgt),
                                  &port, 80) != 0) {
            send_simple(c->sock, 400, "Bad Request");
            return -1;
        }
        RLOGD("conn %u: %s http://%s:%u", c->id, method, tgt,
              (unsigned)port);
        if (wait_open(c, port, tgt) != 0)
            return -1;
        if (is_killed(c))
            return -1;

        htrw_init(&c->rw, conn_emit, c);
        if (htrw_feed(&c->rw, c->hbuf, c->hlen) != 0) {
            send_simple(c->sock, 400, "Bad Request");
            send_close(c->id, RDPCH_CLOSE_ERROR);
            c->close_sent = 1;
            return -1;
        }
        c->mode = MODE_HTTP;
        break;

    case HTRW_LINE_HTTPS_ABS:
        send_simple(c->sock, 502, "Bad Gateway");
        RLOGW("conn %u: https:// absolute URI refused", c->id);
        return -1;

    default:
        send_simple(c->sock, 400, "Bad Request");
        return -1;
    }

    /* leftover bytes after the head (early TLS ClientHello etc.) are
     * covered by htrw_feed above for MODE_HTTP; tunnel mode forwards
     * them explicitly */
    if (c->mode == MODE_TUNNEL && c->hlen > head_end) {
        if (rdpch_send_data_frames(P.xp, c->id, c->hbuf + head_end,
                                   c->hlen - head_end) != 0) {
            send_close(c->id, RDPCH_CLOSE_ERROR);
            c->close_sent = 1;
            return -1;
        }
        pconn_add_inflight(c, c->hlen - head_end); /* v2 flow window */
    }

    free(c->hbuf);
    c->hbuf = NULL;
    c->hlen = c->hcap = 0;
    return 0;
}

/* returns -1 browser socket error, 0 would-block/empty,
 * 1 channel closed and fully flushed */
static int drain_to_browser(pconn *c)
{
    for (;;) {
        const uint8_t *seg;
        size_t seglen;
        int n;

        EnterCriticalSection(&c->lock);
        if (c->killed && bc_len(&c->to_sock) == 0) {
            LeaveCriticalSection(&c->lock);
            return 1; /* channel closed and flushed: stop */
        }
        seglen = bc_peek(&c->to_sock, &seg);
        if (seglen == 0) {
            LeaveCriticalSection(&c->lock);
            return 0;
        }
        n = send(c->sock, (const char *)seg, (int)seglen, 0);
        if (n > 0) {
            bc_consume(&c->to_sock, (size_t)n);
            c->credit += (uint32_t)n; /* flow-control credit to peer */
        }
        LeaveCriticalSection(&c->lock);

        if (n > 0)
            pconn_maybe_ack(c);

        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK)
                return 0; /* FD_WRITE will wake us */
            return -1;
        }
    }
}

/* flush pending channel->browser bytes, half-close, wait briefly */
static void drain_then_shutdown(pconn *c)
{
    uint64_t deadline = rdpch_now_ms() + PROXY_LINGER_MS;

    for (;;) {
        uint64_t remain = deadline - rdpch_now_ms();
        WSANETWORKEVENTS ne;
        const uint8_t *seg;
        size_t seglen;
        int n;

        EnterCriticalSection(&c->lock);
        seglen = bc_peek(&c->to_sock, &seg);
        LeaveCriticalSection(&c->lock);
        if (seglen == 0)
            break;

        n = send(c->sock, (const char *)seg, (int)seglen, 0);
        if (n > 0) {
            EnterCriticalSection(&c->lock);
            bc_consume(&c->to_sock, (size_t)n);
            c->credit += (uint32_t)n;
            LeaveCriticalSection(&c->lock);
            pconn_maybe_ack(c);
            continue;
        }
        if (n != SOCKET_ERROR || WSAGetLastError() != WSAEWOULDBLOCK)
            break; /* socket error: give up */
        if (remain > (uint64_t)INFINITE)
            remain = INFINITE - 1;
        if (remain == 0 ||
            WaitForSingleObject(c->ev_sock, (DWORD)remain) == WAIT_TIMEOUT)
            break;
        if (WSAEnumNetworkEvents(c->sock, c->ev_sock, &ne) != 0)
            break;
    }

    shutdown(c->sock, SD_SEND);

    /* wait for the browser to finish reading (recv EOF) */
    for (;;) {
        uint64_t remain = deadline - rdpch_now_ms();
        WSANETWORKEVENTS ne;
        char t[256];
        int n;

        if (remain > (uint64_t)INFINITE)
            remain = INFINITE - 1;
        if (remain == 0 ||
            WaitForSingleObject(c->ev_sock, (DWORD)remain) == WAIT_TIMEOUT)
            break;
        if (WSAEnumNetworkEvents(c->sock, c->ev_sock, &ne) != 0)
            break;
        if (ne.lNetworkEvents & (FD_READ | FD_CLOSE)) {
            n = recv(c->sock, t, sizeof(t), 0);
            if (n <= 0)
                break; /* EOF or error: done */
        }
    }
}

/* browser sent EOF first: we already sent CLOSE(EOF); keep draining
 * responses until the channel acknowledges (or budget runs out) */
static void drain_until_channel_close(pconn *c)
{
    uint64_t deadline = rdpch_now_ms() + PROXY_LINGER_MS;

    for (;;) {
        uint64_t remain = deadline - rdpch_now_ms();
        HANDLE hs[3];
        DWORD w;

        if (remain > (uint64_t)INFINITE)
            remain = INFINITE - 1;
        hs[0] = c->ev_buf;
        hs[1] = c->ev_kill;
        hs[2] = P.ev_stop;
        w = WaitForMultipleObjects(3, hs, FALSE, (DWORD)remain);
        if (w != WAIT_OBJECT_0)
            break; /* timeout, channel close or stop */

        if (drain_to_browser(c) != 0)
            break; /* socket error */
        if (is_killed(c) && bc_len(&c->to_sock) == 0)
            break;
    }

    if (is_killed(c)) {
        /* channel confirmed: flush the rest politely */
        drain_then_shutdown(c);
    }
}

static unsigned __stdcall conn_thread(void *arg)
{
    pconn *c = (pconn *)arg;
    size_t head_end = 0;

    rdpch_set_nodelay(c->sock);

    if (read_head(c, &head_end) == 0 && setup_relay(c, head_end) == 0) {
        WSAEventSelect(c->sock, c->ev_sock,
                       FD_READ | FD_WRITE | FD_CLOSE);

        for (;;) {
            HANDLE hs[4];
            DWORD w;

            hs[0] = c->ev_sock;
            hs[1] = c->ev_buf;
            hs[2] = c->ev_kill;
            hs[3] = P.ev_stop;
            w = WaitForMultipleObjects(4, hs, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 + 2) {
                /* channel closed us: flush, half-close, linger */
                if (drain_to_browser(c) >= 0)
                    drain_then_shutdown(c);
                break;
            }
            if (w == WAIT_OBJECT_0 + 3)
                break; /* shutting down: close now */

            if (w == WAIT_OBJECT_0) {
                WSANETWORKEVENTS ne;
                int dead = 0;

                WSAEnumNetworkEvents(c->sock, c->ev_sock, &ne);
                if (ne.lNetworkEvents & (FD_READ | FD_CLOSE)) {
                    int chunks = 0;
                    for (;;) {
                        char buf[PROXY_RECV_BUF];
                        int n = recv(c->sock, buf, sizeof(buf), 0);
                        if (n > 0) {
                            int rc;
                            /* Pace bulk request bodies against the
                             * channel queue AND the peer's receive
                             * window: a slow RDP link throttles the
                             * browser via backlog_bytes; a slow
                             * client-side app (disk, AV scan) throttles
                             * it via the peer's ACK credits.  Both
                             * leave TCP backpressure to do the
                             * end-to-end throttling long before
                             * anything overflows.  Keep draining
                             * to_browser while waiting so the reverse
                             * direction cannot overflow either. */
                            while (!P.stopping && !is_killed(c) &&
                                   ((P.xp->ops->backlog_bytes &&
                                     P.xp->ops->backlog_bytes(P.xp) >
                                         RDPCH_SEND_BACKLOG_MAX) ||
                                    pconn_inflight_over(c))) {
                                if (drain_to_browser(c) != 0) {
                                    dead = 1;
                                    break;
                                }
                                Sleep(5);
                            }
                            if (dead)
                                break;
                            if (c->mode == MODE_HTTP)
                                rc = htrw_feed(&c->rw, buf, (size_t)n);
                            else {
                                rc = rdpch_send_data_frames(P.xp, c->id,
                                                             buf,
                                                             (size_t)n);
                                if (rc == 0)
                                    pconn_add_inflight(c, (size_t)n);
                            }
                            if (rc != 0) {
                                send_close(c->id, RDPCH_CLOSE_ERROR);
                                c->close_sent = 1;
                                dead = 1;
                                break;
                            }
                            if (++chunks >= 64)
                                break; /* yield to drain_to_browser */
                            continue;
                        }
                        if (n == 0) {
                            /* browser half-close: tell the channel,
                             * keep draining responses to it */
                            send_close(c->id, RDPCH_CLOSE_EOF);
                            c->close_sent = 1;
                            c->browser_eof = 1;
                            dead = 2;
                        } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
                            send_close(c->id, RDPCH_CLOSE_ERROR);
                            c->close_sent = 1;
                            dead = 1;
                        }
                        break;
                    }
                }
                if (dead == 2)
                    break;
                if (dead)
                    goto out;
            }

            {
                int rc = drain_to_browser(c);
                if (rc < 0) {
                    if (!c->close_sent) {
                        send_close(c->id, RDPCH_CLOSE_ERROR);
                        c->close_sent = 1;
                    }
                    goto out;
                }
                if (rc == 1) {
                    drain_then_shutdown(c);
                    goto out;
                }
            }
        }

        if (c->browser_eof)
            drain_until_channel_close(c);
    }

out:
    RLOGD("conn %u: closed", c->id);
    pconn_cleanup(c);   /* drops the table reference if still linked */
    pconn_unref(c);     /* thread reference - may free c */
    return 0;
}

/* ---- accept ---- */

static void handle_new_conn(SOCKET s)
{
    pconn *c;

    EnterCriticalSection(&P.tlock);
    if (P.count >= P.cfg.max_conns) {
        LeaveCriticalSection(&P.tlock);
        RLOGW("proxy: connection limit reached (%d), refusing", P.count);
        closesocket(s);
        return;
    }
    LeaveCriticalSection(&P.tlock);

    c = (pconn *)calloc(1, sizeof(*c));
    if (!c) {
        closesocket(s);
        return;
    }
    c->id = (uint32_t)InterlockedIncrement(&P.next_id);
    if (c->id == 0) /* wrap: 0 is reserved */
        c->id = (uint32_t)InterlockedIncrement(&P.next_id);
    c->refs = 2; /* table + worker */
    c->sock = s;
    bc_init(&c->to_sock);
    InitializeCriticalSection(&c->lock);
    c->ev_sock = CreateEvent(NULL, TRUE, FALSE, NULL);
    c->ev_buf = CreateEvent(NULL, FALSE, FALSE, NULL);
    c->ev_kill = CreateEvent(NULL, TRUE, FALSE, NULL);
    c->ev_open = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!c->ev_sock || !c->ev_buf || !c->ev_kill || !c->ev_open)
        goto fail;

    EnterCriticalSection(&P.tlock);
    c->next = P.head;
    P.head = c;
    P.count++;
    LeaveCriticalSection(&P.tlock);

    c->thread = (HANDLE)_beginthreadex(NULL, 0, conn_thread, c, 0, NULL);
    if (!c->thread)
        goto fail_thread;
    return;

fail_thread:
    EnterCriticalSection(&P.tlock);
    {
        pconn **pp = &P.head;
        while (*pp && *pp != c)
            pp = &(*pp)->next;
        if (*pp) {
            *pp = c->next;
            P.count--;
        }
    }
    LeaveCriticalSection(&P.tlock);
fail:
    if (c->ev_sock)
        CloseHandle(c->ev_sock);
    if (c->ev_buf)
        CloseHandle(c->ev_buf);
    if (c->ev_kill)
        CloseHandle(c->ev_kill);
    if (c->ev_open)
        CloseHandle(c->ev_open);
    DeleteCriticalSection(&c->lock);
    bc_free(&c->to_sock);
    free(c);
    closesocket(s);
}

static DWORD WINAPI accept_thread(void *arg)
{
    (void)arg;

    for (;;) {
        HANDLE hs[2];
        DWORD w;
        WSANETWORKEVENTS ne;

        hs[0] = P.lsev;
        hs[1] = P.ev_stop;
        w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0 + 1)
            return 0;

        if (WSAEnumNetworkEvents(P.ls, P.lsev, &ne) != 0)
            continue;
        if (!(ne.lNetworkEvents & FD_ACCEPT))
            continue;

        for (;;) {
            struct sockaddr_in sa;
            int salen = sizeof(sa);
            SOCKET s = accept(P.ls, (struct sockaddr *)&sa, &salen);
            if (s == INVALID_SOCKET)
                break;
            if (P.stopping) {
                closesocket(s);
                break;
            }
            handle_new_conn(s);
        }
    }
}

/* ---- channel frame routing (transport thread) ---- */

static void on_open_result(const rdpch_frame *f, int ok)
{
    pconn *c = table_ref(f->conn_id);

    if (!c)
        return;
    EnterCriticalSection(&c->lock);
    if (c->open_result == 0) {
        c->open_result = ok ? 1 : 2;
        if (!ok && f->length >= 1)
            c->open_fail_reason = f->payload[0];
    }
    LeaveCriticalSection(&c->lock);
    SetEvent(c->ev_open);
    pconn_unref(c);
}

static void on_data(const rdpch_frame *f)
{
    pconn *c = table_ref(f->conn_id);
    int overflow = 0;

    if (!c)
        return; /* late frame for a dead connection */

    EnterCriticalSection(&c->lock);
    if (!c->killed) {
        if (bc_append(&c->to_sock, f->payload, f->length) != 0 ||
            bc_len(&c->to_sock) > P.cfg.sock_buf_max) {
            c->killed = 1;
            overflow = 1;
        } else {
            SetEvent(c->ev_buf);
        }
    }
    LeaveCriticalSection(&c->lock);

    if (overflow) {
        RLOGW("conn %u: buffer overflow, closing", f->conn_id);
        send_close(f->conn_id, RDPCH_CLOSE_OVERFLOW);
        SetEvent(c->ev_kill);
    }
    pconn_unref(c);
}

/* peer consumed app data we sent: reopen its flow window (v2) */
static void on_ack(const rdpch_frame *f)
{
    pconn *c = table_ref(f->conn_id);

    if (!c)
        return; /* late frame for a dead connection */
    if (f->length >= 4) {
        uint32_t credit = (uint32_t)f->payload[0] |
                          ((uint32_t)f->payload[1] << 8) |
                          ((uint32_t)f->payload[2] << 16) |
                          ((uint32_t)f->payload[3] << 24);
        pconn_apply_credit(c, credit);
    }
    pconn_unref(c);
}

static void on_close(const rdpch_frame *f)
{
    pconn *c = table_ref(f->conn_id);

    if (!c)
        return;
    EnterCriticalSection(&c->lock);
    c->killed = 1;
    LeaveCriticalSection(&c->lock);
    SetEvent(c->ev_kill);
    pconn_unref(c);
}

static void proxy_on_frame(void *user, const rdpch_frame *f)
{
    (void)user;
    if (P.stopping)
        return;
    switch (f->cmd) {
    case RDPCH_CMD_OPEN_OK:
        on_open_result(f, 1);
        break;
    case RDPCH_CMD_OPEN_FAIL:
        on_open_result(f, 0);
        break;
    case RDPCH_CMD_DATA:
        on_data(f);
        break;
    case RDPCH_CMD_CLOSE:
        on_close(f);
        break;
    case RDPCH_CMD_ACK:
        on_ack(f);
        break;
    default:
        break;
    }
}

static void proxy_on_up(void *user)
{
    (void)user;
    RLOGI("proxy: channel up");
}

static void proxy_on_down(void *user)
{
    pconn *c;

    (void)user;
    RLOGI("proxy: channel down, killing all connections");
    EnterCriticalSection(&P.tlock);
    for (c = P.head; c; c = c->next) {
        EnterCriticalSection(&c->lock);
        c->killed = 1;
        LeaveCriticalSection(&c->lock);
        SetEvent(c->ev_kill);
        SetEvent(c->ev_open);
    }
    LeaveCriticalSection(&P.tlock);
}

/* ---- lifecycle ---- */

int proxy_start(const proxy_cfg *cfg)
{
    WSADATA wd;
    struct sockaddr_in sa;
    int one = 1;

    if (!P.wsa) {
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0)
            return -1;
        P.wsa = 1;
    }
    if (!P.started) {
        InitializeCriticalSection(&P.tlock);
        P.ev_stop = CreateEvent(NULL, TRUE, FALSE, NULL);
        P.lsev = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (!P.ev_stop || !P.lsev)
            return -1;
        P.started = 1;
    }
    P.cfg = *cfg;
    InterlockedExchange(&P.stopping, 0);
    ResetEvent(P.ev_stop);

    P.ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (P.ls == INVALID_SOCKET) {
        RLOGE("proxy: socket() failed %d", WSAGetLastError());
        return -1;
    }
    setsockopt(P.ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one,
               sizeof(one));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(cfg->listen_port);
    if (bind(P.ls, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        RLOGE("proxy: bind 127.0.0.1:%u failed (%d)",
              (unsigned)cfg->listen_port, WSAGetLastError());
        closesocket(P.ls);
        P.ls = INVALID_SOCKET;
        return -1;
    }
    if (listen(P.ls, SOMAXCONN) != 0) {
        RLOGE("proxy: listen failed (%d)", WSAGetLastError());
        closesocket(P.ls);
        P.ls = INVALID_SOCKET;
        return -1;
    }
    if (WSAEventSelect(P.ls, P.lsev, FD_ACCEPT | FD_CLOSE) != 0) {
        RLOGE("proxy: WSAEventSelect failed (%d)", WSAGetLastError());
        closesocket(P.ls);
        P.ls = INVALID_SOCKET;
        return -1;
    }

    P.thread = CreateThread(NULL, 0, accept_thread, NULL, 0, NULL);
    if (!P.thread) {
        closesocket(P.ls);
        P.ls = INVALID_SOCKET;
        return -1;
    }

    RLOGI("proxy: listening on 127.0.0.1:%u", (unsigned)cfg->listen_port);
    return 0;
}

void proxy_set_xport(rdpch_xport *xp)
{
    P.xp = xp;
}

void proxy_stop(void)
{
    uint64_t deadline;

    if (!P.started)
        return;
    InterlockedExchange(&P.stopping, 1);
    SetEvent(P.ev_stop);

    if (P.thread) {
        WaitForSingleObject(P.thread, INFINITE);
        CloseHandle(P.thread);
        P.thread = NULL;
    }
    if (P.ls != INVALID_SOCKET) {
        closesocket(P.ls);
        P.ls = INVALID_SOCKET;
    }

    /* wake everyone */
    EnterCriticalSection(&P.tlock);
    {
        pconn *c;
        for (c = P.head; c; c = c->next)
            SetEvent(c->ev_kill);
    }
    LeaveCriticalSection(&P.tlock);

    deadline = rdpch_now_ms() + 8000;
    for (;;) {
        HANDLE hs[64];
        DWORD n = 0;
        uint64_t remain = deadline - rdpch_now_ms();
        pconn *c;

        if (remain > (uint64_t)INFINITE)
            remain = INFINITE - 1;

        EnterCriticalSection(&P.tlock);
        for (c = P.head; c && n < 64; c = c->next) {
            if (c->thread)
                hs[n++] = c->thread;
        }
        LeaveCriticalSection(&P.tlock);

        if (n == 0)
            break;

        if (WaitForMultipleObjects(n, hs, FALSE, (DWORD)remain) ==
            WAIT_TIMEOUT) {
            /* detach stragglers */
            EnterCriticalSection(&P.tlock);
            for (c = P.head; c; c = c->next) {
                if (c->thread) {
                    CloseHandle(c->thread);
                    c->thread = NULL;
                }
            }
            LeaveCriticalSection(&P.tlock);
            break;
        }

        EnterCriticalSection(&P.tlock);
        for (c = P.head; c; c = c->next) {
            if (c->thread &&
                WaitForSingleObject(c->thread, 0) == WAIT_OBJECT_0) {
                CloseHandle(c->thread);
                c->thread = NULL;
            }
        }
        LeaveCriticalSection(&P.tlock);
    }

    /* drop remaining table references */
    for (;;) {
        pconn *c;
        EnterCriticalSection(&P.tlock);
        c = P.head;
        if (c) {
            P.head = c->next;
            P.count--;
        }
        LeaveCriticalSection(&P.tlock);
        if (!c)
            break;
        pconn_unref(c);
    }
}

rdpch_xport_cbs proxy_cbs(void)
{
    rdpch_xport_cbs cbs;

    cbs.on_up = proxy_on_up;
    cbs.on_frame = proxy_on_frame;
    cbs.on_down = proxy_on_down;
    cbs.user = NULL;
    return cbs;
}
