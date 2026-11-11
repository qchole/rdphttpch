/* Minimal leveled logging.
 *
 * Output goes to OutputDebugStringA (viewable in DebugView / VS debugger)
 * always.  Additionally:
 *  - console EXEs call rlog_console(1) to mirror to stderr
 *  - if env RDPCH_LOG is set to a file path, output is appended there
 *  - if RDPCH_LOG is one of debug|info|warn|error it acts as level filter
 */
#ifndef RDPCH_LOG_H
#define RDPCH_LOG_H

enum {
    RLOG_DBG = 0,
    RLOG_INFO,
    RLOG_WARN,
    RLOG_ERR,
};

void rlog_init(void);
void rlog_console(int enable);
void rlog_write(int level, const char *fmt, ...);
/* Runtime level override (config file); ignored when RDPCH_LOG
 * names a level. */
void rlog_set_level(int level);

#define RLOGD(...) rlog_write(RLOG_DBG, __VA_ARGS__)
#define RLOGI(...) rlog_write(RLOG_INFO, __VA_ARGS__)
#define RLOGW(...) rlog_write(RLOG_WARN, __VA_ARGS__)
#define RLOGE(...) rlog_write(RLOG_ERR, __VA_ARGS__)

#endif /* RDPCH_LOG_H */
