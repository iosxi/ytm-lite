/* ==================================================================
 * ffcookie.c - Firefox の cookie(cookies.sqlite)から YouTube の cookie を読む
 *
 *  yt-dlp の --cookies-from-browser firefox と同じことを、SQLite の読むだけの
 *  小さな解析でやる(SQLite 本体を入れると exe が数倍になるので)。
 *
 *  - Chrome は 127 から cookie の鍵が Chrome 本体に縛られ(App-Bound Encryption)、
 *    外からは読めない。yt-dlp も Windows の Chrome からは読めなくなっている。
 *    Firefox は暗号化していない。
 *  - Firefox が動いている間も読める(共有で開く)。SQLite のロックは 1GB の位置の
 *    バイトにかけるので、それより小さいファイルの中身は読める。
 *  - 書き戻し前の変更は cookies.sqlite-wal にある。yt-dlp は本体だけをコピーして
 *    読むので取りこぼすが、ここでは WAL の確定済みのフレームを重ねて読む。
 *  - コンテナや、ほかのサイトに埋め込まれた YouTube の cookie(originAttributes が
 *    空でないもの)は使わない。期限切れも使わない。
 *  - 読むだけ。Firefox のファイルには何も書かない。
 * ================================================================== */

#include "ytm.h"
#include <shlobj.h>
#include <tlhelp32.h>

typedef struct {
    BYTE  *db;
    DWORD  dbLen;
    DWORD  pageSize, usable;
    BYTE  *wal;
    DWORD  walLen;
    DWORD *walPg;           /* walPg[i] = ページ番号 i の最新の確定フレームの位置(0 = 無し) */
    DWORD  walPgN;
} Sq;

static DWORD be32(const BYTE *p) { return ((DWORD)p[0] << 24) | ((DWORD)p[1] << 16) | ((DWORD)p[2] << 8) | p[3]; }
static DWORD be16(const BYTE *p) { return ((DWORD)p[0] << 8) | p[1]; }

static BYTE *read_all(const WCHAR *path, DWORD *len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD  size, got = 0;
    BYTE  *p;
    *len = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 256u * 1024 * 1024) { CloseHandle(h); return NULL; }
    p = (BYTE *)malloc(size ? size : 1);
    if (p && size && !ReadFile(h, p, size, &got, NULL)) got = 0;
    CloseHandle(h);
    if (!p) return NULL;
    *len = got;
    return p;
}

/* WAL のチェックサム(SQLite の walChecksumBytes と同じ) */
static void wal_cksum(BOOL bigEndian, const BYTE *p, DWORD n, DWORD *s1, DWORD *s2)
{
    DWORD i;
    for (i = 0; i + 8 <= n; i += 8) {
        DWORD a, b;
        if (bigEndian) { a = be32(p + i); b = be32(p + i + 4); }
        else { a = *(const DWORD *)(p + i); b = *(const DWORD *)(p + i + 4); }
        *s1 += a + *s2;
        *s2 += b + *s1;
    }
}

/* 確定済みのフレームだけを、ページ番号ごとに最新のものとして控える */
static void load_wal(Sq *q)
{
    DWORD magic, ps, salt1, salt2, s1, s2, off, frameSize, maxPg = 0;
    DWORD *tmp;
    BOOL  be;
    if (!q->wal || q->walLen < 32) return;
    magic = be32(q->wal);
    if (magic != 0x377f0682 && magic != 0x377f0683) return;
    be = (magic & 1) != 0;
    ps = be32(q->wal + 8);
    if (ps != q->pageSize) return;
    salt1 = be32(q->wal + 16);
    salt2 = be32(q->wal + 20);
    s1 = s2 = 0;
    wal_cksum(be, q->wal, 24, &s1, &s2);
    if (s1 != be32(q->wal + 24) || s2 != be32(q->wal + 28)) return;
    frameSize = 24 + ps;
    for (off = 32; off + frameSize <= q->walLen; off += frameSize) {
        DWORD pg = be32(q->wal + off);
        if (pg > maxPg) maxPg = pg;
    }
    if (!maxPg) return;
    q->walPg = (DWORD *)calloc((size_t)maxPg + 1, sizeof(DWORD));
    tmp = (DWORD *)calloc((size_t)maxPg + 1, sizeof(DWORD));
    if (!q->walPg || !tmp) { free(tmp); return; }
    q->walPgN = maxPg + 1;
    for (off = 32; off + frameSize <= q->walLen; off += frameSize) {
        const BYTE *f = q->wal + off;
        DWORD pg = be32(f), commit = be32(f + 4);
        if (be32(f + 8) != salt1 || be32(f + 12) != salt2) break;   /* 前の世代の残り */
        wal_cksum(be, f, 8, &s1, &s2);
        wal_cksum(be, f + 24, ps, &s1, &s2);
        if (s1 != be32(f + 16) || s2 != be32(f + 20)) break;        /* 書きかけ */
        if (pg && pg <= maxPg) tmp[pg] = off + 24;
        if (commit) memcpy(q->walPg, tmp, sizeof(DWORD) * q->walPgN);   /* ここまでが確定 */
    }
    free(tmp);
}

static const BYTE *page(Sq *q, DWORD pg)
{
    if (!pg) return NULL;
    if (pg < q->walPgN && q->walPg[pg]) return q->wal + q->walPg[pg];
    if ((unsigned long long)pg * q->pageSize > q->dbLen) return NULL;
    return q->db + (size_t)(pg - 1) * q->pageSize;
}

static int varint(const BYTE *p, const BYTE *end, unsigned long long *v)
{
    int i;
    *v = 0;
    for (i = 0; i < 8; i++) {
        if (p + i >= end) return 0;
        *v = (*v << 7) | (p[i] & 0x7f);
        if (!(p[i] & 0x80)) return i + 1;
    }
    if (p + 8 >= end) return 0;
    *v = (*v << 8) | p[8];
    return 9;
}

/* セルの中身(はみ出し分を含む)を連続したバッファにして返す */
static BYTE *cell_payload(Sq *q, const BYTE *pg, const BYTE *cell, DWORD *outLen)
{
    const BYTE *end = pg + q->pageSize;
    unsigned long long plen, rowid;
    int    n1, n2;
    DWORD  U = q->usable, X = U - 35, M, K, local;
    BYTE  *buf;
    (void)pg;
    n1 = varint(cell, end, &plen);
    if (!n1) return NULL;
    n2 = varint(cell + n1, end, &rowid);
    if (!n2 || plen > 64 * 1024 * 1024) return NULL;
    cell += n1 + n2;
    if (plen <= X) local = (DWORD)plen;
    else {
        M = ((U - 12) * 32 / 255) - 23;
        K = M + (DWORD)((plen - M) % (U - 4));
        local = K <= X ? K : M;
    }
    if (cell + local > end) return NULL;
    buf = (BYTE *)malloc((size_t)plen + 1);
    if (!buf) return NULL;
    memcpy(buf, cell, local);
    if (local < plen) {
        DWORD ov = be32(cell + local), got = local, guard = 0;
        while (ov && got < plen && guard++ < 100000) {
            const BYTE *op = page(q, ov);
            DWORD take = U - 4;
            if (!op) break;
            if (take > plen - got) take = (DWORD)(plen - got);
            memcpy(buf + got, op + 4, take);
            got += take;
            ov = be32(op);
        }
        if (got < plen) { free(buf); return NULL; }
    }
    *outLen = (DWORD)plen;
    return buf;
}

/* レコードの col 番目の値。種類(0=NULL、1=整数、3=文字列)を返す */
typedef struct { int kind; long long i; const char *s; DWORD n; } Val;

static BOOL record_col(const BYTE *rec, DWORD len, int col, Val *v)
{
    const BYTE *end = rec + len, *h;
    unsigned long long hsize, st;
    DWORD off;
    int   n, c;
    memset(v, 0, sizeof(*v));
    n = varint(rec, end, &hsize);
    if (!n || hsize > len) return FALSE;
    h = rec + n;
    off = (DWORD)hsize;
    for (c = 0; h < rec + hsize; c++) {
        DWORD sz;
        n = varint(h, rec + hsize, &st);
        if (!n) return FALSE;
        h += n;
        if (st >= 12) sz = (DWORD)((st - (st & 1 ? 13 : 12)) / 2);
        else { static const BYTE z[10] = { 0, 1, 2, 3, 4, 6, 8, 8, 0, 0 }; sz = z[st < 10 ? st : 0]; }
        if (c == col) {
            if (off + sz > len) return FALSE;
            if (st == 0) v->kind = 0;
            else if (st >= 1 && st <= 6) {
                long long x = (signed char)rec[off];
                DWORD k;
                for (k = 1; k < sz; k++) x = (x << 8) | rec[off + k];
                v->kind = 1; v->i = x;
            } else if (st == 8 || st == 9) { v->kind = 1; v->i = st == 9; }
            else if (st >= 13 && (st & 1)) { v->kind = 3; v->s = (const char *)rec + off; v->n = sz; }
            return TRUE;
        }
        off += sz;
    }
    return TRUE;    /* 列が無い(後から足された列) = NULL */
}

typedef void (*RowFn)(void *ctx, const BYTE *rec, DWORD len);

static void walk_table(Sq *q, DWORD pg, int depth, RowFn fn, void *ctx)
{
    const BYTE *p = page(q, pg), *hdr;
    DWORD ncell, i;
    BYTE  type;
    if (!p || depth > 30) return;
    hdr = p + (pg == 1 ? 100 : 0);
    type = hdr[0];
    ncell = be16(hdr + 3);
    if (type == 0x0D) {                     /* 葉 */
        for (i = 0; i < ncell; i++) {
            DWORD co = be16(hdr + 8 + i * 2), len = 0;
            BYTE *rec;
            if (co >= q->pageSize) continue;
            rec = cell_payload(q, p, p + co, &len);
            if (rec) { fn(ctx, rec, len); free(rec); }
        }
    } else if (type == 0x05) {              /* 内部 */
        for (i = 0; i < ncell; i++) {
            DWORD co = be16(hdr + 12 + i * 2);
            if (co + 4 <= q->pageSize) walk_table(q, be32(p + co), depth + 1, fn, ctx);
        }
        walk_table(q, be32(hdr + 8), depth + 1, fn, ctx);
    }
}

/* sqlite_schema から moz_cookies の根のページと列の並びを得る */
typedef struct { DWORD root; char *sql; } Schema;

static void schema_row(void *ctx, const BYTE *rec, DWORD len)
{
    Schema *s = (Schema *)ctx;
    Val type, name, root, sql;
    if (!record_col(rec, len, 0, &type) || !record_col(rec, len, 1, &name) ||
        !record_col(rec, len, 3, &root) || !record_col(rec, len, 4, &sql)) return;
    if (type.kind == 3 && type.n == 5 && !memcmp(type.s, "table", 5) &&
        name.kind == 3 && name.n == 11 && !memcmp(name.s, "moz_cookies", 11) && root.kind == 1 && sql.kind == 3) {
        s->root = (DWORD)root.i;
        free(s->sql);
        s->sql = U8NDup(sql.s, sql.n);
    }
}

/* CREATE TABLE moz_cookies (id INTEGER PRIMARY KEY, originAttributes TEXT ..., name TEXT, ...) */
static int column_index(const char *sql, const char *col)
{
    const char *p = strchr(sql, '(');
    int idx = 0;
    size_t cl = strlen(col);
    if (!p) return -1;
    p++;
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
        if (!_strnicmp(p, "CONSTRAINT", 10) || !_strnicmp(p, "PRIMARY KEY", 11) || !_strnicmp(p, "UNIQUE", 6)) return -1;
        if (!_strnicmp(p, col, cl) && (p[cl] == ' ' || p[cl] == ',' || p[cl] == ')')) return idx;
        /* 次の列へ(括弧の中のカンマは飛ばす) */
        {
            int depth = 0;
            for (; *p; p++) {
                if (*p == '(') depth++;
                else if (*p == ')') { if (!depth) return -1; depth--; }
                else if (*p == ',' && !depth) { p++; break; }
            }
        }
        idx++;
    }
    return -1;
}

/* originAttributes ごとの束。'' はふつうの cookie、'^userContextId=2' はコンテナ、
   '^firstPartyDomain=youtube.com' はサイトごとの分離、'^partitionKey=…' はほかのサイトに埋め込まれた分 */
#define MAX_GROUPS 16
typedef struct {
    char   origin[192];
    Buf    out, music;     /* music.youtube.com だけの cookie は先に入れる(同名なら先のものが使われる) */
    int    count;
    BOOL   partition;
} Group;

typedef struct {
    int    cName, cValue, cHost, cExpiry, cOrigin;
    LONG64 now;
    Group  g[MAX_GROUPS];
    int    ng;
    int    rows, expired;  /* 診断用: YouTube の行の数、期限切れで捨てた数 */
} Collect;

static Group *group_for(Collect *c, const char *o, DWORD n)
{
    int i;
    for (i = 0; i < c->ng; i++)
        if (strlen(c->g[i].origin) == n && !memcmp(c->g[i].origin, o, n)) return &c->g[i];
    if (c->ng >= MAX_GROUPS || n >= sizeof(c->g[0].origin)) return NULL;
    memcpy(c->g[c->ng].origin, o, n);
    c->g[c->ng].origin[n] = 0;
    c->g[c->ng].partition = strstr(c->g[c->ng].origin, "partitionKey=") != NULL;
    return &c->g[c->ng++];
}

static void cookie_row(void *ctx, const BYTE *rec, DWORD len)
{
    Collect *c = (Collect *)ctx;
    Val name, value, host, expiry, origin;
    BOOL music;
    Group *g;
    Buf *dst;
    if (!record_col(rec, len, c->cName, &name) || !record_col(rec, len, c->cValue, &value) ||
        !record_col(rec, len, c->cHost, &host) || !record_col(rec, len, c->cExpiry, &expiry) ||
        !record_col(rec, len, c->cOrigin, &origin)) return;
    if (host.kind != 3 || name.kind != 3 || value.kind != 3 || !name.n) return;
    music = host.n == 17 && !memcmp(host.s, "music.youtube.com", 17);
    if (!music && !(host.n == 12 && !memcmp(host.s, ".youtube.com", 12)) &&
        !(host.n == 11 && !memcmp(host.s, "youtube.com", 11))) return;
    c->rows++;
    if (expiry.kind == 1) {
        /* 期限の単位は値の大きさで見分ける(版 16 からミリ秒だが、版で決め打ちすると
           秒のまま残った行を全部「期限切れ」にしてしまうので)。1e11 秒は西暦 5000 年より先 */
        LONG64 e = expiry.i > 100000000000LL ? expiry.i / 1000 : expiry.i;
        if (e && e < c->now) { c->expired++; return; }
    }
    g = group_for(c, origin.kind == 3 ? origin.s : "", origin.kind == 3 ? origin.n : 0);
    if (!g) return;
    dst = music ? &g->music : &g->out;
    if (dst->n) BufAddStr(dst, "; ");
    BufAdd(dst, name.s, name.n);
    BufAddChar(dst, '=');
    BufAdd(dst, value.s, value.n);
    g->count++;
}

static char *group_cookie(Group *g)
{
    Buf all = {0};
    if (g->music.n) BufAddStr(&all, g->music.p);
    if (g->out.n) { if (all.n) BufAddStr(&all, "; "); BufAddStr(&all, g->out.p); }
    return all.p;
}

/* 1 つのプロファイルのフォルダから読む。成功したら "a=1; b=2" */
char *ff_read_youtube_cookies(const WCHAR *profileDir, int *count, char *err, size_t errlen)
{
    WCHAR   path[MAX_PATH];
    Sq      q;
    Schema  sch = { 0, NULL };
    Collect c;
    char   *res = NULL;
    DWORD   userVersion;

    memset(&q, 0, sizeof(q));
    memset(&c, 0, sizeof(c));
    *count = 0;
    _snwprintf(path, MAX_PATH, L"%s\\cookies.sqlite", profileDir);
    path[MAX_PATH - 1] = 0;
    q.db = read_all(path, &q.dbLen);
    if (!q.db || q.dbLen < 100 || memcmp(q.db, "SQLite format 3", 16)) {
        _snprintf(err, errlen, "Firefox の cookies.sqlite を読めなかった");
        goto done;
    }
    q.pageSize = be16(q.db + 16);
    if (q.pageSize == 1) q.pageSize = 65536;
    q.usable = q.pageSize - q.db[20];
    userVersion = be32(q.db + 60);
    _snwprintf(path, MAX_PATH, L"%s\\cookies.sqlite-wal", profileDir);
    path[MAX_PATH - 1] = 0;
    q.wal = read_all(path, &q.walLen);
    load_wal(&q);

    walk_table(&q, 1, 0, schema_row, &sch);
    if (!sch.root || !sch.sql) { _snprintf(err, errlen, "Firefox の cookie の表が見つからなかった"); goto done; }
    c.cName   = column_index(sch.sql, "name");
    c.cValue  = column_index(sch.sql, "value");
    c.cHost   = column_index(sch.sql, "host");
    c.cExpiry = column_index(sch.sql, "expiry");
    c.cOrigin = column_index(sch.sql, "originAttributes");
    if (c.cName < 0 || c.cValue < 0 || c.cHost < 0 || c.cExpiry < 0 || c.cOrigin < 0) {
        _snprintf(err, errlen, "Firefox の cookie の表の形が想定と違う");
        goto done;
    }
    (void)userVersion;
    c.now = unix_time();
    walk_table(&q, sch.root, 0, cookie_row, &c);
    /* ふつうの cookie でログインしていればそれ。無ければコンテナやサイトごとの分離の束
       (Firefox の設定で YouTube の cookie がそちらに入る)。埋め込み用の束は使わない */
    {
        int pass, i;
        for (pass = 0; pass < 2 && !res; pass++)
            for (i = 0; i < c.ng && !res; i++) {
                char *ck;
                if (c.g[i].partition || (pass == 0) != (c.g[i].origin[0] == 0)) continue;
                ck = group_cookie(&c.g[i]);
                if (ck && yt_cookie_ok(ck)) { res = ck; *count = c.g[i].count; log_printf("firefox cookies: group '%s'", c.g[i].origin); }
                else if (ck) { SecureZeroMemory(ck, strlen(ck)); free(ck); }
            }
    }
    if (!res) {
        /* 遠くの PC からの報告でも原因が分かるよう、数を添える */
        int frames = 0, i, others = 0, parts = 0;
        for (i = 0; i < (int)q.walPgN; i++) if (q.walPg[i]) frames++;
        for (i = 0; i < c.ng; i++) { if (c.g[i].partition) parts += c.g[i].count; else if (c.g[i].origin[0]) others += c.g[i].count; }
        _snprintf(err, errlen, "Firefox で YouTube にログインしていないようです(SAPISID が無い。YouTube の cookie %d 個、"
                  "期限切れ %d、コンテナ等 %d、埋め込み用 %d、WAL %d ページ)", c.rows, c.expired, others, parts, frames);
        goto done;
    }

done:
    {
        int i;
        for (i = 0; i < c.ng; i++) {
            if (c.g[i].out.p) { SecureZeroMemory(c.g[i].out.p, c.g[i].out.n); BufFree(&c.g[i].out); }
            if (c.g[i].music.p) { SecureZeroMemory(c.g[i].music.p, c.g[i].music.n); BufFree(&c.g[i].music); }
        }
    }
    if (q.db) { SecureZeroMemory(q.db, q.dbLen); free(q.db); }
    if (q.wal) { SecureZeroMemory(q.wal, q.walLen); free(q.wal); }
    free(q.walPg);
    free(sch.sql);
    if (res) err[0] = 0;
    else err[errlen - 1] = 0;
    return res;
}

/* Firefox の版(プロファイルの compatibility.ini の LastVersion)から User-Agent を作る */
void ff_user_agent(const WCHAR *profileDir, char *ua, size_t cap)
{
    WCHAR path[MAX_PATH], v[64];
    int   major;
    _snwprintf(path, MAX_PATH, L"%s\\compatibility.ini", profileDir);
    path[MAX_PATH - 1] = 0;
    GetPrivateProfileStringW(L"Compatibility", L"LastVersion", L"", v, ARRAYSIZE(v), path);
    major = _wtoi(v);
    if (major < 100) major = 150;
    _snprintf(ua, cap, "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:%d.0) Gecko/20100101 Firefox/%d.0", major, major);
    ua[cap - 1] = 0;
}

static BOOL has_cookies(const WCHAR *dir)
{
    WCHAR p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%s\\cookies.sqlite", dir);
    p[MAX_PATH - 1] = 0;
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* 「…\App\Firefox64\firefox.exe」から Portable 版のフォルダ(App の親)を得る */
static BOOL portable_root_from_exe(const WCHAR *exe, WCHAR *root, size_t cap)
{
    const WCHAR *p = exe, *app = NULL;
    for (; *p; p++)
        if ((*p == L'\\' || *p == L'/') && !_wcsnicmp(p + 1, L"App\\", 4)) app = p;
    if (!app) return FALSE;
    if ((size_t)(app - exe) >= cap) return FALSE;
    wmemcpy(root, exe, (size_t)(app - exe));
    root[app - exe] = 0;
    return TRUE;
}

/* 指定されたもの(プロファイルのフォルダ、Portable 版のフォルダ、firefox.exe や FirefoxPortable.exe)
   から、cookies.sqlite のあるプロファイルのフォルダを得る。
   Portable 版(PortableApps.com)は FirefoxPortable\Data\profile、本体は FirefoxPortable\App\Firefox64\firefox.exe */
BOOL ff_resolve_profile(const WCHAR *given, WCHAR *out, size_t cap)
{
    WCHAR dir[MAX_PATH], t[MAX_PATH], *slash;
    DWORD a;
    if (!given || !*given) return FALSE;
    wcsncpy_s(dir, MAX_PATH, given, _TRUNCATE);
    /* 前後の引用符と末尾の \ を外す */
    if (dir[0] == L'"') { memmove(dir, dir + 1, wcslen(dir) * sizeof(WCHAR)); slash = wcschr(dir, L'"'); if (slash) *slash = 0; }
    while (wcslen(dir) > 3 && (dir[wcslen(dir) - 1] == L'\\' || dir[wcslen(dir) - 1] == L'/')) dir[wcslen(dir) - 1] = 0;
    a = GetFileAttributesW(dir);
    if (a == INVALID_FILE_ATTRIBUTES) return FALSE;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        /* exe が指定された: Portable の構成なら Data\profile、そうでなければ exe のフォルダ */
        if (portable_root_from_exe(dir, t, MAX_PATH)) { wcscpy_s(dir, MAX_PATH, t); }
        else { slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0; }
    }
    if (has_cookies(dir)) { wcsncpy_s(out, cap, dir, _TRUNCATE); return TRUE; }
    _snwprintf(t, MAX_PATH, L"%s\\Data\\profile", dir);
    t[MAX_PATH - 1] = 0;
    if (has_cookies(t)) { wcsncpy_s(out, cap, t, _TRUNCATE); return TRUE; }
    _snwprintf(t, MAX_PATH, L"%s\\profile", dir);           /* Data フォルダを指定された */
    t[MAX_PATH - 1] = 0;
    if (has_cookies(t)) { wcsncpy_s(out, cap, t, _TRUNCATE); return TRUE; }
    return FALSE;
}

/* Portable 版のプロファイルのフォルダから、起動に使う exe を得る(無ければ FALSE) */
BOOL ff_launcher_for_profile(const WCHAR *profile, WCHAR *exe, size_t cap)
{
    WCHAR root[MAX_PATH], t[MAX_PATH];
    size_t n = wcslen(profile);
    if (n < 13 || _wcsicmp(profile + n - 13, L"\\Data\\profile")) return FALSE;
    wcsncpy_s(root, MAX_PATH, profile, n - 13);
    _snwprintf(t, MAX_PATH, L"%s\\FirefoxPortable.exe", root);
    t[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(t) == INVALID_FILE_ATTRIBUTES) {
        _snwprintf(t, MAX_PATH, L"%s\\App\\Firefox64\\firefox.exe", root);
        if (GetFileAttributesW(t) == INVALID_FILE_ATTRIBUTES) return FALSE;
    }
    wcsncpy_s(exe, cap, t, _TRUNCATE);
    return TRUE;
}

/* 「\Device\ImDisk2\…」のような NT の名前を「R:\…」に直す。
   2026-10-13: RAM ディスク(ImDisk)の上の firefox.exe は、QueryFullProcessImageNameW(0)でも
   この形で返ってきて、Portable 版だと見分けられなかった */
static void device_to_dos(WCHAR *path, size_t cap)
{
    WCHAR drive[3] = L"A:", target[MAX_PATH], t[MAX_PATH];
    if (wcsncmp(path, L"\\Device\\", 8)) return;
    for (; drive[0] <= L'Z'; drive[0]++) {
        size_t n;
        if (!QueryDosDeviceW(drive, target, MAX_PATH)) continue;
        n = wcslen(target);
        if (!_wcsnicmp(path, target, n) && path[n] == L'\\') {
            _snwprintf(t, MAX_PATH, L"%s%s", drive, path + n);
            t[MAX_PATH - 1] = 0;
            wcsncpy_s(path, cap, t, _TRUNCATE);
            return;
        }
    }
}

/* 動いている Firefox の起動オプションの「-profile <フォルダ>」(Portable 版はこれで起動する) */
typedef LONG (NTAPI *fnNtQIP)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } USTR;

static BOOL profile_from_cmdline(HANDLE h, WCHAR *out, size_t cap)
{
    static fnNtQIP nqip;
    BYTE  *buf;
    ULONG  len = 0;
    WCHAR *cmd, *p;
    BOOL   ok = FALSE;
    if (!nqip) nqip = (fnNtQIP)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (!nqip) return FALSE;
    buf = (BYTE *)malloc(65536);
    if (!buf) return FALSE;
    /* 60 = ProcessCommandLineInformation(Windows 8.1 から。PROCESS_QUERY_LIMITED_INFORMATION で読める) */
    if (nqip(h, 60, buf, 65536 - sizeof(WCHAR), &len) == 0) {
        USTR *us = (USTR *)buf;
        cmd = (WCHAR *)calloc(us->Length / sizeof(WCHAR) + 1, sizeof(WCHAR));
        if (cmd) {
            memcpy(cmd, us->Buffer, us->Length);
            for (p = cmd; (p = wcsstr(p, L"-profile")) != NULL; p += 8) {
                WCHAR *s = p + 8, *e;
                if (p > cmd && p[-1] != L' ' && p[-1] != L'-') continue;
                if (*s != L' ' && *s != L'\t') continue;
                while (*s == L' ' || *s == L'\t') s++;
                if (*s == L'"') { s++; e = wcschr(s, L'"'); }
                else e = wcschr(s, L' ');
                if (e) *e = 0;
                if (*s) { wcsncpy_s(out, cap, s, _TRUNCATE); ok = TRUE; }
                break;
            }
            free(cmd);
        }
    }
    free(buf);
    return ok;
}

static void add_candidate(WCHAR (*list)[MAX_PATH], int *n, int max, const WCHAR *dir)
{
    int i;
    if (*n >= max || !has_cookies(dir)) return;
    for (i = 0; i < *n; i++) if (!_wcsicmp(list[i], dir)) return;
    wcsncpy_s(list[(*n)++], MAX_PATH, dir, _TRUNCATE);
}

/* 読む候補を並べる: 動いている Portable 版 → いつものプロファイル → その他のプロファイル */
int ff_candidates(WCHAR (*list)[MAX_PATH], int max)
{
    int            n = 0;
    HANDLE         snap;
    PROCESSENTRY32W pe;
    WCHAR          p[MAX_PATH], ini[MAX_PATH], secs[4096], val[MAX_PATH], appdata[MAX_PATH];
    const WCHAR   *s;

    /* 1. 動いている firefox.exe が「…\App\Firefox…\firefox.exe」なら Portable 版 */
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) do {
            if (!_wcsicmp(pe.szExeFile, L"firefox.exe")) {
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (h) {
                    WCHAR exe[MAX_PATH], arg[MAX_PATH];
                    DWORD len = MAX_PATH;
                    /* 1. 起動オプションの -profile(いちばん確か) */
                    if (profile_from_cmdline(h, arg, MAX_PATH)) {
                        device_to_dos(arg, MAX_PATH);
                        if (ff_resolve_profile(arg, p, MAX_PATH)) add_candidate(list, &n, max, p);
                    }
                    /* 2. exe の場所(…\App\Firefox…\firefox.exe なら Portable 版) */
                    if (QueryFullProcessImageNameW(h, 0, exe, &len)) {
                        device_to_dos(exe, MAX_PATH);
                        if (ff_resolve_profile(exe, p, MAX_PATH)) add_candidate(list, &n, max, p);
                    }
                    CloseHandle(h);
                }
            }
        } while (Process32NextW(snap, &pe));
        CloseHandle(snap);
    }
    /* 2. いつものプロファイル */
    if (ff_find_profile(p, MAX_PATH)) add_candidate(list, &n, max, p);
    /* 3. profiles.ini にあるほかのプロファイル */
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
        _snwprintf(ini, MAX_PATH, L"%s\\Mozilla\\Firefox\\profiles.ini", appdata);
        GetPrivateProfileSectionNamesW(secs, ARRAYSIZE(secs), ini);
        for (s = secs; *s; s += wcslen(s) + 1)
            if (!_wcsnicmp(s, L"Profile", 7) && GetPrivateProfileStringW(s, L"Path", L"", val, MAX_PATH, ini)) {
                WCHAR *q;
                if (GetPrivateProfileIntW(s, L"IsRelative", 1, ini)) _snwprintf(p, MAX_PATH, L"%s\\Mozilla\\Firefox\\%s", appdata, val);
                else wcsncpy_s(p, MAX_PATH, val, _TRUNCATE);
                for (q = p; *q; q++) if (*q == L'/') *q = L'\\';
                add_candidate(list, &n, max, p);
            }
    }
    return n;
}

/* ふだん使われているプロファイルを探す。
   profiles.ini の [Install...] の Default(いま使われているもの)を最優先し、
   無ければ [Profile*] の Default=1、それも無ければ cookies.sqlite の一番新しいもの。 */
BOOL ff_find_profile(WCHAR *out, size_t cap)
{
    WCHAR       roots[2][MAX_PATH], ini[MAX_PATH], secs[4096], val[MAX_PATH];
    const WCHAR *s;
    int         r;
    WCHAR       appdata[MAX_PATH], local[MAX_PATH];

    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata))) return FALSE;
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local))) local[0] = 0;
    _snwprintf(roots[0], MAX_PATH, L"%s\\Mozilla\\Firefox", appdata);
    _snwprintf(roots[1], MAX_PATH, L"%s\\Packages\\Mozilla.Firefox_n80bbvh6b1yt2\\LocalCache\\Roaming\\Mozilla\\Firefox", local);
    for (r = 0; r < 2; r++) {
        WCHAR best[MAX_PATH] = L"";
        _snwprintf(ini, MAX_PATH, L"%s\\profiles.ini", roots[r]);
        if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) continue;
        GetPrivateProfileSectionNamesW(secs, ARRAYSIZE(secs), ini);
        for (s = secs; *s; s += wcslen(s) + 1)
            if (!_wcsnicmp(s, L"Install", 7) && GetPrivateProfileStringW(s, L"Default", L"", val, MAX_PATH, ini)) {
                _snwprintf(best, MAX_PATH, L"%s\\%s", roots[r], val);
                break;
            }
        if (!best[0])
            for (s = secs; *s; s += wcslen(s) + 1)
                if (!_wcsnicmp(s, L"Profile", 7) && GetPrivateProfileIntW(s, L"Default", 0, ini) == 1 &&
                    GetPrivateProfileStringW(s, L"Path", L"", val, MAX_PATH, ini)) {
                    if (GetPrivateProfileIntW(s, L"IsRelative", 1, ini)) _snwprintf(best, MAX_PATH, L"%s\\%s", roots[r], val);
                    else wcsncpy_s(best, MAX_PATH, val, _TRUNCATE);
                    break;
                }
        if (best[0]) {
            WCHAR *p;
            for (p = best; *p; p++) if (*p == L'/') *p = L'\\';
            wcsncpy_s(out, cap, best, _TRUNCATE);
            return TRUE;
        }
    }
    return FALSE;
}
