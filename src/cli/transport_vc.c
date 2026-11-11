/* mstsc virtual channel transport (client side).
 *
 * Single-flight write queue: the pump thread is the only caller of
 * VirtualChannelWrite.  It pops a node OUTSIDE the lock, writes it, and
 * the WRITE_COMPLETE callback (delivered on any mstsc thread) frees the
 * node, clears the in-flight flag and kicks the pump again.  This is
 * deadlock-free no matter which thread completions arrive on.
 *
 * The transport object is never freed: mstsc can keep the DLL loaded
 * and deliver late callbacks, and the DLL pins itself in the process.
 */
#include <windows.h>
#include <cchannel.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "transports.h"

#define VC_MAX_QUEUE 2048 /* frames (~2.8 MB) */

typedef struct vcnode {
    struct vcnode *next;
    uint32_t       gen;      /* channel generation at enqueue time */
    uint32_t       len;
    uint8_t        data[RDPCH_MAX_FRAME];
} vcnode;

typedef struct {
    rdpch_xport      base;
    rdpch_xport_cbs  cbs;
    rdpch_vc_write_fn write_fn;
    volatile LONG    armed;
    volatile LONG    dead;
    DWORD            open_handle;   /* guarded by lock */
    uint32_t         gen;           /* bumped on each (re)arm */
    CRITICAL_SECTION lock;          /* queue + inflight */
    vcnode          *head;
    vcnode          *tail;
    int              queued;
    size_t           queued_bytes;  /* for backlog pacing */
    volatile LONG    inflight;
    HANDLE           ev_pump;  /* auto-reset */
    HANDLE           ev_stop;  /* manual */
    HANDLE           thread;
    rdpch_link       link;
} vc_xport;

static void flush_queue_locked(vc_xport *v)
{
    vcnode *node = v->head;
    while (node) {
        vcnode *next = node->next;
        free(node);
        node = next;
    }
    v->head = v->tail = NULL;
    v->queued = 0;
    v->queued_bytes = 0;
}

static int vc_send(rdpch_xport *x, const void *frame, size_t len);

static DWORD WINAPI vc_pump(void *arg)
{
    vc_xport *v = (vc_xport *)arg;

    for (;;) {
        HANDLE hs[2];
        DWORD w;
        vcnode *node = NULL;

        hs[0] = v->ev_pump;
        hs[1] = v->ev_stop;
        {
            DWORD timeout = INFINITE;
            /* gate shut while armed: bounded wait so the loop can
             * re-announce HELLO (see RDPCH_HELLO_REANNOUNCE_MS) */
            if (!v->dead && v->armed && !v->link.hello_done)
                timeout = RDPCH_HELLO_REANNOUNCE_MS;
            w = WaitForMultipleObjects(2, hs, FALSE, timeout);
        }
        if (w == WAIT_OBJECT_0 + 1)
            return 0;

        if (w == WAIT_TIMEOUT) {
            /* handshake still unanswered - re-announce until the peer's
             * gate-opening HELLO arrives.  hello_done is written on
             * mstsc data threads; a stale read costs one extra HELLO. */
            if (!v->dead && v->armed && !v->link.hello_done) {
                uint8_t hello[RDPCH_MAX_FRAME];
                size_t n = rdpch_link_hello_frame(hello, sizeof(hello));
                if (n)
                    vc_send(&v->base, hello, n);
            }
            continue;
        }

        EnterCriticalSection(&v->lock);
        if (v->dead) {
            /* disconnected: stay parked.  The pump must survive so a
             * later xport_vc_arm (mstsc reconnect, same process) has a
             * writer again - exiting here killed all sends after the
             * first disconnect, and the stalled HELLO made the server
             * silently drop every frame as pre-hello.  Only ev_stop
             * (teardown) ends the thread. */
            LeaveCriticalSection(&v->lock);
            continue;
        }
        if (!v->inflight) {
            node = v->head;
            if (node) {
                v->head = node->next;
                if (!v->head)
                    v->tail = NULL;
                v->queued--;
                v->queued_bytes -= node->len;
                v->inflight = 1;
                node->gen = v->gen;
            }
        }
        LeaveCriticalSection(&v->lock);

        if (!node)
            continue;

        {
            UINT rc = v->write_fn(v->open_handle, node->data, node->len,
                                  node);
            if (rc != CHANNEL_RC_OK) {
                /* no completion callback will come for a failed write */
                uint32_t gen = node->gen;
                free(node);
                EnterCriticalSection(&v->lock);
                if (gen == v->gen)
                    v->inflight = 0;
                LeaveCriticalSection(&v->lock);
                RLOGW("vc: VirtualChannelWrite failed rc=%u", rc);
            }
        }
        /* success: WRITE_COMPLETE callback finishes the node */
    }
}

/* Server re-handshook (its process or WTS channel bounced): answer with
 * our HELLO so its fresh gate opens.  Runs on whatever mstsc thread
 * delivers data, so only enqueue - the pump thread writes it. */
static void vc_resend_hello(void *user)
{
    vc_xport *v = (vc_xport *)user;
    uint8_t hello[RDPCH_MAX_FRAME];
    size_t n = rdpch_link_hello_frame(hello, sizeof(hello));
    if (n && vc_send(&v->base, hello, n) == 0)
        RLOGI("vc: peer re-handshake, HELLO re-sent");
}

static int vc_send(rdpch_xport *x, const void *frame, size_t len)
{
    vc_xport *v = (vc_xport *)x;
    vcnode *node;

    if (len == 0 || len > RDPCH_MAX_FRAME)
        return -1;

    EnterCriticalSection(&v->lock);
    if (v->dead || !v->armed || v->queued >= VC_MAX_QUEUE) {
        LeaveCriticalSection(&v->lock);
        return -1;
    }
    node = (vcnode *)malloc(sizeof(*node));
    if (!node) {
        LeaveCriticalSection(&v->lock);
        return -1;
    }
    node->next = NULL;
    node->len = (uint32_t)len;
    memcpy(node->data, frame, len);
    if (v->tail)
        v->tail->next = node;
    else
        v->head = node;
    v->tail = node;
    v->queued++;
    v->queued_bytes += len;
    SetEvent(v->ev_pump);
    LeaveCriticalSection(&v->lock);
    return 0;
}

static size_t vc_backlog(rdpch_xport *x)
{
    vc_xport *v = (vc_xport *)x;
    size_t b;

    EnterCriticalSection(&v->lock);
    b = v->queued_bytes;
    LeaveCriticalSection(&v->lock);
    return b;
}

static int vc_start(rdpch_xport *x)
{
    (void)x;
    return 0; /* thread managed by create/teardown */
}

static void vc_stop(rdpch_xport *x)
{
    xport_vc_teardown(x);
}

static rdpch_xport_ops vc_ops = {
    vc_start,
    vc_send,
    vc_backlog,
    vc_stop,
};

rdpch_xport *xport_vc_create(rdpch_vc_write_fn write_fn,
                             const rdpch_xport_cbs *cbs)
{
    vc_xport *v = (vc_xport *)calloc(1, sizeof(*v));

    if (!v)
        return NULL;
    v->base.ops = &vc_ops;
    v->cbs = *cbs;
    v->write_fn = write_fn;
    rdpch_link_init(&v->link, cbs);
    v->link.resend_hello = vc_resend_hello;
    v->link.resend_user  = v;
    InitializeCriticalSection(&v->lock);
    v->ev_pump = CreateEvent(NULL, FALSE, FALSE, NULL);
    v->ev_stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!v->ev_pump || !v->ev_stop ||
        (v->thread = CreateThread(NULL, 0, vc_pump, v, 0, NULL)) == NULL) {
        RLOGE("vc: init failed");
        if (v->ev_pump)
            CloseHandle(v->ev_pump);
        if (v->ev_stop)
            CloseHandle(v->ev_stop);
        DeleteCriticalSection(&v->lock);
        free(v);
        return NULL;
    }
    return &v->base;
}

void xport_vc_arm(rdpch_xport *x, DWORD open_handle)
{
    vc_xport *v = (vc_xport *)x;
    uint8_t hello[RDPCH_MAX_FRAME];
    size_t hello_len;

    EnterCriticalSection(&v->lock);
    v->dead = 0;
    v->armed = 1;
    v->inflight = 0;
    v->gen++;
    v->open_handle = open_handle;
    rdpch_link_reset(&v->link);
    flush_queue_locked(v);
    LeaveCriticalSection(&v->lock);

    if (v->cbs.on_up)
        v->cbs.on_up(v->cbs.user);

    hello_len = rdpch_link_hello_frame(hello, sizeof(hello));
    if (hello_len)
        vc_send(x, hello, hello_len);
    SetEvent(v->ev_pump);
}

void xport_vc_disconnected(rdpch_xport *x)
{
    vc_xport *v = (vc_xport *)x;

    EnterCriticalSection(&v->lock);
    if (v->dead) {
        LeaveCriticalSection(&v->lock);
        return;
    }
    v->dead = 1;
    v->armed = 0;
    flush_queue_locked(v);
    LeaveCriticalSection(&v->lock);
    SetEvent(v->ev_pump);

    if (v->cbs.on_down)
        v->cbs.on_down(v->cbs.user);
}

void xport_vc_on_data(rdpch_xport *x, const void *data, uint32_t len)
{
    vc_xport *v = (vc_xport *)x;

    RLOGD("vc: on_data %u bytes", len);
    if (rdpch_link_feed(&v->link, data, len) != 0) {
        RLOGE("vc: protocol error");
        xport_vc_disconnected(x);
    }
}

void xport_vc_on_write_done(rdpch_xport *x, LPVOID pUserData)
{
    vc_xport *v = (vc_xport *)x;
    vcnode *node = (vcnode *)pUserData;
    uint32_t gen;

    if (!node)
        return;
    gen = node->gen;
    free(node);
    EnterCriticalSection(&v->lock);
    if (gen == v->gen)
        v->inflight = 0; /* current generation: pump may send next */
    LeaveCriticalSection(&v->lock);
    SetEvent(v->ev_pump);
}

void xport_vc_teardown(rdpch_xport *x)
{
    vc_xport *v = (vc_xport *)x;

    EnterCriticalSection(&v->lock);
    v->dead = 1;
    v->armed = 0;
    flush_queue_locked(v);
    LeaveCriticalSection(&v->lock);
    SetEvent(v->ev_pump);
    SetEvent(v->ev_stop);

    if (v->thread) {
        if (WaitForSingleObject(v->thread, 8000) == WAIT_OBJECT_0) {
            CloseHandle(v->thread);
            v->thread = NULL;
        }
        /* else: detach; object is never freed so late completions stay
         * safe */
    }
}
