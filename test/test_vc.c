/* Unit test for src/cli/transport_vc.c: the pump thread must survive
 * DISCONNECTED so a same-process mstsc reconnect (xport_vc_arm) can
 * write again.  Regression: the pump used to exit on dead, so after
 * the first disconnect nothing was ever written and the server
 * silently dropped all client frames as pre-hello. */
#include <windows.h>
#include <cchannel.h>
#include <stdio.h>
#include <string.h>

#include "transports.h"

static int g_fail;
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

static struct {
    HANDLE  ev;          /* signaled on every accepted write */
    DWORD   handles[32]; /* open_handle per accepted write */
    int     writes;
} stub;

static rdpch_xport *g_xp; /* for the WRITE_COMPLETE emulation */

static UINT WINAPI fake_write(DWORD open_handle, LPVOID pData,
                              ULONG dataLength, LPVOID pUserData)
{
    (void)pData;
    (void)dataLength;
    if (stub.writes < 32)
        stub.handles[stub.writes] = open_handle;
    stub.writes++;
    SetEvent(stub.ev);
    /* emulate mstsc completing the write: frees the node and lets
     * the pump send the next one */
    xport_vc_on_write_done(g_xp, pUserData);
    return CHANNEL_RC_OK;
}

static int wait_write(int timeout_ms)
{
    if (WaitForSingleObject(stub.ev, (DWORD)timeout_ms) != WAIT_OBJECT_0)
        return 0;
    return 1;
}

static void on_up(void *u)   { (void)u; }
static void on_down(void *u) { (void)u; }
static void on_frame(void *u, const rdpch_frame *f) { (void)u; (void)f; }

int main(void)
{
    rdpch_xport_cbs cbs;
    rdpch_xport *xp;
    uint8_t frame[32];
    size_t flen;

    stub.ev = CreateEvent(NULL, FALSE, FALSE, NULL);

    cbs.on_up = on_up;
    cbs.on_frame = on_frame;
    cbs.on_down = on_down;
    cbs.user = NULL;

    xp = xport_vc_create(fake_write, &cbs);
    CHECK(xp != NULL);
    g_xp = xp;

    flen = rdpch_link_hello_frame(frame, sizeof(frame));
    CHECK(flen > 0);

    /* ---- first connect ---- */
    xport_vc_arm(xp, 100);
    CHECK(wait_write(2000));
    CHECK(stub.handles[0] == 100); /* arm's HELLO */

    CHECK(xp->ops->send(xp, frame, flen) == 0);
    CHECK(wait_write(2000));       /* pump alive, node accepted */

    /* ---- network drop ---- */
    xport_vc_disconnected(xp);
    /* give the pump time to actually observe dead (and, in the buggy
     * variant, exit) - disconnected/arm back-to-back can race past it */
    Sleep(200);
    CHECK(xp->ops->send(xp, frame, flen) == -1); /* armed=0: rejected */

    /* ---- same-process reconnect ---- */
    xport_vc_arm(xp, 200);
    CHECK(wait_write(2000));       /* REGRESSION: pump must still exist */
    CHECK(stub.handles[stub.writes - 1] == 200); /* fresh HELLO, new handle */

    CHECK(xp->ops->send(xp, frame, flen) == 0);
    CHECK(wait_write(2000));       /* and traffic keeps flowing */

    /* ---- re-announce: gate shut -> HELLO repeats ~1 Hz ----
     * Regression: the peer's answering HELLO can be swallowed by its
     * echo rate limit (bounce storm), leaving this side's gate shut
     * forever - the peer keeps answering, every reply lands in the
     * pre-hello drop, the proxy dies with 504s and nothing retries. */
    xport_vc_arm(xp, 300);
    CHECK(wait_write(2000));       /* arm's HELLO */
    CHECK(wait_write(2500));       /* re-announce must re-send on its own */
    {
        int i;
        for (i = 0; i < 10 && wait_write(50); i++)
            ;                      /* drain any in-flight re-announce */
    }
    /* peer answers: feed a valid HELLO, gate opens, quiet follows */
    xport_vc_on_data(xp, frame, (uint32_t)flen);
    Sleep(1600);
    CHECK(!wait_write(300));       /* gate open: no more re-announces */

    xport_vc_teardown(xp);

    CloseHandle(stub.ev);
    if (g_fail) {
        printf("%d FAILED\n", g_fail);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
