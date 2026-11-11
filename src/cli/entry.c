/* rdpchcli.dll - mstsc add-in: the client half of the tunnel.
 *
 * Loaded via HKCU\Software\Microsoft\Terminal Server Client\Default\
 * Addins\RDPCH\DllName.  mstsc calls VirtualChannelEntry at startup;
 * we register the "RDPCH" static channel, and on CHANNEL_EVENT_CONNECTED
 * open it and arm the vc transport.  All heavy lifting lives in
 * core.c (relay engine) and transport_vc.c (single-flight writer).
 *
 * The DLL pins itself in the process: mstsc may deliver late
 * WRITE_COMPLETE callbacks after TERMINATED, and the vc transport
 * object is never freed. */
#include <windows.h>
#include <cchannel.h>
#include <pchannel.h>
#include <stdio.h>
#include <string.h>

#include "core.h"
#include "log.h"
#include "transports.h"

#define RDPCH_VC_NAME "RDPCH"

typedef struct {
    CHANNEL_ENTRY_POINTS ep;   /* copy: caller's struct dies with Entry */
    LPVOID               init_handle;
    int                  registered; /* VirtualChannelInit succeeded */
    int                  inited;     /* core + vc transport created */
    rdpch_xport         *xp;
} rdpch_dll;

static rdpch_dll G;

/* must outlive VirtualChannelInit: mstsc stamps status flags into it */
static CHANNEL_DEF g_chan_def;

/* ---- lazy bring-up (first CONNECTED) ---- */

static int ensure_inited(void)
{
    core_cfg cfg;
    rdpch_xport_cbs cbs;

    if (G.inited)
        return 0;
    core_cfg_default(&cfg);
    if (core_start(&cfg) != 0) {
        RLOGE("cli: core_start failed");
        return -1;
    }
    cbs = core_cbs();
    G.xp = xport_vc_create(G.ep.pVirtualChannelWrite, &cbs);
    if (!G.xp) {
        RLOGE("cli: vc transport create failed");
        core_stop();
        return -1;
    }
    core_set_xport(G.xp);
    G.inited = 1;
    return 0;
}

/* ---- mstsc callbacks ---- */

static VOID WINAPI rdpch_open_event(DWORD openHandle, UINT event,
                                    LPVOID pdata, UINT32 dataLength,
                                    UINT32 totalLength, UINT32 dataFlags);

static VOID WINAPI rdpch_init_event(LPVOID pInitHandle, UINT event,
                                    LPVOID pData, UINT dataLength)
{
    switch (event) {
    case CHANNEL_EVENT_INITIALIZED:
        RLOGD("cli: mstsc initialized");
        break;

    case CHANNEL_EVENT_CONNECTED: {
        DWORD open_handle = 0;
        UINT rc;

        if (!G.registered)
            break;
        if (ensure_inited() != 0)
            break;
        rc = G.ep.pVirtualChannelOpen(pInitHandle, &open_handle,
                                      (PCHAR)RDPCH_VC_NAME,
                                      rdpch_open_event);
        if (rc != CHANNEL_RC_OK) {
            /* happens when the server side refused/unknown channel */
            RLOGE("cli: VirtualChannelOpen failed rc=%u", rc);
            break;
        }
        RLOGI("cli: channel %s connected (server %.*S)", RDPCH_VC_NAME,
              dataLength / sizeof(WCHAR), (wchar_t *)pData);
        xport_vc_arm(G.xp, open_handle);
        break;
    }

    case CHANNEL_EVENT_V1_CONNECTED:
        RLOGW("cli: old server, no virtual channel support");
        break;

    case CHANNEL_EVENT_DISCONNECTED:
        RLOGI("cli: disconnected");
        if (G.xp)
            xport_vc_disconnected(G.xp);
        break;

    case CHANNEL_EVENT_TERMINATED:
        RLOGI("cli: terminated, resetting state");
        if (G.xp) {
            core_stop();          /* joins connection workers first */
            xport_vc_teardown(G.xp); /* bounded join of the pump */
        }
        G.xp = NULL;
        G.inited = 0;
        G.registered = 0;
        G.init_handle = NULL;
        break;

    default:
        break;
    }
}

static VOID WINAPI rdpch_open_event(DWORD openHandle, UINT event,
                                    LPVOID pdata, UINT32 dataLength,
                                    UINT32 totalLength, UINT32 dataFlags)
{
    (void)openHandle;
    (void)totalLength;
    (void)dataFlags;
    switch (event) {
    case CHANNEL_EVENT_DATA_RECEIVED:
        /* chunk boundaries carry no framing meaning: feed the bytes */
        if (G.xp)
            xport_vc_on_data(G.xp, pdata, dataLength);
        break;
    case CHANNEL_EVENT_WRITE_COMPLETE:
    case CHANNEL_EVENT_WRITE_CANCELLED:
        if (G.xp)
            xport_vc_on_write_done(G.xp, pdata);
        break;
    default:
        break;
    }
}

/* ---- entry point ---- */

BOOL VCAPITYPE VirtualChannelEntry(PCHANNEL_ENTRY_POINTS pEntryPoints)
{
    UINT rc;

    if (!pEntryPoints ||
        pEntryPoints->cbSize < sizeof(CHANNEL_ENTRY_POINTS))
        return FALSE;

    rlog_init();
    RLOGI("rdpchcli: VirtualChannelEntry (pid %lu)",
          GetCurrentProcessId());

    G.ep = *pEntryPoints; /* copy NOW: invalid after we return */

    memset(&g_chan_def, 0, sizeof(g_chan_def));
    strcpy_s(g_chan_def.name, sizeof(g_chan_def.name), RDPCH_VC_NAME);
    g_chan_def.options = 0; /* no SHOW_PROTOCOL: server sees raw bytes */

    rc = G.ep.pVirtualChannelInit(&G.init_handle, &g_chan_def, 1,
                                  VIRTUAL_CHANNEL_VERSION_WIN2000,
                                  rdpch_init_event);
    if (rc != CHANNEL_RC_OK) {
        RLOGE("rdpchcli: VirtualChannelInit failed rc=%u", rc);
        return FALSE;
    }
    if (!(g_chan_def.options & CHANNEL_OPTION_INITIALIZED)) {
        RLOGE("rdpchcli: channel not marked initialized");
        return FALSE;
    }
    G.registered = 1;
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        /* pin: late mstsc callbacks must not touch freed code */
        {
            HMODULE self;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_PIN,
                               (LPCWSTR)&DllMain, &self);
        }
        /* load canary: raw-Win32 proof of load, independent of the log
         * system, env vars and log levels.  Appends one line per load. */
        {
            WCHAR path[MAX_PATH];
            DWORD n = GetModuleFileNameW(hinst, path, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                WCHAR *slash = wcsrchr(path, L'\\');
                HANDLE f;
                if (slash) {
                    wcscpy(slash + 1, L"load_canary.txt");
                    f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ,
                                    NULL, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, NULL);
                    if (f != INVALID_HANDLE_VALUE) {
                        char line[96];
                        int len = _snprintf(line, sizeof(line),
                                            "load pid=%lu tick=%lu\n",
                                            GetCurrentProcessId(),
                                            GetTickCount());
                        if (len > 0) {
                            DWORD written;
                            WriteFile(f, line, (DWORD)len, &written, NULL);
                        }
                        CloseHandle(f);
                    }
                }
            }
        }
    }
    return TRUE;
}
