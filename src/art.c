/* ==================================================================
 * art.c - 再生中の曲のジャケット画像を、曲の一覧の中央の背景に薄く敷く
 *
 *  本人の指定(2026-10-13): 一覧の中央の背景に半透過で。曲名が読みづらくならず、
 *  絵も見える濃さに。曲が変わったら、新しい画像が届くまでは空にする(前の曲の絵を
 *  残すと紛らわしい)。
 *
 *  - 画像は SMTC に渡すために取っている JPEG(既定 544px)をそのまま使う。取りに行く
 *    回数は増えない。
 *  - 一覧の背景は単色なので、絵を背景色とあらかじめ混ぜたもの(= 半透過で重ねた見た目)を作り、
 *    一覧が行を描く前に、見えている範囲の中央へ貼る(art_paint)。スクロールしても絵は動かない。
 *  - JPEG のまま持っておき、大きさ・テーマ・DPI が変わったら WIC で作り直す。
 *    WIC(WindowsCodecs.dll)は再生中の ytm-lite に元から読み込まれている(2026-10-13 実測)。
 * ================================================================== */

#include "ytm.h"
#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")

#define ART_MAX_Y_DARK   89     /* ダーク: 絵の明るさの上限(背景 32) */
#define ART_MIN_Y_LIGHT  178    /* ライト: 絵の明るさの下限(背景 255) */

static HWND     g_list;
static BYTE    *g_jpeg;          /* いまの曲の画像(無ければ NULL) */
static size_t   g_jpegN;
static int      g_side;          /* いま敷いている絵の一辺(px)。0 = 敷いていない */
static COLORREF g_bg;
static int      g_alpha;

/* JPEG を一辺 side に収まるよう縮め、背景色 bg と alpha(0〜255)で混ぜた DIB にする */
static HBITMAP make_bitmap(const BYTE *data, size_t n, int side, COLORREF bg, int alpha)
{
    IWICImagingFactory    *f = NULL;
    IWICStream            *s = NULL;
    IWICBitmapDecoder     *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICBitmapScaler      *sc = NULL;
    IWICFormatConverter   *cv = NULL;
    HBITMAP                hb = NULL;
    BITMAPINFO             bi;
    BYTE                  *bits = NULL;
    UINT                   w = 0, h = 0;
    int                    dw, dh, i;
    int                    br = GetRValue(bg), bgc = GetGValue(bg), bb = GetBValue(bg);

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f)))
        return NULL;
    if (FAILED(IWICImagingFactory_CreateStream(f, &s)) ||
        FAILED(IWICStream_InitializeFromMemory(s, (BYTE *)data, (DWORD)n)) ||
        FAILED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)s, NULL, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) ||
        FAILED(IWICBitmapFrameDecode_GetSize(fr, &w, &h)) || !w || !h)
        goto done;
    /* 縦横比を保って一辺 side に収める。元(既定 544px)より大きい窓では引き伸ばす
       (本人の指定「可能な限り最大化」。薄く敷くので粗さは目立たない) */
    if (w >= h) { dw = side; dh = (int)((LONGLONG)side * h / w); }
    else        { dh = side; dw = (int)((LONGLONG)side * w / h); }
    if (dw < 1 || dh < 1) goto done;
    if (FAILED(IWICImagingFactory_CreateBitmapScaler(f, &sc)) ||
        FAILED(IWICBitmapScaler_Initialize(sc, (IWICBitmapSource *)fr, (UINT)dw, (UINT)dh, WICBitmapInterpolationModeFant)) ||
        FAILED(IWICImagingFactory_CreateFormatConverter(f, &cv)) ||
        FAILED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)sc, &GUID_WICPixelFormat32bppBGR,
                                              WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)))
        goto done;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = dw;
    bi.bmiHeader.biHeight = -dh;                /* 上から下へ */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    hb = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!hb) goto done;
    if (FAILED(IWICFormatConverter_CopyPixels(cv, NULL, (UINT)dw * 4, (UINT)dw * 4 * (UINT)dh, bits))) {
        DeleteObject(hb);
        hb = NULL;
        goto done;
    }
    /* 背景色に alpha で重ねた色にする(背景は単色なので、これで半透過と同じ見た目になる)。
       そのうえで明るさ(Y)に上限(ダーク)・下限(ライト)を設け、超えた画素は色味を保ったまま背景色の側へ寄せる。
       濃くしても、絵の明るい所(ダーク)・暗い所(ライト)で字が沈まないように。限度は灰色の字
       (再生できない曲。ダーク 170、ライト 96)がコントラスト比 3:1 を保てる明るさ(ダーク 89、ライト 178) */
    {
        int bgY = (54 * br + 183 * bgc + 19 * bb) >> 8;
        BOOL dark = bgY < 128;
        int  lim = dark ? ART_MAX_Y_DARK : ART_MIN_Y_LIGHT;
        for (i = 0; i < dw * dh; i++) {
            BYTE *p = bits + i * 4;
            int   b = bb  + (p[0] - bb)  * alpha / 255;
            int   g = bgc + (p[1] - bgc) * alpha / 255;
            int   r = br  + (p[2] - br)  * alpha / 255;
            int   y = (54 * r + 183 * g + 19 * b) >> 8;
            if ((dark && y > lim) || (!dark && y < lim)) {
                /* 背景色からの差を (lim - bgY) / (y - bgY) 倍に縮める */
                int num = lim - bgY, den = y - bgY;
                b = bb  + (b - bb)  * num / den;
                g = bgc + (g - bgc) * num / den;
                r = br  + (r - br)  * num / den;
            }
            p[0] = (BYTE)b;
            p[1] = (BYTE)g;
            p[2] = (BYTE)r;
            p[3] = 0;
        }
    }

done:
    if (cv) IWICFormatConverter_Release(cv);
    if (sc) IWICBitmapScaler_Release(sc);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (s) IWICStream_Release(s);
    IWICImagingFactory_Release(f);
    return hb;
}

/* いま貼っている絵(こちらが持つ)。g_side は作ったときの一辺の目安 */
static HBITMAP g_hbm;
static int     g_bmW, g_bmH;

static void drop_bitmap(void)
{
    if (g_hbm) DeleteObject(g_hbm);
    g_hbm = NULL;
    g_bmW = g_bmH = 0;
    g_side = 0;
}

/* 曲の一覧の見えている範囲(見出しの下)の中央 */
static void list_area(RECT *rc)
{
    RECT hd;
    HWND header = ListView_GetHeader(g_list);
    GetClientRect(g_list, rc);
    if (header && IsWindowVisible(header) && GetWindowRect(header, &hd)) rc->top += hd.bottom - hd.top;
}

void art_init(HWND list)
{
    g_list = list;
}

/* 一覧の大きさ・テーマ・設定に合わせて作り直す。force でなければ、一辺が変わったときだけ */
void art_refresh(BOOL force)
{
    RECT     rc;
    int      side, alpha;
    COLORREF bg;
    HBITMAP  hb;
    BITMAP   bm;

    if (!g_list) return;
    if (!g_jpeg || g_cfg.artOpacity <= 0) {
        if (g_hbm) { drop_bitmap(); InvalidateRect(g_list, NULL, TRUE); }
        return;
    }
    list_area(&rc);
    side = min(rc.right - rc.left, rc.bottom - rc.top);             /* 短い辺いっぱい(本人の指定「可能な限り最大化」) */
    if (side < 16) return;
    bg = theme_ctrl_back();
    alpha = g_cfg.artOpacity * 255 / 100;
    if (!force && g_hbm && side == g_side && bg == g_bg && alpha == g_alpha) return;
    hb = make_bitmap(g_jpeg, g_jpegN, side, bg, alpha);
    if (!hb) return;
    drop_bitmap();
    g_hbm = hb;
    GetObjectW(hb, sizeof(bm), &bm);
    g_bmW = bm.bmWidth;
    g_bmH = bm.bmHeight;
    g_side = side;
    g_bg = bg;
    g_alpha = alpha;
    InvalidateRect(g_list, NULL, TRUE);
}

/* 絵のうち、clip(一覧のクライアント座標)に重なる部分だけを貼る。
   一覧のカスタム描画から、行を描く直前(CDDS_ITEMPREPAINT)にその行の範囲を、描き始め(CDDS_PREPAINT)に
   行の無い下の空きを渡す。こうすると、絵を貼った範囲には必ずすぐ後で字が描かれ、字が絵の下に隠れない。
   (v14 は描き始めに絵を丸ごと貼っていたため、マウスを乗せた・スクロールしたときなど一部の行だけを
   描き直す経路で、ほかの行の字が絵に隠れた。2026-10-13 本人の報告)
   見えている範囲の中央に貼るので、スクロールしても絵は動かない。ListView 自身の背景画像(LVM_SETBKIMAGE)は
   中身と一緒にスクロールしてしまい、自動スクロールで絵が画面の外へ出た(2026-10-13 実測)のでやめた */
void art_paint(HDC dc, const RECT *clip)
{
    RECT    rc, art, part;
    HDC     mem;
    HGDIOBJ old;
    if (!g_hbm) return;
    list_area(&rc);
    art.left = rc.left + (rc.right - rc.left - g_bmW) / 2;
    art.top = rc.top + (rc.bottom - rc.top - g_bmH) / 2;
    art.right = art.left + g_bmW;
    art.bottom = art.top + g_bmH;
    if (!IntersectRect(&part, &art, clip)) return;
    if (!IntersectRect(&part, &part, &rc)) return;          /* 見出しの上には出さない */
    mem = CreateCompatibleDC(dc);
    if (!mem) return;
    old = SelectObject(mem, g_hbm);
    BitBlt(dc, part.left, part.top, part.right - part.left, part.bottom - part.top,
           mem, part.left - art.left, part.top - art.top, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
}

BOOL art_shown(void)
{
    return g_hbm != NULL;
}

/* 新しい曲の画像(NULL で消す) */
void art_set(const BYTE *jpeg, size_t n)
{
    free(g_jpeg);
    g_jpeg = NULL;
    g_jpegN = 0;
    if (jpeg && n) {
        g_jpeg = (BYTE *)malloc(n);
        if (g_jpeg) { memcpy(g_jpeg, jpeg, n); g_jpegN = n; }
    }
    art_refresh(TRUE);
}
