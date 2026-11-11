#ifndef RDPCH_CLI_TRANSPORTS_H
#define RDPCH_CLI_TRANSPORTS_H

#include <windows.h>

#include "transport.h"

/* ---- mstsc virtual channel transport ----
 * Matches VIRTUALCHANNELWRITE in cchannel.h (VCAPITYPE = _stdcall). */
typedef UINT(WINAPI *rdpch_vc_write_fn)(DWORD open_handle, LPVOID pData,
                                        ULONG dataLength, LPVOID pUserData);

rdpch_xport *xport_vc_create(rdpch_vc_write_fn write_fn,
                             const rdpch_xport_cbs *cbs);
/* (Re)arm with a freshly opened channel handle; sends HELLO. */
void xport_vc_arm(rdpch_xport *x, DWORD open_handle);
/* CHANNEL_EVENT_DISCONNECTED: kill sends, flush queue, notify down. */
void xport_vc_disconnected(rdpch_xport *x);
/* CHANNEL_EVENT_DATA_RECEIVED. */
void xport_vc_on_data(rdpch_xport *x, const void *data, uint32_t len);
/* CHANNEL_EVENT_WRITE_COMPLETE / WRITE_CANCELLED. */
void xport_vc_on_write_done(rdpch_xport *x, LPVOID pUserData);
/* CHANNEL_EVENT_TERMINATED: stop pump thread (bounded join, may leak). */
void xport_vc_teardown(rdpch_xport *x);

/* ---- mock TCP connector (testing) ---- */
rdpch_xport *xport_tcp_connect_create(const char *host, uint16_t port,
                                      const rdpch_xport_cbs *cbs);

#endif /* RDPCH_CLI_TRANSPORTS_H */
