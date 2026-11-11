/* WTS virtual channel transport (server side).
 *
 * One IO thread owns the WTS channel handle exclusively:
 *   - opens it with exponential backoff (survives RDP reconnects)
 *   - waits on [WTSVirtualFileHandle event | write-queue event | stop]
 *   - drains reads with timeout 0 into the frame parser
 *   - pops write nodes and writes them OUTSIDE the queue lock
 * All other threads only touch the queue via xport send().
 */
#include <windows.h>
#include <wtsapi32.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "transports.h"

#define WTS_MAX_QUEUE 1024   /* frames (~1.4 MB) */
#define RDPCH_CHANNEL_NAME "RDPCH"

typedef struct wnode {
    struct wnode *next;
    uint32_t      len;
    uint8_t       data[RDPCH_MAX_FRAME];
} wnode;

typedef struct {
    rdpch_xport      base;
    rdpch_xport_cbs  cbs;
    rdpch_link       link;
    CRITICAL_SECTION lock;
    wnode           *head;
    wnode           *tail;
    int              queued;
    size_t           queued_bytes; /* for backlog pacing */
    volatile LONG    dead;   /* stopped for good */
    volatile LONG    up;     /* channel currently open */
    HANDLE           ev_queue; /* auto-reset */
    HANDLE           ev_stop;  /* manual */
    HANDLE           thread;
} wts_xport;

static int wts_send(rdpch_xport *x, const void *frame, size_t len);

static int write_full(HANDLE h, const uint8_t *p, uint32_t len)
{
    while (len > 0) {
        ULONG written = 0;
        if (!WTSVirtualChannelWrite(h, (PCHAR)p, len, &written)) {
            RLOGE("wts: write failed err=%lu", GetLastError());
            return -1;
        }
        if (written == 0) {
            RLOGE("wts: write returned 0");
            return -1;
        }
        p += written;
        len -= written;
    }
    return 0;
}

static int send_hello_locked_owner(wts_xport *w, HANDLE h)
{
    uint8_t hello[RDPCH_MAX_FRAME];
    size_t n = rdpch_link_hello_frame(hello, sizeof(hello));
    (void)w;
    return n && write_full(h, hello, (uint32_t)n) == 0 ? 0 : -1;
}

/* Peer re-handshook (mstsc re-armed): answer with our HELLO.  Runs on the
 * IO thread inside the read drain, so only enqueue - the queue drain after
 * the read phase performs the actual write. */
static void wts_resend_hello(void *user)
{
    wts_xport *w = (wts_xport *)user;
    uint8_t hello[RDPCH_MAX_FRAME];
    size_t n = rdpch_link_hello_frame(hello, sizeof(hello));
    if (n && wts_send(&w->base, hello, n) == 0)
        RLOGI("wts: peer re-handshake, HELLO re-sent");
}

static void drop_queue(wts_xport *w)
{
    wnode *node;
    EnterCriticalSection(&w->lock);
    node = w->head;
    w->head = w->tail = NULL;
    w->queued = 0;
    w->queued_bytes = 0;
    LeaveCriticalSection(&w->lock);
    while (node) {
        wnode *next = node->next;
        free(node);
        node = next;
    }
}

static DWORD WINAPI wts_thread(void *arg)
{
    wts_xport *w = (wts_xport *)arg;
    DWORD backoff = 1000;

    while (!w->dead) {
        HANDLE h = NULL;
        HANDLE ev = NULL;
        PVOID qbuf = NULL;
        DWORD qbytes = 0;
        int was_up = 0;

        /* ---- open phase ---- */
        while (!w->dead && !h) {
            h = WTSVirtualChannelOpenEx(WTS_CURRENT_SESSION,
                                        (LPSTR)RDPCH_CHANNEL_NAME, 0);
            if (!h) {
                DWORD err = GetLastError();
                if (err != ERROR_FILE_NOT_FOUND && err != ERROR_NOT_FOUND)
                    RLOGI("wts: open failed err=%lu, retrying in %lu ms",
                          err, backoff);
                if (WaitForSingleObject(w->ev_stop, backoff) ==
                    WAIT_OBJECT_0)
                    return 0;
                backoff = backoff < 5000 ? backoff * 2 : 5000;
            }
        }
        if (w->dead || !h)
            return 0;

        rdpch_link_reset(&w->link);
        drop_queue(w);

        if (WTSVirtualChannelQuery(h, WTSVirtualFileHandle, &qbuf,
                                   &qbytes) == FALSE || !qbuf ||
            qbytes < sizeof(HANDLE)) {
            RLOGE("wts: WTSVirtualChannelQuery failed");
            WTSVirtualChannelClose(h);
            if (qbuf)
                WTSFreeMemory(qbuf);
            continue;
        }
        ev = *(HANDLE *)qbuf;
        WTSFreeMemory(qbuf);

        if (send_hello_locked_owner(w, h) != 0) {
            WTSVirtualChannelClose(h);
            continue;
        }

        InterlockedExchange(&w->up, 1);
        was_up = 1;
        backoff = 1000;
        RLOGI("wts: channel %s up", RDPCH_CHANNEL_NAME);
        if (w->cbs.on_up)
            w->cbs.on_up(w->cbs.user);

        /* ---- run phase ---- */
        for (;;) {
            HANDLE hs[3];
            DWORD wait;
            DWORD timeout;

            hs[0] = ev;
            hs[1] = w->ev_queue;
            hs[2] = w->ev_stop;

            /* gate shut: bounded wait so the loop can re-announce */
            timeout = w->link.hello_done ? INFINITE
                                         : RDPCH_HELLO_REANNOUNCE_MS;
            wait = WaitForMultipleObjects(3, hs, FALSE, timeout);
            if (wait == WAIT_OBJECT_0 + 2 || w->dead)
                break;

            if (wait == WAIT_TIMEOUT) {
                /* handshake still unanswered (lost HELLO, or the peer's
                 * echo fell into its rate-limit window) - re-announce
                 * until the gate opens; see RDPCH_HELLO_REANNOUNCE_MS */
                if (!w->link.hello_done)
                    send_hello_locked_owner(w, h);
                continue;
            }
            if (wait == WAIT_OBJECT_0 + 2 || w->dead)
                break;

            if (wait == WAIT_OBJECT_0) {
                int down = 0;
                int got = 0;
                for (;;) {
                    BYTE buf[1600];
                    ULONG n = 0;
                    if (!WTSVirtualChannelRead(h, 0, (PCHAR)buf,
                                               sizeof(buf), &n)) {
                        DWORD err = GetLastError();
                        if (err != ERROR_IO_PENDING &&
                            err != ERROR_NO_DATA) {
                            RLOGI("wts: read error %lu", err);
                            down = 1;
                        }
                        break;
                    }
                    if (n == 0)
                        break;
                    got = 1;
                    RLOGD("wts: read %lu bytes", n);
                    if (rdpch_link_feed(&w->link, buf, n) != 0) {
                        RLOGE("wts: protocol error, dropping channel");
                        down = 1;
                        break;
                    }
                }
                if (down)
                    break;
                /* the file-handle event can be sticky (it also covers
                 * write completions): without data, back off so a
                 * permanently-signaled event cannot spin the core */
                if (!got)
                    Sleep(5);
            }

            /* Drain the write queue on EVERY wake.  WaitForMultipleObjects
             * reports only the LOWEST signaled index; a sticky channel
             * event would starve the queue branch forever otherwise. */
            {
                int down = 0;
                for (;;) {
                    wnode *node;
                    EnterCriticalSection(&w->lock);
                    node = w->head;
                    if (node) {
                        w->head = node->next;
                        if (!w->head)
                            w->tail = NULL;
                        w->queued--;
                        w->queued_bytes -= node->len;
                    }
                    LeaveCriticalSection(&w->lock);
                    if (!node)
                        break;
                    RLOGD("wts: writing %u bytes", node->len);
                    if (write_full(h, node->data, node->len) != 0) {
                        free(node);
                        down = 1;
                        break;
                    }
                    RLOGD("wts: wrote %u bytes", node->len);
                    free(node);
                    if (w->dead)
                        break;
                }
                if (down)
                    break;
            }
        }

        /* ---- down phase ---- */
        InterlockedExchange(&w->up, 0);
        WTSVirtualChannelClose(h);
        drop_queue(w);
        if (was_up && w->cbs.on_down)
            w->cbs.on_down(w->cbs.user);
        RLOGI("wts: channel down");
    }
    return 0;
}

static int wts_start(rdpch_xport *x)
{
    wts_xport *w = (wts_xport *)x;
    if (w->thread)
        return 0;
    w->thread = CreateThread(NULL, 0, wts_thread, w, 0, NULL);
    return w->thread ? 0 : -1;
}

static int wts_send(rdpch_xport *x, const void *frame, size_t len)
{
    wts_xport *w = (wts_xport *)x;
    wnode *node;

    if (len == 0 || len > RDPCH_MAX_FRAME)
        return -1;

    EnterCriticalSection(&w->lock);
    if (w->dead || !w->up || w->queued >= WTS_MAX_QUEUE) {
        LeaveCriticalSection(&w->lock);
        return -1;
    }
    node = (wnode *)malloc(sizeof(*node));
    if (!node) {
        LeaveCriticalSection(&w->lock);
        return -1;
    }
    node->next = NULL;
    node->len = (uint32_t)len;
    memcpy(node->data, frame, len);
    if (w->tail)
        w->tail->next = node;
    else
        w->head = node;
    w->tail = node;
    w->queued++;
    w->queued_bytes += len;
    SetEvent(w->ev_queue);
    LeaveCriticalSection(&w->lock);
    return 0;
}

static size_t wts_backlog(rdpch_xport *x)
{
    wts_xport *w = (wts_xport *)x;
    size_t b;

    EnterCriticalSection(&w->lock);
    b = w->queued_bytes;
    LeaveCriticalSection(&w->lock);
    return b;
}

static void wts_stop(rdpch_xport *x)
{
    wts_xport *w = (wts_xport *)x;

    if (!w)
        return;
    EnterCriticalSection(&w->lock);
    InterlockedExchange(&w->dead, 1);
    LeaveCriticalSection(&w->lock);
    SetEvent(w->ev_stop);
    SetEvent(w->ev_queue);
    if (w->thread) {
        WaitForSingleObject(w->thread, INFINITE);
        CloseHandle(w->thread);
        w->thread = NULL;
    }
    drop_queue(w);
    DeleteCriticalSection(&w->lock);
    CloseHandle(w->ev_queue);
    CloseHandle(w->ev_stop);
    free(w);
}

static rdpch_xport_ops wts_ops = {
    wts_start,
    wts_send,
    wts_backlog,
    wts_stop,
};

rdpch_xport *xport_wts_create(const rdpch_xport_cbs *cbs)
{
    wts_xport *w = (wts_xport *)calloc(1, sizeof(*w));

    if (!w)
        return NULL;
    w->base.ops = &wts_ops;
    w->cbs = *cbs;
    rdpch_link_init(&w->link, cbs);
    w->link.resend_hello = wts_resend_hello;
    w->link.resend_user  = w;
    InitializeCriticalSection(&w->lock);
    w->ev_queue = CreateEvent(NULL, FALSE, FALSE, NULL);
    w->ev_stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!w->ev_queue || !w->ev_stop) {
        if (w->ev_queue)
            CloseHandle(w->ev_queue);
        if (w->ev_stop)
            CloseHandle(w->ev_stop);
        DeleteCriticalSection(&w->lock);
        free(w);
        return NULL;
    }
    return &w->base;
}
