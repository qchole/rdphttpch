#include "util.h"

#include <windows.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

uint64_t rdpch_now_ms(void)
{
    return (uint64_t)GetTickCount64();
}

int rdpch_set_nodelay(SOCKET s)
{
    BOOL one = TRUE;
    return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one,
                      sizeof(one)) == 0 ? 0 : -1;
}

int rdpch_istarts_with(const char *s, size_t n, const char *pfx)
{
    size_t m = strlen(pfx);
    if (n < m)
        return 0;
    return _strnicmp(s, pfx, m) == 0;
}

static int parse_port(const char *s, size_t n, uint16_t def_port, uint16_t *out)
{
    char tmp[8];
    unsigned long v;

    if (n == 0) {
        *out = def_port;
        return 0;
    }
    if (n >= sizeof(tmp))
        return -1;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    v = strtoul(tmp, NULL, 10);
    if (v == 0 || v > 65535)
        return -1;
    *out = (uint16_t)v;
    return 0;
}

/* host[:port] / [v6][:port] / bare-ipv6 */
int rdpch_parse_authority(const char *s, size_t n,
                          char *host, size_t hostcap,
                          uint16_t *port, uint16_t def_port)
{
    const char *hostptr;
    size_t hostlen;
    const char *ports;
    size_t portslen;

    if (n == 0 || hostcap < 2)
        return -1;

    if (s[0] == '[') {
        const char *close = (const char *)memchr(s, ']', n);
        if (!close || close == s + 1)
            return -1;
        hostptr = s + 1;
        hostlen = (size_t)(close - hostptr);
        ports = close + 1;
        portslen = n - (size_t)(ports - s);
        if (portslen > 0) {
            if (ports[0] != ':')
                return -1;
            ports++;
            portslen--;
        }
    } else {
        size_t i, colons = 0, colon_at = 0;
        for (i = 0; i < n; i++) {
            if (s[i] == ':') {
                colons++;
                if (colons == 1)
                    colon_at = i;
            }
        }
        if (colons == 1) {
            hostptr = s;
            hostlen = colon_at;
            ports = s + colon_at + 1;
            portslen = n - colon_at - 1;
        } else {
            /* 0 colons (host only) or >=2 colons (bare IPv6 literal) */
            hostptr = s;
            hostlen = n;
            ports = s + n;
            portslen = 0;
        }
    }

    if (hostlen == 0 || hostlen >= hostcap)
        return -1;
    memcpy(host, hostptr, hostlen);
    host[hostlen] = '\0';

    return parse_port(ports, portslen, def_port, port);
}
