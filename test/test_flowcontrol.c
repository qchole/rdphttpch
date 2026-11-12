/* e2e test: receiver-side backpressure (large-transfer disconnect
 * family), rdpch edition.
 *
 * The Oct sender pacing (backlog_bytes) left the RECEIVE direction
 * without end-to-end flow control: DATA frames buffer into the
 * per-conn to_sock chain capped at sock_buf_max (4 MB) and exceeding
 * it KILLS the connection (CLOSE_OVERFLOW).  A consumer slower than
 * the carrier (slow disk, AV scan, browser stall) fills the buffer in
 * seconds and the transfer dies mid-stream.
 *
 * v2 adds per-conn credit flow control (ACK frames): a sender pauses
 * its socket reads while the peer has > RDPCH_FLOW_WINDOW unacked
 * bytes; the receiver credits consumed bytes back.  These scenarios
 * drive the REAL engines over the mock carrier and must never kill a
 * conn:
 *   A: CONNECT-tunnel upload to a slow-reading target
 *   B: CONNECT-tunnel download to a slow-reading app
 *   C: fast bidirectional tunnel echo (regression guard)
 *   D: HTTP absolute-form POST through the rewriter to a slow target
 *      (exercises MODE_HTTP inflight accounting at conn_emit)
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "log.h"
#include "proxy.h"
#include "../src/srv/transports.h"
#include "../src/cli/transports.h"

#define CARRIER_PORT 19071
#define PROXY_PORT    18171
#define TARGET_PORT   19072

#define TOTAL_A (8u * 1024 * 1024)
#define TOTAL_B (8u * 1024 * 1024)
#define TOTAL_C (4u * 1024 * 1024)
#define TOTAL_D (8u * 1024 * 1024)
#define CHUNK   16384
#define TARGET_DELAY_MS 20   /* slow consumer: ~0.8 MB/s */

static int g_fail;
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

/* ---- target oracle (what the client core dials) ---- */

enum { TM_SINK_SLOW = 0, TM_SRC_FAST = 1, TM_ECHO = 2, TM_HTTP_SINK = 3 };

static volatile LONG g_mode = TM_SINK_SLOW;
static volatile LONG64 g_target_got;   /* body bytes received / sent */
static volatile LONG g_target_bad;     /* content mismatches */
static volatile LONG g_target_eof;     /* saw clean EOF */
static HANDLE g_stop_target;

static uint8_t expect_byte(uint64_t off)
{
    return (uint8_t)((off * 31u + 7u) & 0xFF);
}

static void target_verify(const uint8_t *buf, int n, uint64_t off)
{
    int i;

    for (i = 0; i < n; i++) {
        if (buf[i] != expect_byte(off + (uint64_t)i)) {
            InterlockedIncrement(&g_target_bad);
            return;
        }
    }
}

/* wait until the socket event fires (WOULDBLOCK retry helper) */
static int target_wait(SOCKET s, WSAEVENT ev)
{
    WSANETWORKEVENTS ne;

    if (WaitForSingleObject(ev, 30000) != WAIT_OBJECT_0)
        return -1;
    return WSAEnumNetworkEvents(s, ev, &ne) == 0 ? 0 : -1;
}

/* drain until the HTTP head end ("\r\n\r\n" or "\n\n"); returns head
 * length or -1 */
static int http_read_head(SOCKET s, WSAEVENT ev, uint8_t *out, size_t cap)
{
    size_t have = 0;

    while (have < cap) {
        int n = recv(s, (char *)out + have, (int)(cap - have), 0);

        if (n > 0) {
            size_t i;
            have += (size_t)n;
            for (i = have > 4 ? have - 4 : 0; i + 1 < have; i++) {
                if (out[i] == '\n' && out[i + 1] == '\n')
                    return (int)i + 2;
                if (out[i] == '\n' && out[i + 1] == '\r' &&
                    i + 2 < have && out[i + 2] == '\n')
                    return (int)i + 3;
            }
            continue;
        }
        if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
            if (target_wait(s, ev) != 0)
                return -1;
            continue;
        }
        return -1;
    }
    return -1;
}

static unsigned __stdcall target_conn(void *arg)
{
    SOCKET s = (SOCKET)(uintptr_t)arg;
    WSAEVENT ev = WSACreateEvent();
    uint64_t off = 0;
    LONG mode = InterlockedExchangeAdd(&g_mode, 0);

    WSAEventSelect(s, ev, FD_READ | FD_WRITE | FD_CLOSE);

    for (;;) {
        WSANETWORKEVENTS ne;
        DWORD w = WaitForSingleObject(ev, INFINITE);

        if (w != WAIT_OBJECT_0)
            break;
        if (WSAEnumNetworkEvents(s, ev, &ne) != 0)
            break;
        if (mode == TM_HTTP_SINK) {
            /* read the rewritten request head, then the body slowly */
            uint8_t head[8192];
            int hlen = http_read_head(s, ev, head, sizeof(head));

            if (hlen <= 0)
                goto out;
            for (;;) {
                char buf[CHUNK];
                int n = recv(s, buf, sizeof(buf), 0);

                if (n > 0) {
                    target_verify((const uint8_t *)buf, n, off);
                    off += (uint64_t)n;
                    InterlockedExchangeAdd64(&g_target_got, n);
                    if (off >= TOTAL_D) {
                        static const char ok[] =
                            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
                            "\r\n";
                        send(s, ok, (int)sizeof(ok) - 1, 0);
                        shutdown(s, SD_SEND);
                        InterlockedIncrement(&g_target_eof);
                        goto out;
                    }
                    Sleep(TARGET_DELAY_MS); /* slow consumer */
                    continue;
                }
                if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
                    if (target_wait(s, ev) != 0)
                        goto out;
                    continue;
                }
                goto out;
            }
        }
        if (mode == TM_SRC_FAST &&
            (ne.lNetworkEvents & (FD_READ | FD_WRITE | FD_CLOSE))) {
            /* push TOTAL_B bytes, half-close, drain peer.  Gated on any
             * event: FD_WRITE is the only signal a download-only flow
             * ever produces (the peer sends nothing). */
            while (off < TOTAL_B) {
                uint8_t buf[CHUNK];
                int i, n, soff = 0;

                for (i = 0; i < CHUNK; i++)
                    buf[i] = expect_byte(off + (uint64_t)i);
                n = (int)((TOTAL_B - off) < CHUNK ? (TOTAL_B - off)
                                                  : CHUNK);
                while (soff < n) {
                    int k = send(s, (char *)buf + soff, n - soff, 0);

                    if (k > 0) {
                        soff += k;
                        continue;
                    }
                    if (k == SOCKET_ERROR &&
                        WSAGetLastError() == WSAEWOULDBLOCK) {
                        if (target_wait(s, ev) != 0)
                            goto out;
                        continue;
                    }
                    goto out;
                }
                off += (uint64_t)n;
                InterlockedExchangeAdd64(&g_target_got, n);
            }
            shutdown(s, SD_SEND);
            /* peer reads slowly; wait for its EOF */
            for (;;) {
                char t[CHUNK];
                int n = recv(s, t, sizeof(t), 0);

                if (n == 0) {
                    InterlockedIncrement(&g_target_eof);
                    goto out;
                }
                if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
                    goto out;
                if (n < 0 && target_wait(s, ev) != 0)
                    goto out;
            }
        }
        if (ne.lNetworkEvents & (FD_READ | FD_CLOSE)) {
            if (mode == TM_SINK_SLOW) {
                for (;;) {
                    char buf[CHUNK];
                    int n = recv(s, buf, sizeof(buf), 0);

                    if (n > 0) {
                        target_verify((const uint8_t *)buf, n, off);
                        off += (uint64_t)n;
                        InterlockedExchangeAdd64(&g_target_got, n);
                        Sleep(TARGET_DELAY_MS); /* slow consumer */
                        continue;
                    }
                    if (n == 0) {
                        InterlockedIncrement(&g_target_eof);
                        goto out;
                    }
                    if (WSAGetLastError() != WSAEWOULDBLOCK)
                        goto out;
                    break; /* drained for now */
                }
            } else if (mode == TM_ECHO) {
                /* echo (structure mirrors test/echo_main.c) */
                for (;;) {
                    char buf[CHUNK];
                    int n, off2 = 0;

                    n = recv(s, buf, sizeof(buf), 0);
                    if (n > 0) {
                        while (off2 < n) {
                            int k = send(s, buf + off2, n - off2, 0);

                            if (k > 0) {
                                off2 += k;
                                continue;
                            }
                            if (k == SOCKET_ERROR &&
                                WSAGetLastError() == WSAEWOULDBLOCK) {
                                if (target_wait(s, ev) != 0)
                                    goto out;
                                continue;
                            }
                            goto out;
                        }
                        InterlockedExchangeAdd64(&g_target_got, n);
                        continue;
                    }
                    if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
                        goto out; /* EOF or error */
                    break; /* drained for now */
                }
            }
        }
        if (ne.lNetworkEvents & FD_CLOSE)
            goto out;
    }
out:
    shutdown(s, SD_SEND);
    closesocket(s);
    WSACloseEvent(ev);
    return 0;
}

static unsigned __stdcall target_thread(void *arg)
{
    SOCKET ls;
    WSAEVENT ev;
    struct sockaddr_in sa;
    int one = 1;

    (void)arg;
    ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one,
               sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(TARGET_PORT);
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(ls, SOMAXCONN) != 0) {
        printf("FAIL: target bind/listen\n");
        g_fail++;
        return 0;
    }
    ev = WSACreateEvent();
    WSAEventSelect(ls, ev, FD_ACCEPT);

    for (;;) {
        HANDLE hs[2] = { ev, g_stop_target };
        WSANETWORKEVENTS ne;
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);

        if (w != WAIT_OBJECT_0)
            break;
        if (WSAEnumNetworkEvents(ls, ev, &ne) != 0)
            continue;
        for (;;) {
            SOCKET s = accept(ls, NULL, NULL);
            HANDLE t;

            if (s == INVALID_SOCKET)
                break;
            t = (HANDLE)_beginthreadex(NULL, 0, target_conn,
                                       (void *)(uintptr_t)s, 0, NULL);
            if (t)
                CloseHandle(t);
            else
                closesocket(s);
        }
    }
    closesocket(ls);
    WSACloseEvent(ev);
    return 0;
}

/* ---- app side (the browser/curl dialing the proxy) ---- */

static int g_direct;

/* plain TCP connect to the proxy (or straight to the target when
 * --direct); no CONNECT handshake - for plain-HTTP scenarios */
static SOCKET app_tcp_connect(void)
{
    struct sockaddr_in sa;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD tmo = 60000;

    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(g_direct ? TARGET_PORT : PROXY_PORT);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static SOCKET tunnel_connect(void)
{
    SOCKET s = app_tcp_connect();
    static const char req[] =
        "CONNECT 127.0.0.1:19072 HTTP/1.1\r\n"
        "Host: 127.0.0.1:19072\r\n"
        "\r\n";

    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;
    if (g_direct)
        return s;

    /* CONNECT handshake: send request, expect 200 first line */
    {
        int off = 0;
        while (off < (int)sizeof(req) - 1) {
            int k = send(s, req + off, (int)sizeof(req) - 1 - off, 0);
            if (k <= 0) {
                closesocket(s);
                return INVALID_SOCKET;
            }
            off += k;
        }
    }
    {
        char resp[512];
        int have = 0;
        for (;;) {
            int k;
            if (have >= (int)sizeof(resp) - 1)
                break;
            k = recv(s, resp + have, (int)sizeof(resp) - 1 - have, 0);
            if (k <= 0) {
                closesocket(s);
                return INVALID_SOCKET;
            }
            have += k;
            resp[have] = 0;
            if (strstr(resp, "\r\n\r\n") || strstr(resp, "\n\n"))
                break;
        }
        if (strncmp(resp, "HTTP/1.1 200", 12) != 0 &&
            strncmp(resp, "HTTP/1.0 200", 12) != 0) {
            printf("   CONNECT rejected: %.60s\n", resp);
            closesocket(s);
            return INVALID_SOCKET;
        }
    }
    return s;
}

/* send n pattern bytes starting at stream offset off; -1 on failure */
static int app_send_range(SOCKET s, uint64_t off, uint64_t n, uint64_t *sent_out)
{
    uint64_t done = 0;

    while (done < n) {
        uint8_t buf[CHUNK];
        uint64_t left = n - done;
        int len = (int)(left < CHUNK ? left : CHUNK);
        int i, soff = 0;

        for (i = 0; i < len; i++)
            buf[i] = expect_byte(off + done + (uint64_t)i);
        while (soff < len) {
            int k = send(s, (char *)buf + soff, len - soff, 0);

            if (k > 0) {
                soff += k;
                continue;
            }
            *sent_out += done + (uint64_t)soff;
            return -1; /* conn murdered mid-upload */
        }
        done += (uint64_t)len;
    }
    *sent_out += done;
    return 0;
}

/* read exactly want bytes (verify pattern), then expect clean EOF */
static int app_read_expect(SOCKET s, uint64_t want, int slow, uint64_t *got_out)
{
    uint64_t off = 0;

    while (off < want) {
        uint8_t buf[CHUNK];
        int n = recv(s, (char *)buf, sizeof(buf), 0);

        if (n > 0) {
            int i;
            for (i = 0; i < n; i++) {
                if (buf[i] != expect_byte(off + (uint64_t)i)) {
                    *got_out = off;
                    return -2; /* corruption */
                }
            }
            off += (uint64_t)n;
            *got_out = off;
            if (slow)
                Sleep(TARGET_DELAY_MS);
            continue;
        }
        if (n == 0) {
            *got_out = off;
            return -1; /* early EOF: conn killed mid-transfer */
        }
        *got_out = off;
        return -1; /* timeout / reset */
    }
    /* expect EOF next */
    {
        uint8_t t[16];
        int n = recv(s, (char *)t, sizeof(t), 0);

        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            Sleep(100);
            n = recv(s, (char *)t, sizeof(t), 0);
        }
        if (n != 0) {
            *got_out = off;
            return -3; /* no clean EOF */
        }
    }
    *got_out = off;
    return 0;
}

static void target_reset(LONG mode)
{
    InterlockedExchange(&g_mode, mode);
    InterlockedExchange64(&g_target_got, 0);
    InterlockedExchange(&g_target_bad, 0);
    InterlockedExchange(&g_target_eof, 0);
}

static void wait_target_eof(void)
{
    DWORD deadline = GetTickCount() + 90000;

    while (InterlockedExchangeAdd(&g_target_eof, 0) == 0 &&
           GetTickCount() < deadline)
        Sleep(20);
}

/* ---- scenarios ---- */

static void scenario_upload_slow_sink(void)
{
    SOCKET s;
    uint64_t sent = 0, got = 0;
    int rc;

    printf("-- A: tunnel upload to slow target (%u MB) --\n", TOTAL_A >> 20);
    target_reset(TM_SINK_SLOW);
    s = tunnel_connect();
    CHECK(s != INVALID_SOCKET);
    if (s == INVALID_SOCKET)
        return;

    rc = app_send_range(s, 0, TOTAL_A, &sent);
    if (rc != 0) {
        printf("   DISCONNECT during upload after %.2f MB\n",
               sent / 1048576.0);
    } else {
        shutdown(s, SD_SEND);
        rc = app_read_expect(s, 0, 0, &got); /* expect clean EOF */
    }
    wait_target_eof();
    closesocket(s);

    CHECK(rc == 0);
    CHECK(g_target_bad == 0);
    CHECK(g_target_got == TOTAL_A);
    if (rc == 0 && g_target_bad == 0 && g_target_got == TOTAL_A)
        printf("   ok: %u MB delivered intact\n", TOTAL_A >> 20);
}

static void scenario_download_slow_app(void)
{
    SOCKET s;
    uint64_t got = 0;
    int rc;

    printf("-- B: tunnel download to slow app (%u MB) --\n", TOTAL_B >> 20);
    target_reset(TM_SRC_FAST);
    s = tunnel_connect();
    CHECK(s != INVALID_SOCKET);
    if (s == INVALID_SOCKET)
        return;

    rc = app_read_expect(s, TOTAL_B, 1, &got);
    if (rc != 0)
        printf("   DISCONNECT during download after %.2f MB (rc %d)\n",
               got / 1048576.0, rc);
    closesocket(s);
    wait_target_eof();

    CHECK(rc == 0);
    CHECK(g_target_got == TOTAL_B);
    if (rc == 0 && g_target_got == TOTAL_B)
        printf("   ok: %u MB delivered intact\n", TOTAL_B >> 20);
}

static void scenario_echo_fast(void)
{
    SOCKET s;
    uint64_t sent = 0, got = 0, off = 0;
    int rc = 0;
    const uint64_t win = 256 * 1024;

    printf("-- C: fast bidirectional tunnel echo (%u MB) --\n",
           TOTAL_C >> 20);
    target_reset(TM_ECHO);
    s = tunnel_connect();
    CHECK(s != INVALID_SOCKET);
    if (s == INVALID_SOCKET)
        return;

    /* full-duplex in bounded windows: keep the in-flight backlog per
     * direction small (realistic request/response interleaving) */
    while (rc == 0 && off < TOTAL_C) {
        uint64_t left = TOTAL_C - off;
        int n = (int)((left < win) ? left : win);
        int i;

        rc = app_send_range(s, off, (uint64_t)n, &sent);
        if (rc != 0)
            break;
        for (i = 0; i < n; ) {
            uint8_t rbuf[CHUNK];
            int k = recv(s, (char *)rbuf, sizeof(rbuf), 0);
            int j;

            if (k <= 0) {
                printf("   echo disconnect after %.2f MB\n",
                       (off + (uint64_t)i) / 1048576.0);
                rc = -1;
                break;
            }
            for (j = 0; j < k; j++) {
                if (rbuf[j] != expect_byte(off + (uint64_t)i +
                                                (uint64_t)j)) {
                    printf("   echo corruption at %.2f MB\n",
                           (off + (uint64_t)(i + j)) / 1048576.0);
                    rc = -2;
                    break;
                }
            }
            if (rc != 0)
                break;
            i += k;
            got += (uint64_t)k;
        }
        off += (uint64_t)n;
    }
    if (rc == 0) {
        shutdown(s, SD_SEND);
        /* target half-closes after seeing our EOF; expect clean EOF */
        {
            uint8_t t[16];
            int n = recv(s, (char *)t, sizeof(t), 0);

            if (n != 0) {
                printf("   no clean EOF at echo end\n");
                rc = -3;
            }
        }
    }
    closesocket(s);

    CHECK(rc == 0);
    CHECK(g_target_got == TOTAL_C);
    CHECK(got == TOTAL_C);
}

/* HTTP absolute-form POST through the rewriter: the upload direction
 * then flows via htrw_feed -> conn_emit, exercising the v2 inflight
 * accounting on the rewriter path. */
static void scenario_http_post_slow(void)
{
    SOCKET s;
    uint64_t sent = 0, got = 0;
    int rc;
    char head[256];

    printf("-- D: HTTP POST via rewriter to slow target (%u MB) --\n",
           TOTAL_D >> 20);
    target_reset(TM_HTTP_SINK);
    s = app_tcp_connect(); /* plain HTTP: absolute-form, no CONNECT */
    CHECK(s != INVALID_SOCKET);
    if (s == INVALID_SOCKET)
        return;

    snprintf(head, sizeof(head),
             "POST http://127.0.0.1:19072/upload HTTP/1.1\r\n"
             "Host: 127.0.0.1:19072\r\n"
             "Content-Length: %u\r\n"
             "\r\n", TOTAL_D);
    {
        int off = 0, len = (int)strlen(head);
        while (off < len) {
            int k = send(s, head + off, len - off, 0);
            if (k <= 0) {
                closesocket(s);
                CHECK(!"head send failed");
                return;
            }
            off += k;
        }
    }

    rc = app_send_range(s, 0, TOTAL_D, &sent);
    if (rc != 0) {
        printf("   DISCONNECT during POST after %.2f MB\n",
               sent / 1048576.0);
    } else {
        /* read the empty 200 response */
        char resp[512];
        int have = 0;
        rc = 0;
        while (have < (int)sizeof(resp) - 1) {
            int k = recv(s, resp + have, (int)sizeof(resp) - 1 - have, 0);
            if (k <= 0) {
                rc = -1;
                break;
            }
            have += k;
            resp[have] = 0;
            if (strstr(resp, "\r\n\r\n"))
                break;
        }
        if (rc == 0 && strncmp(resp, "HTTP/1.1 200", 12) != 0) {
            printf("   bad response: %.60s\n", resp);
            rc = -2;
        }
        got = TOTAL_D;
    }
    wait_target_eof();
    closesocket(s);

    CHECK(rc == 0);
    CHECK(g_target_bad == 0);
    CHECK(g_target_got == TOTAL_D);
    if (rc == 0 && g_target_bad == 0 && g_target_got == TOTAL_D)
        printf("   ok: %u MB POSTed intact\n", TOTAL_D >> 20);
}

int main(int argc, char **argv)
{
    WSADATA wd;
    core_cfg ccfg;
    proxy_cfg pcfg;
    rdpch_xport_cbs cbs;
    rdpch_xport *sx, *cx;
    HANDLE tt;
    const char *only = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--only") && i + 1 < argc)
            only = argv[++i];
        else if (!strcmp(argv[i], "--direct"))
            g_direct = 1;
    }

    rlog_init();
    rlog_console(1); /* WARN-level engine kill paths go to stderr */

    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0)
        return 1;

    g_stop_target = CreateEvent(NULL, TRUE, FALSE, NULL);
    tt = (HANDLE)_beginthreadex(NULL, 0, target_thread, NULL, 0, NULL);
    Sleep(200);

    /* server side: proxy listener + mock carrier */
    proxy_cfg_default(&pcfg, PROXY_PORT);
    cbs = proxy_cbs();
    sx = xport_mock_create(CARRIER_PORT, &cbs);
    CHECK(sx != NULL);
    proxy_set_xport(sx);
    CHECK(proxy_start(&pcfg) == 0);
    CHECK(sx->ops->start(sx) == 0);

    /* client side: relay core + connector to the carrier */
    core_cfg_default(&ccfg);
    CHECK(core_start(&ccfg) == 0);
    cbs = core_cbs();
    cx = xport_tcp_connect_create("127.0.0.1", CARRIER_PORT, &cbs);
    CHECK(cx != NULL);
    core_set_xport(cx);
    CHECK(cx->ops->start(cx) == 0);

    Sleep(500); /* carrier + HELLO settle */

    if (!only || only[0] == 'c' || only[0] == 'C')
        scenario_echo_fast();    /* sanity first: chain must work */
    if (!only || only[0] == 'a' || only[0] == 'A')
        scenario_upload_slow_sink();
    if (!only || only[0] == 'b' || only[0] == 'B')
        scenario_download_slow_app();
    if (!only || only[0] == 'd' || only[0] == 'D')
        scenario_http_post_slow();

    core_stop();        /* joins conn workers before the transport dies */
    cx->ops->stop(cx);
    sx->ops->stop(sx);
    proxy_stop();

    SetEvent(g_stop_target);
    WaitForSingleObject(tt, 5000);
    CloseHandle(tt);
    CloseHandle(g_stop_target);
    WSACleanup();

    if (g_fail) {
        printf("%d FAILED\n", g_fail);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
