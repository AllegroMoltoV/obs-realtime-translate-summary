# 段階 1 の前提の検証の結果

## はじめに

段階 1 では、Windows の OBS 33.0.1 の中で、音声の取り出し、libcurl の WebSocket による文字起こし、libcurl の HTTPS による gpt-6-luna の英訳と概要が成り立つことを確かめました。計画は Beads の課題 `obs-realtime-translate-summary-rbg.1` の design にあります。検証は 2026-11-10 に行いました。

## 検証環境

- Windows 11、OBS Studio 33.0.1 (64-bit)
- ビルド: Visual Studio Build Tools 2026、CMake 4.2.1、プリセット `windows-x64`、構成 RelWithDebInfo
- 音声のソース: `vocal@totalmix` (`wasapi_input_capture`)。OBS の標本化周波数は 44,100 Hz
- 検証用のコード: コミット `41abb74` の `src/verify-session.cpp` ほか。起動は環境変数で行い、ログには本文とキーを書かない

## 検証方法

OBS の起動は、起動用のスクリプト経由で行いました。このスクリプトは `.tmp/.env` のキーと検証の設定を環境変数に入れて OBS を起動します。検証 C から E の OBS の開閉と発声は依頼者が行いました。判定には、OBS のログのうちプラグインが書いた行を使いました。抜粋は `docs/logs/2026-11-10-stage1-verify-obs-log-excerpt.txt` にあります。

| 検証 | 内容 | 合格の条件 |
| --- | --- | --- |
| A | プラグインの読み込み | `plugin loaded successfully` の行がある |
| B | 変換した音声の長さと経過時間の比 | 60 秒以上で 0.99 から 1.01 |
| C | 文字起こし | `session.updated` を受け取り、差分と確定のイベントが届く。エラーのイベントが無い |
| D | 英訳と概要 | HTTP 200 で、概要の `display_lines` が 3 要素 |
| E | 10 分以上の連続の接続 | 遅れたフレームの増分が総フレームの 0.1% 以下。新たな音声のバッファリングの行が無い |

## 検証結果

### A: 読み込み

OBS のログに次の行が出ました。実行時に読み込まれた libcurl は OBS 同梱のもので、TLS は Schannel です。

```
[realtime-translate-summary] libcurl runtime version=8.12.1-DEV ssl=Schannel https=1 wss=1
[realtime-translate-summary] plugin loaded successfully (version 0.1.0)
```

### B: 音声

音声だけを確かめる実行では、75 秒の間、比は 1.0000 から 1.0014 でした。欠落した音声は 0 フレームです。文字起こしと並行した実行では、2,675.7 秒の間、比は 0.9999 から 1.0006 でした。0.9999 は停止の直前の最後の 1 回の計測です。欠落した音声は 0 フレームです。

### C: 文字起こし

接続は HTTP 101 で成功し、`session.created` と `session.updated` を受け取りました。`gpt-transcribe` と `server_vad` の組み合わせで、エラーのイベントは届きませんでした。最初の 80 秒で、差分のイベントが 49 件、確定のイベントが 8 件届きました。確定のうち 1 件は 0 文字でした。

### D: 英訳と概要

`gpt-6-luna` に `reasoning.effort` を `none`、`temperature` を元のツールと同じ値にした要求は、英訳と概要の 176 件のすべてで HTTP 200 が返りました。`reasoning_tokens` はすべて 0、`cached_tokens` もすべて 0 でした。概要の `display_lines` は 88 件すべてで 3 要素でした。

| 処理 | 件数 | 中央値 | p95 | 最大 | 元のツールの実績の中央値 | 元のツールの実績の p95 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 英訳 | 88 | 1,218.5 ms | 1,901.2 ms | 2,773.3 ms | 844.1 ms | 1,268.4 ms |
| 概要 | 88 | 1,412.5 ms | 2,093.1 ms | 2,729.1 ms | 1,176.2 ms | 1,817.3 ms |

確定のイベントは 144 件でしたが、英訳と概要を終えたのは 88 件でした。検証用のコードは、確定した発話ごとに英訳と概要を順に待ちます。

入力のトークン数は、英訳が 1 回あたり 54 から 66、概要が 720 から 773 でした。

### E: 連続の接続

14:12:12 から 14:56:48 まで、2,675.7 秒の間、切断とエラーなしで接続が続きました。遅れたフレームは検証の開始から 10 秒で 304 になりました。その後、14:41:53 から 14:42:03 の間に 306、14:55:23 から 14:55:33 の間に 307 になりました。検証の開始から 10 秒以降の増分は 3 フレームで、同じ期間の総フレーム約 80,000 の 0.004% です。音声のバッファリングの行は、検証の開始前の OBS の起動時の 1 件だけでした。終了時のログには `Number of memory leaks: 0` が出ました。

### macOS と Linux の CI

非公開リポジトリ `AllegroMoltoV/obs-realtime-translate-summary` の GitHub Actions の実行 42765532837 で、clang-format と gersemi の整形の確認、Ubuntu 26.04、macOS (macos-26)、Windows のビルドがすべて成功しました。

### 検証の過程で判明した事実

この PC の `obs64.exe` には、利用者の互換性の設定で「管理者として実行」が付いています。OBS のログには `Running as administrator: true` が出ました。この状態では、`taskkill` が Access is denied を返し、起動用のスクリプトの環境変数も OBS に届きませんでした。起動用のスクリプトで `__COMPAT_LAYER=RunAsInvoker` を設定すると、`Running as administrator: false` で起動し、環境変数が届きました。

起動用の `.cmd` が呼ぶ Windows PowerShell 5.1 は、BOM の無い UTF-8 のスクリプトの日本語で構文エラーになりました。スクリプトを ASCII だけにすると解消しました。

## 考察

INIT.md で未確認だった、OBS のプロセスの中での libcurl の WSS と HTTPS は、OBS 同梱の libcurl で使えると考えています。これにより、epic の design の libcurl の入手の仮決めは確定の条件を満たしたと判断します。

`gpt-transcribe` と `server_vad` の組み合わせは、公式の文書に記述がないものの、2026-11-10 の時点で受け付けられていると考えています。

`gpt-6-luna` で `reasoning.effort` を `none` にしたときの `temperature` の受け付けと、推論のトークンが 0 になることは、今回の要求の範囲では成り立つと考えています。

英訳と概要の処理時間は、中央値と p95 のどちらも元のツールの Gemini の実績より長くなりました。ただし、今回の検証用のコードは英訳と概要を直列に待つため、本番の構成での処理時間とは条件が異なると考えています。段階 3 で、英訳を並行させ、概要を最短間隔の規則で呼ぶ構成にしてから、INIT.md 11.2 節の比較をするのが妥当だと考えています。

概要の入力は 1 回あたり約 720 トークンで、OpenAI の文書が示すキャッシュの最小の長さ 1,024 トークンに届かないため、キャッシュの割引は適用されないと推測します。
