#ifndef RDPCH_UTIL_H
#define RDPCH_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <winsock2.h>

uint64_t rdpch_now_ms(void);

int rdpch_set_nodelay(SOCKET s);

/* Parse "host[:port]" / "[v6]:port" authority.  Returns 0 on success.
 * Empty host, port 0/out-of-range or overflowed buffers return -1.
 * def_port is used when no explicit port is given. */
int rdpch_parse_authority(const char *s, size_t n,
                          char *host, size_t hostcap,
                          uint16_t *port, uint16_t def_port);

/* Case-insensitive ASCII prefix compare; returns 1 if s starts with pfx. */
int rdpch_istarts_with(const char *s, size_t n, const char *pfx);

#endif /* RDPCH_UTIL_H */
