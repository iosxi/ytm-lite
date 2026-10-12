/* ==================================================================
 * ytm.h - ytm-lite 全体で共有する宣言
 *
 *  ytm-lite は YouTube Music の自分のプレイリストを再生するだけの
 *  ネイティブアプリ。ブラウザを使わず、メモリを最小にすることが目的。
 *
 *  通信は 2 系統に分けている(DEVELOPMENT.md「安全のための作法」)。
 *   - アカウント: music.youtube.com の browse だけ(プレイリストの一覧と中身)。
 *                 利用者の cookie を使う。ブラウザの YouTube Music と同じ要求。
 *   - 再生      : www.youtube.com の player を、cookie なしの匿名で呼ぶ
 *                 (VISIONOS クライアント。PO トークンも JS も要らない)。
 * ================================================================== */
#ifndef YTM_H
#define YTM_H

#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define APP_NAME     L"ytm-lite"
#define APP_VERSION  "5"            /* .rc の VERSIONINFO と .manifest も合わせて上げる */

/* 画面のスレッドへ届ける通知 */
#define WM_APP_LIBRARY   (WM_APP + 1)   /* lParam = LibResult*  */
#define WM_APP_PLAYLIST  (WM_APP + 2)   /* lParam = PlResult*   */
#define WM_APP_AUDIO     (WM_APP + 3)   /* lParam = AudioResult* */
#define WM_APP_PLAYER    (WM_APP + 4)   /* wParam = PEV_*, lParam = gen */
#define WM_APP_WAIT      (WM_APP + 5)   /* wParam = 待つ秒数(0 = 待ち終わり) */
#define WM_APP_PROGRESS  (WM_APP + 6)   /* wParam = 読んだ曲数、lParam = プレイリスト ID(char*、受け取った側が free) */

/* ---------------------------------------------------------------- util.c */
typedef struct { char *p; size_t n, cap; } Buf;

void  BufAdd(Buf *b, const void *data, size_t n);
void  BufAddStr(Buf *b, const char *s);
void  BufAddChar(Buf *b, char c);
void  BufAddf(Buf *b, const char *fmt, ...);
void  BufAddJsonStr(Buf *b, const char *s);     /* "..." の形で、エスケープして足す */
void  BufFree(Buf *b);
char *BufTake(Buf *b);

WCHAR *U2W(const char *s);
char  *W2U(const WCHAR *w);
char  *U8Dup(const char *s);
char  *U8NDup(const char *s, size_t n);
WCHAR *WDup(const WCHAR *s);
const char *U8FindI(const char *hay, const char *needle);
void  Sha1Hex(const char *s, char out[41]);
void  log_printf(const char *fmt, ...);          /* debug=1 のときだけ ytm-lite.log へ */
void  out_printf(const char *fmt, ...);          /* -probe などの結果を標準出力へ(UTF-8) */
LONG64 now_ms(void);                             /* 起動からのミリ秒(GetTickCount64) */
LONG64 unix_time(void);

/* ---------------------------------------------------------------- json.c */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct J {
    JType       t;
    const char *key;        /* 親がオブジェクトのとき */
    const char *s;          /* J_STR */
    double      n;          /* J_NUM、J_BOOL(0/1) */
    struct J   *child, *next;
} J;
typedef struct JDoc JDoc;

JDoc       *JsonParse(const char *text, size_t len);   /* 失敗は NULL */
J          *JsonRoot(JDoc *d);
void        JsonFree(JDoc *d);
J          *JGet(const J *obj, const char *key);
J          *JAt(const J *arr, int i);
J          *JPath(const J *j, const char *path);       /* "a.b.0.c" */
const char *JStr(const J *j, const char *path);        /* 文字列でなければ NULL */
double      JNum(const J *j, const char *path, double def);
J          *JFind(const J *j, const char *key);         /* 深さ優先で最初に見つかったもの */
WCHAR      *JText(const J *textObj);                    /* runs[].text をつないだもの、または simpleText */

/* ---------------------------------------------------------------- net.c */
typedef struct {
    int  status;
    Buf  body;              /* 末尾に NUL を足してある */
    int  retryAfter;        /* 秒。無ければ 0 */
    char *setCookie;        /* Set-Cookie の値を改行区切りで(無ければ NULL) */
} HttpResp;

void NetInit(void);
int  HttpPost(const char *url, const char *headers, const char *body, HttpResp *r, char *err, size_t errlen);
int  HttpGet(const char *url, const char *headers, HttpResp *r, char *err, size_t errlen);
int  HttpGetRange(const char *url, const char *headers, BYTE *dst, DWORD want, DWORD *got, int *status,
                  volatile LONG *cancel, char *err, size_t errlen);
void HttpRespFree(HttpResp *r);

/* ---------------------------------------------------------------- config.c */
typedef struct {
    WCHAR ini[MAX_PATH];
    WCHAR dir[MAX_PATH];        /* ini のあるフォルダ(ログもここ) */
    char *cookie;               /* アカウントの cookie(NULL = 未設定)。貼り付けたものは ini に DPAPI で暗号化して置く */
    int   cookieSource;         /* COOKIE_* */
    WCHAR ffProfile[MAX_PATH];  /* Firefox から読むときのプロファイルのフォルダ */
    char  cookieErr[256];       /* 起動時に Firefox から読めなかった理由 */
    char  authuser[8];
    char  ua[512];              /* アカウントの要求に使う User-Agent(貼り付けたヘッダーにあればそれ) */
    char  visitor[1024];        /* 匿名側の visitorData(端末ごとに 1 つ持ち続ける。450 字ほどある) */
    char  guest[2048];          /* 匿名側の cookie(VISITOR_INFO1_LIVE、YSC など。アカウントとは別) */
    LONG64 cooldownUntil;       /* この時刻(UNIX 秒)まで再生側の要求を止める */
    int   volume;               /* 0〜100 */
    int   shuffle, repeat;
    int   x, y, w, h, maximized;
    int   theme;                /* 0 = システムに合わせる、1 = ライト、2 = ダーク */
    int   mediakeys;            /* キーボードのメディアキーを横取りする */
    int   debug;
    char  lastPlaylist[128];
} Config;

enum { COOKIE_NONE = 0, COOKIE_PASTED, COOKIE_FIREFOX };

extern Config g_cfg;

void cfg_init_path(const WCHAR *iniArg);
void cfg_load(void);
void cfg_save(void);
void cfg_set_cookie(const char *cookie);     /* 貼り付けたもの。NULL で消す。すぐ ini に書く */
BOOL cfg_use_firefox(const WCHAR *profile, char *err, size_t errlen);  /* 読めたら以後 Firefox から読む */
BOOL cfg_reload_cookie(char *err, size_t errlen);   /* Firefox から読み直す(Firefox のときだけ) */

/* ---------------------------------------------------------------- ffcookie.c */
char *ff_read_youtube_cookies(const WCHAR *profileDir, int *count, char *err, size_t errlen);
void  ff_user_agent(const WCHAR *profileDir, char *ua, size_t cap);
BOOL  ff_find_profile(WCHAR *out, size_t cap);
BOOL  ff_resolve_profile(const WCHAR *given, WCHAR *out, size_t cap);   /* フォルダや exe からプロファイルへ */
BOOL  ff_launcher_for_profile(const WCHAR *profile, WCHAR *exe, size_t cap);
int   ff_candidates(WCHAR (*list)[MAX_PATH], int max);

/* ---------------------------------------------------------------- slider.c */
#define SLIDER_CLASS  L"ytm-slider"
#define SLM_SETRANGE  (WM_USER + 1)     /* wParam = 最大 */
#define SLM_SETPOS    (WM_USER + 2)     /* wParam = 位置(動かしている最中は無視) */
#define SLM_GETPOS    (WM_USER + 3)
#define SLM_SETSTYLE  (WM_USER + 4)     /* wParam = SLS_* */
#define SLM_SETACTIVE (WM_USER + 5)     /* wParam = シークで曲があるか */
enum { SLS_SEEK = 0, SLS_VOLUME };
void slider_register(HINSTANCE inst);
void cfg_save_guest(void);                   /* visitor と guest を書く */
void cfg_save_cooldown(void);

/* ---------------------------------------------------------------- yt.c */
typedef struct {
    char  *id;          /* プレイリスト ID(先頭の VL は外したもの) */
    WCHAR *title;
    WCHAR *sub;         /* 「プレイリスト • 25 曲」など */
} Playlist;

typedef struct {
    char   vid[16];
    WCHAR *title, *artist, *album;
    int    dur;         /* 秒。不明は 0 */
    BOOL   grey;        /* YouTube Music 上で灰色(再生できない)の曲 */
} Track;

typedef struct { Track *v; int n, cap; } TrackList;
typedef struct { Playlist *v; int n, cap; } PlaylistList;

/* 再生側の結果 */
enum {
    YT_OK = 0,
    YT_SKIP,            /* この曲は再生できない(UNPLAYABLE など)。次の曲へ進んでよい */
    YT_RETRY,           /* 一時的(GVS の 403 など)。少し待って 1 回だけやり直してよい */
    YT_STOP,            /* 429 やボット確認。しばらく何も送らない */
    YT_NET,             /* 通信できない */
    YT_CANCEL
};

typedef struct {
    HGLOBAL h;          /* 音声ファイル(m4a)の中身 */
    DWORD   len;
    int     itag;
    int     durMs;      /* approxDurationMs */
} AudioData;

int  yt_library(PlaylistList *out, char *err, size_t errlen);
int  yt_playlist(const char *id, TrackList *out, BOOL useAccount, WCHAR **title, char *err, size_t errlen);
int  yt_fetch_audio(const char *vid, AudioData *out, volatile LONG *cancel, char *err, size_t errlen);
BOOL yt_parse_auth_text(const char *text, char **cookie, char *authuser, size_t aulen, char *ua, size_t ualen);
BOOL yt_cookie_ok(const char *cookie);
char *yt_playlist_id_from_text(const char *text);      /* URL や ID から ID を取り出す */
int  yt_selftest_jar(void);                            /* 失敗の数 */
void yt_set_wait_hook(void (*fn)(int sec));           /* 上限で待つとき秒数を、終わったら 0 を知らせる */
void yt_set_progress_hook(void (*fn)(const char *id, int count));   /* プレイリストを 1 ページ読むごと */
void yt_free_tracks(TrackList *t);
void yt_free_playlists(PlaylistList *p);

/* ---------------------------------------------------------------- worker.c */
typedef struct { int ok; PlaylistList pls; char err[512]; } LibResult;
typedef struct { int ok; char *id; WCHAR *title; BOOL account; TrackList tracks; char err[512]; } PlResult;
typedef struct {
    int       code;     /* YT_* */
    int       gen;      /* 頼んだときの番号 */
    BOOL      prefetch;
    char      vid[16];
    AudioData a;
    char      err[512];
} AudioResult;

void worker_start(HWND notify);
void worker_stop(void);
void worker_library(void);
void worker_playlist(const char *id, BOOL useAccount);
void worker_audio(const char *vid, int gen, BOOL prefetch);  /* 新しい再生の依頼は古い依頼を取り消す */
void worker_cancel_audio(void);
void free_audio_result(AudioResult *r);

/* ---------------------------------------------------------------- player.c */
enum { PEV_STARTED = 1, PEV_ENDED, PEV_ERROR };
enum { PS_IDLE = 0, PS_PLAYING, PS_PAUSED };

BOOL player_start(HWND notify, BOOL mute);
void player_shutdown(void);
void player_open(AudioData *a, int gen);     /* a の中身を引き取る */
void player_pause(BOOL pause);
void player_seek(int ms);
void player_stop(void);
void player_volume(int v);                   /* 0〜100 */
void player_status(int *state, int *posMs, int *durMs, int *gen);
int  player_decode_test(AudioData *a, int *durMs, int *rate, int *ch, char *err, size_t errlen);

/* ---------------------------------------------------------------- theme.c */
void     theme_init(void);
BOOL     theme_refresh(void);
BOOL     theme_is_dark(void);
COLORREF theme_back(void);
COLORREF theme_footer(void);
COLORREF theme_ctrl_back(void);
COLORREF theme_text(void);
COLORREF theme_dim_text(void);
COLORREF theme_line(void);
COLORREF theme_accent(void);
HBRUSH   theme_back_brush(void);
HBRUSH   theme_footer_brush(void);
HBRUSH   theme_ctrl_brush(void);
void     theme_allow_dark(HWND hwnd);
void     theme_apply_window(HWND hwnd);
void     theme_title_bar(HWND hwnd);                  /* アクティブになるたびに付け直す */
void     theme_draw_button(const DRAWITEMSTRUCT *di); /* ダイアログの押しボタン(自前で描く) */
LRESULT  theme_ctlcolor(UINT msg, HDC dc, HWND ctl, BOOL dimText);

/* ---------------------------------------------------------------- ui.c / main.c */
int  ui_run(HINSTANCE inst, BOOL mute, BOOL noActivate);
int  cli_main(int argc, WCHAR **argv);       /* -probe などを処理したら 1 */
extern HINSTANCE g_inst;

#endif
