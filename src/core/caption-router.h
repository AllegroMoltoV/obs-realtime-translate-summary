#pragma once

#include "turn-tracker.h"

#include <functional>
#include <map>
#include <optional>
#include <string>

namespace rts {

struct TranscriptionEvent {
	enum class Type { Committed, Delta, Completed, Failed, Other };
	Type type = Type::Other;
	std::string type_name;
	std::string item_id;
	std::optional<std::string> previous_item_id;
	std::string delta;
	std::string transcript;
	// error のイベントの種類。本文は持たない
	std::string error_type;
	std::string error_code;
	std::string error_param;
};

// Realtime API のサーバーのイベントを解析する。JSON として不正なら nullopt を返し、例外を投げない
std::optional<TranscriptionEvent> parse_transcription_event(const std::string &message);

// 認識途中の字幕と確定した発話を、発話の順序を守って届ける。元のツールの TranscriptionEventRouter を移した
class CaptionRouter {
public:
	using CaptionFn = std::function<void(const std::string &)>;
	using CompletedFn = std::function<void(const CompletedTurn &)>;

	CaptionRouter(CaptionFn on_caption, CompletedFn on_completed, ClockFn clock = steady_clock_seconds());

	TurnError handle(const TranscriptionEvent &event);

	struct ResetCounts {
		int pending_items = 0;
		int partial_captions = 0;
	};
	ResetCounts reset_session();

private:
	void deliver(const std::vector<CompletedTurn> &turns);

	CaptionFn on_caption_;
	CompletedFn on_completed_;
	ClockFn clock_;
	TurnTracker turns_;
	std::map<std::string, std::string> partials_;
	int latest_displayed_sequence_ = -1;
};

} // namespace rts
