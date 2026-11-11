#include "wininet_cfg.h"

#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

#define IS_KEY  L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings"
#define BK_KEY  L"Software\\rdpch\\proxy-backup"

/* WinINet caps ProxyOverride at ~2064 chars; stay just under it */
#define OV_MAX 2048

static const char *LOCAL_OVERRIDE = "localhost;127.*;<local>";

static LONG open_key(HKEY root, LPCWSTR sub, REGSAM access, HKEY *out)
{
    return RegCreateKeyExW(root, sub, 0, NULL, 0, access, NULL, out,
                           NULL);
}

static int read_dword(HKEY k, LPCWSTR name, DWORD *out)
{
    DWORD type = 0, size = sizeof(*out);

    if (RegQueryValueExW(k, name, NULL, &type, (LPBYTE)out, &size)
            != ERROR_SUCCESS ||
        (type != REG_DWORD && type != REG_DWORD_LITTLE_ENDIAN))
        return -1;
    return 0;
}

/* returns 0 = value exists (copied), 1 = missing, -1 = error */
static int read_str(HKEY k, LPCWSTR name, char *out, size_t cap)
{
    DWORD type = 0;
    wchar_t wbuf[OV_MAX + 2];
    DWORD size = sizeof(wbuf) - sizeof(wchar_t);

    if (RegQueryValueExW(k, name, NULL, &type, (LPBYTE)wbuf, &size)
            != ERROR_SUCCESS)
        return 1;
    if (type != REG_SZ)
        return -1;
    wbuf[size / sizeof(wchar_t)] = 0;
    if (WideCharToMultiByte(CP_ACP, 0, wbuf, -1, out, (int)cap,
                            NULL, NULL) == 0)
        return -1;
    return 0;
}

static int write_dword(HKEY k, LPCWSTR name, DWORD v)
{
    return RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v,
                          sizeof(v)) == ERROR_SUCCESS ? 0 : -1;
}

static int write_str(HKEY k, LPCWSTR name, const char *v)
{
    wchar_t wbuf[OV_MAX + 2];

    if (MultiByteToWideChar(CP_ACP, 0, v, -1, wbuf,
                            (int)(sizeof(wbuf) / sizeof(wchar_t))) == 0)
        return -1;
    return RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)wbuf,
                          (DWORD)((wcslen(wbuf) + 1) * sizeof(wchar_t)))
               == ERROR_SUCCESS ? 0 : -1;
}

static int delete_value(HKEY k, LPCWSTR name)
{
    RegDeleteValueW(k, name);
    return 0;
}

/* Append every ";"/","-separated entry of "extra" to the ";"
 * separated list in ov (cap bytes), skipping entries already present
 * (case-insensitive, whole-entry compare) and entries that no longer
 * fit.  Returns the number of entries appended. */
static int append_entries(char *ov, size_t cap, const char *extra)
{
    int appended = 0;
    const char *p = extra;

    if (!extra)
        return 0;
    while (*p) {
        char raw[256];
        size_t rl = 0, s, e, tl, ol, i;
        int trunc = 0;

        while (*p && *p != ';' && *p != ',') {
            if (rl < sizeof(raw) - 1)
                raw[rl++] = *p;
            else
                trunc = 1;
            p++;
        }
        if (*p)
            p++; /* skip separator */
        s = 0;
        e = rl;
        while (s < e && (raw[s] == ' ' || raw[s] == '\t'))
            s++;
        while (e > s && (raw[e - 1] == ' ' || raw[e - 1] == '\t'))
            e--;
        tl = e - s;
        if (trunc) {
            RLOGW("setproxy: bypass entry too long, ignored");
            continue;
        }
        if (!tl)
            continue;

        ol = strlen(ov);
        for (i = 0; i <= ol;) {
            size_t j = i, ss, se;
            while (ov[j] != 0 && ov[j] != ';')
                j++;
            ss = i;
            se = j;
            while (ss < se && (ov[ss] == ' ' || ov[ss] == '\t'))
                ss++;
            while (se > ss && (ov[se - 1] == ' ' || ov[se - 1] == '\t'))
                se--;
            if (se - ss == tl && _strnicmp(ov + ss, raw + s, tl) == 0)
                break; /* already present */
            i = j + 1;
        }
        if (i <= ol)
            continue; /* found above */

        if (ol + (ol ? 1 : 0) + tl + 1 > cap) {
            RLOGW("setproxy: bypass list full, dropping '%.*s'",
                  (int)tl, raw + s);
            continue;
        }
        if (ol) {
            ov[ol++] = ';';
            memcpy(ov + ol, raw + s, tl);
            ov[ol + tl] = 0;
        } else {
            memcpy(ov, raw + s, tl);
            ov[tl] = 0;
        }
        appended++;
    }
    return appended;
}

/* make WinINet pick up the changed settings */
static void refresh(void)
{
    InternetSetOptionA(NULL, INTERNET_OPTION_SETTINGS_CHANGED, NULL, 0);
    InternetSetOptionA(NULL, INTERNET_OPTION_REFRESH, NULL, 0);
}

int rdpch_proxy_enable(uint16_t port, const char *bypass)
{
    HKEY is, bk = NULL;
    char server[256];
    char override[OV_MAX];
    char merged[OV_MAX];
    DWORD enable = 0;
    int have_server, have_override;
    LONG rc;

    rc = open_key(HKEY_CURRENT_USER, IS_KEY, KEY_READ | KEY_WRITE, &is);
    if (rc != ERROR_SUCCESS) {
        RLOGE("setproxy: cannot open Internet Settings (%ld)", rc);
        return -1;
    }

    /* one-time backup: only if the backup key does not exist yet, so
     * repeated --setproxy calls never clobber the original values */
    rc = open_key(HKEY_CURRENT_USER, BK_KEY, KEY_READ | KEY_WRITE, &bk);
    if (rc != ERROR_SUCCESS) {
        RegCloseKey(is);
        RLOGE("setproxy: cannot open backup key (%ld)", rc);
        return -1;
    }
    {
        DWORD probe = 0;
        if (read_dword(bk, L"BackedUp", &probe) != 0) {
            read_dword(is, L"ProxyEnable", &enable);
            have_server = read_str(is, L"ProxyServer", server,
                                   sizeof(server));
            have_override = read_str(is, L"ProxyOverride", override,
                                     sizeof(override));
            if (have_server == -1 || have_override == -1) {
                RLOGE("setproxy: cannot read current settings");
                RegCloseKey(bk);
                RegCloseKey(is);
                return -1;
            }
            write_dword(bk, L"OldEnable", enable);
            write_dword(bk, L"HaveServer", have_server == 0 ? 1 : 0);
            write_dword(bk, L"HaveOverride", have_override == 0 ? 1 : 0);
            if (have_server == 0)
                write_str(bk, L"OldServer", server);
            if (have_override == 0)
                write_str(bk, L"OldOverride", override);
            write_dword(bk, L"BackedUp", 1);
            RLOGI("setproxy: backed up current settings (enable=%u)",
                  (unsigned)enable);
        }
    }

    snprintf(server, sizeof(server), "127.0.0.1:%u", (unsigned)port);

    /* Recompute ProxyOverride on every run from the backed-up
     * original: user entries first, then our local-only entries,
     * then the caller's --bypass list.  This way changing (or
     * dropping) --bypass takes effect on the next start instead of
     * accumulating leftovers from earlier runs. */
    merged[0] = 0;
    {
        DWORD have = 0;
        if (read_dword(bk, L"HaveOverride", &have) == 0 && have &&
            read_str(bk, L"OldOverride", override, sizeof(override)) == 0)
            snprintf(merged, sizeof(merged), "%s", override);
    }
    append_entries(merged, sizeof(merged), LOCAL_OVERRIDE);
    if (bypass && bypass[0])
        append_entries(merged, sizeof(merged), bypass);

    if (write_str(is, L"ProxyServer", server) != 0 ||
        write_str(is, L"ProxyOverride", merged) != 0 ||
        write_dword(is, L"ProxyEnable", 1) != 0) {
        RLOGE("setproxy: cannot write settings");
        RegCloseKey(bk);
        RegCloseKey(is);
        return -1;
    }

    RegCloseKey(bk);
    RegCloseKey(is);
    refresh();
    RLOGI("setproxy: system proxy -> %s (bypass: %s)", server, merged);
    return 0;
}

int rdpch_proxy_disable(void)
{
    HKEY is, bk = NULL;
    char buf[OV_MAX];
    DWORD v;
    int rc = 0;

    if (open_key(HKEY_CURRENT_USER, IS_KEY, KEY_READ | KEY_WRITE, &is)
            != ERROR_SUCCESS) {
        RLOGE("unsetproxy: cannot open Internet Settings");
        return -1;
    }

    if (RegOpenKeyExW(HKEY_CURRENT_USER, BK_KEY, 0,
                      KEY_READ | KEY_WRITE, &bk) != ERROR_SUCCESS) {
        /* no backup: just turn it off and drop our values */
        write_dword(is, L"ProxyEnable", 0);
        delete_value(is, L"ProxyServer");
        delete_value(is, L"ProxyOverride");
        RegCloseKey(is);
        refresh();
        RLOGI("unsetproxy: proxy disabled (no backup found)");
        return 0;
    }

    if (read_dword(bk, L"OldEnable", &v) == 0)
        write_dword(is, L"ProxyEnable", v);

    if (read_dword(bk, L"HaveServer", &v) == 0 && v) {
        if (read_str(bk, L"OldServer", buf, sizeof(buf)) == 0)
            write_str(is, L"ProxyServer", buf);
    } else {
        delete_value(is, L"ProxyServer");
    }

    if (read_dword(bk, L"HaveOverride", &v) == 0 && v) {
        if (read_str(bk, L"OldOverride", buf, sizeof(buf)) == 0)
            write_str(is, L"ProxyOverride", buf);
    } else {
        delete_value(is, L"ProxyOverride");
    }

    RegCloseKey(bk);
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\rdpch\\proxy-backup");

    RegCloseKey(is);
    refresh();
    RLOGI("unsetproxy: original settings restored");
    return rc;
}
