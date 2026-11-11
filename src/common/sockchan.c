#include "sockchan.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "util.h"

#define SC_MAX_QUEUE 1024 /* frames; ~1.4 MB buffered */

typedef struct sc_node {
    struct sc_node *next;
    size_t          off; /* bytes already sent */
    size_t          len; /* total bytes */
    uint8_t         data[RDPCH_MAX_FRAME];
} sc_node;

struct sockchan {
    volatile LONG       refs;
    volatile LONG       dead;
    SOCKET              sock;
    HANDLE              ev_sock;   /* WSA network event (IO thread only) */
    HANDLE              ev_queue;  /* auto-reset: frames queued */
    HANDLE              ev_stop;   /* manual */
    HANDLE              thread;    /* IO thread (stopped under refs lock) */
    CRITICAL_SECTION    lock;      /* queue + dead flag */
    sc_node            *head;
    sc_node            *tail;
    int                 queued;
    size_t              queued_bytes; /* for backlog pacing */
    rdpch_xport_cbs     cbs;       /* copy of caller's bundle */
    rdpch_link          link;
};

/* Fully send a small buffer (HELLO at connect).  The EnumNetworkEvents
 * before each wait is essential: the WSA event is manual-reset and only
 * Enum clears it.  Waiting on a stale-signaled event returns instantly
 * and the retry loop busy-spins - with both carrier ends spinning,
 * neither ever reads again (seen as a full-duplex transfer stall). */
static int send_all_blocking(sockchan *sc, const void *frame, size_t len)
{
    const uint8_t *p = (const uint8_t *)frame;

    while (len > 0) {
        int n = send(sc->sock, (const char *)p, (int)len, 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                WSANETWORKEVENTS ne;
                HANDLE hs[2] = { sc->ev_sock, sc->ev_stop };
                DWORD w;

                WSAEnumNetworkEvents(sc->sock, sc->ev_sock, &ne);
                w = WaitForMultipleObjects(2, hs, FALSE, 5000);
                if (w != WAIT_OBJECT_0)
                    return -1;
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* IO thread only: push as many queued bytes into the socket as it
 * accepts.  WOULDBLOCK just pauses the drain - the socket is selected
 * for FD_WRITE, so the main wait wakes when space frees.  Never blocks
 * and never spins, so reads always make progress even with both
 * carrier directions saturated.  0 ok, -1 socket error. */
static int drain_queue(sockchan *sc)
{
    for (;;) {
        sc_node *node;
        size_t remaining;
        int done = 0;
        int n;

        EnterCriticalSection(&sc->lock);
        node = sc->head;
        if (node) {
            remaining = node->len - node->off;
        } else {
            remaining = 0;
            done = 1;
        }
        LeaveCriticalSection(&sc->lock);
        if (done)
            return 0;

        n = send(sc->sock, (const char *)node->data + node->off,
                 (int)remaining, 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK)
                return 0; /* paused; FD_WRITE will wake the main loop */
            return -1;
        }

        EnterCriticalSection(&sc->lock);
        node->off += (size_t)n;
        sc->queued_bytes -= (size_t)n;
        if (node->off == node->len) {
            sc->head = node->next;
            if (!sc->head)
                sc->tail = NULL;
            sc->queued--;
        } else {
            node = NULL; /* partial send: stays at the queue head */
        }
        LeaveCriticalSection(&sc->lock);
        if (node)
            free(node);
    }
}

static void free_queue(sockchan *sc)
{
    sc_node *node = sc->head;
    while (node) {
        sc_node *next = node->next;
        free(node);
        node = next;
    }
    sc->head = sc->tail = NULL;
    sc->queued = 0;
    sc->queued_bytes = 0;
}

/* caller holds sc->lock */
static void mark_dead_locked(sockchan *sc)
{
    sc->dead = 1;
}

static DWORD WINAPI sc_thread(void *arg)
{
    sockchan *sc = (sockchan *)arg;
    uint8_t hello[RDPCH_MAX_FRAME];
    size_t hello_len;

    WSAEventSelect(sc->sock, sc->ev_sock, FD_READ | FD_WRITE | FD_CLOSE);
    rdpch_set_nodelay(sc->sock);

    hello_len = rdpch_link_hello_frame(hello, sizeof(hello));
    if (hello_len == 0 || send_all_blocking(sc, hello, hello_len) != 0) {
        RLOGE("sockchan: failed to send HELLO");
        EnterCriticalSection(&sc->lock);
        mark_dead_locked(sc);
        LeaveCriticalSection(&sc->lock);
    } else if (sc->cbs.on_up) {
        sc->cbs.on_up(sc->cbs.user);
    }

    while (!sc->dead) {
        HANDLE hs[3];
        DWORD w;
        WSANETWORKEVENTS ne;

        hs[0] = sc->ev_sock;
        hs[1] = sc->ev_queue;
        hs[2] = sc->ev_stop;

        w = WaitForMultipleObjects(3, hs, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0 + 2)
            break;

        if (w == WAIT_OBJECT_0) {
            if (WSAEnumNetworkEvents(sc->sock, sc->ev_sock, &ne) != 0)
                break;
            if (ne.lNetworkEvents & FD_READ) {
                for (;;) {
                    uint8_t buf[16384];
                    int n = recv(sc->sock, (char *)buf, sizeof(buf), 0);
                    if (n > 0) {
                        if (rdpch_link_feed(&sc->link, buf, (size_t)n) != 0) {
                            RLOGE("sockchan: protocol error");
                            EnterCriticalSection(&sc->lock);
                            mark_dead_locked(sc);
                            LeaveCriticalSection(&sc->lock);
                            break;
                        }
                        continue;
                    }
                    if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
                        EnterCriticalSection(&sc->lock);
                        mark_dead_locked(sc);
                        LeaveCriticalSection(&sc->lock);
                        break;
                    }
                    break;
                }
            }
            if (ne.lNetworkEvents & FD_CLOSE) {
                EnterCriticalSection(&sc->lock);
                mark_dead_locked(sc);
                LeaveCriticalSection(&sc->lock);
            }
            if (sc->dead)
                break;
        }

        /* Drain the write queue on EVERY wake (both ev_sock and
         * ev_queue).  drain_queue pauses on WOULDBLOCK; FD_WRITE keeps
         * the ev_sock branch waking until it finishes. */
        if (drain_queue(sc) != 0) {
            RLOGE("sockchan: write failed");
            EnterCriticalSection(&sc->lock);
            mark_dead_locked(sc);
            LeaveCriticalSection(&sc->lock);
            break;
        }
        if (sc->dead)
            break;
    }

    EnterCriticalSection(&sc->lock);
    mark_dead_locked(sc);
    free_queue(sc);
    LeaveCriticalSection(&sc->lock);

    closesocket(sc->sock);
    WSACloseEvent(sc->ev_sock);

    if (sc->cbs.on_down)
        sc->cbs.on_down(sc->cbs.user);

    sockchan_release(sc); /* thread's reference */
    return 0;
}

sockchan *sockchan_attach(SOCKET s, const rdpch_xport_cbs *cbs)
{
    sockchan *sc = (sockchan *)calloc(1, sizeof(*sc));

    if (!sc)
        return NULL;
    sc->refs = 2; /* creator + IO thread */
    sc->sock = s;
    sc->cbs = *cbs;
    rdpch_link_init(&sc->link, cbs);
    InitializeCriticalSection(&sc->lock);
    sc->ev_sock = WSACreateEvent();
    sc->ev_queue = CreateEvent(NULL, FALSE, FALSE, NULL);
    sc->ev_stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!sc->ev_sock || !sc->ev_queue || !sc->ev_stop ||
        (sc->thread = CreateThread(NULL, 0, sc_thread, sc, 0, NULL)) == NULL) {
        RLOGE("sockchan: init failed");
        if (sc->ev_sock)
            WSACloseEvent(sc->ev_sock);
        if (sc->ev_queue)
            CloseHandle(sc->ev_queue);
        if (sc->ev_stop)
            CloseHandle(sc->ev_stop);
        DeleteCriticalSection(&sc->lock);
        free(sc);
        return NULL;
    }
    return sc;
}

sockchan *sockchan_addref(sockchan *sc)
{
    InterlockedIncrement(&sc->refs);
    return sc;
}

void sockchan_stop(sockchan *sc)
{
    if (!sc)
        return;
    EnterCriticalSection(&sc->lock);
    mark_dead_locked(sc);
    LeaveCriticalSection(&sc->lock);
    SetEvent(sc->ev_stop);
    SetEvent(sc->ev_queue);
    if (sc->thread) {
        WaitForSingleObject(sc->thread, INFINITE);
        CloseHandle(sc->thread);
        sc->thread = NULL;
    }
}

void sockchan_release(sockchan *sc)
{
    if (InterlockedDecrement(&sc->refs) == 0) {
        CloseHandle(sc->ev_queue);
        CloseHandle(sc->ev_stop);
        DeleteCriticalSection(&sc->lock);
        free_queue(sc);
        free(sc);
    }
}

int sockchan_send(sockchan *sc, const void *frame, size_t len)
{
    sc_node *node;

    if (!sc || len == 0 || len > RDPCH_MAX_FRAME)
        return -1;

    EnterCriticalSection(&sc->lock);
    if (sc->dead || sc->queued >= SC_MAX_QUEUE) {
        LeaveCriticalSection(&sc->lock);
        return -1;
    }
    node = (sc_node *)malloc(sizeof(*node));
    if (!node) {
        LeaveCriticalSection(&sc->lock);
        return -1;
    }
    node->next = NULL;
    node->off = 0;
    node->len = (uint32_t)len;
    memcpy(node->data, frame, len);
    if (sc->tail)
        sc->tail->next = node;
    else
        sc->head = node;
    sc->tail = node;
    sc->queued++;
    sc->queued_bytes += len;
    SetEvent(sc->ev_queue);
    LeaveCriticalSection(&sc->lock);
    return 0;
}

size_t sockchan_backlog(sockchan *sc)
{
    size_t b;

    if (!sc)
        return 0;
    EnterCriticalSection(&sc->lock);
    b = sc->queued_bytes;
    LeaveCriticalSection(&sc->lock);
    return b;
}
