/* ==================================================================
 * yt.c - YouTube Music との通信(InnerTube API)
 *
 *  安全のための作法(調べた根拠は DEVELOPMENT.md):
 *
 *  1. アカウントの cookie は music.youtube.com の browse にしか付けない。
 *     ブラウザの YouTube Music がプレイリストを開くときと同じ要求で、回数も
 *     「開いたときに 1 回 + 100 曲ごとに 1 回」だけ。
 *     yt-dlp の Wiki は「アカウントを yt-dlp で使うと BAN されうる」と警告しているが、
 *     それはアカウントで動画を大量に取得する使い方のこと。ここでは再生の要求に
 *     アカウントを一切使わない。
 *
 *  2. 再生の要求(player)は cookie なしの匿名で、VISIONOS クライアントとして送る。
 *     2026-10-10 の yt-dlp で「ログインしないときの既定」になっているもので、
 *     PO トークンも JS(n / sig の解読)も要らない。必要の無いトークンを偽造したり、
 *     YouTube の JS を解読したりしないので、それらが失敗して怪しまれることも無い。
 *     visitorData は端末ごとに 1 つ持ち続ける(実機と同じ)。
 *
 *  3. 回数を抑える。player は曲ごとに 1 回(と、次の曲の先読み 1 回)。間隔は最低 2 秒、
 *     5 分間に 15 回まで。yt-dlp の Wiki によれば匿名の上限は 1 時間に約 1000 回の
 *     webpage/player 要求なので、その 1/5 以下。並列には送らない(worker.c が 1 本で回す)。
 *     「次へ」の連打は ui.c がまとめ、止まった曲の分だけ送る。
 *
 *  4. 拒まれたら引く。429、または「ボットでないことを確認」が返ったら、30 分
 *     (Retry-After があればそれ)何も送らない。ini に書くので、起動し直しても続く。
 *     自動で何度もやり直すことはしない(GVS の 403 だけ、worker.c が 5 秒おいて 1 回)。
 * ================================================================== */

#include "ytm.h"

#define MUSIC_ORIGIN   "https://music.youtube.com"
#define MUSIC_API      "https://music.youtube.com/youtubei/v1/"
#define PLAYER_API     "https://www.youtube.com/youtubei/v1/player?prettyPrint=false"

/* yt-dlp 2026-10-10(_base.py の INNERTUBE_CLIENTS['visionos'])と同じ値 */
#define VIS_NAME       "VISIONOS"
#define VIS_NAME_NUM   "101"
#define VIS_VERSION    "1.02"
#define VIS_UA         "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15"

/* 貼り付けたヘッダーに user-agent が無いときに使う。2026-10 の Chrome の版(この PC で 154)に合わせる */
#define DEFAULT_UA     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/154.0.0.0 Safari/537.36"

#define CHUNK          (10 * 1024 * 1024)      /* yt-dlp と同じ。GVS はこれを超える range を嫌う */
#define MIN_GAP_MS     2000
/* v4 まで 10 分に 20 回。待ちが最長 10 分と長いので、本人の要望で 5 分に 15 回にした(2026-10-12)。
   1 時間あたり最大 180 回で、yt-dlp の Wiki の匿名の上限(約 1000 回)の 1/5 以下。
   実測: ゲストのセッションで約 2.5 秒おきに 10 回続けて送ったまとまりが 2 回あっても拒まれなかった。 */
#define WINDOW_MS      (5 * 60 * 1000)
#define WINDOW_MAX     15
#define COOLDOWN_SEC   (30 * 60)
#define MAX_PAGES      100

static char   g_musicVisitor[1024];      /* アカウントの browse 用(メモリだけ) */
static char   g_userSession[64];        /* datasyncId から。SAPISIDHASH の _u に使う */
static LONG64 g_hist[WINDOW_MAX];       /* player を送った時刻(now_ms) */
static int    g_histPos;

/* ------------------------------------------------------------------ */
/*  cookie と認証                                                      */
/* ------------------------------------------------------------------ */

/* "a=1; b=2" から name の値を取る */
static BOOL cookie_get(const char *cookie, const char *name, char *out, size_t cap)
{
    size_t      nl = strlen(name);
    const char *p = cookie;
    out[0] = 0;
    while (p && *p) {
        while (*p == ' ' || *p == ';') p++;
        if (!strncmp(p, name, nl) && p[nl] == '=') {
            const char *v = p + nl + 1;
            size_t      n = strcspn(v, ";");
            while (n && v[n - 1] == ' ') n--;
            if (n >= cap) n = cap - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return TRUE;
        }
        p = strchr(p, ';');
    }
    return FALSE;
}

BOOL yt_cookie_ok(const char *cookie)
{
    char v[256];
    return cookie && (cookie_get(cookie, "SAPISID", v, sizeof(v)) || cookie_get(cookie, "__Secure-3PAPISID", v, sizeof(v)));
}

/* ブラウザ(と yt-dlp)と同じ形: SAPISIDHASH ts_sha1 SAPISID1PHASH ts_sha1 SAPISID3PHASH ts_sha1 */
static void add_auth(Buf *h, const char *cookie, const char *origin)
{
    static const char *schemes[3] = { "SAPISIDHASH", "SAPISID1PHASH", "SAPISID3PHASH" };
    static const char *names[3]   = { "SAPISID", "__Secure-1PAPISID", "__Secure-3PAPISID" };
    LONG64 ts = unix_time();
    int    i, n = 0;
    char   sid[256], hash[41];
    Buf    in = {0};

    for (i = 0; i < 3; i++) {
        if (!cookie_get(cookie, names[i], sid, sizeof(sid)) &&
            !(i == 0 && cookie_get(cookie, "__Secure-3PAPISID", sid, sizeof(sid))))   /* SAPISID が無ければ 3P で代用 */
            continue;
        in.n = 0;
        if (g_userSession[0]) BufAddf(&in, "%s ", g_userSession);
        BufAddf(&in, "%lld %s %s", ts, sid, origin);
        Sha1Hex(in.p, hash);
        BufAddStr(h, n++ ? " " : "Authorization: ");
        BufAddf(h, "%s %lld_%s%s", schemes[i], ts, hash, g_userSession[0] ? "_u" : "");
    }
    if (n) BufAddStr(h, "\r\n");
    BufFree(&in);
}

/* 貼り付けられたもの(要求ヘッダー全体、curl のコマンド、cookie の値だけ)から取り出す */
static void trim_quotes(char *s)
{
    size_t n;
    while (*s == ' ' || *s == '\t') memmove(s, s + 1, strlen(s));
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\\' || s[n - 1] == '\'' || s[n - 1] == '"' || s[n - 1] == '^')) s[--n] = 0;
}

BOOL yt_parse_auth_text(const char *text, char **cookie, char *authuser, size_t aulen, char *ua, size_t ualen)
{
    char       *copy = U8Dup(text), *line, *ctx = NULL;
    char        pendingKey[32] = "";
    Buf         ck = {0};

    *cookie = NULL;
    authuser[0] = 0;
    ua[0] = 0;
    if (!copy) return FALSE;
    for (line = strtok_s(copy, "\r\n", &ctx); line; line = strtok_s(NULL, "\r\n", &ctx)) {
        char *p = line, *colon;
        while (*p == ' ' || *p == '\t') p++;
        /* curl: -H 'name: value' / -b 'cookie' / --cookie 'cookie' */
        if (!strncmp(p, "-H ", 3) || !strncmp(p, "--header ", 9)) { p = strchr(p, ' ') + 1; while (*p == ' ') p++; if (*p == '\'' || *p == '"' || *p == '^') p++; }
        else if (!strncmp(p, "-b ", 3) || !strncmp(p, "--cookie ", 9)) {
            p = strchr(p, ' ') + 1;
            while (*p == ' ') p++;
            if (*p == '\'' || *p == '"' || *p == '^') p++;
            trim_quotes(p);
            ck.n = 0;
            BufAddStr(&ck, p);
            continue;
        }
        if (pendingKey[0]) {       /* Chrome の「名前:」の次の行に値が来る形 */
            trim_quotes(p);
            if (!_stricmp(pendingKey, "cookie")) { ck.n = 0; BufAddStr(&ck, p); }
            else if (!_stricmp(pendingKey, "x-goog-authuser")) strncpy_s(authuser, aulen, p, _TRUNCATE);
            else if (!_stricmp(pendingKey, "user-agent")) strncpy_s(ua, ualen, p, _TRUNCATE);
            pendingKey[0] = 0;
            continue;
        }
        colon = strchr(p, ':');
        if (colon && colon != p && colon - p < (int)sizeof(pendingKey)) {
            char key[32];
            char *v = colon + 1;
            memcpy(key, p, (size_t)(colon - p));
            key[colon - p] = 0;
            while (*v == ' ' || *v == '\t') v++;
            if (!_stricmp(key, "cookie") || !_stricmp(key, "x-goog-authuser") || !_stricmp(key, "user-agent")) {
                trim_quotes(v);
                if (!*v) { strcpy_s(pendingKey, sizeof(pendingKey), key); continue; }
                if (!_stricmp(key, "cookie")) { ck.n = 0; BufAddStr(&ck, v); }
                else if (!_stricmp(key, "x-goog-authuser")) strncpy_s(authuser, aulen, v, _TRUNCATE);
                else strncpy_s(ua, ualen, v, _TRUNCATE);
            }
        }
    }
    free(copy);
    if (!ck.p) {
        /* cookie の値だけを貼ったとき */
        if (strstr(text, "SAPISID=") || strstr(text, "__Secure-3PAPISID=")) {
            const char *s = text;
            while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
            for (; *s; s++) if (*s != '\r' && *s != '\n') BufAddChar(&ck, *s);
            trim_quotes(ck.p);
            ck.n = strlen(ck.p);
        }
    }
    if (!ck.p || !yt_cookie_ok(ck.p)) { BufFree(&ck); return FALSE; }
    *cookie = BufTake(&ck);
    return TRUE;
}

char *yt_playlist_id_from_text(const char *text)
{
    const char *p = strstr(text, "list=");
    size_t      n;
    if (p) p += 5;
    else {
        p = text;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "VL", 2)) p += 2;
    }
    n = 0;
    while ((p[n] >= 'A' && p[n] <= 'Z') || (p[n] >= 'a' && p[n] <= 'z') || (p[n] >= '0' && p[n] <= '9') || p[n] == '_' || p[n] == '-') n++;
    if (n < 2 || n > 100) return NULL;
    return U8NDup(p, n);
}

/* ------------------------------------------------------------------ */
/*  匿名側のゲストのセッション                                          */
/*                                                                     */
/*  2026-10-12 の実測: cookie も visitorData も持たない匿名の要求を      */
/*  数分で 7 回(browse 4、player 3)送ったところ、3 回目の player で     */
/*  「ボットではないことを確認」が返った。yt-dlp は player の前に必ず     */
/*  ページを開いてゲストの cookie と visitorData を得て、それを付けて送る。 */
/*  ここでも 1 台の端末として同じゲストのセッションを持ち続ける           */
/*  (ini の visitor と guest)。                                        */
/* ------------------------------------------------------------------ */

/* Set-Cookie(改行区切り)を jar("a=1; b=2")に取り込む。変わったら TRUE */
static BOOL jar_merge(char *jar, size_t cap, const char *setCookies)
{
    Buf         out = {0};
    const char *line = setCookies;
    BOOL        changed = FALSE;
    char        name[128], val[1024];

    if (!setCookies) return FALSE;
    BufAddStr(&out, jar);
    while (*line) {
        size_t      ll = strcspn(line, "\n"), nl, vl;
        const char *eq = memchr(line, '=', ll), *semi;
        BOOL        del;
        if (eq && eq > line && (size_t)(eq - line) < sizeof(name)) {
            nl = (size_t)(eq - line);
            memcpy(name, line, nl);
            name[nl] = 0;
            semi = memchr(eq + 1, ';', ll - nl - 1);
            vl = semi ? (size_t)(semi - eq - 1) : ll - nl - 1;
            if (vl < sizeof(val)) {
                char  cur[1024];
                memcpy(val, eq + 1, vl);
                val[vl] = 0;
                del = !val[0] || U8FindI(line, "Max-Age=0") != NULL;
                {
                    /* いまの値と比べ、違えば作り直す */
                    char *copy = U8Dup(out.p), *p, *ctx = NULL;
                    Buf   nb = {0};
                    BOOL  found = FALSE;
                    for (p = strtok_s(copy, ";", &ctx); p; p = strtok_s(NULL, ";", &ctx)) {
                        while (*p == ' ') p++;
                        if (!*p) continue;
                        if (!strncmp(p, name, nl) && p[nl] == '=') {
                            found = TRUE;
                            strncpy_s(cur, sizeof(cur), p + nl + 1, _TRUNCATE);
                            if (del || strcmp(cur, val)) changed = TRUE;
                            if (del) continue;
                            if (nb.n) BufAddStr(&nb, "; ");
                            BufAddf(&nb, "%s=%s", name, val);
                            continue;
                        }
                        if (nb.n) BufAddStr(&nb, "; ");
                        BufAddStr(&nb, p);
                    }
                    if (!found && !del) {
                        if (nb.n) BufAddStr(&nb, "; ");
                        BufAddf(&nb, "%s=%s", name, val);
                        changed = TRUE;
                    }
                    free(copy);
                    BufFree(&out);
                    out = nb;
                    if (!out.p) BufAdd(&out, "", 0);
                }
            }
        }
        line += ll;
        if (*line == '\n') line++;
    }
    if (changed) {
        if (out.n < cap) strcpy_s(jar, cap, out.p);
        else changed = FALSE;          /* 入りきらないものは取り込まない(切れた cookie を送らない) */
    }
    BufFree(&out);
    return changed;
}

int yt_selftest_jar(void)
{
    char jar[256] = "";
    int  fail = 0;
    fail += !jar_merge(jar, sizeof(jar), "YSC=abc; Domain=.youtube.com; Path=/; Secure; HttpOnly\nVISITOR_INFO1_LIVE=v1; Max-Age=15552000");
    fail += strcmp(jar, "YSC=abc; VISITOR_INFO1_LIVE=v1") != 0;
    fail += jar_merge(jar, sizeof(jar), "YSC=abc; Path=/");                    /* 同じ値: 変わらない */
    fail += !jar_merge(jar, sizeof(jar), "YSC=def; Path=/\nX=1");
    fail += strcmp(jar, "YSC=def; VISITOR_INFO1_LIVE=v1; X=1") != 0;
    fail += !jar_merge(jar, sizeof(jar), "VISITOR_INFO1_LIVE=; Max-Age=0");    /* 消す */
    fail += strcmp(jar, "YSC=def; X=1") != 0;
    return fail;
}

static void note_guest_cookies(const char *setCookies)
{
    if (jar_merge(g_cfg.guest, sizeof(g_cfg.guest), setCookies)) cfg_save_guest();
}

static BOOL rate_wait(volatile LONG *cancel);
static void locale_hl_gl(char *hl, char *gl);

/* visitorData とゲストの cookie が無ければ、ブラウザで YouTube を開いたときと同じく
   トップページを 1 回取って得る(初回だけ)。 */
static int ensure_guest(volatile LONG *cancel, char *err, size_t errlen)
{
    Buf         h = {0};
    HttpResp    r;
    const char *p;
    char        hl[4], gl[4];

    if (g_cfg.visitor[0] && g_cfg.guest[0]) return 1;
    if (g_cfg.cooldownUntil > unix_time()) { _snprintf(err, errlen, "YouTube に拒まれたため、匿名の要求を止めています"); return 0; }
    if (!rate_wait(cancel)) return 0;
    locale_hl_gl(hl, gl);
    BufAddStr(&h, "User-Agent: " VIS_UA "\r\n");
    BufAddStr(&h, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n");
    BufAddf(&h, "Accept-Language: %s-%s,%s;q=0.9\r\n", hl, gl, hl);
    if (g_cfg.guest[0]) BufAddf(&h, "Cookie: %s\r\n", g_cfg.guest);
    log_printf("guest session: GET www.youtube.com");
    if (!HttpGet("https://www.youtube.com/", h.p, &r, err, errlen)) { BufFree(&h); return 0; }
    BufFree(&h);
    log_printf("  -> HTTP %d, %u bytes", r.status, (unsigned)r.body.n);
    note_guest_cookies(r.setCookie);
    p = strstr(r.body.p, "\"VISITOR_DATA\":\"");
    if (p) {
        size_t n;
        p += 16;
        n = strcspn(p, "\"");
        if (n && n < sizeof(g_cfg.visitor)) {
            memcpy(g_cfg.visitor, p, n);
            g_cfg.visitor[n] = 0;
            cfg_save_guest();
        }
    }
    HttpRespFree(&r);
    if (!g_cfg.visitor[0]) { _snprintf(err, errlen, "YouTube のページから visitorData を得られなかった"); return 0; }
    log_printf("  visitor %u chars, cookies: %s", (unsigned)strlen(g_cfg.visitor), g_cfg.guest[0] ? "yes" : "none");
    return 1;
}

/* ------------------------------------------------------------------ */
/*  music.youtube.com の browse                                         */
/* ------------------------------------------------------------------ */

static void locale_hl_gl(char *hl, char *gl)
{
    WCHAR loc[LOCALE_NAME_MAX_LENGTH];
    strcpy(hl, "ja");
    strcpy(gl, "JP");
    if (GetUserDefaultLocaleName(loc, ARRAYSIZE(loc)) && wcslen(loc) >= 5 && loc[2] == L'-') {
        hl[0] = (char)towlower(loc[0]); hl[1] = (char)towlower(loc[1]); hl[2] = 0;
        gl[0] = (char)towupper(loc[3]); gl[1] = (char)towupper(loc[4]); gl[2] = 0;
    }
}

static void music_client_version(char *out, size_t cap)
{
    SYSTEMTIME st;
    GetSystemTime(&st);           /* ytmusicapi と同じく、その日の日付で作る */
    _snprintf(out, cap, "1.%04d%02d%02d.01.00", st.wYear, st.wMonth, st.wDay);
}

/* 応答から次の要求に使うものを拾う */
static void note_response_context(J *root, BOOL account)
{
    const char *v = JStr(root, "responseContext.visitorData");
    const char *ds = JStr(root, "responseContext.mainAppWebResponseContext.datasyncId");
    if (account && v && !g_musicVisitor[0] && strlen(v) < sizeof(g_musicVisitor))
        strcpy_s(g_musicVisitor, sizeof(g_musicVisitor), v);       /* 切れたものは送らない */
    if (account && ds && !g_userSession[0]) {
        /* "USER||" か "DELEGATED||USER" */
        const char *bar = strstr(ds, "||");
        const char *user = bar && bar[2] ? bar + 2 : ds;
        size_t      n = strcspn(user, "|");
        if (n < sizeof(g_userSession)) { memcpy(g_userSession, user, n); g_userSession[n] = 0; }
    }
}

/* ログインできているか(responseContext の serviceTrackingParams の logged_in) */
static int logged_in_flag(J *root)
{
    J *stp = JPath(root, "responseContext.serviceTrackingParams"), *s, *p;
    if (!stp) return -1;
    for (s = stp->child; s; s = s->next)
        for (p = JGet(s, "params") ? JGet(s, "params")->child : NULL; p; p = p->next) {
            const char *k = JStr(p, "key"), *v = JStr(p, "value");
            if (k && v && !strcmp(k, "logged_in")) return v[0] == '1';
        }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  アカウントの要求で、ログオフの兆しをログに残す                       */
/*                                                                     */
/*  2026-10-13: 遠隔の利用者のアカウントが 2 日続けてログオフされた。     */
/*  次に起きたとき YouTube から何が返っていたかを追えるよう、debug に     */
/*  関係なく ytm-lite.log に書く(log_always)。cookie の値は書かない。    */
/* ------------------------------------------------------------------ */

/* ログインそのものの cookie。これが消される・入れ替わるのはログオフの兆し */
static BOOL is_login_cookie(const char *name)
{
    static const char *const names[] = {
        "SID", "HSID", "SSID", "APISID", "SAPISID", "LOGIN_INFO",
        "__Secure-1PSID", "__Secure-3PSID", "__Secure-1PAPISID", "__Secure-3PAPISID",
        "__Secure-1PSIDTS", "__Secure-3PSIDTS", NULL
    };
    int i;
    for (i = 0; names[i]; i++) if (!strcmp(name, names[i])) return TRUE;
    return FALSE;
}

/* Set-Cookie の 1 行が「消す」ものか(値が空・EXPIRED・deleted、Max-Age=0 以下、Expires が過去の年) */
static BOOL set_cookie_deletes(const char *line, size_t ll, const char *val, size_t vl)
{
    char        tmp[2048];
    const char *p;
    SYSTEMTIME  st;
    if (!vl || (vl == 7 && !strncmp(val, "EXPIRED", 7)) || (vl == 7 && !strncmp(val, "deleted", 7))) return TRUE;
    if (ll >= sizeof(tmp)) ll = sizeof(tmp) - 1;
    memcpy(tmp, line, ll);
    tmp[ll] = 0;
    if ((p = U8FindI(tmp, "Max-Age=")) != NULL && (p[8] == '0' || p[8] == '-')) return TRUE;
    if ((p = U8FindI(tmp, "Expires=")) != NULL) {
        GetSystemTime(&st);
        for (p += 8; *p && *p != ';'; p++)       /* "Thu, 01-Jan-1970 00:00:00 GMT" の 4 桁の年 */
            if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9' &&
                p[3] >= '0' && p[3] <= '9')
                return atoi(p) < st.wYear;
    }
    return FALSE;
}

static BOOL deletes(const char *line)
{
    const char *eq = strchr(line, '='), *semi = strchr(line, ';');
    size_t      vl = semi ? (size_t)(semi - eq - 1) : strlen(eq + 1);
    return set_cookie_deletes(line, strlen(line), eq + 1, vl);
}

int yt_selftest_setcookie(void)
{
    int fail = 0;
    fail += !deletes("SID=EXPIRED; Domain=.youtube.com; Path=/");
    fail += !deletes("SID=; Path=/");
    fail += !deletes("__Secure-3PSID=x; Max-Age=0; Path=/");
    fail += !deletes("SID=x; Expires=Thu, 01-Jan-1970 00:00:01 GMT; Path=/");
    fail += deletes("__Secure-1PSIDTS=sidts-abc; Expires=Fri, 15-Oct-2027 01:02:03 GMT; Path=/; Secure");
    fail += deletes("YSC=abc; Domain=.youtube.com; Path=/; Secure; HttpOnly");
    fail += deletes("VISITOR_INFO1_LIVE=v1; Max-Age=15552000");
    fail += !is_login_cookie("__Secure-1PSIDTS") || is_login_cookie("SIDCC");
    return fail;
}

static BOOL g_loginLogged;     /* 「ログインできている」をこの起動で書いたか */

static void note_account_response(const char *endpoint, const HttpResp *r, J *root)
{
    Buf         login = {0}, other = {0};
    const char *line = r->setCookie;
    int         li;

    /* Set-Cookie: 名前と「消す」かどうかだけ */
    while (line && *line) {
        size_t      ll = strcspn(line, "\n");
        const char *eq = memchr(line, '=', ll);
        if (eq && eq > line && eq - line < 128) {
            char        name[128];
            const char *val = eq + 1, *semi;
            size_t      vl;
            memcpy(name, line, (size_t)(eq - line));
            name[eq - line] = 0;
            semi = memchr(val, ';', ll - (size_t)(val - line));
            vl = semi ? (size_t)(semi - val) : ll - (size_t)(val - line);
            {
                Buf *b = is_login_cookie(name) ? &login : &other;
                if (b->n) BufAddStr(b, ", ");
                BufAddf(b, "%s%s", name, set_cookie_deletes(line, ll, val, vl) ? "(DELETED)" : "");
            }
        }
        line += ll;
        if (*line == '\n') line++;
    }
    if (login.n) log_always("account %s: Set-Cookie of login cookies: %s", endpoint, login.p);
    if (other.n) log_printf("  set-cookie: %s", other.p);

    if (r->status != 200) {
        JDoc       *d = JsonParse(r->body.p, r->body.n);
        J          *e = d ? JsonRoot(d) : NULL;
        const char *st = JStr(e, "error.status"), *msg = JStr(e, "error.message");
        log_always("account %s: HTTP %d %s %s", endpoint, r->status, st ? st : "", msg ? msg : "");
        if (d) JsonFree(d);
    }
    /* logged_in は、ログインできていれば 1、cookie が効いていなければ 0(2026-10-13 実測: アカウントの
       browse は 1、匿名は 0)。WEB の応答にある mainAppWebResponseContext.loggedOut は、YouTube Music では
       どちらの応答にも無かった。「いつまでログインできていたか」も追えるよう、通ったことも起動ごとに 1 回書く */
    if (root) {
        li = logged_in_flag(root);
        if (li != 1) {
            log_always("account %s: NOT logged in (logged_in=%d, cookie from %s)", endpoint, li,
                       g_cfg.cookieSource == COOKIE_FIREFOX ? "Firefox" : "paste");
            g_loginLogged = FALSE;
        } else if (!g_loginLogged) {
            log_always("account %s: logged in (cookie from %s)", endpoint,
                       g_cfg.cookieSource == COOKIE_FIREFOX ? "Firefox" : "paste");
            g_loginLogged = TRUE;
        }
    }
    BufFree(&login);
    BufFree(&other);
}

static JDoc *music_post_once(const char *endpoint, const char *query, const char *bodyTail, BOOL account,
                             BOOL *authFail, char *err, size_t errlen)
{
    Buf      url = {0}, h = {0}, body = {0};
    char     ver[32], hl[4], gl[4];
    HttpResp r;
    JDoc    *doc = NULL;
    /* アカウントはアカウントの visitor、匿名はゲストのセッション(再生側と同じ端末として) */
    const char *visitor = account ? g_musicVisitor : g_cfg.visitor;

    *authFail = FALSE;
    if (account && !g_cfg.cookie) {
        _snprintf(err, errlen, "%s", g_cfg.cookieErr[0] ? g_cfg.cookieErr : "ログイン情報が設定されていない");
        err[errlen - 1] = 0;
        if (g_cfg.cookieSource != COOKIE_NONE) log_always("account %s: no login cookies: %s", endpoint, err);
        *authFail = TRUE;
        return NULL;
    }
    if (!account && g_cfg.cooldownUntil > unix_time()) {
        _snprintf(err, errlen, "YouTube に拒まれたため、あと %lld 分は匿名の要求を止めています",
                  (g_cfg.cooldownUntil - unix_time() + 59) / 60);
        return NULL;
    }
    if (!account && !ensure_guest(NULL, err, errlen)) return NULL;
    music_client_version(ver, sizeof(ver));
    locale_hl_gl(hl, gl);
    BufAddf(&url, MUSIC_API "%s?prettyPrint=false%s", endpoint, query ? query : "");

    BufAddf(&body, "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"%s\",\"hl\":\"%s\",\"gl\":\"%s\"", ver, hl, gl);
    if (visitor[0]) { BufAddStr(&body, ",\"visitorData\":"); BufAddJsonStr(&body, visitor); }
    BufAddStr(&body, "},\"user\":{}},");
    BufAddStr(&body, bodyTail);
    BufAddChar(&body, '}');

    BufAddStr(&h, "Content-Type: application/json\r\n");
    BufAddf(&h, "User-Agent: %s\r\n", !account ? VIS_UA : g_cfg.ua[0] ? g_cfg.ua : DEFAULT_UA);
    BufAddStr(&h, "Accept: */*\r\n");
    BufAddf(&h, "Accept-Language: %s\r\n", hl);
    BufAddStr(&h, "Origin: " MUSIC_ORIGIN "\r\nReferer: " MUSIC_ORIGIN "/\r\n");
    BufAddStr(&h, "X-YouTube-Client-Name: 67\r\n");
    BufAddf(&h, "X-YouTube-Client-Version: %s\r\n", ver);
    if (visitor[0]) BufAddf(&h, "X-Goog-Visitor-Id: %s\r\n", visitor);
    if (!account && g_cfg.guest[0]) BufAddf(&h, "Cookie: %s\r\n", g_cfg.guest);
    if (account) {
        BufAddf(&h, "X-Goog-AuthUser: %s\r\n", g_cfg.authuser[0] ? g_cfg.authuser : "0");
        BufAddStr(&h, "X-Origin: " MUSIC_ORIGIN "\r\n");
        BufAddStr(&h, "X-Youtube-Bootstrap-Logged-In: true\r\n");
        add_auth(&h, g_cfg.cookie, MUSIC_ORIGIN);
        BufAddf(&h, "Cookie: %s\r\n", g_cfg.cookie);
    }

    log_printf("browse %s%s%s", endpoint, query ? query : "", account ? " (account)" : " (anon)");
    if (HttpPost(url.p, h.p, body.p, &r, err, errlen)) {
        if (r.status == 401 || r.status == 403) {
            _snprintf(err, errlen, account ? "ログイン情報が受け付けられなかった(HTTP %d)。設定し直してください" : "拒否された(HTTP %d)", r.status);
            *authFail = account;
        } else if (r.status == 429) {
            _snprintf(err, errlen, "要求が多すぎると言われた(HTTP 429)。しばらく時間をおいてください");
        } else if (r.status != 200) {
            _snprintf(err, errlen, "YouTube Music が HTTP %d を返した", r.status);
        } else {
            doc = JsonParse(r.body.p, r.body.n);
            if (!doc) _snprintf(err, errlen, "応答を読めなかった");
            else note_response_context(JsonRoot(doc), account);
        }
        log_printf("  -> HTTP %d, %u bytes", r.status, (unsigned)r.body.n);
        if (account) note_account_response(endpoint, &r, doc ? JsonRoot(doc) : NULL);
        else note_guest_cookies(r.setCookie);
        HttpRespFree(&r);
    }
    err[errlen - 1] = 0;
    BufFree(&url);
    BufFree(&h);
    BufFree(&body);
    return doc;
}

/* Firefox から読んでいる cookie が拒まれたら(Firefox 側で入れ替わった)、読み直して 1 回だけやり直す */
static BOOL reload_account(void)
{
    char e[256];
    if (g_cfg.cookieSource != COOKIE_FIREFOX || !cfg_reload_cookie(e, sizeof(e))) return FALSE;
    g_userSession[0] = 0;
    g_musicVisitor[0] = 0;
    log_always("account cookies re-read from Firefox to retry");
    return TRUE;
}

static JDoc *music_post(const char *endpoint, const char *query, const char *bodyTail, BOOL account,
                        char *err, size_t errlen)
{
    BOOL  authFail = FALSE;
    JDoc *d = music_post_once(endpoint, query, bodyTail, account, &authFail, err, errlen);
    if (!d && authFail && reload_account()) d = music_post_once(endpoint, query, bodyTail, account, &authFail, err, errlen);
    return d;
}

static void (*g_progressHook)(const char *id, int count);

void yt_set_progress_hook(void (*fn)(const char *id, int count)) { g_progressHook = fn; }

static int parse_duration(const WCHAR *s)
{
    int v = 0, part = 0;
    if (!s) return 0;
    for (; *s; s++) {
        if (*s >= L'0' && *s <= L'9') part = part * 10 + (*s - L'0');
        else if (*s == L':') { v = v * 60 + part; part = 0; }
    }
    return v * 60 + part;
}

static WCHAR *flex_text(J *r, int col)
{
    J *c = JAt(JGet(r, "flexColumns"), col);
    return JText(JPath(c, "musicResponsiveListItemFlexColumnRenderer.text"));
}

/* 一番大きい画像の URL。正方形のジャケット(lh3 / yt3 の「=w60-h60-…」)は thumbnail_size(既定 544)に
   指定し直す。v7 は Chrome と同じ 150 にしていたが、muutask の拡大表示(標準で 180 DIP、125% で 225px)で
   引き伸ばされて粗かった(2026-10-13 本人の指摘)。544 は 89KB、1200 は 258KB(実測) */
static char *thumb_url(J *arr)
{
    J          *t, *last = NULL;
    const char *u;
    const char *eq;
    for (t = arr ? arr->child : NULL; t; t = t->next) last = t;
    u = JStr(last, "url");
    if (!u || strncmp(u, "https://", 8)) return NULL;
    eq = strrchr(u, '=');
    if (eq && eq[1] == 'w' && strstr(eq, "-h")) {
        Buf b = {0};
        BufAdd(&b, u, (size_t)(eq - u));
        BufAddf(&b, "=w%d-h%d-l90-rj", g_cfg.thumbSize, g_cfg.thumbSize);
        return BufTake(&b);
    }
    return U8Dup(u);
}

static void add_track(TrackList *t, J *r)
{
    const char *vid = JStr(r, "playlistItemData.videoId");
    const char *pol = JStr(r, "musicItemRendererDisplayPolicy");
    J          *fx;
    Track      *k;
    if (!vid) {
        J *we = JFind(r, "watchEndpoint");
        vid = JStr(we, "videoId");
    }
    if (!vid || strlen(vid) >= sizeof(k->vid)) return;     /* 削除された曲など */
    if (t->n == t->cap) {
        int    cap = t->cap ? t->cap * 2 : 128;
        Track *v = (Track *)realloc(t->v, sizeof(Track) * cap);
        if (!v) return;
        t->v = v;
        t->cap = cap;
    }
    k = &t->v[t->n++];
    memset(k, 0, sizeof(*k));
    strcpy_s(k->vid, sizeof(k->vid), vid);
    k->title  = flex_text(r, 0);
    k->artist = flex_text(r, 1);
    k->album  = flex_text(r, 2);
    fx = JPath(r, "fixedColumns.0.musicResponsiveListItemFixedColumnRenderer.text");
    if (fx) {
        WCHAR *d = JText(fx);
        k->dur = parse_duration(d);
        free(d);
    }
    k->grey = pol && !strcmp(pol, "MUSIC_ITEM_RENDERER_DISPLAY_POLICY_GREY_OUT");
    k->thumb = thumb_url(JPath(r, "thumbnail.musicThumbnailRenderer.thumbnail.thumbnails"));
}

/* 続きの合図: 新しい形(continuationItemRenderer)と古い形(nextContinuationData) */
static char *new_style_token(J *item)
{
    const char *tok = JStr(item, "continuationItemRenderer.continuationEndpoint.continuationCommand.token");
    if (!tok) {
        J *cmds = JPath(item, "continuationItemRenderer.continuationEndpoint.commandExecutorCommand.commands"), *c;
        for (c = cmds ? cmds->child : NULL; c && !tok; c = c->next) tok = JStr(c, "continuationCommand.token");
    }
    return tok ? U8Dup(tok) : NULL;
}

static void page_query(Buf *q, Buf *tail, const char *browseId, const char *tok, BOOL oldStyle)
{
    q->n = 0;
    tail->n = 0;
    BufAdd(q, "", 0);
    if (!tok) {
        BufAddStr(tail, "\"browseId\":");
        BufAddJsonStr(tail, browseId);
    } else if (oldStyle) {
        BufAddf(q, "&ctoken=%s&continuation=%s&type=next", tok, tok);
        BufAddStr(tail, "\"browseId\":");
        BufAddJsonStr(tail, browseId);
    } else {
        BufAddStr(tail, "\"continuation\":");
        BufAddJsonStr(tail, tok);
    }
}

int yt_playlist(const char *id, TrackList *out, BOOL useAccount, WCHAR **title, char *err, size_t errlen)
{
    Buf   q = {0}, tail = {0};
    char  browseId[160];
    char *tok = NULL;
    BOOL  oldStyle = FALSE;
    int   page, ok = 1;

    memset(out, 0, sizeof(*out));
    err[0] = 0;
    if (title) *title = NULL;
    if (useAccount && !g_cfg.cookie) useAccount = FALSE;
    _snprintf(browseId, sizeof(browseId), "VL%s", id);
    browseId[sizeof(browseId) - 1] = 0;
    for (page = 0; page < MAX_PAGES; page++) {
        JDoc *doc;
        J    *root, *cont, *items = NULL, *it;
        int   before = out->n;
        char *next = NULL;

        if (page) Sleep(250);                  /* ブラウザでスクロールしたときより遅く */
        page_query(&q, &tail, browseId, tok, oldStyle);
        doc = music_post("browse", q.p, tail.p, useAccount, err, errlen);
        if (!doc) { ok = 0; break; }
        root = JsonRoot(doc);
        if (page == 0 && title) {
            J *hdr = JFind(root, "musicResponsiveHeaderRenderer");
            if (!hdr) hdr = JFind(root, "musicEditablePlaylistDetailHeaderRenderer");
            *title = JText(JGet(hdr, "title"));
            if (!*title) *title = JText(JFind(hdr, "title"));
            if (!*title) {
                const char *t = JStr(root, "microformat.microformatDataRenderer.title");
                if (t) *title = U2W(t);
            }
        }
        if ((cont = JFind(root, "musicPlaylistShelfRenderer")) != NULL) items = JGet(cont, "contents");
        else if ((cont = JFind(root, "appendContinuationItemsAction")) != NULL) items = JGet(cont, "continuationItems");
        else if ((cont = JFind(root, "musicPlaylistShelfContinuation")) != NULL) items = JGet(cont, "contents");
        else if ((cont = JFind(root, "musicShelfRenderer")) != NULL) items = JGet(cont, "contents");
        for (it = items ? items->child : NULL; it; it = it->next) {
            J *r = JGet(it, "musicResponsiveListItemRenderer");
            if (r) add_track(out, r);
            else if (JGet(it, "continuationItemRenderer")) { free(next); next = new_style_token(it); oldStyle = FALSE; }
        }
        if (!next && cont) {
            const char *old = JStr(cont, "continuations.0.nextContinuationData.continuation");
            if (old) { next = U8Dup(old); oldStyle = TRUE; }
        }
        if (page == 0 && !items) {
            const char *msg = JStr(root, "alerts.0.alertWithButtonRenderer.text.runs.0.text");
            _snprintf(err, errlen, "プレイリストを読めなかった%s%s", msg ? ": " : "", msg ? msg : "(非公開か、存在しない)");
            ok = 0;
        }
        JsonFree(doc);
        free(tok);
        tok = next;
        if (!ok || !tok || out->n == before) break;
        if (g_progressHook) g_progressHook(id, out->n);     /* 続きがあるときだけ(読み終わりは結果で知らせる) */
    }
    free(tok);
    BufFree(&q);
    BufFree(&tail);
    err[errlen - 1] = 0;
    log_printf("playlist %s: %d tracks, %d pages", id, out->n, page + 1);
    return ok;
}

static void add_playlist(PlaylistList *p, const char *browseId, WCHAR *title, WCHAR *sub)
{
    Playlist *k;
    int       i;
    if (!browseId || strncmp(browseId, "VL", 2) || !browseId[2]) { free(title); free(sub); return; }
    for (i = 0; i < p->n; i++)
        if (!strcmp(p->v[i].id, browseId + 2)) { free(title); free(sub); return; }
    if (p->n == p->cap) {
        int       cap = p->cap ? p->cap * 2 : 32;
        Playlist *v = (Playlist *)realloc(p->v, sizeof(Playlist) * cap);
        if (!v) { free(title); free(sub); return; }
        p->v = v;
        p->cap = cap;
    }
    k = &p->v[p->n++];
    k->id = U8Dup(browseId + 2);
    k->title = title ? title : WDup(L"(名前なし)");
    k->sub = sub;
}

int yt_library(PlaylistList *out, char *err, size_t errlen)
{
    Buf   q = {0}, tail = {0};
    char *tok = NULL;
    BOOL  oldStyle = FALSE, retried = FALSE;
    int   page, ok = 1;

    memset(out, 0, sizeof(*out));
    err[0] = 0;
    if (!g_cfg.cookie && !reload_account()) {
        _snprintf(err, errlen, "%s", g_cfg.cookieErr[0] ? g_cfg.cookieErr : "ログイン情報が設定されていない");
        err[errlen - 1] = 0;
        if (g_cfg.cookieSource != COOKIE_NONE) log_always("account library: no login cookies: %s", err);
        return 0;
    }
    for (page = 0; page < 20; page++) {
        JDoc *doc;
        J    *root, *cont = NULL, *items = NULL, *it;
        char *next = NULL;
        int   before = out->n;

        if (page) Sleep(250);
        page_query(&q, &tail, "FEmusic_liked_playlists", tok, oldStyle);
        doc = music_post("browse", q.p, tail.p, TRUE, err, errlen);
        if (!doc) { ok = 0; break; }
        root = JsonRoot(doc);
        if (page == 0 && logged_in_flag(root) == 0) {
            JsonFree(doc);
            if (!retried && reload_account()) { retried = TRUE; page = -1; continue; }   /* Firefox から読み直して 1 回だけ */
            _snprintf(err, errlen, g_cfg.cookieSource == COOKIE_FIREFOX
                      ? "ログインできていない。Firefox で music.youtube.com にログインしているか確かめてください"
                      : "ログインできていない(cookie が無効か期限切れ)。ログイン情報を設定し直してください");
            ok = 0;
            break;
        }
        if ((cont = JFind(root, "gridRenderer")) != NULL) items = JGet(cont, "items");
        else if ((cont = JFind(root, "gridContinuation")) != NULL) items = JGet(cont, "items");
        else if ((cont = JFind(root, "appendContinuationItemsAction")) != NULL) items = JGet(cont, "continuationItems");
        else if ((cont = JFind(root, "musicShelfRenderer")) != NULL) items = JGet(cont, "contents");
        for (it = items ? items->child : NULL; it; it = it->next) {
            J *m = JGet(it, "musicTwoRowItemRenderer");
            J *r = JGet(it, "musicResponsiveListItemRenderer");
            if (m) {
                const char *bid = JStr(m, "title.runs.0.navigationEndpoint.browseEndpoint.browseId");
                if (!bid) bid = JStr(m, "navigationEndpoint.browseEndpoint.browseId");
                add_playlist(out, bid, JText(JGet(m, "title")), JText(JGet(m, "subtitle")));
            } else if (r) {
                J *be = JFind(r, "browseEndpoint");
                add_playlist(out, JStr(be, "browseId"), flex_text(r, 0), flex_text(r, 1));
            } else if (JGet(it, "continuationItemRenderer")) {
                free(next);
                next = new_style_token(it);
                oldStyle = FALSE;
            }
        }
        if (!next && cont) {
            const char *old = JStr(cont, "continuations.0.nextContinuationData.continuation");
            if (old) { next = U8Dup(old); oldStyle = TRUE; }
        }
        JsonFree(doc);
        free(tok);
        tok = next;
        if (!tok || out->n == before) break;
    }
    free(tok);
    BufFree(&q);
    BufFree(&tail);
    err[errlen - 1] = 0;
    log_printf("library: %d playlists", out->n);
    return ok;
}

/* ------------------------------------------------------------------ */
/*  再生: 匿名の player と GVS                                          */
/* ------------------------------------------------------------------ */

static void start_cooldown(int sec, char *err, size_t errlen, const char *why)
{
    if (sec < COOLDOWN_SEC) sec = COOLDOWN_SEC;
    g_cfg.cooldownUntil = unix_time() + sec;
    cfg_save_cooldown();
    _snprintf(err, errlen, "%s。安全のため %d 分間、再生の要求を止めます", why, (sec + 59) / 60);
    err[errlen - 1] = 0;
    log_printf("cooldown %d sec: %s", sec, why);
}

static void (*g_waitHook)(int sec);

void yt_set_wait_hook(void (*fn)(int sec)) { g_waitHook = fn; }

/* 間隔と回数の上限を守るために待つ。長く待つときは g_waitHook で画面に知らせる
   (2026-10-12: 知らせずに最大 10 分待ち、利用者には「読み込み中…」のまま止まって見えた) */
static BOOL rate_wait(volatile LONG *cancel)
{
    BOOL told = FALSE;
    for (;;) {
        LONG64 now = now_ms(), wait = 0;
        LONG64 last = g_hist[(g_histPos + WINDOW_MAX - 1) % WINDOW_MAX];
        LONG64 oldest = g_hist[g_histPos];          /* 20 回前 */
        if (last && now - last < MIN_GAP_MS) wait = MIN_GAP_MS - (now - last);
        if (oldest && now - oldest < WINDOW_MS && WINDOW_MS - (now - oldest) > wait) wait = WINDOW_MS - (now - oldest);
        if (wait <= 0) break;
        if (wait > 3000) {
            log_printf("rate limit: wait %lld ms", wait);
            if (g_waitHook) { g_waitHook((int)((wait + 999) / 1000)); told = TRUE; }
        }
        while (wait > 0) {
            if (cancel && *cancel) { if (told && g_waitHook) g_waitHook(0); return FALSE; }
            Sleep(wait > 100 ? 100 : (DWORD)wait);
            wait -= 100;
        }
    }
    if (told && g_waitHook) g_waitHook(0);
    g_hist[g_histPos] = now_ms();
    g_histPos = (g_histPos + 1) % WINDOW_MAX;
    return TRUE;
}

static J *pick_format(J *root)
{
    J *fmts = JPath(root, "streamingData.adaptiveFormats"), *f, *best = NULL;
    int bestScore = -1;
    for (f = fmts ? fmts->child : NULL; f; f = f->next) {
        int itag = (int)JNum(f, "itag", 0), score;
        J  *at = JGet(f, "audioTrack");
        if (!JStr(f, "url")) continue;                          /* signatureCipher は扱わない */
        if (itag == 140) score = 20;                            /* AAC-LC 128k */
        else if (itag == 139) score = 10;                       /* HE-AAC 48k */
        else continue;                                          /* Opus(WebM)は Windows が素で読めない */
        if (at && JGet(at, "audioIsDefault") && !JNum(at, "audioIsDefault", 0)) score -= 5;   /* 吹き替え */
        if (score > bestScore) { bestScore = score; best = f; }
    }
    return best;
}

int yt_fetch_audio(const char *vid, AudioData *out, volatile LONG *cancel, char *err, size_t errlen)
{
    Buf         h = {0}, body = {0}, gh = {0}, url = {0};
    HttpResp    r;
    JDoc       *doc = NULL;
    J          *root, *f;
    const char *ps, *reason, *furl;
    LONG64      clen;
    char        hl[4], gl[4];
    int         code = YT_NET;
    BYTE       *dst;
    DWORD       off = 0;

    memset(out, 0, sizeof(*out));
    err[0] = 0;
    if (g_cfg.cooldownUntil > unix_time()) {
        LONG64 left = g_cfg.cooldownUntil - unix_time();
        _snprintf(err, errlen, "YouTube に拒まれたため、あと %lld 分は再生の要求を止めています", (left + 59) / 60);
        return YT_STOP;
    }
    if (!ensure_guest(cancel, err, errlen)) return (cancel && *cancel) ? YT_CANCEL : YT_NET;
    if (!rate_wait(cancel)) return YT_CANCEL;

    locale_hl_gl(hl, gl);
    BufAddStr(&body, "{\"context\":{\"client\":{\"clientName\":\"" VIS_NAME "\",\"clientVersion\":\"" VIS_VERSION "\","
                     "\"deviceMake\":\"Apple\",\"deviceModel\":\"RealityDevice17,1\",\"userAgent\":\"" VIS_UA "\","
                     "\"osName\":\"visionOS\",\"osVersion\":\"26.5.23O471\"");
    BufAddf(&body, ",\"hl\":\"%s\",\"gl\":\"%s\"", hl, gl);
    if (g_cfg.visitor[0]) { BufAddStr(&body, ",\"visitorData\":"); BufAddJsonStr(&body, g_cfg.visitor); }
    BufAddStr(&body, "}},\"videoId\":");
    BufAddJsonStr(&body, vid);
    BufAddStr(&body, ",\"playbackContext\":{\"contentPlaybackContext\":{\"html5Preference\":\"HTML5_PREF_WANTS\"}},"
                     "\"contentCheckOk\":true,\"racyCheckOk\":true}");

    BufAddStr(&h, "Content-Type: application/json\r\nUser-Agent: " VIS_UA "\r\nAccept: */*\r\n");
    BufAddf(&h, "Accept-Language: %s\r\n", hl);
    BufAddStr(&h, "X-YouTube-Client-Name: " VIS_NAME_NUM "\r\nX-YouTube-Client-Version: " VIS_VERSION "\r\n");
    BufAddStr(&h, "Origin: https://www.youtube.com\r\n");
    if (g_cfg.visitor[0]) BufAddf(&h, "X-Goog-Visitor-Id: %s\r\n", g_cfg.visitor);
    if (g_cfg.guest[0]) BufAddf(&h, "Cookie: %s\r\n", g_cfg.guest);

    log_printf("player %s", vid);
    if (!HttpPost(PLAYER_API, h.p, body.p, &r, err, errlen)) goto done;
    log_printf("  -> HTTP %d, %u bytes", r.status, (unsigned)r.body.n);
    note_guest_cookies(r.setCookie);
    if (r.status == 429 || r.status == 403) {
        start_cooldown(r.retryAfter, err, errlen, r.status == 429 ? "要求が多すぎると言われた(HTTP 429)" : "再生の要求が拒否された(HTTP 403)");
        HttpRespFree(&r);
        code = YT_STOP;
        goto done;
    }
    if (r.status != 200) {
        _snprintf(err, errlen, "YouTube が HTTP %d を返した", r.status);
        HttpRespFree(&r);
        code = YT_RETRY;
        goto done;
    }
    doc = JsonParse(r.body.p, r.body.n);
    HttpRespFree(&r);
    if (!doc) { _snprintf(err, errlen, "応答を読めなかった"); code = YT_RETRY; goto done; }
    root = JsonRoot(doc);

    if (!g_cfg.visitor[0]) {
        const char *v = JStr(root, "responseContext.visitorData");
        if (v && strlen(v) < sizeof(g_cfg.visitor)) { strcpy_s(g_cfg.visitor, sizeof(g_cfg.visitor), v); cfg_save_guest(); }
    }

    ps = JStr(root, "playabilityStatus.status");
    reason = JStr(root, "playabilityStatus.reason");
    if (!ps || strcmp(ps, "OK")) {
        log_printf("  playability %s: %s", ps ? ps : "(none)", reason ? reason : "(no reason)");
        if (reason && (U8FindI(reason, "bot") || strstr(reason, "ボット"))) {
            start_cooldown(0, err, errlen, "「ボットではないことを確認」を求められた");
            code = YT_STOP;
        } else {
            _snprintf(err, errlen, "再生できない曲: %s", reason ? reason : (ps ? ps : "理由不明"));
            code = YT_SKIP;
        }
        goto done;
    }
    f = pick_format(root);
    if (!f) { _snprintf(err, errlen, "再生できる音声の形式(AAC)が無かった"); code = YT_SKIP; goto done; }
    furl = JStr(f, "url");
    clen = _atoi64(JStr(f, "contentLength") ? JStr(f, "contentLength") : "0");
    if (clen <= 0) {
        const char *c = strstr(furl, "clen=");
        if (c) clen = _atoi64(c + 5);
    }
    if (clen <= 0 || clen > 512LL * 1024 * 1024) { _snprintf(err, errlen, "音声の大きさが分からなかった"); code = YT_SKIP; goto done; }
    out->itag  = (int)JNum(f, "itag", 0);
    out->durMs = (int)JNum(f, "approxDurationMs", 0);

    /* 丸ごと取る(CHUNK ずつ range で)。GMEM_MOVEABLE は CreateStreamOnHGlobal の条件 */
    out->h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)clen);
    if (!out->h) { _snprintf(err, errlen, "メモリが足りない"); code = YT_SKIP; goto done; }
    dst = (BYTE *)GlobalLock(out->h);
    BufAddStr(&gh, "User-Agent: " VIS_UA "\r\nAccept: */*\r\n");
    BufAddf(&gh, "Accept-Language: %s\r\n", hl);
    log_printf("  itag %d, %lld bytes", out->itag, clen);
    while (off < (DWORD)clen) {
        DWORD want = (DWORD)clen - off, got = 0;
        int   st = 0;
        if (want > CHUNK) want = CHUNK;
        url.n = 0;
        BufAddf(&url, "%s&range=%lu-%lu", furl, off, off + want - 1);
        if (!HttpGetRange(url.p, gh.p, dst + off, want, &got, &st, cancel, err, errlen)) {
            code = (cancel && *cancel) ? YT_CANCEL : YT_NET;
            break;
        }
        if (st == 429) { start_cooldown(0, err, errlen, "配信サーバーに要求が多すぎると言われた(HTTP 429)"); code = YT_STOP; break; }
        if (st != 200 && st != 206) { _snprintf(err, errlen, "配信サーバーが HTTP %d を返した", st); code = YT_RETRY; break; }
        if (got != want) { _snprintf(err, errlen, "音声が途中で切れた(%lu / %lu)", got, want); code = YT_RETRY; break; }
        off += got;
    }
    GlobalUnlock(out->h);
    if (off == (DWORD)clen) {
        out->len = (DWORD)clen;
        code = YT_OK;
        log_printf("  done");
    } else {
        GlobalFree(out->h);
        out->h = NULL;
        log_printf("  failed: %s", err);
    }

done:
    err[errlen - 1] = 0;
    JsonFree(doc);
    BufFree(&h);
    BufFree(&body);
    BufFree(&gh);
    BufFree(&url);
    return code;
}

/* ジャケット画像を取る(画像のサーバーへ。cookie は付けない。曲が変わったときに 1 回だけ) */
int yt_fetch_thumb(const char *url, Buf *out, char *err, size_t errlen)
{
    HttpResp r;
    memset(out, 0, sizeof(*out));
    if (!HttpGet(url, "User-Agent: " DEFAULT_UA "\r\nAccept: image/avif,image/webp,image/*,*/*;q=0.8\r\n", &r, err, errlen)) return 0;
    if (r.status != 200 || r.body.n == 0 || r.body.n > 512 * 1024) {
        _snprintf(err, errlen, "画像: HTTP %d、%u バイト", r.status, (unsigned)r.body.n);
        HttpRespFree(&r);
        return 0;
    }
    *out = r.body;
    memset(&r.body, 0, sizeof(r.body));
    HttpRespFree(&r);
    log_printf("thumbnail %u bytes", (unsigned)out->n);
    return 1;
}

void yt_free_tracks(TrackList *t)
{
    int i;
    for (i = 0; i < t->n; i++) {
        free(t->v[i].title);
        free(t->v[i].artist);
        free(t->v[i].album);
        free(t->v[i].thumb);
    }
    free(t->v);
    memset(t, 0, sizeof(*t));
}

void yt_free_playlists(PlaylistList *p)
{
    int i;
    for (i = 0; i < p->n; i++) {
        free(p->v[i].id);
        free(p->v[i].title);
        free(p->v[i].sub);
    }
    free(p->v);
    memset(p, 0, sizeof(*p));
}
