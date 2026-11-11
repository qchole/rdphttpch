/* rdpchclihost.exe - console host for the client relay core.
 *
 * Connects the core to a mock TCP carrier (rdpchsrv --mock-listen N)
 * so the full chain can be exercised without a real RDP session.
 * The real deployment is rdpchcli.dll inside mstsc.exe (entry.c). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "log.h"
#include "transports.h"
#include "util.h"

static HANDLE g_stop;

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    SetEvent(g_stop);
    return TRUE;
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s --connect HOST:PORT\n", prog);
}

int main(int argc, char **argv)
{
    char host[RDPCH_HOST_MAX + 2] = "127.0.0.1";
    uint16_t port = 0;
    core_cfg cfg;
    rdpch_xport *xp = NULL;
    rdpch_xport_cbs cbs;
    int i;

    rlog_init();
    rlog_console(1);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--connect") && i + 1 < argc) {
            if (rdpch_parse_authority(argv[++i], strlen(argv[i]), host,
                                      sizeof(host), &port, 0) != 0 ||
                port == 0) {
                fprintf(stderr, "bad --connect value (need HOST:PORT)\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if (port == 0) {
        usage(argv[0]);
        return 2;
    }

    g_stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_stop || !SetConsoleCtrlHandler(ctrl_handler, TRUE)) {
        fprintf(stderr, "init failed\n");
        return 1;
    }

    core_cfg_default(&cfg);
    if (core_start(&cfg) != 0) {
        fprintf(stderr, "core_start failed\n");
        return 1;
    }

    cbs = core_cbs();
    xp = xport_tcp_connect_create(host, port, &cbs);
    if (!xp) {
        fprintf(stderr, "cannot create TCP transport\n");
        core_stop();
        return 1;
    }
    core_set_xport(xp);

    if (xp->ops->start(xp) != 0) {
        fprintf(stderr, "transport failed to start\n");
        xp->ops->stop(xp);
        core_stop();
        return 1;
    }

    RLOGI("clihost: relaying via %s:%u (Ctrl-C to stop)", host,
          (unsigned)port);
    WaitForSingleObject(g_stop, INFINITE);
    RLOGI("clihost: shutting down");

    core_stop();      /* joins connection workers first */
    xp->ops->stop(xp); /* frees the transport */
    return 0;
}
