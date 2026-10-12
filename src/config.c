/* ==================================================================
 * config.c - 設定(exe の隣の ytm-lite.ini、または -ini で指定したもの)
 *
 *  cookie はアカウントの鍵そのものなので、平文では置かない。DPAPI(CryptProtectData)で
 *  暗号化して 16 進で書く。同じ Windows ユーザーでしか戻せない。
 *  レジストリには何も書かない。
 * ================================================================== */

#include "ytm.h"
#include <dpapi.h>

#pragma comment(lib, "crypt32.lib")

#define SEC L"ytm-lite"

Config g_cfg;

void cfg_init_path(const WCHAR *iniArg)
{
    WCHAR *slash;
    if (iniArg && *iniArg) {
        if (!GetFullPathNameW(iniArg, MAX_PATH, g_cfg.ini, NULL)) wcsncpy_s(g_cfg.ini, MAX_PATH, iniArg, _TRUNCATE);
    } else {
        GetModuleFileNameW(NULL, g_cfg.ini, MAX_PATH);
        slash = wcsrchr(g_cfg.ini, L'\\');
        if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_cfg.ini), L"ytm-lite.ini");
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
    if (!CryptProtectData(&in, L"ytm-lite", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return NULL;
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
    g_cfg.thumbnail = get_int(L"thumbnail", 1);
    g_cfg.debug     = get_int(L"debug", 0);
    get_str(L"authuser", g_cfg.authuser, sizeof(g_cfg.authuser));
    get_str(L"ua", g_cfg.ua, sizeof(g_cfg.ua));
    get_str(L"visitor", g_cfg.visitor, sizeof(g_cfg.visitor));
    get_str(L"guest", g_cfg.guest, sizeof(g_cfg.guest));
    get_str(L"last_playlist", g_cfg.lastPlaylist, sizeof(g_cfg.lastPlaylist));
    get_str(L"cooldown_until", tmp, sizeof(tmp));
    g_cfg.cooldownUntil = _atoi64(tmp);

    get_str(L"cookie_source", tmp, sizeof(tmp));
    if (!strcmp(tmp, "firefox")) {
        /* Firefox から読む: ini には cookie を置かず、起動のたびに読み直す
           (Firefox が cookie を入れ替えても、取り直しが要らない) */
        g_cfg.cookieSource = COOKIE_FIREFOX;
        GetPrivateProfileStringW(SEC, L"firefox_profile", L"", g_cfg.ffProfile, MAX_PATH, g_cfg.ini);
        cfg_reload_cookie(g_cfg.cookieErr, sizeof(g_cfg.cookieErr));
        return;
    }

    /* cookie の暗号文は長い(数 KB の 2 倍)ので大きな器で読む */
    big = (WCHAR *)malloc(sizeof(WCHAR) * 65536);
    if (big) {
        GetPrivateProfileStringW(SEC, L"cookie", L"", big, 65536, g_cfg.ini);
        if (big[0]) {
            char *hex = W2U(big);
            if (hex) { g_cfg.cookie = unprotect(hex); free(hex); }
            if (g_cfg.cookie) g_cfg.cookieSource = COOKIE_PASTED;
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

/* メモリの cookie を入れ替える(newCookie は引き取る) */
static void swap_cookie(char *newCookie)
{
    if (g_retiredCookie) {
        SecureZeroMemory(g_retiredCookie, strlen(g_retiredCookie));
        free(g_retiredCookie);
    }
    g_retiredCookie = g_cfg.cookie;
    g_cfg.cookie = newCookie;
}

void cfg_set_cookie(const char *cookie)
{
    swap_cookie(cookie && *cookie ? U8Dup(cookie) : NULL);
    put_str(L"cookie_source", NULL);
    put_str(L"firefox_profile", NULL);
    g_cfg.ffProfile[0] = 0;
    if (cookie && *cookie) {
        char *hex = protect(cookie);
        g_cfg.cookieSource = COOKIE_PASTED;
        put_str(L"cookie", hex);
        free(hex);
        put_str(L"authuser", g_cfg.authuser);
        put_str(L"ua", g_cfg.ua);
    } else {
        g_cfg.cookieSource = COOKIE_NONE;
        put_str(L"cookie", NULL);
        put_str(L"authuser", NULL);
        put_str(L"ua", NULL);
        g_cfg.authuser[0] = 0;
        g_cfg.ua[0] = 0;
    }
}

BOOL cfg_reload_cookie(char *err, size_t errlen)
{
    WCHAR prof[MAX_PATH];
    char *ck;
    int   n = 0;
    if (g_cfg.cookieSource != COOKIE_FIREFOX) { _snprintf(err, errlen, "Firefox から読む設定ではない"); return FALSE; }
    wcscpy_s(prof, MAX_PATH, g_cfg.ffProfile);
    if (!prof[0]) {
        if (!ff_find_profile(prof, MAX_PATH)) { _snprintf(err, errlen, "Firefox のプロファイルが見つからない"); return FALSE; }
    } else if (GetFileAttributesW(prof) == INVALID_FILE_ATTRIBUTES) {
        /* 覚えている場所が無い(Portable 版の USB を抜いているなど)。別のプロファイルへは切り替えない
           (別のアカウントを読んでしまうので) */
        char *u = W2U(prof);
        _snprintf(err, errlen, "Firefox のプロファイルが見つからない(%s)", u ? u : "");
        free(u);
        err[errlen - 1] = 0;
        return FALSE;
    }
    ck = ff_read_youtube_cookies(prof, &n, err, errlen);
    if (!ck) return FALSE;
    ff_user_agent(prof, g_cfg.ua, sizeof(g_cfg.ua));
    strcpy_s(g_cfg.authuser, sizeof(g_cfg.authuser), "0");
    swap_cookie(ck);
    log_printf("cookies from Firefox: %d", n);
    return TRUE;
}

/* profile が空なら候補(動いている Portable 版、いつものプロファイル、ほかのプロファイル)を順に読み、
   YouTube にログインしているものを使う。指定があれば、それ(Portable 版のフォルダや exe でもよい)だけ */
BOOL cfg_use_firefox(const WCHAR *profile, char *err, size_t errlen)
{
    WCHAR prof[MAX_PATH];
    char *ck = NULL, *p;
    int   n = 0;
    if (profile && *profile) {
        if (!ff_resolve_profile(profile, prof, MAX_PATH)) {
            _snprintf(err, errlen, "そのフォルダに Firefox のプロファイル(cookies.sqlite)が見つかりません");
            return FALSE;
        }
        ck = ff_read_youtube_cookies(prof, &n, err, errlen);
    } else {
        WCHAR (*list)[MAX_PATH] = (WCHAR (*)[MAX_PATH])malloc(sizeof(WCHAR) * MAX_PATH * 16);
        int   cnt, i;
        if (!list) return FALSE;
        cnt = ff_candidates(list, 16);
        for (i = 0; i < cnt && !ck; i++) {
            ck = ff_read_youtube_cookies(list[i], &n, err, errlen);
            if (ck) wcscpy_s(prof, MAX_PATH, list[i]);
        }
        free(list);
        if (!ck) {
            if (!cnt) _snprintf(err, errlen, "Firefox のプロファイルが見つかりませんでした。Portable 版はフォルダを指定してください");
            else _snprintf(err, errlen, "Firefox のプロファイルを %d 個見ましたが、YouTube にログインしたものがありません", cnt);
            err[errlen - 1] = 0;
            return FALSE;
        }
    }
    if (!ck) return FALSE;
    swap_cookie(ck);
    g_cfg.cookieSource = COOKIE_FIREFOX;
    wcscpy_s(g_cfg.ffProfile, MAX_PATH, prof);
    ff_user_agent(prof, g_cfg.ua, sizeof(g_cfg.ua));
    strcpy_s(g_cfg.authuser, sizeof(g_cfg.authuser), "0");
    /* ini には「Firefox から読む」ことと場所だけ。cookie は書かない(前に貼ったものは消す) */
    put_str(L"cookie", NULL);
    put_str(L"authuser", NULL);
    put_str(L"ua", NULL);
    put_str(L"cookie_source", "firefox");
    p = W2U(prof);
    put_str(L"firefox_profile", p);
    free(p);
    return TRUE;
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
