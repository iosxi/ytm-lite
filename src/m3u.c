/* ==================================================================
 * m3u.c - プレイリストの URL を並べた M3U を読む
 *
 *  1 行に 1 つ、YouTube Music のプレイリストの URL か ID を書いたもの。
 *  「#EXTINF:-1,名前」の行があれば、次の URL の名前にする。ほかの「#」の行は読み飛ばす。
 *  読むだけで通信はしない(中身はその行を選んだときに取りに行く)。
 *
 *  文字コード: UTF-16(BOM あり)、UTF-8(BOM の有無は問わない)、どちらでもなければ
 *  ANSI のコードページ(日本語の Windows なら Shift_JIS)として読む。
 * ================================================================== */

#include "ytm.h"

#define M3U_MAX_BYTES  (1024 * 1024)

static BOOL is_id_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/* 1 行からプレイリストの ID を取る。URL なら「list=」の後ろ、そうでなければ ID だけの行 */
static char *line_to_id(const char *s)
{
    const char *p;
    size_t      n;
    if (strstr(s, "list=")) return yt_playlist_id_from_text(s);
    /* ID だけの行: PL…(ふつう)、RD…(ミックス)、OL…(アルバム)、LM(高く評価した曲)、VL を前に付けたもの */
    p = s;
    if (!strncmp(p, "VL", 2)) p += 2;
    if (strncmp(p, "PL", 2) && strncmp(p, "RD", 2) && strncmp(p, "OL", 2) && strcmp(p, "LM")) return NULL;
    for (n = 0; p[n]; n++) if (!is_id_char(p[n])) return NULL;
    return yt_playlist_id_from_text(s);
}

static void add_item(M3uList *l, char *id, const WCHAR *title)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (!strcmp(l->v[i].id, id)) { free(id); return; }      /* 同じものは 1 つに */
    if (l->n == l->cap) {
        int      cap = l->cap ? l->cap * 2 : 16;
        M3uItem *v = (M3uItem *)realloc(l->v, sizeof(M3uItem) * cap);
        if (!v) { free(id); return; }
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n].id = id;
    l->v[l->n].title = title && *title ? WDup(title) : NULL;
    l->n++;
}

/* バイト列を WCHAR に(BOM と文字コードを見て) */
static WCHAR *decode(const unsigned char *d, size_t n)
{
    WCHAR *w;
    int    len;
    UINT   cp = CP_UTF8;
    DWORD  flags = MB_ERR_INVALID_CHARS;

    if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) {
        size_t chars = (n - 2) / 2;
        w = (WCHAR *)malloc((chars + 1) * sizeof(WCHAR));
        if (!w) return NULL;
        memcpy(w, d + 2, chars * sizeof(WCHAR));
        w[chars] = 0;
        return w;
    }
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) { d += 3; n -= 3; }
    if (!n) return WDup(L"");
    len = MultiByteToWideChar(cp, flags, (const char *)d, (int)n, NULL, 0);
    if (len <= 0) {                     /* UTF-8 ではない: ANSI(Shift_JIS)として */
        cp = CP_ACP;
        flags = 0;
        len = MultiByteToWideChar(cp, flags, (const char *)d, (int)n, NULL, 0);
        if (len <= 0) return NULL;
    }
    w = (WCHAR *)malloc(((size_t)len + 1) * sizeof(WCHAR));
    if (!w) return NULL;
    MultiByteToWideChar(cp, flags, (const char *)d, (int)n, w, len);
    w[len] = 0;
    return w;
}

int m3u_parse(const void *data, size_t n, M3uList *out)
{
    WCHAR *text = decode((const unsigned char *)data, n), *line, *ctx = NULL;
    WCHAR *title = NULL;

    memset(out, 0, sizeof(*out));
    if (!text) return 0;
    for (line = wcstok_s(text, L"\r\n", &ctx); line; line = wcstok_s(NULL, L"\r\n", &ctx)) {
        WCHAR *e;
        while (*line == L' ' || *line == L'\t') line++;
        e = line + wcslen(line);
        while (e > line && (e[-1] == L' ' || e[-1] == L'\t')) *--e = 0;
        if (!*line) continue;
        if (line[0] == L'#') {
            if (!_wcsnicmp(line, L"#EXTINF:", 8)) {
                WCHAR *comma = wcschr(line, L',');
                free(title);
                title = NULL;
                if (comma) {
                    comma++;
                    while (*comma == L' ') comma++;
                    title = WDup(comma);
                }
            }
            continue;
        }
        {
            char *u = W2U(line), *id = u ? line_to_id(u) : NULL;
            free(u);
            if (id) add_item(out, id, title);
            else out->skipped++;
        }
        free(title);
        title = NULL;
    }
    free(title);
    free(text);
    return out->n;
}

BOOL m3u_load(const WCHAR *path, M3uList *out, char *err, size_t errlen)
{
    HANDLE        f;
    LARGE_INTEGER size;
    char         *buf;
    DWORD         got = 0;
    BOOL          ok;

    memset(out, 0, sizeof(*out));
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { _snprintf(err, errlen, "ファイルを開けませんでした"); err[errlen - 1] = 0; return FALSE; }
    if (!GetFileSizeEx(f, &size) || size.QuadPart > M3U_MAX_BYTES) {
        CloseHandle(f);
        _snprintf(err, errlen, "ファイルが大きすぎます(1MB まで)");
        err[errlen - 1] = 0;
        return FALSE;
    }
    buf = (char *)malloc((size_t)size.QuadPart + 1);
    ok = buf && ReadFile(f, buf, (DWORD)size.QuadPart, &got, NULL);
    CloseHandle(f);
    if (!ok) { free(buf); _snprintf(err, errlen, "ファイルを読めませんでした"); err[errlen - 1] = 0; return FALSE; }
    m3u_parse(buf, got, out);
    free(buf);
    if (!out->n) {
        _snprintf(err, errlen, "プレイリストの URL が見つかりませんでした");
        err[errlen - 1] = 0;
        return FALSE;
    }
    return TRUE;
}

void m3u_free(M3uList *l)
{
    int i;
    for (i = 0; i < l->n; i++) { free(l->v[i].id); free(l->v[i].title); }
    free(l->v);
    memset(l, 0, sizeof(*l));
}

int m3u_selftest(void)
{
    static const char utf8[] =
        "\xEF\xBB\xBF#EXTM3U\r\n"
        "#EXTINF:-1,\xE4\xBD\x9C\xE6\xA5\xAD\xE7\x94\xA8\r\n"                    /* 作業用 */
        "https://music.youtube.com/playlist?list=PLaaaaaaaaaa\r\n"
        "  https://www.youtube.com/watch?v=xyz&list=PLbbbbbbbbbb&index=2  \n"
        "\n"
        "# comment\n"
        "PLaaaaaaaaaa\n"                                                       /* 重複 */
        "VLPLcccccccccc\n"
        "https://music.youtube.com/watch?v=abc\n"                              /* プレイリストではない */
        "hello world\n";
    static const char sjis[] = "#EXTINF:-1,\x83\x68\x83\x89\x83\x43\x83\x75\nPLdddddddddd\n";   /* ドライブ */
    M3uList l;
    int     fail = 0;

    fail += m3u_parse(utf8, sizeof(utf8) - 1, &l) != 3;
    fail += l.n < 3 || strcmp(l.v[0].id, "PLaaaaaaaaaa") || strcmp(l.v[1].id, "PLbbbbbbbbbb") || strcmp(l.v[2].id, "PLcccccccccc");
    fail += l.n < 3 || !l.v[0].title || wcscmp(l.v[0].title, L"\x4F5C\x696D\x7528") || l.v[1].title != NULL;
    fail += l.skipped != 2;
    m3u_free(&l);
    if (GetACP() == 932) {
        fail += m3u_parse(sjis, sizeof(sjis) - 1, &l) != 1;
        fail += l.n < 1 || !l.v[0].title || wcscmp(l.v[0].title, L"\x30C9\x30E9\x30A4\x30D6");
        m3u_free(&l);
    }
    return fail;
}

/* ------------------------------------------------------------------ */
/*  書き出し: プレイリストの曲を、ふつうの M3U(拡張 M3U)として         */
/*  #EXTINF:秒数,アーティスト - 曲名 と曲の URL を 1 曲ずつ並べる。       */
/*  本人の指定(2026-10-13)。ほかのプレイヤー(VLC や mpv など)でも読める形 */
/* ------------------------------------------------------------------ */

/* 改行を空白に(1 行に収める) */
static void add_line_text(Buf *b, const WCHAR *w)
{
    char *u = W2U(w ? w : L""), *p;
    if (!u) return;
    for (p = u; *p; p++) if (*p == '\r' || *p == '\n') *p = ' ';
    BufAddStr(b, u);
    free(u);
}

/* UTF-8 の M3U の中身(CRLF)。呼ぶ側で free */
char *m3u_build(const TrackList *t, const WCHAR *name)
{
    Buf b = {0};
    int i;
    BufAddStr(&b, "#EXTM3U\r\n");
    if (name && *name) {
        BufAddStr(&b, "#PLAYLIST:");
        add_line_text(&b, name);
        BufAddStr(&b, "\r\n");
    }
    for (i = 0; i < t->n; i++) {
        const Track *k = &t->v[i];
        if (!k->vid[0]) continue;
        BufAddf(&b, "#EXTINF:%d,", k->dur > 0 ? k->dur : -1);
        if (k->artist && k->artist[0]) {
            add_line_text(&b, k->artist);
            BufAddStr(&b, " - ");
        }
        add_line_text(&b, k->title);
        BufAddf(&b, "\r\nhttps://music.youtube.com/watch?v=%s\r\n", k->vid);
    }
    return b.p;
}

/* 書けた曲の数を返す(失敗は -1) */
int m3u_save(const WCHAR *path, const TrackList *t, const WCHAR *name, char *err, size_t errlen)
{
    char  *s = m3u_build(t, name);
    HANDLE f;
    DWORD  n, wr = 0;
    BOOL   ok;
    int    i, count = 0;
    if (!s) { _snprintf(err, errlen, "書き出す中身を作れませんでした"); err[errlen - 1] = 0; return -1; }
    n = (DWORD)strlen(s);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        free(s);
        _snprintf(err, errlen, "ファイルを作れませんでした");
        err[errlen - 1] = 0;
        return -1;
    }
    ok = WriteFile(f, s, n, &wr, NULL) && wr == n;
    CloseHandle(f);
    free(s);
    if (!ok) { _snprintf(err, errlen, "ファイルに書けませんでした"); err[errlen - 1] = 0; return -1; }
    for (i = 0; i < t->n; i++) if (t->v[i].vid[0]) count++;
    return count;
}

int m3u_selftest_build(void)
{
    Track     v[2];
    TrackList t;
    char     *s;
    int       fail = 0;
    memset(v, 0, sizeof(v));
    strcpy_s(v[0].vid, sizeof(v[0].vid), "abcdefghijk");
    v[0].title = (WCHAR *)L"Song\r\nOne";
    v[0].artist = (WCHAR *)L"Artist";
    v[0].dur = 125;
    strcpy_s(v[1].vid, sizeof(v[1].vid), "lmnopqrstuv");
    v[1].title = (WCHAR *)L"Two";
    t.v = v;
    t.n = 2;
    t.cap = 2;
    s = m3u_build(&t, L"List");
    fail += !s || strcmp(s, "#EXTM3U\r\n#PLAYLIST:List\r\n"
                            "#EXTINF:125,Artist - Song  One\r\nhttps://music.youtube.com/watch?v=abcdefghijk\r\n"
                            "#EXTINF:-1,Two\r\nhttps://music.youtube.com/watch?v=lmnopqrstuv\r\n") != 0;
    free(s);
    return fail;
}
