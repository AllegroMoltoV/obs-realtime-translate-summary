#pragma once

#include <array>
#include <string>
#include <vector>

namespace rts {

// 開始の前に確かめる条件。docs/design/user-interface.md のドックの節にしたがう
enum class StartProblem {
	ApiKeyMissing,
	AudioSourceNotSelected,
	AudioSourceMissing,
	NoTargetSelected,
	TargetMissing,
	TargetNotText,
	DuplicateTarget,
};

struct TargetState {
	std::string name; // 空は「使わない」
	bool exists = false;
	bool is_text = false;
};

struct StartInputs {
	bool api_key_saved = false;
	std::string audio_source;
	bool audio_source_exists = false;
	std::array<TargetState, 3> targets; // 日本語字幕、英語字幕、概要
};

std::vector<StartProblem> check_start(const StartInputs &inputs);

// テキストソースとして扱う種類の ID (版を除いたもの)
bool is_supported_text_source_id(const std::string &unversioned_id);

constexpr int kDefaultSummaryMaxChars = 30;
constexpr double kDefaultSummaryIntervalSeconds = 10.0;
constexpr int kMaxSummaryMaxChars = 200;

bool valid_summary_max_chars(int value);
bool valid_summary_interval(double seconds);

constexpr int kMinAutoStopMinutes = 1;
constexpr int kMaxAutoStopMinutes = 24 * 60;
bool valid_auto_stop_minutes(int minutes);

// 自動停止が有効で、経過時間が指定の分数に達したら true
bool auto_stop_due(bool enabled, int minutes, double elapsed_seconds);

} // namespace rts
