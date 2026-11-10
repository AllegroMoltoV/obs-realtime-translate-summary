# 着手前の追加調査: OpenAI API と obs-deps の libcurl

## はじめに

`.prompts/INIT.md` の「確認できなかったこと」のうち、ビルドや実行をせずに文書と配布物で確かめられる項目を 2026-11-10 に調べました。調査は 2 つのサブエージェントに分け、どちらも読み取りだけで行いました。OpenAI の文書の引用は WebFetch の要約モデルを通して取得したため、一字一句の一致は保証できません。実装で使う前に原文と照合してください。

## 検証結果: OpenAI API

### 文字起こしのモデルと発話区切り

https://developers.openai.com/api/docs/guides/realtime-transcription.md では、Realtime の文字起こしの推奨モデルが `gpt-live-transcribe` です。このモデルは「doesn't support `server_vad` or `semantic_vad`」と書かれています。`gpt-transcribe` については「Use `gpt-transcribe` in a Realtime session only when you specifically need transcription to begin after a committed audio turn or need detected-language output」と書かれており、ページの例は `turn_detection: null` です。`gpt-transcribe` と `server_vad` の組み合わせの記述は、このページと realtime-vad.md では見つかりませんでした。

https://developers.openai.com/api/docs/models/gpt-transcribe.md では、`v1/realtime/transcription_sessions` が Supported、`v1/realtime` が Not supported です。

一方、元のツールの `src/realtime_summary/transcription.py` は、28 行目で `wss://api.openai.com/v1/realtime?intent=transcription` に接続しています。97 行目で `gpt-transcribe` を、110 行目で `server_vad` を指定しています。この設定は 2026-09-14 の実利用で動作しました。

### セッションの最大時間

https://developers.openai.com/api/docs/guides/realtime-conversations.md に「The maximum duration of a Realtime session is 60 minutes.」とあります。文字起こしのセッションに固有の記述と、上限に達したときのイベント名や終了コードの記述は見つかりませんでした。server-events の参照のページは読んでいません。

### Responses API の要求と応答

出典は https://developers.openai.com/api/reference/resources/responses/methods/create.md です。

| 種別 | フィールド | 内容 |
| --- | --- | --- |
| 要求 | `instructions` | system または developer のメッセージ |
| 要求 | `max_output_tokens` | 推論のトークンを含む出力の上限 |
| 要求 | `reasoning.effort` | `none` などを受け付ける |
| 要求 | `temperature` | 0 から 2 |
| 要求 | `text.format` | `type: json_schema` に `name`、`schema`、`strict` を指定する。`strict` の既定は false |
| 要求 | `store` | 省略時は true |
| 応答 | `usage.input_tokens`、`usage.input_tokens_details.cached_tokens` | 入力とキャッシュ済みの入力のトークン数 |
| 応答 | `usage.output_tokens`、`usage.output_tokens_details.reasoning_tokens` | 出力と推論のトークン数 |
| 応答 | `output[]` の message の `content[]` | `type: "output_text"` の `text` を連結して本文を得る。`output_text` は SDK だけの便利なプロパティで、HTTP の応答には含まれない |

`content[]` の `text` フィールドは、例の中での出現だけを確認しました。

### gpt-6-luna の temperature

https://developers.openai.com/api/docs/guides/latest-model.md には「When reasoning effort is not `none`, remove `temperature`, `top_p`, and `top_logprobs`.」とあり、GPT-6 Luna は `none` に対応すると書かれています。`none` のときに `temperature` を受け付けると明記した文と、推論のトークンが 0 になると書いた文は見つかりませんでした。

### データの保持

https://developers.openai.com/api/docs/guides/your-data.md によると、API に送ったデータはオプトインしない限り学習に使われません。不正利用の監視のログは最長 30 日保持されます。`/v1/responses` と `/v1/realtime` のどちらも、学習には使われず、不正利用の監視の保持は 30 日です。

Responses API には「30 day Application State retention period by default, or when the `store` parameter is set to `true`」とあります。`store` の既定は true のため、指定しないと応答が 30 日間保存されます。

### 課金とキャッシュ

`gpt-transcribe` の単価は 1 分あたり 0.0045 米ドルです。送った音声のすべてが課金されるのか、確定した区間だけが課金されるのかを示す記述は見つかりませんでした。確認したのは pricing.md、models/gpt-transcribe.md、realtime-transcription.md、realtime-vad.md です。guides/realtime-costs.md は 404 でした。

https://developers.openai.com/api/docs/guides/prompt-caching.md には「The minimum cacheable prompt length is 1,024 tokens for GPT-5.6 and later.」とあります。gpt-6-luna がこの規則に該当すると明記した文は確認していません。

## 検証結果: obs-deps と libcurl

### obs-deps 2026-10-23

`gh release view 2026-10-23 --repo obsproject/obs-deps` で `windows-deps-2026-10-23-x64.zip` を確認し、スクラッチパッドに展開して中身を確かめました。

- `include/curl/curlver.h` の `LIBCURL_VERSION` は `8.12.1-DEV` です。
- `lib/cmake/CURL/CURLConfig.cmake` の `CURL_SUPPORTED_PROTOCOLS` に `WS` と `WSS` が含まれます。`CURL_SUPPORTED_FEATURES` に `SSL` と `SSPI` が含まれ、OpenSSL への依存はありません。
- `CURLTargets-release.cmake` では、`CURL::libcurl_shared` の import ライブラリが `lib/libcurl_imp.lib`、DLL が `bin/libcurl.dll` です。`CURL::libcurl` はその別名です。

D:/work 配下の `.deps` には、obs-deps 2026-10-23 はありませんでした。

### OBS 同梱の libcurl.dll

`C:\Program Files\obs-studio\bin\64bit\libcurl.dll` の版は 8.12.1-DEV です。obs-deps 2026-10-23 の DLL とはサイズが異なります。前者は 618,768 バイト、後者は 610,304 バイトです。`dumpbin /exports` で `curl_easy_init`、`curl_ws_recv`、`curl_ws_send`、`curl_ws_meta` のエクスポートを確認しました。`dumpbin /dependents` では `CRYPT32`、`bcrypt`、`WS2_32` などに依存し、OpenSSL と zlib の DLL には依存していません。`bin\64bit` と `obs-plugins\64bit` のうち `libcurl.dll` を import しているのは `obs64.exe` だけでした。

### obs-plugintemplate

上流の `CMakeLists.txt` は CURL を扱っていません。`D:/work/obs-simple-drop-shadow/cmake/common/buildspec_common.cmake` の 210 行目と 224 行目で、obs-deps の展開先が `CMAKE_PREFIX_PATH` に入ります。上流の最新コミットは 2025-12-09 です。obs-loudness-balance-monitor の main は、上流から buildspec の版と CI のランナーの版を上げ、`obs33-baseline.yaml` を足しています。

### 既存のプラグインの配布物

| プラグイン | 最新版 | 添付物 | ZIP の構成 |
| --- | --- | --- | --- |
| obs-loudness-balance-monitor | v1.0.3 | `loudness-balance-monitor-1.0.3-windows-x64.zip` | `<name>/<name>.dll`、`.pdb`、`data/locale/` |
| obs-simple-pitchshift | v1.1.1 | `simple-pitchshift-1.1.1-windows-x64.zip` と `simple-pitchshift-1.1.1-source-with-dependencies.tar.gz` | 上に加えて `data/licenses/` |

### GitHub

`gh auth status` では AllegroMoltoV でログイン済みです。スコープは `repo`、`read:org`、`gist`、`admin:public_key` で、`workflow` を含みません。`gh repo view AllegroMoltoV/obs-realtime-translate-summary` は「Could not resolve to a Repository」を返しました。

## 考察

文字起こしの設定は、文書と実績が食い違っています。元のツールの実績から、`intent=transcription` の接続では `gpt-transcribe` と `server_vad` の組み合わせが 2026-09 時点で受け付けられたと考えています。文書の記載が変わった可能性もあるため、段階 1 の手順 3 で最初に確かめるのが妥当だと考えています。

libcurl は、obs-deps の `CURL::libcurl` にリンクし、実行時には OBS 同梱の `libcurl.dll` を使う形で成り立つと推測します。`obs64.exe` が同名の DLL を先に読み込むためです。この形では DLL を同梱しないため、ZIP の構成は obs-loudness-balance-monitor と同じになると考えています。OBS の更新で同梱の curl が変わる危険は残ります。OBS が公式に保証する依存ではないためです。

Realtime セッションの上限が 60 分であれば、1 時間を超える配信では上限の前に計画的に接続を更新する必要があると考えています。

`store` を省略すると発話の本文が 30 日間保存されるため、`store: false` を指定するのが要件 5.5 の趣旨に合うと考えています。

`workflow` スコープが無いと、HTTPS 経由で `.github/workflows` を含む push が拒否される可能性があると推測します。git の通信が ssh であれば影響しない見込みです。
