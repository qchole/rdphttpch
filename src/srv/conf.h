/* rdpchsrv JSON config reader (minimal, no external deps).
 *
 * File: rdpchsrv.json next to the exe, or --config PATH.  Schema:
 *   {
 *     "bypass": ["192.168.*", "10.*", "*.corp.local"],
 *     "log_level": "debug" | "info" | "warn" | "error"
 *   }
 * "bypass" may also be a single string ("192.168.*,10.*"); entries
 * use WinINet ProxyOverride syntax and are merged with --bypass.
 * "log_level" maps to the rlog levels; the RDPCH_LOG environment
 * variable takes precedence when it names a level.  Unknown keys
 * are ignored so the file can grow later. */
#ifndef RDPCH_SRV_CONF_H
#define RDPCH_SRV_CONF_H

#include <stddef.h>

#include "log.h"

#define CONF_BYPASS_MAX 2048

typedef struct {
    char bypass[CONF_BYPASS_MAX]; /* ";"-joined entries, "" if absent */
    int  log_level;               /* RLOG_DBG..RLOG_ERR, -1 if absent */
} rdpch_conf;

void conf_default(rdpch_conf *cfg);

/* Parse JSON text.  Returns 0 = ok, -1 = malformed / wrong schema. */
int conf_parse(const char *text, rdpch_conf *cfg);

/* Read + parse a UTF-8/ANSI JSON file (BOM tolerated).
 * Returns 0 = ok, 1 = file not found, -1 = read/parse error
 * (details logged). */
int conf_load(const wchar_t *path, rdpch_conf *cfg);

#endif /* RDPCH_SRV_CONF_H */
