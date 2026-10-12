/* ==================================================================
 * main.c - 入口
 *
 *  ytm-lite.exe                     画面を出す
 *  ytm-lite.exe -ini <path>         別の ini で動かす(二重起動の判定も ini ごと)
 *  ytm-lite.exe -mute               音量 0 で鳴らす(検証用。処理は同じ)
 *  ytm-lite.exe -noactivate         窓を前面に出さずに開く(検証用。利用者の操作を邪魔しない)
 *
 *  画面を出さずに確かめる入口(結果は標準出力に UTF-8 で。bash なら | cat で読む):
 *  ytm-lite.exe -selftest                   通信しない検査
 *  ytm-lite.exe -probe <videoId> [-decode] [-save <m4a>]
 *                                           匿名の player と音声の取得(-decode で最後まで復号)
 *  ytm-lite.exe -playtest <m4a> [-seconds N]  手元のファイルを音量 0 で鳴らしメモリを報告(通信しない)
 *  ytm-lite.exe -playlist <ID か URL> [-account]   プレイリストを読む(既定は匿名)
 *  ytm-lite.exe -library                    ライブラリのプレイリスト一覧(要ログイン情報)
 *  ytm-lite.exe -setauth <file>             ファイルの中身(要求ヘッダーなど)をログイン情報にする
 *
 *  通信する入口は、いずれも yt.c の作法(間隔・回数・拒まれたら止める)の内側で動く。
 * ================================================================== */

#include "ytm.h"
#include <shellapi.h>
#include <objbase.h>
#include <psapi.h>

#pragma comment(lib, "shell32.lib")

HINSTANCE g_inst;

static const WCHAR *arg_after(int argc, WCHAR **argv, const WCHAR *name)
{
    int i;
    for (i = 1; i < argc - 1; i++)
        if (!_wcsicmp(argv[i], name)) return argv[i + 1];
    return NULL;
}

static BOOL has_arg(int argc, WCHAR **argv, const WCHAR *name)
{
    int i;
    for (i = 1; i < argc; i++)
        if (!_wcsicmp(argv[i], name)) return TRUE;
    return FALSE;
}

static char *read_file_u8(const WCHAR *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD  size, got;
    char  *p;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    p = (char *)malloc((size_t)size + 1);
    if (p && ReadFile(f, p, size, &got, NULL)) p[got] = 0;
    else { free(p); p = NULL; }
    CloseHandle(f);
    return p;
}

/* ------------------------------------------------------------------ */
/*  -selftest                                                          */
/* ------------------------------------------------------------------ */

static int g_fail;
#define CHECK(cond, what) do { if (cond) out_printf("ok   %s", what); else { out_printf("FAIL %s", what); g_fail++; } } while (0)

static void selftest(void)
{
    static const char js[] =
        "{\"a\":{\"b\":[1,2,{\"c\":\"x\\u3042\\ud83c\\udfb5\\n\"}]},\"runs\":[{\"text\":\"foo\"},{\"text\":\"bar\"}],"
        "\"t\":true,\"n\":-1.5e2,\"z\":null,\"deep\":{\"k\":{\"musicX\":{\"v\":\"found\"}}}}";
    JDoc *d = JsonParse(js, sizeof(js) - 1);
    J    *r = JsonRoot(d);
    char  h[41], *ck = NULL, au[8], ua[64], *id;
    WCHAR *w;

    CHECK(d != NULL, "json: parse");
    CHECK(JNum(r, "a.b.1", 0) == 2, "json: path into array");
    CHECK(JStr(r, "a.b.2.c") && !strcmp(JStr(r, "a.b.2.c"), "x\xE3\x81\x82\xF0\x9F\x8E\xB5\n"), "json: \\u escapes and surrogate pair");
    CHECK(JNum(r, "n", 0) == -150, "json: number");
    CHECK(JNum(r, "t", 0) == 1, "json: bool");
    CHECK(JStr(JFind(r, "musicX"), "v") && !strcmp(JStr(JFind(r, "musicX"), "v"), "found"), "json: find by key");
    w = JText(r);
    CHECK(w && !wcscmp(w, L"foobar"), "json: runs text");
    free(w);
    JsonFree(d);
    CHECK(JsonParse("{\"a\":", 5) == NULL, "json: truncated input fails");

    CHECK(yt_selftest_jar() == 0, "guest cookie jar: add, update, delete");

    Sha1Hex("abc", h);
    CHECK(!strcmp(h, "a9993e364706816aba3e25717850c26c9cd0d89d"), "sha1: abc");

    CHECK(yt_parse_auth_text("accept: */*\ncookie: A=1; SAPISID=xyz; LOGIN_INFO=q\nx-goog-authuser: 1\nuser-agent: UA/1\n",
                             &ck, au, sizeof(au), ua, sizeof(ua)) && ck && !strcmp(ck, "A=1; SAPISID=xyz; LOGIN_INFO=q") &&
          !strcmp(au, "1") && !strcmp(ua, "UA/1"), "auth: request headers");
    free(ck); ck = NULL;
    CHECK(yt_parse_auth_text("cookie:\nB=2; __Secure-3PAPISID=abc\nx-goog-authuser:\n0\n", &ck, au, sizeof(au), ua, sizeof(ua)) &&
          ck && !strcmp(ck, "B=2; __Secure-3PAPISID=abc") && !strcmp(au, "0"), "auth: chrome name/value on separate lines");
    free(ck); ck = NULL;
    CHECK(yt_parse_auth_text("curl 'https://music.youtube.com/youtubei/v1/browse' \\\n  -H 'x-goog-authuser: 2' \\\n  -b 'C=3; SAPISID=s' \\\n",
                             &ck, au, sizeof(au), ua, sizeof(ua)) && ck && !strcmp(ck, "C=3; SAPISID=s") && !strcmp(au, "2"), "auth: curl -b");
    free(ck); ck = NULL;
    CHECK(yt_parse_auth_text("  D=4; SAPISID=t; E=5\r\n", &ck, au, sizeof(au), ua, sizeof(ua)) && ck && !strcmp(ck, "D=4; SAPISID=t; E=5"),
          "auth: bare cookie value");
    free(ck); ck = NULL;
    CHECK(!yt_parse_auth_text("cookie: A=1; B=2\n", &ck, au, sizeof(au), ua, sizeof(ua)), "auth: cookie without SAPISID is refused");
    free(ck);

    id = yt_playlist_id_from_text("https://music.youtube.com/playlist?list=PLabc_-123&si=x");
    CHECK(id && !strcmp(id, "PLabc_-123"), "playlist id: from URL");
    free(id);
    id = yt_playlist_id_from_text("VLPLxyz");
    CHECK(id && !strcmp(id, "PLxyz"), "playlist id: VL prefix");
    free(id);

    out_printf(g_fail ? "%d FAILED" : "all passed", g_fail);
}

/* ------------------------------------------------------------------ */

static void print_w(const char *label, const WCHAR *w)
{
    char *u = W2U(w ? w : L"");
    out_printf("%s%s", label, u ? u : "");
    free(u);
}

int cli_main(int argc, WCHAR **argv)
{
    const WCHAR *a;
    char         err[512];

    if (has_arg(argc, argv, L"-selftest")) {
        cfg_init_path(arg_after(argc, argv, L"-ini"));
        selftest();
        return 1;
    }
    if (!has_arg(argc, argv, L"-probe") && !has_arg(argc, argv, L"-playlist") && !has_arg(argc, argv, L"-playtest") &&
        !has_arg(argc, argv, L"-library") && !has_arg(argc, argv, L"-setauth"))
        return 0;

    cfg_init_path(arg_after(argc, argv, L"-ini"));
    cfg_load();
    if (!has_arg(argc, argv, L"-playtest")) NetInit();

    if ((a = arg_after(argc, argv, L"-setauth")) != NULL) {
        char *text = read_file_u8(a), *ck = NULL;
        if (text && yt_parse_auth_text(text, &ck, g_cfg.authuser, sizeof(g_cfg.authuser), g_cfg.ua, sizeof(g_cfg.ua))) {
            cfg_set_cookie(ck);
            out_printf("saved (authuser=%s, ua=%s)", g_cfg.authuser[0] ? g_cfg.authuser : "0", g_cfg.ua[0] ? "yes" : "default");
        } else {
            out_printf("no usable cookie (needs SAPISID or __Secure-3PAPISID)");
        }
        if (ck) { SecureZeroMemory(ck, strlen(ck)); free(ck); }
        free(text);
        return 1;
    }

    if ((a = arg_after(argc, argv, L"-probe")) != NULL) {
        char     *vid = W2U(a);
        AudioData ad;
        LONG64    t0 = now_ms();
        int       code = yt_fetch_audio(vid, &ad, NULL, err, sizeof(err));
        out_printf("code=%d (%s) in %lld ms", code,
                   code == YT_OK ? "OK" : code == YT_SKIP ? "SKIP" : code == YT_RETRY ? "RETRY" : code == YT_STOP ? "STOP" : "NET",
                   now_ms() - t0);
        if (err[0]) out_printf("err=%s", err);
        if (code == YT_OK) {
            out_printf("itag=%d bytes=%lu approxDurationMs=%d", ad.itag, ad.len, ad.durMs);
            if (has_arg(argc, argv, L"-decode")) {
                int dur = 0, rate = 0, ch = 0;
                CoInitializeEx(NULL, COINIT_MULTITHREADED);
                t0 = now_ms();
                if (player_decode_test(&ad, &dur, &rate, &ch, err, sizeof(err)))
                    out_printf("decoded: %d ms, %d Hz, %d ch (decode took %lld ms)", dur, rate, ch, now_ms() - t0);
                else
                    out_printf("decode failed: %s", err);
                CoUninitialize();
            }
            if (ad.h && (a = arg_after(argc, argv, L"-save")) != NULL) {
                HANDLE f = CreateFileW(a, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
                DWORD  wr = 0;
                if (f != INVALID_HANDLE_VALUE) {
                    WriteFile(f, GlobalLock(ad.h), ad.len, &wr, NULL);
                    GlobalUnlock(ad.h);
                    CloseHandle(f);
                }
                out_printf("saved %lu bytes", wr);
            }
            if (ad.h) GlobalFree(ad.h);
        }
        free(vid);
        return 1;
    }

    if ((a = arg_after(argc, argv, L"-playtest")) != NULL) {
        /* 手元の m4a を音量 0 で鳴らし、メモリを報告する(通信しない) */
        HANDLE f = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        const WCHAR *secArg = arg_after(argc, argv, L"-seconds");
        int    secs = secArg ? _wtoi(secArg) : 20, i;
        AudioData ad;
        DWORD  size, got = 0;
        PROCESS_MEMORY_COUNTERS_EX pm;
        if (f == INVALID_HANDLE_VALUE) { out_printf("cannot open file"); return 1; }
        memset(&ad, 0, sizeof(ad));
        size = GetFileSize(f, NULL);
        ad.h = GlobalAlloc(GMEM_MOVEABLE, size);
        ReadFile(f, GlobalLock(ad.h), size, &got, NULL);
        GlobalUnlock(ad.h);
        CloseHandle(f);
        ad.len = got;
        pm.cb = sizeof(pm);
        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof(pm));
        out_printf("before: WS=%lu KB private=%lu KB", (unsigned long)(pm.WorkingSetSize / 1024), (unsigned long)(pm.PrivateUsage / 1024));
        player_start(NULL, TRUE);
        player_volume(0);
        player_open(&ad, 1);
        for (i = 1; i <= secs; i++) {
            int st, pos, dur;
            Sleep(1000);
            if (i % 5 && i != secs) continue;
            player_status(&st, &pos, &dur, NULL);
            K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof(pm));
            out_printf("%3ds state=%d pos=%d/%d ms  WS=%lu KB private=%lu KB peakWS=%lu KB", i, st, pos, dur,
                       (unsigned long)(pm.WorkingSetSize / 1024), (unsigned long)(pm.PrivateUsage / 1024),
                       (unsigned long)(pm.PeakWorkingSetSize / 1024));
        }
        player_shutdown();
        return 1;
    }

    if ((a = arg_after(argc, argv, L"-playlist")) != NULL) {
        char     *text = W2U(a), *id = text ? yt_playlist_id_from_text(text) : NULL;
        TrackList t;
        WCHAR    *title = NULL;
        int       i;
        if (!id) { out_printf("not a playlist id"); free(text); return 1; }
        if (yt_playlist(id, &t, has_arg(argc, argv, L"-account"), &title, err, sizeof(err))) {
            print_w("title=", title);
            out_printf("tracks=%d", t.n);
            for (i = 0; i < t.n && i < 5; i++) {
                char *ti = W2U(t.v[i].title), *ar = W2U(t.v[i].artist);
                out_printf("  %s  %d:%02d  %s / %s%s", t.v[i].vid, t.v[i].dur / 60, t.v[i].dur % 60, ti ? ti : "", ar ? ar : "",
                           t.v[i].grey ? " (grey)" : "");
                free(ti);
                free(ar);
            }
        } else {
            out_printf("error: %s", err);
        }
        free(title);
        yt_free_tracks(&t);
        free(id);
        free(text);
        return 1;
    }

    if (has_arg(argc, argv, L"-library")) {
        PlaylistList p;
        int          i;
        if (yt_library(&p, err, sizeof(err))) {
            out_printf("playlists=%d", p.n);
            for (i = 0; i < p.n; i++) {
                char *ti = W2U(p.v[i].title), *su = W2U(p.v[i].sub);
                out_printf("  %s  %s  (%s)", p.v[i].id, ti ? ti : "", su ? su : "");
                free(ti);
                free(su);
            }
        } else {
            out_printf("error: %s", err);
        }
        yt_free_playlists(&p);
        return 1;
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    int     argc = 0, rc;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    (void)prev; (void)cmd; (void)show;
    g_inst = inst;
    SetDllDirectoryW(L"");                      /* カレントフォルダから DLL を読まない */
    if (argv && cli_main(argc, argv)) { LocalFree(argv); return g_fail ? 1 : 0; }
    cfg_init_path(argv ? arg_after(argc, argv, L"-ini") : NULL);
    cfg_load();
    rc = ui_run(inst, argv && has_arg(argc, argv, L"-mute"), argv && has_arg(argc, argv, L"-noactivate"));
    if (argv) LocalFree(argv);
    return rc;
}
