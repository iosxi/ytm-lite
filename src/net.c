/* ==================================================================
 * net.c - WinHTTP での通信
 *
 *  - cookie は WinHTTP に覚えさせない(WINHTTP_DISABLE_COOKIES)。覚えさせると、
 *    アカウント用の要求で受け取った Set-Cookie が匿名の再生側の要求にも付いてしまい、
 *    2 系統に分けた意味がなくなる。cookie は呼ぶ側が Cookie ヘッダーで明示する。
 *  - HTTP/2 を使う(ブラウザと同じ。1 本の接続で済む)。使えない Windows では HTTP/1.1。
 *  - プロキシは Windows の設定(自動構成を含む)に従う。
 *  - リダイレクトは WinHTTP に任せる(GVS は別のサーバーへ 302 で回すことがある)。
 * ================================================================== */

#include "ytm.h"
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#define MAX_BODY (32 * 1024 * 1024)

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif
#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 133
#endif
#ifndef WINHTTP_PROTOCOL_FLAG_HTTP2
#define WINHTTP_PROTOCOL_FLAG_HTTP2 0x1
#endif

static HINTERNET g_session;

void NetInit(void)
{
    DWORD protos, h2;
    if (g_session) return;
    /* User-Agent は要求ごとにヘッダーで差し替える。ここのは使われない */
    g_session = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_session)   /* Windows 8.1 より前は自動プロキシの指定が無い */
        g_session = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_session) return;
    WinHttpSetTimeouts(g_session, 15000, 15000, 30000, 60000);
    protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(g_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos))) {
        protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;   /* TLS 1.3 を知らない Windows 10 */
        WinHttpSetOption(g_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos));
    }
    h2 = WINHTTP_PROTOCOL_FLAG_HTTP2;
    WinHttpSetOption(g_session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &h2, sizeof(h2));
}

static void set_net_error(char *err, size_t errlen, const char *what)
{
    DWORD       e = GetLastError();
    const char *msg;
    switch (e) {
    case ERROR_WINHTTP_TIMEOUT:            msg = "時間切れ"; break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:  msg = "ホスト名を解決できない(ネットにつながっていない?)"; break;
    case ERROR_WINHTTP_CANNOT_CONNECT:     msg = "接続できない"; break;
    case ERROR_WINHTTP_CONNECTION_ERROR:   msg = "接続が切れた"; break;
    case ERROR_WINHTTP_SECURE_FAILURE:     msg = "TLS(証明書)の確認に失敗"; break;
    case ERROR_WINHTTP_REDIRECT_FAILED:    msg = "リダイレクトに失敗"; break;
    case ERROR_OPERATION_ABORTED:          msg = "中止した"; break;
    default:                               msg = NULL;
    }
    if (msg) _snprintf(err, errlen, "%s: %s", what, msg);
    else _snprintf(err, errlen, "%s: 通信エラー %lu", what, e);
    err[errlen - 1] = 0;
}

typedef struct { HINTERNET conn, req; } Req;

static void close_req(Req *r)
{
    if (r->req) WinHttpCloseHandle(r->req);
    if (r->conn) WinHttpCloseHandle(r->conn);
    r->req = r->conn = NULL;
}

static int open_req(const char *url, const WCHAR *method, const char *headers, const char *body,
                    Req *r, char *err, size_t errlen)
{
    URL_COMPONENTS uc;
    WCHAR  host[256], *w, *path;
    int    ok = 0;
    DWORD  opt;

    memset(r, 0, sizeof(*r));
    if (!g_session) { _snprintf(err, errlen, "WinHTTP を初期化できない"); return 0; }
    w = U2W(url);
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!w || !WinHttpCrackUrl(w, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        _snprintf(err, errlen, "URL の形式が正しくない");
        free(w);
        return 0;
    }
    wcsncpy_s(host, ARRAYSIZE(host), uc.lpszHostName, uc.dwHostNameLength < 255 ? uc.dwHostNameLength : 255);
    /* パスとクエリは文字列上で続いているので、まとめて渡す */
    path = (WCHAR *)malloc(sizeof(WCHAR) * (uc.dwUrlPathLength + uc.dwExtraInfoLength + 2));
    if (!path) { free(w); return 0; }
    if (!uc.dwUrlPathLength) wcscpy_s(path, 2, L"/");
    else { wmemcpy(path, uc.lpszUrlPath, uc.dwUrlPathLength); path[uc.dwUrlPathLength] = 0; }
    if (uc.dwExtraInfoLength) {
        size_t n = wcslen(path);
        wmemcpy(path + n, uc.lpszExtraInfo, uc.dwExtraInfoLength);
        path[n + uc.dwExtraInfoLength] = 0;
    }
    r->conn = WinHttpConnect(g_session, host, uc.nPort, 0);
    if (r->conn)
        r->req = WinHttpOpenRequest(r->conn, method, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (r->req) {
        opt = WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(r->req, WINHTTP_OPTION_DISABLE_FEATURE, &opt, sizeof(opt));
        opt = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(r->req, WINHTTP_OPTION_DECOMPRESSION, &opt, sizeof(opt));
        if (headers && *headers) {
            WCHAR *wh = U2W(headers);
            if (wh) WinHttpAddRequestHeaders(r->req, wh, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            free(wh);
        }
        if (WinHttpSendRequest(r->req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               (LPVOID)body, body ? (DWORD)strlen(body) : 0, body ? (DWORD)strlen(body) : 0, 0) &&
            WinHttpReceiveResponse(r->req, NULL))
            ok = 1;
    }
    if (!ok) {
        char *h = W2U(host);
        set_net_error(err, errlen, h ? h : "接続");
        free(h);
        close_req(r);
    }
    free(path);
    free(w);
    return ok;
}

static int query_status(HINTERNET req)
{
    DWORD status = 0, cb = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &cb, WINHTTP_NO_HEADER_INDEX);
    return (int)status;
}

static int query_retry_after(HINTERNET req)
{
    WCHAR buf[64];
    DWORD cb = sizeof(buf);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"Retry-After", buf, &cb, WINHTTP_NO_HEADER_INDEX)) return 0;
    return _wtoi(buf);
}

/* Set-Cookie をすべて拾う(改行区切り)。cookie は WinHTTP に覚えさせず、呼ぶ側が持つ */
static char *query_set_cookies(HINTERNET req)
{
    Buf   b = {0};
    DWORD idx = 0;
    for (;;) {
        WCHAR buf[4096];
        DWORD cb = sizeof(buf), cur = idx;
        char *u;
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_SET_COOKIE, WINHTTP_HEADER_NAME_BY_INDEX, buf, &cb, &idx)) break;
        u = W2U(buf);
        if (u) { if (b.n) BufAddChar(&b, '\n'); BufAddStr(&b, u); free(u); }
        if (idx == cur) break;
    }
    return b.p;
}

static int do_request(const WCHAR *method, const char *url, const char *headers, const char *body, HttpResp *r,
                      char *err, size_t errlen)
{
    Req   q;
    int   ok = 1;
    char  tmp[16384];
    memset(r, 0, sizeof(*r));
    if (!open_req(url, method, headers, body, &q, err, errlen)) return 0;
    r->status = query_status(q.req);
    r->retryAfter = query_retry_after(q.req);
    r->setCookie = query_set_cookies(q.req);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(q.req, tmp, sizeof(tmp), &got)) { set_net_error(err, errlen, "受信"); ok = 0; break; }
        if (!got) break;
        if (r->body.n + got > MAX_BODY) { _snprintf(err, errlen, "応答が大きすぎる"); ok = 0; break; }
        BufAdd(&r->body, tmp, got);
    }
    if (!r->body.p) BufAdd(&r->body, "", 0);
    close_req(&q);
    if (!ok) HttpRespFree(r);
    return ok;
}

int HttpPost(const char *url, const char *headers, const char *body, HttpResp *r, char *err, size_t errlen)
{
    return do_request(L"POST", url, headers, body, r, err, errlen);
}

int HttpGet(const char *url, const char *headers, HttpResp *r, char *err, size_t errlen)
{
    return do_request(L"GET", url, headers, NULL, r, err, errlen);
}

/* dst へ最大 want バイト読む。status は HTTP の状態 */
int HttpGetRange(const char *url, const char *headers, BYTE *dst, DWORD want, DWORD *got, int *status,
                 volatile LONG *cancel, char *err, size_t errlen)
{
    Req   q;
    int   ok = 1;
    *got = 0;
    *status = 0;
    if (!open_req(url, L"GET", headers, NULL, &q, err, errlen)) return 0;
    *status = query_status(q.req);
    if (*status != 200 && *status != 206) {
        close_req(&q);
        return 1;           /* 状態は呼ぶ側が見る */
    }
    while (*got < want) {
        DWORD n = 0;
        if (cancel && *cancel) { _snprintf(err, errlen, "中止した"); ok = 0; break; }
        if (!WinHttpReadData(q.req, dst + *got, want - *got, &n)) { set_net_error(err, errlen, "音声の受信"); ok = 0; break; }
        if (!n) break;
        *got += n;
    }
    close_req(&q);
    return ok;
}

void HttpRespFree(HttpResp *r)
{
    BufFree(&r->body);
    free(r->setCookie);
    r->setCookie = NULL;
}
