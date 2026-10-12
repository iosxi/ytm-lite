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

#include "yum.h"
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

static void title_bar(HWND hwnd)
{
    BOOL on = g_dark;
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &on, sizeof(on))))
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &on, sizeof(on));
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
    SetWindowTheme(lv, g_dark ? L"DarkMode_ItemsView" : L"ItemsView", NULL);
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
        SetWindowTheme(c, g_dark ? L"DarkMode_CFD" : NULL, NULL);
    } else if (!lstrcmpiW(cls, L"Button")) {
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
