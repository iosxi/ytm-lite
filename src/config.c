/* ==================================================================
 * config.c - 設定(exe の隣の yum-lite.ini、または -ini で指定したもの)
 *
 *  cookie はアカウントの鍵そのものなので、平文では置かない。DPAPI(CryptProtectData)で
 *  暗号化して 16 進で書く。同じ Windows ユーザーでしか戻せない。
 *  レジストリには何も書かない。
 * ================================================================== */

#include "yum.h"
#include <dpapi.h>

#pragma comment(lib, "crypt32.lib")

#define SEC L"yum-lite"

Config g_cfg;

void cfg_init_path(const WCHAR *iniArg)
{
    WCHAR *slash;
    if (iniArg && *iniArg) {
        if (!GetFullPathNameW(iniArg, MAX_PATH, g_cfg.ini, NULL)) wcsncpy_s(g_cfg.ini, MAX_PATH, iniArg, _TRUNCATE);
    } else {
        GetModuleFileNameW(NULL, g_cfg.ini, MAX_PATH);
        slash = wcsrchr(g_cfg.ini, L'\\');
        if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_cfg.ini), L"yum-lite.ini");
    }
    wcscpy_s(g_cfg.dir, MAX_PATH, g_cfg.ini);
    slash = wcsrchr(g_cfg.dir, L'\\');
    if (slash) *slash = 0;
}

static int get_int(const WCHAR *key, int def)
{
    return (int)GetPrivateProfileIntW(SEC, key, def, g_cfg.ini);
}

static void get_str(const WCHAR *key, char *out, size_t cap)
{
    WCHAR w[2048];
    char *u;
    GetPrivateProfileStringW(SEC, key, L"", w, ARRAYSIZE(w), g_cfg.ini);
    u = W2U(w);
    if (u) { strncpy_s(out, cap, u, _TRUNCATE); free(u); }
}

/* ini が無ければ UTF-16 の BOM だけのファイルを作る(WritePrivateProfileStringW が Unicode で書くように) */
static BOOL ensure_ini(void)
{
    HANDLE f;
    DWORD  wr;
    if (GetFileAttributesW(g_cfg.ini) != INVALID_FILE_ATTRIBUTES) return TRUE;
    f = CreateFileW(g_cfg.ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    WriteFile(f, "\xFF\xFE", 2, &wr, NULL);
    CloseHandle(f);
    return TRUE;
}

static void put_str(const WCHAR *key, const char *val)
{
    WCHAR *w = U2W(val ? val : "");
    if (!ensure_ini()) { free(w); return; }   /* 書けない場所: 保存を諦めて、その回だけ効かせる */
    WritePrivateProfileStringW(SEC, key, val ? w : NULL, g_cfg.ini);
    free(w);
}

static void put_int(const WCHAR *key, int v)
{
    char s[32];
    _snprintf(s, sizeof(s), "%d", v);
    put_str(key, s);
}

static char *unprotect(const char *hex)
{
    size_t    n = strlen(hex) / 2, i;
    BYTE     *bin;
    DATA_BLOB in, out;
    char     *res = NULL;
    if (!n) return NULL;
    bin = (BYTE *)malloc(n);
    if (!bin) return NULL;
    for (i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) { free(bin); return NULL; }
        bin[i] = (BYTE)v;
    }
    in.pbData = bin;
    in.cbData = (DWORD)n;
    if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        res = U8NDup((const char *)out.pbData, out.cbData);
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
    }
    free(bin);
    return res;
}

static char *protect(const char *s)
{
    DATA_BLOB in, out;
    char     *hex = NULL;
    DWORD     i;
    in.pbData = (BYTE *)s;
    in.cbData = (DWORD)strlen(s);
    if (!CryptProtectData(&in, L"yum-lite", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return NULL;
    hex = (char *)malloc((size_t)out.cbData * 2 + 1);
    if (hex) {
        for (i = 0; i < out.cbData; i++) sprintf(hex + i * 2, "%02x", out.pbData[i]);
        hex[out.cbData * 2] = 0;
    }
    LocalFree(out.pbData);
    return hex;
}

void cfg_load(void)
{
    WCHAR *big;
    char   tmp[64];

    g_cfg.volume    = get_int(L"volume", 70);
    if (g_cfg.volume < 0 || g_cfg.volume > 100) g_cfg.volume = 70;
    g_cfg.shuffle   = get_int(L"shuffle", 0) != 0;
    g_cfg.repeat    = get_int(L"repeat", 0) != 0;
    g_cfg.x         = get_int(L"x", CW_USEDEFAULT);
    g_cfg.y         = get_int(L"y", CW_USEDEFAULT);
    g_cfg.w         = get_int(L"w", 0);
    g_cfg.h         = get_int(L"h", 0);
    g_cfg.maximized = get_int(L"maximized", 0);
    g_cfg.theme     = get_int(L"theme", 0);
    g_cfg.mediakeys = get_int(L"mediakeys", 1);
    g_cfg.debug     = get_int(L"debug", 0);
    get_str(L"authuser", g_cfg.authuser, sizeof(g_cfg.authuser));
    get_str(L"ua", g_cfg.ua, sizeof(g_cfg.ua));
    get_str(L"visitor", g_cfg.visitor, sizeof(g_cfg.visitor));
    get_str(L"guest", g_cfg.guest, sizeof(g_cfg.guest));
    get_str(L"last_playlist", g_cfg.lastPlaylist, sizeof(g_cfg.lastPlaylist));
    get_str(L"cooldown_until", tmp, sizeof(tmp));
    g_cfg.cooldownUntil = _atoi64(tmp);

    /* cookie の暗号文は長い(数 KB の 2 倍)ので大きな器で読む */
    big = (WCHAR *)malloc(sizeof(WCHAR) * 65536);
    if (big) {
        GetPrivateProfileStringW(SEC, L"cookie", L"", big, 65536, g_cfg.ini);
        if (big[0]) {
            char *hex = W2U(big);
            if (hex) { g_cfg.cookie = unprotect(hex); free(hex); }
        }
        free(big);
    }
}

void cfg_save(void)
{
    put_int(L"volume", g_cfg.volume);
    put_int(L"shuffle", g_cfg.shuffle);
    put_int(L"repeat", g_cfg.repeat);
    if (g_cfg.w > 0 && g_cfg.h > 0) {
        put_int(L"x", g_cfg.x);
        put_int(L"y", g_cfg.y);
        put_int(L"w", g_cfg.w);
        put_int(L"h", g_cfg.h);
    }
    put_int(L"maximized", g_cfg.maximized);
    put_str(L"last_playlist", g_cfg.lastPlaylist);
}

/* 通信のスレッドが古い cookie を読んでいる最中かもしれないので、すぐには解放せず、
   次に入れ替えるときに解放する(そのころには使い終わっている) */
static char *g_retiredCookie;

void cfg_set_cookie(const char *cookie)
{
    if (g_retiredCookie) {
        SecureZeroMemory(g_retiredCookie, strlen(g_retiredCookie));
        free(g_retiredCookie);
    }
    g_retiredCookie = g_cfg.cookie;
    g_cfg.cookie = NULL;
    if (cookie && *cookie) {
        char *hex = protect(cookie);
        g_cfg.cookie = U8Dup(cookie);
        put_str(L"cookie", hex);
        free(hex);
        put_str(L"authuser", g_cfg.authuser);
        put_str(L"ua", g_cfg.ua);
    } else {
        put_str(L"cookie", NULL);
        put_str(L"authuser", NULL);
        put_str(L"ua", NULL);
        g_cfg.authuser[0] = 0;
        g_cfg.ua[0] = 0;
    }
}

void cfg_save_guest(void)
{
    put_str(L"visitor", g_cfg.visitor[0] ? g_cfg.visitor : NULL);
    put_str(L"guest", g_cfg.guest[0] ? g_cfg.guest : NULL);
}

void cfg_save_cooldown(void)
{
    char s[32];
    _snprintf(s, sizeof(s), "%lld", g_cfg.cooldownUntil);
    put_str(L"cooldown_until", g_cfg.cooldownUntil ? s : NULL);
}
