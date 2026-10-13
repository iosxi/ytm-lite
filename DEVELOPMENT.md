# ytm-lite 開発メモ

実装の判断の根拠と、実測の記録。使い方は README.md。

## 方式を選んだ理由（2026-10-12）

### ブラウザを使わない

再生中のメモリの大半は、ブラウザの枠ではなく YouTube Music のページそのもの（JavaScript、DOM、
デコード）が使う。Chromium 系を削っても、ページを描く限り下限は大きい。そこで、ページを描かず、
YouTube の内部 API（InnerTube）を直接呼ぶネイティブアプリにした。Edge の WebView2 は本人が除外した。

### 2026-10 時点の YouTube の事情（調べたこと）

出典: yt-dlp のソース（2026-10-10 のコミット `8973412`）、yt-dlp Wiki の
[PO Token Guide](https://github.com/yt-dlp/yt-dlp/wiki/PO-Token-Guide)、
[Extractors](https://github.com/yt-dlp/yt-dlp/wiki/Extractors)、[EJS](https://github.com/yt-dlp/yt-dlp/wiki/EJS)、
[issue #17226（visionos クライアント）](https://github.com/yt-dlp/yt-dlp/issues/17226)、
ytmusicapi のソース（2026-10-12）。

- **PO トークン**: web、web_music、mweb などは、音声・動画の配信（GVS）に PO トークンが要る
  （BotGuard を動かして作る。Premium 会員は GVS に限り不要）。無いと 403 になるか、
  「アカウントか IP アドレスがブロックされうる」（PO Token Guide）。
- **JS チャレンジ**: tv、web 系は、YouTube の JS（base.js）を実行して n / sig を解かないと
  URL が使えない。yt-dlp は外部の JS 実行環境（Deno など）と yt-dlp-ejs を要求するようになった。
- **visionos クライアント**: PO トークンも JS も要らない。2026-08-19 の yt-dlp から、
  ログインしないときの既定。cookie（アカウント）は使えない。「子ども向け」の動画は再生できない。
  android_vr は 2026-08-17 から全形式 403 になった（yt-dlp のソースのコメント）。
- **アカウントの BAN**: yt-dlp Wiki は「アカウントを yt-dlp で使うと BAN されうる。
  要求の頻度と量に注意」と警告している。
- **回数の上限**（yt-dlp Wiki）: 匿名は 1 時間に約 300 本（webpage/player の要求で約 1000 回）、
  アカウントは約 2000 本（約 4000 回）。
- **ytmusicapi のブラウザ認証**: browse の POST の要求ヘッダー（cookie と x-goog-authuser）を使い、
  `SAPISIDHASH` を毎回作る。「ブラウザのセッションが有効な間（ログアウトしなければ約 2 年）使える」。
  OAuth は 2024-11 に YouTube が共有クライアントを止めたため、自前の Google Cloud のクライアントが要る。

### 選んだ形: 通信を 2 系統に分ける

| | アカウント側 | 再生側 |
| --- | --- | --- |
| 相手 | music.youtube.com `browse` | www.youtube.com `player`、GVS |
| 名乗り | WEB_REMIX（ブラウザの YouTube Music と同じ） | VISIONOS |
| cookie | 利用者のアカウント | ゲストのセッション（端末ごとに 1 つ持ち続ける） |
| 回数 | プレイリストを開いたとき 1 回 + 100 曲ごとに 1 回 | 曲ごとに 1 回（+ 次の曲の先読み 1 回） |

- 再生の要求にアカウントを使わないので、「アカウントで大量に取得した」とみなされる余地がない。
- PO トークンを偽造したり、YouTube の JS を解読したりしない。要らないクライアントだけを使う。
- 音声は AAC（itag 140、128kbps）。Windows に最初からある Media Foundation で読める。
  Opus（WebM）は Windows が素では読めないので使わない。

### 安全のための作法（src/yt.c）

- player は間隔 2 秒以上、5 分間に 15 回まで（1 時間に最大 180 回、匿名の上限の 1/5 以下）。並列にしない。
  v4 までは 10 分に 20 回だったが、待ちが最長 10 分と長く、本人の要望で緩めた（下の「上限を緩めた」）。
- 429、403、ボット確認が返ったら 30 分止める。自動で何度もやり直さない
  （GVS の 403 だけ、5 秒おいて 1 回）。続けて 3 曲再生できなければ止める。
- 音声は yt-dlp と同じく 10MB ずつの range で取る。

## 実測

### 2026-10-12: 匿名の要求でボット確認が返った

cookie も visitorData も持たない匿名の要求を、数分のうちに 7 回送った
（curl で player 1、`-probe` で player 1、`-playlist` で browse 2、画面で browse 2 と player 1）。
最初の 2 回の player は通ったが、3 回目の player で `LOGIN_REQUIRED`（理由に「ボット」）が返り、
30 分の停止が働いた。

yt-dlp は visionos の player の前に、必ずページを開いてゲストの cookie（`VISITOR_INFO1_LIVE`、`YSC` など）と
visitorData を得て、それを付けて送っている。毎回まっさらの匿名で送るのは、それより不自然。
そこで、初回だけトップページを 1 回取ってゲストのセッションを作り、以後はそれを持ち続けるようにした
（ini の `visitor` と `guest`）。

30 分おいて、新しい ini で確かめた（20:50）: トップページ 1 回で `VISITOR_INFO1_LIVE`、`YSC`、
`VISITOR_PRIVACY_METADATA`、`__Secure-ROLLOUT_TOKEN`、`__Secure-YNID`、`__Secure-BUCKET`、`GPS` と
visitorData（458 字）が得られ、続く player は `OK`。先にボット確認が返ったのと同じ曲（`fOT0BUpITw8`）も、
このセッションでは `OK` だった。以後、browse 6 回・player 4 回を送って拒まれていない。
ただし原因が「セッションが無かったこと」だと証明したわけではない（回線の評判の揺れの可能性は残る）。

### 2026-10-12: メモリと CPU（Windows 11、この PC）

同じ公開プレイリストの同じ曲（3:54、itag 140 で 3.6MB）を再生中の値。ytm-lite は `-mute`、Chrome は
`--headless=new --mute-audio`（使い捨てのプロファイル、UA は通常の Chrome 154）。
Chrome が再生していることは CDP で `video.currentTime` が進むのを見て確かめた。

| | プロセス | Private（合計） | Working Set（合計） |
| --- | --- | --- | --- |
| Chrome 154 ヘッドレス | 11 | 594 MB | 953 MB |
| ytm-lite | 1 | 12.8 MB | 36.8 MB |

- Chrome の内訳: レンダラー（YouTube Music のページ）203MB、GPU 149MB、ブラウザ 65MB、video 59MB など。
  この曲は Chrome では動画として再生されていた。ヘッドレスは画面まわりが無いので、通常の Chrome はこれより多い。
  Working Set の合計は共有ページを重ねて数えている。
- ytm-lite の状態ごと（Private / Working Set）:
  何も読まずに起動 3.1MB / 18.5MB、プレイリスト 182 曲を読んだ後 4.2MB / 22.5MB（読み込み中の山は WS 26.6MB）、
  再生中 12.8MB / 36.8MB（取得した音声 3.6MB を含む）、次の曲の先読みを持っている 40 秒間は
  その曲の分だけ増える（10:50 の曲で 23MB）。
- 音声は丸ごとメモリに持つので、長い曲ほど増える（約 1MB/分）。
- スレッドは待機中 11、再生中 17（Media Foundation と WASAPI の分）。

CPU（`Get-Process` の CPU 時間の差。1 秒あたりの CPU 秒、刻みが粗いので ±0.01 程度は揺れる）:

| | CPU |
| --- | --- |
| 画面を出して再生中 | 0.038 |
| `-playtest`（画面なし） | 0.030〜0.054 |
| 同、`AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY` を外した版 | 0.010〜0.021 |
| 復号だけ（`-probe -decode`。213 秒分を 0.67 秒） | 約 0.003 |

- 主な CPU は、WASAPI がこちらのプロセスの中で行う 44.1kHz → デバイスの形への高品質な変換。
  品質の指定を外すと 2〜3 分の 1 になるが、音質を下げるので外していない。
- 画面の描き直しは、0.5 秒ごとに窓全体 → 1 秒ごとに下の帯だけ、にした。描き直しの分は測定の揺れに埋もれた。

### 2026-10-12: 一部の曲が鳴らずに次へ飛ぶ（v2 の不具合、v3 で直した）

本人がログインして「ドライブ」を再生したら、先頭から 10 曲が鳴らずに次々と飛び、11 曲目で
「読み込み中…」のまま止まった。

- 飛んだ曲（例: `jtrtvEAGqcc`）は、取得は `OK` だが `-probe -decode` で **0ms**（1 サンプルも出ない）。
  MP4 の箱を比べると、飛んだ曲だけ `moov/trak/edts/elst` を持っていた
  （`media_time=1600`、先頭の前置き 1600 サンプル ≒ 36ms を飛ばす指定）。復号できた曲には無い。
- `edts` の型を `free` に書き換えたコピーは `-playtest` で普通に再生された（5 秒で 4.72 秒進む。元は位置 0 のまま停止）。
  Windows の MPEG-4 の読み取りは、断片化した MP4 の空の sample table にエディットリストを当てて 0 サンプルにしてしまうらしい（推測）。
- 0 サンプルで「終わった」ことを正常終了として扱ったので、次の曲へ進み、曲ごとに player を送り続けた。
  上限（10 分に 20 回）に当たると、何も知らせずに最大 10 分待つので、「読み込み中…」のまま止まって見えた。

直したこと（v3）:
- 復号の前に `edts` を `free` に書き換える（`player.c` の `neutralize_edts`）。
- 1 サンプルも出ずに終わった曲は「再生できなかった」として扱う（続けて 3 曲で止まる）。
- 上限で待つときは「約 N 秒待ってから読み込みます」と画面に出す。

### 2026-10-12: Firefox から cookie を読む（v4）

本人の要望: cookie の設定が面倒。yt-dlp の `--cookies-from-browser` のようにしたい。Firefox も使う。

- Chrome は 127 から cookie の鍵が Chrome 本体に縛られ（App-Bound Encryption）、外から読めない。
  yt-dlp も Windows の Chrome からは読めない（[yt-dlp #10927](https://github.com/yt-dlp/yt-dlp/issues/10927) など）。
  Firefox は暗号化していない。
- yt-dlp（`cookies.py`）は cookies.sqlite を一時フォルダへコピーして sqlite3 で読む。**WAL（`-wal`）はコピーしない**ので、
  書き戻し前の変更を取りこぼす。ここでは SQLite の読むだけの解析（`ffcookie.c`、約 400 行）で、本体の上に WAL の
  確定済みフレームを重ねて読む。一時フォルダには何も書かない。
- この PC の Firefox: `cookies.sqlite` は 1MB、ページ 32KB、`user_version` 17（期限はミリ秒）。YouTube の cookie は
  74 行あるが、`originAttributes` が空のもの（コンテナでも埋め込みでもないもの）は 22 個。
- 突き合わせ（2026-10-12）: Python の sqlite3 で同じ条件の文字列を作り SHA-1 を比べた。
  本物の Firefox で 22 個・一致。WAL にしか無い 300 行と WAL での書き換え、5000 字の値（はみ出しページ）、
  埋め込み用・期限切れの除外を入れた検証用 DB で 302 個・一致。
- 起動のたびに Firefox から読み直す。アカウントの要求が 401/403 か「ログインしていない」を返したら、読み直して 1 回だけやり直す。
- User-Agent は Firefox のプロファイルの `compatibility.ini` の版から作る（cookie と同じブラウザを名乗る）。

### 2026-10-12: シークバーの赤が残る・ギザギザ（v4）

trackbar は位置が変わってもつまみの周りしか描き直さない。溝の塗り（再生済みの赤）を自前で描いていたので、
戻したときや曲の終わりに赤が残り、つまみ（丸）が trackbar のつまみの矩形より大きかったので跡がギザギザに残った。
毎回全体を裏で描く小さな部品（`slider.c`）に替えた。100 秒 → 10 秒 → 停止と動かして、跡が残らないことを撮って確かめた。

### 2026-10-12: 上限を緩めた（v4）

本人の要望:「10 分に 20 回」は厳しい。最低でも「5 分に 10 回」に。

- 前提: ログインしていても、再生の要求は匿名（visionos、ゲストのセッション）。ログインの有無でボット確認の出方は変わらない。
- 根拠: v2 の不具合で曲が飛び続けたとき、ゲストのセッションで約 2.5 秒おきに 10 回続けて送ったまとまりが 2 回あったが、
  拒まれなかった（本人の環境、21:0x〜21:23）。yt-dlp Wiki の匿名の上限は 1 時間に約 1000 回。
- 5 分に 15 回（1 時間に最大 180 回）にした。待ちは最長 5 分。
- 「次へ」の連打で回数を使わないよう、利用者の操作から 0.4 秒待って、止まった曲だけを取りに行く。
  実測: 再生 1 回のあと「次へ」を 0.1 秒おきに 5 回送ると、player は 6 曲目の 1 回だけだった（以前なら 6 回）。

### 2026-10-13: 23H2 で黒地に黒い文字にならないか（v5）

文字の色を Windows のテーマ任せにしている箇所を洗い出した。黒地に黒い文字になりうるのは、ダイアログの押しボタン
（テーマが文字を描き、`WM_CTLCOLORBTN` の文字色は効かない。23H2 でチェックボックスの文字が黒くなったのと同じ仕組み。obtan v4）
だけだった。押しボタンは自前で描くようにした（`theme_draw_button`）。ほかは自前で色を指定しているか（一覧、見出し、説明文、入力欄）、
自前で描いている（本体のボタン、シークバー、帯）。メニューは背景もテーマが描くので、黒地に黒にはならない。
23H2 の実機では確かめていない。

あわせて:
- 入力欄の縁を自前で描く（ダークでもライトでもテーマの縁が見えにくかった）。複数行の入力欄は DarkMode_Explorer でスクロールバーも暗く。
- 一覧（ListView）は DarkMode_Explorer にした。DarkMode_ItemsView だとスクロールバーが白いままだった（本人の指摘）。見出しは DarkMode_ItemsView のまま。
- ダイアログを動かすとタイトルバーと太い枠が白くなったと本人から報告。こちらでは前面に出さない範囲で再現できなかった
  （`DwmGetWindowAttribute(20)` は 1 のまま）。太い立体の枠（`DS_MODALFRAME`）をやめ、`WCA_USEDARKMODECOLORS` も付け、
  `WM_ACTIVATE` のたびに付け直すようにした。直ったかは本人の確認待ち。

### 2026-10-13: Firefox Portable（v5）

遠隔の利用者から「Firefox Portable でログインしても『ログインしていないようです』になる」と報告。
Portable 版（PortableApps.com）のプロファイルは `FirefoxPortable\Data\profile`、本体は `FirefoxPortable\App\Firefox64\firefox.exe`
（[PortableApps.com](https://portableapps.com/node/49)）。v4 は `%APPDATA%` のプロファイルしか見ていなかった。

- 動いている `firefox.exe` の場所が `…\App\Firefox…` なら Portable 版として `Data\profile` を候補の先頭にする。
  いつものプロファイル、profiles.ini のほかのプロファイルも候補にし、ログインしている最初のものを使う。
- ダイアログにフォルダの欄と［参照…］を足した。Portable 版のフォルダ、`Data`、プロファイル、exe のどれでもよい。
- ［music.youtube.com を開く］: Portable 版なら `FirefoxPortable.exe`、無ければ入っている Firefox、それも無ければ既定のブラウザで開く。
- 覚えたプロファイルが無くなったとき（USB を抜いたなど）は、ほかのプロファイルへ切り替えない（別のアカウントを読むので）。

実測（偽の Portable 構成、`App\Firefox64\firefox.exe` に ping.exe を置いて動かす）: フォルダ・`Data`・exe の指定はどれも
`Data\profile` を読めた。自動では、Portable 版が動いていないときはいつものプロファイル、動いているときは Portable 版を選んだ。
本物の Firefox Portable と［music.youtube.com を開く］は確かめていない。

### 2026-10-13: Firefox Portable が見つからない・読めない（v8）

遠隔の利用者（Windows 11 23H2）から: (1) Portable 版で YouTube Music を開いたまま［Firefox から読み込む］→「ログインしたものがありません」、
(2) Portable 版のフォルダを指定しても「ログインしていないようです」。本人の PC に Portable 版を 4 つ置いてもらって調べた。

- (1) は本人の PC でも再現した。R: は ImDisk の RAM ディスク（RamDay）で、`QueryFullProcessImageNameW` は
  フラグ 0 でも `\Device\ImDisk2\…\App\Firefox64\firefox.exe` を返し、`…\App\` の形として見分けられなかった。
  v8 は起動オプション（`NtQueryInformationProcess` の ProcessCommandLineInformation）の `-profile <フォルダ>` を先に読み
  （Portable 版は `firefox.exe -profile …\Data\profile` で起動する）、`\Device\…` は `QueryDosDevice` でドライブ名に直す。
  直した後、自動で R: の Portable 版を候補の先頭にして読めた（22 個）。
- (2) は本人の PC では再現しない（C: と R: の Portable 版とも、フォルダ指定で 22 個読めた）。遠隔の環境はまだ分からない。
  思い当たる原因に備えた:
  - コンテナやサイトごとの分離（first-party isolation）で、YouTube の cookie の `originAttributes` が空でない →
    空の束にログインが無ければ、ほかの束（埋め込み用 `partitionKey` を除く）も見る。
  - 期限の単位を版（`user_version` ≥ 16 ならミリ秒）で決め打ちしていた → 値の大きさ（1e11 より大きければミリ秒）で見分ける。
  - 失敗したときは「YouTube の cookie の数、期限切れ、コンテナ等、埋め込み用、WAL のページ数」を出す。
    「閉じたときに cookie を削除する」設定の Firefox は、ログインの cookie をファイルに書かないので読めない（そのときは数が少なく出るはず）。
- 遠隔の利用者から、v8 で (1)(2) とも読めるようになったと報告（2026-10-13）。どの手当てが効いたかは確かめていない。
  本人の見立てはコンテナ（`originAttributes` が空でない束を読むようにしたこと）。推測。

### 2026-10-13: Firefox がログオフされた・ログイン情報のダイアログを組み直した（v10）

遠隔の利用者から「ytm-lite に使ったアカウントが、2 日続けて（ログオフしていないのに）ログオフされていた」と報告。
原因は確かめていない（アカウントを使って確かめるのは危険なのでしていない）。見立て:

- Google のログインの cookie のうち `__Secure-1PSIDTS` / `__Secure-3PSIDTS` は頻繁に入れ替わり、古い値は短い時間で拒まれる
  （[fuuka-cata-link #30](https://github.com/dankhael/fuuka-cata-link/pull/30): 書き出した cookie が 35 分で「入れ替わった」と言われた。
  [notebooklm-py](https://github.com/teng-lin/notebooklm-py/blob/main/docs/troubleshooting.md)）。
- 「Firefox から読み込む」は、ふだん使いの Firefox と同じセッションを ytm-lite が同時に使う。Firefox が入れ替えた後も、
  ytm-lite は起動時に読んだ古い値を送り続けるので、Google から見ると盗まれたセッションに見え、セッションごと取り消された、と見ている。
- yt-dlp の Wiki が「シークレット ウィンドウでログインし、cookie を書き出したらそのウィンドウを閉じる」よう勧めるのも、
  同じセッションを同時に使わないため。

v10 でしたこと（本人の指摘に沿って）:
- ログイン情報のダイアログを、手動で貼り付ける方法（シークレット ウィンドウで専用のログインを作り、最後に必ず閉じる）を
  前半に「おすすめ」として置き、1 行 1 手順にした。(2) の URL はクリックでコピー、(6) の欄と (7) の［保存］を手順の中に置いた。
  Firefox の表記は「要求ヘッダー（リクエストヘッダー）をコピー」（本人の Firefox では「要求ヘッダー」だった）。
- 「Firefox から自動で読み込む」は後半にし、「その Firefox で YouTube を使い続けるとログオフされることがある。専用の Firefox で」と注意書きを付けた。
- 送る直前に Firefox から読み直す手当ては、まだ入れていない。

### 2026-10-13: Windows のメディア コントロール（SMTC）に出す（v6）

本人の要望: windows\muutask に再生中として出し、そこから操作したい。
muutask は `GlobalSystemMediaTransportControlsSessionManager`（GSMTC）で読み、`SourceAppUserModelId` が空のセッションは飛ばす
（muutask の `media.cpp`）。ytm-lite は `ISystemMediaTransportControlsInterop::GetForWindow` で SMTC を得て、
曲名・アーティスト・アルバム、状態、位置（TimelineProperties）を出し、ButtonPressed と PlaybackPositionChangeRequested を受ける（`smtc.c`）。

- C から WinRT を使う。SDK の `winrt\windows.media.h` に C 向けの宣言があるが、IID の実体は無いので、
  C++ 側の `MIDL_INTERFACE` / `uuid` の値を写して `DEFINE_GUID` した。
- 知らせは別のスレッドから来るので、受け手は `IAgileObject` を名乗り、`PostMessage` するだけ。
- 位置は、変わったとき（開始・シーク・一時停止・再開）と 5 秒ごとに出す。間は Windows が進める。

実測（2026-10-13、muutask と同じ GSMTC で PowerShell から読んだ）:
`SourceAppUserModelId` は `ytm-lite.exe`（空でない。AUMID を明示しなくてよかった）。曲名・アーティスト・長さ・状態が出て、
外からの一時停止・再生・位置の変更（100 秒）・次の曲がすべて効いた。ytm-lite を終えるとセッションは消えた。
muutask の画面そのものは見ていない。

### 2026-10-13: ジャケット画像を SMTC に渡す（v7）

本人の質問: サムネイルを出すとメモリは増えるか、50MB を超えるか。測ってから入れた。

- 画像の URL はプレイリストの応答に入っている（`thumbnail.musicThumbnailRenderer.thumbnail.thumbnails` の最後）。
  正方形のジャケット（lh3 / yt3 の `=w60-h60…`）は `=w150-h150-l90-rj` にする（Chrome の YouTube Music が渡す大きさ）。
  動画の静止画（i.ytimg.com、400x225）はそのまま。曲が変わったときに 1 回だけ、cookie なしで画像のサーバーから取る（約 12KB）。
- 渡し方を 2 つ比べた（「マイベスト」1 曲目、音量 0、再生中）:

  | 方式 | Private | Working Set | DLL |
  | --- | --- | --- | --- |
  | 画像なし | 14.8 MB | 45.8 MB | 81 |
  | URL のまま（`CreateFromUri`） | 16.0 MB | 52.8 MB | 93 |
  | 自分で取ってバイト列（`SHCreateMemStream` → `CreateRandomAccessStreamOverStream` → `CreateFromStream`） | 15.2 MB | 46.9 MB | 81 |

  URL のままだと Windows の通信部品が読み込まれて重いので、バイト列にした。タスクマネージャーの「メモリ」と同じ
  プライベート ワーキングセットでは、画像なし 13.5MB・画像あり 13.2MB で差は揺れの範囲。
- 読めるかは、muutask の `ReadThumbnail` と同じ手順（`OpenReadAsync` → `Size` → `ReadAsync`）の C++/WinRT の小さな読み手で確かめた。
  11,959 バイト、先頭 `FF D8`（JPEG）。**PowerShell 5.1 からは読めない**（WinRT の型を扱えず `IInputStream` への変換で
  `E_NOINTERFACE`）ので、それを見て「開けない」と判断しないこと。
- 本人の環境で、muutask にジャケットが出ることを確かめてもらった（2026-10-13）。
- v9: 150 では muutask の拡大表示（標準 180 DIP、この PC は 125% で 225px）で引き伸ばされて粗いと本人の指摘。
  `yt3.googleusercontent.com/…=wN-hN-l90-rj` は指定どおりの大きさを返す（実測: 150 は 12KB、300 は 35KB、544 は 89KB、
  1200 は 258KB）。既定を 544 にし（muutask の「とても大きい」430 DIP = 538px まで引き伸ばしなし）、`thumbnail_size` で変えられる。
  544 で muutask と同じ読み手が 89,420 バイトを読めた。プライベート ワーキングセットは 14.1MB（150 のとき 13.2〜13.5MB）。

### 効果が無かったので戻したもの（2026-10-12）

- マニフェストの `heapType=SegmentHeap`: 待機 3,136KB と 3,168KB、プレイリストを読んだ後 4,240KB と 4,224KB。差なし。
- Media Foundation の起動を最初の再生まで遅らせる: 待機時の差は約 40KB。害も無いので残してある。

## 確かめていないこと

- **ログインしたアカウントでの動作**（ライブラリの一覧、非公開のプレイリスト、`SAPISIDHASH`）。
  cookie が無いので、貼り付けの解析を `-selftest` で確かめただけ。
- 既定の再生デバイスの切り替え、スリープからの復帰、長時間の連続再生。
- Windows 10 での動作。
