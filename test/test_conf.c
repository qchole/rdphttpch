/* Unit tests for src/srv/conf.c */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "conf.h"

static int g_fail;
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

/* parse json; if want_err, expect -1, else expect 0 with
 * cfg.bypass == want_bypass and cfg.log_level == want_level */
static void t(const char *json, const char *want_bypass, int want_level,
              int want_err)
{
    rdpch_conf cfg;
    int rc = conf_parse(json, &cfg);

    if (want_err) {
        CHECK(rc == -1);
        return;
    }
    CHECK(rc == 0);
    if (rc == 0) {
        if (strcmp(cfg.bypass, want_bypass) != 0) {
            printf("FAIL: json %s\n  bypass got:  \"%s\"\n"
                   "  bypass want: \"%s\"\n",
                   json, cfg.bypass, want_bypass);
            g_fail++;
        }
        if (cfg.log_level != want_level) {
            printf("FAIL: json %s\n  log_level got %d want %d\n",
                   json, cfg.log_level, want_level);
            g_fail++;
        }
    }
}

static void test_bypass(void)
{
    t("{\"bypass\":[\"192.168.*\"]}", "192.168.*", -1, 0);
    t("{\n  \"bypass\": [\n    \"192.168.*\",\n    \"10.*\"\n  ]\n}\n",
      "192.168.*;10.*", -1, 0);
    t("{\"bypass\":\"a,b\"}", "a,b", -1, 0); /* string form */
    t("{\"bypass\":\"x;y\"}", "x;y", -1, 0);
    t("{\"bypass\": []}", "", -1, 0);
    t("{\"other\": 1}", "", -1, 0); /* no bypass key */
    t("{}", "", -1, 0);
    t("{\"a\":{\"b\":[1,2,{\"c\":null}]},\"bypass\":[\"x\"],\"d\":true}",
      "x", -1, 0); /* unknown keys with complex values skipped */
    t("{\"bypass\":[\"\",\"a\"]}", "a", -1, 0); /* empty entries skipped */
    t("{\"bypass\":[\"a\"],\"bypass\":[\"b\"]}", "b", -1, 0); /* last wins */

    /* escapes */
    t("{\"bypass\":[\"a\\\"b\"]}", "a\"b", -1, 0);
    t("{\"bypass\":[\"a\\\\b\"]}", "a\\b", -1, 0);
    t("{\"bypass\":[\"\\u0041\"]}", "A", -1, 0);
    {
        rdpch_conf cfg;
        char json[64];
        strcpy(json, "{\"bypass\":[\"test\\ud83d\\ude00\"]}");
        CHECK(conf_parse(json, &cfg) == 0);
        CHECK(memcmp(cfg.bypass, "test\xF0\x9F\x98\x80", 8) == 0);
    }
    /* raw UTF-8 bytes pass through unchanged */
    {
        rdpch_conf cfg;
        char json[64];
        strcpy(json, "{\"bypass\":\"\xC3\xA4\xC3\xA9\"}");
        CHECK(conf_parse(json, &cfg) == 0);
        CHECK(memcmp(cfg.bypass, "\xC3\xA4\xC3\xA9", 4) == 0);
    }
}

static void test_log_level(void)
{
    t("{\"log_level\":\"debug\"}", "", RLOG_DBG, 0);
    t("{\"log_level\":\"info\"}", "", RLOG_INFO, 0);
    t("{\"log_level\":\"warn\"}", "", RLOG_WARN, 0);
    t("{\"log_level\":\"error\"}", "", RLOG_ERR, 0);
    t("{\"log_level\":\"WARN\"}", "", RLOG_WARN, 0); /* case-insensitive */
    t("{\"bypass\":[\"10.*\"],\"log_level\":\"warn\"}", "10.*",
      RLOG_WARN, 0);
    t("{\"log_level\":\"verbose\"}", NULL, 0, 1); /* unknown name */
    t("{\"log_level\":1}", NULL, 0, 1);           /* wrong type */
    t("{\"log_level\":null}", NULL, 0, 1);
}

static void test_errors(void)
{
    t("", NULL, 0, 1);
    t("{", NULL, 0, 1);
    t("[1,2]", NULL, 0, 1);               /* not an object */
    t("\"x\"", NULL, 0, 1);
    t("{\"bypass\":[\"a\",]}", NULL, 0, 1); /* trailing comma */
    t("{\"bypass\":[\"a\"},", NULL, 0, 1); /* bracket mismatch */
    t("{\"bypass\":[1]}", NULL, 0, 1);     /* non-string element */
    t("{bypass:[]}", NULL, 0, 1);          /* unquoted key */
    t("{\"bypass\":5}", NULL, 0, 1);       /* wrong value type */
    t("{\"bypass\":true}", NULL, 0, 1);
    t("{} trailing", NULL, 0, 1);          /* garbage after object */
    t("{\"bypass\":\"\\q\"}", NULL, 0, 1); /* bad escape */
    t("{\"bypass\":\"\\u00zz\"}", NULL, 0, 1);
    t("{\"bypass\":\"\\ud800\"}", NULL, 0, 1); /* unpaired high surrogate */
    t("{\"bypass\":\"\\udc00\"}", NULL, 0, 1); /* stray low surrogate */
    t("{\"bypass\":\"a\nb\"}", NULL, 0, 1);    /* raw control char */
    t("{\"other\":1,}", NULL, 0, 1);           /* trailing comma in object */
    {
        /* entry longer than the 1023-byte cap */
        rdpch_conf cfg;
        char json[1200];
        int i, n;
        n = sprintf(json, "{\"bypass\":[\"");
        for (i = 0; i < 1100; i++)
            json[n++] = 'x';
        strcpy(json + n, "\"]}");
        CHECK(conf_parse(json, &cfg) == -1);
    }
}

static void test_file(void)
{
    wchar_t path[MAX_PATH + 32], tmp[MAX_PATH];
    rdpch_conf cfg;
    FILE *f;

    CHECK(GetTempPathW(MAX_PATH, tmp) > 0);
    wcscpy(path, tmp);
    wcscat(path, L"rdpch_conf_test.json");

    DeleteFileW(path);
    CHECK(conf_load(path, &cfg) == 1); /* missing */
    CHECK(cfg.bypass[0] == 0 && cfg.log_level == -1);

    f = _wfopen(path, L"wb");
    CHECK(f != NULL);
    if (f) {
        fputs("\xEF\xBB\xBF{\"bypass\":[\"10.*\",\"172.16.*\"],"
              "\"log_level\":\"warn\"}", f);
        fclose(f);
    }
    CHECK(conf_load(path, &cfg) == 0); /* with BOM */
    CHECK(strcmp(cfg.bypass, "10.*;172.16.*") == 0);
    CHECK(cfg.log_level == RLOG_WARN);

    f = _wfopen(path, L"wb");
    CHECK(f != NULL);
    if (f) {
        fputs("{\"bypass\":[", f); /* truncated */
        fclose(f);
    }
    CHECK(conf_load(path, &cfg) == -1);

    DeleteFileW(path);
}

int main(void)
{
    test_bypass();
    test_log_level();
    test_errors();
    test_file();
    if (g_fail) {
        printf("%d FAILED\n", g_fail);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
