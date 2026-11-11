#ifndef RDPCH_HTTP_REWRITE_H
#define RDPCH_HTTP_REWRITE_H

#include <stddef.h>
#include <stdint.h>

#define HTRW_MAX_LINE   8192
#define HTRW_AUTH_MAX   280

/* request-line classification */
enum {
    HTRW_LINE_MALFORMED = -1,
    HTRW_LINE_ORIGIN    = 0,  /* "GET /path HTTP/1.1"       */
    HTRW_LINE_HTTP_ABS  = 1,  /* "GET http://h/p HTTP/1.1"  */
    HTRW_LINE_HTTPS_ABS = 2,  /* "GET https://h/p HTTP/1.1" (unsupported) */
    HTRW_LINE_CONNECT   = 3,  /* "CONNECT h:443 HTTP/1.1"   */
};

/* internal states */
enum {
    HTRW_ST_LINE = 0,
    HTRW_ST_HDRS,
    HTRW_ST_BODY_CL,
    HTRW_ST_CHUNK_SIZE,
    HTRW_ST_CHUNK_DATA,
    HTRW_ST_CHUNK_CRLF,
    HTRW_ST_CHUNK_TRAIL,
    HTRW_ST_TUNNEL,
};

typedef struct http_rewriter {
    int   state;
    int (*emit)(void *ctx, const void *data, size_t n);
    void *ectx;

    uint8_t  line[HTRW_MAX_LINE + 1];
    size_t   line_len;

    /* per-request */
    char     cur_authority[HTRW_AUTH_MAX]; /* absolute-form authority */
    int      ver10;
    int      saw_host;
    int      body_kind;   /* 0 none, 1 content-length, 2 chunked */
    int      body_may_follow_close; /* HTTP/1.0 implicit body -> tunnel */
    uint64_t body_left;
    uint64_t chunk_left;

    unsigned long requests;
} http_rewriter;

void htrw_init(http_rewriter *r,
               int (*emit)(void *ctx, const void *data, size_t n),
               void *ectx);

/* Feed browser->channel bytes.  Returns 0 on success, -1 on fatal
 * framing violation (line too big, bad chunk header, emitter failed). */
int htrw_feed(http_rewriter *r, const void *data, size_t n);

/* Parse a request line (without trailing CR/LF).
 * kind = HTRW_LINE_*.  authority (userinfo stripped) and ver10 are
 * filled for the relevant kinds; path (origin-form) for HTTP_ABS. */
int htrw_parse_request_line(const char *line, size_t len,
                            char *method, size_t methodcap,
                            char *authority, size_t authcap,
                            char *path, size_t pathcap,
                            int *ver10);

/* Scan a raw header block (lines with '\n' terminators) for the first
 * Host header; returns 0 on success. */
int htrw_find_host(const void *hdrs, size_t len, char *host, size_t cap);

#endif /* RDPCH_HTTP_REWRITE_H */
