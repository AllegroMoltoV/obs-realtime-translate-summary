# リアルタイム字幕・翻訳・要約 / Realtime Translate & Summary

OBS Studio で、日本語の配信の音声からリアルタイムに 3 つの文字列を作り、OBS のテキストソースに表示するプラグインです。

- 認識の途中から更新する日本語字幕
- 確定した発話の英語字幕
- 直近 5 分の発言をまとめた、最大 3 行の日本語の概要

音声の認識、英訳、概要の作成には OpenAI の API を使います。

> このプラグインは無料です。ただし、OpenAI API の利用料金は、利用者ご自身の OpenAI アカウントに別途請求されます。利用には、ご自身で発行した OpenAI の API キーが必要です。料金の目安は「[費用](#費用)」を参照してください。

## 背景

このプラグインは、作者の Python 製ツール [realtime-summary-generator](https://github.com/AllegroMoltoV/realtime-summary-generator) を、OBS Studio のネイティブのプラグインとして作り直したものです。元のツールでは、Python の実行環境、`.env` の作成、OBS WebSocket の設定、決まった名前のテキストソースの作成が必要でした。このプラグインでは、プラグインのフォルダーを置き、OBS の画面の中で設定して開始できます。

## 主な機能

- OBS のドックで、音声を取るソースと、表示先のテキストソース (Text (GDI+) と Text (FreeType 2)) を選んで開始と停止をします。使わない出力は選ばずに無効にできます
- 表示先は本文だけを更新し、フォント、色、配置などの見た目は OBS の設定のまま残します
- 停止中は表示先に「日本語字幕」「英語字幕」「概要」の仮の文字列を出し、開始の前に位置と見た目を調整できます。設定でオフにできます
- 「聞き取ってほしい言葉」に人名や作品名を書くと、文字起こしで正しく書き取られやすくなります
- 音声のソースがミュートの間は、OpenAI へ音声を送りません
- 接続が切れたときは、自動で最大 5 回まで再接続します
- 指定した時間で自動停止できます。停止し忘れによる課金を防ぐための機能で、既定はオフです
- 開始/停止の切り替え、開始、停止のホットキーを OBS の設定で割り当てられます
- 日本語と English の表示に対応しています

## 必要なもの

- Windows x64 の PC と OBS Studio。動作を確認した版は OBS Studio 33.0.1 です
- OpenAI の API キー。[OpenAI の API キーのページ](https://platform.openai.com/api-keys) で発行してください。API の利用料金の支払いの設定も必要です

## 費用

OpenAI API の利用料金は、利用者の OpenAI アカウントに請求されます。プラグインの作者に料金は発生しません。

使うモデルと、2026-11-10 時点の公式の単価は次のとおりです。最新の単価は [OpenAI の料金のページ](https://developers.openai.com/api/docs/pricing) で確認してください。

| 用途 | モデル | 単価 |
| --- | --- | --- |
| 文字起こし | gpt-transcribe | 1 分あたり 0.0045 米ドル |
| 英訳 | gpt-5.6-luna | 100 万トークンあたり入力 0.20 米ドル、出力 1.20 米ドル |
| 概要 | gpt-6-luna | 100 万トークンあたり入力 0.10 米ドル、出力 0.50 米ドル |

1 時間の一人語りの配信あたりの目安は、約 0.06 米ドルから約 0.29 米ドルです。下限は作者の約 28 分の配信の実績を 1 時間に換算した値です。上限は 60 分すべてが文字起こしの課金の対象になった場合の値です。無音の時間の課金の有無は、公式の文書で確認できていません。

ドックの「今回の利用」には、開始から停止までに送信した音声の秒数と、英訳と概要のトークン数を表示します。

## 送信するデータと保存しないもの

- プラグインが通信する相手は `api.openai.com` だけです。開始を押すまで通信しません
- 文字起こしには、音声と「聞き取ってほしい言葉」を送ります
- 英訳と概要には、確定した発話の文字列を送ります。要求には、OpenAI 側に応答を保存させない指定の `store: false` を付けています
- OpenAI の公式の文書によると、API に送ったデータは、利用者がオプトインしない限り学習に使われません。不正利用の監視のため最長 30 日保持されます。最新の条件は [OpenAI の文書](https://developers.openai.com/api/docs/guides/your-data) を確認してください
- 音声、字幕、英訳、概要の履歴をファイルに保存しません。OBS のログとプラグインのログにも、本文と API キーを書きません
- API キーは、Windows の DPAPI で暗号化して `%APPDATA%\obs-studio\plugin_config\realtime-translate-summary\` に保存します。シーンコレクションやプロファイルには保存しません

## 対応環境と配布

| OS | 配布の方法 | 確認の範囲 |
| --- | --- | --- |
| Windows x64 | ビルド済みの ZIP | OBS 33.0.1 で作者がユーザーテストを実施 |
| macOS、Linux | 配布しない | GitHub Actions でビルドが通ることだけを確認。API キーの保存は Windows だけに対応しているため、OBS の中では使えません |

## インストール

1. [Releases](https://github.com/AllegroMoltoV/obs-realtime-translate-summary/releases) から、最新の Windows x64 の ZIP をダウンロードしてください。
2. OBS Studio を終了し、ZIP を展開してください。
3. エクスプローラーのアドレス欄に `%PROGRAMDATA%` と入力して開き、`obs-studio`、`plugins` の順にフォルダーを開いてください。フォルダーが無い場合は、同じ名前で作ってください。
4. ZIP の中の `realtime-translate-summary` フォルダーを、その `plugins` フォルダーへコピーしてください。Windows から管理者の許可を求められた場合は、許可して続けてください。

```text
C:\ProgramData\obs-studio\plugins\
└─ realtime-translate-summary\
   ├─ realtime-translate-summary.dll
   └─ data\
      ├─ licenses\
      └─ locale\
```

`ProgramData` は隠しフォルダーです。アドレス欄にパスを入力すると、隠しフォルダーの表示の設定を変えずに開けます。`realtime-translate-summary` フォルダーを二重に作らないでください。

## 使い方

1. OBS Studio を起動し、メニューの「ドック」から「リアルタイム字幕・翻訳・要約」を表示してください。
2. 表示先のテキストソースを用意してください。Text (GDI+) か Text (FreeType 2) のソースを、字幕を出したい位置に置きます。
3. ドックの「設定…」を開き、OpenAI の API キーを入力して「保存」を押してください。
4. ドックで、マイクなどの音声のソースと、日本語字幕、英語字幕、概要の表示先を選んでください。停止中は表示先に仮の文字列が出るため、位置と見た目を調整してください。
5. 「開始」を押すと、字幕と英訳と概要の表示が始まります。「停止」を押すと、OpenAI への送信が止まり、表示先は仮の文字列に戻ります。

設定画面では、次の項目も変えられます。変更は次の開始から反映します。

| 項目 | 既定 | 内容 |
| --- | --- | --- |
| 聞き取ってほしい言葉 | なし | 人名、作品名、専門用語など、文字起こしで正しく書き取ってほしい言葉を 1 行に 1 つ書きます |
| 概要の 1 行の最大文字数 | 30 | 超えた行は末尾を … で切り詰めます |
| 概要の更新の最短間隔 | 10 秒 | 前回の更新からこの時間が経ったあとに発話が確定すると、概要を更新します |
| 停止中は表示先に仮の文字列を表示する | オン | オフにすると、停止中の表示先は空になります |
| 指定した時間で自動停止する | オフ | オンにすると、開始から指定した分数で停止します |

## 制約

- 表示先のテキストソースの本文は、ソースの設定に入ります。プラグインは停止、OBS の終了、シーンコレクションの切り替えの前に表示を仮の文字列か空に戻しますが、動作中に OBS がシーンコレクションを自動保存すると、その時点の字幕が保存されます
- 発話の区切りには無音が必要です。無音を挟まない長い発話では、字幕の確定が遅れます
- 英訳の処理時間は、作者の計測で 1 回あたり中央値約 1.1 秒です。人名などの固有名詞は、英訳で正しく訳されない場合があります
- 対象の言語は、日本語の入力から英訳と日本語の概要を作る組み合わせに固定しています

## ソースからのビルド

依存ライブラリは [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) の仕組みで自動的に取得します。Windows では Visual Studio 2026 と CMake 3.28 以降が必要です。

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```

既定では、ビルドの直後に `C:\ProgramData\obs-studio\plugins` へ配置します。配置しない場合は、構成の時に `-DENABLE_POST_BUILD_INSTALL=OFF` を付けてください。単体テストは `-DRTS_BUILD_TESTS=ON` で構成し、`ctest --test-dir build_x64 -C RelWithDebInfo` で実行します。

### 依存ライブラリ

- [nlohmann/json](https://github.com/nlohmann/json) v3.12.0 (MIT License)。ライセンス文は `data/licenses/` にあります
- libcurl。OBS Studio に同梱されたものを使います

## AI利用開示 / AI Disclosure

### 日本語

このプラグインの制作では、Anthropic Claude Code を、要件の調査、ソースコード、ビルドの設定、テスト、文書を含むほぼすべての工程で使用しました。作者本人は Windows 上の OBS Studio でユーザーテストを実施しました。コードの読解による厳密な人間のレビューは実施していません。

### English

Anthropic Claude Code was used for almost the entire development process, including requirements research, source code, build configuration, tests, and documentation. The author performed user testing in OBS Studio on Windows but did not perform a rigorous human review of the source code.

## 更新情報

### 次の版 (準備中)

- 最初の公開

---

## License

GPL-2.0 (same as OBS Studio)

## Author

AllegroMoltoV - [https://www.allegromoltov.jp](https://www.allegromoltov.jp)
