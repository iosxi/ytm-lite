/* ==================================================================
 * util.c - 文字列・バッファ・ハッシュ・ログの小道具
 * ================================================================== */

#include "ytm.h"
#include <stdarg.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

void BufAdd(Buf *b, const void *data, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        char  *p;
        while (cap < b->n + n + 1) cap *= 2;
        p = (char *)realloc(b->p, cap);
        if (!p) return;
        b->p = p;
        b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, data, n);
    b->n += n;
    b->p[b->n] = 0;
}

void BufAddStr(Buf *b, const char *s) { BufAdd(b, s, strlen(s)); }
void BufAddChar(Buf *b, char c) { BufAdd(b, &c, 1); }

void BufAddf(Buf *b, const char *fmt, ...)
{
    char    tmp[1024];
    va_list ap;
    int     n;
    va_start(ap, fmt);
    n = _vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(tmp)) {          /* 長いときは測ってから */
        char *big;
        va_start(ap, fmt);
        n = _vscprintf(fmt, ap);
        va_end(ap);
        if (n < 0) return;
        big = (char *)malloc((size_t)n + 1);
        if (!big) return;
        va_start(ap, fmt);
        _vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        BufAdd(b, big, (size_t)n);
        free(big);
        return;
    }
    BufAdd(b, tmp, (size_t)n);
}

void BufAddJsonStr(Buf *b, const char *s)
{
    BufAddChar(b, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { BufAddChar(b, '\\'); BufAddChar(b, (char)c); }
        else if (c < 0x20) BufAddf(b, "\\u%04x", c);
        else BufAddChar(b, (char)c);
    }
    BufAddChar(b, '"');
}

void BufFree(Buf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

char *BufTake(Buf *b)
{
    char *p = b->p;
    if (!p) p = U8Dup("");
    b->p = NULL;
    b->n = b->cap = 0;
    return p;
}

WCHAR *U2W(const char *s)
{
    int    n;
    WCHAR *w;
    if (!s) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    w = (WCHAR *)malloc(sizeof(WCHAR) * (n > 0 ? n : 1));
    if (!w) return NULL;
    if (n <= 0) { w[0] = 0; return w; }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

char *W2U(const WCHAR *w)
{
    int   n;
    char *s;
    if (!w) return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    s = (char *)malloc(n > 0 ? (size_t)n : 1);
    if (!s) return NULL;
    if (n <= 0) { s[0] = 0; return s; }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

char *U8NDup(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *U8Dup(const char *s) { return s ? U8NDup(s, strlen(s)) : NULL; }

WCHAR *WDup(const WCHAR *s)
{
    size_t n;
    WCHAR *p;
    if (!s) return NULL;
    n = wcslen(s) + 1;
    p = (WCHAR *)malloc(n * sizeof(WCHAR));
    if (p) memcpy(p, s, n * sizeof(WCHAR));
    return p;
}

const char *U8FindI(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (_strnicmp(hay, needle, n) == 0) return hay;
    return NULL;
}

void Sha1Hex(const char *s, char out[41])
{
    BCRYPT_ALG_HANDLE  alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    UCHAR              d[20];
    int                i;
    out[0] = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0) != 0) return;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        BCryptHashData(h, (PUCHAR)s, (ULONG)strlen(s), 0);
        if (BCryptFinishHash(h, d, sizeof(d), 0) == 0) {
            for (i = 0; i < 20; i++) sprintf(out + i * 2, "%02x", d[i]);
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
}

LONG64 now_ms(void) { return (LONG64)GetTickCount64(); }

LONG64 unix_time(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (LONG64)(u.QuadPart / 10000000ULL) - 11644473600LL;
}

static CRITICAL_SECTION g_logCs;
static BOOL g_logInit;

void log_printf(const char *fmt, ...)
{
    char       line[2048];
    int        n;
    va_list    ap;
    SYSTEMTIME st;
    WCHAR      path[MAX_PATH];
    HANDLE     f;
    DWORD      wr;

    if (!g_cfg.debug) return;
    if (!g_logInit) { InitializeCriticalSection(&g_logCs); g_logInit = TRUE; }
    GetLocalTime(&st);
    n = _snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    n += _vsnprintf(line + n, sizeof(line) - (size_t)n - 3, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof(line) - 3) n = (int)sizeof(line) - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    _snwprintf(path, MAX_PATH, L"%s\\ytm-lite.log", g_cfg.dir);
    path[MAX_PATH - 1] = 0;
    EnterCriticalSection(&g_logCs);
    f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, line, (DWORD)n, &wr, NULL);
        CloseHandle(f);
    }
    LeaveCriticalSection(&g_logCs);
}

void out_printf(const char *fmt, ...)
{
    Buf     b = {0};
    char    tmp[2048];
    va_list ap;
    int     n;
    DWORD   wr;
    va_start(ap, fmt);
    n = _vsnprintf(tmp, sizeof(tmp) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)sizeof(tmp) - 1;
    tmp[n] = 0;
    BufAddStr(&b, tmp);
    BufAddStr(&b, "\r\n");
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), b.p, (DWORD)b.n, &wr, NULL);
    BufFree(&b);
}
