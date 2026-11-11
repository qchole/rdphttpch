#include "conf.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

/* ---- tiny JSON scanner ----
 *
 * Just enough to walk a top-level object, pull out "bypass", and
 * skip everything else correctly (strings, numbers, literals,
 * nested objects/arrays).  Strict: no trailing commas, no comments,
 * strings must have valid escapes. */

typedef struct {
    const char *p;
} jp;

#define JS_DEPTH_MAX 64

static void js_ws(jp *j)
{
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' ||
           *j->p == '\n')
        j->p++;
}

static int js_hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    int i;

    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else
            return -1;
    }
    *out = v;
    return 0;
}

/* append one code point as UTF-8; out==NULL only validates */
static int js_put_utf8(char *out, size_t cap, size_t *len, unsigned cp)
{
    char tmp[4];
    int n, i;

    if (cp < 0x80) {
        tmp[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else if (cp <= 0x10FFFF) {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    } else {
        return -1;
    }
    if (out) {
        if (*len + (size_t)n >= cap)
            return -1; /* would not fit */
        for (i = 0; i < n; i++)
            out[*len + (size_t)i] = tmp[i];
    }
    *len += (size_t)n;
    return 0;
}

/* parse a JSON string at j->p (must point at '"').  Decoded bytes go
 * to out (cap incl. NUL; out==NULL only validates).  Moves j->p past
 * the closing quote.  0 = ok, -1 = malformed or too long. */
static int js_string(jp *j, char *out, size_t cap)
{
    size_t len = 0;

    j->p++; /* opening quote */
    for (;;) {
        unsigned char c = (unsigned char)*j->p;

        if (c == '"') {
            j->p++;
            if (out) {
                if (len + 1 >= cap)
                    return -1;
                out[len] = 0;
            }
            return 0;
        }
        if (c == 0 || c < 0x20)
            return -1; /* raw control char or EOF */
        if (c != '\\') {
            /* raw byte: file bytes pass through verbatim (UTF-8 in,
             * UTF-8 out; \uXXXX escapes are encoded by the code below) */
            if (out) {
                if (len + 1 >= cap)
                    return -1;
                out[len] = (char)c;
            }
            len++;
            j->p++;
            continue;
        }
        /* escape */
        j->p++;
        switch (*j->p) {
        case '"':  case '\\': case '/':
        case 'b':  case 'f':  case 'n': case 'r': case 't': {
            static const char esc[] = { '"', '\\', '/', '\b', '\f',
                                        '\n', '\r', '\t' };
            static const char *lit = "\"\\/bfnrt";
            const char *hit = strchr(lit, *j->p);
            char decoded = esc[hit - lit];
            if (out && js_put_utf8(out, cap, &len,
                                   (unsigned char)decoded) != 0)
                return -1;
            else if (!out)
                len++;
            j->p++;
            break;
        }
        case 'u': {
            unsigned cp;
            if (js_hex4(j->p + 1, &cp) != 0)
                return -1;
            j->p += 5;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                /* high surrogate: must pair with a low one */
                unsigned lo;
                if (j->p[0] != '\\' || j->p[1] != 'u' ||
                    js_hex4(j->p + 2, &lo) != 0 || lo < 0xDC00 ||
                    lo > 0xDFFF)
                    return -1;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                j->p += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return -1; /* stray low surrogate */
            }
            if (out && js_put_utf8(out, cap, &len, cp) != 0)
                return -1;
            else if (!out)
                len += 4;
            break;
        }
        default:
            return -1;
        }
    }
}

static int js_skip_value(jp *j, int depth);

static int js_skip_number(jp *j)
{
    int seen = 0;

    if (*j->p == '-')
        j->p++;
    while (*j->p >= '0' && *j->p <= '9') {
        j->p++;
        seen = 1;
    }
    if (!seen)
        return -1;
    if (*j->p == '.') {
        j->p++;
        seen = 0;
        while (*j->p >= '0' && *j->p <= '9') {
            j->p++;
            seen = 1;
        }
        if (!seen)
            return -1;
    }
    if (*j->p == 'e' || *j->p == 'E') {
        j->p++;
        if (*j->p == '+' || *j->p == '-')
            j->p++;
        seen = 0;
        while (*j->p >= '0' && *j->p <= '9') {
            j->p++;
            seen = 1;
        }
        if (!seen)
            return -1;
    }
    return 0;
}

static int js_skip_value(jp *j, int depth)
{
    if (depth > JS_DEPTH_MAX)
        return -1;

    switch (*j->p) {
    case '"':
        return js_string(j, NULL, 0);
    case '{': {
        j->p++;
        js_ws(j);
        if (*j->p == '}')
            goto obj_done;
        for (;;) {
            js_ws(j);
            if (*j->p != '"' || js_string(j, NULL, 0) != 0)
                return -1;
            js_ws(j);
            if (*j->p != ':')
                return -1;
            j->p++;
            js_ws(j);
            if (js_skip_value(j, depth + 1) != 0)
                return -1;
            js_ws(j);
            if (*j->p == ',') {
                j->p++;
                continue;
            }
            if (*j->p == '}')
                break;
            return -1;
        }
    obj_done:
        j->p++;
        return 0;
    }
    case '[': {
        j->p++;
        js_ws(j);
        if (*j->p == ']')
            goto arr_done;
        for (;;) {
            js_ws(j);
            if (js_skip_value(j, depth + 1) != 0)
                return -1;
            js_ws(j);
            if (*j->p == ',') {
                j->p++;
                continue;
            }
            if (*j->p == ']')
                break;
            return -1;
        }
    arr_done:
        j->p++;
        return 0;
    }
    case 't':
        if (strncmp(j->p, "true", 4) != 0)
            return -1;
        j->p += 4;
        return 0;
    case 'f':
        if (strncmp(j->p, "false", 5) != 0)
            return -1;
        j->p += 5;
        return 0;
    case 'n':
        if (strncmp(j->p, "null", 4) != 0)
            return -1;
        j->p += 4;
        return 0;
    default:
        return js_skip_number(j);
    }
}

/* ---- "bypass" extraction ---- */

/* append one entry to the ";"-joined list in out */
static int lst_add(char *out, size_t cap, const char *entry)
{
    size_t el = strlen(entry), ol;

    if (!el)
        return 0; /* empty entries are skipped, not fatal */
    ol = strlen(out);
    if (ol + (ol ? 1 : 0) + el + 1 > cap)
        return -1;
    if (ol) {
        out[ol++] = ';';
        memcpy(out + ol, entry, el);
        out[ol + el] = 0;
    } else {
        memcpy(out, entry, el + 1);
    }
    return 0;
}

/* value must be an array of strings or a single string; fills out */
static int parse_bypass_value(jp *j, char *out, size_t cap)
{
    char entry[1024];

    out[0] = 0; /* a duplicate key overwrites the earlier one */
    js_ws(j);
    if (*j->p == '"') {
        if (js_string(j, entry, sizeof(entry)) != 0)
            return -1;
        return lst_add(out, cap, entry);
    }
    if (*j->p != '[')
        return -1;
    j->p++;
    js_ws(j);
    if (*j->p == ']') {
        j->p++;
        return 0;
    }
    for (;;) {
        js_ws(j);
        if (*j->p != '"' || js_string(j, entry, sizeof(entry)) != 0)
            return -1; /* elements must be strings */
        if (lst_add(out, cap, entry) != 0) {
            RLOGW("conf: bypass list too long, entry '%s' dropped",
                  entry);
        }
        js_ws(j);
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        if (*j->p == ']') {
            j->p++;
            return 0;
        }
        return -1;
    }
}

/* value must be one of the rlog level names */
static int parse_log_level_value(jp *j, int *out)
{
    char name[32];

    js_ws(j);
    if (*j->p != '"' || js_string(j, name, sizeof(name)) != 0)
        return -1;
    if (_stricmp(name, "debug") == 0)
        *out = RLOG_DBG;
    else if (_stricmp(name, "info") == 0)
        *out = RLOG_INFO;
    else if (_stricmp(name, "warn") == 0)
        *out = RLOG_WARN;
    else if (_stricmp(name, "error") == 0)
        *out = RLOG_ERR;
    else {
        RLOGE("conf: unknown log_level '%s' (debug|info|warn|error)",
              name);
        return -1;
    }
    return 0;
}

void conf_default(rdpch_conf *cfg)
{
    cfg->bypass[0] = 0;
    cfg->log_level = -1;
}

int conf_parse(const char *text, rdpch_conf *cfg)
{
    jp j = { text };
    char key[256];

    conf_default(cfg);
    js_ws(&j);
    if (*j.p != '{')
        return -1;
    j.p++;
    js_ws(&j);
    if (*j.p == '}') {
        j.p++;
    } else {
        for (;;) {
            js_ws(&j);
            if (*j.p != '"' || js_string(&j, key, sizeof(key)) != 0)
                return -1;
            js_ws(&j);
            if (*j.p != ':')
                return -1;
            j.p++;
            js_ws(&j);
            if (strcmp(key, "bypass") == 0) {
                if (parse_bypass_value(&j, cfg->bypass,
                                       sizeof(cfg->bypass)) != 0)
                    return -1;
            } else if (strcmp(key, "log_level") == 0) {
                if (parse_log_level_value(&j, &cfg->log_level) != 0)
                    return -1;
            } else {
                if (js_skip_value(&j, 1) != 0)
                    return -1;
            }
            js_ws(&j);
            if (*j.p == ',') {
                j.p++;
                continue;
            }
            if (*j.p == '}') {
                j.p++;
                break;
            }
            return -1;
        }
    }
    js_ws(&j);
    if (*j.p != 0)
        return -1; /* trailing garbage */
    return 0;
}

int conf_load(const wchar_t *path, rdpch_conf *cfg)
{
    HANDLE f;
    char *buf;
    DWORD size, got = 0;
    int rc;

    conf_default(cfg);
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            return 1;
        RLOGE("conf: cannot open config file (%lu)", (unsigned long)e);
        return -1;
    }
    size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size > 1024 * 1024) {
        RLOGE("conf: bad config file size");
        CloseHandle(f);
        return -1;
    }
    buf = (char *)malloc(size + 1); /* +1: parser needs NUL termination */
    if (!buf) {
        CloseHandle(f);
        return -1;
    }
    if (!ReadFile(f, buf, size, &got, NULL) || got != size) {
        RLOGE("conf: config read failed");
        free(buf);
        CloseHandle(f);
        return -1;
    }
    buf[size] = 0;
    CloseHandle(f);

    if (size >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
        rc = conf_parse(buf + 3, cfg);
    else
        rc = conf_parse(buf, cfg);
    if (rc != 0)
        RLOGE("conf: malformed JSON config");
    free(buf);
    return rc;
}
