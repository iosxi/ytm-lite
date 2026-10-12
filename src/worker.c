/* ==================================================================
 * worker.c - 通信を受け持つスレッド(1 本だけ)
 *
 *  要求は必ずこのスレッドから 1 つずつ送る(並列に送らない。yt.c の作法 3)。
 *  依頼は待ち行列に入れ、結果は画面のスレッドへ PostMessage で返す。
 *
 *  再生の依頼(worker_audio)は「最新のものだけが意味を持つ」:
 *   - 新しい再生の依頼が来たら、待っている音声の依頼を捨て、取得中のものは取り消す。
 *   - ただし取得中のものが同じ曲の先読みなら、取り消さずに再生の依頼へ格上げする
 *     (同じ曲の player 要求を 2 回送らない)。
 *  再生の依頼は、待っているプレイリストの読み込みより先に処理する。
 * ================================================================== */

#include "ytm.h"

enum { JOB_LIBRARY = 1, JOB_PLAYLIST, JOB_AUDIO };

typedef struct Job {
    struct Job *next;
    int   type;
    char *id;
    BOOL  account;
    char  vid[16];
    int   gen;
    BOOL  prefetch;
} Job;

static CRITICAL_SECTION g_cs;
static HANDLE           g_ev, g_thread;
static Job             *g_head;
static Job             *g_cur;          /* 処理中の依頼(g_cs で守る) */
static volatile LONG    g_quit, g_cancelAudio;
static HWND             g_notify;

static void free_job(Job *j)
{
    if (!j) return;
    free(j->id);
    free(j);
}

static void post(UINT msg, void *p)
{
    if (!g_notify || !PostMessageW(g_notify, msg, 0, (LPARAM)p)) {
        /* 窓が無い: 捨てる */
        if (msg == WM_APP_AUDIO) free_audio_result((AudioResult *)p);
        else if (msg == WM_APP_LIBRARY) { yt_free_playlists(&((LibResult *)p)->pls); free(p); }
        else if (msg == WM_APP_PLAYLIST) { PlResult *r = (PlResult *)p; yt_free_tracks(&r->tracks); free(r->id); free(r->title); free(r); }
    }
}

void free_audio_result(AudioResult *r)
{
    if (!r) return;
    if (r->a.h) GlobalFree(r->a.h);
    free(r);
}

/* 取り消しか終了の合図まで待つ。合図が来たら FALSE */
static BOOL wait_ms(int ms)
{
    while (ms > 0) {
        if (g_quit || g_cancelAudio) return FALSE;
        Sleep(100);
        ms -= 100;
    }
    return TRUE;
}

static void do_audio(Job *j)
{
    AudioResult *r = (AudioResult *)calloc(1, sizeof(AudioResult));
    if (!r) return;
    strcpy_s(r->vid, sizeof(r->vid), j->vid);
    r->code = yt_fetch_audio(j->vid, &r->a, &g_cancelAudio, r->err, sizeof(r->err));
    if (r->code == YT_RETRY) {
        log_printf("retry in 5 s: %s", r->err);
        if (wait_ms(5000)) r->code = yt_fetch_audio(j->vid, &r->a, &g_cancelAudio, r->err, sizeof(r->err));
        else r->code = YT_CANCEL;
        if (r->code == YT_RETRY) r->code = YT_SKIP;        /* 2 回目も駄目ならこの曲は諦める */
    }
    EnterCriticalSection(&g_cs);
    r->gen = j->gen;                 /* 取得中に格上げされたかもしれない */
    r->prefetch = j->prefetch;
    LeaveCriticalSection(&g_cs);
    if (r->code == YT_CANCEL) { free_audio_result(r); return; }
    post(WM_APP_AUDIO, r);
}

static DWORD WINAPI worker_thread(void *arg)
{
    (void)arg;
    for (;;) {
        Job *j;
        WaitForSingleObject(g_ev, INFINITE);
        for (;;) {
            if (g_quit) return 0;
            EnterCriticalSection(&g_cs);
            j = g_head;
            if (j) g_head = j->next;
            g_cur = j;
            InterlockedExchange(&g_cancelAudio, 0);
            LeaveCriticalSection(&g_cs);
            if (!j) break;

            if (j->type == JOB_AUDIO) {
                do_audio(j);
            } else if (j->type == JOB_LIBRARY) {
                LibResult *r = (LibResult *)calloc(1, sizeof(LibResult));
                if (r) {
                    r->ok = yt_library(&r->pls, r->err, sizeof(r->err));
                    post(WM_APP_LIBRARY, r);
                }
            } else if (j->type == JOB_PLAYLIST) {
                PlResult *r = (PlResult *)calloc(1, sizeof(PlResult));
                if (r) {
                    r->id = U8Dup(j->id);
                    r->account = j->account;
                    r->ok = yt_playlist(j->id, &r->tracks, j->account, &r->title, r->err, sizeof(r->err));
                    post(WM_APP_PLAYLIST, r);
                }
            }
            EnterCriticalSection(&g_cs);
            g_cur = NULL;
            LeaveCriticalSection(&g_cs);
            free_job(j);
        }
    }
}

/* yt.c が上限で待つとき(このスレッドの中から呼ばれる)。先読みの待ちは画面に出さない */
static void on_wait(int sec)
{
    BOOL show;
    EnterCriticalSection(&g_cs);
    show = g_cur && g_cur->type == JOB_AUDIO && !g_cur->prefetch;
    LeaveCriticalSection(&g_cs);
    if (show && g_notify) PostMessageW(g_notify, WM_APP_WAIT, (WPARAM)sec, 0);
}

void worker_start(HWND notify)
{
    g_notify = notify;
    yt_set_wait_hook(on_wait);
    InitializeCriticalSection(&g_cs);
    g_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
}

void worker_stop(void)
{
    Job *j, *n;
    if (!g_thread) return;
    g_notify = NULL;
    InterlockedExchange(&g_quit, 1);
    InterlockedExchange(&g_cancelAudio, 1);
    SetEvent(g_ev);
    /* 通信の最中なら WinHTTP の時間切れまでかかりうる。終わりを長くは待たない */
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = NULL;
    EnterCriticalSection(&g_cs);
    for (j = g_head; j; j = n) { n = j->next; free_job(j); }
    g_head = NULL;
    LeaveCriticalSection(&g_cs);
}

static void push(Job *j, BOOL front)
{
    EnterCriticalSection(&g_cs);
    if (front) {
        /* 音声の依頼の後ろ、その他の前に入れる */
        Job **pp = &g_head;
        while (*pp && (*pp)->type == JOB_AUDIO) pp = &(*pp)->next;
        j->next = *pp;
        *pp = j;
    } else {
        Job **pp = &g_head;
        while (*pp) pp = &(*pp)->next;
        j->next = NULL;
        *pp = j;
    }
    LeaveCriticalSection(&g_cs);
    SetEvent(g_ev);
}

void worker_library(void)
{
    Job *j = (Job *)calloc(1, sizeof(Job));
    if (!j) return;
    j->type = JOB_LIBRARY;
    push(j, FALSE);
}

void worker_playlist(const char *id, BOOL useAccount)
{
    Job *j = (Job *)calloc(1, sizeof(Job));
    if (!j) return;
    j->type = JOB_PLAYLIST;
    j->id = U8Dup(id);
    j->account = useAccount;
    push(j, FALSE);
}

/* 待ち行列から音声の依頼を外す(prefetchOnly なら先読みだけ) */
static void drop_pending_audio(BOOL prefetchOnly)
{
    Job **pp = &g_head;
    while (*pp) {
        Job *j = *pp;
        if (j->type == JOB_AUDIO && (!prefetchOnly || j->prefetch)) { *pp = j->next; free_job(j); }
        else pp = &j->next;
    }
}

void worker_audio(const char *vid, int gen, BOOL prefetch)
{
    Job *j;
    EnterCriticalSection(&g_cs);
    if (!prefetch) {
        drop_pending_audio(FALSE);
        if (g_cur && g_cur->type == JOB_AUDIO) {
            if (!strcmp(g_cur->vid, vid)) {
                g_cur->gen = gen;             /* 取得中の先読みを格上げ */
                g_cur->prefetch = FALSE;
                LeaveCriticalSection(&g_cs);
                return;
            }
            InterlockedExchange(&g_cancelAudio, 1);
        }
    } else {
        drop_pending_audio(TRUE);
        if (g_cur && g_cur->type == JOB_AUDIO && !strcmp(g_cur->vid, vid)) { LeaveCriticalSection(&g_cs); return; }
    }
    LeaveCriticalSection(&g_cs);
    j = (Job *)calloc(1, sizeof(Job));
    if (!j) return;
    j->type = JOB_AUDIO;
    strcpy_s(j->vid, sizeof(j->vid), vid);
    j->gen = gen;
    j->prefetch = prefetch;
    push(j, TRUE);
}

void worker_cancel_audio(void)
{
    EnterCriticalSection(&g_cs);
    drop_pending_audio(FALSE);
    if (g_cur && g_cur->type == JOB_AUDIO) InterlockedExchange(&g_cancelAudio, 1);
    LeaveCriticalSection(&g_cs);
}
