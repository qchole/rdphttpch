/* --setproxy / --unsetproxy support: flips the current user's WinINet
 * proxy setting to point at the local listener, backing up whatever
 * was there so --unsetproxy can restore it exactly. */
#ifndef RDPCH_WININET_CFG_H
#define RDPCH_WININET_CFG_H

#include <stdint.h>

/* Point HKCU Internet Settings at 127.0.0.1:port (backs up existing
 * values first; safe to call repeatedly).  "bypass" is an optional
 * ";"/","-separated list of WinINet ProxyOverride entries (e.g.
 * "192.168.*,10.*") merged after the built-in local-only entries;
 * NULL or "" adds nothing.  Returns 0 on success. */
int rdpch_proxy_enable(uint16_t port, const char *bypass);

/* Restore the backed-up settings (or disable the proxy if no backup
 * exists).  Returns 0 on success. */
int rdpch_proxy_disable(void);

#endif /* RDPCH_WININET_CFG_H */
