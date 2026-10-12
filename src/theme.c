/* ==================================================================
 * theme.c - システムのライト/ダークに合わせた配色
 *
 *  kotemado / mayous / iiv の theme.c を元にしたもの。Win32 の古いコントロールには
 *  ダークモードの公式 API が無いので、Windows 自身(エクスプローラなど)と
 *  同じく uxtheme.dll の序数エクスポートを使う。取れなければライトのまま動く。
 *
 *      序数 104  RefreshImmersiveColorPolicyState()
 *      序数 132  ShouldAppsUseDarkMode() -> BYTE
 *      序数 133  AllowDarkModeForWindow(HWND, BYTE)
 *      序数 135  SetPreferredAppMode(1 = AllowDark)
 *      序数 136  FlushMenuThemes()
 * ================================================================== */

#include "ytm.h"
#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19

typedef BYTE (WINAPI *fnShouldAppsUseDarkMode)(void);
typedef BYTE (WINAPI *fnAllowDarkModeForWindow)(HWND, BYTE);
typedef int  (WINAPI *fnSetPreferredAppMode)(int);
typedef void (WINAPI *fnVoid)(void);

static fnShouldAppsUseDarkMode  p_ShouldAppsUseDarkMode;
static fnAllowDarkModeForWindow p_AllowDarkModeForWindow;
static fnSetPreferredAppMode    p_SetPreferredAppMode;
static fnVoid                   p_RefreshImmersiveColorPolicyState;
static fnVoid                   p_FlushMenuThemes;

static BOOL   g_ready, g_dark;
static HBRUSH g_brBack, g_brFooter, g_brCtrl;

void theme_init(void)
{
    HMODULE ux;

    if (g_ready) return;
    g_ready = TRUE;

    ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (ux) {
        p_RefreshImmersiveColorPolicyState = (fnVoid)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(104));
        p_ShouldAppsUseDarkMode  = (fnShouldAppsUseDarkMode)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(132));
        p_AllowDarkModeForWindow = (fnAllowDarkModeForWindow)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(133));
        p_SetPreferredAppMode    = (fnSetPreferredAppMode)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        p_FlushMenuThemes        = (fnVoid)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    }
    if (p_SetPreferredAppMode) p_SetPreferredAppMode(1);
    if (p_RefreshImmersiveColorPolicyState) p_RefreshImmersiveColorPolicyState();
    theme_refresh();
}

/* システムの設定を読み直す。変わっていたら TRUE */
BOOL theme_refresh(void)
{
    BOOL dark;

    if (p_RefreshImmersiveColorPolicyState) p_RefreshImmersiveColorPolicyState();
    if (g_cfg.theme == 1)      dark = FALSE;
    else if (g_cfg.theme == 2) dark = TRUE;
    else dark = p_ShouldAppsUseDarkMode ? p_ShouldAppsUseDarkMode() != 0 : FALSE;
    if (g_brBack && dark == g_dark) return FALSE;

    g_dark = dark;
    if (g_brBack)   DeleteObject(g_brBack);
    if (g_brFooter) DeleteObject(g_brFooter);
    if (g_brCtrl)   DeleteObject(g_brCtrl);
    g_brBack   = CreateSolidBrush(theme_back());
    g_brFooter = CreateSolidBrush(theme_footer());
    g_brCtrl   = CreateSolidBrush(theme_ctrl_back());
    if (p_FlushMenuThemes) p_FlushMenuThemes();
    return TRUE;
}

BOOL     theme_is_dark(void)    { return g_dark; }
COLORREF theme_back(void)       { return g_dark ? RGB(32, 32, 32)    : GetSysColor(COLOR_WINDOW); }
COLORREF theme_footer(void)     { return g_dark ? RGB(44, 44, 44)    : RGB(243, 243, 243); }
COLORREF theme_ctrl_back(void)  { return g_dark ? RGB(32, 32, 32)    : GetSysColor(COLOR_WINDOW); }
COLORREF theme_text(void)       { return g_dark ? RGB(255, 255, 255) : GetSysColor(COLOR_WINDOWTEXT); }
COLORREF theme_dim_text(void)   { return g_dark ? RGB(170, 170, 170) : RGB(96, 96, 96); }
COLORREF theme_line(void)       { return g_dark ? RGB(60, 60, 60)    : RGB(223, 223, 223); }
COLORREF theme_accent(void)     { return g_dark ? RGB(255, 78, 69)   : RGB(204, 0, 0); }
HBRUSH   theme_back_brush(void)   { return g_brBack; }
HBRUSH   theme_footer_brush(void) { return g_brFooter; }
HBRUSH   theme_ctrl_brush(void)   { return g_brCtrl; }

void theme_allow_dark(HWND hwnd)
{
    if (p_AllowDarkModeForWindow) p_AllowDarkModeForWindow(hwnd, (BYTE)g_dark);
}

/* タイトルバーと枠の色。DWM の属性に加えて、uxtheme が枠を描くとき用の
   「ダークの色を使う」(SetWindowCompositionAttribute の WCA_USEDARKMODECOLORS)も付ける。
   2026-10-12: ダイアログを動かしたあと、タイトルバーと太い枠が白くなったと本人から報告
   (こちらでは再現できず。DWM の属性は 1 のままだった) */
typedef struct { DWORD attr; PVOID data; SIZE_T size; } WCADATA;
typedef BOOL (WINAPI *fnSetWindowCompositionAttribute)(HWND, WCADATA *);

void theme_title_bar(HWND hwnd)
{
    static fnSetWindowCompositionAttribute pSWCA;
    static BOOL looked;
    BOOL on = g_dark;
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &on, sizeof(on))))
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &on, sizeof(on));
    if (!looked) {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        looked = TRUE;
        if (u) pSWCA = (fnSetWindowCompositionAttribute)(void *)GetProcAddress(u, "SetWindowCompositionAttribute");
    }
    if (pSWCA) {
        WCADATA d = { 26, &on, sizeof(on) };     /* WCA_USEDARKMODECOLORS */
        pSWCA(hwnd, &d);
    }
}

#define title_bar theme_title_bar

/* 入力欄の縁(WS_EX_CLIENTEDGE の 2px、WS_BORDER の 1px)は、ダークでもテーマが明るい線で描くので
   描き直す。入力中だけ強調色にする(obtan の EditSubclass と同じ) */
static LRESULT CALLBACK edit_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    LRESULT r = DefSubclassProc(h, msg, wp, lp);
    (void)id; (void)ref;
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_ENABLE) {
        RedrawWindow(h, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
    } else if (msg == WM_NCPAINT) {
        /* ライトでもテーマの縁はほとんど見えない(2026-10-12 撮影)ので、両方で描く */
        int edge = (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) ? 2 : (GetWindowLongW(h, GWL_STYLE) & WS_BORDER) ? 1 : 0;
        if (edge) {
            HDC    dc = GetWindowDC(h);
            RECT   rc;
            HBRUSH line = CreateSolidBrush(GetFocus() == h ? theme_accent() : g_dark ? RGB(0x5a, 0x5a, 0x5a) : RGB(0xa0, 0xa0, 0xa0));
            GetWindowRect(h, &rc);
            OffsetRect(&rc, -rc.left, -rc.top);
            FrameRect(dc, &rc, line);
            if (edge == 2) {
                InflateRect(&rc, -1, -1);
                FrameRect(dc, &rc, g_brCtrl);
            }
            DeleteObject(line);
            ReleaseDC(h, dc);
        }
    }
    return r;
}

/* 押しボタンは自前で描く。テーマ任せだと文字の色を指定できず、Windows の版によっては
   黒地に黒い文字になりうる(Windows 11 23H2 でチェックボックスの文字が黒くなった。obtan v4)。
   既定のボタン(Enter で押されるもの)は枠を強調色にする */
void theme_draw_button(const DRAWITEMSTRUCT *di)
{
    HDC      dc = di->hDC;
    RECT     r = di->rcItem;
    WCHAR    text[128];
    HWND     b = di->hwndItem;
    int      dpi = (int)GetDpiForWindow(b), rad = MulDiv(4, dpi, 96);
    BOOL     pressed = (di->itemState & ODS_SELECTED) != 0, disabled = (di->itemState & ODS_DISABLED) != 0;
    BOOL     def = (BOOL)(INT_PTR)GetPropW(b, L"ytm.defbtn") || (di->itemState & ODS_FOCUS);
    COLORREF face = g_dark ? (pressed ? RGB(40, 40, 40) : RGB(55, 55, 55)) : (pressed ? RGB(229, 229, 229) : RGB(253, 253, 253));
    COLORREF edge = def ? theme_accent() : (g_dark ? RGB(80, 80, 80) : RGB(204, 204, 204));
    HBRUSH   fb, ob;
    HPEN     pen, op;
    HFONT    of;

    FillRect(dc, &r, g_brBack);
    fb = CreateSolidBrush(face);
    pen = CreatePen(PS_SOLID, 1, edge);
    ob = (HBRUSH)SelectObject(dc, fb);
    op = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad * 2, rad * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(fb);
    DeleteObject(pen);

    GetWindowTextW(b, text, ARRAYSIZE(text));
    of = (HFONT)SelectObject(dc, (HFONT)SendMessageW(b, WM_GETFONT, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? theme_dim_text() : theme_text());
    DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_HIDEPREFIX);
    SelectObject(dc, of);
}

/* ダイアログの押しボタンを自前で描く形に変える(既定のボタンは印を付けて覚えておく) */
static void own_push_button(HWND c)
{
    LONG st = GetWindowLongW(c, GWL_STYLE), type = st & BS_TYPEMASK;
    if (type != BS_PUSHBUTTON && type != BS_DEFPUSHBUTTON) return;
    if (type == BS_DEFPUSHBUTTON) SetPropW(c, L"ytm.defbtn", (HANDLE)1);
    SetWindowLongW(c, GWL_STYLE, (st & ~BS_TYPEMASK) | BS_OWNERDRAW);
}

/* 一覧(ListView)の見出しの文字色 */
static LRESULT CALLBACK lv_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    if (msg == WM_NOTIFY && g_dark) {
        NMHDR *n = (NMHDR *)lp;
        if (n->code == NM_CUSTOMDRAW && n->hwndFrom == ListView_GetHeader(h)) {
            NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)lp;
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(cd->hdc, theme_text());
                return CDRF_DODEFAULT;
            }
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void apply_listview(HWND lv)
{
    HWND hd = ListView_GetHeader(lv);
    SetWindowSubclass(lv, lv_subclass, 1, 0);
    theme_allow_dark(lv);
    /* 一覧そのものは DarkMode_Explorer(スクロールバーも暗くなる。ItemsView だと白いまま。
       2026-10-13 本人の指摘)、見出しは DarkMode_ItemsView。obtan と同じ組み合わせ */
    SetWindowTheme(lv, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    if (hd) {
        theme_allow_dark(hd);
        SetWindowTheme(hd, g_dark ? L"DarkMode_ItemsView" : NULL, NULL);
    }
    ListView_SetBkColor(lv, theme_ctrl_back());
    ListView_SetTextBkColor(lv, theme_ctrl_back());
    ListView_SetTextColor(lv, theme_text());
}

static BOOL CALLBACK apply_child(HWND c, LPARAM lp)
{
    WCHAR cls[32];
    (void)lp;

    GetClassNameW(c, cls, ARRAYSIZE(cls));
    theme_allow_dark(c);
    if (!lstrcmpiW(cls, WC_LISTVIEWW)) {
        apply_listview(c);
    } else if (!lstrcmpiW(cls, L"Edit")) {
        /* 複数行の入力欄はスクロールバーも暗くしたいので DarkMode_Explorer(色は WM_CTLCOLOREDIT で決める) */
        SetWindowTheme(c, g_dark ? ((GetWindowLongW(c, GWL_STYLE) & ES_MULTILINE) ? L"DarkMode_Explorer" : L"DarkMode_CFD") : NULL, NULL);
        SetWindowSubclass(c, edit_subclass, 3, 0);
        RedrawWindow(c, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
    } else if (!lstrcmpiW(cls, L"Button")) {
        own_push_button(c);         /* 本体の帯のボタンは元から自前で描いているので変わらない */
        SetWindowTheme(c, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
    } else if (!lstrcmpiW(cls, L"ScrollBar")) {
        SetWindowTheme(c, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
    }
    SendMessageW(c, WM_THEMECHANGED, 0, 0);
    return TRUE;
}

/* 窓(ダイアログも)と、その子をいまの配色にする */
void theme_apply_window(HWND w)
{
    theme_allow_dark(w);
    title_bar(w);
    EnumChildWindows(w, apply_child, 0);
    RedrawWindow(w, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

/* WM_CTLCOLOR* の共通処理。処理しないときは 0 を返す */
LRESULT theme_ctlcolor(UINT msg, HDC dc, HWND ctl, BOOL dimText)
{
    WCHAR cls[16];

    switch (msg) {
    case WM_CTLCOLORDLG:
        return (LRESULT)g_brBack;

    case WM_CTLCOLORSTATIC:
        GetClassNameW(ctl, cls, ARRAYSIZE(cls));
        if (!lstrcmpiW(cls, L"Edit")) {         /* 読み取り専用・無効のエディット */
            if (!g_dark) return 0;
            SetTextColor(dc, theme_dim_text());
            SetBkColor(dc, theme_ctrl_back());
            return (LRESULT)g_brCtrl;
        }
        SetTextColor(dc, dimText ? theme_dim_text() : theme_text());
        SetBkColor(dc, theme_back());
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)g_brBack;

    case WM_CTLCOLORBTN:
        return (LRESULT)g_brBack;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (!g_dark) return 0;
        SetTextColor(dc, theme_text());
        SetBkColor(dc, theme_ctrl_back());
        return (LRESULT)g_brCtrl;
    }
    return 0;
}
