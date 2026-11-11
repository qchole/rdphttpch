#include "log.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static int      g_level = RLOG_DBG;
static FILE    *g_file;
static int      g_console;
static int      g_inited;
static CRITICAL_SECTION g_cs;

static int level_from_name(const char *s)
{
    if (_stricmp(s, "debug") == 0)
        return RLOG_DBG;
    if (_stricmp(s, "info") == 0)
        return RLOG_INFO;
    if (_stricmp(s, "warn") == 0)
        return RLOG_WARN;
    if (_stricmp(s, "error") == 0)
        return RLOG_ERR;
    return -1;
}

static void ensure_init(void)
{
    const char *env;
    char path[MAX_PATH];

    if (g_inited)
        return;
    g_inited = 1;
    InitializeCriticalSection(&g_cs);

    env = getenv("RDPCH_LOG");
    if (!env)
        return;
    {
        int lv = level_from_name(env);
        if (lv >= 0)
            g_level = lv;
        else if (GetFullPathNameA(env, MAX_PATH, path, NULL) &&
                 (g_file = fopen(path, "ab")) != NULL) {
            setvbuf(g_file, NULL, _IONBF, 0);
        }
    }
}

void rlog_init(void)
{
    ensure_init();
}

/* Runtime level override (config file).  No-op when RDPCH_LOG
 * already named a level -- the environment variable wins. */
void rlog_set_level(int level)
{
    const char *env;

    if (level < RLOG_DBG || level > RLOG_ERR)
        return;
    ensure_init();
    env = getenv("RDPCH_LOG");
    if (env && level_from_name(env) >= 0)
        return;
    g_level = level;
}

void rlog_console(int enable)
{
    ensure_init();
    g_console = enable;
}

static const char *level_name(int level)
{
    switch (level) {
    case RLOG_DBG:  return "DBG";
    case RLOG_INFO: return "INF";
    case RLOG_WARN: return "WRN";
    default:        return "ERR";
    }
}

void rlog_write(int level, const char *fmt, ...)
{
    char buf[1024];
    char line[1200];
    int n;
    va_list ap;
    ULONGLONG ms = GetTickCount64();

    ensure_init();
    if (level < g_level)
        return;

    va_start(ap, fmt);
    n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;

    _snprintf_s(line, sizeof(line), _TRUNCATE, "[rdpch %s %llu.%03llu] %s\r\n",
                level_name(level), (unsigned long long)(ms / 1000),
                (unsigned long long)(ms % 1000), buf);

    OutputDebugStringA(line);

    EnterCriticalSection(&g_cs);
    if (g_console)
        fputs(line, stderr);
    if (g_file) {
        fputs(line, g_file);
    }
    LeaveCriticalSection(&g_cs);
}
