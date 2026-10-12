/* ==================================================================
 * smtc.c - Windows のメディア コントロール(SMTC)に再生情報を出す
 *
 *  muutask(windows\muutask)などは、Windows の GlobalSystemMediaTransportControls
 *  (GSMTC)から「いま再生中の曲」を読み、再生・一時停止・次・前・位置の変更を頼んでくる。
 *  ここで SystemMediaTransportControls に曲名・状態・位置を出し、頼まれた操作は
 *  WM_APP_SMTC で画面のスレッドへ渡す。音量の表示(Windows のメディアのフライアウト)にも出る。
 *
 *  - WinRT を C から使う(SDK の winrt\windows.media.h の C 向けの宣言)。IID は C 向けには
 *    実体が無いので、ヘッダーの C++ 側の MIDL_INTERFACE の値を写して定義する。
 *  - ボタンの知らせは別のスレッドから来るので、受け手は IAgileObject を名乗り、
 *    PostMessage するだけにする。
 *  - 画面のスレッド(STA)から呼ぶ。
 * ================================================================== */

#include "ytm.h"
#include <roapi.h>
#include <winstring.h>
#include <initguid.h>
#include <windows.media.h>
#include <SystemMediaTransportControlsInterop.h>

#pragma comment(lib, "runtimeobject.lib")

/* windows.media.h の C++ 側の MIDL_INTERFACE / uuid の値(SDK 10.0.26100.0) */
DEFINE_GUID(IID_SMTC_Interop, 0xddb0472d, 0xc911, 0x4a1f, 0x86, 0xd9, 0xdc, 0x3d, 0x71, 0xa9, 0x5f, 0x5a);
DEFINE_GUID(IID_SMTC, 0x99fa3ff4, 0x1742, 0x42a6, 0x90, 0x2e, 0x08, 0x7d, 0x41, 0xf9, 0x65, 0xec);
DEFINE_GUID(IID_SMTC2, 0xea98d2f6, 0x7f3c, 0x4af2, 0xa5, 0x86, 0x72, 0x88, 0x98, 0x08, 0xef, 0xb1);
DEFINE_GUID(IID_SMTC_Timeline, 0x5125316a, 0xc3a2, 0x475b, 0x85, 0x07, 0x93, 0x53, 0x4d, 0xc8, 0x8f, 0x15);
DEFINE_GUID(IID_MusicProps2, 0x00368462, 0x97d3, 0x44b9, 0xb0, 0x0f, 0x00, 0x8a, 0xfc, 0xef, 0xaf, 0x18);
DEFINE_GUID(IID_ButtonHandler, 0x0557e996, 0x7b23, 0x5bae, 0xaa, 0x81, 0xea, 0x0d, 0x67, 0x11, 0x43, 0xa4);
DEFINE_GUID(IID_PositionHandler, 0x44e34f15, 0xbdc0, 0x50a7, 0xac, 0xe4, 0x39, 0xe9, 0x1f, 0xb7, 0x53, 0xf1);
DEFINE_GUID(IID_Agile, 0x94ea2b94, 0xe9cc, 0x49e0, 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90);

typedef __x_ABI_CWindows_CMedia_CISystemMediaTransportControls                 Smtc;
typedef __x_ABI_CWindows_CMedia_CISystemMediaTransportControls2                Smtc2;
typedef __x_ABI_CWindows_CMedia_CISystemMediaTransportControlsDisplayUpdater   Updater;
typedef __x_ABI_CWindows_CMedia_CIMusicDisplayProperties                       Music;
typedef __x_ABI_CWindows_CMedia_CIMusicDisplayProperties2                      Music2;
typedef __x_ABI_CWindows_CMedia_CISystemMediaTransportControlsTimelineProperties Timeline;

static HWND      g_hwnd;
static Smtc     *g_smtc;
static Smtc2    *g_smtc2;
static Timeline *g_tl;
static EventRegistrationToken g_tokButton, g_tokPos;

/* ------------------------------------------------------------------ */
/*  知らせの受け手(ボタンと位置の変更で 1 つずつ。中身は同じ形)          */
/* ------------------------------------------------------------------ */

typedef struct Handler {
    const void *lpVtbl;
    LONG        ref;
    const GUID *iid;
    BOOL        position;
} Handler;

static HRESULT STDMETHODCALLTYPE h_qi(Handler *h, REFIID riid, void **out)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_Agile) || IsEqualGUID(riid, h->iid)) {
        *out = h;
        InterlockedIncrement(&h->ref);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE h_addref(Handler *h) { return (ULONG)InterlockedIncrement(&h->ref); }
static ULONG STDMETHODCALLTYPE h_release(Handler *h)
{
    LONG r = InterlockedDecrement(&h->ref);
    if (!r) free(h);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE button_invoke(Handler *h, void *sender,
                                               __x_ABI_CWindows_CMedia_CISystemMediaTransportControlsButtonPressedEventArgs *args)
{
    enum __x_ABI_CWindows_CMedia_CSystemMediaTransportControlsButton b;
    (void)h; (void)sender;
    if (args && SUCCEEDED(__x_ABI_CWindows_CMedia_CISystemMediaTransportControlsButtonPressedEventArgs_get_Button(args, &b)))
        PostMessageW(g_hwnd, WM_APP_SMTC, (WPARAM)b, 0);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE position_invoke(Handler *h, void *sender,
                                                 __x_ABI_CWindows_CMedia_CIPlaybackPositionChangeRequestedEventArgs *args)
{
    __x_ABI_CWindows_CFoundation_CTimeSpan t;
    (void)h; (void)sender;
    if (args && SUCCEEDED(__x_ABI_CWindows_CMedia_CIPlaybackPositionChangeRequestedEventArgs_get_RequestedPlaybackPosition(args, &t)))
        PostMessageW(g_hwnd, WM_APP_SMTC, SMTC_SEEK, (LPARAM)(t.Duration / 10000));
    return S_OK;
}

typedef struct {
    HRESULT (STDMETHODCALLTYPE *qi)(Handler *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *addref)(Handler *);
    ULONG   (STDMETHODCALLTYPE *release)(Handler *);
    void    *invoke;
} HandlerVtbl;

static const HandlerVtbl g_buttonVtbl   = { h_qi, h_addref, h_release, (void *)button_invoke };
static const HandlerVtbl g_positionVtbl = { h_qi, h_addref, h_release, (void *)position_invoke };

static Handler *new_handler(const HandlerVtbl *vt, const GUID *iid)
{
    Handler *h = (Handler *)calloc(1, sizeof(Handler));
    if (!h) return NULL;
    h->lpVtbl = vt;
    h->ref = 1;
    h->iid = iid;
    return h;
}

/* ------------------------------------------------------------------ */

static HSTRING hs(const WCHAR *s)
{
    HSTRING h = NULL;
    if (s) WindowsCreateString(s, (UINT32)wcslen(s), &h);
    return h;
}

BOOL smtc_init(HWND hwnd)
{
    ISystemMediaTransportControlsInterop *interop = NULL;
    HSTRING  cls = hs(RuntimeClass_Windows_Media_SystemMediaTransportControls);
    Handler *bh, *ph;
    HRESULT  hr;

    g_hwnd = hwnd;
    hr = RoGetActivationFactory(cls, &IID_SMTC_Interop, (void **)&interop);
    WindowsDeleteString(cls);
    if (FAILED(hr)) { log_printf("SMTC: no interop factory 0x%08lx", hr); return FALSE; }
    hr = interop->lpVtbl->GetForWindow(interop, hwnd, &IID_SMTC, (void **)&g_smtc);
    interop->lpVtbl->Release(interop);
    if (FAILED(hr) || !g_smtc) { log_printf("SMTC: GetForWindow failed 0x%08lx", hr); g_smtc = NULL; return FALSE; }

    __x_ABI_CWindows_CMedia_CISystemMediaTransportControls_put_IsEnabled(g_smtc, TRUE);
    __x_ABI_CWindows_CMedia_CISystemMediaTransportControls_put_IsPlayEnabled(g_smtc, TRUE);
    __x_ABI_CWindows_CMedia_CISystemMediaTransportControls_put_IsPauseEnabled(g_smtc, TRUE);
    __x_ABI_CWindows_CMedia_CISystemMediaTransportControls_put_IsNextEnabled(g_smtc, TRUE);
    __x_ABI_CWindows_CMedia_CISystemMediaTransportControls_put_IsPreviousEnabled(g_smtc, TRUE);
    __x_ABI_CWindows_CMedia_CISystemMediaTransportControls_put_IsStopEnabled(g_smtc, TRUE);
    __x_ABI_CWindows_CMedia_CISystemMediaTransportControls_put_PlaybackStatus(g_smtc, MediaPlaybackStatus_Closed);

    bh = new_handler(&g_buttonVtbl, &IID_ButtonHandler);
    if (bh) {
        __x_ABI_CWindows_CMedia_CISystemMediaTransportControls_add_ButtonPressed(g_smtc,
            (__FITypedEventHandler_2_Windows__CMedia__CSystemMediaTransportControls_Windows__CMedia__CSystemMediaTransportControlsButtonPressedEventArgs *)bh,
            &g_tokButton);
        h_release(bh);              /* SMTC が持つ */
    }
    if (SUCCEEDED(__x_ABI_CWindows_CMedia_CISystemMediaTransportControls_QueryInterface(g_smtc, &IID_SMTC2, (void **)&g_smtc2))) {
        HSTRING tcls = hs(RuntimeClass_Windows_Media_SystemMediaTransportControlsTimelineProperties);
        IInspectable *ins = NULL;
        if (SUCCEEDED(RoActivateInstance(tcls, &ins))) {
            ins->lpVtbl->QueryInterface(ins, &IID_SMTC_Timeline, (void **)&g_tl);
            ins->lpVtbl->Release(ins);
        }
        WindowsDeleteString(tcls);
        ph = new_handler(&g_positionVtbl, &IID_PositionHandler);
        if (ph) {
            __x_ABI_CWindows_CMedia_CISystemMediaTransportControls2_add_PlaybackPositionChangeRequested(g_smtc2,
                (__FITypedEventHandler_2_Windows__CMedia__CSystemMediaTransportControls_Windows__CMedia__CPlaybackPositionChangeRequestedEventArgs *)ph,
                &g_tokPos);
            h_release(ph);
        }
    }
    log_printf("SMTC: ready (timeline %s)", g_tl ? "yes" : "no");
    return TRUE;
}

void smtc_shutdown(void)
{
    if (g_smtc2) {
        __x_ABI_CWindows_CMedia_CISystemMediaTransportControls2_remove_PlaybackPositionChangeRequested(g_smtc2, g_tokPos);
        __x_ABI_CWindows_CMedia_CISystemMediaTransportControls2_Release(g_smtc2);
        g_smtc2 = NULL;
    }
    if (g_tl) { __x_ABI_CWindows_CMedia_CISystemMediaTransportControlsTimelineProperties_Release(g_tl); g_tl = NULL; }
    if (g_smtc) {
        __x_ABI_CWindows_CMedia_CISystemMediaTransportControls_remove_ButtonPressed(g_smtc, g_tokButton);
        __x_ABI_CWindows_CMedia_CISystemMediaTransportControls_put_IsEnabled(g_smtc, FALSE);
        __x_ABI_CWindows_CMedia_CISystemMediaTransportControls_Release(g_smtc);
        g_smtc = NULL;
    }
}

void smtc_track(const WCHAR *title, const WCHAR *artist, const WCHAR *album)
{
    Updater *u = NULL;
    Music   *m = NULL;
    Music2  *m2 = NULL;
    if (!g_smtc) return;
    if (FAILED(__x_ABI_CWindows_CMedia_CISystemMediaTransportControls_get_DisplayUpdater(g_smtc, &u))) return;
    __x_ABI_CWindows_CMedia_CISystemMediaTransportControlsDisplayUpdater_put_Type(u, MediaPlaybackType_Music);
    if (SUCCEEDED(__x_ABI_CWindows_CMedia_CISystemMediaTransportControlsDisplayUpdater_get_MusicProperties(u, &m))) {
        HSTRING t = hs(title ? title : L""), a = hs(artist ? artist : L"");
        __x_ABI_CWindows_CMedia_CIMusicDisplayProperties_put_Title(m, t);
        __x_ABI_CWindows_CMedia_CIMusicDisplayProperties_put_Artist(m, a);
        WindowsDeleteString(t);
        WindowsDeleteString(a);
        if (SUCCEEDED(__x_ABI_CWindows_CMedia_CIMusicDisplayProperties_QueryInterface(m, &IID_MusicProps2, (void **)&m2))) {
            HSTRING al = hs(album ? album : L"");
            __x_ABI_CWindows_CMedia_CIMusicDisplayProperties2_put_AlbumTitle(m2, al);
            WindowsDeleteString(al);
            __x_ABI_CWindows_CMedia_CIMusicDisplayProperties2_Release(m2);
        }
        __x_ABI_CWindows_CMedia_CIMusicDisplayProperties_Release(m);
    }
    __x_ABI_CWindows_CMedia_CISystemMediaTransportControlsDisplayUpdater_Update(u);
    __x_ABI_CWindows_CMedia_CISystemMediaTransportControlsDisplayUpdater_Release(u);
}

/* PS_* を SMTC の状態へ。loading は「曲を読み込み中」(止まっているが、これから鳴る) */
void smtc_status(int state, BOOL loading)
{
    enum __x_ABI_CWindows_CMedia_CMediaPlaybackStatus s;
    if (!g_smtc) return;
    s = state == PS_PLAYING ? MediaPlaybackStatus_Playing : state == PS_PAUSED ? MediaPlaybackStatus_Paused
      : loading ? MediaPlaybackStatus_Changing : MediaPlaybackStatus_Stopped;
    __x_ABI_CWindows_CMedia_CISystemMediaTransportControls_put_PlaybackStatus(g_smtc, s);
}

void smtc_timeline(int posMs, int durMs)
{
    __x_ABI_CWindows_CFoundation_CTimeSpan z, d, p;
    if (!g_smtc2 || !g_tl) return;
    z.Duration = 0;
    d.Duration = (INT64)(durMs > 0 ? durMs : 0) * 10000;
    p.Duration = (INT64)(posMs > 0 ? posMs : 0) * 10000;
    __x_ABI_CWindows_CMedia_CISystemMediaTransportControlsTimelineProperties_put_StartTime(g_tl, z);
    __x_ABI_CWindows_CMedia_CISystemMediaTransportControlsTimelineProperties_put_EndTime(g_tl, d);
    __x_ABI_CWindows_CMedia_CISystemMediaTransportControlsTimelineProperties_put_MinSeekTime(g_tl, z);
    __x_ABI_CWindows_CMedia_CISystemMediaTransportControlsTimelineProperties_put_MaxSeekTime(g_tl, d);
    __x_ABI_CWindows_CMedia_CISystemMediaTransportControlsTimelineProperties_put_Position(g_tl, p);
    __x_ABI_CWindows_CMedia_CISystemMediaTransportControls2_UpdateTimelineProperties(g_smtc2, g_tl);
}
