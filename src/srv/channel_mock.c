/* Mock carrier (server side): TCP listener used for loopback testing
 * without a real RDP session.  Accepts exactly one carrier connection
 * at a time; each accepted socket becomes a fresh channel instance. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "sockchan.h"
#include "transports.h"

typedef struct {
    rdpch_xport      base;
    rdpch_xport_cbs  cbs;    /* real caller callbacks */
    uint16_t         port;
    CRITICAL_SECTION lock;   /* guards cur */
    sockchan        *cur;    /* borrowed pointer; ref held by listener */
    volatile LONG    dead;
    HANDLE           ev_gone; /* auto-reset: cur died */
    HANDLE           ev_stop; /* manual */
    HANDLE           thread;
} mock_xport;

static void mock_on_up(void *u)
{
    mock_xport *m = (mock_xport *)u;
    if (m->cbs.on_up)
        m->cbs.on_up(m->cbs.user);
}

static void mock_on_frame(void *u, const rdpch_frame *f)
{
    mock_xport *m = (mock_xport *)u;
    if (m->cbs.on_frame)
        m->cbs.on_frame(m->cbs.user, f);
}

static void mock_on_down(void *u)
{
    mock_xport *m = (mock_xport *)u;
    EnterCriticalSection(&m->lock);
    m->cur = NULL;
    LeaveCriticalSection(&m->lock);
    SetEvent(m->ev_gone);
    if (m->cbs.on_down)
        m->cbs.on_down(m->cbs.user);
}

static DWORD WINAPI mock_thread(void *arg)
{
    mock_xport *m = (mock_xport *)arg;
    struct sockaddr_in sa;
    WSAEVENT ev_acc;
    SOCKET lsock;
    int one = 1;

    lsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lsock == INVALID_SOCKET) {
        RLOGE("mock: socket() failed %d", WSAGetLastError());
        return 0;
    }
    setsockopt(lsock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one,
               sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(m->port);

    if (bind(lsock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        RLOGE("mock: bind 127.0.0.1:%u failed %d", m->port, WSAGetLastError());
        closesocket(lsock);
        return 0;
    }
    ev_acc = WSACreateEvent();
    if (!ev_acc || WSAEventSelect(lsock, ev_acc, FD_ACCEPT) != 0 ||
        listen(lsock, 4) != 0) {
        RLOGE("mock: listen failed %d", WSAGetLastError());
        if (ev_acc)
            WSACloseEvent(ev_acc);
        closesocket(lsock);
        return 0;
    }
    RLOGI("mock: carrier listening on 127.0.0.1:%u", m->port);

    for (;;) {
        HANDLE hs[2];
        DWORD w;
        SOCKET s;
        sockchan *sc;
        rdpch_xport_cbs tcb;

        hs[0] = ev_acc;
        hs[1] = m->ev_stop;
        w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0 + 1 || m->dead)
            break;

        s = accept(lsock, NULL, NULL);
        if (s == INVALID_SOCKET)
            continue;
        if (m->dead) {
            closesocket(s);
            break;
        }

        tcb.on_up = mock_on_up;
        tcb.on_frame = mock_on_frame;
        tcb.on_down = mock_on_down;
        tcb.user = m;

        sc = sockchan_attach(s, &tcb);
        if (!sc) {
            closesocket(s);
            continue;
        }
        EnterCriticalSection(&m->lock);
        m->cur = sc;
        LeaveCriticalSection(&m->lock);
        RLOGI("mock: carrier connected");

        hs[0] = m->ev_gone;
        w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        sockchan_stop(sc);
        sockchan_release(sc);
        EnterCriticalSection(&m->lock);
        if (m->cur == sc)
            m->cur = NULL;
        LeaveCriticalSection(&m->lock);

        if (w == WAIT_OBJECT_0 + 1 || m->dead)
            break;
        RLOGI("mock: carrier gone, accepting again");
    }

    WSACloseEvent(ev_acc);
    closesocket(lsock);
    return 0;
}

static int mock_start(rdpch_xport *x)
{
    mock_xport *m = (mock_xport *)x;
    if (m->thread)
        return 0;
    m->thread = CreateThread(NULL, 0, mock_thread, m, 0, NULL);
    return m->thread ? 0 : -1;
}

static int mock_send(rdpch_xport *x, const void *frame, size_t len)
{
    mock_xport *m = (mock_xport *)x;
    sockchan *sc;
    int rc;

    EnterCriticalSection(&m->lock);
    sc = m->cur;
    if (sc)
        sc = sockchan_addref(sc);
    LeaveCriticalSection(&m->lock);
    if (!sc)
        return -1;
    rc = sockchan_send(sc, frame, len);
    sockchan_release(sc);
    return rc;
}

static size_t mock_backlog(rdpch_xport *x)
{
    mock_xport *m = (mock_xport *)x;
    sockchan *sc;
    size_t b = 0;

    EnterCriticalSection(&m->lock);
    sc = m->cur;
    if (sc)
        sc = sockchan_addref(sc);
    LeaveCriticalSection(&m->lock);
    if (!sc)
        return 0;
    b = sockchan_backlog(sc);
    sockchan_release(sc);
    return b;
}

static void mock_stop(rdpch_xport *x)
{
    mock_xport *m = (mock_xport *)x;
    sockchan *sc;

    if (!m)
        return;
    InterlockedExchange(&m->dead, 1);
    SetEvent(m->ev_stop);
    SetEvent(m->ev_gone);
    if (m->thread) {
        WaitForSingleObject(m->thread, INFINITE);
        CloseHandle(m->thread);
        m->thread = NULL;
    }
    EnterCriticalSection(&m->lock);
    sc = m->cur;
    m->cur = NULL;
    LeaveCriticalSection(&m->lock);
    if (sc) {
        sockchan_stop(sc);
        sockchan_release(sc);
    }
    DeleteCriticalSection(&m->lock);
    CloseHandle(m->ev_gone);
    CloseHandle(m->ev_stop);
    free(m);
}

static rdpch_xport_ops mock_ops = {
    mock_start,
    mock_send,
    mock_backlog,
    mock_stop,
};

rdpch_xport *xport_mock_create(uint16_t listen_port, const rdpch_xport_cbs *cbs)
{
    mock_xport *m = (mock_xport *)calloc(1, sizeof(*m));

    if (!m)
        return NULL;
    m->base.ops = &mock_ops;
    m->cbs = *cbs;
    m->port = listen_port;
    InitializeCriticalSection(&m->lock);
    m->ev_gone = CreateEvent(NULL, FALSE, FALSE, NULL);
    m->ev_stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!m->ev_gone || !m->ev_stop) {
        if (m->ev_gone)
            CloseHandle(m->ev_gone);
        if (m->ev_stop)
            CloseHandle(m->ev_stop);
        DeleteCriticalSection(&m->lock);
        free(m);
        return NULL;
    }
    return &m->base;
}
