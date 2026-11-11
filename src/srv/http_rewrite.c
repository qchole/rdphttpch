#include "http_rewrite.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

static int emit_str(http_rewriter *r, const char *s)
{
    return r->emit(r->ectx, s, strlen(s));
}

static int emit_mem(http_rewriter *r, const void *p, size_t n)
{
    if (n == 0)
        return 0;
    return r->emit(r->ectx, p, n);
}

/* does the line start with "name:" (case-insensitive)? */
static int hdr_is(const char *line, size_t len, const char *name)
{
    size_t n = strlen(name);
    if (len <= n)
        return 0;
    if (line[n] != ':')
        return 0;
    return _strnicmp(line, name, n) == 0;
}

static const char *hdr_value(const char *line, size_t len, const char *name,
                             size_t *vlen)
{
    size_t n = strlen(name);
    const char *v = line + n + 1;

    *vlen = len - n - 1;
    while (*vlen > 0 && (*v == ' ' || *v == '\t')) {
        v++;
        (*vlen)--;
    }
    return v;
}

static int parse_abs_authority(const char *tgt, size_t tlen,
                               char *authority, size_t authcap)
{
    const char *auth = tgt;
    const char *end = tgt + tlen;
    const char *p = auth;
    const char *at = NULL;

    while (p < end && *p != '/' && *p != '?' && *p != '#') {
        if (*p == '@')
            at = p;
        p++;
    }
    if (at)
        auth = at + 1;
    if (auth >= p)
        return -1;
    if ((size_t)(p - auth) >= authcap)
        return -1;
    memcpy(authority, auth, (size_t)(p - auth));
    authority[p - auth] = '\0';
    return 0;
}

int htrw_parse_request_line(const char *line, size_t len,
                            char *method, size_t methodcap,
                            char *authority, size_t authcap,
                            char *path, size_t pathcap,
                            int *ver10)
{
    const char *sp1, *sp2, *tgt, *ver;
    size_t mlen, tlen;

    method[0] = authority[0] = '\0';
    if (path)
        path[0] = '\0';
    *ver10 = 0;

    sp1 = (const char *)memchr(line, ' ', len);
    if (!sp1)
        return HTRW_LINE_MALFORMED;
    mlen = (size_t)(sp1 - line);
    if (mlen == 0 || mlen >= methodcap)
        return HTRW_LINE_MALFORMED;
    tgt = sp1 + 1;
    tlen = len - mlen - 1;
    sp2 = (const char *)memchr(tgt, ' ', tlen);
    if (!sp2)
        return HTRW_LINE_MALFORMED;
    ver = sp2 + 1;
    if ((size_t)(line + len - ver) < 8 || strncmp(ver, "HTTP/1.", 7) != 0 ||
        (ver[7] != '0' && ver[7] != '1'))
        return HTRW_LINE_MALFORMED;
    *ver10 = (ver[7] == '0');

    memcpy(method, line, mlen);
    method[mlen] = '\0';
    tlen = (size_t)(sp2 - tgt);

    if (_stricmp(method, "CONNECT") == 0) {
        if (tlen == 0 || tlen >= authcap)
            return HTRW_LINE_MALFORMED;
        memcpy(authority, tgt, tlen);
        authority[tlen] = '\0';
        return HTRW_LINE_CONNECT;
    }

    if (tlen > 7 && _strnicmp(tgt, "http://", 7) == 0) {
        if (parse_abs_authority(tgt + 7, tlen - 7, authority, authcap) != 0)
            return HTRW_LINE_MALFORMED;
        if (path) {
            /* reconstruct origin-form target */
            const char *end = tgt + tlen;
            const char *p = tgt + 7;
            while (p < end && *p != '/' && *p != '?' && *p != '#')
                p++;
            if (p == end) {
                if (pathcap < 2)
                    return HTRW_LINE_MALFORMED;
                strcpy(path, "/");
            } else {
                size_t plen = (size_t)(end - p);
                if (plen >= pathcap)
                    return HTRW_LINE_MALFORMED;
                memcpy(path, p, plen);
                path[plen] = '\0';
            }
        }
        return HTRW_LINE_HTTP_ABS;
    }

    if (tlen > 8 && _strnicmp(tgt, "https://", 8) == 0) {
        if (parse_abs_authority(tgt + 8, tlen - 8, authority, authcap) != 0)
            return HTRW_LINE_MALFORMED;
        return HTRW_LINE_HTTPS_ABS;
    }

    return HTRW_LINE_ORIGIN;
}

int htrw_find_host(const void *hdrs, size_t len, char *host, size_t cap)
{
    const char *p = (const char *)hdrs;
    const char *end = p + len;

    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (ll > 0 && p[ll - 1] == '\r')
            ll--;
        if (ll == 0)
            return -1; /* end of headers */
        if (hdr_is(p, ll, "Host")) {
            size_t vl;
            const char *v = hdr_value(p, ll, "Host", &vl);
            if (vl == 0 || vl >= cap)
                return -1;
            memcpy(host, v, vl);
            host[vl] = '\0';
            return 0;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    return -1;
}

void htrw_init(http_rewriter *r,
               int (*emit)(void *ctx, const void *data, size_t n),
               void *ectx)
{
    memset(r, 0, sizeof(*r));
    r->state = HTRW_ST_LINE;
    r->emit = emit;
    r->ectx = ectx;
}

/* does this method conventionally carry a body? */
static int method_carries_body(const char *m)
{
    return _stricmp(m, "POST") == 0 || _stricmp(m, "PUT") == 0 ||
           _stricmp(m, "PATCH") == 0;
}

static int on_request_line(http_rewriter *r, char *line, size_t len)
{
    char method[32], authority[HTRW_AUTH_MAX], path[2048];
    int kind, ver10;

    if (len == 0)
        return 0; /* tolerate stray CRLF between pipelined requests */

    kind = htrw_parse_request_line(line, len, method, sizeof(method),
                                   authority, sizeof(authority),
                                   path, sizeof(path), &ver10);
    r->requests++;

    if (kind == HTRW_LINE_MALFORMED || kind == HTRW_LINE_HTTPS_ABS ||
        kind == HTRW_LINE_CONNECT) {
        /* not something we can rewrite safely - pass bytes through
         * unchanged for the rest of this connection */
        RLOGW("htrw: request line kind %d, switching to passthrough", kind);
        r->state = HTRW_ST_TUNNEL;
        return emit_mem(r, line, len) == 0 && emit_str(r, "\r\n") == 0 ? 0 : -1;
    }

    r->ver10 = ver10;
    r->saw_host = 0;
    r->body_kind = 0;
    r->body_left = 0;
    r->body_may_follow_close = (ver10 && method_carries_body(method)) ? 1 : 0;
    strncpy_s(r->cur_authority, sizeof(r->cur_authority), authority,
              _TRUNCATE);

    if (kind == HTRW_LINE_HTTP_ABS) {
        char out[HTRW_MAX_LINE + 32];
        int n = _snprintf_s(out, sizeof(out), _TRUNCATE,
                            "%s %s HTTP/1.%c\r\n", method, path,
                            ver10 ? '0' : '1');
        if (n <= 0 || emit_mem(r, out, (size_t)n) != 0)
            return -1;
    } else {
        if (emit_mem(r, line, len) != 0 || emit_str(r, "\r\n") != 0)
            return -1;
    }
    r->state = HTRW_ST_HDRS;
    return 0;
}

static int on_header_line(http_rewriter *r, char *line, size_t len)
{
    if (len == 0) {
        /* end of header block */
        char tmp[HTRW_AUTH_MAX + 10];
        int n;

        if (!r->saw_host && r->cur_authority[0]) {
            n = _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "Host: %s\r\n",
                            r->cur_authority);
            if (n <= 0 || emit_mem(r, tmp, (size_t)n) != 0)
                return -1;
        }
        if (emit_str(r, "\r\n") != 0)
            return -1;

        if (r->body_kind == 2)
            r->state = HTRW_ST_CHUNK_SIZE;
        else if (r->body_kind == 1 && r->body_left > 0)
            r->state = HTRW_ST_BODY_CL;
        else if (r->ver10 && r->body_kind == 0 && r->body_may_follow_close)
            r->state = HTRW_ST_TUNNEL;
        else
            r->state = HTRW_ST_LINE;
        return 0;
    }

    if (hdr_is(line, len, "Proxy-Connection") ||
        hdr_is(line, len, "Proxy-Authorization"))
        return 0; /* hop-by-hop, stripped */

    if (hdr_is(line, len, "Host"))
        r->saw_host = 1;

    if (hdr_is(line, len, "Content-Length")) {
        size_t vl;
        const char *v = hdr_value(line, len, "Content-Length", &vl);
        char tmp[32];
        char *end;
        if (vl > 0 && vl < sizeof(tmp)) {
            memcpy(tmp, v, vl);
            tmp[vl] = '\0';
            r->body_left = _strtoui64(tmp, &end, 10);
            if (*end == '\0')
                r->body_kind = 1;
        }
    } else if (hdr_is(line, len, "Transfer-Encoding")) {
        size_t vl;
        const char *v = hdr_value(line, len, "Transfer-Encoding", &vl);
        if (vl >= 7 && _strnicmp(v, "chunked", 7) == 0)
            r->body_kind = 2;
    }

    return emit_mem(r, line, len) == 0 && emit_str(r, "\r\n") == 0 ? 0 : -1;
}

static int on_line(http_rewriter *r, char *line, size_t len)
{
    switch (r->state) {
    case HTRW_ST_LINE:
        return on_request_line(r, line, len);

    case HTRW_ST_HDRS:
        return on_header_line(r, line, len);

    case HTRW_ST_CHUNK_SIZE: {
        char *end;
        if (len == 0)
            return 0;
        r->chunk_left = _strtoui64(line, &end, 16);
        if (end == line)
            return -1;
        if (emit_mem(r, line, len) != 0 || emit_str(r, "\r\n") != 0)
            return -1;
        r->state = r->chunk_left ? HTRW_ST_CHUNK_DATA : HTRW_ST_CHUNK_TRAIL;
        return 0;
    }

    case HTRW_ST_CHUNK_CRLF:
        if (emit_mem(r, line, len) != 0 || emit_str(r, "\r\n") != 0)
            return -1;
        r->state = HTRW_ST_CHUNK_SIZE;
        return 0;

    case HTRW_ST_CHUNK_TRAIL:
        if (emit_mem(r, line, len) != 0 || emit_str(r, "\r\n") != 0)
            return -1;
        if (len == 0)
            r->state = HTRW_ST_LINE;
        return 0;

    default:
        return -1;
    }
}

int htrw_feed(http_rewriter *r, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;

    while (n > 0) {
        if (r->state == HTRW_ST_TUNNEL) {
            if (emit_mem(r, p, n) != 0)
                return -1;
            return 0;
        }
        if (r->state == HTRW_ST_BODY_CL) {
            size_t take = n < r->body_left ? n : (size_t)r->body_left;
            if (emit_mem(r, p, take) != 0)
                return -1;
            p += take;
            n -= take;
            r->body_left -= take;
            if (r->body_left == 0)
                r->state = HTRW_ST_LINE;
            continue;
        }
        if (r->state == HTRW_ST_CHUNK_DATA) {
            size_t take = n < r->chunk_left ? n : (size_t)r->chunk_left;
            if (emit_mem(r, p, take) != 0)
                return -1;
            p += take;
            n -= take;
            r->chunk_left -= take;
            if (r->chunk_left == 0)
                r->state = HTRW_ST_CHUNK_CRLF;
            continue;
        }

        /* line-oriented states */
        {
            const uint8_t *nl = (const uint8_t *)memchr(p, '\n', n);
            size_t avail = nl ? (size_t)(nl - p) : n;
            size_t llen;
            char *line;

            if (r->line_len + avail > HTRW_MAX_LINE)
                return -1;
            memcpy(r->line + r->line_len, p, avail);
            r->line_len += avail;
            p += avail;
            n -= avail;

            if (!nl)
                continue; /* need more bytes */
            p++; /* consume '\n' */
            n--;

            llen = r->line_len;
            line = (char *)r->line;
            if (llen > 0 && line[llen - 1] == '\r')
                line[--llen] = '\0';
            else
                line[llen] = '\0';
            r->line_len = 0;

            if (on_line(r, line, llen) != 0)
                return -1;
        }
    }
    return 0;
}
