/* ==================================================================
 * art.c - 再生中の曲のジャケット画像を、真ん中の欄に描く
 *
 *  本人の指定(2026-10-13): YouTube Music の画面に倣った 3 欄(左にライブラリ、真ん中に
 *  ジャケット画像と操作、右に曲の一覧)。曲が変わったら、新しい画像が届くまでは空にする
 *  (前の曲の絵を残すと紛らわしい)。
 *
 *  - 画像は SMTC に渡すために取っている JPEG(既定 544px)をそのまま使う。取りに行く
 *    回数は増えない。
 *  - JPEG のまま持っておき、描く大きさが変わったら WIC で縮め直す(DIB は 1 枚だけ)。
 *    WIC(WindowsCodecs.dll)は再生中の ytm-lite に元から読み込まれている(2026-10-13 実測)。
 *
 *  v14・v15 は曲の一覧の背景に半透過で敷いていたが、字との重なりや濃さの調整が難しく、
 *  この形に改めた(本人の指示)。
 * ================================================================== */

#include "ytm.h"
#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")

static BYTE   *g_jpeg;          /* いまの曲の画像(無ければ NULL) */
static size_t  g_jpegN;
static HBITMAP g_hbm;           /* g_side に収まるよう縮めたもの */
static int     g_side, g_bmW, g_bmH;

/* JPEG を一辺 side の正方形に収まるよう縮めた DIB にする(縦横比は保つ) */
static HBITMAP make_bitmap(const BYTE *data, size_t n, int side, int *outW, int *outH)
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
    int                    dw, dh;

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f)))
        return NULL;
    if (FAILED(IWICImagingFactory_CreateStream(f, &s)) ||
        FAILED(IWICStream_InitializeFromMemory(s, (BYTE *)data, (DWORD)n)) ||
        FAILED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)s, NULL, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) ||
        FAILED(IWICBitmapFrameDecode_GetSize(fr, &w, &h)) || !w || !h)
        goto done;
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
    *outW = dw;
    *outH = dh;

done:
    if (cv) IWICFormatConverter_Release(cv);
    if (sc) IWICBitmapScaler_Release(sc);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (s) IWICStream_Release(s);
    IWICImagingFactory_Release(f);
    return hb;
}

static void drop_bitmap(void)
{
    if (g_hbm) DeleteObject(g_hbm);
    g_hbm = NULL;
    g_side = g_bmW = g_bmH = 0;
}

/* 新しい曲の画像(NULL で消す)。描き直しは呼ぶ側で */
void art_set(const BYTE *jpeg, size_t n)
{
    free(g_jpeg);
    g_jpeg = NULL;
    g_jpegN = 0;
    drop_bitmap();
    if (jpeg && n) {
        g_jpeg = (BYTE *)malloc(n);
        if (g_jpeg) { memcpy(g_jpeg, jpeg, n); g_jpegN = n; }
    }
}

/* r の中央に、r に収まる大きさで描く。画像が無い(または読めない)ときは FALSE(呼ぶ側が空の枠を描く) */
BOOL art_draw(HDC dc, const RECT *r)
{
    int     side = min(r->right - r->left, r->bottom - r->top);
    HDC     mem;
    HGDIOBJ old;
    if (!g_jpeg || side < 8) return FALSE;
    if (!g_hbm || g_side != side) {
        int w = 0, h = 0;
        HBITMAP hb = make_bitmap(g_jpeg, g_jpegN, side, &w, &h);
        if (!hb) return FALSE;
        drop_bitmap();
        g_hbm = hb;
        g_side = side;
        g_bmW = w;
        g_bmH = h;
    }
    mem = CreateCompatibleDC(dc);
    if (!mem) return FALSE;
    old = SelectObject(mem, g_hbm);
    BitBlt(dc, r->left + (r->right - r->left - g_bmW) / 2, r->top + (r->bottom - r->top - g_bmH) / 2,
           g_bmW, g_bmH, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    return TRUE;
}
