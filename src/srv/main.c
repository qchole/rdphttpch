/* rdpchsrv.exe - server half, runs inside the RDP session.
 *
 *   rdpchsrv [--port N] [--max-conns N] [--setproxy [--bypass LIST]]
 *            [--config FILE] [--unsetproxy]
 *   rdpchsrv --mock-listen N [--port N] ...   (loopback testing)
 *
 * Startup order matters: the proxy must be running before the channel
 * transport starts delivering frames, and --setproxy flips the system
 * setting only once everything is up. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "conf.h"
#include "log.h"
#include "proxy.h"
#include "transports.h"
#include "wininet_cfg.h"

static HANDLE g_stop;

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    SetEvent(g_stop);
    return TRUE;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --port N          HTTP proxy listen port (default 18080)\n"
            "  --max-conns N     max concurrent browser connections (default 128)\n"
            "  --mock-listen N   use mock TCP carrier on port N (testing, no RDP)\n"
            "  --setproxy        point the user's WinINet proxy at 127.0.0.1:PORT\n"
            "  --bypass LIST     with --setproxy: extra WinINet ProxyOverride\n"
            "                    entries that bypass the proxy and connect\n"
            "                    directly, \";\" or \",\" separated, e.g.\n"
            "                    \"192.168.*,10.*,*.corp.local\" (matched by\n"
            "                    name as typed, no DNS resolution)\n"
            "  --config FILE     JSON config (default: rdpchsrv.json next to\n"
            "                    the exe); its \"bypass\" entries merge with\n"
            "                    --bypass\n"
            "  --unsetproxy      restore the backed-up proxy settings and exit\n",
            prog);
}

static int parse_u16(const char *s, uint16_t *out)
{
    char *end;
    unsigned long v = strtoul(s, &end, 10);

    if (*s == 0 || *end != 0 || v == 0 || v > 65535)
        return -1;
    *out = (uint16_t)v;
    return 0;
}

static int parse_int(const char *s, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);

    if (*s == 0 || *end != 0 || v < 1 || v > 4096)
        return -1;
    *out = (int)v;
    return 0;
}

static void argv_to_wide(const char *s, wchar_t *out, size_t cap)
{
    if (MultiByteToWideChar(CP_ACP, 0, s, -1, out, (int)cap) <= 0 && cap)
        out[0] = 0;
}

static const char *level_name(int lv)
{
    switch (lv) {
    case RLOG_DBG:  return "debug";
    case RLOG_INFO: return "info";
    case RLOG_WARN: return "warn";
    default:        return "error";
    }
}

/* default config path: <exe dir>\rdpchsrv.json */
static void default_conf_path(wchar_t *path, size_t cap)
{
    static const wchar_t name[] = L"rdpchsrv.json";
    wchar_t *slash;
    size_t n;

    path[0] = 0;
    GetModuleFileNameW(NULL, path, (DWORD)cap);
    slash = wcsrchr(path, L'\\');
    if (slash)
        slash[1] = 0;
    else
        path[0] = 0; /* no dir component: fall back to cwd */
    n = wcslen(path);
    if (n + sizeof(name) / sizeof(name[0]) > cap) {
        path[0] = 0; /* absurdly long: reported as "not found" */
        return;
    }
    wcscpy(path + n, name);
}

int main(int argc, char **argv)
{
    uint16_t port = 18080;
    uint16_t mock_port = 0;
    int max_conns = 128;
    int do_setproxy = 0, do_unsetproxy = 0;
    const char *bypass = NULL;
    const char *conf_arg = NULL;
    char bypass_all[2048] = "";
    rdpch_xport *xp = NULL;
    proxy_cfg pcfg;
    rdpch_xport_cbs cbs;
    HANDLE mutex = NULL;
    int i;

    rlog_init();
    rlog_console(1);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            if (parse_u16(argv[++i], &port) != 0) {
                fprintf(stderr, "bad --port value\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--mock-listen") && i + 1 < argc) {
            if (parse_u16(argv[++i], &mock_port) != 0) {
                fprintf(stderr, "bad --mock-listen value\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--max-conns") && i + 1 < argc) {
            if (parse_int(argv[++i], &max_conns) != 0) {
                fprintf(stderr, "bad --max-conns value\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--setproxy")) {
            do_setproxy = 1;
        } else if (!strcmp(argv[i], "--bypass") && i + 1 < argc) {
            bypass = argv[++i];
        } else if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            conf_arg = argv[++i];
        } else if (!strcmp(argv[i], "--unsetproxy")) {
            do_unsetproxy = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (bypass && !do_setproxy) {
        fprintf(stderr, "--bypass has no effect without --setproxy\n");
        return 2;
    }

    if (do_unsetproxy) {
        return rdpch_proxy_disable() == 0 ? 0 : 1;
    }

    /* config file: "bypass" merges with --bypass; "log_level"
     * applies unless RDPCH_LOG named a level (file errors are
     * logged in conf.c) */
    {
        wchar_t cpath[MAX_PATH + 16];
        rdpch_conf cfg;

        if (conf_arg)
            argv_to_wide(conf_arg, cpath,
                         sizeof(cpath) / sizeof(cpath[0]));
        else
            default_conf_path(cpath, sizeof(cpath) / sizeof(cpath[0]));
        if (conf_load(cpath, &cfg) == 0) {
            if (cfg.log_level >= 0) {
                rlog_set_level(cfg.log_level);
                /* logged at the configured level, so it survives */
                rlog_write(cfg.log_level,
                           "conf: log_level = %s",
                           level_name(cfg.log_level));
            }
            if (cfg.bypass[0])
                RLOGI("conf: %ls: bypass = %s", cpath, cfg.bypass);
        }

        if (cfg.bypass[0] && bypass && bypass[0])
            snprintf(bypass_all, sizeof(bypass_all), "%s;%s",
                     cfg.bypass, bypass);
        else if (bypass && bypass[0])
            snprintf(bypass_all, sizeof(bypass_all), "%s", bypass);
        else
            snprintf(bypass_all, sizeof(bypass_all), "%s", cfg.bypass);
    }

    g_stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_stop || !SetConsoleCtrlHandler(ctrl_handler, TRUE)) {
        fprintf(stderr, "init failed\n");
        return 1;
    }

    /* one instance per session ("Local\" is session-scoped) */
    mutex = CreateMutexW(NULL, TRUE, L"Local\\rdpchsrv");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "rdpchsrv already running in this session\n");
        return 1;
    }

    proxy_cfg_default(&pcfg, port);
    pcfg.max_conns = max_conns;

    cbs = proxy_cbs();
    if (mock_port)
        xp = xport_mock_create(mock_port, &cbs);
    else
        xp = xport_wts_create(&cbs);
    if (!xp) {
        fprintf(stderr, "cannot create %s transport\n",
                mock_port ? "mock" : "WTS");
        return 1;
    }

    RLOGI("rdpchsrv starting (pid %lu, %s mode, proxy port %u)",
          GetCurrentProcessId(), mock_port ? "mock" : "wts",
          (unsigned)port);

    proxy_set_xport(xp);
    if (proxy_start(&pcfg) != 0) {
        fprintf(stderr, "proxy failed to start on 127.0.0.1:%u\n",
                (unsigned)port);
        xp->ops->stop(xp);
        return 1;
    }
    if (xp->ops->start(xp) != 0) {
        fprintf(stderr, "transport failed to start\n");
        proxy_stop();
        xp->ops->stop(xp);
        return 1;
    }

    if (do_setproxy) {
        if (rdpch_proxy_enable(port, bypass_all[0] ? bypass_all : NULL)
                != 0)
            RLOGW("setproxy failed; continuing without system proxy");
    }

    RLOGI("rdpchsrv ready (Ctrl-C to stop)");
    WaitForSingleObject(g_stop, INFINITE);
    RLOGI("shutting down");

    if (do_setproxy)
        rdpch_proxy_disable(); /* restore what --setproxy changed */

    proxy_stop();
    xp->ops->stop(xp); /* frees the transport */

    if (mutex)
        CloseHandle(mutex);
    RLOGI("bye");
    return 0;
}
