/* ==================================================================
 * slider.c - シークバーと音量の横棒(自前の部品)
 *
 *  v3 までは Windows の trackbar をカスタム描画していたが、trackbar は位置が変わっても
 *  つまみの周りしか描き直さない。溝の塗り(再生済みの赤)をこちらで描いていたので、
 *  曲の終わりやシークの後に赤が残り、丸いつまみの跡がギザギザに残った(2026-10-12、本人の指摘)。
 *  そこで、毎回全体を裏で描いて出す小さな部品にした。
 *
 *  親への知らせは trackbar と同じ形: WM_HSCROLL、LOWORD = TB_THUMBTRACK(動かしている間)/
 *  TB_ENDTRACK(離したとき)、lParam = この窓。
 * ================================================================== */

#include "ytm.h"
#include <windowsx.h>

typedef struct {
    int  max, pos;
    int  style;         /* SLS_SEEK / SLS_VOLUME */
    BOOL active;        /* シーク: 曲がある */
    BOOL drag, hot;
    int  wheel;         /* ホイールの回した量の端数(WHEEL_DELTA に満たない分) */
} Slider;

static int dpi_scale(HWND h, int v) { return MulDiv(v, (int)GetDpiForWindow(h), 96); }

static void geometry(HWND h, RECT *track, int *r)
{
    RECT rc;
    GetClientRect(h, &rc);
    *r = dpi_scale(h, 6);
    track->left = rc.left + *r + 1;
    track->right = rc.right - *r - 1;
    track->top = (rc.top + rc.bottom) / 2 - dpi_scale(h, 2);
    track->bottom = track->top + dpi_scale(h, 4);
}

static int thumb_x(Slider *s, const RECT *t)
{
    if (s->max <= 0) return t->left;
    return t->left + MulDiv(s->pos, t->right - t->left, s->max);
}

static void notify(HWND h, int code)
{
    SendMessageW(GetParent(h), WM_HSCROLL, MAKEWPARAM(code, 0), (LPARAM)h);
}

static void set_from_x(HWND h, Slider *s, int x)
{
    RECT t;
    int  r, v;
    geometry(h, &t, &r);
    v = t.right > t.left ? MulDiv(x - t.left, s->max, t.right - t.left) : 0;
    if (v < 0) v = 0;
    if (v > s->max) v = s->max;
    if (v != s->pos) { s->pos = v; InvalidateRect(h, NULL, FALSE); }
}

static void paint(HWND h, Slider *s, HDC hdc)
{
    RECT    rc, t, done;
    int     r, tx, cy;
    HDC     mem;
    HBITMAP bmp, ob;
    HBRUSH  b;
    BOOL    show = s->style == SLS_VOLUME || s->active;
    COLORREF fill = s->style == SLS_SEEK ? theme_accent() : theme_text();

    GetClientRect(h, &rc);
    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    ob = (HBITMAP)SelectObject(mem, bmp);
    FillRect(mem, &rc, theme_footer_brush());
    geometry(h, &t, &r);
    cy = (t.top + t.bottom) / 2;

    b = CreateSolidBrush(theme_line());
    FillRect(mem, &t, b);
    DeleteObject(b);
    if (show) {
        tx = thumb_x(s, &t);
        done = t;
        done.right = tx;
        if (done.right > done.left) {
            b = CreateSolidBrush(s->style == SLS_SEEK ? fill : theme_dim_text());
            FillRect(mem, &done, b);
            DeleteObject(b);
        }
        /* つまみ: 音量は常に、シークは曲があるとき。触っている間は少し大きく */
        {
            int    rr = (s->drag || s->hot) ? r : r - dpi_scale(h, 1);
            HBRUSH tb = CreateSolidBrush(fill);
            HBRUSH obr = (HBRUSH)SelectObject(mem, tb);
            HPEN   op = (HPEN)SelectObject(mem, GetStockObject(NULL_PEN));
            Ellipse(mem, tx - rr, cy - rr, tx + rr + 1, cy + rr + 1);
            SelectObject(mem, obr);
            SelectObject(mem, op);
            DeleteObject(tb);
        }
    }
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK slider_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Slider *s = (Slider *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        s = (Slider *)calloc(1, sizeof(Slider));
        if (!s) return FALSE;
        s->max = 100;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)s);
        break;
    case WM_NCDESTROY:
        free(s);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    case SLM_SETRANGE:
        s->max = (int)wp > 0 ? (int)wp : 1;
        if (s->pos > s->max) s->pos = s->max;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case SLM_SETPOS:
        if (!s->drag && (int)wp != s->pos) {
            s->pos = (int)wp < 0 ? 0 : (int)wp > s->max ? s->max : (int)wp;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case SLM_GETPOS:
        return s->pos;
    case SLM_SETSTYLE:
        s->style = (int)wp;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case SLM_SETACTIVE:
        if (s->active != (BOOL)wp) { s->active = (BOOL)wp; InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(h, s, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (s->style == SLS_SEEK && !s->active) return 0;
        SetCapture(h);
        s->drag = TRUE;
        set_from_x(h, s, GET_X_LPARAM(lp));
        notify(h, TB_THUMBTRACK);
        return 0;
    case WM_MOUSEMOVE:
        if (!s->hot) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            s->hot = TRUE;
            InvalidateRect(h, NULL, FALSE);
        }
        if (s->drag) { set_from_x(h, s, GET_X_LPARAM(lp)); notify(h, TB_THUMBTRACK); }
        return 0;
    case WM_MOUSELEAVE:
        s->hot = FALSE;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (s->drag) {
            s->drag = FALSE;
            ReleaseCapture();
            notify(h, TB_ENDTRACK);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (s->drag && (HWND)lp != h) { s->drag = FALSE; notify(h, TB_ENDTRACK); InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_MOUSEWHEEL: {
        /* 音量は 1 目盛り 5、シークは 1 目盛り 5 秒(本人の要望: シークバーもホイールで前後させる)。
           細かく回せるホイールは 1 回の値が 120 より小さいので、貯めて 1 目盛り分ごとに動かす */
        int notches, v;
        if (s->style == SLS_SEEK && (!s->active || s->drag)) return 0;
        s->wheel += GET_WHEEL_DELTA_WPARAM(wp);
        notches = s->wheel / WHEEL_DELTA;
        s->wheel -= notches * WHEEL_DELTA;
        if (!notches) return 0;
        v = s->pos + notches * 5;
        s->pos = v < 0 ? 0 : v > s->max ? s->max : v;
        InvalidateRect(h, NULL, FALSE);
        notify(h, TB_THUMBTRACK);
        notify(h, TB_ENDTRACK);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void slider_register(HINSTANCE inst)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = slider_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_HAND);
    wc.lpszClassName = SLIDER_CLASS;
    RegisterClassExW(&wc);
}
