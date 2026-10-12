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

- player は間隔 2 秒以上、10 分間に 20 回まで（匿名の上限の 1/8 以下）。並列にしない。
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

### 効果が無かったので戻したもの（2026-10-12）

- マニフェストの `heapType=SegmentHeap`: 待機 3,136KB と 3,168KB、プレイリストを読んだ後 4,240KB と 4,224KB。差なし。
- Media Foundation の起動を最初の再生まで遅らせる: 待機時の差は約 40KB。害も無いので残してある。

## 確かめていないこと

- **ログインしたアカウントでの動作**（ライブラリの一覧、非公開のプレイリスト、`SAPISIDHASH`）。
  cookie が無いので、貼り付けの解析を `-selftest` で確かめただけ。
- 既定の再生デバイスの切り替え、スリープからの復帰、長時間の連続再生。
- Windows 10 での動作。
