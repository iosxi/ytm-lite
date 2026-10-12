/* ==================================================================
 * ui.c - 画面
 *
 *   ┌───────────┬──────────────────────────────┐
 *   │プレイリスト│ 曲の一覧(#、タイトル、アーティスト、アルバム、時間) │
 *   ├───────────┴──────────────────────────────┤
 *   │ 0:12 ━━━━━━━━━━━○━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 3:45 │
 *   │ ⏮ ⏯ ⏭   曲名 / アーティスト(または状態)   🔀 🔁 ──○── ⋯ │
 *   └──────────────────────────────────────────┘
 *
 *  メモリを食わないための作り:
 *   - 曲の一覧は仮想リスト(LVS_OWNERDATA)。文字列は TrackList の 1 か所だけに持つ。
 *   - ジャケット画像は扱わない。
 *   - 再生していないときはタイマーも止める(何もしない間は CPU 0)。
 *   - 先読みは次の曲 1 つだけ、終わる 40 秒前から。
 * ================================================================== */

#include "yum.h"
#include "resource.h"
#include <windowsx.h>
#include <shellapi.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#define CLASS_NAME   L"yum-lite"
#define PROP_INI     L"yum.ini"

#define IDC_PLS      100
#define IDC_TRACKS   101
#define IDC_SEEK     102
#define IDC_VOL      103
#define IDB_PREV     201
#define IDB_PLAY     202
#define IDB_NEXT     203
#define IDB_SHUFFLE  204
#define IDB_REPEAT   205
#define IDB_MENU     206

#define ID_M_LOGIN   301
#define ID_M_OPENURL 302
#define ID_M_REFRESH 303
#define ID_M_LOGOUT  304
#define ID_M_VERSION 305

#define TIMER_TICK   1
#define TIMER_SKIP   2

#define BAR_H        88            /* 下の帯の高さ(DIP) */
#define PREFETCH_MS  40000

/* Segoe MDL2 Assets / Segoe Fluent Icons の字 */
#define G_PREV    L"\xE892"
#define G_PLAY    L"\xE768"
#define G_PAUSE   L"\xE769"
#define G_NEXT    L"\xE893"
#define G_SHUFFLE L"\xE8B1"
#define G_REPEAT  L"\xE8EE"
#define G_MORE    L"\xE712"

typedef struct { LONG ref; TrackList t; } TL;

typedef struct {
    char  *id;
    WCHAR *title, *sub;
    BOOL   account;        /* ライブラリから来たもの(アカウントで読む) */
    TL    *tl;
    BOOL   loading;
} Entry;

static HWND   g_wnd, g_pls, g_tracks, g_seek, g_vol, g_btn[6];
static UINT   g_dpi = 96;
static HFONT  g_font, g_fontBold, g_fontIcon, g_fontIconBig;
static BOOL   g_mute;

static Entry *g_ent;
static int    g_entN, g_entCap;
static int    g_view = -1;
static TL    *g_viewTL;

static TL    *g_playTL;
static int   *g_order, g_orderN, g_pos = -1;
static int    g_gen;
static int    g_failStreak;
static BOOL   g_loadingAudio;
static char   g_prefAsked[16];
static char   g_prefVid[16];
static AudioData g_pref;
static char   g_playWhenLoaded[128];

static WCHAR  g_status[512];
static BOOL   g_statusErr;
static BOOL   g_dragging;
static int    g_lastState = -1;
static BOOL   g_timer;

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

/* ------------------------------------------------------------------ */
/*  小道具                                                              */
/* ------------------------------------------------------------------ */

static TL *tl_new(TrackList *t)
{
    TL *x = (TL *)calloc(1, sizeof(TL));
    if (!x) { yt_free_tracks(t); return NULL; }
    x->ref = 1;
    x->t = *t;
    memset(t, 0, sizeof(*t));
    return x;
}

static TL *tl_ref(TL *x) { if (x) x->ref++; return x; }

static void tl_release(TL *x)
{
    if (x && --x->ref == 0) {
        yt_free_tracks(&x->t);
        free(x);
    }
}

static void fmt_time(WCHAR *out, size_t cap, int ms)
{
    int s = ms / 1000;
    if (s < 0) s = 0;
    if (s >= 3600) _snwprintf(out, cap, L"%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else _snwprintf(out, cap, L"%d:%02d", s / 60, s % 60);
    out[cap - 1] = 0;
}

/* 下の帯だけを描き直す(一覧は描き直さない) */
static void invalidate_bar(void)
{
    RECT rc;
    GetClientRect(g_wnd, &rc);
    rc.top = rc.bottom - S(BAR_H);
    InvalidateRect(g_wnd, &rc, FALSE);
}

static void set_status(const WCHAR *s, BOOL err)
{
    wcsncpy_s(g_status, ARRAYSIZE(g_status), s ? s : L"", _TRUNCATE);
    g_statusErr = err;
    invalidate_bar();
}

static void set_status_u8(const char *s, BOOL err)
{
    WCHAR *w = U2W(s);
    set_status(w, err);
    free(w);
}

static Track *cur_track(void)
{
    if (!g_playTL || g_pos < 0 || g_pos >= g_orderN) return NULL;
    return &g_playTL->t.v[g_order[g_pos]];
}

static void update_title(void)
{
    Track *t = cur_track();
    WCHAR  buf[512];
    if (t && t->title) _snwprintf(buf, ARRAYSIZE(buf), L"%s - %s", t->title, APP_NAME);
    else wcscpy_s(buf, ARRAYSIZE(buf), APP_NAME);
    buf[ARRAYSIZE(buf) - 1] = 0;
    SetWindowTextW(g_wnd, buf);
}

static void ensure_timer(BOOL on)
{
    /* 表示は秒単位なので 1 秒ごとで足りる(0.5 秒だと描き直しが倍になる) */
    if (on && !g_timer) { SetTimer(g_wnd, TIMER_TICK, 1000, NULL); g_timer = TRUE; }
    else if (!on && g_timer) { KillTimer(g_wnd, TIMER_TICK); g_timer = FALSE; }
}

static void clear_prefetch(void)
{
    if (g_pref.h) GlobalFree(g_pref.h);
    memset(&g_pref, 0, sizeof(g_pref));
    g_prefVid[0] = 0;
    g_prefAsked[0] = 0;
}

/* ------------------------------------------------------------------ */
/*  プレイリストの欄                                                    */
/* ------------------------------------------------------------------ */

static int find_entry(const char *id)
{
    int i;
    for (i = 0; i < g_entN; i++)
        if (!strcmp(g_ent[i].id, id)) return i;
    return -1;
}

static int add_entry(const char *id, const WCHAR *title, const WCHAR *sub, BOOL account)
{
    Entry *e;
    if (g_entN == g_entCap) {
        int    cap = g_entCap ? g_entCap * 2 : 32;
        Entry *v = (Entry *)realloc(g_ent, sizeof(Entry) * cap);
        if (!v) return -1;
        g_ent = v;
        g_entCap = cap;
    }
    e = &g_ent[g_entN];
    memset(e, 0, sizeof(*e));
    e->id = U8Dup(id);
    e->title = WDup(title ? title : L"(読み込み中…)");
    e->sub = WDup(sub);
    e->account = account;
    return g_entN++;
}

static void free_entry(Entry *e)
{
    free(e->id);
    free(e->title);
    free(e->sub);
    tl_release(e->tl);
}

static void show_view(int i)
{
    tl_release(g_viewTL);
    g_viewTL = NULL;
    g_view = i;
    if (i >= 0 && i < g_entN) {
        g_viewTL = tl_ref(g_ent[i].tl);
        ListView_SetItemState(g_pls, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_pls, i, FALSE);
    }
    ListView_SetItemCountEx(g_tracks, g_viewTL ? g_viewTL->t.n : 0, 0);
    InvalidateRect(g_tracks, NULL, TRUE);
}

static void load_entry(int i)
{
    Entry *e = &g_ent[i];
    if (e->loading) return;
    e->loading = TRUE;
    worker_playlist(e->id, e->account || g_cfg.cookie != NULL);
    set_status(L"プレイリストを読み込み中…", FALSE);
}

static void select_entry(int i)
{
    if (i < 0 || i >= g_entN) return;
    strncpy_s(g_cfg.lastPlaylist, sizeof(g_cfg.lastPlaylist), g_ent[i].id, _TRUNCATE);
    show_view(i);
    if (!g_ent[i].tl) load_entry(i);
}

/* ------------------------------------------------------------------ */
/*  再生の順番                                                          */
/* ------------------------------------------------------------------ */

static void build_order(int startIdx)
{
    int n = g_playTL ? g_playTL->t.n : 0, i;
    free(g_order);
    g_order = n ? (int *)malloc(sizeof(int) * n) : NULL;
    g_orderN = g_order ? n : 0;
    for (i = 0; i < g_orderN; i++) g_order[i] = i;
    if (g_cfg.shuffle && g_orderN > 1) {
        /* 選んだ曲を先頭に、残りを混ぜる */
        unsigned seed = (unsigned)GetTickCount() ^ (unsigned)(UINT_PTR)&n;
        if (startIdx >= 0) { g_order[0] = startIdx; g_order[startIdx] = 0; }
        for (i = g_orderN - 1; i > (startIdx >= 0 ? 1 : 0); i--) {
            int lo = startIdx >= 0 ? 1 : 0, j;
            seed = seed * 1103515245u + 12345u;
            j = lo + (int)((seed >> 8) % (unsigned)(i - lo + 1));
            { int t = g_order[i]; g_order[i] = g_order[j]; g_order[j] = t; }
        }
        g_pos = 0;
    } else {
        g_pos = startIdx >= 0 ? startIdx : 0;
    }
}

/* dir の向きに、灰色(再生できない)でない曲を探す。無ければ -1 */
static int next_playable(int from, int dir, BOOL wrap)
{
    int k, p = from;
    for (k = 0; k < g_orderN; k++) {
        if (p < 0 || p >= g_orderN) {
            if (!wrap) return -1;
            p = p < 0 ? g_orderN - 1 : 0;
        }
        if (!g_playTL->t.v[g_order[p]].grey) return p;
        p += dir;
    }
    return -1;
}

static void stop_playback(const WCHAR *msg, BOOL err)
{
    g_gen++;
    g_loadingAudio = FALSE;
    worker_cancel_audio();
    player_stop();
    clear_prefetch();
    KillTimer(g_wnd, TIMER_SKIP);
    if (msg) set_status(msg, err);
    InvalidateRect(g_tracks, NULL, FALSE);
}

static void start_track(BOOL user)
{
    Track *t = cur_track();
    if (!t) return;
    g_gen++;
    KillTimer(g_wnd, TIMER_SKIP);
    update_title();
    if (g_prefVid[0] && !strcmp(g_prefVid, t->vid) && g_pref.h) {
        player_open(&g_pref, g_gen);          /* 先読み済み: すぐ鳴らす */
        g_prefVid[0] = 0;
        g_loadingAudio = FALSE;
    } else {
        if (user) player_stop();
        worker_audio(t->vid, g_gen, FALSE);
        g_loadingAudio = TRUE;
        set_status(L"読み込み中…", FALSE);
    }
    clear_prefetch();
    ensure_timer(TRUE);
    InvalidateRect(g_tracks, NULL, FALSE);
    invalidate_bar();
}

static void play_from(TL *tl, int idx)
{
    if (!tl || !tl->t.n) return;
    if (tl != g_playTL) {
        tl_release(g_playTL);
        g_playTL = tl_ref(tl);
    }
    g_failStreak = 0;
    build_order(idx);
    g_pos = next_playable(g_pos, 1, g_cfg.repeat);
    if (g_pos < 0) { set_status(L"再生できる曲がありません", TRUE); return; }
    start_track(TRUE);
}

static void next_track(BOOL user)
{
    int p;
    if (!g_playTL || g_orderN == 0) return;
    p = next_playable(g_pos + 1, 1, FALSE);
    if (p < 0 && g_cfg.repeat) {
        if (g_cfg.shuffle) build_order(-1);
        p = next_playable(0, 1, FALSE);
    }
    if (p < 0) {
        g_pos = -1;
        stop_playback(L"最後まで再生しました", FALSE);
        update_title();
        return;
    }
    g_pos = p;
    start_track(user);
}

static void prev_track(void)
{
    int st, pos, p;
    if (!g_playTL) return;
    player_status(&st, &pos, NULL, NULL);
    if (st != PS_IDLE && pos > 3000) { player_seek(0); return; }
    p = next_playable(g_pos - 1, -1, g_cfg.repeat);
    if (p < 0) p = g_pos;
    if (p < 0) return;
    g_pos = p;
    start_track(TRUE);
}

static void toggle_play(void)
{
    int st;
    player_status(&st, NULL, NULL, NULL);
    if (st == PS_PLAYING) { player_pause(TRUE); return; }
    if (st == PS_PAUSED) { player_pause(FALSE); ensure_timer(TRUE); return; }
    if (g_loadingAudio) return;
    if (cur_track()) { start_track(TRUE); return; }
    if (g_viewTL && g_viewTL->t.n) {
        int sel = ListView_GetNextItem(g_tracks, -1, LVNI_SELECTED);
        play_from(g_viewTL, sel >= 0 ? sel : (g_cfg.shuffle ? -1 : 0));
    }
}

/* 次に鳴らす曲(先読みの対象) */
static const char *peek_next_vid(void)
{
    int p;
    if (!g_playTL || g_orderN == 0) return NULL;
    p = next_playable(g_pos + 1, 1, FALSE);
    if (p < 0 && g_cfg.repeat && !g_cfg.shuffle) p = next_playable(0, 1, FALSE);
    return p >= 0 ? g_playTL->t.v[g_order[p]].vid : NULL;
}

/* ------------------------------------------------------------------ */
/*  配置と描画                                                          */
/* ------------------------------------------------------------------ */

static void create_fonts(void)
{
    NONCLIENTMETRICSW ncm;
    LOGFONTW          lf;
    HDC               dc;
    WCHAR             face[LF_FACESIZE];
    HFONT             old;

    if (g_font) DeleteObject(g_font);
    if (g_fontBold) DeleteObject(g_fontBold);
    if (g_fontIcon) DeleteObject(g_fontIcon);
    if (g_fontIconBig) DeleteObject(g_fontIconBig);
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_dpi);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    lf = ncm.lfMessageFont;
    lf.lfWeight = FW_SEMIBOLD;
    g_fontBold = CreateFontIndirectW(&lf);

    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -S(16);
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Segoe Fluent Icons");     /* Windows 11 */
    g_fontIcon = CreateFontIndirectW(&lf);
    dc = GetDC(g_wnd);
    old = (HFONT)SelectObject(dc, g_fontIcon);
    GetTextFaceW(dc, LF_FACESIZE, face);
    SelectObject(dc, old);
    ReleaseDC(g_wnd, dc);
    if (_wcsicmp(face, L"Segoe Fluent Icons")) {                        /* Windows 10 */
        DeleteObject(g_fontIcon);
        wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Segoe MDL2 Assets");
        g_fontIcon = CreateFontIndirectW(&lf);
    }
    lf.lfHeight = -S(20);
    g_fontIconBig = CreateFontIndirectW(&lf);
}

typedef struct { RECT bar, timeL, timeR, text; int leftW; } Layout;

static void get_layout(Layout *L)
{
    RECT rc;
    int  w, h, barTop, rowY, rowH, x, xr;
    GetClientRect(g_wnd, &rc);
    w = rc.right;
    h = rc.bottom;
    barTop = h - S(BAR_H);
    if (barTop < 0) barTop = 0;
    L->leftW = w * 28 / 100;
    if (L->leftW < S(160)) L->leftW = S(160);
    if (L->leftW > S(320)) L->leftW = S(320);
    SetRect(&L->bar, 0, barTop, w, h);
    SetRect(&L->timeL, S(8), barTop + S(6), S(8) + S(48), barTop + S(30));
    SetRect(&L->timeR, w - S(8) - S(48), barTop + S(6), w - S(8), barTop + S(30));
    rowY = barTop + S(34);
    rowH = S(48);
    x = S(12) + S(36) + S(4) + S(44) + S(4) + S(36) + S(16);
    xr = w - S(12) - S(32) - S(8) - S(96) - S(8) - S(32) - S(4) - S(32) - S(12);
    SetRect(&L->text, x, rowY, xr > x ? xr : x, rowY + rowH);
}

static void layout(void)
{
    Layout L;
    RECT   rc;
    int    w, barTop, rowY, rowH, x;
    HDWP   dw;

    if (!g_tracks) return;
    get_layout(&L);
    GetClientRect(g_wnd, &rc);
    w = rc.right;
    barTop = L.bar.top;
    rowY = barTop + S(34);
    rowH = S(48);

    dw = BeginDeferWindowPos(12);
    dw = DeferWindowPos(dw, g_pls, NULL, 0, 0, L.leftW, barTop, SWP_NOZORDER | SWP_NOACTIVATE);
    dw = DeferWindowPos(dw, g_tracks, NULL, L.leftW + 1, 0, w - L.leftW - 1, barTop, SWP_NOZORDER | SWP_NOACTIVATE);
    dw = DeferWindowPos(dw, g_seek, NULL, L.timeL.right, barTop + S(6), L.timeR.left - L.timeL.right, S(24), SWP_NOZORDER | SWP_NOACTIVATE);
    x = S(12);
    dw = DeferWindowPos(dw, g_btn[0], NULL, x, rowY + (rowH - S(36)) / 2, S(36), S(36), SWP_NOZORDER | SWP_NOACTIVATE);
    x += S(36) + S(4);
    dw = DeferWindowPos(dw, g_btn[1], NULL, x, rowY + (rowH - S(44)) / 2, S(44), S(44), SWP_NOZORDER | SWP_NOACTIVATE);
    x += S(44) + S(4);
    dw = DeferWindowPos(dw, g_btn[2], NULL, x, rowY + (rowH - S(36)) / 2, S(36), S(36), SWP_NOZORDER | SWP_NOACTIVATE);
    x = w - S(12) - S(32);
    dw = DeferWindowPos(dw, g_btn[5], NULL, x, rowY + (rowH - S(32)) / 2, S(32), S(32), SWP_NOZORDER | SWP_NOACTIVATE);
    x -= S(8) + S(96);
    dw = DeferWindowPos(dw, g_vol, NULL, x, rowY + (rowH - S(24)) / 2, S(96), S(24), SWP_NOZORDER | SWP_NOACTIVATE);
    x -= S(8) + S(32);
    dw = DeferWindowPos(dw, g_btn[4], NULL, x, rowY + (rowH - S(32)) / 2, S(32), S(32), SWP_NOZORDER | SWP_NOACTIVATE);
    x -= S(4) + S(32);
    dw = DeferWindowPos(dw, g_btn[3], NULL, x, rowY + (rowH - S(32)) / 2, S(32), S(32), SWP_NOZORDER | SWP_NOACTIVATE);
    EndDeferWindowPos(dw);

    /* 曲の一覧の列幅 */
    {
        int cw = w - L.leftW - 1 - GetSystemMetricsForDpi(SM_CXVSCROLL, g_dpi) - S(4);
        int num = S(44), dur = S(60), rest = cw - num - dur;
        if (rest < S(150)) rest = S(150);
        ListView_SetColumnWidth(g_tracks, 0, num);
        ListView_SetColumnWidth(g_tracks, 1, rest * 42 / 100);
        ListView_SetColumnWidth(g_tracks, 2, rest * 30 / 100);
        ListView_SetColumnWidth(g_tracks, 3, rest - rest * 42 / 100 - rest * 30 / 100);
        ListView_SetColumnWidth(g_tracks, 4, dur);
        ListView_SetColumnWidth(g_pls, 0, L.leftW - GetSystemMetricsForDpi(SM_CXVSCROLL, g_dpi));
    }
    InvalidateRect(g_wnd, NULL, FALSE);
}

static void paint(HDC hdc)
{
    Layout  L;
    RECT    rc, r;
    HDC     mem;
    HBITMAP bmp, oldBmp;
    HFONT   oldFont;
    int     st, pos, dur, gen;
    WCHAR   t[32];
    Track  *tr = cur_track();
    HBRUSH  line;

    get_layout(&L);
    GetClientRect(g_wnd, &rc);
    /* 2 つの一覧の境目 */
    line = CreateSolidBrush(theme_line());
    SetRect(&r, L.leftW, 0, L.leftW + 1, L.bar.top);
    FillRect(hdc, &r, line);

    /* 下の帯はちらつかないよう裏で描く */
    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, L.bar.right - L.bar.left, L.bar.bottom - L.bar.top);
    oldBmp = (HBITMAP)SelectObject(mem, bmp);
    SetViewportOrgEx(mem, 0, -L.bar.top, NULL);
    FillRect(mem, &L.bar, theme_footer_brush());
    SetRect(&r, 0, L.bar.top, rc.right, L.bar.top + 1);
    FillRect(mem, &r, line);
    SetBkMode(mem, TRANSPARENT);

    player_status(&st, &pos, &dur, &gen);
    if (tr && tr->dur && !dur) dur = tr->dur * 1000;
    if (st == PS_IDLE && !g_loadingAudio) pos = 0;
    if (g_dragging) pos = (int)SendMessageW(g_seek, TBM_GETPOS, 0, 0) * 1000;
    oldFont = (HFONT)SelectObject(mem, g_font);
    SetTextColor(mem, theme_dim_text());
    fmt_time(t, ARRAYSIZE(t), tr ? pos : 0);
    DrawTextW(mem, t, -1, &L.timeL, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    fmt_time(t, ARRAYSIZE(t), tr ? dur : 0);
    DrawTextW(mem, t, -1, &L.timeR, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);

    r = L.text;
    r.bottom = r.top + (L.text.bottom - L.text.top) / 2;
    SelectObject(mem, g_fontBold);
    SetTextColor(mem, theme_text());
    DrawTextW(mem, tr && tr->title ? tr->title : APP_NAME, -1, &r, DT_SINGLELINE | DT_BOTTOM | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    r.top = r.bottom + S(2);
    r.bottom = L.text.bottom;
    SelectObject(mem, g_font);
    if (g_status[0]) {
        SetTextColor(mem, g_statusErr ? theme_accent() : theme_dim_text());
        DrawTextW(mem, g_status, -1, &r, DT_SINGLELINE | DT_TOP | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    } else if (tr) {
        WCHAR buf[512];
        _snwprintf(buf, ARRAYSIZE(buf), L"%s%s%s", tr->artist ? tr->artist : L"", tr->album && tr->album[0] ? L" • " : L"",
                   tr->album ? tr->album : L"");
        buf[ARRAYSIZE(buf) - 1] = 0;
        SetTextColor(mem, theme_dim_text());
        DrawTextW(mem, buf, -1, &r, DT_SINGLELINE | DT_TOP | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(mem, oldFont);
    SetViewportOrgEx(mem, 0, 0, NULL);
    BitBlt(hdc, 0, L.bar.top, L.bar.right, L.bar.bottom - L.bar.top, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    DeleteObject(line);
}

static void draw_button(DRAWITEMSTRUCT *di)
{
    HDC      dc = di->hDC;
    RECT     r = di->rcItem;
    int      id = (int)di->CtlID, st;
    const WCHAR *g = L"";
    COLORREF fg = theme_text();
    BOOL     big = FALSE;
    HFONT    old;

    FillRect(dc, &r, theme_footer_brush());
    player_status(&st, NULL, NULL, NULL);
    switch (id) {
    case IDB_PREV:    g = G_PREV; break;
    case IDB_PLAY:    g = (st == PS_PLAYING) ? G_PAUSE : G_PLAY; big = TRUE; break;
    case IDB_NEXT:    g = G_NEXT; break;
    case IDB_SHUFFLE: g = G_SHUFFLE; fg = g_cfg.shuffle ? theme_accent() : theme_dim_text(); break;
    case IDB_REPEAT:  g = G_REPEAT; fg = g_cfg.repeat ? theme_accent() : theme_dim_text(); break;
    case IDB_MENU:    g = G_MORE; fg = theme_dim_text(); break;
    }
    if (big) {
        /* 丸の中に字(YouTube Music の再生ボタンのように) */
        HBRUSH b = CreateSolidBrush(theme_text());
        HBRUSH ob = (HBRUSH)SelectObject(dc, b);
        HPEN   op = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
        RECT   c = r;
        if (di->itemState & ODS_SELECTED) InflateRect(&c, -S(1), -S(1));
        Ellipse(dc, c.left, c.top, c.right, c.bottom);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(b);
        fg = theme_footer();
    } else if (di->itemState & ODS_SELECTED) {
        HBRUSH b = CreateSolidBrush(theme_line());
        FillRect(dc, &r, b);
        DeleteObject(b);
    }
    old = (HFONT)SelectObject(dc, big ? g_fontIconBig : g_fontIcon);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    DrawTextW(dc, g, -1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, old);
}

/* つまみの付いた横棒(シークと音量)。テーマの絵はダークに合わないので自前で描く */
static LRESULT draw_trackbar(NMCUSTOMDRAW *cd)
{
    HWND tb = cd->hdr.hwndFrom;
    if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    if (cd->dwItemSpec == TBCD_TICS) return CDRF_SKIPDEFAULT;
    if (cd->dwItemSpec == TBCD_CHANNEL) {
        RECT ch = cd->rc, th, full, done;
        int  cy = (ch.top + ch.bottom) / 2, hh = S(2);
        HBRUSH b1, b2;
        SendMessageW(tb, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
        GetClientRect(tb, &full);
        FillRect(cd->hdc, &full, theme_footer_brush());
        SetRect(&full, ch.left, cy - hh, ch.right, cy + hh);
        b1 = CreateSolidBrush(theme_line());
        FillRect(cd->hdc, &full, b1);
        done = full;
        done.right = (th.left + th.right) / 2;
        if (tb == g_seek && !cur_track()) done.right = done.left;     /* 曲が無いときは塗らない */
        b2 = CreateSolidBrush(tb == g_seek ? theme_accent() : theme_dim_text());
        if (done.right > done.left) FillRect(cd->hdc, &done, b2);
        DeleteObject(b1);
        DeleteObject(b2);
        return CDRF_SKIPDEFAULT;
    }
    if (cd->dwItemSpec == TBCD_THUMB) {
        RECT   th = cd->rc;
        int    cx = (th.left + th.right) / 2, cy = (th.top + th.bottom) / 2, rr = S(6);
        HBRUSH b = CreateSolidBrush(tb == g_seek ? theme_accent() : theme_text());
        HBRUSH ob = (HBRUSH)SelectObject(cd->hdc, b);
        HPEN   op = (HPEN)SelectObject(cd->hdc, GetStockObject(NULL_PEN));
        if (tb == g_seek && !cur_track()) { SelectObject(cd->hdc, ob); SelectObject(cd->hdc, op); DeleteObject(b); return CDRF_SKIPDEFAULT; }
        Ellipse(cd->hdc, cx - rr, cy - rr, cx + rr + 1, cy + rr + 1);
        SelectObject(cd->hdc, ob);
        SelectObject(cd->hdc, op);
        DeleteObject(b);
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

/* 横棒の溝をクリックしたら、その位置へ飛ぶ(既定は 1 ページずつしか動かない) */
static LRESULT CALLBACK trackbar_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    if (msg == WM_LBUTTONDOWN) {
        RECT ch, th;
        int  x = GET_X_LPARAM(lp);
        SendMessageW(h, TBM_GETCHANNELRECT, 0, (LPARAM)&ch);
        SendMessageW(h, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
        if (x < th.left || x > th.right) {
            int max = (int)SendMessageW(h, TBM_GETRANGEMAX, 0, 0);
            int w = ch.right - ch.left;
            int v = w > 0 ? MulDiv(x - ch.left, max, w) : 0;
            if (v < 0) v = 0;
            if (v > max) v = max;
            SendMessageW(h, TBM_SETPOS, TRUE, v);
            /* つまみが指の下に来たので、そのままドラッグにする */
            SendMessageW(GetParent(h), WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, v), (LPARAM)h);
        }
    } else if (msg == WM_SETFOCUS) {
        SendMessageW(h, WM_UPDATEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/*  ダイアログ                                                          */
/* ------------------------------------------------------------------ */

static const WCHAR AUTH_HELP[] =
    L"自分のプレイリストを読むために、YouTube Music にログインしているブラウザの cookie を使います"
    L"(曲の再生にはアカウントを使いません)。cookie は暗号化して yum-lite.ini に保存します。\r\n\r\n"
    L"1. ブラウザのシークレット(InPrivate)ウィンドウで music.youtube.com を開き、ログインする\r\n"
    L"2. F12 で開発者ツールを開き、[ネットワーク] を「browse」で絞り込む\r\n"
    L"3. ライブラリを開くなどして出てきた browse(POST)を選び、[要求ヘッダー] の cookie の値"
    L"(またはヘッダー全体)をコピーして下に貼り付ける\r\n"
    L"4. シークレットウィンドウを閉じる(同じセッションをブラウザで使い続けると cookie が入れ替わり、"
    L"使えなくなることがある)";

static INT_PTR CALLBACK login_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(dlg, IDC_AUTH_HELP, AUTH_HELP);
        SendDlgItemMessageW(dlg, IDC_AUTH_TEXT, EM_LIMITTEXT, 256 * 1024, 0);
        theme_apply_window(dlg);
        return TRUE;
    case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN:
        return theme_ctlcolor(msg, (HDC)wp, (HWND)lp, FALSE);
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            HWND   ed = GetDlgItem(dlg, IDC_AUTH_TEXT);
            int    n = GetWindowTextLengthW(ed);
            WCHAR *w = (WCHAR *)calloc((size_t)n + 1, sizeof(WCHAR));
            char  *u, *ck = NULL;
            char   au[8], ua[512];
            BOOL   ok = FALSE;
            if (!w) return TRUE;
            GetWindowTextW(ed, w, n + 1);
            u = W2U(w);
            if (u && yt_parse_auth_text(u, &ck, au, sizeof(au), ua, sizeof(ua))) {
                strcpy_s(g_cfg.authuser, sizeof(g_cfg.authuser), au);
                if (ua[0]) strcpy_s(g_cfg.ua, sizeof(g_cfg.ua), ua);
                cfg_set_cookie(ck);
                ok = TRUE;
            }
            if (ck) { SecureZeroMemory(ck, strlen(ck)); free(ck); }
            if (u) { SecureZeroMemory(u, strlen(u)); free(u); }
            SecureZeroMemory(w, (size_t)n * sizeof(WCHAR));
            free(w);
            if (!ok) {
                MessageBoxW(dlg, L"ログイン済みの cookie が見つかりませんでした。\r\n"
                                 L"SAPISID か __Secure-3PAPISID を含む cookie が必要です。", APP_NAME, MB_ICONWARNING);
                return TRUE;
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static INT_PTR CALLBACK url_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(dlg, IDC_URL_HELP, L"プレイリストの URL か ID を入れてください。"
                                           L"公開・限定公開のものは、ログインしなくても開けます。");
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        theme_apply_window(dlg);
        return TRUE;
    case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN:
        return theme_ctlcolor(msg, (HDC)wp, (HWND)lp, FALSE);
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            WCHAR  w[1024];
            char  *u, *id;
            char **out = (char **)GetWindowLongPtrW(dlg, DWLP_USER);
            GetDlgItemTextW(dlg, IDC_URL, w, ARRAYSIZE(w));
            u = W2U(w);
            id = u ? yt_playlist_id_from_text(u) : NULL;
            free(u);
            if (!id) { MessageBoxW(dlg, L"プレイリストの URL か ID として読めませんでした。", APP_NAME, MB_ICONWARNING); return TRUE; }
            *out = id;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  通知の処理                                                          */
/* ------------------------------------------------------------------ */

static void on_library(LibResult *r)
{
    Entry *old = g_ent;
    int    oldN = g_entN, i, sel = -1;
    char   viewId[128] = "";

    if (g_view >= 0 && g_view < g_entN) strncpy_s(viewId, sizeof(viewId), g_ent[g_view].id, _TRUNCATE);
    if (!r->ok) {
        set_status_u8(r->err, TRUE);
        yt_free_playlists(&r->pls);
        free(r);
        return;
    }
    /* 作り直す。読み込み済みの曲は引き継ぐ。URL で開いたものは後ろに残す */
    g_ent = NULL;
    g_entN = g_entCap = 0;
    for (i = 0; i < r->pls.n; i++) {
        int k = add_entry(r->pls.v[i].id, r->pls.v[i].title, r->pls.v[i].sub, TRUE), j;
        if (k < 0) continue;
        for (j = 0; j < oldN; j++)
            if (old[j].id && !strcmp(old[j].id, g_ent[k].id)) { g_ent[k].tl = old[j].tl; old[j].tl = NULL; g_ent[k].loading = old[j].loading; break; }
    }
    for (i = 0; i < oldN; i++) {
        if (!old[i].account && find_entry(old[i].id) < 0) {
            int k = add_entry(old[i].id, old[i].title, old[i].sub, FALSE);
            if (k >= 0) { g_ent[k].tl = old[i].tl; old[i].tl = NULL; g_ent[k].loading = old[i].loading; }
        }
        free_entry(&old[i]);
    }
    free(old);
    yt_free_playlists(&r->pls);
    free(r);

    ListView_SetItemCountEx(g_pls, g_entN, 0);
    InvalidateRect(g_pls, NULL, TRUE);
    set_status(g_entN ? L"" : L"ライブラリにプレイリストがありません", FALSE);
    if (viewId[0]) sel = find_entry(viewId);
    if (sel < 0 && g_cfg.lastPlaylist[0]) {
        sel = find_entry(g_cfg.lastPlaylist);
        if (sel < 0) sel = add_entry(g_cfg.lastPlaylist, NULL, NULL, FALSE), ListView_SetItemCountEx(g_pls, g_entN, 0);
    }
    if (sel >= 0) select_entry(sel);
    else show_view(-1);
}

static void on_playlist(PlResult *r)
{
    int i = find_entry(r->id);
    if (i < 0) i = add_entry(r->id, r->title, NULL, FALSE), ListView_SetItemCountEx(g_pls, g_entN, 0);
    if (i >= 0) {
        Entry *e = &g_ent[i];
        e->loading = FALSE;
        if (r->ok) {
            TL *t = tl_new(&r->tracks);
            tl_release(e->tl);
            e->tl = t;
            if (r->title && (!e->account || !e->title || !wcscmp(e->title, L"(読み込み中…)"))) {
                free(e->title);
                e->title = r->title;
                r->title = NULL;
                InvalidateRect(g_pls, NULL, FALSE);
            }
            if (i == g_view) show_view(i);
            set_status(L"", FALSE);
            if (g_playWhenLoaded[0] && !strcmp(g_playWhenLoaded, e->id)) {
                g_playWhenLoaded[0] = 0;
                play_from(e->tl, g_cfg.shuffle ? -1 : 0);
            }
        } else {
            set_status_u8(r->err, TRUE);
        }
    }
    yt_free_tracks(&r->tracks);
    free(r->id);
    free(r->title);
    free(r);
}

static void on_skip_error(const char *err)
{
    WCHAR msg[600], *w = U2W(err);
    g_failStreak++;
    if (g_failStreak >= 3) {
        _snwprintf(msg, ARRAYSIZE(msg), L"続けて %d 曲再生できなかったので止めました(%s)", g_failStreak, w ? w : L"");
        msg[ARRAYSIZE(msg) - 1] = 0;
        stop_playback(msg, TRUE);
    } else {
        set_status(w, TRUE);
        SetTimer(g_wnd, TIMER_SKIP, 1500, NULL);       /* 少しおいて次へ(連打しない) */
    }
    free(w);
}

static void on_audio(AudioResult *r)
{
    if (r->prefetch) {
        const char *nv = peek_next_vid();
        if (r->code == YT_OK && nv && !strcmp(nv, r->vid)) {
            clear_prefetch();
            g_pref = r->a;
            memset(&r->a, 0, sizeof(r->a));
            strcpy_s(g_prefVid, sizeof(g_prefVid), r->vid);
            strcpy_s(g_prefAsked, sizeof(g_prefAsked), r->vid);
        } else if (r->code == YT_STOP) {
            set_status_u8(r->err, TRUE);
        }
        free_audio_result(r);
        return;
    }
    if (r->gen != g_gen) { free_audio_result(r); return; }
    g_loadingAudio = FALSE;
    switch (r->code) {
    case YT_OK:
        player_open(&r->a, g_gen);
        break;
    case YT_SKIP:
        on_skip_error(r->err);
        break;
    default: {
        WCHAR *w = U2W(r->err);
        stop_playback(w, TRUE);
        free(w);
        break;
    }
    }
    free_audio_result(r);
}

static void on_player(int ev, int gen)
{
    if (gen != g_gen) return;
    if (ev == PEV_STARTED) {
        int dur;
        g_failStreak = 0;
        set_status(L"", FALSE);
        player_status(NULL, NULL, &dur, NULL);
        if (!dur && cur_track()) dur = cur_track()->dur * 1000;
        SendMessageW(g_seek, TBM_SETRANGEMAX, TRUE, dur / 1000 > 0 ? dur / 1000 : 1);
        SendMessageW(g_seek, TBM_SETPOS, TRUE, 0);
        ensure_timer(TRUE);
    } else if (ev == PEV_ENDED) {
        next_track(FALSE);
    } else if (ev == PEV_ERROR) {
        on_skip_error("この曲を再生できなかった(音声を読めない)");
    }
}

static void on_tick(void)
{
    int st, pos, dur, gen;
    player_status(&st, &pos, &dur, &gen);
    if (st != g_lastState) {
        g_lastState = st;
        InvalidateRect(g_btn[1], NULL, FALSE);
    }
    if (st == PS_PLAYING && gen == g_gen) {
        const char *nv;
        if (!dur && cur_track()) dur = cur_track()->dur * 1000;
        if (dur > 0 && dur - pos < PREFETCH_MS && (nv = peek_next_vid()) != NULL && strcmp(g_prefAsked, nv)) {
            strcpy_s(g_prefAsked, sizeof(g_prefAsked), nv);
            worker_audio(nv, 0, TRUE);
        }
    }
    if (!IsIconic(g_wnd)) {
        if (!g_dragging && gen == g_gen && st != PS_IDLE) SendMessageW(g_seek, TBM_SETPOS, TRUE, pos / 1000);
        else if (!g_dragging && st == PS_IDLE && !g_loadingAudio) SendMessageW(g_seek, TBM_SETPOS, TRUE, 0);
        invalidate_bar();
    }
    if (st != PS_PLAYING && !g_loadingAudio) ensure_timer(FALSE);   /* 止まっている間は起きない */
}

/* ------------------------------------------------------------------ */
/*  一覧                                                                */
/* ------------------------------------------------------------------ */

static void tracks_dispinfo(NMLVDISPINFOW *di)
{
    Track *t;
    int    i = di->item.iItem;
    if (!(di->item.mask & LVIF_TEXT) || !g_viewTL || i < 0 || i >= g_viewTL->t.n) return;
    t = &g_viewTL->t.v[i];
    switch (di->item.iSubItem) {
    case 0:
        if (g_viewTL == g_playTL && cur_track() == t) wcscpy_s(di->item.pszText, di->item.cchTextMax, L"\x25B6");
        else _snwprintf(di->item.pszText, (size_t)di->item.cchTextMax, L"%d", i + 1);
        break;
    case 1: wcsncpy_s(di->item.pszText, di->item.cchTextMax, t->title ? t->title : L"", _TRUNCATE); break;
    case 2: wcsncpy_s(di->item.pszText, di->item.cchTextMax, t->artist ? t->artist : L"", _TRUNCATE); break;
    case 3: wcsncpy_s(di->item.pszText, di->item.cchTextMax, t->album ? t->album : L"", _TRUNCATE); break;
    case 4:
        if (t->dur) fmt_time(di->item.pszText, (size_t)di->item.cchTextMax, t->dur * 1000);
        else di->item.pszText[0] = 0;
        break;
    }
}

static LRESULT tracks_customdraw(NMLVCUSTOMDRAW *cd)
{
    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        int i = (int)cd->nmcd.dwItemSpec;
        if (g_viewTL && i >= 0 && i < g_viewTL->t.n) {
            Track *t = &g_viewTL->t.v[i];
            if (g_viewTL == g_playTL && cur_track() == t) cd->clrText = theme_accent();
            else if (t->grey) cd->clrText = theme_dim_text();
        }
        return CDRF_DODEFAULT;
    }
    return CDRF_DODEFAULT;
}

static void pls_dispinfo(NMLVDISPINFOW *di)
{
    int i = di->item.iItem;
    if (!(di->item.mask & LVIF_TEXT) || i < 0 || i >= g_entN) return;
    wcsncpy_s(di->item.pszText, di->item.cchTextMax, g_ent[i].title ? g_ent[i].title : L"", _TRUNCATE);
}

/* ------------------------------------------------------------------ */
/*  メニューとコマンド                                                  */
/* ------------------------------------------------------------------ */

static void refresh_all(void)
{
    int i;
    for (i = 0; i < g_entN; i++) {
        if (i == g_view) continue;
        tl_release(g_ent[i].tl);       /* 次に開いたときに読み直す */
        g_ent[i].tl = NULL;
    }
    if (g_cfg.cookie) { worker_library(); set_status(L"ライブラリを読み込み中…", FALSE); }
    if (g_view >= 0) load_entry(g_view);
}

static void open_url(void)
{
    char *id = NULL;
    int   i;
    if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_OPENURL), g_wnd, url_proc, (LPARAM)&id) != IDOK || !id) return;
    i = find_entry(id);
    if (i < 0) { i = add_entry(id, NULL, NULL, FALSE); ListView_SetItemCountEx(g_pls, g_entN, 0); }
    free(id);
    if (i >= 0) select_entry(i);
}

static void logout(void)
{
    int i, j = 0;
    cfg_set_cookie(NULL);
    /* ライブラリから来たものは外す(URL で開いたものは残す) */
    for (i = 0; i < g_entN; i++) {
        if (g_ent[i].account) { free_entry(&g_ent[i]); continue; }
        g_ent[j++] = g_ent[i];
    }
    g_entN = j;
    ListView_SetItemCountEx(g_pls, g_entN, 0);
    show_view(-1);
    set_status(L"ログイン情報を消しました", FALSE);
}

static void show_menu(void)
{
    HMENU m = CreatePopupMenu();
    RECT  r;
    WCHAR ver[64];
    _snwprintf(ver, ARRAYSIZE(ver), L"%s v%S", APP_NAME, APP_VERSION);
    AppendMenuW(m, MF_STRING, ID_M_LOGIN, g_cfg.cookie ? L"ログイン情報を設定し直す…" : L"ログイン情報を設定…");
    AppendMenuW(m, MF_STRING, ID_M_OPENURL, L"プレイリストの URL を開く…");
    AppendMenuW(m, MF_STRING, ID_M_REFRESH, L"読み直す\tF5");
    if (g_cfg.cookie) AppendMenuW(m, MF_STRING, ID_M_LOGOUT, L"ログイン情報を消す");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | MF_GRAYED, ID_M_VERSION, ver);
    GetWindowRect(g_btn[5], &r);
    TrackPopupMenu(m, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, r.right, r.top, 0, g_wnd, NULL);
    DestroyMenu(m);
}

static void on_command(int id)
{
    switch (id) {
    case IDB_PREV: prev_track(); break;
    case IDB_PLAY: toggle_play(); break;
    case IDB_NEXT: if (cur_track()) next_track(TRUE); break;
    case IDB_SHUFFLE:
        g_cfg.shuffle = !g_cfg.shuffle;
        if (g_playTL && g_pos >= 0 && g_pos < g_orderN) {
            int cur = g_order[g_pos];
            build_order(cur);
            if (!g_cfg.shuffle) g_pos = cur;
            clear_prefetch();
        }
        InvalidateRect(g_btn[3], NULL, FALSE);
        break;
    case IDB_REPEAT:
        g_cfg.repeat = !g_cfg.repeat;
        clear_prefetch();
        InvalidateRect(g_btn[4], NULL, FALSE);
        break;
    case IDB_MENU: show_menu(); break;
    case ID_M_LOGIN:
        if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_LOGIN), g_wnd, login_proc, 0) == IDOK) {
            worker_library();
            set_status(L"ライブラリを読み込み中…", FALSE);
        }
        break;
    case ID_M_OPENURL: open_url(); break;
    case ID_M_REFRESH: refresh_all(); break;
    case ID_M_LOGOUT: logout(); break;
    }
}

/* ------------------------------------------------------------------ */
/*  窓                                                                  */
/* ------------------------------------------------------------------ */

static HWND make_button(int id)
{
    return CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10,
                           g_wnd, (HMENU)(INT_PTR)id, g_inst, NULL);
}

static HWND make_trackbar(int id, int max, int pos)
{
    HWND t = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS | TBS_FIXEDLENGTH,
                             0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(t, TBM_SETRANGE, FALSE, MAKELPARAM(0, max));
    SendMessageW(t, TBM_SETPOS, TRUE, pos);
    SendMessageW(t, TBM_SETTHUMBLENGTH, S(16), 0);
    SendMessageW(t, TBM_SETPAGESIZE, 0, max / 10 ? max / 10 : 1);
    SetWindowSubclass(t, trackbar_subclass, 1, 0);
    return t;
}

static void create_children(void)
{
    LVCOLUMNW col;
    static const WCHAR *names[5] = { L"#", L"タイトル", L"アーティスト", L"アルバム", L"時間" };
    int i;

    g_pls = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
                            LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                            0, 0, 10, 10, g_wnd, (HMENU)IDC_PLS, g_inst, NULL);
    g_tracks = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
                               LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                               0, 0, 10, 10, g_wnd, (HMENU)IDC_TRACKS, g_inst, NULL);
    ListView_SetExtendedListViewStyle(g_pls, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetExtendedListViewStyle(g_tracks, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 100;
    col.pszText = (LPWSTR)L"";
    ListView_InsertColumn(g_pls, 0, &col);
    for (i = 0; i < 5; i++) {
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = (i == 0 || i == 4) ? LVCFMT_RIGHT : LVCFMT_LEFT;
        col.pszText = (LPWSTR)names[i];
        ListView_InsertColumn(g_tracks, i, &col);
    }
    g_btn[0] = make_button(IDB_PREV);
    g_btn[1] = make_button(IDB_PLAY);
    g_btn[2] = make_button(IDB_NEXT);
    g_btn[3] = make_button(IDB_SHUFFLE);
    g_btn[4] = make_button(IDB_REPEAT);
    g_btn[5] = make_button(IDB_MENU);
    g_seek = make_trackbar(IDC_SEEK, 1, 0);
    g_vol = make_trackbar(IDC_VOL, 100, g_cfg.volume);
    SendMessageW(g_vol, TBM_SETPAGESIZE, 0, 10);
}

static void apply_fonts(void)
{
    SendMessageW(g_pls, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_tracks, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_seek, TBM_SETTHUMBLENGTH, S(16), 0);
    SendMessageW(g_vol, TBM_SETTHUMBLENGTH, S(16), 0);
}

static void apply_theme(void)
{
    theme_apply_window(g_wnd);
    InvalidateRect(g_wnd, NULL, TRUE);
}

static void save_placement(void)
{
    WINDOWPLACEMENT wp;
    wp.length = sizeof(wp);
    if (GetWindowPlacement(g_wnd, &wp)) {
        g_cfg.x = wp.rcNormalPosition.left;
        g_cfg.y = wp.rcNormalPosition.top;
        g_cfg.w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        g_cfg.h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
        g_cfg.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    }
}

static void register_media_keys(void)
{
    if (!g_cfg.mediakeys) return;
    RegisterHotKey(g_wnd, 1, MOD_NOREPEAT, VK_MEDIA_PLAY_PAUSE);
    RegisterHotKey(g_wnd, 2, MOD_NOREPEAT, VK_MEDIA_NEXT_TRACK);
    RegisterHotKey(g_wnd, 3, MOD_NOREPEAT, VK_MEDIA_PREV_TRACK);
    RegisterHotKey(g_wnd, 4, MOD_NOREPEAT, VK_MEDIA_STOP);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_wnd = h;
        g_dpi = GetDpiForWindow(h);
        create_fonts();
        create_children();
        apply_fonts();
        apply_theme();
        layout();
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mi = (MINMAXINFO *)lp;
        mi->ptMinTrackSize.x = S(560);
        mi->ptMinTrackSize.y = S(300);
        return 0;
    }
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        g_dpi = HIWORD(wp);
        create_fonts();
        apply_fonts();
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        /* 横棒の背景 */
        SetBkColor((HDC)wp, theme_footer());
        return (LRESULT)theme_footer_brush();
    case WM_CTLCOLORBTN:
        return (LRESULT)theme_footer_brush();
    case WM_DRAWITEM:
        draw_button((DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_SETTINGCHANGE:
        if (lp && !wcscmp((const WCHAR *)lp, L"ImmersiveColorSet") && theme_refresh()) apply_theme();
        break;
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->hwndFrom == g_tracks) {
            if (n->code == LVN_GETDISPINFOW) { tracks_dispinfo((NMLVDISPINFOW *)lp); return 0; }
            if (n->code == NM_CUSTOMDRAW) return tracks_customdraw((NMLVCUSTOMDRAW *)lp);
            if (n->code == LVN_ITEMACTIVATE) {
                int i = ((NMITEMACTIVATE *)lp)->iItem;
                if (i < 0) i = ListView_GetNextItem(g_tracks, -1, LVNI_SELECTED);
                if (i >= 0) play_from(g_viewTL, i);
                return 0;
            }
        } else if (n->hwndFrom == g_pls) {
            if (n->code == LVN_GETDISPINFOW) { pls_dispinfo((NMLVDISPINFOW *)lp); return 0; }
            if (n->code == LVN_ITEMCHANGED) {
                NMLISTVIEW *lv = (NMLISTVIEW *)lp;
                if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED) && lv->iItem != g_view) select_entry(lv->iItem);
                return 0;
            }
            if (n->code == LVN_ITEMACTIVATE) {         /* プレイリストのダブルクリックで頭から */
                int i = ((NMITEMACTIVATE *)lp)->iItem;
                if (i >= 0 && i < g_entN) {
                    if (g_ent[i].tl) play_from(g_ent[i].tl, g_cfg.shuffle ? -1 : 0);
                    else strncpy_s(g_playWhenLoaded, sizeof(g_playWhenLoaded), g_ent[i].id, _TRUNCATE);
                }
                return 0;
            }
        } else if ((n->hwndFrom == g_seek || n->hwndFrom == g_vol) && n->code == NM_CUSTOMDRAW) {
            return draw_trackbar((NMCUSTOMDRAW *)lp);
        }
        break;
    }
    case WM_HSCROLL:
        if ((HWND)lp == g_seek) {
            int code = LOWORD(wp);
            if (code == TB_THUMBTRACK) { g_dragging = TRUE; InvalidateRect(h, NULL, FALSE); }
            else if (code == TB_ENDTRACK) {
                g_dragging = FALSE;
                if (cur_track()) player_seek((int)SendMessageW(g_seek, TBM_GETPOS, 0, 0) * 1000);
            }
        } else if ((HWND)lp == g_vol) {
            g_cfg.volume = (int)SendMessageW(g_vol, TBM_GETPOS, 0, 0);
            player_volume(g_cfg.volume);
        }
        return 0;
    case WM_COMMAND:
        on_command(LOWORD(wp));
        return 0;
    case WM_HOTKEY:
        switch (wp) {
        case 1: toggle_play(); break;
        case 2: if (cur_track()) next_track(TRUE); break;
        case 3: prev_track(); break;
        case 4: stop_playback(NULL, FALSE); break;
        }
        return 0;
    case WM_APPCOMMAND:
        switch (GET_APPCOMMAND_LPARAM(lp)) {
        case APPCOMMAND_MEDIA_PLAY_PAUSE: toggle_play(); return TRUE;
        case APPCOMMAND_MEDIA_NEXTTRACK: if (cur_track()) next_track(TRUE); return TRUE;
        case APPCOMMAND_MEDIA_PREVIOUSTRACK: prev_track(); return TRUE;
        case APPCOMMAND_MEDIA_STOP: stop_playback(NULL, FALSE); return TRUE;
        }
        break;
    case WM_TIMER:
        if (wp == TIMER_TICK) on_tick();
        else if (wp == TIMER_SKIP) { KillTimer(h, TIMER_SKIP); next_track(FALSE); }
        return 0;
    case WM_APP_LIBRARY:  on_library((LibResult *)lp); return 0;
    case WM_APP_PLAYLIST: on_playlist((PlResult *)lp); return 0;
    case WM_APP_AUDIO:    on_audio((AudioResult *)lp); return 0;
    case WM_APP_PLAYER:   on_player((int)wp, (int)lp); InvalidateRect(g_btn[1], NULL, FALSE); return 0;
    case WM_CLOSE:
        save_placement();
        cfg_save();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        player_shutdown();
        worker_stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ini ごとの二重起動の判定 */
static DWORD ini_hash(void)
{
    DWORD  hsh = 2166136261u;
    WCHAR *p;
    for (p = g_cfg.ini; *p; p++) { hsh ^= (DWORD)towlower(*p); hsh *= 16777619u; }
    return hsh;
}

static BOOL CALLBACK find_other(HWND w, LPARAM lp)
{
    WCHAR cls[32];
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    if (!wcscmp(cls, CLASS_NAME) && (DWORD)(UINT_PTR)GetPropW(w, PROP_INI) == (DWORD)lp) {
        if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
        SetForegroundWindow(w);
        return FALSE;
    }
    return TRUE;
}

int ui_run(HINSTANCE inst, BOOL mute, BOOL noActivate)
{
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX icc;
    WCHAR  mname[64];
    HANDLE mtx;
    MSG    m;
    DWORD  hsh = ini_hash();
    int    x, y, w, h;

    g_mute = mute;
    _snwprintf(mname, ARRAYSIZE(mname), L"Local\\yum-lite-%08lx", hsh);
    mtx = CreateMutexW(NULL, TRUE, mname);
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        EnumWindows(find_other, (LPARAM)hsh);
        CloseHandle(mtx);
        return 0;
    }

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);
    theme_init();
    NetInit();

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);

    x = g_cfg.x; y = g_cfg.y; w = g_cfg.w; h = g_cfg.h;
    if (w <= 0 || h <= 0) { x = y = CW_USEDEFAULT; w = 960; h = 600; }
    else {
        /* 画面の外に出ていたら既定の位置へ */
        RECT r = { x, y, x + w, y + h };
        if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) { x = y = CW_USEDEFAULT; }
    }
    g_wnd = CreateWindowExW(0, CLASS_NAME, APP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, NULL, NULL, inst, NULL);
    if (!g_wnd) return 1;
    SetPropW(g_wnd, PROP_INI, (HANDLE)(UINT_PTR)hsh);
    if (g_cfg.w <= 0) {
        /* 初回: 96 DPI で 960x600 相当に */
        SetWindowPos(g_wnd, NULL, 0, 0, S(960), S(600), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (noActivate) {
        ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_wnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        ShowWindow(g_wnd, g_cfg.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    }
    UpdateWindow(g_wnd);

    player_start(g_wnd, mute);
    player_volume(g_cfg.volume);
    worker_start(g_wnd);
    register_media_keys();

    if (g_cfg.cookie) {
        worker_library();
        set_status(L"ライブラリを読み込み中…", FALSE);
    } else {
        set_status(L"ログイン情報が未設定です。右下の … から設定するか、プレイリストの URL を開いてください", FALSE);
        if (g_cfg.lastPlaylist[0]) {
            int i = add_entry(g_cfg.lastPlaylist, NULL, NULL, FALSE);
            ListView_SetItemCountEx(g_pls, g_entN, 0);
            if (i >= 0) select_entry(i);
        }
    }

    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && (m.hwnd == g_wnd || GetParent(m.hwnd) == g_wnd)) {
            BOOL ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
            if (m.wParam == VK_SPACE && !ctrl && !alt && (m.hwnd == g_tracks || m.hwnd == g_pls)) { toggle_play(); continue; }
            if (m.wParam == VK_RIGHT && ctrl) { if (cur_track()) next_track(TRUE); continue; }
            if (m.wParam == VK_LEFT && ctrl) { prev_track(); continue; }
            if (m.wParam == VK_F5) { refresh_all(); continue; }
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (mtx) CloseHandle(mtx);
    return 0;
}
