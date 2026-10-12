/* ==================================================================
 * player.c - 鳴らす: m4a(AAC)を復号して WASAPI へ
 *
 *  - 復号は Media Foundation の Source Reader(Windows に最初からある MPEG-4 の
 *    読み取りと AAC デコーダ)。出力は 16bit PCM、元の速さ・チャンネル数のまま。
 *  - 出力は WASAPI の共有モード。形の変換(44.1kHz → デバイスの 48kHz など)は
 *    AUTOCONVERTPCM で Windows に任せる。
 *  - CPU を食わないよう、イベント駆動にせず、1 秒のバッファを 200ms ごとに
 *    足していく(起きるのは 1 秒に 5 回)。止めている間は眠ったまま。
 *  - 曲の終わりは、復号し終えた時点で PEV_ENDED を知らせる(デバイスにはまだ
 *    最大 1 秒残っている)。その間に次の曲が来れば、止めずに続けて流す(途切れない)。
 *  - 音量は ISimpleAudioVolume(Windows の音量ミキサーのアプリ音量)。
 *    AUDCLNT_STREAMFLAGS_NOPERSIST で、Windows にアプリ音量を覚えさせない
 *    (覚えさせるとレジストリに残る)。
 *  - 既定の再生デバイスが変わったら(2 秒ごとに見る)、いまの位置から付け替える。
 * ================================================================== */

#include "yum.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

/* C では、これらの ID の実体がどの .lib にも無い(iiv の wasapi.h と同じ) */
DEFINE_GUID(CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDeviceEnumerator, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(IID_IAudioClient, 0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(IID_IAudioRenderClient, 0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);
DEFINE_GUID(IID_ISimpleAudioVolume, 0x87CE5498, 0x68D6, 0x44E5, 0x92, 0x15, 0x6D, 0xA4, 0x7E, 0xF8, 0x83, 0xD8);

#define BUF_HNS      10000000LL     /* デバイスのバッファ 1 秒 */
#define TICK_MS      200
#define DEVCHECK_MS  2000

/* ---- 画面のスレッドから来る指示(g_cs で守る) ---- */
static CRITICAL_SECTION g_cs;
static HANDLE    g_thread, g_cmdEv;
static HWND      g_notify;
static BOOL      g_mute;
static BOOL      g_quit;
static AudioData g_new;                 /* 次に開くもの */
static int       g_newGen;
static BOOL      g_hasNew;
static int       g_seekReq = -1;
static int       g_pauseReq = -1;       /* 1 = 止める、0 = 続ける */
static BOOL      g_stopReq;
static int       g_volReq = -1;

/* ---- 状態(スレッドが書き、player_status が読む) ---- */
static volatile LONG g_state, g_posMs, g_durMs, g_gen;

/* ---- スレッドの中だけで使うもの ---- */
typedef struct {
    IMFSourceReader     *rd;
    IMMDeviceEnumerator *en;
    IMMDevice           *dev;
    WCHAR               *devId;
    IAudioClient        *ac;
    IAudioRenderClient  *rc;
    ISimpleAudioVolume  *sv;
    UINT32               bufFrames;
    int                  acRate, acCh;      /* デバイスを開いた形 */
    int                  rate, ch, block;   /* いまの曲の形 */
    BOOL                 running;           /* IAudioClient を Start している */
    BOOL                 eos, endedSent;
    LONG64               written;           /* この曲のぶんとしてデバイスへ書いたフレーム数 */
    int                  baseMs;            /* written = 0 の位置 */
    BYTE                *pcm;
    DWORD                pcmLen, pcmOff, pcmCap;
    int                  volume;
    LONG64               lastDevCheck;
    BOOL                 mfUp;              /* MFStartup 済み(最初の曲を開くときまで遅らせる) */
} P;

static void notify(int ev, int gen)
{
    if (g_notify) PostMessageW(g_notify, WM_APP_PLAYER, (WPARAM)ev, (LPARAM)gen);
}

/* ------------------------------------------------------------------ */
/*  音源(m4a → PCM)                                                    */
/* ------------------------------------------------------------------ */

static void close_source(P *p)
{
    if (p->rd) { IMFSourceReader_Release(p->rd); p->rd = NULL; }
    p->pcmLen = p->pcmOff = 0;
    p->eos = FALSE;
}

/* a の中身(HGLOBAL)を引き取って Source Reader を作る */
static HRESULT open_reader(AudioData *a, IMFSourceReader **out, int *rate, int *ch, int *durMs)
{
    IStream         *stm = NULL;
    IMFByteStream   *bs = NULL;
    IMFAttributes   *ba = NULL;
    IMFSourceReader *rd = NULL;
    IMFMediaType    *t = NULL, *cur = NULL;
    PROPVARIANT      pv;
    ULARGE_INTEGER   size;
    UINT32           v;
    HRESULT          hr;

    *out = NULL;
    hr = CreateStreamOnHGlobal(a->h, TRUE, &stm);          /* 以後 HGLOBAL は stm が持つ */
    if (FAILED(hr)) { GlobalFree(a->h); a->h = NULL; return hr; }
    a->h = NULL;
    size.QuadPart = a->len;
    IStream_SetSize(stm, size);                            /* GlobalAlloc は切り上げるので、本当の長さに */
    hr = MFCreateMFByteStreamOnStream(stm, &bs);
    IStream_Release(stm);
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(IMFByteStream_QueryInterface(bs, &IID_IMFAttributes, (void **)&ba))) {
        IMFAttributes_SetString(ba, &MF_BYTESTREAM_CONTENT_TYPE, L"audio/mp4");
        IMFAttributes_Release(ba);
    }
    hr = MFCreateSourceReaderFromByteStream(bs, NULL, &rd);
    IMFByteStream_Release(bs);
    if (FAILED(hr)) return hr;

    IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    hr = MFCreateMediaType(&t);
    if (SUCCEEDED(hr)) {
        IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        hr = IMFSourceReader_SetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, t);
        IMFMediaType_Release(t);
    }
    if (SUCCEEDED(hr)) hr = IMFSourceReader_GetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur);
    if (SUCCEEDED(hr)) {
        *rate = SUCCEEDED(IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &v)) ? (int)v : 0;
        *ch   = SUCCEEDED(IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_NUM_CHANNELS, &v)) ? (int)v : 0;
        IMFMediaType_Release(cur);
        if (*rate < 8000 || *ch < 1 || *ch > 8) hr = MF_E_INVALIDMEDIATYPE;
    }
    if (FAILED(hr)) { IMFSourceReader_Release(rd); return hr; }

    *durMs = 0;
    PropVariantInit(&pv);
    if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute(rd, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &pv)) &&
        pv.vt == VT_UI8)
        *durMs = (int)(pv.uhVal.QuadPart / 10000);
    PropVariantClear(&pv);
    *out = rd;
    return S_OK;
}

/* 次の PCM を p->pcm に読む。読めなければ(終わり・失敗)FALSE */
static BOOL read_more(P *p)
{
    DWORD           flags = 0, len = 0;
    LONGLONG        ts;
    IMFSample      *s = NULL;
    IMFMediaBuffer *b = NULL;
    BYTE           *data;
    HRESULT         hr;

    p->pcmLen = p->pcmOff = 0;
    if (!p->rd || p->eos) return FALSE;
    for (;;) {
        hr = IMFSourceReader_ReadSample(p->rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, &ts, &s);
        if (FAILED(hr)) { log_printf("ReadSample failed 0x%08lx", hr); p->eos = TRUE; return FALSE; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { if (s) IMFSample_Release(s); p->eos = TRUE; return FALSE; }
        if (s) break;
    }
    if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(s, &b))) {
        if (SUCCEEDED(IMFMediaBuffer_Lock(b, &data, NULL, &len))) {
            if (len > p->pcmCap) {
                BYTE *n = (BYTE *)realloc(p->pcm, len);
                if (n) { p->pcm = n; p->pcmCap = len; }
                else len = 0;
            }
            if (len) memcpy(p->pcm, data, len);
            p->pcmLen = len - len % (DWORD)p->block;
            IMFMediaBuffer_Unlock(b);
        }
        IMFMediaBuffer_Release(b);
    }
    IMFSample_Release(s);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  デバイス                                                            */
/* ------------------------------------------------------------------ */

static void close_device(P *p)
{
    if (p->ac && p->running) IAudioClient_Stop(p->ac);
    p->running = FALSE;
    if (p->sv) { ISimpleAudioVolume_Release(p->sv); p->sv = NULL; }
    if (p->rc) { IAudioRenderClient_Release(p->rc); p->rc = NULL; }
    if (p->ac) { IAudioClient_Release(p->ac); p->ac = NULL; }
    if (p->dev) { IMMDevice_Release(p->dev); p->dev = NULL; }
    if (p->devId) { CoTaskMemFree(p->devId); p->devId = NULL; }
    p->acRate = p->acCh = 0;
}

static void apply_volume(P *p)
{
    if (p->sv) ISimpleAudioVolume_SetMasterVolume(p->sv, g_mute ? 0.0f : p->volume / 100.0f, NULL);
}

static HRESULT open_device(P *p, int rate, int ch)
{
    WAVEFORMATEX wf;
    HRESULT      hr;

    close_device(p);
    if (!p->en) {
        hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&p->en);
        if (FAILED(hr)) return hr;
    }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(p->en, eRender, eConsole, &p->dev);
    if (FAILED(hr)) return hr;
    IMMDevice_GetId(p->dev, &p->devId);
    hr = IMMDevice_Activate(p->dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&p->ac);
    if (FAILED(hr)) { close_device(p); return hr; }
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = (WORD)ch;
    wf.nSamplesPerSec  = (DWORD)rate;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = (WORD)(ch * 2);
    wf.nAvgBytesPerSec = (DWORD)rate * wf.nBlockAlign;
    hr = IAudioClient_Initialize(p->ac, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                                 AUDCLNT_STREAMFLAGS_NOPERSIST,
                                 BUF_HNS, 0, &wf, NULL);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetBufferSize(p->ac, &p->bufFrames);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(p->ac, &IID_IAudioRenderClient, (void **)&p->rc);
    if (SUCCEEDED(hr)) IAudioClient_GetService(p->ac, &IID_ISimpleAudioVolume, (void **)&p->sv);
    if (FAILED(hr)) { log_printf("audio device init failed 0x%08lx", hr); close_device(p); return hr; }
    p->acRate = rate;
    p->acCh = ch;
    apply_volume(p);
    log_printf("audio device: %d Hz, %d ch, buffer %u frames", rate, ch, p->bufFrames);
    return S_OK;
}

static BOOL default_device_changed(P *p)
{
    IMMDevice *d = NULL;
    WCHAR     *id = NULL;
    BOOL       changed = FALSE;
    if (!p->en || !p->devId) return FALSE;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(p->en, eRender, eConsole, &d))) {
        if (SUCCEEDED(IMMDevice_GetId(d, &id))) {
            changed = wcscmp(id, p->devId) != 0;
            CoTaskMemFree(id);
        }
        IMMDevice_Release(d);
    }
    return changed;
}

static UINT32 padding(P *p)
{
    UINT32 pad = 0;
    if (p->ac) IAudioClient_GetCurrentPadding(p->ac, &pad);
    return pad;
}

/* ------------------------------------------------------------------ */
/*  再生の流れ                                                          */
/* ------------------------------------------------------------------ */

static void update_pos(P *p)
{
    LONG64 played = p->written - (LONG64)padding(p);
    int    ms;
    if (played < 0) played = 0;           /* 前の曲の残りがまだ鳴っている */
    ms = p->baseMs + (p->rate ? (int)(played * 1000 / p->rate) : 0);
    InterlockedExchange(&g_posMs, ms);
}

static void go_idle(P *p)
{
    close_source(p);
    close_device(p);                       /* 止めている間はデバイスも離す(メモリと電力) */
    InterlockedExchange(&g_state, PS_IDLE);
    InterlockedExchange(&g_posMs, 0);
}

/* デバイスへ書けるだけ書く */
static HRESULT fill(P *p)
{
    UINT32  pad = 0, avail;
    HRESULT hr;

    if (!p->ac) return S_OK;
    hr = IAudioClient_GetCurrentPadding(p->ac, &pad);
    if (FAILED(hr)) return hr;
    avail = p->bufFrames - pad;
    while (avail > 0 && p->rd) {
        UINT32 n;
        BYTE  *dst;
        if (p->pcmOff >= p->pcmLen) {
            if (!read_more(p)) break;
            continue;
        }
        n = (p->pcmLen - p->pcmOff) / (DWORD)p->block;
        if (n > avail) n = avail;
        hr = IAudioRenderClient_GetBuffer(p->rc, n, &dst);
        if (FAILED(hr)) return hr;
        memcpy(dst, p->pcm + p->pcmOff, (size_t)n * p->block);
        IAudioRenderClient_ReleaseBuffer(p->rc, n, 0);
        p->pcmOff += n * (DWORD)p->block;
        p->written += n;
        avail -= n;
    }
    return S_OK;
}

/* 新しい曲を開く。前の曲がまだデバイスに残っていて形が同じなら、止めずに続ける */
static void do_open(P *p, AudioData *a, int gen)
{
    IMFSourceReader *rd = NULL;
    int     rate = 0, ch = 0, durMs = 0;
    HRESULT hr;
    BOOL    cont;

    close_source(p);
    if (!p->mfUp) p->mfUp = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    hr = open_reader(a, &rd, &rate, &ch, &durMs);
    if (FAILED(hr)) {
        log_printf("open_reader failed 0x%08lx", hr);
        InterlockedExchange(&g_gen, gen);
        notify(PEV_ERROR, gen);
        if (p->running && padding(p) == 0) go_idle(p);
        return;
    }
    if (!durMs) durMs = a->durMs;
    cont = p->ac && p->running && p->acRate == rate && p->acCh == ch;
    if (!cont) {
        hr = (p->ac && p->acRate == rate && p->acCh == ch) ? S_OK : open_device(p, rate, ch);
        if (FAILED(hr)) {
            IMFSourceReader_Release(rd);
            InterlockedExchange(&g_gen, gen);
            notify(PEV_ERROR, gen);
            go_idle(p);
            return;
        }
        if (p->running) { IAudioClient_Stop(p->ac); p->running = FALSE; }
        IAudioClient_Reset(p->ac);
    }
    p->rd = rd;
    p->rate = rate;
    p->ch = ch;
    p->block = ch * 2;
    p->eos = FALSE;
    p->endedSent = FALSE;
    p->baseMs = 0;
    /* 続けて流すときは、前の曲の残り(padding)が鳴り終わるまで位置を 0 に留める */
    p->written = cont ? -(LONG64)padding(p) : 0;
    InterlockedExchange(&g_gen, gen);
    InterlockedExchange(&g_durMs, durMs);
    InterlockedExchange(&g_posMs, 0);
    fill(p);
    if (!p->running) {
        IAudioClient_Start(p->ac);
        p->running = TRUE;
    }
    InterlockedExchange(&g_state, PS_PLAYING);
    notify(PEV_STARTED, gen);
}

static void do_seek(P *p, int ms)
{
    PROPVARIANT pv;
    if (!p->rd) return;
    PropVariantInit(&pv);
    pv.vt = VT_I8;
    pv.hVal.QuadPart = (LONGLONG)ms * 10000;
    if (FAILED(IMFSourceReader_SetCurrentPosition(p->rd, &GUID_NULL, &pv))) return;
    if (p->ac) {
        if (p->running) IAudioClient_Stop(p->ac);
        IAudioClient_Reset(p->ac);
    }
    p->pcmLen = p->pcmOff = 0;
    p->eos = FALSE;
    p->endedSent = FALSE;
    p->written = 0;
    p->baseMs = ms;
    InterlockedExchange(&g_posMs, ms);
    fill(p);
    if (p->ac && g_state == PS_PLAYING) { IAudioClient_Start(p->ac); p->running = TRUE; }
    else p->running = FALSE;
}

/* デバイスが無くなった・既定が変わった: いまの位置から付け替える */
static void reroute(P *p)
{
    int ms = g_posMs;
    log_printf("audio device changed: reopen at %d ms", ms);
    close_device(p);
    if (!p->rd || FAILED(open_device(p, p->rate, p->ch))) { notify(PEV_ERROR, g_gen); go_idle(p); return; }
    p->running = FALSE;
    do_seek(p, ms);
}

static DWORD WINAPI player_thread(void *arg)
{
    P p;
    (void)arg;
    memset(&p, 0, sizeof(p));
    p.volume = g_cfg.volume;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    for (;;) {
        AudioData a;
        int       gen = 0, seek, pause, vol;
        BOOL      hasNew, stop, quit;
        DWORD     wait = (g_state == PS_PLAYING) ? TICK_MS : INFINITE;

        WaitForSingleObject(g_cmdEv, wait);

        EnterCriticalSection(&g_cs);
        quit = g_quit;
        hasNew = g_hasNew; a = g_new; gen = g_newGen; g_hasNew = FALSE; memset(&g_new, 0, sizeof(g_new));
        seek = g_seekReq; g_seekReq = -1;
        pause = g_pauseReq; g_pauseReq = -1;
        stop = g_stopReq; g_stopReq = FALSE;
        vol = g_volReq; g_volReq = -1;
        LeaveCriticalSection(&g_cs);

        if (quit) { if (a.h) GlobalFree(a.h); break; }
        if (vol >= 0) { p.volume = vol; apply_volume(&p); }
        if (stop) go_idle(&p);
        if (hasNew) do_open(&p, &a, gen);
        if (a.h) GlobalFree(a.h);
        if (seek >= 0 && g_state != PS_IDLE) do_seek(&p, seek);
        if (pause == 1 && g_state == PS_PLAYING) {
            if (p.ac && p.running) IAudioClient_Stop(p.ac);
            p.running = FALSE;
            InterlockedExchange(&g_state, PS_PAUSED);
        } else if (pause == 0 && g_state == PS_PAUSED) {
            if (p.ac) { IAudioClient_Start(p.ac); p.running = TRUE; }
            InterlockedExchange(&g_state, PS_PLAYING);
        }

        if (g_state == PS_PLAYING) {
            HRESULT hr = fill(&p);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { reroute(&p); continue; }
            if (now_ms() - p.lastDevCheck >= DEVCHECK_MS) {
                p.lastDevCheck = now_ms();
                if (p.rd && default_device_changed(&p)) { reroute(&p); continue; }
            }
            update_pos(&p);
            if (p.eos && p.pcmOff >= p.pcmLen) {
                if (!p.endedSent) {
                    p.endedSent = TRUE;
                    close_source(&p);
                    p.eos = TRUE;
                    notify(PEV_ENDED, g_gen);        /* 次の曲が来れば続けて流す */
                } else if (padding(&p) == 0) {
                    go_idle(&p);                     /* 次が来ないまま鳴り終わった */
                }
            }
        }
    }
    close_source(&p);
    close_device(&p);
    if (p.en) IMMDeviceEnumerator_Release(p.en);
    free(p.pcm);
    if (p.mfUp) MFShutdown();
    CoUninitialize();
    return 0;
}

/* ------------------------------------------------------------------ */
/*  画面のスレッドから                                                  */
/* ------------------------------------------------------------------ */

BOOL player_start(HWND notifyWnd, BOOL mute)
{
    g_notify = notifyWnd;
    g_mute = mute;
    InitializeCriticalSection(&g_cs);
    g_cmdEv = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, player_thread, NULL, 0, NULL);
    return g_thread != NULL;
}

void player_shutdown(void)
{
    if (!g_thread) return;
    g_notify = NULL;
    EnterCriticalSection(&g_cs);
    g_quit = TRUE;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_cmdEv);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = NULL;
}

void player_open(AudioData *a, int gen)
{
    EnterCriticalSection(&g_cs);
    if (g_new.h) GlobalFree(g_new.h);
    g_new = *a;
    g_newGen = gen;
    g_hasNew = TRUE;
    g_seekReq = -1;
    g_pauseReq = -1;
    g_stopReq = FALSE;
    LeaveCriticalSection(&g_cs);
    memset(a, 0, sizeof(*a));
    SetEvent(g_cmdEv);
}

void player_pause(BOOL pause)
{
    EnterCriticalSection(&g_cs);
    g_pauseReq = pause ? 1 : 0;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_cmdEv);
}

void player_seek(int ms)
{
    EnterCriticalSection(&g_cs);
    g_seekReq = ms < 0 ? 0 : ms;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_cmdEv);
}

void player_stop(void)
{
    EnterCriticalSection(&g_cs);
    if (g_new.h) { GlobalFree(g_new.h); memset(&g_new, 0, sizeof(g_new)); }
    g_hasNew = FALSE;
    g_stopReq = TRUE;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_cmdEv);
}

void player_volume(int v)
{
    EnterCriticalSection(&g_cs);
    g_volReq = v < 0 ? 0 : v > 100 ? 100 : v;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_cmdEv);
}

void player_status(int *state, int *posMs, int *durMs, int *gen)
{
    if (state) *state = g_state;
    if (posMs) *posMs = g_posMs;
    if (durMs) *durMs = g_durMs;
    if (gen)   *gen = g_gen;
}

/* -probe 用: 最後まで復号して長さを数える(鳴らさない)。呼ぶ側で COM を初期化しておく */
int player_decode_test(AudioData *a, int *durMs, int *rate, int *ch, char *err, size_t errlen)
{
    IMFSourceReader *rd = NULL;
    int     r = 0, c = 0, d = 0;
    LONG64  frames = 0;
    HRESULT hr;

    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    hr = open_reader(a, &rd, &r, &c, &d);
    if (FAILED(hr)) {
        _snprintf(err, errlen, "Source Reader を作れなかった(0x%08lx)", hr);
        MFShutdown();
        return 0;
    }
    for (;;) {
        DWORD           flags = 0, len = 0;
        LONGLONG        ts;
        IMFSample      *s = NULL;
        IMFMediaBuffer *b = NULL;
        hr = IMFSourceReader_ReadSample(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, &ts, &s);
        if (FAILED(hr)) { _snprintf(err, errlen, "復号に失敗(0x%08lx)", hr); break; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { if (s) IMFSample_Release(s); break; }
        if (!s) continue;
        if (SUCCEEDED(IMFSample_GetTotalLength(s, &len))) frames += len / (c * 2);
        (void)b;
        IMFSample_Release(s);
    }
    IMFSourceReader_Release(rd);
    MFShutdown();
    *rate = r;
    *ch = c;
    *durMs = r ? (int)(frames * 1000 / r) : 0;
    (void)d;
    return SUCCEEDED(hr);
}
