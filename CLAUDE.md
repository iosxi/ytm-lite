# ytm-lite の作業メモ

`C:\projects\windows\CLAUDE.md` の共通方針に加えて、このプロジェクト固有のこと。
フォルダ名・アプリ名・exe とも `ytm-lite`（本人の指定、2026-10-12。v1 は `yum-lite` と取り違えて作り、v2 で直した）。

## 目的と範囲

**YouTube Music の自分のプレイリストを、ブラウザより桁違いに少ないメモリで再生する**（本人の指示、2026-10-12）。
ログインして使う（自作のプレイリストを聴くため）。ブラウザ（Edge の WebView2 も含む）は使わない。

## 最優先: YouTube に不正とみなされない

本人は以前 yt-dlp で **IP アドレスごと不正とみなされた**ことがある。そうなると本人の利用も、ここでのテストもできなくなる。
通信の作法は `src/yt.c` の冒頭と `DEVELOPMENT.md` にまとめてある。崩さないこと:

- アカウントの cookie は **music.youtube.com の browse だけ**に付ける。player（再生）の要求には付けない。
- 再生は匿名の VISIONOS クライアント（PO トークンも JS も要らない）で、**1 台の端末としてのゲストのセッション**
  （ini の `visitor` と `guest`）を持ち続けて送る。毎回まっさらの匿名で送ると、数回でボット確認が返った（2026-10-12 実測）。
- player は間隔 2 秒以上、10 分間に 20 回まで。並列にしない（`worker.c` の 1 本のスレッドだけが送る）。
- 429・403・ボット確認が返ったら 30 分止める（`cooldown_until`）。**テストのためにこれを消して回避しない。**
  止まったら時間をおくか、本人に相談する。

## テストで YouTube に要求を送るとき

- **回数を数えて、最小にする。** 1 回の確認で済むことを何度も送らない。曲をまたぐ確認（連続再生、先読み）は
  1 プレイリストで 2〜3 曲までにする。
- 利用者の ini を使わない。`-ini <スクラッチの ini>` で動かす（cookie も visitor もクールダウンも ini ごと）。
- 再生部分（復号、WASAPI、メモリ）は通信せずに確かめられる: `-probe <id> -save x.m4a` で 1 曲保存しておき、
  `-playtest x.m4a` で何度でも鳴らす（音量 0）。
- ログは ini に `debug=1` を書くと、ini と同じフォルダの `ytm-lite.log` に出る（要求ごとに 1 行）。

## 確かめ方

- 通信しない検査: `ytm-lite.exe -selftest | cat`
- 通信の確認: `-probe <videoId> [-decode]`、`-playlist <ID か URL> [-account]`、`-library`、`-setauth <ファイル>`
- 画面: `ytm-lite.exe -ini <検証用 ini> -mute -noactivate`（音量 0、前面に出さない）。
  操作は `tools/post.ps1`（`PostMessage`。`WM_HOTKEY` 1=再生/一時停止 2=次 3=前 4=停止、
  `WM_COMMAND` でボタン ID）、撮影は `tools/snap.ps1`（その窓だけを `PrintWindow`）。
- ini のパスごとに二重起動を判定するので、利用者が動かしている本番と並べて動かせる。

## 版を上げるとき

`src/ytm.h` の `APP_VERSION`、`src/ytm-lite.rc` の VERSIONINFO、`src/ytm-lite.manifest` の
`assemblyIdentity` をまとめて上げる。
