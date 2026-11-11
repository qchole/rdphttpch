/* Unit tests for src/srv/http_rewrite.c */
#include <stdio.h>
#include <string.h>

#include "http_rewrite.h"

static int g_fail;
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

/* output capture */
static char out[65536];
static size_t out_len;

static int emit(void *ctx, const void *data, size_t n)
{
    (void)ctx;
    if (out_len + n > sizeof(out))
        return -1;
    memcpy(out + out_len, data, n);
    out_len += n;
    return 0;
}

static http_rewriter rw;

static void rw_reset(void)
{
    out_len = 0;
    htrw_init(&rw, emit, NULL);
}

static void feed_str(const char *s)
{
    CHECK(htrw_feed(&rw, s, strlen(s)) == 0);
}

#define OUT_IS(lit) do { \
    if (out_len != strlen(lit) || memcmp(out, lit, strlen(lit)) != 0) { \
        printf("FAIL %s:%d: output mismatch\n  got:  \"%.*s\"\n  want: \"%s\"\n", \
               __FILE__, __LINE__, (int)out_len, out, lit); \
        g_fail++; \
    } \
} while (0)

static void test_absolute_uri(void)
{
    rw_reset();
    feed_str("GET http://example.com/a/b?q=1 HTTP/1.1\r\n"
             "User-Agent: curl/8\r\n"
             "Accept: */*\r\n"
             "\r\n");
    OUT_IS("GET /a/b?q=1 HTTP/1.1\r\n"
           "User-Agent: curl/8\r\n"
           "Accept: */*\r\n"
           "Host: example.com\r\n"
           "\r\n");
}

static void test_absolute_with_port_and_proxy_headers(void)
{
    rw_reset();
    feed_str("POST http://example.com:8080/submit HTTP/1.1\r\n"
             "Proxy-Connection: keep-alive\r\n"
             "Host: example.com:8080\r\n"
             "Content-Length: 7\r\n"
             "\r\n"
             "a=b&c=d");
    OUT_IS("POST /submit HTTP/1.1\r\n"
           "Host: example.com:8080\r\n"
           "Content-Length: 7\r\n"
           "\r\n"
           "a=b&c=d");
    CHECK(rw.state == HTRW_ST_LINE);
    CHECK(rw.requests == 1);
}

static void test_keepalive_pipeline(void)
{
    rw_reset();
    feed_str("GET http://a.com/1 HTTP/1.1\r\nHost: a.com\r\n\r\n"
             "GET http://a.com/2 HTTP/1.1\r\nHost: a.com\r\n\r\n");
    OUT_IS("GET /1 HTTP/1.1\r\nHost: a.com\r\n\r\n"
           "GET /2 HTTP/1.1\r\nHost: a.com\r\n\r\n");
    CHECK(rw.requests == 2);
}

static void test_chunked_body(void)
{
    rw_reset();
    feed_str("POST http://a.com/c HTTP/1.1\r\n"
             "Transfer-Encoding: chunked\r\n"
             "\r\n"
             "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n"
             "GET /next HTTP/1.1\r\nHost: a.com\r\n\r\n");
    OUT_IS("POST /c HTTP/1.1\r\n"
           "Transfer-Encoding: chunked\r\n"
           "Host: a.com\r\n"
           "\r\n"
           "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n"
           "GET /next HTTP/1.1\r\nHost: a.com\r\n\r\n");
    CHECK(rw.requests == 2);
}

static void test_origin_form_passthrough(void)
{
    rw_reset();
    feed_str("GET /plain HTTP/1.1\r\nHost: b.com\r\nX-Y: 1\r\n\r\n");
    OUT_IS("GET /plain HTTP/1.1\r\nHost: b.com\r\nX-Y: 1\r\n\r\n");
}

static void test_fragmented_feed(void)
{
    /* byte-at-a-time input; the incomplete next request line stays
     * buffered until its LF arrives (keep-alive framing) */
    const char *req = "GET http://x.io/p HTTP/1.1\r\nHost: x.io\r\n\r\n"
                      "GET http://x.io/q HT";
    size_t i;

    rw_reset();
    for (i = 0; i < strlen(req); i++)
        CHECK(htrw_feed(&rw, req + i, 1) == 0);
    OUT_IS("GET /p HTTP/1.1\r\nHost: x.io\r\n\r\n");
    CHECK(rw.line_len == strlen("GET http://x.io/q HT"));
}

static void test_userinfo_stripped(void)
{
    char method[32], auth[HTRW_AUTH_MAX], path[2048];
    int ver10;
    const char *l;

    l = "GET http://user:pw@example.com/ HTTP/1.1";
    CHECK(htrw_parse_request_line(l, strlen(l), method, sizeof(method), auth,
                                  sizeof(auth), path, sizeof(path),
                                  &ver10) == HTRW_LINE_HTTP_ABS);
    CHECK(strcmp(auth, "example.com") == 0);
    CHECK(strcmp(path, "/") == 0);
    CHECK(ver10 == 0);

    l = "CONNECT example.com:443 HTTP/1.1";
    CHECK(htrw_parse_request_line(l, strlen(l), method, sizeof(method), auth,
                                  sizeof(auth), NULL, 0,
                                  &ver10) == HTRW_LINE_CONNECT);
    CHECK(strcmp(auth, "example.com:443") == 0);

    l = "GET https://example.com/ HTTP/1.1";
    CHECK(htrw_parse_request_line(l, strlen(l), method, sizeof(method), auth,
                                  sizeof(auth), path, sizeof(path),
                                  &ver10) == HTRW_LINE_HTTPS_ABS);
    CHECK(strcmp(auth, "example.com") == 0);

    l = "GET * HTTP/1.0";
    CHECK(htrw_parse_request_line(l, strlen(l), method, sizeof(method), auth,
                                  sizeof(auth), NULL, 0,
                                  &ver10) == HTRW_LINE_ORIGIN);
    CHECK(ver10 == 1);

    l = "GARBAGE";
    CHECK(htrw_parse_request_line(l, strlen(l), method, sizeof(method), auth,
                                  sizeof(auth), NULL, 0,
                                  &ver10) == HTRW_LINE_MALFORMED);
}

static void test_find_host(void)
{
    char host[128];
    const char *hdrs = "User-Agent: x\r\nHost:  example.org:81\r\nAccept: z\r\n\r\n";

    CHECK(htrw_find_host(hdrs, strlen(hdrs), host, sizeof(host)) == 0);
    CHECK(strcmp(host, "example.org:81") == 0);
    CHECK(htrw_find_host("Accept: z\r\n\r\n", 14, host, sizeof(host)) == -1);
}

static void test_line_too_long(void)
{
    char big[HTRW_MAX_LINE + 10];
    size_t i;

    for (i = 0; i < sizeof(big) - 1; i++)
        big[i] = 'a';
    big[sizeof(big) - 1] = '\0';

    rw_reset();
    CHECK(htrw_feed(&rw, big, strlen(big)) == -1);
}

static void test_absolute_https_tunnel(void)
{
    /* mid-connection https:// absolute-form falls back to passthrough
     * after the already-rewritten bytes */
    rw_reset();
    feed_str("GET http://a.com/1 HTTP/1.1\r\nHost: a.com\r\n\r\n");
    CHECK(out_len > 0);
    out_len = 0;
    feed_str("GET https://a.com/2 HTTP/1.1\r\nHost: a.com\r\n\r\nREST");
    OUT_IS("GET https://a.com/2 HTTP/1.1\r\nHost: a.com\r\n\r\nREST");
    CHECK(rw.state == HTRW_ST_TUNNEL);
}

static void test_http10_implicit_body(void)
{
    rw_reset();
    feed_str("POST http://a.com/up HTTP/1.0\r\nHost: a.com\r\n\r\n"
             "implicit-body-bytes");
    OUT_IS("POST /up HTTP/1.0\r\nHost: a.com\r\n\r\n"
           "implicit-body-bytes");
    CHECK(rw.state == HTRW_ST_TUNNEL);
}

int main(void)
{
    test_absolute_uri();
    test_absolute_with_port_and_proxy_headers();
    test_keepalive_pipeline();
    test_chunked_body();
    test_origin_form_passthrough();
    test_fragmented_feed();
    test_userinfo_stripped();
    test_find_host();
    test_line_too_long();
    test_absolute_https_tunnel();
    test_http10_implicit_body();

    if (g_fail) {
        printf("test_http_rewrite: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_http_rewrite: all passed\n");
    return 0;
}
