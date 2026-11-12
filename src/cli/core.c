#include "core.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bufchain.h"
#include "log.h"
#include "util.h"

typedef struct cconn {
    struct cconn     *next;
    uint32_t          id;
    volatile LONG     refs;      /* table(1) + worker thread(1) + routers */
    CRITICAL_SECTION  lock;      /* to_sock + killed + peer_eof + flow */
    bufchain          to_sock;   /* channel -> socket */
    SOCKET            sock;
    HANDLE            ev_sock;   /* manual-reset WSA network event */
    HANDLE            ev_buf;    /* auto-reset: to_sock non-empty / peer EOF */
    HANDLE            ev_kill;   /* manual: kill connection */
    volatile LONG     killed;
    volatile LONG     peer_eof;  /* channel sent CLOSE(EOF): upstream done */
    uint64_t          inflight;  /* DATA sent, unacked by the peer (v2) */
    uint32_t          credit;    /* consumed bytes pending ACK to peer */
    char              host[256];
    uint16_t          port;
    HANDLE            thread;
} cconn;

static struct {
    int               started;
    volatile LONG     stopping;
    core_cfg          cfg;
    rdpch_xport      *xp;
    CRITICAL_SECTION  tlock;     /* connection table */
    cconn            *head;
    int               count;
    HANDLE            ev_stop;   /* manual */
    int               wsa;
} C;

void core_cfg_default(core_cfg *cfg)
{
    cfg->max_conns = 128;
    cfg->sock_buf_max = 4 * 1024 * 1024;
    cfg->connect_timeout_ms = 10000;
}

/* The last reference out closes the socket and handles: routers on
 * the channel thread may still hold a temporary ref and SetEvent()
 * after the worker is gone, so nothing may be closed before refs
 * hits 0. */
static void conn_unref(cconn *c)
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
        bc_free(&c->to_sock);
        DeleteCriticalSection(&c->lock);
        free(c);
    }
}

static cconn *table_ref(uint32_t id)
{
    cconn *c;

    EnterCriticalSection(&C.tlock);
    for (c = C.head; c; c = c->next) {
        if (c->id == id) {
            InterlockedIncrement(&c->refs);
            break;
        }
    }
    LeaveCriticalSection(&C.tlock);
    return c;
}

static int send_ctrl(uint32_t id, uint8_t cmd, uint8_t reason)
{
    uint8_t frame[RDPCH_MAX_FRAME];
    size_t n = rdpch_frame_build(frame, sizeof(frame), id, cmd, &reason, 1);

    if (n == 0)
        return -1;
    return C.xp->ops->send(C.xp, frame, n);
}

static int send_open_ok(uint32_t id)
{
    uint8_t frame[RDPCH_FRAME_HEADER_SIZE];
    size_t n = rdpch_frame_build(frame, sizeof(frame), id, RDPCH_CMD_OPEN_OK,
                                 NULL, 0);
    if (n == 0)
        return -1;
    return C.xp->ops->send(C.xp, frame, n);
}

static int send_open_fail(uint32_t id, uint8_t reason)
{
    return send_ctrl(id, RDPCH_CMD_OPEN_FAIL, reason);
}

static int send_close(uint32_t id, uint8_t reason)
{
    return send_ctrl(id, RDPCH_CMD_CLOSE, reason);
}

/* ---- connection worker ---- */

static void conn_cleanup(cconn *c)
{
    int was_linked = 0;

    EnterCriticalSection(&C.tlock);
    {
        cconn **pp = &C.head;
        while (*pp && *pp != c)
            pp = &(*pp)->next;
        if (*pp) {
            *pp = c->next;
            C.count--;
            was_linked = 1;
        }
    }
    LeaveCriticalSection(&C.tlock);

    /* the table reference may already have been dropped by core_stop;
     * socket/handles are closed by the final unref */
    if (was_linked)
        conn_unref(c);
}

static int conn_connect_phase(cconn *c)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[8];
    uint64_t deadline;
    int done = 0;
    int gai;

    if (WaitForSingleObject(c->ev_kill, 0) == WAIT_OBJECT_0 ||
        C.stopping)
        return -1;

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)c->port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    gai = getaddrinfo(c->host, portstr, &hints, &res);
    if (gai != 0 || !res) {
        RLOGI("conn %u: resolve %s failed (%d)", c->id, c->host, gai);
        send_open_fail(c->id, RDPCH_FAIL_DNS);
        return -1;
    }

    /* try every candidate (A and AAAA alike): right after a network
     * blip one family often blackholes (stale v6 route etc.), and a
     * browser would fall through to the next address - so do the same.
     * The overall deadline covers all attempts. */
    deadline = rdpch_now_ms() + C.cfg.connect_timeout_ms;
    for (ai = res; ai; ai = ai->ai_next) {
        WSANETWORKEVENTS ne;
        char addrstr[64];

        if (WaitForSingleObject(c->ev_kill, 0) == WAIT_OBJECT_0 ||
            C.stopping) {
            freeaddrinfo(res);
            return -1;
        }
        if (rdpch_now_ms() >= deadline)
            break; /* no budget left for another candidate */

        c->sock = socket(ai->ai_family, ai->ai_socktype,
                         ai->ai_protocol);
        if (c->sock == INVALID_SOCKET)
            continue;

        if (getnameinfo(ai->ai_addr, (socklen_t)ai->ai_addrlen,
                        addrstr, sizeof(addrstr), NULL, 0,
                        NI_NUMERICHOST) != 0)
            snprintf(addrstr, sizeof(addrstr), "?");

        WSAEventSelect(c->sock, c->ev_sock, FD_CONNECT); /* nonblocking */
        if (connect(c->sock, ai->ai_addr, (int)ai->ai_addrlen) == 0) {
            done = 1; /* rare: immediate success */
        } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            for (;;) {
                uint64_t remain = deadline - rdpch_now_ms();
                HANDLE hs[3];
                DWORD w;

                if (remain > (uint64_t)INFINITE)
                    remain = INFINITE - 1;
                hs[0] = c->ev_sock;
                hs[1] = c->ev_kill;
                hs[2] = C.ev_stop;
                w = WaitForMultipleObjects(3, hs, FALSE, (DWORD)remain);
                if (w == WAIT_OBJECT_0 + 1 || w == WAIT_OBJECT_0 + 2) {
                    freeaddrinfo(res);
                    return -1;
                }
                if (w == WAIT_TIMEOUT)
                    break; /* this candidate stalls: try the next */
                if (WSAEnumNetworkEvents(c->sock, c->ev_sock, &ne) != 0)
                    continue;
                if (ne.lNetworkEvents & FD_CONNECT) {
                    int err = ne.iErrorCode[FD_CONNECT_BIT];
                    if (err == 0)
                        done = 1;
                    else
                        RLOGI("conn %u: connect [%s]:%u refused (%d)",
                              c->id, addrstr, (unsigned)c->port, err);
                    break;
                }
            }
        } else {
            RLOGI("conn %u: connect [%s]:%u failed (%d)",
                  c->id, addrstr, (unsigned)c->port, WSAGetLastError());
        }

        if (done)
            break;
        closesocket(c->sock);
        c->sock = INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (!done) { /* no candidate connected in time */
        uint64_t left = deadline - rdpch_now_ms();
        RLOGI("conn %u: connect %s:%u timed out", c->id, c->host,
              (unsigned)c->port);
        send_open_fail(c->id, left <= 0 ? RDPCH_FAIL_TIMEOUT
                                        : RDPCH_FAIL_REFUSED);
        return -1;
    }

    EnterCriticalSection(&c->lock);
    {
        int killed = c->killed;
        LeaveCriticalSection(&c->lock);
        if (killed)
            return -1;
    }

    if (send_open_ok(c->id) != 0)
        return -1;

    rdpch_set_nodelay(c->sock);
    WSAEventSelect(c->sock, c->ev_sock, FD_READ | FD_WRITE | FD_CLOSE);
    RLOGD("conn %u: open %s:%u", c->id, c->host, (unsigned)c->port);
    return 0;
}

/* v2 flow control: batched consume-credit flush to the peer */
static void conn_maybe_ack(cconn *c);

/* returns -1 when the socket died.  Delivers every byte already
 * accepted from the channel, even when the conn is being killed: the
 * frames arrived before the CLOSE, dropping the tail corrupts the
 * target's stream. */
static int conn_drain_to_sock(cconn *c)
{
    for (;;) {
        const uint8_t *seg;
        size_t seglen;
        int n;

        EnterCriticalSection(&c->lock);
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
            conn_maybe_ack(c);

        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK)
                return 0; /* FD_WRITE will wake us */
            return -1;
        }
    }
}

static int conn_killed(cconn *c)
{
    int k;

    EnterCriticalSection(&c->lock);
    k = c->killed;
    LeaveCriticalSection(&c->lock);
    return k;
}

static int conn_peer_eof(cconn *c)
{
    int e;

    EnterCriticalSection(&c->lock);
    e = c->peer_eof;
    LeaveCriticalSection(&c->lock);
    return e;
}

/* ---- v2 receiver-side flow control ----
 *
 * The sender side of a conn pauses its socket reads while the peer has
 * not acknowledged more than RDPCH_FLOW_WINDOW of our DATA; the
 * receiver credits the peer as its local app consumes bytes from
 * to_sock.  A slow consumer therefore throttles the far end via its
 * own TCP instead of overflowing to_sock (which kills the conn - the
 * large-transfer disconnect bug). */

static int conn_inflight_over(cconn *c)
{
    int over;

    EnterCriticalSection(&c->lock);
    over = c->inflight > RDPCH_FLOW_WINDOW;
    LeaveCriticalSection(&c->lock);
    return over;
}

static void conn_add_inflight(cconn *c, size_t n)
{
    EnterCriticalSection(&c->lock);
    c->inflight += (uint64_t)n;
    LeaveCriticalSection(&c->lock);
}

/* peer consumed app data: reopen its window */
static void conn_apply_credit(cconn *c, uint32_t credit)
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
static void conn_maybe_ack(cconn *c)
{
    uint32_t credit = 0;

    EnterCriticalSection(&c->lock);
    if (c->credit >= RDPCH_FLOW_ACK_CHUNK) {
        credit = c->credit;
        c->credit = 0;
    }
    LeaveCriticalSection(&c->lock);
    if (credit)
        rdpch_send_ack(C.xp, c->id, credit);
}

static unsigned __stdcall conn_thread(void *arg)
{
    cconn *c = (cconn *)arg;

    if (conn_connect_phase(c) == 0) {
        int closing = 0;   /* peer sent CLOSE(EOF): relaying the tail */
        int sd_sent = 0;   /* we shutdown(SD_SEND)-ed the target */

        for (;;) {
            HANDLE hs[4];
            DWORD w;

            hs[0] = c->ev_sock;
            hs[1] = c->ev_buf;
            hs[2] = c->ev_kill;
            hs[3] = C.ev_stop;
            w = WaitForMultipleObjects(4, hs, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 + 2 || w == WAIT_OBJECT_0 + 3)
                break;

            if (w == WAIT_OBJECT_0 + 1 && !closing && conn_peer_eof(c)) {
                closing = 1;
            }

            if (w == WAIT_OBJECT_0) {
                WSANETWORKEVENTS ne;
                int dead = 0;

                WSAEnumNetworkEvents(c->sock, c->ev_sock, &ne);
                if (ne.lNetworkEvents & (FD_READ | FD_CLOSE)) {
                    int chunks = 0;
                    for (;;) {
                        char buf[16384];
                        int n = recv(c->sock, buf, sizeof(buf), 0);
                        if (n > 0) {
                            /* Pace bulk uploads against the channel
                             * queue AND the peer's receive window: a
                             * slow RDP link throttles this socket via
                             * backlog_bytes; a slow target app (disk,
                             * AV scan) throttles it via the peer's ACK
                             * credits.  Both leave TCP backpressure to
                             * do the end-to-end throttling long before
                             * anything overflows.  Keep draining
                             * to_sock while waiting so the reverse
                             * direction cannot overflow either. */
                            while (!C.stopping && !conn_killed(c) &&
                                   ((C.xp->ops->backlog_bytes &&
                                     C.xp->ops->backlog_bytes(C.xp) >
                                         RDPCH_SEND_BACKLOG_MAX) ||
                                    conn_inflight_over(c))) {
                                if (conn_drain_to_sock(c) != 0) {
                                    dead = 1;
                                    break;
                                }
                                Sleep(5);
                            }
                            if (dead)
                                break;
                            if (rdpch_send_data_frames(C.xp, c->id, buf,
                                                       (size_t)n) != 0) {
                                dead = 1; /* channel gone */
                                break;
                            }
                            conn_add_inflight(c, (size_t)n);
                            if (++chunks >= 64)
                                break; /* yield to conn_drain_to_sock */
                            continue;
                        }
                        if (n == 0) {
                            /* target finished: EOF upstream.  In the
                             * closing state this is the reply half of
                             * the peer's CLOSE(EOF); either way the
                             * conn is done. */
                            send_close(c->id, RDPCH_CLOSE_EOF);
                            dead = 1;
                        } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
                            send_close(c->id, RDPCH_CLOSE_ERROR);
                            dead = 1;
                        }
                        break;
                    }
                }
                if (dead)
                    break;
            }

            if (conn_drain_to_sock(c) != 0) {
                send_close(c->id, RDPCH_CLOSE_ERROR);
                break;
            }

            if (closing && !sd_sent && bc_len(&c->to_sock) == 0) {
                /* upstream done and drained: half-close the target so
                 * it finishes; keep relaying its output above */
                shutdown(c->sock, SD_SEND);
                sd_sent = 1;
            }
        }
    }

    RLOGD("conn %u: closed", c->id);
    conn_cleanup(c);   /* drops the table reference if still linked */
    conn_unref(c);     /* thread reference - may free c */
    return 0;
}

/* ---- frame handling (transport thread) ---- */

static void on_open(const rdpch_frame *f)
{
    uint16_t port;
    char host[256];
    cconn *c, *dup;

    if (C.stopping) {
        send_open_fail(f->conn_id, RDPCH_FAIL_LIMIT);
        return;
    }
    if (rdpch_open_decode(f->payload, f->length, &port, host,
                          sizeof(host)) != 0) {
        send_open_fail(f->conn_id, RDPCH_FAIL_PROTO);
        return;
    }
    if ((dup = table_ref(f->conn_id)) != NULL) {
        conn_unref(dup);
        send_open_fail(f->conn_id, RDPCH_FAIL_PROTO);
        return;
    }
    if (C.count >= C.cfg.max_conns) {
        RLOGW("conn %u: limit reached (%d)", f->conn_id, C.count);
        send_open_fail(f->conn_id, RDPCH_FAIL_LIMIT);
        return;
    }

    c = (cconn *)calloc(1, sizeof(*c));
    if (!c) {
        send_open_fail(f->conn_id, RDPCH_FAIL_PROTO);
        return;
    }
    c->id = f->conn_id;
    c->refs = 2; /* table + worker */
    c->sock = INVALID_SOCKET;
    c->port = port;
    strncpy_s(c->host, sizeof(c->host), host, _TRUNCATE);
    bc_init(&c->to_sock);
    InitializeCriticalSection(&c->lock);
    c->ev_sock = CreateEvent(NULL, TRUE, FALSE, NULL);
    c->ev_buf = CreateEvent(NULL, FALSE, FALSE, NULL);
    c->ev_kill = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!c->ev_sock || !c->ev_buf || !c->ev_kill)
        goto fail;

    EnterCriticalSection(&C.tlock);
    c->next = C.head;
    C.head = c;
    C.count++;
    LeaveCriticalSection(&C.tlock);

    c->thread = (HANDLE)_beginthreadex(NULL, 0, conn_thread, c, 0, NULL);
    if (!c->thread)
        goto fail_thread;
    return;

fail_thread:
    EnterCriticalSection(&C.tlock);
    {
        cconn **pp = &C.head;
        while (*pp && *pp != c)
            pp = &(*pp)->next;
        if (*pp) {
            *pp = c->next;
            C.count--;
        }
    }
    LeaveCriticalSection(&C.tlock);
fail:
    if (c->ev_sock)
        CloseHandle(c->ev_sock);
    if (c->ev_buf)
        CloseHandle(c->ev_buf);
    if (c->ev_kill)
        CloseHandle(c->ev_kill);
    DeleteCriticalSection(&c->lock);
    bc_free(&c->to_sock);
    free(c);
    send_open_fail(f->conn_id, RDPCH_FAIL_PROTO);
}

static void on_data(const rdpch_frame *f)
{
    cconn *c = table_ref(f->conn_id);
    int overflow = 0;

    if (!c)
        return; /* late frame for a dead connection */

    EnterCriticalSection(&c->lock);
    if (!c->killed) {
        if (bc_append(&c->to_sock, f->payload, f->length) != 0 ||
            bc_len(&c->to_sock) > C.cfg.sock_buf_max) {
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
    conn_unref(c);
}

/* peer consumed app data we sent: reopen its flow window (v2) */
static void on_ack(const rdpch_frame *f)
{
    cconn *c = table_ref(f->conn_id);

    if (!c)
        return; /* late frame for a dead connection */
    if (f->length >= 4) {
        uint32_t credit = (uint32_t)f->payload[0] |
                          ((uint32_t)f->payload[1] << 8) |
                          ((uint32_t)f->payload[2] << 16) |
                          ((uint32_t)f->payload[3] << 24);
        conn_apply_credit(c, credit);
    }
    conn_unref(c);
}

static void on_close(const rdpch_frame *f)
{
    cconn *c = table_ref(f->conn_id);

    if (!c)
        return;
    if (f->length >= 1 && f->payload[0] == RDPCH_CLOSE_EOF &&
        !conn_killed(c)) {
        /* Upstream half-close (ssh -L semantics): the peer app finished
         * sending.  Deliver what we hold, shutdown our target, and keep
         * relaying its output until IT closes.  Only a CLOSE without
         * EOF (error/overflow) aborts the whole connection. */
        EnterCriticalSection(&c->lock);
        c->peer_eof = 1;
        LeaveCriticalSection(&c->lock);
        SetEvent(c->ev_buf);
    } else {
        EnterCriticalSection(&c->lock);
        c->killed = 1;
        LeaveCriticalSection(&c->lock);
        SetEvent(c->ev_kill);
    }
    conn_unref(c);
}

static void core_on_frame(void *user, const rdpch_frame *f)
{
    (void)user;
    if (C.stopping)
        return;
    switch (f->cmd) {
    case RDPCH_CMD_OPEN:
        on_open(f);
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
    case RDPCH_CMD_OPEN_OK:
    case RDPCH_CMD_OPEN_FAIL:
        RLOGW("cli: unexpected cmd %u (conn %u)", f->cmd, f->conn_id);
        break;
    default:
        break;
    }
}

static void core_on_up(void *user)
{
    (void)user;
    RLOGI("cli: channel up");
}

static void core_on_down(void *user)
{
    cconn *c;

    (void)user;
    RLOGI("cli: channel down, killing all connections");
    EnterCriticalSection(&C.tlock);
    for (c = C.head; c; c = c->next) {
        EnterCriticalSection(&c->lock);
        c->killed = 1;
        LeaveCriticalSection(&c->lock);
        SetEvent(c->ev_kill);
    }
    LeaveCriticalSection(&C.tlock);
}

/* ---- lifecycle ---- */

int core_start(const core_cfg *cfg)
{
    WSADATA wd;

    if (!C.wsa) {
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0)
            return -1;
        C.wsa = 1;
    }
    if (!C.started) {
        InitializeCriticalSection(&C.tlock);
        C.ev_stop = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (!C.ev_stop)
            return -1;
        C.started = 1;
    }
    C.cfg = *cfg;
    InterlockedExchange(&C.stopping, 0);
    ResetEvent(C.ev_stop);
    return 0;
}

void core_set_xport(rdpch_xport *xp)
{
    C.xp = xp;
}

void core_stop(void)
{
    uint64_t deadline;
    int i;

    if (!C.started)
        return;
    InterlockedExchange(&C.stopping, 1);
    SetEvent(C.ev_stop);

    /* wake everyone */
    EnterCriticalSection(&C.tlock);
    {
        cconn *c;
        for (c = C.head; c; c = c->next)
            SetEvent(c->ev_kill);
    }
    LeaveCriticalSection(&C.tlock);

    deadline = rdpch_now_ms() + 8000;
    for (;;) {
        HANDLE hs[64];
        DWORD n = 0;
        uint64_t remain = deadline - rdpch_now_ms();
        cconn *c;

        if (remain > (uint64_t)INFINITE)
            remain = INFINITE - 1;

        EnterCriticalSection(&C.tlock);
        for (c = C.head; c && n < 64; c = c->next) {
            if (c->thread)
                hs[n++] = c->thread;
        }
        LeaveCriticalSection(&C.tlock);

        if (n == 0)
            break;

        if (WaitForMultipleObjects(n, hs, FALSE, (DWORD)remain) ==
            WAIT_TIMEOUT) {
            /* detach stragglers (stuck in getaddrinfo etc.) */
            EnterCriticalSection(&C.tlock);
            for (c = C.head; c; c = c->next) {
                if (c->thread) {
                    CloseHandle(c->thread);
                    c->thread = NULL;
                }
            }
            LeaveCriticalSection(&C.tlock);
            break;
        }

        /* close finished thread handles */
        EnterCriticalSection(&C.tlock);
        for (c = C.head; c; c = c->next) {
            if (c->thread &&
                WaitForSingleObject(c->thread, 0) == WAIT_OBJECT_0) {
                CloseHandle(c->thread);
                c->thread = NULL;
            }
        }
        LeaveCriticalSection(&C.tlock);
    }

    /* drop remaining table references */
    for (i = 0; i < 256; i++) {
        cconn *c;
        EnterCriticalSection(&C.tlock);
        c = C.head;
        if (c) {
            C.head = c->next;
            C.count--;
        }
        LeaveCriticalSection(&C.tlock);
        if (!c)
            break;
        conn_unref(c);
    }
}

rdpch_xport_cbs core_cbs(void)
{
    rdpch_xport_cbs cbs;

    cbs.on_up = core_on_up;
    cbs.on_frame = core_on_frame;
    cbs.on_down = core_on_down;
    cbs.user = NULL;
    return cbs;
}
