# 文書索引

context compaction で文脈が失われた場合は、この索引から必要な文書をたどる。

| 知りたいこと | 参照先 |
|---|---|
| プロジェクトの目的と使い方 | `README.md` |
| 要件と初回セットアップの指示 | `.prompts/INIT.md` |
| 利用者が触れる部分の仕様 (ドック、設定画面、ホットキー、エラーの表示) | `docs/design/user-interface.md` |
| 調査レポート | `docs/reports/` |
| 段階 4 の依頼者の確認とインストールの体験の結果、所見への対応 | `docs/reports/2026-11-10-stage4-user-review.md` |
| 英訳と概要のモデルの比較 (gpt-6-luna、gpt-5.6-luna、gpt-5.4-nano) | `docs/reports/2026-11-10-model-comparison.md` |
| モデルの比較の生成した全文と集計 (入力は架空の発話) | `docs/logs/2026-11-10-model-eval/` |
| 段階 3 の OBS での確認のログの抜粋 (本文なし) | `docs/logs/2026-11-10-stage3-verify-obs-log-excerpt.txt` |
| 段階 1 の前提の検証の結果 (読み込み、音声、文字起こし、英訳と概要、連続の接続) | `docs/reports/2026-11-10-stage1-verification.md` |
| 段階 1 の検証の OBS のログの抜粋 (本文なし) | `docs/logs/2026-11-10-stage1-verify-obs-log-excerpt.txt` |
| 着手前の追加調査: OpenAI API の仕様、obs-deps の libcurl、既存プラグインの配布物 | `docs/reports/2026-11-10-preflight-investigation.md` |
| 実行ログ (生の出力。git 追跡対象外) | `.logs/` |
| 実行ログ (残す価値があると判断したもの) | `docs/logs/` |
| 計画と決定事項 (Beads 使用時の正本) | `bd show <親課題>` |
| 計画 (Beads 非使用時) | `.prompts/PLANS/` |
| 判断を要する論点 (Beads 非使用時) | `.prompts/DISCUSSIONS/` |
| タスクの状態、依存関係、ブロッカー | `bd ready` / `bd blocked` / `bd list` |
