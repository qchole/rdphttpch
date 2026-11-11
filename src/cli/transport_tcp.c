/* Mock carrier (client side): connects to the server's mock listener
 * and keeps reconnecting until stopped.  Used by rdpchclihost.exe for
 * loopback testing without a real RDP session. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "sockchan.h"
#include "transports.h"

typedef struct {
    rdpch_xport      base;
    rdpch_xport_cbs  cbs;
    char             host[256];
    uint16_t         port;
    CRITICAL_SECTION lock;    /* guards cur */
    sockchan        *cur;     /* borrowed; ref held by connector thread */
    volatile LONG    dead;
    HANDLE           ev_gone; /* auto */
    HANDLE           ev_stop; /* manual */
    HANDLE           thread;
} tcp_xport;

static void tcp_on_up(void *u)
{
    tcp_xport *t = (tcp_xport *)u;
    if (t->cbs.on_up)
        t->cbs.on_up(t->cbs.user);
}

static void tcp_on_frame(void *u, const rdpch_frame *f)
{
    tcp_xport *t = (tcp_xport *)u;
    if (t->cbs.on_frame)
        t->cbs.on_frame(t->cbs.user, f);
}

static void tcp_on_down(void *u)
{
    tcp_xport *t = (tcp_xport *)u;
    EnterCriticalSection(&t->lock);
    t->cur = NULL;
    LeaveCriticalSection(&t->lock);
    SetEvent(t->ev_gone);
    if (t->cbs.on_down)
        t->cbs.on_down(t->cbs.user);
}

/* attempt a nonblocking connect; 0 ok, -1 failed/timeout/stop */
static int try_connect(tcp_xport *t, DWORD timeout_ms)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[8];
    SOCKET s = INVALID_SOCKET;
    HANDLE ev;
    DWORD wait;

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)t->port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(t->host, portstr, &hints, &res) != 0 || !res)
        return -1;

    ev = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ev) {
        freeaddrinfo(res);
        return -1;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        WSANETWORKEVENTS ne;
        int rc;

        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        if (WSAEventSelect(s, ev, FD_CONNECT) != 0) {
            closesocket(s);
            s = INVALID_SOCKET;
            continue;
        }
        rc = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        if (rc == 0)
            break; /* connected (loopback can succeed immediately) */
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            s = INVALID_SOCKET;
            continue;
        }
        {
            HANDLE hs[2] = { ev, t->ev_stop };
            wait = WaitForMultipleObjects(2, hs, FALSE, timeout_ms);
            if (wait != WAIT_OBJECT_0) {
                closesocket(s);
                s = INVALID_SOCKET;
                break;
            }
            if (WSAEnumNetworkEvents(s, ev, &ne) == 0 &&
                (ne.lNetworkEvents & FD_CONNECT) &&
                ne.iErrorCode[FD_CONNECT_BIT] == 0)
                break; /* connected */
            closesocket(s);
            s = INVALID_SOCKET;
        }
    }

    CloseHandle(ev);
    freeaddrinfo(res);

    if (s == INVALID_SOCKET)
        return -1;

    /* hand the (now nonblocking) socket to a sockchan */
    {
        sockchan *sc;
        rdpch_xport_cbs tcb;

        tcb.on_up = tcp_on_up;
        tcb.on_frame = tcp_on_frame;
        tcb.on_down = tcp_on_down;
        tcb.user = t;

        sc = sockchan_attach(s, &tcb);
        if (!sc) {
            closesocket(s);
            return -1;
        }
        EnterCriticalSection(&t->lock);
        t->cur = sc;
        LeaveCriticalSection(&t->lock);
        return 0;
    }
}

static DWORD WINAPI tcp_thread(void *arg)
{
    tcp_xport *t = (tcp_xport *)arg;

    while (!t->dead) {
        sockchan *sc;
        HANDLE hs[2];
        DWORD w;

        if (try_connect(t, 3000) != 0) {
            HANDLE ws[1] = { t->ev_stop };
            if (WaitForMultipleObjects(1, ws, FALSE, 500) == WAIT_OBJECT_0)
                break;
            continue;
        }
        RLOGI("tcp: carrier connected to %s:%u", t->host,
              (unsigned)t->port);

        hs[0] = t->ev_gone;
        hs[1] = t->ev_stop;
        w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);

        EnterCriticalSection(&t->lock);
        sc = t->cur;
        t->cur = NULL;
        LeaveCriticalSection(&t->lock);
        if (sc) {
            sockchan_stop(sc);
            sockchan_release(sc);
        }
        if (w == WAIT_OBJECT_0 + 1 || t->dead)
            break;
        RLOGI("tcp: carrier lost, reconnecting");
    }
    return 0;
}

static int tcp_start(rdpch_xport *x)
{
    tcp_xport *t = (tcp_xport *)x;
    if (t->thread)
        return 0;
    t->thread = CreateThread(NULL, 0, tcp_thread, t, 0, NULL);
    return t->thread ? 0 : -1;
}

static int tcp_send(rdpch_xport *x, const void *frame, size_t len)
{
    tcp_xport *t = (tcp_xport *)x;
    sockchan *sc;
    int rc;

    EnterCriticalSection(&t->lock);
    sc = t->cur;
    if (sc)
        sc = sockchan_addref(sc);
    LeaveCriticalSection(&t->lock);
    if (!sc)
        return -1;
    rc = sockchan_send(sc, frame, len);
    sockchan_release(sc);
    return rc;
}

static size_t tcp_backlog(rdpch_xport *x)
{
    tcp_xport *t = (tcp_xport *)x;
    sockchan *sc;
    size_t b = 0;

    EnterCriticalSection(&t->lock);
    sc = t->cur;
    if (sc)
        sc = sockchan_addref(sc);
    LeaveCriticalSection(&t->lock);
    if (!sc)
        return 0;
    b = sockchan_backlog(sc);
    sockchan_release(sc);
    return b;
}

static void tcp_stop(rdpch_xport *x)
{
    tcp_xport *t = (tcp_xport *)x;
    sockchan *sc;

    if (!t)
        return;
    InterlockedExchange(&t->dead, 1);
    SetEvent(t->ev_stop);
    SetEvent(t->ev_gone);
    if (t->thread) {
        WaitForSingleObject(t->thread, INFINITE);
        CloseHandle(t->thread);
        t->thread = NULL;
    }
    EnterCriticalSection(&t->lock);
    sc = t->cur;
    t->cur = NULL;
    LeaveCriticalSection(&t->lock);
    if (sc) {
        sockchan_stop(sc);
        sockchan_release(sc);
    }
    DeleteCriticalSection(&t->lock);
    CloseHandle(t->ev_gone);
    CloseHandle(t->ev_stop);
    free(t);
}

static rdpch_xport_ops tcp_ops = {
    tcp_start,
    tcp_send,
    tcp_backlog,
    tcp_stop,
};

rdpch_xport *xport_tcp_connect_create(const char *host, uint16_t port,
                                      const rdpch_xport_cbs *cbs)
{
    tcp_xport *t = (tcp_xport *)calloc(1, sizeof(*t));

    if (!t)
        return NULL;
    t->base.ops = &tcp_ops;
    t->cbs = *cbs;
    t->port = port;
    strncpy_s(t->host, sizeof(t->host), host, _TRUNCATE);
    InitializeCriticalSection(&t->lock);
    t->ev_gone = CreateEvent(NULL, FALSE, FALSE, NULL);
    t->ev_stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!t->ev_gone || !t->ev_stop) {
        if (t->ev_gone)
            CloseHandle(t->ev_gone);
        if (t->ev_stop)
            CloseHandle(t->ev_stop);
        DeleteCriticalSection(&t->lock);
        free(t);
        return NULL;
    }
    return &t->base;
}
