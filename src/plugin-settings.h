#pragma once

#include <obs.h>

#include <array>
#include <mutex>
#include <string>

namespace rts {

// シーンコレクションごとに保存する選択
struct SourceSelection {
	std::string audio_source;
	std::array<std::string, 3> targets; // 日本語字幕、英語字幕、概要。空は「使わない」
};

// プラグインの設定ファイル (plugin_config/realtime-translate-summary/settings.json) に保存する設定
struct GlobalSettings {
	std::string keywords_text; // 1 行 1 語句
	int summary_max_chars = 30;
	double summary_interval_seconds = 10.0;
};

// 設定の保持。シーンコレクションの読み込みはドックの作成より前に呼ばれるため、ドックとは別に持つ
class PluginSettings {
public:
	static PluginSettings &instance();

	SourceSelection selection() const;
	void set_selection(const SourceSelection &selection);

	GlobalSettings global() const;
	void set_global(const GlobalSettings &settings);
	void load_global();

	// シーンコレクションの保存と読み込み。save_data のこのプラグインのキーの下に入れる
	void save_selection_to(obs_data_t *save_data) const;
	void load_selection_from(obs_data_t *save_data);

	static constexpr const char *kSaveKey = "realtime-translate-summary";

private:
	PluginSettings() = default;
	void save_global_locked() const;

	mutable std::mutex mutex_;
	SourceSelection selection_;
	GlobalSettings global_;
};

} // namespace rts
