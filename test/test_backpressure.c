/* Unit test: transport backlog pacing.
 *
 * Regression for the SFTP-upload breakage: bulk senders used to fire
 * DATA frames at the carrier until its queue filled; the failing
 * send() then read as a dead channel and the connection was killed
 * mid-transfer.  The engines now pace their socket reads with
 * backlog_bytes() so a slow carrier throttles the source via TCP
 * backpressure instead.
 *
 * Drives the real ops layer: xport_mock_create + a raw TCP peer that
 * reads slower than the sender pushes. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "protocol.h"
#include "transports.h"
#include "transport.h"
#include "util.h"

#define N_FRAMES   12000u /* ~17 MB: sustained congestion past any
                             socket-buffer cushioning */
#define FRAME_LEN  RDPCH_MAX_FRAME /* 1409 = 9 hdr + 1400 payload */
#define TEST_PORT  21117

static int g_fail;
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

static void on_up_noop(void *u)   { (void)u; }
static void on_down_noop(void *u) { (void)u; }
static void on_frame_noop(void *u, const rdpch_frame *f) { (void)f; (void)u; }

/* ---- sender thread: engine-style pacing, every send must succeed ---- */

static volatile LONG g_send_done;
static volatile LONG g_send_fail;

static unsigned __stdcall sender_thread(void *arg)
{
    rdpch_xport *x = (rdpch_xport *)arg;
    uint8_t frame[FRAME_LEN];
    uint8_t payload[RDPCH_MAX_PAYLOAD];
    unsigned i;

    memset(payload, 'A', sizeof(payload));
    CHECK(rdpch_frame_build(frame, sizeof(frame), 1, RDPCH_CMD_DATA,
                            payload, sizeof(payload)) == FRAME_LEN);

    for (i = 0; i < N_FRAMES; i++) {
        /* exactly what the engines do before each chunk */
        while (x->ops->backlog_bytes &&
               x->ops->backlog_bytes(x) > RDPCH_SEND_BACKLOG_MAX)
            Sleep(5);
        if (x->ops->send(x, frame, FRAME_LEN) != 0) {
            InterlockedExchange(&g_send_fail, 1);
            break;
        }
    }
    InterlockedExchange(&g_send_done, 1);
    return 0;
}

/* ---- raw TCP peer helpers ---- */

static SOCKET peer_connect(uint16_t port)
{
    struct sockaddr_in sa;
    SOCKET s;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(port);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

/* read exactly n bytes; 0 ok, -1 timeout/error */
static int peer_read_n(SOCKET s, uint8_t *buf, int n, DWORD deadline_ms)
{
    int got = 0;

    while (got < n) {
        int r;

        if (rdpch_now_ms() > deadline_ms)
            return -1;
        r = recv(s, (char *)buf + got, n - got, 0);
        if (r == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                Sleep(1);
                continue;
            }
            return -1;
        }
        if (r == 0)
            return -1;
        got += r;
    }
    return 0;
}

static int peer_make_nonblocking(SOCKET s)
{
    u_long one = 1;
    return ioctlsocket(s, FIONBIO, &one);
}

static void test_paced_bulk(void)
{
    rdpch_xport_cbs cbs;
    rdpch_xport *x;
    SOCKET s = INVALID_SOCKET;
    uint8_t hello[FRAME_LEN], buf[16384];
    uint8_t first_block[FRAME_LEN];
    HANDLE th;
    DWORD deadline;
    unsigned long long got_data = 0;
    unsigned long long want_data = (unsigned long long)N_FRAMES * FRAME_LEN;
    int first_block_checked = 0;

    cbs.on_up = on_up_noop;
    cbs.on_frame = on_frame_noop;
    cbs.on_down = on_down_noop;
    cbs.user = NULL;

    x = xport_mock_create(TEST_PORT, &cbs);
    CHECK(x != NULL);
    if (!x)
        return;
    CHECK(x->ops->start(x) == 0);
    CHECK(x->ops->backlog_bytes != NULL);
    Sleep(200); /* listener up */

    s = peer_connect(TEST_PORT);
    CHECK(s != INVALID_SOCKET);
    if (s == INVALID_SOCKET)
        goto out;
    CHECK(peer_make_nonblocking(s) == 0);

    /* HELLO handshake: carrier sends its own on connect */
    deadline = (DWORD)(rdpch_now_ms() + 5000);
    CHECK(peer_read_n(s, hello, RDPCH_FRAME_HEADER_SIZE + 6,
                      deadline) == 0);
    CHECK(rdpch_link_hello_frame(hello, sizeof(hello)) ==
          RDPCH_FRAME_HEADER_SIZE + 6);
    {
        int off = 0;
        while (off < RDPCH_FRAME_HEADER_SIZE + 6) {
            int r = send(s, (char *)hello + off,
                         RDPCH_FRAME_HEADER_SIZE + 6 - off, 0);
            if (r == SOCKET_ERROR) {
                Sleep(1);
                continue;
            }
            off += r;
        }
    }

    th = (HANDLE)_beginthreadex(NULL, 0, sender_thread, x, 0, NULL);
    CHECK(th != NULL);

    /* slow reader: keeps the carrier congested for the whole run */
    deadline = (DWORD)(rdpch_now_ms() + 120000);
    {
        uint8_t h[FRAME_LEN];
        size_t have = 0;

        while (got_data < want_data && rdpch_now_ms() < deadline &&
               !InterlockedExchangeAdd(&g_send_fail, 0)) {
            int r = recv(s, (char *)buf, sizeof(buf), 0);
            if (r == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) {
                    Sleep(2); /* keep the carrier starved on purpose */
                    continue;
                }
                break;
            }
            if (r == 0)
                break;
            /* every byte after the handshake is a full 1409-byte DATA
             * frame; verify blocks byte-wise */
            {
                size_t i = 0;
                while (i < (size_t)r) {
                    size_t take = FRAME_LEN - have;
                    int full;

                    if (take > (size_t)r - i)
                        take = (size_t)r - i;
                    if (have == 0 && take == FRAME_LEN) {
                        memcpy(h, buf + i, FRAME_LEN);
                        have = FRAME_LEN;
                        i += FRAME_LEN;
                    } else {
                        memcpy(h + have, buf + i, take);
                        have += take;
                        i += take;
                    }
                    full = (have == FRAME_LEN);
                    if (full) {
                        if (!first_block_checked) {
                            memcpy(first_block, h, FRAME_LEN);
                            first_block_checked = 1;
                        } else if (memcmp(first_block, h,
                                          FRAME_LEN) != 0) {
                            CHECK(!"frame data mismatch");
                            goto finish;
                        }
                        got_data += FRAME_LEN;
                        have = 0;
                    }
                }
            }
        }
finish: ;
    }

    CHECK(g_send_fail == 0);
    CHECK(g_send_done == 1);
    CHECK(got_data == want_data);
    if (th) {
        WaitForSingleObject(th, 10000);
        CloseHandle(th);
    }
out:
    if (s != INVALID_SOCKET)
        closesocket(s);
    x->ops->stop(x);
}

/* non-blocking send must still fail fast on a full queue (the kill
 * valve behind the pacing stays intact) */
static void test_nonblocking_still_fails_when_full(void)
{
    rdpch_xport_cbs cbs;
    rdpch_xport *x;
    SOCKET s = INVALID_SOCKET;
    uint8_t frame[FRAME_LEN], hello[FRAME_LEN];
    uint8_t payload[RDPCH_MAX_PAYLOAD];
    int i;

    cbs.on_up = on_up_noop;
    cbs.on_frame = on_frame_noop;
    cbs.on_down = on_down_noop;
    cbs.user = NULL;

    x = xport_mock_create(TEST_PORT + 1, &cbs);
    CHECK(x != NULL);
    if (!x)
        return;
    CHECK(x->ops->start(x) == 0);
    Sleep(200);

    s = peer_connect(TEST_PORT + 1);
    CHECK(s != INVALID_SOCKET);
    if (s == INVALID_SOCKET)
        goto out;
    CHECK(peer_make_nonblocking(s) == 0);
    CHECK(peer_read_n(s, hello, RDPCH_FRAME_HEADER_SIZE + 6,
                      (DWORD)(rdpch_now_ms() + 5000)) == 0);

    memset(payload, 'B', sizeof(payload));
    CHECK(rdpch_frame_build(frame, sizeof(frame), 1, RDPCH_CMD_DATA,
                            payload, sizeof(payload)) == FRAME_LEN);

    /* the peer reads nothing: kernel socket buffers absorb the first
     * chunk, after that the queue fills and the next send must be
     * refused at once.  Push until first refusal. */
    for (i = 0; i < 100000; i++) {
        if (x->ops->send(x, frame, FRAME_LEN) != 0)
            break;
    }
    CHECK(i < 100000); /* refusal actually happened */
    printf("nonblocking: refused after %d queued frames\n", i);
out:
    if (s != INVALID_SOCKET)
        closesocket(s);
    x->ops->stop(x);
}

int main(void)
{
    WSADATA wd;
    rlog_init();
    rlog_console(0);
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0)
        return 1;

    test_paced_bulk();
    test_nonblocking_still_fails_when_full();

    WSACleanup();
    if (g_fail) {
        printf("%d FAILED\n", g_fail);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
